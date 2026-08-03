// SPDX-License-Identifier: Apache-2.0

#include "ovf/crypto/crypto.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct FakeBackend final {
  ovf_crypto_backend_v1 abi{};
  bool running{};
  bool key_exists{};
  std::string error;
};

FakeBackend* Self(ovf_crypto_backend_v1* self) {
  return static_cast<FakeBackend*>(self->implementation);
}

ovf_crypto_status_v1 Start(ovf_crypto_backend_v1* self) {
  Self(self)->running = true;
  return OVF_CRYPTO_STATUS_OK;
}

void Stop(ovf_crypto_backend_v1* self) { Self(self)->running = false; }

ovf_crypto_status_v1 Capabilities(ovf_crypto_backend_v1*, ovf_crypto_capabilities_v1* output) {
  if (output == nullptr || output->struct_size < sizeof(*output)) {
    return OVF_CRYPTO_STATUS_INVALID_ARGUMENT;
  }
  *output = {sizeof(*output), 1, {OVF_CRYPTO_ALGORITHM_SHA2_256}, 8, 4, 4096, 0, 0, 0, {}};
  return OVF_CRYPTO_STATUS_OK;
}

ovf_crypto_status_v1 Random(ovf_crypto_backend_v1* self, ovf_crypto_mutable_bytes_v1 output) {
  if (!Self(self)->running || (output.data == nullptr && output.size != 0)) {
    return OVF_CRYPTO_STATUS_INVALID_STATE;
  }
  std::fill_n(output.data, output.size, UINT8_C(0xA5));
  return OVF_CRYPTO_STATUS_OK;
}

ovf_crypto_status_v1 Import(ovf_crypto_backend_v1* self,
                            const ovf_crypto_key_descriptor_v1* descriptor,
                            ovf_crypto_key_format_v1 format, ovf_crypto_bytes_view_v1 material,
                            ovf_crypto_handle_v1* output) {
  if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor) || output == nullptr ||
      format != OVF_CRYPTO_KEY_FORMAT_RAW || material.data == nullptr || material.size == 0) {
    return OVF_CRYPTO_STATUS_INVALID_ARGUMENT;
  }
  Self(self)->key_exists = true;
  *output = 1;
  return OVF_CRYPTO_STATUS_OK;
}

ovf_crypto_status_v1 Generate(ovf_crypto_backend_v1*, const ovf_crypto_key_descriptor_v1*,
                              ovf_crypto_handle_v1*) {
  return OVF_CRYPTO_STATUS_UNSUPPORTED;
}

ovf_crypto_status_v1 DestroyKey(ovf_crypto_backend_v1* self, ovf_crypto_handle_v1 handle) {
  if (handle != 1 || !Self(self)->key_exists) {
    return OVF_CRYPTO_STATUS_NOT_FOUND;
  }
  Self(self)->key_exists = false;
  return OVF_CRYPTO_STATUS_OK;
}

ovf_crypto_status_v1 CopyResult(std::span<const std::uint8_t> value,
                                ovf_crypto_mutable_bytes_v1* output) {
  if (output == nullptr) {
    return OVF_CRYPTO_STATUS_INVALID_ARGUMENT;
  }
  if (output->data == nullptr || output->size < value.size()) {
    output->size = value.size();
    return OVF_CRYPTO_STATUS_BUFFER_TOO_SMALL;
  }
  std::memcpy(output->data, value.data(), value.size());
  output->size = value.size();
  return OVF_CRYPTO_STATUS_OK;
}

ovf_crypto_status_v1 Hash(ovf_crypto_backend_v1*, std::uint32_t algorithm,
                          ovf_crypto_bytes_view_v1 input, ovf_crypto_mutable_bytes_v1* output) {
  if (algorithm != OVF_CRYPTO_ALGORITHM_SHA2_256 || (input.data == nullptr && input.size != 0)) {
    return OVF_CRYPTO_STATUS_UNSUPPORTED;
  }
  const std::array<std::uint8_t, 4> digest{0xDE, 0xAD, 0xBE, 0xEF};
  return CopyResult(digest, output);
}

ovf_crypto_status_v1 UnsupportedOutput(ovf_crypto_backend_v1*, std::uint32_t, ovf_crypto_handle_v1,
                                       ovf_crypto_bytes_view_v1, ovf_crypto_mutable_bytes_v1*) {
  return OVF_CRYPTO_STATUS_UNSUPPORTED;
}

ovf_crypto_status_v1 UnsupportedAead(ovf_crypto_backend_v1*, std::uint32_t, ovf_crypto_handle_v1,
                                     const ovf_crypto_aead_parameters_v1*, ovf_crypto_bytes_view_v1,
                                     ovf_crypto_mutable_bytes_v1*) {
  return OVF_CRYPTO_STATUS_UNSUPPORTED;
}

ovf_crypto_status_v1 Verify(ovf_crypto_backend_v1*, std::uint32_t, ovf_crypto_handle_v1 handle,
                            ovf_crypto_bytes_view_v1, ovf_crypto_bytes_view_v1 signature,
                            std::uint8_t* valid) {
  if (handle != 1 || valid == nullptr) {
    return OVF_CRYPTO_STATUS_INVALID_ARGUMENT;
  }
  *valid = signature.size == 1 && signature.data != nullptr && signature.data[0] == 0x42;
  return OVF_CRYPTO_STATUS_OK;
}

ovf_crypto_status_v1 Derive(ovf_crypto_backend_v1*, std::uint32_t, ovf_crypto_handle_v1,
                            ovf_crypto_bytes_view_v1, ovf_crypto_bytes_view_v1,
                            ovf_crypto_mutable_bytes_v1*) {
  return OVF_CRYPTO_STATUS_UNSUPPORTED;
}

ovf_crypto_status_v1 PublicValue(ovf_crypto_backend_v1*, ovf_crypto_handle_v1,
                                 ovf_crypto_mutable_bytes_v1*) {
  return OVF_CRYPTO_STATUS_UNSUPPORTED;
}

ovf_crypto_status_v1 Agree(ovf_crypto_backend_v1*, std::uint32_t, ovf_crypto_handle_v1,
                           ovf_crypto_bytes_view_v1, ovf_crypto_bytes_view_v1,
                           const ovf_crypto_key_descriptor_v1*, ovf_crypto_handle_v1*) {
  return OVF_CRYPTO_STATUS_UNSUPPORTED;
}

ovf_crypto_status_v1 ValidateCertificate(ovf_crypto_backend_v1*,
                                         const ovf_crypto_certificate_validation_request_v1*,
                                         ovf_crypto_certificate_validation_result_v1*) {
  return OVF_CRYPTO_STATUS_UNSUPPORTED;
}

ovf_crypto_status_v1 LastError(ovf_crypto_backend_v1*, ovf_crypto_mutable_bytes_v1* output) {
  constexpr std::array<std::uint8_t, 12> message{'f', 'a', 'k', 'e', ' ',  'e',
                                                 'r', 'r', 'o', 'r', '\0', '\0'};
  return CopyResult(std::span(message).first(10), output);
}

ovf_crypto_status_v1 Create(const ovf_crypto_host_api_v1* host,
                            const ovf_crypto_backend_config_v1* config,
                            ovf_crypto_backend_v1** output) {
  if (host == nullptr || host->struct_size < sizeof(*host) || config == nullptr ||
      config->struct_size < sizeof(*config) || output == nullptr) {
    return OVF_CRYPTO_STATUS_INVALID_ARGUMENT;
  }
  auto* backend = new FakeBackend;
  backend->abi = {sizeof(ovf_crypto_backend_v1),
                  OVF_CRYPTO_BACKEND_ABI_VERSION_1,
                  backend,
                  Start,
                  Stop,
                  Capabilities,
                  Random,
                  Import,
                  Generate,
                  DestroyKey,
                  Hash,
                  UnsupportedOutput,
                  UnsupportedAead,
                  UnsupportedAead,
                  UnsupportedOutput,
                  Verify,
                  Derive,
                  PublicValue,
                  Agree,
                  ValidateCertificate,
                  LastError};
  *output = &backend->abi;
  return OVF_CRYPTO_STATUS_OK;
}

void Destroy(ovf_crypto_backend_v1* backend) { delete Self(backend); }

const ovf_crypto_backend_factory_v1 kFactory{sizeof(ovf_crypto_backend_factory_v1),
                                             OVF_CRYPTO_BACKEND_ABI_VERSION_1,
                                             {"fake", 4},
                                             Create,
                                             Destroy};

std::span<const std::byte> Bytes(std::string_view value) {
  return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

TEST(CryptoRuntimeTest, NegotiatesBuffersAndSeparatesInvalidSignatureFromFailure) {
  auto runtime_result = ovf::crypto::Runtime::Create(kFactory);
  ASSERT_TRUE(runtime_result) << runtime_result.error().message;
  auto runtime = std::move(runtime_result).value();

  const auto capabilities = runtime->GetCapabilities();
  ASSERT_TRUE(capabilities);
  EXPECT_EQ(capabilities.value().max_keys, 8U);
  ASSERT_EQ(capabilities.value().algorithms.size(), 1U);
  EXPECT_EQ(capabilities.value().algorithms.front(), ovf::crypto::Algorithm::sha2_256);

  const auto random = runtime->Random(16);
  ASSERT_TRUE(random);
  EXPECT_TRUE(std::ranges::all_of(random.value(),
                                  [](std::byte value) { return value == std::byte{0xA5}; }));

  const auto digest = runtime->Hash(ovf::crypto::Algorithm::sha2_256, Bytes("message"));
  ASSERT_TRUE(digest);
  EXPECT_EQ(digest.value(), (std::vector<std::byte>{std::byte{0xDE}, std::byte{0xAD},
                                                    std::byte{0xBE}, std::byte{0xEF}}));

  auto key_result = runtime->ImportKey(
      {ovf::crypto::Algorithm::ecdsa_p256_sha2_256, ovf::crypto::KeyUsage::verify},
      ovf::crypto::KeyFormat::raw, Bytes("key"));
  ASSERT_TRUE(key_result);
  auto key = std::move(key_result).value();
  const std::array<std::byte, 1> good{std::byte{0x42}};
  const std::array<std::byte, 1> bad{std::byte{0x41}};
  auto valid =
      runtime->Verify(ovf::crypto::Algorithm::ecdsa_p256_sha2_256, key, Bytes("message"), good);
  ASSERT_TRUE(valid);
  EXPECT_TRUE(valid.value());
  valid = runtime->Verify(ovf::crypto::Algorithm::ecdsa_p256_sha2_256, key, Bytes("message"), bad);
  ASSERT_TRUE(valid);
  EXPECT_FALSE(valid.value());
}

TEST(CryptoRuntimeTest, RejectsKeysFromAnotherRuntime) {
  auto first_result = ovf::crypto::Runtime::Create(kFactory);
  auto second_result = ovf::crypto::Runtime::Create(kFactory);
  ASSERT_TRUE(first_result);
  ASSERT_TRUE(second_result);
  auto first = std::move(first_result).value();
  auto second = std::move(second_result).value();
  auto key_result =
      first->ImportKey({ovf::crypto::Algorithm::hmac_sha2_256, ovf::crypto::KeyUsage::mac_generate},
                       ovf::crypto::KeyFormat::raw, Bytes("key"));
  ASSERT_TRUE(key_result);
  auto key = std::move(key_result).value();
  const auto result = second->Mac(ovf::crypto::Algorithm::hmac_sha2_256, key, Bytes("message"));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ovf::crypto::ErrorCode::invalid_argument);
}

TEST(CryptoRuntimeTest, RejectsIncompleteProviderAbi) {
  auto invalid = kFactory;
  invalid.struct_size = 1;
  const auto result = ovf::crypto::Runtime::Create(invalid);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ovf::crypto::ErrorCode::incompatible_abi);
}

} // namespace
