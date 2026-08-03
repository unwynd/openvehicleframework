// SPDX-License-Identifier: Apache-2.0

#include "ovf/crypto/crypto.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace ovf::crypto {
namespace {

ovf_crypto_bytes_view_v1 View(std::span<const std::byte> bytes) noexcept {
  return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

ErrorCode ToErrorCode(ovf_crypto_status_v1 status) noexcept {
  switch (status) {
  case OVF_CRYPTO_STATUS_INVALID_ARGUMENT:
  case OVF_CRYPTO_STATUS_BUFFER_TOO_SMALL:
    return ErrorCode::invalid_argument;
  case OVF_CRYPTO_STATUS_INCOMPATIBLE_ABI:
    return ErrorCode::incompatible_abi;
  case OVF_CRYPTO_STATUS_INVALID_STATE:
    return ErrorCode::invalid_state;
  case OVF_CRYPTO_STATUS_NOT_FOUND:
    return ErrorCode::not_found;
  case OVF_CRYPTO_STATUS_PERMISSION_DENIED:
    return ErrorCode::permission_denied;
  case OVF_CRYPTO_STATUS_UNSUPPORTED:
    return ErrorCode::unsupported;
  case OVF_CRYPTO_STATUS_RESOURCE_EXHAUSTED:
    return ErrorCode::resource_exhausted;
  case OVF_CRYPTO_STATUS_AUTHENTICATION_FAILED:
    return ErrorCode::authentication_failed;
  case OVF_CRYPTO_STATUS_ENTROPY_FAILURE:
    return ErrorCode::entropy_failure;
  case OVF_CRYPTO_STATUS_SHUTTING_DOWN:
    return ErrorCode::shutting_down;
  case OVF_CRYPTO_STATUS_CANCELLED:
    return ErrorCode::cancelled;
  case OVF_CRYPTO_STATUS_DEADLINE_EXCEEDED:
    return ErrorCode::deadline_exceeded;
  case OVF_CRYPTO_STATUS_BACKEND_ERROR:
  case OVF_CRYPTO_STATUS_OK:
    return ErrorCode::backend_failure;
  }
  return ErrorCode::backend_failure;
}

ovf_crypto_key_format_v1 ToAbi(KeyFormat format) noexcept {
  switch (format) {
  case KeyFormat::raw:
    return OVF_CRYPTO_KEY_FORMAT_RAW;
  case KeyFormat::der:
    return OVF_CRYPTO_KEY_FORMAT_DER;
  case KeyFormat::pem:
    return OVF_CRYPTO_KEY_FORMAT_PEM;
  }
  return OVF_CRYPTO_KEY_FORMAT_RAW;
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
  RuntimeState(const ovf_crypto_backend_factory_v1* factory, ovf_crypto_backend_v1* backend,
               ovf_crypto_async_extension_v1* async, void* library) noexcept
      : factory_(*factory), backend_(backend), async_(async), library_(library) {}

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

  RuntimeState(RuntimeState const&) = delete;
  RuntimeState& operator=(RuntimeState const&) = delete;

  Error MakeError(ovf_crypto_status_v1 status) const noexcept {
    std::string message = "cryptographic provider operation failed";
    if (backend_ != nullptr && backend_->last_error != nullptr) {
      std::uint8_t storage[256]{};
      ovf_crypto_mutable_bytes_v1 output{storage, sizeof(storage)};
      if (backend_->last_error(backend_, &output) == OVF_CRYPTO_STATUS_OK &&
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

  bool DestroyKey(ovf_crypto_handle_v1 handle) noexcept {
    std::scoped_lock lock(mutex_);
    return running_ && backend_->key_destroy(backend_, handle) == OVF_CRYPTO_STATUS_OK;
  }

  bool DestroyStream(ovf_crypto_handle_v1 handle) noexcept {
    std::scoped_lock lock(mutex_);
    return running_ && backend_->stream_destroy(backend_, handle) == OVF_CRYPTO_STATUS_OK;
  }

  ovf_crypto_backend_factory_v1 factory_{};
  ovf_crypto_backend_v1* backend_{};
  ovf_crypto_async_extension_v1* async_{};
  void* library_{};
  mutable std::mutex mutex_;
  bool running_{true};
};

} // namespace detail

Key::Key(std::shared_ptr<detail::RuntimeState> state, ovf_crypto_handle_v1 handle) noexcept
    : state_(std::move(state)), handle_(handle) {}

Key::~Key() {
  if (state_ != nullptr && handle_ != OVF_CRYPTO_INVALID_HANDLE_V1) {
    static_cast<void>(state_->DestroyKey(handle_));
  }
}

Key::Key(Key&& other) noexcept
    : state_(std::move(other.state_)), handle_(std::exchange(other.handle_, 0)) {}

Key& Key::operator=(Key&& other) noexcept {
  if (this != &other) {
    if (state_ != nullptr && handle_ != OVF_CRYPTO_INVALID_HANDLE_V1) {
      static_cast<void>(state_->DestroyKey(handle_));
    }
    state_ = std::move(other.state_);
    handle_ = std::exchange(other.handle_, 0);
  }
  return *this;
}

bool Key::valid() const noexcept {
  return state_ != nullptr && handle_ != OVF_CRYPTO_INVALID_HANDLE_V1;
}

InputStream::InputStream(std::shared_ptr<detail::RuntimeState> state, ovf_crypto_handle_v1 handle,
                         ovf_crypto_stream_operation_v1 operation) noexcept
    : state_(std::move(state)), handle_(handle), operation_(operation) {}

InputStream::~InputStream() { Cancel(); }

InputStream::InputStream(InputStream&& other) noexcept
    : state_(std::move(other.state_)), handle_(std::exchange(other.handle_, 0)),
      operation_(other.operation_) {}

InputStream& InputStream::operator=(InputStream&& other) noexcept {
  if (this != &other) {
    Cancel();
    state_ = std::move(other.state_);
    handle_ = std::exchange(other.handle_, 0);
    operation_ = other.operation_;
  }
  return *this;
}

bool InputStream::valid() const noexcept {
  return state_ != nullptr && handle_ != OVF_CRYPTO_INVALID_HANDLE_V1;
}

AeadRecordStream::AeadRecordStream(std::shared_ptr<detail::RuntimeState> state,
                                   ovf_crypto_handle_v1 handle) noexcept
    : state_(std::move(state)), handle_(handle) {}

AeadRecordStream::~AeadRecordStream() { Close(); }

AeadRecordStream::AeadRecordStream(AeadRecordStream&& other) noexcept
    : state_(std::move(other.state_)), handle_(std::exchange(other.handle_, 0)) {}

AeadRecordStream& AeadRecordStream::operator=(AeadRecordStream&& other) noexcept {
  if (this != &other) {
    Close();
    state_ = std::move(other.state_);
    handle_ = std::exchange(other.handle_, 0);
  }
  return *this;
}

bool AeadRecordStream::valid() const noexcept {
  return state_ != nullptr && handle_ != OVF_CRYPTO_INVALID_HANDLE_V1;
}

void AeadRecordStream::Close() noexcept {
  if (valid()) {
    static_cast<void>(state_->DestroyStream(handle_));
    handle_ = OVF_CRYPTO_INVALID_HANDLE_V1;
    state_.reset();
  }
}

class AsyncOperation::State final {
public:
  static void Complete(void* user_data, ovf_crypto_handle_v1,
                       const ovf_crypto_async_result_v1* result) noexcept {
    auto& self = *static_cast<State*>(user_data);
    std::scoped_lock lock(self.mutex);
    if (self.done) {
      return;
    }
    if (result == nullptr || result->struct_size < sizeof(*result)) {
      self.result = Error{ErrorCode::incompatible_abi, "provider returned an invalid async result"};
    } else if (result->status != OVF_CRYPTO_STATUS_OK) {
      self.result = Error{ToErrorCode(result->status), "asynchronous operation failed"};
    } else if (self.boolean_result) {
      self.result = AsyncValue(result->valid != 0);
    } else {
      try {
        std::vector<std::byte> bytes(result->bytes.size);
        if (!bytes.empty()) {
          std::memcpy(bytes.data(), result->bytes.data, bytes.size());
        }
        self.result = AsyncValue(std::move(bytes));
      } catch (...) {
        self.result = Error{ErrorCode::resource_exhausted, "cannot copy asynchronous result"};
      }
    }
    self.done = true;
    self.condition.notify_all();
  }

  std::shared_ptr<detail::RuntimeState> runtime;
  ovf_crypto_async_extension_v1* extension{};
  ovf_crypto_handle_v1 ticket{};
  bool boolean_result{};
  std::mutex mutex;
  std::condition_variable condition;
  bool done{};
  std::variant<std::monostate, AsyncValue, Error> result;
};

AsyncOperation::AsyncOperation(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}
AsyncOperation::~AsyncOperation() {
  if (state_ != nullptr) {
    static_cast<void>(Cancel());
    static_cast<void>(Wait());
  }
}
AsyncOperation::AsyncOperation(AsyncOperation&&) noexcept = default;
AsyncOperation& AsyncOperation::operator=(AsyncOperation&& other) noexcept {
  if (this != &other) {
    if (state_ != nullptr) {
      static_cast<void>(Cancel());
      static_cast<void>(Wait());
    }
    state_ = std::move(other.state_);
  }
  return *this;
}
bool AsyncOperation::valid() const noexcept { return state_ != nullptr; }
Result<AsyncValue> AsyncOperation::Wait() noexcept {
  if (state_ == nullptr) {
    return Error{ErrorCode::invalid_state, "asynchronous operation is not active"};
  }
  std::unique_lock lock(state_->mutex);
  state_->condition.wait(lock, [&] { return state_->done; });
  auto state = std::move(state_);
  if (std::holds_alternative<Error>(state->result)) {
    return std::get<Error>(std::move(state->result));
  }
  return std::get<AsyncValue>(std::move(state->result));
}
bool AsyncOperation::Cancel() noexcept {
  if (state_ == nullptr) {
    return false;
  }
  ovf_crypto_handle_v1 ticket{};
  ovf_crypto_async_extension_v1* extension{};
  {
    std::scoped_lock lock(state_->mutex);
    if (state_->done) {
      return false;
    }
    ticket = state_->ticket;
    extension = state_->extension;
  }
  return extension->cancel(extension, ticket) == OVF_CRYPTO_STATUS_OK;
}

Result<bool> InputStream::Update(std::span<const std::byte> input) noexcept {
  if (!valid()) {
    return Error{ErrorCode::invalid_state, "stream is not active"};
  }
  std::scoped_lock lock(state_->mutex_);
  const auto status = state_->backend_->stream_update(state_->backend_, handle_, View(input));
  return status == OVF_CRYPTO_STATUS_OK ? Result<bool>(true)
                                        : Result<bool>(state_->MakeError(status));
}

Result<std::vector<std::byte>> InputStream::Finish() noexcept {
  if (!valid() || operation_ == OVF_CRYPTO_STREAM_VERIFY) {
    return Error{ErrorCode::invalid_state, "stream cannot produce an output"};
  }
  std::scoped_lock lock(state_->mutex_);
  try {
    ovf_crypto_mutable_bytes_v1 output{nullptr, 0};
    auto status =
        state_->backend_->stream_finish(state_->backend_, handle_, {nullptr, 0}, &output, nullptr);
    if (status != OVF_CRYPTO_STATUS_BUFFER_TOO_SMALL || output.size > 64U * 1024U) {
      return status == OVF_CRYPTO_STATUS_BUFFER_TOO_SMALL
                 ? Error{ErrorCode::resource_exhausted, "stream output exceeds its limit"}
                 : state_->MakeError(status);
    }
    std::vector<std::byte> bytes(output.size);
    output.data = reinterpret_cast<std::uint8_t*>(bytes.data());
    status =
        state_->backend_->stream_finish(state_->backend_, handle_, {nullptr, 0}, &output, nullptr);
    if (status != OVF_CRYPTO_STATUS_OK || output.size > bytes.size()) {
      return status == OVF_CRYPTO_STATUS_OK
                 ? Error{ErrorCode::incompatible_abi, "provider returned an invalid stream output"}
                 : state_->MakeError(status);
    }
    bytes.resize(output.size);
    handle_ = OVF_CRYPTO_INVALID_HANDLE_V1;
    state_.reset();
    return bytes;
  } catch (...) {
    return Error{ErrorCode::resource_exhausted, "cannot allocate stream output"};
  }
}

Result<bool> InputStream::FinishVerify(std::span<const std::byte> signature) noexcept {
  if (!valid() || operation_ != OVF_CRYPTO_STREAM_VERIFY || signature.empty()) {
    return Error{ErrorCode::invalid_state, "stream is not a verifier"};
  }
  std::scoped_lock lock(state_->mutex_);
  std::uint8_t valid{};
  const auto status =
      state_->backend_->stream_finish(state_->backend_, handle_, View(signature), nullptr, &valid);
  if (status != OVF_CRYPTO_STATUS_OK) {
    return state_->MakeError(status);
  }
  handle_ = OVF_CRYPTO_INVALID_HANDLE_V1;
  state_.reset();
  return valid != 0;
}

void InputStream::Cancel() noexcept {
  if (valid()) {
    static_cast<void>(state_->DestroyStream(handle_));
    handle_ = OVF_CRYPTO_INVALID_HANDLE_V1;
    state_.reset();
  }
}

Runtime::Runtime(std::shared_ptr<detail::RuntimeState> state, void* library) noexcept
    : state_(std::move(state)), library_(library) {}

Runtime::~Runtime() {
  Stop();
  state_.reset();
  library_ = nullptr;
}

Result<std::unique_ptr<Runtime>> Runtime::Create(const ovf_crypto_backend_factory_v1& factory,
                                                 RuntimeConfig config) noexcept {
  ovf_crypto_backend_v1* backend{};
  try {
    constexpr auto required_factory_size =
        offsetof(ovf_crypto_backend_factory_v1, destroy) + sizeof(factory.destroy);
    if (factory.struct_size < required_factory_size ||
        factory.abi_version != OVF_CRYPTO_BACKEND_ABI_VERSION_1 || factory.create == nullptr ||
        factory.destroy == nullptr || factory.name.data == nullptr || factory.name.size == 0) {
      return Error{ErrorCode::incompatible_abi, "invalid cryptographic provider factory"};
    }
    ovf_crypto_backend_config_v1 abi_config{
        sizeof(ovf_crypto_backend_config_v1),
        {config.configuration.data(), config.configuration.size()},
        config.max_keys,
        config.max_contexts};
    ovf_crypto_host_api_v1 host{sizeof(ovf_crypto_host_api_v1), nullptr, nullptr, nullptr};
    const auto create_status = factory.create(&host, &abi_config, &backend);
    if (create_status != OVF_CRYPTO_STATUS_OK || backend == nullptr) {
      return Error{ToErrorCode(create_status), "cannot create cryptographic provider"};
    }
    constexpr auto required_backend_size =
        offsetof(ovf_crypto_backend_v1, last_error) + sizeof(backend->last_error);
    const bool valid =
        backend->struct_size >= required_backend_size &&
        backend->abi_version == OVF_CRYPTO_BACKEND_ABI_VERSION_1 && backend->start != nullptr &&
        backend->stop != nullptr && backend->get_capabilities != nullptr &&
        backend->random_bytes != nullptr && backend->key_import != nullptr &&
        backend->key_generate != nullptr && backend->key_destroy != nullptr &&
        backend->hash != nullptr && backend->mac != nullptr && backend->aead_encrypt != nullptr &&
        backend->aead_decrypt != nullptr && backend->sign != nullptr &&
        backend->verify != nullptr && backend->derive != nullptr &&
        backend->key_public_value != nullptr && backend->key_agree != nullptr &&
        backend->certificate_validate != nullptr && backend->stream_create != nullptr &&
        backend->stream_update != nullptr && backend->stream_finish != nullptr &&
        backend->stream_destroy != nullptr && backend->stream_process_record != nullptr &&
        backend->query_extension != nullptr && backend->last_error != nullptr;
    if (!valid) {
      factory.destroy(backend);
      return Error{ErrorCode::incompatible_abi, "incomplete cryptographic provider ABI"};
    }
    const auto start_status = backend->start(backend);
    if (start_status != OVF_CRYPTO_STATUS_OK) {
      factory.destroy(backend);
      return Error{ToErrorCode(start_status), "cannot start cryptographic provider"};
    }
    ovf_crypto_async_extension_v1* async{};
    void* extension{};
    const auto extension_status =
        backend->query_extension(backend, OVF_CRYPTO_EXTENSION_ASYNC_V1, 1, &extension);
    if (extension_status == OVF_CRYPTO_STATUS_OK) {
      async = static_cast<ovf_crypto_async_extension_v1*>(extension);
      constexpr auto required_async_size =
          offsetof(ovf_crypto_async_extension_v1, cancel) + sizeof(async->cancel);
      if (async == nullptr || async->struct_size < required_async_size || async->version != 1 ||
          async->max_outstanding == 0 || async->submit == nullptr || async->cancel == nullptr) {
        backend->stop(backend);
        factory.destroy(backend);
        return Error{ErrorCode::incompatible_abi, "invalid asynchronous provider extension"};
      }
    } else if (extension_status != OVF_CRYPTO_STATUS_UNSUPPORTED) {
      backend->stop(backend);
      factory.destroy(backend);
      return Error{ToErrorCode(extension_status), "cannot query provider extensions"};
    }
    auto state = std::make_shared<detail::RuntimeState>(&factory, backend, async, nullptr);
    backend = nullptr;
    return std::unique_ptr<Runtime>(new Runtime(std::move(state), nullptr));
  } catch (...) {
    if (backend != nullptr) {
      factory.destroy(backend);
    }
    return Error{ErrorCode::resource_exhausted, "cannot allocate cryptographic runtime"};
  }
}

Result<std::unique_ptr<Runtime>> Runtime::Load(std::string_view provider,
                                               RuntimeConfig config) noexcept {
#if defined(__unix__) || defined(__APPLE__)
  if (!ValidProviderName(provider)) {
    return Error{ErrorCode::invalid_argument, "invalid cryptographic provider name"};
  }
#if defined(__APPLE__)
  constexpr std::string_view suffix = ".dylib";
#else
  constexpr std::string_view suffix = ".so";
#endif
  try {
    const std::string filename =
        "libovf_crypto_provider_" + std::string(provider) + std::string(suffix);
    std::vector<std::string> candidates;
    if (const char* search = std::getenv("OVF_CRYPTO_PROVIDER_PATH"); search != nullptr) {
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
    candidates.push_back(filename);
    void* library{};
    for (const auto& candidate : candidates) {
      library = ::dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
      if (library != nullptr) {
        break;
      }
    }
    if (library == nullptr) {
      return Error{ErrorCode::not_found, "cryptographic provider library not found"};
    }
    ::dlerror();
    auto* symbol = ::dlsym(library, OVF_CRYPTO_BACKEND_QUERY_SYMBOL_V1);
    const char* symbol_error = ::dlerror();
    if (symbol == nullptr || symbol_error != nullptr) {
      ::dlclose(library);
      return Error{ErrorCode::incompatible_abi, "provider query symbol is missing"};
    }
    const auto query = reinterpret_cast<ovf_crypto_backend_query_fn_v1>(symbol);
    const auto* factory = query();
    if (factory == nullptr) {
      ::dlclose(library);
      return Error{ErrorCode::incompatible_abi, "provider returned no factory"};
    }
    auto result = Create(*factory, std::move(config));
    if (!result) {
      ::dlclose(library);
      return result.error();
    }
    auto runtime = std::move(result).value();
    runtime->state_->library_ = library;
    runtime->library_ = library;
    return runtime;
  } catch (...) {
    return Error{ErrorCode::resource_exhausted, "cannot load cryptographic provider"};
  }
#else
  static_cast<void>(provider);
  static_cast<void>(config);
  return Error{ErrorCode::unsupported, "dynamic providers are unsupported on this target"};
#endif
}

Result<Capabilities> Runtime::GetCapabilities() const noexcept {
  std::scoped_lock lock(state_->mutex_);
  ovf_crypto_capabilities_v1 capabilities{};
  capabilities.struct_size = sizeof(capabilities);
  const auto status = state_->backend_->get_capabilities(state_->backend_, &capabilities);
  if (status != OVF_CRYPTO_STATUS_OK) {
    return state_->MakeError(status);
  }
  if (capabilities.struct_size < sizeof(capabilities)) {
    return Error{ErrorCode::incompatible_abi, "provider returned truncated capabilities"};
  }
  if (capabilities.algorithm_count > std::size(capabilities.algorithms)) {
    return Error{ErrorCode::incompatible_abi, "provider returned too many algorithms"};
  }
  std::vector<Algorithm> algorithms;
  try {
    algorithms.reserve(capabilities.algorithm_count);
    for (std::uint32_t index = 0; index < capabilities.algorithm_count; ++index) {
      algorithms.push_back(static_cast<Algorithm>(capabilities.algorithms[index]));
    }
  } catch (...) {
    return Error{ErrorCode::resource_exhausted, "cannot allocate capability result"};
  }
  return Capabilities{std::move(algorithms),
                      capabilities.max_keys,
                      capabilities.max_contexts,
                      capabilities.max_input_size,
                      capabilities.supports_persistent_keys != 0,
                      capabilities.supports_hardware_keys != 0,
                      capabilities.supports_secure_memory != 0};
}

Result<std::vector<std::byte>> Runtime::Random(std::size_t size) const noexcept {
  constexpr std::size_t maximum_random_request = 64U * 1024U * 1024U;
  if (size > maximum_random_request) {
    return Error{ErrorCode::resource_exhausted, "random request exceeds the runtime limit"};
  }
  try {
    std::vector<std::byte> output(size);
    std::scoped_lock lock(state_->mutex_);
    const auto status = state_->backend_->random_bytes(
        state_->backend_, {reinterpret_cast<std::uint8_t*>(output.data()), output.size()});
    return status == OVF_CRYPTO_STATUS_OK
               ? Result<std::vector<std::byte>>(std::move(output))
               : Result<std::vector<std::byte>>(state_->MakeError(status));
  } catch (...) {
    return Error{ErrorCode::resource_exhausted, "cannot allocate random output"};
  }
}

Result<Key> Runtime::ImportKey(KeyPolicy policy, KeyFormat format,
                               std::span<const std::byte> material) const noexcept {
  ovf_crypto_key_descriptor_v1 descriptor{sizeof(descriptor),
                                          static_cast<std::uint32_t>(policy.algorithm),
                                          static_cast<std::uint32_t>(policy.usage),
                                          static_cast<std::uint8_t>(policy.exportable),
                                          static_cast<std::uint8_t>(policy.persistent),
                                          {}};
  ovf_crypto_handle_v1 handle{};
  std::scoped_lock lock(state_->mutex_);
  const auto status = state_->backend_->key_import(state_->backend_, &descriptor, ToAbi(format),
                                                   View(material), &handle);
  if (status != OVF_CRYPTO_STATUS_OK) {
    return state_->MakeError(status);
  }
  if (handle == OVF_CRYPTO_INVALID_HANDLE_V1) {
    return Error{ErrorCode::backend_failure, "provider returned an invalid key handle"};
  }
  return Key{state_, handle};
}

Result<Key> Runtime::GenerateKey(KeyPolicy policy) const noexcept {
  ovf_crypto_key_descriptor_v1 descriptor{sizeof(descriptor),
                                          static_cast<std::uint32_t>(policy.algorithm),
                                          static_cast<std::uint32_t>(policy.usage),
                                          static_cast<std::uint8_t>(policy.exportable),
                                          static_cast<std::uint8_t>(policy.persistent),
                                          {}};
  ovf_crypto_handle_v1 handle{};
  std::scoped_lock lock(state_->mutex_);
  const auto status = state_->backend_->key_generate(state_->backend_, &descriptor, &handle);
  if (status != OVF_CRYPTO_STATUS_OK) {
    return state_->MakeError(status);
  }
  if (handle == OVF_CRYPTO_INVALID_HANDLE_V1) {
    return Error{ErrorCode::backend_failure, "provider returned an invalid key handle"};
  }
  return Key{state_, handle};
}

namespace {

template <typename Operation>
Result<std::vector<std::byte>> VariableOutput(detail::RuntimeState& state,
                                              Operation operation) noexcept {
  try {
    ovf_crypto_mutable_bytes_v1 output{nullptr, 0};
    auto status = operation(&output);
    if (status != OVF_CRYPTO_STATUS_BUFFER_TOO_SMALL && status != OVF_CRYPTO_STATUS_OK) {
      return state.MakeError(status);
    }
    constexpr std::size_t maximum_output_size = 64U * 1024U * 1024U;
    if (output.size > maximum_output_size) {
      return Error{ErrorCode::resource_exhausted, "provider output exceeds the runtime limit"};
    }
    std::vector<std::byte> bytes(output.size);
    output.data = reinterpret_cast<std::uint8_t*>(bytes.data());
    status = operation(&output);
    if (status != OVF_CRYPTO_STATUS_OK || output.size > bytes.size()) {
      return status == OVF_CRYPTO_STATUS_OK
                 ? Error{ErrorCode::incompatible_abi, "provider returned an invalid output size"}
                 : state.MakeError(status);
    }
    bytes.resize(output.size);
    return bytes;
  } catch (...) {
    return Error{ErrorCode::resource_exhausted, "cannot allocate cryptographic output"};
  }
}

} // namespace

Result<std::vector<std::byte>>
AeadRecordStream::Process(std::span<const std::byte> record) noexcept {
  if (!valid() || record.empty()) {
    return Error{ErrorCode::invalid_state, "record stream is not active"};
  }
  std::scoped_lock lock(state_->mutex_);
  return VariableOutput(*state_, [&](ovf_crypto_mutable_bytes_v1* output) {
    return state_->backend_->stream_process_record(state_->backend_, handle_, View(record), output);
  });
}

Result<std::vector<std::byte>> Runtime::Hash(Algorithm algorithm,
                                             std::span<const std::byte> input) const noexcept {
  std::scoped_lock lock(state_->mutex_);
  return VariableOutput(*state_, [&](ovf_crypto_mutable_bytes_v1* output) {
    return state_->backend_->hash(state_->backend_, static_cast<std::uint32_t>(algorithm),
                                  View(input), output);
  });
}

Result<std::vector<std::byte>> Runtime::Mac(Algorithm algorithm, const Key& key,
                                            std::span<const std::byte> input) const noexcept {
  if (!key.valid() || key.state_ != state_) {
    return Error{ErrorCode::invalid_argument, "key belongs to another runtime"};
  }
  std::scoped_lock lock(state_->mutex_);
  return VariableOutput(*state_, [&](ovf_crypto_mutable_bytes_v1* output) {
    return state_->backend_->mac(state_->backend_, static_cast<std::uint32_t>(algorithm),
                                 key.handle_, View(input), output);
  });
}

namespace {
ovf_crypto_aead_parameters_v1 ToAbi(AeadParameters parameters) noexcept {
  return {sizeof(ovf_crypto_aead_parameters_v1), View(parameters.nonce),
          View(parameters.associated_data), parameters.tag_size};
}
} // namespace

Result<std::vector<std::byte>>
Runtime::Encrypt(Algorithm algorithm, const Key& key, AeadParameters parameters,
                 std::span<const std::byte> plaintext) const noexcept {
  if (!key.valid() || key.state_ != state_) {
    return Error{ErrorCode::invalid_argument, "key belongs to another runtime"};
  }
  const auto abi_parameters = ToAbi(parameters);
  std::scoped_lock lock(state_->mutex_);
  return VariableOutput(*state_, [&](ovf_crypto_mutable_bytes_v1* output) {
    return state_->backend_->aead_encrypt(state_->backend_, static_cast<std::uint32_t>(algorithm),
                                          key.handle_, &abi_parameters, View(plaintext), output);
  });
}

Result<std::vector<std::byte>>
Runtime::Decrypt(Algorithm algorithm, const Key& key, AeadParameters parameters,
                 std::span<const std::byte> ciphertext) const noexcept {
  if (!key.valid() || key.state_ != state_) {
    return Error{ErrorCode::invalid_argument, "key belongs to another runtime"};
  }
  const auto abi_parameters = ToAbi(parameters);
  std::scoped_lock lock(state_->mutex_);
  return VariableOutput(*state_, [&](ovf_crypto_mutable_bytes_v1* output) {
    return state_->backend_->aead_decrypt(state_->backend_, static_cast<std::uint32_t>(algorithm),
                                          key.handle_, &abi_parameters, View(ciphertext), output);
  });
}

Result<std::vector<std::byte>> Runtime::Sign(Algorithm algorithm, const Key& key,
                                             std::span<const std::byte> message) const noexcept {
  if (!key.valid() || key.state_ != state_) {
    return Error{ErrorCode::invalid_argument, "key belongs to another runtime"};
  }
  std::scoped_lock lock(state_->mutex_);
  return VariableOutput(*state_, [&](ovf_crypto_mutable_bytes_v1* output) {
    return state_->backend_->sign(state_->backend_, static_cast<std::uint32_t>(algorithm),
                                  key.handle_, View(message), output);
  });
}

Result<bool> Runtime::Verify(Algorithm algorithm, const Key& key,
                             std::span<const std::byte> message,
                             std::span<const std::byte> signature) const noexcept {
  if (!key.valid() || key.state_ != state_) {
    return Error{ErrorCode::invalid_argument, "key belongs to another runtime"};
  }
  std::uint8_t valid{};
  std::scoped_lock lock(state_->mutex_);
  const auto status =
      state_->backend_->verify(state_->backend_, static_cast<std::uint32_t>(algorithm), key.handle_,
                               View(message), View(signature), &valid);
  if (status != OVF_CRYPTO_STATUS_OK) {
    return state_->MakeError(status);
  }
  return valid != 0;
}

Result<std::vector<std::byte>> Runtime::Derive(Algorithm algorithm, const Key& key,
                                               std::span<const std::byte> salt,
                                               std::span<const std::byte> info,
                                               std::size_t output_size) const noexcept {
  if (!key.valid() || key.state_ != state_ || output_size == 0) {
    return Error{ErrorCode::invalid_argument, "invalid derivation request"};
  }
  try {
    std::vector<std::byte> output(output_size);
    ovf_crypto_mutable_bytes_v1 abi_output{reinterpret_cast<std::uint8_t*>(output.data()),
                                           output.size()};
    std::scoped_lock lock(state_->mutex_);
    const auto status =
        state_->backend_->derive(state_->backend_, static_cast<std::uint32_t>(algorithm),
                                 key.handle_, View(salt), View(info), &abi_output);
    if (status != OVF_CRYPTO_STATUS_OK || abi_output.size > output.size()) {
      return status == OVF_CRYPTO_STATUS_OK
                 ? Error{ErrorCode::incompatible_abi, "provider returned an invalid output size"}
                 : state_->MakeError(status);
    }
    output.resize(abi_output.size);
    return output;
  } catch (...) {
    return Error{ErrorCode::resource_exhausted, "cannot allocate derivation output"};
  }
}

Result<std::vector<std::byte>> Runtime::PublicValue(const Key& key) const noexcept {
  if (!key.valid() || key.state_ != state_) {
    return Error{ErrorCode::invalid_argument, "key belongs to another runtime"};
  }
  std::scoped_lock lock(state_->mutex_);
  return VariableOutput(*state_, [&](ovf_crypto_mutable_bytes_v1* output) {
    return state_->backend_->key_public_value(state_->backend_, key.handle_, output);
  });
}

Result<Key> Runtime::Agree(Algorithm algorithm, const Key& private_key,
                           std::span<const std::byte> peer_public_value,
                           std::span<const std::byte> salt, KeyPolicy derived_key) const noexcept {
  if (!private_key.valid() || private_key.state_ != state_ || peer_public_value.empty() ||
      derived_key.persistent) {
    return Error{ErrorCode::invalid_argument, "invalid key-agreement request"};
  }
  const ovf_crypto_key_descriptor_v1 descriptor{sizeof(descriptor),
                                                static_cast<std::uint32_t>(derived_key.algorithm),
                                                static_cast<std::uint32_t>(derived_key.usage),
                                                static_cast<std::uint8_t>(derived_key.exportable),
                                                static_cast<std::uint8_t>(derived_key.persistent),
                                                {}};
  ovf_crypto_handle_v1 output{};
  std::scoped_lock lock(state_->mutex_);
  const auto status = state_->backend_->key_agree(
      state_->backend_, static_cast<std::uint32_t>(algorithm), private_key.handle_,
      View(peer_public_value), View(salt), &descriptor, &output);
  if (status != OVF_CRYPTO_STATUS_OK) {
    return state_->MakeError(status);
  }
  return Key(state_, output);
}

Result<CertificateValidationResult>
Runtime::ValidateCertificate(const CertificateValidationRequest& request) const noexcept {
  constexpr std::size_t maximum_chain_entries = 32;
  if (request.leaf.empty() || request.trust_anchors.empty() ||
      request.intermediates.size() > maximum_chain_entries ||
      request.trust_anchors.size() > maximum_chain_entries ||
      request.crls.size() > maximum_chain_entries || request.validation_time_unix_seconds == 0 ||
      request.minimum_security_bits < 80) {
    return Error{ErrorCode::invalid_argument, "invalid certificate-validation request"};
  }
  try {
    const auto make_views = [](std::span<const std::span<const std::byte>> inputs) {
      std::vector<ovf_crypto_bytes_view_v1> output;
      output.reserve(inputs.size());
      for (const auto input : inputs) {
        output.push_back(View(input));
      }
      return output;
    };
    const auto intermediates = make_views(request.intermediates);
    const auto anchors = make_views(request.trust_anchors);
    const auto crls = make_views(request.crls);
    ovf_crypto_certificate_validation_request_v1 abi_request{
        sizeof(abi_request),
        View(request.leaf),
        intermediates.data(),
        intermediates.size(),
        anchors.data(),
        anchors.size(),
        crls.data(),
        crls.size(),
        {request.expected_name.data(), request.expected_name.size()},
        request.validation_time_unix_seconds,
        request.minimum_security_bits,
        static_cast<ovf_crypto_certificate_usage_v1>(request.usage),
        static_cast<std::uint8_t>(request.require_revocation),
        static_cast<std::uint8_t>(request.require_self_signed_anchor),
        {}};
    ovf_crypto_certificate_validation_result_v1 abi_result{};
    abi_result.struct_size = sizeof(abi_result);
    std::scoped_lock lock(state_->mutex_);
    const auto status =
        state_->backend_->certificate_validate(state_->backend_, &abi_request, &abi_result);
    if (status != OVF_CRYPTO_STATUS_OK) {
      return state_->MakeError(status);
    }
    if (abi_result.struct_size < sizeof(abi_result) ||
        abi_result.verdict > OVF_CRYPTO_CERTIFICATE_VERDICT_POLICY_REJECTED) {
      return Error{ErrorCode::incompatible_abi, "provider returned an invalid certificate result"};
    }
    return CertificateValidationResult{abi_result.valid != 0,
                                       static_cast<CertificateVerdict>(abi_result.verdict),
                                       abi_result.verified_chain_length, abi_result.native_status};
  } catch (...) {
    return Error{ErrorCode::resource_exhausted, "cannot allocate certificate-validation input"};
  }
}

Result<InputStream> Runtime::BeginStream(ovf_crypto_stream_operation_v1 operation,
                                         Algorithm algorithm, const Key* key) const noexcept {
  if (key != nullptr && (!key->valid() || key->state_ != state_)) {
    return Error{ErrorCode::invalid_argument, "key belongs to another runtime"};
  }
  const ovf_crypto_stream_descriptor_v1 descriptor{sizeof(descriptor),
                                                   operation,
                                                   static_cast<std::uint32_t>(algorithm),
                                                   key == nullptr ? OVF_CRYPTO_INVALID_HANDLE_V1
                                                                  : key->handle_,
                                                   {},
                                                   {}};
  ovf_crypto_handle_v1 handle{};
  std::scoped_lock lock(state_->mutex_);
  const auto status = state_->backend_->stream_create(state_->backend_, &descriptor, &handle);
  if (status != OVF_CRYPTO_STATUS_OK) {
    return state_->MakeError(status);
  }
  if (handle == OVF_CRYPTO_INVALID_HANDLE_V1) {
    return Error{ErrorCode::incompatible_abi, "provider returned an invalid stream handle"};
  }
  return InputStream(state_, handle, operation);
}

Result<InputStream> Runtime::BeginHash(Algorithm algorithm) const noexcept {
  return BeginStream(OVF_CRYPTO_STREAM_HASH, algorithm, nullptr);
}

Result<InputStream> Runtime::BeginMac(Algorithm algorithm, const Key& key) const noexcept {
  return BeginStream(OVF_CRYPTO_STREAM_MAC, algorithm, &key);
}

Result<InputStream> Runtime::BeginSign(Algorithm algorithm, const Key& key) const noexcept {
  return BeginStream(OVF_CRYPTO_STREAM_SIGN, algorithm, &key);
}

Result<InputStream> Runtime::BeginVerify(Algorithm algorithm, const Key& key) const noexcept {
  return BeginStream(OVF_CRYPTO_STREAM_VERIFY, algorithm, &key);
}

Result<AeadRecordStream> Runtime::BeginRecordStream(ovf_crypto_stream_operation_v1 operation,
                                                    Algorithm algorithm, const Key& key,
                                                    AeadParameters parameters) const noexcept {
  if (!key.valid() || key.state_ != state_ || parameters.nonce.size() != 12 ||
      parameters.tag_size != 16) {
    return Error{ErrorCode::invalid_argument, "invalid AEAD record-stream request"};
  }
  const ovf_crypto_stream_descriptor_v1 descriptor{
      sizeof(descriptor),
      operation,
      static_cast<std::uint32_t>(algorithm),
      key.handle_,
      {sizeof(ovf_crypto_aead_parameters_v1), View(parameters.nonce),
       View(parameters.associated_data), parameters.tag_size},
      {}};
  ovf_crypto_handle_v1 handle{};
  std::scoped_lock lock(state_->mutex_);
  const auto status = state_->backend_->stream_create(state_->backend_, &descriptor, &handle);
  if (status != OVF_CRYPTO_STATUS_OK) {
    return state_->MakeError(status);
  }
  if (handle == OVF_CRYPTO_INVALID_HANDLE_V1) {
    return Error{ErrorCode::incompatible_abi, "provider returned an invalid stream handle"};
  }
  return AeadRecordStream(state_, handle);
}

Result<AeadRecordStream> Runtime::BeginRecordEncryption(Algorithm algorithm, const Key& key,
                                                        AeadParameters parameters) const noexcept {
  return BeginRecordStream(OVF_CRYPTO_STREAM_AEAD_ENCRYPT_RECORDS, algorithm, key, parameters);
}

Result<AeadRecordStream> Runtime::BeginRecordDecryption(Algorithm algorithm, const Key& key,
                                                        AeadParameters parameters) const noexcept {
  return BeginRecordStream(OVF_CRYPTO_STREAM_AEAD_DECRYPT_RECORDS, algorithm, key, parameters);
}

Result<AsyncOperation>
Runtime::SubmitAsync(ovf_crypto_async_operation_v1 operation, Algorithm algorithm, const Key* key,
                     std::span<const std::byte> input, std::span<const std::byte> auxiliary,
                     std::chrono::steady_clock::time_point deadline) const noexcept {
  if (state_->async_ == nullptr) {
    return Error{ErrorCode::unsupported, "provider has no asynchronous extension"};
  }
  if (input.empty() || (key != nullptr && (!key->valid() || key->state_ != state_))) {
    return Error{ErrorCode::invalid_argument, "invalid asynchronous request"};
  }
  if (deadline <= std::chrono::steady_clock::now()) {
    return Error{ErrorCode::deadline_exceeded, "asynchronous deadline has elapsed"};
  }
  try {
    auto completion = std::make_shared<AsyncOperation::State>();
    completion->runtime = state_;
    completion->extension = state_->async_;
    completion->boolean_result = operation == OVF_CRYPTO_ASYNC_VERIFY;
    const auto deadline_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count());
    const ovf_crypto_async_request_v1 request{sizeof(request),
                                              operation,
                                              static_cast<std::uint32_t>(algorithm),
                                              key == nullptr ? OVF_CRYPTO_INVALID_HANDLE_V1
                                                             : key->handle_,
                                              View(input),
                                              View(auxiliary),
                                              deadline_ns,
                                              completion.get(),
                                              AsyncOperation::State::Complete,
                                              {}};
    ovf_crypto_handle_v1 ticket{};
    std::scoped_lock lock(state_->mutex_);
    const auto status = state_->async_->submit(state_->async_, &request, &ticket);
    if (status != OVF_CRYPTO_STATUS_OK) {
      return state_->MakeError(status);
    }
    if (ticket == OVF_CRYPTO_INVALID_HANDLE_V1) {
      return Error{ErrorCode::incompatible_abi, "provider returned an invalid async ticket"};
    }
    completion->ticket = ticket;
    return AsyncOperation(std::move(completion));
  } catch (...) {
    return Error{ErrorCode::resource_exhausted, "cannot allocate asynchronous request"};
  }
}

Result<AsyncOperation>
Runtime::AsyncHash(Algorithm algorithm, std::span<const std::byte> input,
                   std::chrono::steady_clock::time_point deadline) const noexcept {
  return SubmitAsync(OVF_CRYPTO_ASYNC_HASH, algorithm, nullptr, input, {}, deadline);
}

Result<AsyncOperation>
Runtime::AsyncSign(Algorithm algorithm, const Key& key, std::span<const std::byte> message,
                   std::chrono::steady_clock::time_point deadline) const noexcept {
  return SubmitAsync(OVF_CRYPTO_ASYNC_SIGN, algorithm, &key, message, {}, deadline);
}

Result<AsyncOperation>
Runtime::AsyncVerify(Algorithm algorithm, const Key& key, std::span<const std::byte> message,
                     std::span<const std::byte> signature,
                     std::chrono::steady_clock::time_point deadline) const noexcept {
  if (signature.empty()) {
    return Error{ErrorCode::invalid_argument, "signature is empty"};
  }
  return SubmitAsync(OVF_CRYPTO_ASYNC_VERIFY, algorithm, &key, message, signature, deadline);
}

void Runtime::Stop() noexcept {
  if (state_ != nullptr) {
    state_->Stop();
  }
}

} // namespace ovf::crypto
