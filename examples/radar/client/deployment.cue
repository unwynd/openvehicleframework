// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

application: {
	schemaVersion: 1
	name:          "radar_client"
	communication: instances: [{
		interface: "example.radar#RadarService"
		instance:  "front-radar"
		role:      "consumer"
		transport: "network"
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
