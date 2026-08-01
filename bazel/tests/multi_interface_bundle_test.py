# SPDX-License-Identifier: Apache-2.0

"""Verify packaging for an application with multiple interface roles."""

from __future__ import annotations

import json
import os
from pathlib import Path
import tarfile


def main() -> None:
    root = Path(os.environ["TEST_SRCDIR"]) / "_main"
    bundle = root / "examples/sensor_fusion/sensor_fusion_bundle.tar"
    with tarfile.open(bundle) as archive:
        source = archive.extractfile("share/ovf/sensor_fusion/manifest.json")
        if source is None:
            raise SystemExit("application manifest is missing")
        manifest = json.load(source)
        if manifest["requiredProviderProfiles"]:
            raise SystemExit("application bundle selected provider implementations")
        names = [interface["name"] for interface in manifest["interfaces"]]
        expected = ["camera", "environment_model", "radar"]
        if names != expected:
            raise SystemExit(f"unexpected interface inventory: {names}")
        if manifest["frameworkIncluded"]:
            raise SystemExit("application bundle must not include middleware")
        members = set(archive.getnames())
        for index in range(3):
            path = f"share/ovf/sensor_fusion/contract-{index}.ovf-ir.json"
            if path not in members:
                raise SystemExit(f"missing interface artifact: {path}")


if __name__ == "__main__":
    main()
