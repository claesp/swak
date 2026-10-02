# Development diagnostics

Run `tools/quality.py` when a change affects memory ownership, input handling, callbacks, networking, or repeated work. Run all modes before a release. Run the analysis and coverage modes regularly during development, including after plugin changes.

The runner uses Python 3.9 or later and its standard library. It saves logs and `result.json` under `build/quality/`. A failed test, missing tool, crash, or timeout returns a nonzero status. Each mode tests the current source in a separate development build. Do not run simultaneous modes that use the same build directory.

Sanitizers are tools that detect faults during program execution. The sanitizer build detects use-after-free, invalid access, some undefined behavior, and leaks. Leak detection stays enabled. The runner does not suppress graphics-driver leaks or accept them as success. The release preset does not enable diagnostic instrumentation. No diagnostic tool or test executable is installed with the application.

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

The GUI mode tests the native interface under Xvfb. It includes dock changes, repeated closure, rendering, and restored preferences. This mode has a three-minute test limit. The known Mesa leak on this host causes a failure and remains visible in the log.

```sh
python3 tools/quality.py gui
```

# Long sessions

A soak test repeats work in one process for a set duration. Run this mode for at least two hours before a release. The command below adds a short warmup and idle phase to the requested duration. The external watchdog allows another 30 seconds before it stops a stalled process.

```sh
python3 tools/quality.py soak --seconds 7200
```

The workload fills the event store to its limit, then exercises bounded parsing mutations, UDP bursts, event expiry, widget destruction, and preference writes. It uses temporary profiles and local sockets. It tests these resource limits after a two-second warmup:

- Resident memory growth stays below 64 MiB from the warmup baseline.
- Open file descriptors grow by no more than eight.
- The event loop responds within two seconds.
- The idle receiver consumes less than half of one CPU core over one second.

Resident memory means memory currently held in RAM. Allocator caches and sanitizer overhead affect this measurement. The threshold detects large growth, while LeakSanitizer detects unreleased allocations at exit. A smaller leak can pass a short run. Logs record cycles, peak growth, heartbeat delay, and idle CPU time. Compare reports on the same host before changing thresholds.

The default workload does not render the map. Add `--gui` to run the workload with the actual application window and GPU map under Xvfb. This mode also preserves leak failures from the graphics driver. Exercise the actual desktop for hours with representative traffic before release. Hardware graphics, Wayland, screen changes, and driver behavior require that desktop run.

# Coverage and dead code

Coverage records which code the tests execute. This mode requires GCC and a matching `gcov` version. It resets old counters, runs the suites, and produces `coverage.json` and `coverage.md`. Add `--gui` to include native interface paths.

```sh
python3 tools/quality.py coverage --gui
```

Read the unexecuted lines and functions in the report. Add tests for reachable success, error, cancellation, and shutdown paths. Remove unused code only after reference review and analysis. An unexecuted function is not proof that the function is dead. Qt callbacks and plugin entry points require particular care. Record remaining gaps and reasons in the test report. Line and branch counts do not establish that every possible input or path works.

Static analysis examines source without executing it. This mode uses Cppcheck 2.17 or later with the application compilation database and Qt library descriptions. It examines unused functions, warnings, style, performance, and portability. It requests exhaustive branch analysis and fails when Cppcheck reports a diagnostic. Source analysis can produce false alarms and miss faults.

The runner replaces generated Qt `.moc` includes with empty files for source analysis. The compiler and runtime tests exercise that generated code. Cppcheck examines application sources, not tests or third-party headers. Compiler flags and application include paths remain in the analysis database. Each report retains that database for inspection.

```sh
python3 tools/quality.py analyze
```

Review each diagnostic before changing code. If a Qt callback requires an exception, use a narrow suppression with a source comment that explains the reason. Do not suppress whole defect classes. Run compiler warnings and coverage alongside analysis because no single tool finds all dead code or faults.

# Resilience rules

Recoverable input, file, preference, and network failures must leave the interface usable. Bound memory, queues, parsing work, and retry rates. Release resources on every error and shutdown path. Prevent exceptions from escaping Qt callbacks. Test failure recovery followed by a successful operation.

Do not continue after detected memory corruption or catch fatal signals to hide crashes. C++ memory corruption and failing native libraries cannot be safely recovered inside the same process. Features that require isolation from native crashes need a separate worker process and a restart policy. Current static plugins share the application process. This workflow improves fault detection but does not guarantee that the application cannot crash.

The compiler documents the [sanitizer and coverage flags](https://gcc.gnu.org/onlinedocs/gcc/Instrumentation-Options.html). Cppcheck documents its [analysis and suppression behavior](https://cppcheck.sourceforge.io/manual.html). Review Qt and dependency security advisories before a release.

# Data feed diagnostics

The data suite tests KML and KMZ decoding, malformed inputs, local HTTPS failures, cancellation, and repeated refreshes. It also tests polygon holes and date-line geometry. The GUI suite exercises management forms and draws downloaded points, lines, and polygons. The workload now decodes KML during each cycle.

For a downloaded research sample, run `QT_QPA_PLATFORM=offscreen build/debug/swak_data_tests --sample /path/to/sample.kml` from the repository root. This development-only command also accepts KMZ. It reads at most the download limit plus one byte and reports shape counts or a parsing error.

The vector suite tests MVT decoding, property types, polygon holes, malformed records, numeric bounds, background cancellation, source replacement, and decoder exceptions. The native GUI suite tests layer controls, persisted choices, feature inspection, and OpenGL rendering. The workload also decodes a vector tile on each cycle. A downloaded sample can be decoded with `QT_QPA_PLATFORM=offscreen build/debug/swak_vector_tests --sample /path/to/tile.pbf`.

The vector suite includes four dense tiles with more than 200000 vertices in total. It tests complete rendering, cached panning, and image replacement after layer changes. For a local research sample, run `QT_QPA_PLATFORM=offscreen build/debug/swak_vector_tests --benchmark-sample /path/to/tile.pbf`. This development command compares direct painting with cached image composition for 16 tiles across ten pan frames. The comparison uses software painting, not hardware GPU timing.

The vector panning fix passed eight sanitizer suites, static analysis, and coverage with 33 native GUI cases. Reports are under `build/quality/20261002-172852-quick-141261`, `20261002-173021-analyze-142067`, and `20261002-172856-coverage-141297`. The image allocation failure path remains untested. The native sanitizer run passed all functional cases but failed on the existing Mesa leak. That report is `build/quality/20261002-173131-gui-142618`. Read [the handoff](HANDOFF.md) for timing evidence and limits.
