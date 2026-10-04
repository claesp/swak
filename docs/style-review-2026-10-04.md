# Coding style review

This review uses the qt-linux-gui skill, its C++ and Ninja guidance, and the project security rules. The standard is in [Coding style](coding-style.md). This review covers the first-party tree, including tests, development tools, CMake files, and shared plugin headers. Third-party source, fixtures, and generated output retain their original format.

## Changes and evidence

The initial tree compressed many statements and branches onto single lines. The formatted tree uses explicit braces, separate statements, consistent wrapping, and grouped includes. Direct standard-library includes replace incidental declarations from other headers. Four callback locals now have distinct names to remove compiler warnings about shadowed variables. The existing class, method, and preference names remain stable.

The enforced inventory contains 123 C++ files, eight Python files, and two CMake files. The formatter versions are clang-format 19, Black 25.1.0, and cmake-format 0.6.13. The new runner has tests for source selection, missing tools, incompatible versions, and command failure. The existing diagnostic runner exposes a style mode.

All 50 first-party headers compiled independently with the project compiler flags. Evidence is in build/style-headers.log. The debug build passed with Qt 6.8.2 and GCC 14.2. Evidence is in build/style-debug-final.log.

The C++ token comparison accounts for the four reviewed local renames. Other differences are includes, braces, comments, whitespace, and equivalent adjacent string literals. Evidence is in build/style-token-review-final.log. This comparison does not prove semantic equivalence. Compiler and runtime tests also exercise the formatted code.

Python syntax-tree comparisons passed for scripts with formatting-only changes. The intended exceptions are the quality runner and the icon generator. The quality runner adds style tests and includes development tools in source analysis. The generator gives its module constants consistent names. Generated icon output remains unchanged.

## Review scope

| Area | Review focus | Result |
| --- | --- | --- |
| Application and dialogs | Action reuse, ownership, layout, saved configuration, and plain text | Formatting and direct includes changed. Existing behavior remains. |
| Shared interfaces and manifests | Stable roles, ownership, version fields, and validated metadata | No interface or manifest schema change. |
| CoT, LAN, KML, MVT, and archives | Input limits, malformed data, cancellation, and bounded work | Existing validation and limits remain. |
| Servers and background decoding | Callback contexts, lifetime, thread ownership, stale results, and shutdown | Existing contexts, generation guards, and cancellation remain. |
| Map and local objects | Logical coordinates, overlay painting, selection, and bounded history | No rendering or object-model change. |
| Logging and files | Quiet defaults, bounded messages, native handle lifetime, and atomic writes | Existing protections remain. |
| Tests and development tools | Bounded waits, process arguments, timeout reporting, and source analysis | Style tests added. Analysis now includes C++ development tools. |

The callback scan found no context-free connect call with a lambda in application source. Reference captures remain where a local scope or owning service controls their lifetime. Server sessions disconnect socket callbacks before releasing session state. Decoder jobs retain cancellation state until completion.

The logger retains two narrow catch-all handlers that stop diagnostic exceptions from escaping Qt. They do not handle fatal signals or memory corruption. Decoder boundaries catch standard exceptions and return error results. Worker shutdown waits remain in destructors after cancellation or event-loop shutdown.

The map uses explicit geometry for painting and overlays. Ordinary dialogs use Qt layouts. Generated icon paths remain an explicit style exception. The style guide records these cases instead of imposing changes that can alter behavior.

## Limits

Formatting and source analysis do not prove memory safety or complete path coverage. The existing Mesa graphics-driver leak remains unsuppressed. Native Wayland, hardware graphics, all theme and locale combinations, and two-hour desktop operation require separate qualification.

Read the latest section of [Session handoff](HANDOFF.md) for final diagnostic paths, coverage gaps, and release build evidence.

Final coverage reached 11225 of 12172 lines and 1106 of 1178 functions. It exercised 16161 of 31042 branches. Remaining gaps include invalid client-key files, startup manifest rejection logging, and some vector-cache eviction paths. Some unexecuted lines refer to compiler instrumentation at lambda closing braces. No new unused function was reported by source analysis.

The final release build reports no compiler warnings. Ten sanitizer suites and all 86 native interface cases pass. The graphical memory diagnostic still fails on the recorded Mesa leak. The workload ran for 60 seconds, so it does not establish two-hour reliability.
