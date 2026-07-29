// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/application.hpp"

#include <chrono>

int main() {
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
  return stopped ? 0 : 3;
}
