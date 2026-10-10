"""Start the swak executable, and stop it with a signal.

Without --gui, the test needs no display. The minimal Qt platform has no OpenGL, so startup
shows its error and waits. With --gui, the window opens on the current display.
"""

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

EXECUTABLE = None
GUI = False


def start(*arguments, platform="minimal", home=None):
    environment = dict(os.environ, QT_QPA_PLATFORM=platform)
    if home:
        environment.update(HOME=home, XDG_CONFIG_HOME=home, XDG_CACHE_HOME=home)
    return subprocess.Popen(
        [str(EXECUTABLE), *arguments],
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def stop(process, number, seconds=5):
    """Wait until startup is done, then send the signal. Return the exit status."""
    time.sleep(seconds)
    if process.poll() is not None:
        raise AssertionError(f"swak stopped early: {process.communicate()}")
    process.send_signal(number)
    try:
        process.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate()
        raise AssertionError("swak did not stop after the signal.")
    return process.returncode


class StartupTest(unittest.TestCase):
    def test_options(self):
        version = subprocess.run(
            [str(EXECUTABLE), "--version"],
            env=dict(os.environ, QT_QPA_PLATFORM="minimal"),
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(version.returncode, 0)
        self.assertRegex(version.stdout, r"^swak \d+\.\d+\.\d+")
        help_text = subprocess.run(
            [str(EXECUTABLE), "--help"],
            env=dict(os.environ, QT_QPA_PLATFORM="minimal"),
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(help_text.returncode, 0)
        self.assertIn("--preferences <path>", help_text.stdout)
        wrong = subprocess.run(
            [str(EXECUTABLE), "--no-such-option"],
            env=dict(os.environ, QT_QPA_PLATFORM="minimal"),
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(wrong.returncode, 1)
        self.assertIn("no-such-option", wrong.stderr)

    def test_startup_failure(self):
        # Without OpenGL, startup shows its error. A signal closes the message and stops swak
        # with status 1. No preferences are written.
        with tempfile.TemporaryDirectory() as home:
            preferences = Path(home) / "preferences.json"
            for number in (signal.SIGTERM, signal.SIGINT):
                with self.subTest(signal=number):
                    process = start("--preferences", str(preferences), home=home)
                    self.assertEqual(stop(process, number, 3), 1)
            self.assertFalse(preferences.exists())

    @unittest.skipUnless("--gui" in sys.argv or GUI, "needs a display")
    def test_window(self):
        # The window opens. A signal closes it through the event loop, and swak stops with
        # status 0. Without the handler, the signal would stop swak with -15 or -2 and skip the
        # window destructor. Startup already writes the preferences file, so the file only shows
        # that the saved document stays valid.
        with tempfile.TemporaryDirectory() as home:
            preferences = Path(home) / "preferences.json"
            for number in (signal.SIGTERM, signal.SIGINT):
                with self.subTest(signal=number):
                    preferences.unlink(missing_ok=True)
                    process = start("--preferences", str(preferences), platform="xcb", home=home)
                    self.assertEqual(stop(process, number, 8), 0)
                    self.assertIsInstance(json.loads(preferences.read_text()), dict)


def main():
    global EXECUTABLE, GUI
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--gui", action="store_true", help="also open the window")
    args, rest = parser.parse_known_args()
    EXECUTABLE = args.executable.resolve()
    GUI = args.gui
    unittest.main(argv=[sys.argv[0], *rest])


if __name__ == "__main__":
    main()
