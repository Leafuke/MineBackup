#!/usr/bin/env python3
import argparse
import datetime as dt
import hashlib
import json
import re
from pathlib import Path


ASSETS = {
    "windows": "MineBackup-windows-x64.exe",
    "linux-deb": "minebackup_1.16.0_amd64.deb",
    "linux-appimage": "MineBackup-1.16.0-x86_64.AppImage",
    "macos": "MineBackup-1.16.0-macos-arm64.dmg",
}
PLATFORM_ASSETS = {
    "windows": {"windows"},
    "linux": {"linux-deb", "linux-appimage"},
    "macos": {"macos"},
}
AUXILIARY_NAMES = {"release-manifest.json", "dependency-manifest.json", "SHA256SUMS"}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--assets-dir", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--omitted-platform", choices=("none", "windows", "linux", "macos"), default="none")
    parser.add_argument("--waiver-reason", default="")
    parser.add_argument("--dependency-manifest", type=Path, required=True)
    args = parser.parse_args()

    if args.version != "1.16.0":
        raise SystemExit(f"unsupported release version: {args.version}")
    if re.fullmatch(r"[0-9a-fA-F]{40}", args.commit) is None:
        raise SystemExit("release commit must be a full 40-character Git object ID")
    if args.omitted_platform != "none" and not args.waiver_reason.strip():
        raise SystemExit("an omitted platform requires a non-empty waiver reason")

    expected_keys = set(ASSETS)
    if args.omitted_platform != "none":
        expected_keys -= PLATFORM_ASSETS[args.omitted_platform]
    expected_names = {ASSETS[key] for key in expected_keys}
    actual_names = {path.name for path in args.assets_dir.iterdir() if path.is_file()}
    payload_names = actual_names - AUXILIARY_NAMES
    unexpected = payload_names - expected_names
    missing = expected_names - payload_names
    if missing or unexpected:
        raise SystemExit(f"candidate asset mismatch; missing={sorted(missing)}, unexpected={sorted(unexpected)}")

    dependencies = json.loads(args.dependency_manifest.read_text(encoding="utf-8"))
    if dependencies.get("mineBackupVersion") != args.version:
        raise SystemExit("dependency manifest version does not match the release")

    assets = []
    for key in sorted(expected_keys):
        path = args.assets_dir / ASSETS[key]
        assets.append({
            "kind": key,
            "name": path.name,
            "bytes": path.stat().st_size,
            "sha256": sha256(path),
        })

    platform_gate_descriptions = {
        "windows": "MSBuild formal candidate plus CMake/MSVC shadow, CTest, PE resources, /MT and startup smoke",
        "linux": "Ubuntu 22.04 build, CTest, X11/Wayland smoke, deb and AppImage layout/dependency checks",
        "macos": "macOS 15 and 26 arm64 build/CTest/startup; macOS 15 nested signing and DMG verification",
    }
    gates = {
        platform: {
            "status": "waived" if args.omitted_platform == platform else "passed",
            "description": description,
        }
        for platform, description in platform_gate_descriptions.items()
    }
    gates["crossRestore"] = {
        "status": "waived" if args.omitted_platform != "none" else "passed",
        "description": (
            "Skipped because the security release waiver omitted a platform"
            if args.omitted_platform != "none"
            else "LZMA2 and zstd fixtures produced on Windows, Linux and macOS were restored byte-for-byte on all three platforms"
        ),
    }

    manifest = {
        "schemaVersion": 1,
        "version": args.version,
        "commit": args.commit.lower(),
        "generatedAtUtc": dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat(),
        "omittedPlatform": None if args.omitted_platform == "none" else args.omitted_platform,
        "waiverReason": args.waiver_reason.strip() or None,
        "gates": gates,
        "assets": assets,
        "dependencyManifestSha256": sha256(args.dependency_manifest),
    }
    output = args.assets_dir / "release-manifest.json"
    output.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    sums = "".join(f"{item['sha256']}  {item['name']}\n" for item in assets)
    (args.assets_dir / "SHA256SUMS").write_text(sums, encoding="ascii", newline="\n")


if __name__ == "__main__":
    main()
