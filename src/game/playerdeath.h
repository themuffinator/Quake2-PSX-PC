/*
 * playerdeath.h — what happens to a player between the killing hit and the
 * next spawn, transcribed from the executable.
 *
 * ---------------------------------------------------------------------------
 * Where it lives
 * ---------------------------------------------------------------------------
 * The port already had the two ENDS of this: `q2_mp_attribute_kill` and
 * `q2_mp_player_killed` score the kill, and the menu has page 41 with its
 * RESTART / RESUPPLY / QUIT rows. What sat between them was one line in the
 * client — "health <= 0, open the death page" — and everything the console does
 * in between was missing: the body, its animation, its timers, and the two
 * different endings single player and deathmatch give it.
 *
 * Five functions are the whole of it, and they hand off to each other through
 * the entity's think pointer at +0x3C:
 *
 *     0x8003A1C8  the player think — installed at spawn (0x8003B3EC), and the
 *                 only caller of the death handler. Its gate is at 0x8003ADB8:
 *
 *                     if (health > 0)            -> alive
 *                     if (ent2 & 0x00080000)     -> already dead
 *                     player_die(self);
 *                     player_anim(self, 4);      -> ANIM_DEATH
 *                     return;                    -> the rest of the think is
 *                                                   skipped on the killing tick
 *
 *     0x800396AC  player_die         — runs ONCE, because its last act but one
 *                                      is to overwrite +0x3C with the corpse
 *                                      think, so the gate above is never
 *                                      reached again
 *     0x80039550  corpse_think       — the body, every tick
 *     0x8003E238  respawn_think      — the corpse timer, in deathmatch
 *     0x8005B358  body_fade          — the body shrinking out of the world
 *
 * and 0x8003CE14 `player_anim` chooses which move plays.
 *
 * ---------------------------------------------------------------------------
 * Nobody drives a corpse, and that is not a rule anybody wrote
 * ---------------------------------------------------------------------------
 * It falls out of the think swap, and it is the single most important
 * consequence of it. Two things live inside the player think and nowhere else:
 *
 *     0x8003A4A4   the pad read (0x80019154's ONLY call site)
 *     0x8003AD98   the view weapon driver 0x8004EE0C's ONLY call site
 *
 * So the moment `player_die` writes the corpse think over `entity+0x3C`, the
 * pad stops being read and the gun stops being driven — without either being
 * tested for anywhere. There is no `if (dead)` in front of the movement code
 * because there does not need to be one. What still moves the body is
 * `corpse_think`'s own physics: gravity, the ground plane, and the dt * 5
 * friction below.
 *
 * A port with one tick function and no think pointer has to say it out loud.
 * `sim.c` does, at the top of `q2_sim_tick`: a dead player is ticked with a
 * neutral pad AND with the input-derived accumulators cleared, because a
 * neutral pad alone only stops `yaw_rate` and `wish` from growing — a body
 * that died mid-turn would keep turning. `vel` is left alone; that is momentum
 * and the corpse is supposed to keep it.
 *
 * ---------------------------------------------------------------------------
 * player_die, 0x800396AC, in order
 * ---------------------------------------------------------------------------
 *   1. `if ((unsigned)(mod - 9) < 2) killer = -1` (0x800396CC). Means 9 and 10
 *      are `Q2_MOD_ACID` and `Q2_MOD_LAVA`, so the level's own hazards erase
 *      the attacker on the ENTITY, not just in the scoring — everything
 *      downstream reads the corrected byte. `q2_mp_attribute_kill` already
 *      carries this rule and is what this module calls.
 *
 *   2. The victim's player number is `(entity->client - 0x800C7C60) / 224`
 *      (0x800396E0..0x80039720, a divide-by-224 written as a multiply by the
 *      modular inverse of 7 and a shift of 5). 224 is the client stride, which
 *      0x8003B288 confirms from the other side.
 *
 *   3. **The death cry is only raised for a death with no killer**
 *      (`bne s1, -1` at 0x80039728 skips it). A player shot by somebody makes
 *      no sound here at all. Which of the two voices depends on `client+0x84`.
 *      This is not what the port had: `update_pain` (0x8003AE10) was raising
 *      `pla_death4` for every death, because that is where the port first
 *      noticed a health crossing.
 *
 *   4. Deathmatch and single player split (0x80039758, on 0x800AEBCC):
 *
 *      DEATHMATCH  pushes a body record through 0x80020D60 — four 88-byte slots
 *                  from 0x800C6D70, taking the entity's kind, the client's
 *                  halfword at +102 and 80 bytes of pose from +84 — and then,
 *                  guarded by `killer < 4 && victim < 4` (SIGNED, so a world
 *                  kill at -1 passes), calls the map module's export 1. That is
 *                  `q2_mp_player_killed`.
 *
 *      SINGLE      opens menu page 41 and arms its 600-tick countdown
 *                  (0x8002059C -> 0x8001D738, 0x800205B0), then debug-prints
 *                  `"Continues %d\n"` with the byte at 0x800B335D. That byte is
 *                  the resupply count the page's middle row greys itself on,
 *                  which is what identifies it.
 *
 *   5. The tail, both paths (0x800397D4..0x80039856):
 *        - pitch is zeroed and the entity's matrix rebuilt from its angles
 *          (`sh zero, 230(s0)` then `RotMatrix(self+230, self+704)`)
 *        - `client->[0]->[288] = 0` — the 784-byte player record at
 *          0x800D5C30 + i*784 keeps a back-pointer to its entity at +0x120
 *          (written at 0x8003B474), and dying clears it: the player no longer
 *          has a body
 *        - the VIEW WEAPON at +0x44 is freed, and the SAME field becomes the
 *          gib threshold, -40. It is not "a model pointer" — 0x8004EE0C opens
 *          `s6 = self->[68]` and then `s7 = s6->[12]`, so entity+0x44 holds a
 *          whole second ENTITY with a client block of its own, and 0x8006D280
 *          detaches it, unlinks it and pushes it back onto the free stack at
 *          0x800B2BAC. One word, two lives: the gun you are holding while
 *          alive, `gib_health` once dead, which is how corpse_think compares
 *          it against health
 *        - the corpse timer at +0xF4 is set to 1500
 *        - +0x3C becomes corpse_think
 *        - and if the game state at 0x800B2AA8 is 1 or 2 it becomes 3, with a
 *          deadline of `clock + 1200` at 0x800B2A10. 0x80041D30 watches that
 *          deadline and, because state 3 stops it being pushed out again,
 *          eventually raises game-state request 8 — which is 0x8004149C,
 *          "load MagazineExtrQFront". **A single-player death that is never
 *          answered walks back to the front end by itself.**
 *
 * ---------------------------------------------------------------------------
 * corpse_think, 0x80039550
 * ---------------------------------------------------------------------------
 *     if (health <= gib_health) { gib(self); return; }        0x8003956C
 *     physics(self);                                          0x800552B4
 *     approach(&vel[i], 0, dt * 5)  for i in 0..2             0x8006FE3C
 *     clip_velocity(self);                                    0x80039AA4
 *     if (!deathmatch)   { ent2 |= DEAD; return; }            0x80039610
 *     if (animflags == 0){ player_anim(self, DEATH); return; }
 *     ent2 |= DEAD;
 *     relink(self);                                           0x8007CD24
 *     box.y = 143;  bounds.y = pos.y + 143;                   +0x6E, +0x7C
 *     think = respawn_think;  ent2 |= 0x8000;
 *
 * Those two writes are one thing said twice: 0x80020BB0 builds the world
 * bounds at +0x78..+0x8C as `pos + box` from the six halfwords at +0x6C, so
 * setting +0x6E and then +0x7C is setting the box's Y extent and bringing the
 * bound that derives from it along by hand. **The corpse's collision box is
 * flattened once it has finished falling** — id's `self->maxs[2] = -8`, on an
 * axis that points down.
 *
 * The `animflags` test is `lh v0, 258(s0)`, entity+0x102, and it means exactly
 * "has the death animation finished": 0x8003DFE4 clears its low two bits when a
 * move is installed and 0x8003DF90 sets bit 0 the moment the frame cursor walks
 * past the move's end. So the body finishes falling before anything else
 * happens to it.
 *
 * That is also why the two paths set DEAD at different times. Single player
 * raises it on the first corpse tick; deathmatch raises it when the animation
 * ends. Either way it is corpse_think that raises it and NOT the death handler,
 * which is what makes the handler's own gate a one-shot.
 *
 * ---------------------------------------------------------------------------
 * respawn_think, 0x8003E238, and the body's end
 * ---------------------------------------------------------------------------
 *     if (!effect_fade(self)) {                               0x8005B2A8
 *         0x8005B880(self); physics(self);
 *         if (health <= gib_health) { gib(self); return; }
 *     }
 *     clip_velocity(self);
 *     corpse_ticks -= dt;
 *     if (corpse_ticks <= 0) { corpse_ticks = 1; think = body_fade; }
 *
 * and `body_fade` (0x8005B358) takes `dt * 16` off entity+0xFC each tick and
 * releases the model when it reaches zero. The draw-time access census shows
 * that +0xFC is a lighting intensity, not a geometric scale: the body darkens
 * away over 256 dt — under a second — after its five-second wait.
 *
 * **Nothing in this chain respawns the player.** The corpse animates, waits its
 * 1500 and dissolves; putting a player back is 0x8003DDF8, and 0x8003DDF8 has
 * exactly one caller (0x8003DECC, the mode gate this port already carries as
 * `q2_mp_may_respawn`) and one other reference — slot 12 of the engine block,
 * which is QMULTI.C's. The map module decides when you come back, and the
 * engine only ever decides that you are gone.
 *
 * ---------------------------------------------------------------------------
 * The player's own animation set, 0x8003CE14
 * ---------------------------------------------------------------------------
 * Ten moves, looked up by name at 0x8003C5F8..0x8003CBFC in this order:
 *
 *     Stand  Run  Attak  Death 1  Death 2  Death 3  Jump  Pain 1  Pain 2  Pain 3
 *
 * and the six animation ids the code passes select them as:
 *
 *     0 STAND    Stand, unless Attak is mid-play and has not wrapped
 *     1 RUN      Run
 *     2 JUMP     Jump
 *     3 PAIN     Pain 1/2/3, `rand() % 3`
 *     4 DEATH    Death 1/2/3, `rand() % 3`
 *     5 ATTACK   Attak
 *
 * Two rules matter to a death and both are in the function twice over. A death
 * move is never replaced — 0x8003CEB4 refuses to pick a new one when the
 * current move is already Death 1/2/3, and 0x8003D158 refuses to install
 * anything over one. And a pain move holds until it has played out UNLESS the
 * request is DEATH (0x8003D1D8), so being killed while flinching still drops
 * you properly.
 */
#ifndef Q2PSX_GAME_PLAYERDEATH_H
#define Q2PSX_GAME_PLAYERDEATH_H

#include "multiplayer.h"   /* q2_mp_attribute_kill — the mod-9/10 rule */
#include "q2psx.h"

/* ------------------------------------------------------------------------- */
/* Constants, each with the instruction that carries it                       */
/* ------------------------------------------------------------------------- */

/* entity+0x44 after the handler has run — the gib threshold (0x800397FC). */
#define Q2_PDEATH_GIB_HEALTH      (-40)

/* entity+0xF4 (0x80039810). Five seconds at 300 dt to the second, and the same
 * number multiplayer.h already names as Q2_MP_CORPSE_TICKS. */
#define Q2_PDEATH_CORPSE_TICKS    1500

/* The corpse's box on the Y axis once the death animation has ended
 * (0x8003964C, and 0x80039670 for the world bound that derives from it). */
#define Q2_PDEATH_BODY_BOX_Y      143

/* The death page's own countdown (0x800205B0), in level-clock units. */
#define Q2_PDEATH_ARM_TICKS       600

/* clock + 1200 at 0x8003984C, after which 0x80041D30 raises game-state 8 and
 * the console goes back to the front end on its own. */
#define Q2_PDEATH_ABANDON_TICKS   1200

/* The body's light intensity at +0xFC, and what body_fade takes off it per dt
 * (0x8005B36C, `sll v1, v1, 4`). */
#define Q2_PDEATH_SCALE_ONE       4096
#define Q2_PDEATH_FADE_RATE       16

/* corpse_think's velocity friction: `approach(v, 0, dt * 5)` (0x800395A0). */
#define Q2_PDEATH_FRICTION        5

/* What 0x8003DDF8 writes into entity+222 when it places a player (0x8003DE34).
 * Not -1: 4 is the engine's "not a player" sentinel, which the frag hook's own
 * `killer < 4` bound rejects, so a fresh spawn owes nobody a frag. */
#define Q2_PDEATH_NO_KILLER       4

/* 0x8003B2B8, at the same site. */
#define Q2_PDEATH_SPAWN_HEALTH    100

/* entity+0x10C bit — worldscale.h calls it Q2_ENT2_DEAD and this is the same
 * bit, restated here so a reader of this file can see what the gate tests. */
#define Q2_PDEATH_DEAD_BIT        0x00080000u
/* Raised beside it when the body is handed to respawn_think (0x8003966C). */
#define Q2_PDEATH_SETTLED_BIT     0x00008000u

/* entity+0x102, cleared by 0x8003DFE4 and bit 0 set by 0x8003DF90. */
#define Q2_PDEATH_ANIM_WRAPPED    0x0001u
#define Q2_PDEATH_ANIM_OVERRUN    0x0002u

/* ------------------------------------------------------------------------- */
/* The player's moves                                                         */
/* ------------------------------------------------------------------------- */
typedef enum q2_player_move {
    Q2_PMOVE_NONE   = -1,
    Q2_PMOVE_STAND  =  0,
    Q2_PMOVE_RUN,
    Q2_PMOVE_ATTAK,
    Q2_PMOVE_DEATH1,
    Q2_PMOVE_DEATH2,
    Q2_PMOVE_DEATH3,
    Q2_PMOVE_JUMP,
    Q2_PMOVE_PAIN1,
    Q2_PMOVE_PAIN2,
    Q2_PMOVE_PAIN3,
    Q2_PMOVE_COUNT
} q2_player_move;

/* The name the executable looks the move up by (0x800AC554 + 12*i). */
const char *q2_player_move_name(q2_player_move m);

/* True for Death 1/2/3 and for Pain 1/2/3 — the two sets 0x8003CE14 treats
 * as a group rather than as individual moves. */
bool q2_player_move_is_death(q2_player_move m);
bool q2_player_move_is_pain(q2_player_move m);

typedef enum q2_player_anim {
    Q2_PANIM_STAND  = 0,
    Q2_PANIM_RUN    = 1,
    Q2_PANIM_JUMP   = 2,
    Q2_PANIM_PAIN   = 3,
    Q2_PANIM_DEATH  = 4,
    Q2_PANIM_ATTACK = 5
} q2_player_anim;

/*
 * 0x8003CE14, as a pure choice: what move should be playing, given what is
 * playing now and what the tick wants.
 *
 * `roll` is the engine's `rand() % 3` — the caller supplies it so the choice is
 * reproducible in a test. `animflags` is entity+0x102.
 *
 * Returns Q2_PMOVE_NONE when the request is refused, which is a real answer and
 * not a failure: a death move is never replaced, and a pain move holds until it
 * wraps unless the request is DEATH.
 */
q2_player_move q2_player_anim_pick(q2_player_anim want, q2_player_move cur,
                                   u16 animflags, u32 roll);

/* ------------------------------------------------------------------------- */
/* The state a dying player is in                                             */
/* ------------------------------------------------------------------------- */
/*
 * Named after which of the engine's four functions owns the entity, because
 * that is what the state IS: whichever address is sitting in +0x3C.
 */
typedef enum q2_pdeath_stage {
    Q2_PDEATH_ALIVE = 0,  /* +0x3C is the player think, 0x8003A1C8      */
    Q2_PDEATH_DYING,      /* corpse_think, animation still running      */
    Q2_PDEATH_DOWN,       /* respawn_think, the 1500 counting away      */
    Q2_PDEATH_FADING,     /* body_fade, the scale counting away         */
    Q2_PDEATH_GIBBED,     /* health fell past -40; there is no body     */
    Q2_PDEATH_GONE        /* the model was released                     */
} q2_pdeath_stage;

typedef struct q2_player_death {
    q2_pdeath_stage stage;

    /* The entity's own two bytes, +222 and +223, after the handler's
     * correction. */
    s8   killer;
    s16  mod;
    int  victim;          /* (client - 0x800C7C60) / 224                */

    s16  gib_health;      /* entity+0x44                                */
    s16  corpse_ticks;    /* entity+0xF4                                */
    s16  box_y;           /* entity+0x6E, flattened to 143 when down    */
    s16  scale;           /* entity+0xFC light intensity (retained name) */
    s16  velocity[3];     /* entity+0xE0..0xE4                          */
    s16  pitch;           /* entity+0xE6                                */

    q2_player_move move;  /* entity+0x2E0, as an index                  */
    u16  animflags;       /* entity+0x102                               */
    u32  ent2;            /* entity+0x10C, for the DEAD and settled bits*/

    bool linked_weapon;   /* was there a model in +0x44 to release      */
    bool has_body;        /* the player record's +0x120 back-pointer    */
} q2_player_death;

/*
 * What one call to the handler did, for a caller that has to act on it. Every
 * field is something the engine does through a subsystem this struct cannot
 * reach — a sound, a menu page, the map module's export.
 */
typedef struct q2_player_death_event {
    bool cried_out;       /* the death voice was raised (killer == -1)  */
    bool drowned;         /* ...and it was the drowning one             */
    bool body_recorded;   /* the deathmatch body record was pushed      */
    bool frag_hook;       /* the module's export 1 was called           */
    int  frag_killer;     /* what it was called with                    */
    int  frag_victim;
    bool death_page;      /* single player: page 41, armed for 600      */
    bool abandon_armed;   /* the 1200 walk-back-to-the-front-end        */
    bool gibbed;          /* the body never existed                     */
} q2_player_death_event;

/* ------------------------------------------------------------------------- */
/* The chain                                                                  */
/* ------------------------------------------------------------------------- */

/* The state 0x8003B250 leaves a freshly placed player in. */
void q2_player_death_init(q2_player_death *d);

/* The gate at 0x8003ADB8, verbatim: dead when health has gone and the DEAD bit
 * has not yet been raised by the corpse think. */
bool q2_player_should_die(s16 health, u32 ent2_flags);

/*
 * Does this death raise the death voice?
 *
 * 0x80039728 skips the sound outright unless entity+222 is -1, and 0x800396CC
 * has just forced it to -1 for acid and lava. So the answer is "only a death
 * nobody is credited with" — the world's kills and the level's hazards — and a
 * player shot by somebody dies silently.
 *
 * Split out because the sound is raised inside the sim, where the rest of this
 * chain is not.
 */
bool q2_player_death_cries_out(s8 killer_field, s16 means_of_death);

/*
 * 0x800396AC. `killer_field` and `means_of_death` are the entity's own bytes —
 * the port keeps them as `q2_actor.last_attacker` and `q2_actor.last_mod`.
 * `drowning` is `client+0x84`, which picks the second death voice.
 *
 * `ev` may be NULL. Runs once: calling it on a player that is not ALIVE does
 * nothing, which is the one-shot the think-pointer swap gives the original.
 */
void q2_player_die(q2_player_death *d, s8 killer_field, s16 means_of_death,
                   int victim, bool deathmatch, bool drowning,
                   q2_player_death_event *ev);

/*
 * 0x80039550 and 0x8003E238 and 0x8005B358, dispatched on the stage the way the
 * engine dispatches on +0x3C. `dt` is the frame step in the sim's own units;
 * `roll` is the `rand() % 3` for a death move that has not been chosen yet.
 *
 * Returns true while the body is still in the world.
 */
bool q2_player_death_tick(q2_player_death *d, s16 health, s32 dt,
                          bool deathmatch, u32 roll);

/*
 * The frame cursor walking past the end of the death move — 0x8003DF90's
 * `animflags |= 1`. The client advances the rendered Male2 cursor and reports
 * that edge here. A caller with no usable model can still call it immediately:
 * single player never reads the flag.
 */
void q2_player_death_anim_ended(q2_player_death *d);

/* ------------------------------------------------------------------------- */
/* The two endings                                                            */
/* ------------------------------------------------------------------------- */
/*
 * The single-player abandon clock: 0x800B2A10 counted against 0x800B2A18 by
 * 0x80041D30. Arm it with `q2_player_die` (it sets `abandon_armed`), spend it
 * here, and when it returns true the console has raised game-state 8 and is on
 * its way to the front end.
 *
 * `ticks` is in and out; a `ticks` of zero is "not armed" and stays that way,
 * which is the original's own `if (*(0x800B2A10) == 0) return`.
 */
bool q2_player_abandon_tick(s32 *ticks, s32 dt);

/*
 * The resupply spend, 0x8001FF00..0x8001FF10: `*(u8*)0x800B335D -= 1`, and
 * nothing else in the executable ever writes that byte. It is a byte, so this
 * clamps at zero rather than wrapping to 255 — the page greys itself at zero
 * and takes the row out of the navigation, so the original cannot reach the
 * decrement from empty and the clamp is not a behaviour change.
 *
 * Returns true if a continue was actually spent.
 */
bool q2_player_spend_resupply(int *continues);

#endif /* Q2PSX_GAME_PLAYERDEATH_H */
