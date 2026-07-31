// SPDX-License-Identifier: Apache-2.0

package ovf_exec_deployment

allocation: {
	schemaVersion: 1
	generation:    1
	units: {
		platform_storage: {
			id:         10
			name:       "platform_storage"
			kind:       "mount"
			bootstrap:  true
			mount: {
				source:     "/dev/disk/by-label/vehicle-data"
				target:     "/var/lib/vehicle"
				filesystem: "ext4"
				options:    ["nodev", "nosuid"]
			}
			readiness: "mount_present"
			startTimeoutMs: 10000
			stopTimeoutMs:  3000
			retry: {
				maxAttempts: 2
				delayMs:     100
			}
			dependencies:       []
			exclusiveResources: [110]
		}
		vehicle_network: {
			id:         11
			name:       "vehicle_network"
			kind:       "service"
			bootstrap:  false
			executable: "/usr/bin/ovf-vehicle-network"
			arguments:  []
			readiness:  "process_started"
			startTimeoutMs: 5000
			stopTimeoutMs:  3000
			retry: {
				maxAttempts: 2
				delayMs:     100
			}
			dependencies:       ["platform_storage"]
			exclusiveResources: [111]
		}
		camera: {
			id:         1
			name:       "camera"
			kind:       "managed_application"
			bootstrap:  false
			executable: "/usr/bin/ovf-camera"
			arguments:  []
			dependencies:       ["vehicle_network"]
			exclusiveResources: [101]
		}
		radar: {
			id:         2
			name:       "radar"
			kind:       "managed_application"
			bootstrap:  false
			executable: "/usr/bin/ovf-radar"
			arguments:  []
			dependencies:       ["vehicle_network"]
			exclusiveResources: [102]
		}
		sensor_fusion: {
			id:         3
			name:       "sensor_fusion"
			kind:       "managed_application"
			bootstrap:  false
			executable: "/usr/bin/ovf-sensor-fusion"
			arguments:  []
			dependencies:       ["camera", "radar"]
			exclusiveResources: []
		}
		driving_policy: {
			id:         4
			name:       "driving_policy"
			kind:       "managed_application"
			bootstrap:  false
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
