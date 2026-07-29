// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

application: {
	schemaVersion: 1
	name:          "provider_consumer_test"
	communication: instances: [
		{
			interface: "example.radar#RadarService"
			instance:  "front-radar"
			role:      "provider"
			transport: "network"
		},
		{
			interface: "example.radar#RadarService"
			instance:  "rear-radar"
			role:      "consumer"
			transport: "network"
		},
	]
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
