// SPDX-License-Identifier: Apache-2.0

#include <dinit-client.h>

int main() {
  observed_states_t states;
  return states.started ? 1 : 0;
}
