# Linux desktop dependencies

MineBackup builds GLFW 3.4 statically with both the X11 and Wayland backends. The
remaining desktop libraries are dynamic and must be declared by the package.

## Ubuntu 22.04 build packages

```text
build-essential cmake ninja-build pkg-config
libgl1-mesa-dev libcurl4-openssl-dev libglib2.0-dev
libgtk-3-dev libayatana-appindicator3-dev
libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
libwayland-dev libxkbcommon-dev wayland-protocols
```

`libayatana-appindicator3-dev` is preferred. CMake retains a source-compatible
fallback to `appindicator3-0.1` for distributions which still ship only the
historical implementation.

## Runtime libraries

The `.deb` dependency generator must account for at least the libraries below;
the exact package names are resolved on the Ubuntu 22.04 packaging baseline.

```text
libc6 libstdc++6 libgcc-s1 libgl1 libcurl4 libglib2.0-0
libgtk-3-0 libayatana-appindicator3-1
libx11-6 libxrandr2 libxinerama1 libxcursor1 libxi6
libwayland-client0 libxkbcommon0
```

Desktop services intentionally degrade when optional session components are
absent. `xdg-desktop-portal` plus a desktop-specific portal backend provides
OpenURI, notifications and Wayland global shortcuts. A StatusNotifier host is
required for the tray icon. Neither is a hard process-start dependency.

Before accepting a package, run `ldd` on its installed executable and reject any
`not found` entry. AppImage validation must additionally run on Ubuntu 22.04,
24.04 and 26.04 and Debian 12 and 13 without inheriting build-host library paths.
