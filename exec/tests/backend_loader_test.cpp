// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/backend_loader.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <limits.h>
#include <string>
#include <string_view>

#include <stdlib.h>

namespace {

using namespace ovf::exec;
using namespace ovf::exec::detail;

TEST(BackendLoaderTest, LoadsVersionedBackendQueryFromSharedLibrary) {
  const auto* path = std::getenv("OVF_TEST_EXECUTION_PLUGIN");
  ASSERT_NE(path, nullptr);
  char resolved[PATH_MAX];
  ASSERT_NE(::realpath(path, resolved), nullptr);
  auto loaded = LoadBackendLibrary(resolved);
  ASSERT_TRUE(loaded) << loaded.error().message;
  const auto& factory = loaded.value().Factory();
  EXPECT_EQ(factory.abi_version, OVF_EXEC_BACKEND_ABI_VERSION_1);
  EXPECT_EQ(std::string_view(factory.name.data, factory.name.size), "dinit");
}

TEST(BackendLoaderTest, RejectsRelativeLibraryPath) {
  auto loaded = LoadBackendLibrary("libbackend.so");
  ASSERT_FALSE(loaded);
  EXPECT_EQ(loaded.error().code, ErrorCode::invalid_argument);
}

} // namespace
