#!/usr/bin/env python3
"""Exercise profile locks, graceful cancellation and forced tree cleanup."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
from process_contract_support import wait_until, observe_ready, assert_terminated, cleanup


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




def launch(command, environment):
    return subprocess.Popen(
        command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, encoding="utf-8", errors="strict", env=environment,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0,
    )


def run(command, environment):
    return subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True,
                          text=True, encoding="utf-8", env=environment, timeout=15)


def case(cli: Path, root: Path, environment, signals=None, host_binary=None):
    root.mkdir(parents=True)
    profile = root / "profile"
    write_profile(profile)
    jobs_path = profile / "config" / "jobs.json"
    jobs = json.loads(jobs_path.read_text(encoding="utf-8"))
    steps = jobs["jobs"][0]["stages"][0]["steps"]
    template = steps[0]
    steps.clear()
    helper = str(Path(__file__).with_name("process_contract_support.py"))
    for index, name in enumerate(("a", "b")):
        step = dict(template)
        step["stepId"] = f"44444444-4444-4444-8444-{index + 1:012d}"
        step["arguments"] = [helper, str(root), name]
        steps.append(step)
    jobs_path.write_text(json.dumps(jobs), encoding="utf-8")
    prefix = [str(cli), "--data-dir", str(profile), "--json", "--no-network"]
    command = prefix + ["job", "run", "--job", JOB_ID]
    if host_binary:
        command = [str(host_binary), "--signal-contract-host", sys.executable, helper, str(root)]
    host = launch(command, environment)
    observed = []
    try:
        observed = observe_ready(root, host)
        if not signals:
            busy = run(prefix + ["config", "list"], environment)
            if busy.returncode != 3 or parse_single_json(busy.stdout, "lock")["code"] != "profile_busy":
                raise AssertionError(f"exclusive profile lock failed: {busy.stdout}")
            host.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGTERM)
        else:
            host.send_signal(signals[0])
            if host_binary:
                wait_until(lambda: (root / "cancel-blocked").exists(), "stop callback was not reached")
            else:
                wait_until(lambda: all((root / f"{name}.term").exists() for name in ("a", "b")),
                           "first signal was not forwarded", timeout=3)
            # Acknowledgment prevents signal coalescing and proves the first
            # handler ran. An already-exited host is a failure, not success.
            if host.poll() is not None:
                raise AssertionError("host exited gracefully before the second signal")
            host.send_signal(signals[1])
        stdout, stderr = host.communicate(timeout=5 if signals else 15)
        if host.returncode != 9:
            raise AssertionError(f"expected exit 9, got {host.returncode}: {stdout!r} {stderr!r}")
        if signals:
            if stdout.strip():
                raise AssertionError(f"force path emitted graceful JSON/output: {stdout!r}")
        else:
            envelope = parse_single_json(stdout, "graceful signal")
            if envelope.get("code") != "cancelled" or envelope.get("ok") is not False:
                raise AssertionError(f"wrong cancellation envelope: {envelope}")
        assert_terminated(observed, root.name)
        reopened = run(prefix + ["config", "list"], environment)
        if reopened.returncode != 0:
            raise AssertionError(f"profile did not reopen: {reopened.stdout!r} {reopened.stderr!r}")
        if not signals:
            invalid = run(prefix + ["job", "run", "--job", INVALID_JOB_ID], environment)
            envelope = parse_single_json(invalid.stdout, "invalid stderr")
            if invalid.returncode != 6 or envelope.get("code") != "job_failed":
                raise AssertionError(f"wrong failing-job result: {envelope}")
            if "\ufffd" not in json.dumps(envelope, ensure_ascii=False) or len(invalid.stdout.encode("utf-8")) >= 2 * 1024 * 1024:
                raise AssertionError("external stderr was not UTF-8 sanitized and bounded")
    finally:
        cleanup(host, root, observed)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--signal-host", type=Path, required=True)
    parser.add_argument("--work-root", type=Path)
    args = parser.parse_args()
    if args.work_root:
        args.work_root.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="signal-contract-", dir=args.work_root))
    environment = os.environ.copy()
    environment.pop("DISPLAY", None)
    environment.pop("WAYLAND_DISPLAY", None)
    case(args.cli, root / "graceful", environment)
    if os.name != "nt":
        for name, signals in (("term-term", (signal.SIGTERM, signal.SIGTERM)),
                              ("int-int", (signal.SIGINT, signal.SIGINT)),
                              ("term-int", (signal.SIGTERM, signal.SIGINT))):
            case(args.cli, root / name, environment, signals)
    if args.signal_host:
        event = signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGTERM
        case(args.cli, root / "blocked-callback", environment, (event, event), args.signal_host)
    print(f"[PASS] CLI process contract; evidence: {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
