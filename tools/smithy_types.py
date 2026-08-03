#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Shared bounded Smithy type-model validation and canonical IR extraction."""

from __future__ import annotations

BUILTINS = {
    "smithy.api#Boolean": "bool",
    "smithy.api#Float": "f32",
    "smithy.api#Double": "f64",
    "smithy.api#Blob": "bytes",
    "smithy.api#String": "string",
}
COLLECTION_TRAIT = "ovf.model#ovfCollection"
INTEGER_TRAIT = "ovf.model#ovfInteger"
LENGTH_TRAIT = "smithy.api#length"
REQUIRED_TRAIT = "smithy.api#required"
TAG_TRAIT = "ovf.model#ovfTag"
INTEGER_WIDTHS = {"byte": 8, "short": 16, "integer": 32, "long": 64}


def local(shape_id: str) -> str:
    return shape_id.split("#", 1)[1]


def target(value: dict) -> str:
    return value["target"]


def type_ref(shape_id: str) -> str:
    return BUILTINS.get(shape_id, local(shape_id))


def _bounded_collection(shape_id: str, traits: dict, *, persistent: bool) -> dict:
    collection = traits.get(COLLECTION_TRAIT)
    if collection is None:
        raise ValueError(f"{shape_id}: bounded collection trait required")
    storage = collection.get("storage", "").lower()
    if storage not in {"bounded", "fixed", "dynamic"}:
        raise ValueError(f"{shape_id}: invalid collection storage policy")
    if persistent and storage != "bounded":
        raise ValueError(f"{shape_id}: persistent collections must use bounded storage")
    capacity = collection.get("capacity")
    if storage in {"bounded", "fixed"} and (not isinstance(capacity, int) or capacity <= 0):
        raise ValueError(f"{shape_id}: positive collection capacity required")
    length = traits.get(LENGTH_TRAIT, {})
    maximum = length.get("max")
    if maximum is not None and capacity is not None and maximum != capacity:
        raise ValueError(f"{shape_id}: @length max and storage capacity must agree")
    if persistent and maximum is None:
        raise ValueError(f"{shape_id}: persistent collection requires @length(max)")
    result = {"storage": storage}
    if capacity is not None:
        result["capacity"] = capacity
    return result


def compile_types(
    shapes: dict[str, dict],
    namespace: str,
    *,
    excluded: set[str] | None = None,
    persistent: bool = False,
) -> list[dict]:
    """Compile namespace-local data shapes into one canonical bounded type IR."""
    excluded = excluded or set()
    types: list[dict] = []
    for shape_id, shape in sorted(shapes.items()):
        if not shape_id.startswith(namespace + "#") or shape_id in excluded:
            continue
        name = local(shape_id)
        kind = shape["type"]
        traits = shape.get("traits", {})
        if kind in ("service", "operation", "resource", "union", "enum", "intEnum"):
            continue
        if kind in INTEGER_WIDTHS or kind == "bigInteger":
            integer = traits.get(INTEGER_TRAIT)
            if integer is None:
                raise ValueError(f"{shape_id}: exact integer trait required")
            if kind == "bigInteger":
                raise ValueError(f"{shape_id}: bigInteger cannot represent a fixed-width value")
            if integer.get("bits") != INTEGER_WIDTHS[kind]:
                raise ValueError(f"{shape_id}: integer trait width must match Smithy {kind}")
            types.append(
                {
                    "kind": "integer",
                    "name": name,
                    "signed": integer["signed"],
                    "bits": integer["bits"],
                }
            )
        elif kind == "string":
            types.append({"kind": "string", "name": name, **_bounded_collection(
                shape_id, traits, persistent=persistent
            )})
        elif kind == "list":
            types.append(
                {
                    "kind": "sequence",
                    "name": name,
                    "element": type_ref(target(shape["member"])),
                    **_bounded_collection(shape_id, traits, persistent=persistent),
                }
            )
        elif kind == "structure":
            members = []
            tags: set[int] = set()
            for member_name, member in shape.get("members", {}).items():
                tag = member.get("traits", {}).get(TAG_TRAIT)
                if tag is None:
                    raise ValueError(f"{shape_id}${member_name}: stable tag required")
                value = tag["value"]
                if value in tags:
                    raise ValueError(f"{shape_id}: duplicate field tag {value}")
                tags.add(value)
                members.append(
                    {
                        "name": member_name,
                        "tag": value,
                        "type": type_ref(target(member)),
                        "required": REQUIRED_TRAIT in member.get("traits", {}),
                    }
                )
            types.append(
                {"kind": "struct", "name": name, "members": sorted(members, key=lambda item: item["tag"])}
            )
        else:
            raise ValueError(f"{shape_id}: unsupported Smithy shape type {kind}")
    return types
