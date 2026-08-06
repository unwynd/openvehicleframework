# SPDX-License-Identifier: Apache-2.0
"""Run comparable OVF communication transport baselines."""

from __future__ import annotations

import json
import os
from pathlib import Path
import signal
import subprocess
import tarfile
import tempfile
import time


def runfile(relative: str) -> Path:
    runfiles_value = os.environ.get("RUNFILES_DIR")
    if runfiles_value:
        root = Path(runfiles_value)
    else:
        root = next(
            parent.parent
            for parent in Path(__file__).absolute().parents
            if parent.parent.name.endswith(".runfiles")
        )
    for workspace in dict.fromkeys(
        (os.environ.get("TEST_WORKSPACE", "_main"), "_main", "openvehicleframework")
    ):
        candidate = root / workspace / relative
        if candidate.exists():
            return candidate
    raise FileNotFoundError(relative)


def extract(bundle: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(bundle) as archive:
        archive.extractall(destination, filter="data")


def stop(process: subprocess.Popen[str] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def execute(binary: Path, environment: dict[str, str], cwd: Path | None = None) -> dict:
    completed = subprocess.run(
        [binary], cwd=cwd, env=environment, capture_output=True, text=True, timeout=180
    )
    if completed.returncode:
        raise RuntimeError(
            f"{binary.name} failed with {completed.returncode}:\n{completed.stdout}\n{completed.stderr}"
        )
    lines = [line for line in completed.stdout.splitlines() if line.startswith("{")]
    if len(lines) != 1:
        raise RuntimeError(f"{binary.name} produced no unique JSON result:\n{completed.stdout}")
    return json.loads(lines[0])


def main() -> int:
    environment = os.environ.copy()
    results = [execute(runfile("benchmarks/com/iceoryx2_benchmark"), environment)]
    configured_scratch = Path(environment.get("TEST_TMPDIR", Path.cwd()))
    scratch_root = configured_scratch if configured_scratch.is_dir() else Path.cwd()
    with tempfile.TemporaryDirectory(prefix="ovf-com-benchmark-", dir=scratch_root) as temporary:
        platform = Path(temporary)
        extract(runfile("com/vsomeip_platform_bundle.tar"), platform)
        vsomeip_environment = environment.copy()
        vsomeip_environment.pop("VSOMEIP_CONFIGURATION", None)
        vsomeip_environment["LD_LIBRARY_PATH"] = str(platform / "usr/lib")
        routing = subprocess.Popen(
            [platform / "usr/bin/routingmanagerd"],
            cwd=platform / "etc",
            env=vsomeip_environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            time.sleep(0.5)
            if routing.poll() is not None:
                raise RuntimeError(f"routingmanagerd exited:\n{routing.stdout.read()}")
            results.append(
                execute(
                    runfile("benchmarks/com/vsomeip_benchmark"),
                    vsomeip_environment,
                    platform / "etc",
                )
            )
        finally:
            stop(routing)
    print(json.dumps({"schemaVersion": 1, "results": results}, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
