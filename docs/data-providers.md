# Data providers

Research date: October 2, 2026. KML is an XML format for geographic features. KMZ is a ZIP archive that contains a KML document. Data feeds draw above map imagery and below CoT events. They do not create or transmit CoT events.

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

Open Tools > Data Providers. Select Add to create a blank feed. Enter its name, direct HTTPS URL, attribution, and refresh interval. Enable the feed when it is ready. Add and Edit use the same form system as Map Providers.

The table checkbox enables or disables each feed. Multiple feeds can draw together. Edit changes the selected entry. Delete removes the selected row and its geometry. Selecting a row does not enable it. Refresh now updates an enabled feed without waiting for its timer.

The refresh interval ranges from 60 through 86400 seconds. Each feed has its own timer. Its next interval starts after a download and parse attempt finishes. Manual refresh does not overlap an update already in progress. Disabled feeds do not download.

File > Settings > Data providers contains the same table as the management window. Add and Edit expose all feed parameters. Provider changes apply immediately. The form includes color, opacity, line width, point radius, polygon fill opacity, and labels. Names sort according to the desktop locale. The application saves feed configuration and manager geometry across restarts. Missing plugins retain their configuration but do not download data.

A failed update retains the previous valid geometry and marks its credit as STALE. The manager shows the failure. A successful empty document clears the old geometry. Disabling, removing, or editing a feed clears its current geometry. Geometry remains in memory and is downloaded again after restart. The map combines data credits with its clickable, right-aligned attribution panel.

# Supported geometry

The reader supports KML 2.2 and the older Google KML 2.0/2.1/2.2 namespaces. It draws Point, LineString, Polygon with inner rings, and MultiGeometry inside Placemarks. Names appear as plain labels. Hidden folders and placemarks are excluded when visibility precedes their children. Coordinates use longitude and latitude. The renderer projects them to Web Mercator and clips polar positions.

The map draws flat geometry. It does not apply altitude, terrain, time animation, KML styles, style maps, HTML balloons, external icons, models, tours, or image overlays. Each feed uses its configured appearance. Network links do not trigger downloads. The manager reports counts of skipped links and unsupported overlays or models. A document with only unsupported content can therefore show no shapes.

Format references: [KML reference](https://developers.google.com/kml/documentation/kmlreference) and [KMZ archive structure](https://developers.google.com/kml/documentation/kmzarchives).

# Bounds and recovery

Downloads require HTTPS and use normal certificate and hostname checks. A per-feed override starts disabled and appears in the manager status. Each feed owns a separate network manager. The application disables redirects and cookie handling. Requests have a 20-second transfer timeout and a 30-second total deadline. No remote resource can select a local file.

The application limits stored feeds to eight and active work to two downloads or parsers. Parsing uses a private thread pool outside the interface thread. Removal and shutdown cancel work. A replaced feed cannot receive an old parse result. Updates replace complete documents instead of appending duplicate features.

The input bounds are:

- Download bytes: 8 MiB per response.
- Expanded KML: 16 MiB per archive.
- Archive entries: 256, with 32 MiB of declared expanded content.
- XML depth: 32, with 200000 elements and 32 attributes per element.
- Document geometry: 10000 shapes and 100000 coordinate tuples.
- Visible text: 100 feature labels across all feeds.

Archive processing accepts complete ZIP archives with a central directory and never extracts files to disk. The reader skips unused entries without stream decompression. It rejects encrypted entries, links, absolute paths, parent traversal, and multiple KML documents. XML processing rejects document types, entity references, and processing instructions. Invalid coordinates and excessive structure fail the whole update. These bounds do not replace dependency security updates.

# Dependencies

Qt Network supplies HTTPS. QXmlStreamReader supplies XML parsing. Qt Concurrent supplies bounded background work. The KML plugin uses libarchive for ZIP decompression because Qt has no public general-purpose ZIP reader. This avoids a custom archive implementation or private Qt APIs.

Libarchive is dynamically linked and uses a BSD license. The tested Debian packages are `libarchive-dev` and `libarchive13t64`, version `3.7.4-4+deb13u1`. Use distribution security updates. The [Debian tracker](https://security-tracker.debian.org/tracker/source-package/libarchive) records outstanding and fixed issues by package version. Only ZIP support is enabled in this application. Other archive readers remain unused.

The October 2 dependency review found open Debian notices for RAR5 reading, TAR sparse headers, and encrypted ZIP writing. These paths are not used by the released application. The ZIP writer appears only in tests with small, unencrypted fixtures. The separate ACL notice concerns a text ACL parser that this reader does not call. This scope review does not establish that the library has no other defects.

Review references: [RAR5 notice](https://security-tracker.debian.org/tracker/CVE-2026-14164), [TAR notice](https://security-tracker.debian.org/tracker/CVE-2026-15028), [ZIP writer notice](https://security-tracker.debian.org/tracker/CVE-2026-16517), and [ACL notice](https://security-tracker.debian.org/tracker/CVE-2026-5745).

# Object information

KML descriptions and scalar properties appear in the Information pane. ExtendedData names become property names. Point coordinates also appear in the table. SWAK treats all imported fields as read-only plain text. It does not execute HTML or follow description links.

Property names have a 128-character limit. Scalar values and descriptions have a 4096-character limit. Each placemark has at most 256 metadata properties. Each decoded document retains at most 8 MiB of estimated metadata. A limit failure rejects the document and preserves the previous feed data.
