// SPDX-License-Identifier: Apache-2.0

$version: "2"

namespace ovf.model

use smithy.api#idRef
use smithy.api#length
use smithy.api#pattern
use smithy.api#range
use smithy.api#trait

@pattern("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$")
string EntityUuid

@idRef
string ShapeReference

list ShapeReferences {
    member: ShapeReference
}

enum CollectionStorage {
    FIXED
    BOUNDED
    UNBOUNDED
}

@trait(selector: "service")
structure ovfService {
    @required
    id: EntityUuid
    events: ShapeReferences
    fields: ShapeReferences
}

@trait(selector: "operation")
structure ovfMethod {
    @required
    id: EntityUuid
    @required
    tag: Long
    idempotent: Boolean
    fireAndForget: Boolean
}

@trait(selector: "structure")
structure ovfEvent {
    @required
    id: EntityUuid
    @required
    tag: Long
}

@trait(selector: "structure")
structure ovfField {
    @required
    id: EntityUuid
    @required
    tag: Long
    readable: Boolean
    writable: Boolean
    notifiable: Boolean
}

@trait(selector: "structure > member")
structure ovfTag {
    @required
    @range(min: 1, max: 4294967295)
    value: Long
}

@trait(selector: ":is(byte, short, integer, long)")
structure ovfInteger {
    @required
    signed: Boolean
    @required
    @range(min: 8, max: 64)
    bits: Byte
}

@trait(selector: ":is(list, map, blob, string)")
structure ovfCollection {
    @required
    storage: CollectionStorage
    capacity: Long
}
