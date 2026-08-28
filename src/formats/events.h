/*
 * events.h — the Events chunk: compiled trigger scripts.
 *
 * Every map carries one in COMMON.DAT and one per zone. This is the chunk that
 * separates a static geometry viewer from a game: triggers, zone gates, level
 * progression, teleports, doors and lifts are all expressed here.
 *
 * ---------------------------------------------------------------------------
 * Container
 * ---------------------------------------------------------------------------
 *     u32   count                       number of records
 *     { char name[12]; u32 offset }[K]  named entry points; K is implicit
 *     u32   0                           directory terminator
 *     record[count]                     variable length, packed back-to-back
 *
 * K is implicit because the directory ends at the terminator. Walking `count`
 * records from just past it lands exactly on chunk end in 164/164 files, and
 * all 145 COMMON directory offsets and all 886 TrigBounds event links coincide
 * with a walked record boundary. Stub maps carry an 8-byte chunk — count 0,
 * empty directory — and 36 chunks on the disc are exactly that.
 *
 * ---------------------------------------------------------------------------
 * A record is a nested TLV item list
 * ---------------------------------------------------------------------------
 *     u16  size      total record bytes INCLUDING this header. 4..240, %4 == 0
 *     u8   n_items   number of items that follow
 *     u8   flags     mutable; the runtime writes it back
 *     item[n_items]
 *
 *     u8   op        opcode | ONESHOT(0x40) | DISABLED(0x80)
 *     u8   len       total item bytes INCLUDING these two. Always %4 == 0
 *     u8   payload[len - 2]
 *
 * CORRECTION — an earlier pass read bytes +2 and +3 as a "sub-opcode" and a
 * "class byte". Both are refuted, and not merely by a better statistical fit:
 * each was re-derived from three independent instruction sites.
 *
 * Byte +2 is the interpreter's LOOP BOUND — lbu then slt against the item
 * counter, at 0x800279F8 and again in the queue drain at 0x80026E88.
 *
 * Byte +3 is WRITTEN BACK. The run latch at 0x800279A8..0x800279BC computes
 * v = (v & 0x7F) | ((v << 1) & 0x80) | 1, i.e. "mark as run, and copy the
 * one-shot bit into the disabled bit". Opcode 0x14 clears bit 7 (andi 0x7F then
 * sb, at 0x800278B0) and opcode 0x15 sets it (ori 0x80, at 0x80027920). The
 * loader defaults it at 0x80026E68. It looks like a multiple of eight on disc
 * only because the low three bits are runtime state that starts clear.
 *
 * The framing is CONFIRMED on 4,179 records / 6,646 items across all 164
 * containers: the walk p += item[p+1] lands exactly on the record's end AND
 * yields exactly byte +2 items, with zero exceptions. 1,643 records carry two
 * or more items, so the identity is not vacuous.
 *
 * On-disc flags values are exactly {0x08,0x10,0x18,0x20,0x28,0x48,0x50,0x58}.
 * Note 0x60 and 0x68 never occur, so it is NOT "each of the first five,
 * optionally | 0x40".
 *
 * ---------------------------------------------------------------------------
 * *** THE CHUNK IS SELF-MODIFYING ***
 * ---------------------------------------------------------------------------
 * This is the single most load-bearing fact here, and a parser that misses it
 * will hand a renderer indices that overrun a 48-entry array by about 26 KB.
 *
 * There are TWO passes over the chunk, not one.
 *
 * At load time, 0x80026DC0 walks every record and dispatches through its OWN
 * 20-entry table at 0x800ABCF8, indexed by opcode - 3 (bounded at 0x80026EA8).
 * It allocates runtime objects and REWRITES the s16 slots inside the items in
 * place, reading the original values from a SECOND Events pointer at gp+0x178,
 * distinct from the runtime base at gp+0x174.
 *
 * At execution time, 0x80027950 dispatches through a DIFFERENT 21-entry table
 * at 0x800ABD48, indexed by opcode - 2, and the handlers read those rewritten
 * slots as runtime object indices in 0..47.
 *
 * So a given s16 slot holds a zone-local Scene NODE index on disc, and a
 * runtime OBJECT index after load. They are different things occupying the same
 * bytes at different times. The disc values reach 332; the runtime object array
 * at 0x800D6BB0 has exactly 48 entries of 92 bytes, proven three ways — the
 * tick loop's end at base+4416 with stride 92, a 48-iteration reset loop
 * starting at base+92*47, and the allocation guard slti count,48 at 0x80025E48.
 *
 * This module therefore exposes the DISC view only, and names the fields for
 * what they are on disc. The runtime object model is the port's business, and
 * the load-time pre-pass is behaviour to reimplement, not data to parse.
 *
 * ---------------------------------------------------------------------------
 * Object references are zone-local Scene node indices — CONFIRMED
 * ---------------------------------------------------------------------------
 * Proven from code: the loader and the 0x08 handler both scale these s16 by 52
 * and index the Scene pointer at 0x800B2C3C, then read the record's bbox at
 * +16 and +28 — which is exactly the Scene layout.
 *
 * And from data: comparing Scene bounding-box bytes between the zone-0 copy and
 * every other zone copy of the same item slot gives 166/166 identical, against
 * a control of 74/13,200 (0.56%) for randomly paired nodes in the same two
 * zones. Scene origins genuinely do differ per zone, which is the per-zone
 * rebasing.
 *
 * ---------------------------------------------------------------------------
 * Opcode 0x08 is NOT a mover — it is the `func_explosive`
 * ---------------------------------------------------------------------------
 * An earlier pass grouped it with 0x03/0x04/0x05. Its handler at 0x800267C4
 * contains no x300 scaling, no reference to the runtime object array, and no
 * write to +0x4C or +0x4E anywhere in the function. It iterates two 4-element
 * arrays of Scene indices, computes bbox centres, spawns an effect, and clears
 * a Scene flag. Byte +24 is a parameter, not a speed, and byte +25 is SIGNED.
 * The zone diff corroborates: across the 53 differing zone copies, 0x08 differs
 * at +6..+21 and +26 and never at +24 or +25.
 *
 * SOLVED, and the whole of it now lives in `src/game/explosive.h`. It is a
 * DESTROYABLE BRUSH GROUP: the first array is the intact geometry, given a
 * damageable box each and hidden when the group's hit points run out, and the
 * second is the wreckage, hidden at load and shown in its place. `+25`'s sign
 * selects a detonation — `0x8005A778`, the same spawner the creature import
 * table calls `spawn_explosion` — and its one's complement is the debris count.
 *
 * The old name `FXGROUP` survives below because 224 items on the disc and every
 * report this port has ever printed use it, but it is a misnomer: the effects
 * are the smaller half of what the opcode does.
 *
 * WHY IT LOOKED LIKE A MOVER, which is worth writing down because the
 * resemblance is not a coincidence. Bytes +2..+5 are read by NEITHER handler —
 * there is no load at those offsets anywhere in 0x800267C4 or 0x80026A20 — yet
 * they are non-zero on all 288 items on the disc and take only seven distinct
 * values, every one of which decodes as two small s16. That is exactly
 * MOVER_A's `travel` at +2 and `speed` at +4. The opcode shares the movers'
 * header shape and ignores the two fields, which is precisely the sort of thing
 * that makes a census group it with them. `q2psx-inspect explosives` prints the
 * seven values.
 *
 * ---------------------------------------------------------------------------
 * What is still open
 * ---------------------------------------------------------------------------
 * The per-frame motion integrators (the second function pointer in the
 * UserFuncs binding table; 27 of 43 primitives have one, none decompiled), the
 * interior of the 92-byte runtime object, the STRING and MISEVENT string
 * namespaces, and the three record category bits 0x08/0x10/0x20.
 *
 * Consequence for the port: the trigger, arm and disarm graph, zone gates,
 * level progression and teleports are all usable now.
 *
 * STALE TEXT REMOVED. This paragraph used to end "DOORS AND LIFTS ARE NOT,
 * because the link from a script slot to a moving object is manufactured at
 * load time by code that has not been decoded." Both halves stopped being true:
 * mover.h decodes the link and declines to reproduce the rewrite on purpose,
 * and explosive.h does the same for opcode 0x08's constructor. What remains
 * undecoded is the list above it, not the movers.
 */
#ifndef Q2PSX_EVENTS_H
#define Q2PSX_EVENTS_H

#include "level.h"
#include "q2psx.h"

#define Q2_EVENT_DIR_ENTRY_SIZE  16
#define Q2_EVENT_REC_HEADER_SIZE  4
#define Q2_EVENT_ITEM_HEADER_SIZE 2

/* The runtime object array's capacity, for a port reimplementing the load-time
 * pre-pass. Disc slot values are NOT bounded by this — see the header comment. */
#define Q2_EVENT_OBJECT_MAX   48
#define Q2_EVENT_OBJECT_SIZE  92

/* q2_event_record.flags. Bits 0-2 are runtime state and are clear on disc. */
enum {
    Q2_EVREC_HASRUN   = 0x01,
    Q2_EVREC_RT1      = 0x02,
    Q2_EVREC_RT2      = 0x04,
    Q2_EVREC_CAT_A    = 0x08,   /* run on the first frame inside a trigger  */
    Q2_EVREC_CAT_B    = 0x10,   /* run on every frame inside                */
    Q2_EVREC_CAT_C    = 0x20,   /* run on the first frame after leaving     */
    Q2_EVREC_ONESHOT  = 0x40,   /* on run, DISABLED := ONESHOT              */
    Q2_EVREC_DISABLED = 0x80    /* record is skipped                        */
};

/* Item opcode, after masking off the one-shot and disabled bits. */
typedef enum q2_event_op {
    Q2_EVOP_TRIGGER  = 0x02,   /* arm a list of records                      */
    Q2_EVOP_MOVER_A  = 0x03,
    Q2_EVOP_MOVER_B  = 0x04,
    Q2_EVOP_MOVER_C  = 0x05,
    Q2_EVOP_FXGROUP  = 0x08,   /* NOT a mover; see the header comment        */
    Q2_EVOP_WAIT     = 0x09,   /* live handler, zero uses on disc            */
    Q2_EVOP_ZONEGATE = 0x0F,
    Q2_EVOP_FX       = 0x13,
    Q2_EVOP_ENABLE   = 0x14,   /* clears DISABLED on a list of records       */
    Q2_EVOP_DISABLE  = 0x15,   /* sets DISABLED on a list of records         */
    Q2_EVOP_CALL     = 0x16    /* call a UserFuncs primitive                 */
} q2_event_op;

#define Q2_EVOP_MASK      0x3F
#define Q2_EVOP_ONESHOT   0x40
#define Q2_EVOP_DISABLED  0x80

typedef struct q2_event_item {
    u8        op;        /* raw byte, one-shot and disabled bits intact */
    u8        opcode;    /* op & 0x3F                                   */
    u8        len;       /* including the two header bytes              */
    u32       offset;    /* byte offset of this item within the chunk   */
    const u8 *payload;   /* len - 2 bytes                               */
} q2_event_item;

typedef struct q2_event_record {
    u32       offset;    /* byte offset of this record within the chunk */
    u16       size;      /* including the four header bytes             */
    u8        n_items;
    u8        flags;
} q2_event_record;

typedef struct q2_event_dir_entry {
    char name[13];
    u32  offset;         /* byte offset of a record within the chunk */
} q2_event_dir_entry;

typedef struct q2_events {
    const u8 *data;          /* the whole chunk, borrowed         */
    u32       size;
    u32       record_count;
    u32       dir_count;     /* named entry points; may be 0      */
    u32       dir_offset;    /* where the directory starts (== 4) */
    u32       first_record;  /* where the record area starts      */
} q2_events;

/*
 * Parse the Events chunk of an already-opened file. Borrows the file's buffer,
 * so the file must outlive the result. An 8-byte chunk with no records parses
 * successfully with record_count 0 — that is a real state on 36 chunks, not an
 * error.
 */
q2_result q2_events_parse_common(q2_events *out, const q2_common_file *f);
q2_result q2_events_parse_zone(q2_events *out, const q2_zone_file *f);

/* Decode named entry `index`. Returns false if out of range. */
bool q2_events_get_dir_entry(const q2_events *e, u32 index,
                             q2_event_dir_entry *out);

/*
 * Walk records. Pass offset 0 to start; the returned record's `offset` plus its
 * `size` is the next offset. Returns false at chunk end or on a malformed
 * record.
 */
bool q2_events_first_record(const q2_events *e, q2_event_record *out);
bool q2_events_next_record(const q2_events *e, const q2_event_record *prev,
                           q2_event_record *out);

/*
 * Locate a record by its byte offset — how the named directory and the
 * TrigBounds links refer to records. Returns false if the offset is not a
 * record start.
 */
bool q2_events_record_at(const q2_events *e, u32 offset, q2_event_record *out);

/*
 * Decode item `index` of a record. Every item is validated to fit entirely
 * inside the record BEFORE it is handed back, so the payload pointer and length
 * can be trusted together.
 */
bool q2_events_get_item(const q2_events *e, const q2_event_record *rec,
                        u32 index, q2_event_item *out);

/*
 * The operand list carried by TRIGGER / ENABLE / DISABLE: a s16 count followed
 * by that many u16 record offsets, padded to a multiple of four. Returns the
 * entry count and points `offsets_out` at the raw array, or false if the item
 * is not a list opcode or is malformed.
 *
 * The length identity len == 4 + 2*(count + (count & 1)) holds on 501/501 items
 * and all 812 operands land on a walked record start, which is what makes this
 * safe to follow.
 */
bool q2_events_get_list(const q2_event_item *item, u32 *count_out,
                        const u8 **offsets_out);

/* Entry `index` of a list operand, as a record byte offset. */
u16 q2_events_list_entry(const u8 *offsets, u32 index);

/*
 * The UserFuncs primitive index carried by a CALL item. This is a u8 at +2 —
 * the EXE reads it with lbu in both the exec and load paths. Byte +3 is zero on
 * every item on this disc, so a wider read happens to work here and would break
 * on a variant build.
 */
bool q2_events_get_call_index(const q2_event_item *item, u8 *index_out);

/*
 * The two signed halfwords carried by an FX item.  The retail handler at
 * 0x80027840 accepts precisely an 8-byte item, then passes payload+2 as its
 * damage argument and payload+0 as its means-of-death argument to T_Damage.
 *
 * `point` is deliberately absent: the original neglects to initialise its
 * fifth T_Damage argument.  A caller that needs a reproducible result must
 * choose its own defined point rather than treating either operand as one.
 */
bool q2_events_get_fx_damage(const q2_event_item *item, s16 *mod_out,
                             s16 *damage_out);

#endif /* Q2PSX_EVENTS_H */
