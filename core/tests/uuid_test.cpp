// SPDX-License-Identifier: Apache-2.0

#include "ovf/core/uuid.hpp"

#include <gtest/gtest.h>

namespace {

TEST(CoreUuid, DefaultIsZero) {
  ovf::core::Uuid uuid{};
  EXPECT_EQ(uuid.to_string(), "00000000-0000-0000-0000-000000000000");
}

TEST(CoreUuid, FormatsAsHyphenatedHex) {
  ovf::core::Uuid uuid{{{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
                        0xcc, 0xdd, 0xee, 0xff}}};
  EXPECT_EQ(uuid.to_string(), "00112233-4455-6677-8899-aabbccddeeff");
}

TEST(CoreUuid, EqualityAndOrder) {
  ovf::core::Uuid a{{{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}}};
  ovf::core::Uuid b = a;
  ovf::core::Uuid c{{{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17}}};
  EXPECT_EQ(a, b);
  EXPECT_LT(a, c);
}

} // namespace
