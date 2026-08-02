// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

application: {
	schemaVersion: 1
	name:          "radar"
	communication: instances: [{
		interface: "example.radar#RadarService"
		instance:  "front-radar"
		role:      "provider"
		transport: "ipc"
	}]
	logging: {
		loggers: [{
			name:        "radar.service"
			description: "Radar service"
		}]
		queueCapacity:   1024
		criticalReserve: 32
		initialLevel:    "info"
	}
	execution: {
		readiness: "lifecycle_channel"
		startup: timeoutMs: 5000
		shutdown: timeoutMs: 3000
		restart: {
			maxAttempts: 2
			delayMs:     100
		}
	}
}
