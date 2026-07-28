// SPDX-License-Identifier: Apache-2.0

#include "radar/ovf_contract.hpp"

#include "ovf/com/runtime.hpp"

int main() {
  example::radar::RadarFrame frame{};
  ovf::com::Runtime runtime({
      .instance_name = "separate-radar-app",
      .logger = {},
      .dispatcher =
          [](auto task) {
            task();
            return true;
          },
  });
  return frame.objects.empty() && !runtime.IsRunning() ? 0 : 1;
}
