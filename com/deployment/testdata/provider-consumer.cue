// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

deployment: {
	deploymentVersion: 1
	instances: [
		{
			interface: "example.radar#RadarService"
			instance:  "front-radar"
			role:      "provider"
			transport: platform.transport
		},
		{
			interface: "example.radar#RadarService"
			instance:  "rear-radar"
			role:      "consumer"
			transport: platform.transport
		},
	]
}
