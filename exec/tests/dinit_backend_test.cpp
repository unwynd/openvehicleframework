// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/backends/dinit.hpp"

#include <gtest/gtest.h>

namespace {

using namespace ovf::exec;
using namespace ovf::exec::backends;

TEST(DinitBackendTest, ValidatesConfigurationBeforeConnecting) {
  auto relative = CreateDinitBackend({"relative", {{ApplicationId{1}, "camera"}}});
  ASSERT_FALSE(relative);
  EXPECT_EQ(relative.error().code, ErrorCode::invalid_argument);

  auto empty = CreateDinitBackend({"/run/dinitctl", {}});
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error().code, ErrorCode::invalid_argument);

  auto invalid_id = CreateDinitBackend({"/run/dinitctl", {{ApplicationId{}, "camera"}}});
  ASSERT_FALSE(invalid_id);
  EXPECT_EQ(invalid_id.error().code, ErrorCode::invalid_argument);

  auto invalid_name = CreateDinitBackend({"/run/dinitctl", {{ApplicationId{1}, "../camera"}}});
  ASSERT_FALSE(invalid_name);
  EXPECT_EQ(invalid_name.error().code, ErrorCode::invalid_argument);
}

TEST(DinitBackendTest, ReportsUnavailableControlSocket) {
  auto created =
      CreateDinitBackend({"/path/that/does/not/exist/dinitctl", {{ApplicationId{1}, "camera"}}});
  ASSERT_TRUE(created);
  auto backend = std::move(created).value();
  auto status = backend->Inspect(ApplicationId{1});
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code, ErrorCode::backend_unavailable);
}

TEST(DinitBackendTest, RejectsUnmappedApplicationsWithoutConnecting) {
  auto created = CreateDinitBackend({"/run/dinitctl", {{ApplicationId{1}, "camera"}}});
  ASSERT_TRUE(created);
  auto backend = std::move(created).value();
  auto status = backend->Inspect(ApplicationId{2});
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code, ErrorCode::not_found);
}

} // namespace
