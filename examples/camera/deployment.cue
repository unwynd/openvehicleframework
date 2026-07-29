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
	execution: {
		readiness: "required"
		startup: timeoutMs: 5000
		shutdown: timeoutMs: 3000
		restart: {
			maxAttempts: 2
			delayMs:     100
		}
	}
}
