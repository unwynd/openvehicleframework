// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/planner.hpp"

#include <gtest/gtest.h>

namespace {

using namespace ovf::exec;
using namespace ovf::exec::detail;

ValidatedModel Model() {
  ExecutionModel model{ModelGeneration{1},
                       {
                           {ApplicationId{1},
                            "shared",
                            ReadinessPolicy::required,
                            std::chrono::seconds(5),
                            std::chrono::seconds(5),
                            {},
                            {},
                            {}},
                           {ApplicationId{2},
                            "camera",
                            ReadinessPolicy::required,
                            std::chrono::seconds(5),
                            std::chrono::seconds(5),
                            {},
                            {ApplicationId{1}},
                            {}},
                           {ApplicationId{3},
                            "diagnostics",
                            ReadinessPolicy::required,
                            std::chrono::seconds(5),
                            std::chrono::seconds(5),
                            {},
                            {ApplicationId{1}},
                            {ResourceId{7}}},
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
                            "diagnostics",
                            ModeId{1},
                            ReplacementPolicy::queue,
                            {},
                            {{ModeId{1}, "off", {}, {}},
                             {ModeId{2},
                              "remote",
                              {ApplicationId{3}},
                              {{ConstraintKind::requires_mode, {DomainId{1}, ModeId{2}}}}}}},
                       }};
  auto result = ValidateModel(std::move(model));
  EXPECT_TRUE(result);
  return std::move(result).value();
}

TEST(TransitionPlannerTest, BuildsInitialUnionOfActiveDomains) {
  auto model = Model();
  TransitionPlanner planner(model);
  auto initial = planner.InitialConfiguration();
  ASSERT_TRUE(initial);
  EXPECT_EQ(initial.value().running_applications,
            std::unordered_set<ApplicationId>({ApplicationId{1}}));
}

TEST(TransitionPlannerTest, OrdersDependenciesAndRetainsSharedApplications) {
  auto model = Model();
  TransitionPlanner planner(model);
  auto initial = planner.InitialConfiguration();
  ASSERT_TRUE(initial);

  auto plan = planner.Plan(initial.value(), DomainId{1}, ModeId{2});
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan.value().retain, std::vector<ApplicationId>({ApplicationId{1}}));
  EXPECT_TRUE(plan.value().stop.empty());
  EXPECT_EQ(plan.value().start, std::vector<ApplicationId>({ApplicationId{2}}));
}

TEST(TransitionPlannerTest, ValidatesCrossDomainConfiguration) {
  auto model = Model();
  TransitionPlanner planner(model);
  auto initial = planner.InitialConfiguration();
  ASSERT_TRUE(initial);

  auto rejected = planner.Plan(initial.value(), DomainId{2}, ModeId{2});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code, ErrorCode::invalid_transition);

  auto operational = planner.Plan(initial.value(), DomainId{1}, ModeId{2});
  ASSERT_TRUE(operational);
  auto configuration = initial.value();
  configuration.committed_modes[DomainId{1}] = ModeId{2};
  configuration.running_applications.insert(ApplicationId{2});

  auto accepted = planner.Plan(configuration, DomainId{2}, ModeId{2});
  ASSERT_TRUE(accepted);
  EXPECT_EQ(accepted.value().retain,
            std::vector<ApplicationId>({ApplicationId{1}, ApplicationId{2}}));
  EXPECT_EQ(accepted.value().start, std::vector<ApplicationId>({ApplicationId{3}}));
  EXPECT_EQ(accepted.value().affected_resources, std::vector<ResourceId>({ResourceId{7}}));
}

TEST(TransitionPlannerTest, StopsOnlyAfterLastDomainReleasesSharedApplication) {
  auto model = Model();
  TransitionPlanner planner(model);
  auto configuration = planner.InitialConfiguration().value();
  configuration.committed_modes[DomainId{1}] = ModeId{2};
  configuration.committed_modes[DomainId{2}] = ModeId{2};
  configuration.running_applications = {ApplicationId{1}, ApplicationId{2}, ApplicationId{3}};

  auto diagnostics_off = planner.Plan(configuration, DomainId{2}, ModeId{1});
  ASSERT_TRUE(diagnostics_off);
  EXPECT_EQ(diagnostics_off.value().stop, std::vector<ApplicationId>({ApplicationId{3}}));
  EXPECT_TRUE(std::find(diagnostics_off.value().retain.begin(),
                        diagnostics_off.value().retain.end(),
                        ApplicationId{1}) != diagnostics_off.value().retain.end());
}

} // namespace
