"""Test source selection and formatter failures."""

import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "style", Path(__file__).resolve().parents[1] / "tools/style.py"
)
style = importlib.util.module_from_spec(spec)
spec.loader.exec_module(style)


class StyleTest(unittest.TestCase):
    def test_first_party_inventory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            names = (
                "src/core/a.h",
                "src/app/a.cpp",
                "tests/test_a.cpp",
                "tools/tool.py",
                "cmake/version.cmake",
                "src/core/ui_icon_paths.inc",
                "tests/fixtures/input.xml",
                "tests/fixtures/input.cpp",
                "third_party/vendor.h",
                "build/generated.h",
            )
            for name in names:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("")
            (root / "src/linked.h").symlink_to(root / "third_party/vendor.h")
            cpp, python, cmake = style.source_files(root)
            self.assertEqual(
                {str(path.relative_to(root)) for path in cpp},
                {"src/core/a.h", "src/app/a.cpp", "tests/test_a.cpp"},
            )
            self.assertEqual(python, [root / "tools/tool.py"])
            self.assertEqual(cmake, [root / "CMakeLists.txt", root / "cmake/version.cmake"])

    def test_version_rejection(self):
        result = subprocess.CompletedProcess([], 0, stdout="clang-format version 18.1")
        with patch.object(style.subprocess, "run", return_value=result):
            with self.assertRaisesRegex(RuntimeError, "version mismatch"):
                style.check_versions("clang-format", style.ROOT)

    def test_expected_versions(self):
        results = [
            subprocess.CompletedProcess([], 0, stdout=value)
            for value in (
                "Debian clang-format version 19.1.7",
                "python -m black, 25.1.0 (compiled: yes)",
                "0.6.13",
            )
        ]
        with patch.object(style.subprocess, "run", side_effect=results):
            style.check_versions("clang-format", style.ROOT)

    def test_missing_formatter(self):
        with (
            patch.dict(style.os.environ, {}, clear=True),
            patch.object(style.shutil, "which", return_value=None),
            patch.object(style.sys, "argv", ["style.py"]),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            with self.assertRaises(SystemExit) as error:
                style.main()
            self.assertEqual(error.exception.code, 2)

    def test_check_and_fix_are_separate(self):
        for fix in (False, True):
            with (
                self.subTest(fix=fix),
                patch.dict(style.os.environ, {"CLANG_FORMAT": "/tool/clang-format"}),
                patch.object(style, "check_versions"),
                patch.object(
                    style,
                    "source_files",
                    return_value=([Path("a.cpp")], [Path("a.py")], [Path("a.cmake")]),
                ),
                patch.object(style, "run") as run,
                patch.object(style.sys, "argv", ["style.py"] + (["--fix"] if fix else [])),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(style.main(), 0)
                commands = [call.args[0] for call in run.call_args_list]
                self.assertEqual(len(commands), 3)
                self.assertEqual("-i" in commands[0], fix)
                self.assertEqual("--dry-run" in commands[0], not fix)
                self.assertEqual("--check" in commands[1], not fix)
                self.assertEqual("-i" in commands[2], fix)

    def test_main_reports_formatter_failure(self):
        with (
            patch.dict(style.os.environ, {"CLANG_FORMAT": "/tool/clang-format"}),
            patch.object(style, "check_versions", side_effect=RuntimeError("Version mismatch")),
            patch.object(style.sys, "argv", ["style.py"]),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(style.main(), 1)

    def test_command_failure(self):
        with patch.object(
            style.subprocess, "run", side_effect=subprocess.CalledProcessError(1, ["formatter"])
        ):
            with self.assertRaises(subprocess.CalledProcessError):
                style.run(["formatter"], style.ROOT)


if __name__ == "__main__":
    unittest.main()
