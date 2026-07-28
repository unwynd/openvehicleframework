// SPDX-License-Identifier: Apache-2.0

#include "radar/ovf_contract.hpp"
#include "radar/ovf_deployment.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>

namespace {
using namespace example::radar;
using namespace std::chrono_literals;
} // namespace

int main() {
  ovf::com::Runtime runtime({.instance_name = "ovf-radar-client", .logger = {}, .dispatcher = {}});
  if (radar_deployment::Configure(runtime) != ovf::com::RuntimeError::none ||
      runtime.Start() != ovf::com::RuntimeError::none) {
    std::cerr << "failed to start the radar client runtime\n";
    return 1;
  }

  std::mutex mutex;
  std::condition_variable condition;
  auto discovery = ovf::com::Discover(runtime, {radar_deployment::Route()});
  if (!discovery) {
    std::cerr << "failed to start RadarService discovery\n";
    return 2;
  }
  discovery->on_change([&](std::span<ovf::com::ServiceRoute const> routes) {
    if (!routes.empty()) {
      std::lock_guard lock(mutex);
      condition.notify_all();
    }
  });

  std::optional<ovf::com::ServiceRoute> route;
  {
    std::unique_lock lock(mutex);
    if (!condition.wait_for(lock, 10s, [&] {
          route = discovery->select();
          return route.has_value();
        })) {
      std::cerr << "timed out discovering RadarService\n";
      return 3;
    }
  }
  std::cout << "DISCOVERED" << std::endl;

  auto binding = ovf::com::Connect(runtime, *route);
  if (!binding) {
    std::cerr << "failed to connect to RadarService\n";
    return 4;
  }
  RadarServiceProxy proxy(std::move(binding));

  bool radar_received{};
  bool field_received{};
  RadarFrame radar{};
  VehicleState state{};
  auto radar_subscription = proxy.subscribeRadarObjectsChanged();
  auto field_subscription = proxy.subscribeVehicleStateField();
  if (!radar_subscription.valid() || !field_subscription.valid()) {
    std::cerr << "failed to subscribe to RadarService events\n";
    return 5;
  }
  radar_subscription.on_sample([&](RadarFrame const& value) {
    std::lock_guard lock(mutex);
    radar = value;
    radar_received = true;
    condition.notify_all();
  });
  field_subscription.on_sample([&](VehicleState const& value) {
    std::lock_guard lock(mutex);
    state = value;
    field_received = true;
    condition.notify_all();
  });

  auto options = ovf::com::CallOptions{std::chrono::steady_clock::now() + 5s};
  auto calibration = proxy.Calibrate({2.0F}, options).get(options);
  if (!std::holds_alternative<CalibrateOutput>(calibration) ||
      std::get<CalibrateOutput>(calibration).acceptedAt != 42) {
    std::cerr << "Calibrate did not return the expected response\n";
    return 6;
  }
  std::cout << "METHOD_OK" << std::endl;

  options.deadline = std::chrono::steady_clock::now() + 5s;
  auto invalid = proxy.Calibrate({-1.0F}, options).get(options);
  if (invalid.index() != 1 || std::get<InvalidTarget>(std::get<1>(invalid)).reason.view() !=
                                  "target distance must be non-negative") {
    std::cerr << "Calibrate did not return the expected application error\n";
    return 7;
  }
  std::cout << "APPLICATION_ERROR_OK" << std::endl;

  options.deadline = std::chrono::steady_clock::now() + 5s;
  auto field = proxy.getVehicleStateField(options).get(options);
  if (!std::holds_alternative<VehicleState>(field)) {
    std::cerr << "VehicleStateField read failed\n";
    return 8;
  }
  std::cout << "FIELD_READ_OK" << std::endl;

  {
    std::unique_lock lock(mutex);
    if (!condition.wait_for(lock, 10s, [&] { return radar_received && field_received; })) {
      std::cerr << "timed out waiting for event subscriptions\n";
      return 9;
    }
  }
  if (radar.objects.size() != 1 || radar.objects.values()[0].id != 7 ||
      state.speedMetersPerSecond <= 13.5F) {
    std::cerr << "received invalid event data\n";
    return 10;
  }
  std::cout << "EVENT_OK\nFIELD_NOTIFICATION_OK" << std::endl;

  field_subscription.close();
  radar_subscription.close();
  discovery->close();
  runtime.Stop();
  return 0;
}
