// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/exec/error.hpp"
#include "ovf/exec/types.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ovf::exec {

struct RetryPolicy final {
  std::uint32_t max_attempts{1U};
  std::chrono::milliseconds delay{};
};

enum class ExecutionUnitKind : std::uint8_t {
  managed_application,
  service,
  one_shot,
  mount,
  external
};

[[nodiscard]] std::string_view ToString(ExecutionUnitKind value) noexcept;

struct ExecutionUnitDefinition final {
  ApplicationId id;
  std::string name;
  ReadinessPolicy readiness{ReadinessPolicy::required};
  std::chrono::milliseconds start_timeout{5000};
  std::chrono::milliseconds stop_timeout{5000};
  RetryPolicy retry;
  std::vector<ApplicationId> dependencies;
  std::vector<ResourceId> exclusive_resources;
  ExecutionUnitKind kind{ExecutionUnitKind::managed_application};
  bool bootstrap{};
};

using ApplicationDefinition = ExecutionUnitDefinition;

enum class ConstraintKind : std::uint8_t { requires_mode, excludes_mode };

struct ModeConstraint final {
  ConstraintKind kind{ConstraintKind::requires_mode};
  ModeRef other;
};

struct ModeDefinition final {
  ModeId id;
  std::string name;
  std::vector<ExecutionUnitId> units;
  std::vector<ModeConstraint> constraints;
};

struct RecoveryPolicy final {
  FailureAction action{FailureAction::hold_observed_configuration};
  ModeId fallback_mode;
  std::chrono::milliseconds deadline{5000};
};

struct DomainDefinition final {
  DomainId id;
  std::string name;
  ModeId initial_mode;
  ReplacementPolicy replacement{ReplacementPolicy::supersede_if_safe};
  RecoveryPolicy recovery;
  std::vector<ModeDefinition> modes;
};

struct ExecutionModel final {
  ModelGeneration generation;
  std::vector<ExecutionUnitDefinition> units;
  std::vector<DomainDefinition> domains;
};

enum class ModelIssueCode : std::uint16_t {
  invalid_generation,
  invalid_identifier,
  invalid_name,
  duplicate_identifier,
  duplicate_name,
  invalid_timeout,
  invalid_retry_policy,
  invalid_readiness_policy,
  invalid_unit_membership,
  unknown_application,
  unknown_dependency,
  dependency_cycle,
  unknown_initial_mode,
  unknown_constraint_mode,
  self_constraint,
  invalid_fallback_mode,
  contradictory_constraint
};

struct ModelIssue final {
  ModelIssueCode code;
  std::string path;
  std::string message;
};

class ValidatedModel final {
public:
  ValidatedModel(ValidatedModel&& other) noexcept;
  ValidatedModel& operator=(ValidatedModel&& other) noexcept;
  ValidatedModel(const ValidatedModel&) = delete;
  ValidatedModel& operator=(const ValidatedModel&) = delete;

  [[nodiscard]] const ExecutionModel& value() const noexcept { return model_; }
  [[nodiscard]] const ExecutionUnitDefinition* FindUnit(ExecutionUnitId id) const noexcept;
  [[nodiscard]] const ApplicationDefinition* FindApplication(ApplicationId id) const noexcept;
  [[nodiscard]] const DomainDefinition* FindDomain(DomainId id) const noexcept;
  [[nodiscard]] const ModeDefinition* FindMode(ModeRef mode) const noexcept;

private:
  friend Result<ValidatedModel> ValidateModel(ExecutionModel);
  explicit ValidatedModel(ExecutionModel model);
  void BuildIndex();

  ExecutionModel model_;
  std::unordered_map<ExecutionUnitId, std::size_t> units_;
  std::unordered_map<DomainId, std::size_t> domains_;
  std::unordered_map<ModeRef, const ModeDefinition*> modes_;
};

[[nodiscard]] std::vector<ModelIssue> InspectModel(const ExecutionModel& model);
[[nodiscard]] Result<ValidatedModel> ValidateModel(ExecutionModel model);

} // namespace ovf::exec
