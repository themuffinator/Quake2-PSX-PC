#include "aimove.h"

#include <stdlib.h>
#include <string.h>

#include "trig.h"

/* ------------------------------------------------------------------------- */
/* The two engine services a step calls                                       */
/* ------------------------------------------------------------------------- */
static q2_link_fn  g_link;
static q2_touch_fn g_touch;
static void       *g_link_user;

void q2_ai_set_link_hooks(q2_link_fn link, q2_touch_fn touch, void *user)
{
    g_link      = link;
    g_touch     = touch;
    g_link_user = user;
}

/*
 * 0x8005C8C8 — copy the entity's position and facing into the object the
 * renderer and the collision world read. `what` is the original's second
 * argument: bit 0 the origin, bit 2 the yaw, bit 7 suppressing the second copy
 * the interpolator keeps. A port with no link stage can ignore it entirely,
 * which is why it is a hook rather than a call into the renderer.
 */
void q2_link_entity(q2_monster *m, u32 what)
{
    if (g_link && m)
        g_link(m, what, g_link_user);
}

void q2_touch_triggers(q2_monster *m)
{
    if (g_touch && m)
        g_touch(m, g_link_user);
}

/* ------------------------------------------------------------------------- */
/* M_ChangeYaw — 0x80060964                                                   */
/* ------------------------------------------------------------------------- */
/*
 * The one place a creature's facing changes, and the reason a monster turns at
 * a believable rate rather than snapping: the move is clamped to yaw_speed
 * every tick and always takes the short way round.
 */
bool q2_M_ChangeYaw(q2_monster *m)
{
    s32 current, ideal, move, raw, speed;

    if (!m)
        return false;

    current = q2_anglemod(m->angles[2]);
    ideal   = m->ideal_yaw;

    if (current == ideal)
        return false;

    move  = ideal - current;
    speed = m->yaw_speed;

    /*
     * The two bounds are NOT mirror images, and the original's asymmetry is
     * kept: it subtracts a full turn at `move >= 2048` but only adds one at
     * `move <= -2048`, so exactly -2047 goes the long way round where +2047
     * does not. `slti a1, 2048` and `slti a1, -2047` at 0x800609A4 and
     * 0x800609B8.
     */
    if (current < ideal) {
        if (move >= Q2_ANGLE_180)
            move -= Q2_ANGLE_360;
    } else {
        if (move < -Q2_ANGLE_180 + 1)
            move += Q2_ANGLE_360;
    }

    raw = move;

    if (move > 0) {
        if (move > speed)
            move = speed;
    } else {
        if (move < -speed)
            move = -speed;
    }

    /*
     * The PSX addition: a turn bigger than the creature's threshold hands off
     * to a callback and does NOT happen this tick. That is what lets a heavy
     * creature play a turn-in-place animation instead of pivoting.
     */
    if (m->bigturn_threshold && labs((long)raw) > m->bigturn_threshold
        && m->bigturn) {
        m->bigturn(m);
        return true;
    }

    m->angles[2] = (s16)q2_anglemod(current + move);
    q2_link_entity(m, 4);

    return true;
}

/* ------------------------------------------------------------------------- */
/* M_CheckBottom — 0x8005FB24                                                 */
/* ------------------------------------------------------------------------- */
bool q2_M_CheckBottom(q2_monster *m)
{
    const q2_ai_world *w = q2_ai_get_world();
    return w->check_bottom(w->user, m);
}

/* ------------------------------------------------------------------------- */
/* SV_movestep — 0x8005FC78                                                   */
/* ------------------------------------------------------------------------- */
/*
 * `move` is the world-space offset to attempt. Returns true when the creature
 * actually moved.
 *
 * Two entirely different bodies share the name: flyers and swimmers get a
 * direct move with a height correction, everything else gets the step-down
 * trace that lets a walker climb stairs.
 */
bool q2_SV_movestep(q2_monster *m, const s32 move[3], bool relink)
{
    const q2_ai_world *w = q2_ai_get_world();
    s32 oldorg[3], neworg[3], end[3], lifted[3];
    q2_ai_trace tr;
    s32 stepsize;
    bool has_move;
    bool recover_startsolid = false;

    if (!m || !move)
        return false;

    oldorg[0] = m->pos[0];
    oldorg[1] = m->pos[1];
    oldorg[2] = m->pos[2];
    has_move = move[0] != 0 || move[1] != 0 || move[2] != 0;

    /* --- flying and swimming ------------------------------------------- */
    if (m->flags & (Q2_FL_FLY | Q2_FL_SWIM)) {
        int i;

        /* Try one move with vertical motion, then one without. */
        for (i = 0; i < 2; i++) {
            neworg[0] = m->pos[0] + move[0];
            neworg[1] = m->pos[1] + move[1];
            neworg[2] = m->pos[2] + move[2];

            if (i == 0 && m->enemy) {
                s32 dz;

                if (!m->goalentity)
                    m->goalentity = m->enemy;

                dz = m->pos[1] - m->goalentity->pos[1];

                if (m->goalentity->client) {
                    /*
                     * Chasing a player: hold a band around head height rather
                     * than converging exactly, so a flyer bobs instead of
                     * riding on the player's scalp. Y points down, so "above"
                     * is the negative side.
                     */
                    if (dz < -480)
                        neworg[1] += 96;
                    if (!((m->flags & Q2_FL_SWIM) && m->on_ground == false
                          && (m->flags & Q2_FL_INWATER) == 0)) {
                        if (dz > -360)
                            neworg[1] -= 96;
                    }
                } else {
                    if (dz < -96)
                        neworg[1] += 96;
                    else if (dz < 0)
                        neworg[1] += dz;
                    else if (dz < 97)
                        neworg[1] -= dz;
                    else
                        neworg[1] -= 96;
                }
            }

            w->trace(w->user, m->pos, m->mins, m->maxs, neworg, m,
                     Q2_MASK_MONSTERSOLID, &tr);

            if (tr.fraction == Q2_TRACE_ONE) {
                m->pos[0] = tr.endpos[0];
                m->pos[1] = tr.endpos[1];
                m->pos[2] = tr.endpos[2];
                if (relink) {
                    q2_link_entity(m, 0x81);
                    q2_touch_triggers(m);
                }
                return true;
            }

            if (!m->enemy)
                break;
        }

        return false;
    }

    /* --- walking --------------------------------------------------------- */
    /*
     * THREE TRACES, NOT ONE — 0x8005FC78's walk arm, and getting this wrong is
     * what let a creature walk into a wall.
     *
     * What used to be here was real Quake II's shape: one trace from a step
     * above the DESTINATION down to a step below it. That asks "is there floor
     * in the destination column", and nothing at all about the path between
     * here and there. So a creature was teleported to `tr.endpos` whenever the
     * far side had ground, walls in between included.
     *
     * This executable does it in three, each through the same helper
     * (0x8005BD3C), and the middle one is the one that was missing:
     *
     *     0x8005FFA8   the LIFT: origin -> (x, y - step, z), in place
     *     0x80060068   the MOVE: the lift's endpos -> origin + move, at the
     *                  raised height. 0x80060070 compares its fraction against
     *                  4096 and `bne` to 0x8006030C with `addu v0, zero, zero`
     *                  in the delay slot — SV_movestep RETURNS FALSE. A step
     *                  that cannot travel horizontally is refused outright.
     *     0x800600C4   the DROP: the forward endpos -> origin + move + step
     *
     * and only then the fraction == 4096 "walked off the edge" test, the
     * origin copy from res+12/16/20 (0x80060268), and M_CheckBottom.
     */
    stepsize = (m->aiflags & Q2_AI_NOSTEP) ? Q2_STEPSIZE_NOSTEP : Q2_STEPSIZE;

    /* --- 1. the lift, in place ------------------------------------------- */
    {
        s32 lift[3];

        lift[0] = m->pos[0];
        lift[1] = m->pos[1] - stepsize;   /* Y is down, so "up" subtracts */
        lift[2] = m->pos[2];

        w->trace(w->user, m->pos, m->mins, m->maxs, lift, m,
                 Q2_MASK_MONSTERSOLID, &tr);

        /* 0x8005FFC4 sends a start-solid lift to the retail recovery arm. */
        if (tr.startsolid) {
            recover_startsolid = true;
        } else {
            if (tr.allsolid)
                return false;

            lifted[0] = tr.endpos[0];
            lifted[1] = tr.endpos[1];
            lifted[2] = tr.endpos[2];

            /*
             * 0x8005FFCC..0x8005FFE0 carries however much of the lift was
             * achieved into the wished Y. This only differs from simply
             * using the lift endpoint when move[1] is non-zero.
             */
            neworg[0] = m->pos[0] + move[0];
            neworg[1] = m->pos[1] + move[1]
                      + (lifted[1] - m->pos[1]);
            neworg[2] = m->pos[2] + move[2];
        }
    }

    /* A start-solid lift skips both ordinary traces. 0x8005FFC4 -> 600CC. */
    if (!recover_startsolid) {
        /* --- 2. the horizontal move, at the raised height ----------------
         * 0x8005FFE4..0x80060010 skips this trace when every move component
         * is zero. In that case the fraction gate below deliberately still
         * observes the LIFT result. */
        if (has_move) {
            end[0] = neworg[0];
            end[1] = neworg[1];
            end[2] = neworg[2];

            w->trace(w->user, lifted, m->mins, m->maxs, end, m,
                     Q2_MASK_MONSTERSOLID, &tr);
        }

        if (tr.allsolid || tr.startsolid)
            return false;

        /* 0x80060070: anything short of the whole way and the step is refused.
         * This is the gate the port never had, and the reason a creature could
         * push its body into geometry. */
        if (tr.fraction != Q2_TRACE_ONE)
            return false;

        if (has_move) {
            neworg[0] = tr.endpos[0];
            neworg[1] = tr.endpos[1];
            neworg[2] = tr.endpos[2];
        }

        /* --- 3. the drop ------------------------------------------------- */
        end[0] = neworg[0];
        end[1] = m->pos[1] + stepsize;
        end[2] = neworg[2];

        w->trace(w->user, neworg, m->mins, m->maxs, end, m,
                 Q2_MASK_MONSTERSOLID, &tr);

        /* 0x800600D8..0x800600E8 biases an ordinary landing one unit up. */
        tr.endpos[1] -= 1;

        if (tr.startsolid)
            recover_startsolid = true;
        else if (tr.allsolid)
            return false;
    }

    if (recover_startsolid) {
        /*
         * 0x800600EC..0x80060214 — retry without stepping. Retail first
         * traces origin -> origin+move and insists on an exact 4096 fraction.
         * A zero move bypasses that trace (the three component tests at
         * 0x80060128..0x80060150). It then drops from the unstepped wish
         * position to original-Y + stepsize. This arm serves a start-solid
         * result from either the lift or the ordinary drop.
         */
        neworg[0] = m->pos[0] + move[0];
        neworg[1] = m->pos[1] + move[1];
        neworg[2] = m->pos[2] + move[2];

        if (has_move) {
            w->trace(w->user, m->pos, m->mins, m->maxs, neworg, m,
                     Q2_MASK_MONSTERSOLID, &tr);
            if (tr.fraction != Q2_TRACE_ONE)
                return false;
        }

        end[0] = neworg[0];
        end[1] = m->pos[1] + stepsize;
        end[2] = neworg[2];
        w->trace(w->user, neworg, m->mins, m->maxs, end, m,
                 Q2_MASK_MONSTERSOLID, &tr);

        if (tr.allsolid || tr.startsolid)
            return false;
    }

    if (tr.fraction == Q2_TRACE_ONE) {
        /*
         * Walked off an edge. A creature whose floor was pulled out from under
         * it is allowed to fall anyway; everything else refuses the step,
         * which is what keeps monsters on ledges.
         */
        return (m->flags & Q2_FL_PARTIALGROUND) != 0;
    }

    m->pos[0] = tr.endpos[0];
    m->pos[1] = tr.endpos[1];
    m->pos[2] = tr.endpos[2];

    /* Check point traces down for dangling corners. */
    if (!q2_M_CheckBottom(m)) {
        if (m->flags & Q2_FL_PARTIALGROUND) {
            if (relink) {
                q2_link_entity(m, 0x81);
                q2_touch_triggers(m);
            }
            return true;
        }
        m->pos[0] = oldorg[0];
        m->pos[1] = oldorg[1];
        m->pos[2] = oldorg[2];
        return false;
    }

    if (m->flags & Q2_FL_PARTIALGROUND)
        m->flags = (u16)(m->flags & ~(unsigned)Q2_FL_PARTIALGROUND);

    m->on_ground = true;

    if (relink) {
        q2_link_entity(m, 0x81);
        q2_touch_triggers(m);
    }

    return true;
}

/* ------------------------------------------------------------------------- */
/* M_walkmove — 0x800607F8                                                    */
/* ------------------------------------------------------------------------- */
/*
 * The ground plane is XZ, so the move is (sin, 0, cos) scaled by the distance.
 * The original reads a packed {sin, cos} table at 0x800A5430, 4096 entries of
 * four bytes; the port's sine table is the same function.
 */
bool q2_M_walkmove(q2_monster *m, s32 yaw, s32 dist)
{
    s32 move[3];
    s32 a;

    if (!m)
        return false;

    a = q2_anglemod(yaw);

    move[0] = (s32)(((s64)q2_sin12(a) * dist + 4095) >> 12);
    move[1] = 0;
    move[2] = (s32)(((s64)q2_cos12(a) * dist + 4095) >> 12);

    return q2_SV_movestep(m, move, true);
}

/* ------------------------------------------------------------------------- */
/* SV_CloseEnough — inline in M_MoveToGoal at 0x800608A8                      */
/* ------------------------------------------------------------------------- */
bool q2_SV_CloseEnough(const q2_monster *self, const q2_monster *goal, s32 dist)
{
    int i;

    if (!self || !goal)
        return false;

    for (i = 0; i < 3; i++) {
        s32 gmin = goal->pos[i] + goal->mins[i];
        s32 gmax = goal->pos[i] + goal->maxs[i];
        s32 smin = self->pos[i] + self->mins[i];
        s32 smax = self->pos[i] + self->maxs[i];

        if (gmin > smax + dist)
            return false;
        if (gmax < smin - dist)
            return false;
    }

    return true;
}

/* ------------------------------------------------------------------------- */
/* M_MoveToGoal — 0x8006087C                                                  */
/* ------------------------------------------------------------------------- */
void q2_M_MoveToGoal(q2_monster *m, s32 dist)
{
    q2_monster *goal;

    if (!m)
        return;

    goal = m->goalentity;

    /* If the next step hits the enemy, return immediately: a monster that is
     * already touching you should be swinging, not shuffling. */
    if (m->enemy && q2_SV_CloseEnough(m, m->enemy, dist))
        return;

    /*
     * Bump around. The coin flip is the whole of the original's obstacle
     * avoidance: one tick in four it refuses to walk straight even when it
     * could, which shakes a creature out of a corner it would otherwise press
     * against forever.
     */
    if ((rand() & 3) == 1 || !q2_SV_StepDirection(m, m->ideal_yaw, dist)) {
        if (m->spawnflags & Q2_SVFLAG_INUSE)
            q2_SV_NewChaseDir(m, goal, dist);
    }
}

/* ------------------------------------------------------------------------- */
/* SV_StepDirection — 0x80060334                                              */
/* ------------------------------------------------------------------------- */
bool q2_SV_StepDirection(q2_monster *m, s32 yaw, s32 dist)
{
    s32 move[3], oldorigin[3];
    s32 delta, a;

    if (!m)
        return false;

    m->ideal_yaw = (s16)yaw;
    q2_M_ChangeYaw(m);

    a = q2_anglemod(yaw);
    move[0] = (s32)(((s64)q2_sin12(a) * dist + 4095) >> 12);
    move[1] = 0;
    move[2] = (s32)(((s64)q2_cos12(a) * dist + 4095) >> 12);

    oldorigin[0] = m->pos[0];
    oldorigin[1] = m->pos[1];
    oldorigin[2] = m->pos[2];

    if (q2_SV_movestep(m, move, false)) {
        delta = m->angles[2] - m->ideal_yaw;

        /*
         * Not turned far enough, so do not take the step. This is the rule
         * that stops a monster from crabbing sideways along a wall while
         * still facing you — it moves only once its body has caught up with
         * the direction it chose. 513..3583 is "more than 45 degrees".
         */
        if ((u32)(delta - 513) < 3071u) {
            m->pos[0] = oldorigin[0];
            m->pos[1] = oldorigin[1];
            m->pos[2] = oldorigin[2];
        }

        q2_link_entity(m, 0x85);
        q2_touch_triggers(m);
        return true;
    }

    q2_link_entity(m, 0x85);
    q2_touch_triggers(m);
    return false;
}

/* ------------------------------------------------------------------------- */
/* SV_NewChaseDir — 0x80060544                                                */
/* ------------------------------------------------------------------------- */
/*
 * Eight-way pathing with no path data at all: quantise the current heading,
 * work out which way the target lies on each ground axis, and try directions
 * in a fixed order of preference until one produces a step.
 *
 * Yaw 0 faces +Z, so +X is 1024 and -X is 3072, which is why the axis
 * constants below do not look like id's 0 and 180.
 */
void q2_SV_NewChaseDir(q2_monster *actor, q2_monster *enemy, s32 dist)
{
    s32 deltax, deltaz;
    s32 d[3];
    s32 tdir, olddir, turnaround;

    if (!actor || !enemy)
        return;

    olddir     = q2_anglemod((actor->ideal_yaw / Q2_CHASE_STEP)
                             * Q2_CHASE_STEP);
    turnaround = q2_anglemod(olddir - Q2_ANGLE_180);

    deltax = enemy->pos[0] - actor->pos[0];
    deltaz = enemy->pos[2] - actor->pos[2];

    if (deltax > Q2_CHASE_DEADBAND)
        d[1] = Q2_ANGLE_90;                 /* +X */
    else if (deltax < -Q2_CHASE_DEADBAND)
        d[1] = Q2_ANGLE_90 * 3;             /* -X */
    else
        d[1] = Q2_DI_NODIR;

    if (deltaz < -Q2_CHASE_DEADBAND)
        d[2] = Q2_ANGLE_180;                /* -Z */
    else if (deltaz > Q2_CHASE_DEADBAND)
        d[2] = 0;                           /* +Z */
    else
        d[2] = Q2_DI_NODIR;

    /* Try the direct diagonal. */
    if (d[1] != Q2_DI_NODIR && d[2] != Q2_DI_NODIR) {
        if (d[1] == Q2_ANGLE_90)
            tdir = (d[2] != 0) ? 1536 : 512;
        else
            tdir = (d[2] != 0) ? 2560 : 3584;

        if (tdir != turnaround && q2_SV_StepDirection(actor, tdir, dist))
            return;
    }

    /* Try the two axes, in an order a coin flip and the larger delta decide. */
    if ((rand() & 1) || labs((long)deltaz) > labs((long)deltax)) {
        tdir = d[1];
        d[1] = d[2];
        d[2] = tdir;
    }

    if (d[1] != Q2_DI_NODIR && d[1] != turnaround
        && q2_SV_StepDirection(actor, d[1], dist))
        return;

    if (d[2] != Q2_DI_NODIR && d[2] != turnaround
        && q2_SV_StepDirection(actor, d[2], dist))
        return;

    /* No direct path, so keep going the way we already were. */
    if (olddir != Q2_DI_NODIR && q2_SV_StepDirection(actor, olddir, dist))
        return;

    /* Then everything else, sweeping in a randomly chosen rotation. */
    if (rand() & 1) {
        for (tdir = 0; tdir <= 3584; tdir += Q2_CHASE_STEP)
            if (tdir != turnaround && q2_SV_StepDirection(actor, tdir, dist))
                return;
    } else {
        for (tdir = 3584; tdir >= 0; tdir -= Q2_CHASE_STEP)
            if (tdir != turnaround && q2_SV_StepDirection(actor, tdir, dist))
                return;
    }

    /* Only now is doubling back allowed. */
    if (turnaround != Q2_DI_NODIR
        && q2_SV_StepDirection(actor, turnaround, dist))
        return;

    actor->ideal_yaw = (s16)olddir;     /* can't move */

    /*
     * If a bridge was pulled out from underneath a creature it may have no
     * valid standing position at all, and marking it partially grounded is
     * what lets SV_movestep drop it rather than freezing it in the air.
     */
    if (!q2_M_CheckBottom(actor))
        actor->flags |= Q2_FL_PARTIALGROUND;
}
