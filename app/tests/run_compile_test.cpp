// SPDX-License-Identifier: Apache-2.0

// Compile-only smoke test: instantiate ovf::app::Run with stub generator
// helpers so we can verify the scaffold on hosts without the DLT binding.

#include "ovf/app/run.hpp"

namespace ovf::app {

// Stand-ins for the per-application generated helpers. Their real bodies are
// emitted by ovf_cc_application and pull in the platform-specific log
// runtime; here we only need signatures compatible with Run().
inline ovf::com::ApplicationRuntime CreateRuntime(std::string) {
  return ovf::com::ApplicationRuntime(ovf::com::RuntimeConfig{},
                                      std::vector<ovf::com::TransportRegistration>{});
}
inline std::unique_ptr<ovf::log::Runtime> CreateLogRuntime() noexcept { return nullptr; }

} // namespace ovf::app

int main() {
  // Instantiating the template exercises the header. The body is never run.
  auto instantiate = +[]() {
    return ovf::app::Run("stub", [](ovf::app::Context&) { return ovf::app::ExitCode::ok; });
  };
  (void)instantiate;
  return 0;
}
