# Session handoff

Latest implementation, October 4, 2026: Read “Menus, provider controls, overlay scrolling, and Identity presentation” at the end of this file. Earlier sections record historical states. Use Git for the current source revision.

Previous implementation: [Review fixes, October 2](review-fixes-2026-10-02.md). This update addresses the findings in both October 2 reviews. It rebuilds the release and adds regression tests. The review records below describe the earlier state.

Latest review: [Claude Code Qt skill review, October 2](review-2026-10-02-claude.md). It adds five P2 defects in expiring selections, preference save errors, receiver logging, failed tiles, and URL-only attribution. It also lists P3 findings. The three defects in the [earlier review](review-2026-10-02.md) remain open. Application code remains at `2d904ab`. Later sections below contain historical implementation snapshots.

This handoff records the project state on October 2, 2026. The repository is `/home/clpe/code/swak`, and the branch is `main`.

The repository root contains `AGENTS.md` as the entry point for agents that support automatic instruction discovery. It directs agents to this handoff. Agents without that support need an explicit instruction to read these files.

The current implementation includes persistent preferences and independent dock panes. Read `git log -5 --oneline` for the current commit identifiers. The initial application commit is `4f82422`. No requested implementation work remains pending at this point.

# User requirements

The user wants a native Linux application for receiving, generating, and presenting Cursor-on-Target data. The application uses Qt and modern C++, with Ninja for builds. Features use plugin interfaces where practical, starting with a GPU map viewer.

Preserve these development requirements:

- Use the `qt-linux-gui` skill for development.
- Use the `simple-english` skill for Git commits, code comments, and documentation.
- Keep documentation under `docs/`.
- Commit all functional changes with Git.
- Keep the application small, fast, and secure.
- Add external dependencies only with a documented reason.

The Qt skill is at `/home/clpe/.codex/skills/qt-linux-gui/SKILL.md` for Codex. Claude Code uses an adapted copy at `/home/clpe/.claude/skills/qt-linux-gui/SKILL.md`. That copy omits the Codex interface file and adds Claude Code guidance for native GUI inspection. The Simple English skill was supplied in the conversation and is also available through the installed plugin. Claude Code does not have the Simple English skill. Use the skill locations available in the next session if plugin paths change.

# Current implementation

The stack is C++20, Qt 6 Widgets, OpenGL, CMake, and Ninja. The declared minimum versions are Qt 6.4 and CMake 3.24. Qt is the only direct application dependency.

Six statically linked Qt plugins implement five interface types in `src/core/interfaces.h`. Static linkage places the plugins inside the executable. The application does not search directories for external application plugins.

The main implementation files are:

- `src/plugins/cot.cpp`: Bounded CoT XML decoding and encoding.
- `src/plugins/map.cpp`: OpenGL map widget and input controls.
- `src/plugins/basemap.cpp`: Offline schematic outlines and coordinate grid.
- `src/plugins/tracks.cpp`: Event markers and table.
- `src/plugins/udp.cpp`: Optional UDP receiver with bounded polling.
- `src/plugins/editor.cpp`: Event generation, XML preview, file import, and export.
- `src/core/store.cpp`: Event storage, replacement, expiry, and selection.
- `src/core/files.cpp`: Bounded regular-file reads that reject pipes without waiting.
- `src/core/preferences.cpp`: Versioned JSON storage, file locking, and input bounds.
- `src/app/window.cpp`: Dock layout, menus, and plugin integration.

The map uses the Qt OpenGL paint engine through `QOpenGLWidget`. Its land outlines are schematic and unsuitable for navigation. Detailed map data, network tiles, terrain, and military symbol standards are not implemented.

A fresh profile starts with reception disabled and endpoint `127.0.0.1:6969`. Saved running state resumes reception on later launches. UDP messages are unauthenticated. The application generates and exports events but does not transmit them over the network.

Messages are limited to 16384 bytes, and the store holds at most 4096 identities. XML has depth, element, attribute, and text limits. Unknown detail fields are discarded and do not appear in exported XML.

# Latest interface changes

Events, Event editor, UDP receiver, and Plugins each have an independent `QDockWidget`. The three tool docks start tabbed together beside the map. There is no Workspace tools pane.

Every dock uses the same content wrapper. A `QSizeGrip`, the corner resize handle, appears when a dock floats. The Events pane passed a test that resizes it through a mouse drag on this handle.

Each pane has its own View menu action. View > Reset layout restores the default dock arrangement. File > Quit retains the standard quit shortcut. The Events menu contains Clear events.

The window destroys the docks and map before destroying its event store. Preserve this order because plugin widgets reference the store. Hiding the receiver pane does not stop an active receiver.

# Persistent preferences

Preferences use schema version 1 in `preferences.json` under `QStandardPaths::AppConfigLocation`. The command-line flag `--preferences /path/to/file.json` selects a portable profile. The host saves changes every 750 milliseconds and when the main window closes.

`PreferenceParticipant` gives plugin widgets stable identifiers and bounded JSON save and restore methods. Pane names use these identifiers with a `.pane` suffix. Missing plugin sections and Qt dock placeholders survive later saves.

The application stores window and pane geometry, active dock tabs, map position, table columns, editor drafts, last file directories, and receiver state. Live event data and XML previews remain session data. The profile contains private editor and network information.

Writes use `QSaveFile` and owner-only permissions. `QLockFile` prevents concurrent writers. Malformed or unsupported profiles remain untouched and produce a visible status message.

Restart tests cover absent and returning receiver, map, and codec plugins. Missing map support shows an empty-state message. Missing codec support disables event tools without discarding their preferences.

The core and preferences suites passed in debug, release, and sanitizer builds. All 13 native GUI cases passed at normal and 200 percent scaling. The GUI sanitizer process reports pre-existing Mesa leaks, reproduced with identical counts on baseline commit `362a661`.

The release executable saved a separate profile through `--preferences` during an X11 startup test. The JSON file used owner-only read and write permissions.

# Build environment

The host runs Debian 13 on x86-64 with GCC 14.2 and Qt runtime libraries. CMake, Ninja, and Qt development files were initially absent. Administrator installation failed because sudo required a password.

Debian packages were downloaded and extracted under `/tmp/swak-toolchain/root`. This temporary prefix includes development tools and links to system libraries. It is not committed and can disappear after cleanup or reboot.

The temporary toolchain used Qt 6.8.2, CMake 3.31.6, and Ninja 1.12.1. The application was also installed under `/tmp/swak-install` for an initial startup test. That installed copy predates the latest pane changes.

If the temporary toolchain still exists, run these commands from the repository root. The environment variables apply only to the current shell. The debug build writes its outputs under `build/debug`.

```sh
export PATH="/tmp/swak-toolchain/root/usr/bin:$PATH"
export LD_LIBRARY_PATH="/tmp/swak-toolchain/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cmake --preset debug -DCMAKE_PREFIX_PATH=/tmp/swak-toolchain/root/usr
cmake --build --preset debug --parallel 4
ctest --preset debug
```

Read `docs/build.md` for normal system installation and build commands. Existing CMake caches reference the temporary prefix. If that prefix disappears, use a fresh build directory or refresh the CMake configuration.

The current release executable is `build/release/swak`. Debug, release, and sanitizer builds include the preferences implementation. The temporary installed copy predates these changes.

# Test evidence and limits

The initial core suite passed in debug, release, and sanitizer builds. Native GUI tests passed at normal and 200 percent scaling. Screenshots were inspected for map content and panel layout.

After the pane changes, debug and release builds passed. The debug GUI suite passed all five reported cases on Qt 6.8.2. It exercised repeated detach, resize, close, reopen, and redock operations, plus File > Quit.

The test environment used Xvfb with Mesa llvmpipe, a software OpenGL renderer. Hardware GPU execution and native Wayland behavior remain untested. The declared Qt 6.4 baseline, other architectures, and independent TAK interoperability also remain untested.

After the temporary environment variables above are set, run this command from `build/debug`. Xvfb and xauth must be available. The GUI test writes screenshots into its current directory.

```sh
QT_QPA_PLATFORM=xcb xvfb-run -a -s '-screen 0 1600x1200x24 -nolisten tcp' ./swak_gui_tests
```

The managed sandbox blocked local UDP sockets and display access during tests. Those test commands succeeded with approved execution outside the sandbox. Do not treat a sandbox socket failure as an application failure without inspecting the error.

Read `docs/testing.md` for the complete test report. Read `docs/security.md` before changing input or transport handling. Read `docs/architecture.md` before adding plugins.

# Resume work

Start the next session with these steps:

1. Read this handoff and the latest user request.
2. Inspect `git status --short` and recent commits.
3. Read applicable repository instructions and the required skills.
4. Make sure that the build tools and Qt prefix still exist.
5. Preserve unrelated changes from the user.
6. Run tests for the affected behavior and commit the completed changes.

This file records context and does not authorize additional features. Later user instructions take precedence over this snapshot. Update this handoff when architecture, build requirements, or known limits change.

# Development diagnostics added October 2

Use `python3 tools/quality.py quick` for sanitizer tests. Other modes are `gui`, `soak`, `coverage`, and `analyze`. Read `docs/quality.md` for prerequisites, duration controls, native rendering, reports, and failure rules. The runner uses Python standard-library code and isolated development builds. The release preset explicitly disables testing and instrumentation.

`tests/test_soak.cpp` exercises repeated parsing, local UDP traffic, event expiry, plugin widget lifetimes, and preference writes. It measures resident memory, open descriptors, event-loop delay, and idle CPU use. An external process timeout detects hangs even if the Qt event loop stops. `--gui` adds the application window and map to soak or coverage mode.

The quick suite, three runner tests, static analysis, native coverage, and release build passed. A 60-second headless soak passed. A ten-second native soak passed its assertions but failed leak detection with the previously documented Mesa allocations. Coverage reached 87.7 percent of source lines and 89.5 percent of functions. Read `docs/testing.md` for remaining paths and measurements. Multi-hour stability remains untested.

Cppcheck 2.17.1 and its XML library were extracted under `/tmp/swak-quality-tools/root` for this session. This temporary installation is not part of the application. Add `/tmp/swak-quality-tools/root/usr/bin` to `PATH` and `/tmp/swak-quality-tools/root/usr/lib/x86_64-linux-gnu` to `LD_LIBRARY_PATH` before the analysis mode. A normal Debian development host can install Cppcheck through its package manager. `AGENTS.md` now requires regular diagnostics, coverage review, dead-code review, and resilient error handling.

# Settings and logging added October 2

File > Settings opens a dialog for logging, workspace geometry, pane layout, and plugin configuration. `src/core/settings.h` defines the optional `SettingsParticipant` interface. Plugin objects or their widgets provide bounded fields and validation. The host discovers them without plugin-specific dialog code. Apply and OK save through the existing profile, and Cancel discards pending changes.

`src/core/logging.cpp` owns the Qt handler and application logging API. The executable installs the handler before `QApplication` and starts silent. The selected profile can explicitly enable output with severity, urgency, category, Qt-source, and rate filters. The logger uses bounded text and nonblocking pipe or socket output. Logging cannot turn fatal Qt diagnostics into recoverable errors.

The reported mouse-grab message comes from `QWaylandWindow::setMouseGrabEnabled` in Qt Wayland. Default logging now suppresses its text. This change does not add unsupported Wayland mouse capture. Read `docs/usage.md` and `docs/architecture.md` for configuration and extension contracts.

The logging, core, preferences, and workload suites passed under sanitizers. All 18 native GUI cases passed at normal and 200 percent scaling. They include a test plugin section and configuration persistence through plugin absence. Static analysis passed. A release startup with Qt plugin debugging requested produced no console output. Native sanitizer GUI execution still reports the known Mesa leak, without suppression.

# Settings organization update

File > Settings replaces the separate Settings menu. The dialog has General and Panes tabs, with plain pane names. The interface now uses CoT Event Generator and CoT Events. Persistent plugin and pane identifiers remain unchanged.

Settings section lists and dropdown choices use system locale ordering through `QCollator`. Sorting preserves the values attached to choices. The dialog retains its active tab and selected sections in the profile.

All 19 native GUI cases passed at normal and 200 percent scaling. New tests cover Swedish alphabetical order, choice values after sorting, tab separation, names, menu placement, and dialog navigation. Screenshots show both tabs without horizontal overflow. Cppcheck passed, including its unused-function scan. The existing restart and missing-plugin cases also passed.

This interface change does not add dependencies, threads, or long-running loops. This pass did not repeat sanitizer or coverage runs. Native Wayland behavior and other locale orders remain untested in this pass. The earlier Mesa leak remains an open limit.

# Plugin menus added October 2

Tools > Plugins now contains commands from the optional `MenuPlugin` interface in `src/core/interfaces.h`. The CoT Events plugin provides Clear. The host removes empty plugin submenus and disables Plugins when no commands exist. Each window owns its menus and actions. The Clear connection uses that window event store as its lifetime context.

The debug build and Cppcheck passed. The coverage run passed the standard suites and all 21 native GUI cases. Reports are under `build/quality/20261002-100007-coverage-69338`. All new host menu lines and the plugin menu method ran. Tests cover extension discovery, empty and absent providers, repeated window destruction, clearing events, and clearing the selection. The two focused menu tests also passed with address, undefined behavior, and leak detection enabled outside the sandbox.

Coverage still misses existing startup, native file dialogs, mouse map controls, and some pane configuration paths. This change does not remove those gaps. Native screenshots show the Tools menu in dark and light themes. Native Wayland menu behavior remains untested. The release executable still reflects the previous Settings commit until its next rebuild.

# Toolbar and About added October 2

The optional `ToolbarPlugin` interface adds icon actions to the fixed top toolbar. The host supplies each provider with its existing plugin menu, when available. CoT Events shares one Clear action between its menu and toolbar. Icons use the desktop theme with a Qt fallback. View > Main toolbar controls visibility, which survives restarts and absent plugins. Reset layout restores the toolbar.

Help > About shows the version and general application information. Only one About dialog opens for each window. CMake generates `swak/version.h` from `PROJECT_VERSION` for both About and the command-line version. The version remains 0.1.0. No external dependency was added.

Coverage passed the standard suites and all 23 native GUI cases on Qt 6.8.2 with GCC 14.2. Reports are under `build/quality/20261002-103845-coverage-73841`. All new toolbar and About lines ran. The tracks plugin reached 136 of 136 executable lines. Existing startup, file-dialog, map-mouse, and pane-configuration gaps remain. Cppcheck passed after the version moved into a generated header.

Four focused tests passed with address, undefined behavior, and leak detection enabled. Native toolbar, About, and both theme tests also passed at 200 percent scaling. Screenshots show readable About text and the top toolbar. Native Wayland and hardware GPU behavior remain untested in this pass. The release executable includes the earlier plugin menus but needs a rebuild for the toolbar and About changes.

# Online map providers added October 2

Research and access rules are in `docs/map-providers.md`. Three static plugins implement XYZ, WMTS 1.0 KVP, and WMS 1.3 raster requests for EPSG:3857. Presets cover OpenStreetMap, NASA GIBS Blue Marble, and terrestris OpenStreetMap. NASA capabilities and a sample JPEG response were checked live. The terrestris capabilities confirm WMS 1.3.0, EPSG:3857, and OSM-WMS. Automated map tests use only a local HTTPS fixture.

`MapExtension` classifies plugins as providers or tools. `MapProvider` exposes source defaults, fields, validation, and tile URLs. No map tool exists yet. Tools > Map Providers manages registered presets and custom sources. Unknown formats survive profile saves. Settings exposes provider parameters and map opacity, filtering, antialiasing, center, and zoom. Fresh profiles stay offline.

The existing OpenGL widget draws downloaded images between the reference map and event layers. `TileRenderer` limits requests, bytes, dimensions, time, and cache size. HTTPS checks start enabled. A per-source override uses a separate cache and appears in the interface. Redirects and cookies are disabled. Cached images still pass format and dimension limits. No new external library was added.

Coverage passed all five standard suites and all 24 native GUI cases. The report is `build/quality/20261002-111832-coverage-81265`. It exercised 120 of 120 catalog lines and 169 of 169 tile-renderer lines before the final lock-recovery refinement. The manager exercised 98 of 99 lines. The untested manager line handles Add when the catalog is full or no format is installed. Some defensive branches and existing application paths remain untested. An executed line does not establish full input coverage.

Native provider and Settings tests passed at normal and 200 percent scaling. The full native sanitizer test passed 24 GUI cases, then failed on the existing Mesa leak of 240732 bytes in 1073 allocations. The report is `build/quality/20261002-111718-gui-80973`. No application memory error appeared in that run. Hardware GPU, Wayland, and multi-hour online sessions remain untested. Capabilities import, vector tiles, offline archives, and other projections remain outside this implementation.

The final sanitizer run passed all five suites, including expired-cache revalidation and lock recovery. Its report is `build/quality/20261002-112011-quick-81628`. Final Cppcheck analysis passed in `build/quality/20261002-112012-analyze-2`. The development executable includes the map work. The release executable still needs a rebuild.

# Provider forms and attribution updated October 2

Map attribution now aligns to the right over a translucent background. The visible text excludes the license URL. Mouse and keyboard activation open that URL through the default browser. Only HTTP and HTTPS URLs without credentials can open. All text passes HTML escaping before link creation. File > Settings > Map includes attribution background opacity, with a default of 0.55.

The provider manager no longer contains a format selector. Add opens an unsaved form with empty text fields and a format selector. Numeric values and image formats retain safe defaults. Cancel does not create a provider. OK requires valid input and saves only the selected format. Edit uses a single form without the General and Panes tabs. Its labels do not wrap. The unused preset-copying method was removed.

The debug build passed with Qt 6.8.2 and GCC 14.2. All 24 native GUI cases passed. Focused provider and Settings cases also passed at 200 percent scaling. Tests cover blank input, cancellation, successful creation, editing, keyboard link activation, and horizontal overflow. Native screenshots show the compact attribution and readable provider labels. Browser activation uses a test handler, so tests do not launch an external browser.

Static analysis passed in `build/quality/20261002-123602-analyze-2`. All five sanitizer suites passed in `build/quality/20261002-123610-quick-86545`. This pass did not repeat GUI leak analysis, coverage collection, Wayland tests, hardware GPU tests, or long-session tests. Previous coverage gaps and the external Mesa leak remain documented above. The release executable contains the earlier map implementation and needs a rebuild for these interface changes.

# Release rebuild requirement added October 2

`AGENTS.md` now requires a release rebuild before completing each task that changes the application. `docs/build.md` records the same requirement. The Ninja release build passed and includes the provider forms and attribution changes from `e2f4129`. The executable is `build/release/swak`. Its version command reports SWAK 0.1.0. This task changes documentation only and uses the functional test results recorded above.

# Toolbar, internal events, and plugin API updated October 2

The map plugin now adds a provider dropdown to the main toolbar. It includes the offline map, locale-sorted sources, and the provider manager. Missing or invalid providers stay disabled. The existing map catalog saves selection across restarts.

The bottom status field shows the latest internal event for three seconds. New events restart its timer. Error and fatal messages use red text. The Internal events plugin provides a sortable history table and a Clear command. Its pane starts hidden beside the CoT Events tab.

Tools > Plugins controls all pane visibility. Plugins with commands include Show pane in their submenu. View contains only Show main toolbar and Reset layout.

File > Settings > Internal events controls the history limit from 1 through 10000 records, with a default of 500. The model and inbox have fixed bounds. History stays in memory, while the limit and table layout persist. Text logging stays disabled by default. Editor operations, receiver activity, configuration failures, map changes, and existing internal diagnostics use the shared logger.

All ten built-in plugins implement the common Plugin API version 2 and embed a MANIFEST. The loader validates identifiers, versions, interfaces, entry points, platform requirements, and permission declarations. Plugins remain statically linked and trusted. UI callbacks receive PluginServices with the store, activity model, and map catalog. The obsolete MapConsumer interface is removed.

No plugin searches the host widget tree for services. Contracts, manifests, ownership, and the plugin inventory are documented in `docs/plugin-api.md`.

The debug and release builds passed with Qt 6.8.2 and GCC 14.2. The release executable is `build/release/swak` and reports version 0.1.0. Tests and diagnostic instrumentation remain disabled in that build. All 25 native GUI cases passed. Focused tests also passed at 200 percent scaling. Native screenshots show the toolbar and internal-event table.

Static analysis passed in `build/quality/20261002-131004-analyze-2`. Final sanitizer suites passed in `build/quality/20261002-131633-quick-96018`. Coverage passed six core suites and the native GUI suite in `build/quality/20261002-131219-coverage-94626`. It exercised 78 of 81 activity-model lines, 69 of 73 catalog lines, and all 33 activity-plugin lines. Remaining gaps include shutdown guards, catalog rejection during static discovery, existing native file dialogs, and map mouse controls. Line coverage does not establish complete input coverage.

The native GUI sanitizer run passed 25 functional cases but failed on the existing Mesa leak of 240732 bytes in 1073 allocations. Its report is `build/quality/20261002-130808-gui-93592`. No application memory fault appeared in that run.

A 60-second sanitizer workload passed in `build/quality/20261002-131330-soak-95009`. It completed 912 cycles, with 6893568 bytes of peak growth after warmup, a 171-millisecond maximum heartbeat gap, and 0.028 idle CPU seconds. The first attempt exposed an incorrect test assumption about mutated expiry times. The test now expires events after the latest accepted expiry time. It does not change application expiry behavior. Two-hour workloads, native Wayland, and hardware GPU sessions remain untested in this pass.

# Map background and whole zoom levels

The October 2 update removes the schematic offline map. The background uses NATO-style green, `#303D2B`. Each missing tile shows faint MISSING TILE text and a faint edge. The Map configuration controls marker opacity. The provider dropdown contains No map provider to clear the selection. The manager uses Clear selection.

The background plugin keeps its identifier and manifest entry point for compatibility. Its name changes to Map background. The tile renderer uses the same green as the OpenGL clear color. No external dependency was added.

Zoom now uses whole levels within the selected provider limits. Wheel input accumulates partial steps. Keys, Home, provider changes, and restored preferences apply the same limits. Old fractional values round to the nearest whole level. Without a valid provider, levels 0 through 19 remain available.

Debug and release builds passed with Qt 6.8.2 and GCC 14.2. The release executable is `build/release/swak`. Six sanitizer suites passed in `build/quality/20261002-142842-quick-109181`. Static analysis passed in `build/quality/20261002-143111-analyze-2`.

Coverage passed six suites and all 30 native GUI cases in `build/quality/20261002-142950-coverage-109585`. Tests cover marker appearance, transparent markers, restored fractional zoom, provider changes, and input limits. The tile renderer reached 203 of 203 executable lines. The background plugin reached six of six lines. The map plugin reached 243 of 280 lines. Remaining map gaps include mouse controls, arrow keys, and wheel gesture phases. Line coverage does not establish complete input coverage.

Focused native tests passed at 200 percent scaling. Screenshots in `build/map-background/hidpi` show the faint tile markers against the green background. All 30 native GUI cases also passed under sanitizers. LeakSanitizer then reported the existing Mesa leak of 240732 bytes in 1073 allocations. The report is `build/quality/20261002-143110-gui-110357`. This native leak test remains failed.

A 60-second sanitizer workload passed in `build/quality/20261002-143247-soak-110973`. It completed 951 cycles. Peak memory growth after warmup was 6438912 bytes. The maximum heartbeat gap was 60 milliseconds. Idle CPU use was 0.007 seconds. Multi-hour sessions, hardware GPUs, native Wayland, and the minimum Qt version remain untested in this pass.

# Map labels and plugin activation

The October 2 update removes the text overlay from the upper-left map corner. Map controls remain in `docs/usage.md`, the Markdown user manual. Missing tile labels now rise at 45 degrees. Their opacity and tile borders remain unchanged.

The toolbar labels each plugin section with its name. Separators divide adjacent sections. ToolbarPlugin now requires `name()` and uses interface identifier `org.swak.Toolbar/3.0`. The common Plugin API remains version 2. Existing plugins supply their names through the same method that their other interfaces use.

Clicking a plugin name under Tools > Plugins opens and focuses its pane. It does not hide an open pane. Return and Space also activate panes. Hover and Right open command submenus. The Show pane action and visibility checkboxes are removed. Pane close buttons still hide panes. The host handles activation for docked, tabbed, and floating panes.

Debug and release builds passed with Qt 6.8.2 and GCC 14.2. The release executable is `build/release/swak`. Existing Qt test macros report conversion warnings in the soak test. No new application warning appeared.

Six sanitizer suites passed in `build/quality/20261002-144302-quick-112233`. Final static analysis passed in `build/quality/20261002-144702-analyze-3`. Coverage passed six suites and all 31 native GUI cases in `build/quality/20261002-144625-coverage-114139`. Tests exercise mouse activation, keyboard activation, submenu access, repeated activation, and detached panes. An initial direct-action mouse failure led to a shared handler for both menu entry types.

The window reached 452 of 462 executable lines. The tile renderer reached 209 of 209 lines. The map plugin reached 241 of 278 lines. Remaining gaps include default mouse handling in the plugin menu, file dialogs, startup, map mouse controls, and some recovery branches. Line coverage does not establish complete path coverage.

Native screenshots show the diagonal text and named toolbar sections. Focused tests passed at 200 percent scaling. The full native sanitizer run passed all 31 functional cases, then reported the existing Mesa leak. It leaked 240732 bytes in 1073 allocations. This test remains failed in `build/quality/20261002-144630-gui-114213`.

Native Wayland, hardware GPUs, multi-hour sessions, and the minimum Qt version remain untested in this pass. No external dependency was added.

A 60-second sanitizer workload passed in `build/quality/20261002-144811-soak-114835`. It completed 954 cycles with 6791168 bytes of peak memory growth after warmup. The maximum heartbeat gap was 59 milliseconds. Idle CPU use was 0.030 seconds.

# Plugin checkboxes restored

Tools > Plugins again has a checkbox for each pane. Mouse clicks, Return, and Space toggle pane visibility. Pane close buttons update the checkbox. Command submenus remain available without a Show pane entry.

The release rebuild passed. Six sanitizer suites and all 31 native GUI cases passed. Reports are `build/quality/20261002-145255-quick-115651` and `build/quality/20261002-145251-coverage-115590`. Static analysis passed in `build/quality/20261002-145257-analyze-3`. Focused native tests passed at 200 percent scaling.

The native sanitizer test passed all 31 functional cases, then reported the existing Mesa leak of 240732 bytes in 1073 allocations. Its report is `build/quality/20261002-145451-gui-116287`. A 60-second workload passed in `build/quality/20261002-145612-soak-116446`. It completed 941 cycles with 6504448 bytes of peak memory growth, a 63-millisecond maximum heartbeat gap, and 0.009 idle CPU seconds. Previous platform and multi-hour test gaps remain.

# KML and KMZ data providers

Tools > Data Providers manages multiple KML/KMZ feeds. Add and Edit use the shared provider form. List checkboxes enable feeds. Remove deletes feeds, and Refresh now updates an enabled feed. Each feed has a persistent interval from 60 through 86400 seconds, with a default of 300. The same parameters appear in File > Settings > Data providers.

The catalog starts with disabled USGS earthquake and NOAA NDBC observation presets. Research, source terms, live sample results, and supported content are in `docs/data-providers.md`. The parser accepted 30 earthquake points and 1967 NDBC locations from the sampled public responses. The NHC active KMZ contained 12 network links, which this implementation reports without following.

A new static KML plugin implements DataProvider and embeds its MANIFEST. DataSources owns each window's catalog, HTTPS clients, timers, and parsed geometry. PluginServices has an optional data catalog pointer. The common Plugin API remains version 2. All eleven built-in manifests passed catalog tests.

Qt handles HTTPS, XML, and background work. Libarchive handles ZIP in memory through its complete-directory reader. No file extraction or automatic linked-resource retrieval occurs. Bounds cover bytes, archive entries, XML depth, geometry, and work. Explicit counters limit active updates to two, including cancelled parsers until they finish. A regression test exercises parser cancellation while another feed waits.

The GPU map draws points, lines, polygons with holes, and names above imagery and below CoT events. Feed appearance controls include color, opacity, line width, point radius, fill opacity, and labels. Credits share the existing right-aligned, clickable panel. Failed refreshes retain prior geometry and mark its credit STALE. Editing or disabling a feed clears its geometry. Missing provider configuration survives restarts.

This version does not follow KML network links or load remote icons, HTML balloons, image overlays, models, or tours. It ignores KML styles and time animation. It uses flat geometry and feed appearance controls. These limits appear in the manager and manual. Multi-hour online sessions, native Wayland, hardware GPUs, and the minimum Qt version remain untested.

The release executable is rebuilt at `build/release/swak`. An installation smoke test passed in `build/data-package`, including the new manual. The installed executable reports version 0.1.0. The tested stack is Qt 6.8.2, GCC 14.2, Debian 13, and libarchive 3.7.4-4+deb13u1. Release linkage contains no sanitizer library. The dependency review records open notices and the library paths used by this application.

Seven sanitizer suites passed in `build/quality/20261002-153500-quick-124478`. Tests cover malformed XML, KMZ bounds, unsafe paths, duplicate documents, geometry, HTTPS failures, refresh timers, parser exceptions, cancellation, and repeated replacement. The data suite also mutates 500 short documents. Final static analysis passed in `build/quality/20261002-153546-analyze-3`.

Focused native tests passed at 200 percent scaling. Screenshots in `build/data-native/hidpi` show the provider manager and downloaded points, lines, and polygons. The OpenGL framebuffer captures geometry separately from the child attribution label.

A 60-second workload passed in `build/quality/20261002-153130-soak-124194`. It includes repeated KML decoding. It completed 944 cycles with 8159232 bytes of peak memory growth, a 98-millisecond maximum heartbeat gap, and 0.017 idle CPU seconds. This workload does not exercise live feeds or prove multi-hour memory stability.

Final coverage passed seven suites and all 32 native GUI cases in `build/quality/20261002-153545-coverage-124767`. The data controller reached 233 of 234 executable lines. The KML plugin reached 163 of 165 lines. The manager reached 86 of 86 lines. Remaining gaps include the skipped-content status and foreign-namespace branches, plus the manager's double-click and manual-refresh callbacks. Separate live samples exercised foreign namespaces. Line coverage does not establish complete branch or input coverage.

The final native sanitizer run passed all 32 functional cases, then reported the existing Mesa leak of 240732 bytes in 1073 allocations. This leak test remains failed in `build/quality/20261002-153836-gui-125700`. The final release build passed after the update counter fix. Its log is `build/data-verified-release.log`.

# OpenFreeMap and vector layers

The October 2 update adds OpenFreeMap to Map Providers through the MVT plugin. Existing profiles receive the preset once. The catalog records known provider identifiers so deleted presets stay deleted. The preset uses the latest tile alias and zoom levels 0 through 14. Online access stays disabled until the user selects a provider.

The plugin implements MapProvider and the new VectorTileProvider capability. All twelve manifests use common Plugin API 2. The decoder accepts bounded MVT layer versions 1 and 2. It returns owning geometry, exact identifiers, properties, colors, and label flags. The host decodes on two workers and paints through the existing OpenGL map. Four slots cover both downloads and decode jobs. Cancellation flags and source generations prevent stale results.

Right-click opens Map layers and Map information. The keyboard Menu key uses the map center. Layer visibility, label appearance, language, line width, and point radius use shared provider configuration. The information dialog shows plain-text properties for up to twelve matches. Layer choices and dialog geometry survive restarts. Missing plugins retain their configuration.

The renderer uses a native OpenMapTiles palette, system fonts, and simple markers. It does not interpret MapLibre style documents, sprites, terrain, or remote fonts. It does not import TileJSON at runtime or add geocoding and routing. Research and source terms are in `docs/map-providers.md`. Live decoding accepted 79 features in a world tile and 4142 features in a Stockholm tile with POIs.

Protozero 1.8.2 supplies binary field decoding. Its unmodified headers add about 204 KiB to the source tree and no separate runtime library. `docs/dependencies.md` records the archive digest, upstream security review, motivation, and license notices. Installation includes both upstream license files. The host retains HTTPS validation, timeout, cache, retry, and disabled logging defaults.

Input limits cover bytes, layers, dictionaries, properties, geometry commands, vertices, and coordinates. Frame limits cover vertices and labels. When visible geometry fills the cache, other tiles wait for a view change. This prevents repeated eviction and decoding loops. Tests exercise cache pressure, cancellation during replacement and shutdown, decoder exceptions, malformed input, holes, property types, and native interactions.

The release rebuild passed with Qt 6.8.2, GCC 14.2, and Debian 13. The executable remains `build/release/swak`, version 0.1.0. Eight sanitizer suites passed in `build/quality/20261002-161346-quick-132028`. Static analysis passed in `build/quality/20261002-161508-analyze-3`. Focused native tests passed at normal and 200 percent scaling. Screenshots are under `build/openfreemap-native`.

All 33 native GUI cases passed under sanitizers. LeakSanitizer then reported 169724 bytes in 756 allocations from `libGLX_mesa.so.0`. This is the existing driver leak class, with a different allocation count in this run. The native leak test remains failed in `build/quality/20261002-161649-gui-133579`. No suppression or false success was added. Hardware GPUs, native Wayland, minimum Qt compatibility, and multi-hour live-map sessions remain untested in this pass.

Final coverage passed eight suites and all 33 native GUI cases in `build/quality/20261002-161805-coverage-134052`. The MVT plugin reached 206 of 209 executable lines. The shared vector painter reached 71 of 71 lines. The tile controller reached 277 of 278 lines, and the provider catalog reached 140 of 140. Remaining parser misses include a defensive switch default after its tag guard and compiler-attributed closing lines. No parser function remained uncalled. Existing map mouse controls, some arrow keys, and gesture paths remain coverage gaps. Line coverage does not establish complete branch or input coverage.

The final vector sanitizer run passed nine cases in `build/openfreemap-vector-sanitize-final.log`. It includes panning away during decoding and unknown extension fields. A 60-second sanitizer workload passed in `build/quality/20261002-161801-soak-134020`. It completed 972 cycles with 8220672 bytes of peak memory growth, a 77-millisecond maximum heartbeat gap, and 0.022 idle CPU seconds. This short workload does not replace the documented multi-hour release qualification.

The installation smoke test passed in `build/openfreemap-package`. The installed application reports version 0.1.0 and includes the manuals and both protozero license notices. Release linkage contains no sanitizer library or new runtime dependency. The final build and installation logs are `build/openfreemap-release-final.log` and `build/openfreemap-install.log`.

# Vector cache recovery

The reported cache warning came from the fixed 64 MiB decoded-tile budget. The sampled Stockholm tile accounts for 6531 KiB. A normal viewport can therefore exceed the old budget. The previous pressure guard then stopped loading the remaining tiles until the view changed.

The shared map configuration now includes Tile memory cache, from 64 through 512 MiB, with a 256 MiB default. Existing profiles receive that default when the field is absent. The renderer removes offscreen tiles before reducing detail. If visible geometry still exceeds the budget, it requests lower tile zoom levels without changing the map center or selected zoom. This fallback stops at the provider minimum. It remains stable across repaints and retries full detail after a new view or budget change.

The memory budget stays enforced, and decoding still uses at most four combined download and worker slots. A source generation cancels obsolete work during fallback. The host also discards completed tiles that are no longer visible. At the minimum zoom, an exhausted budget produces one actionable warning instead of repeated warnings for every rejected tile. Cache warnings now derive from the current deferred set, so they clear when the condition clears.

The regression test reproduces 160000 KiB of visible tile estimates. It tests full-detail loading at the default budget, lower-detail recovery at 64 MiB, minimum-zoom exhaustion, and recovery after increasing the budget. It also makes sure that a stable view does not repeatedly decode tiles. The native test stores a custom memory limit and restores it after restart.

The final release rebuild passed with Qt 6.8.2 and GCC 14.2. The executable is `build/release/swak`, version 0.1.0. Eight sanitizer suites passed in `build/quality/20261002-165535-quick-138789`. Static analysis passed in `build/quality/20261002-165614-analyze-3`. Final coverage passed eight suites and all 33 native GUI cases in `build/quality/20261002-165535-coverage-138793`.

The tile controller reached 310 of 311 executable lines. The remaining line is a compiler-attributed closing line. Existing map mouse and gesture coverage gaps remain. Line coverage does not establish complete branch or input coverage. An initial test waited for worker completion before the host applied its result. The final test waits for the observable cache state instead.

All 33 native cases also passed under sanitizers. LeakSanitizer then reported the existing Mesa leak of 240732 bytes in 1073 allocations. That native leak test remains failed in `build/quality/20261002-165711-gui-139525`. Hardware GPUs, native Wayland, minimum Qt compatibility, and multi-hour live-map sessions remain untested.

A 60-second sanitizer workload passed in `build/quality/20261002-165818-soak-139809`. It completed 924 cycles with 8183808 bytes of peak memory growth, an 83-millisecond maximum heartbeat gap, and 0.017 idle CPU seconds. The focused pressure test also passed before the full diagnostics. The release build log is `build/vector-cache-release-final.log`.

# Vector drawing and panning, 2026-10-02

The gray squares came from a shared limit of 200000 vertices per frame. Later tiles received a background but no geometry. Panning also transformed and painted every decoded path on the GUI thread.

The host now prepares complete vector images on its existing two workers. The OpenGL view reuses those images during panning. Each image contains 512 physical pixels for 256 logical pixels. The memory budget includes the additional 1024 KiB per image. Decoded geometry remains available for map information and layer controls. No new dependency or plugin interface version was added.

Source, layer, and antialiasing changes discard prepared images. Cancellation covers painting between features, and obsolete generations cannot enter the cache. Reduced detail now remains stable during panning. A zoom, source, or memory limit change permits full detail again. Labels use a per-tile limit and remain clipped at tile edges. Extreme display scaling and reduced-detail fallback can soften images and enlarge labels.

The new regression uses four tiles with more than 200000 vertices in total. All four retain their geometry. Layer changes replace their prepared images. In the focused debug run, 120 cached pan frames took 64 milliseconds without additional decoding. A local Stockholm sample contains 4142 features. Ten software frames with 16 copies took 11001 milliseconds for complete direct painting and 13 milliseconds for cached composition. These timings exclude downloads and initial preparation. They are not hardware GPU measurements.

Eight sanitizer suites passed in `build/quality/20261002-172852-quick-141261`. Static analysis passed in `build/quality/20261002-173021-analyze-142067`. Coverage passed eight suites and 33 native GUI cases in `build/quality/20261002-172856-coverage-141297`. The vector painter reached all 68 executable lines. The tile controller reached 322 of 326 lines. The image allocation failure path remains untested. Other misses are compiler-attributed closing lines. Existing mouse and gesture coverage gaps remain. Line coverage does not establish full branch coverage.

All 33 native sanitizer cases passed before the existing Mesa leak caused status 23. LeakSanitizer reported 240732 bytes in 1073 allocations from `libGLX_mesa.so.0`. The failed report remains `build/quality/20261002-173131-gui-142618`. The focused native vector test also passed at 200 percent scaling. Its screenshot is under `build/vector-pan-hidpi`.

The release rebuild passed with Qt 6.8.2 and GCC 14.2 on Debian 13. The executable remains `build/release/swak`, version 0.1.0. The build log is `build/vector-pan-release.log`. Hardware GPUs, native Wayland, minimum Qt compatibility, and multi-hour live-map sessions remain untested.

The 60-second sanitizer workload passed in `build/quality/20261002-173244-soak-142884`. It completed 927 cycles with 8134656 bytes of peak memory growth, a 70-millisecond maximum heartbeat gap, and 0.019 idle CPU seconds. This short run does not replace multi-hour release qualification.

# Provider tables, status counts, and map controls, 2026-10-02

The map footer now shows only coordinates and zoom. The status bar shows separate CoT Events and retained Internal events counts. Separators divide the fields. A preferences-error separator appears only when an error exists. Internal messages no longer replace status text or raise a pane. History remains available through Tools > Plugins > Internal events.

A left double-click centers the clicked map point without changing zoom. The release event does not select a different CoT event. Existing center preferences retain the new position after restart.

MapManager and DataManager now use the shared ProviderTable. Map rows contain radio buttons, and data rows contain independent checkboxes. Selecting a row does not activate it. Edit and Delete use that row, including when its Format cell holds focus. Missing plugins retain their entries with disabled activation controls. Names use the system collation order.

Both provider sections in File > Settings embed these same managers. Add and Edit keep the shared provider forms. Catalog actions apply and save immediately, as stated in each section. Generic Apply and Cancel do not replace catalog state with an old snapshot. Embedded managers do not replace the saved geometry of separate management windows. SettingsPage has a host-owned widget factory. Plugin interface versions remain unchanged.

The existing local HTTP cache now has a persisted disk limit under File > Settings > Map. The default remains 128 MiB, bounded from 16 through 4096 MiB per HTTPS certificate-policy cache. The label below attribution reports current disk usage for the active cache. A two-second timer reads Qt cache statistics outside map painting. No open provider cache produces an inactive label. Cached raster and vector responses remain separate from decoded memory. Fresh responses survive restart and avoid another download. Expiry, HTTP revalidation, no-store rules, certificate isolation, and process locking remain enforced. This is an expendable cache, not an offline archive or bulk downloader.

Focused native tests passed all seven cases. They cover activation, multiple feeds, row deletion, hidden event history, counts, cache labels, double-click centering, and restored preferences. The new double-click test uses the exact logical center, including half-pixel centers for odd widget dimensions. A data fixture now supplies required attribution. Screenshots are under `build/providers-ui-native`. Four focused cases also passed at 200 percent scaling under `build/providers-ui-hidpi`.

Eight sanitizer suites passed in `build/quality/20261002-191432-quick-145829`. Static analysis passed in `build/quality/20261002-191553-analyze-146471`. Coverage passed eight suites and all 35 native cases in `build/quality/20261002-191434-coverage-145840`. ProviderTable reached all 43 executable lines, and DataManager reached all 88. MapManager reached 114 of 116 lines. The full-catalog Add rejection remains unexecuted. The image allocation failure and some map mouse, arrow-key, and gesture paths remain coverage gaps. Line coverage does not establish complete branch or input coverage.

The release rebuild passed without compiler warnings with Qt 6.8.2 and GCC 14.2 on Debian 13. The executable remains `build/release/swak`, version 0.1.0. The build log is `build/providers-ui-release.log`. Hardware GPUs, native Wayland, minimum Qt compatibility, and multi-hour live-map sessions remain untested.

All 35 native sanitizer cases passed before LeakSanitizer reported the existing Mesa leak. The report contains 240732 bytes in 1073 allocations from `libGLX_mesa.so.0`. The failed run remains visible in `build/quality/20261002-191843-gui-147418`. No leak suppression or false success was added.

The 60-second sanitizer workload passed in `build/quality/20261002-192001-soak-147570`. It completed 933 cycles with 8437760 bytes of peak memory growth, an 88-millisecond maximum heartbeat gap, and 0.058 idle CPU seconds. This short workload does not replace multi-hour release qualification.

# Map selection, movement, and viewport limits, 2026-10-02

The map now supports continuous W/A/S/D panning at 300 logical pixels per second by default. Settings exposes four unique keys, speed, wheel enablement, wheel direction, and wheel steps. Diagonal movement keeps the same speed. Opposite keys cancel movement. Focus loss, window deactivation, and hiding stop the timer. All controls use saved preferences.

The map fetches one extra tile ring by default. Settings permits zero through two rings. The host limits the extra area to 64 tiles and completes visible requests first. Extra tiles cannot evict visible vector tiles under memory pressure. Tests cover nearby cache reuse, cancellation, and bounded memory pressure.

The lowest whole zoom level fills the viewport. Every movement path clamps the center against world edges. Resize and restored preferences apply the same bounds. The cropped world remains accessible by panning. A viewport larger than the highest provider level enlarges existing tiles instead of requesting unsupported levels.

Data geometry follows the current projection and stays inside the world bounds. This removes repeated objects outside the base map. Hit detection uses the same normalized geometry and respects the displayed label limit. The Information plugin shows a read-only label, type, description, and property table. Double-clicking a value copies through Qt clipboard support. Linux also receives the primary selection when available.

The shared MapSelection service owns the selected snapshot. Data, vector, and CoT map hits use this service. Selected points have white outlines. Routes use inverse colors. Polygons have white dashed edges and faint fill. Empty clicks clear selection and hide Information. Source changes clear related snapshots. Selection is transient, while pane layout and header preferences persist.

PaneContent exposes a visibilityRequested signal and a preferredDockArea method through the shared core API. Information starts in the right dock group. This keeps the map and editor usable in shorter windows. Saved layouts take precedence.

The host handles dock visibility without plugin access to the application window. The new static Information plugin has a MANIFEST with clipboard-write permission. Thirteen built-in plugins now use the shared API. Documentation covers the extended service and pane contracts.

KML parsing retains plain-text descriptions, scalar properties, ExtendedData, and point coordinates. Names have a 128-character limit. Values have a 4096-character limit. Placemark metadata has a 256-property limit, and each document has an estimated 8 MiB metadata limit. Invalid documents preserve the previous feed.

The release rebuild passed without compiler warnings with Qt 6.8.2 and GCC 14.2 on Debian 13. The executable remains `build/release/swak`, version 0.1.0. The build log is `build/information-release.log`. Hardware graphics, native Wayland, Qt 6.4 compatibility, and multi-hour use remain untested.

Coverage passed eight suites and all 37 native cases in `build/quality/20261002-200948-coverage-157893`. DataSources reached 296 of 297 lines. MapSelection painting reached all 24 lines. Information reached all 15 functions and 59 of 61 lines. Map controls reached 486 of 499 lines. Native screenshots show the side pane, read-only fields, and white selection outline.

Remaining coverage gaps include the data allocation failure path and non-left double-click fallbacks. Mouse dragging, some arrow-key branches, and several KML rejection branches remain unexecuted. Line coverage does not establish complete branch or input coverage. No new external dependency was added.

All 37 native sanitizer cases passed in `build/quality/20261002-201109-gui-159163`. LeakSanitizer then reported the existing Mesa leak of 240732 bytes in 1073 allocations. The failed diagnostic remains visible. No suppression or false success was added.

The 60-second sanitizer workload passed in `build/quality/20261002-201132-soak-159477`. It completed 1268 cycles with 8253440 bytes of peak memory growth. The maximum heartbeat gap was 39 milliseconds, and idle CPU time was 0.007 seconds. This short run does not replace multi-hour qualification.

An overlapping coverage and sanitizer run failed during a vector cancellation fixture. Shared Qt cache locks can cause this contention. Diagnostic instructions now require sequential application tests across build presets. The final quick run executes alone.

The final eight sanitizer suites passed in `build/quality/20261002-201316-quick-159611`. Static analysis passed in `build/quality/20261002-201317-analyze-3`. The final release command reported no remaining build work.

Five focused native cases passed at 200 percent scaling in `build/information-hidpi/native.log`. They include zoom limits, the Information pane, and keyboard and wheel controls.

# Dense vector recovery and viewport coverage, 2026-10-02

Vector responses now permit eight MiB. Raster responses remain limited to two MiB. Vector parsing permits 50000 features, 500000 vertices, and 65536 dictionary entries per layer. Retained properties have a 16 MiB limit. Estimated geometry and properties have a 64 MiB limit per tile. Worker and request concurrency remain unchanged.

VectorTile has a resourceLimit flag. Limit failures use lower tile levels down to the provider minimum. Invalid input and cancellation have separate error messages. Unsupported minimum-level content remains an explicit failure with bounded retries. The host never counts these failures as rendered tiles.

TileRenderer now exposes coverage counts and audits incomplete coverage every two seconds. The count includes tiles beyond the 256-tile request cap. Complete coverage means prepared images exist, including stale cache entries. It does not establish freshness or availability of remote data. Very large viewports can require zooming in when the provider minimum prevents further detail reduction.

Memory recovery first removes tiles outside the viewport. If visible geometry still exceeds the cache budget, it releases geometry and keeps 256-pixel tile images. At most 256 visible images require 64 MiB. Compact mode pauses extra tile fetching and reports limited object inspection. Changing the provider or memory budget restarts normal decoding. KML and CoT inspection remain available.

Regression tests cover dense geometry, input limits, minimum-level failure, parent-level recovery, and retry backoff during coverage audits. A low-memory case keeps every visible image while panning and restores object inspection after a budget increase. A large-viewport case prevents false complete-coverage reports. Loading coverage text is distinct from failure text in test assertions.

Viewport changes now schedule tile work immediately. A retry timer for an old failed tile no longer delays new visible tiles. Per-tile delays and provider-wide rate limits remain enforced. A regression moves from a failed tile to a valid tile while the old retry delay remains active.

Eight sanitizer suites passed in `build/quality/20261002-203549-quick-165645`. Static analysis passed in `build/quality/20261002-203551-analyze-3`. Coverage passed eight suites and all 37 native cases in `build/quality/20261002-203651-coverage-166089`. The vector fixture now accepts a valid three MiB response and rejects input above eight MiB.

Coverage reached 413 of 451 tile-controller lines and 209 of 213 MVT decoder lines. Remaining gaps include image allocation failure, some parser rejection branches, and existing vector-selection paths. Line coverage does not establish complete branch or input coverage.

All 37 native sanitizer cases passed in `build/quality/20261002-203636-gui-166003`. LeakSanitizer then reported the existing Mesa leak of 240732 bytes in 1073 allocations. The diagnostic remains failed, without suppression.

The 60-second sanitizer workload passed in `build/quality/20261002-203359-soak-165446`. It completed 1275 cycles with 8286208 bytes of peak memory growth. The maximum heartbeat gap was 46 milliseconds, and idle CPU time was 0.003 seconds. This short workload does not replace multi-hour qualification.

The release rebuild passed without compiler warnings with Qt 6.8.2 and GCC 14.2 on Debian 13. The executable remains `build/release/swak`, version 0.1.0. The build log is `build/coverage-fix-release.log`. Hardware graphics, native Wayland, Qt 6.4 compatibility, and multi-hour use remain untested. No external dependency was added.

## 2026-10-02: Keyboard configuration and Information dismissal

File > Settings now contains a Keybindings tab. Plugins add fields through `keySetting` and the existing `SettingsParticipant` interface. General and keyboard fields share one snapshot and one validation step per plugin. Plugin sections retain locale ordering. The profile retains the selected tab and section.

The Map section includes movement, zoom, reset, and alternate navigation keys. Q zooms in, and E zooms out, when the map has focus. Existing custom pan keys survive migration. New defaults that overlap older pan keys start disabled. The four continuous movement keys remain required. Other map bindings can be empty. Duplicate explicit map keys prevent Apply.

The Information section sets its dismissal key. Esc is the default. It clears the map selection and the selected CoT event, then hides Information. It also works in detached panes and when an empty Information pane is visible. Modal dialogs and popup menus retain their normal behavior. The plugin limits keyboard handling to widgets owned by its workspace.

The native regression covers rebinding, duplicate rejection, restart persistence, older profile migration, focus, detached panes, and modal Escape behavior. The existing Information regression also covers CoT selection removal. The native Keybindings screenshot shows the new tab without clipped controls at normal scale.

Eight sanitizer suites passed in `build/quality/20261002-205610-quick-168860`. Static analysis passed in `build/quality/20261002-205825-analyze-2`. All 38 native sanitizer cases passed in `build/quality/20261002-205756-gui-170221`. LeakSanitizer then reported the existing Mesa leak of 240732 bytes in 1073 allocations. The diagnostic remains failed, without suppression.

The release rebuild passed without compiler warnings. The executable is `build/release/swak`, version 0.1.0. The build log is `build/keybindings-release.log`. This task adds no external dependency. Hardware graphics, native Wayland, Qt 6.4 compatibility, and multi-hour use remain untested.

Coverage passed eight suites and 38 native cases in `build/quality/20261002-205817-coverage-170364`. Additional assertions cover alternate zoom keys and all four step directions. That focused native test passed at 200% display scaling. The screenshot is `build/keybindings-highdpi/keybindings-settings.png`. The final source coverage report is `build/keybindings-coverage/coverage.md`. Rebuilding the test changed its gcov checksum, so gcov replaced that test-only profile. Application source profiles retained the complete suite evidence.

Remaining coverage gaps include existing vector context-menu paths, non-left double-click delegation, and defensive fallback branches. Line coverage does not establish complete branch or input coverage. Static analysis found no unused application function that requires removal.

The 60-second sanitizer workload passed in `build/quality/20261002-210002-soak-171585`. It completed 1267 cycles with 8286208 bytes of peak memory growth. The maximum heartbeat gap was 37 milliseconds, and idle CPU time was 0.005 seconds. This workload does not replace multi-hour qualification.

## 2026-10-02: Map status and Settings labels

The bottom status bar now shows map cache size, zoom, pointer coordinates, tile diagnostics, and the certificate warning. A separator keeps this section apart from the event counts. The map no longer draws these diagnostics over its surface. Clickable attribution stays right-aligned, eight logical pixels above the lower map edge.

View widgets can implement the shared `StatusProvider` interface to create a status widget. The host owns that widget through the status bar. The map retains a guarded pointer. This keeps map-specific behavior out of the main window.

Settings field labels now remain on one line in General, Panes, Keybindings, and provider forms. Scrollable forms retain the full label width. Shared `SingleLineLabel` widgets keep notices and result messages on one line. They use an ellipsis when necessary and retain the full text in a tooltip. Embedded Map Providers and Data Providers use the same behavior.

The native regression checks the map status location, separator count, attribution margin, tooltip text, and Settings label policies. Native screenshots show the new status bar and Settings layout. Eight sanitizer suites passed in `build/quality/20261002-211227-quick-173446`. Static analysis passed in `build/quality/20261002-211311-analyze-2` after an explicit `QPointer::data()` return replaced an implicit conversion.

All 38 native sanitizer cases passed in `build/quality/20261002-211355-gui-174713`. LeakSanitizer then reported 102748 bytes in 457 allocations from the existing Mesa paths. The diagnostic remains failed, without suppression. The native screenshot is `build/quality/20261002-211355-gui-174713/map-status-bar.png`.

The release rebuild passed without compiler warnings. The executable remains `build/release/swak`, version 0.1.0. The build log is `build/status-layout-release.log`. This task adds no external dependency. Hardware graphics, native Wayland, Qt 6.4 compatibility, and multi-hour use remain untested.

Coverage passed eight suites and 38 native cases in `build/quality/20261002-211415-coverage-174856`. It executed all 16 lines in the shared single-line label. Settings dialog coverage reached 197 of 199 lines. Remaining gaps include defensive fallback branches and existing vector context-menu paths. Line coverage does not establish complete branch or input coverage.

The 60-second sanitizer workload passed in `build/quality/20261002-211545-soak-175793`. It completed 1280 cycles with 8327168 bytes of peak memory growth. The maximum heartbeat gap was 36 milliseconds, and idle CPU time was 0.005 seconds. This workload does not replace multi-hour qualification.

Focused native tests for map providers, Settings, and data providers passed at 200% display scaling. Screenshots in `build/status-layout-highdpi` show the single-line labels, separate map status section, and lower attribution. Tests used Qt 6.8.2, GCC 14.2, and software Mesa on Debian 13.


## 2026-10-02: Editable local markers

The Markers map tool adds local circles. M activates a crosshair cursor and guide lines at the pointer. A click opens Edit marker with an unsaved draft. Apply stores the title, description, properties, and position. Saved circles use the existing read-only Information pane. Right-click offers Edit, Move, and Delete. Esc cancels a move without changing the stored position. Delete and Backspace work on selected local markers in the map and Information pane.

Edit > Undo and Ctrl+Z restore deleted objects. Undo retains at most 100 deletions within the current session. Saved markers survive restarts through the preference system. Deleted identities and their storage remain reserved until their undo commands expire. This keeps Undo within the object count and memory limits.

The shared UserObjects model stores typed objects with stable identities and bounded JSON geometry. MapTools dispatches input, draws tool content, and handles cancellation and selected-object deletion. ObjectEditor supplies the reusable form. PluginServices exposes these services through optional pointers. The new plugin uses the existing pane, configuration, and map-tool interfaces. Its MANIFEST declares both roles. There are now fourteen built-in plugins.

Storage permits 256 objects and 128 KiB of formatted object data, including retained deletions. Each object permits 8192 bytes, 32 text properties, a 128-character title, and a 2048-character description. Geometry has depth and collection limits. Unknown object types retain bounded data for future plugins. Existing object identities cannot change type.

File > Settings exposes the creation key, circle radius, circle color, and guide opacity. The editor also retains its table header layout. The creation key accepts one unmodified letter. Cross-plugin shortcut conflicts remain a configuration limitation.

Eight sanitizer suites passed in `build/quality/20261002-222519-quick-184280`. Static analysis passed in `build/quality/20261002-222521-analyze-2`. Coverage passed eight suites and all 39 native cases in `build/quality/20261002-222714-coverage-184864`. Coverage executed all 84 UserObjects lines and all 18 MapTools lines. ObjectEditor reached 61 of 72 lines, and the marker plugin reached 165 of 168 lines. Remaining gaps include property delegate interaction, description truncation, the editor Escape fallback, invalid creation keys, and defensive failure paths. Line coverage does not establish complete branch or input coverage.

All 39 native sanitizer cases passed in `build/quality/20261002-222637-gui-184756`. LeakSanitizer then reported the existing Mesa leak of 240732 bytes in 1073 allocations. The diagnostic remains failed, without suppression.

The final 60-second sanitizer workload passed in `build/quality/20261002-223148-soak-185836`. It now includes repeated marker creation, deletion, Undo, and redo. It completed 924 cycles with 8847360 bytes of peak memory growth. The maximum heartbeat gap was 90 milliseconds, and idle CPU time was 0.016 seconds. This workload does not replace multi-hour qualification.

The focused native marker test passed at 200 percent scaling after the final pointer-position change. Screenshots in `build/markers-highdpi` show the editor, selection, and crosshair. The editor screenshot shows complete labels and controls. The release rebuild passed without compiler warnings. The executable remains `build/release/swak`, version 0.1.0. Its build log is `build/markers-release.log`.

Tests used Qt 6.8.2, GCC 14.2, and software Mesa on Debian 13. Hardware graphics, native Wayland, Qt 6.4 compatibility, and multi-hour use remain untested. This task adds no external dependency.


## 2026-10-02: Pointer guides and coordinate grid

Marker guides now use black at 50 percent opacity. Older profiles that retain the former 60 percent default migrate to 50 percent. Other custom opacity values survive. A style version prevents later changes from repeating this migration.

MapTool now separates stored content from transient overlays through paintOverlay and overlayChanged. The map hosts a transparent child widget for tool overlays. Pointer-only motion updates this widget without requesting tile or map-layer painting. The marker preview during movement also uses this surface. Changes to stored content still redraw the map. Pointer coordinates in the status bar continue to update during placement and movement.

The map draws a black coordinate grid at 20 percent opacity. Longitude labels appear at the top and bottom. Latitude labels appear at the left and right. Decimal precision follows rounded intervals from the 1, 2, and 5 sequence. Latitude spacing accounts for Mercator projection, which enlarges areas near the poles. Locale formatting supplies decimal separators. Each axis permits at most 512 lines per frame. File > Settings > General > Map controls visibility, opacity, and spacing.

The marker regression measures 20 pointer moves and permits at most two map paint events. It also requires overlay signals and measures the black guide alpha value. The focused native test passed at 200 percent scaling, including the final status-coordinate update. Screenshots in `build/grid-native` show the grid labels and marker guides. This test does not measure hardware cursor latency.

Eight sanitizer suites passed in `build/quality/20261002-224420-quick-187700`. Coverage passed eight suites and all 39 native cases in `build/quality/20261002-224908-coverage-190159`. Grid coverage reached 54 of 55 lines. The remaining line is the interval fallback. MapTools reached all 22 lines. Marker coverage reached 170 of 173 lines. Existing defensive failures and malformed-key paths remain untested. Line coverage does not establish complete branch or input coverage.

The 60-second sanitizer workload passed in `build/quality/20261002-225039-soak-190722`. It completed 1271 cycles with 11300864 bytes of peak memory growth. The maximum heartbeat gap was 35 milliseconds, and idle CPU time was 0.006 seconds. This short workload does not replace multi-hour qualification.

The release rebuild passed without compiler warnings. The executable remains `build/release/swak`, version 0.1.0. The build log is `build/grid-release.log`. Tests used Qt 6.8.2, GCC 14.2, and software Mesa on Debian 13. Hardware graphics, native Wayland, Qt 6.4 compatibility, and multi-hour use remain untested. This task adds no external dependency.


The final native sanitizer run passed all 39 functional cases in `build/quality/20261002-225224-gui-191008`. LeakSanitizer then reported the existing Mesa leak of 240732 bytes in 1073 allocations. The diagnostic remains failed, without suppression. Final static analysis passed in `build/quality/20261002-225226-analyze-2`.


## 2026-10-02: Build identity and marker mode

Application versions now include the first 12 Git SHA characters, such as `0.1.0+g123456789abc`. Help > About also shows the full SHA and an exact tag when available. Tag labels permit letters, digits, dots, underscores, plus signs, and hyphens. Other characters become underscores. Builds add `-dirty` for uncommitted tracked changes. Missing Git metadata produces `+unknown`. The application does not query a server for newer versions.

Ninja refreshes the generated version header on every build. CMake writes the header only when its contents change. Builds after commits and tag changes therefore refresh their identity without manual configuration. The final release must be rebuilt after the task commit. `tests/test_version.py` tests clean and dirty repositories, commit changes, exact tags, unchanged output timestamps, and missing metadata. Each diagnostic mode runs this test in a temporary repository.

The marker activation key now toggles an active tool off. The default key remains M. Auto-repeat does not toggle the mode. During movement, toggling off restores the original position. The status bar reserves an area for active map tools. The marker plugin supplies M during placement and movement, then clears it when the mode ends. The shared MapTool API supplies statusText and coordinateGridActive. The host does not inspect marker plugin state directly.

The coordinate grid now appears only during marker placement or movement. Its default opacity is 50 percent. Profiles with the old 20 percent default migrate once. Other custom opacity values survive. A saved style version permits later user changes back to 20 percent. Grid visibility remains configurable. The grid draws below data-feed and local markers so it does not darken their symbols.

Enter in the Title field now runs the same Apply method as the button. Invalid properties keep the editor open and preserve the draft. Valid details save and show regular Information. Empty titles still use New Marker.

Eight sanitizer suites passed in `build/quality/20261002-230352-quick-192583`. Final static analysis passed in `build/quality/20261002-230809-analyze-194847`. All 39 native sanitizer cases passed in `build/quality/20261002-230823-gui-195024`. LeakSanitizer then reported the existing Mesa leak of 240732 bytes in 1073 allocations. The diagnostic remains failed, without suppression. Native screenshots show the M indicator, coordinate labels, and full source revision in About.


Coverage passed eight suites and all 39 native cases in `build/quality/20261002-230931-coverage-195389`. MapTools reached all 31 lines. ObjectEditor reached 65 of 73 lines, and the marker plugin reached 174 of 177 lines. Remaining gaps include description truncation, editor fallback paths, and defensive marker failures. Line coverage does not establish complete branch or input coverage.

Focused native tests for markers, About, and grid opacity migration passed at 200 percent scaling. Screenshots are in `build/version-mode-native`. The release build log is `build/version-mode-release.log`. Tests use Qt 6.8.2, GCC 14.2, and software Mesa on Debian 13. Hardware graphics, native Wayland, Qt 6.4 compatibility, and multi-hour use remain untested. This task adds no external dependency.


The 60-second sanitizer workload passed in `build/quality/20261002-231127-soak-196231`. It completed 1259 cycles with 11198464 bytes of peak memory growth. The maximum heartbeat gap was 47 milliseconds, and idle CPU time was 0.003 seconds. This short workload does not replace multi-hour qualification.


## 2026-10-03: Marker activation after Apply

The new focus regression reproduced the reported failure before the fix. Apply hid Edit marker and opened Information without returning keyboard focus to the map. Qt selected another focus target from the pane layout. M only activates the marker tool when the map receives the key. Earlier tests sent keys directly to the map and therefore missed this failure.

After a successful Apply, the marker plugin selects the saved object, activates the map window, and returns focus to the visible map. It uses the existing guarded map pointer. No global shortcut or delayed focus callback was added. Failed validation still leaves the editor active. Enter and the Apply button share the same completion path.

The regression sends keys to QApplication::focusWidget. It repeats creation six times for each combination of docked and detached editor and Information panes. Each layout alternates Enter and Apply. The test also types M in the title without activating marker mode. All four layouts passed after the fix. The existing marker workflow test also passed.


Eight sanitizer suites passed in `build/quality/20261003-075939-quick-199146`. Static analysis passed in `build/quality/20261003-075856-analyze-198992`. All 43 native sanitizer cases passed in `build/quality/20261003-080115-gui-199663`. LeakSanitizer then reported the existing Mesa leak of 240732 bytes in 1073 allocations. The diagnostic remains failed, without suppression.


Coverage passed eight suites and all 43 native cases in `build/quality/20261003-080151-coverage-199956`. Marker coverage reached 180 of 183 lines and exercised the new focus-return path. Remaining gaps include existing defensive marker failures. The null or hidden map guard also retains untested branches. Line coverage does not establish complete branch or input coverage.

All four focus cases also passed at 200 percent scaling. The release build log is `build/marker-focus-release.log`. Tests use Qt 6.8.2, GCC 14.2, and software Mesa on Debian 13. Hardware graphics, native Wayland, Qt 6.4 compatibility, and multi-hour use remain untested. This task adds no external dependency.


The 60-second sanitizer workload passed in `build/quality/20261003-080333-soak-200644`. It completed 1265 cycles with 11272192 bytes of peak memory growth. The maximum heartbeat gap was 38 milliseconds, and idle CPU time was 0.008 seconds. This short workload does not replace multi-hour qualification.

## 2026-10-03: Map overlays and marker appearance

All attached panes now use a shared overlay host above the map. Opening and closing panes leave map dimensions and coordinates unchanged. Existing tabs, resizing, detachment, menu actions, and focus behavior remain available. The host saves overlay relationships separately from the outer toolbar layout. A native test covers migration from the older layout format.

The marker editor now shows Type after Title, followed by eight color buttons. Marker remains the only available type. Each record stores its own bounded color string. Older records without color use orange. New records use the configured default color. Apply, cancellation, movement, deletion undo, and restart retain the expected color.

The initial interrupted work remained in the working tree. Native testing found that QMainWindow needed an explicit Qt::Widget flag for the overlay host. Screenshot review also found unreadable transparent pane titles. The host now paints the masked region with the current theme background. Tests wait for queued mask updates before capturing the map.

Eight sanitizer suites passed in `build/quality/20261003-110857-quick-209833`. Final analysis passed in `build/quality/20261003-111636-analyze-3`. All 46 native sanitizer cases passed in `build/quality/20261003-111635-gui-212162`. LeakSanitizer then reported the existing Mesa leak of 240732 bytes in 1073 allocations. The diagnostic remains failed without suppression. The tile-rendering test now permits 15 seconds because the full map viewport requires more tiles. Coverage passed eight suites and all 46 native cases in `build/quality/20261003-111728-coverage-212881`.

The overlay host reached 45 of 45 source lines. UserObjects reached 87 of 87 lines. ObjectEditor reached 91 of 99 lines, and Markers reached 181 of 184 lines. Remaining gaps include property-editor callbacks, description truncation, metadata sorting, and defensive marker failures. Line coverage does not establish complete branch or input coverage.

Layout migration, overlay geometry, marker appearance, and all four focus cases passed at 200 percent scaling. Screenshots are in `build/overlays-native`. Tests use Qt 6.8.2, GCC 14.2, and software Mesa on Debian 13. Hardware graphics, native Wayland, Qt 6.4 compatibility, and two-hour qualification remain untested. This task adds no external dependency.

The 60-second sanitizer workload passed in `build/quality/20261003-111945-soak-213547`. It completed 905 cycles with 7811072 bytes of peak memory growth. The maximum heartbeat gap was 61 milliseconds, and idle CPU time was 0.025 seconds. This short workload does not replace two-hour qualification. The release build log is `build/overlays-release.log`.


## 2026-10-03: Marker pointer latency

The latency regression reproduced a deferred guide repaint before the fix. The guides also used coordinates from the last mouse event. The OS cursor can move beyond those coordinates before Qt paints the overlay.

Pointer-only changes now request an immediate overlay repaint. Qt can defer painting during OpenGL composition, as the 200 percent scale test demonstrated. The host passes the current cursor position to the shared paintOverlay API. Marker guides and movement previews use that position.

Placement still uses the click event coordinates. Full map painting stays outside the pointer-only path. Display composition can still add latency after application painting.

The regression sends an older mouse position while the cursor occupies a newer position. It requires prompt painting, guide pixels at the current cursor, no full map repaint, and no idle repaint loop.

Eight sanitizer suites passed in `build/quality/20261003-113641-quick-215167`. All 47 native sanitizer cases passed in `build/quality/20261003-114309-gui-216990`. LeakSanitizer reported the existing Mesa leak of 240732 bytes in 1073 allocations. The diagnostic remains failed without suppression.

Coverage passed eight suites and all 47 native cases in `build/quality/20261003-113809-coverage-215824`. MapTools reached all 31 lines. The map plugin reached 546 of 553 lines, and Markers reached 181 of 184 lines. Remaining gaps include existing drag-navigation branches and defensive marker failures. The new overlay painting path ran. Line coverage does not establish complete branch or input coverage.

Source analysis passed in `build/quality/20261003-114030-analyze-216612`. The 60-second sanitizer workload passed in `build/quality/20261003-113926-soak-216458`. It completed 1271 cycles with 11337728 bytes of peak memory growth. The maximum heartbeat gap was 35 milliseconds, and idle CPU time was 0.008 seconds. This short workload does not replace two-hour qualification.

The latency, marker workflow, and four focus cases also passed at 200 percent scaling. Tests use Qt 6.8.2, GCC 14.2, and software Mesa on Debian 13. Actual display latency, hardware graphics, native Wayland, and Qt 6.4 remain untested. This change adds no dependency. The release build log is `build/pointer-release.log`.


## 2026-10-03: Separate map status fields

The map status now contains separate widgets for cache size, zoom, pointer coordinates, tile diagnostics, and the HTTPS warning. QFrame separators replace pipe characters in the combined text. Empty diagnostics and warnings hide their whole sections. Each label keeps plain text, one-line rendering, and a tooltip with the full value.

The map still uses the shared StatusProvider interface. Its returned widget contains the sections, and the status bar owns that widget. Guarded pointers protect updates if labels disappear first. No new dependency or background task was added.

Focused native tests passed for status fields, certificate-warning visibility, and both workspace themes. Eight sanitizer suites passed in `build/quality/20261003-130700-quick-224369`. All 47 native sanitizer cases passed in `build/quality/20261003-130805-gui-224935`. LeakSanitizer reported the existing Mesa leak of 240732 bytes in 1073 allocations. The diagnostic remains failed without suppression.

Coverage passed eight suites and all 47 native cases in `build/quality/20261003-130825-coverage-225093`. The map plugin reached 574 of 581 lines. The new status sections ran. Existing drag-navigation paths and some guarded-pointer branches remain untested. Line coverage does not establish complete branch or input coverage. Source analysis passed in `build/quality/20261003-131043-analyze-225818`.

The 60-second sanitizer workload passed in `build/quality/20261003-130939-soak-225684`. It completed 1263 cycles with 11227136 bytes of peak memory growth. The maximum heartbeat gap was 36 milliseconds, and idle CPU time was 0.008 seconds. This short workload does not replace two-hour qualification.

Provider-status and workspace tests passed at 200 percent scaling. Screenshots are in `build/overlays-native`. Tests use Qt 6.8.2, GCC 14.2, and software Mesa on Debian 13. Hardware graphics, native Wayland, and Qt 6.4 remain untested. The release build log is `build/status-sections-release.log`.

## 2026-10-03: ATAK format references and vector symbols

The latest task reviews ATAK-CIV for factual references without copying source files or artwork. The inspected commit is `9f6893dd657feacc35ec5de03dad721c2e44170e`. A temporary clone remains outside this repository. Read [the reference review](atak-reference-review.md) for source paths, public specifications, and scope.

The image provider now supports TMS row inversion and quadkey addressing through the existing image pipeline. An optional shared `MapProvider::additionalSources` method supplies additional presets. Stable remembered identifiers prevent deleted presets from returning after restart. Missing plugins retain their source records. The separately researched USGS Topo preset uses its public HTTPS tile service and starts inactive. A live request returned a 256-pixel JPEG tile.

A shared vector catalog provides sixteen original ground-unit symbols based on MIL-STD-2525C conventions. Four identities combine with generic unit, infantry, armor, and medical roles. Local markers and received CoT events use the same vector paths. Unsupported CoT types remain circles. This is a limited subset, not a compliance claim. No new dependency, font, or imported artwork was added.

Local records store a bounded symbol identifier beside the existing color. Unknown saved identifiers remain intact. Apply, cancellation, deletion undo, movement, selection, and restart use the existing object services. Marker sizes remain unchanged. CoT selection now uses the symbol frame instead of adding a circular outline.

The first full GUI run exposed an outdated toolbar count after the additional preset. The test now derives the count from registered sources. Source analysis requested standard search algorithms in three loops. Those loops now use `std::find_if`. Focused symbol and toolbar tests passed at 200 percent scaling.

The 60-second sanitizer workload passed in `build/quality/20261003-133859-soak-231845`. It completed 1255 cycles with 11411456 bytes of peak memory growth. The maximum heartbeat gap was 36 milliseconds, and idle CPU time was 0.003 seconds. This short workload does not replace two-hour qualification.

Eight sanitizer suites passed in `build/quality/20261003-134128-quick-232481`. All 48 native sanitizer cases passed in `build/quality/20261003-134225-gui-233188`. LeakSanitizer reported the existing Mesa leak of 240732 bytes in 1073 allocations. The GUI diagnostic remains failed without suppression. Source analysis passed in `build/quality/20261003-134355-analyze-234572`.

Coverage passed eight suites and all 48 native cases in `build/quality/20261003-134245-coverage-233346`. The shared symbols reached 54 of 54 lines, and CoT Events reached 146 of 146 lines. The image provider reached 42 of 43 lines. Its early return for invalid common fields remains unexecuted in that provider. Existing property-editor callbacks, generic map settings fields, and defensive marker failures also retain gaps. Line coverage does not establish complete branch or input coverage.

Tests use Qt 6.8.2, GCC 14.2, and software Mesa on Debian 13. Hardware graphics, native Wayland, Qt 6.4 compatibility, and two-hour qualification remain untested. Screenshots are under `build/atak-native` and the coverage report directory. The release build log is `build/atak-release.log`. The final release metadata refresh uses `build/atak-release-final.log`.

## 2026-10-03: Marker palette and generic type

The marker editor adds the four distinct colors from the shared symbol catalog to its eight general colors. Symbol selection and reopening a saved marker now select the matching color button. The palette reads catalog colors directly, which prevents separate color definitions from drifting apart.

The circle type now appears as Generic marker in the editor and Information panel. Its selection entry has a circle icon from the same drawing helper as the other entries. Saved type identifiers and marker colors remain unchanged. No plugin contract or external dependency changed.

The existing marker tests now cover all four symbol colors through Apply and reopening. They also require a Generic marker icon and all twelve color buttons. Focused tests passed at normal and 200 percent scaling. Native screenshot review found no clipped color buttons or labels.

The USGS Topo preset now defaults to maximum zoom 8 at the user's request. Existing saved provider records retain their values. Metadata alone does not establish tile availability at higher levels. The provider test requires the new default.

Eight sanitizer suites passed in `build/quality/20261003-135106-quick-235887`. All 48 native sanitizer cases passed in `build/quality/20261003-135210-gui-236419`. LeakSanitizer then reported the existing Mesa leak of 240732 bytes in 1073 allocations. The GUI diagnostic remains failed without suppression. The focused sanitizer test also passed after the USGS default changed to 8.

Coverage passed eight suites and all 48 native cases in `build/quality/20261003-135230-coverage-236588`. ObjectEditor reached 123 of 131 lines. The new palette and icon paths ran. Existing property-editor callbacks, description truncation, property sorting, and defensive marker failures retain gaps. Line coverage does not establish complete branch coverage. Source analysis passed in `build/quality/20261003-135454-analyze-237550`.

The 60-second workload passed in `build/quality/20261003-135344-soak-237266`. It completed 1260 cycles with 11325440 bytes of peak memory growth. The maximum heartbeat gap was 66 milliseconds, and idle CPU time was 0.007 seconds. This short run does not replace two-hour qualification. Hardware graphics, native Wayland, and Qt 6.4 remain untested. Tests use Qt 6.8.2 and software Mesa. The release build log is `build/palette-release-final.log`.

## 2026-10-03: Events and CoT Messages names

The application now calls its diagnostic history Events and its CoT pane CoT Messages. The names also appear in Settings, menus, toolbar labels, status counters, tooltips, and plugin metadata. Current user guides and existing label tests use these names.

Plugin identifiers, preference keys, and widget identifiers remain stable. Saved layouts and table configuration retain their existing associations. This change adds no dependency or interface contract.

Eight sanitizer suites passed in `build/quality/20261003-135939-quick-238080`. All 48 native sanitizer cases passed in `build/quality/20261003-140043-gui-238705`. LeakSanitizer reported the existing Mesa leak of 240732 bytes in 1073 allocations. The GUI diagnostic remains failed without suppression. Native screenshots show the new pane, toolbar, and status names without clipping.

Coverage passed eight suites and all 48 native cases in `build/quality/20261003-140114-coverage-238975`. The renamed labels ran through the existing tests. This text-only change adds no branches. Previously recorded property-editor and defensive-path gaps remain. Source analysis passed in `build/quality/20261003-140338-analyze-239810`.

The 60-second workload passed in `build/quality/20261003-140230-soak-239648`. It completed 1254 cycles with 11321344 bytes of peak memory growth. The maximum heartbeat gap was 72 milliseconds, and idle CPU time was 0.007 seconds. This does not replace two-hour qualification. Tests use Qt 6.8.2 and software Mesa. Hardware graphics, native Wayland, and Qt 6.4 remain untested. The release build log is `build/event-names-release-final.log`.


## 2026-10-03: Marker collections

Tools > Marker Collections and File > Settings > Marker collections now share a collection manager. The table supports activation, copying, metadata edits, deletion, import, and export. Core supplies Generic marker. A separate MILSTD2525 plugin supplies the existing sixteen military vector symbols. This remains a limited subset of the standard.

The marker editor places Collection before Type and remembers both choices. Colors come from registered collections. Existing markers retain collection references through movement, deletion undo, and restart. Disabling a collection prevents new placement from that catalog but preserves existing icons. Missing definitions use a saved-color circle. Reimporting the original collection restores its icons. Valid saved definitions work without the original plugin.

The new MarkerCollectionProvider role supplies the same bounded JSON schema as imported archives. PluginServices exposes the per-window collection service. Local rendering, selection, and editor previews use shared vector paths. Received CoT messages retain their existing symbol behavior. Read [the API](plugin-api.md) and [the collection manual](marker-collections.md) for contracts and archive examples.

Import accepts ZIP, TAR, and gzip-compressed TAR. Export writes ZIP or compressed TAR through the existing libarchive dependency. Archives contain one collection.json member and no executable content. Imports reject links, extra members, invalid paths, oversized data, malformed vectors, and duplicate identifiers. No archive files are extracted. Export uses atomic writes and private file permissions. Native file dialogs resolve the default suffix before replacement confirmation.

Eight sanitizer suites passed in `build/quality/20261003-144842-quick-247859`. All 50 native sanitizer cases passed in `build/quality/20261003-145018-gui-248574`. LeakSanitizer then reported the existing Mesa leak of 240732 bytes in 1073 allocations. The GUI diagnostic remains failed without suppression. Source analysis passed in `build/quality/20261003-145005-analyze-2`.

Coverage passed eight suites and all 50 native cases in `build/quality/20261003-145106-coverage-249342`. Collection storage and archives reached 178 of 179 lines. The collection manager reached 88 of 90 lines. The military catalog reached all 23 lines. Tests cover import errors, success after failure, cancellation, missing plugins, disabled collections, placement, metadata changes, and restart.

The archive writer failure after a successful open remains untested. Allocation failures, the export filter callback, the double-click edit shortcut, and catalog changes during an open marker draft retain gaps. Existing property editor callbacks also retain gaps. Line coverage does not establish complete branch or input coverage. The static-analysis review found no dead-code diagnostic.

The release build succeeded with Qt 6.8.2 and GCC 14.2. Its log is `build/collections-release.log`. The final metadata build uses `build/collections-release-final.log`. Hardware graphics, native Wayland, Qt 6.4 compatibility, and two-hour qualification remain untested.

The 60-second sanitizer workload passed in `build/quality/20261003-160617-soak-251322`. It completed 1187 cycles with 12238848 bytes of peak memory growth. The maximum heartbeat gap was 39 milliseconds, and idle CPU time was 0.003 seconds. Each cycle creates and removes a collection. This short workload does not replace two-hour qualification.

Four focused collection and marker cases passed at 200 percent scaling. Native screenshots show the collection manager and marker editor without clipped controls. Screenshots are under `build/atak-native`.


## 2026-10-03: Authenticated map sources

The shared map API now supports HTTP Basic, bearer tokens, and API keys in X- headers or query parameters. Every existing map format uses the same request helper. Map Providers exposes authentication metadata through Edit and a separate masked Credentials dialog. Session secrets never enter the profile. Optional SWAK_MAP_ environment references support restart use without plaintext secrets in preferences.

Authenticated requests require valid certificates and the configured server and port. Redirects remain blocked. Authenticated tiles bypass the disk cache and fetch visible tiles only. Source and secret changes cancel old requests and clear decoded images. HTTP 401 and 403 responses use the provider-wide retry delay. No password vault, OAuth login, token renewal, or client certificate support is implemented.

The further ATAK review found bearer authentication for its configured catalog and HTTP Basic for imagery. It did not reveal an additional reusable public catalog in the reviewed paths. No ATAK code or assets enter this repository. The updated [reference review](atak-reference-review.md) records sources and service limits.

Five inactive presets add USGS Imagery, USGS Imagery Topo, USGS Shaded Relief, Thunderforest Outdoors, and Thunderforest OpenCycleMap. Three public USGS services returned image tiles during live checks. Imagery Topo returned 404 at level zero and images at levels one and two. Its minimum is one. The imagery maximum is eight, and shaded relief stops at seven. Thunderforest needs a user API key. Its templates retain both required attribution links and the SWAK_MAP_THUNDERFOREST_KEY reference.

The common Settings form now selects the first declared choice for an absent field, before locale sorting. This prevents legacy map records from opening with an authentication method selected by alphabetical order. Existing authentication metadata stays portable when plugins are absent. Ordinary provider fields remain visible in profiles, so they must not contain secrets.

Eight sanitizer suites passed in `build/quality/20261003-200513-quick-7275`. All 51 native sanitizer cases passed in `build/quality/20261003-200659-gui-8606`. LeakSanitizer then reported the existing Mesa leak of 240732 bytes in 1073 allocations. The GUI diagnostic remains failed without suppression. Static analysis passed in `build/quality/20261003-200514-analyze-2`.

The local HTTPS tests trust a test-only certificate and restore the original trust configuration afterward. Tests cover missing secrets, all authentication encodings, query escaping, header injection, credential replacement, origin changes, environment references, profile exclusion, redirects, and 401 responses. Native tests cover masked input, cancellation, clearing, legacy defaults, and provider rendering. The release build succeeded in `build/auth-release.log`. The final metadata build uses `build/auth-release-final.log`.

Coverage passed eight suites and all 51 native cases in `build/quality/20261003-200746-coverage-8791`. The authentication helper reached 66 of 67 lines. Its missing malformed-field rejection prompted additional tests. MapManager reached 135 of 137 lines. Remaining gaps include the existing table double-click shortcut and some defensive callback branches. Line coverage does not establish complete branch or input coverage.

The 60-second sanitizer workload passed in `build/quality/20261003-200941-soak-10095`. It completed 1207 cycles with 12210176 bytes of peak memory growth. The maximum heartbeat gap was 36 milliseconds, and idle CPU time was 0.007 seconds. This short run does not replace two-hour qualification.

Credential and provider GUI tests also passed at 200 percent scaling. Screenshots are under `build/atak-native`. Tests use Qt 6.8.2, GCC 14.2, and software Mesa. Hardware graphics, native Wayland, Qt 6.4, production credentials, token expiration against a real provider, and two-hour qualification remain untested. No external dependency was added.

The final sanitizer run also passed all eight suites in `build/quality/20261003-201131-quick-10344`. It adds malformed authentication field inputs and assertions for all five new presets.

The final coverage run passed eight suites and all 51 native cases in `build/quality/20261003-201245-coverage-10682`. The authentication helper reached all 67 lines and all eight functions. Defensive branches and real provider behavior still require further testing.


## 2026-10-03: Marker menu activation

Tools > Plugins now contains Marker instead of the Edit marker pane toggle. The command activates placement and focuses the map. A map click opens the existing editor. The editor title remains Edit marker. The configured marker key still toggles mode, and Escape cancels placement.

PanePlugin now has an optional primaryAction method. The host uses its pane-owned action instead of the pane toggle. MapTool and MapTools expose a shared activation request, and the map supplies its widget. Menu and keyboard activation use the same marker setup code. Other plugin pane toggles retain their behavior. Read [Plugin API](plugin-api.md) for the added contract.

Focused native tests passed for menu activation, cancellation, placement, saving, repeated activation, and the six remaining pane toggles. Source analysis passed in `build/quality/20261003-202203-analyze-2`. The initial release build succeeded in `build/marker-menu-release.log`. The final metadata build uses `build/marker-menu-release-final.log`.

Eight sanitizer suites passed in `build/quality/20261003-202258-quick-12877`. All 52 native sanitizer cases passed in `build/quality/20261003-202443-gui-13827`, including the actual Marker menu click. LeakSanitizer then reported the existing Mesa leak of 240732 bytes in 1073 allocations. The GUI diagnostic remains failed without suppression.

Coverage passed eight suites and all 52 native cases in `build/quality/20261003-202600-coverage-14044`. MapTools reached all 32 lines and all 14 functions. Markers reached 209 of 212 lines and all 47 functions. The new primary action and activation paths ran. Existing defensive marker and configuration paths retain gaps. Line coverage does not establish complete branch coverage.

The 60-second sanitizer workload passed in `build/quality/20261003-202751-soak-15036`. It completed 1200 cycles with 12247040 bytes of peak memory growth. The maximum heartbeat gap was 66 milliseconds, and idle CPU time was 0.004 seconds. This short workload does not replace two-hour qualification. Tests use Qt 6.8.2 and software Mesa. Hardware graphics, native Wayland, and Qt 6.4 remain untested. No new dependency was added.

The Marker command uses the same menu dispatch as pane commands. This closes the menu before activation and handles clicks at 200 percent scaling. Focused marker and pane tests passed at that scale. An earlier test exposed a dropped click through the default submenu handler. The final test also asserts that the action fires once.

The final native sanitizer run passed all 52 functional cases in `build/quality/20261003-203754-gui-16622`. LeakSanitizer still reports the existing Mesa leak of 240732 bytes in 1073 allocations. The GUI diagnostic remains failed without suppression.


## 2026-10-03: Polygon creation and editing

The Polygons plugin adds P and Tools > Plugins > Polygon. Creation uses the shared activation path, crosshair, black guides, and coordinate grid. Clicks add small blue corners and edges. The preview fills the closed area through the pointer with 30 percent blue. Clicking the first corner completes a valid outline. Enter closes three or more placed corners. With two placed corners, Enter uses the pointer as a third corner.

ObjectEditor supplies Title, Polygon type, the shared color palette, Description, and properties. Apply commits the draft. Title Enter also applies. Completed shapes hide their corner dots. Saved shapes show a centered title and open Information on an interior click. Selection adds a solid white outline. Concave shapes use an interior label position when their centroid lies outside the area.

Edit shows corner dots. A corner context menu offers Move and Delete. Invalid moves retain the previous draft geometry. Invalid deletions are disabled. Escape cancels a move before it cancels the draft. Delete and Backspace remove selected polygons, and the shared undo action restores them. P toggles creation, while M and P switch tools through the shared registry.

Shared validation accepts 3 to 64 distinct corners within the displayed world. It rejects crossing edges, overlapping edges, zero area, and malformed coordinates. Existing object-count, record, profile, and undo budgets remain in force. The Area property is reserved. Polygon records survive restart and do not require the plugin for storage preservation.

Area uses bounded spherical integration along the displayed Mercator edges. It is an estimate rather than an ellipsoidal survey result. Locale controls number formatting and metric or imperial units. The user guide documents thresholds, controls, and the single-world geometry limit. No external dependency was added.

MapTool adds interactionActive for event priority and dismiss for one-step cancellation. ObjectEditor adds validated draft geometry replacement and error reporting. MapItem adds a polygon outline style. The API and architecture guides describe these changes. Every plugin still uses the shared API and an embedded MANIFEST.

The final quick sanitizer run passed all eight suites in `build/quality/20261003-222408-quick-24299`. Source analysis passed in `build/quality/20261003-222440-analyze-24726`. All 54 native sanitizer cases passed in `build/quality/20261003-222647-gui-25170`. LeakSanitizer still reports the existing Mesa leak of 240732 bytes in 1073 allocations. The GUI diagnostic remains failed without suppression.

Focused polygon and marker tests passed at 200 percent scaling. Native screenshots cover the editor, preview, and actual OpenGL frame. The frame shows the title, fill, and selection outline. Tests also cover vertex edits, blocked changes, context deletion, undo, properties, shortcut changes, 64-corner limits, repeated cancellation, and restart.

The initial release build succeeded in `build/polygon-release.log`. The final metadata build uses `build/polygon-release-final.log`. Tests use Qt 6.8.2, GCC 14.2, and software Mesa. Hardware graphics, native Wayland, Qt 6.4, allocation failures, and two-hour qualification remain untested. Existing property-editor callbacks and defensive branches retain coverage gaps. Line coverage does not establish complete branch or input coverage.

The 60-second sanitizer workload passed in `build/quality/20261003-222737-soak-25381`. It completed 1188 cycles with 13459456 bytes of peak memory growth. The maximum heartbeat gap was 41 milliseconds, and idle CPU time was 0.005 seconds. Each cycle includes polygon storage, area calculation, deletion, and undo. This short workload does not replace two-hour qualification.

Final coverage passed all eight suites and 54 native cases in `build/quality/20261003-222934-coverage-25615`. Polygon helpers reached all 84 lines and eight functions. The polygon plugin reached 215 of 217 lines and all 51 functions. Its two unexecuted lines are closing braces. MapTools reached all 40 lines and 16 functions. ObjectEditor reached 205 of 210 lines. Its remaining gaps cover collection changes during a draft and description truncation. Defensive branches and nearly coincident floating-point geometry need further input testing.


## 2026-10-03: Polygon selection outline

Selected polygons now use their configured line width for the white outline. The previous renderer added two pixels, which doubled the default width from two to four pixels. Point and route selection retain their existing widths. The outline remains constant during zoom.

Eight sanitizer suites passed in `build/quality/20261003-223556-quick-26949`. Source analysis passed in `build/quality/20261003-223600-analyze-27046`. All 54 native cases passed in `build/quality/20261003-223708-gui-27445`. The existing Mesa leak still makes the GUI sanitizer diagnostic fail. Coverage passed eight suites and 54 native cases in `build/quality/20261003-223747-coverage-27618`. The native OpenGL frame shows the thinner white outline. The final release build log is `build/outline-release-final.log`.

The 60-second workload passed in `build/quality/20261003-223923-soak-28209`. It completed 1169 cycles with 13463552 bytes of peak memory growth. The maximum heartbeat gap was 37 milliseconds, and idle CPU time was 0.003 seconds. Two-hour qualification and hardware graphics remain untested.


## 2026-10-03: High-zoom OpenGL strokes

The earlier outline-width change did not fix the reported rendering fault. A native OpenGL regression reproduced it at zoom levels 12 and 19. The old renderer covered every pixel of a 320 by 240 view with the selection stroke. Low-zoom screenshots did not expose this fault.

The shared selection renderer applied the large world-to-screen scale to QPainter while it used cosmetic pens. It now transforms the path into screen coordinates before Qt draws the stroke. Pen widths use logical screen pixels and follow display scaling. Offscreen wrapped paths skip conversion. The correction covers local polygons, imported polygons, and selected vector lines and areas, including OpenFreeMap geometry.

The KML/KMZ feed renderer used the same drawing pattern. It now uses screen-coordinate paths too. Point selections and vector tile preparation already used screen coordinates. No plugin contract, saved format, dependency, or network behavior changed.

New native tests cover solid polygons, dashed polygons, selected lines, and KML lines and polygons at zoom levels 2, 12, and 19. They compare total stroke coverage against an independent screen-coordinate raster reference. The tests allow small antialiasing differences between raster and OpenGL backends. Focused runs passed at normal and 200 percent scaling. Hardware graphics and native Wayland remain untested.

Eight sanitizer suites passed in `build/quality/20261003-225501-quick-30339`. Source analysis passed in `build/quality/20261003-225506-analyze-30450`. All 69 native cases passed in `build/quality/20261003-225828-gui-30875`. LeakSanitizer still reports the existing Mesa leak of 240732 bytes in 1073 allocations. The GUI sanitizer diagnostic remains failed without suppression.

Coverage passed eight suites and 69 native cases in `build/quality/20261003-225912-coverage-31078`. Selection drawing reached all 27 lines. Data sources reached 297 of 298 lines. The remaining line reports skipped network links and unsupported overlays after a download. Branch coverage remains incomplete, including allocation failures and defensive paths. These results do not establish complete input coverage.

The 60-second workload passed in `build/quality/20261003-230048-soak-31725`. It completed 1195 cycles with 14532608 bytes of peak memory growth. The maximum heartbeat gap was 36 milliseconds, and idle CPU time was 0.003 seconds. Two-hour qualification remains untested.

The release build succeeded in `build/selection-release.log`. The final build with committed version metadata uses `build/selection-release-final.log`.

## 2026-10-03: Polygon corner colors

Polygon corner dots now use the current polygon color during editing and corner moves. Color changes update the dots immediately. New polygons still start with blue dots. The existing polygon workflow test inspects dot colors after reopening a saved green polygon and changing its color.

No plugin contract, stored format, or dependency changed. The change adds no allocations or object ownership. Existing hardware graphics, native Wayland, and two-hour qualification gaps remain.

Eight sanitizer suites passed in `build/quality/20261003-230537-quick-32348`. Source analysis passed in `build/quality/20261003-230539-analyze-3`. All 69 GUI cases passed in `build/quality/20261003-230701-gui-32876`. The dot screenshot shows the selected red color. The GUI sanitizer diagnostic still fails on the existing Mesa leak of 240732 bytes in 1073 allocations.

Coverage passed eight suites and 69 GUI cases in `build/quality/20261003-230725-coverage-33094`. The polygon plugin reached 215 of 217 lines and all 51 functions. Its two unexecuted lines are closing braces. Defensive branches and allocation failures retain coverage gaps. The release build passed in `build/corner-release.log`. The final build with committed version metadata uses `build/corner-release-final.log`.

The 60-second workload passed in `build/quality/20261003-230841-soak-33718`. It completed 1190 cycles with 13541376 bytes of peak memory growth. The maximum heartbeat gap was 40 milliseconds, and idle CPU time was 0.003 seconds. This short run does not replace two-hour qualification.


## 2026-10-03: Overlapping map objects

The map now gathers all matching objects before it selects one. Multiple hits open an object chooser for left and right clicks. Left click selects the chosen object. Right click opens that object's context menu. Single hits act directly. Cancellation preserves the previous selection. Content changes close the menu to avoid stale actions.

The chooser includes local markers, polygons, CoT messages, KML/KMZ data, and vector features. Rows show numbers, labels, and types or sources. Labels escape menu shortcut characters. More objects opens another page of 30 actions. Pages create actions only when opened. The old vector limit of 12 hits no longer hides objects. Stored geometry and parser limits still bound the candidate data.

MapTool adds itemsAt and interact. MapTools aggregates hits and routes chosen IDs back to their owner. Markers and Polygons use the same interface and re-read records before acting. Active creation, movement, and polygon corner editing retain event priority. DataSources and TileRenderer replace itemAt with itemsAt. The unused first-hit API was removed after source analysis. Plugin contracts, architecture, usage, and affected MANIFEST versions were updated. No external dependency or saved schema changed.

Tests cover marker-marker, polygon-polygon, and marker-polygon overlaps, cancellation, model changes during a menu, pagination, direct single hits, and empty clicks. Context tests delete the chosen marker and polygon. Existing CoT/data and vector tests now make explicit choices. Feed tests cover multiple matching sources. Vector tests compare geometry candidates with feature hits. Hardware graphics, native Wayland, and two-hour qualification remain untested.

The final sanitizer run passed eight suites in `build/quality/20261003-232823-quick-38151`. Source analysis passed in `build/quality/20261003-232435-analyze-4`. The final native run passed all 70 cases in `build/quality/20261003-232917-gui-38505`. LeakSanitizer still reports the existing Mesa leak of 240732 bytes in 1073 allocations. The GUI diagnostic remains failed without suppression.

The 60-second workload passed in `build/quality/20261003-232640-soak-37848`. It completed 1184 cycles with 13467648 bytes of peak memory growth. The maximum heartbeat gap was 58 milliseconds, and idle CPU time was 0.007 seconds. This short run does not replace two-hour qualification.

The release build passed in `build/overlap-release.log`. The final build with committed version metadata uses `build/overlap-release-final.log`.

Final coverage passed eight suites and 70 GUI cases in `build/quality/20261003-232942-coverage-38680`. MapTools reached 48 of 49 lines and all 19 functions. Its remaining line is a closing brace. The map plugin reached 648 of 649 lines. Its remaining line is a defensive switch default. Markers reached all 49 functions, and Polygons reached all 52 functions. Missing-record branches, allocation failures, and hardware-specific paths retain coverage gaps. Line coverage does not establish complete input coverage.

The overlap workflow also passed at 200 percent scaling in `build/overlap-hidpi.log`. It covers pagination and dragging through an overlap. Native screenshots show the chooser's labels and object types.


## 2026-10-04: Compact toolbar and Area mode

The object chooser now shows object labels and icons without a heading, numbering, or type suffixes. Tooltips retain type and source details. Local markers use their collection symbol and selected color. Areas use the shared area icon. MapItem adds an optional icon through the shared plugin API.

The main toolbar uses icon-only controls with separators. It no longer shows plugin names. Marker and Area share their pane primary actions with Tools > Plugins and the toolbar. Their checked state tracks active modes. Clicking an active button stops that mode. Tooltips show the configured shortcut. The provider selector uses a globe icon and names its active provider in a tooltip.

Area replaces Polygon in user-facing tool, editor, and mode labels. A activates Area mode. Arrow keys now provide continuous panning in all four directions. Saved defaults migrate from W/A/S/D and P. Other custom bindings remain intact. Navigation and area shortcut version fields make migration run once. The stored polygon type, plugin ID, and object names stay stable for profile compatibility.

Six command icons come from Lucide 0.468.0. Their original SVG files and Lucide/Feather notices are under third_party/lucide. The application draws generated Qt vector paths through a shared icon engine. FontTools 4.55.3 converts the bundled assets during development only. Normal builds require no new runtime dependency. Palette colors and vector scaling support light, dark, and scaled displays. Marker symbols, color swatches, and application branding keep their existing meanings.

Read toolbar-design.md for the Qt and KDE guidance, compact-layout choice, and source links. Plugin contracts, usage, dependency notes, and affected MANIFEST versions were updated. Focused native tests passed mode toggles, shortcut migration, custom bindings, chooser icons, and arrow-key panning. Hardware graphics, native Wayland, and two-hour qualification remain untested.

Eight sanitizer suites passed in `build/quality/20261004-075734-quick-43224`. Source analysis passed in `build/quality/20261004-080201-analyze-3`. The native sanitizer run passed 71 cases in `build/quality/20261004-080325-gui-46675`. The existing Mesa leak still reports 240732 bytes in 1073 allocations. The GUI diagnostic remains failed without suppression.

Coverage passed eight suites and 71 GUI cases in `build/quality/20261004-075934-coverage-44449`. Generated icon paths reached 89 of 90 lines. Their remaining line is a closing brace. The icon engine reached 22 of 23 lines. Its required QIconEngine clone override remains untested. Allocation failures and defensive branches retain coverage gaps. Later focused tests exercise chooser tooltips and explicit dark Fusion colors at 200 percent scaling. The scaled run passed in `build/compact-hidpi.log`, including a background contrast assertion. Native screenshots cover the light and dark toolbar and simplified chooser.

The 60-second workload passed in `build/quality/20261004-080117-soak-45836`. It completed 1191 cycles with 14516224 bytes of peak memory growth. The maximum heartbeat gap was 55 milliseconds, and idle CPU time was 0.007 seconds. This short run does not replace two-hour qualification.

The release build passed in `build/compact-release.log`. The final build with committed version metadata uses `build/compact-release-final.log`. The icon generator reproduced the checked-in paths from the bundled SVG assets without changes.


## 2026-10-04: Undo history availability

The existing Qt undo action already disables itself when no deletions remain to restore. No application logic change was needed. A new native regression deletes three objects and restores them through Edit > Undo and Ctrl+Z. It tests the disabled menu after the last restore, repeated input with empty history, a new deletion, and explicit history removal. A native screenshot shows the empty Undo item in gray. The usage manual now describes this behavior.

All 72 GUI cases passed in `build/quality/20261004-082158-gui-49054`. The GUI sanitizer diagnostic remains failed because Mesa reports 187644 leaked bytes in 836 allocations. The allocation stacks point to libGLX_mesa. Eight sanitizer suites passed in `build/quality/20261004-082303-quick-49432`. Source analysis passed in `build/quality/20261004-082211-analyze-3`. The focused native test passed in `build/undo-focused.log` with Qt 6.8.2 and GCC 14.2.

Coverage passed eight suites and 72 GUI cases in `build/quality/20261004-082347-coverage-49911`. UserObjects reached all 93 lines and all 18 functions. Its branch coverage remains 274 of 526 reported branches. This task adds no application branches. Allocation failures, native Wayland, hardware graphics, and two-hour qualification remain untested.

The 60-second workload passed in `build/quality/20261004-082506-soak-50533`. It completed 1183 cycles with 13606912 bytes of peak memory growth. The maximum heartbeat gap was 37 milliseconds, and idle CPU time was 0.007 seconds. The release build passed in `build/undo-release.log`. The final build with committed version metadata uses `build/undo-release-final.log`.

## 2026-10-04: Servers, LAN peers, and CoT motion

The Servers browser replaces the UDP receiver. It supports multiple TLS, TCP, and UDP inputs through one network worker thread. Bounded mailboxes join repeated UID updates before the GUI receives them. TLS requires certificate validation and supports PEM and PKCS12 credentials. The legacy component and pane identifiers remain stable for saved profiles. File > Identity supplies sender fields to the generator. Transmission requires the explicit generator checkbox. Received messages are not forwarded.

Optional LAN discovery uses UDP port 17421 and direct TCP connections on a selected local IPv4 subnet. Each connection compares a SWAK-COT/1 greeting with recent discovery data. This mode uses plaintext without peer authentication. It remains disabled by default. Read servers.md for protocol details, resource limits, and the development harness.

Matching CoT UIDs move from the current displayed position over a fixed duration. Longer distances produce higher speeds. Large jumps fade out and then fade in. Stale symbols fade to gray, remain briefly, then fade away. File > Settings > CoT animation controls all four limits. Defaults are 600 milliseconds for movement, 100 kilometers for teleport, 600 milliseconds for fades, and 10 seconds for gray retention. Selection and hit detection follow displayed positions. Motion records remain bounded, and the animation timer stops when idle.

SettingsDialog and ProviderTable moved into the shared core. PluginServices adds Identity and Servers. SourcePlugin adds paneId. SettingsWidgetProvider supplies custom Settings content. TrackMotion supplies visual CoT state independently of authoritative event positions. Plugin contracts and affected MANIFEST versions were updated. No runtime dependency was added.

TAK Server reference commit 5187abd46d827d37cfc5708805eced197a837e49 defines the reviewed XML-first protocol. No implementation files were copied. SWAK does not request binary negotiation. Full TAK deployment, enrollment, mission APIs, QUIC, and binary-only endpoints remain outside the tested transport scope.

Nine sanitizer suites and the isolated LAN exchange test passed in build/quality/20261004-112721-quick-62659. Source analysis passed in build/quality/20261004-112950-analyze-3. The tests cover fragmentation, hostile XML, concurrent connections, reconnects, trust failures, PEM and PKCS12 client authentication, load, motion, and expiration. A debug load test received 12000 messages in about 403 milliseconds with 39 GUI heartbeat ticks. The namespace test exercises discovery, bidirectional CoT exchange, and shutdown without changing the host network.

All 74 native GUI cases passed in build/quality/20261004-112948-gui-63312. LeakSanitizer still reports the existing Mesa leak of 240732 bytes in 1073 allocations. The GUI diagnostic remains failed without suppression. Focused server and animation cases also passed at 200 percent scaling in build/servers-hidpi.log. Native screenshots and pixel assertions cover received symbols, movement, and gray stale symbols.

Coverage passed nine suites, 74 native cases, and isolated LAN exchange in build/quality/20261004-113058-coverage-63692. The stream reader reached 40 of 41 lines. LAN protocol validation reached all 13 lines. Servers reached 317 of 324 lines. TrackMotion reached 78 of 84 lines. Unexecuted paths include periodic heartbeat writes, peer expiration callbacks, some form actions, and motion-capacity eviction. Allocation failures and defensive branches also retain gaps. Line coverage does not establish complete input coverage.

The release build passed in build/servers-release.log. The final build with committed version metadata uses build/servers-release-final.log. Hardware graphics, native Wayland, Qt 6.4, physical two-machine LAN discovery, full TAK deployment, and two-hour qualification remain untested. Temporary toolchains under /tmp are local development tools, not portable project dependencies.

The 60-second workload passed in build/quality/20261004-113738-soak-65951. It completed 1174 cycles with 13398016 bytes of peak memory growth. The maximum heartbeat gap was 41 milliseconds, and idle CPU time was 0.017 seconds. This short run does not replace two-hour qualification.

## 2026-10-04: Shared CoT field pickers

Type, Location method, Team, and Role now have shared Select buttons in Identity, the generator, and their Settings pages. Type uses a tree of object affiliations, dimensions, and common functions. Location method separates human and machine sources.

Team supplies 14 common TAK colors. Role supplies eight common TAK roles. Lists use QCollator for system sorting. Custom text remains available, and cancellation preserves the current value.

The generator now exposes and saves Location method, Team, and Role in its draft. Use sender identity restores all sender fields. Existing profiles without those draft fields retain their identity defaults. Received CoT properties remain read-only.

The core adds cotFieldEditor and optional SettingField picker metadata. The editor MANIFEST advances to 0.6.0. No dependency or network operation was added.

ATAK reference commit 9f6893dd657feacc35ec5de03dad721c2e44170e supplied protocol facts. The arrays, CoT hierarchy, and base schema links appear in servers.md. The Type catalog contains common branches, not every MIL-STD-2525 function. Custom codes retain the existing validation rules.

Focused native tests passed at normal and 200 percent scaling in build/pickers-focused.log and build/pickers-hidpi.log. They cover known categories, invalid input, cancellation, custom values, locale sorting, parent destruction, generator XML, shared Settings controls, and profile restart. A native screenshot is build/atak-native/cot-type-picker.png. Source analysis passed in build/quality/20261004-115050-analyze-68233. Final diagnostic and release evidence follows below.

Nine sanitizer suites passed in build/quality/20261004-115047-quick-68138. All 76 native cases passed in build/quality/20261004-115310-gui-69969. The GUI diagnostic still fails on the existing Mesa leak of 240732 bytes in 1073 allocations. No suppression hides this failure. The release build passed in build/pickers-release.log. The final build with committed metadata uses build/pickers-release-final.log.

The 60-second workload passed in build/quality/20261004-115412-soak-70933. It completed 1194 cycles with 12644352 bytes of peak memory growth. The maximum heartbeat gap was 43 milliseconds, and idle CPU time was 0.022 seconds. This short run does not replace two-hour qualification. Hardware graphics, native Wayland, Qt 6.4, and expanded translated labels remain untested.

Coverage passed nine suites and 76 GUI cases in build/quality/20261004-115549-coverage-71289. The new picker reached all 88 lines and all 11 functions. It reached 240 of 546 reported branches. Allocation failures, defensive branches, and other input combinations retain gaps. These results do not establish complete input coverage.


## 2026-10-04: Live TAK Server compatibility

The user requested a local compatibility test. Official TAK Server 5.7-RELEASE-14 was built from commit 5187abd46d827d37cfc5708805eced197a837e49. Configuration and messaging ran in a disposable Java 17 container with PostgreSQL 15 and PostGIS 3.3. The official schema manager applied 94 updates. The API service and web dashboards were excluded. Only loopback TCP and TLS ports were exposed.

The new development-only swak_tak_compat executable uses production Servers, Store, encoding, and parsing code. Two clients exchange synthetic CoT through the server. The probe checks identity fields, same-UID position updates, and a disconnect followed by reconnect. It uses bounded profile reads and finite waits. It remains outside release builds and installation. No application behavior, plugin contract, or runtime dependency changed.

TCP, PEM mutual TLS, and encrypted PKCS12 mutual TLS passed. An untrusted server certificate and missing client certificates failed as expected. The PEM test also passed under AddressSanitizer and UndefinedBehaviorSanitizer without a report. Malformed profiles, missing client configurations, and unsupported protocols failed before sending. Detailed evidence and rerun instructions are in tak-compatibility.md.

The existing application diagnostics from the preceding picker task remain applicable because production sources did not change. This task adds a development probe and tests it against the actual server. These results supersede the earlier statement that no full server deployment test occurred. They do not establish enrollment, LDAP, mission API, federation, binary-only, packet-loss, or two-hour compatibility. The earlier Mesa GUI leak and hardware graphics gaps remain.

The test containers, database volume, and private network were removed after the tests. Cached development images and the reference build remain local. Temporary certificates expire after two days. They contain no production credentials. Raw reports remain under build/tak-*.json and build/tak-*.log. The final release rebuild uses build/tak-compat-release.log.


## 2026-10-04: Separate CoT emulator

The user chose a separate test tool. The development-only swak_cot_emulator now sends two teams with configurable ground and air units. The default scenario contains 22 units near Stockholm. JSON controls counts, type suffixes, affiliations, team colors, speeds, altitude, route radius, separation, seed, and update frequency. CLI flags override the center and rate.

Direct TCP listens on loopback port 18087 by default. UDP sends to an explicit numeric address. TAK mode reuses the shared Servers client and its TLS credentials. All modes reuse the production CoT encoder. No production API, plugin contract, or external dependency changed. Release builds exclude the tool.

Routes use circular patrols with constant ellipsoid altitude. They do not follow terrain or roads. Every unit retains its UID and uses Location method m-s. The tool bounds units, total rate, file input, TCP clients, and output queues. Its timer skips overdue samples. SIGINT and SIGTERM request normal shutdown and a JSON report.

The guide is cot-emulator.md. Start with build/debug/swak_cot_emulator and connect SWAK through Servers using TCP at 127.0.0.1:18087. Center the map at 59.33, 18.0 and zoom to 11 or 12. Use --print-defaults to obtain an editable scenario.

Ten sanitizer suites passed in build/quality/20261004-151505-quick-80032. Application source analysis passed in build/quality/20261004-151608-analyze-3. A separate scan of the emulator passed in build/emulator-tools-analysis.log. The partial scan excludes unused-function claims because it cannot see application callers. Manual review found no unused emulator functions.

The focused native test passed and saved build/atak-native/emulator-map.png. All 77 native cases passed in build/quality/20261004-151710-gui-82009. The GUI diagnostic still fails on the existing Mesa leak of 240732 bytes in 1073 allocations. No suppression hides it.

TLS uses the already tested shared client. This task tests a local TCP destination rather than repeating the actual TAK container deployment. Scheduler overload, failed UDP writes, every malformed nested field, allocation failure, and all certificate combinations retain coverage gaps. Two-hour operation and hardware graphics remain unqualified. Later evidence below records coverage and short load results.


Coverage passed ten suites and 77 GUI cases in build/quality/20261004-151753-coverage-82297. A focused gcov report reached 81 of 82 simulation lines and 106 of 110 emulator lines. The remaining line entries include compiler-generated destruction and multiline initializer accounting. Branch coverage remains incomplete. Raw details are in build/emulator-coverage-detail.

A 60-second TCP load test generated 119999 messages for 100 units at 20 updates per second. It recorded zero failed writes and zero skipped samples. One stalled client disconnected at the output limit. Peak resident memory growth after warmup was 3047424 bytes. The process exited normally. Results are in build/emulator-load.json. This short development test does not establish two-hour stability.

The release rebuild passed in build/emulator-release.log. The final rebuild after the commit uses build/emulator-release-final.log.

The 60-second sanitizer workload passed in build/quality/20261004-151927-soak-83451. It completed 1171 cycles with 13365248 bytes of peak memory growth. The maximum heartbeat gap was 39 milliseconds. Idle CPU time was 0.016 seconds.

## 2026-10-04: Connection error details

The user reported an unhelpful connection failure while trying the emulator. A local inspection found no process listening on the default TCP port 18087. The user's exact address and protocol remain unknown. That observation does not establish every cause of the reported failure.

Server failures now include protocol, address, port, cause, and retry delay. Refusal, failed host lookup, timeout, remote closure, TLS handshake failure, certificate rejection, and UDP bind failure have distinct guidance. Retry counters saturate instead of growing without a bound. Text logging remains opt-in through the shared logger. Messages exclude certificate bodies, private keys, and passwords.

The Servers pane now shows errors in a read-only text box. Users can select and copy the entire message. The simulator prints the required TCP connection configuration and explains loopback binding. Startup failures include the endpoint and socket reason. Invalid CLI arguments identify the flag. JSON errors identify the input role and parsing offset.

Regression tests exercise refusal, retry recovery, TLS against a plaintext listener, readable error details, copying, and port collisions. The native screenshot is build/atak-native/server-connection-error.png. The focused native test passed with Qt 6.8.2 in build/connection-focused.log.

Ten sanitizer suites passed in build/quality/20261004-153339-quick-86600. Source analysis passed in build/quality/20261004-153340-analyze-3. Later evidence below covers final diagnostics and release output. DNS failures and firewall timeouts retain environment-dependent test gaps. Hardware graphics, native Wayland, and two-hour operation remain unqualified.

All 77 native cases passed in build/quality/20261004-153533-gui-88141. The GUI diagnostic still fails on the existing Mesa leak of 240732 bytes in 1073 allocations. Coverage passed ten suites and all native cases in build/quality/20261004-153628-coverage-88483. The separate emulator source scan passed in build/connection-tool-analysis.log. No suppression hides the graphics leak.

The release rebuild passed in build/connection-release.log. The final build after the commit uses build/connection-release-final.log.

The 60-second sanitizer workload passed in build/quality/20261004-153801-soak-89214. It completed 1174 cycles with 13434880 bytes of peak memory growth. The maximum heartbeat gap was 47 milliseconds. Idle CPU time was 0.019 seconds. The final simulator sanitizer suite passed in build/connection-final-simulation.log. This short run does not replace two-hour qualification.

## 2026-10-04: Server forms, Information closure, prediction, and traces

The server form no longer duplicates the Active column with a Connect checkbox. It preserves the active state from the provider record. Shared conditional field metadata disables TLS credential editors and labels for TCP and UDP. Stored credentials remain intact.

Information reopened because live CoT refreshes retained selection and requested pane visibility. An explicit dock close now invokes PaneContent::paneClosed and clears both selection models. The callback applies only to the watched dock. Closing other dialogs and switching tabs do not clear selection.

TrackMotion now predicts display movement from successive sender timestamps. It preserves position continuity with a smooth correction term. Prediction lasts at most two sample intervals and the configured 3000 millisecond default limit. It slows before that limit. Invalid intervals, excessive projected speed, and teleports do not produce predictions. Stale markers freeze during fading. Setting the prediction limit to zero restores interpolation without prediction.

TracePlugin implements the menu and map-tool roles with a MANIFEST. Right-click CoT or local markers to toggle Trace. Each trail keeps at most ten distinct authoritative positions and fades older segments. Limits are 128 enabled marker identities and ten points per identity. Trace count and opacity use the shared Settings interface. Enabled identities survive restart. Session history clears when the source marker disappears. Features without stable marker identities do not offer Trace.

MapTool::populateContextMenu lets plugins contribute actions without replacing marker editing commands. Store::eventAccepted exposes individual accepted positions inside batches. PaneContent and SettingField gained shared hooks described in plugin-api.md. The host still uses static plugin API version 2. No external dependency was added.

Focused motion and native tests passed before the full diagnostics. Tests cover stable rendering, bounded prediction, stale freeze, disabled prediction, TLS editor state, explicit pane closure, trace fading, the ten-position limit, the 128-target limit, preference restoration, and stopping traces. Large traffic with tracing on every marker, extended packet loss, and two-hour operation retain qualification gaps. Further diagnostic and release evidence follows below.

The first full GUI run exposed a chooser that stayed open after local object deletion. MapTools now separates object changes from repaint requests. The focused overlap and trace tests pass in build/trace-menu-focused.log. All 78 native cases then passed in build/quality/20261004-160845-gui-97183. The diagnostic still reports the existing Mesa leak of 240732 bytes in 1073 allocations. No suppression hides it.

Ten sanitizer suites passed in build/quality/20261004-160128-quick-92904. Final source analysis passed in build/quality/20261004-161038-analyze-3. Native images are build/atak-native/server-tcp-fields.png and build/atak-native/marker-trace.png. The short workload now enables 128 trace identities and repeatedly fills and clears their histories.

Coverage passed ten suites and 78 GUI cases in build/quality/20261004-161037-coverage-97953. Trace reached 90 of 93 lines and all 26 functions before the additional local-marker test. Motion reached 96 of 102 lines and all ten functions. Branch coverage remains incomplete. The final local-marker test also exercises color, removal cleanup, and date-line wrapping.

The 60-second sanitizer workload passed in build/quality/20261004-161301-soak-98792. It completed 1173 cycles with 16867328 bytes of peak memory growth. The maximum heartbeat gap was 52 milliseconds. Idle CPU time was 0.024 seconds. This short run does not establish two-hour stability.

The final core trace test passed under sanitizers in build/trace-core-sanitize-final.log. Both focused native cases passed in build/trace-gui-focused-final.log. That native process still reports the same Mesa leak. Qt 6.8.2 supplied the native test environment. The release rebuild after the commit uses build/trace-release-final.log.


## 2026-10-04: Trace visibility and black map labels

Trace history now defaults to 50 positions and accepts two through 50 positions. Default opacity increases from 35 to 70 percent. Older segments retain at least one-quarter of the configured opacity. The upper opacity limit is 100 percent. The limit remains 128 traced identities.

Trace preferences now include styleVersion 2. Older profiles with the original default count or opacity adopt the new defaults. Other saved values remain unchanged. Once migrated, explicit choices of ten positions or 35 percent opacity remain unchanged.

CoT, area, data, and vector map labels use black text. Area labels use a pale translucent background for contrast. Selection outlines and marker colors remain unchanged. Tests exercise the 50-position bound, fading, and preference migration. The workload now adds 52 positions per cycle to exercise history eviction.

The first sanitizer run rejected an incompatible manifest schema value and failed two core cases. The manifest now retains the supported schema. The plugin stores its migration version inside its own preferences. Source analysis passed in build/quality/20261004-163310-analyze-3. All 78 native GUI cases passed in build/quality/20261004-163426-gui-101809. The native diagnostic still reports the existing Mesa leak of 240732 bytes in 1073 allocations. No suppression hides it. Native images confirm black area text and the stronger trace.

The corrected core sanitizer suite passed all 47 cases in build/visibility-core-final.log. The other nine sanitizer suites passed in build/quality/20261004-163309-quick-101128. Coverage passed all ten suites and 78 GUI cases in build/quality/20261004-163615-coverage-102462. The 60-second workload passed in build/quality/20261004-163459-soak-102137. It completed 1113 cycles with 15192064 bytes of peak memory growth and a maximum heartbeat gap of 67 milliseconds. Two-hour operation and hardware graphics remain unqualified. The release build passed in build/visibility-release.log. The final rebuild after the commit uses build/visibility-release-final.log.


## 2026-10-04: Restore section defaults

The user reported saved Trace values of ten positions and 0.35 opacity. The previous source review established shared runtime values, but it did not inspect the user profile. Standard Settings sections now offer Restore section defaults. The host captures initial field values before preference restoration. The dialog stages default field values until Apply or OK. Cancel discards them. Hidden fields, traced identities, and provider records remain intact. Custom provider management tables do not expose a generic reset.

The GUI regression exercises reset, cancellation, application, preserved trace identities, and restart. Plugin guidance describes the startup default contract and SettingsPage.defaults.

Ten sanitizer suites passed in build/quality/20261004-165620-quick-105479. Source analysis passed in build/quality/20261004-165621-analyze-3. All 79 native GUI cases passed in build/quality/20261004-165752-gui-106139. The GUI diagnostic still reports the existing Mesa leak of 240732 bytes in 1073 allocations. The inspected trace-reset.png image shows the reset control and default values. The new regression directly exercises numeric reset fields. Boolean, text, key, and choice reset branches retain direct test gaps.

Coverage passed ten suites and 79 GUI cases in build/quality/20261004-165943-coverage-106697. The 60-second workload passed in build/quality/20261004-165829-soak-106556. It completed 1131 cycles with 17117184 bytes of peak memory growth and a maximum heartbeat gap of 46 milliseconds. Idle CPU time was 0.019 seconds. Two-hour operation and hardware graphics remain unqualified. The release build passed in build/reset-release.log. The final rebuild after the commit uses build/reset-release-final.log.


## 2026-10-04: Paused CoT positions and Information tracing

Marker labels now use white text on a dark translucent background. Local point markers also show their titles. Areas and downloaded data labels use the same contrast scheme. Base vector-map labels retain their provider rendering.

Space toggles displayed CoT positions when the map has focus. The configurable shortcut appears under Map in Keybindings. TrackMotion retains a bounded frozen snapshot while Store continues accepting messages. Resume clears prediction and interpolation and shows the latest retained positions immediately. New messages then animate normally. Pause state is not persisted. The status bar uses a separate pause field.

Information exposes Trace through generic MapTool contributions. Context menus and Information share the Trace action implementation. A dedicated actionsChanged signal refreshes controls after toggles or clearing. Continuous position updates do not recreate the checkbox for the same selection. The focused motion and GUI tests passed in build/pause-motion.log and build/pause-focused.log.

The user also requested readable CoT Type and How fields. cotFieldDescription reuses the selector catalog and appends the original code in parentheses. Unknown categories remain explicit. Information applies this formatting only to CoT objects. Live selection refresh also updates Type and How from the latest message. Tests cover known categories and unknown suffixes.

The initial full runs exposed label padding that touched a selected symbol and an outdated separator-count assertion. Label text now starts farther from point symbols. The status test includes the new pause section. The final analysis passed in build/quality/20261004-184608-analyze-4. The focused tests exercise reception during pause, immediate resume, new objects, expiry, clearing, and Information tracing. Extended pause under high traffic and physical GPU timing retain qualification gaps.

The corrected core sanitizer suite passed 47 cases in build/pause-core-final.log. All 80 native GUI cases passed in build/quality/20261004-185108-gui-116082. The GUI diagnostic still reports the existing Mesa leak of 240732 bytes in 1073 allocations. Inspected images show white marker labels, Information tracing, and readable CoT categories.

The 60-second workload passed in build/quality/20261004-184601-soak-113718. It completed 1130 cycles with 17903616 bytes of peak memory growth and an 80 millisecond maximum heartbeat gap. The first coverage run failed its short UDP progress assertion. The workload now allows a bounded two-second receive wait after its existing pacing delay. Later evidence records the rerun.

Final coverage passed ten suites and 80 GUI cases in build/quality/20261004-185143-coverage-116490. The release build passed in build/pause-release-complete.log. The rebuild after the commit uses build/pause-release-final.log. The user next requested Circle, Distance, and Line tools. That work follows in a separate change.


## 2026-10-04: Circle, Distance, and Line

ShapesPlugin adds C for Circle, D for Distance, and L for Line. Its MANIFEST declares pane, menu, toolbar, and map-tool roles. It reuses ObjectEditor, UserObjects, MapSelection, SettingsParticipant, and deletion undo. Saved range-circle and line records each retain two control points. The older circle type still means a point marker. Circle fill opacity comes from PolygonTool through the shared MapTools API.

Distance keeps at most 64 temporary measurements. They remain for ten seconds and fade over one second by default. A monotonic clock drives the timer, which stops when no measurements remain. The configuration controls duration, fade, line width, point size, and shortcuts. Distances use locale units and spherical surface distance. Circle area uses the shared spherical integrator over 128 points. Circle bounds stay inside the projected world. Lines wrap across the date line.

The focused tests cover geometry, locale units, creation, whole-object movement, cancellation, editing, deletion, undo, restart, and temporary measurement expiry. Additional native tests inspect the actual OpenGL frame and control-point movement.

Ten sanitizer suites passed in build/quality/20261004-190638-quick-120293. Source analysis passed in build/quality/20261004-190633-analyze-4. The first native run found three outdated pane-count expectations. Those tests now include the shared shape editor. Coverage passed ten suites and all 81 native cases in build/quality/20261004-191011-coverage-122132.

Shape coverage reached 170 of 171 lines and 61 of 64 functions. Measurement coverage reached 32 of 33 lines and all five functions. Remaining paths include direct menu callbacks, editor cancellation, and complete-circle boundary rejection. The 64-measurement eviction limit and date-line drawing still need dedicated native tests. No source-analysis finding indicates dead code in the new plugin.

The 60-second workload passed in build/quality/20261004-190858-soak-121970. It completed 1105 cycles with 18284544 bytes of peak memory growth. The maximum heartbeat gap was 51 milliseconds. Idle CPU time was 0.016 seconds. Two-hour operation, physical graphics hardware, and native Wayland remain unqualified. Inspected native images show the circle fill, selection outline, and line distance label.

The release build passed in build/shapes-release.log. The final rebuild after the commit uses build/shapes-release-final.log.

All 81 native sanitizer cases passed in build/quality/20261004-191216-gui-123523. The diagnostic still fails on the existing Mesa leak of 240732 bytes in 1073 allocations. No suppression hides this leak.


## 2026-10-04: Selectable labels, paused traces, Identity, and toolbar groups

Object labels invert their foreground and background colors on selection. Shared bounds include shortened text and padding. CoT, local markers, areas, circles, lines, and data labels use these bounds for selection. Vector labels share their layout between tile rendering and hit testing. Selected vector labels invert cached pixels without another tile render. Overlap selection retains the existing chooser.

TraceTool listens to TrackMotion::pauseChanged. It freezes a bounded display copy while the live history continues recording. Resume shows the latest retained history. User actions can still disable or clear traces during pause. Focused tests compare images before pause, during incoming updates, and after resume.

Identity already generates and persists its initial UID. SettingField::unlockToEdit now adds a lock button and read-only text input. The lock resets when the dialog reopens. Both Identity and Settings use the same field contract. Section reset preserves UID. Tests cover generated identity persistence and the lock workflow.

Toolbar actions now use shared section metadata. Marker, Area, Circle, and Line share the creation section. Distance occupies the measurement section. The host preserves menu action reuse and removes redundant separators. No external dependency was added.

Focused native label and pause tests passed in build/label-focused.log. The initial core pause test exposed a reused coordinate in its date-line fixture. The corrected fixture resets that coordinate. Vector label targets passed in build/label-vector.log. Later entries record full diagnostics and release output.

Ten initial sanitizer suites passed in build/quality/20261004-192222-quick-126169. Final source analysis passed in build/quality/20261004-192752-analyze-3. The first native run passed 82 cases and found one outdated image baseline. The Distance expiry test now clears selection before comparison because selected labels have different colors.

The 60-second workload passed in build/quality/20261004-192626-soak-130115. It completed 1102 cycles with 17170432 bytes of peak memory growth. The maximum heartbeat gap was 48 milliseconds. Idle CPU time was 0.010 seconds. Hardware graphics, native Wayland, and two-hour operation remain unqualified. High-load pause transitions and every translated font retain coverage gaps.

Final coverage passed ten suites and all 84 native cases in build/quality/20261004-192740-coverage-130342. The label helpers and trace code reached every line. Toolbar coverage reached 24 of 25 lines. Remaining gaps include the trailing-separator cleanup and some reset branches for unrelated field types. Inspected screenshots show the UID lock and grouped creation buttons with a separate Distance section.

The release build passed in build/label-release.log. The final rebuild after the commit uses build/label-release-final.log.

All 84 native sanitizer cases passed in build/quality/20261004-193004-gui-132121. The diagnostic still reports the existing Mesa leak of 240732 bytes in 1073 allocations. No suppression hides this leak. The run used Qt 6.8.2 and Xvfb with software OpenGL.

## 2026-10-04: Reboot checkpoint

The user confirmed that the outdated debug executable caused the apparent toolbar grouping error. Both debug and release executables now include functional commit 06a152b3458f. The debug rebuild passed in build/grouping-debug-sync.log. The native grouping regression passed in build/grouping-verify.log. The final release rebuild passed in build/label-release-final.log. No requested implementation work remains pending.

The toolbar order is Map provider, Clear messages, Marker, Area, Circle, Line, and Distance. Separators group Marker through Line together and place Distance in its own section. Labels support selection and invert colors when selected. Space freezes displayed positions and traces while reception continues. Identity generates its initial UID and requires the lock button before editing it.

The working tree was clean before this checkpoint. Commit this checkpoint as documentation only. Its commit follows the functional revision embedded in both executables. No background development task needs to resume after reboot.

This session used temporary tools under /tmp/swak-toolchain/root/usr and /tmp/swak-quality-tools/root/usr. A reboot can remove them. Inspect the local toolchain before the next build. Follow docs/build.md and docs/quality.md if those paths no longer exist. Do not assume that temporary tools are portable to another machine.


## 2026-10-04: Menus, provider controls, overlay scrolling, and Identity presentation

Selected vector labels now redraw in white on solid black after selection geometry. The earlier pixel inversion ran before translucent selection fill and depended on the underlying image. MapItem retains the cached label text, bounds, and normalized font height.

Circle, Distance, and Line use MenuPlugin::rootActions and appear directly in Plugins. A shared locale sorter handles the menu bar, submenus, and transient context menus. Menu separators are removed for global alphabetical order. Toolbar separators remain unchanged.

Servers and Data providers now have toolbar menus with active-state checkboxes and their existing management actions. Each toggle updates the latest source record. MapPanels consumes unhandled wheel events from overlay controls. A map-side filter rejects wheel events over the visible overlay mask.

Identity displays descriptions with raw codes for Type, Location method, and Team. The shared picker retains custom code entry and raw storage. Team choices use colored vector icons. Read-only text fields use disabled palette colors while preserving copying. Lock states use different icons and action tooltips. OK and Apply now use different vector icons.

The focused native tests passed in build/menu-focused-final.log with Qt 6.8.2. The initial fixture omitted required attribution and was corrected. Inspected screenshots show the muted Identity fields and distinct buttons. Full diagnostic evidence follows below.

After reboot, the system supplies Qt 6.8.2, CMake, Ninja, and GCC. Old build caches referenced removed temporary tools. The debug, sanitizer, coverage, and release presets need fresh configuration for the system toolchain. Development-only Xvfb and Cppcheck were extracted under build/dev-tools. Cppcheck also uses an extracted tinyxml2 library and a local cfg symlink. None of these tools enters the release executable.

Ten sanitizer suites passed in build/quality/20261004-200412-quick-9419. The first full native run exposed popup closure during sorting. The sorter now moves actions before menus open and defers separator destruction safely. Tests identify providers by ID rather than old menu positions. Chooser paths passed in build/menu-chooser-fixed.log except that outdated provider-position assertion, which was then corrected.

All 85 native sanitizer cases passed in build/quality/20261004-201155-gui-16788. The diagnostic still reports the existing Mesa leak of 240732 bytes in 1073 allocations. No suppression hides it. Source analysis passed in build/quality/20261004-201156-analyze-3 after restoring the Cppcheck configuration files.

The 60-second workload passed in build/quality/20261004-200717-soak-13331. It completed 1107 cycles with 18141184 bytes of peak memory growth. The maximum heartbeat gap was 47 milliseconds. Idle CPU time was 0.018 seconds. Hardware graphics, native Wayland, two-hour operation, and all locale/font combinations remain unqualified.

Final coverage passed ten suites and all 85 native cases in build/quality/20261004-201244-coverage-17301. The new menu, provider-toolbar, and read-only presentation helpers reached every line and function. The picker and overlay panel code also reached every line. Branch coverage remains incomplete, including rare source-removal timing and all theme/locale combinations.

The release build passed in build/menu-release.log. Final builds after the commit use build/menu-release-final.log and build/menu-debug-final.log. Both executables must use the same committed functional revision. No user request remains pending after these builds succeed.
