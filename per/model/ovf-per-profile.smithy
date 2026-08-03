// SPDX-License-Identifier: Apache-2.0

$version: "2"

namespace ovf.per.model

use ovf.model#EntityUuid
use smithy.api#range
use smithy.api#trait

@trait(selector: "structure")
structure persistentRecord {
    @required
    id: EntityUuid
    @required
    @range(min: 1)
    version: Long
    @required
    key: String
}
