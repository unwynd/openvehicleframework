// SPDX-License-Identifier: Apache-2.0

#include "ovf/crypto/backend_abi.h"

#include <botan/aead.h>
#include <botan/auto_rng.h>
#include <botan/cipher_mode.h>
#include <botan/hash.h>
#include <botan/kdf.h>
#include <botan/mac.h>
#include <botan/pk_algs.h>
#include <botan/pk_keys.h>
#include <botan/pkcs8.h>
#include <botan/pubkey.h>
#include <botan/secmem.h>
#include <botan/x509_key.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kMaximumOperationSize = 64U * 1024U * 1024U;

struct KeyEntry final {
  std::uint32_t generation{1};
  std::uint32_t algorithm{};
  std::uint32_t usage{};
  Botan::secure_vector<std::uint8_t> secret;
  std::unique_ptr<Botan::Private_Key> private_key;
  std::unique_ptr<Botan::Public_Key> public_key;
};

struct Backend final {
  ovf_crypto_backend_v1 abi{};
  std::mutex mutex;
  Botan::AutoSeeded_RNG rng;
  std::vector<std::optional<KeyEntry>> keys;
  std::vector<std::uint32_t> generations;
  std::string last_error;
  bool running{};
};

Backend* Self(ovf_crypto_backend_v1* self) { return static_cast<Backend*>(self->implementation); }

ovf_crypto_status_v1 Fail(Backend& backend, ovf_crypto_status_v1 status,
                          std::string message) noexcept {
  backend.last_error = std::move(message);
  return status;
}

ovf_crypto_status_v1 BotanFailure(Backend& backend, const std::exception& exception) noexcept {
  return Fail(backend, OVF_CRYPTO_STATUS_BACKEND_ERROR, exception.what());
}

bool ValidView(ovf_crypto_bytes_view_v1 value) noexcept {
  return value.data != nullptr || value.size == 0;
}

std::span<const std::uint8_t> Span(ovf_crypto_bytes_view_v1 value) noexcept {
  return {value.data, value.size};
}

ovf_crypto_handle_v1 Handle(std::size_t index, std::uint32_t generation) noexcept {
  return (static_cast<std::uint64_t>(generation) << 32U) | static_cast<std::uint64_t>(index + 1U);
}

KeyEntry* FindKey(Backend& backend, ovf_crypto_handle_v1 handle) noexcept {
  if (handle == OVF_CRYPTO_INVALID_HANDLE_V1) {
    return nullptr;
  }
  const auto encoded_index = static_cast<std::uint32_t>(handle);
  const auto generation = static_cast<std::uint32_t>(handle >> 32U);
  if (encoded_index == 0 || encoded_index > backend.keys.size()) {
    return nullptr;
  }
  auto& slot = backend.keys[encoded_index - 1U];
  return slot.has_value() && slot->generation == generation ? &*slot : nullptr;
}

bool Permits(const KeyEntry& key, std::uint32_t usage) noexcept {
  return (key.usage & usage) == usage;
}

ovf_crypto_status_v1 WriteOutput(ovf_crypto_mutable_bytes_v1* output,
                                 std::span<const std::uint8_t> bytes) noexcept {
  if (output == nullptr) {
    return OVF_CRYPTO_STATUS_INVALID_ARGUMENT;
  }
  if (output->data == nullptr || output->size < bytes.size()) {
    output->size = bytes.size();
    return OVF_CRYPTO_STATUS_BUFFER_TOO_SMALL;
  }
  if (!bytes.empty()) {
    std::memcpy(output->data, bytes.data(), bytes.size());
  }
  output->size = bytes.size();
  return OVF_CRYPTO_STATUS_OK;
}

const char* HashName(std::uint32_t algorithm) noexcept {
  switch (algorithm) {
  case OVF_CRYPTO_ALGORITHM_SHA2_256:
    return "SHA-256";
  case OVF_CRYPTO_ALGORITHM_SHA2_384:
    return "SHA-384";
  case OVF_CRYPTO_ALGORITHM_SHA2_512:
    return "SHA-512";
  default:
    return nullptr;
  }
}

const char* CipherName(std::uint32_t algorithm) noexcept {
  switch (algorithm) {
  case OVF_CRYPTO_ALGORITHM_AES_128_GCM:
    return "AES-128/GCM";
  case OVF_CRYPTO_ALGORITHM_AES_256_GCM:
    return "AES-256/GCM";
  default:
    return nullptr;
  }
}

const char* SignaturePadding(std::uint32_t algorithm) noexcept {
  switch (algorithm) {
  case OVF_CRYPTO_ALGORITHM_ECDSA_P256_SHA2_256:
    return "EMSA1(SHA-256)";
  case OVF_CRYPTO_ALGORITHM_RSA_PSS_SHA2_256:
    return "PSS(SHA-256)";
  case OVF_CRYPTO_ALGORITHM_ED25519:
    return "Pure";
  default:
    return nullptr;
  }
}

Botan::Signature_Format SignatureFormat(std::uint32_t algorithm) noexcept {
  return algorithm == OVF_CRYPTO_ALGORITHM_ECDSA_P256_SHA2_256
             ? Botan::Signature_Format::DerSequence
             : Botan::Signature_Format::Standard;
}

std::size_t SymmetricKeySize(std::uint32_t algorithm) noexcept {
  switch (algorithm) {
  case OVF_CRYPTO_ALGORITHM_AES_128_GCM:
    return 16;
  case OVF_CRYPTO_ALGORITHM_AES_256_GCM:
    return 32;
  case OVF_CRYPTO_ALGORITHM_HMAC_SHA2_256:
  case OVF_CRYPTO_ALGORITHM_HKDF_SHA2_256:
    return 32;
  default:
    return 0;
  }
}

ovf_crypto_status_v1 Start(ovf_crypto_backend_v1* self) {
  auto& backend = *Self(self);
  std::scoped_lock lock(backend.mutex);
  if (backend.running) {
    return Fail(backend, OVF_CRYPTO_STATUS_INVALID_STATE, "Botan provider is already running");
  }
  try {
    std::array<std::uint8_t, 1> probe{};
    backend.rng.randomize(probe);
    backend.running = true;
    return OVF_CRYPTO_STATUS_OK;
  } catch (const std::exception& exception) {
    return Fail(backend, OVF_CRYPTO_STATUS_ENTROPY_FAILURE, exception.what());
  }
}

void Stop(ovf_crypto_backend_v1* self) {
  auto& backend = *Self(self);
  std::scoped_lock lock(backend.mutex);
  backend.running = false;
  for (auto& key : backend.keys) {
    key.reset();
  }
}

ovf_crypto_status_v1 GetCapabilities(ovf_crypto_backend_v1* self,
                                     ovf_crypto_capabilities_v1* output) {
  auto& backend = *Self(self);
  std::scoped_lock lock(backend.mutex);
  if (output == nullptr || output->struct_size < sizeof(*output)) {
    return Fail(backend, OVF_CRYPTO_STATUS_INVALID_ARGUMENT, "capability buffer is truncated");
  }
  const auto max_keys = static_cast<std::uint32_t>(backend.keys.size());
  *output = {sizeof(*output),
             10,
             {OVF_CRYPTO_ALGORITHM_SHA2_256, OVF_CRYPTO_ALGORITHM_SHA2_384,
              OVF_CRYPTO_ALGORITHM_SHA2_512, OVF_CRYPTO_ALGORITHM_HMAC_SHA2_256,
              OVF_CRYPTO_ALGORITHM_AES_128_GCM, OVF_CRYPTO_ALGORITHM_AES_256_GCM,
              OVF_CRYPTO_ALGORITHM_HKDF_SHA2_256, OVF_CRYPTO_ALGORITHM_ECDSA_P256_SHA2_256,
              OVF_CRYPTO_ALGORITHM_RSA_PSS_SHA2_256, OVF_CRYPTO_ALGORITHM_ED25519},
             max_keys,
             max_keys,
             kMaximumOperationSize,
             0,
             0,
             1,
             {}};
  return OVF_CRYPTO_STATUS_OK;
}

ovf_crypto_status_v1 RandomBytes(ovf_crypto_backend_v1* self, ovf_crypto_mutable_bytes_v1 output) {
  auto& backend = *Self(self);
  std::scoped_lock lock(backend.mutex);
  if (!backend.running) {
    return Fail(backend, OVF_CRYPTO_STATUS_INVALID_STATE, "Botan provider is stopped");
  }
  if (output.size > kMaximumOperationSize || (output.data == nullptr && output.size != 0)) {
    return Fail(backend, OVF_CRYPTO_STATUS_INVALID_ARGUMENT, "invalid random output buffer");
  }
  try {
    backend.rng.randomize(output.data, output.size);
    return OVF_CRYPTO_STATUS_OK;
  } catch (const std::exception& exception) {
    return Fail(backend, OVF_CRYPTO_STATUS_ENTROPY_FAILURE, exception.what());
  }
}

ovf_crypto_status_v1 StoreKey(Backend& backend, KeyEntry entry, ovf_crypto_handle_v1* output) {
  if (output == nullptr) {
    return Fail(backend, OVF_CRYPTO_STATUS_INVALID_ARGUMENT, "key handle output is null");
  }
  for (std::size_t index = 0; index < backend.keys.size(); ++index) {
    if (!backend.keys[index].has_value()) {
      const auto generation = backend.generations[index];
      entry.generation = generation;
      backend.keys[index].emplace(std::move(entry));
      *output = Handle(index, generation);
      return OVF_CRYPTO_STATUS_OK;
    }
  }
  return Fail(backend, OVF_CRYPTO_STATUS_RESOURCE_EXHAUSTED, "key capacity is exhausted");
}

ovf_crypto_status_v1 ImportKey(ovf_crypto_backend_v1* self,
                               const ovf_crypto_key_descriptor_v1* descriptor,
                               ovf_crypto_key_format_v1 format, ovf_crypto_bytes_view_v1 material,
                               ovf_crypto_handle_v1* output) {
  auto& backend = *Self(self);
  std::scoped_lock lock(backend.mutex);
  if (!backend.running || descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor) ||
      !ValidView(material) || material.size == 0 || material.size > kMaximumOperationSize) {
    return Fail(backend, OVF_CRYPTO_STATUS_INVALID_ARGUMENT, "invalid key import request");
  }
  if (descriptor->persistent != 0) {
    return Fail(backend, OVF_CRYPTO_STATUS_UNSUPPORTED,
                "persistent keys require a keystore provider");
  }
  try {
    KeyEntry entry;
    entry.algorithm = descriptor->algorithm;
    entry.usage = descriptor->permitted_usage;
    if (format == OVF_CRYPTO_KEY_FORMAT_RAW) {
      const auto required = SymmetricKeySize(descriptor->algorithm);
      if (required == 0 || material.size != required) {
        return Fail(backend, OVF_CRYPTO_STATUS_INVALID_ARGUMENT,
                    "raw key length does not match the selected algorithm");
      }
      entry.secret.assign(material.data, material.data + material.size);
    } else if (format == OVF_CRYPTO_KEY_FORMAT_DER || format == OVF_CRYPTO_KEY_FORMAT_PEM) {
      try {
        entry.private_key = Botan::PKCS8::load_key(Span(material));
      } catch (const std::exception&) {
        entry.public_key = Botan::X509::load_key(Span(material));
      }
    } else {
      return Fail(backend, OVF_CRYPTO_STATUS_INVALID_ARGUMENT, "unsupported key encoding");
    }
    return StoreKey(backend, std::move(entry), output);
  } catch (const std::exception& exception) {
    return BotanFailure(backend, exception);
  }
}

ovf_crypto_status_v1 GenerateKey(ovf_crypto_backend_v1* self,
                                 const ovf_crypto_key_descriptor_v1* descriptor,
                                 ovf_crypto_handle_v1* output) {
  auto& backend = *Self(self);
  std::scoped_lock lock(backend.mutex);
  if (!backend.running || descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
    return Fail(backend, OVF_CRYPTO_STATUS_INVALID_ARGUMENT, "invalid key generation request");
  }
  if (descriptor->persistent != 0) {
    return Fail(backend, OVF_CRYPTO_STATUS_UNSUPPORTED,
                "persistent keys require a keystore provider");
  }
  try {
    KeyEntry entry;
    entry.algorithm = descriptor->algorithm;
    entry.usage = descriptor->permitted_usage;
    const auto secret_size = SymmetricKeySize(descriptor->algorithm);
    if (secret_size != 0) {
      entry.secret.resize(secret_size);
      backend.rng.randomize(entry.secret);
    } else {
      const char* name{};
      const char* parameters{};
      switch (descriptor->algorithm) {
      case OVF_CRYPTO_ALGORITHM_ECDSA_P256_SHA2_256:
      case OVF_CRYPTO_ALGORITHM_ECDH_P256:
        name = descriptor->algorithm == OVF_CRYPTO_ALGORITHM_ECDH_P256 ? "ECDH" : "ECDSA";
        parameters = "secp256r1";
        break;
      case OVF_CRYPTO_ALGORITHM_RSA_PSS_SHA2_256:
        name = "RSA";
        parameters = "2048";
        break;
      case OVF_CRYPTO_ALGORITHM_ED25519:
        name = "Ed25519";
        parameters = "";
        break;
      default:
        return Fail(backend, OVF_CRYPTO_STATUS_UNSUPPORTED, "unsupported key algorithm");
      }
      entry.private_key = Botan::create_private_key(name, backend.rng, parameters);
      if (entry.private_key == nullptr) {
        return Fail(backend, OVF_CRYPTO_STATUS_UNSUPPORTED,
                    "Botan does not provide the selected private-key algorithm");
      }
    }
    return StoreKey(backend, std::move(entry), output);
  } catch (const std::exception& exception) {
    return BotanFailure(backend, exception);
  }
}

ovf_crypto_status_v1 DestroyKey(ovf_crypto_backend_v1* self, ovf_crypto_handle_v1 handle) {
  auto& backend = *Self(self);
  std::scoped_lock lock(backend.mutex);
  auto* key = FindKey(backend, handle);
  if (key == nullptr) {
    return Fail(backend, OVF_CRYPTO_STATUS_NOT_FOUND, "key handle is stale or unknown");
  }
  const auto index = static_cast<std::uint32_t>(handle) - 1U;
  backend.keys[index].reset();
  ++backend.generations[index];
  if (backend.generations[index] == 0) {
    backend.generations[index] = 1;
  }
  return OVF_CRYPTO_STATUS_OK;
}

ovf_crypto_status_v1 Hash(ovf_crypto_backend_v1* self, std::uint32_t algorithm,
                          ovf_crypto_bytes_view_v1 input, ovf_crypto_mutable_bytes_v1* output) {
  auto& backend = *Self(self);
  std::scoped_lock lock(backend.mutex);
  const char* name = HashName(algorithm);
  if (!backend.running || name == nullptr || !ValidView(input) ||
      input.size > kMaximumOperationSize) {
    return Fail(backend, OVF_CRYPTO_STATUS_INVALID_ARGUMENT, "invalid hash request");
  }
  try {
    auto hash = Botan::HashFunction::create_or_throw(name);
    hash->update(Span(input));
    const auto digest = hash->final_stdvec();
    return WriteOutput(output, digest);
  } catch (const std::exception& exception) {
    return BotanFailure(backend, exception);
  }
}

ovf_crypto_status_v1 Mac(ovf_crypto_backend_v1* self, std::uint32_t algorithm,
                         ovf_crypto_handle_v1 handle, ovf_crypto_bytes_view_v1 input,
                         ovf_crypto_mutable_bytes_v1* output) {
  auto& backend = *Self(self);
  std::scoped_lock lock(backend.mutex);
  auto* key = FindKey(backend, handle);
  if (algorithm != OVF_CRYPTO_ALGORITHM_HMAC_SHA2_256 || key == nullptr ||
      key->algorithm != algorithm || !Permits(*key, OVF_CRYPTO_KEY_USAGE_MAC_GENERATE) ||
      !ValidView(input)) {
    return Fail(backend, OVF_CRYPTO_STATUS_PERMISSION_DENIED,
                "MAC key policy rejected the request");
  }
  try {
    auto mac = Botan::MessageAuthenticationCode::create_or_throw("HMAC(SHA-256)");
    mac->set_key(key->secret);
    mac->update(Span(input));
    const auto value = mac->final_stdvec();
    return WriteOutput(output, value);
  } catch (const std::exception& exception) {
    return BotanFailure(backend, exception);
  }
}

ovf_crypto_status_v1 Aead(ovf_crypto_backend_v1* self, std::uint32_t algorithm,
                          ovf_crypto_handle_v1 handle,
                          const ovf_crypto_aead_parameters_v1* parameters,
                          ovf_crypto_bytes_view_v1 input, ovf_crypto_mutable_bytes_v1* output,
                          Botan::Cipher_Dir direction) {
  auto& backend = *Self(self);
  std::scoped_lock lock(backend.mutex);
  auto* key = FindKey(backend, handle);
  const auto usage = direction == Botan::Cipher_Dir::Encryption ? OVF_CRYPTO_KEY_USAGE_ENCRYPT
                                                                : OVF_CRYPTO_KEY_USAGE_DECRYPT;
  const char* name = CipherName(algorithm);
  if (name == nullptr || key == nullptr || key->algorithm != algorithm || !Permits(*key, usage) ||
      parameters == nullptr || parameters->struct_size < sizeof(*parameters) ||
      !ValidView(parameters->nonce) || !ValidView(parameters->associated_data) ||
      !ValidView(input) || parameters->tag_size != 16 || input.size > kMaximumOperationSize) {
    return Fail(backend, OVF_CRYPTO_STATUS_PERMISSION_DENIED,
                "AEAD request or key policy is invalid");
  }
  try {
    auto cipher = Botan::AEAD_Mode::create_or_throw(name, direction);
    cipher->set_key(key->secret);
    cipher->set_associated_data(Span(parameters->associated_data));
    cipher->start(Span(parameters->nonce));
    Botan::secure_vector<std::uint8_t> buffer(input.data, input.data + input.size);
    cipher->finish(buffer);
    return WriteOutput(output, buffer);
  } catch (const Botan::Integrity_Failure& exception) {
    return Fail(backend, OVF_CRYPTO_STATUS_AUTHENTICATION_FAILED, exception.what());
  } catch (const std::exception& exception) {
    return BotanFailure(backend, exception);
  }
}

ovf_crypto_status_v1 Encrypt(ovf_crypto_backend_v1* self, std::uint32_t algorithm,
                             ovf_crypto_handle_v1 key,
                             const ovf_crypto_aead_parameters_v1* parameters,
                             ovf_crypto_bytes_view_v1 input, ovf_crypto_mutable_bytes_v1* output) {
  return Aead(self, algorithm, key, parameters, input, output, Botan::Cipher_Dir::Encryption);
}

ovf_crypto_status_v1 Decrypt(ovf_crypto_backend_v1* self, std::uint32_t algorithm,
                             ovf_crypto_handle_v1 key,
                             const ovf_crypto_aead_parameters_v1* parameters,
                             ovf_crypto_bytes_view_v1 input, ovf_crypto_mutable_bytes_v1* output) {
  return Aead(self, algorithm, key, parameters, input, output, Botan::Cipher_Dir::Decryption);
}

ovf_crypto_status_v1 Sign(ovf_crypto_backend_v1* self, std::uint32_t algorithm,
                          ovf_crypto_handle_v1 handle, ovf_crypto_bytes_view_v1 message,
                          ovf_crypto_mutable_bytes_v1* output) {
  auto& backend = *Self(self);
  std::scoped_lock lock(backend.mutex);
  auto* key = FindKey(backend, handle);
  const char* padding = SignaturePadding(algorithm);
  if (key == nullptr || key->private_key == nullptr || key->algorithm != algorithm ||
      !Permits(*key, OVF_CRYPTO_KEY_USAGE_SIGN) || padding == nullptr || !ValidView(message)) {
    return Fail(backend, OVF_CRYPTO_STATUS_PERMISSION_DENIED,
                "signing key policy rejected the request");
  }
  try {
    Botan::PK_Signer signer(*key->private_key, backend.rng, padding, SignatureFormat(algorithm));
    const auto maximum_signature_size = signer.signature_length();
    if (output == nullptr) {
      return Fail(backend, OVF_CRYPTO_STATUS_INVALID_ARGUMENT, "signature output is null");
    }
    if (output->data == nullptr || output->size < maximum_signature_size) {
      output->size = maximum_signature_size;
      return OVF_CRYPTO_STATUS_BUFFER_TOO_SMALL;
    }
    const auto signature = signer.sign_message(Span(message), backend.rng);
    return WriteOutput(output, signature);
  } catch (const std::exception& exception) {
    return BotanFailure(backend, exception);
  }
}

ovf_crypto_status_v1 Verify(ovf_crypto_backend_v1* self, std::uint32_t algorithm,
                            ovf_crypto_handle_v1 handle, ovf_crypto_bytes_view_v1 message,
                            ovf_crypto_bytes_view_v1 signature, std::uint8_t* valid) {
  auto& backend = *Self(self);
  std::scoped_lock lock(backend.mutex);
  auto* key = FindKey(backend, handle);
  const char* padding = SignaturePadding(algorithm);
  if (key == nullptr || key->algorithm != algorithm ||
      !Permits(*key, OVF_CRYPTO_KEY_USAGE_VERIFY) || padding == nullptr || !ValidView(message) ||
      !ValidView(signature) || valid == nullptr) {
    return Fail(backend, OVF_CRYPTO_STATUS_PERMISSION_DENIED,
                "verification key policy rejected the request");
  }
  try {
    std::unique_ptr<Botan::Public_Key> derived;
    const Botan::Public_Key* public_key = key->public_key.get();
    if (public_key == nullptr && key->private_key != nullptr) {
      derived = key->private_key->public_key();
      public_key = derived.get();
    }
    if (public_key == nullptr) {
      return Fail(backend, OVF_CRYPTO_STATUS_PERMISSION_DENIED, "key contains no public component");
    }
    Botan::PK_Verifier verifier(*public_key, padding, SignatureFormat(algorithm));
    *valid = verifier.verify_message(Span(message), Span(signature)) ? 1U : 0U;
    return OVF_CRYPTO_STATUS_OK;
  } catch (const std::exception& exception) {
    return BotanFailure(backend, exception);
  }
}

ovf_crypto_status_v1 Derive(ovf_crypto_backend_v1* self, std::uint32_t algorithm,
                            ovf_crypto_handle_v1 handle, ovf_crypto_bytes_view_v1 salt,
                            ovf_crypto_bytes_view_v1 info, ovf_crypto_mutable_bytes_v1* output) {
  auto& backend = *Self(self);
  std::scoped_lock lock(backend.mutex);
  auto* key = FindKey(backend, handle);
  if (algorithm != OVF_CRYPTO_ALGORITHM_HKDF_SHA2_256 || key == nullptr ||
      key->algorithm != algorithm || !Permits(*key, OVF_CRYPTO_KEY_USAGE_DERIVE) ||
      !ValidView(salt) || !ValidView(info) || output == nullptr || output->data == nullptr ||
      output->size == 0 || output->size > 8160) {
    return Fail(backend, OVF_CRYPTO_STATUS_PERMISSION_DENIED, "invalid HKDF request");
  }
  try {
    auto kdf = Botan::KDF::create_or_throw("HKDF(SHA-256)");
    kdf->derive_key({output->data, output->size}, key->secret, Span(salt), Span(info));
    return OVF_CRYPTO_STATUS_OK;
  } catch (const std::exception& exception) {
    return BotanFailure(backend, exception);
  }
}

ovf_crypto_status_v1 LastError(ovf_crypto_backend_v1* self, ovf_crypto_mutable_bytes_v1* output) {
  auto& backend = *Self(self);
  std::scoped_lock lock(backend.mutex);
  return WriteOutput(output, {reinterpret_cast<const std::uint8_t*>(backend.last_error.data()),
                              backend.last_error.size()});
}

ovf_crypto_status_v1 Create(const ovf_crypto_host_api_v1* host,
                            const ovf_crypto_backend_config_v1* config,
                            ovf_crypto_backend_v1** output) {
  if (host == nullptr || host->struct_size < sizeof(*host) || config == nullptr ||
      config->struct_size < sizeof(*config) || output == nullptr || config->max_keys == 0) {
    return OVF_CRYPTO_STATUS_INVALID_ARGUMENT;
  }
  try {
    auto backend = std::make_unique<Backend>();
    backend->keys.resize(config->max_keys);
    backend->generations.resize(config->max_keys, 1);
    backend->abi = {sizeof(ovf_crypto_backend_v1),
                    OVF_CRYPTO_BACKEND_ABI_VERSION_1,
                    backend.get(),
                    Start,
                    Stop,
                    GetCapabilities,
                    RandomBytes,
                    ImportKey,
                    GenerateKey,
                    DestroyKey,
                    Hash,
                    Mac,
                    Encrypt,
                    Decrypt,
                    Sign,
                    Verify,
                    Derive,
                    LastError};
    *output = &backend.release()->abi;
    return OVF_CRYPTO_STATUS_OK;
  } catch (...) {
    return OVF_CRYPTO_STATUS_RESOURCE_EXHAUSTED;
  }
}

void Destroy(ovf_crypto_backend_v1* self) { delete Self(self); }

const ovf_crypto_backend_factory_v1 kFactory{sizeof(ovf_crypto_backend_factory_v1),
                                             OVF_CRYPTO_BACKEND_ABI_VERSION_1,
                                             {"botan", 5},
                                             Create,
                                             Destroy};

} // namespace

extern "C" const ovf_crypto_backend_factory_v1* ovf_crypto_backend_query_v1(void) {
  return &kFactory;
}
