#include "aiworld.h"

#include "combat.h"   /* 0x800544EC's narrow phase, shared */

#include "worldscale.h"

#include <stddef.h>
#include <string.h>

#include "aimove.h"

/*
 * How much of the requested move actually happened, as a 1.12 fraction.
 *
 * The collision walk reports where it stopped, not how far it got, so the
 * fraction is recovered from the distance travelled along the dominant axis.
 * That is the same axis the walk itself clips on, so the answer agrees with
 * the original's to within the rounding of one divide — and the AI only ever
 * compares it against 4096 or against another fraction from the same source.
 */
static s32 travelled_fraction(const s32 start[3], const s32 end[3],
                              const s32 hit[3])
{
    s32 axis = 0;
    s32 best = 0;
    int i;
    s64 want, got;

    for (i = 0; i < 3; i++) {
        s32 d = end[i] - start[i];
        if (d < 0)
            d = -d;
        if (d > best) {
            best = d;
            axis = i;
        }
    }

    if (best == 0)
        return Q2_TRACE_ONE;

    want = (s64)end[axis] - start[axis];
    got  = (s64)hit[axis] - start[axis];

    if (want < 0) { want = -want; got = -got; }
    if (got <= 0)
        return 0;
    if (got >= want)
        return Q2_TRACE_ONE;

    return (s32)((got * Q2_TRACE_ONE) / want);
}

/* Is the caller's box a real one? 0x8005BDA8..0x8005BE30 compares all six
 * components against the shared zero vector at 0x8009FBE4; anything non-zero
 * takes the eroded hull. A NULL pointer is the same as a zero box. */
static bool box_is_real(const s16 mins[3], const s16 maxs[3])
{
    int i;

    if (!mins || !maxs)
        return false;
    for (i = 0; i < 3; i++) {
        if (mins[i] || maxs[i])
            return true;
    }
    return false;
}

/*
 * 0x800544EC, for a MOVE rather than for a shot.
 *
 * The narrow phase is combat.c's, shared rather than rewritten: a vertical
 * cylinder of the candidate's entity+0x94 radius around its origin, intersected
 * with the Y slab that runs upward from origin.y + 286 by entity+0x96
 * (0x800545F4, 0x80054650, 0x80054834). What is NOT shared is the filter — the
 * shot path rejects on `takedamage` because T_Damage does, and 0x800545E8 skips
 * only the ignore entity and nothing else.
 *
 * The mover is a POINT here, and that is the console's: 0x800544EC reads the
 * candidate's radius and slab and nothing of the caller's own box. It works
 * because the radius it reads is 286, the player's own half-extent, so a
 * creature that stops 286 from a player's centre stops at their skin.
 *
 * Returns the crossing point, which for a blocked step is where the creature
 * ends up. A start already inside gives fraction 0 and refuses the step
 * outright, which is what the sweep's own `along < 0 -> 0` clamp amounts to.
 */
static bool body_clip_segment(const s32 from[3], const s32 to[3],
                              const q2_move_body *o, s32 out[3])
{
    s32 dir[3];
    s64 enter = 0, leave = 0;
    int k;

    for (k = 0; k < 3; k++)
        dir[k] = to[k] - from[k];

    if (!dir[0] && !dir[1] && !dir[2])
        return false;
    if (!q2_combat_centre_is_ahead(from, dir, o->pos))
        return false;
    if (!q2_combat_cylinder_interval(from, dir, o->pos, o->sweep_radius,
                                     o->height, &enter, &leave))
        return false;
    if (leave <= 0 || enter > Q2_TRACE_SEG_ONE)
        return false;
    if (enter < 0)
        enter = 0;

    for (k = 0; k < 3; k++)
        out[k] = (s32)(from[k] + ((s64)dir[k] * enter) / Q2_TRACE_SEG_ONE);
    return true;
}

static void bound_trace(void *user, const s32 start[3], const s16 mins[3],
                        const s16 maxs[3], const s32 end[3],
                        const q2_monster *ignore, u32 mask, q2_ai_trace *out)
{
    q2_ai_world_bind *b = (q2_ai_world_bind *)user;
    q2_collision *hull;
    s32 pos[3];
    s32 node = -1;

    /*
     * `mask` is read for the one bit that matters — 0x02000000, which
     * SV_movestep sets (0x02020003 at 0x8005FE7C and 0x80060014) and which is
     * what turns the entity clip on. Behind it the console runs TWO passes:
     * the entity boxes (the doors and lifts, 0x80053974) and then the ACTOR
     * list (0x800544EC), and `ignore` belongs to the second. It used to be
     * dropped here with a note saying "CREATURES AND BODIES still do not clip:
     * the port has no per-creature box list to hand this"; it has one now, and
     * the second pass is at the end of this function.
     */

    memset(out, 0, sizeof(*out));

    if (b)
        b->stats.traces++;

    if (!b || !b->coll) {
        out->fraction  = Q2_TRACE_ONE;
        out->endpos[0] = end[0];
        out->endpos[1] = end[1];
        out->endpos[2] = end[2];
        return;
    }

    /*
     * THE HULL SELECT — 0x8005BD3C. A real box goes to SecondaryCol, which is
     * already eroded by the body's half-extent, so the swept point IS the box.
     * A degenerate one goes to PrimaryColl, the un-eroded query hull.
     */
    hull = b->coll;
    if (box_is_real(mins, maxs) && b->move_hull) {
        hull = b->move_hull;
        b->stats.trace_boxed++;
    }

    /*
     * `q2_coll_move` returns false when the move was STOPPED, not when it could
     * not begin — collision.h §0x80044C44 — and a stopped move is the normal,
     * useful answer for a walker's step trace, which exists precisely to be
     * stopped by the floor. `out_pos` is filled in either way; `out_node` is -1
     * only when the walk never found a cell at all, which is the real "started
     * outside the hull".
     *
     * Reading the false as start-solid is what made every creature in the game
     * stand still: `SV_movestep` bails on `allsolid` before it ever looks at
     * the fraction, so a creature standing on a floor was told it was buried in
     * one. Measured on BASE1, 1052 of 1052 traces took that arm.
     */
    if (!q2_coll_move(hull, start, end, -1, pos, &node) && node < 0) {
        /*
         * The eroded hull could not place the START. The original does not
         * retry — 0x8005BD3C hard-selects — but a creature whose body is
         * bigger than the erosion assumes (JAIL2's Gladiator misses the
         * SecondaryCol floor by two units) would otherwise freeze where it
         * previously walked. Fall back to the un-eroded hull and COUNT it, so
         * the placement failures stay visible instead of being papered over.
         */
        if (hull != b->coll &&
            (q2_coll_move(b->coll, start, end, -1, pos, &node) || node >= 0)) {
            b->stats.trace_fallback++;
        } else {
            b->stats.trace_unplaced++;
            out->fraction   = 0;
            out->startsolid = true;
            out->allsolid   = true;
            out->endpos[0]  = start[0];
            out->endpos[1]  = start[1];
            out->endpos[2]  = start[2];
            return;
        }
    }

    /*
     * AND THE DOORS — the entity arm of the same sweep.
     *
     * Clipped from the START to wherever the hull left the move, so the nearer
     * of the two wins with no fractions to compare. The creature's own box
     * inflates the mover boxes, which is the Minkowski sum that makes a swept
     * point stand for a swept body: without it a Gunner's centre would stop
     * flush against a door and half the Gunner would be inside it.
     *
     * The mask gate is the console's: a query that does not ask for entities
     * does not get them.
     */
    if (b->ents && (mask & Q2_MASK_ENTITY_BIT)) {
        q2_move_seg_hit mh;
        s32 body[3];
        int k;

        for (k = 0; k < 3; k++) {
            s32 lo = mins ? (s32)mins[k] : 0;
            s32 hi = maxs ? (s32)maxs[k] : 0;

            if (lo < 0) lo = -lo;
            if (hi < 0) hi = -hi;
            body[k] = lo > hi ? lo : hi;
        }

        if (q2_move_clip_segment(b->ents, start, pos, body, &mh)) {
            pos[0] = mh.pos[0];
            pos[1] = mh.pos[1];
            pos[2] = mh.pos[2];
            b->stats.trace_blocked_ent++;
        }
    }

    /*
     * AND THE OTHER BODIES — 0x8005BF4C, behind the same mask bit.
     *
     * Clipped from the START to wherever the hull and the doors left the move,
     * so the nearest of the three wins with no fractions to compare, exactly as
     * the door arm above does.
     *
     * `ignore` is honoured here and nowhere else in this function, which is why
     * it stopped being `(void)ignore`: 0x8005BF30 puts the caller's own entity
     * in a2 and 0x800545E8 skips it by pointer. Without that a creature's first
     * step would end on its own cylinder and no creature would ever move again.
     *
     * The cylinder's radius is the candidate's entity+0x94 — 286 for a player
     * — and NOT the +0x90 the separation pass reads. They are different fields
     * and the two routines read different ones: 0x800545F4 is `lh a0, 64(a2)`
     * with a2 = entity+84, which is +0x94, while 0x80051428 is `lh v1,
     * 144(a1)`, which is +0x90. 286 is the player's own half-extent, so a
     * creature that stops 286 from a player's centre stops at their skin.
     */
    if (b->bodies && b->bodies->list && (mask & Q2_MASK_ENTITY_BIT)) {
        s32 ignore_id = -1;
        u32 i;

        if (ignore && ignore == b->player_owner) ignore_id = -2;
        else if (ignore && b->body_owner_base && b->body_owner_count) {
            for (i = 0; i < b->body_owner_count; i++)
                if (ignore == &b->body_owner_base[i]) { ignore_id = (s32)i; break; }
        }

        for (i = 0; i < b->bodies->count; i++) {
            const q2_move_body *o = &b->bodies->list[i];
            q2_move_body corpse_body;
            s32 hit[3];

            if (o->id == ignore_id) continue;
            if (!o->solid) {
                const q2_monster *corpse;
                /* Medic import+0xF4 passes 0x06000003, whose extra bit
                 * includes dead actors. They remain non-solid to walkers. */
                if (!(mask & 0x04000000u) || !b->body_owner_base || o->id < 0 ||
                    (u32)o->id >= b->body_owner_count) continue;
                corpse = &b->body_owner_base[o->id];
                if (!corpse->in_use || !corpse->corpse || corpse->dissolve_arm) continue;
                corpse_body = *o;
                memcpy(corpse_body.pos, corpse->pos, sizeof(corpse_body.pos));
                corpse_body.sweep_radius = corpse->hit_radius;
                corpse_body.height = corpse->hit_height;
                o = &corpse_body;
            }
            if (!body_clip_segment(start, pos, o, hit))
                continue;

            pos[0] = hit[0];
            pos[1] = hit[1];
            pos[2] = hit[2];
            out->ent = o->id == -2 ? b->player_owner :
                       (o->id >= 0 && (u32)o->id < b->body_owner_count)
                         ? (q2_monster *)&b->body_owner_base[o->id] : NULL;
            b->stats.trace_blocked_body++;
        }
    }

    out->endpos[0] = pos[0];
    out->endpos[1] = pos[1];
    out->endpos[2] = pos[2];
    out->fraction  = travelled_fraction(start, end, pos);
    if (out->fraction >= Q2_TRACE_ONE)
        b->stats.trace_clear++;
}

static bool bound_los(void *user, const s32 a[3], const s32 b3[3])
{
    q2_ai_world_bind *b = (q2_ai_world_bind *)user;
    s32 pos[3];
    s32 node = -1;

    if (!b || !b->coll)
        return true;

    b->stats.los_calls++;

    if (!q2_coll_move(b->coll, a, b3, -1, pos, &node)) {
        b->stats.los_blocked++;
        return false;
    }

    /* Sight is all-or-nothing in the original: `visible` tests the fraction
     * against 1.0 and takes anything less as blocked. */
    if (pos[0] != b3[0] || pos[1] != b3[1] || pos[2] != b3[2]) {
        b->stats.los_blocked++;
        return false;
    }

    /*
     * AND THE DOORS. `visible` (0x8005B950) runs its sweep through the entity
     * list too, and a closed door is an entity — so a creature that could see
     * you through one was not being generous, it was asking the hull a
     * question the hull cannot answer.
     *
     * This is the line that fixes "creatures attack through doors": every
     * creature's fire hook gates each shot on `q2_visible`, so a sight line
     * that stops at the door stops the shooting with it. A POINT, not a box:
     * sight is the query the original hands a zero box, which is what sends it
     * to PrimaryColl in the first place.
     */
    if (b->ents) {
        q2_move_seg_hit mh;

        if (q2_move_clip_segment(b->ents, a, b3, NULL, &mh)) {
            b->stats.los_blocked++;
            b->stats.los_blocked_ent++;
            return false;
        }
    }

    return true;
}

/*
 * M_CheckBottom — 0x8005FB24, and it is FOUR corners, not one centre.
 *
 * 0x8005FB44..0x8005FC00 writes four stack triples,
 *
 *     (x - 143, y + 502, z - 143)   (x - 143, y + 502, z + 143)
 *     (x + 143, y + 502, z + 143)   (x + 143, y + 502, z - 143)
 *
 * and sweeps each of them FROM THE ENTITY ORIGIN — a0 = 0x800C8E90,
 * PrimaryColl, a1 = the entity, loop bound `slti v0, s2, 4` at 0x8005FC48. The
 * sweeps are DIAGONAL: the start stays at the centre and only the end moves to
 * the corner, so a wall between the two stops the sweep and counts as ground.
 * Four vertical drops at the corners would miss that, which is why the corner
 * offsets go on the END only.
 *
 * The two constants: 143 is half the body's 286 half-extent, and 502 is
 * Q2_EYE_BASE + Q2_STEPSIZE — one step below the FEET, measured from the
 * origin. That 502 is independent proof of the origin convention, since it
 * only lands on the floor if the origin is 286 above the feet.
 *
 * A corner whose sweep COMPLETES is not yet a ledge. 0x8005FC28 sends it to
 * 0x8005FC34 `jal 0x80053974` (a2 = 0 in the delay slot), the clip against the
 * 48-slot entity-box table at 0x800CAE10, which returns 0 when a box stops the
 * segment; 0x8005FC3C `bne v0, zero` then returns 0 — a ledge — only when no
 * box did. The port's table is `b->ents`, the sim's `move_world`, and its
 * entity half (q2_move_clip_segment skips the trigger volumes) holds movers,
 * GLASS, SHOOTTHEN, ROTHATCH, OBJDRAWOFF and intact FXGROUP parts. The last
 * three are registered by scenesolid.c, including health-zero explosives
 * which are solid without having damage callbacks. Freed boxes cease to
 * support a creature in the same call that destroys or hides their object.
 * Creatures, corpses and items use the separate body list, not this Scene-box
 * table, so this probe does not treat another creature as ground.
 */
#define AI_BOTTOM_CORNER  (Q2_EYE_BASE / 2)             /* 143 */

static bool bound_bottom(void *user, const q2_monster *m)
{
    static const s32 k_corner[4][2] = {
        { -AI_BOTTOM_CORNER, -AI_BOTTOM_CORNER },
        { -AI_BOTTOM_CORNER,  AI_BOTTOM_CORNER },
        {  AI_BOTTOM_CORNER,  AI_BOTTOM_CORNER },
        {  AI_BOTTOM_CORNER, -AI_BOTTOM_CORNER }
    };
    q2_ai_world_bind *b = (q2_ai_world_bind *)user;
    s32 start[3];
    int i;

    if (!b || !b->coll || !m)
        return true;

    b->stats.bottom_calls++;

    start[0] = m->pos[0];
    start[1] = m->pos[1];
    start[2] = m->pos[2];

    for (i = 0; i < 4; i++) {
        s32 end[3], pos[3];
        s32 node = -1;

        end[0] = start[0] + k_corner[i][0];
        /* Y points down, so probing for ground adds. From the ORIGIN, which is
         * why the reach is a step below the feet rather than a step. */
        end[1] = start[1] + Q2_EYE_BASE + b->bottom_reach;
        end[2] = start[2] + k_corner[i][1];

        /*
         * A probe that is STOPPED has found ground; one that runs the whole way
         * has found a drop. So the return of `q2_coll_move` is the answer,
         * inverted — the previous reading had it the right way round only by
         * accident, because it also treated "stopped" as failure and so
         * reported a drop under every creature standing on a floor.
         */
        if (!q2_coll_move(b->coll, start, end, -1, pos, &node)) {
            if (node < 0) {
                /* Never found a cell: the creature is outside the hull. */
                b->stats.bottom_fail++;
                return false;
            }
            continue;                   /* stopped short: this corner has ground */
        }

        /*
         * THE CORNER THAT REACHED THE END.
         *
         * 0x8005FB24 does not fail on it either: it hands the completed sweep
         * to 0x80053974, the ENTITY clip, and only that finding nothing makes
         * it a ledge. The entity list here is the sim's entity boxes — mover
         * parts, hatches and intact breakables, see above — so a creature standing
         * on a LIFT, in a doorway whose floor is the door itself, or on a pane
         * is standing on something instead of being told it is at the edge of
         * the world and refusing to walk.
         */
        if (b->ents) {
            q2_move_seg_hit mh;

            if (q2_move_clip_segment(b->ents, start, end, NULL, &mh)) {
                b->stats.bottom_on_ent++;
                continue;
            }
        }

        b->stats.bottom_fail++;
        return false;                   /* ran the whole way: a drop */
    }

    return true;
}

void q2_ai_world_bind_init(q2_ai_world_bind *bind, q2_collision *coll,
                           q2_collision *move_hull)
{
    if (!bind)
        return;

    memset(bind, 0, sizeof(*bind));

    bind->coll         = coll;
    bind->move_hull    = move_hull;
    bind->bottom_reach = Q2_STEPSIZE;

    bind->world.user          = bind;
    bind->world.trace         = bound_trace;
    bind->world.line_of_sight = bound_los;
    bind->world.check_bottom  = bound_bottom;
}

void q2_ai_world_bind_entities(q2_ai_world_bind *bind,
                               const q2_move_world *ents)
{
    if (!bind)
        return;

    /*
     * Stored as handed over, not tested for emptiness: `q2_sim_attach_movers`
     * REALLOCATES the target array and rewrites the count, so a binding that
     * decided at this moment that the world was empty would stay empty for the
     * rest of the level. The clip copes with a count of zero on its own.
     */
    bind->ents = ents;
}

void q2_ai_world_bind_bodies(q2_ai_world_bind *bind,
                             const q2_move_bodies *bodies,
                             const struct q2_monster *owner_base,
                             u32 owner_count)
{
    if (!bind)
        return;

    /* Stored as handed over, for the reason `_bind_entities` gives: the sim
     * rebuilds this list every tick and its count moves with the level. */
    bind->bodies           = bodies;
    bind->body_owner_base  = owner_base;
    bind->body_owner_count = owner_count;
}

void q2_ai_world_bind_install(q2_ai_world_bind *bind)
{
    if (!bind || !bind->coll) {
        q2_ai_set_world(NULL);
        return;
    }
    q2_ai_set_world(&bind->world);
}
