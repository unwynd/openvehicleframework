# SPDX-License-Identifier: Apache-2.0

import os
from pathlib import Path
import signal
import selectors
import subprocess
import sys
import time


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


def main() -> int:
    service_path = runfile("examples/radar/radar_service")
    client_path = runfile("examples/radar/radar_client")
    config_path = runfile("com/transports/vsomeip/config/platform.json")
    routing_manager = next(
        path
        for path in Path(os.environ["RUNFILES_DIR"]).rglob("routingmanagerd")
        if path.is_file() and "vsomeip_test_runtime" in path.parts
    )
    provider = next(
        Path(os.environ["RUNFILES_DIR"]).rglob("libovf_com_provider_vsomeip.so")
    )
    environment = os.environ.copy()
    environment["OVF_COM_PROVIDER_PATH"] = str(provider.parent)
    environment["VSOMEIP_CONFIGURATION"] = str(config_path)
    native_library_path = routing_manager.parent.parent / "lib"
    existing_library_path = environment.get("LD_LIBRARY_PATH")
    environment["LD_LIBRARY_PATH"] = (
        f"{native_library_path}:{existing_library_path}"
        if existing_library_path
        else str(native_library_path)
    )

    routing = subprocess.Popen(
        [routing_manager],
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    routing_socket = Path("/tmp/vsomeip-0")
    routing_deadline = time.monotonic() + 5
    while time.monotonic() < routing_deadline:
        if routing_socket.exists():
            break
        if routing.poll() is not None:
            output = routing.stdout.read() if routing.stdout else ""
            raise RuntimeError(f"routingmanagerd exited before becoming ready:\n{output}")
        time.sleep(0.05)
    else:
        routing.send_signal(signal.SIGTERM)
        output = routing.communicate(timeout=5)[0]
        raise RuntimeError(f"routingmanagerd did not become ready:\n{output}")
    service = subprocess.Popen(
        [service_path],
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        deadline = time.monotonic() + 10
        service_output = []
        selector = selectors.DefaultSelector()
        selector.register(service.stdout, selectors.EVENT_READ)
        while time.monotonic() < deadline:
            ready = selector.select(timeout=min(0.25, deadline - time.monotonic()))
            if ready:
                line = service.stdout.readline()
                if line:
                    service_output.append(line)
                    if "SERVICE_READY" in line:
                        break
            if service.poll() is not None:
                break
        else:
            raise RuntimeError("service did not become ready")
        if not any("SERVICE_READY" in line for line in service_output):
            raise RuntimeError("service exited before becoming ready")

        client = subprocess.run(
            [client_path],
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=30,
        )
        expected = {
            "DISCOVERED",
            "METHOD_OK",
            "APPLICATION_ERROR_OK",
            "FIELD_READ_OK",
            "EVENT_OK",
            "FIELD_NOTIFICATION_OK",
        }
        observed = set(client.stdout.splitlines())
        if client.returncode != 0 or not expected.issubset(observed):
            sys.stderr.write(client.stdout)
            return 1
        return 0
    finally:
        if service.poll() is None:
            service.send_signal(signal.SIGTERM)
            try:
                service.wait(timeout=10)
            except subprocess.TimeoutExpired:
                service.kill()
                service.wait()
        if routing.poll() is None:
            routing.send_signal(signal.SIGTERM)
            try:
                routing.wait(timeout=10)
            except subprocess.TimeoutExpired:
                routing.kill()
                routing.wait()


if __name__ == "__main__":
    raise SystemExit(main())
