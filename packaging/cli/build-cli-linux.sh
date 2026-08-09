#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
  echo "usage: $0 <minebackup-cli> <output-dir> <linux-gcc-x64.zip> <version>" >&2
  exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
binary="$(realpath "$1")"
output_dir="$(mkdir -p "$2" && realpath "$2")"
seven_zip_archive="$(realpath "$3")"
version="$4"
seven_zip_version=26.02-zs-v1.5.7-r2
expected_seven_zip=be246e5a284d3b5e738bad5cbb24c2662996ddb9776e09575b5099ab53fa0ba3
work="$output_dir/.cli-package-work"

[[ -x "$binary" ]] || { echo "minebackup-cli is missing: $binary" >&2; exit 3; }
[[ "$(uname -m)" == x86_64 ]] || { echo "CLI assets must be built on x86_64" >&2; exit 3; }
[[ "$output_dir" != / && "$work" == "$output_dir"/* ]] || { echo "unsafe output directory" >&2; exit 3; }
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "invalid version: $version" >&2; exit 3; }
echo "$expected_seven_zip  $seven_zip_archive" | sha256sum --check --status

rm -rf "$work"
mkdir -p "$work/input"
unzip -p "$seven_zip_archive" 7zz >"$work/input/7zz"
chmod 0755 "$work/input/7zz"
"$work/input/7zz" i | grep -Fq zstd

stage_common() {
  local root="$1"
  install -Dm0755 "$binary" "$root/bin/minebackup-cli"
  install -Dm0755 "$work/input/7zz" \
    "$root/Resources/tools/7zip/$seven_zip_version/7zz"
  install -Dm0644 "$repo_root/LICENSE.txt" "$root/LICENSE.txt"
  install -Dm0644 "$repo_root/LICENSES/7zip-zstd.txt" "$root/LICENSES/7zip-zstd.txt"
  install -Dm0644 "$repo_root/LICENSES/spdlog.txt" "$root/LICENSES/spdlog.txt"
  install -Dm0644 "$repo_root/LICENSES/fmt.txt" "$root/LICENSES/fmt.txt"
  install -Dm0644 "$repo_root/LICENSES/knotlink-sdk-cpp.txt" "$root/LICENSES/knotlink-sdk-cpp.txt"
  install -Dm0644 "$repo_root/packaging/cli/server-manifest.example.json" \
    "$root/examples/server-manifest.json"
  install -Dm0644 "$repo_root/packaging/cli/systemd/minebackup-backup@.service" \
    "$root/scheduling/systemd/minebackup-backup@.service"
  install -Dm0644 "$repo_root/packaging/cli/systemd/minebackup-backup@.timer" \
    "$root/scheduling/systemd/minebackup-backup@.timer"
  install -Dm0644 "$repo_root/packaging/cli/systemd/example.env" \
    "$root/scheduling/systemd/example.env"
  install -Dm0644 "$repo_root/packaging/cli/windows/MineBackup-Job.xml" \
    "$root/scheduling/windows/MineBackup-Job.xml"
}

archive_root="$work/MineBackup-CLI-$version-linux-x64"
stage_common "$archive_root"
(
  cd "$work"
  tar --sort=name --mtime='UTC 1970-01-01' --owner=0 --group=0 --numeric-owner \
    -czf "$output_dir/MineBackup-CLI-$version-linux-x64.tar.gz" \
    "$(basename "$archive_root")"
)

deb_root="$work/deb-root"
install -Dm0755 "$binary" "$deb_root/usr/bin/minebackup-cli"
install -Dm0755 "$work/input/7zz" \
  "$deb_root/usr/share/MineBackup/tools/7zip/$seven_zip_version/7zz"
install -Dm0644 "$repo_root/LICENSE.txt" \
  "$deb_root/usr/share/doc/minebackup-cli/LICENSE.txt"
install -Dm0644 "$repo_root/LICENSES/7zip-zstd.txt" \
  "$deb_root/usr/share/doc/minebackup-cli/7zip-zstd.txt"
install -Dm0644 "$repo_root/packaging/cli/server-manifest.example.json" \
  "$deb_root/usr/share/doc/minebackup-cli/examples/server-manifest.json"
install -Dm0644 "$repo_root/packaging/cli/systemd/minebackup-backup@.service" \
  "$deb_root/usr/lib/systemd/system/minebackup-backup@.service"
install -Dm0644 "$repo_root/packaging/cli/systemd/minebackup-backup@.timer" \
  "$deb_root/usr/lib/systemd/system/minebackup-backup@.timer"
install -Dm0644 "$repo_root/packaging/cli/systemd/example.env" \
  "$deb_root/usr/share/doc/minebackup-cli/examples/systemd.env"
mkdir -p "$deb_root/DEBIAN"
cat >"$deb_root/DEBIAN/control" <<EOF
Package: minebackup-cli
Version: $version
Section: utils
Priority: optional
Architecture: amd64
Maintainer: MineBackup contributors
Depends: libc6 (>= 2.39), libstdc++6 (>= 13.2), libgcc-s1
Description: Headless Minecraft world backup and restore runtime
 MineBackup CLI manages server profiles, jobs, backups, verification and cold restore.
EOF
find "$deb_root" -type d -exec chmod 0755 {} +
dpkg-deb --root-owner-group --build "$deb_root" \
  "$output_dir/minebackup-cli_${version}_amd64.deb"

tar -tzf "$output_dir/MineBackup-CLI-$version-linux-x64.tar.gz" | grep -Fq '/bin/minebackup-cli'
dpkg-deb --contents "$output_dir/minebackup-cli_${version}_amd64.deb" | grep -Fq 'usr/bin/minebackup-cli'
sha256sum "$output_dir/MineBackup-CLI-$version-linux-x64.tar.gz" \
  "$output_dir/minebackup-cli_${version}_amd64.deb"
rm -rf "$work"
