// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/crypto/backend_abi.h"

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
  shutting_down
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

struct RuntimeConfig final {
  std::string configuration;
  std::uint32_t max_keys{128};
  std::uint32_t max_contexts{64};
};

class Runtime final {
public:
  static Result<std::unique_ptr<Runtime>> Create(const ovf_crypto_backend_factory_v1& factory,
                                                 RuntimeConfig config = {}) noexcept;
  static Result<std::unique_ptr<Runtime>> Load(std::string_view provider,
                                               RuntimeConfig config = {}) noexcept;
  ~Runtime();
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
  [[nodiscard]] Result<InputStream> BeginHash(Algorithm algorithm) const noexcept;
  [[nodiscard]] Result<InputStream> BeginMac(Algorithm algorithm, const Key& key) const noexcept;
  [[nodiscard]] Result<InputStream> BeginSign(Algorithm algorithm, const Key& key) const noexcept;
  [[nodiscard]] Result<InputStream> BeginVerify(Algorithm algorithm, const Key& key) const noexcept;
  void Stop() noexcept;

private:
  Runtime(std::shared_ptr<detail::RuntimeState>, void* library) noexcept;
  [[nodiscard]] Result<InputStream> BeginStream(ovf_crypto_stream_operation_v1 operation,
                                                Algorithm algorithm, const Key* key) const noexcept;
  std::shared_ptr<detail::RuntimeState> state_;
  void* library_{};
};

} // namespace ovf::crypto
