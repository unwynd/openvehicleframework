// SPDX-License-Identifier: Apache-2.0

$version: "2"

namespace example.camera

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

structure CameraObject {
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
}

@length(max: 64)
@ovfCollection(storage: "BOUNDED", capacity: 64)
list CameraObjects {
    member: CameraObject
}

structure CameraFrame {
    @required @ovfTag(value: 1)
    capturedAt: TimestampNs
    @required @ovfTag(value: 2)
    objects: CameraObjects
}

@ovfEvent(id: "1d8720d8-8a8e-45ac-a4f0-9ea233d3b9ec", tag: 1)
structure CameraObjectsChanged {
    @required @ovfTag(value: 1)
    value: CameraFrame
}

@ovfService(
    id: "55a3b341-d383-4e2a-aa67-1fa2d808f77d"
    events: [CameraObjectsChanged]
)
service CameraService {
    version: "1.0.0"
}
