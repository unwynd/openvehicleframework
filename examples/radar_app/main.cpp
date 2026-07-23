// SPDX-License-Identifier: Apache-2.0

#include "radar_logic.hpp"

#include "ovf/com/runtime.hpp"

int main() {
  auto frame = radar_app::EmptyFrame();
  ovf::com::Runtime runtime({
      .instance_name = "radar-app",
      .logger = {},
      .dispatcher =
          [](auto task) {
            task();
            return true;
          },
  });
  return frame.objects.empty() && !runtime.IsRunning() ? 0 : 1;
}
