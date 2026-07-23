// SPDX-License-Identifier: Apache-2.0

#include "radar.hpp"

#include <type_traits>

#ifndef OVF_DEPLOYMENT_FILE
#error OVF_DEPLOYMENT_FILE must identify the separately validated deployment
#endif

namespace {
using namespace example::radar;

class RadarImplementation final : public RadarServiceSkeleton {
public:
  auto Calibrate(CalibrateInput const&)
      -> ovf::com::MethodResult<CalibrateOutput, std::variant<InvalidTarget>> override {
    return CalibrateOutput{};
  }
  auto getVehicleStateField() -> ovf::com::MethodResult<VehicleState, std::monostate> override {
    return VehicleState{};
  }
};

static_assert(std::is_same_v<TimestampNs, std::uint64_t>);
static_assert(RadarObjects::capacity() == 64);
static_assert(DiagnosticText::capacity() == 128);
static_assert(RadarServiceContract::CalibrateDescriptor.tag == 2);
static_assert(RadarServiceContract::id.bytes[0] == 0x42);

[[maybe_unused]] auto deployment_file() -> const char* { return OVF_DEPLOYMENT_FILE; }
[[maybe_unused]] RadarImplementation implementation;
} // namespace
