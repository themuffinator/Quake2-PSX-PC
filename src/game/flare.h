/*
 * flare.h — the lens flares, and what a light's `type` byte actually selects.
 *
 * ---------------------------------------------------------------------------
 * The type byte, which was open question #26
 * ---------------------------------------------------------------------------
 * Every one of the 7,814 lights on the disc carries a `type` of 7, 15, 23, 31
 * or 39 — `(n << 3) | 7` for n = 0..4 — and what those five values do was
 * recorded as unknown. They are not "styles" in the Quake sense (there is no
 * animation table anywhere in the image). The flare pass at 0x80075708 splits
 * the byte three ways:
 *
 *     bits 0-2   always 7 on this disc, never read by the flare path
 *     bits 3-5   the FLARE STYLE, 0..4. Style 0 draws nothing; 1..4 select one
 *                of four element lists in the executable
 *     bits 6-7   a SIZE SHIFT: the flare's world reach is 64 << k. Always 0
 *                here, so every flare on the disc is drawn at 64
 *
 * The style is tested before anything else is computed: the per-viewport pass
 * at 0x80075AA4 and 0x80075B58 masks the word at light+16 with 0x3800 and skips
 * the light entirely when it is zero. So a style-0 light lights the world and
 * is never seen; a style-1..4 light also puts a flare on the screen.
 *
 * Disc-wide: 6,183 lights at style 0, 660 at 1, 183 at 2, 2 at 3 and 786 at 4.
 *
 * ---------------------------------------------------------------------------
 * What a flare is made of
 * ---------------------------------------------------------------------------
 * A style is a NULL-terminated list of 8-byte elements at 0x800A1FDC,
 * 0x800A2014, 0x800A2024 and 0x800A203C:
 *
 *     u16 kind      1 = glow disc plus a starburst, 2 = plain disc
 *     s16 size      1.12 multiplier on the flare's screen radius
 *     s16 pos       1.12 position ALONG THE LINE from the screen centre to the
 *                   light: 4096 is at the light, 0 at the centre, negative is
 *                   mirrored through it
 *     s16 colour    1.12 multiplier on the flare's colour
 *
 * That `pos` field is what makes this a lens flare rather than a corona: style
 * 1's six elements sit at 1.0, -0.93, -0.25, 0.15, 1.5 and -1.25 of the way
 * along that line, at shrinking sizes and dimming colours. Style 4 is the same
 * list without its last element, style 3 is its first two, style 2 is its first
 * one alone — so the four styles are nested, and the only real authored choice
 * is how much of the flare a given light shows.
 *
 * ---------------------------------------------------------------------------
 * Where it is drawn, and how bright
 * ---------------------------------------------------------------------------
 * 0x80075708, per light per viewport:
 *
 *   - the light's position relative to the camera goes through MVMVA against
 *     the view rotation with NO translation (the delta is already relative),
 *     and the flare is dropped if the camera-space Z is under 256;
 *   - the same attenuation the entity lighting uses runs again, but with the
 *     size shift as its scale, so a flare's reach is the light's radius scaled
 *     by 64 << k rather than by 64;
 *   - inside the inner radius the colour is the light's own at full strength
 *     and the SIZE grows as `atten >> 7`; outside it the size pins to 32 and
 *     the COLOUR dims as `colour * atten >> 8`. One expression each side of
 *     4096, which is why a flare blooms as you walk into it and fades as you
 *     walk away.
 *
 * Both element kinds are untextured and inherit ABR: the glow is a fan of Gouraud
 * quads bright at the centre and black at the rim, and kind 1 adds eight
 * Gouraud lines radiating from the centre. There is no flare texture anywhere
 * on the disc, which is why looking for one never found anything.
 *
 * ---------------------------------------------------------------------------
 * The two ring generators, which are two routines and not one
 * ---------------------------------------------------------------------------
 * The BURST's 12-gon comes from a general n-gon generator at 0x80074E6C. The
 * DISC's hexagon does NOT: 0x800755CC calls 0x80074C4C, a separate routine that
 * is written out flat, reads a single table entry (index 682 — the 60 degrees a
 * hexagon turns by) and mirrors it into all six vertices rather than looking
 * three of them up. Its vertical radius divides by 240 where the generator
 * divides by 240*4096 against a cosine of 4096, and its vertex order starts at
 * the top where the generator starts at the bottom. A hexagon has 180-degree
 * symmetry, so the two agree on the shape and disagree only in the arithmetic —
 * which is why both are transcribed rather than one aliased onto the other.
 *
 * Both read their angles from the executable's own {sin, cos} table at
 * 0x800A5430: 4096 entries of two halfwords, a full turn in 4096 steps.
 * `q2psx-inspect lights` reads all 16,384 bytes of it back off the disc and
 * compares them against this port's `q2_sin12`/`q2_cos12` — 4096 of 4096 in
 * both columns, so the flare geometry stands on the console's own trig.
 *
 * ---------------------------------------------------------------------------
 * The frame entry, and the gate
 * ---------------------------------------------------------------------------
 * 0x80075BDC is the whole flare stage: it tests a word at 0x800B2ECC, gives up
 * if the viewport count at 0x800B2C2C is not positive, and otherwise runs
 * 0x800759F0 once per viewport. That gate has exactly one writer in the image —
 * 0x80071448 stores 1 to it during start-up — and nothing ever clears it, so on
 * this disc the flares are unconditionally on and the flag is vestigial. It is
 * named here because a reader finding the branch should not have to go looking.
 *
 * The stage is the sixteenth of the frame (0x8003932C stamps 16 into the
 * profiler's slot before the call), which puts it after the world and before
 * the entity draw. Call order does not decide draw order, though: every flare
 * links into the viewport slice's LAST bucket — `db + 11192 + 204*p`, which is
 * slice bucket 50, the same one the damage flash uses — so with the table
 * running forward a flare paints over everything the viewport drew, which is
 * what a lens flare is.
 */
#ifndef Q2PSX_FLARE_H
#define Q2PSX_FLARE_H

/* The Lights chunk parser; see the note in lighting.h about the two entity.h. */
#include "../formats/entity.h"

#include "gpu.h"
#include "gte.h"
#include "lighting.h"
#include "q2psx.h"
#include "world.h"

/* Style 0 draws nothing; 1..4 have element lists. */
#define Q2_FLARE_STYLE_COUNT 5

/* The base screen reach before the type byte's size shift. */
#define Q2_FLARE_BASE_SIZE 64

/* Below this camera-space Z the flare is dropped (0x80075814). */
#define Q2_FLARE_MIN_Z 256

/* The floor the screen radius pins to once the light is past its inner radius
 * (0x800759BC), and the shift that turns a larger attenuation into a larger
 * flare (0x8007594C). */
#define Q2_FLARE_MIN_SCALE 32
#define Q2_FLARE_SCALE_SHIFT 7

/*
 * The reference screen every one of the six folded divides is expressed
 * against, and the three denominators that divide it.
 *
 * Each divide is a `mult`/`mfhi`/`sra`/`subu` magic sequence, and a magic M
 * with post-shift p is a signed divide by the unique d satisfying
 * M == floor(2^(32+p)/d) + 1. Solving all six gives 1310720, 983040, 800000,
 * 600000, 1120000 and 840000 — that is 320 and 240 times 4096, 2500 and 3500,
 * with nothing left over. So the rings are a fraction of the VIEWPORT rather
 * than a count of pixels, and the starburst's arms are the same fraction taken
 * against a smaller reference: an axis arm reaches 4096/2500 = 1.6384x the
 * glow's radius and a diagonal 4096/3500 = 1.1703x.
 */
#define Q2_FLARE_REF_W 320
#define Q2_FLARE_REF_H 240
#define Q2_FLARE_SPIKE_REF 2500   /* the starburst's axis arms  */
#define Q2_FLARE_DIAG_REF  3500   /* its diagonals              */

/* The rims. The burst's 12-gon comes from the general generator at 0x80074E6C;
 * the disc's hexagon from its own unrolled routine at 0x80074C4C, which reads
 * only the one table entry named here. */
#define Q2_FLARE_BURST_SIDES 12
#define Q2_FLARE_DISC_SIDES   6
#define Q2_FLARE_HEX_ANGLE  (4096 / Q2_FLARE_DISC_SIDES)   /* 682 */

/* The starburst is eight lines drawn as four opposed pairs (0x80074FF4's
 * `a1 = 8`, halved for the loop bound). */
#define Q2_FLARE_BURST_LINES 8

/* The rim buffer both generators write into. Three past the last vertex,
 * because the fan reads out[i+1..i+3] and the generators duplicate out[1] and
 * out[2] at the top rather than wrapping the index. */
#define Q2_FLARE_RING_MAX 16

typedef enum q2_flare_kind {
    Q2_FLARE_KIND_NONE  = 0,
    Q2_FLARE_KIND_BURST = 1,   /* 12-gon glow + 8 radiating lines */
    Q2_FLARE_KIND_DISC  = 2    /* 6-gon flat disc                 */
} q2_flare_kind;

typedef struct q2_flare_element {
    u16 kind;      /* q2_flare_kind; 0 terminates the list */
    s16 size;      /* 1.12 */
    s16 pos;       /* 1.12 along centre -> light */
    s16 colour;    /* 1.12 */
} q2_flare_element;

/* The four lists, read out of the executable's data segment. `count` excludes
 * the terminator. Style 0 has none. */
typedef struct q2_flare_style {
    const q2_flare_element *element;
    u32                     count;
} q2_flare_style;

/* The style a light's type byte selects, and its size shift. */
u32 q2_flare_style_of(u8 type);
u32 q2_flare_size_of(u8 type);

/* The built-in table. Returns a style with count == 0 for 0 and for anything
 * out of range. */
const q2_flare_style *q2_flare_style_table(u32 style);

/* ------------------------------------------------------------------------- */
/* Drawing                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Where the flare pass draws into. `centre` and `extent` are the viewport's
 * own, and they are two different things in the original — the GTE geometry
 * offset at view+266/+268 that elements are positioned against, and the 2D
 * extent at +278/+280 that both ring generators scale by. The offset is where
 * the projection puts the view axis, so it is the screen centre in the sense
 * that matters here; pass the same value the world draw gives SetGeomOffset,
 * not a bare width/2, or every element slides along its own line on a viewport
 * whose offset has been moved.
 */
typedef struct q2_flare_view {
    s16 centre[2];
    s16 extent[2];
    u16 bucket;      /* the OT bucket flares are linked into */
} q2_flare_view;

typedef struct q2_flare_stats {
    u32 lights_considered;
    u32 lights_styled;      /* style != 0                          */
    u32 rejected_near;      /* camera-space Z under Q2_FLARE_MIN_Z  */
    u32 rejected_dark;      /* attenuation zero                     */
    u32 flares_drawn;
    u32 prims_emitted;
    u32 ot_overflow;
} q2_flare_stats;

/*
 * Draw one light's flare, if it has one. Returns the number of primitives
 * emitted, which is zero for style 0, for a light behind the camera and for one
 * out of range.
 *
 * The GTE's rotation matrix must already hold the view rotation — this uses it
 * through MVMVA exactly as the original does, so the flare lands on the same
 * pixel the geometry does.
 *
 * The GTE's TRANSLATION must be zero, and that is now read rather than assumed.
 * The per-viewport pass loads TRX/TRY/TRZ from 0x800DDD7C, the translation
 * slot of a MATRIX at 0x800DDD68 — and that object has no writer anywhere in
 * the image. It sits at 0x800DDD68, well past the end of the text segment at
 * 0x800B2800, so it is BSS: zero from start-up and zero for the life of the
 * session. Its three readers agree. Two of them (0x80058450 and 0x80058484)
 * pass 0x800DDD7C to T_Damage as BOTH the `dir` and the `point` argument, which
 * is a call that only makes sense as "no direction, no impact point" — a
 * non-zero vector there would knock the target somewhere. The third
 * (0x80030598) is the entity draw doing exactly what the flare pass does.
 *
 * So it is a static zero VECTOR the engine reuses wherever it needs one, and
 * loading it into TRX/TRY/TRZ is how both passes say "rotation only". That is
 * also what makes the transform self-consistent, because the vector fed to the
 * GTE is already the light's offset FROM the camera.
 */
u32 q2_flare_draw(const q2_light *l, const q2_camera *cam,
                  const q2_flare_view *view, psx_ot *ot, gte_state *gte,
                  q2_flare_stats *stats);

/*
 * The per-viewport pass at 0x800759F0: every dynamic light, then every static
 * light of the collision node the camera stands in. `coll_node` may be negative,
 * in which case only the dynamic lights are considered — that is the original's
 * own behaviour, and it means a flare is only visible from inside a node the
 * level's build tool linked it to.
 */
u32 q2_flare_draw_all(const q2_light_world *w, const q2_camera *cam,
                      s32 coll_node, const q2_flare_view *view,
                      psx_ot *ot, gte_state *gte, q2_flare_stats *stats);

#endif /* Q2PSX_FLARE_H */
