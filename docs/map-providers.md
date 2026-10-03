# Map providers

Research date: October 2, 2026. A provider supplies map images or vector geometry. MVT is the Mapbox Vector Tile format. A format plugin converts the selected source configuration into tile requests. This implementation supports XYZ, WMTS 1.0 KVP, WMS 1.3 images, and MVT vector tiles in Web Mercator, also called EPSG:3857.

# Sources and requirements

OpenStreetMap supplies 256-pixel PNG tiles through `https://tile.openstreetmap.org/{z}/{x}/{y}.png`. It needs visible contributor attribution, an application User-Agent, and HTTP-aware caching. Its public service forbids bulk downloads and offline archives. Access has no availability guarantee. SWAK fetches only the current view and identifies itself as `SWAK/<version>`. Read the [OSMF tile policy](https://operations.osmfoundation.org/policies/tiles/) and [attribution terms](https://www.openstreetmap.org/copyright).

NASA GIBS supplies public WMTS and WMS imagery. The WMTS preset uses `BlueMarble_ShadedRelief_Bathymetry`, style `default`, JPEG images, and matrix set `GoogleMapsCompatible_Level8`. Its zoom range is 0 through 8. The live [Web Mercator capabilities](https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/1.0.0/WMTSCapabilities.xml) confirmed these identifiers during development. Other layers can require a time dimension. Read [GIBS access requirements](https://nasa-gibs.github.io/gibs-api-docs/access-basics/) and [NASA data-use guidance](https://www.earthdata.nasa.gov/engage/open-data-services-software/data-use-policy).

The free terrestris service supplies OpenStreetMap through WMS. The preset uses `https://ows.terrestris.de/osm/service`, layer `OSM-WMS`, and PNG images. The service offers PNG and JPEG and has a paid service for higher availability. Keep OpenStreetMap and terrestris attribution visible. Read the [provider description and endpoint](https://terrestris.de/produkte/freier-osm-wms/).

OpenTopoMap also documents XYZ raster tiles. A custom XYZ entry can use its endpoint when its current access and attribution rules permit the use. It is not a built-in preset because this session could not retrieve its current policy page reliably. Read the [OpenTopoMap description](https://wiki.opentopomap.org/about) before use.

Free map data does not mean unlimited free hosting. Review each provider policy before deployment or distribution. Source operators can change endpoints and terms independently of SWAK.

# Supported formats

XYZ replaces `{z}`, `{x}`, and `{y}` in the configured HTTPS URL. Coordinates use a north-origin square grid with `2^z` rows and columns. The plugin accepts 256-pixel PNG, JPEG, and WebP images. The URL remains editable, so compatible services do not need a new application release.

WMTS uses the `GetTile` KVP request. Configure the layer, style, image format, matrix set, optional time, and optional matrix prefix. The supported matrix grid has a Web Mercator world extent, 256-pixel tiles, and `2^z` rows and columns. Matrix identifiers consist of the configured prefix followed by the zoom integer. The built-in NASA preset follows this grid.

WMS uses `GetMap`, version 1.3.0, `CRS=EPSG:3857`, and a bounding box in meters. SWAK divides the viewport into 256-pixel requests so images can use the same cache and renderer. Configure layer names, style names, and PNG or JPEG output. WMS service URLs and WMTS service URLs must not contain query parameters.

Capabilities discovery, arbitrary projections, irregular WMTS grids, and offline map archives are outside this implementation. The plugins reject unsupported images instead of interpreting them as tiles. A WMS XML error response appears as a tile failure. SWAK does not parse remote map XML or execute remote styles.

# Manage sources

Open Tools > Map Providers. The list contains registered provider presets and saved custom entries. Map tools do not appear in the provider format list. An unavailable format keeps its saved entries visible with a missing-plugin label.

1. Select a source and select Edit to change its configuration.
2. Select the provider radio button to enable online requests for that source.
3. Select Clear selection to stop requests and show the plain green background.

Select Add to open a blank provider form. Select the format in that form. Enter the source details, then select OK. Cancel discards the form. Delete removes the selected row. Selecting a row does not activate it. Changes save through the portable preferences file. An absent plugin does not erase its source configuration. Online maps start disabled in a fresh profile.

File > Settings > Map providers contains the same table, radio buttons, and Add, Edit, Delete, and Clear selection controls. Provider changes apply immediately. File > Settings > Map contains center, zoom, provider opacity, image filtering, and vector edge smoothing. Source zoom limits, layers, styles, image format, matrix set, and time also affect rendering. Attribution always remains visible while a provider is active. The text aligns to the right over a translucent background. Select the attribution link to open its license URL in the default browser. The map configuration includes attribution background opacity.

# HTTPS and cache

Certificate and hostname checks are enabled by default. Each source has an explicit Ignore invalid HTTPS certificates control. If you enable it, the map and manager show that certificate checks are disabled. This permits interception or replacement of map images. The override applies only to that selected source and is stored in its profile.

SWAK uses the existing Qt Network library for asynchronous HTTPS and Qt image readers for raster decoding. These are existing dependencies. It does not add a browser engine or a GIS runtime. Images render through QPainter on QOpenGLWidget, which uses the OpenGL paint engine for texture upload, scaling, and composition.

The renderer permits four active requests and 256 visible tiles. Raster responses have a two MiB limit. Vector responses have an eight MiB limit. Both use ten-second inactivity and fifteen-second total limits. Decoding accepts only 256-by-256 images.

The decoded-tile cache defaults to 256 MiB. File > Settings > Map exposes Tile memory cache, with limits from 64 through 512 MiB.

Failed requests pause for at least 30 seconds. Integer Retry-After values extend this delay up to one hour.

The disk cache uses Qt HTTP caching. Entries without an expiry get a seven-day default. Verified and unchecked HTTPS responses use separate caches. Each cache defaults to 128 MiB. File > Settings > Map exposes Tile disk cache, from 16 through 4096 MiB. Non-cacheable images remain in memory only while their tiles stay in the view. Cached image and vector responses survive restarts and provider changes. Fresh entries avoid another download, and expired entries require HTTP revalidation.

The cache uses the operating system cache directory, not the portable profile. On Linux, this is normally under `$XDG_CACHE_HOME/SWAK/swak/maps` or `~/.cache/SWAK/swak/maps`. A process lock prevents concurrent writers. If a cache is unavailable or locked, online rendering stops and reports the problem.

Requests reject redirects and do not send or store cookies. Zero opacity stops downloads. A provider can see the requested area and client IP address. Cache files contain unencrypted public imagery and URLs. Do not put credentials or secrets in provider URLs. Security limits remain fixed and do not appear as rendering controls.

# Development evidence

Automated tests use a local HTTPS server and a public test certificate under `tests/fixtures/`. The matching private key is only a test fixture. It is not an application credential and is not installed. Tests do not download public map tiles or pan public services.

The tests cover format parameters, bounds, missing plugins, certificate rejection, the explicit override, cache isolation, invalid responses, cancellation, and rendering. Native tests inspect the OpenGL framebuffer and provider manager. Development test results remain in the source repository under `docs/`.


# Tile recovery

A failed tile has a bounded retry delay from 30 seconds through one hour. Other visible tiles continue to load. Service rate limits, service outages, and certificate failures pause the provider. Moving the map cancels obsolete requests during that pause.

An expired image remains visible while its replacement loads. Images without usable cache metadata remain only in the current view. Hiding the map releases those images. Failure records have a limit of 256 entries.

URL-only attribution shows a visible Map attribution link. Additional URLs have separate Source links. The link parser removes trailing sentence punctuation and unmatched closing brackets.

# OpenFreeMap and vector maps

OpenFreeMap supplies free vector tiles without registration or an API key. Vector tiles contain geometry and feature properties. Its service does not supply raster maps, geocoding, routing, or elevation queries. Read the [service description](https://github.com/hyperknot/openfreemap) and [quick start](https://openfreemap.org/quick_start/).

The preset uses `https://tiles.openfreemap.org/planet/latest/{z}/{x}/{y}.pbf`. The October 2 catalog identifies zoom levels 0 through 14 and OpenMapTiles schema 3.16.0. The latest alias follows the deployed dataset. The public [TileJSON catalog](https://tiles.openfreemap.org/planet/latest) describes tile URLs, bounds, layers, and properties. SWAK uses that research for its preset. It does not import TileJSON or MapLibre style documents at runtime.

The provider supports the published [MVT format](https://github.com/mapbox/vector-tile-spec), which uses Protocol Buffers to encode binary records. The plugin accepts layer versions 1 and 2. It draws points, lines, polygons, polygon holes, and multiple geometry parts. Feature properties include text, Boolean values, and numbers. Integer properties and identifiers retain their exact decimal representation, including values above the JSON floating-point precision limit.

The built-in style follows the [OpenMapTiles layer schema](https://openmaptiles.org/docs/schema/). It draws land, water, roads, buildings, boundaries, places, and POIs. A POI is a point of interest. Examples include shops, cafes, and stations. The tile source determines which features appear at each zoom. Unknown layers remain available with a generic geometry style.

Select OpenFreeMap in Tools > Map Providers or the main toolbar. Online access stays disabled until a provider is selected. Existing profiles receive the new preset once. Removing the preset keeps it removed after restart. An unavailable plugin preserves its saved configuration.

Right-click the map, then open Map layers to show or hide individual layers. The keyboard Menu key opens the same controls at the map center. Right-click a feature and select Map information to inspect its layer, identifier, and properties. The information window shows up to twelve matching features. It treats all properties as plain text and does not follow embedded URLs.

File > Settings > Map providers > Edit contains layer visibility, labels, label size, preferred language, line width, and point radius. The provider editor exposes the same controls. An empty language code uses local names. A language such as `en` or `sv` selects the corresponding name when available. The context menu lists known layers and additional layers from loaded tiles. Layer choices and the information window geometry survive restarts.

The native renderer uses a fixed color palette. It does not implement MapLibre expressions, sprites, remote fonts, terrain, building extrusion, route calculation, or a POI search service. It uses the system font and simple point markers. Label placement suppresses overlap within each tile and permits up to 200 labels per tile. Labels remain clipped at tile edges. These maps do not reproduce the Liberty, Bright, or other web styles.

Keep the OpenFreeMap, OpenMapTiles, and OpenStreetMap credits visible. The preset includes links to all three sources. OpenStreetMap data uses the [Open Database License](https://www.openstreetmap.org/copyright). The OpenMapTiles schema uses BSD and CC-BY terms described in its [license](https://github.com/openmaptiles/openmaptiles/blob/master/LICENSE.md). Service software licenses do not replace data attribution requirements.

# Vector input limits

Vector requests use the same HTTPS, cache, timeout, and retry controls as image tiles. Responses must contain raw MVT bytes. SWAK requests identity encoding and does not open gzip archives as tiles. Downloads and pending decode tasks share a limit of four. Two background workers decode and paint tiles. Source changes, hiding, and shutdown cancel obsolete work. The renderer discards results from earlier source generations.

A vector tile has limits of eight MiB, 64 layers, 50000 features, and 500000 vertices. Each feature accepts up to 256 property pairs. Each layer dictionary accepts up to 65536 keys and values. The parser also bounds text, geometry commands, coordinate ranges, and retained properties. Retained properties have a 16 MiB limit. The estimated geometry and property total has a 64 MiB limit per tile. Invalid tiles enter the normal retry path.

The cache accounts for decoded geometry, properties, and prepared images within the configured memory budget. It first removes tiles outside the view. If visible geometry still exceeds the budget, SWAK retains 256-pixel images and releases the geometry. This keeps all visible tiles drawable, including at the provider minimum zoom. Object inspection is unavailable for these compact tiles. KML data and CoT inspection remain available.

The map reports this reduced inspection mode. Increasing Tile memory cache or changing the provider restores normal decoding. Compact mode disables extra tile fetching. At most 256 visible images require 64 MiB, which fits the lowest supported cache budget.

A parser resource limit requests the next lower tile level, down to the provider minimum. The map center and selected zoom stay unchanged. Panning keeps this detail level. Changing zoom or provider permits full-detail requests again. Invalid content, network failures, and failures at the minimum level remain explicit errors with bounded retries.

The renderer counts visible tiles and cached images after each view change. Missing coverage appears as a rendered/total count. A two-second audit schedules unfinished work while respecting existing retry delays and request limits. Coverage measures available images, including stale cached images. It does not establish data freshness. A failed service or invalid minimum-level tile can still leave missing coverage.

Workers paint complete tiles into 512-pixel images for 256 logical pixels. The OpenGL map view reuses these images during panning. The parser limits still bound geometry work per tile. There is no shared frame limit that discards later tiles. Higher display scaling and reduced-detail fallback can soften the prepared images. Labels and line widths scale with the tile during fallback. Feature inspection follows visible layer choices. MVT parsing does not fetch linked resources or execute styles. Development tests cover truncated records, malformed commands, oversized numbers, property types, holes, cancellation, replacement, and worker exceptions.

Live research accepted a world tile with 79 features and a Stockholm tile with 4142 features. The Stockholm sample used tile `14/9014/4818` and contained POIs. These counts describe the sampled dataset, not a fixed service guarantee. Automated tests use local synthetic tiles, so they do not depend on public service availability.

The map shows disk usage below the attribution. The value covers the active certificate-policy cache across its providers. The host refreshes this value every two seconds, outside map painting. The memory-cache limit remains separate. With no open provider cache, the label shows inactive. Cache files are expendable, not an offline archive or a portable map package. SWAK does not bulk-download tiles or ignore HTTP cache restrictions.

# Additional image addressing

The XYZ image plugin also accepts TMS and quadkey addresses. All three use the existing bounded image decoder, cache, HTTPS client, and GPU map renderer. They require Web Mercator tiles. Read [the ATAK reference review](atak-reference-review.md) for source information and scope.

For a south-origin TMS source, enable Invert tile rows (TMS) in the provider editor. The row becomes `2^z - 1 - y`. For a quadkey source, use `{q}` instead of the three coordinate placeholders. Mixed or unknown placeholders are rejected. SWAK accepts integer tile zoom levels from zero through 19. Each source can set a smaller range.

The USGS Topo preset uses `https://basemap.nationalmap.gov/arcgis/rest/services/USGSTopo/MapServer/tile/{z}/{y}/{x}`. This swaps row and column positions without inverting rows. The preset uses zoom levels zero through 8 and links the published service credits. It remains inactive until the user selects it and enables online maps. A deleted preset stays deleted after restart.
