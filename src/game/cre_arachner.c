/*
 * cre_arachner.c — the Arachner, transcribed from its own module.
 *
 * `POWER1 CreAIBin` module 'Arachner', 6,772 bytes relocated to 0x80100000,
 * and the only map on the disc that ships it. Every address below is inside
 * that image at that base, which is also `Q2_CREWORLD_BASE`, so the module
 * addresses quoted here are the ones the loader and the decoder both use.
 *
 * This file used to carry one callback — the attack — and left the other nine
 * on the generic handler. All ten are read out now, together with the four
 * think functions, the module's method table, its sound registrations and its
 * spawn function.
 *
 * ---------------------------------------------------------------------------
 * There is no id Arachner — but its GUN is id's Gladiator, line for line
 * ---------------------------------------------------------------------------
 * A wall-walking, twin-railgun spider is not in PC Quake II, so no single
 * ancestor covers the whole creature. Two of its functions have one anyway,
 * and the correspondence is close enough to be evidence rather than
 * resemblance. An adversarial re-read found this; the first pass through the
 * module said "no ancestor function" and stopped, which understated what is
 * checkable here.
 *
 *   arachner_attack (module+0x12CC) IS id's `gladiator_attack`:
 *
 *       VectorSubtract(self->s.origin, self->enemy->s.origin, v)  <- SELF
 *           first, which is id's own inverted order in this one function
 *       range = VectorLength(v)                    ; import +0xB8
 *       if (range <= MELEE_DISTANCE + 32) return;  ; id's "a small safe zone"
 *       VectorCopy(self->enemy->s.origin, self->pos1);   ; entity+0x5C here
 *       self->pos1[2] += self->enemy->viewheight;
 *       self->monsterinfo.currentmove = <the gun move>
 *
 *     One departure: id plays a charge-up sound between the safe zone and the
 *     pos1 copy. This module plays nothing there — module+0x1338 branches
 *     straight into the four stores at module+0x1344.
 *     One addition: id has no second gun move, so id has nothing matching the
 *     `|dx| + |dz| < |dy|` choice at module+0x13B8. That branch is the PSX's.
 *
 *   ara_rail_fire (module+0xBA8 and module+0xD58) IS id's `GladiatorGun`:
 *
 *       resolve a muzzle point
 *       VectorSubtract(self->pos1, start, dir); VectorNormalize(dir);
 *       play the fire sound
 *       monster_fire_railgun(self, start, dir, 50, 100, <flash>)
 *
 *     Departures: the muzzle comes from a model attachment (import +0x2C)
 *     rather than id's `monster_flash_offset` table, the flash index is 0
 *     rather than MZ2_GLADIATOR_RAILGUN_1, and there are TWO of them.
 *
 * So `blind_target` is id's `pos1`, and the safe zone is id's safe zone.
 * Outside those two functions what CAN be checked is every constant the
 * creature shares with the lineage, and each of these was read before it was
 * recognised:
 *
 *   attack safe zone             module+0x1338, `slti 1053` — that is
 *                                `range <= 1052` and 1052 is
 *                                Q2_MELEE_DISTANCE + 32, id's own expression.
 *                                See ARA_ATTACK_MIN_RANGE for the caveat.
 *   rail damage 50, kick 100     module+0xCE8 / +0xCF4, and id's own figures
 *                                for a monster railgun (GladiatorGun)
 *   "wep_railgf1a"               module+0x1E4 — id's weapons/railgf1a.wav
 *   pain debounce 30 ticks       module+0x1450, 3 s on the 10 Hz clock, id's
 *   no flinch at skill 3         module+0x14BC, id's nightmare rule
 *   skinnum = 1 at half health   module+0x1428, id's `skinnum |= 1`
 *   deadflag == DEAD_DEAD (2)    module+0x1588, the re-entry guard in `die`
 *   svflags |= SVF_DEADMONSTER   module+0x156C and +0x1064, id's bit 1
 *   takedamage = DAMAGE_YES (1)  module+0x15DC, so a corpse can still be gibbed
 *   movetype STEP 5 -> TOSS 7    module+0x112C spawn; TOSS at module+0x105C
 *                                (arachner_corpse) and module+0x1564 (the gib
 *                                arm of `die`). Both are `or` of 0x001C0000
 *                                after `and` of 0xFFC3FFFF. NOT module+0x1064
 *                                — that address is the svflags store above,
 *                                and an earlier draft of this header had it.
 *   solid = SOLID_BBOX (2)       module+0x1138
 *
 * The last four are the interesting ones, because they name two entity fields
 * this port had only half of. `entity+0x20` is not just `flags`: the low
 * halfword is id's FL_*, and the upper one is three packed enums —
 *
 *     bits 16..17   solid       SOLID_BBOX 2 at spawn
 *     bits 18..21   movetype    MOVETYPE_STEP 5 at spawn, MOVETYPE_TOSS 7 dead
 *     bits 22..23   deadflag    DEAD_DEAD 2, and `die` returns early on it
 *
 * — and `entity+0x1C` is not just `spawnflags`: bits 30..31 are `takedamage`,
 * which the death path sets to DAMAGE_YES. All four values are id's own enum
 * members and all four arrive in the arm you would expect them in, which is
 * four independent agreements rather than one.
 *
 * monster.h NAMES ALL FOUR NOW, over these bit positions and with these enum
 * members, having reached the same layout from seven modules independently —
 * `Q2_SOLID_*`, `Q2_MOVETYPE_*`, `Q2_DEAD_*`, `Q2_DAMAGE_*` — and `q2_monster`
 * carries `solid`, `movetype`, `deadflag` and `takedamage` as plain bytes
 * rather than as one packed word, because the port has no reason to reproduce
 * the packing. So every store listed above is WRITTEN below instead of
 * described, and this header no longer owes a report on what it would take.
 * `mass` and `skinnum` arrived with them, which is the rest of the spawn
 * function and the flinch's half-health skin.
 *
 * ---------------------------------------------------------------------------
 * Two railguns, and the module names them itself
 * ---------------------------------------------------------------------------
 * The module init resolves two muzzle poses through import +0x34
 * (`pose_blend_two_positions`) and prints an error when either fails:
 *
 *     module+0x19D0  from animation positions 239 and 253
 *                    "Arachner left Railgun muzzle points are from different
 *                     submodels"                              module+0x238
 *     module+0x19D8  from animation positions 290 and 307
 *                    "Arachner right Railgun muzzle points are from different
 *                     submodels"                              module+0x27C
 *
 * Think 4 reads the first record and think 3 the second, so think 3 is the
 * RIGHT gun and think 4 the LEFT. Both attack moves fire them in that order,
 * right then left, and the two functions are otherwise identical except for
 * one thing: the right one re-reads the enemy's position afterwards and the
 * left one does not (module+0xD00 exists, module+0xEB0 goes straight to the
 * epilogue). So the right gun aims, fires and re-aims; the left gun fires at
 * where the right gun just looked.
 *
 * ---------------------------------------------------------------------------
 * The aim is a SNAPSHOT, not the enemy
 * ---------------------------------------------------------------------------
 * `entity+0x5C..+0x64` — `q2_monster.blind_target` — is written by the attack
 * callback and by the right gun's think, each time as
 *
 *     blind_target    = enemy->origin
 *     blind_target[1] += enemy->view_height
 *
 * and both fire thinks aim at THAT rather than at the enemy directly. This is
 * why the earlier version of this file could only say "what reads them is not
 * established": the readers are the two thinks, and the field is the whole
 * reason the Arachner's shots lag a moving target.
 *
 * ---------------------------------------------------------------------------
 * WALK AND RUN ARE THE SAME NINE FRAMES
 * ---------------------------------------------------------------------------
 * This is the one creature on the disc where "a move is named by its first
 * frame" (crebind.h) cannot name a move. The walk callback installs
 * module+0x17DC and the run callback installs module+0x1808, and the two
 * records are
 *
 *     module+0x17DC   {first 16, last 24, frames module+0x17C0, endfunc 0}
 *                     nine frames of {ai 2 = ai_walk, dist 10, think 0}
 *     module+0x1808   {first 16, last 24, frames module+0x17EC, endfunc 0}
 *                     nine frames of {ai 3 = ai_run,  dist 10, think 0}
 *
 * Same range, same distance, same (absent) think, different AI VERB. Asking
 * `q2_cre_find_move` for frame 16 returns whichever of the two the decoder
 * found first — the walk, because callbacks are walked in slot order — so a
 * running Arachner would have been steered by `ai_walk`, which does not
 * pursue an enemy. `q2_cre_set_move_at` installs by the module address
 * instead, which is unambiguous, and it is used ONLY for this pair; every
 * other move here goes through the normal frame-number lookup.
 *
 * That helper used to be a private one in this file. It is crebind's now — the
 * Tank Commander's walk (module+0x1BC0) and run (module+0x1C3C) share frames
 * 34..49 the same way, two transcriptions had grown the same twenty lines, and
 * the shared copy skips clip pieces for the same reason the private one did.
 *
 * ---------------------------------------------------------------------------
 * What is still owed
 * ---------------------------------------------------------------------------
 *   - The muzzle. Both fire thinks build a world point from the pose record,
 *     the object's rotation matrix at object+0x2C0 and its origin at +0xA4.
 *     This port has no object and no matrix, so the shot has no start point
 *     and no direction. Its FIGURES are no longer owed — `q2_cre_shot` carries
 *     damage, kick and flash, so the 50 / 100 / 0 the module hands
 *     monster_fire_railgun now reaches the host, which resolves the rail along
 *     the line of sight instead of from the barrel.
 *   - The QUIET pain cry, which is the one thing here that is read and still
 *     undeliverable. module+0x19A4 is not a second sample: the module's init
 *     CLONES module+0x19A0 through import +0x3C and drops the clone's volume to
 *     50 through import +0x44. The sound hook carries a handle and no volume,
 *     so both of the Arachner's pain cries come out at full. See
 *     ARA_SND_PAIN2_QUIET and `ara_play`.
 *   - `export 1` at module+0xAAC, which is a render-time hook rather than an
 *     AI one: while the object's animation position lies inside the model's
 *     "Melee" clip it starts two blur trails through import +0x78, over
 *     positions {290, 299} and {239, 244} with colour 0x00202020 and six
 *     samples each. `q2_cre_impl` has no slot for it, so it is named and not
 *     written.
 *   - `module+0x1284` plays `ara_melee1` (module+0x19B0) and NOTHING CALLS IT.
 *     It is not in the method table, not a callback and not a move endfunc.
 *     `msc_udeath` (module+0x19C8) is likewise registered and never read. Two
 *     dead sounds, and they are the disc's rather than this port's, so neither
 *     is given a constant below.
 */
#include <stdlib.h>

#include "ai.h"
#include "crebind.h"
#include "creworld.h"
#include "monster.h"

/* ------------------------------------------------------------------------- */
/* The move set, by first frame                                               */
/* ------------------------------------------------------------------------- */
#define ARA_STAND           0    /*   0..12   module+0x1738  "Stand"        */
/*
 * Frames 16..24 are DELIBERATELY absent from this list. Two records share
 * them and a frame number cannot pick between the two; see ARA_MOVE_WALK and
 * ARA_MOVE_RUN below, and the header.
 */
#define ARA_ATTACK2        25    /*  25..33   module+0x18A4  (unnamed)      */
#define ARA_PAIN1          35    /*  35..39   module+0x1920  "Pain 1"       */
#define ARA_PAIN2          40    /*  40..45   module+0x1944  "Pain 2"       */
#define ARA_MELEE          53    /*  53..64   module+0x183C  "Melee"        */
#define ARA_SWAY           65    /*  65..77   module+0x1770  "Sway"         */
#define ARA_REAR           78    /*  78..93   module+0x17B0  "Rear"         */
#define ARA_ATTACK3        94    /*  94..109  module+0x1900  "Attack 3"     */
#define ARA_DEATH2        110    /* 110..129  module+0x1990  "Death 2"      */
#define ARA_START_ATTACK  130    /* 130..132  module+0x18C0  "Start Attack" */
#define ARA_START_MELEE   133    /* 133..135  module+0x1858  "Start Melee"  */
#define ARA_END_MELEE     136    /* 136..138  module+0x1874  "End Melee"    */
/*
 * "Sway" and "Rear" are reached by no callback and by no endfunc — the census
 * gives both `via -2`. They are in the module and nothing on the disc plays
 * them, which is why there is no handler here that installs either.
 */

/* The two records that share frames 16..24, told apart by address alone: the
 * first is nine frames of ai_walk and the second nine frames of ai_run, and
 * they are identical in every other respect. See the header. */
#define ARA_MOVE_WALK (Q2_CREWORLD_BASE + 0x17DCu)   /* 16..24, ai_walk */
#define ARA_MOVE_RUN  (Q2_CREWORLD_BASE + 0x1808u)   /* 16..24, ai_run  */

/* ------------------------------------------------------------------------- */
/* Sounds, as the module addresses of their handles                           */
/* ------------------------------------------------------------------------- */
#define ARA_SND_PAIN2       (Q2_CREWORLD_BASE + 0x19A0u)  /* ara_pain2      */
/*
 * NOT A SAMPLE. module+0x19A4 is a runtime CLONE of module+0x19A0, made by the
 * init at module+0x870 through import +0x3C, then changed through import +0x44.
 * That setter is (handle, pitch, volume, priority): this call passes
 * (clone, 50, 128, -1), so 50 replaces the clone's pitch modifier, while 128
 * and -1 leave its volume and priority unchanged. Pitch 0 is the setter's
 * unchanged sentinel, but this call does not pass it.
 * So the Arachner's two pain sounds are one sample at the same volume and
 * priority, played at the default 35/32 pitch or the clone's 50/32 pitch; the
 * coin in `arachner_pain` chooses the pitch, not the sample or volume.
 */
#define ARA_SND_PAIN2_QUIET (Q2_CREWORLD_BASE + 0x19A4u)
#define ARA_SND_DEATH       (Q2_CREWORLD_BASE + 0x19A8u)  /* ara_deth1      */
#define ARA_SND_RAIL        (Q2_CREWORLD_BASE + 0x19ACu)  /* wep_railgf1a   */
#define ARA_SND_IDLE        (Q2_CREWORLD_BASE + 0x19BCu)  /* ara_idle1      */
#define ARA_SND_SEARCH      (Q2_CREWORLD_BASE + 0x19C0u)  /* ara_srch1      */
#define ARA_SND_SIGHT       (Q2_CREWORLD_BASE + 0x19C4u)  /* ara_sght1      */
/*
 * module+0x19B0 (`ara_melee1`) and module+0x19C8 (`msc_udeath`) are registered
 * and never played — see the header. Deliberately absent, because a constant
 * nothing uses reads as an unfinished sound rather than as the disc's silence.
 *
 * `ara_idle1` and `ara_srch1` are in no bank on the disc, which is expected and
 * is not this file's problem: the hook asks for them by the module address and
 * the client counts the miss.
 */

/*
 * The hooks, shared with cre_soldier.c (which defines them) and cre_actions.c.
 *
 * The FIRE hook is no longer among them. It took a single packed int, which
 * could name the spawner and not the shot; `q2_cre_fire_shot` takes the
 * module's own figures and does the enemy-alive guards itself, so the rail
 * goes through that instead and this file no longer reaches for
 * `q2_cre_fire_fn`. See `ara_rail_fire`.
 */
extern void (*q2_cre_sound_fn)(q2_monster *m, int which, void *user);
extern void  *q2_cre_sound_user;
extern void (*q2_cre_melee_fn)(q2_monster *m, const s32 aim[3],
                               s32 damage, s32 kick, void *user);
extern void  *q2_cre_melee_user;

static void ara_play(q2_monster *m, u32 handle_addr)
{
    /*
     * The quiet pain handle is asked for by the address of the sample it was
     * cloned from, because that is the one the module registered a NAME for
     * (`q2_creature_world_sound_for_addr` resolves an address against the
     * registrations, creworld.c) and because the port's sound hook carries no
     * volume argument. The consequence is audible and is stated rather than
     * hidden: half the Arachner's pain cries should be quiet and all of them
     * come out loud.
     *
     * HOW quiet is not established. The module asks for 50 (module+0x87C) and
     * the neighbouring pan argument is 128, which import +0x44 treats as its
     * centre sentinel — so the fields are bytes and 50 is somewhere near a
     * fifth of a 255 full scale. Nothing read here fixes the top of the volume
     * range, so "a fifth" would be an inference and is not asserted.
     */
    if (handle_addr == ARA_SND_PAIN2_QUIET)
        handle_addr = ARA_SND_PAIN2;

    if (q2_cre_sound_fn)
        q2_cre_sound_fn(m, (int)handle_addr, q2_cre_sound_user);
}

/* import +0x14, 0..32767. */
static s32 ara_rand(void)
{
    return (s32)(rand() & 0x7FFF);
}

/*
 * `slti v0, v0, 16385` at module+0x14D4 — the module's own integer, which is
 * one past half of 32768 rather than one below it. Kept as written.
 */
#define ARA_R_HALF 16385

/* The melee both thinks swing, module+0xEE4..+0xF20 and +0xF80..+0xFBC. */
#define ARA_MELEE_DAMAGE_BASE  20
#define ARA_MELEE_DAMAGE_SPAN   5
#define ARA_MELEE_KICK        100

/*
 * The rail both thinks fire — the last three arguments of
 * `monster_fire_railgun(self, start, dir, damage, kick, flash)`, import +0x8C.
 * Right gun module+0xCE8 / +0xCF0 / +0xCFC, left gun module+0xE98 / +0xEA0 /
 * +0xEAC, and the two are the same three instructions in the same order.
 *
 * 50 and 100 are id's own figures for `GladiatorGun`, which is the function
 * this one is (see the header). The flash is an `sw zero` in the fire call's
 * delay slot in both guns — index 0, not id's MZ2_GLADIATOR_RAILGUN_1, and the
 * module has no flash table for it to index.
 */
#define ARA_RAIL_DAMAGE  50
#define ARA_RAIL_KICK   100
#define ARA_RAIL_FLASH    0

/*
 * `sh 400, 0x4E(self)` at module+0x1098, the first store the spawn function
 * makes. The module decoder reads the same halfword into `q2_creature.mass`
 * independently and gets 400 as well, so this constant has two readers behind
 * it. It is also id's Gladiator mass — deliberately NOT listed with the
 * lineage constants in the header, because 400 is a round number that several
 * of id's monsters share and it would carry no weight there.
 *
 * `q2_monster.mass` is a live field now; nothing in the framework fills it, so
 * the spawn hook below is what puts the module's number on the creature.
 */
#define ARA_MASS 400

/*
 * module+0x1338: `slti v0, v0, 1053` on the length of (self - enemy).
 *
 * IT IS id's NUMBER, and an earlier draft of this file said it was not. The
 * immediate is one ABOVE the source's constant, not one below it, because the
 * source comparison is `<=`: `range <= 1052` compiles to `range < 1053`. And
 *
 *     1052 = 1020 + 32 = Q2_MELEE_DISTANCE + 32
 *
 * which is `gladiator_attack`'s safe zone written out — id's line is
 * `if (range <= (MELEE_DISTANCE + 32)) return;`, in a function this one
 * otherwise matches store for store (see the header). Two constants that both
 * have to be right for the decomposition to work, in the one id function that
 * has this shape, is why this is a read and not a coincidence.
 *
 * THE CAVEAT, because it is the one thing that does not line up. The port
 * scales id's units by 12 and this module does not scale the 32: id's
 * MELEE_DISTANCE is 80 while Q2_MELEE_DISTANCE is 1020 (= 85 * 12, itself not
 * 80 * 12), so the reach was re-chosen for the console, but the `+ 32` beside
 * it was carried across untouched. Compare cre_soldier.c's SOL_PAIN4_VELOCITY,
 * where id's 100 DID become 1200. So: the expression is id's, the melee term
 * is the console's own, and the 32 is id's literal left unscaled. Nothing here
 * is inferred backwards from id — 1053 was read off module+0x1338 first.
 *
 * The effect either way: the Arachner declines to open fire from inside arm's
 * length and a little beyond, and installs no move at all when it declines.
 */
#define ARA_ATTACK_MIN_RANGE 1053

/*
 * `Q2_SVF_DEADMONSTER` is monster.h's now, beside `Q2_SVF_MONSTER`, and this
 * file's private `ARA_SVF_DEADMONSTER 0x2u` is gone with it: five of the seven
 * modules set the same bit in their gib arm, and the frame driver was already
 * testing the bare literal 2 (monster.c).
 *
 * `ara_set_move_addr` is gone the same way — `q2_cre_set_move_at` (crebind.c)
 * is the same twenty lines, skipping clip pieces for the same reason.
 */

/* ------------------------------------------------------------------------- */
/* The aim snapshot                                                           */
/* ------------------------------------------------------------------------- */
/*
 * module+0x1344..+0x1380 in the attack callback and module+0xD00..+0xD3C in
 * the right gun's think, instruction for instruction the same four stores.
 * Index 1 is the vertical axis and it points DOWN, so adding `view_height`
 * moves the aim point from the enemy's origin to its eye exactly as
 * `visible()` does (monster.h). This is id's `pos1` copy out of
 * `gladiator_attack`; see the header.
 *
 * THE NULL GUARD BELOW IS THE PORT'S. Neither module site has one: module+0x1344
 * loads entity+0xBC and dereferences it immediately, and so does module+0xD00
 * inside the right gun's think. The attack callback is safe because the AI
 * never calls it without an enemy, but the gun's think runs off an animation
 * frame and would fault on a target that died mid-burst. Guarding costs
 * nothing the module does — every store here is unconditional in both arms —
 * and it leaves `blind_target` holding the last live snapshot, which is what
 * the fire thinks then shoot at.
 */
static void ara_track_enemy(q2_monster *self)
{
    if (!self->enemy)
        return;

    self->blind_target[0] = self->enemy->pos[0];
    self->blind_target[1] = self->enemy->pos[1] + self->enemy->view_height;
    self->blind_target[2] = self->enemy->pos[2];
}

/* ------------------------------------------------------------------------- */
/* Think functions, by the index the animation frames use                     */
/* ------------------------------------------------------------------------- */

/*
 * [2] and [8] — module+0xED8 and module+0xF74, and they are BYTE IDENTICAL.
 * Both are one call:
 *
 *     fire_hit(self, {1020, -48, 0}, 20 + rand() % 5, 100)
 *
 * The aim triple is built on the stack rather than copied from a static, which
 * is the only difference from the Berserk's two claws. 1020 is
 * `Q2_MELEE_DISTANCE`; component 1 is the vertical one with the axis pointing
 * down, so -48 is id's `up = +4` at the AI's scale of 12; component 2 is id's
 * `right` and the Arachner leaves it at zero, so the bite is dead ahead.
 *
 * The division at module+0xF10..+0xF3C is the compiler's magic-number divide by
 * five (0x66666667), not a mask, so the spread really is `% 5` and not `& 7`.
 *
 * "Melee" (53..64) uses think 2 on frame 58 and think 8 on frame 63, so the
 * creature bites twice per swing with the same figures both times. Two module
 * functions, so two functions here.
 */
static void ara_bite(q2_monster *self)
{
    s32 aim[3];

    aim[0] = Q2_MELEE_DISTANCE;         /* 1020, module+0xEE4 */
    aim[1] = -Q2_AI_UNITS(4);           /*  -48, module+0xF00 */
    aim[2] = 0;                         /*    0, module+0xF0C */

    if (q2_cre_melee_fn)
        q2_cre_melee_fn(self, aim,
                        ARA_MELEE_DAMAGE_BASE +
                            (ara_rand() % ARA_MELEE_DAMAGE_SPAN),
                        ARA_MELEE_KICK, q2_cre_melee_user);
}

static void arachner_bite1(q2_monster *self) { ara_bite(self); }  /* [2] */
static void arachner_bite2(q2_monster *self) { ara_bite(self); }  /* [8] */

/*
 * The shared body of the two railgun thinks — module+0xBA8 (right) and
 * module+0xD58 (left). In the module's order:
 *
 *   1. resolve the muzzle: import +0x2C with the pose record (module+0x19D8
 *      right, module+0x19D0 left), the model at object+0x10, the animation
 *      position at object+0x100, and a tag taken from object+0xB0 — bit 0 set
 *      means `object[0xB0] >> 17`, clear means -1.
 *   2. rotate it into the world with import +0x28 against object+0x2C0, then
 *      add the object's origin at +0xA4..+0xAC. That is import +0x38 done
 *      inline.
 *   3. dir = blind_target - start, then import +0xC4 to normalise it.
 *   4. play module+0x19AC, `wep_railgf1a`.
 *   5. import +0x8C: monster_fire_railgun(self, start, dir, 50, 100, 0).
 *
 * STEPS 1 THROUGH 3 CANNOT BE RUN. Every one of them reads the link object —
 * the model, the animation position, the rotation matrix, the world origin —
 * and this port keeps none of it on `q2_monster`. So the shot has no muzzle
 * and no direction.
 *
 * STEP 5 NOW DOES. `q2_cre_shot` carries what the module passes, so the slot,
 * the flash, the damage and the kick all go through together and the host
 * resolves the rail along the line of sight instead of from the muzzle. The
 * four fields a railgun has no argument for — speed, hspread, vspread, count —
 * stay zero because the module passes nothing for them; the rail is one trace
 * and not a spread of pellets, and main.c reads a zero count as one shot.
 *
 * ONE DEPARTURE, and it is the shared helper's rather than this file's:
 * `q2_cre_fire_shot` declines a shot with no enemy or a dead one, and neither
 * module site checks. Both fire at `blind_target`, which is a SNAPSHOT and
 * outlives the enemy that made it, so on the console the second barrel of a
 * burst still discharges into where a target just died. The sound is played
 * first here exactly as the module plays it first (module+0xCC4 before
 * module+0xCF8), so the shot being declined does not silence the gun.
 */
static void ara_rail_fire(q2_monster *self)
{
    q2_cre_shot shot;

    ara_play(self, ARA_SND_RAIL);

    /*
     * `Q2_IMP_FIRE_RAILGUN` is import +0x8C, which is the slot both thinks
     * call: `lw v1, 140(s0)` off the module base at module+0xCEC and +0xE9C.
     */
    shot.slot    = Q2_IMP_FIRE_RAILGUN;
    shot.flash   = ARA_RAIL_FLASH;
    shot.damage  = ARA_RAIL_DAMAGE;
    shot.speed   = 0;                   /* a rail has no projectile speed  */
    shot.kick    = ARA_RAIL_KICK;
    shot.hspread = 0;                   /* and no spread: the module       */
    shot.vspread = 0;                   /* passes six arguments, not eight */
    shot.count   = 0;                   /* and no pellet count             */

    q2_cre_fire_shot(self, &shot);
}

/*
 * [3] the RIGHT railgun — module+0xBA8, muzzle pose module+0x19D8.
 *
 * It re-reads the enemy's position after firing (module+0xD00..+0xD3C), which
 * the left gun does not, so the two shots of one attack are aimed one AI tick
 * apart. Frame 99 of "Attack 3" and frame 27 of the 25..33 attack.
 */
static void arachner_rail_right(q2_monster *self)
{
    ara_rail_fire(self);
    ara_track_enemy(self);
}

/*
 * [4] the LEFT railgun — module+0xD58, muzzle pose module+0x19D0.
 *
 * Identical to the right gun up to the fire call and then it simply returns —
 * module+0xEB0 is the epilogue. Frame 105 of "Attack 3" and frame 31 of the
 * 25..33 attack, in both cases after the right gun has re-aimed.
 */
static void arachner_rail_left(q2_monster *self)
{
    ara_rail_fire(self);
}

/*
 * [7] arachner_corpse — module+0x1040, the end callback on "Death 2" and the
 * only reason that move ever stops.
 *
 *     nextthink = 0;                 ; entity+0x90
 *     movetype  = MOVETYPE_TOSS;     ; entity+0x20 bits 18..21, 5 -> 7
 *     svflags  |= SVF_DEADMONSTER;   ; entity+0x40
 *
 * It must be in the method table below and not merely called from `die`:
 * crebind.c resolves a move's endfunc by looking its address up among the
 * callbacks and the methods, and module+0x1040 is only a method. Without it
 * the death animation loops for ever.
 *
 * It does not touch `deadflag` — `arachner_die` already set DEAD_DEAD before
 * installing the move — and it does not shrink the bounding box, which is one
 * of the two extra things id's monster-death code does. Neither is a shortfall
 * here; the module does neither.
 *
 * The movetype store is module+0x105C, `and` of 0xFFC3FFFF then `or` of
 * 0x001C0000 — bits 18..21 set to 7, MOVETYPE_TOSS — and it is written now
 * that `q2_monster` has the field. A body that falls is the point of it.
 */
static void arachner_corpse(q2_monster *self)
{
    self->next_think = 0;
    self->movetype   = Q2_MOVETYPE_TOSS;     /* module+0x105C */
    self->svflags   |= Q2_SVF_DEADMONSTER;   /* module+0x1064 */
}

/* ------------------------------------------------------------------------- */
/* The monsterinfo callbacks                                                  */
/* ------------------------------------------------------------------------- */

/* arachner_stand — module+0x11CC. Four instructions: currentmove = Stand. */
static void arachner_stand(q2_monster *self)
{
    q2_cre_set_move(self, ARA_STAND);
}

/* arachner_idle — module+0x11DC. One call: play(module+0x19BC), ara_idle1. */
static void arachner_idle(q2_monster *self)
{
    ara_play(self, ARA_SND_IDLE);
}

/* arachner_search — module+0x124C. One call: play(module+0x19C0), ara_srch1. */
static void arachner_search(q2_monster *self)
{
    ara_play(self, ARA_SND_SEARCH);
}

/*
 * arachner_sight — module+0x1214. One call: play(module+0x19C4), ara_sght1,
 * and nothing else. The Soldier's sight callback opens with a running shot at
 * range; this one only announces itself.
 */
static void arachner_sight(q2_monster *self, q2_monster *other)
{
    (void)other;
    ara_play(self, ARA_SND_SIGHT);
}

/* arachner_walk — module+0xEC8. Four instructions: currentmove = module+0x17DC.
 * Installed by address, not by frame 16; see the header. */
static void arachner_walk(q2_monster *self)
{
    q2_cre_set_move_at(self, ARA_MOVE_WALK);
}

/*
 * arachner_run — module+0x1010.
 *
 *     if (aiflags & AI_STAND_GROUND) currentmove = Stand;
 *     else                           currentmove = module+0x1808;
 *
 * Two arms and no start-run animation between them, which is the whole
 * function. The stand-ground branch is the one every transcribed creature's
 * run callback opens with.
 */
static void arachner_run(q2_monster *self)
{
    if (self->aiflags & Q2_AI_STAND_GROUND) {
        q2_cre_set_move(self, ARA_STAND);
        return;
    }

    q2_cre_set_move_at(self, ARA_MOVE_RUN);
}

/*
 * arachner_melee — module+0x12BC. Four instructions: currentmove =
 * module+0x1858, "Start Melee" (133..135).
 *
 * That move's endfunc (module+0x11AC) installs "Melee" (53..64), whose own
 * endfunc (module+0x11BC) installs "End Melee" (136..138), which ends on the
 * run callback. Three moves and two three-instruction installers between the
 * decision and the bite; crebind.c's `chain_endfunc` is what carries it, since
 * a bare installer is neither a callback nor a method.
 */
static void arachner_melee(q2_monster *self)
{
    q2_cre_set_move(self, ARA_START_MELEE);
}

/*
 * arachner_attack — module+0x12CC.
 *
 *     v    = self->origin - enemy->origin      ; note the order: self first
 *     if (import[+0xB8](v) < 1053) return;     ; installs NOTHING at all
 *     blind_target = enemy eye position
 *     move = (|v.x| + |v.z| < |v.y|) ? "Attack 3" : "Start Attack"
 *
 * IT CAN DECLINE. Below 1053 the function returns without touching
 * `currentmove`, so the creature carries on with whatever it was playing.
 * `q2_cre_generic_attack` cannot express that — `set_via` always installs
 * something — and it is why a close Arachner should not snap into an attack.
 *
 * THE CHOICE IS VERTICAL. `|dx| + |dz| < |dy|` asks whether the target is
 * further away up-or-down than it is along the floor, which for a creature
 * that walks walls is the question that decides which attack it uses. It is
 * not a range band and it is not a roll.
 *
 * BOTH ARMS SHOOT, which an earlier reading of this function got wrong. The
 * horizontal arm is not "a three-frame gesture": "Start Attack" (130..132)
 * carries no think at all, but its endfunc at module+0x119C installs the
 * unnamed move 25..33, whose think bytes are `0*2 3 0*3 4 0*2` — the same two
 * railgun thinks "Attack 3" (94..109) has at `0*5 3 0*5 4 0*4`. So the
 * vertical arm fires after five frames and the horizontal arm winds up for
 * three frames first and then fires after two.
 *
 * The length is taken with `q2_vector_length`, which is import +0xB8's own
 * function (0x8005C4E8, named in ai.h), rather than by comparing squares. The
 * squared form would order identically; using the real one keeps the boundary
 * on the same side of 1053 as the console's, since that sqrt truncates.
 */
static void arachner_attack(q2_monster *self)
{
    s32 v[3];
    s32 ax, ay, az;

    /*
     * The module dereferences `enemy` unguarded at module+0x12DC. The AI never
     * calls attack without one, so this guard is the port's and not the disc's;
     * it returns rather than installing something, which is the same thing the
     * range test below does.
     */
    if (!self->enemy)
        return;

    v[0] = self->pos[0] - self->enemy->pos[0];
    v[1] = self->pos[1] - self->enemy->pos[1];
    v[2] = self->pos[2] - self->enemy->pos[2];

    if (q2_vector_length(v) < ARA_ATTACK_MIN_RANGE)
        return;

    ara_track_enemy(self);

    ax = v[0] < 0 ? -v[0] : v[0];
    ay = v[1] < 0 ? -v[1] : v[1];
    az = v[2] < 0 ? -v[2] : v[2];

    if (ax + az < ay)
        q2_cre_set_move(self, ARA_ATTACK3);
    else
        q2_cre_set_move(self, ARA_START_ATTACK);
}

/*
 * arachner_pain — module+0x13E8, called as (self, other, kick, damage).
 *
 * In order, and every branch is the module's:
 *
 *   1. `if (health < max_health / 2) skinnum = 1` — module+0x13FC..+0x1428.
 *      `max_health` is the halfword at entity+0x50 and `health` the halfword at
 *      object+0x108; the division is a signed `>> 1` with the sign bias. The
 *      module ASSIGNS 1 where id writes `skinnum |= 1`, which for a creature
 *      with two skins is the same thing — `sh 1, 0x3A(self)` at module+0x1428,
 *      the whole field and not the low bit. `q2_monster.skinnum` is that field
 *      now and is written; `m->hurt` stays beside it because it is what the
 *      skin picker reads (soldier_skin, cre_soldier.c) and the two cannot
 *      disagree here — one store sets both.
 *
 *   2. `if (level.time < pain_debounce_time) return;` then
 *      `pain_debounce_time = level.time + 30` — module+0x1434..+0x1454.
 *      Thirty ticks of the 10 Hz clock is id's three seconds. `level.time` is
 *      the second word of import +0x48.
 *
 *   3. `if (rand() & 4)` play module+0x19A0 else play module+0x19A4 — one
 *      sample at two volumes; see ARA_SND_PAIN2_QUIET. Bit 2 of a 15-bit
 *      random is a fair coin, so this is a coin and not a weighted roll.
 *
 *   4. `if (skill == 3) return;` — module+0x14A4..+0x14BC, reading
 *      `cvars[0]->value` off import +0x4C. id's nightmare rule: the sound
 *      still plays and the skin still turns, and only the flinch is skipped.
 *
 *   5. `currentmove = rand() < 16385 ? "Pain 1" : "Pain 2"`.
 *
 * NOTHING HERE READS THE DAMAGE, and now that the callback carries it that is
 * a statement about the module rather than about the port. `self` moves to
 * `s0` at module+0x13F0 and `a0` is then REUSED at module+0x1400 to hold the
 * object pointer; `a1`, `a2` and `a3` are never read at all. Unlike the Soldier,
 * which promotes a flinch to PAIN4 on a hard upward throw, and unlike the Tank
 * Commander and the Gunner, which split three flinches on `damage <= 10` and
 * `<= 25`, the Arachner's two pain moves are chosen by the coin alone.
 */
static void arachner_pain(q2_monster *self, s16 damage)
{
    (void)damage;   /* read by no branch of module+0x13E8; see above */

    /*
     * A CORPSE DOES NOT FLINCH. The class routes damage to `die` or to `pain`
     * and never to both, so the module is simply never reached once the body is
     * dead; this port's damage path has no such split, and without the guard a
     * dying Arachner re-enters the flinch and freezes partway through its
     * death. The same guard `soldier_pain` carries, for the same reason.
     */
    if (self->dead)
        return;

    if (self->health < self->max_health / 2) {
        self->hurt    = true;
        self->skinnum = 1;              /* module+0x1428, the literal 1 */
    }

    if (q2_level_state.time < self->pain_debounce)
        return;
    self->pain_debounce = q2_level_state.time + 30;   /* 3 s at 10 Hz */

    ara_play(self, (ara_rand() & 4) ? ARA_SND_PAIN2 : ARA_SND_PAIN2_QUIET);

    if (q2_cre_skill() == 3)
        return;

    q2_cre_set_move(self, ara_rand() < ARA_R_HALF ? ARA_PAIN1 : ARA_PAIN2);
}

/*
 * arachner_die — module+0x1508.
 *
 * The gib arm first, and with no dead guard in front of it (module+0x1534):
 *
 *     nextthink = 0;
 *     deadflag  = DEAD_DEAD;        ; entity+0x20 bits 22..23 = 2
 *     movetype  = MOVETYPE_TOSS;    ; entity+0x20 bits 18..21 = 7
 *     svflags  |= SVF_DEADMONSTER;
 *
 * which is `arachner_corpse` plus the deadflag, done at once because there is
 * no animation to wait for. It plays NO SOUND: the module registers
 * `msc_udeath` and this arm does not reach for it, so an Arachner blown apart
 * is silent where a Soldier is not. That is the disc's, not a gap here.
 *
 * Then the normal arm (module+0x1578):
 *
 *     if (deadflag == DEAD_DEAD) return;
 *     play(module+0x19A8);          ; ara_deth1
 *     currentmove = "Death 2";
 *     deadflag    = DEAD_DEAD;
 *     takedamage  = DAMAGE_YES;     ; entity+0x1C bits 30..31 = 1
 *
 * `takedamage = DAMAGE_YES` on a body that has just died is what lets a corpse
 * still be gibbed, and it is id's own value in id's own place. Both fields
 * exist on `q2_monster` now and both are written — `deadflag` alongside this
 * port's `dead`, which stays the guard so that the two cannot drift apart, and
 * `takedamage` where the module's `or` of 0x40000000 puts it (module+0x15DC).
 *
 * THE NORMAL ARM DOES NOT SET MOVETYPE. Only the gib arm does; a body that
 * plays its death animation stays MOVETYPE_STEP until "Death 2" ends and
 * `arachner_corpse` turns it to TOSS. That is the module's split and it is why
 * the corpse method exists at all.
 *
 * DAMAGE IS NOT READ HERE EITHER. The callback carries it now, and this
 * function reloads `a1` with the object pointer at module+0x1518 before
 * looking at it: the arm is chosen by `health` against `gib_health`
 * (entity+0x52 versus object+0x108, module+0x1528), not by the blow. Contrast
 * the Berserk, whose `die` picks its long death on `damage >= 50`.
 *
 * There is ONE death move. The module names 110..129 "Death 2" and there is no
 * "Death 1" anywhere in its table, so there is no random choice to make.
 */
static void arachner_die(q2_monster *self, s16 damage)
{
    (void)damage;   /* read by neither arm of module+0x1508; see above */

    if (self->health <= self->gib_health) {
        /*
         * Every store in the module's gib arm is idempotent, so running it
         * twice is harmless there and here.
         *
         * `gibbed` AND `dead` ARE THE PORT'S, NOT THE MODULE'S. The arm makes
         * exactly four stores — nextthink 0 (module+0x1548), deadflag DEAD_DEAD
         * (module+0x1554), movetype TOSS (module+0x1564) and svflags |= 2
         * (module+0x156C) — and there is no fifth field for "was blown apart".
         *
         * All four are written now. `dead` and `gibbed` are the two that are
         * still THIS PORT'S: `dead` is the guard the console spells as
         * `deadflag == DEAD_DEAD` and is kept in step with the real field
         * beside it, and `gibbed` carries the distinction monster.h keeps ("a
         * body already dead can still be gibbed by a later explosion") which
         * the entity has no field for. cre_berserk.c and cre_soldier.c raise
         * the same two flags in the same place, for the same reason.
         */
        self->gibbed     = true;
        self->dead       = true;
        self->next_think = 0;
        self->deadflag   = Q2_DEAD_DEAD;         /* module+0x1554 */
        self->movetype   = Q2_MOVETYPE_TOSS;     /* module+0x1564 */
        self->svflags   |= Q2_SVF_DEADMONSTER;   /* module+0x156C */
        return;
    }

    if (self->dead)
        return;

    self->dead = true;
    ara_play(self, ARA_SND_DEATH);
    q2_cre_set_move(self, ARA_DEATH2);
    self->deadflag   = Q2_DEAD_DEAD;      /* module+0x15D0 */
    self->takedamage = Q2_DAMAGE_YES;     /* module+0x15DC */
}

/* ------------------------------------------------------------------------- */
/* The spawn function                                                         */
/* ------------------------------------------------------------------------- */
/*
 * module+0x1070, export 0. The full store list in the module's own order:
 * mass 400 to entity+0x4E (module+0x1098, `sh 400`); pain and die to +0xA0 and
 * +0xA4; stand, walk, run, attack, melee, sight, search and idle to
 * +0xE0..+0x100; zero to +0xF4 (no dodge); movetype = MOVETYPE_STEP and THEN
 * solid = SOLID_BBOX into entity+0x20 (module+0x112C then module+0x1138 — that
 * order, one read-modify-write chain on one word);
 * link_entity(self, 5) and link_entity(self, 0x85) through import +0x1C;
 * currentmove = Stand to +0xD8; speed scale 10 to +0x13B; and finally
 * walkmonster_start(self) through import +0xFC.
 *
 * It writes NO health and NO skinnum and has no per-variant dispatch — one
 * class byte, 64, one method table, and the health comes from the class table
 * row. That is the whole difference from the Soldier's spawn function.
 *
 * AND IT IS CALLED NOW. `q2_creature_spawn` runs `impl->spawn` last — after
 * the class byte, the speed scale and the callbacks, and after creworld.c has
 * applied the class row's health, which is the loader's own order (0x8007E68C
 * writes health and 0x8007E698 gib_health, and only THEN does 0x8007E6AC call
 * export 0). So the opening move lands: an Arachner no longer stands inert
 * with a NULL `currentmove` waiting for whichever AI callback happens first.
 *
 * The class byte, the speed scale and the callbacks are the framework's
 * (crebind.c). What this hook carries is everything else the module's export 0
 * writes that the framework does not: the mass, the movetype, the solid type
 * and the opening move. The two `link_entity` calls have no counterpart here —
 * this port has no server-side link stage — and they are named rather than
 * faked; 5 and 0x85 are the module's own arguments, unread beyond that.
 */
static void arachner_spawn(q2_monster *self)
{
    self->mass     = ARA_MASS;              /* module+0x1098 */
    self->movetype = Q2_MOVETYPE_STEP;      /* module+0x112C */
    self->solid    = Q2_SOLID_BBOX;         /* module+0x1138 */

    q2_cre_set_move(self, ARA_STAND);       /* module+0x116C */
}

/* ------------------------------------------------------------------------- */
const q2_cre_impl q2_cre_arachner = {
    "Arachner",
    {
        arachner_stand,     /*  0 stand       module+0x11CC */
        arachner_idle,      /*  1 idle        module+0x11DC */
        arachner_search,    /*  2 search      module+0x124C */
        arachner_walk,      /*  3 walk        module+0xEC8  */
        arachner_run,       /*  4 run         module+0x1010 */
        NULL,               /*  5 dodge       — the spawn function writes ZERO
                             *                  to entity+0xF4 (module+0x1124),
                             *                  so this is the module's own
                             *                  decision rather than a gap. */
        arachner_attack,    /*  6 attack      module+0x12CC */
        arachner_melee,     /*  7 melee       module+0x12BC */
        (q2_class_method)(void *)arachner_sight,  /* 8 sight  module+0x1214 */
        NULL,               /*  9 checkattack — the module installs none, so
                             *                  monster_start fills the slot
                             *                  with the shared M_CheckAttack
                             *                  (import +0xE8, and 0x80061B18
                             *                  only writes it when NULL). */
        NULL,               /* 10 bigturn     — the module installs none;
                             *                  entity+0x108 is never written
                             *                  and the threshold at +0x140
                             *                  stays zero, which disables it.
                             *                  Worth saying twice for a
                             *                  wall-walker: it turns by
                             *                  M_ChangeYaw like everything
                             *                  else. */
        /*
         * 11 and 12 go through the same cast slot 8 does, and for the same
         * reason: `q2_class_method` is `void (*)(q2_monster *)` and these two
         * take the damage as well. `q2_creature_spawn` casts them straight back
         * to their real signature on the way into `m->pain` and `m->die`
         * (crebind.c), which is the only place they are ever called from.
         */
        (q2_class_method)(void *)arachner_pain,  /* 11 pain  module+0x13E8 */
        (q2_class_method)(void *)arachner_die    /* 12 die   module+0x1508 */
    },
    {
        /*
         * Nine methods, from the table the module builds at module+0x19F0 and
         * registers for class byte 64 with import +0x118 (module+0x420). Slot 0
         * is written as an explicit zero by module+0x3C8, so it is the module's
         * own NULL and not an omission — no frame uses think 0, which is what
         * the census's long runs of `0*n` mean.
         */
        NULL,                       /*  0 — the module writes zero here      */
        arachner_walk,              /*  1   module+0xEC8, the walk callback
                                     *      again; no frame references it, but
                                     *      it is in the module's table so it
                                     *      is here                           */
        arachner_bite1,             /*  2   module+0xED8                      */
        arachner_rail_right,        /*  3   module+0xBA8                      */
        arachner_rail_left,         /*  4   module+0xD58                      */
        arachner_run,               /*  5   module+0x1010, the run callback
                                     *      again, and likewise unreferenced
                                     *      by any frame                      */
        NULL,                       /*  6 — module+0x1A08 is never written and
                                     *      the image is zero there, so the
                                     *      module leaves this slot empty      */
        arachner_corpse,            /*  7   module+0x1040, and it MUST be here
                                     *      or "Death 2"'s endfunc cannot be
                                     *      resolved: crebind.c maps an endfunc
                                     *      address to a callback or a method,
                                     *      and module+0x1040 is only a method */
        arachner_bite2,             /*  8   module+0xF74                      */
        /*
         * 9..31 — the module registers nine methods and no more, so these are
         * absent rather than unwritten. crebind.c falls each of them back to
         * the generic trampoline, which finds no decoded think and does
         * nothing, which is what an unregistered slot does on the console too.
         */
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL
    },
    arachner_spawn
};
