// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ovf::com::transports::iceoryx2 {

struct Mapping final {
  enum class Pattern { kPublishSubscribe, kRequestResponse };

  Pattern pattern{Pattern::kPublishSubscribe};
  std::string service;
  std::string type_name;
  std::size_t payload_size{};
  std::size_t payload_alignment{};
  std::size_t history_depth{};
  std::size_t subscriber_buffer{};
  std::size_t max_publishers{};
  std::size_t max_subscribers{};
  std::size_t max_loaned_samples{};
  std::size_t max_borrowed_samples{};
  bool safe_overflow{};
  std::string request_type;
  std::string response_type;
  std::size_t request_payload_size{};
  std::size_t response_payload_size{};
  std::size_t request_buffer{};
  std::size_t response_buffer{};
  std::size_t max_clients{};
  std::size_t max_servers{};
  std::size_t max_loaned_requests{};
  std::size_t max_borrowed_responses{};
  std::size_t max_loaned_responses{};
};

// Canonical deployment form:
// pattern=pubsub;service=vehicle/radar/front/objects;type=RadarObjects;payloadSize=4096;
// alignment=8;history=1;subscriberBuffer=8;maxPublishers=1;
// maxSubscribers=8;safeOverflow=false
[[nodiscard]] auto ParseMapping(std::string_view text, Mapping& out, std::string& error) -> bool;
[[nodiscard]] auto FormatMapping(Mapping const& mapping) -> std::string;

} // namespace ovf::com::transports::iceoryx2
