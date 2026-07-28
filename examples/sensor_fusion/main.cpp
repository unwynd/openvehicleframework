// SPDX-License-Identifier: Apache-2.0

#include "camera/ovf_contract.hpp"
#include "environment_model/ovf_contract.hpp"
#include "ovf_application.hpp"
#include "radar/ovf_contract.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
using namespace std::chrono_literals;

std::atomic_bool running{true};
void Stop(int) { running.store(false); }

class EnvironmentImplementation final
    : public example::environment::EnvironmentModelServiceSkeleton {};
} // namespace

int main() {
  std::signal(SIGINT, Stop);
  std::signal(SIGTERM, Stop);
  auto application = ovf::app::CreateRuntime("ovf-sensor-fusion");
  if (!application) {
    std::cerr << "failed to start sensor fusion runtime\n";
    return 1;
  }

  EnvironmentImplementation implementation;
  auto output = implementation.OfferService(application.get(), ovf::app::environment_model());
  if (!output.valid()) {
    std::cerr << "failed to offer EnvironmentModelService\n";
    return 2;
  }

  std::mutex mutex;
  std::condition_variable condition;
  auto camera =
      example::camera::CameraServiceProxy::Find(application.get(), ovf::app::camera(), 10s);
  auto radar = example::radar::RadarServiceProxy::Find(application.get(), ovf::app::radar(), 10s);
  if (!camera || !radar) {
    std::cerr << "timed out discovering sensor inputs\n";
    return 3;
  }
  auto camera_subscription = camera->subscribeCameraObjectsChanged();
  auto radar_subscription = radar->subscribeRadarObjectsChanged();
  if (!camera_subscription.valid() || !radar_subscription.valid()) {
    std::cerr << "failed to subscribe to sensor inputs\n";
    return 4;
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
  output.close();
  return 0;
}
