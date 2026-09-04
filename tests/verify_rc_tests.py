"""Fail CI when required RC contracts were silently omitted by configuration."""
import argparse
import json
import os
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--build", required=True)
parser.add_argument("--config", default="Release")
args = parser.parse_args()
result = subprocess.run(
    ["ctest", "--test-dir", args.build, "-C", args.config, "--show-only=json-v1"],
    capture_output=True, text=True, check=True,
)
tests = {test["name"] for test in json.loads(result.stdout)["tests"]}
required = {"minebackup.data_core", "minebackup.cli.process_contract", "minebackup.cli.serve_contract"}
if os.name != "nt":
    required.add("minebackup.process_lifecycle")
missing = required - tests
if missing:
    raise SystemExit(f"Required RC contracts are missing (check Python/7-Zip): {sorted(missing)}")
print(f"Required RC contracts registered: {sorted(required)}")
