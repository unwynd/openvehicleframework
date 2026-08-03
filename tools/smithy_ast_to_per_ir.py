#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Compile an assembled Smithy model into canonical persistent-record IR."""

from __future__ import annotations

import re

from tools.smithy_types import compile_types, local

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
    types = compile_types(shapes, namespace, persistent=True)
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
