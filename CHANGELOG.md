# Changelog

What has changed, per release, written for someone deciding whether to download
this rather than for someone reviewing the diff.

Entries land under `## [Unreleased]` as work happens. Cutting a release moves
that section into a dated one of its own and empties the queue, and the same
text becomes the body of the GitHub release — so the notes are written by
whoever did the work, not reconstructed afterwards from commit subjects.

Headings are fixed; `scripts/changelog.py categories` prints the list, and
`scripts/changelog.py check` rejects anything else. Empty headings are dropped
when the notes are rendered, so leaving one at `_Nothing yet._` costs nothing.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
the version numbers follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
While the major version is 0, a minor bump is where behaviour is allowed to
change; see [`docs/RELEASING.md`](docs/RELEASING.md).

## [Unreleased]

### Reconstruction
- _Nothing yet._

### Client
- _Nothing yet._

### Rendering and audio
- _Nothing yet._

### Tools
- _Nothing yet._

### Fixes
- Warnings-as-errors had never passed on any compiler, so CI had been red since 20 August and nothing it said could be trusted. Every one is fixed rather than switched off: 33 on GCC and Clang, and another 20 behind them on MSVC, which stops at the first and so had never reported the rest. Six were real — two undefined behaviour (`gte_sxy` read through a `psx_xy` in the glint and bolt draws), a computation left dead by an earlier fix, an always-true bound on a `u8`, a POSIX function reached through a platform `#ifdef` that bought nothing, and a nested struct zeroed with too few braces. Thirteen were formats that really could truncate a path or a menu label and now say what they cut to. Two MSVC warnings are turned off, both with a reason: C4100 is the unreferenced parameter this project already ignores on GCC and Clang, and C4127 fires on every `CHECK(SOME_TRANSCRIBED_CONSTANT == 36, ...)` in the test suites, which is what those suites are for.
- A solid collision node did not hold nothing. `q2_coll_point_in_node` carried its own copy of the solid-bit mask instead of asking `q2_collision_node_is_solid`, and on one MSVC build the copy did not fire while the accessor did — so a node marked impassable let a point sit inside it. There is now one place that decides what solid means. Found only because MSVC had never been able to build the project in CI, so its tests had never run there.

### Build and packaging
- The repository is licensed: GPL-2.0, in `LICENSE`, and it ships in the release archives as that licence requires.
- Five megabytes of rendered frames — a title screen, a HUD test, two model renders, two level renders and an accidental screenshot of a terminal window — were tracked at the repository root while the README said the repository contains no game assets. They are gone, `.gitignore` covers them, and `scripts/check_paths.py` now fails the build on any tracked file that is an image, a sound or a film — by extension *or* by magic number, because the screenshot was a PNG named `C`.

### Documentation
- _Nothing yet._

## [0.1.0] - 2026-08-28

### Reconstruction
- Every on-disc format is read: the `.DAT` container, zones, scene graph, geometry, collision hulls, spawns, lights, triggers, level scripts, sound banks, models and animation. Checked across the PAL disc's 164 level files — 461,852 vertices, 274,936 quads, 139,240 collision planes, 94,642 collision portals, 1,723 models, 2,036,080 animation keys and 2,475 sounds — with zero failures.
- `docs/openquestions.md` closes with no open questions: 157 resolved, 18 partial with the remaining residue stated, and 4 marked terminal because this disc cannot answer them.
- All seven creature modules are transcribed from their own MIPS, 57 of 57 callbacks, together with the framework they run on — `T_Damage`, `M_ReactToDamage`, and the 71-slot import table named end to end.
- The player's whole frame, the pad read with its nine control styles, and the view's three independently decaying kicks are read out of the executable rather than approximated.
- The `.STX` film format is read *and* written. All 5,301 frames of the three films decode, and the encoder returns every one of `TAKE1BP.STX`'s 7,712 sectors byte-identical, EDC and Reed-Solomon included.

### Client
- `q2psx --version` reports the build and the commit it came from, the way `q2psx-inspect` already did.
- The campaign plays through: eleven levels across five units, Strogg Outpost to Final Showdown, with the mission screen at every unit boundary, the briefing on arrival, and the inventory carried across.
- The boot chain runs ahead of the menu — four logo screens and the intro film — and the campaign ends on the outro, both played to the frame the original stops them at rather than to the end of the file.
- The front end: title screen, single- and multiplayer pages, player, sound and video options, the credits, and all nine memory-card screens.
- Saved games in four slots plus quick save, holding the level clock, the script's flags, trigger residency, collected items, which doors are open and where in their travel, which windows are broken, and who is dead.
- Multiplayer with `--dm`: up to four players in split screen, each with its own spawn, pad, camera, viewport and inventory, sharing one world, played through to the frag limit and the scoreboard.
- Doors and lifts move, rotating brushes turn, scripted ambushes fire, key gates hold, hazards hurt and glass breaks.

### Rendering and audio
- A software rasteriser built to the PlayStation's rules rather than filtered to look like them: exact fixed-point GTE with its saturation flags and reciprocal table, affine texture mapping, ordering-table sort with no depth buffer, 15-bit RGB555 with the ordered dither, all four semi-transparency modes, texture pages, CLUTs and the mask bit. Every one is individually toggleable, so the same build runs perspective-correct at 4K.
- Models draw textured, animated, lit through the GTE's own three-light gather, and backface-rejected against the model linker's own NCLIP pair.
- Lens flares, the water warp, the damage flash, and the status bar with the icon and caption for what you just picked up.
- Audio: SPU-ADPCM sound bank playback for the menu, items, player and every creature; and each map's own seven-track XA playlist.

### Tools
- `q2psx-inspect` identifies a disc, verifies every level file against the documented schema, and checks every format claim in the docs against the disc — so "we understand this format" is something the build evaluates rather than something a document asserts. It also renders levels, models, HUDs and menus to a PPM with no window, and carries a PS-X EXE loader and an R3000A disassembler for the questions only the executable can answer.
- `stx2avi` demuxes a film into the raw video and audio streams ffmpeg can mux, driving the same decoder the game uses rather than a second one.

### Fixes
- The repository could not be cloned on Windows at all. A tracked file was called `nul.ppm`, and `NUL` is a DOS device name, so Git refuses to create it: the *checkout* failed, not the build. Renamed to `parity-frame.ppm`, content unchanged, and `scripts/check_paths.py` now fails CI on any tracked path Windows cannot create.
- Nothing had ever built on Linux. The link failed on undefined `sin` and `sqrt` because nothing asked for libm, which glibc keeps in a library of its own where Windows and macOS fold it into the C runtime.
- `disc.c` did not compile under GCC either. `fseeko`, `ftello`, `off_t` and `strtok_r` are POSIX rather than C11, and this project compiles as strict C11, under which glibc hides them — and an implicitly declared `strtok_r` would have truncated its returned pointer to 32 bits on a 64-bit host. A CD image can exceed what C11's `long`-based `fseek` reaches, so the POSIX interfaces are declared rather than given up.

### Build and packaging
- The version lives in one file, `VERSION`, which CMake reads to seed `project()` and the generated `version.h`. A binary, a tag and an archive cannot disagree about what was built.
- A manual release workflow builds Windows, Linux and macOS, tests each, and only then tags and publishes — so a tag never points at a commit that does not compile. `scripts/` holds the version, changelog, release-note and packaging tools it runs, each usable by hand.
- CI builds and tests on GCC, Clang, MSVC and Apple Clang with warnings as errors, and checks that the version each binary reports is the one in `VERSION`.
- A fetched SDL3 never shipped in the Linux or macOS archives, so those would have contained a client that could not start. Shared libraries now build beside the executables and the client carries an `$ORIGIN` rpath.

### Documentation
- `docs/RELEASING.md` describes the versioning scheme and how to cut a release.
