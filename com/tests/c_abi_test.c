// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/transport_abi.h"

int main(void) {
  ovf_com_transport_config_v1 config = {0};
  config.struct_size = sizeof(config);
  ovf_com_capabilities_v1 capabilities = {0};
  capabilities.struct_size = sizeof(capabilities);
  return OVF_COM_TRANSPORT_ABI_VERSION_1 == 1U && sizeof(ovf_com_uuid_v1) == 16U &&
                 sizeof(ovf_com_handle_v1) == 8U && config.struct_size >= 4U &&
                 capabilities.struct_size >= 4U
             ? 0
             : 1;
}
