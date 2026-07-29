// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/exec/error.hpp"
#include "ovf/exec/types.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace ovf::exec {

using Deadline = std::chrono::steady_clock::time_point;

struct ApplicationOptions final {
  ApplicationId expected_id;
  std::string expected_name;
};

class StopSubscription final {
public:
  StopSubscription() = default;
  ~StopSubscription();
  StopSubscription(StopSubscription&&) noexcept;
  StopSubscription& operator=(StopSubscription&&) noexcept;
  StopSubscription(const StopSubscription&) = delete;
  StopSubscription& operator=(const StopSubscription&) = delete;

  void Reset() noexcept;
  [[nodiscard]] explicit operator bool() const noexcept;

private:
  friend class Application;
  explicit StopSubscription(std::function<void()> unsubscribe);

  std::function<void()> unsubscribe_;
};

class Application final {
public:
  static Result<Application> Create(ApplicationOptions options = {});

  ~Application();
  Application(Application&&) noexcept;
  Application& operator=(Application&&) noexcept;
  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  [[nodiscard]] ApplicationId Id() const noexcept;
  [[nodiscard]] std::string Name() const;
  [[nodiscard]] Result<void> ReportReady() noexcept;
  [[nodiscard]] bool StopRequested() const noexcept;
  [[nodiscard]] StopReason GetStopReason() const noexcept;
  [[nodiscard]] Result<StopReason> WaitForStop(Deadline deadline) noexcept;
  [[nodiscard]] Result<StopSubscription> OnStop(std::function<void(StopReason)> handler);

private:
  class Impl;
  explicit Application(std::shared_ptr<Impl> impl);
  friend class detail_ApplicationFactory;

  std::shared_ptr<Impl> impl_;
};

} // namespace ovf::exec
