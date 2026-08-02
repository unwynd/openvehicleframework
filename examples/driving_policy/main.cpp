// SPDX-License-Identifier: Apache-2.0

#include "environment_model/ovf_contract.hpp"
#include "ovf/exec/application.hpp"
#include "ovf_application.hpp"
#include "ovf_logging.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>

namespace {
using namespace example::environment;
using namespace std::chrono_literals;

constexpr ovf::log::Event kEnvironmentModelReceived{0x36A10001U, "environment_model_received",
                                                    ovf::log::Level::info};
} // namespace

int main() {
  auto execution = ovf::exec::Application::Create();
  if (!execution) {
    std::cerr << "failed to create driving policy execution context\n";
    return 1;
  }
  auto communication = ovf::app::CreateRuntime("ovf-driving-policy");
  if (!communication) {
    std::cerr << "failed to start driving policy runtime\n";
    return 2;
  }
  auto logging = ovf::app::CreateLogRuntime();
  if (!logging) {
    std::cerr << "failed to start driving policy logging runtime\n";
    return 3;
  }
  auto logger = logging->CreateLogger("driving_policy.environment");

  std::mutex mutex;
  std::condition_variable condition;
  auto proxy =
      EnvironmentModelServiceProxy::Find(communication.get(), ovf::app::environment_model(), 10s);
  if (!proxy) {
    std::cerr << "timed out discovering EnvironmentModelService\n";
    static_cast<void>(logger.Warning("timed out discovering EnvironmentModelService"));
    return 4;
  }
  auto subscription = proxy->subscribeEnvironmentModelChanged();
  if (!subscription.valid()) {
    std::cerr << "failed to subscribe to the environment model\n";
    static_cast<void>(logger.Error("failed to subscribe to environment model"));
    return 5;
  }
  bool received{};
  subscription.on_sample([&](EnvironmentModel const& model) {
    std::lock_guard lock(mutex);
    if (!model.objects.empty()) {
      received = true;
      static_cast<void>(logger.Event(kEnvironmentModelReceived,
                                     ovf::log::Field::Unsigned("objects", model.objects.size()),
                                     ovf::log::Field::Unsigned("produced_at", model.producedAt)));
      std::cout << "ENVIRONMENT_MODEL_RECEIVED " << model.objects.size() << std::endl;
      condition.notify_all();
    }
  });
  auto ready = execution.value().ReportReady();
  if (!ready) {
    std::cerr << "failed to report driving policy readiness\n";
    subscription.close();
    static_cast<void>(logger.Error("failed to report driving policy readiness"));
    return 6;
  }
  static_cast<void>(logger.Info("driving policy ready"));
  {
    std::unique_lock lock(mutex);
    if (!condition.wait_for(lock, 15s, [&] { return received; })) {
      std::cerr << "timed out waiting for fused environment data\n";
      subscription.close();
      static_cast<void>(logger.Warning("timed out waiting for fused environment data"));
      return 7;
    }
  }
  auto stopped = execution.value().WaitForStop(ovf::exec::Deadline::max());
  if (!stopped) {
    std::cerr << "failed while waiting for driving policy termination\n";
    subscription.close();
    static_cast<void>(logger.Error("failed while waiting for termination"));
    return 8;
  }
  subscription.close();
  return 0;
}
