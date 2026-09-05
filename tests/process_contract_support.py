"""Private process fixtures and identity-aware cleanup for CLI contracts."""
from __future__ import annotations

import ctypes
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def wait_until(predicate, message: str, timeout: float = 10) -> None:
    deadline = time.monotonic() + timeout
    while not predicate():
        if time.monotonic() >= deadline:
            raise AssertionError(message)
        time.sleep(0.01)


def publish(path: Path, value: dict) -> None:
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value), encoding="utf-8")
    temporary.replace(path)


def process_helper(root: Path, name: str, descendant: bool) -> int:
    # Stay alive after TERM so two-signal tests cannot pass by graceful exit.
    signal.signal(signal.SIGTERM, lambda *_: (root / f"{name}.term").touch())
    record = {"pid": os.getpid(), "pgid": os.getpgrp() if os.name != "nt" else 0}
    publish(root / f"{name}.started.json", record)
    child = None
    if not descendant:
        child = subprocess.Popen(
            [sys.executable, __file__, str(root), name + "-child", "child"],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        wait_until(lambda: (root / f"{name}-child.ready.json").exists(), "descendant not ready")
    publish(root / f"{name}.ready.json", record)
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        if (root / f"{name}.exit").exists():
            # Deliberately leave descendants to the owning ProcessRunner.
            os._exit(0)
        time.sleep(0.01)
    if child and child.poll() is None:
        child.kill()
        child.wait(timeout=5)
    return 2


def posix_snapshot(pid: int):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return None
    # Permission errors are errors, never proof of successful cleanup.
    if sys.platform.startswith("linux"):
        try:
            value = Path(f"/proc/{pid}/stat").read_text()
        except (FileNotFoundError, ProcessLookupError):
            # A task can disappear after /proc/stat was opened but before its
            # read completes; Linux then returns ESRCH rather than ENOENT.
            return None
        fields = value[value.rfind(")") + 2:].split()
        return int(fields[2]), fields[0], fields[19]
    result = subprocess.run(
        ["ps", "-o", "pgid=,stat=,lstart=", "-p", str(pid)],
        capture_output=True, text=True, timeout=5, check=False,
    )
    if not result.stdout.strip():
        if result.returncode not in (0, 1):
            raise AssertionError(f"ps failed: {result.stderr}")
        return None
    fields = result.stdout.split()
    return int(fields[0]), fields[1], " ".join(fields[2:])


class ObservedProcess:
    def __init__(self, record: dict):
        self.pid = record["pid"]
        self.pgid = record["pgid"]
        self.handle = None
        if os.name == "nt":
            from ctypes import wintypes
            self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            self.kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
            self.kernel.OpenProcess.restype = wintypes.HANDLE
            self.kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
            self.kernel.WaitForSingleObject.restype = wintypes.DWORD
            self.kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
            self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            self.handle = self.kernel.OpenProcess(0x100000 | 0x1000 | 0x1, False, self.pid)
            if not self.handle:
                raise ctypes.WinError(ctypes.get_last_error())
        else:
            self.identity = posix_snapshot(self.pid)
            if self.identity is None or self.identity[0] != self.pgid:
                raise AssertionError(f"helper vanished before observation: {record}")

    def running(self) -> bool:
        if self.handle is not None:
            state = self.kernel.WaitForSingleObject(self.handle, 0)
            if state not in (0, 258):
                raise ctypes.WinError(ctypes.get_last_error())
            return state == 258
        current = posix_snapshot(self.pid)
        return (current is not None and current[0] == self.pgid
                and current[2] == self.identity[2] and not current[1].startswith("Z"))

    def cleanup(self) -> None:
        if self.running():
            if self.handle is not None:
                if not self.kernel.TerminateProcess(self.handle, 1):
                    raise ctypes.WinError(ctypes.get_last_error())
            else:
                # Kill the recorded group only while a recorded member still
                # proves its identity; never target the observer's own group.
                if self.pgid <= 1 or self.pgid == os.getpgrp():
                    raise AssertionError(f"unsafe fixture process group: {self.pgid}")
                try:
                    os.killpg(self.pgid, signal.SIGKILL)
                except ProcessLookupError:
                    pass

    def close(self) -> None:
        if self.handle is not None:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def observe_ready(root: Path, host: subprocess.Popen, expected: int = 4):
    def ready():
        if host.poll() is not None:
            stdout, stderr = host.communicate()
            raise AssertionError(f"host exited before ready ({host.returncode}): {stdout!r} {stderr!r}")
        return len(list(root.glob("*.ready.json"))) == expected
    wait_until(ready, f"helpers not ready in {root}")
    return [ObservedProcess(json.loads(path.read_text())) for path in sorted(root.glob("*.ready.json"))]


def assert_terminated(observed, context: str) -> None:
    def remaining():
        return [(item.pid, item.pgid) for item in observed if item.running()]
    try:
        wait_until(lambda: not remaining(), "processes still running", timeout=3)
    except AssertionError as error:
        raise AssertionError(f"{context}: surviving managed PID/PGID: {remaining()}") from error
    print(f"[PASS] {context}: terminated PID/PGID {[(p.pid, p.pgid) for p in observed]}")


def cleanup(host, root: Path, observed) -> None:
    if host.poll() is None:
        if os.name != "nt":
            try:
                host.send_signal(signal.SIGTERM)
                host.wait(timeout=0.1)
            except subprocess.TimeoutExpired:
                host.send_signal(signal.SIGINT)
            except ProcessLookupError:
                pass
        if host.poll() is None:
            try:
                host.wait(timeout=5)
            except subprocess.TimeoutExpired:
                host.kill()
                host.wait(timeout=5)
    # Also collect children from partially completed startup failures.
    known = {p.pid for p in observed}
    for path in root.glob("*.started.json"):
        record = json.loads(path.read_text())
        if record["pid"] not in known:
            try:
                observed.append(ObservedProcess(record))
            except (ProcessLookupError, AssertionError, OSError):
                pass
    try:
        for process in observed:
            process.cleanup()
    finally:
        for process in observed:
            process.close()


if __name__ == "__main__":
    raise SystemExit(process_helper(Path(sys.argv[1]), sys.argv[2], len(sys.argv) > 3))
