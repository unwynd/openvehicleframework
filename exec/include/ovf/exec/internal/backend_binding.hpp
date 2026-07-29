// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/exec/backend_abi.h"
#include "ovf/exec/internal/engine.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ovf::exec::detail {

enum class BackendLogLevel { debug, info, warning, error };
using BackendLogger = std::function<void(BackendLogLevel, std::string_view)>;

struct BackendBindingConfig final {
  std::string configuration;
  std::uint32_t required_parallel_operations{1U};
  bool require_system_recovery{};
  BackendLogger logger;
};

[[nodiscard]] Result<std::unique_ptr<ProcessBackend>>
BindBackend(const ovf_exec_backend_factory_v1& factory, BackendBindingConfig config);

} // namespace ovf::exec::detail
