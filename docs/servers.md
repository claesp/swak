# CoT connections

SWAK receives CoT XML from multiple server connections. Open Tools > Servers, File > Settings > Servers, or the Servers pane. Add creates an empty entry. Edit and Delete affect the selected row. Each Active checkbox controls one connection. Names follow the system alphabetic order.

The browser replaces the UDP receiver. The application removes the automatic Legacy UDP input entry. Explicit UDP entries remain available. Its stable component and pane identifiers remain `org.swak.udp` and `org.swak.udp.pane`. Unsupported saved protocols remain in the profile and do not connect.

TLS is the default protocol. The default port is 8089. Certificate validation remains required.

Set the CA certificate path for a private server authority. Set the client certificate path for certificate authentication. PEM certificates require a separate PEM private key. PKCS12 files contain the certificate and key together.

For an encrypted key or PKCS12 file, set Password environment variable to a dedicated `SWAK_COT_` name. Set that variable before you start SWAK. The profile stores the variable name, not its value. Certificate files remain external to the profile. Update their paths after you move a profile to another computer. Use trusted profiles because they choose credential destinations.

TCP CoT stream supports explicit plaintext connections. The TAK Server example uses port 8088 for `stcp`. UDP input listens on the configured local address and port. It does not connect to a remote UDP server. These transports do not authenticate received messages.

Each stream retries after a failure. Repeated failures increase the retry delay to at most 60 seconds. Connection and incomplete-message timeouts share a configurable limit. Idle streams reconnect after 90 seconds without input. A control heartbeat runs every 30 seconds. CoT control messages do not become map objects.

# Identity and transmission

File > Identity opens the sender configuration. The same fields appear in File > Settings > Identity. The UID identifies the sender across messages. Callsign supplies its readable name. Type and Location method supply CoT `type` and `how`. Team and Role supply the `__group` detail extension.

The generator uses this identity when it opens and when the identity changes. Use sender identity restores those defaults after draft edits. Position and lifetime remain generator fields. New identity in the generator creates a separate draft UID. It does not replace the saved sender identity.

Generate event normally adds a local event. Enable Send generated CoT to connected servers and LAN peers to transmit it. Only connected TCP, TLS, and established LAN sessions receive generated messages. UDP inputs remain receive-only. SWAK never forwards received events automatically. This rule prevents forwarding loops across servers and peers.

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

This implementation does not provide TAK account enrollment, LDAP authentication, mission APIs, QUIC, federation, or binary-only CoT endpoints. The local harness tests protocol behavior without a full TAK Server deployment. A deployment test against a real TAK Server remains separate.

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
