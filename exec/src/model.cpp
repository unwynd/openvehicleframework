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

std::string ApplicationPath(const ApplicationDefinition& application) {
  return "applications[" + std::to_string(application.id.value()) + "]";
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
                {ModelIssueCode::dependency_cycle, ApplicationPath(application), message.str()});
          }
        }
        stack.pop_back();
        visits[application.id] = Visit::complete;
      };

  for (const auto& application : model.applications) {
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

std::vector<ModelIssue> InspectModel(const ExecutionModel& model) {
  std::vector<ModelIssue> issues;
  if (!model.generation.valid()) {
    issues.push_back(
        {ModelIssueCode::invalid_generation, "generation", "generation zero is reserved"});
  }

  std::unordered_set<ApplicationId> application_ids;
  std::unordered_set<std::string> application_names;
  std::unordered_map<ApplicationId, const ApplicationDefinition*> applications;
  for (const auto& application : model.applications) {
    const auto path = ApplicationPath(application);
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
    std::unordered_set<ApplicationId> dependencies;
    for (const auto dependency : application.dependencies) {
      if (!dependencies.insert(dependency).second) {
        issues.push_back({ModelIssueCode::duplicate_identifier, path + ".dependencies",
                          "dependency is listed more than once"});
      }
    }
  }

  for (const auto& application : model.applications) {
    for (const auto dependency : application.dependencies) {
      if (applications.find(dependency) == applications.end()) {
        issues.push_back({ModelIssueCode::unknown_dependency,
                          ApplicationPath(application) + ".dependencies",
                          "dependency does not identify a managed application"});
      }
    }
  }
  InspectDependencyCycles(model, applications, issues);

  std::unordered_set<DomainId> domain_ids;
  std::unordered_set<std::string> domain_names;
  std::unordered_map<ModeRef, const ModeDefinition*> modes;
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
      for (const auto application : mode.applications) {
        if (applications.find(application) == applications.end()) {
          issues.push_back({ModelIssueCode::unknown_application, mode_path + ".applications",
                            "mode selects an unknown application"});
        } else if (!selected.insert(application).second) {
          issues.push_back({ModelIssueCode::duplicate_identifier, mode_path + ".applications",
                            "application is selected more than once"});
        }
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
  applications_.clear();
  domains_.clear();
  modes_.clear();
  for (std::size_t index = 0; index < model_.applications.size(); ++index) {
    applications_.emplace(model_.applications[index].id, index);
  }
  for (std::size_t index = 0; index < model_.domains.size(); ++index) {
    const auto& domain = model_.domains[index];
    domains_.emplace(domain.id, index);
    for (const auto& mode : domain.modes) {
      modes_.emplace(ModeRef{domain.id, mode.id}, &mode);
    }
  }
}

const ApplicationDefinition* ValidatedModel::FindApplication(ApplicationId id) const noexcept {
  const auto found = applications_.find(id);
  return found == applications_.end() ? nullptr : &model_.applications[found->second];
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
