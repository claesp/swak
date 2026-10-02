# Use the workspace

A fresh profile starts with an empty map and no receiver socket. Later launches restore the saved interface and receiver state. CoT Event Generator opens beside the map in the default layout. The View menu restores hidden docks, which are movable tool panels.

CoT Events, CoT Event Generator, UDP receiver, and Plugins each have an independent dock. The three tool docks start as tabs beside the map. Drag a dock title or tab to detach that pane.

Each detached pane has a resize handle in its lower-right corner. Close a pane to hide it. Use File > Quit to close the application.

Use Tools > Plugins > CoT Events > Clear to remove all current events and clear the selection. Plugins add their commands under Tools > Plugins.

The top toolbar provides plugin icons. Its Clear button performs the same action as the CoT Events menu command. Point to an icon to read its tooltip. Use View > Main toolbar to hide or show the toolbar. The application saves this choice across restarts.

Use Help > About to read the application version and general information.

To create a local event, use this sequence:

1. Enter an identity, callsign, event type, and position in CoT Event Generator.
2. Set the event lifetime in seconds.
3. Select Generate event.

The identity names one tracked object. Reusing an identity replaces its stored event when the new timestamp is equal or newer. Select New identity before creating another object.

Latitude and longitude use degrees on WGS 84. Altitude is height above the WGS 84 reference surface, in meters. Generated events use `h-e`, which indicates a human estimate.

The editor generates `a-u-G` events by default. Map markers use blue for `a-f` types, red for `a-h` types, and yellow for other types. These circles do not implement a military symbol standard.

Select a table row or map marker to focus that event. The XML preview then shows that event. Selection changes the preview but keeps the editor fields unchanged.

Drag the map with the left mouse button to move it. Use the wheel to zoom around the pointer. With map focus, use arrow keys to move, plus or minus to zoom, and Home to reset.

The map uses Web Mercator, a rectangular map projection. It clips positions near the poles to approximately 85.05 degrees. The event table retains the original coordinates, including polar positions.

# Receive and save events

UDP carries separate network messages called datagrams. Each datagram must contain one UTF-8 CoT XML event. The receiver does not authenticate senders or encrypt messages.

To receive events, use this sequence:

1. Open the UDP receiver pane from the View menu.
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

Use View > Reset layout to return panes to their initial arrangement. This command keeps editor values, map position, and receiver configuration. Each pane remains available through its own View menu entry.

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

Open File > Settings to edit application and plugin configuration. Apply saves changes and keeps the dialog open. OK saves changes and closes the dialog. Cancel discards changes since the last Apply. Invalid input prevents all pending changes from applying. A save error stays visible, and changes remain active for the current session.

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

Open Tools > Map Providers to select or configure an online map. A fresh profile uses the offline reference map. File > Settings contains source and rendering controls. Read [Map providers](map-providers.md) for sources, formats, attribution, and HTTPS behavior.
