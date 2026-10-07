# TAK Server feature plan

Date: October 7, 2026. This plan compares the client-facing capabilities of TAK Server with SWAK. It proposes the SWAK features that make SWAK a full client of that server. The rule is simple: if TAK Server offers a capability to clients, SWAK should support it.

# Source and method

The review used the [TAK Server repository](https://github.com/TAK-Product-Center/Server) at commit `5187abd46d827d37cfc5708805eced197a837e49`, release 5.7-RELEASE-14. SWAK used the same release for its October 4 compatibility test. Read [TAK Server compatibility tests](tak-compatibility.md).

The review covered these sources:

- The example `CoreConfig.example.xml` for inputs, connectors, the repeater, filters, and buffers.
- 48 Spring REST controllers with about 300 endpoints under `/Marti/api`.
- 22 servlets under `/Marti`, such as `/sync/upload`, `/sync/content`, `/vcm`, and `/TracksKML`.
- 14 protobuf files, including `takmessage.proto`, `takcontrol.proto`, and `missionannouncement.proto`.
- CoT types that the server reads, writes, or repeats, such as `t-x-takp-*`, `t-x-m-*`, `b-t-f-*`, `b-f-t-r`, and `b-a-o-*`.

SWAK contains no code from the repository. This plan cites interfaces and protocol facts only.

# Scope

The plan includes capabilities that a client uses: streaming inputs, client authentication, enrollment, data exchange, and the REST endpoints that ATAK, WinTAK, and iTAK call.

The plan excludes server administration and server-to-server functions. These are federation, user and group administration, input and QoS configuration, retention, injection, certificate administration, LDAP and security configuration, the repeater configuration, database administration, and clustering. A client does not need them to take part in a TAK network.

# SWAK today

SWAK supports these TAK client functions:

- CoT XML streams over TLS (8089) and plaintext TCP (8088), with PEM and PKCS12 client certificates.
- UDP CoT input, receive only.
- The `t-x-c-t` ping. SWAK ignores `t-x-takp-*` control messages and stays in XML mode.
- Sending its own position and generated events, with contact, `__group`, and track details.
- A SWAK-only LAN mode. It does not interoperate with ATAK mesh networking.

The main limit is the CoT model. SWAK keeps the point, contact callsign, `__group`, and track. It discards all other detail content. Chat, shapes, routes, links, emergencies, file shares, and mission data all live in that discarded detail. Most features below depend on a richer detail model first.

# Capability map

Status: Yes means supported, Partial means some support, No means no support. The phase column refers to the phases below.

| Server capability | Server interface | SWAK | Phase |
| --- | --- | --- | --- |
| CoT XML over TLS and TCP | Inputs `tls` 8089, `stcp` 8088 | Yes | — |
| Ping and keepalive | `t-x-c-t`, `t-x-c-t-r` | Yes | — |
| Full CoT detail | Any detail element | No | 0 |
| REST access with the client certificate | Connector 8443, `/Marti/api` | No | 0 |
| Server version and capability check | `/api/version/config`, `/api/version` | No | 0 |
| Certificate enrollment | Connector 8446, `/api/tls/config`, `/api/tls/signClient/v2` | No | 1 |
| Enrollment and connection profiles | `/api/tls/profile/enrollment`, `/api/device/profile/connection` | No | 1 |
| Username and password on the stream | `<auth><cot username password uid/></auth>` for `file` and `ldap` inputs | No | 1 |
| TAK Protocol version 1 (protobuf) | `t-x-takp-v`, `t-x-takp-q`, `t-x-takp-r`, `takmessage.proto` | No | 1 |
| Multicast SA and GeoChat | `239.2.3.1:6969`, `224.10.10.1:17012` | No | 1 |
| UDP send to a server input | Input `udp` 8087 | No | 1 |
| QUIC streaming | Input `quic` 8090 | No | 1 (deferred) |
| Groups and channel selection | `/api/groups/user`, `/api/groups/active` | No | 2 |
| Contacts | Contact details, `/api/contacts/all` | Partial | 2 |
| Delete events | `t-x-d-d` | No | 2 |
| Emergency alerts and repeater | `emergency` detail, `b-a-o-*`, `b-a-o-can` | No | 2 |
| Directed delivery | `marti/dest` with callsign, UID, or mission | No | 2 |
| Shared drawings and routes | `u-d-*` shapes, `b-m-r` routes, `b-m-p-*` points, `link` | No | 2 |
| GeoChat | `b-t-f`, `__chat`, `remarks` | No | 3 |
| Chat receipts and store-and-forward | `b-t-f-d`, `b-t-f-r`, `b-t-f-p`, server chat storage | No | 3 |
| Data package transfer | `b-f-t-r` with `fileshare`, `/sync/upload`, `/sync/content` | No | 4 |
| Server file search | `/sync/search`, `/api/files/metadata` | No | 4 |
| Missions (Data Sync) | `/api/missions/*`, `t-x-m-c`, `t-x-m-i`, `t-x-m-n`, `t-x-m-d` | No | 5 |
| Mission logs, keywords, roles, and invitations | `/api/missions/logs/*`, `/keywords`, `/role`, `/invite` | No | 5 |
| Mission layers and feeds | `/api/missions/{name}/layers`, `/maplayers`, `/feed` | No | 5 |
| COP hierarchy | `/api/cops`, `/api/cops/hierarchy` | No | 5 |
| Server map layers | `/api/maplayers/all` | No | 6 |
| Iconsets | `/api/iconset/*`, `/api/iconurl`, `usericon` detail | No | 6 |
| Video connections | `/api/video`, `/vcm`, `b-i-v`, `__video` | No | 6 |
| History and track queries | `/api/cot/sa`, `/api/cot/xml/{uid}/all`, `/TracksKML` | No | 6 |
| ExCheck checklists | `/api/excheck/*` | No | 7 |
| CITrap reports | `/api/citrap/*` | No | 7 |
| Server data feed catalog | `/api/datafeeds`, `/api/datafeeds/bounds/*` | No | 7 |
| Plugin data exchange | `/api/plugins/{name}/submit` | No | 7 |
| UID properties and sequences | `/api/properties/*`, `/api/sync/sequence/{key}` | No | 7 |

# Phase 0: Foundations

These features have no visible value alone. All later phases need them.

## CoT detail model

Keep every received detail element as bounded XML, in addition to the typed fields. Add typed readers for the common elements: `link`, `remarks`, `__chat`, `chatgrp`, `emergency`, `marti`, `fileshare`, `__video`, `usericon`, `color`, `strokeColor`, `fillColor`, `strokeWeight`, `shape`, `status`, and `precisionlocation`. Write the same elements when SWAK sends an event.

- Limits: keep the existing document limits. Cap the element count, depth, and stored detail size for each event.
- Security: never execute or load anything that a detail names. Treat links and URLs as data until a user acts.
- Tests: round-trip ATAK sample events. Reject over-limit details without losing the typed fields.

## TAK REST client

Add a core service that sends HTTPS requests to the server of a stream entry. It uses the same CA and client certificate as the stream. The default port is 8443 and the base path is `/Marti/api`. The service uses libcurl. Read the libcurl evaluation below.

- Bound response sizes and times. Read JSON through one validation layer. Disable redirects and cookies.
- Send credentials only to the configured host and port, as the map client does now.
- Expose the service to plugins through `PluginServices`, so each feature below stays a plugin.

## Capability check

On connect, read `/api/version/config` and `/api/version`. Show the server version in the server list. Enable REST features only when the server answers. Log the version through the shared logger.

Status, October 7, 2026: Phase 0 is complete. The CoT detail model, the libcurl client, the TAK REST API service, and the version check are in place.

# Phase 1: Connection and identity

## Certificate enrollment

A TAK Server can issue client certificates on connector 8446 without a client certificate. The client reads `/api/tls/config`, generates a key pair, sends a certificate request to `/api/tls/signClient/v2` with a username and password, and receives a signed certificate and the CA chain.

- Add an Enroll action to the server editor. Store the key and certificates as files with owner-only permissions. Never write the password into the profile.
- Read the enrollment profile from `/api/tls/profile/enrollment` and apply supported settings, such as server entries.
- Keys: support both methods. SWAK can generate a new key pair, or the user can select an existing private key file. In both cases SWAK builds the certificate request from that key.
- Dependency: Qt cannot create key pairs or certificate requests. Use OpenSSL `libcrypto`, which the Qt TLS backend already loads. Document the reason in `docs/dependencies.md`.

## Username and password on the stream

Inputs with `auth="file"` or `auth="ldap"` expect an `<auth>` element with a username, password, and UID before any CoT. Add these optional fields to a server entry. Read the password from a `SWAK_COT_` environment variable, like the PKCS12 password. Send the element only over TLS.

## TAK Protocol version 1

The server starts in XML mode. A client can ask for protobuf mode through `t-x-takp-q` after the server sends `t-x-takp-v`. The server confirms with `t-x-takp-r`. Version 1 frames use the `0xbf` magic byte, a varint length, and a `TakMessage`.

- Benefit: smaller messages and less parsing work on busy servers.
- Design: decode `TakMessage`, `CotEvent`, and `Detail` messages with the Protobuf library. Convert them to the same `Event` and detail model. Keep XML as the fallback. Check each frame length before decoding.
- Dependency: Protobuf, with the lite runtime (`libprotobuf-lite`) and `protoc` at build time. Debian 13 supplies runtime 3.21.12. The build needs `libprotobuf-dev` and `protobuf-compiler`. Protobuf uses a BSD 3-clause license.
- Schema: SWAK uses its own schema, `src/core/proto/tak_protocol.proto`. It copies no TAK file, because those files are under GPL version 3. The package, message names, field names, and comments are SWAK's own. Only field numbers and wire types match TAK, because they define the binary format. On October 7, 2026, a descriptor comparison with protoc 3.21.12 found the same field numbers, wire types, and labels as the TAK Server 5.7 schema in all 10 messages.

## Multicast mesh

ATAK and WinTAK share SA on `239.2.3.1:6969` and GeoChat on `224.10.10.1:17012` without a server. The server can also proxy these groups. Add a Mesh SA server type that joins these groups on a selected interface, receives CoT, and sends SWAK's position. Keep the SWAK LAN mode as a separate option.

## UDP send

Allow a UDP entry to send to a server `udp` input, such as port 8087. Keep receive-only UDP as the default.

## QUIC

TAK Server 5.7 adds a QUIC input on port 8090. The server is not an HTTP/3 server. It uses the ALPN name `takstream` and one bidirectional QUIC stream for CoT. Qt has no QUIC client, and libcurl cannot carry this stream. Use the QUIC client in OpenSSL 3.5 instead. Debian 13 supplies OpenSSL 3.5.7. This adds `libssl` to the `libcrypto` dependency from enrollment, from the same package. TLS stays the primary transport. Read the libcurl evaluation below.

# Phase 2: Shared situational awareness

## Groups and channels

Read the user's groups from `/api/groups/user`. Show them as channels for each server. Let the user turn channels on and off through `/api/groups/active`. Keep the selection in the profile.

## Contacts

Build a contact list from received contact details and from `/api/contacts/all`. Show callsign, team, role, last report, and connection state. Add Send to and Chat with actions. Keep at most a bounded number of contacts for each server.

## Delete events

Handle `t-x-d-d`. Remove the named object from the map and the tables. Accept a delete only for objects that the same connection supplied.

## Emergency alerts

Show received emergencies, such as 911 Alert, Ring The Bell, Geo-fence Breached, and Troops In Contact, with a strong banner and map highlight. Handle `b-a-o-can` cancel events. The server repeater resends active alerts, so the display must ignore duplicates. Add a protected Send emergency action with confirmation. Add a cancel action.

## Directed delivery

Add a `marti` element with `dest` entries when the user sends to chosen contacts or a mission. Without a `marti` element, the server delivers to the sender's groups.

## Shared drawings and routes

Send SWAK markers, circles, lines, areas, and routes as standard TAK CoT, so ATAK users see them. Receive the same types as editable or read-only local objects.

| SWAK object | TAK CoT |
| --- | --- |
| Marker | `a-*` or `b-m-p-s-m` point with `usericon` and `color` |
| Circle | `u-d-c-c` with `shape/ellipse` |
| Line | `u-d-f` with `link` points |
| Area | `u-d-f` closed shape with fill color |
| Rectangle | `u-d-r` |
| Route | `b-m-r` with `link` checkpoints and `link_attr` |

- Share only on an explicit Share action for each object. The object then stays shared, and SWAK sends its updates and deletion.
- Add a global setting, Share new objects automatically, under File > Settings. It is off by default. When it is on, SWAK shares each new local object on save.
- Keep receiving and sending separate. Never forward received events automatically.
- Convert stroke and fill colors from the ARGB integer that TAK uses.

# Phase 3: Chat

Add a Chat pane for GeoChat. TAK uses `b-t-f` events with `__chat`, `chatgrp`, `link`, and `remarks`.

- Rooms: All Chat Rooms, team rooms, and direct chats with contacts.
- Receipts: send and show delivered (`b-t-f-d`) and read (`b-t-f-r`) receipts. Show pending (`b-t-f-p`) state.
- Store and forward: the server can store chat for offline users. SWAK receives the stored messages after it connects. Remove duplicates by message ID.
- Limits: bound message length, room count, and history. Show text as plain text only.

# Phase 4: Files and data packages

## Receive data packages

A `b-f-t-r` event carries a `fileshare` element with a sender URL, file name, size, and SHA-256 hash. The file usually comes from `/Marti/sync/content?hash=`.

- Ask the user before a download. Show the sender, name, and size.
- Download with the client certificate. Enforce a size limit. Check the SHA-256 hash before use.
- Read the package `MANIFEST/manifest.xml`. Import CoT files as objects. Pass KML, KMZ, GeoTIFF, GeoPackage, and other files to the existing data and map providers.
- Reuse the archive limits of the GDAL vector reader. Reject unsafe paths and nested archives beyond the limit.

## Send data packages

Build a package from selected objects and files. Upload it with `/Marti/sync/upload`. Send a `b-f-t-r` event to the chosen contacts, or post it to a mission.

## Server files

Add a server file browser through `/Marti/sync/search` and `/api/files/metadata`. Allow download and import. Allow delete only when the server permits it.

# Phase 5: Missions (Data Sync)

Missions are shared, persistent workspaces on the server. They hold CoT objects, files, logs, layers, and feeds. ATAK calls this feature Data Sync.

- List missions from `/api/missions` or `/api/pagedmissions`. Show name, description, creator, and password state.
- Subscribe with `/api/missions/{name}/subscription`. Support passwords and read-only tokens. Keep the subscription per server.
- Load contents: latest CoT from `/missions/{name}/cot`, files from the mission record, and changes from `/missions/{name}/changes`.
- Follow live changes from `t-x-m-c` events on the stream. Handle create (`t-x-m-n`), delete (`t-x-m-d`), and invitation (`t-x-m-i`) events.
- Edit: add and remove objects and files with `/contents`. Respect the mission role: owner, subscriber, or read-only.
- Logs: read and write mission log entries with `/missions/logs/entries`.
- Extras: keywords, expiration, parent and child missions, mission layers, map layers, and data feeds.
- Export: download the mission archive or KML.
- COP: show the mission hierarchy from `/api/cops/hierarchy` when the server provides it.

Show each subscribed mission as a layer group in the Objects pane, with its own visibility control.

# Phase 6: Server-provided resources

## Map layers

`/api/maplayers/all` and mission map layers describe XYZ, WMS, and WMTS sources. Offer them as map provider presets for the current server. Keep them off until the user selects one. They use the existing GDAL raster provider and authentication.

## Iconsets

Download iconsets from the server and add them as marker collections. Show received objects with their `usericon` iconset path. Bound archive size, icon count, and image size. Fall back to the standard symbol for a missing icon.

## Video

Read video connections from `/api/video` and `b-i-v` events with `__video` details. List them in a Video pane. Play them inside SWAK with Qt Multimedia and its FFmpeg backend. Debian 13 supplies Qt Multimedia 6.8.2 and FFmpeg 7.1. The build needs `qt6-multimedia-dev`.

TAK video uses RTSP, RTMP, SRT, HLS, and MPEG-TS over UDP. Start with a test of each protocol in Qt Multimedia. Show a protocol that does not play as unavailable, with the reason. Do not start an external player.

## History and tracks

Query past positions with `/api/cot/sa` for a time range and area, and `/api/cot/xml/{uid}/all` for one track. Show the result as a track and connect it to Trace. Add a time window control. Bound the result size.

# Phase 7: Other services

- ExCheck: show checklists and templates from `/api/excheck`. Allow task status updates. Link checklists to missions.
- CITrap: list, create, and update reports with attachments through `/api/citrap`.
- Data feed catalog: list server feeds from `/api/datafeeds` and `/api/datafeeds/bounds/{bbox}`. Show which feeds the user receives.
- Plugin data: give SWAK plugins a bounded call to `/api/plugins/{name}/submit`.
- Properties and sequences: support `/api/properties/{uid}` and `/api/sync/sequence/{key}` when a feature needs them.

# Recommended order

| Order | Feature | Size | Reason |
| --- | --- | --- | --- |
| 1 | CoT detail model | Medium | Needed by every later feature |
| 2 | TAK REST client and capability check | Medium | Needed by all REST features |
| 3 | Shared drawings and routes, delete events | Medium | High value for mixed ATAK and SWAK teams |
| 4 | Chat with receipts | Medium | Common daily use |
| 5 | Contacts, groups, and channels | Medium | Needed for directed chat and sharing |
| 6 | Emergency alerts | Small | Safety-relevant display, small scope |
| 7 | Certificate enrollment and stream authentication | Medium | Removes manual certificate work |
| 8 | Data packages | Large | Main file exchange in TAK networks |
| 9 | Missions | Large | Persistent shared workspaces |
| 10 | Map layers and iconsets | Small | Reuses existing providers and collections |
| 11 | Multicast mesh | Medium | Serverless ATAK interoperability |
| 12 | TAK Protocol version 1 | Medium | Efficiency on busy servers |
| 13 | History, video, ExCheck, CITrap, and others | Varies | Lower demand |
| 14 | QUIC | Large | Uses the OpenSSL QUIC client |

# Engineering rules for every phase

- Put each feature behind a plugin interface. Use the shared core services for networking, logging, and Settings.
- Validate every server reply in one layer before any state change. Bound sizes, counts, depths, and times. Fuzz the CoT detail and protobuf decoders.
- Keep credentials out of profiles. Send them only to the configured host.
- Never forward received data automatically. Ask the user before downloads, emergency sends, and mission changes.
- Test each feature against a disposable TAK Server 5.7 container, extending `swak_tak_compat`. Add unit tests for parsing and limits. Add GUI tests for each pane.
- Update `docs/plugin-api.md`, `docs/security.md`, `docs/servers.md`, and the user guide with each phase.

# Decisions

The user made these decisions on October 7, 2026.

1. Enrollment keys: support both. SWAK generates a key pair, or the user supplies a private key. SWAK uses OpenSSL `libcrypto`.
2. TAK Protocol version 1: use the Protobuf library.
3. Video: play inside SWAK with Qt Multimedia. Each streaming protocol needs a successful playback test first.
4. Connectivity: evaluate libcurl for all related connections. The result is below.
5. Sharing: share an object only after an explicit Share action. A global setting can share new objects automatically.

The user then chose SWAK's own `.proto` schema instead of the GPL files. This removed the license question for the protocol.

# libcurl evaluation

The development system has libcurl 8.14.1 with OpenSSL 3.5.7, HTTP/2, and HTTP/3 through nghttp3 and ngtcp2. GDAL already links libcurl for raster tiles and remote files, so libcurl adds no new package.

| Connection | Today | libcurl fit | Recommendation |
| --- | --- | --- | --- |
| TAK REST API, enrollment, data packages, missions, iconsets, video list | None | Good. Client certificates in PEM and PKCS12, HTTP/2, HTTP/3, proxies, resumable downloads, and size limits in callbacks. | Use libcurl for the TAK REST client. |
| QUIC CoT stream | None | Not possible. libcurl uses QUIC only for HTTP/3. TAK uses the `takstream` ALPN with a raw stream. | Use the OpenSSL QUIC client. |
| TLS and TCP CoT streams | QSslSocket | Possible with `CURLOPT_CONNECT_ONLY` and `curl_easy_send` or `curl_easy_recv`. It gives no protocol gain. | Keep QSslSocket. Revisit only if TLS settings must match the REST client exactly. |
| UDP and multicast CoT | QUdpSocket | Not supported. | Keep Qt sockets. |
| Map tiles through GDAL | libcurl through GDAL | Already libcurl. | No change. |
| MVT tiles, data feeds, routing, Google and Azure sessions | QNetworkAccessManager | Good. A switch gives one TLS and proxy configuration and HTTP/3. The MVT disk cache needs a replacement. | Move after the TAK REST client proves the libcurl integration. |

Design for the libcurl client:

- One core `NetworkClient` owns a curl multi handle. A worker thread runs it, so the interface thread never waits for the network.
- Each request has a size limit, a time limit, and a cancellation flag. Redirects stay off unless a caller enables them for the same host.
- Credentials come from the same files and environment variables as the CoT stream. Each request checks the destination host before it sends a credential.
- Replies return to the interface thread through queued signals.
- Tests use the existing local HTTPS fixtures and a disposable TAK Server container.

Conclusion: libcurl can replace Qt for all HTTPS connectivity. It cannot replace the QUIC stream or the UDP and multicast inputs. The TLS and TCP CoT streams gain nothing from a switch.
