#include "scene.h"

#include <string.h>

/* For q2_mapmod_clut_index — the palette binding lives with the VRAM layout it
 * indexes, not with the chunk that stores it. */
#include "vram.h"

q2_result q2_scene_parse(q2_scene *out, const q2_zone_file *zone)
{
    const dat_chunk *scene_chunk;
    const dat_chunk *mapmod_chunk;

    if (!out || !zone)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    scene_chunk  = zone->chunk[Q2_ZONE_SCENE];
    mapmod_chunk = zone->chunk[Q2_ZONE_MAP_MOD];

    if (!scene_chunk || !mapmod_chunk)
        return Q2_ERR_BAD_FORMAT;

    if (scene_chunk->size % Q2_SCENE_NODE_SIZE != 0) {
        Q2_ERROR("scene: chunk is %u bytes, not a multiple of %d",
                 scene_chunk->size, Q2_SCENE_NODE_SIZE);
        return Q2_ERR_BAD_FORMAT;
    }

    out->nodes       = scene_chunk->data;
    out->node_count  = scene_chunk->size / Q2_SCENE_NODE_SIZE;
    out->mapmod      = mapmod_chunk->data;
    out->mapmod_size = mapmod_chunk->size;

    if (out->node_count == 0)
        return Q2_ERR_BAD_FORMAT;

    return Q2_OK;
}

bool q2_scene_get_node(const q2_scene *s, u32 index, q2_scene_node *out)
{
    const u8 *rec;
    int i;

    if (!s || !out || index >= s->node_count)
        return false;

    rec = s->nodes + (size_t)index * Q2_SCENE_NODE_SIZE;

    out->mapmod_offset = q2_rd_u32(rec + 0x00);
    out->flags         = q2_rd_u16(rec + 0x08);
    out->area          = q2_rd_u8(rec + 0x0E);

    for (i = 0; i < 3; i++) {
        out->bbox_min[i] = q2_rd_s32(rec + 0x10 + i * 4);
        out->bbox_max[i] = q2_rd_s32(rec + 0x1C + i * 4);
        out->origin[i]   = q2_rd_s32(rec + 0x28 + i * 4);
    }

    return true;
}

void q2_scene_node_bounds(const q2_scene_node *n, s32 min_out[3], s32 max_out[3])
{
    int i;

    if (!n)
        return;

    for (i = 0; i < 3; i++) {
        if (min_out) min_out[i] = n->bbox_min[i] - Q2_SCENE_BBOX_SLOP;
        if (max_out) max_out[i] = n->bbox_max[i] + Q2_SCENE_BBOX_SLOP;
    }
}

bool q2_scene_get_mapmod(const q2_scene *s, u32 index, q2_mapmod_rec *out)
{
    u32 start, end;
    const u8 *rec;
    u32 num_polys, colour_offset, uv_offset;
    u32 i;
    u32 max_col = 0;
    u32 rgb_bytes;

    if (!s || !out || index >= s->node_count)
        return false;

    memset(out, 0, sizeof(*out));

    {
        q2_scene_node node;
        if (!q2_scene_get_node(s, index, &node))
            return false;
        start = node.mapmod_offset;
    }

    /* A record ends where the next node's begins; the last runs to chunk end. */
    if (index + 1 < s->node_count) {
        q2_scene_node next;
        if (!q2_scene_get_node(s, index + 1, &next))
            return false;
        end = next.mapmod_offset;
    } else {
        end = s->mapmod_size;
    }

    if (start > end || end > s->mapmod_size || end - start < 8)
        return false;

    rec = s->mapmod + start;

    num_polys     = q2_rd_u16(rec + 0);
    colour_offset = q2_rd_u16(rec + 2);
    uv_offset     = q2_rd_u32(rec + 4);

    /* The u8 corner-slot encoding cannot express a polygon index >= 64. */
    if (num_polys >= 64)
        return false;

    if (colour_offset != 8 + num_polys * Q2_MAPMOD_POLY_SIZE)
        return false;

    if (uv_offset < colour_offset || uv_offset > end - start)
        return false;

    out->num_polys = num_polys;
    out->polys     = rec + 8;

    /* The colour table's length is implied by the largest corner index used. */
    for (i = 0; i < num_polys; i++) {
        const u8 *p = out->polys + i * Q2_MAPMOD_POLY_SIZE;
        int c;
        for (c = 0; c < 4; c++) {
            if (p[4 + c] > max_col)
                max_col = p[4 + c];
        }
    }

    rgb_bytes = num_polys ? ((3u * (max_col + 1)) + 3u) & ~3u : 0u;

    out->rgb       = rec + colour_offset;
    out->rgb_count = rgb_bytes / 3;

    if (colour_offset + rgb_bytes > uv_offset && num_polys) {
        /* The tables would overlap, which the format never does. */
        return false;
    }

    out->uv       = rec + uv_offset;
    out->uv_count = (end - start - uv_offset) / 8;

    return true;
}

bool q2_mapmod_get_poly(const q2_mapmod_rec *rec, u32 poly, q2_mapmod_poly *out)
{
    const u8 *p;

    if (!rec || !out || poly >= rec->num_polys)
        return false;

    p = rec->polys + (size_t)poly * Q2_MAPMOD_POLY_SIZE;

    memcpy(out->vtx, p + 0, 4);
    memcpy(out->col, p + 4, 4);
    out->clut   = q2_rd_u16(p + 8);
    out->tpage  = p[10];
    out->uv_idx = (u8)(p[11] & 0x3F);
    out->flags  = (u8)(p[11] >> 6);

    return true;
}

bool q2_mapmod_rec_is_sealing(const q2_mapmod_rec *rec)
{
    u32 p;

    /* A record with no polygons seals nothing. Five nodes on the disc are
     * empty, and calling those sealing would be a vacuous truth that hides
     * them from every caller that asks this question. */
    if (!rec || rec->num_polys == 0)
        return false;

    for (p = 0; p < rec->num_polys; p++) {
        q2_mapmod_poly poly;

        if (!q2_mapmod_get_poly(rec, p, &poly))
            return false;
        if (q2_mapmod_clut_index(poly.clut) != 0)
            return false;
    }

    return true;
}
