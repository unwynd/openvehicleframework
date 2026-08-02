// SPDX-License-Identifier: Apache-2.0

package ovf_exec_deployment

platform: {
	dinit: {
		daemonExecutable:    "/usr/sbin/ovf-execd"
		executionModel:      "/etc/ovf/exec/deployment.execution.json"
		backendConfiguration: "/etc/ovf/exec/deployment.backend.json"
		deploymentManifest:  "/usr/share/ovf/exec/manifest.json"
		backendLibrary:    "/usr/lib/libovf_exec_backend_dinit.so"
		systemRecoveryService: "ovf-system-recovery"
		controlSocket:     "/run/ovf/exec/dinit.sock"
		servicesDirectory: "/etc/dinit.d"
		logBufferSize:     65536
		mountExecutable:   "/bin/mount"
		unmountExecutable: "/bin/umount"
		nativeServices:    []
		applicationMountPoint: "/"
		units: vehicle_network: {
			executable:     "/usr/bin/routingmanagerd"
			arguments:      []
			stopExecutable: ""
			stopArguments:  []
		}
		units: diagnostic_log: {
			executable:     "/usr/bin/dlt-daemon"
			arguments:      ["-t", "/run/dlt", "-c", "/etc/dlt.conf"]
			stopExecutable: ""
			stopArguments:  []
		}
	}
	persistence: {
		journal:           "/var/lib/ovf/exec/journal.v1"
		maximumRecordSize: 1048576
		synchronize:       true
	}
	coordinator: {
		socket:        "/run/ovf/exec/coordinator.sock"
		queueCapacity: 128
		workerCount:   4
		connectionCapacity: 128
		maximumMessageSize: 1048576
		observationUids: [0]
		mutationUids:    [0]
	}
}
