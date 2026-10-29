/* Blob placement and the emitted subtractive packet, without a disc. */
#include "modeldraw.h"
#include "raster.h"
#include "trace.h"
#include "worldscale.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static void wr16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32(u8 *p, u32 v) { wr16(p, (u16)v); wr16(p + 2, (u16)(v >> 16)); }

static void test_ground(void)
{
    u8 nodes[72] = {0}, planes[12] = {0};
    q2_collision hull = {0}, original;
    q2_model_instance inst;
    s32 at[3] = {500, 800, 500};
    /* One empty cell, with a floor at Y=1000. */
    wr32(nodes + 12, 2000); wr32(nodes + 16, 1000); wr32(nodes + 20, 2000);
    wr16(nodes + 36 + 24, 1);
    wr16(planes + 2, 1000); wr16(planes + 8, 4096);
    hull.nodes = nodes; hull.planes = planes;
    hull.node_count = hull.plane_count = 1;
    original = hull;
    q2_model_instance_init(&inst);
    inst.shadow_radius = 124;
    q2_model_shadow_ground(&inst, NULL, &hull, NULL, at, false);
    CHECK(inst.shadow_enabled);
    CHECK(inst.shadow_origin[0] == 500 && inst.shadow_origin[2] == 500);
    CHECK(inst.shadow_origin[1] == 1000 + Q2_EYE_BASE);
    CHECK(inst.shadow_height == 200);
    CHECK(memcmp(&hull, &original, sizeof(hull)) == 0);

    /* PrimaryColl is not eroded: no 286-unit correction to its floor. */
    at[1] = 700;
    q2_model_shadow_ground(&inst, &hull, NULL, NULL, at, false);
    CHECK(inst.shadow_enabled && inst.shadow_origin[1] == 1000);
    CHECK(inst.shadow_height == 14);

    /* An unavailable SecondaryColl cell falls back to PrimaryColl. */
    {
        q2_collision empty = {0};
        q2_model_shadow_ground(&inst, &hull, &empty, NULL, at, false);
        CHECK(inst.shadow_enabled && inst.shadow_origin[1] == 1000);
    }

    /* Following a moving body updates X/Z rather than retaining spawn X/Z. */
    at[0] = 650; at[2] = 750;
    q2_model_shadow_ground(&inst, &hull, NULL, NULL, at, false);
    CHECK(inst.shadow_origin[0] == 650 && inst.shadow_origin[2] == 750);

    /* A grounded body can be standing on a moving platform, not map geometry. */
    q2_model_shadow_ground(&inst, NULL, NULL, NULL, at, true);
    CHECK(inst.shadow_enabled && inst.shadow_height == 0);
    CHECK(inst.shadow_origin[1] == 986);
    q2_model_shadow_ground(&inst, NULL, NULL, NULL, at, false);
    CHECK(!inst.shadow_enabled);

    /* A raised lift receives the shadow above the map floor, then carries
     * it as it rises. A trigger occupying the same volume receives nothing. */
    {
        q2_move_target lift = {0};
        q2_move_world movers = {0};
        at[0] = at[2] = 500; at[1] = 700;
        lift.active = true;
        lift.kind = Q2_MOVE_KIND_ENTITY;
        lift.min[1] = 1050;
        lift.max[0] = lift.max[1] = lift.max[2] = 2000;
        movers.targets = &lift; movers.count = 1;
        q2_model_shadow_ground(&inst, NULL, &hull, &movers, at, false);
        CHECK(inst.shadow_enabled && inst.shadow_origin[1] == 1050);
        CHECK(inst.shadow_height == 64);
        lift.min[1] = 1000;
        q2_model_shadow_ground(&inst, NULL, &hull, &movers, at, false);
        CHECK(inst.shadow_enabled && inst.shadow_origin[1] == 1000);
        lift.min[1] = at[1] + Q2_EYE_BASE;
        q2_model_shadow_ground(&inst, NULL, NULL, &movers, at, false);
        CHECK(inst.shadow_enabled && inst.shadow_height == 0);
        lift.kind = 0;
        q2_model_shadow_ground(&inst, NULL, NULL, &movers, at, false);
        CHECK(!inst.shadow_enabled);
    }

    at[1] = 100; /* no floor within the retail 3*128 search */
    q2_model_shadow_ground(&inst, NULL, &hull, NULL, at, false);
    CHECK(!inst.shadow_enabled);
    at[0] = 3000;
    q2_model_shadow_ground(&inst, &hull, &hull, NULL, at, false);
    CHECK(!inst.shadow_enabled);
}

static void test_packet(void)
{
    q2_model model = {0};
    q2_model_instance inst;
    q2_model_draw_stats stats;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    psx_prim full;
    psx_vram *vram = calloc(1, sizeof(*vram));
    psx_framebuffer fb;
    psx_raster_opts opts;
    int x, y, dark = 0;
    CHECK(vram != NULL);
    if (!vram) return;
    CHECK(psx_ot_init(&ot, 256, 1) == Q2_OK);
    q2_camera_default(&cam, 512, 248);
    cam.pos[1] = -1000;
    gte_init(&gte);
    gte_set_projection(&gte, cam.projection, 256, 124);
    q2_model_instance_init(&inst);
    inst.model = &model;
    inst.origin[2] = inst.shadow_origin[2] = 4000;
    inst.shadow_enabled = true;
    inst.shadow_radius = 286;
    psx_ot_clear(&ot);
    CHECK(q2_model_build_ot(&inst, &cam, &ot, &gte, &stats) == 1);
    CHECK(stats.shadows_emitted == 1 && stats.faces_emitted == 0);
    full = ot.prims[0];
    CHECK(full.kind == PSX_PRIM_FT4 && full.quad_zorder);
    CHECK(full.semi_transparent && full.textured_blend);
    CHECK(full.tpage == psx_make_tpage(0, 1, PSX_BLEND_SUB, PSX_TEX_4BIT));
    CHECK(full.clut == psx_make_clut(0, 248));

    /* Render the actual generated packet. A synthetic all-STP grey tile
     * must subtract once, including across the quad's shared diagonal. */
    vram->px[248][1] = 0x8000 | (8 << 10) | (8 << 5) | 8;
    for (y = 480; y <= 495; y++)
        for (x = 56; x <= 59; x++) vram->px[y][x] = 0x1111;
    CHECK(psx_fb_init(&fb, 512, 248) == Q2_OK);
    psx_fb_clear(&fb, 0x7FFF);
    psx_raster_opts_default(&opts);
    opts.dither = false;
    psx_raster_prim(&fb, &full, vram, &opts);
    for (y = 0; y < fb.height; y++) for (x = 0; x < fb.width; x++) {
        u16 pixel = fb.px[y * fb.width + x] & 0x7FFF;
        if (pixel != 0x7FFF) {
            CHECK(pixel == ((23 << 10) | (23 << 5) | 23));
            dark++;
        }
    }
    CHECK(dark > 0);
    /* Index zero remains transparent. */
    memset(vram->px[480], 0, 16 * sizeof(vram->px[0]));
    psx_fb_clear(&fb, 0x7FFF);
    psx_raster_prim(&fb, &full, vram, &opts);
    dark = 0;
    for (y = 0; y < fb.height; y++) for (x = 0; x < fb.width; x++)
        if ((fb.px[y * fb.width + x] & 0x7FFF) != 0x7FFF) dark++;
    CHECK(dark == 0);
    psx_fb_free(&fb);
    free(vram);

    /* An exhausted OT reports overflow, then height halves the footprint. */
    CHECK(q2_model_build_ot(&inst, &cam, &ot, &gte, &stats) == 0);
    CHECK(stats.ot_overflow == 1);
    psx_ot_clear(&ot);
    inst.shadow_height = 300;
    CHECK(q2_model_build_ot(&inst, &cam, &ot, &gte, &stats) == 1);
    CHECK(abs(ot.prims[0].xy[0].x - ot.prims[0].xy[1].x) <
          abs(full.xy[0].x - full.xy[1].x));
    psx_ot_clear(&ot);
    inst.shadow_height = 600;
    CHECK(q2_model_build_ot(&inst, &cam, &ot, &gte, &stats) == 0);
    CHECK(ot.prim_count == 0);
    inst.shadow_height = 0;
    inst.shadow_enabled = false;
    CHECK(q2_model_build_ot(&inst, &cam, &ot, &gte, &stats) == 0);
    inst.shadow_enabled = true;
    inst.shadow_radius = 500; /* a wider creature/corpse uses its live radius */
    CHECK(q2_model_build_ot(&inst, &cam, &ot, &gte, &stats) == 1);
    CHECK(abs(ot.prims[0].xy[0].x - ot.prims[0].xy[1].x) >
          abs(full.xy[0].x - full.xy[1].x));
    psx_ot_free(&ot);
}

int main(void)
{
    test_ground();
    test_packet();
    printf("shadow blobs: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
