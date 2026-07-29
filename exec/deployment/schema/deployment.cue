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

#Deployment: {
	deploymentVersion: 1
	generation:        #Id
	applications:      [#Application, ...#Application]
	domains:           [#Domain, ...#Domain]
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

deploymentValue=deployment: #Deployment
platformValue=platform: #Platform

model: {
	deploymentVersion: deploymentValue.deploymentVersion
	generation:        deploymentValue.generation
	applications:      deploymentValue.applications
	domains:           deploymentValue.domains
	platform:          platformValue
}
