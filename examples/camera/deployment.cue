// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

deployment: {
	deploymentVersion: 1
	instances: [{
		interface: "example.camera#CameraService"
		instance:  "front-camera"
		role:      "provider"
		transport: "ipc"
	}]
}
