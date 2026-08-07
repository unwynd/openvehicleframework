// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

application: {
	schemaVersion: 1
	name:          "camera"
	communication: instances: [{
		interface: "example.camera#CameraService"
		instance:  "front-camera"
		role:      "provider"
		transport: "ipc"
	}]
	logging: {
		loggers: [{
			name:        "camera.capture"
			description: "Camera capture pipeline"
		}]
		events: [{
			name:        "frame_published"
			level:       "info"
			description: "Camera frame published to the runtime"
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
