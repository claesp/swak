# Use the workspace

A fresh profile starts with an empty map and no receiver socket. Later launches restore the saved interface and receiver state. The event editor opens beside the map in the default layout. The View menu restores hidden docks, which are movable tool panels.

Events, Event editor, UDP receiver, and Plugins each have an independent dock. The three tool docks start as tabs beside the map. Drag a dock title or tab to detach that pane.

Each detached pane has a resize handle in its lower-right corner. Close a pane to hide it. Use File > Quit to close the application.

To create a local event, use this sequence:

1. Enter an identity, callsign, event type, and position in the Event editor.
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
