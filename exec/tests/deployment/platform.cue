// SPDX-License-Identifier: Apache-2.0

package ovf_exec_deployment

platform: {
	dinit: {
		backendLibrary:       "/tmp/ovf-execd-e2e/lib/libovf_exec_backend_dinit.so"
		systemRecoveryService: "ovf-system-recovery"
		controlSocket:        "/tmp/ovf-execd-e2e/run/dinit.sock"
		servicesDirectory:    "/tmp/ovf-execd-e2e/daemon_deployment.dinit"
		logBufferSize:        65536
	}
	persistence: {
		journal:           "/tmp/ovf-execd-e2e/state/journal.v1"
		maximumRecordSize: 1048576
		synchronize:       true
	}
	coordinator: {
		socket:             "/tmp/ovf-execd-e2e/run/coordinator.sock"
		queueCapacity:      16
		workerCount:        2
		connectionCapacity: 16
		maximumMessageSize: 65536
		observationUids: [0, 501, 1000]
		mutationUids:    [0, 501, 1000]
	}
}
