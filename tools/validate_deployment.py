#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Validate a deployment against canonical contract and provider profiles."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import re
import uuid
from pathlib import Path

from tools.validate_ir import fingerprint, validate


def read(path: Path) -> dict:
    with path.open(encoding="utf-8") as source: return json.load(source)


def deployment_fingerprint(value: dict) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return hashlib.sha256(encoded.encode()).hexdigest()


_INSTANCE_NAMESPACE = uuid.UUID("91ec7c75-b53f-4b0c-a51c-876eea485f71")


def _wire_id(value: str, lower: int, upper: int) -> int:
    digest = hashlib.sha256(value.encode()).digest()
    return lower + int.from_bytes(digest[:8], "big") % (upper - lower + 1)


def _route_features(role: str, profile: str) -> list[str]:
    common = ["events", "methods", "reliable", "ordered", "deadlines", "cancellation"]
    if role == "consumer":
        common.insert(0, "discovery")
    if profile == "iceoryx2":
        common.extend(["loans", "scatterGather"])
    return common


def _route_limits(profile: str) -> dict:
    return {
        "maxPayloadSize": 4096 if profile == "iceoryx2" else 65536,
        "maxHistoryDepth": 1 if profile in {"iceoryx2", "vsomeip"} else 8,
        "maxOutstandingOperations": 16,
        "maxEndpoints": 8,
    }


def _iceoryx2_event(element: dict) -> dict:
    return {
        "name": element["name"],
        "type": element.get("payload", element.get("value")),
        "payloadSize": 4096,
        "alignment": 8,
        "history": 1,
        "subscriberBuffer": 8,
        "maxPublishers": 2,
        "maxSubscribers": 8,
        "safeOverflow": False,
    }


def _iceoryx2_method(name: str, request_type: str, response_type: str) -> dict:
    return {
        "name": name,
        "requestType": request_type,
        "responseType": response_type,
        "requestPayloadSize": 4096,
        "responsePayloadSize": 4096,
        "alignment": 8,
        "requestBuffer": 16,
        "responseBuffer": 8,
        "maxClients": 8,
        "maxServers": 1,
        "safeOverflow": False,
    }


def _generated_mappings(service: dict, instance_name: str, profile: str) -> dict:
    if profile in {"inproc", "cyclonedds"}:
        elements = {
            element["id"]: element["name"]
            for kind in ("events", "methods", "fields")
            for element in service[kind]
        }
        return {"service": service["name"], "instance": instance_name, "elements": elements}
    if profile == "iceoryx2":
        elements = {
            element["id"]: _iceoryx2_event(element) for element in service["events"]
        }
        elements.update({
            element["id"]: _iceoryx2_method(
                element["name"], element["input"], element["output"]
            )
            for element in service["methods"]
        })
        for field in service["fields"]:
            operations = {}
            if field["readable"]:
                operations["get"] = _iceoryx2_method(
                    f"{field['name']}-get", "Empty", field["value"]
                )
            if field["writable"]:
                operations["set"] = _iceoryx2_method(
                    f"{field['name']}-set", field["value"], "Empty"
                )
            if field["notifiable"]:
                operations["notify"] = _iceoryx2_event(field)
            elements[field["id"]] = operations
        return {
            "service": service["name"],
            "instance": instance_name,
            "elements": elements,
        }
    if profile == "vsomeip":
        elements = {}
        for element in service["events"]:
            elements[element["id"]] = {
                "id": _wire_id(element["id"] + ":event", 0x8001, 0xFFFE),
                "eventGroup": _wire_id(element["id"] + ":group", 1, 0xFFFE),
                "major": 1,
                "minor": 0,
                "kind": "event",
                "reliable": True,
            }
        for element in service["methods"]:
            elements[element["id"]] = {
                "id": _wire_id(element["id"] + ":method", 1, 0x7FFE),
                "eventGroup": 0,
                "major": 1,
                "minor": 0,
                "kind": "method",
                "reliable": True,
            }
        for field in service["fields"]:
            operations = {}
            for operation, kind, notification in (
                ("get", "fieldGet", False),
                ("set", "fieldSet", False),
                ("notify", "fieldNotify", True),
            ):
                enabled = field[{"get": "readable", "set": "writable",
                                 "notify": "notifiable"}[operation]]
                if not enabled:
                    continue
                operations[operation] = {
                    "id": _wire_id(
                        field["id"] + ":" + operation,
                        0x8001 if notification else 1,
                        0xFFFE if notification else 0x7FFE,
                    ),
                    "eventGroup": (
                        _wire_id(field["id"] + ":group", 1, 0xFFFE)
                        if notification else 0
                    ),
                    "major": 1,
                    "minor": 0,
                    "kind": kind,
                    "reliable": True,
                }
            elements[field["id"]] = operations
        return {
            "service": _wire_id(service["id"], 1, 0xFFFE),
            "instance": _wire_id(service["id"] + ":" + instance_name, 1, 0xFFFE),
            "elements": elements,
        }
    raise ValueError(f"unsupported generated mapping profile {profile}")


def resolve_deployment(contract: dict, model: dict) -> dict:
    """Generate a canonical deployment from app intent and binding policy."""
    intent = model["deployment"]
    bindings = {item["transport"]: item for item in model["bindings"]}
    if len(bindings) != len(model["bindings"]):
        raise ValueError("binding policy contains duplicate transport selections")
    namespace = contract["namespace"]
    services = {
        f"{namespace}#{service['name']}": service for service in contract["services"]
    }
    instances = []
    used_bindings = {}
    for instance in intent["instances"]:
        service_name = instance["interface"]
        service = services.get(service_name)
        if service is None:
            continue
        binding = bindings.get(instance["transport"])
        if binding is None:
            raise ValueError(
                f"binding policy does not provide {instance['transport']} transport"
            )
        used_bindings[binding["provider"]] = binding
        requirements = instance.get("requirements", {})
        features = requirements.get(
            "features", _route_features(instance["role"], binding["profile"])
        )
        limits = _route_limits(binding["profile"])
        limits.update(requirements.get("limits", {}))
        route = {
            "provider": binding["provider"],
            "role": instance["role"],
            "required": binding["required"],
            "requiredFeatures": features,
            "limits": limits,
            "mappings": _generated_mappings(
                service, instance["instance"], binding["profile"]
            ),
        }
        if instance["role"] == "consumer":
            route["priority"] = 0
        instance_id = str(uuid.uuid5(
            _INSTANCE_NAMESPACE, f"{service_name}:{instance['instance']}"
        ))
        resolved_instance = {
            "instanceId": instance_id,
            "alias": instance["instance"],
        }
        resolved_instance["serviceId"] = service["id"]
        resolved_instance["routes"] = [route]
        instances.append(resolved_instance)
    if not instances:
        names = ", ".join(sorted(services))
        raise ValueError(f"deployment does not declare an instance of {names}")
    return {
        "deploymentVersion": intent["deploymentVersion"],
        "contractFingerprint": fingerprint(contract),
        "providers": [
            {
                "id": binding["provider"],
                "profile": binding["profile"],
                "required": binding["required"],
                **(
                    {"extensions": binding["extensions"]}
                    if "extensions" in binding
                    else {}
                ),
            }
            for binding in used_bindings.values()
        ],
        "instances": instances,
    }


def structural_errors(value: object) -> list[str]:
    errors: list[str] = []
    allowed_features = {"discovery", "events", "methods", "loans", "scatterGather",
        "reliable", "ordered", "deadlines", "cancellation"}
    limits = {"maxPayloadSize", "maxHistoryDepth", "maxOutstandingOperations", "maxEndpoints"}
    if not isinstance(value, dict): return ["deployment root must be an object"]
    required_root = {"deploymentVersion", "contractFingerprint", "providers", "instances"}
    if set(value) != required_root: errors.append("deployment root has missing or unknown properties")
    if not isinstance(value.get("providers"), list): errors.append("providers must be an array")
    if not isinstance(value.get("instances"), list): errors.append("instances must be an array")
    if errors: return errors
    for index, provider in enumerate(value["providers"]):
        where = f"provider[{index}]"
        if not isinstance(provider, dict): errors.append(f"{where}: must be an object"); continue
        if not {"id", "profile", "required"} <= set(provider) or set(provider) - {"id", "profile", "required", "extensions"}:
            errors.append(f"{where}: missing or unknown properties")
        if not isinstance(provider.get("id"), str) or not re.fullmatch(r"[a-z][a-z0-9_-]*", provider["id"]): errors.append(f"{where}: invalid ID")
        if not isinstance(provider.get("profile"), str): errors.append(f"{where}: invalid profile")
        if not isinstance(provider.get("required"), bool): errors.append(f"{where}: required must be boolean")
        extensions = provider.get("extensions", {})
        if not isinstance(extensions, dict): errors.append(f"{where}: extensions must be an object")
        else:
            for name in extensions:
                if not re.fullmatch(r"[a-z][a-z0-9]*(\.[a-z][a-z0-9-]*)+", name): errors.append(f"{where}: invalid extension namespace {name}")
    for index, instance in enumerate(value["instances"]):
        where = f"instance[{index}]"
        if not isinstance(instance, dict): errors.append(f"{where}: must be an object"); continue
        if not {"serviceId", "instanceId", "routes"} <= set(instance) or set(instance) - {"serviceId", "instanceId", "alias", "routes"}: errors.append(f"{where}: missing or unknown properties")
        for key in ("serviceId", "instanceId"):
            try:
                parsed = uuid.UUID(str(instance.get(key)))
                if str(parsed) != instance.get(key): raise ValueError
                if key == "serviceId" and parsed.version != 4: raise ValueError
                if key == "instanceId" and parsed.version not in (4, 5): raise ValueError
            except ValueError:
                expected = "UUIDv4" if key == "serviceId" else "UUIDv4 or UUIDv5"
                errors.append(f"{where}: {key} must be canonical {expected}")
        if not isinstance(instance.get("routes"), list) or not instance["routes"]: errors.append(f"{where}: routes must be a non-empty array"); continue
        for route_index, route in enumerate(instance["routes"]):
            route_where = f"{where}.route[{route_index}]"
            required = {"provider", "role", "required", "requiredFeatures", "limits", "mappings"}
            allowed = required | {"priority"}
            if not isinstance(route, dict): errors.append(f"{route_where}: must be an object"); continue
            if not required <= set(route) or set(route) - allowed: errors.append(f"{route_where}: missing or unknown properties")
            if route.get("role") not in ("consumer", "provider"): errors.append(f"{route_where}: invalid role")
            if not isinstance(route.get("required"), bool): errors.append(f"{route_where}: required must be boolean")
            features = route.get("requiredFeatures")
            if not isinstance(features, list) or any(item not in allowed_features for item in features) or len(features) != len(set(features)): errors.append(f"{route_where}: invalid requiredFeatures")
            requested_limits = route.get("limits")
            if not isinstance(requested_limits, dict) or set(requested_limits) - limits or any(not isinstance(item, int) or item < 1 for item in requested_limits.values()): errors.append(f"{route_where}: invalid limits")
            mappings = route.get("mappings")
            if not isinstance(mappings, dict) or set(mappings) != {"service", "instance", "elements"} or not isinstance(mappings.get("elements"), dict): errors.append(f"{route_where}: invalid mappings")
    return errors


def profile_errors(profile: object, expected_name: str) -> list[str]:
    if not isinstance(profile, dict): return ["profile must be an object"]
    keys = {"profileVersion", "name", "isolation", "features", "limits"}
    errors = []
    if set(profile) != keys: errors.append("missing or unknown profile properties")
    if profile.get("profileVersion") != 1 or profile.get("name") != expected_name: errors.append("invalid profile identity")
    if profile.get("isolation") not in ("independent", "sharedEngine", "processSingleton"): errors.append("invalid isolation")
    allowed_features = {"discovery", "events", "methods", "loans", "scatterGather", "reliable", "ordered", "deadlines", "cancellation"}
    features = profile.get("features")
    if not isinstance(features, list) or any(item not in allowed_features for item in features) or len(features) != len(set(features)): errors.append("invalid profile features")
    required_limits = {"maxPayloadSize", "maxHistoryDepth", "maxOutstandingOperations", "maxEndpoints"}
    limits = profile.get("limits")
    if not isinstance(limits, dict) or set(limits) != required_limits or any(not isinstance(item, int) or item < 1 for item in limits.values()): errors.append("invalid profile limits")
    return errors


def vsomeip_mapping_errors(mapping: object) -> list[str]:
    if not isinstance(mapping, dict): return ["mapping must be an object"]
    required = {"id", "eventGroup", "major", "minor", "kind", "reliable"}
    if set(mapping) != required: return ["mapping has missing or unknown properties"]
    errors = []
    identifier = mapping.get("id")
    event_group = mapping.get("eventGroup")
    major = mapping.get("major")
    minor = mapping.get("minor")
    kind = mapping.get("kind")
    if not isinstance(identifier, int) or isinstance(identifier, bool) or not 0 <= identifier <= 0xffff:
        errors.append("id must be an unsigned 16-bit integer")
    if not isinstance(event_group, int) or isinstance(event_group, bool) or not 0 <= event_group <= 0xffff:
        errors.append("eventGroup must be an unsigned 16-bit integer")
    if not isinstance(major, int) or isinstance(major, bool) or not 0 <= major <= 0xff:
        errors.append("major must be an unsigned 8-bit integer")
    if not isinstance(minor, int) or isinstance(minor, bool) or not 0 <= minor <= 0xffffffff:
        errors.append("minor must be an unsigned 32-bit integer")
    if kind not in {"event", "method", "fieldGet", "fieldSet", "fieldNotify"}:
        errors.append("invalid kind")
    if not isinstance(mapping.get("reliable"), bool): errors.append("reliable must be boolean")
    notification = kind in {"event", "fieldNotify"}
    if isinstance(event_group, int) and not isinstance(event_group, bool):
        if notification and event_group == 0: errors.append("notification requires a nonzero eventGroup")
        if not notification and event_group != 0: errors.append("method mapping requires eventGroup zero")
    if isinstance(identifier, int) and not isinstance(identifier, bool):
        if notification and identifier < 0x8000: errors.append("notification id must have its high bit set")
        if not notification and identifier >= 0x8000: errors.append("method id must have its high bit clear")
    return errors


def validate_vsomeip_route(route: dict, service_model: dict, where: str) -> list[str]:
    errors = []
    mappings = route.get("mappings", {})
    service, instance = mappings.get("service"), mappings.get("instance")
    if not isinstance(service, int) or isinstance(service, bool) or not 1 <= service < 0xffff:
        errors.append(f"{where}: service must be an assignable 16-bit integer")
    if not isinstance(instance, int) or isinstance(instance, bool) or not 0 <= instance < 0xffff:
        errors.append(f"{where}: instance must be an assignable 16-bit integer")
    wire_ids = set()
    models = {}
    for kind in ("events", "methods", "fields"):
        for element in service_model.get(kind, []): models[element["id"]] = (kind, element)
    for element_id, value in mappings.get("elements", {}).items():
        kind, model = models.get(element_id, ("", {}))
        entries = {"value": value}
        if kind == "fields":
            required = set()
            if model.get("readable"): required.add("get")
            if model.get("writable"): required.add("set")
            if model.get("notifiable"): required.add("notify")
            if not isinstance(value, dict) or set(value) != required:
                errors.append(f"{where}.element[{element_id}]: field mapping requires {', '.join(sorted(required))}")
                continue
            entries = value
        for operation, mapping in entries.items():
            for error in vsomeip_mapping_errors(mapping):
                errors.append(f"{where}.element[{element_id}].{operation}: {error}")
            expected_kind = {
                ("events", "value"): "event", ("methods", "value"): "method",
                ("fields", "get"): "fieldGet", ("fields", "set"): "fieldSet",
                ("fields", "notify"): "fieldNotify",
            }.get((kind, operation))
            if isinstance(mapping, dict) and mapping.get("kind") != expected_kind:
                errors.append(f"{where}.element[{element_id}].{operation}: kind must be {expected_kind}")
            if isinstance(mapping, dict) and isinstance(mapping.get("id"), int):
                wire_id = mapping["id"]
                if wire_id in wire_ids: errors.append(f"{where}: duplicate wire element id {wire_id}")
                wire_ids.add(wire_id)
    return errors


def validate_iceoryx2_route(route: dict, service_model: dict, where: str) -> list[str]:
    errors = []
    mappings = route.get("mappings", {})
    service, instance = mappings.get("service"), mappings.get("instance")
    if not isinstance(service, str) or not service or len(service) > 160:
        errors.append(f"{where}: service must be a nonempty bounded string")
    if not isinstance(instance, str) or not instance or len(instance) > 64:
        errors.append(f"{where}: instance must be a nonempty bounded string")
    models = {}
    for kind in ("events", "methods", "fields"):
        for element in service_model.get(kind, []): models[element["id"]] = kind
    native_names = set()
    event_required = {"name", "type", "payloadSize", "alignment", "history",
                      "subscriberBuffer", "maxPublishers", "maxSubscribers", "safeOverflow"}
    method_required = {"name", "requestType", "responseType", "requestPayloadSize",
                       "responsePayloadSize", "alignment", "requestBuffer", "responseBuffer",
                       "maxClients", "maxServers", "safeOverflow"}
    for element_id, mapping in mappings.get("elements", {}).items():
        kind = models.get(element_id)
        element_where = f"{where}.element[{element_id}]"
        if mapping == {"unsupported": True}:
            continue
        entries = {"value": mapping}
        if kind == "fields":
            model = next((value for value in service_model.get("fields", [])
                          if value["id"] == element_id), {})
            expected = set()
            if model.get("readable"): expected.add("get")
            if model.get("writable"): expected.add("set")
            if model.get("notifiable"): expected.add("notify")
            if not isinstance(mapping, dict) or set(mapping) != expected:
                errors.append(f"{element_where}: field mapping requires {', '.join(sorted(expected))}")
                continue
            entries = mapping
        for operation, entry in entries.items():
            is_event = kind == "events" or operation == "notify"
            required = event_required if is_event else method_required
            entry_where = f"{element_where}.{operation}"
            if not isinstance(entry, dict) or set(entry) != required:
                errors.append(f"{entry_where}: mapping has missing or unknown properties")
                continue
            name = entry.get("name")
            if not isinstance(name, str) or not name or len(name) > 64:
                errors.append(f"{entry_where}: name must be a nonempty bounded string")
            elif name in native_names:
                errors.append(f"{where}: duplicate native element name {name}")
            else:
                native_names.add(name)
            type_keys = ("type",) if is_event else ("requestType", "responseType")
            for key in type_keys:
                if not isinstance(entry.get(key), str) or not entry[key]:
                    errors.append(f"{entry_where}: {key} must be a nonempty string")
            number_keys = (("payloadSize", "subscriberBuffer", "maxPublishers", "maxSubscribers")
                           if is_event else
                           ("requestPayloadSize", "responsePayloadSize", "requestBuffer",
                            "responseBuffer", "maxClients", "maxServers"))
            for key in (*number_keys, "alignment"):
                value = entry.get(key)
                if not isinstance(value, int) or isinstance(value, bool) or value < 1:
                    errors.append(f"{entry_where}: {key} must be a positive integer")
            alignment = entry.get("alignment")
            if isinstance(alignment, int) and not isinstance(alignment, bool) \
                    and alignment > 0 and alignment & (alignment - 1):
                errors.append(f"{entry_where}: alignment must be a power of two")
            if is_event:
                history = entry.get("history")
                if not isinstance(history, int) or isinstance(history, bool) or history < 0:
                    errors.append(f"{entry_where}: history must be a nonnegative integer")
            if not isinstance(entry.get("safeOverflow"), bool):
                errors.append(f"{entry_where}: safeOverflow must be boolean")
            route_limit = route.get("limits", {}).get("maxPayloadSize")
            sizes = (entry.get("payloadSize"),) if is_event else \
                    (entry.get("requestPayloadSize"), entry.get("responsePayloadSize"))
            if isinstance(route_limit, int) and any(
                    isinstance(value, int) and value > route_limit for value in sizes):
                errors.append(f"{entry_where}: payload size exceeds route maxPayloadSize")
    return errors


def validate_deployment(contract: dict, deployment: dict, profiles_dir: Path) -> list[str]:
    errors = [f"contract: {item}" for item in validate(contract)] + structural_errors(deployment)
    if errors: return errors
    if deployment.get("deploymentVersion") != 1: errors.append("deploymentVersion must be 1")
    expected = fingerprint(contract)
    if deployment.get("contractFingerprint") != expected:
        errors.append(f"contractFingerprint mismatch: expected {expected}")

    services = {service["id"]: service for service in contract.get("services", [])}
    providers: dict[str, tuple[dict, dict]] = {}
    for provider in deployment.get("providers", []):
        provider_id = provider.get("id")
        if provider_id in providers:
            errors.append(f"provider {provider_id}: duplicate ID")
            continue
        profile_path = profiles_dir / f"{provider.get('profile')}.json"
        if not profile_path.is_file():
            errors.append(f"provider {provider_id}: profile not found")
            continue
        profile = read(profile_path)
        errors.extend(f"provider {provider_id}: {item}" for item in profile_errors(profile, provider.get("profile")))
        providers[provider_id] = (provider, profile)

    instance_ids: set[str] = set()
    for instance in deployment.get("instances", []):
        instance_id = instance.get("instanceId")
        where = f"instance {instance_id}"
        if instance_id in instance_ids: errors.append(f"{where}: duplicate instance ID")
        instance_ids.add(instance_id)
        service = services.get(instance.get("serviceId"))
        if service is None:
            errors.append(f"{where}: unknown service ID")
            continue
        elements = {element["id"] for kind in ("events", "methods", "fields") for element in service[kind]}
        priorities: set[int] = set()
        for route_index, route in enumerate(instance.get("routes", [])):
            route_where = f"{where}.route[{route_index}]"
            provider_entry = providers.get(route.get("provider"))
            if provider_entry is None:
                errors.append(f"{route_where}: unknown provider {route.get('provider')}")
                continue
            provider, profile = provider_entry
            if profile.get("name") == "vsomeip":
                errors.extend(validate_vsomeip_route(route, service, route_where))
            if profile.get("name") == "iceoryx2":
                errors.extend(validate_iceoryx2_route(route, service, route_where))
            if route.get("role") == "consumer":
                priority = route.get("priority")
                if not isinstance(priority, int): errors.append(f"{route_where}: consumer priority required")
                elif priority in priorities: errors.append(f"{route_where}: duplicate consumer priority {priority}")
                else: priorities.add(priority)
            elif "priority" in route:
                errors.append(f"{route_where}: provider route cannot have priority")
            available = set(profile.get("features", []))
            for feature in sorted(set(route.get("requiredFeatures", [])) - available):
                errors.append(f"{route_where}: unsupported feature {feature}")
            available_limits = profile.get("limits", {})
            for limit, requested in route.get("limits", {}).items():
                supported = available_limits.get(limit, 0)
                if requested > supported:
                    errors.append(f"{route_where}: {limit} {requested} exceeds {supported}")
            mappings = route.get("mappings", {}).get("elements", {})
            missing = sorted(elements - set(mappings))
            unknown = sorted(set(mappings) - elements)
            if missing: errors.append(f"{route_where}: missing element mappings {', '.join(missing)}")
            if unknown: errors.append(f"{route_where}: unknown element mappings {', '.join(unknown)}")
    return errors


def make_plan(deployment: dict, profiles_dir: Path) -> dict:
    providers = []
    for provider in sorted(deployment["providers"], key=lambda item: item["id"]):
        profile = read(profiles_dir / f"{provider['profile']}.json")
        providers.append({"id": provider["id"], "profile": provider["profile"],
            "required": provider["required"], "isolation": profile["isolation"],
            "features": sorted(profile["features"]), "limits": profile["limits"],
            "extensions": provider.get("extensions", {})})
    instances = []
    for instance in sorted(deployment["instances"], key=lambda item: item["instanceId"]):
        consumers = sorted((route for route in instance["routes"] if route["role"] == "consumer"), key=lambda item: item["priority"])
        offered = sorted((route for route in instance["routes"] if route["role"] == "provider"), key=lambda item: (not item["required"], item["provider"]))
        instances.append({"serviceId": instance["serviceId"], "instanceId": instance["instanceId"],
            "alias": instance.get("alias", ""), "consumerRoutes": consumers, "providerRoutes": offered})
    return {"planVersion": 1, "contractFingerprint": deployment["contractFingerprint"],
        "deploymentFingerprint": deployment_fingerprint(deployment), "providers": providers,
        "instances": instances}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--contract", required=True, type=Path)
    parser.add_argument("--deployment", required=True, type=Path)
    parser.add_argument("--profiles", required=True, type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--output-plan", type=Path)
    args = parser.parse_args()
    contract, deployment = read(args.contract), read(args.deployment)
    errors = validate_deployment(contract, deployment, args.profiles)
    result = {"valid": not errors, "deploymentFingerprint": deployment_fingerprint(deployment), "errors": errors}
    encoded = json.dumps(result, sort_keys=True, indent=2) + "\n"
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(encoded, encoding="utf-8")
    else: print(encoded, end="")
    if not errors and args.output_plan:
        plan = json.dumps(make_plan(deployment, args.profiles), sort_keys=True, indent=2) + "\n"
        args.output_plan.parent.mkdir(parents=True, exist_ok=True)
        if not args.output_plan.exists() or args.output_plan.read_text(encoding="utf-8") != plan:
            args.output_plan.write_text(plan, encoding="utf-8")
    if errors:
        for error in errors: print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__": raise SystemExit(main())
