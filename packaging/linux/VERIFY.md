# Linux desktop verification

Run these checks on a real Linux host; they are intentionally not simulated on
the Windows development machine.

## Build and data tests

Install the packages listed in `DEPENDENCIES.md`, plus `xvfb`, `weston`,
`dbus-x11`, `xdg-desktop-portal` and the portal backend for the desktop under
test. Then run:

```bash
cmake --preset linux-x64 -DBUILD_TESTING=ON
cmake --build build/linux-x64 --parallel
ctest --preset linux-x64 --output-on-failure
ldd build/linux-x64/bin/MineBackup | tee build/linux-x64/ldd.txt
! grep -F 'not found' build/linux-x64/ldd.txt
```

Place the packaged read-only resources beside the executable using the final
package layout before desktop smoke testing. During an unpackaged developer
build, at minimum copy the icon font into
`build/linux-x64/bin/Resources/Assets/fontawesome-sp.otf` and ensure a capable
`7z`/`7zz` is installed.

## Automatic GLFW backend smoke

Do not pass MineBackup a backend-selection option. The helper changes only the
session environment and verifies that GLFW itself automatically selected the
expected compiled backend:

```bash
chmod +x packaging/linux/verify-desktop.sh
packaging/linux/verify-desktop.sh build/linux-x64/bin/MineBackup all
```

The expected output captured by the helper is `X11` under Xvfb and `Wayland`
under headless Weston.

## Package build and install

The release workflow downloads the pinned 7-Zip ZS and linuxdeploy inputs and
runs:

```bash
bash packaging/linux/build-packages.sh \
  build/linux-x64/bin/MineBackup build/release \
  build/package-inputs/linux-gcc-x64.zip \
  build/package-inputs/linuxdeploy-x86_64.AppImage

sudo apt install ./build/release/minebackup_1.16.0_amd64.deb
MineBackup --data-dir "$(mktemp -d)/profile"

build/release/MineBackup-1.16.0-x86_64.AppImage \
  --data-dir "$(mktemp -d)/profile"
```

Inspect `dpkg-deb --contents` and `dpkg-deb --field ... Depends`, then run
`ldd /usr/bin/MineBackup` and reject every `not found`. Extract the AppImage and
confirm it contains the binary, desktop entry, icon, licenses, font and pinned
`Resources/tools/7zip/26.01-zs-v1.5.7-r1/7zz` equivalent under
`usr/share/MineBackup`. Move the AppImage and verify a previously enabled
autostart path is repaired on the next normal launch. Place `portable.flag`
beside it and confirm only that case uses adjacent `MineBackupData`.

## Manual capability checks

1. In an X11 session, bind both hotkeys and verify backup and restore fire. Lock
   modifiers must not change the result.
2. In a Wayland session with a GlobalShortcuts portal, bind both shortcuts in a
   single permission dialog. Confirm the settings diagnostic displays the
   portal-returned trigger descriptions, then activate both shortcuts.
3. Deny the shortcut request, and repeat with no GlobalShortcuts portal. The UI
   must report `PermissionRequired` or `Unavailable`; it must never fall back to
   XGrabKey on Wayland.
4. Exercise open-file, open-folder, save-file, OpenURI and notification through
   the active desktop portal.
5. Run without a StatusNotifier host and start with `--silent-startup`. The main
   window must remain visible and the diagnostic must explain that the tray host
   is unavailable. The saved hide-to-tray preference must remain unchanged.
6. Run with an Ayatana-compatible tray host. Confirm Open and Exit work and that
   no second GLib main loop or tray thread remains after exit.

Repeat the packaged smoke on Ubuntu 24.04 and 26.04. Repeat the AppImage smoke
on those releases and Debian 13. Ubuntu 22.04 and Debian 12 are unsupported.
