// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/coordinator_server.hpp"
#include "ovf/exec/internal/coordinator_service.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using namespace ovf::exec;
using namespace ovf::exec::detail;

ValidatedModel Model(ReplacementPolicy replacement = ReplacementPolicy::supersede_if_safe,
                     RecoveryPolicy recovery = {}) {
  ExecutionModel model{
      ModelGeneration{8},
      {{ApplicationId{1}, "base", ReadinessPolicy::required, 1s, 1s, {}, {}, {}},
       {ApplicationId{2}, "feature", ReadinessPolicy::required, 1s, 1s, {}, {}, {}}},
      {{DomainId{1},
        "machine",
        ModeId{1},
        replacement,
        recovery,
        {{ModeId{1}, "idle", {ApplicationId{1}}, {}},
         {ModeId{2}, "active", {ApplicationId{1}, ApplicationId{2}}, {}}}}}};
  auto validated = ValidateModel(std::move(model));
  EXPECT_TRUE(validated);
  return std::move(validated).value();
}

class BlockingBackend final : public ProcessBackend {
public:
  Result<BackendEvidence> Inspect(ApplicationId application) noexcept override {
    std::lock_guard lock(mutex_);
    return BackendEvidence{states_[application], 0, 0, 0, {}};
  }

  Result<BackendEvidence> Start(ApplicationId application, Deadline deadline) noexcept override {
    std::unique_lock lock(mutex_);
    start_entered_ = true;
    entered_.notify_all();
    if (!released_.wait_until(lock, deadline, [this] { return release_; })) {
      return MakeError(ErrorCode::deadline_exceeded, "injected start deadline");
    }
    if (fail_start_) {
      states_[application] = ApplicationState::failed;
      return BackendEvidence{ApplicationState::failed, 31, 0, 71, "injected failure"};
    }
    states_[application] = ApplicationState::ready;
    return BackendEvidence{ApplicationState::ready, 0, 0, 0, {}};
  }

  Result<BackendEvidence> Stop(ApplicationId application, StopReason, Deadline) noexcept override {
    std::lock_guard lock(mutex_);
    states_[application] = ApplicationState::stopped;
    return BackendEvidence{ApplicationState::stopped, 0, 0, 0, {}};
  }

  bool WaitUntilStart(Deadline deadline) {
    std::unique_lock lock(mutex_);
    return entered_.wait_until(lock, deadline, [this] { return start_entered_; });
  }

  void Release() {
    std::lock_guard lock(mutex_);
    release_ = true;
    released_.notify_all();
  }

  void FailStart() {
    std::lock_guard lock(mutex_);
    fail_start_ = true;
  }

private:
  std::mutex mutex_;
  std::condition_variable entered_;
  std::condition_variable released_;
  std::unordered_map<ApplicationId, ApplicationState> states_{
      {ApplicationId{1}, ApplicationState::ready},
      {ApplicationId{2}, ApplicationState::stopped},
  };
  bool start_entered_{};
  bool release_{};
  bool fail_start_{};
};

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    std::string pattern = "/tmp/ovf-exec-ipc-XXXXXX";
    storage_.assign(pattern.begin(), pattern.end());
    storage_.push_back('\0');
    const auto* created = ::mkdtemp(storage_.data());
    if (created != nullptr) {
      path_ = created;
    }
  }
  ~TemporaryDirectory() {
    if (!path_.empty()) {
      ::rmdir(path_.c_str());
    }
  }
  std::string Socket() const { return path_ + "/coordinator.sock"; }

private:
  std::vector<char> storage_;
  std::string path_;
};

std::unique_ptr<ExecutionEngine> Engine(ValidatedModel model,
                                        BlockingBackend** observer = nullptr) {
  auto backend = std::make_unique<BlockingBackend>();
  if (observer != nullptr) {
    *observer = backend.get();
  }
  auto created = ExecutionEngine::Create(std::move(model), std::move(backend),
                                         std::make_unique<MemoryTransitionJournal>());
  EXPECT_TRUE(created);
  return std::move(created).value();
}

TEST(CoordinatorTest, ReturnsAcceptedHandleBeforeAsynchronousExecutionCompletes) {
  BlockingBackend* backend{};
  auto created = detail_CoordinatorFactory::Create(Engine(Model(), &backend),
                                                   {.observe = true, .mutate = true});
  ASSERT_TRUE(created);
  auto coordinator = std::move(created).value();

  std::atomic_uint events{};
  auto subscription = coordinator.Subscribe(
      {}, [&events](const CoordinatorEvent&) { events.fetch_add(1U, std::memory_order_relaxed); });
  ASSERT_TRUE(subscription);

  auto requested = coordinator.RequestMode(DomainId{1}, ModeId{2}, {.timeout = 2s});
  ASSERT_TRUE(requested);
  auto transition = std::move(requested).value();
  EXPECT_TRUE(transition);
  ASSERT_TRUE(backend->WaitUntilStart(std::chrono::steady_clock::now() + 1s));

  auto in_flight = transition.Get();
  ASSERT_TRUE(in_flight);
  EXPECT_FALSE(in_flight.value().phase == TransitionPhase::succeeded);

  backend->Release();
  auto completed = transition.Wait(std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(completed);
  EXPECT_EQ(completed.value().phase, TransitionPhase::succeeded);

  auto snapshot = coordinator.GetSnapshot();
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot.value().model_generation, ModelGeneration{8});
  ASSERT_EQ(snapshot.value().domains.size(), 1U);
  EXPECT_EQ(snapshot.value().domains.front().committed_mode, ModeId{2});
  EXPECT_EQ(snapshot.value().domains.front().status, DomainStatus::stable);
  EXPECT_GE(events.load(std::memory_order_relaxed), 2U);
}

TEST(CoordinatorTest, EnforcesPermissionsAssignedToTheSession) {
  auto created =
      detail_CoordinatorFactory::Create(Engine(Model()), {.observe = true, .mutate = false});
  ASSERT_TRUE(created);
  auto coordinator = std::move(created).value();

  EXPECT_TRUE(coordinator.GetSnapshot());
  auto requested = coordinator.RequestMode(DomainId{1}, ModeId{2});
  ASSERT_FALSE(requested);
  EXPECT_EQ(requested.error().code, ErrorCode::permission_denied);
}

TEST(CoordinatorTest, RejectsReplacementWhenDomainPolicyRequiresSerialization) {
  BlockingBackend* backend{};
  auto created = detail_CoordinatorFactory::Create(
      Engine(Model(ReplacementPolicy::reject_while_busy), &backend),
      {.observe = true, .mutate = true});
  ASSERT_TRUE(created);
  auto coordinator = std::move(created).value();

  auto first = coordinator.RequestMode(DomainId{1}, ModeId{2}, {.timeout = 2s});
  ASSERT_TRUE(first);
  ASSERT_TRUE(backend->WaitUntilStart(std::chrono::steady_clock::now() + 1s));
  auto second = coordinator.RequestMode(DomainId{1}, ModeId{1}, {.timeout = 2s});
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error().code, ErrorCode::busy);
  backend->Release();
  EXPECT_TRUE(first.value().Wait(std::chrono::steady_clock::now() + 1s));
}

TEST(CoordinatorTest, PlansQueuedRequestAgainstEarlierAcceptedTarget) {
  BlockingBackend* backend{};
  auto created = detail_CoordinatorFactory::Create(
      Engine(Model(ReplacementPolicy::queue), &backend), {.observe = true, .mutate = true});
  ASSERT_TRUE(created);
  auto coordinator = std::move(created).value();

  auto enter_active = coordinator.RequestMode(DomainId{1}, ModeId{2}, {.timeout = 2s});
  ASSERT_TRUE(enter_active);
  ASSERT_TRUE(backend->WaitUntilStart(std::chrono::steady_clock::now() + 1s));
  auto return_idle = coordinator.RequestMode(DomainId{1}, ModeId{1}, {.timeout = 2s});
  ASSERT_TRUE(return_idle);

  backend->Release();
  auto first = enter_active.value().Wait(std::chrono::steady_clock::now() + 1s);
  auto second = return_idle.value().Wait(std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(first.value().phase, TransitionPhase::succeeded);
  EXPECT_EQ(second.value().phase, TransitionPhase::succeeded);

  auto domain = coordinator.ResolveDomain(DomainId{1});
  ASSERT_TRUE(domain);
  EXPECT_EQ(domain.value().committed_mode, ModeId{1});
}

TEST(CoordinatorTest, ServesAuthenticatedCoordinatorApiAcrossUnixSocket) {
  BlockingBackend* backend{};
  auto local = detail_CoordinatorFactory::Create(Engine(Model(), &backend),
                                                 {.observe = true, .mutate = true});
  ASSERT_TRUE(local);
  TemporaryDirectory temporary;
  const auto endpoint = temporary.Socket();
  const auto uid = static_cast<std::uint32_t>(::getuid());
  auto server = StartCoordinatorServer(std::move(local).value(), {.endpoint = endpoint,
                                                                  .observation_uids = {uid},
                                                                  .mutation_uids = {uid},
                                                                  .connection_capacity = 16U,
                                                                  .worker_count = 2U,
                                                                  .maximum_message_size = 65536U});
  ASSERT_TRUE(server) << server.error().message;

  auto connected = SystemCoordinator::Connect({endpoint, 1s});
  ASSERT_TRUE(connected) << connected.error().message;
  auto coordinator = std::move(connected).value();
  auto initial_snapshot = coordinator.GetSnapshot();
  ASSERT_TRUE(initial_snapshot) << initial_snapshot.error().message;
  auto mode = coordinator.ResolveMode(DomainId{1}, ModeId{2});
  ASSERT_TRUE(mode);
  EXPECT_EQ(mode.value().name, "active");

  std::atomic_uint events{};
  auto subscription = coordinator.Subscribe(
      {}, [&events](const CoordinatorEvent&) { events.fetch_add(1U, std::memory_order_relaxed); });
  ASSERT_TRUE(subscription);
  auto transition = coordinator.RequestMode(DomainId{1}, ModeId{2}, {.timeout = 2s});
  ASSERT_TRUE(transition);
  ASSERT_TRUE(backend->WaitUntilStart(std::chrono::steady_clock::now() + 1s));
  backend->Release();
  auto completed = transition.value().Wait(std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(completed);
  EXPECT_EQ(completed.value().phase, TransitionPhase::succeeded);
  for (int attempt = 0; attempt < 100 && events.load(std::memory_order_relaxed) == 0U; ++attempt) {
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_GT(events.load(std::memory_order_relaxed), 0U);
}

TEST(CoordinatorTest, DeliversRecoveryEvidenceAndEventsAcrossUnixSocket) {
  BlockingBackend* backend{};
  auto local = detail_CoordinatorFactory::Create(
      Engine(Model(ReplacementPolicy::supersede_if_safe,
                   {FailureAction::enter_fallback_mode, ModeId{1}, 1s}),
             &backend),
      {.observe = true, .mutate = true});
  ASSERT_TRUE(local);
  TemporaryDirectory temporary;
  const auto endpoint = temporary.Socket();
  const auto uid = static_cast<std::uint32_t>(::getuid());
  auto server = StartCoordinatorServer(std::move(local).value(), {.endpoint = endpoint,
                                                                  .observation_uids = {uid},
                                                                  .mutation_uids = {uid},
                                                                  .connection_capacity = 16U,
                                                                  .worker_count = 2U,
                                                                  .maximum_message_size = 65536U});
  ASSERT_TRUE(server);
  auto connected = SystemCoordinator::Connect({endpoint, 1s});
  ASSERT_TRUE(connected);
  auto coordinator = std::move(connected).value();
  std::atomic_uint recovery_events{};
  auto subscription =
      coordinator.Subscribe({.transitions = false,
                             .configuration = false,
                             .recovery = true,
                             .domain = std::nullopt},
                            [&recovery_events](const CoordinatorEvent& event) {
                              if (event.kind == CoordinatorEventKind::recovery_changed) {
                                recovery_events.fetch_add(1U, std::memory_order_relaxed);
                              }
                            });
  ASSERT_TRUE(subscription);
  backend->FailStart();
  auto transition = coordinator.RequestMode(DomainId{1}, ModeId{2}, {.timeout = 2s});
  ASSERT_TRUE(transition);
  ASSERT_TRUE(backend->WaitUntilStart(std::chrono::steady_clock::now() + 1s));
  backend->Release();
  auto completed = transition.value().Wait(std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(completed);
  ASSERT_TRUE(completed.value().failure);
  EXPECT_EQ(completed.value().phase, TransitionPhase::failed);
  EXPECT_EQ(completed.value().failure->recovery_action, FailureAction::enter_fallback_mode);
  EXPECT_TRUE(completed.value().failure->recovery_attempted);
  EXPECT_TRUE(completed.value().failure->recovery_succeeded);
  EXPECT_EQ(completed.value().failure->recovered_mode, ModeId{1});
  for (int attempt = 0; attempt < 100 && recovery_events.load(std::memory_order_relaxed) == 0U;
       ++attempt) {
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_GT(recovery_events.load(std::memory_order_relaxed), 0U);
}

TEST(CoordinatorTest, RejectsPeerWithoutGeneratedObservationAuthority) {
  auto local =
      detail_CoordinatorFactory::Create(Engine(Model()), {.observe = true, .mutate = true});
  ASSERT_TRUE(local);
  TemporaryDirectory temporary;
  const auto endpoint = temporary.Socket();
  const auto other_uid = static_cast<std::uint32_t>(::getuid()) + 1U;
  auto server = StartCoordinatorServer(std::move(local).value(), {.endpoint = endpoint,
                                                                  .observation_uids = {other_uid},
                                                                  .mutation_uids = {other_uid},
                                                                  .connection_capacity = 4U,
                                                                  .worker_count = 1U,
                                                                  .maximum_message_size = 65536U});
  ASSERT_TRUE(server);

  auto connected = SystemCoordinator::Connect({endpoint, 1s});
  ASSERT_FALSE(connected);
  EXPECT_EQ(connected.error().code, ErrorCode::permission_denied);
}

} // namespace
