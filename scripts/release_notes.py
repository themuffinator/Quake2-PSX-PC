#!/usr/bin/env python3
"""Compose the body of a GitHub release for Q2PSX-PC.

The notes are assembled from things that already exist rather than written at
release time:

* the changelog section for the version, or its curated highlights under
  docs/releases/ when the full changelog is too long for a release body;
* the archives actually produced by the build, so the download table cannot
  advertise a file that is not attached;
* the commit and the previous tag, so the release says what it was built from.

The optional one-line summary comes from the workflow's own input box.

The standing preamble — what this is, that you need your own disc — is here
rather than in the workflow, because it is prose that wants editing without
touching CI.
"""

from __future__ import annotations

import argparse
import datetime as dt
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import changelog  # noqa: E402
import version as version_mod  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]

#: Archive suffix -> what a reader needs to know to pick the right one. Anything
#: not listed still appears in the table, labelled with its own suffix, so a new
#: platform is undocumented rather than invisible.
PLATFORM_LABELS = {
    "windows-x64": "Windows 10/11, 64-bit",
    "linux-x64": "Linux, x86-64",
    "macos-arm64": "macOS, Apple silicon",
    "macos-x64": "macOS, Intel",
}

ARCHIVE_SUFFIXES = (".zip", ".tar.gz")

PREAMBLE = """\
A native PC recreation of *Quake II* for the PlayStation — Hammerhead's 1999 port.
The recreated game logic runs natively, with rendering based on the PlayStation's
GPU and GTE behavior. This remains a work in progress; complete gameplay and
hardware parity are not claimed.
"""

REQUIREMENT = """\
**You need your own Quake II PlayStation disc or image.** This download contains
no game assets. Europe (SLES-01534) and USA (SLUS-00757) are the identified releases.
"""

USAGE = """\
Extract the entire archive, then open **USER-GUIDE.html** in a browser for the
offline manual. Keep the runtime libraries beside the executable.

On Windows, double-click `q2psx.exe`. On Linux or macOS, run `./q2psx` from a
terminal in the extracted folder. Choose **BROWSE IMAGE**, select your CUE, then
**START GAME**. Keep all referenced BIN tracks with the CUE.

A raw CUE/BIN image is recommended: 2048-byte ISO images lack the XA data needed
for music and films. Physical-drive support is available on Windows and Linux;
raw-sector access depends on the drive. CHD, MDS/MDF and PBP are unsupported.

The bundled `q2psx-inspect` and `stx2avi` tools are optional; they are not needed
to play.
"""


def repo_slug(explicit: str | None = None) -> str | None:
    """``owner/name``, for building compare links."""
    if explicit:
        return explicit
    env = os.environ.get("GITHUB_REPOSITORY")
    if env:
        return env
    remote = version_mod.git("remote", "get-url", "origin")
    if not remote:
        return None
    match = re.search(r"github\.com[:/](?P<slug>[^/]+/[^/]+?)(?:\.git)?/?$", remote)
    return match["slug"] if match else None


def archive_rows(archive_dir: Path | None, prefix: str) -> list[tuple[str, str]]:
    """``(filename, audience)`` for every archive that was actually built."""
    if archive_dir is None or not archive_dir.is_dir():
        return []

    rows: list[tuple[str, str]] = []
    for path in sorted(archive_dir.rglob("*")):
        if not path.is_file():
            continue
        name = path.name
        suffix = next((s for s in ARCHIVE_SUFFIXES if name.endswith(s)), None)
        if suffix is None or not name.startswith(prefix):
            continue
        platform = name[len(prefix) : -len(suffix)].strip("-")
        rows.append((name, PLATFORM_LABELS.get(platform, platform)))
    return rows


def download_table(rows: list[tuple[str, str]]) -> str:
    if not rows:
        return ""
    lines = ["| Download | For |", "|---|---|"]
    lines.extend(f"| `{name}` | {label} |" for name, label in rows)
    return "\n".join(lines) + "\n"


def changelog_body(path: Path, version: str) -> str:
    """The section for this version, falling back to the pending queue.

    The publish job stamps the changelog before it renders the notes, so the
    dated section is normally there. A dry run has not stamped anything, and
    reading ``[Unreleased]`` is what makes its preview the real thing.
    """
    for label in (version, changelog.UNRELEASED):
        try:
            body = changelog.section_markdown(path, label)
        except changelog.ChangelogError:
            continue
        if body.strip():
            return body
    return "- No documented changes.\n"


def compose(
    *,
    version: str,
    date: str,
    commit: str | None,
    previous_tag: str | None,
    summary: str | None,
    archive_dir: Path | None,
    changelog_path: Path,
    slug: str | None,
    highlights_path: Path | None = None,
) -> str:
    tag = version_mod.tag_for(version)
    prefix = f"{version_mod.archive_prefix_for(version)}-"
    parts: list[str] = []

    if summary and summary.strip():
        parts.append(summary.strip() + "\n")

    parts.append(PREAMBLE)
    changes = changelog_body(changelog_path, version)
    if highlights_path is not None:
        changes = highlights_path.read_text(encoding="utf-8").strip()
        if not changes:
            raise ValueError(f"{highlights_path} has no release highlights")
        if slug:
            changes += (
                f"\n\n[Full changelog](https://github.com/{slug}/blob/{tag}/CHANGELOG.md).\n"
            )
        else:
            changes += "\n\nSee CHANGELOG.md in the download for the full changelog.\n"
    parts.append("## What changed\n\n" + changes)

    parts.append("## Getting it\n\n" + REQUIREMENT)
    table = download_table(archive_rows(archive_dir, prefix))
    if table:
        parts.append(table)
    parts.append(USAGE)

    parts.append(
        "## Verifying a download\n\n"
        "`SHA256SUMS.txt` is attached and covers every archive here:\n\n"
        "```\n"
        "sha256sum -c SHA256SUMS.txt        # Linux\n"
        "shasum -a 256 -c SHA256SUMS.txt    # macOS\n"
        'Get-FileHash q2psx-pc-*.zip        # Windows PowerShell\n'
        "```\n"
    )

    provenance = ["## This build\n"]
    provenance.append(f"- Version `{version}`, tagged `{tag}`")
    if commit:
        provenance.append(f"- Built from commit `{commit[:8]}`")
    provenance.append(f"- Built on {date}")
    if slug and previous_tag:
        provenance.append(
            f"- Full commit log: "
            f"https://github.com/{slug}/compare/{previous_tag}...{tag}"
        )
    elif slug:
        provenance.append(
            f"- Full commit log: https://github.com/{slug}/commits/{tag}"
        )
    if slug:
        provenance.append(
            f"- [Technical references and known limits](https://github.com/{slug}/tree/{tag}/docs) "
            "are available in the source repository."
        )
    parts.append("\n".join(provenance) + "\n")

    return "\n".join(part.rstrip() + "\n" for part in parts if part.strip())


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--version", required=True)
    parser.add_argument("--date", default=dt.date.today().isoformat())
    parser.add_argument("--commit", default=os.environ.get("GITHUB_SHA"))
    parser.add_argument("--previous-tag", default=None)
    parser.add_argument("--summary", default="", help="optional one-line lead")
    parser.add_argument(
        "--archive-dir",
        type=Path,
        default=None,
        help="directory of built archives, used to build the download table",
    )
    parser.add_argument("--changelog", type=Path, default=changelog.CHANGELOG)
    parser.add_argument("--repo", default=None, help="owner/name for compare links")
    parser.add_argument("--output", type=Path, default=None, help="write here instead of stdout")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    try:
        version = version_mod.format_version(version_mod.parse(args.version))
    except version_mod.VersionError as exc:
        print(f"release_notes.py: {exc}", file=sys.stderr)
        return 2

    previous = args.previous_tag
    if previous is None:
        previous = version_mod.highest_release_tag()
        # The tag for this release may already exist by the time the notes are
        # built; comparing a tag against itself is not a useful link.
        if previous == version_mod.tag_for(version):
            tags = version_mod.release_tags()
            previous = tags[1] if len(tags) > 1 else None

    highlights = ROOT / "docs" / "releases" / f"{version}.md"
    text = compose(
        version=version,
        date=args.date,
        commit=args.commit,
        previous_tag=previous or None,
        summary=args.summary,
        archive_dir=args.archive_dir,
        changelog_path=args.changelog,
        slug=repo_slug(args.repo),
        highlights_path=highlights if highlights.is_file() else None,
    )

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8", newline="\n")
        print(f"wrote {args.output} ({len(text)} bytes)")
    else:
        if hasattr(sys.stdout, "reconfigure"):
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
