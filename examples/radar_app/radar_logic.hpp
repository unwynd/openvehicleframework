// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "radar_app.ovf.hpp"

namespace radar_app {

[[nodiscard]] inline auto EmptyFrame() -> example::radar::RadarFrame { return {}; }

} // namespace radar_app
