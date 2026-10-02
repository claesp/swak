# Plugin architecture

The host creates the window and connects built-in plugins to the event store. The store enforces event limits and exposes a Qt table model. A model supplies rows to views without copying a widget for each row.

The plugin interfaces live in `src/core/interfaces.h`. The common plugin API is version 2. Most UI interfaces use `/2.0`. ToolbarPlugin uses `/3.0` for its required name. Unchanged data interfaces retain `/1.0`. Read [Plugin API](plugin-api.md) for the reviewed contracts and manifest schema. Every plugin in this version uses the same compiler, Qt installation, and build configuration.

The extension levels are:

- `Codec` converts bounded messages to events and events to XML.
- `SourcePlugin` creates a receiver panel that submits decoded events to the store.
- `ViewPlugin` creates a presentation widget and receives the available map layers.
- `MapLayer` draws one part of the map in a defined order.
- `ToolPlugin` creates a workspace panel for event interaction.
- `PanePlugin` creates a pane without a codec.
- `MenuPlugin` adds commands under Tools > Plugins.
- `ToolbarPlugin` adds icon actions to the main toolbar.

The CoT plugin implements `Codec`. The UDP plugin implements `SourcePlugin`. The OpenGL map plugin implements `ViewPlugin`.

The basemap plugin draws the plain green background at order zero. The tracks plugin draws events at order 100 and also provides the event table. The editor plugin provides generation, file import, XML preview, and export.

The map uses `QOpenGLWidget` and the Qt OpenGL paint engine. Each layer receives a saved painter state and logical screen coordinates. Qt handles the conversion to physical pixels on scaled displays.

All application state lives on the GUI thread, which processes interface events. The receiver polls its nonblocking socket every 25 milliseconds while active. Each poll handles at most 32 datagrams and starts no further datagram after four milliseconds.

Parsing remains on the GUI thread because each message contains at most 16 KiB. The application batches screen updates through the Qt event loop. Labels stop after 100 visible events, except for the selected event.

# Add a plugin

Plugins use the native Qt plugin mechanism with static linkage. Static linkage places plugin code inside the executable. The host does not search folders for executable plugins.

To add a built-in feature, use this sequence:

1. Implement a core interface in a `QObject` subclass under `src/plugins`.
2. Add `Q_OBJECT`, `Q_PLUGIN_METADATA`, and `Q_INTERFACES` declarations with a MANIFEST file and the common `Plugin` interface.
3. Add a target with `swak_plugin` in `CMakeLists.txt`.
4. Link the target to the application and relevant tests.
5. Add `Q_IMPORT_PLUGIN` to `src/app/plugins.cpp`.
6. Add behavior tests and document input limits under `docs/`.

A plugin can implement more than one interface. The tracks plugin demonstrates this arrangement. In this host, a tool that also implements `MapLayer` appears in the bottom dock.

Each tool and source has an independent dock with a Tools > Plugins activation action. The host groups the side docks as tabs initially. All docks share a content container and a resize handle that appears when detached.

Create widgets with the supplied parent. Keep widgets and painter calls on the GUI thread. Keep each map layer free of retained painter or store references beyond the call.

The host keeps plugin instances alive for the process lifetime. It destroys panels and the map before destroying the event store. Source panels must close their sockets and timers when Qt destroys them.

Use a separate process for untrusted extensions. Native plugins have full application permissions and cannot provide an isolation boundary. Runtime plugin discovery requires a separate design for trust, compatibility, and process isolation.

# Preferences contract

`PreferenceParticipant` is an optional interface for plugin widgets in `src/core/preferences.h`. It supplies a stable identifier and methods to save and restore a JSON object. The host finds participants within its widget tree.

Keep each identifier stable across plugin releases. The host derives persistent pane names from those identifiers. Do not use translated titles, installation paths, or plugin discovery order as identifiers.

The document has root schema version 1. It contains `window`, `panes`, and `components` objects. Unknown root fields and absent plugin sections remain intact when the host saves available features.

Each plugin owns its component keys and must bound restored values. Keep compatible keys stable when adding fields. An incompatible document format requires a new schema version and an explicit migration.

The host uses versioned Qt layout data for dock relationships and separate records for individual panes. Qt retains placeholders for absent panes across saves. If the Qt layout cannot be restored, the host restores available pane records instead.

A missing map plugin produces an empty-state message. A missing codec disables the tools that require it. Both cases preserve the absent component preferences for a later launch.

Preferences use `QStandardPaths`, `QJsonDocument`, `QLockFile`, and `QSaveFile`. No additional library is required. Read [Use the workspace](usage.md) for profile portability and recovery behavior.

# References

The CoT fields follow the base format described in the [MITRE CoT router guide](https://nps.edu/documents/104517539/109705106/COT%2BUser%2BGuide.pdf/4e67c16d-171c-4f8c-9649-7b3870dbcb4c). This application implements its own bounded subset rather than full schema validation. Contact callsigns are the only interpreted detail extension.

Qt documents the graphics lifecycle in [QOpenGLWidget](https://doc.qt.io/qt-6/qopenglwidget.html). Qt documents streaming XML parsing in [QXmlStreamReader](https://doc.qt.io/qt-6/qxmlstreamreader.html). Use documentation for the target Qt version before adding newer APIs.

# Settings sections

`SettingsParticipant` extends `PreferenceParticipant` and has interface identifier `org.swak.Settings/1.0`. It lives in `src/core/settings.h` and is available through `src/core/interfaces.h`. A plugin can implement it on its root object or on a widget that it creates. The host discovers both forms without plugin-specific dialog code.

Each provider supplies a stable identifier, a translated section title, field descriptions, and optional validation. Fields support booleans, bounded numbers, bounded text, and choices. Slash-separated field paths address nested JSON objects and array indices. Field paths and limits come from trusted plugin code, not imported profiles. All identifiers must remain unique across providers.

The dialog reads a snapshot through `savePreferences`. It validates all changed sections before it calls `restorePreferences` on any provider. Unchanged controls do not replace more precise stored values. Apply updates live components and saves the shared profile. Plugins must keep restoration bounded, handle runtime failures, and report operational errors in their interface. The receiver reports bind failures in its pane and retains the saved startup request.

Plugin code must not throw through validation or restoration callbacks. Providers must remain alive for the dialog lifetime. The host keeps static plugin objects alive and owns their widgets. Unknown preference fields and sections for absent providers survive normal saves.

# Logging contract

The executable installs its Qt message handler before constructing `QApplication`. The handler starts silent. A bounded read of the selected profile enables explicitly saved logging before the graphics probe. The window then applies its loaded configuration. Invalid profiles cannot enable logging.

Application and plugin code uses `logMessage` from `src/core/logging.h`. Every call supplies severity, urgency, a stable category, and bounded diagnostic text. Do not log event payloads, credentials, or editor drafts. Use error labels for user-facing failures, regardless of logging configuration. Do not use direct console writes or replace the Qt handler in a plugin.

The logger serializes concurrent calls and limits message size and rate. A separate nonblocking descriptor handles pipes and terminals without changing the original descriptor flags. Socket output uses nonblocking sends. Closed pipes do not deliver a fatal signal to the application. The logger drops unavailable output and never retries inside the interface thread.

This design adds no external dependency. The output adapter uses Linux descriptors and signal masks. It does not intercept arbitrary output from native libraries. Qt documents the supported handler in [Qt logging](https://doc.qt.io/qt-6.8/qtlogging.html).

The Settings dialog separates General sections from pane customization. The host marks pane pages explicitly and keeps stable identifiers separate from translated titles. Section names and choice labels use `QCollator` ordering. Choice values remain attached to their labels after sorting. The default collator follows the system collation locale, including `LC_COLLATE` on Linux. Qt documents this behavior in [QCollator](https://doc.qt.io/qt-6/qcollator.html).

# Plugin menus

`MenuPlugin` is an optional interface with identifier `org.swak.Menu/2.0`. A plugin implements `name()` and `populateMenu(PluginServices &, QMenu &)`. The host creates a submenu under Tools > Plugins and calls the provider once for each window. Plugins do not need a panel or codec to provide commands. The host removes empty submenus and adds pane activation actions. The built-in Plugins pane remains available.

Create actions with the supplied menu as their parent. Connect callbacks with a QObject context that owns the state they use. Do not retain window menus or store references in static plugin objects. Keep callbacks bounded and do not let exceptions escape them. Qt removes connections when the action or its context is destroyed.

The CoT Events plugin adds Clear and connects it directly to the window event store. The host contains no event-specific menu code. This interface adds no external dependency and does not load external plugin files.

# Toolbar and About

`ToolbarPlugin` is an optional interface with identifier `org.swak.Toolbar/3.0`. The host labels each section with `name()` and separates adjacent sections. It calls `populateToolbar(PluginServices &, QToolBar &, QMenu *)` once for each window. It supplies the main toolbar and the same plugin menu, or null when the plugin has no menu interface. The host calls the menu provider first. A toolbar provider can reuse its menu actions or create actions owned by the toolbar.

Use translated action text, a tooltip, and an icon with a fallback. Connect callbacks with a QObject context that owns their state. Do not retain toolbar or menu pointers in static plugin objects. The CoT Events plugin shares its Clear action across both surfaces. Shared actions keep their text, enabled state, and behavior consistent.

The main toolbar stays at the top and has stable identifier `mainToolbar`. The View menu controls its visibility. The saved window layout retains visibility across restarts and missing plugins. Reset layout restores the default toolbar state.

Help > About opens one dialog per window and releases it on close. The dialog uses plain text and contains the application version and general information. CMake supplies `SWAK_VERSION` from `PROJECT_VERSION` to both About and the command-line version. Future general information belongs in this dialog.

# Map registration

`MapExtension` registers a stable map identifier, a name, and a kind: Provider or Tool. A provider also implements `MapProvider`, which supplies defaults, editable fields, validation, and a tile URL builder. The XYZ, WMTS, and WMS plugins register both interfaces. No map tool exists yet. The green background remains an internal map layer.

`MapSources` owns the per-window catalog and implements `SettingsParticipant`. The host discovers participants among its QObject children as well as plugin roots. `PluginServices` supplies the catalog to the view and toolbar callbacks. Only registered providers enter the manager format list. Missing providers retain their saved configuration.

`TileRenderer` handles bounded HTTPS retrieval, caching, and image decoding. It cancels requests when the map hides or changes source. The map widget draws image tiles after the background layer and before event layers. A source change clears decoded images and connection state. Read [Map providers](map-providers.md) for format contracts and limits.

# Data overlays

DataSources owns the per-window feed catalog, refresh timers, HTTPS clients, and decoded geometry. DataProvider plugins decode bounded responses outside the interface thread. The map draws their normalized geometry through its existing OpenGL paint engine, below CoT events.

DataManager uses the shared SettingsDialog for Add and Edit forms. The host embeds the same manager table in File > Settings. Add and Edit use the provider fields. Missing plugin configuration survives restarts. Read [Data providers](data-providers.md) and [Plugin API](plugin-api.md) for contracts and limits.

The MVT plugin supplies OpenFreeMap through the existing MapProvider catalog. Its VectorTileProvider capability decodes bounded vector tiles on two host workers. Workers prepare complete tile images, and the OpenGL map composes those cached images. The host limits combined downloads and decode work to four jobs and discards cancelled results. The same catalog controls attribution, HTTPS, cache, appearance, and saved layer visibility.

ProviderTable supplies shared row selection and activation controls for both catalogs. Map providers use an exclusive radio group, while data providers use independent checkboxes. SettingsPage can embed these manager widgets with a host-owned factory. Catalog actions save immediately and do not share the generic form snapshot. Embedded managers do not overwrite the saved geometry of separate management windows.
