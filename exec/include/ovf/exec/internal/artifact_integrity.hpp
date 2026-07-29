// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/exec/error.hpp"

#include <string>

namespace ovf::exec::detail {

struct RuntimeArtifactPaths final {
  std::string execution_model;
  std::string backend_configuration;
  std::string services_directory;
  std::string manifest;
};

[[nodiscard]] Result<void> VerifyRuntimeArtifacts(const RuntimeArtifactPaths& paths);

} // namespace ovf::exec::detail
