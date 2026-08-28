#!/usr/bin/env python3
"""Tests for the version, changelog, release-note and packaging tools.

The release path is the one path that cannot be debugged in production: a script
that breaks is discovered while cutting a release, with a half-tagged repository
to clean up. So it is tested like anything else, and CI runs this on every pull
request.

    python -m unittest discover -s scripts -p 'test_*.py' -v
    python scripts/test_release_tooling.py

Only the standard library is used, so this needs no environment beyond the
Python that is already on every runner.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import changelog  # noqa: E402
import package  # noqa: E402
import release_notes  # noqa: E402
import version as version_mod  # noqa: E402


def git_repo(root: Path, version: str = "0.1.0") -> None:
    """A throwaway repository with one commit, so tags have something to point at."""
    (root / "VERSION").write_text(f"{version}\n", encoding="utf-8", newline="\n")
    run = lambda *args: subprocess.run(  # noqa: E731
        ["git", *args], cwd=root, check=True, capture_output=True, text=True
    )
    run("init", "-q", "-b", "main")
    run("config", "user.email", "test@example.invalid")
    run("config", "user.name", "Test")
    run("config", "commit.gpgsign", "false")
    run("add", "VERSION")
    run("commit", "-qm", "seed")


def tag(root: Path, name: str) -> None:
    subprocess.run(["git", "tag", name], cwd=root, check=True, capture_output=True)


CHANGELOG_TEMPLATE = """\
# Changelog

## [Unreleased]

### Reconstruction
- The scene graph is read.

### Client
- _Nothing yet._

### Fixes
- Doors no longer stand open.
"""


class VersionParsing(unittest.TestCase):
    def test_accepts_plain_semver(self):
        self.assertEqual(version_mod.parse("1.2.3"), (1, 2, 3))
        self.assertEqual(version_mod.parse("  0.1.0\n"), (0, 1, 0))

    def test_rejects_malformed(self):
        for bad in ("1.2", "1.2.3.4", "v1.2.3", "1.2.3-rc1", "", "a.b.c"):
            with self.subTest(bad=bad), self.assertRaises(version_mod.VersionError):
                version_mod.parse(bad)

    def test_rejects_leading_zeroes(self):
        # 0.01.0 and 0.1.0 would otherwise be two spellings of one version, and
        # only one of them would ever match the tag.
        with self.assertRaises(version_mod.VersionError):
            version_mod.parse("0.01.0")

    def test_bump(self):
        self.assertEqual(version_mod.bump("1.2.3", "patch"), "1.2.4")
        self.assertEqual(version_mod.bump("1.2.3", "minor"), "1.3.0")
        self.assertEqual(version_mod.bump("1.2.3", "major"), "2.0.0")

    def test_derived_names(self):
        self.assertEqual(version_mod.tag_for("0.1.0"), "v0.1.0")
        self.assertEqual(version_mod.archive_prefix_for("0.1.0"), "q2psx-pc-0.1.0")
        self.assertEqual(version_mod.title_for("0.1.0"), "Q2PSX-PC 0.1.0")

    def test_env_line_rejects_injection(self):
        # A value carrying a newline could add an output the caller never set.
        with self.assertRaises(version_mod.VersionError):
            version_mod.env_line("VERSION", "0.1.0\nADMIN=1")
        self.assertEqual(version_mod.env_line("DIRTY", False), "DIRTY=false")


class VersionResolution(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def test_first_release_uses_the_file_as_it_stands(self):
        git_repo(self.root, "0.1.0")
        self.assertEqual(version_mod.resolve(root=self.root), "0.1.0")

    def test_refuses_to_reuse_a_tag(self):
        git_repo(self.root, "0.1.0")
        tag(self.root, "v0.1.0")
        with self.assertRaises(version_mod.VersionError) as caught:
            version_mod.resolve(root=self.root)
        self.assertIn("already exists", str(caught.exception))

    def test_refuses_to_go_backwards(self):
        git_repo(self.root, "0.1.0")
        tag(self.root, "v0.2.0")
        with self.assertRaises(version_mod.VersionError) as caught:
            version_mod.resolve(explicit="0.1.5", root=self.root)
        self.assertIn("not newer", str(caught.exception))

    def test_bump_starts_from_the_newest_tag(self):
        # The file says 0.1.0 but 0.2.0 is tagged, so a patch bump is 0.2.1 --
        # bumping the file would land on a version that already shipped.
        git_repo(self.root, "0.1.0")
        tag(self.root, "v0.2.0")
        self.assertEqual(version_mod.resolve(part="patch", root=self.root), "0.2.1")

    def test_bump_honours_a_hand_edited_file(self):
        git_repo(self.root, "0.5.0")
        tag(self.root, "v0.2.0")
        self.assertEqual(version_mod.resolve(part="minor", root=self.root), "0.6.0")

    def test_ignores_tags_that_are_not_releases(self):
        git_repo(self.root, "0.1.0")
        tag(self.root, "nightly-2026-01-01")
        self.assertIsNone(version_mod.highest_release_tag(self.root))
        self.assertEqual(version_mod.resolve(root=self.root), "0.1.0")

    def test_tags_sort_numerically(self):
        git_repo(self.root, "0.11.0")
        for name in ("v0.2.0", "v0.10.0", "v0.9.0"):
            tag(self.root, name)
        # Lexically, v0.9.0 is the highest. It is not the newest release.
        self.assertEqual(version_mod.highest_release_tag(self.root), "v0.10.0")

    def test_write_round_trips(self):
        git_repo(self.root, "0.1.0")
        version_mod.write_version_file("1.4.2", self.root / "VERSION")
        self.assertEqual(version_mod.read_version_file(self.root / "VERSION"), "1.4.2")
        self.assertEqual((self.root / "VERSION").read_bytes(), b"1.4.2\n")


class Changelog(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / "CHANGELOG.md"
        self.path.write_text(CHANGELOG_TEMPLATE, encoding="utf-8", newline="\n")
        self.addCleanup(self.tmp.cleanup)

    def test_placeholders_are_not_entries(self):
        found = changelog.entries(changelog.find_section(self.path, "Unreleased")[2])
        self.assertEqual(
            found,
            [
                ("Reconstruction", "The scene graph is read."),
                ("Fixes", "Doors no longer stand open."),
            ],
        )

    def test_show_drops_empty_headings(self):
        body = changelog.section_markdown(self.path, "Unreleased")
        self.assertIn("### Reconstruction", body)
        self.assertIn("### Fixes", body)
        self.assertNotIn("### Client", body)

    def test_show_can_keep_empty_headings(self):
        body = changelog.section_markdown(self.path, "Unreleased", include_empty=True)
        for category in changelog.CATEGORIES:
            self.assertIn(f"### {category}", body)

    def test_unknown_heading_is_rejected(self):
        self.path.write_text(
            "# Changelog\n\n## [Unreleased]\n\n### Miscellany\n- Something.\n",
            encoding="utf-8",
            newline="\n",
        )
        with self.assertRaises(changelog.ChangelogError) as caught:
            changelog.section_markdown(self.path, "Unreleased")
        self.assertIn("Miscellany", str(caught.exception))

    def test_release_moves_entries_and_empties_the_queue(self):
        header = changelog.prepare_release(self.path, "0.1.0", "2026-08-28")
        self.assertEqual(header, "## [0.1.0] - 2026-08-28")

        released = changelog.section_markdown(self.path, "0.1.0")
        self.assertIn("The scene graph is read.", released)
        self.assertIn("Doors no longer stand open.", released)

        self.assertEqual(changelog.entries(changelog.find_section(self.path, "Unreleased")[2]), [])
        pending = changelog.section_markdown(self.path, "Unreleased", include_empty=True)
        self.assertIn(changelog.PLACEHOLDER, pending)

    def test_release_keeps_earlier_sections(self):
        changelog.prepare_release(self.path, "0.1.0", "2026-08-28")
        self.path.write_text(
            self.path.read_text(encoding="utf-8").replace(
                "### Client\n- _Nothing yet._",
                "### Client\n- A second cycle of work.",
                1,
            ),
            encoding="utf-8",
            newline="\n",
        )
        changelog.prepare_release(self.path, "0.2.0", "2026-09-01")

        labels = [label for label, _date, _body in changelog.read_sections(self.path)]
        self.assertEqual(labels, ["Unreleased", "0.2.0", "0.1.0"])
        self.assertIn("The scene graph is read.", changelog.section_markdown(self.path, "0.1.0"))

    def test_release_refuses_an_empty_queue(self):
        changelog.prepare_release(self.path, "0.1.0", "2026-08-28")
        with self.assertRaises(changelog.ChangelogError) as caught:
            changelog.prepare_release(self.path, "0.2.0", "2026-09-01")
        self.assertIn("no entries", str(caught.exception))

    def test_release_refuses_a_duplicate_version(self):
        changelog.prepare_release(self.path, "0.1.0", "2026-08-28")
        self.path.write_text(
            self.path.read_text(encoding="utf-8").replace(
                "### Client\n- _Nothing yet._", "### Client\n- More work.", 1
            ),
            encoding="utf-8",
            newline="\n",
        )
        with self.assertRaises(changelog.ChangelogError):
            changelog.prepare_release(self.path, "0.1.0", "2026-09-01")

    def test_bad_date_is_rejected(self):
        for bad in ("2026-13-01", "28-08-2026", "2026-8-1", "tomorrow"):
            with self.subTest(bad=bad), self.assertRaises(changelog.ChangelogError):
                changelog.prepare_release(self.path, "0.1.0", bad)

    def test_the_projects_own_changelog_parses(self):
        self.assertEqual(changelog.main(["check", "--require-entries"]), 0)


class ReleaseNotes(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.path = self.root / "CHANGELOG.md"
        self.path.write_text(CHANGELOG_TEMPLATE, encoding="utf-8", newline="\n")
        self.addCleanup(self.tmp.cleanup)

    def compose(self, **overrides):
        kwargs = dict(
            version="0.1.0",
            date="2026-08-28",
            commit="bfe622759462f012cb814a336147e79e1e7d1e8d",
            previous_tag=None,
            summary="",
            archive_dir=None,
            changelog_path=self.path,
            slug="themuffinator/Q2-PSX-PC",
        )
        kwargs.update(overrides)
        return release_notes.compose(**kwargs)

    def test_carries_the_changelog_and_the_provenance(self):
        text = self.compose()
        self.assertIn("The scene graph is read.", text)
        self.assertIn("`bfe62275`", text)
        self.assertIn("tagged `v0.1.0`", text)
        self.assertIn("2026-08-28", text)

    def test_states_that_no_assets_are_included(self):
        # The one claim in these notes that must never quietly go missing.
        self.assertIn("no game assets", self.compose())

    def test_summary_leads(self):
        self.assertTrue(self.compose(summary="A first release.").startswith("A first release."))

    def test_compare_link_uses_the_previous_tag(self):
        text = self.compose(previous_tag="v0.0.9")
        self.assertIn("compare/v0.0.9...v0.1.0", text)

    def test_without_a_previous_tag_it_links_the_commits(self):
        self.assertIn("commits/v0.1.0", self.compose())

    def test_download_table_lists_what_was_built(self):
        archives = self.root / "dist"
        archives.mkdir()
        for name in (
            "q2psx-pc-0.1.0-linux-x64.tar.gz",
            "q2psx-pc-0.1.0-windows-x64.zip",
            "q2psx-pc-0.1.0-freebsd-x64.tar.gz",
            "notes.txt",
        ):
            (archives / name).write_bytes(b"x")

        text = self.compose(archive_dir=archives)
        self.assertIn("`q2psx-pc-0.1.0-linux-x64.tar.gz` | Linux, x86-64", text)
        self.assertIn("`q2psx-pc-0.1.0-windows-x64.zip` | Windows 10/11, 64-bit", text)
        # An unlabelled platform is still listed, under its own suffix.
        self.assertIn("`q2psx-pc-0.1.0-freebsd-x64.tar.gz` | freebsd-x64", text)
        self.assertNotIn("notes.txt", text)

    def test_falls_back_to_the_pending_queue(self):
        # A dry run has not stamped the changelog, so [0.9.9] does not exist and
        # the preview has to come from [Unreleased] or it would be empty.
        self.assertIn("The scene graph is read.", self.compose(version="0.9.9"))


class Packaging(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.build = self.root / "build"
        (self.build / "bin").mkdir(parents=True)
        self.addCleanup(self.tmp.cleanup)

    def make_binaries(self, *names: str) -> None:
        for name in names:
            (self.build / "bin" / name).write_bytes(b"\x7fELF fake")

    def test_stage_collects_binaries_and_documents(self):
        self.make_binaries("q2psx", "q2psx-inspect", "stx2avi")
        staging = package.stage(
            build_dir=self.build,
            out_dir=self.root / "dist",
            version="0.1.0",
            platform="linux-x64",
            expect=package.EXECUTABLES,
        )
        self.assertEqual(staging.name, "q2psx-pc-0.1.0-linux-x64")
        for name in ("q2psx", "q2psx-inspect", "stx2avi", "README.md", "CHANGELOG.md"):
            self.assertTrue((staging / name).is_file(), name)
        self.assertTrue((staging / "docs" / "FORMATS.md").is_file())

    def test_stage_uses_exe_names_on_windows(self):
        self.make_binaries("q2psx-inspect.exe")
        staging = package.stage(
            build_dir=self.build,
            out_dir=self.root / "dist",
            version="0.1.0",
            platform="windows-x64",
            expect=["q2psx-inspect"],
        )
        self.assertTrue((staging / "q2psx-inspect.exe").is_file())

    def test_stage_fails_on_a_missing_binary(self):
        self.make_binaries("q2psx-inspect")
        with self.assertRaises(package.PackageError) as caught:
            package.stage(
                build_dir=self.build,
                out_dir=self.root / "dist",
                version="0.1.0",
                platform="linux-x64",
                expect=package.EXECUTABLES,
            )
        self.assertIn("was not built", str(caught.exception))

    def test_a_fetched_sdl3_ships_beside_the_client(self):
        # Without this the Linux and macOS archives contain a client that cannot
        # start, and nothing in the build would have said so.
        self.make_binaries("q2psx", "q2psx-inspect", "stx2avi")
        (self.build / "bin" / "libSDL3.so.0").write_bytes(b"\x7fELF fake")
        staging = package.stage(
            build_dir=self.build,
            out_dir=self.root / "dist",
            version="0.1.0",
            platform="linux-x64",
            expect=package.EXECUTABLES,
        )
        self.assertTrue((staging / "libSDL3.so.0").is_file())

    def test_archives_use_the_format_each_platform_can_open(self):
        self.make_binaries("q2psx-inspect")
        for platform, suffix in (("windows-x64", ".zip"), ("linux-x64", ".tar.gz")):
            with self.subTest(platform=platform):
                names = ["q2psx-inspect.exe" if platform.startswith("windows") else "q2psx-inspect"]
                self.make_binaries(*names)
                staging = package.stage(
                    build_dir=self.build,
                    out_dir=self.root / "dist",
                    version="0.1.0",
                    platform=platform,
                    expect=["q2psx-inspect"],
                )
                archive = package.make_archive(staging, platform)
                self.assertTrue(archive.name.endswith(suffix))
                self.assertTrue(archive.is_file())

    def test_archive_contents_are_rooted_in_one_directory(self):
        # Unpacking must not scatter files across the current directory.
        self.make_binaries("q2psx-inspect")
        staging = package.stage(
            build_dir=self.build,
            out_dir=self.root / "dist",
            version="0.1.0",
            platform="linux-x64",
            expect=["q2psx-inspect"],
        )
        import tarfile

        with tarfile.open(package.make_archive(staging, "linux-x64")) as bundle:
            roots = {name.split("/")[0] for name in bundle.getnames()}
        self.assertEqual(roots, {"q2psx-pc-0.1.0-linux-x64"})

    def test_checksums_cover_every_archive(self):
        archives = self.root / "dist"
        archives.mkdir()
        (archives / "q2psx-pc-0.1.0-linux-x64.tar.gz").write_bytes(b"one")
        (archives / "q2psx-pc-0.1.0-windows-x64.zip").write_bytes(b"two")
        (archives / "unrelated.txt").write_bytes(b"three")

        text = package.write_checksums(archives).read_text(encoding="utf-8")
        lines = text.strip().splitlines()
        self.assertEqual(len(lines), 2)
        self.assertNotIn("unrelated.txt", text)
        # Bare names, so `sha256sum -c` works from the download directory.
        self.assertTrue(all("/" not in line.split("  ", 1)[1] for line in lines))

    def test_checksums_need_something_to_check(self):
        empty = self.root / "empty"
        empty.mkdir()
        with self.assertRaises(package.PackageError):
            package.write_checksums(empty)


if __name__ == "__main__":
    unittest.main(verbosity=2)
