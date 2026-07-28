// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

platforms: [{
	transport: "ipc"
	profile:   "iceoryx2"
	provider:  "shared_memory"
	required:  true
}, {
	transport: "network"
	profile:   "vsomeip"
	provider:  "vehicle_network"
	required:  true
}]
