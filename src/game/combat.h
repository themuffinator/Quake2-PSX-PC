/*
 * combat.h — delivering damage: armour, knockback, splash, hitscan.
 *
 * ---------------------------------------------------------------------------
 * The damage function
 * ---------------------------------------------------------------------------
 * Everything that hurts anything goes through 0x80057D54, which twenty-one
 * call sites reach. Its signature is
 *
 *     damage(attacker, target, amount, mod, point)
 *
 * and it does, in this order: record who hit what; apply knockback for the four
 * means-of-death that carry it; halve the amount if a monster hit a player at
 * the lowest skill; refuse everything while the target is invulnerable; take
 * what power armour absorbs; take what armour absorbs; subtract the rest from
 * health; set the damage-effect timer the mod implies.
 *
 * Two things it does NOT do, and both matter for a port:
 *
 *   - **It does not kill anything.** There is no die callback and no pain
 *     callback. Health goes negative and the entity's own think notices later.
 *   - **For a creature with an AI brain it does not even subtract health.** It
 *     posts the damage to the module through 0x800627F8 and returns; the
 *     module owns the creature's health. Only brainless entities and players
 *     have their health decremented here.
 *
 * ---------------------------------------------------------------------------
 * Armour
 * ---------------------------------------------------------------------------
 * Two stages, in this order, and the second knows whether the first fired.
 *
 * **Power armour** (0x80057A9C) applies when the player holds one of the two
 * power items (bits 0x18000 of the powerup word) and has cells. It absorbs two
 * thirds of the incoming damage, capped at twice the cells held, and spends one
 * cell for every two points absorbed.
 *
 * **Armour** (0x80057BE4) then takes `(bias + protection * damage) >> 12`,
 * capped at what is left, where `protection` comes from the three-record table
 * described in weapontables.h and the bias is 4095 outside deathmatch and 2048
 * inside it. That bias is a rounding rule, not a fudge: 4095 rounds every
 * non-zero fraction UP, so single-point hits are still absorbed, while 2048
 * rounds to nearest. Deathmatch armour is therefore very slightly weaker
 * against small hits, which is exactly the sort of thing that would never be
 * guessed and has to be read.
 *
 * Mod 8 skips both stages entirely — the only damage class in the game that
 * ignores armour outright.
 *
 * ---------------------------------------------------------------------------
 * Knockback
 * ---------------------------------------------------------------------------
 * Only four means of death push: rail, grenade, rocket and bullet. The impulse
 * is `unit(target - point) * scale * damage / 2400 >> 4`, where the scale is
 * about 1.95 per unit of a global at 0x800B3358 plus 64 — except when a player
 * hits themselves, where it is about 6.25 instead. That asymmetry is the rocket
 * jump, and it is three times as strong as being hit by someone else's rocket.
 *
 * The impulse accumulates into the target rather than being applied: the engine
 * adds it to a triple at entity+0x2F8 and raises bit 0x4000, and only a living
 * target gets it — a corpse takes the impulse as an absolute value instead of
 * an addition, which is how bodies do not inherit a dead entity's momentum.
 *
 * ---------------------------------------------------------------------------
 * Radius damage
 * ---------------------------------------------------------------------------
 * 0x80050810 sweeps a box of the blast radius, rejects anything whose centre is
 * further than `radius + its own radius`, and applies `damage - dist*170/4096`.
 * The falloff coefficient is a length constant and was retuned: PC Quake II
 * loses half a point per unit, which at this world scale would be 0.05 per unit
 * against the console's 0.0415.
 *
 * ---------------------------------------------------------------------------
 * Hitscan
 * ---------------------------------------------------------------------------
 * 0x8004874C is the one bullet path: trace from the muzzle to `origin + dir`
 * — the direction vector carries the range, nothing is normalised — draw the
 * tracer, damage a breakable surface if the world was hit, then re-trace
 * against entities and damage the first one found with mod 18.
 *
 * The rail (0x8004917C) is the same shape with mod 3, and it keeps going: it
 * re-traces from each impact so one shot can pass through several targets.
 */
#ifndef Q2PSX_COMBAT_H
#define Q2PSX_COMBAT_H

#include "inventory.h"
#include "monster.h"
#include "q2psx.h"
#include "weapontables.h"

/* ------------------------------------------------------------------------- */
/* Means of death                                                             */
/*                                                                            */
/* Values 1..21, read from the `a3` immediate at each of the twenty-one call   */
/* sites of 0x80057D54. Six of them are named from what their call site does;  */
/* the rest keep their number, because naming a mod we have not identified     */
/* would be an invention dressed as a reading.                                 */
/* ------------------------------------------------------------------------- */
enum {
    Q2_MOD_NONE         =  0,
    Q2_MOD_ENERGY_BOLT  =  1,   /* 0x80049E34 blaster bolt, 0x8004BC48 BFG   */
    Q2_MOD_2            =  2,   /* raises the +0x2F0 effect timer to 15      */
    Q2_MOD_RAIL         =  3,   /* 0x80049330                                */
    Q2_MOD_4            =  4,   /* raises the +0x2F2 effect timer to 30      */
    Q2_MOD_5            =  5,   /* raises the +0x2F4 effect timer to 5       */
    Q2_MOD_6            =  6,
    Q2_MOD_MELEE        =  7,   /* 0x800612F0, a creature's contact hit      */
    Q2_MOD_NO_ARMOUR    =  8,   /* 0x8003D380 — the only class armour skips  */
    Q2_MOD_ACID         =  9,   /* 0x8002E4B0; throttled to once per 400     */
    Q2_MOD_LAVA         = 10,   /* 0x8002E524; throttled to once per 100     */
    Q2_MOD_LASER        = 11,   /* 0x8002E284, 0x80049B38                    */
    Q2_MOD_EXPLOSION    = 12,   /* 0x80048F50                                */
    Q2_MOD_GRENADE      = 13,   /* 0x80049FBC, 0x8004A904                    */
    Q2_MOD_14           = 14,
    Q2_MOD_ROCKET       = 15,   /* 0x8004AE14                                */
    Q2_MOD_16           = 16,
    Q2_MOD_17           = 17,
    Q2_MOD_BULLET       = 18,   /* 0x80048974 — every hitscan weapon         */
    Q2_MOD_19           = 19,   /* 0x80039DAC                                */
    Q2_MOD_CRUSH        = 20,   /* 0x80051E74 — a mover closing on something */
    Q2_MOD_21           = 21,
    Q2_MOD_COUNT        = 22
};

/* True when armour uses its ENERGY column rather than its normal one. The set
 * is the jump table at 0x800ACE1C, sixteen entries indexed by mod-1; mods above
 * 16 fall past it and are treated as ordinary damage. */
bool q2_mod_is_energy(s16 mod);

/* True when the mod imparts knockback. Exactly {3, 13, 15, 18} — the branch
 * chain at 0x80057ED0..0x80057EE8. */
bool q2_mod_knocks_back(s16 mod);

/*
 * The green light an armed energy-bolt effect casts.
 *
 * `0x80058638` is the handler that runs right after `q2_mod_effect_timer`'s
 * range, and it gates on one byte:
 *
 *     80058650  lbu   v0, 753(s1)     ; entity+0x2F1 == effect[1]
 *     80058658  beq   v0, zero, ...   ; nothing armed -> nothing to do
 *     80058660  sltiu v0, v0, 3       ; below 3 takes a DIFFERENT branch
 *     80058674  addiu a2, s0, -5460   ; 0x800AEAAC -- the colour
 *
 * `Q2_MOD_ENERGY_BOLT` is the mod that arms slot 1, and it arms it with exactly
 * 3 — so this light is what an energy-bolt hit looks like on the tick it lands,
 * and the `< 3` arm is the tail of the same effect. Its preset:
 *
 *     0x800AEAAC   00 FF 00      rgb(0, 255, 0)
 *     0x800AEAB0   20 03 14 05   u16 800, 1300   inner, outer
 *
 * Close to the BFG blast's green but not equal to it (1000 / 1400), so the two
 * are separate effects.
 */
#define Q2_ENERGY_LIGHT_R      0
#define Q2_ENERGY_LIGHT_G    255
#define Q2_ENERGY_LIGHT_B      0
#define Q2_ENERGY_LIGHT_INNER  800
#define Q2_ENERGY_LIGHT_OUTER 1300

/* The damage-effect timer a mod arms, or 0. Returns the slot in `slot` and the
 * value as the result (0x800585A4..0x80058604). */
s16 q2_mod_effect_timer(s16 mod, int *slot);


/* ------------------------------------------------------------------------- */
/* Rules that live in globals rather than in the damage function              */
/* ------------------------------------------------------------------------- */
typedef struct q2_combat_rules {
    bool deathmatch;      /* 0x800AEBCC                                      */
    s16  skill;           /* 0x800B334A; 0 halves what monsters do to you    */
    s16  knockback_mass;  /* 0x800B3358; the impulse scale's input           */
    s32  level_time;      /* 0x800AEBAC, in ticks                            */
} q2_combat_rules;

void q2_combat_rules_default(q2_combat_rules *r);

/* The impulse divisor and the final shift, both immediates. */
#define Q2_KNOCKBACK_DIVISOR 2400
#define Q2_KNOCKBACK_SHIFT      4

/* Splash falloff: `damage - (dist * 170) >> 12` (0x800509E0..0x800509F4). */
#define Q2_SPLASH_FALLOFF_NUM  170
#define Q2_SPLASH_FALLOFF_SHIFT 12

/* Power armour: two thirds absorbed, one cell per two points (0x80057AE0). */
#define Q2_POWER_ARMOUR_NUM   2
#define Q2_POWER_ARMOUR_DEN   3
#define Q2_POWER_ARMOUR_CELLS 2
/* Bits of the powerup word that enable it (0x80057AC4). */
#define Q2_POWERUP_POWER_ARMOUR 0x00018000u

/* Armour's rounding bias, 0x80057C1C / 0x80057C20. */
#define Q2_ARMOUR_BIAS_SP 4095
#define Q2_ARMOUR_BIAS_DM 2048

/*
 * Environmental throttles, 0x80058268 and 0x800582AC. The level clock runs at
 * 300 ticks per second — established independently by the mover scripting,
 * where one authoring unit of time is 300 (userfuncs.h) — so acid hurts about
 * three times a second at most and lava three times as often.
 *
 * That rate is also what makes the universal 30-tick refire gate legible: a
 * tenth of a second, which is a floor rather than a per-weapon fire rate.
 */
#define Q2_TICKS_PER_SECOND    300
#define Q2_ENV_THROTTLE_ACID   400
#define Q2_ENV_THROTTLE_LAVA   100

/* 0x800629B4: how far below zero a corpse's health is allowed to run. */
#define Q2_HEALTH_FLOOR (-9999)

/* ------------------------------------------------------------------------- */
/* An actor: anything that can be hurt                                        */
/*                                                                            */
/* The engine's target is an entity with an optional client block. Rather than */
/* model both, this carries the union of the fields the damage function        */
/* touches, and the sync helpers below move them in and out of q2_monster and  */
/* q2_inventory so nothing else has to change.                                */
/* ------------------------------------------------------------------------- */
typedef struct q2_actor {
    s32  origin[3];
    /*
     * The volume clipped by 0x800544EC. The narrow phase is a vertical
     * cylinder, not the sphere the first reconstruction used: entity+0x94 is
     * its horizontal radius and the entity's vertical bounds form a separate
     * slab. Keeping the local bounds also carries the corpse resize across
     * q2_actor_from_monster instead of silently restoring a standing body.
     */
    s32  radius;            /* entity+0x94, horizontal X/Z radius            */
    s16  height;            /* entity+0x96, Y slab height                    */
    s16  mins[3];           /* retained source hull for actor projection     */
    s16  maxs[3];           /* its Y span seeds height above                 */

    s16  health;
    s16  gib_health;        /* below this the body is destroyed              */

    /* Client-only. `has_client` is the engine's `entity+0x0C != NULL`, and it
     * is what decides whether armour exists at all. */
    bool has_client;
    s16  armour;
    u8   armour_class;      /* index into the three-record table             */
    s16  cells;             /* power armour spends these                     */
    u32  powerups;
    s32  invuln_until;      /* client+0xB0                                   */
    s32  protect_until;     /* client+0xB4                                   */
    s32  env_next;          /* client+0x94, the throttle for mods 9 and 10   */

    /*
     * What T_Damage (0x800627F8) reads to decide the surprise bonus: a monster
     * that has not acquired an enemy yet takes DOUBLE from a shot fired by
     * something with a client block, while it is still alive.
     *
     * This replaces an `ai_owned` flag that claimed a module-driven creature
     * has its damage POSTED to the AI rather than subtracted. It does not: the
     * subtraction is at 0x80062958, into (entity+0x24)+0x108, and the call at
     * 0x800584B4 that the claim rested on passes a DIFFERENT entity — the one
     * at entity+0x2EC — while the caller has already stored the target's own
     * health at 0x800583F8. The flag was never set to true, so nothing changed
     * when it went; what changed is that the surprise bonus now exists.
     */
    bool is_monster;        /* svflags & SVF_MONSTER, entity+0x40 bit 2      */
    bool has_enemy;         /* entity+0xBC != NULL                           */

    /*
     * entity+0x1C bits 30..31, and it is what decides whether a thing can be
     * hurt AT ALL — the fifth instruction of T_Damage is
     * `lw v1, 28(targ)` / `and v1, 0xC0000000` / `beq v1, zero, epilogue`
     * (0x80062838..0x80062848), before health, knockback, pain or die.
     *
     * A creature is DAMAGE_AIM from `monster_start` (0x80061A94) and every
     * module's own `die` downgrades it to DAMAGE_YES on the normal death arm,
     * which is precisely what keeps a body shootable so it can still be blown
     * apart. This side had no field for it and filtered on health instead,
     * which is why nothing on this disc could ever be gibbed.
     *
     * A PRECISE NEGATIVE on the two values: the whole executable holds exactly
     * two readers of these bits — 0x80062844 in T_Damage and 0x80049CD4 in the
     * BFG beam sweep — and both only test nonzero. There is no `srl 30` and no
     * sign test anywhere. So on THIS disc DAMAGE_YES and DAMAGE_AIM are
     * behaviourally identical; id's "auto targeting recognises this" has no
     * reader here, and the distinction is carried because the modules write it,
     * not because anything acts on it.
     */
    u8   takedamage;

    /* entity+0x20. Only the two bits the damage function tests are modelled. */
    bool no_knockback;      /* FL_NO_KNOCKBACK, 0x800: knockback forced to 0 */
    bool godmode;           /* FL_GODMODE, 0x10: damage forced to 0          */

    /*
     * Which player this actor IS, or -1 for anything that is not one.
     *
     * The engine carries the same thing as a signed byte at entity+222 — the
     * killer's id, which `q2_mp_attribute_kill` already takes and which nothing
     * had ever been able to supply, because an actor could not say who it
     * belonged to. Without it a deathmatch kill has a victim and no killer.
     */
    s8   owner;

    /* Written by the damage function. */
    s8   last_attacker;     /* entity+222: who last hurt this, -1 for world  */
    s32  knockback[3];      /* entity+0x2F8..0x2FC                           */
    bool knocked;           /* entity+0x10C bit 0x4000                       */
    s16  last_mod;          /* entity+0xDF                                   */
    u8   effect[6];         /* entity+0x2F0..0x2F4                           */
} q2_actor;

/* True while an actor's energy-bolt effect is at full strength (effect[1] >= 3),
 * which is the arm 0x80058660 takes to the lit path. */
bool q2_actor_energy_lit(const q2_actor *a);

/* Where a shot stopped considering a target — see the note in combat.c. */
typedef struct q2_combat_scan_stats {
    u32 tested;
    u32 skipped;        /* NULL, or the shooter itself                       */
    u32 dead;
    u32 behind;         /* the target is behind the muzzle                   */
    u32 beyond_world;   /* the world stopped the ray first                   */
    u32 off_axis;       /* outside the horizontal cylinder or vertical slab  */
    u32 hit;
} q2_combat_scan_stats;

/*
 * Per shooter, because one shared counter cannot say whose shot it was.
 *
 * Slot 0..3 is a player; slot 4 is everything else. `q2_combat_scan_who` is set
 * by whoever is about to fire and left alone otherwise. Without this dimension
 * a capture reports 2126 scans and no way to tell that every one of them
 * belonged to the player who was not under test — which is exactly what
 * happened, three code changes in a row, with byte-identical totals each time.
 */
#define Q2_COMBAT_SCAN_SLOTS 5
#define Q2_COMBAT_SCAN_OTHER 4

extern q2_combat_scan_stats q2_combat_scan_by[Q2_COMBAT_SCAN_SLOTS];
extern int                  q2_combat_scan_who;

/* The sum over every slot, for callers that only want a total. */
extern q2_combat_scan_stats q2_combat_scan;

void q2_actor_init(q2_actor *a);

/* Move state between the port's existing structures and an actor. */
void q2_actor_from_monster(q2_actor *a, const q2_monster *m);
void q2_actor_to_monster(const q2_actor *a, q2_monster *m);
void q2_actor_from_player(q2_actor *a, const q2_inventory *inv,
                          const s32 pos[3]);
void q2_actor_to_player(const q2_actor *a, q2_inventory *inv);

/* ------------------------------------------------------------------------- */
/* What a hit did                                                             */
/* ------------------------------------------------------------------------- */
typedef struct q2_damage_result {
    s16  taken;             /* what reached health                            */
    s16  absorbed_armour;
    s16  absorbed_power;
    bool blocked;           /* invulnerable, or throttled                     */
    bool killed;            /* health crossed zero on this hit                */
    bool gibbed;            /* and went below gib_health                      */
    bool surprised;         /* the ×2 bonus applied — 0x800628F8              */
} q2_damage_result;

/*
 * The damage function, 0x80057D54.
 *
 * `attacker` may be NULL (world damage). `point` may be NULL, in which case no
 * knockback is computed — which is what the engine does when its fifth argument
 * is zero (0x80057EC8).
 */
q2_damage_result q2_combat_damage(q2_actor *attacker, q2_actor *target,
                                  s16 damage, s16 mod, const s32 point[3],
                                  const q2_combat_rules *rules);

/* The two absorption stages, exposed because each is separately checkable. */
s16 q2_combat_power_armour_absorb(q2_actor *a, s16 damage);
s16 q2_combat_armour_absorb(q2_actor *a, s16 damage, bool energy,
                            bool power_armour_fired,
                            const q2_combat_rules *rules);

/*
 * Radius damage, 0x80050810.
 *
 * Applies to every actor in `targets` except `ignore`. Returns how many were
 * hurt. `attacker` is credited with the damage.
 */
u32 q2_combat_radius_damage(q2_actor *attacker, q2_actor *ignore,
                            const s32 point[3], s16 damage, s16 radius,
                            s16 mod, q2_actor **targets, u32 count,
                            const q2_combat_rules *rules);

/* The falloff on its own: what `damage` becomes at `dist` world units. */
s16 q2_combat_splash_at(s16 damage, s32 dist);

/* ------------------------------------------------------------------------- */
/* Creature attacks                                                           */
/*                                                                            */
/* Creatures do not have their own damage path: they reach the same function   */
/* everything else does. What is theirs is WHICH mod and which projectile.     */
/*                                                                            */
/*   - a contact hit is mod 7, applied at 0x800612F0 with the creature's own   */
/*     origin as the damage point, so it knocks nothing back (mod 7 is not in  */
/*     the knockback set)                                                      */
/*   - a thrown grenade is the SAME spawner the grenade launcher uses          */
/*     (0x8004A088 from 0x80061728); its wrapper uses 600 in the ballistic      */
/*     solve and also passes 600 as the spawner's +0xF4 timer                  */
/*   - a rocket is 0x8004AF28 from 0x80062164, with the aim scaled by 3/2      */
/*   - a BFG blast is 0x8004BE04 from a two-line wrapper at 0x800621BC         */
/*                                                                            */
/* The per-creature DAMAGE is not here, and deliberately so: it lives in each  */
/* creature's relocated module, which is open work (openquestions #6). A       */
/* caller supplies it, so binding a module later needs no change here.         */
/* ------------------------------------------------------------------------- */

/* A creature's contact hit. Nothing more than the damage call with mod 7 and
 * the attacker's own position as the point, which is what 0x800612F0 does. */
q2_damage_result q2_combat_melee(q2_actor *attacker, q2_actor *target,
                                 s16 damage, const q2_combat_rules *rules);

/* The creature wrapper's grenade argument. It participates in the ballistic
 * solve and is then reused as the fuse passed at 0x80061724. */
#define Q2_CREATURE_GRENADE_ARG 600

/* ------------------------------------------------------------------------- */
/* Tracing                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Distance from a point to a ray, squared, with the along-ray distance. The
 * primitive the entity trace rests on, kept because it is worth testing on its
 * own. `dir` is NOT assumed to be unit length — the fire functions hand over a
 * vector whose length is the range — so `along` comes back in units of |dir|
 * scaled by 4096.
 */
s64 q2_combat_ray_dist_sq(const s32 origin[3], const s32 dir[3],
                          const s32 point[3], s64 *out_along);

/*
 * Fallback horizontal radius for callers which project an actor without a
 * usable entity radius. Normal actors carry their own radius and Y slab, just
 * as 0x800544EC reads entity+0x94/+0x96; this value is not added to that hull.
 * There is no moving-radius argument at either the hitscan call 0x8004891C or
 * the entity/projectile mover call 0x80046A98: both pass start, end and the
 * entity to ignore, and the sweep reads only each candidate's entity+0x94.
 */
#define Q2_HITSCAN_RADIUS 286

/*
 * The nearest actor whose vertical cylinder a segment enters, or -1. Shared
 * by the bullet path and the projectile mover, which need the same question
 * answered. `fallback_radius` is only used for an actor with no radius.
 */
s32 q2_combat_nearest_on_segment(const s32 origin[3], const s32 dir[3],
                                 s32 fallback_radius, q2_actor **targets,
                                 u32 count);

/*
 * One hitscan trace, 0x8004874C.
 *
 * The trace runs from `origin` to `origin + dir` and stops at the first actor
 * cylinder it crosses. `world_fraction` is 4096 when nothing solid is in the
 * way, or the 1.0.12 fraction at which the world stops it — pass the result of
 * the caller's own world trace, so this module does not need to know about
 * collision hulls.
 *
 * Returns the index of the actor hit, or -1.
 */
s32 q2_combat_fire_bullet(q2_actor *attacker, const s32 origin[3],
                          const s32 dir[3], s16 damage, s32 world_fraction,
                          s32 fallback_radius, q2_actor **targets, u32 count,
                          const q2_combat_rules *rules,
                          q2_damage_result *out);

/*
 * The rail, 0x8004917C. Same trace, mod 3, and it does not stop at the first
 * target: every actor along the beam takes the full damage. Returns how many
 * were hit.
 */
u32 q2_combat_fire_rail(q2_actor *attacker, const s32 origin[3],
                        const s32 dir[3], s16 damage, s32 world_fraction,
                        s32 fallback_radius, q2_actor **targets, u32 count,
                        const q2_combat_rules *rules);

#endif /* Q2PSX_COMBAT_H */
