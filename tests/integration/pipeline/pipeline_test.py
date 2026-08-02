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


def executable_runfile(name: str) -> Path:
    root = Path(os.environ["RUNFILES_DIR"])
    matches = [
        candidate
        for candidate in root.rglob(name)
        if candidate.is_file() and os.access(candidate, os.X_OK)
    ]
    if len(matches) != 1:
        raise RuntimeError(f"expected exactly one executable runfile named {name}: {matches}")
    return matches[0]


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
    application_id: int,
    service_name: str,
    deployment: Path,
) -> tuple[subprocess.Popen[str], object]:
    log = log_path.open("w", encoding="utf-8")
    ready_read, ready_write = os.pipe()
    managed_environment = environment.copy()
    managed_environment["OVF_EXEC_APPLICATION_ID"] = str(application_id)
    managed_environment["OVF_EXEC_READY_FD"] = str(ready_write)
    managed_environment["DINIT_SERVICE"] = service_name
    managed_environment["OVF_COM_DEPLOYMENT"] = str(deployment)
    process = subprocess.Popen(
        [executable],
        cwd=workdir,
        env=managed_environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        pass_fds=(ready_write,),
    )
    os.close(ready_write)
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
                if os.read(ready_read, 1) != b"\x01":
                    raise RuntimeError(f"{executable.name} sent an invalid readiness notification")
                os.close(ready_read)
                return process, log
    os.close(ready_read)
    remainder = process.stdout.read()
    log.write(remainder)
    log.close()
    raise RuntimeError(f"{executable.name} did not report {marker}:\n"
                       f"{log_path.read_text()}")


def main() -> int:
    outputs_value = os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR")
    if not outputs_value:
        raise RuntimeError("TEST_UNDECLARED_OUTPUTS_DIR is required")
    outputs = Path(outputs_value)
    logs = outputs / "sensor-fusion-pipeline"
    logs.mkdir(parents=True, exist_ok=True)
    processes: list[subprocess.Popen[str]] = []
    open_logs: list[object] = []
    with tempfile.TemporaryDirectory(
        prefix="ovf-sensor-fusion-", dir=outputs
    ) as temporary:
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
        dlt_ipc = root / "run" / "dlt"
        dlt_ipc.mkdir(parents=True)
        environment["DLT_PIPE_DIR"] = str(dlt_ipc)
        deployments = runfile(
            "tests/integration/pipeline/communication_deployment.runtime"
        )
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
        dlt_log = (logs / "dlt-daemon.log").open("w", encoding="utf-8")
        open_logs.append(dlt_log)
        dlt_daemon = subprocess.Popen(
            [executable_runfile("dlt-daemon"), "-t", dlt_ipc],
            cwd=workdir,
            env=environment,
            stdout=dlt_log,
            stderr=subprocess.STDOUT,
            text=True,
        )
        processes.append(dlt_daemon)
        try:
            for executable, marker, name, application_id, deployment_name in (
                (runfile("examples/radar/radar_iceoryx2_service"),
                 "SERVICE_READY", "radar", 2, "radar"),
                (runfile("examples/camera/camera"), "CAMERA_READY", "camera", 1, "camera"),
                (runfile("examples/sensor_fusion/sensor_fusion"),
                 "SENSOR_FUSION_READY", "sensor-fusion", 3, "sensor_fusion"),
            ):
                process, log = start_until(
                    executable,
                    marker,
                    environment,
                    workdir,
                    logs / f"{name}.log",
                    application_id,
                    f"ovf-app-{application_id}",
                    deployments / f"{deployment_name}.json",
                )
                processes.append(process)
                open_logs.append(log)
            policy, policy_log = start_until(
                runfile("examples/driving_policy/driving_policy"),
                "ENVIRONMENT_MODEL_RECEIVED",
                environment,
                workdir,
                logs / "driving-policy.log",
                4,
                "ovf-app-4",
                deployments / "driving_policy.json",
            )
            processes.append(policy)
            open_logs.append(policy_log)
            return 0
        finally:
            for process in reversed(processes):
                stop(process)
            for log in open_logs:
                log.close()


if __name__ == "__main__":
    raise SystemExit(main())
