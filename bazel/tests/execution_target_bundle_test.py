# SPDX-License-Identifier: Apache-2.0

"""Verify deterministic execution target assembly and application binding."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import tarfile
import tempfile

from tools.ovf_build import package_execution_target


def add(archive: tarfile.TarFile, path: str, content: bytes, mode: int = 0o644) -> None:
    member = tarfile.TarInfo(path)
    member.size = len(content)
    member.mode = mode
    archive.addfile(member, io.BytesIO(content))


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="ovf-target-bundle-test-") as temporary:
        root = Path(temporary)
        executable = b"application-binary"
        install_path = "opt/camera/bin/camera"
        application = root / "camera.tar"
        app_manifest = {
            "applicationBundleVersion": 1,
            "name": "camera",
            "files": {
                "executable": {
                    "path": install_path,
                    "sha256": hashlib.sha256(executable).hexdigest(),
                }
            },
        }
        with tarfile.open(application, "w") as archive:
            add(archive, install_path, executable, 0o755)
            add(
                archive,
                "share/ovf/camera/manifest.json",
                json.dumps(app_manifest).encode(),
            )
        model = root / "model.json"
        model.write_text('{"generation":1,"units":[]}\n', encoding="utf-8")
        backend = root / "backend.json"
        backend.write_text("{}\n", encoding="utf-8")
        deployment_manifest = root / "deployment-manifest.json"
        deployment_manifest.write_text(
            json.dumps({
                "artifactVersion": 2,
                "modelGeneration": 1,
                "applicationArtifacts": [{
                    "name": "camera",
                    "bazelTarget": "//examples/camera:camera",
                    "installPath": install_path,
                    "sha256": hashlib.sha256(executable).hexdigest(),
                }],
            }),
            encoding="utf-8",
        )
        services = root / "services"
        services.mkdir()
        (services / "boot").write_text("type = internal\n", encoding="utf-8")
        binaries = []
        for name in ("daemon", "plugin", "dinit"):
            path = root / name
            path.write_bytes(name.encode())
            binaries.append(path)
        output = root / "target.tar"
        package_execution_target(argparse.Namespace(
            execution_model=model,
            backend_config=backend,
            deployment_manifest=deployment_manifest,
            services=services,
            daemon=binaries[0],
            backend_plugin=binaries[1],
            dinit=binaries[2],
            application_bundle=[application],
            platform_bundle=[],
            provider=[],
            output=output,
        ))
        with tarfile.open(output) as archive:
            names = archive.getnames()
            if names != sorted(names):
                raise SystemExit("target bundle is not deterministically ordered")
            if install_path not in names or "etc/dinit.d/boot" not in names:
                raise SystemExit(f"target bundle is incomplete: {names}")
            source = archive.extractfile("usr/share/ovf/target/manifest.json")
            if source is None:
                raise SystemExit("target manifest is missing")
            manifest = json.load(source)
            if manifest["applications"] != ["camera"]:
                raise SystemExit("target manifest lost the application binding")
            runtime_source = archive.extractfile("usr/share/ovf/exec/manifest.json")
            if runtime_source is None:
                raise SystemExit("runtime execution manifest is missing")
            runtime_manifest = json.load(runtime_source)
            if runtime_manifest.get("backendConfiguration") != "deployment.backend.json":
                raise SystemExit("runtime backend path was not relocated")
            if runtime_manifest.get("servicesDirectory") != "dinit.d":
                raise SystemExit("runtime services path was not relocated")


if __name__ == "__main__":
    main()
