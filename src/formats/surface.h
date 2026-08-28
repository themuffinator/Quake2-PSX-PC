/*
 * surface.h — what a surface IS: its blend mode, its draw policy, and the
 * Scene-node flags that decide whether and how it is drawn at all.
 *
 * Everything here was read out of the executable and then checked against the
 * disc. Three separate mechanisms meet in this file, and they were previously
 * documented as three unknowns:
 *
 *   1. The blend selector -> {primitive code, semi-transparency mode} pair.
 *   2. `Scene.flags08`, which was "bitfield, meaning unknown" (open question
 *      #12) and is in fact four independent fields.
 *   3. The subdivision policy those flags select, which is what controls the
 *      affine texture warp on near geometry.
 *
 * ---------------------------------------------------------------------------
 * 1. Blend selectors — the two tables
 * ---------------------------------------------------------------------------
 * A drawn surface picks its transparency from a selector, 0..7, through two
 * parallel tables in the executable:
 *
 *     POLY.code   = codeTable[sel]  | 0x3C
 *     POLY.tpage  = tpageTable[page] | blendTable[sel]
 *
 * `codeTable` is eight bytes at 0x800AE614, read at 0x800682EC (world) and
 * 0x8006DE7C (models). Its contents are {0, 2, 2, 2, 2, 0, 0, 0}: bit 1 of a
 * PSX polygon command is ABE, so selector 0 and selectors 5..7 are opaque and
 * selectors 1..4 are semi-transparent.
 *
 * `blendTable` is eight halfwords at 0x800B36D8, built by the loop at
 * 0x80078378 as blendTable[i+1] = i*32 for i = 0..3 and then blendTable[0] = 32.
 * That yields {32, 0, 32, 64, 96, 0, 0, 0}; tpage bits 5-6 are the ABR field, so
 * the selectors map onto the hardware's four modes as:
 *
 *     sel 0   opaque                          (ABR 1, never reached)
 *     sel 1   semi, ABR 0   B/2 + F/2         glass
 *     sel 2   semi, ABR 1   B + F             fire, energy, liquid surfaces
 *     sel 3   semi, ABR 2   B - F             shadow, smoke
 *     sel 4   semi, ABR 3   B + F/4           faint additive
 *     sel 5-7 opaque, and their blendTable slots are never written
 *
 * Only entries 0..4 are initialised. 5..7 are BSS, hence zero, and the port
 * says so rather than inventing values for them.
 *
 * WHERE EACH CONSUMER SITS IN THAT TABLE, and why the world is a special case:
 *
 *   - A model face carries its selector directly in `face.flags >> 5`
 *     (0x8006DE44), so all five behaviours above are reachable per face.
 *   - The world carries only ONE bit of choice per polygon — `MapMod.clut & 3`,
 *     non-zero meaning transparent — and hard-codes the rest. The opaque path
 *     at 0x800682E8 uses codeTable[0] and blendTable[2]; the transparent path
 *     at 0x800682BC writes the literal 0x3E and DOES NOT touch the tpage word.
 *
 * The consequence is the one thing about world transparency that a port cannot
 * get right by reading the data alone. The opaque path does not merely read
 * `tpageTable[page] | blendTable[2]` — it WRITES IT BACK (`sh` at 0x80068320).
 * The table is built by GetTPage(0, 0, ...) with an ABR argument of 0, so a page
 * starts at ABR 0 (half) and is permanently promoted to ABR 1 (additive) the
 * first time any opaque polygon on it is drawn. Transparent polygons then
 * inherit whatever the table has reached.
 *
 * So "world semi-transparency is additive" is true in the steady state and not
 * true on the first frame a page is touched, and the port models the mutable
 * table rather than the constant, because that is the mechanism. See
 * q2_tpage_table.
 *
 * ---------------------------------------------------------------------------
 * 2. Scene.flags08 — four fields, not one
 * ---------------------------------------------------------------------------
 * The zone draw at 0x80067714 and the node emitter at 0x8006651C / 0x800665B8 /
 * 0x80066740 read the halfword at Scene node +8 four different ways:
 *
 *     bits 0-9    runtime object index, 1-based; 0 means "no object"
 *     bits 10-13  the SETWIBBLE field; only bits 10-11 are read (below)
 *     bit  14     draw this node through the deferred/depth-sorted path
 *     bit  15     do not draw this node
 *
 * **Bits 0-9 are the node-to-mover binding**, and they are what made movers look
 * impossible from the data side. `andi 0x3FF` at 0x80067724 and again at
 * 0x800665D4; the value scales by 92 and indexes the runtime object array whose
 * first element is at 0x800D6BB0, so the array base used is 0x800D6B54 and slot
 * = value - 1. On disc the field is ZERO on every node — the zone loader clears
 * the low ten bits — and the Events constructors fill it in when a map's script
 * binds a mover to a node. A port that reads it off the disc will always see 0
 * and must let its own mover binding write it.
 *
 * That is also why a node's stored origin is uniform across a zone and yet doors
 * move: the object supplies a rotation, a displacement and a rotational pivot,
 * which the zone draw applies at 0x800678B4-0x8006793C:
 *
 *     RotMatrix(&obj[0x0C], m)              three s16 Euler angles
 *     node position += obj.s16[0x12..0x16]  the mover's linear displacement
 *                    - R(obj.s16[0x18..0x1C])
 *                    + obj.s16[0x18..0x1C]  rotate about this local pivot
 *
 * The constructors derive the pivot from the raw Scene-box midpoint minus the
 * Scene origin; ROTHATCH then applies its item+10..+14 hinge adjustment. See
 * rotator.h for the instruction-level derivation.
 *
 * **Bit 15 is a hide flag** (`andi 0x8000` at 0x8006771C, branch straight to the
 * next node). It is clear on all 17,035 nodes on the disc: it is what OBJDRAWOFF
 * sets at runtime.
 *
 * **Bit 14 selects the deferred path** (`srl 14; andi 1` at 0x80066524). Instead
 * of linking straight into the node's ordering-table entry, the emitter takes a
 * record off a bump allocator, projects the node's origin to get one depth for
 * the whole node, records the mover-displaced bounding box, and hands the record
 * to 0x80047744 keyed on the node's area byte (low 7 bits, 64-byte stride
 * table at 0x800D8D78). It is set on the nodes whose on-disc flags are
 * 0x4000/0x4400/0x4800.
 *
 * ---------------------------------------------------------------------------
 * 3. SETWIBBLE, and what "wibble" actually controls
 * ---------------------------------------------------------------------------
 * SETWIBBLE (0x8002E79C) is a plain read-modify-write of the node's flags word:
 *
 *     flags = (flags & 0xFFFFC3FF) | ((arg & 0xF) << 10)
 *
 * so it owns bits 10..13. The renderer reads only bits 10-11 (0x80066740), which
 * select one of four quad-linking routines. What separates them is *when a
 * near-camera quad gets subdivided*, and subdivision is precisely what removes
 * the affine texture warp — so the field is a per-surface warp control:
 *
 *     0  0x800AF7CC  full culling; subdivide any quad nearer than the threshold
 *     1  0x800AFC9C  no subdivision at all, and reject a quad outright when
 *                    either of two opposite corners projected to depth zero
 *     2  0x800AFA2C  full culling; subdivide only quads whose `clut` low byte
 *                    has bits 2-5 set (0x800AFBD4)
 *     3  -           the node's quads are not linked at all
 *
 * Variant 2 half-overturns a previous claim, and the half that does not hold is
 * worth stating plainly. `MapMod.clut`'s low byte was documented as "bits 0-1
 * select semi-transparency, the other six are build-time residue the engine
 * never reads". Bits 2-5 ARE read: 0x800AFBD4 masks them and uses the result as
 * this variant's per-polygon subdivision gate. That much is code.
 *
 * What the disc does NOT support is that they were authored for it. If they
 * were a deliberate permission they should concentrate in the nodes that select
 * variant 2, and `q2psx-inspect surfaces` measures the opposite: they are set on
 * 71.9 % of polygons inside variant-2 nodes against 94.1 % everywhere else. So
 * the honest reading is that they are residue the engine happens to consult —
 * the earlier "never reads" was wrong, and "carries a meaning" is unproven.
 * Bits 6-7 are set on zero of 274,936 polygons, so the whole of the 251,872
 * figure that description rested on is bits 2-5.
 *
 * Subdivision itself is 0x800B007C: it re-projects a 5x5 grid of vertices (21
 * new ones; the four corners are already transformed and arrive as pointers) and
 * writes them into 16 POLY_GT4 packets at 52-byte stride, i.e. one quad becomes
 * a 4x4 mesh. The decision at 0x800698A0 is budget-aware: subdivide freely while
 * the packet pool is healthy, only for very near quads (depth <= threshold/4)
 * while it is half gone, and never once it is nearly exhausted.
 *
 * Variant 3 never occurs on disc, which is the consistency check that the field
 * is runtime state: a script hides a surface group with SETWIBBLE 3 and restores
 * it with 0..2. Values 4..15 alias onto 0..3, because the renderer masks to two
 * bits — SETWIBBLE 4 and SETWIBBLE 0 are the same instruction as far as drawing
 * is concerned. Bits 12-13 are authored on disc (the 0x1000 and 0x1400 values)
 * and NO reader was located for them anywhere in the image.
 */
#ifndef Q2PSX_SURFACE_H
#define Q2PSX_SURFACE_H

#include "gpu.h"
#include "q2psx.h"

/* ------------------------------------------------------------------------- */
/* Blend selectors                                                            */
/* ------------------------------------------------------------------------- */
#define Q2_SURF_SELECTOR_COUNT 8

/* The world's two fixed selectors, from 0x800682E8 and 0x80068304. */
#define Q2_SURF_WORLD_CODE_SELECTOR  0u
#define Q2_SURF_WORLD_BLEND_SELECTOR 2u

/* codeTable, 0x800AE614. Values are OR-ed into the 0x3C polygon command. */
extern const u8  q2_surf_code_table[Q2_SURF_SELECTOR_COUNT];

/* blendTable, 0x800B36D8, as the loop at 0x80078378 leaves it. Values are
 * OR-ed into a texture-page word, so they are already shifted into ABR. */
extern const u16 q2_surf_blend_table[Q2_SURF_SELECTOR_COUNT];

/* The PSX polygon command bit that enables blending. */
#define Q2_SURF_CODE_ABE 0x02u

/* Does selector `sel` produce a semi-transparent primitive? Out-of-range
 * selectors are opaque, matching the two-bit and three-bit masks the engine
 * applies before indexing. */
bool q2_surf_selector_semi(u32 sel);

/* The hardware blend mode selector `sel` names. Meaningless when the selector
 * is opaque, and returns PSX_BLEND_HALF there rather than leaving it undefined. */
psx_blend q2_surf_selector_blend(u32 sel);

/* ------------------------------------------------------------------------- */
/* The mutable texture-page table                                             */
/*                                                                            */
/* Twenty words built at 0x80077FE8 as GetTPage(0, 0, 64*(i+1), 256), i.e. 4bpp */
/* pages at ABR 0. The world's opaque path promotes a page's ABR in place; the  */
/* transparent path reads whatever it finds. Model faces read it without ever   */
/* writing back (0x8006DE50).                                                   */
/* ------------------------------------------------------------------------- */
#define Q2_TPAGE_TABLE_LEN 20

typedef struct q2_tpage_table {
    u16 word[Q2_TPAGE_TABLE_LEN];
} q2_tpage_table;

/* Fill the table exactly as 0x80077FE8 does: page i sits at x = 64*(i+1)
 * halfwords, y = 256, 4bpp, ABR 0. */
void q2_tpage_table_init(q2_tpage_table *t);

/*
 * The world's opaque path (0x80068300-0x80068324): OR blendTable[2] into the
 * page's entry, write it back, and use the result. The write-back is the whole
 * point — call this for every opaque world polygon, in draw order.
 */
u16 q2_tpage_world_opaque(q2_tpage_table *t, u32 page);

/* The world's transparent path (0x800682C8): read the entry as it stands. */
u16 q2_tpage_world_semi(const q2_tpage_table *t, u32 page);

/* A model face (0x8006DE40): read the entry and OR the face's own selector in,
 * without modifying the table. */
u16 q2_tpage_model(const q2_tpage_table *t, u32 page, u32 sel);

/* The blend mode a tpage word carries, for handing to the rasteriser. */
Q2PSX_INLINE psx_blend q2_tpage_blend(u16 tpage)
{
    return (psx_blend)((tpage >> 5) & 3u);
}

/* ------------------------------------------------------------------------- */
/* Scene.flags08                                                              */
/* ------------------------------------------------------------------------- */
#define Q2_SCENE_FLAG_OBJECT_MASK   0x03FFu
#define Q2_SCENE_FLAG_WIBBLE_MASK   0x3C00u
#define Q2_SCENE_FLAG_WIBBLE_SHIFT  10
#define Q2_SCENE_FLAG_DEFERRED      0x4000u
#define Q2_SCENE_FLAG_NODRAW        0x8000u

/* The draw variants bits 10-11 select. */
typedef enum q2_surf_variant {
    Q2_SURF_VARIANT_SUBDIVIDE = 0,  /* 0x800AF7CC — subdivide when near      */
    Q2_SURF_VARIANT_FLAT      = 1,  /* 0x800AFC9C — never subdivide          */
    Q2_SURF_VARIANT_TAGGED    = 2,  /* 0x800AFA2C — subdivide tagged polys   */
    Q2_SURF_VARIANT_HIDDEN    = 3   /* nothing is linked                     */
} q2_surf_variant;

/* Runtime object slot, 0..47, or -1 when the node is bound to no object.
 * Always -1 for flags read straight off the disc. */
Q2PSX_INLINE s32 q2_scene_flags_object(u16 flags)
{
    u32 v = (u32)flags & Q2_SCENE_FLAG_OBJECT_MASK;
    return v ? (s32)(v - 1u) : -1;
}

Q2PSX_INLINE q2_surf_variant q2_scene_flags_variant(u16 flags)
{
    return (q2_surf_variant)((flags >> Q2_SCENE_FLAG_WIBBLE_SHIFT) & 3u);
}

/* The whole four-bit SETWIBBLE field, including the two bits nothing reads. */
Q2PSX_INLINE u32 q2_scene_flags_wibble(u16 flags)
{
    return ((u32)flags & Q2_SCENE_FLAG_WIBBLE_MASK) >> Q2_SCENE_FLAG_WIBBLE_SHIFT;
}

Q2PSX_INLINE bool q2_scene_flags_nodraw(u16 flags)
{
    return (flags & Q2_SCENE_FLAG_NODRAW) != 0;
}

Q2PSX_INLINE bool q2_scene_flags_deferred(u16 flags)
{
    return (flags & Q2_SCENE_FLAG_DEFERRED) != 0;
}

/* Apply SETWIBBLE's rule to a flags word: bits 10-13 replaced, nothing else
 * touched. Transcribed from 0x8002E7CC-0x8002E7D8. */
Q2PSX_INLINE u16 q2_scene_flags_set_wibble(u16 flags, u32 value)
{
    return (u16)((flags & ~(unsigned)Q2_SCENE_FLAG_WIBBLE_MASK)
                 | ((value & 0xFu) << Q2_SCENE_FLAG_WIBBLE_SHIFT));
}

/* ------------------------------------------------------------------------- */
/* Per-polygon subdivision gate                                               */
/*                                                                            */
/* Variant 2 reads bits 2-5 of MapMod.clut's low byte (0x800AFBCC: `lb` of the */
/* signed byte at poly+8, `andi 0x3C`). Non-zero means this polygon may be     */
/* subdivided; zero means it is linked flat however near it gets. Whether the  */
/* exporter meant anything by those bits is a separate question the disc       */
/* answers in the negative — see the header comment. This is what the engine   */
/* does with them either way.                                                  */
/* ------------------------------------------------------------------------- */
#define Q2_SURF_POLY_SUBDIVIDE_MASK 0x3Cu

Q2PSX_INLINE bool q2_surf_poly_may_subdivide(u16 clut)
{
    return ((u32)clut & Q2_SURF_POLY_SUBDIVIDE_MASK) != 0;
}

/*
 * Should a quad at summed depth `depth_sum` be subdivided?
 *
 * `depth_sum` is the sum of two opposite corners' projected Z, which is what the
 * linkers compute (`addu t5, t5, t4` at 0x800AF874). The threshold is the
 * viewport's own, from the display record at +264 and parked in 0x800B2CCC;
 * 0x800B2CC8 holds a quarter of it and is the tighter test the pool-pressure
 * path uses. `pressure` picks between them exactly as 0x800698A0 does.
 */
typedef enum q2_surf_pool_pressure {
    Q2_SURF_POOL_FREE  = 0,  /* subdivide anything nearer than the threshold */
    Q2_SURF_POOL_TIGHT = 1,  /* only when nearer than a quarter of it        */
    Q2_SURF_POOL_FULL  = 2   /* never                                        */
} q2_surf_pool_pressure;

bool q2_surf_should_subdivide(q2_surf_variant variant,
                              u16 clut,
                              s32 depth_sum,
                              s32 threshold,
                              q2_surf_pool_pressure pressure);

/* The subdivision mesh the original builds: a 5x5 vertex grid over the quad,
 * emitted as 16 POLY_GT4 packets. Confirmed by counting 0x800B007C's writes —
 * 21 freshly projected vertices, four corners passed in, and packet offsets
 * running to 812 = 52*15 + 32. */
#define Q2_SURF_SUBDIV_STEPS 4
#define Q2_SURF_SUBDIV_QUADS (Q2_SURF_SUBDIV_STEPS * Q2_SURF_SUBDIV_STEPS)

#endif /* Q2PSX_SURFACE_H */
