// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/exec/coordinator.hpp"
#include "ovf/exec/internal/coordinator_client.hpp"
#include "ovf/exec/internal/engine.hpp"

#include <cstddef>
#include <memory>

namespace ovf::exec::detail {

struct CoordinatorPermissions final {
  bool observe{};
  bool mutate{};
};

struct CoordinatorServiceOptions final {
  std::size_t queue_capacity{64U};
  std::size_t worker_count{2U};
};

} // namespace ovf::exec::detail

namespace ovf::exec {

class detail_CoordinatorFactory final {
public:
  [[nodiscard]] static Result<SystemCoordinator>
  Create(std::unique_ptr<detail::ExecutionEngine> engine,
         detail::CoordinatorPermissions permissions,
         detail::CoordinatorServiceOptions options = {});

  [[nodiscard]] static SystemCoordinator
  CreateClient(std::shared_ptr<detail::CoordinatorClient> client);
};

} // namespace ovf::exec
