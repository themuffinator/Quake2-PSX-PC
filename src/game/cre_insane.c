/*
 * cre_insane.c — the Insane, transcribed from its own module.
 *
 * `LAB CreAIBin` module 'Insane', 6224 bytes relocated to 0x80100000. Export 0
 * (the spawn function) is 0x8010084C, export 2 is the precache, export 3 is the
 * named-move block. Every address below is inside that image.
 *
 * It ships on one map and it never attacks anything: it has no attack callback,
 * no melee, no sight, no dodge and no checkattack. What it has instead is a
 * standing set, a walking set, a crawling set and a crucified pose, and the
 * whole module is about which of the four a given placement gets and how it
 * moves between them. That is PC Quake II's `misc_insane` — set dressing that
 * screams — and the read lands on id's own source repeatedly.
 *
 * ---------------------------------------------------------------------------
 * It is id's misc_insane, and here is the evidence
 * ---------------------------------------------------------------------------
 * These were not tuned to produce a match; they are what the disassembly says.
 *
 *   0x80100894  health     = 100      id's `self->health = 100`
 *   0x8010089C  gib_health = -50      id's `self->gib_health = -50`
 *   0x801008A4  mass       = 300      id's `self->mass = 300`
 *   0x80100904  aiflags   |= 0x100    id's `aiflags |= AI_GOOD_GUY`
 *   0x801008F4  dodge/attack/melee/sight written as literal ZERO, in that
 *               order — id's four explicit NULLs, in id's order
 *   0x801009A4  the else-arm is `walkmonster_start(self); skinnum = rand() % 3`
 *               and the if-arm is `flags |= FL_NO_KNOCKBACK; flymonster_start`,
 *               which is id's crucified/not-crucified split verbatim, down to
 *               the `rand() % 3` (a 0x55555556 magic-multiply here)
 *   0x80100AD8  insane_dead: `if (flag) flags |= FL_FLY; else movetype =
 *               MOVETYPE_TOSS;` then `svflags |= SVF_DEADMONSTER;
 *               nextthink = 0` — id's insane_dead line for line
 *   0x80100778  `if (deadflag == DEAD_DEAD) return;` at the head of the death
 *   0x801007A8  `takedamage = DAMAGE_YES` on death
 *   0x80100630  `if (skill->value == 3) return;` — id's "no pain anims in
 *               nightmare", read straight off the cvar table (import +0x4C)
 *   0x80100608  pain_debounce_time = level.time + 30 — id's `+ 3` seconds on
 *               the 10 Hz clock
 *   0x80100D38  `random() < 0.3` as 9830/32768 in insane_checkdown
 *   0x80100A74  `random() < 0.5`  as `slti 16384` in insane_stand, and
 *   0x80100BEC  `random() <= 0.5` as `slti 16385` in insane_walk and
 *               insane_run. Both instructions are strictly less-than; the
 *               `<=` is in the constant, 16385 being `r <= 16384`. The module
 *               keeps id's `<` / `<=` distinction between those two functions,
 *               which is a stronger tell than either constant.
 *
 * The move set is id's too, once the PSX's own frame numbering is applied:
 * uptodown, downtoup, stand_normal, stand_insane, jumpdown, down, walk_normal,
 * walk_insane, stand_pain, stand_death, crawl, runcrawl, crawl_pain,
 * crawl_death and the cross. Two departures in the animation data:
 *
 *   - id's single `insane_move_down` is split into THREE moves here, "Down 1"
 *     (99-108), "Down 2" (109-147) and "Down 3" (148-158), chained by endfuncs.
 *     Only Down 2 and Down 3 count as "down" in the module's own tests; Down 1
 *     does not. The three span 99..158, which is SIXTY frames; no id frame
 *     count is claimed for the move they replace, because the disc cannot be
 *     asked for one and the split makes a count comparison meaningless anyway.
 *   - id's cross and struggle_cross — two moves there, and the frame counts
 *     are id's side of the comparison rather than anything this disc says —
 *     are one SINGLE-FRAME move here, "On Back" at 282. What IS read off the
 *     disc is the record: module+0x139C is {282, 282, +0x1398, insane_dead},
 *     first frame equal to last. A crucified Insane holds one
 *     pose, and because a one-frame move reaches its end callback every tick,
 *     that pose re-runs `insane_dead` forever — which is how the module makes
 *     it inert scenery rather than by id's AI_HOLD_FRAME.
 *
 * ---------------------------------------------------------------------------
 * Why stand, walk and run are ONE address
 * ---------------------------------------------------------------------------
 * They are not. The spawn function writes them twice.
 *
 *   0x801008CC  stand = 0x80100A3C      insane_stand
 *   0x801008D8  walk  = 0x80100B30      insane_walk
 *   0x801008E4  run   = 0x80100C1C      insane_run
 *
 * and then, only when the crucified bit is set (0x801009FC):
 *
 *   0x80100A08  stand = 0x801010D4      insane_cross
 *   0x80100A0C  walk  = 0x801010D4
 *   0x80100A10  run   = 0x801010D4
 *   0x80100A1C  currentmove = "On Back", frame = 282
 *
 * A crucified Insane has no stand, no walk and no run: all three collapse into
 * "hold the On Back pose". The census reports one address for the three because
 * `decode_spawn` (creature.c) keeps the LAST store to each slot, so what it
 * prints is the crucified arm. Both arms are reproduced here — the primary
 * three in the callback table, and the override in the spawn hook.
 *
 * ---------------------------------------------------------------------------
 * The spawnflags, and the fact that the module reads them from two places
 * ---------------------------------------------------------------------------
 * Four bits are tested. In the entity they are `spawnflags >> 18`; in the link
 * object they are the halfword at +0xF2. Those are ONE value: the entity filler
 * at 0x8007E618..0x8007E62C does
 *
 *     spawnflags = (spawnflags & 0xF803FFFF) | ((object[0xF2] & 0x1FF) << 18)
 *
 * so bit n of object+0xF2 is map spawnflag (1 << n), and this file uses the
 * single accessor `insane_flags` for both.
 *
 *   0x08  the PRONE variant. insane_stand goes to the floor instead of
 *         standing, insane_walk/insane_run play the crawl, insane_dead sets
 *         FL_FLY rather than MOVETYPE_TOSS so the body does not drop, and the
 *         spawn gives it FL_NO_KNOCKBACK and flymonster_start instead of
 *         walkmonster_start. id has this behaviour split across two flags,
 *         CRAWL (4) and CRUCIFIED (8); the module carries one, tested with
 *         id's CRUCIFIED literal but selecting id's CRAWL animations.
 *   0x10  aiflags |= AI_STAND_GROUND. id's own bit for STAND_GROUND, same
 *         value, same effect.
 *   0x40  the CROSS. Takes the three callbacks above, and insane_pain refuses
 *         to flinch — which sits exactly where id's "Don't go into pain frames
 *         if crucified" test sits, on id's flag 8. So the cross moved from
 *         id's 8 to this module's 0x40.
 *   0x80  VOCAL. Raises the chance of a moan from 39% to 51% and unlocks the
 *         second sound; see insane_moan.
 *
 * ---------------------------------------------------------------------------
 * Three id enums packed into one word, which is what made the spawn readable
 * ---------------------------------------------------------------------------
 * The 32-bit word at entity+0x20 is not just `flags`. Its top half is three
 * bitfields, and each one lands on an id enum:
 *
 *     bits  0..15   FL_*                      (monster.h already has these)
 *     bits 16..17   solid      = 2            SOLID_BBOX
 *     bits 18..21   movetype   = 5            MOVETYPE_STEP  (7 = TOSS on death)
 *     bits 22..23   deadflag   = 2            DEAD_DEAD
 *
 * and entity+0x1C carries one more beside the spawnflags:
 *
 *     bits 30..31   takedamage = 1            DAMAGE_YES
 *
 * Four independent id enums with their exact values in their exact widths.
 * `monster.h` now carries all four as fields, with that same layout written
 * down there and checked against the other six modules, so this file no longer
 * reads them and throws them away. Every one is written where the module
 * writes it: solid, movetype and mass at spawn, movetype again in insane_dead,
 * deadflag and takedamage in insane_die, skinnum in the upright arm of the
 * spawn.
 *
 * ---------------------------------------------------------------------------
 * The sounds — two, and the module wants three
 * ---------------------------------------------------------------------------
 * The precache (export 2) resolves three names and stores the handles:
 *
 *     module+0x17B8  insane2
 *     module+0x17BC  insane9
 *     module+0x17C0  msc_udeath
 *
 * then at 0x80100540 falls the first back onto the second if it did not
 * resolve, and gives both a pan of 127 through import +0x44. `msc_udeath` is
 * written by the precache and READ BY NOTHING — a sweep of the whole relocated
 * image finds one store to +0x17C0, at 0x8010054C, and no load anywhere. The
 * Insane never gibs audibly: its gib arm plays nothing at all.
 *
 * The handles are passed to the host as MODULE ADDRESSES, which is what
 * `cre_actions.c` does for a decoded creature and what the client resolves
 * first (`q2_creature_world_sound_for_addr`), so no name table is needed here.
 *
 * ---------------------------------------------------------------------------
 * What is still owed
 * ---------------------------------------------------------------------------
 * `q2_cre_impl.spawn` is CALLED now, which for this creature is the whole
 * game: everything an Insane IS gets decided in export 0. Its map flags now
 * arrive before it is called: spawn.c transcribes the engine filler at
 * 0x8007E61C..0x8007E62C, preserving the surrounding entity bits and copying
 * Population.spawn.flags low nine bits into spawnflags bits 18..26. The prone,
 * stand-ground, cross and vocal variants are therefore live rather than all
 * silently selecting the upright default.
 *
 *   - `self->target` (entity+0x16) now receives Population.spawn.link as its
 *     literal signed halfword. What a non-zero value MEANS for this creature
 *     is not established — see insane_walk — but its observed 0xFFFF sentinel
 *     stays -1 rather than being silently rewritten to zero.
 *   - link_entity and the walkmonster_start / flymonster_start pair have no
 *     port equivalent; both are named at the spawn hook.
 *
 * And three things in the image that are owed nothing, listed so a later
 * reader does not go looking for their callers: 0x80101104 (the prone
 * predicate), 0x80101120 (the five-record "am I down" test) and 0x80101174 (a
 * fifth copy of the moan) are complete functions that NOTHING in the relocated
 * module references. They are the out-of-line copies of routines the compiler
 * inlined at every use, and they are useful as corroboration rather than as
 * code — see insane_flags, insane_is_down and insane_moan.
 */
#include <stdlib.h>

#include "ai.h"
#include "crebind.h"
#include "monster.h"

/* ------------------------------------------------------------------------- */
/* The move set, by first frame                                               */
/* ------------------------------------------------------------------------- */
/*
 * The module address is given for each because two pairs of moves share a
 * frame range and can only be told apart by it. See the note on INS_CRAWL.
 */
#define INS_UP_DOWN     0    /*   0..39   "Up Down"   module+0x148C  uptodown  */
#define INS_DOWN_UP    40    /*  40..58   "Down Up"   module+0x14D8  downtoup  */
#define INS_STAND_N    59    /*  59..64   "Stand N"   module+0x1388            */
#define INS_STAND_I    64    /*  64..92   "Stand I"   module+0x1404            */
#define INS_JUMP_DOWN  94    /*  94..98   "Jump Down" module+0x14F8  jumpdown  */
#define INS_DOWN_1     99    /*  99..108  "Down 1"    module+0x1528            */
#define INS_DOWN_2    109    /* 109..147  "Down 2"    module+0x15B0            */
#define INS_DOWN_3    148    /* 148..158  "Down 3"    module+0x15E4            */
/*
 * TWO RECORDS EACH, and a frame number cannot tell them apart.
 *
 * The module ships walk and run variants of Walk 1, Walk 2 and Crawl as
 * separate move records over the SAME frame range. Read as the four words of a
 * `q2_mmove` — {first, last, frames, endfunc} — the two WALK pairs differ in
 * exactly one word:
 *
 *     module+0x161C  {160, 172, +0x15F4, insane_walk}
 *     module+0x162C  {160, 172, +0x15F4, insane_run}
 *
 * one frame array between them. That is id exactly: `insane_move_walk_normal`
 * and `insane_move_run_normal` share one frame array and differ only in
 * endfunc.
 *
 * THE CRAWL PAIR DOES NOT DIFFER AT ALL:
 *
 *     module+0x1740  {227, 235, +0x1724, NULL}
 *     module+0x1750  {227, 235, +0x1724, NULL}
 *
 * word for word the same, both with no end callback. The "run crawl" is a
 * duplicate record, not a variant, so whichever of the two is installed the
 * animation and the (absent) endfunc are identical.
 *
 * `q2_cre_find_move` keys on the first frame and returns the FIRST record with
 * it, which reaches only the walk half of each pair. That used to be a stated
 * departure here: `insane_run` installed the walk records, and the two walks
 * then ended in `insane_walk` instead of `insane_run`. It is not a departure
 * any more. `q2_cre_set_move_at` keys on the record's own module address, all
 * six records are decoded (the census lists nineteen moves for the Insane and
 * clips none of them), and each function now installs the one the module
 * names. The six addresses are given names below rather than written inline,
 * because a bare 0x8010162C in a callback says nothing on its own.
 */
#define INS_WALK_1    160    /* 160..172  module+0x161C walk / +0x162C run     */
#define INS_WALK_2    173    /* 173..198  module+0x168C walk / +0x169C run     */
#define INS_ST_PAIN   199    /* 199..209  "St Pain"   module+0x16D0            */
#define INS_ST_DEATH  210    /* 210..226  "St Death"  module+0x1714            */
#define INS_CRAWL     227    /* 227..235  module+0x1740 walk / +0x1750 run     */
#define INS_CR_PAIN   236    /* 236..244  "Cr Pain"   module+0x177C            */
#define INS_CR_DEATH  245    /* 245..251  "Cr Death"  module+0x17A4            */
#define INS_ON_BACK   282    /* 282..282  "On Back"   module+0x139C            */

/*
 * The three ambiguous pairs, by module address. Each line names the
 * instruction that FORMS the address — the `addiu` in a branch delay slot,
 * which is why the six sit at odd-looking offsets — and every one is then
 * stored into currentmove by its function's single `sw v0, 0xD8(entity)`. Read
 * off the disassembly, not picked out of the census listing.
 */
#define INS_MV_WALK_1_WALK  0x8010161Cu  /* 0x80100BFC, insane_walk's arm      */
#define INS_MV_WALK_1_RUN   0x8010162Cu  /* 0x80100CE8, insane_run's           */
#define INS_MV_WALK_2_WALK  0x8010168Cu  /* 0x80100C04                         */
#define INS_MV_WALK_2_RUN   0x8010169Cu  /* 0x80100CF0                         */
#define INS_MV_CRAWL_WALK   0x80101740u  /* 0x80100B60                         */
#define INS_MV_CRAWL_RUN    0x80101750u  /* 0x80100C4C, identical record       */

/* ------------------------------------------------------------------------- */
/* Spawnflags — `spawnflags >> 18`, which is object+0xF2 (see the header)      */
/* ------------------------------------------------------------------------- */
#define INS_SF_CRAWL         0x008u  /* prone: crawl set, no knockback, flies  */
#define INS_SF_STAND_GROUND  0x010u  /* aiflags |= AI_STAND_GROUND             */
#define INS_SF_CRUCIFIED     0x040u  /* stand/walk/run all become insane_cross */
#define INS_SF_VOCAL         0x080u  /* moans more often, and with two sounds  */

static u32 insane_flags(const q2_monster *m)
{
    /*
     * Bits 18..26 of entity+0x1C. The module reads the same nine bits off the
     * link object at +0xF2 in insane_walk, insane_run and the moan; the entity
     * filler at 0x8007E618..0x8007E62C proves the two are one value —
     * `lhu v0, 0xF2(object)` / `andi 0x1FF` / `sll 18` / `or` into
     * spawnflags & 0xF803FFFF.
     *
     * The prone half of it is also in the image out of line, at 0x80101104:
     * `lhu object+0xF2; andi 8; sltu v0, zero, v0` — the bool this file spells
     * `insane_flags(m) & INS_SF_CRAWL`. Like 0x80101120 it is referenced by
     * nothing in the module, so it is a spare copy rather than a caller.
     */
    return (m->spawnflags >> 18) & 0x1FFu;
}

/* ------------------------------------------------------------------------- */
/* Sounds — passed to the host as the module address of the handle             */
/* ------------------------------------------------------------------------- */
#define INS_SND_INSANE2  0x801017B8u   /* insane2                             */
#define INS_SND_INSANE9  0x801017BCu   /* insane9                             */
/* module+0x17C0 is msc_udeath: resolved by the precache, loaded by nothing. */

extern void (*q2_cre_sound_fn)(q2_monster *m, int which, void *user);
extern void  *q2_cre_sound_user;

static void ins_play(q2_monster *m, u32 handle_addr)
{
    /* The module calls import +0x20, play_sound(handle, entity+0x24). The port
     * routes it through the shared hook, which the client resolves by address
     * exactly as it does for a decoded creature. */
    if (q2_cre_sound_fn)
        q2_cre_sound_fn(m, (int)handle_addr, q2_cre_sound_user);
}

/* ------------------------------------------------------------------------- */
/* random(), in the module's own fixed point                                  */
/* ------------------------------------------------------------------------- */
static s32 ins_rand(void)
{
    /* Import +0x14, 0..32767. */
    return (s32)(rand() & 0x7FFF);
}

/* The module's literal thresholds, kept as the integers it uses. */
#define INS_R_LT_HALF  16384   /* `random() < 0.5`   — insane_stand, checkdown */
/*
 * BOTH OF THESE ARE USED WITH `<`, and the second one is the reason to say so.
 *
 * Every threshold in this module arrives as a `slti rD, rS, imm`, which is
 * STRICTLY less-than. `slti 16384` is `r < 16384` — id's `random() < 0.5` on a
 * 0..32767 draw. `slti 16385` is `r < 16385`, which is `r <= 16384` and so id's
 * `random() <= 0.5`. The `<=` lives in the CONSTANT, not in the operator: a C
 * test written `r <= 16385` would take one draw the module rejects.
 */
#define INS_R_LE_HALF  16385   /* insane_walk 0x80100BEC, insane_run 0x80100CD8 */
#define INS_R_0_30      9830   /* `random() < 0.30`  — checkdown, Stand I      */
#define INS_R_0_36     11796   /* `random() < 0.36`  — checkdown, Stand N      */

/*
 * The moan's two gates, applied to the LOW FOURTEEN BITS of one draw rather
 * than to the whole of it (`andi 0x3FFF` at 0x80100E68 and 0x80100E8C). So the
 * comparison is against a 0..16383 value: 8001 leaves a 51.2% chance of a
 * sound and 10001 leaves 39.0%.
 */
#define INS_MOAN_GATE_VOCAL   8001
#define INS_MOAN_GATE_PLAIN  10001

/* ------------------------------------------------------------------------- */
/*
 * "Am I on the floor?" — the five-record test the module runs at FOUR sites.
 *
 * At 0x80100664 (pain), 0x801007E0 (die), 0x80100B74 (walk) and 0x80100C60
 * (run) the module compares `currentmove` against exactly five records:
 * Down 2, Down 3, Crawl, RunCrawl and Cr Pain. That is id's four —
 * insane_move_down, crawl, runcrawl, crawl_pain — with `down` split in two.
 *
 * Down 1 (99-108) is deliberately NOT in the set, in any of the four sites.
 *
 * That the four sites really are ONE predicate is not an inference: the module
 * also carries the routine OUT OF LINE at 0x80101120, five `beq`s against the
 * same five addresses returning 1, otherwise 0. Nothing in the relocated image
 * references it — a sweep of all 6224 bytes finds no code or data word holding
 * 0x80101120 — so it is the compiler's unused copy of a function it inlined
 * everywhere, and it is the module stating the predicate in its own words.
 *
 * Four comparisons cover the five records here because Crawl and RunCrawl
 * share a first frame, which is the same collision described at INS_CRAWL.
 */
static bool insane_is_down(const q2_monster *m)
{
    s32 f;

    if (!m->currentmove)
        return false;

    f = m->currentmove->first_frame;
    return f == INS_DOWN_2 || f == INS_DOWN_3
        || f == INS_CRAWL  || f == INS_CR_PAIN;
}

/* ------------------------------------------------------------------------- */
/* The monsterinfo callbacks                                                  */
/* ------------------------------------------------------------------------- */

/*
 * insane_stand — module+0xA3C. Callback 0, and method 1.
 *
 * The module folded id's `insane_stand` and `insane_checkdown` into one
 * function under the prone bit: a prone Insane told to stand gets DOWN on the
 * floor — id's own 50/50 between uptodown and jumpdown — and an upright one
 * picks between its two standing animations, which is id's `random() < 0.5`.
 *
 * It is the end callback of Down Up, Stand N and Stand I (module+0x14D8,
 * +0x1388, +0x1404), which is why it is also method 1.
 */
static void insane_stand(q2_monster *self)
{
    if (insane_flags(self) & INS_SF_CRAWL) {
        /* 0x80100A74: rand() < 16384 takes uptodown, otherwise jumpdown. */
        q2_cre_set_move(self, ins_rand() < INS_R_LT_HALF ? INS_UP_DOWN
                                                         : INS_JUMP_DOWN);
        return;
    }

    /* 0x80100AA8, the same coin between the normal and the insane stand. */
    q2_cre_set_move(self, ins_rand() < INS_R_LT_HALF ? INS_STAND_N
                                                     : INS_STAND_I);
}

/*
 * insane_walk — module+0xB30. Callback 3, and method 4 (Walk 1 and Walk 2 end
 * in it).
 *
 * Three arms, in the module's order:
 *
 *   1. prone -> the Crawl move and nothing else. id's `if (spawnflags & 4)
 *      currentmove = &insane_move_crawl`.
 *   2. `self->target != 0` -> do not wander. If it is already on the floor it
 *      goes to Down 3, whose end callback (module+0x10E4) installs Down Up and
 *      so gets it back on its feet; otherwise it falls through to the stand.
 *      This is where id tests `aiflags & AI_STAND_GROUND` instead. The module
 *      reads a SIGNED HALFWORD at entity+0x16 (`lh v0, 22(s0)`), which the
 *      entity filler takes from the placement record's `link` field
 *      (0x8007E608/0x8007E614) and which `monster.h` names `target`. What a
 *      non-zero link means for a misc_insane is NOT established, and the
 *      record's documented "no link" sentinel is 0xFFFF rather than 0 — which
 *      arrives as signed -1 and does take this arm. That is the retail
 *      transport; `monster_start_go` separately treats only positive values as
 *      path targets, so the two readers keep their own predicates.
 *   3. otherwise the coin between the two walk cycles, `random() <= 0.5`.
 */
static void insane_walk(q2_monster *self)
{
    if (insane_flags(self) & INS_SF_CRAWL) {
        /* 0x80100B60 names module+0x1740, the walk side of the crawl pair. */
        q2_cre_set_move_at(self, INS_MV_CRAWL_WALK);
        return;
    }

    if (self->target != 0) {
        if (insane_is_down(self))
            q2_cre_set_move(self, INS_DOWN_3);
        else
            insane_stand(self);
        return;
    }

    /*
     * 0x80100BEC is `slti v0, v0, 16385` — `r < 16385`, which is `r <= 16384`
     * and so id's `random() <= 0.5`, against insane_stand's `slti 16384` for
     * id's `random() < 0.5`. The module preserves id's own split between the
     * two functions, and the comparison here is written with the module's
     * immediate and the module's `<`; see INS_R_LE_HALF.
     */
    q2_cre_set_move_at(self, ins_rand() < INS_R_LE_HALF ? INS_MV_WALK_1_WALK
                                                        : INS_MV_WALK_2_WALK);
}

/*
 * insane_run — module+0xC1C. Callback 4, and method 5.
 *
 * Structurally identical to insane_walk and it selects the run variants of the
 * same three moves — module+0x1750 for the crawl, +0x162C and +0x169C for the
 * two walks. Those carry the same frame ranges as the walk records, so they are
 * addressed by module address rather than by frame; see INS_CRAWL.
 */
static void insane_run(q2_monster *self)
{
    if (insane_flags(self) & INS_SF_CRAWL) {
        /* 0x80100C4C names module+0x1750, which is module+0x1740 word for
         * word — see INS_CRAWL. Reaching the right one costs nothing here and
         * says what the module said. */
        q2_cre_set_move_at(self, INS_MV_CRAWL_RUN);
        return;
    }

    if (self->target != 0) {
        if (insane_is_down(self))
            q2_cre_set_move(self, INS_DOWN_3);
        else
            insane_stand(self);
        return;
    }

    /* 0x80100CD8, `slti 16385` — the same `r < 16385` as insane_walk, and the
     * two arms name the RUN records at 0x80100CE8 and 0x80100CF0. Those end in
     * insane_run rather than insane_walk, which is the branch that was inert
     * while the port could only reach a move by its first frame. */
    q2_cre_set_move_at(self, ins_rand() < INS_R_LE_HALF ? INS_MV_WALK_1_RUN
                                                        : INS_MV_WALK_2_RUN);
}

/*
 * insane_dead — module+0xAD8. Method 2, and id's insane_dead line for line.
 *
 * Reached three ways: as the end callback of St Death, Cr Death and On Back,
 * and inline from insane_die. A prone body gets FL_FLY so it stays where it
 * fell; an upright one becomes MOVETYPE_TOSS and drops.
 */
static void insane_dead(q2_monster *self)
{
    if (insane_flags(self) & INS_SF_CRAWL)
        self->flags |= Q2_FL_FLY;                  /* 0x80100AF8, `ori 0x1`  */
    else
        /*
         * 0x80100B04..0x80100B18, `lw +0x20; & 0xFFC3FFFF; | 0x001C0000; sw` —
         * 0x1C0000 >> 18 is 7, id's MOVETYPE_TOSS, and an upright body starts
         * falling. This used to be read and dropped for want of a field.
         */
        self->movetype = Q2_MOVETYPE_TOSS;

    self->svflags   |= Q2_SVF_DEADMONSTER;   /* 0x80100B24 */
    self->next_think = 0;                    /* 0x80100B20 */
}

/*
 * insane_cross — module+0x10D4. Method 13, and the crucified stand, walk and
 * run all at once.
 *
 * Four instructions: `currentmove = "On Back"`. Because that move is one frame
 * long its end callback — insane_dead — fires every tick, which is what keeps a
 * crucified Insane pinned and inert.
 */
static void insane_cross(q2_monster *self)
{
    q2_cre_set_move(self, INS_ON_BACK);
}

/*
 * insane_pain — module+0x5D0. Callback 11, and it now RECEIVES the damage.
 *
 * id's insane_pain with its sound removed and everything else intact.
 *
 * AND IT STILL DOES NOT WANT THE ARGUMENT, which is worth saying now that one
 * is there to want. The prologue puts a0 in s1 at 0x801005EC and the body
 * never touches a1, a2 or a3 again — there is no comparison against anything
 * but level.time, the skill cvar, the spawnflags and currentmove in the whole
 * function. That is id as well: `insane_pain` splits on `self->health`, not on
 * damage, and only to choose which of four `player/male/pain%i_%i.wav` samples
 * to play. The sample is gone from this module (see the discarded draw below)
 * and it took the only reader of anything but `self` with it. So `damage` is
 * named in the signature the shared table now requires and deliberately left
 * unread, rather than being pressed into a test the module does not make.
 */
static void insane_pain(q2_monster *self, s16 damage)
{
    /* 0x801005F0: level.time off import +0x48 (0x800E46DC). */
    if (q2_level_state.time < self->pain_debounce)
        return;

    /* 0x80100604/0x80100608: three seconds on the 10 Hz AI clock. */
    self->pain_debounce = q2_level_state.time + Q2_AI_SECONDS(3);

    /*
     * 0x80100614 CALLS rand() AND THROWS THE RESULT AWAY. Nothing between that
     * call and the skill test reads v0. id's insane_pain computes
     * `r = 1 + (rand() & 1)` here to build the filename of a player pain
     * sound — `player/male/pain%i_%i.wav` — and this module has no such sound
     * and kept the draw. Reproduced because it advances the shared RNG stream,
     * which every other threshold in this file is drawn from.
     */
    (void)ins_rand();

    /* 0x8010061C..0x80100634: cvar[0]->value, read through import +0x4C. id's
     * "no pain anims in nightmare". */
    if (q2_cre_skill() == 3)
        return;

    /* 0x80100648: id's "Don't go into pain frames if crucified", on this
     * module's 0x40 rather than id's 8. */
    if (insane_flags(self) & INS_SF_CRUCIFIED) {
        insane_cross(self);
        return;
    }

    /*
     * id's four-move test, here the five-record one. There is no
     * `health < max_health / 2 -> skinnum = 1` in this module: the Insane's
     * skin is `rand() % 3` at spawn and it has no wounded pair, so the port's
     * `hurt` flag is deliberately not set here.
     */
    q2_cre_set_move(self, insane_is_down(self) ? INS_CR_PAIN : INS_ST_PAIN);
}

/*
 * insane_die — module+0x6D8. Callback 12, and it now receives the damage too.
 *
 * Three arms, and the first two both end in insane_dead's body inlined.
 *
 * THE DAMAGE IS UNREAD HERE AND THE DISASSEMBLY IS BLUNT ABOUT IT: the FIRST
 * instruction of the function, 0x801006D8, is `addu a1, a0, zero`. It
 * overwrites the second argument register with the entity pointer before
 * anything can look at it, and every load in the body is off that a1. id's
 * insane_die does use damage — it hands it to `ThrowGib` as the gib velocity —
 * and this module throws no gibs at all, so the argument died with them.
 */
static void insane_die(q2_monster *self, s16 damage)
{
    /*
     * 0x801006F0: `gib_health < health` branches away, so the FALL-THROUGH is
     * the destroyed body. It sets deadflag = DEAD_DEAD (bits 22..23 of
     * entity+0x20, 0x80100714) and then runs insane_dead.
     *
     * It plays NOTHING and spawns nothing. id throws bone, meat and a head here
     * and plays `misc/udeath.wav`; this module resolved that sound into
     * module+0x17C0 at precache time and never loads it again. That silence is
     * the module's, not a gap in the transcription.
     */
    if (self->health <= self->gib_health) {
        self->dead     = true;
        self->deadflag = Q2_DEAD_DEAD;  /* 0x80100700..0x80100724            */
        self->gibbed   = true;          /* the port's own field, "destroyed" */
        insane_dead(self);
        return;
    }

    /*
     * 0x80100768..0x80100778: `if (deadflag == DEAD_DEAD) return;`, read as
     * `(entity+0x20 >> 22) & 3` against 2.
     *
     * `self->dead` stays the guard rather than `self->deadflag`. They are the
     * same fact and `q2_monster_damage_reaction` tests the bool, so testing the
     * other one here would be two fields that can disagree about whether this
     * body has already died. The field below records what the module wrote;
     * the bool is what anybody asks.
     */
    if (self->dead)
        return;

    self->dead       = true;
    self->deadflag   = Q2_DEAD_DEAD;    /* 0x8010078C, bits 22..23 of +0x20  */
    self->takedamage = Q2_DAMAGE_YES;   /* 0x801007A8, bits 30..31 of +0x1C  */

    /* 0x801007B8: a prone body skips the death animation entirely — id's
     * `if (self->spawnflags & 8) insane_dead (self);`. */
    if (insane_flags(self) & INS_SF_CRAWL) {
        insane_dead(self);
        return;
    }

    /*
     * 0x801007E0. id picks between the two deaths by testing the current FRAME
     * against the crawl and down ranges; the module tests the current MOVE
     * against the same five records the pain handler uses. Same partition,
     * asked of the move rather than the frame — which orders identically,
     * because a frame is only in one of those ranges while its move is
     * playing.
     */
    q2_cre_set_move(self, insane_is_down(self) ? INS_CR_DEATH : INS_ST_DEATH);
}

/* ------------------------------------------------------------------------- */
/* Think functions, by the index the animation frames use                     */
/* ------------------------------------------------------------------------- */

/*
 * [6] insane_checkdown — module+0xD08.
 *
 * The last frame of Stand N (59-64) and the last frame of Stand I (64-92) both
 * carry think 6, which is id's `insane_checkdown` as those moves' end callback.
 * It is the one thing that makes an idle Insane lie down on its own.
 *
 * The module keeps id's 0.3 for one of the two and raises it to 0.36 for the
 * other, chosen on which stand is currently playing (0x80100D34):
 *
 *     currentmove == Stand N  ->  11796/32768 = 0.36
 *     anything else           ->   9830/32768 = 0.30, which is id's
 *
 * Note the delay slot: the 9830 comparison at 0x80100D38 is computed
 * unconditionally and is the one the "not Stand N" arm then tests, so the two
 * thresholds are not symmetrical code — the 0.36 overwrites it.
 *
 * Then id's inner 50/50 between uptodown and jumpdown, but only for a prone
 * Insane; an upright one drops straight into Down 1 instead.
 */
static void insane_checkdown(q2_monster *self)
{
    s32 r = ins_rand();
    const q2_mmove *stand_n = q2_cre_find_move(self, INS_STAND_N);
    s32 gate = (stand_n && self->currentmove == stand_n) ? INS_R_0_36
                                                         : INS_R_0_30;

    if (r >= gate)
        return;

    if (insane_flags(self) & INS_SF_CRAWL) {
        /* 0x80100D80: rand() < 16384 takes uptodown, otherwise jumpdown. */
        q2_cre_set_move(self, ins_rand() < INS_R_LT_HALF ? INS_UP_DOWN
                                                         : INS_JUMP_DOWN);
        return;
    }

    q2_cre_set_move(self, INS_DOWN_1);   /* 0x80100DA8 */
}

/*
 * [7] insane_down_getup — module+0xDC0.
 *
 * The last frame of Down 2 (109-147). Half the time it advances into Down 3,
 * which ends in module+0x10E4 -> Down Up -> insane_stand, so the creature
 * hauls itself back to its feet. The other half it falls through to Down 2's
 * own end callback (module+0xE14), which reinstalls Down 2 and it keeps
 * thrashing.
 *
 * A prone Insane never gets up: the flag test returns before the coin is
 * looked at. The draw still happens first (0x80100DD4), so the RNG stream
 * advances either way.
 */
static void insane_down_getup(q2_monster *self)
{
    s32 r = ins_rand();

    if (insane_flags(self) & INS_SF_CRAWL)
        return;
    if (r >= INS_R_LT_HALF)
        return;

    q2_cre_set_move(self, INS_DOWN_3);
}

/*
 * [8] insane_down_again — module+0xE14. Not a frame think: it is the end
 * callback of Down 1 and Down 2, and it is four instructions that install
 * Down 2. Down 1 therefore always runs into Down 2, and Down 2 loops on itself
 * until think 7 above lets it out.
 */
static void insane_down_again(q2_monster *self)
{
    q2_cre_set_move(self, INS_DOWN_2);
}

/*
 * [9] [10] [11] [12] insane_moan — module+0xE24, +0xED0, +0xF7C and +0x1028.
 *
 * FOUR IDENTICAL COPIES — and a fifth nobody calls. The four registered bodies
 * are the same instruction sequence to the register: only the branch targets
 * differ, because the code sits at four addresses. Every one draws once, gates
 * on the same two constants and reaches the same two handles. id has four
 * distinct sound thinks here — insane_fist, insane_shake, insane_moan and
 * insane_scream, each with its own sample — and this disc carries two Insane
 * samples, so all four collapsed onto one picker.
 *
 * The FIFTH copy is at 0x80101174. It is the same body again, it is not in the
 * method table (which stops at 0x80101028 for slot 12 and 0x801010D4 for 13),
 * and no code or data word in the relocated image holds its address. Dead in
 * the image, named here so "there are four" is not read as "there are four
 * bodies".
 *
 * Which frames use them, from the module's own frame scripts: think 12 opens
 * Walk 1, Walk 2 and both Crawls, fires seven frames into Down 3, and fires
 * once more partway through Down 2; think 9 fires twice during Up Down and once
 * early in Down 2; think 11 once in each of those two; think 10 on the first
 * frame of Stand I.
 *
 * The gate takes the LOW 14 BITS of the draw, so it is a 0..16383 value against
 * 8001 or 10001 — 51.2% or 39.0% chance of a sound. The choice of sample then
 * reads BIT 3 OF THE SAME DRAW, so the two decisions are correlated rather than
 * independent; that is what the module does and it is not a transcription
 * shortcut.
 */
static void insane_moan(q2_monster *self)
{
    s32 r  = ins_rand();
    u32 sf = insane_flags(self);

    if (sf & INS_SF_VOCAL) {
        if ((r & 0x3FFF) < INS_MOAN_GATE_VOCAL)
            return;
        ins_play(self, (r & 8) ? INS_SND_INSANE2 : INS_SND_INSANE9);
        return;
    }

    if ((r & 0x3FFF) < INS_MOAN_GATE_PLAIN)
        return;
    ins_play(self, INS_SND_INSANE9);
}

/* ------------------------------------------------------------------------- */
/* The spawn function — export 0, module+0x84C                                */
/* ------------------------------------------------------------------------- */
/*
 * Everything export 0 does beyond writing the five callbacks, which
 * `q2_creature_spawn` already installs from the decoded module.
 *
 * IT RUNS NOW. `q2_creature_spawn` calls `impl->spawn` last — after the class
 * byte, the scale and the callbacks — and `creworld.c` applies the class row's
 * health before that, which is the loader's own order: 0x8007E68C and
 * 0x8007E698 write health and gib_health out of the row, and only then does
 * 0x8007E6AC call the module's export 0, which is free to overwrite them. For
 * this creature that matters more than for any other on the disc, because the
 * Insane's four variants exist nowhere else: the callbacks, the animations,
 * the knockback and the skin are all chosen down here.
 *
 * ITS INPUT ARRIVES BEFORE THIS FUNCTION. Every variant test reads
 * `spawnflags >> 18`; spawn.c now gives those nine bits the retail source,
 * Population.spawn.flags, using the exact 0xF803FFFF preserve mask and 0x1FF
 * source mask. A crucified LAB placement therefore reaches the cross override
 * below instead of standing up as the generic upright variant.
 *
 * The health IS set here, and it matters: class table row 34 carries health 0
 * and gib -50 for the Insane, and the row is applied BEFORE this runs, so
 * without the module's own write the creature would spawn dead. The -50 in the
 * row and the -50 at 0x8010089C agree, which is the check that says both
 * readings are right.
 *
 * Read and dropped, with the offset named for each — and the list is down to
 * three, because solid, movetype, mass and skinnum have fields now:
 *
 *   0x80100914  link_entity(self, 1) then link_entity(self, 0x81) — copy the
 *               origin into the render position, then again with bit 7 set so
 *               the linked position is left alone. The port links elsewhere.
 *   0x80100978  sb 10, 0x13B(self)   speed_scale = 10, which
 *               `q2_creature_spawn` already reads out of the decoded module.
 *   0x801009AC / 0x80100994  walkmonster_start (import +0xFC) or
 *               flymonster_start (import +0x100), chosen on the prone bit. The
 *               port's equivalent is `q2_monster_start_go`, which the creature
 *               world already runs, and it has no flying variant.
 */
static void insane_spawn(q2_monster *m)
{
    u32 sf;

    if (!m)
        return;

    /*
     * 0x8010086C..0x80100890, and it is ONE read-modify-write of the packed
     * word at entity+0x20: `& 0xFFC3FFFF | 0x00140000` puts 5 in bits 18..21
     * and `& 0xFFFCFFFF | 0x00020000` puts 2 in bits 16..17. 0x140000 >> 18 is
     * MOVETYPE_STEP and 0x20000 >> 16 is SOLID_BBOX, both id's own enumerants,
     * and both have fields now.
     */
    m->movetype = Q2_MOVETYPE_STEP;
    m->solid    = Q2_SOLID_BBOX;

    /* 0x80100894 / 0x8010089C: the module's own health and gib threshold,
     * written over whatever the class row supplied. Note where each lands —
     * health is `sh 100, 0x108(entity+0x24)`, into the LINK object, and
     * gib_health is `sh -50, 0x52(self)`, into the entity. */
    m->health     = 100;
    m->gib_health = -50;

    /* 0x801008A4, `sh 300, 0x4E(self)`. id's `self->mass = 300`, and the
     * decoder has reported it for this creature since it was written with
     * nowhere to put it. */
    m->mass = 300;
    /*
     * max_health is NOT written by the module. The engine's `monster_start`
     * does it, and the two instructions were read to be sure of it: 0x80061ADC
     * is `lhu a1, 0x108(entity+0x24)` — the health this function has just put
     * in the link object — and 0x80061B00 is `sh a1, 0x50(entity)`. That is
     * id's `self->max_health = self->health`, and monster_start (0x800619E0)
     * is what BOTH of the module's start calls reach — import +0xFC
     * `walkmonster_start` for an upright Insane and import +0x100
     * `flymonster_start` for a prone one — so it runs whichever arm the
     * spawnflag takes. This port has no monster_start, so the pair is
     * kept consistent here — said out loud rather than left to look like the
     * module's own write.
     */
    m->max_health = 100;

    /* 0x80100904: id's `aiflags |= AI_GOOD_GUY`, and it is the whole reason
     * the Insane is not counted in the level's monster total. */
    m->aiflags |= Q2_AI_GOOD_GUY;

    sf = insane_flags(m);

    /* The test is 0x8010093C `andi v0, v0, 0x10` on `spawnflags >> 18`, and
     * the body is 0x80100950 `ori v0, v0, 0x1` into aiflags: id's
     * `if (self->spawnflags & 16) aiflags |= AI_STAND_GROUND`, on id's own bit
     * with id's own AI_STAND_GROUND value. */
    if (sf & INS_SF_STAND_GROUND)
        m->aiflags |= Q2_AI_STAND_GROUND;

    /* 0x8010095C: the initial move, before any variant override. */
    q2_cre_set_move(m, INS_STAND_N);

    /*
     * 0x80100974: the prone/upright fork, and the two arms are NOT symmetrical
     * — which is why they are written as one if/else rather than as two tests.
     */
    if (sf & INS_SF_CRAWL) {
        /* 0x80100984, `lhu +0x20; ori 0x800; sh`: the prone variant takes no
         * knockback. Its `flymonster_start` sets FL_FLY as well; the port's
         * start does not, so only the bit the module writes explicitly is set
         * here. Then it jumps to 0x801009EC, skipping the skin entirely. */
        m->flags |= Q2_FL_NO_KNOCKBACK;
    } else {
        /*
         * 0x801009B4..0x801009E8: one draw through import +0x14, divided by
         * three with the 0x55555556 magic multiply, remainder stored as a
         * halfword at entity+0x3A. id's `self->skinnum = rand() % 3`.
         *
         * The draw is in this arm ALONE, so the two variants do not consume
         * the same amount of the RNG stream — which is the sort of thing every
         * threshold in this file is downstream of. And note it is one of THREE
         * whole skins rather than a clean/wounded pair, which is why
         * insane_pain does not set the port's `hurt`; `hurt` is the low bit of
         * exactly this field.
         */
        m->skinnum = (u8)(ins_rand() % 3);
    }

    /*
     * 0x80100A04..0x80100A24: the crucified override, and the answer to why
     * the census prints one address for stand, walk and run. All three become
     * insane_cross, the move becomes the single-frame On Back, and the frame is
     * set to 282 by hand because a one-frame move would otherwise start
     * wherever the previous one left off.
     */
    if (sf & INS_SF_CRUCIFIED) {
        m->stand = insane_cross;
        m->walk  = insane_cross;
        m->run   = insane_cross;
        q2_cre_set_move(m, INS_ON_BACK);
        m->frame = INS_ON_BACK;
    }
}

/* ------------------------------------------------------------------------- */
const q2_cre_impl q2_cre_insane = {
    "Insane",
    {
        insane_stand,       /*  0 stand       module+0xA3C, or +0x10D4 when
                             *                the crucified bit is set — see
                             *                insane_spawn */
        NULL,               /*  1 idle        — the module installs none      */
        NULL,               /*  2 search      — nor one of these              */
        insane_walk,        /*  3 walk        module+0xB30, likewise          */
        insane_run,         /*  4 run         module+0xC1C, likewise          */
        NULL,               /*  5 dodge       — the module writes a literal
                             *                zero to entity+0xF4 at
                             *                0x801008F4. id's misc_insane
                             *                writes NULL there too. */
        NULL,               /*  6 attack      — literal zero, 0x801008F8      */
        NULL,               /*  7 melee       — literal zero, 0x801008FC      */
        NULL,               /*  8 sight       — literal zero, 0x80100900. The
                             *                Insane never notices anybody. */
        NULL,               /*  9 checkattack — the module installs none, so
                             *                monster_start leaves the
                             *                engine's M_CheckAttack in place */
        NULL,               /* 10 bigturn     — the module installs none      */
        /* Slots 11 and 12 take the damage now, so they do not fit
         * `q2_class_method` and go in through the same cast the binder uses to
         * take them out again (crebind.c, `m->pain = (void (*)(q2_monster *,
         * s16))`). Neither of the two reads the argument; both say why. */
        (q2_class_method)(void *)insane_pain,  /* 11 pain  module+0x5D0       */
        (q2_class_method)(void *)insane_die    /* 12 die   module+0x6D8       */
    },
    /*
     * The method table, read out of the module's own array at module+0x17C8 —
     * fourteen words registered in one call to import +0x118 at 0x8010036C
     * with class byte 94 in a0.
     *
     * Only six of the fourteen are reached by an animation frame's think byte
     * (6, 7, 9, 10, 11 and 12). Of the other eight, slots 0 and 3 are empty —
     * 0 is written zero and 3 is never written at all — and slot 13 is not an
     * endfunc but the crucified stand/walk/run. That leaves FIVE, and they are
     * here because the binder resolves a move's END CALLBACK by looking its
     * module address up in this table (crebind.c):
     *
     *     [1] insane_stand       Down Up, Stand N, Stand I            3 moves
     *     [2] insane_dead        St Death, Cr Death, On Back          3 moves
     *     [4] insane_walk        Walk 1, Walk 2 (walk variants)       2 moves
     *     [5] insane_run         Walk 1, Walk 2 (run variants),
     *                            St Pain, Cr Pain                     4 moves
     *     [8] insane_down_again  Down 1, Down 2                       2 moves
     *
     * Fourteen of the Insane's nineteen moves, therefore. Of the remaining
     * five, three end in a pure installer (Up Down and Jump Down in
     * module+0x10F4 -> Crawl, Down 3 in module+0x10E4 -> Down Up) which
     * crebind's `chain_endfunc` follows without needing a name, and the two
     * Crawl records have no end callback at all and simply loop.
     */
    {
        NULL,                   /*  0 — the module's own slot 0 is zero       */
        insane_stand,           /*  1 — Down Up, Stand N and Stand I end here */
        insane_dead,            /*  2 — St Death, Cr Death and On Back        */
        NULL,                   /*  3 — the module SKIPS +0x0C: the store is
                                 *      absent from the table build at
                                 *      0x801002F0..0x801002FC, so the slot is
                                 *      left zero rather than written zero */
        insane_walk,            /*  4 — Walk 1 and Walk 2, walk variants      */
        insane_run,             /*  5 — Walk 1 and Walk 2, run variants, and
                                 *      St Pain and Cr Pain */
        insane_checkdown,       /*  6 */
        insane_down_getup,      /*  7 */
        insane_down_again,      /*  8 — Down 1 and Down 2 end here            */
        insane_moan,            /*  9 */
        insane_moan,            /* 10 */
        insane_moan,            /* 11 */
        insane_moan,            /* 12 */
        insane_cross,           /* 13 — installed as all three of stand, walk
                                 *      and run on a crucified Insane */
        /*
         * 14..31 — the module's table stops at fourteen and no frame carries a
         * think byte above 12.
         */
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
    },
    insane_spawn
};
