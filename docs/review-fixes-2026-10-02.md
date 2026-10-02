# Review fixes

This update addresses the eight P2 findings and fifteen P3 findings in the two October 2 reviews. The original reviews remain historical records. No dependency or plugin interface version changes.

## Behavior changes

- Selected events clear on expiry without selecting a neighbor. The same table row can be selected again after the map clears selection.
- Coordinates sort numerically. Internal events sort by actual timestamps and numeric severity or urgency. Display text remains localized.
- Invalid map entries remain saved and disabled. Other valid providers remain usable. Provider forms report actual write failures.
- Read-only profiles skip automatic saves. Persistent save errors produce one diagnostic until the error changes or a save succeeds. Failed writes retry after 30 seconds.
- Lock permission errors permit read-only profile use. Stale locks from another host use the Qt timeout. Active profiles require local storage.
- Receiver summaries have a one-second interval. Clean batches clear old pane errors. Old rejections do not make later clean summaries warnings.
- Failed tiles have individual, bounded retry delays. Other tiles continue to load. Rate limits, service outages, and certificate failures pause the provider.
- View changes cancel obsolete requests during a pause. Expired images remain visible during replacement. Uncached images remain only in the current view.
- URL-only attribution has a visible link. Additional URLs have separate links. Trailing punctuation does not become part of a license URL.
- Pane menu marks follow restored visibility. Changes within the same dock area preserve tabs. Detached positions use screen coordinates.
- WMS and WMTS queries encode plus signs. XML namespace declarations count toward the attribute limit. Manifest patterns reject trailing newlines.
- Internal events preserve printable Unicode characters outside the Basic Multilingual Plane. Regular-file logging shares the standard-error file offset.
- Installation includes only user guides. Optimized builds enable `_FORTIFY_SOURCE=2` unless the toolchain supplies another value.

The URL-only label says Map attribution. This preserves the earlier requirement to keep raw URLs out of the visible attribution text.

## Regression coverage

New tests exercise the reported selection, sort, provider recovery, failed saves, and diagnostic flooding paths. They also exercise mixed failed and successful tiles, cancellation during a service pause, and stale-image rendering.

Additional tests cover read-only directories, copied stale locks, shared log offsets, Unicode, query encoding, namespace limits, manifest anchors, and restored pane menus. Native tests cover provider forms, attribution links, and tab preservation. `QAbstractItemModelTester` exercises the event model during expiry.

The first stale-lock fixture failed because its final buffered write changed the test timestamp. The fixture now flushes before it sets the timestamp. The corrected test passes. Static analysis also identified a loop and a shadowed name during development. Both findings are resolved.

## Test environment

The test host uses Debian 13, x86-64, GCC 14.2, Qt 6.8.2, CMake, and Ninja. Native tests use Xvfb and Mesa llvmpipe. No test contacts a public map service.

The final static analysis passed in `build/quality/20261002-141154-analyze-2`. The final six sanitizer suites passed in `build/quality/20261002-141535-quick-106615`. Sanitizers test memory access, undefined behavior, and leaks in those suites.

The final coverage run passed all six suites and all 29 native GUI cases. Its report is `build/quality/20261002-141801-coverage-107224`. All executable lines in the tile renderer and map catalog ran. The final test also exercised repeated write failures followed by recovery.

The native sanitizer run passed 28 GUI cases. It then reported the existing Mesa leak of 240732 bytes in 1073 allocations. The report is `build/quality/20261002-141057-gui-104797`. This remains a failed leak diagnostic, not a clean native memory result.

The provider, pane, and selection cases passed at 200 percent scaling. Their log is `build/review-fixes/hidpi/gui.log`. The normal native run and its images are under `build/review-fixes/`.

The 60-second sanitizer workload passed in `build/quality/20261002-141652-soak-107104`. It completed 956 cycles. Peak growth after warmup was 8228864 bytes. The maximum heartbeat gap was 90 milliseconds. Idle CPU time was 0.007 seconds.

The release build passed without compiler warnings. Its executable remains `build/release/swak`, version 0.1.0. The preset disables tests, sanitizers, and coverage. A preprocessor probe reported `_FORTIFY_SOURCE=2` and `__USE_FORTIFY_LEVEL=2`.

Installation produced the executable, desktop entry, icon, and two user guides. An installed startup test saved an isolated profile without text output. An external three-second timeout ended that test. The installation record is `build/review-fixes/install.json`.

## Remaining limits

The review observations about logical-pixel tile resolution and synchronous disk-cache access remain design limits. This update does not add higher-resolution provider requests or a worker for the disk cache.

Native Wayland, hardware GPUs, Qt 6.4, other architectures, and multi-hour sessions remain untested. A one-minute workload does not establish leak-free operation over hours. The Mesa diagnostic remains unresolved.

Startup failures, native file dialogs, some map mouse controls, and several defensive branches remain outside executed coverage. Executed lines do not prove complete input or branch coverage. No certification claim applies to this work.
