# Plugin API

This review covers all thirteen built-in plugins. Every plugin implements `Plugin` and uses the shared contracts in `src/core/`. The common API version is 2. No plugin depends on a concrete application window class.

The application links plugins into the executable. Qt embeds each `src/plugins/<plugin>/MANIFEST` file through `Q_PLUGIN_METADATA`. The common metadata identifier is `org.swak.Plugin/2.0`. The application does not load external plugin binaries or search plugin directories.

# Host services

`PluginServices` supplies references to the event store, internal-event model, and map catalog. Its optional `data` pointer supplies the data catalog. Its optional `selection` pointer supplies the shared map selection. The host passes this context to each UI factory and menu or toolbar callback. The context exists only during the call. Plugins can capture its service pointers in callbacks whose QObject context limits their lifetime.

The host owns the services for each window. Plugins must not save service pointers, menus, or widgets in their static plugin objects. Qt parents own created widgets and actions. The host destroys plugin widgets before the event store. All model and widget operations use the GUI thread.

Use `services.store` for CoT data and selection. Use `services.maps` for provider discovery, selection, and catalog changes. Emit `MapSources::managementRequested` to request the provider manager. Use `services.activity` for internal-event history. If `services.data` is non-null, use it for data feeds and geometry. No plugin searches the host widget tree to find these services.

# Feature interfaces

Plugins implement the feature interfaces that they need. Their manifests declare the same roles. The host compares declarations against the interfaces that Qt exposes.

| Interface | Identifier suffix | Contract |
| --- | --- | --- |
| `Plugin` | `/2.0` | Returns the common API version. |
| `Codec` | `/1.0` | Decodes bounded bytes and encodes CoT events. |
| `MapLayer` | `/1.0` | Supplies drawing order and paints a map layer. |
| `ViewPlugin` | `/2.0` | Creates the main view from services, layers, and a parent widget. |
| `ToolPlugin` | `/2.0` | Creates an event tool from services, a codec, and a parent widget. |
| `SourcePlugin` | `/2.0` | Creates a receiver from services, a codec, and a parent widget. |
| `PanePlugin` | `/2.0` | Creates a pane from services and a parent widget, without a codec. |
| `MenuPlugin` | `/2.0` | Adds actions through `populateMenu(PluginServices &, QMenu &)`. |
| `ToolbarPlugin` | `/3.0` | Supplies `name()` and adds controls through `populateToolbar(PluginServices &, QToolBar &, QMenu *)`. |
| `MapExtension` | `/1.0` | Supplies a stable map identifier, name, and Provider or Tool kind. |
| `DataProvider` | `/1.0` | Supplies `dataId()`, `dataName()`, disabled `presets()`, and bounded `decode(bytes, cancelled)`. |
| `VectorTileProvider` | `/1.0` | Supplies bounded vector decoding and known layer names. |
| `MapProvider` | `/1.0` | Supplies source defaults, fields, validation, and tile URLs. |
| `PreferenceParticipant` | C++ interface, no Qt identifier | Saves and restores bounded JSON under a stable component identifier. |
| `SettingsParticipant` | `/1.0` | Adds a title, editable fields, and optional validation to preferences. |

The unchanged data and map interfaces retain version 1.0. Most UI interfaces use version 2.0 because their service arguments changed. ToolbarPlugin uses version 3.0 because it now requires `name()`. The former `MapConsumer` interface is removed. The view now receives the map catalog through the shared context.

The host adds a plain-text label from `ToolbarPlugin::name()` before each toolbar section. Separators divide these sections. The host calls menu callbacks before toolbar callbacks. A toolbar callback receives its plugin menu or null. Plugins can share one action between their menu and toolbar. Plugins must not assume that a menu exists.

Tools and sources require a codec for their panes. The host leaves those panes unavailable when no codec exists. Codec-free panes, menus, and independent map layers remain available. Pane visibility does not unload plugin code or stop a receiver.

The host owns pane placement, visibility actions, and layout persistence. Plugins with commands receive a submenu under Tools > Plugins. The plugin entry has a checkbox that shows or hides its pane. Return and Space also toggle the pane. Hover and Right open the command submenu. Plugins without commands use a direct checkbox action. Closing a pane hides it.

# Implemented plugins

| Manifest directory | Name | Roles |
| --- | --- | --- |
| `src/plugins/kml` | KML and KMZ data | Data provider |
| `src/plugins/cot` | Cursor-on-Target XML | Codec |
| `src/plugins/basemap` | Map background | Map layer |
| `src/plugins/map` | OpenGL map | View and toolbar |
| `src/plugins/tracks` | CoT Events | Map layer, tool, menu, and toolbar |
| `src/plugins/editor` | CoT Event Generator | Tool |
| `src/plugins/udp` | UDP receiver | Source |
| `src/plugins/activity` | Internal events | Pane and menu |
| `src/plugins/information` | Information | Pane |
| `src/plugins/mvt` | MVT vector tiles | Map provider and vector tiles |
| `src/plugins/xyz` | XYZ image tiles | Map provider |
| `src/plugins/wmts` | WMTS image tiles | Map provider |
| `src/plugins/wms` | WMS images | Map provider |

The map plugin creates the provider selector through `ToolbarPlugin`. The selector uses `MapSources` and requests the manager through its public signal. The internal-event plugin creates its table and Clear action through `PanePlugin` and `MenuPlugin`. Both use explicit services instead of host widget searches.

The editor, receiver, and event table report operations through `logMessage`. The map renderer and host use the same logger. Plugin code does not write directly to the console.

# Manifest schema

Each MANIFEST contains a JSON object. Its schema version is 1. Qt embeds the object as `MetaData`, so no separate manifest file is needed beside the executable.

| Field | Required value or meaning |
| --- | --- |
| `schemaVersion` | Integer 1. |
| `id` | Unique stable identifier under `org.swak`. Map identifiers match `mapId()`. Data identifiers match `dataId()`. |
| `name` | Nonempty display name, at most 512 characters. |
| `description` | Nonempty description, at most 512 characters. |
| `version` | Plugin version with three numeric parts. |
| `apiVersion` | Integer 2. |
| `entryPoint` | Unqualified Qt class name of the plugin object. |
| `linkage` | `static`. |
| `platforms` | Array that includes `linux`. |
| `qtMajor` | Qt major version used by the host. |
| `qtMinimum` | Minimum runtime Qt version with three numeric parts. |
| `interfaces` | Exact list of implemented feature roles, without duplicates. |
| `requires` | `["codec"]` for tools and sources, otherwise `[]`. |
| `permissions` | Declared operating-system access, without duplicate or unknown values. |
| `settingsSchemaVersion` | Integer 1 for the preferences contract. |

Permission values are `network-udp`, `network-https`, `disk-cache`, `browser-links`, `clipboard-write`, and `user-selected-files`. They document access and do not create a security sandbox. Native plugin code retains application permissions. Static plugins use the same compiler, Qt installation, and build flags as the host.

`builtInPlugins()` validates manifests before the host uses their features. It rejects incompatible declarations and duplicate identifiers. It reports failures through the shared logger. Unknown metadata identifiers do not enter the SWAK plugin catalog.

# Internal events

Use `logMessage(severity, urgency, category, message)` from any thread. Internal categories start with `swak.`. Supply a short operational description without payloads, credentials, or editor drafts. Severity values are Debug, Info, Warning, Error, and Fatal. Urgency values are Routine, Attention, and Immediate.

Internal history remains independent of text output. Text logging stays disabled until the user enables it. Qt diagnostics follow the text-output configuration and do not enter internal history.

A mutex protects a 256-record inbox. Each category contains at most 128 characters, and each message contains at most 2048 characters. The model collects records every 100 milliseconds. Excess bursts discard the oldest inbox records. This bound prevents an event queue from growing during overload.

The model retains 500 events by default. File > Settings > Internal events allows 1 through 10000 events. Lower limits remove the oldest rows immediately. History stays in memory and does not survive process restarts. The limit, pane layout, table widths, order, and sort selection survive restarts.

The status bar shows CoT and retained internal-event counts with separators. Incoming events do not open the history pane. Errors and fatal events use red text in the history table. Clearing the table also discards pending events for that model. It does not change text-output configuration.

# Add a plugin

Create a QObject plugin class and its MANIFEST under `src/plugins`. Implement `Plugin` and the required feature interfaces. Add the metadata file to `Q_PLUGIN_METADATA` and list all interfaces in `Q_INTERFACES`.

Register the static target with `swak_plugin` in `CMakeLists.txt`. Link it to the executable and relevant test targets. Add `Q_IMPORT_PLUGIN` in `src/app/plugins.cpp`. Add tests for malformed input, missing services, cancellation, and shutdown as applicable.

Keep callbacks bounded and do not let exceptions escape Qt calls. Use shared preference and field helpers for saved values. Update the manifest and this inventory when roles change. Run the applicable diagnostics and rebuild the release executable before completing the task.


# Recovery and table order

The map catalog preserves invalid provider entries and disables their network use. A provider must reject invalid input before it creates a request. Catalog edits validate changed entries without discarding other saved providers.

`Store::SortRole` supplies numeric coordinates and timestamps for table order. `Qt::DisplayRole` retains localized text. `ActivityModel::SortRole` supplies timestamps and numeric severity and urgency values. Table plugins must use these roles when they sort those models.

Provider forms use the host persistence callback. They keep the form open when a save fails. The public plugin interface versions remain unchanged.

# Data provider contract

`PluginServices::data` supplies an optional per-window `DataSources` pointer. It defaults to null for callers without data support. The view subscribes to `contentChanged` and draws data through the catalog. The catalog also implements the shared preference and configuration interfaces.

DataProvider decoders run on at most two background threads. Keep them reentrant and free of QObject or GUI operations. Read the atomic cancellation flag during bounded work. Return complete normalized geometry or an error in `DataDocument`. Keep the plugin instance alive until all windows finish their workers.

The host owns downloads, per-feed timers, configuration, and rendering. Data plugins supply disabled presets and decode responses. Shared data fields provide endpoint, attribution, timer, and appearance controls. Add forms select one installed format when several data plugins exist. Unknown formats remain in the saved catalog.

Each data plugin declares `data-provider` in its MANIFEST. Its `dataId()` must match the manifest identifier. This version links the KML plugin statically and adds no external plugin discovery. Read [Data providers](data-providers.md) for parser limits and dependency details.

# Vector tile capability

`VectorTileProvider` adds the optional interface `org.swak.VectorTiles/1.0` to a MapProvider. Its manifest declares both `map-provider` and `vector-tiles`. The host rejects the vector role without the map provider role. The common Plugin API remains version 2, and MapProvider remains version 1. All plugins still use the same toolchain and Qt installation.

`decodeTile(bytes, cancelled)` runs on a host worker and returns an owning VectorTile value. The provider must support concurrent calls and cooperative cancellation. It must bound input, output, and work. It must not retain borrowed input or access widgets. An error produces a nonempty error string. A valid empty tile has no features and no error. The host converts standard exceptions into a controlled tile failure.

VectorFeature stores a layer name, an exact identifier string, properties, a geometry kind, and tile-local coordinates normalized by the tile extent. The provider also supplies each feature color and a label-only flag. Polygon paths use odd-even filling for holes. The renderer clips buffered geometry to its tile. The cache adds the prepared image size to the provider's conservative `costKiB` estimate. The static provider must outlive its host workers.

`layerNames()` supplies known layer names before tiles arrive. The host adds names from decoded tiles. The MVT provider uses `visibleLayers` Boolean entries, with absent names visible by default. The shared settings form exposes OpenMapTiles layers and appearance controls. The map context menu changes the same persisted entries. Map information reads loaded feature properties without another network request.

The host prepares vector images on the same bounded workers that decode tiles. The OpenGL view composes the cached images. Decoded features remain available for layer controls and map information. Source and antialiasing changes discard prepared images. Cancellation also covers painting between features. The plugin interface versions remain unchanged.

Each worker owns its QImage until painting finishes. Only completed images cross to the GUI thread. This follows the [Qt rules for painting in threads](https://doc.qt.io/qt-6/threads-modules.html#painting-in-threads).

The host SettingsPage structure accepts an optional widget factory for catalog pages. MapManager and DataManager share ProviderTable in Settings and their separate windows. Catalog actions apply immediately through the existing MapSources and DataSources methods. The host skips generic snapshot forms for these two catalogs, so Apply cannot restore stale provider values. Plugin interfaces remain unchanged.

# Map selection and pane requests

`MapSelection` stores an optional owning `MapItem` value and emits `changed` when that value changes. The host owns this service per window. `MapItem` contains a label, type, plain-text description, JSON properties, normalized world geometry, and drawing dimensions. Selection uses the GUI thread. Consumers must tolerate a null service pointer.

`DataSources::itemAt` and `TileRenderer::itemAt` return a snapshot for the topmost hit. The map view passes that snapshot to the shared selection. An empty hit clears selection. Data documents retain bounded description and property values in each `DataShape`. Vector selection retains the feature properties and selects the closest point in a multipoint feature.

A pane can derive from `PaneContent` and emit `visibilityRequested(bool)`. The host connects this signal to the dock. A show request raises the pane without taking keyboard focus. A hide request closes the pane view.

The pane can override preferredDockArea() to select its initial dock area. The default is the bottom area. Information uses the right area. Saved layouts take precedence. This optional base class leaves existing PanePlugin factories unchanged.

The Information plugin implements PanePlugin and PreferenceParticipant. Its MANIFEST declares `clipboard-write`, a permission for user-requested copies to the OS clipboard. The common Plugin API remains version 2. All built-in plugins compile against the same extended service structure. No external binary compatibility is promised.
