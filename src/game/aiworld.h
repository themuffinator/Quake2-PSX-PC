/*
 * aiworld.h — binding the creature AI to the real collision model.
 *
 * The AI asks the world three questions and nothing else: can this creature
 * see that one, where does a box move end up, and is there ground under my
 * feet. `ai.h` takes those as function pointers so the behaviour can be
 * exercised without a disc; this is the implementation that answers them out
 * of a level's own hull.
 *
 * The three map onto routines the original already has, which is why the
 * binding is thin rather than a reimplementation:
 *
 *   line_of_sight   `visible` (0x8005B950) ends in a swept move through the
 *                   collision graph at 0x80044C44 against PrimaryColl
 *                   (0x800C8E90) — `q2_coll_move` here.
 *   trace           the same walk, but against the OTHER hull; see below.
 *   check_bottom    M_CheckBottom (0x8005FB24) — FOUR corner sweeps, not one
 *                   probe under the centre.
 *
 * ---------------------------------------------------------------------------
 * WHICH HULL, and the two mistakes that came of getting it wrong
 * ---------------------------------------------------------------------------
 * This header used to say the AI's mins/maxs are "advisory", because
 * SecondaryCol is PrimaryColl already eroded by the mover's box. The design
 * statement was right and the WIRING contradicted it: bound_trace discarded the
 * box it was handed and swept a bare point through PrimaryColl, the UN-eroded
 * hull. A creature's centre was therefore allowed to travel until the CENTRE
 * touched the wall, i.e. up to its own half-extent embedded in it.
 *
 * The original picks per call. 0x8005BD3C compares the caller's mins and maxs
 * against the shared zero vector at 0x8009FBE4 and branches:
 *
 *     0x8005BE4C  addiu s0, 0x800C8FE8   SecondaryCol, when the box is real
 *     0x8005BEB8  addiu s0, 0x800C8E90   PrimaryColl,  when it is zero or NULL
 *
 * and SV_movestep (0x8005FC78) always passes a real box — 0x8005CCF4 fills it
 * from obj+0x6C..0x76 before the call — so a walking creature is a BOX in the
 * eroded hull. Sight and the ground probe pass no box and stay on PrimaryColl.
 *
 * The second mistake is what made the first one look right. A Population spawn
 * record's Y is the FEET, and every consumer here treats the creature's `pos`
 * as the entity ORIGIN, 286 above them (worldscale.h's Q2_EYE_BASE; the same
 * lift sim.c already applies to the player and item.c to an item). With the
 * feet value fed to the eroded hull, 214 of 214 creatures resolved to no cell
 * at all — a measurement that was sound, was recorded, and answered the wrong
 * question. Lifted, 21 of 22 in-zone spawn records on four maps are inside
 * SecondaryCol. The two changes are only correct together.
 *
 * A creature whose zone has no collision loaded falls back to the open world,
 * which is the same failure the stand-in has and is visible rather than silent.
 */
#ifndef Q2PSX_AIWORLD_H
#define Q2PSX_AIWORLD_H

#include "ai.h"
#include "collision.h"
#include "q2psx.h"
#include "trace.h"

/*
 * What the binding did, so a creature that will not move is a measurement
 * rather than a guess.
 *
 * `trace_unplaced` is the one that matters: `q2_coll_move` reports failure when
 * it cannot find a cell for the START point, and a walker's step trace starts
 * from a point the creature is not standing at. If nearly every trace lands
 * there, the fault is in placing the start, not in the geometry.
 */
typedef struct q2_ai_world_stats {
    u32 traces;
    u32 trace_unplaced;     /* the start point resolved to no cell        */
    u32 trace_clear;        /* ran the whole way: "walked off an edge"    */
    u32 trace_boxed;        /* took the eroded hull, i.e. a real box      */
    u32 trace_fallback;     /* the eroded hull could not place the start  */
    u32 bottom_calls;
    u32 bottom_fail;
    u32 los_calls;
    u32 los_blocked;
    u32 los_blocked_ent;    /* by a DOOR rather than by the hull          */
    u32 trace_blocked_ent;  /* a step trace a mover cut short             */
    u32 trace_blocked_body; /* ...and one another BODY cut short          */
    u32 bottom_on_ent;      /* a corner whose ground was a lift, not floor*/
} q2_ai_world_stats;

typedef struct q2_ai_world_bind {
    /*
     * BOTH hulls, because 0x8005BD3C selects between them per call.
     *
     * `coll` is PrimaryColl — sight, the ground probe, and any trace whose box
     * is degenerate. `move_hull` is SecondaryCol, PrimaryColl eroded by the
     * body's own half-extent, and is what a walking creature's box moves in.
     * A NULL `move_hull` degrades to `coll`, which is the old behaviour and is
     * counted rather than silent.
     */
    q2_collision *coll;
    q2_collision *move_hull;

    /*
     * THE DOORS, and neither hull contains them.
     *
     * PrimaryColl and SecondaryCol are the map's static geometry; a door or a
     * lift is a Scene node bound to a runtime object whose box lives in the
     * entity table (trace.h). Without this the three questions are all
     * answered as though every door in the level were open — a creature sees
     * you through a shut one, shoots you through it, and walks into it.
     *
     * The original does not have this gap because it never separated the two:
     * `visible` clips its sight line against the entity list, and SV_movestep
     * passes MASK_MONSTERSOLID (0x02020003), whose 0x02000000 bit is what
     * turns the entity clip on at 0x800544EC.
     *
     * Borrowed and may be NULL, in which case the binding behaves as it did
     * before and the `*_ent` counters stay at zero.
     */
    const q2_move_world *ents;

    /*
     * AND THE OTHER BODIES, which are not in that list either.
     *
     * `0x8005BD3C` runs the hull walk, then the entity-box clip, and then —
     * gated on the caller's mask — the ACTOR sweep:
     *
     *     8005BF18  lui  v0, 0x0200
     *     8005BF1C  and  v0, s6, v0        ; the mask
     *     8005BF20  beq  v0, zero, +0x34   ; no bit, no actors
     *     8005BF30  addu a2, s3, zero      ; the entity to IGNORE
     *     8005BF38  lw   v0, 0x800B2B90    ; the actor list, begin
     *     8005BF40  lw   v1, 0x800B2B98    ; ...and end
     *     8005BF4C  jal  0x800544EC        ; the sweep
     *
     * and `SV_movestep` passes MASK_MONSTERSOLID, 0x02020003, on every walking
     * and flying step (aimove.c). So a creature is stopped by the player and by
     * other creatures on the console, and this port's `bound_trace` dropped
     * `ignore` on the floor and clipped doors and nothing else — which is the
     * whole of "monsters don't collide with players" from the creature's side.
     *
     * The list is the sim's actor list (trace.h's `q2_move_bodies`), the same
     * one the player's separation pass runs over, so the two halves cannot
     * disagree about who is solid. Borrowed; NULL behaves as before.
     *
     * `ignore` arrives as a `q2_monster *`, and the handles in the list are the
     * client's indices into its own creature set — so the binding is told where
     * that set starts and turns the pointer into an index.
     */
    const q2_move_bodies    *bodies;
    const struct q2_monster *body_owner_base;   /* the creature set's array   */
    u32                      body_owner_count;
    struct q2_monster       *player_owner; /* body id -2, the AI sight client */

    /* How far below a creature ground may be and still count. The step height,
     * because a creature that can climb a step can also stand off one. */
    s32 bottom_reach;

    q2_ai_world_stats stats;

    /* Filled in by the binder; pass this to q2_ai_set_world. */
    q2_ai_world world;
} q2_ai_world_bind;

/*
 * Point `bind` at a zone's hulls and install it as the AI's world.
 *
 * The binding borrows both; the caller keeps ownership and must not free them
 * while creatures are thinking. Passing a NULL `coll` installs the open world.
 * `move_hull` may be NULL, in which case every trace uses `coll`.
 */
void q2_ai_world_bind_init(q2_ai_world_bind *bind, q2_collision *coll,
                           q2_collision *move_hull);

/*
 * Give the binding the sim's move world, so sight, movement and the ground
 * probe all stop at a closed door.
 *
 * Separate from `_init` because the two are established at different times: the
 * hulls come with the zone and the mover boxes come after the event script has
 * been walked and the movers built. Pass the sim's `&sim->move_world`, whose
 * address is stable across the reallocation `q2_sim_attach_movers` does to the
 * target array itself.
 */
void q2_ai_world_bind_entities(q2_ai_world_bind *bind,
                               const q2_move_world *ents);

/*
 * Give the binding the sim's ACTOR list, so a creature is stopped by the player
 * and by other creatures — the arm at 0x8005BF4C above.
 *
 * `owner_base` is the creature set's `monsters` array and `owner_count` its
 * length; a body whose handle is in `[0, owner_count)` belongs to
 * `owner_base[handle]`, which is how `ignore` is recognised. Pass NULL for a
 * caller with no creature set, and the sweep will simply never ignore anything
 * — which is right for a caller whose only bodies are players.
 */
void q2_ai_world_bind_bodies(q2_ai_world_bind *bind,
                             const q2_move_bodies *bodies,
                             const struct q2_monster *owner_base,
                             u32 owner_count);

void q2_ai_world_bind_install(q2_ai_world_bind *bind);

#endif /* Q2PSX_AIWORLD_H */
