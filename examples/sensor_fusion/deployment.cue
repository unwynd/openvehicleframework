// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

application: {
	schemaVersion: 1
	name:          "sensor_fusion"
	communication: instances: [{
		interface: "example.camera#CameraService"
		instance:  "front-camera"
		role:      "consumer"
		transport: "ipc"
	}, {
		interface: "example.radar#RadarService"
		instance:  "front-radar"
		role:      "consumer"
		transport: "ipc"
	}, {
		interface: "example.environment#EnvironmentModelService"
		instance:  "fused-environment"
		role:      "provider"
		transport: "network"
	}]
	execution: {
		readiness: "lifecycle_channel"
		startup: timeoutMs: 7000
		shutdown: timeoutMs: 3000
		restart: {
			maxAttempts: 2
			delayMs:     200
		}
	}
}
