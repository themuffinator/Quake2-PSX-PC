/*
 * trace.h — the engine's entity mover, transcribed from SLES_015.34.
 *
 * collision.[ch] answers geometric questions about a hull. This is the layer
 * above it: what the original does with those answers when something is trying
 * to move through the world.
 *
 *     0x80045144   move with sliding                  q2_move
 *     0x800456B0   move, retry if it ends up stuck    q2_move_checked
 *     0x8004583C   the player's step-up/step-down     q2_move_step
 *     0x8005625C   the one-unit unstick nudge         q2_move_push_vector
 *
 * ---------------------------------------------------------------------------
 * Two arguments that look like flags and are structs
 * ---------------------------------------------------------------------------
 * 0x80045144 takes four registers, and the third and fourth are each a pair of
 * halfwords rather than an int — which is why an earlier reading of the step
 * sequence recorded "mode = 0 / 3 / 0" without being able to say what a mode
 * was. Read at 0x80045200 (a3 hi), 0x8004526C (a2 lo), 0x800453F0 (a2 hi) and
 * 0x80045444 (a3 lo), they are:
 *
 *     a2.lo  ground_mode   record a contact as GROUND and set the on-ground
 *                          flag when the surface is flat enough
 *     a2.hi  push_mode     apply the one-unit unstick nudge after a hit
 *     a3.lo  iterations    how many further slide attempts to make
 *     a3.hi  sweep_ents    also sweep against other entities
 *
 * The player's three moves per frame pass them from a table at 0x800AE930:
 *
 *     up    {0, 0}  0 iterations   no ground detection, no nudge
 *     slide {0, 1}  3 iterations   nudge on, ground detection OFF
 *     down  {1, 0}  0 iterations   ground detection ON
 *
 * Ground is therefore decided by the DOWN move alone. A port that sets
 * on-ground from the horizontal slide will report standing on walls.
 *
 * ---------------------------------------------------------------------------
 * Sign conventions, all of which bite
 * ---------------------------------------------------------------------------
 * +Y points DOWN. Cell planes face OUT of the empty cell. So the floor under
 * an entity bounds its cell in the +Y direction and its outward normal has
 * ny > 0: "flat enough to stand on" is `normal.y > entity->max_slope_ny`, a
 * comparison in the positive direction (0x80045288).
 *
 * `q2_move` returns 1 when the move went through untouched and 0 when anything
 * stopped it — the same inverted sense the two trace primitives use, and for
 * the same reason: a blocked move is the only kind that produces a contact
 * normal.
 */
#ifndef Q2PSX_TRACE_H
#define Q2PSX_TRACE_H

#include "collision.h"
#include "q2psx.h"

/* Entity flag bits this module reads or writes, at entity+0x44. */
#define Q2_ENT_ON_GROUND      0x00000020u /* 0x800452D8: blocked by a floor    */
#define Q2_ENT_ON_ENTITY      0x00000040u /* 0x800452AC: standing on an entity */
#define Q2_ENT_GROUNDED_MASK  0x00000060u /* 0x80045C14 tests both together    */
#define Q2_ENT_LOW_STEP       0x00000600u /* 0x80045880: halves the step height*/

/* The index of the entity being stood on lives in bits 18..23 (0x800452C0). */
#define Q2_ENT_STANDON_SHIFT 18
#define Q2_ENT_STANDON_MASK  0x00FC0000u

/*
 * The mover's view of an entity. Offsets are the original's, so a reader with
 * the disassembly open can check every one of them.
 */
typedef struct q2_move_ent {
    s32 pos[3];            /* +0x00 */
    s16 ground_normal[3];  /* +0x0C: the contact normal with the largest ny   */
    /*
     * +0x12: the contact with the SMALLEST |ny| — the most wall-like surface
     * touched this frame, which is what makes this slot a wall detector rather
     * than a second floor slot. It is reset to (0, 4096, 0) before each frame's
     * moves precisely because 4096 is the largest |ny| there is, so the
     * sentinel always loses to a real contact. 0x800453CC.
     */
    s16 last_normal[3];
    u32 flags;             /* +0x44 */
    /*
     * +0x48: the steepest ny that still counts as ground; compared with `<`.
     * Q2_MAX_SLOPE_NY (2600) for every entity — 0x8006C1B0 writes it right
     * after the allocator's memset, so it is never zero on the console.
     */
    s16 max_slope_ny;
    s32 node;              /* +0x4E: the cached collision cell               */
} q2_move_ent;

/* The arguments 0x80045144 packs into two registers. */
typedef struct q2_move_mode {
    bool ground_mode;   /* a2.lo */
    bool push_mode;     /* a2.hi */
    s32  iterations;    /* a3.lo */
    bool sweep_ents;    /* a3.hi */

    /*
     * What the entity sweep runs against when `sweep_ents` is set. NULL means
     * no other entities exist, which is the state a zone with an empty table is
     * in anyway — the original still calls the sweep and it finds nothing.
     */
    const struct q2_move_world *world;
} q2_move_mode;

/* ------------------------------------------------------------------------- */
/* The entity half of collision                                               */
/* ------------------------------------------------------------------------- */
/*
 * 0x80052C70 — sweep a moving POINT against one box.
 *
 * The moving entity is a point here, not a box, because the box has already
 * been added to the target: the caller passes the target's own bounds and the
 * mover's half-extents, and this inflates the former by the latter. Same trick
 * the world hull uses — SecondaryCol is PrimaryColl eroded by 286 — applied
 * per-pair at runtime because the other entity moves.
 *
 * `other_dy` is the target's own vertical displacement this frame, subtracted
 * from the mover's before the test. Only the vertical axis is made relative --
 * and that is the CONSOLE'S choice, not a simplification here.
 *
 * This file used to give the reason as "movers travel on one axis and it is
 * always this one", which is false: PLATFORM travels on all three (mover.h).
 * The real reason is that the console is handed the whole triple and reads one
 * of it. `0x80053E34` passes `&box + 0x30` -- the s16 displacement 0x80051EC0
 * accumulates on every axis -- as the sixth argument, and 0x80052C70 touches
 * exactly two instructions of it:
 *
 *     80052E20  lh   v0, 2(t4)      ; element [1] and no other
 *     80052E28  subu a0, v1, v0     ; delta[1] -= other[1]
 *     80052E3C  addu v1, v0, v1     ; pos[1]   += other[1]
 *
 * Elements [0] and [2] are never loaded. So a player standing on the disc's one
 * train is carried DOWN with it and not ALONG with it, on the console as here;
 * a port that fixed that would be diverging, not completing.
 *
 * The six faces are tested in the order +Y, -Y, +Z, -Z, +X, -X and the FIRST
 * one that passes wins — not the earliest impact. +Y is down, so the floor of
 * the box the mover is standing on is tested first, which is what makes
 * standing on things stable.
 *
 * Returns true on contact, with `out_pos` the exact contact position (computed
 * directly, never as a fraction of the move) and `out_normal` one of the six
 * axis normals from the table at 0x800AEA2C.
 */
bool q2_move_sweep_box(const s32 pos[3], const s16 delta[3],
                       const s32 box_min[3], const s32 box_max[3],
                       s16 other_dy, s32 half_extent,
                       s32 out_pos[3], s16 out_normal[3]);

/*
 * 0x8005396C's caller-side AABB gate: does the whole move, inflated by
 * `half_extent` on all six faces, overlap the box at all? The original runs
 * this before the sweep and skips the expensive test when it fails.
 */
bool q2_move_box_overlap(const s32 move_min[3], const s32 move_max[3],
                         const s32 box_min[3], const s32 box_max[3]);

/*
 * What 0x80053C58 sweeps against: the 48-slot entity table at 0x800CAE10 and
 * then the map's trigger volumes at 0x800C9114, in that order.
 *
 * The port takes the list from the caller instead of owning a fixed table,
 * because the two halves live in different places here — but the ORDER matters
 * and is preserved: entities first, then volumes, and within each the table
 * order, because the sweep keeps the last contact rather than the nearest.
 */
typedef enum q2_move_kind {
    Q2_MOVE_KIND_WORLD   = -1,  /* 0x80052C70's own default */
    Q2_MOVE_KIND_ENTITY  = 2,   /* 0x80053E68               */
    Q2_MOVE_KIND_VOLUME  = 3    /* 0x80054090               */
} q2_move_kind;

typedef struct q2_move_target {
    s32 min[3], max[3];  /* the target's own bounds                          */

    /*
     * THE SWEPT ENVELOPE — slot +0x18..+0x2C, and it is a second box, not a
     * copy of the first.
     *
     * 0x80051EC0 maintains both when a mover moves. The live box at +0x00 gets
     * the frame's delta added to BOTH corners (0x80051F08-0x80051F7C), so it
     * translates. The envelope gets it added to ONE corner at a time, chosen
     * by the sign of each component (`bgez v1` -> +0x24 when positive, +0x18
     * when negative, at 0x80051FBC onward), so it only ever GROWS: it covers
     * the whole of the door's travel.
     *
     * The distinction is load-bearing. The broad-phase gate that decides
     * whether to sweep entities at all (0x80050CE0 with a2 = 1) reads the
     * ENVELOPE — 0x80050D94 `lw v0, 32(a0)` is slot+0x20 — while the narrow
     * sweep uses the live box. Gating on the live box would only arm the sweep
     * for a player already inside the door, not for one about to be crossed by
     * it.
     */
    s32 env_min[3], env_max[3];

    s16 dy;              /* its vertical motion this frame; lifts use it     */
    u16 mask;            /* flags; 0 means "always considered"               */
    s16 kind;            /* q2_move_kind                                     */
    s32 id;              /* the caller's handle, reported back on contact    */
    bool active;         /* the 48-slot table's byte +54                     */
} q2_move_target;

typedef struct q2_move_world {
    const q2_move_target *targets;
    u32 count;
    s32 half_extent;     /* the MOVER's, 286 for the player                  */
    u16 mask;            /* the mover's own mask: 0x810 when its flag bit 0
                          * is set, else 0 (0x8005553C). A target whose mask
                          * is non-zero is skipped unless it intersects this. */
} q2_move_world;

typedef struct q2_move_contact {
    s32  pos[3];
    s16  normal[3];
    s16  kind;
    s32  id;
    bool hit;
} q2_move_contact;

/*
 * 0x80053C58 — sweep a move against every target.
 *
 * Returns 1 when nothing was hit and 0 when something was, matching both the
 * original and q2_move's own sense. `out->pos` always receives where the move
 * actually ends up. After a contact the sweep CONTINUES from the clipped
 * position against the remaining targets, so a move into a corner of two
 * entities is resolved in one pass.
 */
int q2_move_sweep_world(const q2_move_world *w, const s32 pos[3],
                        const s32 dest[3], q2_move_contact *out);

/*
 * 0x80050CE0 — which volume is the entity standing in?
 *
 * Tests a box of (286, 285, 286) half-extents around `pos` — note the vertical
 * 285, which is the original's, not a typo — against every target whose flags
 * intersect `mask`, and returns the first match's flags, or 0.
 *
 * The player passes 0x3360. Three of those bits are understood: 0x0200 makes
 * an entity sink slowly, 0x2000 makes it buoyant and boosts it when grounded,
 * and 0x1000 gates a flag on its vertical speed. See worldscale.h.
 */
u16 q2_move_contents(const q2_move_world *w, const s32 pos[3], u16 mask);

/* The contents box's half-extents, 0x80050CE8..0x80050D38. */
#define Q2_CONTENTS_HALF_XZ 286
#define Q2_CONTENTS_HALF_Y  285

/* The mask the player's own query passes, 0x800458A0. Of its six bits, 0x0200,
 * 0x1000 and 0x2000 are understood; 0x0100, 0x0040 and 0x0020 are volume
 * classes the player cares about whose effects are not yet traced. */
#define Q2_CONTENTS_MASK 0x3360u

/*
 * 0x8005625C — the unstick nudge.
 *
 * Picks the dominant axis and sign of `normal` and returns the corresponding
 * unit vector from the table at 0x800AEA64. The mover SUBTRACTS it, so the
 * result moves back into the cell the normal points out of.
 */
void q2_move_push_vector(const s16 normal[3], s16 out[3]);

/*
 * 0x80045144 — move `ent` by `delta`, sliding along whatever it hits.
 *
 * `delta` is int16 per axis, as the original stores it; a caller with a larger
 * intent must break the move up rather than widen the type, because the whole
 * arithmetic chain below is 16-bit.
 *
 * Returns 1 when the entire move went through, 0 when anything stopped it.
 * Updates `ent->pos`, `ent->node`, `ent->flags`, and the two contact normals.
 */
int q2_move(q2_collision *coll, q2_move_ent *ent, const s16 delta[3],
            const q2_move_mode *mode);

/*
 * 0x80050CE0 with a2 = 1 — does a 286/285/286 box around `pos` overlap the
 * swept ENVELOPE of any active ENTITY target?
 *
 * This is 0x8004576C's gate: the answer decides whether the move is worth
 * re-running with entity sweeping on. Testing the envelope rather than the
 * live box is what makes it arm for a player a moving door is about to reach,
 * as opposed to one already inside it.
 */
bool q2_move_overlaps_any(const q2_move_world *w, const s32 pos[3]);

/* ------------------------------------------------------------------------- */
/* Segment against the entity boxes — the pass every non-movement query was   */
/* missing                                                                    */
/* ------------------------------------------------------------------------- */
/*
 * A DOOR IS AN ENTITY, AND THE HULL DOES NOT CONTAIN IT.
 *
 * PrimaryColl and SecondaryCol are the map's static geometry. A door, a lift,
 * a crusher and a train are Scene nodes bound to runtime objects, and their
 * boxes live in the 48-slot entity table this file's `q2_move_world` stands
 * for. So a query answered out of the hull alone is answered as if every door
 * in the level were open, and the console does not do that: `0x8004874C` runs
 * the bullet through the hull and then RE-TRACES against the entity list
 * (`0x800544EC`), `visible` clips its sight line the same way, and
 * `M_CheckBottom` passes a corner that reached the end of its drop to
 * `0x80053974` before calling it a ledge.
 *
 * This is that second pass, in one place, because it had to be added to five
 * callers at once and five copies of a slab test is how they drift apart.
 *
 * NOT `q2_move_sweep_world`, and the reason is arithmetic rather than taste:
 * that one is the MOVE sweep, its delta goes through `sub16` and therefore
 * wraps at 16 bits, and it keeps the LAST contact rather than the nearest.
 * Both are right for a player's 30-unit step and both are wrong for a shot
 * whose range is tens of thousands of units and which must stop at the FIRST
 * thing it meets.
 *
 * VOLUMES ARE SKIPPED. The trigger half of the target list is not solid — a
 * shot does not stop in a water brush and a creature can see across one — and
 * the entity half is solid to everything, which is the same split
 * `q2_move_sweep_world` makes when it puts the mask test inside the volume
 * arm only.
 */
/* 1.0 as a 1.0.12 fraction, the scale every segment in the port is
 * parametrised in (Q2_ONE_12 in fixed.h, restated here so this header keeps
 * the two includes it has). */
#define Q2_TRACE_SEG_ONE 4096

typedef struct q2_move_seg_hit {
    s32  frac;        /* 1.0.12 along from..to; 0 means the start was inside */
    s32  pos[3];      /* the crossing point                                  */
    s16  normal[3];   /* the face's axis normal, 1.3.12, pointing along the
                       * ray as every other contact normal here does         */
    s32  id;          /* the target's own handle: the MOVER's index          */
    bool hit;
} q2_move_seg_hit;

/*
 * The nearest ENTITY box the segment `from`..`to` crosses, or false.
 *
 * `inflate` is the Minkowski sum a caller with a body of its own needs: each
 * box grows by `inflate[k]` on both faces of axis k, so the swept POINT is the
 * swept box. NULL is a bare point, which is what a shot and a sight line are.
 * Per axis rather than one scalar because a creature is taller than it is
 * wide, and one scalar taken from its height would make it that wide too.
 */
bool q2_move_clip_segment(const q2_move_world *w, const s32 from[3],
                          const s32 to[3], const s32 inflate[3],
                          q2_move_seg_hit *out);

/*
 * 0x800456B0 — move, and if the entity ends up somewhere it may not be, put it
 * back and try again with entity sweeping enabled.
 *
 * The original's retry condition is 0x80050CE0, the contents/entity overlap
 * test. The port runs the same move twice only when `stuck_test` is supplied
 * and reports that the resulting position is invalid; with no test it is one
 * plain `q2_move` — and with no test ANYWHERE, which is how this shipped, the
 * entity sweep is dead code and no mover can block anything.
 */
typedef bool (*q2_move_stuck_fn)(const q2_move_ent *ent, const void *user);

/* The gate above, in that shape; `user` is the q2_move_world. It is only read,
 * hence const: the callers pass the same world they hand `q2_move_checked`. */
bool q2_move_near_entity(const q2_move_ent *ent, const void *user);

int q2_move_checked(q2_collision *coll, q2_move_ent *ent, const s16 delta[3],
                    s32 iterations, bool ground_mode, bool push_mode,
                    q2_move_stuck_fn stuck_test, const void *user,
                    const q2_move_world *world);

/*
 * 0x8004583C — one frame of an entity's stepped movement.
 *
 * Lifts by the step height, slides by `delta`, then drops back by the same
 * amount plus however far the lift actually got. That third move is the one
 * that decides ground contact.
 *
 * Returns true when the entity finished the frame on the ground.
 */
bool q2_move_step(q2_collision *coll, q2_move_ent *ent, const s16 delta[3],
                  const q2_move_world *world);

/* 0x80045880: 216 unless the entity's flags carry 0x600, then 108. */
s32 q2_move_step_height(u32 flags);

#endif /* Q2PSX_TRACE_H */
