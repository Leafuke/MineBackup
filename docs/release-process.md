# MineBackup 1.16 release process

Normal pull requests and pushes build and test Windows, Linux and macOS
candidates. A `v1.16.0` tag starts one orchestration workflow; platform jobs
upload non-public candidate artifacts and never query or mutate the "latest"
GitHub Release.

The convergence job accepts only the seven fixed asset names (four desktop
assets plus Windows ZIP, Linux tar and Linux deb server assets), checks that the
annotated tag and every candidate refer to the same commit, and generates
`release-manifest.json`, `dependency-manifest.json` and `SHA256SUMS`. The only
publishing job uses the protected `release` environment. It uploads to a draft
for that exact tag and makes the draft public only after every upload succeeds.
A rerun can resume that same draft but refuses to modify an already-published
release.

A security-only missing-platform release must be started manually, select the
one omitted platform, provide a reason and pass the protected-environment
approval. The omission and reason are recorded in the release manifest. Normal
tag releases cannot use the waiver. The public Release notes also identify the
omitted platform and approved reason.

Before pushing the tag, confirm that the selected commit is the candidate that
passed the complete matrix:

```powershell
git switch develop
git status --short
git log -1 --oneline
git tag -a v1.16.0 -m "MineBackup 1.16.0"
git push origin v1.16.0
```

Package and platform-specific commands are in `packaging/windows/VERIFY.md`,
`packaging/linux/VERIFY.md` and `packaging/macos/VERIFY.md`. Cross-platform
acceptance must additionally create Full, Smart and Clean Restore fixtures with
both LZMA2 and zstd on each platform and restore each fixture on the other two
platforms. KnotLink is a release gate: run the strict protocol, command,
SDK-lifecycle and server-manager CTest targets on Windows, Ubuntu 24.04 and
macOS arm64. On Windows, additionally verify that an installed KnotLinkService
older than 3.2.0.0 triggers the startup update reminder, while 3.2.0.0 or newer
can be detected and started. Then exercise `PING`,
`GET_CAPABILITIES`, disable/re-enable, and restart. With mod 3.0.0 or newer,
verify handshake, hot backup, hot restore and rejoin events. Confirm that
`BACKUP 0 0`, `SEND ...` and `RESTORE_CURRENT ...` create no task.

The failure-path acceptance is equally important: no installed server,
unknown/old version, refused connection, remote disconnect, repeated stop and
application exit must remain bounded. A server that does not expose ports 6370
and 6378 within 10 seconds must leave MineBackup interactive and display the
failure reason.
