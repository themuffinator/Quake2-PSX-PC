#include "projectile.h"

#include "trig.h"
#include "random.h"
#include "sdkmath.h"
#include "trace.h"
#include "worldscale.h"

#include <string.h>

void q2_projectiles_init(q2_projectiles *list)
{
    if (!list)
        return;
    memset(list, 0, sizeof(*list));
}

static q2_projectile *alloc_slot(q2_projectiles *list, s32 *out_index)
{
    u32 i;

    for (i = 0; i < Q2_PROJ_MAX; i++) {
        if (!list->p[i].in_use) {
            memset(&list->p[i], 0, sizeof(list->p[i]));
            list->p[i].in_use = true;
            list->live++;
            if (out_index)
                *out_index = (s32)i;
            return &list->p[i];
        }
    }
    if (out_index)
        *out_index = -1;
    return NULL;
}

static void release(q2_projectiles *list, u32 index)
{
    if (index >= Q2_PROJ_MAX || !list->p[index].in_use)
        return;
    list->p[index].in_use = false;
    if (list->live)
        list->live--;
}

/* The runtime stores velocity in a common 1.0.12 representation so collision
 * reflection is independent of projectile kind. Movement on the console is
 * performed on each spawner's raw velocity, however: bolt /1, shared entities
 * /320, BFG /64. The launch conversions have at least 12.8 fixed units per raw
 * unit, so rounding the inverse to nearest recovers every original s16 exactly
 * instead of introducing a one-unit error for negative components. */
static s32 velocity_divisor(q2_proj_kind kind)
{
    if (kind == Q2_PROJ_BOLT)
        return 1;
    if (kind == Q2_PROJ_BFG)
        return Q2_BFG_VEL_DIV;
    return Q2_VEL_DIV;
}

static s32 fixed_to_raw(s32 fixed, s32 divisor)
{
    s64 scaled = (s64)fixed * divisor;

    if (scaled >= 0)
        return (s32)((scaled + 2048) / 4096);
    return (s32)-(((-scaled) + 2048) / 4096);
}

static s32 raw_to_fixed(s32 raw, s32 divisor)
{
    return (s32)(((s64)raw * 4096) / divisor);
}

s32 q2_projectile_raw_velocity(const q2_projectile *p, int axis)
{
    if (!p || axis < 0 || axis > 2)
        return 0;
    return fixed_to_raw(p->vel[axis], velocity_divisor(p->kind));
}

/* ------------------------------------------------------------------------- */
u16 q2_projectile_flags_for_mod(s16 mod)
{
    /*
     * The three callers of 0x8004D70C pass the flags and the means of death as
     * a pair, and there are only two pairs: (11, 6) at 0x8004C11C/0x8004C124
     * and at 0x800620D4/0x800620DC, and (14, 5) at 0x8004D3E8/0x8004D3F0 and
     * at 0x800620BC/0x800620C8. Anything that is not a bolt has neither.
     */
    if (mod == Q2_MOD_BOLT_HYPER)
        return Q2_PROJ_FLAGS_HYPERBLASTER;
    if (mod == Q2_MOD_BOLT)
        return Q2_PROJ_FLAGS_BLASTER;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* SquareRootNR (0x80055CBC) as shock.c stands in for it: an exact floor root,
 * where the console seeds two Newton steps from libgte's table. */
static s32 floor_sqrt(u32 v)
{
    u32 r = 0, bit = 1u << 30;

    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (s32)r;
}

void q2_projectile_aim_rocket(q2_projectile *p, s32 shooter_yaw)
{
    s32 vx, vy, vz;

    if (!p)
        return;
    vx = (s16)q2_projectile_raw_velocity(p, 0);
    vy = (s16)q2_projectile_raw_velocity(p, 1);
    vz = (s16)q2_projectile_raw_velocity(p, 2);

    /* 0x8004B1D4..0x8004B208: `mult` of the two halfwords, one root, then
     * ratan2 of the vertical against it, negated. */
    p->base[0]   = (s16)-q2_ratan2(vy, floor_sqrt((u32)(vx * vx + vz * vz)));
    p->base[1]   = (s16)shooter_yaw;
    p->angles[0] = p->base[0];
    p->angles[1] = p->base[1];
    p->angles[2] = 0;
    p->phase     = 0;                      /* 0x8004B244 `sw zero, 72(s0)` */
}

void q2_projectile_set_attitude(q2_projectile *p, s32 r0, s32 r1, s32 r2)
{
    if (!p)
        return;
    p->angles[0] = (s16)r0;
    p->angles[1] = (s16)r1;
    p->angles[2] = (s16)r2;
}

void q2_projectile_spin_bfg(q2_projectile *p, s32 start, s32 end)
{
    if (!p)
        return;
    p->anim     = (s16)start;
    p->anim_end = (s16)end;
}

/* ------------------------------------------------------------------------- */
s32 q2_projectile_launch(q2_projectiles *list, const q2_fire_result_v2 *fire,
                         s32 owner, s32 now)
{
    q2_projectile *p;
    s32 index = -1;
    const q2_shot *s;
    int k;

    if (!list || !fire || !fire->fired || fire->shot_count == 0)
        return -1;

    switch (fire->kind) {
    case Q2_FK_BOLT:
    case Q2_FK_GRENADE:
    case Q2_FK_HAND_GRENADE:
    case Q2_FK_ROCKET:
    case Q2_FK_BFG:
        break;
    default:
        return -1;      /* hitscan and rail resolve without an entity */
    }

    p = alloc_slot(list, &index);
    if (!p)
        return -1;

    s = &fire->shot[0];
    memcpy(p->pos, s->origin, sizeof(p->pos));
    p->damage = s->damage;
    p->mod    = fire->mod;
    p->owner  = owner;
    p->node   = Q2_PROJ_NODE_UNKNOWN; /* found on the first move */

    switch (fire->kind) {
    case Q2_FK_BOLT:
        /*
         * The bolt is the one projectile whose direction IS its velocity: the
         * spawner stores the argument straight into the entity's +0x52 and the
         * sweep at 0x80047D40 multiplies it by the frame's dt. So there is
         * nothing to normalise, and the hyperblaster's bolt really is half the
         * speed of the blaster's because its shift is one bit deeper.
         */
        p->kind = Q2_PROJ_BOLT;
        p->splash_radius = 0;
        p->expires = now + Q2_LIFETIME_BOLT;
        /* record+0x22, from the fifth argument of 0x8004D70C. It decides
         * whether this bolt trails or has a body, and the two are exclusive. */
        p->flags = q2_projectile_flags_for_mod(p->mod);
        for (k = 0; k < 3; k++)
            p->vel[k] = s->dir[k] * 4096;      /* 1.0.12, as the mover wants */
        return index;
    case Q2_FK_GRENADE:
        p->kind = Q2_PROJ_GRENADE;
        p->splash_radius = Q2_SPLASH_RADIUS_GRENADE;
        p->expires = now + (fire->projectile_timer ? fire->projectile_timer
                                                   : Q2_GRENADE_LAUNCH_FUSE);
        break;
    case Q2_FK_HAND_GRENADE:
        p->kind = Q2_PROJ_HAND_GRENADE;
        p->splash_radius = Q2_SPLASH_RADIUS_GRENADE;
        p->expires = now + (fire->projectile_timer ? fire->projectile_timer
                                                   : Q2_HAND_GRENADE_FUSE);
        /* 0x8004ABC4 writes state 1 and 0x8004ABCC writes 4096 to +0x4C.
         * There is deliberately no velocity until the 411 crossing. Keeping
         * the raw charge in vel[2] avoids widening the save record. */
        p->node   = Q2_PROJ_NODE_HELD;
        p->vel[2] = Q2_HAND_GRENADE_CHARGE_START;
        return index;
    case Q2_FK_ROCKET:
        p->kind = Q2_PROJ_ROCKET;
        p->splash_radius = Q2_SPLASH_RADIUS_ROCKET;
        p->expires = now + (fire->projectile_timer ? fire->projectile_timer
                                                   : Q2_ROCKET_LIFETIME);
        break;
    default:
        p->kind = Q2_PROJ_BFG;
        p->splash_radius = Q2_SPLASH_RADIUS_BFG;
        p->expires = now + (fire->projectile_timer ? fire->projectile_timer
                                                   : Q2_BFG_LIFETIME);
        break;
    }

    if (s->dir[0] == 0 && s->dir[1] == 0 && s->dir[2] == 0) {
        release(list, (u32)index);
        return -1;
    }

    if (p->kind == Q2_PROJ_BFG) {
        /* 0x8004B5F0 rotates literal {0,0,768}. q2_sim_aim is already the
         * rotation matrix's forward column, so each raw component is the
         * console's signed fixed multiply — there is no Euclidean normalise.
         * The arithmetic shift matters for a negative, non-integral product. */
        for (k = 0; k < 3; k++) {
            s32 raw = (s32)(((s64)s->dir[k] * Q2_BFG_RAW_SPEED) >> 12);
            p->vel[k] = raw_to_fixed(raw, Q2_BFG_VEL_DIV);
        }
    } else {
        for (k = 0; k < 3; k++)
            p->vel[k] = (s32)(((s64)s->dir[k] * 4096) / Q2_VEL_DIV);
    }

    return index;
}

/* ------------------------------------------------------------------------- */
s32 q2_projectile_hand_held_index(const q2_projectiles *list, s32 owner)
{
    u32 i;

    if (!list)
        return -1;

    for (i = 0; i < Q2_PROJ_MAX; i++) {
        const q2_projectile *p = &list->p[i];

        if (p->in_use && p->kind == Q2_PROJ_HAND_GRENADE &&
            p->node == Q2_PROJ_NODE_HELD && p->owner == owner)
            return (s32)i;
    }
    return -1;
}

s32 q2_projectile_hand_charge(const q2_projectiles *list, s32 owner)
{
    s32 index = q2_projectile_hand_held_index(list, owner);

    return index >= 0 ? list->p[index].vel[2] : 0;
}

bool q2_projectile_hand_update(q2_projectiles *list, s32 owner,
                               const s32 attached_pos[3], s32 cook_dt)
{
    s32 index = q2_projectile_hand_held_index(list, owner);
    q2_projectile *p;

    if (index < 0 || !attached_pos)
        return false;

    p = &list->p[index];
    memcpy(p->pos, attached_pos, sizeof(p->pos));
    if (cook_dt > 0)
        p->vel[2] += Q2_HAND_GRENADE_CHARGE_PER_DT * cook_dt;
    return true;
}

bool q2_projectile_hand_release(q2_projectiles *list, s32 owner,
                                const s32 origin[3], const s32 raw_dir[3])
{
    s32 index = q2_projectile_hand_held_index(list, owner);
    q2_projectile *p;
    int k;

    if (index < 0 || !origin || !raw_dir)
        return false;
    if (raw_dir[0] == 0 && raw_dir[1] == 0 && raw_dir[2] == 0)
        return false;

    p = &list->p[index];
    memcpy(p->pos, origin, sizeof(p->pos));
    for (k = 0; k < 3; k++)
        p->vel[k] = raw_to_fixed(raw_dir[k], Q2_VEL_DIV);
    p->node = Q2_PROJ_NODE_UNKNOWN;
    return true;
}

/* ------------------------------------------------------------------------- */
void q2_projectile_step(q2_projectiles *list, u32 index, s32 gravity, s32 dt,
                        s32 now, q2_proj_step *out)
{
    q2_projectile *p;
    s32 divisor;
    int k;

    if (out)
        memset(out, 0, sizeof(*out));
    if (!list || index >= Q2_PROJ_MAX || !out)
        return;

    p = &list->p[index];
    if (!p->in_use)
        return;

    /* Grenade3 state 1 is positioned by its owner and has no mover. Its fuse
     * still runs: 0x8004A3DC subtracts dt before the state dispatch. */
    if (p->kind == Q2_PROJ_HAND_GRENADE &&
        p->node == Q2_PROJ_NODE_HELD) {
        memcpy(out->from, p->pos, sizeof(out->from));
        memcpy(out->to, p->pos, sizeof(out->to));
        if (p->expires && now >= p->expires)
            out->expired = true;
        return;
    }

    if (dt <= 0)
        dt = Q2_DT_NOMINAL;

    if (p->kind == Q2_PROJ_GRENADE || p->kind == Q2_PROJ_HAND_GRENADE) {
        bool back = ((u16)p->angles[0] & 1u) == 0;
        if (p->flags & Q2_PROJ_TOSS_STOPPED) {
            memcpy(out->from, p->pos, sizeof(out->from));
            memcpy(out->to, p->pos, sizeof(out->to));
            out->expired = p->expires && now >= p->expires;
            return;
        }
        /* 0x80046BE0..0x80046CA0: three shared rand draws, independent of dt. */
        for (k = 0; k < 3; k++) {
            s32 turn = 160 + 2 * (q2_random_next() & 31);
            p->angles[k] = (s16)(back ? p->angles[k] - turn : p->angles[k] + turn);
        }
    }

    /*
     * A ROCKET CIRCLES ITS LINE OF FLIGHT, before it moves — the head of
     * PrimaryRocketActive, 0x8004AD34..0x8004ADBC. Its phase at +0x48 gains
     * `dt << 5` a tick, and the attitude it is drawn with is the launch
     * attitude plus a sine and a cosine of that phase, each halfword taken
     * `sll 16; sra 22`: a 64-step (5.6 degree) wobble of pitch against yaw,
     * once round every 4096 / 384 ticks at the PAL delta.
     */
    /* The BFG ball's spin: the delta on, the move's end off until below it
     * (0x8004B7FC..0x8004B838), in the halfword the console keeps it in. */
    if (p->kind == Q2_PROJ_BFG && p->anim_end > 0) {
        p->anim = (s16)(p->anim + dt);
        while (p->anim >= p->anim_end)
            p->anim = (s16)(p->anim - p->anim_end);
    }

    if (p->kind == Q2_PROJ_ROCKET) {
        p->phase     = (u16)((p->phase + ((u32)dt << 5)) & 0xFFFu);
        p->angles[0] = (s16)(p->base[0] + ((s16)q2_sin12(p->phase) >> 6));
        p->angles[1] = (s16)(p->base[1] + ((s16)q2_cos12(p->phase) >> 6));
    }

    /*
     * Only the grenades fall; the bolt, the rocket and the BFG blast fly
     * straight, which is why nothing but the grenades has a launch arc.
     *
     * THE SCALE. `gravity` is Q2_GRAVITY in the PLAYER's units, where a
     * position advances as `vel * dt / Q2_VEL_DIV` and the integrator adds
     * `g * dt` to the velocity each tick (sim.c's integrate_vertical). The
     * list keeps velocity in 1.0.12 for collision response, but this step
     * recovers the original raw value before integrating it. Re-encoding the
     * same acceleration in the stored representation changes it by
     *
     *     dv_p = g * dt * 4096 / Q2_VEL_DIV
     *
     * which at g = 32, dt = 12 gives 4915 against the 384 the player adds, and
     * both move the per-tick position delta by the same 14.4 units. This used
     * to be a bare `+= gravity` — 32 in a 1.0.12 velocity, an acceleration of
     * 0.008 units a tick, about 600 times too small. Which is why the grenade
     * launcher and the hand grenade had no arc at all.
     */
    divisor = velocity_divisor(p->kind);

    if (p->kind == Q2_PROJ_GRENADE || p->kind == Q2_PROJ_HAND_GRENADE) {
        s32 raw_y = fixed_to_raw(p->vel[1], Q2_VEL_DIV);
        raw_y = (s16)((u32)raw_y + (u32)gravity * (u32)dt);
        /* Shared mover 0x80046464..0x800464A0 uses the same one-sided
         * terminal clamp as the player path. Upward velocity is unrestricted;
         * only a fall past +8192 is capped. */
        if (raw_y > Q2_TERMINAL_VY)
            raw_y = Q2_TERMINAL_VY;
        p->vel[1] = raw_to_fixed(raw_y, Q2_VEL_DIV);
    }

    memcpy(out->from, p->pos, sizeof(out->from));

    /*
     * AND THE FRAME DELTA, which this step used to drop entirely.
     *
     * The bolt sweep at 0x80047D40 forms `pos += vel * dt`; the shared entity
     * mover uses `pos += vel * dt / 320`; BFG's private mover uses `/64`.
     * `velocity_divisor` selects those three instruction-level paths after the
     * common fixed representation is converted back to raw velocity. Omitting
     * dt made a bolt 12x too slow at the nominal tick and 20x too slow in the
     * 1/30 s headless step — 64 units a frame where it should cover 768.
     */
    for (k = 0; k < 3; k++) {
        s32 raw = fixed_to_raw(p->vel[k], divisor);
        /* The bolt sweep stores vel*dt through a signed halfword before its
         * 32-bit position add (0x80047D58..0x80047DA4). */
        if (p->kind == Q2_PROJ_BOLT)
            out->to[k] = (s32)((u32)p->pos[k] +
                (u32)(s32)(s16)((u32)(s32)(s16)raw * (u32)dt));
        else
            out->to[k] = p->pos[k] + (s32)(((s64)raw * dt) / divisor);
        if (p->kind == Q2_PROJ_GRENADE || p->kind == Q2_PROJ_HAND_GRENADE)
            out->to[k] = p->pos[k] + (s16)(((s32)(s16)raw * dt) / divisor);
    }

    if (p->expires && now >= p->expires)
        out->expired = true;
}

void q2_projectile_commit(q2_projectiles *list, u32 index, const s32 to[3])
{
    if (!list || index >= Q2_PROJ_MAX || !to)
        return;
    if (!list->p[index].in_use)
        return;
    memcpy(list->p[index].pos, to, sizeof(list->p[index].pos));
}

bool q2_projectile_ignore_owner(q2_projectile *p, const q2_actor *owner)
{
    int axis;
    if (!p || !owner) return false;
    if (p->kind != Q2_PROJ_GRENADE && p->kind != Q2_PROJ_HAND_GRENADE)
        return true;
    if (p->kind == Q2_PROJ_HAND_GRENADE && p->node == Q2_PROJ_NODE_HELD)
        return true;
    if (!(p->flags & Q2_PROJ_OWNER_CLEAR)) {
        for (axis = 0; axis <= 2; axis += 2) {
            s32 low = (s32)((u32)owner->origin[axis] + (u32)(s32)owner->mins[axis] - 40u);
            s32 high = (s32)((u32)owner->origin[axis] + (u32)(s32)owner->maxs[axis] + 40u);
            if (p->pos[axis] < low || p->pos[axis] > high)
                p->flags |= Q2_PROJ_OWNER_CLEAR;
        }
    }
    return !(p->flags & Q2_PROJ_OWNER_CLEAR);
}

void q2_projectile_rebound(q2_projectile *p, const s32 desired[3],
                            const s32 clipped[3], const s16 normal[3])
{
    s16 velocity[3], push[3];
    s32 remainder[3], direction[3], unit[3], dot2, speed;
    u32 square = 0, dot = 0;
    int k;
    if (!p || !desired || !clipped || !normal) return;
    for (k = 0; k < 3; k++) {
        velocity[k] = (s16)fixed_to_raw(p->vel[k], Q2_VEL_DIV);
        square += (u32)((s32)velocity[k] * velocity[k]);
    }
    speed = (s32)(q2_sdk_sqrt0(square) * Q2_GRENADE_RESTITUTION) >> 12;
    q2_move_push_vector(normal, push);
    for (k = 0; k < 3; k++) {
        p->pos[k] = clipped[k] - push[k];
        remainder[k] = desired[k] - p->pos[k];
        dot += (u32)(s32)normal[k] * (u32)remainder[k];
    }
    dot2 = (s32)(0u - dot * 2u);
    for (k = 0; k < 3; k++) {
        s32 reflected = desired[k] + (s32)(((s64)normal[k] * dot2 + 0x800000) >> 24);
        direction[k] = reflected - p->pos[k];
    }
    q2_sdk_vector_normal(direction, unit);
    square = 0;
    for (k = 0; k < 3; k++) {
        velocity[k] = (s16)((s32)((u32)unit[k] * (u32)speed) >> 12);
        p->vel[k] = raw_to_fixed(velocity[k], Q2_VEL_DIV);
        square += (u32)((s32)velocity[k] * velocity[k]);
    }
    if ((s32)square > (s32)Q2_GRENADE_REST_SPEED_SQ) {
        p->flags = (u16)(p->flags & ~Q2_PROJ_TOSS_REST);
        p->angles[0] = (s16)(p->angles[0] + 1);
    } else {
        p->flags |= Q2_PROJ_TOSS_REST;
    }
    p->bounced = true;
}

bool q2_projectile_expire(q2_projectiles *list, u32 index)
{
    if (!list || index >= Q2_PROJ_MAX || !list->p[index].in_use)
        return false;

    release(list, index);
    return true;
}

/* ------------------------------------------------------------------------- */
u32 q2_projectile_detonate(q2_projectiles *list, u32 index,
                           q2_actor *attacker, q2_actor **targets, u32 count,
                           const q2_combat_rules *rules)
{
    return q2_projectile_detonate_traced(list, index, attacker, targets, count,
                                         rules, NULL, NULL);
}

u32 q2_projectile_detonate_traced(q2_projectiles *list, u32 index,
                                  q2_actor *attacker, q2_actor **targets,
                                  u32 count, const q2_combat_rules *rules,
                                  q2_combat_clear_fn clear, void *clear_ctx)
{
    q2_projectile *p;
    u32 hurt = 0;

    if (!list || index >= Q2_PROJ_MAX)
        return 0;
    p = &list->p[index];
    if (!p->in_use)
        return 0;

    /* Every splash site in the header's table calls 0x80050810, so the fuse's
     * blast takes the same per-candidate occlusion as a contact blast. */
    if (p->splash_radius > 0 && p->damage > 0)
        hurt = q2_combat_radius_damage_traced(attacker, NULL, p->pos,
                                              p->damage, p->splash_radius,
                                              p->mod, targets, count, rules,
                                              clear, clear_ctx);

    release(list, index);
    return hurt;
}

bool q2_projectile_impact(q2_projectiles *list, u32 index,
                          const s32 point[3],
                          q2_actor *attacker, q2_actor *hit,
                          q2_actor **targets, u32 target_count,
                          const q2_combat_rules *rules)
{
    return q2_projectile_impact_traced(list, index, point, attacker,
                                       hit, targets, target_count, rules,
                                       NULL, NULL);
}

bool q2_projectile_impact_traced(q2_projectiles *list, u32 index,
                                 const s32 point[3],
                                 q2_actor *attacker, q2_actor *hit,
                                 q2_actor **targets, u32 target_count,
                                 const q2_combat_rules *rules,
                                 q2_combat_clear_fn clear, void *clear_ctx)
{
    q2_projectile *p;

    if (!list || index >= Q2_PROJ_MAX || !point)
        return false;
    p = &list->p[index];
    if (!p->in_use)
        return false;

    memcpy(p->pos, point, sizeof(p->pos));

    /* Grenade2/3 skip direct damage on an owner contact and pass no ignored
     * victim to radius damage (0x80049FAC -> 0x8004A024). Other contacts apply
     * direct damage first and exclude that victim from the following blast. */
    if ((p->kind == Q2_PROJ_GRENADE || p->kind == Q2_PROJ_HAND_GRENADE) &&
        hit == attacker)
        hit = NULL;
    if (hit && p->damage > 0)
        q2_combat_damage(attacker, hit, p->damage, p->mod, point, rules);

    /* The contact blast is the same 0x80050810 as the fuse's (the header's
     * table), so it is occluded the same way. */
    if (p->splash_radius > 0 && p->damage > 0)
        q2_combat_radius_damage_traced(attacker, hit, p->pos, p->damage,
                                       p->splash_radius, p->mod,
                                       targets, target_count, rules,
                                       clear, clear_ctx);

    release(list, index);
    return true;
}
