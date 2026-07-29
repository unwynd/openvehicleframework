// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/exec/internal/engine.hpp"

#include <memory>
#include <string>
#include <unordered_map>

namespace ovf::exec::backends {

struct DinitConfig final {
  std::string control_socket;
  std::unordered_map<ApplicationId, std::string> services;
};

[[nodiscard]] Result<std::unique_ptr<detail::ProcessBackend>>
CreateDinitBackend(DinitConfig config);

} // namespace ovf::exec::backends
