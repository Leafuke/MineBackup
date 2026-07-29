#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 5 ]]; then
  echo "usage: $0 <MineBackup> <output-dir> <linux-gcc-x64.zip> <linuxdeploy.AppImage> <version>" >&2
  exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
binary="$(realpath "$1")"
output_dir="$(mkdir -p "$2" && realpath "$2")"
seven_zip_archive="$(realpath "$3")"
linuxdeploy="$(realpath "$4")"
version="$5"
seven_zip_version=26.02-zs-v1.5.7-r2
work="${output_dir}/.linux-package-work"
generated_desktop="$(realpath "$(dirname "$binary")/../generated/io.github.leafuke.MineBackup.desktop")"

[[ -x "$binary" ]] || { echo "MineBackup executable is missing: $binary" >&2; exit 3; }
[[ "$(uname -m)" == x86_64 ]] || { echo "Linux packages must be built on x86_64" >&2; exit 3; }
[[ "$output_dir" != / && "$work" == "$output_dir"/* ]] || { echo "unsafe output directory" >&2; exit 3; }
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo "version must use the numeric X.Y.Z form: $version" >&2
  exit 3
}

expected_seven_zip=be246e5a284d3b5e738bad5cbb24c2662996ddb9776e09575b5099ab53fa0ba3
expected_linuxdeploy=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
echo "$expected_seven_zip  $seven_zip_archive" | sha256sum --check --status
echo "$expected_linuxdeploy  $linuxdeploy" | sha256sum --check --status

rm -rf "$work"
mkdir -p "$work/tools" "$work/icon"
unzip -q "$seven_zip_archive" 7zz -d "$work/tools"
chmod 0755 "$work/tools/7zz" "$linuxdeploy"
"$work/tools/7zz" i | grep -Fq zstd

python3 - "$repo_root/MineBackup/Assets/icons/mine10.ico" "$work/icon/io.github.leafuke.MineBackup.png" <<'PY'
from PIL import Image
import sys

source, destination = sys.argv[1:]
image = Image.open(source)
image.convert("RGBA").save(destination, format="PNG", optimize=False)
PY

stage_common() {
  local root="$1"
  install -Dm0755 "$binary" "$root/usr/bin/MineBackup"
  install -Dm0644 "$generated_desktop" \
    "$root/usr/share/applications/io.github.leafuke.MineBackup.desktop"
  install -Dm0644 "$work/icon/io.github.leafuke.MineBackup.png" \
    "$root/usr/share/icons/hicolor/64x64/apps/io.github.leafuke.MineBackup.png"
  install -Dm0644 "$repo_root/MineBackup/Assets/fontawesome-sp.otf" \
    "$root/usr/share/MineBackup/Assets/fontawesome-sp.otf"
  install -Dm0755 "$work/tools/7zz" \
    "$root/usr/share/MineBackup/tools/7zip/$seven_zip_version/7zz"
  install -Dm0644 "$repo_root/MineBackup/Assets/tool-manifest.json" \
    "$root/usr/share/MineBackup/tool-manifest.json"
  install -Dm0644 "$repo_root/LICENSE.txt" \
    "$root/usr/share/doc/minebackup/LICENSE.txt"
  install -Dm0644 "$repo_root/LICENSES/7zip-zstd.txt" \
    "$root/usr/share/doc/minebackup/7zip-zstd.txt"
  install -Dm0644 "$repo_root/LICENSES/rclone.txt" \
    "$root/usr/share/doc/minebackup/rclone.txt"
}

deb_root="$work/deb-root"
stage_common "$deb_root"
mkdir -p "$deb_root/DEBIAN"
cat >"$deb_root/DEBIAN/control" <<EOF
Package: minebackup
Version: $version
Section: utils
Priority: optional
Architecture: amd64
Maintainer: MineBackup contributors
Depends: libc6 (>= 2.39), libstdc++6 (>= 13.2), libgcc-s1, libgl1, libcurl4, libglib2.0-0, libgtk-3-0, libayatana-appindicator3-1, libx11-6, libxrandr2, libxinerama1, libxcursor1, libxi6, libwayland-client0, libxkbcommon0
Description: Cross-platform Minecraft world backup manager
 MineBackup provides local backup, restore, history and optional cloud workflows.
EOF
find "$deb_root" -type d -exec chmod 0755 {} +
dpkg-deb --root-owner-group --build "$deb_root" \
  "$output_dir/minebackup_${version}_amd64.deb"

appdir="$work/MineBackup.AppDir"
stage_common "$appdir"
export APPIMAGE_EXTRACT_AND_RUN=1
export ARCH=x86_64
export NO_STRIP=1
export OUTPUT="$output_dir/MineBackup-${version}-x86_64.AppImage"
"$linuxdeploy" --appdir "$appdir" \
  --executable "$appdir/usr/bin/MineBackup" \
  --desktop-file "$appdir/usr/share/applications/io.github.leafuke.MineBackup.desktop" \
  --icon-file "$appdir/usr/share/icons/hicolor/64x64/apps/io.github.leafuke.MineBackup.png" \
  --output appimage

chmod 0755 "$output_dir/MineBackup-${version}-x86_64.AppImage"
dpkg-deb --info "$output_dir/minebackup_${version}_amd64.deb" >/dev/null
mkdir -p "$work/verify"
(
  cd "$work/verify"
  "$output_dir/MineBackup-${version}-x86_64.AppImage" --appimage-extract >/dev/null
  test -x squashfs-root/usr/bin/MineBackup
  test -x "squashfs-root/usr/share/MineBackup/tools/7zip/$seven_zip_version/7zz"
)
rm -rf "$work"

sha256sum "$output_dir/minebackup_${version}_amd64.deb" \
  "$output_dir/MineBackup-${version}-x86_64.AppImage"
