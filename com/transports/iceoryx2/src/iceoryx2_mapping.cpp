// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/transports/iceoryx2_mapping.hpp"

#include <charconv>
#include <limits>

namespace ovf::com::transports::iceoryx2 {
namespace {
auto Number(std::string_view value, std::size_t& out) -> bool {
  if (value.empty())
    return false;
  std::uint64_t parsed{};
  auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
      parsed > std::numeric_limits<std::size_t>::max())
    return false;
  out = static_cast<std::size_t>(parsed);
  return true;
}
auto PowerOfTwo(std::size_t value) -> bool { return value != 0 && (value & (value - 1)) == 0; }
} // namespace

auto ParseMapping(std::string_view text, Mapping& out, std::string& error) -> bool {
  Mapping value{};
  std::uint32_t seen{};
  while (!text.empty()) {
    auto separator = text.find(';');
    auto item = text.substr(0, separator);
    text = separator == std::string_view::npos ? std::string_view{} : text.substr(separator + 1);
    auto equal = item.find('=');
    if (equal == std::string_view::npos || equal == 0 || equal + 1 == item.size()) {
      error = "mapping entries must be key=value";
      return false;
    }
    auto key = item.substr(0, equal);
    auto item_value = item.substr(equal + 1);
    std::uint32_t bit{};
    bool valid = true;
    if (key == "pattern") {
      bit = 1U << 0;
      if (item_value == "pubsub")
        value.pattern = Mapping::Pattern::kPublishSubscribe;
      else if (item_value == "requestResponse")
        value.pattern = Mapping::Pattern::kRequestResponse;
      else
        valid = false;
    } else if (key == "service") {
      bit = 1U << 1;
      value.service = item_value;
      valid = value.service.size() <= 255;
    } else if (key == "type") {
      bit = 1U << 2;
      value.type_name = item_value;
      valid = value.type_name.size() <= 255;
    } else if (key == "payloadSize") {
      bit = 1U << 3;
      valid = Number(item_value, value.payload_size) && value.payload_size != 0;
    } else if (key == "alignment") {
      bit = 1U << 4;
      valid = Number(item_value, value.payload_alignment) && PowerOfTwo(value.payload_alignment);
    } else if (key == "history") {
      bit = 1U << 5;
      valid = Number(item_value, value.history_depth);
    } else if (key == "subscriberBuffer") {
      bit = 1U << 6;
      valid = Number(item_value, value.subscriber_buffer) && value.subscriber_buffer != 0;
    } else if (key == "maxPublishers") {
      bit = 1U << 7;
      valid = Number(item_value, value.max_publishers) && value.max_publishers != 0;
    } else if (key == "maxSubscribers") {
      bit = 1U << 8;
      valid = Number(item_value, value.max_subscribers) && value.max_subscribers != 0;
    } else if (key == "safeOverflow") {
      bit = 1U << 9;
      if (item_value == "true")
        value.safe_overflow = true;
      else if (item_value == "false")
        value.safe_overflow = false;
      else
        valid = false;
    } else if (key == "requestType") {
      bit = 1U << 10;
      value.request_type = item_value;
      valid = !value.request_type.empty() && value.request_type.size() <= 255;
    } else if (key == "responseType") {
      bit = 1U << 11;
      value.response_type = item_value;
      valid = !value.response_type.empty() && value.response_type.size() <= 255;
    } else if (key == "requestPayloadSize") {
      bit = 1U << 12;
      valid = Number(item_value, value.request_payload_size) && value.request_payload_size != 0;
    } else if (key == "responsePayloadSize") {
      bit = 1U << 13;
      valid = Number(item_value, value.response_payload_size) && value.response_payload_size != 0;
    } else if (key == "requestBuffer") {
      bit = 1U << 14;
      valid = Number(item_value, value.request_buffer) && value.request_buffer != 0;
    } else if (key == "responseBuffer") {
      bit = 1U << 15;
      valid = Number(item_value, value.response_buffer) && value.response_buffer != 0;
    } else if (key == "maxClients") {
      bit = 1U << 16;
      valid = Number(item_value, value.max_clients) && value.max_clients != 0;
    } else if (key == "maxServers") {
      bit = 1U << 17;
      valid = Number(item_value, value.max_servers) && value.max_servers != 0;
    } else {
      error = "unknown mapping key";
      return false;
    }
    if (!valid) {
      error = "invalid " + std::string(key);
      return false;
    }
    if ((seen & bit) != 0) {
      error = "duplicate mapping key";
      return false;
    }
    seen |= bit;
  }
  constexpr std::uint32_t common = (1U << 0) | (1U << 1) | (1U << 4) | (1U << 9);
  constexpr std::uint32_t pubsub =
      common | (1U << 2) | (1U << 3) | (1U << 5) | (1U << 6) | (1U << 7) | (1U << 8);
  constexpr std::uint32_t request_response = common | (1U << 10) | (1U << 11) | (1U << 12) |
                                             (1U << 13) | (1U << 14) | (1U << 15) | (1U << 16) |
                                             (1U << 17);
  auto expected = value.pattern == Mapping::Pattern::kPublishSubscribe ? pubsub : request_response;
  if (seen != expected) {
    error = "mapping is incomplete";
    return false;
  }
  out = std::move(value);
  error.clear();
  return true;
}

auto FormatMapping(Mapping const& value) -> std::string {
  if (value.pattern == Mapping::Pattern::kRequestResponse) {
    return "pattern=requestResponse;service=" + value.service +
           ";requestType=" + value.request_type + ";responseType=" + value.response_type +
           ";requestPayloadSize=" + std::to_string(value.request_payload_size) +
           ";responsePayloadSize=" + std::to_string(value.response_payload_size) +
           ";alignment=" + std::to_string(value.payload_alignment) +
           ";requestBuffer=" + std::to_string(value.request_buffer) +
           ";responseBuffer=" + std::to_string(value.response_buffer) +
           ";maxClients=" + std::to_string(value.max_clients) +
           ";maxServers=" + std::to_string(value.max_servers) +
           ";safeOverflow=" + (value.safe_overflow ? "true" : "false");
  }
  return "pattern=pubsub;service=" + value.service + ";type=" + value.type_name +
         ";payloadSize=" + std::to_string(value.payload_size) +
         ";alignment=" + std::to_string(value.payload_alignment) +
         ";history=" + std::to_string(value.history_depth) +
         ";subscriberBuffer=" + std::to_string(value.subscriber_buffer) +
         ";maxPublishers=" + std::to_string(value.max_publishers) +
         ";maxSubscribers=" + std::to_string(value.max_subscribers) +
         ";safeOverflow=" + (value.safe_overflow ? "true" : "false");
}
} // namespace ovf::com::transports::iceoryx2
