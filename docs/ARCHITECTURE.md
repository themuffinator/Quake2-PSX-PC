# Architecture

## The shape of the problem

Quake II on PlayStation is not Quake II with a different renderer. Hammerhead
rebuilt the game for a console with 2 MB of RAM, 1 MB of VRAM, 512 KB of sound
RAM and no floating-point unit. The world is not a BSP tree streamed from a
`.bsp`; it is a set of pre-baked *zones* with their own geometry, collision,
lighting and visibility data. The entity system, AI, and level scripting are
Hammerhead's own.

So this project is not a port of id's GPL source with PSX data loaded into it.
It is a reimplementation of Hammerhead's engine, informed by the original
executable's structure and validated against the original data.

## Layering

Dependencies point strictly downward. Nothing in a lower layer knows a higher
layer exists.

```
        ┌──────────────────────────────────────────┐
        │  client      window, input, main loop    │
        ├──────────────────────────────────────────┤
        │  game        entities, AI, physics       │
        ├──────────────┬───────────┬───────────────┤
        │  screen      │  menu     │  audio        │
        ├──────────────┴───────────┴───────────────┤
        │  render      rasteriser, PSX rules       │
        ├──────────────────────────────────────────┤
        │  psx         GTE + GPU primitive model   │   ← fidelity core
        ├──────────────────────────────────────────┤
        │  formats     .DAT container, level schema│
        ├──────────────────────────────────────────┤
        │  build       release identification      │
        ├──────────────────────────────────────────┤
        │  disc        images, ISO9660, CD-XA      │
        ├──────────────────────────────────────────┤
        │  common      types, fixed point, hashing │
        └──────────────────────────────────────────┘
```

### `common`
Fixed-width types, little-endian unaligned readers, the fixed-point formats, and
SHA-256. Nothing here allocates policy.

The endian readers matter more than they look: every integer on the disc is
little-endian because the console was, and the host may not be. No code in this
project casts a pointer into disc data to a wider type.

### `disc`
Turns "a thing the user pointed at" into a flat file namespace plus a track list.
Handles `.cue`/`.bin`, `.iso`, and bare images; understands Mode 1, Mode 2 Form 1
and Mode 2 Form 2 sectors, which matters because ordinary files are Form 1 while
streamed audio and video are Form 2 with a different payload size. Multi-file
CUE sheets are laid out as a single disc: each FILE-local INDEX is rebased after
the complete preceding backing file, while reads still seek to INDEX 01 inside
their own file. Raw reads inside a physically stored INDEX 00 range seek
backwards from that INDEX 01 offset into the upcoming track's backing file.

The public surface is deliberately small — `disc_find`, `disc_read_file`,
`disc_read_raw_sector` — so that adding CHD support or a physical CD backend
later touches nothing above this layer.

### `build`
Identifies *which release* the disc is, keyed on the SHA-256 of the boot
executable rather than on the region. This is the design decision most likely to
be questioned, so: the game's text, level table and pickup tables live inside the
executable. A localised or revised release moves them. Keying data-table offsets
off "PAL" would break the first time a German disc appears; keying them off the
exact executable cannot.

An uncatalogued disc is *not* rejected. It reports as unknown and runs in generic
mode, because refusing to boot on a regional release nobody has dumped for us
would break the project's central promise.

### `formats`
Parsers for on-disc data. The `.DAT` container is a fixed-schema directory of
named chunks; `level.h` maps those names to typed slots.

The chunk schema was established by census over every level file on the disc, not
by reading one file and generalising — see `q2psx-inspect dats`. That distinction
caught a real subtlety: the last directory entry is a nameless sentinel whose
offset is end-of-data, and chunk *indices* are not stable across files even
though the *name set* is closed.

### `psx` — the fidelity core
The GTE and the GPU primitive model. This is where the PlayStation look is
decided, and it is deliberately a layer of its own rather than part of `render`.

The game emits GPU primitives into an ordering table exactly as the original did.
Backends consume that primitive stream. Keeping this indirection is what makes
faithful rendering possible at all: the backend sees what the hardware saw, so it
can reproduce the hardware's rules rather than approximate the result.

See [`FIDELITY.md`](FIDELITY.md) for what that buys and how it is verified.

### `render`, `audio`, `game`, `client`
`render` consumes `psx_ot` and rasterises it with the hardware's rules. `screen`
sits on top of it and owns everything the console's display code owned: the
display and draw environments, the double buffer, the shape of the ordering
table, the viewport layouts and the frame lock. It is the reason `render` grew a
clip rectangle and a drawing offset, and the reason `psx_ot` grew a window — on
the console those are not conveniences but the mechanism, because split screen
is one table walked once with draw-env packets inside it changing the clip.
`audio` has SPU-ADPCM for sound effects and CD-XA ADPCM for streamed music; plain
PCM for the CD audio track is not wired up. `game` is the reimplemented simulation, and
it also owns the modules that turn data into primitives: `world.c` for brush
geometry, `modeldraw.c` for posed models and `effect.c` for the presentation
layer. `client` is the SDL3 host.

`effect.c` is in `game` rather than in `render` on purpose. An effect is spawned
by a gameplay event and integrated by the gameplay clock — the console puts its
particle groups and debris on the same entity list as everything else — so a
headless caller gets the bursts and their motion with no screen attached, and
the client's only job is to append them to the frame's ordering table. Putting
them in `render` would have made "what does a rocket detonating look like" a
question only a windowed build could answer.

`build` has a second job beyond identifying the release. It carries a PS-X EXE
loader and an R3000A disassembler, because the remaining unknowns are questions
about the original's *code*, and answering them should not depend on a tool
outside this repository.

### Saved games, and where the seam falls

The save system is split across two layers, and the split is the interesting
part rather than an implementation detail.

`menu/memcard.[ch]` is the **front end**: nine screens transcribed from the
executable's item tables, the release-gated state machine that drives them, and
three function pointers — poll, request, act-on-a-row — where the console calls
into `libmcrd`. That is where the reconstruction stops, because behind those
pointers is hardware a PC does not have.

`game/save.[ch]` and `game/saveui.[ch]` are the **back end**: what a save
contains, how it is written, and a file-backed implementation of exactly those
three function pointers. They are in `game` rather than beside the front end
because they need the simulation and nothing from the menu — so the dependency
points one way, and the whole flow can be driven and tested with no screen
attached. `client` is the only thing that knows about both, and binding them is
four assignments.

Two consequences are worth stating.

The **container is a deliberate divergence**. The original wrote 8 KB memory-card
blocks with a directory and a whole layer of "insufficient free blocks" handling;
this writes an ordinary host file with a chunked, checksummed, fixed-width
layout. Unlike the rendering, where the hardware's limits *are* the experience, a
save file's container is invisible — so reproducing block management would buy
failure modes and nothing a player can perceive. What the save *contains* still
mirrors the original's state.

What it contains is the other consequence, and it is larger than it looks: the
level clock (every powerup expiry and refire gate is an absolute deadline on it),
the script's event flags, which trigger volumes the player is *standing in*,
which items have been collected, the mover's carried state and the weapon
generator. A save with position and inventory alone writes a file and does not
restore a game.

`q2psx-inspect save` runs the whole thing against a real map — capture, write,
read back, compare field by field, apply to a fresh simulation — and can draw
every screen the front end passes through.

## Why C11

The original is C. The data structures are C structures. Fixed-point arithmetic
with explicit overflow behaviour is easier to keep honest without operator
overloading quietly changing the type of an intermediate. The project builds with
clang, gcc and MSVC, and `-fwrapv` is on deliberately — the GTE relies on
two's-complement wrapping to reproduce hardware saturation.

## Testing strategy

The offline tool is the test harness. `q2psx-inspect verify` runs every level
file on the disc through the real typed loader — the same code the engine will
use — and fails if any file has an unknown chunk or is missing a mandatory one.
It currently passes on all 164 COMMON/ZONE files of the PAL build.

This is the pattern to keep: every format claim in `FORMATS.md` should have a
corresponding check in the tool, so that "we understand this format" is a
statement the build system can evaluate rather than an assertion in a document.

## Current status

| Layer | State |
|---|---|
| `common` | working — types, fixed point, fixed-point trig, SHA-256 |
| `disc` | working — cue/bin, iso, ISO9660, Form 1/2, SYSTEM.CNF |
| `build` | working — PAL build fingerprinted and catalogued |
| `formats` | container, level schema, vertex pool, scene/geometry, collision, spawns, lights, models and their animation |
| `psx` | GTE and ordering table implemented; needs conformance tests |
| `render` | software rasteriser working; world and models textured |
| `audio` | sound bank and SPU-ADPCM working; XA music and CD-DA not started |
| `game` | zone loading, OT construction, model drawing, the simulation, the effects, and the multiplayer session |
| `client` | SDL3 client flies through a zone at the console's own resolution |

Validated against the PAL disc (`q2psx-inspect verify` and `audio`):

```
COMMON.DAT : 49 resolved
ZONE*.DAT  : 115 resolved
vertices   : 461852 across all zones
quads      : 274936
coll planes: 139240, of which 139057 are unit normals (99.87%)
spawns     : 240
lights     : 7814, 0 failing the radius invariant
failed     : 0

banks      : 49
sounds     : 2475 (118 looping)
invalid ADPCM blocks : 0
largest bank : FRAGTOWE, 522000 bytes against 522240 usable
```

Geometry renders end to end. Across 19 maps spanning a 2-quad stub to a
5,875-quad level, every quad transforms and emits with none rejected.

## What is genuinely known versus assumed

Worth stating plainly, because a reimplementation built on a confident guess is
worse than one built on an acknowledged gap.

**Established by evidence across the whole disc:**

- The `.DAT` container, including the nameless end sentinel.
- The complete chunk name set for COMMON.DAT (14, plus one map-specific extra)
  and ZONE*.DAT (12 mandatory, 2 optional), and the fact that chunk *order* is
  not stable while the *name set* is closed.
- The `Points` chunk: a grouped vertex pool, 12 bytes per point, exact size
  agreement on all 115 zones and coherent level-shaped bounding boxes.
- The disc's identity: serial, executable hash, volume timestamp.

- `Scene` nodes and `MapMod` quads: 17,035 nodes and 274,936 quads with zero
  violations of the size identities, re-derived independently before being
  committed to.
- The collision hull layout, including its size equation and sentinel.
- Collision plane normals as 1.3.12 unit vectors.
- The sound bank, the VAG headers' big-endianness, and SPU-ADPCM.
- Spawn points and lights, the latter confirmed by an arithmetic invariant
  between two of their own fields.

**Not yet established:**

- The texture codec. Every texture on the disc is behind it, so nothing can be
  drawn textured. This is the single biggest gap.
- The `CastList` face vertex-index base, so no models.
- The world coordinate scale relative to PC Quake II, so no correct physics.
- The `Events` operand encoding, so no doors, lifts or level progression.
- The three trailing bytes on each point. They read as a reverse index into
  `MapMod` corners and hold for 99.971% of entries, which is close enough to be
  suggestive and far enough from 100% that they should be rebuilt at load time
  rather than trusted.
- Collision plane *points* are only 95.6% confirmed. Player movement must not be
  built on them until that reaches 100%.

**Closed since this list was written**, and worth recording because both closed
the same way — by looking outside the executable:

- The `UserFuncs` primitive a trigger volume calls is now resolved from its
  record, so `INCROUCH`, `INLOWCROUCH`, `INWATER`, `UNDERWATER` and `DONTJUMP`
  come from the level data rather than from a caller-supplied flag. That also
  settles "`entity+0x98 & 0x20000` is never set": nothing in the *binary* sets
  it, and 52 volumes across 15 maps do.
- `client+0x156`'s three timed halfwords are the **view kicks**, not a wobble:
  three amplitudes with three deadlines and three decay periods, composed into
  the camera's angles at `0x80038260`.

The reverse-engineering findings, with per-field confidence markers, live in
[`FORMATS.md`](FORMATS.md); the prioritised gaps are in
[`openquestions.md`](openquestions.md).
