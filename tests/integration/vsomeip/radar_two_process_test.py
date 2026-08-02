# SPDX-License-Identifier: Apache-2.0

"""Validate independent vSomeIP platform and application bundle lifecycles."""

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


EXPECTED = {
    "DISCOVERED",
    "METHOD_OK",
    "APPLICATION_ERROR_OK",
    "FIELD_READ_OK",
    "EVENT_OK",
    "FIELD_NOTIFICATION_OK",
}


def runfile(relative: str) -> Path:
    root = Path(os.environ["RUNFILES_DIR"])
    workspaces = (
        os.environ.get("TEST_WORKSPACE", "_main"),
        "_main",
        "openvehicleframework",
    )
    for workspace in dict.fromkeys(workspaces):
        candidate = root / workspace / relative
        if candidate.exists():
            return candidate
    raise FileNotFoundError(f"runfile not found: {relative}")


def extract(bundle: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(bundle) as archive:
        for member in archive.getmembers():
            target = (destination / member.name).resolve()
            if destination.resolve() not in target.parents:
                raise RuntimeError(f"unsafe bundle member: {member.name}")
        archive.extractall(destination, filter="data")


def stop(process: subprocess.Popen[str] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def start_service(
    executable: Path,
    environment: dict[str, str],
    workdir: Path,
    log_path: Path,
    deployment: Path,
) -> tuple[subprocess.Popen[str], object]:
    log = log_path.open("w", encoding="utf-8")
    ready_read, ready_write = os.pipe()
    managed_environment = environment.copy()
    managed_environment["OVF_EXEC_APPLICATION_ID"] = "1"
    managed_environment["OVF_EXEC_READY_FD"] = str(ready_write)
    managed_environment["DINIT_SERVICE"] = "ovf-radar-service"
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
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        for key, _ in selector.select(timeout=0.25):
            line = key.fileobj.readline()
            if line:
                log.write(line)
                log.flush()
                if "SERVICE_READY" in line:
                    if os.read(ready_read, 1) != b"\x01":
                        raise RuntimeError("service sent an invalid readiness notification")
                    os.close(ready_read)
                    return process, log
        if process.poll() is not None:
            break
    remainder = process.stdout.read()
    os.close(ready_read)
    log.write(remainder)
    log.close()
    raise RuntimeError(f"service did not become ready:\n{log_path.read_text()}")


def run_client(
    executable: Path,
    environment: dict[str, str],
    workdir: Path,
    log_path: Path,
    deployment: Path,
) -> None:
    ready_read, ready_write = os.pipe()
    managed_environment = environment.copy()
    managed_environment["OVF_EXEC_APPLICATION_ID"] = "2"
    managed_environment["OVF_EXEC_READY_FD"] = str(ready_write)
    managed_environment["DINIT_SERVICE"] = "ovf-radar-client"
    managed_environment["OVF_COM_DEPLOYMENT"] = str(deployment)
    client = subprocess.Popen(
        [executable],
        cwd=workdir,
        env=managed_environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        pass_fds=(ready_write,),
    )
    os.close(ready_write)
    assert client.stdout is not None
    output: list[str] = []
    observed: set[str] = set()
    selector = selectors.DefaultSelector()
    selector.register(client.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline and client.poll() is None:
        for key, _ in selector.select(timeout=0.25):
            line = key.fileobj.readline()
            if line:
                output.append(line)
                observed.add(line.rstrip())
                if EXPECTED.issubset(observed):
                    break
        if EXPECTED.issubset(observed):
            break
    readiness = os.read(ready_read, 1)
    os.close(ready_read)
    stop(client)
    remainder = client.stdout.read()
    if remainder:
        output.append(remainder)
        observed.update(remainder.splitlines())
    content = "".join(output)
    log_path.write_text(content, encoding="utf-8")
    if readiness != b"\x01" or client.returncode != 0 or not EXPECTED.issubset(observed):
        raise RuntimeError(f"client exchange failed:\n{content}")


def main() -> int:
    outputs_value = os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR")
    if not outputs_value:
        raise RuntimeError("TEST_UNDECLARED_OUTPUTS_DIR is required")
    outputs = Path(outputs_value)
    logs = outputs / "vsomeip-lifecycle"
    logs.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="ovf-vsomeip-bundles-", dir=outputs) as temporary:
        root = Path(temporary)
        platform = root / "platform"
        service_root = root / "service"
        client_root = root / "client"
        extract(runfile("com/vsomeip_platform_bundle.tar"), platform)
        extract(runfile("log/dlt_platform_bundle.tar"), platform)
        extract(runfile("examples/radar/radar_service_bundle.tar"), service_root)
        extract(runfile("examples/radar/radar_client_bundle.tar"), client_root)

        environment = os.environ.copy()
        environment.pop("VSOMEIP_CONFIGURATION", None)
        environment["LD_LIBRARY_PATH"] = str(platform / "usr/lib")
        environment["OVF_COM_PROVIDER_PATH"] = str(platform / "usr/lib/ovf/providers")
        dlt_runtime = root / "run/dlt"
        dlt_runtime.mkdir(parents=True)
        environment["DLT_PIPE_DIR"] = str(dlt_runtime)
        workdir = platform / "etc"
        deployments = runfile(
            "tests/integration/vsomeip/communication_deployment.runtime"
        )
        routing_log = (logs / "routingmanagerd.log").open("w", encoding="utf-8")
        routing: subprocess.Popen[str] | None = None
        dlt: subprocess.Popen[str] | None = None
        service: subprocess.Popen[str] | None = None
        service_log = None
        try:
            routing = subprocess.Popen(
                [platform / "usr/bin/routingmanagerd"],
                cwd=workdir,
                env=environment,
                stdout=routing_log,
                stderr=subprocess.STDOUT,
                text=True,
            )
            dlt_log = (logs / "dlt-daemon.log").open("w", encoding="utf-8")
            dlt = subprocess.Popen(
                [platform / "usr/bin/dlt-daemon", "-t", dlt_runtime],
                cwd=workdir,
                env=environment,
                stdout=dlt_log,
                stderr=subprocess.STDOUT,
                text=True,
            )
            routing_pid = routing.pid
            service_executable = service_root / "opt/radar_service/bin/radar_service"
            client_executable = client_root / "opt/radar_client/bin/radar_client"
            for generation in (1, 2):
                service, service_log = start_service(
                    service_executable,
                    environment,
                    workdir,
                    logs / f"service-{generation}.log",
                    deployments / "radar_service.json",
                )
                run_client(
                    client_executable,
                    environment,
                    workdir,
                    logs / f"client-{generation}.log",
                    deployments / "radar_client.json",
                )
                stop(service)
                service = None
                if service_log:
                    service_log.close()
                    service_log = None
                if routing.poll() is not None or routing.pid != routing_pid:
                    raise RuntimeError("middleware did not survive application replacement")
            return 0
        except Exception:
            sys.stderr.write(routing_log.name + "\n")
            raise
        finally:
            stop(service)
            if service_log:
                service_log.close()
            stop(routing)
            stop(dlt)
            routing_log.close()
            if dlt is not None:
                dlt_log.close()


if __name__ == "__main__":
    raise SystemExit(main())
