// SPDX-License-Identifier: Apache-2.0

// C-6: verify EventSubscription::close is a quiescence boundary.
//
// The test constructs an in-process runtime, offers a service, subscribes
// to an event, and interleaves publish() calls with close(). After close()
// returns, no further sample callback must fire.

#include "ovf/com/provider_binding.hpp"
#include "ovf/com/runtime.hpp"
#include "radar.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

extern "C" const ovf_com_transport_factory_v1* ovf_com_inproc_transport_query_v1(void);

namespace {

using namespace example::radar;
using namespace std::chrono_literals;

class RadarService final : public RadarServiceSkeleton {
public:
  auto Calibrate(CalibrateInput const&)
      -> ovf::com::MethodResult<CalibrateOutput, std::variant<InvalidTarget>> override {
    return CalibrateOutput{};
  }
  auto getVehicleStateField() -> ovf::com::MethodResult<VehicleState, std::monostate> override {
    return VehicleState{};
  }
  auto setVehicleStateField(VehicleState const&)
      -> std::optional<ovf::com::Error> override {
    return std::nullopt;
  }
  auto Delay(DelayInput const&)
      -> ovf::com::MethodResult<DelayOutput, std::monostate> override {
    return DelayOutput{};
  }
};

TEST(SubscriptionClose, QuiescesInFlightCallbacks) {
  ovf::com::Runtime runtime({.instance_name = "test", .logger = {}, .dispatcher = {}});
  ASSERT_EQ(runtime.AddTransport(*ovf_com_inproc_transport_query_v1(),
                                 {.configuration = "", .max_endpoints = 32,
                                  .max_outstanding_operations = 16}),
            ovf::com::Error::none);
  ASSERT_EQ(runtime.Start(), ovf::com::Error::none);

  ovf::com::RouteBinding route{
      RadarServiceContract::id,
      ovf::com::Uuid{{0xc1, 0x81, 0x3c, 0x19, 0x1f, 0x06, 0x49, 0x14, 0xbb, 0x0d, 0xc6, 0x08,
                      0x0f, 0x1a, 0x84, 0x77}},
      1, 65536, 8, {}, "inproc", "radar", 0};

  RadarService service;
  auto server_binding = ovf::com::Offer(runtime, route);
  ASSERT_TRUE(server_binding);
  RadarServiceOffer offer(server_binding, service);
  ASSERT_TRUE(offer.valid());

  auto client_binding = ovf::com::FindService(runtime, route, 1s);
  ASSERT_TRUE(client_binding);
  RadarServiceProxy proxy(client_binding);

  std::atomic<std::uint64_t> received{};
  std::atomic<bool> ran_after_close{false};
  std::atomic<bool> subscription_closed{false};
  auto subscription = proxy.subscribeRadarObjectsChanged();
  subscription.OnSample([&](RadarFrame const&) {
    if (subscription_closed.load(std::memory_order_acquire)) {
      ran_after_close.store(true, std::memory_order_release);
    }
    received.fetch_add(1, std::memory_order_release);
  });

  // Publish a burst to exercise the callback dispatch path.
  RadarFrame frame{};
  frame.capturedAt = 1;
  ASSERT_TRUE(frame.objects.push_back({1, 1.0F, 0.5F, 90}));
  for (int i = 0; i < 100; ++i) {
    (void)offer.publishRadarObjectsChanged(frame);
  }
  std::this_thread::sleep_for(20ms);

  subscription.close();
  subscription_closed.store(true, std::memory_order_release);

  // Any publishes after close() must not deliver samples.
  for (int i = 0; i < 200; ++i) {
    (void)offer.publishRadarObjectsChanged(frame);
  }
  std::this_thread::sleep_for(20ms);

  EXPECT_FALSE(ran_after_close.load()) << "callback ran after close() returned";

  offer.close();
  runtime.Stop();
}

} // namespace
