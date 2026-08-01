// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/coordinator.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: mode_client SOCKET DOMAIN MODE\n";
    return 2;
  }
  const auto domain = std::strtoull(argv[2], nullptr, 10);
  const auto mode = std::strtoull(argv[3], nullptr, 10);
  auto coordinator =
      ovf::exec::SystemCoordinator::Connect({std::string{argv[1]}, std::chrono::seconds{5}});
  if (!coordinator) {
    std::cerr << coordinator.error().message << '\n';
    return 3;
  }
  auto transition =
      coordinator.value().RequestMode(ovf::exec::DomainId{domain}, ovf::exec::ModeId{mode});
  if (!transition) {
    std::cerr << transition.error().message << '\n';
    return 4;
  }
  auto completed =
      transition.value().Wait(std::chrono::steady_clock::now() + std::chrono::seconds{30});
  if (!completed) {
    std::cerr << completed.error().message << '\n';
    return 5;
  }
  std::cout << "TRANSITION_COMPLETE " << transition.value().Id().value() << ' ' << domain << ' '
            << mode << '\n';
  return 0;
}
