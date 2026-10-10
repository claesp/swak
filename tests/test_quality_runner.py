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

    def test_soak_omits_allocation_stacks(self):
        for mode in ("quick", "gui", "coverage", "analyze"):
            self.assertNotIn("malloc_context_size", quality.asan_options(mode))
        soak = quality.asan_options("soak")
        self.assertIn("malloc_context_size=0", soak)
        self.assertIn("detect_leaks=1", soak)
        self.assertIn("halt_on_error=1", soak)

    def test_soak_warmup_scales_with_duration(self):
        self.assertEqual(quality.soak_warmup(1), 2)
        self.assertEqual(quality.soak_warmup(5), 2)
        self.assertEqual(quality.soak_warmup(600), 60)
        self.assertEqual(quality.soak_warmup(7200), 300)
        self.assertEqual(quality.soak_warmup(86400), 300)

    def test_soak_limits_exceed_qt_default(self):
        for seconds in (1, 7200, 86400):
            external, qt_limit = quality.soak_limits(seconds)
            self.assertGreater(external, seconds + quality.soak_warmup(seconds))
            self.assertGreater(qt_limit, external * 1000)
            self.assertGreaterEqual(qt_limit, 300000)

    def test_coverage_merges_shared_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory) / "build"
            report = Path(directory) / "report"
            build.mkdir()
            report.mkdir()
            source = Path(directory) / "src"
            source.mkdir()
            (source / "example.cpp").touch()
            for name in ("one", "two"):
                (build / f"{name}.gcno").touch()

            def gcov(command, *args):
                count = 3 if command[-1].stem == "two" else 0
                data = {
                    "current_working_directory": directory,
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
                        # A removed source with old objects in the build tree.
                        {
                            "file": "src/removed.cpp",
                            "lines": [{"line_number": 1, "count": 0}],
                            "functions": [],
                        },
                    ],
                }
                with gzip.open(report / "example.gcov.json.gz", "wt") as output:
                    json.dump(data, output)

            with patch.object(quality, "run", gcov), patch.object(quality, "ROOT", Path(directory)):
                quality.coverage_report(build, report, {})
            result = json.loads((report / "coverage.json").read_text())
            self.assertEqual(list(result), ["example.cpp"])
            self.assertEqual(result["example.cpp"]["lines"], {"7": 3, "8": 0})
            self.assertEqual(result["example.cpp"]["branches"], {"7:0": 3})
            self.assertIn("Unexecuted lines: 8", (report / "coverage.md").read_text())


if __name__ == "__main__":
    unittest.main()
