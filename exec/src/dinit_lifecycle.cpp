// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/lifecycle_backend.hpp"

#include <atomic>
#include <cerrno>
#include <charconv>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace ovf::exec::detail {
namespace {

constexpr std::string_view kIdVariable = "OVF_EXEC_APPLICATION_ID";
constexpr std::string_view kNameVariable = "DINIT_SERVICE";
constexpr std::string_view kReadyVariable = "OVF_EXEC_READY_FD";

Result<std::uint64_t> ParseUnsignedEnvironment(std::string_view variable) {
  const auto* value = std::getenv(std::string(variable).c_str());
  if (value == nullptr || *value == '\0') {
    return MakeError(ErrorCode::configuration_error,
                     std::string(variable) + " is not present in the launch environment");
  }
  std::uint64_t parsed{};
  const std::string_view input(value);
  const auto result = std::from_chars(input.data(), input.data() + input.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != input.data() + input.size()) {
    return MakeError(ErrorCode::configuration_error,
                     std::string(variable) + " is not an unsigned integer");
  }
  return parsed;
}

#if defined(__unix__) || defined(__APPLE__)

volatile sig_atomic_t signal_write_fd = -1;

extern "C" void HandleTerminationSignal(int) {
  const int descriptor = signal_write_fd;
  if (descriptor >= 0) {
    const std::uint8_t token = 1U;
    const auto ignored = ::write(descriptor, &token, sizeof(token));
    static_cast<void>(ignored);
  }
}

class DinitLifecycleBackend final : public LifecycleBackend {
public:
  static Result<std::unique_ptr<LifecycleBackend>> Create(const ApplicationOptions& options) {
    auto id_value = ParseUnsignedEnvironment(kIdVariable);
    if (!id_value || id_value.value() == 0U) {
      return id_value ? MakeError(ErrorCode::invalid_identifier, "application identifier is zero")
                      : id_value.error();
    }
    const ApplicationId id{id_value.value()};
    const auto* name_value = std::getenv(std::string(kNameVariable).c_str());
    if (name_value == nullptr || *name_value == '\0') {
      return MakeError(ErrorCode::configuration_error,
                       "DINIT_SERVICE is not present in the launch environment");
    }
    std::string name{name_value};
    if (options.expected_id && options.expected_id != id) {
      return MakeError(ErrorCode::configuration_error,
                       "launched application identifier does not match the expected identifier");
    }
    if (!options.expected_name.empty() && options.expected_name != name) {
      return MakeError(ErrorCode::configuration_error,
                       "launched application name does not match the expected name");
    }
    auto ready_value = ParseUnsignedEnvironment(kReadyVariable);
    if (!ready_value || ready_value.value() > static_cast<std::uint64_t>(INT_MAX)) {
      return ready_value
                 ? MakeError(ErrorCode::configuration_error, "readiness descriptor is out of range")
                 : ready_value.error();
    }
    const int ready_fd = static_cast<int>(ready_value.value());
    if (::fcntl(ready_fd, F_GETFD) == -1) {
      return MakeError(ErrorCode::configuration_error,
                       "readiness descriptor is not open in this process", errno);
    }

    int stop_pipe[2]{};
    if (::pipe(stop_pipe) != 0) {
      return MakeError(ErrorCode::resource_exhausted, "could not create stop notification pipe",
                       errno);
    }
    for (const int descriptor : stop_pipe) {
      const int status_flags = ::fcntl(descriptor, F_GETFL);
      const int descriptor_flags = ::fcntl(descriptor, F_GETFD);
      if (status_flags == -1 || descriptor_flags == -1 ||
          ::fcntl(descriptor, F_SETFL, status_flags | O_NONBLOCK) == -1 ||
          ::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) == -1) {
        const int error = errno;
        ::close(stop_pipe[0]);
        ::close(stop_pipe[1]);
        return MakeError(ErrorCode::resource_exhausted,
                         "could not configure stop notification pipe", error);
      }
    }

    try {
      auto backend = std::unique_ptr<DinitLifecycleBackend>(
          new DinitLifecycleBackend(id, std::move(name), ready_fd, stop_pipe[0], stop_pipe[1]));
      auto installed = backend->InstallSignalHandlers();
      if (!installed) {
        return installed.error();
      }
      backend->worker_ = std::thread([instance = backend.get()] { instance->DispatchStops(); });
      return std::unique_ptr<LifecycleBackend>(std::move(backend));
    } catch (...) {
      ::close(stop_pipe[0]);
      ::close(stop_pipe[1]);
      return MakeError(ErrorCode::resource_exhausted, "could not create lifecycle worker");
    }
  }

  ~DinitLifecycleBackend() override {
    shutting_down_.store(true, std::memory_order_release);
    const std::uint8_t token = 0U;
    const auto ignored = ::write(stop_write_fd_, &token, sizeof(token));
    static_cast<void>(ignored);
    if (worker_.joinable()) {
      worker_.join();
    }
    RestoreSignalHandlers();
    ::close(stop_read_fd_);
    ::close(stop_write_fd_);
  }

  ApplicationId Id() const noexcept override { return id_; }
  std::string Name() const override { return name_; }

  Result<void> ReportReady() noexcept override {
    bool expected = false;
    if (!ready_reported_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return MakeError(ErrorCode::invalid_transition, "readiness has already been reported");
    }
    const std::uint8_t token = 1U;
    for (;;) {
      if (::write(ready_fd_, &token, sizeof(token)) == sizeof(token)) {
        return {};
      }
      if (errno == EINTR) {
        continue;
      }
      ready_reported_.store(false, std::memory_order_release);
      return MakeError(ErrorCode::communication_error, "readiness notification failed", errno);
    }
  }

  Result<void> ReportFailure(const FailureReport&) noexcept override {
    return MakeError(ErrorCode::unsupported,
                     "failure reporting requires the execution daemon control channel");
  }

  std::uint64_t Subscribe(StopHandler handler) override {
    if (!handler) {
      return 0U;
    }
    std::lock_guard lock(mutex_);
    const auto id = next_subscription_++;
    handlers_.emplace(id, std::move(handler));
    return id;
  }

  void Unsubscribe(std::uint64_t subscription) noexcept override {
    std::lock_guard lock(mutex_);
    handlers_.erase(subscription);
  }

  bool StopRequested() const noexcept override {
    return stop_requested_.load(std::memory_order_acquire);
  }

  StopReason GetStopReason() const noexcept override {
    return stop_requested_.load(std::memory_order_acquire) ? StopReason::supervisor_request
                                                           : StopReason::none;
  }

  Result<StopReason> WaitForStop(Deadline deadline) noexcept override {
    std::unique_lock lock(mutex_);
    if (!condition_.wait_until(
            lock, deadline, [this] { return stop_requested_.load(std::memory_order_acquire); })) {
      return MakeError(ErrorCode::deadline_exceeded, "stop wait reached its deadline");
    }
    return StopReason::supervisor_request;
  }

private:
  DinitLifecycleBackend(ApplicationId id, std::string name, int ready_fd, int stop_read_fd,
                        int stop_write_fd)
      : id_(id), name_(std::move(name)), ready_fd_(ready_fd), stop_read_fd_(stop_read_fd),
        stop_write_fd_(stop_write_fd) {}

  Result<void> InstallSignalHandlers() {
    if (signal_write_fd != -1) {
      return MakeError(ErrorCode::already_exists,
                       "termination signal handling is already installed");
    }
    struct sigaction action{};
    action.sa_handler = HandleTerminationSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (::sigaction(SIGTERM, &action, &previous_term_) != 0) {
      return MakeError(ErrorCode::backend_error, "could not install termination signal handlers",
                       errno);
    }
    if (::sigaction(SIGINT, &action, &previous_int_) != 0) {
      const int error = errno;
      ::sigaction(SIGTERM, &previous_term_, nullptr);
      return MakeError(ErrorCode::backend_error, "could not install termination signal handlers",
                       error);
    }
    signal_write_fd = stop_write_fd_;
    handlers_installed_ = true;
    return {};
  }

  void RestoreSignalHandlers() noexcept {
    if (!handlers_installed_) {
      return;
    }
    signal_write_fd = -1;
    ::sigaction(SIGTERM, &previous_term_, nullptr);
    ::sigaction(SIGINT, &previous_int_, nullptr);
  }

  void DispatchStops() {
    while (!shutting_down_.load(std::memory_order_acquire)) {
      struct pollfd descriptor{stop_read_fd_, POLLIN, 0};
      const int result = ::poll(&descriptor, 1, -1);
      if (result < 0 && errno == EINTR) {
        continue;
      }
      if (result <= 0) {
        continue;
      }
      std::uint8_t tokens[64];
      while (::read(stop_read_fd_, tokens, sizeof(tokens)) > 0) {
      }
      if (shutting_down_.load(std::memory_order_acquire)) {
        break;
      }
      stop_requested_.store(true, std::memory_order_release);
      std::vector<StopHandler> handlers;
      {
        std::lock_guard lock(mutex_);
        handlers.reserve(handlers_.size());
        for (const auto& [id, handler] : handlers_) {
          static_cast<void>(id);
          handlers.push_back(handler);
        }
      }
      condition_.notify_all();
      for (auto& handler : handlers) {
        try {
          handler(StopReason::supervisor_request);
        } catch (...) {
          // User callback failures cannot terminate the lifecycle dispatcher.
        }
      }
    }
  }

  ApplicationId id_;
  std::string name_;
  int ready_fd_;
  int stop_read_fd_;
  int stop_write_fd_;
  struct sigaction previous_term_{};
  struct sigaction previous_int_{};
  bool handlers_installed_{};
  std::atomic_bool ready_reported_{false};
  std::atomic_bool stop_requested_{false};
  std::atomic_bool shutting_down_{false};
  std::thread worker_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::unordered_map<std::uint64_t, StopHandler> handlers_;
  std::uint64_t next_subscription_{1U};
};

#endif

} // namespace

Result<std::unique_ptr<LifecycleBackend>>
CreateDinitLifecycleBackend(const ApplicationOptions& options) {
#if defined(__unix__) || defined(__APPLE__)
  return DinitLifecycleBackend::Create(options);
#else
  static_cast<void>(options);
  return MakeError(ErrorCode::unsupported, "the dinit lifecycle backend requires a POSIX platform");
#endif
}

} // namespace ovf::exec::detail
