// SPDX-License-Identifier: Apache-2.0

#include "ovf/log/dlt_sink.hpp"
#include "ovf/log/log.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;

class Child final {
public:
  Child() = default;
  explicit Child(pid_t pid) : pid_(pid) {}
  ~Child() { Stop(); }
  Child(Child const&) = delete;
  Child& operator=(Child const&) = delete;
  Child(Child&& other) noexcept : pid_(std::exchange(other.pid_, -1)) {}

  void Stop() noexcept {
    if (pid_ <= 0)
      return;
    kill(pid_, SIGINT);
    for (int count = 0; count < 50; ++count) {
      if (waitpid(pid_, nullptr, WNOHANG) == pid_) {
        pid_ = -1;
        return;
      }
      std::this_thread::sleep_for(10ms);
    }
    kill(pid_, SIGKILL);
    waitpid(pid_, nullptr, 0);
    pid_ = -1;
  }

private:
  pid_t pid_{-1};
};

std::filesystem::path FindExecutable(std::string_view name) {
  auto const* root = std::getenv("TEST_SRCDIR");
  if (root == nullptr)
    return {};
  for (auto const& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (entry.is_regular_file() && entry.path().filename() == name &&
        access(entry.path().c_str(), X_OK) == 0)
      return entry.path();
  }
  return {};
}

Child Spawn(std::filesystem::path const& executable, std::vector<std::string> arguments,
            std::filesystem::path const& output) {
  const auto pid = fork();
  if (pid != 0)
    return Child(pid);
  const auto descriptor = open(output.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
  if (descriptor < 0)
    _exit(126);
  dup2(descriptor, STDOUT_FILENO);
  dup2(descriptor, STDERR_FILENO);
  close(descriptor);
  std::vector<char*> values;
  values.push_back(const_cast<char*>(executable.c_str()));
  for (auto& argument : arguments)
    values.push_back(argument.data());
  values.push_back(nullptr);
  execv(executable.c_str(), values.data());
  _exit(127);
}

bool WaitForPort() {
  for (int attempt = 0; attempt < 100; ++attempt) {
    const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(3490);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    const bool connected =
        connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    close(descriptor);
    if (connected)
      return true;
    std::this_thread::sleep_for(20ms);
  }
  return false;
}

std::string Read(std::filesystem::path const& path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

TEST(DltEndToEndTest, DaemonReceivesStructuredEventAndTypedPayload) {
  auto const daemon_path = FindExecutable("dlt-daemon");
  auto const receiver_path = FindExecutable("dlt-receive");
  ASSERT_FALSE(daemon_path.empty());
  ASSERT_FALSE(receiver_path.empty());
  auto const output_root = std::filesystem::path(std::getenv("TEST_UNDECLARED_OUTPUTS_DIR"));
  auto const ipc = output_root / "dlt-ipc";
  std::filesystem::create_directories(ipc);
  ASSERT_EQ(setenv("DLT_PIPE_DIR", ipc.c_str(), 1), 0);

  auto daemon = Spawn(daemon_path, {"-t", ipc.string()}, output_root / "daemon.log");
  ASSERT_TRUE(WaitForPort()) << Read(output_root / "daemon.log");
  auto receiver = Spawn(receiver_path, {"-a", "127.0.0.1"}, output_root / "received.log");
  std::this_thread::sleep_for(150ms);

  constexpr std::array mappings{
      ovf::log::DltContextMapping{"camera.capture", "CAPT", "Camera capture"},
  };
  auto runtime =
      ovf::log::Runtime::Create({.application_name = "camera", .shutdown_flush = 1s},
                                ovf::log::CreateDltSink({.application_id = "CAMR",
                                                         .application_description = "Camera",
                                                         .contexts = mappings,
                                                         .verbose = true,
                                                         .shutdown_flush = 1s}));
  ASSERT_NE(runtime, nullptr);
  auto logger = runtime->CreateLogger("camera.capture");
  constexpr ovf::log::Event event{0x125A0041U, "frame_published", ovf::log::Level::info};
  ASSERT_EQ(
      logger.Event(event, ovf::log::Field::Unsigned("sequence", 42),
                   ovf::log::Field::Text("classification", "vehicle"),
                   ovf::log::Field::Floating("distance_m", 12.5),
                   ovf::log::Field::Text("token", "must-not-leak", ovf::log::Sensitivity::secret)),
      ovf::log::WriteResult::accepted);
  ASSERT_EQ(logger.Warning("frame deadline missed", ovf::log::Field::Unsigned("delay_us", 750)),
            ovf::log::WriteResult::accepted);
  ASSERT_TRUE(runtime->Flush(1s));
  std::this_thread::sleep_for(250ms);
  runtime->Stop();
  receiver.Stop();
  daemon.Stop();

  auto const received = Read(output_root / "received.log");
  EXPECT_NE(received.find("CAMR"), std::string::npos) << received;
  EXPECT_NE(received.find("CAPT"), std::string::npos) << received;
  EXPECT_NE(received.find("frame_published"), std::string::npos) << received;
  EXPECT_NE(received.find("frame deadline missed"), std::string::npos) << received;
  EXPECT_NE(received.find("750"), std::string::npos) << received;
  EXPECT_NE(received.find("vehicle"), std::string::npos) << received;
  EXPECT_NE(received.find("42"), std::string::npos) << received;
  EXPECT_NE(received.find("REDACTED"), std::string::npos) << received;
  EXPECT_EQ(received.find("must-not-leak"), std::string::npos) << received;
}

} // namespace
