// SPDX-License-Identifier: Apache-2.0

// Compile-only smoke test: instantiate ovf::app::Run with stub generator
// helpers so we can verify the scaffold on hosts without the DLT binding.

#include "ovf/app/run.hpp"

#include <cassert>

namespace ovf::app {

// Stand-ins for the per-application generated helpers. Their real bodies are
// emitted by ovf_cc_application and pull in the platform-specific log
// runtime; here we only need signatures compatible with Run().
inline ovf::com::ApplicationRuntimeResult CreateRuntime(std::string) {
  return ovf::com::CreateApplicationRuntime(ovf::com::RuntimeConfig{},
                                            std::vector<ovf::com::TransportRegistration>{});
}
inline std::unique_ptr<ovf::log::Runtime> CreateLogRuntime() noexcept { return nullptr; }
inline PersistenceRuntimeStartup CreatePersistenceRuntime() noexcept { return std::nullopt; }
inline CryptoRuntimeStartup CreateCryptoRuntime() noexcept { return std::nullopt; }

} // namespace ovf::app

int main() {
  // Instantiating the template exercises the header. The body is never run.
  auto instantiate = +[]() {
    return ovf::app::Run("stub", [](ovf::app::Context&) { return ovf::app::ExitCode::ok; });
  };
  (void)instantiate;

  struct TestRuntime {};
  {
    ovf::app::RuntimeHandle<TestRuntime> owned(
        new TestRuntime, +[](TestRuntime* runtime) noexcept { delete runtime; });
    assert(owned.get() != nullptr);
    ovf::app::RuntimeHandle<TestRuntime> moved(std::move(owned));
    assert(owned.get() == nullptr);
    assert(moved.get() != nullptr);
  }
  return 0;
}
