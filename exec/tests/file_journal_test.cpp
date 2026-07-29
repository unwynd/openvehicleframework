// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/file_journal.hpp"

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using namespace ovf::exec;
using namespace ovf::exec::detail;

class TemporaryFile final {
public:
  TemporaryFile() {
    const auto* directory = std::getenv("TEST_TMPDIR");
    std::string pattern =
        std::string{directory == nullptr ? "/tmp" : directory} + "/ovf-exec-journal-XXXXXX";
    storage_.assign(pattern.begin(), pattern.end());
    storage_.push_back('\0');
    const int descriptor = ::mkstemp(storage_.data());
    if (descriptor >= 0) {
      ::close(descriptor);
      path_ = storage_.data();
    }
  }

  ~TemporaryFile() {
    if (!path_.empty()) {
      ::unlink(path_.c_str());
    }
  }

  const std::string& path() const noexcept { return path_; }

private:
  std::vector<char> storage_;
  std::string path_;
};

JournalEvent Event(TransitionPhase phase) {
  TransitionPlan plan{
      ModelGeneration{7}, DomainId{1},        ModeId{2},          ModeId{3},
      {ApplicationId{1}}, {ApplicationId{2}}, {ApplicationId{3}}, {DomainId{1}, DomainId{4}},
      {ResourceId{9}},
  };
  TransitionSnapshot transition{
      TransitionId{11},
      DomainId{1},
      ModeId{2},
      ModeId{3},
      phase,
      std::chrono::steady_clock::now(),
      TransitionFailure{
          .error = MakeError(ErrorCode::backend_error, "backend failed", 42),
          .application = ApplicationId{3},
          .evidence = {ApplicationState::failed, 17, 9, 88, "native evidence"},
          .recovery_action = FailureAction::enter_fallback_mode,
          .recovery_attempted = true,
          .recovery_succeeded = true,
          .recovery_error = std::nullopt,
          .recovered_mode = ModeId{4},
          .recovery_stopped_applications = {ApplicationId{2}},
          .recovery_started_applications = {ApplicationId{3}},
      },
  };
  return {std::move(transition), ModelGeneration{7}, std::move(plan)};
}

ValidatedModel Model() {
  ExecutionModel model{ModelGeneration{7},
                       {{ApplicationId{1},
                         "base",
                         ReadinessPolicy::required,
                         std::chrono::milliseconds{1000},
                         std::chrono::milliseconds{1000},
                         {},
                         {},
                         {}},
                        {ApplicationId{2},
                         "feature",
                         ReadinessPolicy::required,
                         std::chrono::milliseconds{1000},
                         std::chrono::milliseconds{1000},
                         {},
                         {},
                         {}}},
                       {{DomainId{1},
                         "machine",
                         ModeId{1},
                         ReplacementPolicy::queue,
                         {},
                         {{ModeId{1}, "idle", {ApplicationId{1}}, {}},
                          {ModeId{2}, "active", {ApplicationId{1}, ApplicationId{2}}, {}}}}}};
  auto validated = ValidateModel(std::move(model));
  EXPECT_TRUE(validated);
  return std::move(validated).value();
}

ValidatedModel RecoveryModel() {
  ExecutionModel model = Model().value();
  model.domains[0].recovery = {FailureAction::enter_fallback_mode, ModeId{3},
                               std::chrono::seconds{1}};
  model.domains[0].modes.push_back({ModeId{3}, "fallback", {}, {}});
  auto validated = ValidateModel(std::move(model));
  EXPECT_TRUE(validated);
  return std::move(validated).value();
}

class ReplayBackend final : public ProcessBackend {
public:
  explicit ReplayBackend(bool feature_ready, bool base_ready = true,
                         bool fail_feature_start = false)
      : fail_feature_start_(fail_feature_start) {
    states_[ApplicationId{1}] = base_ready ? ApplicationState::ready : ApplicationState::stopped;
    states_[ApplicationId{2}] = feature_ready ? ApplicationState::ready : ApplicationState::stopped;
  }

  Result<BackendEvidence> Inspect(ApplicationId application) noexcept override {
    return BackendEvidence{states_[application], 0, 0, 0, {}};
  }
  Result<BackendEvidence> Start(ApplicationId application, Deadline) noexcept override {
    operations.push_back("start:" + std::to_string(application.value()));
    if (fail_feature_start_ && application == ApplicationId{2}) {
      states_[application] = ApplicationState::failed;
      return BackendEvidence{ApplicationState::failed, 41, 0, 82, "recovery test failure"};
    }
    states_[application] = ApplicationState::ready;
    return BackendEvidence{ApplicationState::ready, 0, 0, 0, {}};
  }
  Result<BackendEvidence> Stop(ApplicationId application, StopReason, Deadline) noexcept override {
    operations.push_back("stop:" + std::to_string(application.value()));
    states_[application] = ApplicationState::stopped;
    return BackendEvidence{ApplicationState::stopped, 0, 0, 0, {}};
  }

  std::vector<std::string> operations;

private:
  std::unordered_map<ApplicationId, ApplicationState> states_;
  bool fail_feature_start_{};
};

TEST(FileJournalTest, RoundTripsCompleteTransitionEvidenceAndPlan) {
  TemporaryFile file;
  ASSERT_FALSE(file.path().empty());
  auto opened = OpenFileTransitionJournal({file.path()});
  ASSERT_TRUE(opened);
  auto journal = std::move(opened).value();
  ASSERT_TRUE(journal->Append(Event(TransitionPhase::failed)));

  auto replayed = journal->Replay();
  ASSERT_TRUE(replayed);
  ASSERT_EQ(replayed.value().size(), 1U);
  const auto& event = replayed.value().front();
  EXPECT_EQ(event.generation, ModelGeneration{7});
  EXPECT_EQ(event.transition.id, TransitionId{11});
  EXPECT_EQ(event.transition.failure->error.message, "backend failed");
  EXPECT_EQ(event.transition.failure->evidence.exit_code, 17);
  EXPECT_TRUE(event.transition.failure->recovery_succeeded);
  EXPECT_EQ(event.transition.failure->recovered_mode, ModeId{4});
  EXPECT_EQ(event.transition.failure->recovery_stopped_applications,
            (std::vector<ApplicationId>{ApplicationId{2}}));
  ASSERT_TRUE(event.plan);
  EXPECT_EQ(event.plan->start, std::vector<ApplicationId>({ApplicationId{3}}));
  EXPECT_EQ(event.plan->affected_resources, std::vector<ResourceId>({ResourceId{9}}));
}

TEST(FileJournalTest, PreventsASecondWriter) {
  TemporaryFile file;
  auto first = OpenFileTransitionJournal({file.path()});
  ASSERT_TRUE(first);
  auto second = OpenFileTransitionJournal({file.path()});
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error().code, ErrorCode::busy);
}

TEST(FileJournalTest, RepairsTornTailBeforeLaterAppends) {
  TemporaryFile file;
  {
    auto opened = OpenFileTransitionJournal({file.path()});
    ASSERT_TRUE(opened);
    ASSERT_TRUE(opened.value()->Append(Event(TransitionPhase::planned)));
  }
  const int descriptor = ::open(file.path().c_str(), O_WRONLY | O_APPEND);
  ASSERT_GE(descriptor, 0);
  const std::uint8_t torn[]{0x4FU, 0x56U, 0x46U};
  ASSERT_EQ(::write(descriptor, torn, sizeof(torn)), static_cast<ssize_t>(sizeof(torn)));
  ::close(descriptor);

  auto reopened = OpenFileTransitionJournal({file.path()});
  ASSERT_TRUE(reopened);
  auto replayed = reopened.value()->Replay();
  ASSERT_TRUE(replayed);
  ASSERT_EQ(replayed.value().size(), 1U);
  ASSERT_TRUE(reopened.value()->Append(Event(TransitionPhase::succeeded)));
  replayed = reopened.value()->Replay();
  ASSERT_TRUE(replayed);
  ASSERT_EQ(replayed.value().size(), 2U);
  EXPECT_EQ(replayed.value().back().transition.phase, TransitionPhase::succeeded);
}

TEST(FileJournalTest, RestoresCommittedConfigurationAndTransitionSequence) {
  using namespace std::chrono_literals;
  TemporaryFile file;
  {
    auto journal = OpenFileTransitionJournal({file.path()});
    ASSERT_TRUE(journal);
    auto engine = ExecutionEngine::Create(Model(), std::make_unique<ReplayBackend>(false),
                                          std::move(journal).value());
    ASSERT_TRUE(engine);
    auto completed =
        engine.value()->RequestMode(DomainId{1}, ModeId{2}, std::chrono::steady_clock::now() + 1s);
    ASSERT_TRUE(completed);
    EXPECT_EQ(completed.value().id, TransitionId{1});
  }

  auto journal = OpenFileTransitionJournal({file.path()});
  ASSERT_TRUE(journal);
  auto backend = std::make_unique<ReplayBackend>(false);
  auto* backend_observer = backend.get();
  auto restored = ExecutionEngine::Create(Model(), std::move(backend), std::move(journal).value());
  ASSERT_TRUE(restored);
  auto snapshot = restored.value()->Snapshot();
  EXPECT_EQ(snapshot.configuration.committed_modes.at(DomainId{1}), ModeId{2});
  EXPECT_TRUE(snapshot.configuration.running_applications.contains(ApplicationId{2}));
  EXPECT_EQ(backend_observer->operations, std::vector<std::string>({"start:2"}));

  auto completed =
      restored.value()->RequestMode(DomainId{1}, ModeId{1}, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(completed);
  EXPECT_EQ(completed.value().id, TransitionId{2});
  EXPECT_EQ(completed.value().phase, TransitionPhase::succeeded);
}

TEST(FileJournalTest, PreservesObservedStateAndFinalizesInterruptedTransition) {
  TemporaryFile file;
  auto journal = OpenFileTransitionJournal({file.path()});
  ASSERT_TRUE(journal);
  auto journal_owner = std::move(journal).value();
  JournalEvent interrupted{
      TransitionSnapshot{
          TransitionId{5},
          DomainId{1},
          ModeId{1},
          ModeId{2},
          TransitionPhase::starting,
          std::chrono::steady_clock::now(),
          std::nullopt,
      },
      ModelGeneration{7},
      TransitionPlan{
          ModelGeneration{7},
          DomainId{1},
          ModeId{1},
          ModeId{2},
          {ApplicationId{1}},
          {},
          {ApplicationId{2}},
          {DomainId{1}},
          {},
      },
  };
  ASSERT_TRUE(journal_owner->Append(interrupted));
  journal_owner.reset();

  auto reopened = OpenFileTransitionJournal({file.path()});
  ASSERT_TRUE(reopened);
  auto restored = ExecutionEngine::Create(Model(), std::make_unique<ReplayBackend>(true),
                                          std::move(reopened).value());
  ASSERT_TRUE(restored);
  const auto snapshot = restored.value()->Snapshot();
  EXPECT_EQ(snapshot.configuration.committed_modes.at(DomainId{1}), ModeId{1});
  EXPECT_TRUE(snapshot.configuration.running_applications.contains(ApplicationId{2}));
  auto transition = restored.value()->GetTransition(TransitionId{5});
  ASSERT_TRUE(transition);
  EXPECT_EQ(transition.value().phase, TransitionPhase::failed);
  ASSERT_TRUE(transition.value().failure);
  EXPECT_EQ(transition.value().failure->error.code, ErrorCode::backend_unavailable);
}

TEST(FileJournalTest, RestoresFallbackCommitAndRecoveryApplicationEffects) {
  using namespace std::chrono_literals;
  TemporaryFile file;
  {
    auto journal = OpenFileTransitionJournal({file.path()});
    ASSERT_TRUE(journal);
    auto engine =
        ExecutionEngine::Create(RecoveryModel(), std::make_unique<ReplayBackend>(false, true, true),
                                std::move(journal).value());
    ASSERT_TRUE(engine);
    auto completed =
        engine.value()->RequestMode(DomainId{1}, ModeId{2}, std::chrono::steady_clock::now() + 1s);
    ASSERT_TRUE(completed);
    EXPECT_EQ(completed.value().phase, TransitionPhase::failed);
    EXPECT_EQ(engine.value()->Snapshot().configuration.committed_modes.at(DomainId{1}), ModeId{3});
  }

  auto journal = OpenFileTransitionJournal({file.path()});
  ASSERT_TRUE(journal);
  auto backend = std::make_unique<ReplayBackend>(false, false);
  auto* observer = backend.get();
  auto restored =
      ExecutionEngine::Create(RecoveryModel(), std::move(backend), std::move(journal).value());
  ASSERT_TRUE(restored) << restored.error().message;
  const auto snapshot = restored.value()->Snapshot();
  EXPECT_EQ(snapshot.configuration.committed_modes.at(DomainId{1}), ModeId{3});
  EXPECT_TRUE(snapshot.configuration.running_applications.empty());
  EXPECT_TRUE(observer->operations.empty());
}

} // namespace
