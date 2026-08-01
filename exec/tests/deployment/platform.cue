// SPDX-License-Identifier: Apache-2.0

package ovf_exec_deployment

platform: {
	dinit: {
		daemonExecutable:       "/var/tmp/ovf-execd-e2e/bin/ovf-execd"
		executionModel:         "/var/tmp/ovf-execd-e2e/artifacts/deployment.execution.json"
		backendConfiguration:   "/var/tmp/ovf-execd-e2e/artifacts/daemon_deployment.backend.json"
		deploymentManifest:     "/var/tmp/ovf-execd-e2e/artifacts/deployment.manifest.json"
		backendLibrary:       "/var/tmp/ovf-execd-e2e/lib/libovf_exec_backend_dinit.so"
		systemRecoveryService: "ovf-system-recovery"
		controlSocket:        "/var/tmp/ovf-execd-e2e/run/dinit.sock"
		servicesDirectory:    "/var/tmp/ovf-execd-e2e/daemon_deployment.dinit"
		logBufferSize:        65536
		mountExecutable:      "/bin/mount"
		unmountExecutable:    "/bin/umount"
		nativeServices:       []
		applicationMountPoint: "/var/tmp/ovf-execd-e2e"
		units: system_test: {
			executable:     "/var/tmp/ovf-execd-e2e/bin/system-test"
			arguments:      []
			stopExecutable: ""
			stopArguments:  []
		}
	}
	persistence: {
		journal:           "/var/tmp/ovf-execd-e2e/state/journal.v1"
		maximumRecordSize: 1048576
		synchronize:       true
	}
	coordinator: {
		socket:             "/var/tmp/ovf-execd-e2e/run/coordinator.sock"
		queueCapacity:      16
		workerCount:        2
		connectionCapacity: 16
		maximumMessageSize: 65536
		observationUids: [0, 501, 1000]
		mutationUids:    [0, 501, 1000]
	}
}
