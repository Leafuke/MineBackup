# Memory and UI performance

This document defines the repeatable Windows measurement protocol used for UI
memory work. The measurements are diagnostic data, not release gates.

## Metrics

- **Private Bytes** measures committed memory that cannot be shared with other
  processes. It is the primary metric for retained application allocations.
- **Private working set** measures the private pages currently resident in RAM.
- **Working set** includes both private and shareable resident pages. Driver and
  mapped-image pages can make this substantially larger than Private Bytes.
- Thread and handle counts help reveal lifecycle leaks that memory totals alone
  can hide.

Do not use a one-off Task Manager value as a pass/fail signal. Record all three
memory metrics and use the median of five samples after the application reaches
a stable state.

## Reproduction protocol

1. Build and run the same Windows x64 Release preset for both revisions.
2. Use the same disposable profile and disable update checks and KnotLink so
   network timing does not affect the comparison.
3. At each checkpoint, wait for visible work to settle, then collect five
   samples one second apart:

   ```powershell
   pwsh -File tools/perf/Sample-WindowsProcessMemory.ps1 `
     -ProcessId <pid> -Samples 5 -Label main-window `
     -CsvPath build/perf/main-window.csv
   ```

4. Record these checkpoints in order: cold startup, main window, settings,
   history, pages closed, tray at 5 seconds, tray at 15 seconds, and the first
   cold restore.
5. Repeat the open-pages/close-pages/tray-cooldown/restore sequence 20 times.
   Check for crashes, steadily increasing memory, and growth in thread or
   handle counts.

Use one profile per measured revision. Opening a page may legitimately create
cached data, glyphs, or driver resources, so compare the stable post-close and
cold-tray states as well as the peak.

## Result fields

For every checkpoint record the five-sample medians plus the following runtime
diagnostics when available:

- mapped font bytes;
- font atlas dimensions and texture format;
- active ImGui viewport count;
- desktop UI session rebuild duration.

## Measured results (2026-08-02)

The measurements below use the Windows x64 MSVC Release build, the same bundled
`msyh.ttc`, and disposable profiles with update checks, KnotLink, and automatic
world scanning disabled. The baseline is revision `c650c95`; the optimized
revision is `6c81ed8`. Each table value is the median of five one-second samples.

| Revision and checkpoint | Private Bytes (MiB) | Private working set (MiB) | Working set (MiB) | Threads | Handles |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline, main window | 172.89 | 148.31 | 192.00 | 13 | 394 |
| Optimized, main window | 151.83 | 127.47 | 171.04 | 13 | 391 |
| Optimized, tray at 5 seconds (warm) | 151.83 | 127.47 | 171.07 | 13 | 391 |
| Baseline, tray at 15 seconds | 172.89 | 148.31 | 192.04 | 13 | 394 |
| Optimized, tray at 15 seconds (cold) | 83.78 | 58.25 | 101.46 | 13 | 376 |
| Optimized, first cold restore | 148.56 | 124.01 | 167.71 | 15 | 392 |
| Baseline, silent tray startup | 163.70 | 139.31 | 175.49 | 13 | 303 |
| Optimized, silent tray startup (cold) | 5.06 | 3.89 | 20.30 | 10 | 235 |

The controlled comparison shows:

- the visible main-window Private Bytes median fell by 21.06 MiB (12.18%);
- the 15-second tray Private Bytes median fell by 89.11 MiB (51.54%);
- unloading the optimized visible session releases 68.05 MiB (44.82%) of
  Private Bytes and 69.22 MiB (54.30%) of private working set;
- silent tray startup Private Bytes fell by 158.64 MiB (96.91%) because no
  GLFW window, OpenGL context, or ImGui context is created until activation.

The unload diagnostic reported a 19,704,352-byte mapped font, a 512x256 RGBA32
atlas, and one active viewport. The first measured cold rebuild took 13.24 ms.
The font mapping removes the font file from private heap ownership, but the
read-only mapped pages can still appear in total working-set measurements.

## Validation

- Both Windows Release presets completed their full test suites: 9/9 tests for
  `windows-msvc-x64-release` and 9/9 for
  `windows-msvc-x64-no-v15-release`.
- Automated tests cover mapped-file success and failure, move and repeated-close
  behavior, ImGui/context lifetime ordering, history filtering and invalidation,
  deletion-safe indices, and the exact 9.9/10-second UI lifecycle boundary.
- A real Windows smoke run verified warm hide, cold unload, single-instance cold
  activation, silent cold startup, restore, and tray exit.
- A 20-cycle cold-unload/restore run completed 20/20 cycles. Cold handle count
  changed from 376 to 372 and restored handle count from 392 to 388. Restored
  Private Bytes changed from 169.86 MiB to 155.18 MiB, with no monotonic resource
  growth observed. Cold Private Bytes was 141.30 MiB on the first cycle and
  146.67 MiB on the last; allocator/driver caching means individual samples need
  not return to an identical value.

Settings/history checkpoint measurements and the multi-monitor DPI, viewport
drag-out/drag-back, Chinese/font-icon, and page-state checks remain manual
release-regression items. They are deliberately not claimed as automated
coverage here.

## Remaining baseline and scope

The visible application still retains background services, process/runtime
state, the OpenGL driver, and the live UI session. The 83.78 MiB cold-tray value
likewise includes background subsystems that must continue running. These are
diagnostic results rather than fixed merge thresholds; compare trends using the
same profile and build conditions.

This work does not call `EmptyWorkingSet`, migrate the Windows renderer to
D3D11, or maintain a local Alpha8 fork of the ImGui OpenGL backend. Multi-
viewports, docking, dynamic DPI fonts, background backups, tray commands,
notifications, and global hotkeys remain supported while the UI session is
cold.

## References

- [Dear ImGui font documentation](https://github.com/ocornut/imgui/blob/master/docs/FONTS.md)
- [Microsoft memory footprint optimization](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/memory-footprint-optimization)
