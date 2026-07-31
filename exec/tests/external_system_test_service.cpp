// SPDX-License-Identifier: Apache-2.0

#include <csignal>
#include <fstream>

#include <unistd.h>

namespace {

constexpr const char* kStarted = "/tmp/ovf-execd-e2e/state/system.started";
constexpr const char* kManagedStopped = "/tmp/ovf-execd-e2e/state/managed.stopped";
constexpr const char* kStopped = "/tmp/ovf-execd-e2e/state/system.stopped";
volatile sig_atomic_t stopping = 0;

extern "C" void Stop(int) { stopping = 1; }

} // namespace

int main() {
  struct sigaction action{};
  action.sa_handler = Stop;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGTERM, &action, nullptr) != 0) {
    return 1;
  }
  {
    std::ofstream marker{kStarted, std::ios::trunc};
    if (!marker) {
      return 2;
    }
  }
  const unsigned char ready = 1U;
  if (::write(3, &ready, sizeof(ready)) != sizeof(ready)) {
    return 5;
  }
  while (stopping == 0) {
    ::pause();
  }
  if (::access(kManagedStopped, F_OK) != 0) {
    return 3;
  }
  std::ofstream marker{kStopped, std::ios::trunc};
  return marker ? 0 : 4;
}
