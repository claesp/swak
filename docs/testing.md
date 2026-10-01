# Test report

This report records the initial implementation on October 1, 2026. The test host ran Debian 13 on x86-64 with GCC 14.2 and Qt 6.8.2. CMake 3.31.6 generated Ninja 1.12.1 builds.

The host lacked development packages and administrator access. The build used Debian packages extracted under `/tmp/swak-toolchain`. The repository contains no downloaded libraries or machine-specific tool paths.

Debug and release builds succeeded with the selected compiler warnings. The core suite passed in debug, release, and sanitizer builds. AddressSanitizer and UndefinedBehaviorSanitizer reported no errors in those test runs.

The core suite covers these behaviors:

- XML round trips and escaped Unicode text.
- Malformed messages, size limits, nesting, declarations, coordinates, and timestamps.
- Truncated messages and a bounded corpus of null-byte mutations.
- Event replacement, expiry, capacity, selection, and model behavior.
- Projection round trips and polar coordinate handling.
- Regular files, oversized files, directories, missing files, and pipes.
- UDP bind failures, invalid addresses, reception, oversized datagrams, restart, and closure.

The GUI suite ran in Xvfb with Mesa llvmpipe. LlvmPipe renders OpenGL through the CPU when hardware acceleration is unavailable. The tests exercised generation, replacement, selected-event updates, map keys, resizing, and window closure.

The GUI runs covered normal and 200 percent display scaling. Captured images were inspected for map content and panel layout. The tests used the desktop theme and an explicit light palette.

Installation into a temporary prefix succeeded. The installed executable reported version 0.1.0 using system Qt libraries without a build-directory runtime path. It also opened in Xvfb and remained active until the startup test terminated it.

Hardware GPU execution and native Wayland behavior remain untested. The minimum Qt 6.4 baseline and non-x86 architectures also remain untested. File-dialog interaction, screen-reader behavior, and independent TAK interoperability require further desktop tests.

The deterministic parser corpus is not a sustained fuzzing campaign. The tests do not establish complete CoT schema compatibility or resistance to every resource attack. The security limits and supported subset are documented in [Security boundaries](security.md).

# Independent panes

On October 2, 2026, the GUI suite passed with separate Events, Event editor, UDP receiver, and Plugins docks. Each dock passed two detach, resize, close, reopen, and redock cycles. The Events test also resized the detached window through a mouse drag on its corner handle.

The tests cover the four View menu entries and File > Quit. Native rendering passed with the desktop theme and a light palette on Qt 6.8.2 under Xvfb. This run used software OpenGL and did not test native Wayland window decorations.

# Persistent preferences

On October 2, 2026, debug, release, and sanitizer builds passed the core and preferences suites. The native GUI suite passed 13 cases at normal and 200 percent display scaling. Tests used separate temporary profile files and did not change the user profile.

The added cases cover these behaviors:

- Main window geometry, maximized state, pane layout, active tabs, and floating sizes across restarts.
- Map position, editor drafts, receiver endpoints, and event table columns across restarts.
- Automatic saves before application closure.
- Removal and return of receiver, map, and codec plugins.
- Retention of unknown fields while plugins are absent.
- Receiver restart, port conflicts, and cancellation of a saved startup request.
- Recovery of off-screen windows and invalid individual values.
- Preservation of malformed files and unsupported file versions.
- Private file permissions, storage limits, write failures, and concurrent writer exclusion.

All 13 GUI cases also passed under address and undefined behavior sanitizers. LeakSanitizer then reported 240732 bytes in 1073 allocations from `libGLX_mesa.so.0`, so that process returned a failure status. The unchanged baseline at `362a661` produced the same allocation count, byte count, and library stack locations.

This comparison identifies a pre-existing graphics-stack issue rather than a new preferences regression. No leak suppression was added. Native Wayland placement and hardware GPU behavior remain untested.

# Development diagnostics

On October 2, 2026, the diagnostic runner passed its tests for success, failure, infinite-loop termination, and coverage aggregation. The sanitizer quick mode passed the core, preferences, and workload suites. Cppcheck 2.17.1 passed with Qt descriptions and exhaustive branch analysis. The review removed an unused window constructor and resolved the reported source issues. Narrow comments explain value-return and test-access exceptions.

A 60-second headless workload passed under address and undefined behavior sanitizers with leak detection enabled. It completed 1279 cycles. Peak resident memory grew by 8052736 bytes after warmup. The longest event-loop gap was 57 milliseconds. Idle CPU time was 0.003 seconds over one second. The file descriptor bound passed.

A ten-second native workload passed its behavior and resource assertions under Xvfb with Mesa llvmpipe. It completed 246 cycles with 9113600 bytes of peak growth, a 45-millisecond maximum gap, and 0.032 seconds of idle CPU time. LeakSanitizer then reported the known 240732-byte Mesa leak in 1073 allocations. The runner returned status 1 and preserved the underlying status 23 in its diagnostic message. No suppression was added.

The coverage mode passed the core, preferences, workload, and 13 native GUI cases. GCC 14.2 recorded 948 of 1081 source lines and 137 of 153 functions as executed. It recorded 1498 of 3104 compiler branches as executed. Branch totals include generated exception paths within source expressions. These figures are evidence of gaps, not a claim of complete path coverage.

Uncovered areas include application startup, file-dialog interaction, several map input handlers, preference-directory failures, and short-read failures. Native hardware and Wayland runs remain outstanding. No multi-hour run occurred in this session. Run the documented two-hour workload and desktop tests before claiming long-session stability.

The release preset built successfully with tests, sanitizers, and coverage disabled. Its compiler commands contain no sanitizer or coverage flags. Its executable does not link sanitizer or Qt Test libraries. Python, Cppcheck, and coverage tools remain development-only dependencies.

Run instructions and report locations are in [Development diagnostics](quality.md). Reports remain under the ignored `build/quality/` directory. The source repository stores the workflow and this dated evidence, not machine-specific reports.

# Settings and logging

On October 2, 2026, debug and sanitizer suites passed the logging, core, preferences, and workload tests. The logging tests cover silent defaults, explicit enablement, severity, urgency, category filtering, rate limits, escaped output, and concurrent calls. Separate child processes test fatal diagnostics and unavailable output. Closed pipes, full pipes, closed sockets, and active sockets passed. Fatal Qt diagnostics terminate even when their text is disabled.

All 18 native GUI cases passed under Xvfb with Qt 6.8.2 at normal and 200 percent scaling. Added cases cover Apply, Cancel, validation before application, saved configuration, and visible save failures. A test plugin adds a section through the new interface. Its values survive plugin removal and return. Other cases cover unchanged numeric precision, column order validation, and hidden column widths.

The release startup test used a fresh temporary profile with `QT_DEBUG_PLUGINS=1`. Both stdout and stderr remained empty during the two-second run. The supervisor then stopped the process. Static analysis passed with Cppcheck 2.17.1.

The native sanitizer GUI cases passed their assertions. LeakSanitizer still reported the known 240732-byte Mesa leak in 1073 allocations, and the runner retained the failure. The logging handler runs only in the application and logging probes. It does not suppress test framework or sanitizer reports.

The warning about mouse capture was located in Qt 6.8 Wayland source. Its exact triggering gesture was not reproduced in a native Wayland session. Hardware graphics, native Wayland placement, and Qt 6.4 remain untested.
