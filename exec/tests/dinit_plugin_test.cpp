// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/backend_abi.h"
#include "ovf/exec/internal/backend_binding.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace {

using namespace ovf::exec;
using namespace ovf::exec::detail;

TEST(DinitPluginTest, BindsGeneratedConfigurationThroughTheBackendAbi) {
  const auto* path = std::getenv("OVF_TEST_EXECUTION_BACKEND");
  ASSERT_NE(path, nullptr);
  std::ifstream input(path, std::ios::binary);
  ASSERT_TRUE(input);
  const std::string configuration{std::istreambuf_iterator<char>{input}, {}};
  ASSERT_FALSE(configuration.empty());

  const auto* factory = ovf_exec_backend_query_v1();
  ASSERT_NE(factory, nullptr);
  auto bound = BindBackend(
      *factory, {.configuration = configuration, .required_parallel_operations = 4U, .logger = {}});
  ASSERT_TRUE(bound) << bound.error().message;

  auto observation = bound.value()->Inspect(ApplicationId{1});
  ASSERT_FALSE(observation);
  EXPECT_EQ(observation.error().code, ErrorCode::backend_error);
}

TEST(DinitPluginTest, RejectsHandwrittenOrMalformedConfiguration) {
  const auto* factory = ovf_exec_backend_query_v1();
  ASSERT_NE(factory, nullptr);
  auto bound = BindBackend(
      *factory,
      {.configuration = R"({"kind":"dinit"})", .required_parallel_operations = 1U, .logger = {}});
  ASSERT_FALSE(bound);
  EXPECT_EQ(bound.error().code, ErrorCode::invalid_argument);
}

} // namespace
