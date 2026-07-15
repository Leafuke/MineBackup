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

Repeat the packaged smoke on Ubuntu 22.04, 24.04 and 26.04. Repeat the AppImage
smoke on those releases and Debian 12 and 13.
