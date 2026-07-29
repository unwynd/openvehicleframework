// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/backend_abi.h"

int main(void) {
  ovf_exec_evidence_v1 evidence = {0};
  evidence.struct_size = sizeof(evidence);
  evidence.state = OVF_EXEC_APPLICATION_READY;
  return evidence.struct_size == sizeof(ovf_exec_evidence_v1) ? 0 : 1;
}
