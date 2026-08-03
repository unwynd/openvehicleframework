// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

application: {
	schemaVersion: 1
	name:          "driving_policy"
	communication: instances: [{
		interface: "example.environment#EnvironmentModelService"
		instance:  "fused-environment"
		role:      "consumer"
		transport: "network"
	}]
	logging: {
		loggers: [{
			name:        "driving_policy.environment"
			description: "Driving policy environment input"
		}]
		queueCapacity:   512
		criticalReserve: 32
		initialLevel:    "info"
	}
	persistence: stores: [{
		name:              "policy-state"
		minimumDurability: "process_crash"
		capacityBytes:     262144
		maxEntries:        8
		maxKeySize:        64
		maxValueSize:      1024
		maxBlobSize:       65536
		schemaId:          "e4627558-8f16-4c36-8363-51cddc8e5533"
		schemaVersion:     1
	}]
	execution: {
		readiness: "lifecycle_channel"
		startup: timeoutMs: 5000
		shutdown: timeoutMs: 3000
		restart: {
			maxAttempts: 1
			delayMs:     0
		}
	}
}
