// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/backend_binding.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using namespace ovf::exec;
using namespace ovf::exec::detail;

struct FakeBackend final {
  ovf_exec_backend_v1 abi{};
  const ovf_exec_host_api_v1* host{};
  std::vector<std::uint64_t> started;
  std::vector<std::uint64_t> stopped;
};

ovf_exec_status_v1 Capabilities(ovf_exec_backend_v1*, ovf_exec_capabilities_v1* value) {
  value->struct_size = sizeof(*value);
  value->max_parallel_operations = 4;
  value->supports_readiness = 1;
  value->supports_graceful_stop = 1;
  value->supports_exit_evidence = 1;
  value->supports_system_recovery = 1;
  return OVF_EXEC_STATUS_OK;
}

ovf_exec_status_v1 Inspect(ovf_exec_backend_v1*, std::uint64_t, ovf_exec_evidence_v1* evidence) {
  evidence->struct_size = sizeof(*evidence);
  evidence->state = OVF_EXEC_APPLICATION_STOPPED;
  return OVF_EXEC_STATUS_OK;
}

ovf_exec_status_v1 Start(ovf_exec_backend_v1* self, std::uint64_t application, std::uint64_t,
                         ovf_exec_evidence_v1* evidence) {
  auto& backend = *static_cast<FakeBackend*>(self->implementation);
  backend.started.push_back(application);
  evidence->struct_size = sizeof(*evidence);
  evidence->state = OVF_EXEC_APPLICATION_READY;
  evidence->message = {"ready", 5};
  backend.host->log(backend.host->user_data, OVF_EXEC_LOG_INFO, {"started", 7});
  return OVF_EXEC_STATUS_OK;
}

ovf_exec_status_v1 Stop(ovf_exec_backend_v1* self, std::uint64_t application,
                        ovf_exec_stop_reason_v1, std::uint64_t, ovf_exec_evidence_v1* evidence) {
  auto& backend = *static_cast<FakeBackend*>(self->implementation);
  backend.stopped.push_back(application);
  evidence->struct_size = sizeof(*evidence);
  evidence->state = OVF_EXEC_APPLICATION_STOPPED;
  return OVF_EXEC_STATUS_OK;
}

ovf_exec_status_v1 SystemRecovery(ovf_exec_backend_v1* self, std::uint64_t) {
  auto& backend = *static_cast<FakeBackend*>(self->implementation);
  backend.started.push_back(999U);
  return OVF_EXEC_STATUS_OK;
}

ovf_exec_status_v1 Create(const ovf_exec_host_api_v1* host, const ovf_exec_backend_config_v1*,
                          ovf_exec_backend_v1** result) {
  auto* backend = new FakeBackend;
  backend->host = host;
  backend->abi = {sizeof(ovf_exec_backend_v1),
                  OVF_EXEC_BACKEND_ABI_VERSION_1,
                  backend,
                  Capabilities,
                  Inspect,
                  Start,
                  Stop,
                  SystemRecovery};
  *result = &backend->abi;
  return OVF_EXEC_STATUS_OK;
}

void Destroy(ovf_exec_backend_v1* backend) {
  delete static_cast<FakeBackend*>(backend->implementation);
}

ovf_exec_backend_factory_v1 Factory() {
  return {sizeof(ovf_exec_backend_factory_v1),
          OVF_EXEC_BACKEND_ABI_VERSION_1,
          {"fake", 4},
          Create,
          Destroy};
}

TEST(BackendBindingTest, ValidatesAndCopiesBackendEvidence) {
  std::vector<std::string> logs;
  auto factory = Factory();
  auto bound = BindBackend(factory, {.configuration = "{}",
                                     .required_parallel_operations = 2,
                                     .logger = [&](BackendLogLevel, std::string_view message) {
                                       logs.emplace_back(message);
                                     }});
  ASSERT_TRUE(bound);
  auto backend = std::move(bound).value();

  auto started = backend->Start(ApplicationId{9}, std::chrono::steady_clock::now());
  ASSERT_TRUE(started);
  EXPECT_EQ(started.value().state, ApplicationState::ready);
  EXPECT_EQ(started.value().message, "ready");
  EXPECT_EQ(logs, std::vector<std::string>({"started"}));

  auto stopped =
      backend->Stop(ApplicationId{9}, StopReason::mode_change, std::chrono::steady_clock::now());
  ASSERT_TRUE(stopped);
  EXPECT_EQ(stopped.value().state, ApplicationState::stopped);

  auto recovery = backend->RequestSystemRecovery(std::chrono::steady_clock::now());
  ASSERT_TRUE(recovery);
}

TEST(BackendBindingTest, RejectsInsufficientCapabilities) {
  auto factory = Factory();
  auto result =
      BindBackend(factory, {.configuration = "", .required_parallel_operations = 8, .logger = {}});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::unsupported);
}

TEST(BackendBindingTest, NegotiatesSystemRecoveryCapability) {
  auto factory = Factory();
  auto result = BindBackend(factory, {.configuration = "",
                                      .required_parallel_operations = 1,
                                      .require_system_recovery = true,
                                      .logger = {}});
  ASSERT_TRUE(result) << result.error().message;
}

} // namespace
