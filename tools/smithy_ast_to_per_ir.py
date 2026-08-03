#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Compile an assembled Smithy model into canonical persistent-record IR."""

from __future__ import annotations

import re

from tools.smithy_ast_to_ir import (
    BUILTINS,
    COLLECTION_TRAIT,
    INTEGER_TRAIT,
    REQUIRED_TRAIT,
    TAG_TRAIT,
    local,
    target,
    type_ref,
)

RECORD_TRAIT = "ovf.per.model#persistentRecord"


def compile_per_model(ast: dict) -> dict:
    shapes: dict[str, dict] = ast["shapes"]
    records = [
        shape_id
        for shape_id, shape in shapes.items()
        if RECORD_TRAIT in shape.get("traits", {})
    ]
    if not records:
        raise ValueError("at least one persistent record is required")
    namespaces = {shape_id.split("#", 1)[0] for shape_id in records}
    if len(namespaces) != 1:
        raise ValueError("one persistent contract may contain only one namespace")
    namespace = next(iter(namespaces))
    types = []
    for shape_id, shape in sorted(shapes.items()):
        if not shape_id.startswith(namespace + "#"):
            continue
        name, kind, traits = local(shape_id), shape["type"], shape.get("traits", {})
        if kind in ("byte", "short", "integer", "long", "bigInteger"):
            integer = traits.get(INTEGER_TRAIT)
            if integer is None:
                raise ValueError(f"{shape_id}: exact integer trait required")
            types.append({"kind": "integer", "name": name, **integer})
        elif kind == "string":
            collection = traits.get(COLLECTION_TRAIT)
            if collection is None or collection.get("storage") != "BOUNDED":
                raise ValueError(f"{shape_id}: bounded collection trait required")
            types.append({"kind": "string", "name": name, "capacity": collection["capacity"]})
        elif kind == "structure":
            members = []
            tags = set()
            for member_name, member in shape.get("members", {}).items():
                tag = member.get("traits", {}).get(TAG_TRAIT)
                if tag is None:
                    raise ValueError(f"{shape_id}${member_name}: stable tag required")
                value = tag["value"]
                if value in tags:
                    raise ValueError(f"{shape_id}: duplicate field tag {value}")
                tags.add(value)
                members.append({
                    "name": member_name,
                    "tag": value,
                    "type": type_ref(target(member)),
                    "required": REQUIRED_TRAIT in member.get("traits", {}),
                })
            types.append({"kind": "struct", "name": name,
                          "members": sorted(members, key=lambda item: item["tag"])})
        else:
            raise ValueError(f"{shape_id}: unsupported persistent shape type {kind}")
    record_models = []
    keys = set()
    identifiers = set()
    for shape_id in sorted(records):
        trait = shapes[shape_id]["traits"][RECORD_TRAIT]
        key = trait["key"]
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_.-]{0,127}", key):
            raise ValueError(f"{shape_id}: invalid persistent key")
        if key in keys or trait["id"] in identifiers:
            raise ValueError("persistent record keys and identifiers must be unique")
        keys.add(key)
        identifiers.add(trait["id"])
        record_models.append({"name": local(shape_id), **trait})
    return {
        "perIrVersion": 1,
        "namespace": namespace,
        "records": record_models,
        "types": types,
    }
