/*
 * cmd_coll.c — check the collision model against the disc.
 *
 * The model in src/formats/collision.[ch] was transcribed from SLES_015.34, so
 * the question this answers is not "does the format look plausible" but "does
 * the data satisfy every invariant the transcribed code depends on". Each
 * counter below corresponds to something the console would get wrong — an
 * out-of-range index, a portal that leads nowhere, an arithmetic frame that
 * does not fit — if the reading were mistaken.
 */
#include "cmd_coll.h"

#include "collision.h"
#include "level.h"
#include "trigger.h"
#include "q2psx.h"
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct coll_stats {
    unsigned long long chunks;
    unsigned long long chunks_failed;

    unsigned long long nodes;
    unsigned long long planes;
    unsigned long long links;

    unsigned long long solid_nodes;
    unsigned long long leaf_nodes;      /* no planes at all                   */

    /* Packing limits the link encoding imposes. */
    unsigned long long nodes_over_32_planes;
    unsigned long long hulls_over_2048_nodes;

    /* Link field ranges. */
    unsigned long long link_node_oob;
    unsigned long long link_plane_oob;
    unsigned long long link_back_oob;
    unsigned long long link_self;

    /* Reciprocity: A -> B through plane p must come back B -> A through the
     * back plane the first link named. */
    unsigned long long link_pairs_ok;
    unsigned long long link_pairs_missing;

    /* Arithmetic frame. */
    unsigned long long node_extent_over_16bit;
    unsigned long long plane_point_high_bit;
    unsigned long long max_extent;
    unsigned long long max_plane_point;

    /* Record tails. */
    unsigned long long contents_nonzero;
    unsigned long long contents_max;
    unsigned long long pad_nonzero;
    unsigned long long sort_offset_nonzero;
    unsigned long long sort_offset_negative;
    unsigned long long sort_offset_max;
    unsigned long long first_light_nonzero;
    unsigned long long first_light_max;

    /* Normals. */
    unsigned long long normals_unit;
    unsigned long long normals_bad;

    /* Portal-graph reachability, per hull. */
    unsigned long long hulls;
    unsigned long long hulls_connected;

    /* Live traces. */
    unsigned long long traces;
    unsigned long long traces_completed;
    unsigned long long traces_left_hull;
    unsigned long long traces_moved_backwards;
    unsigned long long traces_wrapped;      /* delta exceeded the 16-bit frame */

    unsigned long long nodes_unreachable;

    /* Trigger volumes, which are collision targets too. */
    unsigned long long volumes;
    unsigned long long volumes_masked;      /* flags & the player's 0x3360 */
    unsigned long long volumes_sink;        /* 0x0200                      */
    unsigned long long volumes_float;       /* 0x2000                      */
    unsigned long long volumes_speedgate;   /* 0x1000                      */
    unsigned long long volumes_sweep;       /* flags & 0x810               */
} coll_stats;

static u32 node_planes(const q2_collision *c, u32 i)
{
    return q2_collision_node_plane_count(c, i);
}

/* Does node `b` carry a link back to `a` through plane `bp`? */
static bool has_back_link(const q2_collision *c, u32 b, u32 a, u32 bp)
{
    q2_coll_node here, next;
    u32 i;

    if (!q2_collision_get_node(c, b, &here) ||
        !q2_collision_get_node(c, b + 1, &next))
        return false;

    for (i = here.first_link; i < next.first_link; i++) {
        q2_coll_link l;

        if (!q2_collision_get_link(c, i, &l))
            break;

        if ((u32)(l.raw & Q2_COLL_LINK_NODE_MASK) != a)
            continue;
        if ((u32)(l.raw >> Q2_COLL_LINK_PLANE_SHIFT) == bp)
            return true;
    }

    return false;
}

/*
 * Breadth-first walk of the portal graph from node 0, using a visited bitmap
 * and the node list itself as the queue. A hull whose nodes are not all
 * reachable is not necessarily wrong — a zone can hold disjoint sealed
 * volumes — but a hull where almost nothing is reachable would mean the link
 * decode is wrong, so the number is worth having.
 */
static bool hull_connected(const q2_collision *c, u8 *seen, u32 *queue,
                           unsigned long long *out_reached)
{
    u32 head = 0, tail = 0, i;
    unsigned long long reached = 0;

    if (c->node_count == 0)
        return true;

    memset(seen, 0, c->node_count);

    queue[tail++] = 0;
    seen[0] = 1;

    while (head < tail) {
        q2_coll_node here, next;
        u32 n = queue[head++];

        reached++;

        if (!q2_collision_get_node(c, n, &here) ||
            !q2_collision_get_node(c, n + 1, &next))
            continue;

        for (i = here.first_link; i < next.first_link; i++) {
            q2_coll_link l;
            u32 nb;

            if (!q2_collision_get_link(c, i, &l))
                break;

            nb = l.raw & Q2_COLL_LINK_NODE_MASK;
            if (nb >= c->node_count || seen[nb])
                continue;

            seen[nb] = 1;
            queue[tail++] = nb;
        }
    }

    if (out_reached)
        *out_reached = reached;

    return reached == c->node_count;
}

/*
 * Exercise the transcribed trace on real geometry: from the centre of a node,
 * fire toward the centre of every eighth other node and check the walk
 * terminates somewhere sane.
 *
 * "Sane" is deliberately weak. A trace SHOULD stop early — that is what a wall
 * is — so the only real failures are leaving the hull entirely (ending in no
 * node) or ending up further from the target than it started, which would mean
 * the fraction arithmetic has the wrong sign.
 */
static void exercise_traces(q2_collision *c, coll_stats *st)
{
    u32 i, step;

    if (c->node_count < 2)
        return;

    step = c->node_count / 16;
    if (step == 0)
        step = 1;

    for (i = 0; i + step < c->node_count; i += step) {
        q2_coll_node a, b;
        s32 start[3], end[3], got[3], node = -1;
        s64 before = 0, after = 0;
        bool ok;
        int k;

        if (!q2_collision_get_node(c, i, &a) ||
            !q2_collision_get_node(c, i + step, &b))
            continue;

        for (k = 0; k < 3; k++) {
            start[k] = a.bbox_min[k] + (a.bbox_max[k] - a.bbox_min[k]) / 2;
            end[k]   = b.bbox_min[k] + (b.bbox_max[k] - b.bbox_min[k]) / 2;
        }

        /* Only start from somewhere the hull agrees we are. A node's box
         * centre can fall outside its own planes when the node is a wedge. */
        if (!q2_coll_point_in_node(c, i, start))
            continue;

        st->traces++;

        ok = q2_coll_move(c, start, end, (s32)i, got, &node);
        if (ok)
            st->traces_completed++;

        if (node < 0)
            st->traces_left_hull++;

        for (k = 0; k < 3; k++) {
            s64 d0 = (s64)end[k] - start[k];
            s64 d1 = (s64)end[k] - got[k];
            before += d0 * d0;
            after  += d1 * d1;
        }

        /*
         * The engine forms the remaining move as an int16 per axis, so a
         * request longer than a signed halfword wraps and the walk sets off in
         * the wrong direction. That is the console's behaviour, not a defect
         * here, so it is counted apart rather than as a failure. Nothing in the
         * game asks for a move that long — a frame's travel is a few hundred
         * units — but a synthetic probe across a whole map does.
         */
        for (k = 0; k < 3; k++) {
            s64 d = (s64)end[k] - start[k];
            if (d > 32767 || d < -32768) {
                st->traces_wrapped++;
                break;
            }
        }

        if (after > before)
            st->traces_moved_backwards++;
    }
}

static void census_hull(q2_collision *c, coll_stats *st, u8 *seen, u32 *queue,
                        const char *label, const char *path, int verbose)
{
    u32 i, k;
    unsigned long long reached = 0;
    bool connected;

    st->hulls++;
    st->nodes  += c->node_count;
    st->planes += c->plane_count;
    st->links  += c->link_count;

    if (c->node_count > Q2_COLL_MAX_NODES) {
        st->hulls_over_2048_nodes++;
        printf("  %s %s: %u nodes exceeds the link encoding's 2048\n",
               path, label, c->node_count);
    }

    for (i = 0; i < c->node_count; i++) {
        q2_coll_node n, next;
        u32 np;
        int axis;

        if (!q2_collision_get_node(c, i, &n) ||
            !q2_collision_get_node(c, i + 1, &next))
            continue;

        if (n.first_plane & Q2_COLL_SOLID)
            st->solid_nodes++;

        np = node_planes(c, i);
        if (np == 0)
            st->leaf_nodes++;
        if (np > Q2_COLL_MAX_PLANES_PER_NODE)
            st->nodes_over_32_planes++;

        if (n.contents != 0) {
            st->contents_nonzero++;
            if (n.contents > st->contents_max)
                st->contents_max = n.contents;
        }
        if (n.pad[0] || n.pad[1] || n.pad[2])
            st->pad_nonzero++;
        if (n.sort_offset != 0) {
            st->sort_offset_nonzero++;
            if (n.sort_offset < 0)
                st->sort_offset_negative++;
            else if ((unsigned long long)n.sort_offset > st->sort_offset_max)
                st->sort_offset_max = (unsigned long long)n.sort_offset;
        }
        if (n.first_light != 0) {
            st->first_light_nonzero++;
            if (n.first_light > st->first_light_max)
                st->first_light_max = n.first_light;
        }

        /* The relative frame is 16-bit, so a node wider than 65535 on any axis
         * would alias. */
        for (axis = 0; axis < 3; axis++) {
            s64 ext = (s64)n.bbox_max[axis] - n.bbox_min[axis];

            if (ext > (s64)st->max_extent)
                st->max_extent = (unsigned long long)ext;
            if (ext > 65535)
                st->node_extent_over_16bit++;
        }

        /* Links. */
        for (k = n.first_link; k < next.first_link; k++) {
            q2_coll_link l;
            u32 nb, pl;

            if (!q2_collision_get_link(c, k, &l))
                break;

            nb = l.raw & Q2_COLL_LINK_NODE_MASK;
            pl = l.raw >> Q2_COLL_LINK_PLANE_SHIFT;

            if (nb >= c->node_count) { st->link_node_oob++; continue; }
            if (nb == i)               st->link_self++;
            if (pl >= np)              st->link_plane_oob++;
            if (l.back_plane >= node_planes(c, nb)) st->link_back_oob++;

            if (has_back_link(c, nb, i, l.back_plane))
                st->link_pairs_ok++;
            else
                st->link_pairs_missing++;
        }
    }

    for (i = 0; i < c->plane_count; i++) {
        q2_coll_plane p;
        s32 len;

        if (!q2_collision_get_plane(c, i, &p))
            break;

        len = q2_coll_normal_len_sq(&p);
        if (len >= 4094 * 4094 && len <= 4097 * 4097)
            st->normals_unit++;
        else
            st->normals_bad++;

        if ((p.a | p.b | p.c) & 0x8000u)
            st->plane_point_high_bit++;

        if (p.a > st->max_plane_point) st->max_plane_point = p.a;
        if (p.b > st->max_plane_point) st->max_plane_point = p.b;
        if (p.c > st->max_plane_point) st->max_plane_point = p.c;
    }

    connected = hull_connected(c, seen, queue, &reached);
    if (connected) {
        st->hulls_connected++;
    } else {
        st->nodes_unreachable += (unsigned long long)c->node_count - reached;
        if (verbose)
            printf("  %s %s: portal graph reaches %llu of %u nodes\n",
                   path, label, reached, c->node_count);
    }

    exercise_traces(c, st);
}

int cmd_coll(disc *d, const char *map, int zone_index)
{
    int i, n = disc_file_count(d);
    coll_stats st;
    u8 *seen = NULL;
    u32 *queue = NULL;
    size_t cap = 0;
    int verbose = (map != NULL);

    memset(&st, 0, sizeof(st));

    printf("Checking the collision model against every zone on the disc...\n\n");

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *base = strrchr(f->path, '/');
        q2_buf buf;
        q2_zone_file zf;
        int which;

        base = base ? base + 1 : f->path;

        /*
         * The map's trigger volumes are collision targets as much as the hulls
         * are: 0x80053C58 sweeps against them after the entity table, and
         * 0x80050CE0 queries their flags for the entity's liquid state.
         */
        if (strncmp(base, "COMMON", 6) == 0) {
            q2_common_file cf;
            q2_triggers tr;

            if (disc_read_file(d, f->path, &buf) != Q2_OK)
                continue;
            if (q2_common_open(&cf, &buf) != Q2_OK) {
                q2_buf_free(&buf);
                continue;
            }
            if (q2_triggers_parse(&tr, &cf) == Q2_OK) {
                u32 k;
                for (k = 0; k < tr.count; k++) {
                    q2_trigger t;
                    if (!q2_trigger_get(&tr, k, &t))
                        continue;
                    st.volumes++;
                    if (t.flags & Q2_CONTENTS_MASK) st.volumes_masked++;
                    if (t.flags & 0x0200u)          st.volumes_sink++;
                    if (t.flags & 0x2000u)          st.volumes_float++;
                    if (t.flags & 0x1000u)          st.volumes_speedgate++;
                    if (t.flags & 0x0810u)          st.volumes_sweep++;
                }
            }
            q2_common_close(&cf);
            continue;
        }

        if (strncmp(base, "ZONE", 4) != 0)
            continue;

        if (map) {
            /* Path looks like /Q2DATA/<MAP>/ZONE<n>.DAT */
            if (!strstr(f->path, map))
                continue;
            if (zone_index >= 0) {
                char want[32];
                snprintf(want, sizeof(want), "ZONE%d.DAT", zone_index);
                if (strcmp(base, want) != 0)
                    continue;
            }
        }

        if (disc_read_file(d, f->path, &buf) != Q2_OK)
            continue;

        if (q2_zone_open(&zf, &buf) != Q2_OK) {
            q2_buf_free(&buf);
            continue;
        }

        for (which = 0; which < 2; which++) {
            q2_collision c;

            st.chunks++;

            if (q2_collision_parse(&c, &zf,
                                   which == 0 ? Q2_COLL_PRIMARY
                                              : Q2_COLL_SECONDARY) != Q2_OK) {
                st.chunks_failed++;
                printf("  PARSE FAILED  %s %s\n", f->path,
                       which == 0 ? "PrimaryColl" : "SecondaryCol");
                continue;
            }

            if (c.node_count + 1 > cap) {
                void *p1, *p2;
                cap = (size_t)c.node_count + 16;
                p1 = realloc(seen, cap);
                p2 = realloc(queue, cap * sizeof(u32));
                if (!p1 || !p2) {
                    free(p1 ? p1 : seen);
                    free(p2 ? p2 : queue);
                    q2_buf_free(&buf);
                    printf("out of memory\n");
                    return 1;
                }
                seen  = (u8 *)p1;
                queue = (u32 *)p2;
            }

            census_hull(&c, &st, seen, queue,
                        which == 0 ? "PrimaryColl" : "SecondaryCol",
                        f->path, verbose);
        }

        q2_buf_free(&buf);
    }

    free(seen);
    free(queue);

    printf("Chunks\n");
    printf("  parsed              : %llu\n", st.chunks - st.chunks_failed);
    printf("  failed              : %llu\n", st.chunks_failed);
    printf("  nodes / planes / links : %llu / %llu / %llu\n",
           st.nodes, st.planes, st.links);
    printf("\n");

    printf("Node fields\n");
    printf("  solid (first_plane bit 15) : %llu\n", st.solid_nodes);
    printf("  with no planes at all      : %llu\n", st.leaf_nodes);
    printf("  contents != 0              : %llu  (max %llu)\n",
           st.contents_nonzero, st.contents_max);
    printf("  bytes +33..35 non-zero     : %llu\n", st.pad_nonzero);
    printf("  SortData offset (+28) != 0 : %llu  (max %llu, %llu negative)\n",
           st.sort_offset_nonzero, st.sort_offset_max,
           st.sort_offset_negative);
    printf("  firstLight (+30) != 0      : %llu  (max %llu)\n",
           st.first_light_nonzero, st.first_light_max);
    printf("\n");

    printf("The link encoding's own limits\n");
    printf("  nodes with > 32 planes     : %llu   (5-bit plane field)\n",
           st.nodes_over_32_planes);
    printf("  hulls with > 2048 nodes    : %llu   (11-bit node field)\n",
           st.hulls_over_2048_nodes);
    printf("  link node index out of range  : %llu\n", st.link_node_oob);
    printf("  link plane index out of range : %llu\n", st.link_plane_oob);
    printf("  link back plane out of range  : %llu\n", st.link_back_oob);
    printf("  links pointing at their owner : %llu\n", st.link_self);
    printf("  reciprocal portal pairs       : %llu ok, %llu missing\n",
           st.link_pairs_ok, st.link_pairs_missing);
    printf("\n");

    printf("The 16-bit relative frame\n");
    printf("  node axis extents > 65535  : %llu   (max %llu)\n",
           st.node_extent_over_16bit, st.max_extent);
    printf("  plane points with bit 15   : %llu   (max %llu)\n",
           st.plane_point_high_bit, st.max_plane_point);
    printf("\n");

    printf("Plane normals\n");
    printf("  unit (4094..4097)          : %llu\n", st.normals_unit);
    printf("  not unit                   : %llu\n", st.normals_bad);
    printf("\n");

    printf("Portal graph\n");
    printf("  hulls fully connected      : %llu of %llu\n",
           st.hulls_connected, st.hulls);
    printf("  nodes no portal walk reaches : %llu of %llu\n",
           st.nodes_unreachable, st.nodes);
    printf("\n");

    printf("Trigger volumes, as collision targets\n");
    printf("  volumes                    : %llu\n", st.volumes);
    printf("  matching the contents mask : %llu   (0x3360)\n", st.volumes_masked);
    printf("    sink slowly   (0x0200)   : %llu\n", st.volumes_sink);
    printf("    buoyant       (0x2000)   : %llu\n", st.volumes_float);
    printf("    speed gate    (0x1000)   : %llu\n", st.volumes_speedgate);
    printf("  swept against (0x0810)     : %llu\n", st.volumes_sweep);
    printf("\n");

    printf("Live traces through the transcribed walker\n");
    printf("  attempted                  : %llu\n", st.traces);
    printf("  reached the destination    : %llu\n", st.traces_completed);
    printf("  ended outside every node   : %llu\n", st.traces_left_hull);
    printf("  asked for a move > 16 bits : %llu   (wraps on the console too)\n",
           st.traces_wrapped);
    printf("  ended further away         : %llu\n", st.traces_moved_backwards);

    return (st.chunks_failed || st.link_node_oob || st.link_plane_oob ||
            st.link_back_oob || st.nodes_over_32_planes ||
            st.hulls_over_2048_nodes || st.node_extent_over_16bit ||
            st.plane_point_high_bit || st.normals_bad ||
            st.traces_left_hull ||
            st.traces_moved_backwards > st.traces_wrapped) ? 1 : 0;
}
