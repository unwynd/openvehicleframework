#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Hermetic build actions used by the public OVF Bazel application API."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import uuid

from tools.smithy_ast_to_ir import compile_model
from tools.exec_deployment import (
    finalize_manifest as finalize_execution_manifest,
    generate_artifacts as generate_execution_artifacts,
    validate_semantics as validate_execution_semantics,
    validate_smithy_model as validate_execution_smithy_model,
)
from tools.validate_deployment import (
    deployment_fingerprint,
    make_plan,
    read,
    resolve_deployment,
    validate_deployment,
)
from tools.validate_ir import fingerprint


def write_if_changed(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text(encoding="utf-8") != content:
        path.write_text(content, encoding="utf-8")


def valid_install_path(value: str) -> bool:
    return (
        re.fullmatch(r"[A-Za-z0-9._-]+(?:/[A-Za-z0-9._-]+)*", value) is not None
        and all(part not in {".", ".."} for part in value.split("/"))
    )


def compile_contract(args: argparse.Namespace) -> int:
    with tempfile.TemporaryDirectory(prefix="ovf-contract-") as temporary:
        root = Path(temporary)
        config = {
            "version": "1.0",
            "sources": [str(path.resolve()) for path in args.idl],
            "imports": [str(args.profile.resolve())],
            "projections": {
                "ovf": {"plugins": {"model": {"includePreludeShapes": False}}}
            },
        }
        config_path = root / "smithy-build.json"
        config_path.write_text(json.dumps(config, sort_keys=True), encoding="utf-8")
        output = root / "smithy-output"
        subprocess.run(
            [
                str(args.smithy.resolve()),
                "build",
                "--config",
                str(config_path),
                "--output",
                str(output),
                "--no-color",
            ],
            check=True,
        )
        ast = read(output / "ovf/model/model.json")
    model = compile_model(ast, args.service)
    encoded = json.dumps(model, indent=2, ensure_ascii=False) + "\n"
    write_if_changed(args.ir, encoded)
    metadata = {
        "artifactVersion": 1,
        "contractFingerprint": fingerprint(model),
        "ir": args.ir.name,
        "generatedHeader": args.generated_header,
        "service": args.service,
    }
    write_if_changed(args.metadata, json.dumps(metadata, sort_keys=True, indent=2) + "\n")
    return 0


def compile_deployment(args: argparse.Namespace) -> int:
    contract = read(args.contract)
    with tempfile.TemporaryDirectory(prefix="ovf-cue-") as cue_cache:
        environment = dict(os.environ)
        environment["CUE_CACHE_DIR"] = cue_cache
        environment["CUE_CONFIG_DIR"] = cue_cache
        cue_arguments = [
            str(args.cue.resolve()),
            "export",
            str(args.schema.resolve()),
            str(args.deployment.resolve()),
            str(args.binding.resolve()),
            "--expression",
            "model",
            "--out",
            "json",
        ]
        completed = subprocess.run(
            cue_arguments,
            capture_output=True,
            text=True,
            env=environment,
        )
        if completed.returncode != 0:
            raise ValueError(completed.stderr.strip())
    model = json.loads(completed.stdout)
    deployment = resolve_deployment(contract, model)
    write_if_changed(
        args.deployment_ir,
        json.dumps(deployment, sort_keys=True, indent=2) + "\n",
    )
    with tempfile.TemporaryDirectory(prefix="ovf-profiles-") as temporary:
        profiles = Path(temporary)
        for profile in args.profile:
            shutil.copyfile(profile, profiles / profile.name)
        errors = validate_deployment(contract, deployment, profiles)
        if errors:
            raise ValueError("\n".join(errors))
        plan = make_plan(deployment, profiles)
    report = {
        "valid": True,
        "deploymentFingerprint": deployment_fingerprint(deployment),
        "errors": [],
    }
    write_if_changed(args.plan, json.dumps(plan, sort_keys=True, indent=2) + "\n")
    write_if_changed(args.report, json.dumps(report, sort_keys=True, indent=2) + "\n")
    return 0


def compile_application_model(args: argparse.Namespace) -> int:
    with tempfile.TemporaryDirectory(prefix="ovf-application-cue-") as cue_cache:
        environment = dict(os.environ)
        environment["CUE_CACHE_DIR"] = cue_cache
        environment["CUE_CONFIG_DIR"] = cue_cache
        exported = subprocess.run(
            [
                str(args.cue.resolve()),
                "export",
                str(args.deployment.resolve()),
                "--expression",
                "application",
                "--out",
                "json",
            ],
            capture_output=True,
            text=True,
            env=environment,
        )
        if exported.returncode != 0:
            raise ValueError(exported.stderr.strip())
    application = json.loads(exported.stdout)
    contracts = []
    for specification in args.interface:
        name, separator, path = specification.partition("=")
        if not separator or not name or not path:
            raise ValueError("interface must use name=contract-path syntax")
        contract = read(Path(path))
        service_names = {
            f"{contract['namespace']}#{service['name']}": service
            for service in contract["services"]
        }
        matches = [
            instance
            for instance in application["communication"]["instances"]
            if instance["interface"] in service_names
        ]
        if len(matches) != 1:
            raise ValueError(
                f"interface {name} must bind exactly one application instance"
            )
        instance = matches[0]
        service = service_names[instance["interface"]]
        instance_id = str(
            uuid.uuid5(
                uuid.UUID("91ec7c75-b53f-4b0c-a51c-876eea485f71"),
                f"{instance['interface']}:{instance['instance']}",
            )
        )
        contracts.append({
            "name": name,
            "serviceId": service["id"],
            "instanceId": instance_id,
            "instance": instance["instance"],
            "interface": instance["interface"],
            "role": instance["role"],
            "transport": instance["transport"],
        })
    model = {
        "applicationModelVersion": 1,
        "name": application["name"],
        "interfaces": sorted(contracts, key=lambda item: item["name"]),
    }
    write_if_changed(args.output, json.dumps(model, sort_keys=True, indent=2) + "\n")
    return 0


def _native_mapping(service: int, instance: int, entry: dict) -> str:
    return (f"service={service};instance={instance};element={entry.get('id', 0)};"
            f"eventGroup={entry.get('eventGroup', 0)};major={entry.get('major', 0)};"
            f"minor={entry.get('minor', 0)};kind={entry.get('kind', '')};"
            f"reliable={str(entry.get('reliable', False)).lower()}")


def _iceoryx_event(base: str, entry: dict) -> str:
    return (f"pattern=pubsub;service={base}/{entry.get('name', '')};type={entry.get('type', '')};"
            f"payloadSize={entry.get('payloadSize', 0)};alignment={entry.get('alignment', 0)};"
            f"history={entry.get('history', 0)};subscriberBuffer={entry.get('subscriberBuffer', 0)};"
            f"maxPublishers={entry.get('maxPublishers', 0)};maxSubscribers={entry.get('maxSubscribers', 0)};"
            f"safeOverflow={str(entry.get('safeOverflow', False)).lower()}")


def _iceoryx_method(base: str, entry: dict) -> str:
    return (f"pattern=requestResponse;service={base}/{entry.get('name', '')};"
            f"requestType={entry.get('requestType', '')};responseType={entry.get('responseType', '')};"
            f"requestPayloadSize={entry.get('requestPayloadSize', 0)};"
            f"responsePayloadSize={entry.get('responsePayloadSize', 0)};alignment={entry.get('alignment', 0)};"
            f"requestBuffer={entry.get('requestBuffer', 0)};responseBuffer={entry.get('responseBuffer', 0)};"
            f"maxClients={entry.get('maxClients', 0)};maxServers={entry.get('maxServers', 0)};"
            f"safeOverflow={str(entry.get('safeOverflow', False)).lower()}")


def _runtime_route(plan: dict) -> tuple[dict, dict]:
    instance = plan["instances"][0]
    route = instance["providerRoutes"][0] if instance["providerRoutes"] else instance["consumerRoutes"][0]
    provider = next(item for item in plan["providers"] if item["id"] == route["provider"])
    mapping = route["mappings"]
    profile = provider["profile"]
    numeric = isinstance(mapping["service"], int)
    base = f"{mapping['service']}/{mapping['instance']}"
    elements = []
    for element_id, value in mapping["elements"].items():
        event = method = ""
        if profile == "iceoryx2":
            if "notify" in value or "get" in value or "set" in value:
                event = _iceoryx_event(base, value["notify"]) if "notify" in value else ""
                operation = value.get("get", value.get("set"))
                method = _iceoryx_method(base, operation) if operation else ""
            elif "requestType" in value:
                method = _iceoryx_method(base, value)
            else:
                event = _iceoryx_event(base, value)
        elif isinstance(value, str):
            event = method = value
        elif "id" in value:
            native = _native_mapping(mapping["service"], mapping["instance"], value)
            if value.get("kind") in {"event", "fieldNotify"}: event = native
            else: method = native
        else:
            if "notify" in value: event = _native_mapping(mapping["service"], mapping["instance"], value["notify"])
            operation = value.get("get", value.get("set"))
            if operation: method = _native_mapping(mapping["service"], mapping["instance"], operation)
        elements.append({"id": element_id, "event": event, "method": method})
    service_mapping = (_native_mapping(mapping["service"], mapping["instance"],
                                       {"kind": "method", "reliable": True, "major": 1})
                       if numeric else base)
    runtime_route = {
        "serviceId": instance["serviceId"], "instanceId": instance["instanceId"],
        "instance": instance["alias"], "provider": profile, "nativeService": service_mapping,
        "elements": sorted(elements, key=lambda item: item["id"]),
        "maxPayloadSize": route["limits"]["maxPayloadSize"],
        "historyDepth": route["limits"]["maxHistoryDepth"],
        "priority": route.get("priority", 0),
    }
    transport = {"provider": profile, "configuration": "", "maxEndpoints": route["limits"]["maxEndpoints"],
                 "maxOutstandingOperations": route["limits"]["maxOutstandingOperations"]}
    return runtime_route, transport


def compile_communication_deployment(args: argparse.Namespace) -> int:
    if not (len(args.application_model) == len(args.application_deployment) == len(args.application_name)):
        raise ValueError("application communication inputs must have matching lengths")
    args.output.mkdir(parents=True, exist_ok=True)
    binding_values = []
    for binding_path in args.binding:
        with tempfile.TemporaryDirectory(prefix="ovf-com-binding-cue-") as cue_cache:
            environment = dict(os.environ, CUE_CACHE_DIR=cue_cache, CUE_CONFIG_DIR=cue_cache)
            completed = subprocess.run([str(args.cue.resolve()), "export", str(binding_path.resolve()),
                "--expression", "bindings", "--out", "json"], capture_output=True, text=True,
                env=environment)
            if completed.returncode: raise ValueError(completed.stderr.strip())
        binding_values.extend(json.loads(completed.stdout))
    transports = [item["transport"] for item in binding_values]
    if len(transports) != len(set(transports)):
        raise ValueError("system communication deployment selects a transport more than once")
    with tempfile.TemporaryDirectory(prefix="ovf-com-profiles-") as temporary:
        profiles = Path(temporary)
        for profile in args.profile: shutil.copyfile(profile, profiles / profile.name)
        for name, model_path, deployment_path in zip(args.application_name, args.application_model,
                                                     args.application_deployment, strict=True):
            model = read(model_path)
            routes, transports = [], {}
            for specification in args.contract:
                owner, separator, contract_path = specification.partition("=")
                if not separator or owner != name: continue
                contract = read(Path(contract_path))
                with tempfile.TemporaryDirectory(prefix="ovf-com-cue-") as cue_cache:
                    environment = dict(os.environ, CUE_CACHE_DIR=cue_cache, CUE_CONFIG_DIR=cue_cache)
                    completed = subprocess.run([str(args.cue.resolve()), "export",
                        str(deployment_path.resolve()), "--expression", "application", "--out", "json"],
                        capture_output=True, text=True, env=environment)
                    if completed.returncode: raise ValueError(completed.stderr.strip())
                application = json.loads(completed.stdout)
                deployment = resolve_deployment(contract, {
                    "deployment": {"deploymentVersion": application["schemaVersion"],
                                   "instances": application["communication"]["instances"]},
                    "bindings": binding_values,
                })
                errors = validate_deployment(contract, deployment, profiles)
                if errors: raise ValueError("\n".join(errors))
                plan = make_plan(deployment, profiles)
                route, transport = _runtime_route(plan)
                routes.append(route); transports[transport["provider"]] = transport
            if len(routes) != len(model["interfaces"]):
                raise ValueError(f"application {name} did not resolve every interface")
            output = {"runtimeDeploymentVersion": 1, "application": name,
                      "transports": sorted(transports.values(), key=lambda item: item["provider"]),
                      "routes": sorted(routes, key=lambda item: (item["serviceId"], item["instance"]))}
            write_if_changed(args.output / f"{name}.json", json.dumps(output, sort_keys=True, indent=2) + "\n")
    return 0


def compile_execution_deployment(args: argparse.Namespace) -> int:
    with tempfile.TemporaryDirectory(prefix="ovf-exec-smithy-") as temporary:
        root = Path(temporary)
        config = {
            "version": "1.0",
            "sources": [str(args.model.resolve())],
            "projections": {
                "ovf": {"plugins": {"model": {"includePreludeShapes": True}}}
            },
        }
        config_path = root / "smithy-build.json"
        config_path.write_text(json.dumps(config, sort_keys=True), encoding="utf-8")
        output = root / "smithy-output"
        subprocess.run(
            [
                str(args.smithy.resolve()),
                "build",
                "--config",
                str(config_path),
                "--output",
                str(output),
                "--no-color",
            ],
            check=True,
        )
        schema_ast = read(output / "ovf/model/model.json")
    with tempfile.TemporaryDirectory(prefix="ovf-exec-cue-") as cue_cache:
        environment = dict(os.environ)
        environment["CUE_CACHE_DIR"] = cue_cache
        environment["CUE_CONFIG_DIR"] = cue_cache
        application_inputs = list(zip(
            args.application,
            args.application_executable,
            args.application_install_path,
            args.application_target,
            strict=True,
        ))
        applications = []
        application_artifacts = []
        for application, executable, install_path, target in application_inputs:
            exported = subprocess.run(
                [
                    str(args.cue.resolve()),
                    "export",
                    str(application.resolve()),
                    "--expression",
                    "{schemaVersion: application.schemaVersion, "
                    "name: application.name, execution: application.execution}",
                    "--out",
                    "json",
                ],
                capture_output=True,
                text=True,
                env=environment,
            )
            if exported.returncode != 0:
                raise ValueError(
                    f"application execution fragment is invalid: {application}\n"
                    f"{exported.stderr.strip()}"
                )
            fragment = json.loads(exported.stdout)
            fragment["executableRelativePath"] = install_path
            applications.append(fragment)
            application_artifacts.append({
                "name": fragment["name"],
                "bazelTarget": target,
                "installPath": install_path,
                "sha256": digest(executable),
            })
        allocation_export = subprocess.run(
            [
                str(args.cue.resolve()),
                "export",
                str(args.allocation.resolve()),
                "--expression",
                "allocation",
                "--out",
                "json",
            ],
            capture_output=True,
            text=True,
            env=environment,
        )
        if allocation_export.returncode != 0:
            raise ValueError(
                f"execution allocation is invalid: {args.allocation}\n"
                f"{allocation_export.stderr.strip()}"
            )
        allocation = json.loads(allocation_export.stdout)
        mismatched_unit_names = sorted(
            name
            for name, unit in allocation.get("units", {}).items()
            if unit.get("name") != name
        )
        if mismatched_unit_names:
            raise ValueError(
                "execution-unit allocation keys must match their names: "
                + ", ".join(mismatched_unit_names)
            )
        fragment_names = [application.get("name") for application in applications]
        if any(not isinstance(name, str) or not name for name in fragment_names):
            raise ValueError(
                "every application deployment fragment must define a non-empty name"
            )
        duplicate_names = sorted(
            {
                name
                for name in fragment_names
                if fragment_names.count(name) > 1
            }
        )
        if duplicate_names:
            raise ValueError(
                "duplicate application deployment fragments: "
                + ", ".join(duplicate_names)
            )
        allocated_names = {
            name
            for name, unit in allocation.get("units", {}).items()
            if unit.get("kind") == "managed_application"
        }
        supplied_names = set(fragment_names)
        if allocated_names != supplied_names:
            missing = sorted(allocated_names - supplied_names)
            unexpected = sorted(supplied_names - allocated_names)
            details = []
            if missing:
                details.append("missing application targets: " + ", ".join(missing))
            if unexpected:
                details.append(
                    "unallocated application targets: " + ", ".join(unexpected)
                )
            raise ValueError("; ".join(details))
        application_values = Path(cue_cache) / "applications.cue"
        application_values.write_text(
            "package ovf_exec_deployment\n\napplicationFragments: "
            + json.dumps(applications, sort_keys=True)
            + "\n",
            encoding="utf-8",
        )
        completed = subprocess.run(
            [
                str(args.cue.resolve()),
                "export",
                str(args.schema.resolve()),
                str(args.allocation.resolve()),
                str(application_values),
                str(args.platform.resolve()),
                "--expression",
                "model",
                "--out",
                "json",
            ],
            capture_output=True,
            text=True,
            env=environment,
        )
        if completed.returncode != 0:
            raise ValueError(completed.stderr.strip())
    model = json.loads(completed.stdout)
    errors = validate_execution_smithy_model(schema_ast, model)
    errors.extend(validate_execution_semantics(model))
    if errors:
        raise ValueError("\n".join(errors))
    generate_execution_artifacts(
        model,
        schema_ast,
        args.execution_ir,
        args.backend_config,
        args.metadata,
        application_artifacts,
    )
    return 0


def finalize_execution_deployment(args: argparse.Namespace) -> int:
    finalize_execution_manifest(args.metadata, args.services, args.manifest)
    return 0


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tar_bytes(content: bytes, mode: int) -> tarfile.TarInfo:
    info = tarfile.TarInfo()
    info.size = len(content)
    info.mode = mode
    info.mtime = 0
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    return info


def add_file(archive: tarfile.TarFile, source: Path, target: str, mode: int) -> None:
    content = source.read_bytes()
    info = tar_bytes(content, mode)
    info.name = target
    archive.addfile(info, io.BytesIO(content))


def package_execution_target(args: argparse.Namespace) -> int:
    entries: dict[str, tuple[bytes, int]] = {}

    def insert(path: str, content: bytes, mode: int) -> None:
        normalized = str(Path(path))
        if path.startswith("/") or normalized == "." or ".." in Path(path).parts:
            raise ValueError(f"unsafe target bundle path: {path}")
        existing = entries.get(normalized)
        value = (content, mode & 0o777)
        if existing is not None and existing != value:
            raise ValueError(f"conflicting target bundle path: {normalized}")
        entries[normalized] = value

    def merge(archive_path: Path) -> dict[str, Any] | None:
        manifest: dict[str, Any] | None = None
        with tarfile.open(archive_path) as archive:
            for member in archive.getmembers():
                if member.isdir():
                    continue
                if not member.isfile():
                    raise ValueError(
                        f"unsupported archive member in {archive_path}: {member.name}"
                    )
                source = archive.extractfile(member)
                if source is None:
                    raise ValueError(f"cannot read archive member: {member.name}")
                content = source.read()
                insert(member.name, content, member.mode)
                if member.name.startswith("share/ovf/") and member.name.endswith(
                    "/manifest.json"
                ):
                    candidate = json.loads(content)
                    if candidate.get("applicationBundleVersion") == 2:
                        manifest = candidate
        return manifest

    execution_manifest = read(args.deployment_manifest)
    expected_applications = {
        item["name"]: item for item in execution_manifest["applicationArtifacts"]
    }
    actual_applications: dict[str, dict[str, Any]] = {}
    for bundle in args.application_bundle:
        manifest = merge(bundle)
        if manifest is None:
            raise ValueError(f"application bundle has no manifest: {bundle}")
        name = manifest["name"]
        if name in actual_applications:
            raise ValueError(f"duplicate application bundle: {name}")
        actual_applications[name] = manifest
    if set(actual_applications) != set(expected_applications):
        raise ValueError("application bundles do not match the execution deployment")
    for name, expected in expected_applications.items():
        executable = actual_applications[name]["files"]["executable"]
        if executable["path"] != expected["installPath"]:
            raise ValueError(f"application install path mismatch: {name}")
        if executable["sha256"] != expected["sha256"]:
            raise ValueError(f"application executable digest mismatch: {name}")

    for bundle in args.platform_bundle:
        merge(bundle)
    insert("etc/ovf/exec/deployment.execution.json", args.execution_model.read_bytes(), 0o644)
    insert("etc/ovf/exec/deployment.backend.json", args.backend_config.read_bytes(), 0o644)
    runtime_manifest = dict(execution_manifest)
    runtime_manifest["backendConfiguration"] = "deployment.backend.json"
    runtime_manifest["servicesDirectory"] = "dinit.d"
    runtime_manifest_content = (
        json.dumps(runtime_manifest, sort_keys=True, indent=2).encode() + b"\n"
    )
    insert("usr/share/ovf/exec/manifest.json", runtime_manifest_content, 0o644)
    for service in sorted(args.services.iterdir()):
        if service.is_file():
            insert(f"etc/dinit.d/{service.name}", service.read_bytes(), 0o644)
    for deployment in sorted(args.communication_deployment.iterdir()):
        if deployment.is_file():
            insert(f"etc/ovf/com/{deployment.name}", deployment.read_bytes(), 0o644)
    insert("usr/sbin/ovf-execd", args.daemon.read_bytes(), 0o755)
    insert("usr/sbin/dinit", args.dinit.read_bytes(), 0o755)
    insert("usr/lib/libovf_exec_backend_dinit.so", args.backend_plugin.read_bytes(), 0o755)
    for provider in args.provider:
        insert(f"usr/lib/ovf/providers/{provider.name}", provider.read_bytes(), 0o755)

    execution_model = read(args.execution_model)
    required_base_executables = sorted({
        executable
        for unit in execution_model["units"]
        for executable in (unit["executable"], unit["stopExecutable"])
        if executable and executable.lstrip("/") not in entries
    })
    inventory = {
        "targetBundleVersion": 1,
        "executionGeneration": execution_manifest["modelGeneration"],
        "executionManifestSha256": hashlib.sha256(runtime_manifest_content).hexdigest(),
        "applications": sorted(expected_applications),
        "requiredBaseExecutables": required_base_executables,
        "files": {
            path: {"sha256": hashlib.sha256(content).hexdigest(), "mode": f"{mode:04o}"}
            for path, (content, mode) in sorted(entries.items())
        },
    }
    inventory_content = json.dumps(inventory, sort_keys=True, indent=2).encode() + b"\n"
    insert("usr/share/ovf/target/manifest.json", inventory_content, 0o644)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(args.output, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        for path, (content, mode) in sorted(entries.items()):
            info = tar_bytes(content, mode)
            info.name = path
            archive.addfile(info, io.BytesIO(content))
    return 0


def package_application(args: argparse.Namespace) -> int:
    install_path = args.install_path
    if not valid_install_path(install_path):
        raise ValueError("application install path must be a normalized relative path")
    model = read(args.application_model)
    contracts = [(path, read(path)) for path in args.contract]
    manifest = {
        "applicationBundleVersion": 2,
        "name": args.name,
        "requiredProviderProfiles": [],
        "frameworkIncluded": False,
        "files": {
            "executable": {
                "path": install_path,
                "sha256": digest(args.executable),
            },
        },
        "interfaces": model["interfaces"],
    }
    manifest["files"]["applicationModel"] = {
        "path": f"share/ovf/{args.name}/application.json",
        "sha256": digest(args.application_model),
    }
    manifest["files"]["deploymentIntent"] = {
        "path": f"share/ovf/{args.name}/deployment.cue",
        "sha256": digest(args.application_deployment),
    }
    manifest_content = json.dumps(manifest, sort_keys=True, indent=2).encode() + b"\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(args.output, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        add_file(archive, args.executable, install_path, 0o755)
        add_file(archive, args.application_model,
                 f"share/ovf/{args.name}/application.json", 0o644)
        add_file(archive, args.application_deployment,
                 f"share/ovf/{args.name}/deployment.cue", 0o644)
        for index, (path, _) in enumerate(contracts):
            add_file(archive, path,
                     f"share/ovf/{args.name}/contract-{index}.ovf-ir.json", 0o644)
        info = tar_bytes(manifest_content, 0o644)
        info.name = f"share/ovf/{args.name}/manifest.json"
        archive.addfile(info, io.BytesIO(manifest_content))
    return 0


def package_vsomeip_platform(args: argparse.Namespace) -> int:
    runtime_targets = {
        "routingmanagerd": ("usr/bin/routingmanagerd", 0o755),
        "libovf_com.so": ("usr/lib/libovf_com.so", 0o755),
        "libovf_exec_application.so": ("usr/lib/libovf_exec_application.so", 0o755),
        "libovf_com_provider_vsomeip.so": (
            "usr/lib/ovf/providers/libovf_com_provider_vsomeip.so",
            0o755,
        ),
        "libvsomeip3.so": ("usr/lib/libvsomeip3.so", 0o755),
        "libvsomeip3.so.3": ("usr/lib/libvsomeip3.so.3", 0o755),
        "libvsomeip3-cfg.so": ("usr/lib/libvsomeip3-cfg.so", 0o755),
        "libvsomeip3-cfg.so.3": ("usr/lib/libvsomeip3-cfg.so.3", 0o755),
        "libvsomeip3-e2e.so": ("usr/lib/libvsomeip3-e2e.so", 0o755),
        "libvsomeip3-e2e.so.3": ("usr/lib/libvsomeip3-e2e.so.3", 0o755),
        "libvsomeip3-sd.so": ("usr/lib/libvsomeip3-sd.so", 0o755),
        "libvsomeip3-sd.so.3": ("usr/lib/libvsomeip3-sd.so.3", 0o755),
    }
    sources: dict[str, Path] = {}
    for source in args.runtime:
        if source.name in runtime_targets:
            if source.name in sources:
                raise ValueError(f"duplicate platform runtime artifact: {source.name}")
            sources[source.name] = source
    missing = sorted(set(runtime_targets) - set(sources))
    if missing:
        raise ValueError(f"missing platform runtime artifacts: {', '.join(missing)}")

    entries = [
        (target, sources[name], mode)
        for name, (target, mode) in runtime_targets.items()
    ]
    entries.extend(
        [
            ("etc/vsomeip.json", args.configuration, 0o644),
            (
                "usr/lib/systemd/system/ovf-vsomeip-routing.service",
                args.service_unit,
                0o644,
            ),
            (
                "usr/share/licenses/openvehicleframework/LICENSE",
                args.framework_license,
                0o644,
            ),
            ("usr/share/licenses/vsomeip/LICENSE", args.vsomeip_license, 0o644),
            ("usr/share/licenses/boost/LICENSE_1_0.txt", args.boost_license, 0o644),
        ]
    )
    files = {
        target: {"sha256": digest(source), "mode": f"{mode:04o}"}
        for target, source, mode in sorted(entries)
    }
    manifest = {
        "platformBundleVersion": 1,
        "name": "ovf-vsomeip",
        "transport": "vsomeip",
        "routingApplication": "routingmanagerd",
        "configuration": "etc/vsomeip.json",
        "serviceUnit": "usr/lib/systemd/system/ovf-vsomeip-routing.service",
        "applicationsIncluded": False,
        "files": files,
    }
    manifest_content = json.dumps(manifest, sort_keys=True, indent=2).encode() + b"\n"
    entries.append(
        (
            "usr/share/ovf/platform/vsomeip/manifest.json",
            None,
            0o644,
        )
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(args.output, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        for target, source, mode in sorted(entries):
            if source is None:
                info = tar_bytes(manifest_content, mode)
                info.name = target
                archive.addfile(info, io.BytesIO(manifest_content))
            else:
                add_file(archive, source, target, mode)
    return 0


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    commands = result.add_subparsers(dest="command", required=True)
    contract = commands.add_parser("contract")
    contract.add_argument("--smithy", required=True, type=Path)
    contract.add_argument("--profile", required=True, type=Path)
    contract.add_argument("--idl", required=True, action="append", type=Path)
    contract.add_argument("--service")
    contract.add_argument("--ir", required=True, type=Path)
    contract.add_argument("--generated-header", required=True)
    contract.add_argument("--metadata", required=True, type=Path)
    contract.set_defaults(run=compile_contract)

    deployment = commands.add_parser("deployment")
    deployment.add_argument("--cue", required=True, type=Path)
    deployment.add_argument("--schema", required=True, type=Path)
    deployment.add_argument("--contract", required=True, type=Path)
    deployment.add_argument("--deployment", required=True, type=Path)
    deployment.add_argument("--binding", required=True, type=Path)
    deployment.add_argument("--deployment-ir", required=True, type=Path)
    deployment.add_argument("--profile", required=True, action="append", type=Path)
    deployment.add_argument("--plan", required=True, type=Path)
    deployment.add_argument("--report", required=True, type=Path)
    deployment.set_defaults(run=compile_deployment)

    execution = commands.add_parser("execution-deployment")
    execution.add_argument("--smithy", required=True, type=Path)
    execution.add_argument("--model", required=True, type=Path)
    execution.add_argument("--cue", required=True, type=Path)
    execution.add_argument("--schema", required=True, type=Path)
    execution.add_argument("--allocation", required=True, type=Path)
    execution.add_argument("--application", required=True, action="append", type=Path)
    execution.add_argument(
        "--application-executable", required=True, action="append", type=Path
    )
    execution.add_argument(
        "--application-install-path", required=True, action="append"
    )
    execution.add_argument("--application-target", required=True, action="append")
    execution.add_argument("--platform", required=True, type=Path)
    execution.add_argument("--execution-ir", required=True, type=Path)
    execution.add_argument("--backend-config", required=True, type=Path)
    execution.add_argument("--metadata", required=True, type=Path)
    execution.set_defaults(run=compile_execution_deployment)

    execution_manifest = commands.add_parser("execution-manifest")
    execution_manifest.add_argument("--metadata", required=True, type=Path)
    execution_manifest.add_argument("--services", required=True, type=Path)
    execution_manifest.add_argument("--manifest", required=True, type=Path)
    execution_manifest.set_defaults(run=finalize_execution_deployment)
    application_model = commands.add_parser("application-model")
    application_model.add_argument("--cue", required=True, type=Path)
    application_model.add_argument("--deployment", required=True, type=Path)
    application_model.add_argument("--interface", required=True, action="append")
    application_model.add_argument("--output", required=True, type=Path)
    application_model.set_defaults(run=compile_application_model)

    communication = commands.add_parser("communication-deployment")
    communication.add_argument("--cue", required=True, type=Path)
    communication.add_argument("--binding", required=True, action="append", type=Path)
    communication.add_argument("--application-name", required=True, action="append")
    communication.add_argument("--application-model", required=True, action="append", type=Path)
    communication.add_argument("--application-deployment", required=True, action="append", type=Path)
    communication.add_argument("--contract", required=True, action="append")
    communication.add_argument("--profile", required=True, action="append", type=Path)
    communication.add_argument("--output", required=True, type=Path)
    communication.set_defaults(run=compile_communication_deployment)

    package = commands.add_parser("package")
    package.add_argument("--name", required=True)
    package.add_argument("--executable", required=True, type=Path)
    package.add_argument("--install-path", required=True)
    package.add_argument("--contract", required=True, action="append", type=Path)
    package.add_argument("--application-model", required=True, type=Path)
    package.add_argument("--application-deployment", required=True, type=Path)
    package.add_argument("--output", required=True, type=Path)
    package.set_defaults(run=package_application)

    platform = commands.add_parser("package-vsomeip-platform")
    platform.add_argument("--runtime", required=True, action="append", type=Path)
    platform.add_argument("--configuration", required=True, type=Path)
    platform.add_argument("--service-unit", required=True, type=Path)
    platform.add_argument("--framework-license", required=True, type=Path)
    platform.add_argument("--vsomeip-license", required=True, type=Path)
    platform.add_argument("--boost-license", required=True, type=Path)
    platform.add_argument("--output", required=True, type=Path)
    platform.set_defaults(run=package_vsomeip_platform)
    target = commands.add_parser("package-execution-target")
    target.add_argument("--execution-model", required=True, type=Path)
    target.add_argument("--backend-config", required=True, type=Path)
    target.add_argument("--deployment-manifest", required=True, type=Path)
    target.add_argument("--services", required=True, type=Path)
    target.add_argument("--communication-deployment", required=True, type=Path)
    target.add_argument("--daemon", required=True, type=Path)
    target.add_argument("--backend-plugin", required=True, type=Path)
    target.add_argument("--dinit", required=True, type=Path)
    target.add_argument("--application-bundle", required=True, action="append", type=Path)
    target.add_argument("--platform-bundle", action="append", type=Path, default=[])
    target.add_argument("--provider", action="append", type=Path, default=[])
    target.add_argument("--output", required=True, type=Path)
    target.set_defaults(run=package_execution_target)
    return result


def main() -> int:
    args = parser().parse_args()
    return args.run(args)


if __name__ == "__main__":
    raise SystemExit(main())
