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
        service = subprocess.Popen(
            [service_binary],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
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
            raise RuntimeError("service did not become ready")
        client = subprocess.run(
            [client_binary],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=30,
        )
        client_log_path.write_text(client.stdout, encoding="utf-8")
        observed = set(client.stdout.splitlines())
        if client.returncode != 0 or not EXPECTED.issubset(observed):
            raise RuntimeError(f"client exchange failed:\n{client.stdout}")
        return 0
    finally:
        stop(service)
        if service and service.stdout:
            service_log.write(service.stdout.read())
        service_log.close()


if __name__ == "__main__":
    raise SystemExit(main())
