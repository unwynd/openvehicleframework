// SPDX-License-Identifier: Apache-2.0

package ovf_exec_deployment

platform: {
	dinit: {
		daemonExecutable:       "/var/tmp/ovf-exec-pipeline/usr/sbin/ovf-execd"
		executionModel:         "/var/tmp/ovf-exec-pipeline/etc/ovf/exec/deployment.execution.json"
		backendConfiguration:   "/var/tmp/ovf-exec-pipeline/etc/ovf/exec/deployment.backend.json"
		deploymentManifest:     "/var/tmp/ovf-exec-pipeline/usr/share/ovf/exec/manifest.json"
		backendLibrary:         "/var/tmp/ovf-exec-pipeline/usr/lib/libovf_exec_backend_dinit.so"
		systemRecoveryService: "ovf-system-recovery"
		controlSocket:          "/var/tmp/ovf-exec-pipeline/run/dinit.sock"
		servicesDirectory:      "/var/tmp/ovf-exec-pipeline/etc/dinit.d"
		logBufferSize:          1048576
		mountExecutable:        "/bin/mount"
		unmountExecutable:      "/bin/umount"
		nativeServices:         []
		applicationMountPoint:  "/var/tmp/ovf-exec-pipeline"
		units: vehicle_network: {
			executable:     "/var/tmp/ovf-exec-pipeline/usr/bin/routingmanagerd"
			arguments:      []
			stopExecutable: ""
			stopArguments:  []
		}
	}
	persistence: {
		journal:           "/var/tmp/ovf-exec-pipeline/var/lib/ovf/exec/journal.v1"
		maximumRecordSize: 1048576
		synchronize:       true
	}
	coordinator: {
		socket:             "/var/tmp/ovf-exec-pipeline/run/coordinator.sock"
		queueCapacity:      128
		workerCount:        4
		connectionCapacity: 128
		maximumMessageSize: 1048576
		observationUids: [0, 1000]
		mutationUids:    [0, 1000]
	}
}
