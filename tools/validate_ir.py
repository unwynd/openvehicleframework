#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Validate OVF communication IR and optionally check backward compatibility."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import uuid
from pathlib import Path

BUILTINS = {"bool", "f32", "f64", "string", "bytes"}
NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def load(path: Path) -> dict:
    with path.open(encoding="utf-8") as source:
        return json.load(source)


def validate(model: dict) -> list[str]:
    errors: list[str] = []
    if model.get("irVersion") != 1:
        errors.append("irVersion must be 1")
    types = model.get("types", [])
    services = model.get("services", [])
    type_names = {item.get("name") for item in types}
    if len(type_names) != len(types):
        errors.append("type names must be unique")

    def check_name(value: object, where: str) -> None:
        if not isinstance(value, str) or not NAME.fullmatch(value):
            errors.append(f"{where}: invalid name")

    def check_uuid(value: object, where: str) -> None:
        try:
            parsed = uuid.UUID(str(value))
            if parsed.version != 4 or str(parsed) != value:
                raise ValueError
        except (ValueError, AttributeError):
            errors.append(f"{where}: ID must be a canonical lowercase UUIDv4")

    def check_ref(value: object, where: str) -> None:
        if value not in BUILTINS and value not in type_names:
            errors.append(f"{where}: unresolved type {value!r}")

    for shape in types:
        where = f"type {shape.get('name', '?')}"
        check_name(shape.get("name"), where)
        kind = shape.get("kind")
        if kind == "integer":
            if shape.get("bits") not in (8, 16, 32, 64):
                errors.append(f"{where}: unsupported integer width")
            if not isinstance(shape.get("signed"), bool):
                errors.append(f"{where}: signed must be boolean")
        elif kind in ("sequence", "string"):
            if kind == "sequence":
                check_ref(shape.get("element"), where)
            storage = shape.get("storage")
            capacity = shape.get("capacity")
            if storage not in ("fixed", "bounded", "unbounded"):
                errors.append(f"{where}: invalid storage")
            if storage in ("fixed", "bounded") and (
                not isinstance(capacity, int) or capacity < 1
            ):
                errors.append(f"{where}: fixed/bounded sequence needs positive capacity")
            if storage == "unbounded" and capacity is not None:
                errors.append(f"{where}: unbounded sequence cannot have capacity")
        elif kind == "struct":
            members = shape.get("members", [])
            tags = [member.get("tag") for member in members]
            names = [member.get("name") for member in members]
            if len(tags) != len(set(tags)):
                errors.append(f"{where}: member tags must be unique")
            if len(names) != len(set(names)):
                errors.append(f"{where}: member names must be unique")
            for member in members:
                check_name(member.get("name"), where)
                if not isinstance(member.get("tag"), int) or not 1 <= member["tag"] <= 0xFFFFFFFF:
                    errors.append(f"{where}: member tag out of range")
                check_ref(member.get("type"), where)
                if not isinstance(member.get("required"), bool):
                    errors.append(f"{where}: member required must be boolean")
        else:
            errors.append(f"{where}: unsupported kind {kind!r}")

    entity_ids: set[str] = set()
    for service in services:
        where = f"service {service.get('name', '?')}"
        check_name(service.get("name"), where)
        check_uuid(service.get("id"), where)
        if service.get("id") in entity_ids:
            errors.append(f"{where}: duplicate entity ID")
        entity_ids.add(service.get("id"))
        elements = service.get("events", []) + service.get("methods", []) + service.get("fields", [])
        tags = [element.get("tag") for element in elements]
        if len(tags) != len(set(tags)):
            errors.append(f"{where}: element tags must be unique across all element kinds")
        for element in elements:
            item_where = f"{where}.{element.get('name', '?')}"
            check_name(element.get("name"), item_where)
            check_uuid(element.get("id"), item_where)
            if element.get("id") in entity_ids:
                errors.append(f"{item_where}: duplicate entity ID")
            entity_ids.add(element.get("id"))
            if not isinstance(element.get("tag"), int) or not 1 <= element["tag"] <= 0xFFFFFFFF:
                errors.append(f"{item_where}: element tag out of range")
        for event in service.get("events", []):
            check_ref(event.get("payload"), where)
        for method in service.get("methods", []):
            check_ref(method.get("input"), where)
            check_ref(method.get("output"), where)
            for error in method.get("errors", []):
                check_ref(error, where)
        for field in service.get("fields", []):
            check_ref(field.get("value"), where)
    return errors


def compatibility_errors(old: dict, new: dict) -> list[str]:
    errors: list[str] = []
    old_types = {item["name"]: item for item in old.get("types", [])}
    new_types = {item["name"]: item for item in new.get("types", [])}
    for name, before in old_types.items():
        after = new_types.get(name)
        if after is None:
            errors.append(f"removed type {name}")
            continue
        if before["kind"] != after["kind"]:
            errors.append(f"changed kind of type {name}")
        elif before["kind"] in ("integer", "sequence", "string") and before != after:
            errors.append(f"changed representation or bound of type {name}")
        elif before["kind"] == "struct":
            new_by_tag = {member["tag"]: member for member in after["members"]}
            for member in before["members"]:
                candidate = new_by_tag.get(member["tag"])
                if candidate != member:
                    errors.append(f"changed/removed {name} member tag {member['tag']}")
            old_tags = {member["tag"] for member in before["members"]}
            for member in after["members"]:
                if member["tag"] not in old_tags and member["required"]:
                    errors.append(f"added required member {name}.{member['name']}")

    old_services = {item["id"]: item for item in old.get("services", [])}
    new_services = {item["id"]: item for item in new.get("services", [])}
    for service_id, before in old_services.items():
        after = new_services.get(service_id)
        if after is None:
            errors.append(f"removed service {before['name']}")
            continue
        for kind in ("events", "methods", "fields"):
            new_elements = {item["id"]: item for item in after[kind]}
            for element in before[kind]:
                if new_elements.get(element["id"]) != element:
                    errors.append(f"changed/removed {before['name']}.{element['name']}")
    return errors


def normalized(model: dict) -> dict:
    value = json.loads(json.dumps(model))
    value["types"] = sorted(value.get("types", []), key=lambda item: item["name"])
    for shape in value["types"]:
        if shape["kind"] == "struct":
            shape["members"] = sorted(shape["members"], key=lambda item: item["tag"])
    value["services"] = sorted(value.get("services", []), key=lambda item: item["id"])
    for service in value["services"]:
        for kind in ("events", "methods", "fields"):
            service[kind] = sorted(service[kind], key=lambda item: item["tag"])
        for method in service["methods"]:
            method["errors"] = sorted(method.get("errors", []))
    return value


def fingerprint(model: dict) -> str:
    encoded = json.dumps(normalized(model), sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return hashlib.sha256(encoded.encode("utf-8")).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("model", type=Path)
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    model = load(args.model)
    errors = validate(model)
    if args.baseline:
        baseline = load(args.baseline)
        errors.extend(f"compatibility: {item}" for item in validate(baseline))
        if not errors:
            errors.extend(f"compatibility: {item}" for item in compatibility_errors(baseline, model))
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print(fingerprint(model))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
