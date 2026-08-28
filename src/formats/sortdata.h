/*
 * sortdata.h — the zone's draw order, and the answer to open question #7.
 *
 * `SortData` was the last undecoded chunk that changes what the screen looks
 * like. It was described as "bit-packed, no offset table, no fixed per-node
 * record (4.0…88.6 bytes per scene node — a 22x spread). Almost certainly draw
 * order data; transparency and overdraw will be wrong without it." All of that
 * is right, and the reason no record size fits is that there are no records: it
 * is a variable-width opcode stream with a per-zone header declaring its own
 * field widths.
 *
 * The reader is the zone draw's own, inlined seven times at 0x80066B70 through
 * 0x800676D8. Nothing else in the image touches the chunk.
 *
 * ---------------------------------------------------------------------------
 * The bit reader
 * ---------------------------------------------------------------------------
 * LSB-first within little-endian 32-bit words, with a mask table at 0x8009FBF0
 * holding (1 << n) - 1 at stride 4. Three globals hold the state: the current
 * word (0x800B2C44), the count of bits still unread in it (0x800B2C8C), and the
 * pointer to the next word (0x800B2C90).
 *
 *     read(n):
 *         if bits_left >= n:
 *             v = (word >> (32 - bits_left)) & mask[n];  bits_left -= n
 *         else:
 *             v = bits_left ? (word >> (32 - bits_left)) : 0
 *             got = bits_left;  word = *ptr++
 *             v |= (word & mask[n - got]) << got
 *             bits_left = 32 - (n - got)
 *
 * The stream does not start at the chunk's beginning. The viewport's own record
 * carries a BYTE offset at +28 (`lh` at 0x80066AFC), and the reader word-aligns
 * down and skips the remainder as bits — so a zone's streams can share a chunk
 * at arbitrary byte boundaries.
 *
 * ---------------------------------------------------------------------------
 * The header: seven widths, then a base
 * ---------------------------------------------------------------------------
 * Seven fields, each holding its width MINUS ONE, so a width is 1..16 and the
 * encoder never wastes a code on the impossible zero-width field:
 *
 *     4 bits   w_base      - 1     the width of the base field, and of any
 *                                  later base update
 *     3 bits   w_op_short  - 1     opcode width in windowed mode
 *     4 bits   w_op_long   - 1     opcode width in absolute mode
 *     3 bits   w_f1        - 1     screen-region record field 1
 *     3 bits   w_f3        - 1                            3
 *     3 bits   w_f4        - 1                            4
 *     4 bits   w_f2        - 1     screen-region skip length, in BITS
 *   w_base bits  base            added to a windowed-mode node index
 *
 * 24 header bits plus w_base. The odd mixture of 3- and 4-bit widths is not a
 * pattern this file invented: it is the literal sequence of `slti` bounds at
 * 0x80066B30, 0x80066BCC, 0x80066C78, 0x80066D24, 0x80066DD0, 0x80066E7C and
 * 0x80066F28.
 *
 * ---------------------------------------------------------------------------
 * The opcode stream
 * ---------------------------------------------------------------------------
 * Two modes, because a zone's node indices cluster. Windowed mode spends
 * w_op_short bits and adds `base`; absolute mode spends w_op_long and does not.
 * Opcode 2 switches modes and carries the first value of the new mode with it.
 *
 *     0        end of stream                       (0x80067238)
 *     1        a SCREEN-REGION change              (0x80067240)
 *     2        switch mode; the replacement opcode follows, read at the OTHER
 *              mode's width                        (0x8006718C, 0x80067604)
 *     >= 3     scene node (op - 3), plus `base` in windowed mode
 *                                                  (0x800676B4)
 *
 * A screen-region record is four fields in stream order — f1 (w_f1), f2
 * (w_f2), f3 (w_f3), f4 (w_f4) — handed to 0x80065D08 as
 * (f1 signed, f3 & 0xFF, f4 & 0xFF, bucket). What happens next depends on what
 * that projection returns, which is why this decoder cannot be a pure pull loop:
 *
 *     drawn == false  ->  skip f2 more bits. f2 is a BIT LENGTH, and the skip
 *                         is done with the word-stepping arithmetic at
 *                         0x800675A8, so it can cross any number of words.
 *     drawn == true   ->  if f2 is non-zero, read w_base bits as a NEW base.
 *
 * f1 names the Scene/Points marker projected into a rectangle, f3 names its
 * parent screen record, and f4 names the seven-bit draw area registered at the
 * current bucket. The GTE origin and GPU draw environment change to that local
 * rectangle until a later in-list DRAWENV restores the previous one. The return
 * value is whether the rectangle has non-zero area. f2 is therefore BRANCH
 * DATA, not an entity model payload: the rejected arm skips f2 bits, while the
 * accepted arm uses a non-zero f2 as the signal that a new w_base-bit window
 * base follows. `Q2_SORT_ENTITY` and q2_sort_entity_resolve retain their old
 * names only for source compatibility; call the latter with the projection
 * result before asking for the next item.
 *
 * ---------------------------------------------------------------------------
 * What this means for the sort, and it is not what the port assumed
 * ---------------------------------------------------------------------------
 * The camera area's full-view insertion point is seeded at bucket 45
 * (0x80066970), then the actual opcode stream starts at 43 (0x80066A34). The
 * latter is decremented in exactly one place: after an entity record
 * (0x800675E0). The node path never touches it —
 * it reads the current value at 0x80067AE8 / 0x80067DB8, draws the whole node
 * into that one bucket, and stops the whole stream if the bucket has fallen
 * below 4 (0x80067B28, 0x80067DF0).
 *
 * Two things follow, and both are corrections to what world.c has been doing:
 *
 *   - THE WORLD IS NOT DEPTH-SORTED AT ALL. Its order is authored, and the
 *     ordering table is only carrying it. Computing a bucket per quad from the
 *     GTE's depth — which is what the port does — produces a *different* order,
 *     not a finer-grained version of the same one.
 *   - Buckets exist to interleave ENTITIES with the world at the right depth.
 *     A run of nodes shares one bucket; the bucket steps down each time an
 *     screen region is placed. That is the whole mechanism.
 *
 * Because a PSX ordering table is a prepend list, primitives within one bucket
 * draw in reverse insertion order, so a run of nodes in one bucket paints
 * last-emitted-first. The stream's order is therefore front-to-back as emitted
 * and back-to-front as drawn.
 */
#ifndef Q2PSX_SORTDATA_H
#define Q2PSX_SORTDATA_H

#include "level.h"
#include "q2psx.h"

/* The full-view camera area is seeded at 45; the authored stream itself starts
 * at 43. They are two consecutive setup operations, not alternatives. */
#define Q2_SORT_BUCKET_SEED  45
#define Q2_SORT_BUCKET_START 43

/* The stream stops when the bucket falls below this (0x80067B28). */
#define Q2_SORT_BUCKET_FLOOR 4

typedef struct q2_sortdata {
    const u8 *data;    /* borrowed from the zone file */
    u32       size;
} q2_sortdata;

q2_result q2_sortdata_parse(q2_sortdata *out, const q2_zone_file *zone);

typedef struct q2_sort_header {
    u8  w_base;
    u8  w_op_short;
    u8  w_op_long;
    u8  w_f1;
    u8  w_f2;
    u8  w_f3;
    u8  w_f4;
    u32 base;
} q2_sort_header;

typedef enum q2_sort_kind {
    Q2_SORT_END = 0,
    Q2_SORT_NODE,
    Q2_SORT_ENTITY
} q2_sort_kind;

typedef struct q2_sort_item {
    q2_sort_kind kind;

    u32 node;        /* NODE: the Scene node index                     */

    s32 f1;          /* ENTITY: signed, argument 1 of the entity draw   */
    u32 f2;          /* ENTITY: rejected-branch skip length in bits      */
    u32 f3, f4;      /* ENTITY: bytes, arguments 2 and 3                */

    u32 bucket;      /* the ordering-table bucket this item belongs in  */
} q2_sort_item;

typedef struct q2_sort_reader {
    const u8 *data;
    u32       size;

    u32  word;        /* 0x800B2C44 */
    u32  bits_left;   /* 0x800B2C8C */
    u32  next_word;   /* 0x800B2C90, as a word index                    */

    q2_sort_header hdr;

    u32  bucket;
    bool windowed;    /* the sp+128 mode latch, inverted for readability */
    bool ended;
    bool overrun;     /* the stream ran off the end of the chunk         */

    /* Set while an ENTITY item is waiting for q2_sort_entity_resolve. */
    bool pending_entity;
    u32  pending_f2;
} q2_sort_reader;

/*
 * Start reading at `byte_offset` into the chunk — the viewport record's own +28
 * — with `bucket0` as the starting ordering-table bucket. Decodes the header.
 * Returns false if the chunk cannot supply even the header.
 */
bool q2_sort_begin(q2_sort_reader *r, const q2_sortdata *sd,
                   u32 byte_offset, u32 bucket0);

/*
 * Pull the next item. Returns false at end of stream, on overrun, or while an
 * entity is unresolved. `out` is always written when true is returned.
 */
bool q2_sort_next(q2_sort_reader *r, q2_sort_item *out);

/*
 * Tell the reader whether the entity it just handed you was actually drawn.
 * Must be called after every Q2_SORT_ENTITY item and before the next
 * q2_sort_next, because the stream's shape depends on the answer.
 */
void q2_sort_entity_resolve(q2_sort_reader *r, bool drawn);

/* How many bits the reader has consumed, for callers that want to check a
 * stream ends where the next one begins. */
u32 q2_sort_bit_position(const q2_sort_reader *r);

/*
 * Enumerate the chunk's streams by tiling it.
 *
 * This is an OFFLINE census helper, not the renderer's lookup. Runtime already
 * has the exact byte offset on disc: PrimaryColl node +28, selected by the
 * viewport camera's cell. Tiling remains useful for checking the bit grammar
 * and surveying otherwise-unreferenced tails: decode the rejected entity arm
 * to the end opcode, round up to the next byte, and start again.
 *
 * Writes up to `max` offsets and returns how many streams the chunk holds —
 * which may exceed `max`, so a caller can size an array in two passes. A stream
 * that overruns ends the enumeration, since nothing after it can be trusted.
 *
 * Measured over the whole disc by `q2psx-inspect surfaces`: 8,968 streams across
 * 115 chunks, 87 of them overrunning at chunk tails, and 178,801 node references
 * with none out of range.
 */
u32 q2_sortdata_enumerate(const q2_sortdata *sd, u32 *offsets, u32 max);

/* The byte offset of tiled stream `index`, or false if the chunk has no such
 * stream. Diagnostic only; live drawing reads PrimaryColl.sort_offset. */
bool q2_sortdata_stream_offset(const q2_sortdata *sd, u32 index, u32 *out);

#endif /* Q2PSX_SORTDATA_H */
