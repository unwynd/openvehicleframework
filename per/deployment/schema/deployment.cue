// SPDX-License-Identifier: Apache-2.0

package ovf_deployment

#Durability: "buffered" | "process_crash" | "media"
#Access: "read_only" | "read_write"

#Store: {
	name:              string & =~"^[A-Za-z][A-Za-z0-9_.-]{0,127}$"
	access:            #Access | *"read_write"
	minimumDurability: #Durability | *"process_crash"
	capacityBytes:     int & >=4096 & <=1099511627776
	maxEntries:        int & >=1 & <=16777216
	maxKeySize:        int & >=1 & <=4096 | *256
	maxValueSize:      int & >=1 & <=67108864 | *65536
	maxBlobSize:       int & >=1 & <=1099511627776 | *16777216
}

#Persistence: {
	stores: [...#Store] & [_, ...]
}

#Application: {
	name:        string & =~"^[A-Za-z][A-Za-z0-9_.-]{0,62}$"
	persistence: #Persistence
	...
}

#Binding: {
	provider: "sqlite"
	configuration: {
		root:          string
		journalMode:   "persist" | "wal" | *"wal"
		busyTimeoutMs: int & >=0 & <=60000 | *5000
	}
}

application: #Application
binding:     #Binding
