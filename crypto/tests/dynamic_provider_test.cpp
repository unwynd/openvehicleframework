// SPDX-License-Identifier: Apache-2.0

#include "ovf/crypto/crypto.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

namespace {

TEST(DynamicProviderTest, LoadsBotanThroughTheVersionedAbi) {
  const char* runfiles = std::getenv("TEST_SRCDIR");
  ASSERT_NE(runfiles, nullptr);
  const std::string provider_directory = std::string(runfiles) + "/_main/crypto";
  ASSERT_EQ(setenv("OVF_CRYPTO_PROVIDER_PATH", provider_directory.c_str(), 1), 0);
  ovf::crypto::RuntimeConfig config;
  config.configuration = "{}";
  auto runtime_result = ovf::crypto::Runtime::Load("botan", std::move(config));
  ASSERT_TRUE(runtime_result) << runtime_result.error().message;
  auto runtime = std::move(runtime_result).value();
  const auto random = runtime->Random(32);
  ASSERT_TRUE(random) << random.error().message;
  EXPECT_EQ(random.value().size(), 32U);
}

} // namespace
