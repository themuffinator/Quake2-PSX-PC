#!/usr/bin/env python3
"""Reject tracked paths this repository must not contain.

Two rules, both about a path's *name* rather than its contents, and both here
because both were broken and nothing said so until it was too late to be cheap.

Rule 1 — the path must be one Windows can check out
---------------------------------------------------
A file called ``nul.ppm`` was committed, and `NUL` is a DOS device name that Git
for Windows refuses to create. The whole repository then became un-clonable on
Windows — not the build, the *checkout*:

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

Rule 2 — the path must not be a rendered frame
----------------------------------------------
The README's central claim is that this repository contains **no game assets**,
which is a large part of what makes distributing it lawful. Seven image files
had been committed to the root regardless: a title screen, a HUD test, two
model renders, two level renders, and an accidental screenshot of a terminal
window. Six of the seven were pictures of id Software's artwork, drawn off a
disc by this project's own tools.

A claim the build cannot check is a claim that drifts. This one is checkable.

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

#: Anything that decodes to a picture, a sound or a film is either a rendered
#: frame or an extracted asset, and neither belongs in the index. Nothing here
#: needs one: the documentation is Markdown and its diagrams are ASCII.
MEDIA_SUFFIXES = {
    ".ppm", ".pgm", ".pnm", ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tga",
    ".pcx", ".lbm", ".wav", ".ogg", ".mp3", ".avi", ".mp4", ".webm",
}

#: Magic numbers for the same thing under a name that hides it. Only signatures
#: no text file could begin with, so matching one is conclusive.
MEDIA_MAGIC = [
    (b"\x89PNG\r\n\x1a\n", "PNG image"),
    (b"\xff\xd8\xff", "JPEG image"),
    (b"GIF87a", "GIF image"),
    (b"GIF89a", "GIF image"),
    (b"BM", "BMP image"),
    (b"RIFF", "RIFF audio or video"),
    (b"OggS", "Ogg stream"),
    (b"ID3", "MP3 audio"),
    (b"\x1a\x45\xdf\xa3", "Matroska or WebM video"),
]


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


def media_blobs(paths: list[str]) -> list[str]:
    """Every tracked path that is a picture, a sound or a film."""
    return [path for path in paths if Path(path).suffix.lower() in MEDIA_SUFFIXES]


def media_by_content(paths: list[str], root: Path = ROOT) -> list[tuple[str, str]]:
    """Media smuggled in under a name that does not look like media.

    Checking the suffix is not enough: the file this rule caught was a PNG
    called ``C``, left behind by a shell redirection that had meant to write to
    a path beginning ``C:``. It had no extension at all, so nothing about its
    name gave it away.

    Only unambiguous magic numbers are matched — a byte sequence no source file
    or document would begin with — so a false positive would take real effort.
    """
    found: list[tuple[str, str]] = []
    for path in paths:
        try:
            with open(root / path, "rb") as handle:
                head = handle.read(16)
        except OSError:
            continue

        for magic, kind in MEDIA_MAGIC:
            if head.startswith(magic):
                found.append((path, kind))
                break
        else:
            # Netpbm is "P" + a digit + whitespace, which is narrow enough to be
            # safe: no C file, script or Markdown document opens that way.
            if (
                len(head) >= 3
                and head[0:1] == b"P"
                and head[1:2].isdigit()
                and head[2:3].isspace()
            ):
                found.append((path, "Netpbm image"))

    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args(argv)

    paths = tracked_paths(args.root)
    failed = False

    found = problems(paths)
    if found:
        failed = True
        print(f"{len(found)} tracked path(s) cannot be checked out on Windows:", file=sys.stderr)
        for path, why in found:
            print(f"  {path}: {why}", file=sys.stderr)
        print(
            "\nA repository containing one of these cannot be cloned on Windows at all.\n"
            "Rename the file; the content is not the problem, the name is.",
            file=sys.stderr,
        )

    named = [(path, "media file extension") for path in media_blobs(paths)]
    sniffed = [
        (path, f"{kind}, despite its name")
        for path, kind in media_by_content(paths, args.root)
        if path not in dict(named)
    ]
    media = named + sniffed

    if media:
        failed = True
        print(f"\n{len(media)} tracked media file(s):", file=sys.stderr)
        for path, why in media:
            print(f"  {path}: {why}", file=sys.stderr)
        print(
            "\nThis repository states that it contains no game assets, and a rendered\n"
            "frame is a picture of the disc's own artwork. Write it somewhere\n"
            ".gitignore already covers -- .tmp/ is what the tools use.",
            file=sys.stderr,
        )

    if failed:
        return 1

    print(f"{len(paths)} tracked paths: checkoutable on Windows, and no media among them")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
