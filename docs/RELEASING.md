# Versioning and releasing

## Where the version lives

In one file:

```
VERSION          0.1.0
```

Everything else derives from it, in one direction:

```
VERSION ──> CMake project()  ──> generated version.h ──> what a binary prints
        └─> scripts/version.py ──> the git tag  v0.1.0
                               └─> archive names q2psx-pc-0.1.0-linux-x64.tar.gz
                               └─> the GitHub release title
```

Nothing reads the version from anywhere else, so a binary, a tag and an archive
cannot disagree about what was built. The header also records `git describe` and
a dirty flag, which is how a binary stays traceable to a commit even when it was
built from a tree ahead of the tag — but that is *provenance*, not the version.

A plain text file rather than a literal inside `CMakeLists.txt` is deliberate:
cutting a release means writing a new version, and rewriting a file that holds
one number cannot go wrong the way a regex against build-system source can.

```bash
python scripts/version.py show
```

## What the numbers mean

Semantic versioning, read against the *disc*:

| Bump | For |
|---|---|
| **major** | Reserved for 1.0.0 — the campaign, the front end and the fidelity switches all correct, with nothing known to be wrong. |
| **minor** | New behaviour: a subsystem that now runs, a format that is now read, a fidelity switch that now exists. While the major version is 0, this is also where behaviour is allowed to *change*. |
| **patch** | Fixes and packaging, with no new behaviour. |

Saved games are the one compatibility promise: the container is chunked,
checksummed and fixed-width so a save written by one build loads under another.
A release that cannot keep that gets a minor bump and says so in its notes.

## The changelog

`CHANGELOG.md` is a queue, not a history that gets written at release time.
Entries land under `## [Unreleased]` as the work happens, under one of the fixed
headings:

```bash
python scripts/changelog.py categories   # the headings
python scripts/changelog.py check        # does the file parse?
```

Write for someone deciding whether to download this, not for someone reviewing
the diff. Empty headings are dropped when the notes are rendered, so leaving one
at `_Nothing yet._` costs nothing.

Releasing moves that section, verbatim, into a dated section of its own and
empties the queue. The same text becomes the body of the GitHub release, which
is the point: the notes are written by whoever did the work, while they still
remember what it was for.

The workflow **refuses to release with an empty queue**. Release notes that say
nothing are worse than no release.

For a large release, `docs/releases/<version>.md` can supply concise highlights
for the GitHub release body. The renderer uses that file when present and links
to the full changelog at the release tag. The complete dated changelog still
ships in every archive; releases without a highlights file use it verbatim.

## Cutting a release

From the repository's **Actions** tab, run **Release**:

| Input | |
|---|---|
| `bump` | `none` releases the version in `VERSION` as it stands — which is what a first release wants. `patch` / `minor` / `major` compute the next one. `explicit` uses the `version` box. |
| `version` | Only read when `bump` is `explicit`. |
| `prerelease` | Marks it a pre-release on GitHub. |
| `summary` | An optional one-line lead above the notes. |
| `dry_run` | Does everything except tag and publish. |

It runs in three stages, and the order is the whole design:

1. **Validate.** Resolve the version and refuse it if the tag exists or the
   number goes backwards. Check the changelog has entries. Render the notes and
   put them in the job summary. Nothing has been built or written yet, so a bad
   input costs about twenty seconds.
2. **Build.** Windows, Linux and macOS, each with the resolved version stamped
   into `VERSION` first, each running the full test suite, each packaging its own
   archive. Any failure stops here.
3. **Publish.** Only now: commit the version and the stamped changelog, tag it,
   push, and create the release with the archives and `SHA256SUMS.txt`.

**The tag is pushed after the build succeeds, never before.** A tag pointing at
a commit that does not compile is worse than no tag, because it looks releasable
forever after.

### Try it first

Run it with `dry_run` ticked. You get the archives as workflow artifacts and the
rendered notes in the job summary, and the repository is untouched — no commit,
no tag, no release. Worth doing whenever the packaging or the notes have changed.

## Doing it by hand

Each step is a script that runs on its own, which is how they are debugged:

```bash
python scripts/version.py resolve --bump minor      # what would this release be?
python scripts/changelog.py show                    # what would the notes say?
python scripts/release_notes.py --version 0.2.0     # the whole rendered body
python scripts/package.py stage --platform linux-x64
python scripts/package.py stage --build-dir build-msvc --config Release --platform windows-x64
python scripts/package.py checksums --archive-dir dist
```

`scripts/check_paths.py` is the fifth, and it is not about releasing: it rejects
any tracked path Windows cannot check out. A file called `nul.ppm` once made the
repository impossible to clone on Windows — `NUL` is a DOS device name, so the
*checkout* failed, not the build — and nothing reported it until a Windows runner
tried. It runs in CI so the next one fails on a pull request instead.

`scripts/test_release_tooling.py` covers all five, and CI runs it, so a change
here fails on a pull request rather than at release time.

## What ends up in an archive

[`scripts/package.py`](../scripts/package.py) uses an explicit allowlist:

- `q2psx`, `q2psx-inspect` and `stx2avi`, with their platform extensions and runtime libraries.
- `USER-GUIDE.html`, the complete offline player manual, stamped with the release version.
- `README.txt`, a short, platform-specific entry point to the manual and game.
- `LICENSE`, `CREDITS.md` and the full `CHANGELOG.md`.
- `Play.cmd` and `Play.vbs` on Windows.

The repository README, implementation notes and format specifications stay in
the source repository. The manual links to them online where appropriate;
its essential instructions, styling and navigation need no internet access.
Commercial artwork, disc-derived media, reconstructed fonts, disc images,
saves and local settings are excluded. Do not add a directory glob to the
package allowlist.

## Maintaining the player guide

Edit [`USER-GUIDE.html`](../USER-GUIDE.html) directly; it is a standalone,
readable source document. Keep its single `build-version` marker intact.
The packager replaces “Development edition” with the resolved release version.
Windows `stage-local` stamps and copies the same guide using CMake, so a normal
local build also leaves a current manual in `.install/` without requiring Python.

Follow [the visual design reference](DESIGN.md), especially document mode,
readability, editorial voice and the user-guide layout. Use the palette from
`docs/design/tokens.json`, installed reading-font fallbacks, restrained section
numbers and one-column procedures. Local reconstructed display fonts and
commercial reference imagery are not release assets.

Before release, run the tooling tests, open the **packaged** HTML offline,
check a narrow viewport and browser print preview, and follow its local links.
The tests check version stamping, navigation targets, offline dependencies,
palette consistency and archive contents. They do not replace visual review
or checking instructions against the current client.
