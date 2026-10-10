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
        def connections(server):
            root = parse(tak_server.core_config(server, "openldap", "hub"))
            return {
                item.get("displayName"): item
                for item in root.iter(NAMESPACE + "federation-outgoing")
            }

        # Server A uses its certificate on the v2 port.
        a = connections(tak_server.SERVER)
        self.assertEqual(set(a), {tak_server.HUB_CONNECTION})
        self.assertEqual(a[tak_server.HUB_CONNECTION].get("port"), "9102")
        self.assertIsNone(a[tak_server.HUB_CONNECTION].get("useToken"))
        # Server B uses an automatic token on the token port.
        b = connections(tak_server.SERVER_B)
        self.assertEqual(set(b), {tak_server.HUB_CONNECTION})
        self.assertEqual(b[tak_server.HUB_CONNECTION].get("port"), str(tak_server.HUB_TOKEN_PORT))
        self.assertEqual(b[tak_server.HUB_CONNECTION].get("useToken"), "true")
        self.assertEqual(b[tak_server.HUB_CONNECTION].get("tokenType"), "automatic")


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

    def test_hub_profile(self):
        data = json.loads(tak_server.profile(True, federation="hub").read_text())
        self.assertTrue(data["federation"]["hub"])
        self.assertTrue(data["ops"]["certificate"].endswith("ops.pem"))
        # The same Ops user connects to server B.
        self.assertEqual(data["opsB"]["certificate"], data["ops"]["certificate"])
        self.assertEqual(data["opsB"]["host"], tak_server.ADDRESS[tak_server.SERVER_B])
        self.assertTrue(data["federation"]["receiverLog"].endswith(tak_server.RECEIVER_LOG))
        self.assertEqual(data["federation"]["token"]["port"], tak_server.HUB_TOKEN_PORT)
        data = json.loads(tak_server.profile(True).read_text())
        self.assertFalse(data["federation"]["hub"])
        self.assertNotIn("token", data["federation"])

    def test_deployment(self):
        with patch.object(tak_server, "DEPLOYMENT", Path(self.directory.name) / "d.json"):
            default = {"directory": "openldap", "federation": "direct"}
            self.assertEqual(tak_server.deployment(), default)
            tak_server.DEPLOYMENT.write_text('{"directory": "ad", "federation": "hub"}')
            self.assertEqual(tak_server.deployment(), {"directory": "ad", "federation": "hub"})
            tak_server.DEPLOYMENT.write_text('{"federation": "mesh"}')
            self.assertEqual(tak_server.deployment(), default)
            tak_server.DEPLOYMENT.write_text("[1]")
            self.assertEqual(tak_server.deployment(), default)

    def test_deployed_directory(self):
        with patch.object(tak_server, "DEPLOYMENT", Path(self.directory.name) / "d.json"):
            self.assertEqual(tak_server.deployed_directory(), "openldap")
            tak_server.DEPLOYMENT.write_text('{"directory": "ad"}')
            self.assertEqual(tak_server.deployed_directory(), "ad")
            tak_server.DEPLOYMENT.write_text('{"directory": "other"}')
            self.assertEqual(tak_server.deployed_directory(), "openldap")
            tak_server.DEPLOYMENT.write_text("{")
            self.assertEqual(tak_server.deployed_directory(), "openldap")


def openssl(*arguments):
    return subprocess.run(["openssl", *arguments], check=True, capture_output=True).stdout


class HubTest(unittest.TestCase):
    def setUp(self):
        # A root and two intermediate CAs, as tak_server.py certs creates them.
        self.directory = tempfile.TemporaryDirectory()
        certs = Path(self.directory.name)
        self.certs = certs
        openssl(
            "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/O=SWAK Test/CN=SWAK Test CA",
            "-keyout", str(certs / "ca.key"), "-out", str(certs / "ca.pem"),
        )  # fmt: skip
        (certs / "ext").write_text("basicConstraints=critical,CA:TRUE,pathlen:0\n")
        for name in ("ca-a", "ca-b"):
            openssl(
                "req", "-newkey", "rsa:2048", "-nodes", "-subj", f"/O=SWAK Test/CN={name}",
                "-keyout", str(certs / f"{name}.key"), "-out", str(certs / f"{name}.csr"),
            )  # fmt: skip
            openssl(
                "x509", "-req", "-in", str(certs / f"{name}.csr"), "-days", "1",
                "-CA", str(certs / "ca.pem"), "-CAkey", str(certs / "ca.key"),
                "-CAcreateserial", "-extfile", str(certs / "ext"),
                "-out", str(certs / f"{name}.pem"),
            )  # fmt: skip

    def tearDown(self):
        self.directory.cleanup()

    def hub(self, function, *arguments):
        with (
            patch.object(tak_server, "CERTS", self.certs),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            return function(*arguments)

    def test_ca_group(self):
        # The hub names a CA group by the RFC 2253 issuer of the CA certificate and the
        # lowercase SHA-256 hex of that certificate.
        for name in ("ca", "ca-a", "ca-b"):
            with self.subTest(name=name):
                der = openssl("x509", "-in", str(self.certs / f"{name}.pem"), "-outform", "DER")
                self.assertEqual(
                    self.hub(tak_server.ca_group, name),
                    "CN=SWAK Test CA,O=SWAK Test-" + hashlib.sha256(der).hexdigest(),
                )
        self.assertNotEqual(
            self.hub(tak_server.ca_group, "ca-a"), self.hub(tak_server.ca_group, "ca-b")
        )

    def test_policy(self):
        policy = self.hub(tak_server.hub_policy)
        group_a = self.hub(tak_server.ca_group, "ca-a")
        group_b = self.hub(tak_server.ca_group, "ca-b")
        # Filters apply only between groups that are not interconnected.
        self.assertEqual(
            policy["groups"],
            [{"uid": group_a, "interconnected": False}, {"uid": group_b, "interconnected": False}],
        )
        cells = policy["additionalData"]["uiData"]["cells"]
        groups = {cell["id"]: cell for cell in cells if cell["graphType"] == "GroupCell"}
        edges = [cell for cell in cells if cell["graphType"] == "EdgeCell"]
        self.assertEqual(
            {cell["roger_federation"]["name"] for cell in groups.values()}, {group_a, group_b}
        )
        for cell in groups.values():
            self.assertFalse(cell["roger_federation"]["interconnected"])
            self.assertEqual(cell["roger_federation"]["tokenAuthDuration"], -1)
        # Only the CA group of server B may get a token.
        self.assertEqual(
            {
                cell["roger_federation"]["name"]: cell["roger_federation"]["allowTokenAuth"]
                for cell in groups.values()
            },
            {group_a: False, group_b: True},
        )
        rules = {}
        for edge in edges:
            source = groups[edge["source"]["id"]]["roger_federation"]["name"]
            target = groups[edge["target"]["id"]]["roger_federation"]["name"]
            rules[(source, target)] = (
                edge["roger_federation"]["groupsFilterType"],
                edge["roger_federation"]["allowedGroups"],
            )
        # From server A to server B, only SwakTest passes. The other direction passes all.
        self.assertEqual(
            rules,
            {
                (group_a, group_b): ("allowed", [tak_server.FEDERATION_GROUP]),
                (group_b, group_a): ("allGroups", []),
            },
        )
        # The hub fails on a missing plugin list, so each entry has every list.
        plugins = {item["pluginName"]: item for item in policy["pluginsData"]["plugins"]}
        self.assertEqual(set(plugins), {"swak-interceptor", "swak-sender", "swak-receiver"})
        for item in plugins.values():
            for key in ("sendTo", "dontSend", "dontReceive"):
                self.assertEqual(item[key], [])
            self.assertFalse(item["disabled"])
        self.assertTrue(plugins["swak-interceptor"]["receiveByDefault"])
        self.assertTrue(plugins["swak-sender"]["sendByDefault"])
        # The receiver takes messages only from the CA group of server A.
        self.assertFalse(plugins["swak-receiver"]["receiveByDefault"])
        self.assertEqual(plugins["swak-receiver"]["receiveFrom"], [group_a])


class HubTokenTest(unittest.TestCase):
    def test_token_checks(self):
        reply = subprocess.CompletedProcess(
            [], 0, 'Picked up options\n{"hubTokenForged": {"passed": true, "detail": "x"}}\n', ""
        )
        with patch.object(tak_server, "podman", return_value=reply) as podman:
            checks = tak_server.hub_token_checks()
        self.assertEqual(checks, {"hubTokenForged": {"passed": True, "detail": "x"}})
        # The probe asks for a token with the certificate of server B and then server A.
        arguments = podman.call_args.args
        self.assertEqual(arguments[3], tak_server.HUB)
        self.assertTrue(arguments[-3].endswith("takserver-b.pem"))
        self.assertTrue(arguments[-2].endswith("takserver.pem"))
        self.assertEqual(arguments[-1], str(tak_server.HUB_TOKEN_PORT))
        # A failed run becomes one failed check with the error text.
        for stdout in ("", "not json", "[1]"):
            with self.subTest(stdout=stdout):
                failed = subprocess.CompletedProcess([], 1, stdout, "error: no class")
                with patch.object(tak_server, "podman", return_value=failed):
                    checks = tak_server.hub_token_checks()
                self.assertFalse(checks["hubTokenProbe"]["passed"])

    def test_merge_checks(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "report.json"
            report.write_text(json.dumps({"passed": True, "features": {"a": {"passed": True}}}))
            self.assertEqual(tak_server.merge_checks(report, {"b": {"passed": True}}), 0)
            data = json.loads(report.read_text())
            self.assertTrue(data["passed"])
            self.assertEqual(set(data["features"]), {"a", "b"})
            self.assertEqual(tak_server.merge_checks(report, {"c": {"passed": False}}), 1)
            data = json.loads(report.read_text())
            self.assertFalse(data["passed"])
            self.assertEqual(data["failedFeatures"], ["c"])
            # A failed probe stays failed after passing hub checks.
            report.write_text(json.dumps({"passed": False, "features": {}}))
            self.assertEqual(tak_server.merge_checks(report, {"d": {"passed": True}}), 1)
            for text in ("{", "[]"):
                report.write_text(text)
                self.assertEqual(tak_server.merge_checks(report, {}), 1)


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
