// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/transports/vsomeip_mapping.hpp"

#include <gtest/gtest.h>
#include <string>

using ovf::com::transports::vsomeip::ElementKind;
using ovf::com::transports::vsomeip::FormatMapping;
using ovf::com::transports::vsomeip::Mapping;
using ovf::com::transports::vsomeip::ParseMapping;

TEST(VsomeipMappingTest, ParsesFormatsAndRejectsInvalidMappings) {
  Mapping mapping{};
  std::string error;
  auto canonical = "service=4660;instance=1;element=32769;eventGroup=1;"
                   "major=1;minor=0;kind=event;reliable=true";
  ASSERT_TRUE(ParseMapping(canonical, mapping, error));
  ASSERT_TRUE(mapping.service == 4660 && mapping.instance == 1 && mapping.element == 32769);
  ASSERT_TRUE(mapping.event_group == 1 && mapping.major_version == 1 && mapping.minor_version == 0);
  ASSERT_TRUE(mapping.kind == ElementKind::event && mapping.reliable);
  ASSERT_TRUE(FormatMapping(mapping) == canonical);

  ASSERT_TRUE(!ParseMapping("", mapping, error));
  ASSERT_TRUE(!ParseMapping("service=0;instance=1;element=1;eventGroup=1;major=1;minor=0;"
                            "kind=event;reliable=true",
                            mapping, error));
  ASSERT_TRUE(!ParseMapping("service=1;instance=1;element=1;eventGroup=0;major=1;minor=0;"
                            "kind=event;reliable=true",
                            mapping, error));
  ASSERT_TRUE(!ParseMapping("service=1;instance=1;element=1;eventGroup=1;major=1;minor=0;"
                            "kind=method;reliable=false",
                            mapping, error));
  ASSERT_TRUE(!ParseMapping("service=1;service=2;instance=1;element=1;eventGroup=0;major=1;"
                            "minor=0;kind=method;reliable=false",
                            mapping, error));
}
