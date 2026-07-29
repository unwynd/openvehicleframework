// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/lifecycle_backend.hpp"

#include <gtest/gtest.h>

#include <condition_variable>
#include <mutex>
#include <unordered_map>

namespace {

using namespace ovf::exec;

class FakeLifecycleBackend final : public detail::LifecycleBackend {
public:
  ApplicationId Id() const noexcept override { return ApplicationId{42}; }
  std::string Name() const override { return "test-application"; }

  Result<void> ReportReady() noexcept override {
    if (ready_) {
      return MakeError(ErrorCode::invalid_transition, "already ready");
    }
    ready_ = true;
    return {};
  }

  std::uint64_t Subscribe(StopHandler handler) override {
    std::lock_guard lock(mutex_);
    const auto id = next_id_++;
    handlers_.emplace(id, std::move(handler));
    return id;
  }

  void Unsubscribe(std::uint64_t subscription) noexcept override {
    std::lock_guard lock(mutex_);
    handlers_.erase(subscription);
  }

  bool StopRequested() const noexcept override { return stopped_; }
  StopReason GetStopReason() const noexcept override { return reason_; }

  Result<StopReason> WaitForStop(Deadline deadline) noexcept override {
    std::unique_lock lock(mutex_);
    if (!condition_.wait_until(lock, deadline, [this] { return stopped_; })) {
      return MakeError(ErrorCode::deadline_exceeded, "deadline");
    }
    return reason_;
  }

  void RequestStop(StopReason reason) {
    std::unordered_map<std::uint64_t, StopHandler> handlers;
    {
      std::lock_guard lock(mutex_);
      reason_ = reason;
      stopped_ = true;
      handlers = handlers_;
    }
    condition_.notify_all();
    for (auto& [id, handler] : handlers) {
      static_cast<void>(id);
      handler(reason);
    }
  }

  bool ready_{};

private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool stopped_{};
  StopReason reason_{StopReason::none};
  std::uint64_t next_id_{1};
  std::unordered_map<std::uint64_t, StopHandler> handlers_;
};

TEST(ApplicationTest, EnforcesSingleEndpointAndLifecycleTransitions) {
  auto backend = std::make_unique<FakeLifecycleBackend>();
  auto* observer = backend.get();
  auto result = detail_ApplicationFactory::Create(std::move(backend));
  ASSERT_TRUE(result);
  auto application = std::move(result).value();

  EXPECT_EQ(application.Id(), ApplicationId{42});
  EXPECT_EQ(application.Name(), "test-application");
  EXPECT_TRUE(application.ReportReady());
  EXPECT_FALSE(application.ReportReady());
  EXPECT_TRUE(observer->ready_);

  auto duplicate = detail_ApplicationFactory::Create(std::make_unique<FakeLifecycleBackend>());
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error().code, ErrorCode::already_exists);
}

TEST(ApplicationTest, DeliversStopAndSupportsSubscriptionLifetime) {
  auto backend = std::make_unique<FakeLifecycleBackend>();
  auto* controller = backend.get();
  auto result = detail_ApplicationFactory::Create(std::move(backend));
  ASSERT_TRUE(result);
  auto application = std::move(result).value();

  StopReason observed{StopReason::none};
  auto subscription = application.OnStop([&](StopReason reason) { observed = reason; });
  ASSERT_TRUE(subscription);

  controller->RequestStop(StopReason::mode_change);
  EXPECT_TRUE(application.StopRequested());
  EXPECT_EQ(application.GetStopReason(), StopReason::mode_change);
  EXPECT_EQ(observed, StopReason::mode_change);
  auto waited = application.WaitForStop(std::chrono::steady_clock::now());
  ASSERT_TRUE(waited);
  EXPECT_EQ(waited.value(), StopReason::mode_change);

  subscription.value().Reset();
}

} // namespace
