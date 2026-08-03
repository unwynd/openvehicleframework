// SPDX-License-Identifier: Apache-2.0

$version: "2"

namespace example.state

use ovf.model#ovfCollection
use ovf.model#ovfInteger
use ovf.model#ovfTag
use ovf.per.model#persistentRecord
use smithy.api#length
use smithy.api#required

@length(max: 24)
@ovfCollection(storage: "BOUNDED", capacity: 24)
string ModeText

@ovfInteger(signed: false, bits: 64)
long Sequence

@persistentRecord(
    id: "7b5a1411-c98e-4bcc-b672-443415c156f1"
    version: 1
    key: "operational-state"
)
structure OperationalState {
    @required @ovfTag(value: 1)
    sequence: Sequence
    @required @ovfTag(value: 2)
    mode: ModeText
    @ovfTag(value: 3)
    reason: ModeText
}
