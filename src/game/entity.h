/*
 * entity.h — the entity record, and the per-tick dispatch over it.
 *
 * Until now the port had a monster set, a pickup set, a mover set and a
 * projectile list, each with its own loop. The engine has one kind of thing:
 * a record with a THINK POINTER at +0x3C that a single sweep calls every tick.
 * Items, movers, doors, projectiles, gibs and creatures are all that record
 * with a different think installed. Modelling that directly is what lets an
 * item respawn, a door open and a rocket fly under one clock.
 *
 * ---------------------------------------------------------------------------
 * The record, and where each field is
 * ---------------------------------------------------------------------------
 * The engine's entity is at least 0x2E0 bytes and most of it is renderer state
 * this port does not need. What is reproduced here is every field the item and
 * spawn paths actually touch, each carrying the offset it came from so a reader
 * can go back to the disassembly:
 *
 *     +0x10   model            the CastList node, looked up by name (0x8006D008)
 *     +0x2C   spawn_origin     position at spawn, y already raised by 286
 *     +0x3C   think            called every tick by the sweep
 *     +0x44   flags            the item table's flags, verbatim
 *     +0x48   effect           the touch dispatch index
 *     +0x4C   respawn_at       deathmatch respawn countdown
 *     +0x54   pos              FEET, copied straight from the place record
 *     +0x78   bounds           the box the touch test overlaps
 *     +0xA4   origin           what the renderer uses: pos.y + 286 - model_off
 *     +0xD2   kind             46 for an item (0x80059A60)
 *     +0xDA   place_id         the id the place record carried
 *     +0xE6   angles           SVECTOR; yaw at +0xE8 is the spin
 *     +0xF4   remove_in        the Q2_ITEM_TIMED countdown
 *     +0xF8   model_offset     lh model[+0x1C], the model's own height bias
 *     +0xFC   light intensity  4096 == 1.0; 0 while materialising
 *     +0xFE   light fade       second intensity factor; explosion ramp
 *     +0x100  frame            animation cursor, wrapped by the clip length
 *     +0x108  health
 *     +0x10C  render_flags     0x08000001 at spawn; bit 1 cleared when !(flags&1)
 *     +0x118  per-player state 100 bytes each; bit 0x80 is "taken by this player"
 *     +0x2AC  glow             r, g, b written by the glow block
 *     +0x2C0  matrix           rebuilt from angles whenever the item spins
 *
 * ---------------------------------------------------------------------------
 * Time
 * ---------------------------------------------------------------------------
 * One clock, 300 ticks to the second, which is the same clock sim.h runs and
 * combat.h documents. Every duration in the item path is a multiple of it:
 * a powerup is 9000 (30 s), the pickup caption 900 (3 s), a deathmatch respawn
 * 9000, mega health's decay interval 1500 (5 s), a dropped item's life 1500.
 *
 * The per-tick advance is the engine's `0x800B2DB4`, which is dt in those units
 * — NOT a frame count. A think that decrements by 1 per call would run 300
 * times too slowly at 25 Hz.
 */
#ifndef Q2PSX_GAME_ENTITY_H
#define Q2PSX_GAME_ENTITY_H

#include "collision.h"   /* the placement drop traces against SecondaryCol */
#include "fixed.h"
#include "gamevars.h"
#include "inventory.h"
#include "itemtable.h"
#include "q2psx.h"
#include "weapon.h"      /* q2_rng — the engine's own pseudo-random source */
#include "worldscale.h"

/* ------------------------------------------------------------------------- */
/* The shared placement drop — the tail of 0x80050FA0                        */
/* ------------------------------------------------------------------------- */
/*
 * EVERY placed entity in this engine is dropped onto the floor at spawn, and
 * the port did not do it for any of them.
 *
 * 0x80050FA0 is the shared placement routine — the item spawner, the creature
 * spawner, the save restore and two others all reach it. Its tail lifts the
 * record's Y by 286 and then by a further 30 (`addiu v0, v0, -286` at
 * 0x80051068 and `addiu v0, v0, -30` at 0x800510A4, +Y being down), and then
 * runs a DOWNWARD SWEEP of up to `dist` units against SecondaryCol, writing
 * back the Y it stopped at — offset +4 and nothing else (0x800511C0) — and the
 * cell it ended in at +0x4E (0x80051228).
 *
 * The distance is the caller's. The item spawner passes 1024 (`sll a3, a3, 10`
 * at 0x80059A50) unless the item's table flag 0x100 is set, and the creature
 * spawner passes the same 1024 out of the word at 0x800AE894, whose halfwords
 * are 0xFFFF and 0x0400 — a zone hint of -1 and a drop of 1024.
 *
 * So the authored Y is a HINT, not a position: the level is authored with
 * things floating above their floor and the engine settles them. Without it an
 * item hangs where the record put it and a creature hangs in the air — which
 * is why a creature could be reasoned about correctly by the collision layer
 * and still look wrong, and why "monsters stuck in geometry" survived two
 * rounds of collision fixes that were each individually right.
 *
 * `pos` is in and out, in the ENTITY ORIGIN frame, and only its Y is written.
 * Returns false when the sweep could not place the start at all, which the
 * original treats as a hard failure — it frees the item and error-loops on a
 * creature — so a caller must not carry on as if the entity were placed.
 */
bool q2_entity_drop_to_floor(q2_collision *coll, s32 pos[3], s32 dist,
                             s32 *out_node);

/* The nudge above the record before the sweep, 0x800510A4. */
#define Q2_ENTITY_SPAWN_NUDGE 30
/* What the item and creature spawners both pass — 0x80059A50 and 0x800AE894. */
#define Q2_ENTITY_DROP_DIST   1024

struct q2_entity;
struct q2_entity_world;

/* Four, because the per-player block inside the entity record is 100 bytes with
 * four of them between +0x118 and +0x2A8, and the touch sweep walks
 * `0x800B2C2C` of them. */
#define Q2_MAX_PLAYERS 4

/*
 * `render_flags` bit 0x01000000 — what makes an entity EVICTABLE.
 *
 * The allocator at 0x8006C0F0 recycles an entity that carries this AND a
 * non-zero +0xF4 when the pool runs dry. Both marks together, which is why a
 * spawned item (timed respawn, no bit) is not a candidate and a transient
 * effect is.
 *
 * It is also what tells the draw that an entity was PLACED BY ITS SPAWNER
 * rather than by a Population record, so the item vertical bias does not apply
 * to it — see entitydraw.c and modelent.h.
 */
#define Q2_RF_TRANSIENT 0x01000000u

/* 0x8006DCD8: models carrying this bit are registered on an area's 128-entry
 * Quick list instead of its 32-entry Standard list. The record remains a box
 * record; only the dependency graph it participates in changes. */
#define Q2_RF_QUICK_SORT 0x00800000u

/* entity+0xD2. 46 is what the item spawner writes (0x80059A60); other values
 * belong to entity kinds this module does not own yet. */
#define Q2_ENT_KIND_ITEM 46

/*
 * A transient MODEL ENTITY — an effect model with a think and no pickup.
 *
 * Not a value read out of the executable: 0x8005A778 never writes +0xD2 at all,
 * so the console leaves it at the allocator's zero and tells the two apart by
 * the evictable marks instead (modelent.h). This port needs a name for "not an
 * item" in the same field, and picks one outside the item table's range rather
 * than inventing a console value that is not there.
 */
#define Q2_ENT_KIND_MODEL 0x1000

/* ------------------------------------------------------------------------- */
/* An entity                                                                  */
/* ------------------------------------------------------------------------- */
typedef void (*q2_think_fn)(struct q2_entity *self, struct q2_entity_world *w);

typedef struct q2_entity {
    bool in_use;

    q2_think_fn think;          /* +0x3C */
    u16  kind;                  /* +0xD2 */

    s32  pos[3];                /* +0x54, feet    */
    s32  origin[3];             /* +0xA4, draw    */
    s32  spawn_origin[3];       /* +0x2C          */
    s32  angles[3];             /* +0xE6, 4096-step circle */

    s32  bounds_min[3];         /* +0x78 */
    s32  bounds_max[3];

    u32  flags;                 /* +0x44 */
    u32  render_flags;          /* +0x10C */
    u32  effect;                /* +0x48 */
    u16  place_id;              /* +0xDA */

    /* Historical field name retained in the save/API: this is LIGHT
     * INTENSITY, not a geometric model scale. */
    s16  scale;                 /* +0xFC */

    /*
     * THE SECOND LIGHT-INTENSITY FACTOR — +0xFE.
     *
     *     8006B298  lh   v1, 252(s2)      ; +0xFC, `scale`
     *     8006B29C  lh   v0, 254(s2)      ; +0xFE, this
     *     8006B2A4  mult v1, v0           ; ...and the product scales a row
     *     8006B2BC  sra  a1, v1, 11       ;    of the GTE LIGHT matrix
     *
     * The destination is the matrix at 0x800DDD1C, installed by SetLightMatrix
     * at 0x8006BBD4. The model rotation installed next comes independently
     * from entity+0x2C0. The same product scales the ambient back colour at
     * 0x8006B468, so neither field changes geometry.
     *
     * The allocator initialises both to 4096 (0x8006C1B8). `scale` is what an
     * item's materialise ramp drives; this field is what the model-entity
     * explosion fades from full to zero over the back 61% of its life.
     */
    s16  fade;                  /* +0xFE, historical name */

    s16  model_offset;          /* +0xF8 */
    s32  frame;                 /* +0x100 */
    s16  health;                /* +0x108 */

    /* +0x4E — the cell the placement drop ended in, which q2_move_ent calls
     * `node` at the same offset (trace.h). -1 when nothing placed it. */
    s32  node;

    /*
     * The surface byte the spawner is handed — +0x9E, `sb s4, 158(s1)` at
     * 0x8005AA40. Opcode 0x08 passes the Scene node's area byte & 0x7F, whose
     * low seven bits index retail's 64-byte screen-area table. Movement writes
     * the occupied collision cell's byte +32 here as well (0x80046B08), and
     * entitydraw.c now uses it to route the model's private packet chain.
     */
    u8   surface;               /* +0x9E */

    /* +0x90, written with 128 at 0x8005A9D8. No decoded reader. */
    s16  field90;

    /* The explosion light's outer radius, computed every tick before the
     * 0x80075C34 runtime-light call. See modelent.h. */
    s32  flash;

    s32  remove_in;             /* +0xF4 */
    s32  respawn_at;            /* +0x4C */
    bool taken[Q2_MAX_PLAYERS]; /* +0x118 + n*100, bit 0x80 */
    bool hidden;                /* 0x8005B6C0 took it out of the draw list */

    u8   glow[3];               /* +0x2AC */

    /*
     * The model. The engine holds a pointer to the CastList node it found by
     * name. The item table's final list is copied separately because it names
     * posed vertices that expand the floor-shadow footprint; every shipped
     * item model has one animation clip and always plays clip zero.
     */
    char model[Q2_ITEM_MODEL_LEN + 1];
    u16  shadow_vertex[Q2_ITEM_SHADOW_VERTEX_MAX];
    u8   shadow_vertex_count;
    s32  clip_length;           /* lh model[+2], the wrap the frame uses */

    /*
     * Where the model was found. The engine resolves once at spawn and holds a
     * pointer (0x80058850); this holds the bank AND the index, because a map's
     * models are spread over COMMON's CastList and up to eight zones' and an
     * index is only meaningful against the bank it came from. Resolving against
     * one bank and drawing against another is exactly the bug this pair
     * prevents. -1 is "not resolved".
     */
    s32                         model_index;
    const struct q2_model_bank *model_bank;

    /* The table record this came from, when it came from one. Borrowed. */
    const q2_item_def *def;

    /*
     * Stable Population identity for save reconstruction. Runtime entity
     * slots are reusable, so an ordinal in q2_entity_set is not an item's
     * identity after a collected pickup and a later CREBATCH. -1 means this
     * entity did not come from a Population place record (a dropped item,
     * effect model, or front-end prop).
     */
    s32 population_group;
    u32 population_slot;
} q2_entity;

void q2_entity_init(q2_entity *e);

/* True when the entity should be drawn: alive, not hidden, and not already
 * collected by this player. */
bool q2_entity_visible(const q2_entity *e, u32 player);

/* The engine's box overlap at 0x800552D8, on the +0x78 bounds. */
bool q2_entity_bounds_overlap(const q2_entity *a, const q2_entity *b);

/* Set the +0x78 box to a cube of `half` about the draw origin. The engine fills
 * it from the model, which this port does not read yet — so the extent is
 * INFERRED and comes from worldscale.h rather than being invented here. */
void q2_entity_set_bounds(q2_entity *e, s32 half);

/* ------------------------------------------------------------------------- */
/* What a think wants to tell the world about                                 */
/*                                                                            */
/* The engine calls the sound and light services directly. This port has no    */
/* audio path wired into the game layer yet, so a think records what it would  */
/* have played and the caller drains it. That keeps the behaviour testable and */
/* keeps the game layer free of a dependency it does not have.                */
/* ------------------------------------------------------------------------- */
typedef enum q2_ent_sound {
    Q2_SND_ITEM        = 0,   /* itm_pkup     */
    Q2_SND_WEAPON      = 1,   /* wep_noammo   */
    Q2_SND_AMMO        = 2,   /* msc_am_pkup  */
    Q2_SND_MEGA_HEALTH = 3,   /* itm_m_health */
    Q2_SND_LARGE_MEDKIT= 4,   /* itm_l_health */
    Q2_SND_MEDKIT      = 5,   /* itm_n_health */
    Q2_SND_ARMOUR      = 6,   /* msc_ar1_pkup */
    Q2_SND_ARMOUR_SHARD= 7,   /* msc_ar2_pkup */
    Q2_SND_POWER       = 8,   /* msc_power1   */
    Q2_SND_QUAD        = 9,   /* itm_damage3  */
    Q2_SND_PROTECT     = 10,  /* itm_protect  */
    Q2_SND_TELEPORT    = 11,  /* msc_tele1, the materialise effect */

    /*
     * The player's own, from the movement frame rather than from an entity
     * think — the sound pointers at 0x800B2914, 0x800B2918, 0x800B292C,
     * 0x800B28EC and 0x800B294C..0x800B2958. They arrive on this channel
     * because it is the one a headless caller can already drain, and because
     * the alternative is a second events queue that exists to carry six ids.
     */
    Q2_SND_FOOTSTEP_A  = 12,  /* 0x800B2914, 0x8003AAE8 */
    Q2_SND_FOOTSTEP_B  = 13,  /* 0x800B2918, 0x8003AAF4 */
    Q2_SND_FOOTSTEP_WET= 14,  /* 0x800B292C, 0x8003AAC0 — in the 0x4 state  */
    Q2_SND_LAND        = 15,  /* 0x800B28EC, 0x80039D6C — a HARD landing    */
    Q2_SND_PAIN_25     = 16,  /* 0x800B294C, health under 25                */
    Q2_SND_PAIN_50     = 17,  /* 0x800B2950                                 */
    Q2_SND_PAIN_75     = 18,  /* 0x800B2954                                 */
    Q2_SND_PAIN_100    = 19,  /* 0x800B2958                                 */

    /*
     * DYING, which the player had never done audibly. The death handler picks
     * between these two on the client's own air/underwater field and explicitly
     * does NOT raise a pain grunt on the killing tick — so the port, which ran
     * update_pain unconditionally, played a wounded noise for a death.
     */
    Q2_SND_DEATH       = 20,  /* 0x800B28E4, pla_death4                     */
    Q2_SND_DROWN       = 21,  /* 0x800B28E8, pla_drown1 — drowned instead   */

    /*
     * The jump's own sound, and the SOFT landing — the two the player makes
     * most often and the two this port made none of.
     *
     * `Q2_SND_JUMP` is 0x800B2900, played at 0x8003E214 on the jump's success
     * path only. `Q2_SND_LAND_SOFT` is 0x800B2904, played at 0x80039DBC —
     * which is the arm `0x80039D50 slti v0, s0, 31` branches to, i.e. the band
     * BELOW the damage threshold. This file used to record that a landing
     * softer than 31 was silent; it is the opposite, and it is why an ordinary
     * hop had no audio at either end of it.
     *
     * Both names come out of the run of find_sound / sw pairs at 0x8003BA00,
     * where the compiler hoists the NEXT name's setup above the current store —
     * so 0x800B2900 takes 0x800AC488 "pla_jump1" and 0x800B2904 takes
     * 0x800AC494 "pla_land1", one slot earlier than the addresses suggest.
     */
    Q2_SND_JUMP        = 22,  /* 0x800B2900, 0x8003E214 — pla_jump1         */
    Q2_SND_LAND_SOFT   = 23,  /* 0x800B2904, 0x80039DBC — pla_land1         */

    /*
     * The water transitions, 0x8003D254. Both the submerge at 0x8003D2DC and
     * the shallow wade at 0x8003D4A0 play the SAME sound; only leaving the
     * water has its own. Same hoisting trap as above — 0x800B2938 takes the
     * name at 0x800AC4D0 and 0x800B293C the one at 0x800AC4DC.
     */
    Q2_SND_WATER_IN    = 24,  /* 0x800B2938 — pla_watr_in                   */
    Q2_SND_WATER_OUT   = 25,  /* 0x800B293C — pla_watr_out                  */
    Q2_SND_WATER_UNDER = 26,  /* 0x800B2940 — pla_watr_un                   */
    Q2_SND_GASP         = 27, /* 0x800B28F8 — pla_gasp1                     */

    Q2_SND_COUNT
} q2_ent_sound;

/*
 * The per-tick event queue.
 *
 * This was 32, and 32 is exactly the size of the projectile pool — one-for-one,
 * because every live projectile raises one LIGHT event per tick. A frame with
 * the pool full therefore filled the queue before the entity sweep ran, and
 * every item glow, creature sound and pain grunt raised after it was dropped.
 * Measured at the time: a 200-frame standing `--shoot` run reported
 * "2704 lights added, 1526 dropped".
 *
 * The saturation itself was a symptom of projectiles moving at a twentieth of
 * their speed and never reaching a wall, and that is fixed — the steady state
 * is now one or two bolts alive. The ceiling is raised anyway, because
 * one-to-one with the projectile pool leaves the queue with no room for the
 * rest of the world in a busy fight, and dropping a pain grunt to a rocket is
 * not a trade anything here wants to make.
 *
 * Not a read constant: the console appends to the light list directly and has
 * no queue of this kind. It is the port's seam and is sized for headroom.
 */
#define Q2_ENT_EVENTS_MAX 96

typedef enum q2_ent_event_kind {
    Q2_ENT_EVENT_SOUND = 0,   /* play `sound` at `pos`                        */
    Q2_ENT_EVENT_LIGHT,       /* a dynamic light of `glow` and `radius`       */
    Q2_ENT_EVENT_BURST        /* the pickup particle burst, 0x8005B6C0        */
} q2_ent_event_kind;

typedef struct q2_ent_event {
    u8  kind;           /* a q2_ent_event_kind                            */
    u8  sound;          /* a q2_ent_sound; only for Q2_ENT_EVENT_SOUND    */
    s32 pos[3];
    u8  glow[3];        /* Q2_ENT_EVENT_LIGHT and _BURST                  */

    /*
     * Which model the burst belongs to, for _BURST only, or -1.
     *
     * The original's burst counts the item's own VERTICES to decide how many
     * quads to throw (0x8006D6AC, summing each 8-byte part's `num_verts` at
     * +3), so a drawing side has to know which model it was. The game half has
     * no bank to count with; the client does.
     */
    s32 model_index;
    s32 radius;         /* Q2_ENT_EVENT_LIGHT: the OUTER radius            */
    s32 inner_radius;   /* Q2_ENT_EVENT_LIGHT: the inner, 0 if unspecified  */
} q2_ent_event;

typedef struct q2_ent_events {
    q2_ent_event e[Q2_ENT_EVENTS_MAX];
    u32          count;
    u32          dropped;
} q2_ent_events;

void q2_ent_events_clear(q2_ent_events *ev);
void q2_ent_sound_at(q2_ent_events *ev, q2_ent_sound which, const s32 pos[3]);
/*
 * Raise a dynamic light.
 *
 * BOTH radii are carried. They were not always: the event held only the outer,
 * and the client recovered the inner by matching the outer against the presets
 * it knew (800 -> 300, 1400 -> 1000, 1300 -> 800). That worked while every
 * preset had a distinct outer and would have failed silently the first time two
 * shared one — a light of the wrong size, on a path with no error to report.
 * Pass `inner` as 0 to mean "unspecified" and let the consumer choose.
 */
void q2_ent_light_at(q2_ent_events *ev, const s32 pos[3], const u8 glow[3],
                     s32 inner_radius, s32 radius);
void q2_ent_burst_at(q2_ent_events *ev, const s32 pos[3], const u8 glow[3],
                     s32 model_index);

/* ------------------------------------------------------------------------- */
/* The world a think runs in                                                  */
/* ------------------------------------------------------------------------- */
typedef struct q2_entity_player {
    bool          present;
    q2_inventory *inv;      /* borrowed; NULL means "no client" */
    s32           pos[3];   /* feet */
    q2_entity     ent;      /* the box the touch test overlaps  */
} q2_entity_player;

typedef struct q2_entity_world {
    s32 level_time;     /* 0x800AEBAC, 300 ticks to the second */
    s32 dt;             /* 0x800B2DB4, this tick's advance     */

    bool deathmatch;    /* 0x800AEBCC */
    bool weapons_stay;  /* 0x800B3360 */
    u32  cheats;        /* the GAME VARIABLES bits at 0x800B29EC */

    q2_entity_player player[Q2_MAX_PLAYERS];
    u32              player_count;   /* 0x800B2C2C */

    const q2_item_table *items;      /* borrowed; NULL uses the built-in */

    q2_ent_events events;

    /*
     * The engine's pseudo-random source, for the materialise sparkles. Held
     * here so a test can drive it and get the same fifteen offsets twice.
     */
    q2_rng rng;
} q2_entity_world;

void q2_entity_world_init(q2_entity_world *w);

/* Register a player. `inv` is borrowed and must outlive the world. */
void q2_entity_world_add_player(q2_entity_world *w, u32 slot,
                                q2_inventory *inv, const s32 pos[3]);

/* Move a registered player, keeping its touch box in step. */
void q2_entity_world_move_player(q2_entity_world *w, u32 slot,
                                 const s32 pos[3]);

/* ------------------------------------------------------------------------- */
/* The set, and the sweep                                                     */
/* ------------------------------------------------------------------------- */
typedef struct q2_entity_set {
    q2_entity *ent;
    u32        count;
    u32        capacity;
} q2_entity_set;

void       q2_entity_set_free(q2_entity_set *set);
q2_entity *q2_entity_alloc(q2_entity_set *set);
void       q2_entity_remove(q2_entity *e);

/*
 * One tick: call every live entity's think.
 *
 * `w->dt` is the advance in level-clock units; `w->level_time` is advanced by
 * this function, once, before the sweep — which is the order the engine uses,
 * so a think comparing against `level_time` sees the new value.
 *
 * Returns how many thinks ran.
 */
u32 q2_entity_run(q2_entity_set *set, q2_entity_world *w);

#endif /* Q2PSX_GAME_ENTITY_H */
