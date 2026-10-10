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
import urllib.parse
import urllib.request
import zipfile
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
NETWORK = "swak-tak"
DATABASE = "swak-tak-db"
DATABASE_B = "swak-tak-db-b"
SERVER = "swak-tak-server"
# Server B federates with server A and authenticates its users with LDAP.
SERVER_B = "swak-tak-server-b"
LDAP = "swak-tak-ldap"
# In hub mode, both servers federate through a TAK federation hub instead of directly.
HUB = "swak-tak-hub"
HUB_RUN = WORK / "hub"
CONTAINERS = (SERVER, SERVER_B, DATABASE, DATABASE_B, LDAP, HUB)
# A fixed subnet lets the server certificates name the container addresses.
SUBNET = "10.89.77.0/24"
GATEWAY = "10.89.77.1"
ADDRESS = {
    SERVER: "10.89.77.10",
    SERVER_B: "10.89.77.11",
    LDAP: "10.89.77.12",
    HUB: "10.89.77.13",
    # Fixed database addresses keep dynamic allocation away from the addresses above.
    DATABASE: "10.89.77.20",
    DATABASE_B: "10.89.77.21",
}
PASSWORD = "atakatak"
# Host loopback port for each server port.
PORTS = {8088: 18088, 8089: 18089, 8443: 18443, 8446: 18446}
PORTS_B = {8089: 28089, 8443: 28443, 8446: 28446}
# QUIC uses UDP.
UDP_PORTS = {8090: 18090}
# The ops client is only in the group Ops, which the hub policy does not let through.
CLIENTS = ("left", "right", "admin", "ops")
ENROLL_USER = "swak-enroll"
ENROLL_PASSWORD = "swak-enroll-test"
# Server B uses one of these directories over LDAPS. Each has a test user in the groups
# SwakTest and the group named here. The ad image is a Samba Active Directory controller.
DIRECTORIES = {
    "openldap": {
        "image": "localhost/swak-tak-ldap:local",
        "context": "ldap",
        "user": "swak-ldap",
        "password": "swak-ldap-test",
        "group": "SwakLdap",
        "ldap": (
            '        <ldap url="ldaps://swak-tak-ldap:636/" style="DS" '
            'userstring="cn={username},ou=users,dc=swak,dc=test" '
            'serviceAccountDN="cn=admin,dc=swak,dc=test" '
            'serviceAccountCredential="swak-ldap-admin" '
            'groupBaseRDN="ou=groups,dc=swak,dc=test" nestedGroupLookup="true" '
            'groupObjectClass="groupOfNames" userObjectClass="inetOrgPerson" '
            'groupNameExtractorRegex="(?i)cn=(.*?)(?:,|$)" @TRUST@/>\n'
        ),
    },
    "ad": {
        "image": "localhost/swak-tak-ad:local",
        "context": "ad",
        "user": "swak-ad",
        "password": "swak-ad-test",
        "group": "SwakAd",
        "ldap": (
            '        <ldap url="ldaps://swak-tak-ldap:636/dc=swak,dc=test" style="AD" '
            'userstring="{username}@swak.test" '
            'serviceAccountDN="Administrator@swak.test" '
            'serviceAccountCredential="Swak-AD-test1" @TRUST@/>\n'
        ),
    },
}
# TAK Server checks the directory certificate with the test CA.
LDAPS_TRUST = (
    'ldapsTruststore="JKS" ldapsTruststoreFile="/opt/tak/certs/files/truststore-root.jks" '
    f'ldapsTruststorePass="{PASSWORD}"'
)
DEPLOYMENT = WORK / "deployment.json"
# SWAK sends to the TAK multicast input and receives from a TAK static subscription.
MULTICAST_INPUT = ("239.2.3.1", 6969)
MULTICAST_OUTPUT = ("239.2.3.2", 6970)
FEDERATION_GROUP = "SwakTest"
# Server A also sends this group to its federate. The hub policy filters it out.
FILTERED_GROUP = "Ops"


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
        ":federation-hub-policy:bootJar",
        ":federation-hub-broker:bootJar",
        ":federation-hub-plugin-manager:bootJar",
        ":federation-hub-plugins:buildSDK",
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

    for name, container in (("takserver", SERVER), ("takserver-b", SERVER_B), ("fedhub", HUB)):
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
    issue(
        "ldap",
        f"/O=SWAK Test/CN={LDAP}",
        f"subjectAltName=DNS:{LDAP},IP:{ADDRESS[LDAP]}\nextendedKeyUsage=serverAuth\n",
    )
    # The JDK keytool converts the stores into the JKS files of the TAK configuration.
    for stale in (
        "takserver.jks",
        "takserver-b.jks",
        "fedhub.jks",
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
        f"keytool -importkeystore -noprompt -srckeystore fedhub.p12 -srcstoretype PKCS12 "
        f"-srcstorepass {PASSWORD} -destkeystore fedhub.jks -deststoretype JKS "
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
            f'<User identifier="swak-ops" fingerprint="{fingerprint(CERTS / "ops.pem")}" '
            f'role="ROLE_ANONYMOUS"><groupList>{FILTERED_GROUP}</groupList></User>',
            f'<User identifier="{ENROLL_USER}" password="{bcrypt_hash(ENROLL_PASSWORD)}" '
            'passwordHashed="true" role="ROLE_ANONYMOUS"><groupList>SwakTest</groupList></User>',
        ]
    (run_dir / "UserAuthenticationFile.xml").write_text(
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
        '<UserAuthenticationFile xmlns="http://bbn.com/marti/xml/bindings">\n  '
        + "\n  ".join(users)
        + "\n</UserAuthenticationFile>\n"
    )


def core_config(server, directory="openldap", federation="direct"):
    """Fill the CoreConfig template for server A, or for server B with one directory.

    In direct federation, server B connects to server A. In hub federation, both servers
    connect to the hub.
    """
    fields = {
        "AUTH_DEFAULT": "file",
        "LDAP": "",
        "INPUTS": "",
        "STATIC": "",
        "DATABASE": DATABASE,
        "KEYSTORE": "takserver.jks",
        "FEDERATION_OUTGOING": "",
    }
    hub_outgoing = (
        f'        <federation-outgoing displayName="swak-hub" address="{HUB}" '
        'port="9102" protocolVersion="2" reconnectInterval="5" enabled="true"/>\n'
    )
    if server == SERVER:
        if federation == "hub":
            fields["FEDERATION_OUTGOING"] = hub_outgoing
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
            LDAP=DIRECTORIES[directory]["ldap"].replace("@TRUST@", LDAPS_TRUST),
            FEDERATION_OUTGOING=(
                hub_outgoing
                if federation == "hub"
                else f'        <federation-outgoing displayName="swak-a" address="{SERVER}" '
                'port="9001" protocolVersion="2" reconnectInterval="5" enabled="true"/>\n'
            ),
        )
    text = (TEMPLATES / "CoreConfig.xml").read_text()
    for key, value in fields.items():
        text = text.replace(f"@{key}@", value)
    return text


def prepare(run_dir, server, directory="openldap", federation="direct"):
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
    (run_dir / "CoreConfig.xml").write_text(core_config(server, directory, federation))
    (run_dir / "logs").mkdir(exist_ok=True)
    if run_dir != RUN:
        # Each server has its own copy, so the containers do not share a volume.
        shutil.copytree(CERTS, run_dir / "certs" / "files", dirs_exist_ok=True)
    write_users(run_dir, server)


def ca_group():
    """Return the hub CA group of the test CA: its issuer DN and SHA-256 fingerprint."""
    issuer = run(
        [
            "openssl",
            "x509",
            "-in",
            str(CERTS / "ca.pem"),
            "-noout",
            "-issuer",
            "-nameopt",
            "RFC2253",
        ],
        capture_output=True,
        text=True,
    ).stdout.strip()
    digest = fingerprint(CERTS / "ca.pem").replace(":", "").lower()
    return issuer.split("=", 1)[1] + "-" + digest


def hub_policy():
    """Return the hub policy for the test CA group.

    The group is not interconnected. One edge from the group to itself lets only the group
    SwakTest through, so the hub drops events of the group Ops. The plugin entries enable the
    SWAK test interceptor and sender for every federate.
    """
    group = ca_group()
    cells = [
        {
            "graphType": "GroupCell",
            "id": "swak-test-ca",
            "type": "bpmn.Activity",
            "roger_federation": {
                "name": group,
                "description": "SWAK Test CA",
                "attributes": [],
                "interconnected": False,
                "groupFilters": [],
                "type": "Group",
                "stringId": "SWAK Test CA",
            },
        },
        {
            "graphType": "EdgeCell",
            "id": "swak-test-filter",
            "type": "bpmn.Flow",
            "source": {"id": "swak-test-ca"},
            "target": {"id": "swak-test-ca"},
            "roger_federation": {
                "name": "SWAK Test CA -> SWAK Test CA",
                "groupsFilterType": "allowed",
                "allowedGroups": [FEDERATION_GROUP],
                "disallowedGroups": [],
                "edgeFilters": [],
                "type": "Federate Policy",
            },
        },
    ]

    def plugin(name, send, receive):
        # The hub fails on missing lists, so every list is present.
        return {
            "pluginName": name,
            "sendTo": [],
            "dontSend": [],
            "receiveFrom": [],
            "dontReceive": [],
            "sendByDefault": send,
            "receiveByDefault": receive,
            "disabled": False,
        }

    return {
        "name": "swak",
        "federate_edges": [
            {
                "source": group,
                "destination": group,
                "groupsFilterType": "allowed",
            }
        ],
        "groups": [{"uid": group, "interconnected": False}],
        "additionalData": {"uiData": {"name": "swak", "description": "SWAK test", "cells": cells}},
        "pluginsData": {
            "plugins": [
                plugin("swak-interceptor", False, True),
                plugin("swak-sender", True, False),
            ]
        },
    }


def prepare_hub():
    """Copy the hub jars, configuration, policy, and keystores into the hub folder."""
    jars = {
        "federation-hub-policy.jar": SOURCE / "src" / "federation-hub-policy",
        "federation-hub-broker.jar": SOURCE / "src" / "federation-hub-broker",
        "federation-hub-plugin-manager.jar": SOURCE / "src" / "federation-hub-plugin-manager",
        "federation-hub-plugins-sdk.jar": SOURCE / "src" / "federation-hub-plugins",
    }
    (HUB_RUN / "jars").mkdir(parents=True, exist_ok=True)
    (HUB_RUN / "configs").mkdir(exist_ok=True)
    for name, project in jars.items():
        built = project / "build" / "libs" / f"{name[:-4]}-{RELEASE}.jar"
        if not built.exists():
            sys.exit("Run build again. The federation hub jars are missing.")
        shutil.copy2(built, HUB_RUN / "jars" / name)
    shutil.copy2(TEMPLATES / "hub" / "start.sh", HUB_RUN / "start.sh")
    (HUB_RUN / "plugins").mkdir(exist_ok=True)
    shutil.copy2(
        TEMPLATES / "hub" / "SwakHubPlugin.java", HUB_RUN / "plugins" / "SwakHubPlugin.java"
    )
    # The plugins need Ignite and its dependencies. The plugin manager jar holds them.
    libraries = HUB_RUN / "plugin-lib"
    shutil.rmtree(libraries, ignore_errors=True)
    libraries.mkdir()
    with zipfile.ZipFile(HUB_RUN / "jars" / "federation-hub-plugin-manager.jar") as archive:
        for entry in archive.infolist():
            name = entry.filename
            if name.startswith("BOOT-INF/lib/") and name.endswith(".jar") and "/" not in name[13:]:
                (libraries / name[13:]).write_bytes(archive.read(entry))
    shutil.copy2(
        TEMPLATES / "hub" / "federation-hub-broker.yml",
        HUB_RUN / "configs" / "federation-hub-broker.yml",
    )
    (HUB_RUN / "ui_generated_policy.json").write_text(json.dumps(hub_policy(), indent=2))
    files = HUB_RUN / "certs" / "files"
    files.mkdir(parents=True, exist_ok=True)
    for name in ("fedhub.jks", "fed-truststore.jks", "ca.pem"):
        shutil.copy2(CERTS / name, files / name)


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


def admin_request(port, path, body=None, method=None):
    """Send a REST request as the test administrator and return the decoded JSON reply."""
    context = ssl.create_default_context(cafile=str(CERTS / "ca.pem"))
    context.load_cert_chain(str(CERTS / "admin.pem"), str(CERTS / "admin.key"))
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(
        f"https://localhost:{port}/Marti{path}",
        data=data,
        method=method or ("GET" if body is None else "POST"),
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
                    pending[port] = federates[0]
        time.sleep(3)
    if None in pending.values():
        sys.exit("The servers did not federate. Read the messaging logs.")
    # Group mapping makes the servers send the group names of each event, which the hub
    # policy filters. Each server maps the received names to its local SwakTest.
    remote_groups = {
        PORTS[8443]: [FEDERATION_GROUP],
        PORTS_B[8443]: [FEDERATION_GROUP, FILTERED_GROUP],
    }
    for port, federate in pending.items():
        federate_id = federate["id"]
        admin_request(
            port,
            "/api/federategroups",
            {"federateId": federate_id, "group": FEDERATION_GROUP, "direction": "BOTH"},
        )
        admin_request(
            port, "/api/federatedetails", {**federate, "federatedGroupMapping": True}, "PUT"
        )
        for remote in remote_groups[port]:
            query = urllib.parse.urlencode({"remoteGroup": remote, "localGroup": FEDERATION_GROUP})
            admin_request(
                port,
                f"/api/federategroupsmap/{urllib.parse.quote(federate_id, safe='')}?{query}",
                {},
            )
    # Server A also sends Ops. Without the hub filter, the server B user receives Ops events.
    admin_request(
        PORTS[8443],
        "/api/federategroups",
        {
            "federateId": pending[PORTS[8443]]["id"],
            "group": FILTERED_GROUP,
            "direction": "OUTBOUND",
        },
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
        "--ip",
        ADDRESS[name],
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
    """Start the databases, the directory, and the two federated TAK Servers."""
    directory = DIRECTORIES[args.directory]
    prepare(RUN, SERVER, federation=args.federation)
    prepare(RUN_B, SERVER_B, args.directory, args.federation)
    if args.federation == "hub":
        prepare_hub()
    if not (CERTS / "ldap.pem").exists():
        sys.exit("Run certs again. The directory needs its certificate.")
    run(
        ["podman", "build", "-q", "-t", directory["image"], str(TEMPLATES / directory["context"])],
        capture_output=True,
    )
    # The directory reads its certificate from a private copy. Samba requires key mode 0600.
    directory_certs = WORK / "directory-certs"
    directory_certs.mkdir(mode=0o700, exist_ok=True)
    for name in ("ca.pem", "ldap.pem", "ldap.key"):
        shutil.copy2(CERTS / name, directory_certs / name)
        os.chmod(directory_certs / name, 0o600)
    podman("rm", "-f", *CONTAINERS, check=False)
    if network_subnet() != SUBNET:
        podman("network", "rm", NETWORK, check=False)
        podman("network", "create", "--subnet", SUBNET, NETWORK)
    podman(
        "run",
        "-d",
        "--name",
        LDAP,
        "--hostname",
        "swakad",
        "--network",
        NETWORK,
        "--ip",
        ADDRESS[LDAP],
        "-v",
        f"{directory_certs}:/certs:ro,Z",
        directory["image"],
    )
    if args.federation == "hub":
        podman(
            "run",
            "-d",
            "--name",
            HUB,
            "--network",
            NETWORK,
            "--ip",
            ADDRESS[HUB],
            "-v",
            f"{HUB_RUN}:/opt/tak/federation-hub:Z",
            IMAGE,
            "/opt/tak/federation-hub/start.sh",
        )
    DEPLOYMENT.write_text(json.dumps({"directory": args.directory, "federation": args.federation}))
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
    print("Server B uses the directory", args.directory, "and federation", args.federation)


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


def deployment():
    """Return the directory and federation mode of the running deployment."""
    try:
        data = json.loads(DEPLOYMENT.read_text())
    except (OSError, ValueError):
        data = {}
    if not isinstance(data, dict):
        data = {}
    directory = data.get("directory")
    federation = data.get("federation")
    return {
        "directory": directory if directory in DIRECTORIES else "openldap",
        "federation": federation if federation in ("direct", "hub") else "direct",
    }


def deployed_directory():
    """Return the directory of the running deployment."""
    return deployment()["directory"]


def profile(bridge, load=None, directory="openldap", federation="direct"):
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
        "ops": client("ops"),
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
            "group": DIRECTORIES[directory]["group"],
            # Through the hub, the policy drops the Ops events and the test plugins run.
            "hub": federation == "hub",
            "enrollment": {
                "port": port(SERVER_B, 8446),
                "username": DIRECTORIES[directory]["user"],
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
    state = deployment()
    directory = state["directory"]
    environment = dict(
        os.environ,
        SWAK_COT_ENROLL=ENROLL_PASSWORD,
        SWAK_COT_LDAP=DIRECTORIES[directory]["password"],
        QT_QPA_PLATFORM="offscreen",
    )
    command = [str(probe), str(profile(not args.loopback, load, directory, state["federation"]))]
    if not args.loopback:
        command = ["podman", "unshare", "--rootless-netns", *command]
    loss = getattr(args, "loss", 0) or getattr(args, "delay", 0)
    if loss:
        impair(args.loss, args.delay)
    try:
        with report.open("w") as output:
            result = subprocess.run(command, env=environment, stdout=output, check=False)
    finally:
        if loss:
            impair(0, 0)
    print(report.read_text())
    sys.exit(result.returncode)


def test(args):
    """Run the compatibility probe against the running deployment."""
    suffix = "-loopback" if args.loopback else ""
    run_probe(args, WORK / f"report-{args.preset}{suffix}.json")


def impair(loss, delay):
    """Add packet loss and delay between the probe and server A in both directions.

    The bridge sends from the probe to the containers, and server A's interface sends from the
    server. Zero values remove the rules.
    """
    if not 0 <= loss <= 50 or not 0 <= delay <= 2000:
        sys.exit("Use a loss of 0 to 50 percent and a delay of 0 to 2000 ms.")
    tc = shutil.which("tc") or "/usr/sbin/tc"
    pid = podman("inspect", "-f", "{{.State.Pid}}", SERVER, capture=True).stdout.strip()
    rule = ["root", "netem", "loss", f"{loss}%", "delay", f"{delay}ms"]
    # The bridge is the interface with the gateway address in the rootless namespace.
    addresses = podman_unshare("ip", "-o", "-4", "addr", "show", capture=True).stdout
    bridge = next(
        (line.split()[1] for line in addresses.splitlines() if f" {GATEWAY}/" in line), None
    )
    if not bridge:
        sys.exit("Cannot find the Podman bridge.")
    targets = [[], ["nsenter", "-t", pid, "-n"]]
    for prefix, device in zip(targets, (bridge, "eth0")):
        command = [*prefix, tc, "qdisc"]
        if loss or delay:
            podman_unshare(*command, "replace", "dev", device, *rule)
        else:
            podman_unshare(*command, "del", "dev", device, "root", check=False)
    print(f"Network loss {loss}%, delay {delay} ms")


def podman_unshare(*command, capture=False, check=True):
    """Run a command in the rootless network namespace of Podman."""
    result = subprocess.run(
        ["podman", "unshare", "--rootless-netns", *command],
        check=False,
        capture_output=capture,
        text=True,
    )
    if check and result.returncode != 0:
        sys.exit(f"Command failed: {' '.join(command)}")
    return result


def load(args):
    """Send positions for a time and measure delivery, latency, memory, and disconnects."""
    if args.loopback and (args.loss or args.delay):
        sys.exit("Packet loss needs the bridge mode.")
    data = {"seconds": args.seconds, "rate": args.rate, "objects": args.objects}
    if args.client == "quic":
        data["client"] = "quic"
    if args.minimum is not None:
        data["minimumRatio"] = args.minimum
    run_probe(args, WORK / f"load-{args.preset}-{args.client}.json", data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("fetch", help="clone the pinned TAK Server source").set_defaults(run=fetch)
    commands.add_parser("build", help="build the TAK Server war").set_defaults(run=build)
    commands.add_parser("certs", help="create test credentials").set_defaults(run=certs)
    start = commands.add_parser("up", help="start the test deployment")
    start.add_argument("--timeout", type=int, default=420)
    start.add_argument(
        "--directory",
        default="openldap",
        choices=sorted(DIRECTORIES),
        help="the LDAPS directory of server B",
    )
    start.add_argument(
        "--federation",
        default="direct",
        choices=["direct", "hub"],
        help="federate directly or through a federation hub",
    )
    start.set_defaults(run=up)
    commands.add_parser("status", help="show the deployment state").set_defaults(run=status)
    probe = commands.add_parser("test", help="run swak_tak_compat")
    probe.set_defaults(run=test)
    stress = commands.add_parser("load", help="run swak_tak_compat with a position load")
    stress.add_argument("--seconds", type=int, default=600)
    stress.add_argument("--rate", type=int, default=10, help="positions per second")
    stress.add_argument("--objects", type=int, default=20, help="distinct UIDs")
    stress.add_argument("--client", default="tls", choices=["tls", "quic"], help="sender stream")
    stress.add_argument("--loss", type=float, default=0, help="packet loss in percent")
    stress.add_argument("--delay", type=int, default=0, help="added delay in milliseconds")
    stress.add_argument("--minimum", type=float, help="minimum delivery ratio, default 0.99")
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
