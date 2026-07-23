// SPDX-License-Identifier: Apache-2.0

$version: "2"

namespace example.radar

use ovf.model#ovfCollection
use ovf.model#ovfEvent
use ovf.model#ovfField
use ovf.model#ovfInteger
use ovf.model#ovfMethod
use ovf.model#ovfService
use ovf.model#ovfTag
use smithy.api#error
use smithy.api#length
use smithy.api#required

@length(max: 128)
@ovfCollection(storage: "BOUNDED", capacity: 128)
string DiagnosticText

@ovfInteger(signed: false, bits: 64)
bigInteger TimestampNs

@ovfInteger(signed: false, bits: 16)
integer ObjectId

@ovfInteger(signed: false, bits: 8)
byte ConfidencePercent

structure RadarObject {
    @required @ovfTag(value: 1)
    id: ObjectId
    @required @ovfTag(value: 2)
    longitudinalMeters: Float
    @required @ovfTag(value: 3)
    lateralMeters: Float
    @required @ovfTag(value: 4)
    confidence: ConfidencePercent
}

@length(max: 64)
@ovfCollection(storage: "BOUNDED", capacity: 64)
list RadarObjects {
    member: RadarObject
}

structure RadarFrame {
    @required @ovfTag(value: 1)
    capturedAt: TimestampNs
    @required @ovfTag(value: 2)
    objects: RadarObjects
}

@ovfEvent(id: "851b0fc7-d41f-4e4f-9673-4be1aeed6f62", tag: 1)
structure RadarObjectsChanged {
    @required @ovfTag(value: 1)
    value: RadarFrame
}

structure VehicleState {
    @required @ovfTag(value: 1)
    speedMetersPerSecond: Float
}

@ovfField(
    id: "aac81bc6-8197-4b52-887c-c918630cbd2d"
    tag: 3
    readable: true
    writable: false
    notifiable: true
)
structure VehicleStateField {
    @required @ovfTag(value: 1)
    value: VehicleState
}

structure CalibrateInput {
    @required @ovfTag(value: 1)
    targetDistanceMeters: Float
}

structure CalibrateOutput {
    @required @ovfTag(value: 1)
    acceptedAt: TimestampNs
}

@error("client")
structure InvalidTarget {
    @required @ovfTag(value: 1)
    reason: DiagnosticText
}

@ovfMethod(
    id: "89dfe84f-1ab7-416d-891c-f603bc1f4557"
    tag: 2
    idempotent: false
    fireAndForget: false
)
operation Calibrate {
    input: CalibrateInput
    output: CalibrateOutput
    errors: [InvalidTarget]
}

@ovfService(
    id: "42bd2fc5-8f32-4e57-8a33-94fb0a5cf71d"
    events: [RadarObjectsChanged]
    fields: [VehicleStateField]
)
service RadarService {
    version: "1.0.0"
    operations: [Calibrate]
}
