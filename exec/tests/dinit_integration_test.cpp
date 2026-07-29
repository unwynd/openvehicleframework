// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/backends/dinit.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using namespace ovf::exec;
using namespace ovf::exec::backends;

class ChildProcess final {
public:
  explicit ChildProcess(pid_t process) : process_(process) {}
  ~ChildProcess() {
    if (process_ <= 0) {
      return;
    }
    ::kill(process_, SIGTERM);
    for (int attempt = 0; attempt < 100; ++attempt) {
      if (::waitpid(process_, nullptr, WNOHANG) == process_) {
        process_ = -1;
        return;
      }
      std::this_thread::sleep_for(10ms);
    }
    ::kill(process_, SIGKILL);
    ::waitpid(process_, nullptr, 0);
  }

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

private:
  pid_t process_;
};

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    const auto* test_directory = std::getenv("TEST_TMPDIR");
    std::string pattern =
        std::string{test_directory == nullptr ? "/tmp" : test_directory} + "/ovf-exec-dinit-XXXXXX";
    storage_.assign(pattern.begin(), pattern.end());
    storage_.push_back('\0');
    const auto* created = ::mkdtemp(storage_.data());
    if (created != nullptr) {
      path_ = created;
    }
  }

  ~TemporaryDirectory() {
    if (!path_.empty()) {
      ::unlink((path_ + "/boot").c_str());
      ::unlink((path_ + "/camera").c_str());
      ::unlink((path_ + "/control.sock").c_str());
      ::rmdir(path_.c_str());
    }
  }

  const std::string& path() const noexcept { return path_; }

private:
  std::vector<char> storage_;
  std::string path_;
};

std::string AbsolutePath(const char* path) {
  char resolved[PATH_MAX]{};
  return ::realpath(path, resolved) == nullptr ? std::string{} : std::string{resolved};
}

TEST(DinitIntegrationTest, StartsObservesAndStopsReadinessReportingService) {
  const auto* daemon = std::getenv("OVF_TEST_DINIT");
  const auto* service = std::getenv("OVF_TEST_DINIT_SERVICE");
  ASSERT_NE(daemon, nullptr);
  ASSERT_NE(service, nullptr);

  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const auto daemon_path = AbsolutePath(daemon);
  const auto service_path = AbsolutePath(service);
  ASSERT_FALSE(daemon_path.empty()) << "cannot resolve dinit executable: " << daemon;
  ASSERT_FALSE(service_path.empty()) << "cannot resolve test service: " << service;
  const auto socket = temporary.path() + "/control.sock";
  const auto boot_description = temporary.path() + "/boot";
  const auto description = temporary.path() + "/camera";
  {
    std::ofstream output(boot_description);
    ASSERT_TRUE(output);
    output << "type = internal\n";
  }
  {
    std::ofstream output(description);
    ASSERT_TRUE(output);
    output << "type = process\n"
           << "command = " << service_path << "\n"
           << "ready-notification = pipevar:OVF_EXEC_READY_FD\n"
           << "restart = false\n"
           << "start-timeout = 2\n"
           << "stop-timeout = 2\n"
           << "log-type = buffer\n";
  }

  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    ::execl(daemon_path.c_str(), daemon_path.c_str(), "--user", "--services-dir",
            temporary.path().c_str(), "--socket-path", socket.c_str(), nullptr);
    _exit(127);
  }
  ChildProcess daemon_process(pid);

  for (int attempt = 0; attempt < 200 && ::access(socket.c_str(), F_OK) != 0; ++attempt) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_EQ(::access(socket.c_str(), F_OK), 0) << "dinit control socket was not created";

  auto created = CreateDinitBackend({socket, "camera", {{ApplicationId{1}, "camera"}}});
  ASSERT_TRUE(created);
  auto backend = std::move(created).value();
  auto started = backend->Start(ApplicationId{1}, std::chrono::steady_clock::now() + 2s);
  ASSERT_TRUE(started) << started.error().message;
  EXPECT_EQ(started.value().state, ApplicationState::ready);

  auto observed = backend->Inspect(ApplicationId{1});
  ASSERT_TRUE(observed);
  EXPECT_EQ(observed.value().state, ApplicationState::ready);

  auto stopped = backend->Stop(ApplicationId{1}, StopReason::mode_change,
                               std::chrono::steady_clock::now() + 2s);
  ASSERT_TRUE(stopped) << stopped.error().message;
  EXPECT_EQ(stopped.value().state, ApplicationState::stopped);

  auto recovery = backend->RequestSystemRecovery(std::chrono::steady_clock::now() + 2s);
  ASSERT_TRUE(recovery) << recovery.error().message;
  auto recovery_observed = backend->Inspect(ApplicationId{1});
  ASSERT_TRUE(recovery_observed);
  EXPECT_NE(recovery_observed.value().state, ApplicationState::stopped);
}

} // namespace
