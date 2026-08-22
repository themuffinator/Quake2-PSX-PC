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
 * — live particles, beams, the last shot's tracer — because it is regenerated
 * within a tick and storing it would only make the format brittle.
 *
 * Creatures are not saved either, and that is a limit rather than a choice: the
 * sim does not own them (sim.h — a caller registers actors), so there is no set
 * to walk. q2_save_apply says so at the site rather than silently restoring a
 * level with its monsters back at full health and no way to tell.
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
#include "spawn.h"
#include "projectile.h"
#include "q2psx.h"
#include "sim.h"

#define Q2_SAVE_MAGIC      "Q2PS"

/*
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
#define Q2_SAVE_VERSION    4   /* the BRKS chunk is additive: an older file
                               * simply has none, and a reader that does not
                               * know the tag skips it — which is what the
                               * chunked format is for */

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

struct q2_mission;   /* mission.h; only the copy helpers below need it */

/* ------------------------------------------------------------------------- */
/* Per-entity state                                                           */
/* ------------------------------------------------------------------------- */
/*
 * The entity SET is rebuilt from the map by q2_sim_attach_items, which walks
 * the Population in a fixed order — so the save does not store entities, it
 * stores the mutable part of each one and reapplies it by index. `place_id` is
 * carried alongside as a check: if the map's population has changed under the
 * save, the ids stop matching and the restore says so instead of teleporting
 * one item's "taken" flag onto another.
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
    s16 scale;            /* +0xFC, mid-materialise when it is not 4096       */
    s16 health;           /* +0x108                                           */
    s32 frame;            /* +0x100                                           */
    s32 spin;             /* +0xE8, the yaw the think decrements              */
    s32 remove_in;        /* +0xF4                                            */
    s32 respawn_at;       /* +0x4C                                            */
    s32 pos[3];           /* +0x54 — a dropped item does not stay where it
                           * spawned, so position is state, not map data      */
} q2_save_entity;

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
 * A STATED DIVERGENCE: the AI's own timers and its target are not carried, so a
 * creature that was hunting the player resumes from its stand. The console
 * keeps them; this port would have to serialise pointers into a set that is
 * rebuilt, and restoring a stale enemy pointer is worse than restarting the
 * hunt. Recorded here rather than left to be discovered.
 */
typedef struct q2_save_creature {
    u8  in_use;
    u8  dead;
    s16 health;
    s16 frame;
    s16 angles[3];
    s32 pos[3];
} q2_save_creature;

typedef struct q2_save_breakable {
    s32 scene_node;
    s16 health;
    u8  broken;
} q2_save_breakable;

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

    /* Projectiles in flight. A rocket mid-air is as much state as the player. */
    q2_projectile proj[Q2_PROJ_MAX];

    /* --- the world ------------------------------------------------------- */
    /* One flags byte per event record, indexed as the runtime indexes them. */
    u8  *event_flags;
    u32  event_count;

    /* One byte per trigger volume: was the player inside it last tick. */
    u8  *trigger_inside;
    u32  trigger_count;

    /* Per-entity mutable state, parallel to the rebuilt entity set. */
    q2_save_entity *entities;
    u32             entity_count;

    /* Which panes have been shot, and how much they have left. */
    q2_save_breakable *breakables;
    u32                breakable_count;

    /* Which doors and lifts are open, and where in their travel. */
    q2_save_mover *movers;
    u32            mover_count;

    /*
     * Who is dead and where the rest are standing. Parallel to the rebuilt
     * creature set, which is built from the map's spawn records in a fixed
     * order — the same assumption the entity chunk makes.
     */
    q2_save_creature *creatures;
    u32               creature_count;

    /* --- presentation ---------------------------------------------------- */
    q2_save_level_stats mission[Q2_SAVE_MISSION_ROWS];
    s32  mission_unit;

    s16  settings[Q2_SAVE_SETTINGS_MAX];
    u32  settings_count;
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

/*
 * The movers, which live in the CLIENT rather than the sim — the same shape as
 * the mission tallies below, and for the same reason: `q2_save_capture` is
 * handed a `q2_sim` and the door set is not in one.
 */
void q2_save_capture_movers(q2_save *s, const q2_mover_set *set);
void q2_save_apply_movers(const q2_save *s, q2_mover_set *set);

/* And the creatures, for the same reason: the set lives in the client. The
 * count must match on apply, or the save is for a different population and
 * nothing is restored. */
void q2_save_capture_creatures(q2_save *s, const q2_monster_set *set);
void q2_save_apply_creatures(const q2_save *s, q2_monster_set *set);

/* The mission tallies, which live in the client rather than the sim (mission.h
 * — the counters are inputs to the screen). Both are no-ops on NULL. */
void q2_save_capture_mission(q2_save *s, const struct q2_mission *m);
void q2_save_apply_mission(const q2_save *s, struct q2_mission *m);

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

/* A default label for a new save: the map, zone and elapsed time. Returns
 * `out`. */
const char *q2_save_default_label(const q2_save *s, char *out, u32 out_size);

#endif /* Q2PSX_SAVE_H */
