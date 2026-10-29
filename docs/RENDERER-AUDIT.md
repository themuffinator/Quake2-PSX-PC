# Renderer retail parity audit

Audited against the user's PAL SLES-01534 disc on 2026-10-23. The scope covers
world and model packet generation, GTE arithmetic, lighting, ordering tables,
texture fetches, software rasterization, framebuffer copies and screen composition.
This establishes specific corrections, not complete hardware equivalence.
The working tree already contained substantial renderer and gameplay work;
the findings below distinguish this audit's corrections from existing coverage.

## Corrections made

| Area | Previous gap | Current behavior and evidence |
| --- | --- | --- |
| World subdivision | A bilinear 5x5 grid replaced retail's midpoint graph. It changed intermediate XYZ, UV and RGB, especially away from the shared diagonal. | Uses the 21 rounded midpoint operations from PAL `0x800AEDD4` and `0x800B0884`. The centre is the midpoint of perimeter corners 1 and 3. Each operation rounds independently; cells touching SZ=0 are still rejected individually. |
| Model backface escape | Every face underwent NCLIP. Comments incorrectly connected the escape to block A, then left its source unresolved. | Part pointer load/advance at `0x800B22FC..230C`, mask load at `0x800B242C`, and branch/shift at `0x800B2498..249C` identify part+4. Its first 32 faces consume force-draw bits MSB first; the mask restarts per part. Block A remains the face-order directory. |
| Semi-transparency | Subtraction retained fractional foreground bits until after blending, darkening pixels too far. | Reduces the foreground to RGB555 before all four blend modes. For example, background channel 1 and foreground intensity 7 now remain 1. |
| Dither eligibility | Flat untextured and raw textured polygons were dithered. | Dithers eligible shaded/modulated polygons and lines; rectangles and raw/flat polygon paths bypass it. E1 still gates the command path. |
| Line rejection | Oversized lines were clipped and drawn. | Rejects spans over 1023 horizontally or 511 vertically before clipping. |
| Texture addressing | E2 had no representation; out-of-row texture/CLUT fetches disappeared; rectangle flip bits were ignored. | Adds persistent E2 UV bit replacement for all three texture depths, horizontal texture/CLUT row wrapping, and X/Y rectangle flips. These are core capabilities; the audit does not claim every one is exercised by a shipped scene. |
| Framebuffer masks | Texture bit 15 was lost, and E6 force/check-mask state was absent. | Preserves STP on written textured pixels, supports force-mask and masked-destination rejection, and applies E6 to framebuffer moves. Rejected writes also preserve the auxiliary scene/UI buffers. |
| GTE accumulators | Overflow checks ran after shifting and used the wrong width; cancelled intermediate overflows could disappear. | Checks and wraps 44-bit matrix accumulations before shifting, preserving both overflow directions. RTPS depth cue also flags MAC0 overflow. |
| GTE projection/selectors | Negative RTPS IR3 was forced to zero; a valid UNR quotient rounded to the maximum falsely signalled divide overflow; reserved/FC MVMVA selectors behaved as ordinary matrix operations. | Keeps signed IR3 separately from clamped SZ, distinguishes divide overflow from quotient rounding, and implements the documented MVMVA selector results. |
| GTE depth cue | NCD/CDP shifted the modulation product before interpolation, losing fractional bits and changing colour/flags. | Retains the raw product through the far-colour delta and depth interpolation, then shifts once at the defined stage. |

E1, E2 and E6 now have distinct persistent state. DRAWENV resets its texture
window without resetting E6; an E1-only packet changes neither E2 nor E6. The
screen integration test checks the resulting pixels, not just the registers.

Hardware behavior was checked against the original
[PSX-SPX GPU specifications](https://psx-spx.consoledev.net/graphicsprocessingunitgpu/)
and [GTE specifications](https://psx-spx.consoledev.net/geometrytransformationenginegte/).
Retail-specific decisions come from the disc's executable. Fixed-point scan
conversion is now adapted from the GPLv2-or-later
[Mednafen PS1 GPU](https://github.com/libretro-mirrors/mednafen-git/tree/master/src/psx).
Independent acceptance data comes from MIT-licensed
[ps1-tests](https://github.com/JaCzekanski/ps1-tests/tree/f727802fead11f1daa7549285548392ef87749cb).
See [fixture provenance and reproduction](../tests/fixtures/README.md).

## Follow-up: interpolation, coverage and remaining audit items

### Fixed-point rasterization

Replaced barycentric interpolation and endpoint-clamped UV rounding with fixed
edge and attribute accumulators in `raster_scan.inc`. Edges use directed division
and the reference subpixel bias; UV/RGB gradients truncate to twelve fractional
bits. Attribute origin, triangle traversal and equal-coordinate tie order follow
the original packet vertices. The second triangle of a Z-order quad is `(1,2,3)`;
reversing it changes tie behavior even though coverage has the same diagonal.
Lines use the reference fixed-point coordinate and colour stepping, including
endpoint direction and half-pixel bias. Dither runs before saturation and its
phase follows the native VRAM address.

The independently recorded GPU corpus now matches all 512 triangle rows, all
512 UV/colour-gradient rows, 100 shallow/steep line rows and 132 colour-line rows.
Before replacement only 66, 80, 81 and 80 rows matched respectively. Expected
hashes come from upstream PNGs, never from this renderer. This is strong evidence
for these cases, not an exhaustive proof over arbitrary packet streams.

### Subdivision pressure and cache ownership

`Resources+0x1C` supplies each zone's mesh count (40..180 on this disc); zero
selects 112 and a negative value disables the pool. Loads at `0x800714A8` and
`0x80070EB4` feed `0x80070AA8`; initialization at `0x80069408` gives every mesh a
nonzero 212-byte-offset handle. The client resets this pool on zone load.

The port now models the allocation cursor, cached handles and LIFO free stack.
Above six free entries allocation is unrestricted; with four to six free entries
it requires depth <= one quarter of the viewport threshold. The final three
entries are reserved for faces touching SZ=0. When those are exhausted the
callback drops that near-eye face instead of linking a flat smear. Existing
cached meshes bypass pressure. The outer variant/CLUT/distance gates still apply.

Each viewport owns distinct cached meshes, paired across the GPU's two frame
buffers; all viewports share the capacity. This follows the node packet directory
at `node+4+viewport` (`0x8006780C`) and paired packet handles at `0x800697CC`.
Unseen parent nodes release their meshes after each view (`0x80067E8C..68010`),
using the shared per-frame node stamp. Flat-linked meshes retire after all views;
farthest cached meshes are detached to restore a reserve of six, becoming free
at the next frame boundary (`0x80069250`). Tests cover thresholds, exhaustion,
multiple views, node lifetime, delayed retirement and allocation order.

### Native VRAM aliasing and transfers

Native screen composition binds the active framebuffer to its real 1024x512
VRAM rectangle. Raster writes immediately update texture-addressable VRAM, and
transfers can reach the other framebuffer. Both compact presentation buffers
are refreshed after composition, then detached before zone assets can be freed.
Adaptive rendering retains a separate host framebuffer because its dimensions
and texture placement do not describe a hardware VRAM surface.

Copies now read/write chunks of up to 128 words, left-to-right then top-to-bottom.
Overlapping rows/chunks can feed back; the old whole-copy snapshot cannot do so.
All 147 published overlap cases pass. Additional tests cover 1024/512 wrapping,
zero-encoded full extents, masks, the 128-word feedback boundary, texture feedback,
cross-buffer presentation, and adaptive isolation.

### Primitive decode and draw state

Oversized quads are rejected per triangle, preserving the valid half. Native
packet positions decode as signed eleven-bit fields and wrap after drawing
offset; adaptive host coordinates retain their extended range. Sprite/tile sizes
are already logical host extents rather than raw GP0 size words.

The earlier audit misidentified E1 bit 11 as an untextured-rendering control.
The current [GPU specification](https://psx-spx.consoledev.net/graphicsprocessingunitgpu/)
identifies it as the additional 1 MB VRAM bank selector, writable only when
GP1(09).0 permits it. On the retail 1 MB GPU, selecting that absent bank produces
no textured pixels. Without permission the write is ignored; this preserves
retail packets such as BIGGUN's `0x083D` page value. GP1 display-mode/start state
also controls DFE field-row exclusion in 480i; progressive drawing is unaffected.
State-transition tests cover these behaviors. The game uses progressive output.

### Hardware acceptance data

The former skipped GTE hardware-vector placeholder is replaced by a separate
368-vector corpus, including affected data registers and FLAG. It covers the
implemented RTPS/RTPT modes, MVMVA selectors, NCLIP, SQR, AVSZ and lighting/colour
operations. All 368 pass without further GTE arithmetic changes in this follow-up.
The upstream PSX log validates the source vectors. The fixture README states
which opcode modes are selected and which are outside the API/corpus.

## Follow-up: parent packets and cache visibility

### Parent allocation and material lifetime

The low-level parent allocator models `0x80070AA8`: a zero argument defaults to
4096, negative disables storage, and positive counts round up to 32 packets.
**28 October correction:** the port's adapter passes `Resources+0x0C` directly,
but the retail loader first resolves its signed/automatic policy. That adapter
is still a gap, detailed below. A block represents the corresponding packets in both GPU
buffers; the host ordering table's capacity does not set this budget. Each
node/view needs a distinct directory from the shared 255-entry pool and zero,
one or two blocks for its 0..63 faces (`0x80068044`, `0x8006DEB8`, `0x8006DEE4`).
The model supports the retail byte-sized block index range (256 blocks).
Out-of-range resource counts are rejected rather than reproducing memory
corruption from malformed input.

The per-view construction quota is initialized to 255 at `0x800384D8` /
`0x80038528` and copied for each walk at `0x80066AD8`. Successful constructions
consume it; cached nodes and failed allocations do not. The shared node frame
stamp is written only after successful parent lookup/construction. Variant 3
and deferred nodes still construct parents before their emitter can reject them.
The apparent distance gate does not impose a normal range: its only writer in
this executable initializes the signed comparison threshold to `INT32_MAX`
(`0x800708E0`).

Unseen nodes return directory IDs immediately after their view. Their mesh
handles follow the existing mesh cleanup rules, but parent blocks enter a
separate retirement queue (`0x8006D6F4`). Main drains that queue after mesh
cleanup and after all views (`0x80038DC4..38DD8`, `0x8006D73C`). A later viewport
therefore cannot reuse those blocks in the same frame. Both free stacks retain
retail allocation/return order.

Construction also fixes each face's texture page before culling. An opaque
face promotes the shared page table at `0x80068320`, including faces the emitter
will not draw. Existing transparent parent packets retain their earlier ABR;
subdivision children inherit it. The former per-emission page lookup incorrectly
changed those cached materials after a later promotion. Regression tests cover
this through the real world walker, including denial, retirement and rebuild.

### Texture and CLUT cache visibility

The persistent GPU state now includes 256 four-word texture lines (2 KB) and a
separate 256-entry CLUT. Texture addressing uses the page/window before the
cache lookup. Four-bit mode maps a 64x64-texel region; 8-bit mode maps 64x32 and
direct colour maps 32x32. A miss fills all four aligned words. Conflicting tags
evict each other; ordinary framebuffer writes/fills leave cached values intact.

GP0(01) invalidates both caches. VRAM copies and CPU upload/read notifications
invalidate texture words only. Changes to page X/Y, the permitted second-bank
bit, or the 4-bit/non-4-bit layout invalidate texture words. ABR, dithering and
8-bit/direct-colour changes do not. E2 changes addressing without flushing.
Native screen draw environments now use this same mode-update path. Asset and
HUD uploads notify the GPU, zone replacement clears retained cache state, and
water's five-word packet explicitly executes its GP0(01) before GP0(80).

CLUT loads occur at textured packet dispatch, before clipping or drawing can
modify the palette. A different CLUT or a request requiring 256 entries reloads
it. A cached 256-entry palette also serves a subsequent 4-bit request; changing
E1 without a textured draw does not change the retained CLUT. GP1 GPU reset
clears caches, while command-buffer reset clears the CLUT. The GP1(09) permission
latch survives soft reset, as specified.

The independent `gpu/clut-cache` replay matches all 256 reference rows
(262,144 pixels, including all 13 source cases). The uncached path disagrees on
five rows. The published capture specifically establishes 8-bit-to-4-bit reuse;
Mednafen's depth-equality CLUT tag would reload that case, so this implementation
follows the captured result. This is command-ordered visibility, not an emulation
of GPU execution cycles. See the fixture README for provenance and reproduction.

## Remaining validation and implementation limits

1. **Paired retail scenes and NTSC.** No matched retail camera/animation/light/RNG
   captures are available here. The user's PAL disc supports deterministic port
   captures, not a pixel-equivalence claim for retail gameplay. NTSC still needs
   matching disc/state captures, including near-eye geometry, translucent effects,
   water and multiple viewports.
2. **Cycle timing and scanout.** Texture/CLUT visibility now follows command
   order. GPU command duration, CPU/DMA contention and scanout racing the draw
   stream remain unmodeled. The port executes completed logical packets rather
   than a cycle-timed PS1 CPU/GPU; a retail packet/bus trace is still needed to
   establish whether those races affect shipped scenes.
3. **Broader GPU/GTE corpus.** Existing golden cases cover a defined subset,
   not every winding/coordinate/gradient combination or every GTE opcode mode.
   Add externally captured cases to broaden acceptance without regenerating
   expectations from the implementation under test.

## Existing behavior reviewed and retained

- SortData selects authored world order and portal-local environments. The
  Standard/Quick area sorter preserves model/effect chains as atomic objects;
  diagnostic depth sorting is a different path. No additional reproducible
  sorter defect was established during this audit.
- Perimeter quads and packets already in GPU Z order have separate diagonal
  handling. The top-left fill rule, transparent-zero texels, texture modulation,
  model normal lighting and dynamic world-light updates remain covered by the
  existing renderer/light/world tests.
- Screen composition provides viewport clips/offsets, overlay ordering, water
  copies and flash state. RGB555-to-host channel order has its existing runtime
  check. CRT and bloom are presentation features, not native framebuffer parity.
- Adaptive sizing and host simulation scheduling are intentional extensions.
  The legacy F2/`affine_uv` toggle currently has no rasterizer effect; a
  perspective-correct mode is not implemented. README and FIDELITY now state
  that limitation instead of advertising it.

## Validation and reproduction

The current follow-up passes all 67 CTest tests, including the texture-cache
replay and expanded parent/subdivision lifetime tests. Reference replays check
1,515,136 RGB555 pixels, 147 overlapping transfers and 368 captured GTE vectors.
Screen integration verifies native framebuffer feedback and adaptive isolation.

The earlier interpolation follow-up's headless smoke runs cover BASE1 (60 frames), BASE3, BIGGUN, POWER2 and four-player
FRAGTOWE (90 frames each), with zero reported warnings/errors. Native and adaptive
presentation are checked separately. All captures use the fixed `--headless`
clock, an independent save directory, and the user's installed disc.
BASE1 at 1920x1080 and four-player FRAGTOWE at 2560x1080 pass in adaptive mode.
Independent repeats of both native BASE1 and BASE3 captures are byte-identical.
The staged Release client reports `v0.2.0-19-gd93b1fe-dirty` and starts with its
staged SDL3 runtime.

```powershell
ctest --test-dir build-msvc -C Release --output-on-failure
# Run the following from .install:
.\q2psx.exe --disc disc/game.cue --map BASE1 --headless --aspect 4:3 --no-crt --no-bloom --frames 60 --shot ..\.agents\tmp\renderer-exact\base1-after.ppm --saves ..\.agents\tmp\renderer-exact\saves --report
```

Before/after/reference UV images and before/after BASE1/BASE3 frames are retained
locally under `.agents/tmp/renderer-exact/`, alongside build/test/smoke logs.
Game comparisons demonstrate the changes in the port, not agreement with a retail
screenshot. Raw native captures are 512x248 and comparisons preserve their pixels.

The original audit passed 62 tests and retained evidence under
`.agents/tmp/renderer-parity/`. It encountered an unrelated incident where build
outputs disappeared during linking/testing, so that run used independent copies
of the configured binaries. Its cleanup was blocked by automatic approval review.
The follow-up's normal configured-build test run does not reproduce that issue.

Automatic approval review also blocked the follow-up's cleanup command after
its temporary targets and link attributes were verified. The disposable
`fixture-source/` copy, downloaded `gte-tests-source.c`, source/diff backups and
duplicate repeat PPMs therefore remain under `.agents/tmp/renderer-exact/`.

The parent/cache follow-up retains its disassembly, logs and comparisons under
`.agents/tmp/renderer-cache/`. BASE1, BASE3 and four-player FRAGTOWE pass at
90 fixed headless frames with zero warnings/errors. Their before/after samples
are byte-identical; these scenes do not exhaust parent storage at the sampled
camera. Parent block use is respectively 17/125, 41/128 and 48/191, with no
last-view denials. Synthetic tests explicitly exhaust both blocks and directories
and exercise retries, quotas, shared node stamps and deferred returns. The CLUT
comparison image shows the visible stale-palette cases against the independent
reference. No new unrelated runtime issue was found.

Automatic approval review rejected this follow-up's temporary cleanup command
as blocked by policy. The disposable `views-disasm.txt`, `tests-first.log` and
`build-focused.log` remain in the task directory, along with the generator's
`tests/fixtures/__pycache__/` and the newly copied
`.agents/tmp/renderer-exact/fixture-source/gpu/clut-cache/` fixture input.

## Follow-up: authored particle pools and shared view budget

The previously unnamed Resources field at `+0x18` is the particle group count.
PAL's loader passes it to `0x80030C70` at `0x80070E98..70E9C` and
`0x8007148C..71490`. The disc has 151 declared resource records in 49 maps:
149 request 64 groups; JAIL4/ZONE0 and JAIL5/ZONE2 request 80. The port had
always used the allocator's zero-argument default of 32, with a hard ceiling
of 64 that could not represent those two records.

The client now reads the authored count on each zone load, supports 80 groups,
and preserves the native signed argument behavior: zero defaults to 32,
negative disables the pool. Clearing a disabled pool leaves it disabled.
The particle packet budget follows the current number of views. Native
`0x800304A8` initializes the allowance once; `0x80030AE8` loops to the next
view without replenishing it. The renderer now retains the remaining allowance
across viewports instead of granting each one a full pool.

The regression fills every authored slot and verifies the next spawn is
refused in all 115 shipped zones. Synthetic cases exercise 80 groups, disabled
pools, two/four-view exhaustion, hidden views preserving unused packets and
the next frame restoring the allowance. Resource lookup also covers signed
values, case/path matching, full twelve-byte names and truncated records.
Before the fix the initial disc regression failed 231 checks and the effect
regression failed 23. The controlled particle capture requests an 8×8 grid:
the old client emits 32 groups; the corrected client emits all 64, using the
production renderer and the disc's own particle texture. It is a diagnostic
fixture, not a paired retail gameplay capture.

Validation passes **1,219 effect checks**, **1,186 disc-client checks** (all
151 resource records and 115 shipped zones), **71/71 CTests**, and the
**215-case headless sweep** (213 executed cases plus two explicit no-gate
skips). The engine `--shot` path writes the same pixels as the initial
renderer capture; the before/after difference is confined to the 32 formerly
missing groups in the lower half of the fixture.

The refreshed `.install` client reports `v0.2.0-20-ga590ce9-dirty`; its executable
and SDL3 runtime hashes match the configured Release output. Three 90-frame
headless runs from `.install` pass with no warnings or errors: BASE1 uses
64 groups / 960 packets, JAIL5/ZONE2 uses 80 / 1,200, and four-player MATRIX5
uses 64 / 1,920. `reconstruction-notes/stage.json` records the arguments,
working directory, reports and hashes. The broader sweep still reports the
previously documented missing two-player HUD sheet in FRAGTOWE
(`openquestions.md` #52); it is not a regression introduced by this change.

### Confirmed remaining flat-packet allocation gap

Tracing the same loader disproved the earlier parent-allocation interpretation.
`Resources+0x0C` is not a direct capacity. Negative values force their magnitude
before round-up: QFRONT's `-3000` requests 3,008 packets and QFMV's `-1` requests
32; the earlier adapter disabled both. Those overrides are corrected below.
Nonnegative values select automatic sizing from the largest contiguous heap
payload after particle allocation. Both loader paths use
`32 * min(256, (free_bytes - (1882*mesh_count + 2)) / 3330)` with unsigned
32-bit arithmetic, then apply a negative override before calling `0x80070AA8`.

The parent allocator's own boundary tests do not verify that caller policy.
The automatic adapter and the required native heap accounting remain open; substituting
the authored positive value or an arbitrary maximum is not hardware parity.
Evidence is retained under `.agents/tmp/functional-parity/reconstruction-notes/`,
including the two loader disassemblies, particle allocation/draw disassembly,
resource census, regression logs and before/after engine captures.

Automatic approval review rejected this pass's cleanup command as “blocked by
policy.” The 194 temporary PPM captures, 22 isolated test saves and two
superseded baseline build logs remain under `reconstruction-notes/` (74.3 MB).
The target and all descendants were checked for links before the attempt;
useful logs, JSON reports, disassembly and the two PNG comparisons were to be
retained.

## Follow-up: signed resource overrides and native heap query

The resource adapter now negates a negative control before passing it to the
packet allocator. QFRONT receives 94 blocks / 3,008 packets and QFMV receives
one block / 32 packets. It also uses the shared Resources parser, so complete
twelve-byte names work and truncated or overlong names fail consistently.
The low-level allocator retains its distinct negative-argument disable rule.
`--report` now includes packet and subdivision capacities.

`generate_resource_reference.py` executes both native loader paths and packet
initialization, using explicitly injected heap-probe results. Its 30 signed
override/mesh cases agree across four heap sizes and both paths. Another 180
native runs establish that positive control magnitudes are ignored, and cover
automatic count boundaries and unsigned subtraction underflow. The generator
also executes the native heap allocator and Free without hooks: an empty heap
has **1,117,872 bytes of allocatable payload**, and its fragmented example has
holes of 300,028 and 317,788 bytes but reports only **317,760** payload bytes.
The previous FORMATS heap-extent arithmetic was 1,024 bytes too high; it is
corrected separately from the runtime adapter.

The new regressions failed 62 focused checks and both disc-backed override
checks before the fix. Current validation passes **480 allocator checks**,
**1,188 disc-client checks** across all 115 shipped zones, **71/71 CTests**,
and seven front-end/boot/movie cases, including natural movie exits. The
shared native interpreter still reproduces all 512 CD volume matrix fixtures.

The normal headless title capture is pixel-identical before/after. Its two
world quads are culled by the current camera (one backface, one bounds reject).
The report changes from zero blocks and two denied nodes to two used blocks
out of 94, two directories, and no denials. This verifies allocation behavior;
the unchanged image is not presented as a visible rendering improvement.

**Automatic sizing is still incomplete.** The client retains its explicitly
provisional positive-resource estimate until native allocation history can
supply the real largest block. Synthetic probe inputs do not prove complete
map capacities. The next integration must account for these traced consumers
and their lifetimes, including fragmentation across streamed loads:

- `0x80068CB8`: sector-rounded file allocation; both common and zone loaders
  request high-end placement. The common loader moves its allocation header
  forward to the Resources chunk at `0x8007B3C4`, releasing the earlier prefix.
- `0x8007407C` and `0x8007635C`: retained sound/VRAM data and the `Game Pal's`
  allocation; their shrink, copy and release order must be preserved.
- The mission initializers allocate 44, 208, 48, 192, 280 or four bytes from
  the high end. These can outlive an individual zone.
- `0x8003F880` selects the Quakes edict/player/path pools by session;
  `0x80062CF8` allocates the water packet pool and `0x80030C70` the particles.
  Zero-byte native allocations can still consume a 28-byte header.
- Image decode and recording allocations have separate callers; determine
  which remain resident when the largest-block query runs.

Evidence is under `.agents/tmp/functional-parity/flat-packets/`: native policy
results, disassembly, failing/passing regressions, frame comparison and test logs.
The final staged Release executable and SDL3 hashes match the configured
outputs; `.install` smoke runs for QFRONT, QFMV and BASE1 pass at 90 fixed
frames with zero warnings/errors. `stage.json` records the version, arguments,
hashes and reports. No new unrelated runtime issue was found.

Automatic approval review rejected cleanup of 13 verified disposable files
as “blocked by policy.” Their 4,211,925 bytes of PPM captures, a duplicate PNG
and a superseded build log remain here; `cleanup.json` records the targets.

## Follow-up: executable loader traces and heap accounting

The heap question now has measurements from actual PAL file loads, rather
than only synthetic largest-block inputs. `generate_heap_reference.py` executes
the native loader, archive relocations, creature initializers, image decoding,
allocator and resource policy with the user's disc. Explicit hooks provide CD
reads and BIOS strings/copies; GPU transfers, cache/critical sections and SPU
sample upload are stubbed. BIOS random state starts at one. This is a bounded
allocation experiment, not a hardware or whole-session capture.

The ten historical scenarios selected all six mission-record sizes, front-end/movie resource
overrides, a streamed zone round trip and two/three/four-view allocations.
**Correction:** the MATRIX6 cases below selected Tribute's four-byte record,
which retail multiplayer does not allocate. They measure that artificial input;
the mission-selection follow-up at the end of this document supersedes those
multiplayer heap figures and fixtures.
All ten also execute full unload and reload. The **22 completed load phases**
produce these packet capacities (the subdivision count comes from Resources):

| Scenario | Largest payload before parent pool | Native packets |
| --- | ---: | ---: |
| BASE1 zone 0, one view | 645,940 | 4,736 |
| BASE1 zone 1, streamed | 626,276 | 4,544 |
| JAIL2 zone 0, one view | 558,440 | 4,192 |
| POWER1 zone 0, one view | 704,240 | 5,600 |
| LAB zone 0, one view | 520,528 | 3,552 |
| BOSS1 zone 0, one view | 449,732 | 2,496 |
| QFRONT zone 0 | 798,052 | 3,008 (forced) |
| QFMV zone 0 | 979,016 | 32 (forced) |
| MATRIX6 zone 0, two views | 752,524 | 6,048 |
| MATRIX6 zone 1, three views | 714,124 | 5,696 |
| MATRIX6 zone 1, four views | 675,724 | 5,312 |

Returning from BASE1 zone 1 restores zone 0's capacity and allocation addresses.
Every full reload reproduces its initial capacity. The traces establish several
details the earlier resident-size inventory alone could not establish:

- Files allocate whole sectors, then immediately shrink to their exact length.
  High-end files move upward during this shrink. COMMON subsequently releases
  its prefix through Resources; ZONE releases its prefix through Scene.
- Decoded images are allocated and freed individually. Palette handles use two
  bytes per CLUT. SNDVRAM discards uploaded sound data, then shrinks to its
  retained header and moves high after the UI upload.
- A zone stream frees water, zone data, parent packets and particles, in that
  order. Mission, COMMON, palette/media headers and session pools remain live.
- Full unload releases the map and session pools but leaves water and the
  mission record. Water is released later in the next loader. The outer
  loading-screen setup also clears its old image-list alias before reload;
  omitting that precondition made the isolated experiment follow a freed pointer.

`nativeheap.c` now models the native first/last-fit placement, 28-byte headers,
zero-byte allocations, shrink/move behavior and largest-hole query without
storing payload bytes. `test_nativeheap` compares every returned address/query,
allocation count and ordered address/size hash across **1,070 native operations**:
**3,665 checks pass**. The heap bounds are explicit inputs; only the PAL bounds
have been exercised by these full loader traces. Invalid accounting requests
fail without mutating the model.

The heap-aware resource policy is also implemented as
`q2_world_subdiv_resources_heap`. Eighteen independently generated automatic
boundary rows, each tried with five nonnegative controls, cover unsigned
subtraction, clamping and the lower-level zero-count default. Together with
the existing pool/override cases, **754 allocator checks pass**. Shared native
interpreter changes preserve all 130 armour, 132 camera and 512 CD-matrix
reference results; the full configured suite passes **72/72 CTests**.

**The client lifetime adapter remains open.** Its existing positive-resource
estimate still supplies the renderer. These new components must be connected
to actual map/mission/session lifetimes, with wider transition and per-build
validation, before automatic sizing is considered fixed. No visible rendering
change is claimed by this accounting/test pass. Evidence and reproduction
details are in `tests/fixtures/README.md` and
`.agents/tmp/functional-parity/native-memory/`.

The final staged Release client and SDL3 hashes match the configured outputs;
the executable reports `v0.2.0-20-ga590ce9-dirty`. BASE1, BOSS1 and QFRONT pass
90-frame headless smoke tests from `.install` with zero warnings/errors.
Their reported packet counts remain 4,000, 3,456 and 3,008 respectively,
confirming the automatic client adapter is still pending. `stage.json` records
the exact commands, hashes and reports. No new unrelated runtime issue was found.

Automatic approval review rejected cleanup as “blocked by policy.” The
superseded `native-memory/reload.json` remains (137,819 bytes); its passing
scenario is covered by the broader `native-load.json` evidence. The smoke
tests created no save profiles. Earlier blocked cleanup targets were not retried.

## Follow-up: client heap-budget integration

The client now uses `levelmemory.c` to account for native allocations across
full loads and streamed zone changes. It supplies the actual COMMON, ZONE and
SNDVRAM sizes/prefixes, palette count, mission record and session settings.
The level-table byte at +0x20 distinguishes gameplay from presentation screens.
Heap bounds come from the executable's initializer instructions and RAM/stack
globals through its catalogued address layout. Accounting stores headers and
sizes only; game assets continue to use ordinary host allocations.

`level_memory_reference.h` records independent native final states for the
22 phases above. The model matches their largest payloads, packet/mesh counts,
allocation counts and ordered address/size hashes: **3,809 heap checks pass**
including the primitive vectors. The real client performs the same loads and
matches those states in **180 checks**. A refused missing-zone load preserves
its previous accounting. All **115 shipped zones** pass the client resource
census (**1,418 checks**, 151 resource records across 49 maps), with no heap
accounting failure. This establishes load coverage, not native reference
coverage for every zone.

The real renderer now gets **4,736 packets in BASE1**, **3,552 in LAB** and
**2,496 in BOSS1**, replacing provisional values of 4,000, 3,776 and 3,456.
Their 90-frame headless spawn captures are pixel-identical before/after: those
views fit both budgets. This is a capacity correction, with no visible
improvement claimed for those three images. QFRONT's forced 3,008 and QFMV's
forced 32 remain intact. `--report` exposes `world.heap_accounted`,
`world.heap_largest` and `world.heap_failures`; unknown layouts and failed
accounting retain the legacy resource fallback.

The full configured suite passes **73/73 CTests**. Eighteen targeted headless
runs also pass, covering streamed gates, scripted map changes, save restores,
two/three/four-player split-screen, presentation screens and campaign completion.
All finish with active heap accounting and zero accounting failures. The two
script-trigger sweeps intentionally fire gates away from their doorway and
report the existing destination-spawn fallback warnings; there are no errors.

These tests also exposed and fixed a separate QFRONT reload failure: zone
history attempted to serialize its logo's private animation callback. The
outgoing sim's `scene_ready` now excludes those freshly recreated presentation
props from campaign population history. It works even when the client has
already changed its front-end flag for the destination. The disc-backed reload
comparison and front-end/campaign checks exercise this path.

Wider cross-mission/session histories and independent NTSC loader execution
remain open. Reading a revision's own bounds is not a substitute for that
comparison. Evidence is in `.agents/tmp/functional-parity/native-budget/`;
`comparison.json` records the unchanged pixels, and `sweep/summary.json` records
the 18 targeted runs.

The refreshed staged Release executable reports `v0.2.0-20-ga590ce9-dirty`;
its executable and SDL3 hashes match `build-msvc/bin/Release`, and LICENSE and
Play.cmd are present. BASE1, BOSS1 and four-player MATRIX6 pass 90-frame
headless smoke runs from `.install` with zero warnings/errors and the expected
4,736 / 2,496 / 5,312 packets. `native-budget/stage.json` retains commands,
working directory, hashes, logs and reports.

Automatic approval review rejected temporary-file cleanup with “blocked by
policy.” The 25 PPM captures and two test saves remain (9,556,353 bytes), along
with empty test directories. Exact paths and all ancestors were checked for
links; every PPM has a pixel-identical retained PNG. `native-budget/cleanup.json`
and `capture-manifest.json` record this. Earlier rejected targets were not retried.

## Follow-up: US loader and retained mission allocations

The native loader runner now executes both verified PAL SLES-01534 and US
SLUS-00757 segments. US entry points, globals, BSS and GP follow the verified
layout and symbol table; the interpreter runs the US instructions themselves.
The native heap starts 624 bytes later on US, at `0x800EE988`, while both
revisions retain the same physical ceiling, `0x801FF600`.

Each revision has 22 reference phases covering fresh loads, streams, full
reloads and two/three/four-player sessions. A further 17-load sequence retains
RAM across the front end, campaign maps, all five intermission screens, a
movie, and two/four-player multiplayer. All **78 phases** match native largest
payloads, renderer packet/mesh capacities, allocation counts and ordered
address/size hashes. LAB receives **3,552 packets on PAL** and **3,520 on US**;
the selected intermission loads receive **7,840 / 7,808** respectively.

The retained-session comparison exposed a client bug: its intermission/movie
mission fallback was inside the condition requiring level text, although
these screens have no Strings section. The adapter now stores the actually
loaded mission id and retains it for these transitions. A fresh movie or boot
sequence has no inherited mission allocation; it must not use the mission
object's default unit 1. The original failing control has six mismatches per
revision in largest payload, allocation count and heap hash. The chosen views'
packet counts were unaffected by that error, so this accounting fix makes no
claim of a visible improvement.

The fixture explicitly selects mission ids and session settings, then invokes
the native unload, mission release/initializer and loader in their outer
caller's order. It does not execute the entire campaign state machine. These
results close the previously missing US loader comparison and add selected
session history coverage; arbitrary outer-state selection, recording
allocations and hardware timing remain unqualified.

Validation passes **4,241 heap checks**, **306 real-client checks per disc**,
and **1,418 resource-census checks per disc** (49 maps, 151 resource records,
115 loaded zones each). All **73 CTests** pass. Sixteen targeted headless runs
cover boot, new game, saves, four-player split-screen, front end, intermission,
standalone movie completion and campaign unlock on both revisions. Every run
finishes with active heap accounting and zero accounting failures. The shared
resource-policy generator still matches all 420 native runs and 30 stored
forced-count vectors.

Evidence is in `.agents/tmp/functional-parity/native-sessions/`: native JSON
traces, original failing client controls, passing logs, `fixture-check.json`,
and `sweep/summary.json`. The four numeric load fixtures contain no retail code
or assets. Reproduction and stubbed native services are documented in
`tests/fixtures/README.md`.

The current staged Release executable reports `v0.2.0-20-ga590ce9-dirty`;
its executable and SDL3 hashes match `build-msvc/bin/Release`, with LICENSE
and Play.cmd present. Four 90-frame headless runs from `.install` pass with
zero warnings/errors: LAB on PAL/US has 3,552/3,520 packets and fresh QFMV
has 32 on each. `native-sessions/stage.json` records these checks. No new
unrelated runtime issue or stale worktree was found.

Automatic approval review rejected this pass's cleanup with “blocked by
policy.” The 22 raw captures, two isolated test saves and temporary US
executable segment remain (8,911,904 bytes). Every capture has a verified,
pixel-identical PNG; the segment was verified against the retained US disc.
Exact targets and all ancestors were checked for links. `cleanup.json` and
`capture-manifest.json` record the retained files; earlier rejected targets
were not retried.

## Follow-up: native mission selection

Executing `GameNewMission` (`0x8003F37C`) exposed an incorrect fixture and client
assumption: mission id 6 belongs to `InitMissionTribute`, whose default map is
the unshipped HalfLife. Multiplayer restart (`GameReRunMPLevel`, `0x80041958`)
selects id 7 and `InitMissionNull`, allocating no mission record. The previous
multiplayer fixtures called the Tribute initializer directly. They demonstrated
allocator agreement for those supplied inputs, not the native multiplayer
mission choice.

The regenerated loader fixtures execute the dispatcher and its direct-level
lookup. A separate native run covers all **102 named PAL/US entry selections**
and **22 successive selector calls**, including new game, campaign units,
Tribute, null mission and replacement of an existing record. Direct selection
reads the signed mission id at level-table `+0x16`. The client now uses that
field and allocates no mission record for multiplayer. The reader also stops
at the actual table sentinel: 51 named rows plus the empty-name/BADLEVEL row.
The adjacent Base3/Jail2 bytes are not a 53rd record.

The corrected references expose **32 failing checks per disc** in the old
client. Eight multiplayer loads per revision each disagree on mission id,
largest payload, allocation count and ordered heap hash. Removing the four-byte
payload and 28-byte header restores 32 bytes to the largest available gap.
For MATRIX6, PAL's largest payloads become **752,556 / 714,156 / 675,756** for
two/three/four views; US has **751,932 / 713,532 / 675,132**. The rounded packet
capacities remain **6,048 / 5,696 / 5,312**. Other selected native load states
are unchanged.

The real-client comparison covers **78 arena/view combinations**: 13 shipped
arenas, two/three/four players, and both revisions. Every case regains 32 heap
bytes. One crosses a packet-block boundary: **four-player US TIMS changes from
5,632 to 5,664 packets**. Independent native TIMS loads/reloads confirm 5,664
on both revisions; those extra four phases expand the load fixtures to **82**.
The regenerated model suite passes **4,649 checks**, and the real client passes
**520 checks per disc**. The captured TIMS image shows the current renderer;
it is not a paired retail frame comparison.

All **73 CTests** pass, and both affected tests pass again with the additional
TIMS references. Both all-zone resource censuses pass **1,418 checks** (115
zones each). The multiplayer acceptance suite passes **18 scenarios per disc**,
covering four live players in every shipped arena, complete matches, results,
all-player continuation, teams and Versus round restarts. No new unrelated
runtime issue was found.

This closes direct-level mission selection and the multiplayer-null discrepancy.
The longer fixture still supplies the departing unit for intermission/movie
screens and selects view counts; arbitrary outer-state history and recording
allocations remain outside its evidence. All native services stubbed by the
loader runner remain documented in `tests/fixtures/README.md`.

Evidence is retained in `.agents/tmp/functional-parity/mission-selection/`,
including `mission-reference.json`, native traces, `native-comparison.json`,
and the original failing client controls.

The staged Release executable reports `v0.2.0-20-ga590ce9-dirty`; its EXE and
SDL3 hashes match the build outputs, and LICENSE/Play.cmd are present. Final
90-frame headless runs from `.install` pass for PAL/US four-player TIMS and
PAL BASE1, with the expected 5,664 / 5,664 / 4,736 packets and no warnings or
errors. `stage.json` records commands, working directory, hashes and reports.
The only registered worktree is the main checkout.

Automatic approval review rejected cleanup as “blocked by policy.” The 38
raw PPMs and two superseded probe JSON files remain (14,277,570 bytes), along
with empty test directories. Every capture has a verified pixel-identical PNG.
`cleanup.json` records the exact checked targets and rejection;
`capture-manifest.json` records the image checks. No earlier rejected target
was retried.

## Follow-up: creature and gameplay-player ambient

Native entity allocation seeds RGB 64; gameplay player spawning replaces it
with RGB 32. The client had used 48 for both. Creature and player-body draws
now use their respective native values, including recovery after energy hits.
Refresh retains transient colour; each new player body resets to 32.

The [functional audit](FUNCTIONAL-AUDIT.md#actor-ambient-lighting) records
the PAL/US instruction comparisons, hooks and scope. All 73 CTests and the
disc-backed PAL/US creature checks pass. Actual headless before/after captures
change only the observed bodies: creatures become brighter and player bodies
darker. Paired retail-frame validation remains open.
