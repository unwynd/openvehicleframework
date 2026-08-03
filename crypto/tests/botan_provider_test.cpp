// SPDX-License-Identifier: Apache-2.0

#include "ovf/crypto/crypto.hpp"

#include <botan/auto_rng.h>
#include <botan/pk_algs.h>
#include <botan/pubkey.h>
#include <botan/x509_ca.h>
#include <botan/x509self.h>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
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

std::span<const std::byte> Bytes(const std::vector<std::uint8_t>& value) {
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

TEST(BotanProviderTest, AgreesOnTheSameKeyAndRejectsMalformedCertificates) {
  auto runtime = CreateRuntime();
  ASSERT_NE(runtime, nullptr);
  auto first_result =
      runtime->GenerateKey({ovf::crypto::Algorithm::ecdh_p256, ovf::crypto::KeyUsage::derive});
  auto second_result =
      runtime->GenerateKey({ovf::crypto::Algorithm::ecdh_p256, ovf::crypto::KeyUsage::derive});
  ASSERT_TRUE(first_result) << first_result.error().message;
  ASSERT_TRUE(second_result) << second_result.error().message;
  auto first = std::move(first_result).value();
  auto second = std::move(second_result).value();
  const auto first_public = runtime->PublicValue(first);
  const auto second_public = runtime->PublicValue(second);
  ASSERT_TRUE(first_public) << first_public.error().message;
  ASSERT_TRUE(second_public) << second_public.error().message;
  const ovf::crypto::KeyPolicy session_policy{ovf::crypto::Algorithm::hmac_sha2_256,
                                              ovf::crypto::KeyUsage::mac_generate};
  auto first_secret = runtime->Agree(ovf::crypto::Algorithm::ecdh_p256, first,
                                     second_public.value(), Bytes("session-v1"), session_policy);
  auto second_secret = runtime->Agree(ovf::crypto::Algorithm::ecdh_p256, second,
                                      first_public.value(), Bytes("session-v1"), session_policy);
  ASSERT_TRUE(first_secret) << first_secret.error().message;
  ASSERT_TRUE(second_secret) << second_secret.error().message;
  auto first_session = std::move(first_secret).value();
  auto second_session = std::move(second_secret).value();
  const auto first_mac =
      runtime->Mac(ovf::crypto::Algorithm::hmac_sha2_256, first_session, Bytes("proof"));
  const auto second_mac =
      runtime->Mac(ovf::crypto::Algorithm::hmac_sha2_256, second_session, Bytes("proof"));
  ASSERT_TRUE(first_mac);
  ASSERT_TRUE(second_mac);
  EXPECT_EQ(first_mac.value(), second_mac.value());

  const std::array malformed{std::byte{0x30}, std::byte{0x01}, std::byte{0x00}};
  const std::array<std::span<const std::byte>, 1> anchors{malformed};
  const ovf::crypto::CertificateValidationRequest request{
      .leaf = malformed,
      .intermediates = {},
      .trust_anchors = anchors,
      .crls = {},
      .expected_name = {},
      .validation_time_unix_seconds = 1'700'000'000,
      .minimum_security_bits = 128,
      .usage = ovf::crypto::CertificateUsage::unspecified,
      .require_revocation = false,
      .require_self_signed_anchor = true,
  };
  const auto validation = runtime->ValidateCertificate(request);
  ASSERT_TRUE(validation) << validation.error().message;
  EXPECT_FALSE(validation.value().valid);
  EXPECT_EQ(validation.value().verdict, ovf::crypto::CertificateVerdict::malformed);
  const auto imported = runtime->ValidateAndImportPublicKey(
      request, {ovf::crypto::Algorithm::ecdsa_p256_sha2_256, ovf::crypto::KeyUsage::verify});
  ASSERT_TRUE(imported);
  EXPECT_FALSE(imported.value().validation.valid);
  EXPECT_EQ(imported.value().validation.verdict, ovf::crypto::CertificateVerdict::malformed);
  EXPECT_FALSE(imported.value().key.valid());
}

TEST(BotanProviderTest, ValidatesAChainAndImportsOnlyItsTrustedPublicKey) {
  Botan::AutoSeeded_RNG rng;
  auto root_key = Botan::create_private_key("ECDSA", rng, "secp256r1");
  auto leaf_key = Botan::create_private_key("ECDSA", rng, "secp256r1");
  ASSERT_NE(root_key, nullptr);
  ASSERT_NE(leaf_key, nullptr);
  Botan::X509_Cert_Options root_options("OVF Test Root");
  root_options.CA_key();
  const auto root = Botan::X509::create_self_signed_cert(root_options, *root_key, "SHA-256", rng);
  Botan::X509_Cert_Options leaf_options("radar.local");
  leaf_options.dns = "radar.local";
  const auto request = Botan::X509::create_cert_req(leaf_options, *leaf_key, "SHA-256", rng);
  const Botan::X509_CA authority(root, *root_key, "SHA-256", rng);
  const auto leaf = authority.sign_request(request, rng, Botan::X509_Time("250101000000Z"),
                                           Botan::X509_Time("300101000000Z"));
  const auto root_der = root.BER_encode();
  const auto leaf_der = leaf.BER_encode();
  const std::array<std::span<const std::byte>, 1> anchors{Bytes(root_der)};
  const auto validation_time =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
  const ovf::crypto::CertificateValidationRequest validation_request{
      .leaf = Bytes(leaf_der),
      .intermediates = {},
      .trust_anchors = anchors,
      .crls = {},
      .expected_name = "radar.local",
      .validation_time_unix_seconds = validation_time,
      .minimum_security_bits = 128,
      .usage = ovf::crypto::CertificateUsage::server_authentication,
      .require_revocation = false,
      .require_self_signed_anchor = true,
  };
  auto runtime = CreateRuntime();
  ASSERT_NE(runtime, nullptr);
  auto imported_result = runtime->ValidateAndImportPublicKey(
      validation_request,
      {ovf::crypto::Algorithm::ecdsa_p256_sha2_256, ovf::crypto::KeyUsage::verify});
  ASSERT_TRUE(imported_result) << imported_result.error().message;
  auto imported = std::move(imported_result).value();
  EXPECT_TRUE(imported.validation.valid) << imported.validation.native_status;
  EXPECT_EQ(imported.validation.verdict, ovf::crypto::CertificateVerdict::trusted);
  ASSERT_TRUE(imported.key.valid());

  Botan::PK_Signer signer(*leaf_key, rng, "EMSA1(SHA-256)", Botan::Signature_Format::DerSequence);
  const auto signature = signer.sign_message(
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>("trusted message"), 15),
      rng);
  const auto verified = runtime->Verify(ovf::crypto::Algorithm::ecdsa_p256_sha2_256, imported.key,
                                        Bytes("trusted message"), Bytes(signature));
  ASSERT_TRUE(verified);
  EXPECT_TRUE(verified.value());

  auto wrong_name_request = validation_request;
  wrong_name_request.expected_name = "other.local";
  const auto rejected = runtime->ValidateCertificate(wrong_name_request);
  ASSERT_TRUE(rejected);
  EXPECT_FALSE(rejected.value().valid);
  EXPECT_EQ(rejected.value().verdict, ovf::crypto::CertificateVerdict::name_mismatch);
}

TEST(BotanProviderTest, StreamsHashMacSignAndVerifyWithoutBufferingMessages) {
  auto runtime = CreateRuntime();
  ASSERT_NE(runtime, nullptr);

  auto hash_result = runtime->BeginHash(ovf::crypto::Algorithm::sha2_256);
  ASSERT_TRUE(hash_result) << hash_result.error().message;
  auto hash = std::move(hash_result).value();
  ASSERT_TRUE(hash.Update(Bytes("large ")));
  ASSERT_TRUE(hash.Update(Bytes("message")));
  const auto streamed_digest = hash.Finish();
  const auto direct_digest =
      runtime->Hash(ovf::crypto::Algorithm::sha2_256, Bytes("large message"));
  ASSERT_TRUE(streamed_digest);
  ASSERT_TRUE(direct_digest);
  EXPECT_EQ(streamed_digest.value(), direct_digest.value());
  EXPECT_FALSE(hash.valid());

  const auto key_material = Hex<32>("000102030405060708090a0b0c0d0e0f"
                                    "101112131415161718191a1b1c1d1e1f");
  auto mac_key_result = runtime->ImportKey(
      {ovf::crypto::Algorithm::hmac_sha2_256, ovf::crypto::KeyUsage::mac_generate},
      ovf::crypto::KeyFormat::raw, key_material);
  ASSERT_TRUE(mac_key_result);
  auto mac_key = std::move(mac_key_result).value();
  auto mac_result = runtime->BeginMac(ovf::crypto::Algorithm::hmac_sha2_256, mac_key);
  ASSERT_TRUE(mac_result);
  auto mac = std::move(mac_result).value();
  ASSERT_TRUE(mac.Update(Bytes("large ")));
  ASSERT_TRUE(mac.Update(Bytes("message")));
  const auto streamed_mac = mac.Finish();
  const auto direct_mac =
      runtime->Mac(ovf::crypto::Algorithm::hmac_sha2_256, mac_key, Bytes("large message"));
  ASSERT_TRUE(streamed_mac);
  ASSERT_TRUE(direct_mac);
  EXPECT_EQ(streamed_mac.value(), direct_mac.value());

  auto signing_key_result =
      runtime->GenerateKey({ovf::crypto::Algorithm::ed25519,
                            ovf::crypto::KeyUsage::sign | ovf::crypto::KeyUsage::verify});
  ASSERT_TRUE(signing_key_result);
  auto signing_key = std::move(signing_key_result).value();
  auto signer_result = runtime->BeginSign(ovf::crypto::Algorithm::ed25519, signing_key);
  ASSERT_TRUE(signer_result);
  auto signer = std::move(signer_result).value();
  ASSERT_TRUE(signer.Update(Bytes("large ")));
  ASSERT_TRUE(signer.Update(Bytes("message")));
  const auto signature = signer.Finish();
  ASSERT_TRUE(signature);

  auto verifier_result = runtime->BeginVerify(ovf::crypto::Algorithm::ed25519, signing_key);
  ASSERT_TRUE(verifier_result);
  auto verifier = std::move(verifier_result).value();
  ASSERT_TRUE(verifier.Update(Bytes("large ")));
  ASSERT_TRUE(verifier.Update(Bytes("message")));
  const auto valid = verifier.FinishVerify(signature.value());
  ASSERT_TRUE(valid);
  EXPECT_TRUE(valid.value());
  EXPECT_FALSE(verifier.valid());
}

TEST(BotanProviderTest, BoundsAndReclaimsStreamingContexts) {
  const auto* factory = ovf_crypto_backend_query_v1();
  ovf::crypto::RuntimeConfig config;
  config.max_keys = 2;
  config.max_contexts = 1;
  auto runtime_result = ovf::crypto::Runtime::Create(*factory, std::move(config));
  ASSERT_TRUE(runtime_result);
  auto runtime = std::move(runtime_result).value();
  auto first_result = runtime->BeginHash(ovf::crypto::Algorithm::sha2_256);
  ASSERT_TRUE(first_result);
  auto first = std::move(first_result).value();
  const auto exhausted = runtime->BeginHash(ovf::crypto::Algorithm::sha2_256);
  ASSERT_FALSE(exhausted);
  EXPECT_EQ(exhausted.error().code, ovf::crypto::ErrorCode::resource_exhausted);
  first.Cancel();
  EXPECT_FALSE(first.valid());
  const auto replacement = runtime->BeginHash(ovf::crypto::Algorithm::sha2_256);
  EXPECT_TRUE(replacement);
}

TEST(BotanProviderTest, AuthenticatesEveryAeadStreamRecordBeforeRelease) {
  auto runtime = CreateRuntime();
  ASSERT_NE(runtime, nullptr);
  const auto key_material = Hex<32>("000102030405060708090a0b0c0d0e0f"
                                    "101112131415161718191a1b1c1d1e1f");
  auto key_result =
      runtime->ImportKey({ovf::crypto::Algorithm::aes_256_gcm,
                          ovf::crypto::KeyUsage::encrypt | ovf::crypto::KeyUsage::decrypt},
                         ovf::crypto::KeyFormat::raw, key_material);
  ASSERT_TRUE(key_result);
  auto key = std::move(key_result).value();
  const auto nonce = Hex<12>("101112131415161718191a1b");
  const ovf::crypto::AeadParameters parameters{nonce, Bytes("record-stream-v1"), 16};
  auto encrypt_result =
      runtime->BeginRecordEncryption(ovf::crypto::Algorithm::aes_256_gcm, key, parameters);
  auto decrypt_result =
      runtime->BeginRecordDecryption(ovf::crypto::Algorithm::aes_256_gcm, key, parameters);
  ASSERT_TRUE(encrypt_result);
  ASSERT_TRUE(decrypt_result);
  auto encrypt = std::move(encrypt_result).value();
  auto decrypt = std::move(decrypt_result).value();

  const auto first_ciphertext = encrypt.Process(Bytes("first record"));
  const auto second_ciphertext = encrypt.Process(Bytes("second record"));
  ASSERT_TRUE(first_ciphertext);
  ASSERT_TRUE(second_ciphertext);
  EXPECT_NE(first_ciphertext.value(), second_ciphertext.value());
  const auto first_plaintext = decrypt.Process(first_ciphertext.value());
  const auto second_plaintext = decrypt.Process(second_ciphertext.value());
  ASSERT_TRUE(first_plaintext);
  ASSERT_TRUE(second_plaintext);
  EXPECT_EQ(first_plaintext.value(),
            std::vector<std::byte>(Bytes("first record").begin(), Bytes("first record").end()));
  EXPECT_EQ(second_plaintext.value(),
            std::vector<std::byte>(Bytes("second record").begin(), Bytes("second record").end()));

  auto rejecting_result =
      runtime->BeginRecordDecryption(ovf::crypto::Algorithm::aes_256_gcm, key, parameters);
  ASSERT_TRUE(rejecting_result);
  auto rejecting = std::move(rejecting_result).value();
  auto tampered = first_ciphertext.value();
  tampered.back() ^= std::byte{1};
  const auto rejected = rejecting.Process(tampered);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code, ovf::crypto::ErrorCode::authentication_failed);
}

TEST(BotanProviderTest, CompletesAsynchronousHashSignAndVerifyRequests) {
  using namespace std::chrono_literals;
  auto runtime = CreateRuntime();
  ASSERT_NE(runtime, nullptr);
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  auto hash_result =
      runtime->AsyncHash(ovf::crypto::Algorithm::sha2_256, Bytes("async message"), deadline);
  ASSERT_TRUE(hash_result) << hash_result.error().message;
  auto hash = std::move(hash_result).value();
  const auto async_digest = hash.Wait();
  const auto direct_digest =
      runtime->Hash(ovf::crypto::Algorithm::sha2_256, Bytes("async message"));
  ASSERT_TRUE(async_digest);
  ASSERT_TRUE(direct_digest);
  ASSERT_TRUE(std::holds_alternative<std::vector<std::byte>>(async_digest.value()));
  EXPECT_EQ(std::get<std::vector<std::byte>>(async_digest.value()), direct_digest.value());
  EXPECT_FALSE(hash.valid());

  auto key_result =
      runtime->GenerateKey({ovf::crypto::Algorithm::ed25519,
                            ovf::crypto::KeyUsage::sign | ovf::crypto::KeyUsage::verify});
  ASSERT_TRUE(key_result);
  auto key = std::move(key_result).value();
  auto sign_result =
      runtime->AsyncSign(ovf::crypto::Algorithm::ed25519, key, Bytes("async message"), deadline);
  ASSERT_TRUE(sign_result) << sign_result.error().message;
  auto sign = std::move(sign_result).value();
  const auto signature_result = sign.Wait();
  ASSERT_TRUE(signature_result);
  ASSERT_TRUE(std::holds_alternative<std::vector<std::byte>>(signature_result.value()));
  const auto& signature = std::get<std::vector<std::byte>>(signature_result.value());

  auto verify_result = runtime->AsyncVerify(ovf::crypto::Algorithm::ed25519, key,
                                            Bytes("async message"), signature, deadline);
  ASSERT_TRUE(verify_result) << verify_result.error().message;
  auto verify = std::move(verify_result).value();
  const auto valid = verify.Wait();
  ASSERT_TRUE(valid);
  ASSERT_TRUE(std::holds_alternative<bool>(valid.value()));
  EXPECT_TRUE(std::get<bool>(valid.value()));
}

TEST(BotanProviderTest, CancelsQueuedAsynchronousWorkExactlyOnce) {
  using namespace std::chrono_literals;
  const auto* factory = ovf_crypto_backend_query_v1();
  ovf::crypto::RuntimeConfig config;
  config.max_keys = 2;
  config.max_contexts = 2;
  auto runtime_result = ovf::crypto::Runtime::Create(*factory, std::move(config));
  ASSERT_TRUE(runtime_result);
  auto runtime = std::move(runtime_result).value();
  const std::vector<std::byte> input(8U * 1024U * 1024U, std::byte{0x5A});
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  auto first_result = runtime->AsyncHash(ovf::crypto::Algorithm::sha2_512, input, deadline);
  auto cancelled_result = runtime->AsyncHash(ovf::crypto::Algorithm::sha2_512, input, deadline);
  ASSERT_TRUE(first_result);
  ASSERT_TRUE(cancelled_result);
  auto first = std::move(first_result).value();
  auto cancelled = std::move(cancelled_result).value();
  EXPECT_TRUE(cancelled.Cancel());
  const auto outcome = cancelled.Wait();
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().code, ovf::crypto::ErrorCode::cancelled);
  const auto first_outcome = first.Wait();
  EXPECT_TRUE(first_outcome);
}

TEST(BotanProviderTest, EnforcesAsynchronousDeadlines) {
  using namespace std::chrono_literals;
  auto runtime = CreateRuntime();
  ASSERT_NE(runtime, nullptr);
  const std::vector<std::byte> input(8U * 1024U * 1024U, std::byte{0x33});
  auto operation_result = runtime->AsyncHash(ovf::crypto::Algorithm::sha2_512, input,
                                             std::chrono::steady_clock::now() + 1ns);
  if (!operation_result) {
    EXPECT_EQ(operation_result.error().code, ovf::crypto::ErrorCode::deadline_exceeded);
    return;
  }
  auto operation = std::move(operation_result).value();
  const auto outcome = operation.Wait();
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().code, ovf::crypto::ErrorCode::deadline_exceeded);
}

TEST(BotanProviderTest, DrainsCompletionsBeforeProviderShutdownReturns) {
  using namespace std::chrono_literals;
  auto runtime = CreateRuntime();
  ASSERT_NE(runtime, nullptr);
  const std::vector<std::byte> input(8U * 1024U * 1024U, std::byte{0x77});
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  auto first_result = runtime->AsyncHash(ovf::crypto::Algorithm::sha2_512, input, deadline);
  auto second_result = runtime->AsyncHash(ovf::crypto::Algorithm::sha2_512, input, deadline);
  ASSERT_TRUE(first_result);
  ASSERT_TRUE(second_result);
  auto first = std::move(first_result).value();
  auto second = std::move(second_result).value();
  runtime->Stop();
  const auto first_outcome = first.Wait();
  const auto second_outcome = second.Wait();
  const auto completed_or_stopped = [](const ovf::crypto::Result<ovf::crypto::AsyncValue>& result) {
    return result || result.error().code == ovf::crypto::ErrorCode::shutting_down;
  };
  EXPECT_TRUE(completed_or_stopped(first_outcome));
  EXPECT_TRUE(completed_or_stopped(second_outcome));
}

} // namespace
