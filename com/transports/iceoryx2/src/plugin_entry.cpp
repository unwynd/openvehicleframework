// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/transports/iceoryx2.h"

extern "C" const ovf_com_transport_factory_v1* ovf_com_transport_query_v1(void) {
  return ovf_com_iceoryx2_transport_query_v1();
}
