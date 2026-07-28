# SPDX-License-Identifier: Apache-2.0

"""Black-box verification for the public application build API."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import tarfile


def main() -> None:
    root = Path(os.environ["TEST_SRCDIR"]) / "_main"
    bundle = root / "examples/radar/radar_inproc_client_bundle.tar"
    with tarfile.open(bundle) as archive:
        expected = [
            "bin/radar_inproc_client",
            "etc/ovf/radar_inproc_client/deployment.json",
            "etc/ovf/radar_inproc_client/plan.json",
            "share/ovf/radar_inproc_client/contract.ovf-ir.json",
            "share/ovf/radar_inproc_client/deployment-validation.json",
            "share/ovf/radar_inproc_client/manifest.json",
        ]
        if archive.getnames() != expected:
            raise SystemExit(f"unexpected application bundle: {archive.getnames()}")
        for member in archive.getmembers():
            if member.mtime != 0 or member.uid != 0 or member.gid != 0:
                raise SystemExit(f"non-reproducible tar metadata: {member.name}")
        manifest_source = archive.extractfile(
            "share/ovf/radar_inproc_client/manifest.json"
        )
        if manifest_source is None:
            raise SystemExit("application manifest is missing")
        manifest = json.load(manifest_source)
        if manifest["frameworkIncluded"]:
            raise SystemExit("application bundle must not include framework middleware")
        if manifest["requiredProviderProfiles"] != ["inproc"]:
            raise SystemExit("required provider profile was not preserved")
        members = {member.name: member for member in archive.getmembers()}
        for entry in manifest["files"].values():
            source = archive.extractfile(members[entry["path"]])
            if source is None:
                raise SystemExit(f"missing {entry['path']}")
            if hashlib.sha256(source.read()).hexdigest() != entry["sha256"]:
                raise SystemExit(f"digest mismatch for {entry['path']}")


if __name__ == "__main__":
    main()
