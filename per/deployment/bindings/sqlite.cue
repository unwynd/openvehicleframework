// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

binding: {
	provider: "sqlite"
	configuration: {
		root:          "/var/lib/ovf/per"
		journalMode:   "wal"
		busyTimeoutMs: 5000
	}
}
