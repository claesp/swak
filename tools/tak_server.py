#!/usr/bin/env python3
"""Builds and runs a disposable TAK Server for SWAK compatibility tests.

The deployment uses only loopback ports and test credentials under build/tak-server. It never
uses production data. Read docs/tak-compatibility.md for the stages and the probe.
"""

import argparse
import json
import os
import shutil
import socket
import ssl
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "build" / "tak-server"
SOURCE = WORK / "src"
RUN = WORK / "run"
RUN_B = WORK / "run-b"
CERTS = RUN / "certs" / "files"
TEMPLATES = ROOT / "tools" / "tak-server"
REPOSITORY = "https://github.com/TAK-Product-Center/Server.git"
COMMIT = "5187abd46d827d37cfc5708805eced197a837e49"
RELEASE = "5.7-RELEASE-14"
IMAGE = "localhost/swak-tak-build:local"
DATABASE_IMAGE = "docker.io/postgis/postgis:15-3.3"
LDAP_IMAGE = "localhost/swak-tak-ldap:local"
NETWORK = "swak-tak"
DATABASE = "swak-tak-db"
DATABASE_B = "swak-tak-db-b"
SERVER = "swak-tak-server"
# Server B federates with server A and authenticates its users with LDAP.
SERVER_B = "swak-tak-server-b"
LDAP = "swak-tak-ldap"
CONTAINERS = (SERVER, SERVER_B, DATABASE, DATABASE_B, LDAP)
# A fixed subnet lets the server certificates name the container addresses.
SUBNET = "10.89.77.0/24"
GATEWAY = "10.89.77.1"
ADDRESS = {SERVER: "10.89.77.10", SERVER_B: "10.89.77.11", LDAP: "10.89.77.12"}
PASSWORD = "atakatak"
# Host loopback port for each server port.
PORTS = {8088: 18088, 8089: 18089, 8443: 18443, 8446: 18446}
PORTS_B = {8089: 28089, 8443: 28443, 8446: 28446}
# QUIC uses UDP.
UDP_PORTS = {8090: 18090}
CLIENTS = ("left", "right", "admin")
ENROLL_USER = "swak-enroll"
ENROLL_PASSWORD = "swak-enroll-test"
# tools/tak-server/ldap/seed.ldif holds this user.
LDAP_USER = "swak-ldap"
LDAP_PASSWORD = "swak-ldap-test"
# SWAK sends to the TAK multicast input and receives from a TAK static subscription.
MULTICAST_INPUT = ("239.2.3.1", 6969)
MULTICAST_OUTPUT = ("239.2.3.2", 6970)
FEDERATION_GROUP = "SwakTest"


def run(command, **options):
    """Run a command and stop with its output on failure."""
    print("+", " ".join(str(part) for part in command), flush=True)
    result = subprocess.run(command, check=False, **options)
    if result.returncode != 0:
        sys.exit(f"Command failed with status {result.returncode}.")
    return result


def podman(*arguments, check=True, capture=False):
    """Run podman. Without check, return the result for the caller to inspect."""
    command = ["podman", *arguments]
    if check:
        return run(command, capture_output=capture, text=True)
    return subprocess.run(command, check=False, capture_output=True, text=True)


def fetch(_args):
    """Clone the pinned TAK Server source with the history that its build reads."""
    if (SOURCE / ".git").exists():
        print("The source exists. Remove build/tak-server/src to fetch it again.")
        return
    SOURCE.parent.mkdir(parents=True, exist_ok=True)
    run(["git", "init", "-q", str(SOURCE)])
    run(["git", "-C", str(SOURCE), "remote", "add", "origin", REPOSITORY])
    run(["git", "-C", str(SOURCE), "fetch", "-q", "origin", COMMIT])
    run(["git", "-C", str(SOURCE), "checkout", "-q", "-B", "main", "FETCH_HEAD"])
    # The TAK build reads its version from git describe, so it needs a release tag.
    run(
        [
            "git",
            "-C",
            str(SOURCE),
            "-c",
            "user.name=swak",
            "-c",
            "user.email=swak@localhost",
            "tag",
            "-a",
            "-f",
            RELEASE,
            "-m",
            "Local tag for the SWAK test build",
        ]
    )


def build(_args):
    """Build the image, then the TAK Server war and the schema manager without web dashboards."""
    if not (SOURCE / ".git").exists():
        sys.exit("Run fetch first.")
    run(["podman", "build", "-t", IMAGE, "-f", str(TEMPLATES / "Containerfile"), str(TEMPLATES)])
    gradle = WORK / "gradle-home"
    gradle.mkdir(parents=True, exist_ok=True)
    podman(
        "run",
        "--rm",
        "-v",
        f"{SOURCE}:/work:Z",
        "-v",
        f"{gradle}:/root/.gradle:Z",
        "-w",
        "/work/src",
        IMAGE,
        "./gradlew",
        "--no-daemon",
        "-q",
        ":takserver-core:bootWar",
        ":takserver-schemamanager:shadowJar",
        "-x",
        "test",
        "-x",
        ":takserver-tool-ui:deployToCore",
        "-x",
        ":takserver-core:deployMetricsDashboard",
    )


def openssl(*arguments):
    run(["openssl", *arguments], capture_output=True)


def fingerprint(path):
    """Return the SHA-256 certificate fingerprint in the colon form that TAK user files use."""
    result = run(
        ["openssl", "x509", "-in", str(path), "-noout", "-fingerprint", "-sha256"],
        capture_output=True,
        text=True,
    )
    return result.stdout.strip().split("=", 1)[1]


def certs(_args):
    """Create a private test CA, the server keystores, client certificates, and the user file."""
    CERTS.mkdir(parents=True, exist_ok=True)
    os.chmod(CERTS, 0o700)
    ca_key, ca_pem = CERTS / "ca.key", CERTS / "ca.pem"
    openssl(
        "req",
        "-x509",
        "-newkey",
        "rsa:2048",
        "-nodes",
        "-days",
        "7",
        "-subj",
        "/O=SWAK Test/CN=SWAK Test CA",
        "-keyout",
        str(ca_key),
        "-out",
        str(ca_pem),
        "-addext",
        "basicConstraints=critical,CA:TRUE",
        "-addext",
        "keyUsage=critical,keyCertSign,cRLSign",
    )

    def issue(name, subject, extensions):
        key, request, pem = CERTS / f"{name}.key", CERTS / f"{name}.csr", CERTS / f"{name}.pem"
        extension_file = CERTS / f"{name}.ext"
        extension_file.write_text(extensions)
        openssl(
            "req",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-subj",
            subject,
            "-keyout",
            str(key),
            "-out",
            str(request),
        )
        openssl(
            "x509",
            "-req",
            "-in",
            str(request),
            "-CA",
            str(ca_pem),
            "-CAkey",
            str(ca_key),
            "-CAcreateserial",
            "-days",
            "7",
            "-sha256",
            "-extfile",
            str(extension_file),
            "-out",
            str(pem),
        )
        request.unlink()
        extension_file.unlink()
        return key, pem

    for name, container in (("takserver", SERVER), ("takserver-b", SERVER_B)):
        server_key, server_pem = issue(
            name,
            f"/O=SWAK Test/CN={container}",
            f"subjectAltName=DNS:localhost,DNS:{container},IP:127.0.0.1,IP:{ADDRESS[container]}\n"
            "extendedKeyUsage=serverAuth,clientAuth\n",
        )
        openssl(
            "pkcs12",
            "-export",
            "-in",
            str(server_pem),
            "-inkey",
            str(server_key),
            # Federation reads the CA from the chain after the server certificate.
            "-certfile",
            str(ca_pem),
            "-name",
            "takserver",
            "-passout",
            f"pass:{PASSWORD}",
            "-out",
            str(CERTS / f"{name}.p12"),
        )
    openssl(
        "pkcs12",
        "-export",
        "-in",
        str(ca_pem),
        "-inkey",
        str(ca_key),
        "-name",
        "signing",
        "-passout",
        f"pass:{PASSWORD}",
        "-out",
        str(CERTS / "signing.p12"),
    )
    for client in CLIENTS:
        issue(client, f"/O=SWAK Test/CN=swak-{client}", "extendedKeyUsage=clientAuth\n")
    # The JDK keytool converts the stores into the JKS files of the TAK configuration.
    for stale in (
        "takserver.jks",
        "takserver-b.jks",
        "truststore-root.jks",
        "fed-truststore.jks",
        "intermediate-signing.jks",
    ):
        (CERTS / stale).unlink(missing_ok=True)
    script = (
        f"keytool -importkeystore -noprompt -srckeystore takserver.p12 -srcstoretype PKCS12 "
        f"-srcstorepass {PASSWORD} -destkeystore takserver.jks -deststoretype JKS "
        f"-deststorepass {PASSWORD} && "
        f"keytool -importkeystore -noprompt -srckeystore takserver-b.p12 -srcstoretype PKCS12 "
        f"-srcstorepass {PASSWORD} -destkeystore takserver-b.jks -deststoretype JKS "
        f"-deststorepass {PASSWORD} && "
        f"keytool -importkeystore -noprompt -srckeystore signing.p12 -srcstoretype PKCS12 "
        f"-srcstorepass {PASSWORD} -destkeystore intermediate-signing.jks -deststoretype JKS "
        f"-deststorepass {PASSWORD} && "
        f"keytool -importcert -noprompt -alias ca -file ca.pem -keystore truststore-root.jks "
        f"-storetype JKS -storepass {PASSWORD} && "
        # Messaging opens the federation truststore even when federation is off.
        f"cp truststore-root.jks fed-truststore.jks"
    )
    podman(
        "run",
        "--rm",
        "-v",
        f"{CERTS}:/certs:Z",
        "-w",
        "/certs",
        IMAGE,
        "sh",
        "-c",
        script,
        capture=True,
    )
    print(f"Test credentials are in {CERTS}. They expire after seven days.")


def bcrypt_hash(password):
    """Hash a password with the BCrypt code of the TAK Server build, which file users need."""
    tool = RUN / "tmp"
    tool.mkdir(parents=True, exist_ok=True)
    (tool / "Hash.java").write_text(
        "public class Hash { public static void main(String[] a) { System.out.print("
        "org.springframework.security.crypto.bcrypt.BCrypt.hashpw(a[0], "
        "org.springframework.security.crypto.bcrypt.BCrypt.gensalt())); } }\n"
    )
    script = (
        "cd /opt/tak/tmp && unzip -o -q -j ../takserver.war "
        "'WEB-INF/lib/spring-security-crypto-*.jar' -d crypto && "
        "java -cp 'crypto/*' Hash.java \"$PASSWORD_TO_HASH\""
    )
    result = subprocess.run(
        [
            "podman",
            "run",
            "--rm",
            "-e",
            f"PASSWORD_TO_HASH={password}",
            "-v",
            f"{RUN}:/opt/tak:Z",
            IMAGE,
            "sh",
            "-c",
            script,
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0 or not result.stdout.startswith("$2"):
        sys.exit("Cannot hash the enrollment password: " + result.stderr[-400:])
    return result.stdout.strip()


def write_users(run_dir, server):
    """Write the TAK user file. Server A has three certificate users and one enrollment user.

    Server B has only the administrator. Its other users come from LDAP.
    """
    users = [
        f'<User identifier="swak-admin" fingerprint="{fingerprint(CERTS / "admin.pem")}" '
        'role="ROLE_ADMIN"><groupList>SwakTest</groupList></User>',
    ]
    if server == SERVER:
        users += [
            f'<User identifier="swak-left" fingerprint="{fingerprint(CERTS / "left.pem")}" '
            'role="ROLE_ANONYMOUS"><groupList>SwakTest</groupList><groupList>Ops</groupList>'
            "</User>",
            f'<User identifier="swak-right" fingerprint="{fingerprint(CERTS / "right.pem")}" '
            'role="ROLE_ANONYMOUS"><groupList>SwakTest</groupList></User>',
            f'<User identifier="{ENROLL_USER}" password="{bcrypt_hash(ENROLL_PASSWORD)}" '
            'passwordHashed="true" role="ROLE_ANONYMOUS"><groupList>SwakTest</groupList></User>',
        ]
    (run_dir / "UserAuthenticationFile.xml").write_text(
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
        '<UserAuthenticationFile xmlns="http://bbn.com/marti/xml/bindings">\n  '
        + "\n  ".join(users)
        + "\n</UserAuthenticationFile>\n"
    )


def core_config(server):
    """Fill the CoreConfig template for server A or server B."""
    fields = {
        "AUTH_DEFAULT": "file",
        "LDAP": "",
        "INPUTS": "",
        "STATIC": "",
        "DATABASE": DATABASE,
        "KEYSTORE": "takserver.jks",
        "FEDERATION_OUTGOING": "",
    }
    if server == SERVER:
        group, port = MULTICAST_INPUT
        fields["INPUTS"] = (
            f'        <input _name="mcast" protocol="mcast" group="{group}" port="{port}" '
            f'auth="anonymous"><filtergroup>{FEDERATION_GROUP}</filtergroup></input>\n'
        )
        group, port = MULTICAST_OUTPUT
        fields["STATIC"] = (
            f'        <static _name="mcastout" protocol="mcast" address="{group}" port="{port}">'
            f"<filtergroup>{FEDERATION_GROUP}</filtergroup></static>\n"
        )
    else:
        fields.update(
            AUTH_DEFAULT="ldap",
            DATABASE=DATABASE_B,
            KEYSTORE="takserver-b.jks",
            LDAP=(
                f'        <ldap url="ldap://{LDAP}:389/" style="DS" '
                'userstring="cn={username},ou=users,dc=swak,dc=test" '
                'serviceAccountDN="cn=admin,dc=swak,dc=test" '
                'serviceAccountCredential="swak-ldap-admin" '
                'groupBaseRDN="ou=groups,dc=swak,dc=test" nestedGroupLookup="true" '
                'groupObjectClass="groupOfNames" userObjectClass="inetOrgPerson" '
                'groupNameExtractorRegex="(?i)cn=(.*?)(?:,|$)"/>\n'
            ),
            FEDERATION_OUTGOING=(
                f'        <federation-outgoing displayName="swak-a" address="{SERVER}" '
                'port="9001" protocolVersion="2" reconnectInterval="5" enabled="true"/>\n'
            ),
        )
    text = (TEMPLATES / "CoreConfig.xml").read_text()
    for key, value in fields.items():
        text = text.replace(f"@{key}@", value)
    return text


def prepare(run_dir, server):
    """Copy the build output and the configuration into the deployment folder of one server."""
    war = SOURCE / "src" / "takserver-core" / "build" / "libs" / f"takserver-core-{RELEASE}.war"
    schema = (
        SOURCE
        / "src"
        / "takserver-schemamanager"
        / "build"
        / "libs"
        / f"schemamanager-{RELEASE}-uber.jar"
    )
    if not war.exists() or not schema.exists():
        sys.exit("Run build first.")
    if not (CERTS / "takserver-b.jks").exists():
        sys.exit("Run certs first.")
    run_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(war, run_dir / "takserver.war")
    shutil.copy2(schema, run_dir / "SchemaManager.jar")
    for name in ("TAKIgniteConfig.xml", "start.sh"):
        shutil.copy2(TEMPLATES / name, run_dir / name)
    (run_dir / "CoreConfig.xml").write_text(core_config(server))
    (run_dir / "logs").mkdir(exist_ok=True)
    if run_dir != RUN:
        # Each server has its own copy, so the containers do not share a volume.
        shutil.copytree(CERTS, run_dir / "certs" / "files", dirs_exist_ok=True)
    write_users(run_dir, server)


def wait_port(port, seconds, tls=False):
    """Wait until a loopback port accepts a connection, or a TLS handshake starts."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
                if not tls:
                    return True
                context = ssl.create_default_context(cafile=str(CERTS / "ca.pem"))
                context.load_cert_chain(str(CERTS / "admin.pem"), str(CERTS / "admin.key"))
                with context.wrap_socket(connection, server_hostname="localhost"):
                    return True
        except OSError:
            time.sleep(2)
    return False


def admin_request(port, path, body=None):
    """Send a REST request as the test administrator and return the decoded JSON reply."""
    context = ssl.create_default_context(cafile=str(CERTS / "ca.pem"))
    context.load_cert_chain(str(CERTS / "admin.pem"), str(CERTS / "admin.key"))
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(
        f"https://localhost:{port}/Marti{path}",
        data=data,
        method="GET" if body is None else "POST",
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(request, context=context, timeout=20) as reply:
        return json.loads(reply.read() or b"null")


def federate(seconds):
    """Wait for the federation connection, then share the test group in both directions."""
    deadline = time.monotonic() + seconds
    pending = {PORTS[8443]: None, PORTS_B[8443]: None}
    while time.monotonic() < deadline and None in pending.values():
        for port, found in pending.items():
            if found is None:
                try:
                    federates = admin_request(port, "/api/federates").get("data") or []
                except OSError:
                    federates = []
                if federates:
                    pending[port] = federates[0]["id"]
        time.sleep(3)
    if None in pending.values():
        sys.exit("The servers did not federate. Read the messaging logs.")
    for port, federate_id in pending.items():
        admin_request(
            port,
            "/api/federategroups",
            {"federateId": federate_id, "group": FEDERATION_GROUP, "direction": "BOTH"},
        )
    print("Server A and server B federate the group", FEDERATION_GROUP)


def network_subnet():
    result = podman(
        "network", "inspect", "-f", "{{range .Subnets}}{{.Subnet}}{{end}}", NETWORK, check=False
    )
    return result.stdout.strip() if result.returncode == 0 else None


def start_database(name):
    podman(
        "run",
        "-d",
        "--name",
        name,
        "--network",
        NETWORK,
        "-e",
        "POSTGRES_USER=martiuser",
        "-e",
        "POSTGRES_PASSWORD=swak-test",
        "-e",
        "POSTGRES_DB=cot",
        DATABASE_IMAGE,
    )


def wait_database(name):
    for _ in range(60):
        if (
            podman(
                "exec", name, "pg_isready", "-U", "martiuser", "-d", "cot", check=False
            ).returncode
            == 0
        ):
            return
        time.sleep(2)
    sys.exit(f"The database {name} did not start.")


def start_server(name, run_dir, ports, udp_ports):
    podman(
        "run",
        "--rm",
        "--network",
        NETWORK,
        "-v",
        f"{run_dir}:/opt/tak:Z",
        "-w",
        "/opt/tak",
        IMAGE,
        "java",
        "-jar",
        "SchemaManager.jar",
        "upgrade",
    )
    published = []
    for inside, outside in ports.items():
        published += ["-p", f"127.0.0.1:{outside}:{inside}"]
    for inside, outside in udp_ports.items():
        published += ["-p", f"127.0.0.1:{outside}:{inside}/udp"]
    podman(
        "run",
        "-d",
        "--name",
        name,
        "--network",
        NETWORK,
        "--ip",
        ADDRESS[name],
        *published,
        "-v",
        f"{run_dir}:/opt/tak:Z",
        IMAGE,
        "/opt/tak/start.sh",
    )


def up(args):
    """Start the databases, the LDAP directory, and the two federated TAK Servers."""
    prepare(RUN, SERVER)
    prepare(RUN_B, SERVER_B)
    run(["podman", "build", "-q", "-t", LDAP_IMAGE, str(TEMPLATES / "ldap")], capture_output=True)
    podman("rm", "-f", *CONTAINERS, check=False)
    if network_subnet() != SUBNET:
        podman("network", "rm", NETWORK, check=False)
        podman("network", "create", "--subnet", SUBNET, NETWORK)
    podman("run", "-d", "--name", LDAP, "--network", NETWORK, "--ip", ADDRESS[LDAP], LDAP_IMAGE)
    for name in (DATABASE, DATABASE_B):
        start_database(name)
    for name in (DATABASE, DATABASE_B):
        wait_database(name)
    time.sleep(3)
    start_server(SERVER, RUN, PORTS, UDP_PORTS)
    start_server(SERVER_B, RUN_B, PORTS_B, {})
    print("Waiting for the TAK Server services. The first start takes a few minutes.")
    ready = all(
        wait_port(port, args.timeout, tls=True)
        for port in (PORTS[8089], PORTS[8443], PORTS_B[8089], PORTS_B[8443])
    )
    if not ready:
        sys.exit("The TAK Servers did not start. Read build/tak-server/run*/logs.")
    federate(args.timeout)
    print("The TAK Servers are ready on loopback ports", ", ".join(str(p) for p in PORTS.values()))


def down(_args):
    """Remove the containers and the network. Keep the build and the credentials."""
    podman("rm", "-f", *CONTAINERS, check=False)
    podman("network", "rm", NETWORK, check=False)
    print("The test deployment is removed.")


def status(_args):
    for name in CONTAINERS:
        result = podman("inspect", "-f", "{{.State.Status}}", name, check=False)
        print(name, result.stdout.strip() or "absent")
    for inside, outside in list(PORTS.items()) + list(PORTS_B.items()):
        print(f"port {outside} ({inside}):", "open" if wait_port(outside, 1) else "closed")


def profile(bridge, load=None):
    """Write the probe profile with the test credentials.

    A bridge profile uses the container addresses. The probe then runs in the rootless network
    namespace of Podman, where it can also use multicast. A loopback profile uses the published
    ports, and has no multicast checks.
    """

    def port(server, inside):
        if bridge:
            return inside
        return (PORTS if server == SERVER else PORTS_B)[inside]

    def host(server):
        return ADDRESS[server] if bridge else "localhost"

    def client(name, protocol="tls", inside=8089):
        return {
            "protocol": protocol,
            "host": host(SERVER),
            "port": inside if bridge else {**PORTS, **UDP_PORTS}[inside],
            "ca": str(CERTS / "ca.pem"),
            "certificate": str(CERTS / f"{name}.pem"),
            "key": str(CERTS / f"{name}.key"),
            "apiPort": port(SERVER, 8443),
        }

    data = {
        "left": client("left"),
        "right": client("right"),
        "admin": client("admin"),
        "plain": {"protocol": "stcp", "host": host(SERVER), "port": port(SERVER, 8088)},
        "quic": client("left", "quic", 8090),
        "enrollment": {
            "port": port(SERVER, 8446),
            "username": ENROLL_USER,
            "passwordEnv": "SWAK_COT_ENROLL",
        },
        "federation": {
            "host": host(SERVER_B),
            "port": port(SERVER_B, 8089),
            "apiPort": port(SERVER_B, 8443),
            "ca": str(CERTS / "ca.pem"),
            "group": "SwakLdap",
            "enrollment": {
                "port": port(SERVER_B, 8446),
                "username": LDAP_USER,
                "passwordEnv": "SWAK_COT_LDAP",
            },
        },
    }
    if bridge:
        data["multicast"] = {
            "localAddress": GATEWAY,
            "send": {"host": MULTICAST_INPUT[0], "port": MULTICAST_INPUT[1]},
            "receive": {"host": MULTICAST_OUTPUT[0], "port": MULTICAST_OUTPUT[1]},
        }
    if load:
        data["load"] = load
    path = WORK / "profile.json"
    path.write_text(json.dumps(data, indent=2))
    return path


def run_probe(args, report, load=None):
    """Run swak_tak_compat with a profile and write its JSON report."""
    probe = ROOT / "build" / args.preset / "swak_tak_compat"
    if not probe.exists():
        sys.exit(f"Build swak_tak_compat with the {args.preset} preset first.")
    environment = dict(
        os.environ,
        SWAK_COT_ENROLL=ENROLL_PASSWORD,
        SWAK_COT_LDAP=LDAP_PASSWORD,
        QT_QPA_PLATFORM="offscreen",
    )
    command = [str(probe), str(profile(not args.loopback, load))]
    if not args.loopback:
        command = ["podman", "unshare", "--rootless-netns", *command]
    with report.open("w") as output:
        result = subprocess.run(command, env=environment, stdout=output, check=False)
    print(report.read_text())
    sys.exit(result.returncode)


def test(args):
    """Run the compatibility probe against the running deployment."""
    suffix = "-loopback" if args.loopback else ""
    run_probe(args, WORK / f"report-{args.preset}{suffix}.json")


def load(args):
    """Send positions for a time and measure delivery, latency, memory, and disconnects."""
    run_probe(
        args,
        WORK / f"load-{args.preset}.json",
        {"seconds": args.seconds, "rate": args.rate, "objects": args.objects},
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("fetch", help="clone the pinned TAK Server source").set_defaults(run=fetch)
    commands.add_parser("build", help="build the TAK Server war").set_defaults(run=build)
    commands.add_parser("certs", help="create test credentials").set_defaults(run=certs)
    start = commands.add_parser("up", help="start the test deployment")
    start.add_argument("--timeout", type=int, default=420)
    start.set_defaults(run=up)
    commands.add_parser("status", help="show the deployment state").set_defaults(run=status)
    probe = commands.add_parser("test", help="run swak_tak_compat")
    probe.set_defaults(run=test)
    stress = commands.add_parser("load", help="run swak_tak_compat with a position load")
    stress.add_argument("--seconds", type=int, default=600)
    stress.add_argument("--rate", type=int, default=10, help="positions per second")
    stress.add_argument("--objects", type=int, default=20, help="distinct UIDs")
    stress.set_defaults(run=load)
    for command in (probe, stress):
        command.add_argument("--preset", default="debug", choices=["debug", "sanitize", "release"])
        command.add_argument(
            "--loopback",
            action="store_true",
            help="use the published loopback ports from the host and skip multicast",
        )
    commands.add_parser("down", help="remove the test deployment").set_defaults(run=down)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
