#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 || $# -gt 4 ]]; then
  echo "usage: $0 <MineBackup.app> <7zz> <output-dir> [version]" >&2
  exit 2
fi

app="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
seven_zip="$(realpath "$2")"
output_dir="$(mkdir -p "$3" && cd "$3" && pwd)"
version="${4:-1.16.0}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
seven_zip_version=26.02-zs-v1.5.7-r2
identity="${MINEBACKUP_CODESIGN_IDENTITY:--}"
work="$output_dir/.macos-package-work"
dmg="$output_dir/MineBackup-${version}-macos-arm64.dmg"

[[ "$(uname -m)" == arm64 ]] || { echo "DMG must be built on Apple Silicon" >&2; exit 3; }
[[ "$output_dir" != / && "$work" == "$output_dir"/* ]] || { echo "unsafe output directory" >&2; exit 3; }
[[ -d "$app" && -x "$app/Contents/MacOS/MineBackup" ]] || { echo "invalid app bundle" >&2; exit 3; }
[[ -x "$seven_zip" ]] || { echo "7zz is missing" >&2; exit 3; }
lipo -info "$app/Contents/MacOS/MineBackup" | grep -Fq arm64
! lipo -info "$app/Contents/MacOS/MineBackup" | grep -Fq x86_64
"$seven_zip" i | grep -Fq zstd

rm -rf "$work" "$dmg"
mkdir -p "$work/dmg" \
  "$app/Contents/Resources/tools/7zip/$seven_zip_version" \
  "$app/Contents/Resources/licenses"
install -m 0755 "$seven_zip" \
  "$app/Contents/Resources/tools/7zip/$seven_zip_version/7zz"
install -m 0644 "$repo_root/MineBackup/Assets/tool-manifest.json" \
  "$app/Contents/Resources/tool-manifest.json"
install -m 0644 "$repo_root/LICENSE.txt" "$app/Contents/Resources/licenses/LICENSE.txt"
install -m 0644 "$repo_root/LICENSES/7zip-zstd.txt" \
  "$app/Contents/Resources/licenses/7zip-zstd.txt"
install -m 0644 "$repo_root/LICENSES/rclone.txt" \
  "$app/Contents/Resources/licenses/rclone.txt"

# Sign from the innermost executable outward. Deliberately never use --deep.
codesign --force --sign "$identity" --timestamp=none \
  "$app/Contents/Resources/tools/7zip/$seven_zip_version/7zz"
codesign --force --sign "$identity" --timestamp=none "$app/Contents/MacOS/MineBackup"
codesign --force --sign "$identity" --timestamp=none "$app"
codesign --verify --strict --verbose=2 "$app"

ditto "$app" "$work/dmg/MineBackup.app"
ln -s /Applications "$work/dmg/Applications"
hdiutil create -quiet -fs HFS+ -format UDZO -volname "MineBackup $version" \
  -srcfolder "$work/dmg" "$dmg"
hdiutil verify "$dmg"
codesign --verify --strict --verbose=2 "$work/dmg/MineBackup.app"
rm -rf "$work"
shasum -a 256 "$dmg"
