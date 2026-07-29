# SPDX-License-Identifier: Apache-2.0

"""Exercise the transport-neutral radar applications over iceoryx2 IPC."""

from __future__ import annotations

import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
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
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def main() -> int:
    plugin = runfile(
        "com/transports/iceoryx2/libovf_com_provider_iceoryx2"
        + (".dylib" if sys.platform == "darwin" else ".so")
    )
    service_binary = runfile("examples/radar/radar_iceoryx2_service")
    client_binary = runfile("examples/radar/radar_iceoryx2_client")
    environment = os.environ.copy()
    environment["OVF_COM_PROVIDER_PATH"] = str(plugin.parent)
    output = Path(os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR", tempfile.gettempdir()))
    service_log_path = output / "iceoryx2-radar-service.log"
    client_log_path = output / "iceoryx2-radar-client.log"
    service: subprocess.Popen[str] | None = None
    service_log = service_log_path.open("w", encoding="utf-8")
    try:
        service_ready_read, service_ready_write = os.pipe()
        service_environment = environment.copy()
        service_environment["OVF_EXEC_APPLICATION_ID"] = "1"
        service_environment["OVF_EXEC_READY_FD"] = str(service_ready_write)
        service_environment["DINIT_SERVICE"] = "ovf-radar-service"
        service = subprocess.Popen(
            [service_binary],
            env=service_environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            pass_fds=(service_ready_write,),
        )
        os.close(service_ready_write)
        assert service.stdout is not None
        selector = selectors.DefaultSelector()
        selector.register(service.stdout, selectors.EVENT_READ)
        deadline = time.monotonic() + 10
        ready = False
        while time.monotonic() < deadline and service.poll() is None:
            for key, _ in selector.select(timeout=0.25):
                line = key.fileobj.readline()
                service_log.write(line)
                service_log.flush()
                if "SERVICE_READY" in line:
                    ready = True
                    break
            if ready:
                break
        if not ready:
            os.close(service_ready_read)
            raise RuntimeError("service did not become ready")
        if os.read(service_ready_read, 1) != b"\x01":
            os.close(service_ready_read)
            raise RuntimeError("service sent an invalid readiness notification")
        os.close(service_ready_read)

        client_ready_read, client_ready_write = os.pipe()
        client_environment = environment.copy()
        client_environment["OVF_EXEC_APPLICATION_ID"] = "2"
        client_environment["OVF_EXEC_READY_FD"] = str(client_ready_write)
        client_environment["DINIT_SERVICE"] = "ovf-radar-client"
        client = subprocess.Popen(
            [client_binary],
            env=client_environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            pass_fds=(client_ready_write,),
        )
        os.close(client_ready_write)
        assert client.stdout is not None
        selector = selectors.DefaultSelector()
        selector.register(client.stdout, selectors.EVENT_READ)
        lines: list[str] = []
        observed: set[str] = set()
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and client.poll() is None:
            for key, _ in selector.select(timeout=0.25):
                line = key.fileobj.readline()
                if line:
                    lines.append(line)
                    observed.add(line.rstrip())
            if EXPECTED.issubset(observed):
                break
        client_ready = os.read(client_ready_read, 1)
        os.close(client_ready_read)
        stop(client)
        remainder = client.stdout.read()
        if remainder:
            lines.append(remainder)
            observed.update(remainder.splitlines())
        content = "".join(lines)
        client_log_path.write_text(content, encoding="utf-8")
        if client_ready != b"\x01" or client.returncode != 0 or not EXPECTED.issubset(observed):
            raise RuntimeError(f"client exchange failed:\n{content}")
        return 0
    finally:
        stop(service)
        if service and service.stdout:
            service_log.write(service.stdout.read())
        service_log.close()


if __name__ == "__main__":
    raise SystemExit(main())
