# CoT connections

SWAK receives CoT XML from multiple server connections. Open Tools > Servers, File > Settings > Servers, or the Servers pane. Add creates an empty entry. Edit and Delete affect the selected row. Each Active checkbox controls one connection. Names follow the system alphabetic order.

The browser replaces the UDP receiver. The application removes the automatic Legacy UDP input entry. Explicit UDP entries remain available. Its stable component and pane identifiers remain `org.swak.udp` and `org.swak.udp.pane`. Unsupported saved protocols remain in the profile and do not connect.

TLS is the default protocol. The default port is 8089. Certificate validation remains required.

Set the CA certificate path for a private server authority.

# Certificate enrollment

TAK Server can issue a client certificate to a user with a TAK account. Select a TLS entry and then TAK Server > Enroll .... Enter the username and password of the account. The enrollment port is normally 8446. Choose Create a new key, or Use an existing PEM key file. Then select Enroll.

SWAK reads the required certificate subject from `/Marti/api/tls/config`. It builds a certificate request with CN set to the username and exactly the required entries, such as O and OU. It signs the request with SHA-256 and sends it to `/Marti/api/tls/signClient/v2`, with the sender UID and the SWAK version. Both requests use HTTP Basic authentication over HTTPS. They trust only the CA of the entry, or the system CAs when the entry has none. Set the CA first when the server uses a private authority.

A new key is an RSA 2048-bit key. SWAK checks that the signed certificate matches the key. It saves the files in a private folder under the application data directory, `certificates/<entry>`. The folder allows access only by the owner. `client.key` holds a new key, `client.pem` holds the signed certificate and its chain, and `ca.pem` holds the returned CA certificates. The entry then uses these files. An entry without a CA also receives `ca.pem`. An existing key file stays where it is.

When the entry names a password variable, that password encrypts a new key with AES-256 and decrypts an existing key. Without a password variable, the new key file is not encrypted and relies on the private folder. The account password is used for the two requests only and is never saved. The enrollment data package, with server preferences, is not read yet.

TAK Server inputs with `auth="file"` or `auth="ldap"` expect a username and password before the first CoT event. Set TAK username and TAK password environment variable for such a server. The variable name starts with `SWAK_COT_`. Set it before you start SWAK. After the TLS handshake, SWAK sends an `auth` element with the username, password, sender UID, and callsign. A username needs TLS, so the password never crosses the network in plain text. An unset variable stops the connection with a reason. A change of sender identity reconnects only the entries with a login.

TLS entries also use the TAK Server REST API. TAK Server serves it on port 8443 under `/Marti/api`. TAK REST API port sets the port. Set it to 0 to turn the API off for the entry. Each request uses the same CA and client certificate as the CoT stream. After each new TLS connection, SWAK reads `/Marti/api/version/config`. The server status then shows a line such as `TAK Server 5.7.14-RELEASE, REST API 3.` A failed check shows its reason and repeats after five minutes while the stream stays connected. Later TAK features use this API. Set the client certificate path for certificate authentication. PEM certificates require a separate PEM private key. PKCS12 files contain the certificate and key together.

For an encrypted key or PKCS12 file, set Password environment variable to a dedicated `SWAK_COT_` name. Set that variable before you start SWAK. The profile stores the variable name, not its value. Certificate files remain external to the profile. Update their paths after you move a profile to another computer. Use trusted profiles because they choose credential destinations.

TCP CoT stream supports explicit plaintext connections. The TAK Server example uses port 8088 for `stcp`. UDP input listens on the configured local address and port. UDP output to a server sends generated CoT to a server UDP input, such as TAK Server port 8087. It needs the numeric address of the server. It sends one event per datagram and reads nothing. These transports do not authenticate messages, and UDP gives no delivery guarantee.

Each stream retries after a failure. Repeated failures increase the retry delay to at most 60 seconds. Connection and incomplete-message timeouts share a configurable limit. Idle streams reconnect after 90 seconds without input. A control heartbeat runs every 30 seconds. CoT control messages do not become map objects.

# Identity and transmission

File > Identity opens the sender configuration. The same fields appear in File > Settings > Identity. The UID identifies the sender across messages. Callsign supplies its readable name. Type and Location method supply CoT `type` and `how`. Team and Role supply the `__group` detail extension.

The generator uses this identity when it opens and when the identity changes. Use sender identity restores those defaults after draft edits. Position and lifetime remain generator fields. New identity in the generator creates a separate draft UID. It does not replace the saved sender identity.

Generate event normally adds a local event. Enable Send generated CoT to connected servers and LAN peers to transmit it. Only connected TCP, TLS, UDP output, and established LAN sessions receive generated messages. UDP inputs remain receive-only. SWAK never forwards received events automatically. This rule prevents forwarding loops across servers and peers.

# TAK Protocol version 1

TAK Server can switch a stream from CoT XML to TAK Protocol version 1, a smaller binary format. The server offers the switch with a `t-x-takp-v` event after the connection and any login. By default, SWAK answers with a `t-x-takp-q` request for version 1. After a positive `t-x-takp-r` response, both directions use binary frames. TAK message format controls this for TLS, TCP, and QUIC entries. Select XML only to keep XML. Without an offer, the stream stays XML. The status then shows `Connected, TAK Protocol 1`.

A binary frame is the byte 0xbf, a length, and one message. SWAK keeps detail elements that have only the typed values, such as contact, group, track, status, device, and position source, in their binary fields. All other detail elements travel as XML text inside the message, as TAK expects. SWAK checks each received message with the same XML decoder and limits as an XML event. An invalid frame stops the connection, and the normal retry applies.

# QUIC

TAK Server 5.7 adds a QUIC input, normally on port 8090. Select QUIC CoT stream (TAK Server 5.7) as the protocol. QUIC uses the same CA, client certificate, key, password variable, TAK login, and REST API port as a TLS entry. SWAK uses the OpenSSL QUIC client with the ALPN name `takstream`. It checks the server certificate against the host name or address. After the handshake, SWAK sends a ping, because the client opens the single TAK stream with its first write. The stream then carries CoT XML like a TLS stream, with the same limits, timeouts, and pings. QUIC uses UDP, so a firewall must allow UDP to the server port.

# Multicast mesh

ATAK and WinTAK share positions without a server through multicast. Add a server with the protocol Multicast mesh (ATAK). Select an IPv4 interface. For situational awareness, use group 239.2.3.1 and port 6969. For GeoChat, add a second entry with group 224.10.10.1 and port 17012.

The entry joins the group on the selected interface. It receives CoT XML datagrams and sends generated CoT when sending is enabled. Datagrams use a time-to-live of one, so they stay on the local network segment. SWAK does not receive its own datagrams. Multicast mesh uses plaintext without authentication. Use it only on a trusted network. ATAK can also send TAK Protocol version 1 datagrams, which start with 0xbf, the version 1, and 0xbf. SWAK decodes them and counts them in the status. SWAK sends XML on the mesh, which ATAK also accepts.

# Channels

TAK Server groups decide which users exchange data. TAK clients call them channels. Select a TLS or QUIC entry with a TAK REST API port, then TAK Server > Channels .... SWAK reads the groups of your user from `/Marti/api/groups/all?useCache=true`. Each channel appears once, even when the server lists it for both directions. Check the channels that should send and receive data, and select OK. SWAK sends the whole list with the new active flags to `/Marti/api/groups/active`, with your sender UID. TAK Server applies the choice to every client of the same user. A server that does not allow the change answers with an error, which SWAK shows below the buttons.

# Contacts

Tools > Plugins > Contacts shows a pane with the TAK users that SWAK can address. A position report from a server entry with a `contact` endpoint adds or updates a user. Local events and objects without a contact endpoint are not users. Refresh reads `/Marti/api/contacts/all` from each enabled entry with a TAK REST API port. These users stay in the list in gray, without a current position, until they report one. An error names the entry and keeps its last list. Double-click a user, or select Show on map in the context menu, to center the map on the user and select it. The list holds at most 2048 users and updates at most once a second.

# Share objects

SWAK sends a local object to TAK users only when you share it. Right-click a marker, circle, line, area, or route on the map, or open its Information menu. Select Share to send it to the channels of your user. Select Share with ... to send it only to chosen users from Contacts. TAK Server reads the `marti` element with one `dest` callsign for each user. Multicast groups, UDP outputs, and LAN peers ignore this element and send to all receivers.

| SWAK object | TAK CoT |
| --- | --- |
| Marker | `b-m-p-s-m` spot marker with `color` and `usericon` |
| Military marker | Its CoT unit type, such as `a-f-G-U-C-I` |
| Circle | `u-d-c-c` with `shape/ellipse` and the radius in meters |
| Line | `u-d-f` with one `link` point for each end |
| Area | `u-d-f` with the first point again at the end and a `fillColor` |
| Route | `b-m-r` with `b-m-p-w` check point links and `link_attr` |

A shared object stays shared. SWAK sends each change, and it sends a `t-x-d-d` delete with `__forcedelete` when you delete the object. Undo shares the object again. Stop sharing sends the delete and keeps the object local. Users that you remove from Share with ... get a delete. Shared objects are stale after 24 hours, so SWAK sends all of them again every 15 minutes and after each start. When no output is connected or the outgoing queue is full, SWAK tries again every 5 seconds and logs the reason once. SWAK never sends received events.

Tools > Plugins > Sharing > Share New Objects Automatically, or the same setting in File > Settings > Sharing, shares each new local object with your channels. It is off by default. Objects that SWAK restores at start are not new. A MIL-STD-2525 marker uses its CoT unit type, so TAK clients show the same symbol. They ignore the marker color.

# Received drawings and routes

SWAK draws the circles (`u-d-c-c`), ellipses (`u-d-c-e`), lines and areas (`u-d-f` and `u-d-f-m`), rectangles (`u-d-r`), and routes (`b-m-r`) that TAK clients share. It uses the stroke and fill colors, the stroke weight from 1 to 10, and the route color from `link_attr`. Closed `u-d-f` shapes and rectangles are areas. A route shows its check points when the route is large enough on screen. The label shows the callsign. These drawings are read-only. A click on the outline, or inside a circle or area, selects the drawing and shows its information. Drawings with more than 1000 points, invalid points, or truncated detail do not show.

Tools > Plugins > TAK Drawings > Show TAK Drawings turns the drawings on and off. File > Settings > TAK Drawings also sets their opacity. The CoT layer draws no point symbol for these types. It draws a received spot marker (`b-m-p-s-m`) in the color of its `color` element.

# Chat

Tools > Plugins > Chat shows the GeoChat pane. It has All Chat Rooms, a room for your team from File > Identity, and one room for each direct chat. A room with unread messages shows in bold with the count. Type a message and press Enter or select Send. New Chat starts a direct chat with a user from Contacts. Chat with in the Contacts menu does the same and shows the pane. Right-click a message and select Copy to copy its text.

SWAK sends GeoChat in the ATAK format: a `b-t-f` event with `__chat`, `chatgrp`, a link to the sender, and the text in `remarks`. A direct message also has a `marti` destination, so TAK Server sends it only to that user. Team messages use the parent `TeamGroups`. SWAK shows only messages for All Chat Rooms, your team, and direct messages to your UID. Multicast peers also receive the chats of other teams and other users, and SWAK ignores them.

Your own messages show their state. Waiting for a connection means that no output is connected. SWAK keeps at most 64 waiting messages and tries again every 5 seconds. Sent means that an output took the message. Stored by the server (`b-t-f-p`), Delivered (`b-t-f-d`), and Read (`b-t-f-r`) come from receipts. A state never moves back. SWAK sends a delivered receipt when a direct message arrives and a read receipt when you open the room.

TAK Server can store messages for offline users and send them after the connection starts. SWAK removes duplicates by message ID and keeps each room in time order. Chat events do not go to the map or to CoT Messages. Limits: 4096 characters for each message, 128 rooms, 500 messages for each room, and 4096 remembered message IDs. SWAK shows text as plain text and removes control and direction characters. History stays in memory and ends when SWAK closes.

# Emergency alerts

TAK clients send emergency alerts as CoT. SWAK recognizes 911 Alert (`b-a-o-tbl`), Ring The Bell (`b-a-o-pan`), Troops In Contact (`b-a-o-opn`), Geo-fence Breached (`b-a-g`), and the ATAK custom alert (`b-a-o-c`). An active alert has a red double ring and a label with its type and the callsign of the person in distress. The emergency button at the right end of the toolbar turns red while any alert is active. Its tooltip lists the alerts. Its menu centers the map on an alert and selects it. Each new alert adds one Warning to Events. TAK Server repeats active alerts, and SWAK ignores these repeats.

A cancel (`b-a-o-can`) with the same UID removes the alert, when it comes from the same server entry. SWAK keeps no marker for the cancel.

To send an alert, open the event editor. Set the latitude and longitude of your position. Select the alert type and then Send emergency .... SWAK asks for confirmation. Like ATAK, it uses your identity UID with `-9-1-1`, a link to your identity, the callsign with `-Alert`, and the `emergency` element that the TAK Server repeater checks. The alert stays active for 24 hours unless you cancel it. Cancel emergency sends the cancel for the same UID. SWAK sends both to all connected servers, UDP outputs, mesh groups, and LAN peers.

# Local LAN servers

Local LAN provides serverless discovery and direct CoT exchange. To add a Local LAN entry, do these steps:

1. Add a server.
2. Select Local LAN as its protocol.
3. Select an active IPv4 interface.
4. Enable the entry in the Active column.

A new profile contains no LAN entry. Local LAN uses plaintext without peer authentication. Use it only on a trusted local network.

SWAK sends a small broadcast every five seconds on UDP port 17421. The broadcast contains a protocol name, version, random session identifier, and TCP listener port. It contains no location, callsign, certificate, or password. Its IP time-to-live is one. Discovery accepts only addresses on the selected subnet. It rejects loopback, multicast, network, broadcast, and local addresses.

Two peers use their session identifiers to choose one connection initiator. The lower identifier connects to the higher identifier. Each side sends `SWAK-COT/1`, a space, its discovery identifier, and a newline. Each side compares the greeting with recent discovery data before it accepts CoT. The greeting prevents accidental protocol mixing. It does not authenticate a peer.

Discovery records expire after 20 seconds. Each LAN entry retains at most 16 discovered peers. The client allows at most 16 connected peers across all LAN entries. A peer connection uses the same XML limits and timeout rules as a server connection. Multiple clients on the same IP address do not discover each other. Local LAN does not support IPv6 discovery, routed discovery, relay forwarding, or automatic firewall changes.

# CoT movement and expiration

CoT UID determines whether a message updates an existing marker. Movement uses a fixed duration, so longer moves have higher speed. A new update starts from the current displayed position. Crossing the map date line uses the shorter wrapped path. Large jumps use a fade-out and fade-in instead of a flight across the map.

File > Settings > CoT animation controls movement duration, teleport distance, fade duration, and gray retention. Defaults are 600 milliseconds, 100 kilometers, 600 milliseconds, and 10 seconds. A stale event leaves the live message table. Its map symbol fades to gray, remains for the retention period, then fades away. Stale symbols are not selectable. A fresh update with the same UID restores its active appearance.

# Development harness

The harness uses the Python standard library. SWAK does not install it or link it into the released application. Run it from the repository root. Add a TCP CoT stream for `127.0.0.1` and port 8088 in SWAK.

```sh
python3 tools/cot_harness.py --port 8088 --rate 1000 --count 12000 --contacts 500
```

Use `--fragment 7 --bad-every 100` to split writes and insert rejected messages. Use `--motion --contacts 1 --rate 20 --batch 1` to exercise movement and large jumps. After transmission ends, markers expire according to `--lifetime`. The harness keeps the connection open until that interval ends.

For TLS, supply `--cert` and `--key`. Use `--client-ca` to require a client certificate. Test credentials under `tests/fixtures` are public development fixtures. Never use them for a real server. The PKCS12 fixture password is `swak-test-only`.

The automated server suite tests fragmentation at every byte boundary, invalid XML, input limits, concurrent servers, reconnects, certificate rejection, certificate authentication, and load. Native GUI tests receive, draw, select, and transmit a CoT event. Motion tests use explicit times to exercise intermediate positions, teleport fades, stale retention, and fresh updates.

# TAK Server review

The reference is TAK Server commit `5187abd46d827d37cfc5708805eced197a837e49`. SWAK uses protocol information from this review. It contains no copied TAK Server implementation files.

The [example configuration](https://github.com/TAK-Product-Center/Server/blob/5187abd46d827d37cfc5708805eced197a837e49/src/takserver-core/example/CoreConfig.example.xml) defines TLS, streaming TCP, UDP, and multicast inputs. The [stream negotiation implementation](https://github.com/TAK-Product-Center/Server/blob/5187abd46d827d37cfc5708805eced197a837e49/src/takserver-core/src/main/java/com/bbn/marti/nio/protocol/connections/StreamingProtoBufOrCoTProtocol.java) starts in XML mode. A client request negotiates binary mode. SWAK remains in XML mode and does not request that switch.

The [streaming test client](https://github.com/TAK-Product-Center/Server/blob/5187abd46d827d37cfc5708805eced197a837e49/src/testing/load_test/pyTakStreamingCot.py) demonstrates TLS, CoT streams, and heartbeats. SWAK uses its own bounded XML frame reader. It handles quoted attributes, comments, and CDATA before the CoT decoder validates each complete document.

This implementation does not provide TAK account enrollment, LDAP authentication, mission APIs, QUIC, federation, or binary-only CoT endpoints. Read [TAK Server feature plan](tak-server-feature-plan.md) for the proposed client features. The local harness tests protocol behavior without a full TAK Server deployment. A deployment test against a real TAK Server remains separate.

# CoT field pickers

Select beside Type opens a category tree. Object types contain affiliation, dimension, and common function branches. Location method contains human and machine branches. Team contains the 14 common TAK colors. Role contains Team Member, Team Lead, HQ, Sniper, Medic, Forward Observer, RTO, and K9. Lists use the system sorting order.

The same controls appear in Identity, the generator, and their Settings sections. The generator saves its Location method, Team, and Role with its draft. Use sender identity restores all sender fields. Received messages remain read-only.

The Type tree contains common categories, not the complete MIL-STD-2525 function catalog. Custom text remains available for other codes. Opening or canceling a picker preserves the existing value. OK applies only a value that passes the existing CoT field rules. The picker does not add missing detail extensions that a specialized type requires.

The choices use protocol facts from ATAK commit 9f6893dd657feacc35ec5de03dad721c2e44170e. The [ATAK arrays](https://github.com/TAK-Product-Center/atak-civ/blob/9f6893dd657feacc35ec5de03dad721c2e44170e/atak/ATAK/app/src/main/res/values/arrays.xml) define common teams and roles. The [CoT hierarchy](https://github.com/TAK-Product-Center/atak-civ/blob/9f6893dd657feacc35ec5de03dad721c2e44170e/takcot/mitre/types.txt) describes type categories. The [base schema](https://github.com/TAK-Product-Center/atak-civ/blob/9f6893dd657feacc35ec5de03dad721c2e44170e/takcot/mitre/CoT%20Base-Event%20Schema%20%20(PUBLIC%20RELEASE).xsd) defines Location method categories. SWAK contains no copied source files or implementation code.

The [local TAK compatibility test](tak-compatibility.md) passed CoT XML exchange through official TAK Server 5.7-RELEASE-14. It covers TCP, PEM and PKCS12 mutual TLS, identity fields, position updates, reconnects, and certificate rejection. Other deployment features require separate tests.

# Connection details

The Servers pane shows full connection errors in a read-only text box. Select and copy the text when you report a failure. Errors include the protocol, address, port, cause, and retry delay. Connection refusal, host lookup failure, timeout, TLS failure, and UDP bind failure have separate messages.

For the local simulator, use TCP CoT stream at 127.0.0.1:18087. Start the separate emulator first. Its default listener does not accept TLS. Read [CoT emulator](cot-emulator.md) for remote connections and startup errors.

The Active column controls server connections. The editor preserves that state and does not show a second Connect checkbox. Select TLS to enable the certificate, private key, and password fields. TCP, UDP, and Local LAN keep those fields disabled without removing stored credentials.

# Interface selection and LAN status

Each server editor includes an Interface selector. It lists local addresses with their interface names. System default uses normal routing for TCP and TLS. For UDP, it uses the configured local bind address.

An explicit interface address binds the socket to that address. The operating system still controls routing. A missing address produces a connection error without fallback.

Local LAN requires an explicit, non-loopback IPv4 address. Its Host and Port fields remain disabled because discovery supplies peer addresses and ports. Separate LAN entries can use separate interfaces. Changing, disabling, or deleting an entry closes only its discovery sockets and peer sessions. Reconnect restarts all active entries.

Tools > Plugins > Local LAN opens the status pane. It shows the status of each LAN entry and a read-only table of discovered peers. Columns show the server, peer identifier, address, port, connection status, received messages, and rejected messages. The pane uses the same saved layout as other panes.

An active global LAN configuration from an older profile becomes a Local LAN server entry when the catalog has space. An inactive configuration creates no entry. The serverSchemaVersion field prevents repeated migration after the user deletes the new entry. The interface address remains visible as unavailable on a computer that does not have it. Select a replacement interface before you connect.
