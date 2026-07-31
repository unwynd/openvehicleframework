// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/exec/application.hpp"
#include "ovf/exec/model.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ovf::exec {

struct CoordinatorOptions final {
  std::string endpoint;
  std::chrono::milliseconds connect_timeout{2000};
};

struct TransitionOptions final {
  std::chrono::milliseconds timeout{30000};
};

struct ExecutionDomain final {
  DomainId id;
  std::string name;
  ModeId committed_mode;
  std::optional<ModeId> target_mode;
  DomainStatus status{DomainStatus::unknown};
  std::optional<TransitionId> active_transition;
};

struct ExecutionMode final {
  DomainId domain;
  ModeId id;
  std::string name;
};

struct ApplicationSnapshot final {
  ApplicationId id;
  std::string name;
  ApplicationState state{ApplicationState::unknown};
};

struct ExecutionUnitSnapshot final {
  ExecutionUnitId id;
  std::string name;
  ExecutionUnitKind kind{ExecutionUnitKind::managed_application};
  bool bootstrap{};
  ExecutionUnitState state{ExecutionUnitState::unknown};
};

struct TransitionFailureSnapshot final {
  Error error;
  std::optional<ApplicationId> application;
  std::int32_t exit_code{};
  std::int32_t signal{};
  std::uint64_t backend_code{};
  std::string backend_message;
  FailureAction recovery_action{FailureAction::hold_observed_configuration};
  bool recovery_attempted{};
  bool recovery_succeeded{};
  std::optional<Error> recovery_error;
  std::optional<ModeId> recovered_mode;
  std::vector<ApplicationId> recovery_stopped_applications;
  std::vector<ApplicationId> recovery_started_applications;
};

struct TransitionSnapshot final {
  TransitionId id;
  DomainId domain;
  ModeId source_mode;
  ModeId target_mode;
  TransitionPhase phase{TransitionPhase::received};
  std::chrono::steady_clock::time_point updated_at;
  std::optional<TransitionFailureSnapshot> failure;
};

struct SystemSnapshot final {
  ModelGeneration model_generation;
  std::uint64_t revision{};
  bool recovering{};
  std::vector<ExecutionDomain> domains;
  std::vector<ExecutionUnitSnapshot> units;
  std::vector<ApplicationSnapshot> applications;
  std::vector<TransitionSnapshot> transitions;
};

enum class CoordinatorEventKind : std::uint8_t {
  transition_changed,
  configuration_changed,
  recovery_changed
};

struct CoordinatorEvent final {
  CoordinatorEventKind kind{CoordinatorEventKind::transition_changed};
  std::uint64_t revision{};
  std::optional<TransitionId> transition;
  std::optional<DomainId> domain;
};

struct EventFilter final {
  bool transitions{true};
  bool configuration{true};
  bool recovery{true};
  std::optional<DomainId> domain;
};

class EventSubscription final {
public:
  EventSubscription() = default;
  ~EventSubscription();
  EventSubscription(EventSubscription&&) noexcept;
  EventSubscription& operator=(EventSubscription&&) noexcept;
  EventSubscription(const EventSubscription&) = delete;
  EventSubscription& operator=(const EventSubscription&) = delete;

  void Reset() noexcept;
  [[nodiscard]] explicit operator bool() const noexcept;

private:
  friend class SystemCoordinator;
  explicit EventSubscription(std::function<void()> unsubscribe);
  std::function<void()> unsubscribe_;
};

class Transition final {
public:
  Transition() = default;

  [[nodiscard]] TransitionId Id() const noexcept;
  [[nodiscard]] Result<TransitionSnapshot> Get() const;
  [[nodiscard]] Result<TransitionSnapshot> Wait(Deadline deadline) const;
  [[nodiscard]] explicit operator bool() const noexcept;

private:
  class Impl;
  friend class SystemCoordinator;
  explicit Transition(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};

class SystemCoordinator final {
public:
  using EventHandler = std::function<void(const CoordinatorEvent&)>;

  static Result<SystemCoordinator> Connect(CoordinatorOptions options = {});

  ~SystemCoordinator();
  SystemCoordinator(SystemCoordinator&&) noexcept;
  SystemCoordinator& operator=(SystemCoordinator&&) noexcept;
  SystemCoordinator(const SystemCoordinator&) = delete;
  SystemCoordinator& operator=(const SystemCoordinator&) = delete;

  [[nodiscard]] Result<SystemSnapshot> GetSnapshot() const;
  [[nodiscard]] Result<ExecutionDomain> ResolveDomain(DomainId domain) const;
  [[nodiscard]] Result<ExecutionMode> ResolveMode(DomainId domain, ModeId mode) const;
  [[nodiscard]] Result<Transition> RequestMode(DomainId domain, ModeId mode,
                                               TransitionOptions options = {});
  [[nodiscard]] Result<void> Cancel(TransitionId transition);
  [[nodiscard]] Result<EventSubscription> Subscribe(EventFilter filter, EventHandler handler);

private:
  class Impl;
  explicit SystemCoordinator(std::shared_ptr<Impl> impl);
  friend class detail_CoordinatorFactory;
  std::shared_ptr<Impl> impl_;
};

} // namespace ovf::exec
