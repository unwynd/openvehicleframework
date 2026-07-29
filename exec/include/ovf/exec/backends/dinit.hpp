// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/exec/internal/engine.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ovf::exec::backends {

struct DinitConfig final {
  std::string control_socket;
  std::string system_recovery_service;
  std::unordered_map<ApplicationId, std::string> services;
};

[[nodiscard]] Result<std::unique_ptr<detail::ProcessBackend>>
CreateDinitBackend(DinitConfig config);

[[nodiscard]] Result<DinitConfig> ParseDinitConfig(std::string_view configuration);

} // namespace ovf::exec::backends
