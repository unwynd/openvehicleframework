// SPDX-License-Identifier: Apache-2.0

#include "ovf_application.hpp"
#include "radar/ovf_contract.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
using namespace example::radar;
using namespace std::chrono_literals;

std::atomic_bool running{true};

void Stop(int) { running.store(false); }

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
  std::signal(SIGINT, Stop);
  std::signal(SIGTERM, Stop);

  auto application = ovf::app::CreateRuntime("ovf-radar-service");
  if (!application) {
    std::cerr << "failed to start the radar service runtime\n";
    return 1;
  }

  RadarService implementation;
  auto service = implementation.OfferService(application.get(), ovf::app::radar());
  if (!service.valid()) {
    std::cerr << "failed to offer RadarService\n";
    return 2;
  }

  std::cout << "SERVICE_READY" << std::endl;
  std::uint64_t sequence{};
  while (running.load()) {
    RadarFrame frame{};
    frame.capturedAt = ++sequence;
    if (!frame.objects.push_back({7, 12.5F, -1.5F, 98}) ||
        service.publishRadarObjectsChanged(frame)) {
      std::cerr << "failed to publish RadarObjectsChanged\n";
      return 3;
    }
    implementation.set_speed(13.5F + static_cast<float>(sequence));
    if (service.publishVehicleStateField(implementation.state())) {
      std::cerr << "failed to publish VehicleStateField\n";
      return 4;
    }
    std::this_thread::sleep_for(100ms);
  }

  service.close();
  return 0;
}
