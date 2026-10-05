# Decoder dependency

SWAK includes unmodified protozero 1.8.2 headers under `third_party/protozero`. Protozero is a small Protocol Buffers decoder used by established mapping projects. Qt Core does not provide this decoder. This dependency avoids a custom implementation of binary field and variable-length integer parsing. It adds no separate runtime library, browser engine, or network client.

The upstream [repository](https://github.com/mapbox/protozero) identifies its mapping users. The [change log](https://github.com/mapbox/protozero/blob/master/CHANGELOG.md) records a Boolean reader overrun fix in 1.8.1. The pinned 1.8.2 version includes that fix. The [security page](https://github.com/mapbox/protozero/security) listed no published advisories during the October 2, 2026 review. This result does not establish that the library contains no defects.

The source archive is `https://codeload.github.com/mapbox/protozero/tar.gz/refs/tags/v1.8.2`. Its SHA-256 digest is `68d1df951b4d8397355c8fc9ffed40d9c06282a877fe847403feac6edcee3ef1`. The import contains upstream headers and both license files. CMake installs the license files with the application documentation. Builds do not download dependencies.

Protozero uses BSD-2-Clause terms, with Apache-2.0 terms for code identified in `LICENSE.from_folly`. Keep both notices when you distribute binaries or source. Changes to the dependency require a new source digest, license review, advisory review, and parser diagnostics. SWAK supplies separate bounds and wire-type validation around the library. Application tests exercise the decoder under address and undefined behavior sanitizers.

Qt and libarchive remain system dependencies. Read the build manual for their required components. Read the data provider manual for the libarchive dependency review and its restricted ZIP use.

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

Help > Credits uses the existing library version APIs and pinned asset versions. It adds no dependencies. The table names direct application dependencies, bundled headers and icons, libarchive compression backends, and the active Qt TLS backend. System graphics and desktop dependencies vary with the host.

The license references include [Qt licensing](https://doc.qt.io/qt-6/licensing.html), [libarchive](https://www.libarchive.org/), and [GeographicLib](https://github.com/geographiclib/geographiclib). Bundled protozero and Lucide notices remain under third_party. The table summarizes license families and does not replace the distributed notices.

Clear commands use an original eraser path in the shared vector icon factory. Delete commands retain the bundled Lucide trash icon.
