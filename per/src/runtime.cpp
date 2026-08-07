// SPDX-License-Identifier: Apache-2.0

#include "ovf/per/per.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace ovf::per {
namespace {

ErrorCode ToErrorCode(ovf_per_status_v1 status) noexcept {
  switch (status) {
  case OVF_PER_STATUS_INVALID_ARGUMENT:
  case OVF_PER_STATUS_BUFFER_TOO_SMALL:
    return ErrorCode::invalid_argument;
  case OVF_PER_STATUS_INCOMPATIBLE_ABI:
    return ErrorCode::incompatible_abi;
  case OVF_PER_STATUS_INVALID_STATE:
    return ErrorCode::invalid_state;
  case OVF_PER_STATUS_NOT_FOUND:
    return ErrorCode::not_found;
  case OVF_PER_STATUS_PERMISSION_DENIED:
    return ErrorCode::permission_denied;
  case OVF_PER_STATUS_UNSUPPORTED:
    return ErrorCode::unsupported;
  case OVF_PER_STATUS_RESOURCE_EXHAUSTED:
    return ErrorCode::resource_exhausted;
  case OVF_PER_STATUS_CONFLICT:
    return ErrorCode::conflict;
  case OVF_PER_STATUS_QUOTA_EXCEEDED:
    return ErrorCode::quota_exceeded;
  case OVF_PER_STATUS_CORRUPTED:
    return ErrorCode::corrupted;
  case OVF_PER_STATUS_IO_ERROR:
    return ErrorCode::io_error;
  case OVF_PER_STATUS_BUSY:
    return ErrorCode::busy;
  case OVF_PER_STATUS_SHUTTING_DOWN:
    return ErrorCode::shutting_down;
  case OVF_PER_STATUS_BACKEND_ERROR:
  case OVF_PER_STATUS_OK:
    return ErrorCode::backend_failure;
  }
  return ErrorCode::backend_failure;
}

ovf_per_durability_v1 ToAbi(Durability value) noexcept {
  switch (value) {
  case Durability::buffered:
    return OVF_PER_DURABILITY_BUFFERED;
  case Durability::process_crash:
    return OVF_PER_DURABILITY_PROCESS_CRASH;
  case Durability::media:
    return OVF_PER_DURABILITY_MEDIA;
  }
  return OVF_PER_DURABILITY_BUFFERED;
}

Durability FromAbi(ovf_per_durability_v1 value) noexcept {
  switch (value) {
  case OVF_PER_DURABILITY_BUFFERED:
    return Durability::buffered;
  case OVF_PER_DURABILITY_PROCESS_CRASH:
    return Durability::process_crash;
  case OVF_PER_DURABILITY_MEDIA:
    return Durability::media;
  }
  return Durability::buffered;
}

RecoveryState FromAbi(ovf_per_recovery_state_v1 value) noexcept {
  switch (value) {
  case OVF_PER_RECOVERY_CLEAN_V1:
    return RecoveryState::clean;
  case OVF_PER_RECOVERY_JOURNAL_REPLAYED_V1:
    return RecoveryState::journal_replayed;
  case OVF_PER_RECOVERY_FAILED_CLOSED_V1:
    return RecoveryState::failed_closed;
  case OVF_PER_RECOVERY_RESET_V1:
    return RecoveryState::reset;
  case OVF_PER_RECOVERY_MIGRATED_V1:
    return RecoveryState::migrated;
  case OVF_PER_RECOVERY_ROLLED_BACK_V1:
    return RecoveryState::rolled_back;
  }
  return RecoveryState::failed_closed;
}

ovf_per_bytes_view_v1 View(std::span<const std::byte> value) noexcept {
  return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

bool ValidProviderName(std::string_view provider) noexcept {
  return !provider.empty() &&
         provider.find_first_not_of(
             "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
             std::string_view::npos;
}

} // namespace

namespace detail {

class RuntimeState final {
public:
  RuntimeState(const ovf_per_backend_factory_v1& factory, ovf_per_backend_v1* backend,
               void* library) noexcept
      : factory_(factory), backend_(backend), library_(library) {}

  ~RuntimeState() {
    Stop();
    if (backend_ != nullptr) {
      factory_.destroy(backend_);
      backend_ = nullptr;
    }
#if defined(__unix__) || defined(__APPLE__)
    if (library_ != nullptr) {
      ::dlclose(library_);
    }
#endif
  }

  Error MakeError(ovf_per_status_v1 status) const noexcept {
    std::string message = "persistency provider operation failed";
    if (backend_ != nullptr && backend_->last_error != nullptr) {
      std::uint8_t storage[256]{};
      ovf_per_mutable_bytes_v1 output{storage, sizeof(storage)};
      if (backend_->last_error(backend_, &output) == OVF_PER_STATUS_OK &&
          output.size <= sizeof(storage)) {
        message.assign(reinterpret_cast<const char*>(storage), output.size);
      }
    }
    return {ToErrorCode(status), std::move(message)};
  }

  void Stop() noexcept {
    std::scoped_lock lock(mutex_);
    if (running_ && backend_ != nullptr) {
      backend_->stop(backend_);
      running_ = false;
    }
  }

  ovf_per_backend_factory_v1 factory_{};
  ovf_per_backend_v1* backend_{};
  void* library_{};
  mutable std::mutex mutex_;
  bool running_{true};
};

} // namespace detail

namespace {

Result<std::optional<std::vector<std::byte>>>
GetValue(const std::shared_ptr<detail::RuntimeState>& state, ovf_per_handle_v1 handle,
         std::span<const std::byte> key) noexcept {
  if (state == nullptr || handle == OVF_PER_INVALID_HANDLE_V1 || key.empty()) {
    return Error{ErrorCode::invalid_argument, "transaction or key is invalid"};
  }
  std::scoped_lock lock(state->mutex_);
  if (!state->running_) {
    return Error{ErrorCode::shutting_down, "persistency runtime is stopped"};
  }
  try {
    ovf_per_mutable_bytes_v1 output{nullptr, 0};
    auto status = state->backend_->transaction_get(state->backend_, handle, View(key), &output);
    if (status == OVF_PER_STATUS_NOT_FOUND) {
      return std::optional<std::vector<std::byte>>{};
    }
    if (status != OVF_PER_STATUS_BUFFER_TOO_SMALL) {
      return state->MakeError(status);
    }
    if (output.size == 0) {
      return std::optional<std::vector<std::byte>>(std::vector<std::byte>{});
    }
    std::vector<std::byte> value(output.size);
    output.data = reinterpret_cast<std::uint8_t*>(value.data());
    status = state->backend_->transaction_get(state->backend_, handle, View(key), &output);
    if (status != OVF_PER_STATUS_OK || output.size > value.size()) {
      return status == OVF_PER_STATUS_OK
                 ? Error{ErrorCode::incompatible_abi, "provider returned an invalid value size"}
                 : state->MakeError(status);
    }
    value.resize(output.size);
    return std::optional<std::vector<std::byte>>(std::move(value));
  } catch (...) {
    return Error{ErrorCode::resource_exhausted, "cannot allocate persistency value"};
  }
}

void CloseTransaction(std::shared_ptr<detail::RuntimeState>& state,
                      ovf_per_handle_v1& handle) noexcept {
  if (state != nullptr && handle != OVF_PER_INVALID_HANDLE_V1) {
    std::scoped_lock lock(state->mutex_);
    if (state->running_) {
      static_cast<void>(state->backend_->transaction_close(state->backend_, handle));
    }
  }
  handle = OVF_PER_INVALID_HANDLE_V1;
  state.reset();
}

} // namespace

ReadTransaction::ReadTransaction(std::shared_ptr<detail::RuntimeState> state,
                                 ovf_per_handle_v1 handle, std::uint64_t generation) noexcept
    : state_(std::move(state)), handle_(handle), generation_(generation) {}
ReadTransaction::~ReadTransaction() { Close(); }
ReadTransaction::ReadTransaction(ReadTransaction&& other) noexcept
    : state_(std::move(other.state_)), handle_(std::exchange(other.handle_, 0)),
      generation_(other.generation_) {}
ReadTransaction& ReadTransaction::operator=(ReadTransaction&& other) noexcept {
  if (this != &other) {
    Close();
    state_ = std::move(other.state_);
    handle_ = std::exchange(other.handle_, 0);
    generation_ = other.generation_;
  }
  return *this;
}
bool ReadTransaction::valid() const noexcept {
  return state_ != nullptr && handle_ != OVF_PER_INVALID_HANDLE_V1;
}
std::uint64_t ReadTransaction::generation() const noexcept { return generation_; }
Result<std::optional<std::vector<std::byte>>>
ReadTransaction::Get(std::span<const std::byte> key) const noexcept {
  return GetValue(state_, handle_, key);
}
void ReadTransaction::Close() noexcept { CloseTransaction(state_, handle_); }

ReadTransaction::Cursor::Cursor(std::shared_ptr<detail::RuntimeState> state,
                                ovf_per_handle_v1 handle) noexcept
    : state_(std::move(state)), handle_(handle) {}
ReadTransaction::Cursor::~Cursor() { Close(); }
ReadTransaction::Cursor::Cursor(Cursor&& other) noexcept
    : state_(std::move(other.state_)), handle_(std::exchange(other.handle_, 0)) {}
ReadTransaction::Cursor& ReadTransaction::Cursor::operator=(Cursor&& other) noexcept {
  if (this != &other) {
    Close();
    state_ = std::move(other.state_);
    handle_ = std::exchange(other.handle_, 0);
  }
  return *this;
}
bool ReadTransaction::Cursor::valid() const noexcept {
  return state_ != nullptr && handle_ != OVF_PER_INVALID_HANDLE_V1;
}
Result<std::optional<Entry>> ReadTransaction::Cursor::Next() noexcept {
  if (!valid()) {
    return Error{ErrorCode::invalid_state, "cursor is not open"};
  }
  std::scoped_lock lock(state_->mutex_);
  try {
    ovf_per_mutable_bytes_v1 key{nullptr, 0};
    ovf_per_mutable_bytes_v1 value{nullptr, 0};
    auto status = state_->backend_->cursor_next(state_->backend_, handle_, &key, &value);
    if (status == OVF_PER_STATUS_NOT_FOUND) {
      return std::optional<Entry>{};
    }
    if (status != OVF_PER_STATUS_BUFFER_TOO_SMALL) {
      return state_->MakeError(status);
    }
    Entry entry{std::vector<std::byte>(key.size), std::vector<std::byte>(value.size)};
    key.data = reinterpret_cast<std::uint8_t*>(entry.key.data());
    value.data = reinterpret_cast<std::uint8_t*>(entry.value.data());
    status = state_->backend_->cursor_next(state_->backend_, handle_, &key, &value);
    if (status != OVF_PER_STATUS_OK || key.size > entry.key.size() ||
        value.size > entry.value.size()) {
      return status == OVF_PER_STATUS_OK
                 ? Error{ErrorCode::incompatible_abi, "provider returned invalid cursor sizes"}
                 : state_->MakeError(status);
    }
    entry.key.resize(key.size);
    entry.value.resize(value.size);
    return std::optional<Entry>(std::move(entry));
  } catch (...) {
    return Error{ErrorCode::resource_exhausted, "cannot allocate cursor entry"};
  }
}
void ReadTransaction::Cursor::Close() noexcept {
  if (valid()) {
    std::scoped_lock lock(state_->mutex_);
    if (state_->running_) {
      static_cast<void>(state_->backend_->cursor_close(state_->backend_, handle_));
    }
  }
  handle_ = OVF_PER_INVALID_HANDLE_V1;
  state_.reset();
}
Result<ReadTransaction::Cursor>
ReadTransaction::Iterate(std::span<const std::byte> prefix) const noexcept {
  if (!valid()) {
    return Error{ErrorCode::invalid_state, "read transaction is not active"};
  }
  std::scoped_lock lock(state_->mutex_);
  ovf_per_handle_v1 cursor{};
  const auto status =
      state_->backend_->cursor_open(state_->backend_, handle_, View(prefix), &cursor);
  return status == OVF_PER_STATUS_OK ? Result<Cursor>(Cursor(state_, cursor))
                                     : Result<Cursor>(state_->MakeError(status));
}

WriteTransaction::WriteTransaction(std::shared_ptr<detail::RuntimeState> state,
                                   ovf_per_handle_v1 handle, std::uint64_t generation) noexcept
    : state_(std::move(state)), handle_(handle), generation_(generation) {}
WriteTransaction::~WriteTransaction() { Abort(); }
WriteTransaction::WriteTransaction(WriteTransaction&& other) noexcept
    : state_(std::move(other.state_)), handle_(std::exchange(other.handle_, 0)),
      generation_(other.generation_) {}
WriteTransaction& WriteTransaction::operator=(WriteTransaction&& other) noexcept {
  if (this != &other) {
    Abort();
    state_ = std::move(other.state_);
    handle_ = std::exchange(other.handle_, 0);
    generation_ = other.generation_;
  }
  return *this;
}
bool WriteTransaction::valid() const noexcept {
  return state_ != nullptr && handle_ != OVF_PER_INVALID_HANDLE_V1;
}
std::uint64_t WriteTransaction::base_generation() const noexcept { return generation_; }
Result<std::optional<std::vector<std::byte>>>
WriteTransaction::Get(std::span<const std::byte> key) const noexcept {
  return GetValue(state_, handle_, key);
}
Result<bool> WriteTransaction::Put(std::span<const std::byte> key,
                                   std::span<const std::byte> value) noexcept {
  if (!valid() || key.empty()) {
    return Error{ErrorCode::invalid_argument, "transaction or key is invalid"};
  }
  std::scoped_lock lock(state_->mutex_);
  const auto status =
      state_->backend_->transaction_put(state_->backend_, handle_, View(key), View(value));
  return status == OVF_PER_STATUS_OK ? Result<bool>(true) : Result<bool>(state_->MakeError(status));
}
Result<bool> WriteTransaction::Erase(std::span<const std::byte> key) noexcept {
  if (!valid() || key.empty()) {
    return Error{ErrorCode::invalid_argument, "transaction or key is invalid"};
  }
  std::scoped_lock lock(state_->mutex_);
  std::uint8_t erased{};
  const auto status =
      state_->backend_->transaction_erase(state_->backend_, handle_, View(key), &erased);
  return status == OVF_PER_STATUS_OK ? Result<bool>(erased != 0)
                                     : Result<bool>(state_->MakeError(status));
}
Result<CommitInfo> WriteTransaction::Commit() noexcept {
  if (!valid()) {
    return Error{ErrorCode::invalid_state, "write transaction is not active"};
  }
  std::scoped_lock lock(state_->mutex_);
  ovf_per_commit_result_v1 result{sizeof(result), 0, OVF_PER_DURABILITY_BUFFERED};
  const auto status = state_->backend_->transaction_commit(state_->backend_, handle_, &result);
  if (status != OVF_PER_STATUS_OK) {
    return state_->MakeError(status);
  }
  static_cast<void>(state_->backend_->transaction_close(state_->backend_, handle_));
  handle_ = OVF_PER_INVALID_HANDLE_V1;
  state_.reset();
  return CommitInfo{result.generation, FromAbi(result.achieved_durability)};
}
void WriteTransaction::Abort() noexcept {
  if (valid()) {
    std::scoped_lock lock(state_->mutex_);
    if (state_->running_) {
      static_cast<void>(state_->backend_->transaction_abort(state_->backend_, handle_));
      static_cast<void>(state_->backend_->transaction_close(state_->backend_, handle_));
    }
  }
  handle_ = OVF_PER_INVALID_HANDLE_V1;
  state_.reset();
}

Store::Store(std::shared_ptr<detail::RuntimeState> state, ovf_per_handle_v1 handle) noexcept
    : state_(std::move(state)), handle_(handle) {}
Store::~Store() { Close(); }
Store::Store(Store&& other) noexcept
    : state_(std::move(other.state_)), handle_(std::exchange(other.handle_, 0)) {}
Store& Store::operator=(Store&& other) noexcept {
  if (this != &other) {
    Close();
    state_ = std::move(other.state_);
    handle_ = std::exchange(other.handle_, 0);
  }
  return *this;
}
bool Store::valid() const noexcept {
  return state_ != nullptr && handle_ != OVF_PER_INVALID_HANDLE_V1;
}
Result<ReadTransaction> Store::BeginRead() const noexcept {
  if (!valid()) {
    return Error{ErrorCode::invalid_state, "store is not open"};
  }
  std::scoped_lock lock(state_->mutex_);
  ovf_per_handle_v1 transaction{};
  std::uint64_t generation{};
  const auto status =
      state_->backend_->read_begin(state_->backend_, handle_, &transaction, &generation);
  return status == OVF_PER_STATUS_OK
             ? Result<ReadTransaction>(ReadTransaction(state_, transaction, generation))
             : Result<ReadTransaction>(state_->MakeError(status));
}
Result<WriteTransaction> Store::BeginWrite(Durability durability) const noexcept {
  if (!valid()) {
    return Error{ErrorCode::invalid_state, "store is not open"};
  }
  std::scoped_lock lock(state_->mutex_);
  ovf_per_handle_v1 transaction{};
  std::uint64_t generation{};
  const auto status = state_->backend_->write_begin(state_->backend_, handle_, ToAbi(durability),
                                                    &transaction, &generation);
  return status == OVF_PER_STATUS_OK
             ? Result<WriteTransaction>(WriteTransaction(state_, transaction, generation))
             : Result<WriteTransaction>(state_->MakeError(status));
}
Result<WriteTransaction> Store::BeginWriteAt(std::uint64_t expected_generation,
                                             Durability durability) const noexcept {
  if (!valid()) {
    return Error{ErrorCode::invalid_state, "store is not open"};
  }
  std::scoped_lock lock(state_->mutex_);
  ovf_per_handle_v1 transaction{};
  std::uint64_t generation{};
  const auto status = state_->backend_->write_begin_at(
      state_->backend_, handle_, ToAbi(durability), expected_generation, &transaction, &generation);
  return status == OVF_PER_STATUS_OK
             ? Result<WriteTransaction>(WriteTransaction(state_, transaction, generation))
             : Result<WriteTransaction>(state_->MakeError(status));
}
Result<CommitInfo> Store::Reset(std::span<const Entry> initial_data,
                                Durability durability) const noexcept {
  if (!valid()) {
    return Error{ErrorCode::invalid_state, "store is not open"};
  }
  try {
    std::vector<ovf_per_entry_v1> entries;
    entries.reserve(initial_data.size());
    for (const auto& entry : initial_data) {
      if (entry.key.empty()) {
        return Error{ErrorCode::invalid_argument, "initial-data key is empty"};
      }
      entries.push_back({View(entry.key), View(entry.value)});
    }
    std::scoped_lock lock(state_->mutex_);
    ovf_per_commit_result_v1 result{sizeof(result), 0, OVF_PER_DURABILITY_BUFFERED};
    const auto status = state_->backend_->store_reset(state_->backend_, handle_, entries.data(),
                                                      entries.size(), ToAbi(durability), &result);
    return status == OVF_PER_STATUS_OK
               ? Result<CommitInfo>(
                     CommitInfo{result.generation, FromAbi(result.achieved_durability)})
               : Result<CommitInfo>(state_->MakeError(status));
  } catch (...) {
    return Error{ErrorCode::resource_exhausted, "cannot allocate reset entries"};
  }
}
Result<StoreStatus> Store::GetStatus() const noexcept {
  if (!valid()) {
    return Error{ErrorCode::invalid_state, "store is not open"};
  }
  std::scoped_lock lock(state_->mutex_);
  ovf_per_store_status_v1 status_value{};
  status_value.struct_size = sizeof(status_value);
  const auto status = state_->backend_->store_status(state_->backend_, handle_, &status_value);
  return status == OVF_PER_STATUS_OK
             ? Result<StoreStatus>(StoreStatus{
                   status_value.generation, status_value.schema_version,
                   FromAbi(status_value.recovery_state), status_value.successful_commits,
                   status_value.rejected_operations, status_value.recovery_count})
             : Result<StoreStatus>(state_->MakeError(status));
}
Result<MigrationInfo> Store::Migrate(const MigrationPlan& plan,
                                     const MigrationTransform& transform) const noexcept {
  if (!valid() || plan.migration_id.empty() || plan.source_schema_id.empty() ||
      plan.target_schema_id.empty() || plan.source_schema_version == 0 ||
      plan.target_schema_version == 0 ||
      (!plan.allow_downgrade && plan.target_schema_version <= plan.source_schema_version) ||
      !transform) {
    return Error{ErrorCode::invalid_argument, "migration plan is invalid"};
  }
  ovf_per_handle_v1 migration{};
  ovf_per_migration_status_v1 migration_status{};
  migration_status.struct_size = sizeof(migration_status);
  std::array<std::byte, 4096> checkpoint_storage{};
  ovf_per_mutable_bytes_v1 checkpoint{reinterpret_cast<std::uint8_t*>(checkpoint_storage.data()),
                                      checkpoint_storage.size()};
  const ovf_per_migration_descriptor_v1 descriptor{
      sizeof(descriptor),
      {plan.migration_id.data(), plan.migration_id.size()},
      {plan.source_schema_id.data(), plan.source_schema_id.size()},
      plan.source_schema_version,
      {plan.target_schema_id.data(), plan.target_schema_id.size()},
      plan.target_schema_version,
      ToAbi(plan.durability),
      static_cast<std::uint8_t>(plan.allow_downgrade),
      {}};
  {
    std::scoped_lock lock(state_->mutex_);
    const auto status = state_->backend_->migration_begin(
        state_->backend_, handle_, &descriptor, &migration, &migration_status, &checkpoint);
    if (status != OVF_PER_STATUS_OK) {
      return state_->MakeError(status);
    }
  }
  auto close_migration = [&]() noexcept {
    std::scoped_lock lock(state_->mutex_);
    static_cast<void>(state_->backend_->migration_abort(state_->backend_, migration));
    static_cast<void>(state_->backend_->migration_close(state_->backend_, migration));
    migration = OVF_PER_INVALID_HANDLE_V1;
  };
  try {
    std::uint64_t processed_entries = migration_status.processed_entries;
    auto read_result = BeginRead();
    if (!read_result) {
      close_migration();
      return read_result.error();
    }
    auto read = std::move(read_result).value();
    auto cursor_result = read.Iterate();
    if (!cursor_result) {
      close_migration();
      return cursor_result.error();
    }
    auto cursor = std::move(cursor_result).value();
    const auto checkpoint_bytes = std::span(checkpoint_storage).first(checkpoint.size);
    while (true) {
      auto next = cursor.Next();
      if (!next) {
        close_migration();
        return next.error();
      }
      if (!next.value()) {
        break;
      }
      Entry source = std::move(*next.value());
      if (!checkpoint_bytes.empty() &&
          !std::lexicographical_compare(checkpoint_bytes.begin(), checkpoint_bytes.end(),
                                        source.key.begin(), source.key.end())) {
        continue;
      }
      auto transformed = transform(source);
      if (!transformed) {
        close_migration();
        return transformed.error();
      }
      ovf_per_entry_v1 target{};
      const ovf_per_entry_v1* target_pointer{};
      if (transformed.value()) {
        target = {View(transformed.value()->key), View(transformed.value()->value)};
        target_pointer = &target;
      }
      std::scoped_lock lock(state_->mutex_);
      const auto status = state_->backend_->migration_apply(
          state_->backend_, migration, View(source.key), target_pointer,
          transformed.value().has_value() ? 1 : 0);
      if (status != OVF_PER_STATUS_OK) {
        const auto error = state_->MakeError(status);
        static_cast<void>(state_->backend_->migration_abort(state_->backend_, migration));
        static_cast<void>(state_->backend_->migration_close(state_->backend_, migration));
        migration = OVF_PER_INVALID_HANDLE_V1;
        return error;
      }
      ++processed_entries;
    }
    cursor.Close();
    read.Close();
    std::scoped_lock lock(state_->mutex_);
    ovf_per_commit_result_v1 result{};
    result.struct_size = sizeof(result);
    const auto status = state_->backend_->migration_commit(state_->backend_, migration, &result);
    if (status != OVF_PER_STATUS_OK) {
      const auto error = state_->MakeError(status);
      static_cast<void>(state_->backend_->migration_close(state_->backend_, migration));
      return error;
    }
    static_cast<void>(state_->backend_->migration_close(state_->backend_, migration));
    return MigrationInfo{{result.generation, FromAbi(result.achieved_durability)},
                         processed_entries,
                         migration_status.resumed != 0};
  } catch (...) {
    close_migration();
    return Error{ErrorCode::backend_failure, "migration transform raised an exception"};
  }
}
Result<CommitInfo> Store::Rollback(Durability durability) const noexcept {
  if (!valid()) {
    return Error{ErrorCode::invalid_state, "store is not open"};
  }
  std::scoped_lock lock(state_->mutex_);
  ovf_per_commit_result_v1 result{};
  result.struct_size = sizeof(result);
  const auto status =
      state_->backend_->store_rollback(state_->backend_, handle_, ToAbi(durability), &result);
  return status == OVF_PER_STATUS_OK ? Result<CommitInfo>(CommitInfo{
                                           result.generation, FromAbi(result.achieved_durability)})
                                     : Result<CommitInfo>(state_->MakeError(status));
}
void Store::Close() noexcept {
  if (valid()) {
    std::scoped_lock lock(state_->mutex_);
    if (state_->running_) {
      static_cast<void>(state_->backend_->store_close(state_->backend_, handle_));
    }
  }
  handle_ = OVF_PER_INVALID_HANDLE_V1;
  state_.reset();
}

Runtime::Runtime(std::shared_ptr<detail::RuntimeState> state) noexcept : state_(std::move(state)) {}
Runtime::~Runtime() { Stop(); }

Result<std::unique_ptr<Runtime>> Runtime::Open(OpenOptions options) noexcept {
  if (options.factory != nullptr) {
    return Create(*options.factory, std::move(options.config));
  }
  if (!options.provider.empty()) {
    if (!options.provider_directory.empty()) {
      return LoadFrom(options.provider, options.provider_directory, std::move(options.config));
    }
    return Load(options.provider, std::move(options.config));
  }
  return Error{ErrorCode::invalid_argument,
               "Runtime::Open requires either a factory or a provider name"};
}

Result<std::unique_ptr<Runtime>> Runtime::Create(const ovf_per_backend_factory_v1& factory,
                                                 RuntimeConfig config) noexcept {
  ovf_per_backend_v1* backend{};
  try {
    constexpr auto factory_size =
        offsetof(ovf_per_backend_factory_v1, destroy) + sizeof(factory.destroy);
    if (factory.struct_size < factory_size ||
        factory.abi_version != OVF_PER_BACKEND_ABI_VERSION_1 || factory.create == nullptr ||
        factory.destroy == nullptr || factory.name.data == nullptr || factory.name.size == 0) {
      return Error{ErrorCode::incompatible_abi, "invalid persistency provider factory"};
    }
    const ovf_per_backend_config_v1 abi_config{
        sizeof(abi_config),
        {config.configuration.data(), config.configuration.size()},
        config.max_stores,
        config.max_transactions};
    const auto status = factory.create(&abi_config, &backend);
    if (status != OVF_PER_STATUS_OK || backend == nullptr) {
      return Error{ToErrorCode(status), "cannot create persistency provider"};
    }
    constexpr auto backend_size =
        offsetof(ovf_per_backend_v1, last_error) + sizeof(backend->last_error);
    const bool valid =
        backend->struct_size >= backend_size &&
        backend->abi_version == OVF_PER_BACKEND_ABI_VERSION_1 && backend->start != nullptr &&
        backend->stop != nullptr && backend->get_capabilities != nullptr &&
        backend->store_open != nullptr && backend->store_close != nullptr &&
        backend->read_begin != nullptr && backend->write_begin != nullptr &&
        backend->transaction_get != nullptr && backend->transaction_put != nullptr &&
        backend->transaction_erase != nullptr && backend->transaction_commit != nullptr &&
        backend->transaction_abort != nullptr && backend->transaction_close != nullptr &&
        backend->blob_read_open != nullptr && backend->blob_replace_begin != nullptr &&
        backend->blob_read != nullptr && backend->blob_write != nullptr &&
        backend->blob_commit != nullptr && backend->blob_abort != nullptr &&
        backend->blob_close != nullptr && backend->write_begin_at != nullptr &&
        backend->cursor_open != nullptr && backend->cursor_next != nullptr &&
        backend->cursor_close != nullptr && backend->store_reset != nullptr &&
        backend->store_status != nullptr && backend->migration_begin != nullptr &&
        backend->migration_apply != nullptr && backend->migration_commit != nullptr &&
        backend->migration_abort != nullptr && backend->migration_close != nullptr &&
        backend->store_rollback != nullptr && backend->last_error != nullptr;
    if (!valid) {
      factory.destroy(backend);
      return Error{ErrorCode::incompatible_abi, "persistency provider table is incomplete"};
    }
    if (backend->start(backend) != OVF_PER_STATUS_OK) {
      factory.destroy(backend);
      return Error{ErrorCode::backend_failure, "cannot start persistency provider"};
    }
    auto state = std::make_shared<detail::RuntimeState>(factory, backend, nullptr);
    return std::unique_ptr<Runtime>(new Runtime(std::move(state)));
  } catch (...) {
    if (backend != nullptr) {
      factory.destroy(backend);
    }
    return Error{ErrorCode::resource_exhausted, "cannot allocate persistency runtime"};
  }
}

Result<std::unique_ptr<Runtime>> Runtime::Load(std::string_view provider,
                                               RuntimeConfig config) noexcept {
  return LoadFrom(provider, {}, std::move(config));
}

Result<std::unique_ptr<Runtime>> Runtime::LoadFrom(std::string_view provider,
                                                   std::string_view provider_directory,
                                                   RuntimeConfig config) noexcept {
#if defined(__unix__) || defined(__APPLE__)
  if (!ValidProviderName(provider)) {
    return Error{ErrorCode::invalid_argument, "invalid persistency provider name"};
  }
#if defined(__APPLE__)
  const std::string filename = "libovf_per_provider_" + std::string(provider) + ".dylib";
#else
  const std::string filename = "libovf_per_provider_" + std::string(provider) + ".so";
#endif
  if (!provider_directory.empty() && provider_directory.front() != '/') {
    return Error{ErrorCode::invalid_argument, "provider directory must be absolute"};
  }
  std::vector<std::string> candidates;
  if (const char* search = std::getenv("OVF_PER_PROVIDER_PATH"); search != nullptr) {
    std::string paths(search);
    std::size_t offset{};
    while (offset <= paths.size()) {
      const auto end = paths.find(':', offset);
      const auto directory = paths.substr(offset, end - offset);
      candidates.push_back(directory.empty() ? filename : directory + "/" + filename);
      if (end == std::string::npos) {
        break;
      }
      offset = end + 1;
    }
  }
  if (!provider_directory.empty()) {
    candidates.push_back(std::string(provider_directory) + "/" + filename);
  }
  candidates.push_back(filename);
  void* library{};
  for (const auto& candidate : candidates) {
    library = ::dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library != nullptr) {
      break;
    }
  }
  if (library == nullptr) {
    return Error{ErrorCode::not_found, "cannot load persistency provider"};
  }
  const auto query = reinterpret_cast<ovf_per_backend_query_fn_v1>(
      ::dlsym(library, OVF_PER_BACKEND_QUERY_SYMBOL_V1));
  if (query == nullptr || query() == nullptr) {
    ::dlclose(library);
    return Error{ErrorCode::incompatible_abi, "persistency provider query is missing"};
  }
  auto created = Create(*query(), std::move(config));
  if (!created) {
    ::dlclose(library);
    return created.error();
  }
  created.value()->state_->library_ = library;
  return created;
#else
  static_cast<void>(provider);
  static_cast<void>(provider_directory);
  static_cast<void>(config);
  return Error{ErrorCode::unsupported, "dynamic providers are unsupported on this target"};
#endif
}

Result<Capabilities> Runtime::GetCapabilities() const noexcept {
  if (state_ == nullptr) {
    return Error{ErrorCode::invalid_state, "persistency runtime is stopped"};
  }
  std::scoped_lock lock(state_->mutex_);
  ovf_per_capabilities_v1 capabilities{};
  capabilities.struct_size = sizeof(capabilities);
  const auto status = state_->backend_->get_capabilities(state_->backend_, &capabilities);
  if (status != OVF_PER_STATUS_OK) {
    return state_->MakeError(status);
  }
  return Capabilities{capabilities.max_stores,
                      capabilities.max_transactions,
                      capabilities.max_store_bytes,
                      capabilities.max_key_size,
                      capabilities.max_value_size,
                      capabilities.max_blob_size,
                      FromAbi(capabilities.maximum_durability),
                      capabilities.persistent != 0,
                      capabilities.cross_process_leases != 0};
}

Result<Store> Runtime::OpenStore(StoreOptions options) const noexcept {
  if (state_ == nullptr || options.logical_name.empty() || options.capacity_bytes == 0 ||
      options.max_entries == 0 || options.max_key_size == 0 || options.max_value_size == 0 ||
      options.max_blob_size == 0 || options.schema_id.empty() || options.schema_version == 0) {
    return Error{ErrorCode::invalid_argument, "store options are invalid"};
  }
  std::scoped_lock lock(state_->mutex_);
  const ovf_per_store_descriptor_v1 descriptor{
      sizeof(descriptor),
      {options.logical_name.data(), options.logical_name.size()},
      options.access == Access::read_only ? OVF_PER_ACCESS_READ_ONLY : OVF_PER_ACCESS_READ_WRITE,
      ToAbi(options.minimum_durability),
      options.capacity_bytes,
      options.max_entries,
      options.max_key_size,
      options.max_value_size,
      options.max_blob_size,
      {options.schema_id.data(), options.schema_id.size()},
      options.schema_version};
  ovf_per_handle_v1 handle{};
  const auto status = state_->backend_->store_open(state_->backend_, &descriptor, &handle);
  return status == OVF_PER_STATUS_OK ? Result<Store>(Store(state_, handle))
                                     : Result<Store>(state_->MakeError(status));
}

Store::BlobReader::BlobReader(std::shared_ptr<detail::RuntimeState> state, ovf_per_handle_v1 handle,
                              std::uint64_t size, std::uint64_t generation) noexcept
    : state_(std::move(state)), handle_(handle), size_(size), generation_(generation) {}
Store::BlobReader::~BlobReader() { Close(); }
Store::BlobReader::BlobReader(BlobReader&& other) noexcept
    : state_(std::move(other.state_)), handle_(std::exchange(other.handle_, 0)), size_(other.size_),
      generation_(other.generation_) {}
Store::BlobReader& Store::BlobReader::operator=(BlobReader&& other) noexcept {
  if (this != &other) {
    Close();
    state_ = std::move(other.state_);
    handle_ = std::exchange(other.handle_, 0);
    size_ = other.size_;
    generation_ = other.generation_;
  }
  return *this;
}
bool Store::BlobReader::valid() const noexcept {
  return state_ != nullptr && handle_ != OVF_PER_INVALID_HANDLE_V1;
}
std::uint64_t Store::BlobReader::size() const noexcept { return size_; }
std::uint64_t Store::BlobReader::generation() const noexcept { return generation_; }
Result<std::size_t> Store::BlobReader::Read(std::uint64_t offset,
                                            std::span<std::byte> output) const noexcept {
  if (!valid() || offset > size_ || output.size() > size_ - offset) {
    return Error{ErrorCode::invalid_argument, "blob read is outside its declared size"};
  }
  if (output.empty()) {
    return std::size_t{0};
  }
  std::scoped_lock lock(state_->mutex_);
  ovf_per_mutable_bytes_v1 bytes{reinterpret_cast<std::uint8_t*>(output.data()), output.size()};
  const auto status = state_->backend_->blob_read(state_->backend_, handle_, offset, &bytes);
  return status == OVF_PER_STATUS_OK ? Result<std::size_t>(bytes.size)
                                     : Result<std::size_t>(state_->MakeError(status));
}
void Store::BlobReader::Close() noexcept {
  if (valid()) {
    std::scoped_lock lock(state_->mutex_);
    if (state_->running_) {
      static_cast<void>(state_->backend_->blob_close(state_->backend_, handle_));
    }
  }
  handle_ = OVF_PER_INVALID_HANDLE_V1;
  state_.reset();
}

Store::BlobWriter::BlobWriter(std::shared_ptr<detail::RuntimeState> state, ovf_per_handle_v1 handle,
                              std::uint64_t size, std::uint64_t position) noexcept
    : state_(std::move(state)), handle_(handle), size_(size), position_(position) {}
Store::BlobWriter::~BlobWriter() { Abort(); }
Store::BlobWriter::BlobWriter(BlobWriter&& other) noexcept
    : state_(std::move(other.state_)), handle_(std::exchange(other.handle_, 0)), size_(other.size_),
      position_(other.position_) {}
Store::BlobWriter& Store::BlobWriter::operator=(BlobWriter&& other) noexcept {
  if (this != &other) {
    Abort();
    state_ = std::move(other.state_);
    handle_ = std::exchange(other.handle_, 0);
    size_ = other.size_;
    position_ = other.position_;
  }
  return *this;
}
bool Store::BlobWriter::valid() const noexcept {
  return state_ != nullptr && handle_ != OVF_PER_INVALID_HANDLE_V1;
}
std::uint64_t Store::BlobWriter::size() const noexcept { return size_; }
std::uint64_t Store::BlobWriter::position() const noexcept { return position_; }
Result<std::size_t> Store::BlobWriter::Write(std::span<const std::byte> input) noexcept {
  if (!valid() || position_ > size_ || input.size() > size_ - position_) {
    return Error{ErrorCode::invalid_argument, "blob write exceeds its declared size"};
  }
  if (input.empty()) {
    return std::size_t{0};
  }
  std::scoped_lock lock(state_->mutex_);
  const auto status =
      state_->backend_->blob_write(state_->backend_, handle_, position_, View(input));
  if (status != OVF_PER_STATUS_OK) {
    return state_->MakeError(status);
  }
  position_ += input.size();
  return input.size();
}
Result<CommitInfo> Store::BlobWriter::Commit() noexcept {
  if (!valid() || position_ != size_) {
    return Error{ErrorCode::invalid_state, "blob replacement is incomplete"};
  }
  std::scoped_lock lock(state_->mutex_);
  ovf_per_commit_result_v1 result{};
  result.struct_size = sizeof(result);
  const auto status = state_->backend_->blob_commit(state_->backend_, handle_, &result);
  if (status != OVF_PER_STATUS_OK) {
    return state_->MakeError(status);
  }
  static_cast<void>(state_->backend_->blob_close(state_->backend_, handle_));
  handle_ = OVF_PER_INVALID_HANDLE_V1;
  state_.reset();
  return CommitInfo{result.generation, FromAbi(result.achieved_durability)};
}
void Store::BlobWriter::Abort() noexcept {
  if (valid()) {
    std::scoped_lock lock(state_->mutex_);
    if (state_->running_) {
      static_cast<void>(state_->backend_->blob_abort(state_->backend_, handle_));
      static_cast<void>(state_->backend_->blob_close(state_->backend_, handle_));
    }
  }
  handle_ = OVF_PER_INVALID_HANDLE_V1;
  state_.reset();
}

Result<Store::BlobReader> Store::OpenBlob(std::span<const std::byte> key) const noexcept {
  if (!valid() || key.empty()) {
    return Error{ErrorCode::invalid_argument, "store or blob key is invalid"};
  }
  std::scoped_lock lock(state_->mutex_);
  ovf_per_handle_v1 blob{};
  std::uint64_t size{};
  std::uint64_t generation{};
  const auto status = state_->backend_->blob_read_open(state_->backend_, handle_, View(key), &blob,
                                                       &size, &generation);
  return status == OVF_PER_STATUS_OK
             ? Result<BlobReader>(BlobReader(state_, blob, size, generation))
             : Result<BlobReader>(state_->MakeError(status));
}

Result<Store::BlobWriter> Store::BeginBlobReplace(std::span<const std::byte> key,
                                                  std::uint64_t size,
                                                  Durability durability) const noexcept {
  if (!valid() || key.empty()) {
    return Error{ErrorCode::invalid_argument, "store or blob key is invalid"};
  }
  std::scoped_lock lock(state_->mutex_);
  ovf_per_handle_v1 blob{};
  std::uint64_t generation{};
  const auto status = state_->backend_->blob_replace_begin(
      state_->backend_, handle_, View(key), size, ToAbi(durability), &blob, &generation);
  return status == OVF_PER_STATUS_OK ? Result<BlobWriter>(BlobWriter(state_, blob, size, 0))
                                     : Result<BlobWriter>(state_->MakeError(status));
}

void Runtime::Stop() noexcept {
  if (state_ != nullptr) {
    state_->Stop();
    state_.reset();
  }
}

} // namespace ovf::per
