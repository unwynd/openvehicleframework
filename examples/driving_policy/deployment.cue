// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

application: {
	schemaVersion: 1
	name:          "driving_policy"
	communication: instances: [{
		interface: "example.environment#EnvironmentModelService"
		instance:  "fused-environment"
		role:      "consumer"
		transport: "network"
	}]
	execution: {
		readiness: "required"
		startup: timeoutMs: 5000
		shutdown: timeoutMs: 3000
		restart: {
			maxAttempts: 1
			delayMs:     0
		}
	}
}
