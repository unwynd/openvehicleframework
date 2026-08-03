// SPDX-License-Identifier: Apache-2.0

#include "ovf/per/per.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

namespace {

std::string Directory(std::string path) {
  const auto separator = path.find_last_of('/');
  return separator == std::string::npos ? std::string{} : path.substr(0, separator);
}

} // namespace

TEST(PerDynamicProviderTest, LoadsInstalledAbiProviderFromExplicitDirectory) {
  ASSERT_EQ(::testing::internal::GetArgvs().size(), 2U);
  const char* test_root = std::getenv("TEST_TMPDIR");
  ASSERT_NE(test_root, nullptr);
  std::string plugin = ::testing::internal::GetArgvs()[1];
  if (plugin.empty() || plugin.front() != '/') {
    const char* source_root = std::getenv("TEST_SRCDIR");
    const char* workspace = std::getenv("TEST_WORKSPACE");
    ASSERT_NE(source_root, nullptr);
    ASSERT_NE(workspace, nullptr);
    plugin = std::string(source_root) + "/" + workspace + "/" + plugin;
  }
  auto runtime =
      ovf::per::Runtime::LoadFrom("sqlite", Directory(plugin),
                                  {.configuration = "{\"root\":\"" + std::string(test_root) +
                                                    "/dynamic\",\"journal_mode\":\"wal\"}"});
  ASSERT_TRUE(runtime) << runtime.error().message;
  const auto capabilities = runtime.value()->GetCapabilities();
  ASSERT_TRUE(capabilities);
  EXPECT_TRUE(capabilities.value().persistent);
}
