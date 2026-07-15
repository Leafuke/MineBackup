#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <7-Zip-zstd-source> <output-7zz>" >&2
  exit 2
fi

source_root="$(cd "$1" && pwd)"
output="$2"
pinned_commit=d8d651b72a6a85353a23d3f19e0fd2d96c0f36b4

[[ "$(uname -m)" == arm64 ]] || { echo "7zz must be built on Apple Silicon" >&2; exit 3; }
[[ "$(git -C "$source_root" rev-parse HEAD)" == "$pinned_commit" ]] || {
  echo "7-Zip-zstd source is not the pinned v26.01-v1.5.7-R1 commit" >&2
  exit 3
}

make -C "$source_root/CPP/7zip/Bundles/Alone2" \
  -f ../../cmpl_mac_arm64.mak -j"$(sysctl -n hw.logicalcpu)"
built="$source_root/CPP/7zip/Bundles/Alone2/b/m_arm64/7zz"
[[ -x "$built" ]] || { echo "7zz build output was not produced" >&2; exit 4; }
mkdir -p "$(dirname "$output")"
install -m 0755 "$built" "$output"
lipo -info "$output" | grep -Fq arm64
! lipo -info "$output" | grep -Fq x86_64
"$output" i | grep -Fq zstd
