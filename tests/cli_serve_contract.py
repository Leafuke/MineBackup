#!/usr/bin/env python3
"""Exercise serve forwarding, cancellation, disconnect, and shutdown contracts."""

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


CONFIG_ID = "51111111-1111-4111-8111-111111111111"
JOB_ID = "52222222-2222-4222-8222-222222222222"
STAGE_ID = "53333333-3333-4333-8333-333333333333"
STEP_ID = "54444444-4444-4444-8444-444444444444"


def write_profile(profile: Path) -> None:
    world = profile / "server" / "world"
    (profile / "config").mkdir(parents=True)
    world.mkdir(parents=True)
    (world / "level.dat").write_bytes(b"minebackup-serve-contract")
    config = "\n".join(
        [
            "[Config1]",
            "ConfigName=Serve contract",
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
                "name": "serve cancellation contract",
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
            }
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


def run_cli(cli: Path, profile: Path, environment: dict[str, str], *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(cli), "--data-dir", str(profile), "--json", *arguments],
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


def status(cli: Path, profile: Path, environment: dict[str, str]) -> dict:
    response = run_cli(cli, profile, environment, "serve", "status")
    if response.returncode != 0:
        raise AssertionError(
            f"serve status failed ({response.returncode}): {response.stderr!r} {response.stdout!r}"
        )
    return parse_single_json(response.stdout, "serve status")


def wait_for_active(cli: Path, profile: Path, environment: dict[str, str], expected: int) -> None:
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        value = status(cli, profile, environment)
        if value.get("data", {}).get("activeOperationCount") == expected:
            return
        time.sleep(0.05)
    raise AssertionError(f"serve did not reach activeOperationCount={expected}")


def signal_client(process: subprocess.Popen[str]) -> None:
    if os.name == "nt":
        process.send_signal(signal.CTRL_BREAK_EVENT)
    else:
        process.send_signal(signal.SIGINT)


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
        Path(tempfile.mkdtemp(prefix="minebackup-cli-serve-"))
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
    creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    serve = subprocess.Popen(
        [str(cli), "--data-dir", str(profile), "--json", "--no-network", "serve"],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=environment,
        creationflags=creation_flags,
    )
    clients: list[subprocess.Popen[str]] = []
    try:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if serve.poll() is not None:
                stdout, stderr = serve.communicate()
                raise AssertionError(
                    f"serve exited during startup ({serve.returncode}): {stderr!r} {stdout!r}"
                )
            probe = run_cli(cli, profile, environment, "serve", "status")
            if probe.returncode == 0:
                break
            time.sleep(0.05)
        else:
            raise AssertionError("serve did not become ready")

        listed = run_cli(cli, profile, environment, "config", "list")
        listed_json = parse_single_json(listed.stdout, "forwarded config list")
        if listed.returncode != 0 or listed_json.get("command") != "config.list":
            raise AssertionError(f"config list was not transparently forwarded: {listed_json!r}")

        manifest = root / "exported-manifest.json"
        exported = run_cli(
            cli, profile, environment,
            "profile", "export", "--output", str(manifest),
        )
        if exported.returncode != 0 or not manifest.is_file():
            raise AssertionError(
                f"forwarded profile export failed: {exported.stderr!r} {exported.stdout!r}"
            )
        applied = run_cli(
            cli, profile, environment,
            "profile", "apply", "--file", str(manifest),
        )
        applied_json = parse_single_json(applied.stdout, "forwarded profile apply")
        if applied.returncode != 0 or applied_json.get("command") != "profile.apply":
            raise AssertionError(f"profile apply did not reload serve: {applied_json!r}")

        job_command = [
            str(cli), "--data-dir", str(profile), "--json",
            "job", "run", "--job", JOB_ID,
        ]
        cancellable = subprocess.Popen(
            job_command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=environment,
            creationflags=creation_flags,
        )
        clients.append(cancellable)
        wait_for_active(cli, profile, environment, 1)
        signal_client(cancellable)
        stdout, stderr = cancellable.communicate(timeout=15)
        if cancellable.returncode != 9:
            raise AssertionError(
                f"forwarded cancellation returned {cancellable.returncode}: {stderr!r} {stdout!r}"
            )
        cancelled = parse_single_json(stdout, "forwarded cancellation")
        if cancelled.get("code") != "cancelled":
            raise AssertionError(f"forwarded cancellation returned the wrong result: {cancelled!r}")
        wait_for_active(cli, profile, environment, 0)

        disconnected = subprocess.Popen(
            job_command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=environment,
            creationflags=creation_flags,
        )
        clients.append(disconnected)
        wait_for_active(cli, profile, environment, 1)
        disconnected.kill()
        disconnected.communicate(timeout=5)

        stopped = run_cli(cli, profile, environment, "serve", "stop")
        stopped_json = parse_single_json(stopped.stdout, "serve stop")
        if stopped.returncode != 0 or stopped_json.get("command") != "serve.stop":
            raise AssertionError(f"serve stop failed: {stopped_json!r}")
        serve_stdout, serve_stderr = serve.communicate(timeout=15)
        if serve.returncode != 0:
            raise AssertionError(
                f"serve did not stop cleanly ({serve.returncode}): {serve_stderr!r} {serve_stdout!r}"
            )
        final = parse_single_json(serve_stdout, "serve final output")
        if final.get("command") != "serve" or final.get("code") != "success":
            raise AssertionError(f"serve returned the wrong final result: {final!r}")
    finally:
        for client in clients:
            if client.poll() is None:
                client.kill()
                client.wait(timeout=5)
        if serve.poll() is None:
            serve.kill()
            serve.wait(timeout=5)
        if owned_root:
            shutil.rmtree(root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"serve process contract failed: {error}", file=sys.stderr)
        raise
