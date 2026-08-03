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
		schemaId:          "7b5a1411-c98e-4bcc-b672-443415c156f1"
		schemaVersion:     1
		initialData: [{key: "mode", valueHex: "7265616479"}]
	}]
}
