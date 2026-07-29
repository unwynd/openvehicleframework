// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/engine.hpp"

#include <algorithm>
#include <thread>
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

Result<std::vector<JournalEvent>> MemoryTransitionJournal::Replay() noexcept {
  try {
    return Events();
  } catch (...) {
    return MakeError(ErrorCode::resource_exhausted, "cannot copy memory journal");
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

  auto replayed = journal->Replay();
  if (!replayed) {
    return MakeError(ErrorCode::persistence_error, "transition journal cannot be replayed",
                     static_cast<std::uint64_t>(replayed.error().code));
  }
  std::unordered_map<TransitionId, TransitionSnapshot> transitions;
  std::unordered_map<TransitionId, TransitionPlan> plans;
  std::uint64_t next_transition{1U};
  const auto valid_progress = [](TransitionPhase previous, TransitionPhase next) {
    if (previous == next) {
      return true;
    }
    if (previous == TransitionPhase::recovering) {
      return next == TransitionPhase::failed || next == TransitionPhase::recovery_failed;
    }
    const bool terminal =
        previous == TransitionPhase::succeeded || previous == TransitionPhase::rejected ||
        previous == TransitionPhase::failed || previous == TransitionPhase::cancelled ||
        previous == TransitionPhase::superseded || previous == TransitionPhase::deadline_exceeded ||
        previous == TransitionPhase::recovery_failed;
    return !terminal && static_cast<std::uint8_t>(previous) < static_cast<std::uint8_t>(next);
  };
  for (const auto& event : replayed.value()) {
    if (event.generation != model.value().generation) {
      return MakeError(ErrorCode::configuration_error,
                       "journal model generation does not match active model",
                       event.generation.value);
    }
    if (!event.transition.id || !event.transition.domain || !event.transition.source_mode ||
        !event.transition.target_mode ||
        model.FindMode({event.transition.domain, event.transition.source_mode}) == nullptr ||
        model.FindMode({event.transition.domain, event.transition.target_mode}) == nullptr) {
      return MakeError(ErrorCode::persistence_error,
                       "journal transition identity is not valid for the active model",
                       event.transition.id.value());
    }
    const auto previous = transitions.find(event.transition.id);
    if (previous != transitions.end() &&
        (previous->second.domain != event.transition.domain ||
         previous->second.source_mode != event.transition.source_mode ||
         previous->second.target_mode != event.transition.target_mode ||
         !valid_progress(previous->second.phase, event.transition.phase))) {
      return MakeError(ErrorCode::persistence_error, "journal transition history is inconsistent",
                       event.transition.id.value());
    }
    transitions[event.transition.id] = event.transition;
    next_transition = std::max(next_transition, event.transition.id.value() + 1U);
    if (event.plan) {
      if (event.plan->generation != event.generation ||
          event.plan->domain != event.transition.domain ||
          event.plan->source_mode != event.transition.source_mode ||
          event.plan->target_mode != event.transition.target_mode) {
        return MakeError(ErrorCode::persistence_error, "journal plan does not match its transition",
                         event.transition.id.value());
      }
      const auto valid_applications = [&model](const std::vector<ApplicationId>& applications) {
        return std::all_of(applications.begin(), applications.end(), [&model](ApplicationId id) {
          return model.FindApplication(id) != nullptr;
        });
      };
      const auto valid_domains = [&model](const std::vector<DomainId>& domains) {
        return std::all_of(domains.begin(), domains.end(),
                           [&model](DomainId id) { return model.FindDomain(id) != nullptr; });
      };
      if (!valid_applications(event.plan->retain) || !valid_applications(event.plan->stop) ||
          !valid_applications(event.plan->start) || !valid_domains(event.plan->guarded_domains)) {
        return MakeError(ErrorCode::persistence_error,
                         "journal plan references an unknown model object",
                         event.transition.id.value());
      }
      plans[event.transition.id] = *event.plan;
    }
    if (event.transition.failure) {
      const auto& failure = *event.transition.failure;
      if (failure.recovered_mode) {
        if (model.FindMode({event.transition.domain, *failure.recovered_mode}) == nullptr) {
          return MakeError(ErrorCode::persistence_error,
                           "journal recovery mode is not valid for the domain",
                           event.transition.id.value());
        }
        initial.value().committed_modes[event.transition.domain] = *failure.recovered_mode;
      }
      for (const auto application : failure.recovery_stopped_applications) {
        if (model.FindApplication(application) == nullptr) {
          return MakeError(ErrorCode::persistence_error,
                           "journal recovery stop references an unknown application",
                           event.transition.id.value());
        }
        initial.value().running_applications.erase(application);
      }
      for (const auto application : failure.recovery_started_applications) {
        if (model.FindApplication(application) == nullptr) {
          return MakeError(ErrorCode::persistence_error,
                           "journal recovery start references an unknown application",
                           event.transition.id.value());
        }
        initial.value().running_applications.insert(application);
      }
    }
    if (event.transition.phase == TransitionPhase::succeeded) {
      const auto plan = plans.find(event.transition.id);
      if (plan == plans.end()) {
        return MakeError(ErrorCode::persistence_error,
                         "successful transition has no persisted plan",
                         event.transition.id.value());
      }
      initial.value().committed_modes[plan->second.domain] = plan->second.target_mode;
      for (const auto application : plan->second.stop) {
        initial.value().running_applications.erase(application);
      }
      for (const auto application : plan->second.start) {
        initial.value().running_applications.insert(application);
      }
    }
  }

  std::unordered_set<ApplicationId> observed_running;
  const bool recovering = !replayed.value().empty();
  for (const auto& application : model.value().applications) {
    auto observation = backend->Inspect(application.id);
    if (!observation) {
      return observation.error();
    }
    if (observation.value().state == ApplicationState::ready) {
      observed_running.insert(application.id);
    }
  }
  const auto is_terminal = [](TransitionPhase phase) {
    return phase == TransitionPhase::succeeded || phase == TransitionPhase::rejected ||
           phase == TransitionPhase::failed || phase == TransitionPhase::cancelled ||
           phase == TransitionPhase::superseded || phase == TransitionPhase::deadline_exceeded ||
           phase == TransitionPhase::recovery_failed;
  };
  std::vector<TransitionId> interrupted;
  for (const auto& [id, transition] : transitions) {
    if (!is_terminal(transition.phase)) {
      interrupted.push_back(id);
    }
  }

  if (!recovering) {
    for (const auto application : initial.value().running_applications) {
      if (!observed_running.contains(application)) {
        return MakeError(ErrorCode::backend_unavailable,
                         "initially required application is not ready", application.value());
      }
    }
  } else if (interrupted.empty()) {
    for (const auto& application : model.value().applications) {
      const bool expected = initial.value().running_applications.contains(application.id);
      const bool observed = observed_running.contains(application.id);
      if (expected == observed) {
        continue;
      }
      if (expected) {
        auto started = backend->Start(application.id,
                                      std::chrono::steady_clock::now() + application.start_timeout);
        if (!started || started.value().state != ApplicationState::ready) {
          return MakeError(ErrorCode::backend_unavailable,
                           "cannot restore required application during recovery",
                           application.id.value());
        }
      } else {
        auto stopped = backend->Stop(application.id, StopReason::recovery,
                                     std::chrono::steady_clock::now() + application.stop_timeout);
        if (!stopped || stopped.value().state != ApplicationState::stopped) {
          return MakeError(ErrorCode::backend_unavailable,
                           "cannot stop unexpected application during recovery",
                           application.id.value());
        }
      }
    }
  } else {
    initial.value().running_applications = observed_running;
  }
  auto engine = std::unique_ptr<ExecutionEngine>(new ExecutionEngine(
      std::move(model), std::move(backend), std::move(journal), std::move(initial).value()));
  engine->transitions_ = std::move(transitions);
  engine->plans_ = std::move(plans);
  engine->next_transition_.store(next_transition, std::memory_order_relaxed);
  for (const auto& [id, transition] : engine->transitions_) {
    static_cast<void>(transition);
    engine->cancellation_[id] = false;
  }
  for (const auto id : interrupted) {
    auto transition = engine->transitions_.at(id);
    auto failed = engine->Fail(
        transition, MakeError(ErrorCode::backend_unavailable,
                              "transition was interrupted by execution coordinator restart"));
    if (!failed || !failed.value().failure) {
      return MakeError(ErrorCode::persistence_error,
                       "interrupted transition could not be finalized", id.value());
    }
  }
  return engine;
}

ExecutionEngine::ExecutionEngine(ValidatedModel model, std::unique_ptr<ProcessBackend> backend,
                                 std::unique_ptr<TransitionJournal> journal,
                                 SystemConfiguration configuration)
    : model_(std::move(model)), planner_(model_), backend_(std::move(backend)),
      journal_(std::move(journal)), configuration_(std::move(configuration)) {}

Result<AcceptedTransition> ExecutionEngine::AcceptMode(DomainId domain, ModeId target,
                                                       Deadline deadline) noexcept {
  SystemConfiguration baseline;
  {
    std::lock_guard lock(mutex_);
    baseline = configuration_;
  }
  return AcceptModeFrom(baseline, domain, target, deadline);
}

Result<AcceptedTransition> ExecutionEngine::AcceptModeFrom(const SystemConfiguration& baseline,
                                                           DomainId domain, ModeId target,
                                                           Deadline deadline) noexcept {
  if (deadline <= std::chrono::steady_clock::now()) {
    return MakeError(ErrorCode::deadline_exceeded, "transition deadline has expired");
  }
  TransitionSnapshot transition;
  transition.id = TransitionId{next_transition_.fetch_add(1U, std::memory_order_relaxed)};
  transition.domain = domain;
  transition.target_mode = target;
  transition.updated_at = std::chrono::steady_clock::now();

  TransitionPlan plan;
  std::optional<Error> planning_error;
  {
    std::lock_guard lock(mutex_);
    const auto source = baseline.committed_modes.find(domain);
    if (source != baseline.committed_modes.end()) {
      transition.source_mode = source->second;
    }
    transitions_.emplace(transition.id, transition);
    cancellation_.emplace(transition.id, false);
    auto planned = planner_.Plan(baseline, domain, target);
    if (!planned) {
      planning_error = planned.error();
    } else {
      plan = std::move(planned).value();
      plans_[transition.id] = plan;
    }
  }
  if (planning_error) {
    auto failed = Fail(transition, std::move(*planning_error));
    return failed ? failed.value().failure->error : failed.error();
  }

  auto recorded = Record(transition, TransitionPhase::validated);
  if (!recorded) {
    auto failed = Fail(transition, recorded.error());
    return failed ? failed.value().failure->error : failed.error();
  }
  recorded = Record(transition, TransitionPhase::planned);
  if (!recorded) {
    auto failed = Fail(transition, recorded.error());
    return failed ? failed.value().failure->error : failed.error();
  }
  return AcceptedTransition{transition.id, std::move(plan), deadline};
}

Result<TransitionSnapshot> ExecutionEngine::Execute(AcceptedTransition accepted) noexcept {
  auto current = GetTransition(accepted.id);
  if (!current) {
    return current.error();
  }
  auto transition = std::move(current).value();
  auto& plan = accepted.plan;
  const auto deadline = accepted.deadline;
  const auto domain = transition.domain;
  const auto target = transition.target_mode;
  Result<void> recorded;
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
      return Recover(transition, error, application, stopped ? stopped.value() : BackendEvidence{});
    }
    std::lock_guard lock(mutex_);
    configuration_.running_applications.erase(application);
  }

  if (!plan.start.empty()) {
    recorded = Record(transition, TransitionPhase::starting);
    if (!recorded) {
      return Fail(transition, recorded.error());
    }
    const bool requires_readiness =
        std::any_of(plan.start.begin(), plan.start.end(), [this](ApplicationId application) {
          return model_.FindApplication(application)->readiness == ReadinessPolicy::required;
        });
    if (requires_readiness) {
      recorded = Record(transition, TransitionPhase::awaiting_readiness);
      if (!recorded) {
        return Fail(transition, recorded.error());
      }
    }
  }
  for (const auto application : plan.start) {
    if (IsCancelled(transition.id)) {
      return Fail(transition, MakeError(ErrorCode::cancelled, "transition was cancelled"));
    }
    const auto* definition = model_.FindApplication(application);
    BackendEvidence last_evidence;
    Error last_error =
        MakeError(ErrorCode::backend_error, "application did not reach its required state");
    bool application_started = false;
    for (std::uint32_t attempt = 1U; attempt <= definition->retry.max_attempts; ++attempt) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        last_error =
            MakeError(ErrorCode::deadline_exceeded, "transition deadline expired during retry");
        break;
      }
      const auto operation_deadline = std::min(deadline, now + definition->start_timeout);
      auto started = backend_->Start(application, operation_deadline);
      if (started) {
        last_evidence = started.value();
        const bool required_state = definition->readiness == ReadinessPolicy::required
                                        ? started.value().state == ApplicationState::ready
                                        : started.value().state == ApplicationState::starting ||
                                              started.value().state == ApplicationState::ready;
        if (required_state) {
          application_started = true;
          break;
        }
        last_error =
            MakeError(ErrorCode::backend_error, "application did not reach its required state");
      } else {
        last_error = started.error();
      }
      if (attempt == definition->retry.max_attempts) {
        break;
      }
      const auto cleanup_deadline =
          std::min(deadline, std::chrono::steady_clock::now() + definition->stop_timeout);
      auto cleaned = backend_->Stop(application, StopReason::restart, cleanup_deadline);
      if (!cleaned || cleaned.value().state != ApplicationState::stopped) {
        last_error = cleaned ? MakeError(ErrorCode::backend_error,
                                         "failed start attempt could not be cleaned up")
                             : cleaned.error();
        if (cleaned) {
          last_evidence = cleaned.value();
        }
        break;
      }
      const auto retry_at =
          std::min(deadline, std::chrono::steady_clock::now() + definition->retry.delay);
      std::this_thread::sleep_until(retry_at);
    }
    if (!application_started) {
      return Recover(transition, std::move(last_error), application, std::move(last_evidence));
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

Result<TransitionSnapshot> ExecutionEngine::RequestMode(DomainId domain, ModeId target,
                                                        Deadline deadline) noexcept {
  auto accepted = AcceptMode(domain, target, deadline);
  if (!accepted) {
    return accepted.error();
  }
  return Execute(std::move(accepted).value());
}

Result<void> ExecutionEngine::Cancel(TransitionId transition) noexcept {
  std::lock_guard lock(mutex_);
  const auto found = cancellation_.find(transition);
  if (found == cancellation_.end()) {
    return MakeError(ErrorCode::not_found, "transition is not known");
  }
  const auto phase = transitions_.at(transition).phase;
  if (phase == TransitionPhase::committing || phase == TransitionPhase::succeeded ||
      phase == TransitionPhase::rejected || phase == TransitionPhase::failed ||
      phase == TransitionPhase::cancelled || phase == TransitionPhase::superseded ||
      phase == TransitionPhase::deadline_exceeded || phase == TransitionPhase::recovery_failed) {
    return MakeError(ErrorCode::invalid_transition,
                     "transition is no longer at a cancellation point");
  }
  found->second = true;
  return {};
}

Result<TransitionSnapshot> ExecutionEngine::GetTransition(TransitionId transition) const noexcept {
  std::lock_guard lock(mutex_);
  const auto found = transitions_.find(transition);
  if (found == transitions_.end()) {
    return MakeError(ErrorCode::not_found, "transition is not known");
  }
  return found->second;
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
  std::optional<TransitionPlan> plan;
  {
    std::lock_guard lock(mutex_);
    const auto found = plans_.find(transition.id);
    if (found != plans_.end()) {
      plan = found->second;
    }
  }
  auto persisted = journal_->Append({transition, model_.value().generation, std::move(plan)});
  if (!persisted) {
    return MakeError(ErrorCode::persistence_error, "transition phase could not be persisted",
                     static_cast<std::uint64_t>(persisted.error().code));
  }
  std::lock_guard lock(mutex_);
  transitions_[transition.id] = transition;
  return {};
}

Result<void> ExecutionEngine::ApplyRecoveryPlan(const TransitionPlan& plan, Deadline deadline,
                                                TransitionFailure& failure) noexcept {
  for (const auto application : plan.stop) {
    const auto* definition = model_.FindApplication(application);
    const auto operation_deadline =
        std::min(deadline, std::chrono::steady_clock::now() + definition->stop_timeout);
    auto stopped = backend_->Stop(application, StopReason::recovery, operation_deadline);
    if (!stopped || stopped.value().state != ApplicationState::stopped) {
      failure.application = application;
      if (stopped) {
        failure.evidence = stopped.value();
      }
      return stopped ? Result<void>{MakeError(ErrorCode::backend_error,
                                              "recovery could not stop application")}
                     : Result<void>{stopped.error()};
    }
    try {
      failure.recovery_stopped_applications.push_back(application);
    } catch (...) {
      return MakeError(ErrorCode::resource_exhausted, "cannot record recovery stop evidence");
    }
    {
      std::lock_guard lock(mutex_);
      configuration_.running_applications.erase(application);
    }
  }
  for (const auto application : plan.start) {
    const auto* definition = model_.FindApplication(application);
    bool ready = false;
    for (std::uint32_t attempt = 1U; attempt <= definition->retry.max_attempts; ++attempt) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return MakeError(ErrorCode::deadline_exceeded, "recovery deadline expired");
      }
      auto started =
          backend_->Start(application, std::min(deadline, now + definition->start_timeout));
      if (started) {
        failure.evidence = started.value();
        const bool required_state = definition->readiness == ReadinessPolicy::required
                                        ? started.value().state == ApplicationState::ready
                                        : started.value().state == ApplicationState::starting ||
                                              started.value().state == ApplicationState::ready;
        if (required_state) {
          ready = true;
          break;
        }
      } else if (attempt == definition->retry.max_attempts) {
        failure.application = application;
        return started.error();
      }
      if (attempt < definition->retry.max_attempts) {
        const auto cleanup_deadline =
            std::min(deadline, std::chrono::steady_clock::now() + definition->stop_timeout);
        auto cleaned = backend_->Stop(application, StopReason::recovery, cleanup_deadline);
        if (!cleaned || cleaned.value().state != ApplicationState::stopped) {
          failure.application = application;
          if (cleaned) {
            failure.evidence = cleaned.value();
          }
          return cleaned ? Result<void>{MakeError(ErrorCode::backend_error,
                                                  "recovery retry could not clean up application")}
                         : Result<void>{cleaned.error()};
        }
        std::this_thread::sleep_until(
            std::min(deadline, std::chrono::steady_clock::now() + definition->retry.delay));
      }
    }
    if (!ready) {
      failure.application = application;
      return MakeError(ErrorCode::backend_error, "recovery could not start application");
    }
    try {
      failure.recovery_started_applications.push_back(application);
    } catch (...) {
      return MakeError(ErrorCode::resource_exhausted, "cannot record recovery start evidence");
    }
    {
      std::lock_guard lock(mutex_);
      configuration_.running_applications.insert(application);
    }
  }
  return {};
}

Result<TransitionSnapshot> ExecutionEngine::Recover(TransitionSnapshot& transition, Error error,
                                                    std::optional<ApplicationId> application,
                                                    BackendEvidence evidence) noexcept {
  transition.failure = TransitionFailure{
      .error = std::move(error),
      .application = application,
      .evidence = std::move(evidence),
      .recovery_action = FailureAction::hold_observed_configuration,
      .recovery_attempted = false,
      .recovery_succeeded = false,
      .recovery_error = std::nullopt,
      .recovered_mode = std::nullopt,
      .recovery_stopped_applications = {},
      .recovery_started_applications = {},
  };
  const auto* domain = model_.FindDomain(transition.domain);
  if (domain == nullptr || domain->recovery.action == FailureAction::hold_observed_configuration) {
    return Fail(transition, transition.failure->error, transition.failure->application,
                transition.failure->evidence);
  }
  transition.failure->recovery_action = domain->recovery.action;
  transition.failure->recovery_attempted = true;
  auto recorded = Record(transition, TransitionPhase::recovering);
  if (!recorded) {
    return Fail(transition, recorded.error());
  }
  const auto deadline = std::chrono::steady_clock::now() + domain->recovery.deadline;
  Result<void> recovered;
  if (domain->recovery.action == FailureAction::enter_fallback_mode) {
    SystemConfiguration configuration;
    {
      std::lock_guard lock(mutex_);
      configuration = configuration_;
    }
    auto plan = planner_.Reconcile(configuration, domain->id, domain->recovery.fallback_mode);
    if (!plan) {
      recovered = plan.error();
    } else {
      recovered = ApplyRecoveryPlan(plan.value(), deadline, *transition.failure);
      if (recovered) {
        std::lock_guard lock(mutex_);
        configuration_.committed_modes[domain->id] = domain->recovery.fallback_mode;
        transition.failure->recovered_mode = domain->recovery.fallback_mode;
      }
    }
  } else if (domain->recovery.action == FailureAction::stop_domain) {
    SystemConfiguration configuration;
    {
      std::lock_guard lock(mutex_);
      configuration = configuration_;
    }
    auto stopped = planner_.StopDomainApplications(configuration, domain->id);
    if (!stopped) {
      recovered = stopped.error();
    } else {
      TransitionPlan plan;
      plan.stop = std::move(stopped).value();
      recovered = ApplyRecoveryPlan(plan, deadline, *transition.failure);
    }
  } else if (domain->recovery.action == FailureAction::request_system_recovery) {
    recovered = backend_->RequestSystemRecovery(deadline);
  } else {
    recovered = MakeError(ErrorCode::unsupported, "recovery policy is not implemented");
  }
  if (!recovered) {
    transition.failure->recovery_error = recovered.error();
    recorded = Record(transition, TransitionPhase::recovery_failed);
    return recorded ? Result<TransitionSnapshot>{transition}
                    : Result<TransitionSnapshot>{recorded.error()};
  }
  transition.failure->recovery_succeeded = true;
  recorded = Record(transition, TransitionPhase::failed);
  return recorded ? Result<TransitionSnapshot>{transition}
                  : Result<TransitionSnapshot>{recorded.error()};
}

Result<TransitionSnapshot> ExecutionEngine::Fail(TransitionSnapshot& transition, Error error,
                                                 std::optional<ApplicationId> application,
                                                 BackendEvidence evidence) noexcept {
  transition.failure = TransitionFailure{
      .error = std::move(error),
      .application = application,
      .evidence = std::move(evidence),
      .recovery_action = FailureAction::hold_observed_configuration,
      .recovery_attempted = false,
      .recovery_succeeded = false,
      .recovery_error = std::nullopt,
      .recovered_mode = std::nullopt,
      .recovery_stopped_applications = {},
      .recovery_started_applications = {},
  };
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
