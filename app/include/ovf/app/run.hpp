// SPDX-License-Identifier: Apache-2.0

#pragma once

// ovf::app::Run composes exec + com + log initialization for a typical OVF
// application. It replaces the 100–170 lines of bootstrap boilerplate that
// each example previously duplicated: create Application, create com
// runtime, create log runtime + logger, report ready, loop, clean up.
//
// The generated headers `ovf_application.hpp` and `ovf_logging.hpp` (both
// under `namespace ovf::app`) must be included by the translation unit that
// calls Run(); the scaffold looks up their factory functions by name.
//
// Usage:
//   int main() {
//     return ovf::app::Run("ovf-camera", [](ovf::app::Context& ctx) {
//         CameraImplementation impl;
//         auto offer = impl.OfferService(ctx.com(), ovf::app::camera());
//         if (!offer.valid()) return ovf::app::ExitCode::service_offer_failed;
//         ctx.OnTick(100ms, [&](auto& log) {
//             // ... publish, log ...
//             return ovf::app::TickAction::continue_loop;
//         });
//         return ctx.Run();
//     });
//   }

#include "ovf/app/facilities.hpp"
#include "ovf/com/runtime.hpp"
#include "ovf/exec/application.hpp"
#include "ovf/log/log.hpp"

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace ovf::app {

// Forward declarations of the generator-emitted helpers Run relies on. Each
// ovf_cc_application deployment emits definitions in this same namespace; the
// forward declarations here let the template body compile without depending
// on the generated headers being seen first.
ovf::com::ApplicationRuntimeResult CreateRuntime(std::string instance_name);
std::unique_ptr<ovf::log::Runtime> CreateLogRuntime() noexcept;
PersistenceRuntimeStartup CreatePersistenceRuntime() noexcept;
CryptoRuntimeStartup CreateCryptoRuntime() noexcept;

enum class ExitCode : int {
  ok = 0,
  execution_init_failed = 10,
  communication_init_failed = 11,
  logging_init_failed = 12,
  persistence_init_failed = 13,
  crypto_init_failed = 14,
  service_offer_failed = 20,
  discovery_timeout = 21,
  subscription_failed = 22,
  ready_failed = 30,
  runtime_error = 40,
  application_error = 50,
};

enum class TickAction { continue_loop, stop };

class Context final {
public:
  Context(ovf::exec::Application& execution, ovf::com::Runtime& communication,
          ovf::log::Runtime& logging, ovf::log::Logger&& logger,
          PersistenceRuntimeHandle persistence, CryptoRuntimeHandle crypto) noexcept
      : execution_(&execution), communication_(&communication), logging_(&logging),
        logger_(std::move(logger)), persistence_(std::move(persistence)),
        crypto_(std::move(crypto)) {}

  [[nodiscard]] ovf::exec::Application& exec() noexcept { return *execution_; }
  [[nodiscard]] ovf::com::Runtime& com() noexcept { return *communication_; }
  [[nodiscard]] ovf::log::Runtime& log() noexcept { return *logging_; }
  [[nodiscard]] ovf::log::Logger& logger() noexcept { return logger_; }
  [[nodiscard]] ovf::per::Runtime* per() noexcept { return persistence_.get(); }
  [[nodiscard]] const ovf::per::Runtime* per() const noexcept { return persistence_.get(); }
  [[nodiscard]] ovf::crypto::Runtime* crypto() noexcept { return crypto_.get(); }
  [[nodiscard]] const ovf::crypto::Runtime* crypto() const noexcept { return crypto_.get(); }

  // ReportReady wraps Application::ReportReady with a consistent diagnostic.
  [[nodiscard]] ExitCode ReportReady() noexcept {
    auto ready = execution_->ReportReady();
    if (!ready) {
      std::fprintf(stderr, "ovf::app: ReportReady failed: %s\n", ready.error().message.c_str());
      return ExitCode::ready_failed;
    }
    return ExitCode::ok;
  }

  [[nodiscard]] bool StopRequested() const noexcept { return execution_->StopRequested(); }

  // Run keeps the process alive until the executor asks it to stop.
  [[nodiscard]] ExitCode Run() noexcept {
    auto stopped = execution_->WaitForStop(ovf::exec::Deadline::max());
    if (!stopped) {
      std::fprintf(stderr, "ovf::app: WaitForStop failed: %s\n", stopped.error().message.c_str());
      return ExitCode::runtime_error;
    }
    return ExitCode::ok;
  }

  // Tick loops until StopRequested, invoking body with the shared logger on
  // every period. body returns TickAction::stop to break out early.
  template <typename Body>
  [[nodiscard]] ExitCode Tick(std::chrono::steady_clock::duration period, Body body) noexcept {
    while (!execution_->StopRequested()) {
      const auto action = body(logger_);
      if (action == TickAction::stop) {
        return ExitCode::ok;
      }
      std::this_thread::sleep_for(period);
    }
    return ExitCode::ok;
  }

private:
  ovf::exec::Application* execution_;
  ovf::com::Runtime* communication_;
  ovf::log::Runtime* logging_;
  ovf::log::Logger logger_;
  PersistenceRuntimeHandle persistence_;
  CryptoRuntimeHandle crypto_;
};

// Run performs the standard exec + com + log initialization and hands the
// caller a Context. The caller's body returns an ExitCode which becomes the
// process exit code. logger_name defaults to instance_name.
template <typename Body>
int Run(std::string_view instance_name, std::string_view logger_name, Body body) noexcept {
  auto execution = ovf::exec::Application::Create();
  if (!execution) {
    std::fprintf(stderr, "ovf::app: Application::Create failed: %s\n",
                 execution.error().message.c_str());
    return static_cast<int>(ExitCode::execution_init_failed);
  }
  auto communication = CreateRuntime(std::string(instance_name));
  if (!communication) {
    std::fprintf(stderr, "ovf::app: com runtime init failed: %s\n",
                 communication.error().message.c_str());
    return static_cast<int>(ExitCode::communication_init_failed);
  }
  auto logging = CreateLogRuntime();
  if (!logging) {
    std::fprintf(stderr, "ovf::app: log runtime init failed\n");
    return static_cast<int>(ExitCode::logging_init_failed);
  }
  auto logger = logging->CreateLogger(logger_name);
  PersistenceRuntimeHandle persistence;
  if (auto startup = CreatePersistenceRuntime()) {
    if (!*startup) {
      std::fprintf(stderr, "ovf::app: persistence runtime init failed: %s\n",
                   startup->error().message.c_str());
      return static_cast<int>(ExitCode::persistence_init_failed);
    }
    persistence = std::move(*startup).value();
  }
  CryptoRuntimeHandle crypto;
  if (auto startup = CreateCryptoRuntime()) {
    if (!*startup) {
      std::fprintf(stderr, "ovf::app: crypto runtime init failed: %s\n",
                   startup->error().message.c_str());
      return static_cast<int>(ExitCode::crypto_init_failed);
    }
    crypto = std::move(*startup).value();
  }
  Context ctx(execution.value(), communication.value().get(), *logging, std::move(logger),
              std::move(persistence), std::move(crypto));
  return static_cast<int>(body(ctx));
}

template <typename Body> int Run(std::string_view instance_name, Body body) noexcept {
  return Run(instance_name, instance_name, std::move(body));
}

} // namespace ovf::app
