// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/transports/iceoryx2_mapping.hpp"

#include <gtest/gtest.h>
#include <string>

TEST(Iceoryx2MappingTest, ParsesFormatsAndRejectsInvalidMappings) {
  using namespace ovf::com::transports::iceoryx2;
  constexpr auto text = "service=vehicle/radar/front/objects;type=RadarObjects;"
                        "payloadSize=64;alignment=8;history=1;subscriberBuffer=8;"
                        "maxPublishers=2;maxSubscribers=8;safeOverflow=false";
  Mapping mapping{};
  std::string error;
  ASSERT_TRUE(ParseMapping(text, mapping, error));
  ASSERT_TRUE(mapping.service == "vehicle/radar/front/objects");
  ASSERT_TRUE(mapping.payload_size == 64);
  ASSERT_TRUE(mapping.payload_alignment == 8);
  ASSERT_TRUE(FormatMapping(mapping) == text);
  ASSERT_TRUE(!ParseMapping("service=x", mapping, error));
  ASSERT_TRUE(!ParseMapping("service=x;service=y;type=t;payloadSize=1;alignment=1;"
                            "history=0;subscriberBuffer=1;maxPublishers=1;"
                            "maxSubscribers=1;safeOverflow=false",
                            mapping, error));
  ASSERT_TRUE(!ParseMapping("service=x;type=t;payloadSize=1;alignment=3;history=0;"
                            "subscriberBuffer=1;maxPublishers=1;maxSubscribers=1;"
                            "safeOverflow=false",
                            mapping, error));
}
