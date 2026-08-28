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

int main(void)
{
    puts("world quad gates");
    puts("================");

    test_sealing();
    test_scene_area();
    test_sort_region_visibility();
    test_backface();
    test_model_backface();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
