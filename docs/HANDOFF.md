# Session handoff

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

The Qt skill is at `/home/clpe/.codex/skills/qt-linux-gui/SKILL.md`. The Simple English skill was supplied in the conversation and is also available through the installed plugin. Use the skill locations available in the next session if plugin paths change.

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

Cppcheck 2.17.1 and its XML library were extracted under `/tmp/swak-quality-tools/root` for this session. This temporary installation is not part of the application. A normal Debian development host can install Cppcheck through its package manager. `AGENTS.md` now requires regular diagnostics, coverage review, dead-code review, and resilient error handling.

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
