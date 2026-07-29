// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/engine.hpp"

#include <gtest/gtest.h>

#include <unordered_map>

namespace {

using namespace std::chrono_literals;
using namespace ovf::exec;
using namespace ovf::exec::detail;

ValidatedModel Model(std::uint32_t feature_attempts = 1U,
                     ReadinessPolicy feature_readiness = ReadinessPolicy::required) {
  ExecutionModel model{
      ModelGeneration{3},
      {
          {ApplicationId{1}, "base", ReadinessPolicy::required, 1s, 1s, {}, {}, {}},
          {ApplicationId{2},
           "feature",
           feature_readiness,
           1s,
           1s,
           {feature_attempts, 1ms},
           {ApplicationId{1}},
           {}},
      },
      {{DomainId{1},
        "machine",
        ModeId{1},
        ReplacementPolicy::supersede_if_safe,
        {},
        {{ModeId{1}, "startup", {ApplicationId{1}}, {}},
         {ModeId{2}, "operational", {ApplicationId{1}, ApplicationId{2}}, {}}}}}};
  auto result = ValidateModel(std::move(model));
  EXPECT_TRUE(result);
  return std::move(result).value();
}

ValidatedModel RecoveryModel(FailureAction action) {
  auto model = Model().value();
  ExecutionModel recovered{model.generation,
                           model.applications,
                           {{DomainId{1},
                             "machine",
                             ModeId{1},
                             ReplacementPolicy::supersede_if_safe,
                             {action, ModeId{3}, 1s},
                             {{ModeId{1}, "startup", {ApplicationId{1}}, {}},
                              {ModeId{2}, "operational", {ApplicationId{1}, ApplicationId{2}}, {}},
                              {ModeId{3}, "fallback", {}, {}}}}}};
  auto result = ValidateModel(std::move(recovered));
  EXPECT_TRUE(result);
  return std::move(result).value();
}

class FakeProcessBackend final : public ProcessBackend {
public:
  Result<BackendEvidence> Inspect(ApplicationId application) noexcept override {
    return BackendEvidence{states[application], 0, 0, 0, {}};
  }

  Result<BackendEvidence> Start(ApplicationId application, Deadline) noexcept override {
    operations.push_back("start:" + std::to_string(application.value()));
    const auto failures = failures_remaining.find(application);
    if (fail_start == application ||
        (failures != failures_remaining.end() && failures->second > 0U)) {
      if (failures != failures_remaining.end() && failures->second > 0U) {
        --failures->second;
      }
      states[application] = ApplicationState::failed;
      return BackendEvidence{ApplicationState::failed, 19, 0, 77, "injected failure"};
    }
    states[application] = start_state;
    return BackendEvidence{start_state, 0, 0, 0, {}};
  }

  Result<BackendEvidence> Stop(ApplicationId application, StopReason, Deadline) noexcept override {
    operations.push_back("stop:" + std::to_string(application.value()));
    if (fail_stop == application) {
      states[application] = ApplicationState::failed;
      return BackendEvidence{ApplicationState::failed, 23, 0, 91, "injected stop failure"};
    }
    states[application] = ApplicationState::stopped;
    return BackendEvidence{ApplicationState::stopped, 0, 0, 0, {}};
  }

  Result<void> RequestSystemRecovery(Deadline) noexcept override {
    system_recovery_requested = true;
    return {};
  }

  std::unordered_map<ApplicationId, ApplicationState> states{
      {ApplicationId{1}, ApplicationState::ready},
      {ApplicationId{2}, ApplicationState::stopped},
  };
  std::vector<std::string> operations;
  std::unordered_map<ApplicationId, std::uint32_t> failures_remaining;
  ApplicationId fail_start;
  ApplicationId fail_stop;
  ApplicationState start_state{ApplicationState::ready};
  bool system_recovery_requested{};
};

TEST(ExecutionEngineTest, ExecutesAndAtomicallyCommitsMode) {
  auto backend = std::make_unique<FakeProcessBackend>();
  auto* backend_observer = backend.get();
  auto journal = std::make_unique<MemoryTransitionJournal>();
  auto* journal_observer = journal.get();
  auto created = ExecutionEngine::Create(Model(), std::move(backend), std::move(journal));
  ASSERT_TRUE(created);
  auto engine = std::move(created).value();

  auto transition =
      engine->RequestMode(DomainId{1}, ModeId{2}, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(transition);
  EXPECT_EQ(transition.value().phase, TransitionPhase::succeeded);
  EXPECT_EQ(backend_observer->operations, std::vector<std::string>({"start:2"}));

  const auto snapshot = engine->Snapshot();
  EXPECT_EQ(snapshot.configuration.committed_modes.at(DomainId{1}), ModeId{2});
  EXPECT_TRUE(snapshot.configuration.running_applications.contains(ApplicationId{2}));

  const auto events = journal_observer->Events();
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back().transition.phase, TransitionPhase::succeeded);
}

TEST(ExecutionEngineTest, PreservesObservedStateAndEvidenceOnPartialFailure) {
  auto backend = std::make_unique<FakeProcessBackend>();
  backend->fail_start = ApplicationId{2};
  auto journal = std::make_unique<MemoryTransitionJournal>();
  auto created = ExecutionEngine::Create(Model(), std::move(backend), std::move(journal));
  ASSERT_TRUE(created);
  auto engine = std::move(created).value();

  auto transition =
      engine->RequestMode(DomainId{1}, ModeId{2}, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(transition);
  EXPECT_EQ(transition.value().phase, TransitionPhase::failed);
  ASSERT_TRUE(transition.value().failure);
  ASSERT_TRUE(transition.value().failure.has_value());
  EXPECT_EQ(transition.value().failure->application, ApplicationId{2});
  EXPECT_EQ(transition.value().failure->evidence.exit_code, 19);
  EXPECT_EQ(transition.value().failure->evidence.native_code, 77U);

  const auto snapshot = engine->Snapshot();
  EXPECT_EQ(snapshot.configuration.committed_modes.at(DomainId{1}), ModeId{1});
  EXPECT_FALSE(snapshot.configuration.running_applications.contains(ApplicationId{2}));
}

TEST(ExecutionEngineTest, RejectsInvalidRequestsWithoutBackendEffects) {
  auto backend = std::make_unique<FakeProcessBackend>();
  auto* observer = backend.get();
  auto created = ExecutionEngine::Create(Model(), std::move(backend),
                                         std::make_unique<MemoryTransitionJournal>());
  ASSERT_TRUE(created);
  auto engine = std::move(created).value();

  auto transition =
      engine->RequestMode(DomainId{99}, ModeId{2}, std::chrono::steady_clock::now() + 1s);
  ASSERT_FALSE(transition);
  EXPECT_EQ(transition.error().code, ErrorCode::not_found);
  EXPECT_TRUE(observer->operations.empty());
}

TEST(ExecutionEngineTest, RetriesFailedStartsWithinDeploymentPolicy) {
  auto backend = std::make_unique<FakeProcessBackend>();
  auto* observer = backend.get();
  observer->failures_remaining[ApplicationId{2}] = 2U;
  auto created = ExecutionEngine::Create(Model(3U), std::move(backend),
                                         std::make_unique<MemoryTransitionJournal>());
  ASSERT_TRUE(created);

  auto transition =
      created.value()->RequestMode(DomainId{1}, ModeId{2}, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(transition);
  EXPECT_EQ(transition.value().phase, TransitionPhase::succeeded);
  EXPECT_EQ(observer->operations,
            (std::vector<std::string>{"start:2", "stop:2", "start:2", "stop:2", "start:2"}));
}

TEST(ExecutionEngineTest, AcceptsProcessStartedReadinessCondition) {
  auto backend = std::make_unique<FakeProcessBackend>();
  backend->start_state = ApplicationState::starting;
  auto created =
      ExecutionEngine::Create(Model(1U, ReadinessPolicy::process_started), std::move(backend),
                              std::make_unique<MemoryTransitionJournal>());
  ASSERT_TRUE(created);

  auto transition =
      created.value()->RequestMode(DomainId{1}, ModeId{2}, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(transition);
  EXPECT_EQ(transition.value().phase, TransitionPhase::succeeded);
}

TEST(ExecutionEngineTest, EntersConfiguredFallbackAfterTransitionFailure) {
  auto backend = std::make_unique<FakeProcessBackend>();
  auto* observer = backend.get();
  observer->fail_start = ApplicationId{2};
  auto journal = std::make_unique<MemoryTransitionJournal>();
  auto* journal_observer = journal.get();
  auto created = ExecutionEngine::Create(RecoveryModel(FailureAction::enter_fallback_mode),
                                         std::move(backend), std::move(journal));
  ASSERT_TRUE(created);

  auto transition =
      created.value()->RequestMode(DomainId{1}, ModeId{2}, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(transition);
  EXPECT_EQ(transition.value().phase, TransitionPhase::failed);
  ASSERT_TRUE(transition.value().failure);
  EXPECT_TRUE(transition.value().failure->recovery_attempted);
  EXPECT_TRUE(transition.value().failure->recovery_succeeded);
  EXPECT_EQ(transition.value().failure->recovered_mode, ModeId{3});
  EXPECT_EQ(created.value()->Snapshot().configuration.committed_modes.at(DomainId{1}), ModeId{3});
  EXPECT_FALSE(
      created.value()->Snapshot().configuration.running_applications.contains(ApplicationId{1}));
  EXPECT_EQ(observer->operations, (std::vector<std::string>{"start:2", "stop:1"}));
  const auto events = journal_observer->Events();
  EXPECT_TRUE(std::any_of(events.begin(), events.end(), [](const auto& event) {
    return event.transition.phase == TransitionPhase::recovering;
  }));
}

TEST(ExecutionEngineTest, StopsDomainApplicationsAfterTransitionFailure) {
  auto backend = std::make_unique<FakeProcessBackend>();
  auto* observer = backend.get();
  observer->fail_start = ApplicationId{2};
  auto created =
      ExecutionEngine::Create(RecoveryModel(FailureAction::stop_domain), std::move(backend),
                              std::make_unique<MemoryTransitionJournal>());
  ASSERT_TRUE(created);

  auto transition =
      created.value()->RequestMode(DomainId{1}, ModeId{2}, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(transition);
  EXPECT_EQ(transition.value().phase, TransitionPhase::failed);
  EXPECT_TRUE(created.value()->Snapshot().configuration.running_applications.empty());
  EXPECT_EQ(observer->operations, (std::vector<std::string>{"start:2", "stop:1"}));
}

TEST(ExecutionEngineTest, ReportsRecoveryFailureWithoutCommittingFallback) {
  auto backend = std::make_unique<FakeProcessBackend>();
  backend->fail_start = ApplicationId{2};
  backend->fail_stop = ApplicationId{1};
  auto created =
      ExecutionEngine::Create(RecoveryModel(FailureAction::enter_fallback_mode), std::move(backend),
                              std::make_unique<MemoryTransitionJournal>());
  ASSERT_TRUE(created);

  auto transition =
      created.value()->RequestMode(DomainId{1}, ModeId{2}, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(transition);
  EXPECT_EQ(transition.value().phase, TransitionPhase::recovery_failed);
  ASSERT_TRUE(transition.value().failure);
  EXPECT_TRUE(transition.value().failure->recovery_error.has_value());
  EXPECT_FALSE(transition.value().failure->recovery_succeeded);
  EXPECT_EQ(created.value()->Snapshot().configuration.committed_modes.at(DomainId{1}), ModeId{1});
}

TEST(ExecutionEngineTest, EscalatesConfiguredSystemRecovery) {
  auto backend = std::make_unique<FakeProcessBackend>();
  auto* observer = backend.get();
  observer->fail_start = ApplicationId{2};
  auto created =
      ExecutionEngine::Create(RecoveryModel(FailureAction::request_system_recovery),
                              std::move(backend), std::make_unique<MemoryTransitionJournal>());
  ASSERT_TRUE(created);

  auto transition =
      created.value()->RequestMode(DomainId{1}, ModeId{2}, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(transition);
  EXPECT_EQ(transition.value().phase, TransitionPhase::failed);
  ASSERT_TRUE(transition.value().failure);
  EXPECT_EQ(transition.value().failure->recovery_action, FailureAction::request_system_recovery);
  EXPECT_TRUE(transition.value().failure->recovery_succeeded);
  EXPECT_TRUE(observer->system_recovery_requested);
}

} // namespace
