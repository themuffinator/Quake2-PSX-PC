/*
 * lighting.h — how the world lights the things standing in it.
 *
 * ---------------------------------------------------------------------------
 * What lights what
 * ---------------------------------------------------------------------------
 * Quake II PSX has two lighting systems and they never meet:
 *
 *   THE WORLD is lit at build time. Every `MapMod` quad carries four indices
 *   into a per-node RGB table and the world renderer just copies them into the
 *   packet (0x800681D8). That baked Gouraud table IS the PlayStation's
 *   lightmap: there is no lightmap texture, no second pass, and nothing at
 *   runtime modulates it. world.c has drawn it correctly since the texturing
 *   pass; this module does not touch it.
 *
 *   EVERYTHING ELSE — models, creatures, items, the view weapon — is lit per
 *   vertex by the GTE from at most THREE point lights chosen per entity per
 *   frame, plus an ambient. That is what this module reconstructs, from
 *   0x8006AFE8 and the two functions it calls.
 *
 * ---------------------------------------------------------------------------
 * The pipeline, in the original's order
 * ---------------------------------------------------------------------------
 *  1. Four candidate slots and a four-entry ranking are reset (0x8006AFE8).
 *  2. Every light that might reach the entity is offered to the ranker
 *     (0x8006AD4C). Three sources, and which ones run depends on the entity:
 *       - the WORLD dynamic list, 16 slots at 0x800E3D18;
 *       - the STATIC lights of the collision node the entity stands in,
 *         through SpaceLights (see spacelights.h);
 *       - or, when signed entity halfword +0xF4 is negative, the VIEW dynamic
 *         list at 0x800E3ED8 instead of both of the above.
 *     An entity with no collision node gets one synthetic light instead of the
 *     static ones — see Q2_LIGHT_FALLBACK_*.
 *  3. Each offer computes a distance attenuation (0x80076040), scales the
 *     light's colour by it, and keeps the result only if it is brighter than
 *     one of the three already held. The candidate always lands in the slot
 *     ranked last, so the loser is simply overwritten next time.
 *  4. The survivors become GTE state: their DIRECTIONS become the light
 *     matrix's rows, their COLOURS become the colour matrix's columns, and the
 *     entity's own glow colour becomes the back colour (0x8008AD0C).
 *  5. Vertices shade through NCT — normal, no primitive colour, no depth cue —
 *     so a model's texture is modulated purely by the light reaching it.
 *
 * Three lights is not a budget chosen here. It is the GTE's: the light matrix
 * has three rows.
 *
 * ---------------------------------------------------------------------------
 * The arithmetic that has to be exact
 * ---------------------------------------------------------------------------
 * The light-to-entity delta is computed in SIXTEEN-bit wrapping arithmetic
 * (`lhu`/`lhu`/`subu` at 0x8006AD70), exactly like the collision queries. A
 * 32-bit port agrees until a light is more than 32 K units away on some axis,
 * at which point the original wraps and this must wrap with it.
 *
 * The attenuation is linear in the SQUARE of the distance, not in the distance:
 *
 *     atten = ((radiusSq - distSq) << 12) / (radiusSq - innerRadiusSq)
 *
 * clamped to 4096 by the caller but NOT clamped below it inside the flare path,
 * which is what makes a flare grow as you walk into a light.
 */
#ifndef Q2PSX_LIGHTING_H
#define Q2PSX_LIGHTING_H

/* The Lights chunk parser. Spelled out because src/game has an entity.h of its
 * own — the game entity — and this needs the on-disc one. */
#include "../formats/entity.h"

#include "gte.h"
#include "q2psx.h"
#include "weapon.h"
#include "spacelights.h"

/* The GTE holds three lights; the engine ranks four so the fourth can be the
 * scratch slot every candidate is written into before it is judged. */
#define Q2_LIGHT_SLOTS      4
#define Q2_LIGHT_ACTIVE_MAX 3

/* The scale argument 0x8006AD4C passes to the attenuation, and the shift that
 * undoes it. Together they are the identity; the flare path passes a larger
 * scale to widen the same light's reach. */
#define Q2_LIGHT_ATTEN_SCALE 64
#define Q2_LIGHT_ATTEN_SHIFT 6

/* 1.12 one, the value the attenuation saturates at and the neutral value for
 * both entity intensity fields. */
#define Q2_LIGHT_ONE 4096

/*
 * The light an entity gets when it is not in any collision node — 0x8006B150
 * building a record at 0x800A1C48 out of the entity's own position.
 *
 * Everything but the position is fixed data in the executable: a dim grey at
 * 48/48/48, a radius of 32767, and an inner radius squared EQUAL to the outer,
 * which makes the attenuation's denominator zero and so pins it at 1.0. It is a
 * flat fill light, not a falloff.
 */
#define Q2_LIGHT_FALLBACK_OFS_X   500
#define Q2_LIGHT_FALLBACK_OFS_Y (-2000)
#define Q2_LIGHT_FALLBACK_OFS_Z (-1000)
#define Q2_LIGHT_FALLBACK_GREY    0x30

/* The two runtime light lists, 16 records of 28 bytes each at 0x800E3D18 and
 * 0x800E3ED8. The count is the array size, not a soft limit: the engine's own
 * end pointers cannot exceed it. */
#define Q2_DYNLIGHT_MAX 16

/* ------------------------------------------------------------------------- */
/* One candidate slot — the 16-byte record at 0x800DDC60                      */
/* ------------------------------------------------------------------------- */
typedef struct q2_light_slot {
    s16 total;     /* +0x00  r + g + b; the ranking key                     */
    s16 rgb[3];    /* +0x02  1.12 colour after attenuation, clamped to 4096 */
    s16 d[3];      /* +0x08  light - entity, 16-bit wrapped                 */
} q2_light_slot;

/* The ranker's whole state. */
typedef struct q2_light_set {
    q2_light_slot slot[Q2_LIGHT_SLOTS];
    s16           rank[Q2_LIGHT_SLOTS];  /* slot indices, brightest first   */
    u32           count;                 /* gp+1400: accepted lights        */
} q2_light_set;

/* ------------------------------------------------------------------------- */
/* What the world offers a light gather                                       */
/* ------------------------------------------------------------------------- */
typedef struct q2_light_world {
    /* COMMON.DAT's Lights array and the zone's per-node index lists. Both may
     * be NULL, which simply means no static lights. */
    const q2_light_list  *statics;
    const q2_spacelights *space;

    /*
     * The two runtime lists.
     *
     * `world` lights everything standing in the map, and is refilled from empty
     * every frame (0x80075B94). `view` is the list the **negative +0xF4** branch
     * reaches instead — and **nothing in the image ever appends to it.** Its end pointer
     * at `0x800B2EC4` has exactly two writers, both of which reset it to the
     * start of the array. The two arrays are also contiguous, and the world
     * appender's full test is a compare against the view array's base, so what
     * is really there is one 16-slot array with a second, permanently empty,
     * view over its end.
     *
     * The consequence is a real behaviour and not a gap in this port: an entity
     * whose signed +0xF4 is negative gets NO lights at all and is drawn by its
     * back colour alone. The field is kept because the engine's storage is kept.
     */
    q2_light dynamic_world[Q2_DYNLIGHT_MAX];
    u32      dynamic_world_count;
    q2_light dynamic_view[Q2_DYNLIGHT_MAX];
    u32      dynamic_view_count;
} q2_light_world;

/*
 * 0x80075C34 — append a runtime light to the world list.
 *
 * The caller passes RADII and this squares them, which is why the on-disc
 * records carry the squares and a redundant radius: the disc format is this
 * function's output, written out by the build tool.
 *
 * It also confirms the `type` byte's bit fields from the other side. It masks
 * `0x3800` and ORs `(style & 7) << 11`, then masks `0xC000` and ORs
 * `(size_shift & 3) << 14` — and never touches bits 0..2, which is why a
 * runtime light's type has the low bits clear while every light on the disc has
 * them set to 7. Nothing reads them.
 *
 * Returns false when the list is full: sixteen lights, and the seventeenth is
 * silently dropped rather than replacing anything.
 */
bool q2_light_add_dynamic(q2_light_world *w, const s32 pos[3],
                          const u8 rgb[3], s32 inner_radius, s32 outer_radius,
                          u32 style, u32 size_shift);

/* ------------------------------------------------------------------------- */
/* FLKLIGHT — a script-placed light that blinks                               */
/* ------------------------------------------------------------------------- */
/*
 * `FLKLIGHT` is one of the two script light primitives (userfuncs.h).
 *
 * **The two randomised formulas are RADII, not durations.** `userfuncs.h`
 * records them as "on/off times ... ((rand()*500)>>15)+400 and +1000", and an
 * earlier version of this file implemented them that way. Reading the handler
 * at `0x800287A0` shows where each value goes:
 *
 *     8002884C  sra   v0, v0, 15
 *     80028850  addiu v0, v0, 400     ; ((r1*500)>>15) + 400
 *     80028858  sh    v0, 16(sp)      ; ...into a2's LOW half  = INNER radius
 *     80028888  addiu v0, v0, 1000    ; ((r2*500)>>15) + 1000
 *     8002888C  sh    v0, 18(sp)      ; ...into a2's HIGH half = OUTER radius
 *
 * and `0x800288C8` passes that `a2` straight to `0x80075C34`. So a flicker is
 * 400..899 inner and 1000..1499 outer, redrawn per flash. Its colour comes from
 * the item at +18/+19/+20, which is what the operand table already said.
 *
 * **There is no on/off phase, and this port no longer invents one.** The exec at
 * `0x800287A0` loops the primitive's objects, adds one dynamic light each and
 * returns — no timer, no state, nothing that could turn it off. What makes a
 * flicker flicker is that BOTH radii are redrawn from `rand()` on every call, so
 * the same script record casts a different-sized light each time it runs.
 *
 * An earlier version of this file carried a `q2_flklights` set with an on/off
 * phase and a placeholder duration. That machinery had no counterpart in the
 * original and is gone; FLKLIGHT is raised as a transient, exactly like
 * TIMEDLIGHT, and the two radius functions below are all that is needed.
 *
 * The disc carries exactly ONE FLKLIGHT call, so this is deliberately small: a
 * fixed set, no allocation, and a tick that costs nothing when empty.
 */
/* The flicker's radii, as 0x8002882C and 0x8002885C compute them. */
s32 q2_flklight_inner_radius(s32 rand_0_32767);
s32 q2_flklight_outer_radius(s32 rand_0_32767);


/* 0x80075B94 — empty the runtime lists at the top of a frame. */
void q2_light_world_begin_frame(q2_light_world *w);

/* ------------------------------------------------------------------------- */
/* What a gather produces                                                     */
/* ------------------------------------------------------------------------- */
typedef struct q2_light_env {
    /* The GTE colour matrix: row is the colour component, COLUMN is the light.
     * Row 0 is therefore the three lights' red. */
    gte_matrix colour;

    /* Three world-space directions, normalised to 1.3.12 and then scaled by
     * the entity's intensity. Rows of the light matrix once the caller has
     * composed them with the model's own rotation. */
    s16 dir[Q2_LIGHT_ACTIVE_MAX][3];

    /* Back colour, 0..255 per component — pass straight to
     * gte_set_back_colour, which does the <<4 the hardware wants. */
    s32 back[3];

    u32 active;   /* how many of `dir` and the colour columns are real, 0..3 */
} q2_light_env;

/* ------------------------------------------------------------------------- */
/* The pieces, each named for the function it came from                       */
/* ------------------------------------------------------------------------- */

/*
 * 0x80076040 — the distance attenuation, 1.12.
 *
 * `scale` multiplies both radii squared before the comparison (the entity path
 * passes Q2_LIGHT_ATTEN_SCALE, i.e. the identity; the flare path passes the
 * flare's own size). Returns 0 outside the outer radius and 4096 when the two
 * radii are equal. The result is NOT clamped: inside the inner radius it
 * exceeds 4096, which the flare path relies on and the entity path clamps.
 */
s32 q2_light_attenuation(const q2_light *l, const s16 d[3], s32 scale);

/* The 16-bit wrapping delta the whole system is defined in terms of. */
void q2_light_delta(const q2_light *l, const s32 origin[3], s16 out[3]);

/* 0x8006AFE8's prologue: four empty slots ranked 0,1,2,3. */
void q2_light_set_begin(q2_light_set *set);

/*
 * 0x8006AD4C — offer one light. Returns true when it displaced one of the three
 * being held. Rejects on any axis delta at or beyond the light's radius before
 * it computes anything expensive, which is the original's own early-out and is
 * why the radius field exists alongside radiusSq.
 */
bool q2_light_set_add(q2_light_set *set, const s32 origin[3], const q2_light *l);

/*
 * 0x8006AFE8 — the whole gather for one entity.
 *
 * `coll_node` is the SecondaryCol node the entity stands in, or negative for
 * none (which selects the fallback light). `entity_f4` is the signed halfword
 * at entity +0xF4: values >= 0 pick the world/static path; values < 0 pick the
 * view dynamic list and skip both. Passing the field rather than a boolean is
 * deliberate — the viewmodel writes literal +1 and therefore takes WORLD.
 */
void q2_light_gather(q2_light_set *set, const q2_light_world *w,
                     const s32 origin[3], s32 coll_node, s16 entity_f4);

/*
 * 0x8006B184…0x8006B4B8 — turn a finished gather into GTE state.
 *
 * `intensity_a` and `intensity_b` are the entity's two 1.12 scale fields
 * (+0xFC and +0xFE); pass Q2_LIGHT_ONE for both to get the original's neutral
 * behaviour, where the back colour comes out as exactly the glow bytes. `glow`
 * is the entity's own ambient colour, 0..255 per component, or NULL for black.
 */
void q2_light_env_build(q2_light_env *env, const q2_light_set *set,
                        s32 intensity_a, s32 intensity_b, const u8 glow[3]);

/* Load an environment into a GTE. The light matrix is the three directions as
 * rows, so a caller that needs them in model space must compose first and call
 * gte_set_light_matrix itself; this is the world-space form. */
void q2_light_env_apply(const q2_light_env *env, gte_state *g);

/*
 * 0x8008A5B8 / 0x8008A5E8 — libgte's VectorNormal, reproduced.
 *
 * Normalises an integer vector to 1.3.12. The original squares through the GTE
 * (SQR with sf = 0, so the magnitude is NOT pre-divided), rounds the leading
 * zero count DOWN TO EVEN so the square root's exponent stays an integer,
 * shifts the magnitude into the resulting [64,256) window, looks a reciprocal
 * square root up in a 192-entry table at 0x800A9C54, and shifts the product
 * back by half the exponent.
 *
 * This computes the same table from `32768 / sqrt(i + 64)`;
 * `q2psx-inspect lights` measures the two against each other entry by entry.
 */
void q2_vector_normal(const s16 in[3], s16 out[3]);

/* The reciprocal-square-root table, for the tool that compares it. */
s16 q2_light_rsqrt_table(u32 index);
#define Q2_LIGHT_RSQRT_ENTRIES 192

/*
 * 0x80075E14 — the per-entity ambient fade.
 *
 * `current` walks one `steps`-th of the way toward `target` per call, per
 * component, in integer arithmetic that truncates — so it converges only when
 * the gap exceeds `steps`, and the last few units arrive when the gap finally
 * drops below it. Passing steps == 0 is read as 1, as the original does.
 */
void q2_light_glow_fade(u8 current[3], const u8 target[3], s32 steps);

#endif /* Q2PSX_LIGHTING_H */
