// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/core/result.hpp"
#include "ovf/crypto/backend_abi.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ovf::crypto {

enum class ErrorCode : std::uint8_t {
  invalid_argument,
  incompatible_abi,
  invalid_state,
  not_found,
  permission_denied,
  unsupported,
  resource_exhausted,
  authentication_failed,
  entropy_failure,
  backend_failure,
  shutting_down,
  cancelled,
  deadline_exceeded
};

struct Error final {
  ErrorCode code;
  std::string message;
};

template <typename T> using Result = ovf::core::Result<T, Error>;

enum class Algorithm : std::uint32_t {
  sha2_256 = OVF_CRYPTO_ALGORITHM_SHA2_256,
  sha2_384 = OVF_CRYPTO_ALGORITHM_SHA2_384,
  sha2_512 = OVF_CRYPTO_ALGORITHM_SHA2_512,
  hmac_sha2_256 = OVF_CRYPTO_ALGORITHM_HMAC_SHA2_256,
  aes_128_gcm = OVF_CRYPTO_ALGORITHM_AES_128_GCM,
  aes_256_gcm = OVF_CRYPTO_ALGORITHM_AES_256_GCM,
  hkdf_sha2_256 = OVF_CRYPTO_ALGORITHM_HKDF_SHA2_256,
  ecdsa_p256_sha2_256 = OVF_CRYPTO_ALGORITHM_ECDSA_P256_SHA2_256,
  rsa_pss_sha2_256 = OVF_CRYPTO_ALGORITHM_RSA_PSS_SHA2_256,
  ed25519 = OVF_CRYPTO_ALGORITHM_ED25519,
  ecdh_p256 = OVF_CRYPTO_ALGORITHM_ECDH_P256
};

enum class KeyUsage : std::uint32_t {
  sign = OVF_CRYPTO_KEY_USAGE_SIGN,
  verify = OVF_CRYPTO_KEY_USAGE_VERIFY,
  encrypt = OVF_CRYPTO_KEY_USAGE_ENCRYPT,
  decrypt = OVF_CRYPTO_KEY_USAGE_DECRYPT,
  mac_generate = OVF_CRYPTO_KEY_USAGE_MAC_GENERATE,
  mac_verify = OVF_CRYPTO_KEY_USAGE_MAC_VERIFY,
  derive = OVF_CRYPTO_KEY_USAGE_DERIVE
};

constexpr KeyUsage operator|(KeyUsage left, KeyUsage right) noexcept {
  return static_cast<KeyUsage>(static_cast<std::uint32_t>(left) |
                               static_cast<std::uint32_t>(right));
}

enum class KeyFormat : std::uint8_t { raw, der, pem };

struct KeyPolicy final {
  Algorithm algorithm;
  KeyUsage usage;
  bool exportable{false};
  bool persistent{false};
};

struct AeadParameters final {
  std::span<const std::byte> nonce;
  std::span<const std::byte> associated_data;
  std::uint32_t tag_size{16};
};

struct Capabilities final {
  std::vector<Algorithm> algorithms;
  std::uint32_t max_keys{};
  std::uint32_t max_contexts{};
  std::uint64_t max_input_size{};
  bool persistent_keys{};
  bool hardware_keys{};
  bool secure_memory{};
};

enum class CertificateUsage : std::uint8_t {
  unspecified,
  server_authentication,
  client_authentication,
  certificate_authority,
  ocsp_signing,
  encryption
};

enum class CertificateVerdict : std::uint8_t {
  trusted,
  untrusted,
  expired,
  revoked,
  name_mismatch,
  usage_rejected,
  malformed,
  revocation_unknown,
  policy_rejected
};

struct CertificateValidationRequest final {
  std::span<const std::byte> leaf;
  std::span<const std::span<const std::byte>> intermediates;
  std::span<const std::span<const std::byte>> trust_anchors;
  std::span<const std::span<const std::byte>> crls;
  std::string_view expected_name;
  std::uint64_t validation_time_unix_seconds{};
  std::uint32_t minimum_security_bits{128};
  CertificateUsage usage{CertificateUsage::unspecified};
  bool require_revocation{false};
  bool require_self_signed_anchor{true};
};

struct CertificateValidationResult final {
  bool valid{};
  CertificateVerdict verdict{CertificateVerdict::untrusted};
  std::uint32_t verified_chain_length{};
  std::uint64_t native_status{};
};

namespace detail {
class RuntimeState;
}

class Key final {
public:
  Key() = default;
  ~Key();
  Key(Key const&) = delete;
  Key& operator=(Key const&) = delete;
  Key(Key&&) noexcept;
  Key& operator=(Key&&) noexcept;
  [[nodiscard]] bool valid() const noexcept;

private:
  friend class Runtime;
  Key(std::shared_ptr<detail::RuntimeState>, ovf_crypto_handle_v1) noexcept;
  std::shared_ptr<detail::RuntimeState> state_;
  ovf_crypto_handle_v1 handle_{};
};

struct CertificatePublicKeyResult final {
  CertificateValidationResult validation;
  Key key;
};

class InputStream final {
public:
  InputStream() = default;
  ~InputStream();
  InputStream(InputStream const&) = delete;
  InputStream& operator=(InputStream const&) = delete;
  InputStream(InputStream&&) noexcept;
  InputStream& operator=(InputStream&&) noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Result<bool> Update(std::span<const std::byte> input) noexcept;
  [[nodiscard]] Result<std::vector<std::byte>> Finish() noexcept;
  [[nodiscard]] Result<bool> FinishVerify(std::span<const std::byte> signature) noexcept;
  void Cancel() noexcept;

private:
  friend class Runtime;
  InputStream(std::shared_ptr<detail::RuntimeState>, ovf_crypto_handle_v1,
              ovf_crypto_stream_operation_v1) noexcept;
  std::shared_ptr<detail::RuntimeState> state_;
  ovf_crypto_handle_v1 handle_{};
  ovf_crypto_stream_operation_v1 operation_{};
};

class AeadRecordStream final {
public:
  AeadRecordStream() = default;
  ~AeadRecordStream();
  AeadRecordStream(AeadRecordStream const&) = delete;
  AeadRecordStream& operator=(AeadRecordStream const&) = delete;
  AeadRecordStream(AeadRecordStream&&) noexcept;
  AeadRecordStream& operator=(AeadRecordStream&&) noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Result<std::vector<std::byte>> Process(std::span<const std::byte> record) noexcept;
  void Close() noexcept;

private:
  friend class Runtime;
  AeadRecordStream(std::shared_ptr<detail::RuntimeState>, ovf_crypto_handle_v1) noexcept;
  std::shared_ptr<detail::RuntimeState> state_;
  ovf_crypto_handle_v1 handle_{};
};

using AsyncValue = std::variant<std::vector<std::byte>, bool>;

class AsyncOperation final {
public:
  AsyncOperation() = default;
  ~AsyncOperation();
  AsyncOperation(AsyncOperation const&) = delete;
  AsyncOperation& operator=(AsyncOperation const&) = delete;
  AsyncOperation(AsyncOperation&&) noexcept;
  AsyncOperation& operator=(AsyncOperation&&) noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Result<AsyncValue> Wait() noexcept;
  [[nodiscard]] bool Cancel() noexcept;

private:
  friend class Runtime;
  class State;
  explicit AsyncOperation(std::shared_ptr<State>) noexcept;
  std::shared_ptr<State> state_;
};

struct RuntimeConfig final {
  std::string configuration;
  std::uint32_t max_keys{128};
  std::uint32_t max_contexts{64};
};

// Options for the unified Runtime::Open entry point. Set factory to construct
// with a linked-in provider; set provider (and optionally provider_directory)
// to dynamically load one. Exactly one of the two must be provided.
struct OpenOptions final {
  RuntimeConfig config{};
  const ovf_crypto_backend_factory_v1* factory{nullptr};
  std::string_view provider{};
  std::string_view provider_directory{};
};

// Grouped facades over Runtime. Each facade holds a Runtime pointer and
// forwards to the flat member functions. Callers can write
//   auto digest = runtime->hash().Compute(algo, input);
// instead of the flat runtime->Hash(algo, input), which makes the header
// easier to skim by concern. The flat methods are unchanged.
class Runtime;

class RandomFacade final {
public:
  explicit RandomFacade(Runtime* rt) noexcept : runtime_(rt) {}
  [[nodiscard]] Result<std::vector<std::byte>> Bytes(std::size_t size) const noexcept;

private:
  Runtime* runtime_;
};

class KeysFacade final {
public:
  explicit KeysFacade(Runtime* rt) noexcept : runtime_(rt) {}
  [[nodiscard]] Result<Key> Import(KeyPolicy policy, KeyFormat format,
                                   std::span<const std::byte> material) const noexcept;
  [[nodiscard]] Result<Key> Generate(KeyPolicy policy) const noexcept;
  [[nodiscard]] Result<std::vector<std::byte>> PublicValue(const Key& key) const noexcept;
  [[nodiscard]] Result<Key> Agree(Algorithm algorithm, const Key& private_key,
                                  std::span<const std::byte> peer_public_value,
                                  std::span<const std::byte> salt,
                                  KeyPolicy derived_key) const noexcept;
  [[nodiscard]] Result<std::vector<std::byte>> Derive(Algorithm algorithm, const Key& key,
                                                      std::span<const std::byte> salt,
                                                      std::span<const std::byte> info,
                                                      std::size_t output_size) const noexcept;

private:
  Runtime* runtime_;
};

class HashFacade final {
public:
  explicit HashFacade(Runtime* rt) noexcept : runtime_(rt) {}
  [[nodiscard]] Result<std::vector<std::byte>>
  Compute(Algorithm algorithm, std::span<const std::byte> input) const noexcept;
  [[nodiscard]] Result<std::vector<std::byte>> Mac(Algorithm algorithm, const Key& key,
                                                   std::span<const std::byte> input) const noexcept;

private:
  Runtime* runtime_;
};

class AeadFacade final {
public:
  explicit AeadFacade(Runtime* rt) noexcept : runtime_(rt) {}
  [[nodiscard]] Result<std::vector<std::byte>>
  Encrypt(Algorithm algorithm, const Key& key, AeadParameters parameters,
          std::span<const std::byte> plaintext) const noexcept;
  [[nodiscard]] Result<std::vector<std::byte>>
  Decrypt(Algorithm algorithm, const Key& key, AeadParameters parameters,
          std::span<const std::byte> ciphertext) const noexcept;

private:
  Runtime* runtime_;
};

class SignatureFacade final {
public:
  explicit SignatureFacade(Runtime* rt) noexcept : runtime_(rt) {}
  [[nodiscard]] Result<std::vector<std::byte>>
  Sign(Algorithm algorithm, const Key& key, std::span<const std::byte> message) const noexcept;
  [[nodiscard]] Result<bool> Verify(Algorithm algorithm, const Key& key,
                                    std::span<const std::byte> message,
                                    std::span<const std::byte> signature) const noexcept;

private:
  Runtime* runtime_;
};

class PkiFacade final {
public:
  explicit PkiFacade(Runtime* rt) noexcept : runtime_(rt) {}
  [[nodiscard]] Result<CertificateValidationResult>
  ValidateCertificate(const CertificateValidationRequest& request) const noexcept;
  [[nodiscard]] Result<CertificatePublicKeyResult>
  ValidateAndImportPublicKey(const CertificateValidationRequest& request,
                             KeyPolicy policy) const noexcept;

private:
  Runtime* runtime_;
};

class Runtime final {
public:
  // Open is the preferred factory entry point; the older Create/Load below
  // are retained as thin wrappers so existing call sites keep compiling.
  static Result<std::unique_ptr<Runtime>> Open(OpenOptions options) noexcept;

  static Result<std::unique_ptr<Runtime>> Create(const ovf_crypto_backend_factory_v1& factory,
                                                 RuntimeConfig config = {}) noexcept;
  static Result<std::unique_ptr<Runtime>> Load(std::string_view provider,
                                               RuntimeConfig config = {}) noexcept;
  ~Runtime();

  // Grouped facade accessors.
  [[nodiscard]] RandomFacade random() noexcept { return RandomFacade(this); }
  [[nodiscard]] KeysFacade keys() noexcept { return KeysFacade(this); }
  [[nodiscard]] HashFacade hash() noexcept { return HashFacade(this); }
  [[nodiscard]] AeadFacade aead() noexcept { return AeadFacade(this); }
  [[nodiscard]] SignatureFacade signatures() noexcept { return SignatureFacade(this); }
  [[nodiscard]] PkiFacade pki() noexcept { return PkiFacade(this); }
  Runtime(Runtime const&) = delete;
  Runtime& operator=(Runtime const&) = delete;

  [[nodiscard]] Result<Capabilities> GetCapabilities() const noexcept;
  [[nodiscard]] Result<std::vector<std::byte>> Random(std::size_t size) const noexcept;
  [[nodiscard]] Result<Key> ImportKey(KeyPolicy policy, KeyFormat format,
                                      std::span<const std::byte> material) const noexcept;
  [[nodiscard]] Result<Key> GenerateKey(KeyPolicy policy) const noexcept;
  [[nodiscard]] Result<std::vector<std::byte>>
  Hash(Algorithm algorithm, std::span<const std::byte> input) const noexcept;
  [[nodiscard]] Result<std::vector<std::byte>> Mac(Algorithm algorithm, const Key& key,
                                                   std::span<const std::byte> input) const noexcept;
  [[nodiscard]] Result<std::vector<std::byte>>
  Encrypt(Algorithm algorithm, const Key& key, AeadParameters parameters,
          std::span<const std::byte> plaintext) const noexcept;
  [[nodiscard]] Result<std::vector<std::byte>>
  Decrypt(Algorithm algorithm, const Key& key, AeadParameters parameters,
          std::span<const std::byte> ciphertext) const noexcept;
  [[nodiscard]] Result<std::vector<std::byte>>
  Sign(Algorithm algorithm, const Key& key, std::span<const std::byte> message) const noexcept;
  [[nodiscard]] Result<bool> Verify(Algorithm algorithm, const Key& key,
                                    std::span<const std::byte> message,
                                    std::span<const std::byte> signature) const noexcept;
  [[nodiscard]] Result<std::vector<std::byte>> Derive(Algorithm algorithm, const Key& key,
                                                      std::span<const std::byte> salt,
                                                      std::span<const std::byte> info,
                                                      std::size_t output_size) const noexcept;
  [[nodiscard]] Result<std::vector<std::byte>> PublicValue(const Key& key) const noexcept;
  [[nodiscard]] Result<Key> Agree(Algorithm algorithm, const Key& private_key,
                                  std::span<const std::byte> peer_public_value,
                                  std::span<const std::byte> salt,
                                  KeyPolicy derived_key) const noexcept;
  [[nodiscard]] Result<CertificateValidationResult>
  ValidateCertificate(const CertificateValidationRequest& request) const noexcept;
  [[nodiscard]] Result<CertificatePublicKeyResult>
  ValidateAndImportPublicKey(const CertificateValidationRequest& request,
                             KeyPolicy policy) const noexcept;
  [[nodiscard]] Result<InputStream> BeginHash(Algorithm algorithm) const noexcept;
  [[nodiscard]] Result<InputStream> BeginMac(Algorithm algorithm, const Key& key) const noexcept;
  [[nodiscard]] Result<InputStream> BeginSign(Algorithm algorithm, const Key& key) const noexcept;
  [[nodiscard]] Result<InputStream> BeginVerify(Algorithm algorithm, const Key& key) const noexcept;
  [[nodiscard]] Result<AeadRecordStream>
  BeginRecordEncryption(Algorithm algorithm, const Key& key,
                        AeadParameters parameters) const noexcept;
  [[nodiscard]] Result<AeadRecordStream>
  BeginRecordDecryption(Algorithm algorithm, const Key& key,
                        AeadParameters parameters) const noexcept;
  [[nodiscard]] Result<AsyncOperation>
  AsyncHash(Algorithm algorithm, std::span<const std::byte> input,
            std::chrono::steady_clock::time_point deadline) const noexcept;
  [[nodiscard]] Result<AsyncOperation>
  AsyncSign(Algorithm algorithm, const Key& key, std::span<const std::byte> message,
            std::chrono::steady_clock::time_point deadline) const noexcept;
  [[nodiscard]] Result<AsyncOperation>
  AsyncVerify(Algorithm algorithm, const Key& key, std::span<const std::byte> message,
              std::span<const std::byte> signature,
              std::chrono::steady_clock::time_point deadline) const noexcept;
  void Stop() noexcept;

private:
  Runtime(std::shared_ptr<detail::RuntimeState>, void* library) noexcept;
  [[nodiscard]] Result<InputStream> BeginStream(ovf_crypto_stream_operation_v1 operation,
                                                Algorithm algorithm, const Key* key) const noexcept;
  [[nodiscard]] Result<AeadRecordStream>
  BeginRecordStream(ovf_crypto_stream_operation_v1 operation, Algorithm algorithm, const Key& key,
                    AeadParameters parameters) const noexcept;
  [[nodiscard]] Result<AsyncOperation>
  SubmitAsync(ovf_crypto_async_operation_v1 operation, Algorithm algorithm, const Key* key,
              std::span<const std::byte> input, std::span<const std::byte> auxiliary,
              std::chrono::steady_clock::time_point deadline) const noexcept;
  std::shared_ptr<detail::RuntimeState> state_;
  void* library_{};
};

// Facade forwarders. Kept inline so the runtime stays header-only for these
// wrapper calls; each simply defers to the flat Runtime member.
inline Result<std::vector<std::byte>> RandomFacade::Bytes(std::size_t size) const noexcept {
  return runtime_->Random(size);
}
inline Result<Key> KeysFacade::Import(KeyPolicy policy, KeyFormat format,
                                      std::span<const std::byte> material) const noexcept {
  return runtime_->ImportKey(policy, format, material);
}
inline Result<Key> KeysFacade::Generate(KeyPolicy policy) const noexcept {
  return runtime_->GenerateKey(policy);
}
inline Result<std::vector<std::byte>> KeysFacade::PublicValue(const Key& key) const noexcept {
  return runtime_->PublicValue(key);
}
inline Result<Key> KeysFacade::Agree(Algorithm algorithm, const Key& private_key,
                                     std::span<const std::byte> peer_public_value,
                                     std::span<const std::byte> salt,
                                     KeyPolicy derived_key) const noexcept {
  return runtime_->Agree(algorithm, private_key, peer_public_value, salt, derived_key);
}
inline Result<std::vector<std::byte>> KeysFacade::Derive(Algorithm algorithm, const Key& key,
                                                         std::span<const std::byte> salt,
                                                         std::span<const std::byte> info,
                                                         std::size_t output_size) const noexcept {
  return runtime_->Derive(algorithm, key, salt, info, output_size);
}
inline Result<std::vector<std::byte>>
HashFacade::Compute(Algorithm algorithm, std::span<const std::byte> input) const noexcept {
  return runtime_->Hash(algorithm, input);
}
inline Result<std::vector<std::byte>>
HashFacade::Mac(Algorithm algorithm, const Key& key,
                std::span<const std::byte> input) const noexcept {
  return runtime_->Mac(algorithm, key, input);
}
inline Result<std::vector<std::byte>>
AeadFacade::Encrypt(Algorithm algorithm, const Key& key, AeadParameters parameters,
                    std::span<const std::byte> plaintext) const noexcept {
  return runtime_->Encrypt(algorithm, key, parameters, plaintext);
}
inline Result<std::vector<std::byte>>
AeadFacade::Decrypt(Algorithm algorithm, const Key& key, AeadParameters parameters,
                    std::span<const std::byte> ciphertext) const noexcept {
  return runtime_->Decrypt(algorithm, key, parameters, ciphertext);
}
inline Result<std::vector<std::byte>>
SignatureFacade::Sign(Algorithm algorithm, const Key& key,
                      std::span<const std::byte> message) const noexcept {
  return runtime_->Sign(algorithm, key, message);
}
inline Result<bool> SignatureFacade::Verify(Algorithm algorithm, const Key& key,
                                            std::span<const std::byte> message,
                                            std::span<const std::byte> signature) const noexcept {
  return runtime_->Verify(algorithm, key, message, signature);
}
inline Result<CertificateValidationResult>
PkiFacade::ValidateCertificate(const CertificateValidationRequest& request) const noexcept {
  return runtime_->ValidateCertificate(request);
}
inline Result<CertificatePublicKeyResult>
PkiFacade::ValidateAndImportPublicKey(const CertificateValidationRequest& request,
                                      KeyPolicy policy) const noexcept {
  return runtime_->ValidateAndImportPublicKey(request, policy);
}

} // namespace ovf::crypto
