#include "combat.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Mod properties                                                             */
/* ------------------------------------------------------------------------- */
/*
 * The jump table at 0x800ACE1C, sixteen words indexed by mod-1. Each entry
 * lands on either 0x80058348 (`s0 = 1`) or 0x80058350 (`s0 = 0`), and `s0` is
 * the third argument to the armour routine — which reads the `+4` column when
 * it is set and the `+2` column when it is clear. So the flag is "this is
 * energy damage".
 *
 * Anything above 16 misses the table's `sltiu ..., 16` bound and falls through
 * with s0 already zero, so mods 17..21 — bullets among them — are ordinary.
 */
static const u8 k_mod_energy[Q2_MOD_COUNT] = {
    /*  0 */ 0,
    /*  1 */ 1, /*  2 */ 1, /*  3 */ 0, /*  4 */ 1,
    /*  5 */ 1, /*  6 */ 1, /*  7 */ 0, /*  8 */ 0,
    /*  9 */ 0, /* 10 */ 0, /* 11 */ 1, /* 12 */ 1,
    /* 13 */ 0, /* 14 */ 1, /* 15 */ 0, /* 16 */ 1,
    /* 17 */ 0, /* 18 */ 0, /* 19 */ 0, /* 20 */ 0, /* 21 */ 0
};

bool q2_mod_is_energy(s16 mod)
{
    if (mod < 0 || mod >= Q2_MOD_COUNT)
        return false;
    return k_mod_energy[mod] != 0;
}

bool q2_mod_knocks_back(s16 mod)
{
    /* 0x80057ED0..0x80057EE8, in the order the branches test them. */
    return mod == Q2_MOD_ROCKET || mod == Q2_MOD_GRENADE ||
           mod == Q2_MOD_RAIL   || mod == Q2_MOD_BULLET;
}

s16 q2_mod_effect_timer(s16 mod, int *slot)
{
    /* 0x800585A4..0x80058604. Four mods arm a timer, each in its own byte. */
    switch (mod) {
    case Q2_MOD_ENERGY_BOLT: if (slot) *slot = 1; return 3;
    case Q2_MOD_2:           if (slot) *slot = 0; return 15;
    case Q2_MOD_4:           if (slot) *slot = 2; return 30;
    case Q2_MOD_5:           if (slot) *slot = 4; return 5;
    default:                 if (slot) *slot = -1; return 0;
    }
}

bool q2_actor_energy_lit(const q2_actor *a)
{
    /* 0x80058650 reads entity+0x2F1, which combat.h maps to effect[1]; the
     * lit path is the `>= 3` arm at 0x80058660. */
    return a && a->effect[1] >= 3;
}

void q2_combat_rules_default(q2_combat_rules *r)
{
    if (!r)
        return;
    memset(r, 0, sizeof(*r));
    r->skill = 1;     /* not the lowest, so monster damage is not halved */
}

/* ------------------------------------------------------------------------- */
/* Actors                                                                     */
/* ------------------------------------------------------------------------- */
void q2_actor_init(q2_actor *a)
{
    int k;

    if (!a)
        return;
    memset(a, 0, sizeof(*a));
    a->owner         = -1;      /* not a player until a caller says so */
    a->last_attacker = -1;
    a->radius = 286;      /* entity+0x94, the live actor's X/Z radius */
    a->height = 572;      /* entity+0x96, from origin-286 to origin+286 */
    for (k = 0; k < 3; k++) {
        a->mins[k] = -286;
        a->maxs[k] =  286;
    }
}

void q2_actor_from_monster(q2_actor *a, const q2_monster *m)
{
    s32 radius;
    int k;

    if (!a || !m)
        return;
    q2_actor_init(a);
    a->origin[0] = m->pos[0];
    a->origin[1] = m->pos[1];
    a->origin[2] = m->pos[2];
    a->health     = m->health;
    a->gib_health = m->gib_health;
    a->has_client = false;
    a->is_monster = (m->svflags & Q2_SVF_MONSTER) != 0;
    a->has_enemy  = (m->enemy != NULL);

    /*
     * 0x800544EC clips X/Z against entity+0x94 and Y as a separate interval.
     * q2_monster owns the corresponding hull in this port. In particular,
     * q2_monster_corpse_detach makes it wider and much shorter; dropping these
     * six values here made a dead body keep its standing hit volume.
     */
    radius = 0;
    for (k = 0; k < 3; k++) {
        s32 lo, hi;

        a->mins[k] = m->mins[k];
        a->maxs[k] = m->maxs[k];
        if (k == 1)
            continue;
        lo = m->mins[k] < 0 ? -(s32)m->mins[k] : (s32)m->mins[k];
        hi = m->maxs[k] < 0 ? -(s32)m->maxs[k] : (s32)m->maxs[k];
        if (lo > radius) radius = lo;
        if (hi > radius) radius = hi;
    }
    if (radius > 0)
        a->radius = radius;
    {
        s32 height = (s32)m->maxs[1] - m->mins[1];

        /* The console quarters one 572-unit height field to 143. The port's
         * symmetric hull quarters -286 and +286 separately to -71/+71 and
         * therefore loses the odd unit; put it back in the actor projection. */
        if (m->corpse && height > 0 && m->mins[1] < 0 && m->maxs[1] > 0)
            height++;
        if (height > 0 && height <= 32767)
            a->height = (s16)height;
    }

    /*
     * AND WHETHER IT CAN BE HURT AT ALL, which never crossed this boundary.
     * `q2_monster.takedamage` is written by `monster_start` and by every
     * module's `die`; without it here the combat side had only `health` to
     * judge by, and health is the wrong question (see `nearest_hit`).
     */
    a->takedamage = m->takedamage;
}

void q2_actor_to_monster(const q2_actor *a, q2_monster *m)
{
    if (!a || !m)
        return;
    m->health = a->health;

    /*
     * IT NO LONGER RAISES `dead` ITSELF, and that is a fix rather than a
     * removal.
     *
     * The console raises the flag inside the creature's own `die`, and every
     * transcribed `*_die` opens with `if (self->dead) return;` — so a sync that
     * set it here handed the die handler a creature that was already dead and
     * the handler returned on its first instruction. The port worked around
     * that by clearing the flag around the call at the one call site, which is
     * a workaround for a bug that only exists because of this line.
     *
     * `q2_monster_damage_reaction` (monster.c) is now the only thing that sets
     * it, which is where T_Damage's own dispatch sets it too.
     */
}

void q2_actor_from_player(q2_actor *a, const q2_inventory *inv,
                          const s32 pos[3])
{
    s8 owner;

    if (!a)
        return;

    /*
     * WHICH PLAYER this is survives the refresh. `q2_actor_init` clears it to
     * -1, and this runs on every hit — so the first bolt that landed on a
     * player erased their identity, and the kill that followed was attributed
     * to the world. A staged pair produced exactly that: "player 1 killed by
     * -1", and because the runtime blames the victim for a world kill, player
     * 1 was docked a frag for being shot.
     */
    /*
     * And so does the one CLIENT-ONLY timer which has no inventory field:
     * `env_next` is client+0x94, the deadline that throttles acid and lava to
     * once per 400 and once per 100 ticks. Clearing it here made that throttle
     * unobservable: a hazard volume calls this every tick, each call reset the
     * deadline it had just armed, and 20 points of lava landed thirty times a
     * second instead of three. Standing in it killed a full-health player
     * inside a fifth of a second, which is fast enough to look like the volume
     * test being wrong rather than the throttle being erased.
     *
     * The two protection deadlines are different. They ARE inventory state:
     * client+0xB0 and +0xB4 are `invuln_until` and `enviro_until`, respectively.
     * The damage actor is a projection of that client record, so carrying its
     * stale copies back through this refresh silently made both pickups visual
     * only. Reload them from the inventory below, every damage attempt.
     */
    owner   = a->owner;
    {
        s32 env     = a->env_next;

        q2_actor_init(a);
        a->env_next      = env;
    }
    a->owner = owner;
    if (pos) {
        a->origin[0] = pos[0];
        a->origin[1] = pos[1];
        a->origin[2] = pos[2];
    }
    a->has_client = true;

    /*
     * A PLAYER IS DAMAGEABLE, and once the sweep honours `takedamage` that has
     * to be said out loud rather than assumed from a non-zero health. id's
     * `PutClientInServer` sets `takedamage = DAMAGE_AIM`; the console's client
     * arm of the damage router reaches T_Damage through the same gate every
     * other entity does.
     */
    a->takedamage = Q2_DAMAGE_AIM;

    if (!inv)
        return;
    a->health       = inv->health;
    a->armour       = inv->armour;
    a->armour_class = 0;
    a->cells        = inv->ammo[Q2_AMMO_CELLS];
    a->gib_health   = -100;
    a->invuln_until = inv->invuln_until;
    a->protect_until = inv->enviro_until;
}

void q2_actor_to_player(const q2_actor *a, q2_inventory *inv)
{
    if (!a || !inv)
        return;
    inv->health = a->health;
    inv->armour = a->armour;
    inv->ammo[Q2_AMMO_CELLS] = a->cells;
}

/* ------------------------------------------------------------------------- */
/* Armour                                                                     */
/* ------------------------------------------------------------------------- */
s16 q2_combat_power_armour_absorb(q2_actor *a, s16 damage)
{
    s32 save, cap;

    if (!a || damage <= 0 || !a->has_client)
        return 0;
    /* 0x80057AC0: both power items live in one bit pair of the powerup word. */
    if (!(a->powerups & Q2_POWERUP_POWER_ARMOUR))
        return 0;
    if (a->cells <= 0)
        return 0;

    /* 0x80057AE0: `(damage * 2) / 3`, signed, truncating. */
    save = ((s32)damage * Q2_POWER_ARMOUR_NUM) / Q2_POWER_ARMOUR_DEN;

    /* 0x80057AF4: capped at twice the cells held. */
    cap = (s32)a->cells * 2;
    if (save >= cap)
        save = cap;
    save = (s16)save;
    if (save <= 0)
        return 0;

    /* 0x80057B8C: one cell per two points absorbed, truncating. */
    a->cells = (s16)(a->cells - (save / Q2_POWER_ARMOUR_CELLS));
    if (a->cells < 0)
        a->cells = 0;

    return (s16)save;
}

s16 q2_combat_armour_absorb(q2_actor *a, s16 damage, bool energy,
                            bool power_armour_fired,
                            const q2_combat_rules *rules)
{
    const q2_weapon_tables *t = q2_weapon_tables_builtin();
    const q2_wt_armour *cls;
    s32 bias, factor, save;

    (void)power_armour_fired;   /* only suppresses the hit sound, not the maths */

    if (!a || damage <= 0 || !a->has_client)
        return 0;
    if (a->armour <= 0)
        return 0;
    if (a->armour_class >= Q2_WT_ARMOUR_CLASSES)
        return 0;

    /* 0x80057C0C: the bias is chosen by the same global the skill check uses.
     * 4095 rounds every non-zero fraction up; 2048 rounds to nearest. */
    bias = (rules && rules->deathmatch) ? Q2_ARMOUR_BIAS_DM : Q2_ARMOUR_BIAS_SP;

    cls    = &t->armour[a->armour_class];
    factor = energy ? cls->energy_protection : cls->normal_protection;

    /* 0x80057C7C: `(bias + factor * damage) >> 12`, a LOGICAL shift of a value
     * that cannot be negative here because both terms are non-negative. */
    save = (bias + factor * (s32)damage) >> 12;
    if (save > a->armour)
        save = a->armour;
    save = (s16)save;
    if (save <= 0)
        return 0;

    a->armour = (s16)(a->armour - save);
    return (s16)save;
}

/* ------------------------------------------------------------------------- */
/* Knockback                                                                  */
/* ------------------------------------------------------------------------- */
static s32 isqrt64(s64 v)
{
    s64 lo = 0, hi = 0x7FFFFFFF, best = 0;

    if (v <= 0)
        return 0;
    while (lo <= hi) {
        s64 mid = lo + (hi - lo) / 2;
        if (mid * mid <= v) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return (s32)best;
}

static void apply_knockback(q2_actor *attacker, q2_actor *target,
                            s16 damage, const s32 point[3],
                            const q2_combat_rules *rules)
{
    s64 dir[3];
    s32 len;
    s32 scale;
    s32 mass = rules ? rules->knockback_mass : 0;
    bool self = (attacker == target) && target->has_client;
    int i;

    dir[0] = (s64)target->origin[0] - point[0];
    dir[1] = (s64)target->origin[1] - point[1];
    dir[2] = (s64)target->origin[2] - point[2];

    /* 0x80057F28 normalises through 0x8008A588; the port does the same in
     * 1.3.12 so the multiply below keeps its scale. */
    len = isqrt64(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len <= 0)
        return;
    for (i = 0; i < 3; i++)
        dir[i] = (dir[i] * 4096) / len;

    /*
     * 0x80057F80: `125 * (mass + 64) / 64` for an ordinary hit, and
     * 0x80057FA8: `25 * (mass + 64) / 4` when a player hurt themselves.
     * The second is 3.2 times the first — the rocket jump.
     */
    if (self)
        scale = (25 * (mass + 64)) >> 2;
    else
        scale = (125 * (mass + 64)) >> 6;

    for (i = 0; i < 3; i++) {
        /*
         * 0x80057FC8..0x800580C4: `scale * unit[i] * damage / 2400 >> 4`, and
         * NOTHING divides the 1.3.12 scale back out — the unit vector's 4096 is
         * part of the impulse's magnitude. That is why the s16 clamps below are
         * reachable: anything past about 123 points of damage saturates.
         */
        s64 v = (s64)scale * dir[i];
        v = (v * damage) / Q2_KNOCKBACK_DIVISOR;
        v >>= Q2_KNOCKBACK_SHIFT;

        /* 0x800580E8: only the vertical component is floored, and only outside
         * deathmatch. World Y grows downward, so -3072 is a ceiling on how far
         * a blast underfoot can throw you — the single-player rocket jump has a
         * limit the deathmatch one does not. */
        if (i == 1 && !(rules && rules->deathmatch) && v < -3072)
            v = -3072;

        /* 0x800580F8..0x80058188: each component is clamped into s16. */
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;

        /* 0x80058188: a living target accumulates, a dead one is overwritten. */
        if (target->health > 0)
            target->knockback[i] += (s32)v;
        else
            target->knockback[i]  = (s32)v;
    }

    if (target->health > 0)
        target->knocked = true;
}

/* ------------------------------------------------------------------------- */
/* The damage function                                                        */
/* ------------------------------------------------------------------------- */
q2_damage_result q2_combat_damage(q2_actor *attacker, q2_actor *target,
                                  s16 damage, s16 mod, const s32 point[3],
                                  const q2_combat_rules *rules)
{
    q2_damage_result out;
    q2_combat_rules local;
    s32 amount = damage;
    s16 saved_power = 0, saved_armour = 0;
    bool was_alive;

    memset(&out, 0, sizeof(out));

    if (!target)
        return out;
    if (!rules) {
        q2_combat_rules_default(&local);
        rules = &local;
    }

    was_alive = target->health > 0;
    target->last_mod = mod;

    /* Who did it, so a scoring hook has a killer as well as a victim. The
     * engine's own byte is entity+222 and -1 there means the world. */
    target->last_attacker = attacker ? attacker->owner : (s8)-1;

    if (amount <= 0)
        return out;

    /* Knockback comes first and does not care about armour: 0x80057EC0 runs
     * before any absorption, and only when a point was supplied. */
    /* 0x8006291C: FL_NO_KNOCKBACK zeroes the impulse and nothing else. */
    if (point && q2_mod_knocks_back(mod) && !target->no_knockback)
        apply_knockback(attacker, target, damage, point, rules);

    /*
     * 0x800582C8: at skill 0, a monster hitting a player does half. The test is
     * on the ATTACKER having no client block, which is what makes it "a monster
     * hit you" rather than "you were hurt".
     */
    if (rules->skill == 0 && target->has_client &&
        attacker && !attacker->has_client)
        amount = (amount + 1) >> 1;

    /* Invulnerability and the second protection powerup: 0x80058244 and
     * 0x80058230 both return outright while the clock has not passed. */
    if (target->has_client) {
        if (rules->level_time < target->invuln_until ||
            rules->level_time < target->protect_until) {
            out.blocked = true;
            return out;
        }
    }

    /* The two environmental mods are throttled per target rather than per hit:
     * 0x80058268 sets the next allowed time 400 ticks out, 0x800582AC 100. */
    if (mod == Q2_MOD_ACID || mod == Q2_MOD_LAVA) {
        s32 gap = (mod == Q2_MOD_ACID) ? Q2_ENV_THROTTLE_ACID
                                       : Q2_ENV_THROTTLE_LAVA;
        if (target->has_client) {
            if (rules->level_time < target->env_next) {
                out.blocked = true;
                return out;
            }
            target->env_next = rules->level_time + gap;
        }
    }

    /* Armour. Mod 8 is the one class that skips both stages (0x80058358). */
    if (mod != Q2_MOD_NO_ARMOUR) {
        saved_power = q2_combat_power_armour_absorb(target, (s16)amount);
        amount -= saved_power;

        saved_armour = q2_combat_armour_absorb(target, (s16)amount,
                                               q2_mod_is_energy(mod),
                                               saved_power != 0, rules);
        amount -= saved_armour;
    }

    out.absorbed_power  = saved_power;
    out.absorbed_armour = saved_armour;

    if (amount <= 0)
        return out;

    /*
     * Both of these sit INSIDE T_Damage (0x800627F8), after the absorption the
     * caller has already done — so they scale what armour left, not what the
     * weapon started with. It makes no difference to a creature, which has no
     * armour, and it would to a player who ever acquired SVF_MONSTER.
     *
     * The surprise bonus, 0x800628C8-0x80062910: a monster that has not
     * acquired an enemy takes double from an attacker with a client block,
     * while it is still alive. Four conditions, in the original's order.
     */
    if (target->is_monster && !target->has_enemy && was_alive &&
        attacker && attacker->has_client) {
        amount *= 2;
        out.surprised = true;
    }

    /* 0x8006292C: godmode zeroes the damage. The engine's exemption is a
     * dflags bit (0x20) that no caller in this port sets. */
    if (target->godmode) {
        out.surprised = false;
        return out;
    }

    target->health = (s16)(target->health - amount);
    out.taken = (s16)amount;

    if (target->health <= 0) {
        /*
         * `killed` is the TRANSITION and stays behind `was_alive`; the other
         * two are properties of the hit and were wrongly behind it.
         *
         * THE FLOOR RUNS ON EVERY health<=0 OUTCOME, not only the first.
         * 0x800629B4 sits after the subtraction with no already-dead test in
         * front of it, and leaving it inside the transition let a corpse's s16
         * run down without limit: measured, 100 rockets at 300 points take a
         * body to -30040 and 110 take it to **+32496** — the field wraps and
         * the creature is alive again. The console cannot reach that.
         *
         * `gibbed` likewise. The module's own `die` does the real test — the
         * Soldier's at module+0x2324, before its already-dead guard — but this
         * flag is what the client and the effects read, so it has to be true
         * on the hit that takes an already-dead body past `gib_health` and not
         * only on the hit that killed it.
         */
        if (was_alive)
            out.killed = true;

        out.gibbed = target->health <= target->gib_health;

        if (target->health < Q2_HEALTH_FLOOR)
            target->health = (s16)Q2_HEALTH_FLOOR;
    }

    {
        int slot;
        s16 v = q2_mod_effect_timer(mod, &slot);
        if (slot >= 0 && slot < (int)(sizeof(target->effect)))
            target->effect[slot] = (u8)v;
    }

    return out;
}

/* ------------------------------------------------------------------------- */
/* Radius damage                                                              */
/* ------------------------------------------------------------------------- */
s16 q2_combat_splash_at(s16 damage, s32 dist)
{
    s32 loss = (s32)(((s64)dist * Q2_SPLASH_FALLOFF_NUM) >>
                     Q2_SPLASH_FALLOFF_SHIFT);
    s32 v = (s32)damage - loss;
    return v > 0 ? (s16)v : 0;
}

u32 q2_combat_radius_damage(q2_actor *attacker, q2_actor *ignore,
                            const s32 point[3], s16 damage, s16 radius,
                            s16 mod, q2_actor **targets, u32 count,
                            const q2_combat_rules *rules)
{
    u32 hurt = 0, i;

    if (!point || !targets || damage <= 0 || radius <= 0)
        return 0;

    for (i = 0; i < count; i++) {
        q2_actor *t = targets[i];
        s64 dx, dy, dz, d2;
        s64 reach;
        s32 dist;
        s16 points;

        if (!t || t == ignore)
            continue;

        dx = (s64)t->origin[0] - point[0];
        dy = (s64)t->origin[1] - point[1];
        dz = (s64)t->origin[2] - point[2];
        d2 = dx * dx + dy * dy + dz * dz;

        /* 0x800509AC: the comparison radius is the blast plus the target's own,
         * so a large creature is caught by a blast that misses its centre. */
        reach = (s64)radius + t->radius;
        if (d2 > reach * reach)
            continue;

        dist   = isqrt64(d2);
        points = q2_combat_splash_at(damage, dist);
        if (points <= 0)
            continue;

        q2_combat_damage(attacker, t, points, mod, point, rules);
        hurt++;
    }

    return hurt;
}

/* ------------------------------------------------------------------------- */
/* Tracing                                                                    */
/* ------------------------------------------------------------------------- */
s64 q2_combat_ray_dist_sq(const s32 origin[3], const s32 dir[3],
                          const s32 point[3], s64 *out_along)
{
    s64 vx, vy, vz;
    s64 len2, dot, along;
    s64 cx, cy, cz;

    if (out_along)
        *out_along = 0;
    if (!origin || !dir || !point)
        return 0;

    vx = (s64)point[0] - origin[0];
    vy = (s64)point[1] - origin[1];
    vz = (s64)point[2] - origin[2];

    len2 = (s64)dir[0] * dir[0] + (s64)dir[1] * dir[1] + (s64)dir[2] * dir[2];
    if (len2 <= 0)
        return vx * vx + vy * vy + vz * vz;

    dot = (s64)dir[0] * vx + (s64)dir[1] * vy + (s64)dir[2] * vz;

    /* Fraction along the ray, 1.0.12 — 4096 is the far end. Nothing is
     * normalised because the direction's LENGTH is the weapon's range. */
    along = (dot * 4096) / len2;
    if (out_along)
        *out_along = along;

    cx = vx - ((s64)dir[0] * along) / 4096;
    cy = vy - ((s64)dir[1] * along) / 4096;
    cz = vz - ((s64)dir[2] * along) / 4096;

    return cx * cx + cy * cy + cz * cz;
}

/*
 * Where a shot stopped considering a target. "It missed" has five causes and
 * only one of them is aim; counting them apart is the difference between fixing
 * the right thing and guessing three times, which is what this cost.
 */
q2_combat_scan_stats q2_combat_scan;
q2_combat_scan_stats q2_combat_scan_by[Q2_COMBAT_SCAN_SLOTS];
int                  q2_combat_scan_who = Q2_COMBAT_SCAN_OTHER;

/* Both the total and the shooter's own slot, so neither has to be derived. */
#define SCAN_BUMP(field)                                                          do {                                                                              q2_combat_scan.field++;                                                       if (q2_combat_scan_who >= 0 &&                                                    q2_combat_scan_who < Q2_COMBAT_SCAN_SLOTS)                                    q2_combat_scan_by[q2_combat_scan_who].field++;                        } while (0)

static s64 trace_isqrt(s64 v)
{
    s64 lo = 0, hi = 3037000499LL, best = 0;

    if (v <= 0)
        return 0;
    if (hi > v)
        hi = v;
    while (lo <= hi) {
        s64 mid = lo + (hi - lo) / 2;
        if (mid == 0 || mid <= v / mid) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return best;
}

/*
 * Normal retail inputs stay well inside this exact-integer envelope. A bullet
 * direction is the 4096 aim scaled by four (plus sub-4096 spread), rail is the
 * unscaled aim, and a target which survived the broad box can be only one
 * segment length plus its 286/429-unit radius away. Projectile tick segments
 * are shorter still. Keeping each horizontal term <= 30000 bounds cross^2,
 * radius^2*a, the discriminant and the fixed-root numerator below to s64.
 *
 * The public host API nevertheless accepts arbitrary s32 coordinates. Those
 * use the long-double arm instead of invoking signed-overflow UB; that arm is
 * not reached by retail-scale gameplay.
 */
#define Q2_TRACE_EXACT_TERM_MAX 30000

static bool trace_term_is_exact(s64 v)
{
    if (v < 0)
        v = -v;
    return v <= Q2_TRACE_EXACT_TERM_MAX;
}

static s64 trace_root_from_long_double(long double v)
{
    if (v >= 2147483647.0L)
        return 2147483647;
    if (v <= -2147483647.0L)
        return -2147483647;
    return (s64)v;               /* C truncates toward zero, as 0x800B11D8 */
}

/* 0x800546B0..0x80054704 rejects a centre whose dot along the sweep is not
 * positive. Only the sign is needed, so do not route arbitrary host s32 input
 * through q2_combat_ray_dist_sq's dot*4096 fraction. */
static bool actor_centre_is_ahead(const s32 origin[3], const s32 dir[3],
                                  const q2_actor *t)
{
    s64 rel[3];
    int k;
    bool exact = true;

    for (k = 0; k < 3; k++) {
        rel[k] = (s64)t->origin[k] - origin[k];
        if (!trace_term_is_exact(rel[k]) || !trace_term_is_exact(dir[k]))
            exact = false;
    }
    if (exact) {
        s64 dot = (s64)dir[0] * rel[0] + (s64)dir[1] * rel[1] +
                  (s64)dir[2] * rel[2];
        return dot > 0;
    }
    return (long double)dir[0] * rel[0] +
           (long double)dir[1] * rel[1] +
           (long double)dir[2] * rel[2] > 0.0L;
}

/*
 * The literal narrow phase in 0x800544EC.
 *
 * X/Z solve a quadratic against entity+0x94, producing the two entry/exit
 * roots in 1.0.12. Y is not part of that distance: it produces its own slab
 * interval, and the function intersects the two. That distinction is visible
 * at a box corner and after the corpse volume is made wider and shorter.
 */
static bool actor_cylinder_interval(const s32 origin[3], const s32 dir[3],
                                    const q2_actor *t, s32 fallback_radius,
                                    s64 *out_enter, s64 *out_exit)
{
    s64 ox, oz, a, b, disc;
    s64 h_enter, h_exit, v_enter, v_exit;
    s64 radius;
    s64 ymin, ymax;

    if (!origin || !dir || !t)
        return false;

    radius = t->radius > 0 ? t->radius : fallback_radius;
    if (radius <= 0)
        return false;

    /* 0x800545F4..0x8005467C: the cheap swept-box rejection precedes the
     * quadratic. Besides saving the solve, this defines the degenerate
     * vertical-ray case where the horizontal quadratic has a == 0. */
    {
        s64 end[3];
        s64 xmin, xmax, zmin, zmax;

        end[0] = (s64)origin[0] + dir[0];
        end[1] = (s64)origin[1] + dir[1];
        end[2] = (s64)origin[2] + dir[2];
        xmin = origin[0] < end[0] ? origin[0] : end[0];
        xmax = origin[0] > end[0] ? origin[0] : end[0];
        zmin = origin[2] < end[2] ? origin[2] : end[2];
        zmax = origin[2] > end[2] ? origin[2] : end[2];
        if (xmax < (s64)t->origin[0] - radius ||
            xmin > (s64)t->origin[0] + radius ||
            zmax < (s64)t->origin[2] - radius ||
            zmin > (s64)t->origin[2] + radius)
            return false;
    }

    ox = (s64)origin[0] - t->origin[0];
    oz = (s64)origin[2] - t->origin[2];

    if (dir[0] == 0 && dir[2] == 0) {
        /* The retail solver explicitly returns 0..4096 here. The broad box
         * above is therefore the exact horizontal test for a vertical ray. */
        h_enter = -(s64)0x7FFFFFFF;
        h_exit  =  (s64)0x7FFFFFFF;
    } else if (trace_term_is_exact(dir[0]) &&
               trace_term_is_exact(dir[2]) &&
               trace_term_is_exact(ox) && trace_term_is_exact(oz) &&
               trace_term_is_exact(radius)) {
        s64 cross;
        s64 root;
        s64 denom;

        a = (s64)dir[0] * dir[0] + (s64)dir[2] * dir[2];
        b = 2 * ((s64)dir[0] * ox + (s64)dir[2] * oz);

        cross = (s64)dir[0] * oz - (s64)dir[2] * ox;
        /* b^2 - 4ac == 4(r^2*a - cross^2). This equivalent form avoids
         * overflowing on the two large, nearly cancelling b^2/4ac terms. */
        /* Test the unscaled discriminant first. A legal host actor can have a
         * broad-box-overlapping but very off-axis centre; multiplying that
         * negative value by four before rejecting it can overflow s64 even
         * though all retail-scale hits remain exact. The positive arm is
         * bounded by Q2_TRACE_EXACT_TERM_MAX and is safe to scale. */
        disc = radius * radius * a - cross * cross;

        /* 0x80054768 uses a strict comparison for the tangent case. */
        if (disc <= 0)
            return false;
        disc *= 4;

        root  = trace_isqrt(disc);
        denom = 2 * a;
        /* 0x800B11D8 receives +/-2048 and divides by a, i.e. these
         * (-b +/- sqrt(d)) * 4096 / (2a) roots, truncated toward zero. */
        h_enter = ((-b - root) * 4096) / denom;
        h_exit  = ((-b + root) * 4096) / denom;
    } else {
        long double la = (long double)dir[0] * dir[0] +
                         (long double)dir[2] * dir[2];
        long double lb = 2.0L * ((long double)dir[0] * ox +
                                 (long double)dir[2] * oz);
        long double lc = (long double)ox * ox +
                         (long double)oz * oz -
                         (long double)radius * radius;
        long double ld = lb * lb - 4.0L * la * lc;
        long double root;

        if (ld <= 0.0L)
            return false;
        root = sqrtl(ld);
        h_enter = trace_root_from_long_double(
            (-lb - root) * 4096.0L / (2.0L * la));
        h_exit = trace_root_from_long_double(
            (-lb + root) * 4096.0L / (2.0L * la));
    }
    if (h_enter > h_exit)
        return false;

    /* 0x80054834..0x8005483C builds exactly these two endpoints: the lower
     * face is always origin.y + 286, and entity+0x96 reaches upward from it. */
    ymax = (s64)t->origin[1] + 286;
    ymin = ymax - (t->height > 0
                       ? t->height
                       : (s32)t->maxs[1] - t->mins[1]);

    if (dir[1] > 0) {
        v_enter = ((ymin - origin[1]) * 4096) / dir[1];
        v_exit  = ((ymax - origin[1]) * 4096) / dir[1];
    } else if (dir[1] < 0) {
        s64 denom = -(s64)dir[1];
        v_enter = (((s64)origin[1] - ymax) * 4096) / denom;
        v_exit  = (((s64)origin[1] - ymin) * 4096) / denom;
    } else {
        if ((s64)origin[1] < ymin || (s64)origin[1] > ymax)
            return false;
        v_enter = -(s64)0x7FFFFFFF;
        v_exit  =  (s64)0x7FFFFFFF;
    }

    if (v_enter > h_enter)
        h_enter = v_enter;
    if (v_exit < h_exit)
        h_exit = v_exit;
    if (h_enter > h_exit)
        return false;

    if (out_enter)
        *out_enter = h_enter;
    if (out_exit)
        *out_exit = h_exit;
    return true;
}

/* Shared scan: the nearest actor whose cylinder the segment crosses within
 * the fraction the world allows. Returns its index, or -1. */
static s32 nearest_hit(const s32 origin[3], const s32 dir[3],
                       s32 world_fraction, s32 fallback_radius,
                       q2_actor **targets, u32 count, u32 skip_mask_index,
                       s64 *out_along)
{
    s32 best = -1;
    s64 best_along = 0;
    u32 i;

    for (i = 0; i < count; i++) {
        q2_actor *t = targets[i];
        s64 along = 0, leave = 0;

        SCAN_BUMP(tested);
        if (!t || i == skip_mask_index) {
            SCAN_BUMP(skipped);
            continue;
        }
        /*
         * NOT `health <= 0`. HEALTH IS NOT HOW THE CONSOLE DECIDES WHAT CAN BE
         * SHOT — `takedamage` is, and that is the whole reason a corpse can be
         * blown apart.
         *
         * The console's own entity sweep (0x800544EC, called from the hitscan
         * at 0x8004891C) filters on identity, bbox overlap, being ahead, the
         * perpendicular radius, the fraction band and the vertical slab — and
         * on NOTHING else. No health, no svflags, no solid. `findradius`
         * (0x8005FA28) filters on solid and inuse, again not health. What
         * rejects a shot is T_Damage's own first test, five instructions in:
         * `lw v1, 28(targ)` / `and v1, 0xC0000000` / `beq v1, zero, epilogue`
         * at 0x80062838..0x80062848.
         *
         * And a creature is DAMAGE_AIM from `monster_start` (0x80061A94) and
         * downgraded to DAMAGE_YES by every module's own `die` on its normal
         * death arm — the Soldier at module+0x23B4, the Gunner at
         * module+0x1780 — which is exactly so that the body stays shootable.
         * Filtering on health here threw that away and made the corpse
         * untouchable, which is why nothing could ever be gibbed.
         *
         * Filtering the sweep on `takedamage` is the two rules composed: the
         * console's sweep has no filter and its T_Damage rejects on this bit,
         * so rejecting here reaches the same set without calling damage on a
         * target that would refuse it.
         */
        if (!t->takedamage) {
            SCAN_BUMP(dead);
            continue;
        }

        if (!actor_centre_is_ahead(origin, dir, t)) {
            SCAN_BUMP(behind);
            continue;
        }

        if (!actor_cylinder_interval(origin, dir, t, fallback_radius,
                                     &along, &leave) || leave <= 0 ||
            along > 4096) {
            SCAN_BUMP(off_axis);
            continue;
        }
        if (along < 0)
            along = 0;
        if (along > world_fraction) {
            SCAN_BUMP(beyond_world);
            continue;
        }

        SCAN_BUMP(hit);

        if (best < 0 || along < best_along) {
            best = (s32)i;
            best_along = along;
        }
    }

    if (out_along)
        *out_along = best_along;
    return best;
}

q2_damage_result q2_combat_melee(q2_actor *attacker, q2_actor *target,
                                 s16 damage, const q2_combat_rules *rules)
{
    q2_damage_result out;

    memset(&out, 0, sizeof(out));
    if (!attacker || !target)
        return out;

    /* 0x800612F0 passes the attacker's own origin as the damage point, so a
     * melee hit lands with mod 7 — which is not in the knockback set, and so
     * a creature's claws move nothing. */
    return q2_combat_damage(attacker, target, damage, Q2_MOD_MELEE,
                            attacker->origin, rules);
}

s32 q2_combat_nearest_on_segment(const s32 origin[3], const s32 dir[3],
                                 s32 fallback_radius, q2_actor **targets,
                                 u32 count)
{
    if (!origin || !dir || !targets)
        return -1;
    return nearest_hit(origin, dir, 4096, fallback_radius, targets, count,
                       (u32)-1, NULL);
}

s32 q2_combat_fire_bullet(q2_actor *attacker, const s32 origin[3],
                          const s32 dir[3], s16 damage, s32 world_fraction,
                          s32 fallback_radius, q2_actor **targets, u32 count,
                          const q2_combat_rules *rules,
                          q2_damage_result *out)
{
    s32 idx;
    s64 along = 0;
    s32 point[3];
    int k;

    if (out)
        memset(out, 0, sizeof(*out));
    if (!origin || !dir || !targets)
        return -1;
    if (world_fraction <= 0 || world_fraction > 4096)
        world_fraction = (world_fraction <= 0) ? 0 : 4096;

    idx = nearest_hit(origin, dir, world_fraction, fallback_radius,
                      targets, count, (u32)-1, &along);
    if (idx < 0)
        return -1;

    for (k = 0; k < 3; k++)
        point[k] = origin[k] + (s32)(((s64)dir[k] * along) / 4096);

    {
        q2_damage_result r = q2_combat_damage(attacker, targets[idx], damage,
                                              Q2_MOD_BULLET, point, rules);
        if (out)
            *out = r;
    }
    return idx;
}

u32 q2_combat_fire_rail(q2_actor *attacker, const s32 origin[3],
                        const s32 dir[3], s16 damage, s32 world_fraction,
                        s32 fallback_radius, q2_actor **targets, u32 count,
                        const q2_combat_rules *rules)
{
    u32 hit = 0, i;

    if (!origin || !dir || !targets)
        return 0;
    if (world_fraction <= 0 || world_fraction > 4096)
        world_fraction = (world_fraction <= 0) ? 0 : 4096;

    /* The rail does not stop in this port: retain its established target-list
     * order while replacing only the volume test. */
    for (i = 0; i < count; i++) {
        q2_actor *t = targets[i];
        s64 along = 0, leave = 0;
        s32 point[3];
        int k;

        /* The same rule as `nearest_hit` above, and for the same reason. */
        if (!t || !t->takedamage)
            continue;

        if (!actor_centre_is_ahead(origin, dir, t))
            continue;
        if (!actor_cylinder_interval(origin, dir, t, fallback_radius,
                                     &along, &leave) || leave <= 0)
            continue;
        if (along < 0)
            along = 0;
        if (along > world_fraction || along > 4096)
            continue;

        for (k = 0; k < 3; k++)
            point[k] = origin[k] + (s32)(((s64)dir[k] * along) / 4096);

        q2_combat_damage(attacker, t, damage, Q2_MOD_RAIL, point, rules);
        hit++;
    }

    return hit;
}
