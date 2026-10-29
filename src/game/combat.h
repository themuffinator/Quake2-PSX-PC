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
 * means-of-death that carry it; run the ACID and LAVA arms; halve the amount if
 * a monster hit a player at the lowest skill; refuse everything while the
 * target is invulnerable; take what power armour absorbs; take what armour
 * absorbs; subtract the rest from health; play the mod's hit sound if the
 * target survived; set the damage-effect timer the mod implies.
 *
 * THE ENVIRONMENT SUIT IS NOT PART OF THAT REFUSAL. The damage function reads
 * client+0xB4 at exactly one instruction — 0x80058230, inside the mod-9 arm —
 * and the general test at 0x80058304 reads only client+0xB0. (The word is read
 * elsewhere in the image: the status bar's powerup walk draws its countdown
 * from it at 0x80035CB0. That reader refuses nothing.) The suit stops acid and
 * nothing else, not even lava: 0x8005828C's arm is the throttle alone and
 * reads neither timer. Both throttles compare STRICTLY and UNSIGNED
 * (`sltu env_next, level_time`), so the tick on which the deadline is reached
 * is still refused; the protection tests are `sltu` as well.
 *
 * WHO MEETS WHAT. 0x800582C8 `beq s0, zero, 0x8005842C` splits the function in
 * two. A target WITH a client block stays in 0x80057D54 for the skill halving,
 * invulnerability, armour, the ONE SHOT KILL rewrite (0x80058394) and the
 * health store. A target WITHOUT one leaves for T_Damage at 0x800584B4, whose
 * first real test is the `takedamage` gate at 0x80062838 — so that bit gates
 * creatures and never gates a player.
 *
 * The client arm leaves death handling to the player's think. The creature
 * arm calls 0x800627F8 (T_Damage), which subtracts health and dispatches pain,
 * death and enemy reactions. The port drains those pending reactions into the
 * bound creature callbacks after updating its actor state.
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
 * described in weapontables.h and the bias is chosen by the SKILL halfword:
 *
 *     80057C0C  lui   v0, 0x800B
 *     80057C10  lh    v0, 0x800B334A       ; skill, NOT 0x800AEBCC
 *     80057C18  bne   v0, zero, 0x80057C24
 *     80057C1C  addiu a0, zero, 2048       ; delay slot: skill != 0
 *     80057C20  addiu a0, zero, 4095       ; skill == 0
 *
 * That bias is a rounding rule, not a fudge: 4095 rounds every non-zero
 * fraction UP, so single-point hits are still absorbed, while 2048 rounds to
 * nearest. So armour on EASY is very slightly stronger against small hits, and
 * Medium and Hard share the weaker bias. This used to be written here and in
 * the code as a deathmatch split, which reads the wrong global — 0x800AEBCC is
 * the deathmatch word and the same function reads it at 0x80057DA8 and
 * 0x800580E0, nowhere near the bias.
 *
 * Mod 8 skips both stages entirely — the only damage class in the game that
 * ignores armour outright.
 *
 * Both helpers accept a full signed word and reject ZERO, not negative damage.
 * A negative saved amount restores inventory. The power cap is the signed
 * halfword of twice the cells; ordinary armour sign-extends its shifted result
 * before the cap. Keep the full remaining word between the two stages.
 *
 * ---------------------------------------------------------------------------
 * Knockback
 * ---------------------------------------------------------------------------
 * Only four means of death push: rail, grenade, rocket and bullet. The block
 * is 0x80057EC0..0x80058204 and it reads NO flags word: its only two guards are
 * 0x80057EC0 `beq s4, zero` (the original damage is zero) and 0x80057EC8
 * `beq s6, zero` (no point was supplied), then the four mod tests. The impulse
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
 * Three things happen between that falloff and the damage call, and none of
 * them is distance:
 *
 *   - 0x80050A04 `bne a2, s4` / 0x80050A0C `sra s0, s0, 1` — the blast's OWN
 *     owner takes half. The subtraction at 0x80050A08 is in the delay slot and
 *     therefore unconditional; the shift is not, and both precede the
 *     `blez s0` reject at 0x80050A10.
 *   - 0x80050A24 `jal 0x80044C44` — the SWEPT MOVE through the hull
 *     (collision.h's q2_coll_move), from the blast (a1) to the candidate's
 *     origin (a2 = candidate+0x54) through the collision context at
 *     0x800C8E90, with the caller's cell as the starting hint (a3; entity+0xA0,
 *     `lh a1, 160(ent)` at the grenade and rocket sites). It is a trace, not a
 *     separate zone or node query.
 *   - 0x80050A3C `jal 0x80053974` — the clip of the same segment against the
 *     48-slot entity-box table at 0x800CAE10; 0x80050A44 `beq v0, zero`
 *     skips the candidate when a box stops it.
 *
 * A zero from either gate skips the candidate outright, so retail's blast does
 * not go through a wall. `q2_combat_radius_damage` is a pure function with no
 * world, so the pair is injected as one callback: see
 * `q2_combat_radius_damage_traced`.
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

#include "gamevars.h"      /* Q2_CHEAT_*: 0x800B29EC reaches the damage path */
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
    /* The two sites whose `a3` is the immediate 1: 0x80049E34, the BFG's beam
     * pass, and 0x8004BC48, the ball's contact hit. NOT the blaster bolt —
     * 0x80047F08 takes its mod from `lh a3, -38(s3)`, a field, so which mod a
     * bolt carries is the spawner's choice and not this constant. */
    Q2_MOD_ENERGY_BOLT  =  1,   /* 0x80049E34 BFG beam, 0x8004BC48 BFG contact */
    Q2_MOD_2            =  2,   /* raises the +0x2F0 effect timer to 15      */
    Q2_MOD_RAIL         =  3,   /* 0x80049330                                */
    Q2_MOD_4            =  4,   /* raises the +0x2F2 effect timer to 30      */
    /*
     * The two ENERGY BOLT classes, and they are not mod 1.
     *
     * 0x8004D70C's sixth argument is stored at record+0x32 (0x8004D7B0)
     * and handed to the damage function as a3 on every hit (0x80047EF8).
     * The hyperblaster passes 5 (0x8004D3F0) and the blaster 6
     * (0x8004C124); a monster's blaster passes the same pair on the
     * `andi v0, 0x40` split at 0x800620A8 (0x800620C8 and 0x800620DC).
     * Mod 1 is the BFG's, and giving a bolt mod 1 armed the wrong
     * victim-side effect timer.
     */
    Q2_MOD_BOLT_HYPER   =  5,   /* raises the +0x2F4 effect timer to 5       */
    Q2_MOD_BOLT         =  6,
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

/*
 * Native entity allocation seeds current/target ambient to 0x40
 * (0x8006C1D8..0x8006C200); gameplay player spawn replaces both with 0x20
 * (0x8003B444..0x8003B470). Creature spawn retains the allocator's value.
 * 0x8005B88C supplies seven steps to their shared fade. Items and the menu's
 * player showcase have separate initialization paths, not these actor targets.
 */
#define Q2_ACTOR_AMBIENT_DEFAULT 0x40
#define Q2_PLAYER_AMBIENT_DEFAULT 0x20
#define Q2_ACTOR_AMBIENT_STEPS      7

/* The damage-effect timer a mod arms, or 0. Returns the slot in `slot` and the
 * value as the result (0x800585A4..0x80058604). */
s16 q2_mod_effect_timer(s16 mod, int *slot);

/*
 * The RUMBLE a hit on a player raises through 0x80040800, or 0 for none. The
 * name says "sound" and is kept because the fields that carry it are; what
 * the call does is shake the victim's pad.
 *
 * 0x80058504 `sltiu v0, v1, 21` indexes the twenty-one-word jump table at
 * 0x800ACE5C by mod-1; the four arms it lands on are 0x8005852C (id 13,
 * strength 4096), 0x80058538 (id 16, 2048), 0x80058544 (id 15, 4096) and
 * 0x80058550 (none). `volume` may be NULL.
 *
 * 0x80040800 IS pad rumble — SetShockData, shock.h — and this note used to
 * say the opposite. The "four voice slots at entity+732" are four PATTERN
 * slots at GameWindows[player]+732, and 13, 15 and 16 are ShockData_Explosion,
 * _Impact_Distinct and _Impact_Mergeable. The damage call passes a3 = 0, so
 * unlike the train's it is NON-POSITIONAL — no attenuation and no distance
 * reject.
 */
u8 q2_mod_hit_sound(s16 mod, s16 *volume);


/* ------------------------------------------------------------------------- */
/* Rules that live in globals rather than in the damage function              */
/* ------------------------------------------------------------------------- */
struct q2_actor;
typedef void (*q2_combat_environment_fn)(void *user, const struct q2_actor *target, s16 mod);

typedef struct q2_combat_rules {
    bool deathmatch;      /* 0x800AEBCC                                      */
    s16  skill;           /* 0x800B334A; 0 halves what monsters do to you,
                           * and picks armour's rounding bias (0x80057C10)   */
    s16  knockback_mass;  /* 0x800B3358; the impulse scale's input           */
    s32  level_time;      /* 0x800AEBAC, in ticks                            */
    /*
     * The GAME VARIABLES cheat halfword at 0x800B29EC, built once by
     * 0x8001C698 and read straight out of the global by the damage function at
     * 0x80058398. See common/gamevars.h for the four bits. Only
     * Q2_CHEAT_ONE_SHOT_KILL is read here.
     */
    u32  cheats;
    /* 0x8005827C/0x800582C0: admitted acid/lava hits request a body sound
     * before the general protection/armour stages. The world owns playback
     * and the shared lava cadence; standalone combat may omit this hook. */
    q2_combat_environment_fn environment_sound;
    void *environment_sound_user;
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

/*
 * Armour's rounding bias, 0x80057C1C / 0x80057C20, selected by 0x80057C10's
 * `lh 0x800B334A` and 0x80057C18's `bne v0, zero` — the SKILL halfword, the
 * same one the skill-0 damage halving reads at 0x800582D0. It is not the
 * deathmatch word: these constants used to be named _SP and _DM and were
 * driven by `rules->deathmatch`, which no instruction supports.
 */
#define Q2_ARMOUR_BIAS_EASY   4095   /* skill == 0: every fraction rounds up */
#define Q2_ARMOUR_BIAS_NORMAL 2048   /* skill 1 and 2: round to nearest      */

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

/*
 * What entity+222 holds when whatever hurt you was not a player.
 *
 * The four projectile spawners write the firing client's index, or this
 * literal when the owner has no client block: 0x8004A208 (grenade),
 * 0x8004ABF4, 0x8004B1C4 (rocket) and 0x8004BF80. 0x8003DDF8 places every
 * player with it (0x8003DE24 loads it, 0x8003DE34 stores it). It is also the
 * value 0x80057E5C tests before printing "Multiplayer, can't determine which
 * player hit other player".
 *
 * Four is deliberately OUT of range of the frag table (Q2_MP_MAX_PLAYERS is 4)
 * and, being non-negative, it also fails 0x80039728's `bne s1, -1` — so both
 * observables, the death voice and the frag, are off, provided the death
 * handler tests the byte RAW as the console does (`q2_mp_killer_field`). In
 * single player it also stands in for the pool slot 0x80057E88 computes (23
 * for a NULL attacker), which the death handler cannot tell from it. In
 * deathmatch it stands in for nothing: every arm there is modelled, including
 * the two that store nothing (see `last_attacker` below). And it is what
 * `q2_actor_init` seeds, so an actor nobody has hit owes nobody a frag.
 *
 * multiplayer.h carries the same guarded definition, so either header may be
 * included first.
 */
#ifndef Q2_MP_NOT_A_PLAYER
#define Q2_MP_NOT_A_PLAYER 4
#endif

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
    s16  entity_kind;       /* Population kind, entity+0xD2 (not CRE row)     */

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
    /*
     * FL_NO_KNOCKBACK, 0x800. IT GATES NOTHING ON THIS DISC, and this side used
     * to apply it to the wrong impulse. The bit has exactly one reader,
     * 0x80062914 `lhu v1, 32(s1)` / 0x8006291C `andi v0, v1, 0x800` /
     * 0x80062928 `addu s3, zero, zero`, and s3 is T_Damage's own `knockback`
     * ARGUMENT — its eighth, at caller 28(sp). T_Damage has one caller,
     * 0x800584B4, and 0x80058464 `sw zero, 28(sp)` passes that argument as
     * zero every time. So the bit zeroes a value that is already zero.
     *
     * The OUTER function's impulse (0x80057EC0..0x80058204) reads no flags word
     * at all, so gating it on this bit was an invented rule. The field is kept
     * because the creature modules do write entity+0x20, not because anything
     * on this disc acts on it.
     */
    bool no_knockback;
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
    /*
     * entity+222, the credited killer. NOT "-1 for the world": the damage
     * function never writes -1 into this byte in either mode.
     *
     *   - deathmatch, attacker with a client: 0x80057E04 stores the attacker's
     *     client index;
     *   - deathmatch, attacker without one: 0x80057E68 copies the ATTACKER's
     *     own +222 byte — whoever last hurt it — unless that byte is 4, when
     *     0x80057E5C prints "Multiplayer, can't determine which player hit
     *     other player" (0x800ACD9C) and nothing is stored;
     *   - deathmatch, NO attacker, target with a client: 0x80057DC8..0x80057E00
     *     store the TARGET's own client index, so the world's kill is the
     *     victim's suicide — it passes the frag hook's `slti s1, 4` and does
     *     not cry out. This port writes `owner` there;
     *   - deathmatch, NO attacker, target without a client: 0x80057DC0 stores
     *     nothing;
     *   - single player: 0x80057E88..0x80057EB8 stores
     *     `(attacker - 0x800CBA28) / 768`, the actor-array index. With a NULL
     *     attacker that chain evaluates to -2797289, whose low byte is 0x17, so
     *     the `sb` at 0x80057EB8 leaves **23**. This port records the
     *     attacker's `owner`, or 4.
     *
     * The one place in the image that stores a literal -1 here is 0x800396DC,
     * inside the death handler, and only for mods 9 and 10.
     *
     * `q2_actor_init` seeds 4 (0x8003DE34 places a player with it), and both
     * refreshes below CARRY the byte, because "nothing stored" has to mean
     * "what the last hit left" and not "whatever the refresh reset it to".
     */
    s8   last_attacker;
    s32  knockback[3];      /* entity+0x2F8..0x2FC                           */
    bool knocked;           /* entity+0x10C bit 0x4000                       */
    s16  last_mod;          /* entity+0xDF                                   */
    /*
     * HOW MANY TIMES entity+0xDF HAS BEEN WRITTEN. The console has no such
     * counter: the byte itself is the flag, and the live player's think reads
     * it at 0x8003ADF8 and clears it at 0x8003AE0C, which is what makes PAIN a
     * one-shot animation request.
     *
     * DEVIATION, and here is why. In the port `last_mod` has readers the
     * console's control flow never lets meet that clear — the killing tick's
     * death build (client main.c) and `q2_player_death_cries_out`, both of
     * which the console reaches through the death arm at 0x8003ADB8, which
     * returns (`j 0x8003b014`) before the pain read. Rather than reorder those,
     * the port splits the byte in two: `last_mod` keeps WHICH mod, and this
     * counts the writes so the pain read still has an edge to consume. Bumped
     * wherever the console stores the byte (0x80057E84, 0x80057EBC).
     */
    u32  damage_serial;
    /* Transient identity for the AI damage drain. The client compares this
     * with its live actor arrays; it never dereferences or serializes it. */
    struct q2_actor *damage_source;
    /* T_Damage reached its AI tail, including a hit absorbed to zero. */
    bool damage_reaction_pending;
    /*
     * Six damage-effect timer bytes, entity+0x2F0..0x2F5. The last slot is
     * real: the ticker 0x8005B830 reads +0x2F5 at 0x8005B844 and decrements it
     * at 0x8005B868, so the range ends at 0x2F5, not 0x2F4 as this line used to
     * say. effect[3] (+0x2F3): `q2psx-inspect access 0x2F3` finds no load or
     * store whose immediate offset is 0x2F3 in SLES_015.34, so nothing in the
     * main executable arms, reads or ticks it that way. That is all it
     * measures: the tool does not scan the relocated modules, skips $gp and
     * $sp bases, and cannot see an address formed by arithmetic. Which ticker
     * owns which slot is in effect.h, at q2_fx_actor_present (0x8005B880).
     *
     * Armed on the hit and run down on LATER ticks, so both refreshes below
     * carry them: neither q2_monster nor q2_inventory has a copy.
     */
    u8   effect[6];         /* entity+0x2F0..0x2F5                           */

    /*
     * entity+0x2AC — THE BODY'S OWN AMBIENT, the triplet that becomes the GTE
     * back colour when this actor is drawn (q2_light_env_build, 0x8006B468).
     *
     * 0x8005B880 opens by calling the fade 0x80075E14 with `steps` 7
     * (0x8005B88C `addiu a1, zero, 7` / 0x8005B894 `jal`), which walks +0x2AC a
     * seventh of the way toward +0x2B0 per call; and the effect[1] >= 3 arm at
     * 0x800586E8 copies the four bytes at 0x800AEAAC — the energy light's own
     * 0/255/0 — straight into +0x2AC, so an energy hit turns the body green for
     * a tick and it eases back out over the following ones.
     *
     * Carried across both refreshes below for the same reason effect[] is:
     * neither q2_monster nor q2_inventory holds a copy, and the fade only
     * means anything if the value survives the frame it was written in.
     *
     * The target at +0x2B0 is constant for these actors: 0x40 for creatures,
     * 0x20 for gameplay players. PAL/US allocation and player-spawn execution
     * establish both values; the shared creature spawner and every distinct
     * module's initialization retain 0x40 in the native fixture. The item
     * spawner's 0x30 does not apply to creatures or player bodies.
     */
    u8   ambient[3];        /* entity+0x2AC                                  */

    /*
     * ARMOUR_FADE, gp+1184 (0x800AEAA0), and the screen wash it gates.
     *
     * The console's is a single global byte, because the console can raise the
     * wash where it decides: CheckPowerArmour calls SetWinFade at 0x80057B80
     * and CheckArmour at 0x80057D24, both writing the viewport fields the
     * viewport draw at 0x80076764 reads. This port has no screen inside
     * combat.c, so the absorb stage records WHICH of the two fired and the
     * client drains it into q2_screen_flash_set — one raise per frame, which is
     * what the console's last-writer-wins on view+672 comes to.
     *
     * It lives on the actor rather than in a file-scope global because the
     * console's viewport is picked from the entity's own client pointer
     * (0x80057B3C `lw v1, 0(s1)`), so the raise belongs to the entity that was
     * hit — and because a global would be wrong the moment two players are in
     * the frame.
     *
     * 0x80057AB0 clears it as the FIRST instruction of CheckPowerArmour, before
     * any guard, so a hit that finds no shield clears it too.
     */
    s8   armour_fade;       /* Q2_ACTOR_FADE_*, gp+1184                      */
} q2_actor;

/*
 * The two SetWinFade raisers, with the operands each site passes.
 *
 *   POWER  0x800AEAA4 = 00 80 20 00 → rgb(0,128,32), a green wash;
 *          mode 2 (FADE) at 0x80057B54, strength 10 at 0x80057B38 and initial
 *          10 in [sp+16] at 0x80057B58 — ten drawn frames easing out.
 *   ARMOUR 0x800AEAA8 = 80 80 80 00 → rgb(128,128,128), grey;
 *          mode 2 at 0x80057CF8, strength 20 at 0x80057CDC and initial 20 at
 *          0x80057CFC — twenty frames.
 *
 * Both are far stronger and far longer than the health/armour delta flash the
 * HUD raises at 0x8003AE10 (grey 0x40, at most five frames), which is the only
 * one this port used to have.
 */
typedef enum q2_actor_fade {
    Q2_ACTOR_FADE_NONE   = 0,
    Q2_ACTOR_FADE_POWER  = 1,   /* 0x80057B80 */
    Q2_ACTOR_FADE_ARMOUR = 2    /* 0x80057D24 */
} q2_actor_fade;

#define Q2_ACTOR_FADE_MODE            2    /* SetWinFade's a2 at both sites  */
#define Q2_ACTOR_FADE_POWER_STRENGTH  10   /* 0x80057B38 / 0x80057B58        */
#define Q2_ACTOR_FADE_ARMOUR_STRENGTH 20   /* 0x80057CDC / 0x80057CFC        */

extern const u8 q2_actor_fade_power_rgb[3];    /* 0x800AEAA4 */
extern const u8 q2_actor_fade_armour_rgb[3];   /* 0x800AEAA8 */

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

/*
 * Move state between the port's existing structures and an actor.
 *
 * The two `from` refreshes rebuild the actor from its source every time they
 * run, EXCEPT for the entity state the source has no copy of, which they
 * carry: `effect[]` and `last_attacker` from both, plus `owner`, `env_next`
 * and `last_mod` from a player. A caller that means a NEW body — a respawn,
 * which 0x8003DDF8 builds from a freshly cleared entity — calls
 * `q2_actor_init` first.
 */
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

    /*
     * The per-mod hit sound, 0x800584D4..0x800585A0. These are the emitter's
     * own operands rather than a name invented for them, following the
     * precedent mover.h:236-244 set for the other 0x80040800 ids. The damage
     * function also makes the call itself: the pair goes to SetShockData on
     * the victim's window through the shock sink (shock.h), so a caller that
     * reads these is reading what was already requested.
     */
    u8   hit_sound_id;      /* 0x800ACE5C's entry; 0 when the mod is silent   */
    s16  hit_sound_vol;     /* 4096 or 2048                                   */
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
s16 q2_combat_power_armour_absorb(q2_actor *a, s32 damage);
s16 q2_combat_armour_absorb(q2_actor *a, s32 damage, bool energy,
                            bool power_armour_fired,
                            const q2_combat_rules *rules);

/*
 * Radius damage, 0x80050810.
 *
 * Applies to every actor in `targets` except `ignore`. Returns how many were
 * handed to the damage function. (Retail returns a FLAG: 0x80050A60 `addiu s3,
 * zero, 1` in the delay slot of the damage call, returned by 0x80050A78 — the
 * same answer for none or one, and "at least one" beyond that.) `attacker` is
 * credited with the damage — and, when it is itself in the list, takes HALF:
 * 0x80050A0C `sra s0, s0, 1` on the owner arm, before the `blez` reject at
 * 0x80050A10 rather than after it.
 *
 * `ignore` is the caller's own exclusion and is NOT the engine's owner skip:
 * 0x800508BC/0x800508C4 only skip the owner when the caller's tenth argument is
 * set, and all six projectile call sites store zero there (0x8004A048,
 * 0x8004A944, 0x8004A9DC, 0x8004AE48, 0x8004BBCC, 0x8004BC84). Only the generic
 * helper at 0x80050CC4 passes a non-zero flag.
 *
 * This entry point sweeps distance only, which is what it always did — retail
 * also occludes. Callers that have a world should use the traced form below.
 */
u32 q2_combat_radius_damage(q2_actor *attacker, q2_actor *ignore,
                            const s32 point[3], s16 damage, s16 radius,
                            s16 mod, q2_actor **targets, u32 count,
                            const q2_combat_rules *rules);

/*
 * "Can the blast see this candidate?", asked once per surviving candidate.
 * `from` is the blast point and `to` the candidate's origin, matching
 * 0x80050A1C/0x80050A20's arguments.
 */
typedef bool (*q2_combat_clear_fn)(void *ctx, const s32 from[3],
                                   const s32 to[3]);

/*
 * The same sweep with the two line-of-sight gates injected.
 *
 * `clear` runs between the falloff and the damage call, exactly where
 * 0x80050A24 and 0x80050A3C run, and a false result skips the candidate
 * without counting it. A NULL `clear` means "everything is visible", which is
 * what `q2_combat_radius_damage` passes.
 *
 * ONE CALLBACK ANSWERS FOR BOTH GATES, in their order: 0x80050A24's
 * 0x80044C44 is the swept move through the hull (q2_coll_move), and
 * 0x80050A3C's 0x80053974 is the clip against the entity boxes at 0x800CAE10.
 * simcombat.c's `splash_clear` supplies exactly that pair through
 * q2_sim_trace. The one input it does not pass is 0x80050A24's a3, the
 * caller's cell: q2_sim_trace starts from -1 and finds the first cell by brute
 * force (0x80044C74).
 */
u32 q2_combat_radius_damage_traced(q2_actor *attacker, q2_actor *ignore,
                                   const s32 point[3], s16 damage, s16 radius,
                                   s16 mod, q2_actor **targets, u32 count,
                                   const q2_combat_rules *rules,
                                   q2_combat_clear_fn clear, void *ctx);

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

/* Damage at fire_hit's computed contact point. The separate kick is delivered
 * by the creature host after this returns, as at 0x800613B0..0x800614B0. */
q2_damage_result q2_combat_melee(q2_actor *attacker, q2_actor *target,
                                 s16 damage, const s32 point[3],
                                 const q2_combat_rules *rules);

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
 * Older approximation of 0x800544EC's narrow phase, with the entity unpacked
 * as a vertical cylinder of
 * `radius` around `centre`, intersected with the Y slab that runs from
 * `centre.y + 286` upward by `height` (0x80054834..0x8005483C builds exactly
 * those two endpoints). `out_enter` and `out_exit` come back as 1.0.12
 * fractions along `origin`..`origin + dir`.
 *
 * Exposed because the shot path is not the only caller of 0x800544EC: the AI's
 * own trace helper runs it too (0x8005BF4C, behind the caller's 0x02000000 mask
 * bit) to stop a creature's step against another body. Sharing the arithmetic
 * rather than writing it twice is trace.h's rule about five copies of a slab
 * test being how they drift apart.
 *
 * These helpers retain exact-root/fraction approximations that differ from
 * native integer execution; see q2_combat_trace_actors for the measured
 * query. Their existing hitscan/AI consumers have not been migrated.
 * `q2_combat_centre_is_ahead` implements the conceptual dot-product gate:
 * a candidate behind the ray is not a candidate.
 */
bool q2_combat_cylinder_interval(const s32 origin[3], const s32 dir[3],
                                 const s32 centre[3], s32 radius, s16 height,
                                 s64 *out_enter, s64 *out_exit);
bool q2_combat_centre_is_ahead(const s32 origin[3], const s32 dir[3],
                               const s32 centre[3]);

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

/* Native 0x800544EC actor collision, including its integer root, clipping
 * after each accepted candidate and strict horizontal endpoint gate. The
 * caller supplies the native candidate set; damage immunity does not remove
 * a collision body. ignore is omitted before scanning, never after selection.
 * out receives the clipped point, or to if clear. NULL out returns the first
 * accepted candidate, as the native early-return mode does. */
s32 q2_combat_trace_actors(const s32 from[3], const s32 to[3],
                            const q2_actor *ignore, q2_actor *const *targets,
                            u32 count, s32 out[3]);

/* FindPlayerTarget, 0x80039DDC. Angles use native pitch/yaw/roll; the caller
 * converts the host pitch sign. The eleven elevation rays use the complete
 * actor list: retail overwrites its earlier projected-screen filter. The
 * visibility callback tests PrimaryColl only, after the actor query clips the
 * segment. A missing eye leaves direction/pitch unchanged and returns -1. */
typedef bool (*q2_aim_visible_fn)(void *user, const s32 from[3],
                                const s32 to[3], int ray);
s32 q2_combat_find_target(const s32 eye[3], const s32 angles[3],
                          const q2_actor *ignore, q2_actor *const *targets,
                          u32 count, q2_aim_visible_fn visible, void *user,
                          s16 direction[3], s16 *pitch);

/*
 * The same question cut short by the world: the nearest actor entered before
 * `world_fraction` (1.0.12, 4096 = the whole segment), with the 1.0.12
 * fraction at which it is entered in `out_along`. What q2_combat_fire_bullet
 * asks before it deals damage, for a caller that has to deal it itself.
 */
s32 q2_combat_nearest_hit(const s32 origin[3], const s32 dir[3],
                          s32 world_fraction, s32 fallback_radius,
                          q2_actor **targets, u32 count, s64 *out_along);

/*
 * THE NATIVE HITSCAN'S ACTOR HALF, which every instant weapon on the disc
 * shares: the bullet (0x8004891C), the rail (0x80049318), the creatures'
 * rounds through those two, the mission laser (0x80048F8C) and the creature
 * laser (0x80049AA8) all call LineIntersectCreCylinder 0x800544EC, and only
 * that.
 *
 * The world is traced first; `world_fraction` is where it stopped `origin +
 * dir` (4096 when nothing did). The actor query then runs to THAT point and
 * overwrites it with the nearest body's entry, which is the point DoDamage is
 * handed. The query ignores `attacker` and nothing else: an immune body still
 * stops the shot and shields what is behind it, and it is DoDamage's own first
 * test (0x80057D54) that refuses the damage. `point`, when given, receives the
 * entry or, with no body hit, the world's stopping point.
 *
 * Returns the index of the actor hit, or -1.
 */
s32 q2_combat_hitscan(q2_actor *attacker, const s32 origin[3],
                      const s32 dir[3], s16 damage, s16 mod,
                      s32 world_fraction, q2_actor **targets, u32 count,
                      const q2_combat_rules *rules, q2_damage_result *out,
                      s32 point[3]);

/*
 * One bullet, 0x8004874C: q2_combat_hitscan with mod 18. `fallback_radius` is
 * unused — the native query reads each body's own radius — and is kept for
 * the callers that still pass it.
 */
s32 q2_combat_fire_bullet(q2_actor *attacker, const s32 origin[3],
                          const s32 dir[3], s16 damage, s32 world_fraction,
                          s32 fallback_radius, q2_actor **targets, u32 count,
                          const q2_combat_rules *rules,
                          q2_damage_result *out);

/*
 * The rail, 0x8004917C. ONE query and ONE DoDamage (mod 3, `a3 = 3` at
 * 0x80049330): the beam is drawn to the world's end, but only the nearest
 * body along it is hurt. Quake II on the PC penetrates; this console's rail
 * does not. Returns 1 when a body was hit, else 0.
 */
u32 q2_combat_fire_rail(q2_actor *attacker, const s32 origin[3],
                        const s32 dir[3], s16 damage, s32 world_fraction,
                        s32 fallback_radius, q2_actor **targets, u32 count,
                        const q2_combat_rules *rules);

#endif /* Q2PSX_COMBAT_H */
