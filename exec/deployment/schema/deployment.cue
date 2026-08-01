// SPDX-License-Identifier: Apache-2.0

package ovf_exec_deployment

import (
	"list"
	"strings"
)

#Id: int & >=1
#Name: string & =~"^[a-z][a-z0-9_-]*$"
#AbsolutePath: string & =~"^/[^\\x00]*$"
#OptionalAbsolutePath: "" | #AbsolutePath
#OptionalName: "" | #Name
#RelativeInstallPath: string & =~"^[a-zA-Z0-9._-]+(?:/[a-zA-Z0-9._-]+)*$"

#Unit: {
	id:         #Id
	name:       #Name
	kind:       "managed_application" | "service" | "one_shot" | "mount" | "external"
	bootstrap:  bool
	executable: #OptionalAbsolutePath
	arguments: [...string]
	nativeService: #OptionalName
	stopExecutable: #OptionalAbsolutePath
	stopArguments: [...string]
	readiness: "lifecycle_channel" | "process_started" | "supervisor_notification" |
		"successful_exit" | "socket_available" | "mount_present"
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
	executableRelativePath: #RelativeInstallPath
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
	units:        [...#Id]
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
	units:        [#Name]: #AllocatedUnit
	domains:      [#AllocatedDomain, ...#AllocatedDomain]
}

#AllocatedUnit: {
	id:         #Id
	name:       #Name
	kind:       "managed_application" | "service" | "one_shot" | "mount" | "external"
	bootstrap:  *false | bool
	executable: *"" | #AbsolutePath
	arguments: *[] | [...string]
	nativeService: *"" | #Name
	stopExecutable: *"" | #AbsolutePath
	stopArguments: *[] | [...string]
	readiness?: "process_started" | "supervisor_notification" | "successful_exit" |
		"socket_available" | "mount_present"
	startTimeoutMs?: int & >=1
	stopTimeoutMs?:  int & >=1
	retry?: {
		maxAttempts: int & >=1 & <=16
		delayMs:     int & >=0
	}
	mount?: {
		source:     #AbsolutePath
		target:     #AbsolutePath
		filesystem: string & =~"^[a-zA-Z0-9._-]+$"
		options:    [string & =~"^[a-zA-Z0-9._=-]+$", ...string & =~"^[a-zA-Z0-9._=-]+$"]
	}
	dependencies:       [...#Name]
	exclusiveResources: [...#Id]
	if kind == "managed_application" {
		executable: ""
	}
	if kind != "managed_application" && kind != "external" && kind != "mount" {
		readiness:       _
		startTimeoutMs:  _
		stopTimeoutMs:   _
		retry:           _
	}
	if kind == "mount" {
		mount:           _
		readiness:       "mount_present"
		startTimeoutMs:  _
		stopTimeoutMs:   _
		retry:           _
	}
	if kind == "external" {
		nativeService:  #Name
		readiness:      _
		startTimeoutMs: _
		stopTimeoutMs:  _
		retry:          _
	}
}

#AllocatedMode: {
	id:           #Id
	name:         #Name
	units:        [...#Name]
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
		daemonExecutable:    #AbsolutePath
		executionModel:      #AbsolutePath
		backendConfiguration: #AbsolutePath
		deploymentManifest:  #AbsolutePath
		backendLibrary:    #AbsolutePath
		systemRecoveryService: #Name
		controlSocket:     #AbsolutePath
		servicesDirectory: #AbsolutePath
		logBufferSize:     int & >=4096
		mountExecutable:   #AbsolutePath
		unmountExecutable: #AbsolutePath
		nativeServices:    [...#Name]
		applicationMountPoint: #AbsolutePath
		units: [#Name]: {
			executable:     #AbsolutePath
			arguments:      [...string]
			stopExecutable: #OptionalAbsolutePath
			stopArguments:  [...string]
		}
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
	units: list.Concat([[for fragment in applicationValues {
		let assigned = allocationValue.units[fragment.name]
		id:         assigned.id
		name:       fragment.name
		kind:       assigned.kind
		bootstrap:  assigned.bootstrap
		executable: strings.TrimSuffix(platformValue.dinit.applicationMountPoint, "/") + "/" + fragment.executableRelativePath
		arguments:  assigned.arguments
		stopExecutable: assigned.stopExecutable
		stopArguments:  assigned.stopArguments
		nativeService: assigned.nativeService
		readiness:  fragment.execution.readiness
		startTimeoutMs: fragment.execution.startup.timeoutMs
		stopTimeoutMs:  fragment.execution.shutdown.timeoutMs
		retry:           fragment.execution.restart
		dependencies: [for dependency in assigned.dependencies {
			allocationValue.units[dependency].id
		}]
		exclusiveResources: assigned.exclusiveResources
	}], [for _, assigned in allocationValue.units if assigned.kind != "managed_application" {
		id:         assigned.id
		name:       assigned.name
		kind:       assigned.kind
		bootstrap:  assigned.bootstrap
		if assigned.kind == "mount" {
			executable: platformValue.dinit.mountExecutable
			arguments: list.Concat([["-t", assigned.mount.filesystem],
				["-o", strings.Join(assigned.mount.options, ",")],
				[assigned.mount.source, assigned.mount.target]])
			stopExecutable: platformValue.dinit.unmountExecutable
			stopArguments:  [assigned.mount.target]
		}
		if assigned.kind != "mount" {
			if assigned.kind == "service" || assigned.kind == "one_shot" {
				executable:     platformValue.dinit.units[assigned.name].executable
				arguments:      platformValue.dinit.units[assigned.name].arguments
				stopExecutable: platformValue.dinit.units[assigned.name].stopExecutable
				stopArguments:  platformValue.dinit.units[assigned.name].stopArguments
			}
			if assigned.kind == "external" {
				executable:     ""
				arguments:      []
				stopExecutable: ""
				stopArguments:  []
			}
		}
		nativeService: assigned.nativeService
		readiness:       assigned.readiness
		startTimeoutMs:  assigned.startTimeoutMs
		stopTimeoutMs:   assigned.stopTimeoutMs
		retry:           assigned.retry
		dependencies: [for dependency in assigned.dependencies {
			allocationValue.units[dependency].id
		}]
		exclusiveResources: assigned.exclusiveResources
	}]])
	domains: [for domain in allocationValue.domains {
		id:          domain.id
		name:        domain.name
		initialMode: domain.initialMode
		replacement: domain.replacement
		recovery:    domain.recovery
		modes: [for mode in domain.modes {
			id:   mode.id
			name: mode.name
			units: [for unit in mode.units {
				allocationValue.units[unit].id
			}]
			constraints: mode.constraints
		}]
	}]
	platform:          platformValue
}
