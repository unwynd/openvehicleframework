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
  std::uint16_t seen{};
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
    std::uint16_t bit{};
    bool valid = true;
    if (key == "service") {
      bit = 1U << 0;
      value.service = item_value;
      valid = value.service.size() <= 255;
    } else if (key == "type") {
      bit = 1U << 1;
      value.type_name = item_value;
      valid = value.type_name.size() <= 255;
    } else if (key == "payloadSize") {
      bit = 1U << 2;
      valid = Number(item_value, value.payload_size) && value.payload_size != 0;
    } else if (key == "alignment") {
      bit = 1U << 3;
      valid = Number(item_value, value.payload_alignment) && PowerOfTwo(value.payload_alignment);
    } else if (key == "history") {
      bit = 1U << 4;
      valid = Number(item_value, value.history_depth);
    } else if (key == "subscriberBuffer") {
      bit = 1U << 5;
      valid = Number(item_value, value.subscriber_buffer) && value.subscriber_buffer != 0;
    } else if (key == "maxPublishers") {
      bit = 1U << 6;
      valid = Number(item_value, value.max_publishers) && value.max_publishers != 0;
    } else if (key == "maxSubscribers") {
      bit = 1U << 7;
      valid = Number(item_value, value.max_subscribers) && value.max_subscribers != 0;
    } else if (key == "safeOverflow") {
      bit = 1U << 8;
      if (item_value == "true")
        value.safe_overflow = true;
      else if (item_value == "false")
        value.safe_overflow = false;
      else
        valid = false;
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
    seen = static_cast<std::uint16_t>(seen | bit);
  }
  if (seen != 0x1ffU) {
    error = "mapping is incomplete";
    return false;
  }
  out = std::move(value);
  error.clear();
  return true;
}

auto FormatMapping(Mapping const& value) -> std::string {
  return "service=" + value.service + ";type=" + value.type_name +
         ";payloadSize=" + std::to_string(value.payload_size) +
         ";alignment=" + std::to_string(value.payload_alignment) +
         ";history=" + std::to_string(value.history_depth) +
         ";subscriberBuffer=" + std::to_string(value.subscriber_buffer) +
         ";maxPublishers=" + std::to_string(value.max_publishers) +
         ";maxSubscribers=" + std::to_string(value.max_subscribers) +
         ";safeOverflow=" + (value.safe_overflow ? "true" : "false");
}
} // namespace ovf::com::transports::iceoryx2
