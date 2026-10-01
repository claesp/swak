# Use the workspace

The application starts with an empty map and no receiver socket. The event editor opens beside the map. The View menu restores hidden docks, which are movable tool panels.

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

1. Open the UDP receiver tab.
2. Enter a numeric local address and port.
3. Select Start receiver.

The default address, `127.0.0.1`, accepts traffic only from the local computer. Bind a specific network interface only on a protected network. Binding `0.0.0.0` exposes the receiver on every IPv4 interface.

Select Stop receiver to close the socket. Closing the window also closes the socket. The panel shows accepted and rejected message counts and the latest rejection reason.

Select Import XML to read one event from a regular file. Select Export XML to save the current preview. Canceling either file dialog leaves the current data unchanged.

The preview contains supported fields only. Unknown detail fields are discarded during import and are absent from exported XML. The application retains no original message archive.

Expired events disappear within approximately one second. Events with a future start time appear on the map when that time arrives. The table includes these pending events before their start time.

The application stores up to 4096 identities in memory. Closing the application discards those events. Export any required events before closing the window.
