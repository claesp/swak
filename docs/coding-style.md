# Coding style

This standard applies to first-party C++, Python, and CMake code, including tests and development tools. It follows the qt-linux-gui skill and the existing Linux, Qt 6.4, and C++20 baseline. Repository rules take precedence over formatter defaults. Formatting does not prove correct behavior or memory safety.

## Automatic format

Use four spaces, UTF-8, LF line endings, and a final newline. Use a 100-column target. Keep long literal strings intact when splitting them changes their meaning. The repository formatter files define indentation, wrapping, and brace placement.

Use clang-format 19 for C++. Put function and type opening braces on a new line. Put control-statement opening braces on the same line. Use braces for every conditional and loop body. Keep each statement on its own line. Empty functions can remain on one line.

Group local includes before Qt includes, then other system includes. Include the matching header first in implementation files when one exists. Sort each group. Use self-contained headers with pragma once. Do not depend on a different header to supply a required standard-library declaration.

Use Black 25.1.0 for Python and cmake-format 0.6.13 for CMake. The Python baseline remains 3.9. Keep CMake targets, dependencies, and source lists explicit. Use the repository Ninja presets and keep machine paths out of committed files.

Third-party source, test fixtures, and generated files retain their source format. In particular, ui_icon_paths.inc comes from tools/generate_icons.py. Change its generator when icon behavior changes. Do not reformat vendor code or generated Qt code. MANIFEST and other JSON files use two-space indentation.

## Names and structure

Use UpperCamelCase for types and enum values. Use lowerCamelCase for C++ functions, parameters, and variables. Private data members end with an underscore. Public records use names without that suffix. Use descriptive UpperCamelCase names for shared limits and constants.

Use snake_case for Python functions and variables. Use uppercase names for module constants. Preserve external API names, Qt overrides, stable identifiers, protocol fields, and preference keys. Use short mathematical names only where their meaning is clear.

Keep code in the swak namespace. Put implementation-only helpers in an unnamed namespace. Do not add using-directives to headers. Prefer small functions with one purpose. Separate validation, state changes, and interface updates when that makes ownership or failure handling clearer. Do not split code only to meet a line-count target.

Use plain English for comments and documentation. Explain reasons, ownership, units, and limits. Do not repeat the code in comments. Put project documents under docs. Keep temporary review evidence under build.

## Values and ownership

Initialize fields. Use const for values that do not change. Use auto when the initializer makes the type clear. Use explicit types when units, narrowing, or an interface contract needs them. Keep related declarations readable. Do not shadow a parameter or local variable in a nested scope.

Prefer values and standard containers. Use unique_ptr for exclusive ownership outside QObject parent ownership. Use shared_ptr only for shared lifetime, such as worker cancellation state or a mailbox. Do not combine smart-pointer ownership with QObject parent ownership for the same object.

Use QObject parents for widgets, actions, and timers. Use QPointer for borrowed QObject references that can expire independently. A raw pointer or reference requires an owner that outlives its use. Bind asynchronous callbacks to a context object. Capture durable values in queued work. Reference captures require a documented or locally evident lifetime.

Use nullptr, scoped enums, and checked casts. Use qobject_cast for Qt interfaces and QObject types. Use dynamic_cast for non-QObject interface roles where required. Avoid reinterpret_cast, const_cast, C-style casts, and manual allocation. Keep native handles inside a small lifetime-owning wrapper.

## Qt interface and workers

Keep widgets and their models on the interface thread. Use layouts and size policies for ordinary controls. Map rendering, overlays, and coordinate hit testing can use explicit geometry. Use logical display coordinates for interface sizes.

Reuse QAction objects between menus and toolbars. Use shared plugin interfaces, configuration fields, and embedded manifests. Preserve row identity when sorting. Use model notifications around data changes. Keep remote text plain unless a reviewed helper escapes intentional markup.

Use translation-aware interface text and locale-aware sorting and units. Provide names and tooltips for icon controls. Preserve keyboard access, disabled states, and focus. Respect standard menu conventions, including Quit last in File.

Use asynchronous network APIs. Bound work per event-loop pass. Keep GUI callbacks free of sleep, processEvents loops, and blocking network waits. Tests can use bounded Qt waits. Shutdown can wait for a cancelled, bounded worker to finish before it destroys resources.

Use a worker QObject for threaded services. Create sockets on their owning thread. Define cancellation, timeout, stale-result rejection, and shutdown before adding asynchronous work. Release locks before calling external callbacks or updating widgets.

## Input and failures

Treat network data, profiles, archives, and user-selected files as untrusted. Apply size, count, depth, time, and numeric bounds before allocation or parsing. Reject nonfinite coordinates and unsupported schemes. Use the shared bounded file reader and atomic preference writes.

Keep TLS certificate and hostname validation enabled by default. Preserve explicit supported exceptions and credential-cache isolation. Do not place credentials or payloads in logs. Use the shared severity and urgency logger. Logging stays disabled until the user enables it.

Return a specific recoverable error through the existing result type. Leave valid state intact when an operation fails. Do not throw through Qt callbacks. Catch known library exceptions at the decoding boundary. The noexcept logger retains a narrow catch-all to prevent diagnostic failures from escaping. No handler can recover from memory corruption or fatal signals.

Keep loops, queues, caches, histories, and retries bounded for long sessions. Use cancellation flags and source generations for background work. Do not use assertions to validate external input. Assertions in development tools can describe trusted pinned fixtures.

## Enforcement and review

Install clang-format-19, Black 25.1.0, and cmake-format 0.6.13 as development tools. On Debian 13, the packages are clang-format-19, black, and cmake-format. They do not become application dependencies.

From the repository root, run this command to report format differences:

```sh
python3 tools/style.py
```

To apply the format, run this command:

```sh
python3 tools/style.py --fix
```

CLANG_FORMAT can select an alternative path to clang-format 19. The script rejects incompatible formatter versions. It uses explicit file lists and does not format build output or third-party files. Review the diff after formatting, especially inserted braces and include changes.

The diagnostic runner also supports python3 tools/quality.py style. Run source analysis, sanitizer tests, native interface tests, coverage, and the required workload before a release. Record untested paths and platform limits. Keep functional repairs separate from mechanical formatting when practical.
