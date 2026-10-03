# Security boundaries

Incoming XML and file contents are untrusted data. Native plugin code is trusted application code. The receiver accepts unauthenticated positions, so a valid message does not establish that a position is true.

The application applies these input limits:

- Messages contain at most 16384 bytes.
- XML contains at most 12 nested levels and 256 elements.
- Each element contains at most 32 attributes.
- Identities, types, and callsigns contain at most 128 UTF-16 code units.
- The store contains at most 4096 event identities.
- Events expire no later than seven days after reception.

The parser rejects document type declarations, entity references, processing instructions, null bytes, and unsupported encodings. It rejects duplicate point, detail, and contact elements. Unknown elements are accepted only within bounded detail content and are then discarded.

Text fields exclude control, format, and surrogate characters. This restriction excludes supplementary Unicode characters, including many emoji. The interface presents external text as plain text. The attribution link escapes all source text before it creates HTML. User actions can open HTTP or HTTPS license links through the default browser. Links with credentials or other schemes cannot open.

Coordinates must be finite decimal numbers within geographic ranges. Altitude must lie between minus 100 million and plus 100 million meters. Horizontal and vertical uncertainty must lie between zero and 100 million meters.

Timestamps must include a timezone. The stale time must follow the start time and cannot precede the event time. Events with start or event times more than five minutes ahead are rejected.

Expired events and older updates are rejected. Equal timestamps can replace an existing identity. These rules reduce accidental inconsistency but do not prevent sender impersonation or replay.

File import opens files without waiting for pipe writers and then inspects the open descriptor. It accepts regular files only and reads at most the message limit plus one byte. Export uses `QSaveFile` to replace the destination after a complete write.

The receiver starts after an explicit interface action or restoration of a saved running state. Its default binding accepts local traffic only. The bounded polling loop limits work per interface cycle, and the operating system drops traffic when its socket queue fills.

The application creates no persistent event cache and writes no message logs. Preferences include editor drafts, file directories, and the receiver address. Treat the preferences file as private data. Remote map requests require a selected provider and enabled online maps. The application does not run shell commands or load external application plugins. Qt still loads its system platform and graphics plugins as part of normal startup.

Use a trusted Qt installation and graphics driver. Native libraries and graphics drivers share the application process. This version does not provide a process sandbox, encrypted transport, or sender authentication.

# Development rules

Keep new dependencies limited to features that Qt cannot reasonably provide. Document the reason and input boundaries before adding a dependency. Keep certificate and hostname checks enabled by default. Map sources support a user-selected exception, as requested, with separate caches and a visible warning.

Bound allocations and work before parsing external data. Add tests for malformed input and resource limits. Keep generated files and local tool paths out of Git.

Use the sanitizer preset after changes to parsing, ownership, or file handling. Run the native GUI test after interface changes. Record any untested graphics backend or target platform in the test report.

Follow [Development diagnostics](quality.md) for repeated memory, responsiveness, coverage, and source analysis runs. Exercise recoverable failure paths and successful operations after failures. Keep work and memory bounded during sessions that last for hours. Do not ignore sanitizer failures or attempt to recover from memory corruption inside the same process.

# Preferences storage

Preferences use a versioned JSON file with a one MiB limit. The file accepts regular-file reads only. Invalid files and unsupported versions remain untouched, and the interface reports the error.

Writes replace the file atomically through `QSaveFile`. The saved file permits access only to its owner. A process lock prevents concurrent instances from overwriting the same preferences.

Unknown plugin sections remain in the document. Plugins must bound every value before applying it to controls or operations. Do not store credentials in this file.

# Diagnostic output

The application disables text logging before Qt startup. Only an explicit enabled flag in the selected profile permits output. Severity, urgency, source filtering, and rate limits apply to both application and Qt messages. Logging configuration uses the bounded preferences reader and preserves unknown fields.

Messages have bounded length and cannot inject terminal control characters or extra log lines. Excess messages are dropped. Nonblocking pipe and socket writes prevent diagnostic backpressure from stopping the interface. A broken output pipe cannot terminate the application. Regular-file output still depends on filesystem latency.

Plugin code must use the shared logger and avoid private payloads. Qt diagnostics can include paths or other private context. Fatal diagnostics retain their original termination behavior even when text output is disabled. Development diagnostic tools retain their independent output.

Map input limits, cache isolation, and HTTPS behavior are documented in [Map providers](map-providers.md). Remote tiles remain untrusted after HTTPS checks and after cache retrieval.

# Internal events and plugin manifests

Internal-event history uses bounded memory and plain text. It does not create an event file. Worker threads submit records through a bounded inbox, without queued Qt calls for each message. Excess bursts discard the oldest pending records. Console output still requires explicit configuration.

Each built-in plugin declares its API, roles, requirements, and permissions in an embedded MANIFEST. The host rejects incompatible declarations before it uses the plugin features. These declarations do not restrict native-code permissions. Static linkage and the existing trust boundary remain unchanged. See [Plugin API](plugin-api.md).

# KML and KMZ feeds

Data Providers adds HTTPS feed input and ZIP decompression. Read [Data providers](data-providers.md) for request isolation, cancellation, XML limits, archive limits, and supported content. Feed descriptions and linked resources never execute or load automatically. Libarchive is the only new non-Qt library.

MVT tiles cross an additional binary-input boundary. The pinned protozero decoder reads binary fields. Application limits cover wire types, lengths, dictionaries, features, commands, vertices, property storage, and coordinate arithmetic. Worker cancellation and source generations prevent stale results. Map information presents plain text without active links. No remote style expression, script, sprite, or font is executed or loaded. Read `docs/dependencies.md` for provenance and advisory review.

# Map information and movement

The Information pane treats remote metadata as plain text. It never loads remote images or executes description markup. Copy actions write only after a user double-click. KML metadata uses limits for names, values, property counts, and total retained bytes. Invalid metadata rejects the new document and leaves the previous feed intact.

Map selection holds owning values instead of borrowed geometry pointers. Feed changes clear data selection. The host destroys pane widgets before their services. Key panning stops on key release, focus loss, window deactivation, and hiding. Each timer tick limits elapsed movement to 100 milliseconds after a stall.

Tile fetching permits zero through two extra rings, with at most 64 extra tiles. Visible downloads take priority. Background tiles cannot force visible vector tiles from memory. Existing request, decoder, TLS, and cache limits still apply. Prefetching does not introduce bulk downloads or a persistent offline archive.

# Dense vector tiles

Vector downloads have an eight MiB limit. Raster downloads retain the two MiB limit. Vector parsing permits 500000 vertices and 50000 features per tile. The estimate for retained geometry and properties cannot exceed 64 MiB per tile. Two workers and four combined requests or jobs remain the concurrency limits.

Memory recovery releases cached geometry before it discards visible images. Compact images use 256 KiB each and remain inside the configured cache budget. The coverage audit respects request backoff, source generations, cancellation, and shutdown. Invalid data remains an error. It never counts a missing tile as rendered.

# Local editable objects

Local objects remain in the private preferences file and are not transmitted. The model accepts at most 256 records. Each formatted record contains at most 8192 bytes. Live records and deleted records reserved for undo share a 128 KiB budget. The undo stack retains at most 100 commands. Undo snapshots are process-local and disappear on restart.

Titles contain at most 128 characters. Descriptions contain at most 2048 characters. Each record has at most 32 text properties. Names contain at most 64 characters, and values contain at most 256 characters. Duplicate names and reserved coordinate names prevent Apply. Geometry uses finite coordinates and bounded JSON containers, with at most eight nested levels. Circle coordinates remain within the normalized world bounds.

The editor changes a copy until Apply succeeds. Move previews do not alter saved coordinates. Imported objects never enter the editable store. Selection ownership prevents a marker command from deleting imported data. Objects and undo commands use per-window ownership. Tool registration uses guarded QObject pointers and no new worker threads or timers.


Marker records accept colors only in `#RRGGBB` format. The existing record and model budgets include this field. Color selection does not load files, styles, or remote resources. Overlay panels add no worker, network request, or dependency.

The image provider accepts only known coordinate placeholders and a boolean row-inversion value. Tile coordinates remain bounded before shifts or quadkey generation. New presets do not enable downloads. Military symbols use built-in vector paths and bounded identifiers, without external fonts, images, or symbol scripts. Unknown saved identifiers remain intact and render as circles.

# Marker collection archives

Collection archives use the existing libarchive library with ZIP, TAR, and gzip support. The importer reads a regular file of at most one MiB. It accepts one regular `collection.json` entry with at most 256 KiB of expanded data. It rejects links, encryption, extra entries, and other paths. It never extracts archive members to disk or enables external decompression programs.

Vector definitions accept bounded move, line, and cubic commands. They reject nonfinite coordinates, invalid colors, duplicate identifiers, and excessive geometry. The renderer does not load external images, fonts, scripts, or URLs. Imported tooltip text is escaped before Qt renders it. Catalog storage and decoded paths have fixed input limits. Libarchive and Qt still share the application process and remain part of its native-library trust boundary.

# Map credentials

Map authentication adds a boundary between the profile, session memory, and HTTPS requests. Profiles contain authentication metadata and an optional restricted environment-variable name. Passwords and tokens remain outside saved preferences. No credential library or new external dependency is added.

The shared request helper limits secrets, rejects controls, and checks the destination host and port. Authentication requires certificate validation even when a source offers a certificate exception. Redirects stay disabled. Authenticated requests cannot use the shared disk cache. Source and credential changes cancel old work and clear decoded images. HTTP 401 and 403 responses use the provider-wide retry delay.

Environment references use the `SWAK_MAP_` prefix. Only put dedicated map secrets in that namespace. A profile can direct its named secret to its configured server, so use trusted profiles. Query credentials reach the provider URL and can appear in its server logs. SWAK uses generic tile errors without those URLs. Process memory and launch environments remain sensitive. Read [Map providers](map-providers.md) for storage limits and supported authentication methods.

# Local polygon geometry

The shared model accepts at most 64 corners per polygon. Coordinates must be finite and within the normalized map bounds. Validation rejects duplicate corners, crossing edges, overlapping edges, and zero area. It also reserves the Area property for the calculated value. Existing record, profile, object-count, and undo budgets still apply.

Corner moves and deletion modify the editor draft only. Apply validates the full object before storage. Invalid moves retain the previous draft geometry. Escape discards the move preview. Tool changes, pane closure, and window shutdown discard unfinished geometry. The polygon tool adds no workers, timers, file access, or network access.

Area calculations use bounded numerical integration on a sphere. They do not provide an ellipsoidal survey measurement. Geometry validation uses floating-point comparisons and rejects near-zero projected areas below a fixed threshold. It does not claim exact arithmetic for nearly coincident edges.
