// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

deployment: {
	deploymentVersion: 1
	instances: [{
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
}
