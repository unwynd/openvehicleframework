// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ovf/com/transport_abi.h"
#include "ovf/core/result.hpp"

namespace ovf::com {

namespace detail {
class RuntimeAccess;
}

enum class LogLevel { debug, info, warning, error };

using Logger = std::function<void(LogLevel, std::string_view)>;
/* A custom dispatcher follows docs/com-executor-contract.md. In particular it is bounded, FIFO,
   and serializes callbacks submitted for the same provider handle. */
using Dispatcher = std::function<bool(std::function<void()>)>;

struct RuntimeConfig {
  std::string instance_name;
  Logger logger;
  Dispatcher dispatcher;
};

struct TransportConfig {
  std::string configuration;
  std::uint32_t max_endpoints{256};
  std::uint32_t max_outstanding_operations{256};
  std::chrono::steady_clock::duration start_timeout{std::chrono::seconds(5)};
  std::chrono::steady_clock::duration stop_timeout{std::chrono::seconds(5)};
};

// Unified com error enum. Covers both lifecycle failures (returned from Runtime
// entry points as an enum) and method-call failures (embedded in MethodResult).
enum class Error : std::uint8_t {
  none,
  invalid_argument,
  incompatible_abi,
  duplicate_transport,
  invalid_state,
  provider_failure,
  unavailable,
  unsupported,
  resource_exhausted,
  not_found,
  cancelled,
  deadline_exceeded,
  shutting_down,
};

enum class TransportHealthState { initializing, ready, degraded, failed, stopped };
enum class DiagnosticOperation {
  provider,
  discovery,
  endpoint,
  subscription,
  publish,
  request,
  response
};

struct CommunicationDiagnostic {
  std::string provider;
  Error error;
  DiagnosticOperation operation_kind{DiagnosticOperation::provider};
  std::int64_t native_code{};
  std::uint64_t endpoint{};
  std::uint64_t operation{};
  std::string message;
};

struct TransportHealth {
  std::string provider;
  TransportHealthState state{TransportHealthState::stopped};
  std::uint64_t sequence{};
  CommunicationDiagnostic diagnostic;
};

struct TransportRegistration {
  std::string provider;
  TransportConfig config;
};

struct DeploymentConfig {
  std::string path;
};

class Runtime final {
public:
  explicit Runtime(RuntimeConfig config);
  ~Runtime();

  Runtime(Runtime&&) noexcept;
  Runtime& operator=(Runtime&&) noexcept;
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  [[nodiscard]] Error AddTransport(const ovf_com_transport_factory_v1& factory,
                                   TransportConfig config = {});

  [[nodiscard]] Error LoadTransport(std::string_view provider, TransportConfig config = {});

  [[nodiscard]] Error ConfigureDeployment(DeploymentConfig const& deployment);

  [[nodiscard]] Error Start();
  void Stop() noexcept;

  [[nodiscard]] bool IsRunning() const noexcept;
  [[nodiscard]] std::vector<std::string> TransportNames() const;
  [[nodiscard]] std::vector<TransportHealth> Health() const;
  void OnHealth(std::function<void(TransportHealth const&)> callback);
  void OnDiagnostic(std::function<void(CommunicationDiagnostic const&)> callback);

private:
  friend class detail::RuntimeAccess;
  class Impl;
  std::unique_ptr<Impl> impl_;
};

struct ApplicationRuntimeError final {
  Error code{Error::none};
  std::string message;
};

class ApplicationRuntime final {
public:
  ~ApplicationRuntime() = default;
  ApplicationRuntime(ApplicationRuntime const&) = delete;
  ApplicationRuntime& operator=(ApplicationRuntime const&) = delete;
  ApplicationRuntime(ApplicationRuntime&&) noexcept = default;
  ApplicationRuntime& operator=(ApplicationRuntime&&) noexcept = default;

  [[nodiscard]] auto get() noexcept -> Runtime& { return runtime_; }

private:
  friend ovf::core::Result<ApplicationRuntime, ApplicationRuntimeError>
      CreateApplicationRuntime(RuntimeConfig, std::vector<TransportRegistration>) noexcept;
  friend ovf::core::Result<ApplicationRuntime, ApplicationRuntimeError>
      CreateApplicationRuntime(RuntimeConfig, DeploymentConfig) noexcept;
  explicit ApplicationRuntime(Runtime runtime) noexcept : runtime_(std::move(runtime)) {}
  Runtime runtime_;
};

using ApplicationRuntimeResult = ovf::core::Result<ApplicationRuntime, ApplicationRuntimeError>;

[[nodiscard]] ApplicationRuntimeResult
CreateApplicationRuntime(RuntimeConfig config,
                         std::vector<TransportRegistration> transports) noexcept;
[[nodiscard]] ApplicationRuntimeResult
CreateApplicationRuntime(RuntimeConfig config, DeploymentConfig deployment) noexcept;

} // namespace ovf::com
