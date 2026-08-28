/*
 * test_coll.c — the collision model transcribed from SLES_015.34.
 *
 * The disc-wide check lives in `q2psx-inspect coll`; this pins the behaviours
 * that no census can see because they need geometry chosen to provoke them:
 * the 16-bit relative frame, the solid bit, the exact rational clip fraction,
 * the portal crossing and its 256-unit slack, and the lift-slide-drop sequence
 * that decides whether the player is standing on anything.
 *
 * The hull below is built by hand in the on-disc layout and fed through the
 * real parser, so the record packing is under test too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "collision.h"
#include "trace.h"
#include "worldscale.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what)
{
    g_checks++;
    if (!condition) {
        printf("  FAIL  %s\n", what);
        g_failures++;
    }
}

static void check_eq_i(s64 got, s64 want, const char *what)
{
    g_checks++;
    if (got != want) {
        printf("  FAIL  %s: got %lld, want %lld\n",
               what, (long long)got, (long long)want);
        g_failures++;
    }
}

/* ------------------------------------------------------------------------- */
/* A hand-built hull                                                          */
/* ------------------------------------------------------------------------- */
/*
 * Two boxes side by side on the X axis, joined by a portal in the plane they
 * share, plus a third box marked solid.
 *
 *      X:  0 ....... 1000 ....... 2000        3000 ..... 4000
 *          |  node 0  |   node 1   |          |  node 2   |
 *                     ^ portal                 (solid)
 *
 * Y and Z run 0..1000 in every node. +Y is down, so node planes with ny = +4096
 * are floors.
 */
#define NODES  3
#define PLANES (NODES * 6)
#define LINKS  2

static u8 g_chunk[4 + (NODES + 1) * Q2_COLL_NODE_SIZE
                    + PLANES * Q2_COLL_PLANE_SIZE
                    + LINKS * Q2_COLL_LINK_SIZE];

static void wr16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

/* One axis-aligned box: six planes, in the order -X +X -Y +Y -Z +Z, each
 * stored as a point relative to the node's own minimum corner. */
static void write_box_planes(u8 *p, s32 sx, s32 sy, s32 sz)
{
    static const s16 n[6][3] = {
        {-4096, 0, 0}, {4096, 0, 0},
        {0, -4096, 0}, {0, 4096, 0},
        {0, 0, -4096}, {0, 0, 4096}
    };
    s32 pt[6][3];
    int i, k;

    memset(pt, 0, sizeof(pt));
    pt[1][0] = sx;      /* +X face sits at the far corner */
    pt[3][1] = sy;
    pt[5][2] = sz;

    for (i = 0; i < 6; i++) {
        for (k = 0; k < 3; k++)
            wr16(p + i * 12 + k * 2, (u32)pt[i][k]);
        for (k = 0; k < 3; k++)
            wr16(p + i * 12 + 6 + k * 2, (u32)(u16)n[i][k]);
    }
}

static void build_hull(bool mark_solid)
{
    static const s32 box[NODES][6] = {
        {    0, 0, 0, 1000, 1000, 1000 },
        { 1000, 0, 0, 2000, 1000, 1000 },
        { 3000, 0, 0, 4000, 1000, 1000 }
    };
    u8 *nodes, *planes, *links;
    int i, k;

    memset(g_chunk, 0, sizeof(g_chunk));

    wr16(g_chunk + 0, NODES);
    wr16(g_chunk + 2, PLANES);

    nodes  = g_chunk + 4;
    planes = nodes + (NODES + 1) * Q2_COLL_NODE_SIZE;
    links  = planes + PLANES * Q2_COLL_PLANE_SIZE;

    for (i = 0; i < NODES; i++) {
        u8 *rec = nodes + i * Q2_COLL_NODE_SIZE;
        u32 first = (u32)(i * 6);

        for (k = 0; k < 3; k++) {
            wr32(rec + 0  + k * 4, (u32)box[i][k]);
            wr32(rec + 12 + k * 4, (u32)box[i][3 + k]);
        }

        if (mark_solid && i == 2)
            first |= Q2_COLL_SOLID;

        wr16(rec + 24, first);
        wr16(rec + 26, (u32)(i == 0 ? 0 : (i == 1 ? 1 : 2)));
        wr16(rec + 28, (u32)(100 + i * 7)); /* PrimaryColl SortData offset */
        wr16(rec + 30, (u32)(20 + i * 3));  /* SecondaryCol light start   */
        rec[32] = (u8)(i == 1 ? 7 : 0);     /* node 1 carries a contents id */

        write_box_planes(planes + i * 6 * Q2_COLL_PLANE_SIZE,
                         box[i][3] - box[i][0],
                         box[i][4] - box[i][1],
                         box[i][5] - box[i][2]);
    }

    /* The sentinel. */
    {
        u8 *rec = nodes + NODES * Q2_COLL_NODE_SIZE;
        wr16(rec + 24, PLANES);
        wr16(rec + 26, LINKS);
    }

    /*
     * node 0's +X plane is index 1 within the node; node 1's -X plane is
     * index 0. So node 0 links out through plane 1 to node 1, coming back
     * through node 1's plane 0, and the reverse.
     */
    wr16(links + 0, (1u << Q2_COLL_LINK_PLANE_SHIFT) | 1u);
    wr16(links + 2, 0);
    wr16(links + 4, (0u << Q2_COLL_LINK_PLANE_SHIFT) | 0u);
    wr16(links + 6, 1);
}

static bool open_hull(q2_collision *out, bool mark_solid)
{
    dat_chunk chunk;
    q2_zone_file zf;

    build_hull(mark_solid);

    memset(&chunk, 0, sizeof(chunk));
    chunk.data = g_chunk;
    chunk.size = (u32)sizeof(g_chunk);

    memset(&zf, 0, sizeof(zf));
    zf.chunk[Q2_ZONE_SECONDARY_COL] = &chunk;

    return q2_collision_parse(out, &zf, Q2_COLL_SECONDARY) == Q2_OK;
}

/* ------------------------------------------------------------------------- */
static void test_parse(void)
{
    q2_collision c;
    q2_coll_node n;
    q2_coll_link l;

    printf("chunk layout\n");

    check(open_hull(&c, false), "the hand-built chunk parses");
    check_eq_i(c.node_count, NODES, "node count");
    check_eq_i(c.plane_count, PLANES, "plane count");
    check_eq_i(c.link_count, LINKS, "link count");

    check(q2_collision_get_node(&c, 1, &n), "node 1 reads back");
    check_eq_i(n.bbox_min[0], 1000, "node 1 min x");
    check_eq_i(n.sort_offset, 107, "SortData offset is halfword +28");
    check_eq_i(n.first_light, 23, "SpaceLights offset is halfword +30");
    check_eq_i(n.contents, 7, "the contents id is byte +32");
    check_eq_i(q2_collision_node_plane_count(&c, 1), 6, "planes per node");
    check_eq_i(q2_collision_node_link_count(&c, 1), 1, "links per node");

    check(q2_collision_get_link(&c, 0, &l), "link 0 reads back");
    check_eq_i(l.raw & Q2_COLL_LINK_NODE_MASK, 1, "link 0 names node 1");
    check_eq_i(l.raw >> Q2_COLL_LINK_PLANE_SHIFT, 1,
               "link 0 sits in plane 1 of its owner");
    check_eq_i(l.back_plane, 0, "link 0's matching plane in the neighbour");
}

/* ------------------------------------------------------------------------- */
static void test_point_in_node(void)
{
    q2_collision c;
    s32 inside[3]  = { 500, 500, 500 };
    s32 outside[3] = { 1500, 500, 500 };
    s32 corner[3]  = { 0, 0, 0 };
    s32 face[3]    = { 1000, 500, 500 };

    printf("point in node\n");
    open_hull(&c, false);

    check(q2_coll_point_in_node(&c, 0, inside), "the centre of node 0 is inside it");
    check(!q2_coll_point_in_node(&c, 0, outside), "node 1's centre is not in node 0");
    check(q2_coll_point_in_node(&c, 1, outside), "node 1's centre is in node 1");

    /* A point exactly on a plane is INSIDE: the test rejects only d > 0. */
    check(q2_coll_point_in_node(&c, 0, corner), "the minimum corner counts as inside");
    check(q2_coll_point_in_node(&c, 0, face), "a point on the shared face is inside");

    check_eq_i(q2_coll_find_node(&c, inside, -1, true), 0, "find_node locates node 0");
    check_eq_i(q2_coll_find_node(&c, outside, -1, true), 1, "find_node locates node 1");

    /* From a hint, the neighbour is found through the link list without any
     * brute-force sweep — that is the whole point of the portal list. */
    check_eq_i(q2_coll_find_node(&c, outside, 0, false), 1,
               "find_node reaches the neighbour from a hint, no sweep");

    {
        s32 nowhere[3] = { 2500, 500, 500 };    /* the gap between 1 and 2 */
        check_eq_i(q2_coll_find_node(&c, nowhere, -1, true), -1,
                   "a point in no node reports -1");
    }
}

/* ------------------------------------------------------------------------- */
static void test_solid_bit(void)
{
    q2_collision c;
    s32 p[3] = { 3500, 500, 500 };

    printf("the solid bit\n");

    open_hull(&c, false);
    check(q2_coll_point_in_node(&c, 2, p), "node 2 holds the point when open");

    open_hull(&c, true);
    check(q2_collision_node_is_solid(&c, 2), "bit 15 of first_plane reads as solid");
    check(!q2_coll_point_in_node(&c, 2, p),
          "a solid node holds nothing, whatever its planes say");
    check_eq_i(q2_collision_node_plane_count(&c, 2), 6,
               "the plane count still masks the solid bit off");
}

/* ------------------------------------------------------------------------- */
static void test_clip(void)
{
    q2_collision c;
    s32 p[3] = { 500, 500, 500 };
    s16 d[3];
    s32 num, den;

    printf("clipping a move against one node\n");
    open_hull(&c, false);

    /* Straight at the +X face, 500 units away, asking for 2000: the fraction
     * must come back as exactly one quarter, as an unreduced rational. */
    d[0] = 2000; d[1] = 0; d[2] = 0;
    q2_coll_clip(&c, 0, p, d, &num, &den);
    check(num * 4 == den, "a move four times the gap clips at one quarter");
    check_eq_i(c.hit_plane, 1, "the +X plane is plane 1 of the node");

    /* Short of the face: nothing is hit, and the original signals that by
     * leaving the fraction at its 1/1 initialiser. */
    d[0] = 100;
    q2_coll_clip(&c, 0, p, d, &num, &den);
    check_eq_i(num, den, "a move that fits leaves num == den");
    check_eq_i(c.hit_plane, -1, "and reports no plane");

    /* Moving away from a plane never clips against it. */
    d[0] = -2000;
    q2_coll_clip(&c, 0, p, d, &num, &den);
    check(num * 4 == den, "moving the other way clips on the -X plane instead");
    check_eq_i(c.hit_plane, 0, "which is plane 0");

    /* A move parallel to every plane hits nothing at all. */
    d[0] = 0; d[1] = 0; d[2] = 0;
    q2_coll_clip(&c, 0, p, d, &num, &den);
    check_eq_i(num, den, "a zero move clips nothing");
}

/* ------------------------------------------------------------------------- */
static void test_move_through_portal(void)
{
    q2_collision c;
    s32 start[3] = { 500, 500, 500 };
    s32 end[3]   = { 1500, 500, 500 };
    s32 got[3];
    s32 node = -1;

    printf("moving through a portal\n");
    open_hull(&c, false);

    check(q2_coll_move(&c, start, end, 0, got, &node),
          "a move into the neighbour completes");
    check_eq_i(got[0], 1500, "and arrives exactly at the destination");
    check_eq_i(node, 1, "ending in the neighbouring node");

    /* Into the wall at the far end of node 1: stopped, and the cached cell is
     * still node 1 rather than -1. */
    end[0] = 5000;
    check(!q2_coll_move(&c, start, end, 0, got, &node),
          "a move into a wall is reported as blocked");
    check(got[0] < 2001 && got[0] >= 1999,
          "and stops at the wall");
    check(c.hit_plane_index >= 0, "with the plane that stopped it recorded");

    {
        q2_coll_plane pl;
        check(q2_collision_get_plane(&c, (u32)c.hit_plane_index, &pl),
              "the recorded plane reads back");
        check_eq_i(pl.nx, 4096, "and it is the +X wall");
    }

    /* The contents transition along the path is recorded once, where it
     * changes: node 0 carries 0 and node 1 carries 7. */
    end[0] = 1500;
    q2_coll_move(&c, start, end, 0, got, &node);
    check(c.path_count >= 2, "two contents regions are recorded");
    check_eq_i(c.path[0].id, 0, "the first is node 0's");
    check_eq_i(c.path[1].id, 7, "the second is node 1's");

    /* A null move is answered without touching the hull. */
    check(q2_coll_move(&c, start, start, 0, got, &node),
          "a zero-length move completes");
    check_eq_i(got[0], start[0], "and does not move");

    /* An unknown starting cell costs a sweep and still works. */
    check(q2_coll_move(&c, start, end, -1, got, &node),
          "a move with no hint finds its own starting cell");
    check_eq_i(node, 1, "and still arrives");

    /* Starting outside every cell moves nothing at all — the position stays
     * exactly where it was rather than teleporting to the destination. */
    {
        s32 nowhere[3] = { 2500, 500, 500 };
        check(!q2_coll_move(&c, nowhere, end, -1, got, &node),
              "a move from outside the hull is blocked");
        check_eq_i(got[0], 2500, "and leaves the position untouched");
    }
}

/* ------------------------------------------------------------------------- */
static void test_16bit_frame(void)
{
    q2_collision c;
    q2_coll_node n;
    s32 p[3];
    s16 rel[3];

    printf("the 16-bit relative frame\n");
    open_hull(&c, false);
    q2_collision_get_node(&c, 1, &n);

    p[0] = 1500; p[1] = 500; p[2] = 500;
    q2_coll_relative(&n, p, rel);
    check_eq_i(rel[0], 500, "a point inside gives its true offset");

    /* The subtraction is modulo 65536 and read back signed, so a point far
     * enough away wraps. The AABB gate is what keeps that from mattering
     * in the real hull, and this is the demonstration that it has to. */
    p[0] = 1000 + 40000;
    q2_coll_relative(&n, p, rel);
    check_eq_i(rel[0], 40000 - 65536, "and one 40,000 units away wraps negative");
}

/* ------------------------------------------------------------------------- */
static void test_push_vector(void)
{
    s16 n[3], out[3];

    printf("the unstick nudge\n");

    n[0] = 4096; n[1] = 0; n[2] = 0;
    q2_move_push_vector(n, out);
    check(out[0] == 1 && out[1] == 0 && out[2] == 0, "+X normal nudges +X");

    n[0] = -4096;
    q2_move_push_vector(n, out);
    check(out[0] == -1 && out[1] == 0 && out[2] == 0, "-X normal nudges -X");

    n[0] = 0; n[1] = 4096;
    q2_move_push_vector(n, out);
    check(out[1] == 1 && out[0] == 0 && out[2] == 0, "a floor nudges +Y");

    /* The comparisons are strict, so an exact tie keeps the earlier axis. */
    n[0] = 2896; n[1] = 2896; n[2] = 0;
    q2_move_push_vector(n, out);
    check(out[0] == 1 && out[1] == 0, "a 45-degree normal keeps X, not a split");
}

/* ------------------------------------------------------------------------- */
/*
 * The two contact slots, and which extreme each of them keeps.
 *
 * `ground_normal` keeps the largest SIGNED ny — the flattest floor of the frame.
 * `last_normal` keeps the smallest |ny| — the most WALL-LIKE contact. They are
 * different comparisons on purpose (0x80045318 vs 0x800453CC), and the second
 * one is the port's only wall detector: the ladder flag, the velocity clip and
 * the water-exit jump all read it.
 *
 * It is reset to (0, 4096, 0) once per tick, and 4096 is the largest |ny| a
 * 1.3.12 unit normal can hold, so the sentinel is chosen to LOSE to the frame's
 * first real contact. Keeping the largest |ny| instead — which this port did —
 * makes the predicate unsatisfiable against its own sentinel and freezes the
 * field on the flat floor that was never touched. Every one of its readers then
 * silently does nothing, for the whole session.
 */
static void test_contact_slots(void)
{
    q2_collision c;
    q2_move_ent  ent;
    s16 delta[3];

    printf("the two contact slots\n");
    open_hull(&c, false);

    memset(&ent, 0, sizeof(ent));
    ent.pos[0] = 1500; ent.pos[1] = 500; ent.pos[2] = 500;
    ent.node = 1;
    ent.max_slope_ny = Q2_MAX_SLOPE_NY;

    /* The per-tick reset the mover's caller applies (0x80045AC8). */
    ent.last_normal[0] = 0; ent.last_normal[1] = 4096; ent.last_normal[2] = 0;

    /* Straight into node 1's +X face at x = 2000, which is a vertical wall. */
    delta[0] = 900; delta[1] = 0; delta[2] = 0;
    q2_move_checked(&c, &ent, delta, 0, false, false, NULL, NULL, NULL);

    check_eq_i(ent.last_normal[0], 4096,
               "a wall contact replaces the flat sentinel in last_normal");
    check_eq_i(ent.last_normal[1], 0,
               "and its ny is zero, which is what makes it read as a wall");

    /*
     * And the sentinel must not be replaced by something FLATTER than itself.
     * A second contact with a larger |ny| loses, so the frame's answer is the
     * most wall-like surface it touched and not the last one it happened to
     * touch.
     */
    ent.last_normal[0] = 0; ent.last_normal[1] = 4096; ent.last_normal[2] = 0;
    ent.pos[0] = 1500; ent.pos[1] = 500; ent.pos[2] = 500;
    ent.node = 1;

    delta[0] = 0; delta[1] = 900; delta[2] = 0;   /* +Y is down: into the floor */
    q2_move_checked(&c, &ent, delta, 0, false, false, NULL, NULL, NULL);

    check_eq_i(ent.last_normal[1], 4096,
               "a floor as flat as the sentinel does not displace it");
}

/* ------------------------------------------------------------------------- */
static void test_step_move(void)
{
    q2_collision c;
    q2_move_ent ent;
    s16 delta[3];

    printf("the stepped frame move\n");
    open_hull(&c, false);

    check_eq_i(q2_move_step_height(0), Q2_STEP_HEIGHT, "the default step height");
    check_eq_i(q2_move_step_height(Q2_ENT_LOW_STEP), Q2_STEP_HEIGHT_LOW,
               "and the halved one when 0x600 is set");

    /*
     * The step sequence only runs for an entity that is ALREADY STANDING on
     * something and is not submerged — 0x80045ADC tests `flags & 0x60` and
     * `flags & 0x100` and branches to a plain slide at 0x80045CA4 otherwise.
     * So every case below has to say which of the two it is exercising, and an
     * entity fresh out of memset takes the AIRBORNE path.
     */
    memset(&ent, 0, sizeof(ent));
    ent.pos[0] = 500; ent.pos[1] = 500; ent.pos[2] = 500;
    ent.node = 0;
    ent.max_slope_ny = 2048;

    delta[0] = 300; delta[1] = 0; delta[2] = 0;
    q2_move_step(&c, &ent, delta, NULL);

    check_eq_i(ent.pos[0], 800, "an airborne slide moves the full distance");
    check_eq_i(ent.pos[1], 500,
               "and does NOT step down — that would cancel a jump");
    check(!(ent.flags & Q2_ENT_ON_GROUND),
          "with nothing under it, nothing is touched");

    /*
     * Grounded, and NOTHING within a step below — which is the case the step
     * sequence is wrong for, and which 0x80045C68 exists to undo.
     *
     * This used to assert a net step down of one whole step height per frame,
     * on the reasoning that "one frame from the middle of a tall cell does not
     * reach the floor". That was the port keeping whatever the drop produced.
     * The original checks the ground flags at 0x80045C18 and, finding them
     * clear with `ground_normal[1]` still zero — the drop touched nothing at
     * all — rewinds to the post-slide position and falls by the LIFT amount
     * alone (s4 at 0x80045C7C, not `drop + step`).
     *
     * So the lift is undone and no more, and the frame's net vertical effect
     * over open space is ZERO. Anything below that is gravity's to supply.
     * With the old behaviour, walking in mid-air teleported the entity 216
     * units downward every tick, which is what made every kerb and stair edge
     * snap.
     */
    memset(&ent, 0, sizeof(ent));
    ent.pos[0] = 500; ent.pos[1] = 500; ent.pos[2] = 500;
    ent.node = 0;
    ent.max_slope_ny = 2048;
    ent.flags = Q2_ENT_ON_GROUND;

    delta[0] = 300; delta[1] = 0; delta[2] = 0;
    q2_move_step(&c, &ent, delta, NULL);

    check_eq_i(ent.pos[0], 800, "walking moves the full distance when nothing is in the way");
    check_eq_i(ent.pos[1], 500,
               "and over open space the frame's net vertical effect is nothing");
    check(!(ent.flags & Q2_ENT_ON_GROUND),
          "with nothing under it, the drop finds no ground");

    /* Close enough that the drop reaches the floor at y = 1000. */
    memset(&ent, 0, sizeof(ent));
    ent.pos[0] = 500; ent.pos[1] = 900; ent.pos[2] = 500;
    ent.node = 0;
    ent.max_slope_ny = 2048;
    ent.flags = Q2_ENT_ON_GROUND;

    delta[0] = 300; delta[1] = 0; delta[2] = 0;
    q2_move_step(&c, &ent, delta, NULL);

    check(ent.flags & Q2_ENT_ON_GROUND, "within a step of the floor, the drop lands");
    check_eq_i(ent.pos[1], 1000, "resting exactly on it");
    check_eq_i(ent.ground_normal[1], 4096, "with the floor's normal recorded");

    /*
     * And the airborne path lands too — it is the only way an entity ever
     * BECOMES grounded, because the step sequence needs it to be already. Its
     * slide carries ground_mode (table 0x800AE93C = {1,1}), so a fall that
     * reaches the floor declares ground on the way through.
     */
    memset(&ent, 0, sizeof(ent));
    ent.pos[0] = 500; ent.pos[1] = 900; ent.pos[2] = 500;
    ent.node = 0;
    ent.max_slope_ny = 2048;

    delta[0] = 0; delta[1] = 300; delta[2] = 0;
    q2_move_step(&c, &ent, delta, NULL);

    check(ent.flags & Q2_ENT_ON_GROUND, "a falling entity lands on the floor");

    /*
     * A couple of units short of the plane, not exactly on it: the airborne
     * slide runs with push_mode set (table 0x800AE93C is {1,1}), so the unstick
     * nudge at 0x8005625C backs the entity off the surface it just hit. The
     * step sequence's drop has push_mode clear and does land exactly, which is
     * why the following tick puts it on 1000 — see test_sim's hull test.
     */
    check(ent.pos[1] > 1000 - 8 && ent.pos[1] <= 1000,
          "just short of the plane, because the airborne slide nudges off it");

    /* Into the far wall of node 1: stopped, but still standing. */
    memset(&ent, 0, sizeof(ent));
    ent.pos[0] = 1900; ent.pos[1] = 900; ent.pos[2] = 500;
    ent.node = 1;
    ent.max_slope_ny = 2048;
    ent.flags = Q2_ENT_ON_GROUND;

    delta[0] = 300; delta[1] = 0; delta[2] = 0;
    q2_move_step(&c, &ent, delta, NULL);

    check(ent.pos[0] <= 2000, "a wall stops the move");
    check(ent.pos[0] > 1900, "but does not undo it");
    check(ent.flags & Q2_ENT_ON_GROUND, "and the entity is still on the ground");

    /*
     * Ground is decided by the DOWN move alone. Walking straight into a wall
     * from mid-air must NOT read as standing on it: the slide is passed
     * ground_mode = 0 (table entry 0x800AE934).
     */
    {
        q2_move_mode mode;

        memset(&ent, 0, sizeof(ent));
        ent.pos[0] = 1900; ent.pos[1] = 500; ent.pos[2] = 500;
        ent.node = 1;
        ent.max_slope_ny = 2048;

        mode.ground_mode = false;
        mode.push_mode   = true;
        mode.iterations  = 3;
        mode.sweep_ents  = false;
        mode.world       = NULL;

        delta[0] = 300; delta[1] = 0; delta[2] = 0;
        q2_move(&c, &ent, delta, &mode);

        check(!(ent.flags & Q2_ENT_ON_GROUND),
              "the slide move never sets the on-ground flag");
    }
}

/* ------------------------------------------------------------------------- */
static void test_box_sweep(void)
{
    s32 pos[3]  = { 0, 0, 0 };
    s16 delta[3];
    s32 bmin[3] = { -500, 1000, -500 };
    s32 bmax[3] = {  500, 2000,  500 };
    s32 got[3];
    s16 n[3];

    printf("the entity box sweep\n");

    /* Falling straight down onto a box 1000 below, with a 286 half-extent:
     * contact is at the inflated top face, 1000 - 286 = 714. */
    delta[0] = 0; delta[1] = 1000; delta[2] = 0;
    check(q2_move_sweep_box(pos, delta, bmin, bmax, 0,
                            Q2_SWEEP_HALF_EXTENT, got, n),
          "a fall onto a box below reports contact");
    check_eq_i(got[1], 1000 - Q2_SWEEP_HALF_EXTENT,
               "at the box's top face grown by the half-extent");
    check(n[1] == 4096, "with the floor normal");

    /* The same fall while the box itself rises to meet it: subtracting the
     * platform's own motion is what lets an entity ride a lift. */
    check(q2_move_sweep_box(pos, delta, bmin, bmax, 400,
                            Q2_SWEEP_HALF_EXTENT, got, n),
          "a rising platform still reports contact");

    /* Moving away from it hits nothing. */
    delta[1] = -1000;
    check(!q2_move_sweep_box(pos, delta, bmin, bmax, 0,
                             Q2_SWEEP_HALF_EXTENT, got, n),
          "moving away from the box misses it");

    /* Sideways past it, far enough out that the inflated box cannot reach. */
    {
        s32 side[3] = { 5000, 0, 0 };
        delta[0] = 100; delta[1] = 0; delta[2] = 0;
        check(!q2_move_sweep_box(side, delta, bmin, bmax, 0,
                                 Q2_SWEEP_HALF_EXTENT, got, n),
              "a move well clear of the box misses it");
    }

    /* The caller-side AABB gate. */
    {
        s32 mmin[3] = { -100, -100, -100 }, mmax[3] = { 100, 100, 100 };
        check(!q2_move_box_overlap(mmin, mmax, bmin, bmax),
              "a move nowhere near the box fails the gate");
        mmax[1] = 1500;
        check(q2_move_box_overlap(mmin, mmax, bmin, bmax),
              "and passes it once they overlap");
    }
}

/* ------------------------------------------------------------------------- */
static void test_world_sweep(void)
{
    q2_move_target targets[3];
    q2_move_world w;
    q2_move_contact hit;
    s32 pos[3]  = { 0, 0, 0 };
    s32 dest[3] = { 0, 2000, 0 };

    printf("sweeping a move against every target\n");

    memset(targets, 0, sizeof(targets));

    /* An inactive slot: the 48-entry table's byte +54 is zero. */
    targets[0].min[0] = -500; targets[0].min[1] = 500;  targets[0].min[2] = -500;
    targets[0].max[0] =  500; targets[0].max[1] = 1000; targets[0].max[2] =  500;
    targets[0].kind   = Q2_MOVE_KIND_ENTITY;
    targets[0].id     = 7;
    targets[0].active = false;

    /* A live one, further down. */
    targets[1] = targets[0];
    targets[1].min[1] = 1500; targets[1].max[1] = 2500;
    targets[1].id     = 9;
    targets[1].active = true;

    /* A volume whose flags the mover does not carry. */
    targets[2] = targets[1];
    targets[2].min[1] = 1000; targets[2].max[1] = 1200;
    targets[2].kind   = Q2_MOVE_KIND_VOLUME;
    targets[2].mask   = 0x0800;
    targets[2].id     = 11;

    memset(&w, 0, sizeof(w));
    w.targets     = targets;
    w.count       = 3;
    w.half_extent = Q2_SWEEP_HALF_EXTENT;
    w.mask        = 0;      /* carries nothing, so the volume is skipped */

    check(!q2_move_sweep_world(&w, pos, dest, &hit),
          "the fall is stopped by the live target");
    check_eq_i(hit.id, 9, "and it is the live one, not the inactive slot");
    check_eq_i(hit.pos[1], 1500 - Q2_SWEEP_HALF_EXTENT,
               "stopping at its inflated top face");
    check_eq_i(hit.kind, Q2_MOVE_KIND_ENTITY, "reported as an entity");

    /*
     * Give the mover the volume's flags and it is considered too. The sweep
     * takes the LAST contact, not the nearest — but because each contact moves
     * the destination closer, a later target can only win by being nearer
     * still, which is what happens here.
     */
    w.mask = 0x0810;
    check(!q2_move_sweep_world(&w, pos, dest, &hit),
          "with the flags carried, the volume is considered");
    check_eq_i(hit.id, 11, "and the nearer volume, tested last, wins");
    check_eq_i(hit.kind, Q2_MOVE_KIND_VOLUME, "reported as a volume");
    check_eq_i(hit.pos[1], 1000 - Q2_SWEEP_HALF_EXTENT,
               "stopping at the volume's inflated top face");

    /* Nothing in the way at all. */
    dest[1] = 100;
    check(q2_move_sweep_world(&w, pos, dest, &hit),
          "a short move reaches its destination");
    check_eq_i(hit.pos[1], 100, "untouched");
    check(!hit.hit, "with no contact recorded");
}

/* ------------------------------------------------------------------------- */
/*
 * The second pass every non-movement query was missing: a segment against the
 * ENTITY boxes. Before this existed, doors were solid to the player's feet and
 * to nothing else — shots, rockets and sight lines all went through them.
 */
static void test_clip_segment(void)
{
    q2_move_target targets[4];
    q2_move_world w;
    q2_move_seg_hit hit;
    s32 from[3] = { 0, 0, 0 };
    s32 to[3]   = { 0, 0, 10000 };

    printf("clipping a segment against the entity boxes\n");

    memset(targets, 0, sizeof(targets));

    /* A door across the line at z = 2000..2200. */
    targets[0].min[0] = -1000; targets[0].min[1] = -1000; targets[0].min[2] = 2000;
    targets[0].max[0] =  1000; targets[0].max[1] =  1000; targets[0].max[2] = 2200;
    targets[0].kind   = Q2_MOVE_KIND_ENTITY;
    targets[0].id     = 3;
    targets[0].active = true;

    /* A second door FURTHER along: the nearest has to win, which is the one
     * thing q2_move_sweep_world does not do. */
    targets[1] = targets[0];
    targets[1].min[2] = 4000; targets[1].max[2] = 4200;
    targets[1].id     = 5;

    /* An inactive slot nearer than either. */
    targets[2] = targets[0];
    targets[2].min[2] = 500; targets[2].max[2] = 700;
    targets[2].id     = 8;
    targets[2].active = false;

    /* A trigger VOLUME nearer still. A shot does not stop in one. */
    targets[3] = targets[0];
    targets[3].min[2] = 200; targets[3].max[2] = 400;
    targets[3].kind   = Q2_MOVE_KIND_VOLUME;
    targets[3].mask   = 0x2000;
    targets[3].id     = 12;

    memset(&w, 0, sizeof(w));
    w.targets     = targets;
    w.count       = 4;
    w.half_extent = Q2_SWEEP_HALF_EXTENT;

    check(q2_move_clip_segment(&w, from, to, NULL, &hit),
          "a shot down the corridor is stopped");
    check_eq_i(hit.id, 3, "by the NEAREST door, not the last one tested");
    check_eq_i(hit.pos[2], 2000, "at its near face");
    check_eq_i(hit.frac, 2000 * 4096 / 10000,
               "and the fraction is where along the shot that is");
    check_eq_i(hit.normal[2], 4096,
               "the contact normal points along the ray, into the surface");
    check_eq_i(hit.normal[0], 0, "and is zero off the entry axis");

    /*
     * The two skips, checked by removing the door and seeing the segment run
     * clear past both the inactive slot and the volume — which sit in front of
     * where the door was.
     */
    targets[0].active = false;
    targets[1].active = false;
    check(!q2_move_clip_segment(&w, from, to, NULL, &hit),
          "an inactive door and a trigger volume stop nothing");
    check_eq_i(hit.frac, Q2_TRACE_SEG_ONE, "the whole segment survives");
    check(!hit.hit, "with no contact recorded");
    targets[0].active = true;
    targets[1].active = true;

    /* A segment that stops short of the door. */
    to[2] = 1500;
    check(!q2_move_clip_segment(&w, from, to, NULL, &hit),
          "a shot that ends before the door reaches its end");
    to[2] = 10000;

    /* Missing it sideways. */
    from[0] = 5000; to[0] = 5000;
    check(!q2_move_clip_segment(&w, from, to, NULL, &hit),
          "a shot down a parallel corridor misses");
    from[0] = 0; to[0] = 0;

    /*
     * THE BODY. A creature is a box, and inflating the door by its own
     * half-extents is what stops the creature's CENTRE early enough that the
     * creature itself does not end up inside the door.
     */
    {
        s32 body[3] = { 286, 572, 286 };

        check(q2_move_clip_segment(&w, from, to, body, &hit),
              "a creature walking at the door is stopped");
        check_eq_i(hit.pos[2], 2000 - 286,
                   "one half-extent short of the face, not flush against it");
    }

    /*
     * Starting INSIDE a box. There is no entry face, so there is no normal and
     * the fraction is zero — which for a sight line means blind and for a shot
     * means it goes nowhere. Both are the honest answer.
     */
    {
        s32 inside[3] = { 0, 0, 2100 };

        check(q2_move_clip_segment(&w, inside, to, NULL, &hit),
              "a segment starting inside a door is blocked");
        check_eq_i(hit.frac, 0, "at zero distance");
        check_eq_i(hit.normal[2], 0, "with no entry face to name");
    }

    /*
     * A near-parallel ray over a long distance. The slab quotient here is
     * numerator 10,000 << 12 over a denominator of 1, which is 40.96 million —
     * fine — but the same shape with a longer segment overflows a 32-bit
     * intermediate and turns a clean miss into a hit at a wrapped fraction.
     * The test is that a ray that grazes past the door's side reports a miss.
     */
    {
        s32 graze_from[3] = { 1001, 0, 0 };
        s32 graze_to[3]   = { 1002, 0, 1000000 };

        check(!q2_move_clip_segment(&w, graze_from, graze_to, NULL, &hit),
              "a near-parallel ray just outside the door misses over a "
              "million units");
    }

    /* No world at all, and an empty one: both are misses rather than crashes,
     * which is what a zone with no movers hands the AI binding. */
    check(!q2_move_clip_segment(NULL, from, to, NULL, &hit),
          "a null world blocks nothing");
    w.count = 0;
    check(!q2_move_clip_segment(&w, from, to, NULL, &hit),
          "and neither does an empty one");
}

/* ------------------------------------------------------------------------- */
static void test_contents(void)
{
    q2_move_target vols[2];
    q2_move_world w;
    s32 inside[3]  = { 0, 0, 0 };
    s32 outside[3] = { 100000, 0, 0 };

    printf("the contents query\n");

    memset(vols, 0, sizeof(vols));

    vols[0].min[0] = -1000; vols[0].min[1] = -1000; vols[0].min[2] = -1000;
    vols[0].max[0] =  1000; vols[0].max[1] =  1000; vols[0].max[2] =  1000;
    vols[0].kind   = Q2_MOVE_KIND_VOLUME;
    vols[0].mask   = 0x2000;
    vols[0].active = true;

    vols[1] = vols[0];
    vols[1].mask = 0x0004;      /* outside the player's mask */

    memset(&w, 0, sizeof(w));
    w.targets     = vols;
    w.count       = 2;
    w.half_extent = Q2_SWEEP_HALF_EXTENT;

    check_eq_i(q2_move_contents(&w, inside, Q2_CONTENTS_MASK), 0x2000,
               "a matching volume returns its own flags");
    check_eq_i(q2_move_contents(&w, outside, Q2_CONTENTS_MASK), 0,
               "and a point elsewhere returns nothing");
    check_eq_i(q2_move_contents(&w, inside, 0x0004), 0x0004,
               "a different mask selects the other volume");
    check_eq_i(q2_move_contents(&w, inside, 0x0001), 0,
               "a mask nothing carries returns nothing");

    /*
     * The box is 285 tall and 286 wide, not 286 cubed. At exactly 1,286 units
     * clear of the volume the two disagree: the horizontal reach still touches
     * it and the vertical one no longer does.
     */
    {
        s32 above[3] = { 0, -1286, 0 };
        s32 beside[3] = { -1286, 0, 0 };

        check_eq_i(q2_move_contents(&w, above, Q2_CONTENTS_MASK), 0,
                   "1,286 above the volume, the 285-unit vertical reach misses");
        check_eq_i(q2_move_contents(&w, beside, Q2_CONTENTS_MASK), 0x2000,
                   "1,286 beside it, the 286-unit horizontal reach touches");

        above[1] = -1285;
        check_eq_i(q2_move_contents(&w, above, Q2_CONTENTS_MASK), 0x2000,
                   "and one unit closer, the vertical one touches too");
    }
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    printf("collision model\n\n");

    test_parse();
    test_point_in_node();
    test_solid_bit();
    test_clip();
    test_move_through_portal();
    test_16bit_frame();
    test_push_vector();
    test_contact_slots();
    test_step_move();
    test_box_sweep();
    test_world_sweep();
    test_clip_segment();
    test_contents();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
