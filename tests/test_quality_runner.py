"""Test diagnostic failure reporting and the external time limit."""

import importlib.util
import gzip
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "quality", Path(__file__).resolve().parents[1] / "tools/quality.py"
)
quality = importlib.util.module_from_spec(spec)
spec.loader.exec_module(quality)


class RunnerTest(unittest.TestCase):
    def test_success_and_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "run.log"
            quality.run([sys.executable, "-c", 'print("evidence")'], log, 5)
            self.assertIn("evidence", log.read_text())
            with self.assertRaisesRegex(RuntimeError, "status 7"):
                quality.run([sys.executable, "-c", "raise SystemExit(7)"], log, 5)

    def test_busy_loop_timeout(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "run.log"
            start = time.monotonic()
            with self.assertRaisesRegex(RuntimeError, "stopped"):
                quality.run([sys.executable, "-c", "while True: pass"], log, 0.2)
            self.assertLess(time.monotonic() - start, 5)
            self.assertIn("time limit", log.read_text())

    def test_coverage_merges_shared_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory) / "build"
            report = Path(directory) / "report"
            build.mkdir()
            report.mkdir()
            for name in ("one", "two"):
                (build / f"{name}.gcno").touch()

            def gcov(command, *args):
                count = 3 if command[-1].stem == "two" else 0
                data = {
                    "current_working_directory": str(quality.ROOT),
                    "files": [
                        {
                            "file": "src/example.cpp",
                            "lines": [
                                {"line_number": 7, "count": count, "branches": [{"count": count}]},
                                {"line_number": 8, "count": 0},
                            ],
                            "functions": [{"name": "example", "execution_count": count}],
                        },
                        {"file": "/usr/include/external.h", "lines": [], "functions": []},
                    ],
                }
                with gzip.open(report / "example.gcov.json.gz", "wt") as output:
                    json.dump(data, output)

            with patch.object(quality, "run", gcov):
                quality.coverage_report(build, report, {})
            result = json.loads((report / "coverage.json").read_text())
            self.assertEqual(list(result), ["example.cpp"])
            self.assertEqual(result["example.cpp"]["lines"], {"7": 3, "8": 0})
            self.assertEqual(result["example.cpp"]["branches"], {"7:0": 3})
            self.assertIn("Unexecuted lines: 8", (report / "coverage.md").read_text())


if __name__ == "__main__":
    unittest.main()
