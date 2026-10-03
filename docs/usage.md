# SWAK user manual

A fresh profile starts with an empty map and no receiver socket. Later launches restore the saved interface and receiver state. CoT Event Generator opens beside the map in the default layout.

Tools > Plugins restores hidden docks, which are movable tool panels. Select a plugin checkbox to show or hide its pane. A checked box means that the pane is open. Hover over its name, or press Right, to open its command submenu. Return or Space also toggles the selected pane. Close the pane to hide it.

CoT Messages, CoT Event Generator, UDP receiver, and Plugins each have an independent dock. The three tool docks start as tabs beside the map. Drag a dock title or tab to detach that pane.

Each detached pane has a resize handle in its lower-right corner. Close a pane to hide it. Use File > Quit to close the application.

Use Tools > Plugins > CoT Messages > Clear to remove all current events and clear the selection. Plugins add their commands under Tools > Plugins.

The top toolbar groups plugin icons under each plugin name. Its Clear button performs the same action as the CoT Messages menu command. Point to an icon to read its tooltip. Use View > Show main toolbar to hide or show the toolbar. The application saves this choice across restarts.

Use Help > About to read the application version and general information.

To create a local event, use this sequence:

1. Enter an identity, callsign, event type, and position in CoT Event Generator.
2. Set the event lifetime in seconds.
3. Select Generate event.

The identity names one tracked object. Reusing an identity replaces its stored event when the new timestamp is equal or newer. Select New identity before creating another object.

Latitude and longitude use degrees on WGS 84. Altitude is height above the WGS 84 reference surface, in meters. Generated events use `h-e`, which indicates a human estimate.

The editor generates `a-u-G` events by default. Map markers use blue for `a-f` types, red for `a-h` types, and yellow for other types. These circles do not implement a military symbol standard.

Select a table row or map marker to focus that event. The XML preview then shows that event. Selection changes the preview but keeps the editor fields unchanged.

Drag the map with the left mouse button to move it. Double-click with the left button to center that point without changing zoom. Use the wheel to zoom around the pointer. With map focus, hold W, A, S, or D to move north, west, south, or east. Diagonal movement keeps the same speed. Release the keys to stop. Losing focus also stops movement. Arrow keys move one step. Q and E zoom in and out. Plus and minus also change zoom, and Home resets the view.

The map uses Web Mercator, a rectangular map projection. It clips positions near the poles to approximately 85.05 degrees. The event table retains the original coordinates, including polar positions.

# Receive and save events

UDP carries separate network messages called datagrams. Each datagram must contain one UTF-8 CoT XML event. The receiver does not authenticate senders or encrypt messages.

To receive events, use this sequence:

1. Open the UDP receiver pane from Tools > Plugins.
2. Enter a numeric local address and port.
3. Select Start receiver.

The initial address, `127.0.0.1`, accepts traffic only from the local computer. An active receiver resumes on the next launch. Select Stop receiver before closing if reception must stay disabled. Bind a specific network interface only on a protected network. Binding `0.0.0.0` exposes the receiver on every IPv4 interface.

Select Stop receiver to close the socket. Closing the window also closes the socket. The panel shows accepted and rejected message counts and the latest rejection reason.

Select Import XML to read one event from a regular file. Select Export XML to save the current preview. Canceling either file dialog leaves the current data unchanged.

The preview contains supported fields only. Unknown detail fields are discarded during import and are absent from exported XML. The application retains no original message archive.

Expired events disappear within approximately one second. Events with a future start time appear on the map when that time arrives. The table includes these pending events before their start time.

The application stores up to 4096 identities in memory. Closing the application discards those events. Export any required events before closing the window.

# Preferences

SWAK saves interface changes automatically at intervals of 750 milliseconds and again when the main window closes. Unchanged preferences do not cause another write. A forced process termination can lose changes made since the last save.

Saved preferences include these interface choices:

- Main window position, size, and maximized state.
- Pane positions, sizes, visibility, floating state, dock grouping, and active tabs.
- Map position and zoom.
- Event table column order, widths, visibility, and sorting.
- Editor identity, callsign, type, coordinates, altitude, and lifetime.
- Last directories selected for file import and export.
- Receiver address, port, and requested running state.

Live events, the selected event, message counts, and generated XML previews are session data. They are not preferences and do not survive a restart. Editor drafts survive without automatically generating events.

Use View > Reset layout to return panes to their initial arrangement. This command keeps editor values, map position, and receiver configuration. Each pane remains available under Tools > Plugins. View contains only Show main toolbar and Reset layout.

The default file is `preferences.json` in the Qt application configuration directory. On Linux, Qt uses the user configuration location under `XDG_CONFIG_HOME` or `~/.config`. The application identifies itself with organization `SWAK` and name `swak`.

To use a portable profile, launch the application with an explicit file path. Run this example from the repository root after building the release executable. The application creates the parent directory when needed.

```sh
./build/release/swak --preferences /path/to/profile/preferences.json
```

Close SWAK before copying its preferences file to another computer. Copy only the JSON file, not its `.lock` file. Launch SWAK with `--preferences` and the copied file path on the destination computer.

Unavailable plugins keep their saved preferences. When a plugin returns, its controls and pane can recover their previous state. New panes use their default layout when no saved state exists.

Windows from disconnected screens move onto an available screen. Qt stores geometry independently of CPU byte order. On Wayland, the compositor controls window placement and can limit exact position restoration.

A preferences error appears in the status bar. If a file is invalid or uses an unsupported version, keep a copy before replacing it. Use a different `--preferences` file to start a fresh profile.

Only one running instance can write a given profile. A second instance reports the conflict and uses defaults without saving. Different profile paths support independent instances.

If a saved receiver cannot bind its endpoint, the receiver pane reports the error. Its requested running state remains saved for the next launch. Select Stop receiver to cancel that request.

# Settings

Open File > Settings to edit application and plugin configuration. Apply saves changes and keeps the dialog open. OK saves changes and closes the dialog. Cancel discards unapplied form changes. Provider tables apply and save their changes immediately. Invalid input prevents all pending changes from applying. A save error stays visible, and changes remain active for the current session.

The General tab contains application and plugin configuration. The Panes tab contains pane visibility, placement, and dimensions. Pane entries use their names without a prefix. Section lists and dropdown choices follow the system locale for alphabetical ordering.

Plugin sections include map position and zoom, editor fields and directories, receiver configuration, and table columns. Column positions must form a unique sequence from zero through four. Plugins without configurable parameters show an explanatory section. Protocol limits remain fixed security boundaries.

The dialog stores values in the same portable preferences file as the workspace controls. Changes survive restarts, and absent plugin sections remain in the file. The dialog also remembers its size, position, tab, and selected sections. The desktop compositor controls final window placement on Wayland.

# Text logging

Text logging starts disabled in every build of the application. To enable it, select Enable text logging to standard error in the Logging section. The choice survives restarts through the selected profile. Qt diagnostics before profile loading remain silent.

Severity describes the type of message: Debug, Info, Warning, Error, or Fatal. Urgency describes the required response: Routine, Attention, or Immediate. Both minimum thresholds must match before a message is written. Qt Debug and Info messages use Routine urgency. Qt Warning messages use Attention. Qt Critical and Fatal messages use Immediate urgency.

The category identifies the source of a message. Application categories start with `swak.`, and Qt categories start with `qt.`. Use the category prefix field to restrict output, for example `swak.preferences` or `qt.qpa`. An empty prefix accepts all categories. Include Qt diagnostics controls Qt output independently of the thresholds. Existing Qt category rules can restrict output further, but cannot enable logging when the application disables it.

The message-rate limit bounds output each second. The logger drops excess messages and bounds each message to 2048 characters. It escapes line breaks and removes terminal control characters. It drops output when a pipe or socket cannot accept it immediately. Output to regular files still depends on filesystem performance. The application does not create or rotate log files. A terminal or service manager can capture standard error after logging is enabled.

Logs can contain private information from Qt or future plugins. Review captured output before sharing it. Application messages do not include event payloads or editor drafts. Interactive error labels remain visible when text logging is off. Help, version, and command-line usage responses are interactive output rather than logs.

A fatal Qt diagnostic still terminates the process when its text is disabled. Logging does not replace error handling or recover memory corruption. Development test and sanitizer output remains independent of application logging. Libraries that write directly to file descriptors bypass the Qt handler and cannot receive severity metadata from this logger.

# Wayland mouse warning

The message `This plugin supports grabbing the mouse only for popup windows` comes from Qt, not an application plugin. Qt emits it from `QWaylandWindow::setMouseGrabEnabled` when the target is not a popup. Dock operations can request mouse capture through Qt. The source review did not identify the exact user gesture that triggered this instance.

The application logging controls classify this message as Warning severity with Attention urgency. Default configuration suppresses its text. This does not change Wayland mouse-capture behavior. The [Qt 6.8 source](https://github.com/qt/qtwayland/blob/6.8/src/client/qwaylandwindow.cpp) contains the original warning.

# Online maps

Open Tools > Map Providers to select or configure an online map. A fresh profile has no map provider and shows a plain green background. File > Settings contains source and rendering controls. Read [Map providers](map-providers.md) for sources, formats, attribution, and HTTPS behavior.

# Events and map selection

The map toolbar button selects an online provider or clears the selection. Unavailable providers remain visible but disabled. Map Providers opens the manager from that dropdown.

The status bar shows CoT Messages and retained Events counts, with a separator between them. New events do not open or raise a pane. The status bar does not show transient event messages. Tools > Plugins > Events opens the retained history. Clear removes its rows and pending entries.

File > Settings > Events controls the history limit from 1 through 10000 events. The default is 500. History stays in memory for the current process. Text logging remains disabled unless you enable it separately. Very large bursts discard older pending records to keep memory bounded.

# Failure recovery

A read-only profile does not trigger repeated save attempts. The application reports a persistent save error once. After a write failure, automatic saves retry every 30 seconds. Apply and OK still attempt an immediate save and report its result.

A lock permission error permits read-only use of an existing valid profile. A profile locked by another running local instance still uses defaults. Qt uses a 30-second stale-lock threshold for locks without a matching live local process. Use profiles on local storage. Do not share one active profile between computers.

Invalid map providers remain in the profile and cannot fetch tiles. Other valid providers remain usable. Correct the invalid entry in Map Providers.

The receiver updates its pane for each batch. Events contain at most one traffic summary each second. A later clean batch clears the pane error. Summaries classify new rejections without retaining an old warning forever.


# Map background and zoom

The map uses NATO-style green (`#303D2B`) beneath its tiles. No map provider clears the selection. The old schematic map is removed.

Each missing tile shows faint MISSING TILE text at 45 degrees and a faint edge. File > Settings > Map controls Missing tile marker opacity. A loaded tile replaces its marker. Missing providers and unavailable tiles use the same background.

Zoom uses whole levels within the active provider minimum and maximum. Each wheel step and plus or minus key changes one level. Small wheel movements accumulate until they reach a step. Home resets the center and selects level 1, within the provider limits.

Provider changes and restored profiles also apply those limits. With no valid provider, whole levels 0 through 19 remain available. Old fractional values round to the nearest whole level.

# Data overlays

Tools > Data Providers manages KML and KMZ feeds. Enable a preset or add a direct HTTPS URL. Each enabled feed refreshes on its configured interval and draws above the map tiles.

Use Edit to set its refresh interval and appearance. Remove deletes a feed, and Refresh now requests an immediate update. The same parameters appear in File > Settings > Data providers. Read [Data providers](data-providers.md) for public sources, supported geometry, and limits.

OpenFreeMap is available in Map Providers and the provider dropdown. It supplies native vector maps with layers, labels, and points of interest. Right-click the map to change Map layers or inspect Map information. The keyboard Menu key uses the map center. File > Settings > Map providers contains the same layer controls and vector appearance parameters. Read `map-providers.md` for source terms and supported formats.

The bottom status bar shows separate map fields for cache size, zoom, pointer coordinates, tile diagnostics, and the HTTPS warning. Vertical separators divide these fields. Empty diagnostics and warnings disappear.

File > Settings > Map contains the Tile disk cache limit. The default is 128 MiB, with a range from 16 through 4096 MiB. The size describes the active HTTPS cache, which is separate from decoded memory. An inactive cache label means that no usable map provider cache is open.

File > Settings contains tables for Map providers and Data providers. The same tables appear in Tools > Map Providers and Tools > Data Providers. A map radio button selects one provider. Data checkboxes enable independent feeds. Row selection controls Edit and Delete without changing activation. Clear selection turns off map providers. Table names follow the operating system sort order.

# Map movement and selection

File > Settings > Keybindings contains the keyboard controls for Map and Information. General > Map contains speed, wheel controls, and tile margin. Keys must differ and must not use modifiers. Letters, digits, function keys, and arrow keys are supported. The default speed is 300 logical pixels per second. Wheel zoom works only over the map. You can disable it, reverse it, or change its step size.

The lowest zoom fills the map area. Zoom uses whole levels. Panning stops at the world edges. A window resize can raise the lowest zoom. The cropped part of the world remains reachable by panning. If the viewport exceeds the provider limit, SWAK enlarges tiles from the highest available level.

Data geometry follows the current map position and zoom. SWAK clips geometry and selection marks to the world bounds. It requests one extra tile ring by default, after visible tiles. Set the tile margin from zero through two in Settings. Each extra area contains at most 64 tiles. Extra tiles never displace visible vector tiles under memory pressure.

Click a map object to open Information. The pane shows its label, type, description, and properties. The fields are read-only. Double-click a value, title, type, or description to copy its text through the OS clipboard. Descriptions remain plain text, including any HTML markup. Click empty map space to clear the selection and hide the pane.

Selected points have a white outline. Selected routes use the inverse of their drawing color. Selected polygons have a dashed white edge and a faint white fill.

Selection uses a snapshot of the object. A change to the corresponding data source clears the snapshot. Selection does not survive restart. The pane layout and table header do survive restart.

# Map coverage and dense tiles

The map status section shows a rendered/total tile count while coverage is incomplete. A two-second audit schedules unfinished work. Failed downloads keep their retry delay. Invalid data remains an error. Coverage counts cached images, so it does not establish data freshness.

Dense tiles accept more geometry within fixed input and memory limits. If visible geometry exceeds Tile memory cache, SWAK keeps compact tile images and releases object details. The map reports this mode. Increase Tile memory cache in File > Settings > Map to restore inspection. Changing the provider also restarts normal decoding. KML and CoT selection remain available.

Tiles that exceed parser limits use a lower detail level when the provider supports one. Missing minimum-level tiles remain marked missing. The coverage count includes tiles beyond the request cap. If a very large viewport exceeds that cap at the provider minimum level, zoom in to enlarge each tile. Network failure and invalid content can still prevent complete coverage.

# Keyboard configuration

Open File > Settings > Keybindings. Select a plugin section. Select a key field and press the replacement key. Select Apply to save the changes. The saved keys survive restarts. Existing custom movement keys remain unchanged during migration.

Map controls require map focus. Q zooms in, and E zooms out. The four movement keys remain required and distinct. Zoom, reset, alternate zoom, and step keys can be empty to disable them. Conflicting map keys prevent Apply. The standard shortcuts for text fields and application menus remain unchanged.

Esc closes Information and clears the map object and CoT selection. This command works in the main window and detached panes. Modal dialogs and popup menus keep their normal Escape behavior. Information supports Esc or one function key. When an object is selected, its dismissal key takes priority over a matching map key.

The map keeps clickable attribution at its bottom-right edge. Status text uses an ellipsis when the window is narrow. Hover over the status section to read its full text. Settings field labels stay on one line. Narrow forms provide horizontal scrolling for long labels. Settings notices and result messages also stay on one line, with their full text available on hover.

# Local markers

Focus the map and press M to create a marker. The pointer changes to the standard crosshair. Black horizontal and vertical guide lines follow the pointer at 50 percent opacity. Click a map position to place a draft circle. The Edit marker pane opens with editable title, description, and properties. An empty title uses New Marker.

Use Add property and Remove property to change the property table. Property names must be unique. Select Apply to store the marker. The regular Information pane then shows its saved values. Cancel, Esc, or closing the editor discards the draft. Committed markers survive application restarts in the portable preferences file.

Click a saved marker to select it. Right-click it for Edit, Move, or Delete. Edit opens the editable pane again. Move attaches a preview to the pointer until the next left click. Esc cancels the move and restores the original position. It also restores the marker selection. Guide lines remain visible during a move.

With the map or Information pane focused, Backspace or Delete removes the selected local marker. Imported objects remain read-only. Ctrl+Z and Edit > Undo restore deleted markers, including their position and properties. The history keeps the last 100 deletions during the current session. It does not survive a restart. Creation, editing, and movement do not enter this deletion history.

File > Settings > General > Markers changes circle color, radius, and guide opacity. File > Settings > Keybindings > Markers changes the creation key. It accepts one letter without modifiers. A tool activation key takes priority over a map navigation key with the same value. Choose different keys for these actions. The model allows at most 256 objects and reserves storage for undo. ID, Latitude, and Longitude are reserved property names.


During marker placement or movement, the coordinate grid shows longitude along the top and bottom edges, and latitude along the left and right edges. Lines and labels use black at 50 percent opacity. Intervals follow zoom and latitude, with rounded decimal degrees and compass letters. File > Settings > General > Map controls grid visibility, opacity, and spacing in logical pixels.


Press the marker activation key again to leave placement mode. The default key is M. The same key cancels movement and restores the original position. The status bar shows M during placement or movement and clears it when that mode ends. Press Enter in the Title field to apply marker details. Enter uses the same validation as Apply.

Help > About shows the application version and full Git SHA. The SHA identifies the source commit. The version includes a short SHA and adds `-dirty` when tracked files contain uncommitted changes. About also shows an exact Git tag when one exists. Builds without Git metadata show `+unknown`. These values identify the build but do not query a server for newer releases.


The coordinate grid hides when marker mode ends. The grid stays below data-feed and local markers. File > Settings > General > Map can also disable the grid during marker mode.


After Apply or Enter saves a marker, keyboard focus returns to the map. Press M to create the next marker without closing Information or clicking the map first. This also applies when the editor or Information pane is detached.


# Overlay panes and marker colors

Attached panes cover the map instead of reducing its size. Opening Information keeps the map center and scale unchanged. Pane tabs, resizing, close buttons, and detachment still work. File > Settings > Panes controls the overlay edge and dimensions. Saved layouts from earlier versions become overlay layouts.

The marker editor shows Title, Collection, Type, Color, Description, and properties in that order. Type lists symbols from the selected collection. Core offers Generic marker with a circle icon. The palette includes eight general colors and colors from registered collections. Select a color button before Apply.

Orange is the default color. File > Settings > Markers changes the default for new markers. Existing markers retain their own colors.

# Ground-unit symbols

The marker editor separates Core and MILSTD2525 types through its Collection field. Select a type before Apply. Each military type starts with its identity color. The color buttons can override that color. The normal Information panel shows the saved symbol name.

Received CoT events use the same vector symbols for supported exact ground-unit types. Other types remain circles. Read [the symbol scope](atak-reference-review.md) before using these symbols for operational interpretation. The implementation is a limited subset of MIL-STD-2525C.

# Marker collections

Tools > Marker Collections manages Core, MILSTD2525, and imported symbol collections. File > Settings > Marker collections provides the same table. After placing a marker, choose Collection before Type. Add copies a collection, Edit changes its metadata, and Delete removes a user collection. Import and Export exchange bounded ZIP or TAR archives. Read [Marker collections](marker-collections.md) for formats, limits, and behavior when a collection disappears.

# Authenticated maps

In Map Providers, select a row and click Edit to choose its authentication method. Apply the provider configuration, then click Credentials to enter a session password or token. The dialog masks the value and never writes it into the profile. Read [Map providers](map-providers.md#credentials) for restart use through environment variables and the Thunderforest templates.
