// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/scheduler.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace ovf::exec;
using namespace ovf::exec::detail;

TEST(TransitionSchedulerTest, AllowsDisjointTransitionsConcurrently) {
  TransitionScheduler scheduler;
  auto first =
      scheduler.Acquire(TransitionId{1}, {{DomainId{1}}, {ApplicationId{1}}, {ResourceId{1}}},
                        std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(first);
  auto second =
      scheduler.Acquire(TransitionId{2}, {{DomainId{2}}, {ApplicationId{2}}, {ResourceId{2}}},
                        std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(second);
  EXPECT_EQ(scheduler.ActiveLeaseCount(), 2U);
}

TEST(TransitionSchedulerTest, SerializesEveryKindOfOverlap) {
  TransitionScheduler scheduler;
  auto first =
      scheduler.Acquire(TransitionId{1}, {{DomainId{1}}, {ApplicationId{1}}, {ResourceId{1}}},
                        std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(first);

  for (const auto& resources :
       {TransitionResources{{DomainId{1}}, {ApplicationId{2}}, {ResourceId{2}}},
        TransitionResources{{DomainId{2}}, {ApplicationId{1}}, {ResourceId{2}}},
        TransitionResources{{DomainId{2}}, {ApplicationId{2}}, {ResourceId{1}}}}) {
    auto blocked =
        scheduler.Acquire(TransitionId{2}, resources, std::chrono::steady_clock::now() + 10ms);
    ASSERT_FALSE(blocked);
    EXPECT_EQ(blocked.error().code, ErrorCode::deadline_exceeded);
  }
}

TEST(TransitionSchedulerTest, WakesWaiterWhenLeaseIsReleased) {
  TransitionScheduler scheduler;
  auto first = scheduler.Acquire(TransitionId{1}, {{DomainId{1}}, {}, {}},
                                 std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(first);

  std::promise<Result<TransitionScheduler::Lease>> completed;
  auto result = completed.get_future();
  std::thread waiter([&] {
    completed.set_value(scheduler.Acquire(TransitionId{2}, {{DomainId{1}}, {}, {}},
                                          std::chrono::steady_clock::now() + 1s));
  });

  EXPECT_EQ(result.wait_for(20ms), std::future_status::timeout);
  first.value().Reset();
  ASSERT_EQ(result.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(result.get());
  waiter.join();
}

TEST(TransitionSchedulerTest, RejectsMalformedResourceRequests) {
  TransitionScheduler scheduler;
  auto no_domain = scheduler.Acquire(TransitionId{1}, {}, std::chrono::steady_clock::now() + 1s);
  ASSERT_FALSE(no_domain);
  EXPECT_EQ(no_domain.error().code, ErrorCode::invalid_argument);

  auto duplicate = scheduler.Acquire(TransitionId{1}, {{DomainId{1}, DomainId{1}}, {}, {}},
                                     std::chrono::steady_clock::now() + 1s);
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error().code, ErrorCode::invalid_argument);
}

} // namespace
