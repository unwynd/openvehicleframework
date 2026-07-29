// SPDX-License-Identifier: Apache-2.0

#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include <unistd.h>

namespace {

volatile sig_atomic_t stopping = 0;

extern "C" void Stop(int) { stopping = 1; }

} // namespace

int main() {
  const auto* value = std::getenv("OVF_EXEC_READY_FD");
  if (value == nullptr) {
    return 2;
  }
  int descriptor{-1};
  const std::string_view input(value);
  const auto parsed = std::from_chars(input.data(), input.data() + input.size(), descriptor);
  if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || descriptor < 0) {
    return 3;
  }

  struct sigaction action{};
  action.sa_handler = Stop;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGTERM, &action, nullptr) != 0) {
    return 4;
  }
  const std::uint8_t ready = 1U;
  if (::write(descriptor, &ready, sizeof(ready)) != sizeof(ready)) {
    return 5;
  }
  while (stopping == 0) {
    ::pause();
  }
  return 0;
}
