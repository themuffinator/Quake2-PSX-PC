/*
 * spacelights.c — the per-node light index lists.
 *
 * Transcribed from the walk at 0x8006B0C8. The one thing worth restating here
 * rather than only in the header: the index lives in the HIGH halfword of the
 * collision node's +28 field, so it is read at byte offset +30, and the count
 * comes from the successor node exactly as the plane and link counts do.
 */
#include "spacelights.h"

#include <string.h>

/* Byte offset of the SpaceLights start index within a 36-byte collision node.
 * The low halfword at +28 is PrimaryColl's SortData byte offset; this reader is
 * on the SecondaryCol context and deliberately uses the other half. */
#define Q2_COLL_NODE_LIGHT_FIRST 30

static u32 node_light_first(const q2_collision *c, u32 index)
{
    return q2_rd_u16(c->nodes + (size_t)index * Q2_COLL_NODE_SIZE
                     + Q2_COLL_NODE_LIGHT_FIRST);
}

q2_result q2_spacelights_open(q2_spacelights *out, const q2_zone_file *zone,
                              const q2_collision *secondary_hull)
{
    const dat_chunk *chunk;

    if (!out || !zone)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    chunk = zone->chunk[Q2_ZONE_SPACE_LIGHTS];
    if (chunk && chunk->size >= 2) {
        out->data  = chunk->data;
        out->count = chunk->size / 2;
    }

    if (!secondary_hull || !secondary_hull->nodes)
        return Q2_OK;

    /*
     * The sentinel's index is how many entries the hull can reach. Zero is
     * legal and happens on fifteen zones — the front end, the intermission
     * screens and the FMV stubs, which have no static lights and ship the
     * minimum four-byte chunk anyway.
     *
     * It is also what PrimaryColl gives, because that hull's copy of the field
     * is zero on every node of every zone. This cannot tell the two apart, so
     * pass the SECONDARY hull: the primary one silently yields no lights.
     */
    out->used = node_light_first(secondary_hull, secondary_hull->node_count);

    if (out->used > out->count)
        out->used = out->count;

    out->hull = secondary_hull;
    return Q2_OK;
}

bool q2_spacelights_range(const q2_spacelights *sl, u32 node,
                          u32 *out_first, u32 *out_count)
{
    u32 first, end;

    if (out_first) *out_first = 0;
    if (out_count) *out_count = 0;

    if (!sl || !sl->hull || node >= sl->hull->node_count)
        return false;

    first = node_light_first(sl->hull, node);
    end   = node_light_first(sl->hull, node + 1);

    /* The disc is monotonic on every zone, but a corrupt file must not turn
     * into a negative count and walk backwards through the chunk. */
    if (end > sl->count) end = sl->count;
    if (first > end)     first = end;

    if (out_first) *out_first = first;
    if (out_count) *out_count = end - first;
    return true;
}

bool q2_spacelights_entry(const q2_spacelights *sl, u32 index, u16 *out)
{
    if (!sl || !sl->data || index >= sl->count)
        return false;
    if (out)
        *out = q2_rd_u16(sl->data + (size_t)index * 2);
    return true;
}

bool q2_spacelights_get(const q2_spacelights *sl, u32 node, u32 n,
                        u16 *out_light_index)
{
    u32 first, count;

    if (!q2_spacelights_range(sl, node, &first, &count) || n >= count)
        return false;

    return q2_spacelights_entry(sl, first + n, out_light_index);
}
