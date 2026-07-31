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
		readiness: "lifecycle_channel"
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
	units: managed_test: {
		id:         1
		name:       "managed_test"
		kind:       "managed_application"
		bootstrap:  false
		executable: "/tmp/ovf-execd-e2e/bin/managed-test"
		arguments:  []
		dependencies:       ["system_test"]
		exclusiveResources: []
	}
	units: system_test: {
		id:         2
		name:       "system_test"
		kind:       "service"
		bootstrap:  false
		executable: "/tmp/ovf-execd-e2e/bin/system-test"
		arguments:  []
		readiness:  "supervisor_notification"
		startTimeoutMs: 2000
		stopTimeoutMs:  2000
		retry: {
			maxAttempts: 2
			delayMs:     10
		}
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
			units:       []
			constraints:  []
		}, {
			id:           2
			name:         "running"
			units:       ["system_test", "managed_test"]
			constraints:  []
		}]
	}]
}
