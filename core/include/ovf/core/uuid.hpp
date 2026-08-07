// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ovf::core {

// Uuid is the shared 128-bit identifier used across com and per generated
// code. It is intentionally trivial (aggregate of a byte array) so it can be
// initialized directly from a brace-enclosed byte list in generated headers.
struct Uuid final {
  std::array<std::uint8_t, 16> bytes{};

  friend constexpr auto operator<=>(Uuid const&, Uuid const&) = default;
  friend constexpr bool operator==(Uuid const&, Uuid const&) = default;

  // to_string returns the canonical 36-character 8-4-4-4-12 hyphenated form.
  [[nodiscard]] std::string to_string() const {
    static constexpr char kHex[] = "0123456789abcdef";
    static constexpr std::array<std::size_t, 4> kHyphens{4, 6, 8, 10};
    std::string out;
    out.reserve(36);
    std::size_t hyphen = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      if (hyphen < kHyphens.size() && i == kHyphens[hyphen]) {
        out.push_back('-');
        ++hyphen;
      }
      out.push_back(kHex[(bytes[i] >> 4U) & 0x0fU]);
      out.push_back(kHex[bytes[i] & 0x0fU]);
    }
    return out;
  }
};

} // namespace ovf::core
