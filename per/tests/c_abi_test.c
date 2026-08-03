// SPDX-License-Identifier: Apache-2.0

#include "ovf/per/backend_abi.h"

#include <stddef.h>

int main(void) {
  ovf_per_backend_v1 backend = {0};
  ovf_per_store_descriptor_v1 descriptor = {0};
  ovf_per_commit_result_v1 commit = {0};
  backend.struct_size = sizeof(backend);
  descriptor.struct_size = sizeof(descriptor);
  commit.struct_size = sizeof(commit);
  return backend.struct_size >= descriptor.struct_size &&
                 commit.struct_size >= sizeof(ovf_per_commit_result_v1)
             ? 0
             : 1;
}
