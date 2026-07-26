#!/usr/bin/env python3
"""Flags std:: symbols used without the header that declares them.

libc++ (macOS) pulls a lot of <algorithm> in transitively, so a missing
include builds fine locally and then fails on MSVC or libstdc++. That has
broken CI more than once; this catches it before pushing.

Deliberately shallow: a substring scan, no preprocessor, no template
awareness. It only knows about symbols that have actually caused trouble,
and it only reports a symbol when the file uses it and includes neither its
header nor a header that is allowed to stand in for it.

Usage: python3 tools/check_includes.py   (exit 1 if anything is missing)
"""

import re
import subprocess
import sys
from pathlib import Path

# symbol -> header that must be included to use it
REQUIRED = {}
for symbol in (
    "min", "max", "clamp", "sort", "stable_sort", "find", "find_if", "copy",
    "fill", "any_of", "all_of", "none_of", "count_if", "remove_if", "reverse",
    "lower_bound", "upper_bound", "min_element", "max_element",
):
    REQUIRED[symbol] = "<algorithm>"
for symbol in ("accumulate", "iota", "reduce"):
    REQUIRED[symbol] = "<numeric>"

SKIP_DIRS = ("third_party/",)


def tracked_sources():
    out = subprocess.run(
        ["git", "ls-files", "*.cpp", "*.h"],
        capture_output=True, text=True, check=True,
    ).stdout.split()
    return [p for p in out if not p.startswith(SKIP_DIRS)]


def main():
    root = Path(__file__).resolve().parent.parent
    problems = []

    for relative in tracked_sources():
        text = (root / relative).read_text(encoding="utf-8", errors="replace")
        included = set(re.findall(r"#include\s+(<[^>]+>)", text))
        used = set(re.findall(r"\bstd::(\w+)\b", text))
        for symbol in sorted(used & REQUIRED.keys()):
            header = REQUIRED[symbol]
            if header not in included:
                problems.append(f"{relative}: uses std::{symbol} without {header}")

    if problems:
        print("Missing standard headers:\n")
        for problem in problems:
            print(f"  {problem}")
        print(f"\n{len(problems)} problem(s). These build on libc++ but "
              f"fail on MSVC / libstdc++.")
        return 1

    print(f"Include check passed ({len(tracked_sources())} files).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
