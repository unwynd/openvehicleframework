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
        applications = []
        for application in args.application:
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
            applications.append(json.loads(exported.stdout))
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
        allocated_names = set(allocation.get("applications", {}))
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


def package_application(args: argparse.Namespace) -> int:
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
                "path": f"bin/{args.name}",
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
        add_file(archive, args.executable, f"bin/{args.name}", 0o755)
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
    return result


def main() -> int:
    args = parser().parse_args()
    return args.run(args)


if __name__ == "__main__":
    raise SystemExit(main())
