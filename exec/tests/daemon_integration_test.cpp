// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/coordinator.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using namespace ovf::exec;

constexpr const char* kRoot = "/tmp/ovf-execd-e2e";

std::string AbsolutePath(const char* path) {
  char resolved[PATH_MAX]{};
  return path != nullptr && ::realpath(path, resolved) != nullptr ? std::string{resolved}
                                                                  : std::string{};
}

class ChildProcess final {
public:
  ChildProcess() = default;
  explicit ChildProcess(pid_t process) : process_(process) {}
  ~ChildProcess() { Stop(); }

  ChildProcess(ChildProcess&& other) noexcept
      : process_(std::exchange(other.process_, static_cast<pid_t>(-1))) {}
  ChildProcess& operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
      Stop();
      process_ = std::exchange(other.process_, static_cast<pid_t>(-1));
    }
    return *this;
  }
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  void Stop() {
    if (process_ <= 0) {
      return;
    }
    ::kill(process_, SIGTERM);
    for (int attempt = 0; attempt < 200; ++attempt) {
      if (::waitpid(process_, nullptr, WNOHANG) == process_) {
        process_ = -1;
        return;
      }
      std::this_thread::sleep_for(10ms);
    }
    ::kill(process_, SIGKILL);
    ::waitpid(process_, nullptr, 0);
    process_ = -1;
  }

private:
  pid_t process_{-1};
};

class TargetLayout final {
public:
  TargetLayout() {
    std::error_code error;
    std::filesystem::remove_all(kRoot, error);
    if (error) {
      throw std::runtime_error{"cannot clean target layout: " + error.message()};
    }
    for (const auto* directory :
         {"bin", "lib", "run", "daemon_deployment.dinit", "state", "artifacts"}) {
      if (!std::filesystem::create_directories(std::string{kRoot} + "/" + directory, error) ||
          error) {
        throw std::runtime_error{"cannot create target layout: " + error.message()};
      }
    }
  }

  ~TargetLayout() {
    std::error_code ignored;
    std::filesystem::remove_all(kRoot, ignored);
  }

  static std::string Path(std::string_view suffix) {
    return std::string{kRoot} + "/" + std::string{suffix};
  }

  void CopyFile(const std::string& source, std::string_view destination) {
    std::error_code error;
    ASSERT_TRUE(std::filesystem::copy_file(
        source, Path(destination), std::filesystem::copy_options::overwrite_existing, error))
        << source << ": " << error.message();
  }

  void CopyDirectory(const std::string& source, std::string_view destination) {
    std::error_code error;
    std::filesystem::copy(source, Path(destination),
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          error);
    ASSERT_FALSE(error) << source << ": " << error.message();
  }
};

ChildProcess Launch(const std::vector<std::string>& arguments) {
  const pid_t process = ::fork();
  EXPECT_GE(process, 0);
  if (process == 0) {
    std::vector<char*> raw;
    raw.reserve(arguments.size() + 1U);
    for (const auto& argument : arguments) {
      raw.push_back(const_cast<char*>(argument.c_str()));
    }
    raw.push_back(nullptr);
    ::execv(raw.front(), raw.data());
    _exit(127);
  }
  return ChildProcess{process};
}

bool WaitForPath(const std::string& path) {
  for (int attempt = 0; attempt < 400; ++attempt) {
    if (::access(path.c_str(), F_OK) == 0) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

Result<SystemCoordinator> Connect() {
  const CoordinatorOptions options{TargetLayout::Path("run/coordinator.sock"), 2s};
  for (int attempt = 0; attempt < 200; ++attempt) {
    auto coordinator = SystemCoordinator::Connect(options);
    if (coordinator) {
      return coordinator;
    }
    std::this_thread::sleep_for(10ms);
  }
  return MakeError(ErrorCode::backend_unavailable, "coordinator did not become available");
}

TEST(ExecutionDaemonIntegrationTest, RunsTransitionAndRecoversJournalAcrossDaemonRestart) {
  if (::getuid() != 0U && ::getuid() != 501U && ::getuid() != 1000U) {
    GTEST_SKIP() << "test deployment does not authorize uid " << ::getuid();
  }
  const auto daemon = AbsolutePath(std::getenv("OVF_TEST_EXEC_DAEMON"));
  const auto dinit = AbsolutePath(std::getenv("OVF_TEST_DINIT"));
  const auto helper = AbsolutePath(std::getenv("OVF_TEST_MANAGED_SERVICE"));
  const auto system_service = AbsolutePath(std::getenv("OVF_TEST_SYSTEM_SERVICE"));
  const auto plugin = AbsolutePath(std::getenv("OVF_TEST_DINIT_PLUGIN"));
  const auto model = AbsolutePath(std::getenv("OVF_TEST_EXECUTION_MODEL"));
  const auto backend = AbsolutePath(std::getenv("OVF_TEST_EXECUTION_BACKEND"));
  const auto services = AbsolutePath(std::getenv("OVF_TEST_EXECUTION_SERVICES"));
  const auto manifest = AbsolutePath(std::getenv("OVF_TEST_EXECUTION_MANIFEST"));
  ASSERT_FALSE(daemon.empty());
  ASSERT_FALSE(dinit.empty());
  ASSERT_FALSE(helper.empty());
  ASSERT_FALSE(system_service.empty());
  ASSERT_FALSE(plugin.empty());
  ASSERT_FALSE(model.empty());
  ASSERT_FALSE(backend.empty());
  ASSERT_FALSE(services.empty());
  ASSERT_FALSE(manifest.empty());

  TargetLayout target;
  target.CopyFile(helper, "bin/managed-test");
  target.CopyFile(system_service, "bin/system-test");
  target.CopyFile(plugin, "lib/libovf_exec_backend_dinit.so");
  target.CopyFile(model, "artifacts/deployment.execution.json");
  target.CopyFile(backend, "artifacts/daemon_deployment.backend.json");
  target.CopyFile(manifest, "artifacts/deployment.manifest.json");
  target.CopyDirectory(services, "daemon_deployment.dinit");

  auto dinit_process =
      Launch({dinit, "--user", "--services-dir", TargetLayout::Path("daemon_deployment.dinit"),
              "--socket-path", TargetLayout::Path("run/dinit.sock")});
  ASSERT_TRUE(WaitForPath(TargetLayout::Path("run/dinit.sock")));

  const std::vector<std::string> daemon_arguments{
      daemon,
      "--model",
      TargetLayout::Path("artifacts/deployment.execution.json"),
      "--backend",
      TargetLayout::Path("artifacts/daemon_deployment.backend.json"),
      "--services",
      TargetLayout::Path("daemon_deployment.dinit"),
      "--manifest",
      TargetLayout::Path("artifacts/deployment.manifest.json"),
  };
  auto execution_daemon = Launch(daemon_arguments);
  auto coordinator = Connect();
  ASSERT_TRUE(coordinator) << coordinator.error().message;

  std::atomic_uint64_t event_count{};
  auto subscription = coordinator.value().Subscribe(
      {}, [&event_count](const CoordinatorEvent&) { event_count.fetch_add(1U); });
  ASSERT_TRUE(subscription) << subscription.error().message;
  auto transition = coordinator.value().RequestMode(DomainId{1}, ModeId{2}, {.timeout = 3s});
  ASSERT_TRUE(transition) << transition.error().message;
  auto completed = transition.value().Wait(std::chrono::steady_clock::now() + 4s);
  ASSERT_TRUE(completed) << completed.error().message;
  EXPECT_EQ(completed.value().phase, TransitionPhase::succeeded);
  auto running = coordinator.value().GetSnapshot();
  ASSERT_TRUE(running);
  ASSERT_EQ(running.value().domains.size(), 1U);
  EXPECT_EQ(running.value().domains.front().committed_mode, ModeId{2});
  ASSERT_EQ(running.value().applications.size(), 1U);
  EXPECT_EQ(running.value().applications.front().state, ApplicationState::ready);
  ASSERT_EQ(running.value().units.size(), 2U);
  EXPECT_EQ(running.value().units[0].kind, ExecutionUnitKind::managed_application);
  EXPECT_EQ(running.value().units[1].kind, ExecutionUnitKind::service);
  for (int attempt = 0; attempt < 100 && event_count.load() == 0U; ++attempt) {
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_GT(event_count.load(), 0U);

  subscription.value().Reset();
  execution_daemon.Stop();
  ASSERT_TRUE(std::filesystem::exists(TargetLayout::Path("state/journal.v1")));

  execution_daemon = Launch(daemon_arguments);
  auto recovered = Connect();
  ASSERT_TRUE(recovered) << recovered.error().message;
  auto snapshot = recovered.value().GetSnapshot();
  ASSERT_TRUE(snapshot) << snapshot.error().message;
  EXPECT_EQ(snapshot.value().model_generation, ModelGeneration{17});
  ASSERT_EQ(snapshot.value().domains.size(), 1U);
  EXPECT_EQ(snapshot.value().domains.front().committed_mode, ModeId{2});
  ASSERT_EQ(snapshot.value().applications.size(), 1U);
  EXPECT_EQ(snapshot.value().applications.front().state, ApplicationState::ready);

  auto stopped = recovered.value().RequestMode(DomainId{1}, ModeId{1}, {.timeout = 3s});
  ASSERT_TRUE(stopped) << stopped.error().message;
  auto stop_result = stopped.value().Wait(std::chrono::steady_clock::now() + 4s);
  ASSERT_TRUE(stop_result) << stop_result.error().message;
  EXPECT_EQ(stop_result.value().phase, TransitionPhase::succeeded);
  auto final_snapshot = recovered.value().GetSnapshot();
  ASSERT_TRUE(final_snapshot);
  EXPECT_EQ(final_snapshot.value().applications.front().state, ApplicationState::stopped);
  ASSERT_TRUE(WaitForPath(TargetLayout::Path("state/system.stopped")));
}

} // namespace
