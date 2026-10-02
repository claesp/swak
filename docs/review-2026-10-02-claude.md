# Claude Code Qt skill review

Review date: October 2, 2026. Reviewed revision: `b85b732`. Application code is unchanged since `2d904ab`.

This review used the Claude Code copy of the `qt-linux-gui` skill at `/home/clpe/.claude/skills/qt-linux-gui/`. It applied the skill's C++, ownership, thread, security, and Linux guidance. The project declares no safety level or certification target. This review makes no certification claim.

The three defects in the [previous review](review-2026-10-02.md) remain open. The code at each location is unchanged. This review adds five P2 findings and several P3 findings. P2 means normal-priority corrective work. P3 means low-priority work.

## P2: An expiring selection moves to another event

Locations: `src/core/store.cpp:61`, `src/plugins/tracks.cpp:163`, and `src/plugins/map.cpp:59`.

`Store::expire` removes rows one at a time. When the selected row leaves the model, `QItemSelectionModel` moves the current index to a neighbor and emits `currentRowChanged`. The tracks table then calls `store.select` with the neighbor identity. That identity still exists, so the final check at `store.cpp:74` keeps it.

The user never selects the neighbor. The map recenters on it, and the editor preview shows it. Two reviewers found this defect independently. A Qt 6.8.2 probe confirmed the index movement.

Ignore current-index changes during store removal, or clear the selection before the removal starts. Add a test that attaches the table and expires the selected event.

## P2: An unwritable profile logs an error every 750 milliseconds

Locations: `src/app/window.cpp:277`, `src/app/window.cpp:356`, and `src/core/preferences.cpp:56`.

The save timer repeats while the window is visible. `Preferences::save` returns false at once when the profile is not writable. Each failure logs an Error with Attention urgency. A second instance, an invalid profile, or an unsupported version produces about 80 errors each minute. The status bar stays red. The default history of 500 records holds only this message after about six minutes.

This contradicts `docs/usage.md`, which says a second instance uses defaults without saving. A persistent write failure, such as a full disk, also retries the write on every tick.

Report a save failure once until the state changes. Skip timed saves when the profile is read-only. Add a test that keeps a second instance open for several timer intervals.

## P2: The receiver floods internal events

Locations: `src/plugins/udp.cpp:150` and `src/plugins/udp.cpp:152`.

The receiver drains its socket every 25 milliseconds. Each drain that accepts a message logs a status record. At steady traffic, this record replaces useful history within seconds. When text logging is enabled, it also uses the shared budget of 20 lines per second. Other diagnostics are then dropped.

`lastError_` clears only when the receiver starts. After one rejected datagram, every later status record uses Warning severity.

Update the pane text on each drain, but log only state changes or a periodic summary. Clear or age the last error. Add a test for severity after a rejection followed by valid traffic.

## P2: A tile that keeps failing stalls the map

Locations: `src/core/tiles.cpp:145`, `src/core/tiles.cpp:154`, and `src/core/tiles.cpp:189`.

The renderer does not remember failed tiles. Each failure pauses all requests for at least 30 seconds. After the pause, the same failed tile is first in request order again. With one tile that keeps failing, about three tiles load every 30 seconds. With four, no other tile loads. Overlay and regional sources often return 404 for empty tiles.

During the pause, `pump` returns before it cancels requests for tiles that left the view. Those requests can fail and extend the pause.

Record failed tiles with a bounded backoff. Let other visible tiles load during that backoff. Cancel obsolete requests before the pause check. Add a test with failing and valid tiles in the same view.

## P2: A URL-only attribution is invisible

Location: `src/plugins/map.cpp:142`.

The attribution code removes every URL from the text and links the remaining text. When the user enters only a URL, the remaining text is empty. The label shows an empty box with no visible text and no usable link. The field label invites this input. This breaks the rule in `docs/map-providers.md` that attribution always remains visible.

The code also removes a second URL without linking it. Trailing punctuation, such as a closing parenthesis, stays in the link target.

Use the URL as link text when no other text remains. Add a test for URL-only attribution.

## P3 findings

- `src/app/window.cpp:197` and `:205`: Show pane and the submenu check mark use the dock state before `restorePreferences`. After a restart with a hidden pane, both stay checked. The first click does nothing. A probe confirmed this.
- `src/app/window.cpp:425`: Applying any pane setting calls `addDockWidget`. This removes the pane from its tab group, even when the area did not change. A probe confirmed this.
- `src/app/window.cpp:424` and `:428`: The Settings dialog stores docked `x` and `y` values relative to the main window. Detached placement applies them as screen coordinates.
- `src/plugins/tracks.cpp:171`: After an empty map click clears the selection, a click on the same table row does not select the event again. `clearSelection` keeps the current index. A probe confirmed this.
- `src/core/activity.cpp:51` and `:52`: The internal events table sorts severity and urgency as translated text. It sorts time as local text, which fails during the repeated daylight-saving hour.
- `src/core/tiles.cpp:128`: An expired tile disappears before its replacement arrives. Short-lived tiles blank the map until new requests finish.
- `src/core/tiles.cpp:195`: A response without disk-cache metadata receives a seven-day memory lifetime. A response that the disk cache did not store can therefore outlive its view.
- `src/plugins/wms.cpp:56` and `src/plugins/wmts.cpp:66`: `QUrlQuery` does not encode `+` in layer, style, or time values. Many servers decode it as a space. A WMTS time such as `12:00:00+02:00` then fails.
- `src/core/preferences.cpp:27`: Every lock failure reports another instance, including permission errors. A read-only portable profile is then ignored. `setStaleLockTime(0)` keeps a lock from another host forever.
- `src/plugins/cot.cpp:44`: Namespace declarations do not count toward the limit of 32 attributes. The 16384-byte message limit still bounds the work.
- `src/core/logging.cpp:85`: The logger reopens standard error with its own file offset. With `2>file`, other writers to standard error can overwrite logger lines.
- `src/core/activity.cpp:16`: Internal-event text drops all characters outside the Basic Multilingual Plane, such as emoji in provider names.
- `src/core/plugin_catalog.cpp:19` and `:20`: The manifest patterns accept a trailing newline because `$` matches before a final newline. Manifests are trusted build input.
- `CMakeLists.txt:61`: Installation copies all of `docs/`, including this review and the session handoff with developer paths.
- `CMakeLists.txt:24`: Release builds do not set `_FORTIFY_SOURCE`. Debian GCC does not add it by default.

## Observations

Tile zoom uses logical pixels. At 200 percent scaling, tiles appear enlarged and soft. `QNetworkDiskCache` reads metadata and removes expired entries on the GUI thread. A slow home directory can delay the interface.

The reviewers found these areas clean: CoT XML limits, event validation, store bounds, regular-file reads, preference writes, the activity inbox, lambda connection contexts, window teardown order, URL validation, redirect and cookie handling, reply lifetimes, image limits, and projection bounds.

## Evidence

The environment uses Debian 13 on x86-64, GCC 14.2, Qt 6.8.2, CMake, and Ninja from the temporary toolchain.

- All six sanitizer suites passed. Report: `build/quality/20261002-133713-quick-99060`.
- Cppcheck passed, including the unused-function scan. Report: `build/quality/20261002-133826-analyze-99573`.
- Qt probes confirmed the selection, dock, and menu behavior. They ran in the session scratchpad and are not in Git.

The first Cppcheck attempt failed because the loader could not find `libtinyxml2.so.11`. Add `/tmp/swak-quality-tools/root/usr/lib/x86_64-linux-gnu` to `LD_LIBRARY_PATH` when you use the temporary Cppcheck.

## Remaining evidence gaps

This review did not rerun the native GUI suite, coverage, or long workloads. It made no network requests. The tests do not cover the P2 findings above. Native Wayland, hardware GPU execution, the declared Qt 6.4 minimum, and multi-hour sessions remain untested.
