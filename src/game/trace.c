#include "trace.h"

#include <string.h>

#include "weapon.h"      /* q2_rng_next — the separation pass's coincident draw */
#include "worldscale.h"

bool q2_move_step(q2_collision *coll, q2_move_ent *ent, const s16 delta[3],
                  const q2_move_world *world, const q2_move_bodies *bodies,
                  s32 self_id, const s16 mins[3], const s16 maxs[3], q2_rng *rng)
{
    return q2_move_actor(coll, ent, delta, world, bodies, self_id, mins, maxs,
                          rng, false);
}

void q2_move_ground_project(q2_move_ent *ent, s16 velocity[3])
{
    s32 ny = ent->ground_normal[1];
    if (ny <= ent->max_slope_ny || !ny || !(ent->flags & 0x2060u) ||
        (ent->flags & 0x980u)) return;
    /* addu/subu wrap before the signed division. */
    velocity[1] = (s16)((s32)(0u -
        ((u32)((s32)velocity[0] * ent->ground_normal[0]) +
         (u32)((s32)velocity[2] * ent->ground_normal[2]))) / ny);
}

static s16 move_ease(s16 value, s32 target, s32 step)
{
    s32 v = value;
    if (v < target) { v += step; if (v > target) v = target; }
    else { v -= step; if (v < target) v = target; }
    return (s16)v;
}

void q2_move_integrate(q2_move_ent *ent, s16 v[3], s16 impulse[3],
                       u32 *flags, s32 gravity, s32 dt, bool multiplayer,
                       s16 delta[3])
{
    s32 dv, dvdt, half;
    int k;
    delta[0] = (s16)((s32)((u32)(s32)v[0] * (u32)dt) / Q2_VEL_DIV);
    delta[2] = (s16)((s32)((u32)(s32)v[2] * (u32)dt) / Q2_VEL_DIV);
    if ((*flags & 0x1000u) || (ent->flags & 0x180u) ||
        ((ent->flags & 0x800u) && !(*flags & 0x4000u))) {
        delta[1] = (s16)((s32)((u32)(s32)v[1] * (u32)dt) / Q2_VEL_DIV);
        ent->flags &= ~Q2_ENT_GROUNDED_MASK;
        for (k = 0; k < 3; k++) { v[k] = (s16)(v[k] + impulse[k]); impulse[k] = 0; }
        return; /* 0x80045F44 clears the halfwords, but leaves 0x4000 armed. */
    }
    dv = (s32)((u32)gravity * (u32)dt);
    dvdt = (s32)((u32)dv * (u32)dt);
    half = dvdt / 2;
    delta[1] = (s16)((s32)((u32)(s32)v[1] * (u32)dt + (u32)half) / Q2_VEL_DIV);
    v[1] = (s16)((u32)(s32)v[1] + (u32)dv);
    if (ent->flags & Q2_ENT_LIQUID_FLOAT)
        v[1] = move_ease(v[1], Q2_LIQUID_FLOAT_VY, dt * Q2_LIQUID_EASE_RATE);
    else if (ent->flags & Q2_ENT_LIQUID_SINK) {
        if (v[1] < Q2_LIQUID_SINK_VY) v[1] = (s16)(v[1] - dt * Q2_LIQUID_SINK_PUSH);
        else v[1] = move_ease(v[1], Q2_LIQUID_SINK_VY, dt * Q2_LIQUID_EASE_RATE);
    } else if (v[1] > Q2_TERMINAL_VY) v[1] = Q2_TERMINAL_VY;
    if (!(*flags & 0x4000u)) return;
    ent->flags &= ~0x2860u;
    memset(ent->ground_normal, 0, sizeof(ent->ground_normal));
    ent->last_normal[0] = ent->last_normal[2] = 0;
    ent->last_normal[1] = 4096;
    *flags &= ~0x4000u;
    for (k = 0; k < 3; k++) {
        v[k] = (s16)(v[k] + impulse[k]);
        if (k == 1 && !multiplayer && v[k] < Q2_IMPULSE_CEILING)
            v[k] = Q2_IMPULSE_CEILING;
        delta[k] = (s16)((s32)((u32)(s32)v[k] * (u32)dt) / Q2_VEL_DIV);
        impulse[k] = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Helpers in the original's arithmetic                                       */
/* ------------------------------------------------------------------------- */

/* 0x8004545C: differences are taken between low halfwords, so they wrap. */
static s16 sub16(s32 a, s32 b)
{
    return (s16)(u16)((u16)((u32)a & 0xFFFFu) - (u16)((u32)b & 0xFFFFu));
}

/* ------------------------------------------------------------------------- */
/* 0x8005625C — the unstick nudge                                             */
/* ------------------------------------------------------------------------- */
/*
 * Six cases, chosen by a running maximum over +nx, -nx, +ny, -ny, +nz, -nz in
 * that order, each selecting a unit vector from the table at 0x800AEA64:
 *
 *     0x800AEA64  ( 1, 0, 0)      0x800AEA6C  (-1, 0, 0)
 *     0x800AEA74  ( 0, 1, 0)      0x800AEA7C  ( 0,-1, 0)
 *     0x800AEA84  ( 0, 0, 1)      0x800AEA8C  ( 0, 0,-1)
 *
 * The comparisons are strict (`slt`), so a tie keeps the earlier axis — which
 * is why an exactly diagonal normal nudges along X rather than splitting.
 */
void q2_move_push_vector(const s16 normal[3], s16 out[3])
{
    static const s16 axis[6][3] = {
        { 1, 0, 0}, {-1, 0, 0},
        { 0, 1, 0}, { 0,-1, 0},
        { 0, 0, 1}, { 0, 0,-1}
    };
    s32 best;
    int which = 0;

    if (!out)
        return;

    out[0] = out[1] = out[2] = 0;
    if (!normal)
        return;

    best = normal[0];
    if (best < -(s32)normal[0]) { best = -(s32)normal[0]; which = 1; }
    if (best <  (s32)normal[1]) { best =  (s32)normal[1]; which = 2; }
    if (best < -(s32)normal[1]) { best = -(s32)normal[1]; which = 3; }
    if (best <  (s32)normal[2]) { best =  (s32)normal[2]; which = 4; }
    if (best < -(s32)normal[2]) {                         which = 5; }

    out[0] = axis[which][0];
    out[1] = axis[which][1];
    out[2] = axis[which][2];
}

/* ------------------------------------------------------------------------- */
/* 0x80052C70 — swept point against one inflated box                          */
/* ------------------------------------------------------------------------- */
bool q2_move_box_overlap(const s32 move_min[3], const s32 move_max[3],
                         const s32 box_min[3], const s32 box_max[3])
{
    int i;

    if (!move_min || !move_max || !box_min || !box_max)
        return false;

    /* 0x80052CB8..0x80052D44, in the original's own axis order. */
    for (i = 0; i < 3; i++) {
        if (move_max[i] < box_min[i]) return false;
        if (box_max[i] < move_min[i]) return false;
    }

    return true;
}

bool q2_move_sweep_box(const s32 pos[3], const s16 delta[3],
                       const s32 box_min[3], const s32 box_max[3],
                       s16 other_dy, s32 half_extent,
                       s32 out_pos[3], s16 out_normal[3])
{
    /*
     * axis, "use the low face", and the normal, in the original's test order.
     * The normals are the six axis vectors at 0x800AEA2C, 8 bytes apart.
     */
    static const struct { int a, b, c; int low; s16 n[3]; } face[6] = {
        { 1, 0, 2, 1, { 0,  4096,  0} },   /* 0x800AEA2C */
        { 1, 0, 2, 0, { 0, -4096,  0} },   /* 0x800AEA34 */
        { 2, 0, 1, 1, { 0,  0,  4096} },   /* 0x800AEA3C */
        { 2, 0, 1, 0, { 0,  0, -4096} },   /* 0x800AEA44 */
        { 0, 1, 2, 1, { 4096,  0,  0} },   /* 0x800AEA4C */
        { 0, 1, 2, 0, {-4096,  0,  0} }    /* 0x800AEA54 */
    };
    s32 lo[3], hi[3];
    s32 d[3];
    int i, f;

    if (!pos || !delta || !box_min || !box_max)
        return false;

    /* The Minkowski sum: the target's box grown by the mover's half-extent. */
    for (i = 0; i < 3; i++) {
        lo[i] = box_min[i] - half_extent;
        hi[i] = box_max[i] + half_extent;
        d[i]  = delta[i];
    }

    /* Only the vertical axis is made relative to the target's own motion. */
    d[1] -= other_dy;

    for (f = 0; f < 6; f++) {
        int a = face[f].a, b = face[f].b, c = face[f].c;
        s32 plane = face[f].low ? lo[a] : hi[a];
        s32 pa    = pos[a] + ((a == 1) ? (s32)other_dy : 0);
        s32 gap, tb, tc;

        if (face[f].low) {
            if (plane < pa) continue;   /* already past the face   */
            if (d[a] < 0)   continue;   /* heading away from it    */
        } else {
            if (pa < plane) continue;
            if (d[a] > 0)   continue;
        }

        gap = plane - pa;

        /*
         * Both offsets are `d[other] * gap / d[a]`, truncating toward zero. A
         * move exactly parallel to the face (d[a] == 0) takes the zero branch
         * and can still report a contact — the original does that too, and the
         * caller's AABB gate is what keeps it from mattering.
         */
        tb = (d[a] != 0) ? (s32)(((s64)d[b] * gap) / d[a]) : 0;
        if (tb < lo[b] - pos[b] || hi[b] - pos[b] < tb)
            continue;

        tc = (d[a] != 0) ? (s32)(((s64)d[c] * gap) / d[a]) : 0;
        if (tc < lo[c] - pos[c] || hi[c] - pos[c] < tc)
            continue;

        if (out_pos) {
            out_pos[a] = plane;
            out_pos[b] = pos[b] + tb;
            out_pos[c] = pos[c] + tc;
        }
        if (out_normal) {
            out_normal[0] = face[f].n[0];
            out_normal[1] = face[f].n[1];
            out_normal[2] = face[f].n[2];
        }

        return true;
    }

    return false;
}

/* ------------------------------------------------------------------------- */
/* 0x80053C58 — sweep a move against every target                             */
/* ------------------------------------------------------------------------- */
int q2_move_sweep_world(const q2_move_world *w, const s32 pos[3],
                        const s32 dest[3], q2_move_contact *out)
{
    s32 cur[3];
    int result = 1;
    u32 i;
    int k;

    if (out) {
        memset(out, 0, sizeof(*out));
        out->kind = Q2_MOVE_KIND_WORLD;
        out->id   = -1;
    }

    if (!pos || !dest)
        return 1;

    cur[0] = dest[0]; cur[1] = dest[1]; cur[2] = dest[2];

    if (w && w->targets) {
        for (i = 0; i < w->count; i++) {
            const q2_move_target *t = &w->targets[i];
            s32 mmin[3], mmax[3];
            s32 hit_pos[3];
            s16 hit_n[3];
            s16 delta[3];

            if (!t->active)
                continue;               /* 0x80053E20, byte +54 of the slot */

            /*
             * THE MASK TEST BELONGS TO THE VOLUME ARM ONLY.
             *
             * 0x80053C58 walks the 48-slot ENTITY table first and the trigger
             * volumes second, and the mask compare at 0x80054050 is inside the
             * second walk. An entity — a door, a lift, a crusher — is solid to
             * anything that reaches it; a volume is not, unless the mover's own
             * mask says so.
             *
             * Written as one shared test with `t->mask && ...`, a volume whose
             * flags halfword is ZERO fell through the guard and blocked. That
             * was harmless only for as long as the sweep was unreachable
             * (nothing armed it). BASE1 zone 0 has three such volumes — one of
             * them a 728 x 1186 x 980 box straddling a corridor — so arming the
             * sweep without fixing this would have shipped three invisible
             * walls.
             */
            if (t->kind == Q2_MOVE_KIND_VOLUME &&
                !(t->mask & w->mask))
                continue;

            /* The move's AABB, rebuilt each iteration because a contact moves
             * the destination (0x80053EA8 onward). */
            for (k = 0; k < 3; k++) {
                s32 lo = pos[k] < cur[k] ? pos[k] : cur[k];
                s32 hi = pos[k] < cur[k] ? cur[k] : pos[k];

                mmin[k] = lo - w->half_extent;
                mmax[k] = hi + w->half_extent;
                delta[k] = sub16(cur[k], pos[k]);
            }

            if (!q2_move_box_overlap(mmin, mmax, t->min, t->max))
                continue;

            if (!q2_move_sweep_box(pos, delta, t->min, t->max, t->dy,
                                   w->half_extent, hit_pos, hit_n))
                continue;

            result = 0;
            cur[0] = hit_pos[0];
            cur[1] = hit_pos[1];
            cur[2] = hit_pos[2];

            if (out) {
                out->hit = true;
                out->kind = t->kind;
                out->id   = t->id;
                out->normal[0] = hit_n[0];
                out->normal[1] = hit_n[1];
                out->normal[2] = hit_n[2];
            }
        }
    }

    if (out) {
        out->pos[0] = cur[0];
        out->pos[1] = cur[1];
        out->pos[2] = cur[2];
    }

    return result;
}

/* ------------------------------------------------------------------------- */
/* 0x80050CE0 — which volume is the entity in                                 */
/* ------------------------------------------------------------------------- */
/*
 * 0x80050CE0's ENTITY HALF — the 48-slot walk at 0x80050D7C, which runs before
 * the volume walk q2_move_contents reproduces and is skipped whole when a3 is
 * zero.
 *
 * Same box as the contents query (286 / 285 / 286 half-extents); `envelope` is
 * the caller's a2, and it chooses which of the slot's TWO boxes is tested —
 * 0x80050D94 reads slot+0x20, the +0x18..+0x2C swept envelope, and 0x80050E04
 * reads slot+0x00, the live box. The two public entry points below are the
 * game's two call sites, and neither is the other's optimisation: see trace.h.
 */
static bool overlaps_entity_slots(const q2_move_world *w, const s32 pos[3],
                                  bool envelope)
{
    s32 lo[3], hi[3];
    u32 i;

    if (!w || !w->targets || !pos)
        return false;

    lo[0] = pos[0] - Q2_CONTENTS_HALF_XZ;
    lo[1] = pos[1] - Q2_CONTENTS_HALF_Y;
    lo[2] = pos[2] - Q2_CONTENTS_HALF_XZ;
    hi[0] = pos[0] + Q2_CONTENTS_HALF_XZ;
    hi[1] = pos[1] + Q2_CONTENTS_HALF_Y;
    hi[2] = pos[2] + Q2_CONTENTS_HALF_XZ;

    for (i = 0; i < w->count; i++) {
        const q2_move_target *t = &w->targets[i];

        /* 0x80050D7C's `lbu v0, 50(a0)` on a 64-byte slot: the table is walked
         * whole and the inactive slots are skipped, not compacted away. */
        if (!t->active || t->kind != Q2_MOVE_KIND_ENTITY)
            continue;
        if (q2_move_box_overlap(lo, hi,
                                envelope ? t->env_min : t->min,
                                envelope ? t->env_max : t->max))
            return true;
    }

    return false;
}

/*
 * a2 = 1 — 0x8004576C's gate, asked before deciding whether a move needs the
 * entity sweep at all. "Could a mover's travel reach me this frame", not "am I
 * inside one now".
 *
 * This had no counterpart in the port, which is why q2_move_checked's retry —
 * and therefore q2_move_sweep_world itself — was unreachable: all five callers
 * passed a NULL stuck_test, and a NULL test returns before the arm that turns
 * entity sweeping on.
 */
bool q2_move_overlaps_any(const q2_move_world *w, const s32 pos[3])
{
    return overlaps_entity_slots(w, pos, true);
}

/* a2 = 0 — 0x80045D78's end-of-frame gate: slot+0x00..+0x14, where the entity
 * IS, not where it is going. */
bool q2_move_overlaps_any_live(const q2_move_world *w, const s32 pos[3])
{
    return overlaps_entity_slots(w, pos, false);
}

/* ------------------------------------------------------------------------- */
/* Segment against the entity boxes                                           */
/* ------------------------------------------------------------------------- */
/*
 * The slab test, in the 1.0.12 the rest of the port parametrises segments in.
 *
 * `t0` and `t1` stay 64-bit all the way through. A near-axis-parallel ray
 * makes the quotient enormous — the numerator is a world distance shifted up
 * twelve and the denominator can be one unit — and truncating it to 32 bits
 * turns a miss into a hit at a wrapped fraction. The range check at the end is
 * what discards those, so it has to see the real number.
 *
 * `axis` stays -1 when the START is inside the box: nothing ever raised `t0`
 * off zero, so there is no entry face and no normal to report. The caller gets
 * a fraction of 0, which is the honest answer for a shot fired from inside a
 * crusher.
 *
 * `plane` is the entry face's own coordinate, and it is reported because the
 * fraction cannot reconstruct it: 1.0.12 truncates, so recomputing the contact
 * from the fraction lands up to a unit short of the face. 0x80052C70 has the
 * same split — `out_pos[a] = plane` exactly, the other two from the ratio — so
 * this is the original's arrangement rather than a repair of the port's.
 */
static bool seg_hits_box(const s32 from[3], const s32 to[3],
                         const s32 bmin[3], const s32 bmax[3],
                         const s32 inflate[3],
                         s32 *out_frac, int *out_axis, int *out_sign,
                         s32 *out_plane)
{
    s64 t0 = 0, t1 = Q2_TRACE_SEG_ONE;
    int axis = -1, sign = 0;
    s32 plane = 0;
    int k;

    for (k = 0; k < 3; k++) {
        s32 grow = inflate ? inflate[k] : 0;
        s32 lo = bmin[k] - grow;
        s32 hi = bmax[k] + grow;
        s32 d  = to[k] - from[k];
        s64 a, b, near_t, far_t;

        if (d == 0) {
            /* Parallel to this pair of faces: either always between them or
             * never. */
            if (from[k] < lo || from[k] > hi)
                return false;
            continue;
        }

        a = (((s64)lo - from[k]) << 12) / d;
        b = (((s64)hi - from[k]) << 12) / d;
        near_t = a < b ? a : b;
        far_t  = a < b ? b : a;

        if (near_t > t0) {
            t0    = near_t;
            axis  = k;
            sign  = (d > 0) ? 1 : -1;
            plane = (d > 0) ? lo : hi;
        }
        if (far_t < t1)
            t1 = far_t;
        if (t0 > t1)
            return false;
    }

    if (t0 < 0 || t0 > Q2_TRACE_SEG_ONE)
        return false;

    *out_frac  = (s32)t0;
    *out_axis  = axis;
    *out_sign  = sign;
    *out_plane = plane;
    return true;
}

bool q2_move_clip_segment(const q2_move_world *w, const s32 from[3],
                          const s32 to[3], const s32 inflate[3],
                          q2_move_seg_hit *out)
{
    s32 best_frac  = Q2_TRACE_SEG_ONE;
    int best_axis  = -1, best_sign = 0;
    s32 best_plane = 0;
    s32 best_id    = -1;
    bool found     = false;
    u32 i;
    int k;

    if (out) {
        memset(out, 0, sizeof(*out));
        out->frac = Q2_TRACE_SEG_ONE;
        out->id   = -1;
        if (from) {
            out->pos[0] = from[0];
            out->pos[1] = from[1];
            out->pos[2] = from[2];
        }
    }

    if (!w || !w->targets || !from || !to)
        return false;

    for (i = 0; i < w->count; i++) {
        const q2_move_target *t = &w->targets[i];
        s32 frac, plane;
        int axis, sign;

        if (!t->active)
            continue;
        /* Entities only: see the header. A volume is not solid. */
        if (t->kind != Q2_MOVE_KIND_ENTITY)
            continue;

        if (!seg_hits_box(from, to, t->min, t->max, inflate,
                          &frac, &axis, &sign, &plane))
            continue;

        /* The NEAREST wins, unlike the move sweep, which keeps the last. */
        if (found && frac >= best_frac)
            continue;

        found      = true;
        best_frac  = frac;
        best_axis  = axis;
        best_sign  = sign;
        best_plane = plane;
        best_id    = t->id;

        if (frac == 0)
            break;          /* nothing can be nearer than the start */
    }

    if (!found)
        return false;

    if (out) {
        out->hit  = true;
        out->frac = best_frac;
        out->id   = best_id;
        for (k = 0; k < 3; k++)
            out->pos[k] = from[k] +
                (s32)((((s64)to[k] - from[k]) * best_frac) >> 12);
        if (best_axis >= 0) {
            /* Exactly on the face, not the truncated fraction's idea of it. */
            out->pos[best_axis]    = best_plane;
            out->normal[best_axis] = (s16)(best_sign > 0 ? 4096 : -4096);
        }
    }

    return true;
}

/* The stuck_test shape q2_move_checked wants, over the move world it is
 * already carrying. */
bool q2_move_near_entity(const q2_move_ent *ent, const void *user)
{
    const q2_move_world *w = (const q2_move_world *)user;

    return ent && q2_move_overlaps_any(w, ent->pos);
}

u16 q2_move_contents(const q2_move_world *w, const s32 pos[3], u16 mask)
{
    s32 lo[3], hi[3];
    u32 i;

    if (!w || !w->targets || !pos || mask == 0)
        return 0;

    /* The vertical half-extent is 285, not 286 (0x80050CF8 / 0x80050D28). */
    lo[0] = pos[0] - Q2_CONTENTS_HALF_XZ;
    lo[1] = pos[1] - Q2_CONTENTS_HALF_Y;
    lo[2] = pos[2] - Q2_CONTENTS_HALF_XZ;
    hi[0] = pos[0] + Q2_CONTENTS_HALF_XZ;
    hi[1] = pos[1] + Q2_CONTENTS_HALF_Y;
    hi[2] = pos[2] + Q2_CONTENTS_HALF_XZ;

    for (i = 0; i < w->count; i++) {
        const q2_move_target *t = &w->targets[i];

        if (t->kind != Q2_MOVE_KIND_VOLUME)
            continue;
        if (!(t->mask & mask))
            continue;
        if (!q2_move_box_overlap(lo, hi, t->min, t->max))
            continue;

        return t->mask;     /* 0x80050F78 returns the volume's own flags */
    }

    return 0;
}

/* 0x80045888 — 216 is in the branch's DELAY SLOT, so it is the default. */
s32 q2_move_step_height(u32 flags)
{
    return (flags & Q2_ENT_LOW_STEP) ? Q2_STEP_HEIGHT_LOW : Q2_STEP_HEIGHT;
}

/*
 * 0x800454F0..0x80045628. The projection of `d` onto `n`, in world units:
 * a 64-bit product, rounded by adding 2^23, then shifted right by 24 — the
 * normal is 1.3.12 and `d` already carries one factor of 4096, so 2^24 is the
 * right divisor and the addend is a true round-to-nearest.
 */
static s16 project(s16 n, s32 d)
{
    s64 p = (s64)n * (s64)d + (s64)0x800000;

    /* An arithmetic shift on a signed value is what the console does; spell it
     * out so a compiler that defines >> as logical cannot change the answer. */
    if (p < 0)
        return (s16)(-(s32)((-p + 0xFFFFFF) >> 24));

    return (s16)(s32)(p >> 24);
}

/* ------------------------------------------------------------------------- */
/* 0x80045144 — move with sliding                                             */
/* ------------------------------------------------------------------------- */
int q2_move(q2_collision *coll, q2_move_ent *ent, const s16 delta_in[3],
            const q2_move_mode *mode)
{
    s16 delta[3];
    s32 iterations;
    int result = 1;
    int i;

    if (!coll || !ent || !delta_in || !mode)
        return 1;

    delta[0] = delta_in[0];
    delta[1] = delta_in[1];
    delta[2] = delta_in[2];

    iterations = mode->iterations;

    for (;;) {
        s32  want[3];
        s32  dest[3];
        s32  end_pos[3];
        s32  end_node = -1;
        s16  normal[3] = {0, 0, 0};
        s16  push[3]   = {0, 0, 0};
        bool have_normal = false;
        int  world_ok;
        int  ents_ok = 1;

        for (i = 0; i < 3; i++)
            want[i] = ent->pos[i] + (s32)delta[i];

        dest[0] = want[0]; dest[1] = want[1]; dest[2] = want[2];

        /*
         * 0x80045220: the entity sweep runs FIRST and only when a3.hi is set,
         * which on the player's path means only 0x800456B0's retry. When it
         * hits, its clipped position — not the requested one — is what the
         * world trace is then asked for.
         */
        if (mode->sweep_ents && mode->world) {
            q2_move_contact hit;

            ents_ok = q2_move_sweep_world(mode->world, ent->pos, want, &hit);

            if (!ents_ok) {
                dest[0] = hit.pos[0];
                dest[1] = hit.pos[1];
                dest[2] = hit.pos[2];

                normal[0] = hit.normal[0];
                normal[1] = hit.normal[1];
                normal[2] = hit.normal[2];
                have_normal = true;

                result = 0;

                /* 0x80045244: the same flattest-floor rule as the world half,
                 * and the same nesting — 0x80045248's `beq` carries the ground
                 * block with it. See the note in the world half below. */
                if (ent->ground_normal[1] < hit.normal[1]) {
                    ent->ground_normal[0] = hit.normal[0];
                    ent->ground_normal[1] = hit.normal[1];
                    ent->ground_normal[2] = hit.normal[2];

                    /* 0x800452A4: standing on an entity is its own flag, with
                     * the thing being stood on named in bits 18..23. */
                    if (mode->ground_mode && ent->max_slope_ny < hit.normal[1]) {
                        if (hit.kind == Q2_MOVE_KIND_ENTITY) {
                            ent->flags |= Q2_ENT_ON_ENTITY;
                            ent->flags &= ~Q2_ENT_STANDON_MASK;
                            ent->flags |= ((u32)hit.id & 0x3Fu) << Q2_ENT_STANDON_SHIFT;
                        } else {
                            ent->flags |= Q2_ENT_ON_GROUND;
                        }
                    }
                }
            }
        }

        world_ok = q2_coll_move(coll, ent->pos, dest, ent->node,
                                end_pos, &end_node) ? 1 : 0;

        if (!world_ok && coll->hit_plane_index >= 0) {
            q2_coll_plane pl;

            if (q2_collision_get_plane(coll, (u32)coll->hit_plane_index, &pl)) {
                normal[0] = pl.nx;
                normal[1] = pl.ny;
                normal[2] = pl.nz;
                have_normal = true;

                /*
                 * 0x80045318: keep the flattest floor seen this frame. +Y is
                 * down, so "flattest floor" is the LARGEST ny.
                 *
                 * And the ground flag is INSIDE this, not beside it —
                 * 0x8004531C's `beq` skips the whole block, the ground-mode
                 * test at 0x8004533C included. A contact steeper than one
                 * already seen this frame cannot declare ground even if it
                 * would pass the slope limit on its own. Written as siblings, a
                 * second, worse contact in the same frame could set the flag
                 * against a ground_normal describing a different surface, and
                 * the drop's own verdict is what the jump reads.
                 */
                if (ent->ground_normal[1] < pl.ny) {
                    ent->ground_normal[0] = pl.nx;
                    ent->ground_normal[1] = pl.ny;
                    ent->ground_normal[2] = pl.nz;

                    /* 0x8004534C: and only the ground-mode move may declare it. */
                    if (mode->ground_mode && ent->max_slope_ny < pl.ny)
                        ent->flags |= Q2_ENT_ON_GROUND;
                }
            }
        }

        /* 0x80045374: the cached cell and the position come back from the
         * trace whether it completed or not. */
        ent->node   = end_node;
        ent->pos[0] = end_pos[0];
        ent->pos[1] = end_pos[1];
        ent->pos[2] = end_pos[2];

        /* 0x800453A0: `world_ok & ents_ok` — the move is finished only when
         * BOTH halves let it through. */
        if (world_ok && ents_ok)
            return result;

        result = 0;

        /*
         * 0x800453AC — a second slot, keyed on |ny| instead, so a ceiling
         * counts here and cannot count as ground above. It keeps the SMALLEST
         * |ny| of the frame: the most WALL-LIKE contact touched. This slot is
         * the port's wall detector, and the comparison was the wrong way round.
         *
         *     800453AC  lh   v1, 2(s2)     ; v1 = the new contact's ny
         *     800453B0  lh   v0, 20(s1)    ; v0 = last_normal[1]
         *     800453B4/BC   bgez/subu      ; v1 = |new|
         *     800453C0/C8   bgez/subu      ; v0 = |old|
         *     800453CC  slt  v1, v1, v0    ; |new| < |old| ?
         *     800453D0  beq  v1, zero, ... ; not smaller -> keep the old one
         *
         * Which is why the per-tick reset is (0, 4096, 0): 4096 is the LARGEST
         * |ny| a 1.3.12 unit normal can carry, so the sentinel is chosen to lose
         * to the frame's first real contact. Keeping the largest instead made
         * the predicate unsatisfiable against its own sentinel — the field was
         * frozen at (0, 4096, 0) for the entire session, and everything reading
         * it was reading a floor that was never touched.
         *
         * Three things were dead because of it: the ladder flag (0x800 needs
         * -1023 <= ny < 1024 and always saw 4096, so all 31 ladder volumes on
         * the disc were indistinguishable from air), the velocity clip, and the
         * water-exit jump. The sibling slot above, on the SIGNED ny into
         * ground_normal, is a different comparison and was already right.
         */
        if (have_normal) {
            s32 a = ent->last_normal[1] < 0 ? -(s32)ent->last_normal[1]
                                            :  (s32)ent->last_normal[1];
            s32 b = normal[1] < 0 ? -(s32)normal[1] : (s32)normal[1];

            if (b < a) {
                ent->last_normal[0] = normal[0];
                ent->last_normal[1] = normal[1];
                ent->last_normal[2] = normal[2];
            }
        }

        /* 0x800453F8: back off one unit along the dominant axis. */
        if (mode->push_mode && have_normal) {
            q2_move_push_vector(normal, push);
            for (i = 0; i < 3; i++)
                ent->pos[i] -= (s32)push[i];
        }

        /* 0x80045444: out of attempts. */
        if (iterations == 0)
            return result;

        /* 0x8004545C: what is left of the move, and the slide. */
        {
            s16 rem[3];
            s32 d;

            for (i = 0; i < 3; i++)
                rem[i] = sub16(want[i], ent->pos[i]);

            d = (s32)normal[0] * rem[0]
              + (s32)normal[1] * rem[1]
              + (s32)normal[2] * rem[2];

            for (i = 0; i < 3; i++)
                delta[i] = (s16)(rem[i] - project(normal[i], d));

            if (mode->push_mode) {
                for (i = 0; i < 3; i++)
                    delta[i] = (s16)(delta[i] - push[i]);
            }
        }

        iterations--;
    }
}

/* ------------------------------------------------------------------------- */
/* 0x800456B0 — move, and retry if the result is invalid                      */
/* ------------------------------------------------------------------------- */
int q2_move_checked(q2_collision *coll, q2_move_ent *ent, const s16 delta[3],
                    s32 iterations, bool ground_mode, bool push_mode,
                    q2_move_stuck_fn stuck_test, const void *user,
                    const q2_move_world *world)
{
    q2_move_ent saved;
    q2_move_mode mode;
    int rc;

    if (!coll || !ent || !delta)
        return 1;

    /* 0x800456E4: position, cached cell and both contact normals are saved,
     * and nothing else — the flags word is patched rather than restored. */
    saved = *ent;

    mode.ground_mode = ground_mode;
    mode.push_mode   = push_mode;
    mode.iterations  = iterations;
    mode.sweep_ents  = false;
    mode.world       = world;

    rc = q2_move(coll, ent, delta, &mode);

    if (!stuck_test || !stuck_test(ent, user))
        return rc;

    /* 0x8004577C: put everything back, clear the two contact flags this move
     * may have set, and go again with entity sweeping on. */
    ent->pos[0] = saved.pos[0];
    ent->pos[1] = saved.pos[1];
    ent->pos[2] = saved.pos[2];
    ent->node   = saved.node;
    memcpy(ent->ground_normal, saved.ground_normal, sizeof(ent->ground_normal));
    memcpy(ent->last_normal,   saved.last_normal,   sizeof(ent->last_normal));
    ent->flags &= ~(Q2_ENT_ON_GROUND | Q2_ENT_ON_ENTITY);

    mode.sweep_ents = true;

    return q2_move(coll, ent, delta, &mode);
}

/* ------------------------------------------------------------------------- */
/* 0x80051258 — bodies push each other apart                                  */
/* ------------------------------------------------------------------------- */
/* The shape 0x8008A7E8 has; the same routine ai.c keeps for the same reason. */
static s32 separate_isqrt(s64 n)
{
    s64 x, y;

    if (n <= 0)
        return 0;

    x = n;
    y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return (s32)x;
}

u32 q2_move_separate(const q2_move_bodies *w, s32 self_id, const s32 pos[3],
                     const s16 mins[3], const s16 maxs[3], s16 radius,
                     s16 delta[3], struct q2_rng *rng)
{
    /*
     * MY BOX, WHERE THE FRAME WILL LEAVE ME — 0x80051294..0x8005132C builds it
     * from pos + delta and only then adds mins and maxs. Testing where I am
     * would let a body walk into another one and be pushed out next frame,
     * which is the flicker this avoids.
     */
    s32 at[3], box_min[3], box_max[3];
    u32 result = 0;
    u32 i;
    int k;

    if (!w || !w->list || !w->count || !pos || !delta)
        return 0;

    for (k = 0; k < 3; k++) {
        at[k]      = pos[k] + delta[k];
        box_min[k] = at[k] + (mins ? mins[k] : 0);
        box_max[k] = at[k] + (maxs ? maxs[k] : 0);
    }

    for (i = 0; i < w->count; i++) {
        const q2_move_body *o = &w->list[i];
        s32 omin[3], omax[3];
        s64 dx, dz, d2, reach;
        s32 dist, pen;
        bool clear = false;

        /* 0x800513E0 skips self by pointer; the port's handle says the same
         * thing and survives the list being rebuilt each tick. */
        if (!o->solid || o->id == self_id)
            continue;

        for (k = 0; k < 3; k++) {
            omin[k] = o->pos[k] + o->mins[k];
            omax[k] = o->pos[k] + o->maxs[k];
        }

        /*
         * 0x8005133C..0x800513CC — the six-face reject, and it is what keeps
         * the horizontal cylinder below from reaching a creature standing one
         * floor down. Without it a body directly above or below another would
         * be shoved sideways.
         */
        for (k = 0; k < 3; k++) {
            if (box_max[k] < omin[k] || omax[k] < box_min[k]) {
                clear = true;
                break;
            }
        }
        if (clear)
            continue;

        /*
         * 0x800513FC..0x8005144C — dx^2 + dz^2 against (r + r)^2. X and Z only:
         * 0x80051400 reads +0x54 and 0x80051414 reads +0x5C, and nothing reads
         * +0x58. The vertical axis has already had its say in the box test.
         */
        dx = (s64)at[0] - o->pos[0];
        dz = (s64)at[2] - o->pos[2];
        d2 = dx * dx + dz * dz;
        reach = (s64)radius + o->radius;
        if (d2 >= reach * reach)
            continue;

        result |= Q2_SEPARATE_PUSHED;

        /*
         * 0x800514EC — two bodies at exactly the same X and Z have no direction
         * to be pushed along, so the original draws one: `(rand() - 16384) >> 10`
         * on each axis, which is +/- 16 units. Without an RNG there is nothing
         * honest to put here, so the pair is left to the next frame.
         */
        if (dx == 0 && dz == 0) {
            if (rng) {
                delta[0] = (s16)(delta[0] + ((q2_rng_next(rng) - 16384) >> 10));
                delta[2] = (s16)(delta[2] + ((q2_rng_next(rng) - 16384) >> 10));
            }
            continue;
        }

        /* 0x800514B4 the root, 0x800514E8 the penetration, 0x8005157C and
         * 0x800515B8 the two accumulates. Integer divides, as the original's
         * are — this is 16-bit arithmetic all the way down. */
        dist = separate_isqrt(d2);
        if (dist <= 0)
            continue;
        pen = (s32)reach - dist;
        if (pen <= 0)
            continue;

        delta[0] = (s16)(delta[0] + (s32)((dx * pen) / dist));
        delta[2] = (s16)(delta[2] + (s32)((dz * pen) / dist));
    }

    return result;
}

/* ------------------------------------------------------------------------- */
/* 0x80045D54 — the end-of-frame validity gate                                */
/* ------------------------------------------------------------------------- */
q2_move_step_stats q2_move_step_scan;

/*
 * WHERE THE FRAME ENDED HAS TO BE SOMEWHERE THE ENTITY MAY BE.
 *
 * 0x8004583C's arms all converge here — 0x80045C18 from the stepped arm, and
 * 0x80045CDC and 0x80045CF4 from the airborne one — and the last thing the
 * routine does before the integrator is ask two questions about the position
 * it has arrived at. If either answer is wrong the whole lift/slide/drop is
 * thrown away and the entity is put back at the pre-lift snapshot.
 *
 * This is retail's last-resort guard, and it is the one that stops a player
 * from ending a tick inside a door, a lift or a crusher: the moves themselves
 * sweep the entity boxes, but an entity that MOVED THIS TICK can close on a
 * stationary player, and nothing in the slide sequence would notice. It is
 * also what keeps an entity that has left the hull from keeping an off-hull
 * position — the drop can put a box through a gap the cell graph has no cell
 * behind.
 *
 *   80045D58  jal 0x8005553C          ; the mask, from the ENTITY's flags
 *   80045D78  jal 0x80050CE0          ; a2 = 0 (live box), a3 = !ON_ENTITY
 *   80045D80  bne v0, zero, 0x80045E04    ; -> rewind
 *   80045D98  jal 0x80044F54          ; find_node(pos, hint = +0xA2, brute=0)
 *   80045DA8  beq v1, v0, 0x80045DB8  ; -1 -> the second chance
 *   80045DB4  sh  v1, 162(s0)         ; else STORE the new cell and commit
 *   80045DC0  jal 0x800447C0          ; (ctx, pos, the OLD cell)
 *   80045DC8  beq v0, zero, 0x80045E04    ; it refuses too -> rewind
 *   80045E04  sh  s7, 162(s0)         ; REWIND: the cell, then +0x54..+0x5C
 *
 * NOT reproduced: 0x80045DD0-0x80045E00's commit also copies the position into
 * entity+0xA4..+0xAC and then rewrites +0xA8 as `pos.y - (s16)ent[+0xF8] +
 * 286`. The port models no such origin field at all, so there is nothing to
 * write it to; that is a separate gap and not one this gate can close.
 */
static void move_step_gate(q2_collision *coll, q2_move_ent *ent,
                           const q2_move_world *world,
                           const s32 pre_pos[3], s32 pre_node)
{
    u16  mask;
    bool bad;
    s32  found;

    q2_move_step_scan.gates++;

    /*
     * 0x8005553C is handed ent+0x54, the position, and reads 68(a0) — which is
     * ent+0x98, the flags word — so the mask this query sweeps volumes with is
     * the ENTITY's own, not `world->mask`. Bit 0 set means 0x810, else 0.
     *
     * Which entities set bit 0 was never traced (see sim.c's note beside
     * q2_move_world.mask), so for the player as the port builds it this is 0
     * and q2_move_contents returns 0 without looking at anything. Written as
     * the whole call anyway: the day that bit is understood, the volume half
     * of the gate starts working with no further change here.
     */
    mask = (ent->flags & 1u) ? 0x810u : 0u;

    /*
     * 0x80050CE0 walks the 48 entity slots first and the volumes after, and
     * a3 = ((flags >> 6) & 1) ^ 1 skips the entity walk entirely when the
     * entity is standing ON an entity. That skip is not an optimisation:
     * resting on a lift's top face puts the 285-half-height box through the
     * lift's own live box every tick, and without it a player riding a lift
     * would be rewound to where it boarded, every frame, forever.
     */
    bad = !(ent->flags & Q2_ENT_ON_ENTITY) &&
          q2_move_overlaps_any_live(world, ent->pos);
    if (!bad)
        bad = q2_move_contents(world, ent->pos, mask) != 0;

    if (bad) {
        q2_move_step_scan.rewound_overlap++;
        ent->node   = pre_node;
        ent->pos[0] = pre_pos[0];
        ent->pos[1] = pre_pos[1];
        ent->pos[2] = pre_pos[2];
        return;
    }

    /*
     * 0x80045D98 — re-locate the cell from where the frame actually ended,
     * with the cached one as the hint and NO brute-force sweep. On success the
     * cell is stored (0x80045DB4) as part of committing, which is how an
     * entity that stepped through a portal ends the frame holding the cell it
     * is now in rather than the one it left.
     */
    found = q2_coll_find_node(coll, ent->pos, ent->node, false);
    if (found != -1) {
        q2_move_step_scan.relocated++;
        ent->node = found;
        return;
    }

    /*
     * 0x80045DC0 — AND THE SECOND CHANCE IS NOT A TEST, IT IS A REPAIR.
     *
     * This used to ask q2_coll_point_in_node whether the position was in the
     * cached cell and rewind the frame when it was not, under a note saying
     * the port was "stricter than retail" and that a rewind was the safe
     * direction to be wrong in. It is not safe. 0x800447C0 does not answer a
     * question — it PULLS the point into the cell, one grid unit at a time,
     * toward the centroid of the cell's own planes, and writes the corrected
     * position back (q2_coll_settle_point, transcribed in collision.c). Only
     * the arm that runs out of room returns zero, and only that arm rewinds
     * (0x80045DC8 `beq v0, zero, 0x80045E04`).
     *
     * The cost of the approximation was a player who could not move at all.
     * BASE2 zone 0 and MATRIX5 zone 0 both spawn on floors where
     * q2_coll_find_node cannot re-locate the cell after a step: 45 of the
     * first 90 frames were rewound on BASE2, the position never changed, and
     * `q2psx-inspect walk BASE2 0 200` reported the player walking 0 world
     * units in 99 ticks of walking input.
     *
     * The `node < 0 -> accept` case lives inside q2_coll_settle_point now,
     * where 0x800447F4 puts it. It is still not optional: the port reaches
     * node = -1 as a real state (sim.c's player_area).
     */
    if (q2_coll_settle_point(coll, ent->pos, ent->node))
        return;

    q2_move_step_scan.rewound_unplaced++;
    ent->node   = pre_node;
    ent->pos[0] = pre_pos[0];
    ent->pos[1] = pre_pos[1];
    ent->pos[2] = pre_pos[2];
}

/* ------------------------------------------------------------------------- */
/* 0x8004583C — the stepped frame move                                        */
/* ------------------------------------------------------------------------- */
bool q2_move_actor(q2_collision *coll, q2_move_ent *ent, const s16 delta_in[3],
                  const q2_move_world *world,
                  const q2_move_bodies *bodies, s32 self_id,
                  const s16 mins[3], const s16 maxs[3], struct q2_rng *rng,
                  bool force_step)
{
    /*
     * THE FRAME'S DELTA IS A LOCAL, because the separation pass writes to it.
     *
     * 0x80051258 accumulates its push into entity+0xEC..+0xF0, which is the
     * entity's own delta and is what 0x80045B70 then hands to the slide. The
     * port's callers pass the delta in rather than keeping it on the entity, so
     * the copy is where the write lands; the integrator rewrites the caller's
     * every tick anyway, exactly as the console's does.
     */
    s16 delta[3];
    s32 step;
    s32 saved_y;
    s32 drop;
    s16 v[3];
    bool grounded;
    s32 pre_pos[3],  pre_node;
    s32 post_pos[3], post_node;

    if (!coll || !ent || !delta_in)
        return false;

    delta[0] = delta_in[0];
    delta[1] = delta_in[1];
    delta[2] = delta_in[2];

    /*
     * 0x80045990 / 0x80045998 — the append and the separation, at the TOP of
     * the stepped move and before anything reads the flags. The append is the
     * caller's job here: `bodies` is the list, already published.
     */
    if (bodies)
        (void)q2_move_separate(bodies, self_id, ent->pos, mins, maxs,
                               Q2_BODY_RADIUS, delta, rng);

    step    = q2_move_step_height(ent->flags);
    saved_y = ent->pos[1];

    /*
     * 0x80045A98/0x80045AA0 — the PRE-LIFT snapshot, taken here because that is
     * where the original takes it: above the flags read at 0x80045AD4 that
     * chooses between the stepped and the airborne path, so both can rewind to
     * it. See the fallback after the drop.
     */
    pre_node   = ent->node;
    pre_pos[0] = ent->pos[0];
    pre_pos[1] = ent->pos[1];
    pre_pos[2] = ent->pos[2];
    post_node  = pre_node;
    post_pos[0] = pre_pos[0];
    post_pos[1] = pre_pos[1];
    post_pos[2] = pre_pos[2];

    /*
     * 0x80045ADC — the step sequence is for an entity that is ALREADY STANDING
     * on something and is not submerged. Everything else takes the simple slide
     * below.
     *
     * This is not an optimisation. The step sequence ends with a drop of a whole
     * step height, which is far larger than one tick of a jump — a jump produces
     * about 100 units of upward delta against a 216-unit step down — so running
     * it while airborne cancels the jump completely and the player never leaves
     * the floor. The flags are read here, before they are cleared.
     */
    grounded = ((ent->flags & Q2_ENT_GROUNDED_MASK) != 0 || force_step) &&
               !(ent->flags & Q2_ENT_UNDERWATER);

    if (!grounded) {
        /*
         * 0x80045CA4 — airborne, or swimming. One slide with the frame delta,
         * three further attempts, and BOTH modes on: this move may declare
         * ground (which is how a fall lands) and it applies the unstick nudge.
         * Table entry 0x800AE93C = {1, 1}.
         */
        q2_move_checked(coll, ent, delta, 3, true, true,
                        q2_move_near_entity, world, world);

        /*
         * 0x80045CD0 — if that did not land but the running ground normal says
         * a floor was touched, clear it and probe 20 units down to commit.
         * Delta from 0x800AE940, mode {1, 0}.
         */
        if (!(ent->flags & Q2_ENT_GROUNDED_MASK) &&
            ent->max_slope_ny < ent->ground_normal[1]) {
            ent->ground_normal[0] = ent->ground_normal[1] =
                ent->ground_normal[2] = 0;

            v[0] = 0; v[1] = 20; v[2] = 0;
            q2_move_checked(coll, ent, v, 0, true, false,
                        q2_move_near_entity, world, world);
        }

        /*
         * 0x80045CDC and 0x80045CF4 both branch to 0x80045D54, and the 20-unit
         * probe above falls into it — the airborne arm has no exit of its own.
         * This is the arm a player falling onto a lift or caught by a
         * descending crusher is actually on, so the gate matters more here
         * than on the stepped arm.
         */
        move_step_gate(coll, ent, world, pre_pos, pre_node);

        return (ent->flags & Q2_ENT_GROUNDED_MASK) != 0;
    }

    /*
     * 0x80045B14 — both contact flags are cleared before the frame's moves, so
     * ground is decided fresh every frame. THE STEPPED ARM ONLY: the branch
     * that picks the arm sits above the clear, and its delay slot is just a
     * constant load, so the airborne path jumps straight past it.
     *
     *     80045B0C  beq   v0, zero, 0x80045CA4   ; -> the airborne arm
     *     80045B10  addiu v0, zero, -33          ; delay slot, a constant
     *     80045B14  and   v0, v1, v0             ; ~0x20
     *     80045B1C  and   v0, v0, v1             ; ~0x40
     *     80045B20  sw    v0, 152(s0)
     *
     * Clearing it on both arms is only reachable while UNDERWATER — that is the
     * one state whose entity can be grounded and still take the airborne path —
     * and it dropped the ground flag a submerged player standing on the bottom
     * should keep. That latch is what keeps the 20-unit down probe skipped and
     * what makes the 0x2000 buoyancy boost fire on the frames the console fires
     * it on.
     */
    ent->flags &= ~(Q2_ENT_ON_GROUND | Q2_ENT_ON_ENTITY);

    /* Up. -Y is up. Table entry 0x800AE930 = {0, 0}, zero further attempts. */
    v[0] = 0; v[1] = (s16)(-step); v[2] = 0;
    q2_move_checked(coll, ent, v, 0, false, false,
                    q2_move_near_entity, world, world);

    /*
     * How far the LIFT actually got, measured before the slide runs.
     *
     * 0x80045B90 reads pos.y here and 0x80045BA0 subtracts it from the value
     * saved at 0x80045B68, which was read before the lift — so the drop is
     * sized by the lift ALONE and any vertical motion the slide contributes is
     * deliberately not counted. Measuring it after the slide instead (which is
     * the natural way to write this, and what this function used to do) makes
     * an entity sliding down a slope drop twice as far each frame and jitter.
     */
    drop = saved_y - ent->pos[1];

    /* Slide. 0x800AE934 = {0, 1}: nudge on, ground detection off, and three
     * further attempts — enough for a corner of two walls and a floor. */
    q2_move_checked(coll, ent, delta, 3, false, true,
                    q2_move_near_entity, world, world);

    /* 0x80045BA4..0x80045BBC — the POST-SLIDE snapshot, which is the other
     * place the fallback below can rewind to. */
    post_node  = ent->node;
    post_pos[0] = ent->pos[0];
    post_pos[1] = ent->pos[1];
    post_pos[2] = ent->pos[2];

    /*
     * 0x80045BC0..0x80045BD8 clears the ground normal before the down move, so
     * the value the drop leaves is this frame's and never last frame's. The
     * jump reads it, so a stale one lets you jump in mid-air.
     */
    ent->ground_normal[0] = ent->ground_normal[1] = ent->ground_normal[2] = 0;

    /* Down, by however far the lift managed plus the step. 0x800AE938 = {1, 0}:
     * this move, and only this move, may declare ground. */
    v[0] = 0; v[1] = (s16)(drop + step); v[2] = 0;
    q2_move_checked(coll, ent, v, 0, true, false,
                        q2_move_near_entity, world, world);

    /*
     * -----------------------------------------------------------------------
     * 0x80045C0C — WHAT HAPPENS WHEN THE STEP-DOWN DOES NOT FIND A FLOOR
     * -----------------------------------------------------------------------
     * The port stopped at the drop and kept whatever it produced. The original
     * does not: 0x80045C18 tests the ground flags, and when they are clear it
     * UNDOES the step sequence and moves a different way instead. Two arms,
     * chosen at 0x80045C28 on whether the drop touched anything at all.
     *
     * Without this, walking off any ledge shorter than a step height committed
     * the whole 216-unit drop in one tick — the player was teleported to the
     * bottom of the step-down instead of walking off the edge and falling, so
     * every kerb, stair edge and doorway lip produced a visible snap and a
     * vertical velocity the fall-damage delta then measured against nothing.
     */
    if (!(ent->flags & Q2_ENT_GROUNDED_MASK)) {
        if (ent->ground_normal[1] == 0) {
            /*
             * 0x80045C68 — arm B. The drop hit NOTHING: there is no floor
             * within a step below, so the step-down was the wrong move
             * entirely. Put the entity back where the slide left it and fall
             * only by however far the LIFT rose — s4 at 0x80045C7C is `drop`
             * alone, not `drop + step`, so the lift is undone and no more.
             * Anything beyond that is this frame's gravity to supply.
             */
            ent->pos[0] = post_pos[0];
            ent->pos[1] = post_pos[1];
            ent->pos[2] = post_pos[2];
            ent->node   = post_node;

            v[0] = 0; v[1] = (s16)drop; v[2] = 0;
            q2_move_checked(coll, ent, v, 0, true, false,
                            q2_move_near_entity, world, world);
        } else {
            /*
             * 0x80045C30 — arm A. Something WAS touched and it was too steep to
             * stand on. The whole lift-slide-drop is discarded: rewind to the
             * pre-lift snapshot and re-run the frame's own delta as an ordinary
             * slide, mode {1, 1} with three further attempts (0x80045C64), so
             * the entity slides along the steep face rather than being stepped
             * onto it.
             *
             * `ground_normal` is deliberately NOT restored — 0x80045C44 writes
             * only the node and 0x80045C54 only the position. The steep normal
             * the drop just found is what this re-slide runs against.
             */
            ent->pos[0] = pre_pos[0];
            ent->pos[1] = pre_pos[1];
            ent->pos[2] = pre_pos[2];
            ent->node   = pre_node;

            q2_move_checked(coll, ent, delta, 3, true, true,
                            q2_move_near_entity, world, world);
        }
    }

    /* 0x80045C18 falls through to the gate when the drop found ground, and
     * both fallback arms above jump back into it (0x80045C60 -> 0x80045D44,
     * 0x80045C9C -> 0x80045D4C), so every path through the stepped arm is
     * gated too. */
    move_step_gate(coll, ent, world, pre_pos, pre_node);

    return (ent->flags & Q2_ENT_GROUNDED_MASK) != 0;
}
