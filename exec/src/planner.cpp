// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/planner.hpp"

#include <algorithm>
#include <functional>
#include <set>

namespace ovf::exec::detail {

TransitionPlanner::TransitionPlanner(const ValidatedModel& model) noexcept : model_(model) {}

Result<SystemConfiguration> TransitionPlanner::InitialConfiguration() const {
  SystemConfiguration configuration;
  configuration.generation = model_.value().generation;
  for (const auto& domain : model_.value().domains) {
    configuration.committed_modes.emplace(domain.id, domain.initial_mode);
  }
  auto required = RequiredApplications(configuration.committed_modes);
  if (!required) {
    return required.error();
  }
  configuration.running_applications = std::move(required).value();
  return configuration;
}

Result<TransitionPlan> TransitionPlanner::Plan(const SystemConfiguration& current, DomainId domain,
                                               ModeId target) const {
  if (current.generation != model_.value().generation) {
    return MakeError(ErrorCode::configuration_error,
                     "system configuration generation does not match the active model");
  }
  const auto* domain_definition = model_.FindDomain(domain);
  if (domain_definition == nullptr) {
    return MakeError(ErrorCode::not_found, "execution domain is not defined");
  }
  if (model_.FindMode({domain, target}) == nullptr) {
    return MakeError(ErrorCode::not_found, "execution mode is not defined by the domain");
  }
  const auto current_mode = current.committed_modes.find(domain);
  if (current_mode == current.committed_modes.end()) {
    return MakeError(ErrorCode::configuration_error,
                     "system configuration has no committed mode for the domain");
  }
  if (current_mode->second == target) {
    return MakeError(ErrorCode::invalid_transition, "domain is already in the target mode");
  }

  auto proposed_modes = current.committed_modes;
  proposed_modes[domain] = target;
  auto constraints = ValidateConstraints(proposed_modes);
  if (!constraints) {
    return constraints.error();
  }
  auto required = RequiredApplications(proposed_modes);
  if (!required) {
    return required.error();
  }

  std::unordered_set<ApplicationId> to_start;
  std::unordered_set<ApplicationId> to_stop;
  std::unordered_set<ApplicationId> retained;
  for (const auto application : required.value()) {
    if (current.running_applications.contains(application)) {
      retained.insert(application);
    } else {
      to_start.insert(application);
    }
  }
  for (const auto application : current.running_applications) {
    if (!required.value().contains(application)) {
      to_stop.insert(application);
    }
  }

  std::set<ResourceId> resources;
  for (const auto application : to_start) {
    const auto* definition = model_.FindApplication(application);
    resources.insert(definition->exclusive_resources.begin(),
                     definition->exclusive_resources.end());
  }
  for (const auto application : to_stop) {
    const auto* definition = model_.FindApplication(application);
    resources.insert(definition->exclusive_resources.begin(),
                     definition->exclusive_resources.end());
  }

  TransitionPlan plan;
  plan.generation = current.generation;
  plan.domain = domain;
  plan.source_mode = current_mode->second;
  plan.target_mode = target;
  plan.retain = Order(retained, false);
  plan.stop = Order(to_stop, true);
  plan.start = Order(to_start, false);
  std::set<DomainId> guarded_domains{domain};
  for (const auto& candidate_domain : model_.value().domains) {
    for (const auto& candidate_mode : candidate_domain.modes) {
      for (const auto& constraint : candidate_mode.constraints) {
        if (candidate_domain.id == domain || constraint.other.domain == domain) {
          guarded_domains.insert(candidate_domain.id);
          guarded_domains.insert(constraint.other.domain);
        }
      }
    }
  }
  plan.guarded_domains.assign(guarded_domains.begin(), guarded_domains.end());
  plan.affected_resources.assign(resources.begin(), resources.end());
  return plan;
}

Result<std::unordered_set<ApplicationId>>
TransitionPlanner::RequiredApplications(const std::unordered_map<DomainId, ModeId>& modes) const {
  std::unordered_set<ApplicationId> required;
  for (const auto& domain : model_.value().domains) {
    const auto selected = modes.find(domain.id);
    if (selected == modes.end()) {
      return MakeError(ErrorCode::configuration_error,
                       "system configuration is missing an execution domain");
    }
    const auto* mode = model_.FindMode({domain.id, selected->second});
    if (mode == nullptr) {
      return MakeError(ErrorCode::configuration_error,
                       "system configuration selects an undefined execution mode");
    }
    required.insert(mode->applications.begin(), mode->applications.end());
  }

  std::vector<ApplicationId> pending(required.begin(), required.end());
  while (!pending.empty()) {
    const auto application = pending.back();
    pending.pop_back();
    const auto* definition = model_.FindApplication(application);
    if (definition == nullptr) {
      return MakeError(ErrorCode::configuration_error,
                       "execution mode selects an undefined application");
    }
    for (const auto dependency : definition->dependencies) {
      if (required.insert(dependency).second) {
        pending.push_back(dependency);
      }
    }
  }
  return required;
}

Result<void>
TransitionPlanner::ValidateConstraints(const std::unordered_map<DomainId, ModeId>& modes) const {
  for (const auto& [domain, mode] : modes) {
    const auto* definition = model_.FindMode({domain, mode});
    if (definition == nullptr) {
      return MakeError(ErrorCode::configuration_error,
                       "system configuration contains an undefined mode");
    }
    for (const auto& constraint : definition->constraints) {
      const auto other = modes.find(constraint.other.domain);
      const bool selected = other != modes.end() && other->second == constraint.other.mode;
      if (constraint.kind == ConstraintKind::requires_mode && !selected) {
        return MakeError(ErrorCode::invalid_transition,
                         "target system configuration does not satisfy a required mode");
      }
      if (constraint.kind == ConstraintKind::excludes_mode && selected) {
        return MakeError(ErrorCode::invalid_transition,
                         "target system configuration selects mutually exclusive modes");
      }
    }
  }
  return {};
}

std::vector<ApplicationId>
TransitionPlanner::Order(const std::unordered_set<ApplicationId>& applications,
                         bool reverse) const {
  std::vector<ApplicationId> ordered;
  std::unordered_set<ApplicationId> visited;
  std::function<void(ApplicationId)> visit = [&](ApplicationId application) {
    if (!applications.contains(application) || !visited.insert(application).second) {
      return;
    }
    const auto* definition = model_.FindApplication(application);
    std::vector<ApplicationId> dependencies = definition->dependencies;
    std::sort(dependencies.begin(), dependencies.end());
    for (const auto dependency : dependencies) {
      visit(dependency);
    }
    ordered.push_back(application);
  };

  std::vector<ApplicationId> stable(applications.begin(), applications.end());
  std::sort(stable.begin(), stable.end());
  for (const auto application : stable) {
    visit(application);
  }
  if (reverse) {
    std::reverse(ordered.begin(), ordered.end());
  }
  return ordered;
}

} // namespace ovf::exec::detail
