# Data providers

Research date: October 2, 2026. A data feed is a set of vector features from a URL or a local file. GDAL reads the features. GDAL is the Geospatial Data Abstraction Library. KML is an XML format for geographic features. KMZ is a ZIP archive that contains a KML document. Data feeds draw above map imagery and below CoT events. They do not create or transmit CoT events.

# Open sources

These sources provide public downloads without an account or API key. Availability and content can change. The two included presets start disabled.

| Source | Endpoint or catalog | Content and access terms | Refresh choice |
| --- | --- | --- | --- |
| USGS earthquakes | [Direct M2.5+ past-day KML](https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/2.5_day_age.kml) | Earthquake point locations. USGS-produced data are in the U.S. public domain. Credit USGS and respect any marked third-party rights. | Preset: 300 seconds. The sampled document requests at least 60 seconds. |
| NOAA NDBC observations | [Direct marine observations KML](https://www.ndbc.noaa.gov/kml/marineobs_as_kml.php) | Station and ship locations with observations. NOAA/NWS content is public domain unless marked otherwise. Credit NOAA, NDBC, and the contributing station operators. | Preset: 300 seconds. This is an application default, not a publisher guarantee. |
| NOAA National Hurricane Center | [Active KML/KMZ catalog](https://www.nhc.noaa.gov/gis/activekml.php) | Current hurricane products. The active KMZ is a network-link catalog, not a direct geometry feed. NOAA/NWS reuse terms apply. | Sampled catalog: minimum 120 seconds. Individual products follow their publication schedules. |

The USGS and NDBC endpoints returned direct KML during research. Their sampled responses contained 33878 and 1842993 bytes respectively. The NHC endpoint returned a 1806-byte KMZ with network links and a screen overlay. These live samples are temporary research evidence, not committed fixtures or availability promises.

Use direct geometry URLs from the NHC catalog for custom feeds. The active catalog itself does not draw hurricane geometry in this version. SWAK reports skipped network links. It does not follow them or load their linked resources.

Source documentation: [USGS feed instructions](https://earthquake.usgs.gov/earthquakes/feed/v1.0/kml.php), [USGS reuse terms](https://www.usgs.gov/information-policies-and-instructions/copyrights-and-credits), [NDBC observations](https://www.ndbc.noaa.gov/observations.shtml), and [NOAA/NWS reuse terms](https://www.weather.gov/disclaimer).

# Manage feeds

Open Tools > Data Providers. Select Add to create a blank feed. Enter its name, location, attribution, and refresh interval. The location is a direct HTTPS URL or an absolute local file path, such as `/home/user/sites.shp` or `file:///home/user/sites.gpkg`. Enable the feed when it is ready. Add and Edit use the same form system as Map Providers.

The table checkbox enables or disables each feed. Multiple feeds can draw together. Edit changes the selected entry. Delete removes the selected row and its geometry. Selecting a row does not enable it. Refresh now updates an enabled feed without waiting for its timer.

The refresh interval ranges from 60 through 86400 seconds. Each feed has its own timer. Its next interval starts after a download and parse attempt finishes. A local file feed reads the file again at each interval. Manual refresh does not overlap an update already in progress. Disabled feeds do not download.

File > Settings > Data providers contains the same table as the management window. Add and Edit expose all feed parameters. Provider changes apply immediately. The form includes color, opacity, line width, point radius, polygon fill opacity, and labels.

Names sort according to the desktop locale. The application saves feed configuration and manager geometry across restarts. Missing plugins retain their configuration but do not download data.

A failed update retains the previous valid geometry and marks its credit as STALE. The manager shows the failure. A successful empty document clears the old geometry. Disabling, removing, or editing a feed clears its current geometry. Geometry remains in memory, and the application downloads it again after restart. The map combines data credits with its clickable, right-aligned attribution panel.

# Supported formats

The GDAL vector plugin reads these formats from HTTPS and local files: KML and KMZ, GeoJSON and GeoJSON sequences, Esri JSON, TopoJSON, JSON-FG, GPX, GML, GeoRSS, GeoPackage, SQLite, FlatGeobuf, OpenFileGDB, MapInfo, Esri shapefiles, CSV, S-57 charts, OpenStreetMap, DXF, DGN, JML, GTFS, PMTiles, MVT, ODS, and XLSX. A ZIP archive can hold one dataset, such as a shapefile with its sibling files. Local shapefiles and file geodatabase directories open in place.

GDAL converts each layer from its coordinate reference system to WGS 84 longitude and latitude. A layer without a reference system uses longitude and latitude. The renderer projects the coordinates to Web Mercator and clips polar positions.

The map draws points, lines, polygons with holes, and multipart geometry. GDAL converts curves to straight segments. Feature fields named `name` and `description` supply the label and the description. Other fields become read-only properties. The map draws flat geometry. It does not apply altitude, terrain, time animation, styles, HTML balloons, external icons, models, tours, or image overlays. Each feed uses its configured appearance.

KML content has these GDAL behaviors:

- A placemark with visibility 0 stays hidden. GDAL does not report folder visibility, so placemarks in a hidden folder appear.
- Network links do not trigger downloads. The manager reports counts of skipped links and unsupported overlays or models.
- A placemark without coordinates, or with coordinates that are not numbers, has no map shape. It does not stop the update.
- A Point with more than one coordinate uses the first coordinate.
- An unknown KML namespace does not stop the update.

A document with only unsupported content can show no shapes.

Format references: [GDAL vector drivers](https://gdal.org/drivers/vector/index.html), [KML reference](https://developers.google.com/kml/documentation/kmlreference), and [KMZ archive structure](https://developers.google.com/kml/documentation/kmzarchives).

# Bounds and recovery

Remote feeds require HTTPS and use normal certificate and hostname checks. A per-feed override starts disabled and appears in the manager status. Each feed owns a separate network manager. The application disables redirects and cookie handling. Requests have a 20-second transfer timeout and a 30-second total deadline. No remote resource can select a local file. Only the user can enter a local path.

The application limits stored feeds to eight and active work to two downloads or parsers. Parsing uses a private thread pool outside the interface thread. Removal and shutdown cancel work. A replaced feed cannot receive an old parse result. Updates replace complete documents instead of appending duplicate features.

The input bounds are:

- Download bytes: 8 MiB per response.
- Archive entries: 256, with 16 MiB for each entry and 32 MiB of declared expanded content.
- Layers: 64 for each dataset.
- Features read: 100000 for each dataset.
- Document geometry: 10000 shapes and 100000 coordinate tuples.
- Nested geometry: 8 levels.
- Visible text: 100 feature labels across all feeds.

GDAL reads ZIP archives in memory and never extracts files to disk. SWAK lists the archive first. It rejects absolute paths, parent traversal, oversized entries, and archives with more than one dataset. SWAK rejects XML document type declarations before GDAL reads KML, GPX, GML, or GeoRSS. Coordinates outside the valid longitude and latitude ranges fail the whole update. Excessive structure also fails the whole update. These bounds do not replace dependency security updates.

# Dependencies

Qt Network supplies HTTPS. GDAL decodes the formats and reads ZIP archives. Qt Concurrent supplies bounded background work. Read `docs/dependencies.md` for the GDAL review and the accepted in-process risk.

Before October 5, 2026, a custom QXmlStreamReader parser read KML, and libarchive read KMZ archives. GDAL replaced both for data feeds. Marker collections still use libarchive.

# Object information

Feature descriptions and fields appear in the Information pane. KML ExtendedData names become property names. Point coordinates also appear in the table. SWAK treats all imported fields as read-only plain text. It does not execute HTML or follow description links.

Property names have a 128-character limit. Scalar values and descriptions have a 4096-character limit. Each feature has at most 256 metadata properties. GDAL text values longer than 4096 characters are cut to that length. Each decoded document retains at most 8 MiB of estimated metadata. A limit failure rejects the document and preserves the previous feed data.
