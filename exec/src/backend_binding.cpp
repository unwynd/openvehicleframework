// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/backend_binding.hpp"

#include <chrono>
#include <utility>

namespace ovf::exec::detail {
namespace {

std::string_view View(ovf_exec_string_view_v1 value) {
  return {value.data == nullptr ? "" : value.data, value.data == nullptr ? 0U : value.size};
}

Error StatusError(ovf_exec_status_v1 status, std::string operation) {
  ErrorCode code{ErrorCode::backend_error};
  switch (status) {
  case OVF_EXEC_STATUS_INVALID_ARGUMENT:
    code = ErrorCode::invalid_argument;
    break;
  case OVF_EXEC_STATUS_INVALID_STATE:
    code = ErrorCode::invalid_transition;
    break;
  case OVF_EXEC_STATUS_NOT_FOUND:
    code = ErrorCode::not_found;
    break;
  case OVF_EXEC_STATUS_PERMISSION_DENIED:
    code = ErrorCode::permission_denied;
    break;
  case OVF_EXEC_STATUS_BUSY:
    code = ErrorCode::busy;
    break;
  case OVF_EXEC_STATUS_CANCELLED:
    code = ErrorCode::cancelled;
    break;
  case OVF_EXEC_STATUS_DEADLINE_EXCEEDED:
    code = ErrorCode::deadline_exceeded;
    break;
  case OVF_EXEC_STATUS_RESOURCE_EXHAUSTED:
    code = ErrorCode::resource_exhausted;
    break;
  case OVF_EXEC_STATUS_UNSUPPORTED:
    code = ErrorCode::unsupported;
    break;
  case OVF_EXEC_STATUS_OK:
    code = ErrorCode::none;
    break;
  case OVF_EXEC_STATUS_BACKEND_ERROR:
  default:
    code = ErrorCode::backend_error;
    break;
  }
  return MakeError(code, std::move(operation) + " failed", static_cast<std::uint64_t>(status));
}

ApplicationState State(ovf_exec_application_state_v1 state) {
  switch (state) {
  case OVF_EXEC_APPLICATION_STARTING:
    return ApplicationState::starting;
  case OVF_EXEC_APPLICATION_READY:
    return ApplicationState::ready;
  case OVF_EXEC_APPLICATION_STOPPING:
    return ApplicationState::stopping;
  case OVF_EXEC_APPLICATION_STOPPED:
    return ApplicationState::stopped;
  case OVF_EXEC_APPLICATION_FAILED:
    return ApplicationState::failed;
  case OVF_EXEC_APPLICATION_KILLED:
    return ApplicationState::killed;
  case OVF_EXEC_APPLICATION_UNAVAILABLE:
    return ApplicationState::unavailable;
  case OVF_EXEC_APPLICATION_UNKNOWN:
  default:
    return ApplicationState::unknown;
  }
}

ovf_exec_stop_reason_v1 Reason(StopReason reason) {
  switch (reason) {
  case StopReason::mode_change:
    return OVF_EXEC_STOP_MODE_CHANGE;
  case StopReason::system_shutdown:
    return OVF_EXEC_STOP_SYSTEM_SHUTDOWN;
  case StopReason::restart:
    return OVF_EXEC_STOP_RESTART;
  case StopReason::supervisor_request:
    return OVF_EXEC_STOP_SUPERVISOR_REQUEST;
  case StopReason::dependency_failure:
    return OVF_EXEC_STOP_DEPENDENCY_FAILURE;
  case StopReason::recovery:
    return OVF_EXEC_STOP_RECOVERY;
  case StopReason::none:
  case StopReason::unknown:
  default:
    return OVF_EXEC_STOP_UNKNOWN;
  }
}

struct HostContext final {
  BackendLogger logger;
  ovf_exec_host_api_v1 api{};
};

std::uint64_t DeadlineNanoseconds(Deadline deadline) {
  const auto value =
      std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count();
  return value <= 0 ? 0U : static_cast<std::uint64_t>(value);
}

class AbiBackend final : public ProcessBackend {
public:
  AbiBackend(const ovf_exec_backend_factory_v1& factory, ovf_exec_backend_v1* backend,
             std::unique_ptr<HostContext> host)
      : factory_(factory), backend_(backend), host_(std::move(host)) {}

  ~AbiBackend() override { factory_.destroy(backend_); }

  Result<BackendEvidence> Inspect(ApplicationId application) noexcept override {
    return Invoke("inspect", [&](auto* evidence) {
      return backend_->inspect(backend_, application.value(), evidence);
    });
  }

  Result<BackendEvidence> Start(ApplicationId application, Deadline deadline) noexcept override {
    return Invoke("start", [&](auto* evidence) {
      return backend_->start(backend_, application.value(), DeadlineNanoseconds(deadline),
                             evidence);
    });
  }

  Result<BackendEvidence> Stop(ApplicationId application, StopReason reason,
                               Deadline deadline) noexcept override {
    return Invoke("stop", [&](auto* evidence) {
      return backend_->stop(backend_, application.value(), Reason(reason),
                            DeadlineNanoseconds(deadline), evidence);
    });
  }

private:
  template <typename Operation>
  Result<BackendEvidence> Invoke(std::string operation, Operation invoke) noexcept {
    ovf_exec_evidence_v1 evidence{};
    evidence.struct_size = sizeof(evidence);
    const auto status = invoke(&evidence);
    if (status != OVF_EXEC_STATUS_OK) {
      return StatusError(status, std::move(operation));
    }
    if (evidence.struct_size < sizeof(ovf_exec_evidence_v1)) {
      return MakeError(ErrorCode::incompatible_abi,
                       "backend returned a truncated evidence structure");
    }
    try {
      return BackendEvidence{State(evidence.state), evidence.exit_code, evidence.signal,
                             evidence.native_code, std::string(View(evidence.message))};
    } catch (...) {
      return MakeError(ErrorCode::resource_exhausted, "backend evidence could not be copied");
    }
  }

  ovf_exec_backend_factory_v1 factory_;
  ovf_exec_backend_v1* backend_;
  std::unique_ptr<HostContext> host_;
};

} // namespace

Result<std::unique_ptr<ProcessBackend>> BindBackend(const ovf_exec_backend_factory_v1& factory,
                                                    BackendBindingConfig config) {
  if (factory.struct_size < sizeof(ovf_exec_backend_factory_v1) ||
      factory.abi_version != OVF_EXEC_BACKEND_ABI_VERSION_1 || factory.name.data == nullptr ||
      factory.name.size == 0U || factory.create == nullptr || factory.destroy == nullptr) {
    return MakeError(ErrorCode::incompatible_abi, "execution backend factory is incompatible");
  }

  std::unique_ptr<HostContext> context;
  try {
    context = std::make_unique<HostContext>();
    context->logger = config.logger;
  } catch (...) {
    return MakeError(ErrorCode::resource_exhausted, "execution backend host allocation failed");
  }
  context->api.struct_size = sizeof(context->api);
  context->api.user_data = context.get();
  context->api.log = [](void* user_data, ovf_exec_log_level_v1 level,
                        ovf_exec_string_view_v1 message) {
    auto& current = *static_cast<HostContext*>(user_data);
    if (!current.logger) {
      return;
    }
    BackendLogLevel mapped{BackendLogLevel::info};
    switch (level) {
    case OVF_EXEC_LOG_DEBUG:
      mapped = BackendLogLevel::debug;
      break;
    case OVF_EXEC_LOG_WARNING:
      mapped = BackendLogLevel::warning;
      break;
    case OVF_EXEC_LOG_ERROR:
      mapped = BackendLogLevel::error;
      break;
    case OVF_EXEC_LOG_INFO:
    default:
      break;
    }
    current.logger(mapped, View(message));
  };
  context->api.monotonic_time_ns = [](void*) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
  };
  const ovf_exec_backend_config_v1 abi_config{
      sizeof(ovf_exec_backend_config_v1),
      {config.configuration.data(), config.configuration.size()},
      config.required_parallel_operations};
  ovf_exec_backend_v1* backend = nullptr;
  const auto created = factory.create(&context->api, &abi_config, &backend);
  if (created != OVF_EXEC_STATUS_OK) {
    return StatusError(created, "backend creation");
  }
  if (backend == nullptr || backend->struct_size < sizeof(ovf_exec_backend_v1) ||
      backend->abi_version != OVF_EXEC_BACKEND_ABI_VERSION_1 ||
      backend->get_capabilities == nullptr || backend->inspect == nullptr ||
      backend->start == nullptr || backend->stop == nullptr) {
    if (backend != nullptr) {
      factory.destroy(backend);
    }
    return MakeError(ErrorCode::incompatible_abi, "execution backend is incompatible");
  }

  ovf_exec_capabilities_v1 capabilities{};
  capabilities.struct_size = sizeof(capabilities);
  const auto queried = backend->get_capabilities(backend, &capabilities);
  if (queried != OVF_EXEC_STATUS_OK || capabilities.struct_size < sizeof(capabilities)) {
    factory.destroy(backend);
    return queried == OVF_EXEC_STATUS_OK
               ? MakeError(ErrorCode::incompatible_abi, "backend returned truncated capabilities")
               : StatusError(queried, "capability query");
  }
  if (capabilities.max_parallel_operations < config.required_parallel_operations ||
      capabilities.supports_readiness == 0U || capabilities.supports_graceful_stop == 0U) {
    factory.destroy(backend);
    return MakeError(ErrorCode::unsupported,
                     "backend does not satisfy required execution capabilities");
  }

  try {
    auto owned = std::make_unique<AbiBackend>(factory, backend, std::move(context));
    return std::unique_ptr<ProcessBackend>(std::move(owned));
  } catch (...) {
    factory.destroy(backend);
    return MakeError(ErrorCode::resource_exhausted, "execution backend adapter allocation failed");
  }
}

} // namespace ovf::exec::detail
