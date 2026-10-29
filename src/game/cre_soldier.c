/*
 * cre_soldier.c — the Soldier, transcribed from its own module.
 *
 * The Soldier ships on seven of the disc's thirteen AI maps — BASE0 through
 * BASE3, JAIL5, MAGDEMO and WASTE1 — which makes it most of the monsters in
 * the game. Its module is `BASE1 CreAIBin` module 0, 13,164 bytes relocated,
 * and every address below is inside it at a base of 0x80100000.
 *
 * ---------------------------------------------------------------------------
 * It is PC Quake II's soldier, line for line
 * ---------------------------------------------------------------------------
 * Every function decoded lands on id's own source, and several of them do so
 * on constants that were not tuned to produce the match:
 *
 *   soldier_stand          module+0x19A0  `random() < 0.8`  as 26214/32768
 *   soldier_idle           module+0x1A0C  `random() > 0.8`  as 26215/32768
 *   soldier_walk1_random   module+0x1AA0  `random() > 0.1`  as  3278/32768
 *   soldier_fire           module+0x1120  three flash tables chosen by skinnum
 *   soldier_duck_down      module+0x1E64  `pausetime = level.time + 1 s`
 *   soldier_attack3_refire module+0x1F6C  `level.time + 0.4 < pausetime`
 *   soldier_dead           module+0x20F0  movetype TOSS, SVF_DEADMONSTER
 *   soldier_pain           module+0x1028  `velocity[2] > 100` as < -1200
 *   soldier_die            module+0x2318  `health <= gib_health`, gib_health -30
 *
 * The spawn function (module+0x1604, export 0) writes exactly id's SET of
 * callbacks — pain +0xA0, die +0xA4, stand +0xE0, walk +0xEC, run +0xF0,
 * dodge +0xF4, sight +0x100, attack +0xF8, listed in the order the module
 * stores them rather than the order id's source assigns them — with
 * `melee` (+0xFC) explicitly zeroed because a soldier has none; sets `mass = 100`
 * (entity+0x4E) and the animation speed scale to 12 (entity+0x13B);
 * packs `solid = SOLID_BBOX` and
 * `movetype = MOVETYPE_STEP` into entity+0x20; links the entity twice through
 * import +0x1C; calls its own stand callback; calls walkmonster_start
 * (import +0xFC); and only then writes the variant's `skinnum` — +0x1754,
 * +0x1860 or +0x1960, one per arm — followed by the tail all three arms share,
 * `health` at module+0x1968 and `gib_health = -30` at +0x1974.
 *
 * `soldier_spawn` at the bottom of this file is everything in that list the
 * framework does not already do, and it exists because `q2_creature_spawn`
 * CALLS `impl->spawn` now. It did not when this file was written, and the
 * sentence that used to stand here — "so this file installs no `spawn` hook" —
 * is withdrawn. The callbacks, the speed scale and the two links stay the
 * framework's; mass, solid, movetype, skinnum, health and the stand call are
 * the module's own stores and are made there.
 *
 * ---------------------------------------------------------------------------
 * The three soldiers are one creature with three skins
 * ---------------------------------------------------------------------------
 * `skinnum` (entity+0x3A) selects both the weapon and the variant, and the
 * class table's three Soldier rows carry the health that goes with each:
 *
 *     skinnum 0..1   blaster      class 19, health 20   (light guard)
 *     skinnum 2..3   shotgun      class 18, health 30   (shotgun guard)
 *     skinnum 4+     machinegun   class 20, health 40   (machinegun guard)
 *
 * Those are PC Quake II's own figures, creature for creature. A row the module
 * does not know is not a fourth variant: module+0x1654 falls through to
 * `printf("Unknown soldier type\r\n")` at module+0x2B4 and installs NOTHING —
 * no callbacks, no health. See `soldier_skin` for what this port does instead.
 *
 * ---------------------------------------------------------------------------
 * THE DODGE IS EMPTY, and two whole animations fall off the disc with it
 * ---------------------------------------------------------------------------
 * The spawn function does install a dodge — module+0x22F0 into entity+0xF4 at
 * module+0x16C0 — and module+0x22F0 is two instructions, `jr ra` and a
 * delay-slot nop. So this is not a gap in the port: the console's Soldier
 * cannot dodge, because its module ships a handler that returns.
 *
 * That one cut explains everything else the census flags as unreachable. id's
 * `soldier_dodge` is the only thing that installs `soldier_move_duck` and
 * `soldier_move_attack3`, so on this disc:
 *
 *   Duck 45-49 (module+0x2F68) — a real mmove with real frames and
 *   `soldier_run` as its endfunc, which nothing installs. Its thinks 12, 19
 *   and 13 are duck_down / duck_hold / duck_up.
 *
 *   Attack3 30-38 (module+0x2EE8) — likewise a real mmove, and part of the
 *   answer to the census's "module names 6 ranges no decoded move claims":
 *   THREE of those six lie inside it, namely 30-31 (labelled "Death6"),
 *   32-35 "Fire 3 Aim" and 36-38 "Fire 3 Shoot". The other three — 0-0 "Run",
 *   12-14 "Fire 1 Done" and 215-224 "Fire 2 Done" — are not Attack3's and are
 *   still unexplained. Nine frames of ai_charge 0 with thinks 14, 15, 13 at
 *   frames 32, 35 and 36, endfunc `soldier_run` — id's `soldier_move_attack3`
 *   frame for frame. Read out of the record itself: module+0x2EE8 is
 *   {30, 38, frames module+0x2ECC, endfunc module+0x1ADC} and the 27 bytes at
 *   module+0x2ECC are nine {ai 4, dist 0, think} triples with 14, 15 and 13 in
 *   slots 2, 5 and 6. The model still carries "Fire 3 Aim" and
 *   "Fire 3 Shoot" for it; nothing plays them.
 *
 * All five of those think functions are transcribed below anyway, and
 * registered in the method table, because the module registers them: it builds
 * 23 methods at module+0x32E8 and hands the whole table to all three class
 * bytes. A table with holes where the module has entries would say the port
 * failed to read them.
 *
 * ---------------------------------------------------------------------------
 * Sounds — thirteen handles, and the module names every one
 * ---------------------------------------------------------------------------
 * The module carries two tables sharing one index: thirteen resolved handles
 * at `module+0x32A0` on a 4-byte stride, and thirteen 12-byte NAME fields at
 * `module+0x1D0`. The handles are zero on disc because the engine fills them
 * at load; the names are on the disc, and each registration call was followed
 * from the bytes it packs to the slot it stores:
 *
 *     +0x32A0  sol_idle1     +0x32B8  sol_deth1    +0x32C8  msc_udeath
 *     +0x32A4  sol_sght1     +0x32BC  sol_deth2    +0x32CC  wep_machgf1b
 *     +0x32A8  sol_sght1     +0x32C0  sol_deth3    +0x32D0  wep_shotgf1b
 *     +0x32AC  sol_pain1     +0x32C4  wep_shotgr1b / wep_sshotr1b
 *     +0x32B0  sol_pain2
 *     +0x32B4  sol_pain3
 *
 * +0x32A8 really is `sol_sght1` a second time: the calls at module+0x690 and
 * module+0x6F8 pack the same twelve bytes of module+0x1DC, the second one out
 * of registers the first spilled to the stack. id has `sound_sight1` =
 * solsght1 and `sound_sight2` = solsrch1, and this disc ships no search sound,
 * so the second slot gets the first's name.
 *
 * AND THE HANDLES FALL BACK ON EACH OTHER. module+0xE50..+0xF28 runs the same
 * shape twice, once over the pain trio and once over the death trio:
 *
 *     h = pain2 ? pain2 : (pain1 ? pain1 : pain3);
 *     if (!pain1) pain1 = h;  if (!pain2) pain2 = h;  if (!pain3) pain3 = h;
 *
 * import +0x24 returns 0 for a name the map's bank does not carry, so a map
 * shipping only one of the three plays that one for all three. module+0xD9C is
 * the same idea in one step: register `wep_shotgr1b`, and only if that comes
 * back zero register `wep_sshotr1b` into the same slot.
 *
 * WHAT THE MODULE ACTUALLY PLAYS is four of the thirteen, all unconditional —
 * the PSX dropped id's skinnum-selected pain and death voices:
 *
 *     module+0x1A44  think 2   +0x32A0  sol_idle1
 *     module+0x1A78  think 3   +0x32C4  wep_shotgr1b   (the cock)
 *     module+0x2250  sight     +0x32A4  sol_sght1
 *     module+0x1078  pain      +0x32B4  sol_pain3
 *     module+0x23D0  die       +0x32C0  sol_deth3
 *
 * plus +0x32D0 and +0x32CC inside `soldier_fire`. `msc_udeath` is registered
 * and NO instruction loads it, so a Soldier's gib is silent as far as its own
 * module is concerned; the sound belongs to whatever the engine runs on the
 * corpse (import +0x11C).
 *
 * THE INDEX BELOW IS THE PORT'S, NOT THE MODULE'S. `q2_cre_soldier_sound_name`
 * is called from `client/main.c` with a literal 8 and 9 for the two fire
 * sounds, so slots 0..10 are frozen where they were and the two names that
 * were missing are appended at 11 and 12. The module's own slot is written
 * beside every entry.
 *
 * ---------------------------------------------------------------------------
 * Frame numbers are the animation's identity
 * ---------------------------------------------------------------------------
 * Moves are looked up by their first frame rather than by position, so the
 * constants below are the disc's own frame numbering and can be checked
 * against `q2psx-inspect creatures` directly.
 */
#include "random.h"
#include <string.h>

#include "ai.h"
#include "creature.h"
#include "crebind.h"
#include "monster.h"

/* ------------------------------------------------------------------------- */
/* The move set, by first frame                                               */
/* ------------------------------------------------------------------------- */
#define SOL_ATTACK1     0     /*   0..11   attak1   module+0x2E74 */
#define SOL_ATTACK2    12     /*  12..29   attak2   module+0x2EBC */
#define SOL_ATTACK3    30     /*  30..38   attak3   module+0x2EE8 — unreachable */
#define SOL_ATTACK4    39     /*  39..44   attak4   module+0x2F0C */
#define SOL_DUCK       45     /*  45..49   duck     module+0x2F68 — unreachable */
#define SOL_PAIN1      50     /*  50..54           module+0x2D8C */
#define SOL_PAIN2      55     /*  55..61           module+0x2DB4 */
#define SOL_PAIN3      62     /*  62..79           module+0x2DFC */
#define SOL_PAIN4      80     /*  80..96           module+0x2E40 */
#define SOL_START_RUN  97     /*  97..98           module+0x2D48 */
#define SOL_RUN        99     /*  99..104          module+0x2D6C */
#define SOL_ATTACK6   109     /* 109..122  runs    module+0x2F48 */
#define SOL_STAND1    146     /* 146..175          module+0x2C04 */
#define SOL_STAND3    176     /* 176..214          module+0x2C8C */
#define SOL_WALK1     215     /* 215..247          module+0x2D00 */
#define SOL_WALK2     256     /* 256..265          module+0x2D30 */
#define SOL_DEATH1    272     /* 272..307          module+0x2FE4 */
#define SOL_DEATH2    308     /* 308..342          module+0x3060 */
/*
 * The module's four death moves are Death1, Death2, Death5 and Death6 — NOT
 * 1/2/4/6. Death3 (343..387) and Death4 (388..440) exist in the table and no
 * module code references either. The frames below were always right; the label
 * on the third was not, because the move-name reader was pairing every name
 * with the next record's frames (creature.c).
 */
#define SOL_DEATH5    441     /* 441..464          module+0x3200 */
#define SOL_DEATH6    465     /* 465..474          module+0x3230 */

/* Frames a think function jumps to by name. */
#define FRAME_attak102     1
#define FRAME_attak110     9
#define FRAME_attak204    15
#define FRAME_attak216    27
#define FRAME_attak303    32
#define FRAME_runs03      111
#define FRAME_walk101     215

/* ------------------------------------------------------------------------- */
/* Sounds                                                                     */
/* ------------------------------------------------------------------------- */
/*
 * Port index -> module handle slot. The header above has the whole thirteen
 * and how each was read; these six are the ones code below asks for.
 *
 * PAIN AND DEATH NAME THE SLOT THE MODULE PLAYS, NOT THE SAMPLE THE CONSOLE
 * HEARS. module+0xE50..+0xF28 fills a null pain or death handle from its trio
 * — pain2, then pain1, then pain3; deth2, then deth1, then deth3 — and
 * sol_pain3 is in no bank on the disc, so on the console +0x32B4 always holds
 * a fallback. That is the Soldier entries of the table in creature.c, applied
 * to the name below by q2_cre_sound_resolve (crebind.h); the index stays 4
 * because 4 is what soldier_pain plays (module+0x1078, `lw a0, 12980(v1)`).
 */
typedef enum sol_sound {
    SOL_SND_IDLE   = 0,     /* +0x32A0  sol_idle1     think 2, module+0x1A44 */
    SOL_SND_SIGHT  = 1,     /* +0x32A4  sol_sght1     sight,   module+0x2250 */
    SOL_SND_PAIN   = 4,     /* +0x32B4  sol_pain3     pain,    module+0x1078 */
    SOL_SND_DEATH  = 7,     /* +0x32C0  sol_deth3     die,     module+0x23D0 */
    SOL_SND_MACHGUN = 8,    /* +0x32CC  wep_machgf1b  fire,    module+0x1550 */
    SOL_SND_SHOTGUN = 9,    /* +0x32D0  wep_shotgf1b  fire,    module+0x148C */
    /* Registered at module+0xCF8 and loaded by nothing — see the header. Kept
     * so the name is still reachable for the bank, not because this plays it. */
    SOL_SND_UDEATH = 10,    /* +0x32C8  msc_udeath                           */
    SOL_SND_COCK   = 11,    /* +0x32C4  wep_shotgr1b  think 3, module+0x1A78 */
    SOL_SND_COUNT  = 13
} sol_sound_id;

/*
 * The thirteen names. Indices 0..10 are frozen by `client/main.c`, which asks
 * for 8 and 9 by literal; 11 and 12 are the two the module registers into
 * +0x32C4 and that this list used to be missing entirely.
 */
static const char *const k_sol_sound_names[SOL_SND_COUNT] = {
    /*  0  +0x32A0 */ "sol_idle1",
    /*  1  +0x32A4 */ "sol_sght1",
    /*  2  +0x32AC */ "sol_pain1",
    /*  3  +0x32B0 */ "sol_pain2",
    /*  4  +0x32B4 */ "sol_pain3",
    /*  5  +0x32B8 */ "sol_deth1",
    /*  6  +0x32BC */ "sol_deth2",
    /*  7  +0x32C0 */ "sol_deth3",
    /*  8  +0x32CC */ "wep_machgf1b",
    /*  9  +0x32D0 */ "wep_shotgf1b",
    /* 10  +0x32C8 */ "msc_udeath",
    /* 11  +0x32C4 */ "wep_shotgr1b",
    /* 12  +0x32C4 */ "wep_sshotr1b"   /* only when the bank has no shotgr1b */
};

const char *q2_cre_soldier_sound_name(int which)
{
    if (which < 0 || which >= SOL_SND_COUNT)
        return NULL;
    return k_sol_sound_names[which];
}

void (*q2_cre_sound_fn)(q2_monster *m, int which, void *user);
void  *q2_cre_sound_user;
static void (*g_pitched_sound)(q2_monster *, int, u8, void *);
static void *g_pitched_sound_user;
void (*q2_cre_melee_fn)(q2_monster *m, const s32 aim[3],
                        s32 damage, s32 kick, void *user);
void  *q2_cre_melee_user;

#define g_sound      q2_cre_sound_fn
#define g_sound_user q2_cre_sound_user

void q2_cre_set_sound_hook(void (*fn)(q2_monster *, int, void *), void *user)
{
    g_sound      = fn;
    g_sound_user = user;
    g_pitched_sound = NULL;
    g_pitched_sound_user = NULL;
    q2_cre_set_sound_owner_hook(NULL, NULL);
}

void q2_cre_set_pitched_sound_hook(void (*fn)(q2_monster *, int, u8, void *),
                                   void *user)
{
    g_pitched_sound = fn;
    g_pitched_sound_user = user;
}

void q2_cre_play_pitched_sound(q2_monster *m, u32 handle, u8 pitch)
{
    if (g_pitched_sound)
        g_pitched_sound(m, (int)handle, pitch, g_pitched_sound_user);
    else if (g_sound)
        g_sound(m, (int)handle, g_sound_user);
}

void q2_cre_set_melee_hook(void (*fn)(q2_monster *, const s32 *, s32, s32,
                                      void *), void *user)
{
    q2_cre_melee_fn   = fn;
    q2_cre_melee_user = user;
}

static void sol_play(q2_monster *m, sol_sound_id which)
{
    if (g_sound)
        g_sound(m, (int)which, g_sound_user);
}

/* ------------------------------------------------------------------------- */
/* random(), in the module's own fixed point                                  */
/* ------------------------------------------------------------------------- */
#include <stdlib.h>

static s32 sol_rand(void)
{
    return (s32)(q2_random_next() & 0x7FFF);
}

/*
 * The module's literal thresholds, kept as the integers they are.
 *
 * The compiler emits `slti rX, rY, K` for both `random() < f` and
 * `random() > f`, so the two differ by one and BOTH are here rather than one
 * standing in for the other. The pairs that appear: 26214/26215 for 0.8, and
 * 16384/16385 for 0.5. This used to carry 16383 for every 0.5 test, which is
 * off by one against the module in five places.
 */
#define SOL_R_0_8   26214       /* `random() < 0.8`  module+0x19D8 */
#define SOL_R_0_8b  26215       /* `random() > 0.8`  module+0x1A34 */
#define SOL_R_0_1    3278       /* `random() > 0.1`  module+0x1ABC */
#define SOL_R_0_5   16384       /* `random() < 0.5`  six sites: module+0x1BD8,
                                 * +0x1C94, +0x1D6C, +0x1E28, +0x219C, +0x2200 */
#define SOL_R_0_5b  16385       /* `random() > 0.5`  module+0x22C8 */

/* ------------------------------------------------------------------------- */
/* Skill, which several think functions gate on                               */
/* ------------------------------------------------------------------------- */
static s32 g_skill = 1;

void q2_cre_set_skill(s32 skill) { g_skill = skill; }
s32  q2_cre_skill(void)          { return g_skill; }

/* ------------------------------------------------------------------------- */
/* The firing hook. The module reaches the engine's bullet and bolt spawners  */
/* through its import table; the port routes them through one callback so     */
/* combat stays in combat.c rather than leaking into every creature.          */
/* ------------------------------------------------------------------------- */
/* Shared with cre_actions.c, the same way the sound and melee hooks are: a
 * decoded creature reaches the engine's spawners through the same one callback
 * a transcribed one does. */
/*
 * THE OLD ONE-INT HOOK, and the Soldier no longer uses it.
 *
 * It carries `table * 8 + flash` or, from a decoded creature, the import slot
 * its think called (cre_actions.c), and nothing else — no muzzle, no aim. The
 * Soldier used it until its shot had to say WHERE it goes: flashes 5 and 6 are
 * fired straight ahead from a falling body, and a host that only knows "the
 * Soldier fired" can do nothing but put that round into the player. So
 * `soldier_fire` now hands over a whole `q2_cre_shot` like every other
 * transcribed creature, and this stays for the decoded path alone.
 */
void (*q2_cre_fire_fn)(q2_monster *m, int flash_index, void *user);
void  *q2_cre_fire_user;

#define g_fire      q2_cre_fire_fn
#define g_fire_user q2_cre_fire_user

void q2_cre_set_fire_hook(void (*fn)(q2_monster *, int, void *), void *user)
{
    g_fire      = fn;
    g_fire_user = user;
}

/*
 * soldier_fire — module+0x1120.
 *
 * Three muzzle-flash tables, chosen by skinnum at module+0x1150..+0x1188:
 * `< 2` takes module+0x3240, `< 4` takes +0x3260, otherwise +0x3280. The
 * caller's flash number indexes the chosen table by word (module+0x118C), and
 * the entry is what decides whether the shot is a blaster bolt, a shotgun
 * spread or a machinegun burst.
 *
 * The weapon itself is picked a second time at module+0x1430, on the same two
 * thresholds: `skinnum < 2` calls import +0x80 (blaster, damage 5 in a3 and
 * speed 600 at sp+16, module+0x144C/+0x1450), `< 4` calls import +0x88
 * (shotgun, damage 2, kick 1, hspread 1000, vspread 500, 12 pellets,
 * module+0x14B0..+0x14D0) and the rest call import +0x84 (bullet, damage 2,
 * kick 4, hspread 300, vspread 500, module+0x1574..+0x158C). 300 and 500 are
 * id's DEFAULT_BULLET_HSPREAD and DEFAULT_BULLET_VSPREAD and 12 is its
 * DEFAULT_SHOTGUN_COUNT, unchanged, and they are the figures in the three
 * `q2_cre_shot`s below.
 *
 * THE WHOLE FUNCTION, in the module's order:
 *
 *   1. AngleVectors(self->angles, forward, right, NULL)   module+0x11AC
 *   2. start = the posed muzzle (module+0x32D8, vertices 228/234 blended at
 *      load) rotated by the object's matrix, plus its origin  +0x11FC..+0x1270
 *   3. flash 5 or 6 — `addiu v0, s4, -5; sltiu v0, v0, 2` at +0x1274 — and
 *      aim = forward, a straight copy. These are the two DEATH1 frames,
 *      293 and 296 (think 20 and 21): the body fires where it faces while it
 *      falls, at nobody.
 *      Any other flash: end = enemy origin with the enemy's view height
 *      added (+0x12A0..+0x12F8), aim = end - start, then vectoangles,
 *      AngleVectors again for a fresh forward/right/up, two rand()s —
 *      r = crandom * 1000 and u = crandom * 500, `125 * (rand - 16384) << 4`
 *      and `<< 3` over 32768 (+0x1334..+0x13B8) — and
 *      end = start + 8192 forward + r right + u up, aim = normalize(end -
 *      start). id's soldier_fire to the constant.
 *   4. The weapon, on skinnum again (+0x1430).
 *
 * THE MACHINEGUN ARM IS A BURST, and the port had dropped the half that made
 * it one. module+0x14E8 tests AI_HOLD_FRAME (entity+0xDC & 0x80) and, only
 * when it is clear, stores `pausetime = level.time + 3 + rand() % 8` at
 * module+0x1540; after the shot, module+0x159C..+0x15CC holds the frame while
 * `level.time < pausetime` and releases it after. M_MoveFrame re-runs a held
 * frame's think (monster.c), so frame 41 of Attack4 fires again on every 10 Hz
 * tick until the pause runs out: four to eleven rounds a trigger pull, where
 * the port fired one. The same arm runs from the two Death1 frames, so a
 * machinegun guard that dies that way empties a burst into the floor twice on
 * its way down rather than firing two single shots.
 *
 * The two shotgun and machinegun arms each play their module's own report
 * first — +0x32D0 at +0x148C and +0x32CC at +0x1550, both through import
 * +0x20 at the object — and the engine's spawner then plays its own too
 * (0x80061F04, 0x80061EAC): two voices of one sample, as on the console. The
 * blaster arm plays nothing; its bolt has no report.
 */
/* Which of the three flash tables this soldier's skin selects. */
static u8 soldier_skin(const q2_monster *m);

/* module+0xF20..+0xF48: the muzzle cache's two model vertices. */
#define SOL_MUZZLE_VERT_A 228
#define SOL_MUZZLE_VERT_B 234

/* `crandom() * scale` as module+0x1344 and +0x1378 compute it: the product is
 * divided by 32768 rounding toward zero (`bgez; addiu 32767; sra 15`). */
static s32 sol_crandom(s32 scale)
{
    return (s32)(((s64)(sol_rand() - 16384) * (2 * scale)) / 32768);
}

static void soldier_fire(q2_monster *self, int flash_number)
{
    u8 skin = soldier_skin(self);
    int table = (skin < 2) ? 0 : (skin < 4 ? 1 : 2);
    q2_cre_shot shot;
    s32 forward[3], right[3], up[3];
    int k;

    memset(&shot, 0, sizeof(shot));
    shot.flash = table * 8 + flash_number;
    shot.aimed = true;

    q2_angle_vectors(self->angles, forward, right, NULL);
    q2_cre_muzzle(self, SOL_MUZZLE_VERT_A, SOL_MUZZLE_VERT_B, shot.start);

    if (flash_number == 5 || flash_number == 6) {
        for (k = 0; k < 3; k++)
            shot.dir[k] = forward[k];
    } else {
        s32 end[3], aim[3], r, u;
        s16 dir[3];

        /*
         * `self->enemy` with no test, as the module reads it: these flashes
         * only run from the attack moves, which are only entered with one.
         * The NULL arm is the port's — it aims along forward rather than
         * faulting the host.
         */
        if (self->enemy) {
            end[0] = self->enemy->pos[0];
            end[1] = self->enemy->pos[1] + self->enemy->view_height;
            end[2] = self->enemy->pos[2];
            for (k = 0; k < 3; k++)
                aim[k] = end[k] - shot.start[k];
            q2_vectoangles(aim, dir);
            q2_angle_vectors(dir, forward, right, up);
        } else {
            q2_angle_vectors(self->angles, forward, right, up);
        }

        r = sol_crandom(1000);
        u = sol_crandom(500);
        q2_vector_ma(shot.start, 8192, forward, end);
        q2_vector_ma(end, r, right, end);
        q2_vector_ma(end, u, up, end);
        for (k = 0; k < 3; k++)
            shot.dir[k] = end[k] - shot.start[k];
        q2_vector_normalize(shot.dir);
    }

    if (skin <= 1) {
        /* import +0x80: damage 5 in a3, speed 600 and EF_BLASTER 8 on the
         * stack (module+0x144C..+0x1460). */
        shot.slot   = Q2_IMP_FIRE_BLASTER;
        shot.damage = 5;
        shot.speed  = 600;
        shot.count  = 1;
        q2_cre_fire_shot(self, &shot);
    } else if (skin <= 3) {
        /* import +0x88 (module+0x14B0..+0x14D8). */
        sol_play(self, SOL_SND_SHOTGUN);
        shot.slot    = Q2_IMP_FIRE_SHOTGUN;
        shot.damage  = 2;
        shot.kick    = 1;
        shot.hspread = 1000;
        shot.vspread = 500;
        shot.count   = 12;
        q2_cre_fire_shot(self, &shot);
    } else {
        if (!(self->aiflags & Q2_AI_HOLD_FRAME))
            self->pausetime = q2_level_state.time + 3 + (sol_rand() % 8);

        /* import +0x84 (module+0x1568..+0x1598). */
        sol_play(self, SOL_SND_MACHGUN);
        shot.slot    = Q2_IMP_FIRE_BULLET;
        shot.damage  = 2;
        shot.kick    = 4;
        shot.hspread = 300;
        shot.vspread = 500;
        shot.count   = 1;
        q2_cre_fire_shot(self, &shot);

        if (q2_level_state.time >= self->pausetime)
            self->aiflags &= ~(u32)Q2_AI_HOLD_FRAME;
        else
            self->aiflags |= Q2_AI_HOLD_FRAME;
    }
}

/* ------------------------------------------------------------------------- */
/* Think functions, by the index the animation frames use                     */
/* ------------------------------------------------------------------------- */

/* [2] soldier_idle — module+0x1A0C */
static void soldier_idle(q2_monster *self)
{
    if (sol_rand() >= SOL_R_0_8b)
        sol_play(self, SOL_SND_IDLE);
}

/* [3] soldier_cock — module+0x1A68. Four instructions around one play of
 * module+0x32C4, which is `wep_shotgr1b` — the shotgun RELOAD, not its fire
 * sound. It is on frame 7 of attak1 and frame 24 of attak2. */
static void soldier_cock(q2_monster *self)
{
    sol_play(self, SOL_SND_COCK);
}

/*
 * [4] soldier_walk1_random — module+0x1AA0.
 *
 * Nine times out of ten it sends the animation back to the start of walk1, so
 * a patrolling soldier loops the long walk cycle and only occasionally falls
 * through into walk2. That one line is the whole difference between a soldier
 * that paces and one that marches.
 */
static void soldier_walk1_random(q2_monster *self)
{
    if (sol_rand() >= SOL_R_0_1)
        self->nextframe = FRAME_walk101;
}

/* [6] [9] [16] [17] [20] [21] — six of the seven firing frames, each a
 * three-line wrapper around module+0x1120 with the flash number in a1:
 * +0x1B3C=0, +0x1CD0=1, +0x1F9C=3, +0x1FBC=7, +0x20B0=5, +0x20D0=6. The
 * seventh, [14] at +0x1EF0 with a1=2, is NOT a bare wrapper — it inlines
 * soldier_duck_down ahead of the call, and is written out separately below.
 * Flash 4 has no wrapper and no frame that would want it. */
static void soldier_fire1(q2_monster *s) { soldier_fire(s, 0); }
static void soldier_fire2(q2_monster *s) { soldier_fire(s, 1); }
static void soldier_fire4(q2_monster *s) { soldier_fire(s, 3); }
static void soldier_fire6(q2_monster *s) { soldier_fire(s, 5); }
static void soldier_fire7(q2_monster *s) { soldier_fire(s, 6); }
static void soldier_fire8(q2_monster *s) { soldier_fire(s, 7); }

/*
 * The variant, and it comes from the CLASS TABLE ROW.
 *
 * The module registers all three class bytes — 87, 89, 88 — against ONE shared
 * method table (module+0x3C8, +0x4F4, +0x510 hand module+0x32E8 to each) and
 * then dispatches inside its spawn function on the class-table id instead:
 * module+0x1604 reads `lh v1, 0xD2(entity+0x24)` and branches on 19 / 18 / 20,
 * writing
 *
 *     id 19 -> skinnum 0, health 20    (module+0x1860, +0x1858)   blaster
 *     id 18 -> skinnum 2, health 30    (module+0x1754, +0x175C)   shotgun
 *     id 20 -> skinnum 4, health 40    (module+0x1960, +0x1964)   machinegun
 *
 * and `q2psx-inspect classes` confirms rows 18/19/20 carry health 30/20/40.
 *
 * This used to reconstruct the variant from `class_byte` (87 -> 0, 88 -> 2,
 * else -> 4), which the module never does — and because the loader maps a row
 * to a class byte by ORDINAL, all three came out permuted: the 30-hp shotgun
 * guard fired a blaster, the 20-hp light guard fired a machinegun and the 40-hp
 * machinegun guard fired a shotgun. It also mis-steered soldier_attack and both
 * refire pairs, which gate on `skinnum < 4` and `skinnum < 2`.
 *
 * SPLIT IN TWO now that `q2_monster.skinnum` is a real field and `soldier_spawn`
 * runs: `soldier_base_skin` is the module's own dispatch and is what the spawn
 * hook stores into entity+0x3A, and `soldier_skin` is that base with the pain
 * handler's low bit on top. Both read the class row, so the stored field and
 * this function cannot come apart — and this one still answers for a creature
 * that reached the AI without the hook, which a bare `m->skinnum` read would
 * silently call a blaster guard.
 */
static u8 soldier_base_skin(const q2_monster *m)
{
    u8 base;

    switch (m->pop_class_id) {
    case 19: base = 0; break;   /* blaster    */
    case 18: base = 2; break;   /* shotgun    */
    case 20: base = 4; break;   /* machinegun */
    default:
        /*
         * NOT the module's own fall-through, and this comment used to claim it
         * was. module+0x1640 and module+0x1654 answer an unknown row by
         * printing "Unknown soldier type" (module+0x2B4) through import +0x18 and returning with
         * nothing installed — no callbacks, no skinnum, no health. That is not
         * reproducible here: this function is only ever reached from a creature
         * the binder already spawned, so by the time it runs the refusal has
         * been missed. The machinegun arm is chosen because it is the one that
         * needs no ammunition-specific handling, and it is a CHOICE.
         */
        base = 4;
        break;
    }

    return base;
}

static u8 soldier_skin(const q2_monster *m)
{
    /* The wounded half of the pair once the pain handler has set the low bit —
     * see q2_monster.hurt. `soldier_pain` and `soldier_die` now set the same
     * bit in `m->skinnum` itself, which is the store the module makes; this
     * stays the derived form so the two agree by construction. */
    u8 base = soldier_base_skin(m);

    return m->hurt ? (u8)(base | 1u) : base;
}

/*
 * [7] soldier_attack1_refire1 — module+0x1B5C.
 * [8] soldier_attack1_refire2 — module+0x1C18.
 *
 * The two halves of one rule, split by weapon: a blaster soldier re-aims and
 * fires again, a shotgun or machinegun soldier only does so at the highest
 * skill or in melee range. `skinnum < 2` is the blaster.
 *
 * The module's short-circuit is the same as C's: module+0x1BDC jumps straight
 * to the frame store when the skill-3 roll hits, so `range` is never called on
 * that path.
 *
 * `!self->enemy` IS A PORT ADDITION, in these two, in the attack-2 pair, in
 * `soldier_attack6_refire` and in `soldier_sight`. The module does not test
 * it: module+0x1B84 loads entity+0xBC and module+0x1B8C dereferences it for
 * the object pointer with nothing in between, and id's `soldier_attack1_refire1`
 * is the same — `if (self->enemy->health <= 0) return;` with no guard. Both
 * rely on the caller only reaching these frames from an attack move, which is
 * only entered with an enemy. Kept because a NULL here faults the host rather
 * than reading a stale entity, and named rather than left to look transcribed.
 */
static void soldier_attack1_refire1(q2_monster *self)
{
    if (soldier_skin(self) > 1)
        return;
    if (!self->enemy || self->enemy->health <= 0)
        return;

    if ((g_skill == 3 && sol_rand() < SOL_R_0_5)
        || q2_range(self, self->enemy) == Q2_RANGE_MELEE)
        self->nextframe = FRAME_attak102;
    else
        self->nextframe = FRAME_attak110;
}

static void soldier_attack1_refire2(q2_monster *self)
{
    if (soldier_skin(self) < 2)
        return;
    if (!self->enemy || self->enemy->health <= 0)
        return;

    if ((g_skill == 3 && sol_rand() < SOL_R_0_5)
        || q2_range(self, self->enemy) == Q2_RANGE_MELEE)
        self->nextframe = FRAME_attak102;
}

/*
 * [10] soldier_attack2_refire1 — module+0x1CF0.
 * [11] soldier_attack2_refire2 — module+0x1DAC.
 *
 * The same split for the second attack sequence.
 */
static void soldier_attack2_refire1(q2_monster *self)
{
    if (soldier_skin(self) > 1)
        return;
    if (!self->enemy || self->enemy->health <= 0)
        return;

    if ((g_skill == 3 && sol_rand() < SOL_R_0_5)
        || q2_range(self, self->enemy) == Q2_RANGE_MELEE)
        self->nextframe = FRAME_attak204;
    else
        self->nextframe = FRAME_attak216;
}

static void soldier_attack2_refire2(q2_monster *self)
{
    if (soldier_skin(self) < 2)
        return;
    if (!self->enemy || self->enemy->health <= 0)
        return;

    if ((g_skill == 3 && sol_rand() < SOL_R_0_5)
        || q2_range(self, self->enemy) == Q2_RANGE_MELEE)
        self->nextframe = FRAME_attak204;
}

/* ------------------------------------------------------------------------- */
/* Ducking — the three handlers the empty dodge orphaned                      */
/* ------------------------------------------------------------------------- */
/*
 * entity+0x1C is packed, and its top two bits are `takedamage`.
 *
 * duck_down writes 1 there and duck_up writes 2, which is id's DAMAGE_YES and
 * DAMAGE_AIM in that exact order — the pair is what identifies the field, and
 * `cre_berserk.c` reads the same two bits out of its own die handler.
 *
 * BOTH ARE WRITTEN NOW. This note used to end "so it is NOT written here",
 * because nothing on the reconstructed side had a `takedamage`; `monster.h`
 * has one (entity+0x1C bits 30..31, the enum values id's) and the two handlers
 * below store into it. `soldier_die` writes the third store, DAMAGE_YES on the
 * corpse.
 *
 * id's duck_down and duck_up also move `maxs[2]` by 32 and re-link the entity.
 * The module does neither — a ducking Soldier keeps its full box — and that is
 * a real console-versus-PC difference rather than an omission here.
 */

/*
 * [12] soldier_duck_down — module+0x1E64.
 *
 *     if (aiflags & AI_DUCKED) return;          ; +0x1E70, bit 0x800
 *     aiflags |= AI_DUCKED;                     ; +0x1E88
 *     takedamage = DAMAGE_YES;                  ; +0x1E98, entity+0x1C
 *     pausetime = level.time + 10;              ; +0x1EB4, entity+0x10C
 *
 * The 10 is one second on the 10 Hz AI clock, which is id's
 * `pausetime = level.time + 1` unchanged. The early return on an already-set
 * flag is id's too, and it is what stops a re-entered duck resetting the timer.
 */
static void soldier_duck_down(q2_monster *self)
{
    if (self->aiflags & Q2_AI_DUCKED)
        return;

    self->aiflags |= Q2_AI_DUCKED;
    self->takedamage = Q2_DAMAGE_YES;
    self->pausetime = q2_level_state.time + Q2_AI_SECONDS(1);
}

/*
 * [13] soldier_duck_up — module+0x1EC0, and it is straight-line: no guard.
 *
 *     takedamage = DAMAGE_AIM;                  ; +0x1EE0, entity+0x1C
 *     aiflags &= ~AI_DUCKED;                    ; +0x1EE4, mask -2049
 *
 * Also the seventh frame of the unreachable Attack3, which is why id's
 * `soldier_move_attack3` ends a burst standing up.
 */
static void soldier_duck_up(q2_monster *self)
{
    self->takedamage = Q2_DAMAGE_AIM;
    self->aiflags &= ~(u32)Q2_AI_DUCKED;
}

/*
 * [19] soldier_duck_hold — module+0x2068. id's `monster_duck_hold` exactly:
 *
 *     if (level.time >= pausetime) aiflags &= ~AI_HOLD_FRAME;   ; +0x2090
 *     else                         aiflags |=  AI_HOLD_FRAME;   ; +0x20A4
 *
 * AI_HOLD_FRAME is what freezes the animation, so the crouch is held for the
 * second `soldier_duck_down` bought and then released.
 */
static void soldier_duck_hold(q2_monster *self)
{
    if (q2_level_state.time >= self->pausetime)
        self->aiflags &= ~(u32)Q2_AI_HOLD_FRAME;
    else
        self->aiflags |= Q2_AI_HOLD_FRAME;
}

/*
 * [14] soldier_fire3 — module+0x1EF0.
 *
 * id's two-liner: `soldier_duck_down(self); soldier_fire(self, 2);`. The
 * module INLINES the duck — module+0x1EFC..+0x1F4C is module+0x1E68..+0x1EB4
 * instruction for instruction, same loads, same 0x800 test, same
 * 0x3FFFFFFF/0x40000000 takedamage rewrite, same level.time + 10 — and then
 * falls into `jal 0x80101120` with a1 = 2. NOT byte-identical, and this
 * comment used to say it was: the guard branch at +0x1F08 targets +0x1F54
 * where +0x1E74 targets +0x1EB8, and the delay slots differ (+0x1F0C is
 * `addu a0, a1, zero`, +0x1E78 is `lui a0, 0x3FFF`). Written as the call it
 * was in the source, since the two are the same function.
 *
 * Frame 32 of Attack3, which nothing installs.
 */
static void soldier_fire3(q2_monster *self)
{
    soldier_duck_down(self);
    soldier_fire(self, 2);
}

/*
 * [15] soldier_attack3_refire — module+0x1F6C.
 *
 *     if (level.time + 4 < pausetime) nextframe = 32;
 *
 * id's `if ((level.time + 0.4) < self->monsterinfo.pausetime) self->s.frame =
 * FRAME_attak303;` — 0.4 s is 4 ticks, and attak301 is frame 30 so attak303 is
 * 32. Frame 35 of Attack3, which nothing installs.
 */
static void soldier_attack3_refire(q2_monster *self)
{
    if (q2_level_state.time + 4 < self->pausetime)
        self->nextframe = FRAME_attak303;
}

/*
 * [18] soldier_attack6_refire — module+0x1FDC.
 *
 * The running shot. It keeps firing only at the top skill and only while the
 * target is still far enough away to be worth shooting at on the move.
 */
static void soldier_attack6_refire(q2_monster *self)
{
    if (!self->enemy || self->enemy->health <= 0)
        return;
    if (q2_range(self, self->enemy) < Q2_RANGE_MID)
        return;
    if (g_skill == 3)
        self->nextframe = FRAME_runs03;
}

/*
 * [22] soldier_dead — module+0x20F0, the end callback on all four death moves.
 *
 *     nextthink = 0;                    ; +0x2100, entity+0x90
 *     movetype  = MOVETYPE_TOSS;        ; +0x2110, entity+0x20 bits 18..21 = 7
 *     svflags  |= SVF_DEADMONSTER;      ; +0x211C, entity+0x40 |= 2
 *
 * Three of id's six: it also shrinks the box to (-16,-16,-24)..(16,16,-8) and
 * re-links. MOVETYPE_TOSS is 7 in id's enum and SVF_DEADMONSTER is 2, and both
 * land exactly, which is what identifies the two packed fields at entity+0x20.
 *
 * ALL THREE STORES LAND NOW. This used to write the svflag as a bare 2 because
 * monster.h named only Q2_SVF_MONSTER, and to leave entity+0x20 bits 18..21
 * unwritten because the port had no `movetype`. It has both — Q2_SVF_DEADMONSTER
 * and Q2_MOVETYPE_TOSS — and the shrink and the re-link are still the two of
 * id's six this module does not do. It does not touch `deadflag`: `soldier_die`
 * raised that before installing the move.
 *
 * This slot used to be NULL, which mattered more than it looks: the binder
 * resolves a move's endfunc by finding its address among the callbacks and the
 * methods, so with method 22 empty every death move's endfunc resolved to
 * nothing and the corpse re-ran its own death animation for ever.
 */
static void soldier_dead(q2_monster *self)
{
    self->next_think = 0;
    self->movetype   = Q2_MOVETYPE_TOSS;
    self->svflags   |= Q2_SVF_DEADMONSTER;
}

/* ------------------------------------------------------------------------- */
/* The monsterinfo callbacks                                                  */
/* ------------------------------------------------------------------------- */

/* soldier_stand — module+0x19A0 */
static void soldier_stand(q2_monster *self)
{
    const q2_mmove *cur = self->currentmove;
    const q2_mmove *s3  = q2_cre_find_move(self, SOL_STAND3);

    if ((s3 && cur == s3) || sol_rand() < SOL_R_0_8)
        q2_cre_set_move(self, SOL_STAND1);
    else
        q2_cre_set_move(self, SOL_STAND3);
}

/* soldier_walk — module+0x2180 */
static void soldier_walk(q2_monster *self)
{
    if (sol_rand() < SOL_R_0_5)
        q2_cre_set_move(self, SOL_WALK1);
    else
        q2_cre_set_move(self, SOL_WALK2);
}

/*
 * soldier_run — module+0x1ADC.
 *
 * A soldier told to stand its ground stands rather than running, and one that
 * was walking breaks into the run only after a two-frame start; anything else
 * plays that start first. That is what stops it snapping from a walk cycle
 * straight into a sprint.
 *
 * It is also the endfunc on almost every move the Soldier has — attack1,
 * attack2, attack3, attack4, attack6, all four pains and the duck — so it is
 * where the creature comes back from anything that is not dying.
 */
static void soldier_run(q2_monster *self)
{
    const q2_mmove *cur = self->currentmove;

    if (self->aiflags & Q2_AI_STAND_GROUND) {
        q2_cre_set_move(self, SOL_STAND1);
        return;
    }

    if (cur == q2_cre_find_move(self, SOL_WALK1)
        || cur == q2_cre_find_move(self, SOL_WALK2)
        || cur == q2_cre_find_move(self, SOL_START_RUN))
        q2_cre_set_move(self, SOL_RUN);
    else
        q2_cre_set_move(self, SOL_START_RUN);
}

/* soldier_attack — module+0x21CC */
static void soldier_attack(q2_monster *self)
{
    if (soldier_skin(self) < 4) {
        if (sol_rand() < SOL_R_0_5)
            q2_cre_set_move(self, SOL_ATTACK1);
        else
            q2_cre_set_move(self, SOL_ATTACK2);
    } else {
        q2_cre_set_move(self, SOL_ATTACK4);
    }
}

/*
 * soldier_dodge — module+0x22F0, installed into entity+0xF4 by the spawn
 * function at module+0x16C0, module+0x17C8 and module+0x18CC (once per
 * variant arm).
 *
 * TWO INSTRUCTIONS: `jr ra` and a delay-slot nop. Not a stub this port left
 * behind — the module's own dodge does nothing, and every consequence follows
 * from that: the Duck move (45-49) and the Attack3 move (30-38) are both live
 * mmove records that nothing on the disc installs, and thinks 12, 13, 14, 15
 * and 19 are registered handlers that never run. id's `soldier_dodge` is the
 * only caller of either move.
 *
 * Written out rather than left NULL so the table below says "the disc has a
 * handler here and it does nothing" instead of "not transcribed" — the same
 * distinction `cre_berserk.c` draws for `berserk_strike`.
 *
 * `crebind.c` already installs slot 5 into `q2_monster.dodge` and already
 * records that nothing calls it, so there is no plumbing left owing.
 */
static void soldier_dodge(q2_monster *self, q2_monster *other, s32 eta)
{
    (void)self;
    (void)other;
    (void)eta;
}

/*
 * soldier_sight — module+0x2240.
 *
 * Plays the sight sound, and above the lowest skill a soldier that spots you
 * from a distance opens with the running shot rather than closing first. The
 * module reads only a0; `other` arrives in a1 and is never touched.
 */
static void soldier_sight(q2_monster *self, q2_monster *other)
{
    (void)other;

    sol_play(self, SOL_SND_SIGHT);

    if (g_skill > 0 && self->enemy
        && q2_range(self, self->enemy) >= Q2_RANGE_MID) {
        if (sol_rand() >= SOL_R_0_5b)
            q2_cre_set_move(self, SOL_ATTACK6);
    }
}

/*
 * The velocity that promotes a flinch to PAIN4 — module+0x1028 and again at
 * module+0x1090, both `slti v0, v0, -1200` on entity+0x6C.
 *
 * id tests `self->velocity[2] > 100`. Index 1 is the vertical axis here and it
 * points DOWN, and the AI's world scale is 12: 100 * 12 = 1200, with the sign
 * flipped. Two independent conversions landing on one immediate is what says
 * entity+0x6C is velocity[1] rather than something else three words in.
 */
#define SOL_PAIN4_VELOCITY (-1200)

/* soldier_pain — module+0xFA8 */
static void soldier_pain(q2_monster *self, s16 damage)
{
    s32 r;

    /*
     * THE DAMAGE IS PASSED NOW AND THIS HANDLER DOES NOT READ IT.
     *
     * `pain` is called as (self, other, kick, damage), so the amount arrives in
     * a3 — and module+0xFA8..+0x1118 never touches a1, a2 or a3 at all: a0 is
     * copied to s0 at module+0xFB0, every load after that is off s0 or off the
     * import table, and a1's first appearance is the WRITE at module+0x107C
     * setting up the sound call. Nothing in it gates on how hard the hit was.
     *
     * A result, not an omission. The Berserk's pain reads a3 at module+0x1264
     * and the Tank Commander's splits three ways on it, so the argument is
     * plainly reachable in this ABI and the tools show it where it is read; the
     * Soldier's chooses on velocity, skill and a roll instead. id's
     * `soldier_pain` takes `int damage` and ignores it too — the promotion to
     * PAIN4 below is on `velocity[2] > 100`, which is the console's
     * substitute for a damage threshold and is where a harder hit shows up.
     */
    (void)damage;

    /*
     * A CORPSE DOES NOT FLINCH, and this is the other half of the stuck death.
     *
     * The class routes damage to `die` or to `pain`, never to both, so the
     * handler is simply never reached once a monster is dead — the module has
     * no such guard because it does not need one. This port had none either,
     * and with no debounce (below) every further round that landed on a dying
     * soldier re-entered the pain move at frame zero, interrupting the death
     * animation partway and leaving the body frozen in whatever pose the fall
     * had reached. A soldier killed by a burst, which is every soldier killed
     * by a machinegun, was shot several more times on its way down.
     *
     * A port addition, then, and named as one: the debounce alone stops most
     * of it, and this closes the case the debounce cannot.
     */
    if (self->dead)
        return;

    /*
     * BLOODIED AT HALF HEALTH — module+0xFBC..+0xFF4, `skinnum |= 1` on the
     * halfword at entity+0x3A. Not a state change: the port wrote
     * `attack_state = Q2_AS_STRAIGHT` here, which is a different field
     * entirely, so soldiers charged straight at you whenever they were hurt
     * and stayed unmarked for the rest of their lives.
     *
     * `skinnum` is a field now, so the module's actual store is made rather
     * than only its port-side flag: the two are set together and `soldier_skin`
     * derives the same value, so nothing can end up half-bloodied.
     */
    if (self->health < self->max_health / 2) {
        self->hurt = true;
        self->skinnum |= 1u;
    }

    /*
     * THE DEBOUNCE — module+0x1000..+0x1070, entity+0xA8 against level.time.
     *
     * Three seconds on the class's own clock, 30 of the 10 Hz AI ticks. Every
     * hit re-entered the flinch from its first frame without it, so a soldier
     * held under automatic fire never advanced past pose one and never
     * returned to running.
     *
     * A hit taken WHILE the debounce is running is not simply ignored: one
     * that throws the body upward hard enough promotes an ordinary flinch to
     * PAIN4, the off-its-feet one. The three-way move test at module+0x1034 is
     * on this arm ONLY.
     */
    if (q2_level_state.time < self->pain_debounce) {
        if (self->velocity[1] < SOL_PAIN4_VELOCITY && self->currentmove &&
            (self->currentmove->first_frame == SOL_PAIN1 ||
             self->currentmove->first_frame == SOL_PAIN2 ||
             self->currentmove->first_frame == SOL_PAIN3))
            q2_cre_set_move(self, SOL_PAIN4);
        return;
    }

    self->pain_debounce = q2_level_state.time + Q2_AI_SECONDS(3);

    sol_play(self, SOL_SND_PAIN);

    /*
     * THE SECOND VELOCITY TEST, module+0x1088, which this file was missing
     * altogether — so a soldier launched by a rocket or a grenade played an
     * ordinary flinch instead of being knocked off its feet, and PAIN4 was
     * only ever reachable from the debounce arm above.
     *
     * Unguarded by the current move, unlike the one above: any hit hard enough
     * takes it, whatever the soldier was doing.
     *
     * AND IT COMES BEFORE THE SKILL TEST — module+0x1088 then module+0x10A4 —
     * which is ALSO id's order, not a departure from it: id's `soldier_pain`
     * puts `if (self->velocity[2] > 100)` between the pain sound and the
     * `if (skill->value == 3) return;` labelled "no pain anims in nightmare".
     * So on skill 3 a Soldier still gets thrown off its feet even though
     * nothing else can stagger it, on the PC as well as here. (This comment
     * previously claimed the module had reordered the two against id. It has
     * not; the claim was invented and is withdrawn.)
     */
    if (self->velocity[1] < SOL_PAIN4_VELOCITY) {
        q2_cre_set_move(self, SOL_PAIN4);
        return;
    }

    /*
     * NO FLINCH ON THE HARDEST SKILL — module+0x10A4..+0x10BC, reading
     * `skill->value` out of cvar[0] of the table at import +0x4C. The sound
     * still plays and the skin still turns; the animation is what is skipped.
     */
    if (g_skill == 3)
        return;

    /*
     * A THREE-WAY SPLIT, not four. PAIN4 is reachable only through the two
     * promotions above — it is the knocked-off-its-feet animation and a plain
     * hit never selects it. 10813 and 21626 are id's 0.33 and 0.66 of 32768.
     */
    r = sol_rand();
    if (r < 10813)
        q2_cre_set_move(self, SOL_PAIN1);
    else if (r < 21626)
        q2_cre_set_move(self, SOL_PAIN2);
    else
        q2_cre_set_move(self, SOL_PAIN3);
}

/* soldier_die — module+0x22F8 */
static void soldier_die(q2_monster *self, s16 damage)
{
    /*
     * AND THE DIE HANDLER IGNORES THE DAMAGE TOO, read the same way as in
     * `soldier_pain`: module+0x22F8..+0x246C never touches a1, a2 or a3 — a0
     * goes to s1 at module+0x2300 and a1's first appearance is the write at
     * module+0x23B8. id's `soldier_die` takes `int damage` and does not use it
     * either. Neither of this creature's two damage callbacks reads the
     * argument the plumbing now carries, and that is the whole finding.
     *
     * The gib arm below is not a damage test in disguise: it compares the
     * health that is LEFT against `gib_health`, which is the halfword at
     * entity+0x52 and not the amount of the hit.
     */
    (void)damage;

    /*
     * THE GIB ARM — module+0x2310..+0x2370, and it is tested FIRST and without
     * a dead guard, comparing the halfword at object+0x108 against the one at
     * entity+0x52 so the boundary is inclusive. Every store in it is
     * idempotent, so a body already dead can still be blown apart by a later
     * explosion, which is the order the module uses.
     *
     * What it does: deadflag = DEAD_DEAD, nextthink = 0, movetype =
     * MOVETYPE_TOSS, svflags |= SVF_DEADMONSTER — the two packed ones land in
     * one store at module+0x2364. That is `soldier_dead`'s body plus the
     * deadflag, and NOTHING else — no sound, no move, no gibs. The body simply
     * becomes a toss-corpse where it stands.
     *
     * All four are written now; the first pass could only reach two of them.
     *
     * `self->gibbed` below has NO counterpart in the module. It is the port's
     * own bookkeeping flag (monster.h), set here because this is the arm the
     * module reaches past `gib_health` and the reconstructed side has nothing
     * else that records which of the two deaths happened. A port addition, and
     * so is `self->dead`, which is the same fact as the deadflag kept in the
     * bool everything else reads — the two are set together and cannot drift.
     *
     * NO SOUND, which this file used to play. `msc_udeath` is registered into
     * module+0x32C8 at module+0xCF8 and no instruction anywhere in the module
     * loads it, so the udeath belongs to whatever the engine runs on a corpse
     * (import +0x11C) and not to the Soldier. Registering it is how the name
     * gets resident in the map's bank.
     */
    if (self->health <= self->gib_health) {
        self->gibbed     = true;
        self->dead       = true;
        self->deadflag   = Q2_DEAD_DEAD;   /* +0x2350, bits 22..23 = 2 */
        self->next_think = 0;              /* +0x2344                  */
        self->movetype   = Q2_MOVETYPE_TOSS; /* +0x2360, bits 18..21 = 7 */
        self->svflags   |= Q2_SVF_DEADMONSTER;
        return;
    }

    /* `deadflag == DEAD_DEAD` — module+0x237C..+0x2384, the 2-bit field at
     * entity+0x20 bits 22..23. `dead` stays the guard, because that is the one
     * bool `q2_monster_damage_reaction` and the frame driver both read; the
     * field is written beside it so the two say the same thing rather than
     * one of them going stale. */
    if (self->dead)
        return;

    self->dead     = true;
    self->deadflag = Q2_DEAD_DEAD;      /* module+0x2398, +0x23A4 */

    /* A corpse still takes damage, so it can still be blown apart by the arm
     * above — module+0x23BC..+0x23C4 masks entity+0x1C with 0x3FFFFFFF, ORs
     * 0x40000000 and stores it back, which is takedamage = 1 in bits 30..31.
     * This used to be a note saying the port had no takedamage. */
    self->takedamage = Q2_DAMAGE_YES;

    /* The wounded skin, which a death sets whether or not pain ever did —
     * module+0x23CC..+0x23D8, `skinnum |= 1` on the halfword at entity+0x3A. */
    self->hurt = true;
    self->skinnum |= 1u;
    sol_play(self, SOL_SND_DEATH);

    /*
     * FOUR WAYS BY MODULO, not by quartile thresholds.
     *
     * module+0x2400..+0x2414 is the compiler's signed `% 4`: a bias of 3 for
     * negatives, an arithmetic shift by 2, then subtract. rand() is 0..32767
     * so the sign arm never runs and this is `rand() & 3`, but it is written
     * as the modulo the module computes. Distribution is identical to the
     * thresholds this used to carry — 32768 divides by 4 exactly — and the
     * arithmetic is not, which is the point.
     */
    switch (sol_rand() % 4) {
    case 0:  q2_cre_set_move(self, SOL_DEATH1); break;
    case 1:  q2_cre_set_move(self, SOL_DEATH2); break;
    case 2:  q2_cre_set_move(self, SOL_DEATH5); break;
    default: q2_cre_set_move(self, SOL_DEATH6); break;
    }
}

/* ------------------------------------------------------------------------- */
/* The spawn function                                                         */
/* ------------------------------------------------------------------------- */
/*
 * soldier_spawn — everything module+0x1604 (export 0) does that the framework
 * does not, and it has a caller now: `q2_creature_spawn` runs `impl->spawn`
 * last, after the class byte, the scale and the thirteen callbacks.
 *
 * The module has three arms, one per class-table row, and they are IDENTICAL
 * instruction for instruction except for the two figures at the tail —
 * module+0x165C (row 18), +0x1760 (row 19) and +0x1864 (row 20) are the same
 * forty stores three times over. What each arm writes:
 *
 *     speed_scale = 12          ; +0x1670  entity+0x13B — the framework's,
 *                                          from the creature record's scale
 *     mass        = 100         ; +0x1678  entity+0x4E
 *     the eight callbacks       ; +0x1684..+0x16E0, melee zeroed at +0x16EC
 *     movetype    = STEP        ; +0x16F4  entity+0x20 bits 18..21 = 5
 *     solid       = BBOX        ; +0x1700  entity+0x20 bits 16..17 = 2
 *     link, link                ; +0x1710, +0x1724  import 2 with 5 and 133
 *     self->stand(self)         ; +0x1734  through entity+0xE0
 *     walkmonster_start         ; +0x1744  import 58
 *     skinnum, health           ; +0x1754/+0x175C per arm, stored at +0x1968
 *     gib_health  = -30         ; +0x196C, +0x1974  entity+0x52
 *
 * AND THE TAIL IS AFTER walkmonster_start FOR A REASON. Import 58 is
 * 0x80062240, which parks a think on entity+0x94 (0x8006241C) and `jal`s
 * `monster_start` at 0x800619E0. That function ZEROES skinnum — `sh zero,
 * 58(s0)` at 0x80061AFC — and takes `max_health` from whatever health the
 * entity carries at that moment (0x80061B00). So the module writes its skinnum
 * and its health afterwards deliberately, and `max_health` ends up the row's
 * rather than the module's. The port's `q2_creature_spawn` stands in for
 * `monster_start` and runs before this hook, so the same order holds — all
 * but its level count, which comes after because it reads AI_GOOD_GUY, and
 * its random start frame (0x80061B2C), which this hook does not reproduce: it
 * does not call q2_monster_walk_start, so the draw the console takes for it
 * is not taken here (monster.h, over the start wrappers).
 *
 * MASS, MOVETYPE AND SOLID were dropped by the first pass because the port had
 * nowhere to put them. It has all three now (monster.h), and `mass` is written
 * HERE rather than left to the framework: `q2_creature` decodes it per module
 * and `q2_creature_spawn` still does not copy it onto the entity, so this is
 * the only store that reaches the field.
 *
 * HEALTH is the loader's figure and the module's, and they agree row for row —
 * the class table carries 30/20/40 and -30 for rows 18/19/20 and `creworld.c`
 * applies them before this runs, which is the loader's own order (0x8007E68C
 * and 0x8007E698 write health, 0x8007E6AC then calls export 0). So these
 * stores change nothing today. They are made anyway because the module makes
 * them, and because the order means a module that disagreed with its row would
 * win. `max_health` (entity+0x50) is NOT among them: no arm writes it, the
 * loader's pass is what sets it, and `soldier_pain` reads it back at
 * module+0xFBC.
 *
 * THE UNKNOWN ROW writes nothing at all — module+0x1978 prints "Unknown
 * soldier type" and returns, no callbacks, no health, no skinnum. That refusal
 * cannot be reproduced from here for the reason `soldier_skin` gives: the
 * binder has already spawned the creature by the time this runs. The shared
 * stores are made for it and `health` is left as the class row gave it, which
 * is the nearest thing to "the module declined to have an opinion".
 */
static void soldier_spawn(q2_monster *m)
{
    m->mass     = 100;
    m->movetype = Q2_MOVETYPE_STEP;
    m->solid    = Q2_SOLID_BBOX;

    /* entity+0x3A, the base index the module's own dispatch picks. The pain
     * and death handlers OR the low bit onto it later. */
    m->skinnum = soldier_base_skin(m);

    /*
     * The tail all three arms share, module+0x1968..+0x1974: the arm's own
     * health into object+0x108 and gib_health -30 into entity+0x52. The
     * unknown-row arm branches past it, so a row this port had to accept and
     * the module would not keeps whatever the class table gave it.
     */
    switch (m->pop_class_id) {
    case 18: m->health = 30; m->gib_health = -30; break;  /* +0x175C shotgun */
    case 19: m->health = 20; m->gib_health = -30; break;  /* +0x1858 blaster */
    case 20: m->health = 40; m->gib_health = -30; break;  /* +0x1964 machgun */
    default: break;
    }

    /*
     * AND THE STAND CALL — module+0x172C..+0x1738, an indirect jalr on
     * entity+0xE0 with a0 = self, before walkmonster_start rather than after.
     * It is what gives a placed Soldier a `currentmove`: without it the
     * creature stands with a NULL move until some AI callback happens to
     * install one, which is the same hole `crebind.c` describes for the
     * Arachner. `q2_creature_spawn` has already installed the callback by the
     * time this runs, so this is the module's call and not a substitute for it.
     *
     * IT IS MADE LAST HERE AND THE MODULE MAKES IT FIRST — a stated reorder,
     * because the module's stand call sits at +0x1734 ahead of the skinnum and
     * health tail and this one sits behind it. `soldier_stand` reads neither
     * field: it picks between Stand1 and Stand3 on `currentmove` and one roll,
     * so the two orders cannot differ. Written this way so the skinnum a
     * think function would read is already in place before anything runs.
     */
    if (m->stand)
        m->stand(m);
}

/* ------------------------------------------------------------------------- */
const q2_cre_impl q2_cre_soldier = {
    "Soldier",
    {
        soldier_stand,      /*  0 stand       */
        NULL,               /*  1 idle        — the module installs none */
        NULL,               /*  2 search      — nor one of these         */
        soldier_walk,       /*  3 walk        */
        soldier_run,        /*  4 run         */
        /*
         * 5 dodge — NOT a gap, and no longer a NULL. module+0x22F0 is `jr ra`
         * and a nop, so the disc's Soldier has a dodge handler that does
         * nothing. See soldier_dodge for what that costs the creature.
         */
        (q2_class_method)(void *)soldier_dodge,  /* 5 dodge */
        soldier_attack,     /*  6 attack      */
        NULL,               /*  7 melee       — a soldier has none       */
        (q2_class_method)(void *)soldier_sight,  /* 8 sight */
        NULL,               /*  9 checkattack — the module installs none */
        NULL,               /* 10 bigturn     — nor one of these         */
        /*
         * 11 and 12 take the damage now — `void (*)(q2_monster *, s16)` on
         * entity+0xA0 and +0xA4 — so they are cast in like dodge and sight
         * rather than fitting `q2_class_method` directly. Neither reads the
         * argument: see the note at the head of each.
         */
        (q2_class_method)(void *)soldier_pain,   /* 11 pain */
        (q2_class_method)(void *)soldier_die     /* 12 die  */
    },
    {
        /* 23 slots, which is exactly what module+0x3D0..+0x4F0 builds at
         * module+0x32E8 before handing the table to class bytes 87, 89 and 88.
         * Slot 0 is the module's own NULL. */
        NULL,                       /*  0 — the module stores zero      */
        soldier_stand,              /*  1 module+0x19A0 */
        soldier_idle,               /*  2 module+0x1A0C */
        soldier_cock,               /*  3 module+0x1A68 */
        soldier_walk1_random,       /*  4 module+0x1AA0 */
        soldier_run,                /*  5 module+0x1ADC */
        soldier_fire1,              /*  6 module+0x1B3C */
        soldier_attack1_refire1,    /*  7 module+0x1B5C */
        soldier_attack1_refire2,    /*  8 module+0x1C18 */
        soldier_fire2,              /*  9 module+0x1CD0 */
        soldier_attack2_refire1,    /* 10 module+0x1CF0 */
        soldier_attack2_refire2,    /* 11 module+0x1DAC */
        soldier_duck_down,          /* 12 module+0x1E64 — Duck 45, unreached */
        soldier_duck_up,            /* 13 module+0x1EC0 — Duck 48, Attack3 36 */
        soldier_fire3,              /* 14 module+0x1EF0 — Attack3 32, unreached */
        soldier_attack3_refire,     /* 15 module+0x1F6C — Attack3 35, unreached */
        soldier_fire4,              /* 16 module+0x1F9C */
        soldier_fire8,              /* 17 module+0x1FBC */
        soldier_attack6_refire,     /* 18 module+0x1FDC */
        soldier_duck_hold,          /* 19 module+0x2068 — Duck 46, unreached */
        soldier_fire6,              /* 20 module+0x20B0 */
        soldier_fire7,              /* 21 module+0x20D0 */
        soldier_dead,               /* 22 module+0x20F0 — the death endfunc */
        /* 23..31 — the module registers none, so these are the shared inert
         * handler on the console and NULL here. */
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
    },
    /* module+0x1604, export 0 — mass, solid, movetype, skinnum, health,
     * gib_health and the stand call. The scale, the links and
     * walkmonster_start stay the framework's. */
    soldier_spawn
};
