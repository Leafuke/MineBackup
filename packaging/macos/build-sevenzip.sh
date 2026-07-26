#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <7-Zip-zstd-source> <output-7zz>" >&2
  exit 2
fi

source_root="$(cd "$1" && pwd)"
output="$2"
pinned_commit=2dcb6815c0e6877988171c47e1824b86172ff3b2

[[ "$(uname -m)" == arm64 ]] || { echo "7zz must be built on Apple Silicon" >&2; exit 3; }
[[ "$(git -C "$source_root" rev-parse HEAD)" == "$pinned_commit" ]] || {
  echo "7-Zip-zstd source is not the pinned v26.02-v1.5.7-R2 commit" >&2
  exit 3
}

# The pinned 7-Zip ZS makefile enables -Werror.  AppleClang 17 diagnoses
# warnings in its bundled BLAKE3 and MD4 sources that newer AppleClang accepts.
# Keep the upstream warning set, but do not turn third-party warnings into
# errors; the pinned revision and the resulting binary are verified below.
make -C "$source_root/CPP/7zip/Bundles/Alone2" \
  -f ../../cmpl_mac_arm64.mak \
  CFLAGS_WARN_WALL="-Wall -Wextra" \
  -j"$(sysctl -n hw.logicalcpu)"
built="$source_root/CPP/7zip/Bundles/Alone2/b/m_arm64/7zz"
[[ -x "$built" ]] || { echo "7zz build output was not produced" >&2; exit 4; }
mkdir -p "$(dirname "$output")"
install -m 0755 "$built" "$output"
lipo -info "$output" | grep -Fq arm64
! lipo -info "$output" | grep -Fq x86_64
"$output" i | grep -Fq zstd
