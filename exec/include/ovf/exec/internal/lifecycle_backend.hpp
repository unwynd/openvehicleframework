// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/exec/application.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ovf::exec::detail {

class LifecycleBackend {
public:
  using StopHandler = std::function<void(StopReason)>;

  virtual ~LifecycleBackend() = default;
  [[nodiscard]] virtual ApplicationId Id() const noexcept = 0;
  [[nodiscard]] virtual std::string Name() const = 0;
  [[nodiscard]] virtual Result<void> ReportReady() noexcept = 0;
  [[nodiscard]] virtual std::uint64_t Subscribe(StopHandler handler) = 0;
  virtual void Unsubscribe(std::uint64_t subscription) noexcept = 0;
  [[nodiscard]] virtual bool StopRequested() const noexcept = 0;
  [[nodiscard]] virtual StopReason GetStopReason() const noexcept = 0;
  [[nodiscard]] virtual Result<StopReason> WaitForStop(Deadline deadline) noexcept = 0;
};

[[nodiscard]] Result<std::unique_ptr<LifecycleBackend>>
CreateDinitLifecycleBackend(const ApplicationOptions& options);

} // namespace ovf::exec::detail

namespace ovf::exec {

class detail_ApplicationFactory final {
public:
  [[nodiscard]] static Result<Application>
  Create(std::unique_ptr<detail::LifecycleBackend> backend);
};

} // namespace ovf::exec
