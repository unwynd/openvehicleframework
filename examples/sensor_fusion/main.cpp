// SPDX-License-Identifier: Apache-2.0

#include "camera/ovf_contract.hpp"
#include "camera/ovf_deployment.hpp"
#include "environment_model/ovf_contract.hpp"
#include "environment_model/ovf_deployment.hpp"
#include "radar/ovf_contract.hpp"
#include "radar/ovf_deployment.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>

namespace {
using namespace std::chrono_literals;

std::atomic_bool running{true};
void Stop(int) { running.store(false); }

auto WaitForRoute(ovf::com::DiscoveryWatch& discovery, std::condition_variable& condition,
                  std::mutex& mutex) -> std::optional<ovf::com::ServiceRoute> {
  std::optional<ovf::com::ServiceRoute> route;
  std::unique_lock lock(mutex);
  condition.wait_for(lock, 10s, [&] {
    route = discovery.select();
    return route.has_value();
  });
  return route;
}

class EnvironmentImplementation final
    : public example::environment::EnvironmentModelServiceSkeleton {};
} // namespace

int main() {
  std::signal(SIGINT, Stop);
  std::signal(SIGTERM, Stop);
  ovf::com::Runtime runtime({.instance_name = "ovf-sensor-fusion", .logger = {}, .dispatcher = {}});
  if (camera_deployment::Configure(runtime) != ovf::com::RuntimeError::none ||
      environment_model_deployment::Configure(runtime) != ovf::com::RuntimeError::none ||
      runtime.Start() != ovf::com::RuntimeError::none) {
    std::cerr << "failed to start sensor fusion runtime\n";
    return 1;
  }

  EnvironmentImplementation implementation;
  auto offer_binding = ovf::com::Offer(runtime, environment_model_deployment::Route());
  example::environment::EnvironmentModelServiceOffer output(std::move(offer_binding),
                                                            implementation);
  if (!output.valid()) {
    std::cerr << "failed to offer EnvironmentModelService\n";
    return 2;
  }

  std::mutex mutex;
  std::condition_variable condition;
  auto camera_discovery = ovf::com::Discover(runtime, {camera_deployment::Route()});
  auto radar_discovery = ovf::com::Discover(runtime, {radar_deployment::Route()});
  if (!camera_discovery || !radar_discovery) {
    std::cerr << "failed to start input discovery\n";
    return 3;
  }
  auto notify = [&](std::span<ovf::com::ServiceRoute const> routes) {
    if (!routes.empty()) {
      condition.notify_all();
    }
  };
  camera_discovery->on_change(notify);
  radar_discovery->on_change(notify);
  auto camera_route = WaitForRoute(*camera_discovery, condition, mutex);
  auto radar_route = WaitForRoute(*radar_discovery, condition, mutex);
  if (!camera_route || !radar_route) {
    std::cerr << "timed out discovering sensor inputs\n";
    return 4;
  }

  auto camera_binding = ovf::com::Connect(runtime, *camera_route);
  auto radar_binding = ovf::com::Connect(runtime, *radar_route);
  if (!camera_binding || !radar_binding) {
    std::cerr << "failed to connect to sensor inputs\n";
    return 5;
  }
  example::camera::CameraServiceProxy camera(std::move(camera_binding));
  example::radar::RadarServiceProxy radar(std::move(radar_binding));
  auto camera_subscription = camera.subscribeCameraObjectsChanged();
  auto radar_subscription = radar.subscribeRadarObjectsChanged();
  if (!camera_subscription.valid() || !radar_subscription.valid()) {
    std::cerr << "failed to subscribe to sensor inputs\n";
    return 6;
  }

  example::camera::CameraFrame latest_camera{};
  example::radar::RadarFrame latest_radar{};
  bool have_camera{};
  bool have_radar{};
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
  camera_subscription.on_sample([&](example::camera::CameraFrame const& frame) {
    std::lock_guard lock(mutex);
    latest_camera = frame;
    have_camera = true;
    if (!publish()) {
      running.store(false);
    }
  });
  radar_subscription.on_sample([&](example::radar::RadarFrame const& frame) {
    std::lock_guard lock(mutex);
    latest_radar = frame;
    have_radar = true;
    if (!publish()) {
      running.store(false);
    }
  });

  std::cout << "SENSOR_FUSION_READY" << std::endl;
  while (running.load()) {
    std::this_thread::sleep_for(100ms);
  }
  radar_subscription.close();
  camera_subscription.close();
  radar_discovery->close();
  camera_discovery->close();
  output.close();
  runtime.Stop();
  return 0;
}
