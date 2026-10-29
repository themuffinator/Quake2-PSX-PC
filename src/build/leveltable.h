/*
 * leveltable.h — the executable's level table, and cross-map progression.
 *
 * Zone gates move the player between zones of one map. Moving between MAPS
 * needs this table, which is the only place a display name is tied to a
 * directory on the disc.
 *
 * ---------------------------------------------------------------------------
 * Layout
 * ---------------------------------------------------------------------------
 * 52 records (51 named entries and a sentinel) of 56 bytes, at file offset
 * 0x84EC8 in the PAL executable:
 *
 *     +0x00  char[12]  display name    e.g. "Base0", "COLD STORAGE"
 *     +0x0C  char[10]  directory name  e.g. "BASE0" — the Q2DATA/LEVELS folder
 *     +0x16  s16       mission id: 0 none, 1..5 campaign, 6 Tribute
 *     +0x18  u32       runtime pointer, initially 0
 *     +0x1C  s32       always 0x80050008 — a pointer, identical in every record
 *     +0x20  u8        0 on screens, 1 on campaign maps, 2 or 4 on arenas
 *     +0x21  u8        padding, 0
 *     +0x22  u8[7]     the music playlist — see below
 *     +0x29  s8        -7: the playlist's loop-back
 *     +0x2A  u8[2]     the terminating zero, never reached
 *     +0x2C  s32[3]    always 0
 *
 * The stride was established empirically rather than assumed. Scanning for
 * mixed-case names with an uppercase twin nearby gives 31 hits, and the gaps
 * between them are 56 in 24 of 30 cases — the outliers being the records whose
 * display name is not a simple capitalisation of the directory.
 *
 * ---------------------------------------------------------------------------
 * The bytes at +0x22, which ARE the music playlist
 * ---------------------------------------------------------------------------
 * This file used to describe +0x23..+0x27 as a five-byte run of unknown
 * meaning, +0x22 as unrelated, and +0x28 as `0xF900 | n`, and it declined to
 * call any of it music because that was only suggestive. The player's own
 * cursor walk at `0x80071A68` settles it, and the earlier split was a misread:
 *
 *     p = cursor;  cursor = p + 1;  v = *cursor
 *     v == 0   ->  the list is over
 *     v <  0   ->  cursor += v          (a relative jump BACK)
 *     id = *cursor;  the id names a stream through musictable.h
 *
 * So +0x22..+0x28 are **seven track ids**, +0x29 is `0xF9` — minus seven, which
 * lands the cursor back on +0x22 — and +0x2A is the terminating zero that is
 * therefore never reached. Every level loops a seven-track playlist. What
 * looked like a word `0xF900 | n` was the seventh id, the loop byte and two
 * zeros read together.
 *
 * The seven are a lead track and then six consecutive ones from lead + 3,
 * wrapped into 2..17: BASE0 is 14 then 17, 2, 3, 4, 5, 6; BASE1 is 8 then
 * 11..16; BASE2 is 19 then 6..11; JAIL2 is 2 then 5..10.
 *
 * ---------------------------------------------------------------------------
 * The table terminator
 * ---------------------------------------------------------------------------
 * The final record has an empty display name and "BADLEVEL" directory. Native
 * lookup stops on that empty first word. The following bytes are neighboring
 * data ("Base3" / "Jail2"), not a 53rd level record.
 */
#ifndef Q2PSX_LEVELTABLE_H
#define Q2PSX_LEVELTABLE_H

#include "ident.h"
#include "q2psx.h"

#define Q2_LEVEL_RECORD_SIZE 56
#define Q2_LEVEL_NAME_LEN    12
#define Q2_LEVEL_DIRECTORY_LEN 10

/*
 * The raw byte list the music cursor walks, +0x22 through +0x2B. It is not a
 * fixed-length run: a POSITIVE byte is a track id, a NEGATIVE one is a relative
 * jump back, and a zero ends the list. Most levels are seven ids then -7, but
 * QFRONT is one id then -1 (the title screen's single looping track) and
 * MAGDEMO is four then -4.
 */
#define Q2_LEVEL_PLAYLIST    10

/* Where the table sits in the catalogued PAL build. A different build moves it,
 * which is exactly why identification is per-build — see ident.h. */
#define Q2_LEVELTABLE_OFFSET_SLES01534 0x84EC8
#define Q2_LEVELTABLE_COUNT_SLES01534  52

typedef struct q2_level_entry {
    char display[Q2_LEVEL_NAME_LEN + 1];   /* shown to the player      */
    char directory[Q2_LEVEL_NAME_LEN + 1]; /* Q2DATA/LEVELS/<this>     */

    /* The music playlist's raw bytes; walk them with
     * q2_level_playlist_next rather than reading them as ids. */
    u8   playlist[Q2_LEVEL_PLAYLIST];

    bool is_placeholder;  /* BADLEVEL or the terminator                */
    s16 mission_unit;     /* +0x16: native direct-level mission selector. */
    u8 game_kind;         /* +0x20: nonzero selects single-player pools. */
} q2_level_entry;

typedef struct q2_level_table {
    q2_buf         exe;      /* owns the executable image */
    q2_level_entry *entries;
    u32             count;
} q2_level_table;

/*
 * Read the level table out of the disc's boot executable.
 *
 * Fails with Q2_ERR_UNSUPPORTED on a build whose table location is not known,
 * rather than reading a plausible-looking wrong address.
 */
q2_result q2_level_table_load(q2_level_table *out, const disc *d,
                              const q2_build_id *id);
void      q2_level_table_free(q2_level_table *t);

/* Find a level by its disc directory name, case-insensitively. */
const q2_level_entry *q2_level_find(const q2_level_table *t, const char *directory);

/*
 * The engine's own playlist walk, 0x80071A68.
 *
 * Start with `*cursor = -1`; each call advances and returns the next track id,
 * or -1 when the list ends. A negative byte moves the cursor back by its own
 * value, so a list can loop forever — which every real level's does, and which
 * is why a caller must not treat this as an enumeration to run to completion.
 */
int q2_level_playlist_next(const q2_level_entry *e, int *cursor);

/* Find by the display name instead, which is what a script's LOADMAP names. */
const q2_level_entry *q2_level_find_display(const q2_level_table *t,
                                            const char *display);

#endif /* Q2PSX_LEVELTABLE_H */
