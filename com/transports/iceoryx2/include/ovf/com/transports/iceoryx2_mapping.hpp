// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ovf::com::transports::iceoryx2 {

struct Mapping final {
  std::string service;
  std::string type_name;
  std::size_t payload_size{};
  std::size_t payload_alignment{};
  std::size_t history_depth{};
  std::size_t subscriber_buffer{};
  std::size_t max_publishers{};
  std::size_t max_subscribers{};
  bool safe_overflow{};
};

// Canonical deployment form:
// service=vehicle/radar/front/objects;type=RadarObjects;payloadSize=4096;
// alignment=8;history=1;subscriberBuffer=8;maxPublishers=1;
// maxSubscribers=8;safeOverflow=false
[[nodiscard]] auto ParseMapping(std::string_view text, Mapping& out, std::string& error) -> bool;
[[nodiscard]] auto FormatMapping(Mapping const& mapping) -> std::string;

} // namespace ovf::com::transports::iceoryx2
