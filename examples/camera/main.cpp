// SPDX-License-Identifier: Apache-2.0

#include "camera/ovf_contract.hpp"
#include "ovf/exec/application.hpp"
#include "ovf_application.hpp"

#include <chrono>
#include <iostream>
#include <thread>

namespace {
using namespace example::camera;
using namespace std::chrono_literals;

class CameraImplementation final : public CameraServiceSkeleton {};
} // namespace

int main() {
  auto execution = ovf::exec::Application::Create();
  if (!execution) {
    std::cerr << "failed to create camera execution context\n";
    return 1;
  }
  auto communication = ovf::app::CreateRuntime("ovf-camera");
  if (!communication) {
    std::cerr << "failed to start camera runtime\n";
    return 2;
  }
  CameraImplementation implementation;
  auto service = implementation.OfferService(communication.get(), ovf::app::camera());
  if (!service.valid()) {
    std::cerr << "failed to offer CameraService\n";
    return 3;
  }
  auto ready = execution.value().ReportReady();
  if (!ready) {
    std::cerr << "failed to report camera readiness\n";
    service.close();
    return 4;
  }
  std::cout << "CAMERA_READY" << std::endl;
  std::uint64_t sequence{};
  while (!execution.value().StopRequested()) {
    CameraFrame frame{};
    frame.capturedAt = ++sequence;
    CameraObject object{11, {}, 12.3F, -1.4F, 96};
    if (!object.classification.assign("vehicle") || !frame.objects.push_back(object) ||
        service.publishCameraObjectsChanged(frame)) {
      std::cerr << "failed to publish camera frame\n";
      service.close();
      return 5;
    }
    std::this_thread::sleep_for(100ms);
  }
  service.close();
  return 0;
}
