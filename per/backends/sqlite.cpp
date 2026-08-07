// SPDX-License-Identifier: Apache-2.0

#include "ovf/per/backend_abi.h"

#include <json/json.h>
#include <sqlite3.h>

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct StoreData final {
  std::string path;
  std::string journal_mode;
  std::uint64_t capacity{};
  std::uint32_t max_entries{};
  std::uint32_t max_key_size{};
  std::uint32_t max_value_size{};
  std::uint64_t max_blob_size{};
  std::string schema_id;
  std::uint64_t schema_version{};
  ovf_per_durability_v1 minimum_durability{};
  ovf_per_recovery_state_v1 open_recovery_state{OVF_PER_RECOVERY_CLEAN_V1};
};

struct StoreHandle final {
  std::shared_ptr<StoreData> store;
  ovf_per_access_v1 access{};
};

struct Transaction final {
  std::shared_ptr<StoreData> store;
  sqlite3* database{};
  std::uint64_t base_generation{};
  ovf_per_durability_v1 durability{};
  bool write{};
  bool finished{};
  bool commit_prepared{};
};

struct BlobOperation final {
  std::shared_ptr<StoreData> store;
  sqlite3* database{};
  sqlite3_blob* blob{};
  std::uint64_t base_generation{};
  std::uint64_t size{};
  std::uint64_t written{};
  ovf_per_durability_v1 durability{};
  bool write{};
  bool finished{};
  bool commit_prepared{};
};

struct Cursor final {
  ovf_per_handle_v1 transaction{};
  sqlite3_stmt* statement{};
  bool row_ready{};
};

struct MigrationOperation final {
  std::shared_ptr<StoreData> store;
  std::string migration_id;
  std::uint64_t source_generation{};
  std::string target_schema_id;
  std::uint64_t target_schema_version{};
  ovf_per_durability_v1 durability{};
};

struct SqliteBackend final {
  ovf_per_backend_v1 abi{};
  std::mutex mutex;
  std::string root;
  std::string journal_mode{"persist"};
  std::uint32_t busy_timeout_ms{1000};
  std::uint32_t max_stores{};
  std::uint32_t max_transactions{};
  std::unordered_map<std::string, std::weak_ptr<StoreData>> stores;
  std::unordered_map<ovf_per_handle_v1, StoreHandle> store_handles;
  std::unordered_map<ovf_per_handle_v1, Transaction> transactions;
  std::unordered_map<ovf_per_handle_v1, BlobOperation> blobs;
  std::unordered_map<ovf_per_handle_v1, Cursor> cursors;
  std::unordered_map<ovf_per_handle_v1, MigrationOperation> migrations;
  ovf_per_handle_v1 next_handle{1};
  bool running{};
  std::string error;
};

SqliteBackend* Self(ovf_per_backend_v1* backend) noexcept {
  return static_cast<SqliteBackend*>(backend->implementation);
}

void SetError(SqliteBackend& backend, std::string value) { backend.error = std::move(value); }

ovf_per_status_v1 SqliteStatus(int status) noexcept {
  switch (status) {
  case SQLITE_OK:
  case SQLITE_DONE:
  case SQLITE_ROW:
    return OVF_PER_STATUS_OK;
  case SQLITE_BUSY:
  case SQLITE_LOCKED:
    return OVF_PER_STATUS_BUSY;
  case SQLITE_FULL:
  case SQLITE_TOOBIG:
    return OVF_PER_STATUS_QUOTA_EXCEEDED;
  case SQLITE_CORRUPT:
  case SQLITE_NOTADB:
    return OVF_PER_STATUS_CORRUPTED;
  case SQLITE_READONLY:
  case SQLITE_PERM:
    return OVF_PER_STATUS_PERMISSION_DENIED;
  case SQLITE_NOMEM:
    return OVF_PER_STATUS_RESOURCE_EXHAUSTED;
  case SQLITE_IOERR:
  case SQLITE_CANTOPEN:
    return OVF_PER_STATUS_IO_ERROR;
  default:
    return OVF_PER_STATUS_BACKEND_ERROR;
  }
}

ovf_per_status_v1 Fail(SqliteBackend& backend, sqlite3* database, int status,
                       std::string_view context) {
  std::string message(context);
  if (database != nullptr) {
    message += ": ";
    message += sqlite3_errmsg(database);
  }
  SetError(backend, std::move(message));
  return SqliteStatus(status);
}

int Execute(sqlite3* database, const char* sql) noexcept {
  return sqlite3_exec(database, sql, nullptr, nullptr, nullptr);
}

std::string Hex(std::string_view value) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const unsigned char character : value) {
    output << std::setw(2) << static_cast<unsigned>(character);
  }
  return output.str();
}

bool ParseConfiguration(ovf_per_string_view_v1 configuration, SqliteBackend& backend) {
  if (configuration.data == nullptr || configuration.size == 0) {
    SetError(backend, "SQLite provider configuration is empty");
    return false;
  }
  Json::CharReaderBuilder builder;
  builder["allowComments"] = false;
  builder["collectComments"] = false;
  builder["failIfExtra"] = true;
  builder["rejectDupKeys"] = true;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value document;
  std::string errors;
  if (!reader->parse(configuration.data, configuration.data + configuration.size, &document,
                     &errors) ||
      !document.isObject() || !document["root"].isString()) {
    SetError(backend, "invalid SQLite provider configuration: " + errors);
    return false;
  }
  backend.root = document["root"].asString();
  if (const char* override_root = std::getenv("OVF_PER_STORAGE_ROOT"); override_root != nullptr) {
    backend.root = override_root;
  }
  if (backend.root.empty() || backend.root.front() != '/') {
    SetError(backend, "SQLite provider root must be an absolute path");
    return false;
  }
  if (document.isMember("journal_mode")) {
    if (!document["journal_mode"].isString()) {
      SetError(backend, "journal_mode must be a string");
      return false;
    }
    backend.journal_mode = document["journal_mode"].asString();
    if (backend.journal_mode != "persist" && backend.journal_mode != "wal") {
      SetError(backend, "journal_mode must be persist or wal");
      return false;
    }
  }
  if (document.isMember("busy_timeout_ms")) {
    if (!document["busy_timeout_ms"].isUInt() || document["busy_timeout_ms"].asUInt() > 60000U) {
      SetError(backend, "busy_timeout_ms must be between 0 and 60000");
      return false;
    }
    backend.busy_timeout_ms = document["busy_timeout_ms"].asUInt();
  }
  for (const auto& member : document.getMemberNames()) {
    if (member != "root" && member != "journal_mode" && member != "busy_timeout_ms") {
      SetError(backend, "unknown SQLite provider configuration member: " + member);
      return false;
    }
  }
  return true;
}

bool EnsureDirectory(const std::string& path) {
  std::string current;
  std::size_t offset = 1;
  while (offset <= path.size()) {
    const auto separator = path.find('/', offset);
    const auto end = separator == std::string::npos ? path.size() : separator;
    if (end > offset) {
      current.append("/").append(path, offset, end - offset);
      struct stat information{};
      if (::lstat(current.c_str(), &information) == 0) {
        if (!S_ISDIR(information.st_mode)) {
          return false;
        }
      } else if (errno != ENOENT || ::mkdir(current.c_str(), S_IRWXU) != 0) {
        return false;
      }
    }
    if (separator == std::string::npos) {
      break;
    }
    offset = separator + 1;
  }
  return true;
}

bool HasHotJournal(const std::string& database_path) noexcept {
  const std::string journal = database_path + "-journal";
  const int descriptor = ::open(journal.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    return false;
  }
  unsigned char first{};
  const auto count = ::read(descriptor, &first, 1);
  ::close(descriptor);
  return count == 1 && first != 0;
}

int ConfigureConnection(sqlite3* database, const StoreData& store, std::uint32_t busy_timeout_ms) {
  sqlite3_extended_result_codes(database, 1);
  sqlite3_busy_timeout(database, static_cast<int>(busy_timeout_ms));
  sqlite3_db_config(database, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
  sqlite3_db_config(database, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
  if (Execute(database, "PRAGMA mmap_size=0; PRAGMA temp_store=MEMORY;") != SQLITE_OK) {
    return sqlite3_errcode(database);
  }
  const std::string journal = "PRAGMA journal_mode=" + store.journal_mode + ";";
  return Execute(database, journal.c_str());
}

int OpenDatabase(const StoreData& store, std::uint32_t busy_timeout_ms, sqlite3** output) {
  const int status = sqlite3_open_v2(store.path.c_str(), output,
                                     SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                         SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW,
                                     nullptr);
  if (status != SQLITE_OK) {
    return status;
  }
  return ConfigureConnection(*output, store, busy_timeout_ms);
}

bool ReadMetadata(sqlite3* database, StoreData& store, std::uint64_t* generation) {
  constexpr const char* sql =
      "SELECT generation,capacity,max_entries,max_key_size,max_value_size,max_blob_size,"
      "minimum_durability,schema_id,schema_version "
      "FROM ovf_meta WHERE id=1";
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
    return false;
  }
  const int status = sqlite3_step(statement);
  if (status == SQLITE_ROW) {
    if (generation != nullptr) {
      *generation = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 0));
    }
    const bool matches =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement, 1)) == store.capacity &&
        static_cast<std::uint32_t>(sqlite3_column_int(statement, 2)) == store.max_entries &&
        static_cast<std::uint32_t>(sqlite3_column_int(statement, 3)) == store.max_key_size &&
        static_cast<std::uint32_t>(sqlite3_column_int(statement, 4)) == store.max_value_size &&
        static_cast<std::uint64_t>(sqlite3_column_int64(statement, 5)) == store.max_blob_size &&
        sqlite3_column_int(statement, 6) == static_cast<int>(store.minimum_durability) &&
        std::string_view(reinterpret_cast<const char*>(sqlite3_column_text(statement, 7)),
                         static_cast<std::size_t>(sqlite3_column_bytes(statement, 7))) ==
            store.schema_id &&
        static_cast<std::uint64_t>(sqlite3_column_int64(statement, 8)) == store.schema_version;
    sqlite3_finalize(statement);
    return matches;
  }
  sqlite3_finalize(statement);
  return false;
}

int InitializeDatabase(sqlite3* database, const StoreData& store) {
  constexpr const char* schema =
      "BEGIN EXCLUSIVE;"
      "CREATE TABLE IF NOT EXISTS ovf_meta("
      "id INTEGER PRIMARY KEY CHECK(id=1),generation INTEGER NOT NULL,capacity INTEGER NOT NULL,"
      "max_entries INTEGER NOT NULL,max_key_size INTEGER NOT NULL,max_value_size INTEGER NOT NULL,"
      "max_blob_size INTEGER NOT NULL,minimum_durability INTEGER NOT NULL,"
      "schema_id TEXT NOT NULL,schema_version INTEGER NOT NULL,"
      "successful_commits INTEGER NOT NULL,rejected_operations INTEGER NOT NULL,"
      "recovery_count INTEGER NOT NULL,recovery_state INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS ovf_values(key BLOB PRIMARY KEY,value BLOB NOT NULL) WITHOUT "
      "ROWID;"
      "CREATE TABLE IF NOT EXISTS ovf_blobs(key BLOB UNIQUE NOT NULL,value BLOB NOT NULL);"
      "CREATE TABLE IF NOT EXISTS ovf_migration("
      "id TEXT PRIMARY KEY,source_generation INTEGER NOT NULL,source_schema_id TEXT NOT NULL,"
      "source_schema_version INTEGER NOT NULL,target_schema_id TEXT NOT NULL,"
      "target_schema_version INTEGER NOT NULL,last_key BLOB NOT NULL,processed INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS ovf_migration_values("
      "key BLOB PRIMARY KEY,value BLOB NOT NULL) WITHOUT ROWID;"
      "CREATE TABLE IF NOT EXISTS ovf_rollback_meta("
      "schema_id TEXT NOT NULL,schema_version INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS ovf_rollback_values("
      "key BLOB PRIMARY KEY,value BLOB NOT NULL) WITHOUT ROWID;"
      "CREATE TABLE IF NOT EXISTS ovf_rollback_blobs("
      "key BLOB UNIQUE NOT NULL,value BLOB NOT NULL);"
      "COMMIT;";
  int status = Execute(database, schema);
  if (status != SQLITE_OK) {
    return status;
  }
  constexpr const char* insert =
      "INSERT OR IGNORE INTO ovf_meta VALUES(1,0,?1,?2,?3,?4,?5,?6,?7,?8,0,0,0,?9)";
  sqlite3_stmt* statement{};
  status = sqlite3_prepare_v2(database, insert, -1, &statement, nullptr);
  if (status != SQLITE_OK) {
    return status;
  }
  sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(store.capacity));
  sqlite3_bind_int(statement, 2, static_cast<int>(store.max_entries));
  sqlite3_bind_int(statement, 3, static_cast<int>(store.max_key_size));
  sqlite3_bind_int(statement, 4, static_cast<int>(store.max_value_size));
  sqlite3_bind_int64(statement, 5, static_cast<sqlite3_int64>(store.max_blob_size));
  sqlite3_bind_int(statement, 6, static_cast<int>(store.minimum_durability));
  sqlite3_bind_text64(statement, 7, store.schema_id.data(), store.schema_id.size(), SQLITE_STATIC,
                      SQLITE_UTF8);
  sqlite3_bind_int64(statement, 8, static_cast<sqlite3_int64>(store.schema_version));
  sqlite3_bind_int(statement, 9, static_cast<int>(OVF_PER_RECOVERY_CLEAN_V1));
  status = sqlite3_step(statement);
  sqlite3_finalize(statement);
  return status == SQLITE_DONE ? SQLITE_OK : status;
}

ovf_per_status_v1 Start(ovf_per_backend_v1* backend) {
  auto& self = *Self(backend);
  std::scoped_lock lock(self.mutex);
  if (!EnsureDirectory(self.root)) {
    SetError(self, "cannot create SQLite provider root");
    return OVF_PER_STATUS_IO_ERROR;
  }
  self.running = true;
  return OVF_PER_STATUS_OK;
}

void Stop(ovf_per_backend_v1* backend) {
  auto& self = *Self(backend);
  std::scoped_lock lock(self.mutex);
  self.running = false;
  for (auto& [handle, cursor] : self.cursors) {
    static_cast<void>(handle);
    sqlite3_finalize(cursor.statement);
  }
  self.cursors.clear();
  self.migrations.clear();
  for (auto& [handle, transaction] : self.transactions) {
    static_cast<void>(handle);
    if (!transaction.finished) {
      Execute(transaction.database, "ROLLBACK;");
    }
    sqlite3_close(transaction.database);
  }
  self.transactions.clear();
  for (auto& [handle, blob] : self.blobs) {
    static_cast<void>(handle);
    if (blob.blob != nullptr) {
      sqlite3_blob_close(blob.blob);
    }
    if (!blob.finished) {
      Execute(blob.database, "ROLLBACK;");
    }
    sqlite3_close(blob.database);
  }
  self.blobs.clear();
  self.store_handles.clear();
}

ovf_per_status_v1 Capabilities(ovf_per_backend_v1* backend, ovf_per_capabilities_v1* output) {
  if (output == nullptr || output->struct_size < sizeof(*output)) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  const auto& self = *Self(backend);
  *output = {sizeof(*output),
             self.max_stores,
             self.max_transactions,
             UINT64_C(1024) * 1024U * 1024U,
             4096,
             64U * 1024U * 1024U,
             UINT64_C(1024) * 1024U * 1024U,
             OVF_PER_DURABILITY_MEDIA,
             1,
             1,
             {}};
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 OpenStore(ovf_per_backend_v1* backend,
                            const ovf_per_store_descriptor_v1* descriptor,
                            ovf_per_handle_v1* output) {
  auto& self = *Self(backend);
  if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor) || output == nullptr ||
      descriptor->logical_name.data == nullptr || descriptor->logical_name.size == 0 ||
      descriptor->logical_name.size > 256 || descriptor->capacity_bytes == 0 ||
      descriptor->max_entries == 0 || descriptor->max_key_size == 0 ||
      descriptor->max_value_size == 0 || descriptor->max_blob_size == 0 ||
      descriptor->schema_id.data == nullptr || descriptor->schema_id.size == 0 ||
      descriptor->schema_version == 0) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  if (!self.running) {
    return OVF_PER_STATUS_SHUTTING_DOWN;
  }
  const std::string name(descriptor->logical_name.data, descriptor->logical_name.size);
  std::shared_ptr<StoreData> store;
  if (const auto found = self.stores.find(name); found != self.stores.end()) {
    store = found->second.lock();
  }
  if (store == nullptr) {
    if (self.stores.size() >= self.max_stores) {
      SetError(self, "store limit reached");
      return OVF_PER_STATUS_RESOURCE_EXHAUSTED;
    }
    store = std::make_shared<StoreData>(StoreData{
        self.root + "/" + Hex(name) + ".db", self.journal_mode, descriptor->capacity_bytes,
        descriptor->max_entries, descriptor->max_key_size, descriptor->max_value_size,
        descriptor->max_blob_size,
        std::string(descriptor->schema_id.data, descriptor->schema_id.size),
        descriptor->schema_version, descriptor->minimum_durability, OVF_PER_RECOVERY_CLEAN_V1});
    const bool hot_journal = HasHotJournal(store->path);
    sqlite3* database{};
    int status = OpenDatabase(*store, self.busy_timeout_ms, &database);
    if (status == SQLITE_OK) {
      status = InitializeDatabase(database, *store);
    }
    if (status != SQLITE_OK) {
      const auto result = Fail(self, database, status, "cannot initialize SQLite store");
      if (database != nullptr) {
        sqlite3_close(database);
      }
      return result;
    }
    if (!ReadMetadata(database, *store, nullptr)) {
      SetError(self, "SQLite store metadata does not match deployment");
      sqlite3_close(database);
      return OVF_PER_STATUS_INVALID_ARGUMENT;
    }
    if (hot_journal) {
      const int recovery_status = Execute(
          database,
          "UPDATE ovf_meta SET recovery_count=recovery_count+1,recovery_state=2 WHERE id=1;");
      if (recovery_status != SQLITE_OK) {
        const auto result =
            Fail(self, database, recovery_status, "cannot record SQLite journal recovery");
        sqlite3_close(database);
        return result;
      }
      store->open_recovery_state = OVF_PER_RECOVERY_JOURNAL_REPLAYED_V1;
    }
    sqlite3_close(database);
    self.stores[name] = store;
  } else if (store->capacity != descriptor->capacity_bytes ||
             store->max_entries != descriptor->max_entries ||
             store->max_key_size != descriptor->max_key_size ||
             store->max_value_size != descriptor->max_value_size ||
             store->max_blob_size != descriptor->max_blob_size ||
             store->schema_id !=
                 std::string_view(descriptor->schema_id.data, descriptor->schema_id.size) ||
             store->schema_version != descriptor->schema_version ||
             store->minimum_durability != descriptor->minimum_durability) {
    SetError(self, "SQLite store was reopened with incompatible deployment");
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  const auto handle = self.next_handle++;
  self.store_handles.emplace(handle, StoreHandle{std::move(store), descriptor->access});
  *output = handle;
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 CloseStore(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle) {
  auto& self = *Self(backend);
  std::scoped_lock lock(self.mutex);
  return self.store_handles.erase(handle) == 1 ? OVF_PER_STATUS_OK : OVF_PER_STATUS_NOT_FOUND;
}

bool MigrationActive(sqlite3* database);

ovf_per_status_v1 Begin(ovf_per_backend_v1* backend, ovf_per_handle_v1 store_handle,
                        ovf_per_durability_v1 durability, bool write,
                        const std::uint64_t* expected_generation, ovf_per_handle_v1* output,
                        std::uint64_t* generation) {
  auto& self = *Self(backend);
  if (output == nullptr || generation == nullptr || durability < OVF_PER_DURABILITY_BUFFERED ||
      durability > OVF_PER_DURABILITY_MEDIA) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto store = self.store_handles.find(store_handle);
  if (store == self.store_handles.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  if (self.transactions.size() >= self.max_transactions) {
    return OVF_PER_STATUS_RESOURCE_EXHAUSTED;
  }
  if (write && store->second.access != OVF_PER_ACCESS_READ_WRITE) {
    return OVF_PER_STATUS_PERMISSION_DENIED;
  }
  if (write && durability < store->second.store->minimum_durability) {
    SetError(self, "transaction durability is below deployment minimum");
    return OVF_PER_STATUS_PERMISSION_DENIED;
  }
  sqlite3* database{};
  int status = OpenDatabase(*store->second.store, self.busy_timeout_ms, &database);
  if (status == SQLITE_OK) {
    const char* synchronous = durability == OVF_PER_DURABILITY_MEDIA ? "PRAGMA synchronous=EXTRA;"
                              : durability == OVF_PER_DURABILITY_PROCESS_CRASH
                                  ? "PRAGMA synchronous=NORMAL;"
                                  : "PRAGMA synchronous=OFF;";
    status = Execute(database, synchronous);
  }
  if (status == SQLITE_OK) {
    status = Execute(database, write ? "BEGIN IMMEDIATE;" : "BEGIN;");
  }
  std::uint64_t current_generation{};
  if (status == SQLITE_OK && !ReadMetadata(database, *store->second.store, &current_generation)) {
    status = SQLITE_CORRUPT;
  }
  if (status == SQLITE_OK && write && MigrationActive(database)) {
    Execute(database, "ROLLBACK;");
    sqlite3_close(database);
    return OVF_PER_STATUS_BUSY;
  }
  if (status == SQLITE_OK && write && expected_generation != nullptr &&
      current_generation != *expected_generation) {
    Execute(database, "ROLLBACK;");
    sqlite3_close(database);
    return OVF_PER_STATUS_CONFLICT;
  }
  if (status != SQLITE_OK) {
    const auto result = Fail(self, database, status, "cannot begin SQLite transaction");
    if (database != nullptr) {
      sqlite3_close(database);
    }
    return result;
  }
  const auto handle = self.next_handle++;
  self.transactions.emplace(handle, Transaction{store->second.store, database, current_generation,
                                                durability, write, false, false});
  *output = handle;
  *generation = current_generation;
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 BeginRead(ovf_per_backend_v1* backend, ovf_per_handle_v1 store,
                            ovf_per_handle_v1* output, std::uint64_t* generation) {
  return Begin(backend, store, OVF_PER_DURABILITY_BUFFERED, false, nullptr, output, generation);
}

ovf_per_status_v1 BeginWrite(ovf_per_backend_v1* backend, ovf_per_handle_v1 store,
                             ovf_per_durability_v1 durability, ovf_per_handle_v1* output,
                             std::uint64_t* generation) {
  return Begin(backend, store, durability, true, nullptr, output, generation);
}

ovf_per_status_v1 BeginWriteAt(ovf_per_backend_v1* backend, ovf_per_handle_v1 store,
                               ovf_per_durability_v1 durability, std::uint64_t expected_generation,
                               ovf_per_handle_v1* output, std::uint64_t* generation) {
  return Begin(backend, store, durability, true, &expected_generation, output, generation);
}

ovf_per_status_v1 Get(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle,
                      ovf_per_bytes_view_v1 key, ovf_per_mutable_bytes_v1* output) {
  auto& self = *Self(backend);
  if (key.data == nullptr || key.size == 0 || output == nullptr) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto transaction = self.transactions.find(handle);
  if (transaction == self.transactions.end() || transaction->second.finished) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  sqlite3_stmt* statement{};
  int status =
      sqlite3_prepare_v2(transaction->second.database, "SELECT value FROM ovf_values WHERE key=?1",
                         -1, &statement, nullptr);
  if (status == SQLITE_OK) {
    status = sqlite3_bind_blob64(statement, 1, key.data, key.size, SQLITE_STATIC);
  }
  if (status == SQLITE_OK) {
    status = sqlite3_step(statement);
  }
  if (status == SQLITE_DONE) {
    sqlite3_finalize(statement);
    return OVF_PER_STATUS_NOT_FOUND;
  }
  if (status != SQLITE_ROW) {
    const auto result =
        Fail(self, transaction->second.database, status, "cannot read SQLite value");
    sqlite3_finalize(statement);
    return result;
  }
  const auto size = static_cast<size_t>(sqlite3_column_bytes(statement, 0));
  if (output->data == nullptr || output->size < size) {
    output->size = size;
    sqlite3_finalize(statement);
    return OVF_PER_STATUS_BUFFER_TOO_SMALL;
  }
  if (size != 0) {
    std::memcpy(output->data, sqlite3_column_blob(statement, 0), size);
  }
  output->size = size;
  sqlite3_finalize(statement);
  return OVF_PER_STATUS_OK;
}

bool WithinQuota(sqlite3* database, const StoreData& store) {
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(database,
                         "SELECT sum(entries),sum(bytes) FROM (SELECT count(*) entries,"
                         "coalesce(sum(length(key)+length(value)),0) bytes FROM ovf_values "
                         "UNION ALL SELECT count(*),coalesce(sum(length(key)+length(value)),0) "
                         "FROM ovf_blobs)",
                         -1, &statement, nullptr) != SQLITE_OK) {
    return false;
  }
  const bool valid =
      sqlite3_step(statement) == SQLITE_ROW &&
      static_cast<std::uint64_t>(sqlite3_column_int64(statement, 0)) <= store.max_entries &&
      static_cast<std::uint64_t>(sqlite3_column_int64(statement, 1)) <= store.capacity;
  sqlite3_finalize(statement);
  return valid;
}

bool MigrationActive(sqlite3* database) {
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(database, "SELECT count(*) FROM ovf_migration", -1, &statement, nullptr) !=
      SQLITE_OK) {
    return true;
  }
  const bool active =
      sqlite3_step(statement) != SQLITE_ROW || sqlite3_column_int(statement, 0) != 0;
  sqlite3_finalize(statement);
  return active;
}

ovf_per_status_v1 Put(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle,
                      ovf_per_bytes_view_v1 key, ovf_per_bytes_view_v1 value) {
  auto& self = *Self(backend);
  if (key.data == nullptr || key.size == 0 || (value.data == nullptr && value.size != 0)) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto transaction = self.transactions.find(handle);
  if (transaction == self.transactions.end() || !transaction->second.write ||
      transaction->second.finished) {
    return OVF_PER_STATUS_INVALID_STATE;
  }
  auto& item = transaction->second;
  if (key.size > item.store->max_key_size || value.size > item.store->max_value_size) {
    SetError(self, "key or value exceeds its configured bound");
    return OVF_PER_STATUS_QUOTA_EXCEEDED;
  }
  int status = Execute(item.database, "SAVEPOINT ovf_mutation;");
  sqlite3_stmt* statement{};
  if (status == SQLITE_OK) {
    status = sqlite3_prepare_v2(item.database,
                                "INSERT INTO ovf_values VALUES(?1,?2) ON CONFLICT(key) DO UPDATE "
                                "SET value=excluded.value",
                                -1, &statement, nullptr);
  }
  if (status == SQLITE_OK) {
    status = sqlite3_bind_blob64(statement, 1, key.data, key.size, SQLITE_STATIC);
  }
  if (status == SQLITE_OK) {
    status = sqlite3_bind_blob64(statement, 2, value.data, value.size, SQLITE_STATIC);
  }
  if (status == SQLITE_OK) {
    status = sqlite3_step(statement);
  }
  sqlite3_finalize(statement);
  if (status != SQLITE_DONE || !WithinQuota(item.database, *item.store)) {
    Execute(item.database, "ROLLBACK TO ovf_mutation; RELEASE ovf_mutation;");
    if (status == SQLITE_DONE) {
      SetError(self, "transaction exceeds store quota");
      return OVF_PER_STATUS_QUOTA_EXCEEDED;
    }
    return Fail(self, item.database, status, "cannot write SQLite value");
  }
  status = Execute(item.database, "RELEASE ovf_mutation;");
  return status == SQLITE_OK ? OVF_PER_STATUS_OK
                             : Fail(self, item.database, status, "cannot release SQLite savepoint");
}

ovf_per_status_v1 Erase(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle,
                        ovf_per_bytes_view_v1 key, std::uint8_t* erased) {
  auto& self = *Self(backend);
  if (key.data == nullptr || key.size == 0 || erased == nullptr) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto transaction = self.transactions.find(handle);
  if (transaction == self.transactions.end() || !transaction->second.write ||
      transaction->second.finished) {
    return OVF_PER_STATUS_INVALID_STATE;
  }
  sqlite3_stmt* statement{};
  int status = sqlite3_prepare_v2(transaction->second.database,
                                  "DELETE FROM ovf_values WHERE key=?1", -1, &statement, nullptr);
  if (status == SQLITE_OK) {
    status = sqlite3_bind_blob64(statement, 1, key.data, key.size, SQLITE_STATIC);
  }
  if (status == SQLITE_OK) {
    status = sqlite3_step(statement);
  }
  sqlite3_finalize(statement);
  if (status != SQLITE_DONE) {
    return Fail(self, transaction->second.database, status, "cannot erase SQLite value");
  }
  *erased = sqlite3_changes(transaction->second.database) == 1 ? 1 : 0;
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 Commit(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle,
                         ovf_per_commit_result_v1* output) {
  auto& self = *Self(backend);
  if (output == nullptr || output->struct_size < sizeof(*output)) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto transaction = self.transactions.find(handle);
  if (transaction == self.transactions.end() || !transaction->second.write ||
      transaction->second.finished) {
    return OVF_PER_STATUS_INVALID_STATE;
  }
  auto& item = transaction->second;
  int status = SQLITE_OK;
  if (!item.commit_prepared) {
    sqlite3_stmt* statement{};
    status = sqlite3_prepare_v2(
        item.database,
        "UPDATE ovf_meta SET generation=generation+1,successful_commits=successful_commits+1 "
        "WHERE id=1 AND generation=?1",
        -1, &statement, nullptr);
    if (status == SQLITE_OK) {
      status = sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(item.base_generation));
    }
    if (status == SQLITE_OK) {
      status = sqlite3_step(statement);
    }
    sqlite3_finalize(statement);
    if (status != SQLITE_DONE) {
      return Fail(self, item.database, status, "cannot advance SQLite generation");
    }
    if (sqlite3_changes(item.database) != 1) {
      return OVF_PER_STATUS_CONFLICT;
    }
    item.commit_prepared = true;
  }
  status = Execute(item.database, "COMMIT;");
  if (status != SQLITE_OK) {
    return Fail(self, item.database, status, "cannot commit SQLite transaction");
  }
  item.finished = true;
  *output = {sizeof(*output), item.base_generation + 1, item.durability};
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 Abort(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle) {
  auto& self = *Self(backend);
  std::scoped_lock lock(self.mutex);
  const auto transaction = self.transactions.find(handle);
  if (transaction == self.transactions.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  if (!transaction->second.finished) {
    const int status = Execute(transaction->second.database, "ROLLBACK;");
    if (status != SQLITE_OK) {
      return Fail(self, transaction->second.database, status,
                  "cannot roll back SQLite transaction");
    }
    transaction->second.finished = true;
  }
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 CloseTransaction(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle) {
  auto& self = *Self(backend);
  std::scoped_lock lock(self.mutex);
  const auto transaction = self.transactions.find(handle);
  if (transaction == self.transactions.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  for (auto cursor = self.cursors.begin(); cursor != self.cursors.end();) {
    if (cursor->second.transaction == handle) {
      sqlite3_finalize(cursor->second.statement);
      cursor = self.cursors.erase(cursor);
    } else {
      ++cursor;
    }
  }
  if (!transaction->second.finished) {
    Execute(transaction->second.database, "ROLLBACK;");
  }
  const int status = sqlite3_close(transaction->second.database);
  self.transactions.erase(transaction);
  return status == SQLITE_OK ? OVF_PER_STATUS_OK
                             : Fail(self, nullptr, status, "cannot close SQLite transaction");
}

ovf_per_status_v1 OpenBlobRead(ovf_per_backend_v1* backend, ovf_per_handle_v1 store_handle,
                               ovf_per_bytes_view_v1 key, ovf_per_handle_v1* output,
                               std::uint64_t* size, std::uint64_t* generation) {
  auto& self = *Self(backend);
  if (key.data == nullptr || key.size == 0 || output == nullptr || size == nullptr ||
      generation == nullptr) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto handle = self.store_handles.find(store_handle);
  if (handle == self.store_handles.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  sqlite3* database{};
  int status = OpenDatabase(*handle->second.store, self.busy_timeout_ms, &database);
  if (status == SQLITE_OK) {
    status = Execute(database, "PRAGMA synchronous=OFF; BEGIN;");
  }
  std::uint64_t current_generation{};
  if (status == SQLITE_OK && !ReadMetadata(database, *handle->second.store, &current_generation)) {
    status = SQLITE_CORRUPT;
  }
  sqlite3_int64 rowid{};
  std::uint64_t blob_size{};
  sqlite3_stmt* statement{};
  if (status == SQLITE_OK) {
    status = sqlite3_prepare_v2(database, "SELECT rowid,length(value) FROM ovf_blobs WHERE key=?1",
                                -1, &statement, nullptr);
  }
  if (status == SQLITE_OK) {
    status = sqlite3_bind_blob64(statement, 1, key.data, key.size, SQLITE_STATIC);
  }
  if (status == SQLITE_OK) {
    status = sqlite3_step(statement);
  }
  if (status == SQLITE_ROW) {
    rowid = sqlite3_column_int64(statement, 0);
    blob_size = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 1));
    status = blob_size <= INT32_MAX ? SQLITE_OK : SQLITE_TOOBIG;
  } else if (status == SQLITE_DONE) {
    sqlite3_finalize(statement);
    Execute(database, "ROLLBACK;");
    sqlite3_close(database);
    return OVF_PER_STATUS_NOT_FOUND;
  }
  sqlite3_finalize(statement);
  sqlite3_blob* blob{};
  if (status == SQLITE_OK && blob_size != 0) {
    status = sqlite3_blob_open(database, "main", "ovf_blobs", "value", rowid, 0, &blob);
  }
  if (status != SQLITE_OK) {
    const auto result = Fail(self, database, status, "cannot open SQLite blob reader");
    Execute(database, "ROLLBACK;");
    sqlite3_close(database);
    return result;
  }
  const auto blob_handle = self.next_handle++;
  self.blobs.emplace(blob_handle, BlobOperation{handle->second.store, database, blob,
                                                current_generation, blob_size, blob_size,
                                                OVF_PER_DURABILITY_BUFFERED, false, false, false});
  *output = blob_handle;
  *size = blob_size;
  *generation = current_generation;
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 BeginBlobReplace(ovf_per_backend_v1* backend, ovf_per_handle_v1 store_handle,
                                   ovf_per_bytes_view_v1 key, std::uint64_t size,
                                   ovf_per_durability_v1 durability, ovf_per_handle_v1* output,
                                   std::uint64_t* generation) {
  auto& self = *Self(backend);
  if (key.data == nullptr || key.size == 0 || output == nullptr || generation == nullptr) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto handle = self.store_handles.find(store_handle);
  if (handle == self.store_handles.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  const auto& store = *handle->second.store;
  if (handle->second.access != OVF_PER_ACCESS_READ_WRITE) {
    return OVF_PER_STATUS_PERMISSION_DENIED;
  }
  if (durability < store.minimum_durability) {
    return OVF_PER_STATUS_PERMISSION_DENIED;
  }
  if (key.size > store.max_key_size || size > store.max_blob_size || size > INT32_MAX) {
    return OVF_PER_STATUS_QUOTA_EXCEEDED;
  }
  sqlite3* database{};
  int status = OpenDatabase(store, self.busy_timeout_ms, &database);
  if (status == SQLITE_OK) {
    const char* synchronous = durability == OVF_PER_DURABILITY_MEDIA ? "PRAGMA synchronous=EXTRA;"
                              : durability == OVF_PER_DURABILITY_PROCESS_CRASH
                                  ? "PRAGMA synchronous=NORMAL;"
                                  : "PRAGMA synchronous=OFF;";
    status = Execute(database, synchronous);
  }
  if (status == SQLITE_OK) {
    status = Execute(database, "BEGIN IMMEDIATE;");
  }
  std::uint64_t current_generation{};
  if (status == SQLITE_OK && !ReadMetadata(database, *handle->second.store, &current_generation)) {
    status = SQLITE_CORRUPT;
  }
  sqlite3_stmt* statement{};
  if (status == SQLITE_OK) {
    status = sqlite3_prepare_v2(database,
                                "INSERT INTO ovf_blobs(key,value) VALUES(?1,?2) ON CONFLICT(key) "
                                "DO UPDATE SET value=excluded.value",
                                -1, &statement, nullptr);
  }
  if (status == SQLITE_OK) {
    status = sqlite3_bind_blob64(statement, 1, key.data, key.size, SQLITE_STATIC);
  }
  if (status == SQLITE_OK) {
    status = sqlite3_bind_zeroblob64(statement, 2, size);
  }
  if (status == SQLITE_OK) {
    status = sqlite3_step(statement);
  }
  sqlite3_finalize(statement);
  if (status == SQLITE_DONE) {
    status = WithinQuota(database, store) ? SQLITE_OK : SQLITE_FULL;
  }
  sqlite3_int64 rowid{};
  statement = nullptr;
  if (status == SQLITE_OK) {
    status = sqlite3_prepare_v2(database, "SELECT rowid FROM ovf_blobs WHERE key=?1", -1,
                                &statement, nullptr);
  }
  if (status == SQLITE_OK) {
    status = sqlite3_bind_blob64(statement, 1, key.data, key.size, SQLITE_STATIC);
  }
  if (status == SQLITE_OK) {
    status = sqlite3_step(statement);
  }
  if (status == SQLITE_ROW) {
    rowid = sqlite3_column_int64(statement, 0);
    status = SQLITE_OK;
  }
  sqlite3_finalize(statement);
  sqlite3_blob* blob{};
  if (status == SQLITE_OK && size != 0) {
    status = sqlite3_blob_open(database, "main", "ovf_blobs", "value", rowid, 1, &blob);
  }
  if (status != SQLITE_OK) {
    const auto result = Fail(self, database, status, "cannot begin SQLite blob replacement");
    Execute(database, "ROLLBACK;");
    sqlite3_close(database);
    return result;
  }
  const auto blob_handle = self.next_handle++;
  self.blobs.emplace(blob_handle,
                     BlobOperation{handle->second.store, database, blob, current_generation, size,
                                   0, durability, true, false, false});
  *output = blob_handle;
  *generation = current_generation;
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 ReadBlob(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle,
                           std::uint64_t offset, ovf_per_mutable_bytes_v1* output) {
  auto& self = *Self(backend);
  if (output == nullptr || (output->data == nullptr && output->size != 0)) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto found = self.blobs.find(handle);
  if (found == self.blobs.end() || found->second.write || found->second.finished ||
      offset > found->second.size || output->size > found->second.size - offset) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  if (output->size == 0) {
    return OVF_PER_STATUS_OK;
  }
  const int status = sqlite3_blob_read(found->second.blob, output->data,
                                       static_cast<int>(output->size), static_cast<int>(offset));
  return status == SQLITE_OK
             ? OVF_PER_STATUS_OK
             : Fail(self, found->second.database, status, "cannot read SQLite blob");
}

ovf_per_status_v1 WriteBlob(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle,
                            std::uint64_t offset, ovf_per_bytes_view_v1 input) {
  auto& self = *Self(backend);
  if (input.data == nullptr && input.size != 0) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto found = self.blobs.find(handle);
  if (found == self.blobs.end() || !found->second.write || found->second.finished ||
      offset != found->second.written || input.size > found->second.size - offset) {
    return OVF_PER_STATUS_INVALID_STATE;
  }
  if (input.size == 0) {
    return OVF_PER_STATUS_OK;
  }
  const int status = sqlite3_blob_write(found->second.blob, input.data,
                                        static_cast<int>(input.size), static_cast<int>(offset));
  if (status != SQLITE_OK) {
    return Fail(self, found->second.database, status, "cannot write SQLite blob");
  }
  found->second.written += input.size;
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 CommitBlob(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle,
                             ovf_per_commit_result_v1* output) {
  auto& self = *Self(backend);
  if (output == nullptr || output->struct_size < sizeof(*output)) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto found = self.blobs.find(handle);
  if (found == self.blobs.end() || !found->second.write || found->second.finished ||
      found->second.written != found->second.size) {
    return OVF_PER_STATUS_INVALID_STATE;
  }
  auto& item = found->second;
  if (item.blob != nullptr) {
    const int status = sqlite3_blob_close(item.blob);
    item.blob = nullptr;
    if (status != SQLITE_OK) {
      return Fail(self, item.database, status, "cannot close SQLite blob writer");
    }
  }
  int status = SQLITE_OK;
  if (!item.commit_prepared) {
    sqlite3_stmt* statement{};
    status = sqlite3_prepare_v2(
        item.database,
        "UPDATE ovf_meta SET generation=generation+1,successful_commits=successful_commits+1 "
        "WHERE id=1 AND generation=?1",
        -1, &statement, nullptr);
    if (status == SQLITE_OK) {
      status = sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(item.base_generation));
    }
    if (status == SQLITE_OK) {
      status = sqlite3_step(statement);
    }
    sqlite3_finalize(statement);
    if (status != SQLITE_DONE) {
      return Fail(self, item.database, status, "cannot advance SQLite blob generation");
    }
    if (sqlite3_changes(item.database) != 1) {
      return OVF_PER_STATUS_CONFLICT;
    }
    item.commit_prepared = true;
  }
  status = Execute(item.database, "COMMIT;");
  if (status != SQLITE_OK) {
    return Fail(self, item.database, status, "cannot commit SQLite blob replacement");
  }
  item.finished = true;
  *output = {sizeof(*output), item.base_generation + 1, item.durability};
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 AbortBlob(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle) {
  auto& self = *Self(backend);
  std::scoped_lock lock(self.mutex);
  const auto found = self.blobs.find(handle);
  if (found == self.blobs.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  if (found->second.blob != nullptr) {
    sqlite3_blob_close(found->second.blob);
    found->second.blob = nullptr;
  }
  if (!found->second.finished) {
    const int status = Execute(found->second.database, "ROLLBACK;");
    if (status != SQLITE_OK) {
      return Fail(self, found->second.database, status, "cannot abort SQLite blob replacement");
    }
    found->second.finished = true;
  }
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 CloseBlob(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle) {
  auto& self = *Self(backend);
  std::scoped_lock lock(self.mutex);
  const auto found = self.blobs.find(handle);
  if (found == self.blobs.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  if (found->second.blob != nullptr) {
    sqlite3_blob_close(found->second.blob);
  }
  if (!found->second.finished) {
    Execute(found->second.database, "ROLLBACK;");
  }
  const int status = sqlite3_close(found->second.database);
  self.blobs.erase(found);
  return status == SQLITE_OK ? OVF_PER_STATUS_OK
                             : Fail(self, nullptr, status, "cannot close SQLite blob operation");
}

ovf_per_status_v1 OpenCursor(ovf_per_backend_v1* backend, ovf_per_handle_v1 transaction_handle,
                             ovf_per_bytes_view_v1 prefix, ovf_per_handle_v1* output) {
  auto& self = *Self(backend);
  if ((prefix.data == nullptr && prefix.size != 0) || output == nullptr) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto transaction = self.transactions.find(transaction_handle);
  if (transaction == self.transactions.end() || transaction->second.finished ||
      transaction->second.write) {
    return OVF_PER_STATUS_INVALID_STATE;
  }
  sqlite3_stmt* statement{};
  const char* sql = prefix.size == 0
                        ? "SELECT key,value FROM ovf_values ORDER BY key"
                        : "SELECT key,value FROM ovf_values WHERE substr(key,1,?1)=?2 ORDER BY key";
  int status = sqlite3_prepare_v2(transaction->second.database, sql, -1, &statement, nullptr);
  if (status == SQLITE_OK && prefix.size != 0) {
    status = sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(prefix.size));
  }
  if (status == SQLITE_OK && prefix.size != 0) {
    status = sqlite3_bind_blob64(statement, 2, prefix.data, prefix.size, SQLITE_STATIC);
  }
  if (status != SQLITE_OK) {
    sqlite3_finalize(statement);
    return Fail(self, transaction->second.database, status, "cannot open SQLite cursor");
  }
  const auto handle = self.next_handle++;
  self.cursors.emplace(handle, Cursor{transaction_handle, statement, false});
  *output = handle;
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 NextCursor(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle,
                             ovf_per_mutable_bytes_v1* key, ovf_per_mutable_bytes_v1* value) {
  auto& self = *Self(backend);
  if (key == nullptr || value == nullptr) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto cursor = self.cursors.find(handle);
  if (cursor == self.cursors.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  if (!cursor->second.row_ready) {
    const int status = sqlite3_step(cursor->second.statement);
    if (status == SQLITE_DONE) {
      return OVF_PER_STATUS_NOT_FOUND;
    }
    if (status != SQLITE_ROW) {
      return Fail(self, sqlite3_db_handle(cursor->second.statement), status,
                  "cannot advance SQLite cursor");
    }
    cursor->second.row_ready = true;
  }
  const auto key_size = static_cast<std::size_t>(sqlite3_column_bytes(cursor->second.statement, 0));
  const auto value_size =
      static_cast<std::size_t>(sqlite3_column_bytes(cursor->second.statement, 1));
  if ((key_size != 0 && (key->data == nullptr || key->size < key_size)) ||
      (value_size != 0 && (value->data == nullptr || value->size < value_size))) {
    key->size = key_size;
    value->size = value_size;
    return OVF_PER_STATUS_BUFFER_TOO_SMALL;
  }
  if (key_size != 0) {
    std::memcpy(key->data, sqlite3_column_blob(cursor->second.statement, 0), key_size);
  }
  if (value_size != 0) {
    std::memcpy(value->data, sqlite3_column_blob(cursor->second.statement, 1), value_size);
  }
  key->size = key_size;
  value->size = value_size;
  cursor->second.row_ready = false;
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 CloseCursor(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle) {
  auto& self = *Self(backend);
  std::scoped_lock lock(self.mutex);
  const auto cursor = self.cursors.find(handle);
  if (cursor == self.cursors.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  const int status = sqlite3_finalize(cursor->second.statement);
  self.cursors.erase(cursor);
  return status == SQLITE_OK ? OVF_PER_STATUS_OK
                             : Fail(self, nullptr, status, "cannot close SQLite cursor");
}

ovf_per_status_v1 ResetStore(ovf_per_backend_v1* backend, ovf_per_handle_v1 store_handle,
                             const ovf_per_entry_v1* entries, std::size_t count,
                             ovf_per_durability_v1 durability, ovf_per_commit_result_v1* output) {
  auto& self = *Self(backend);
  if ((entries == nullptr && count != 0) || output == nullptr ||
      output->struct_size < sizeof(*output)) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto handle = self.store_handles.find(store_handle);
  if (handle == self.store_handles.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  const auto& store = *handle->second.store;
  if (handle->second.access != OVF_PER_ACCESS_READ_WRITE) {
    return OVF_PER_STATUS_PERMISSION_DENIED;
  }
  if (durability < store.minimum_durability) {
    return OVF_PER_STATUS_PERMISSION_DENIED;
  }
  sqlite3* database{};
  int status = OpenDatabase(store, self.busy_timeout_ms, &database);
  if (status == SQLITE_OK) {
    const char* synchronous = durability == OVF_PER_DURABILITY_MEDIA ? "PRAGMA synchronous=EXTRA;"
                              : durability == OVF_PER_DURABILITY_PROCESS_CRASH
                                  ? "PRAGMA synchronous=NORMAL;"
                                  : "PRAGMA synchronous=OFF;";
    status = Execute(database, synchronous);
  }
  if (status == SQLITE_OK) {
    status = Execute(database, "BEGIN IMMEDIATE; DELETE FROM ovf_values; DELETE FROM ovf_blobs;");
  }
  sqlite3_stmt* statement{};
  if (status == SQLITE_OK) {
    status = sqlite3_prepare_v2(database, "INSERT INTO ovf_values VALUES(?1,?2)", -1, &statement,
                                nullptr);
  }
  for (std::size_t index = 0; status == SQLITE_OK && index < count; ++index) {
    if (entries[index].key.data == nullptr || entries[index].key.size == 0 ||
        entries[index].key.size > store.max_key_size ||
        (entries[index].value.data == nullptr && entries[index].value.size != 0) ||
        entries[index].value.size > store.max_value_size) {
      status = SQLITE_MISUSE;
      break;
    }
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    status = sqlite3_bind_blob64(statement, 1, entries[index].key.data, entries[index].key.size,
                                 SQLITE_STATIC);
    if (status == SQLITE_OK) {
      status = sqlite3_bind_blob64(statement, 2, entries[index].value.data,
                                   entries[index].value.size, SQLITE_STATIC);
    }
    if (status == SQLITE_OK) {
      status = sqlite3_step(statement) == SQLITE_DONE ? SQLITE_OK : sqlite3_errcode(database);
    }
  }
  sqlite3_finalize(statement);
  if (status == SQLITE_OK && !WithinQuota(database, store)) {
    status = SQLITE_FULL;
  }
  std::uint64_t generation{};
  if (status == SQLITE_OK && !ReadMetadata(database, *handle->second.store, &generation)) {
    status = SQLITE_CORRUPT;
  }
  if (status == SQLITE_OK) {
    status =
        Execute(database, "UPDATE ovf_meta SET generation=generation+1,"
                          "successful_commits=successful_commits+1,recovery_count=recovery_count+1,"
                          "recovery_state=4 WHERE id=1; COMMIT;");
  }
  if (status != SQLITE_OK) {
    Execute(database, "ROLLBACK;");
    const auto result = status == SQLITE_MISUSE
                            ? OVF_PER_STATUS_INVALID_ARGUMENT
                            : Fail(self, database, status, "cannot reset SQLite store");
    sqlite3_close(database);
    return result;
  }
  sqlite3_close(database);
  *output = {sizeof(*output), generation + 1, durability};
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 StoreStatus(ovf_per_backend_v1* backend, ovf_per_handle_v1 store_handle,
                              ovf_per_store_status_v1* output) {
  auto& self = *Self(backend);
  if (output == nullptr || output->struct_size < sizeof(*output)) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto handle = self.store_handles.find(store_handle);
  if (handle == self.store_handles.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  sqlite3* database{};
  int status = OpenDatabase(*handle->second.store, self.busy_timeout_ms, &database);
  std::uint64_t generation{};
  if (status == SQLITE_OK && !ReadMetadata(database, *handle->second.store, &generation)) {
    status = SQLITE_CORRUPT;
  }
  if (status != SQLITE_OK) {
    const auto result = Fail(self, database, status, "cannot read SQLite store status");
    sqlite3_close(database);
    return result;
  }
  sqlite3_stmt* statement{};
  status = sqlite3_prepare_v2(database,
                              "SELECT generation,schema_version,recovery_state,"
                              "successful_commits,rejected_operations,recovery_count "
                              "FROM ovf_meta WHERE id=1",
                              -1, &statement, nullptr);
  if (status == SQLITE_OK) {
    status = sqlite3_step(statement);
  }
  if (status != SQLITE_ROW) {
    const auto result = Fail(self, database, status, "cannot read SQLite health counters");
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return result;
  }
  *output = {sizeof(*output),
             static_cast<std::uint64_t>(sqlite3_column_int64(statement, 0)),
             static_cast<std::uint64_t>(sqlite3_column_int64(statement, 1)),
             static_cast<ovf_per_recovery_state_v1>(sqlite3_column_int(statement, 2)),
             static_cast<std::uint64_t>(sqlite3_column_int64(statement, 3)),
             static_cast<std::uint64_t>(sqlite3_column_int64(statement, 4)),
             static_cast<std::uint64_t>(sqlite3_column_int64(statement, 5))};
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return OVF_PER_STATUS_OK;
}

bool Same(ovf_per_string_view_v1 value, const std::string& expected) {
  return value.data != nullptr && std::string_view(value.data, value.size) == expected;
}

ovf_per_status_v1 BeginMigration(ovf_per_backend_v1* backend, ovf_per_handle_v1 store_handle,
                                 const ovf_per_migration_descriptor_v1* descriptor,
                                 ovf_per_handle_v1* output,
                                 ovf_per_migration_status_v1* migration_status,
                                 ovf_per_mutable_bytes_v1* checkpoint) {
  auto& self = *Self(backend);
  if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor) || output == nullptr ||
      migration_status == nullptr || migration_status->struct_size < sizeof(*migration_status) ||
      checkpoint == nullptr || descriptor->migration_id.data == nullptr ||
      descriptor->migration_id.size == 0 || descriptor->target_schema_id.data == nullptr ||
      descriptor->target_schema_id.size == 0 || descriptor->target_schema_version == 0) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto handle = self.store_handles.find(store_handle);
  if (handle == self.store_handles.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  auto& store = *handle->second.store;
  if (handle->second.access != OVF_PER_ACCESS_READ_WRITE) {
    return OVF_PER_STATUS_PERMISSION_DENIED;
  }
  if (!Same(descriptor->source_schema_id, store.schema_id) ||
      descriptor->source_schema_version != store.schema_version) {
    return OVF_PER_STATUS_CONFLICT;
  }
  const std::string migration_id(descriptor->migration_id.data, descriptor->migration_id.size);
  const std::string target_schema(descriptor->target_schema_id.data,
                                  descriptor->target_schema_id.size);
  sqlite3* database{};
  int status = OpenDatabase(store, self.busy_timeout_ms, &database);
  if (status == SQLITE_OK) {
    status = Execute(database, "BEGIN IMMEDIATE;");
  }
  std::uint64_t generation{};
  if (status == SQLITE_OK && !ReadMetadata(database, store, &generation)) {
    status = SQLITE_CORRUPT;
  }
  sqlite3_stmt* statement{};
  bool resumed{};
  std::uint64_t processed{};
  std::vector<std::uint8_t> last_key;
  if (status == SQLITE_OK) {
    status = sqlite3_prepare_v2(
        database,
        "SELECT source_generation,source_schema_id,source_schema_version,target_schema_id,"
        "target_schema_version,last_key,processed FROM ovf_migration WHERE id=?1",
        -1, &statement, nullptr);
  }
  if (status == SQLITE_OK) {
    status = sqlite3_bind_text64(statement, 1, migration_id.data(), migration_id.size(),
                                 SQLITE_STATIC, SQLITE_UTF8);
  }
  if (status == SQLITE_OK) {
    status = sqlite3_step(statement);
  }
  if (status == SQLITE_ROW) {
    resumed = true;
    generation = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 0));
    const std::string_view source_id(
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
        static_cast<std::size_t>(sqlite3_column_bytes(statement, 1)));
    const auto source_version = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 2));
    const std::string_view target_id(
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 3)),
        static_cast<std::size_t>(sqlite3_column_bytes(statement, 3)));
    const auto target_version = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 4));
    const auto* bytes = static_cast<const std::uint8_t*>(sqlite3_column_blob(statement, 5));
    const auto size = static_cast<std::size_t>(sqlite3_column_bytes(statement, 5));
    if (size != 0) {
      last_key.assign(bytes, bytes + size);
    }
    processed = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 6));
    if (source_id != store.schema_id || source_version != store.schema_version ||
        target_id != target_schema || target_version != descriptor->target_schema_version) {
      status = SQLITE_CONSTRAINT;
    } else {
      status = SQLITE_OK;
    }
  } else if (status == SQLITE_DONE) {
    sqlite3_finalize(statement);
    statement = nullptr;
    if (MigrationActive(database)) {
      status = SQLITE_CONSTRAINT;
    } else {
      status =
          sqlite3_prepare_v2(database, "INSERT INTO ovf_migration VALUES(?1,?2,?3,?4,?5,?6,x'',0)",
                             -1, &statement, nullptr);
      if (status == SQLITE_OK) {
        sqlite3_bind_text64(statement, 1, migration_id.data(), migration_id.size(), SQLITE_STATIC,
                            SQLITE_UTF8);
        sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(generation));
        sqlite3_bind_text64(statement, 3, store.schema_id.data(), store.schema_id.size(),
                            SQLITE_STATIC, SQLITE_UTF8);
        sqlite3_bind_int64(statement, 4, static_cast<sqlite3_int64>(store.schema_version));
        sqlite3_bind_text64(statement, 5, target_schema.data(), target_schema.size(), SQLITE_STATIC,
                            SQLITE_UTF8);
        sqlite3_bind_int64(statement, 6,
                           static_cast<sqlite3_int64>(descriptor->target_schema_version));
        status = sqlite3_step(statement) == SQLITE_DONE ? SQLITE_OK : sqlite3_errcode(database);
      }
    }
  }
  sqlite3_finalize(statement);
  if (status == SQLITE_OK && checkpoint->size < last_key.size()) {
    checkpoint->size = last_key.size();
    status = SQLITE_TOOBIG;
  }
  if (status == SQLITE_OK) {
    if (!last_key.empty()) {
      std::memcpy(checkpoint->data, last_key.data(), last_key.size());
    }
    checkpoint->size = last_key.size();
    status = Execute(database, "COMMIT;");
  }
  if (status != SQLITE_OK) {
    Execute(database, "ROLLBACK;");
    const auto result = status == SQLITE_CONSTRAINT
                            ? OVF_PER_STATUS_CONFLICT
                            : Fail(self, database, status, "cannot begin SQLite migration");
    sqlite3_close(database);
    return result;
  }
  sqlite3_close(database);
  const auto migration_handle = self.next_handle++;
  self.migrations.emplace(migration_handle,
                          MigrationOperation{handle->second.store, migration_id, generation,
                                             target_schema, descriptor->target_schema_version,
                                             descriptor->durability});
  *output = migration_handle;
  *migration_status = {
      sizeof(*migration_status), generation, processed, static_cast<std::uint8_t>(resumed), {}};
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 ApplyMigration(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle,
                                 ovf_per_bytes_view_v1 source_key, const ovf_per_entry_v1* target,
                                 std::uint8_t write_target) {
  auto& self = *Self(backend);
  if (source_key.data == nullptr || source_key.size == 0 || write_target > 1 ||
      (write_target != 0 && target == nullptr)) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto operation = self.migrations.find(handle);
  if (operation == self.migrations.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  const auto& store = *operation->second.store;
  if (write_target != 0 && (target->key.data == nullptr || target->key.size == 0 ||
                            target->key.size > store.max_key_size ||
                            (target->value.data == nullptr && target->value.size != 0) ||
                            target->value.size > store.max_value_size)) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  sqlite3* database{};
  int status = OpenDatabase(store, self.busy_timeout_ms, &database);
  if (status == SQLITE_OK) {
    status = Execute(database, "BEGIN IMMEDIATE;");
  }
  sqlite3_stmt* statement{};
  if (status == SQLITE_OK && write_target != 0) {
    status = sqlite3_prepare_v2(
        database,
        "INSERT INTO ovf_migration_values VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET "
        "value=excluded.value",
        -1, &statement, nullptr);
    if (status == SQLITE_OK) {
      sqlite3_bind_blob64(statement, 1, target->key.data, target->key.size, SQLITE_STATIC);
      sqlite3_bind_blob64(statement, 2, target->value.data, target->value.size, SQLITE_STATIC);
      status = sqlite3_step(statement) == SQLITE_DONE ? SQLITE_OK : sqlite3_errcode(database);
    }
    sqlite3_finalize(statement);
    statement = nullptr;
  }
  if (status == SQLITE_OK) {
    status = sqlite3_prepare_v2(
        database, "UPDATE ovf_migration SET last_key=?1,processed=processed+1 WHERE id=?2", -1,
        &statement, nullptr);
    if (status == SQLITE_OK) {
      sqlite3_bind_blob64(statement, 1, source_key.data, source_key.size, SQLITE_STATIC);
      sqlite3_bind_text64(statement, 2, operation->second.migration_id.data(),
                          operation->second.migration_id.size(), SQLITE_STATIC, SQLITE_UTF8);
      status = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(database) == 1
                   ? SQLITE_OK
                   : SQLITE_CONSTRAINT;
    }
  }
  sqlite3_finalize(statement);
  if (status == SQLITE_OK) {
    sqlite3_stmt* quota{};
    status = sqlite3_prepare_v2(
        database,
        "SELECT count(*),coalesce(sum(length(key)+length(value)),0) FROM ovf_migration_values", -1,
        &quota, nullptr);
    if (status == SQLITE_OK && sqlite3_step(quota) == SQLITE_ROW &&
        (static_cast<std::uint64_t>(sqlite3_column_int64(quota, 0)) > store.max_entries ||
         static_cast<std::uint64_t>(sqlite3_column_int64(quota, 1)) > store.capacity)) {
      status = SQLITE_FULL;
    }
    sqlite3_finalize(quota);
  }
  if (status == SQLITE_OK) {
    status = Execute(database, "COMMIT;");
  }
  if (status != SQLITE_OK) {
    Execute(database, "ROLLBACK;");
    const auto result = Fail(self, database, status, "cannot stage SQLite migration entry");
    sqlite3_close(database);
    return result;
  }
  sqlite3_close(database);
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 CommitMigration(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle,
                                  ovf_per_commit_result_v1* output) {
  auto& self = *Self(backend);
  if (output == nullptr || output->struct_size < sizeof(*output)) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto operation = self.migrations.find(handle);
  if (operation == self.migrations.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  auto& item = operation->second;
  sqlite3* database{};
  int status = OpenDatabase(*item.store, self.busy_timeout_ms, &database);
  if (status == SQLITE_OK) {
    status = Execute(
        database,
        "BEGIN EXCLUSIVE;DELETE FROM ovf_rollback_meta;DELETE FROM ovf_rollback_values;"
        "DELETE FROM ovf_rollback_blobs;INSERT INTO ovf_rollback_values SELECT * FROM ovf_values;"
        "INSERT INTO ovf_rollback_blobs SELECT key,value FROM ovf_blobs;");
  }
  sqlite3_stmt* statement{};
  if (status == SQLITE_OK) {
    status = sqlite3_prepare_v2(database, "INSERT INTO ovf_rollback_meta VALUES(?1,?2)", -1,
                                &statement, nullptr);
    if (status == SQLITE_OK) {
      sqlite3_bind_text64(statement, 1, item.store->schema_id.data(), item.store->schema_id.size(),
                          SQLITE_STATIC, SQLITE_UTF8);
      sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(item.store->schema_version));
      status = sqlite3_step(statement) == SQLITE_DONE ? SQLITE_OK : sqlite3_errcode(database);
    }
    sqlite3_finalize(statement);
    statement = nullptr;
  }
  if (status == SQLITE_OK) {
    status = Execute(database, "DELETE FROM ovf_values;INSERT INTO ovf_values SELECT * FROM "
                               "ovf_migration_values;");
  }
  if (status == SQLITE_OK) {
    status = sqlite3_prepare_v2(
        database,
        "UPDATE ovf_meta SET generation=generation+1,schema_id=?1,schema_version=?2,"
        "successful_commits=successful_commits+1,recovery_count=recovery_count+1,"
        "recovery_state=5 WHERE id=1 AND generation=?3",
        -1, &statement, nullptr);
    if (status == SQLITE_OK) {
      sqlite3_bind_text64(statement, 1, item.target_schema_id.data(), item.target_schema_id.size(),
                          SQLITE_STATIC, SQLITE_UTF8);
      sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(item.target_schema_version));
      sqlite3_bind_int64(statement, 3, static_cast<sqlite3_int64>(item.source_generation));
      status = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(database) == 1
                   ? SQLITE_OK
                   : SQLITE_CONSTRAINT;
    }
    sqlite3_finalize(statement);
  }
  if (status == SQLITE_OK) {
    status =
        Execute(database, "DELETE FROM ovf_migration_values;DELETE FROM ovf_migration;COMMIT;");
  }
  if (status != SQLITE_OK) {
    Execute(database, "ROLLBACK;");
    const auto result = status == SQLITE_CONSTRAINT
                            ? OVF_PER_STATUS_CONFLICT
                            : Fail(self, database, status, "cannot activate SQLite migration");
    sqlite3_close(database);
    return result;
  }
  sqlite3_close(database);
  item.store->schema_id = item.target_schema_id;
  item.store->schema_version = item.target_schema_version;
  *output = {sizeof(*output), item.source_generation + 1, item.durability};
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 AbortMigration(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle) {
  auto& self = *Self(backend);
  std::scoped_lock lock(self.mutex);
  return self.migrations.contains(handle) ? OVF_PER_STATUS_OK : OVF_PER_STATUS_NOT_FOUND;
}

ovf_per_status_v1 CloseMigration(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle) {
  auto& self = *Self(backend);
  std::scoped_lock lock(self.mutex);
  return self.migrations.erase(handle) == 1 ? OVF_PER_STATUS_OK : OVF_PER_STATUS_NOT_FOUND;
}

ovf_per_status_v1 RollbackStore(ovf_per_backend_v1* backend, ovf_per_handle_v1 store_handle,
                                ovf_per_durability_v1 durability,
                                ovf_per_commit_result_v1* output) {
  auto& self = *Self(backend);
  if (output == nullptr || output->struct_size < sizeof(*output)) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto handle = self.store_handles.find(store_handle);
  if (handle == self.store_handles.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  auto& store = *handle->second.store;
  if (handle->second.access != OVF_PER_ACCESS_READ_WRITE || durability < store.minimum_durability) {
    return OVF_PER_STATUS_PERMISSION_DENIED;
  }
  sqlite3* database{};
  int status = OpenDatabase(store, self.busy_timeout_ms, &database);
  if (status == SQLITE_OK) {
    status = Execute(database, "BEGIN EXCLUSIVE;");
  }
  sqlite3_stmt* statement{};
  std::string schema_id;
  std::uint64_t schema_version{};
  if (status == SQLITE_OK) {
    status = sqlite3_prepare_v2(database, "SELECT schema_id,schema_version FROM ovf_rollback_meta",
                                -1, &statement, nullptr);
  }
  if (status == SQLITE_OK) {
    status = sqlite3_step(statement);
  }
  if (status == SQLITE_ROW) {
    schema_id.assign(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)),
                     static_cast<std::size_t>(sqlite3_column_bytes(statement, 0)));
    schema_version = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 1));
    status = SQLITE_OK;
  } else if (status == SQLITE_DONE) {
    status = SQLITE_NOTFOUND;
  }
  sqlite3_finalize(statement);
  if (status == SQLITE_OK) {
    status = Execute(database,
                     "DELETE FROM ovf_values;INSERT INTO ovf_values SELECT * FROM "
                     "ovf_rollback_values;DELETE FROM ovf_blobs;INSERT INTO ovf_blobs(key,value) "
                     "SELECT key,value FROM ovf_rollback_blobs;");
  }
  if (status == SQLITE_OK) {
    status = sqlite3_prepare_v2(
        database,
        "UPDATE ovf_meta SET generation=generation+1,schema_id=?1,schema_version=?2,"
        "successful_commits=successful_commits+1,recovery_count=recovery_count+1,"
        "recovery_state=6 WHERE id=1",
        -1, &statement, nullptr);
    if (status == SQLITE_OK) {
      sqlite3_bind_text64(statement, 1, schema_id.data(), schema_id.size(), SQLITE_STATIC,
                          SQLITE_UTF8);
      sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(schema_version));
      status = sqlite3_step(statement) == SQLITE_DONE ? SQLITE_OK : sqlite3_errcode(database);
    }
    sqlite3_finalize(statement);
  }
  std::uint64_t generation{};
  if (status == SQLITE_OK) {
    status = Execute(database, "DELETE FROM ovf_rollback_values;DELETE FROM ovf_rollback_blobs;"
                               "DELETE FROM ovf_rollback_meta;COMMIT;");
  }
  if (status == SQLITE_OK) {
    StoreData expected = store;
    expected.schema_id = schema_id;
    expected.schema_version = schema_version;
    if (!ReadMetadata(database, expected, &generation)) {
      status = SQLITE_CORRUPT;
    }
  }
  if (status != SQLITE_OK) {
    Execute(database, "ROLLBACK;");
    const auto result = status == SQLITE_NOTFOUND
                            ? OVF_PER_STATUS_NOT_FOUND
                            : Fail(self, database, status, "cannot roll back SQLite migration");
    sqlite3_close(database);
    return result;
  }
  sqlite3_close(database);
  store.schema_id = std::move(schema_id);
  store.schema_version = schema_version;
  *output = {sizeof(*output), generation, durability};
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 LastError(ovf_per_backend_v1* backend, ovf_per_mutable_bytes_v1* output) {
  auto& self = *Self(backend);
  if (output == nullptr) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  if (output->data == nullptr || output->size < self.error.size()) {
    output->size = self.error.size();
    return OVF_PER_STATUS_BUFFER_TOO_SMALL;
  }
  std::memcpy(output->data, self.error.data(), self.error.size());
  output->size = self.error.size();
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 Create(const ovf_per_backend_config_v1* config, ovf_per_backend_v1** output) {
  if (config == nullptr || config->struct_size < sizeof(*config) || output == nullptr ||
      config->max_stores == 0 || config->max_transactions == 0) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  try {
    auto backend = std::make_unique<SqliteBackend>();
    backend->max_stores = config->max_stores;
    backend->max_transactions = config->max_transactions;
    if (!ParseConfiguration(config->configuration, *backend)) {
      return OVF_PER_STATUS_INVALID_ARGUMENT;
    }
    backend->abi = {sizeof(ovf_per_backend_v1),
                    OVF_PER_BACKEND_ABI_VERSION_1,
                    backend.get(),
                    Start,
                    Stop,
                    Capabilities,
                    OpenStore,
                    CloseStore,
                    BeginRead,
                    BeginWrite,
                    Get,
                    Put,
                    Erase,
                    Commit,
                    Abort,
                    CloseTransaction,
                    OpenBlobRead,
                    BeginBlobReplace,
                    ReadBlob,
                    WriteBlob,
                    CommitBlob,
                    AbortBlob,
                    CloseBlob,
                    BeginWriteAt,
                    OpenCursor,
                    NextCursor,
                    CloseCursor,
                    ResetStore,
                    StoreStatus,
                    BeginMigration,
                    ApplyMigration,
                    CommitMigration,
                    AbortMigration,
                    CloseMigration,
                    RollbackStore,
                    LastError};
    *output = &backend.release()->abi;
    return OVF_PER_STATUS_OK;
  } catch (...) {
    return OVF_PER_STATUS_RESOURCE_EXHAUSTED;
  }
}

void Destroy(ovf_per_backend_v1* backend) { delete Self(backend); }

const ovf_per_backend_factory_v1 kFactory{sizeof(ovf_per_backend_factory_v1),
                                          OVF_PER_BACKEND_ABI_VERSION_1,
                                          {"sqlite", 6},
                                          Create,
                                          Destroy};

} // namespace

extern "C" const ovf_per_backend_factory_v1* ovf_per_backend_query_v1(void) { return &kFactory; }
