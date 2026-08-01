# SPDX-License-Identifier: Apache-2.0

"""Black-box tests for hermetic validation and generation actions."""

from __future__ import annotations

import os
import json
from pathlib import Path
import subprocess
import sys

from tools.validate_deployment import (
    make_plan,
    read,
    resolve_deployment,
    validate_deployment as validate_deployment_model,
)


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
    deployment = compile_deployment(
        root,
        root / arguments[1],
        root / arguments[2],
    )
    errors = validate_deployment_model(
        read(root / "com/model/examples/radar.ovf-ir.json"),
        deployment,
        root / "com/deployment/profiles",
    )
    actual = 1 if errors else 0
    if actual != expected:
        raise SystemExit(
            f"expected deployment validation exit {expected}, received {actual}: "
            + "\n".join(errors)
        )


def compile_deployment(root: Path, source: Path, binding: Path) -> dict:
    cue_candidates = sorted(Path(os.environ["TEST_SRCDIR"]).glob("*cue_cli*/cue"))
    if len(cue_candidates) != 1:
        raise SystemExit(f"expected one hermetic CUE binary, found {cue_candidates}")
    environment = dict(os.environ)
    environment["CUE_CACHE_DIR"] = os.environ["TEST_TMPDIR"]
    environment["CUE_CONFIG_DIR"] = os.environ["TEST_TMPDIR"]
    completed = subprocess.run(
        [
            str(cue_candidates[0]),
            "export",
            str(root / "com/deployment/schema/deployment.cue"),
            str(source),
            str(binding),
            "--expression",
            "model",
            "--out",
            "json",
        ],
        check=False,
        capture_output=True,
        text=True,
        env=environment,
    )
    if completed.returncode:
        raise SystemExit(f"CUE deployment compilation failed:\n{completed.stderr}")
    return resolve_deployment(
        read(root / "com/model/examples/radar.ovf-ir.json"),
        json.loads(completed.stdout),
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
        source = root / arguments[1]
        binding = root / arguments[2]
        for output in (first, second):
            deployment = compile_deployment(root, source, binding)
            encoded = json.dumps(
                make_plan(deployment, root / "com/deployment/profiles"),
                sort_keys=True,
                indent=2,
            ) + "\n"
            output.write_text(encoded, encoding="utf-8")
        base = None
    else:
        raise SystemExit(f"unknown reproducibility mode: {arguments[0]}")
    if base is not None:
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
