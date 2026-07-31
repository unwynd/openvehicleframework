// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/coordinator_service.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace ovf::exec {
namespace {

bool IsTerminal(TransitionPhase phase) noexcept {
  return phase == TransitionPhase::succeeded || phase == TransitionPhase::rejected ||
         phase == TransitionPhase::failed || phase == TransitionPhase::cancelled ||
         phase == TransitionPhase::superseded || phase == TransitionPhase::deadline_exceeded ||
         phase == TransitionPhase::recovery_failed;
}

TransitionSnapshot PublicSnapshot(const detail::TransitionSnapshot& source) {
  TransitionSnapshot result{source.id,    source.domain,     source.source_mode, source.target_mode,
                            source.phase, source.updated_at, std::nullopt};
  if (source.failure) {
    result.failure = TransitionFailureSnapshot{
        source.failure->error,
        source.failure->application,
        source.failure->evidence.exit_code,
        source.failure->evidence.signal,
        source.failure->evidence.native_code,
        source.failure->evidence.message,
        source.failure->recovery_action,
        source.failure->recovery_attempted,
        source.failure->recovery_succeeded,
        source.failure->recovery_error,
        source.failure->recovered_mode,
        source.failure->recovery_stopped_applications,
        source.failure->recovery_started_applications,
    };
  }
  return result;
}

bool Matches(const EventFilter& filter, const CoordinatorEvent& event) noexcept {
  if (filter.domain && event.domain != filter.domain) {
    return false;
  }
  switch (event.kind) {
  case CoordinatorEventKind::transition_changed:
    return filter.transitions;
  case CoordinatorEventKind::configuration_changed:
    return filter.configuration;
  case CoordinatorEventKind::recovery_changed:
    return filter.recovery;
  }
  return false;
}

} // namespace

class CoordinatorService final : public std::enable_shared_from_this<CoordinatorService> {
public:
  struct Subscription final {
    EventFilter filter;
    SystemCoordinator::EventHandler handler;
  };

  CoordinatorService(std::unique_ptr<detail::ExecutionEngine> engine,
                     detail::CoordinatorPermissions permissions, std::size_t queue_capacity)
      : engine_(std::move(engine)), permissions_(permissions), queue_capacity_(queue_capacity),
        projected_(engine_->Snapshot().configuration) {}

  ~CoordinatorService() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    work_available_.notify_all();
    changed_.notify_all();
    for (auto& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  void Start(std::size_t worker_count) {
    workers_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
      workers_.emplace_back([this] { Worker(); });
    }
  }

  Result<SystemSnapshot> Snapshot() const {
    if (!permissions_.observe) {
      return MakeError(ErrorCode::permission_denied, "coordinator observation is not authorized");
    }
    const auto engine_snapshot = engine_->Snapshot();
    const auto& model = engine_->Model().value();
    SystemSnapshot result;
    result.model_generation = model.generation;
    {
      std::lock_guard lock(mutex_);
      result.revision = revision_;
      result.recovering = recovering_;
    }

    result.transitions.reserve(engine_snapshot.transitions.size());
    std::unordered_map<DomainId, const detail::TransitionSnapshot*> active;
    std::unordered_map<DomainId, const detail::TransitionSnapshot*> failed;
    for (const auto& transition : engine_snapshot.transitions) {
      result.transitions.push_back(PublicSnapshot(transition));
      if (!IsTerminal(transition.phase)) {
        const auto found = active.find(transition.domain);
        if (found == active.end() || found->second->id < transition.id) {
          active[transition.domain] = &transition;
        }
      } else if (transition.failure) {
        const auto found = failed.find(transition.domain);
        if (found == failed.end() || found->second->id < transition.id) {
          failed[transition.domain] = &transition;
        }
      }
    }

    result.domains.reserve(model.domains.size());
    for (const auto& domain : model.domains) {
      ExecutionDomain snapshot;
      snapshot.id = domain.id;
      snapshot.name = domain.name;
      const auto committed = engine_snapshot.configuration.committed_modes.find(domain.id);
      if (committed != engine_snapshot.configuration.committed_modes.end()) {
        snapshot.committed_mode = committed->second;
      }
      const auto transition = active.find(domain.id);
      if (transition != active.end()) {
        snapshot.target_mode = transition->second->target_mode;
        snapshot.active_transition = transition->second->id;
        snapshot.status = DomainStatus::transitioning;
      } else {
        snapshot.status = result.recovering            ? DomainStatus::recovering
                          : failed.contains(domain.id) ? DomainStatus::degraded
                                                       : DomainStatus::stable;
      }
      result.domains.push_back(std::move(snapshot));
    }

    result.units.reserve(model.units.size());
    result.applications.reserve(model.units.size());
    for (const auto& application : model.units) {
      const auto state = engine_snapshot.configuration.running_units.contains(application.id)
                             ? ApplicationState::ready
                             : ApplicationState::stopped;
      result.units.push_back(
          {application.id, application.name, application.kind, application.bootstrap, state});
      if (application.kind == ExecutionUnitKind::managed_application) {
        result.applications.push_back({application.id, application.name, state});
      }
    }
    return result;
  }

  Result<ExecutionDomain> Domain(DomainId id) const {
    auto snapshot = Snapshot();
    if (!snapshot) {
      return snapshot.error();
    }
    const auto found =
        std::find_if(snapshot.value().domains.begin(), snapshot.value().domains.end(),
                     [id](const auto& domain) { return domain.id == id; });
    return found == snapshot.value().domains.end()
               ? Result<ExecutionDomain>{MakeError(ErrorCode::not_found, "domain is not defined")}
               : Result<ExecutionDomain>{*found};
  }

  Result<ExecutionMode> Mode(DomainId domain, ModeId mode) const {
    if (!permissions_.observe) {
      return MakeError(ErrorCode::permission_denied, "coordinator observation is not authorized");
    }
    const auto* definition = engine_->Model().FindMode({domain, mode});
    if (definition == nullptr) {
      return MakeError(ErrorCode::not_found, "mode is not defined in the domain");
    }
    return ExecutionMode{domain, definition->id, definition->name};
  }

  Result<TransitionId> Request(DomainId domain, ModeId mode, TransitionOptions options) {
    if (!permissions_.mutate) {
      return MakeError(ErrorCode::permission_denied, "coordinator mutation is not authorized");
    }
    if (options.timeout <= std::chrono::milliseconds::zero()) {
      return MakeError(ErrorCode::invalid_argument, "transition timeout must be positive");
    }
    const auto* domain_definition = engine_->Model().FindDomain(domain);
    if (domain_definition == nullptr) {
      return MakeError(ErrorCode::not_found, "domain is not defined");
    }

    std::unique_lock lock(mutex_);
    if (stopping_) {
      return MakeError(ErrorCode::backend_unavailable, "coordinator is stopping");
    }
    const auto active = active_domains_.find(domain);
    if (active != active_domains_.end()) {
      if (domain_definition->replacement == ReplacementPolicy::reject_while_busy) {
        return MakeError(ErrorCode::busy, "domain already has an active transition",
                         active->second.value());
      }
      if (domain_definition->replacement == ReplacementPolicy::supersede_if_safe) {
        auto cancelled = engine_->Cancel(active->second);
        if (!cancelled) {
          return MakeError(ErrorCode::busy, "active transition cannot be superseded safely",
                           active->second.value());
        }
        projected_ = engine_->Snapshot().configuration;
      }
    }
    if (queue_.size() >= queue_capacity_) {
      return MakeError(ErrorCode::resource_exhausted, "coordinator transition queue is full");
    }
    const bool queued = active != active_domains_.end() &&
                        domain_definition->replacement == ReplacementPolicy::queue;
    auto accepted =
        queued
            ? engine_->AcceptModeFrom(projected_, domain, mode,
                                      std::chrono::steady_clock::now() + options.timeout)
            : engine_->AcceptMode(domain, mode, std::chrono::steady_clock::now() + options.timeout);
    if (!accepted) {
      return accepted.error();
    }
    const auto id = accepted.value().id;
    projected_.committed_modes[domain] = mode;
    for (const auto application : accepted.value().plan.stop) {
      projected_.running_units.erase(application);
    }
    for (const auto application : accepted.value().plan.start) {
      projected_.running_units.insert(application);
    }
    queue_.push_back(std::move(accepted).value());
    active_domains_[domain] = id;
    ++revision_;
    const CoordinatorEvent event{CoordinatorEventKind::transition_changed, revision_, id, domain};
    std::vector<SystemCoordinator::EventHandler> handlers;
    CollectHandlersLocked(event, handlers);
    lock.unlock();
    work_available_.notify_one();
    changed_.notify_all();
    for (const auto& handler : handlers) {
      try {
        handler(event);
      } catch (...) {
      }
    }
    return id;
  }

  Result<void> Cancel(TransitionId transition) {
    if (!permissions_.mutate) {
      return MakeError(ErrorCode::permission_denied, "coordinator mutation is not authorized");
    }
    return engine_->Cancel(transition);
  }

  Result<TransitionSnapshot> TransitionState(TransitionId transition) const {
    if (!permissions_.observe) {
      return MakeError(ErrorCode::permission_denied, "coordinator observation is not authorized");
    }
    auto snapshot = engine_->GetTransition(transition);
    return snapshot ? Result<TransitionSnapshot>{PublicSnapshot(snapshot.value())}
                    : Result<TransitionSnapshot>{snapshot.error()};
  }

  Result<TransitionSnapshot> Wait(TransitionId transition, Deadline deadline) const {
    std::unique_lock lock(mutex_);
    for (;;) {
      lock.unlock();
      auto snapshot = TransitionState(transition);
      if (!snapshot || IsTerminal(snapshot.value().phase)) {
        return snapshot;
      }
      lock.lock();
      if (changed_.wait_until(lock, deadline) == std::cv_status::timeout) {
        return MakeError(ErrorCode::deadline_exceeded, "transition wait deadline expired");
      }
      if (stopping_) {
        return MakeError(ErrorCode::backend_unavailable, "coordinator is stopping");
      }
    }
  }

  Result<std::function<void()>> Subscribe(EventFilter filter,
                                          SystemCoordinator::EventHandler handler) {
    if (!permissions_.observe) {
      return MakeError(ErrorCode::permission_denied, "coordinator observation is not authorized");
    }
    if (!handler) {
      return MakeError(ErrorCode::invalid_argument, "event handler is required");
    }
    std::lock_guard lock(mutex_);
    if (subscriptions_.size() >= queue_capacity_) {
      return MakeError(ErrorCode::resource_exhausted, "coordinator subscription limit reached");
    }
    const auto id = next_subscription_++;
    subscriptions_.emplace(id, Subscription{std::move(filter), std::move(handler)});
    std::weak_ptr<CoordinatorService> weak = shared_from_this();
    return std::function<void()>([weak, id] {
      if (const auto service = weak.lock()) {
        std::lock_guard guard(service->mutex_);
        service->subscriptions_.erase(id);
      }
    });
  }

private:
  void Worker() {
    for (;;) {
      detail::AcceptedTransition accepted;
      {
        std::unique_lock lock(mutex_);
        work_available_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (stopping_ && queue_.empty()) {
          return;
        }
        accepted = std::move(queue_.front());
        queue_.pop_front();
      }
      const auto domain = accepted.plan.domain;
      const auto id = accepted.id;
      const auto before = engine_->Snapshot().configuration.committed_modes.at(domain);
      auto completed = engine_->Execute(std::move(accepted));
      const auto after = engine_->Snapshot().configuration.committed_modes.at(domain);
      static_cast<void>(completed);

      std::vector<SystemCoordinator::EventHandler> transition_handlers;
      std::vector<SystemCoordinator::EventHandler> configuration_handlers;
      std::vector<SystemCoordinator::EventHandler> recovery_handlers;
      CoordinatorEvent transition_event;
      CoordinatorEvent configuration_event;
      CoordinatorEvent recovery_event;
      {
        std::lock_guard lock(mutex_);
        const auto active = active_domains_.find(domain);
        if (active != active_domains_.end() && active->second == id) {
          active_domains_.erase(active);
        }
        if (queue_.empty() && active_domains_.empty()) {
          projected_ = engine_->Snapshot().configuration;
        }
        ++revision_;
        transition_event = {CoordinatorEventKind::transition_changed, revision_, id, domain};
        CollectHandlersLocked(transition_event, transition_handlers);
        if (completed &&
            (completed.value().phase == TransitionPhase::succeeded || before != after)) {
          configuration_event = {CoordinatorEventKind::configuration_changed, revision_, id,
                                 domain};
          CollectHandlersLocked(configuration_event, configuration_handlers);
        }
        const auto* definition = engine_->Model().FindDomain(domain);
        if (completed && completed.value().failure && definition != nullptr &&
            definition->recovery.action != FailureAction::hold_observed_configuration) {
          recovery_event = {CoordinatorEventKind::recovery_changed, revision_, id, domain};
          CollectHandlersLocked(recovery_event, recovery_handlers);
        }
      }
      changed_.notify_all();
      for (const auto& handler : transition_handlers) {
        try {
          handler(transition_event);
        } catch (...) {
        }
      }
      for (const auto& handler : configuration_handlers) {
        try {
          handler(configuration_event);
        } catch (...) {
        }
      }
      for (const auto& handler : recovery_handlers) {
        try {
          handler(recovery_event);
        } catch (...) {
        }
      }
    }
  }

  void CollectHandlersLocked(const CoordinatorEvent& event,
                             std::vector<SystemCoordinator::EventHandler>& handlers) const {
    for (const auto& [id, subscription] : subscriptions_) {
      static_cast<void>(id);
      if (Matches(subscription.filter, event)) {
        handlers.push_back(subscription.handler);
      }
    }
  }

  std::unique_ptr<detail::ExecutionEngine> engine_;
  detail::CoordinatorPermissions permissions_;
  const std::size_t queue_capacity_;
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  std::condition_variable work_available_;
  std::deque<detail::AcceptedTransition> queue_;
  detail::SystemConfiguration projected_;
  std::unordered_map<DomainId, TransitionId> active_domains_;
  std::unordered_map<std::uint64_t, Subscription> subscriptions_;
  std::vector<std::thread> workers_;
  std::uint64_t revision_{1U};
  std::uint64_t next_subscription_{1U};
  bool recovering_{};
  bool stopping_{};
};

class SystemCoordinator::Impl final {
public:
  explicit Impl(std::shared_ptr<detail::CoordinatorClient> client) : client(std::move(client)) {}
  std::shared_ptr<detail::CoordinatorClient> client;
};

class Transition::Impl final {
public:
  Impl(std::shared_ptr<detail::CoordinatorClient> client, TransitionId id)
      : client(std::move(client)), id(id) {}
  std::shared_ptr<detail::CoordinatorClient> client;
  TransitionId id;
};

class LocalCoordinatorClient final : public detail::CoordinatorClient {
public:
  explicit LocalCoordinatorClient(std::shared_ptr<CoordinatorService> service)
      : service_(std::move(service)) {}

  Result<SystemSnapshot> Snapshot() const override { return service_->Snapshot(); }
  Result<ExecutionDomain> Domain(DomainId domain) const override {
    return service_->Domain(domain);
  }
  Result<ExecutionMode> Mode(DomainId domain, ModeId mode) const override {
    return service_->Mode(domain, mode);
  }
  Result<TransitionId> Request(DomainId domain, ModeId mode, TransitionOptions options) override {
    return service_->Request(domain, mode, options);
  }
  Result<void> Cancel(TransitionId transition) override { return service_->Cancel(transition); }
  Result<TransitionSnapshot> TransitionState(TransitionId transition) const override {
    return service_->TransitionState(transition);
  }
  Result<TransitionSnapshot> Wait(TransitionId transition, Deadline deadline) const override {
    return service_->Wait(transition, deadline);
  }
  Result<std::function<void()>> Subscribe(EventFilter filter,
                                          SystemCoordinator::EventHandler handler) override {
    return service_->Subscribe(std::move(filter), std::move(handler));
  }

private:
  std::shared_ptr<CoordinatorService> service_;
};

EventSubscription::EventSubscription(std::function<void()> unsubscribe)
    : unsubscribe_(std::move(unsubscribe)) {}
EventSubscription::~EventSubscription() { Reset(); }
EventSubscription::EventSubscription(EventSubscription&& other) noexcept
    : unsubscribe_(std::exchange(other.unsubscribe_, std::function<void()>{})) {}
EventSubscription& EventSubscription::operator=(EventSubscription&& other) noexcept {
  if (this != &other) {
    Reset();
    unsubscribe_ = std::exchange(other.unsubscribe_, std::function<void()>{});
  }
  return *this;
}
void EventSubscription::Reset() noexcept {
  if (unsubscribe_) {
    unsubscribe_();
    unsubscribe_ = {};
  }
}
EventSubscription::operator bool() const noexcept { return static_cast<bool>(unsubscribe_); }

Transition::Transition(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
TransitionId Transition::Id() const noexcept { return impl_ ? impl_->id : TransitionId{}; }
Result<TransitionSnapshot> Transition::Get() const {
  return impl_ ? impl_->client->TransitionState(impl_->id)
               : Result<TransitionSnapshot>{
                     MakeError(ErrorCode::invalid_transition, "transition handle is empty")};
}
Result<TransitionSnapshot> Transition::Wait(Deadline deadline) const {
  return impl_ ? impl_->client->Wait(impl_->id, deadline)
               : Result<TransitionSnapshot>{
                     MakeError(ErrorCode::invalid_transition, "transition handle is empty")};
}
Transition::operator bool() const noexcept { return static_cast<bool>(impl_); }

Result<SystemCoordinator> SystemCoordinator::Connect(CoordinatorOptions options) {
  auto connected = detail::ConnectCoordinatorIpc(options);
  return connected ? Result<SystemCoordinator>{detail_CoordinatorFactory::CreateClient(
                         std::move(connected).value())}
                   : Result<SystemCoordinator>{connected.error()};
}
SystemCoordinator::SystemCoordinator(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
SystemCoordinator::~SystemCoordinator() = default;
SystemCoordinator::SystemCoordinator(SystemCoordinator&&) noexcept = default;
SystemCoordinator& SystemCoordinator::operator=(SystemCoordinator&&) noexcept = default;

Result<SystemSnapshot> SystemCoordinator::GetSnapshot() const {
  return impl_ ? impl_->client->Snapshot()
               : Result<SystemSnapshot>{
                     MakeError(ErrorCode::invalid_transition, "coordinator handle is empty")};
}
Result<ExecutionDomain> SystemCoordinator::ResolveDomain(DomainId domain) const {
  return impl_ ? impl_->client->Domain(domain)
               : Result<ExecutionDomain>{
                     MakeError(ErrorCode::invalid_transition, "coordinator handle is empty")};
}
Result<ExecutionMode> SystemCoordinator::ResolveMode(DomainId domain, ModeId mode) const {
  return impl_ ? impl_->client->Mode(domain, mode)
               : Result<ExecutionMode>{
                     MakeError(ErrorCode::invalid_transition, "coordinator handle is empty")};
}
Result<Transition> SystemCoordinator::RequestMode(DomainId domain, ModeId mode,
                                                  TransitionOptions options) {
  if (!impl_) {
    return MakeError(ErrorCode::invalid_transition, "coordinator handle is empty");
  }
  auto requested = impl_->client->Request(domain, mode, options);
  return requested ? Result<Transition>{Transition{
                         std::make_shared<Transition::Impl>(impl_->client, requested.value())}}
                   : Result<Transition>{requested.error()};
}
Result<void> SystemCoordinator::Cancel(TransitionId transition) {
  return impl_ ? impl_->client->Cancel(transition)
               : Result<void>{
                     MakeError(ErrorCode::invalid_transition, "coordinator handle is empty")};
}
Result<EventSubscription> SystemCoordinator::Subscribe(EventFilter filter, EventHandler handler) {
  if (!impl_) {
    return MakeError(ErrorCode::invalid_transition, "coordinator handle is empty");
  }
  auto subscribed = impl_->client->Subscribe(std::move(filter), std::move(handler));
  return subscribed ? Result<EventSubscription>{EventSubscription{std::move(subscribed).value()}}
                    : Result<EventSubscription>{subscribed.error()};
}

Result<SystemCoordinator>
detail_CoordinatorFactory::Create(std::unique_ptr<detail::ExecutionEngine> engine,
                                  detail::CoordinatorPermissions permissions,
                                  detail::CoordinatorServiceOptions options) {
  if (!engine) {
    return MakeError(ErrorCode::invalid_argument, "execution engine is required");
  }
  if (!permissions.observe && permissions.mutate) {
    return MakeError(ErrorCode::invalid_argument,
                     "mutation authority requires observation authority");
  }
  if (options.queue_capacity == 0U || options.worker_count == 0U ||
      options.worker_count > options.queue_capacity) {
    return MakeError(ErrorCode::invalid_argument, "invalid coordinator resource limits");
  }
  try {
    auto service = std::make_shared<CoordinatorService>(std::move(engine), permissions,
                                                        options.queue_capacity);
    service->Start(options.worker_count);
    return CreateClient(std::make_shared<LocalCoordinatorClient>(std::move(service)));
  } catch (...) {
    return MakeError(ErrorCode::resource_exhausted, "cannot create coordinator resources");
  }
}

SystemCoordinator
detail_CoordinatorFactory::CreateClient(std::shared_ptr<detail::CoordinatorClient> client) {
  return SystemCoordinator{std::make_shared<SystemCoordinator::Impl>(std::move(client))};
}

} // namespace ovf::exec
