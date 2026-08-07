// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/application.hpp"
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
  auto execution = ovf::exec::Application::Create();
  if (!execution) {
    std::cerr << "failed to create the radar client execution context\n";
    return 1;
  }
  auto communication = ovf::app::CreateRuntime("ovf-radar-client");
  if (!communication) {
    std::cerr << "failed to start the radar client runtime\n";
    return 2;
  }
  auto logging = ovf::app::CreateLogRuntime();
  if (!logging) {
    std::cerr << "failed to start radar client logging runtime\n";
    return 3;
  }
  auto logger = logging->CreateLogger("radar.client");

  std::mutex mutex;
  std::condition_variable condition;
  auto proxy = RadarServiceProxy::Find(communication.get(), ovf::app::radar(), 10s);
  if (!proxy) {
    std::cerr << "timed out discovering RadarService\n";
    static_cast<void>(logger.Warning("timed out discovering RadarService"));
    return 4;
  }
  static_cast<void>(logger.Info("RadarService discovered"));
  std::cout << "DISCOVERED" << std::endl;

  bool radar_received{};
  bool field_received{};
  RadarFrame radar{};
  VehicleState state{};
  auto radar_subscription = proxy->subscribeRadarObjectsChanged();
  auto field_subscription = proxy->subscribeVehicleStateField();
  if (!radar_subscription.valid() || !field_subscription.valid()) {
    std::cerr << "failed to subscribe to RadarService events\n";
    static_cast<void>(logger.Error("failed to subscribe to RadarService events"));
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
  auto ready = execution.value().ReportReady();
  if (!ready) {
    std::cerr << "failed to report radar client readiness\n";
    field_subscription.close();
    radar_subscription.close();
    static_cast<void>(logger.Error("failed to report radar client readiness"));
    return 6;
  }

  auto options = ovf::com::CallOptions{std::chrono::steady_clock::now() + 5s};
  auto calibration = proxy->Calibrate({2.0F}, options).get(options);
  if (!std::holds_alternative<CalibrateOutput>(calibration) ||
      std::get<CalibrateOutput>(calibration).acceptedAt != 42) {
    std::cerr << "Calibrate did not return the expected response\n";
    static_cast<void>(logger.Error("calibration returned an unexpected response"));
    return 7;
  }
  std::cout << "METHOD_OK" << std::endl;

  options.deadline = std::chrono::steady_clock::now() + 5s;
  auto invalid = proxy->Calibrate({-1.0F}, options).get(options);
  if (invalid.index() != 1 || std::get<InvalidTarget>(std::get<1>(invalid)).reason.view() !=
                                  "target distance must be non-negative") {
    std::cerr << "Calibrate did not return the expected application error (result index "
              << invalid.index() << ")\n";
    static_cast<void>(logger.Error("calibration application error was invalid",
                                   ovf::log::Field::Unsigned("result_index", invalid.index())));
    return 8;
  }
  std::cout << "APPLICATION_ERROR_OK" << std::endl;

  options.deadline = std::chrono::steady_clock::now() + 5s;
  auto field = proxy->getVehicleStateField(options).get(options);
  if (!std::holds_alternative<VehicleState>(field)) {
    std::cerr << "VehicleStateField read failed\n";
    static_cast<void>(logger.Error("vehicle state read failed"));
    return 9;
  }
  std::cout << "FIELD_READ_OK" << std::endl;

  options.deadline = std::chrono::steady_clock::now() + 5s;
  auto field_write = proxy->setVehicleStateField({31.25F}, options).get(options);
  if (field_write) {
    std::cerr << "VehicleStateField write failed\n";
    return 10;
  }
  options.deadline = std::chrono::steady_clock::now() + 5s;
  auto updated_field = proxy->getVehicleStateField(options).get(options);
  if (!std::holds_alternative<VehicleState>(updated_field) ||
      std::get<VehicleState>(updated_field).speedMetersPerSecond != 31.25F) {
    std::cerr << "VehicleStateField did not retain the written value\n";
    return 11;
  }
  std::cout << "FIELD_WRITE_OK" << std::endl;

  options.deadline = std::chrono::steady_clock::now() + 50ms;
  auto delayed = proxy->Delay({300}, options).get(options);
  if (!std::holds_alternative<ovf::com::Error>(delayed) ||
      std::get<ovf::com::Error>(delayed) !=
          ovf::com::Error::deadline_exceeded) {
    std::cerr << "Delay did not expire at its deadline\n";
    return 12;
  }
  std::cout << "DEADLINE_OK" << std::endl;

  options.deadline = std::chrono::steady_clock::now() + 5s;
  auto cancelled = proxy->Delay({300}, options);
  cancelled.cancel();
  auto cancelled_result = cancelled.get(options);
  if (!std::holds_alternative<ovf::com::Error>(cancelled_result) ||
      std::get<ovf::com::Error>(cancelled_result) !=
          ovf::com::Error::cancelled) {
    std::cerr << "Delay did not complete as cancelled\n";
    return 13;
  }
  std::cout << "CANCELLATION_OK" << std::endl;

  {
    std::unique_lock lock(mutex);
    if (!condition.wait_for(lock, 10s, [&] { return radar_received && field_received; })) {
      std::cerr << "timed out waiting for event subscriptions\n";
      static_cast<void>(logger.Warning("timed out waiting for radar subscriptions"));
      return 14;
    }
  }
  if (radar.objects.size() != 1 || radar.objects.values()[0].id != 7 ||
      state.speedMetersPerSecond <= 13.5F) {
    std::cerr << "received invalid event data\n";
    static_cast<void>(logger.Error("received invalid radar event data"));
    return 15;
  }
  std::cout << "EVENT_OK\nFIELD_NOTIFICATION_OK" << std::endl;

  auto stopped = execution.value().WaitForStop(ovf::exec::Deadline::max());
  if (!stopped) {
    std::cerr << "failed while waiting for radar client termination\n";
    field_subscription.close();
    radar_subscription.close();
    static_cast<void>(logger.Error("failed while waiting for termination"));
    return 16;
  }
  field_subscription.close();
  radar_subscription.close();
  return 0;
}
