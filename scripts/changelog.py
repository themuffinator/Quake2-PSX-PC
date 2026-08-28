#!/usr/bin/env python3
"""Changelog tool for Q2PSX-PC.

``CHANGELOG.md`` is the pending release-note queue. Work lands with a bullet
under ``## [Unreleased]``; cutting a release moves that section, verbatim, into
a dated section of its own and empties the queue for the next cycle. The same
text becomes the body of the GitHub release, so the notes are written by whoever
did the work rather than reconstructed from commit subjects afterwards.

Commands
--------
``show``
    Print one section's body. Empty categories are dropped unless asked for, so
    the release notes never carry a run of "nothing here" headings.

``check``
    The file parses, ``[Unreleased]`` exists, and its headings are the ones this
    project uses. ``--require-entries`` additionally demands at least one real
    bullet, which is what stops a release going out with empty notes.

``release``
    Move ``[Unreleased]`` into ``[VERSION] - DATE`` and reset the queue.

``categories``
    Print the category list, so the section template has one definition.

Why bullets are moved rather than regenerated
---------------------------------------------
This project's commit subjects are written as findings ("A corpse is not a
creature, and this port had no step that said so") rather than as changelog
lines. They are the better record of the work and the worse description of the
result, and no amount of post-processing turns one into the other.
"""

from __future__ import annotations

import argparse
import datetime as dt
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHANGELOG = ROOT / "CHANGELOG.md"

UNRELEASED = "Unreleased"

#: The headings a section may carry, in the order they are rendered. Ordered by
#: what a reader of the release notes cares about first: what the port now does,
#: then what was learned, then the machinery around it.
CATEGORIES = [
    "Reconstruction",
    "Client",
    "Rendering and audio",
    "Tools",
    "Fixes",
    "Build and packaging",
    "Documentation",
]

PLACEHOLDER = "_Nothing yet._"

#: Written under a heading with no entries. Matched case-insensitively, and a
#: few older spellings are accepted so a hand-edited file still parses.
PLACEHOLDERS = {
    "_nothing yet._",
    "nothing yet.",
    "nothing yet",
    "_none._",
    "none.",
    "none",
    "no documented changes.",
}

SECTION_RE = re.compile(r"^##\s+\[(?P<label>[^\]]+)\](?:\s*[-–]\s*(?P<date>\d{4}-\d{2}-\d{2}))?\s*$")
CATEGORY_RE = re.compile(r"^###\s+(?P<label>.+?)\s*$")
BULLET_RE = re.compile(r"^\s*[-*]\s+(?P<text>.+?)\s*$")
LABEL_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]*$")


class ChangelogError(Exception):
    """The changelog is not in a shape this tool can work with."""


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------


def read_sections(path: Path = CHANGELOG) -> list[tuple[str, str | None, list[str]]]:
    """Split the file into ``(label, date, body_lines)`` in document order."""
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError:
        raise ChangelogError(f"{path} does not exist") from None

    sections: list[tuple[str, str | None, list[str]]] = []
    label: str | None = None
    date: str | None = None
    body: list[str] = []

    for line in lines:
        match = SECTION_RE.match(line)
        if match:
            if label is not None:
                sections.append((label, date, body))
            label = match["label"]
            date = match["date"]
            body = []
            continue
        if label is not None:
            body.append(line)

    if label is not None:
        sections.append((label, date, body))
    return sections


def find_section(path: Path, label: str) -> tuple[str, str | None, list[str]]:
    for found_label, date, body in read_sections(path):
        if found_label.lower() == label.lower():
            return (found_label, date, body)
    raise ChangelogError(f"no section [{label}] in {path.name}")


def is_placeholder(text: str) -> bool:
    return text.strip().lower() in PLACEHOLDERS


def entries(body: list[str]) -> list[tuple[str, str]]:
    """Every real bullet in a section body, as ``(category, text)``.

    Placeholders are dropped, and a bullet before any ``###`` heading is filed
    under the first category rather than discarded — a hand-edited file should
    not silently lose an entry because the heading was forgotten.
    """
    found: list[tuple[str, str]] = []
    category = CATEGORIES[0]

    for line in body:
        heading = CATEGORY_RE.match(line)
        if heading:
            category = canonical_category(heading["label"])
            continue
        bullet = BULLET_RE.match(line)
        if not bullet:
            continue
        text = re.sub(r"\s+", " ", bullet["text"].strip())
        if not text or is_placeholder(text):
            continue
        found.append((category, text))

    return found


def canonical_category(label: str) -> str:
    cleaned = label.strip()
    for category in CATEGORIES:
        if cleaned.lower() == category.lower():
            return category
    raise ChangelogError(
        f"unknown category heading '### {cleaned}'. "
        f"Use one of: {', '.join(CATEGORIES)}"
    )


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------


def render(found: list[tuple[str, str]], *, include_empty: bool) -> list[str]:
    """Render entries back to markdown, grouped and in canonical order."""
    grouped: dict[str, list[str]] = {category: [] for category in CATEGORIES}
    for category, text in found:
        grouped[category].append(text)

    lines: list[str] = []
    for category in CATEGORIES:
        bullets = grouped[category]
        if not bullets and not include_empty:
            continue
        if lines:
            lines.append("")
        lines.append(f"### {category}")
        if bullets:
            lines.extend(f"- {bullet}" for bullet in bullets)
        else:
            lines.append(f"- {PLACEHOLDER}")

    return lines


def empty_section_lines() -> list[str]:
    return ["", *render([], include_empty=True)]


def section_markdown(path: Path, label: str, *, include_empty: bool = False) -> str:
    """One section's body as markdown, ready to paste into release notes."""
    _label, _date, body = find_section(path, label)
    found = entries(body)
    if not found and not include_empty:
        return ""
    return "\n".join(render(found, include_empty=include_empty)).strip() + "\n"


# ---------------------------------------------------------------------------
# Mutation
# ---------------------------------------------------------------------------


def validate_label(value: str) -> str:
    label = value.strip()
    if not LABEL_RE.match(label):
        raise ChangelogError(f"{value!r} is not a usable section label")
    return label


def validate_date(value: str) -> str:
    text = value.strip()
    try:
        parsed = dt.date.fromisoformat(text)
    except ValueError:
        raise ChangelogError(f"{value!r} is not a YYYY-MM-DD date") from None
    if parsed.isoformat() != text:
        raise ChangelogError(f"{value!r} is not a YYYY-MM-DD date")
    return text


def section_header_index(lines: list[str], label: str) -> int:
    for index, line in enumerate(lines):
        match = SECTION_RE.match(line)
        if match and match["label"].lower() == label.lower():
            return index
    return -1


def next_section_index(lines: list[str], start: int) -> int:
    for index in range(start + 1, len(lines)):
        if SECTION_RE.match(lines[index]):
            return index
    return len(lines)


def prepare_release(path: Path, version: str, date: str) -> str:
    """Move ``[Unreleased]`` into a dated section and empty the queue.

    The moved bullets keep their wording. Only the grouping is normalised, so a
    section written by hand and one written by this tool read the same.
    """
    version = validate_label(version)
    date = validate_date(date)

    for label, _date, _body in read_sections(path):
        if label.lower() == version.lower():
            raise ChangelogError(f"[{version}] already exists in {path.name}")

    lines = path.read_text(encoding="utf-8").splitlines()
    start = section_header_index(lines, UNRELEASED)
    if start < 0:
        raise ChangelogError(f"no section [{UNRELEASED}] in {path.name}")
    end = next_section_index(lines, start)

    found = entries(lines[start + 1 : end])
    if not found:
        raise ChangelogError(
            f"[{UNRELEASED}] has no entries. A release needs notes; "
            "add bullets to CHANGELOG.md first."
        )

    header = f"## [{version}] - {date}"
    block = ["", header, "", *render(found, include_empty=False)]
    updated = lines[: start + 1] + empty_section_lines() + block + lines[end:]

    path.write_text("\n".join(updated).rstrip() + "\n", encoding="utf-8", newline="\n")
    return header


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--changelog", type=Path, default=CHANGELOG)
    sub = parser.add_subparsers(dest="command")

    show = sub.add_parser("show", help="print one section's body")
    show.add_argument("--version", default=UNRELEASED, help="section label (default: Unreleased)")
    show.add_argument("--all-categories", action="store_true", help="keep headings that have no entries")

    check = sub.add_parser("check", help="validate the file's structure")
    check.add_argument(
        "--require-entries",
        action="store_true",
        help="also fail when [Unreleased] has no bullets",
    )

    release = sub.add_parser("release", help="move [Unreleased] into a dated section")
    release.add_argument("--version", required=True)
    release.add_argument("--date", default=dt.date.today().isoformat())

    sub.add_parser("categories", help="print the category headings")

    return parser


def run_check(path: Path, require_entries: bool) -> int:
    sections = read_sections(path)
    if not sections:
        raise ChangelogError(f"{path.name} has no '## [...]' sections")

    labels = [label for label, _date, _body in sections]
    if not any(label.lower() == UNRELEASED.lower() for label in labels):
        raise ChangelogError(f"{path.name} has no [{UNRELEASED}] section")

    # canonical_category() raises on an unknown heading; entries() calls it for
    # every section, so this validates the whole file rather than just the queue.
    total = 0
    for label, _date, body in sections:
        found = entries(body)
        if label.lower() != UNRELEASED.lower():
            total += len(found)

    pending = entries(find_section(path, UNRELEASED)[2])
    if require_entries and not pending:
        raise ChangelogError(
            f"[{UNRELEASED}] has no entries. A release needs notes; "
            "add bullets to CHANGELOG.md first."
        )

    released = len(labels) - 1
    print(
        f"{path.name}: {len(pending)} pending "
        f"{'entry' if len(pending) == 1 else 'entries'}, "
        f"{released} released {'section' if released == 1 else 'sections'}, "
        f"{total} historical entries"
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    command = args.command or "check"
    path: Path = args.changelog

    try:
        if command == "categories":
            for category in CATEGORIES:
                print(category)
            return 0

        if command == "show":
            text = section_markdown(path, args.version, include_empty=args.all_categories)
            sys.stdout.write(text or "- No documented changes.\n")
            return 0

        if command == "check":
            return run_check(path, args.require_entries)

        if command == "release":
            print(prepare_release(path, args.version, args.date))
            return 0

    except ChangelogError as exc:
        print(f"changelog.py: {exc}", file=sys.stderr)
        return 2

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
