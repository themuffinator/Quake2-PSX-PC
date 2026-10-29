/*
 * cre_gunner.c — the Gunner, transcribed from its own module.
 *
 * The Gunner ships on two of the disc's thirteen AI maps, POWER1 and WASTE3.
 * Its module is `POWER1 CreAIBin` module 'Gunner', 7,624 bytes relocated, and
 * every address below is inside it at a base of 0x80100000. Class byte 79,
 * animation scale 12, mass 200; the class table's row 10 gives it health 149
 * and gib_health -70.
 *
 * ---------------------------------------------------------------------------
 * It is PC Quake II's gunner, line for line
 * ---------------------------------------------------------------------------
 * This started as one transcribed callback — `gunner_attack` — with everything
 * else on the generic handlers. Reading the rest turns it into id's `gunner.c`
 * with the numbers intact, and several of them are not numbers anyone would
 * have tuned to produce a match:
 *
 *     gunner_dodge      module+0x17B4   `random() > 0.25` as rand() < 8193
 *     gunner_fidget     module+0x12C4   `random() <= 0.05` as rand() < 1639
 *     gunner_pain       module+0x15AC   damage <= 10 / <= 25, the three arms
 *     gunner_duck_down  module+0x137C   skill >= 2, `random() > 0.5`,
 *                                       pausetime = level.time + 1 s
 *     gunner_refire_chain module+0x11E8 `random() <= 0.5` as rand() < 16385
 *     GunnerGrenade     module+0x1138   damage 50, speed 600
 *     GunnerFire        module+0x9A8    damage 3, kick 4, spread 300 x 500,
 *                                       enemy lead of -0.2 x velocity
 *
 * 300 and 500 are id's DEFAULT_BULLET_HSPREAD and DEFAULT_BULLET_VSPREAD to the
 * unit, -819/4096 is id's -0.2 lead, and 50/600 is id's `monster_fire_grenade
 * (self, start, aim, 50, 600, flash_number)`. Mass 200 and gib_health -70 are
 * id's gunner as well. Only the health differs: id's 175 against the disc's 149.
 *
 * ---------------------------------------------------------------------------
 * The two muzzles are built once, at init
 * ---------------------------------------------------------------------------
 * The module's init calls `pose_blend_two_positions` (import +0x34) twice and
 * keeps the results in its own BSS:
 *
 *     module+0x828   blend(model, 227, 231) -> module+0x1D28   the chain gun
 *     module+0x874   blend(model, 273, 281) -> module+0x1D30
 *
 * `GunnerFire` hands module+0x1D28 to `attachment_local_offset`, and so does
 * export 1, the flash light — which is why they light up in the same place.
 * `GunnerGrenade` does not use either record: it asks
 * `attachment_world_point` for position 273 outright, the first half of the
 * second blend. Import +0x34 has no caller anywhere in the EXECUTABLE — it
 * exists purely for modules — but the Gunner is not the only module that takes
 * it up: the Soldier calls it once (BASE0, module+0xF44), the Infantry once
 * (POWER2), the Arachner twice (POWER1) and the Tank Commander four times
 * (COMMAND). An earlier draft of this comment called the Gunner "the disc's
 * only use of it", which is false and is corrected here.
 *
 * ---------------------------------------------------------------------------
 * Three things the spawn function tells us that were not written down
 * ---------------------------------------------------------------------------
 * `module+0xB78` (export 0) writes the word at entity+0x20 twice with masks
 * that also appear, identically, in the Soldier's spawn at `module+0x16F0`:
 * bits 16..17 are set to 2 and bits 18..21 to 5. `gunner_dead` (module+0x134C)
 * and the gib arm of `gunner_die` then rewrite bits 18..21 to 7, and both die
 * arms set bits 22..23 to 2.
 *
 *     bits 16..17   solid       2 = SOLID_BBOX      (id's monster spawn)
 *     bits 18..21   movetype    5 = MOVETYPE_STEP, 7 = MOVETYPE_TOSS
 *     bits 22..23   deadflag    2 = DEAD_DEAD       (monster.c already had this)
 *
 * and, at entity+0x1C, bits 30..31 are a two-bit `takedamage`: `gunner_die`
 * and `gunner_duck_down` set 1 (DAMAGE_YES), `monster_duck_up` sets 2
 * (DAMAGE_AIM), which are id's values for exactly those three lines.
 *
 * ALL FOUR ARE WRITTEN NOW. This used to say none of them had a field in this
 * port's `q2_monster` and that each was named at the point it would have gone;
 * monster.h carries `solid`, `movetype`, `deadflag` and `takedamage` as their
 * own members, with the bit layout above documented there, so the names below
 * are assignments rather than comments. `mass` is the same case, and the spawn
 * hook writes it.
 *
 * ---------------------------------------------------------------------------
 * Two things the census gets wrong about this creature
 * ---------------------------------------------------------------------------
 * Both are the same decoder bug — walking past a `jr ra` into the next
 * function — and both were checked against the module's own tables:
 *
 *   * **Think 13 does nothing.** `q2psx-inspect creatures` reports
 *     `[13] flashlight(+A0)?`, the muzzle-flash light. Method table entry 13 is
 *     written at module+0x94C and it is `module+0x14B4`, which is two
 *     instructions: `jr ra` / `nop`. The `call(+A0)` belongs to **export 1**
 *     at module+0x14BC, the per-object flash-light routine the ENGINE calls
 *     while drawing — not a think. Leaving method[13] NULL here would let the
 *     generic fallback run that mis-decode, so it carries an explicit no-op.
 *
 *   * **The "Runshoot" move is orphaned.** The census attributes 102-107 to
 *     the walk callback. It is installed by a function at module+0x159C with
 *     exactly `gunner_walk`'s shape — `lui / addiu / jr ra / sw` in the delay
 *     slot, four words — sitting immediately after `gunner_walk`'s own `jr ra`,
 *     and `xrefs 0x8010159C` finds zero references of any kind. The disc ships
 *     a Gunner run-and-shoot animation that no code path can reach.
 *
 * ---------------------------------------------------------------------------
 * The chain gun is on the far side of two endfuncs
 * ---------------------------------------------------------------------------
 * `gunner_attack` never installs the firing move. It installs 137-143 "Attack
 * Chain", whose endfunc `module+0x11D8` installs 144-151 "Fire Chain" — eight
 * consecutive `GunnerFire` frames — whose endfunc `module+0x11E8` decides
 * between going round again and 152-158 "End Chain".
 *
 * Both are in the module's METHOD TABLE, at indices 3 and 4 (module+0x8CC and
 * +0x8D8), which is what lets `crebind`'s `resolve_endfunc` name them: it looks
 * an endfunc address up among the callbacks and the methods. Index 3 is a bare
 * installer so `chain_endfunc` was already covering it; index 4 is not, so
 * until now the Fire Chain move had a NULL endfunc and looped forever — a
 * Gunner that opened fire never stopped. Filling method[3] and method[4] is
 * what closes the chain.
 *
 * Index 9 (`gunner_dead`) is the same case and matters for the same reason:
 * `module+0x134C` is not a callback either, and the Death move's decoded
 * `endfunc_move` is zero, so `chain_endfunc` could not stand in and the death
 * animation ended in nothing.
 *
 * Indices 5 and 8 are filled because the MODULE registers them, NOT because
 * anything needed them: `resolve_endfunc` (crebind.c) walks the thirteen
 * callbacks BEFORE the method table, and `module+0x127C` is callback 0 while
 * `module+0x131C` is callback 4, so both endfuncs already resolved with these
 * two slots NULL. An earlier draft claimed all three were filled "for the same
 * reason"; only index 9 shares index 4's problem.
 *
 * The full table, read from module+0x8A8..+0x954 into module+0x1D40:
 *
 *     [0] NULL              [5] gunner_stand      [10] gunner_duck_down
 *     [1] GunnerGrenade     [6] gunner_idlesound  [11] monster_duck_hold
 *     [2] GunnerFire        [7] gunner_fidget     [12] monster_duck_up
 *     [3] gunner_fire_chain [8] gunner_run        [13] (empty)
 *     [4] gunner_refire_chain [9] gunner_dead
 *
 * ---------------------------------------------------------------------------
 * What is still owed
 * ---------------------------------------------------------------------------
 * Two entries left this list. `pain` now takes the damage — monster.h's
 * callback is `void (*)(q2_monster *, s16)` and `q2_monster_damage_reaction`
 * passes T_Damage's own amount — so the three-way flinch split is live rather
 * than pinned to its lightest arm. And `q2_cre_shot` carries the figures, so
 * the damage, speed, kick and spread read out of the two fire thinks are
 * delivered instead of recorded. What remains:
 *
 *   * Muzzle posing now uses the host attachment hook: vertices 227/231 for
 *     the chain gun and vertex 273 for the grenade. The shot carries the
 *     module's aim, including the chain gun's negative velocity lead.
 *   * Nothing calls `dodge`. Slot 5 is installed — `crebind.c` wires it — and
 *     the executable has no reader for entity+0xF4 at all, which crebind.c
 *     records at length. The handler is here because the module writes it.
 *   * (No longer owed.) `q2_cre_fire_shot` used to refuse a shot with no
 *     enemy or a dead one; it only counts them now, so every grenade frame
 *     reaches the hook. That is the module's shape: `GunnerGrenade` never
 *     loads the enemy, `GunnerFire` reads its origin with no null test
 *     (module+0xA8C..+0xA94), and neither loads its health. The health test is
 *     the refire's, module+0x11E8 (`lh v0, 264(v0)` at +0x120C, `blez` at
 *     +0x1214): whether the chain fires again, not whether this round leaves.
 */
#include "random.h"
#include <stdlib.h>

#include "ai.h"
#include "crebind.h"
#include "monster.h"

/* ------------------------------------------------------------------------- */
/* The move set, by first frame                                               */
/* ------------------------------------------------------------------------- */
#define GUN_STAND         0     /*   0..29   "Stand"         module+0x1ABC */
#define GUN_FIDGET       30     /*  30..69   "Fidget"        module+0x1A50 */
#define GUN_WALK         76     /*  76..88   "Walk"          module+0x1AF4 */
#define GUN_RUN          94     /*  94..101  "Run"           module+0x1B1C */
/*
 * 102..107 "Runshoot" (module+0x1B40) is deliberately absent. Its installer at
 * module+0x159C has no references anywhere in the image — see the header.
 */
#define GUN_ATTACK_GRENADE  108 /* 108..128  "Attack 1"      module+0x1CF4 */
#define GUN_ATTACK_CHAIN    137 /* 137..143  "Attack Chain"  module+0x1C54 */
#define GUN_FIRE_CHAIN      144 /* 144..151  "Fire Chain"    module+0x1C7C */
#define GUN_END_CHAIN       152 /* 152..158  "End Chain"     module+0x1CA4 */
#define GUN_PAIN1           159 /* 159..176  "Pain1"         module+0x1BD0 */
#define GUN_PAIN2           177 /* 177..184  "Pain2"         module+0x1B88 */
#define GUN_PAIN3           185 /* 185..189  "Pain3"         module+0x1B60 */
#define GUN_DEATH           190 /* 190..200  "Death"         module+0x1C04 */
#define GUN_DUCK            201 /* 201..208  "Duck"          module+0x1C2C */

/* ------------------------------------------------------------------------- */
/*
 * Sounds, and the module names them itself.
 *
 * Fourteen calls to `sound_index_by_name` (import +0x24) in all: seven in the
 * init (first `jalr` at module+0x450, handles stored at module+0x478, +0x4C4,
 * +0x56C, +0x614, +0x6BC, +0x760 and +0x82C) and seven more in the spawn
 * function (first `jalr` at module+0xD58). The init additionally writes an
 * explicit zero to +0x1D18 at module+0x770, which the spawn then fills with
 * `par_atck2`. Each call takes a 12-byte literal by value and the returned
 * handle is stored to a fixed word of module BSS. Eight distinct handle words
 * result. Six of them (+0x1D08, +0x1D0C, +0x1D10, +0x1D14, +0x1D1C, +0x1D20)
 * are written twice, once by each pass; +0x1D24 only the init resolves, and
 * +0x1D18 only the spawn does — the init just zeroes it.
 *
 * The names were read out of `POWER1/COMMON.DAT` at the file offsets the
 * relocation puts module+0x1C8..+0x204 at, so the address-to-name map below is
 * recovered rather than inferred from slot order:
 *
 *     module+0x1C8  "gun_pain1"    -> +0x1D08 and +0x1D0C (registered twice)
 *     module+0x1D4  "gun_death1"   -> +0x1D10
 *     module+0x1E0  "gun_sight1"   -> +0x1D20
 *     module+0x1EC  "gun_srch1"    -> +0x1D1C
 *     module+0x1F8  "gun_idle1"    -> +0x1D14
 *     module+0x204  "msc_udeath"   -> +0x1D24
 *     module+0x2A0  "par_atck2"    -> +0x1D18
 *
 * `par_atck2` is registered and never played by any code read here; it is
 * listed so its absence from the calls below is a fact rather than a gap.
 * Only one of the two `gun_pain1` handles (+0x1D08) is ever used.
 *
 * The hook is handed the module ADDRESS, not an index — that is what
 * `cre_actions.c` passes and what `client_cre_sound` resolves first, through
 * the module's own registrations. The `int` round-trip is the same one
 * `cre_actions.c` relies on: the client casts back to u32.
 */
#define GUN_SND_PAIN    0x80101D08u
#define GUN_SND_DEATH   0x80101D10u
#define GUN_SND_IDLE    0x80101D14u
#define GUN_SND_SEARCH  0x80101D1Cu
#define GUN_SND_SIGHT   0x80101D20u
#define GUN_SND_UDEATH  0x80101D24u

/*
 * The sound hook, shared with cre_soldier.c and cre_actions.c.
 *
 * The FIRE hook used to be declared here too — `q2_cre_fire_fn`, one `int` —
 * and both fire thinks below went through it. They go through
 * `q2_cre_fire_shot` now, which carries the module's own figures, so the extern
 * is gone rather than kept unused.
 */
extern void (*q2_cre_sound_fn)(q2_monster *m, int which, void *user);
extern void  *q2_cre_sound_user;

static void gun_play(q2_monster *m, u32 handle_addr)
{
    if (q2_cre_sound_fn)
        q2_cre_sound_fn(m, (int)handle_addr, q2_cre_sound_user);
}

/* random(), in the module's own fixed point: import +0x14 returns 0..32767. */
static s32 gun_rand(void)
{
    return (s32)(q2_random_next() & 0x7FFF);
}

/* The module's literal thresholds, kept as the integers it uses. */
#define GUN_R_0_25   8193       /* `random() > 0.25` at module+0x17D8 */
#define GUN_R_0_05   1639       /* `random() <= 0.05` at module+0x12F8 */
#define GUN_R_0_5   16385       /* `random() <= 0.5`  at module+0x1248 */

/* ------------------------------------------------------------------------- */
/*
 * The two weapons, with the arguments the module hands the engine's spawners.
 *
 * Every figure here was already read for the first pass and had nowhere to go:
 * the old fire hook carried a single `int` and could name the spawner and
 * nothing else. `q2_cre_shot` (crebind.h) takes the lot, so these are the
 * module's own call sites written out. A field the weapon does not take is
 * left at zero and said to be zero, because crebind.h's hook declines a shot
 * whose damage is zero rather than inventing one — an unread figure has to
 * show.
 *
 * Damage 3 / kick 4 and damage 50 / speed 600 are id's own numbers for
 * `GunnerFire` and `GunnerGrenade`; 300 and 500 are DEFAULT_BULLET_HSPREAD and
 * DEFAULT_BULLET_VSPREAD to the unit.
 */
static const q2_cre_shot gun_shot_grenade = {
    .slot    = Q2_IMP_FIRE_GRENADE,  /* import +0x94, called at module+0x11BC */
    .flash   = 0,       /* sp+20, module+0x11C0 — and it stays zero on all
                         * four grenade frames, where id varies the flash     */
    .damage  = 50,      /* a3,    module+0x11AC                               */
    .speed   = 600,     /* sp+16, module+0x11B4                               */
    .kick    = 0,       /* monster_fire_grenade takes neither a kick...       */
    .hspread = 0,
    .vspread = 0,       /* ...nor a spread: six arguments, and that is all    */
    .count   = 1        /* crebind.h's convention for anything but a shotgun  */
};

static const q2_cre_shot gun_shot_bullet = {
    .slot    = Q2_IMP_FIRE_BULLET,   /* import +0x84, called at module+0xB58  */
    .flash   = 0,       /* sp+28, module+0xB5C                                */
    .damage  = 3,       /* a3,    module+0xB38                                */
    .speed   = 0,       /* a hitscan bullet has no speed argument             */
    .kick    = 4,       /* sp+16, module+0xB3C                                */
    .hspread = 300,     /* sp+20, module+0xB44                                */
    .vspread = 500,     /* sp+24, module+0xB50                                */
    .count   = 1        /* one bullet a frame; the shotgun path is the
                         * Soldier's, not this creature's                     */
};

/* ------------------------------------------------------------------------- */
/* Think functions, by the index the animation frames use                     */
/* ------------------------------------------------------------------------- */

/*
 * [1] GunnerGrenade — module+0x1138.
 *
 * Four of these on the grenade attack, at frames 112, 115, 118 and 121, which
 * is id's four spaced `GunnerGrenade` calls in `gunner_frames_attack_grenade`.
 *
 * What it does, in order:
 *
 *     AngleVectors(self->angles, forward, right, NULL)      import +0xD8
 *     aim = forward                                          (a straight copy,
 *                                                             id's VectorCopy)
 *     attachment_world_point(self->object, 273, start)      import +0x38
 *     monster_fire_grenade(self, start, aim, 50, 600, 0)    import +0x94
 *
 * 50 and 600 are id's own damage and speed. 273 is the model attachment
 * position that stands in for id's `monster_flash_offset[flash_number]`, and
 * it is CONSTANT across all four frames where id varies the flash number —
 * which is a real departure and the console's own.
 *
 * Resolve vertex 273 alone, then pass the body's forward vector to the
 * engine wrapper, which supplies the toss and spread. Neither this muzzle
 * nor this direction depends on the enemy surviving the volley.
 */
static void gunner_grenade(q2_monster *self)
{
    q2_cre_shot shot = gun_shot_grenade;

    shot.aimed = true;
    q2_cre_muzzle(self, 273, 273, shot.start);
    q2_angle_vectors(self->angles, shot.dir, NULL, NULL);
    q2_cre_fire_shot(self, &shot);
}

/*
 * [2] GunnerFire — module+0x9A8.
 *
 * Eight consecutive frames of it are the whole of the Fire Chain move, which
 * is id's `gunner_frames_fire_chain` exactly. Read in order:
 *
 *     attachment_local_offset(&local, model, pose(module+0x1D28),
 *                             object->anim_position, tag)   import +0x2C
 *     local_to_world_svector(object->matrix, &local, &local) import +0x28
 *     start = object->origin + local
 *     target = enemy->origin
 *     VectorMA(target, -819, enemy->velocity, target)        import +0xC0
 *     target[1] += enemy->view_height
 *     aim = target - start
 *     VectorNormalize(aim)                                   import +0xC4
 *     monster_fire_bullet(self, start, aim, 3, 4, 300, 500, 0)  import +0x84
 *
 * -819/4096 is -0.19995, id's `VectorMA (target, -0.2, enemy->velocity,
 * target)`. Damage 3 and kick 4 are id's; 300 and 500 are DEFAULT_BULLET_HSPREAD
 * and DEFAULT_BULLET_VSPREAD unchanged. Index 1 is the vertical axis here and
 * the view height is ADDED, which is the same sign `visible()` uses.
 *
 * The muzzle hook supplies the posed 227/231 attachment; the module's own
 * velocity correction and eye height determine the trace direction.
 */
static void gunner_fire(q2_monster *self)
{
    q2_cre_shot shot = gun_shot_bullet;
    s32 target[3];
    int k;

    shot.aimed = true;
    q2_cre_muzzle(self, 227, 231, shot.start);
    if (self->enemy) {
        q2_vector_ma(self->enemy->pos, -819, self->enemy->velocity, target);
        target[1] += self->enemy->view_height;
        for (k = 0; k < 3; k++)
            shot.dir[k] = target[k] - shot.start[k];
        q2_vector_normalize(shot.dir);
    } else {
        q2_angle_vectors(self->angles, shot.dir, NULL, NULL);
    }
    q2_cre_fire_shot(self, &shot);
}

/*
 * [3] gunner_fire_chain — module+0x11D8.
 *
 * Three instructions: install the Fire Chain move. It is the endfunc of the
 * Attack Chain move and the only route into the chain gun.
 */
static void gunner_fire_chain(q2_monster *self)
{
    q2_cre_set_move(self, GUN_FIRE_CHAIN);
}

/*
 * [4] gunner_refire_chain — module+0x11E8, and the endfunc of the Fire Chain.
 *
 * id's `gunner_refire_chain` without a line's difference: keep firing only
 * while the enemy is alive AND visible AND the coin says so, otherwise wind the
 * gun down through the End Chain move. The coin is `rand() < 16385`, which is
 * id's `random() <= 0.5` on a 0..32767 generator.
 *
 * Until this was filled in, `crebind`'s `resolve_endfunc` could not name the
 * address — it is a method-table entry rather than a callback, and index 4 was
 * NULL — so the Fire Chain move had no endfunc and a firing Gunner never
 * stopped firing.
 *
 * THE NULL TEST ON `enemy` IS THIS PORT'S, and it is not in the module. The
 * module opens `lw a1, 188(s0)` and immediately dereferences it — `lw v0,
 * 36(a1)` then `lh v0, 264(v0)` — so a Gunner that reaches this endfunc with no
 * enemy reads through zero. id's `gunner_refire_chain` has no guard either. The
 * guard is here rather than a departure worth taking a fault over, and it fails
 * to the module's own else arm, the End Chain move, which is where an enemy
 * that is dead or out of sight sends it anyway.
 */
static void gunner_refire_chain(q2_monster *self)
{
    if (self->enemy && self->enemy->health > 0
        && q2_visible(self, self->enemy)
        && gun_rand() < GUN_R_0_5) {
        q2_cre_set_move(self, GUN_FIRE_CHAIN);
        return;
    }

    q2_cre_set_move(self, GUN_END_CHAIN);
}

/* [6] gunner_idlesound — module+0x128C. One frame of the Fidget move, 37. */
static void gunner_idlesound(q2_monster *self)
{
    gun_play(self, GUN_SND_IDLE);
}

/*
 * [7] gunner_fidget — module+0x12C4. Three frames of the Stand move, 9, 19
 * and 29.
 *
 * A Gunner told to hold its ground does not fidget; otherwise one time in
 * twenty it breaks into the forty-frame Fidget animation. 1639/32768 is
 * 0.050018 — id's `random() <= 0.05`, with the module's threshold one above
 * the exact 1638.4 so the comparison is `<= 1638`.
 */
static void gunner_fidget(q2_monster *self)
{
    if (self->aiflags & Q2_AI_STAND_GROUND)
        return;

    if (gun_rand() < GUN_R_0_05)
        q2_cre_set_move(self, GUN_FIDGET);
}

/*
 * [9] gunner_dead — module+0x134C, the endfunc of the Death move.
 *
 * id's `gunner_dead`: shrink the hull, MOVETYPE_TOSS, SVF_DEADMONSTER,
 * nextthink 0, relink. The console keeps three of the five.
 *
 *   * `nextthink = 0` (entity+0x90) is written. It has no effect here because
 *     `q2_monster_set_tick` (spawn.c) takes the `dead` branch and `continue`s
 *     before it looks at `next_think` at all — written for faithfulness, not
 *     for behaviour.
 *   * `svflags |= 2` (entity+0x40) is written. 2 is id's SVF_DEADMONSTER, and
 *     monster.h names it now — this used to spell the bit out because the
 *     header named only SVF_MONSTER.
 *   * `movetype = MOVETYPE_TOSS` is bits 18..21 of entity+0x20 going from 5 to
 *     7 (`and 0xFFC3FFFF` / `or 0x001C0000` at module+0x1360..+0x1368), and it
 *     is written: monster.h carries the field. It used to be named and dropped.
 *   * The hull is NOT resized: the module writes nothing to entity+0x6C..+0x76
 *     in this function, where id sets mins and maxs. A console departure.
 *   * There is no relink call either — no import +0x1C anywhere in the body.
 *   * The deadflag is NOT touched here, and that is the module's doing: only
 *     the two arms of `gunner_die` write bits 22..23.
 */
static void gunner_dead(q2_monster *self)
{
    self->next_think = 0;
    self->movetype   = Q2_MOVETYPE_TOSS;
    self->svflags   |= Q2_SVF_DEADMONSTER;
}

/*
 * [10] gunner_duck_down — module+0x137C, the first frame of the Duck move.
 *
 * id's `gunner_duck_down` with its one memorable branch intact: at skill 2 or
 * above, half the time the Gunner throws a grenade WHILE it ducks. The coin is
 * the module's `rand() & 1` where id writes `random() > 0.5`, so it is a coin
 * rather than a weighted roll.
 *
 * `pausetime = level.time + 10` is id's `level.time + 1` on the 10 Hz clock.
 *
 * `takedamage = DAMAGE_YES` — bits 30..31 of entity+0x1C going to 1, at
 * module+0x13F8..+0x140C — IS WRITTEN NOW; it used to be named and dropped for
 * want of a field. The module does it after the grenade branch and before the
 * pausetime, on both arms of the skill test, and that is the order below.
 *
 * Still named rather than written: `maxs[2] -= 32`, which id does and the
 * module does not — there is no store to entity+0x6C..+0x76 in the body, so the
 * console's duck does not shrink the hull.
 */
static void gunner_duck_down(q2_monster *self)
{
    if (self->aiflags & Q2_AI_DUCKED)
        return;

    self->aiflags |= Q2_AI_DUCKED;

    if (q2_cre_skill() >= 2 && (q2_random_next() & 1))
        gunner_grenade(self);

    self->takedamage = Q2_DAMAGE_YES;
    self->pausetime  = q2_level_state.time + Q2_AI_SECONDS(1);
}

/*
 * [11] monster_duck_hold — module+0x143C. The shared helper, byte for byte the
 * same one the Infantry carries, and id's `monster_duck_hold` exactly.
 */
static void gunner_duck_hold(q2_monster *self)
{
    if (q2_level_state.time >= self->pausetime)
        self->aiflags &= ~(u32)Q2_AI_HOLD_FRAME;
    else
        self->aiflags |= Q2_AI_HOLD_FRAME;
}

/*
 * [12] monster_duck_up — module+0x1484.
 *
 * Sets `takedamage = DAMAGE_AIM` — bits 30..31 of entity+0x1C going to 2 at
 * module+0x1488..+0x14A4, which is id's own value for that line and is the
 * cross-check that says the field is read correctly — and then clears
 * AI_DUCKED. The module does them in that order and it is written now; the
 * first pass had no field and named it instead.
 *
 * `maxs[2] += 32` and the relink are absent from the module, mirroring
 * `gunner_duck_down`.
 */
static void gunner_duck_up(q2_monster *self)
{
    self->takedamage = Q2_DAMAGE_AIM;
    self->aiflags   &= ~(u32)Q2_AI_DUCKED;
}

/*
 * [13] — module+0x14B4, which is `jr ra` / `nop`.
 *
 * It sits on the first frame of the Attack Chain move and does nothing. This
 * has to be an explicit no-op rather than a NULL, because `q2_creature_bind`
 * substitutes the generic think for any NULL slot and the generic think would
 * run the decoder's mis-read of this function — see the header.
 */
static void gunner_think_none(q2_monster *self)
{
    (void)self;
}

/* ------------------------------------------------------------------------- */
/* The monsterinfo callbacks                                                  */
/* ------------------------------------------------------------------------- */

/*
 * gunner_stand — module+0x127C, and also method[5]. Three instructions, no
 * branch.
 *
 * ONE move uses it as an endfunc, not three: the census's per-move table gives
 * `(-1) 80101A50 end 8010127C->80101ABC 30-69`, the Fidget move, and no other
 * move's endfunc is this address. (`gunner_run` is the busy one — six moves.)
 * Method[5] is filled because the module registers it at module+0x8E4, not
 * because the endfunc needed it; see the header.
 */
static void gunner_stand(q2_monster *self)
{
    q2_cre_set_move(self, GUN_STAND);
}

/* gunner_search — module+0x1554. One sound and nothing else, id's own. */
static void gunner_search(q2_monster *self)
{
    gun_play(self, GUN_SND_SEARCH);
}

/* gunner_walk — module+0x158C. Three instructions. */
static void gunner_walk(q2_monster *self)
{
    q2_cre_set_move(self, GUN_WALK);
}

/*
 * gunner_run — module+0x131C, and also method[8]: it is the endfunc of the
 * grenade attack, the duck, the End Chain and all three pain moves.
 *
 * id's `gunner_run` to the line — stand ground beats running.
 */
static void gunner_run(q2_monster *self)
{
    if (self->aiflags & Q2_AI_STAND_GROUND)
        q2_cre_set_move(self, GUN_STAND);
    else
        q2_cre_set_move(self, GUN_RUN);
}

/*
 * gunner_dodge — module+0x17B4.
 *
 * id's `gunner_dodge`: three times in four it ignores the incoming shot, and
 * otherwise it adopts the shooter as an enemy if it had none and ducks.
 * `rand() < 8193` is `random() > 0.25` on a 0..32767 generator, 8192 being
 * exactly a quarter of 32768.
 *
 * `eta` is read into no register at all — the module ignores its third
 * argument, where id's soldier uses it to set `pausetime`. The duck's own
 * pausetime is set a frame later, by `gunner_duck_down`.
 *
 * NOTHING CALLS THIS. `crebind.c` installs slot 5 and explains at length why it
 * then leaves it unreachable: sweeping the whole executable for a load off
 * entity+0xF4 finds none, so this build has no `check_dodge` call site. The
 * handler is here because the module writes it, and it is inert because the
 * console is.
 */
static void gunner_dodge(q2_monster *self, q2_monster *other, s32 eta)
{
    (void)eta;

    if (gun_rand() >= GUN_R_0_25)
        return;

    if (!self->enemy)
        self->enemy = other;

    q2_cre_set_move(self, GUN_DUCK);
}

/*
 * gunner_attack — module+0x1814.
 *
 * `import +0xB4` is `0x8005EF84`, and it is `q2_range` — which this port
 * already carries. The identification is not by shape alone: it subtracts the
 * two origins, takes `q2_vector_length_sq` (`0x8005C59C`, already named in
 * ai.h), and compares against 0x003F803F or 0x000FE00F depending on whether the
 * entity's class byte at +0x23 is 68 — which is 2040^2 - 1 and 1020^2 - 1, the
 * long-melee and ordinary melee bands, with the same off-by-one the port's own
 * `q2_range` reproduces. So the branch is `range == Q2_RANGE_MELEE`.
 *
 * That is id's `gunner_attack`: close in, the chain gun; otherwise a coin
 * between the grenade volley and the chain gun. The coin is `rand() & 1`,
 * which is the low bit and not a weighted table, where id writes
 * `random() <= 0.5`.
 *
 * WHICH MOVE IS WHICH was the one thing the earlier pass got backwards in its
 * naming. 108-128 was called the burst; it is the GRENADE volley — its four
 * think 1s at frames 112, 115, 118 and 121 each reach `monster_fire_grenade`.
 * 137-143 was called the snap; it is the chain gun's WIND-UP, and it holds no
 * fire think at all. The bullets are two endfuncs further on, in 144-151.
 *
 * `q2_range(self, NULL)` returns Q2_RANGE_FAR, so an enemyless Gunner takes the
 * coin, which is what the module does. The earlier pass diverted that case to
 * the generic handler; the guard is gone.
 */
static void gunner_attack(q2_monster *self)
{
    if (q2_range(self, self->enemy) == Q2_RANGE_MELEE) {
        q2_cre_set_move(self, GUN_ATTACK_CHAIN);
        return;
    }

    q2_cre_set_move(self, (q2_random_next() & 1) ? GUN_ATTACK_GRENADE
                                       : GUN_ATTACK_CHAIN);
}

/* gunner_sight — module+0x151C. One sound, and `other` is discarded by the
 * module's own first instruction. id's `gunner_sight` does the same. */
static void gunner_sight(q2_monster *self, q2_monster *other)
{
    (void)other;
    gun_play(self, GUN_SND_SIGHT);
}

/*
 * gunner_pain — module+0x15AC.
 *
 * id's `gunner_pain` line for line, including all three of its branches:
 * bloodied below half health, a three-second debounce, no flinch at all on
 * nightmare, and then a THREE-WAY SPLIT BY DAMAGE — `damage <= 10` gives the
 * five-frame Pain3, `damage <= 25` the eight-frame Pain2, and anything heavier
 * the eighteen-frame Pain1. The module's immediates are `slti 11` and
 * `slti 26`, which are those two comparisons compiled.
 *
 * THE DAMAGE ARRIVES NOW. The module's handler takes id's four arguments and
 * keeps the fourth in s2 (`addu s2, a3, zero` in the delay slot at
 * module+0x15EC), then tests `slti v0, s2, 11` at module+0x1658 and
 * `slti v0, s2, 26` at module+0x166C — a `<=` compiles as one above the source
 * constant, so those are id's `damage <= 10` and `damage <= 25`. monster.h's
 * callback is `void (*)(q2_monster *, s16)` and `q2_monster_damage_reaction`
 * passes T_Damage's own amount, so the split below runs on the real figure.
 *
 * This used to be two functions: a selector taking a `s32 damage` and a
 * one-line callback that called it with a stand-in of 0 — a number that is not
 * in the disassembly, chosen only because it lands on the lightest arm. Both
 * the stand-in and the wrapper are gone.
 */
static void gunner_pain(q2_monster *self, s16 damage)
{
    /*
     * A corpse does not flinch. The module has no such guard because the class
     * routes damage to `die` or to `pain` and never to both; this port's
     * reaction path can reach `pain` on a body that is already dying, which is
     * the same case cre_soldier.c documents.
     */
    if (self->dead)
        return;

    /*
     * `skinnum = 1` at entity+0x3A — the wounded half of the pair. The module
     * ASSIGNS 1 (`addiu v0, zero, 1` / `sh v0, 58(s0)` at module+0x15F0), it
     * does not OR the low bit the way monster.h describes the convention. That
     * is the same thing only if the Gunner's base skin is 0, which NOTHING read
     * here establishes — the spawn function writes no skinnum at all, and the
     * class table's row 10 carries none. `skinnum` is a field now, so the
     * ASSIGNMENT is reproduced as an assignment; `m->hurt` is written beside it
     * because it is the flag the rest of this port reads, and on a base skin of
     * 0 the two say the same thing.
     */
    if (self->health < self->max_health / 2) {
        self->skinnum = 1;
        self->hurt    = true;
    }

    /* pain_debounce_time at entity+0xA8; 30 ticks is id's three seconds. */
    if (q2_level_state.time < self->pain_debounce)
        return;

    self->pain_debounce = q2_level_state.time + Q2_AI_SECONDS(3);

    gun_play(self, GUN_SND_PAIN);

    /* No pain anims in nightmare — the module reads cvar[0]->value through
     * import +0x4C and compares against 3, which is id's own line. */
    if (q2_cre_skill() == 3)
        return;

    if (damage <= 10)
        q2_cre_set_move(self, GUN_PAIN3);
    else if (damage <= 25)
        q2_cre_set_move(self, GUN_PAIN2);
    else
        q2_cre_set_move(self, GUN_PAIN1);
}

/*
 * gunner_die — module+0x16A4.
 *
 * id's `gunner_die` with both arms in id's order:
 *
 *     if (health <= gib_health) { udeath; deadflag = DEAD_DEAD; return; }
 *     if (deadflag == DEAD_DEAD) return;
 *     death sound; deadflag = DEAD_DEAD; takedamage = DAMAGE_YES;
 *     currentmove = death
 *
 * The gib test is `slt gib_health, health` inverted, so it fires on
 * `health <= gib_health` — and the class table gives this creature
 * gib_health -70, which is id's gunner exactly.
 *
 * The gib arm is tested BEFORE the already-dead guard, so a body can still be
 * blown apart by a later explosion. It spawns no gibs: where id calls ThrowGib
 * six times and ThrowHead once, the console plays `msc_udeath`, zeroes
 * nextthink, sets movetype to MOVETYPE_TOSS, raises SVF_DEADMONSTER and sets
 * deadflag = DEAD_DEAD. Three of those writes are exactly the ones
 * `gunner_dead` makes, done immediately instead of at the end of a death
 * animation; the sound and the deadflag are this arm's own. `gunner_dead`
 * neither plays a sound nor touches the deadflag.
 *
 * ALL THREE PACKED FIELDS ARE WRITTEN NOW, where the first pass named them and
 * dropped them for want of anywhere to put them. The gib arm's
 * `and 0xFF3FFFFF` / `or 0x00800000` at module+0x1704..+0x170C is
 * `deadflag = DEAD_DEAD` and its `and 0xFFC3FFFF` / `or 0x001C0000` at
 * module+0x1710..+0x171C is `movetype = MOVETYPE_TOSS`; the normal arm repeats
 * the deadflag write at module+0x1784..+0x178C and adds
 * `and 0x3FFFFFFF` / `or 0x40000000` on entity+0x1C at module+0x1790..+0x1798,
 * which is `takedamage = DAMAGE_YES`.
 *
 * THE NORMAL ARM'S ORDER IS THE MODULE'S, NOT ID'S. Its two stores land at
 * module+0x179C and +0x17A0, after `currentmove` at module+0x177C, where the
 * quote above has id writing both before it. The code below follows the
 * module; nothing reads either field between the three writes, so the order is
 * a matter of faithfulness rather than of behaviour.
 *
 * The already-dead guard stays on `self->dead` rather than on `deadflag`. The
 * module tests bits 22..23 against 2 (`srl 22` / `andi 3` at module+0x1738),
 * and this port's `dead` is the same guard kept in step by
 * `q2_monster_damage_reaction` — testing one and writing the other is how the
 * two would come to disagree.
 *
 * The damage argument is read by neither arm: the module keeps nothing out of
 * a3 anywhere in the body, where `gunner_pain` keeps it in s2 straight away.
 * The parameter is present because monster.h's `die` carries it.
 */
static void gunner_die(q2_monster *self, s16 damage)
{
    (void)damage;

    if (self->health <= self->gib_health) {
        /*
         * THE SOUND IS GUARDED HERE AND IS NOT IN THE MODULE. The gib arm has
         * no already-gibbed test of any kind — a body hit again past
         * gib_health replays `msc_udeath` on the console, which is id's
         * behaviour too. The guard is this port's, kept for the same reason
         * cre_soldier.c keeps it, and it is the only difference between the
         * two arms below and the disassembly.
         */
        if (!self->gibbed) {
            self->gibbed = true;
            gun_play(self, GUN_SND_UDEATH);
        }
        /* The console's own substitute for the gib shower: no gibs at all,
         * just the three writes gunner_dead also makes plus DEAD_DEAD. */
        self->dead       = true;
        self->deadflag   = Q2_DEAD_DEAD;
        self->next_think = 0;
        self->movetype   = Q2_MOVETYPE_TOSS;
        self->svflags   |= Q2_SVF_DEADMONSTER;
        return;
    }

    if (self->dead)
        return;

    self->dead = true;
    gun_play(self, GUN_SND_DEATH);
    q2_cre_set_move(self, GUN_DEATH);
    self->deadflag   = Q2_DEAD_DEAD;
    self->takedamage = Q2_DAMAGE_YES;
}

/* ------------------------------------------------------------------------- */
/*
 * The spawn function, and it needs a hook after all.
 *
 * module+0xB78 writes, in order: mass 200 (entity+0x4E); then NINE callbacks in
 * the module's own order — pain (+0xA0), die (+0xA4), stand (+0xE0), walk
 * (+0xEC), run (+0xF0), dodge (+0xF4), attack (+0xF8), search (+0xE8), sight
 * (+0x100) — pain and die FIRST, which is the same order the Soldier's spawn
 * uses; then an explicit zero to `melee` (entity+0xFC) at module+0xC40; the
 * solid and movetype bits at entity+0x20; `currentmove` = Stand; animation
 * scale 12 (entity+0x13B); `walkmonster_start` (import +0xFC); and seven sound
 * registrations. The two `link_entity` calls do NOT bracket the callback block
 * — both follow it, back to back at module+0xC64 (flags 1) and module+0xC80
 * (flags 129), after the solid/movetype write and before `currentmove`.
 *
 * The scale comes across in the decode and `q2_creature_spawn` applies it. The
 * initial move does not need setting here because `q2_monster_start_go` calls
 * `m->stand(m)`, which reaches the same move by the same route the console
 * does.
 *
 * WHAT IS LEFT IS THE THREE FIELDS, and this used to end "Mass, solid and
 * movetype have no field in `q2_monster`. So: NULL." All three exist now, and
 * `q2_creature_spawn` runs `impl->spawn` last — after the class byte, the scale
 * and the callbacks, and after `creworld.c` has applied the class row's health
 * — which is the loader's own order at 0x8007E68C/0x8007E698/0x8007E6AC. So the
 * hook writes exactly what export 0 writes and nothing more:
 *
 *     mass     200            `addiu v0, zero, 200` / `sh v0, 78(s0)`
 *                             at module+0xB98 and +0xBC0
 *     solid    SOLID_BBOX     `and 0xFFFCFFFF` / `or 0x00020000` on
 *                             entity+0x20, module+0xC4C..+0xC54
 *     movetype MOVETYPE_STEP  `and 0xFFC3FFFF` / `or 0x00140000` on the same
 *                             word, module+0xC44..+0xC48
 *
 * The module masks movetype in first and solid second; the order does not
 * matter once they are separate members, and the values are id's SOLID_BBOX 2
 * and MOVETYPE_STEP 5. Export 0 writes no health, no skinnum and no takedamage,
 * so none is written here — the class table's row 10 health 149 that
 * `creworld.c` has already applied stands, which is the console's behaviour: a
 * module free to overwrite it does not.
 */
static void gunner_spawn(q2_monster *m)
{
    m->mass     = 200;
    m->solid    = Q2_SOLID_BBOX;
    m->movetype = Q2_MOVETYPE_STEP;
}

const q2_cre_impl q2_cre_gunner = {
    "Gunner",
    {
        gunner_stand,       /*  0 stand       */
        NULL,               /*  1 idle        — the module installs none, and
                             *                  neither does id's gunner: the
                             *                  idle sound is a frame think    */
        gunner_search,      /*  2 search      */
        gunner_walk,        /*  3 walk        */
        gunner_run,         /*  4 run         */
        /* 5 dodge — installed, and nothing calls it. See gunner_dodge. */
        (q2_class_method)(void *)gunner_dodge,
        gunner_attack,      /*  6 attack      */
        NULL,               /*  7 melee       — the spawn function writes an
                             *                  explicit zero to entity+0xFC at
                             *                  module+0xC40; a gunner has none */
        (q2_class_method)(void *)gunner_sight,   /* 8 sight */
        NULL,               /*  9 checkattack — the module installs none, so
                             *                  monster_start leaves the
                             *                  engine's M_CheckAttack in place */
        NULL,               /* 10 bigturn     — the module installs none       */
        /*
         * 11 and 12 — pain and die — take the damage now, so both need the
         * same cast slot 5 and slot 8 already use: `q2_class_method` is
         * `void (*)(q2_monster *)` and `crebind.c` casts each back to the
         * signature monster.h declares for that slot.
         */
        (q2_class_method)(void *)gunner_pain,
        (q2_class_method)(void *)gunner_die
    },
    {
        NULL,                   /*  0 — the module writes an explicit zero here
                                 *      at module+0x8A8                        */
        gunner_grenade,         /*  1 */
        gunner_fire,            /*  2 */
        gunner_fire_chain,      /*  3 endfunc only */
        gunner_refire_chain,    /*  4 endfunc only */
        gunner_stand,           /*  5 endfunc only */
        gunner_idlesound,       /*  6 */
        gunner_fidget,          /*  7 */
        gunner_run,             /*  8 endfunc only */
        gunner_dead,            /*  9 endfunc only */
        gunner_duck_down,       /* 10 */
        gunner_duck_hold,       /* 11 */
        gunner_duck_up,         /* 12 */
        gunner_think_none,      /* 13 — the module's own `jr ra`; see header  */
        /* 14..31 — the module registers nothing here and no frame uses them. */
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
    },
    gunner_spawn
};
