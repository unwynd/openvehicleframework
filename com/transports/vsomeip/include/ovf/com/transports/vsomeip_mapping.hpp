// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ovf::com::transports::vsomeip {

enum class ElementKind : std::uint8_t { event, method, field_get, field_set, field_notify };

struct Mapping final {
  std::uint16_t service{};
  std::uint16_t instance{};
  std::uint16_t element{};
  std::uint16_t event_group{};
  std::uint8_t major_version{};
  std::uint32_t minor_version{};
  ElementKind kind{};
  bool reliable{};
};

// Parses the canonical deployment form:
// service=4660;instance=1;element=32769;eventGroup=1;major=1;minor=0;
// kind=event;reliable=true
[[nodiscard]] auto ParseMapping(std::string_view text, Mapping& out, std::string& error) -> bool;
[[nodiscard]] auto FormatMapping(Mapping const& mapping) -> std::string;

} // namespace ovf::com::transports::vsomeip
