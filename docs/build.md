# Build and run

The baseline is Linux, C++20, CMake 3.24, Ninja, and Qt 6.4 or later. The graphics driver must support OpenGL 3.3. The project requires the Qt Core, Gui, Widgets, Network, OpenGLWidgets, Concurrent, and Test components. Marker collections and the KML plugin require libarchive development files. UTM, UPS, and MGRS grids require GeographicLib development files.

On Debian 13, install the build packages with these commands. The package manager requires administrator access. Debian 13 includes the OpenGL development files in `qt6-base-dev`.

```sh
sudo apt-get update
sudo apt-get install build-essential cmake ninja-build qt6-base-dev libarchive-dev libgeographiclib-dev
```

Run these commands from the repository root. The debug preset uses Ninja and writes generated files under `build/debug`. The test suite opens local UDP sockets.

```sh
cmake --preset debug
cmake --build --preset debug --parallel 4
ctest --preset debug
./build/debug/swak
```

For a nonstandard Qt installation, set `CMAKE_PREFIX_PATH` during configuration. Use one Qt installation for headers, libraries, and tools. Keep machine-specific paths in an untracked `CMakeUserPresets.json` file.

```sh
cmake --preset debug -DCMAKE_PREFIX_PATH=/path/to/qt
```

Always rebuild the release executable after application changes, before you complete the task. The release preset uses a separate directory. Run the following commands from the repository root. Installation includes the executable, desktop entry, icon, and user guides. Developer reviews and session notes stay in the source repository.

```sh
cmake --preset release
cmake --build --preset release --parallel 4
cmake --install build/release --prefix "$HOME/.local"
```

The installed application uses system Qt libraries and platform plugins. It does not bundle Qt or install additional libraries. The `~/.local/bin` directory must appear in `PATH` for the desktop entry to launch SWAK.

# Tests

Read [Development diagnostics](quality.md) for the on-demand runner, long-session workloads, source analysis, and coverage reports.

Qt Test exercises message parsing, event storage, file limits, projection, and receiver shutdown. AddressSanitizer detects certain memory errors during tests. UndefinedBehaviorSanitizer detects certain invalid C++ operations during tests.

Run these commands from the repository root to use both diagnostic tools. The sanitizer preset uses a separate directory. These tools do not establish that the application is secure.

```sh
cmake --preset sanitize
cmake --build --preset sanitize --parallel 4
ctest --preset sanitize
```

The GUI test requires a display with OpenGL support. Run the following command from `build/debug` in a desktop session. The test creates a window and writes `gui-smoke.png`, `gui-light.png`, and `map-smoke.png` in the current directory.

```sh
./swak_gui_tests
```

Xvfb provides a temporary X11 display for unattended tests. On Debian 13, install `xvfb` and `xauth` first. Run this command from `build/debug` when a desktop display is unavailable.

```sh
QT_QPA_PLATFORM=xcb xvfb-run -a -s '-screen 0 1440x1000x24 -nolisten tcp' ./swak_gui_tests
```

The ordinary CTest suite uses the offscreen platform and does not test rendering. The GUI test runs separately to avoid silent skips on hosts without OpenGL. Native Wayland behavior requires a separate desktop test.


# Release hardening

Optimized GCC and Clang builds set `_FORTIFY_SOURCE=2` unless the toolchain supplies its own value. This enables supported library bounds diagnostics. Existing stack protection and linker protection remain enabled. Tests and sanitizers remain excluded from the release preset.

The MVT provider uses pinned protozero headers included in the source tree. Builds do not fetch them. Read `docs/dependencies.md` for provenance, licenses, and the security review.


# Source revision

Each build refreshes `generated/swak/version.h` through `cmake/version.cmake`. The version includes the first 12 Git SHA characters. The full SHA and an exact tag appear in Help > About. The build adds `-dirty` when tracked files differ from the commit. It does not include untracked files in this test.

Ninja runs the metadata task on every build. CMake changes the header only when its contents change. This updates the version after a commit or tag change without requiring manual configuration. Git is a build-time tool only. A source archive without Git metadata uses `+unknown`.

After you commit release sources, rebuild the release preset. Run `build/release/swak --version` to read its source identity. Development diagnostics test metadata generation in a temporary Git repository.

# CoT emulator

Build the separate development tool with `cmake --build --preset debug --target swak_cot_emulator`. Read [CoT emulator](cot-emulator.md) for local reception, TAK connections, and scenario parameters. The release executable does not include this tool.

# Graphics startup

SWAK prefers EGL for the Qt X11 graphics connection. EGL and GLX connect OpenGL rendering to the window system. This avoids a GLX allocation leak in Mesa. The project reproduced this leak during KDE theme startup on the development host. Rendering still uses OpenGL and the installed driver. Qt tries GLX if its EGL integration cannot initialize. Native Wayland selection remains unchanged.

SWAK preserves an explicit QT_XCB_GL_INTEGRATION environment value. For a GLX comparison, run this command from the repository root:

```sh
QT_XCB_GL_INTEGRATION=xcb_glx ./build/release/swak
```

To use the SWAK default again, unset QT_XCB_GL_INTEGRATION before startup. This default requires no system driver changes or leak suppressions. GPU and desktop combinations outside the tested host still require native testing.
