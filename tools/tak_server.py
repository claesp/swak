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
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "build" / "tak-server"
SOURCE = WORK / "src"
RUN = WORK / "run"
CERTS = RUN / "certs" / "files"
TEMPLATES = ROOT / "tools" / "tak-server"
REPOSITORY = "https://github.com/TAK-Product-Center/Server.git"
COMMIT = "5187abd46d827d37cfc5708805eced197a837e49"
RELEASE = "5.7-RELEASE-14"
IMAGE = "localhost/swak-tak-build:local"
DATABASE_IMAGE = "docker.io/postgis/postgis:15-3.3"
NETWORK = "swak-tak"
DATABASE = "swak-tak-db"
SERVER = "swak-tak-server"
PASSWORD = "atakatak"
# Host loopback port for each server port.
PORTS = {8088: 18088, 8089: 18089, 8443: 18443, 8446: 18446}
CLIENTS = ("left", "right", "admin")
ENROLL_USER = "swak-enroll"
ENROLL_PASSWORD = "swak-enroll-test"


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

    server_key, server_pem = issue(
        "takserver",
        "/O=SWAK Test/CN=takserver",
        "subjectAltName=DNS:localhost,DNS:takserver,IP:127.0.0.1\n"
        "extendedKeyUsage=serverAuth,clientAuth\n",
    )
    openssl(
        "pkcs12",
        "-export",
        "-in",
        str(server_pem),
        "-inkey",
        str(server_key),
        "-name",
        "takserver",
        "-passout",
        f"pass:{PASSWORD}",
        "-out",
        str(CERTS / "takserver.p12"),
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
        "truststore-root.jks",
        "fed-truststore.jks",
        "intermediate-signing.jks",
    ):
        (CERTS / stale).unlink(missing_ok=True)
    script = (
        f"keytool -importkeystore -noprompt -srckeystore takserver.p12 -srcstoretype PKCS12 "
        f"-srcstorepass {PASSWORD} -destkeystore takserver.jks -deststoretype JKS "
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


def write_users():
    """Write the TAK user file: three certificate users and one enrollment user."""
    users = [
        f'<User identifier="swak-left" fingerprint="{fingerprint(CERTS / "left.pem")}" '
        'role="ROLE_ANONYMOUS"><groupList>SwakTest</groupList><groupList>Ops</groupList></User>',
        f'<User identifier="swak-right" fingerprint="{fingerprint(CERTS / "right.pem")}" '
        'role="ROLE_ANONYMOUS"><groupList>SwakTest</groupList></User>',
        f'<User identifier="swak-admin" fingerprint="{fingerprint(CERTS / "admin.pem")}" '
        'role="ROLE_ADMIN"><groupList>SwakTest</groupList></User>',
        f'<User identifier="{ENROLL_USER}" password="{bcrypt_hash(ENROLL_PASSWORD)}" '
        'passwordHashed="true" role="ROLE_ANONYMOUS"><groupList>SwakTest</groupList></User>',
    ]
    (RUN / "UserAuthenticationFile.xml").write_text(
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
        '<UserAuthenticationFile xmlns="http://bbn.com/marti/xml/bindings">\n  '
        + "\n  ".join(users)
        + "\n</UserAuthenticationFile>\n"
    )


def prepare():
    """Copy the build output and the configuration into the deployment folder."""
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
    if not (CERTS / "takserver.jks").exists():
        sys.exit("Run certs first.")
    shutil.copy2(war, RUN / "takserver.war")
    shutil.copy2(schema, RUN / "SchemaManager.jar")
    for name in ("CoreConfig.xml", "TAKIgniteConfig.xml", "start.sh"):
        shutil.copy2(TEMPLATES / name, RUN / name)
    (RUN / "logs").mkdir(exist_ok=True)
    write_users()


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


def up(args):
    """Start the database, apply the schema, and start the TAK Server services."""
    prepare()
    if podman("network", "exists", NETWORK, check=False).returncode != 0:
        podman("network", "create", NETWORK)
    podman("rm", "-f", DATABASE, SERVER, check=False)
    podman(
        "run",
        "-d",
        "--name",
        DATABASE,
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
    for _ in range(60):
        if (
            podman(
                "exec", DATABASE, "pg_isready", "-U", "martiuser", "-d", "cot", check=False
            ).returncode
            == 0
        ):
            break
        time.sleep(2)
    else:
        sys.exit("The database did not start.")
    time.sleep(3)
    podman(
        "run",
        "--rm",
        "--network",
        NETWORK,
        "-v",
        f"{RUN}:/opt/tak:Z",
        "-w",
        "/opt/tak",
        IMAGE,
        "java",
        "-jar",
        "SchemaManager.jar",
        "upgrade",
    )
    published = []
    for inside, outside in PORTS.items():
        published += ["-p", f"127.0.0.1:{outside}:{inside}"]
    podman(
        "run",
        "-d",
        "--name",
        SERVER,
        "--network",
        NETWORK,
        *published,
        "-v",
        f"{RUN}:/opt/tak:Z",
        IMAGE,
        "/opt/tak/start.sh",
    )
    print("Waiting for the TAK Server services. The first start takes a few minutes.")
    ready = wait_port(PORTS[8089], args.timeout, tls=True) and wait_port(
        PORTS[8443], args.timeout, tls=True
    )
    if not ready:
        sys.exit("The TAK Server did not start. Read build/tak-server/run/logs.")
    print("The TAK Server is ready on loopback ports", ", ".join(str(p) for p in PORTS.values()))


def down(_args):
    """Remove the containers and the network. Keep the build and the credentials."""
    podman("rm", "-f", SERVER, DATABASE, check=False)
    podman("network", "rm", NETWORK, check=False)
    print("The test deployment is removed.")


def status(_args):
    for name in (DATABASE, SERVER):
        result = podman("inspect", "-f", "{{.State.Status}}", name, check=False)
        print(name, result.stdout.strip() or "absent")
    for inside, outside in PORTS.items():
        print(f"port {outside} ({inside}):", "open" if wait_port(outside, 1) else "closed")


def profile():
    """Write the probe profile with the test credentials."""

    def client(name, protocol="tls", port=PORTS[8089]):
        return {
            "protocol": protocol,
            "host": "localhost",
            "port": port,
            "ca": str(CERTS / "ca.pem"),
            "certificate": str(CERTS / f"{name}.pem"),
            "key": str(CERTS / f"{name}.key"),
            "apiPort": PORTS[8443],
        }

    data = {
        "left": client("left"),
        "right": client("right"),
        "admin": client("admin"),
        "plain": {"protocol": "stcp", "host": "localhost", "port": PORTS[8088]},
        "enrollment": {
            "port": PORTS[8446],
            "username": ENROLL_USER,
            "passwordEnv": "SWAK_COT_ENROLL",
        },
    }
    path = WORK / "profile.json"
    path.write_text(json.dumps(data, indent=2))
    return path


def test(args):
    """Run the compatibility probe against the running deployment."""
    probe = ROOT / "build" / args.preset / "swak_tak_compat"
    if not probe.exists():
        sys.exit(f"Build swak_tak_compat with the {args.preset} preset first.")
    environment = dict(os.environ, SWAK_COT_ENROLL=ENROLL_PASSWORD, QT_QPA_PLATFORM="offscreen")
    report = WORK / f"report-{args.preset}.json"
    with report.open("w") as output:
        result = subprocess.run(
            [str(probe), str(profile())], env=environment, stdout=output, check=False
        )
    print(report.read_text())
    sys.exit(result.returncode)


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
    probe.add_argument("--preset", default="debug", choices=["debug", "sanitize", "release"])
    probe.set_defaults(run=test)
    commands.add_parser("down", help="remove the test deployment").set_defaults(run=down)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
