// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/application.hpp"
#include "ovf_application.hpp"
#include "ovf_logging.hpp"
#include "radar/ovf_contract.hpp"

#include <chrono>
#include <iostream>
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
        return ovf::com::CommunicationError::provider_failure;
      }
      return std::variant<InvalidTarget>{std::move(error)};
    }
    return CalibrateOutput{42};
  }

  auto getVehicleStateField() -> ovf::com::MethodResult<VehicleState, std::monostate> override {
    return state_;
  }

  auto set_speed(float speed) -> void { state_.speedMetersPerSecond = speed; }

  [[nodiscard]] auto state() const -> VehicleState const& { return state_; }

private:
  VehicleState state_{13.5F};
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
    static_cast<void>(logger.Error("failed to offer RadarService"));
    return 4;
  }
  auto ready = execution.value().ReportReady();
  if (!ready) {
    std::cerr << "failed to report radar readiness\n";
    service.close();
    static_cast<void>(logger.Error("failed to report radar readiness"));
    return 5;
  }

  static_cast<void>(logger.Info("radar service ready"));
  std::cout << "SERVICE_READY" << std::endl;
  std::uint64_t sequence{};
  while (!execution.value().StopRequested()) {
    RadarFrame frame{};
    frame.capturedAt = ++sequence;
    if (!frame.objects.push_back({7, 12.5F, -1.5F, 98}) ||
        service.publishRadarObjectsChanged(frame)) {
      std::cerr << "failed to publish RadarObjectsChanged\n";
      service.close();
      static_cast<void>(logger.Error("failed to publish radar objects",
                                     ovf::log::Field::Unsigned("sequence", sequence)));
      return 6;
    }
    implementation.set_speed(13.5F + static_cast<float>(sequence));
    if (service.publishVehicleStateField(implementation.state())) {
      std::cerr << "failed to publish VehicleStateField\n";
      service.close();
      static_cast<void>(logger.Error("failed to publish vehicle state",
                                     ovf::log::Field::Unsigned("sequence", sequence)));
      return 7;
    }
    static_cast<void>(logger.Debug("radar scan published",
                                   ovf::log::Field::Unsigned("sequence", sequence),
                                   ovf::log::Field::Unsigned("objects", frame.objects.size())));
    std::this_thread::sleep_for(100ms);
  }

  service.close();
  return 0;
}
