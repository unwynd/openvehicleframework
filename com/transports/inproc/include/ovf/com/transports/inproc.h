// SPDX-License-Identifier: Apache-2.0

#ifndef OVF_COM_TRANSPORTS_INPROC_H
#define OVF_COM_TRANSPORTS_INPROC_H

#include "ovf/com/transport_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

const ovf_com_transport_factory_v1* ovf_com_inproc_transport_query_v1(void);

#ifdef __cplusplus
}
#endif

#endif // OVF_COM_TRANSPORTS_INPROC_H
