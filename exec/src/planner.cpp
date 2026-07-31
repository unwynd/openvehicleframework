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
  auto required = RequiredUnits(configuration.committed_modes);
  if (!required) {
    return required.error();
  }
  configuration.running_units = std::move(required).value();
  return configuration;
}

Result<TransitionPlan> TransitionPlanner::Plan(const SystemConfiguration& current, DomainId domain,
                                               ModeId target) const {
  return BuildPlan(current, domain, target, false);
}

Result<TransitionPlan> TransitionPlanner::Reconcile(const SystemConfiguration& current,
                                                    DomainId domain, ModeId target) const {
  return BuildPlan(current, domain, target, true);
}

Result<TransitionPlan> TransitionPlanner::BuildPlan(const SystemConfiguration& current,
                                                    DomainId domain, ModeId target,
                                                    bool allow_current_mode) const {
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
  if (!allow_current_mode && current_mode->second == target) {
    return MakeError(ErrorCode::invalid_transition, "domain is already in the target mode");
  }

  auto proposed_modes = current.committed_modes;
  proposed_modes[domain] = target;
  auto constraints = ValidateConstraints(proposed_modes);
  if (!constraints) {
    return constraints.error();
  }
  auto required = RequiredUnits(proposed_modes);
  if (!required) {
    return required.error();
  }

  std::unordered_set<ExecutionUnitId> to_start;
  std::unordered_set<ExecutionUnitId> to_stop;
  std::unordered_set<ExecutionUnitId> retained;
  for (const auto application : required.value()) {
    if (current.running_units.contains(application)) {
      retained.insert(application);
    } else {
      to_start.insert(application);
    }
  }
  for (const auto application : current.running_units) {
    if (!required.value().contains(application)) {
      to_stop.insert(application);
    }
  }

  std::set<ResourceId> resources;
  for (const auto application : to_start) {
    const auto* definition = model_.FindUnit(application);
    resources.insert(definition->exclusive_resources.begin(),
                     definition->exclusive_resources.end());
  }
  for (const auto application : to_stop) {
    const auto* definition = model_.FindUnit(application);
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

Result<std::vector<ExecutionUnitId>>
TransitionPlanner::StopDomainUnits(const SystemConfiguration& current, DomainId domain) const {
  if (current.generation != model_.value().generation || model_.FindDomain(domain) == nullptr) {
    return MakeError(ErrorCode::configuration_error,
                     "cannot stop an unknown domain or model generation");
  }
  auto remaining_modes = current.committed_modes;
  remaining_modes.erase(domain);
  std::unordered_set<ExecutionUnitId> required;
  for (const auto& unit : model_.value().units) {
    if (unit.bootstrap) {
      required.insert(unit.id);
    }
  }
  for (const auto& [selected_domain, selected_mode] : remaining_modes) {
    const auto* mode = model_.FindMode({selected_domain, selected_mode});
    if (mode == nullptr) {
      return MakeError(ErrorCode::configuration_error,
                       "system configuration selects an undefined execution mode");
    }
    required.insert(mode->units.begin(), mode->units.end());
  }
  for (const auto unit : required) {
    const auto* definition = model_.FindUnit(unit);
    if (definition == nullptr ||
        std::any_of(
            definition->dependencies.begin(), definition->dependencies.end(),
            [&required](ExecutionUnitId dependency) { return !required.contains(dependency); })) {
      return MakeError(ErrorCode::configuration_error,
                       "remaining domain configuration omits a required execution unit");
    }
  }
  std::unordered_set<ExecutionUnitId> stopped;
  for (const auto application : current.running_units) {
    if (!required.contains(application)) {
      stopped.insert(application);
    }
  }
  return Order(stopped, true);
}

Result<std::unordered_set<ExecutionUnitId>>
TransitionPlanner::RequiredUnits(const std::unordered_map<DomainId, ModeId>& modes) const {
  std::unordered_set<ExecutionUnitId> required;
  for (const auto& unit : model_.value().units) {
    if (unit.bootstrap) {
      required.insert(unit.id);
    }
  }
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
    required.insert(mode->units.begin(), mode->units.end());
  }

  for (const auto unit : required) {
    const auto* definition = model_.FindUnit(unit);
    if (definition == nullptr) {
      return MakeError(ErrorCode::configuration_error,
                       "execution mode selects an undefined execution unit");
    }
    for (const auto dependency : definition->dependencies) {
      if (!required.contains(dependency)) {
        return MakeError(ErrorCode::invalid_transition,
                         "target modes omit an explicit execution-unit dependency");
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

std::vector<ExecutionUnitId>
TransitionPlanner::Order(const std::unordered_set<ExecutionUnitId>& units, bool reverse) const {
  std::vector<ExecutionUnitId> ordered;
  std::unordered_set<ExecutionUnitId> visited;
  std::function<void(ExecutionUnitId)> visit = [&](ExecutionUnitId application) {
    if (!units.contains(application) || !visited.insert(application).second) {
      return;
    }
    const auto* definition = model_.FindUnit(application);
    std::vector<ExecutionUnitId> dependencies = definition->dependencies;
    std::sort(dependencies.begin(), dependencies.end());
    for (const auto dependency : dependencies) {
      visit(dependency);
    }
    ordered.push_back(application);
  };

  std::vector<ExecutionUnitId> stable(units.begin(), units.end());
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
