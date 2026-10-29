/*
 * sim.h — the fixed-rate game simulation.
 *
 * This is the first piece of actual *game* rather than data plumbing. It runs
 * the world forward in the original's own time units, using constants read out
 * of the executable rather than invented ones.
 *
 * ---------------------------------------------------------------------------
 * Time
 * ---------------------------------------------------------------------------
 * The engine does not think in seconds or in frames. It has a `dt` counter in
 * units of 1/300 s, advanced by a per-field amount (6 on PAL, i.e. 50 fields per
 * second) and clamped so a long frame cannot integrate arbitrarily far. The
 * nominal logic step is 12 dt units, which is 25 Hz — the game simulates at half
 * the field rate and renders every field.
 *
 * That matters for faithfulness. Running the simulation at the display rate, or
 * at a fixed 60 Hz, changes jump arcs and monster timing even if every other
 * constant is right. So the tick is in dt units and the host converts, never the
 * other way around.
 *
 * The clamp (Q2_DT_MAX) is a real behaviour, not a safety net: on a slow frame
 * the original ran the world slower rather than taking a huge step, so physics
 * stayed stable and the game visibly slowed down. A port that instead sub-steps
 * to catch up would be more "correct" and less faithful. We clamp.
 *
 * ---------------------------------------------------------------------------
 * Units
 * ---------------------------------------------------------------------------
 * Everything is world units and integers, per worldscale.h. Positions are s32,
 * velocities are s32 in world units per dt unit scaled by Q2_VEL_DIV. No floats
 * anywhere: the original had no FPU, and its rounding is part of how it feels.
 *
 * ---------------------------------------------------------------------------
 * Collision
 * ---------------------------------------------------------------------------
 * Movement runs through trace.[ch] and collision.[ch], both transcribed from
 * the original rather than inferred. In outline: the zone's cells are EMPTY
 * convex volumes joined by portals, a move is clipped in the cell it starts in
 * and then crosses a portal into the next, and the frame's motion is a lift,
 * a slide and a drop rather than a single sweep.
 *
 * Two things about it are worth knowing before reading this file:
 *
 *   - The hull is SecondaryCol, not PrimaryColl, and it is PrimaryColl eroded
 *     by the player's own 286-unit half-extent. A point moving in it is the
 *     player's cube moving in the world.
 *   - `player.pos` is the FEET; the mover works from the entity ORIGIN, 286
 *     above them. q2_sim_origin_y / q2_sim_feet_y convert.
 *
 * ---------------------------------------------------------------------------
 * Combat
 * ---------------------------------------------------------------------------
 * Firing, damage and projectiles live in weapon.[ch], combat.[ch] and
 * projectile.[ch]; simcombat.c is the only place that knows about all three at
 * once plus the collision hull. What this file contributes is the clock and the
 * trace: the level clock the weapon gates read is this same dt counter, 300
 * units to the second, which is what makes the universal 30-tick refire a tenth
 * of a second.
 *
 * ---------------------------------------------------------------------------
 * What this does NOT do yet
 * ---------------------------------------------------------------------------
 * Creatures are not owned here — a caller registers the actors the player can
 * hit — and their own attack figures are still inside the relocated AI modules
 * (openquestions #6).
 *
 * ---------------------------------------------------------------------------
 * Entities
 * ---------------------------------------------------------------------------
 * Items ARE owned here, because the engine owns them the same way: one entity
 * set, one think sweep, run at the end of the tick so an item's touch test sees
 * where the player actually ended up. `q2_sim_attach_items` spawns the map's
 * resident-zone and LevelBin-selected Population place records; without it the
 * set is empty and the sweep is free. See entity.h and item.h.
 */
#ifndef Q2PSX_SIM_H
#define Q2PSX_SIM_H

#include "collision.h"
#include "combat.h"
#include "effect.h"
#include "entity.h"
#include "events_rt.h"
#include "explosive.h"
#include "item.h"
#include "modelent.h"   /* q2_model_ent_sounds, the spawn's bang */
#include "mover.h"
#include "projectile.h"
#include "scene.h"
#include "trace.h"
#include "trigger.h"
#include "userfuncs.h"
#include "q2psx.h"
#include "weapon.h"
#include "worldscale.h"
#include "world.h"

/* The map's CastList, so an item's model can be resolved at spawn. */
struct q2_model_bank;
struct q2_rotator_set;
struct q2_scene_solid;

/* ------------------------------------------------------------------------- */
/* Input, as the pad delivered it                                             */
/* ------------------------------------------------------------------------- */
/*
 * This is the 12-byte struct 0x80019154 fills and 0x8003A1C8 consumes, not a
 * shape the port chose. Two things about it are worth stating up front:
 *
 *   - The two movement axes are SIGNED PAD AXES at +-Q2_INPUT_FULL, because
 *     the wish velocity is `(maxspeed * axis) >> 7` and that reaches exactly
 *     maxspeed at 128. Feeding it +-1024 makes the player eight times too fast.
 *   - The two look axes are not angle deltas. They are a target TURN RATE,
 *     eased, and the angle integrates from the rate. Feeding a delta straight
 *     in produces a turn that keeps going after the stick is released.
 *
 * There is no crouch here, and that is a finding rather than an omission: see
 * Q2_ENT_INCROUCH in worldscale.h.
 */
/*
 * Every bit is now accounted for. They are produced by 0x80019154's shared tail
 * at 0x80019A04, which turns each of four configurable PAD MASKS into up to
 * three bits — a press edge, a held bit, and a held-and-was-held bit — plus
 * four the arms set directly. See pad.h for which pad button feeds which mask
 * under each of the nine control styles.
 */
#define Q2_BTN_SWIM_UP   0x00200000u /* bit 21, jump mask HELD    @0x8003AB88 */
#define Q2_BTN_JUMP      0x00400000u /* bit 22, jump mask PRESSED @0x8003A918 */
#define Q2_BTN_ATTACK_PRESS 0x00800000u /* bit 23, fire mask pressed          */
#define Q2_BTN_ATTACK_REPEAT 0x01000000u/* bit 24, fire mask held since last  */
#define Q2_BTN_ATTACK    0x02000000u /* bit 25, fire mask held                */
#define Q2_BTN_WEAP_NEXT 0x04000000u /* bit 26, 0x8004ECD8                    */
#define Q2_BTN_WEAP_PREV 0x08000000u /* bit 27, 0x8004ED00                    */
#define Q2_BTN_PAUSE     0x10000000u /* bit 28, START's press edge @0x80019190*/
#define Q2_BTN_LOOK_DOWN 0x20000000u /* bit 29, the look-down button held     */
#define Q2_BTN_LOOK_UP   0x40000000u /* bit 30, the look-up button held       */
#define Q2_BTN_MOVING    0x80000000u /* bit 31, the FORWARD axis is deflected */

typedef struct q2_input {
    s16  forward;     /* -Q2_PAD_FULL..+Q2_PAD_FULL                           */
    s16  side;
    s16  pitch;       /* look axis, NOT an angle delta — see above            */
    s16  yaw;
    u32  buttons;     /* Q2_BTN_*                                             */

    /*
     * Bit 25 of `buttons`, kept as its own field because the fire path takes a
     * bool. q2_pad_read sets both and they cannot disagree.
     */
    bool attack;
} q2_input;

/* ------------------------------------------------------------------------- */
/* Player state                                                               */
/* ------------------------------------------------------------------------- */
/* Four pads, four viewports, four players — the same bound multiplayer.h uses
 * and the same one the engine's own `killer < 4 && victim < 4` guard enforces. */
#define Q2_SIM_MAX_PLAYERS 4

/* Selected by the client's death handler before a tick. A library-only caller
 * can keep LIVE: Q2_ENT2_DEAD selects CORPSE automatically. This is derived
 * runtime state, not an additional field in the saved-game format. */
typedef enum q2_player_motion {
    Q2_PLAYER_MOTION_LIVE = 0,
    Q2_PLAYER_MOTION_CORPSE,  /* 0x80039550: brake, then move */
    Q2_PLAYER_MOTION_BODY,    /* 0x8003E238: move without braking */
    Q2_PLAYER_MOTION_NONE     /* fade, dissolve, gibbed or freed */
} q2_player_motion;

typedef struct q2_player {
    q2_player_motion motion;
    /* The client's death chain raises DEAD later than the sim's input gate.
     * Until then a corpse still takes the mover's body-separation pass. */
    bool corpse_separate;
    s32  pos[3];        /* world units                                        */
    s32  vel[3];        /* world units per dt, pre-divide by Q2_VEL_DIV       */

    /*
     * The view angles at entity+0xE6: pitch, yaw, roll in the 4096-step circle.
     * `yaw` and `pitch` are kept as their own names because everything outside
     * the sim reads them, and `roll` is derived from the wish velocity rather
     * than from input.
     */
    s32  yaw, pitch, roll;

    /* FindPlayerTarget's client+0x3C direction and +0x44/+0x46 barrel pitch.
     * Acquire/ease once per live logic tick; every weapon substep consumes
     * the same direction. The last published GameWindow position/cell is
     * separate from the player's current, possibly already moved, eye. */
    s16 aim_direction[3], aim_pitch, aim_assist;
    s32 aim_eye[3], aim_node;
    bool aim_ready, aim_view_valid;

    /*
     * The wish velocity at client+0x04: side, vertical, forward. The engine
     * eases these toward the stick and then rotates them into `vel`, so this is
     * the state that carries a player's momentum intent across ticks.
     */
    s16  wish[3];

    /* The look rates at client+0x0A and client+0x0C. The angles integrate from
     * these; the stick only asks for a rate. */
    s16  pitch_rate, yaw_rate;

    /*
     * The impulse accumulator at entity+0x2F8 and its arming bit. Everything
     * that wants to change velocity from outside the integrator posts here.
     */
    s16  impulse[3];
    bool impulse_armed;

    /*
     * The frame delta at entity+0xEC, and the reason it has to be state.
     *
     * The mover's three moves consume this and the integrator writes it, in that
     * order, inside one function (0x80045B70 reads it, 0x800461DC writes it). So
     * a tick moves by the delta the PREVIOUS tick computed. Keeping it in the
     * player rather than as a local is what reproduces that one-tick offset.
     */
    s16  frame_delta[3];

    s16  jump_hold;     /* client+0x20                                        */
    s32  view_height;   /* entity+0xF6, eased toward the flags' target        */
    bool on_ground;
    bool crouching;     /* derived from the env flags, not from input         */
    s32  ground_y;      /* world Y of the surface under the player            */

    /*
     * The entity's SECOND flag word, entity+0x10C. Three of its bits reach the
     * movement code and they are not interchangeable with the +0x98 word above:
     *
     *   0x1000  set from the GAME VARIABLES flag at 0x800B2AA4 (0x8003A41C).
     *           It turns gravity OFF at 0x80045EE0, blocks the jump at
     *           0x8003A8F0, blocks the view recentre and the water-exit jump —
     *           i.e. it is the fly cheat, not merely a movement-basis switch.
     *   0x4000  the impulse accumulator is armed; `impulse_armed` mirrors it.
     *   0x8000  set by the item path; not read by movement.
     */
    u32  ent2_flags;

    /*
     * The view recentre, 0x8003A780. Not a setting and not a key: holding the
     * two LOOK buttons together for a second frame arms it, and it then walks
     * the pitch to zero and disarms itself.
     */
    u8   look_hist;     /* client+0x24, a shift register of "a look button
                         * was held", one bit per tick                       */
    bool recentring;    /* client+0x25                                       */
    s16  autocentre;    /* client+0x26, cleared whenever the pitch axis moves */

    /* Fall damage's own outputs: a view kick and how long it lasts. */
    s16  fall_value;    /* client+0x9A, in the 4096 circle                    */
    s32  fall_time;     /* client+0xD4, a level-clock deadline                */

    /*
     * Water. The five fields 0x8003D254 keeps, which is the LAST call the
     * player's frame makes (0x8003B00C). Entering and leaving water are edges;
     * remaining submerged is a life-support clock with its own armour-bypassing
     * damage path.
     *
     * `wade` counts consecutive ticks in shallow water and exists only so the
     * splash fires on the FIRST one; `water_air` is non-zero while submerged
     * and is what makes leaving water an edge. While submerged it accumulates
     * the engine's dt, is reset to one by a live rebreather, and scales the
     * periodic drowning damage after its 300-tick deadline expires.
     */
    s32  wade;          /* client+0x80                                       */
    s32  water_air;     /* client+0x84                                       */
    s32  water_next;    /* client+0x88, next breath/drowning pass            */
    s32  splash_time;   /* client+0xD8, a level-clock deadline                */
    bool water_voice;   /* client+0xDF, every-other drowning pass emits sound */

    /*
     * The other two view kicks 0x80038260 composes, kept here because it reads
     * all three out of the client record:
     *
     *   `kick` / `kick_time`   client+0xA0..0xA4 and +0xCC — pitch, yaw and
     *                          roll, decaying over 30 ticks. What a shot posts.
     *   `hurt_kick` / `pain_time`  client+0x9C, +0x9E and +0xD0 — pitch and
     *                          roll scaled over 150. Native damage preserves
     *                          the amplitudes; they begin at zero.
     *
     * `pain_time` is genuinely one field doing two jobs: 0x8003AF50 also uses
     * it as the pain SOUND throttle and sets it to now+210, which is longer
     * than the kick's own 150 — so for the first 60 ticks the kick is scaled by
     * more than one rather than less. That is the console's arithmetic, not a
     * bug in this transcription.
     */
    s16  kick[3];
    s32  kick_time;
    s16  hurt_kick[2];
    s32  pain_time;

    /*
     * entity+0x10A and client+0x50: last tick's health and armour, compared at
     * 0x8003AE10 to decide whether the player was hurt this tick. The engine
     * has no "was damaged" flag — it diffs these two, which is why a heal and a
     * hit in the same tick cancel and make no sound.
     */
    s16  prev_health, prev_armour;

    /* The native JUMP request is the successful impulse, not airborne state.
     * Transient output of this player's most recent game tick. */
    bool jumped_this_tick;

    s32  footstep_time; /* client+0x8C                                        */
    int  foot;          /* client+0x90, which of the two sounds is next       */

    /*
     * Which control style the pad is configured for, 0..8 — see pad.h. Styles
     * BELOW Q2_PAD_STYLE_EASED_FROM set the look rate outright and styles from
     * it up ease it (0x8003A670 is `slti style, 6` branching to the eased arm
     * when the test FAILS). The port had this comparison inverted, which made
     * the mouse and stick styles glide and the digital ones snap.
     */
    int  look_scheme;

    /*
     * The mover's own state, carried across ticks exactly as the original
     * carries it in the entity record: the cached collision cell at +0x4E, the
     * flags word at +0x44, the two contact normals, and the slope limit the
     * ground test compares against.
     */
    q2_move_ent  ent;
} q2_player;

/* ------------------------------------------------------------------------- */
/* Combat state                                                               */
/*                                                                            */
/* The level clock the fire functions gate on is the same dt clock the sim     */
/* already runs: 300 units to the second, which is what makes the universal    */
/* 30-tick refire a tenth of a second. So there is one clock here, not two.    */
/*                                                                            */
/* Creatures are NOT owned by the sim. A caller registers an array of actors   */
/* and the sim shoots at them, so the same combat code serves the client, the  */
/* offline `walk` harness and a test with three actors in a line.              */
/* ------------------------------------------------------------------------- */
typedef struct q2_sim_combat {
    q2_inventory    inv;
    q2_combat_rules rules;
    q2_rng          rng;
    q2_projectiles  projectiles;

    int  weapon_id;         /* 1-based, 0 for "no weapon"                    */
    s32  next_fire;         /* level tick the refire gate opens              */
    s16  kick[3];           /* the last shot's view kick, for the renderer   */

    /*
     * How many bullets the chaingun spends per shot. The console reads this
     * from the weapon's spin state (0x8004CAE0 loads it from the view model's
     * runtime object at +0x2C); the view model is not reconstructed yet, so
     * the port holds it here and defaults to one.
     */
    int  chaingun_bullets;

    /* Registered by the caller. */
    q2_actor **targets;
    u32        target_count;

    /* The player as something that can be hurt. Kept in step with `inv`. */
    q2_actor   self;

    /* The last shot, so a caller can draw tracers and play the sound. */
    q2_fire_result_v2 last_shot;

    /*
     * How many times `last_shot` has been written, so a caller can tell a NEW
     * shot from the same one read twice.
     *
     * It is needed because `last_shot` is a latch, not an event: it is written
     * when the trigger is pulled and never cleared, so `fired` stays true from
     * the last shot until the next pull. A caller playing a sound off `fired`
     * alone therefore played it again on every frame after the trigger was
     * released — one shot, and then that shot forever.
     *
     * The frame rate made it worse rather than causing it. A fire attempt
     * happens on a TICK and the client reads this every rendered FRAME, so even
     * while the trigger was held each shot was heard once per frame between
     * ticks — three times over at 90 fps.
     */
    u32 shot_serial;
} q2_sim_combat;

/*
 * The half of `q2_sim_combat` that belongs to a PLAYER rather than to the world.
 *
 * `rules`, `rng`, `projectiles`, `targets` and `target_count` are the world's:
 * one list of bolts in flight, one set of things that can be hurt, one set of
 * rules. Everything below is one player's, and four players need four of them.
 *
 * They are swapped in and out of `q2_sim.combat` around a player's tick rather
 * than being addressed through an index, because `sim->combat.inv` appears
 * eighty-six times across the game, the client and the tests, and every one of
 * those sites means "the player whose frame is running" — which is exactly what
 * the swap makes true. `cur_player` selects which, the same way it does for
 * `player[]`.
 */
typedef struct q2_player_combat {
    q2_inventory      inv;
    int               weapon_id;
    s32               next_fire;
    s16               kick[3];
    int               chaingun_bullets;
    q2_actor          self;
    q2_fire_result_v2 last_shot;
    u32               shot_serial;   /* swapped with it; see q2_sim_combat */
} q2_player_combat;

/* ------------------------------------------------------------------------- */
/* Simulation                                                                 */
/* ------------------------------------------------------------------------- */
#define Q2_SIM_MAX_BREAKABLES 48   /* the console's object array is 48 */

/*
 * The two primitives that own a damageable box, and they do different things
 * when it runs out of hit points:
 *
 *   GLASS      throws debris — a burst per hit and a shatter at the end.
 *   SHOOTTHEN  RUNS ITS OWN RECORD. `0x8002E81C` frees the box and calls the
 *              record dispatcher (0x80027950) with the offset its constructor
 *              cached in obj+0x40, so the primitive is a shoot-to-activate
 *              switch: shoot the panel, and whatever the rest of that record
 *              does happens. Four calls on the disc.
 */
typedef enum q2_breakable_kind {
    Q2_BREAKABLE_GLASS = 0,
    Q2_BREAKABLE_SHOOTTHEN,
    /*
     * A SHOOTABLE DOOR OR BUTTON, and they are the same thing.
     *
     * Not a CALL primitive at all — it is a plain MOVER_A whose s16 at +20 is
     * non-zero. Its load handler at 0x80025D24 then installs the damage
     * callback 0x8002F050 at object+0x24 and flags the object's box with bit
     * 0x4, and bit 0x4 is the only thing a weapon impact gates on. 14 such
     * panels exist, across ten maps.
     *
     * The port used to decode that halfword as a boolean `touch_opens` and
     * read it nowhere, and the box registry only ever scanned CALL items — so
     * there was no path from a shot to a mover at all and a shootable button
     * did nothing.
     *
     * BUTTON and ROTBUTTON, the two CALL primitives with the word in their
     * name, are NOT shootable: neither constructor allocates a box or installs
     * a +0x24 callback. A shot correctly does nothing to those.
     */
    Q2_BREAKABLE_MOVER,

    /*
     * OPCODE 0x08 — the `func_explosive`, and the biggest of the four families
     * by a long way: 224 items in the disc's zone scripts against GLASS's ten
     * calls.
     *
     * Neither a CALL nor a mover. Its constructor at 0x80026A20 allocates a box
     * PER INTACT NODE — up to four, all naming the same item — so one authored
     * group registers as several entries here and `part` says which. Whichever
     * of them takes the fatal shot destroys the whole group, because the hit
     * points live in the ITEM and not in the box.
     *
     * The state, the geometry swap and the effects live in explosive.h; this
     * array owns only the box the shot trace tests.
     */
    Q2_BREAKABLE_FXGROUP
} q2_breakable_kind;

typedef struct q2_breakable {
    s32 scene_node;
    s32 bmin[3];        /* the box the shot is tested against                */
    s32 bmax[3];
    s16 health;         /* item[+6], which the console mutates IN THE ITEM   */
    u8  count_a;        /* GLASS item[+10]: pieces per hit, from the point   */
    u8  count_b;        /* GLASS item[+12]: pieces on shattering, box-wide   */
    u8  kind;           /* q2_breakable_kind                                 */
    /* FXGROUP: which of the item's four intact nodes this box is, and which
     * entry of the explosive set owns it. The set is the caller's, so the
     * index is only meaningful while that set is attached. */
    u8  part;
    s16 owner;
    u32 record_offset;  /* SHOOTTHEN: the record to run — obj+0x40           */
    /* MOVER: the event item the leaf was built from, which is the identity
     * q2_movers_trigger_item keys on. */
    u32 item_offset;
    /* GLASS/SHOOTTHEN: index of its live entity box in `volumes`.
     * Movers are inserted before it, so this is rebased whenever that prefix
     * is rebuilt. -1 for breakables that never block movement. */
    s32 solid_target;
    bool broken;
} q2_breakable;

typedef struct q2_sim {
    const q2_world_zone *zone;
    /*
     * The players. One sim, one WORLD — its entities, its script, its items,
     * its effects — and up to four players inside it, which is what makes a
     * shared world possible at all: four separate sims each spawn their own
     * copy of the map's items and run their own script, so nothing they do can
     * be seen by anyone else.
     *
     * `cur_player` is which one the player half of a tick is running. It is set
     * by `q2_sim_advance_player` and left alone otherwise, so every existing
     * caller sees player 0 and behaves exactly as before.
     */
    q2_player            player[Q2_SIM_MAX_PLAYERS];
    int                  cur_player;
    int                  player_count;
    q2_sim_combat        combat;

    /*
     * Each player's half of the above, parked while another player's frame
     * runs. Slot `cur_player` is the one currently live in `combat`.
     */
    q2_player_combat     pcombat[Q2_SIM_MAX_PLAYERS];

    /* Everything a projectile can hit; see q2_sim_set_world_targets. */
    q2_actor           **world_targets;
    u32                  world_target_count;

    /* The level clock the weapon gates and the damage throttles use, in dt
     * units. Advanced by q2_sim_tick alongside the physics. */
    s32                  level_time;

    /*
     * THIS tick's dt, in the same units — the port's stand-in for the global
     * frame delta at 0x800B2DB4. Set by q2_sim_tick beside the clock, for the
     * world half of the tick, which does not take dt as an argument and needs
     * it anyway: a projectile advances by `vel * dt`, not by `vel`.
     */
    s32                  cur_dt;

    /*
     * Whether the TICK may fire the weapon from `input->attack`.
     *
     * False for a caller that owns a view-weapon machine, because on the
     * console the machine is the only thing that calls a fire function. True
     * for one that does not — the offline harness, the tests — so a shot can
     * still be made without an animation to drive it.
     */
    bool                 fire_from_input;

    /*
     * The hull entities move in is SecondaryCol, not PrimaryColl. The zone
     * loader builds two contexts and the mover at 0x80045144 loads the second
     * one (0x800C8FE8, set from "SecondaryCol" at 0x8007B648). PrimaryColl is
     * kept alongside because everything that is not movement — AI, line of
     * sight, spawn validation — uses it through 0x800C8E90.
     */
    q2_collision coll;          /* SecondaryCol: the movement hull            */
    bool         coll_ready;
    q2_collision coll_primary;  /* PrimaryColl: queries other than movement   */
    bool         coll_primary_ready;
    s32          current_node;  /* which cell the player is in, -1 if unknown */

    /* Trigger volumes and the script they fire. Both are optional: a zone with
     * neither still simulates, it just has no gameplay. */
    q2_triggers  triggers;
    bool         triggers_ready;
    q2_events    events;
    q2_event_rt  event_rt;
    bool         events_ready;

    /* Which triggers the player was inside last tick, so a volume fires on
     * ENTRY rather than every tick while standing in it. One bit per trigger. */
    u8          *trigger_inside;
    u32          trigger_capacity;

    /*
     * The environment mask each volume asserts while you are standing in it —
     * the port's form of the volume dispatcher at 0x80027E64.
     *
     * INWATER, UNDERWATER, INCROUCH, INLOWCROUCH, INLAVA and DONTJUMP are
     * `UserFuncs` primitives whose whole body is one OR into entity+0x98. So
     * rather than execute a volume's record 25 times a second to reach six
     * one-line functions, this scans each record's CALL items ONCE at attach
     * and keeps the bits they would set.
     *
     * That is a divergence in mechanism and not in behaviour, and it is the
     * honest one: a level-triggered re-execution would also re-run whatever
     * else the record does, which is not what the dispatcher's predicate arm
     * amounts to.
     *
     * NULL when the map has no UserFuncs table, in which case the flags fall
     * back to `env_flags` alone.
     */
    u32         *volume_env;

    /*
     * And what a volume HURTS you for, read the same way and applied on the
     * same pass.
     *
     * Five primitives are damage volumes and their bodies are as short as the
     * environment ones: `INACID` is one point at mod 9, `INLAVA` twenty at mod
     * 10, the two `UNDER` variants are the same amounts with an extra flag, and
     * `LASERWALL` carries its amount in its own operand at mod 11. Read out of
     * 0x8002E49C, 0x8002E4C8, 0x8002E500, 0x8002E53C and 0x8002E1F0.
     *
     * They are LEVEL-triggered — asserted every tick you stand in the volume,
     * not once on entry — and that is not a choice: the damage function keeps a
     * per-target throttle for mods 9 and 10 (`env_next`, 400 and 100 ticks),
     * and a throttle only means something if the call repeats. Firing once on
     * entry would make lava a single point of damage you could stand in.
     *
     * The attacker is NOBODY, which is the `a0 = zero` at every one of those
     * call sites, and is why drowning in lava is not a frag for anyone.
     */
    s16         *volume_damage;
    s16         *volume_mod;

    /*
     * Entries into shallow water — 0x8003D4C4, one GenerateWaterSplash call
     * each. Counted because the splash is otherwise only visible on screen, and
     * a headless run is where the port is measured.
     */
    u32          water_splashes;

    /* The map's breakable panes, and which collision node each one occupies. */
    q2_breakable breakable[Q2_SIM_MAX_BREAKABLES];
    u32          breakable_count;
    u32          breakable_hits;   /* shots that reached one                 */
    u32          breakable_pieces; /* and the debris they threw              */
    u32          breakable_fired;  /* SHOOTTHEN records raised by a shot     */
    /*
     * Shootable leaves whose hit points reached zero this tick, by event-item
     * offset. Queued rather than opened here: the mover set is the caller's.
     */
    u32          breakable_open[8];
    u32          breakable_open_count;

    /*
     * THE `func_explosive` SET, borrowed the way the mover set is.
     *
     * It is the caller's because destroying a group changes which Scene nodes
     * DRAW, and the hide array belongs to the client (world.h). The sim holds
     * the pointer so that `q2_sim_breakable_shot` — which is five frames deep
     * in the weapon path and cannot grow an argument — can reach it.
     */
    struct q2_explosive_set *explosives;

    /*
     * The map's CastList, so a detonation can bind the `Explosion` MODEL
     * ENTITY (modelent.h). Borrowed, and set by whichever attach was given
     * one; NULL simply means no effect model, which is what a map with no such
     * entry gets anyway.
     */
    const struct q2_model_bank *model_bank;

    /*
     * Nodes whose visibility changed this tick, drained by the owner into its
     * hide array. Hide and show both, because a destroyed group swaps one
     * group of nodes for another.
     *
     * Queued rather than written for the reason mover.h gives about `sealed`:
     * the hide array has a second writer already — the script's OBJDRAWOFF —
     * and an unconditional per-tick write from here would undo it.
     */
    struct { s16 node; u8 hidden; } node_vis[32];
    u32          node_vis_count;

    u32          explosive_destroyed;  /* groups that came apart      */
    u32          explosive_blasts;     /* ...of those, ones that blew */
    u32          explosive_models;     /* ...and ones that spawned a model */

    /*
     * Rockets and grenades that went off, and how many of those bound the
     * `Explosion` model — PrimaryRocketActive (0x8004ADF0), both grenade
     * thinks (0x80049F9C/0x8004A01C, 0x8004A8E0/0x8004A9B0). Each spawn is
     * also a SetShockData for every pad (modelent.h).
     */
    u32          projectile_blasts;
    u32          projectile_models;

    /*
     * WHERE each detonation happened, so the owner can place `wep_grenlx1a`.
     *
     * 0x8002695C plays it through the by-handle path at 0x80073704 with the
     * node's box centre as the position — the same argument the explosion
     * itself is given. Queued rather than played, exactly as a mover's
     * transition sound is (mover.h): the sim knows where the blast was and the
     * client owns the mixer and the map's sound bank.
     *
     * Four, because one item has at most four intact parts and each detonates.
     */
    s32          blast_at[Q2_EXPLOSIVE_MAX_PARTS][3];
    u32          blast_count;

    /*
     * AND EACH EXPLOSION MODEL'S OWN BANG, which is a second sound and not
     * the same one twice by accident.
     *
     * Every spawn that binds the `Explosion` model calls SoundByCreature
     * (0x8005AA54) with Sound_Explosion — `wep_grenlx1a` again, through
     * PreInitExplosion's handle at 0x800AEA9C (modelent.h). The rockets and
     * grenades have no other: their thinks never play one themselves, so
     * without this queue they went off silently.
     *
     * A DESTROYABLE BRUSH PLAYS IT TWICE. PSXEXPLODE calls the spawn at
     * 0x80026950 and then, without looking at what it returned, its own
     * report at 0x8002695C — the handle at 0x800B27F8, which
     * InitEventSoundsEachLevel resolved from the same key in the same table,
     * so both name the same request. Each start takes its own channel from
     * GetFreeChannel (0x800740DC) and nothing on that path or in
     * ProcessSounds' 24-voice walk compares one voice with another; neither
     * request has a random pitch range (SetRandomPitch's only weapon callers
     * name wep_machgf1b). So in a map that carries `Explosion` a brush
     * detonation starts two identical voices at the same point in the same
     * frame, and the report stays queued on `blast_at` beside this. In one
     * that does not, the spawn bails and only the report plays.
     *
     * Filled by q2_model_ent_spawn itself, so every path to it — the brush,
     * the missiles, and the gib dispatcher's boss arms through q2_gib_world's
     * `sounds` — lands here. Drain with q2_sim_next_explosion_sound.
     */
    q2_model_ent_sounds explosion_sounds;
    /* Grenade2/3 geometry contact, wep_grenlb1b at the rebound position. */
    q2_model_ent_sounds grenade_bounce_sounds;

    /*
     * AND WHERE EACH PANE SHATTERED, so the owner can play Q2_GLASS_SOUND.
     *
     * The same arrangement, for the same reason, one handler along: 0x8002A4A4
     * plays the handle at 0x800B27FC (`Sound_GlassSmash`) through 0x80073704 at
     * the centre of the pane's Scene node box, which 0x8002A3E4..0x8002A4A0
     * builds on the stack immediately before the call. Every breaking window in
     * the game was silent in this port until this queue existed — the sample is
     * in the maps' own banks (BASE1's SNDVRAM entry 29 is `amb_brkglas`) and
     * nothing ever asked for it.
     *
     * Eight is an arbitrary depth. The console plays the sound the instant it
     * makes it and cannot drop one, so all this bound decides is how many panes
     * may break in a single tick before a smash is lost; a ninth is dropped
     * exactly as a thirty-third node_vis change is.
     */
    s32          glass_at[8][3];
    u32          glass_count;

    /* The scene the panes' nodes belong to, kept so the hitscan path can throw
     * their debris without every shot carrying a scene pointer. */
    const struct q2_scene *breakable_scene;

    /* How many hazard hits actually LANDED — the throttle blocks most of the
     * calls, so this is the number that says the rate is right rather than the
     * number of times the volume was tested. */
    u32          hazard_hits;

    q2_userfuncs userfuncs;
    bool         userfuncs_ready;

    /*
     * The sweep and contents target list, in the ORDER the original walks it:
     * the 48-slot ENTITY table at 0x800CAE10 first, then the map's trigger
     * volumes at 0x800C9114. One allocation, because `q2_move_world` takes one
     * array and because the order is what decides which contact survives — the
     * sweep keeps the LAST one, not the nearest.
     *
     * `mover_count` entries at the front are mover PARTS: one box per Scene
     * node a door or lift translates, refreshed every tick from the mover's
     * accumulated offset. Before this the array held volumes only, nothing ever
     * produced a Q2_MOVE_KIND_ENTITY target, and a closed door was scenery the
     * player walked through.
     */
    q2_move_target *volumes;
    u32             volume_count;   /* all entity boxes + trigger volumes      */
    u32             mover_count;    /* how many of them are mover parts        */
    u32             breakable_solid_count; /* GLASS/SHOOTTHEN after movers      */
    /* ROTHATCH, OBJDRAWOFF and every intact FXGROUP part follow breakables.
     * Their indices are implicit, so rebuilding either prefix preserves them. */
    struct q2_scene_solid *scene_solids;
    u32                   scene_solid_count;

    /*
     * The mover parts' PRISTINE boxes, six s32 each, parallel to the first
     * `mover_count` entries above. The live box is the pristine one plus the
     * mover's accumulated offset, recomputed each tick rather than integrated,
     * because the DRAW adds `m->offset` to the same node and a hull that
     * accumulated its own rounding would drift away from the geometry it is
     * supposed to be.
     */
    s32            *mover_base;
    s32            *mover_last_off; /* previous tick's offset, for `dy`        */

    q2_move_world   move_world;

    /*
     * THE ACTOR LIST — 0x800B2B90..0x800B2B98, and it is not the box table.
     *
     * A 32-slot array of live entity pointers, double-buffered and swapped each
     * frame at 0x8005525C; `0x800552B4` appends to it and the stepped mover
     * 0x8004583C appends the entity it is about to move (0x80045990) before
     * running the separation pass over it (0x80045998). That pass is what makes
     * a creature something a player cannot walk through, and the port had
     * nothing corresponding to it — `move_world` above is doors, glass and
     * trigger brushes, and no creature was ever in it.
     *
     * Rebuilt each tick from the sim's own players and from whatever the owner
     * has registered with `q2_sim_set_bodies` (the creatures; the client keeps
     * them). Owned here rather than borrowed because the players have to go in
     * it and only the sim has them.
     */
    q2_move_body   *bodies;
    u32             body_count;
    u32             body_capacity;
    q2_move_bodies  body_world;

    /* What the owner registered, kept so the rebuild can copy it. Borrowed. */
    const q2_move_body *extra_bodies;
    u32                 extra_body_count;

    /*
     * Take up a better weapon the moment it is picked up.
     *
     * ON by default and a deliberate deviation from the disc, which only
     * switches out of the blaster (0x80037E78). False restores that rule
     * exactly. See q2_sim_give_weapon for what "better" means — it is the
     * console's own preference list at 0x8009DB7C, not a new one.
     */
    bool         autoswitch;

    /* A capture aid: the player takes no damage. See q2_sim_hurt_player. */
    bool         invulnerable;

    /* Set when a zone gate fires; the caller performs the load. */
    bool         zone_change_pending;
    u32          zone_change_target;

    /*
     * `--zone-trace`. Names the trigger volume the player crossed, because the
     * client can see that a gate fired but not WHERE from, and "the gate is
     * being raised by the wrong volume" and "the gate is being answered in the
     * wrong place" are two different faults with one symptom.
     */
    bool         trace_zone;
    u32          trace_last_trigger;   /* the last volume entered            */
    s32          trace_last_box[6];    /* its min[3] then max[3]             */

    s32  dt_accum;      /* leftover dt units not yet consumed by a tick       */

    /*
     * AND THE SUB-UNIT REMAINDER, which the accumulator used to throw away.
     *
     * `(s32)(elapsed * 300)` truncates. A frame shorter than 1/300 s adds
     * ZERO, so above 300 fps the accumulator never reaches Q2_DT_NOMINAL and
     * the world never steps at all — and between the two, a fixed fraction of
     * every frame is lost. The front end is the one scene light enough to
     * cross that cliff: QFRONT is two nodes and eight vertices and runs at
     * around 560 fps in a window, where a BASE1 session runs at about 31.
     * That is why the title logo stuttered and nothing in a level did.
     *
     * Carried across ticks and NEVER cleared. Clearing it alongside dt_accum
     * would discard up to a whole 1/300 s per tick — most of the error this
     * exists to remove.
     */
    double dt_frac;

    /*
     * The frame counter, the port's [0x800B2DE4]: +1 per WORLD tick whatever
     * the tick's dt (0x800705C4 `addiu a1, a1, 1`, stored at 0x80070610). It
     * is NOT the level clock, which moves by dt. The damage-effect drawers key
     * their vertex phase on it (`& 1`, `& 7`; effect.h), so a clock standing in
     * for it holds one phase forever at an even dt.
     *
     * Zero from q2_sim_init. `xrefs 0x800B2DE4` finds three zeroing stores in
     * the main executable besides the increment: 0x800708C0 (in 0x80070748,
     * beside the entity counters 0x800B2D04/0x800B2D08; called from 0x8006E09C
     * and at 0x80070E6C, immediately before 0x80070E74 calls 0x80070C94),
     * 0x8007C9F0 (in 0x8007C938, called from 0x80070C94) and 0x8007CA94, the
     * last two beside the frame-delta accumulator 0x800B2E24. That these are
     * the level start is INFERRED from the company they keep. The counter's
     * address is also exported at 0x80079AAC, so a relocated module could
     * write it as well; none is known to. q2_sim_settle's ticks advance it
     * too, and the console has no settle, so the absolute phase after a spawn
     * is the port's own. Only the step matters to the look.
     *
     * Incremented at the TOP of the world half of q2_sim_tick, because that is
     * where the console's frame counts it: 0x80070490 (called at 0x80038D44)
     * stores the new value before 0x8006A4F0 (called at 0x80038D4C) runs the
     * entity thinks that present each actor. So a tick presents with the
     * count that includes itself, as a console frame does.
     */
    u32  tick_count;
    /* Calls to 0x8003DF0C, shared by all players. Retail's counter at
     * 0x800AE898 starts at 3; storing calls since startup (zero) gives the
     * same first/fifth/ninth sound with (calls++ & 3) == 0. The client carries
     * it across every load, including saved games; it is not card state. */
    u32  lava_burn_calls;
    s32  dt_per_field;  /* 6 on PAL, 5 on NTSC — the build's field rate       */

    /*
     * The build's own frame, VSync(2) of those fields: 12 on PAL and 10 on
     * NTSC. The smallest step the world takes — the accumulator waits for this
     * much before ticking. Fixed at init from the video standard alone, so the
     * GAME SPEED variable, which rewrites dt_per_field, does not move it.
     */
    s32  frame_dt;

    /*
     * Downward acceleration. A constant in the original *until* the GAME
     * VARIABLES menu exists: 0x8001C6D0 recomputes the global at 0x800AE924
     * as (slider + 64) >> 2 whenever the variables are enabled, and writes
     * Q2_GRAVITY back when they are not. Initialised to Q2_GRAVITY, so a caller
     * that never opens the menu sees exactly the previous behaviour.
     */
    s32  gravity;

    /*
     * BLAST FORCE, the impulse scale's input, 0x800B3358. The damage function
     * reads it raw — `lh 0x800B3358` at 0x80057F84, then 125*(v+64)>>6 for an
     * ordinary hit and 25*(v+64)>>2 when a player hurt themselves — so unlike
     * GRAVITY and GAME SPEED it is NOT transformed by 0x8001C698; the menu's
     * store at 0x8001BE38 puts the slider's own value in the halfword.
     *
     * Initialised to 64, which is what the reset routine writes (0x80020494
     * `addiu v1, zero, 64` / 0x80020498 `sh v1, 884(v0)`), so a caller that
     * never opens the menu gets the disc's scale of 250 / 800 rather than the
     * 125 / 400 a zeroed struct would give.
     */
    s32  blast_force;

    /*
     * Environment flags asserted by the CALLER, OR'd in alongside whatever the
     * map's own volumes assert (`volume_env`).
     *
     * This used to be the only source, because the port could not resolve a
     * volume's record to its UserFuncs primitive. It can now, so a map with
     * water and crouch volumes crouches and swims on its own and this is what
     * is left: a way for a test, a tool or a debug key to hold a state the
     * geometry does not.
     */
    u32  env_flags;

    /*
     * 0x800AEBCC: zero in single player. It gates the -3072 impulse ceiling and
     * the end-of-frame basis rebuild, so it is real behaviour rather than a
     * mode flag the port invented.
     */
    bool multiplayer;

    /*
     * 0x800B2AA4, a GAME VARIABLES setting. Non-zero routes all movement through
     * the full view-basis rotation instead of the yaw-only path, which lets pitch
     * steer horizontal movement.
     */
    bool full_basis_movement;

    /* 0x800B29EC & 0x40 — when set, fall damage is suppressed. */
    bool no_fall_damage;

    /*
     * 0x800B3360 — the WEAPON STAY row on the GAME VARIABLES page, and a
     * halfword of its own rather than a bit of the cheat word: the page's row
     * table points straight at it (0x8009A748, 0x8009A808, 0x8009A8C8 all hold
     * the address), 0x8001BE5C and 0x800204B4 zero it, and the two readers
     * fetch it with `lh` — 0x80037E60 inside the weapon-give helper and
     * 0x8005988C inside the touch fold. gamevars.h records the same split.
     *
     * Kept beside `cheats` rather than in it for exactly that reason, and
     * carried UNGATED by the multiplayer flag because the console's menu writes
     * the halfword directly; both readers require deathmatch themselves.
     */
    bool weapons_stay;

    /*
     * 0x800B3342 — the AUTOCENTRE row on the player page, and until now the one
     * shipped setting with nothing on the other end of it.
     *
     * It gates the timer at 0x8003A4BC that pulls the view level while you walk
     * (see `q2_player.autocentre`). Single player only: 0x8003A93C skips the arm
     * outright in deathmatch.
     */
    bool autocentre_setting;

    /*
     * 0x800B29EC itself, the GAME VARIABLES word the pause menu writes (see
     * gamevars.h). `no_fall_damage` above is bit 0x40 of it, kept separate
     * because the movement code reads it every tick; the item dispatch reads
     * bit 0 from here instead of being handed a second copy.
     */
    u32  cheats;

    /*
     * The entity set and the world its thinks run in — items today, and
     * whatever else moves onto the think pointer next.
     *
     * The sim owns it rather than a caller because the touch sweep needs the
     * player's inventory and position every tick, and those live here. It is
     * empty until q2_sim_attach_items is called, and a sim without it runs
     * exactly as before: q2_entity_run over an empty set does nothing.
     *
     * `ent_world.level_time` is kept in step with `level_time`, not maintained
     * separately — one clock, as in the original.
     */
    q2_entity_set   entities;
    q2_entity_world ent_world;
    bool            entities_ready;

    /*
     * Population place groups keep the same two-bit lifetime as creature
     * groups: selected, then run. The chunk and tables are borrowed from the
     * current map; `item_group_run` owns one byte per Population group and is
     * the bit-1 shadow that stops CREBATCH from duplicating a place list.
     */
    q2_population                  item_population;
    const q2_item_table           *item_table;
    const struct q2_model_bank    *item_bank;
    u8                            *item_group_run;
    u32                           *item_group_order;
    u32                            item_group_order_count;
    bool                           item_population_ready;

    /*
     * Set while q2_sim_settle is driving the tick, and read by the tick to
     * hold the ENTITY SWEEP back.
     *
     * The settle is the port's, not the console's — the engine drops a player
     * onto the floor with no ticks at all — so anything the sweep does during
     * it is a thing the game did before the player saw a frame. For items that
     * is not a nuance: a start position within 286 units of a pickup had it
     * COLLECTED during the settle, and because the event queue is cleared at
     * the top of the next tick the sound, the particle burst and the caption
     * were all thrown away with it. The item was simply gone on frame 0.
     *
     * Holding the sweep back for those ticks puts the pickup back on the first
     * tick the player actually plays, which is where the console has it. It
     * also costs the item thinks their settle ticks — the spin, the glow and
     * the materialise ramp — which is the same argument: those ticks do not
     * happen on the console either.
     */
    bool            settling;

    /* The entity set holds QFRONT's title-screen objects rather than a map's
     * items, so `q2_sim_scene_page` has something to address. See
     * q2_sim_attach_scene. */
    bool            scene_ready;

    /*
     * The presentation layer.
     *
     * It lives in the sim rather than in the client for the reason the original
     * puts it on the entity list: an effect is spawned by a gameplay event —
     * a rocket detonating, a creature bleeding — and integrated by the same
     * clock as everything else. A headless caller gets the bursts and their
     * motion without a screen; the client only has to draw them.
     *
     * `fx.tab` is NULL until q2_sim_attach_effects is called with the tables
     * out of the executable, and a sim without them spawns nothing: no ramp
     * means no colour, and inventing one would be the port making up an effect
     * the console does not have.
     *
     * `fx_rng` is separate from the weapon generator on purpose. Effects draw
     * far more numbers than firing does, and sharing one stream would make a
     * shot's spread depend on how many sparks happened to be alive — which the
     * original does do, since it has one rand(), but which makes a seeded
     * replay of the SIMULATION impossible to reproduce independently of the
     * frame rate. This is a deliberate divergence and it is why it is a second
     * generator rather than the same one.
     */
    q2_fx_world  fx;
    q2_rng       fx_rng;
    bool         fx_ready;

    /* Where an actor's posed mesh comes from, for the damage-effect drawers
     * (effect.h, 0x8005B880). NULL, or a hook that returns false, draws no
     * crackle — the timers still tick. Installed by the client, which owns
     * the models. */
    bool (*fx_mesh)(void *user, const struct q2_actor *a, q2_fx_mesh_src *out);
    void  *fx_mesh_user;

    /* The map's glint — its `GlintMod` mesh and the band state a level script
     * would fill. Points into `common`'s bytes. */
    q2_fx_glint      glint;
} q2_sim;

/* ------------------------------------------------------------------------- */
/* Feet and origin                                                            */
/* ------------------------------------------------------------------------- */
/*
 * `q2_player.pos` is the FEET, because that is what a StartPos names and what
 * the renderer, HUD and client all expect. The mover works in the entity
 * ORIGIN's frame instead — the centre of the 572-unit cube, Q2_EYE_BASE above
 * the feet — because that is the point the movement hull is built for.
 *
 * This is not a convention the port invented. SecondaryCol is PrimaryColl
 * ERODED BY 286 ON EVERY AXIS, measured over 5,275 axis probes across all 115
 * zones: the difference between the two hulls' free space is exactly 286 in
 * 37% of probes and within two units in 52%, against a distribution that would
 * be flat if they were unrelated. So SecondaryCol is the configuration-space
 * hull of the player's own cube, and a POINT moving in it is that cube moving
 * in the world. Which in turn is why the whole player call chain contains no
 * per-entity bounds access at all: the box is baked into the geometry.
 *
 * It also settles the "is 286 the real player hull or a broad-phase margin"
 * question in FORMATS.md §9.12: it is the real hull.
 *
 * +Y points down, so the origin sits at a SMALLER Y than the feet.
 */
Q2PSX_INLINE s32 q2_sim_origin_y(s32 feet_y) { return feet_y - Q2_EYE_BASE; }
Q2PSX_INLINE s32 q2_sim_feet_y(s32 origin_y) { return origin_y + Q2_EYE_BASE; }

void q2_sim_init(q2_sim *sim, const q2_world_zone *zone, int tick_rate_hz);
void q2_sim_free(q2_sim *sim);

/* ------------------------------------------------------------------------- */
/* Effects                                                                    */
/* ------------------------------------------------------------------------- */
/*
 * Attach the effect tables, which live in the boot executable. `tab` must
 * outlive the sim. Until this is called the sim runs exactly as it did before:
 * every spawn point below is a no-op, so a caller that has no executable to
 * hand still simulates.
 *
 * `seed` seeds the effect generator. Pass the same value for a reproducible
 * replay of the visuals.
 */
void q2_sim_attach_effects(q2_sim *sim, const q2_fx_tables *tab, u32 seed);

/*
 * Attach the map's glint mesh — its `GlintMod` chunk. Optional: only BIGGUN
 * carries one, and a map without it has no glint rather than a substitute.
 * `common` must outlive the sim, because the mesh points into its bytes.
 *
 * Returns false when the map has no chunk or the chunk is too small to hold
 * vertices.
 */
bool q2_sim_attach_glint(q2_sim *sim, const q2_common_file *common);
void q2_sim_glint_tick(q2_sim *sim);
void q2_sim_glint_evacuation_tick(q2_sim *sim, s32 dt);

/*
 * Throw a burst of debris. `bmin`/`bmax` are a Scene node's box and `at` an
 * optional fixed point — see q2_fx_debris_burst. The pieces are stepped by
 * q2_sim_combat_tick against PRIMARY collision, which is the hull the original
 * moves them in.
 *
 * This is what a breaking pane calls, through q2_sim_breakable_call below.
 */
u32 q2_sim_debris_burst(q2_sim *sim, const s32 bmin[3], const s32 bmax[3],
                        const s32 *at, u32 count, u8 area);

/*
 * The area byte a particle spawner wants — `0x800686C4`.
 *
 * Non-zero in means non-zero out (0x800686D0); zero asks PrimaryColl which
 * cell `at` is in and returns that cell's area record. Public because the
 * spawn sites live in two translation units and the console has one helper.
 */
u8 q2_sim_fx_area(q2_sim *sim, u8 area, const s32 at[3]);

/*
 * GLASS and SHOOTTHEN, run from a script CALL.
 *
 * `0x8002A350` is GLASS, and reading it settles both what a pane throws and
 * what a script can make it do:
 *
 *     obj = objectArray + 92 * item[+4]          ; 92-byte runtime objects
 *     if (item[+4] >= 0)
 *         debris(obj->node, obj->param_a, obj)   ; 0x8002A384, EVERY call
 *     if (damage != 0) {
 *         item[+6] -= damage                     ; hit points, mutated in place
 *         if (item[+6] > 0) return               ; still standing
 *     }
 *     if (item[+4] < 0) return                   ; 0x8002A3BC
 *     if (!Event_MuteLevFlag) {                  ; 0x8002A3C4/0x8002A3CC
 *         debris(obj->node, obj->param_b, 0)     ; 0x8002A3DC, the shatter
 *         sound(Sound_GlassSmash,                ; 0x8002A490/0x8002A4A4
 *               centre of the node's box)        ; 0x8002A3E4..0x8002A4A0
 *     }
 *     Scene2Data[obj->node].flags08 |= 0x8000    ; 0x8002A4C8..0x8002A4E0
 *     FreeSolidBoxs(obj->box)                    ; 0x8002A4E4/0x8002A4E8
 *     obj->box = 0; obj->damage_fn = 0           ; 0x8002A4F0/0x8002A4F4
 *
 * THE LAST THREE LINES ARE BELOW THE BRANCH, and this transcription used to
 * stop at the sound and miss them entirely. Bit 15 of a Scene node's flags at
 * +8 is do-not-draw, so the console's pane VANISHES the instant it breaks; the
 * port drew the geometry of every shot-out window for as long as the zone was
 * loaded. Being below the branch also means both happen during the EVE_ replay,
 * so a pane the script already broke comes back broken and invisible rather
 * than whole. The hide is queued through q2_sim_next_node_vis and the freed box
 * is the port's `broken` flag plus q2_sim_breakables_sync_solidity, which is
 * the same pair of effects: nothing draws it and nothing can hit it again.
 *
 * `0x80064558` — the debris burst this port already transcribes — takes the
 * SCENE NODE INDEX, a count, and a third argument that is a fixed origin or
 * zero: zero scatters the pieces uniformly through the node's box, which is
 * what makes a shattering window come apart across its whole surface. So the
 * hit burst comes out of a point and the shatter comes out of the pane.
 *
 * The sound is `amb_brkglas` — see Q2_GLASS_SOUND below for how the handle at
 * 0x800B27FC is named — played positionally at the box centre, and it is inside
 * the branch with the shatter rather than below it with the hide.
 *
 * A SCRIPTED call passes damage 0, and the branch at `0x8002A390` therefore
 * skips the hit-point subtract entirely and falls straight through to the
 * shatter — so a trigger volume that calls GLASS breaks it on the spot. That
 * is the whole reason this is reachable now: it needs no damage dispatch.
 *
 * SHOOTTHEN is deliberately not run: it returns at `0x8002E840` on a zero
 * damage argument, so a script call is a genuine no-op for it.
 *
 * `ops` supplies the two-buffer rebase (q2_uf_operand_at); pass NULL to read
 * operands in place. Returns the pieces actually spawned.
 */
u32 q2_sim_breakable_call(q2_sim *sim, const q2_scene *scene,
                          const q2_uf_operands *ops,
                          const q2_event_item *item, u8 call_index);

/* ------------------------------------------------------------------------- */
/* Shooting one                                                               */
/* ------------------------------------------------------------------------- */
/*
 * The route from a shot to a breakable, which openquestions #67 called "what is
 * still owed". It is a real chain in the executable and this is all of it:
 *
 *   0x800CAE10  a 48-entry array of 64-byte records — **damageable boxes**, one
 *               per breakable, allocated by 0x800555D8 from a Scene node's own
 *               box. The byte at +54 is the slot's use flag: zero means free,
 *               bit 0x4 means "this box can be damaged" and bit 0x8 is set the
 *               first time something hits it.
 *
 *   0x80053AA4  the SWEEP. The shot's trace walks all 48 slots, tests the ray
 *               against each in-use one (0x80052078) and, on a hit, writes hit
 *               kind 2 and the slot INDEX into the trace result at +18 and +22.
 *
 *   0x800488DC  a weapon's impact. It reads that index back, re-forms the
 *               record pointer, checks bit 0x4, sets bit 0x8, and calls...
 *
 *   0x8002EF1C  the ROUTER: walk the 48-entry runtime object array at
 *               0x800D6BB0 (92-byte stride), and for every object whose +0x28
 *               equals that record pointer, store the damage point in the
 *               object at +0x00..+0x08 and call the object's own +0x24
 *               callback with its CALL item. Five call sites, all weapons:
 *               0x80046634, 0x80047E54, 0x800488DC, 0x800492E8, 0x80049B18.
 *
 *   0x8002A518  GLASS's load constructor, which is what ties the two together:
 *               it takes the pane's Scene node, allocates a box from it through
 *               0x800555D8, and stores the returned record pointer in obj+0x28.
 *
 * So a pane's identity, as far as the weapon code is concerned, is a BOX in a
 * 48-entry registry that the shot trace tests separately from the world hull.
 * #67 guessed a collision node and that was close but not it — the registry is
 * its own structure, which is why the object array has no reference from the
 * weapon code and why nothing scans it looking for a victim.
 *
 * This port keeps the registry and the sweep and not the console's memory
 * layout: the box is the Scene node's, and the ray test is a slab test rather
 * than 0x80052078 transcribed, which is stated rather than implied.
 */
/*
 * Register the map's breakables. `ops` supplies the two-buffer rebase, exactly
 * as `q2_sim_breakable_call` takes it, because four of the disc's ten object
 * slots read -1 in COMMON's copy and resolve only in a zone's.
 *
 * Every GLASS record also receives a Q2_MOVE_KIND_ENTITY target from the same
 * raw Scene box. It is solid to movement, point traces and projectiles until
 * the fatal callback frees it; SHOOTTHEN and explosive boxes retain their
 * existing damage-only semantics.
 *
 * Returns how many were registered.
 */
u32 q2_sim_attach_breakables(q2_sim *sim, const q2_scene *scene,
                             const q2_uf_operands *ops);

/*
 * The sweep and the router in one: does the segment `from` -> `to` cross a
 * breakable's box, and if so, hurt it.
 *
 * Returns the pieces thrown, zero when nothing was in the way. The hit burst
 * comes out of the crossing POINT and the shatter out of the pane's whole box,
 * which is the difference between the two debris calls at 0x8002A384 and
 * 0x8002A3DC.
 *
 * A fatal hit on a pane also runs PSXGLASS's tail (0x8002A4AC..0x8002A4F4):
 * the node is queued for hiding through q2_sim_next_node_vis and its smash is
 * queued through q2_sim_next_glass_smash. Both are the caller's to drain — the
 * hide array and the mixer are the owner's — and a caller that drains neither
 * gets the behaviour this port had before they existed: a shot-out window that
 * is still drawn and still silent.
 */
/*
 * How far past a hitscan's impact point the pane sweep still looks, in
 * world units along the ray.
 *
 * A pane is solid, so the pellet meant to break it is the pellet it
 * stopped, and the trace hands back a point on the near side of that
 * surface — one unit of back-off at 0x800453F8 plus the hull's own
 * rounding. Eight units covers it and reaches nothing behind: the
 * thinnest wall on the disc is thicker than that, and the console has no
 * equivalent tolerance only because its sweep runs inside the trace.
 */
#define Q2_BREAKABLE_REACH 8

u32 q2_sim_breakable_shot(q2_sim *sim, const s32 from[3], const s32 to[3],
                          s16 damage);

/* Re-apply intact/broken pane state to the movement boxes. Save loading writes
 * the breakable records directly, then calls this once so shattered panes do
 * not become solid again merely because a zone was reconstructed first. */
void q2_sim_breakables_sync_solidity(q2_sim *sim);

/* Register the remaining users of retail's entity-box allocator (0x800555D8).
 * Call after building explosives and rotators, before the load-time scripts.
 * Repeated calls replace the old registry. Only TARGET rotators own boxes;
 * SIMROT and ROTBUTTON constructors allocate none. */
q2_result q2_sim_attach_scene_solids(q2_sim *sim, const q2_scene *scene,
                                      const q2_uf_operands *ops,
                                      const struct q2_rotator_set *rotators);
/* Refresh hatch displacement and explosive lifetimes. NULL leaves hatch
 * positions alone, useful when only an explosive changed inside the sim. */
void q2_sim_scene_solids_update(q2_sim *sim,
                                 const struct q2_rotator_set *rotators);
/* OBJDRAWOFF frees the node's box; later visibility alone cannot recreate it. */
void q2_sim_scene_solids_hide(q2_sim *sim, s32 node);

/* LevelBin-created box, e.g. JAIL3's lowered bridge. Repeated reconstruction
 * replaces the same node's box instead of allocating duplicate solids. */
bool q2_sim_scene_solid_set(q2_sim *sim, s32 node,
                             const s32 min[3], const s32 max[3]);

/*
 * Put a zone's `func_explosive` groups into the same registry.
 *
 * One box per INTACT node, which is what 0x80026A20 allocates: a four-part
 * group is four boxes naming one item, and a shot through any of them counts
 * against the group's shared hit points.
 *
 * A group whose authored health is zero is registered but NOT damageable — the
 * constructor's 0x80026C8C arm installs no callback — so its box is skipped
 * here rather than made shootable. Such a group is destroyed only by a script
 * running its item, through `q2_sim_explosive_trigger_item`.
 *
 * `set` is borrowed and must outlive the sim's use of it. Call after
 * `q2_sim_attach_breakables`, which resets the registry. Returns how many boxes
 * were added.
 */
u32 q2_sim_attach_explosives(q2_sim *sim, struct q2_explosive_set *set,
                             const q2_scene *scene);

/*
 * Destroy the group the item at `item_offset` built — a script reaching an
 * opcode-0x08 item, which the console does with damage zero.
 *
 * Spawns the effects and queues the visibility change, exactly as a fatal shot
 * does. Returns true when something was destroyed.
 */
bool q2_sim_explosive_trigger_item(q2_sim *sim, u32 item_offset);

/*
 * Take the next queued node visibility change, or false when the queue is
 * empty. `hidden` is 1 to stop drawing the node and 0 to resume.
 */
bool q2_sim_next_node_vis(q2_sim *sim, s16 *node, u8 *hidden);

/*
 * Take the next queued detonation position, or false when the queue is empty.
 *
 * The caller plays Q2_EXPLOSIVE_SOUND there — `wep_grenlx1a`, the handle at
 * 0x800B27F8, which 0x8002695C is the ONLY reader of in the whole executable.
 */
bool q2_sim_next_blast(q2_sim *sim, s32 out[3]);

/*
 * Take the next queued Explosion-model bang, or false when the queue is empty.
 *
 * The caller plays Q2_MODEL_ENT_SOUND there, positionally — SoundByCreature
 * (0x8005AA54) at the spawn's point. Drain it every tick beside
 * q2_sim_next_blast, and before it: the spawn's call comes first in PSXEXPLODE
 * (0x80026950, then 0x8002695C), so its voice is the one allocated first.
 */
bool q2_sim_next_explosion_sound(q2_sim *sim, s32 out[3]);

/*
 * WHAT A BREAKING PANE SOUNDS LIKE, and there is no guesswork in the name.
 *
 * 0x800B27FC has exactly two references in the whole image: the read at
 * 0x8002A490 that feeds 0x8002A4A4, and the store at 0x8002DB70 — the tail of
 * `InitEventSoundsEachLevel` (0x8002D46C, EVENT.C), which hands GetSoundNum
 * (0x80073518) the twelve-byte key at 0x800ABCC8, `amb_brkglas`. The NTSC
 * disc's symbol table names that global `Sound_GlassSmash`.
 *
 * The pair either side of it settles any remaining doubt: the same function's
 * previous call (0x8002DACC, key 0x800ABCBC `wep_grenlx1a`) stores its answer
 * at 0x8002DAE4 into 0x800B27F8, which is the handle the detonation report
 * reads. Two adjacent keys in the string table, two adjacent handles in bss,
 * initialised one after the other, and one reader each.
 *
 * Eleven characters in a twelve-byte field, which is exactly what
 * client_find_sound's prefix rule matches: BASE1's bank carries it as SNDVRAM
 * entry 29, `029_amb_brkglas`.
 */
#define Q2_GLASS_SOUND "amb_brkglas"

/*
 * Take the next queued pane-smash position, or false when the queue is empty.
 *
 * The caller plays Q2_GLASS_SOUND there, positionally, exactly as it plays a
 * detonation's report: 0x8002A4A4 is the same 0x80073704 by-handle entry with
 * the same kind of argument, the centre of the pane's Scene node box.
 *
 * Drain this every tick beside q2_sim_next_blast. A queue left undrained keeps
 * its entries and eventually drops new ones, which is a mute pane again.
 */
bool q2_sim_next_glass_smash(q2_sim *sim, s32 out[3]);

/*
 * Where the sim fires effects from, so a reader does not have to find them:
 *
 *   a projectile detonating          -> explosion   (0x800486EC), x2
 *   the BFG ball detonating          -> BFG burst   (0x8004BDC4), x3
 *   a bullet stopping on the WORLD   -> bullet puff (0x800489D8) through the
 *                                       second spawner 0x8003004C
 *   a creature taking damage         -> blood       (0x80048C08), x2
 *   the player taking damage         -> blood
 *   every actor, once a world tick   -> its damage effects, 0x8005B880
 *                                       (q2_fx_actor_present): the crackles,
 *                                       the mesh spark, the quad shell and
 *                                       the energy light
 *
 * A KILL RAISES NOTHING OF ITS OWN. This block used to list "a creature
 * reaching zero health -> gib (0x800596B0)". 0x800596B0 is inside the ITEM
 * think 0x80059330 and is the item materialise burst (Q2_FX_ITEM_MATERIALISE
 * in effect.h), not a gib; simcombat.c no longer fires it on a kill. What the
 * console raises when a body comes apart is ThrowGibs' mesh blood spray
 * (0x8005A3D4 -> 0x8005B320, q2_fx_gib_spray), which needs the posed model and
 * so is the gib owner's to raise, not the combat step's.
 *
 * Two older stale claims are gone as well. "A bolt striking
 * anything -> spark (0x8003E0C0)" was wrong: the spark's only reachable caller
 * in the executable is in the player's per-frame state think, which this port
 * does not model, and the address the mapping rested on is an entity
 * allocator rather than a burst. And "the hitscan weapons do not spawn
 * anything here yet" stopped being true when fx_hitscan_impact was added —
 * `world_fraction_for` gives the contact point, which is exactly what the note
 * said was missing.
 *
 * The `xN` are the outer loops the sites sit in. See `repeat` in effect.h.
 */

/*
 * Put a zone's pusher-enabled doors and lifts into the move world, so they are
 * solid. Visual-only movers (including PAL PISTON calls with +18 == 0) are not
 * registered.
 *
 * One target per Scene node each mover translates, taking the node record's
 * raw bbox — 0x800555D8 copies `scene_node_record + 16`, six s32, verbatim, so
 * NOT q2_scene_node_bounds, which inflates by Q2_SCENE_BBOX_SLOP.
 *
 * Call after q2_movers_build (and q2_movers_build_calls) and after
 * q2_sim_attach_gameplay, which is what allocates the volume half of the list.
 * The set is borrowed, not owned; q2_sim_movers_update reads it every tick.
 */
q2_result q2_sim_attach_movers(q2_sim *sim, const q2_mover_set *set,
                               const q2_scene *scene);

/*
 * Slide the registered boxes to where the movers now are — 0x80051EC0, which
 * the original runs once per mover per frame.
 *
 * Two boxes move, differently. The live box translates by the frame's delta;
 * the swept envelope only ever grows, one corner at a time by the sign of each
 * component, so it ends up covering the whole travel. `dy` takes the frame's
 * vertical change, which is what q2_move_sweep_box consumes as `other_dy` and
 * therefore what lets a player ride a lift.
 *
 * Call once per tick, immediately after q2_movers_tick.
 */
void q2_sim_movers_update(q2_sim *sim, const q2_mover_set *set);

/*
 * 0x80046234 and the 0x80051E74 damage arm: when a moving volume reaches a
 * player, retail first tries to carry that player by the volume's complete
 * three-axis step.  This is deliberately a plain collision move -- no step
 * sequence, no slide retries and no other mover sweep.  If it cannot complete,
 * the origin is restored and the mover's generic rollback damages the blocker
 * for thirty points with MOD_CRUSH.
 *
 * The boolean is the first half: true means the player was carried and the
 * mover may commit its step; false means the caller must veto that step.  Keep
 * the damage as a separate entry because one mover can own several boxes and
 * retail damages only after its whole attempted move has rolled back.
 */
bool q2_sim_mover_push(q2_sim *sim, const s32 step[3]);

#define Q2_MOVER_CRUSH_DAMAGE 30
q2_damage_result q2_sim_mover_crush(q2_sim *sim);

/*
 * Attach the map's trigger volumes and event script.
 *
 * Separate from init because these live in COMMON.DAT, which is per MAP, while
 * the zone geometry is per ZONE. Optional: a sim without them still runs, it
 * just has no gameplay.
 */
q2_result q2_sim_attach_gameplay(q2_sim *sim, const q2_common_file *common);

/* Rebind contact hazards once resident-zone operands are available. COMMON
 * owns the trigger records; LASERWALL's zone-specific object slot and damage
 * come from the resident Events copy. NULL retains the COMMON operands. */
void q2_sim_bind_hazards(q2_sim *sim, const q2_uf_operands *ops);

/*
 * Spawn the map's items and start running their thinks.
 *
 * Separate from q2_sim_attach_gameplay for the same reason that one is separate
 * from init: Population is per MAP, and a caller that only wants to walk around
 * need not pay for it. `table` may be NULL, in which case the built-in item
 * table is used — which is the right default for the catalogued PAL build and
 * refuses nothing.
 *
 * `zone` is which of the map's zones is loaded, and it matters because
 * Population is per MAP while a session is in one ZONE: a group the disc names
 * after a different zone is dropped rather than left standing in this one. Pass
 * -1 to spawn every group, which is what an offline pass over a whole map wants.
 * For a live zone the map's LevelBin supplies the other half of the answer: an
 * unzoned category or script batch is spawned only when its selector call names
 * it. See q2_item_spawn_zone for the shared creature/item fallback rule.
 *
 * `bank` is the map's CastList and may also be NULL. When it is given, each
 * item's model is resolved at spawn as the engine does it (0x80058850), so the
 * renderer never has to look one up mid-frame; without it, the resolve happens
 * on first draw instead.
 */
/*
 * The TITLE SCREEN's objects, which are not a Population and not a scene: five
 * item table ids QFRONT's `LevelBin` hands the engine's own item spawner, all
 * standing at (0, 0, 1700) facing half a circle round. The whole derivation is
 * in levelbin.h; the short version is that the spinning logo is an item and
 * spins for the same reason a shotgun on a pedestal does.
 *
 * Returns how many were spawned — 0 for every map that is not the front end,
 * because no other module carries the list. Call it INSTEAD of
 * q2_sim_attach_items, which QFRONT's empty Population would leave with nothing
 * in the set and `entities_ready` false, so the thinks never ran.
 */
#define Q2_SIM_SCENE_BASE 0x80100000u

u32 q2_sim_attach_scene(q2_sim *sim, const q2_common_file *common,
                        const q2_item_table *table,
                        const struct q2_model_bank *bank);

/*
 * What a front-end page change does to the scene — `module+0x3414`'s tail.
 *
 * `title` picks which of the module's two thinks the logo runs: the title
 * screen raises its light intensity to full, every other page dims it to a
 * quarter. The model geometry itself stays fixed.
 * `visible` is false for the one page that hides even the logo (id 11).
 *
 * The four player models are not addressed: the module shows object 0 and only
 * object 0, so they stay hidden whatever the page.
 */
void q2_sim_scene_page(q2_sim *sim, bool title, bool visible);
void q2_sim_scene_players(q2_sim *sim, const s16 colours[4], int players);

/*
 * One frame of the TITLE SCREEN, which is not one frame of the game.
 *
 * The client freezes the world while a menu is up, and for the pause menu that
 * is right — the original stops the level clock. The front end is the case that
 * rule gets wrong: it is a LEVEL with a page over it, its module installs a
 * per-frame hook (`engine+0x290`), and the logo's rotation is that level
 * running. Frozen, the scene is a still.
 *
 * This runs the entity set and nothing else, on the same clock and the same
 * accumulator `q2_sim_advance` uses, and returns 1 on the frames that stepped.
 * The narrowing is not a simplification: QFRONT is two nodes and eight
 * vertices with an empty `Population`, no `Events` and no player, so movement,
 * triggers, projectiles and the touch sweep have nothing to act on. Running
 * them would be running them over nothing.
 */
u32 q2_sim_scene_advance(q2_sim *sim, double elapsed_seconds);

q2_result q2_sim_attach_items(q2_sim *sim, const q2_common_file *common,
                              int zone, const q2_item_table *table,
                              const struct q2_model_bank *bank);

/*
 * The same attach, but told WHICH batches to spawn instead of asking the map.
 *
 * `batch` NULL is exactly q2_sim_attach_items: the module's own slot-36 call
 * sites decide, which is right for every single-player level.
 *
 * A multiplayer arena is the exception, and the reason this entry point exists.
 * QMULTI.C's init (0x80100140) does not consult the map at all — it spawns the
 * batches by NAME from a list the MODE picks, and the arena module names every
 * batch in its code whatever the mode, so a scan cannot tell VERSUS from
 * deathmatch. Pass q2_mp_batches' output here and the mode decides, as it does
 * on the disc. The names are spawned in the order given, which is the order the
 * console's allocator saw them in.
 */
q2_result q2_sim_attach_item_batches(q2_sim *sim, const q2_common_file *common,
                                     int zone, const q2_item_table *table,
                                     const struct q2_model_bank *bank,
                                     const char *const *batch, u32 batch_count);

/*
 * Run one named Population place group, as a CREBATCH CALL does. Returns the
 * number of newly spawned entities. The group's retail bit-1 latch is shared
 * with q2_sim_attach_items, so a startup group or a batch already activated
 * once returns zero and cannot duplicate its pickups.
 */
u32 q2_sim_activate_item_group(q2_sim *sim, const char *group);

/* What the item thinks asked for since the last call: pickup sounds, the
 * materialise sound, and the glow lights. Cleared at the start of every tick. */
const q2_ent_events *q2_sim_entity_events(const q2_sim *sim);

/* Consume a pending zone change, if any. Returns false when none is queued. */
bool q2_sim_take_zone_change(q2_sim *sim, u32 *out_zone);

/* Place the player, e.g. at a StartPos. */
void q2_sim_spawn(q2_sim *sim, const s32 pos[3], s32 yaw);

/*
 * Drop the player onto the floor beneath where they were spawned, using the
 * mover, and clear the fall state. Call after q2_sim_spawn and before the first
 * tick — a start position is not a standing position (see the note in sim.c).
 * Does nothing without a collision hull.
 */
void q2_sim_settle(q2_sim *sim);

/*
 * Advance the world by `elapsed_seconds` of real time.
 *
 * Converts to dt units, clamps as the original did, and runs whole logic ticks.
 * Returns the number of ticks actually run, which is 0 on a fast frame — the
 * caller should still render, interpolating if it wants smoothness.
 */
u32 q2_sim_advance(q2_sim *sim, const q2_input *input, double elapsed_seconds);

/*
 * The step that call would run with, without running it — 0 when it would not
 * tick at all.
 *
 * This exists for POINTING DEVICES. Every look input the engine has asks for a
 * RATE, and the angle integrates it as `rate * dt / 10`; a stick can do that
 * because a stick is held, but a mouse reports a DISPLACEMENT that has already
 * happened. Turning one into the other needs the step it is about to be
 * integrated over, and a host that guesses gets a sensitivity that changes with
 * the frame rate — the same physical movement turning the player half as far at
 * 60 fps as at 30, because each frame carries half the motion into a step half
 * as long.
 *
 * `q2_sim_advance` decides its own step through this function, so the answer a
 * caller is given is the one the tick then uses.
 */
s32 q2_sim_next_dt(const q2_sim *sim, double elapsed_seconds);

/*
 * One extra player's frame, against the world this frame has already advanced.
 *
 * `index` must be 1..3. The world half of a tick — the entity sweep, the
 * effects, the glint, the clock — runs only for player 0, so four players in
 * one sim share one world instead of each carrying a copy of it.
 */
void q2_sim_advance_player(q2_sim *sim, int index, const q2_input *input,
                           s32 dt);

/* Select whose combat state is live without advancing the world. Animation
 * driven shots use this after movement. Restore the previous cur_player after
 * processing the owner; shared projectiles, targets and clocks stay in place.
 * Invalid indices are ignored. */
void q2_sim_select_player(q2_sim *sim, int index);

/*
 * Give an extra player a level start's inventory and weapon, on a fresh actor
 * (killer byte 4, as 0x8003DE34 places a player). The actor's `owner` comes
 * back as the player index. Registers the owner for shared item touches.
 */
void q2_sim_player_reset_combat(q2_sim *sim, int index);

/* A fresh client record from an explicit spawn loadout. Unlike the convenience
 * reset above this never borrows another player's current health or weapon.
 * Position the player with q2_sim_spawn first; this seeds the pain baseline,
 * clears weapon cadence and registers an inventory that follows combat swaps. */
void q2_sim_player_loadout(q2_sim *sim, int index,
                            const q2_inventory *inv, int weapon);

/* Where the projectiles in flight got to — launched, stepped, expired, hit. */
typedef struct q2_sim_proj_stats {
    u32 launched;
    u32 stepped;
    u32 expired;
    u32 hit;

    /*
     * How close a bolt ever came. `nearest_hit` wants the ray to pass within
     * Q2_HITSCAN_RADIUS + the target's own 286, and it measures `along` as a
     * FRACTION of the ray — so a per-tick segment that is short compared with
     * the distance to the target puts the target past the far end and it is
     * never considered, however well aimed. These say which of "nowhere near"
     * and "near but rejected" is happening.
     */
    u32 near_miss;      /* a target within reach of the segment's line       */
    u32 past_end;       /* ...but beyond the segment's far end               */
    s64 closest_sq;     /* the smallest perpendicular distance squared seen  */
    s32 seg_len;        /* the last segment's length, in world units         */

    /* WHOSE origin produced `closest_sq`, and where it was. Printed rather
     * than reasoned about — see question 59c. */
    s32 closest_origin[3];
    s32 closest_from[3];
    s32 closest_owner;

    /* Shots that reached the launcher and found the 32-slot pool full. Counted
     * because the alternative — the shot vanishing after the ammo is spent —
     * is invisible, and was for as long as it was happening every tick. */
    u32 dropped_full;

    /*
     * Bolts and rockets a runtime solid (door, lift or intact pane) stopped, as
     * against the hull. Separately counted because "a rocket goes through a
     * closed door/window" and "a rocket goes through a wall" look the same
     * from the far side and are different faults: the second is a bad hull,
     * the first is an entity the sweep never asked about.
     */
    u32 stopped_on_entity;

    /*
     * RADIUS DAMAGE'S OCCLUSION, asked once per candidate that survived the
     * falloff (0x80050A24 then 0x80050A3C): how many were asked, how many a
     * wall or a solid entity box hid from the blast, and how many were waved
     * through because the blast point sat in no cell at all — a state only the
     * port reaches (see splash_clear in simcombat.c), counted so it cannot hide.
     */
    u32 splash_asked;
    u32 splash_occluded;
    u32 splash_unplaced;

    /*
     * THE MUZZLE'S CLIP into PrimaryColl (Find3dSpaceGivenSecondary, every
     * fire function but the BFG's and the hand grenade's): shots whose muzzle
     * the hull pulled back from inside a wall, and shots whose shooter stood
     * in no PrimaryColl cell, so the unclipped point was kept.
     */
    u32 muzzle_clipped;
    u32 muzzle_unplaced;
} q2_sim_proj_stats;

extern q2_sim_proj_stats q2_sim_proj_scan;

/*
 * Everything a projectile in flight can hit, which is not the same list as
 * whatever the player currently ticking can shoot at.
 *
 * `combat.targets` is the SHOOTER's list and deliberately excludes them. The
 * projectile step runs on player 0's tick because bolts are the world's, so it
 * was using player 0's list — and a bolt fired by player 1 at player 0 could
 * never find its target, because player 0 is the one name that list leaves out.
 * 601 bolts flew and none of them hit anything.
 *
 * The owner is skipped by pointer at impact instead, which is what makes a
 * world-wide list safe.
 *
 * Every reader takes `world_targets ? world_targets : combat.targets`, the
 * per-actor presentation pass (0x8005B880, in q2_sim_combat_tick) included.
 * So a single-player caller that only registers its creatures with
 * q2_sim_set_targets still has them presented every world tick, and one that
 * publishes the same list here as well gets each creature presented once, not
 * twice.
 */
void q2_sim_set_world_targets(q2_sim *sim, q2_actor **targets, u32 count);

/* One logic tick at the nominal step. Exposed so tests can drive it exactly. */
void q2_sim_tick(q2_sim *sim, const q2_input *input, s32 dt);

/* The eye position to render from, accounting for view height. */
void q2_sim_eye(const q2_sim *sim, s32 out_pos[3]);
/* Read a viewport owner's eye without changing the live combat player. */
void q2_sim_player_eye(const q2_sim *sim, int index, s32 out_pos[3]);

/*
 * 0x80038260 — the weapon's view angles: pitch, yaw and roll in the
 * 4096-step circle, with every kick the player is carrying folded in.
 *
 * The camera uses the player's plain aim. This function's sole retail caller
 * (0x8004F404) adds three decaying offsets to the weapon instead:
 *
 *     kick        30 ticks   what firing posts; pitch, yaw and roll
 *     hurt_kick  150 ticks   stored pitch and roll; not assigned by damage
 *     fall_value  90 ticks   what landing posts; pitch only
 *
 * Each is scaled by `(deadline - now) / period` computed in 1.0.12 — so a kick
 * whose deadline is FURTHER out than its own period is scaled by more than one,
 * which is a real consequence of `pain_time` being 210 while the hurt kick
 * decays over 150 and not something to clamp away.
 *
 * The weapon's placement negates the pitch (0x8004F41C); this returns the entity's own
 * sign, because that is what the aim and the weapon both use.
 */
void q2_sim_view_angles(const q2_sim *sim, s32 out[3]);
/* Includes this player's decaying firing, damage and landing kicks. */
void q2_sim_player_view_angles(const q2_sim *sim, int index, s32 out[3]);

/*
 * Post a view kick — what a weapon's recoil and a damage hit do. `period` picks
 * which of the three decays it joins; use Q2_VIEW_KICK_* below.
 */
#define Q2_VIEW_KICK_FIRE   30    /* client+0xCC, 0x800382D4 */
#define Q2_VIEW_KICK_HURT  150    /* client+0xD0, 0x80038364 */
#define Q2_VIEW_KICK_FALL   90    /* client+0xD4, 0x800383D4 */

/* ------------------------------------------------------------------------- */
/* Collision                                                                  */
/*                                                                            */
/* A thin convenience over q2_coll_move for callers that want a swept segment  */
/* rather than an entity move. `fraction` is 1.0.12 and is DERIVED from the    */
/* clipped end point, not carried through the trace: the engine works in exact */
/* rationals and never forms a fraction at all.                               */
/* ------------------------------------------------------------------------- */
typedef struct q2_trace {
    s32  fraction;      /* 1.0.12: 4096 means the whole move succeeded */
    s32  end[3];
    s32  normal[3];     /* 1.3.12 unit normal, valid when hit          */
    s32  node;          /* the cell the trace ended in, -1 if none     */
    s32  contents;      /* that cell's contents id                     */
    /*
     * WHICH RUNTIME ENTITY BOX stopped it, or -1 for the world. Doors and glass
     * are not part of either hull (trace.h), so a trace that ends on one ends
     * on nothing the `node`/`contents` pair can describe — they are cell fields
     * and there is no cell. This is the other half of the answer.
     */
    s32  ent;
    bool hit;
} q2_trace;

/*
 * Clipped against the zone's HULL and then against its runtime entity boxes —
 * doors, lifts and intact glass — in that order, nearest wins. The second pass
 * is not an extra: these are runtime objects absent from the static hull, so
 * without it a bullet sees every door open and every pane already broken.
 */
void q2_sim_trace(q2_sim *sim, const s32 start[3], const s32 end[3],
                  q2_trace *out);

/* ------------------------------------------------------------------------- */
/* Combat                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * Register the creatures the player can shoot. The sim borrows the array; the
 * caller keeps ownership and may pass NULL to clear it.
 */
void q2_sim_set_targets(q2_sim *sim, q2_actor **targets, u32 count);

/*
 * Register the SOLID bodies the player can walk into — the other half of the
 * actor list at 0x800B2B90 (see `q2_sim.bodies`).
 *
 * Separate from `q2_sim_set_targets` because the two lists answer different
 * questions and the console keeps them apart too: the target list is what a
 * shot sweeps (0x800544EC, radius entity+0x94 = 286) and this is what the
 * stepped mover separates (0x80051258, radius entity+0x90 = 128). A corpse is
 * on one and not the other.
 *
 * The sim copies the entries into its own array on the next tick; the caller
 * keeps ownership of what it passes and may pass NULL to clear it. The sim adds
 * its own players, so a caller registers creatures only.
 */
void q2_sim_set_bodies(q2_sim *sim, const q2_move_body *bodies, u32 count);

/* Reset the combat state to a freshly spawned player: the blaster, no other
 * weapon, an empty projectile list. Called by q2_sim_init. */
void q2_sim_combat_init(q2_sim *sim);

/* Advance the projectiles by one tick and resolve what they hit. Called by
 * q2_sim_tick after movement, so a rocket meets the wall the player is
 * standing against rather than the one they were standing against. */
void q2_sim_combat_tick(q2_sim *sim);

/* Give a weapon and select it if nothing better is held, exactly as the pickup
 * path at 0x80037E28 does: the switch happens only when the blaster is out. */
bool q2_sim_give_weapon(q2_sim *sim, int weapon_id);

/*
 * 0x80037E84's store, decided rather than performed: the weapon a player
 * holding `held` ends up with after being granted `weapon_id`, given this sim's
 * `autoswitch`. Returns `held` unchanged when nothing switches.
 *
 * Split out of q2_sim_give_weapon because the item sweep's pickups need the
 * same rule and cannot use that function: it refuses an already-owned weapon,
 * where the touch dispatch must still report the pickup as collected
 * (0x80037E4C), and it knows nothing of the infinite-ammo arm or the grenade
 * launcher's second grant. `inv` is the inventory the ammo test reads — pass
 * the one the grant belongs to, which in split-screen is not always the live
 * player's.
 */
int q2_sim_weapon_after_pickup(const q2_sim *sim, const q2_inventory *inv,
                               int held, int weapon_id);

/* Step to the next or previous usable weapon. Returns false when there is
 * nothing to switch to, which is what the original's cycle reports.
 *
 * This is the PAD's function — 0x80050758, the +/-1 neighbour scan. It is not
 * what a refire pass runs; see below. */
bool q2_sim_cycle_weapon(q2_sim *sim, int dir);

/*
 * Take the best weapon that is both owned and fed, from the fixed auto-switch
 * preference list — 0x800506C4, which the console's refire pass calls after
 * every shot. Idempotent, and returns true only when the held weapon changed.
 */
bool q2_sim_autoselect_weapon(q2_sim *sim);

/*
 * Fire the held weapon.
 *
 * Hitscan and rail shots are traced against the zone's own hull, so a bullet
 * stops at a wall rather than reaching through it, and the surviving fraction
 * is what limits which creatures it can reach. Projectiles are put in the
 * sim's list and advanced by q2_sim_tick.
 *
 * Returns what was fired, which is also left in `sim->combat.last_shot`.
 */
q2_fire_result_v2 q2_sim_fire(q2_sim *sim);

/* The view model retains its fire function until the switch completes.
 * weapon_id may therefore differ from the inventory's selected weapon. */
q2_fire_result_v2 q2_sim_fire_weapon(q2_sim *sim, int weapon_id,
                                    int chaingun_bullets);

typedef enum q2_hand_grenade_update {
    Q2_HAND_GRENADE_NONE = 0,
    Q2_HAND_GRENADE_HELD,
    Q2_HAND_GRENADE_RELEASED,
    Q2_HAND_GRENADE_EXPIRED
} q2_hand_grenade_update;

/* Feed Grenade3 the owning view weapon's world position and model-timeline
 * outputs after q2_vw_advance. It stays hidden/attached while held, accumulates
 * 6*cook_dt charge, detonates before release if its 1650-tick fuse elapsed,
 * and otherwise releases on the 411 crossing. */
q2_hand_grenade_update q2_sim_hand_grenade_update(
    q2_sim *sim, const s32 attached_pos[3], s32 cook_dt, bool release);

/*
 * The rail's spiral from `start` to `end` (q2_fx_rail_trail), turned by the
 * shooter's own angles — the player's view, or a creature's facing — because
 * the console rings it in the SHOOTER's matrix, entity+0x2C0, not the beam's.
 */
void q2_sim_rail_trail(q2_sim *sim, const s32 start[3], const s32 end[3],
                       s32 yaw, s32 pitch, s32 roll);

/* Hurt the player, going through the same damage path everything else does. */
q2_damage_result q2_sim_hurt_player(q2_sim *sim, q2_actor *attacker,
                                    s16 damage, s16 mod, const s32 point[3]);

/*
 * One creature round from `start` to `start + dir` — 0x8004874C as the
 * creature spawners reach it. The first box entered before the world takes
 * it: a creature other than `attacker` (its index in combat.targets comes
 * back), the player (Q2_SIM_HIT_PLAYER), or the world (-1, with the puff).
 * `dir` carries the range, as the console's does: the bullet spawners pass
 * the 1.12 aim shifted left by two, 16384 units. `mod` is the means of death
 * (Q2_MOD_BULLET for the spawners' own rounds); `mark_world` false leaves no
 * puff on the wall, for a bolt this port resolves along its line. `impact`,
 * when not NULL, receives where the round stopped (or its range's end).
 */
#define Q2_SIM_HIT_PLAYER       (-2)
#define Q2_SIM_CRE_HITSCAN_MAX  128
s32 q2_sim_creature_hitscan(q2_sim *sim, q2_actor *attacker,
                            const s32 start[3], const s32 dir[3], s16 damage,
                            s16 mod, bool mark_world, s32 impact[3]);

/* Mission beam damage, 0x80048DC8: nearest actor on the complete authored
 * segment, without a world trace. Returns a creature index, HIT_PLAYER or -1.
 * Call once per logic tick; the visual emitter is independent. */
s32 q2_sim_mission_laser(q2_sim *sim, const s32 start[3], const s32 end[3],
                          s16 damage, s16 mod);

typedef struct q2_cre_rail_result {
    u32 creatures_hit;
    bool player_hit;
    s32 end[3];
} q2_cre_rail_result;

/* The rail passes through actors and stops at world geometry. `dir` is the
 * full trace displacement, as for creature_hitscan; end is also the trail's
 * endpoint. The shooter is excluded from the damage sweep. */
q2_cre_rail_result q2_sim_creature_rail(q2_sim *sim, q2_actor *attacker,
                                       const s32 start[3], const s32 dir[3],
                                       s16 damage);
/* 0x800621F8 -> 0x800499A0: one damaged body, but the beam reaches the wall.
 * Aim components narrow to halfwords and scale by eight in the engine. */
q2_cre_rail_result q2_sim_creature_laser(q2_sim *sim, q2_actor *attacker,
                                        const s32 start[3], const s32 aim[3],
                                        s16 damage);

/*
 * A creature's BOLT, ROCKET or GRENADE, launched as an engine projectile rather
 * than resolved along its line: monster_fire_blaster (0x80062000) and
 * monster_fire_rocket (0x8006210C) hand the player's spawners a velocity made
 * from the module's 1.12 aim — `aim / 64` for the bolt, `aim * 3/2` for the
 * rocket — so it flies, trails or smokes, bursts where it lands and splashes.
 * `shooter` is credited and never hit; the player is a target of it. A
 * rocket points along the shooter's yaw, as 0x8004B210 takes it (projectile.h).
 * The grenade wrapper (0x800614D4) adds its two random toss components,
 * scales by 3/2 and passes a 600-tick fuse. Only that wrapper uses `speed`.
 * Blaster `effects & 0x40` selects hyper bolts (flags 14, MOD 5); otherwise
 * it uses flags 11, MOD 6. The Flyer supplies a raw aim delta, not a unit vector.
 * Returns the slot, or -1 for a full pool or invalid kind/speed.
 */
s32 q2_sim_creature_launch(q2_sim *sim, q2_actor *shooter, q2_fire_kind kind,
                           const s32 start[3], const s32 aim[3], s16 damage,
                           s32 shooter_yaw, s32 speed, u32 effects);

/* Apply a validated Events FX item to the current player. Returns false when
 * the item is not the retail fixed-size FX form. The original leaves
 * T_Damage's point argument uninitialised; this reproducible port uses NULL,
 * so the hit has no invented directional knockback. */
bool q2_sim_apply_event_fx(q2_sim *sim, const q2_event_item *item);

/* Publish the rendered GameWindow position for the next acquisition pass.
 * An offline caller without a published view uses the current player eye. */
void q2_sim_set_aim_view(q2_sim *sim, int player, const s32 eye[3], s32 node);
void q2_sim_find_target(q2_sim *sim, s32 dt);

/* The latched client+0x3C firing direction, in 1.3.12. Before the first live
 * logic tick, direct/offline fire falls back to the player's unassisted aim. */
void q2_sim_aim(const q2_sim *sim, s16 out[3]);

#endif /* Q2PSX_SIM_H */
