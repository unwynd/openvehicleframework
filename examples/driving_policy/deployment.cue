// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

deployment: {
	deploymentVersion: 1
	instances: [{
		interface: "example.environment#EnvironmentModelService"
		instance:  "fused-environment"
		role:      "consumer"
		transport: "network"
	}]
}
