// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

application: {
	schemaVersion: 1
	name:          "crypto_facility_compile"
	communication: instances: [{
		interface: "example.radar#RadarService"
		instance:  "front-radar"
		role:      "consumer"
		transport: "network"
	}]
	crypto: {}
	execution: {
		readiness: "lifecycle_channel"
		startup: timeoutMs: 1000
		shutdown: timeoutMs: 1000
		restart: {
			maxAttempts: 0
			delayMs:     0
		}
	}
}
