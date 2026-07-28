// SPDX-License-Identifier: Apache-2.0

$version: "2"

namespace example.environment

use ovf.model#ovfCollection
use ovf.model#ovfEvent
use ovf.model#ovfInteger
use ovf.model#ovfService
use ovf.model#ovfTag
use smithy.api#length
use smithy.api#required

@ovfInteger(signed: false, bits: 64)
bigInteger TimestampNs

@ovfInteger(signed: false, bits: 16)
integer TrackId

@ovfInteger(signed: false, bits: 8)
byte ConfidencePercent

@length(max: 32)
@ovfCollection(storage: "BOUNDED", capacity: 32)
string Classification

structure FusedObject {
    @required @ovfTag(value: 1)
    id: TrackId
    @required @ovfTag(value: 2)
    classification: Classification
    @required @ovfTag(value: 3)
    longitudinalMeters: Float
    @required @ovfTag(value: 4)
    lateralMeters: Float
    @required @ovfTag(value: 5)
    confidence: ConfidencePercent
    @required @ovfTag(value: 6)
    observedByRadar: Boolean
    @required @ovfTag(value: 7)
    observedByCamera: Boolean
}

@length(max: 128)
@ovfCollection(storage: "BOUNDED", capacity: 128)
list FusedObjects {
    member: FusedObject
}

structure EnvironmentModel {
    @required @ovfTag(value: 1)
    producedAt: TimestampNs
    @required @ovfTag(value: 2)
    objects: FusedObjects
}

@ovfEvent(id: "19a61d89-4f40-48d7-b234-afaf556d7ef0", tag: 1)
structure EnvironmentModelChanged {
    @required @ovfTag(value: 1)
    value: EnvironmentModel
}

@ovfService(
    id: "aa6eefb7-845b-49a7-bc12-320805b82dc9"
    events: [EnvironmentModelChanged]
)
service EnvironmentModelService {
    version: "1.0.0"
}
