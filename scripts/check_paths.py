#!/usr/bin/env python3
"""Reject tracked paths that Windows cannot check out.

This exists because a file called ``nul.ppm`` was committed, and `NUL` is a DOS
device name that Git for Windows refuses to create. The whole repository then
became un-clonable on Windows — not the build, the *checkout*:

    error: invalid path 'nul.ppm'
    fatal: unable to checkout working tree

Nothing reported that until a Windows job tried it, and the message names the
file without saying why it is a problem. So the check runs in CI, where the
answer is one line long and arrives on the pull request that introduced it.

What is rejected, per path component:

* a DOS device name — ``NUL``, ``CON``, ``AUX``, ``PRN``, ``COM1``-``COM9``,
  ``LPT1``-``LPT9`` — with or without an extension, in any case;
* a character Windows forbids: ``< > : " | ? *``, backslash, or a control byte;
* a trailing dot or space, which Windows silently strips, so ``docs.`` and
  ``docs`` would collide;
* two paths differing only in case, which collide on a case-insensitive
  filesystem and produce a checkout that is dirty the moment it is made.

    python scripts/check_paths.py
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

DOS_DEVICES = {
    "CON",
    "PRN",
    "AUX",
    "NUL",
    *(f"COM{index}" for index in range(1, 10)),
    *(f"LPT{index}" for index in range(1, 10)),
}

ILLEGAL_CHARACTERS = set('<>:"|?*\\') | {chr(code) for code in range(32)} | {chr(127)}


def tracked_paths(root: Path = ROOT) -> list[str]:
    """Every path in the index, as git records it (forward slashes, UTF-8)."""
    result = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=root,
        capture_output=True,
        check=True,
    )
    return [p for p in result.stdout.decode("utf-8").split("\0") if p]


def problems(paths: list[str]) -> list[tuple[str, str]]:
    """``(path, why)`` for every path Windows would reject or confuse."""
    found: list[tuple[str, str]] = []

    for path in paths:
        for component in path.split("/"):
            # Windows matches a device name before the first dot, so `nul.ppm`
            # is the NUL device and not a file that happens to be called nul.
            stem = component.split(".", 1)[0].upper()
            if stem in DOS_DEVICES:
                found.append((path, f"'{component}' is the DOS device name {stem}"))

            illegal = sorted(set(component) & ILLEGAL_CHARACTERS)
            if illegal:
                rendered = ", ".join(repr(character) for character in illegal)
                found.append((path, f"'{component}' contains {rendered}"))

            if component != component.rstrip(". "):
                found.append((path, f"'{component}' ends in a dot or a space"))

    by_lowercase: dict[str, list[str]] = defaultdict(list)
    for path in paths:
        by_lowercase[path.lower()].append(path)
    for collisions in by_lowercase.values():
        if len(collisions) > 1:
            found.append((collisions[0], f"collides case-insensitively with {collisions[1]}"))

    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args(argv)

    paths = tracked_paths(args.root)
    found = problems(paths)

    if found:
        print(f"{len(found)} tracked path(s) cannot be checked out on Windows:", file=sys.stderr)
        for path, why in found:
            print(f"  {path}: {why}", file=sys.stderr)
        print(
            "\nA repository containing one of these cannot be cloned on Windows at all.\n"
            "Rename the file; the content is not the problem, the name is.",
            file=sys.stderr,
        )
        return 1

    print(f"{len(paths)} tracked paths, all checkoutable on Windows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
