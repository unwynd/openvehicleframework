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
            str(args.platform.resolve()),
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
                    if candidate.get("applicationBundleVersion") == 1:
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
    plans = [read(path) for path in args.plan]
    required = sorted({
        provider["profile"]
        for plan in plans
        for provider in plan["providers"]
        if provider["required"]
    })
    interfaces = []
    for index, (contract, deployment, plan, report) in enumerate(
        zip(args.contract, args.deployment, args.plan, args.report, strict = True)
    ):
        resolved_plan = plans[index]
        resolved_contract = read(contract)
        interfaces.append({
            "name": resolved_contract["services"][0]["name"],
            "contractFingerprint": resolved_plan["contractFingerprint"],
            "deploymentFingerprint": resolved_plan["deploymentFingerprint"],
            "contract": contract,
            "deployment": deployment,
            "plan": plan,
            "report": report,
        })
    manifest = {
        "applicationBundleVersion": 1,
        "name": args.name,
        "requiredProviderProfiles": required,
        "frameworkIncluded": False,
        "files": {
            "executable": {
                "path": install_path,
                "sha256": digest(args.executable),
            },
        },
        "interfaces": [],
    }
    if len(interfaces) == 1:
        interface = interfaces[0]
        manifest["contractFingerprint"] = interface["contractFingerprint"]
        manifest["deploymentFingerprint"] = interface["deploymentFingerprint"]
        manifest["files"].update({
            "contract": {
                "path": f"share/ovf/{args.name}/contract.ovf-ir.json",
                "sha256": digest(interface["contract"]),
            },
            "deployment": {
                "path": f"etc/ovf/{args.name}/deployment.json",
                "sha256": digest(interface["deployment"]),
            },
            "plan": {
                "path": f"etc/ovf/{args.name}/plan.json",
                "sha256": digest(interface["plan"]),
            },
            "validationReport": {
                "path": f"share/ovf/{args.name}/deployment-validation.json",
                "sha256": digest(interface["report"]),
            },
        })
    for index, interface in enumerate(interfaces):
        stem = f"interface-{index}"
        manifest["interfaces"].append({
            "name": interface["name"],
            "contractFingerprint": interface["contractFingerprint"],
            "deploymentFingerprint": interface["deploymentFingerprint"],
            "files": {
                "contract": f"share/ovf/{args.name}/{stem}.ovf-ir.json",
                "deployment": f"etc/ovf/{args.name}/{stem}.deployment.json",
                "plan": f"etc/ovf/{args.name}/{stem}.plan.json",
                "validationReport": f"share/ovf/{args.name}/{stem}.validation.json",
            },
        })
    manifest_content = json.dumps(manifest, sort_keys=True, indent=2).encode() + b"\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(args.output, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        add_file(archive, args.executable, install_path, 0o755)
        for index, interface in enumerate(interfaces):
            if len(interfaces) == 1:
                add_file(archive, interface["deployment"],
                         f"etc/ovf/{args.name}/deployment.json", 0o644)
                add_file(archive, interface["plan"],
                         f"etc/ovf/{args.name}/plan.json", 0o644)
                add_file(archive, interface["contract"],
                         f"share/ovf/{args.name}/contract.ovf-ir.json", 0o644)
                add_file(archive, interface["report"],
                         f"share/ovf/{args.name}/deployment-validation.json", 0o644)
            else:
                stem = f"interface-{index}"
                add_file(archive, interface["deployment"],
                         f"etc/ovf/{args.name}/{stem}.deployment.json", 0o644)
                add_file(archive, interface["plan"],
                         f"etc/ovf/{args.name}/{stem}.plan.json", 0o644)
                add_file(archive, interface["contract"],
                         f"share/ovf/{args.name}/{stem}.ovf-ir.json", 0o644)
                add_file(archive, interface["report"],
                         f"share/ovf/{args.name}/{stem}.validation.json", 0o644)
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
    deployment.add_argument("--platform", required=True, type=Path)
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

    package = commands.add_parser("package")
    package.add_argument("--name", required=True)
    package.add_argument("--executable", required=True, type=Path)
    package.add_argument("--install-path", required=True)
    package.add_argument("--contract", required=True, action="append", type=Path)
    package.add_argument("--deployment", required=True, action="append", type=Path)
    package.add_argument("--plan", required=True, action="append", type=Path)
    package.add_argument("--report", required=True, action="append", type=Path)
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
