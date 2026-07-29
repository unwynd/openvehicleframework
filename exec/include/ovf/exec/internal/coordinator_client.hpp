// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/exec/coordinator.hpp"

#include <functional>
#include <memory>

namespace ovf::exec::detail {

class CoordinatorClient {
public:
  virtual ~CoordinatorClient() = default;
  [[nodiscard]] virtual Result<SystemSnapshot> Snapshot() const = 0;
  [[nodiscard]] virtual Result<ExecutionDomain> Domain(DomainId domain) const = 0;
  [[nodiscard]] virtual Result<ExecutionMode> Mode(DomainId domain, ModeId mode) const = 0;
  [[nodiscard]] virtual Result<TransitionId>
  Request(DomainId domain, ModeId mode, TransitionOptions options) = 0;
  [[nodiscard]] virtual Result<void> Cancel(TransitionId transition) = 0;
  [[nodiscard]] virtual Result<TransitionSnapshot>
  TransitionState(TransitionId transition) const = 0;
  [[nodiscard]] virtual Result<TransitionSnapshot>
  Wait(TransitionId transition, Deadline deadline) const = 0;
  [[nodiscard]] virtual Result<std::function<void()>>
  Subscribe(EventFilter filter, SystemCoordinator::EventHandler handler) = 0;
};

[[nodiscard]] Result<std::shared_ptr<CoordinatorClient>>
ConnectCoordinatorIpc(const CoordinatorOptions& options);

} // namespace ovf::exec::detail
