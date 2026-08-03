// SPDX-License-Identifier: Apache-2.0

#include "ovf/crypto/backend_abi.h"

int main(void) {
  ovf_crypto_capabilities_v1 capabilities = {0};
  capabilities.struct_size = sizeof(capabilities);
  return OVF_CRYPTO_BACKEND_ABI_VERSION_1 == 1U && OVF_CRYPTO_INVALID_HANDLE_V1 == 0U ? 0 : 1;
}
