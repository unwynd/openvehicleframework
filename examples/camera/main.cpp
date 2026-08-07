// SPDX-License-Identifier: Apache-2.0

#include "camera/ovf_contract.hpp"
#include "ovf/app/run.hpp"
#include "ovf_application.hpp"
#include "ovf_logging.hpp"

#include <chrono>
#include <iostream>

namespace {
using namespace example::camera;
using namespace std::chrono_literals;
using namespace ovf::log::literals;

class CameraImplementation final : public CameraServiceSkeleton {};
} // namespace

int main() {
  return ovf::app::Run("ovf-camera", "camera.capture", [](ovf::app::Context& ctx) {
    CameraImplementation implementation;
    auto service = implementation.OfferService(ctx.com(), ovf::app::camera());
    if (!service.valid()) {
      std::cerr << "failed to offer CameraService\n";
      return ovf::app::ExitCode::service_offer_failed;
    }
    if (auto ready = ctx.ReportReady(); ready != ovf::app::ExitCode::ok) {
      return ready;
    }
    ctx.logger().Info("camera service ready");
    std::cout << "CAMERA_READY" << std::endl;
    std::uint64_t sequence{};
    return ctx.Tick(100ms, [&](ovf::log::Logger& log) {
      CameraFrame frame{};
      frame.capturedAt = ++sequence;
      CameraObject object{11, {}, 12.3F, -1.4F, 96};
      if (!object.classification.assign("vehicle") || !frame.objects.push_back(object) ||
          service.publishCameraObjectsChanged(frame)) {
        std::cerr << "failed to publish camera frame\n";
        log.Error("failed to publish camera frame", "sequence"_field = sequence);
        return ovf::app::TickAction::stop;
      }
      static_cast<void>(log.Event(ovf::app::kFramePublished, "sequence"_field = sequence,
                                  "objects"_field = frame.objects.size()));
      return ovf::app::TickAction::continue_loop;
    });
  });
}
