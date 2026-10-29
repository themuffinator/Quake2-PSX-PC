/*
 * save.h — persisting and restoring a game in progress.
 *
 * ---------------------------------------------------------------------------
 * A deliberate divergence, stated up front
 * ---------------------------------------------------------------------------
 * The original saved to a PlayStation memory card: fixed 8 KB blocks, a card
 * directory, and a whole layer of "insufficient free blocks" and "remove memory
 * card" handling. A native port has a filesystem, and reproducing block
 * management would add failure modes the host does not have in exchange for
 * nothing a player can perceive.
 *
 * So this writes an ordinary host file. That is a departure from faithfulness
 * and it is a considered one — unlike the rendering, where the hardware's
 * limits ARE the experience, a save file's container is invisible. What the
 * save CONTAINS still mirrors the original's state, and the *front end* that
 * reaches it is the console's own: four slots, because SAVE FILE has four rows
 * (memcard.h), driven through the console's own state numbers.
 *
 * Reading the original's card format is a separate job, worth doing so existing
 * saves can be imported. It is not this.
 *
 * ---------------------------------------------------------------------------
 * What a save has to contain, and why the obvious short list is wrong
 * ---------------------------------------------------------------------------
 * A first pass at this stored position, angles and inventory. That is enough to
 * write a file and nowhere near enough to restore a game, because the state a
 * player would notice missing is almost all somewhere else:
 *
 *   the level clock       Every powerup expiry (client+0xAC..0xB8), the refire
 *                         gate, mega health's decay and every item respawn is
 *                         an ABSOLUTE deadline on this clock. Restore the
 *                         inventory without it and a quad either lasts forever
 *                         or is already gone.
 *   the event flags       Which triggers have fired. Without them every door
 *                         the player opened is shut again.
 *   the trigger residency Which volumes the player is *standing in*. A volume
 *                         fires on ENTRY; loading into one with the bit clear
 *                         re-fires it, so a save made inside a teleport pad
 *                         teleports on load.
 *   the entity set        Which items have been collected. Without it a save
 *                         is also a resupply.
 *   the mover state       Velocity, the cached collision cell, the one-tick
 *                         frame delta the integrator carries (sim.h). Dropping
 *                         these is what makes a restored player stutter or fall
 *                         through the floor they were standing on.
 *   the generators        The weapon RNG. Two loads of the same save should
 *                         shoot the same spread.
 *
 * All of it is below. What is deliberately NOT saved is transient presentation
 * — live particles, beams, explosion/gib models and the last shot's tracer.
 * These effects finish naturally; loading a save drops their presentation.
 *
 * The client supplies creatures and doors through the capture/apply helpers
 * below because it owns those sets. CAIS additionally restores AI animation,
 * targets, timers and pursuit history; ROTR restores motion between endpoints.
 *
 * ---------------------------------------------------------------------------
 * Format
 * ---------------------------------------------------------------------------
 * A fixed header then a sequence of tagged chunks:
 *
 *     "Q2PS"  u32 version  u32 body_size  u32 crc32(body)
 *     body := { u32 tag, u32 size, u8 payload[size] } *
 *
 * Everything is little-endian and written field by field. The previous version
 * `fwrite`d the inventory struct directly, which made the file depend on the
 * host's padding and alignment — a save written by one compiler would have read
 * back as garbage under another, which is exactly the failure the round-trip
 * test exists to catch and the one thing a struct write cannot be tested for on
 * a single host. Nothing here writes a struct.
 *
 * Chunks are tagged and sized so a reader skips what it does not know. That is
 * what lets a later version add state without invalidating existing saves, and
 * it is why the version below is bumped only for changes that alter the meaning
 * of an existing chunk.
 *
 * The header carries the disc's serial, so a save cannot be loaded against the
 * wrong release — the level table and script offsets differ between builds, and
 * a mismatched save would restore a player into the wrong map's coordinates.
 */
#ifndef Q2PSX_SAVE_H
#define Q2PSX_SAVE_H

#include "events_rt.h"
#include "inventory.h"
#include "mover.h"
#include "rotator.h"
#include "spawn.h"
#include "projectile.h"
#include "q2psx.h"
#include "nativemission.h"
#include "sim.h"
#include "viewweapon.h"
#include "zonestate.h"

#define Q2_SAVE_MAGIC      "Q2PS"

/*
 * 6 — an EVNT byte changed meaning, and the item latches joined the file.
 *
 * The event runtime used to refuse a record on ONESHOT && HASRUN, so a spent
 * one-shot CAT_A record was saved as 0x49. It now refuses on DISABLED alone,
 * as 0x8002799C does, and the latch at 0x800279A8..0x800279BC writes
 * DISABLED := ONESHOT — the same record spent is 0xC9. Loaded as-is, a
 * version-5 byte of 0x49 would come back ARMED, and every spent one-shot
 * record in the level — CREBATCH spawns, STRING messages, doors — would fire
 * again. The bump is needed as well as the fix, because 0x49 is still a legal
 * byte now: it is what ENABLE (0x800278B0, bit 7 only) leaves on a re-armed
 * one-shot record, so a v5 byte and a v6 byte cannot be told apart otherwise.
 *
 * VERSION 5 IS MIGRATED, NOT REFUSED: on read, every EVNT byte with ONESHOT
 * and HASRUN both set gets DISABLED. That is exact, not a guess — the runtime
 * that wrote version 5 refused every record with both bits set and never
 * cleared HASRUN (its ENABLE cleared bit 7 alone), so in a v5 file that pair
 * always meant spent. A v5 file has no EVIT chunk, so its item latches come
 * back clear: the state that runtime had, since it never set one.
 *
 * EVIT is the event runtime's per-item latch shadow (events_rt.h,
 * `item_flags`): bit 7 of each item's op byte, written by 0x80027498. The
 * console keeps those bits beside the record bits in its own `EVE_<map>`
 * block (0x80029078, 0x80029214..0x80029254).
 *
 * 5 — deferred Population history joined the file in ITEM, and projectile
 * velocity/timer values acquired the retail per-kind stepping semantics.
 * Version 4 cannot be migrated honestly: it has no activation order for a
 * CREBATCH roster, and the same PROJ bytes would move at the wrong rate. It is
 * rejected at the header rather than applied approximately.
 *
 * 4 — the water life-support state joined the player chunk: accumulated air,
 * its next deadline, and the every-other-warning latch. A save taken mid-dive
 * must not return the player to the surface with a fresh breath.
 *
 * 3 — the movement frame's own state joined the player chunk: the second flag
 * word, the view-recentre latch, and the three view kicks with their deadlines.
 *
 * 2 was the same chunked format without those. It is not read either, and for
 * the same reason version 1 is not: the player chunk is fixed-width, so a
 * version-2 file parsed as this one would take the following chunk's bytes as a
 * kick and restore a player permanently flinching. Refusing is the honest
 * answer and it is what q2_save_read reports.
 *
 * Version 1 was a flat format that stored a raw `q2_inventory`.
 */
/*
 * 7 — LVLC, the level's LIVE counters: how many secrets have been found, which
 * INSECRET items have already counted, and the per-zone kill tallies. MISN has
 * always carried the mission table's u8 copy of the first of these, but the
 * console restores the live globals FROM that row on a load (`0x80022210`,
 * called once, from the save restore at `0x8003DD9C`) and the port had nothing
 * to restore them into: the client's own `secrets_found` was zeroed by the map
 * load and written back over the restored row on the next frame. The seen-list
 * and the kill slots are the port's own state and have no console counterpart
 * — see the chunk's comment in save.c.
 *
 * A version-6 file has no LVLC and is still read: an unknown-or-absent tag is
 * skipped, and the restore falls back to `q2_mission_get_counts` on the row,
 * which is `0x80022210` itself.
 */
/*
 * 8 — HELP, the help computer's CURRENT MESSAGE PAIR, as the two 12-byte
 * Strings keys.
 *
 * `HelpComputer_SetMessages` (0x80020F34) keeps the keys of the HELPCOMPUTER
 * that last fired in `LastOrders` (0x800C6EDC) and `LastObjective`
 * (0x800C6ED0) and resolves them into the two displayed pointers. They are not
 * session scratch: `HelpComputer_GetMessages` (0x800218A8) copies both keys
 * straight out into the unit's save block on the save arm of every
 * `LoadSaveHub` — 0x800227D4 in QMBASE and its four siblings at 0x80022F40,
 * 0x80023A90, 0x80024288 and 0x800249F8 — and the restore arm hands them back
 * to `SetMessages`. `HelpComputer_Reset` (0x80021968), which writes twelve
 * zero bytes from 0x800AB87C into both, sits only on the arm taken when the
 * unit has NO saved block.
 *
 * The KEYS travel, not the text: the console re-resolves them through the
 * level's Strings on the way back in, so a restore in a different level shows
 * that level's wording of the same message. The port does the same, and falls
 * back to the shipped "Awaiting Orders." defaults for a file with no chunk.
 *
 * A version-7 file has no HELP and is still read, the same way a version-6 file
 * has no LVLC.
 */
#define Q2_SAVE_VERSION    8

/* Additive chunks on version 8: EVHS holds visited-map event blocks (map name,
 * script size/count/identity, record latches, item latches and destroyed-group
 * bytes); EXPL holds resident group item offsets, shared health and destruction.
 * Missing EVHS starts a fresh history. Missing EXPL uses saved BRKS for broken
 * shootable groups and the existing script replay for script-only destruction.
 * ROTR holds rotator angles, pending motion and timers. CAIS holds creature
 * behavior, stable target/move keys, the AI clock and pursuit trail. SKIL holds
 * the difficulty used to build the creature roster.
 * CAIS v3 adds script callback/scale state; v4 adds export-0 initialization
 * and module-shared decisions/explosion phases. Version 5 adds physical actor
 * motion, pending impulses, local clock/phase and animation transition state.
 * Version 6 adds the player's two noise entities (position and stamp) and
 * references to them from the alert slots and creature targets.
 * Versions 1..5 still read,
 * remapping old roster indices around newly supported Jorg/Rider/Medic slots.
 * Missing ROTR/CAIS retains the older endpoint replay/basic creature restore.
 * Older readers skip these chunks under the existing container contract. */

/* EOBJ records permanent script-object changes without a one-shot latch.
 * They also travel in EVHS on map departure. Older files retain normal EVE_
 * replay; they cannot recover an object change that was never recorded. */

/* ZPOP preserves authored and dynamic objects in previously visited zones.
 * Version 1: map count, map name[16], Population identity, object count and
 * 58-byte authored records. Version 2 adds map clocks/trail/alerts, a serial
 * allocator and a typed pointer-free runtime after each 58-byte prefix. Moves
 * and references rebind by module offset/authored key; dynamics use a stable
 * serial. Source slots only reconstruct resident native-save allocation order.
 * Version 3 adds script callbacks and scale. Version 4 adds spawn initialization
 * and module-shared creature state, including pending boss/revival transitions.
 * Version 5 adds the same physical actor state as CAIS v5, rebasing its clock
 * and invalidating its collision-cell cache when the zone is rebuilt.
 * Version 1 and missing ZPOP remain readable; neither borrows session history. */

/* RAND adds the shared gameplay random stream. Older files have no recorded
 * state for that stream, so their load retains the reconstructed world's state. */

/* VWPN preserves the live view-weapon machine and its consumed-shot cursors.
 * Old files keep the previous fresh-weapon initialization on load. Runtime
 * table/model pointers are rebound from this disc, never stored in the file. */

/* ERTS preserves resident script clocks, TIMER/WAIT slots and pending triggers.
 * Missing ERTS retains legacy replay; a new save replaces that reconstructed
 * scheduler without firing any items while loading. */

/* PRST adds projectile effect flags and the BFG beam-damage accumulator.
 * Older files reconstruct bolt flags from the saved means of death, and start
 * the unrecorded beam accumulator at zero. */

/* The oldest version read, with the migration above. Older is refused. */
#define Q2_SAVE_VERSION_OLDEST 5

#define Q2_SAVE_MAP_LEN    16
#define Q2_SAVE_SERIAL_LEN 16
#define Q2_SAVE_LABEL_LEN  32

/* The four rows of SAVE FILE (memcard.h §"The four rows the runtime fills in").
 * The slot count is the console's screen, not a number chosen here. */
#define Q2_SAVE_SLOTS      4

/* How many menu settings travel with a save. The game layer does not include
 * menu.h — that would invert the dependency — so the settings arrive as an
 * opaque run of s16 the caller fills from its own q2_menu_settings. The
 * console does the same thing at states 14 and 16, which "apply the game
 * variables" out of what was loaded (memcard.h). */
#define Q2_SAVE_SETTINGS_MAX 48

/* Six 25-byte level records, as the MISSION screen stores them (mission.h). */
#define Q2_SAVE_MISSION_ROWS 6
#define Q2_SAVE_MISSION_NAME 21

/*
 * The level's live counters (LVLC, version 7).
 *
 * `secrets_found` is the console's `0x800B29FC`, which `0x80022210` reloads
 * out of the mission row on a restore. The other two halves are the PORT's,
 * and are here because the console does not need them:
 *
 *  - `secret_seen[]` is the dedupe list. The port counts DISTINCT INSECRET
 *    items because the event runtime fires a volume on entry rather than
 *    continuously (client main.c, `secrets_found`), so a restore that put the
 *    count back without the list would let the same secret count twice and
 *    push the figure past the map's total.
 *  - the zone slots are how the port keeps a LEVEL's kill tally on a machine
 *    that rebuilds the creature set at every zone gate. The console counts
 *    kills into one global and never rebuilds, so one number sufficed there.
 *
 * `Q2_SAVE_SECRETS_SEEN` matches the client's own array, and
 * `Q2_SAVE_LEVEL_ZONES` is one more than the highest ZONE<N>.DAT on the disc
 * (ZONE5, on both retail discs), so a real level always fits.
 */
#define Q2_SAVE_SECRETS_SEEN 64
#define Q2_SAVE_LEVEL_ZONES  8

typedef struct q2_save_level_counters {
    bool present;                            /* false on a pre-v7 file       */
    u32  secrets_found;
    u32  secret_seen_count;
    u32  secret_seen[Q2_SAVE_SECRETS_SEEN];  /* item offsets already counted */
    u32  zone_dead[Q2_SAVE_LEVEL_ZONES];     /* kills recorded per zone      */
    u32  zone_placed[Q2_SAVE_LEVEL_ZONES];   /* what each zone placed        */
} q2_save_level_counters;

/*
 * The help computer's current pair (HELP, version 8).
 *
 * Twelve bytes each because that is what `LastOrders` and `LastObjective` are
 * — `HelpComputer_SetMessages` moves them with three `lwl`/`lwr` pairs
 * (0x80020F80..0x80020F94), and `HelpComputer_Reset` zeroes the same twelve
 * from 0x800AB87C. `present` is false on anything older than version 8, and an
 * empty key means "never fired", which is exactly what Reset leaves behind.
 */
#define Q2_SAVE_HELP_KEY 12

typedef struct q2_save_help {
    bool present;
    char orders_key[Q2_SAVE_HELP_KEY + 1];      /* 0x800C6EDC */
    char objective_key[Q2_SAVE_HELP_KEY + 1];   /* 0x800C6ED0 */
} q2_save_help;

struct q2_mission;   /* mission.h; only the copy helpers below need it */

/* ------------------------------------------------------------------------- */
/* Per-entity state                                                           */
/* ------------------------------------------------------------------------- */
/*
 * Version-4 saves rebuilt the entity SET from the map and reapplied this
 * mutable part by index. Version 5 also carries q2_save_item_key for each slot,
 * because a collected item frees its slot and a later CREBATCH can reuse it:
 * after that, rebuilding in Population order no longer produces the same
 * indices. `place_id` remains a second check against changed map data.
 */
/*
 * Which think is installed. The pointer itself cannot be written to a file, and
 * it is not decoration: a Q2_ITEM_TIMED entity switches to the shrink think
 * when it is collected (0x8005B358), and a save made mid-shrink that restored
 * the spawn think would show the item growing back.
 */
typedef enum q2_save_think {
    Q2_SAVE_THINK_NONE = 0,
    Q2_SAVE_THINK_ITEM,       /* q2_item_think        — 0x80059330 */
    Q2_SAVE_THINK_SHRINK,     /* q2_item_shrink_think — 0x8005B358 */
    Q2_SAVE_THINK_OTHER       /* something this version does not name */
} q2_save_think;

typedef struct q2_save_entity {
    u16 place_id;
    u8  in_use;
    u8  hidden;
    u8  taken;            /* bit per player, so four players fit in a byte    */
    u8  think;            /* a q2_save_think                                  */
    s16 scale;            /* +0xFC light intensity (historical field name)    */
    s16 health;           /* +0x108                                           */
    s32 frame;            /* +0x100                                           */
    s32 spin;             /* +0xE8, the yaw the think decrements              */
    s32 remove_in;        /* +0xF4                                            */
    s32 respawn_at;       /* +0x4C                                            */
    s32 pos[3];           /* +0x54 — a dropped item does not stay where it
                           * spawned, so position is state, not map data      */
} q2_save_entity;

/* Stable identity of one Population-backed entity. `group == -1` names a
 * dead/free or non-Population slot and retains the legacy index treatment. */
typedef struct q2_save_item_key {
    s32 group;
    u32 slot;
} q2_save_item_key;

/*
 * A breakable's mutable state.
 *
 * Keyed by SCENE NODE rather than by index, for the same reason a mover is
 * keyed by its item's offset: the registry is rebuilt from the map on load and
 * an ordinal is only stable while build order never changes. The node is the
 * pane's identity in the map.
 *
 * Without this a save made after shooting a window restores it intact, and the
 * shards that were already on the floor come back as a whole pane — which is
 * the same class of defect as a save that shuts every door the player opened,
 * and that one was fixed by carrying the script flags.
 */
/*
 * A door or lift's mutable state.
 *
 * Keyed by the event item it was built from — the identity `mover.h` already
 * establishes for exactly this reason — plus WHICH of that item's movers it is,
 * because a `MOVER_C` double door is two leaves from one item.
 *
 * Without this a save reloads with every door shut, and worse than shut: the
 * script flags ARE carried, so the record that opened it has already run and
 * will not run again. The door is closed and cannot be reopened.
 */
typedef struct q2_save_mover {
    u32 item_offset;
    u8  seq;            /* 0 or 1: which leaf of this item                   */
    u8  state;
    u8  saved_state;
    u8  block_timer;
    u8  triggered;
    u8  announced;
    u16 delay_timer;    /* a countdown at run time, not just a setting       */
    u16 wait_timer;
    s32 offset;         /* how far along its travel it is                    */
} q2_save_mover;

/*
 * A creature's mutable state — what a player would notice, not the whole
 * struct.
 *
 * The rest of `q2_monster` is function pointers and pointers to other monsters,
 * and both are rebuilt at load: the module is relocated and bound again, so the
 * callbacks come back by construction. What does NOT come back is who is dead,
 * where the survivors are standing and how hurt they are, and without those a
 * save reloads into a room the player has already cleared, full again.
 *
 * CRES remains the compatibility baseline. The optional CAIS chunk adds the
 * AI state using roster indices and module-relative move keys, never pointers.
 */
typedef struct q2_save_creature {
    u8  in_use;
    u8  dead;
    s16 health;
    s16 frame;
    s16 angles[3];
    s32 pos[3];
} q2_save_creature;

typedef struct q2_save_rotator {
    s16 node;
    u8 kind, axis;
    s32 accum;
    s16 angle;
    u8 state, step_pending, motor_active;
    u16 delay, hold;
} q2_save_rotator;

struct q2_save_ai;
struct q2_creature_world;

typedef struct q2_save_breakable {
    s32 scene_node;
    s16 health;
    u8  broken;
} q2_save_breakable;

typedef struct q2_save_explosive {
    u32 item_offset;
    s16 health;
    u8  destroyed;
} q2_save_explosive;

/* ------------------------------------------------------------------------- */
/* One level's tally, as the MISSION screen's 25-byte record holds it          */
/* ------------------------------------------------------------------------- */
typedef struct q2_save_level_stats {
    char name[Q2_SAVE_MISSION_NAME + 1];
    u8   secrets, secrets_total;
    u8   kills,   kills_total;
} q2_save_level_stats;

/* ------------------------------------------------------------------------- */
/* The snapshot                                                               */
/* ------------------------------------------------------------------------- */
typedef struct q2_save_event_runtime {
    u32 script_size, script_crc, record_count;
    s32 clock, tick;
    u32 pending_count;
    u32 pending[Q2_EVENT_RT_PENDING_MAX];
    q2_event_timer timers[Q2_EVENT_RT_TIMER_MAX];
    q2_event_wait waits[Q2_EVENT_RT_WAIT_MAX];
} q2_save_event_runtime;

typedef struct q2_save {
    /* --- identity ------------------------------------------------------- */
    char serial[Q2_SAVE_SERIAL_LEN];  /* the disc this save belongs to       */
    char map[Q2_SAVE_MAP_LEN];
    s32  zone;
    char label[Q2_SAVE_LABEL_LEN];    /* what the SAVE FILE row shows        */
    u32  timestamp;                   /* host epoch seconds, 0 when unknown  */

    /* --- the player ------------------------------------------------------ */
    /*
     * The whole mover state, not just where they are standing. Held as the
     * live struct because every field of it is state the tick reads; the
     * serialiser writes it out member by member, so the file does not inherit
     * the struct's padding.
     */
    q2_player    player;

    /* --- the simulation -------------------------------------------------- */
    s32  level_time;      /* the clock every deadline in the save is against  */
    u32  tick_count;
    s32  dt_accum;
    s32  dt_per_field;
    s32  gravity;
    u32  env_flags;
    u32  cheats;
    s32  current_node;
    u8   multiplayer;
    u8   full_basis_movement;
    u8   no_fall_damage;

    /* --- combat ---------------------------------------------------------- */
    q2_inventory inventory;
    s32  weapon_id;
    s32  next_fire;
    s16  kick[3];
    s32  chaingun_bullets;
    u32  rng_state;       /* the weapon generator: same save, same spread     */
    u32  fx_rng_state;
    bool game_rng_present;
    u32  game_rng_state;

    bool viewweapon_present;
    q2_viewweapon viewweapon;  /* scalar state only; tab/model are NULL */
    s32 viewweapon_selected;
    u32 shot_serial, shot_serial_shown, shot_serial_heard;
    bool shot_fired, shot_dry, cock_pending;
    s16 shot_sound;

    /* Projectiles in flight. A rocket mid-air is as much state as the player. */
    q2_projectile proj[Q2_PROJ_MAX];
    bool projectile_runtime_present;

    /* --- the world ------------------------------------------------------- */
    /* One flags byte per event record, indexed as the runtime indexes them. */
    u8  *event_flags;
    u32  event_count;

    /* The item latches, one byte per Events chunk byte, indexed by item offset
     * as the runtime's `item_flags` is. Version 6; absent from a v5 file. */
    u8  *event_item_flags;
    u32  event_item_count;
    /* Optional EOBJ: permanent object changes without a one-shot latch. */
    u8  *event_object_flags;
    u32  event_object_count;
    bool event_runtime_present;
    q2_save_event_runtime event_runtime;

    /* EVE_ blocks for previously visited maps. EVHS is additive: older files
     * have no history, so loading them starts with only their resident map's
     * EVNT/EVIT state, never the abandoned session's history. */
    bool           event_history_present;
    q2_event_store event_history;

    /* ZPOP is additive: older saves start with no visited-zone populations. */
    bool zone_history_present;
    q2_zone_history zone_history;

    /* NMIS is additive; old files reconstruct their available event latches. */
    bool native_mission_present;
    q2_native_mission native_mission;

    /* One byte per trigger volume: was the player inside it last tick. */
    u8  *trigger_inside;
    u32  trigger_count;

    /* Per-entity mutable state, parallel to the rebuilt entity set. */
    q2_save_entity *entities;
    u32             entity_count;

    /*
     * ITEM, introduced with version 5. `item_group_order` is the exact order
     * in which startup selection and later CREBATCH calls first ran groups;
     * membership is therefore also the one-shot latch. Keys are parallel to
     * ENTS and let restore rebuild a reused runtime slot from its Population
     * identity rather than guessing from its current index.
     *
     * Compatibility decision: a version-4 file cannot reconstruct a deferred
     * roster whose history it never recorded, and old PROJ bytes now have
     * different per-kind stepping semantics. The header therefore advances to
     * version 5 and rejects v4. From v5 on, ITEM is mandatory when the loaded
     * map has Population groups; no-Population utility snapshots may omit it.
     */
    bool              item_state_present;
    u32               item_population_group_count;
    u32              *item_group_order;
    u32               item_group_order_count;
    q2_save_item_key *item_keys;
    u32               item_key_count;

    /* Which panes have been shot, and how much they have left. */
    q2_save_breakable *breakables;
    u32                breakable_count;

    /* Group health is shared by all its hitboxes; script-only groups have no
     * hitbox at all. Neither can be reconstructed from BRKS alone. */
    q2_save_explosive *explosives;
    u32                explosive_count;

    /* Which doors and lifts are open, and where in their travel. */
    q2_save_mover *movers;
    u32            mover_count;

    bool rotators_present;
    q2_save_rotator *rotators;
    u32 rotator_count;

    /*
     * Who is dead and where the rest are standing. Parallel to the rebuilt
     * creature set, which is built from the map's spawn records in a fixed
     * order — the same assumption the entity chunk makes.
     */
    q2_save_creature *creatures;
    u32               creature_count;
    struct q2_save_ai *ai;
    bool skill_present;
    s32 skill;

    /* --- presentation ---------------------------------------------------- */
    q2_save_level_stats mission[Q2_SAVE_MISSION_ROWS];
    s32  mission_unit;

    /* ...and the live counters behind that table's first column. */
    q2_save_level_counters level;

    /* The help computer's current message pair, as keys (0x800218A8). */
    q2_save_help help;

    s16  settings[Q2_SAVE_SETTINGS_MAX];
    u32  settings_count;
    bool cheat_level_present;
    u8   cheat_level;            /* engine+378, included in native Game Config */
} q2_save;

void q2_save_free(q2_save *s);

/* ------------------------------------------------------------------------- */
/* Capture and apply                                                          */
/* ------------------------------------------------------------------------- */
/*
 * Capture the current state. Everything variable-length is copied, so the
 * runtime may change afterwards without disturbing the snapshot.
 *
 * `inv` is the inventory to record. It is a separate argument rather than being
 * taken from `sim->combat.inv` because a caller may hold the player's inventory
 * outside the sim; pass NULL to use the sim's own, which is the normal case.
 */
q2_result q2_save_capture(q2_save *out, const q2_sim *sim,
                          const q2_inventory *inv,
                          const char *serial, const char *map, s32 zone);

/*
 * Apply a loaded save to a simulation.
 *
 * Fails if the save is for a different disc, or if the map does not match — the
 * caller must load the right map and zone first, because restoring a position
 * into the wrong geometry is worse than refusing.
 *
 * The caller is expected to have run the same attach sequence a fresh load runs
 * (gameplay, items, effects); this restores the mutable state on top of it. An
 * entity set of a different size than the save's is a mismatched map and is
 * reported rather than partially applied.
 *
 * `inv` may be NULL, in which case only the sim's own inventory is restored.
 */
q2_result q2_save_apply(const q2_save *s, q2_sim *sim, q2_inventory *inv,
                        const char *serial, const char *map);

/* Deep copies; a failed copy leaves the destination unchanged. Restore also
 * clears an existing store when the file predates EVHS. */
q2_result q2_save_capture_event_history(q2_save *s, const q2_event_store *store);
q2_result q2_save_restore_event_history(const q2_save *s, q2_event_store *store);

/* Build the resident map's replay input from the SAVE, after its Events chunk
 * is attached but before STARTLEV/replay constructs the world. The chunk's
 * identity comes from the loaded runtime; EVNT/EVIT sizes must match it. */
q2_result q2_save_event_carry(const q2_save *s, const q2_event_rt *rt,
                              q2_event_carry *out);

/*
 * The movers, which live in the CLIENT rather than the sim — the same shape as
 * the mission tallies below, and for the same reason: `q2_save_capture` is
 * handed a `q2_sim` and the door set is not in one.
 */
void q2_save_capture_movers(q2_save *s, const q2_mover_set *set);
void q2_save_apply_movers(const q2_save *s, q2_mover_set *set);

q2_result q2_save_capture_rotators(q2_save *s, const q2_rotator_set *set);
/* Apply to freshly rebuilt rotators whose old audio voices have been stopped. */
q2_result q2_save_apply_rotators(const q2_save *s, q2_rotator_set *set);

/* And the creatures, for the same reason: the set lives in the client. The
 * count must match on apply, or the save is for a different population and
 * nothing is restored. */
void q2_save_capture_creatures(q2_save *s, const q2_monster_set *set);
void q2_save_apply_creatures(const q2_save *s, q2_monster_set *set);

/* The client calls these alongside CRES. No AI chunk means legacy behavior.
 * Skill must be restored BEFORE spawning the map's difficulty-filtered roster.
 * phase_us is the unspent fraction of the client's 100 ms AI tick. */
q2_result q2_save_capture_ai(q2_save *s, const struct q2_creature_world *world,
                             u32 phase_us);
q2_result q2_save_apply_ai(const q2_save *s, struct q2_creature_world *world,
                           u32 *phase_us);
bool q2_save_get_skill(const q2_save *s, s32 *skill);
/* Called by q2_save_apply and again after client-side presentation rebuilds,
 * so their constructor draws cannot consume the saved gameplay sequence. */
void q2_save_apply_game_rng(const q2_save *s);

q2_result q2_save_capture_viewweapon(q2_save *s, const q2_viewweapon *vw,
                                     int selected, u32 shown, u32 heard,
                                     bool cock_pending);
/* Keeps the current weapon unchanged for a legacy save without VWPN.
 * The caller rebinds the model after success and restores the shot cursors. */
q2_result q2_save_apply_viewweapon(const q2_save *s, q2_viewweapon *vw,
                                   const q2_vm_tables *tables);

/* The mission tallies, which live in the client rather than the sim (mission.h
 * — the counters are inputs to the screen). Both are no-ops on NULL. */
void q2_save_capture_mission(q2_save *s, const struct q2_mission *m);
void q2_save_apply_mission(const q2_save *s, struct q2_mission *m);

/*
 * And the live counters the row is a copy of. `set` marks the chunk present;
 * `get` returns false — writing nothing — for a file that has none, which is
 * every save written before version 7, and leaves the caller to fall back to
 * `q2_mission_get_counts` on the restored row.
 */
void q2_save_set_level_counters(q2_save *s, const q2_save_level_counters *lc);
bool q2_save_get_level_counters(const q2_save *s, q2_save_level_counters *out);

/*
 * `HelpComputer_GetMessages` / `HelpComputer_SetMessages`, 0x800218A8 and
 * 0x80020F34 — the two Strings KEYS of the message pair that is current, which
 * every unit hub copies into and out of its save block.
 *
 * `set` marks the chunk present even when both keys are empty: empty is a real
 * answer, and it is what `HelpComputer_Reset` writes on the arm taken when a
 * unit has no saved block at all. `get` returns false — writing nothing — for a
 * file older than version 8, and the caller then keeps the session's pair.
 */
void q2_save_set_help_keys(q2_save *s, const char *orders_key,
                           const char *objective_key);
bool q2_save_get_help_keys(const q2_save *s, q2_save_help *out);

/* The menu settings, as an opaque run of s16 — see Q2_SAVE_SETTINGS_MAX. */
void q2_save_set_settings(q2_save *s, const s16 *values, u32 count);
u32  q2_save_get_settings(const q2_save *s, s16 *out, u32 count);

/* ------------------------------------------------------------------------- */
/* Files                                                                      */
/* ------------------------------------------------------------------------- */
q2_result q2_save_write(const q2_save *s, const char *path);
q2_result q2_save_read(q2_save *out, const char *path);

/*
 * Just the identity, without allocating the variable-length sections. This is
 * what the SAVE FILE rows are built from, and it is why listing four slots does
 * not mean reading four whole games off disk.
 */
typedef struct q2_save_info {
    bool used;            /* a file is there and its header parsed           */
    u32  version;
    char label[Q2_SAVE_LABEL_LEN];
    char map[Q2_SAVE_MAP_LEN];
    char serial[Q2_SAVE_SERIAL_LEN];
    s32  zone;
    u32  timestamp;
    s32  level_time;      /* elapsed play time on the 300 Hz clock            */
    s16  health;
    s32  weapon_id;
} q2_save_info;

q2_result q2_save_read_info(q2_save_info *out, const char *path);

/* ------------------------------------------------------------------------- */
/* Slots                                                                      */
/* ------------------------------------------------------------------------- */
/*
 * Where saves live. Resolved once, in this order:
 *
 *   the override set by q2_save_set_dir       (tests, and a --saves flag)
 *   %APPDATA%\Q2PSX-PC\saves                  on Windows
 *   $XDG_DATA_HOME/q2psx-pc/saves, else $HOME/.local/share/q2psx-pc/saves
 *   ./saves                                   when nothing else resolves
 *
 * The directory is created on the first write, not at startup: a session that
 * never saves should not leave a directory behind.
 */
const char *q2_save_dir(void);
void        q2_save_set_dir(const char *dir);

q2_result q2_save_slot_path(int slot, char *out, u32 out_size);
q2_result q2_save_slot_write(const q2_save *s, int slot);
q2_result q2_save_slot_read(q2_save *out, int slot);
q2_result q2_save_slot_info(q2_save_info *out, int slot);
q2_result q2_save_slot_delete(int slot);

/* Every slot, in order. `out` must hold Q2_SAVE_SLOTS entries; an empty slot
 * comes back with `used` false rather than being skipped, because the screen
 * draws four rows either way. Returns how many are in use. */
u32 q2_save_slots_scan(q2_save_info *out, u32 count);

/*
 * The text of one SAVE FILE row. An unused slot is the empty string, which is
 * exactly what that screen wants: the selection bar tests the label against the
 * empty string, so an unfilled row draws nothing and gets no bar (memcard.h).
 *
 * Returns `out`.
 */
const char *q2_save_slot_row(const q2_save_info *info, int slot,
                             char *out, u32 out_size);

/*
 * LOAD/SAVE SETTINGS uses the same four memory-card positions but a different
 * file type from a saved game (engine flag 0x01000000 selects game saves;
 * zero selects settings, with bit 31 naming a MULTI UNLOCK file). Both kinds
 * of settings file carry the current cheat tier, as native Game Config does.
 * The game layer keeps the remaining payload opaque so menu.h remains a
 * one-way dependency: the client packs its settings and match setup into this
 * bounded run of signed words.
 */
#define Q2_SETTINGS_VALUE_MAX 96
typedef struct q2_settings_blob {
    u32 count;
    s16 value[Q2_SETTINGS_VALUE_MAX];
    u8 cheat_level;
    bool unlock_file;
} q2_settings_blob;

q2_result q2_settings_slot_path(int slot, char *out, u32 out_size);
q2_result q2_settings_slot_write(const q2_settings_blob *settings, int slot);
q2_result q2_settings_slot_read(q2_settings_blob *out, int slot);
q2_result q2_settings_slot_delete(int slot);
u32       q2_settings_slots_scan(bool *used, u32 count);

/* A default label for a new save: the map, zone and elapsed time. Returns
 * `out`. */
const char *q2_save_default_label(const q2_save *s, char *out, u32 out_size);

#endif /* Q2PSX_SAVE_H */
