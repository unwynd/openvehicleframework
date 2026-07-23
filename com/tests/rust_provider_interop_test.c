// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/transport_abi.h"
#include <assert.h>

extern const ovf_com_transport_factory_v1* ovf_com_rust_conformance_query_v1(void);

static ovf_com_status_v1 dispatch(void* host, ovf_com_task_fn_v1 task,
                                  ovf_com_task_release_fn_v1 release, void* user) {
  (void)host;
  task(user);
  release(user);
  return OVF_COM_STATUS_OK;
}
static uint64_t now(void* host) {
  (void)host;
  return 1;
}

int main(void) {
  const ovf_com_transport_factory_v1* factory = ovf_com_rust_conformance_query_v1();
  assert(factory && factory->struct_size == sizeof(*factory));
  assert(factory->abi_version == OVF_COM_TRANSPORT_ABI_VERSION_1);
  ovf_com_host_api_v1 host = {sizeof(host), 0, 0, dispatch, now};
  ovf_com_transport_config_v1 config = {sizeof(config), {0, 0}, {0, 0}, 1, 1};
  ovf_com_transport_v1* transport = 0;
  assert(factory->create(&host, &config, &transport) == OVF_COM_STATUS_OK);
  assert(transport && transport->struct_size == sizeof(*transport));
  ovf_com_capabilities_v1 capabilities = {0};
  capabilities.struct_size = sizeof(capabilities);
  assert(transport->get_capabilities(transport, &capabilities) == OVF_COM_STATUS_OK);
  assert(capabilities.isolation == OVF_COM_ISOLATION_INDEPENDENT);
  assert(transport->start(transport) == OVF_COM_STATUS_OK);
  assert(transport->stop(transport) == OVF_COM_STATUS_OK);
  factory->destroy(transport);
}
