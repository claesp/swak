"""Test the configuration that tools/tak_server.py writes for the TAK test deployment."""

import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ElementTree

spec = importlib.util.spec_from_file_location(
    "tak_server", Path(__file__).resolve().parents[1] / "tools/tak_server.py"
)
tak_server = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tak_server)

NAMESPACE = "{http://bbn.com/marti/xml/config}"


def parse(text):
    """Parse a filled configuration. No template field may remain."""
    if re.search(r"@[A-Z_]+@", text):
        raise AssertionError("A template field remains.")
    return ElementTree.fromstring(text)


class CoreConfigTest(unittest.TestCase):
    def outgoing(self, root):
        return [
            (item.get("address"), item.get("port"))
            for item in root.iter(NAMESPACE + "federation-outgoing")
        ]

    def test_server_a_direct(self):
        root = parse(tak_server.core_config(tak_server.SERVER))
        inputs = {item.get("_name"): item for item in root.iter(NAMESPACE + "input")}
        self.assertEqual(inputs["mcast"].get("group"), tak_server.MULTICAST_INPUT[0])
        self.assertEqual(inputs["quic"].get("protocol"), "quic")
        static = next(root.iter(NAMESPACE + "static"))
        self.assertEqual(static.get("address"), tak_server.MULTICAST_OUTPUT[0])
        self.assertEqual(next(root.iter(NAMESPACE + "auth")).get("default"), "file")
        self.assertIsNone(next(root.iter(NAMESPACE + "ldap"), None))
        self.assertEqual(self.outgoing(root), [])
        self.assertIn("swak-tak-db:5432", tak_server.core_config(tak_server.SERVER))

    def test_server_b_directories(self):
        for directory, style, url in (
            ("openldap", "DS", "ldaps://swak-tak-ldap:636/"),
            ("ad", "AD", "ldaps://swak-tak-ldap:636/dc=swak,dc=test"),
        ):
            with self.subTest(directory=directory):
                text = tak_server.core_config(tak_server.SERVER_B, directory)
                root = parse(text)
                ldap = next(root.iter(NAMESPACE + "ldap"))
                self.assertEqual(ldap.get("style"), style)
                self.assertEqual(ldap.get("url"), url)
                self.assertTrue(ldap.get("ldapsTruststoreFile").endswith("truststore-root.jks"))
                self.assertEqual(next(root.iter(NAMESPACE + "auth")).get("default"), "ldap")
                self.assertEqual(self.outgoing(root), [(tak_server.SERVER, "9001")])
                self.assertIn("swak-tak-db-b:5432", text)
                self.assertIn("takserver-b.jks", text)
                self.assertIsNone(next(root.iter(NAMESPACE + "static"), None))

    def test_hub_federation(self):
        for server in (tak_server.SERVER, tak_server.SERVER_B):
            with self.subTest(server=server):
                root = parse(tak_server.core_config(server, "openldap", "hub"))
                self.assertEqual(self.outgoing(root), [(tak_server.HUB, "9102")])


class ProfileTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.patch = patch.object(tak_server, "WORK", Path(self.directory.name))
        self.patch.start()

    def tearDown(self):
        self.patch.stop()
        self.directory.cleanup()

    def test_bridge(self):
        data = json.loads(tak_server.profile(True).read_text())
        self.assertEqual(data["left"]["host"], tak_server.ADDRESS[tak_server.SERVER])
        self.assertEqual(data["left"]["port"], 8089)
        self.assertEqual(data["quic"]["port"], 8090)
        self.assertEqual(data["federation"]["host"], tak_server.ADDRESS[tak_server.SERVER_B])
        self.assertEqual(data["multicast"]["localAddress"], tak_server.GATEWAY)
        self.assertEqual(data["federation"]["group"], "SwakLdap")
        self.assertNotIn("load", data)

    def test_loopback(self):
        data = json.loads(tak_server.profile(False, {"seconds": 5}, "ad").read_text())
        self.assertEqual(data["left"]["host"], "localhost")
        self.assertEqual(data["left"]["port"], tak_server.PORTS[8089])
        self.assertEqual(data["quic"]["port"], tak_server.UDP_PORTS[8090])
        self.assertEqual(data["federation"]["port"], tak_server.PORTS_B[8089])
        self.assertEqual(data["federation"]["enrollment"]["username"], "swak-ad")
        self.assertEqual(data["federation"]["group"], "SwakAd")
        self.assertNotIn("multicast", data)
        self.assertEqual(data["load"], {"seconds": 5})
        # The profile names environment variables, never passwords.
        self.assertNotIn(tak_server.DIRECTORIES["ad"]["password"], json.dumps(data))

    def test_deployed_directory(self):
        with patch.object(tak_server, "DEPLOYMENT", Path(self.directory.name) / "d.json"):
            self.assertEqual(tak_server.deployed_directory(), "openldap")
            tak_server.DEPLOYMENT.write_text('{"directory": "ad"}')
            self.assertEqual(tak_server.deployed_directory(), "ad")
            tak_server.DEPLOYMENT.write_text('{"directory": "other"}')
            self.assertEqual(tak_server.deployed_directory(), "openldap")
            tak_server.DEPLOYMENT.write_text("{")
            self.assertEqual(tak_server.deployed_directory(), "openldap")


class HubTest(unittest.TestCase):
    def test_ca_group(self):
        with tempfile.TemporaryDirectory() as directory:
            certs = Path(directory)
            subprocess.run(
                [
                    "openssl",
                    "req",
                    "-x509",
                    "-newkey",
                    "rsa:2048",
                    "-nodes",
                    "-days",
                    "1",
                    "-subj",
                    "/O=SWAK Test/CN=SWAK Test CA",
                    "-keyout",
                    str(certs / "ca.key"),
                    "-out",
                    str(certs / "ca.pem"),
                ],
                check=True,
                capture_output=True,
            )
            der = subprocess.run(
                ["openssl", "x509", "-in", str(certs / "ca.pem"), "-outform", "DER"],
                check=True,
                capture_output=True,
            ).stdout
            with (
                patch.object(tak_server, "CERTS", certs),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                group = tak_server.ca_group()
                policy = tak_server.hub_policy()
        # The hub names a CA group by the RFC 2253 issuer and the lowercase SHA-256 hex.
        self.assertEqual(group, "CN=SWAK Test CA,O=SWAK Test-" + hashlib.sha256(der).hexdigest())
        self.assertEqual(policy["groups"], [{"uid": group, "interconnected": True}])
        cell = policy["additionalData"]["uiData"]["cells"][0]
        self.assertEqual(cell["graphType"], "GroupCell")
        self.assertEqual(cell["roger_federation"]["name"], group)
        self.assertTrue(cell["roger_federation"]["interconnected"])


class LoadTest(unittest.TestCase):
    def test_impair_limits(self):
        # The limits stop the run before any Podman command.
        with patch.object(tak_server, "podman") as podman:
            for loss, delay in ((-1, 0), (51, 0), (0, -1), (0, 2001)):
                with self.subTest(loss=loss, delay=delay), self.assertRaises(SystemExit):
                    tak_server.impair(loss, delay)
            podman.assert_not_called()

    def test_loopback_loss(self):
        args = argparse.Namespace(
            loopback=True, loss=5, delay=0, seconds=5, rate=1, objects=1, client="tls"
        )
        with patch.object(tak_server, "run_probe") as run_probe, self.assertRaises(SystemExit):
            tak_server.load(args)
        run_probe.assert_not_called()

    def test_load_profile(self):
        args = argparse.Namespace(
            loopback=False,
            loss=0,
            delay=0,
            seconds=30,
            rate=4,
            objects=2,
            client="quic",
            minimum=0.5,
            preset="debug",
        )
        with patch.object(tak_server, "run_probe") as run_probe:
            tak_server.load(args)
        report, data = run_probe.call_args.args[1:]
        self.assertTrue(str(report).endswith("load-debug-quic.json"))
        self.assertEqual(
            data,
            {"seconds": 30, "rate": 4, "objects": 2, "client": "quic", "minimumRatio": 0.5},
        )


if __name__ == "__main__":
    unittest.main()
