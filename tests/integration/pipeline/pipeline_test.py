# SPDX-License-Identifier: Apache-2.0

"""Exercise camera and radar fusion through to driving policy."""

from __future__ import annotations

import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import tarfile
import tempfile
import time


def runfile(relative: str) -> Path:
    root = Path(os.environ["RUNFILES_DIR"])
    for workspace in dict.fromkeys(
        (os.environ.get("TEST_WORKSPACE", "_main"), "_main", "openvehicleframework")
    ):
        candidate = root / workspace / relative
        if candidate.exists():
            return candidate
    raise FileNotFoundError(relative)


def stop(process: subprocess.Popen[str] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def start_until(
    executable: Path,
    marker: str,
    environment: dict[str, str],
    workdir: Path,
    log_path: Path,
) -> tuple[subprocess.Popen[str], object]:
    log = log_path.open("w", encoding="utf-8")
    process = subprocess.Popen(
        [executable],
        cwd=workdir,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    assert process.stdout is not None
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline and process.poll() is None:
        for key, _ in selector.select(timeout=0.25):
            line = key.fileobj.readline()
            log.write(line)
            log.flush()
            if marker in line:
                return process, log
    remainder = process.stdout.read()
    log.write(remainder)
    log.close()
    raise RuntimeError(f"{executable.name} did not report {marker}:\n"
                       f"{log_path.read_text()}")


def main() -> int:
    outputs = Path(os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR", tempfile.gettempdir()))
    logs = outputs / "sensor-fusion-pipeline"
    logs.mkdir(parents=True, exist_ok=True)
    processes: list[subprocess.Popen[str]] = []
    open_logs: list[object] = []
    with tempfile.TemporaryDirectory(prefix="ovf-sensor-fusion-") as temporary:
        root = Path(temporary)
        platform = root / "platform"
        providers = root / "providers"
        platform.mkdir()
        providers.mkdir()
        with tarfile.open(runfile("com/vsomeip_platform_bundle.tar")) as archive:
            archive.extractall(platform, filter="data")
        for plugin in (
            runfile("com/transports/iceoryx2/libovf_com_provider_iceoryx2.so"),
            platform / "usr/lib/ovf/providers/libovf_com_provider_vsomeip.so",
        ):
            (providers / plugin.name).symlink_to(plugin)

        environment = os.environ.copy()
        environment.pop("VSOMEIP_CONFIGURATION", None)
        environment["LD_LIBRARY_PATH"] = str(platform / "usr/lib")
        environment["OVF_COM_PROVIDER_PATH"] = str(providers)
        workdir = platform / "etc"
        routing_log = (logs / "routingmanagerd.log").open("w", encoding="utf-8")
        open_logs.append(routing_log)
        routing = subprocess.Popen(
            [platform / "usr/bin/routingmanagerd"],
            cwd=workdir,
            env=environment,
            stdout=routing_log,
            stderr=subprocess.STDOUT,
            text=True,
        )
        processes.append(routing)
        try:
            for executable, marker, name in (
                (runfile("examples/radar/radar_iceoryx2_service"),
                 "SERVICE_READY", "radar"),
                (runfile("examples/camera/camera"), "CAMERA_READY", "camera"),
                (runfile("examples/sensor_fusion/sensor_fusion"),
                 "SENSOR_FUSION_READY", "sensor-fusion"),
            ):
                process, log = start_until(
                    executable, marker, environment, workdir, logs / f"{name}.log"
                )
                processes.append(process)
                open_logs.append(log)
            policy = subprocess.run(
                [runfile("examples/driving_policy/driving_policy")],
                cwd=workdir,
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=30,
            )
            (logs / "driving-policy.log").write_text(policy.stdout, encoding="utf-8")
            if policy.returncode != 0 or "ENVIRONMENT_MODEL_RECEIVED" not in policy.stdout:
                raise RuntimeError(f"driving policy received no fused data:\n{policy.stdout}")
            return 0
        finally:
            for process in reversed(processes):
                stop(process)
            for log in open_logs:
                log.close()


if __name__ == "__main__":
    raise SystemExit(main())
