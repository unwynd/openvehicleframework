// SPDX-License-Identifier: Apache-2.0

#include "ovf/crypto/crypto.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

extern "C" const ovf_crypto_backend_factory_v1* ovf_crypto_backend_query_v1(void);

namespace {

std::span<const std::byte> Bytes(std::string_view value) {
  return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

template <std::size_t Size> std::array<std::byte, Size> Hex(std::string_view value) {
  std::array<std::byte, Size> output{};
  for (std::size_t index = 0; index < Size; ++index) {
    const auto digit = [](char character) -> std::uint8_t {
      if (character >= '0' && character <= '9') {
        return static_cast<std::uint8_t>(character - '0');
      }
      return static_cast<std::uint8_t>(character - 'a' + 10);
    };
    output[index] =
        static_cast<std::byte>((digit(value[index * 2]) << 4U) | digit(value[index * 2 + 1]));
  }
  return output;
}

std::unique_ptr<ovf::crypto::Runtime> CreateRuntime() {
  const auto* factory = ovf_crypto_backend_query_v1();
  EXPECT_NE(factory, nullptr);
  ovf::crypto::RuntimeConfig config;
  config.max_keys = 16;
  config.max_contexts = 8;
  auto result = ovf::crypto::Runtime::Create(*factory, std::move(config));
  EXPECT_TRUE(result) << (result ? "" : result.error().message);
  return result ? std::move(result).value() : nullptr;
}

TEST(BotanProviderTest, MatchesSha256AndHmacSha256KnownAnswers) {
  auto runtime = CreateRuntime();
  ASSERT_NE(runtime, nullptr);

  const auto digest = runtime->Hash(ovf::crypto::Algorithm::sha2_256, Bytes("abc"));
  ASSERT_TRUE(digest) << digest.error().message;
  const auto expected_digest =
      Hex<32>("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(digest.value(), std::vector<std::byte>(expected_digest.begin(), expected_digest.end()));

  const auto key_material = Hex<32>("000102030405060708090a0b0c0d0e0f"
                                    "101112131415161718191a1b1c1d1e1f");
  auto key_result = runtime->ImportKey(
      {ovf::crypto::Algorithm::hmac_sha2_256, ovf::crypto::KeyUsage::mac_generate},
      ovf::crypto::KeyFormat::raw, key_material);
  ASSERT_TRUE(key_result) << key_result.error().message;
  auto key = std::move(key_result).value();
  const auto mac = runtime->Mac(ovf::crypto::Algorithm::hmac_sha2_256, key, Bytes("abc"));
  ASSERT_TRUE(mac) << mac.error().message;
  const auto expected_mac = Hex<32>("f0133729c4163dede81e21cd47839256"
                                    "da58171238c8a0d874397c73b14e1e47");
  EXPECT_EQ(mac.value(), std::vector<std::byte>(expected_mac.begin(), expected_mac.end()));
}

TEST(BotanProviderTest, AuthenticatedEncryptionRejectsTampering) {
  auto runtime = CreateRuntime();
  ASSERT_NE(runtime, nullptr);
  const auto key_material = Hex<32>("000102030405060708090a0b0c0d0e0f"
                                    "101112131415161718191a1b1c1d1e1f");
  auto key_result =
      runtime->ImportKey({ovf::crypto::Algorithm::aes_256_gcm,
                          ovf::crypto::KeyUsage::encrypt | ovf::crypto::KeyUsage::decrypt},
                         ovf::crypto::KeyFormat::raw, key_material);
  ASSERT_TRUE(key_result) << key_result.error().message;
  auto key = std::move(key_result).value();
  const auto nonce = Hex<12>("101112131415161718191a1b");
  const auto associated_data = Bytes("vehicle-message-v1");
  const ovf::crypto::AeadParameters parameters{nonce, associated_data, 16};
  const auto ciphertext = runtime->Encrypt(ovf::crypto::Algorithm::aes_256_gcm, key, parameters,
                                           Bytes("authenticated payload"));
  ASSERT_TRUE(ciphertext) << ciphertext.error().message;
  const auto plaintext =
      runtime->Decrypt(ovf::crypto::Algorithm::aes_256_gcm, key, parameters, ciphertext.value());
  ASSERT_TRUE(plaintext) << plaintext.error().message;
  EXPECT_EQ(plaintext.value(), std::vector<std::byte>(Bytes("authenticated payload").begin(),
                                                      Bytes("authenticated payload").end()));

  auto tampered = ciphertext.value();
  tampered.front() ^= std::byte{1};
  const auto rejected =
      runtime->Decrypt(ovf::crypto::Algorithm::aes_256_gcm, key, parameters, tampered);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code, ovf::crypto::ErrorCode::authentication_failed);
}

TEST(BotanProviderTest, DerivesKeysAndSeparatesSignatureMismatchFromProviderErrors) {
  auto runtime = CreateRuntime();
  ASSERT_NE(runtime, nullptr);
  const auto input_key = Hex<32>("000102030405060708090a0b0c0d0e0f"
                                 "101112131415161718191a1b1c1d1e1f");
  auto derivation_key_result =
      runtime->ImportKey({ovf::crypto::Algorithm::hkdf_sha2_256, ovf::crypto::KeyUsage::derive},
                         ovf::crypto::KeyFormat::raw, input_key);
  ASSERT_TRUE(derivation_key_result) << derivation_key_result.error().message;
  auto derivation_key = std::move(derivation_key_result).value();
  const auto derived = runtime->Derive(ovf::crypto::Algorithm::hkdf_sha2_256, derivation_key,
                                       Bytes("salt"), Bytes("context"), 32);
  ASSERT_TRUE(derived) << derived.error().message;
  EXPECT_EQ(derived.value().size(), 32U);

  constexpr std::array signing_algorithms{
      ovf::crypto::Algorithm::ecdsa_p256_sha2_256,
      ovf::crypto::Algorithm::rsa_pss_sha2_256,
      ovf::crypto::Algorithm::ed25519,
  };
  for (const auto algorithm : signing_algorithms) {
    auto signing_key_result = runtime->GenerateKey(
        {algorithm, ovf::crypto::KeyUsage::sign | ovf::crypto::KeyUsage::verify});
    ASSERT_TRUE(signing_key_result) << signing_key_result.error().message;
    auto signing_key = std::move(signing_key_result).value();
    const auto signature = runtime->Sign(algorithm, signing_key, Bytes("decision"));
    ASSERT_TRUE(signature) << signature.error().message;
    const auto valid =
        runtime->Verify(algorithm, signing_key, Bytes("decision"), signature.value());
    ASSERT_TRUE(valid) << valid.error().message;
    EXPECT_TRUE(valid.value());
    const auto invalid =
        runtime->Verify(algorithm, signing_key, Bytes("altered"), signature.value());
    ASSERT_TRUE(invalid) << invalid.error().message;
    EXPECT_FALSE(invalid.value());
  }
}

} // namespace
