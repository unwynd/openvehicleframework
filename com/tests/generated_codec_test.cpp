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
