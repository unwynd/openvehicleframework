// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/transports/vsomeip.h"

extern "C" const ovf_com_transport_factory_v1* ovf_com_transport_query_v1() {
  return ovf_com_vsomeip_transport_query_v1();
}
