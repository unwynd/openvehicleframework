#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Exercise durable persistence and crash rollback across real processes."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys


def run(binary: str, root: Path, mode: str, expected: int = 0) -> str:
    completed = subprocess.run(
        [binary, str(root), mode], capture_output=True, text=True, check=False
    )
    if completed.returncode != expected:
        raise RuntimeError(
            f"{mode}: expected {expected}, received {completed.returncode}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return completed.stdout


def main() -> int:
    if len(sys.argv) != 2:
        raise RuntimeError("expected the e2e application path")
    output_root = Path(os.environ["TEST_UNDECLARED_OUTPUTS_DIR"])
    database_root = output_root / "database"
    binary = sys.argv[1]
    transcript = []
    transcript.append(run(binary, database_root, "initialize"))
    run(binary, database_root, "crash", 23)
    transcript.append(run(binary, database_root, "verify-old"))
    transcript.append(run(binary, database_root, "update"))
    transcript.append(run(binary, database_root, "verify-new"))
    (output_root / "per-e2e.log").write_text("".join(transcript), encoding="utf-8")
    print("durable restart and interrupted replacement verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
