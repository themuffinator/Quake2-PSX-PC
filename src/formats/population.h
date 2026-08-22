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
 * Path record (path group only), 24 bytes:
 *     0x00  s32  x, y, z    xyz is at +0x00 here, NOT +0x04 as in a spawn
 *     0x0C  u16  unk0
 *     0x0E  u16  zero
 *     0x10  u32  link0      neighbour index; 0xFFFFFFFF means none (INFERRED)
 *     0x14  u32  link1
 *
 * Place record, 16 bytes:
 *     0x00  s32  x, y, z
 *     0x0C  u16  angle_flags   the LOW TWELVE BITS are a heading on the
 *                           4096-step circle and bits 12..15 are something
 *                           else. CONFIRMED from the reader: the entity spawner
 *                           at 0x80058930 does `lhu`, `andi 0xFFF`, `sh` into
 *                           the entity's yaw at +0xE8. An earlier note read the
 *                           whole halfword as one value and concluded "flags
 *                           OR'd with an angle" from the observed 4096 / 32768 /
 *                           36864 / 53248 — those are all heading 0 with upper
 *                           bits set. 627 of 1,013 records set at least one
 *                           upper bit and all four are used; what they mean is
 *                           still UNKNOWN.
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
    u16 unk0;
    u32 link0, link1;
} q2_pop_path;

typedef struct q2_pop_place {
    s32 x, y, z;
    u16 unk;    /* angle in bits 0..11, unknown flags in 12..15 */
    u16 id;     /* keys the item table; see itemtable.h        */
} q2_pop_place;

/* The heading the entity spawner takes from a place record (0x80058938). */
#define Q2_POP_PLACE_ANGLE(unk)  ((unk) & 0x0FFFu)
#define Q2_POP_PLACE_FLAGS(unk)  ((unk) & 0xF000u)

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
