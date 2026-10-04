#!/usr/bin/env python3
"""Test two LAN clients inside a temporary Linux network namespace."""
import argparse
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, default=Path("build/debug/swak_server_tests"))
    parser.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not args.inside:
        environment = os.environ.copy()
        environment["SWAK_LAN_PARENT_NAMESPACE"] = str(os.stat("/proc/self/ns/net").st_ino)
        return subprocess.run(
            [
                "unshare",
                "--user",
                "--map-root-user",
                "--net",
                sys.executable,
                str(Path(__file__).resolve()),
                "--inside",
                "--executable",
                str(executable),
            ],
            env=environment,
            timeout=30,
        ).returncode
    parent_namespace = os.environ.get("SWAK_LAN_PARENT_NAMESPACE")
    if not parent_namespace or parent_namespace == str(os.stat("/proc/self/ns/net").st_ino):
        parser.error("Run this tool without --inside to create an isolated network namespace.")
    for command in [
        ["ip", "link", "set", "lo", "up"],
        ["ip", "link", "add", "swak-test0", "type", "dummy"],
        ["ip", "address", "add", "192.0.2.1/24", "brd", "+", "dev", "swak-test0"],
        ["ip", "address", "add", "192.0.2.2/24", "brd", "+", "dev", "swak-test0"],
        ["ip", "link", "set", "swak-test0", "up"],
    ]:
        subprocess.run(command, check=True, timeout=5)
    env = os.environ.copy()
    env["SWAK_TEST_LAN"] = "1"
    env["QT_QPA_PLATFORM"] = "offscreen"
    return subprocess.run([str(executable), "lanPeers"], env=env, timeout=20).returncode


if __name__ == "__main__":
    sys.exit(main())
