/*
 * population.h — Population: where the actors and pickups go.
 *
 * The last piece of level data a game needs. Geometry says what the world looks
 * like, collision says where you can walk, triggers say what fires — this says
 * what is standing in the room when you arrive.
 *
 * ---------------------------------------------------------------------------
 * Layout
 * ---------------------------------------------------------------------------
 *     group[]                     24 bytes each, terminated by u32 0
 *     ... then the per-group lists, located by chunk-relative offset
 *
 * A group is:
 *     0x00  char[12]  name
 *     0x0C  u32       spawn_offset   chunk-relative; 0 means the group has none
 *     0x10  u32       place_offset   chunk-relative; 0 means none
 *     0x14  u32       flags     zero ON DISC; the runtime sets bit 0 when a
 *                                 script selects the group (0x80056C60) and
 *                                 tests bit 1 to stop it running twice
 *
 * 76 distinct group names exist game-wide: zone names, script batch names, item
 * categories, and one special path group. An empty Population chunk is 8 bytes.
 *
 * ---------------------------------------------------------------------------
 * Three list layouts, and three DIFFERENT terminators
 * ---------------------------------------------------------------------------
 * This is the part that punishes a careless reader. The lists do not share a
 * terminator convention:
 *
 *     spawn list  (most groups)   24 bytes/record, terminated by u32 0
 *     path list   (path group)    24 bytes/record, terminated by 0xFFFFFFFF
 *     place list  (all groups)    16 bytes/record, terminated by 0xFFFFFFFF
 *
 * Reading a place list expecting a zero terminator runs off the end; reading a
 * path list expecting one stops at the first node whose coordinates happen to
 * begin with a zero word.
 *
 * The spawn list also changes SHAPE for the path group — same 24-byte stride,
 * entirely different fields. Which layout applies is selected by the group's
 * name, not by anything in the record. That is why q2_pop_group_is_path exists.
 *
 * Spawn record (non-path groups), 24 bytes:
 *     0x00  u32  class_id   25 distinct values 0..37. NOT a ModelNames index:
 *                           15 of 673 exceed the map's name count and name
 *                           resolution yields nonsense. Meaning UNKNOWN.
 *     0x04  s32  x
 *     0x08  s32  y
 *     0x0C  s32  z
 *     0x10  u16  angle      0..3958; 0/1015/2041/3068 dominate (INFERRED)
 *     0x12  u16  link       copied verbatim as the signed entity.target
 *                           halfword by 0x8007E608/0x8007E614. 0xFFFF is
 *                           the observed no-link sentinel (482 of 673).
 *     0x14  u16  flags      map spawn flags. The entity filler at
 *                           0x8007E61C masks this to its low nine bits and
 *                           writes those into entity.spawnflags bits 18..26.
 *                           Individual meanings are creature-defined (the
 *                           Insane uses 0x08, 0x10, 0x40 and 0x80).
 *     0x16  u16  index      slot 0..323, not strictly monotonic, UNKNOWN
 *
 * Path record (path group only), 24 bytes. CONFIRMED field by field against
 * the dedicated path-corner spawner at 0x8007F390, whose only caller is
 * 0x800575A4, reached from 0x80057588 where the literal "PathCorner" at
 * 0x800ACD90 selects the group. The loop runs with s1 = record and
 * s0 = record + 0x0C:
 *     0x00  s32  x, y, z    xyz is at +0x00 here, NOT +0x04 as in a spawn.
 *                           0x8007F404-0x8007F418 copy it VERBATIM into the
 *                           entity: no eye lift, no nudge, no drop to floor.
 *     0x0C  u16  flags      low nine bits into entity.spawnflags 18..26, the
 *                           same transport a spawn record's flags use
 *                           (0x8007F430-0x8007F448, mask 0xF803FFFF)
 *     0x0E  u16  wait       low byte written to entity+0x1C byte 0 by
 *                           0x8007F3F8/0x8007F400; path_corner_touch
 *                           (0x8005F2F4) reads it back as the pause in ticks
 *                           the creature holds at this corner
 *     0x10  u16  targetname this corner's own id — `lhu v0,4(s0)` /
 *                           `sh v0,24(a2)` at 0x8007F3E0/0x8007F3E8
 *     0x14  u16  target     the next corner's id — `lhu v0,8(s0)` /
 *                           `sh v0,22(a2)` at 0x8007F3EC/0x8007F3F4
 *
 * These two were documented here as "link0/link1 neighbour index (INFERRED)".
 * They are not neighbour indices in a list: read as (targetname, target) the
 * 20 BASE1 records form closed cycles — 25->28->27->26->25 — and 8 of that
 * map's 9 creature links land on one. The creature entity filler at
 * 0x8007E5A0-0x8007E6DC never writes entity+0x18 at all, so on this disc a
 * targetname belongs to a path corner and to nothing else.
 *
 * Place record, 16 bytes:
 *     0x00  s32  x, y, z
 *     0x0C  u16  angle_flags   low 12 bits: heading on the 4096-step circle.
 *                           0x2000 / 0x4000 / 0x8000 are NOT_EASY /
 *                           NOT_MEDIUM / NOT_HARD, tested by the sole list
 *                           walker at 0x8007F538 before item spawn. Bit 12 is
 *                           never read: it is a map-wide authoring/export
 *                           marker, set on either every place or none in all
 *                           34 maps that carry places. The spawner at
 *                           0x80058930 masks the heading with 0x0FFF.
 *     0x0E  u16  id         0..56, 41 distinct. CONFIRMED as the key into the
 *                           24-byte item table at 0x8009F5CC: the spawner scans
 *                           it with `lb`, so only the low byte can ever match.
 *                           All 1,013 place records on the disc resolve, and
 *                           every one names a model its own map ships
 *                           (`q2psx-inspect items`). See src/build/itemtable.h.
 *
 * Region between a list's terminator and the next declared offset is slack, not
 * data: two maps leave a 4-byte gap after the group table, as do 18 spawn lists
 * and 34 place lists.
 */
#ifndef Q2PSX_POPULATION_H
#define Q2PSX_POPULATION_H

#include "level.h"
#include "q2psx.h"

#define Q2_POP_GROUP_SIZE  24
#define Q2_POP_SPAWN_SIZE  24
#define Q2_POP_PATH_SIZE   24
#define Q2_POP_PLACE_SIZE  16

#define Q2_POP_LINK_NONE   0xFFFFu
#define Q2_POP_TERM_FFFF   0xFFFFFFFFu

/*
 * The placement record's nine map-authored flag bits.  0x8007E61C clears
 * entity.spawnflags bits 18..26, then ORs these bits there.  Do not promote
 * this to an entity-class-specific enum: the same transport belongs to every
 * creature, while the individual bits belong to its own module.
 */
#define Q2_POP_SPAWN_FLAGS_MASK   0x01FFu
#define Q2_POP_SPAWN_FLAGS_SHIFT  18u

/*
 * AND THREE OF THE BITS THE TRANSPORT ABOVE DISCARDS ARE THE DIFFICULTY.
 *
 * SpawnGeneral (CREATURE.C 0x800565C4) is the one function that turns a spawn
 * record into a creature — the engine installs it at engine+0x3A0 (0x8007A020)
 * and at module+0x68 (0x8007DB10), and ProcessCreatureBatches calls it directly
 * for a CREBATCH (0x80056DF0). Before it allocates anything it asks LOADLEV.C
 * GetDifficultySetting (0x800565E0 -> 0x8007CCEC) and switches three ways on
 * the answer, reading `lhu 20(record)` — the record's +0x14 `flags` — each time:
 *
 *     800565E0  jal   GetDifficultySetting
 *     80056620  lhu   v0, 20(s1)    skill 0
 *     80056628  andi  v0, v0, 0x800
 *     8005663C  lhu   v0, 20(s1)    skill 1
 *     80056644  andi  v0, v0, 0x1000
 *     80056658  lhu   v0, 20(s1)    skill 2
 *     80056660  andi  v0, v0, 0x2000
 *
 * On a hit it sets v0 = 0 and jumps to the epilogue at 0x800567E0: no entity,
 * no model, no AI. Skill 3 and anything negative fall through the `slti`/`beq`
 * chain at 0x800565F0..0x8005661C unfiltered.
 *
 * CreatureCount (0x8005771C) applies the identical mask to the kill
 * denominator at 0x80057770..0x80057878, which is the confirmation: those
 * records are meant not to exist at that skill, not merely to be hidden.
 *
 * The masks are NOT the place record's. A place record puts NOT_EASY /
 * NOT_MEDIUM / NOT_HARD at 0x2000 / 0x4000 / 0x8000 of its +0x0C (below); a
 * spawn record puts them at 0x800 / 0x1000 / 0x2000 of its +0x14. The two
 * cannot share a constant and they are written out separately on purpose.
 */
#define Q2_POP_SPAWN_NOT_EASY     0x0800u
#define Q2_POP_SPAWN_NOT_MEDIUM   0x1000u
#define Q2_POP_SPAWN_NOT_HARD     0x2000u
#define Q2_POP_SPAWN_DIFFICULTY   0x3800u

/* Exact 0x800565E8..0x80056664 selection. Skills outside the retail menu's
 * 0..2 range take the fall-through arm and are not filtered. */
bool q2_pop_spawn_allows_skill(u16 flags, s32 skill);

typedef struct q2_pop_group {
    char name[13];
    u32  spawn_offset;   /* 0 when absent */
    u32  place_offset;
} q2_pop_group;

typedef struct q2_pop_spawn {
    u32 class_id;
    s32 x, y, z;
    u16 angle;
    u16 link;        /* verbatim source for q2_monster.target (signed) */
    u16 flags;       /* low 9 -> q2_monster.spawnflags bits 18..26 */
    u16 index;
} q2_pop_spawn;

typedef struct q2_pop_path {
    s32 x, y, z;
    u16 flags;       /* low 9 -> entity.spawnflags bits 18..26 */
    u16 wait;        /* low byte -> entity+0x1C byte 0, the corner's pause */
    u16 targetname;  /* this corner's id                                   */
    u16 target;      /* the next corner's id                               */
} q2_pop_path;

typedef struct q2_pop_place {
    s32 x, y, z;
    u16 angle_flags;
    u16 id;     /* keys the item table; see itemtable.h */
} q2_pop_place;

/* The heading the entity spawner takes from a place record (0x80058938). */
#define Q2_POP_PLACE_ANGLE(v)       ((v) & 0x0FFFu)
#define Q2_POP_PLACE_FLAGS(v)       ((v) & 0xF000u)
#define Q2_POP_PLACE_UNUSED_1000    0x1000u
#define Q2_POP_PLACE_NOT_EASY       0x2000u
#define Q2_POP_PLACE_NOT_MEDIUM     0x4000u
#define Q2_POP_PLACE_NOT_HARD       0x8000u
#define Q2_POP_PLACE_DIFFICULTY     0xE000u

/* Exact 0x8007F54C..0x8007F5D8 selection. Skills outside the retail menu's
 * 0..2 range take the default arm and are not filtered. */
bool q2_pop_place_allows_skill(u16 angle_flags, s32 skill);

typedef struct q2_population {
    const u8 *data;
    u32       size;
    u32       group_count;
} q2_population;

q2_result q2_population_parse(q2_population *out, const q2_common_file *common);

bool q2_pop_get_group(const q2_population *p, u32 index, q2_pop_group *out);

/* True when this group's spawn list uses the path-node layout. Selected by
 * name, because nothing in the records distinguishes them. */
bool q2_pop_group_is_path(const q2_pop_group *g);

/*
 * The zone a group's NAME claims, or -1 when it names none.
 *
 * ---------------------------------------------------------------------------
 * Why a name is the only thing there is to go on
 * ---------------------------------------------------------------------------
 * A group is not spawned because it exists. It is spawned because a script
 * SELECTED it: `0x80056C60` takes a twelve-byte name, finds the group, and sets
 * bit 0 of its flags word; the spawn pass at `0x80056D68` then walks the table
 * and runs only the groups with bit 0 set and bit 1 clear, setting bit 1 so one
 * cannot run twice. Both are exported to the relocatable level modules through
 * the import block at `0x800B2FE4` — slot 9 selects, slot 11 spawns one place
 * record (FORMATS §15.5). And the flags word is ZERO ON DISC for all 222 groups
 * of all 49 maps, so at level load nothing is selected and nothing spawns.
 *
 * Which groups a level selects therefore lives in its `LevelBin` module, which
 * this port does not run yet. So a caller cannot know the real answer — but it
 * can read the part the disc does state plainly, which is that 74 of the 222
 * groups are named after a zone: every map that carries `Zone<N>` groups carries
 * exactly one per zone it ships, `Zone0`…`Zone4` against ZONE0…ZONE4.DAT.
 *
 * A leading `Zone<digits>` is therefore taken as naming that zone, suffix and
 * all — `Zone1bob`, `Zone1Key`, `Zone2_1` are batches OF that zone. Everything
 * else (`Weapons`, `Ammo`, `ShotgunRoom`, `BerserkHide`, `PathCorner`) claims no
 * zone and gets -1: the deathmatch maps partition their Population by item
 * CATEGORY rather than by zone and have no `Zone<N>` group at all.
 *
 * Matched case-insensitively, as q2_pop_group_is_path is.
 */
int q2_pop_group_zone(const q2_pop_group *g);

/* True only for an exact Zone<digits> name. A suffix still identifies the
 * owning zone, but denotes a separate script batch, not startup population. */
bool q2_pop_group_is_zone(const q2_pop_group *g);

/*
 * Walk a group's lists. `slot` counts from 0; each returns false at the list's
 * terminator, at the end of the chunk, or when the group has no such list.
 *
 * Each honours its own terminator — they genuinely differ, see the header.
 */
bool q2_pop_get_spawn(const q2_population *p, const q2_pop_group *g,
                      u32 slot, q2_pop_spawn *out);
bool q2_pop_get_path(const q2_population *p, const q2_pop_group *g,
                     u32 slot, q2_pop_path *out);
bool q2_pop_get_place(const q2_population *p, const q2_pop_group *g,
                      u32 slot, q2_pop_place *out);

#endif /* Q2PSX_POPULATION_H */
