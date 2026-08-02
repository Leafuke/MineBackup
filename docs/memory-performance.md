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

The results section is populated after the implementation and its Windows
regression run. D3D11 migration and a locally maintained Alpha8 OpenGL backend
are intentionally outside this work.
