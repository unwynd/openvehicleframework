// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ovf/com/transport_abi.h"

namespace ovf::com {

namespace detail {
class RuntimeAccess;
}

enum class LogLevel { debug, info, warning, error };

using Logger = std::function<void(LogLevel, std::string_view)>;
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
};

enum class RuntimeError {
  none,
  invalid_argument,
  incompatible_abi,
  duplicate_transport,
  invalid_state,
  transport_error,
  unsupported,
  resource_exhausted,
  not_found,
  cancelled,
  deadline_exceeded,
  shutting_down
};

class Runtime final {
public:
  explicit Runtime(RuntimeConfig config);
  ~Runtime();

  Runtime(Runtime&&) noexcept;
  Runtime& operator=(Runtime&&) noexcept;
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  [[nodiscard]] RuntimeError AddTransport(const ovf_com_transport_factory_v1& factory,
                                          TransportConfig config = {});

  [[nodiscard]] RuntimeError LoadTransport(std::string_view provider, TransportConfig config = {});

  [[nodiscard]] RuntimeError Start();
  void Stop() noexcept;

  [[nodiscard]] bool IsRunning() const noexcept;
  [[nodiscard]] std::vector<std::string> TransportNames() const;

private:
  friend class detail::RuntimeAccess;
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace ovf::com
