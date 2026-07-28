// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

#Feature: "discovery" | "events" | "methods" | "loans" | "scatterGather" |
	"reliable" | "ordered" | "deadlines" | "cancellation"

#Requirements: {
	features?: [...#Feature]
	limits?: {
		maxPayloadSize?:          int & >=1
		maxHistoryDepth?:         int & >=1
		maxOutstandingOperations?: int & >=1
		maxEndpoints?:            int & >=1
	}
}

#Instance: {
	interface: string & =~"^[A-Za-z_][A-Za-z0-9_.]*#[A-Za-z_][A-Za-z0-9_]*$"
	instance:  string & =~"^[a-z][a-z0-9-]*$"
	role:      "consumer" | "provider"
	transport: "ipc" | "network"
	requirements?: #Requirements
}

#Deployment: {
	deploymentVersion: 1
	instances:         [#Instance, ...#Instance]
}

#Platform: {
	transport: "ipc" | "network"
	profile:   "inproc" | "iceoryx2" | "vsomeip" | "cyclonedds"
	provider:  string & =~"^[a-z][a-z0-9_-]*$"
	required:  bool
	extensions?: [string]: _
}

deploymentValue=deployment: #Deployment
platformValues=platforms:   [#Platform, ...#Platform]

model: {
	deployment: deploymentValue
	platforms:  platformValues
}
