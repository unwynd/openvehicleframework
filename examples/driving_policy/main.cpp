// SPDX-License-Identifier: Apache-2.0

#include "environment_model/ovf_contract.hpp"
#include "ovf_application.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>

namespace {
using namespace example::environment;
using namespace std::chrono_literals;
} // namespace

int main() {
  auto application = ovf::app::CreateRuntime("ovf-driving-policy");
  if (!application) {
    std::cerr << "failed to start driving policy runtime\n";
    return 1;
  }

  std::mutex mutex;
  std::condition_variable condition;
  auto proxy =
      EnvironmentModelServiceProxy::Find(application.get(), ovf::app::environment_model(), 10s);
  if (!proxy) {
    std::cerr << "timed out discovering EnvironmentModelService\n";
    return 2;
  }
  auto subscription = proxy->subscribeEnvironmentModelChanged();
  if (!subscription.valid()) {
    std::cerr << "failed to subscribe to the environment model\n";
    return 3;
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
      return 4;
    }
  }
  subscription.close();
  return 0;
}
