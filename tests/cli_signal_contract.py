"""Exercise actual CLI jobs and the isolated native signal-handler host."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

from cli_process_contract import write_profile, parse_single_json, JOB_ID, INVALID_JOB_ID
from process_contract_support import wait_until, observe_ready, assert_terminated, cleanup


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
    parser.add_argument("--signal-host", type=Path)
    parser.add_argument("--work-root", type=Path)
    parser.add_argument("--only-forced", action="store_true")
    args = parser.parse_args()
    if args.work_root:
        args.work_root.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="signal-contract-", dir=args.work_root))
    environment = os.environ.copy()
    environment.pop("DISPLAY", None)
    environment.pop("WAYLAND_DISPLAY", None)
    if not args.only_forced:
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
