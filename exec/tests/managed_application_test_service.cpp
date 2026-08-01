// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/application.hpp"

#include <chrono>
#include <fstream>

#include <unistd.h>

int main() {
  if (::access("/var/tmp/ovf-execd-e2e/state/system.started", F_OK) != 0) {
    return 4;
  }
  auto application = ovf::exec::Application::Create(
      {.expected_id = ovf::exec::ApplicationId{1}, .expected_name = "ovf-app-1"});
  if (!application) {
    return 1;
  }
  auto ready = application.value().ReportReady();
  if (!ready) {
    return 2;
  }
  auto stopped = application.value().WaitForStop(ovf::exec::Deadline::max());
  if (!stopped) {
    return 3;
  }
  std::ofstream marker{"/var/tmp/ovf-execd-e2e/state/managed.stopped", std::ios::trunc};
  return marker ? 0 : 5;
}
