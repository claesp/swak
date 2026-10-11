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

The GUI runs covered normal and 200 percent display scaling. Captured images were inspected for map content and pane layout. The tests used the desktop theme and an explicit light palette.

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

Uncovered areas include application startup, file-dialog interaction, several map input handlers, preference-directory failures, and short-read failures. Native hardware and Wayland runs remain outstanding. No multi-hour run occurred in this session. Run the documented two-hour workload and desktop tests before you claim long-session stability.

The release preset built successfully with tests, sanitizers, and coverage disabled. Its compiler commands contain no sanitizer or coverage flags. Its executable does not link sanitizer or Qt Test libraries. Python, Cppcheck, and coverage tools remain development-only dependencies.

Run instructions and report locations are in [Development diagnostics](quality.md). Reports remain under the ignored `build/quality/` directory. The source repository stores the workflow and this dated evidence, not machine-specific reports.

# Settings and logging

On October 2, 2026, debug and sanitizer suites passed the logging, core, preferences, and workload tests. The logging tests cover silent defaults, explicit enablement, severity, urgency, category filtering, rate limits, escaped output, and concurrent calls. Separate child processes test fatal diagnostics and unavailable output. Closed pipes, full pipes, closed sockets, and active sockets passed. Fatal Qt diagnostics terminate even when their text is disabled.

All 18 native GUI cases passed under Xvfb with Qt 6.8.2 at normal and 200 percent scaling. Added cases cover Apply, Cancel, validation before application, saved configuration, and visible save failures. A test plugin adds a section through the new interface. Its values survive plugin removal and return. Other cases cover unchanged numeric precision, column order validation, and hidden column widths.

The release startup test used a fresh temporary profile with `QT_DEBUG_PLUGINS=1`. Both stdout and stderr remained empty during the two-second run. The supervisor then stopped the process. Static analysis passed with Cppcheck 2.17.1.

The native sanitizer GUI cases passed their assertions. LeakSanitizer still reported the known 240732-byte Mesa leak in 1073 allocations, and the runner retained the failure. The logging handler runs only in the application and logging probes. It does not suppress test framework or sanitizer reports.

The review located the mouse capture warning in Qt 6.8 Wayland source. It did not reproduce the exact triggering gesture in a native Wayland session. Hardware graphics, native Wayland placement, and Qt 6.4 remain untested.

# Coverage review, October 11

Coverage with the native GUI suite reached 22759 of 24296 source lines (93.7 percent) and 2354 of 2456 functions (95.8 percent). The report is `build/quality/20261011-000628-coverage-3234727`. An earlier report counted four removed plugins at 0 percent, because their old objects stayed in the build tree. The runner now skips sources that no longer exist.

New GUI tests run the Checklists, Reports, Missions, and Files panes against a local HTTPS server. They list, open, start, update, subscribe, download, delete, upload, and export, and they check server errors and cancelled dialogs. The shared server is `tests/https_harness.h`. The tests use Qt file dialogs, because a desktop theme can supply native dialogs that a test cannot control. The Reports test found a defect: every attachment failed, because the file reader accepted at most 1 MiB, and attachments need 16 MiB. The QUIC test now also connects with a PEM certificate and a plain or encrypted PEM key, and it checks a wrong key password.

The largest remaining gaps:

- `core/servers.cpp`: LAN discovery and peers. The `--lan` integration mode runs this code. This review did not run it.
- `core/server_browser.cpp`, `app/window.cpp`, and the drawing plugins: mouse and keyboard paths of the map and the editors.
- `core/tiles.cpp`: allocation failures and some cache recovery branches.
- `app/main.cpp`: application startup. No automated test starts the executable. Earlier sessions started the installed executable by hand.
- `plugins/gdal_vector.cpp` and `plugins/gdal_raster.cpp`: GDAL error branches for damaged files.
- `core/tak_enrollment.cpp`: OpenSSL failures during key and request creation.

The Python tests now cover the command helpers of `tools/tak_server.py`: failed commands, Podman lookups, password hashing, packet loss rules, probe runs, and argument errors. Container start functions run only against a live deployment. `tools/quality.py` mode orchestration runs only in real diagnostic runs.

# Remaining gaps, October 11

Coverage with the native GUI suite and the LAN mode reached 23195 of 24345 source lines (95.3 percent) and 2383 of 2462 functions (96.8 percent). The report is `build/quality/20261011-010029-coverage-3413530`.

New tests:

- `tests/test_startup.py` starts the executable. It checks `--version`, `--help`, and a wrong option. Without OpenGL, it stops the startup error message with SIGTERM and SIGINT. With `--gui`, it opens the window and expects status 0 after each signal. `app/main.cpp` now runs completely.
- `quitSignals` checks the new signal handler, its single instance, and the restored handlers.
- `receiverPolicy` sends control messages, stale, distant, future, early, invalid, and older events to a UDP input.
- `connectionDiagnostics` also checks an unknown host and a refused broadcast address.
- `enrollmentFailures` checks a short RSA key, an invalid port, failed configuration and signing steps, invalid settings, and a certificate folder that cannot be made.
- `vectorLimits` checks degenerate shapes, wrapped lines, nested collections, the shape and coordinate limits, cancellation, archive limits, and local KML and KMZ files.
- The GDAL raster test also sends basic and header credentials, and checks the zoom range and an invalid WMTS entry.
- The session map test checks the bounds of a narrow view and a failed attribution request.
- GUI tests cover editor import and export, the Channels dialog, the server table, the Identity and Servers dialogs, pane settings, restored pane places, and the step keys.

Paths that remain untested:

- The default socket error text. Qt reports the errors that a local test can cause with named error codes.
- Archive entry names with `..` or backslashes. GDAL removes or changes these names before SWAK checks them, so the check is a second defense.
- After a successful enrollment, a failed permission change of a written file, and an entry without a CA that receives the server CA. The second path needs a server certificate that the system trusts.
- LAN peer rows in the Local LAN pane. Only the namespace test makes LAN peers, and it runs without the GUI.
- Some mouse paths of the drawing tools, and legacy route preferences with four coordinate fields.

# Image allocation faults

`core/image_allocation.*` creates the vector and raster tile images. Tests call `setImageFaultForTests` to make it return a null image or throw `std::bad_alloc`. `tests/image_fault.h` ends the fault when the test scope ends, also after a failed check.

- `imageAllocation` checks the hook itself.
- `imageAllocationFailure` in the vector tests expects "Vector tile image allocation failed." and "Vector tile preparation failed.", and then a normal render.
- `gdalRasterFiles` expects "Map tile image allocation failed." from the provider and the renderer, and "Map tile rendering failed." when the worker catches the exception. The same dataset then renders again, so the dataset pool and the GDAL options survive the exception.

A check with the null image fault turned off made both renderer tests fail, so the tests depend on the fault.

# OpenSSL allocation faults

`swak_openssl_fault_tests` replaces the OpenSSL memory functions before the first OpenSSL allocation, so it has its own executable. It makes allocation N fail and runs an enrollment function for each N until a run needs fewer allocations. It also replaces the random source with a seeded generator. Key generation and signatures then repeat exactly, so every successful run must give exactly the reference result. Key generation runs are long, so that sweep tests a sample of each phase.

The sweeps cover key generation, key reading, certificate requests, and enrollment replies. Each failure must give one of the documented messages. A sanitizer build checks the failure paths for leaks when the test ends. The sweep found two faults:

- Key reading reported "Use an RSA key with at least 2048 bits" for a valid key when OpenSSL could not read the key size. It now reports that the key cannot be read.
- OpenSSL 3.5.7 aborts when the allocation at `crypto/asn1/tasn_new.c:299` fails during `X509_REQ_sign`. A sweep with one child process for each allocation located the crash. The test does not fail allocations at that line, and it reports how many it kept. Read [Dependencies](dependencies.md).

An allocation failure cannot cause the request encoding error, so a test sends a request above 64 KiB. The second encoding call now has a check too. No allocation failure reaches that check, so it stays untested.

`enrollmentFailures` also checks a subject field that OpenSSL refuses, a refused or dropped signing request, a reply without a certificate, a directory in place of the key, certificate, or CA file, and a certificate path with a control character. The entry stays unchanged after each failure. The last run then enrolls in a usable folder.

