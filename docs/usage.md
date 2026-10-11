# SWAK user manual

A fresh profile starts with an empty map and no receiver socket. Later launches restore the saved interface and receiver state. CoT Event Generator opens beside the map in the default layout.

Tools > Plugins restores hidden docks, which are movable containers for tool panes. Select a plugin checkbox to show or hide its pane. A checked box means that the pane is open. Hover over its name, or press Right, to open its command submenu. Return or Space also toggles the selected pane. Close the pane to hide it.

CoT Messages, CoT Event Generator, Servers, and Plugins each have an independent dock. The three tool docks start as tabs beside the map. Drag a dock title or tab to detach that pane.

Each detached pane has a resize handle in its lower-right corner. Close a pane to hide it. Use File > Quit to close the application. SIGTERM and SIGINT, for example from a session logout or Ctrl+C in a terminal, also close it and save the preferences.

Use Tools > Plugins > CoT Messages > Clear to remove all current events and clear the selection. Plugins add their commands under Tools > Plugins.

The top toolbar groups plugin icons with separators. Map Providers, Servers, and Data Providers share one toolbar section. Clear remains in Tools > Plugins > CoT Messages. Hover over an icon to read its tooltip. Use View > Toolbar to hide or show the toolbar. The application saves this choice across restarts.

Use Help > About to read the application version and general information.

To create a local event, use this sequence:

1. Enter an identity, callsign, event type, and position in CoT Event Generator.
2. Set the event lifetime in seconds.
3. Select Generate event.

The identity names one tracked object. Reusing an identity replaces its stored event when the new timestamp is equal or newer. Select New identity before you create another object.

Latitude and longitude use degrees on WGS 84. Altitude is height above the WGS 84 reference surface, in meters. The default location method is `h-e`, which indicates a human estimate. File > Identity can change that default.

The sender identity supplies the default event type, initially `a-f-G-U`. CoT map symbols use the supported military-symbol subset and affiliation colors.

Select a table row or map marker to focus that event. The XML preview then shows that event. Selection changes the preview but keeps the editor fields unchanged.

Drag the map with the left mouse button to move it. Double-click with the left button to center that point without changing zoom. Use the wheel to zoom around the pointer.

With map focus, hold W, A, S, and D to pan north, west, south, and east. Diagonal movement keeps the same speed. Release the keys to stop. Losing focus also stops movement. Q and E zoom in and out. Plus and minus also change zoom, and Home resets the view.

The map uses Web Mercator, a rectangular map projection. It clips positions near the poles to approximately 85.05 degrees. The event table retains the original coordinates, including polar positions.

# Display mode

View > Display Mode selects Standard or Modern. Standard follows the desktop style. Modern is a dark interface in the style of film user interfaces. It uses glass panels, cyan accents, the Rajdhani interface face, the wide Michroma face for pane titles, and JetBrains Mono for the status bar. Map labels become dark boxes with a cyan border. SWAK saves the mode in the profile. Other windows of the desktop keep their look.

In Modern, the map shows a HUD over the part of the map that panes leave free. A thin frame with corner brackets surrounds it. The upper left shows the title, the number of tracks, and the MGRS and DMS coordinates of the center. The upper right shows a recording mark with UTC time, the map source, and the sensor look. A fine crosshair marks the center. View > Scope darkens the map outside a central circle with bearing marks. The scale and the attribution keep their places at the bottom.

View > Sensor Look gives the Modern map a film sensor look: Normal, Night Vision, Thermal, CRT, or Noir. The look grades the map image, the layers, and the selection. The HUD, the scale, and the zoom levels stay sharp above it. The looks only change colors. Thermal does not show temperatures, and Night Vision does not show light levels. Standard always draws the normal map, and keeps the choice for Modern.

Esri World Imagery, NASA GIBS VIIRS True Color, and NASA GIBS Black Marble are map presets that suit Modern. Select them in Tools > Map Providers. In both display modes, the map credit stays in the part of the map that panes leave free. Providers require a visible credit.

# Receive and save events

Open Tools > Servers to add TLS or TCP stream connections. Enable each required connection with its Active checkbox. The application can receive from multiple servers at once. Read [CoT connections](servers.md) for certificates and optional serverless LAN peers.

For a UDP input, select UDP input in its edit form. Enter a numeric local address and port. Each datagram must contain one UTF-8 CoT XML event. UDP does not authenticate senders or encrypt messages.

A UDP bind address of `127.0.0.1` accepts local traffic only. Binding `0.0.0.0` accepts traffic on every IPv4 interface. Clear Active to close the input. The browser retains the startup choice and reports received, rejected, and dropped messages.

Select Import XML to read one event from a regular file. Select Export XML to save the current preview. Canceling either file dialog leaves the current data unchanged.

The preview contains supported fields only. Import discards unknown detail fields, so exported XML does not contain them. The application retains no original message archive.

Expired events leave the live table within approximately one second. Their map symbols fade to gray before removal, according to CoT animation configuration. Events with a future start time appear on the map when that time arrives. The table includes these pending events before their start time.

The application stores up to 4096 identities in memory. Closing the application discards those events. Export any required events before you close the window.

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

Use View > Reset Layout ... to return panes to their initial arrangement. This command keeps editor values, map position, and receiver configuration. Tools > Plugins contains pane toggles and the Marker placement command. View also controls the toolbar, grid, map scale, and zoom legend.

The default file is `preferences.json` in the Qt application configuration directory. On Linux, Qt uses the user configuration location under `XDG_CONFIG_HOME` or `~/.config`. The application identifies itself with organization `SWAK` and name `swak`.

To use a portable profile, launch the application with an explicit file path. Run this example from the repository root after you build the release executable. The application creates the parent directory when needed.

```sh
./build/release/swak --preferences /path/to/profile/preferences.json
```

Close SWAK before you copy its preferences file to another computer. Copy only the JSON file, not its `.lock` file. Launch SWAK with `--preferences` and the copied file path on the destination computer.

Unavailable plugins keep their saved preferences. When a plugin returns, its controls and pane can recover their previous state. New panes use their default layout when no saved state exists.

Windows from disconnected screens move onto an available screen. Qt stores geometry independently of CPU byte order. On Wayland, the compositor controls window placement and can limit exact position restoration.

A preferences error appears in the status bar. If a file is invalid or uses an unsupported version, keep a copy before you replace it. Use a different `--preferences` file to start a fresh profile.

Only one running instance can write a given profile. A second instance reports the conflict and uses defaults without saving. Different profile paths support independent instances.

If a saved input cannot bind its endpoint, Servers reports the error and retries. Its active state remains saved. Clear its Active checkbox to stop retries.

# Settings

Open File > Settings to edit application and plugin configuration. Apply saves changes and keeps the dialog open. OK saves changes and closes the dialog. Cancel discards unapplied form changes.

Provider tables apply and save their changes immediately. Invalid input prevents all pending changes from applying. A save error stays visible, and changes remain active for the current session.

The General tab contains application and plugin configuration. The Panes tab contains pane visibility, placement, and dimensions. Pane entries use their names without a prefix. Section lists and dropdown choices follow the system locale for alphabetical ordering.

Plugin sections include map position and zoom, editor fields and directories, receiver configuration, and table columns. Column positions must form a unique sequence from zero through four. Plugins without configurable parameters show an explanatory section. Protocol limits remain fixed security boundaries.

The dialog stores values in the same portable preferences file as the workspace controls. Changes survive restarts, and absent plugin sections remain in the file. The dialog also remembers its size, position, tab, and selected sections. The desktop compositor controls final window placement on Wayland.

# Text logging

Text logging starts disabled in every build of the application. To enable it, select Enable text logging to standard error in the Logging section. The choice survives restarts through the selected profile. Qt diagnostics before profile loading remain silent.

Severity describes the type of message: Debug, Info, Warning, Error, or Fatal. Urgency describes the required response: Routine, Attention, or Immediate. Both minimum thresholds must match before the logger writes a message. Qt Debug and Info messages use Routine urgency. Qt Warning messages use Attention. Qt Critical and Fatal messages use Immediate urgency.

The category identifies the source of a message. Application categories start with `swak.`, and Qt categories start with `qt.`. Use the category prefix field to restrict output, for example `swak.preferences` or `qt.qpa`. An empty prefix accepts all categories. Include Qt diagnostics controls Qt output independently of the thresholds. Existing Qt category rules can restrict output further, but cannot enable logging when the application disables it.

The message-rate limit bounds output each second. The logger drops excess messages and bounds each message to 2048 characters. It escapes line breaks and removes terminal control characters. It drops output when a pipe or socket cannot accept it immediately.

Output to regular files still depends on filesystem performance. The application does not create or rotate log files. A terminal or service manager can capture standard error after you enable logging.

Logs can contain private information from Qt or future plugins. Review captured output before you share it. Application messages do not include event payloads or editor drafts. Interactive error labels remain visible when text logging is off. Help, version, and command-line usage responses are interactive output rather than logs.

A fatal Qt diagnostic still terminates the process when its text is disabled. Logging does not replace error handling or recover memory corruption. Development test and sanitizer output remains independent of application logging. Libraries that write directly to file descriptors bypass the Qt handler and cannot receive severity metadata from this logger.

# Wayland mouse warning

The message `This plugin supports grabbing the mouse only for popup windows` comes from Qt, not an application plugin. Qt emits it from `QWaylandWindow::setMouseGrabEnabled` when the target is not a popup. Dock operations can request mouse capture through Qt. The source review did not identify the exact user gesture that triggered this instance.

The application logging controls classify this message as Warning severity with Attention urgency. Default configuration suppresses its text. This does not change Wayland mouse-capture behavior. The [Qt 6.8 source](https://github.com/qt/qtwayland/blob/6.8/src/client/qwaylandwindow.cpp) contains the original warning.

# Online maps

Open Tools > Map Providers to select or configure an online map. Google Maps and Azure Maps presets need your own API key. Read [Google Maps and Azure Maps](map-providers.md#google-maps-and-azure-maps). A fresh profile has no map provider and shows a plain green background. File > Settings contains source and rendering controls. Read [Map providers](map-providers.md) for sources, formats, attribution, and HTTPS behavior.

# Events and map selection

The map toolbar button selects an online provider or clears the selection. Unavailable providers remain visible but disabled. Map Providers opens the manager from that dropdown.

The status bar shows CoT Messages and retained Events counts, with a separator between them. New events do not open or raise a pane. The status bar does not show transient event messages. Tools > Plugins > Events opens the retained history. Clear removes its rows and pending entries.

The Events table has Time, Severity, Source, and Event columns. The urgency of an event controls text logging only, so the table does not show it. Hover over a long event to read it in a wrapped tooltip. Double-click a row, or select it and press Enter, to open the full event in a window. Right-click a row for View event ... and Copy. Ctrl+C also copies the selected event. Copy puts one line on the clipboard, with tabs between the time, severity, source, and event text. Each event holds at most 2048 characters.

File > Settings > Events controls the history limit from 1 through 10000 events. The default is 500. History stays in memory for the current process. Text logging remains disabled unless you enable it separately. Very large bursts discard older pending records to keep memory bounded.

# Failure recovery

A read-only profile does not trigger repeated save attempts. The application reports a persistent save error once. After a write failure, automatic saves retry every 30 seconds. Apply and OK still attempt an immediate save and report its result.

A lock permission error permits read-only use of an existing valid profile. A profile locked by another running local instance still uses defaults. Qt uses a 30-second stale-lock threshold for locks without a matching live local process. Use profiles on local storage. Do not share one active profile between computers.

Invalid map providers remain in the profile and cannot fetch tiles. Other valid providers remain usable. Correct the invalid entry in Map Providers.

The receiver updates its pane for each batch. Events contain at most one traffic summary each second. A later clean batch clears the pane error. Summaries classify new rejections without retaining an old warning forever.


# Map background and zoom

The map uses NATO-style green (`#303D2B`) beneath its tiles. To clear the provider selection, select Disable in the map toolbar dropdown. Disable sits directly before More ... at the end of the list. The map then shows only the green background. This version removes the old schematic map.

Each missing tile shows faint MISSING TILE text at 45 degrees and a faint edge. File > Settings > Map controls Missing tile marker opacity. A loaded tile replaces its marker. Missing providers and unavailable tiles use the same background.

Zoom uses whole levels within the active provider minimum and maximum. Each wheel step and plus or minus key changes one level. Small wheel movements accumulate until they reach a step. Home resets the center and selects level 1, within the provider limits.

Provider changes and restored profiles also apply those limits. With no valid provider, whole levels 0 through 19 remain available. Old fractional values round to the nearest whole level.

# Data overlays

Tools > Data Providers manages KML and KMZ feeds. Enable a preset or add a direct HTTPS URL. Each enabled feed refreshes on its configured interval and draws above the map tiles.

Use Edit to set its refresh interval and appearance. Remove deletes a feed, and Refresh now requests an immediate update. The same parameters appear in File > Settings > Data providers. Read [Data providers](data-providers.md) for public sources, supported geometry, and limits.

OpenFreeMap is available in Map Providers and the provider dropdown. It supplies native vector maps with layers, labels, and points of interest. Right-click the map to change Map layers or inspect Map information. The keyboard Menu key uses the map center. File > Settings > Map providers contains the same layer controls and vector appearance parameters. Read `map-providers.md` for source terms and supported formats.

The bottom status bar shows separate map fields for cache size, zoom, pointer coordinates, tile diagnostics, and the HTTPS warning. Vertical separators divide these fields. Empty diagnostics and warnings disappear.

File > Settings > Map contains the Tile disk cache limit. The default is 128 MiB, with a range from 16 through 4096 MiB. The size describes the active HTTPS cache, which is separate from decoded memory. An inactive cache label means that no usable map provider cache is open.

File > Settings contains tables for Map providers and Data providers. The same tables appear in Tools > Map Providers and Tools > Data Providers. A map radio button selects one provider. Data checkboxes enable independent feeds.

Row selection controls Edit and Delete without changing activation. Clear selection turns off map providers. Table names follow the operating system sort order.

# Map movement and selection

File > Settings > Keybindings contains the keyboard controls for Map and Information. General > Map contains speed, wheel controls, and tile margin. Keys must differ and must not use modifiers. Keys can be letters, digits, function keys, or arrow keys. The default speed is 300 logical pixels per second.

Wheel zoom works only over the map. You can disable it, reverse it, or change its step size.

The lowest zoom fills the map area. Zoom uses whole levels. Panning stops at the world edges. A window resize can raise the lowest zoom. The cropped part of the world remains reachable by panning. If the viewport exceeds the provider limit, SWAK enlarges tiles from the highest available level.

Data geometry follows the current map position and zoom. SWAK clips geometry and selection marks to the world bounds. It requests one extra tile ring by default, after visible tiles. Set the tile margin from zero through two in Settings. Each extra area contains at most 64 tiles. Extra tiles never displace visible vector tiles under memory pressure.

Click a map object to open Information. The pane shows its label, type, description, and properties. The fields are read-only. Double-click a value, title, type, or description to copy its text through the OS clipboard. Descriptions from data feeds, such as KML balloons, often contain HTML. Information shows them as plain text without the markup, and it loads no images or links. Local and CoT descriptions appear exactly as written. For CoT objects, the description is the TAK remarks text. Information also shows the contact endpoint, battery level, device, and link count when the sender includes them. Click empty map space to clear the selection and hide the pane.

Selected points have a white outline. Selected routes use the inverse of their drawing color. Selected polygons have a dashed white edge and a faint white fill.

Selection uses a snapshot of the object. A change to the corresponding data source clears the snapshot. Selection does not survive restart. The pane layout and table header do survive restart.

# Map coverage and dense tiles

The map status section shows a rendered/total tile count while coverage is incomplete. A two-second audit schedules unfinished work. Failed downloads keep their retry delay. Invalid data remains an error. Coverage counts cached images, so it does not establish data freshness.

Dense tiles accept more geometry within fixed input and memory limits. If visible geometry exceeds Tile memory cache, SWAK keeps compact tile images and releases object details. The map reports this mode. Increase Tile memory cache in File > Settings > Map to restore inspection. Changing the provider also restarts normal decoding. KML and CoT selection remain available.

Tiles that exceed parser limits use a lower detail level when the provider supports one. Missing minimum-level tiles remain marked missing. The coverage count includes tiles beyond the request cap. If a very large viewport exceeds that cap at the provider minimum level, zoom in to enlarge each tile. Network failure and invalid content can still prevent complete coverage.

# Keyboard configuration

Open File > Settings > Keybindings. Select a plugin section. Select a key field. Press the replacement key. Select Apply to save the changes. The saved keys survive restarts. Existing custom movement keys remain unchanged during migration.

Map controls require map focus. Q zooms in, and E zooms out. The four movement keys remain required and distinct. Zoom, reset, alternate zoom, and step keys can be empty to disable them. Conflicting map keys prevent Apply. The standard shortcuts for text fields and application menus remain unchanged.

Esc closes Information and clears the map object and CoT selection. This command works in the main window and detached panes. Modal dialogs and popup menus keep their normal Escape behavior. Information supports Esc or one function key. When an object is selected, its dismissal key takes priority over a matching map key.

The map keeps clickable attribution at its bottom-right edge. Status text uses an ellipsis when the window is narrow. Hover over the status section to read its full text. Settings field labels stay on one line. Narrow forms provide horizontal scrolling for long labels. Settings notices and result messages also stay on one line, with their full text available on hover.

# Local markers

To create a marker, focus the map. Then press Shift+M. The pointer changes to the standard crosshair. Black horizontal and vertical guide lines follow the pointer at 50 percent opacity.

Click a map position to place a draft circle. The Edit marker pane opens with editable title, description, and properties. An empty title uses New Marker.

Use Add property and Remove property to change the property table. Property names must be unique. Select Apply to store the marker. The regular Information pane then shows its saved values. Cancel, Esc, or closing the editor discards the draft. Committed markers survive application restarts in the portable preferences file.

Click a saved marker to select it. Right-click it for Edit, Move, or Delete. Edit opens the editable pane again. Move attaches a preview to the pointer until the next left click.

Esc cancels the move and restores the original position. It also restores the marker selection. Guide lines remain visible during a move.

With the map or Information pane focused, Backspace or Delete removes the selected local marker. Imported objects remain read-only. Ctrl+Z and Edit > Undo restore deleted markers, including their position and properties. The history keeps the last 100 deletions during the current session. It does not survive a restart. Creation, editing, and movement do not enter this deletion history.

Undo stays available while the history contains deletions to restore. After the last restore, the menu item becomes gray and Ctrl+Z does nothing. A new deletion enables Undo again.

File > Settings > General > Markers changes circle color, radius, and guide opacity. File > Settings > Keybindings > Markers changes the creation key. It accepts one letter without modifiers.

A tool activation key takes priority over a map navigation key with the same value. Choose different keys for these actions. The model allows at most 256 objects and reserves storage for undo. ID, Latitude, and Longitude are reserved property names.


The coordinate grid operates independently of marker modes. File > Settings > General > Grids manages its appearance and coordinate system.


Press the marker activation key again to leave placement mode. The default key is M. The same key cancels movement and restores the original position. The status bar shows M during placement or movement and clears it when that mode ends. Press Enter in the Title field to apply marker details. Enter uses the same validation as Apply.

Help > About shows the application version and full Git SHA. The SHA identifies the source commit. The version includes a short SHA and adds `-dirty` when tracked files contain uncommitted changes. About also shows an exact Git tag when one exists. Builds without Git metadata show `+unknown`. These values identify the build but do not query a server for newer releases.


The grid stays below data-feed and local markers. Choose No grid in Grids to hide it.


After Apply or Enter saves a marker, keyboard focus returns to the map. Press Shift+M to create the next marker without closing Information or clicking the map first. This also applies when the editor or Information pane is detached.


# Overlay panes and marker colors

Attached panes cover the map instead of reducing its size. Opening Information keeps the map center and scale unchanged. Pane tabs, resizing, close buttons, and detachment still work. File > Settings > Panes controls the overlay edge and dimensions. Saved layouts from earlier versions become overlay layouts.

The marker editor shows Title, Collection, Type, Color, Description, and properties in that order. Type lists symbols from the selected collection. Core offers Generic marker with a circle icon. The palette includes eight general colors and colors from registered collections. Select a color button before Apply.

Orange is the default color. File > Settings > Markers changes the default for new markers and the Create marker shortcut. The shortcut is one letter with or without Shift. The default is Shift+M. Profiles that still use the earlier default M change to Shift+M once. A custom letter stays as you set it. Existing markers retain their own colors.

# Ground-unit symbols

The marker editor separates Core and MILSTD2525 types through its Collection field. Select a type before Apply. Each military type starts with its identity color. The color buttons can override that color. The normal Information pane shows the saved symbol name.

Received CoT events use the same vector symbols for supported exact ground-unit types. Other types remain circles. Read [the symbol scope](atak-reference-review.md) before you use these symbols for operational interpretation. The implementation is a limited subset of MIL-STD-2525C.

# Marker collections

Tools > Marker Collections manages Core, MILSTD2525, and imported symbol collections. File > Settings > Marker collections provides the same table. After you place a marker, choose Collection before Type. Add copies a collection, Edit changes its metadata, and Delete removes a user collection. Import and Export exchange bounded ZIP or TAR archives. Read [Marker collections](marker-collections.md) for formats, limits, and behavior when a collection disappears.

# Authenticated maps

To use an authenticated map, do these steps in Map Providers:

1. Select a row.
2. Select Edit to choose its authentication method.
3. Apply the provider configuration.
4. Select Credentials to enter a session password or token.

The dialog masks the value and never writes it into the profile. Read [Map providers](map-providers.md#credentials) for restart use through environment variables and the Thunderforest templates.

Tools > Plugins > Marker activates marker placement. Click the map to open the marker editor. Press Escape to cancel placement. The configured marker key still toggles marker mode.

# Create and edit polygons

Press Shift+A while the map has focus, or select Area on the toolbar or under Tools > Plugins. The crosshair and black guide lines follow the pointer. Click to place each blue corner. After two corners, the preview fills the area through the current pointer position with 30 percent blue.

To close the polygon, click the first corner after you place at least three corners. Enter closes the placed outline. With exactly two placed corners, Enter adds the current pointer position as the third corner. A polygon needs three distinct corners and a nonzero area. Crossing or overlapping edges prevent completion. Each polygon supports at most 64 corners.

The editor contains Title, Type, Color, Description, and properties. Apply saves the polygon. Enter in Title also saves it. The map shows the title at the center of the area. For concave shapes whose center lies outside the area, the label uses an interior span. Completed polygons hide their corner dots.

Click inside a saved polygon to open Information. The selected area has a white outline at the configured polygon line width. Right-click inside it and select Edit to show its corners again. Right-click a corner to Move or Delete it. SWAK disables Delete when the deletion creates an invalid polygon. A move that creates an invalid polygon remains a preview until you choose a valid position.

Escape during a corner move restores its previous position in the draft. Escape again cancels the edit. Apply saves all corner and metadata changes together. Delete or Backspace removes the selected polygon. Edit > Undo or Ctrl+Z restores a deleted polygon.

Area previews label each edge with its length. Labels update for the live edge and the closing edge. They also appear during vertex editing. Distances use the operating system unit system.

Shift+A toggles Area mode. Shift+M switches to marker creation. A tool switch discards an unfinished shape or edit. File > Settings > Keybindings contains the area shortcut. The Areas section controls corner size, outline width, fill opacity, and guide opacity.

Information formats area with the system locale and measurement system. Metric locales use m² below 1000 m² and km² from that threshold. Imperial locales use ft², acres, and mi² as the area increases. Area uses a spherical Earth radius of 6371008.8 meters and integrates along the straight map edges. It is an estimate, not an ellipsoidal survey measurement. The shape follows the displayed world and does not wrap across the date line.

# Create and edit routes

A route is a line with several points, such as a patrol path. Press Shift+R while the map has focus, or select Route on the toolbar. The Route button is next to Line. Click the map to place each point. Each click adds a new segment from the previous point. The preview shows the length of each segment and the total length.

Press Enter to finish the route. A route needs at least two points and can have at most 250. The Edit route pane then opens. Enter the title, description, color, and properties, and select Apply. Escape cancels an unfinished route.

Right-click a route for the same commands as other markers: Edit, Move, Delete ..., and the shared tools such as Information. Move follows the pointer until you click the new position. In Edit, double-click a point to move it, or right-click a point to move or delete it. The map shows the route title and its total length. Information shows Length and the number of points.

File > Settings > Keybindings contains the route shortcut. The Routes section controls point size, line width, and guide opacity. Route planning stores its results as routes. Read [Route planning](routing.md).

# Select overlapping objects

When objects overlap, click their shared area to open the object chooser. Select the required object from the list. Each row shows its label and icon. Tooltips provide type and source details. Use More objects to reach another page. Press Escape to cancel without changing selection.

For object commands, right-click the overlap and choose an object first. Its context menu then opens. Local markers and polygons provide their normal edit and delete commands. Downloaded objects provide information. Active drawing and corner editing continue to use their own controls.


The main toolbar uses icons and separators. Hover over a button to read its command and shortcut. Marker and Area buttons show their active state. Click an active mode button again to stop it. The globe button opens the map provider list. Its tooltip names the current provider. Clear CoT Messages remains in its plugin menu.

Older profiles migrate default arrow pan keys to W/A/S/D. Area, Circle, Line, Distance, and Route use Shift+A, Shift+C, Shift+L, Shift+D, and Shift+R. Older default tool keys migrate to these combinations. Other custom keys remain unchanged.

Area, Circle, Line, and Distance accept letters with optional Shift. File > Settings > Keybindings can change these assignments. Saved geometry still uses the polygon type internally.


# Servers, Identity, and CoT animation

Tools > Servers manages simultaneous TLS and TCP connections and local UDP inputs. File > Identity configures sender defaults for the generator. File > Settings > CoT animation controls movement, teleport fades, and gray retention after expiration. Read [CoT connections](servers.md) for certificates, optional LAN peers, transmission controls, and harness commands.

# Marker motion and traces

CoT display motion estimates velocity from successive timestamped positions. It predicts for at most two update intervals and the configured prediction limit. The display corrects incoming positions smoothly. It slows prediction to a stop when updates cease. Stale markers stay stationary during their gray fade. Large jumps retain the teleport effect.

In File > Settings > CoT animation, set Prediction limit to zero to disable prediction. The default limit is 3000 milliseconds. Movement duration controls correction toward new positions. Coordinates in Information remain the received coordinates. Prediction changes only the display. Duplicate timestamps, extreme projected speeds, and teleport updates do not produce a velocity estimate.

Right-click a CoT or local marker and select Trace. The checked action starts a history of distinct received or saved positions. Select Trace again to stop and remove that history. Tools > Plugins > Trace > Clear traces removes all histories and tracing choices.

A trace retains at most 50 positions. The default opacity is 70 percent. Older segments appear more faintly, with a minimum of one-quarter of the configured opacity. File > Settings > Trace controls visibility, the position count, opacity, and line width. Clear Show traces to hide all traces. Recording continues, so the traces return complete when you show them again. Hidden traces cannot be clicked.

At most 128 markers can have tracing enabled. Tracing choices survive restart. Position histories last only for the session and clear when the source marker disappears. Imported map features without stable marker identities do not offer Trace.

Close Information with its top-right button to clear selection. Incoming updates cannot reopen it until you select another object. In Servers, use the Active column to control connections. The server editor does not repeat that checkbox. TLS credential fields remain gray until you select TLS.

# Restore configuration defaults

Open File > Settings and select a standard configuration section. Select Restore section defaults. Select Apply or OK to save the default values. Select Cancel to discard the staged reset. A reset affects only the fields in that section and tab. Provider management tables keep their existing add, edit, and delete controls.

For Trace, the defaults are 50 positions, 0.70 maximum opacity, and 1.5 pixels of line width. The reset preserves traced marker identities and current histories within the restored limit. Older saved profiles can contain ten positions and 0.35 opacity.

# Pause CoT positions

With the map focused, press Space to pause displayed CoT positions. Press Space again to resume. The status bar shows CoT positions paused during the pause. Reception and the message store continue operating.

Resume places each current marker at its latest received position without animation. Later updates use normal animation. New markers appear after resume. Stale records continue aging and disappear according to the configured retention rules.

File > Settings > Keybindings > Map controls the pause shortcut. Holding the key does not repeatedly toggle pause. A restart starts with updates enabled. Pause does not change local marker editing or provider refreshes.

Information now contains a Trace checkbox for supported CoT and local point markers. It uses the same state as the right-click Trace command. Marker labels use white text on a dark translucent background.

Information describes CoT Type and How with names from the shared selector catalog. It keeps the complete original code in parentheses. Unknown categories remain marked as unknown and retain their original codes. For example, m-s appears as Machine generated / Simulation (m-s).

# Circle, Distance, and Line

Click an object label to select its object. Click a FOV cone, a trace line, or a prediction line to select the object that owns it. Selected labels invert their foreground and background colors. Overlapping labels use the same object chooser as overlapping symbols.

With the map focused, press C to start Circle. Click the center. Move the pointer to set the radius. The preview shows the current radius in local units. Click again to open the object editor.

Enter the title, description, color, and properties. Select Apply to save the circle. Circle uses the fill opacity from File > Settings > Areas.

Press Shift+D to start Distance. Click the first point. Move the pointer to read the distance. Click the second point to finish. The measurement stays for ten seconds, then fades for one second and disappears.

Measurements are temporary and do not survive restart. The tool retains at most 64 measurements.

Press Shift+L to start Line. Click each endpoint. Complete the object editor. Then select Apply. The saved line shows its title and distance.

Information shows Distance for lines and Area for circles. Measurements use the operating-system locale and unit system. Distances use meters or feet and change to kilometers or miles at the corresponding threshold.

Right-click a saved circle or line to select Edit, Move, or Delete. During editing, right-click a control point to move it. Both control points are required, so their Delete action stays disabled. Moving a circle center retains its radius. Moving its edge control changes its radius.

Esc cancels a pending move. Delete and Backspace delete the selected saved object. Ctrl+Z restores a deleted object.

The toolbar and Tools > Plugins provide the same creation actions. File > Settings > Keybindings configures C, D, and L. The Circle and line tools section controls line width, control-point size, measurement duration, and fade time.

Circles remain circular on the projected map. Their area estimate integrates a 128-point boundary over a spherical Earth. Distance uses the shortest spherical surface distance and ignores elevation. Lines wrap across the date line, but their straight map segment approximates the surface path. These values are estimates for map work. Circles must remain inside the projected world.

# Identity and paused traces

The application generates a UID on first startup and saves it in the profile. Open File > Identity to view it. Select the lock button beside UID before you edit it. Select the button again to lock it.

Apply or OK saves the value. A new dialog starts with UID locked. Cancel discards unapplied changes. Section reset preserves UID.

Space freezes both displayed CoT positions and traces. Incoming messages and bounded trace history continue updating internally. Resume shows the latest positions and trace history immediately.

The main toolbar groups Marker, Area, Circle, Line, and Route in one creation section. A separator places Distance and Route planning in the measurement section.


# Menus and provider controls

The menu bar uses File, Edit, View, Tools, Help in that order. Dropdown menus sort alphabetically with the operating-system locale. File keeps Quit last, with a separator before it. Circle, Distance, and Line appear directly under Tools > Plugins. SWAK removes other menu separators so each menu stays sorted. Toolbar sections retain separators.

Use the Servers toolbar button to enable or disable configured servers. Use its Servers command to open management. Use the Data providers button to enable or disable feeds. Its Data Providers command opens management. Multiple servers and feeds can stay active together.

Scrolling inside an overlay pane affects that pane. At either scrollbar limit, wheel events do not zoom the map. Move the pointer onto the uncovered map before you zoom.

Identity shows Type, Location method, and Team descriptions with their original codes in parentheses. Use Select to change these fields or enter a custom code. Read-only text fields appear muted and still support selection and copying. The UID button shows a closed lock when it offers Unlock and an open lock when it offers Lock. OK uses a checkmark. Apply uses a disk icon and keeps the dialog open.

Selected vector labels use white text on a black background. The selection highlight does not cover the label background.

The Plugins pane lists built-in plugins in a sortable table. Select a row to read its full manifest below the table. A manifest describes the plugin version, purpose, interfaces, requirements, permissions, and supported platform. These fields are read-only.

# Marker size and predictions

Open File > Settings > Map to change Marker icon scale. The choices are 50%, 75%, 100%, 125%, 150%, and 200%. The default is 100%. Apply changes the map immediately and saves the value for later sessions. The setting scales local markers, CoT symbols, and points from Data Providers. Click targets, label positions, and selection outlines follow the new size.

Toolbar icons and label text retain their size. Geographic circles, areas, lines, and the map provider imagery retain their dimensions. Qt still applies the system display scale.

To enable a forecast, right-click a CoT object and select Predict. You can also use Predict in the Information pane. Select the same control again to stop the forecast. Tools > Plugins > Predict > Clear predictions stops all forecasts.

Predict collects recent CoT positions for each enabled object. After two distinct message times, it draws connected dashed segments through the 30-second, 1-minute, and 5-minute forecasts. Perpendicular marks identify each endpoint. The marks decrease in length from 30 seconds to 5 minutes. The line ends at the final tick.

The latest movement supplies the current speed and heading. With three positions, the estimate also accounts for acceleration and heading change. These trends decay over time to avoid indefinite acceleration or circling. They do not account for roads, terrain, planned routes, or future turns. A stationary object produces overlapping endpoints.

File > Settings > Predict controls visibility, the history window, line width, and opacity. Clear Show predictions to hide all prediction lines. The history continues, so the lines return when you show them again. Hidden lines cannot be clicked. The default history window is five minutes, with at most 50 samples per object. The latest three positions determine movement trends. The plugin supports at most 128 enabled objects. Enabled identities survive restart, but received history does not.

CoT animation controls forecast movement and fades. Long moves cover more distance within the same animation duration. Moves above the teleport threshold fade between positions. Forecast lines follow the displayed object position and fade with stale objects. Space freezes both objects and forecasts while message reception continues. Resume shows the latest positions immediately.

Large source jumps, gaps beyond the history window, and speeds above 1000 meters per second reset the estimate. A changed position with the same timestamp also resets it. The plugin waits for new history before it produces another forecast. It ignores older timestamps and samples less than 50 milliseconds apart. Coordinates beyond the map latitude limit use the map boundary.

To mark an Area as restricted, enable Restricted in its editor and select Apply. The area keeps its faded color and adds diagonal lines at 45 degrees. The preview changes immediately. Cancel discards the change. The flag survives editing, deletion followed by Undo, and restart. New areas start with Restricted disabled.


# Map lock and deletion

The Lock button sits at the right end of the main toolbar. Select it to stop map panning and zooming. Select it again to restore navigation. Object selection and creation remain available while the map is locked. The lock state survives restart. File > Settings > Map also exposes this control.

Arrow keys stay within overlay controls when a table reaches its edge. Select the map to use its navigation keys.

Delete ... opens a confirmation dialog for objects, area corners, providers, servers, and marker collections. Press Enter to delete the selected item. Press Escape to cancel. Object deletion remains available through Undo. Provider, server, and collection deletion does not support Undo. Object context menus put Edit before Delete ....

The Information pane shows Restricted as Yes or No for each Area.


# Map view and reference controls

The main view appears in the Map tab. Its keyboard, mouse, overlay, and lock controls retain their behavior. The scale appears at the lower left. The scale uses the local ground distance at its latitude. Units follow the operating system locale, with meters and kilometers or feet and miles.

The faded indicator at the right edge shows the supported zoom levels. The scale and the zoom indicator have no background panel. A thin white outline keeps their black marks visible on dark maps. A longer tick marks the current level. View > Scale and View > Zoom Level control visibility. Both choices survive restart.

Open File > Settings > General > Grids to manage coordinate grids. Longitude/latitude is the default selection. UTM and MGRS are also available. Select a radio button to activate a type. Select No grid to hide all types.

View > Grid also hides or shows the grid. Showing it restores the last selected type. Both the hidden state and the remembered type survive restart. The menu item is disabled when no grid types remain.

Select Add to create a named type. Select a row before Edit or Delete .... Each type stores its coordinate system, color, opacity, target spacing, line width, and angle-label format. The angle format applies to longitude/latitude grids.

Edits apply immediately after the editor accepts them. Delete requires confirmation. A deleted active type leaves the grid hidden.

Longitude/latitude labels support decimal degrees, degrees and minutes, or degrees, minutes, and seconds. UTM and MGRS use WGS 84 and include the Norway and Svalbard zone exceptions. Polar regions use UPS. At broad zoom levels, these grids show zone boundaries. Closer views add lines in meters or MGRS subdivisions. Target spacing adapts to rounded intervals and bounded drawing work.

Grid types and the active choice survive restart. The catalog holds at most 32 types. Existing longitude/latitude appearance values migrate from older map profiles. Marker icon scaling applies to existing local markers, CoT symbols, and data points. Marker radius applies to existing local markers. Active previews also repaint after these changes.

To use serverless communication, add a server with the Local LAN protocol. Select its IPv4 interface. Enable its Active checkbox. Tools > Plugins > Local LAN shows discovery status and peer connections. The Interface field also selects a local address for TCP, TLS, and UDP entries.

# Heading and editing controls

Identity shows Role as a description with its code, like Type, Location method, and Team. Use its selector to change the value. A locked field shows the Unlock icon. An unlocked field shows the Lock icon. The map navigation button follows the same rule.

Circle includes the same Restricted flag as Area. Select this flag to add diagonal lines to the fill. The Information pane shows the saved flag.

While you edit an Area, double-click a corner to start Move. Click its new location to place it. Press Escape to cancel that move. The corner menu places Move before Delete.

Grid lines use antialiasing to soften their edges. New grids use 30 percent opacity. Existing grids keep their saved opacity. Change the opacity under Grids in Settings.

The zoom legend appears at the right edge of the map. Click a tick to select that zoom level. The map keeps its center and respects the selected provider limits. The navigation lock also blocks legend clicks. View > Zoom Level controls its visibility.

The Information pane shows ground speed for received CoT objects. The calculation uses the distance and time between consecutive messages. It shows kilometers per hour or miles per hour from the operating system locale. Reported speed provides a fallback when no valid calculation exists. Unknown values remain marked Unknown.

A faint cone shows reported heading, or an estimated heading from recent movement. CoT animation in Settings controls its visibility, angle, length, and opacity. The cone follows the marker during movement, teleport fades, and stale fades. Space freezes the cone and motion values with the map. The angle is a visual aid, not a measured sensor field of view.

The Map providers, Servers, and Data providers menus on the main toolbar end with the More ... action. This entry opens the corresponding management window. A separator places it below the provider choices. The Tools menu keeps the full management names.


# Clear commands and object cones

Events, CoT Messages, Predict, and Trace provide Clear ... under Tools > Plugins. Each command opens a confirmation dialog. Enter confirms the action. Esc cancels it. Clearing Predict or Trace removes every enabled object flag as well as the drawn paths. New messages do not enable those flags again.

View > Reset Layout ... appears last, after a separator. Its confirmation dialog protects the current arrangement from an accidental reset. Map Providers uses No map to deselect the current provider.

Each CoT object starts with its FOV cone off. FOV means field of view. Select FOV cone in the object context menu or Information pane to enable it. The application saves this choice by object identity. File > Settings > FOV controls the shared cone appearance:

- Show FOV cones turns all cones on or off. The default is on.
- Cone angle sets the opening from 5 through 180 degrees. The default is 60.
- Cone length sets the radius from 10 through 200 logical pixels. The default is 60. Marker scaling also applies.
- Fill opacity sets the cone fill from 0.01 through 1. The default is 0.15.
- Outline width draws the cone edges from 0 through 5 pixels. The default 0 draws no outline.
- Outline opacity sets the edge opacity from 0.05 through 1. The default is 0.6.

Earlier profiles kept these values under CoT animation. The FOV page reads them once and saves them in its own section.

Circles and Lines have separate General, Panes, and Keybindings entries in Settings. Lines also contains the temporary Distance configuration. Older combined configuration initializes both sections. Later changes remain independent. The Identity command remains under File and no longer appears in Settings.

Help > Credits lists library and icon versions, licenses, attributions, and project URLs. Qt and libarchive report their runtime versions. GeographicLib reports its build version. The table also includes compression libraries reported by libarchive and the active Qt TLS backend. System graphics and desktop libraries depend on the Linux installation.


# Objects and routes

Tools > Plugins > Objects lists current CoT objects by title and type. The list includes objects outside the viewport. Repeated messages update the same UID row. Expired or cleared messages disappear.

Double-click a row to center the map without selecting the object. Right-click a row to use Information, Trace, Predict, and FOV cone actions. Table column arrangement and sorting survive restart.

Tools > Plugins > Route planning opens the routing preview. Read [Route planning](routing.md) for provider management, map picking, transport modes, and current limits.
