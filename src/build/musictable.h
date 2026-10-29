/*
 * musictable.h — which stream a music id names, and answer to open question #13.
 *
 * The question was "which in-game situation selects which music id", and it was
 * stuck because the id lives in a `$gp` global (`0x800B2E5C`) whose writer had
 * not been located. Both halves are now read, and they are two separate tables.
 *
 * ---------------------------------------------------------------------------
 * The id names a stream — 0x800A1DD8, 22 records of six bytes
 * ---------------------------------------------------------------------------
 * The player at `0x80071718` indexes it by the id with the multiply spelled out
 * at 0x80071760 (`id*2 + id`, doubled — a stride of six), then:
 *
 *     +0  s8   file, 0..4, indexing the five `QUAKE_x.XAI` names at 0x800A1E5C;
 *              NEGATIVE selects CD-DA through the native TOC, as ids 0 and 1 do
 *              (0x80071778's `bltz` takes the other arm entirely)
 *     +1  u8   channel, 0..3            (0x800717E8)
 *     +2  u16  zero on every record
 *     +4  u16  duration in TENTHS OF A SECOND
 *
 * The duration's unit is not asserted, it is checked: `q2psx-inspect music`
 * demultiplexes every stream on the disc and measures it. Nineteen agree
 * within the rounding of one sector; QUAKE_C channel 3's table duration is
 * one second shorter than its audio. Native playback advances at the authored
 * timer, not at decoded EOF.
 *
 * Ids 2..21 are therefore exactly the twenty (file, channel) pairs the disc
 * carries, in file-major order. Ids 0 and 1 have zero duration and are absent
 * from every named PAL/US level playlist. The port leaves those reserved ids
 * silent; it does not implement their native CD-DA command path.
 *
 * ---------------------------------------------------------------------------
 * What SELECTS an id — the level's own playlist
 * ---------------------------------------------------------------------------
 * `0x80071A68` is a cursor walk, and the list it walks is in the LEVEL TABLE:
 *
 *     p = cursor;  cursor = p + 1;  v = *cursor
 *     v == 0   ->  the list is over
 *     v <  0   ->  cursor += v          (a relative jump BACK)
 *     id = *cursor
 *
 * Thirty-seven named records have seven ids followed by `0xF9` (minus seven),
 * which returns the cursor to +0x22. QFRONT loops one id using `0xFF`; MAGDEMO
 * loops four using `0xFC`. Twelve records start with zero and stop immediately.
 * Native execution covers all 51 named records on both supported revisions.
 *
 * The seven are a lead track and then six consecutive ones from lead + 3,
 * wrapped into 2..17: BASE0 is 14 then 17, 2, 3, 4, 5, 6; BASE1 is 8 then
 * 11..16; BASE2 is 19 then 6..11; JAIL2 is 2 then 5..10. Ids 18..21 —
 * QUAKE_E's four channels — are used as opening tracks, including BASE3,
 * BASE2, WASTE2 and WASTE4 respectively.
 */
#ifndef Q2PSX_MUSICTABLE_H
#define Q2PSX_MUSICTABLE_H

#include "disc.h"
#include "exe.h"
#include "ident.h"
#include "q2psx.h"

#define Q2_MUSIC_RECORD_SIZE 6
#define Q2_MUSIC_COUNT      22   /* 0/1 reserved CD-DA; 2..21 XA streams      */
#define Q2_MUSIC_FILES       5   /* QUAKE_A .. QUAKE_E                         */

/* PAL build. Per-build like every other table here, and refusing an
 * uncatalogued executable is the point. */
#define Q2_MUSICTABLE_ADDR_SLES01534  0x800A1DD8u

/* The five stream file names, in the order the id's `file` field indexes. */
extern const char *const q2_music_files[Q2_MUSIC_FILES];

typedef struct q2_music_entry {
    s8  file;        /* < 0: native CD-DA branch         */
    u8  channel;
    u16 tenths;      /* duration, in tenths of a second  */
} q2_music_entry;

typedef struct q2_music_table {
    q2_music_entry entry[Q2_MUSIC_COUNT];
    u32            count;
} q2_music_table;

/*
 * Read the table out of the disc's boot executable. Fails with
 * Q2_ERR_UNSUPPORTED on a build whose address is not catalogued rather than
 * reading a plausible-looking wrong one.
 */
q2_result q2_music_table_load(q2_music_table *out, const disc *d,
                              const q2_build_id *id);

/* The entry for an id, or NULL when the id is out of range. */
const q2_music_entry *q2_music_get(const q2_music_table *t, int id);

/* XA file stem for an id, or NULL for a reserved CD-DA or invalid entry.
 * The caller forms `Q2DATA/<NAME>.XAI`. */
const char *q2_music_file_name(const q2_music_table *t, int id);

#endif /* Q2PSX_MUSICTABLE_H */
