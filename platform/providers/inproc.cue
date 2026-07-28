// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

platforms: [{
	transport: "ipc"
	profile:   "inproc"
	provider:  "local"
	required:  true
	extensions: "org.openvehicleframework.inproc": {
		queueCapacity: 64
	}
}]
