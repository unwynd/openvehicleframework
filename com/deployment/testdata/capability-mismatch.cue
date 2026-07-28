// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

deployment: {
	deploymentVersion: 1
	instances: [{
		interface: "example.radar#RadarService"
		instance:  "front-radar"
		role:      "consumer"
		transport: platforms[0].transport
		requirements: {
			features: ["discovery", "events", "methods", "loans"]
			limits: {
				maxPayloadSize:          2097152
				maxHistoryDepth:         512
				maxOutstandingOperations: 256
				maxEndpoints:            256
			}
		}
	}]
}
