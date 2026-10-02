# Plugin API

This review covers all ten built-in plugins. Every plugin implements `Plugin` and uses the shared contracts in `src/core/`. The common API version is 2. No plugin depends on a concrete application window class.

The application links plugins into the executable. Qt embeds each `src/plugins/<plugin>/MANIFEST` file through `Q_PLUGIN_METADATA`. The common metadata identifier is `org.swak.Plugin/2.0`. The application does not load external plugin binaries or search plugin directories.

# Host services

`PluginServices` supplies references to the event store, internal-event model, and map catalog. The host passes this context to each UI factory and menu or toolbar callback. The context exists only during the call. Plugins can capture its service pointers in callbacks whose QObject context limits their lifetime.

The host owns the services for each window. Plugins must not save service pointers, menus, or widgets in their static plugin objects. Qt parents own created widgets and actions. The host destroys plugin widgets before the event store. All model and widget operations use the GUI thread.

Use `services.store` for CoT data and selection. Use `services.maps` for provider discovery, selection, and catalog changes. Emit `MapSources::managementRequested` to request the provider manager. Use `services.activity` for internal-event history. No plugin searches the host widget tree to find these services.

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
| `ToolbarPlugin` | `/2.0` | Adds controls through `populateToolbar(PluginServices &, QToolBar &, QMenu *)`. |
| `MapExtension` | `/1.0` | Supplies a stable map identifier, name, and Provider or Tool kind. |
| `MapProvider` | `/1.0` | Supplies source defaults, fields, validation, and tile URLs. |
| `PreferenceParticipant` | C++ interface, no Qt identifier | Saves and restores bounded JSON under a stable component identifier. |
| `SettingsParticipant` | `/1.0` | Adds a title, editable fields, and optional validation to preferences. |

The unchanged data and map interfaces retain version 1.0. The UI interfaces use version 2.0 because their service arguments changed. The former `MapConsumer` interface is removed. The view now receives the map catalog through the shared context.

The host calls menu callbacks before toolbar callbacks. A toolbar callback receives its plugin menu or null. Plugins can share one action between their menu and toolbar. Plugins must not assume that a menu exists.

Tools and sources require a codec for their panes. The host leaves those panes unavailable when no codec exists. Codec-free panes, menus, and independent map layers remain available. Pane visibility does not unload plugin code or stop a receiver.

The host owns pane placement, visibility actions, and layout persistence. Plugins with commands receive a submenu under Tools > Plugins. Its Show pane action controls the pane. The submenu checkmark reflects visibility. Plugins without commands use a direct checkbox action.

# Implemented plugins

| Manifest directory | Name | Roles |
| --- | --- | --- |
| `src/plugins/cot` | Cursor-on-Target XML | Codec |
| `src/plugins/basemap` | Offline reference map | Map layer |
| `src/plugins/map` | OpenGL map | View and toolbar |
| `src/plugins/tracks` | CoT Events | Map layer, tool, menu, and toolbar |
| `src/plugins/editor` | CoT Event Generator | Tool |
| `src/plugins/udp` | UDP receiver | Source |
| `src/plugins/activity` | Internal events | Pane and menu |
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
| `id` | Unique stable identifier under `org.swak`. Map provider identifiers must match `mapId()`. |
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

Permission values are `network-udp`, `network-https`, `disk-cache`, `browser-links`, and `user-selected-files`. They document access and do not create a security sandbox. Native plugin code retains application permissions. Static plugins use the same compiler, Qt installation, and build flags as the host.

`builtInPlugins()` validates manifests before the host uses their features. It rejects incompatible declarations and duplicate identifiers. It reports failures through the shared logger. Unknown metadata identifiers do not enter the SWAK plugin catalog.

# Internal events

Use `logMessage(severity, urgency, category, message)` from any thread. Internal categories start with `swak.`. Supply a short operational description without payloads, credentials, or editor drafts. Severity values are Debug, Info, Warning, Error, and Fatal. Urgency values are Routine, Attention, and Immediate.

Internal history remains independent of text output. Text logging stays disabled until the user enables it. Qt diagnostics follow the text-output configuration and do not enter internal history.

A mutex protects a 256-record inbox. Each category contains at most 128 characters, and each message contains at most 2048 characters. The model collects records every 100 milliseconds. Excess bursts discard the oldest inbox records. This bound prevents an event queue from growing during overload.

The model retains 500 events by default. File > Settings > Internal events allows 1 through 10000 events. Lower limits remove the oldest rows immediately. History stays in memory and does not survive process restarts. The limit, pane layout, table widths, order, and sort selection survive restarts.

The status field shows the latest collected event for three seconds. A newer event restarts that timer. Errors and fatal events use red text in the field and table. Clearing the table also discards pending events for that model. It does not change text-output configuration.

# Add a plugin

Create a QObject plugin class and its MANIFEST under `src/plugins`. Implement `Plugin` and the required feature interfaces. Add the metadata file to `Q_PLUGIN_METADATA` and list all interfaces in `Q_INTERFACES`.

Register the static target with `swak_plugin` in `CMakeLists.txt`. Link it to the executable and relevant test targets. Add `Q_IMPORT_PLUGIN` in `src/app/plugins.cpp`. Add tests for malformed input, missing services, cancellation, and shutdown as applicable.

Keep callbacks bounded and do not let exceptions escape Qt calls. Use shared preference and field helpers for saved values. Update the manifest and this inventory when roles change. Run the applicable diagnostics and rebuild the release executable before completing the task.


# Recovery and table order

The map catalog preserves invalid provider entries and disables their network use. A provider must reject invalid input before it creates a request. Catalog edits validate changed entries without discarding other saved providers.

`Store::SortRole` supplies numeric coordinates and timestamps for table order. `Qt::DisplayRole` retains localized text. `ActivityModel::SortRole` supplies timestamps and numeric severity and urgency values. Table plugins must use these roles when they sort those models.

Provider forms use the host persistence callback. They keep the form open when a save fails. The public plugin interface versions remain unchanged.
