/*
 * spacelights.h — the SpaceLights chunk, and which lights reach a given place.
 *
 * ---------------------------------------------------------------------------
 * What this chunk is, and why it looked undecodable
 * ---------------------------------------------------------------------------
 * `SpaceLights` is a flat `uint16_t` array with no header, no length prefix and
 * no offset table. Two earlier passes tried to partition it across the zone's
 * SCENE nodes and got 0.68…7.12 entries per node with no rule that fit — which
 * is the whole of openquestions #9.
 *
 * The partition exists. It is just not in this chunk, and it is not keyed by a
 * scene node:
 *
 *     the light index list for SECONDARY COLLISION node i is
 *         SpaceLights[ node[i].c_hi .. node[i+1].c_hi )
 *
 * where `c_hi` is the HIGH halfword of the collision node's 32-bit field at
 * +28. The low halfword is the PrimaryColl cell's SortData byte offset; this
 * independent SecondaryCol consumer reads the high half at byte offset +30.
 *
 * Read out of the entity light gather at 0x8006B0C8:
 *
 *     lh   a0, 162(s2)          ; entity's collision node, -1 if none
 *     bltz a0, <no node>
 *     sll  v0, a0, 3            ; \
 *     addu v0, v0, a0           ;  > index * 36 — the collision node stride
 *     sll  v0, v0, 2            ; /
 *     lw   v1, -28692(v1)       ; 0x800C8FEC = ctx+0x04 of the SECONDARY
 *                               ;              collision context, i.e. its
 *                               ;              node array
 *     lw   a0, 11984(a0)        ; 0x800B2ED0 = the SpaceLights chunk
 *     lh   v1, 30(v0)           ; node[i].c_hi
 *     lh   v0, 66(v0)           ; node[i+1].c_hi   (30 + 36)
 *     ...                       ; walk the halfwords between them
 *     sll  a1, v0, 3            ; \
 *     subu a1, a1, v0           ;  > index * 28 — the Lights record stride
 *     sll  a1, a1, 2            ; /
 *     lw   v0, 11988(v0)        ; 0x800B2ED4 = the Lights chunk
 *
 * So the entries are indices into `COMMON.DAT`'s `Lights` array, and the
 * partition rides on the collision hull the player actually moves in. The
 * sentinel node that already exists to terminate the plane and link ranges
 * terminates this one too.
 *
 * ---------------------------------------------------------------------------
 * Checked against the disc
 * ---------------------------------------------------------------------------
 * `q2psx-inspect lights` runs the following over all 115 zones:
 *
 *   - `PrimaryColl`'s high halfword is ZERO on every node of every zone. Only
 *     `SecondaryCol` carries the index, which is an independent confirmation
 *     that the engine's choice of context is the right one — the same field in
 *     the other hull was never filled in.
 *   - `SecondaryCol`'s is non-decreasing on 115/115 and starts at 0 on 115/115.
 *   - All 37,285 reachable entries index a light that exists: zero out of range
 *     against light counts of 96…374.
 *   - 13,805 nodes, mean 2.70 lights each, max 45, and 1,682 nodes with none.
 *
 * The tail past the sentinel is build residue, 0…12 halfwords per zone and
 * non-zero in 261 of them across the disc. The engine cannot reach it and
 * neither does this parser.
 */
#ifndef Q2PSX_SPACELIGHTS_H
#define Q2PSX_SPACELIGHTS_H

#include "collision.h"
#include "level.h"
#include "q2psx.h"

/*
 * The chunk plus the hull that partitions it. Borrows both, so the zone file
 * and the collision must outlive it.
 */
typedef struct q2_spacelights {
    const u8 *data;          /* borrowed: the raw halfword array           */
    u32       count;         /* halfwords present in the chunk             */
    u32       used;          /* halfwords the sentinel node reaches, <= count */
    const q2_collision *hull;/* the SECONDARY hull; NULL leaves every range empty */
} q2_spacelights;

/*
 * Open the chunk. `hull` must be the zone's SecondaryCol, parsed with
 * q2_collision_parse(..., Q2_COLL_SECONDARY). Passing the primary hull is not
 * detectable here and yields every range empty, because that hull's copy of the
 * field is zero on every node of every zone.
 *
 * A zone with no SpaceLights chunk, or an empty one, opens successfully with
 * count == 0. Seventeen zones on the disc ship a 4-byte (2-entry) chunk, and
 * fifteen of those — the front end, the intermissions and the FMV stubs — also
 * have an entirely zero partition, so no node reaches any of it.
 */
q2_result q2_spacelights_open(q2_spacelights *out, const q2_zone_file *zone,
                              const q2_collision *secondary_hull);

/* The half-open range of light indices belonging to collision node `node`.
 * Returns false — with both outputs zeroed — for an out-of-range node. */
bool q2_spacelights_range(const q2_spacelights *sl, u32 node,
                          u32 *out_first, u32 *out_count);

/* The n'th light index of node `node`. */
bool q2_spacelights_get(const q2_spacelights *sl, u32 node, u32 n,
                        u16 *out_light_index);

/* The raw halfword at `index`, for tools that want to walk the array flat. */
bool q2_spacelights_entry(const q2_spacelights *sl, u32 index, u16 *out);

#endif /* Q2PSX_SPACELIGHTS_H */
