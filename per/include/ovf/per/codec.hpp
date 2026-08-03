// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/per/per.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ovf::per {

template <std::size_t Capacity> class BoundedString final {
public:
  [[nodiscard]] bool assign(std::string_view value) {
    if (value.size() > Capacity) {
      return false;
    }
    value_.assign(value);
    return true;
  }
  [[nodiscard]] std::string_view view() const noexcept { return value_; }
  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  friend bool operator==(const BoundedString&, const BoundedString&) = default;

private:
  std::string value_;
};

namespace codec {

template <typename T, bool = std::is_floating_point_v<T>> struct ScalarBits;
template <typename T> struct ScalarBits<T, false> final {
  using type = std::make_unsigned_t<T>;
};
template <typename T> struct ScalarBits<T, true> final {
  using type = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
};

inline bool Append(std::vector<std::byte>& output, std::span<const std::byte> bytes,
                   std::size_t maximum) {
  if (bytes.size() > maximum || output.size() > maximum - bytes.size()) {
    return false;
  }
  output.insert(output.end(), bytes.begin(), bytes.end());
  return true;
}

template <typename T>
  requires(std::is_integral_v<T> || std::is_floating_point_v<T>)
bool EncodeScalar(T value, std::vector<std::byte>& output, std::size_t maximum) {
  using Bits = typename ScalarBits<T>::type;
  Bits bits{};
  if constexpr (std::is_floating_point_v<T>) {
    bits = std::bit_cast<Bits>(value);
  } else {
    bits = static_cast<Bits>(value);
  }
  std::byte encoded[sizeof(T)]{};
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    encoded[sizeof(T) - index - 1] = static_cast<std::byte>(bits & 0xffU);
    bits >>= 8U;
  }
  return Append(output, encoded, maximum);
}

template <typename T>
  requires(std::is_integral_v<T> || std::is_floating_point_v<T>)
bool DecodeScalar(std::span<const std::byte> input, T& value) {
  if (input.size() != sizeof(T)) {
    return false;
  }
  using Bits = typename ScalarBits<T>::type;
  Bits bits{};
  for (const auto byte : input) {
    bits = static_cast<Bits>((bits << 8U) | std::to_integer<unsigned>(byte));
  }
  if constexpr (std::is_floating_point_v<T>) {
    value = std::bit_cast<T>(bits);
  } else {
    value = static_cast<T>(bits);
  }
  return true;
}

template <typename T> struct Codec;

template <typename T>
  requires(std::is_integral_v<T> || std::is_floating_point_v<T>)
struct Codec<T> final {
  static bool Encode(T value, std::vector<std::byte>& output, std::size_t maximum) {
    return EncodeScalar(value, output, maximum);
  }
  static bool Decode(std::span<const std::byte> input, T& value) {
    return DecodeScalar(input, value);
  }
};

template <std::size_t Capacity> struct Codec<BoundedString<Capacity>> final {
  static bool Encode(const BoundedString<Capacity>& value, std::vector<std::byte>& output,
                     std::size_t maximum) {
    const auto text = value.view();
    return text.size() <= Capacity &&
           Append(output, {reinterpret_cast<const std::byte*>(text.data()), text.size()}, maximum);
  }
  static bool Decode(std::span<const std::byte> input, BoundedString<Capacity>& value) {
    return input.size() <= Capacity &&
           value.assign({reinterpret_cast<const char*>(input.data()), input.size()});
  }
};

template <typename T> struct Codec<std::optional<T>> final {
  static bool Encode(const std::optional<T>& value, std::vector<std::byte>& output,
                     std::size_t maximum) {
    if (!value) {
      return true;
    }
    return Codec<T>::Encode(*value, output, maximum);
  }
  static bool Decode(std::span<const std::byte> input, std::optional<T>& value) {
    T decoded{};
    if (!Codec<T>::Decode(input, decoded)) {
      return false;
    }
    value = std::move(decoded);
    return true;
  }
};

inline bool AppendField(std::uint32_t tag, std::span<const std::byte> payload,
                        std::vector<std::byte>& output, std::size_t maximum) {
  return payload.size() <= std::numeric_limits<std::uint32_t>::max() &&
         EncodeScalar(tag, output, maximum) &&
         EncodeScalar(static_cast<std::uint32_t>(payload.size()), output, maximum) &&
         Append(output, payload, maximum);
}

inline bool NextField(std::span<const std::byte> input, std::size_t& offset, std::uint32_t& tag,
                      std::span<const std::byte>& payload) {
  if (offset > input.size() || input.size() - offset < 8) {
    return false;
  }
  std::uint32_t size{};
  if (!DecodeScalar(input.subspan(offset, 4), tag) ||
      !DecodeScalar(input.subspan(offset + 4, 4), size) || size > input.size() - offset - 8) {
    return false;
  }
  payload = input.subspan(offset + 8, size);
  offset += 8 + size;
  return true;
}

inline bool AppendEnvelope(std::span<const std::byte, 16> schema, std::uint64_t version,
                           std::span<const std::byte> payload, std::vector<std::byte>& output,
                           std::size_t maximum) {
  constexpr std::byte magic[]{std::byte{'O'}, std::byte{'V'}, std::byte{'P'}, std::byte{'R'}};
  return Append(output, magic, maximum) && Append(output, schema, maximum) &&
         EncodeScalar(version, output, maximum) && Append(output, payload, maximum);
}

inline bool OpenEnvelope(std::span<const std::byte> input, std::span<const std::byte, 16> schema,
                         std::uint64_t version, std::span<const std::byte>& payload) {
  constexpr std::byte magic[]{std::byte{'O'}, std::byte{'V'}, std::byte{'P'}, std::byte{'R'}};
  if (input.size() < 28 || !std::equal(std::begin(magic), std::end(magic), input.begin()) ||
      !std::equal(schema.begin(), schema.end(), input.begin() + 4)) {
    return false;
  }
  std::uint64_t encoded_version{};
  if (!DecodeScalar(input.subspan(20, 8), encoded_version) || encoded_version != version) {
    return false;
  }
  payload = input.subspan(28);
  return true;
}

} // namespace codec
} // namespace ovf::per
