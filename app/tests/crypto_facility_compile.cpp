// SPDX-License-Identifier: Apache-2.0

#include "ovf_application.hpp"

int main() {
  auto startup = ovf::app::CreateCryptoRuntime();
  return startup.has_value() && !startup->has_value() ? 0 : 1;
}
