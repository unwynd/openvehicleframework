// SPDX-License-Identifier: Apache-2.0

package ovf_exec_deployment

deployment: {
	deploymentVersion: 1
	generation:        17
	applications: [{
		id:         1
		name:       "managed_test"
		executable: "/tmp/ovf-execd-e2e/bin/managed-test"
		arguments: []
		readiness: "required"
		startTimeoutMs: 2000
		stopTimeoutMs:  2000
		retry: {
			maxAttempts: 2
			delayMs:     10
		}
		dependencies:      []
		exclusiveResources: []
	}]
	domains: [{
		id:          1
		name:        "machine"
		initialMode: 1
		replacement: "reject_while_busy"
		recovery: {
			action:     "hold_observed_configuration"
			deadlineMs: 2000
		}
		modes: [{
			id:           1
			name:         "stopped"
			applications: []
			constraints:  []
		}, {
			id:           2
			name:         "running"
			applications: [1]
			constraints:  []
		}]
	}]
}
