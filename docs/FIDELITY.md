# Rendering fidelity specification

The goal is not "a PlayStation filter". It is that a frame produced by this port
and a frame produced by the original hardware, given the same game state, should
be the same frame — including the parts that were, strictly speaking, hardware
limitations.

That is only achievable if the pipeline has the same *shape* as the original. A
modern renderer that transforms in floating point and then adds wobble as a
post-process gets the character wrong, because the artefacts are not noise laid
on top of a clean image — they are the consequence of specific arithmetic. So the
arithmetic is reproduced, and the artefacts follow.

Everything below is individually toggleable via `r_psx_*` settings. The default
for every one of them is faithful.

---

## 1. Vertex wobble

**Cause.** The Geometry Transformation Engine outputs screen coordinates as
signed 16-bit integers with no fractional component. A vertex that should land at
x = 100.7 lands at x = 100. As the camera or object moves, vertices jump between
whole pixels rather than sliding smoothly, and because each vertex of a polygon
rounds independently, the polygon visibly shears and swims.

**Second cause, often missed.** The perspective divide is not a divide. The GTE
normalises the divisor, looks up a seed in a 257-entry reciprocal table, runs two
Newton–Raphson refinement steps, and multiplies. The result is *close to* correct
but not correct, and the error varies with depth. This means the wobble is not a
clean "round to nearest pixel" — it has its own texture.

**Implementation.** `src/psx/gte.c`. The reciprocal table is generated from its
defining formula at init, the divide follows the hardware's exact sequence, and
`gte_push_sxy()` truncates to `s16` with the hardware's saturation limits of
[-1024, 1023]. Vertex positions are never widened to float anywhere in the
pipeline.

**Setting.** `r_psx_vertex_snap` (default on). Off transforms at full precision.

---

## 2. Affine texture mapping

**Cause.** The GPU interpolates U and V linearly in *screen space*. It has no W
term and does no perspective correction. On a polygon that is small or close to
screen-parallel this is invisible; on a large floor or wall viewed at a shallow
angle the texture visibly warps and slides, and the distortion changes as the
camera moves.

**Implementation.** `psx_prim` carries `u8` UVs and no W. The backend must
interpolate them affinely. This is a *requirement on the backend*, not something
the frontend can enforce — a GPU backend has to explicitly avoid the perspective
correction it would otherwise get for free, either by writing the interpolation
by hand or by feeding it `noperspective` varyings.

**Setting.** `r_psx_affine_uv` (default on).

**Note.** Because the original had no perspective correction, the artists and
level designers worked around it by subdividing large surfaces. Turning this off
therefore does not just "fix" the game — it reveals geometry that was tessellated
for a problem that no longer exists.

---

## 3. Ordering-table sorting, and the absence of a depth buffer

**Cause.** There is no Z-buffer. GPU packets are chained into an ordering table
and its buckets are drawn far to near. The assignment is emitter-specific:
ordinary fallback geometry can use `AVSZ3`/`AVSZ4`, but authored world nodes use
SortData buckets and each model/deferred object is one private packet chain.
Retail's regional sorter relates those chains with camera-relative world bounds;
projected-origin depth is the secondary order for its Quick list. Sorting is per
object or polygon, never per pixel.

**Visible consequences that must be preserved:**

- Intersecting polygons cannot interleave; one wins entirely, and which one wins
  can flip between frames as the average depths cross. This is the classic
  PlayStation "polygon pop".
- A long polygon sorts by its average depth, so it can incorrectly occlude, or be
  occluded by, something that overlaps only one end of it.
- Within a single bucket, order is defined by insertion. The hardware built the
  list by prepending, so the **last primitive added is drawn first**.

**Implementation.** `src/psx/gpu.c`. `psx_ot_add()` prepends to the bucket's
intrusive linked list and `psx_ot_walk()` iterates bucket 0 upward, with depth
inverted on insertion so the result is far to near. When the primitive pool fills, geometry is dropped
rather than the pool being grown — the original did the same, and reported it.

**World order.** The console does not sort the world by depth at all: the draw
order is authored in `SortData` and the table only carries it (see
`src/formats/sortdata.h`). The viewport's PrimaryColl cell selects the stream
by carrying its exact byte offset at node `+28`; this is now the normal renderer
path. Opcode 1 projects its named Scene/Points group and chooses its bitstream
arm from whether the rectangle has non-zero area. That predicate is part of the
codec: forcing either arm globally desynchronises every later node. Together
these rules remove missing rooms and the port-created z-fighting where a light,
button or other near-coplanar wall detail shared a coarse depth bucket with its
backing face and their node-index order flipped at a slab boundary.

The camera area's full-view record is seeded at console bucket 45, then the
SortData stream itself starts at 43. Opcode 1 is a screen-region change (the
decoder's `ENTITY` name is historical): it projects its Scene/Points marker,
clips that rectangle to the parent named by `f3`, registers `f4` as a seven-bit
draw area at the current authored bucket, and moves both the GTE origin and GPU
draw environment into that local rectangle. Values below a parent edge clamp
to its minimum and values above it clamp to its maximum. A hidden marker passes
a literal zero rectangle and must take the false bitstream arm; a rectangle
collapsed on either axis is normalised the same way. Treating either as visible
desynchronises every record that follows. Draw-environment restore
packets live inside the OT, so they execute in authored order, and world 2D
culling uses the current region's extent rather than the full viewport.

Deferred wall nodes, models and effects build private chains against those
areas. At retail's `0x80046E14` drain, area 1 concatenates unsorted at slice
bucket 46, area 2 is sorted at slice bucket 1, and each ordinary live area's
newest-first Standard list (at most 32 records) becomes a dependency
graph from its AABBs and the camera position. Strictly overlapping boxes are
split at the midpoint of their shallowest overlap before comparison, and an
already-established edge suppresses its reciprocal. Quick records (at most
128, including particle/beam points and flagged models) depend on Standard
records but not on one another; currently unblocked Quick records are stable-
sorted by signed projected-origin depth. The drain alternates those ready Quick
runs with a cyclic Standard selection, breaks a true cycle at the row with the
fewest dependencies, and `CatPrim`s each result as one atomic chain onto the
authored bucket. An area not registered by the current viewport is stale and
its objects are culled rather than globally depth-sorted through a different
room.

The area's projection selector is deliberately stateful. A negative selector
returns without changing the GTE; area 0 selects the global extent, area 1 the
current viewport, and an ordinary live area uses `viewport centre - region
minimum`. The view-weapon entity is explicitly area 1 (`0x8004EE58`) before it
reaches the normal model selector at `0x8006BEB0`. Treating `-1` as a full-view
reset let the gun inherit a particle or projectile's portal-local origin and
made it move or clip when the visible area set changed.

`--depth-sort` retains the old approximation for diagnostics. A bucket there is
a depth slab, about a hundred units wide at `PSX_OT_SUBDIV` 8 and the default
sort range, and insertion order still decides ties inside it.

On the diagnostic fallback, `psx_ot_add_depth()` takes the depth the bucket quantised away and orders the
slab by it, so the order inside a slab continues the order between slabs and
there is no boundary left to swap across. The two console artifacts above are
untouched: a primitive is still sorted by ONE depth for the whole of it, so long
polygons still sort by their average and intersecting polygons still pop.
Area-routed emitters instead supply the one batch depth retail assigns to the
whole private chain as well as its point or absolute bounds; only the Quick
merge orders directly by that depth.
`psx_ot_add()` and `psx_ot_add_bucket()` do not, and behave exactly as before.

**Setting.** `r_psx_ot_sort` (default on). Off enables a depth buffer.

---

## 4. 15-bit colour and ordered dithering

**Cause.** The framebuffer is RGB555. Gouraud shading and blending are computed
at higher precision and then truncated to 5 bits per channel, which would band
badly, so the GPU adds a 4x4 ordered dither pattern before truncating.

**Implementation.** `psx_dither_matrix` in `src/psx/gpu.c`, applied per channel
as a function of screen position. Bit 15 of each pixel is the mask bit, not
alpha.

**Setting.** `r_psx_dither` (default on), `r_psx_15bit` (default on).

### 4a. Channel order — red lives in the LOW bits

Calling the format "RGB555" is how this went wrong once already, so state it as
bits. A PSX framebuffer halfword is

```
 15   14 .. 10   9 .. 5   4 .. 0
 STP      B         G        R
```

Red occupies bits 0–4. `psx_rgb555()` in `src/psx/gpu.h` builds that layout and
`unpack555()` in `src/render/raster.c` reads it back; every palette on the disc
is stored the same way, which is why `src/formats/vram.h` calls CLUT entries
BGR555 rather than RGB555. Nothing inside the port ever holds a pixel with red
in the high bits.

**Where it can go wrong is the hand-off to the host**, and only there. The
client uploads the finished front buffer to an SDL texture with a plain memcpy,
so the texture's format has to name the console's layout — SDL orders its format
names most-significant-channel-first, which makes that `SDL_PIXELFORMAT_XBGR1555`.
`XRGB1555` is the mirror image and was what shipped: it produced a complete,
sharp, correctly-lit picture with **red and blue exchanged in every pixel**.
Quake II's rust-brown rock came out slate blue and its violet sky came out
crimson, which reads like a palette fault or a PAL/NTSC problem and sends the
search a long way from the one line responsible.

Two things make this defect worth its own section:

- **It is invisible to this project's own comparison workflow.** `--shot` writes
  its PPM off the front buffer through `unpack555()`, *before* SDL is handed
  anything. Captures stayed correct for as long as the bug existed, so a
  before/after screenshot could not show it and the disagreement only appeared
  when someone looked at the window and the capture side by side. A defect
  visible in the window and never in a capture is, by construction, in the
  presentation format.
- **Region has nothing to do with it.** PAL and NTSC differ in line count and
  field rate — 256 lines at 50 Hz against 240 at 60 — and in nothing about
  colour. The GPU packs the same halfword either way. "The PAL disc has
  different colours" is not a hypothesis this hardware supports.

**Guard.** `src/client/main.c` asks SDL what it would pack pure red into,
immediately after creating the texture, and compares it against
`psx_rgb555(255, 0, 0)`. They must both be `0x001F`. A mismatch warns with both
values rather than letting the picture argue for itself.

---

## 5. Semi-transparency

The GPU has exactly four blend modes, selected per primitive:

| Mode | Operation | Typical use |
|---|---|---|
| 0 | `B/2 + F/2` | glass, water, ghosts |
| 1 | `B + F` | muzzle flashes, explosions |
| 2 | `B - F` | shadow, smoke darkening |
| 3 | `B + F/4` | faint additive glow |

There is no per-pixel alpha and no arbitrary blend factor. `psx_blend` in
`src/psx/gpu.h` enumerates exactly these four and no more.

---

## 6. Texture pages, CLUTs and the texture window

Textures live in the same 1024x512 16-bit VRAM as the framebuffers, addressed as
256x256 texel pages in 4-, 8- or 16-bit mode. 4- and 8-bit modes index a CLUT
stored elsewhere in VRAM.

Consequences to preserve:

- UVs are `u8`, so they **wrap within the page**. A UV of 260 is 4, not clamped.
- The texture window register masks UVs, which is how the original tiled small
  textures without duplicating them in VRAM.
- Because VRAM is shared, a texture can overlap the framebuffer. This was used
  deliberately for effects and must not be "fixed" by giving textures their own
  storage. `psx_vram` in `src/psx/gpu.h` is therefore one flat array, not a
  texture atlas.

---

## 6a. The framebuffer as a source

One primitive reads pixels back: `PSX_PRIM_MOVE`, the GPU's VRAM-to-VRAM
rectangle copy (GP0 0x80, libgpu's `DR_MOVE`). The underwater effect is built
entirely out of it — it displaces strips of the frame that has already been
drawn rather than distorting any geometry — so a backend has to support it or
the effect simply does not appear.

Three rules a backend must not "improve":

- **No clip, no offset.** A copy takes absolute coordinates. The drawing area
  and drawing offset are rasteriser state and do not apply, which is why the
  effect computes its own buffer-relative origin.
- **Overlap is the normal case, not an edge case.** Source and destination
  overlap on every single copy the effect makes, and the result must be as if
  the source were read whole before the destination was written.
- **Nothing else in the pipeline touches it.** No dither, no blend, no mask
  test beyond the hardware's own — the halfwords are copied verbatim.

A hardware backend implementing this as a render-to-texture pass must therefore
resolve the framebuffer before the copy and not batch it with the surrounding
primitives, because its input is the output of everything that preceded it in
the ordering table.

---

## 7. Near-plane behaviour

The GPU does not clip. Geometry crossing the projection plane produces garbage
coordinates, so the original either rejected such polygons outright (watching the
GTE's overflow flags) or subdivided them in software beforehand.

This matters because it is *visible*: geometry very close to the camera vanishes
rather than being clipped smoothly. `gte_project_point()` returns the flag state
so the caller can make the same decision the original made.

**Setting.** `r_psx_near_reject` (default on).

---

## 8. Timing

PAL builds ran the game logic off the 50 Hz vertical blank; NTSC builds off 60 Hz.
For many PAL ports of this era that made the game genuinely slower, and the PAL
framebuffer was 256x256 rather than 256x240.

`q2_build_tick_rate()` reports the build's native rate. The renderer may
interpolate to any display rate, but the *simulation* steps at the build's native
rate by default so that physics, animation and AI behave as they did.

**Setting.** `g_tick_rate` (default: the build's native rate).

---

## Verifying fidelity

Claims here are testable, and should be tested rather than eyeballed:

1. **GTE conformance.** Feed known vectors through `gte_rtps`/`gte_rtpt` and
   compare `SXY`, `SZ`, `MAC` and `FLAG` against values captured from hardware or
   from a reference GTE implementation. Bit-exact is the bar; "close" means the
   wobble is wrong.
2. **Divide conformance.** `gte_divide()` is exposed specifically so it can be
   swept across its whole input domain and diffed against a reference.
3. **OT ordering.** Assert that primitives added to one bucket emerge in reverse
   insertion order, that buckets emerge far-to-near, and that a bucket whose
   primitives carry a depth key emerges in key order with equal keys still
   falling back on reverse insertion. `tests/test_gte.c`.
4. **Frame comparison.** The end goal: capture the same scene from an emulator
   running the original and from this port, and diff. Any pixel difference is a
   bug in one of the above.

Items 1–3 are unit-testable today and should gate merges. Item 4 needs the
renderer, and is the acceptance test for the project as a whole.
