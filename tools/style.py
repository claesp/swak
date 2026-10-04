#!/usr/bin/env python3
"""Check the first-party source style, or apply it with --fix."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
GENERATED = Path("src/core/ui_icon_paths.inc")


def source_files(root):
    """Select source files without build output, fixtures, or vendor files."""
    cpp, python, cmake = [], [], [root / "CMakeLists.txt"]
    for directory in ("src", "tests", "tools", "cmake"):
        for path in sorted((root / directory).rglob("*")):
            if not path.is_file() or path.is_symlink():
                continue
            relative = path.relative_to(root)
            if relative == GENERATED or relative.parts[:2] == ("tests", "fixtures"):
                continue
            if path.suffix in (".cpp", ".h"):
                cpp.append(path)
            elif path.suffix == ".py":
                python.append(path)
            elif path.suffix == ".cmake":
                cmake.append(path)
    return cpp, python, cmake


def run(command, root):
    subprocess.run(command, cwd=root, check=True, timeout=120)


def check_versions(clang, root):
    commands = (
        ([clang, "--version"], r"version 19\."),
        ([sys.executable, "-m", "black", "--version"], r"black, 25\.1\.0\b"),
        ([sys.executable, "-m", "cmakelang.format", "--version"], r"^0\.6\.13\s*$"),
    )
    for command, pattern in commands:
        result = subprocess.run(
            command, cwd=root, check=True, capture_output=True, text=True, timeout=15
        )
        if not re.search(pattern, result.stdout):
            raise RuntimeError("Formatter version mismatch. Read docs/coding-style.md.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fix", action="store_true", help="Apply the project format.")
    args = parser.parse_args()
    clang = os.environ.get("CLANG_FORMAT") or shutil.which("clang-format-19")
    if not clang:
        parser.error("clang-format-19 is required. Read docs/coding-style.md.")
    try:
        check_versions(clang, ROOT)
        cpp, python, cmake = source_files(ROOT)
        run(
            [
                clang,
                "--style=file",
                *(["-i"] if args.fix else ["--dry-run", "--Werror"]),
                *map(str, cpp),
            ],
            ROOT,
        )
        for path in python:
            run(
                [sys.executable, "-m", "black", *([] if args.fix else ["--check"]), str(path)],
                ROOT,
            )
        run(
            [
                sys.executable,
                "-m",
                "cmakelang.format",
                *(["-i"] if args.fix else ["--check"]),
                *map(str, cmake),
            ],
            ROOT,
        )
    except (OSError, subprocess.SubprocessError, RuntimeError) as error:
        print(f"Style check failed: {error}", file=sys.stderr)
        return 1
    print("First-party source style passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
