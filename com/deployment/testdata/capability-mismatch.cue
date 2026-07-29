// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

application: {
	schemaVersion: 1
	name:          "capability_mismatch_test"
	communication: instances: [{
		interface: "example.radar#RadarService"
		instance:  "front-radar"
		role:      "consumer"
		transport: "network"
		requirements: {
			features: ["discovery", "events", "methods", "loans"]
			limits: {
				maxPayloadSize:          2097152
				maxHistoryDepth:         512
				maxOutstandingOperations: 256
				maxEndpoints:            256
			}
		}
	}]
	execution: {
		readiness: "required"
		startup: timeoutMs: 1000
		shutdown: timeoutMs: 1000
		restart: {
			maxAttempts: 1
			delayMs:     0
		}
	}
}
