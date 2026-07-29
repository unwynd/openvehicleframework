# SPDX-License-Identifier: Apache-2.0

"""Validate generated supervisor descriptions with the pinned dinit parser."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess


def main() -> int:
    checker = Path(os.environ["OVF_TEST_DINIT_CHECK"])
    services = Path(os.environ["OVF_TEST_DINIT_SERVICES"])
    names = sorted(path.name for path in services.iterdir() if path.is_file())
    service_names = ["boot", "ovf-app-1", "ovf-app-2", "ovf-app-3", "ovf-app-4"]
    expected_names = service_names + [
        "ovf-app-1.env",
        "ovf-app-2.env",
        "ovf-app-3.env",
        "ovf-app-4.env",
    ]
    if names != sorted(expected_names):
        raise RuntimeError(f"unexpected generated services: {names}")
    for identifier in range(1, 5):
        environment = (services / f"ovf-app-{identifier}.env").read_text(encoding="utf-8")
        if environment != f"OVF_EXEC_APPLICATION_ID={identifier}\n":
            raise RuntimeError(f"invalid lifecycle identity for application {identifier}")
    completed = subprocess.run(
        [str(checker), "--user", "--services-dir", str(services), *service_names],
        check=False,
        capture_output=True,
        text=True,
    )
    expected_missing = {
        f"Service 'ovf-app-{identifier}': warning: could not stat command executable "
        f"'{executable}': No such file or directory"
        for identifier, executable in (
            (1, "/usr/bin/ovf-camera"),
            (2, "/usr/bin/ovf-radar"),
            (3, "/usr/bin/ovf-sensor-fusion"),
            (4, "/usr/bin/ovf-driving-policy"),
        )
    }
    diagnostics = {
        line.strip()
        for line in completed.stderr.splitlines()
        if line.strip() and line.strip() != "Secondary checks complete."
    }
    if completed.returncode != 0 and diagnostics != expected_missing:
        raise RuntimeError(
            f"dinit-check rejected generated services:\n"
            f"{completed.stdout}\n{completed.stderr}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
