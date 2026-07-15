# macOS 15+ native desktop verification

These checks run on real Apple Silicon hosts. The Windows development machine
does not attempt to cross-compile or emulate AppKit.

## Build, architecture and bundle

```bash
brew install cmake ninja sevenzip
cmake --preset macos-arm64 -DBUILD_TESTING=ON
cmake --build build/macos-arm64 --parallel
ctest --preset macos-arm64 --output-on-failure

app=build/macos-arm64/bin/MineBackup.app
plutil -lint "$app/Contents/Info.plist"
test "$(plutil -extract CFBundleIdentifier raw "$app/Contents/Info.plist")" = io.github.leafuke.MineBackup
test "$(plutil -extract CFBundleShortVersionString raw "$app/Contents/Info.plist")" = 1.16.0
test "$(plutil -extract LSMinimumSystemVersion raw "$app/Contents/Info.plist")" = 15.0
lipo -info "$app/Contents/MacOS/MineBackup"
codesign --force --sign - --timestamp=none "$app"
codesign --verify --strict --verbose=2 "$app"
```

`lipo` must report only `arm64`. Repeat the build and startup smoke on macOS 15
and macOS 26.

## Build and mount the DMG

On macOS 15 arm64, build the pinned 7-Zip ZS source and package from the inside
out:

```bash
git init build/7zip-zstd
git -C build/7zip-zstd remote add origin https://github.com/mcmilk/7-Zip-zstd.git
git -C build/7zip-zstd fetch --depth 1 origin d8d651b72a6a85353a23d3f19e0fd2d96c0f36b4
git -C build/7zip-zstd checkout --detach FETCH_HEAD
bash packaging/macos/build-sevenzip.sh build/7zip-zstd build/package-inputs/7zz
MINEBACKUP_CODESIGN_IDENTITY=- bash packaging/macos/build-dmg.sh \
  build/macos-arm64/bin/MineBackup.app build/package-inputs/7zz build/release

hdiutil attach build/release/MineBackup-1.16.0-macos-arm64.dmg
codesign --verify --strict --verbose=2 /Volumes/'MineBackup 1.16.0'/MineBackup.app
```

The packaging script signs the nested `7zz`, main executable and `.app` in that
order and never uses `codesign --deep`. Copy the app to `/Applications`, launch,
then detach the DMG. The package is ad-hoc signed but not notarized: document
only **Privacy & Security → Open Anyway**. Do not disable Gatekeeper or suggest
`xattr`.

## Read-only app and data locations

```bash
profile="$(mktemp -d)/MineBackupProfile"
chmod -R a-w "$app"
"$app/Contents/MacOS/MineBackup" --data-dir "$profile"
```

Confirm config, data, state, cache, runtime, tools and logs are created only in
the external profile or standard Library directories. No file inside `.app`
may be created or modified.

## Native desktop acceptance

1. Exercise open-file, open-folder and save-file panels. Verify aliases resolve
   and cancellation is reported without changing the selected path.
2. Open an HTTPS URL, open a folder and reveal a backup in Finder through
   NSWorkspace.
3. Trigger a notification. Test the first authorization prompt, allowed state,
   denied state and foreground presentation; each state must be distinct in the
   capability panel.
4. Create and remove the NSStatusItem. Confirm Open activates and focuses the
   GLFW window and Exit performs the normal coordinated shutdown.
5. Bind backup and restore hotkeys, then attempt a conflicting binding. The old
   pair must remain active after the failed reconfiguration.
6. Move `MineBackup.app` outside Applications and verify launch-at-login reports
   a location requirement. Move it to `/Applications`, enable it, test
   `Enabled`, `RequiresApproval` and revoked states, and use the in-app button to
   open Login Items settings.
7. With one special configuration marked AutoExecute, log out and back in.
   Confirm the `keyAELaunchedAsLogInItem` launch event follows the same stable-ID
   execution path as `--autostart`. A normal Finder launch must not be treated as
   an autostart launch.
8. Mark the app read-only and repeat launch, tray, dialogs, notification and
   login-item status checks.

## Sanitizer pass

On a macOS 15 development host, build a separate candidate and repeat the
startup/exit, dialogs, tray and notification smoke:

```bash
cmake -S . -B build/macos-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_OBJCXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build/macos-asan --parallel
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  build/macos-asan/bin/MineBackup.app/Contents/MacOS/MineBackup \
  --data-dir "$(mktemp -d)/profile"
```

There must be no AppKit main-thread violations, Objective-C lifetime failures,
AddressSanitizer errors or UndefinedBehaviorSanitizer errors.
