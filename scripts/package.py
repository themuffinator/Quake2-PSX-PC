#!/usr/bin/env python3
"""Assemble a Q2PSX-PC release archive.

This project ships **no game assets** — a player supplies their own disc — so
the archive is built from an explicit list of files and never from a glob over a
directory. A glob is one stray output file away from redistributing something
that is not ours to redistribute; a list cannot be.

The same reason keeps this in Python rather than in the workflow's shell: the
list is then one declaration read by all three runners, and it can be run by
hand before trusting it to CI.

    scripts/package.py stage    --build-dir build --platform linux-x64
    scripts/package.py checksums --archive-dir dist

``stage`` builds ``dist/q2psx-pc-<version>-<platform>/``, then archives it —
``.zip`` for Windows, ``.tar.gz`` elsewhere, which is what each platform's users
can open without installing anything.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import sys
import tarfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import version as version_mod  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]

#: Executables, by CMake target name. The extension is added per platform.
#: All three are required: a release missing the client is not a release, and a
#: release missing the tools cannot be checked against a disc by whoever
#: downloads it.
EXECUTABLES = ["q2psx", "q2psx-inspect", "stx2avi"]

#: Documentation, as ``source -> path inside the archive``. README and the
#: changelog sit at the top where someone unpacking will see them; the reference
#: documents keep their own directory.
DOCUMENTS = {
    "README.md": "README.md",
    "CHANGELOG.md": "CHANGELOG.md",
    # GPL-2.0 section 1 requires the licence to travel with the binary. This is
    # not documentation and it is not optional.
    "LICENSE": "LICENSE",
    "docs/ARCHITECTURE.md": "docs/ARCHITECTURE.md",
    "docs/FIDELITY.md": "docs/FIDELITY.md",
    "docs/FORMATS.md": "docs/FORMATS.md",
    "docs/openquestions.md": "docs/openquestions.md",
    "docs/RELEASING.md": "docs/RELEASING.md",
}

#: Runtime libraries the executables need beside them. SDL3 is the only one, and
#: only when it was fetched and built rather than found on the system.
RUNTIME_LIBRARY_PATTERNS = ["SDL3.dll", "libSDL3.so*", "libSDL3*.dylib"]

CHECKSUM_FILE = "SHA256SUMS.txt"
ARCHIVE_SUFFIXES = (".zip", ".tar.gz")


class PackageError(Exception):
    """A release archive could not be assembled from what was built."""


def executable_name(target: str, platform: str) -> str:
    return f"{target}.exe" if platform.startswith("windows") else target


def find_runtime_libraries(build_dir: Path) -> list[Path]:
    """Locate shared libraries that must ship beside the executables.

    ``bin/`` is checked first because that is where the build puts them; the
    wider search is a fallback for a build whose output directories were
    overridden, and picking the first match per name keeps a symlink chain from
    turning into three copies of one library.
    """
    found: dict[str, Path] = {}
    for pattern in RUNTIME_LIBRARY_PATTERNS:
        candidates = sorted(build_dir.glob(f"bin/{pattern}")) or sorted(build_dir.rglob(pattern))
        for candidate in candidates:
            if candidate.is_file() and candidate.name not in found:
                found[candidate.name] = candidate
    return list(found.values())


def copy_runtime_library(source: Path, destination: Path) -> str:
    """Copy a runtime library, keeping a symlink a symlink.

    A fetched SDL3 installs as the usual three-name chain --
    ``libSDL3.so -> libSDL3.so.0 -> libSDL3.so.0.2.8`` -- and the client's
    DT_NEEDED names the middle one. Copying each of them by value would put
    three identical four-megabyte files in the archive; keeping the links is
    both smaller and the layout a Linux install has anyway.

    Falls back to copying where symlinks cannot be created, which is Windows
    without the privilege -- and Windows has no chain to preserve, only
    SDL3.dll.
    """
    if source.is_symlink():
        target = os.readlink(source)
        try:
            destination.symlink_to(target)
            return f"{source.name} -> {target}"
        except (OSError, NotImplementedError):
            pass
    shutil.copy2(source, destination)
    return source.name


def stage(
    *,
    build_dir: Path,
    out_dir: Path,
    version: str,
    platform: str,
    expect: list[str],
) -> Path:
    """Copy the release files into ``out_dir/<name>/`` and return that path."""
    name = f"{version_mod.archive_prefix_for(version)}-{platform}"
    staging = out_dir / name
    if staging.exists():
        shutil.rmtree(staging)
    (staging / "docs").mkdir(parents=True)

    bindir = build_dir / "bin"
    for target in expect:
        source = bindir / executable_name(target, platform)
        if not source.is_file():
            raise PackageError(f"{source} was not built")
        shutil.copy2(source, staging / source.name)
        print(f"  {source.name}")

    if "q2psx" in expect:
        for library in find_runtime_libraries(build_dir):
            print(f"  {copy_runtime_library(library, staging / library.name)}")

    for source_rel, dest_rel in DOCUMENTS.items():
        source = ROOT / source_rel
        if not source.is_file():
            raise PackageError(f"{source_rel} is missing from the repository")
        destination = staging / dest_rel
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        print(f"  {dest_rel}")

    return staging


def make_archive(staging: Path, platform: str) -> Path:
    """Archive the staging directory in the format that platform expects.

    zip for Windows and tar.gz elsewhere: tar preserves the executable bit,
    which a Unix user needs and a zip would drop, and Windows opens a zip with
    no extra software.
    """
    parent = staging.parent
    if platform.startswith("windows"):
        archive = parent / f"{staging.name}.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
            for path in sorted(staging.rglob("*")):
                if path.is_file():
                    bundle.write(path, path.relative_to(parent).as_posix())
    else:
        archive = parent / f"{staging.name}.tar.gz"
        with tarfile.open(archive, "w:gz") as bundle:
            bundle.add(staging, arcname=staging.name)

    return archive


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def write_checksums(archive_dir: Path) -> Path:
    """Write one ``SHA256SUMS.txt`` covering every archive in a directory.

    Names are written bare, so a downloader who put the archive and this file in
    the same directory can run ``sha256sum -c`` without editing either.
    """
    archives = sorted(
        path
        for path in archive_dir.rglob("*")
        if path.is_file() and path.name.endswith(ARCHIVE_SUFFIXES)
    )
    if not archives:
        raise PackageError(f"no archives found under {archive_dir}")

    lines = [f"{sha256(path)}  {path.name}" for path in archives]
    destination = archive_dir / CHECKSUM_FILE
    destination.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")

    for line in lines:
        print(line)
    return destination


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    stage_cmd = sub.add_parser("stage", help="assemble and archive one platform's release")
    stage_cmd.add_argument("--build-dir", type=Path, default=ROOT / "build")
    stage_cmd.add_argument("--out-dir", type=Path, default=ROOT / "dist")
    stage_cmd.add_argument("--version", default=None, help="defaults to the VERSION file")
    stage_cmd.add_argument("--platform", required=True, help="e.g. windows-x64, linux-x64, macos-arm64")
    stage_cmd.add_argument(
        "--expect",
        default=",".join(EXECUTABLES),
        help="comma-separated executables that must be present",
    )
    stage_cmd.add_argument("--no-archive", action="store_true", help="stage the directory but do not archive it")

    sums = sub.add_parser("checksums", help="write SHA256SUMS.txt over a directory of archives")
    sums.add_argument("--archive-dir", type=Path, required=True)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    try:
        if args.command == "checksums":
            destination = write_checksums(args.archive_dir)
            print(f"wrote {destination}")
            return 0

        version = args.version or version_mod.read_version_file()
        version = version_mod.format_version(version_mod.parse(version))
        expect = [name.strip() for name in args.expect.split(",") if name.strip()]
        unknown = [name for name in expect if name not in EXECUTABLES]
        if unknown:
            raise PackageError(f"not a known executable: {', '.join(unknown)}")

        args.out_dir.mkdir(parents=True, exist_ok=True)
        print(f"staging {version} for {args.platform}")
        staging = stage(
            build_dir=args.build_dir,
            out_dir=args.out_dir,
            version=version,
            platform=args.platform,
            expect=expect,
        )

        if args.no_archive:
            print(f"staged {staging}")
            return 0

        archive = make_archive(staging, args.platform)
        size = archive.stat().st_size
        print(f"wrote {archive} ({size / 1024 / 1024:.1f} MiB)")
        return 0

    except (PackageError, version_mod.VersionError) as exc:
        print(f"package.py: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
