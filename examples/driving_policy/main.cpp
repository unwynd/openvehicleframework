// SPDX-License-Identifier: Apache-2.0

#include "environment_model/ovf_contract.hpp"
#include "ovf/app/run.hpp"
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
using namespace ovf::log::literals;

void ReportPersistenceFailure(std::string_view operation, const ovf::per::Error& error) {
  std::cerr << operation << ": " << error.message << '\n';
}
} // namespace

int main() {
  return ovf::app::Run(
      "ovf-driving-policy", "driving_policy.environment", [](ovf::app::Context& ctx) {
        auto& logger = ctx.logger();

        auto* persistence = ctx.per();
        if (persistence == nullptr) {
          std::cerr << "persistence runtime is not available\n";
          return ovf::app::ExitCode::persistence_init_failed;
        }
        auto policy_store = ovf::deployment::driving_policy::OpenPolicyState(*persistence);
        if (!policy_store) {
          ReportPersistenceFailure("failed to open policy state", policy_store.error());
          return ovf::app::ExitCode::persistence_init_failed;
        }

        std::uint64_t sequence{};
        {
          auto initial_read = policy_store.value().BeginRead();
          if (!initial_read) {
            ReportPersistenceFailure("failed to read policy state", initial_read.error());
            return ovf::app::ExitCode::persistence_init_failed;
          }
          auto restored = example::driving_policy::PolicyStatePersistent::Get(initial_read.value());
          if (!restored) {
            ReportPersistenceFailure("failed to decode policy state", restored.error());
            return ovf::app::ExitCode::persistence_init_failed;
          }
          if (restored.value()) {
            sequence = restored.value()->sequence;
            std::cout << "POLICY_STATE_RECOVERED sequence=" << sequence << std::endl;
          }
        }

        auto proxy =
            EnvironmentModelServiceProxy::Find(ctx.com(), ovf::app::environment_model(), 10s);
        if (!proxy) {
          std::cerr << "timed out discovering EnvironmentModelService\n";
          logger.Warning("timed out discovering EnvironmentModelService");
          return ovf::app::ExitCode::discovery_timeout;
        }
        auto subscription = proxy->subscribeEnvironmentModelChanged();
        if (!subscription.valid()) {
          std::cerr << "failed to subscribe to the environment model\n";
          logger.Error("failed to subscribe to environment model");
          return ovf::app::ExitCode::subscription_failed;
        }

        std::mutex mutex;
        std::condition_variable condition;
        bool received{};
        bool persistence_failed{};
        subscription.OnSample([&](EnvironmentModel const& model) {
          std::lock_guard lock(mutex);
          if (model.objects.empty()) {
            return;
          }
          example::driving_policy::PolicyState state{
              .sequence = ++sequence,
              .objectCount = static_cast<std::uint32_t>(model.objects.size()),
              .producedAt = model.producedAt,
          };
          auto committed = policy_store.value().With([&](ovf::per::WriteTransaction& tx) {
            return example::driving_policy::PolicyStatePersistent::Put(tx, state)
                .map([](bool) {})
                .or_else([](ovf::per::Error err) -> ovf::per::Result<void> { return err; });
          });
          if (!committed) {
            persistence_failed = true;
            logger.Error("failed to persist policy state");
            condition.notify_all();
            return;
          }
          std::cout << "POLICY_STATE_COMMITTED sequence=" << sequence << std::endl;
          received = true;
          static_cast<void>(logger.Event(ovf::app::kEnvironmentModelReceived,
                                         "objects"_field = model.objects.size(),
                                         "produced_at"_field = model.producedAt));
          std::cout << "ENVIRONMENT_MODEL_RECEIVED " << model.objects.size() << std::endl;
          condition.notify_all();
        });

        if (auto ready = ctx.ReportReady(); ready != ovf::app::ExitCode::ok) {
          return ready;
        }
        logger.Info("driving policy ready");
        {
          std::unique_lock lock(mutex);
          if (!condition.wait_for(lock, 15s, [&] { return received || persistence_failed; })) {
            std::cerr << "timed out waiting for fused environment data\n";
            logger.Warning("timed out waiting for fused environment data");
            return ovf::app::ExitCode::application_error;
          }
          if (persistence_failed) {
            std::cerr << "failed to persist driving policy state\n";
            return ovf::app::ExitCode::application_error;
          }
        }
        return ctx.Run();
      });
}
