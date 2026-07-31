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

#Application: {
	schemaVersion: 1
	name:          string & =~"^[a-z][a-z0-9_-]*$"
	communication: {
		instances: [#Instance, ...#Instance]
	}
	execution: {
		readiness: *"lifecycle_channel" | "process_started"
		startup: timeoutMs: int & >=1
		shutdown: timeoutMs: int & >=1
		restart: {
			maxAttempts: int & >=1 & <=16
			delayMs:     int & >=0
		}
	}
}

#Platform: {
	transport: "ipc" | "network"
	profile:   "inproc" | "iceoryx2" | "vsomeip" | "cyclonedds"
	provider:  string & =~"^[a-z][a-z0-9_-]*$"
	required:  bool
	extensions?: [string]: _
}

applicationValue=application: #Application
platformValues=platforms:   [#Platform, ...#Platform]

model: {
	deployment: {
		deploymentVersion: applicationValue.schemaVersion
		instances:         applicationValue.communication.instances
	}
	platforms:  platformValues
}
