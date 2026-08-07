// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/core/result.hpp"
#include "ovf/per/backend_abi.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ovf::per {

enum class ErrorCode : std::uint8_t {
  invalid_argument,
  incompatible_abi,
  invalid_state,
  not_found,
  permission_denied,
  unsupported,
  resource_exhausted,
  conflict,
  quota_exceeded,
  corrupted,
  io_error,
  busy,
  shutting_down,
  backend_failure
};

struct Error final {
  ErrorCode code;
  std::string message;
};

template <typename T> using Result = ovf::core::Result<T, Error>;

enum class Durability : std::uint8_t { buffered, process_crash, media };
enum class Access : std::uint8_t { read_only, read_write };

struct RuntimeConfig final {
  std::string configuration;
  std::uint32_t max_stores{32};
  std::uint32_t max_transactions{128};
};

struct StoreOptions final {
  std::string logical_name;
  Access access{Access::read_write};
  Durability minimum_durability{Durability::process_crash};
  std::uint64_t capacity_bytes{1024U * 1024U};
  std::uint32_t max_entries{1024};
  std::uint32_t max_key_size{256};
  std::uint32_t max_value_size{64U * 1024U};
  std::uint64_t max_blob_size{16U * 1024U * 1024U};
  std::string schema_id{"dynamic"};
  std::uint64_t schema_version{1};
};

struct Capabilities final {
  std::uint32_t max_stores{};
  std::uint32_t max_transactions{};
  std::uint64_t max_store_bytes{};
  std::uint32_t max_key_size{};
  std::uint32_t max_value_size{};
  std::uint64_t max_blob_size{};
  Durability maximum_durability{};
  bool persistent{};
  bool cross_process_leases{};
};

struct CommitInfo final {
  std::uint64_t generation{};
  Durability achieved_durability{};
};

enum class RecoveryState : std::uint8_t {
  clean,
  journal_replayed,
  failed_closed,
  reset,
  migrated,
  rolled_back
};

struct StoreStatus final {
  std::uint64_t generation{};
  std::uint64_t schema_version{};
  RecoveryState recovery_state{};
  std::uint64_t successful_commits{};
  std::uint64_t rejected_operations{};
  std::uint64_t recovery_count{};
};

struct Entry final {
  std::vector<std::byte> key;
  std::vector<std::byte> value;
};

struct MigrationPlan final {
  std::string migration_id;
  std::string source_schema_id;
  std::uint64_t source_schema_version{};
  std::string target_schema_id;
  std::uint64_t target_schema_version{};
  Durability durability{Durability::process_crash};
  bool allow_downgrade{};
};

struct MigrationInfo final {
  CommitInfo commit;
  std::uint64_t processed_entries{};
  bool resumed{};
};

using MigrationTransform = std::function<Result<std::optional<Entry>>(const Entry& source)>;

namespace detail {
class RuntimeState;
}

class ReadTransaction final {
public:
  class Cursor;
  ReadTransaction() = default;
  ~ReadTransaction();
  ReadTransaction(ReadTransaction const&) = delete;
  ReadTransaction& operator=(ReadTransaction const&) = delete;
  ReadTransaction(ReadTransaction&&) noexcept;
  ReadTransaction& operator=(ReadTransaction&&) noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint64_t generation() const noexcept;
  [[nodiscard]] Result<std::optional<std::vector<std::byte>>>
  Get(std::span<const std::byte> key) const noexcept;
  [[nodiscard]] Result<Cursor> Iterate(std::span<const std::byte> prefix = {}) const noexcept;
  void Close() noexcept;

private:
  friend class Store;
  ReadTransaction(std::shared_ptr<detail::RuntimeState>, ovf_per_handle_v1, std::uint64_t) noexcept;
  std::shared_ptr<detail::RuntimeState> state_;
  ovf_per_handle_v1 handle_{};
  std::uint64_t generation_{};
};

class ReadTransaction::Cursor final {
public:
  Cursor() = default;
  ~Cursor();
  Cursor(Cursor const&) = delete;
  Cursor& operator=(Cursor const&) = delete;
  Cursor(Cursor&&) noexcept;
  Cursor& operator=(Cursor&&) noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Result<std::optional<Entry>> Next() noexcept;
  void Close() noexcept;

private:
  friend class ReadTransaction;
  Cursor(std::shared_ptr<detail::RuntimeState>, ovf_per_handle_v1) noexcept;
  std::shared_ptr<detail::RuntimeState> state_;
  ovf_per_handle_v1 handle_{};
};

class WriteTransaction final {
public:
  WriteTransaction() = default;
  ~WriteTransaction();
  WriteTransaction(WriteTransaction const&) = delete;
  WriteTransaction& operator=(WriteTransaction const&) = delete;
  WriteTransaction(WriteTransaction&&) noexcept;
  WriteTransaction& operator=(WriteTransaction&&) noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint64_t base_generation() const noexcept;
  [[nodiscard]] Result<std::optional<std::vector<std::byte>>>
  Get(std::span<const std::byte> key) const noexcept;
  [[nodiscard]] Result<bool> Put(std::span<const std::byte> key,
                                 std::span<const std::byte> value) noexcept;
  [[nodiscard]] Result<bool> Erase(std::span<const std::byte> key) noexcept;
  [[nodiscard]] Result<CommitInfo> Commit() noexcept;
  void Abort() noexcept;

private:
  friend class Store;
  WriteTransaction(std::shared_ptr<detail::RuntimeState>, ovf_per_handle_v1,
                   std::uint64_t) noexcept;
  std::shared_ptr<detail::RuntimeState> state_;
  ovf_per_handle_v1 handle_{};
  std::uint64_t generation_{};
};

class Store final {
public:
  Store() = default;
  ~Store();
  Store(Store const&) = delete;
  Store& operator=(Store const&) = delete;
  Store(Store&&) noexcept;
  Store& operator=(Store&&) noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Result<ReadTransaction> BeginRead() const noexcept;
  [[nodiscard]] Result<WriteTransaction>
  BeginWrite(Durability durability = Durability::process_crash) const noexcept;
  [[nodiscard]] Result<WriteTransaction>
  BeginWriteAt(std::uint64_t expected_generation,
               Durability durability = Durability::process_crash) const noexcept;

  // With() runs `body` inside a write transaction, commits on Ok, aborts on
  // any error returned by body or by Commit. This eliminates the common
  // BeginWrite -> Put -> Commit dance and its silent-drop failure mode when
  // Commit is forgotten. The body must return Result<void>.
  template <typename Body>
  [[nodiscard]] Result<CommitInfo> With(Durability durability, Body&& body) const noexcept {
    auto transaction = BeginWrite(durability);
    if (!transaction) {
      return transaction.error();
    }
    auto outcome = std::forward<Body>(body)(transaction.value());
    if (!outcome) {
      transaction.value().Abort();
      return outcome.error();
    }
    return transaction.value().Commit();
  }
  template <typename Body> [[nodiscard]] Result<CommitInfo> With(Body&& body) const noexcept {
    return With(Durability::process_crash, std::forward<Body>(body));
  }
  [[nodiscard]] Result<CommitInfo>
  Reset(std::span<const Entry> initial_data,
        Durability durability = Durability::process_crash) const noexcept;
  [[nodiscard]] Result<StoreStatus> GetStatus() const noexcept;
  [[nodiscard]] Result<MigrationInfo> Migrate(const MigrationPlan& plan,
                                              const MigrationTransform& transform) const noexcept;
  [[nodiscard]] Result<CommitInfo>
  Rollback(Durability durability = Durability::process_crash) const noexcept;
  class BlobReader;
  class BlobWriter;
  [[nodiscard]] Result<BlobReader> OpenBlob(std::span<const std::byte> key) const noexcept;
  [[nodiscard]] Result<BlobWriter>
  BeginBlobReplace(std::span<const std::byte> key, std::uint64_t size,
                   Durability durability = Durability::process_crash) const noexcept;
  void Close() noexcept;

private:
  friend class Runtime;
  Store(std::shared_ptr<detail::RuntimeState>, ovf_per_handle_v1) noexcept;
  std::shared_ptr<detail::RuntimeState> state_;
  ovf_per_handle_v1 handle_{};
};

class Store::BlobReader final {
public:
  BlobReader() = default;
  ~BlobReader();
  BlobReader(BlobReader const&) = delete;
  BlobReader& operator=(BlobReader const&) = delete;
  BlobReader(BlobReader&&) noexcept;
  BlobReader& operator=(BlobReader&&) noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint64_t size() const noexcept;
  [[nodiscard]] std::uint64_t generation() const noexcept;
  [[nodiscard]] Result<std::size_t> Read(std::uint64_t offset,
                                         std::span<std::byte> output) const noexcept;
  void Close() noexcept;

private:
  friend class Store;
  BlobReader(std::shared_ptr<detail::RuntimeState>, ovf_per_handle_v1, std::uint64_t,
             std::uint64_t) noexcept;
  std::shared_ptr<detail::RuntimeState> state_;
  ovf_per_handle_v1 handle_{};
  std::uint64_t size_{};
  std::uint64_t generation_{};
};

class Store::BlobWriter final {
public:
  BlobWriter() = default;
  ~BlobWriter();
  BlobWriter(BlobWriter const&) = delete;
  BlobWriter& operator=(BlobWriter const&) = delete;
  BlobWriter(BlobWriter&&) noexcept;
  BlobWriter& operator=(BlobWriter&&) noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint64_t size() const noexcept;
  [[nodiscard]] std::uint64_t position() const noexcept;
  [[nodiscard]] Result<std::size_t> Write(std::span<const std::byte> input) noexcept;
  [[nodiscard]] Result<CommitInfo> Commit() noexcept;
  void Abort() noexcept;

private:
  friend class Store;
  BlobWriter(std::shared_ptr<detail::RuntimeState>, ovf_per_handle_v1, std::uint64_t,
             std::uint64_t) noexcept;
  std::shared_ptr<detail::RuntimeState> state_;
  ovf_per_handle_v1 handle_{};
  std::uint64_t size_{};
  std::uint64_t position_{};
};

// Options for the unified Runtime::Open entry point. Set factory to construct
// with a linked-in provider; set provider (and optionally provider_directory)
// to dynamically load one.
struct OpenOptions final {
  RuntimeConfig config{};
  const ovf_per_backend_factory_v1* factory{nullptr};
  std::string_view provider{};
  std::string_view provider_directory{};
};

class Runtime final {
public:
  // Open is the preferred factory entry point; the older Create/Load/LoadFrom
  // below are retained as thin wrappers so existing call sites keep compiling.
  static Result<std::unique_ptr<Runtime>> Open(OpenOptions options) noexcept;

  static Result<std::unique_ptr<Runtime>> Create(const ovf_per_backend_factory_v1& factory,
                                                 RuntimeConfig config = {}) noexcept;
  static Result<std::unique_ptr<Runtime>> Load(std::string_view provider,
                                               RuntimeConfig config = {}) noexcept;
  static Result<std::unique_ptr<Runtime>> LoadFrom(std::string_view provider,
                                                   std::string_view provider_directory,
                                                   RuntimeConfig config = {}) noexcept;
  ~Runtime();
  Runtime(Runtime const&) = delete;
  Runtime& operator=(Runtime const&) = delete;
  [[nodiscard]] Result<Capabilities> GetCapabilities() const noexcept;
  [[nodiscard]] Result<Store> OpenStore(StoreOptions options) const noexcept;
  void Stop() noexcept;

private:
  explicit Runtime(std::shared_ptr<detail::RuntimeState>) noexcept;
  std::shared_ptr<detail::RuntimeState> state_;
};

} // namespace ovf::per
