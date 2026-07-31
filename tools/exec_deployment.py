#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Validate and generate execution deployment artifacts."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
from typing import Any

REQUIRED = "smithy.api#required"
LENGTH = "smithy.api#length"
RANGE = "smithy.api#range"
PATTERN = "smithy.api#pattern"
SAFE_COMMAND_TOKEN = re.compile(r"^[A-Za-z0-9_./:=+,@%-]+$")
TERMINAL_ACTIONS = {
    "hold_observed_configuration",
    "enter_fallback_mode",
    "stop_domain",
    "request_system_recovery",
}


def _traits(shape: dict[str, Any]) -> dict[str, Any]:
    return shape.get("traits", {})


def _apply_traits(value: Any, traits: dict[str, Any], path: str) -> list[str]:
    errors: list[str] = []
    length = traits.get(LENGTH)
    if length is not None and isinstance(value, (str, list, dict)):
        if "min" in length and len(value) < length["min"]:
            errors.append(f"{path}: length is below {length['min']}")
        if "max" in length and len(value) > length["max"]:
            errors.append(f"{path}: length exceeds {length['max']}")
    bounds = traits.get(RANGE)
    if bounds is not None and isinstance(value, int) and not isinstance(value, bool):
        if "min" in bounds and value < bounds["min"]:
            errors.append(f"{path}: value is below {bounds['min']}")
        if "max" in bounds and value > bounds["max"]:
            errors.append(f"{path}: value exceeds {bounds['max']}")
    pattern = traits.get(PATTERN)
    if pattern is not None and isinstance(value, str):
        expression = pattern if isinstance(pattern, str) else pattern.get("value", "")
        if re.fullmatch(expression, value) is None:
            errors.append(f"{path}: value does not match {expression}")
    return errors


def _validate_shape(
    ast: dict[str, Any], shape_id: str, value: Any, path: str,
    member_traits: dict[str, Any] | None = None,
) -> list[str]:
    shapes = ast["shapes"]
    if shape_id == "smithy.api#Boolean":
        errors = [] if isinstance(value, bool) else [f"{path}: expected boolean"]
    elif shape_id in ("smithy.api#Integer", "smithy.api#Long"):
        errors = (
            []
            if isinstance(value, int) and not isinstance(value, bool)
            else [f"{path}: expected integer"]
        )
    elif shape_id == "smithy.api#String":
        errors = [] if isinstance(value, str) else [f"{path}: expected string"]
    else:
        shape = shapes.get(shape_id)
        if shape is None:
            return [f"{path}: Smithy shape is missing: {shape_id}"]
        kind = shape["type"]
        errors = []
        if kind in ("long", "integer"):
            if not isinstance(value, int) or isinstance(value, bool):
                errors.append(f"{path}: expected integer")
        elif kind == "string":
            if not isinstance(value, str):
                errors.append(f"{path}: expected string")
        elif kind == "enum":
            allowed = {
                member.get("traits", {}).get("smithy.api#enumValue")
                for member in shape.get("members", {}).values()
            }
            if value not in allowed:
                errors.append(f"{path}: expected one of {sorted(allowed)}")
        elif kind == "list":
            if not isinstance(value, list):
                errors.append(f"{path}: expected list")
            else:
                member = shape["member"]
                for index, item in enumerate(value):
                    errors.extend(
                        _validate_shape(
                            ast,
                            member["target"],
                            item,
                            f"{path}[{index}]",
                            _traits(member),
                        )
                    )
        elif kind == "map":
            if not isinstance(value, dict):
                errors.append(f"{path}: expected object")
            else:
                key = shape["key"]
                item = shape["value"]
                for name, entry in value.items():
                    errors.extend(
                        _validate_shape(ast, key["target"], name, f"{path}.<key>", _traits(key))
                    )
                    errors.extend(
                        _validate_shape(
                            ast, item["target"], entry, f"{path}.{name}", _traits(item)
                        )
                    )
        elif kind == "structure":
            if not isinstance(value, dict):
                errors.append(f"{path}: expected object")
            else:
                members = shape.get("members", {})
                unknown = sorted(set(value) - set(members))
                errors.extend(f"{path}.{name}: unknown member" for name in unknown)
                for name, member in members.items():
                    if REQUIRED in _traits(member) and name not in value:
                        errors.append(f"{path}.{name}: required member is missing")
                    elif name in value:
                        errors.extend(
                            _validate_shape(
                                ast,
                                member["target"],
                                value[name],
                                f"{path}.{name}",
                                _traits(member),
                            )
                        )
        else:
            errors.append(f"{path}: unsupported Smithy shape type {kind}")
        errors.extend(_apply_traits(value, _traits(shape), path))
    errors.extend(_apply_traits(value, member_traits or {}, path))
    return errors


def validate_smithy_model(ast: dict[str, Any], model: dict[str, Any]) -> list[str]:
    return _validate_shape(ast, "ovf.exec.model#ExecutionDeployment", model, "model")


def _duplicates(values: list[dict[str, Any]], key: str) -> set[Any]:
    seen: set[Any] = set()
    duplicates: set[Any] = set()
    for value in values:
        item = value[key]
        if item in seen:
            duplicates.add(item)
        seen.add(item)
    return duplicates


def validate_semantics(model: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    units = model["units"]
    domains = model["domains"]
    for duplicate in sorted(_duplicates(units, "id")):
        errors.append(f"units: duplicate id {duplicate}")
    for duplicate in sorted(_duplicates(units, "name")):
        errors.append(f"units: duplicate name {duplicate}")
    unit_ids = {item["id"] for item in units}
    dependencies: dict[int, list[int]] = {}
    bootstrap_ids = {item["id"] for item in units if item["bootstrap"]}
    native_services = set(model["platform"]["dinit"]["nativeServices"])
    readiness_by_kind = {
        "managed_application": {"lifecycle_channel", "process_started"},
        "service": {"process_started", "supervisor_notification"},
        "one_shot": {"successful_exit"},
        "mount": {"mount_present"},
        "external": {"process_started", "supervisor_notification"},
    }
    for unit in units:
        path = f"units[{unit['id']}]"
        kind = unit["kind"]
        executable = unit["executable"]
        native_service = unit["nativeService"]
        if kind == "external":
            if executable:
                errors.append(f"{path}.executable: external units use nativeService")
            if not native_service:
                errors.append(f"{path}.nativeService: external unit requires a service name")
            elif native_service not in native_services:
                errors.append(
                    f"{path}.nativeService: service is not supplied by the platform"
                )
        elif not executable:
            errors.append(f"{path}.executable: executable unit requires an absolute path")
        if executable and not SAFE_COMMAND_TOKEN.fullmatch(executable):
            errors.append(f"{path}.executable: contains unsupported command characters")
        if unit["readiness"] not in readiness_by_kind.get(kind, set()):
            errors.append(f"{path}.readiness: unsupported for {kind}")
        for argument in unit["arguments"]:
            if not SAFE_COMMAND_TOKEN.fullmatch(argument):
                errors.append(f"{path}.arguments: contains unsupported command characters")
        stop_executable = unit["stopExecutable"]
        if stop_executable and not SAFE_COMMAND_TOKEN.fullmatch(stop_executable):
            errors.append(f"{path}.stopExecutable: contains unsupported command characters")
        for argument in unit["stopArguments"]:
            if not SAFE_COMMAND_TOKEN.fullmatch(argument):
                errors.append(f"{path}.stopArguments: contains unsupported command characters")
        if kind == "mount" and not stop_executable:
            errors.append(f"{path}.stopExecutable: mount unit requires an unmount operation")
        unknown = sorted(set(unit["dependencies"]) - unit_ids)
        if unknown:
            errors.append(f"{path}.dependencies: unknown unit ids {unknown}")
        if unit["id"] in unit["dependencies"]:
            errors.append(f"{path}.dependencies: unit depends on itself")
        if unit["bootstrap"] and any(
            dependency not in bootstrap_ids for dependency in unit["dependencies"]
        ):
            errors.append(f"{path}.dependencies: bootstrap unit depends on managed unit")
        dependencies[unit["id"]] = unit["dependencies"]

    visiting: set[int] = set()
    visited: set[int] = set()

    def visit(unit: int) -> None:
        if unit in visited:
            return
        if unit in visiting:
            errors.append(f"units: dependency cycle includes {unit}")
            return
        visiting.add(unit)
        for dependency in dependencies.get(unit, []):
            visit(dependency)
        visiting.remove(unit)
        visited.add(unit)

    for unit in sorted(unit_ids):
        visit(unit)

    domain_ids = {item["id"] for item in domains}
    if len(domain_ids) != len(domains):
        errors.append("domains: duplicate domain id")
    for domain in domains:
        path = f"domains[{domain['id']}]"
        modes = domain["modes"]
        mode_ids = {mode["id"] for mode in modes}
        if len(mode_ids) != len(modes):
            errors.append(f"{path}.modes: duplicate mode id")
        if domain["initialMode"] not in mode_ids:
            errors.append(f"{path}.initialMode: mode is not defined")
        recovery = domain["recovery"]
        if recovery["action"] not in TERMINAL_ACTIONS:
            errors.append(f"{path}.recovery.action: unsupported recovery action")
        fallback = recovery.get("fallbackMode")
        if recovery["action"] == "enter_fallback_mode" and fallback not in mode_ids:
            errors.append(f"{path}.recovery.fallbackMode: valid fallback is required")
        if recovery["action"] != "enter_fallback_mode" and fallback is not None:
            errors.append(f"{path}.recovery.fallbackMode: only valid for fallback recovery")
        for mode in modes:
            mode_path = f"{path}.modes[{mode['id']}]"
            unknown = sorted(set(mode["units"]) - unit_ids)
            if unknown:
                errors.append(f"{mode_path}.units: unknown unit ids {unknown}")
            for constraint in mode["constraints"]:
                other = constraint["other"]
                other_domain = next(
                    (item for item in domains if item["id"] == other["domain"]), None
                )
                if other_domain is None:
                    errors.append(f"{mode_path}.constraints: unknown domain {other['domain']}")
                elif other["mode"] not in {item["id"] for item in other_domain["modes"]}:
                    errors.append(
                        f"{mode_path}.constraints: unknown mode "
                        f"{other['domain']}.{other['mode']}"
                    )
                if other["domain"] == domain["id"] and other["mode"] == mode["id"]:
                    errors.append(f"{mode_path}.constraints: mode cannot constrain itself")
    assigned_units = {
        unit for domain in domains for mode in domain["modes"] for unit in mode["units"]
    }
    misplaced_bootstrap = sorted(assigned_units & bootstrap_ids)
    if misplaced_bootstrap:
        errors.append(f"domains: bootstrap units must not be mode members {misplaced_bootstrap}")
    unassigned = sorted(unit_ids - bootstrap_ids - assigned_units)
    if unassigned:
        errors.append(f"domains: non-bootstrap units have no explicit mode membership {unassigned}")
    return errors


def normalized(model: dict[str, Any]) -> dict[str, Any]:
    result = json.loads(json.dumps(model))
    for unit in result["units"]:
        unit["dependencies"] = sorted(set(unit["dependencies"]))
        unit["exclusiveResources"] = sorted(set(unit["exclusiveResources"]))
    result["units"].sort(key=lambda item: item["id"])
    for domain in result["domains"]:
        for mode in domain["modes"]:
            mode["units"] = sorted(set(mode["units"]))
            mode["constraints"].sort(
                key=lambda item: (
                    item["kind"], item["other"]["domain"], item["other"]["mode"]
                )
            )
        domain["modes"].sort(key=lambda item: item["id"])
    result["domains"].sort(key=lambda item: item["id"])
    return result


def service_name(unit: dict[str, Any]) -> str:
    if unit["kind"] == "external":
        return unit["nativeService"]
    if unit["kind"] == "managed_application":
        return f"ovf-app-{unit['id']}"
    return f"ovf-unit-{unit['id']}"


def fingerprint(value: Any) -> str:
    content = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(content).hexdigest()


def generate_artifacts(
    model: dict[str, Any],
    schema_ast: dict[str, Any],
    output_model: Path,
    backend_config: Path,
    metadata_path: Path,
) -> None:
    model = normalized(model)
    output_model.parent.mkdir(parents=True, exist_ok=True)
    output_model.write_text(json.dumps(model, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    units = [
        {"id": item["id"], "service": service_name(item)}
        for item in model["units"]
    ]
    backend = {
        "backendVersion": 2,
        "kind": "dinit",
        "library": model["platform"]["dinit"]["backendLibrary"],
        "systemRecoveryService": model["platform"]["dinit"][
            "systemRecoveryService"
        ],
        "controlSocket": model["platform"]["dinit"]["controlSocket"],
        "units": units,
    }
    backend_config.write_text(
        json.dumps(backend, sort_keys=True, indent=2) + "\n", encoding="utf-8"
    )
    metadata = {
        "artifactVersion": 1,
        "modelGeneration": model["generation"],
        "modelFingerprint": fingerprint(model),
        "executionModelArtifactFingerprint": hashlib.sha256(
            output_model.read_bytes()
        ).hexdigest(),
        "backendConfigurationFingerprint": hashlib.sha256(
            backend_config.read_bytes()
        ).hexdigest(),
        "smithySchemaFingerprint": fingerprint(schema_ast),
        "backendConfiguration": backend_config.name,
    }
    metadata_path.write_text(
        json.dumps(metadata, sort_keys=True, indent=2) + "\n", encoding="utf-8"
    )


def finalize_manifest(metadata_path: Path, services: Path, manifest_path: Path) -> None:
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    metadata["servicesDirectory"] = services.name
    metadata["serviceFingerprints"] = {
        path.name: hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(services.iterdir())
        if path.is_file()
    }
    manifest_path.write_text(
        json.dumps(metadata, sort_keys=True, indent=2) + "\n", encoding="utf-8"
    )
