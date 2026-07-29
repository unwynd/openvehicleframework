// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/exec/model.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ovf::exec::detail {

struct SystemConfiguration final {
  ModelGeneration generation;
  std::unordered_map<DomainId, ModeId> committed_modes;
  std::unordered_set<ApplicationId> running_applications;
};

struct TransitionPlan final {
  ModelGeneration generation;
  DomainId domain;
  ModeId source_mode;
  ModeId target_mode;
  std::vector<ApplicationId> retain;
  std::vector<ApplicationId> stop;
  std::vector<ApplicationId> start;
  std::vector<DomainId> guarded_domains;
  std::vector<ResourceId> affected_resources;
};

class TransitionPlanner final {
public:
  explicit TransitionPlanner(const ValidatedModel& model) noexcept;

  [[nodiscard]] Result<SystemConfiguration> InitialConfiguration() const;
  [[nodiscard]] Result<TransitionPlan> Plan(const SystemConfiguration& current, DomainId domain,
                                            ModeId target) const;
  [[nodiscard]] Result<TransitionPlan> Reconcile(const SystemConfiguration& current,
                                                 DomainId domain, ModeId target) const;
  [[nodiscard]] Result<std::vector<ApplicationId>>
  StopDomainApplications(const SystemConfiguration& current, DomainId domain) const;

private:
  [[nodiscard]] Result<TransitionPlan> BuildPlan(const SystemConfiguration& current,
                                                 DomainId domain, ModeId target,
                                                 bool allow_current_mode) const;
  [[nodiscard]] Result<std::unordered_set<ApplicationId>>
  RequiredApplications(const std::unordered_map<DomainId, ModeId>& modes) const;
  [[nodiscard]] Result<void>
  ValidateConstraints(const std::unordered_map<DomainId, ModeId>& modes) const;
  [[nodiscard]] std::vector<ApplicationId>
  Order(const std::unordered_set<ApplicationId>& applications, bool reverse) const;

  const ValidatedModel& model_;
};

} // namespace ovf::exec::detail
