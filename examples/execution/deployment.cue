// SPDX-License-Identifier: Apache-2.0

package ovf_exec_deployment

deployment: {
	deploymentVersion: 1
	generation:        1
	applications: [{
		id:         1
		name:       "camera"
		executable: "/usr/bin/ovf-camera"
		arguments: []
		readiness: "required"
		startTimeoutMs: 5000
		stopTimeoutMs:  3000
		retry: {
			maxAttempts: 2
			delayMs:     100
		}
		dependencies: []
		exclusiveResources: [101]
	}, {
		id:         2
		name:       "radar"
		executable: "/usr/bin/ovf-radar"
		arguments: []
		readiness: "required"
		startTimeoutMs: 5000
		stopTimeoutMs:  3000
		retry: {
			maxAttempts: 2
			delayMs:     100
		}
		dependencies: []
		exclusiveResources: [102]
	}, {
		id:         3
		name:       "sensor_fusion"
		executable: "/usr/bin/ovf-sensor-fusion"
		arguments: []
		readiness: "required"
		startTimeoutMs: 7000
		stopTimeoutMs:  3000
		retry: {
			maxAttempts: 2
			delayMs:     200
		}
		dependencies: [1, 2]
		exclusiveResources: []
	}, {
		id:         4
		name:       "driving_policy"
		executable: "/usr/bin/ovf-driving-policy"
		arguments: []
		readiness: "required"
		startTimeoutMs: 5000
		stopTimeoutMs:  3000
		retry: {
			maxAttempts: 1
			delayMs:     0
		}
		dependencies: [3]
		exclusiveResources: []
	}]
	domains: [{
		id:          1
		name:        "machine"
		initialMode: 1
		replacement: "supersede_if_safe"
		recovery: {
			action:     "hold_observed_configuration"
			deadlineMs: 5000
		}
		modes: [{
			id:           1
			name:         "boot"
			applications: []
			constraints:  []
		}, {
			id:           2
			name:         "operational"
			applications: [1, 2, 3]
			constraints:  []
		}]
	}, {
		id:          2
		name:        "driving"
		initialMode: 1
		replacement: "reject_while_busy"
		recovery: {
			action:       "enter_fallback_mode"
			fallbackMode: 1
			deadlineMs:   5000
		}
		modes: [{
			id:           1
			name:         "inactive"
			applications: []
			constraints:  []
		}, {
			id:           2
			name:         "active"
			applications: [4]
			constraints: [{
				kind: "requires_mode"
				other: {
					domain: 1
					mode:   2
				}
			}]
		}]
	}]
}
