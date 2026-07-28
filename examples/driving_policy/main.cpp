// SPDX-License-Identifier: Apache-2.0

#include "environment_model/ovf_contract.hpp"
#include "environment_model/ovf_deployment.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>

namespace {
using namespace example::environment;
using namespace std::chrono_literals;
} // namespace

int main() {
  ovf::com::Runtime runtime(
      {.instance_name = "ovf-driving-policy", .logger = {}, .dispatcher = {}});
  if (environment_model_deployment::Configure(runtime) != ovf::com::RuntimeError::none ||
      runtime.Start() != ovf::com::RuntimeError::none) {
    std::cerr << "failed to start driving policy runtime\n";
    return 1;
  }

  std::mutex mutex;
  std::condition_variable condition;
  auto discovery = ovf::com::Discover(runtime, {environment_model_deployment::Route()});
  if (!discovery) {
    std::cerr << "failed to discover EnvironmentModelService\n";
    return 2;
  }
  discovery->on_change([&](std::span<ovf::com::ServiceRoute const> routes) {
    if (!routes.empty()) {
      condition.notify_all();
    }
  });
  std::optional<ovf::com::ServiceRoute> route;
  {
    std::unique_lock lock(mutex);
    condition.wait_for(lock, 10s, [&] {
      route = discovery->select();
      return route.has_value();
    });
  }
  if (!route) {
    std::cerr << "timed out discovering EnvironmentModelService\n";
    return 3;
  }

  auto binding = ovf::com::Connect(runtime, *route);
  if (!binding) {
    std::cerr << "failed to connect to EnvironmentModelService\n";
    return 4;
  }
  EnvironmentModelServiceProxy proxy(std::move(binding));
  auto subscription = proxy.subscribeEnvironmentModelChanged();
  if (!subscription.valid()) {
    std::cerr << "failed to subscribe to the environment model\n";
    return 5;
  }
  bool received{};
  subscription.on_sample([&](EnvironmentModel const& model) {
    std::lock_guard lock(mutex);
    if (!model.objects.empty()) {
      received = true;
      std::cout << "ENVIRONMENT_MODEL_RECEIVED " << model.objects.size() << std::endl;
      condition.notify_all();
    }
  });
  {
    std::unique_lock lock(mutex);
    if (!condition.wait_for(lock, 15s, [&] { return received; })) {
      std::cerr << "timed out waiting for fused environment data\n";
      return 6;
    }
  }
  subscription.close();
  discovery->close();
  runtime.Stop();
  return 0;
}
