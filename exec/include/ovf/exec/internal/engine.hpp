// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/exec/internal/planner.hpp"
#include "ovf/exec/internal/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ovf::exec::detail {

struct BackendEvidence final {
  ApplicationState state{ApplicationState::unknown};
  std::int32_t exit_code{};
  std::int32_t signal{};
  std::uint64_t native_code{};
  std::string message;
};

class ProcessBackend {
public:
  virtual ~ProcessBackend() = default;
  [[nodiscard]] virtual Result<BackendEvidence> Inspect(ApplicationId application) noexcept = 0;
  [[nodiscard]] virtual Result<BackendEvidence> Start(ApplicationId application,
                                                      Deadline deadline) noexcept = 0;
  [[nodiscard]] virtual Result<BackendEvidence> Stop(ApplicationId application, StopReason reason,
                                                     Deadline deadline) noexcept = 0;
};

struct TransitionFailure final {
  Error error;
  std::optional<ApplicationId> application;
  BackendEvidence evidence;
};

struct TransitionSnapshot final {
  TransitionId id;
  DomainId domain;
  ModeId source_mode;
  ModeId target_mode;
  TransitionPhase phase{TransitionPhase::received};
  std::chrono::steady_clock::time_point updated_at;
  std::optional<TransitionFailure> failure;
};

struct JournalEvent final {
  TransitionSnapshot transition;
  ModelGeneration generation;
};

class TransitionJournal {
public:
  virtual ~TransitionJournal() = default;
  [[nodiscard]] virtual Result<void> Append(const JournalEvent& event) noexcept = 0;
};

class MemoryTransitionJournal final : public TransitionJournal {
public:
  [[nodiscard]] Result<void> Append(const JournalEvent& event) noexcept override;
  [[nodiscard]] std::vector<JournalEvent> Events() const;

private:
  mutable std::mutex mutex_;
  std::vector<JournalEvent> events_;
};

struct EngineSnapshot final {
  SystemConfiguration configuration;
  std::vector<TransitionSnapshot> transitions;
};

class ExecutionEngine final {
public:
  static Result<std::unique_ptr<ExecutionEngine>>
  Create(ValidatedModel model, std::unique_ptr<ProcessBackend> backend,
         std::unique_ptr<TransitionJournal> journal);

  ~ExecutionEngine() = default;
  ExecutionEngine(const ExecutionEngine&) = delete;
  ExecutionEngine& operator=(const ExecutionEngine&) = delete;

  [[nodiscard]] Result<TransitionSnapshot> RequestMode(DomainId domain, ModeId target,
                                                       Deadline deadline) noexcept;
  [[nodiscard]] Result<void> Cancel(TransitionId transition) noexcept;
  [[nodiscard]] EngineSnapshot Snapshot() const;

private:
  ExecutionEngine(ValidatedModel model, std::unique_ptr<ProcessBackend> backend,
                  std::unique_ptr<TransitionJournal> journal, SystemConfiguration configuration);

  [[nodiscard]] Result<void> Record(TransitionSnapshot& transition, TransitionPhase phase) noexcept;
  [[nodiscard]] Result<TransitionSnapshot> Fail(TransitionSnapshot& transition, Error error,
                                                std::optional<ApplicationId> application = {},
                                                BackendEvidence evidence = {}) noexcept;
  [[nodiscard]] bool IsCancelled(TransitionId transition) const noexcept;

  ValidatedModel model_;
  TransitionPlanner planner_;
  TransitionScheduler scheduler_;
  std::unique_ptr<ProcessBackend> backend_;
  std::unique_ptr<TransitionJournal> journal_;
  mutable std::mutex mutex_;
  SystemConfiguration configuration_;
  std::unordered_map<TransitionId, TransitionSnapshot> transitions_;
  std::unordered_map<TransitionId, bool> cancellation_;
  std::atomic<std::uint64_t> next_transition_{1U};
};

} // namespace ovf::exec::detail
