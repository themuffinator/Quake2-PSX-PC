#!/usr/bin/env python3
"""Version authority for Q2PSX-PC.

The version lives in the VERSION file at the repository root, and only there.
CMake reads that file to seed ``project()``, which seeds the generated
``version.h``, which is what a binary prints. This script reads the same file,
so every question about "what version is this" has one answer.

What it is for
--------------
``show`` / ``json``
    Report the current version and how the working tree relates to it.

``resolve``
    Decide which version a release should carry, and refuse the decision if it
    would collide with or go backwards from an existing tag. The release
    workflow runs this before it builds anything, so a bad version costs a few
    seconds rather than a full matrix build and a bogus tag.

``check``
    A repository invariant CI can enforce: the VERSION file parses, and it has
    not fallen behind the tags.

Why the version is not derived from ``git describe``
---------------------------------------------------
It could be, and then a shallow clone or a missing tag would silently produce a
differently-versioned binary. A file in the tree is the same in every checkout,
including a source tarball with no git metadata at all.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION_FILE = ROOT / "VERSION"

PROJECT_NAME = "Q2PSX-PC"
PROJECT_BLURB = "Native PC recreation of Quake II for PlayStation 1"

#: Tags are ``v0.1.0``. The prefix is stripped everywhere the bare number is
#: wanted; it exists so a tag is obviously a release and not a branch.
TAG_PREFIX = "v"

#: Release archives are ``q2psx-pc-0.1.0-linux-x64.tar.gz``.
ARTIFACT_PREFIX = "q2psx-pc"

SEMVER_RE = re.compile(r"^(?P<major>0|[1-9][0-9]*)\.(?P<minor>0|[1-9][0-9]*)\.(?P<patch>0|[1-9][0-9]*)$")
SAFE_ENV_KEY_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


class VersionError(Exception):
    """A version could not be parsed, or would be an illegal thing to release."""


# ---------------------------------------------------------------------------
# Parsing and ordering
# ---------------------------------------------------------------------------


def parse(text: str) -> tuple[int, int, int]:
    """Parse ``MAJOR.MINOR.PATCH``.

    Leading zeroes are rejected rather than normalised: ``0.01.0`` and ``0.1.0``
    would otherwise be two spellings of one version, and the tag would only ever
    match one of them.
    """
    match = SEMVER_RE.match(text.strip())
    if not match:
        raise VersionError(
            f"{text.strip()!r} is not MAJOR.MINOR.PATCH "
            "(three non-negative integers, no leading zeroes, no suffix)"
        )
    return (int(match["major"]), int(match["minor"]), int(match["patch"]))


def format_version(parts: tuple[int, int, int]) -> str:
    return "{}.{}.{}".format(*parts)


def read_version_file(path: Path = VERSION_FILE) -> str:
    try:
        raw = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        raise VersionError(f"{path} does not exist") from None
    return format_version(parse(raw))


def write_version_file(version: str, path: Path = VERSION_FILE) -> None:
    """Write the VERSION file.

    Newline is forced to ``\\n`` so a release cut from a Windows runner does not
    produce a one-line diff against every release cut from Linux.
    """
    parse(version)
    path.write_text(f"{version}\n", encoding="utf-8", newline="\n")


def bump(version: str, part: str) -> str:
    major, minor, patch = parse(version)
    if part == "major":
        return format_version((major + 1, 0, 0))
    if part == "minor":
        return format_version((major, minor + 1, 0))
    if part == "patch":
        return format_version((major, minor, patch + 1))
    raise VersionError(f"unknown version part: {part!r}")


# ---------------------------------------------------------------------------
# Git
# ---------------------------------------------------------------------------


def git(*args: str, root: Path = ROOT) -> str | None:
    """Run git and return stripped stdout, or None if it failed or is absent.

    Everything git tells us here is context, never the source of the version, so
    a checkout without git — or without tags — degrades to "unknown" instead of
    failing the caller.
    """
    try:
        result = subprocess.run(
            ["git", *args],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )
    except (OSError, ValueError):
        return None
    if result.returncode != 0:
        return None
    return result.stdout.strip() or None


def release_tags(root: Path = ROOT) -> list[str]:
    """Every tag that names a release, newest version first."""
    out = git("tag", "--list", f"{TAG_PREFIX}*", root=root)
    if not out:
        return []

    tags: list[tuple[tuple[int, int, int], str]] = []
    for line in out.splitlines():
        tag = line.strip()
        if not tag.startswith(TAG_PREFIX):
            continue
        try:
            tags.append((parse(tag[len(TAG_PREFIX):]), tag))
        except VersionError:
            # A tag this scheme did not create. Not an error — just not ours.
            continue

    tags.sort(key=lambda item: item[0], reverse=True)
    return [tag for _parts, tag in tags]


def highest_release_tag(root: Path = ROOT) -> str | None:
    tags = release_tags(root)
    return tags[0] if tags else None


def tag_exists(tag: str, root: Path = ROOT) -> bool:
    return git("rev-parse", "--verify", "--quiet", f"refs/tags/{tag}", root=root) is not None


def describe(root: Path = ROOT) -> str | None:
    return git("describe", "--tags", "--always", "--dirty=-dirty", root=root)


def head_commit(root: Path = ROOT) -> str | None:
    return git("rev-parse", "HEAD", root=root)


def is_dirty(root: Path = ROOT) -> bool:
    return bool(git("status", "--porcelain", root=root))


# ---------------------------------------------------------------------------
# Metadata
# ---------------------------------------------------------------------------


def tag_for(version: str) -> str:
    return f"{TAG_PREFIX}{version}"


def archive_prefix_for(version: str) -> str:
    return f"{ARTIFACT_PREFIX}-{version}"


def title_for(version: str) -> str:
    return f"{PROJECT_NAME} {version}"


def metadata(version: str | None = None, root: Path = ROOT) -> dict[str, object]:
    """Everything derivable about a version, in one dict."""
    resolved = version if version is not None else read_version_file(root / "VERSION")
    major, minor, patch = parse(resolved)
    commit = head_commit(root)
    previous = highest_release_tag(root)

    return {
        "project": PROJECT_NAME,
        "blurb": PROJECT_BLURB,
        "version": resolved,
        "version_major": major,
        "version_minor": minor,
        "version_patch": patch,
        "tag": tag_for(resolved),
        "tag_exists": tag_exists(tag_for(resolved), root),
        "title": title_for(resolved),
        "archive_prefix": archive_prefix_for(resolved),
        "previous_tag": previous or "",
        "commit": commit or "",
        "commit_short": (commit or "")[:8],
        "describe": describe(root) or "",
        "dirty": is_dirty(root),
    }


def resolve(
    *,
    part: str | None = None,
    explicit: str | None = None,
    root: Path = ROOT,
) -> str:
    """Decide which version to release, and prove it is a legal choice.

    ``explicit`` wins if given. Otherwise ``part`` bumps from whichever is
    higher, the VERSION file or the newest tag — taking the higher of the two
    means a version bumped by hand in the file is honoured, and a tag pushed
    from somewhere else is never overwritten.

    With neither, the VERSION file is released as it stands. That is the case
    for a first release, where the file holds a version nothing has tagged yet.
    """
    current = read_version_file(root / "VERSION")
    newest_tag = highest_release_tag(root)
    newest_tagged = newest_tag[len(TAG_PREFIX):] if newest_tag else None

    if explicit is not None:
        candidate = format_version(parse(explicit))
    elif part is not None:
        base = current
        if newest_tagged is not None and parse(newest_tagged) > parse(current):
            base = newest_tagged
        candidate = bump(base, part)
    else:
        candidate = current

    if tag_exists(tag_for(candidate), root):
        raise VersionError(
            f"tag {tag_for(candidate)} already exists. "
            "Pass a bump (patch/minor/major) or an explicit version."
        )

    if newest_tagged is not None and parse(candidate) <= parse(newest_tagged):
        raise VersionError(
            f"{candidate} is not newer than the newest release tag "
            f"{newest_tag}. Releases must move forwards."
        )

    return candidate


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------


def env_line(key: str, value: object) -> str:
    """Render one ``KEY=value`` line for a GitHub Actions output file.

    Newlines are rejected rather than escaped: a value carrying one could inject
    an extra output, and nothing here has a legitimate reason to contain one.
    """
    if not SAFE_ENV_KEY_RE.match(key):
        raise VersionError(f"{key!r} is not a usable output name")
    rendered = str(value).lower() if isinstance(value, bool) else str(value)
    if any(char in rendered for char in "\r\n") or any(ord(c) < 32 or ord(c) == 127 for c in rendered):
        raise VersionError(f"value for {key} contains control characters")
    return f"{key}={rendered}"


def emit_outputs(meta: dict[str, object], destination: str | None) -> None:
    """Write ``KEY=value`` lines to a file (or stdout when none is given)."""
    lines = [env_line(key, value) for key, value in meta.items()]
    text = "\n".join(lines) + "\n"
    if destination:
        with open(destination, "a", encoding="utf-8") as handle:
            handle.write(text)
    else:
        sys.stdout.write(text)


def print_summary(meta: dict[str, object]) -> None:
    print(f"{meta['project']} {meta['version']}")
    print(f"  tag        : {meta['tag']}{'  (already exists)' if meta['tag_exists'] else ''}")
    print(f"  title      : {meta['title']}")
    print(f"  archives   : {meta['archive_prefix']}-<platform>.(zip|tar.gz)")
    print(f"  previous   : {meta['previous_tag'] or 'none (this would be the first release)'}")
    print(f"  describe   : {meta['describe'] or 'not a git checkout'}")
    print(f"  commit     : {meta['commit_short'] or 'unknown'}")
    if meta["dirty"]:
        print("  tree       : MODIFIED - not releasable as-is")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="command")

    sub.add_parser("show", help="print the current version and its git context")
    sub.add_parser("json", help="print all version metadata as JSON")

    check = sub.add_parser("check", help="verify the VERSION file is well-formed and not behind the tags")
    check.add_argument("--strict", action="store_true", help="also fail on a modified working tree")

    res = sub.add_parser("resolve", help="decide the version for a release")
    group = res.add_mutually_exclusive_group()
    group.add_argument(
        "--bump",
        choices=("major", "minor", "patch"),
        help="bump from the higher of the VERSION file and the newest tag",
    )
    group.add_argument("--set", dest="explicit", help="release this exact version")
    res.add_argument("--write", action="store_true", help="write the resolved version to the VERSION file")
    res.add_argument(
        "--github-output",
        nargs="?",
        const=os.environ.get("GITHUB_OUTPUT", ""),
        default=None,
        help="append KEY=value metadata here (defaults to $GITHUB_OUTPUT)",
    )

    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    command = args.command or "show"

    try:
        if command == "show":
            print_summary(metadata())
            return 0

        if command == "json":
            print(json.dumps(metadata(), indent=2, sort_keys=True))
            return 0

        if command == "check":
            version = read_version_file()
            newest = highest_release_tag()
            if newest is not None and parse(version) < parse(newest[len(TAG_PREFIX):]):
                print(
                    f"VERSION holds {version}, which is behind the newest release tag {newest}.",
                    file=sys.stderr,
                )
                return 1
            if args.strict and is_dirty():
                print("working tree is modified", file=sys.stderr)
                return 1
            print(f"VERSION is {version} (newest tag: {newest or 'none'})")
            return 0

        if command == "resolve":
            version = resolve(part=args.bump, explicit=args.explicit)
            if args.write:
                write_version_file(version)
            meta = metadata(version)
            # metadata() re-reads git, so tag_exists is False by construction
            # here; resolve() already refused anything else.
            if args.github_output is not None:
                emit_outputs(meta, args.github_output or None)
            else:
                print(version)
            return 0

    except VersionError as exc:
        print(f"version.py: {exc}", file=sys.stderr)
        return 2

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
