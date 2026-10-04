# CoT emulator

The separate emulator sends two simulated teams across the map. It uses the same CoT encoder and TAK connection code as SWAK. The tool requires no new dependency. Release builds and installation exclude it.

Each team starts with six infantry units, three vehicles, one helicopter, and one fixed-wing aircraft. Alpha uses Blue and friendly affiliation. Bravo uses Red and hostile affiliation. The default center is Stockholm at latitude 59.33 and longitude 18.0.

Units follow circular patrol routes in opposite directions. Ground units use the configured radius. Air units use twice that radius. Each unit retains its UID, which identifies the same object across messages. Each message contains a callsign, type, team, role, position, altitude, UTC times, and position uncertainty. The Location method is m-s for simulated data.

These routes do not follow roads or terrain. Altitude stays constant above the WGS84 ellipsoid. The tool does not model terrain elevation, flight dynamics, or tactical decisions.

# Start a local test

Use the build environment in [Build and run](build.md). Run these commands from the repository root:

```sh
cmake --preset debug
cmake --build --preset debug --target swak_cot_emulator --parallel 2
./build/debug/swak_cot_emulator
```

In SWAK, open Servers through Tools > Plugins. Add an enabled TCP server with host 127.0.0.1 and port 18087. Center the map at latitude 59.33 and longitude 18.0. Use zoom level 11 or 12 to see both teams. The emulator accepts up to eight TCP clients.

Press Ctrl+C in the emulator terminal to stop transmission. The last position becomes stale after 15 seconds by default. SWAK then uses its configured stale fade and removal behavior.

For a five-minute test with five updates per second per unit, run:

```sh
./build/debug/swak_cot_emulator --frequency 5 --duration 300
```

For UDP, add an enabled UDP listener in SWAK on port 18087. Run:

```sh
./build/debug/swak_cot_emulator --udp --address 127.0.0.1 --port 18087
```

The direct modes accept numeric IPv4 or IPv6 addresses. TCP binds only to loopback by default. An explicit wildcard TCP address exposes the listener on available interfaces. UDP rejects wildcard, multicast, and the limited broadcast address.

# Change the scenario

Save the default scenario from the repository root:

```sh
./build/debug/swak_cot_emulator --print-defaults > /tmp/swak-scenario.json
```

Edit that JSON file. Then run:

```sh
./build/debug/swak_cot_emulator --scenario /tmp/swak-scenario.json
```

The top-level fields control the scenario:

| Field | Meaning | Supported values |
| --- | --- | --- |
| latitude | Center latitude in degrees | -80 to 80 |
| longitude | Center longitude in degrees | -180 to 180 |
| frequency | Updates per second per unit | 0.1 to 20 |
| lifetime | Seconds before each update becomes stale | Integer 1 to 3600, longer than the update interval |
| radius | Ground patrol radius in meters | 100 to 50000 |
| separation | Distance between team centers in meters | 0 to 100000 |
| seed | Repeatable route seed | Integer 0 to 4294967295 |
| teams | Team definitions | Exactly two objects |
| types | Unit definitions shared by both teams | One to sixteen objects |

Each team contains name, team, and affiliation. The team field carries a TAK color name. The affiliation field carries one CoT affiliation letter, such as f or h. Each unit definition contains name, type, count, speed, and altitude. The type is a ground or air suffix, such as G-U-C-I or A-M-H. The emulator combines it with each team affiliation.

Count applies separately to each team. Zero disables that unit type. Each team must retain at least one unit. Each type supports up to 512 units per team. The total limit is 1024 units and 2000 messages per second. Speed uses meters per second from 0 to 400. Altitude uses meters from -500 to 20000.

Missing top-level fields retain defaults. A supplied teams or types array replaces the whole array. Unknown top-level fields and invalid values stop startup. JSON files cannot exceed 16 KiB.

The --frequency, --latitude, and --longitude flags override file values. The --duration flag sets a limit of up to 86400 seconds. Zero, the default, means continuous operation. Run --help to see the flags.

Each run uses a random UID prefix. For repeatable identities, supply --session with a distinct prefix. Do not reuse that prefix for simultaneous runs because matching UIDs update the same objects. A fixed seed produces the same routes and initial positions. Wall-clock timestamps and timer scheduling still vary between runs.

# Send through TAK Server

Create a JSON file that contains one server configuration:

```json
{
  "protocol": "tls",
  "host": "localhost",
  "port": 18089,
  "ca": "/path/to/test-ca.pem",
  "certificate": "/path/to/client.pem",
  "key": "/path/to/client.key"
}
```

Use certificate paths and a server that trusts the test identity. For PKCS12, use the archive path as certificate and omit key. For an encrypted archive, set passwordEnv to a dedicated SWAK_COT_ environment variable. Keep passwords and private key contents out of the JSON file.

Run this command from the repository root:

```sh
./build/debug/swak_cot_emulator --tak-profile /path/to/server.json --duration 300
```

Connect SWAK to the same server with an identity that can receive the test group. The profile uses the shared Servers configuration, including certificate trust and reconnect behavior. Plaintext tests can use protocol stcp. The emulator does not combine a TAK profile with direct address, port, or UDP flags.

# Resource limits and results

The scheduler spaces messages across each update interval. It generates at most sixteen messages per timer callback. It skips overdue samples instead of building a growing backlog. Each TCP client has a 256 KiB output limit. A slow client disconnects when it reaches that limit. The shared TAK client also bounds its output queue.

The terminal shows startup and connection state. On normal termination, standard output contains a JSON report. Generated counts samples. Queued counts accepted sends and counts each TCP recipient separately. Dropped counts failed sends. ScheduleSkipped counts missed samples. SlowClients counts disconnected slow clients. A queued send does not prove remote delivery.

The tests cover routes, identities, limits, invalid input, TCP reception through SWAK, UDP, a TCP destination, interruption, and map rendering. Run the simulation suite after a debug build:

```sh
ctest --preset debug -R simulation
```

Read [Development diagnostics](quality.md) for sanitizer runs. Read [TAK compatibility](tak-compatibility.md) for the separate test against an actual TAK Server.
