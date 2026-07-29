// SPDX-License-Identifier: Apache-2.0

package ovf_exec_deployment

platform: {
	dinit: {
		backendLibrary:    "/usr/lib/libovf_exec_backend_dinit.so"
		controlSocket:     "/run/ovf/exec/dinit.sock"
		servicesDirectory: "/etc/dinit.d"
		logBufferSize:     65536
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
