// SPDX-License-Identifier: Apache-2.0

package ovf_exec_deployment

application: {
	schemaVersion: 1
	name:          "managed_test"
	communication: instances: [{
		interface: "test.exec#ManagedTest"
		instance:  "managed-test"
		role:      "provider"
		transport: "ipc"
	}]
	execution: {
		readiness: "required"
		startup: timeoutMs: 2000
		shutdown: timeoutMs: 2000
		restart: {
			maxAttempts: 2
			delayMs:     10
		}
	}
}

allocation: {
	schemaVersion: 1
	generation:    17
	applications: managed_test: {
		id:         1
		executable: "/tmp/ovf-execd-e2e/bin/managed-test"
		arguments:  []
		dependencies:       []
		exclusiveResources: []
	}
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
			applications: ["managed_test"]
			constraints:  []
		}]
	}]
}
