// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

application: {
	name: "per-e2e"
	persistence: stores: [{
		name:              "operational-state"
		minimumDurability: "process_crash"
		capacityBytes:     1048576
		maxEntries:        64
		maxKeySize:        64
		maxValueSize:      4096
		maxBlobSize:       262144
	}]
}
