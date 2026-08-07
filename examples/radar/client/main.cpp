// SPDX-License-Identifier: Apache-2.0

#include "ovf/app/run.hpp"
#include "ovf_application.hpp"
#include "ovf_logging.hpp"
#include "radar/ovf_contract.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>

namespace {
using namespace example::radar;
using namespace std::chrono_literals;
} // namespace

int main() {
  return ovf::app::Run("ovf-radar-client", "radar.client", [](ovf::app::Context& ctx) {
    auto& logger = ctx.logger();

    auto proxy = RadarServiceProxy::Find(ctx.com(), ovf::app::radar(), 10s);
    if (!proxy) {
      std::cerr << "timed out discovering RadarService\n";
      logger.Warning("timed out discovering RadarService");
      return ovf::app::ExitCode::discovery_timeout;
    }
    logger.Info("RadarService discovered");
    std::cout << "DISCOVERED" << std::endl;

    std::mutex mutex;
    std::condition_variable condition;
    bool radar_received{};
    bool field_received{};
    RadarFrame radar{};
    VehicleState state{};
    auto radar_subscription = proxy->subscribeRadarObjectsChanged();
    auto field_subscription = proxy->subscribeVehicleStateField();
    if (!radar_subscription.valid() || !field_subscription.valid()) {
      std::cerr << "failed to subscribe to RadarService events\n";
      logger.Error("failed to subscribe to RadarService events");
      return ovf::app::ExitCode::subscription_failed;
    }
    radar_subscription.OnSample([&](RadarFrame const& value) {
      std::lock_guard lock(mutex);
      radar = value;
      radar_received = true;
      condition.notify_all();
    });
    field_subscription.OnSample([&](VehicleState const& value) {
      std::lock_guard lock(mutex);
      state = value;
      field_received = true;
      condition.notify_all();
    });
    if (auto ready = ctx.ReportReady(); ready != ovf::app::ExitCode::ok) {
      return ready;
    }

    auto options = ovf::com::CallOptions{std::chrono::steady_clock::now() + 5s};
    auto calibration = proxy->Calibrate({2.0F}, options).get();
    if (!calibration.ok() || calibration.value().acceptedAt != 42) {
      std::cerr << "Calibrate did not return the expected response\n";
      logger.Error("calibration returned an unexpected response");
      return ovf::app::ExitCode::application_error;
    }
    std::cout << "METHOD_OK" << std::endl;

    options.deadline = std::chrono::steady_clock::now() + 5s;
    auto invalid = proxy->Calibrate({-1.0F}, options).get();
    if (!invalid.is<InvalidTarget>() ||
        invalid.as<InvalidTarget>().reason.view() != "target distance must be non-negative") {
      std::cerr << "Calibrate did not return the expected application error\n";
      logger.Error("calibration application error was invalid");
      return ovf::app::ExitCode::application_error;
    }
    std::cout << "APPLICATION_ERROR_OK" << std::endl;

    options.deadline = std::chrono::steady_clock::now() + 5s;
    auto field = proxy->getVehicleStateField(options).get();
    if (!field.ok()) {
      std::cerr << "VehicleStateField read failed\n";
      logger.Error("vehicle state read failed");
      return ovf::app::ExitCode::application_error;
    }
    std::cout << "FIELD_READ_OK" << std::endl;

    options.deadline = std::chrono::steady_clock::now() + 5s;
    auto field_write = proxy->setVehicleStateField({31.25F}, options).get();
    if (field_write) {
      std::cerr << "VehicleStateField write failed\n";
      return ovf::app::ExitCode::application_error;
    }
    options.deadline = std::chrono::steady_clock::now() + 5s;
    auto updated_field = proxy->getVehicleStateField(options).get();
    if (!updated_field.ok() || updated_field.value().speedMetersPerSecond != 31.25F) {
      std::cerr << "VehicleStateField did not retain the written value\n";
      return ovf::app::ExitCode::application_error;
    }
    std::cout << "FIELD_WRITE_OK" << std::endl;

    options.deadline = std::chrono::steady_clock::now() + 50ms;
    auto delayed = proxy->Delay({300}, options).get();
    if (!delayed.has_com_error() || delayed.com_error() != ovf::com::Error::deadline_exceeded) {
      std::cerr << "Delay did not expire at its deadline\n";
      return ovf::app::ExitCode::application_error;
    }
    std::cout << "DEADLINE_OK" << std::endl;

    options.deadline = std::chrono::steady_clock::now() + 5s;
    auto cancel_op = proxy->Delay({300}, options);
    cancel_op.cancel();
    auto cancel_result = cancel_op.get();
    if (!cancel_result.has_com_error() || cancel_result.com_error() != ovf::com::Error::cancelled) {
      std::cerr << "Delay did not complete as cancelled\n";
      return ovf::app::ExitCode::application_error;
    }
    std::cout << "CANCELLATION_OK" << std::endl;

    {
      std::unique_lock lock(mutex);
      if (!condition.wait_for(lock, 10s, [&] { return radar_received && field_received; })) {
        std::cerr << "timed out waiting for event subscriptions\n";
        logger.Warning("timed out waiting for radar subscriptions");
        return ovf::app::ExitCode::application_error;
      }
    }
    if (radar.objects.size() != 1 || radar.objects.values()[0].id != 7 ||
        state.speedMetersPerSecond <= 13.5F) {
      std::cerr << "received invalid event data\n";
      logger.Error("received invalid radar event data");
      return ovf::app::ExitCode::application_error;
    }
    std::cout << "EVENT_OK\nFIELD_NOTIFICATION_OK" << std::endl;

    return ctx.Run();
  });
}
