// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/engine.hpp"

#include <gtest/gtest.h>

#include <unordered_map>

namespace {

using namespace std::chrono_literals;
using namespace ovf::exec;
using namespace ovf::exec::detail;

ValidatedModel Model() {
  ExecutionModel model{
      ModelGeneration{3},
      {
          {ApplicationId{1}, "base", ReadinessPolicy::required, 1s, 1s, {}, {}, {}},
          {ApplicationId{2},
           "feature",
           ReadinessPolicy::required,
           1s,
           1s,
           {},
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

class FakeProcessBackend final : public ProcessBackend {
public:
  Result<BackendEvidence> Inspect(ApplicationId application) noexcept override {
    return BackendEvidence{states[application], 0, 0, 0, {}};
  }

  Result<BackendEvidence> Start(ApplicationId application, Deadline) noexcept override {
    operations.push_back("start:" + std::to_string(application.value()));
    if (fail_start == application) {
      states[application] = ApplicationState::failed;
      return BackendEvidence{ApplicationState::failed, 19, 0, 77, "injected failure"};
    }
    states[application] = ApplicationState::ready;
    return BackendEvidence{ApplicationState::ready, 0, 0, 0, {}};
  }

  Result<BackendEvidence> Stop(ApplicationId application, StopReason, Deadline) noexcept override {
    operations.push_back("stop:" + std::to_string(application.value()));
    states[application] = ApplicationState::stopped;
    return BackendEvidence{ApplicationState::stopped, 0, 0, 0, {}};
  }

  std::unordered_map<ApplicationId, ApplicationState> states{
      {ApplicationId{1}, ApplicationState::ready},
      {ApplicationId{2}, ApplicationState::stopped},
  };
  std::vector<std::string> operations;
  ApplicationId fail_start;
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

} // namespace
