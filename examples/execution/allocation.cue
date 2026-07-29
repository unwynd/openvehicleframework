// SPDX-License-Identifier: Apache-2.0

package ovf_exec_deployment

allocation: {
	schemaVersion: 1
	generation:    1
	applications: {
		camera: {
			id:         1
			executable: "/usr/bin/ovf-camera"
			arguments:  []
			dependencies:       []
			exclusiveResources: [101]
		}
		radar: {
			id:         2
			executable: "/usr/bin/ovf-radar"
			arguments:  []
			dependencies:       []
			exclusiveResources: [102]
		}
		sensor_fusion: {
			id:         3
			executable: "/usr/bin/ovf-sensor-fusion"
			arguments:  []
			dependencies:       ["camera", "radar"]
			exclusiveResources: []
		}
		driving_policy: {
			id:         4
			executable: "/usr/bin/ovf-driving-policy"
			arguments:  []
			dependencies:       ["sensor_fusion"]
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
			applications: []
			constraints:  []
		}, {
			id:           2
			name:         "operational"
			applications: ["camera", "radar", "sensor_fusion"]
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
			applications: ["driving_policy"]
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
