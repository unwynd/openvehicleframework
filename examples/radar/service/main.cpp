// SPDX-License-Identifier: Apache-2.0

#include "ovf/app/run.hpp"
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
using namespace ovf::log::literals;

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

  auto setVehicleStateField(VehicleState const& value) -> std::optional<ovf::com::Error> override {
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
  return ovf::app::Run("ovf-radar-service", "radar.service", [](ovf::app::Context& ctx) {
    RadarService implementation;
    auto service = implementation.OfferService(ctx.com(), ovf::app::radar());
    if (!service.valid()) {
      std::cerr << "failed to offer RadarService\n";
      ctx.logger().Error("failed to offer RadarService");
      return ovf::app::ExitCode::service_offer_failed;
    }
    if (auto ready = ctx.ReportReady(); ready != ovf::app::ExitCode::ok) {
      return ready;
    }
    ctx.logger().Info("radar service ready");
    std::cout << "SERVICE_READY" << std::endl;
    std::uint64_t sequence{};
    return ctx.Tick(100ms, [&](ovf::log::Logger& log) {
      RadarFrame frame{};
      frame.capturedAt = ++sequence;
      if (!frame.objects.push_back({7, 12.5F, -1.5F, 98}) ||
          service.publishRadarObjectsChanged(frame)) {
        std::cerr << "failed to publish RadarObjectsChanged\n";
        log.Error("failed to publish radar objects", "sequence"_field = sequence);
        return ovf::app::TickAction::stop;
      }
      implementation.set_speed(13.5F + static_cast<float>(sequence));
      if (service.publishVehicleStateField(implementation.state())) {
        std::cerr << "failed to publish VehicleStateField\n";
        log.Error("failed to publish vehicle state", "sequence"_field = sequence);
        return ovf::app::TickAction::stop;
      }
      log.Debug("radar scan published", "sequence"_field = sequence,
                "objects"_field = frame.objects.size());
      return ovf::app::TickAction::continue_loop;
    });
  });
}
