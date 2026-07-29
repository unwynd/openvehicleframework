// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/model.hpp"

#include <gtest/gtest.h>

namespace {

using namespace ovf::exec;

ExecutionModel ValidModel() {
  return {ModelGeneration{7},
          {
              {ApplicationId{1},
               "camera",
               ReadinessPolicy::required,
               std::chrono::seconds(2),
               std::chrono::seconds(2),
               {2, std::chrono::milliseconds(10)},
               {},
               {ResourceId{1}}},
              {ApplicationId{2},
               "fusion",
               ReadinessPolicy::required,
               std::chrono::seconds(2),
               std::chrono::seconds(2),
               {},
               {ApplicationId{1}},
               {}},
          },
          {
              {DomainId{1},
               "machine",
               ModeId{1},
               ReplacementPolicy::supersede_if_safe,
               {},
               {{ModeId{1}, "startup", {ApplicationId{1}}, {}},
                {ModeId{2}, "operational", {ApplicationId{1}, ApplicationId{2}}, {}}}},
              {DomainId{2},
               "driving",
               ModeId{1},
               ReplacementPolicy::queue,
               {FailureAction::enter_fallback_mode, ModeId{1}, std::chrono::seconds(3)},
               {{ModeId{1}, "inactive", {}, {}},
                {ModeId{2},
                 "active",
                 {ApplicationId{2}},
                 {{ConstraintKind::requires_mode, {DomainId{1}, ModeId{2}}}}}}},
          }};
}

TEST(ExecutionModelTest, AcceptsOrthogonalDomainsAndSharedApplications) {
  auto result = ValidateModel(ValidModel());
  ASSERT_TRUE(result) << result.error().message;

  const auto& model = result.value();
  ASSERT_NE(model.FindApplication(ApplicationId{2}), nullptr);
  EXPECT_EQ(model.FindApplication(ApplicationId{2})->name, "fusion");
  ASSERT_NE(model.FindDomain(DomainId{2}), nullptr);
  EXPECT_EQ(model.FindDomain(DomainId{2})->name, "driving");
  ASSERT_NE(model.FindMode({DomainId{2}, ModeId{2}}), nullptr);
  EXPECT_EQ(model.FindMode({DomainId{2}, ModeId{2}})->name, "active");
}

TEST(ExecutionModelTest, ReportsEveryStaticErrorInOneInspection) {
  auto model = ValidModel();
  model.generation = {};
  model.applications[0].id = {};
  model.applications[0].name = "not a valid name";
  model.applications[0].start_timeout = std::chrono::milliseconds::zero();
  model.applications[1].dependencies = {ApplicationId{99}};
  model.domains[0].initial_mode = ModeId{99};

  const auto issues = InspectModel(model);
  EXPECT_GE(issues.size(), 6U);

  const auto result = ValidateModel(std::move(model));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::configuration_error);
  EXPECT_EQ(result.error().support_data, issues.size());
}

TEST(ExecutionModelTest, DetectsDependencyCycles) {
  auto model = ValidModel();
  model.applications[0].dependencies = {ApplicationId{2}};

  const auto issues = InspectModel(model);
  EXPECT_TRUE(std::any_of(issues.begin(), issues.end(), [](const auto& issue) {
    return issue.code == ModelIssueCode::dependency_cycle;
  }));
}

TEST(ExecutionModelTest, RejectsContradictoryModeConstraints) {
  auto model = ValidModel();
  auto& constraints = model.domains[1].modes[1].constraints;
  constraints.push_back({ConstraintKind::excludes_mode, {DomainId{1}, ModeId{2}}});

  const auto issues = InspectModel(model);
  EXPECT_TRUE(std::any_of(issues.begin(), issues.end(), [](const auto& issue) {
    return issue.code == ModelIssueCode::contradictory_constraint;
  }));
}

} // namespace
