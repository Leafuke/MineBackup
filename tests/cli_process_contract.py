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
JOB_ID = "22222222-2222-4222-8222-222222222222"
STAGE_ID = "33333333-3333-4333-8333-333333333333"
STEP_ID = "44444444-4444-4444-8444-444444444444"
INVALID_JOB_ID = "25555555-5555-4555-8555-555555555555"
INVALID_STAGE_ID = "36666666-6666-4666-8666-666666666666"
INVALID_STEP_ID = "47777777-7777-4777-8777-777777777777"


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
            "",
        ]
    )
    (profile / "config" / "config.ini").write_text(config, encoding="utf-8")
    jobs = {
        "schemaVersion": 1,
        "jobs": [
            {
                "jobId": JOB_ID,
                "name": "cancellation contract",
                "stages": [
                    {
                        "stageId": STAGE_ID,
                        "name": "wait",
                        "steps": [
                            {
                                "stepId": STEP_ID,
                                "name": "long process",
                                "type": "process",
                                "executable": sys.executable,
                                "arguments": ["-c", "import time; time.sleep(60)"],
                                "workingDirectory": "",
                                "timeoutSeconds": 0,
                                "maximumCapturedBytes": 4096,
                                "lowPriority": False,
                            }
                        ],
                    }
                ],
            },
            {
                "jobId": INVALID_JOB_ID,
                "name": "invalid stderr contract",
                "stages": [
                    {
                        "stageId": INVALID_STAGE_ID,
                        "name": "invalid stderr",
                        "steps": [
                            {
                                "stepId": INVALID_STEP_ID,
                                "name": "invalid stderr",
                                "type": "process",
                                "executable": sys.executable,
                                "arguments": [
                                    "-c",
                                    "import sys; sys.stderr.buffer.write(b'\\xff' + b'x' * (2 * 1024 * 1024)); sys.exit(7)",
                                ],
                                "workingDirectory": "",
                                "timeoutSeconds": 10,
                                "maximumCapturedBytes": 64 * 1024 * 1024,
                                "lowPriority": False,
                            }
                        ],
                    }
                ],
            },
        ],
    }
    (profile / "config" / "jobs.json").write_text(
        json.dumps(jobs, ensure_ascii=False, indent=2), encoding="utf-8"
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
        "job",
        "run",
        "--job",
        JOB_ID,
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

        invalid = subprocess.run(
            [
                str(cli),
                "--data-dir",
                str(profile),
                "--json",
                "--no-network",
                "job",
                "run",
                "--job",
                INVALID_JOB_ID,
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=environment,
            timeout=15,
            check=False,
        )
        invalid_json = parse_single_json(invalid.stdout, "invalid stderr direct job")
        if invalid.returncode != 6 or invalid_json.get("code") != "job_failed":
            raise AssertionError(
                f"invalid stderr job returned the wrong result: {invalid.returncode} {invalid_json!r}"
            )
        details = json.dumps(invalid_json, ensure_ascii=False)
        if "\ufffd" not in details or len(invalid.stdout.encode("utf-8")) >= 2 * 1024 * 1024:
            raise AssertionError(
                f"invalid stderr job was not sanitized and bounded: {len(invalid.stdout)} {invalid_json!r}"
            )
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
