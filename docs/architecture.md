# Plugin architecture

The host creates the window and connects built-in plugins to the event store. The store enforces event limits and exposes a Qt table model. A model supplies rows to views without copying a widget for each row.

The plugin interfaces live in `src/core/interfaces.h`. Interface identifiers end in `/1.0` to distinguish incompatible revisions. Every plugin in this version uses the same compiler, Qt installation, and build configuration.

The extension levels are:

- `Codec` converts bounded messages to events and events to XML.
- `SourcePlugin` creates a receiver panel that submits decoded events to the store.
- `ViewPlugin` creates a presentation widget and receives the available map layers.
- `MapLayer` draws one part of the map in a defined order.
- `ToolPlugin` creates a workspace panel for event interaction.

The CoT plugin implements `Codec`. The UDP plugin implements `SourcePlugin`. The OpenGL map plugin implements `ViewPlugin`.

The basemap plugin draws the offline reference shapes at order zero. The tracks plugin draws events at order 100 and also provides the event table. The editor plugin provides generation, file import, XML preview, and export.

The map uses `QOpenGLWidget` and the Qt OpenGL paint engine. Each layer receives a saved painter state and logical screen coordinates. Qt handles the conversion to physical pixels on scaled displays.

All application state lives on the GUI thread, which processes interface events. The receiver polls its nonblocking socket every 25 milliseconds while active. Each poll handles at most 32 datagrams and starts no further datagram after four milliseconds.

Parsing remains on the GUI thread because each message contains at most 16 KiB. The application batches screen updates through the Qt event loop. Labels stop after 100 visible events, except for the selected event.

# Add a plugin

Plugins use the native Qt plugin mechanism with static linkage. Static linkage places plugin code inside the executable. The host does not search folders for executable plugins.

To add a built-in feature, use this sequence:

1. Implement a core interface in a `QObject` subclass under `src/plugins`.
2. Add `Q_OBJECT`, `Q_PLUGIN_METADATA`, and `Q_INTERFACES` declarations.
3. Add a target with `swak_plugin` in `CMakeLists.txt`.
4. Link the target to the application and relevant tests.
5. Add `Q_IMPORT_PLUGIN` to `src/app/plugins.cpp`.
6. Add behavior tests and document input limits under `docs/`.

A plugin can implement more than one interface. The tracks plugin demonstrates this arrangement. In this host, a tool that also implements `MapLayer` appears in the bottom dock.

Create widgets with the supplied parent. Keep widgets and painter calls on the GUI thread. Keep each map layer free of retained painter or store references beyond the call.

The host keeps plugin instances alive for the process lifetime. It destroys panels and the map before destroying the event store. Source panels must close their sockets and timers when Qt destroys them.

Use a separate process for untrusted extensions. Native plugins have full application permissions and cannot provide an isolation boundary. Runtime plugin discovery requires a separate design for trust, compatibility, and process isolation.

# References

The CoT fields follow the base format described in the [MITRE CoT router guide](https://nps.edu/documents/104517539/109705106/COT%2BUser%2BGuide.pdf/4e67c16d-171c-4f8c-9649-7b3870dbcb4c). This application implements its own bounded subset rather than full schema validation. Contact callsigns are the only interpreted detail extension.

Qt documents the graphics lifecycle in [QOpenGLWidget](https://doc.qt.io/qt-6/qopenglwidget.html). Qt documents streaming XML parsing in [QXmlStreamReader](https://doc.qt.io/qt-6/qxmlstreamreader.html). Use documentation for the target Qt version before adding newer APIs.
