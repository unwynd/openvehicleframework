// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/exec/internal/coordinator_service.hpp"
#include "ovf/exec/internal/file_journal.hpp"

#include <string>
#include <vector>

namespace ovf::exec::detail {

struct CoordinatorEndpoint final {
  std::string socket;
  CoordinatorServiceOptions limits;
  std::vector<std::uint32_t> observation_uids;
  std::vector<std::uint32_t> mutation_uids;
};

struct RuntimeDeployment final {
  ValidatedModel model;
  std::string backend_kind;
  std::string backend_configuration;
  FileJournalOptions journal;
  CoordinatorEndpoint coordinator;
};

[[nodiscard]] Result<RuntimeDeployment>
LoadRuntimeDeployment(const std::string& execution_model_path,
                      const std::string& backend_configuration_path);

} // namespace ovf::exec::detail
