# Map providers

Research date: October 2, 2026. A provider supplies map images or vector geometry. MVT is the Mapbox Vector Tile format. Since October 5, 2026, the GDAL raster provider renders all raster maps. GDAL is the Geospatial Data Abstraction Library. It supports XYZ templates, WMTS 1.0 KVP, WMS 1.3, local raster files, and other GDAL datasets. The MVT provider supplies vector tiles. The map draws everything in Web Mercator, also called EPSG:3857.

# Sources and requirements

OpenStreetMap supplies 256-pixel PNG tiles through `https://tile.openstreetmap.org/{z}/{x}/{y}.png`. It needs visible contributor attribution, an application User-Agent, and HTTP-aware caching. Its public service forbids bulk downloads and offline archives. Access has no availability guarantee. SWAK fetches only the current view and identifies itself as `SWAK/<version>`. Read the [OSMF tile policy](https://operations.osmfoundation.org/policies/tiles/) and [attribution terms](https://www.openstreetmap.org/copyright).

NASA GIBS supplies public WMTS and WMS imagery. The WMTS preset uses `BlueMarble_ShadedRelief_Bathymetry`, style `default`, JPEG images, and matrix set `GoogleMapsCompatible_Level8`. Its zoom range is 0 through 8. The live [Web Mercator capabilities](https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/1.0.0/WMTSCapabilities.xml) confirmed these identifiers during development. Other layers can require a time dimension. Read [GIBS access requirements](https://nasa-gibs.github.io/gibs-api-docs/access-basics/) and [NASA data-use guidance](https://www.earthdata.nasa.gov/engage/open-data-services-software/data-use-policy).

Three presets suit the Modern display mode. The God's Eye View project, which inspired that mode, uses the same keyless imagery services.

- Esri World Imagery uses the XYZ template `https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}`. The row comes before the column. The service needs no key, and its zoom range is 0 through 19. Esri governs access through the [Esri Master Agreement](https://www.esri.com/en-us/legal/terms/full-master-agreement) and can change it. Keep the credit line "Powered by Esri | Source: Esri, Maxar, Earthstar Geographics, and the GIS User Community" visible. Review the current ArcGIS Location Platform terms before use at scale.
- NASA GIBS VIIRS True Color uses the WMTS layer `VIIRS_SNPP_CorrectedReflectance_TrueColor`, matrix set `GoogleMapsCompatible_Level9`, and JPEG images. It has no time, so GIBS serves its default day, which is the newest one. That day can still have gaps between satellite passes.
- NASA GIBS Black Marble uses the WMTS layer `VIIRS_Black_Marble`, matrix set `GoogleMapsCompatible_Level8`, PNG images, and the time 2016-01-01. It shows city lights at night.

The live Web Mercator capabilities confirmed these identifiers on October 11, 2026. The 3D sources of God's Eye View, which are Google photorealistic tiles, Cesium ion, and Re:Earth terrain, need a 3D globe. The SWAK map is flat, so it does not use them.

The free terrestris service supplies OpenStreetMap through WMS. The preset uses `https://ows.terrestris.de/osm/service`, layer `OSM-WMS`, and PNG images. The service offers PNG and JPEG and has a paid service for higher availability. Keep OpenStreetMap and terrestris attribution visible. Read the [provider description and endpoint](https://terrestris.de/produkte/freier-osm-wms/).

OpenTopoMap also documents XYZ raster tiles. A custom XYZ entry can use its endpoint when its current access and attribution rules permit the use. It is not a built-in preset because this session could not retrieve its current policy page reliably. Read the [OpenTopoMap description](https://wiki.opentopomap.org/about) before use.

Free map data does not mean unlimited free hosting. Review each provider policy before deployment or distribution. Source operators can change endpoints and terms independently of SWAK.

# Google Maps and Azure Maps

Research date: October 6, 2026. Two provider plugins add commercial services. Each one needs your own API key and a billing account with the service. SWAK adds no key. Read the service terms before you select a source. Usage counts against your account.

The Google Map Tiles plugin uses the [Map Tiles API](https://developers.google.com/maps/documentation/tile/2d-tiles-overview). It adds three presets: Google Maps Roadmap, Satellite, and Terrain. Each one is off until you select it. Supply the key through Credentials or the `SWAK_MAP_GOOGLE_KEY` environment variable. The key travels as the `key` query parameter.

Google needs a session before it sends tiles. SWAK sends a POST to `https://tile.googleapis.com/v1/createSession` with the map type, language, and region. The language defaults to `en-US` and the region defaults to `US`. Terrain also requests the roadmap layer, as Google requires. Every tile request then carries the session token. SWAK renews the session after one day, before it expires, or after Google rejects it. A rejected session uses the provider-wide retry delay. Read [session tokens](https://developers.google.com/maps/documentation/tile/session_tokens).

Google requires its attribution on the map. The source attribution is Google Maps. SWAK shows this text, not the Google logo image. After the view rests for 0.75 seconds, SWAK asks the viewport service for the data credits of the visible area. The map attribution panel shows them on the next line. The same view does not ask again. The [Map Tiles API policies](https://developers.google.com/maps/documentation/tile/policies) prohibit caching except as the HTTP cache headers permit. Authenticated sources, including Google, use no disk cache and fetch only visible tiles.

The Azure Maps plugin uses [Get Map Tile](https://learn.microsoft.com/en-us/rest/api/maps/render/get-map-tile) with API version 2024-04-01. Azure Maps replaces Bing Maps. Microsoft ended free and basic Bing Maps keys on June 30, 2025, and retires Bing Maps for Enterprise on June 30, 2028. SWAK adds no Bing source. The presets are Azure Maps Road, Dark Grey, and Imagery. Imagery starts at zoom level 1. Supply the key through Credentials or `SWAK_MAP_AZURE_KEY`. The key travels as the `subscription-key` query parameter.

Azure Maps also requires the attribution of the visible area. SWAK asks [Get Map Attribution](https://learn.microsoft.com/en-us/rest/api/maps/render/get-map-attribution) after the view rests. The source attribution is Microsoft Azure Maps. The reported credits follow on the next line.

Both plugins fix the service address and key parameter. You can change the name, map type or style, language, Google region, zoom range, and environment variable. Session and attribution replies have a limit of 64 KiB. SWAK converts attribution HTML to one line of plain text with at most 300 characters. A failed attribution request keeps the last text.

# Supported formats

The GDAL raster provider has four source types. Select the type in the Source type field. Saved XYZ, WMS, and WMTS entries from earlier versions convert to the matching type when SWAK loads the profile.

XYZ replaces `{z}`, `{x}`, and `{y}` in the configured HTTPS URL. Coordinates use a north-origin square grid with `2^z` rows and columns. GDAL decodes the tile images, which include PNG, JPEG, and WebP. The URL remains editable, so compatible services do not need a new application release.

WMTS uses the `GetTile` KVP request. Configure the layer, style, image format, matrix set, optional time, and optional matrix prefix. The supported matrix grid has a Web Mercator world extent, 256-pixel tiles, and `2^z` rows and columns. Matrix identifiers consist of the configured prefix followed by the zoom integer. The built-in NASA preset follows this grid.

WMS uses `GetMap`, version 1.3.0, `CRS=EPSG:3857`, and a bounding box in meters. GDAL requests one 256-pixel image for each map tile. Configure layer names, style names, and PNG or JPEG output. WMS service URLs and WMTS service URLs must not contain query parameters. SWAK encodes reserved characters in layer, style, and time values.

For these three service types, SWAK reads exactly one remote tile for each map tile. GDAL does not fetch neighboring tiles.

The GDAL dataset type opens any raster that GDAL reads. Enter one of these locations:

- An absolute local file path, such as `/data/area.tif`.
- `/vsicurl/` followed by an HTTPS URL, for a cloud-optimized GeoTIFF or another remote file.
- `WMTS:` followed by an HTTPS capabilities URL and GDAL WMTS options.
- `WMS:` followed by an HTTPS service URL and GDAL WMS options.

Remote datasets do not use credentials. GDAL would send them to every host that a capabilities document names. Use the WMS, WMTS, or tile service types for a service that needs credentials.

Local files can use GeoTIFF and COG, MBTiles, GeoPackage, PMTiles, JPEG 2000, PNG, JPEG, WebP, GIF, BMP, DTED and SRTM elevation, NITF, CADRG and CIB through RPF table-of-contents files, ADRG, Erdas Imagine, ENVI, NetCDF, HDF5, Arc/Info ASCII grids, USGS DEM, BSB nautical charts, Esri compact caches, MRF, Zarr, PDF, GRIB, and VRT. Remote files use a smaller list without VRT, because a VRT can read other local files.

GDAL warps each dataset tile from its own coordinate reference system to Web Mercator. Palette images keep their colors. Elevation and other data that is not 8-bit appears as gray between the dataset minimum and maximum. Pixels outside the dataset are transparent. Large files without overviews render slowly at low zoom. Add overviews with `gdaladdo` for faster display.

Capabilities discovery and offline map archives are outside this implementation. SWAK does not execute remote styles. A WMS XML error response appears as a tile failure.

# Manage sources

Open Tools > Map Providers. The list contains registered provider presets and saved custom entries. Map tools do not appear in the provider format list. An unavailable format keeps its saved entries visible with a missing-plugin label.

1. Select a source and select Edit to change its configuration.
2. Select the provider radio button to enable online requests for that source.
3. Select Clear selection to stop requests and show the plain green background.

Select Add to open a blank provider form. Select the format in that form. Enter the source details. Then select OK. Cancel discards the form.

Delete removes the selected row. Selecting a row does not activate it. Changes save through the portable preferences file. An absent plugin does not erase its source configuration. Online maps start disabled in a fresh profile.

File > Settings > Map providers contains the same table, radio buttons, and Add, Edit, Delete, and Clear selection controls. Provider changes apply immediately. File > Settings > Map contains center, zoom, provider opacity, image filtering, and vector edge smoothing. Source zoom limits, layers, styles, image format, matrix set, and time also affect rendering.

Attribution always remains visible while a provider is active. The text aligns to the right over a translucent background. Select the attribution link to open its license URL in the default browser. The map configuration includes attribution background opacity.

# HTTPS and cache

SWAK enables certificate and hostname checks by default. Each source has an explicit Ignore invalid HTTPS certificates control. If you enable it, the map and manager show that certificate checks are disabled. This permits interception or replacement of map images. The override applies only to that selected source and is stored in its profile.

GDAL fetches raster tiles through its libcurl client. Qt Network fetches MVT tiles. Images render through QPainter on QOpenGLWidget, which uses the OpenGL paint engine for texture upload, scaling, and composition.

The renderer permits four active jobs and 256 visible tiles. Raster requests have a ten-second connect limit and a ten-second request limit in GDAL. MVT responses have an eight MiB limit, ten-second inactivity, and fifteen-second total limits.

The decoded-tile cache defaults to 256 MiB. File > Settings > Map exposes Tile memory cache, with limits from 64 through 512 MiB.

GDAL keeps a raster disk cache under `maps/gdal` in the operating system cache directory. On Linux, this is normally `~/.cache/SWAK/swak/maps/gdal`. Sources with certificate checks use its `verified` folder, and sources with the certificate exception use its `unchecked` folder. `WMS:` and `WMTS:` datasets cache in a `datasets` folder below these. Sources with credentials use no disk cache. Entries expire after seven days. File > Settings > Map exposes Tile disk cache, from 16 through 4096 MiB. GDAL removes old entries above that size. MVT tiles use Qt HTTP caching in the `verified` and `unchecked` directories, with a process lock.

The raster changes have these known gaps compared with the earlier Qt client:

- GDAL follows HTTP redirects. A redirect can send a custom API key header to another server.
- SWAK does not set its own response size limit for raster tiles. GDAL decodes the fixed 256-pixel tiles.
- GDAL ignores `Retry-After`. Failed tiles use the fixed 30-second retry delay.
- The GDAL disk cache has no lock between windows.

Requests do not store cookies. Zero opacity stops downloads. A provider can see the requested area and client IP address. Cache files contain unencrypted public imagery. Do not put credentials or secrets in provider URLs. Security limits remain fixed and do not appear as rendering controls.

# Development evidence

Automated tests use a local HTTPS server and a public test certificate under `tests/fixtures/`. The matching private key is only a test fixture. It is not an application credential. Installation does not include it. Tests do not download public map tiles or pan public services.

The tests cover format parameters, bounds, missing plugins, certificate rejection, the explicit override, cache isolation, invalid responses, cancellation, and rendering. GDAL raster tests render generated GeoTIFF, palette, and elevation files. They also fetch XYZ, inverted, quadkey, WMS, and WMTS tiles from the local HTTPS server and check the request paths and credentials. Native tests inspect the OpenGL framebuffer and provider manager. Development test results remain in the source repository under `docs/`.


# Tile recovery

A failed tile has a bounded retry delay from 30 seconds through one hour. Other visible tiles continue to load. For MVT, service rate limits, service outages, and certificate failures pause the provider. For GDAL raster sources, a dataset that does not open pauses the provider. Moving the map cancels obsolete requests during that pause.

An expired image remains visible while its replacement loads. Images without usable cache metadata remain only in the current view. Hiding the map releases those images. Failure records have a limit of 256 entries.

URL-only attribution shows a visible Map attribution link. Additional URLs have separate Source links. The link parser removes trailing sentence punctuation and unmatched closing brackets.

# OpenFreeMap and vector maps

OpenFreeMap supplies free vector tiles without registration or an API key. Vector tiles contain geometry and feature properties. Its service does not supply raster maps, geocoding, routing, or elevation queries. Read the [service description](https://github.com/hyperknot/openfreemap) and [quick start](https://openfreemap.org/quick_start/).

The preset uses `https://tiles.openfreemap.org/planet/latest/{z}/{x}/{y}.pbf`. The October 2 catalog identifies zoom levels 0 through 14 and OpenMapTiles schema 3.16.0. The latest alias follows the deployed dataset. The public [TileJSON catalog](https://tiles.openfreemap.org/planet/latest) describes tile URLs, bounds, layers, and properties. SWAK uses that research for its preset. It does not import TileJSON or MapLibre style documents at runtime.

The provider supports the published [MVT format](https://github.com/mapbox/vector-tile-spec), which uses Protocol Buffers to encode binary records. GDAL decodes each tile from memory. GDAL drops features with truncated geometry and layers with an unsupported version, and draws the rest of the tile. It draws points, lines, polygons, polygon holes, and multiple geometry parts. Feature properties include text, Boolean values, and numbers. Integer properties and identifiers retain their exact decimal representation, including values above the JSON floating-point precision limit.

The built-in style follows the [OpenMapTiles layer schema](https://openmaptiles.org/docs/schema/). It draws land, water, roads, buildings, boundaries, places, and POIs. A POI is a point of interest. Examples include shops, cafes, and stations. The tile source determines which features appear at each zoom. Unknown layers remain available with a generic geometry style.

Select OpenFreeMap in Tools > Map Providers or the main toolbar. Online access stays disabled until you select a provider. Existing profiles receive the new preset once. Removing the preset keeps it removed after restart. An unavailable plugin preserves its saved configuration.

To show or hide individual layers, right-click the map. Then open Map layers. The keyboard Menu key opens the same controls at the map center. To inspect the layer, identifier, and properties of a feature, right-click the feature. Then select Map information. The information window shows up to twelve matching features. It treats all properties as plain text and does not follow embedded URLs.

File > Settings > Map providers > Edit contains layer visibility, labels, label size, preferred language, line width, and point radius. The provider editor exposes the same controls. An empty language code uses local names. A language such as `en` or `sv` selects the corresponding name when available. The context menu lists known layers and additional layers from loaded tiles. Layer choices and the information window geometry survive restarts.

The native renderer uses a fixed color palette. It does not implement MapLibre expressions, sprites, remote fonts, terrain, building extrusion, route calculation, or a POI search service. It uses the system font and simple point markers. Label placement suppresses overlap within each tile and permits up to 200 labels per tile. Labels remain clipped at tile edges. These maps do not reproduce the Liberty, Bright, or other web styles.

Keep the OpenFreeMap, OpenMapTiles, and OpenStreetMap credits visible. The preset includes links to all three sources. OpenStreetMap data uses the [Open Database License](https://www.openstreetmap.org/copyright). The OpenMapTiles schema uses BSD and CC-BY terms described in its [license](https://github.com/openmaptiles/openmaptiles/blob/master/LICENSE.md). Service software licenses do not replace data attribution requirements.

# Vector input limits

Vector requests use the same HTTPS, cache, timeout, and retry controls as image tiles. Responses must contain raw MVT bytes. SWAK requests identity encoding and does not open gzip archives as tiles. Downloads and pending decode tasks share a limit of four.

Two background workers decode and paint tiles. Source changes, hiding, and shutdown cancel obsolete work. The renderer discards results from earlier source generations.

A vector tile has limits of eight MiB, 64 layers, 50000 features, and 500000 vertices. Each feature accepts up to 256 property pairs. Each layer dictionary accepts up to 65536 keys and values.

The parser also bounds text, geometry commands, coordinate ranges, and retained properties. Retained properties have a 16 MiB limit. The estimated geometry and property total has a 64 MiB limit per tile. Invalid tiles enter the normal retry path.

The cache accounts for decoded geometry, properties, and prepared images within the configured memory budget. It first removes tiles outside the view. If visible geometry still exceeds the budget, SWAK retains 256-pixel images and releases the geometry. This keeps all visible tiles drawable, including at the provider minimum zoom. Object inspection is unavailable for these compact tiles. KML data and CoT inspection remain available.

The map reports this reduced inspection mode. Increasing Tile memory cache or changing the provider restores normal decoding. Compact mode disables extra tile fetching. At most 256 visible images require 64 MiB, which fits the lowest supported cache budget.

A parser resource limit requests the next lower tile level, down to the provider minimum. The map center and selected zoom stay unchanged. Panning keeps this detail level. Changing zoom or provider permits full-detail requests again. Invalid content, network failures, and failures at the minimum level remain explicit errors with bounded retries.

The renderer counts visible tiles and cached images after each view change. Missing coverage appears as a rendered/total count. A two-second audit schedules unfinished work while respecting existing retry delays and request limits. Coverage measures available images, including stale cached images. It does not establish data freshness. A failed service or invalid minimum-level tile can still leave missing coverage.

Workers paint complete tiles into 512-pixel images for 256 logical pixels. The OpenGL map view reuses these images during panning. The parser limits still bound geometry work per tile. There is no shared frame limit that discards later tiles.

Higher display scaling and reduced-detail fallback can soften the prepared images. Labels and line widths scale with the tile during fallback. Feature inspection follows visible layer choices.

MVT parsing does not fetch linked resources or execute styles. SWAK limits layers to 64, features to 50000, and vertices to 500000 in each tile. Development tests cover truncated records, malformed commands, oversized numbers, property types, holes, cancellation, replacement, and worker exceptions.

Live research accepted a world tile with 79 features and a Stockholm tile with 4142 features. The Stockholm sample used tile `14/9014/4818` and contained POIs. These counts describe the sampled dataset, not a fixed service guarantee. Automated tests use local synthetic tiles, so they do not depend on public service availability.

The map shows disk usage below the attribution. The value covers the active certificate-policy cache across its providers. The host refreshes this value every two seconds, outside map painting. The memory-cache limit remains separate. With no open provider cache, the label shows inactive.

Cache files are expendable, not an offline archive or a portable map package. SWAK does not bulk-download tiles or ignore HTTP cache restrictions.

# Additional image addressing

The XYZ source type also accepts TMS and quadkey addresses. GDAL fetches and decodes all three. They require Web Mercator tiles. Read [the ATAK reference review](atak-reference-review.md) for source information and scope.

For a south-origin TMS source, enable Invert tile rows (TMS) in the provider editor. The row becomes `2^z - 1 - y`. For a quadkey source, use `{q}` instead of the three coordinate placeholders. SWAK rejects mixed or unknown placeholders. SWAK accepts integer tile zoom levels from zero through 19. Each source can set a smaller range.

The USGS Topo preset uses `https://basemap.nationalmap.gov/arcgis/rest/services/USGSTopo/MapServer/tile/{z}/{y}/{x}`. This swaps row and column positions without inverting rows. The preset uses zoom levels zero through 8 and links the published service credits. It remains inactive until the user selects it and enables online maps. A deleted preset stays deleted after restart.

# Credentials

Edit a provider to select Authentication. The common controls support None, Basic username and password, Bearer token, API key header, and API key query. Basic uses the Authentication username field. API keys use the named parameter. Custom headers must start with `X-`, such as `X-API-Key`.

Select the provider row. Then select Credentials. Enter its password or token in the masked field. The value stays in memory for this session. Cancel preserves the previous value. An empty accepted value clears the session secret. This then permits a configured environment variable to supply the secret.

For restart use, set Secret environment variable to a name such as `SWAK_MAP_PRIVATE_TOKEN`. Supply that variable through your trusted launch environment. Names must start with `SWAK_MAP_` and contain uppercase letters, digits, or underscores. The profile saves the variable name, not its value. Do not enter secrets in the HTTPS URL or other ordinary fields. These fields appear in profiles and tooltips.

The application does not store passwords on disk or provide a password vault. Environment values remain available to the process and its launch environment. Session values remain in process memory. Qt containers do not guarantee secure erasure of all copies. Credentials permit printable ASCII values up to 4096 bytes. Non-ASCII Basic passwords require server-specific encoding support that this version does not provide.

Authenticated requests require valid HTTPS certificates. GDAL receives the credential through per-thread HTTP options, not through the dataset description. Query keys appear in the request URL. SWAK replaces URL query values in GDAL messages before it logs or shows them. Authentication failures use the shared retry delay. Authenticated tiles bypass the disk cache and fetch only visible tiles. Read the redirect gap in HTTPS and cache. Secret changes cancel requests and clear decoded images. Endpoint and authentication changes clear the session secret.

The Google presets use the `key` query parameter and `SWAK_MAP_GOOGLE_KEY`. The Azure Maps presets use `subscription-key` and `SWAK_MAP_AZURE_KEY`. Read Google Maps and Azure Maps above.

The Thunderforest templates use API key query authentication with parameter `apikey`. Set the session credential or supply `SWAK_MAP_THUNDERFOREST_KEY` before launch. Read the service terms and account limits before you select the source. No template activates itself. Credentials do not grant access beyond the service account permissions.
