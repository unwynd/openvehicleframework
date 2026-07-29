// SPDX-License-Identifier: Apache-2.0

package ovf_exec_deployment

#Id: int & >=1
#Name: string & =~"^[a-z][a-z0-9_-]*$"
#AbsolutePath: string & =~"^/[^\\x00]*$"

#Application: {
	id:         #Id
	name:       #Name
	executable: #AbsolutePath
	arguments: [...string]
	readiness: *"required" | "process_started"
	startTimeoutMs: int & >=1
	stopTimeoutMs:  int & >=1
	retry: {
		maxAttempts: int & >=1 & <=16
		delayMs:     int & >=0
	}
	dependencies:      [...#Id]
	exclusiveResources: [...#Id]
}

#ApplicationFragment: {
	schemaVersion: 1
	name:          #Name
	execution: {
		readiness: *"required" | "process_started"
		startup: timeoutMs: int & >=1
		shutdown: timeoutMs: int & >=1
		restart: {
			maxAttempts: int & >=1 & <=16
			delayMs:     int & >=0
		}
	}
}

#Constraint: {
	kind: "requires_mode" | "excludes_mode"
	other: {
		domain: #Id
		mode:   #Id
	}
}

#Mode: {
	id:           #Id
	name:         #Name
	applications: [...#Id]
	constraints:  [...#Constraint]
}

#Domain: {
	id:          #Id
	name:        #Name
	initialMode: #Id
	replacement: *"supersede_if_safe" | "reject_while_busy" | "queue"
	recovery: {
		action: *"hold_observed_configuration" | "enter_fallback_mode" |
			"stop_domain" | "request_system_recovery"
		fallbackMode?: #Id
		deadlineMs:    int & >=1
	}
	modes: [#Mode, ...#Mode]
}

#Allocation: {
	schemaVersion: 1
	generation:    #Id
	applications: [#Name]: #AllocatedApplication
	domains:      [#AllocatedDomain, ...#AllocatedDomain]
}

#AllocatedApplication: {
	id:         #Id
	executable: #AbsolutePath
	arguments: [...string]
	dependencies:       [...#Name]
	exclusiveResources: [...#Id]
}

#AllocatedMode: {
	id:           #Id
	name:         #Name
	applications: [...#Name]
	constraints:  [...#Constraint]
}

#AllocatedDomain: {
	id:          #Id
	name:        #Name
	initialMode: #Id
	replacement: *"supersede_if_safe" | "reject_while_busy" | "queue"
	recovery: {
		action: *"hold_observed_configuration" | "enter_fallback_mode" |
			"stop_domain" | "request_system_recovery"
		fallbackMode?: #Id
		deadlineMs:    int & >=1
	}
	modes: [#AllocatedMode, ...#AllocatedMode]
}

#Platform: {
	dinit: {
		backendLibrary:    #AbsolutePath
		systemRecoveryService: #Name
		controlSocket:     #AbsolutePath
		servicesDirectory: #AbsolutePath
		logBufferSize:     int & >=4096
	}
	persistence: {
		journal:           #AbsolutePath
		maximumRecordSize: int & >=4096 & <=16777216
		synchronize:       bool
	}
	coordinator: {
		socket:        #AbsolutePath
		queueCapacity: int & >=1 & <=4096
		workerCount:   int & >=1 & <=64
		connectionCapacity: int & >=1 & <=4096
		maximumMessageSize: int & >=4096 & <=16777216
		observationUids: [...int & >=0 & <=4294967295]
		mutationUids:    [...int & >=0 & <=4294967295]
	}
}

allocationValue=allocation: #Allocation
applicationValues=applicationFragments: [#ApplicationFragment, ...#ApplicationFragment]
platformValue=platform: #Platform

model: {
	deploymentVersion: allocationValue.schemaVersion
	generation:        allocationValue.generation
	applications: [for fragment in applicationValues {
		let assigned = allocationValue.applications[fragment.name]
		id:         assigned.id
		name:       fragment.name
		executable: assigned.executable
		arguments:  assigned.arguments
		readiness:  fragment.execution.readiness
		startTimeoutMs: fragment.execution.startup.timeoutMs
		stopTimeoutMs:  fragment.execution.shutdown.timeoutMs
		retry:           fragment.execution.restart
		dependencies: [for dependency in assigned.dependencies {
			allocationValue.applications[dependency].id
		}]
		exclusiveResources: assigned.exclusiveResources
	}]
	domains: [for domain in allocationValue.domains {
		id:          domain.id
		name:        domain.name
		initialMode: domain.initialMode
		replacement: domain.replacement
		recovery:    domain.recovery
		modes: [for mode in domain.modes {
			id:   mode.id
			name: mode.name
			applications: [for application in mode.applications {
				allocationValue.applications[application].id
			}]
			constraints: mode.constraints
		}]
	}]
	platform:          platformValue
}
