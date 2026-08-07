// SPDX-License-Identifier: Apache-2.0

#include "camera/ovf_contract.hpp"
#include "environment_model/ovf_contract.hpp"
#include "ovf/app/run.hpp"
#include "ovf_application.hpp"
#include "ovf_logging.hpp"
#include "radar/ovf_contract.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>

namespace {
using namespace std::chrono_literals;

class EnvironmentImplementation final
    : public example::environment::EnvironmentModelServiceSkeleton {};
} // namespace

int main() {
  return ovf::app::Run("ovf-sensor-fusion", "sensor_fusion.processing", [](ovf::app::Context& ctx) {
    auto& logger = ctx.logger();
    EnvironmentImplementation implementation;
    auto output = implementation.OfferService(ctx.com(), ovf::app::environment_model());
    if (!output.valid()) {
      std::cerr << "failed to offer EnvironmentModelService\n";
      logger.Error("failed to offer EnvironmentModelService");
      return ovf::app::ExitCode::service_offer_failed;
    }

    auto camera = example::camera::CameraServiceProxy::Find(ctx.com(), ovf::app::camera(), 10s);
    auto radar = example::radar::RadarServiceProxy::Find(ctx.com(), ovf::app::radar(), 10s);
    if (!camera || !radar) {
      std::cerr << "timed out discovering sensor inputs\n";
      logger.Warning("timed out discovering sensor inputs");
      return ovf::app::ExitCode::discovery_timeout;
    }
    auto camera_subscription = camera->subscribeCameraObjectsChanged();
    auto radar_subscription = radar->subscribeRadarObjectsChanged();
    if (!camera_subscription.valid() || !radar_subscription.valid()) {
      std::cerr << "failed to subscribe to sensor inputs\n";
      logger.Error("failed to subscribe to sensor inputs");
      return ovf::app::ExitCode::subscription_failed;
    }

    std::mutex mutex;
    example::camera::CameraFrame latest_camera{};
    example::radar::RadarFrame latest_radar{};
    bool have_camera{};
    bool have_radar{};
    std::atomic_bool communication_failed{};
    std::uint64_t output_sequence{};
    auto publish = [&] {
      if (!have_camera || !have_radar) {
        return true;
      }
      example::environment::EnvironmentModel model{};
      model.producedAt = ++output_sequence;
      for (auto const& source : latest_camera.objects.values()) {
        example::environment::FusedObject object{
            source.id, {},  source.longitudinalMeters, source.lateralMeters, source.confidence,
            false,     true};
        if (!object.classification.assign(source.classification.view()) ||
            !model.objects.push_back(object)) {
          return false;
        }
      }
      for (auto const& source : latest_radar.objects.values()) {
        example::environment::FusedObject object{
            source.id, {},   source.longitudinalMeters, source.lateralMeters, source.confidence,
            true,      false};
        if (!object.classification.assign("unclassified") || !model.objects.push_back(object)) {
          return false;
        }
      }
      return !output.publishEnvironmentModelChanged(model).has_value();
    };
    camera_subscription.OnSample([&](example::camera::CameraFrame const& frame) {
      std::lock_guard lock(mutex);
      latest_camera = frame;
      have_camera = true;
      if (!publish()) {
        communication_failed.store(true);
      }
    });
    radar_subscription.OnSample([&](example::radar::RadarFrame const& frame) {
      std::lock_guard lock(mutex);
      latest_radar = frame;
      have_radar = true;
      if (!publish()) {
        communication_failed.store(true);
      }
    });

    if (auto ready = ctx.ReportReady(); ready != ovf::app::ExitCode::ok) {
      return ready;
    }
    logger.Info("sensor fusion ready");
    std::cout << "SENSOR_FUSION_READY" << std::endl;
    return ctx.Tick(100ms, [&](ovf::log::Logger& log) {
      if (communication_failed.load()) {
        log.Error("failed to publish fused environment model");
        return ovf::app::TickAction::stop;
      }
      return ovf::app::TickAction::continue_loop;
    });
  });
}
