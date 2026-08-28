/*
 * gpu.h — the PlayStation GPU primitive model and ordering table.
 *
 * The game does not call a modern graphics API. It builds a linked list of GPU
 * primitives — exactly the structures libgpu used — and hands it to a backend.
 * Keeping that indirection is what makes faithful rendering possible: the
 * backend sees the same primitive stream the real hardware saw, so it can
 * reproduce the hardware's rasterisation rules rather than approximating them.
 *
 * The four properties that define the PlayStation look, and where they live:
 *
 *   1. Integer vertex positions      — produced by gte.c, stored here as s16.
 *   2. Affine (non-perspective) UVs  — u8 texture coordinates with no W term;
 *                                      the backend must NOT perspective-correct.
 *   3. No depth buffer               — depth is the ordering table bucket index;
 *                                      primitives draw strictly back-to-front.
 *   4. 15-bit colour with dithering  — the backend's final resolve step.
 */
#ifndef Q2PSX_GPU_H
#define Q2PSX_GPU_H

#include "fixed.h"
#include "q2psx.h"

/* ------------------------------------------------------------------------- */
/* Primitive kinds                                                            */
/* ------------------------------------------------------------------------- */
typedef enum psx_prim_kind {
    PSX_PRIM_F3 = 0,   /* flat triangle                     */
    PSX_PRIM_FT3,      /* flat textured triangle            */
    PSX_PRIM_G3,       /* gouraud triangle                  */
    PSX_PRIM_GT3,      /* gouraud textured triangle         */
    PSX_PRIM_F4,       /* flat quad                         */
    PSX_PRIM_FT4,      /* flat textured quad                */
    PSX_PRIM_G4,       /* gouraud quad                      */
    PSX_PRIM_GT4,      /* gouraud textured quad             */
    PSX_PRIM_LINE_F2,
    PSX_PRIM_LINE_G2,
    PSX_PRIM_SPRT,     /* screen-space sprite               */
    PSX_PRIM_TILE,     /* untextured rectangle              */
    PSX_PRIM_TPAGE,    /* draw-mode change                  */
    PSX_PRIM_MOVE,     /* VRAM -> VRAM rectangle copy       */
    PSX_PRIM_DRAW_ENV, /* clipped-region draw environment   */
    PSX_PRIM_KIND_COUNT
} psx_prim_kind;

/* ------------------------------------------------------------------------- */
/* Semi-transparency modes. The PSX has exactly four; B is the framebuffer     */
/* value and F the incoming fragment.                                         */
/* ------------------------------------------------------------------------- */
typedef enum psx_blend {
    PSX_BLEND_HALF   = 0,   /* B/2 + F/2 — the usual "glass" look   */
    PSX_BLEND_ADD    = 1,   /* B   + F   — muzzle flashes, fire     */
    PSX_BLEND_SUB    = 2,   /* B   - F   — shadow, smoke            */
    PSX_BLEND_QUARTER= 3    /* B   + F/4 — faint additive           */
} psx_blend;

/* Texture colour depth within a texture page. */
typedef enum psx_tex_bpp {
    PSX_TEX_4BIT  = 0,
    PSX_TEX_8BIT  = 1,
    PSX_TEX_16BIT = 2
} psx_tex_bpp;

/* ------------------------------------------------------------------------- */
/* Common primitive fields                                                    */
/* ------------------------------------------------------------------------- */
typedef struct psx_xy   { s16 x, y; } psx_xy;    /* integer screen pixels     */
typedef struct psx_uv   { u8  u, v; } psx_uv;    /* texel within a 256x256 page */
typedef struct psx_rgb  { u8  r, g, b, pad; } psx_rgb;

/*
 * One primitive. This is a tagged union rather than the original's variable-size
 * packets: the storage cost is irrelevant on a PC and it makes the backend's job
 * a straight switch. Field meanings match the hardware exactly.
 *
 * `tpage` packs the texture page as the hardware does:
 *   bits 0-3  : page X base (page * 64 halfwords)
 *   bit  4    : page Y base (0 or 256)
 *   bits 5-6  : semi-transparency mode
 *   bits 7-8  : colour depth
 * `clut` packs the palette location:
 *   bits 0-5  : X / 16
 *   bits 6-14 : Y
 */
typedef struct psx_prim {
    psx_prim_kind kind;

    psx_xy   xy[4];        /* 3 for triangles, 4 for quads, 2 for lines  */
    psx_uv   uv[4];
    psx_rgb  rgb[4];       /* [0] only for flat primitives               */

    u16      tpage;
    u16      clut;

    bool     semi_transparent;  /* the primitive's ABE bit                */
    bool     textured_blend;    /* modulate texture by rgb (libgpu "raw") */

    /*
     * Which corner order this quad's four slots are in.
     *
     * false — PERIMETER, the default, and what both MapMod and the model bank
     *         store: the four corners walk round the quad. The console converts
     *         that to Z order by exchanging corners 2 and 3 on the way into the
     *         packet, so the hardware's split expressed in THESE indices is
     *         (0,1,3)+(1,2,3) — see the diagonal note in render/raster.c.
     * true  — libgpu Z ORDER, which is what the hardware's own POLY_x4 packets
     *         use: 0 1 / 2 3 across two rows, splitting (0,1,2)+(1,3,2).
     *
     * The flare pass builds real console packets and therefore hands over Z
     * order; splitting those on the perimeter rule drew half of every sector as
     * a black sliver, turning the 12-gon glow into a six-spoke pinwheel and the
     * ghosts into bowties. Rather than reorder the packets — which would stop
     * them matching 0x800754D8..0x80075554 field for field — the backend is
     * told which convention it is looking at.
     */
    bool     quad_zorder;

    u16      otz;          /* ordering-table bucket this landed in       */

    /*
     * The sub-bucket depth this primitive was ordered by, larger being farther,
     * or PSX_OT_KEY_NONE for a packet whose place is structural. See
     * psx_ot_add_depth.
     */
    u32      sort_key;
} psx_prim;

/* ------------------------------------------------------------------------- */
/* PSX_PRIM_MOVE — the one primitive that reads the framebuffer               */
/*                                                                            */
/* libgpu's `DR_MOVE`: GP0(0x80), copy a rectangle of VRAM to somewhere else  */
/* in VRAM. The game builds these by hand rather than through `SetDrawMove` — */
/* the pool at `0x80062D3C` pre-fills each packet with tag `0x05000000`,      */
/* `code[0] = 0x01000000` (flush the texture cache) and `code[1] = 0x80000000`*/
/* (the copy), leaving only the two coordinate pairs and the size to be       */
/* written per use. That pool is called **"Water Moves"** in the executable's  */
/* own descriptor string at `0x800ACF7C`, which is what names the effect.     */
/*                                                                            */
/* The fields it uses, matching `code[2]`, `code[3]` and `code[4]`:           */
/*                                                                            */
/*     xy[0]   source, in VRAM coordinates                                    */
/*     xy[1]   destination                                                    */
/*     xy[2]   width and height, as x and y                                   */
/*                                                                            */
/* TWO RULES THAT ARE NOT THE ONES A POLYGON FOLLOWS, and both matter:        */
/*                                                                            */
/*   - The copy is NOT clipped by the drawing area and NOT displaced by the   */
/*     drawing offset. Those are rasteriser state; this is a blit, and its    */
/*     coordinates are absolute. The water warp relies on it: it computes its */
/*     own `view.x + buffer origin` and would land twice as far off if the    */
/*     draw env's offset were applied on top.                                 */
/*   - Overlapping source and destination are allowed and are the normal      */
/*     case, so the copy must behave as though the source were read whole     */
/*     before the destination is written.                                     */
/* ------------------------------------------------------------------------- */
#define PSX_MOVE_SRC  0
#define PSX_MOVE_DST  1
#define PSX_MOVE_SIZE 2

/* ------------------------------------------------------------------------- */
/* Ordering table                                                             */
/*                                                                            */
/* The PlayStation has no depth buffer. Instead, each emitter links packets to */
/* an ordering-table bucket, and the buckets are walked from far to near. Two  */
/* consequences the port must preserve:                                       */
/*                                                                            */
/*   - There is no per-pixel visibility. Fallback polygons may use average Z,  */
/*     authored world runs use SortData buckets, and retail's regional sorter  */
/*     keeps each model or deferred object chain atomic. Intersections can pop */
/*     at whichever granularity that emitter supplies.                        */
/*   - Within one bucket, order is defined by insertion (the hardware walks a  */
/*     singly-linked list built by prepending, so *last in draws first*).      */
/*                                                                            */
/* WHICH WAY THE TABLE IS WALKED — read out of the executable, not assumed.    */
/* The game builds its table with `ClearOTag` (`0x800837C0`, called at         */
/* `0x80018398`), whose loop writes into each entry the address of the *next*  */
/* one, and hands `DrawOTag` the address of entry 0. So the hardware walks     */
/* bucket 0 first and every later bucket paints on top of it. Three separate   */
/* things in the frame depend on that direction and would be nonsense reversed:*/
/* the full-screen background clear sits at OT[1] and must precede all four    */
/* viewports; each viewport's draw-env packet sits at slice bucket 1 and must  */
/* precede that viewport's geometry; and the damage flash sits at slice bucket */
/* 50 and must land on top of it.                                             */
/*                                                                            */
/* A HIGHER BUCKET IS THEREFORE NEARER. To keep that from leaking into every   */
/* emitter, `otz` here is a DEPTH — larger is farther, exactly as it arrives   */
/* from the GTE — and the table inverts it. `psx_ot_add_bucket` is the escape  */
/* hatch for the few packets that know which bucket they want.                 */
/* ------------------------------------------------------------------------- */
/* The retail world sorter addresses draw areas with a seven-bit name. */
#define PSX_OT_AREA_COUNT 128

/*
 * A private primitive chain waiting for retail's screen-area drain.
 *
 * The console record is 20 bytes and points at external point/bounds storage.
 * The port owns its memory, so it keeps copies of the spatial data and camera
 * alongside the equivalent fields. `head`/`tail` are indices into psx_ot's
 * primitive pool; an empty batch has both set to -1.
 */
typedef struct psx_ot_batch {
    s32 head;
    s32 tail;
    s16 order;          /* record +4: signed depth used by the Quick merge */
    u8  area;           /* seven-bit screen-area name                      */
    u8  point;          /* record +6 bit 0: point rather than an AABB       */
    u8  quick;          /* area +12 list; otherwise the Standard +8 list   */
    u8  pad[3];
    s32 spatial[6];     /* point[3], or min[3] followed by max[3]           */
    s32 camera[3];      /* retail reads the current camera at 0x800B2B24   */
} psx_ot_batch;

/*
 * One of 0x80065804's 20-byte screen-change records, reduced to the fields
 * which affect rendering.  Coordinates are relative to the viewport.  `add`
 * is the inherited draw offset (normally the water/shake displacement), while
 * min/max are the projected region in the unshifted viewport.
 */
typedef struct psx_ot_area_screen {
    s16 min_x, min_y;
    s16 max_x, max_y;
    s16 add_x, add_y;
} psx_ot_area_screen;

/* The executable allocates records 0..95; 3..95 are the ordinary area walk. */
#define PSX_OT_AREA_RETAIL_COUNT 96

typedef struct psx_ot {
    psx_prim *prims;       /* flat pool of all primitives this frame     */
    u32       prim_count;
    u32       prim_capacity;

    s32      *bucket_head; /* index of first prim in bucket, -1 if empty */
    s32      *next;        /* intrusive singly-linked list, parallel to prims */
    u32       bucket_count;

    /*
     * The slice of the table the current viewport owns.
     *
     * This is not a convenience: it is the hardware model. The console's world
     * renderer is handed a base pointer (the per-viewport slice address parked
     * in 0x800B2D60) and links primitives relative to it, which is how one
     * table and one DrawOTag produce four independently clipped viewports. A
     * window of zero length means the whole table.
     */
    u32       window_base;
    u32       window_len;

    /* SortData buckets are relative to the WHOLE 51-entry console slice,
     * while the ordinary geometry window begins two entries into it. */
    u32       authored_base;
    u32       authored_len;

    /* Named insertion points for the current viewport. */
    u32       area_bucket[PSX_OT_AREA_COUNT];
    u8        area_valid[PSX_OT_AREA_COUNT];
    psx_ot_area_screen area_screen[PSX_OT_AREA_COUNT];
    u8        area_screen_valid[PSX_OT_AREA_COUNT];
    s16       area_view_ofs_x, area_view_ofs_y;
    bool      area_routing;

    /* Private chains drained through the named insertion points. */
    psx_ot_batch *batch;
    u32           batch_count;
    u32           batch_capacity;
} psx_ot;

/* Constrain subsequent psx_ot_add calls to [base, base+len). len == 0 releases
 * the window. psx_ot_clear releases it too, so a window never outlives a frame
 * by accident. */
void psx_ot_set_window(psx_ot *ot, u32 base, u32 len);

/* Install the complete console slice which authored bucket numbers address.
 * `psx_ot_set_window` may name a smaller depth-sortable subset of the same
 * slice.  Both arguments are in console buckets. */
void psx_ot_set_authored_window(psx_ot *ot, u32 base, u32 len);
u32  psx_ot_authored_bucket(const psx_ot *ot, u32 console_bucket);

/*
 * Register and resolve the current viewport's named insertion points.
 * `console_bucket` is relative to the viewport slice and is scaled by
 * PSX_OT_SUBDIV. The map can be cleared without discarding primitives, which
 * is required when a split-screen frame advances to its next viewport.
 */
void psx_ot_area_clear(psx_ot *ot);
void psx_ot_area_prepare(psx_ot *ot, s16 view_w, s16 view_h,
                         s16 add_x, s16 add_y,
                         s16 view_ofs_x, s16 view_ofs_y);
bool psx_ot_area_register(psx_ot *ot, u32 area, u32 console_bucket);
bool psx_ot_area_register_screen(psx_ot *ot, u32 area, u32 console_bucket,
                                 const psx_ot_area_screen *screen);
bool psx_ot_area_bucket(const psx_ot *ot, u32 area, u32 *bucket);
bool psx_ot_area_get_screen(const psx_ot *ot, u32 area,
                            psx_ot_area_screen *screen);
bool psx_ot_area_projection(const psx_ot *ot, s32 area,
                            s32 *centre_x, s32 *centre_y);
bool psx_ot_area_active(const psx_ot *ot);

/*
 * Retail's per-area painter sorter (0x80046E14 / 0x80047080).
 *
 * Standard batches participate in the bounds dependency graph (maximum 32 per
 * area). Quick batches depend on Standard batches but not on one another
 * (maximum 128), and are stably merged by signed `order`. Both begin calls copy
 * their spatial data, so stack-backed mover bounds are safe. A batch is one
 * atomic GPU chain: use psx_ot_batch_add for a new primitive or
 * psx_ot_batch_link_prim for a primitive already built with psx_ot_alloc.
 */
#define PSX_OT_BATCH_INVALID (-1)
s32 psx_ot_batch_begin_box(psx_ot *ot, u32 area, bool quick, s16 order,
                           const s32 min[3], const s32 max[3],
                           const s32 camera[3]);
s32 psx_ot_batch_begin_point(psx_ot *ot, u32 area, bool quick, s16 order,
                             const s32 point[3], const s32 camera[3]);
psx_prim *psx_ot_batch_add(psx_ot *ot, s32 batch);
bool psx_ot_batch_link_prim(psx_ot *ot, s32 batch, psx_prim *prim);
void psx_ot_flush_batches(psx_ot *ot);

/* How many buckets an emitter may address right now — the window if one is
 * installed, the whole table otherwise. */
u32  psx_ot_bucket_span(const psx_ot *ot);

/*
 * Map a GTE depth into the slice the current window owns, scaled by the
 * viewport's far distance.
 *
 * The port used to shift the depth right by a fixed amount, which was fine
 * against a table of thousands of buckets and is not fine against the console's
 * real one: a viewport slice is 51 entries, so a fixed shift saturates
 * everything past a few hundred units onto the slice's far end and the sort
 * stops distinguishing anything — including a weapon held inches from the eye
 * from the wall behind it.
 *
 * `far_z` is the viewport's own far distance (view+264, parked at 0x800B2CCC),
 * so the scale is the console's number rather than a tuning constant. A far_z
 * of zero falls back to the old shift, which is what the offline tools that
 * build their own camera still get.
 */
/*
 * HOW MANY REAL BUCKETS ONE CONSOLE BUCKET IS WORTH.
 *
 * The console's table is the hardware's: 217 entries, a 51-entry slice per
 * viewport, and everything in screen.h is that table's numbering. Nothing about
 * those numbers is a rendering decision — they are an address space — and the
 * port is not obliged to have exactly as many.
 *
 * It matters because the port DOES sort by depth where the console sorts from
 * SortData, and 51 buckets is nowhere near enough to do that with. A slice
 * spanning the deepest shipped view (35,091 units on BASE0) at 51 buckets is
 * 700 units per bucket: a monster and the wall behind it fall in the same one
 * and their order becomes arbitrary. Squeezing the range down to 6,400 instead
 * — which is what the port did — buys near-field resolution by collapsing
 * everything beyond into a single bucket, and that collapse is the reported far
 * distance fault. Neither end of that trade is affordable at 51 buckets.
 *
 * So the table is subdivided. Every structural bucket index is multiplied by
 * this on the way in — psx_ot_set_window and psx_ot_add_bucket are the only two
 * doors, so callers keep naming the console's numbers and keep their relative
 * order exactly — and the depth mapping gets the whole subdivided slice to
 * spread across. At 8, a slice is 408 buckets and the full 36,864-unit range
 * resolves to about 100 units per bucket, finer than the 128 the old collapsed
 * mapping managed over its 6,400.
 */
#define PSX_OT_SUBDIV 8

/*
 * The ends of a slice belong to packets whose bucket is STRUCTURAL, and depth
 * must not be allowed to reach them.
 *
 * A viewport slice is 51 entries with the draw env at 1, the water plane near
 * 49 and the damage flash at 50 (screen.h). The depth mapping used to run over
 * the whole slice, so a surface at depth 0 landed on the flash's bucket and one
 * past the far distance landed on the env's — which is how widening the sort
 * range put world geometry over the status bar. Two console buckets are held
 * back at each end so the depth range and the structural range cannot meet.
 */
#define PSX_OT_DEPTH_RESERVE (2 * PSX_OT_SUBDIV)

u32  q2_ot_bucket_for_depth(const psx_ot *ot, u32 depth, s32 far_z);

q2_result psx_ot_init(psx_ot *ot, u32 bucket_count, u32 prim_capacity);
void      psx_ot_free(psx_ot *ot);
void      psx_ot_clear(psx_ot *ot);

/*
 * Add a primitive at depth `otz` — larger is farther, so it lands in a LOWER
 * bucket and is drawn earlier. The depth is clamped to the addressable span and
 * mapped into the window if one is installed. Returns NULL if the pool is full;
 * the original printed "Out of ScreenChanges on frame %d" and dropped geometry,
 * and we surface the same condition rather than growing silently.
 */
psx_prim *psx_ot_add(psx_ot *ot, u16 otz);

/*
 * WHY A BUCKET IS NOT ENOUGH, AND WHAT `key` IS FOR.
 *
 * A bucket is a depth SLAB — about 100 world units wide at PSX_OT_SUBDIV 8 and
 * the default sort range — and everything inside one slab is ordered by
 * insertion, which for the world means node index. Node index has nothing to do
 * with depth, so a light panel, a shootable button or any other detail surface
 * sitting a few units off the wall it is mounted on shares that wall's bucket
 * and the two are ordered arbitrarily. That order holds while both stay in the
 * slab and is overruled the moment a slab boundary happens to fall between
 * them, so walking towards such a surface makes it swap in and out — the
 * reported z-fighting. It is not a decal system misbehaving; it is the sort
 * being flat inside a bucket and stepping between buckets.
 *
 * `key` carries the depth the bucket quantised away, so the order inside a slab
 * continues the order between slabs instead of contradicting it. Larger is
 * farther, matching `otz`; equal keys keep the hardware's prepend rule, so a
 * caller that passes the same key for every primitive — or PSX_OT_KEY_NONE,
 * which asks for a plain prepend and is what `psx_ot_add` and
 * `psx_ot_add_bucket` use — behaves exactly as it did before this existed.
 *
 * A PSX_OT_KEY_NONE primitive is also a BARRIER: keyed primitives never sort
 * past one. Structural packets live in reserved buckets of their own
 * (PSX_OT_DEPTH_RESERVE) so this should not arise, but if one ever shares a
 * bucket with geometry its position stays where the prepend rule put it rather
 * than being decided by a depth it never supplied.
 *
 * This does not make the sort exact and is not meant to. A primitive is still
 * ordered by ONE depth for the whole of it, so long polygons still sort by
 * their average and can still incorrectly occlude, and intersecting polygons
 * still pop. Those are the console's artifacts and they stay. What goes is the
 * quantisation on top of them, which is the port's own.
 */
#define PSX_OT_KEY_NONE 0xFFFFFFFFu

psx_prim *psx_ot_add_depth(psx_ot *ot, u16 otz, u32 key);

/* Use a live named insertion point, falling back to ordinary depth when the
 * current viewport did not register `area`. */
psx_prim *psx_ot_add_area_depth(psx_ot *ot, u32 area, u16 otz, u32 key);

/* Add to an already-resolved absolute bucket while retaining a sort key. */
psx_prim *psx_ot_add_bucket_depth(psx_ot *ot, u32 bucket, u16 otz, u32 key);

/*
 * Add a primitive at an ABSOLUTE bucket index, ignoring the window and the
 * depth inversion. This is what a packet whose place in the table is structural
 * rather than depth-derived uses — the draw envs, the damage flash and the
 * performance meter all name their bucket outright.
 */
psx_prim *psx_ot_add_bucket(psx_ot *ot, u32 bucket);

/*
 * Allocation and linking, split apart — for a caller that must BUILD its
 * primitives in one order and DRAW them in another. The model path is the one
 * that needs it: a face can only be resolved while its own part owns the
 * scratch window, but the order it draws in comes from the model's own table
 * (model.h, block A). See psx_ot_alloc in gpu.c.
 *
 * A primitive allocated and never linked never draws.
 */
psx_prim *psx_ot_alloc(psx_ot *ot);
bool      psx_ot_link_prim(psx_ot *ot, psx_prim *prim, u32 bucket, u32 key);

/* Map a depth to the absolute bucket `psx_ot_add` would choose. */
u32 psx_ot_depth_bucket(const psx_ot *ot, u32 otz);

/* Iterate the table in DrawOTag order: bucket 0 first, so far geometry is drawn
 * before near geometry, and within a bucket the farthest `sort_key` first,
 * falling back to the most recently added where the keys are equal or absent.
 * `fn` is called once per primitive. */
typedef void (*psx_ot_visit_fn)(const psx_prim *prim, void *user);
void psx_ot_walk(psx_ot *ot, psx_ot_visit_fn fn, void *user);

/* ------------------------------------------------------------------------- */
/* Draw-mode helpers                                                          */
/* ------------------------------------------------------------------------- */
Q2PSX_INLINE u16 psx_make_tpage(int page_x, int page_y, psx_blend blend, psx_tex_bpp bpp)
{
    return (u16)(((page_x & 0x0F))
               | ((page_y & 0x01) << 4)
               | (((int)blend & 0x03) << 5)
               | (((int)bpp   & 0x03) << 7));
}

Q2PSX_INLINE u16 psx_make_clut(int x, int y)
{
    return (u16)(((x >> 4) & 0x3F) | ((y & 0x1FF) << 6));
}

/* ------------------------------------------------------------------------- */
/* VRAM                                                                       */
/*                                                                            */
/* 1024x512 16-bit words, exactly as on hardware. Textures, palettes and both  */
/* framebuffers all live in here and can overlap — the original relied on that */
/* layout, so the port keeps it rather than using separate texture objects.    */
/* ------------------------------------------------------------------------- */
#define PSX_VRAM_WIDTH   1024
#define PSX_VRAM_HEIGHT  512

typedef struct psx_vram {
    u16 px[PSX_VRAM_HEIGHT][PSX_VRAM_WIDTH];
} psx_vram;

/* RGB555 with the mask bit in bit 15. */
Q2PSX_INLINE u16 psx_rgb555(u8 r, u8 g, u8 b)
{
    return (u16)(((r >> 3) & 0x1F) | (((g >> 3) & 0x1F) << 5) | (((b >> 3) & 0x1F) << 10));
}

/*
 * The GPU's ordered dither matrix, applied when 24-bit colour is reduced to the
 * 15-bit framebuffer. Values are added to each channel before truncation. This
 * 4x4 pattern is a large part of why PlayStation gradients look the way they do,
 * so the backend applies it by default.
 */
extern const s8 psx_dither_matrix[4][4];

Q2PSX_INLINE u8 psx_dither_channel(u8 value, int x, int y)
{
    s32 v = (s32)value + psx_dither_matrix[y & 3][x & 3];
    return (u8)q2_clamp_s32(v, 0, 255);
}

#endif /* Q2PSX_GPU_H */
