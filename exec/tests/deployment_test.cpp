// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/deployment.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>

namespace {

using namespace ovf::exec;
using namespace ovf::exec::detail;

TEST(ExecutionDeploymentTest, LoadsGeneratedRuntimeConfiguration) {
  const auto* model = std::getenv("OVF_TEST_EXECUTION_MODEL");
  const auto* backend = std::getenv("OVF_TEST_EXECUTION_BACKEND");
  ASSERT_NE(model, nullptr);
  ASSERT_NE(backend, nullptr);

  auto loaded = LoadRuntimeDeployment(model, backend);
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(loaded.value().model.value().generation, ModelGeneration{1});
  EXPECT_NE(loaded.value().model.FindUnit(ApplicationId{3}), nullptr);
  const auto* bootstrap = loaded.value().model.FindUnit(ApplicationId{10});
  ASSERT_NE(bootstrap, nullptr);
  EXPECT_EQ(bootstrap->kind, ExecutionUnitKind::mount);
  EXPECT_TRUE(bootstrap->bootstrap);
  const auto* network = loaded.value().model.FindUnit(ApplicationId{11});
  ASSERT_NE(network, nullptr);
  EXPECT_EQ(network->kind, ExecutionUnitKind::service);
  EXPECT_EQ(network->dependencies, (std::vector<ApplicationId>{ApplicationId{10}}));
  EXPECT_NE(loaded.value().model.FindMode({DomainId{2}, ModeId{2}}), nullptr);
  EXPECT_EQ(loaded.value().backend_kind, "dinit");
  EXPECT_EQ(loaded.value().backend_library, "/usr/lib/libovf_exec_backend_dinit.so");
  EXPECT_EQ(loaded.value().journal.path, "/var/lib/ovf/exec/journal.v1");
  EXPECT_EQ(loaded.value().journal.maximum_record_size, 1048576U);
  EXPECT_TRUE(loaded.value().journal.synchronize);
  EXPECT_EQ(loaded.value().coordinator.socket, "/run/ovf/exec/coordinator.sock");
  EXPECT_EQ(loaded.value().coordinator.limits.queue_capacity, 128U);
  EXPECT_EQ(loaded.value().coordinator.limits.worker_count, 4U);
  EXPECT_EQ(loaded.value().coordinator.connection_capacity, 128U);
  EXPECT_EQ(loaded.value().coordinator.maximum_message_size, 1048576U);
  EXPECT_EQ(loaded.value().coordinator.observation_uids, (std::vector<std::uint32_t>{0U}));
  EXPECT_EQ(loaded.value().coordinator.mutation_uids, (std::vector<std::uint32_t>{0U}));
}

TEST(ExecutionDeploymentTest, RejectsMissingArtifacts) {
  auto loaded = LoadRuntimeDeployment("/path/does/not/exist", "/path/does/not/exist-either");
  ASSERT_FALSE(loaded);
  EXPECT_EQ(loaded.error().code, ErrorCode::configuration_error);
}

} // namespace
