// SPDX-License-Identifier: Apache-2.0

#include "environment_model/ovf_contract.hpp"
#include "ovf/exec/application.hpp"
#include "ovf_application.hpp"
#include "ovf_logging.hpp"
#include "ovf_persistence.hpp"
#include "policy_state/ovf_record.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string_view>

namespace {
using namespace example::environment;
using namespace std::chrono_literals;

constexpr ovf::log::Event kEnvironmentModelReceived{0x36A10001U, "environment_model_received",
                                                    ovf::log::Level::info};

int PersistenceFailure(std::string_view operation, const ovf::per::Error& error) {
  std::cerr << operation << ": " << error.message << '\n';
  return 4;
}
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
  auto persistence = ovf::deployment::driving_policy::CreateRuntime();
  if (!persistence) {
    return PersistenceFailure("failed to start persistence runtime", persistence.error());
  }
  auto policy_store = ovf::deployment::driving_policy::OpenPolicyState(*persistence.value());
  if (!policy_store) {
    return PersistenceFailure("failed to open policy state", policy_store.error());
  }

  std::uint64_t sequence{};
  {
    auto initial_read = policy_store.value().BeginRead();
    if (!initial_read) {
      return PersistenceFailure("failed to read policy state", initial_read.error());
    }
    auto restored = example::driving_policy::PolicyStatePersistent::Get(initial_read.value());
    if (!restored) {
      return PersistenceFailure("failed to decode policy state", restored.error());
    }
    if (restored.value()) {
      sequence = restored.value()->sequence;
      std::cout << "POLICY_STATE_RECOVERED sequence=" << sequence << std::endl;
    }
  }

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
  bool persistence_failed{};
  subscription.on_sample([&](EnvironmentModel const& model) {
    std::lock_guard lock(mutex);
    if (!model.objects.empty()) {
      auto write = policy_store.value().BeginWrite();
      if (!write) {
        persistence_failed = true;
        static_cast<void>(logger.Error("failed to begin policy state update"));
        condition.notify_all();
        return;
      }
      example::driving_policy::PolicyState state{
          .sequence = ++sequence,
          .objectCount = static_cast<std::uint32_t>(model.objects.size()),
          .producedAt = model.producedAt,
      };
      auto stored = example::driving_policy::PolicyStatePersistent::Put(write.value(), state, 1024);
      if (!stored) {
        persistence_failed = true;
        static_cast<void>(logger.Error("failed to encode policy state"));
        condition.notify_all();
        return;
      }
      auto committed = write.value().Commit();
      if (!committed) {
        persistence_failed = true;
        static_cast<void>(logger.Error("failed to commit policy state"));
        condition.notify_all();
        return;
      }
      std::cout << "POLICY_STATE_COMMITTED sequence=" << sequence << std::endl;
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
    if (!condition.wait_for(lock, 15s, [&] { return received || persistence_failed; })) {
      std::cerr << "timed out waiting for fused environment data\n";
      subscription.close();
      static_cast<void>(logger.Warning("timed out waiting for fused environment data"));
      return 7;
    }
    if (persistence_failed) {
      std::cerr << "failed to persist driving policy state\n";
      subscription.close();
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
