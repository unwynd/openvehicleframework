#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prepare the Linux validation lab, run a command, and retain diagnostics."""

from __future__ import annotations

from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time


WORKSPACE = Path(os.environ.get("OVF_LAB_WORKSPACE", "/workspace"))
CACHE = Path(os.environ.get("OVF_LAB_CACHE", "/var/cache/ovf/bazel"))
LOGS = Path(os.environ.get("OVF_LAB_LOGS", "/var/log/ovf"))
SOURCE = Path("/opt/ovf/source")
AUTHORIZED_KEYS = Path("/run/ovf/authorized_keys")


def prepare_workspace() -> None:
    WORKSPACE.mkdir(parents=True, exist_ok=True)
    populated = False
    if not (WORKSPACE / "MODULE.bazel").exists():
        shutil.copytree(SOURCE, WORKSPACE, dirs_exist_ok=True)
        populated = True
    if populated:
        subprocess.run(["chown", "-R", "ovf:ovf", str(WORKSPACE)], check=True)
    for path in (CACHE, LOGS):
        path.mkdir(parents=True, exist_ok=True)
        subprocess.run(["chown", "-R", "ovf:ovf", str(path)], check=True)


def start_ssh() -> bool:
    if not AUTHORIZED_KEYS.is_file():
        return False
    ssh = Path("/home/ovf/.ssh")
    ssh.mkdir(mode=0o700, parents=True, exist_ok=True)
    shutil.copyfile(AUTHORIZED_KEYS, ssh / "authorized_keys")
    os.chmod(ssh / "authorized_keys", 0o600)
    subprocess.run(["chown", "-R", "ovf:ovf", str(ssh)], check=True)
    subprocess.run(["ssh-keygen", "-A"], check=True)
    subprocess.run(["/usr/sbin/sshd"], check=True)
    return True


def metadata(command: list[str], ssh_enabled: bool) -> None:
    values = {
        "startedAt": datetime.now(timezone.utc).isoformat(),
        "architecture": os.uname().machine,
        "kernel": os.uname().release,
        "command": command,
        "workspace": str(WORKSPACE),
        "cache": str(CACHE),
        "sshEnabled": ssh_enabled,
    }
    (LOGS / "environment.json").write_text(
        json.dumps(values, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def run(command: list[str]) -> int:
    log_path = LOGS / "console.log"
    environment = os.environ.copy()
    environment["TEST_TMPDIR"] = "/tmp/ovf-tests"
    if command and command[0] == "bazel":
        command = [
            *command[:1],
            f"--output_user_root={CACHE}",
            *command[1:],
            "--test_output=errors",
            "--build_event_json_file=/var/log/ovf/build-events.json",
        ]
    with log_path.open("a", encoding="utf-8", buffering=1) as log:
        process = subprocess.Popen(
            ["runuser", "--user", "ovf", "--", *command],
            cwd=WORKSPACE,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        assert process.stdout is not None
        for line in process.stdout:
            sys.stdout.write(line)
            log.write(line)
        return process.wait()


def collect_testlogs() -> None:
    matches = list(CACHE.glob("*/execroot/_main/bazel-out/*/testlogs"))
    if matches:
        destination = LOGS / "testlogs"
        shutil.copytree(matches[-1], destination, dirs_exist_ok=True)


def hold() -> None:
    print("OVF lab is holding for debugging; use docker exec or SSH as user ovf.")
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    while True:
        time.sleep(60)


def main() -> int:
    command = sys.argv[1:] or ["bazel", "test", "--config=strict", "//:all_tests"]
    prepare_workspace()
    ssh_enabled = start_ssh()
    metadata(command, ssh_enabled)
    result = run(command)
    collect_testlogs()
    if os.environ.get("OVF_LAB_HOLD") == "1":
        hold()
    return result


if __name__ == "__main__":
    raise SystemExit(main())
