# TAK Server compatibility tests

The development probe uses SWAK's actual Servers, Store, XML parser, and encoder. It opens two client connections through a configured server. It sends synthetic CoT in both directions, checks identity fields and positions, updates the same UIDs, and reconnects one client. When the profile names a REST API port, it then runs the feature checks below. Its JSON report records each result. A nonzero exit code means that a stage or a check failed.

# Disposable test server

`tools/tak_server.py` builds and runs official TAK Server 5.7-RELEASE-14 from source commit 5187abd46d827d37cfc5708805eced197a837e49 in Podman containers. It needs rootless Podman, Git, OpenSSL, network access for the first build, and about 8 GiB of free memory. Everything it writes stays under `build/tak-server`.

The deployment has two TAK Servers, two PostgreSQL databases, and one directory on the Podman network `swak-tak` with the fixed subnet 10.89.77.0/24. In hub mode, it also has a TAK federation hub:

| Container | Address | Use |
| --- | --- | --- |
| swak-tak-server (server A) | 10.89.77.10 | File users, multicast input and output, federation server |
| swak-tak-server-b (server B) | 10.89.77.11 | Directory users over LDAPS, outgoing federation to server A or the hub |
| swak-tak-ldap | 10.89.77.12 | OpenLDAP from `tools/tak-server/ldap`, or Samba Active Directory from `tools/tak-server/ad` |
| swak-tak-hub | 10.89.77.13 | The federation hub policy service, plugin manager, broker, UI, and SWAK test plugins, only in hub mode. The UI listens on loopback port 19100 |
| swak-tak-hub-db | 10.89.77.14 | MongoDB 6.0 for the hub replay, only with `--hub-disruption` |
| swak-tak-db, swak-tak-db-b | 10.89.77.20, 10.89.77.21 | The databases of server A and server B |

The host gets only loopback ports. Server A publishes 18088 (plain TCP CoT), 18089 (TLS CoT), 18090/udp (QUIC), 18443 (REST API with client certificates), and 18446 (enrollment with a user name and password). Server B publishes 28089, 28443, and 28446. The databases and the directory stay private.

Run the stages from the repository root:

```sh
python3 tools/tak_server.py fetch
python3 tools/tak_server.py build
python3 tools/tak_server.py certs
python3 tools/tak_server.py up
cmake --build --preset debug --target swak_tak_compat --parallel 4
python3 tools/tak_server.py test
python3 tools/tak_server.py load --seconds 600 --rate 10
python3 tools/tak_server.py load --seconds 120 --loss 5 --delay 50 --client quic
python3 tools/tak_server.py down
```

- `fetch` clones the pinned source and adds the local release tag that the TAK build reads.
- `build` builds the TAK Server war and the schema manager without the web dashboards. Gradle keeps its cache under `build/tak-server/gradle-home`.
- `certs` creates a private test CA, the keystores of both servers and the hub, the directory certificate, and the client certificates `left`, `right`, and `admin`. Each server certificate names its container and address. The intermediate CAs `ca-a` and `ca-b` under the root sign the certificates of server A and server B, so the hub sees two CA groups. Each keystore holds the CA chain after the server certificate, because federation and the hub read the CAs from that chain. The root signs the client and hub certificates. The credentials expire after seven days.
- `up` builds the directory image, starts the databases and the directory, applies the TAK schema, and starts the configuration, messaging, and API services of both servers from `tools/tak-server/start.sh`. It fills `tools/tak-server/CoreConfig.xml` for each server. It writes the user files with BCrypt from the TAK build. When both servers list the federate, it shares the group `SwakTest` in both directions through `/api/federategroups`. `--directory openldap` (the default) or `--directory ad` selects the directory of server B. Both serve LDAPS only, and TAK Server checks them with the test CA. `--federation direct` (the default) connects server B to server A. `--federation hub` starts the hub and connects both servers to it. The hub policy has the CA groups of server A and server B, which are not interconnected. The edge from A to B lets only the group `SwakTest` through. The edge from B to A lets every group through. Both servers turn on federated group mapping, so each event carries its group names. Both servers send `SwakTest` and `Ops`, and both map the received names to their local `SwakTest`. The hub also runs the test plugin `tools/tak-server/hub/SwakHubPlugin.java` in three roles. The interceptor renames events with the UID suffix `-PLUGIN` to the callsign `SWAK HUB PLUGIN`. The sender sends the event `SWAK-HUB-PLUGIN-SENDER` every two seconds. The receiver takes messages only from the CA group of server A and writes their UIDs to `build/tak-server/hub/logs/plugin-receiver-events.log`. Server A connects to the hub with its certificate on port 9102. Server B connects without a client certificate to the token port 9103 with an automatic token, which the hub issues because the CA group of server B has `allowTokenAuth`. After the probe, `test` runs `tools/tak-server/hub/HubTokenProbe.java` in the hub container and adds its checks to the report.

The hub UI admits the test administrator by CN and SHA-512 fingerprint in `authorized_users.yml`. When the hub starts, `up` asks the UI at `/api/generateJwtToken` for two manual tokens of the token group `SWAK_token_group`. The policy lists only the first one, and `up` activates the policy at `/api/activateFederationPolicy/swak`. The token group has the same edges as the CA group of server B. `--hub-token manual` connects server B with the listed token instead of an automatic token.

`--hub-disruption` starts MongoDB and turns on the hub replay. The hub then keeps federated mission changes and sends them again when a federate opens its mission stream. With replay, the hub closes that stream after the replay, and TAK Server does not open it again, so live mission changes stop until the next connection. For this reason, replay is off by default.
- `test` writes `build/tak-server/profile.json` and runs the probe in the rootless network namespace of Podman (`podman unshare --rootless-netns`). There the probe reaches the container addresses and the bridge, so it can use multicast. Use `--loopback` to run the probe on the host with the published ports and without the multicast checks. Use `--preset sanitize` to run the probe under AddressSanitizer and UndefinedBehaviorSanitizer. The report is `build/tak-server/report-<preset>.json`, or `report-<preset>-loopback.json`.
- `load` runs the probe with a load profile instead of the feature checks. It takes `--seconds`, `--rate` (positions per second), `--objects` (distinct UIDs), `--client tls` or `--client quic` for the sender, and `--minimum` for the delivery ratio. `--loss` (percent) and `--delay` (ms) add netem rules on the bridge and on the interface of server A, so both directions lose packets. The run removes the rules at its end. The report is `build/tak-server/load-<preset>-<client>.json`.
- `status` shows the containers and ports. `down` removes the containers and the network but keeps the build and the credentials.

The test users on server A are `swak-left` and `swak-right` with certificates, `swak-ops` with a certificate in the group `Ops` only, also on server B, `swak-admin` with the admin role, and `swak-enroll` with a password for enrollment. Server B has `swak-admin` in its user file. OpenLDAP has `swak-ldap` in the groups `SwakTest` and `SwakLdap`. Active Directory has `swak-ad` in the groups `SwakTest` and `SwakAd`. The probe sends synthetic objects with short lifetimes and creates and deletes a test mission, checklist template, and video connection. It also uploads a test iconset.

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
| excheckTemplate, excheckTemplateList, excheckStart, excheckLoad, excheckTaskUpdate | ExCheck | A posted template, its list entry, a started checklist, its tasks, and a task set to Complete and read back |
| citrapCreate, citrapList, citrapUpdate | Citrap | A report with an attachment, the list, and a status change |
| videoCreate, videoList | VideoSources | A connection from `/video` |
| dataFeeds, mapLayers | readServerFeeds, readServerMapLayers | The catalogs parse |
| history | TrackHistory | Past positions of the left client |
| iconsetUpload, iconsetIcon | IconCache | The admin uploads an iconset, then the left client gets its icon |
| quicConnect, quicExchange | Servers (QUIC) | A QUIC stream by host name, and CoT to and from a TLS client |
| enrollment, enrolledConnection | TakEnrollment | A signed certificate, then a TLS stream with it |
| multicastJoin, multicastSend, multicastReceive | Servers (mcast) | SWAK sends to the TAK input 239.2.3.1:6969 and receives from the TAK static subscription 239.2.3.2:6970 |
| ldapEnrollment, ldapConnection, ldapGroups | TakEnrollment, TakChannels | The directory user enrolls on server B, connects, and gets the directory groups |
| federationInbound, federationOutbound | Servers | Server A delivers an event from the server B client, and server B delivers an event from the left client, directly or through the hub |
| federationGroupFilter | Servers | Server B delivers a `SwakTest` event. Through the hub, it does not deliver an `Ops` event in 12 seconds. In direct federation, it delivers the `Ops` event, so the servers do not filter it |
| federationGroupReverse | Servers | Server A delivers an `Ops` event from server B. Through the hub, this needs the separate edge from B to A |
| missionFederation | Missions | Server A creates a mission for `SwakTest`, and SWAK adds an object to it. As the directory user on server B, SWAK subscribes to the federated mission and loads the object, directly or through the hub |
| hubTokenFederation | TakServerApi | Through the hub only: the outgoing connection of server B uses a token on port 9103 and is connected |
| hubTokenForged, hubTokenNotAllowed, hubTokenIssued, hubTokenTampered, hubTokenWithoutKey | HubTokenProbe | Through the hub only: the hub refuses a forged token, issues no token for the CA group of server A, issues a token for server B that it accepts, and refuses that token with one changed signature character. hubTokenWithoutKey records that the hub issued the token for a posted certificate |
| missionDisruption | Missions | SWAK turns off the outgoing connection of server B through its REST API, adds an object to a new mission on server A, and turns the connection on again. Server B must then have the object. In direct federation, server A sends the missed change. Through the hub, server A stays connected, so the check runs only with `--hub-disruption` |
| hubReplayStopsLiveMissions | Missions | With `--hub-disruption` only, in place of missionFederation: a live mission change does not reach server B after the replay. The check fails when TAK Server fixes this fault |
| hubManualToken, hubManualTokenUnlisted | HubTokenProbe | The hub accepts the UI token that the policy lists, and refuses the valid UI token that it does not list |
| hubDisruptionStored | `tak_server.py` | With `--hub-disruption` only: MongoDB holds federated mission changes in `cot.federate_event` |
| hubPluginIntercept, hubPluginSender, hubPluginReceiver | Servers | Through the hub only: server B gets a `-PLUGIN` event with the interceptor callsign, both servers get the sender event, and the receiver file has the event from server A but not the delivered event from server B |

The multicast checks run only in the bridge mode of `test`. The load report records sent and delivered positions, the delivery ratio, repeated positions, disconnects, queue drops, latency percentiles, and the resident memory of the probe. The load passes when at least 99 percent of the positions arrive, without a repeated position or a disconnect.

# Results, October 9, 2026

All transport stages and all 48 feature checks passed against TAK Server 5.7-RELEASE-14. The loopback mode passed its 45 checks. The run under AddressSanitizer and UndefinedBehaviorSanitizer also passed without a report. A 30-minute load at 20 positions per second with 50 UIDs delivered all 36000 positions without a repeat, a disconnect, or a queue drop. The median latency was 21 ms, the 95th percentile 40 ms, and the maximum 71 ms. The probe memory grew from 43.3 MiB to 43.9 MiB.

All 48 checks also passed with OpenLDAP over LDAPS, with Samba Active Directory over LDAPS, and with federation through the hub. With the group filter and plugin checks, direct federation passes 52 checks, the hub passes 62, the hub with `--hub-token manual --hub-disruption` passes 64, and the loopback mode passes 49. The hub counts include the HubTokenProbe checks. In direct federation, server B had the change from its disconnected time after 7 to 9 seconds. Through the hub with replay, it had it after 9 seconds. The runs under AddressSanitizer and UndefinedBehaviorSanitizer passed in direct and hub mode. Two-minute loads at 10 positions per second with 5 percent loss and 50 ms delay in both directions delivered all 1200 positions without a disconnect:

| Sender | Median latency | 95th percentile | Maximum |
| --- | --- | --- | --- |
| TLS | 232 ms | 910 ms | 2551 ms |
| QUIC | 130 ms | 340 ms | 1140 ms |

With 20 percent loss and 100 ms delay, TCP throughput collapses. In 90 seconds at 5 positions per second, TLS delivered 289 of 450 positions within the 10-second tail (median 11 s), and QUIC delivered 425 (median 1.3 s). Neither client disconnected. These runs do not pass the 99 percent minimum, and they are a characterization only.

The first runs found these SWAK faults, which are now fixed:

- Data packages used stored zip entries with data descriptors. Java ZipInputStream, which TAK Server and ATAK use, rejects them. SWAK now writes deflated entries.
- Enrollment rejected the signed certificate, because TAK Server wraps base64 text at 64 characters. SWAK now removes the line breaks.
- `/api/video` answers JSON, not XML. SWAK now reads both.
- CI-TRAP needs a zip package with the report XML to create a report, raw attachment bytes with a Content-Disposition file name, a WKT `POINT(longitude latitude)` location, and `userDescription` as an attribute. The search answers JSON. SWAK now uses this format.
- ExCheck needs `clientUid` on the active list and `clientUid` and `startTime` to start a checklist.
- ExCheck task status uses the XML values `Pending`, `Complete`, `Complete (late)`, and `Late`. SWAK sent the enumeration names, such as `COMPLETE`, and the server stored no status. SWAK now sends the XML values. It also writes `status` and `completeDTG` in the order of the task schema.
- `/api/iconimage` needs the admin role. SWAK now gets iconset icons from `/api/icon/<iconset UID>/<group>/<name>`, which every user can read.
- A QUIC entry with a host name never connected. SWAK polled the stream before the name lookup ended. SWAK now waits for the lookup, tries IPv4 addresses first, and tries the next address after five seconds.

TAK Server behavior that the probe works around:

- TAK Server processes an iconset upload in the background and caches a missing icon. A client that asks before the processing ends gets HTTP 404 until the cache entry expires. The probe waits until `/api/iconseturl/<uid>` lists the icon.
- Rootless Podman cannot route multicast between the host and the containers. The probe joins the groups on the bridge address 10.89.77.1 in the rootless network namespace.
- TAK Server drops an event that arrives before it registers the receiving subscription. With added delay, this lost the first exchange. The probe now repeats its positions every 3 seconds, as ATAK does.
- Samba cannot provision in a rootless container with its defaults. The image lowers the ID map start to 20000 and keeps NT ACLs in a TDB file. The Samba controller also refuses simple binds without TLS, as AD does.
- The hub names a CA group by the issuer DN and the lowercase SHA-256 fingerprint of the CA certificate.
- TAK Server puts group names on a federated event only when the federate has `federatedGroupMapping`. Without it, the hub drops every event on an edge with a group filter. Interconnected groups ignore edge filters.
- A TAK Server drops a second connection to the same hub as a duplicate before it sends a token, also with `allowDuplicate`. So one TAK Server cannot test a refused token next to a working connection. HubTokenProbe tests the refusals directly.
- The token port does not ask for a client certificate. `GetAuthTokenByX509` issues a token to any client that posts a certificate from a CA group with `allowTokenAuth`. The client does not have to prove that it holds the private key, and server certificates are not secret. Treat `allowTokenAuth` as permission for anyone who has a certificate of that CA. Give token CA groups only the groups that such a client may receive.
- A health check with a valid token and no event stream gets `NOT_CONNECTED` and stays open, so a blocking client waits until its deadline.
- Without a cluster, each TAK Server process keeps missions in a local Caffeine cache for `caffeineFileCacheSeconds`, which is 120 seconds by default. The messaging process adds content that arrives on the CoT stream or from a federate, but the API process answers `/api/missions/{name}/cot`. In 4 of 18 runs, a mission read showed no object for at least one minute after the object arrived, and the database already held it. In the run that waited longer, server B showed the object after 122 seconds, just after the cache time. Most runs needed 1 to 3 seconds. The mission checks now wait 150 seconds and report the time that they needed. A SWAK user can see the same delay after adding an object to a mission.
- Mission federation needs federated group mapping on the TAK federates, or edges that let all groups through, because a mission change carries the groups of the mission only with mapping. The receiving server creates the mission from the first change. Mission federation does not need MongoDB in the hub. MongoDB stores changes only for hub disruption tolerance.
- With `missionFederationDisruptionEnabled`, the hub replays the stored changes when a federate opens its mission stream, and then completes that stream, also after an empty replay. TAK Server logs `received server onCompleted` and does not open the stream again. Live mission changes to that federate stop until it connects again. The broker also tests MongoDB only once at start, so MongoDB must answer before the broker starts.
- The hub UI needs the policy service in the same container for its Ignite client. It signs manual tokens with its own keystore, which must be the broker keystore. A policy activation reads `policies/<name>.json` before `ui_generated_policy.json`, so `up` removes old policy files.
- The plugin SDK sets the plugin type on the metadata object that `getMetadata` returns. A plugin that returns a new object on each call registers without a type, and the hub never calls it.
- The hub fails on a sender plugin without a policy entry and on a plugin entry without its four group lists. It also sends messages to a disabled interceptor when another plugin is enabled. The test policy avoids these cases. The repository has no sample plugin.

These results establish compatibility with this server version and configuration. A Windows Active Directory server, hub OAuth sign-in, and hub replay of mission files remain outside the probe. The GUI tests cover the panes through local fixtures.

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

The probe remains outside the release executable and installation. It requires no new runtime dependency. It does not replace GUI rendering tests or tests against your deployment's authentication policy.

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
