// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

#Level: "fatal" | "error" | "warning" | "info" | "debug" | "trace"

#Logger: {
	name:        string & =~"^[A-Za-z][A-Za-z0-9_.-]{0,62}$"
	description: string & =~"^.{1,255}$" | *name
}

#Logging: {
	loggers:         [...#Logger] & [_, ...]
	queueCapacity:   int & >=2 & <=65536 | *4096
	criticalReserve: int & >=1 & <queueCapacity | *64
	producerWaitMs:  int & >=0 & <=1000 | *5
	shutdownFlushMs: int & >=0 & <=30000 | *250
	initialLevel:    #Level | *"info"
}

#Application: {
	name:    string & =~"^[A-Za-z][A-Za-z0-9_.-]{0,62}$"
	logging: #Logging
	...
}

#Binding: {
	provider: "dlt"
	verbose:  bool | *false
}

application: #Application
binding:     #Binding
