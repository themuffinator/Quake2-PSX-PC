/*
 * collision.c — the engine's collision model, transcribed from SLES_015.34.
 *
 * Every function here names the address it came from. Where the original's
 * arithmetic is observable — 16-bit wrapping, truncating division, an exact
 * rational fraction carried instead of a divide — it is reproduced rather than
 * cleaned up, because those are the parts that decide where a player ends up.
 */
#include "collision.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Record access                                                              */
/* ------------------------------------------------------------------------- */

static void read_node(const u8 *rec, q2_coll_node *out)
{
    int i;

    for (i = 0; i < 3; i++) {
        out->bbox_min[i] = q2_rd_s32(rec + 0 + i * 4);
        out->bbox_max[i] = q2_rd_s32(rec + 12 + i * 4);
    }
    out->first_plane = q2_rd_u16(rec + 24);
    out->first_link  = q2_rd_u16(rec + 26);
    out->sort_offset = q2_rd_s16(rec + 28);
    out->first_light = q2_rd_u16(rec + 30);
    out->contents    = q2_rd_u8(rec + 32);
    out->pad[0]      = q2_rd_u8(rec + 33);
    out->pad[1]      = q2_rd_u8(rec + 34);
    out->pad[2]      = q2_rd_u8(rec + 35);
}

/* The node record without the struct copy, for the hot paths. */
static const u8 *node_rec(const q2_collision *c, u32 index)
{
    return c->nodes + (size_t)index * Q2_COLL_NODE_SIZE;
}

/* First plane / one-past-last plane of a node, both already masked. 0x800441A8
 * reads the successor's field through +60, i.e. +36+24, which is why the
 * sentinel has to exist. */
static void node_plane_range(const q2_collision *c, u32 index,
                             u32 *first, u32 *end)
{
    const u8 *rec = node_rec(c, index);

    *first = q2_rd_u16(rec + 24) & Q2_COLL_PLANE_MASK;
    *end   = q2_rd_u16(rec + 60) & Q2_COLL_PLANE_MASK;

    if (*end > c->plane_count) *end = c->plane_count;
    if (*first > *end)         *first = *end;
}

static void node_link_range(const q2_collision *c, u32 index,
                            u32 *first, u32 *end)
{
    const u8 *rec = node_rec(c, index);

    *first = q2_rd_u16(rec + 26);
    *end   = q2_rd_u16(rec + 62);

    if (*end > c->link_count) *end = c->link_count;
    if (*first > *end)        *first = *end;
}

q2_result q2_collision_parse(q2_collision *out, const q2_zone_file *zone,
                             q2_coll_which which)
{
    const dat_chunk *chunk;
    u32 num_nodes, num_planes, num_links;
    size_t nodes_bytes, planes_bytes, fixed;
    const u8 *p;
    q2_coll_node sentinel;

    if (!out || !zone)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    out->hit_plane     = -1;
    out->hit_node      = -1;
    out->current_node  = -1;
    out->blocked_node  = -1;
    out->blocked_plane = -1;

    chunk = zone->chunk[which == Q2_COLL_PRIMARY
                        ? Q2_ZONE_PRIMARY_COLL
                        : Q2_ZONE_SECONDARY_COL];
    if (!chunk || chunk->size < 4)
        return Q2_ERR_BAD_FORMAT;

    p          = chunk->data;
    num_nodes  = q2_rd_u16(p + 0);
    num_planes = q2_rd_u16(p + 2);

    /* Guard every multiply before it can overflow or run past the chunk. */
    nodes_bytes  = (size_t)(num_nodes + 1) * Q2_COLL_NODE_SIZE;
    planes_bytes = (size_t)num_planes * Q2_COLL_PLANE_SIZE;
    fixed        = 4 + nodes_bytes + planes_bytes;

    if (fixed > chunk->size) {
        Q2_ERROR("collision: %u nodes + %u planes need %zu bytes, chunk has %u",
                 num_nodes, num_planes, fixed, chunk->size);
        return Q2_ERR_BAD_FORMAT;
    }

    if ((chunk->size - fixed) % Q2_COLL_LINK_SIZE != 0)
        return Q2_ERR_BAD_FORMAT;

    num_links = (u32)((chunk->size - fixed) / Q2_COLL_LINK_SIZE);

    /* The sentinel's totals must agree with the header, which is what turns the
     * size arithmetic from plausible into proven for this file. */
    read_node(p + 4 + (size_t)num_nodes * Q2_COLL_NODE_SIZE, &sentinel);

    if ((sentinel.first_plane & Q2_COLL_PLANE_MASK) != num_planes ||
        sentinel.first_link != num_links) {
        Q2_ERROR("collision: sentinel says %u planes / %u links, header implies %u / %u",
                 sentinel.first_plane & Q2_COLL_PLANE_MASK, sentinel.first_link,
                 num_planes, num_links);
        return Q2_ERR_BAD_FORMAT;
    }

    out->nodes       = p + 4;
    out->planes      = p + 4 + nodes_bytes;
    out->links       = p + fixed;
    out->node_count  = num_nodes;
    out->plane_count = num_planes;
    out->link_count  = num_links;

    return Q2_OK;
}

bool q2_collision_get_node(const q2_collision *c, u32 index, q2_coll_node *out)
{
    if (!c || !out || !c->nodes || index > c->node_count)   /* == count is the sentinel */
        return false;

    read_node(node_rec(c, index), out);
    return true;
}

bool q2_collision_get_plane(const q2_collision *c, u32 index, q2_coll_plane *out)
{
    const u8 *rec;

    if (!c || !out || !c->planes || index >= c->plane_count)
        return false;

    rec = c->planes + (size_t)index * Q2_COLL_PLANE_SIZE;

    out->a  = q2_rd_u16(rec + 0);
    out->b  = q2_rd_u16(rec + 2);
    out->c  = q2_rd_u16(rec + 4);
    out->nx = q2_rd_s16(rec + 6);
    out->ny = q2_rd_s16(rec + 8);
    out->nz = q2_rd_s16(rec + 10);

    return true;
}

bool q2_collision_get_link(const q2_collision *c, u32 index, q2_coll_link *out)
{
    const u8 *rec;

    if (!c || !out || !c->links || index >= c->link_count)
        return false;

    rec = c->links + (size_t)index * Q2_COLL_LINK_SIZE;

    out->raw        = q2_rd_u16(rec + 0);
    out->back_plane = q2_rd_u16(rec + 2);

    return true;
}

u32 q2_collision_node_plane_count(const q2_collision *c, u32 index)
{
    u32 first, end;

    if (!c || !c->nodes || index >= c->node_count)
        return 0;

    node_plane_range(c, index, &first, &end);
    return end - first;
}

u32 q2_collision_node_link_count(const q2_collision *c, u32 index)
{
    u32 first, end;

    if (!c || !c->nodes || index >= c->node_count)
        return 0;

    node_link_range(c, index, &first, &end);
    return end - first;
}

bool q2_collision_node_is_solid(const q2_collision *c, u32 index)
{
    /* `>=`: index == node_count is the sentinel record, not a node. */
    if (!c || !c->nodes || index >= c->node_count)
        return false;

    return (q2_rd_u16(node_rec(c, index) + 24) & Q2_COLL_SOLID) != 0;
}

/* ------------------------------------------------------------------------- */
/* The 16-bit relative frame                                                  */
/* ------------------------------------------------------------------------- */
/*
 * 0x8004414C..0x80044184. Both operands are loaded with `lhu` and subtracted,
 * so the result is the true difference modulo 65536, read back as signed.
 * Everything downstream lives in this frame.
 */
static void relative_rec(const u8 *rec, const s32 point[3], s16 out[3])
{
    int i;

    for (i = 0; i < 3; i++) {
        u16 pv = (u16)((u32)point[i] & 0xFFFFu);
        u16 bv = q2_rd_u16(rec + i * 4);        /* low half of bbox_min[i] */
        out[i] = (s16)(u16)(pv - bv);
    }
}

void q2_coll_relative(const q2_coll_node *node, const s32 point[3], s16 out[3])
{
    int i;

    if (!node || !point || !out)
        return;

    for (i = 0; i < 3; i++) {
        u16 pv = (u16)((u32)point[i] & 0xFFFFu);
        u16 bv = (u16)((u32)node->bbox_min[i] & 0xFFFFu);
        out[i] = (s16)(u16)(pv - bv);
    }
}

bool q2_coll_plane_point(const q2_collision *c, u32 node_index, u32 plane_index,
                         s32 out_point[3])
{
    q2_coll_node node;
    q2_coll_plane plane;

    if (!c || !out_point)
        return false;
    if (!q2_collision_get_node(c, node_index, &node))
        return false;
    if (!q2_collision_get_plane(c, plane_index, &plane))
        return false;

    /* An unsigned displacement from the node's minimum corner. There is
     * deliberately no bounding-box check: a plane is infinite and its reference
     * point need not sit inside the box. */
    out_point[0] = node.bbox_min[0] + (s32)plane.a;
    out_point[1] = node.bbox_min[1] + (s32)plane.b;
    out_point[2] = node.bbox_min[2] + (s32)plane.c;

    return true;
}

s32 q2_coll_plane_distance(const q2_collision *c, u32 node_index,
                           u32 plane_index, const s32 point[3])
{
    const u8 *prec;
    s16 rel[3];

    if (!c || !point || !c->nodes || node_index > c->node_count)
        return 0;
    if (!c->planes || plane_index >= c->plane_count)
        return 0;

    relative_rec(node_rec(c, node_index), point, rel);
    prec = c->planes + (size_t)plane_index * Q2_COLL_PLANE_SIZE;

    /* 0x80044384: (rel - p) . n, with p sign-extended and the subtraction in
     * 32 bits. Positive is OUTSIDE the cell. */
    return (s32)(rel[0] - q2_rd_s16(prec + 0)) * q2_rd_s16(prec + 6)
         + (s32)(rel[1] - q2_rd_s16(prec + 2)) * q2_rd_s16(prec + 8)
         + (s32)(rel[2] - q2_rd_s16(prec + 4)) * q2_rd_s16(prec + 10);
}

/* ------------------------------------------------------------------------- */
/* 0x80044098 — point in node                                                 */
/* ------------------------------------------------------------------------- */
bool q2_coll_point_in_node(const q2_collision *c, u32 index, const s32 point[3])
{
    const u8 *rec;
    s16 rel[3];
    u32 first, end, k;
    int i;

    if (!c || !point || !c->nodes || index >= c->node_count)
        return false;

    rec = node_rec(c, index);

    /* The AABB gate comes first and is a plain 32-bit compare. It is also what
     * keeps the 16-bit frame below well defined for every point that survives. */
    for (i = 0; i < 3; i++) {
        s32 v = point[i];
        if (v > q2_rd_s32(rec + 12 + i * 4)) return false;
        if (v < q2_rd_s32(rec + 0  + i * 4)) return false;
    }

    /* 0x80044190: a solid node holds nothing. Through the accessor rather
     * than a second copy of the same mask: one place decides what solid
     * means, and the two cannot drift. */
    if (q2_collision_node_is_solid(c, index))
        return false;

    relative_rec(rec, point, rel);
    node_plane_range(c, index, &first, &end);

    for (k = first; k < end; k++) {
        const u8 *p = c->planes + (size_t)k * Q2_COLL_PLANE_SIZE;
        s32 d;

        /*
         * 0x800441DC..0x80044248. Here — and only here — the plane point is
         * read UNSIGNED and each difference is truncated back to 16 bits before
         * the multiply. The two readings agree on every plane the disc holds,
         * because no stored component reaches 0x8000, but the truncation is
         * reproduced anyway: it is what the console executes.
         */
        s16 dx = (s16)(u16)((u16)rel[0] - q2_rd_u16(p + 0));
        s16 dy = (s16)(u16)((u16)rel[1] - q2_rd_u16(p + 2));
        s16 dz = (s16)(u16)((u16)rel[2] - q2_rd_u16(p + 4));

        d = (s32)dx * q2_rd_s16(p + 6)
          + (s32)dy * q2_rd_s16(p + 8)
          + (s32)dz * q2_rd_s16(p + 10);

        if (d > 0)
            return false;
    }

    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x80044F54 — locate the node holding a point                               */
/* ------------------------------------------------------------------------- */
s32 q2_coll_find_node(const q2_collision *c, const s32 point[3], s32 hint,
                      bool brute)
{
    u32 i;

    if (!c || !point || !c->nodes)
        return -1;

    if (hint >= 0 && (u32)hint < c->node_count) {
        u32 first, end;

        if (q2_coll_point_in_node(c, (u32)hint, point))
            return hint;

        /*
         * The hint's portal neighbours, in link order. This is the whole reason
         * the link list exists on the query side: an entity that has moved is
         * nearly always one portal away from where it was.
         */
        node_link_range(c, (u32)hint, &first, &end);

        for (i = first; i < end; i++) {
            u32 nb = q2_rd_u16(c->links + (size_t)i * Q2_COLL_LINK_SIZE)
                     & Q2_COLL_LINK_NODE_MASK;

            if (nb < c->node_count && q2_coll_point_in_node(c, nb, point))
                return (s32)nb;
        }
    }

    if (!brute)
        return -1;

    for (i = 0; i < c->node_count; i++) {
        if (q2_coll_point_in_node(c, i, point))
            return (s32)i;
    }

    return -1;
}

/* ------------------------------------------------------------------------- */
/* 0x80044294 — may a point occupy this node                                  */
/* ------------------------------------------------------------------------- */
bool q2_coll_probe(const q2_collision *c, u32 node, const s32 point[3],
                   s32 skip_plane, bool *out_inside, s32 *out_marginal_plane)
{
    const u8 *rec;
    s16 rel[3];
    u32 first, end, k;
    bool inside = true;
    s32  marginal = -1;

    if (out_inside)         *out_inside = false;
    if (out_marginal_plane) *out_marginal_plane = -1;

    if (!c || !point || !c->nodes || node >= c->node_count)
        return false;

    rec = node_rec(c, node);

    /* 0x800442BC: solid nodes are refused before anything is computed, and the
     * original memsets the whole result rather than reporting a near miss. */
    if (q2_rd_u16(rec + 24) & Q2_COLL_SOLID)
        return false;

    relative_rec(rec, point, rel);
    node_plane_range(c, node, &first, &end);

    for (k = first; k < end; k++) {
        const u8 *p = c->planes + (size_t)k * Q2_COLL_PLANE_SIZE;
        s32 d;

        /* The shared face of the portal we came through. The entering point
         * sits exactly on it, and floating it either way would be a coin
         * flip, so the original simply skips it. */
        if ((s32)(k - first) == skip_plane)
            continue;

        d = (s32)(rel[0] - q2_rd_s16(p + 0)) * q2_rd_s16(p + 6)
          + (s32)(rel[1] - q2_rd_s16(p + 2)) * q2_rd_s16(p + 8)
          + (s32)(rel[2] - q2_rd_s16(p + 4)) * q2_rd_s16(p + 10);

        if (d <= 0)
            continue;                       /* inside this plane */

        if (d >= Q2_COLL_PROBE_SLACK)
            return false;                   /* properly outside: refuse */

        inside   = false;                   /* outside, but within the slack */
        marginal = (s32)(k - first);
    }

    if (out_inside)         *out_inside = inside;
    if (out_marginal_plane) *out_marginal_plane = marginal;

    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x800445A4 — clip a move against one node                                  */
/* ------------------------------------------------------------------------- */
void q2_coll_clip(q2_collision *c, u32 node, const s32 point[3],
                  const s16 delta[3], s32 *out_num, s32 *out_den)
{
    const u8 *rec;
    s16 rel[3];
    u32 first, end, k;
    s32 best_num = 1, best_den = 1;

    if (out_num) *out_num = 1;
    if (out_den) *out_den = 1;

    if (!c || !point || !delta || !c->nodes || node >= c->node_count)
        return;

    c->hit_plane = -1;

    rec = node_rec(c, node);
    relative_rec(rec, point, rel);
    node_plane_range(c, node, &first, &end);

    for (k = first; k < end; k++) {
        const u8 *p = c->planes + (size_t)k * Q2_COLL_PLANE_SIZE;
        s16 nx = q2_rd_s16(p + 6), ny = q2_rd_s16(p + 8), nz = q2_rd_s16(p + 10);
        s32 den, num;

        /* 0x800446B8: only planes the move is heading toward. */
        den = (s32)nx * delta[0] + (s32)ny * delta[1] + (s32)nz * delta[2];
        if (den <= 0)
            continue;

        /*
         * 0x800446C0: the plane point MINUS the relative position, so this is
         * the negation of q2_coll_plane_distance — positive means still inside.
         * A negative value means the point already sits outside the plane, and
         * 0x80044710 skips it. That is the whole defence against a node whose
         * planes are not exactly convex: the move is simply not clipped by a
         * plane it has already crossed.
         */
        num = (s32)(q2_rd_s16(p + 0) - rel[0]) * nx
            + (s32)(q2_rd_s16(p + 2) - rel[1]) * ny
            + (s32)(q2_rd_s16(p + 4) - rel[2]) * nz;
        if (num < 0)
            continue;

        /*
         * 0x800B1010: compare num/den against the running best as an exact
         * rational. Both products are non-negative here, which is why the
         * original's unsigned high/low word compare is correct.
         */
        if ((s64)num * best_den < (s64)den * best_num) {
            best_num  = num;
            best_den  = den;
            c->hit_plane       = (s32)(k - first);
            c->hit_plane_index = (s32)k;
            c->hit_node        = (s32)node;
        }
    }

    if (out_num) *out_num = best_num;
    if (out_den) *out_den = best_den;
}

/* ------------------------------------------------------------------------- */
/* 0x80044420 — cross a portal                                                */
/* ------------------------------------------------------------------------- */
s32 q2_coll_cross(const q2_collision *c, u32 node, const s32 point[3],
                  s32 hit_plane, s32 *out_near_node, s32 *out_near_plane)
{
    u32 first, end, i;
    s32 near_node = -1, near_plane = -1;

    if (out_near_node)  *out_near_node = -1;
    if (out_near_plane) *out_near_plane = -1;

    if (!c || !point || !c->nodes || node >= c->node_count || hit_plane < 0)
        return -1;

    node_link_range(c, node, &first, &end);

    for (i = first; i < end; i++) {
        const u8 *rec = c->links + (size_t)i * Q2_COLL_LINK_SIZE;
        u16 raw  = q2_rd_u16(rec + 0);
        s32 back = q2_rd_s16(rec + 2);
        u32 nb;
        bool inside = false;
        s32  marginal = -1;

        /* 0x800444A8: only links whose portal lies in the plane we stopped at. */
        if ((s32)(raw >> Q2_COLL_LINK_PLANE_SHIFT) != hit_plane)
            continue;

        nb = raw & Q2_COLL_LINK_NODE_MASK;
        if (nb >= c->node_count)
            continue;

        if (!q2_coll_probe(c, nb, point, back, &inside, &marginal))
            continue;

        if (inside) {
            /* 0x800444DC: an outright fit ends the search immediately. */
            if (out_near_node)  *out_near_node = -1;
            if (out_near_plane) *out_near_plane = -1;
            return (s32)nb;
        }

        /* 0x80045218: remember the near miss and keep looking. */
        near_node  = (s32)nb;
        near_plane = marginal;
    }

    if (out_near_node)  *out_near_node = near_node;
    if (out_near_plane) *out_near_plane = near_plane;

    return -1;
}

/* ------------------------------------------------------------------------- */
/* 0x80044C44 — the swept move                                                */
/* ------------------------------------------------------------------------- */
static void path_reset(q2_collision *c)
{
    c->path_count = 0;
    memset(c->path, 0, sizeof(c->path));
}

/* The terminal record: the original always writes the final position and the
 * final node's contents into the slot the list has reached, appended or not. */
static void path_terminate(q2_collision *c, const s32 pos[3])
{
    q2_coll_contact *slot = &c->path[c->path_count];

    slot->pos[0] = pos[0];
    slot->pos[1] = pos[1];
    slot->pos[2] = pos[2];

    if (c->current_node >= 0 && (u32)c->current_node < c->node_count)
        slot->id = (s16)q2_rd_u8(node_rec(c, (u32)c->current_node) + 32);
    else
        slot->id = 0;
}

bool q2_coll_move(q2_collision *c, const s32 start[3], const s32 end[3],
                  s32 node_hint, s32 out_pos[3], s32 *out_node)
{
    s32 pos[3];
    s32 node;
    s32 prev_contents = -1;
    int i;

    if (!c || !start || !end)
        return false;

    pos[0] = start[0];
    pos[1] = start[1];
    pos[2] = start[2];

    c->blocked_node  = -1;
    c->blocked_plane = -1;
    path_reset(c);

    /* 0x80044C74: an invalid hint costs a brute-force sweep, and only then. */
    node = node_hint;
    if (node == -1)
        node = q2_coll_find_node(c, start, -1, true);

    c->current_node = node;

    /* 0x80044CF4: a null move is answered without touching the hull at all,
     * and reports success even from outside the world. */
    if (start[0] == end[0] && start[1] == end[1] && start[2] == end[2]) {
        pos[0] = end[0]; pos[1] = end[1]; pos[2] = end[2];
        goto finish;
    }

    /* 0x80044D70: no cell, no move. The position is left exactly where it
     * started rather than teleported to the destination. */
    if (node < 0)
        goto finish;

    while (node >= 0) {
        const u8 *rec = node_rec(c, (u32)node);
        s32 contents  = (s32)q2_rd_u8(rec + 32);
        s16 delta[3];
        s32 num, den;
        s32 near_node = -1, near_plane = -1;
        s32 next;

        /*
         * 0x80044D9C: record a contact wherever the contents id changes. The
         * list is bounded; when it is full the original stops recording AND
         * stops updating the running id, so it keeps re-testing every cell.
         */
        if (contents != prev_contents && c->path_count < Q2_COLL_PATH_MAX) {
            q2_coll_contact *slot = &c->path[c->path_count++];

            prev_contents = contents;
            slot->id      = (s16)contents;
            slot->pos[0]  = pos[0];
            slot->pos[1]  = pos[1];
            slot->pos[2]  = pos[2];
        }

        c->current_node = node;

        /* 0x80044DF0: the remaining move, in the same 16-bit frame as
         * everything else. A move longer than a signed halfword wraps, exactly
         * as it does on the console. */
        for (i = 0; i < 3; i++)
            delta[i] = (s16)(u16)((u16)((u32)end[i] & 0xFFFFu)
                               -  (u16)((u32)pos[i] & 0xFFFFu));

        q2_coll_clip(c, (u32)node, pos, delta, &num, &den);

        /* 0x80044E40: the initialiser is 1/1 and only a strictly smaller
         * fraction replaces it, so equality is exactly "nothing was hit". */
        if (num == den) {
            pos[0] = end[0]; pos[1] = end[1]; pos[2] = end[2];
            c->blocked_node = -1;       /* 0x80044D48 clears only this one */
            goto finish;
        }

        /* 0x8005652C: scale the delta by the rational fraction. The original
         * multiplies into 64 bits and divides once, truncating toward zero. */
        for (i = 0; i < 3; i++) {
            s64 scaled = (s64)delta[i] * num;
            delta[i] = (s16)(den != 0 ? (s32)(scaled / den) : 0);
            pos[i]  += delta[i];
        }

        next = q2_coll_cross(c, (u32)node, pos, c->hit_plane,
                             &near_node, &near_plane);

        /*
         * 0x80044EAC, delay slot included: the new node is taken
         * unconditionally, and the loop then stops if it turned out to be the
         * one we were already in. Crossing into yourself is how a portal whose
         * neighbour is its own owner terminates.
         */
        if (next == node) {
            c->blocked_node  = near_node;
            c->blocked_plane = near_plane;
            node = -1;
        } else if (next < 0) {
            c->blocked_node  = near_node;
            c->blocked_plane = near_plane;
            node = -1;
        } else {
            node = next;
        }
    }

finish:
    path_terminate(c, pos);

    if (out_pos) {
        out_pos[0] = pos[0];
        out_pos[1] = pos[1];
        out_pos[2] = pos[2];
    }
    if (out_node)
        *out_node = c->current_node;

    /* 0x80044F28: `~node != 0`, i.e. the move completed unless it ran out of
     * cells. A blocked move is the ONLY thing that gives the caller a contact
     * normal, which is why the sense reads backwards at first glance. */
    return node != -1;
}

/* ------------------------------------------------------------------------- */
/* 0x80043E20 — distance along a direction to the nearest plane               */
/* ------------------------------------------------------------------------- */
s32 q2_coll_ray_span(q2_collision *c, u32 node, const s32 point[3],
                     const s16 dir[3], s32 scale)
{
    const u8 *rec;
    s16 rel[3];
    u32 first, end, k;
    s32 best = 0x0FFFFFFF;

    if (!c || !point || !dir || !c->nodes || node >= c->node_count || scale == 0)
        return best << 2;

    c->hit_plane = -1;

    rec = node_rec(c, node);
    relative_rec(rec, point, rel);
    node_plane_range(c, node, &first, &end);

    for (k = first; k < end; k++) {
        const u8 *p = c->planes + (size_t)k * Q2_COLL_PLANE_SIZE;
        s16 nx = q2_rd_s16(p + 6), ny = q2_rd_s16(p + 8), nz = q2_rd_s16(p + 10);
        s32 den, num, t;

        /* Each product is divided BEFORE the sum, which is what keeps this
         * inside 32 bits for a long direction vector. */
        den = ((s32)nx * dir[0]) / scale
            + ((s32)ny * dir[1]) / scale
            + ((s32)nz * dir[2]) / scale;
        if (den <= 0)
            continue;

        num = (s32)(q2_rd_s16(p + 0) - rel[0]) * nx
            + (s32)(q2_rd_s16(p + 2) - rel[1]) * ny
            + (s32)(q2_rd_s16(p + 4) - rel[2]) * nz;

        t = (num * 16) / den;

        if (t >= 0 && t < best) {
            best = t;
            c->hit_plane = (s32)(k - first);
        }
    }

    return best << 2;
}

/* ------------------------------------------------------------------------- */
/* 0x80045088 — locate a point and report what is there                       */
/* ------------------------------------------------------------------------- */
bool q2_coll_probe_point(q2_collision *c, const s32 point[3], s32 hint,
                         q2_coll_point_query *q)
{
    s32 node;

    if (!c || !point || !q)
        return false;

    q->node     = -1;
    q->plane    = -1;
    q->span     = 0;
    q->contents = 0;

    /* 0x800450B4: always with the brute-force fallback enabled. */
    node = q2_coll_find_node(c, point, hint, true);
    if (node < 0)
        return false;

    q->span     = q2_coll_ray_span(c, (u32)node, point, q->dir, q->scale);
    q->contents = (s32)q2_rd_u8(node_rec(c, (u32)node) + 32);
    q->node     = node;
    q->plane    = c->hit_plane;

    return true;
}

s32 q2_coll_normal_len_sq(const q2_coll_plane *p)
{
    if (!p)
        return 0;
    return (s32)p->nx * p->nx + (s32)p->ny * p->ny + (s32)p->nz * p->nz;
}
