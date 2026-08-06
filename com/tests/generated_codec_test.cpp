// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/generated.hpp"

#include <gtest/gtest.h>

TEST(GeneratedCodec, BooleanRoundTripUsesCanonicalValues) {
  auto encoded = ovf::com::encode(true);
  bool decoded{};
  EXPECT_TRUE(ovf::com::decode(encoded, decoded));
  EXPECT_TRUE(decoded);
}

TEST(GeneratedCodec, BooleanRejectsNonCanonicalValues) {
  const std::array invalid{std::byte{2}};
  bool decoded{};
  EXPECT_FALSE(ovf::com::decode(invalid, decoded));
}

TEST(GeneratedCodec, ContainersAndBytesRoundTrip) {
  std::vector<std::uint16_t> values{1, 0x1234, 0xffff};
  auto encoded_values = ovf::com::encode(values);
  std::vector<std::uint16_t> decoded_values;
  ASSERT_TRUE(ovf::com::decode(encoded_values, decoded_values));
  EXPECT_EQ(decoded_values, values);

  std::string text{"camera\0frame", 12};
  auto encoded_text = ovf::com::encode(text);
  std::string decoded_text;
  ASSERT_TRUE(ovf::com::decode(encoded_text, decoded_text));
  EXPECT_EQ(decoded_text, text);

  std::array<std::uint32_t, 3> fixed{3, 2, 1};
  auto encoded_fixed = ovf::com::encode(fixed);
  std::array<std::uint32_t, 3> decoded_fixed{};
  ASSERT_TRUE(ovf::com::decode(encoded_fixed, decoded_fixed));
  EXPECT_EQ(decoded_fixed, fixed);
}

TEST(GeneratedCodec, RejectsImpossibleUnboundedSequenceLength) {
  const std::array invalid{std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0x7f}};
  std::vector<std::uint64_t> decoded;
  EXPECT_FALSE(ovf::com::decode(invalid, decoded));
  EXPECT_TRUE(decoded.empty());
}
