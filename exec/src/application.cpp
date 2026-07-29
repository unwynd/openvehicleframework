// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/application.hpp"

#include "ovf/exec/internal/lifecycle_backend.hpp"

#include <atomic>
#include <mutex>
#include <utility>

namespace ovf::exec {
namespace {

std::atomic_bool application_exists{false};

} // namespace

class Application::Impl final {
public:
  explicit Impl(std::unique_ptr<detail::LifecycleBackend> backend) : backend_(std::move(backend)) {}

  ~Impl() { application_exists.store(false, std::memory_order_release); }

  detail::LifecycleBackend& Backend() noexcept { return *backend_; }
  const detail::LifecycleBackend& Backend() const noexcept { return *backend_; }

private:
  std::unique_ptr<detail::LifecycleBackend> backend_;
};

StopSubscription::StopSubscription(std::function<void()> unsubscribe)
    : unsubscribe_(std::move(unsubscribe)) {}

StopSubscription::~StopSubscription() { Reset(); }

StopSubscription::StopSubscription(StopSubscription&& other) noexcept
    : unsubscribe_(std::exchange(other.unsubscribe_, std::function<void()>{})) {}

StopSubscription& StopSubscription::operator=(StopSubscription&& other) noexcept {
  if (this != &other) {
    Reset();
    unsubscribe_ = std::exchange(other.unsubscribe_, std::function<void()>{});
  }
  return *this;
}

void StopSubscription::Reset() noexcept {
  if (unsubscribe_) {
    auto unsubscribe = std::move(unsubscribe_);
    unsubscribe();
  }
}

StopSubscription::operator bool() const noexcept { return static_cast<bool>(unsubscribe_); }

Application::Application(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

Application::~Application() = default;
Application::Application(Application&&) noexcept = default;
Application& Application::operator=(Application&&) noexcept = default;

Result<Application> Application::Create(ApplicationOptions options) {
  auto backend = detail::CreateDinitLifecycleBackend(options);
  if (!backend) {
    return backend.error();
  }
  return detail_ApplicationFactory::Create(std::move(backend).value());
}

ApplicationId Application::Id() const noexcept {
  return impl_ ? impl_->Backend().Id() : ApplicationId{};
}

std::string Application::Name() const { return impl_ ? impl_->Backend().Name() : std::string{}; }

Result<void> Application::ReportReady() noexcept {
  if (!impl_) {
    return MakeError(ErrorCode::invalid_transition, "application has been moved from");
  }
  return impl_->Backend().ReportReady();
}

bool Application::StopRequested() const noexcept {
  return impl_ && impl_->Backend().StopRequested();
}

StopReason Application::GetStopReason() const noexcept {
  return impl_ ? impl_->Backend().GetStopReason() : StopReason::unknown;
}

Result<StopReason> Application::WaitForStop(Deadline deadline) noexcept {
  if (!impl_) {
    return MakeError(ErrorCode::invalid_transition, "application has been moved from");
  }
  return impl_->Backend().WaitForStop(deadline);
}

Result<StopSubscription> Application::OnStop(std::function<void(StopReason)> handler) {
  if (!impl_) {
    return MakeError(ErrorCode::invalid_transition, "application has been moved from");
  }
  if (!handler) {
    return MakeError(ErrorCode::invalid_argument, "stop handler is empty");
  }
  const auto subscription = impl_->Backend().Subscribe(std::move(handler));
  if (subscription == 0U) {
    return MakeError(ErrorCode::resource_exhausted, "stop subscription could not be registered");
  }
  std::weak_ptr<Impl> weak = impl_;
  return StopSubscription([weak, subscription] {
    if (const auto impl = weak.lock()) {
      impl->Backend().Unsubscribe(subscription);
    }
  });
}

Result<void> Application::ReportFailure(FailureReport report) noexcept {
  if (!impl_) {
    return MakeError(ErrorCode::invalid_transition, "application has been moved from");
  }
  if (report.code == 0U || report.message.empty()) {
    return MakeError(ErrorCode::invalid_argument,
                     "failure code must be nonzero and message must not be empty");
  }
  return impl_->Backend().ReportFailure(report);
}

Result<Application>
detail_ApplicationFactory::Create(std::unique_ptr<detail::LifecycleBackend> backend) {
  if (!backend) {
    return MakeError(ErrorCode::invalid_argument, "lifecycle backend is null");
  }
  bool expected = false;
  if (!application_exists.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return MakeError(ErrorCode::already_exists,
                     "one application lifecycle endpoint already exists in this process");
  }
  try {
    return Application(std::make_shared<Application::Impl>(std::move(backend)));
  } catch (...) {
    application_exists.store(false, std::memory_order_release);
    return MakeError(ErrorCode::resource_exhausted,
                     "application lifecycle endpoint allocation failed");
  }
}

} // namespace ovf::exec
