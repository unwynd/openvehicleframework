# SPDX-License-Identifier: Apache-2.0

"""Black-box verification of the deployable vSomeIP platform bundle."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import tarfile


def main() -> None:
    bundle = (
        Path(os.environ["TEST_SRCDIR"])
        / os.environ["TEST_WORKSPACE"]
        / "com/vsomeip_platform_bundle.tar"
    )
    expected = [
        "etc/vsomeip.json",
        "usr/bin/routingmanagerd",
        "usr/lib/libovf_com.so",
        "usr/lib/libvsomeip3-cfg.so",
        "usr/lib/libvsomeip3-cfg.so.3",
        "usr/lib/libvsomeip3-e2e.so",
        "usr/lib/libvsomeip3-e2e.so.3",
        "usr/lib/libvsomeip3-sd.so",
        "usr/lib/libvsomeip3-sd.so.3",
        "usr/lib/libvsomeip3.so",
        "usr/lib/libvsomeip3.so.3",
        "usr/lib/ovf/providers/libovf_com_provider_vsomeip.so",
        "usr/lib/systemd/system/ovf-vsomeip-routing.service",
        "usr/share/licenses/boost/LICENSE_1_0.txt",
        "usr/share/licenses/openvehicleframework/LICENSE",
        "usr/share/licenses/vsomeip/LICENSE",
        "usr/share/ovf/platform/vsomeip/manifest.json",
    ]
    with tarfile.open(bundle) as archive:
        if archive.getnames() != expected:
            raise SystemExit(f"unexpected platform bundle: {archive.getnames()}")
        for member in archive.getmembers():
            if member.mtime != 0 or member.uid != 0 or member.gid != 0:
                raise SystemExit(f"non-reproducible tar metadata: {member.name}")
        source = archive.extractfile("usr/share/ovf/platform/vsomeip/manifest.json")
        if source is None:
            raise SystemExit("platform manifest is missing")
        manifest = json.load(source)
        if manifest["applicationsIncluded"] or manifest["transport"] != "vsomeip":
            raise SystemExit("invalid platform shipping boundary")
        members = {member.name: member for member in archive.getmembers()}
        for path, entry in manifest["files"].items():
            source = archive.extractfile(members[path])
            if source is None:
                raise SystemExit(f"missing {path}")
            if hashlib.sha256(source.read()).hexdigest() != entry["sha256"]:
                raise SystemExit(f"digest mismatch for {path}")


if __name__ == "__main__":
    main()
