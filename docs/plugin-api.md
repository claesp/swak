# Plugin API

This review covers all fourteen built-in plugins. Every plugin implements `Plugin` and uses the shared contracts in `src/core/`. The common API version is 2. No plugin depends on a concrete application window class.

The application links plugins into the executable. Qt embeds each `src/plugins/<plugin>/MANIFEST` file through `Q_PLUGIN_METADATA`. The common metadata identifier is `org.swak.Plugin/2.0`. The application does not load external plugin binaries or search plugin directories.

# Host services

`PluginServices` supplies references to the event store, internal-event model, and map catalog. Its optional `data` pointer supplies the data catalog. Its optional `selection` pointer supplies the shared map selection. The optional `mapTools` and `objects` pointers supply map interaction and local object storage. The host passes this context to each UI factory and menu or toolbar callback. The context exists only during the call. Plugins can capture its service pointers in callbacks whose QObject context limits their lifetime.

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
| `src/plugins/tracks` | CoT Messages | Map layer, tool, menu, and toolbar |
| `src/plugins/editor` | CoT Event Generator | Tool |
| `src/plugins/udp` | UDP receiver | Source |
| `src/plugins/activity` | Events | Pane and menu |
| `src/plugins/information` | Information | Pane |
| `src/plugins/markers` | Markers | Pane, map tool, and Core collection |
| `src/plugins/milstd2525` | MILSTD2525 | Marker collection |
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

# Events

Use `logMessage(severity, urgency, category, message)` from any thread. Internal categories start with `swak.`. Supply a short operational description without payloads, credentials, or editor drafts. Severity values are Debug, Info, Warning, Error, and Fatal. Urgency values are Routine, Attention, and Immediate.

Internal history remains independent of text output. Text logging stays disabled until the user enables it. Qt diagnostics follow the text-output configuration and do not enter internal history.

A mutex protects a 256-record inbox. Each category contains at most 128 characters, and each message contains at most 2048 characters. The model collects records every 100 milliseconds. Excess bursts discard the oldest inbox records. This bound prevents an event queue from growing during overload.

The model retains 500 events by default. File > Settings > Events allows 1 through 10000 events. Lower limits remove the oldest rows immediately. History stays in memory and does not survive process restarts. The limit, pane layout, table widths, order, and sort selection survive restarts.

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

# Vector recovery and coverage

VectorTile now has a resourceLimit flag. A decoder sets this flag with an error when a valid request exceeds a resource boundary. The host can request a lower tile level for this case. Other decode errors retain the normal retry policy. The static plugin interface identifier remains unchanged.

TileRenderer::coverage() returns the visible tile count and the count with prepared images. The map status reports incomplete coverage. Under memory pressure, the renderer can retain pixels without geometry. Feature inspection then returns no hits for those tiles. Image coverage remains distinct from feature availability and data freshness.

# Keybinding fields

`SettingField::Kind::Key` and `keySetting(path, label)` add keyboard controls through the existing `SettingsParticipant` interface. The host places these fields in the Keybindings tab. It sorts plugin sections with the system locale. General and keyboard fields retain one shared snapshot and one validation callback per participant. Apply validates every changed participant before it changes any component.

The editor uses `QKeySequenceEdit`. Values use `QKeySequence::PortableText` in the existing component object. Plugins define allowed keys, conflicts, defaults, focus scope, and empty-value behavior. Plugins must bound imported key strings and reject unsupported sequences. The field description does not install a shortcut. Existing profile keys and missing-plugin sections remain intact.

The map owns its navigation keys. Information owns its dismissal key and limits event handling to widgets owned by its workspace. Information ignores modal dialogs and popup menus. Closing Information clears both shared selection services. No dependency or plugin role changes.

# View status widgets

A view widget can implement `StatusProvider` from `core/interfaces.h`. The host calls `createStatusWidget(parent)` once and adds the result after the event counts, with a separator. The status bar owns the returned widget. A null result adds no section. A missing view plugin adds no section.

The view updates its status widget on the interface thread. It must use a guarded pointer if the status widget can be destroyed first. The map uses `QPointer` and publishes cache size, zoom, pointer coordinates, tile diagnostics, and the certificate warning. The map returns a group with separate labels and QFrame separators. Empty diagnostics and warnings hide their sections, including separators. The host contains no map-specific status code.

`SingleLineLabel` renders plain text on one line. It uses an ellipsis when space is limited and retains the full text in its tooltip. This shared widget serves status sections and Settings notices. Field labels retain their full width inside scrollable forms.

# Editable map objects and tools

`PluginServices` supplies per-window `UserObjects` and `MapTools` pointers. The host owns both services. Static plugins use these shared interfaces rather than searching for host widgets. The marker plugin registers `MapExtension::Kind::Tool` and `PanePlugin`. Its MANIFEST declares `map-tool` and `pane`. It does not enter the map provider catalog.

`UserObject` separates identity, type, title, description, text properties, geometry, and color. Color uses the seven-character `#RRGGBB` format. Older records without color use orange (`#ffc857`). `UserObjects` owns committed records and portable preferences. Geometry is a bounded JSON object. The circle type uses normalized Mercator coordinates named `x` and `y`. Unknown object types retain their bounded records for future tools. New geometry types can reuse the metadata editor and deletion commands.

`ObjectEditor` works on a copy of a record. Apply validates the whole copy before it writes through `UserObjects::put`. Cancellation drops that copy. `MapSelection` carries an owner identifier and object identifier for editable selections. Imported selections leave these fields empty. The regular Information pane remains read-only. It asks `MapTools::deleteSelection` to handle deletion, without knowing the owning plugin.

A `MapTool` receives map input events, a viewport, and the map widget. Returning true consumes the event. The map paints registered tools after imported layers and before selection outlines. Each tool receives a saved painter state. `changed` requests another frame. `cancel` stops a pending operation. Tools must reject events outside their scope and avoid changing committed state during previews.

The tool registry holds guarded pointers. Pane ownership controls each tool lifetime. The marker tool draws circles through the existing OpenGL painter. Its move preview leaves stored geometry unchanged until the click that commits it. Esc cancels an active tool before it dismisses Information. Hiding the map also cancels pending tool operations.

`UserObjects::undoStack` supplies the application Edit > Undo action. Shared deletion commands retain complete records. Their history is bounded to 100 commands and is not serialized. Deleted records reserve model capacity until restoration or history removal. This reserve keeps a full model from blocking undo. GUI input, drawing, and model updates remain on the interface thread.


`MapTool::paintOverlay` draws transient content above the map. Its pointer argument contains the cursor position sampled at paint time, in logical map coordinates. Tools must use that argument for pointer-following previews. The host uses a transparent, mouse-transparent child widget for this content.

Emit `overlayChanged` for pointer-only changes. The host requests an immediate overlay repaint. Qt can defer presentation to the next composed frame. Do not emit that signal from a paint method.

Emit `changed` when stored content or tool mode changes. The host then updates both surfaces.

Overlay painting does not request map tile painting. The view refreshes the overlay when its projection changes. Overlay methods stay on the interface thread and must keep work bounded.


`MapTool::statusText` returns a short active-mode indicator or empty text. `MapTools::statusText` combines these indicators. The host reserves a separate status area and refreshes it on `MapTools::changed`. Plugins emit `changed` when a mode starts or ends. The marker tool returns M during placement and movement. Pointer-only updates do not refresh this area.


`MapTool::coordinateGridActive` requests the coordinate grid during an active tool mode. `MapTools` combines these requests. The view draws the grid only when a tool requests it and the user enables it. Mode changes emit `changed`, so grid visibility follows activation and cancellation.


# Map overlay panels

The host places all attached panes above the map in `MapPanels`. Panes cover map pixels without changing the map viewport. Plugins keep the same `PanePlugin` and `PaneContent` interfaces. They must use the supplied widget parent. Detached panes remain separate native windows.

`window.overlayLayout` stores pane relationships. `window.layout` stores the outer window toolbar state. The host reads older dock layouts when `overlayLayout` is absent. Individual pane records and unknown plugin sections remain available as before.

The shared object editor shows Collection after Title and Type after Collection. Type lists symbols from the selected collection. The editor adds unique colors from registered collections to its eight general colors. It preserves other valid stored colors. Apply saves the selected color with the object. Deletion undo and movement retain that color.

# Source presets and shared symbols

`MapProvider::additionalSources` optionally returns a JSON object of stable preset identifiers and source records. The default implementation returns an empty object. The host sets each record's `format` to the provider identifier and validates the source. Presets use the same settings, management, networking, and rendering interfaces as user sources. The shared interface remains `/1.0` within the existing same-build static plugin contract.

Preset identifiers use the same bounded syntax as user source identifiers. Plugins must keep identifiers stable and distinct from provider identifiers. `MapSources` adds missing presets to old profiles without replacing existing records. Its `knownPresets` array prevents a deleted preset from returning. The host retains records when a provider disappears and limits the catalog to 32 sources. The remembered preset list stops growing at 256 entries.

`UserObject::symbol` stores an optional identifier of at most 64 letters, digits, periods, or hyphens. An empty identifier means the existing circle. Unsupported identifiers remain in preferences and render as circles. Symbol choice does not change the underlying geometry type. The editor stores the symbol and color together on Apply. Undo, movement, cancellation, and restart retain the complete record.

`markerSymbols`, `markerSymbolForCot`, `markerFrame`, and `paintMarkerSymbol` form the shared vector-symbol API in `core/marker_symbols.h`. The marker tool and CoT Messages plugin use these functions. `MapItem::symbol` carries the frame choice to the selection renderer. No plugin owns a separate icon catalog. The current subset and provenance appear in [the reference review](atak-reference-review.md).

# Marker collection providers

`MarkerCollectionProvider` declares `org.swak.MarkerCollection/1.0`. Its `markerCollection()` method returns one bounded collection definition. The manifest declares the `marker-collection` role alongside any other roles. The host validates this definition before accepting the plugin. Collection identifiers are independent of plugin identifiers and must remain stable and unique.

The Markers plugin supplies Core through this interface. The separate MILSTD2525 plugin supplies the military catalog. Both use the same schema as imported archives. `PluginServices::collections` points to the per-window `MarkerCollections` service. The host owns this service, and all mutations occur on the interface thread. The same-build static plugin contract still applies.

`MarkerCollections` owns portable definitions, enabled flags, and decoded vector paths. The service validates inputs before it publishes symbols. Rendering uses cached paths rather than repeated JSON parsing. `CollectionSymbol` contains an identifier, name, default color, frame, and glyph. `paintCollectionSymbol` is the shared painter entry point. Archive formats and bounds appear in [Marker collections](marker-collections.md).

`UserObject::collection` stores the collection identifier beside `symbol`. Empty legacy references resolve to Core for an empty symbol and MILSTD2525 otherwise. Explicit unknown references remain intact. `MapItem::markerOutline` carries normalized geometry for the selected local symbol. The selection renderer scales that geometry at the current marker radius. Missing symbols use a circle without changing the saved record.

`ObjectEditor` receives the collection service and stores its last collection and type choices in its existing preferences. The collection manager uses the same provider-table control as map and data management. Settings embeds that manager without pane controls. No new external dependency or dynamic-code loader is added.

# Map authentication

Common map fields add `authMode`, `authUser`, `authParameter`, and `authEnvironment`. An absent mode means `none`. Modes `basic`, `bearer`, `header`, and `query` use the same shared request path for every map format. Providers return URLs without secrets. They continue to call `validateMapSource` for common input rules.

`MapSources::setCredential` accepts a bounded session secret for an existing authenticated source. An empty value removes it. `credential` resolves the session value before the restricted environment reference. Secrets never enter `savePreferences`. The map host passes the result to `TileRenderer::setSource` as a separate argument.

`authorizeMapRequest` applies a secret only to HTTPS requests for the configured host and port. It rejects certificate bypass and invalid authentication fields. It sets request attributes that prevent disk storage and automatic authentication reuse. The renderer also removes its disk cache for authenticated sources and blocks redirects. Plugins must not add a second credential path or log authorized URLs.

The generic form selects a choice field's first declared value when a saved value is absent. Locale sorting changes the visible order only. This keeps legacy providers unauthenticated when their editor opens.

# Primary pane commands

PanePlugin can return a pane-owned QAction from `primaryAction(QWidget *)`. The argument is the dock content container, which contains the plugin widget. The host uses that action instead of the pane toggle in Tools > Plugins. The default remains the existing pane toggle.

MapTool can emit `activationRequested`, which MapTools forwards to the map view. The view cancels the current tool, calls `activate(QWidget &)`, and focuses the map. The default activation method does nothing. Markers uses this path for the Marker menu action and shares its placement setup with the keyboard action. The editor pane still opens after placement or an explicit edit.
