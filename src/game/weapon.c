#include "weapon.h"

#include <string.h>

#include "combat.h"
#include "shock.h"
#include "trig.h"

/* ------------------------------------------------------------------------- */
/*
 * The eleven fire functions, transcribed.
 *
 * Column by column, with the instruction each immediate came from:
 *
 *   blaster       damage 8/32      0x8004C090 / 0x8004C0B8   kick -11
 *   shotgun       damage 6/24      0x8004C2EC / 0x8004C314   kick -22   x5
 *   supershotgun  damage 8/32      0x8004C5B0 / 0x8004C5D8   kick -22   x10
 *   machinegun    damage 8/32      0x8004C7C4 / 0x8004C8CC   kick rand
 *   chaingun      damage 8/32      0x8004CB20 / 0x8004CC14   kick rand
 *   handgrenade   damage 120/480   0x8004EC2C / 0x8004EC30
 *   grenadelaunch damage 120/480   0x8004CE84 / 0x8004CEAC   kick -11
 *   rocketlaunch  damage computed  0x8004D0F8                kick -11
 *   hyperblaster  damage 20/80     0x8004D35C / 0x8004D384   kick   0
 *   railgun       damage computed  0x8004D5D8                kick -34
 *   bfg           damage 200/800   0x8004EB60 / 0x8004EB64
 *
 * Refire is `level_time + 30` at 0x8004C000, 0x8004C258, 0x8004C51C,
 * 0x8004C814, 0x8004CB5C, 0x8004CEA0, 0x8004D0C0 and 0x8004D5BC. The
 * hyperblaster writes none: its rate is the view model animation's.
 */
/*
 * THE 30-TICK GATE IS NOT THE CONSOLE'S, and it was in every row of this table.
 *
 * `weapon.h` above says "thirty ticks, written as `level_time + 30`, for every
 * weapon that sets it". The write is real — the blaster's is at 0x8004BFFC,
 * `addiu v0, s0, 30; sw v0, 204(s1)` — and NOTHING READS CLIENT+204 anywhere in
 * the weapon path. It is a value the fire functions store and the engine never
 * consults.
 *
 * What actually limits the rate is the animation. On the console the IDLE arm
 * of the view-model machine calls the fire function itself (`jalr` at
 * 0x8004FB30) and only then enters FIRE, so a weapon fires once per fire clip
 * — the blaster's is 110 ticks — and the four weapons that need to be faster
 * have their own frame driver at 0x8004FEE8 to make it so.
 *
 * With a 30-tick gate on top the port fired every weapon at ten a second
 * regardless of its clip, which is the "reload times and general handling"
 * the report is about. Every row is zero now and the clip is the rate, which
 * is what the field's own comment ("0 when the animation drives the rate")
 * has said all along.
 *
 * The dry click keeps its own deadline — see Q2_WEAPON_DRY_REFIRE. That one is
 * the port's, because the port asks the fire function every tick and the
 * console asks it once per pass.
 */
/*
 * The two columns after `kick_random` are which kick fields the function
 * stores (weapon.h, "View kick") and the id it hands SetShockData (shock.h).
 */
const q2_weapon_behaviour q2_weapon_behaviour_table[Q2_WT_SLOTS] = {
    /* 0 — no weapon. 0x8004EB08 is `jr ra; nop`. */
    { Q2_FK_NONE, 0, 0, 0, {{0,0,0}}, 0, false, 0, 0, 0, Q2_SHOCK_NONE,
      0x8004EB08u },

    /* 1 blaster — a bolt entity, no ammo, no scatter. Mod 6 is the sixth
     * argument its call site passes to the spawner (0x8004C124). */
    { Q2_FK_BOLT, 8, 32, 1, {{0,0,0}}, -11, false, Q2_KICK_PITCH, 0,
      Q2_MOD_BOLT, Q2_SHOCK_BLASTER, 0x8004BFBCu },

    /* 2 shotgun — five pellets, +-1024 on every axis. */
    { Q2_FK_BULLET, 6, 24, 5, {{4,4,4}}, -22, false, Q2_KICK_PITCH, 0,
      Q2_MOD_BULLET, Q2_SHOCK_SHOTGUN, 0x8004C1C0u },

    /* 3 super shotgun — ten pellets, +-2048 across and +-1024 otherwise.
     * 0x8004C5F0 shifts by 3 where 0x8004C60C and 0x8004C634 shift by 4. */
    { Q2_FK_BULLET, 8, 32, 10, {{3,4,4}}, -22, false, Q2_KICK_PITCH, 0,
      Q2_MOD_BULLET, Q2_SHOCK_SSHOTGUN, 0x8004C488u },

    /* 4 machinegun — one bullet, +-512, a random kick on all three axes. */
    { Q2_FK_BULLET, 8, 32, 1, {{5,5,5}}, 0, true, Q2_KICK_ALL, 0,
      Q2_MOD_BULLET, Q2_SHOCK_MACHINEGUN, 0x8004C744u },

    /* 5 chaingun — as the machinegun, but the pellet count comes from the
     * spin state the caller passes in, and the random kick is yaw and roll
     * only. */
    { Q2_FK_BULLET, 8, 32, 0, {{5,5,5}}, 0, true,
      Q2_KICK_YAW | Q2_KICK_ROLL, 0,
      Q2_MOD_BULLET, Q2_SHOCK_CHAINGUN, 0x8004CA9Cu },

    /* 6 hand grenade — the shock is the PRIME (7); the throw's (8) belongs to
     * the held entity's release at 0x8004A52C. */
    { Q2_FK_HAND_GRENADE, 120, 480, 1, {{0,0,0}}, 0, false, 0, 0,
      Q2_MOD_GRENADE, Q2_SHOCK_GRENADE_OPEN, 0x8004EBDCu },

    /* 7 grenade launcher. */
    { Q2_FK_GRENADE, 120, 480, 1, {{0,0,0}}, -11, false, Q2_KICK_PITCH, 0,
      Q2_MOD_GRENADE, Q2_SHOCK_GRENADEL, 0x8004CE18u },

    /* 8 rocket launcher — damage is 100 + rand() % 20, quad shifts it left
     * by two rather than picking a second immediate. */
    { Q2_FK_ROCKET, 0, 0, 1, {{0,0,0}}, -11, false, Q2_KICK_PITCH, 0,
      Q2_MOD_ROCKET, Q2_SHOCK_ROCKETL, 0x8004D038u },

    /* 9 hyperblaster — no kick, no refire gate of its own. Mod 5, from its
     * call site's sixth argument (0x8004D3F0). */
    { Q2_FK_BOLT, 20, 80, 1, {{0,0,0}}, 0, false, 0, 0,
      Q2_MOD_BOLT_HYPER, Q2_SHOCK_HYPERBLASTER, 0x8004D250u },

    /* 10 railgun — 100, or 150 in deathmatch; quad shifts left by two. */
    { Q2_FK_RAIL, 0, 0, 1, {{0,0,0}}, -34, false, Q2_KICK_PITCH, 0,
      Q2_MOD_RAIL, Q2_SHOCK_RAILGUN, 0x8004D498u },

    /* 11 BFG — fifty cells a shot, the most expensive in the game. */
    { Q2_FK_BFG, 200, 800, 1, {{0,0,0}}, 0, false, 0, 0,
      Q2_MOD_ENERGY_BOLT, Q2_SHOCK_BFG, 0x8004EB10u },

    /* 12 — the phantom slot the cycle scan walks into. */
    { Q2_FK_NONE, 0, 0, 0, {{0,0,0}}, 0, false, 0, 0, 0, Q2_SHOCK_NONE, 0 }
};

/* ------------------------------------------------------------------------- */
/* The engine's deterministic generator                                      */
/* ------------------------------------------------------------------------- */
void q2_rng_seed(q2_rng *r, u32 seed)
{
    if (r)
        r->state = seed;
}

void q2_creature_muzzle_light(s32 rand_a, s32 rand_b, s32 *inner, s32 *outer)
{
    if (inner)
        *inner = ((rand_a * Q2_CRE_MUZZLE_INNER_SCALE) >> 15)
                 + Q2_CRE_MUZZLE_INNER_BASE;
    if (outer)
        *outer = ((rand_b * Q2_CRE_MUZZLE_OUTER_SCALE) >> 15)
                 + Q2_CRE_MUZZLE_OUTER_BASE;
}

bool q2_weapon_has_muzzle_light(u8 weapon_id)
{
    return weapon_id == Q2_WID_MACHINEGUN || weapon_id == Q2_WID_CHAINGUN;
}

void q2_weapon_muzzle_light(u8 weapon_id, s32 rand_a, s32 rand_b,
                            s32 *inner, s32 *outer)
{
    bool cg = weapon_id == Q2_WID_CHAINGUN;

    if (inner)
        *inner = ((rand_a * (cg ? Q2_CG_MUZZLE_INNER_SCALE
                                : Q2_MUZZLE_INNER_SCALE)) >> 15)
                 + (cg ? Q2_CG_MUZZLE_INNER_BASE : Q2_MUZZLE_INNER_BASE);
    if (outer)
        *outer = ((rand_b * (cg ? Q2_CG_MUZZLE_OUTER_SCALE
                                : Q2_MUZZLE_OUTER_SCALE)) >> 15)
                 + (cg ? Q2_CG_MUZZLE_OUTER_BASE : Q2_MUZZLE_OUTER_BASE);
}

s32 q2_rng_next(q2_rng *r)
{
    if (!r)
        return 0;
    /*
     * This IS the engine's generator, not merely one with the same range.
     *
     * 0x80089E28 is `addiu t2, zero, 160; jr t2` with `t1 = 0x2F` — a jump to
     * the PlayStation BIOS A-function table, entry 0x2F, which is `rand()`. The
     * BIOS implements it as `x = x*0x41C64E6D + 0x3039; return (x>>16) & 0x7FFF`,
     * and the constants below are 0x41C64E6D and 0x3039 written in decimal.
     *
     * So given the same seed this reproduces the console's sequence exactly, and
     * anything that needs the engine's randomness to be RIGHT rather than merely
     * plausible — FLKLIGHT's on/off times, for one — can be built on it. An
     * earlier version of this comment said "any full-period generator does",
     * which undersold what was already here.
     */
    r->state = r->state * 1103515245u + 12345u;
    return (s32)((r->state >> 16) & 0x7FFFu);
}

/* ------------------------------------------------------------------------- */
/* Selection                                                                  */
/* ------------------------------------------------------------------------- */
bool q2_weapon_usable(const q2_inventory *inv, int id)
{
    const q2_weapon_tables *t = q2_weapon_tables_builtin();
    s32 need, type;

    if (!inv || id <= 0 || id >= Q2_WT_SLOTS)
        return false;
    if (!(inv->weapons & t->owned_bit[id]))
        return false;

    need = t->ammo_per_shot[id];
    type = t->ammo_type[id];
    if (need <= 0)
        return true;                 /* the blaster */
    if (type < 0 || type >= Q2_AMMO_COUNT)
        return false;

    /* `slt v0, ammo, need` at 0x80050730 — strictly less fails, so exactly
     * enough for one shot still counts as usable. */
    return inv->ammo[type] >= (s16)need;
}

int q2_weapon_cycle(const q2_inventory *inv, int current_id, int dir)
{
    int step = (dir >= 0) ? 1 : -1;
    int id   = current_id;
    int guard;

    if (!inv)
        return Q2_WID_NONE;

    /*
     * 0x80050758. The scan wraps over 1..12, not 1..11: `bgtz` sends a
     * non-positive index to 12 and `slti ..., 13` sends 13 or more to 1. Slot
     * 12's owned-bit is zero so it is always rejected, but the wrap point is
     * what decides which weapon "previous" lands on from the blaster.
     */
    for (guard = 0; guard <= Q2_WID_CYCLE_MAX + 1; guard++) {
        id += step;
        if (id <= 0)
            id = Q2_WID_CYCLE_MAX;
        else if (id >= Q2_WID_CYCLE_MAX + 1)
            id = Q2_WID_BLASTER;

        if (!q2_weapon_usable(inv, id))
            continue;

        /* 0x800507F0: coming back to what is already held means "no change". */
        if (id == current_id)
            return Q2_WID_NONE;
        return id & 0xFF;
    }

    return Q2_WID_NONE;
}

void q2_weapon_refund(q2_inventory *inv, int weapon_id)
{
    const q2_weapon_tables *tab = q2_weapon_tables_builtin();
    u32 need;
    s8  type;

    if (!inv || weapon_id <= 0 || weapon_id > Q2_WID_COUNT)
        return;

    need = tab->ammo_per_shot[weapon_id];
    type = (s8)tab->ammo_type[weapon_id];

    if (need > 0 && type >= 0 && type < Q2_AMMO_COUNT)
        inv->ammo[type] = (s16)(inv->ammo[type] + (s16)need);
}

bool q2_weapon_consume(q2_inventory *inv, int weapon_id)
{
    const q2_weapon_tables *tab = q2_weapon_tables_builtin();
    u32 need;
    s8  type;

    if (!inv || weapon_id <= 0 || weapon_id > Q2_WID_COUNT)
        return false;

    need = tab->ammo_per_shot[weapon_id];
    type = (s8)tab->ammo_type[weapon_id];
    if (need == 0)
        return true;
    if (type < 0 || type >= Q2_AMMO_COUNT || inv->ammo[type] < (s16)need)
        return false;

    inv->ammo[type] = (s16)(inv->ammo[type] - (s16)need);
    return true;
}

int q2_weapon_autoselect(const q2_inventory *inv)
{
    const q2_weapon_tables *t = q2_weapon_tables_builtin();
    u32 i;

    if (!inv)
        return Q2_WID_NONE;

    /* 0x800506C4 walks the list until one is owned AND fed. */
    for (i = 0; i < t->autoswitch_count; i++) {
        int id = t->autoswitch[i];
        if (q2_weapon_usable(inv, id))
            return id;
    }
    return Q2_WID_NONE;
}

/* ------------------------------------------------------------------------- */
/* Muzzle                                                                     */
/* ------------------------------------------------------------------------- */
void q2_weapon_muzzle_origin(const s16 muzzle[3], const s32 eye[3],
                             s32 yaw, s32 pitch, s32 roll, s32 out[3])
{
    s16 rot[3][3];
    int i;

    if (!out)
        return;
    if (!muzzle || !eye) {
        if (eye) { out[0] = eye[0]; out[1] = eye[1]; out[2] = eye[2]; }
        return;
    }

    /*
     * THE CAMERA'S OWN MATRIX, APPLIED AS ITS TRANSPOSE — the same three lines
     * q2_vw_place already uses to put the weapon MODEL in the world, and that
     * is the whole correctness argument: the shot has to leave from the point
     * the picture draws the barrel at, and the picture is drawn with
     * q2_rotation_view(cam->yaw, cam->pitch, cam->roll) (world.c, modeldraw.c).
     * Applying that matrix's transpose is what makes the two agree.
     *
     * WHAT WAS HERE, and why 28 passing tests did not catch it. The basis was
     * hand-rolled as right/up/fwd, and the `up` row was the view matrix's row 1
     * with ONLY its middle component negated: (sp*sy, -cp, sp*cy) against the
     * matrix's (sp*sy, +cp, sp*cy). Two things followed.
     *
     *   - The vertical offset went the wrong way. muzzle[1] is DOWN (see
     *     weapontables.h), so the muzzle was placed ABOVE the eye instead of
     *     below it: an error of 2*cos(pitch)*muzzle[1], which is 112 world
     *     units for the blaster and 280 for the hyperblaster.
     *   - Flipping one component of three left the basis NOT ORTHONORMAL.
     *     dot(up, fwd) = 2*sin(p)*cos(p), so at a 45-degree pitch `up` and
     *     `fwd` are the identical vector and the whole vertical part of the
     *     offset is applied along forward instead.
     *
     * The test only ever exercised pitch 0 — the one pitch at which the
     * hand-rolled basis happens to be orthonormal — and pinned the inverted
     * sign as the expected answer.
     *
     * ON THE ROLL. The fire functions apply RotateS (0x8006FC1C) with the
     * player's own matrix at +0x2C0, which 0x8003AB60 builds with RotMatrix
     * from the entity's angles just after storing the strafe roll
     * (0x8003AB38). RotMatrix is Ry*Rx*Rz with the roll innermost, and with
     * the Y negations the blaster applies around it (0x8004C01C, 0x8004C04C)
     * the composition is Ry(yaw)Rx(pitch)Rz(-roll) — this transpose's, so the
     * roll is the console's. An earlier note here read RotMatrix as Rz*Ry*Rx
     * and called the two different; trig.c has since corrected that reading.
     *
     * What still differs is ROUNDING: RotateS floors each row in the frame
     * whose Y is negated, this floors in the unnegated one, so a component can
     * land one unit apart (the blaster at yaw 0, pitch 300 gives Y -60 there
     * and -61 here). Not reproduced without a native fixture to pin it.
     */
    q2_rotation_view(rot, yaw, pitch, roll);

    for (i = 0; i < 3; i++) {
        s64 v = (s64)rot[0][i] * muzzle[0]
              + (s64)rot[1][i] * muzzle[1]
              + (s64)rot[2][i] * muzzle[2];
        out[i] = eye[i] + (s32)(v >> 12);
    }
}

/* ------------------------------------------------------------------------- */
/* Firing                                                                     */
/* ------------------------------------------------------------------------- */

/* `((rand & 0x7FFF) - 16384) >>> n`, truncated to the s16 the kick field holds.
 * The shift is LOGICAL in the original (0x8004C7D0 is `srl`, not `sra`), so a
 * negative value becomes a large positive one and only survives as a small
 * negative number because the store is a halfword. Reproduced, not corrected. */
static s16 kick_draw(q2_rng *rng, int shift)
{
    u32 v = (u32)((q2_rng_next(rng) & 0x7FFF) - 16384);
    return (s16)(u16)(v >> shift);
}

/* `rand() - 16384`, arithmetic-shifted: the scatter draw. */
static s32 spread_draw(q2_rng *rng, int shift)
{
    s32 v = q2_rng_next(rng) - 16384;
    return v >> shift;
}

q2_fire_result_v2 q2_weapon_fire(q2_inventory *inv, q2_rng *rng,
                                 const q2_weapon_tables *tab,
                                 int weapon_id,
                                 const s32 eye[3], s32 yaw, s32 pitch, s32 roll,
                                 const s16 aim[3],
                                 s32 now, s32 next_fire,
                                 bool quad, bool deathmatch,
                                 int chaingun_bullets)
{
    q2_fire_result_v2 res;
    const q2_weapon_behaviour *w;
    s32 origin[3];
    s16 damage;
    int pellets, i;
    s32 need, type;

    memset(&res, 0, sizeof(res));
    res.sound     = -1;
    res.next_fire = next_fire;

    if (!inv || !eye || weapon_id <= 0 || weapon_id >= Q2_WT_SLOTS)
        return res;
    if (!tab)
        tab = q2_weapon_tables_builtin();

    w = &q2_weapon_behaviour_table[weapon_id];
    res.kind = w->kind;
    res.mod  = w->mod;

    if (w->kind == Q2_FK_NONE)
        return res;

    /* The refire gate is checked before the ammo, so a shot blocked by the
     * clock costs nothing. */
    if (now < next_fire)
        return res;

    if (!(inv->weapons & tab->owned_bit[weapon_id]))
        return res;

    need = tab->ammo_per_shot[weapon_id];
    type = tab->ammo_type[weapon_id];

    /* 0x8004CB18 spends the burst count, not the table's one-round minimum.
     * The view driver clamps it to available ammo at 0x80050200. Keep that
     * clamp here as well so direct sim callers cannot create free rounds. */
    pellets = w->pellets;
    if (weapon_id == Q2_WID_CHAINGUN) {
        pellets = chaingun_bullets;
        if (pellets < 0) pellets = 0;
        if (pellets > 3) pellets = 3;
        if (type >= 0 && type < Q2_AMMO_COUNT && inv->ammo[type] > 0 &&
            pellets > inv->ammo[type])
            pellets = inv->ammo[type];
    }

    /* The BFG's `sltiu v0, cells, 50` at 0x8004EB38 is the same shape as every
     * other weapon's `bne ammo, zero` — a shot needs its whole cost up front. */
    if (need > 0) {
        if (type < 0 || type >= Q2_AMMO_COUNT || inv->ammo[type] < (s16)need) {
            res.dry   = true;
            res.sound = Q2_WSND_NO_AMMO;
            /*
             * AND THE DRY PATH TAKES THE GATE TOO, which it did not: it
             * returned before `res.next_fire` was advanced, so an empty weapon
             * re-ran this branch on every single tick. That was invisible for
             * as long as nothing consumed `res.sound`; the moment the no-ammo
             * click has a speaker it becomes a buzz at tick rate.
             *
             * The console cannot show us a figure for this — its idle state
             * only asks the fire function once per fire pass, so it has no
             * dry deadline to read. Q2_WEAPON_DRY_REFIRE is therefore the
             * port's own, and marked as such.
             */
            res.next_fire = now + Q2_WEAPON_DRY_REFIRE;
            return res;
        }
        /* Grenade3 is paid for at RELEASE, not at prime. 0x8004A7C0 is in
         * the held entity's 411-crossing arm, after the throw sound and just
         * before state 3. Every other fire function spends here. */
        if (w->kind != Q2_FK_HAND_GRENADE)
            inv->ammo[type] = (s16)(inv->ammo[type] -
                (weapon_id == Q2_WID_CHAINGUN ? pellets : need));
    }

    res.fired = true;
    if (w->refire > 0)
        res.next_fire = now + w->refire;

    q2_weapon_muzzle_origin(tab->muzzle[weapon_id], eye, yaw, pitch, roll,
                            origin);

    /* View kick, and which of its fields this weapon stores at all. */
    res.kick_axes = w->kick_axes;
    if (weapon_id == Q2_WID_CHAINGUN) {
        /* 0x8004CB1C..0x8004CB58: two draws, both shifted by 11, into yaw and
         * roll. Pitch is not drawn and not written. */
        res.kick[1] = kick_draw(rng, 11);
        res.kick[2] = kick_draw(rng, 11);
    } else if (w->kick_random) {
        /* 0x8004C7C0..0x8004C804: three draws, shifts 11, 12, 12. */
        res.kick[0] = kick_draw(rng, 11);
        res.kick[1] = kick_draw(rng, 12);
        res.kick[2] = kick_draw(rng, 12);
    } else {
        res.kick[0] = w->kick_pitch;
    }

    /*
     * The pad's half of the kick. Every fire function asks once per call, at
     * the end; the chaingun only when its spin fired something (0x8004CCD0
     * `beq` on the bullet count skips the sound and the shock together).
     */
    res.shock = w->shock;
    if (weapon_id == Q2_WID_CHAINGUN && chaingun_bullets <= 0)
        res.shock = Q2_SHOCK_NONE;

    /* Damage. */
    switch (weapon_id) {
    case Q2_WID_ROCKET_LAUNCHER:
        damage = (s16)(Q2_ROCKET_DAMAGE_BASE +
                       (q2_rng_next(rng) % Q2_ROCKET_DAMAGE_SPREAD));
        if (quad)
            damage = (s16)(damage * Q2_QUAD_MULTIPLIER);
        break;
    case Q2_WID_RAILGUN:
        damage = deathmatch ? Q2_RAILGUN_DAMAGE_DM : Q2_RAILGUN_DAMAGE;
        if (quad)
            damage = (s16)(damage * Q2_QUAD_MULTIPLIER);
        break;
    default:
        damage = quad ? w->damage_quad : w->damage;
        break;
    }

    if (pellets > Q2_FIRE_MAX_PELLETS)
        pellets = Q2_FIRE_MAX_PELLETS;

    switch (w->kind) {
    case Q2_FK_BULLET:
        if (!pellets)
            break; /* a zero-rate spin has no bullet, bang or shock */
        for (i = 0; i < pellets; i++) {
            q2_shot *s = &res.shot[res.shot_count++];
            int k;

            memcpy(s->origin, origin, sizeof(s->origin));
            for (k = 0; k < 3; k++) {
                s32 d = aim ? (s32)aim[k] * Q2_AIM_SCALE_BULLET : 0;
                if (w->spread.shift[k])
                    d += spread_draw(rng, w->spread.shift[k]);
                s->dir[k] = d;
            }
            s->damage = damage;
        }
        res.sound = (weapon_id == Q2_WID_SHOTGUN)       ? Q2_WSND_SHOTGUN
                  : (weapon_id == Q2_WID_SUPER_SHOTGUN) ? Q2_WSND_SUPER_SHOTGUN
                                                        : Q2_WSND_MACHINEGUN;
        break;

    case Q2_FK_BOLT: {
        q2_shot *s = &res.shot[res.shot_count++];
        int shift = (weapon_id == Q2_WID_HYPERBLASTER) ? Q2_AIM_SHIFT_BOLT_HYPER
                                                       : Q2_AIM_SHIFT_BOLT_BLASTER;
        int k;
        memcpy(s->origin, origin, sizeof(s->origin));
        for (k = 0; k < 3; k++) {
            /* `bgez / addiu 63 / sra 6` — a divide that rounds toward zero. */
            s32 a = aim ? aim[k] : 0;
            s32 bias = (1 << shift) - 1;
            s->dir[k] = (a >= 0 ? a : a + bias) >> shift;
        }
        s->damage = damage;
        /* No speed: for a bolt the direction IS the velocity — 0x8004D78C
         * stores this vector straight into the entity's +0x52. */
        res.sound = (weapon_id == Q2_WID_HYPERBLASTER) ? Q2_WSND_HYPERBLASTER
                                                       : Q2_WSND_BLASTER;
        break;
    }

    case Q2_FK_RAIL: {
        q2_shot *s = &res.shot[res.shot_count++];
        memcpy(s->origin, origin, sizeof(s->origin));
        /* The railgun hands the aim vector over untouched (0x8004D600 copies
         * player+0x3C straight to the stack), and the rail shifts it: the
         * shot's `dir` carries the range here as a bullet's does. A bare aim
         * stopped every slug 4096 units out, a quarter of its reach. */
        s->dir[0] = (aim ? aim[0] : 0) * Q2_AIM_SCALE_RAIL;
        s->dir[1] = (aim ? aim[1] : 0) * Q2_AIM_SCALE_RAIL;
        s->dir[2] = (aim ? aim[2] : 0) * Q2_AIM_SCALE_RAIL;
        s->damage = damage;
        res.sound = Q2_WSND_RAILGUN;
        break;
    }

    case Q2_FK_ROCKET: {
        q2_shot *s = &res.shot[res.shot_count++];
        int k;
        memcpy(s->origin, origin, sizeof(s->origin));
        for (k = 0; k < 3; k++)
            s->dir[k] = (aim ? aim[k] : 0) * Q2_AIM_SCALE_ROCKET;
        s->damage = damage;
        res.projectile_timer = Q2_ROCKET_LIFETIME;
        res.sound = Q2_WSND_ROCKET;
        break;
    }

    case Q2_FK_GRENADE: {
        q2_shot *s = &res.shot[res.shot_count++];
        s16 local[3];
        s32 dir[3];

        memcpy(s->origin, origin, sizeof(s->origin));

        /* The launcher ignores the aim vector: it rotates a fixed local
         * direction, up 2048 and forward 6144, by the view (0x8004CF40). */
        local[0] = 0;
        /*
         * NEGATED at the call site, and the constant left positive so it keeps
         * matching the disc word at 0x800AE9F8. The launcher's local direction
         * is (0, 2048, 6144) with the Y meaning UP, and q2_weapon_muzzle_origin
         * now takes a (right, DOWN, forward) triple — the same convention the
         * muzzle table uses. The console gets there the other way round: it
         * rotates the constant unnegated and negates the rotated Y afterwards
         * (0x8004CF94), with no pre-negation, which is the same result.
         */
        local[1] = -Q2_GRENADE_LAUNCH_UP;
        local[2] = Q2_GRENADE_LAUNCH_FORWARD;
        q2_weapon_muzzle_origin(local, (const s32[3]){ 0, 0, 0 },
                                yaw, pitch, roll, dir);
        memcpy(s->dir, dir, sizeof(s->dir));
        s->damage = damage;

        res.projectile_timer = Q2_GRENADE_LAUNCH_FUSE;
        res.sound = Q2_WSND_GRENADE_LAUNCH;
        break;
    }

    case Q2_FK_HAND_GRENADE: {
        q2_shot *s = &res.shot[res.shot_count++];

        /* 0x8004AA6C creates Grenade3 hidden at the owner's eye. It has no
         * launch direction yet: the held think supplies the charged vector
         * only when the view-model crosses 411. A zero vector is therefore
         * meaningful for this kind rather than a failed projectile. */
        memcpy(s->origin, origin, sizeof(s->origin));
        s->damage = damage;
        res.projectile_timer = Q2_HAND_GRENADE_FUSE;
        /* Prime and throw belong to animation crossings 261 and 411. */
        res.sound = -1;
        break;
    }

    case Q2_FK_BFG: {
        q2_shot *s = &res.shot[res.shot_count++];
        int k;
        memcpy(s->origin, origin, sizeof(s->origin));
        for (k = 0; k < 3; k++)
            s->dir[k] = aim ? aim[k] : 0;
        s->damage = damage;
        res.projectile_timer = Q2_BFG_LIFETIME;
        res.sound = Q2_WSND_BFG;
        break;
    }

    case Q2_FK_NONE:
    default:
        break;
    }

    return res;
}
