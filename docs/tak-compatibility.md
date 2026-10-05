# TAK Server compatibility tests

The development probe uses SWAK's actual Servers, Store, XML parser, and encoder. It opens two client connections through a configured server. It sends synthetic CoT in both directions, checks identity fields and positions, updates the same UIDs, and reconnects one client. Its JSON report records the result. A nonzero exit code means that the test failed.

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
