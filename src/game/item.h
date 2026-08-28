/*
 * item.h — items in the world: spawning them, animating them, collecting them.
 *
 * This is the whole of the original's item path, transcribed. Three functions in
 * the executable do all of it and each is named here with its address, because
 * every number below came out of one of them:
 *
 *     0x800599DC   spawn   place record + item table -> a live entity
 *     0x80059330   think   animate, spin, materialise, glow, respawn, touch
 *     0x80036348   touch   a 55-entry dispatch: what collecting one DOES
 *
 * The table those read is decoded in itemtable.h. What this file adds is the
 * behaviour, and the behaviour is where the numbers live: the dispatch's
 * handlers carry their amounts as immediate operands, so there is no table to
 * read and each one had to be disassembled. All 43 live handlers are below with
 * the instruction address each amount came from.
 *
 * ---------------------------------------------------------------------------
 * The touch protocol, and why it has three outcomes rather than two
 * ---------------------------------------------------------------------------
 * `0x80036348` returns 0, 1 or 2, and the difference is not cosmetic:
 *
 *     2   nothing happened. The sweep moves to the next player and the item
 *         stays exactly as it was. Returned when the effect index is out of
 *         range, when its slot is inert, or when the effect could not apply —
 *         full health, full ammo, a weapon already held under weapons-stay.
 *     0   collected. The item plays its sound, bursts particles and is either
 *         freed (single player) or hidden with a respawn timer (deathmatch).
 *     1   collected, but it was a WEAPON-style pickup, which matters only
 *         because weapons-stay in deathmatch keeps the item in the world for
 *         everyone else. Every handler that returns 1 clears the same register
 *         (`s6`) that the epilogue XORs with 1 (0x80037308).
 *
 * The caller then folds 1 back into 0 unless deathmatch AND weapons-stay are
 * both on (0x80059878…0x800598B0) — so outside deathmatch a weapon behaves
 * exactly like anything else.
 *
 * ---------------------------------------------------------------------------
 * INFINITE AMMO changes what a weapon pickup gives
 * ---------------------------------------------------------------------------
 * Every weapon handler begins by testing bit 0 of the GAME VARIABLES word at
 * 0x800B29EC, which the menu sets from the INFINITE AMMO toggle
 * (0x8001C718, and FORMATS.md's menu table names it). When it is on, the weapon
 * grants the *capacity* of its ammo type instead of a fixed amount. That is a
 * real behaviour difference reachable from the pause menu, so it is modelled
 * rather than ignored.
 *
 * ---------------------------------------------------------------------------
 * Two asymmetries that are the original's, not this port's
 * ---------------------------------------------------------------------------
 *   - Body and jacket armour refuse the pickup when armour is already at the
 *     class cap (0x80036C60, 0x80036D40). **Combat armour has no such test**
 *     (0x80036CB0 goes straight to the clamped add), so it is always collected.
 *   - Mega health, adrenaline, the silencer and the power shield never refuse
 *     either: they set the "took something" flag unconditionally.
 *
 * Both are reproduced. A port that tidied them would diverge on the first
 * full-armour player who walks over combat armour.
 */
#ifndef Q2PSX_ITEM_H
#define Q2PSX_ITEM_H

#include "entity.h"
#include "itemtable.h"
#include "population.h"
#include "q2psx.h"

/* ------------------------------------------------------------------------- */
/* Durations, all on the 300 Hz level clock                                   */
/* ------------------------------------------------------------------------- */
#define Q2_ITEM_POWERUP_TICKS    9000   /* 30 s  0x800370E8               */
#define Q2_ITEM_CAPTION_TICKS     900   /* 3 s   0x80037300               */
#define Q2_ITEM_MEGA_DECAY_TICKS 1500   /* 5 s   0x80036E58               */
#define Q2_ITEM_DROP_LIFE        1500   /* 0x80059AB4, the spawn default  */
#define Q2_ITEM_RESPAWN_TICKS    9000   /* 30 s  0x80059908               */
#define Q2_ITEM_RESPAWN_MEGA    32400   /* 108 s 0x8003790C — effect 32   */
#define Q2_ITEM_SHRINK_RESET        2   /* 0x800597F4                     */

/* The materialise ramp and the spin rate, per tick of the level clock. */
#define Q2_ITEM_SCALE_RATE          2   /* 0x800595B4: scale += 2 * dt    */
#define Q2_ITEM_SPIN_RATE           3   /* 0x8005947C: yaw  -= 3 * dt     */
#define Q2_ITEM_SHRINK_RATE        16   /* 0x8005B36C: scale -= 16 * dt   */
#define Q2_ITEM_SPARKLES           15   /* 0x8005963C                     */

/* The dynamic light the glow flags raise: a quarter of the pulse value per lit
 * channel (0x80059704) and a radius of the pulse plus 100 (0x80059798). The
 * pulse itself comes from the engine's own sine table indexed by level time,
 * which this port does not reproduce — see q2_item_glow_pulse. */
#define Q2_ITEM_GLOW_RADIUS_BIAS  100
#define Q2_ITEM_LIGHT_OFFSET_X    (-50)   /* 0x80059780 */
#define Q2_ITEM_LIGHT_OFFSET_Y   (-500)   /* 0x80059790 */

/*
 * The touch box, CONFIRMED.
 *
 * The placement call at 0x80050FA0 writes the entity's mins and maxs from two
 * halfwords at 0x800AEAB8 and 0x800AEABA, which read **286** and **572**:
 *
 *     mins = (-286, -572 + 286, -286) = (-286, -286, -286)
 *     maxs = (+286,        286, +286)
 *
 * — a 286-unit cube about the entity's position, the same half-extent as the
 * player's own hull. The absolute box the overlap test reads is then
 * `position + mins/maxs` (0x8005A9DC), so both boxes are cubes about their
 * entity's +0x54 and an item is reachable when they overlap.
 */
#define Q2_ITEM_TOUCH_HALF Q2_SWEEP_HALF_EXTENT

/*
 * The spawn lift, also CONFIRMED, and the one number an earlier reading of this
 * path would have got wrong.
 *
 * 0x80051060 copies the place record's xyz into the entity and then RAISES it
 * twice — `y -= 286` at 0x80051068 and `y -= 30` at 0x800510A4, +Y being down —
 * before handing the entity to the hull query at 0x80044F54. An item therefore
 * does not sit where its place record says; it sits 316 units above it, and the
 * draw origin's own +286 is applied on top of that.
 */
#define Q2_ITEM_SPAWN_LIFT (Q2_EYE_BASE + 30)

/* ------------------------------------------------------------------------- */
/* What an effect index does                                                  */
/* ------------------------------------------------------------------------- */
typedef enum q2_item_fx_kind {
    Q2_ITEMFX_NONE = 0,        /* the dispatch's failure exit, or out of range   */
    Q2_ITEMFX_WEAPON,
    Q2_ITEMFX_AMMO,
    Q2_ITEMFX_ARMOUR,
    Q2_ITEMFX_ARMOUR_SHARD,
    Q2_ITEMFX_POWER_SHIELD,
    Q2_ITEMFX_HEALTH,
    Q2_ITEMFX_MEGA_HEALTH,
    Q2_ITEMFX_ADRENALINE,
    Q2_ITEMFX_BANDOLIER,
    Q2_ITEMFX_AMMO_PACK,
    Q2_ITEMFX_SILENCER,
    Q2_ITEMFX_POWERUP,
    Q2_ITEMFX_KEY,
    Q2_ITEMFX_KIND_COUNT
} q2_item_fx_kind;

/* Which of the four powerup words at client+0xAC an effect extends. */
typedef enum q2_powerup_slot {
    Q2_POWERUP_QUAD = 0,      /* client+0xAC */
    Q2_POWERUP_INVULN,        /* client+0xB0 */
    Q2_POWERUP_ENVIRO,        /* client+0xB4 */
    Q2_POWERUP_BREATHER,      /* client+0xB8 */
    Q2_POWERUP_SLOT_COUNT
} q2_powerup_slot;

typedef struct q2_item_fx {
    u8  kind;        /* a q2_item_fx_kind                                        */
    s8  subject;     /* weapon id / q2_ammo / q2_armour_class / slot / -1    */
    s16 amount;      /* single player                                        */
    s16 amount_dm;   /* deathmatch                                          */
    u8  sound;       /* a q2_ent_sound                                      */
    u32 key_bit;     /* Q2_ITEMFX_KEY only                                      */
    u32 addr;        /* the handler this row was read from                  */
} q2_item_fx;

/* Indexed by effect index, so slot 0 covers effects 0 and 1 (both out of the
 * dispatch's range) and the array runs to Q2_ITEM_EFFECT_LAST. */
const q2_item_fx *q2_item_effect_def(u32 effect);

/* A short name for an effect, for the offline tools. Never NULL. */
const char *q2_item_effect_name(u32 effect);

/* ------------------------------------------------------------------------- */
/* Spawning                                                                   */
/* ------------------------------------------------------------------------- */
typedef struct q2_item_spawn_stats {
    u32 places;         /* place records seen                              */
    u32 spawned;
    u32 no_def;         /* no table record matched the place id            */
    u32 inert;          /* spawned, but its effect index has no handler    */
    u32 scenery;        /* spawned with effect 0 — decoration by design    */
    u32 no_memory;
    u32 skill_filtered; /* +0x0C difficulty flags rejected this place       */
    u32 other_zone;     /* groups skipped: their name claims another zone  */
    u32 not_selected;   /* unzoned groups the LevelBin did not select       */
} q2_item_spawn_stats;

/*
 * One place record -> one entity, exactly as 0x800599DC does it.
 *
 * `model_offset` is `lh model[+0x1C]` — the model's own vertical bias, which the
 * engine reads from the CastList node it just looked up. A caller that has the
 * model bank open passes it; 0 is correct for a caller that does not, and only
 * shifts the draw origin.
 *
 * Returns NULL when no table record matches the place id, which is the engine's
 * behaviour too: nothing spawns and the record is silently skipped.
 */
q2_entity *q2_item_spawn(q2_entity_set *set, const q2_pop_place *place,
                         const q2_item_table *table, s16 model_offset,
                         q2_collision *coll);

/*
 * Every place record of every group selected for the loaded zone. Path groups
 * carry place lists too — the two lists are independent — so unlike the
 * creature walk this does not skip them.
 *
 * `zone` drops the groups the disc says belong somewhere else: a group whose
 * name claims a zone (q2_pop_group_zone) is spawned only when that is the zone
 * being loaded. Pass -1 for every group regardless, which is what a census over
 * a whole map wants.
 *
 * `levelbin` is the map's own LevelBin module. `q2_levelbin_selected` recovers
 * its calls to the retail group selector, so an unzoned group such as `Weapons`
 * or `ShotgunRoom` spawns only when the module selected it. This is the same
 * rule `q2_creature_world_hold_batches` applies to creature groups. A resident
 * `Zone<N>` group remains the fallback when the module is absent or empty;
 * unzoned script batches do not appear early.
 *
 * Before either spawner runs, retail's list walker at `0x8007F538` rejects a
 * place carrying NOT_EASY / NOT_MEDIUM / NOT_HARD for the current global
 * skill. Zone -1 remains a raw census and deliberately bypasses that filter.
 *
 * `group_run` is optional. When supplied it has `pop->group_count` bytes and
 * records every group this startup pass actually ran, ready for
 * q2_item_spawn_group to enforce the same retail bit-1 latch at runtime.
 */
q2_result q2_item_spawn_zone(q2_entity_set *set, const q2_population *pop,
                             int zone, const u8 *levelbin, u32 levelbin_size,
                             const q2_item_table *table,
                             q2_collision *coll,
                             u8 *group_run,
                             q2_item_spawn_stats *stats);

/*
 * Spawn one Population group's place list by its twelve-byte name.
 *
 * `group_run`, when non-NULL, has `pop->group_count` bytes and is the port's
 * shadow of the Population group flags' retail bit-1 latch. A group selected
 * at level load or by CREBATCH sets its byte before the place-list walk; asking
 * for it again succeeds but spawns nothing. Pass the same bitmap to
 * `q2_item_spawn_zone` and this function so startup and runtime selection share
 * one latch.
 */
q2_result q2_item_spawn_group(q2_entity_set *set,
                              const q2_population *pop,
                              const char *group,
                              const q2_item_table *table,
                              q2_collision *coll,
                              u8 *group_run,
                              q2_item_spawn_stats *stats);

/* Every group of the map, whichever zone it names. `q2_item_spawn_zone` with
 * -1, kept under its own name because a census is a different intent. */
q2_result q2_item_spawn_all(q2_entity_set *set, const q2_population *pop,
                            const q2_item_table *table,
                            q2_item_spawn_stats *stats);

/* ------------------------------------------------------------------------- */
/* The think                                                                  */
/* ------------------------------------------------------------------------- */

/* 0x80059330. Installed by q2_item_spawn; exposed so a test can call it. */
void q2_item_think(q2_entity *e, q2_entity_world *w);

/* 0x8005B358. The think a collected Q2_ITEM_TIMED entity switches to: shrink
 * away, then free. */
void q2_item_shrink_think(q2_entity *e, q2_entity_world *w);

/*
 * The glow pulse the light block reads.
 *
 * The engine indexes a table at 0x800A5430 by `(0x800AEBAC << 6) & 0x3FC0`,
 * i.e. by the low 8 bits of the level time, and takes `(value + 4096) >> 4`.
 * The table is the engine's 256-entry signed sine, which this port has in
 * trig.h — so this is a reproduction of the arithmetic, not of the table.
 */
s32 q2_item_glow_pulse(s32 level_time);

/* ------------------------------------------------------------------------- */
/* The touch                                                                  */
/* ------------------------------------------------------------------------- */
typedef enum q2_touch_result {
    Q2_TOUCH_COLLECTED = 0,   /* s6 stayed 1: the epilogue returns 0        */
    Q2_TOUCH_WEAPON    = 1,   /* s6 was cleared: weapons-stay applies       */
    Q2_TOUCH_NOTHING   = 2    /* s3 stayed 0                                */
} q2_touch_result;

/*
 * 0x80036348, with the entity and the client separated out.
 *
 * `pos` is where the sound is placed, which the engine takes from the PLAYER's
 * own +0x54 rather than the item's (0x80036450: `a1 = s4 + 84`).
 */
q2_touch_result q2_item_touch(u32 effect, q2_inventory *inv, const s32 pos[3],
                             q2_entity_world *w);

/* The armour cross-conversion at 0x80037338: how much armour picking up class
 * `cls` yields, and the class/value bookkeeping it does on the way. */
s16 q2_item_armour_amount(q2_inventory *inv, u8 cls);

/* ------------------------------------------------------------------------- */
/* What the HUD shows for the last thing collected                            */
/* ------------------------------------------------------------------------- */
/*
 * 0x800359C0's prologue — the status bar's fourth sub-draw, and the reader of
 * the two fields `q2_item_touch` writes at 0x800372F0.
 *
 * The console draws a pickup caption from the SAME per-viewport hook that
 * draws the bar (`0x800337D0`), every frame, for as long as the deadline
 * holds: an icon in the bar's upper-left field and a line of text beside it.
 * It is not a notification — it does not age, does not stack and does not take
 * a slot in the four-line ring.
 *
 * Returns false when there is nothing to draw. Otherwise `*icon` is the ICON
 * RECT INDEX, which for an item is the effect id itself (the sub-draw does
 * `index * 5 + 0x8009C478` on `client+84`), and `*name` is that effect's entry
 * in the 57-name table — never NULL.
 *
 * IT MUTATES, which is why it takes a writable inventory: the expiry test is
 * inside the draw and clears `last_item` in place (`sb zero, 84(a3)` at
 * 0x80035A4C). Call it once per drawn frame.
 *
 * ONE FRAME OF THE EXPIRY IS VISIBLE and is reproduced. The sub-draw reloads
 * `last_item` AFTER clearing it and carries on with index 0 — rect 0, the 1x1
 * blank, and the empty name — so the frame a caption dies still runs the draw
 * with nothing in it. Only the frame after that takes the early-out at
 * 0x80035A28 and returns without drawing at all.
 */
bool q2_item_pickup_caption(q2_inventory *inv, s32 level_time,
                            const q2_item_table *table,
                            u8 *icon, const char **name);

/* True while the powerup is held at `level_time`. */
bool q2_item_powerup_active(const q2_inventory *inv, q2_powerup_slot slot,
                            s32 level_time);

/*
 * Mega health's decay: while the flag is up and health is over the cap, one
 * point comes off every Q2_ITEM_MEGA_DECAY_TICKS. Called from the player's own
 * tick rather than an entity think, because the engine keeps it in the client.
 */
void q2_item_mega_health_tick(q2_inventory *inv, s32 level_time);

#endif /* Q2PSX_ITEM_H */
