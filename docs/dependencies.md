# GDAL

SWAK uses GDAL 3.10 for map and data formats. GDAL is the Geospatial Data Abstraction Library. It reads more than 200 raster and vector formats and converts coordinates between reference systems. The user decided on October 5, 2026 to replace the custom KML and MVT decoders with GDAL. This decision also brings local files and the other GDAL formats into SWAK. Qt does not supply these formats.

Install `libgdal-dev` to build SWAK. The runtime package is `libgdal36`. On Debian 13 it adds 37 packages, which include PROJ, GEOS, HDF5, NetCDF, and database client libraries. SWAK links GDAL dynamically and bundles no GDAL code. Use distribution security updates for GDAL and its format libraries.

GDAL runs in the application process. The user selected this design. docs/quality.md asks for a separate worker process when native code can fail with memory corruption. GDAL format drivers are a large native attack surface, and fuzzing regularly finds memory faults in them. A fault in a GDAL driver can therefore stop SWAK. Record this accepted risk in reviews. Do not move GDAL into a worker process without a new user decision.

SWAK limits what GDAL can reach. `initializeGdal` in src/core/gdal_support.cpp sets these limits once:

- Removed drivers: drivers that run programs, open database connections, or fetch any URL. GPSBabel, HTTP, PostgreSQL, MySQL, ODBC, and the cloud service drivers are examples.
- Allowed drivers: each open call names a fixed list of file format drivers. The vector list excludes VRT, which can open other files.
- Files: GDAL writes no `.aux.xml` sidecar files and does not scan directories on open.
- Memory: the raster block cache is 64 MiB. One GeoJSON object can use at most 16 MiB.
- Network: certificate checks stay on. Requests time out after 20 seconds and retry once.

SWAK also keeps its own limits around GDAL. Read [Data providers](data-providers.md) for the data limits and [Map providers](map-providers.md) for the vector tile limits. GDAL uses the MIT license. Its linked libraries use their own licenses. Help > Credits lists GDAL and its version.

The earlier versions of SWAK included protozero 1.8.2 headers for MVT decoding. GDAL replaced them, and the source tree no longer contains them.

# libcurl

SWAK links libcurl directly for HTTPS requests to TAK Server. The user decided on October 7, 2026 to use libcurl for all HTTPS connectivity. Read the libcurl evaluation in [TAK Server feature plan](tak-server-feature-plan.md). GDAL already loaded libcurl, so the runtime adds no package. Install `libcurl4-openssl-dev` to build SWAK. SWAK needs libcurl 7.85 or later. The client passes certificates and keys to libcurl from memory, so libcurl must use OpenSSL. libcurl uses the curl license, a permissive MIT-style license.

# OpenSSL

SWAK links OpenSSL `libcrypto` and `libssl` 3.2 or later directly. Enrollment uses them for keys and certificate requests. QUIC streams use the OpenSSL QUIC client, which OpenSSL 3.2 added. The tests also use the QUIC server of OpenSSL 3.5. Qt cannot create key pairs or certificate requests. The user decided on October 7, 2026 to use OpenSSL for these tasks. The Qt TLS backend and libcurl already load OpenSSL, so the runtime adds no package. Install `libssl-dev` to build SWAK. OpenSSL 3 uses the Apache License 2.0.

OpenSSL 3.5.7 has a defect on one allocation failure path. When the allocation at `crypto/asn1/tasn_new.c:299` fails during `X509_REQ_sign`, OpenSSL frees an embedded ASN.1 field, and the process aborts. SWAK cannot prevent this, because the failure happens inside OpenSSL. It needs an out-of-memory condition at that point of an enrollment. `tests/test_openssl_faults.cpp` records the site. Check it again after each OpenSSL update.

# Protobuf

SWAK uses the Protobuf lite runtime for TAK Protocol version 1. The user decided on October 7, 2026 to use the Protobuf library. The schema is SWAK's own file, `src/core/proto/tak_protocol.proto`, which matches the TAK wire format and copies no TAK file. The build runs `protoc` and compiles the generated code in a separate library without the SWAK warning flags. Install `libprotobuf-dev` and `protobuf-compiler` to build SWAK. The runtime package is `libprotobuf-lite32t64` on Debian 13. Protobuf uses a BSD 3-clause license.

# Qt Multimedia

The video plugin plays TAK streams inside SWAK with Qt Multimedia and Qt Multimedia Widgets. The user decided on October 7, 2026 to play video inside SWAK and not to start an external player. Only the video plugin links these modules. Qt Multimedia uses its FFmpeg backend on Debian 13, which loads the system FFmpeg 7.1 libraries. FFmpeg decodes untrusted stream data in the SWAK process. SWAK limits this risk: it plays only RTSP, RTMP, SRT, HLS over HTTP or HTTPS, and MPEG-TS over UDP, never local files; it plays only after the user selects Play; it has no audio output; and it stops a stream when the pane hides. Install `qt6-multimedia-dev` to build SWAK. Qt Multimedia uses the LGPL 3. FFmpeg uses the LGPL 2.1 or later in the Debian build.

Qt, Qt Multimedia, libarchive, GeographicLib, GDAL, libcurl, OpenSSL, and Protobuf are system dependencies. Read the build manual for their required components.

Marker collections also use the existing system libarchive dependency. The shared collection service reads ZIP, TAR, and gzip-compressed TAR, and writes ZIP or compressed TAR. It enables no external decompression programs and extracts no files. The additional archive use avoids a separate ZIP implementation or dependency. Read [Marker collections](marker-collections.md) for the one-file format and resource limits.

Local polygon tools add no external dependency. Qt paints and stores the geometry. The application calculates a spherical area estimate internally. [Turf area documentation](https://turfjs.org/docs/api/area) informed the review of geographic area conventions. SWAK includes no Turf code or package.


## Command icons

The application includes six SVG assets from [Lucide 0.468.0](https://github.com/lucide-icons/lucide/tree/0.468.0/icons). Lucide provides a consistent 24-unit grid with two-unit strokes. The included license covers Lucide and identifies Feather-derived icons. The source assets and notices are under `third_party/lucide`. Installation includes the notices.

`tools/generate_icons.py` generates the checked-in Qt paths from those SVG assets. FontTools 4.55.3 is a development-only converter. Normal builds and releases do not need it. This conversion avoids a new runtime SVG library and preserves vector scaling. Run `python3 tools/generate_icons.py` with that FontTools version available to regenerate the paths. The script reads only the bundled assets and performs no downloads.

Application branding, marker symbols, and color swatches retain their own meanings. The shared command set covers Clear, Marker, Area, Map, Edit, and Move.


## CoT transport and diagnostics

CoT connections use the existing Qt Network and Core libraries. TLS and PKCS12 support use the Qt TLS backend installed on the host. This change adds no runtime dependency or copied TAK Server implementation. The CoT and LAN harnesses use the Python standard library. Isolated LAN tests also use the development tools unshare and ip. Installation includes none of these harnesses.

Circle, Distance, and Line use original vector paths in tools/generate_icons.py. They share the application icon renderer and its stroke style. They add no library dependency.

The UID lock and unlock buttons use original vector paths in the shared icon factory. They add no external dependency.

The Server, Data providers, OK, and Apply icons use original vector paths in the shared factory. Team swatches use the existing vector icon engine. These changes add no runtime dependency.

# Source formatters

Development uses clang-format 19, Black 25.1.0, and cmake-format 0.6.13 to enforce the documented source style. These tools provide repeatable formatting across the repository. SWAK does not link, package, or install them. Read [Coding style](coding-style.md) for commands and exclusions.


# Coordinate conversion

Grid Types uses GeographicLib for WGS 84 conversion between geographic coordinates, UTM, UPS, and MGRS. UTM uses zones in meters. UPS covers polar regions. MGRS names cells within these coordinate systems. Qt does not supply these conversions. This dependency avoids custom projection series and military grid lettering rules.

The upstream [GeographicLib repository](https://github.com/geographiclib/geographiclib) describes these conversions and its MIT license. Development uses Debian GeographicLib 2.5-1. The package copyright notices are in GeographicLib-copyright.txt and install with SWAK. The notices also identify other package components. No source files from GeographicLib enter the repository. Builds find an installed development package and do not download code.

On this host, the Debian packages are extracted under build/geographiclib. The CMake cache names the extracted include directory and static library. This local path is not portable. Install libgeographiclib-dev on another machine, as described in build.md. The release executable on this host links the static library. A normal system build can link its shared library.

The grid renderer limits entries, visible lines, sampling points, coordinate formats, and user text. GeographicLib receives finite, bounded coordinates. Its coordinate-domain exceptions stop the affected grid cell. The application does not suppress memory corruption or fatal signals.


# Credits inventory

Help > Credits uses the existing library version APIs and pinned asset versions. It adds no dependencies. GDAL reports the PROJ and GEOS versions through its build information. The libcurl row shows the loaded libcurl version and its TLS library. One row names the other format libraries that GDAL loads from the system. The table names direct application dependencies, bundled headers and icons, libarchive compression backends, and the active Qt TLS backend. System graphics and desktop dependencies vary with the host.

The license references include [Qt licensing](https://doc.qt.io/qt-6/licensing.html), [libarchive](https://www.libarchive.org/), [GeographicLib](https://github.com/geographiclib/geographiclib), and [GDAL](https://gdal.org/license.html). Bundled Lucide notices remain under third_party. The table summarizes license families and does not replace the distributed notices.

Clear commands use an original eraser path in the shared vector icon factory. Delete commands retain the bundled Lucide trash icon.
