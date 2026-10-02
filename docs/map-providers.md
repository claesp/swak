# Map providers

Research date: October 2, 2026. A provider supplies map images. A format plugin converts the selected source configuration into tile requests. This implementation supports XYZ, WMTS 1.0 KVP, and WMS 1.3 images in Web Mercator, also called EPSG:3857.

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

Capabilities discovery, arbitrary projections, irregular WMTS grids, vector tiles, and offline map archives are outside this implementation. The plugins reject unsupported images instead of interpreting them as tiles. A WMS XML error response appears as a tile failure. SWAK does not parse remote map XML or execute remote styles.

# Manage sources

Open Tools > Map Providers. The list contains registered provider presets and saved custom entries. Map tools do not appear in the provider format list. An unavailable format keeps its saved entries visible with a missing-plugin label.

1. Select a source and select Edit to change its configuration.
2. Select Use provider to enable online requests for that source.
3. Select Use offline map to stop requests and show the schematic reference map.

Select Add to open a blank provider form. Select the format in that form. Enter the source details, then select OK. Cancel discards the form. Remove deletes the selected source configuration. Changes save through the portable preferences file. An absent plugin does not erase its source configuration. Online maps start disabled in a fresh profile.

File > Settings > Map providers contains the same source parameters and the online-map switch. File > Settings > Map contains center, zoom, provider opacity, image filtering, and vector edge smoothing. Source zoom limits, layers, styles, image format, matrix set, and time also affect rendering. Attribution always remains visible while a provider is active. The text aligns to the right over a translucent background. Select the attribution link to open its license URL in the default browser. The map configuration includes attribution background opacity.

# HTTPS and cache

Certificate and hostname checks are enabled by default. Each source has an explicit Ignore invalid HTTPS certificates control. If you enable it, the map and manager show that certificate checks are disabled. This permits interception or replacement of map images. The override applies only to that selected source and is stored in its profile.

SWAK uses the existing Qt Network library for asynchronous HTTPS and Qt image readers for raster decoding. These are existing dependencies. It does not add a browser engine or a GIS runtime. Images render through QPainter on QOpenGLWidget, which uses the OpenGL paint engine for texture upload, scaling, and composition.

The renderer permits four active requests and 256 visible tiles. Each response has a two MiB limit, a ten-second inactivity limit, and a fifteen-second total limit. Decoding accepts only 256-by-256 images. The decoded-image cache holds at most 64 MiB. Failed requests pause for at least 30 seconds. Integer Retry-After values extend this delay up to one hour.

The disk cache uses Qt HTTP caching. Entries without an expiry get a seven-day default. Verified and unchecked HTTPS responses use separate caches, each limited to 128 MiB. Non-cacheable images remain in memory only while their tiles stay in the view.

The cache uses the operating system cache directory, not the portable profile. A process lock prevents concurrent writers. If a cache is unavailable or locked, online rendering stops and reports the problem.

Requests reject redirects and do not send or store cookies. Zero opacity stops downloads. A provider can see the requested area and client IP address. Cache files contain unencrypted public imagery and URLs. Do not put credentials or secrets in provider URLs. Security limits remain fixed and do not appear as rendering controls.

# Development evidence

Automated tests use a local HTTPS server and a public test certificate under `tests/fixtures/`. The matching private key is only a test fixture. It is not an application credential and is not installed. Tests do not download public map tiles or pan public services.

The tests cover format parameters, bounds, missing plugins, certificate rejection, the explicit override, cache isolation, invalid responses, cancellation, and rendering. Native tests inspect the OpenGL framebuffer and provider manager. Read the dated results in [HANDOFF.md](HANDOFF.md) for the current test outcome and remaining limits.
