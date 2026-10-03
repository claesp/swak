# Decoder dependency

SWAK includes unmodified protozero 1.8.2 headers under `third_party/protozero`. Protozero is a small Protocol Buffers decoder used by established mapping projects. Qt Core does not provide this decoder. This dependency avoids a custom implementation of binary field and variable-length integer parsing. It adds no separate runtime library, browser engine, or network client.

The upstream [repository](https://github.com/mapbox/protozero) identifies its mapping users. The [change log](https://github.com/mapbox/protozero/blob/master/CHANGELOG.md) records a Boolean reader overrun fix in 1.8.1. The pinned 1.8.2 version includes that fix. The [security page](https://github.com/mapbox/protozero/security) listed no published advisories during the October 2, 2026 review. This result does not establish that the library contains no defects.

The source archive is `https://codeload.github.com/mapbox/protozero/tar.gz/refs/tags/v1.8.2`. Its SHA-256 digest is `68d1df951b4d8397355c8fc9ffed40d9c06282a877fe847403feac6edcee3ef1`. The import contains upstream headers and both license files. CMake installs the license files with the application documentation. Builds do not download dependencies.

Protozero uses BSD-2-Clause terms, with Apache-2.0 terms for code identified in `LICENSE.from_folly`. Keep both notices when distributing binaries or source. Changes to the dependency require a new source digest, license review, advisory review, and parser diagnostics. SWAK supplies separate bounds and wire-type validation around the library. Application tests exercise the decoder under address and undefined behavior sanitizers.

Qt and libarchive remain system dependencies. Read the build manual for their required components. Read the data provider manual for the libarchive dependency review and its restricted ZIP use.

Marker collections also use the existing system libarchive dependency. The shared collection service reads ZIP, TAR, and gzip-compressed TAR, and writes ZIP or compressed TAR. It enables no external decompression programs and extracts no files. The additional archive use avoids a separate ZIP implementation or dependency. Read [Marker collections](marker-collections.md) for the one-file format and resource limits.
