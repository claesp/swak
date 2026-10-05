# Development diagnostics

Run `tools/quality.py` when a change affects memory ownership, input handling, callbacks, networking, or repeated work. Run all modes before a release. Run the analysis and coverage modes regularly during development, including after plugin changes.

The runner uses Python 3.9 or later and its standard library. It saves logs and `result.json` under `build/quality/`. A failed test, missing tool, crash, or timeout returns a nonzero status. Each mode tests the current source in a separate development build.

Run modes that execute the application one at a time. Test caches use shared per-user paths, even across different build directories. Parallel runs can fail during cache locking.

Sanitizers are tools that detect faults during program execution. The sanitizer build detects use-after-free, invalid access, some undefined behavior, and leaks. Leak detection stays enabled. The runner does not suppress graphics-driver leaks or accept them as success. The release preset does not enable diagnostic instrumentation. Installation includes no diagnostic tool or test executable.

On Debian 13, install the development tools with administrator access:

```sh
sudo apt-get install python3 cppcheck gcc g++ cmake ninja-build qt6-base-dev xvfb xauth
```

Use the build environment described in [Build and run](build.md). Run these commands from the repository root. For a separate Qt installation, append `--qt-prefix /path/to/qt` to each command. Use `--jobs 2` to limit parallel compilation.

# Regular runs

The quick mode builds with address and undefined behavior sanitizers. It runs the vector, data, map, core, internal-event, logging, preferences, and short workload suites. Each suite has a 60-second limit. A separate process enforces the limit even when the application event loop stops.

```sh
python3 tools/quality.py quick
```

The GUI mode tests the native interface under Xvfb. It includes dock changes, repeated closure, rendering, and restored preferences. This mode has a three-minute test limit. The default EGL graphics path avoids the known Mesa GLX startup leak on this host. An explicit GLX override still exposes that leak.

```sh
python3 tools/quality.py gui
```

# Long sessions

A soak test repeats work in one process for a set duration. Run this mode for at least two hours before a release. The command below adds a short warmup and idle phase to the requested duration. The external watchdog allows another 60 seconds before it stops a stalled process.

```sh
python3 tools/quality.py soak --seconds 7200
```

The workload fills the event store to its limit, then exercises bounded parsing mutations, UDP bursts, event expiry, widget destruction, and preference writes. It uses temporary profiles and local sockets. It tests these resource limits after a two-second warmup:

- Resident memory growth stays below 64 MiB from the warmup baseline.
- Open file descriptors grow by no more than eight.
- The event loop responds within two seconds.
- The idle process consumes less than half of one CPU core over one second.

Resident memory means memory currently held in RAM. Allocator caches and sanitizer overhead affect this measurement. The threshold detects large growth, while LeakSanitizer detects unreleased allocations at exit. A smaller leak can pass a short run. Logs record cycles, peak growth, heartbeat delay, and idle CPU time. Compare reports on the same host before you change thresholds.

The default workload does not render the map. Add `--gui` to run the workload with the actual application window and GPU map under Xvfb. This mode also preserves leak failures from the graphics driver. Exercise the actual desktop for hours with representative traffic before release. Hardware graphics, Wayland, screen changes, and driver behavior require that desktop run.

# Coverage and dead code

Coverage records which code the tests execute. This mode requires GCC and a matching `gcov` version. It resets old counters, runs the suites, and produces `coverage.json` and `coverage.md`. Add `--gui` to include native interface paths.

```sh
python3 tools/quality.py coverage --gui
```

Read the unexecuted lines and functions in the report. Add tests for reachable success, error, cancellation, and shutdown paths. Remove unused code only after reference review and analysis. An unexecuted function is not proof that the function is dead. Qt callbacks and plugin entry points require particular care.

Record remaining gaps and reasons in the test report. Line and branch counts do not establish that every possible input or path works.

Static analysis examines source without executing it. This mode uses Cppcheck 2.17 or later with the application compilation database and Qt library descriptions. It examines unused functions, warnings, style, performance, and portability. It requests exhaustive branch analysis and fails when Cppcheck reports a diagnostic. Source analysis can produce false alarms and miss faults.

The runner replaces generated Qt `.moc` includes with empty files for source analysis. The compiler and runtime tests exercise that generated code. Cppcheck examines application and C++ development-tool sources, not tests or third-party headers. Compiler flags and application include paths remain in the analysis database. Each report retains that database for inspection.

```sh
python3 tools/quality.py analyze
```

Review each diagnostic before you change code. If a Qt callback requires an exception, use a narrow suppression with a source comment that explains the reason. Do not suppress whole defect classes. Run compiler warnings and coverage alongside analysis because no single tool finds all dead code or faults.

# Resilience rules

Recoverable input, file, preference, and network failures must leave the interface usable. Bound memory, queues, parsing work, and retry rates. Release resources on every error and shutdown path. Prevent exceptions from escaping Qt callbacks. Test failure recovery followed by a successful operation.

Do not continue after detected memory corruption. Do not catch fatal signals to hide crashes. The application cannot safely recover from C++ memory corruption or failing native libraries inside the same process. Features that require isolation from native crashes need a separate worker process and a restart policy. Current static plugins share the application process. This workflow improves fault detection but does not guarantee that the application cannot crash.

The compiler documents the [sanitizer and coverage flags](https://gcc.gnu.org/onlinedocs/gcc/Instrumentation-Options.html). Cppcheck documents its [analysis and suppression behavior](https://cppcheck.sourceforge.io/manual.html). Review Qt and dependency security advisories before a release.

# Data feed diagnostics

The data suite tests KML and KMZ decoding, malformed inputs, local HTTPS failures, cancellation, and repeated refreshes. It also tests polygon holes and date-line geometry. The GUI suite exercises management forms and draws downloaded points, lines, and polygons. The workload now decodes KML during each cycle.

For a downloaded research sample, run `QT_QPA_PLATFORM=offscreen build/debug/swak_data_tests --sample /path/to/sample.kml` from the repository root. This development-only command also accepts KMZ. It reads at most the download limit plus one byte and reports shape counts or a parsing error.

The vector suite tests MVT decoding, property types, polygon holes, malformed records, numeric bounds, background cancellation, source replacement, and decoder exceptions. The native GUI suite tests layer controls, persisted choices, feature inspection, and OpenGL rendering. The workload also decodes a vector tile on each cycle. To decode a downloaded sample, use `QT_QPA_PLATFORM=offscreen build/debug/swak_vector_tests --sample /path/to/tile.pbf`.

The vector suite includes four dense tiles with more than 200000 vertices in total. It tests complete rendering, cached panning, and image replacement after layer changes. For a local research sample, run `QT_QPA_PLATFORM=offscreen build/debug/swak_vector_tests --benchmark-sample /path/to/tile.pbf`. This development command compares direct painting with cached image composition for 16 tiles across ten pan frames. The comparison uses software painting, not hardware GPU timing.

The vector panning fix passed eight sanitizer suites, static analysis, and coverage with 33 native GUI cases. Reports are under `build/quality/20261002-172852-quick-141261`, `20261002-173021-analyze-142067`, and `20261002-172856-coverage-141297`. The image allocation failure path remains untested. The native sanitizer run passed all functional cases but failed on the existing Mesa leak. That report is `build/quality/20261002-173131-gui-142618`. Read [the handoff](HANDOFF.md) for timing evidence and limits.

The soak workload also creates local marker records, deletes them, and cycles through undo and redo. It bounds deletion history during repeated work.


The marker regression measures map paint events during 20 pointer moves. It permits at most two map repaints and requires a separate overlay signal. It also measures the black guide alpha value. Coordinate grid tests draw low and high zoom levels near the equator and both poles. These tests do not measure hardware cursor latency or native Wayland presentation.

The workload also creates polygon records, calculates their area, deletes them, and cycles through undo and redo. Polygon tests cover geometry limits, crossing edges, invalid edits, locale units, menu activation, and restart. Native tests compare preview opacity and inspect the OpenGL frame.

Selection rendering tests compare OpenGL stroke coverage with a screen-coordinate reference at zoom levels 2, 12, and 19. They cover solid polygons, dashed polygons, lines, and downloaded KML geometry. Summed pixel intensity allows small raster and OpenGL antialiasing differences. These tests also run at 200 percent display scaling during focused regression runs.


# CoT connection diagnostics

The servers suite adds local TCP and TLS harnesses. It tests XML fragmentation, malformed messages, multiple connections, timeout recovery, and burst traffic. It also tests certificate rejection, PEM and encrypted PKCS12 authentication, LAN discovery validation, and deterministic animation states. The full native suite receives, draws, selects, and transmits CoT through the server browser.

Use tools/cot_harness.py for manual and sustained traffic. Read [CoT connections](servers.md) for commands and transport configuration. The script uses only the Python standard library and stays outside the release installation. Public test credentials must never protect a real deployment.

A full TAK Server deployment and two physical LAN peers require separate integration tests. An isolated namespace test does not establish physical-network discovery or firewall behavior. It also does not establish interoperability across all authenticated TAK configurations.

Linux hosts with unprivileged user and network namespaces can run an isolated two-peer test. It creates two temporary dummy interfaces inside the namespace. It does not change host interfaces or firewall rules. The test exchanges CoT in both directions across independent Local LAN entries. It also tests entry removal, peer-table selection, listener port changes, reconnects, and disabling a peer.

```sh
python3 tools/test_lan.py --executable build/debug/swak_server_tests
python3 tools/quality.py quick --lan
python3 tools/quality.py coverage --gui --lan
```

The LAN test requires `unshare` and `ip`. A host policy that disables namespaces makes this diagnostic fail. The standard servers suite skips this integration case unless the isolated harness enables it.

For an actual TAK Server test, build and run swak_tak_compat. Read [TAK compatibility](tak-compatibility.md) for the profile format, safety limits, and local results. This test sends synthetic messages and requires a configured test server. It does not run as part of CTest.

# Emulator diagnostics

The simulation suite tests the separate CoT emulator. It covers deterministic routes, movement speed, stable identities, coordinate bounds, invalid configuration, TCP and UDP reception, TAK client mode, and interruption. The native GUI suite receives both teams and inspects the rendered map. Both suites run through the existing diagnostic modes.

The standard source report covers application and C++ development-tool code. It includes tools/cot_emulator.cpp and tools/cot_simulation.cpp through the debug compilation database. Do not interpret unused functions from a partial source scan as dead application code. Read the latest handoff for emulator load results and remaining paths without tests.

The workload now enables 128 trace identities. Each cycle adds 52 positions to one identity, then expires the source. This exercises bounded trace history, repeated clearing, and shutdown. Native tests inspect fading, the context action, preference restoration, TLS field dependencies, and explicit Information closure. Motion tests cover continuity, bounded prediction, and stale freeze.

The motion tests exercise pause, incoming position updates, new markers, immediate resume, expiry, and clearing. Native tests exercise the Space shortcut and Information Trace control. They also inspect CoT category descriptions and preserved unknown codes. The UDP workload allows two seconds for receive progress after each paced burst.

Circle and Line tests cover two-point geometry, world bounds, locale units, control editing, move cancellation, deletion, undo, and restart. Native tests inspect the OpenGL circle fill and temporary Distance expiry. The workload creates and deletes both saved shape types and exercises their undo records.

Label tests cover native clicks outside marker symbols, inverted text colors, visible vector labels, hidden labels, and label collisions. Trace tests compare frozen images during updates and changed images after resume. Identity tests cover generated UID persistence, locked input, unlocking, relocking, and a reopened dialog.

Menu tests cover locale ordering, direct plugin actions, provider toggles, and management action reuse. Native tests exercise wheel events at both overlay scroll limits. They inspect selected vector labels, read-only palettes, UID lock states, Team icon colors, and distinct OK and Apply icons.

# Source style

Read [Coding style](coding-style.md) for the project standard and formatter versions. Run python3 tools/quality.py style to report format differences without changing source. Run python3 tools/style.py --fix to apply the format. Review inserted braces and include changes before you commit.

The style pass covers first-party C++, Python, and CMake code. It excludes vendor code, generated files, and fixtures. The source-analysis pass also covers the C++ development tools. Formatter tests exercise file selection, version rejection, and command failure.

Marker scaling tests exercise all six percentages, point drawing, hit targets, selection outlines, saved values, and canceled edits. Native tests cover local markers and CoT symbols. Data tests cover imported points. Basemap selection keeps its original radius.

Predict tests cover known speeds, stationary positions, date-line crossing, high latitudes, invalid input, equal timestamps, bounded history, and enabled-target limits. Native tests exercise both activation controls, endpoint teleport fades, pause, resume, stale sources, clear, and restart. The workload enables 128 forecast targets and repeatedly feeds timestamped positions. Forecast accuracy against real routes remains unqualified.

Restricted Area tests cover fill and stripes, draft preview, cancellation, Apply, Undo, saved profiles, and absent or invalid legacy fields. Native screenshots cover the OpenGL pattern. Other object editors hide the checkbox.

Grid tests cover saved definitions, invalid values, catalog bounds, canceled edits, deletion, and legacy migration. Known coordinates cover Norway, Svalbard, the equator, and both polar systems. Rendering tests cover all angle formats, broad zone views, close views, and the date line. Native tests exercise tab switching, provider grouping, decoration visibility, and live resizing of existing markers.

The trace workload uses an explicit clock after timestamp mutation. It extends the synthetic expiry before it advances the trace. This keeps generated trace samples valid without weakening the application parser or store rules. Rejected samples report their exact error.

# Graphics leak regression

The application, native GUI tests, and soak executable call the same graphics startup helper before QApplication. The helper prefers EGL on X11 and preserves explicit backend choices. Leak detection stays enabled with no suppression file. The graphicsLifecycle test creates, renders, and destroys twenty QOpenGLWidget instances in one process. The full GUI suite also exercises real map lifetimes and screenshots.

After a sanitizer build, run the isolated native regression from the repository root:

```sh
QT_QPA_PLATFORM=xcb xvfb-run -a build/sanitize/swak_gui_tests graphicsLifecycle
```

For comparison, select GLX explicitly:

```sh
QT_QPA_PLATFORM=xcb QT_XCB_GL_INTEGRATION=xcb_glx xvfb-run -a build/sanitize/swak_gui_tests graphicsLifecycle
```

On the development host, the GLX path reports 240732 leaked bytes in 1073 allocations. An empty Qt application reports the same amount without a map widget. The extended allocation stack reaches Mesa through glXChooseFBConfig and the KDE platform theme during QApplication construction. These results identify a startup-path leak, not growth per map widget. The default EGL path avoids these allocations and passes leak detection.

Qt retains a fallback from EGL integration initialization to GLX. Its source defines this order in [QXcbConnection](https://github.com/qt/qtbase/blob/v6.4.3/src/plugins/platforms/xcb/qxcbconnection.cpp#L1090-L1121). Hardware graphics, Wayland, and fallback behavior on systems without EGL require separate host testing. Do not disable leak detection when you test those systems.

The workload waits for 500 milliseconds without model updates, animation updates, or rendered frames before its idle CPU measurement. This wait has a twenty-second limit. A continuously active map fails the wait. The default stale-marker fade and gray retention last about 11 seconds. This activity no longer counts as idle work. The CPU threshold remains unchanged.
