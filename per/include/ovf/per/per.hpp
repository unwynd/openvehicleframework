// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/per/backend_abi.h"

#include <cstddef>
#include <cstdint>
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

template <typename T> class [[nodiscard]] Result final {
public:
  Result(T value) : value_(std::move(value)) {}
  Result(Error error) : value_(std::move(error)) {}
  [[nodiscard]] bool has_value() const noexcept { return std::holds_alternative<T>(value_); }
  explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] T& value() & { return std::get<T>(value_); }
  [[nodiscard]] const T& value() const& { return std::get<T>(value_); }
  [[nodiscard]] T&& value() && { return std::get<T>(std::move(value_)); }
  [[nodiscard]] const Error& error() const& { return std::get<Error>(value_); }

private:
  std::variant<T, Error> value_;
};

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

namespace detail {
class RuntimeState;
}

class ReadTransaction final {
public:
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
  void Close() noexcept;

private:
  friend class Store;
  ReadTransaction(std::shared_ptr<detail::RuntimeState>, ovf_per_handle_v1, std::uint64_t) noexcept;
  std::shared_ptr<detail::RuntimeState> state_;
  ovf_per_handle_v1 handle_{};
  std::uint64_t generation_{};
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

class Runtime final {
public:
  static Result<std::unique_ptr<Runtime>> Create(const ovf_per_backend_factory_v1& factory,
                                                 RuntimeConfig config = {}) noexcept;
  static Result<std::unique_ptr<Runtime>> Load(std::string_view provider,
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
