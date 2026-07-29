// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/backend_abi.h"
#include "ovf/exec/backends/dinit.hpp"

#include <chrono>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace ovf::exec;

struct PluginBackend final {
  ovf_exec_backend_v1 api{};
  std::unique_ptr<ovf::exec::detail::ProcessBackend> backend;
};

thread_local std::string evidence_message;

PluginBackend* Implementation(ovf_exec_backend_v1* self) noexcept {
  return self == nullptr ? nullptr : static_cast<PluginBackend*>(self->implementation);
}

ovf_exec_status_v1 Status(const Error& error) noexcept {
  switch (error.code) {
  case ErrorCode::invalid_argument:
  case ErrorCode::invalid_identifier:
  case ErrorCode::configuration_error:
    return OVF_EXEC_STATUS_INVALID_ARGUMENT;
  case ErrorCode::invalid_transition:
  case ErrorCode::already_exists:
    return OVF_EXEC_STATUS_INVALID_STATE;
  case ErrorCode::not_found:
    return OVF_EXEC_STATUS_NOT_FOUND;
  case ErrorCode::permission_denied:
    return OVF_EXEC_STATUS_PERMISSION_DENIED;
  case ErrorCode::busy:
  case ErrorCode::superseded:
    return OVF_EXEC_STATUS_BUSY;
  case ErrorCode::cancelled:
    return OVF_EXEC_STATUS_CANCELLED;
  case ErrorCode::deadline_exceeded:
    return OVF_EXEC_STATUS_DEADLINE_EXCEEDED;
  case ErrorCode::resource_exhausted:
    return OVF_EXEC_STATUS_RESOURCE_EXHAUSTED;
  case ErrorCode::unsupported:
  case ErrorCode::incompatible_abi:
    return OVF_EXEC_STATUS_UNSUPPORTED;
  case ErrorCode::none:
    return OVF_EXEC_STATUS_OK;
  default:
    return OVF_EXEC_STATUS_BACKEND_ERROR;
  }
}

ovf_exec_application_state_v1 State(ApplicationState state) noexcept {
  switch (state) {
  case ApplicationState::starting:
    return OVF_EXEC_APPLICATION_STARTING;
  case ApplicationState::ready:
    return OVF_EXEC_APPLICATION_READY;
  case ApplicationState::stopping:
    return OVF_EXEC_APPLICATION_STOPPING;
  case ApplicationState::stopped:
    return OVF_EXEC_APPLICATION_STOPPED;
  case ApplicationState::failed:
    return OVF_EXEC_APPLICATION_FAILED;
  case ApplicationState::killed:
    return OVF_EXEC_APPLICATION_KILLED;
  case ApplicationState::unavailable:
    return OVF_EXEC_APPLICATION_UNAVAILABLE;
  case ApplicationState::unknown:
  default:
    return OVF_EXEC_APPLICATION_UNKNOWN;
  }
}

StopReason Reason(ovf_exec_stop_reason_v1 reason) noexcept {
  switch (reason) {
  case OVF_EXEC_STOP_MODE_CHANGE:
    return StopReason::mode_change;
  case OVF_EXEC_STOP_SYSTEM_SHUTDOWN:
    return StopReason::system_shutdown;
  case OVF_EXEC_STOP_RESTART:
    return StopReason::restart;
  case OVF_EXEC_STOP_SUPERVISOR_REQUEST:
    return StopReason::supervisor_request;
  case OVF_EXEC_STOP_DEPENDENCY_FAILURE:
    return StopReason::dependency_failure;
  case OVF_EXEC_STOP_RECOVERY:
    return StopReason::recovery;
  case OVF_EXEC_STOP_UNKNOWN:
  default:
    return StopReason::unknown;
  }
}

Deadline Time(std::uint64_t nanoseconds) noexcept {
  return Deadline{std::chrono::nanoseconds{nanoseconds}};
}

ovf_exec_status_v1 Evidence(Result<ovf::exec::detail::BackendEvidence> result,
                            ovf_exec_evidence_v1* output) noexcept {
  if (!result) {
    return Status(result.error());
  }
  if (output == nullptr || output->struct_size < sizeof(ovf_exec_evidence_v1)) {
    return OVF_EXEC_STATUS_INVALID_ARGUMENT;
  }
  evidence_message = result.value().message;
  output->state = State(result.value().state);
  output->exit_code = result.value().exit_code;
  output->signal = result.value().signal;
  output->native_code = result.value().native_code;
  output->message = {evidence_message.data(), evidence_message.size()};
  return OVF_EXEC_STATUS_OK;
}

ovf_exec_status_v1 Capabilities(ovf_exec_backend_v1* self, ovf_exec_capabilities_v1* output) {
  if (Implementation(self) == nullptr || output == nullptr ||
      output->struct_size < sizeof(ovf_exec_capabilities_v1)) {
    return OVF_EXEC_STATUS_INVALID_ARGUMENT;
  }
  *output = {
      sizeof(ovf_exec_capabilities_v1), 64U, 1U, 1U, 1U, 0U, 0U, {0U, 0U, 0U},
  };
  return OVF_EXEC_STATUS_OK;
}

ovf_exec_status_v1 Inspect(ovf_exec_backend_v1* self, std::uint64_t application,
                           ovf_exec_evidence_v1* output) {
  auto* plugin = Implementation(self);
  return plugin == nullptr || application == 0U
             ? OVF_EXEC_STATUS_INVALID_ARGUMENT
             : Evidence(plugin->backend->Inspect(ApplicationId{application}), output);
}

ovf_exec_status_v1 Start(ovf_exec_backend_v1* self, std::uint64_t application,
                         std::uint64_t deadline, ovf_exec_evidence_v1* output) {
  auto* plugin = Implementation(self);
  return plugin == nullptr || application == 0U || deadline == 0U
             ? OVF_EXEC_STATUS_INVALID_ARGUMENT
             : Evidence(plugin->backend->Start(ApplicationId{application}, Time(deadline)), output);
}

ovf_exec_status_v1 Stop(ovf_exec_backend_v1* self, std::uint64_t application,
                        ovf_exec_stop_reason_v1 reason, std::uint64_t deadline,
                        ovf_exec_evidence_v1* output) {
  auto* plugin = Implementation(self);
  return plugin == nullptr || application == 0U || deadline == 0U
             ? OVF_EXEC_STATUS_INVALID_ARGUMENT
             : Evidence(plugin->backend->Stop(ApplicationId{application}, Reason(reason),
                                              Time(deadline)),
                        output);
}

ovf_exec_status_v1 Create(const ovf_exec_host_api_v1* host,
                          const ovf_exec_backend_config_v1* config, ovf_exec_backend_v1** output) {
  if (host == nullptr || host->struct_size < sizeof(ovf_exec_host_api_v1) || config == nullptr ||
      config->struct_size < sizeof(ovf_exec_backend_config_v1) ||
      config->configuration.data == nullptr || output == nullptr ||
      config->required_parallel_operations > 64U) {
    return OVF_EXEC_STATUS_INVALID_ARGUMENT;
  }
  auto parsed = ovf::exec::backends::ParseDinitConfig(
      {config->configuration.data, config->configuration.size});
  if (!parsed) {
    return Status(parsed.error());
  }
  auto backend = ovf::exec::backends::CreateDinitBackend(std::move(parsed).value());
  if (!backend) {
    return Status(backend.error());
  }
  auto* plugin = new (std::nothrow) PluginBackend;
  if (plugin == nullptr) {
    return OVF_EXEC_STATUS_RESOURCE_EXHAUSTED;
  }
  plugin->backend = std::move(backend).value();
  plugin->api = {
      sizeof(ovf_exec_backend_v1),
      OVF_EXEC_BACKEND_ABI_VERSION_1,
      plugin,
      Capabilities,
      Inspect,
      Start,
      Stop,
  };
  *output = &plugin->api;
  return OVF_EXEC_STATUS_OK;
}

void Destroy(ovf_exec_backend_v1* backend) { delete Implementation(backend); }

constexpr char kName[] = "dinit";
const ovf_exec_backend_factory_v1 kFactory{
    sizeof(ovf_exec_backend_factory_v1),
    OVF_EXEC_BACKEND_ABI_VERSION_1,
    {kName, sizeof(kName) - 1U},
    Create,
    Destroy,
};

} // namespace

extern "C" const ovf_exec_backend_factory_v1* ovf_exec_backend_query_v1(void) { return &kFactory; }
