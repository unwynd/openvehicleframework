// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/transports/vsomeip_mapping.hpp"

#include <array>
#include <charconv>
#include <limits>

namespace ovf::com::transports::vsomeip {
namespace {

auto ParseUnsigned(std::string_view value, std::uint32_t limit, std::uint32_t& out) -> bool {
  if (value.empty())
    return false;
  std::uint32_t parsed{};
  auto const* begin = value.data();
  auto const* end = begin + value.size();
  auto result = std::from_chars(begin, end, parsed, 10);
  if (result.ec != std::errc{} || result.ptr != end || parsed > limit)
    return false;
  out = parsed;
  return true;
}

auto Kind(std::string_view value, ElementKind& out) -> bool {
  constexpr std::array entries{
      std::pair{"event", ElementKind::event},
      std::pair{"method", ElementKind::method},
      std::pair{"fieldGet", ElementKind::field_get},
      std::pair{"fieldSet", ElementKind::field_set},
      std::pair{"fieldNotify", ElementKind::field_notify},
  };
  for (auto const& [name, kind] : entries) {
    if (value == name) {
      out = kind;
      return true;
    }
  }
  return false;
}

auto KindName(ElementKind value) -> std::string_view {
  switch (value) {
  case ElementKind::event:
    return "event";
  case ElementKind::method:
    return "method";
  case ElementKind::field_get:
    return "fieldGet";
  case ElementKind::field_set:
    return "fieldSet";
  case ElementKind::field_notify:
    return "fieldNotify";
  }
  return {};
}

} // namespace

auto ParseMapping(std::string_view text, Mapping& out, std::string& error) -> bool {
  Mapping parsed{};
  std::uint8_t seen{};
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
    auto value = item.substr(equal + 1);
    std::uint32_t number{};
    std::uint8_t bit{};
    if (key == "service") {
      bit = 1U << 0;
      if (!ParseUnsigned(value, 0xffff, number) || number == 0)
        error = "invalid service";
      parsed.service = static_cast<std::uint16_t>(number);
    } else if (key == "instance") {
      bit = 1U << 1;
      if (!ParseUnsigned(value, 0xffff, number) || number == 0xffff)
        error = "invalid instance";
      parsed.instance = static_cast<std::uint16_t>(number);
    } else if (key == "element") {
      bit = 1U << 2;
      if (!ParseUnsigned(value, 0xffff, number))
        error = "invalid element";
      parsed.element = static_cast<std::uint16_t>(number);
    } else if (key == "eventGroup") {
      bit = 1U << 3;
      if (!ParseUnsigned(value, 0xffff, number))
        error = "invalid eventGroup";
      parsed.event_group = static_cast<std::uint16_t>(number);
    } else if (key == "major") {
      bit = 1U << 4;
      if (!ParseUnsigned(value, 0xff, number))
        error = "invalid major";
      parsed.major_version = static_cast<std::uint8_t>(number);
    } else if (key == "minor") {
      bit = 1U << 5;
      if (!ParseUnsigned(value, std::numeric_limits<std::uint32_t>::max(), number))
        error = "invalid minor";
      parsed.minor_version = number;
    } else if (key == "kind") {
      bit = 1U << 6;
      if (!Kind(value, parsed.kind))
        error = "invalid kind";
    } else if (key == "reliable") {
      bit = 1U << 7;
      if (value == "true")
        parsed.reliable = true;
      else if (value == "false")
        parsed.reliable = false;
      else
        error = "invalid reliable";
    } else {
      error = "unknown mapping key";
      return false;
    }
    if (!error.empty())
      return false;
    if ((seen & bit) != 0) {
      error = "duplicate mapping key";
      return false;
    }
    seen = static_cast<std::uint8_t>(seen | bit);
  }
  constexpr std::uint8_t required = 0xff;
  if (seen != required) {
    error = "mapping is incomplete";
    return false;
  }
  const bool notification =
      parsed.kind == ElementKind::event || parsed.kind == ElementKind::field_notify;
  if (notification != (parsed.event_group != 0)) {
    error = notification ? "notification requires eventGroup" : "method mapping forbids eventGroup";
    return false;
  }
  out = parsed;
  error.clear();
  return true;
}

auto FormatMapping(Mapping const& value) -> std::string {
  return "service=" + std::to_string(value.service) +
         ";instance=" + std::to_string(value.instance) +
         ";element=" + std::to_string(value.element) +
         ";eventGroup=" + std::to_string(value.event_group) +
         ";major=" + std::to_string(value.major_version) +
         ";minor=" + std::to_string(value.minor_version) +
         ";kind=" + std::string(KindName(value.kind)) +
         ";reliable=" + (value.reliable ? "true" : "false");
}

} // namespace ovf::com::transports::vsomeip
