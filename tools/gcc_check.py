#!/usr/bin/env python3
"""Syntax-checks every first-party translation unit with real GCC.

CI's Linux jobs build with GCC, which warns about things AppleClang does
not: -Wsign-conversion on int->size_t, -Wrange-loop-construct on cheap
structured-binding copies, and more. Finding those one at a time through CI
costs a ~10 minute round trip each. This finds them all at once, locally.

Each file is compiled with -fsyntax-only using its OWN flags from the
compile database, so per-target include paths (Catch2, SDL, ...) are right.
Nothing is linked and no platform backend is built, so SDL's Objective-C
never gets involved.

Requires `brew install gcc`. Skips files whose diagnostics would be about
third-party headers rather than our code.

Usage: python3 tools/gcc_check.py [build-dir]   (default: build/debug)
"""

import concurrent.futures
import glob
import json
import re
import shlex
import subprocess
import sys
from pathlib import Path

WARNINGS = [
    "-std=c++23", "-fsyntax-only",
    "-Wall", "-Wextra", "-Wpedantic", "-Wshadow", "-Wconversion",
    "-Wsign-conversion", "-Wnon-virtual-dtor", "-Wold-style-cast",
    "-Woverloaded-virtual", "-Wnull-dereference", "-Wimplicit-fallthrough",
]

# These only exist to talk to a platform API (SDL/GL/ImGui/miniaudio
# internals). GCC-on-macOS cannot parse their headers, and CI's real Linux
# build covers them anyway.
SKIP = re.compile(r"engine/(platform/window|debug/imgui_layer|audio/audio_engine)\.cpp$")


def find_gxx():
    candidates = sorted(glob.glob("/opt/homebrew/bin/g++-*") +
                        glob.glob("/usr/local/bin/g++-*"))
    return candidates[-1] if candidates else None


def first_party(path: str, root: Path) -> bool:
    try:
        relative = Path(path).resolve().relative_to(root)
    except ValueError:
        return False
    return relative.parts[0] in ("engine", "game", "tests") and not SKIP.search(str(relative))


def flags_for(entry) -> list[str]:
    """Include/define flags only -- drop the compiler, -o, -c and the input."""
    args = shlex.split(entry["command"]) if "command" in entry else list(entry["arguments"])
    out, i = [], 0
    while i < len(args):
        a = args[i]
        if a.startswith(("-I", "-D", "-U")):
            out.append(a)
        elif a in ("-isystem", "-include"):
            out.extend([a, args[i + 1]])
            i += 1
        i += 1
    return out


def main():
    root = Path(__file__).resolve().parent.parent
    build_dir = root / (sys.argv[1] if len(sys.argv) > 1 else "build/debug")
    database = build_dir / "compile_commands.json"

    gxx = find_gxx()
    if gxx is None:
        print("No homebrew g++ found. Install with: brew install gcc", file=sys.stderr)
        return 2
    if not database.exists():
        print(f"Missing {database}. Configure the build first.", file=sys.stderr)
        return 2

    entries = [e for e in json.loads(database.read_text()) if first_party(e["file"], root)]
    if not entries:
        print("No first-party translation units found in the compile database.",
              file=sys.stderr)
        return 2

    print(f"Using {gxx}")
    print(f"Checking {len(entries)} translation units...\n")

    def check(entry):
        result = subprocess.run(
            [gxx, *WARNINGS, *flags_for(entry), entry["file"]],
            capture_output=True, text=True, cwd=entry.get("directory", str(root)),
        )
        return entry["file"], result.returncode, result.stderr

    failures = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        for path, code, stderr in pool.map(check, entries):
            # Do NOT key off the return code: without -Werror a warning
            # still exits 0, and warnings are exactly what CI turns into
            # errors. Scan the diagnostics instead.
            ours = [line for line in stderr.splitlines() if "_deps/" not in line]
            problems = [line for line in ours if "error:" in line or "warning:" in line]
            if not problems and code == 0:
                continue
            failures += 1
            print(f"--- {Path(path).relative_to(root)}")
            if problems:
                print("\n".join(ours[:24]) + "\n")
            else:
                # Failed for a reason our filter hid (usually a third-party
                # header GCC-on-macOS cannot parse). Say so rather than
                # silently counting it as clean.
                print(f"  exited {code} with no first-party diagnostics; "
                      f"add to SKIP if this is a platform header issue\n")

    if failures:
        print(f"GCC check: {failures} of {len(entries)} translation units have problems.")
        return 1
    print(f"GCC check passed ({len(entries)} translation units).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
