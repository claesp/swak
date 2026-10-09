# Plugin API

This review covers all twenty-two built-in plugins. Every plugin implements `Plugin` and uses the shared contracts in `src/core/`. The common API version is 2. No plugin depends on a concrete application window class.

The application links plugins into the executable. Qt embeds each `src/plugins/<plugin>/MANIFEST` file through `Q_PLUGIN_METADATA`. The common metadata identifier is `org.swak.Plugin/2.0`. The application does not load external plugin binaries or search plugin directories.

# Host services

`PluginServices` supplies references to the event store, internal-event model, and map catalog. Its optional `data` pointer supplies the data catalog. Its optional `selection` pointer supplies the shared map selection. The optional `mapTools` and `objects` pointers supply map interaction and local object storage.

The host passes this context to each UI factory and menu or toolbar callback. The context exists only during the call. Plugins can capture its service pointers in callbacks whose QObject context limits their lifetime.

The Window owns every service and destroys objects in a fixed order. First it destroys dock panes and the map view. Then it destroys Servers, the Store, and the other services. Last, it destroys menus, toolbars, and the objects that MenuPlugin and ToolbarPlugin create. So every service outlives the panes and the map view. Menu and toolbar objects outlive the services. Their destructors must not use a service. The comment on PluginServices in src/core/interfaces.h gives the order and the owner of each pointer.

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
| `RouteProvider` | `/1.0` | Supplies route identity, presets, request bodies, and bounded response decoding. |
| `MapProvider` | `/1.0` | Supplies source defaults, fields, validation, and tile URLs. |
| `PreferenceParticipant` | C++ interface, no Qt identifier | Saves and restores bounded JSON under a stable component identifier. |
| `SettingsParticipant` | `/1.0` | Adds a title, editable fields, and optional validation to preferences. |

The unchanged data and map interfaces retain version 1.0. Most UI interfaces use version 2.0 because their service arguments changed. ToolbarPlugin uses version 3.0 because it now requires `name()`. This version removes the former `MapConsumer` interface. The view now receives the map catalog through the shared context.

The host uses icon-only toolbar sections with separators. Plugin names remain available through the API but do not consume toolbar space. The host calls menu callbacks before toolbar callbacks. A toolbar callback receives its plugin menu or null. Plugins can share one action between their menu and toolbar. Plugins must not assume that a menu exists.

Tools and sources require a codec for their panes. The host leaves those panes unavailable when no codec exists. Codec-free panes, menus, and independent map layers remain available. Pane visibility does not unload plugin code or stop a receiver.

The host owns pane placement, visibility actions, and layout persistence. Plugins with commands receive a submenu under Tools > Plugins. The plugin entry has a checkbox that shows or hides its pane. Return and Space also toggle the pane. Hover and Right open the command submenu.

Plugins without commands use a direct checkbox action unless they supply a primaryAction. Marker and Area use direct creation commands. Closing a pane hides it.

# Implemented plugins

| Manifest directory | Name | Roles |
| --- | --- | --- |
| `src/plugins/objects` | Objects | Pane |
| `src/plugins/route_planner` | Route planning | Pane and map tool |
| `src/plugins/valhalla` | Valhalla | Route provider |
| `src/plugins/gdal_vector` | GDAL vector data | Data provider |
| `src/plugins/cot` | Cursor-on-Target XML | Codec |
| `src/plugins/basemap` | Map background | Map layer |
| `src/plugins/map` | OpenGL map | View and toolbar |
| `src/plugins/tracks` | CoT Messages | Map layer, tool, and menu |
| `src/plugins/editor` | CoT Event Generator | Tool |
| `src/plugins/servers` | Servers | Source |
| `src/plugins/activity` | Events | Pane and menu |
| `src/plugins/information` | Information | Pane |
| `src/plugins/trace` | Trace | Menu and map tool |
| `src/plugins/predict` | Predict | Menu and map tool |
| `src/plugins/shapes` | Circles and Lines | Pane, menu, toolbar, and map tool |
| `src/plugins/markers` | Markers | Pane, map tool, and Core collection |
| `src/plugins/polygons` | Areas and routes | Pane, menu, toolbar, and map tool |
| `src/plugins/milstd2525` | MILSTD2525 | Marker collection |
| `src/plugins/mvt` | MVT vector tiles | Map provider and vector tiles |
| `src/plugins/gdal_raster` | GDAL raster maps | Map provider and raster tiles |

The map plugin creates the provider selector through `ToolbarPlugin`. The selector uses `MapSources` and requests the manager through its public signal. The internal-event plugin creates its table and Clear action through `PanePlugin` and `MenuPlugin`. Both use explicit services instead of host widget searches.

The editor, receiver, and event table report operations through `logMessage`. The map renderer and host use the same logger. Plugin code does not write directly to the console.

# Manifest schema

Each MANIFEST contains a JSON object. Its schema version is 1. Qt embeds the object as `MetaData`, so SWAK needs no separate manifest file beside the executable.

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

# CoT detail

`Event::detail` holds all detail children of a CoT event as canonical XML, without the detail element. It holds at most `MaxDetailCharacters`, 32768 characters. The Store also keeps at most `Store::DetailBudget` characters for all events together. `Event::origin` names the server entry that received the event and is empty for local events. `Event::detailTruncated` marks an event whose detail was larger. The typed fields, such as callsign, team, role, course, and speed, stay valid in that case.

Read detail through `core/cot_detail.h`. `detailElements` and `detailElement` return small element trees. Typed readers cover `remarks`, the contact endpoint, `link`, `marti` destinations, `emergency`, `fileshare`, `__chat`, `__video`, `usericon`, color elements with signed ARGB values, and the `status` battery. Readers return empty values for missing or out-of-range data.

`encodeCot` writes the typed fields and then the retained elements. A typed field replaces only its own attributes, so a changed callsign keeps the contact endpoint. Clearing team and role removes `__group`. Clearing course and speed removes `track`. Build new detail with `CotElement` and `detailText`, which skips invalid names and values.

# HTTPS and TAK Server API

`PluginServices::network` is the shared `NetworkClient`. `send(request, context, callback)` starts a request and returns its identifier. The callback runs once on the interface thread with a `NetworkReply`. It does not run after `cancel(id)` or after the context object dies. Each `NetworkRequest` names its expected host and port. The client rejects a URL that does not match. Set `maxBytes` and `timeoutMs` for each request. Fill `tls` with `readTlsFiles` to present a client certificate.

`PluginServices::tak` is the `TakServerApi`. It is null when no server service exists. `request(serverId, method, path, body, context, callback)` sends a request to `https://host:apiPort/Marti/api` of a TLS server entry with that entry's credentials. `info(serverId)` returns the server version from the last check, and `infoChanged` reports changes. `apiUrl` builds the URL and rejects paths without a leading slash or with `..`.

# Contacts

`PluginServices::contacts` is the per-window `Contacts` table model in `core/contacts.h`. The window owns it, and it outlives the panes. Each row is a `Contact` with the UID in `Qt::UserRole`. `refreshServers()` reads the REST contact lists and emits `refreshed(error)` once, with an empty error on success. `readTakContacts` parses one contact list reply. The contacts plugin implements `PanePlugin`, and its MANIFEST declares `pane`. Its pane saves the header state under `org.swak.contacts`.

# Sharing

`PluginServices::sharing` is the per-window `ObjectSharing` service in `core/object_sharing.h`. `share(id, recipients)` shares a local object, `stopSharing(id)` sends a delete, and `isShared` and `recipients` read the state. The service is also the Sharing settings participant. `core/tak_shapes.h` converts a `UserObject` to TAK CoT with `sharedObjectEvent` and builds deletes with `sharedDeleteEvent`. `UserObjects::added` reports objects that a put creates. Preference restores and Undo do not emit it, and a restore emits one `changed` after all objects. The sharing plugin implements `MenuPlugin`. Its MANIFEST declares `menu`. It registers a map tool that adds Share, Share with ..., and Stop sharing to the menus of local objects.

# Received drawings

`core/tak_drawings.h` reads TAK drawing events into a `TakDrawing` with normalized Mercator points, colors, a stroke width, and a label anchor. `takDrawingType` names the types that the drawing layer owns, and the CoT layer skips their point symbols. The TAK Drawings plugin implements `MapLayer` and `MenuPlugin`, and its MANIFEST declares `map-layer` and `menu`. Each window gets a drawing tool that is also the `org.swak.takdrawings` settings participant. The tool parses an event only when its time or detail size changes, so frames do not parse detail XML. Its `itemsAt` returns `org.swak.events` items, so the map selects the event.

# Chat

`PluginServices::chat` is the per-window `Chat` service in `core/chat.h`. `rooms`, `room`, `openDirect`, `send`, and `markRead` give the rooms and send messages. `roomsChanged`, `messagesChanged`, and `roomRequested` report changes. `Store::messageReceived` delivers `b-t-f` events. The Store does not keep them. `readChatMessage`, `readChatReceipt`, `chatMessageEvent`, and `chatReceiptEvent` read and build the ATAK format. The chat plugin implements `PanePlugin`, and its MANIFEST declares `pane`. Its pane saves the splitter state under `org.swak.chat`. The contacts pane adds Chat with when the chat service exists.

# Files and data packages

`PluginServices::files` is the per-window `FileShares` service in `core/file_shares.h`. It lists offers, downloads and imports them, reads and deletes server files, and sends packages. `core/data_package.h` reads and writes zip packages with their limits. `core/package_import.h` imports a package into `UserObjects`, `DataSources`, and `MapSources`. `localObjectFromCot` in `core/tak_shapes.h` converts CoT to local objects. `TakServerApi::martiUrl` and `martiRequest` reach paths below `/Marti`, such as `/sync/content`, with the same entry checks as `apiUrl`. `Store::messageReceived` also delivers `b-f-t-r` offers. The files plugin implements `PanePlugin`, and its MANIFEST declares `pane`.

# Missions

`PluginServices::missions` is the per-window `Missions` service in `core/missions.h`. It lists, subscribes, loads, edits, and exports missions and implements `PreferenceParticipant` under `org.swak.missions`. `Store::removeOrigin` removes the objects of one origin, and `Store::messageReceived` also delivers `t-x-m` mission events. `TakServerApi::martiRequest` takes extra headers, such as the mission bearer token. The missions plugin implements `PanePlugin`, and its MANIFEST declares `pane`. Its pane saves the splitter state under `org.swak.missions.view` and registers a map tool for Add to Mission and Remove from a mission.

# Track history and server icons

`PluginServices::history` is the per-window `TrackHistory` service in `core/track_history.h`. `load` reads past positions for a time window, `add` adds a track, and `loaded` reports each result. The Trace plugin continues a loaded track. The history plugin implements `MapLayer` and `MenuPlugin`, and its MANIFEST declares `map-layer` and `menu`. `PluginServices::icons` is the per-window `IconCache` in `core/icon_cache.h`. `icon` returns a loaded icon or starts a bounded request, and `insert` adds an icon. The CoT layer reads the `usericon` path once for each event change and draws the icon when it is available.

# Video

`PluginServices::video` is the per-window `VideoSources` list in `core/video_sources.h`. It reads `__video` events and `/Marti/api/video` lists and checks stream URLs with `videoProtocol`. It does not link Qt Multimedia. The video plugin implements `PanePlugin`, and its MANIFEST declares `pane`. It is the only part that links Qt Multimedia. Its pane saves the splitter state under `org.swak.video`.

# Emergency alerts

`core/emergency.h` provides `emergencyKinds`, `emergencyEvent`, `emergencyCancelEvent`, `activeEmergency`, and `emergencyLabel`. The emergencies plugin implements `MapLayer` and `ToolbarPlugin`. Its manifest declares `map-layer` and `toolbar`. It reads the Store and does not change it. It centers the map through `MapTools::centerRequested`. Its toolbar button is a trailing action with `swakToolbarTrailingOrder` -1, so it stays left of the navigation lock. Trailing actions sort by this property, and the default is 0.

# Add a plugin

Create a QObject plugin class and its MANIFEST under `src/plugins`. Implement `Plugin` and the required feature interfaces. Add the metadata file to `Q_PLUGIN_METADATA`. List all interfaces in `Q_INTERFACES`.

Register the static target with `swak_plugin` in `CMakeLists.txt`. Link it to the executable and relevant test targets. Add `Q_IMPORT_PLUGIN` in `src/app/plugins.cpp`. Add tests for malformed input, missing services, cancellation, and shutdown as applicable.

Keep callbacks bounded. Do not let exceptions escape Qt calls. Use shared preference and field helpers for saved values. Update the manifest and this inventory when roles change. Run the applicable diagnostics and rebuild the release executable before you complete the task.


# Recovery and table order

The map catalog preserves invalid provider entries and disables their network use. A provider must reject invalid input before it creates a request. Catalog edits validate changed entries without discarding other saved providers.

`Store::SortRole` supplies numeric coordinates and timestamps for table order. `Qt::DisplayRole` retains localized text. `ActivityModel::SortRole` supplies timestamps and numeric severity values. `ActivityModel::Column` names the four columns: Time, Severity, Source, and Event. The model keeps urgency in `ActivityEvent` but does not show it. Table plugins must use these roles when they sort those models.

Provider forms use the host persistence callback. They keep the form open when a save fails. The public plugin interface versions remain unchanged.

# Data provider contract

`PluginServices::data` supplies an optional per-window `DataSources` pointer. It defaults to null for callers without data support. The view subscribes to `contentChanged` and draws data through the catalog. The catalog also implements the shared preference and configuration interfaces.

DataProvider decoders run on at most two background threads. Keep them reentrant and free of QObject or GUI operations. Read the atomic cancellation flag during bounded work. Return complete normalized geometry or an error in `DataDocument`. Keep the plugin instance alive until all windows finish their workers.

`decodeFile(path, cancelled)` reads a local file in place. Formats with sibling files, such as shapefiles, need this method. The default returns an error. `legacyIds()` returns earlier format identifiers. The catalog matches saved entries with these identifiers, so old profiles keep working. The GDAL vector plugin reports `org.swak.kml` as a legacy identifier.

The host owns downloads, per-feed timers, configuration, and rendering. Data plugins supply disabled presets and decode responses. Shared data fields provide endpoint, attribution, timer, and appearance controls. Add forms select one installed format when several data plugins exist. Unknown formats remain in the saved catalog.

Each data plugin declares `data-provider` in its MANIFEST. Its `dataId()` must match the manifest identifier. This version links the GDAL vector plugin statically and adds no external plugin discovery. Read [Data providers](data-providers.md) for parser limits and dependency details.

# Raster tile capability

`MapServiceProvider` adds the optional interface `org.swak.MapService/1.0` to a MapProvider for online services. Its manifest declares `map-provider` and `map-service`. The host rejects the service role without the map provider role. The renderer calls its methods on the GUI thread. A non-empty `sessionUrl` makes the renderer send a POST with `sessionBody` before any tile request. `readSession` returns a token and an expiry. The renderer then builds tiles with `sessionTileUrl` and renews the session before it expires. `attributionUrl` and `readAttribution` report credits for the visible area. The renderer converts them to plain text and `TileRenderer::attribution` returns them. Every request passes through `authorizeMapRequest`, so it must use the source endpoint host. The Google and Azure Maps plugins implement this interface. `validateServiceSource` checks their fixed address and key parameter.

`RasterTileProvider` adds the optional interface `org.swak.RasterTiles/1.0` to a MapProvider. Its manifest declares both `map-provider` and `raster-tiles`. The host rejects the raster role without the map provider role. The renderer calls `renderTile(request, cancelled)` on a worker thread instead of fetching a tile URL. Keep the method reentrant.

`RasterRequest` carries the source, the session credential, the cache directory, the cache size, and the tile address. An empty cache directory means that the provider must not write a disk cache. The renderer passes an empty directory for authenticated sources. Return a 256-pixel image or an error in `RasterTile`. Set `providerWide` when the whole source failed, for example when the dataset does not open.

`MapProvider::legacyIds()` returns earlier format identifiers. `MapProvider::migrate(source)` converts a saved entry with such an identifier. The catalog calls it when it restores a profile and then stores the current identifier. A replaced provider adds no second default entry when the profile already knew one of its legacy identifiers.

# Vector tile capability

`VectorTileProvider` adds the optional interface `org.swak.VectorTiles/1.0` to a MapProvider. Its manifest declares both `map-provider` and `vector-tiles`. The host rejects the vector role without the map provider role. The common Plugin API remains version 2, and MapProvider remains version 1. All plugins still use the same toolchain and Qt installation.

`decodeTile(bytes, cancelled)` runs on a host worker and returns an owning VectorTile value. The provider must support concurrent calls and cooperative cancellation. It must bound input, output, and work. It must not retain borrowed input or access widgets.

An error produces a nonempty error string. A valid empty tile has no features and no error. The host converts standard exceptions into a controlled tile failure.

VectorFeature stores a layer name, an exact identifier string, properties, a geometry kind, and tile-local coordinates normalized by the tile extent. The provider also supplies each feature color and a label-only flag. Polygon paths use odd-even filling for holes. The renderer clips buffered geometry to its tile. The cache adds the prepared image size to the provider's conservative `costKiB` estimate. The static provider must outlive its host workers.

`layerNames()` supplies known layer names before tiles arrive. The host adds names from decoded tiles. The MVT provider uses `visibleLayers` Boolean entries, with absent names visible by default. The shared settings form exposes OpenMapTiles layers and appearance controls. The map context menu changes the same persisted entries. Map information reads loaded feature properties without another network request.

The host prepares vector images on the same bounded workers that decode tiles. The OpenGL view composes the cached images. Decoded features remain available for layer controls and map information. Source and antialiasing changes discard prepared images. Cancellation also covers painting between features. The plugin interface versions remain unchanged.

Each worker owns its QImage until painting finishes. Only completed images cross to the GUI thread. This follows the [Qt rules for painting in threads](https://doc.qt.io/qt-6/threads-modules.html#painting-in-threads).

The host SettingsPage structure accepts an optional widget factory for catalog pages. MapManager and DataManager share ProviderTable in Settings and their separate windows. Catalog actions apply immediately through the existing MapSources and DataSources methods. The host skips generic snapshot forms for these two catalogs, so Apply cannot restore stale provider values. Plugin interfaces remain unchanged.

# Map selection and pane requests

`MapSelection` stores an optional owning `MapItem` value and emits `changed` when that value changes. The host owns this service per window. `MapItem` contains a label, type, plain-text description, JSON properties, normalized world geometry, and drawing dimensions. Selection uses the GUI thread. Consumers must tolerate a null service pointer.

`DataSources::itemsAt` and `TileRenderer::itemsAt` return snapshots for all hits. The map view lets the user choose when objects overlap. It passes the chosen snapshot to the shared selection. An empty hit clears selection. Data documents retain bounded description and property values in each `DataShape`. Vector selection retains the feature properties and selects the closest point in a multipoint feature.

A pane can derive from `PaneContent` and emit `visibilityRequested(bool)`. The host connects this signal to the dock. A show request raises the pane without taking keyboard focus. A hide request closes the pane view.

The pane can override preferredDockArea() to select its initial dock area. The default is the bottom area. Information uses the right area. Saved layouts take precedence. This optional base class leaves existing PanePlugin factories unchanged.

The Information plugin implements PanePlugin and PreferenceParticipant. Its MANIFEST declares `clipboard-write`, a permission for user-requested copies to the OS clipboard. The common Plugin API remains version 2. All built-in plugins compile against the same extended service structure. SWAK promises no external binary compatibility.

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

`UserObject` separates identity, type, title, description, text properties, geometry, and color. Color uses the seven-character `#RRGGBB` format. Older records without color use orange (`#ffc857`). `UserObjects` owns committed records and portable preferences.

Geometry is a bounded JSON object. The circle type uses normalized Mercator coordinates named `x` and `y`. Unknown object types retain their bounded records for future tools. New geometry types can reuse the metadata editor and deletion commands.

`ObjectEditor` works on a copy of a record. Apply validates the whole copy before it writes through `UserObjects::put`. Cancellation drops that copy.

`MapSelection` carries an owner identifier and object identifier for editable selections. CoT selections use owner org.swak.events and their UID. Imported map features without stable identities leave these fields empty. The regular Information pane remains read-only. It asks `MapTools::deleteSelection` to handle deletion, without knowing the owning plugin.

A `MapTool` receives map input events, a viewport, and the map widget. Returning true consumes the event. The map paints registered tools after imported layers and before selection outlines. Each tool receives a saved painter state.

`changed` requests another frame. `cancel` stops a pending operation. Tools must reject events outside their scope and avoid changing committed state during previews.

The tool registry holds guarded pointers. Pane ownership controls each tool lifetime. The marker tool draws circles through the existing OpenGL painter. Its move preview leaves stored geometry unchanged until the click that commits it. Esc cancels an active tool before it dismisses Information. Hiding the map also cancels pending tool operations.

`UserObjects::undoStack` supplies the application Edit > Undo action. Shared deletion commands retain complete records. Their history holds at most 100 commands. The host does not serialize this history. Deleted records reserve model capacity until restoration or history removal. This reserve keeps a full model from blocking undo. GUI input, drawing, and model updates remain on the interface thread.


`MapTool::paintOverlay` draws transient content above the map. Its pointer argument contains the cursor position sampled at paint time, in logical map coordinates. Tools must use that argument for pointer-following previews. The host uses a transparent, mouse-transparent child widget for this content.

Emit `overlayChanged` for pointer-only changes. The host requests an immediate overlay repaint. Qt can defer presentation to the next composed frame. Do not emit that signal from a paint method.

Emit `changed` when stored content or tool mode changes. The host then updates both surfaces.

Overlay painting does not request map tile painting. The view refreshes the overlay when its projection changes. Overlay methods stay on the interface thread and must keep work bounded.


`MapTool::statusText` returns a short active-mode indicator or empty text. `MapTools::statusText` combines these indicators. The host reserves a separate status area and refreshes it on `MapTools::changed`. Plugins emit `changed` when a mode starts or ends. The marker tool returns M during placement and movement. Pointer-only updates do not refresh this area.


`MapTool::guidesActive` reports active placement or movement guides. `MapTools` combines these requests for cursor behavior. Grids controls coordinate grids independently of tools.


# Map overlay panes

The host places all attached panes above the map in `MapPanels`. Panes cover map pixels without changing the map viewport. Plugins keep the same `PanePlugin` and `PaneContent` interfaces. They must use the supplied widget parent. Detached panes remain separate native windows.

`window.overlayLayout` stores pane relationships. `window.layout` stores the outer window toolbar state. The host reads older dock layouts when `overlayLayout` is absent. Individual pane records and unknown plugin sections remain available as before.

The shared object editor shows Collection after Title and Type after Collection. Type lists symbols from the selected collection. The editor adds unique colors from registered collections to its eight general colors. It preserves other valid stored colors. Apply saves the selected color with the object. Deletion undo and movement retain that color.

# Source presets and shared symbols

`MapProvider::additionalSources` optionally returns a JSON object of stable preset identifiers and source records. The default implementation returns an empty object. The host sets each record's `format` to the provider identifier and validates the source. Presets use the same settings, management, networking, and rendering interfaces as user sources. The shared interface remains `/1.0` within the existing same-build static plugin contract.

Preset identifiers use the same bounded syntax as user source identifiers. Plugins must keep identifiers stable and distinct from provider identifiers. `MapSources` adds missing presets to old profiles without replacing existing records. Its `knownPresets` array prevents a deleted preset from returning. The host retains records when a provider disappears and limits the catalog to 32 sources. The remembered preset list stops growing at 256 entries.

`UserObject::symbol` stores an optional identifier of at most 64 letters, digits, periods, or hyphens. An empty identifier means the existing circle. Unsupported identifiers remain in preferences and render as circles. Symbol choice does not change the underlying geometry type. The editor stores the symbol and color together on Apply. Undo, movement, cancellation, and restart retain the complete record.

`markerSymbols`, `markerSymbolForCot`, `markerFrame`, and `paintMarkerSymbol` form the shared vector-symbol API in `core/marker_symbols.h`. The marker tool and CoT Messages plugin use these functions. `MapItem::symbol` carries the frame choice to the selection renderer. No plugin owns a separate icon catalog. The current subset and provenance appear in [the reference review](atak-reference-review.md).

# Marker collection providers

`MarkerCollectionProvider` declares `org.swak.MarkerCollection/1.0`. Its `markerCollection()` method returns one bounded collection definition. The manifest declares the `marker-collection` role alongside any other roles. The host validates this definition before it accepts the plugin. Collection identifiers are independent of plugin identifiers and must remain stable and unique.

The Markers plugin supplies Core through this interface. The separate MILSTD2525 plugin supplies the military catalog. Both use the same schema as imported archives. `PluginServices::collections` points to the per-window `MarkerCollections` service. The host owns this service, and all mutations occur on the interface thread. The same-build static plugin contract still applies.

`MarkerCollections` owns portable definitions, enabled flags, and decoded vector paths. The service validates inputs before it publishes symbols. Rendering uses cached paths rather than repeated JSON parsing. `CollectionSymbol` contains an identifier, name, default color, frame, and glyph. `paintCollectionSymbol` is the shared painter entry point. Archive formats and bounds appear in [Marker collections](marker-collections.md).

`UserObject::collection` stores the collection identifier beside `symbol`. Empty legacy references resolve to Core for an empty symbol and MILSTD2525 otherwise. Explicit unknown references remain intact. `MapItem::markerOutline` carries normalized geometry for the selected local symbol. The selection renderer scales that geometry at the current marker radius. Missing symbols use a circle without changing the saved record.

`ObjectEditor` receives the collection service and stores its last collection and type choices in its existing preferences. The collection manager uses the same provider-table control as map and data management. Settings embeds that manager without pane controls. This change adds no new external dependency or dynamic-code loader.

# Map authentication

Common map fields add `authMode`, `authUser`, `authParameter`, and `authEnvironment`. An absent mode means `none`. Modes `basic`, `bearer`, `header`, and `query` use the same shared request path for every map format. Providers return URLs without secrets. They continue to call `validateMapSource` for common input rules.

`MapSources::setCredential` accepts a bounded session secret for an existing authenticated source. An empty value removes it. `credential` resolves the session value before the restricted environment reference. Secrets never enter `savePreferences`. The map host passes the result to `TileRenderer::setSource` as a separate argument.

`authorizeMapRequest` applies a secret only to HTTPS requests for the configured host and port. It rejects certificate bypass and invalid authentication fields. It sets request attributes that prevent disk storage and automatic authentication reuse. The renderer also removes its disk cache for authenticated sources and blocks redirects. Plugins must not add a second credential path or log authorized URLs.

The generic form selects a choice field's first declared value when a saved value is absent. Locale sorting changes the visible order only. This keeps legacy providers unauthenticated when their editor opens.

# Primary pane commands

PanePlugin can return a pane-owned QAction from `primaryAction(QWidget *)`. The argument is the dock content container, which contains the plugin widget. The host shares that action between Tools > Plugins and the main toolbar. Supply its icon, tooltip, and active state. The default remains the existing pane toggle.

MapTool can emit `activationRequested`, which MapTools forwards to the map view. The view cancels the current tool, calls `activate(QWidget &)`, and focuses the map. The default activation method does nothing. Markers uses this path for the Marker menu action and shares its placement setup with the keyboard action. The editor pane still opens after placement or an explicit edit.

# Polygon tools

The Areas plugin implements Plugin, PanePlugin, and MapExtension. Its embedded MANIFEST declares the pane and map-tool roles. The primary pane command activates polygon creation through the same API as Marker.

MapTool::interactionActive gives an active gesture or editor first access to map events. MapTools then tries inactive tools for unhandled events. Keyboard activation uses activationRequested, so the host cancels previous tool state before it activates another tool.

MapTool::dismiss cancels one interaction step. Its default calls cancel. The Information plugin uses MapTools::dismiss for Escape. Polygon vertex moves override this method to discard the preview while retaining the editor draft. MapTools::cancel still clears all tool state on activation changes and shutdown.

ObjectEditor supports circle and polygon metadata. Polygon editing hides marker collections and uses the Polygon type. setGeometry validates a replacement draft geometry without changing its metadata controls or saved record. reportError shows a recoverable edit error. configurationChanged also reports draft color and geometry changes.

Polygon objects use type polygon and geometry.vertices, an array of normalized [x, y] pairs. The closing corner is implicit. The shared model accepts 3 to 64 distinct corners and rejects crossing edges, overlapping edges, and zero area. The existing object and profile budgets still apply. Unknown plugin records remain in the shared store.

The polygons plugin also creates `route` objects through a second editor pane, `org.swak.routes.pane`. A route stores two to 250 points in the same `vertices` geometry as a polygon. `Length` is a reserved route property. UserObjects measures each object as compact JSON, with a limit of 16 KiB for each object and 128 KiB in total.

MapItem::outlineStyle controls the selected polygon outline. Imported areas retain dashed outlines by default. Local polygons request a solid white outline. Polygon tools require no new external dependency.

## Overlapping map objects

`MapTool::itemsAt(view, position)` returns all committed objects under a logical map position. Each result supplies the stable owner, object ID, label, and type. The default returns an empty list. Hit testing must not change selection or open a menu. Keep results bounded by the underlying object store.

`MapTool::interact(item, event, map)` handles a chosen object. The default returns false. A tool must reject another owner's item and find its current record by ID. A missing record causes no action. Left release selects the object. A context event opens that object's menu. Tool-specific creation, movement, and corner editing remain in `handle`.

A tool can also report the owner of a drawn aid. The FOV cone, Trace, and Predict tools return the owner and ID of the CoT object or local marker under a cone, trace line, or prediction line. The map view resolves CoT owners to the normal CoT selection. It removes duplicate owner and ID pairs before the chooser opens. `lineDistance` in `core/map_tools.h` measures the distance from a point to an open polyline for these tests.

`MapTool::attributions` returns plain-text credits. The default returns an empty list. The map view shows them below the map provider and data feed attributions, with the same link handling.

`MapTools` collects hits and routes chosen objects through this API. Markers and Areas both use it. The view also collects CoT, data feed, and vector feature hits. `DataSources::itemsAt` and `TileRenderer::itemsAt` return all hits. These methods replace the former first-hit `itemAt` methods.

The host opens a chooser when more than one object matches. Menu rows show a label and icon. Tooltips show type and source details. More objects appear on pages of 30. Pages allocate their actions only when the user opens them.

Cancellation preserves selection. Content changes close the chooser before an old result can act. No saved schema changes. These shared C++ classes require all static plugins to rebuild together.


`MapItem::icon` optionally supplies a chooser icon. The marker tool uses `collectionIcon` to preserve its actual symbol and color. The host uses shared UI icons for area and point fallbacks. `uiIcon` provides the application command icons. Plugins must use these factories for matching commands. The icon engine uses Qt vector paths and the active palette, and requires no runtime icon theme or SVG parser.

# Server services and reusable forms

PluginServices now supplies optional Identity and Servers pointers. The host owns these per-window services. Source plugins use the server service for connection management. The generator uses Identity for defaults and Servers for explicit transmission. The CoT codec and connection worker share the same stateless XML decoder. Its parser state is local to each call.

SourcePlugin adds an optional paneId method. A stable pane identifier preserves saved layout when a source receives a new name. The Servers plugin retains the previous UDP identifiers for migration. Its MANIFEST adds network-tcp and network-tls permissions. The catalog recognizes those permissions alongside network-udp and user-selected-files.

SettingsWidgetProvider adds an optional createSettingsWidget method. The host uses that widget for a custom Settings section. Servers uses this interface for its table and management controls. The widget must use its supplied parent and must not outlive its provider. Server edits update the service immediately, like map and data provider edits.

SettingsDialog, SettingsPage, and ProviderTable now live under src/core. The host and plugin widgets share these form and table implementations. Server edit forms use the same validation, alphabetical choice ordering, and single-line labels as other provider forms.

Store adds acceptBatch for bounded groups of decoded events. It groups redraw and changed-row notifications. TrackMotion owns per-UID display state and animation preferences. Its changed signal requests a redraw without changing the event table. MapSelection::paint accepts an optional displayed point and opacity for a moving CoT selection outline.

`SettingField::picker` adds optional CoT selection to a text field. `cotSetting` creates a bounded field for Type, How, Team, or Role. SettingsDialog keeps the text editor as the saved value and adds the shared selection button beside it. Custom values remain supported.

`cotFieldEditor` supplies the same control for plugin forms. Its returned container owns the supplied line editor. The caller sets the editor name, accessible name, and maximum length before wrapping it. Picker dialogs belong to that editor and close when it is destroyed. Plugins must use these shared controls for editable CoT fields.

# Trace and shared interaction hooks

MapTool::populateContextMenu receives the selected MapItem and its menu. Tools can add actions without replacing the owning plugin's commands. MapTools calls each registered tool. Actions must retain stable identifiers and use a QObject receiver with a suitable lifetime. Live position updates keep object menus open. Removed CoT rows and map-source changes still close affected chooser menus.

TracePlugin uses the existing menu and map-tool roles. Its MANIFEST declares org.swak.trace. The menu owns its TraceTool, which registers through PluginServices::mapTools. The helper uses Store and UserObjects to record authoritative positions. It does not record predicted display positions. It exposes configuration through SettingsParticipant and draws through MapTool::paint.

PaneContent::watchPane attaches the host dock to the content. PaneContent::paneClosed receives an explicit close event from that dock. The default method does nothing. Information clears both map selection and CoT selection. Child dialogs and tab changes do not count as pane closure.

SettingField supports enabledBy and enabledValue. A field names a choice field in the same configuration page and its required value. An enabledValue array accepts any listed value. SettingsDialog disables the editor and label when the value differs. Disabled fields retain their values.

The Servers form uses this for TLS credentials. These additions retain plugin API version 2 because this build links and rebuilds all native plugins together.

Store::eventAccepted emits each accepted authoritative Event on the GUI thread, including events inside a batch. A receiver must copy any data that it retains. Trace uses this signal so a batch cannot hide intermediate accepted positions. Store::changed still marks the completed batch for drawing and disappearance handling.

MapTool::itemsChanged marks changed local object membership or properties. MapTools forwards it separately from drawing updates. Object chooser menus use this signal to cancel stale choices. Continuous trace repainting does not close menus.

# Restoring section defaults

The host captures standard SettingsPage values before it loads saved preferences. Plugins must expose their startup defaults through savePreferences at that point. SettingsPage.defaults carries this snapshot to the shared dialog.

Restore section defaults updates only the fields in the current section and tab. Apply and OK use the existing validation and save callbacks. Cancel discards staged values. Hidden fields and plugin records remain intact. Custom management widgets keep their own actions and do not receive a generic reset button.

# Information actions and paused positions

MapTool::populateInformationMenu contributes checkable actions for the selected object. Information shows these actions as checkboxes and invokes the same QAction commands. MapTool::actionsChanged requests a state refresh through MapTools. Trace uses the same action implementation for its context menu and Information control. Other plugins do not need Trace-specific identifiers or access to its records.

TrackMotion::setPaused freezes its displayed snapshot. Store continues accepting and replacing messages through its usual bounded path. Resuming clears prediction and interpolation for the retained latest positions. Later messages use normal animation. Pause state lasts only for the session and is not a saved preference.

TrackMotion::pauseChanged reports pause transitions. TraceTool retains a bounded display snapshot during pause while its live history continues recording. Resume discards the snapshot and draws the latest retained history.

The shared mapLabelBounds and paintMapLabel helpers use the same shortened text and padding. The selected argument inverts foreground and background colors. Plugins must use matching bounds for label clicks. DataSources::paint accepts the selected MapItem to style feed labels.

Feed items use a provider owner and a shape index for session selection. Vector labels retain normalized cachedLabelBounds in MapItem. MapItem also retains cachedLabelText and its normalized font height. MapSelection redraws selected vector labels with white text on black after the selection geometry. It does not rebuild tile images.

SettingField::unlockToEdit makes a text field read-only until the user selects its lock button. The dialog owns the lock state and does not save it. Section reset preserves these fields. Identity uses this shared contract for UID in both Identity and Settings.

setToolbarSection assigns a shared toolbar section and order to a QAction. The host groups tagged actions after plugin registration. Creation contains Marker, Area, Circle, and Line. Measurement contains Distance. Plugins reuse their existing actions, so menus and toolbar controls retain the same state and callbacks.

cotFieldDescription uses the same category catalog as the CoT selectors. Information uses it for CoT Type and How without changing the stored message. Unknown categories retain the original code.

# Circle, line, and measurement tools

ShapesPlugin uses one shared ObjectEditor and MapTool for its three creation modes. Its menu and toolbar reuse the same QAction objects. Each host owns its editor and tool instances. The plugin tracks editors with QPointer and removes destroyed instances.

UserObject type range-circle stores a center and radius control point in geometry.vertices. Type line stores two endpoints. The existing circle type remains a point marker. Shared validation bounds coordinates and rejects duplicate points. Circles must fit inside the projected world.

ID, Area, and Distance remain computed properties for these shapes. ObjectEditor retains the common title, description, colors, and property table.

MapTool::areaFillOpacity optionally supplies the shared area fill value. MapTools uses the first provider, or 0.3 without one. PolygonTool supplies the configured Area value. ShapesTool uses it for circles. The provider must return a finite value between zero and one.

Distance measurements stay inside ShapesTool and never enter UserObjects. The tool keeps at most 64 measurements. A monotonic clock controls duration and fading. The timer stops when the list becomes empty. Circles and lines use the existing preference store and deletion undo history.


MenuPlugin::rootActions returns true when a plugin contributes direct entries to Tools > Plugins. The host omits that plugin submenu and its editor toggle. ShapesPlugin uses this for Circle, Distance, and Line. Other plugins retain their pane actions.

Shared menu ordering uses QCollator and ignores mnemonic markers. It sorts dropdown and popup menus and removes ordinary separators. Actions with a positive menuTailOrder follow the sorted entries. File uses this property for the separator and Quit action. The menu bar keeps File, Edit, View, Tools, Help in that order. Toolbar separators remain intact.

The shared addProviderToolbar helper adds a multi-select source menu with the existing management action. The host uses it for Servers and Data providers. Each toggle reads the latest source record before it updates its enabled field. Recoverable errors use the shared logger. These controls keep provider management in the host and use the existing source services.

SettingField::describeCode displays CoT descriptions while retaining the raw code. cotFieldValue and setCotFieldValue read and write that value. Identity uses this for Type, Location method, Team, and Role. The description field is read-only.

The shared picker supports selection and custom raw values. UiBehavior uses disabled palette colors for read-only text fields while preserving selection and copying. It restores the original palette when editing resumes. Standard OK and Apply buttons use distinct shared icons.

MapPanels consumes unhandled wheel events at the overlay boundary. Child controls receive wheel events first. The map filter also rejects wheel events over the visible overlay mask. Floating panes remain separate windows.

The plugin catalog exposes pluginManifest for accepted built-in instances. It returns validated embedded metadata or an empty object for unknown instances. The Plugins pane shows each accepted plugin once. Selection shows every manifest field as read-only plain text. No plugin interface version changes.

# Marker scaling and Predict

Viewport carries markerScalePercent with a default of 100. Its markerSize helper accepts the six supported percentages and otherwise uses 100%. Marker layers must use that helper for symbol radii and hit targets. markerOffset adjusts a label anchor by the changed radius. Label fonts and line widths remain in logical screen pixels.

MapItem.radius stores the unscaled symbol radius. MapSelection applies the viewport scale when MapItem.scaleIcon is true. Basemap features set scaleIcon to false because their symbols belong to the tile renderer. Geographic geometry does not use the icon scale. These additions retain API version 2 because all static plugins rebuild together.

PredictPlugin uses MenuPlugin, MapExtension, MapTool, and SettingsParticipant. Its MANIFEST declares org.swak.predict. Its context and Information actions share the same implementation through the existing MapTool hooks. The menu owns the tool. Predict requires no window-specific code or new dependency.

PredictionHistory derives current speed and bearing from the latest segment. The preceding segment supplies acceleration and heading change. The model bounds acceleration to five meters per second squared. It bounds heading change to five degrees per second. Both trends decay with a 30-second time constant.

Five-second integration steps follow great-circle segments, with speed bounded to 1000 meters per second. The model accepts horizons through 1800 seconds. It does not use the animation estimate as input.

PredictTool listens to Store::eventAccepted and keeps history only for enabled CoT identities. It uses a private TrackMotion instance for forecast endpoints. That member has no QObject parent because it is not a separate configuration participant. It inherits movement duration, teleport distance, and fade duration from the store animation. The tool disables its short movement extrapolation because the endpoints already represent forecasts.

The source TrackMotion snapshot supplies the line origin, stale color, and opacity. pauseChanged freezes the private animation too. Resume snaps both engines to their latest positions. Predict records do not alter Store events, object selection, or outgoing messages.

TrackMotion::remove discards a live animation record without a fade. A paused snapshot remains visible until resume. Predict uses this method for removed targets and new history after an expired source. Explicit target removal also removes that target from painting immediately.

UserObject.restricted stores the Area restriction flag. JSON uses the optional restricted boolean and defaults to false when the field is absent or invalid. ObjectEditor exposes the control only for polygon records and emits configurationChanged for draft previews. The Area renderer adds Qt BDiagPattern in screen coordinates above the existing fill. Editing vertices retains the flag because it is separate from geometry.


# Navigation and deletion controls

PluginServices::navigationLock supplies an optional QAction owned by the map view. The view publishes it during creation. The map toolbar plugin exposes the same action. Its checked state blocks user panning and zooming and persists as navigationLocked. All static plugins rebuild together, so the common API version remains 2.

The swakToolbarTrailing action property places controls after an expanding spacer. arrangeToolbarSections handles this property and the existing creation and measurement groups. Repeated arrangement replaces the old spacer.

MapPanels consumes unhandled keyboard and wheel events after child controls process them. Overlay boundaries must not forward navigation events to the map.

The shared confirmDeletion helper uses plain text and bounds the displayed name to 256 characters. Enter activates Delete. Escape activates Cancel. deleteObject captures the object identity before the dialog and delegates accepted deletion to UserObjects.

Model removal remains available without a dialog for noninteractive work. Interactive deletion paths must use the shared helper. Context actions use menuOrder to place Edit before Delete ... while other entries retain locale ordering.

Predict draws successive segments from the source to 30 seconds, then 1 minute, then 5 minutes. The final segment ends at its tick with a flat cap and a short solid connection. Perpendicular endpoint marks measure 14, 10, and 6 logical pixels. Forecast labels are absent. Areas expose their Restricted flag as a derived Information property.


# Main view and coordinate grids

The host places its ViewPlugin widget in the Map tab of mainViews. MapPanels remains a child of that widget, so its overlay coordinates do not include the tab bar. StatusProvider still receives the actual view. Hiding the tab cancels map interaction and stops panning through the existing view lifecycle.

PluginServices::viewActions publishes optional view-owned actions for the View menu. The map publishes Grid, Scale, and Zoom Level. These actions share state with the map configuration fields and persist through SettingsParticipant.

ToolbarSection::Providers groups map, server, and data selectors before creation and measurement actions. CoT Messages retains MenuPlugin but no longer implements ToolbarPlugin. Its MANIFEST matches these roles.

GridTypes is a map-owned SettingsParticipant and SettingsWidgetProvider. Its component identifier is org.swak.grids. It stores at most 32 validated named records, one active identifier, and the last selected identifier. The geographic, utm, and mgrs system identifiers select renderers. All records use the same editor and catalog operations.

Missing active identifiers hide the grid. GridTypes::setVisible restores the last selected type or a remaining default. The lastSelected preference preserves that choice while the grid is hidden. Legacy map grid values migrate only before the catalog receives restored or explicit user state.

Each record contains name, system, color, opacity, spacing, width, and format. The renderer bounds zones, per-axis lines, and line samples. GeographicLib supplies UTM/UPS conversion and MGRS notation. Drawing uses the current Viewport and logical pixels. This extends the same-build static API without a new plugin loading mechanism.

MapPanels publishes its occupied region through the shared MapOcclusionProperty widget property. The map positions reference controls above bottom overlays on the left edge. Map coordinates and click targets still use the complete viewport. Scale distance uses the legend position, not a resized map projection.

# Additional source panes

SourcePlugin::additionalPanes returns optional PluginPane records. Each record supplies a stable id, a translated title, and a widget owned by the supplied parent. The host places each widget in an overlay pane and adds a checkable Tools > Plugins action. The shared layout service saves geometry and visibility. Plugins declare these panes in the MANIFEST additionalPanes array with id and name fields.

The Servers plugin supplies org.swak.lan.pane for Local LAN status and peer rows. It uses the same Servers service as the main browser. Servers stores Local LAN configuration as ordinary server records with protocol lan and localAddress. The former global lan getter and setter no longer exist. Statistics identify each peer through serverId and peerId. LAN server statistics expose state, address, and peer count.

Plugin API version 2 remains the same because this build compiles all static plugins together. The outer preference format remains version 1. Servers uses its own serverSchemaVersion value of 2 to track migration inside that format.

# Motion details and map controls

Event has optional course and speed values. Course uses degrees clockwise from true north. Speed uses meters per second. The CoT codec reads and writes these values through detail/track. The shared validator accepts courses from 0 through 360 and speeds from 0 through 1000. Missing values remain unknown.

TrackKinematics holds speed, heading, and flags that identify calculated speed and estimated heading. TrackMotion exposes these values through kinematics and VisualEvent. Its frozen snapshot preserves these values while paused.

The shared groundMotion helper calculates ground speed and initial bearing from two positions and message timestamps. PredictionHistory uses the same helper. Estimates require intervals from 50 milliseconds through 10 minutes and speeds no greater than 1000 meters per second. TrackMotion also rejects estimates across its configured teleport distance or an expired track.

The CoT layer draws a heading cone at the animated position. The cone uses the same opacity and stale color as its marker. Reported course takes priority over estimated heading. HeadingConeStyle holds visibility, angle, length, fill opacity, outline width, and outline opacity. The tracks plugin owns the FOV settings page, `org.swak.fov`, and sets the style through `TrackMotion::setHeadingConeStyle`. The page falls back to the earlier `org.swak.track-motion` section. CoT animation configuration no longer contains the cone style. Its length uses logical pixels and marker scaling.

The cone indicates direction, not a measured sensor field of view. The Information pane shows speed, heading, and their sources. Static objects without motion history receive no invented velocity.

ObjectEditor exposes Restricted for polygon and range-circle records. Both tools draw the saved flag as diagonal fill lines. Circle Information includes the flag. The polygon tool handles a corner double-click as Move and consumes the final release. Its corner menu gives Move priority through menuOrder.

mapZoomLegendRect and mapZoomLevelAt share screen geometry for drawing and hit testing. Clicking the right-side legend selects a bounded integer zoom level around the map center. The navigation lock blocks this change. Lock buttons show the action available on the next click.


The host and graphical test executables call configureGraphics before QApplication. This shared startup helper sets the OpenGL format and the default X11 integration preference. Plugins do not change the graphics integration after startup. The addMenuFooterAction helper creates a separated final menu action with explicit sorting priority. Provider toolbar menus use it for More ... without renaming their shared management actions.


# Additional editor panes and preference migration

PanePlugin::additionalPanes returns optional PluginPane records, like SourcePlugin::additionalPanes. Each record provides a stable layout identifier, a translated title, and a parent-owned widget. The pane plugin declares extra panes in its MANIFEST. Root-action tools control editor visibility through their creation commands. Settings discovers each editor through the shared participant interface.

PreferenceParticipant::preferenceFallbackId supplies an optional migration source. The host reads that section only when the current section is absent or is not an object. Future writes use the current identifier. Unknown sections remain intact. Lines migrates from org.swak.shapes into org.swak.lines. Circles retains org.swak.shapes and the existing org.swak.shapes.pane layout identifier.

TrackMotion stores bounded headingConeTargets preferences for up to 4096 object identities, with 128 characters per identity. No target starts enabled. setHeadingConeEnabled changes one target and emits coneTargetsChanged. The CoT plugin contributes the same checkable FOV cone action to context and Information menus through MapTool. The renderer requires both the object flag and the shared appearance switch.

TrackMotion::configurationChanged reports a change to restored animation configuration. Predict uses this signal instead of copying preferences on every animation frame. TrackMotion::animationPreferences excludes object cone flags, so incoming predictions do not copy them.

Predict caches the teleport threshold and endpoint animation configuration at construction and on that signal, including while no targets are active. Its incoming-event and painting paths do not fetch configuration. TrackMotion, Trace, map navigation, and shape measurement timers use typed fields populated by restorePreferences. The cone renderer reads the existing typed HeadingConeStyle cache rather than preference data.


# Objects and routing

Objects is a PanePlugin with a sorted proxy over Store. It shows one row per current CoT UID, including objects outside the viewport. Store expiry and removal update the table through model signals. The pane does not keep another event history. cotMapItem builds the shared CoT identity and properties for map and table actions.

MapTools::centerRequested carries projected world coordinates. The map clamps the center and preserves zoom and selection. Explicit centering works while manual navigation is locked. Objects uses this signal for double-clicks. Its context menu uses the same MapTools contributions as the map, plus Information.

RouteProvider/1.0 supplies routeId, routeName, presets, request, and decode. Its stable routeId matches its MANIFEST id. The manifest role is route-provider. RouteResult contains longitude/latitude pairs, meters, seconds, and an error.

The host supplies RouteSources through PluginServices::routes. RouteSources owns provider configuration, bounded HTTPS requests, cancellation, and result notification. It retains unavailable provider entries.

The Route planning plugin uses PanePlugin and MapExtension with the tool role. Its MapTool handles endpoint picking and returns the provider attribution through `MapTool::attributions`. Its primaryAction is the Route planning toolbar button, which shows or hides the pane. Results become `route` objects in UserObjects, which the polygons plugin draws and edits. Its SettingsWidgetProvider exposes provider management through the shared Settings host. Valhalla supplies the first route-provider format. Read routing.md for protocol bounds and routing limits.
