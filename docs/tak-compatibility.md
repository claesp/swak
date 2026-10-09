# TAK Server compatibility tests

The development probe uses SWAK's actual Servers, Store, XML parser, and encoder. It opens two client connections through a configured server. It sends synthetic CoT in both directions, checks identity fields and positions, updates the same UIDs, and reconnects one client. When the profile names a REST API port, it then runs the feature checks below. Its JSON report records each result. A nonzero exit code means that a stage or a check failed.

# Disposable test server

`tools/tak_server.py` builds and runs official TAK Server 5.7-RELEASE-14 from source commit 5187abd46d827d37cfc5708805eced197a837e49 in Podman containers. It needs Podman, Git, OpenSSL, network access for the first build, and about 4 GiB of free memory. Everything it writes stays under `build/tak-server`. The deployment publishes only loopback ports: 18088 (plain TCP CoT), 18089 (TLS CoT), 18443 (REST API with client certificates), and 18446 (enrollment with a user name and password). The database and the service network stay private.

Run the stages from the repository root:

```sh
python3 tools/tak_server.py fetch
python3 tools/tak_server.py build
python3 tools/tak_server.py certs
python3 tools/tak_server.py up
cmake --build --preset debug --target swak_tak_compat --parallel 4
python3 tools/tak_server.py test
python3 tools/tak_server.py down
```

- `fetch` clones the pinned source and adds the local release tag that the TAK build reads.
- `build` builds the TAK Server war and the schema manager without the web dashboards. Gradle keeps its cache under `build/tak-server/gradle-home`.
- `certs` creates a private test CA, the server keystores, and the client certificates `left`, `right`, and `admin`. They expire after seven days.
- `up` starts PostgreSQL 15 with PostGIS 3.3, applies the TAK schema, and starts the configuration, messaging, and API services from `tools/tak-server/start.sh`. It writes the user file with BCrypt from the TAK build. The configuration is `tools/tak-server/CoreConfig.xml`.
- `test` writes `build/tak-server/profile.json` and runs the probe. Use `--preset sanitize` to run the probe under AddressSanitizer and UndefinedBehaviorSanitizer. The report is `build/tak-server/report-<preset>.json`.
- `status` shows the containers and ports. `down` removes the containers and the network but keeps the build and the credentials.

The test users are `swak-left` and `swak-right` with certificates, `swak-admin` with the admin role, and `swak-enroll` with a password for enrollment. The probe sends synthetic objects with short lifetimes and creates and deletes a test mission and video connection.

# Feature checks

| Check | Production code | What passes |
| --- | --- | --- |
| version | TakServerApi | The server version after the TLS connection |
| channels | TakChannels | The user's groups, and an apply of the same active set |
| contacts | Contacts | The other connected client in `/contacts/all` |
| chatDirect, chatReceipt, chatAllRooms | Chat | A direct message, its delivered receipt, and an All Chat Rooms message |
| shareDrawing, deleteDrawing | ObjectSharing, readTakDrawing | A shared area arrives as a drawing, and Stop sharing removes it |
| emergency, emergencyCancel | emergencyEvent | An alert arrives and its cancel removes it |
| packageUpload, packageOffer, packageImport, serverFiles | FileShares, data packages | Upload, the directed offer, download, hash check, import, and the file list |
| missionCreate to missionDelete | Missions | Subscribe, add an object, write the log, export the archive |
| excheckList | ExCheck | The active checklist list and templates |
| citrapCreate, citrapList, citrapUpdate | Citrap | A report with an attachment, the list, and a status change |
| videoCreate, videoList | VideoSources | A connection from `/video` |
| dataFeeds, mapLayers | readServerFeeds, readServerMapLayers | The catalogs parse |
| history | TrackHistory | Past positions of the left client |
| enrollment, enrolledConnection | TakEnrollment | A signed certificate, then a TLS stream with it |

# Results, October 9, 2026

All transport stages and all 31 feature checks passed against TAK Server 5.7-RELEASE-14. The run under AddressSanitizer and UndefinedBehaviorSanitizer also passed without a report. The first runs found these SWAK faults, which are now fixed:

- Data packages used stored zip entries with data descriptors. Java ZipInputStream, which TAK Server and ATAK use, rejects them. SWAK now writes deflated entries.
- Enrollment rejected the signed certificate, because TAK Server wraps base64 text at 64 characters. SWAK now removes the line breaks.
- `/api/video` answers JSON, not XML. SWAK now reads both.
- CI-TRAP needs a zip package with the report XML to create a report, raw attachment bytes with a Content-Disposition file name, a WKT `POINT(longitude latitude)` location, and `userDescription` as an attribute. The search answers JSON. SWAK now uses this format.
- ExCheck needs `clientUid` on the active list and `clientUid` and `startTime` to start a checklist.

These results establish compatibility with this server version and configuration. LDAP, federation, QUIC, multicast, iconsets, ExCheck templates and task updates, long-duration loads, and packet loss remain outside the probe. The GUI tests cover the panes through local fixtures.

# Manual profile

Use a disposable test server. The probe transmits synthetic markers with a three-minute lifetime. Give both test users permission to exchange data in the same server group. Certificate trust, user permissions, and server filters affect the result. Do not put passwords or private key contents in the profile.

Build the probe from the repository root with the debug preset:

```sh
cmake --preset debug
cmake --build --preset debug --target swak_tak_compat --parallel 2
```

Create a local JSON profile with two client configurations. Use separate test certificates for the two clients. Each server certificate must match the configured host.

```json
{
  "left": {
    "protocol": "tls",
    "host": "localhost",
    "port": 18089,
    "ca": "/path/to/test-ca.pem",
    "certificate": "/path/to/client-a.pem",
    "key": "/path/to/client-a.key"
  },
  "right": {
    "protocol": "tls",
    "host": "localhost",
    "port": 18089,
    "ca": "/path/to/test-ca.pem",
    "certificate": "/path/to/client-b.pem",
    "key": "/path/to/client-b.key"
  }
}
```

For PKCS12 credentials, set certificate to the archive path. Omit key. Use passwordEnv with a dedicated SWAK_COT_ environment variable. For plaintext TCP, use protocol stcp and its configured port. Plaintext tests do not establish certificate authentication.

Run the probe from the repository root after the server starts:

```sh
./build/debug/swak_tak_compat /path/to/profile.json
```

The probe remains outside the release executable and installation. It requires no new runtime dependency. It does not replace GUI rendering tests, sustained load tests, or tests against your deployment's authentication policy.

# Local results, October 4, 2026

The test used official TAK Server 5.7-RELEASE-14, source commit 5187abd46d827d37cfc5708805eced197a837e49. The [official build guide](https://github.com/TAK-Product-Center/Server/blob/5187abd46d827d37cfc5708805eced197a837e49/README.md) supplied the deployment approach. Java 17 built the configuration and messaging services in a disposable container. The test excluded web dashboards and the API service. PostgreSQL 15 with PostGIS 3.3 used the official schema manager. It applied 94 database updates.

Both test clients used the same SWAK core as commit 306120f8d22f. The server used normal persistence and a shared test group. TLS used a temporary private CA and two separate client identities. The test published only loopback ports 18088 and 18089. The database and service network remained private.

| Test | Result | Evidence |
| --- | --- | --- |
| TCP exchange, identity fields, position updates, and reconnect | Passed | build/tak-tcp-result.json |
| TLS with PEM client certificates | Passed | build/tak-tls-sanitize-result.json |
| TLS with encrypted PKCS12 client certificates | Passed | build/tak-pkcs12-result.json |
| Server certificate without a trusted CA | Rejected as expected | build/tak-untrusted-result.json |
| TLS without client certificates | Rejected as expected | build/tak-no-client-result.json |
| AddressSanitizer and UndefinedBehaviorSanitizer during TLS exchange | Passed without a report | build/tak-tls-sanitize-errors.log |

TAK's log confirmed PEER_DID_NOT_RETURN_A_CERTIFICATE for the missing-client test. SWAK reported TLS certificate validation failed for the untrusted-server test. Malformed profiles, missing client configurations, and unsupported protocols also failed before transmission.

This establishes basic CoT XML interoperability with this server version and configuration. It does not establish compatibility with every TAK deployment. Enrollment, LDAP, mission APIs, federation, binary-only endpoints, long-duration loads, and packet loss remain outside this test. The existing GUI tests cover rendering through SWAK's Store. This probe tests transport and decoded values without opening the GUI.
