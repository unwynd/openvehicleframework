#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Compile an assembled Smithy JSON AST into canonical OVF communication IR."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from tools.validate_ir import normalized, validate

SERVICE_TRAIT = "ovf.model#ovfService"
METHOD_TRAIT = "ovf.model#ovfMethod"
EVENT_TRAIT = "ovf.model#ovfEvent"
FIELD_TRAIT = "ovf.model#ovfField"
TAG_TRAIT = "ovf.model#ovfTag"
INTEGER_TRAIT = "ovf.model#ovfInteger"
COLLECTION_TRAIT = "ovf.model#ovfCollection"
REQUIRED_TRAIT = "smithy.api#required"
BUILTINS = {
    "smithy.api#Boolean": "bool", "smithy.api#Float": "f32",
    "smithy.api#Double": "f64", "smithy.api#Blob": "bytes",
    "smithy.api#String": "string"
}


def local(shape_id: str) -> str: return shape_id.split("#", 1)[1]
def target(value: dict) -> str: return value["target"]
def type_ref(shape_id: str) -> str: return BUILTINS.get(shape_id, local(shape_id))


def compile_model(ast: dict, service_id: str | None) -> dict:
    shapes: dict[str, dict] = ast["shapes"]
    services = [item for item, shape in shapes.items() if SERVICE_TRAIT in shape.get("traits", {})]
    if service_id is None:
        if len(services) != 1: raise ValueError("exactly one OVF service is required unless --service is used")
        service_id = services[0]
    if service_id not in shapes: raise ValueError(f"service not found: {service_id}")
    service_shape = shapes[service_id]
    service_trait = service_shape.get("traits", {}).get(SERVICE_TRAIT)
    if service_trait is None: raise ValueError(f"service lacks {SERVICE_TRAIT}")
    namespace = service_id.split("#", 1)[0]
    referenced_wrappers = set(service_trait.get("events", []) + service_trait.get("fields", []))

    types: list[dict] = []
    for shape_id, shape in sorted(shapes.items()):
        if not shape_id.startswith(namespace + "#") or shape_id in referenced_wrappers:
            continue
        name, kind, traits = local(shape_id), shape["type"], shape.get("traits", {})
        if kind in ("service", "operation", "resource", "union", "enum", "intEnum"):
            continue
        if kind in ("byte", "short", "integer", "long", "bigInteger"):
            integer = traits.get(INTEGER_TRAIT)
            if integer is None: raise ValueError(f"{shape_id}: exact integer trait required")
            types.append({"kind": "integer", "name": name,
                "signed": integer["signed"], "bits": integer["bits"]})
        elif kind == "string":
            collection = traits.get(COLLECTION_TRAIT)
            if collection is None: raise ValueError(f"{shape_id}: collection bound required")
            item = {"kind": "string", "name": name,
                "storage": collection["storage"].lower()}
            if "capacity" in collection: item["capacity"] = collection["capacity"]
            types.append(item)
        elif kind == "list":
            collection = traits.get(COLLECTION_TRAIT)
            if collection is None: raise ValueError(f"{shape_id}: collection bound required")
            item = {"kind": "sequence", "name": name,
                "element": type_ref(target(shape["member"])),
                "storage": collection["storage"].lower()}
            if "capacity" in collection: item["capacity"] = collection["capacity"]
            types.append(item)
        elif kind == "structure":
            members = []
            for member_name, member in shape.get("members", {}).items():
                tag = member.get("traits", {}).get(TAG_TRAIT)
                if tag is None: raise ValueError(f"{shape_id}${member_name}: tag required")
                members.append({"name": member_name, "tag": tag["value"],
                    "type": type_ref(target(member)),
                    "required": REQUIRED_TRAIT in member.get("traits", {})})
            types.append({"kind": "struct", "name": name,
                "members": sorted(members, key=lambda item: item["tag"])})
        else:
            raise ValueError(f"{shape_id}: unsupported Smithy shape type {kind}")

    events = []
    for event_id in service_trait.get("events", []):
        shape = shapes[event_id]
        trait = shape.get("traits", {}).get(EVENT_TRAIT)
        members = shape.get("members", {})
        if trait is None or set(members) != {"value"}: raise ValueError(f"{event_id}: invalid event wrapper")
        events.append({"id": trait["id"], "name": local(event_id), "tag": trait["tag"],
            "payload": type_ref(target(members["value"]))})

    fields = []
    for field_id in service_trait.get("fields", []):
        shape = shapes[field_id]
        trait = shape.get("traits", {}).get(FIELD_TRAIT)
        members = shape.get("members", {})
        if trait is None or set(members) != {"value"}: raise ValueError(f"{field_id}: invalid field wrapper")
        fields.append({"id": trait["id"], "name": local(field_id), "tag": trait["tag"],
            "value": type_ref(target(members["value"])), "readable": trait.get("readable", False),
            "writable": trait.get("writable", False), "notifiable": trait.get("notifiable", False)})

    methods = []
    for reference in service_shape.get("operations", []):
        operation_id = target(reference)
        shape = shapes[operation_id]
        trait = shape.get("traits", {}).get(METHOD_TRAIT)
        if trait is None: raise ValueError(f"{operation_id}: method trait required")
        methods.append({"id": trait["id"], "name": local(operation_id), "tag": trait["tag"],
            "input": type_ref(target(shape["input"])), "output": type_ref(target(shape["output"])),
            "errors": sorted(type_ref(target(item)) for item in shape.get("errors", [])),
            "idempotent": trait.get("idempotent", False)})

    model = {"irVersion": 1, "namespace": namespace, "services": [{
        "id": service_trait["id"], "name": local(service_id), "version": service_shape["version"],
        "events": sorted(events, key=lambda item: item["tag"]),
        "methods": sorted(methods, key=lambda item: item["tag"]),
        "fields": sorted(fields, key=lambda item: item["tag"])}], "types": types}
    errors = validate(model)
    if errors: raise ValueError("\n".join(errors))
    return normalized(model)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("ast", type=Path)
    parser.add_argument("--service")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    ast = json.loads(args.ast.read_text(encoding="utf-8"))
    model = compile_model(ast, args.service)
    content = json.dumps(model, indent=2, ensure_ascii=False) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text(encoding="utf-8") != content:
        args.output.write_text(content, encoding="utf-8")
    return 0


if __name__ == "__main__": raise SystemExit(main())
