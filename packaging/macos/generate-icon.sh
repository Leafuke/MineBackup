#!/usr/bin/env bash
set -euo pipefail

[[ $# -eq 2 ]] || { echo "usage: generate-icon.sh <source.png> <output.icns>" >&2; exit 2; }

source_png="$1"
output_icns="$2"
[[ -f "$source_png" ]] || { echo "icon source is missing: $source_png" >&2; exit 3; }
command -v sips >/dev/null || { echo "sips is required to generate the macOS icon" >&2; exit 3; }
command -v iconutil >/dev/null || { echo "iconutil is required to generate the macOS icon" >&2; exit 3; }

work="$(mktemp -d "${TMPDIR:-/tmp}/minebackup-icon.XXXXXX")"
trap 'rm -rf "$work"' EXIT
iconset="$work/MineBackup.iconset"
mkdir -p "$iconset" "$(dirname "$output_icns")"

make_icon() {
    local size="$1"
    local name="$2"
    sips -s format png -z "$size" "$size" "$source_png" --out "$iconset/$name" >/dev/null
}

make_icon 16 icon_16x16.png
make_icon 32 icon_16x16@2x.png
make_icon 32 icon_32x32.png
make_icon 64 icon_32x32@2x.png
make_icon 128 icon_128x128.png
make_icon 256 icon_128x128@2x.png
make_icon 256 icon_256x256.png
make_icon 512 icon_256x256@2x.png
make_icon 512 icon_512x512.png
make_icon 1024 icon_512x512@2x.png

iconutil --convert icns --output "$output_icns" "$iconset"
