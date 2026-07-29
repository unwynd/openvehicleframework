// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/engine.hpp"

#include <algorithm>
#include <utility>

namespace ovf::exec::detail {

Result<void> MemoryTransitionJournal::Append(const JournalEvent& event) noexcept {
  try {
    std::lock_guard lock(mutex_);
    events_.push_back(event);
    return {};
  } catch (...) {
    return MakeError(ErrorCode::resource_exhausted, "memory journal capacity exhausted");
  }
}

std::vector<JournalEvent> MemoryTransitionJournal::Events() const {
  std::lock_guard lock(mutex_);
  return events_;
}

Result<std::unique_ptr<ExecutionEngine>>
ExecutionEngine::Create(ValidatedModel model, std::unique_ptr<ProcessBackend> backend,
                        std::unique_ptr<TransitionJournal> journal) {
  if (!backend || !journal) {
    return MakeError(ErrorCode::invalid_argument, "engine backend and journal are required");
  }
  TransitionPlanner planner(model);
  auto initial = planner.InitialConfiguration();
  if (!initial) {
    return initial.error();
  }

  for (const auto application : initial.value().running_applications) {
    auto observation = backend->Inspect(application);
    if (!observation) {
      return observation.error();
    }
    if (observation.value().state != ApplicationState::ready) {
      return MakeError(ErrorCode::backend_unavailable,
                       "initially required application is not ready", application.value());
    }
  }
  return std::unique_ptr<ExecutionEngine>(new ExecutionEngine(
      std::move(model), std::move(backend), std::move(journal), std::move(initial).value()));
}

ExecutionEngine::ExecutionEngine(ValidatedModel model, std::unique_ptr<ProcessBackend> backend,
                                 std::unique_ptr<TransitionJournal> journal,
                                 SystemConfiguration configuration)
    : model_(std::move(model)), planner_(model_), backend_(std::move(backend)),
      journal_(std::move(journal)), configuration_(std::move(configuration)) {}

Result<TransitionSnapshot> ExecutionEngine::RequestMode(DomainId domain, ModeId target,
                                                        Deadline deadline) noexcept {
  TransitionSnapshot transition;
  transition.id = TransitionId{next_transition_.fetch_add(1U, std::memory_order_relaxed)};
  transition.domain = domain;
  transition.target_mode = target;
  transition.updated_at = std::chrono::steady_clock::now();

  TransitionPlan plan;
  std::optional<Error> planning_error;
  {
    std::lock_guard lock(mutex_);
    const auto source = configuration_.committed_modes.find(domain);
    if (source != configuration_.committed_modes.end()) {
      transition.source_mode = source->second;
    }
    transitions_.emplace(transition.id, transition);
    cancellation_.emplace(transition.id, false);
    auto planned = planner_.Plan(configuration_, domain, target);
    if (!planned) {
      planning_error = planned.error();
    } else {
      plan = std::move(planned).value();
    }
  }
  if (planning_error) {
    return Fail(transition, std::move(*planning_error));
  }

  auto recorded = Record(transition, TransitionPhase::validated);
  if (!recorded) {
    return Fail(transition, recorded.error());
  }
  recorded = Record(transition, TransitionPhase::planned);
  if (!recorded) {
    return Fail(transition, recorded.error());
  }

  TransitionResources resources;
  resources.domains = plan.guarded_domains;
  resources.applications = plan.stop;
  resources.applications.insert(resources.applications.end(), plan.start.begin(), plan.start.end());
  resources.exclusive_resources = plan.affected_resources;
  auto lease = scheduler_.Acquire(transition.id, std::move(resources), deadline);
  if (!lease) {
    return Fail(transition, lease.error());
  }

  std::optional<Error> replanning_error;
  {
    std::lock_guard lock(mutex_);
    auto replanned = planner_.Plan(configuration_, domain, target);
    if (!replanned) {
      replanning_error = replanned.error();
    } else if (replanned.value().start != plan.start || replanned.value().stop != plan.stop) {
      replanning_error = MakeError(ErrorCode::superseded,
                                   "system configuration changed while transition was waiting");
    }
  }
  if (replanning_error) {
    return Fail(transition, std::move(*replanning_error));
  }

  if (!plan.stop.empty()) {
    recorded = Record(transition, TransitionPhase::stopping);
    if (!recorded) {
      return Fail(transition, recorded.error());
    }
  }
  for (const auto application : plan.stop) {
    if (IsCancelled(transition.id)) {
      return Fail(transition, MakeError(ErrorCode::cancelled, "transition was cancelled"));
    }
    const auto* definition = model_.FindApplication(application);
    const auto operation_deadline =
        std::min(deadline, std::chrono::steady_clock::now() + definition->stop_timeout);
    auto stopped = backend_->Stop(application, StopReason::mode_change, operation_deadline);
    if (!stopped || stopped.value().state != ApplicationState::stopped) {
      const auto error =
          stopped ? MakeError(ErrorCode::backend_error, "application did not stop cleanly")
                  : stopped.error();
      return Fail(transition, error, application, stopped ? stopped.value() : BackendEvidence{});
    }
    std::lock_guard lock(mutex_);
    configuration_.running_applications.erase(application);
  }

  if (!plan.start.empty()) {
    recorded = Record(transition, TransitionPhase::starting);
    if (!recorded) {
      return Fail(transition, recorded.error());
    }
  }
  for (const auto application : plan.start) {
    if (IsCancelled(transition.id)) {
      return Fail(transition, MakeError(ErrorCode::cancelled, "transition was cancelled"));
    }
    const auto* definition = model_.FindApplication(application);
    const auto operation_deadline =
        std::min(deadline, std::chrono::steady_clock::now() + definition->start_timeout);
    auto started = backend_->Start(application, operation_deadline);
    if (!started || started.value().state != ApplicationState::ready) {
      const auto error =
          started ? MakeError(ErrorCode::backend_error, "application did not become ready")
                  : started.error();
      return Fail(transition, error, application, started ? started.value() : BackendEvidence{});
    }
    std::lock_guard lock(mutex_);
    configuration_.running_applications.insert(application);
  }

  recorded = Record(transition, TransitionPhase::committing);
  if (!recorded) {
    return Fail(transition, recorded.error());
  }
  {
    std::lock_guard lock(mutex_);
    configuration_.committed_modes[domain] = target;
  }
  recorded = Record(transition, TransitionPhase::succeeded);
  if (!recorded) {
    return Fail(transition, recorded.error());
  }
  return transition;
}

Result<void> ExecutionEngine::Cancel(TransitionId transition) noexcept {
  std::lock_guard lock(mutex_);
  const auto found = cancellation_.find(transition);
  if (found == cancellation_.end()) {
    return MakeError(ErrorCode::not_found, "transition is not known");
  }
  const auto phase = transitions_.at(transition).phase;
  if (phase == TransitionPhase::committing || phase == TransitionPhase::succeeded ||
      phase == TransitionPhase::failed || phase == TransitionPhase::cancelled) {
    return MakeError(ErrorCode::invalid_transition,
                     "transition is no longer at a cancellation point");
  }
  found->second = true;
  return {};
}

EngineSnapshot ExecutionEngine::Snapshot() const {
  std::lock_guard lock(mutex_);
  EngineSnapshot snapshot;
  snapshot.configuration = configuration_;
  snapshot.transitions.reserve(transitions_.size());
  for (const auto& [id, transition] : transitions_) {
    static_cast<void>(id);
    snapshot.transitions.push_back(transition);
  }
  std::sort(snapshot.transitions.begin(), snapshot.transitions.end(),
            [](const auto& left, const auto& right) { return left.id < right.id; });
  return snapshot;
}

Result<void> ExecutionEngine::Record(TransitionSnapshot& transition,
                                     TransitionPhase phase) noexcept {
  transition.phase = phase;
  transition.updated_at = std::chrono::steady_clock::now();
  auto persisted = journal_->Append({transition, model_.value().generation});
  if (!persisted) {
    return MakeError(ErrorCode::persistence_error, "transition phase could not be persisted",
                     static_cast<std::uint64_t>(persisted.error().code));
  }
  std::lock_guard lock(mutex_);
  transitions_[transition.id] = transition;
  return {};
}

Result<TransitionSnapshot> ExecutionEngine::Fail(TransitionSnapshot& transition, Error error,
                                                 std::optional<ApplicationId> application,
                                                 BackendEvidence evidence) noexcept {
  transition.failure = TransitionFailure{std::move(error), application, std::move(evidence)};
  const auto terminal =
      transition.failure->error.code == ErrorCode::cancelled ? TransitionPhase::cancelled
      : transition.failure->error.code == ErrorCode::deadline_exceeded
          ? TransitionPhase::deadline_exceeded
      : transition.failure->error.code == ErrorCode::superseded ? TransitionPhase::superseded
                                                                : TransitionPhase::failed;
  auto recorded = Record(transition, terminal);
  if (!recorded) {
    transition.failure->error = MakeError(ErrorCode::persistence_error,
                                          "terminal transition failure could not be persisted");
  }
  return transition;
}

bool ExecutionEngine::IsCancelled(TransitionId transition) const noexcept {
  std::lock_guard lock(mutex_);
  const auto found = cancellation_.find(transition);
  return found != cancellation_.end() && found->second;
}

} // namespace ovf::exec::detail
