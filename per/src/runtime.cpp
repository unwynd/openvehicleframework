// SPDX-License-Identifier: Apache-2.0

#include "ovf/per/per.hpp"

#include <cstddef>
#include <cstring>
#include <mutex>

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
        backend->last_error != nullptr;
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
#if defined(__unix__) || defined(__APPLE__)
  if (!ValidProviderName(provider)) {
    return Error{ErrorCode::invalid_argument, "invalid persistency provider name"};
  }
#if defined(__APPLE__)
  const std::string library_name = "libovf_per_provider_" + std::string(provider) + ".dylib";
#else
  const std::string library_name = "libovf_per_provider_" + std::string(provider) + ".so";
#endif
  void* library = ::dlopen(library_name.c_str(), RTLD_NOW | RTLD_LOCAL);
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
  return Capabilities{capabilities.max_stores,      capabilities.max_transactions,
                      capabilities.max_store_bytes, capabilities.max_key_size,
                      capabilities.max_value_size,  FromAbi(capabilities.maximum_durability),
                      capabilities.persistent != 0, capabilities.cross_process_leases != 0};
}

Result<Store> Runtime::OpenStore(StoreOptions options) const noexcept {
  if (state_ == nullptr || options.logical_name.empty() || options.capacity_bytes == 0 ||
      options.max_entries == 0 || options.max_key_size == 0 || options.max_value_size == 0) {
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
      options.max_value_size};
  ovf_per_handle_v1 handle{};
  const auto status = state_->backend_->store_open(state_->backend_, &descriptor, &handle);
  return status == OVF_PER_STATUS_OK ? Result<Store>(Store(state_, handle))
                                     : Result<Store>(state_->MakeError(status));
}

void Runtime::Stop() noexcept {
  if (state_ != nullptr) {
    state_->Stop();
    state_.reset();
  }
}

} // namespace ovf::per
