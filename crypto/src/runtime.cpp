// SPDX-License-Identifier: Apache-2.0

#include "ovf/crypto/crypto.hpp"

#include <algorithm>
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
               void* library) noexcept
      : factory_(*factory), backend_(backend), library_(library) {}

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

  ovf_crypto_backend_factory_v1 factory_{};
  ovf_crypto_backend_v1* backend_{};
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
        backend->verify != nullptr && backend->derive != nullptr && backend->last_error != nullptr;
    if (!valid) {
      factory.destroy(backend);
      return Error{ErrorCode::incompatible_abi, "incomplete cryptographic provider ABI"};
    }
    const auto start_status = backend->start(backend);
    if (start_status != OVF_CRYPTO_STATUS_OK) {
      factory.destroy(backend);
      return Error{ToErrorCode(start_status), "cannot start cryptographic provider"};
    }
    auto state = std::make_shared<detail::RuntimeState>(&factory, backend, nullptr);
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

void Runtime::Stop() noexcept {
  if (state_ != nullptr) {
    state_->Stop();
  }
}

} // namespace ovf::crypto
