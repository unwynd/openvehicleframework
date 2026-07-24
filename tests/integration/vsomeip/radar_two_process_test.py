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
    destination.mkdir(parents=True)
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
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        for key, _ in selector.select(timeout=0.25):
            line = key.fileobj.readline()
            if line:
                log.write(line)
                log.flush()
                if "SERVICE_READY" in line:
                    return process, log
        if process.poll() is not None:
            break
    remainder = process.stdout.read()
    log.write(remainder)
    log.close()
    raise RuntimeError(f"service did not become ready:\n{log_path.read_text()}")


def run_client(
    executable: Path,
    environment: dict[str, str],
    workdir: Path,
    log_path: Path,
) -> None:
    client = subprocess.run(
        [executable],
        cwd=workdir,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=30,
    )
    log_path.write_text(client.stdout, encoding="utf-8")
    observed = set(client.stdout.splitlines())
    if client.returncode != 0 or not EXPECTED.issubset(observed):
        raise RuntimeError(f"client exchange failed:\n{client.stdout}")


def main() -> int:
    outputs = Path(os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR", tempfile.gettempdir()))
    logs = outputs / "vsomeip-lifecycle"
    logs.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="ovf-vsomeip-bundles-") as temporary:
        root = Path(temporary)
        platform = root / "platform"
        service_root = root / "service"
        client_root = root / "client"
        extract(runfile("com/vsomeip_platform_bundle.tar"), platform)
        extract(runfile("examples/radar/radar_service_bundle.tar"), service_root)
        extract(runfile("examples/radar/radar_client_bundle.tar"), client_root)

        environment = os.environ.copy()
        environment.pop("VSOMEIP_CONFIGURATION", None)
        environment["LD_LIBRARY_PATH"] = str(platform / "usr/lib")
        environment["OVF_COM_PROVIDER_PATH"] = str(platform / "usr/lib/ovf/providers")
        workdir = platform / "etc"
        routing_log = (logs / "routingmanagerd.log").open("w", encoding="utf-8")
        routing: subprocess.Popen[str] | None = None
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
            routing_pid = routing.pid
            service_executable = service_root / "bin/radar_service"
            client_executable = client_root / "bin/radar_client"
            for generation in (1, 2):
                service, service_log = start_service(
                    service_executable,
                    environment,
                    workdir,
                    logs / f"service-{generation}.log",
                )
                run_client(
                    client_executable,
                    environment,
                    workdir,
                    logs / f"client-{generation}.log",
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
            routing_log.close()


if __name__ == "__main__":
    raise SystemExit(main())
