// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/model.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <sstream>
#include <unordered_set>

namespace ovf::exec {
namespace {

bool IsValidName(const std::string& name) {
  if (name.empty() || name.size() > 128U) {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](unsigned char character) {
    return std::isalnum(character) != 0 || character == '_' || character == '-';
  });
}

template <typename Id>
void InspectIdentity(Id id, const std::string& name, const std::string& path,
                     std::unordered_set<Id>& ids, std::unordered_set<std::string>& names,
                     std::vector<ModelIssue>& issues) {
  if (!id) {
    issues.push_back(
        {ModelIssueCode::invalid_identifier, path + ".id", "identifier zero is reserved"});
  } else if (!ids.insert(id).second) {
    issues.push_back(
        {ModelIssueCode::duplicate_identifier, path + ".id", "identifier is not unique"});
  }
  if (!IsValidName(name)) {
    issues.push_back({ModelIssueCode::invalid_name, path + ".name",
                      "name must contain 1-128 ASCII letters, digits, '_' or '-' characters"});
  } else if (!names.insert(name).second) {
    issues.push_back({ModelIssueCode::duplicate_name, path + ".name", "name is not unique"});
  }
}

std::string UnitPath(const ApplicationDefinition& unit) {
  return "units[" + std::to_string(unit.id.value()) + "]";
}

std::string DomainPath(const DomainDefinition& domain) {
  return "domains[" + std::to_string(domain.id.value()) + "]";
}

std::string ModePath(const DomainDefinition& domain, const ModeDefinition& mode) {
  return DomainPath(domain) + ".modes[" + std::to_string(mode.id.value()) + "]";
}

void InspectDependencyCycles(
    const ExecutionModel& model,
    const std::unordered_map<ApplicationId, const ApplicationDefinition*>& applications,
    std::vector<ModelIssue>& issues) {
  enum class Visit : std::uint8_t { unseen, active, complete };
  std::unordered_map<ApplicationId, Visit> visits;
  std::vector<ApplicationId> stack;
  std::unordered_set<ApplicationId> reported;

  std::function<void(const ApplicationDefinition&)> visit =
      [&](const ApplicationDefinition& application) {
        visits[application.id] = Visit::active;
        stack.push_back(application.id);
        for (const auto dependency : application.dependencies) {
          const auto found = applications.find(dependency);
          if (found == applications.end()) {
            continue;
          }
          if (visits[dependency] == Visit::unseen) {
            visit(*found->second);
          } else if (visits[dependency] == Visit::active && reported.insert(dependency).second) {
            std::ostringstream message;
            message << "dependency cycle:";
            const auto begin = std::find(stack.begin(), stack.end(), dependency);
            for (auto item = begin; item != stack.end(); ++item) {
              message << ' ' << item->value() << " ->";
            }
            message << ' ' << dependency.value();
            issues.push_back(
                {ModelIssueCode::dependency_cycle, UnitPath(application), message.str()});
          }
        }
        stack.pop_back();
        visits[application.id] = Visit::complete;
      };

  for (const auto& application : model.units) {
    if (visits[application.id] == Visit::unseen) {
      visit(application);
    }
  }
}

} // namespace

std::string_view ToString(ApplicationState value) noexcept {
  switch (value) {
  case ApplicationState::unknown:
    return "unknown";
  case ApplicationState::starting:
    return "starting";
  case ApplicationState::ready:
    return "ready";
  case ApplicationState::stopping:
    return "stopping";
  case ApplicationState::stopped:
    return "stopped";
  case ApplicationState::failed:
    return "failed";
  case ApplicationState::killed:
    return "killed";
  case ApplicationState::unavailable:
    return "unavailable";
  }
  return "unknown";
}

std::string_view ToString(DomainStatus value) noexcept {
  switch (value) {
  case DomainStatus::unknown:
    return "unknown";
  case DomainStatus::stable:
    return "stable";
  case DomainStatus::transitioning:
    return "transitioning";
  case DomainStatus::degraded:
    return "degraded";
  case DomainStatus::recovering:
    return "recovering";
  }
  return "unknown";
}

std::string_view ToString(TransitionPhase value) noexcept {
  switch (value) {
  case TransitionPhase::received:
    return "received";
  case TransitionPhase::validated:
    return "validated";
  case TransitionPhase::planned:
    return "planned";
  case TransitionPhase::stopping:
    return "stopping";
  case TransitionPhase::starting:
    return "starting";
  case TransitionPhase::awaiting_readiness:
    return "awaiting_readiness";
  case TransitionPhase::committing:
    return "committing";
  case TransitionPhase::succeeded:
    return "succeeded";
  case TransitionPhase::rejected:
    return "rejected";
  case TransitionPhase::failed:
    return "failed";
  case TransitionPhase::cancelled:
    return "cancelled";
  case TransitionPhase::superseded:
    return "superseded";
  case TransitionPhase::deadline_exceeded:
    return "deadline_exceeded";
  case TransitionPhase::recovery_failed:
    return "recovery_failed";
  case TransitionPhase::recovering:
    return "recovering";
  }
  return "unknown";
}

std::string_view ToString(StopReason value) noexcept {
  switch (value) {
  case StopReason::none:
    return "none";
  case StopReason::mode_change:
    return "mode_change";
  case StopReason::system_shutdown:
    return "system_shutdown";
  case StopReason::restart:
    return "restart";
  case StopReason::supervisor_request:
    return "supervisor_request";
  case StopReason::dependency_failure:
    return "dependency_failure";
  case StopReason::recovery:
    return "recovery";
  case StopReason::unknown:
    return "unknown";
  }
  return "unknown";
}

std::string_view ToString(ExecutionUnitKind value) noexcept {
  switch (value) {
  case ExecutionUnitKind::managed_application:
    return "managed_application";
  case ExecutionUnitKind::service:
    return "service";
  case ExecutionUnitKind::one_shot:
    return "one_shot";
  case ExecutionUnitKind::mount:
    return "mount";
  case ExecutionUnitKind::external:
    return "external";
  }
  return "unknown";
}

std::vector<ModelIssue> InspectModel(const ExecutionModel& model) {
  std::vector<ModelIssue> issues;
  if (!model.generation.valid()) {
    issues.push_back(
        {ModelIssueCode::invalid_generation, "generation", "generation zero is reserved"});
  }

  std::unordered_set<ApplicationId> application_ids;
  std::unordered_set<std::string> application_names;
  std::unordered_map<ApplicationId, const ApplicationDefinition*> applications;
  for (const auto& application : model.units) {
    const auto path = UnitPath(application);
    InspectIdentity(application.id, application.name, path, application_ids, application_names,
                    issues);
    applications.emplace(application.id, &application);
    if (application.start_timeout.count() <= 0 || application.stop_timeout.count() <= 0) {
      issues.push_back(
          {ModelIssueCode::invalid_timeout, path, "start and stop timeouts must be positive"});
    }
    if (application.retry.max_attempts == 0U || application.retry.max_attempts > 100U ||
        application.retry.delay.count() < 0) {
      issues.push_back({ModelIssueCode::invalid_retry_policy, path + ".retry",
                        "retry attempts must be 1-100 and delay must not be negative"});
    }
    const bool valid_readiness =
        (application.kind == ExecutionUnitKind::managed_application &&
         (application.readiness == ReadinessPolicy::lifecycle_channel ||
          application.readiness == ReadinessPolicy::process_started)) ||
        (application.kind == ExecutionUnitKind::service &&
         (application.readiness == ReadinessPolicy::process_started ||
          application.readiness == ReadinessPolicy::supervisor_notification)) ||
        (application.kind == ExecutionUnitKind::one_shot &&
         application.readiness == ReadinessPolicy::successful_exit) ||
        (application.kind == ExecutionUnitKind::mount &&
         application.readiness == ReadinessPolicy::mount_present) ||
        (application.kind == ExecutionUnitKind::external &&
         (application.readiness == ReadinessPolicy::process_started ||
          application.readiness == ReadinessPolicy::supervisor_notification));
    if (!valid_readiness) {
      issues.push_back({ModelIssueCode::invalid_readiness_policy, path + ".readiness",
                        "readiness policy is incompatible with the execution unit kind"});
    }
    std::unordered_set<ApplicationId> dependencies;
    for (const auto dependency : application.dependencies) {
      if (!dependencies.insert(dependency).second) {
        issues.push_back({ModelIssueCode::duplicate_identifier, path + ".dependencies",
                          "dependency is listed more than once"});
      }
    }
  }

  for (const auto& application : model.units) {
    for (const auto dependency : application.dependencies) {
      if (applications.find(dependency) == applications.end()) {
        issues.push_back({ModelIssueCode::unknown_dependency,
                          UnitPath(application) + ".dependencies",
                          "dependency does not identify an execution unit"});
      }
    }
  }
  InspectDependencyCycles(model, applications, issues);

  std::unordered_set<DomainId> domain_ids;
  std::unordered_set<std::string> domain_names;
  std::unordered_map<ModeRef, const ModeDefinition*> modes;
  std::unordered_set<ApplicationId> assigned_units;
  for (const auto& domain : model.domains) {
    const auto domain_path = DomainPath(domain);
    InspectIdentity(domain.id, domain.name, domain_path, domain_ids, domain_names, issues);
    std::unordered_set<ModeId> mode_ids;
    std::unordered_set<std::string> mode_names;
    for (const auto& mode : domain.modes) {
      const auto mode_path = ModePath(domain, mode);
      InspectIdentity(mode.id, mode.name, mode_path, mode_ids, mode_names, issues);
      modes.emplace(ModeRef{domain.id, mode.id}, &mode);
      std::unordered_set<ApplicationId> selected;
      for (const auto application : mode.units) {
        if (applications.find(application) == applications.end()) {
          issues.push_back({ModelIssueCode::unknown_application, mode_path + ".units",
                            "mode selects an unknown execution unit"});
        } else if (!selected.insert(application).second) {
          issues.push_back({ModelIssueCode::duplicate_identifier, mode_path + ".units",
                            "execution unit is selected more than once"});
        }
        assigned_units.insert(application);
      }
    }
    if (mode_ids.find(domain.initial_mode) == mode_ids.end()) {
      issues.push_back({ModelIssueCode::unknown_initial_mode, domain_path + ".initial_mode",
                        "initial mode is not defined by this domain"});
    }
    if (domain.recovery.deadline.count() <= 0) {
      issues.push_back({ModelIssueCode::invalid_timeout, domain_path + ".recovery.deadline",
                        "recovery deadline must be positive"});
    }
    if (domain.recovery.action == FailureAction::enter_fallback_mode &&
        mode_ids.find(domain.recovery.fallback_mode) == mode_ids.end()) {
      issues.push_back({ModelIssueCode::invalid_fallback_mode,
                        domain_path + ".recovery.fallback_mode",
                        "fallback mode is not defined by this domain"});
    }
  }

  for (const auto& unit : model.units) {
    const bool assigned = assigned_units.contains(unit.id);
    if (unit.bootstrap && assigned) {
      issues.push_back({ModelIssueCode::invalid_unit_membership, UnitPath(unit) + ".bootstrap",
                        "bootstrap unit must not be assigned to an execution mode"});
    } else if (!unit.bootstrap && !assigned) {
      issues.push_back({ModelIssueCode::invalid_unit_membership, UnitPath(unit),
                        "non-bootstrap unit must have explicit mode membership"});
    }
    if (unit.bootstrap && std::any_of(unit.dependencies.begin(), unit.dependencies.end(),
                                      [&applications](ApplicationId dependency) {
                                        const auto found = applications.find(dependency);
                                        return found != applications.end() &&
                                               !found->second->bootstrap;
                                      })) {
      issues.push_back({ModelIssueCode::invalid_unit_membership, UnitPath(unit) + ".dependencies",
                        "bootstrap unit cannot depend on a mode-controlled unit"});
    }
  }

  for (const auto& domain : model.domains) {
    for (const auto& mode : domain.modes) {
      const auto mode_ref = ModeRef{domain.id, mode.id};
      std::unordered_map<ModeRef, ConstraintKind> constraints;
      for (const auto& constraint : mode.constraints) {
        const auto path = ModePath(domain, mode) + ".constraints";
        if (constraint.other == mode_ref) {
          issues.push_back({ModelIssueCode::self_constraint, path, "mode cannot constrain itself"});
        }
        if (modes.find(constraint.other) == modes.end()) {
          issues.push_back({ModelIssueCode::unknown_constraint_mode, path,
                            "constraint references an unknown mode"});
        }
        const auto [found, inserted] = constraints.emplace(constraint.other, constraint.kind);
        if (!inserted && found->second != constraint.kind) {
          issues.push_back({ModelIssueCode::contradictory_constraint, path,
                            "same mode is both required and excluded"});
        }
      }
    }
  }

  return issues;
}

ValidatedModel::ValidatedModel(ExecutionModel model) : model_(std::move(model)) { BuildIndex(); }

ValidatedModel::ValidatedModel(ValidatedModel&& other) noexcept : model_(std::move(other.model_)) {
  BuildIndex();
}

ValidatedModel& ValidatedModel::operator=(ValidatedModel&& other) noexcept {
  if (this != &other) {
    model_ = std::move(other.model_);
    BuildIndex();
  }
  return *this;
}

void ValidatedModel::BuildIndex() {
  units_.clear();
  domains_.clear();
  modes_.clear();
  for (std::size_t index = 0; index < model_.units.size(); ++index) {
    units_.emplace(model_.units[index].id, index);
  }
  for (std::size_t index = 0; index < model_.domains.size(); ++index) {
    const auto& domain = model_.domains[index];
    domains_.emplace(domain.id, index);
    for (const auto& mode : domain.modes) {
      modes_.emplace(ModeRef{domain.id, mode.id}, &mode);
    }
  }
}

const ExecutionUnitDefinition* ValidatedModel::FindUnit(ExecutionUnitId id) const noexcept {
  const auto found = units_.find(id);
  return found == units_.end() ? nullptr : &model_.units[found->second];
}

const ApplicationDefinition* ValidatedModel::FindApplication(ApplicationId id) const noexcept {
  return FindUnit(id);
}

const DomainDefinition* ValidatedModel::FindDomain(DomainId id) const noexcept {
  const auto found = domains_.find(id);
  return found == domains_.end() ? nullptr : &model_.domains[found->second];
}

const ModeDefinition* ValidatedModel::FindMode(ModeRef mode) const noexcept {
  const auto found = modes_.find(mode);
  return found == modes_.end() ? nullptr : found->second;
}

Result<ValidatedModel> ValidateModel(ExecutionModel model) {
  const auto issues = InspectModel(model);
  if (!issues.empty()) {
    return MakeError(ErrorCode::configuration_error,
                     issues.front().path + ": " + issues.front().message, issues.size());
  }
  return ValidatedModel(std::move(model));
}

} // namespace ovf::exec
