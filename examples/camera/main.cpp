// SPDX-License-Identifier: Apache-2.0

#include "camera/ovf_contract.hpp"
#include "ovf_application.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
using namespace example::camera;
using namespace std::chrono_literals;

std::atomic_bool running{true};
void Stop(int) { running.store(false); }

class CameraImplementation final : public CameraServiceSkeleton {};
} // namespace

int main() {
  std::signal(SIGINT, Stop);
  std::signal(SIGTERM, Stop);
  auto application = ovf::app::CreateRuntime("ovf-camera");
  if (!application) {
    std::cerr << "failed to start camera runtime\n";
    return 1;
  }
  CameraImplementation implementation;
  auto service = implementation.OfferService(application.get(), ovf::app::camera());
  if (!service.valid()) {
    std::cerr << "failed to offer CameraService\n";
    return 2;
  }
  std::cout << "CAMERA_READY" << std::endl;
  std::uint64_t sequence{};
  while (running.load()) {
    CameraFrame frame{};
    frame.capturedAt = ++sequence;
    CameraObject object{11, {}, 12.3F, -1.4F, 96};
    if (!object.classification.assign("vehicle") || !frame.objects.push_back(object) ||
        service.publishCameraObjectsChanged(frame)) {
      std::cerr << "failed to publish camera frame\n";
      return 3;
    }
    std::this_thread::sleep_for(100ms);
  }
  service.close();
  return 0;
}
