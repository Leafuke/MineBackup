#!/usr/bin/env python3
"""Exercise the headless CLI profile lock and graceful signal contract."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time


CONFIG_ID = "11111111-1111-4111-8111-111111111111"
SPECIAL_CONFIG_ID = "22222222-2222-4222-8222-222222222222"
TASK_ID = "33333333-3333-4333-8333-333333333333"


def write_profile(profile: Path) -> None:
    world = profile / "server" / "world"
    (profile / "config").mkdir(parents=True)
    world.mkdir(parents=True)
    (world / "level.dat").write_bytes(b"minebackup-headless-process-contract")
    config = "\n".join(
        [
            "[Config1]",
            "ConfigName=Headless process contract",
            f"ConfigId={CONFIG_ID}",
            f"SavePath={profile / 'server'}",
            "WorldData=",
            "world",
            "Primary world",
            "*",
            f"BackupPath={profile / 'backups'}",
            "ZipProgram=",
            "ZipFormat=7z",
            "ZipLevel=1",
            "ZipMethod=LZMA2",
            "CpuThreads=1",
            "KeepCount=0",
            "SmartBackup=2",
            "UseLowPriority=0",
            "SkipIfUnchanged=1",
            "MaxSmartBackups=5",
            "CloudSyncEnabled=0",
            "[SpCfg2]",
            "Name=Recurring contract",
            f"SpecialConfigId={SPECIAL_CONFIG_ID}",
            "ZipLevel=1",
            "KeepCount=0",
            "CpuThreads=1",
            "UseLowPriority=0",
            "",
        ]
    )
    (profile / "config" / "config.ini").write_text(config, encoding="utf-8")
    tasks = {
        "schemaVersion": 1,
        "specialConfigs": [
            {
                "specialConfigId": SPECIAL_CONFIG_ID,
                "tasks": [
                    {
                        "taskId": TASK_ID,
                        "name": "wait before first backup",
                        "type": "backup",
                        "executionMode": "sequential",
                        "enabled": True,
                        "trigger": {"type": "interval", "intervalMinutes": 1},
                        "target": {"configId": CONFIG_ID, "worldPath": "world"},
                    }
                ],
            }
        ],
    }
    (profile / "config" / "special-tasks.json").write_text(
        json.dumps(tasks, ensure_ascii=False, indent=2), encoding="utf-8"
    )


def parse_single_json(stdout: str, context: str) -> dict:
    stripped = stdout.strip()
    if not stripped:
        raise AssertionError(f"{context}: stdout was empty")
    try:
        value = json.loads(stripped)
    except json.JSONDecodeError as error:
        raise AssertionError(f"{context}: stdout was not one JSON object: {stripped!r}") from error
    if not isinstance(value, dict) or value.get("schemaVersion") != 1:
        raise AssertionError(f"{context}: unexpected JSON envelope: {value!r}")
    return value


def send_first_signal(process: subprocess.Popen[str]) -> None:
    if os.name == "nt":
        process.send_signal(signal.CTRL_BREAK_EVENT)
    else:
        process.send_signal(signal.SIGTERM)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--work-root", type=Path)
    arguments = parser.parse_args()
    cli = arguments.cli.resolve()
    if not cli.is_file():
        raise FileNotFoundError(cli)

    owned_root = arguments.work_root is None
    root = (
        Path(tempfile.mkdtemp(prefix="minebackup-cli-process-"))
        if owned_root
        else arguments.work_root.resolve()
    )
    if not owned_root:
        shutil.rmtree(root, ignore_errors=True)
        root.mkdir(parents=True)
    profile = root / "profile"
    write_profile(profile)

    environment = os.environ.copy()
    environment.pop("DISPLAY", None)
    environment.pop("WAYLAND_DISPLAY", None)
    command = [
        str(cli),
        "--data-dir",
        str(profile),
        "--json",
        "--no-network",
        "run-special",
        SPECIAL_CONFIG_ID,
    ]
    creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    running = subprocess.Popen(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=environment,
        creationflags=creation_flags,
    )
    try:
        # Let the designated long-running process claim the profile before a
        # contender starts probing the same lock.
        time.sleep(0.2)
        busy = None
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if running.poll() is not None:
                stdout, stderr = running.communicate()
                raise AssertionError(
                    f"recurring CLI exited before cancellation ({running.returncode}): "
                    f"stderr={stderr!r} stdout={stdout!r}"
                )
            busy = subprocess.run(
                [
                    str(cli),
                    "--data-dir",
                    str(profile),
                    "--json",
                    "config",
                    "list",
                ],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                env=environment,
                timeout=10,
                check=False,
            )
            if busy.returncode == 3:
                break
            time.sleep(0.05)
        if busy is None or busy.returncode != 3:
            raise AssertionError("a second CLI did not observe the exclusive profile lock")
        busy_json = parse_single_json(busy.stdout, "profile lock")
        if busy_json.get("code") != "profile_busy":
            raise AssertionError(f"profile lock returned the wrong code: {busy_json!r}")

        # Lock acquisition happens before signal-handler installation. Give the
        # first process enough time to complete preflight and enter its wait.
        time.sleep(0.5)
        send_first_signal(running)
        stdout, stderr = running.communicate(timeout=15)
        if running.returncode != 9:
            raise AssertionError(
                f"first signal returned {running.returncode}, expected 9: "
                f"stderr={stderr!r} stdout={stdout!r}"
            )
        cancelled = parse_single_json(stdout, "signal cancellation")
        if cancelled.get("code") != "cancelled" or cancelled.get("ok") is not False:
            raise AssertionError(f"signal cancellation returned the wrong envelope: {cancelled!r}")

        forced = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=environment,
            creationflags=creation_flags,
        )
        try:
            time.sleep(0.2)
            forced_deadline = time.monotonic() + 10
            while time.monotonic() < forced_deadline:
                contender = subprocess.run(
                    [
                        str(cli),
                        "--data-dir",
                        str(profile),
                        "--json",
                        "config",
                        "list",
                    ],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    env=environment,
                    timeout=10,
                    check=False,
                )
                if contender.returncode == 3:
                    break
                if forced.poll() is not None:
                    raise AssertionError("second-signal CLI exited before acquiring its profile lock")
                time.sleep(0.05)
            else:
                raise AssertionError("second-signal CLI did not acquire its profile lock")
            time.sleep(0.5)
            forced_started = time.monotonic()
            send_first_signal(forced)
            try:
                send_first_signal(forced)
            except (ProcessLookupError, OSError):
                # Graceful cancellation may win the race, which still leaves
                # the process in the required exit-9 state.
                pass
            forced_stdout, forced_stderr = forced.communicate(timeout=5)
            if forced.returncode != 9 or time.monotonic() - forced_started >= 5:
                raise AssertionError(
                    f"second signal did not permit immediate exit 9 ({forced.returncode}): "
                    f"stderr={forced_stderr!r} stdout={forced_stdout!r}"
                )
        finally:
            if forced.poll() is None:
                forced.kill()
                forced.wait(timeout=5)
    finally:
        if running.poll() is None:
            running.kill()
            running.wait(timeout=5)
        if owned_root:
            shutil.rmtree(root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # Keep CI diagnostics compact and actionable.
        print(f"headless process contract failed: {error}", file=sys.stderr)
        raise
