// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/exec/coordinator.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ovf::exec::detail {

struct CoordinatorServerOptions final {
  std::string endpoint;
  std::vector<std::uint32_t> observation_uids;
  std::vector<std::uint32_t> mutation_uids;
  std::size_t connection_capacity{64U};
  std::size_t worker_count{8U};
  std::size_t maximum_message_size{1024U * 1024U};
};

class CoordinatorServer {
public:
  virtual ~CoordinatorServer() = default;
  [[nodiscard]] virtual std::string Endpoint() const = 0;
};

[[nodiscard]] Result<std::unique_ptr<CoordinatorServer>>
StartCoordinatorServer(SystemCoordinator coordinator, CoordinatorServerOptions options);

} // namespace ovf::exec::detail
