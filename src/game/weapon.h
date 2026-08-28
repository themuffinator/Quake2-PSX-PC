/*
 * weapon.h — firing: what each of the eleven weapons actually does.
 *
 * ---------------------------------------------------------------------------
 * This replaces an inference with a reading
 * ---------------------------------------------------------------------------
 * An earlier pass could not find a damage table and said so: the per-weapon
 * numbers were taken from the PC lineage and labelled INFERRED. They are no
 * longer inferred, and the reason the table was never found is that there is
 * no table. Each weapon has its **own fire function**, and every number —
 * damage, pellet count, spread, view kick, refire — is an immediate operand
 * inside it.
 *
 * The functions are reached through a 12-entry pointer array at 0x8009D704
 * indexed by the live weapon id. Slot 0 is 0x8004EB08, which is literally
 * `jr ra; nop`: firing "no weapon" is a call that returns. That stub is the
 * cleanest proof of the 1-based indexing FORMATS.md §9.6 derives from the
 * auto-switch list, because it only makes sense as slot zero.
 *
 *     id  weapon             fire function
 *      1  Blaster            0x8004BFBC
 *      2  Shotgun            0x8004C1C0
 *      3  Super Shotgun      0x8004C488
 *      4  Machinegun         0x8004C744
 *      5  Chaingun           0x8004CA9C
 *      6  Hand Grenade       0x8004EBDC
 *      7  Grenade Launcher   0x8004CE18
 *      8  Rocket Launcher    0x8004D038
 *      9  Hyperblaster       0x8004D250
 *     10  Railgun            0x8004D498
 *     11  BFG                0x8004EB10
 *
 * Where a value is an id-for-id match with PC Quake II it is called out, not
 * because the PC value was assumed but because agreement on a number nothing in
 * the decode could have produced is evidence the decode is right. The railgun's
 * 100 / 150-in-deathmatch and the rocket's `100 + rand() % 20` are the two
 * strongest: both are compound and both land exactly.
 *
 * ---------------------------------------------------------------------------
 * Quad damage is a multiply at the fire site
 * ---------------------------------------------------------------------------
 * Every fire function starts by comparing the level clock against a per-player
 * expiry word and picking one of two immediates — 8 or 32, 6 or 24, 120 or 480.
 * It is always exactly four times. The rocket and railgun, whose damage is not
 * a constant, shift left by two instead. So quad is not a flag consulted by the
 * damage code; it is chosen where the shot is made, which is why it does not
 * multiply splash damage from a rocket someone else fired.
 *
 * ---------------------------------------------------------------------------
 * Spread is per world axis, not in a plane
 * ---------------------------------------------------------------------------
 * The scatter weapons add `rand() - 16384` scaled by a per-weapon shift to each
 * component of the aim vector, which the fire functions have already multiplied
 * by four. So the jitter is in world axes rather than in the shooter's right/up
 * plane the way the PC game does it. The magnitudes still line up: the shotgun
 * is +-1024 on all three against id's 500/500 out of 8192, and the super
 * shotgun is +-2048 on x and +-1024 on y and z against id's 1000/500. Two to
 * one, in both games.
 *
 * ---------------------------------------------------------------------------
 * View kick
 * ---------------------------------------------------------------------------
 * Written straight into the player's kick fields, and constant for most
 * weapons: -11 for the blaster, grenade launcher and rocket launcher, -22 for
 * both shotguns, -34 for the railgun, zero for the hyperblaster. The machinegun
 * and chaingun instead take three random draws, `((rand & 0x7FFF) - 16384) >>>
 * n` with an UNSIGNED shift on a value that is usually negative; truncated to
 * the s16 the field holds, that yields a small signed number, so the effect is
 * a jitter of about +-8 in pitch and +-4 in yaw and roll. The port reproduces
 * the truncation rather than the intent, because the truncation is what the
 * console did.
 *
 * ---------------------------------------------------------------------------
 * Refire
 * ---------------------------------------------------------------------------
 * Thirty ticks, written as `level_time + 30`, for every weapon that sets it —
 * which is all of them except the hyperblaster, whose rate comes from its view
 * model animation loop instead. So the console does not have per-weapon fire
 * rates in the PC sense; it has one gate plus animation length.
 */
#ifndef Q2PSX_WEAPON_H
#define Q2PSX_WEAPON_H

#include "inventory.h"
#include "q2psx.h"
#include "weapontables.h"

/* ------------------------------------------------------------------------- */
/* Ids                                                                        */
/*                                                                            */
/* The engine's live weapon id is 1..11 with 0 meaning "no weapon". The port's */
/* q2_weapon enum in inventory.h is 0-based, so the two differ by one and the  */
/* conversions below are the only place that should know it.                  */
/* ------------------------------------------------------------------------- */
#define Q2_WID_NONE              0
#define Q2_WID_BLASTER           1
#define Q2_WID_SHOTGUN           2
#define Q2_WID_SUPER_SHOTGUN     3
#define Q2_WID_MACHINEGUN        4
#define Q2_WID_CHAINGUN          5
#define Q2_WID_HAND_GRENADE      6
#define Q2_WID_GRENADE_LAUNCHER  7
#define Q2_WID_ROCKET_LAUNCHER   8
#define Q2_WID_HYPERBLASTER      9
#define Q2_WID_RAILGUN          10
#define Q2_WID_BFG              11
#define Q2_WID_COUNT            11

/* The phantom the original's cycle loop walks into. Its owned-bit is zero, so
 * the scan always rejects it — but the bound is 12, not 11, and a port that
 * tightens it changes which weapon a wrap-around lands on. */
#define Q2_WID_CYCLE_MAX        12

Q2PSX_INLINE int q2_wid_from_weapon(q2_weapon w) { return (int)w + 1; }
Q2PSX_INLINE q2_weapon q2_weapon_from_wid(int id) { return (q2_weapon)(id - 1); }

/* ------------------------------------------------------------------------- */
/* Per-weapon behaviour, transcribed from the fire functions                  */
/* ------------------------------------------------------------------------- */
typedef enum q2_fire_kind {
    Q2_FK_NONE = 0,     /* slot 0: 0x8004EB08, `jr ra`                       */
    Q2_FK_BULLET,       /* one or more hitscan traces, 0x8004874C            */
    Q2_FK_BOLT,         /* an energy bolt entity, 0x8004D70C                 */
    Q2_FK_RAIL,         /* an instant penetrating trace, 0x8004917C          */
    Q2_FK_GRENADE,      /* a bouncing, fused projectile, 0x8004A088          */
    Q2_FK_HAND_GRENADE, /* thrown, primed while held, 0x8004AA6C             */
    Q2_FK_ROCKET,       /* a flying projectile that explodes, 0x8004AF28     */
    Q2_FK_BFG           /* 0x8004BE04                                        */
} q2_fire_kind;

/*
 * How a weapon jitters its aim. The value is the arithmetic-shift-right applied
 * to `rand() - 16384`, per component, against an aim vector the fire function
 * has scaled by four. Zero means no scatter.
 */
typedef struct q2_spread {
    s16 shift[3];       /* 0 = no jitter on this axis                        */
} q2_spread;

typedef struct q2_weapon_behaviour {
    q2_fire_kind kind;

    s16 damage;         /* the plain immediate; 0 when it is computed        */
    s16 damage_quad;    /* the quad immediate; always 4x when both are fixed */

    s16 pellets;        /* traces per shot; 0 for the chaingun, which asks
                         * its spin state how many                           */
    q2_spread spread;

    s16 kick_pitch;     /* written to the player's kick field                */
    bool kick_random;   /* machinegun and chaingun: three random draws       */

    s16 refire;         /* ticks; 0 when the animation drives the rate       */
    s16 mod;            /* means of death the shot carries                   */

    u32 addr;           /* the fire function, so a reader can check a row    */
} q2_weapon_behaviour;

/* Indexed by the 1-based weapon id; slot 0 is the do-nothing stub. */
extern const q2_weapon_behaviour q2_weapon_behaviour_table[Q2_WT_SLOTS];

/*
 * How long an EMPTY weapon waits before it will click again.
 *
 * A PORT CONSTANT, not a read one, and there is no figure to read: on the
 * console the idle state calls the fire function once per fire pass and the
 * animation gates the rate, so a dry trigger has no deadline of its own to
 * carry. Here the sim fires from its own tick, so without a gate the no-ammo
 * branch re-runs — and re-plays its sound — every tick the trigger is held.
 *
 * 30 is the universal refire the level clock's 300-per-second makes a tenth of
 * a second, which is the shortest interval anything else in the fire path uses.
 */
#define Q2_WEAPON_DRY_REFIRE 30

/* Rocket damage is `100 + rand() % 20` (0x8004D0C4..0x8004D0F8) and railgun
 * damage is 150 in deathmatch and 100 otherwise (0x8004D5D8). Both are
 * computed rather than immediate, so they get named constants instead. */
#define Q2_ROCKET_DAMAGE_BASE   100
#define Q2_ROCKET_DAMAGE_SPREAD  20
#define Q2_RAILGUN_DAMAGE       100
#define Q2_RAILGUN_DAMAGE_DM    150

/* The quad multiplier, as it appears at every fire site. */
#define Q2_QUAD_MULTIPLIER 4

/* Grenade launcher: a fixed local launch velocity (0, up 2048, forward 6144)
 * rotated by the view, at 0x8004CF40 reading gp+1016. The ratio 2048:6144 is
 * exactly id's 200-up-out-of-600-forward. It is already in the shared entity
 * mover's pre-divide units; 0x800464D4 advances each component as vel*dt/320.
 *
 * 900 is NOT a speed. The caller passes it as argument 5 at 0x8004CF9C, the
 * spawner copies that argument to entity+0xF4 at 0x8004A2F8, and the think at
 * 0x80049FE8 subtracts dt from +0xF4 until it reaches zero. */
#define Q2_GRENADE_LAUNCH_UP      2048
#define Q2_GRENADE_LAUNCH_FORWARD 6144
#define Q2_GRENADE_LAUNCH_FUSE     900   /* 0x8004CF9C -> entity+0xF4       */
#define Q2_HAND_GRENADE_FUSE      1650   /* 0x8004EC3C -> entity+0xF4       */

/* Grenade3's private held/release figures, read from 0x8004A368.
 *
 * The spawner writes 4096 to entity+0x4C. While the owner's view-model
 * position is exactly 380, the held think adds 6 * dt to that field. On the
 * 261 and 411 crossings it plays the prime and throw samples respectively;
 * the latter rotates {0,2048,charge} and makes the entity visible.
 *
 * The release origin literal at 0x800AE980 is {80,-50,200}. The original
 * negates its middle component before RotMatrix and negates world Y after it;
 * in this port's (right, DOWN, forward) muzzle convention the final local
 * triple is the literal itself. */
#define Q2_HAND_GRENADE_CHARGE_START       4096
#define Q2_HAND_GRENADE_CHARGE_PER_DT         6
#define Q2_HAND_GRENADE_RELEASE_UP          2048
#define Q2_HAND_GRENADE_RELEASE_RIGHT         80
#define Q2_HAND_GRENADE_RELEASE_DOWN         -50
#define Q2_HAND_GRENADE_RELEASE_FORWARD      200

/* The rocket spawner stores 20000 at entity+0xF4 (0x8004B030). Its think at
 * 0x8004AD74 subtracts dt from that halfword and frees the entity at zero.
 * Direction is the player's 1.3.12 aim doubled by the fire function, already
 * expressed in the same pre-divide-by-320 units as every shared entity. */
#define Q2_ROCKET_LIFETIME       20000

/*
 * The BFG has its own mover. The spawner rotates the literal s16 vector
 * {0,0,768} at 0x800AE9B4, and 0x8004B90C..0x8004B9DC advances each component
 * as vel*dt/64. The 2400 stored at entity+0xF4 is independently counted down at
 * 0x8004B8C0, making an eight-second lifetime on the 300 Hz level clock.
 */
#define Q2_BFG_RAW_SPEED      768
#define Q2_BFG_VEL_DIV         64
#define Q2_BFG_LIFETIME      2400

/* The aim vector the fire functions read from the player at +0x3C..+0x40 is
 * scaled differently by each. These are the shifts, as read. */
#define Q2_AIM_SHIFT_BOLT_BLASTER   6    /* 0x8004C0D0: aim >> 6             */
#define Q2_AIM_SHIFT_BOLT_HYPER     7    /* 0x8004D39C: aim >> 7             */
#define Q2_AIM_SCALE_BULLET         4    /* 0x8004C330: aim << 2             */
#define Q2_AIM_SCALE_ROCKET         2    /* 0x8004D190: aim << 1             */

/* ------------------------------------------------------------------------- */
/* Weapon selection — 0x800506C4 and 0x80050758                               */
/* ------------------------------------------------------------------------- */

/*
 * Step to the next usable weapon. `dir` is +1 for the engine's "next" and -1
 * for "previous"; the original passes a non-zero flag for +1 and zero for -1.
 *
 * Returns the id to switch to, or 0 when nothing else is usable — which is
 * also what it returns when the scan comes back to the weapon already held,
 * exactly as the original's final comparison does.
 *
 * A weapon is usable when it is owned AND the player has at least one shot's
 * worth of its ammo. The scan wraps at 12 and at 1.
 */
int q2_weapon_cycle(const q2_inventory *inv, int current_id, int dir);

/*
 * The best weapon by the preference list at 0x8009DB7C — railgun, hyperblaster,
 * rocket launcher, chaingun, machinegun, super shotgun, shotgun, blaster.
 * Explosives are deliberately absent. Returns 0 when nothing is usable.
 */
int q2_weapon_autoselect(const q2_inventory *inv);

/*
 * Put back what a shot cost, for the one case where the shot does not happen
 * after `q2_weapon_fire` has already charged for it: the projectile pool being
 * full. Not a game mechanic — the original's entity pool can fail the same way
 * — but the alternative is charging the player for a rocket that never exists.
 * The caller restores the refire deadline itself, since it is the one holding
 * the value from before the shot.
 */
void q2_weapon_refund(q2_inventory *inv, int weapon_id);

/* Charge one shot's ammo now. Ordinary fire functions do this inside
 * q2_weapon_fire; Grenade3 is the exception, because 0x8004A7C0 decrements
 * grenades only when the held entity crosses the release position. */
bool q2_weapon_consume(q2_inventory *inv, int weapon_id);

/* True when the id is owned and has ammo for one shot. */
bool q2_weapon_usable(const q2_inventory *inv, int id);

/*
 * The MUZZLE FLASH light, for the two weapons that carry one.
 *
 * The machinegun's fire function (0x8004C744, weapon id 4) and the chaingun's
 * (0x8004CA9C, id 5) each append a dynamic light. Both read their colour from
 * 0x800AE9D4 -- `C8 64 64`, rgb(200, 100, 100) -- and both compute their radii
 * from a SINGLE BIOS rand() draw (0x8004C970 -> 0x80089E28):
 *
 *     8004C978  sll/addu/sll/addu/sll   ; r * 100
 *     8004C98C  sra  s0, s0, 15         ; >> 15
 *     8004C994  addiu s0, s0, 250       ; inner = ((r*100)>>15) + 250
 *
 *     8004C998  sll/addu/sll/addu/sll   ; r * 200
 *     8004C9AC  sra  v1, v1, 15         ; >> 15
 *     8004C9C0  addiu v1, v1, 700       ; outer = ((r*200)>>15) + 700
 *
 * One draw feeds both, so the flash's size varies shot to shot but its inner and
 * outer stay in proportion: 250..349 and 700..899. `q2_rng_next` is the BIOS
 * generator bit for bit (see weapon.c), so this reproduces the console's
 * sequence rather than approximating it.
 */
#define Q2_MUZZLE_LIGHT_R      200
#define Q2_MUZZLE_LIGHT_G      100
#define Q2_MUZZLE_LIGHT_B      100
#define Q2_MUZZLE_INNER_BASE   250
#define Q2_MUZZLE_INNER_SCALE  100
#define Q2_MUZZLE_OUTER_BASE   700
#define Q2_MUZZLE_OUTER_SCALE  200

/*
 * The CREATURE muzzle flash — import slot 0xA0, which only the Gunner's think 13
 * calls on the whole disc.
 *
 * Same colour as the player's, and the same shape of computation, but bigger and
 * drawn TWICE rather than once (0x800311B0 and 0x800311E0 are two separate
 * rand() calls):
 *
 *     inner = ((r1 * 200) >> 15) + 850     850..1049
 *     outer = ((r2 * 400) >> 15) + 1200   1200..1599
 *
 * The `bgez ...; addiu 32767` around each shift is the compiler's signed-divide
 * idiom, not evidence of a signed input; `rand()` returns 0..32767 so it never
 * takes that arm. An earlier note here read it as a sign and used it as a reason
 * not to wire this — wrongly.
 */
#define Q2_CRE_MUZZLE_INNER_BASE   850
#define Q2_CRE_MUZZLE_INNER_SCALE  200
#define Q2_CRE_MUZZLE_OUTER_BASE  1200
#define Q2_CRE_MUZZLE_OUTER_SCALE  400

/* The creature flash's radii, from its TWO draws, as 0x800311B8 computes them. */
void q2_creature_muzzle_light(s32 rand_a, s32 rand_b, s32 *inner, s32 *outer);

/* True for the two weapon ids whose fire function appends a light. */
bool q2_weapon_has_muzzle_light(u8 weapon_id);

/* The flash's radii from one rand draw, exactly as 0x8004C978 computes them. */
void q2_weapon_muzzle_light(s32 rand_0_32767, s32 *inner, s32 *outer);

/* ------------------------------------------------------------------------- */
/* Firing                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * Deterministic pseudo-random source.
 *
 * Spread, kick and rocket damage all draw from the engine's generator. The port
 * keeps its own so a replay or a test is reproducible; seed it per level to get
 * the original's variety without its unpredictability.
 */
typedef struct q2_rng { u32 state; } q2_rng;

void q2_rng_seed(q2_rng *r, u32 seed);
/* 0..32767, the range every fire function assumes when it subtracts 16384. */
s32  q2_rng_next(q2_rng *r);

/*
 * Where the shot leaves the player.
 *
 * `muzzle` is the weapon table's triple and its middle component is DOWN, not
 * up — see weapontables.h. `eye` is the player's eye. The offset is rotated by
 * the camera's own matrix, transposed, so the point lands in the frame the
 * picture is drawn in; the roll is included for the same reason.
 */
void q2_weapon_muzzle_origin(const s16 muzzle[3], const s32 eye[3],
                             s32 yaw, s32 pitch, s32 roll, s32 out[3]);

/*
 * One shot's worth of trace or projectile, produced by q2_weapon_fire.
 *
 * A pellet's direction is the aim vector already scaled and jittered, so its
 * length carries the weapon's range: the trace runs from `origin` to
 * `origin + dir` and stops at the first thing it meets. That is the original's
 * own convention (0x80048790 adds the direction to the origin to get the trace
 * end) and it is why nothing here normalises.
 */
#define Q2_FIRE_MAX_PELLETS 20

typedef struct q2_shot {
    s32 origin[3];
    s32 dir[3];
    s16 damage;
} q2_shot;

typedef struct q2_fire_result_v2 {
    bool fired;             /* false when out of ammo or still refiring      */
    bool dry;               /* out of ammo: the console plays wep_noammo     */

    q2_fire_kind kind;
    s16  mod;

    u32  shot_count;
    q2_shot shot[Q2_FIRE_MAX_PELLETS];

    s16  kick[3];           /* what to write into the player's view kick     */
    s32  next_fire;         /* level tick when firing is allowed again       */
    s16  sound;             /* q2_wt_sound, or -1                            */
    s32  projectile_timer;  /* spawner's fuse/lifetime, in level ticks       */
} q2_fire_result_v2;

/*
 * Fire the held weapon.
 *
 * `aim` is the player's aim vector in the engine's own units — the s16 triple
 * at player+0x3C, which every fire function scales for itself. `now` is the
 * level clock in ticks. `quad` and `deathmatch` select the alternative
 * immediates the fire functions carry.
 *
 * Consumes ammo and advances the refire gate as the original does: the rate
 * limit is checked before ammo, so a blocked shot costs nothing, and a shot
 * with no ammo costs no time.
 */
q2_fire_result_v2 q2_weapon_fire(q2_inventory *inv, q2_rng *rng,
                                 const q2_weapon_tables *tab,
                                 int weapon_id,
                                 const s32 eye[3], s32 yaw, s32 pitch, s32 roll,
                                 const s16 aim[3],
                                 s32 now, s32 next_fire,
                                 bool quad, bool deathmatch,
                                 int chaingun_bullets);

#endif /* Q2PSX_WEAPON_H */
