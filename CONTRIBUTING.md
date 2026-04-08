# Contributing to MineBackup

Thank you for your interest in contributing to MineBackup.

## Ways to Contribute

- Report bugs
- Propose or implement improvements
- Improve documentation
- Help with localization and wording

## Before You Start

1. Search existing issues to avoid duplicates.
2. For larger changes, open an issue first to discuss scope and approach.
3. Keep pull requests focused on one topic.

## Development Setup

### Requirements

- C++20 compiler
- Git
- CMake 3.15+ (Linux/macOS workflows)
- Visual Studio 2022 with v143 toolset (Windows)

### Repository Layout

- Main source: `MineBackup/src`
- Third-party source: `MineBackup/third_party`
- Windows solution: `MineBackup.sln`
- CMake build entry: `CMakeLists.txt`

## Build Instructions

### Windows (recommended for local development)

Use Visual Studio:

1. Open `MineBackup.sln`
2. Select `Release|x64` (or `Debug|x64`)
3. Build the `MineBackup` project

Command line option:

```powershell
msbuild .\MineBackup.sln /p:Configuration=Release /p:Platform=x64
```

### Linux/macOS (CMake)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## Code and Change Guidelines

- Follow existing code style and naming conventions in nearby files.
- Avoid unrelated refactors in the same pull request.
- Add comments only where logic is not obvious.
- Keep runtime data behavior consistent unless discussed first.
  MineBackup currently stores runtime files (such as config/history/log/theme)
  next to the executable or working directory.
- If your change affects user behavior, update documentation and release notes.

## Localization Notes

- UI language strings are maintained in `MineBackup/src/infra/i18n.cpp`.
- Ensure both Chinese and English entries remain consistent when applicable.

## Pull Request Checklist

Before opening a PR, please verify:

- The project builds successfully on your target platform.
- You tested the affected workflows manually.
- New or changed behavior is documented.
- The PR description clearly explains what changed.
- The PR description clearly explains why it changed.
- The PR description clearly explains how it was tested.

For UI changes, screenshots are appreciated.

## Reporting Bugs

Use GitHub Issues:

https://github.com/Leafuke/MineBackup/issues

Please include:

- Version and platform
- Steps to reproduce
- Expected result and actual result
- Logs or screenshots if available

## Security Issues

For vulnerabilities, please follow:

- `SECURITY.md`
