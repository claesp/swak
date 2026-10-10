"""Test the configuration that tools/tak_server.py writes for the TAK test deployment."""

import argparse
import base64
import contextlib
import hashlib
import http.cookiejar
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
# A JWT that expired at the epoch. The tests do not check its signature.
EXPIRED = "e30." + base64.urlsafe_b64encode(b'{"exp": 0}').decode().rstrip("=") + ".c2ln"


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

    def test_server_b(self):
        text = tak_server.core_config(tak_server.SERVER_B)
        root = parse(text)
        ldap = next(root.iter(NAMESPACE + "ldap"))
        self.assertEqual(ldap.get("style"), "DS")
        self.assertEqual(ldap.get("url"), "ldaps://swak-tak-ldap:636/")
        self.assertTrue(ldap.get("ldapsTruststoreFile").endswith("truststore-root.jks"))
        self.assertEqual(next(root.iter(NAMESPACE + "auth")).get("default"), "ldap")
        self.assertEqual(self.outgoing(root), [(tak_server.SERVER, "9001")])
        self.assertIn("swak-tak-db-b:5432", text)
        self.assertIn("takserver-b.jks", text)
        self.assertIsNone(next(root.iter(NAMESPACE + "static"), None))

    def test_hub_federation(self):
        def connections(server):
            root = parse(tak_server.core_config(server, "hub"))
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
        data = json.loads(tak_server.profile(False, {"seconds": 5}).read_text())
        self.assertEqual(data["left"]["host"], "localhost")
        self.assertEqual(data["left"]["port"], tak_server.PORTS[8089])
        self.assertEqual(data["quic"]["port"], tak_server.UDP_PORTS[8090])
        self.assertEqual(data["federation"]["port"], tak_server.PORTS_B[8089])
        self.assertEqual(data["federation"]["enrollment"]["username"], "swak-ldap")
        self.assertEqual(data["federation"]["group"], "SwakLdap")
        self.assertNotIn("multicast", data)
        self.assertEqual(data["load"], {"seconds": 5})
        # The profile names environment variables, never passwords.
        self.assertNotIn(tak_server.DIRECTORY["password"], json.dumps(data))

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
            default = {"federation": "direct", "hubDisruption": False}
            self.assertEqual(tak_server.deployment(), default)
            tak_server.DEPLOYMENT.write_text('{"federation": "hub", "hubDisruption": true}')
            self.assertEqual(tak_server.deployment(), {"federation": "hub", "hubDisruption": True})
            # Only a JSON true turns replay on.
            tak_server.DEPLOYMENT.write_text('{"hubDisruption": "yes"}')
            self.assertFalse(tak_server.deployment()["hubDisruption"])
            tak_server.DEPLOYMENT.write_text('{"federation": "mesh"}')
            self.assertEqual(tak_server.deployment(), default)
            tak_server.DEPLOYMENT.write_text("[1]")
            self.assertEqual(tak_server.deployment(), default)
            tak_server.DEPLOYMENT.write_text("{")
            self.assertEqual(tak_server.deployment(), default)


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
        policy = self.hub(tak_server.hub_policy, ["listed.token"])
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
        # The token group lists only the given tokens and has the edges of server B.
        tokens = {
            cell["id"]: cell for cell in cells if cell["graphType"] == "FederationTokenGroupCell"
        }
        self.assertEqual(len(tokens), 1)
        token_group = next(iter(tokens.values()))["roger_federation"]
        self.assertEqual(token_group["name"], tak_server.TOKEN_GROUP)
        self.assertFalse(token_group["interconnected"])
        self.assertEqual(token_group["tokens"], [{"token": "listed.token", "expiration": -1}])
        named = {**groups, **tokens}
        rules = {}
        for edge in edges:
            source = named[edge["source"]["id"]]["roger_federation"]["name"]
            target = named[edge["target"]["id"]]["roger_federation"]["name"]
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
                (group_a, tak_server.TOKEN_GROUP): ("allowed", [tak_server.FEDERATION_GROUP]),
                (tak_server.TOKEN_GROUP, group_a): ("allGroups", []),
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
        self.assertTrue(arguments[-4].endswith("takserver-b.pem"))
        self.assertTrue(arguments[-3].endswith("takserver.pem"))
        self.assertEqual(arguments[-2], str(tak_server.HUB_TOKEN_PORT))
        self.assertEqual(arguments[-1], "probe/tokens.json")
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


class HubOptionsTest(unittest.TestCase):
    def test_manual_token(self):
        root = parse(tak_server.core_config(tak_server.SERVER_B, "hub", "a.b.c"))
        outgoing = next(root.iter(NAMESPACE + "federation-outgoing"))
        self.assertEqual(outgoing.get("tokenType"), "manual")
        self.assertEqual(outgoing.get("connectionToken"), "a.b.c")
        self.assertEqual(outgoing.get("port"), str(tak_server.HUB_TOKEN_PORT))

    def test_broker_template(self):
        # prepare_hub turns replay off by replacing this exact line.
        template = (tak_server.TEMPLATES / "hub" / "federation-hub-broker.yml").read_text()
        self.assertEqual(template.count("missionFederationDisruptionEnabled: true"), 1)
        self.assertIn(f"dbHost: {tak_server.HUB_DB}", template)
        self.assertIn(f"port: {tak_server.HUB_TOKEN_PORT}", template)
        self.assertIn("missionFederationDisruptionMaxFileSizeBytes: 4096", template)

    def test_disruption_checks(self):
        for output, passed in (("27", True), ("0", False), ("", False), ("error", False)):
            with (
                self.subTest(output=output),
                patch.object(tak_server, "mongo", return_value=output),
            ):
                check = tak_server.hub_disruption_checks()["hubDisruptionStored"]
                self.assertEqual(check["passed"], passed)

    def test_disruption_profile(self):
        with (
            tempfile.TemporaryDirectory() as directory,
            patch.object(tak_server, "WORK", Path(directory)),
        ):
            data = json.loads(
                tak_server.profile(True, federation="hub", disruption=True).read_text()
            )
            self.assertTrue(data["federation"]["hubDisruption"])
            self.assertEqual(data["federation"]["outgoing"], tak_server.HUB_CONNECTION)
            data = json.loads(tak_server.profile(True, disruption=True).read_text())
            # Replay applies only to the hub. Direct federation turns off the link to server A.
            self.assertFalse(data["federation"]["hubDisruption"])
            self.assertEqual(data["federation"]["outgoing"], "swak-a")


class HubOAuthTest(unittest.TestCase):
    def test_checks(self):
        statuses = {
            ("/api/isAdmin", "a.b.c"): (200, ""),
            ("/api/isAdmin", "v.w.x"): (403, ""),
            ("/api/isAdmin", "a.b.A"): (401, ""),
            ("/api/isAdmin", None): (401, ""),
            ("/api/oauth/login/auth", None): (
                302,
                f"https://{tak_server.KEYCLOAK}:8443/realms/swak/protocol/openid-connect/auth?"
                "response_type=code&client_id=swak-hub",
            ),
        }
        tokens = {tak_server.OAUTH_ADMIN: "a.b.c", tak_server.OAUTH_VIEWER: "v.w.x"}
        with (
            patch.object(tak_server, "oauth_token", side_effect=tokens.get),
            patch.object(
                tak_server,
                "oauth_status",
                side_effect=lambda path, token=None: statuses[(path, token)],
            ),
            patch.object(tak_server, "hub_oauth_browser_checks", return_value={}),
        ):
            checks = tak_server.hub_oauth_checks()
        self.assertEqual(
            set(checks),
            {
                "hubOAuthToken",
                "hubOAuthAdmin",
                "hubOAuthNotAdmin",
                "hubOAuthTampered",
                "hubOAuthNoToken",
                "hubOAuthLoginRedirect",
            },
        )
        self.assertTrue(all(check["passed"] for check in checks.values()), checks)

    def test_wrong_answers_fail(self):
        # An administrator token that the UI refuses, and a redirect to another server.
        with (
            patch.object(tak_server, "oauth_token", return_value="a.b.c"),
            patch.object(tak_server, "oauth_status", return_value=(302, "https://elsewhere/")),
            patch.object(tak_server, "hub_oauth_browser_checks", return_value={}),
        ):
            checks = tak_server.hub_oauth_checks()
        self.assertFalse(checks["hubOAuthAdmin"]["passed"])
        self.assertFalse(checks["hubOAuthLoginRedirect"]["passed"])
        # Without tokens, only the token check remains, and it fails.
        with patch.object(tak_server, "oauth_token", side_effect=OSError("refused")):
            checks = tak_server.hub_oauth_checks()
        self.assertEqual(list(checks), ["hubOAuthToken"])
        self.assertFalse(checks["hubOAuthToken"]["passed"])

    @staticmethod
    def jar(**cookies):
        jar = http.cookiejar.CookieJar()
        for name, value in cookies.items():
            jar.set_cookie(
                http.cookiejar.Cookie(
                    0, name, value, None, False, "localhost.local", False, False, "/", True,
                    True, None, False, None, None, {},
                )
            )  # fmt: skip
        return jar

    def browser_checks(self, statuses, signed_in=True):
        """Run the browser checks with a sign-in result and mocked answers of the UI."""
        admin = self.jar(access_token=EXPIRED, refresh_token="r")
        browsers = {
            tak_server.OAUTH_ADMIN: (admin, "" if signed_in else "code exchange: HTTP 500"),
            tak_server.OAUTH_VIEWER: (self.jar(access_token="viewer"), ""),
            (tak_server.OAUTH_ADMIN[0], "wrong-password"): (
                self.jar(),
                "Keycloak sign-in: HTTP 200",
            ),
        }

        def status(path, token=None, jar=None, refresh=None):
            if jar is not None:
                # The UI refreshes the expired token and sets a new one.
                jar.clear()
                jar.set_cookie(next(iter(self.jar(access_token="renewed"))))
                return statuses["refresh"], ""
            return statuses[(path, token, refresh)]

        with (
            patch.object(tak_server, "oauth_browser", side_effect=lambda user: browsers[user]),
            patch.object(tak_server, "oauth_status", side_effect=status),
            patch.object(tak_server.time, "sleep"),
        ):
            return tak_server.hub_oauth_browser_checks()

    def test_browser(self):
        checks = self.browser_checks(
            {
                # The first administrator check and the expired check use the same token.
                # Here the UI accepts it the first time and signs out the second time.
                ("/api/isAdmin", EXPIRED, None): (200, ""),
                ("/api/isAdmin", "viewer", None): (403, ""),
                ("/api/oauth/login/redirect?code=swak&state=other", None, None): (400, ""),
                "refresh": 200,
                ("/api/isAdmin", EXPIRED, "swak.invalid"): (301, "/"),
            }
        )
        self.assertEqual(
            set(checks),
            {
                "hubOAuthBrowserAdmin",
                "hubOAuthBrowserNotAdmin",
                "hubOAuthBrowserWrongPassword",
                "hubOAuthStateMismatch",
                "hubOAuthRefresh",
                "hubOAuthRefreshRefused",
                "hubOAuthExpired",
            },
        )
        # The mock answers 200 to the expired check, so only that check fails.
        failed = {name for name, check in checks.items() if not check["passed"]}
        self.assertEqual(failed, {"hubOAuthExpired"})

    def test_browser_failures(self):
        # A failed sign-in skips the refresh checks. A redirect that signs in on a wrong state
        # fails, and so does a viewer that the UI admits.
        checks = self.browser_checks(
            {
                ("/api/oauth/login/redirect?code=swak&state=other", None, None): (302, "/"),
                ("/api/isAdmin", "viewer", None): (200, ""),
                ("/api/isAdmin", EXPIRED, None): (200, ""),
            },
            signed_in=False,
        )
        for name in ("hubOAuthBrowserAdmin", "hubOAuthBrowserNotAdmin", "hubOAuthStateMismatch"):
            self.assertFalse(checks[name]["passed"], name)
        self.assertTrue(checks["hubOAuthBrowserWrongPassword"]["passed"])
        self.assertNotIn("hubOAuthRefresh", checks)

    def test_jwt_claims(self):
        self.assertEqual(tak_server.jwt_claims(EXPIRED), {"exp": 0})

    def test_login_form(self):
        form = tak_server.LoginForm()
        form.feed(
            '<form id="other" action="/a"></form>'
            '<form id="kc-form-login" action="/b?x=1&amp;y=2"></form>'
        )
        # The parser decodes the entities of attribute values.
        self.assertEqual(form.action, "/b?x=1&y=2")

    def test_configuration(self):
        # The UI must find all eight Keycloak values, or it does not open the OAuth port.
        text = (tak_server.TEMPLATES / "hub" / "federation-hub-ui.yml").read_text()
        values = dict(
            line.split(": ", 1) for line in text.splitlines() if ": " in line and line[0] != "#"
        )
        self.assertEqual(values["allowOauth"], "true")
        self.assertEqual(values["oauthPort"], "9104")
        self.assertEqual(values["keycloakClientId"], tak_server.OAUTH_CLIENT[0])
        self.assertEqual(values["keycloakSecret"], tak_server.OAUTH_CLIENT[1])
        self.assertIn(
            f"{tak_server.HUB_OAUTH_PORT}/api/oauth/login/redirect", values["keycloakrRedirectUri"]
        )
        for key in (
            "keycloakAdminClaimValue",
            "keycloakConfigurationEndpoint",
            "keycloakClaimName",
            "keycloakTlsCertFile",
            "keycloakServerName",
        ):
            self.assertTrue(values.get(key), key)
        realm = json.loads((tak_server.TEMPLATES / "keycloak" / "swak-realm.json").read_text())
        client = realm["clients"][0]
        self.assertEqual((client["clientId"], client["secret"]), tak_server.OAUTH_CLIENT)
        self.assertIn(values["keycloakrRedirectUri"], client["redirectUris"])
        mapper = client["protocolMappers"][0]["config"]
        self.assertEqual(mapper["claim.name"], values["keycloakClaimName"])
        users = {user["username"]: user for user in realm["users"]}
        self.assertEqual(
            users[tak_server.OAUTH_ADMIN[0]]["realmRoles"], [values["keycloakAdminClaimValue"]]
        )
        self.assertNotIn("realmRoles", users[tak_server.OAUTH_VIEWER[0]])


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


def completed(code=0, stdout="", stderr=""):
    return subprocess.CompletedProcess([], code, stdout=stdout, stderr=stderr)


class CommandTest(unittest.TestCase):
    """Test the command helpers without Podman. Each failure must stop the script."""

    def setUp(self):
        quiet = patch("sys.stdout", new_callable=io.StringIO)
        quiet.start()
        self.addCleanup(quiet.stop)

    def test_run(self):
        with patch.object(tak_server.subprocess, "run", return_value=completed()) as run:
            self.assertEqual(tak_server.run(["true"]).returncode, 0)
            self.assertFalse(run.call_args.kwargs["check"])
        with patch.object(tak_server.subprocess, "run", return_value=completed(3)):
            with self.assertRaisesRegex(SystemExit, "status 3"):
                tak_server.run(["false"])

    def test_podman_without_check(self):
        # Without check, a failed command returns its result and does not stop the script.
        with patch.object(tak_server.subprocess, "run", return_value=completed(1)) as run:
            self.assertEqual(tak_server.podman("inspect", "x", check=False).returncode, 1)
        self.assertEqual(run.call_args.args[0], ["podman", "inspect", "x"])

    def test_network_subnet(self):
        for result, expected in (
            (completed(0, "10.89.77.0/24\n"), "10.89.77.0/24"),
            (completed(125, "", "no such network"), None),
        ):
            with self.subTest(expected=expected):
                with patch.object(tak_server.subprocess, "run", return_value=result):
                    self.assertEqual(tak_server.network_subnet(), expected)

    def test_bcrypt_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(tak_server, "RUN", Path(directory)):
                with patch.object(
                    tak_server.subprocess, "run", return_value=completed(0, "$2a$10$abc\n")
                ) as run:
                    self.assertEqual(tak_server.bcrypt_hash("secret"), "$2a$10$abc")
                # The password goes in the environment, not in the shell script.
                command = run.call_args.args[0]
                self.assertIn("PASSWORD_TO_HASH=secret", command)
                self.assertNotIn("secret", command[-1])
                for result in (completed(1, "", "no java"), completed(0, "error")):
                    with patch.object(tak_server.subprocess, "run", return_value=result):
                        with self.assertRaisesRegex(SystemExit, "Cannot hash"):
                            tak_server.bcrypt_hash("secret")

    def test_fetch_existing_source(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            (source / ".git").mkdir()
            with (
                patch.object(tak_server, "SOURCE", source),
                patch.object(tak_server, "run") as run,
            ):
                tak_server.fetch(None)
            run.assert_not_called()

    def test_podman_unshare(self):
        with patch.object(tak_server.subprocess, "run", return_value=completed(2)) as run:
            self.assertEqual(tak_server.podman_unshare("ip", check=False).returncode, 2)
            with self.assertRaisesRegex(SystemExit, "Command failed: ip"):
                tak_server.podman_unshare("ip")
        self.assertEqual(run.call_args.args[0][:3], ["podman", "unshare", "--rootless-netns"])

    def impair(self, loss, delay, addresses):
        calls = []

        def unshare(*command, capture=False, check=True):
            calls.append((command, check))
            return completed(0, addresses if capture else "")

        with (
            patch.object(tak_server, "podman", return_value=completed(0, "4242\n")),
            patch.object(tak_server, "podman_unshare", side_effect=unshare),
            patch.object(tak_server.shutil, "which", return_value="/sbin/tc"),
        ):
            tak_server.impair(loss, delay)
        return calls[1:]

    def test_impair(self):
        addresses = (
            "1: lo    inet 127.0.0.1/8 scope host lo\n"
            "3: podman1    inet 10.89.77.1/24 brd 10.89.77.255 scope global podman1\n"
        )
        calls = self.impair(5, 100, addresses)
        rule = ("root", "netem", "loss", "5%", "delay", "100ms")
        self.assertEqual(
            calls,
            [
                (("/sbin/tc", "qdisc", "replace", "dev", "podman1", *rule), True),
                (
                    ("nsenter", "-t", "4242", "-n", "/sbin/tc", "qdisc", "replace", "dev", "eth0")
                    + rule,
                    True,
                ),
            ],
        )
        # Zero values remove the rules. A missing rule must not stop the script.
        calls = self.impair(0, 0, addresses)
        self.assertEqual([command[-4] for command, _ in calls], ["del", "del"])
        self.assertEqual([check for _, check in calls], [False, False])
        with self.assertRaisesRegex(SystemExit, "Podman bridge"):
            self.impair(5, 0, "1: lo    inet 127.0.0.1/8 scope host lo\n")

    def test_down_and_status(self):
        with patch.object(tak_server, "podman", return_value=completed(1)) as podman:
            tak_server.down(None)
        # Removal continues when a container or the network is absent.
        self.assertEqual([call.kwargs["check"] for call in podman.call_args_list], [False, False])
        with (
            patch.object(tak_server, "podman", return_value=completed(1)),
            patch.object(tak_server, "wait_port", return_value=False),
        ):
            tak_server.status(None)
        output = tak_server.sys.stdout.getvalue()
        self.assertIn(f"{tak_server.SERVER} absent", output)
        self.assertIn("port 18443 (8443): closed", output)

    def probe_args(self, **options):
        return argparse.Namespace(**{"preset": "debug", "loopback": False, "delay": 0, **options})

    def test_run_probe_without_build(self):
        with tempfile.TemporaryDirectory() as directory:
            with (
                patch.object(tak_server, "ROOT", Path(directory)),
                self.assertRaisesRegex(SystemExit, "Build swak_tak_compat"),
            ):
                tak_server.run_probe(self.probe_args(loss=0), Path(directory) / "report.json")

    def run_probe(self, state, probe_code, checks, load=None, loss=0, side_effect=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "build" / "debug").mkdir(parents=True)
            (root / "build" / "debug" / "swak_tak_compat").touch()
            report = root / "report.json"

            def probe(command, stdout, **_options):
                if side_effect:
                    raise side_effect
                stdout.write(json.dumps({"passed": probe_code == 0, "features": {}}))
                return completed(probe_code)

            with (
                patch.object(tak_server, "ROOT", root),
                patch.object(tak_server, "deployment", return_value=state),
                patch.object(tak_server, "profile", return_value=root / "profile.json"),
                patch.object(tak_server.subprocess, "run", side_effect=probe) as run,
                patch.object(tak_server, "impair") as impair,
                patch.object(tak_server, "hub_token_checks", return_value=checks),
                patch.object(tak_server, "hub_oauth_checks", return_value={}),
                patch.object(tak_server, "hub_disruption_checks", return_value={}) as replay,
            ):
                try:
                    with self.assertRaises(SystemExit) as stop:
                        tak_server.run_probe(self.probe_args(loss=loss), report, load)
                finally:
                    self.impair_calls = impair.call_args_list
                    self.replay_called = replay.called
                    self.command = run.call_args.args[0] if run.called else None
            return stop.exception.code, json.loads(report.read_text() or "{}")

    def test_run_probe_hub(self):
        direct = {"federation": "direct", "hubDisruption": False}
        hub = {"federation": "hub", "hubDisruption": False}
        failed = {"hubToken": {"passed": False, "detail": "x"}}
        passed = {"hubToken": {"passed": True, "detail": "x"}}
        code, report = self.run_probe(direct, 0, failed)
        self.assertEqual(code, 0)
        self.assertNotIn("hubToken", report["features"])
        self.assertEqual(self.command[:3], ["podman", "unshare", "--rootless-netns"])
        # A failed hub check fails a probe run that passed.
        code, report = self.run_probe(hub, 0, failed)
        self.assertEqual((code, report["failedFeatures"]), (1, ["hubToken"]))
        self.assertFalse(self.replay_called)
        # A failed probe stays failed when the hub checks pass.
        code, _ = self.run_probe(hub, 2, passed)
        self.assertEqual(code, 1)
        self.run_probe({"federation": "hub", "hubDisruption": True}, 0, passed)
        self.assertTrue(self.replay_called)
        # A load run does not add the hub checks.
        code, report = self.run_probe(hub, 0, failed, load={"seconds": 1})
        self.assertEqual(code, 0)
        self.assertNotIn("hubToken", report["features"])

    def test_run_probe_removes_impairment(self):
        self.run_probe({"federation": "direct", "hubDisruption": False}, 0, {}, loss=5)
        self.assertEqual([call.args for call in self.impair_calls], [(5, 0), (0, 0)])
        # The script removes the rules also when the probe cannot start.
        with self.assertRaises(OSError):
            self.run_probe(
                {"federation": "direct", "hubDisruption": False},
                0,
                {},
                loss=5,
                side_effect=OSError("exec format error"),
            )
        self.assertEqual([call.args for call in self.impair_calls], [(5, 0), (0, 0)])

    def main(self, *arguments):
        with (
            patch.object(tak_server.sys, "argv", ["tak_server.py", *arguments]),
            patch("sys.stderr", new_callable=io.StringIO),
        ):
            tak_server.main()

    def test_main(self):
        with patch.object(tak_server, "up") as up:
            self.main("up", "--federation", "hub", "--hub-disruption")
        args = up.call_args.args[0]
        self.assertEqual(
            (args.federation, args.hub_disruption, args.hub_token, args.timeout),
            ("hub", True, "automatic", 420),
        )
        with patch.object(tak_server, "load") as load:
            self.main("load", "--client", "quic", "--loopback")
        args = load.call_args.args[0]
        self.assertEqual(
            (args.client, args.loopback, args.preset, args.minimum), ("quic", True, "debug", None)
        )
        for arguments in ((), ("up", "--federation", "mesh"), ("test", "--preset", "fast")):
            with self.subTest(arguments=arguments), self.assertRaises(SystemExit) as stop:
                self.main(*arguments)
            self.assertEqual(stop.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
