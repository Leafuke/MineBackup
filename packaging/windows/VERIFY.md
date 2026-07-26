# Windows release verification

Run the automated gate from a Developer PowerShell:

```powershell
cmake --preset windows-msvc-x64
cmake --build --preset windows-msvc-x64-release
ctest --preset windows-msvc-x64-release --output-on-failure
cmake --build --preset windows-msvc-x64-release --target check_msbuild_source_parity
msbuild .\MineBackup.sln /p:Configuration=Release /p:Platform=x64 /m

.\cmake\VerifyWindowsArtifact.ps1 -Executable .\x64\Release\MineBackup.exe
.\cmake\SmokeTestWindowsArtifact.ps1 -Executable .\x64\Release\MineBackup.exe
```

Repeat the artifact scripts against the SignPath result. They verify 1.16.0
VERSIONINFO, x64 GUI subsystem, both icons, the embedded font and 7-Zip,
absence of dynamic VC/UCRT imports, startup survival and immediate refusal of
the deprecated `--service` mode.

Complete the following on Windows 10 22H2 x64 and Windows 11:

1. Exercise open/save/folder dialogs, tray Open/Exit, a notification and both
   global hotkeys. A conflicting hotkey must leave the old pair active.
2. Enable current-user autostart and confirm there is exactly one `MineBackup`
   value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, containing
   only the quoted current executable and `--autostart`. Move the executable,
   start normally and confirm MineBackup repairs the path and reports it.
3. Start two copies with one profile and confirm the second activates the first;
   two different `--data-dir` profiles must run independently.
4. Confirm there is no UI or API for installing/starting Service Mode. For an
   actual old MineBackup service, inspect the shown ImagePath, approve removal
   and UAC, and confirm the service is removed. Point a disposable test service
   at another executable and confirm MineBackup refuses to modify it.
5. Create and restore LZMA2/zstd Full, Smart and Clean Restore chains, then run
   the cross-platform fixture matrix from `docs/release-process.md`.
