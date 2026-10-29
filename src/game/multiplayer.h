/*
 * multiplayer.h — the multiplayer runtime, reconstructed from QMULTI.C.
 *
 * ---------------------------------------------------------------------------
 * Where the multiplayer game actually lives
 * ---------------------------------------------------------------------------
 * Not in the executable. Two earlier passes swept SLES_015.34 for a deathmatch
 * scoring function and found nothing, and the sweep was right: the executable
 * carries the *hook*, not the rules. The player-death handler at `0x800396AC`
 * ends with
 *
 *     if (multiplayer && killer < 4 && victim < 4)
 *         (*(0x800B2F58))->[4](killer, victim);
 *
 * and `0x800B2F58` is the map's own relocatable `LevelBin` module, installed by
 * `0x8007A330`. So the whole of deathmatch is a per-map module — and the module
 * names itself, because its debug string is `"QMULTI.C : LEVEL IS %s\n"`.
 *
 * The module is 5,608 bytes and, checked disc-wide, is **byte-identical on all
 * thirteen arenas** after relocation (MATRIX1…MATRIX9, THEVAT, TIMS, PODCITY,
 * FRAGTOWE). One QMULTI.C, compiled once, stamped into each. Every other map on
 * the disc has a different LevelBin and no multiplayer spawn points, and the two
 * facts partition the disc identically — see `q2psx-inspect multi`.
 *
 * ---------------------------------------------------------------------------
 * How the module reaches the engine
 * ---------------------------------------------------------------------------
 * `0x8007A330` writes `0x800B2FE4` into the module header at `+0x08`, and that
 * one pointer is both the engine's exported API and the shared session state:
 * `0x80079818` fills it with function and data pointers, and the settings the
 * menu already documents (`GRAVITY` at `0x800B335A`, and so on) sit in the same
 * block a little higher up. That is why the session variables below are given as
 * offsets from `0x800B2FE4` as well as absolute addresses — they are the same
 * struct the menu writes to.
 *
 * The slots QMULTI.C uses, read out of the table builder. FORMATS.md §15 counts
 * the same block in slots rather than bytes, so both are given:
 *
 *     +0x004   1  0x8008A4D8   printf (debug only)
 *     +0x024   9  0x80056C60   spawn a named Population batch
 *     +0x028  10  0x80056F8C   select the named Population group
 *     +0x030  12  0x8003DDF8   place player i at a MultiSpawn  (see below)
 *     +0x064  25  &0x800B2D8C  slot for the per-frame level hook
 *     +0x078  30  &0x800B2A24  the team/flag enable — see the note on cut modes
 *     +0x0D4  53  &0x800B2DB4  the frame's dt
 *     +0x0D8  54  &0x800B2C2C  the active player count
 *     +0x0EC  59  &0x800AEBAC  the level clock, in dt units
 *     +0x104  65  0x800D5C30   the player array, stride 784
 *     +0x110  68  0x800894B8   sprintf
 *     +0x1E4 121  0x8001A384   menu page-enter — called with page 45
 *     +0x200 128  0x8001A474   menu item-table loader
 *     +0x20C 131  0x80019B88   the menu engine's tick
 *     +0x3AC 235  &0x800B2E28  the engine's game-state request word
 *
 * The order of the two Population calls is what tells them apart: init passes
 * the GROUP name "MultiBatches" through slot 10 once, and then each BATCH name
 * through slot 9. Slot 10's own body copies its 12-byte argument into a global
 * at 0x800D4AE8, which is what a selection does and what a spawn does not.
 *
 * ---------------------------------------------------------------------------
 * The session variables
 * ---------------------------------------------------------------------------
 *     0x800B334E   E+874   time limit, MINUTES, -1 for none
 *     0x800B3350   E+876   frag limit, -1 for none
 *     0x800B3352   E+878   round limit
 *     0x800B3354   E+880   game mode
 *     0x800B3356   E+882   player count
 *     0x800B3498   E+1204  s16 frags[4]
 *     0x800B34A0   E+1212  s16 team_frags[4]   (round wins in VERSUS)
 *     0x800B34A8   E+1220  s16 kills[4][4]
 *
 * Nothing in the executable writes the mode, the limits or the score arrays:
 * `QFRONT`'s own LevelBin does, which is why an EXE-only search for them fails.
 *
 * ---------------------------------------------------------------------------
 * Six modes, three of them reachable
 * ---------------------------------------------------------------------------
 * QMULTI.C branches on six mode values and `QMRESULT`'s LevelBin carries six
 * scoreboard titles in the same order — `DM SCORES`, `TEAM DM SCORES`,
 * `CTF SCORES`, `TAG SCORES`, `TEAM TAG SCORES`, `VERSUS SCORES` — so the
 * numbering is not inferred, it is spelled out twice.
 *
 * But the front end's mode selector at `0x8010459C` writes exactly three values,
 * 0, 1 and 5, and offers exactly three labels. CTF, TAG and TEAM TAG are built,
 * described in the rules text, given scoreboard titles and given item batches,
 * and cannot be chosen. Two further pieces of evidence say the same thing:
 * QMULTI.C's init does `*(E->[0x78]) = 0` and then immediately branches on
 * `*(E->[0x78])`, so the flag/team setup that follows is unreachable; and no
 * arena on the disc carries a `RedFlag` or `BlueFlag` batch to spawn.
 *
 * This module implements all six, because the rules for the three cut ones are
 * as well established as the rules for the three shipped ones, and marks which
 * is which so a caller cannot accidentally present a cut mode as original
 * behaviour.
 *
 * ---------------------------------------------------------------------------
 * What is NOT reconstructed here
 * ---------------------------------------------------------------------------
 * The flag entity itself. CTF/TAG/TEAM TAG spawn `RedFlag`/`BlueFlag` batches
 * and score through a path this module never sees, because the shipped arenas
 * contain no flags and the code that would have run is dead. Their scoring is
 * therefore left unimplemented rather than guessed: `q2_mp_player_killed` does
 * for modes 2..4 exactly what the original does, which is nothing.
 */
#ifndef Q2PSX_MULTIPLAYER_H
#define Q2PSX_MULTIPLAYER_H

#include "q2psx.h"

/* Four pads, four viewports, four score slots. Every bound in QMULTI.C and in
 * the death hook's `killer < 4 && victim < 4` guard is this number. */
#define Q2_MP_MAX_PLAYERS 4
#define Q2_MP_MAX_TEAMS   4

/* `MultiSpawn0` … `MultiSpawn7`: the selector at 0x80071004 walks a fixed eight
 * names, building each by writing '0' + i over byte 10 of "MultiSpawn\0\0". */
#define Q2_MP_MAX_SPAWNS  8

/* ------------------------------------------------------------------------- */
/* Modes                                                                      */
/* ------------------------------------------------------------------------- */
typedef enum q2_mp_mode {
    Q2_MP_DEATHMATCH      = 0,  /* one frag per kill, frag limit ends it      */
    Q2_MP_TEAM_DEATHMATCH = 1,  /* team frags; a team kill costs a frag       */
    Q2_MP_CTF             = 2,  /* both flags — cut                           */
    Q2_MP_TAG             = 3,  /* red flag only, hold it longest — cut       */
    Q2_MP_TEAM_TAG        = 4,  /* red flag, carrier disarmed — cut           */
    Q2_MP_VERSUS          = 5,  /* last one standing wins the round           */
    Q2_MP_MODE_COUNT
} q2_mp_mode;

/* True for the three the front end can actually select (0x8010459C). */
bool q2_mp_mode_selectable(q2_mp_mode mode);

/* The front end's own label, and QMRESULT's scoreboard title. */
const char *q2_mp_mode_name(q2_mp_mode mode);
const char *q2_mp_score_title(q2_mp_mode mode);

/* ------------------------------------------------------------------------- */
/* Limits                                                                     */
/* ------------------------------------------------------------------------- */
/*
 * The front end does not store a limit, it stores an INDEX into one of three
 * option tables and converts on the way out (0x801021A0). The tables and the
 * shipped default indices are QFRONT LevelBin data, so they are transcribed
 * here and checked against the disc by `q2psx-inspect multi`.
 *
 * VERSUS is the exception the conversion itself makes: in mode 5 the time limit
 * is forced to -1 and the round limit replaces the frag limit.
 */
#define Q2_MP_NO_LIMIT ((s16)-1)

#define Q2_MP_TIME_OPTION_COUNT  12
#define Q2_MP_FRAG_OPTION_COUNT   9
#define Q2_MP_ROUND_OPTION_COUNT  5

#define Q2_MP_TIME_OPTION_DEFAULT  5   /* -> 10 minutes, "TIME LIMIT   10"    */
#define Q2_MP_FRAG_OPTION_DEFAULT  5   /* -> 10 frags,   "FRAG LIMIT   10"    */
#define Q2_MP_ROUND_OPTION_DEFAULT 2   /* -> 3 rounds,   "ROUND LIMIT   3"    */

extern const s16 q2_mp_time_options[Q2_MP_TIME_OPTION_COUNT];
extern const s16 q2_mp_frag_options[Q2_MP_FRAG_OPTION_COUNT];
extern const s16 q2_mp_round_options[Q2_MP_ROUND_OPTION_COUNT];

/* ------------------------------------------------------------------------- */
/* Population batches                                                         */
/* ------------------------------------------------------------------------- */
/*
 * Init selects the group "MultiBatches" and then spawns batches by name
 * (0x80100140). The set depends on the mode and is the only place the cut modes
 * leave a visible mark on a shipped arena's load:
 *
 *     always            Weapons, Specials
 *     unless VERSUS     Health, Armour, Ammo
 *     modes 2, 3, 4     RedFlag
 *     mode 2            BlueFlag
 *
 * VERSUS dropping health, armour and ammo is the rule its own description
 * states: "THERE ARE NO AMMO OR HEALTH POWER-UPS IN THE LEVEL."
 */
#define Q2_MP_BATCH_GROUP "MultiBatches"
#define Q2_MP_MAX_BATCHES 7

/* Fill `out` with the batch names this mode spawns, in the original's order.
 * Returns how many were written. `out` may hold Q2_MP_MAX_BATCHES entries. */
u32 q2_mp_batches(q2_mp_mode mode, const char *out[Q2_MP_MAX_BATCHES]);

/* ------------------------------------------------------------------------- */
/* Spawn selection                                                            */
/* ------------------------------------------------------------------------- */
/*
 * One `MultiSpawnN` as the map supplies it. These are ordinary `StartPos`
 * records — the selector looks them up by name through the same 28-byte table
 * everything else does — so a map may have fewer than eight and the indices
 * need not be contiguous.
 */
typedef struct q2_mp_spawn {
    bool present;
    s32  pos[3];
    s16  angle;
} q2_mp_spawn;

/*
 * The state the selector reads out of each player. `alive` is the original's
 * two tests together: the player record's entity pointer at +288 is non-NULL
 * and that entity's health at +264 is above zero.
 */
typedef struct q2_mp_player_view {
    bool alive;
    s32  pos[3];
} q2_mp_player_view;

/*
 * Pick a spawn point — the reconstruction of 0x80071004.
 *
 * For each present spawn it takes the SMALLEST squared distance to any living
 * player, and returns the spawn whose smallest distance is LARGEST: the classic
 * farthest-point rule. Two details are the original's and both matter:
 *
 *   - each axis delta is divided by eight, rounding toward zero, BEFORE it is
 *     squared. At the disc's scale that is a coarse grid, and it is what keeps
 *     the sum inside 32 bits on a machine with no 64-bit multiply;
 *   - the loop bound is the menu's player count at 0x800B3356, not the number
 *     of live players, and a spawn with no living player near it keeps the
 *     sentinel -1 rather than a distance.
 *
 * When nothing qualifies — no spawn present, or no player alive — the original
 * falls back to picking `MultiSpawn(rand() & 7)` and retrying until it finds
 * one that exists. `rng` supplies that draw; it is called with no bound and
 * masked here exactly as `0x80089E28`'s result is masked with 7. Pass NULL to
 * take the first present spawn instead, which is what a deterministic harness
 * wants.
 *
 * Returns the spawn index, or -1 when the map has no MultiSpawn at all.
 */
typedef u32 (*q2_mp_rng_fn)(void *user);

int q2_mp_select_spawn(const q2_mp_spawn spawns[Q2_MP_MAX_SPAWNS],
                       const q2_mp_player_view *players, u32 player_count,
                       q2_mp_rng_fn rng, void *rng_user);

/* The squared distance the selector actually compares, exposed so a test can
 * check the divide-then-square without reimplementing it. */
s32 q2_mp_spawn_dist2(const s32 a[3], const s32 b[3]);

/* ------------------------------------------------------------------------- */
/* Session                                                                    */
/* ------------------------------------------------------------------------- */
/*
 * How a match ends. These are the values QMULTI.C stores in its own
 * `module+0x15E4`, and they choose both the banner and what the engine is asked
 * to do next, so they are the whole of the exit rules in one enum.
 */
typedef enum q2_mp_end {
    Q2_MP_RUNNING          = 0,
    Q2_MP_END_TIME_UP      = 1,  /* clock passed the limit   -> "TIME UP"     */
    Q2_MP_END_FRAG_LIMIT   = 2,  /* a score reached the limit-> "GAME OVER"   */
    Q2_MP_END_ROUND_OVER   = 3,  /* VERSUS round won         -> "ROUND OVER"  */
    Q2_MP_END_MATCH_OVER   = 4,  /* VERSUS round limit met   -> "GAME OVER"   */
    Q2_MP_END_ROUND_DRAWN  = 5   /* VERSUS, nobody left      -> "ROUND DRAWN" */
} q2_mp_end;

/*
 * What the runtime asks the host for once the banner has run out. The numbers
 * are the engine's own game-state ids, written to 0x800B2E28 and dispatched by
 * the main loop at 0x80018720:
 *
 *     11  0x800415F4  load "MPResults" at its "Default" start — the scoreboard
 *     19  0x80041958  reload flag 7 with no map change — the next round
 */
typedef enum q2_mp_request {
    Q2_MP_REQ_NONE          = 0,
    Q2_MP_REQ_RESULTS       = 11,
    Q2_MP_REQ_RESTART_ROUND = 19
} q2_mp_request;

/* The banner countdown's initial value, from the module's own initialised data
 * at +0x15DC. It is decremented by the frame's dt, which is 6 per field on PAL,
 * so 450 is a second and a half. */
#define Q2_MP_BANNER_TICKS 450

typedef struct q2_mp_session {
    s16 mode;            /* 0x800B3354                                       */
    s16 time_limit;      /* 0x800B334E, minutes, -1 none                     */
    s16 frag_limit;      /* 0x800B3350, -1 none                              */
    s16 round_limit;     /* 0x800B3352                                       */
    s16 player_count;    /* 0x800B3356                                       */

    s16 frags[Q2_MP_MAX_PLAYERS];        /* 0x800B3498                       */
    s16 team_frags[Q2_MP_MAX_TEAMS];     /* 0x800B34A0, round wins in VERSUS */
    s16 kills[Q2_MP_MAX_PLAYERS][Q2_MP_MAX_PLAYERS];  /* 0x800B34A8          */

    /* Per-player team colour, config+2 (0x800B32B8 + p*34).
     * FrontEndResetMultiplayerDefaults seeds this with p & 1. */
    s16 team[Q2_MP_MAX_PLAYERS];

    /* QMULTI.C's own state, at the module offsets named. */
    s16  end;            /* +0x15E4, q2_mp_end                                */
    s16  last_alive;     /* +0x15E0                                           */
    bool banner_armed;   /* +0x15D8                                           */
    s32  banner_ticks;   /* +0x15DC                                           */

    /* Set once when the banner expires; the caller consumes it. */
    q2_mp_request request;
} q2_mp_session;

/*
 * Start a match. This is the front end's own reset (QFRONT +0x2000) plus
 * QMULTI.C's `*(module+0x15E4) = 0`: every score, every team score and the whole
 * kill matrix are cleared, and the limits are taken from the option tables at
 * the shipped default indices. Callers that have their own indices should set
 * the limits afterwards.
 *
 * `players` is clamped to Q2_MP_MAX_PLAYERS.
 */
void q2_mp_session_init(q2_mp_session *s, q2_mp_mode mode, int players);

/* Reloading QMULTI resets its module state, preserving engine-owned scores,
 * team assignments and limits between Versus rounds. */
void q2_mp_round_start(q2_mp_session *s);

/* QMRESULT +0x1580: each player's fire edge latches READY. Once all are ready,
 * a 150-tick countdown must pass below zero before request 18 is raised. */
typedef struct q2_mp_results {
    u32 ready;
    s32 countdown;
} q2_mp_results;
void q2_mp_results_init(q2_mp_results *results);
bool q2_mp_results_tick(q2_mp_results *results, int players,
                         u32 fire_pressed, s32 dt);

/*
 * A kill — the reconstruction of the module's export 1, at +0x0BF4, which the
 * engine calls from `0x800396AC` with the killer's id and the victim's.
 *
 * `killer` is the byte at entity+222, so -1 is a world kill, and the original's
 * first act is to blame the victim for it: `if (killer < 0) killer = victim`.
 * The whole call is ignored once the match has ended.
 *
 * Two behaviours here are the original's and are deliberately kept:
 *
 *   - the kill matrix is incremented at `[victim][victim]`, not
 *     `[killer][victim]`. The index really is the victim twice over
 *     (`victim*8 + victim*2`), and nothing on the disc ever reads the matrix
 *     back, so the diagonal is what the game records;
 *   - in DEATHMATCH a suicide (`killer == victim`) costs a frag, and in TEAM
 *     DEATHMATCH a team kill costs the killer a frag AND the team a frag — so a
 *     player who kills a teammate is charged twice, once personally and once
 *     for the team, exactly as the rules text says.
 *
 * The frag limit is tested against the killer's score in DEATHMATCH and against
 * the killer's TEAM score in TEAM DEATHMATCH, and only for modes 0 and 1: it is
 * `if ((unsigned)mode < 2)` in the original.
 */
void q2_mp_player_killed(q2_mp_session *s, int killer, int victim);

/*
 * VERSUS's own end test, which the original runs inside the same export after
 * the scoring: with the mode at 5 it counts living players and, when exactly one
 * is left, gives that player a round win and ends the round — or the match, if
 * the win count has reached the round limit. Nobody left is a drawn round.
 *
 * Split out because the engine calls it through the death hook while the port
 * has the player states to hand, and because it needs them and the scoring does
 * not.
 */
void q2_mp_versus_check(q2_mp_session *s, const q2_mp_player_view *players,
                        u32 player_count);

/*
 * The per-frame hook the module installs into the engine's level slot
 * (0x80100EA8, installed at +0x1304).
 *
 * `level_time` is the engine's clock at 0x800AEBAC and `dt` the frame's step at
 * 0x800B2DB4, both in the sim's own dt units. The time limit is compared as
 *
 *     level_time > time_limit * 18000
 *
 * — 18000 dt units is sixty seconds at 300 units to the second, which is what
 * makes the menu's numbers minutes. The original builds 18000 out of shifts and
 * subtracts rather than a multiply, and compares UNSIGNED.
 *
 * Once the match has ended this runs the banner instead: it arms once, counts
 * `dt` down from Q2_MP_BANNER_TICKS, and when the count goes negative sets
 * `request` to Q2_MP_REQ_RESTART_ROUND for a round that ended and
 * Q2_MP_REQ_RESULTS for a match that did.
 *
 * Returns the pending request, which is Q2_MP_REQ_NONE until then.
 */
q2_mp_request q2_mp_frame(q2_mp_session *s, s32 level_time, s32 dt);

/* Take the pending request, clearing it. */
q2_mp_request q2_mp_take_request(q2_mp_session *s);

/*
 * The banner for the current end state — "TIME UP", "GAME OVER", "ROUND OVER"
 * or "ROUND DRAWN". NULL while the match is running.
 *
 * WHERE IT GOES, because a caller has to put it somewhere: each of the four
 * one-item tables is { text, x = 256, y = 124 }, and the winner line beside it
 * is a SEPARATE row at { "", x = 256, y = 20 } — module+0x14E8, loaded with
 * font size 16 at 0x80100FF4 and filled by 0x8010098C, while the banner uses
 * size 32 at 0x801010BC. So the winner line sits at the TOP of the screen
 * and the banner across the middle; they are not stacked.
 *
 * 0x80101008 skips the winner row entirely when the end state is 5,
 * Q2_MP_END_ROUND_DRAWN — a drawn round shows the banner alone.
 */
const char *q2_mp_banner(const q2_mp_session *s);

/*
 * May a dead player respawn? The gate at 0x8003DEB4 is one comparison: every
 * mode respawns except VERSUS, where death is out for the round.
 *
 * The engine also requires the pad's fire button on a fresh press (0x8001FC50)
 * and the menu to be closed (0x800AE8B4); those belong to the client.
 */
bool q2_mp_may_respawn(const q2_mp_session *s);

/* ------------------------------------------------------------------------- */
/* Attribution                                                                */
/* ------------------------------------------------------------------------- */
/*
 * Who gets the frag. The engine carries the answer on the entity itself: the
 * signed byte at +222 is the killer's id and the byte at +223 is the means of
 * death, both written by the damage function at `0x80057D54`. In multiplayer
 * that function derives the id from the attacker's entity index when there is
 * an attacking entity, and copies the attacker's own +222 through when there is
 * not — which is how a rocket's owner survives the rocket. 4 is its "not a
 * player" sentinel, and hitting it is what prints "Multiplayer, can't determine
 * which player hit other player".
 *
 * The death handler then applies one correction before it calls the frag hook,
 * and it is the only place a means of death changes the scoring:
 *
 *     if ((unsigned)(mod - 9) < 2) killer = -1;
 *
 * so means 9 and 10 erase the attacker entirely and the kill is charged to the
 * victim as a suicide. Those two are `Q2_MOD_ACID` and `Q2_MOD_LAVA` — the
 * throttled environmental damage of combat.h — which is the rule stated as a
 * rule: dying in the level's own hazards is never somebody else's frag, however
 * you came to be standing in them.
 */
#define Q2_MP_MOD_SELF_FIRST 9   /* Q2_MOD_ACID */
#define Q2_MP_MOD_SELF_LAST  10  /* Q2_MOD_LAVA */

/*
 * What entity+222 holds when whatever hurt you was not a player: the four
 * projectile spawners store it for an owner with no client block (0x8004A208,
 * 0x8004ABF4, 0x8004B1C4, 0x8004BF80), 0x8003DDF8 places every player with it
 * (0x8003DE24 loads it, 0x8003DE34 stores it), and 0x80057E5C tests for it
 * before printing "Multiplayer, can't determine which player hit other player".
 * combat.h carries the same guarded definition.
 */
#ifndef Q2_MP_NOT_A_PLAYER
#define Q2_MP_NOT_A_PLAYER 4
#endif

/*
 * The byte the death handler actually reads, 0x800396C4..0x800396EC:
 *
 *     800396C4  lbu   v0, 223(s0)       ; the mod, as an UNSIGNED byte
 *     800396CC  addiu v0, v0, -9
 *     800396D0  sltiu v0, v0, 2
 *     800396D4  beq   v0, zero, 0x800396E0
 *     800396D8  addiu v0, zero, -1      ; delay slot
 *     800396DC  sb    v0, 222(s0)       ; acid and lava only
 *     800396EC  lb    s1, 222(s0)       ; then the byte, SIGNED, untouched
 *
 * So this is the override and nothing else: -1 for mods 9 and 10, and the
 * caller's byte, as the `sb`/`lb` pair leaves it, for every other mod. It is
 * what both of the handler's gates test — the death voice at 0x80039728
 * (`bne s1, -1`) and the frag hook at 0x80039774 (`slti s1, 4`, signed) — and
 * neither of them tests anything narrower. A 4 stays a 4 here, and 4 is not -1
 * and is not below 4, so it gets neither the voice nor the hook.
 */
s8 q2_mp_killer_field(int killer_field, int means_of_death);

/*
 * The same byte folded for SCORING: -1 for mods 9 and 10 AND for anything
 * outside [0, Q2_MP_MAX_PLAYERS), otherwise the player's id.
 *
 * This is not the handler's gate, and it is not what the frag hook is called
 * with. The handler calls the hook with the raw byte and only when that byte
 * is below 4 (0x80039774); this maps 4 to -1, and `q2_mp_player_killed`
 * charges a -1 to the victim as a suicide, where the console, handed a 4,
 * calls nothing at all. Kept, with that behaviour, for its one production
 * caller in the client's scoring pass; the death chain reads
 * `q2_mp_killer_field` instead.
 */
int q2_mp_attribute_kill(int killer_field, int means_of_death);

/*
 * The corpse timer the death handler installs (`obj->[244] = 1500`), in dt
 * units — five seconds at 300 to the second. It governs the body, not the
 * respawn: respawning waits on the pad, not on a clock.
 */
#define Q2_MP_CORPSE_TICKS 1500

/* ------------------------------------------------------------------------- */
/* Presentation                                                               */
/* ------------------------------------------------------------------------- */
/*
 * Which HUD graphic set the session loads, from `0x8003FE20`: the image in
 * slot 14 is `qk_menu.lbm` in single player, `qk2_menu.lbm` in multiplayer with
 * fewer than three players and `qkm_menu.lbm` with three or four — the HUD
 * shrinking as the viewport does. `multipics.lbm` and `multipic2.lbm` are
 * loaded unconditionally into slots 8 and 12 and are still undecoded.
 */
const char *q2_mp_hud_image(bool multiplayer, int players);

/* ------------------------------------------------------------------------- */
/* Who won                                                                    */
/* ------------------------------------------------------------------------- */
/*
 * The winner code from 0x80100660, in its own numbering:
 *
 *     0..3   player 0..3 won
 *     4..7   team 0..3 won (BLUE, RED, PURPLE, GREEN)
 *     8      drawn — the top two scores are equal
 *
 * The sort is the original's bubble sort over the scores, so ties below the top
 * two do not matter and the draw test is exactly `sorted[0] == sorted[1]`.
 *
 * In TEAM DEATHMATCH the original builds its candidate list in a way worth
 * naming rather than quietly fixing: it marks which team colours are in use, and
 * then pairs the mark for colour `i` with the score of PLAYER `i`. With one
 * player per colour the two agree, but retail defaults to alternating teams:
 * this is an original winner-banner bug, not a reason to change the teams.
 * QMRESULT separately sorts the actual team totals. This function preserves
 * the banner's behaviour.
 */
#define Q2_MP_WINNER_DRAW 8

int q2_mp_find_winner(const q2_mp_session *s);

/*
 * The line the end-of-match banner shows under the winner code (0x8010098C):
 * "<name> WINS" for a player, "BLUE TEAM WIN" and friends for a team,
 * "DRAWN MATCH" for a draw. `names` supplies the four player names the original
 * reads out of the pad-configuration block at 0x800B32CC + p*34; pass NULL for
 * a default of "PLAYER 1".."PLAYER 4".
 *
 * Returns `out`.
 */
const char *q2_mp_winner_text(const q2_mp_session *s, int winner,
                              const char *const *names,
                              char *out, u32 out_size);

/* ------------------------------------------------------------------------- */
/* The scoreboard                                                             */
/* ------------------------------------------------------------------------- */
/*
 * The lines QMRESULT shows when a match ends — the screen the runtime asks for
 * with Q2_MP_REQ_RESULTS, which is state 11, "load MPResults".
 *
 * QMRESULT is a level directory of its own with an 840-byte zone and a 29,988
 * byte LevelBin, and that module carries every piece of text this composes: the
 * six titles in mode order (`DM SCORES`, `TEAM DM SCORES`, `CTF SCORES`,
 * `TAG SCORES`, `TEAM TAG SCORES`, `VERSUS SCORES`), the four colour names, the
 * team line `"%s TEAM SCORED %d"`, and the two-line prompt `ALL PLAYERS PRESS`
 * / `FIRE TO CONTINUE`.
 *
 * Writes up to `max` lines of Q2_MP_SCORE_LINE bytes and returns how many. In a
 * Team Deathmatch groups players under their team totals. Both teams and
 * players sort descending, preserving slot order on ties. `names` supplies the four player names as
 * `q2_mp_winner_text` takes them; NULL gives "PLAYER 1".."PLAYER 4".
 *
 * q2_mp_score_rows also supplies the runtime layout recovered from QMRESULT
 * +0x0E1C..+0x1550: 15-pixel row steps, a gap after each team, and a starting
 * y of (252 - 11 * layout_rows) / 2. READY belongs to the player, not their
 * sorted row. Title and the fixed prompts are separate page furniture.
 */
#define Q2_MP_SCORE_LINE 40
#define Q2_MP_SCORE_ROWS (Q2_MP_MAX_PLAYERS + Q2_MP_MAX_TEAMS)

typedef struct q2_mp_score_row {
    char text[Q2_MP_SCORE_LINE];
    s16 y;
    s8 player;       /* -1 for a team heading */
    s8 colour;
    bool highlight;  /* team headings, or every row outside Team Deathmatch */
} q2_mp_score_row;

u32 q2_mp_score_rows(const q2_mp_session *s, const char *const *names,
                      u32 ready, q2_mp_score_row *rows, u32 max);

u32 q2_mp_scoreboard(const q2_mp_session *s, const char *const *names,
                     char lines[][Q2_MP_SCORE_LINE], u32 max);

/* The team names, indexed 0..3 as the winner code's 4..7 arm indexes them. */
const char *q2_mp_team_name(int team);

#endif /* Q2PSX_MULTIPLAYER_H */
