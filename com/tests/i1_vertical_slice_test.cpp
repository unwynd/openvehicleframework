// SPDX-License-Identifier: Apache-2.0

#include "radar.hpp"

#include "ovf/com/provider_binding.hpp"
#include "ovf/com/runtime.hpp"
#include "ovf/com/transports/inproc.h"

#include <cassert>
#include <chrono>
#include <cmath>

namespace {
using namespace example::radar;
using namespace std::chrono_literals;

class RadarImplementation final : public RadarServiceSkeleton {
public:
  auto Calibrate(CalibrateInput const& request)
      -> ovf::com::MethodResult<CalibrateOutput, std::variant<InvalidTarget>> override {
    ++calibrations;
    if (request.targetDistanceMeters < 0.0F) {
      InvalidTarget error{};
      assert(error.reason.assign("target distance must be non-negative"));
      return std::variant<InvalidTarget>{std::move(error)};
    }
    return CalibrateOutput{123456789U};
  }

  auto getVehicleStateField() -> ovf::com::MethodResult<VehicleState, std::monostate> override {
    return state;
  }

  VehicleState state{13.5F};
  int calibrations{};
};

auto Route(std::uint8_t suffix, std::int32_t priority) -> ovf::com::RouteBinding {
  auto route = ovf::com::RouteBinding{
      RadarServiceContract::id,
      {{0xc1, 0x81, 0x3c, 0x19, 0x1f, 0x06, 0x49, 0x14, 0xbb, 0x0d, 0xc6, 0x08, 0x0f, 0x1a, 0x84,
        suffix}},
      1,
      65536,
      8,
      {},
      "inproc",
      "radar",
      priority,
  };
  return route;
}
} // namespace

int main() {
  ovf::com::Runtime runtime({.instance_name = "i5", .logger = {}, .dispatcher = {}});
  auto const* factory = ovf_com_inproc_transport_query_v1();
  assert(
      runtime.AddTransport(
          *factory, {.configuration = "", .max_endpoints = 32, .max_outstanding_operations = 16}) ==
      ovf::com::RuntimeError::none);
  assert(runtime.Start() == ovf::com::RuntimeError::none);

  auto fallback_route = Route(0x6a, 10);
  auto preferred_route = Route(0x6b, 1);
  auto discovery = ovf::com::Discover(runtime, {fallback_route, preferred_route});
  assert(discovery && discovery->routes().empty());
  int discovery_changes{};
  discovery->on_change([&](std::span<ovf::com::ServiceRoute const>) { ++discovery_changes; });

  RadarImplementation fallback_implementation;
  auto fallback_binding = ovf::com::Offer(runtime, fallback_route);
  assert(fallback_binding);
  RadarServiceOffer fallback_offer(fallback_binding, fallback_implementation);
  assert(fallback_offer.valid());
  RadarImplementation implementation;
  auto server_binding = ovf::com::Offer(runtime, preferred_route);
  assert(server_binding);
  RadarServiceOffer offer(server_binding, implementation);
  assert(offer.valid());
  assert(discovery_changes >= 2);
  auto selected = discovery->select();
  assert(selected && selected->provider() == "inproc");
  assert(selected->instance_id().bytes.back() == 0x6b);
  assert(selected->route_epoch() == 1);

  auto client_binding = ovf::com::Connect(runtime, *selected);
  assert(client_binding);
  RadarServiceProxy proxy(client_binding);

  int radar_samples{};
  RadarFrame received{};
  auto radar_subscription = proxy.subscribeRadarObjectsChanged();
  radar_subscription.on_sample([&](RadarFrame const& frame) {
    received = frame;
    ++radar_samples;
  });
  RadarFrame frame{};
  frame.capturedAt = 42;
  assert(frame.objects.push_back({7, 12.5F, -1.5F, 98}));
  assert(!offer.publishRadarObjectsChanged(frame));
  assert(radar_samples == 1 && received.capturedAt == 42);
  assert(received.objects.size() == 1 && received.objects.values()[0].id == 7);

  auto deadline = ovf::com::CallOptions{std::chrono::steady_clock::now() + 1s};
  auto operation = proxy.Calibrate({2.0F}, deadline);
  auto result = operation.get(deadline);
  assert(std::holds_alternative<CalibrateOutput>(result));
  assert(std::get<CalibrateOutput>(result).acceptedAt == 123456789U);

  auto invalid_operation = proxy.Calibrate({-1.0F}, deadline);
  auto invalid = invalid_operation.get(deadline);
  assert(invalid.index() == 1);
  auto const& app_error = std::get<1>(invalid);
  assert(std::get<InvalidTarget>(app_error).reason.view() ==
         "target distance must be non-negative");

  auto field_operation = proxy.getVehicleStateField(deadline);
  auto field_value = field_operation.get(deadline);
  assert(std::holds_alternative<VehicleState>(field_value));
  assert(std::fabs(std::get<VehicleState>(field_value).speedMetersPerSecond - 13.5F) < 0.001F);

  int field_updates{};
  VehicleState notified{};
  auto field_subscription = proxy.subscribeVehicleStateField();
  field_subscription.on_sample([&](VehicleState const& value) {
    notified = value;
    ++field_updates;
  });
  implementation.state.speedMetersPerSecond = 21.0F;
  assert(!offer.publishVehicleStateField(implementation.state));
  assert(field_updates == 1 && notified.speedMetersPerSecond == 21.0F);

  auto expired = ovf::com::CallOptions{std::chrono::steady_clock::now() - 1ms};
  auto expired_operation = proxy.Calibrate({2.0F}, expired);
  auto expired_result = expired_operation.get(expired);
  assert(std::get<ovf::com::CommunicationError>(expired_result) ==
         ovf::com::CommunicationError::deadline_exceeded);

  offer.close();
  assert(discovery->routes().size() == 1);
  assert(discovery->select()->instance_id().bytes.back() == 0x6a);
  auto unavailable_operation = proxy.Calibrate({2.0F}, deadline);
  auto unavailable = unavailable_operation.get(deadline);
  assert(std::get<ovf::com::CommunicationError>(unavailable) ==
         ovf::com::CommunicationError::unavailable);
  fallback_offer.close();
  assert(discovery->routes().empty());

  field_subscription.close();
  radar_subscription.close();
  discovery->close();
  runtime.Stop();
}
