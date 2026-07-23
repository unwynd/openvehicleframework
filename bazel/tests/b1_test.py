# SPDX-License-Identifier: Apache-2.0

"""Black-box tests for hermetic validation and generation actions."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys


def workspace() -> Path:
    return Path(os.environ["TEST_SRCDIR"]) / "_main"


def run(command: list[str], expected: int = 0) -> None:
    completed = subprocess.run(command, check=False)
    if completed.returncode != expected:
        raise SystemExit(
            f"expected exit {expected}, received {completed.returncode}: {command}"
        )


def validate_ir(root: Path, arguments: list[str]) -> None:
    expected = int(arguments[0])
    resolved = [
        argument if argument.startswith("--") else str(root / argument)
        for argument in arguments[1:]
    ]
    run([str(root / "tools/validate_ir"), *resolved], expected)


def validate_deployment(root: Path, arguments: list[str]) -> None:
    expected = int(arguments[0])
    deployment = arguments[1]
    run(
        [
            str(root / "tools/validate_deployment"),
            "--contract",
            str(root / "com/model/examples/radar.ovf-ir.json"),
            "--deployment",
            str(root / "com/deployment/examples" / deployment),
            "--profiles",
            str(root / "com/deployment/profiles"),
        ],
        expected,
    )


def reproducible(root: Path, arguments: list[str]) -> None:
    first = Path(os.environ["TEST_TMPDIR"]) / "first"
    second = Path(os.environ["TEST_TMPDIR"]) / "second"
    if arguments[0] == "generate":
        base = [
            str(root / "codegen/ovf_codegen"),
            str(root / "com/model/examples/radar.ovf-ir.json"),
            "--output",
        ]
    elif arguments[0] == "plan":
        base = [
            str(root / "tools/validate_deployment"),
            "--contract",
            str(root / "com/model/examples/radar.ovf-ir.json"),
            "--deployment",
            str(root / "com/deployment/examples" / arguments[1]),
            "--profiles",
            str(root / "com/deployment/profiles"),
            "--output-plan",
        ]
    else:
        raise SystemExit(f"unknown reproducibility mode: {arguments[0]}")
    run([*base, str(first)])
    run([*base, str(second)])
    if first.read_bytes() != second.read_bytes():
        raise SystemExit("repeated generation produced different bytes")


def main() -> None:
    root = workspace()
    mode, *arguments = sys.argv[1:]
    if mode == "ir":
        validate_ir(root, arguments)
    elif mode == "deployment":
        validate_deployment(root, arguments)
    elif mode == "reproducible":
        reproducible(root, arguments)
    elif mode == "compare":
        run([str(root / "tools/compare_ir"), *(str(root / value) for value in arguments)])
    else:
        raise SystemExit(f"unknown test mode: {mode}")


if __name__ == "__main__":
    main()
