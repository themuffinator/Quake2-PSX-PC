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
#include "trace.h"      /* q2_move_world — the entity list a dropped item hits */

struct q2_fx_world;
struct q2_model_bank;

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
    u32 no_placement;   /* no valid SecondaryCol cell for the spawn sweep */
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
 * Returns NULL for an unknown id, allocation failure, or a placement outside
 * the supplied resident hull. NULL hull supports asset-only callers.
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
 * its calls to the retail group selector, so an unzoned or suffixed group such
 * as `Weapons` or `Zone1Key` needs an explicit selection. This is the same
 * rule `q2_creature_world_hold_batches` applies to creature groups. A resident
 * `Zone<N>` group remains the fallback when the module is absent or empty;
 * script batches do not appear early merely because their names start with a
 * zone. Conditional mission selections use q2_sim_attach_item_batches instead
 * of treating this static call scan as executed control flow.
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
/* The engine state an item think reaches that q2_entity_world does not carry */
/* ------------------------------------------------------------------------- */
/*
 * Two item thinks reach console GLOBALS rather than anything on the entity or
 * its world: the materialise burst spawns into the one particle pool
 * (0x800596B0 -> 0x80030284, whose pool is a fixed block at 0x800D4B24 and a
 * group list the whole engine shares), and a creature's dropped item moves
 * through the shared toss mover, which installs SecondaryCol (0x800C8FE8) and
 * walks the 48-slot entity table (0x800CAE10) and the gravity word at
 * [0x800AE924]. None of those is per-entity on the console, so the binding is
 * module-wide here too, and the host installs it once per zone load.
 *
 * Every field may be NULL. With no `fx` the materialise burst still draws its
 * 45 numbers (so the generator stream is the same with or without a pool) and
 * spawns nothing. With no `hull` a dropped item is traced against the entity
 * list alone; with no `ents` against the hull alone; with neither it flies
 * until something else stops it. With no `gravity` it falls at Q2_GRAVITY.
 */
typedef struct q2_item_env {
    struct q2_fx_world         *fx;       /* the pool 0x80030284 spawns into */
    q2_collision               *hull;     /* SecondaryCol: 0x80046DE8        */
    const q2_move_world        *ents;     /* what 0x80053974 clips against   */
    const s32                  *gravity;  /* [0x800AE924], read live         */
} q2_item_env;

/* Copy `env` into the module; NULL clears every field. */
void q2_item_bind_env(const q2_item_env *env);

/* ------------------------------------------------------------------------- */
/* A creature's death drop — the spawner 0x8002085C and its think 0x80020C48  */
/* ------------------------------------------------------------------------- */
/*
 * The last two links of the chain monster.h documents over
 * q2_monster_drop_item_for_class: the four-slot queue hands each resolved drop
 * to 0x8002085C, which has exactly one caller (0x80020840). It is NOT the
 * placed-item spawner and it shares almost nothing with it:
 *
 *   - no placement routine, so NO 286+30 LIFT and NO FLOOR SWEEP — the
 *     recorded origin goes straight into +0x54 (0x800209C4) and +0xA4
 *     (0x80020990), because it is already a live entity's origin;
 *   - the draw origin on the SPAWN FRAME is that position raised by 286 AND by
 *     the model bias (0x800209A4 `addiu -286`, 0x800209AC `subu ext2`), where
 *     every later frame lowers it by 286 instead (0x80020CE4 `addiu +286`).
 *     One frame, 572 units apart; both are the console's;
 *   - angles are ZEROED (0x80020AC8, six bytes) and the twelve random bits the
 *     queue drew become the heading of a TOSS: `{sin, cos}` at 0x800A5430
 *     indexed `heading & 0xFFF` (0x80020A2C), speed from one rand() and an
 *     upward kick from a second;
 *   - the flags word is the record's with Q2_ITEM_TIMED ORed in (0x80020B38),
 *     except for item 21, which keeps the record's word (0x80020B1C);
 *   - its OWN box, (-256, ext2-512, -256)..(256, ext2, 256) about the position
 *     (0x80020B4C..0x80020C1C), set once. `access 0x78` finds word stores to
 *     +0x78 in five functions — this spawner, the player spawn 0x8003D4FC,
 *     the model-entity spawner 0x8005A778, 0x80079818 and 0x8007D990 — and
 *     neither the toss mover 0x800463E8 nor the item think 0x80059330 is one
 *     of them. So the touch box stays where the creature died while the item
 *     flies and after it lands; the console's, reproduced;
 *   - render flags +0x10C are never written, and the allocator 0x8006C098
 *     clears them, so a drop carries NONE: no floor shadow, and the mover's
 *     tail skips the shadow link at 0x80046B4C. The placed spawner's
 *     0x08000001 (0x800587C0) is not this spawner's.
 *
 * `rand15` stands for BIOS rand() (0..32767) and is called EXACTLY TWICE, and
 * only once the item record, the entity and the model have all been found —
 * 0x80020A00 and 0x80020A58 are both past the three early-outs at 0x80020894,
 * 0x800208A8 and 0x80020970. So a drop that fails spends no random numbers.
 * The creature layer's rand() is the host's C library one (monster.c draws the
 * heading from it); pass the same, masked to fifteen bits.
 *
 * `banks` is where 0x8006D008 looks the model name up. A drop whose model no
 * bank carries is freed and nothing spawns (0x80020968 `jal 0x8006D280`).
 * `bank_count` 0 means the caller has no CastList at all — a test or a census —
 * and skips the lookup, leaving the bias at zero, as q2_item_spawn does for a
 * caller without a bank.
 *
 * `cell` is the creature's own SecondaryCol cell, the spawner's fifth argument
 * (`lh 78(a3)` at 0x80020838 -> +0xA2). The port keeps none on a creature, so
 * the host finds it for `pos`; -1 is "not found". The surface byte +0x9E is
 * that cell's byte +32 in the bound hull (0x80020AF8..0x80020B14).
 *
 * Returns the entity, or NULL when nothing spawned — no record, no memory, no
 * model, or the port's in-flight table full (see Q2_ITEM_TOSS_MAX).
 */
#define Q2_ITEM_DROP_KEEPS_FLAGS     21    /* 0x80020B18: `addiu v0, zero, 21`  */
#define Q2_ITEM_DROP_TOSS_LIFE       96    /* 0x80020ADC: +0xF4 while it flies  */
#define Q2_ITEM_DROP_LANDED_LIFE   8700    /* 0x80020CFC: +0xF4 once it rests   */
#define Q2_ITEM_DROP_FIELD90        128    /* 0x80020BB4: +0x90                 */
#define Q2_ITEM_DROP_BOX_HALF       256    /* 0x80020B4C / 0x80020B64           */
#define Q2_ITEM_DROP_BOX_DEPTH      512    /* 0x80020B68: mins.y = ext2 - 512   */
#define Q2_ITEM_DROP_SPEED_BASE     256    /* 0x80020A3C: 256 + (r*768 >> 15)   */
#define Q2_ITEM_DROP_SPEED_SPAN     768    /* 0x80020A08..0x80020A10: r*3 << 8  */
#define Q2_ITEM_DROP_KICK_BASE    (-3072)  /* 0x80020A80                        */
#define Q2_ITEM_DROP_KICK_SPAN     1536    /* 0x80020A64..0x80020A6C: r*3 << 9  */

/*
 * 0x80020CA0 `ori v1, v1, 0x34FF` on `lui 0xC`: a contact lands the item only
 * when vx^2 + vy^2 + vz^2 is at most this (`slt` at 0x80020CB0, so equal lands).
 * The toss mover is called with a rebound of ZERO (0x80020C58, a2 = 0), which
 * zeroes the velocity on any contact at all (0x800466FC `mult v0, s6`), so in
 * practice the first contact always lands it — floor or wall. Kept as the test
 * it is rather than as the shortcut it amounts to.
 */
#define Q2_ITEM_DROP_REST_SPEED2  0xC34FF

/*
 * The shared toss mover's own figures (0x800463E8), for the flight. The step
 * is `vel * dt / 320` per axis, truncated, stored as a halfword — the compiler's
 * divide by 320 is the `0x66666667` / `sra 7` pair at 0x800464EC..0x80046514;
 * gravity is `[0x800AE924] * dt` added to vel.y as a HALFWORD (`lhu`/`sh`) and
 * capped at 8192 (`slti 8193`, 0x80046490) unless +0x10C carries 0x2000.
 */
#define Q2_ITEM_TOSS_STEP_DIV       320
#define Q2_ITEM_TOSS_TERMINAL      8192
#define Q2_ITEM_TOSS_NO_GRAVITY  0x2000u

/*
 * THE PORT'S ONE LIMIT HERE. The velocity lives at +0xE0..+0xE4 on the
 * console's entity and q2_entity has no field for it, so an in-flight drop's
 * velocity is kept in a side table keyed by (set, slot). A slot is stable
 * across the set's growth (entity.c reuses and never moves indices), where a
 * pointer would not be. Sixty-four is far more than the four drops a frame the
 * queue can produce times the second or so a drop is in the air; a spawn that
 * finds it full is refused rather than launched with no velocity.
 */
#define Q2_ITEM_TOSS_MAX 64

q2_entity *q2_item_drop_spawn(q2_entity_set *set, const q2_item_table *table,
                              const struct q2_model_bank *const *banks,
                              u32 bank_count, u8 item_id, u16 heading,
                              const s32 pos[3], s32 cell, int (*rand15)(void));

/*
 * 0x80020C48 — one tick of a dropped item's flight: the toss mover
 * (0x80046DDC, whose only caller this is), then either the landing — +0x50 = 1,
 * Q2_ITEM_TIMED unless Q2_ITEM_OBJECTIVE, an 8700-tick life, and the ordinary
 * item think 0x80059330 from the next tick — or the draw origin re-derived from
 * the new position. Installed by q2_item_drop_spawn.
 */
void q2_item_drop_think(q2_entity *e, q2_entity_world *w);

/* The velocity an in-flight drop carries (+0xE0..+0xE4); false when `e` is not
 * one. For the test and the census. */
bool q2_item_drop_velocity(const q2_entity *e, s16 out[3]);
bool q2_item_drop_restore(const q2_entity_set *set, u32 slot, const s16 vel[3]);

/* How many drops are in the air. */
u32 q2_item_drop_in_flight(void);

/* Forget every in-flight drop. The host calls it whenever it frees the set its
 * drops live in, so no record outlives its entity. */
void q2_item_drop_reset(void);

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
