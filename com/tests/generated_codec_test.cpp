// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/generated.hpp"
#include "radar.hpp"

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

TEST(GeneratedCodec, FramesTaggedStructuresInsideSequences) {
  ovf::com::BoundedVector<example::radar::RadarObject, 4> objects;
  ASSERT_TRUE(objects.push_back({7, 12.5F, -1.25F, 93}));
  ASSERT_TRUE(objects.push_back({8, 18.0F, 2.5F, 81}));

  auto encoded = ovf::com::encode(objects);
  decltype(objects) decoded;
  ASSERT_TRUE(ovf::com::decode(encoded, decoded));
  ASSERT_EQ(decoded.size(), 2U);
  EXPECT_EQ(decoded.values()[0].id, 7U);
  EXPECT_EQ(decoded.values()[1].id, 8U);

  ASSERT_FALSE(encoded.empty());
  encoded.pop_back();
  EXPECT_FALSE(ovf::com::decode(encoded, decoded));
  EXPECT_EQ(decoded.size(), 2U);
}

TEST(GeneratedCodec, TaggedStructuresSkipUnknownOptionalFieldsAndRejectDuplicates) {
  example::radar::RadarObject object{7, 12.5F, -1.25F, 93};
  auto encoded = ovf::com::encode(object);
  ASSERT_EQ(encoded.size(), 43U);
  EXPECT_EQ(encoded[0], std::byte{1});
  EXPECT_EQ(encoded[4], std::byte{2});

  auto with_unknown = encoded;
  const std::array unknown{std::byte{99}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
                           std::byte{0},  std::byte{0}, std::byte{0}, std::byte{42}};
  with_unknown.insert(with_unknown.end(), unknown.begin(), unknown.end());
  example::radar::RadarObject decoded{};
  EXPECT_TRUE(ovf::com::decode(with_unknown, decoded));
  EXPECT_EQ(decoded.id, object.id);
  EXPECT_EQ(decoded.confidence, object.confidence);

  auto duplicate = encoded;
  duplicate.insert(duplicate.end(), encoded.begin(), encoded.end());
  EXPECT_FALSE(ovf::com::decode(duplicate, decoded));
  EXPECT_FALSE(ovf::com::decode(std::span<const std::byte>{}, decoded));
}

TEST(GeneratedCodec, ReportsCompileTimeMaximumSizeForBoundedTypes) {
  constexpr auto maximum = ovf::com::maximum_encoded_size<example::radar::RadarObject>();
  static_assert(maximum.has_value());
  static_assert(*maximum == 43U);
  EXPECT_EQ(ovf::com::maximum_encoded_size<std::string>(), std::nullopt);
}
