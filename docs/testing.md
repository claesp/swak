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
