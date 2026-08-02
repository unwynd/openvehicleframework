// SPDX-License-Identifier: Apache-2.0

package ovf_exec_deployment

allocation: {
	schemaVersion: 1
	generation:    1
	units: {
		diagnostic_log: {
			id:         12
			name:       "diagnostic_log"
			kind:       "service"
			bootstrap:  true
			readiness:  "process_started"
			startTimeoutMs: 5000
			stopTimeoutMs:  3000
			retry: {
				maxAttempts: 2
				delayMs:     100
			}
			dependencies:       []
			exclusiveResources: [112]
		}
		vehicle_network: {
			id:         11
			name:       "vehicle_network"
			kind:       "service"
			bootstrap:  false
			readiness:  "process_started"
			startTimeoutMs: 5000
			stopTimeoutMs:  3000
			retry: {
				maxAttempts: 2
				delayMs:     100
			}
			dependencies:       []
			exclusiveResources: [111]
		}
		camera: {
			id:         1
			name:       "camera"
			kind:       "managed_application"
			bootstrap:  false
			arguments:  []
			dependencies:       ["diagnostic_log", "vehicle_network"]
			exclusiveResources: [101]
		}
		radar: {
			id:         2
			name:       "radar"
			kind:       "managed_application"
			bootstrap:  false
			arguments:  []
			dependencies:       ["diagnostic_log", "vehicle_network"]
			exclusiveResources: [102]
		}
		sensor_fusion: {
			id:         3
			name:       "sensor_fusion"
			kind:       "managed_application"
			bootstrap:  false
			arguments:  []
			dependencies:       ["camera", "diagnostic_log", "radar"]
			exclusiveResources: []
		}
		driving_policy: {
			id:         4
			name:       "driving_policy"
			kind:       "managed_application"
			bootstrap:  false
			arguments:  []
			dependencies:       ["diagnostic_log", "sensor_fusion"]
			exclusiveResources: []
		}
	}
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
			units:       []
			constraints:  []
		}, {
			id:           2
			name:         "operational"
			units: ["vehicle_network", "camera", "radar", "sensor_fusion"]
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
			units:       []
			constraints:  []
		}, {
			id:           2
			name:         "active"
			units:       ["driving_policy"]
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
