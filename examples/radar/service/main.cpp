// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/application.hpp"
#include "ovf_application.hpp"
#include "ovf_logging.hpp"
#include "radar/ovf_contract.hpp"

#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
using namespace example::radar;
using namespace std::chrono_literals;

class RadarService final : public RadarServiceSkeleton {
public:
  auto Calibrate(CalibrateInput const& request)
      -> ovf::com::MethodResult<CalibrateOutput, std::variant<InvalidTarget>> override {
    if (request.targetDistanceMeters < 0.0F) {
      InvalidTarget error{};
      if (!error.reason.assign("target distance must be non-negative")) {
        return ovf::com::Error::provider_failure;
      }
      return std::variant<InvalidTarget>{std::move(error)};
    }
    return CalibrateOutput{42};
  }

  auto getVehicleStateField() -> ovf::com::MethodResult<VehicleState, std::monostate> override {
    std::lock_guard lock(mutex_);
    return state_;
  }

  auto setVehicleStateField(VehicleState const& value)
      -> std::optional<ovf::com::Error> override {
    std::lock_guard lock(mutex_);
    state_ = value;
    externally_set_ = true;
    return std::nullopt;
  }

  auto Delay(DelayInput const& request)
      -> ovf::com::MethodResult<DelayOutput, std::monostate> override {
    std::this_thread::sleep_for(std::chrono::milliseconds(request.milliseconds));
    return DelayOutput{request.milliseconds};
  }

  auto set_speed(float speed) -> void {
    std::lock_guard lock(mutex_);
    if (!externally_set_)
      state_.speedMetersPerSecond = speed;
  }

  [[nodiscard]] auto state() const -> VehicleState {
    std::lock_guard lock(mutex_);
    return state_;
  }

private:
  mutable std::mutex mutex_;
  VehicleState state_{13.5F};
  bool externally_set_{};
};
} // namespace

int main() {
  auto execution = ovf::exec::Application::Create();
  if (!execution) {
    std::cerr << "failed to create the radar execution context\n";
    return 1;
  }
  auto communication = ovf::app::CreateRuntime("ovf-radar-service");
  if (!communication) {
    std::cerr << "failed to start the radar service runtime\n";
    return 2;
  }
  auto logging = ovf::app::CreateLogRuntime();
  if (!logging) {
    std::cerr << "failed to start radar service logging runtime\n";
    return 3;
  }
  auto logger = logging->CreateLogger("radar.service");

  RadarService implementation;
  auto service = implementation.OfferService(communication.get(), ovf::app::radar());
  if (!service.valid()) {
    std::cerr << "failed to offer RadarService\n";
    logger.Error("failed to offer RadarService");
    return 4;
  }
  auto ready = execution.value().ReportReady();
  if (!ready) {
    std::cerr << "failed to report radar readiness\n";
    service.close();
    logger.Error("failed to report radar readiness");
    return 5;
  }

  logger.Info("radar service ready");
  std::cout << "SERVICE_READY" << std::endl;
  std::uint64_t sequence{};
  while (!execution.value().StopRequested()) {
    RadarFrame frame{};
    frame.capturedAt = ++sequence;
    if (!frame.objects.push_back({7, 12.5F, -1.5F, 98}) ||
        service.publishRadarObjectsChanged(frame)) {
      std::cerr << "failed to publish RadarObjectsChanged\n";
      service.close();
      logger.Error("failed to publish radar objects",
                                     ovf::log::Field::Unsigned("sequence", sequence));
      return 6;
    }
    implementation.set_speed(13.5F + static_cast<float>(sequence));
    if (service.publishVehicleStateField(implementation.state())) {
      std::cerr << "failed to publish VehicleStateField\n";
      service.close();
      logger.Error("failed to publish vehicle state",
                                     ovf::log::Field::Unsigned("sequence", sequence));
      return 7;
    }
    logger.Debug("radar scan published",
                                   ovf::log::Field::Unsigned("sequence", sequence),
                                   ovf::log::Field::Unsigned("objects", frame.objects.size()));
    std::this_thread::sleep_for(100ms);
  }

  service.close();
  return 0;
}
