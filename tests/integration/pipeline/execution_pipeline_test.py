# SPDX-License-Identifier: Apache-2.0

"""Boot the packaged example stack through dinit and ovf::exec."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import signal
import sqlite3
import subprocess
import tarfile
import time


ROOT = Path("/var/tmp/ovf-exec-pipeline")
OVERLAY_STORAGE = Path("/var/tmp/ovf-overlay")
LOWER = OVERLAY_STORAGE / "lower"
UPPER = OVERLAY_STORAGE / "upper"
WORK = OVERLAY_STORAGE / "work"
PERSISTENCE_PROVIDER_MOUNT = Path("/usr/lib/ovf/providers")
PERSISTENCE_STORAGE_MOUNT = Path("/var/lib/ovf/per")


def runfile(relative: str) -> Path:
    root = Path(os.environ["RUNFILES_DIR"])
    for workspace in dict.fromkeys(
        (os.environ.get("TEST_WORKSPACE", "_main"), "_main", "openvehicleframework")
    ):
        candidate = root / workspace / relative
        if candidate.exists():
            return candidate
    raise FileNotFoundError(relative)


def wait_for(
    path: Path, process: subprocess.Popen[str], console_path: Path, timeout: float = 20
) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        if process.poll() is not None:
            console = console_path.read_text(encoding="utf-8", errors="replace")
            raise RuntimeError(
                f"dinit exited with {process.returncode}; console output:\n{console}"
            )
        time.sleep(0.05)
    raise TimeoutError(f"timed out waiting for {path}")


def transition(domain: int, mode: int, logs: Path) -> None:
    completed = subprocess.run(
        [
            runfile(os.environ["OVF_TEST_MODE_CLIENT"]),
            ROOT / "run/coordinator.sock",
            str(domain),
            str(mode),
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=40,
    )
    (logs / f"transition-{domain}-{mode}.log").write_text(
        completed.stdout + completed.stderr, encoding="utf-8"
    )
    if completed.returncode != 0 or "TRANSITION_COMPLETE" not in completed.stdout:
        raise RuntimeError(f"transition {domain}.{mode} failed: {completed.stderr}")


def service_log(service: str) -> str:
    completed = subprocess.run(
        [
            runfile(os.environ["OVF_TEST_DINITCTL"]),
            "--socket-path",
            ROOT / "run/dinit.sock",
            "catlog",
            service,
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=10,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"cannot retrieve {service} log: {completed.stderr}")
    return completed.stdout


def mount_target_filesystem(archive_path: Path) -> None:
    subprocess.run(
        ["sudo", "-n", "umount", str(ROOT)],
        check=False,
        capture_output=True,
        text=True,
    )
    subprocess.run(
        ["sudo", "-n", "chmod", "-R", "a+rwx", str(OVERLAY_STORAGE)],
        check=False,
        capture_output=True,
        text=True,
    )
    for path in (ROOT, LOWER, UPPER, WORK):
        shutil.rmtree(path, ignore_errors=True)
        path.mkdir(parents=True)
    with tarfile.open(archive_path) as archive:
        archive.extractall(LOWER, filter="data")
    mounted = subprocess.run(
        [
            "sudo",
            "-n",
            "mount",
            "-t",
            "overlay",
            "overlay",
            "-o",
            f"lowerdir={LOWER},upperdir={UPPER},workdir={WORK}",
            ROOT,
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if mounted.returncode != 0:
        raise RuntimeError(f"cannot mount target OverlayFS: {mounted.stderr.strip()}")
    filesystem = subprocess.run(
        ["findmnt", "--noheadings", "--output", "FSTYPE", "--target", ROOT],
        check=False,
        capture_output=True,
        text=True,
    )
    if filesystem.returncode != 0 or filesystem.stdout.strip() != "overlay":
        subprocess.run(["sudo", "-n", "umount", ROOT], check=False)
        raise RuntimeError("target root is not mounted as OverlayFS")


def unmount_target_filesystem() -> None:
    unmounted = subprocess.run(
        ["sudo", "-n", "umount", ROOT], check=False, capture_output=True, text=True
    )
    if unmounted.returncode != 0:
        raise RuntimeError(f"cannot unmount target OverlayFS: {unmounted.stderr.strip()}")
    for path in (ROOT, LOWER, UPPER, WORK):
        shutil.rmtree(path, ignore_errors=True)


def mount_persistence_paths() -> None:
    for source, target in (
        (ROOT / "usr/lib/ovf/providers", PERSISTENCE_PROVIDER_MOUNT),
        (ROOT / "var/lib/ovf/per", PERSISTENCE_STORAGE_MOUNT),
    ):
        provision = subprocess.run(
            ["sudo", "-n", "install", "-d", "-m", "0750", str(target)],
            check=False,
            capture_output=True,
            text=True,
        )
        if provision.returncode != 0:
            raise RuntimeError(f"cannot provision {target}: {provision.stderr.strip()}")
        mounted = subprocess.run(
            ["sudo", "-n", "mount", "--bind", str(source), str(target)],
            check=False,
            capture_output=True,
            text=True,
        )
        if mounted.returncode != 0:
            raise RuntimeError(f"cannot bind {source} to {target}: {mounted.stderr.strip()}")
    ownership = subprocess.run(
        [
            "sudo",
            "-n",
            "chown",
            f"{os.getuid()}:{os.getgid()}",
            str(PERSISTENCE_STORAGE_MOUNT),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if ownership.returncode != 0:
        raise RuntimeError(f"cannot assign persistence storage: {ownership.stderr.strip()}")


def unmount_persistence_paths() -> None:
    for target in (PERSISTENCE_STORAGE_MOUNT, PERSISTENCE_PROVIDER_MOUNT):
        result = subprocess.run(
            ["sudo", "-n", "umount", target], check=False, capture_output=True, text=True
        )
        if result.returncode != 0:
            subprocess.run(
                ["sudo", "-n", "umount", "--lazy", target],
                check=False,
                capture_output=True,
                text=True,
            )


def main() -> int:
    outputs = Path(os.environ["TEST_UNDECLARED_OUTPUTS_DIR"])
    logs = outputs / "execution-pipeline"
    logs.mkdir(parents=True, exist_ok=True)
    mount_target_filesystem(runfile(os.environ["OVF_TEST_TARGET_FILESYSTEM"]))
    persistence_provider = (
        ROOT / "usr/lib/ovf/providers/libovf_per_provider_sqlite.so"
    )
    persistence_root = ROOT / "var/lib/ovf/per"
    if not persistence_provider.is_file() or not persistence_root.is_dir():
        raise RuntimeError("persistence platform artifacts are absent from target filesystem")
    persistence = subprocess.run(
        [
            runfile(os.environ["OVF_TEST_PER_CLIENT"]),
            persistence_provider.parent,
            persistence_root,
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=20,
        env={**os.environ, "LD_LIBRARY_PATH": str(ROOT / "usr/lib")},
    )
    (logs / "persistence.log").write_text(
        persistence.stdout + persistence.stderr, encoding="utf-8"
    )
    if (
        persistence.returncode != 0
        or "PERSISTENCE_INSTALLED_PROVIDER_VERIFIED" not in persistence.stdout
    ):
        raise RuntimeError(f"installed persistence provider failed: {persistence.stderr}")
    mount_persistence_paths()
    (ROOT / "run").mkdir(parents=True, exist_ok=True)
    (ROOT / "var/lib/ovf/exec").mkdir(parents=True, exist_ok=True)
    runtime_directory = subprocess.run(
        [
            "sudo",
            "-n",
            "install",
            "-d",
            "-m",
            "0775",
            "-o",
            str(os.getuid()),
            "-g",
            str(os.getgid()),
            "/run/dlt",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if runtime_directory.returncode != 0:
        raise RuntimeError(
            f"cannot provision the DLT runtime directory: {runtime_directory.stderr.strip()}"
        )

    environment = os.environ.copy()
    environment["LD_LIBRARY_PATH"] = str(ROOT / "usr/lib")
    environment["OVF_COM_PROVIDER_PATH"] = str(ROOT / "usr/lib/ovf/providers")
    environment["VSOMEIP_CONFIGURATION"] = str(ROOT / "etc/vsomeip.json")
    console_path = logs / "dinit-console.log"
    console = console_path.open("w", encoding="utf-8")
    dinit = subprocess.Popen(
        [
            ROOT / "usr/sbin/dinit",
            "--user",
            "--services-dir",
            ROOT / "etc/dinit.d",
            "--socket-path",
            ROOT / "run/dinit.sock",
        ],
        env=environment,
        stdout=console,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        wait_for(ROOT / "run/coordinator.sock", dinit, console_path)
        transition(1, 2, logs)
        transition(2, 2, logs)
        deadline = time.monotonic() + 20
        policy_log = ""
        while time.monotonic() < deadline:
            policy_log = service_log("ovf-app-4")
            if "ENVIRONMENT_MODEL_RECEIVED" in policy_log:
                break
            time.sleep(0.2)
        if "ENVIRONMENT_MODEL_RECEIVED" not in policy_log:
            raise RuntimeError("driving policy did not receive fused camera and radar data")
        (logs / "ovf-app-4-initial.log").write_text(policy_log, encoding="utf-8")
        if "POLICY_STATE_COMMITTED" not in policy_log:
            raise RuntimeError("driving policy did not commit its persistent Smithy record")
        transition(2, 1, logs)
        persisted_files = sorted(
            path.name for path in PERSISTENCE_STORAGE_MOUNT.iterdir() if path.is_file()
        )
        (logs / "persistence-files.log").write_text(
            "\n".join(persisted_files) + "\n", encoding="utf-8"
        )
        if not any(name.endswith(".db") for name in persisted_files):
            raise RuntimeError("driving policy did not create a persistent database")
        policy_database = PERSISTENCE_STORAGE_MOUNT / ("policy-state".encode().hex() + ".db")
        with sqlite3.connect(policy_database) as database:
            generation = database.execute(
                "SELECT generation FROM ovf_meta WHERE id=1"
            ).fetchone()[0]
            value_count = database.execute("SELECT count(*) FROM ovf_values").fetchone()[0]
        (logs / "persistence-state.log").write_text(
            f"generation={generation} values={value_count}\n", encoding="utf-8"
        )
        if generation == 0 or value_count == 0:
            raise RuntimeError("driving policy commit was not durable after shutdown")
        transition(2, 2, logs)
        deadline = time.monotonic() + 20
        restarted_generation = generation
        while time.monotonic() < deadline:
            with sqlite3.connect(policy_database) as database:
                restarted_generation = database.execute(
                    "SELECT generation FROM ovf_meta WHERE id=1"
                ).fetchone()[0]
            if restarted_generation > generation:
                break
            time.sleep(0.2)
        recovered_log = service_log("ovf-app-4")
        (logs / "ovf-app-4-restarted.log").write_text(recovered_log, encoding="utf-8")
        (logs / "persistence-restart.log").write_text(
            f"before={generation} after={restarted_generation}\n", encoding="utf-8"
        )
        if restarted_generation <= generation:
            raise RuntimeError("restarted driving policy did not update recovered state")
        transition(2, 1, logs)
        transition(1, 1, logs)
        for service in (
            "ovf-execd",
            "ovf-unit-11",
            "ovf-app-1",
            "ovf-app-2",
            "ovf-app-3",
            "ovf-app-4",
        ):
            (logs / f"{service}.log").write_text(service_log(service), encoding="utf-8")
        manifest = json.loads((ROOT / "usr/share/ovf/target/manifest.json").read_text())
        (logs / "target-manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        shutil.copyfile(ROOT / "var/lib/ovf/exec/journal.v1", logs / "journal.v1")
        return 0
    finally:
        if dinit.poll() is None:
            dinit.send_signal(signal.SIGTERM)
            try:
                dinit.wait(timeout=10)
            except subprocess.TimeoutExpired:
                dinit.kill()
                dinit.wait()
        console.close()
        unmount_persistence_paths()
        unmount_target_filesystem()


if __name__ == "__main__":
    raise SystemExit(main())
