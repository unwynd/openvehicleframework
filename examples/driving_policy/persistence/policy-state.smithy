// SPDX-License-Identifier: Apache-2.0

$version: "2"

namespace example.driving_policy

use ovf.model#ovfInteger
use ovf.model#ovfTag
use ovf.per.model#persistentRecord
use smithy.api#required

@ovfInteger(signed: false, bits: 64)
long Sequence

@ovfInteger(signed: false, bits: 64)
long Timestamp

@ovfInteger(signed: false, bits: 32)
integer ObjectCount

@persistentRecord(
    id: "e4627558-8f16-4c36-8363-51cddc8e5533"
    version: 1
    key: "environment-observation"
)
structure PolicyState {
    @required @ovfTag(value: 1)
    sequence: Sequence
    @required @ovfTag(value: 2)
    objectCount: ObjectCount
    @required @ovfTag(value: 3)
    producedAt: Timestamp
}
