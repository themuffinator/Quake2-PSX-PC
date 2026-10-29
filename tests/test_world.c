/*
 * test_world.c — the two gates that decide whether a world quad is drawn.
 *
 * Both were missing, and between them they blacked out doorways and whole
 * corridors all over the game. The disc-wide evidence for each lives in
 * `q2psx-inspect surfaces`, which counts the sealing nodes and cross-checks
 * them against every SortData stream. What a census cannot reach is the exact
 * decision — which corners the backface test uses, which way its comparisons
 * run, and whether "sealing" is a property of a node or of a polygon — so that
 * is what is pinned here.
 */
#include <stdio.h>
#include <string.h>

#include "scene.h"
#include "world.h"
#include "modeldraw.h"

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

/* ------------------------------------------------------------------------- */
/* Sealing geometry — a node whose every polygon binds CLUT index 0            */
/* ------------------------------------------------------------------------- */

/* Build a MapMod polygon record in place. `clut_hi` gives each polygon's
 * palette index; everything else is whatever a real record would carry. */
static void make_rec(q2_mapmod_rec *rec, u8 *storage,
                     const u8 *clut_hi, u32 count)
{
    u32 p;

    memset(rec, 0, sizeof(*rec));
    memset(storage, 0, (size_t)count * Q2_MAPMOD_POLY_SIZE);

    for (p = 0; p < count; p++) {
        u8 *poly = storage + (size_t)p * Q2_MAPMOD_POLY_SIZE;

        poly[0] = 0; poly[1] = 1; poly[2] = 2; poly[3] = 3;   /* vtx */
        poly[8] = 0x30;                                        /* clut low  */
        poly[9] = clut_hi[p];                                  /* clut high */
        poly[10] = 0;                                          /* tpage     */
        poly[11] = 0;                                          /* uv_idx    */
    }

    rec->num_polys = count;
    rec->polys     = storage;
}

static void test_sealing(void)
{
    u8 storage[8 * Q2_MAPMOD_POLY_SIZE];
    q2_mapmod_rec rec;

    puts("\nsealing geometry");

    {
        static const u8 all_zero[4] = { 0, 0, 0, 0 };
        make_rec(&rec, storage, all_zero, 4);
        check(q2_mapmod_rec_is_sealing(&rec),
              "every polygon on CLUT index 0 makes the node sealing");
    }

    {
        /* The disc has no node like this — 0 of 11,255 polygons — so the point
         * of the case is that one real palette is enough to disqualify the
         * whole node. Getting this backwards would delete real surface. */
        static const u8 one_real[4] = { 0, 0, 22, 0 };
        make_rec(&rec, storage, one_real, 4);
        check(!q2_mapmod_rec_is_sealing(&rec),
              "one real palette anywhere in the node disqualifies it");
    }

    {
        static const u8 real[4] = { 16, 22, 40, 85 };
        make_rec(&rec, storage, real, 4);
        check(!q2_mapmod_rec_is_sealing(&rec),
              "ordinary surface is not sealing");
    }

    {
        static const u8 one[1] = { 0 };
        make_rec(&rec, storage, one, 1);
        check(q2_mapmod_rec_is_sealing(&rec),
              "a single-polygon sealing node still counts");
    }

    {
        /* Five nodes on the disc carry no polygons. Calling those sealing
         * would be a vacuous truth that hides them from every caller. */
        static const u8 none[1] = { 0 };
        make_rec(&rec, storage, none, 0);
        check(!q2_mapmod_rec_is_sealing(&rec),
              "an empty record seals nothing");
    }

    check(!q2_mapmod_rec_is_sealing(NULL), "a null record seals nothing");

    /*
     * The palette index is the HIGH byte. If it were read as the whole
     * halfword, the low byte's residue — set on 91.6% of polygons — would make
     * almost nothing look sealing.
     */
    {
        static const u8 all_zero[2] = { 0, 0 };
        make_rec(&rec, storage, all_zero, 2);
        storage[8]  = 0xFF;   /* low byte residue on polygon 0 */
        storage[20] = 0x3C;
        check(q2_mapmod_rec_is_sealing(&rec),
              "the low byte's residue does not affect the palette index");
    }
}

static void test_scene_area(void)
{
    u8 raw[Q2_SCENE_NODE_SIZE];
    q2_scene scene;
    q2_scene_node node;

    puts("\nscene draw area");
    memset(raw, 0, sizeof(raw));
    memset(&scene, 0, sizeof(scene));
    raw[0x0E] = 0xA7;
    scene.nodes = raw;
    scene.node_count = 1;

    check(q2_scene_get_node(&scene, 0, &node),
          "a Scene record decodes");
    check(node.area == 0xA7,
          "Scene byte +0x0E is retained as the deferred draw area");
    check((node.area & 0x7F) == 0x27,
          "the area table uses its low seven bits");
}

static void test_sort_region_visibility(void)
{
    psx_ot_area_screen r;

    puts("\nSortData screen-region visibility");
    memset(&r, 0, sizeof(r));
    r.max_x = 64;
    r.max_y = 32;
    check(q2_world_sort_region_visible(&r),
          "a rectangle with two non-zero dimensions is visible");

    r.max_x = 0;
    check(!q2_world_sort_region_visible(&r),
          "a vertical-axis line takes retail's false stream arm");
    r.max_x = 64;
    r.max_y = 0;
    check(!q2_world_sort_region_visible(&r),
          "a horizontal-axis line takes retail's false stream arm");
    check(!q2_world_sort_region_visible(NULL),
          "a missing region is not visible");
}

/* ------------------------------------------------------------------------- */
/* Backface rejection                                                         */
/* ------------------------------------------------------------------------- */
static void set_quad(gte_sxy q[4],
                     s16 x0, s16 y0, s16 x1, s16 y1,
                     s16 x2, s16 y2, s16 x3, s16 y3)
{
    q[0].x = x0; q[0].y = y0;
    q[1].x = x1; q[1].y = y1;
    q[2].x = x2; q[2].y = y2;
    q[3].x = x3; q[3].y = y3;
}

static void test_backface(void)
{
    gte_state gte;
    gte_sxy   q[4];
    gte_sxy   r[4];
    int       i;

    puts("\nbackface rejection");

    memset(&gte, 0, sizeof(gte));

    /*
     * A screen-space square, corners running clockwise with Y down. NCLIP's
     * signed area over (v0,v1,v3) is +10000, so this is the facing the
     * original keeps.
     */
    set_quad(q, 0, 0, 100, 0, 100, 100, 0, 100);
    check(q2_world_quad_faces_camera(&gte, q),
          "the front facing is drawn");

    /* The same quad with its corners reversed is the other facing, and exactly
     * one of the two may survive. A test that only checked the accepted case
     * would pass against a function that always returned true, which is
     * precisely the function the port had. */
    for (i = 0; i < 4; i++)
        r[i] = q[3 - i];
    check(!q2_world_quad_faces_camera(&gte, r),
          "and the back facing is not");

    /*
     * The second NCLIP is not redundant. A quad that has folded — one corner
     * swung across the diagonal, which is what a near-plane vertex does —
     * has one half facing away and one facing the camera, and the original
     * keeps it. Corner v0 is dragged past the v1-v3 diagonal so that the
     * (v0,v1,v3) half inverts while the (v2,v1,v3) half does not.
     */
    set_quad(q, 160, 160, 100, 0, 100, 100, 0, 100);
    gte.sxy[0] = q[0]; gte.sxy[1] = q[1]; gte.sxy[2] = q[3];
    gte_nclip(&gte);
    check(gte.mac0 <= 0, "the folded quad's first half fails on its own");
    check(q2_world_quad_faces_camera(&gte, q),
          "but the second half rescues it, as the original does");

    /*
     * Degenerate is not visible. A quad collapsed to a line gives zero area on
     * both halves, and the original's comparisons — `> 0` then `>= 0` to
     * reject — drop it. Reading either as its non-strict twin would emit a
     * sliver of every edge-on surface in the level.
     */
    set_quad(q, 0, 0, 50, 0, 100, 0, 150, 0);
    check(!q2_world_quad_faces_camera(&gte, q),
          "a quad collapsed to a line is not drawn");

    /* And it must not depend on leftover GTE state: the same quad twice in a
     * row answers the same way. */
    set_quad(q, 0, 0, 100, 0, 100, 100, 0, 100);
    {
        bool first  = q2_world_quad_faces_camera(&gte, q);
        bool second = q2_world_quad_faces_camera(&gte, q);
        check(first == second, "the test is not order-dependent");
    }
}

/* ------------------------------------------------------------------------- */
/* The linker's rejection set, and the near rejection that is not in it        */
/* ------------------------------------------------------------------------- */

static void set_depth(u16 d[4], u16 d0, u16 d1, u16 d2, u16 d3)
{
    d[0] = d0; d[1] = d1; d[2] = d2; d[3] = d3;
}

/*
 * The finding this pins: none of the three quad linkers rejects a quad for
 * having a corner close to the eye.
 *
 * `q2_world_quad_linker_rejects()` was carrying an extra arm — "a corner's
 * divide overflowed and nothing is going to subdivide this, so drop the whole
 * quad" — which has no counterpart in the executable. The depth sum is compared
 * exactly once, at 0x800AF974, and only to decide whether SubDivRoutine is
 * called; when that routine declines and returns 0 (0x800699A8, 0x800699C4) the
 * `bne v0, zero` at 0x800AF9B4 is not taken and 0x800AF9BC links the flat quad
 * anyway. Variant 1 (0x800AFC9C) never calls the hook at all and still links.
 *
 * The cases below are all quads whose corners are far nearer than the divide
 * overflow threshold — `gte_divide` overflows once the projection distance
 * reaches twice the depth, so at the one-player H of 160 anything under SZ 80 —
 * and every one of them must be drawn.
 */
static void test_near_is_not_a_rejection(void)
{
    gte_state gte;
    gte_sxy   q[4];
    u16       d[4];

    puts("\nnear corners are not a rejection");

    memset(&gte, 0, sizeof(gte));

    /* The clockwise square the world keeps, well inside a 320x240 viewport,
     * with one corner at SZ 4 — deep inside the overflow band. */
    set_quad(q, 40, 40, 200, 40, 200, 180, 40, 180);
    set_depth(d, 4, 900, 900, 900);

    check(q2_world_quad_linker_rejects(&gte, Q2_SURF_VARIANT_SUBDIVIDE,
                                       q, d, 320, 240) == Q2_WORLD_QUAD_DRAW,
          "variant 0 draws a quad with a corner inside the overflow band");
    check(q2_world_quad_linker_rejects(&gte, Q2_SURF_VARIANT_TAGGED,
                                       q, d, 320, 240) == Q2_WORLD_QUAD_DRAW,
          "variant 2 draws it too");

    /*
     * And variant 1 above all, because it never subdivides: the deleted test's
     * escape clause could never fire for it, so every near variant-1 quad in
     * the level was being thrown away unconditionally.
     */
    check(q2_world_quad_linker_rejects(&gte, Q2_SURF_VARIANT_FLAT,
                                       q, d, 320, 240) == Q2_WORLD_QUAD_DRAW,
          "and variant 1, which has no subdivision to be rescued by");

    /* All four corners near — a floor the player is standing on. Still drawn. */
    set_depth(d, 3, 5, 6, 4);
    check(q2_world_quad_linker_rejects(&gte, Q2_SURF_VARIANT_SUBDIVIDE,
                                       q, d, 320, 240) == Q2_WORLD_QUAD_DRAW,
          "a quad with all four corners near is still drawn");

    /*
     * Variant 1's zero-depth test is a different thing and is still here
     * (0x800AFD20 / 0x800AFD34). It looks at vertex 1 and vertex 3 only, so a
     * zero on vertex 0 or 2 is not its business — and variants 0 and 2 do not
     * have the test at all. Reading it as "any corner at zero" or applying it
     * to all three variants would put the deleted rejection back in disguise.
     */
    set_depth(d, 900, 0, 900, 900);
    check(q2_world_quad_linker_rejects(&gte, Q2_SURF_VARIANT_FLAT,
                                       q, d, 320, 240)
              == Q2_WORLD_QUAD_REJECT_FLAT,
          "variant 1 drops a quad whose vertex 1 projected to depth zero");
    set_depth(d, 900, 900, 900, 0);
    check(q2_world_quad_linker_rejects(&gte, Q2_SURF_VARIANT_FLAT,
                                       q, d, 320, 240)
              == Q2_WORLD_QUAD_REJECT_FLAT,
          "and one whose vertex 3 did");
    set_depth(d, 0, 900, 0, 900);
    check(q2_world_quad_linker_rejects(&gte, Q2_SURF_VARIANT_FLAT,
                                       q, d, 320, 240) == Q2_WORLD_QUAD_DRAW,
          "but not one where only vertices 0 and 2 are at zero");
    set_depth(d, 900, 0, 900, 0);
    check(q2_world_quad_linker_rejects(&gte, Q2_SURF_VARIANT_SUBDIVIDE,
                                       q, d, 320, 240) == Q2_WORLD_QUAD_DRAW,
          "and variants 0 and 2 have no zero-depth test to apply");

    /*
     * The straddle escape, which is what makes a near quad drawable at all: its
     * saturated corners land off opposite edges of the viewport, and variants 0
     * and 2 only reject when all four are off the SAME edge
     * (0x800AF8DC-0x800AF968). Variant 1 (0x800AFDA0-0x800AFDEC) has no escape.
     */
    set_depth(d, 4, 900, 900, 900);
    set_quad(q, -900, -900, 900, -900, 900, 900, -900, 900);
    check(q2_world_quad_linker_rejects(&gte, Q2_SURF_VARIANT_SUBDIVIDE,
                                       q, d, 320, 240) == Q2_WORLD_QUAD_DRAW,
          "a quad straddling the viewport on both axes survives");
    check(q2_world_quad_linker_rejects(&gte, Q2_SURF_VARIANT_FLAT,
                                       q, d, 320, 240)
              == Q2_WORLD_QUAD_REJECT_BOUNDS,
          "but variant 1 needs a corner inside on each axis");

    /* All four off the same edge is the case that really is rejected. */
    set_quad(q, -900, 40, -700, 40, -700, 180, -900, 180);
    set_depth(d, 900, 900, 900, 900);
    check(q2_world_quad_linker_rejects(&gte, Q2_SURF_VARIANT_SUBDIVIDE,
                                       q, d, 320, 240)
              == Q2_WORLD_QUAD_REJECT_BOUNDS,
          "all four corners off the same edge is rejected");

    /* A viewport with no area rejects everything, which is the guard the build
     * loop used to carry separately. */
    set_quad(q, 40, 40, 200, 40, 200, 180, 40, 180);
    check(q2_world_quad_linker_rejects(&gte, Q2_SURF_VARIANT_SUBDIVIDE,
                                       q, d, 0, 240)
              == Q2_WORLD_QUAD_REJECT_BOUNDS,
          "an empty clip rectangle draws nothing");

    /* Backface still runs, and reports itself as backface rather than as one of
     * the other two. */
    {
        gte_sxy r[4];
        int i;

        for (i = 0; i < 4; i++)
            r[i] = q[3 - i];
        check(q2_world_quad_linker_rejects(&gte, Q2_SURF_VARIANT_SUBDIVIDE,
                                           r, d, 320, 240)
                  == Q2_WORLD_QUAD_REJECT_BACK,
              "the back facing is still culled, and says so");
    }
}

/*
 * The model linker's own rejection, whose two comparisons are the world's
 * EXCHANGED (0x800B24A0 `blez` against 0x800AF8A8 `bgtz`). Model quads are
 * wound the opposite way round, so the same square that the world draws is one
 * a model drops, and vice versa.
 *
 * This is here rather than in test_model.c because that binary links only the
 * format libraries and the rule needs the GTE — and because the point of the
 * test is the RELATIONSHIP between the two rules, which is only visible with
 * both in front of you.
 */
static void test_model_backface(void)
{
    gte_state gte;
    gte_sxy   q[4];
    gte_sxy   r[4];
    int       i;

    puts("\nbackface rejection: models, inverted");

    memset(&gte, 0, sizeof(gte));

    /* The clockwise square the world keeps. A model drops it. */
    set_quad(q, 0, 0, 100, 0, 100, 100, 0, 100);
    check(q2_world_quad_faces_camera(&gte, q),
          "the world draws the clockwise square");
    check(!q2_model_quad_faces_camera(&gte, q),
          "and a model does not: the comparisons are exchanged");

    /* And the reverse winding, the other way round. Checking both directions
     * is what makes this a test of the inversion rather than of one constant:
     * a function stuck at either answer fails one of the four. */
    for (i = 0; i < 4; i++)
        r[i] = q[3 - i];
    check(!q2_world_quad_faces_camera(&gte, r),
          "the world drops the anticlockwise square");
    check(q2_model_quad_faces_camera(&gte, r),
          "and a model draws it");

    /*
     * Degenerate is VISIBLE on this path, and that asymmetry is the code's,
     * not an oversight here: the model's first test is `MAC0 <= 0`, which a
     * zero area satisfies. The world's is `> 0`, which it does not. A quad
     * collapsed to a line covers no pixels either way, so nothing depends on
     * it — but folding the two rules into one shared helper with a sign flag
     * would silently change this, which is why it is pinned.
     */
    set_quad(q, 0, 0, 50, 0, 100, 0, 150, 0);
    check(q2_model_quad_faces_camera(&gte, q),
          "a degenerate quad passes the model's non-strict first test");

    /* Not order-dependent, for the same reason the world's is not. */
    set_quad(q, 0, 0, 100, 0, 100, 100, 0, 100);
    {
        bool first  = q2_model_quad_faces_camera(&gte, q);
        bool second = q2_model_quad_faces_camera(&gte, q);
        check(first == second, "the test is not order-dependent");
    }
}

/* ------------------------------------------------------------------------- */
/* ProcessMobileLights — a runtime light repaints the level's own vertices     */
/* ------------------------------------------------------------------------- */
/*
 * The pass 0x80074598 runs after DrawAllScenes every frame and that this port
 * never had: sixteen dynamic lights were appended, gathered per ENTITY, and
 * dropped, so a rocket lit itself and left the corridor it flew down black.
 *
 * The world half is a walk, and this is the walk: one node, one polygon, four
 * points, one light in the middle of it. What the test pins is not the ramp —
 * lighting.c owns that and test_light.c checks it — but the three things the
 * walk decides: that the light is tested against the NODE's box, that the point
 * is made local against the node's origin, and that a corner is painted only
 * when the point's own slot list names it.
 */
static void mob_put_u16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

static void mob_put_u32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static void mob_put_s32(u8 *p, s32 v) { mob_put_u32(p, (u32)v); }

static void test_model_part_force_draw(void)
{
    u8 bytes[64 + 48 + 16 + 66 * 16] = {0};
    q2_model model = {0};
    q2_model_instance inst;
    q2_model_draw_stats stats;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    const s16 xy[4][2] = {{-100,-100},{100,-100},{100,100},{-100,100}};
    int i;
    model.base = bytes; model.size = sizeof(bytes);
    model.hdr.num_verts = 4; model.hdr.num_parts = 2; model.hdr.num_faces = 66;
    model.hdr.ofs_verts = 64; model.hdr.ofs_parts = 112; model.hdr.ofs_faces = 128;
    for (i = 0; i < 4; i++) {
        mob_put_u16(bytes + 64 + i*12, (u16)xy[i][0]);
        mob_put_u16(bytes + 66 + i*12, (u16)xy[i][1]);
    }
    /* Both parts reuse the same projected scratch slots. Each has 33 backfaces
     * so a wrapping mask or a mask read from the draw-order table is observable. */
    mob_put_u16(bytes + 112, 33); bytes[115] = 4;
    mob_put_u16(bytes + 120, 33);
    for (i = 0; i < 66; i++) {
        u8 *face = bytes + 128 + i*16;
        face[0] = 0; face[1] = 1; face[2] = 2; face[3] = 3;
        face[4] = (u8)i;
    }
    q2_camera_default(&cam, 512, 248);
    q2_model_instance_init(&inst);
    inst.model = &model; inst.origin[2] = 2000;
    gte_init(&gte);
    gte_set_projection(&gte, 160, 256, 124);
    check(psx_ot_init(&ot, 256, 80) == Q2_OK, "model force test OT allocates");
    check(q2_model_build_ot(&inst, &cam, &ot, &gte, &stats) == 0,
          "unforced model backfaces are rejected");
    check(stats.faces_rejected_back == 66, "both parts exercise NCLIP");
    mob_put_u32(bytes + 116, 0x80000001u);
    mob_put_u32(bytes + 124, 0x80000001u);
    psx_ot_clear(&ot);
    check(q2_model_build_ot(&inst, &cam, &ot, &gte, &stats) == 4,
          "part masks force first and 32nd faces, restarting per part");
    check(stats.faces_rejected_back == 62, "33rd face does not wrap to bit 31");
    if (ot.prim_count == 4) {
        check(ot.prims[0].uv[0].u == 0 && ot.prims[1].uv[0].u == 31 &&
              ot.prims[2].uv[0].u == 33 && ot.prims[3].uv[0].u == 64,
              "MSB-first force bits select exactly the retail faces");
    }
    psx_ot_free(&ot);
}

/* The quad's four corners, 1000 units square, 2000 in front of the camera. */
static const s16 mob_pt[4][3] = {
    { -500, -500, 2000 },
    {  500, -500, 2000 },
    {  500,  500, 2000 },
    { -500,  500, 2000 },
};

/*
 * Vertex j paints packet corner (0, 1, 3, 2)[j] — the Z order libgpu wants, and
 * the shuffle the world renderer's own colour writes establish. A slot byte is
 * poly * 4 + packet corner, so for polygon 0 those are 0, 1, 3 and 2.
 */
static const u8 mob_slot_of_vertex[4] = { 0, 1, 3, 2 };

static void mob_build(u8 *scene_nodes, u8 *mapmod, u8 *points,
                      const u8 *slot0, u8 baked)
{
    int v;

    /* One Scene node: MapMod at 0, variant 0, origin at the world origin and a
     * box that contains the quad. */
    memset(scene_nodes, 0, Q2_SCENE_NODE_SIZE);
    mob_put_u32(scene_nodes + 0x00, 0);       /* mapmod_offset */
    mob_put_u16(scene_nodes + 0x08, 0);       /* flags: drawn, variant 0 */
    for (v = 0; v < 3; v++) {
        mob_put_s32(scene_nodes + 0x10 + v * 4, -4000);
        mob_put_s32(scene_nodes + 0x1C + v * 4,  4000);
        mob_put_s32(scene_nodes + 0x28 + v * 4,  0);   /* origin */
    }

    /* One polygon: vertices 0..3, every corner on colour 0, no CLUT, UV set 0. */
    memset(mapmod, 0, 32);
    mob_put_u16(mapmod + 0, 1);      /* num_polys      */
    mob_put_u16(mapmod + 2, 20);     /* 8 + 1*12       */
    mob_put_u32(mapmod + 4, 24);     /* uv_offset      */
    mapmod[8]  = 0; mapmod[9]  = 1; mapmod[10] = 2; mapmod[11] = 3;  /* vtx */
    mapmod[12] = 0; mapmod[13] = 0; mapmod[14] = 0; mapmod[15] = 0;  /* col */
    mob_put_u16(mapmod + 16, 0x3000);  /* clut: a real palette, not sealing */
    mapmod[18] = 0;                    /* tpage  */
    mapmod[19] = 0;                    /* uv_idx */
    mapmod[20] = baked; mapmod[21] = baked; mapmod[22] = baked;

    for (v = 0; v < 4; v++) {
        u8 *p = points + v * Q2_POINT_SIZE;

        mob_put_u16(p + 0, (u16)mob_pt[v][0]);
        mob_put_u16(p + 2, (u16)mob_pt[v][1]);
        mob_put_u16(p + 4, (u16)mob_pt[v][2]);
        p[6] = slot0[v];
        p[7] = p[8] = p[9] = p[10] = p[11] = 0xFF;
    }
}

static u32 mob_run(const u8 *slot0, bool with_light, q2_world_stats *out)
{
    u8 scene_nodes[Q2_SCENE_NODE_SIZE];
    u8 mapmod[32];
    u8 points[4 * Q2_POINT_SIZE];
    q2_point_group group = { 0, 4 };
    q2_world_zone z;
    q2_light_world lw;
    q2_camera cam;
    q2_world_stats stats;
    q2_world_render render;
    psx_ot ot;
    gte_state gte;
    u32 emitted;

    mob_build(scene_nodes, mapmod, points, slot0, 64);

    memset(&z, 0, sizeof(z));
    z.scene.nodes       = scene_nodes;
    z.scene.node_count  = 1;
    z.scene.mapmod      = mapmod;
    z.scene.mapmod_size = sizeof(mapmod);
    z.points.data       = points;
    z.points.count      = 4;
    z.points.groups     = &group;
    z.points.group_count = 1;

    memset(&lw, 0, sizeof(lw));
    if (with_light) {
        static const s32 at[3]  = { 0, 0, 2000 };
        static const u8  rgb[3] = { 200, 100, 100 };

        /* 600/1200, the muzzle-flash pair — the shift comes out 3 and the
         * working radii 5625 and 22500, which lighting.h works through. */
        q2_light_add_dynamic(&lw, at, rgb, 600, 1200, 0, 0);
    }
    z.lights = &lw;

    memset(&cam, 0, sizeof(cam));
    cam.projection = 160;

    q2_world_render_init(&render);
    memset(&ot, 0, sizeof(ot));
    psx_ot_init(&ot, 1024, 64);
    memset(&gte, 0, sizeof(gte));
    memset(&stats, 0, sizeof(stats));

    emitted = q2_world_build_ot(&z, &cam, 320, 240, &ot, &gte, &render, &stats);

    if (out)
        *out = stats;

    psx_ot_free(&ot);
    return emitted;
}

static void test_mobile_light_repaints_the_world(void)
{
    q2_world_stats lit, dark, half;
    u8 slots[4];
    int v;

    puts("\nmobile lights repaint the world's own vertices");

    for (v = 0; v < 4; v++)
        slots[v] = mob_slot_of_vertex[v];

    check(mob_run(slots, true, &lit) == 1, "the quad is emitted");
    check(lit.mobile_lights == 1,
          "the light's box overlaps the node's, so the node is lit");
    check(lit.mobile_corners == 4,
          "all four corners take a share of it");
    check(lit.mobile_unslotted == 0,
          "and every corner is named by its point's slot list");

    /* With the list empty nothing is repainted, which is what this port did on
     * every frame of every map before the walk existed. */
    check(mob_run(slots, false, &dark) == 1, "the same quad still draws");
    check(dark.mobile_lights == 0 && dark.mobile_corners == 0,
          "with no runtime light the baked colours stand alone");

    /*
     * A POINT PAINTS ONLY THE CORNERS IT NAMES. 0x80074A84 breaks the slot walk
     * on the first 0xFF, so a point with no slots paints nothing at all — the
     * console has no way to reach that packet corner. Blanking vertex 2's slot
     * must cost exactly one corner, not four and not none.
     */
    slots[2] = 0xFF;
    check(mob_run(slots, true, &half) == 1, "the quad is emitted either way");
    check(half.mobile_corners == 3,
          "a point whose slot list is empty paints no corner");
    check(half.mobile_unslotted == 1, "and is counted as unreachable");
}

/* Values obtained from the add/sra midpoint sequence at PAL 0x800AEEA0..
 * 0x800AF0C0, with perimeter corners 7,102,239,31. The centre is 66,
 * not the bilinear average 94. The odd values also expose intermediate
 * rounding: the first quarter is 30, not a truncated-delta lerp's 31. */
static void test_retail_subdivision(void)
{
    static const u8 value[5][5] = {
        { 7, 30, 54, 78, 102 }, { 13, 36, 60, 84, 136 },
        { 19, 42, 66, 118, 170 }, { 25, 48, 100, 152, 204 },
        { 31, 83, 135, 187, 239 }
    };
    static const u8 corner[4] = { 7, 102, 239, 31 };
    static const int dx[4] = {0, 1, 1, 0}, dy[4] = {0, 0, 1, 1};
    u8 scene_nodes[Q2_SCENE_NODE_SIZE], mapmod[40] = {0};
    u8 points[4 * Q2_POINT_SIZE];
    q2_point_group group = {0, 4};
    q2_world_zone z = {0};
    q2_camera cam = {0};
    q2_world_stats stats;
    q2_world_render render;
    psx_ot ot = {0};
    gte_state gte, reference;
    int i, x, y;
    u32 emitted;

    mob_build(scene_nodes, mapmod, points, mob_slot_of_vertex, 0);
    mob_put_u32(mapmod + 4, 32);
    for (i = 0; i < 4; i++) {
        mapmod[12+i] = (u8)i;
        memset(mapmod + 20+3*i, corner[i], 3);
        mapmod[32+2*(3-i)] = mapmod[33+2*(3-i)] = corner[i];
        mob_put_u16(points+i*Q2_POINT_SIZE+4, (u16)(2000+corner[i]));
    }
    z.scene.nodes = scene_nodes; z.scene.node_count = 1;
    z.scene.mapmod = mapmod; z.scene.mapmod_size = sizeof(mapmod);
    z.points.data = points; z.points.count = 4;
    z.points.groups = &group; z.points.group_count = 1;
    cam.projection = 160;
    q2_world_render_init(&render);
    render.subdiv_threshold = 40000;
    if (psx_ot_init(&ot, 1024, 64) != Q2_OK) {
        check(false, "allocate subdivision test OT");
        return;
    }
    emitted = q2_world_build_ot(&z, &cam, 320, 240, &ot, &gte, &render, &stats);
    check(emitted == 16 && stats.quads_subdivided == 1,
          "one near world quad produces the retail sixteen packets");
    reference = gte;
    if (emitted == 16) for (y = 0; y < 4; y++) for (x = 0; x < 4; x++) {
        const psx_prim *p = &ot.prims[y*4+x];
        for (i = 0; i < 4; i++) {
            int gx = x+dx[i], gy = y+dy[i];
            u8 expected = value[gy][gx];
            reference.v[0].x = (s16)(-500+gx*250);
            reference.v[0].y = (s16)(-500+gy*250);
            reference.v[0].z = (s16)(2000+expected);
            gte_rtps(&reference, false);
            check(p->xy[i].x == reference.sxy[2].x && p->xy[i].y == reference.sxy[2].y,
                  "subdivision projects the retail midpoint geometry");
            check(p->rgb[i].r == expected && p->rgb[i].g == expected && p->rgb[i].b == expected,
                  "subdivision lighting follows rounded diagonal midpoints");
            check(p->uv[i].u == expected && p->uv[i].v == expected,
                  "subdivision UVs follow rounded diagonal midpoints");
        }
    }
    /* Parent storage gates the real walker independently of the host OT and
     * the mesh pool. Denial must not allocate a child or promote a material. */
    q2_world_render_init(&render);render.subdiv_threshold=40000;
    q2_world_packet_reset(&render,-1);
    emitted=q2_world_build_ot(&z,&cam,320,240,&ot,&gte,&render,&stats);
    check(!emitted && stats.nodes_packet_denied==1 && !render.subdiv_used,
          "parent exhaustion denies a node before it can allocate meshes");
    check(!(render.tpage.word[0]&0x60),"failed parent does not promote texture page");

    q2_world_packet_reset(&render,32);
    mob_put_u16(mapmod+16,0x0101); /* transparent on an unpromoted page */
    emitted=q2_world_build_ot(&z,&cam,320,240,&ot,&gte,&render,&stats);
    check(emitted==16 && !(ot.prims[0].tpage&0x60),
          "new transparent parent and children retain original blend mode");
    q2_tpage_world_opaque(&render.tpage,0);
    q2_world_render_end_frame(&render);
    emitted=q2_world_build_ot(&z,&cam,320,240,&ot,&gte,&render,&stats);
    check(emitted==16 && !(ot.prims[0].tpage&0x60),
          "later page promotion does not rewrite cached parent or children");
    q2_world_render_end_frame(&render);
    mob_put_u16(scene_nodes+8,0x8000);
    q2_world_build_ot(&z,&cam,320,240,&ot,&gte,&render,&stats);
    check(!render.packet_dirs_used && render.packet_pending_count==1,
          "hidden node retires its parent while block remains pending");
    q2_world_render_end_frame(&render);mob_put_u16(scene_nodes+8,0);
    emitted=q2_world_build_ot(&z,&cam,320,240,&ot,&gte,&render,&stats);
    check(emitted==16 && (ot.prims[0].tpage&0x60)==0x20,
          "reconstructed parent samples the now-promoted texture page");

    q2_world_render_init(&render);mob_put_u16(mapmod+16,0x0100);
    mob_put_u16(scene_nodes+8,0x0C00); /* emitter hidden, parent still built */
    emitted=q2_world_build_ot(&z,&cam,320,240,&ot,&gte,&render,&stats);
    check(!emitted && render.packet_used==1 && (render.tpage.word[0]&0x60)==0x20,
          "variant 3 constructs materials before dispatching its empty emitter");
    psx_ot_free(&ot);
}

int main(void)
{
    puts("world quad gates");
    puts("================");

    test_sealing();
    test_scene_area();
    test_sort_region_visibility();
    test_backface();
    test_near_is_not_a_rejection();
    test_model_backface();
    test_model_part_force_draw();
    test_mobile_light_repaints_the_world();
    test_retail_subdivision();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
