// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/transports/iceoryx2_mapping.hpp"

#include <gtest/gtest.h>
#include <string>

TEST(Iceoryx2MappingTest, ParsesFormatsAndRejectsInvalidMappings) {
  using namespace ovf::com::transports::iceoryx2;
  constexpr auto text = "pattern=pubsub;service=vehicle/radar/front/objects;type=RadarObjects;"
                        "payloadSize=64;alignment=8;history=1;subscriberBuffer=8;"
                        "maxPublishers=2;maxSubscribers=8;maxLoanedSamples=2;"
                        "maxBorrowedSamples=2;safeOverflow=false";
  Mapping mapping{};
  std::string error;
  ASSERT_TRUE(ParseMapping(text, mapping, error));
  ASSERT_TRUE(mapping.service == "vehicle/radar/front/objects");
  ASSERT_TRUE(mapping.payload_size == 64);
  ASSERT_TRUE(mapping.payload_alignment == 8);
  ASSERT_TRUE(FormatMapping(mapping) == text);
  ASSERT_TRUE(!ParseMapping("service=x", mapping, error));
  ASSERT_TRUE(!ParseMapping("pattern=pubsub;service=x;service=y;type=t;payloadSize=1;alignment=1;"
                            "history=0;subscriberBuffer=1;maxPublishers=1;"
                            "maxSubscribers=1;safeOverflow=false",
                            mapping, error));
  ASSERT_TRUE(!ParseMapping("pattern=pubsub;service=x;type=t;payloadSize=1;alignment=3;history=0;"
                            "subscriberBuffer=1;maxPublishers=1;maxSubscribers=1;"
                            "safeOverflow=false",
                            mapping, error));
  constexpr auto method =
      "pattern=requestResponse;service=vehicle/radar/front/calibrate;"
      "requestType=CalibrateInput;responseType=CalibrateResult;"
      "requestPayloadSize=128;responsePayloadSize=256;alignment=8;"
      "requestBuffer=16;responseBuffer=8;maxClients=4;maxServers=1;maxLoanedRequests=2;"
      "maxBorrowedResponses=2;maxLoanedResponses=1;safeOverflow=false";
  ASSERT_TRUE(ParseMapping(method, mapping, error));
  ASSERT_EQ(mapping.pattern, Mapping::Pattern::kRequestResponse);
  ASSERT_EQ(mapping.request_payload_size, 128);
  ASSERT_EQ(mapping.response_payload_size, 256);
  ASSERT_EQ(FormatMapping(mapping), method);
}
