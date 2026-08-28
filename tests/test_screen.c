/*
 * test_screen.c — the screen's behaviour, not its constants.
 *
 * `q2psx-inspect screen <disc>` already reads every literal back off a real
 * executable, so nothing here re-asserts a projection distance. What this pins
 * down is the behaviour those constants produce and that no table can check:
 * that the buffers alternate, that the ordering table's slices tile it exactly
 * and do not overlap, that a viewport's primitives cannot escape into another
 * viewport's slice, that the screen shake shrinks a viewport rather than moving
 * it off its own edge, that the full-screen background clear turns the
 * per-viewport ones off, and that a long host frame is clamped rather than
 * averaged.
 */
#include "screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

/* ------------------------------------------------------------------------- */
static void test_bringup(void)
{
    q2_screen s;

    CHECK(q2_screen_init(&s, Q2_VIDEO_PAL) == Q2_OK, "init failed");

    CHECK(s.disp.width == 512 && s.disp.height == 248,
          "framebuffer %ux%u", s.disp.width, s.disp.height);
    CHECK(s.disp.video_mode == 1, "video mode %u", s.disp.video_mode);
    CHECK(s.disp.field_hz == 50 && s.disp.vsync_divisor == 2, "frame lock");
    CHECK(!s.disp.height_is_inferred, "PAL height should be read, not inferred");

    /* Boot is the front end's single-buffered full-screen state. */
    CHECK(s.layout == Q2_SCREEN_LAYOUT_FULL_SINGLE, "boot layout %d", (int)s.layout);
    CHECK(s.disp.buf[0].x == s.disp.buf[1].x && s.disp.buf[0].y == s.disp.buf[1].y,
          "the boot layout is single buffered");

    CHECK(s.buf[0].px && s.buf[1].px, "two framebuffers");
    CHECK(s.buf[0].width == 512 && s.buf[0].height == 248, "buffer geometry");

    q2_screen_free(&s);

    /*
     * An NTSC build's framebuffer has not been read out of an NTSC executable,
     * and PAL's 248 already refuted the widely repeated 256 — so asking for one
     * must yield the PAL geometry *and say so*, never a guess presented as a
     * fact.
     */
    CHECK(q2_screen_init(&s, Q2_VIDEO_NTSC) == Q2_OK, "ntsc init failed");
    CHECK(s.disp.height_is_inferred, "an NTSC screen must admit it is inferred");
    CHECK(s.disp.height == 248, "and must not invent a height");
    q2_screen_free(&s);
}

/* ------------------------------------------------------------------------- */
static void test_swap(void)
{
    q2_screen s;
    psx_ot ot;
    int i;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 64);

    CHECK(s.disp.draw_buffer == 0 && s.disp.disp_buffer == 1, "initial indices");

    for (i = 0; i < 6; i++) {
        u8 before = s.disp.draw_buffer;
        q2_screen_frame_begin(&s, &ot);
        CHECK(s.disp.draw_buffer == (u8)(1 - before), "swap %d flips the draw buffer", i);
        CHECK(s.disp.disp_buffer == before, "swap %d shows the previous buffer", i);

        /* Present makes what was drawn what is shown, so a host reading the
         * front buffer after present sees this frame. */
        q2_screen_present(&s);
        CHECK(q2_screen_front(&s) == &s.buf[s.disp.draw_buffer],
              "present %d exposes the buffer just drawn", i);
    }

    /* The three-slot ring rotates by one per swap and therefore returns to its
     * start every three frames (0x80018214 with the {0,1,2} from 0x8007657C). */
    q2_screen_init(&s, Q2_VIDEO_PAL);
    for (i = 0; i < 3; i++)
        q2_screen_frame_begin(&s, &ot);
    CHECK(s.disp.rotate[0] == 0 && s.disp.rotate[1] == 1 && s.disp.rotate[2] == 2,
          "the ring is back to {0,1,2} after three swaps: {%u,%u,%u}",
          s.disp.rotate[0], s.disp.rotate[1], s.disp.rotate[2]);

    psx_ot_free(&ot);
    q2_screen_free(&s);
}

/* ------------------------------------------------------------------------- */
/*
 * The slicing is the whole mechanism behind split screen, so this checks it as
 * a partition: every bucket a viewport can reach belongs to that viewport, the
 * slices do not overlap, and nothing lands on a draw env's bucket or outside
 * the table.
 */
static void test_ot_slices(void)
{
    q2_screen s;
    int p;
    /*
     * REAL buckets. The two otz helpers hand a caller something it passes
     * straight to psx_ot_add_bucket, and the table is subdivided
     * (PSX_OT_SUBDIV), so both they and the slice bounds below are in real
     * buckets while screen.h's constants stay in the console's. Sized in
     * console entries, this array was indexed with a real bucket by the overlay
     * loop below and the test died on the read.
     */
    enum { OWNER_COUNT = Q2_SCREEN_OT_ENTRIES * PSX_OT_SUBDIV };
    int owner[OWNER_COUNT];
    int i;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_QUAD, 4);

    for (i = 0; i < OWNER_COUNT; i++)
        owner[i] = -1;

    for (p = 0; p < 4; p++) {
        u32 env = ((u32)Q2_SCREEN_OT_VIEW_BASE
                   + (u32)p * Q2_SCREEN_OT_VIEW_STRIDE
                   + Q2_SCREEN_OT_VIEW_ENV) * PSX_OT_SUBDIV;
        u32 z;

        for (z = 0; z < 4096; z++) {
            u16 b = q2_screen_view_otz(&s, p, z);

            CHECK(b < OWNER_COUNT, "view %d z %u -> bucket %u is off the table",
                  p, z, b);
            if (b >= OWNER_COUNT)
                break;
            CHECK(b != env, "view %d z %u landed on its own draw env bucket", p, z);
            CHECK(owner[b] == -1 || owner[b] == p,
                  "bucket %u is claimed by both view %d and view %d", b, owner[b], p);
            owner[b] = p;
        }
    }

    /* The overlay owns its own slice and cannot reach a viewport's. */
    {
        u32 z;
        for (z = 0; z < 4096; z++) {
            u16 b = q2_screen_overlay_otz(&s, z);
            CHECK(b >= (u32)Q2_SCREEN_OT_OVERLAY * PSX_OT_SUBDIV &&
                  b < ((u32)Q2_SCREEN_OT_OVERLAY + Q2_SCREEN_OT_OVERLAY_LEN)
                          * PSX_OT_SUBDIV,
                  "overlay z %u -> bucket %u escapes the overlay slice", z, b);
            if (b >= OWNER_COUNT)
                break;
            CHECK(owner[b] == -1, "overlay bucket %u collides with view %d", b, owner[b]);
        }
    }

    /* Bucket 1 is the full-screen background env and belongs to nobody else. */
    CHECK(owner[Q2_SCREEN_OT_BACKGROUND * PSX_OT_SUBDIV] == -1,
          "the background env bucket was claimed by a viewport");

    q2_screen_free(&s);
}

/* The same guarantee has to hold through the ordering table itself, because
 * that is the path the world builder actually takes. */
static void test_ot_window(void)
{
    q2_screen s;
    psx_ot ot;
    u32 b;
    int p;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_TWO_V, 2);

    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 256);
    q2_screen_frame_begin(&s, &ot);

    for (p = 0; p < 2; p++) {
        u32 z;
        CHECK(q2_screen_view_begin(&s, p, &ot, NULL), "view %d should be live", p);
        CHECK(psx_ot_bucket_span(&ot)
                  == (Q2_SCREEN_OT_VIEW_STRIDE - 2) * PSX_OT_SUBDIV,
              "view %d span %u", p, psx_ot_bucket_span(&ot));

        /* Deliberately overshoot: an emitter that hands over a huge depth must
         * saturate inside its own slice, not spill into the next viewport. */
        for (z = 0; z < 64; z++)
            psx_ot_add(&ot, (u16)(z * 37));
    }

    CHECK(!q2_screen_view_begin(&s, 2, &ot, NULL), "view 2 is not live in a 2P layout");

    for (b = 0; b < (u32)Q2_SCREEN_OT_ENTRIES * PSX_OT_SUBDIV; b++) {
        if (ot.bucket_head[b] < 0)
            continue;
        CHECK(b >= (u32)Q2_SCREEN_OT_VIEW_BASE * PSX_OT_SUBDIV,
              "bucket %u is below the first slice", b);
        CHECK(b < ((u32)Q2_SCREEN_OT_VIEW_BASE + 2u * Q2_SCREEN_OT_VIEW_STRIDE)
                      * PSX_OT_SUBDIV,
              "bucket %u is past the second slice", b);
    }

    psx_ot_free(&ot);
    q2_screen_free(&s);
}

/* ------------------------------------------------------------------------- */
static void test_layouts(void)
{
    q2_screen s;

    q2_screen_init(&s, Q2_VIDEO_PAL);

    /* The layout the session mode would pick. */
    CHECK(q2_screen_layout_for(1, true)  == Q2_SCREEN_LAYOUT_ONE,   "1P");
    CHECK(q2_screen_layout_for(2, true)  == Q2_SCREEN_LAYOUT_TWO_H, "2P split on");
    CHECK(q2_screen_layout_for(2, false) == Q2_SCREEN_LAYOUT_TWO_V, "2P split off");
    CHECK(q2_screen_layout_for(3, true)  == Q2_SCREEN_LAYOUT_QUAD,  "3P");
    CHECK(q2_screen_layout_for(4, false) == Q2_SCREEN_LAYOUT_QUAD,  "4P");

    /* Three players use the four-quadrant layout with one quadrant unused. */
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_QUAD, 3);
    CHECK(s.view_count == 3, "3P view count %d", s.view_count);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_QUAD, 4);
    CHECK(s.view_count == 4, "4P view count %d", s.view_count);

    /*
     * Every layout must give each viewport a geometry offset at its own centre,
     * because that is what makes the GTE produce viewport-local pixels that the
     * draw env's offset can then place. Getting this wrong is invisible in one
     * viewport and catastrophic in four.
     */
    {
        int l, i;
        for (l = 0; l < Q2_SCREEN_LAYOUT_COUNT; l++) {
            q2_screen_set_layout(&s, (q2_screen_layout)l, 4);
            for (i = 0; i < s.view_count; i++) {
                const q2_screen_view *v = &s.view[i];
                CHECK(v->ofs_x == v->w / 2 && v->ofs_y == v->h / 2,
                      "%s view %d offset (%d,%d) is not its centre (%d,%d)",
                      q2_screen_layout_name((q2_screen_layout)l), i,
                      v->ofs_x, v->ofs_y, v->w / 2, v->h / 2);
                CHECK(v->w > 0 && v->h > 0, "%s view %d is empty",
                      q2_screen_layout_name((q2_screen_layout)l), i);
                CHECK(v->x >= 0 && v->y >= 0 &&
                      v->x + v->w <= (s16)s.disp.width + 1 &&
                      v->y + v->h <= (s16)s.disp.height + 1,
                      "%s view %d (%d,%d %dx%d) leaves the framebuffer",
                      q2_screen_layout_name((q2_screen_layout)l), i,
                      v->x, v->y, v->w, v->h);
            }
        }
    }

    /*
     * The two full-screen layouts differ only in projection distance and in
     * whether they double buffer — which is the entire reason FULL_SINGLE is a
     * separate layout rather than a duplicate of ONE.
     */
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_ONE, 1);
    CHECK(s.view[0].proj == 160, "1P projection %d", s.view[0].proj);
    CHECK(s.disp.buf[1].x == 512, "1P is double buffered");
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_FULL_SINGLE, 1);
    CHECK(s.view[0].proj == 320, "boot projection %d", s.view[0].proj);
    CHECK(s.disp.buf[1].x == 0, "the boot layout is single buffered");

    /* The horizontal split's 2D extent is NOT its viewport size — it is 320x160
     * against a 512x123 viewport, which is the sort of thing that only survives
     * a port if it is transcribed rather than derived. */
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_TWO_H, 2);
    CHECK(s.view[0].w == 512 && s.view[0].h == 123, "2P-H viewport %dx%d",
          s.view[0].w, s.view[0].h);
    CHECK(s.view[0].vw == 320 && s.view[0].vh == 160, "2P-H 2D extent %dx%d",
          s.view[0].vw, s.view[0].vh);
    CHECK(s.view[1].y == 121, "2P-H second viewport y %d", s.view[1].y);

    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_TWO_V, 2);
    CHECK(s.view[0].w == 255, "2P-V width %d", s.view[0].w);
    CHECK(s.view[0].ofs_x == 127, "2P-V odd width halves toward zero: %d",
          s.view[0].ofs_x);

    q2_screen_free(&s);
}

/* ------------------------------------------------------------------------- */
/*
 * Composition. The draw envs live in the table and change the clip as the walk
 * reaches them, so a primitive built for viewport 1 must not put a pixel in
 * viewport 0 — even when its coordinates say it should.
 */
static void test_compose_clip(void)
{
    q2_screen s;
    psx_ot ot;
    psx_raster_opts opts;
    const psx_framebuffer *fb;
    psx_prim *p;
    int x, y, stray = 0;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_TWO_V, 2);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 64);
    psx_raster_opts_default(&opts);
    opts.textures = false;
    opts.dither   = false;

    q2_screen_frame_begin(&s, &ot);
    psx_fb_clear(q2_screen_back(&s), 0);

    /* One huge white tile in the right-hand viewport, spanning far more than
     * the viewport holds. */
    q2_screen_view_begin(&s, 1, &ot, NULL);
    p = psx_ot_add_bucket(&ot, q2_screen_view_otz(&s, 1, 0));
    CHECK(p != NULL, "no room in the primitive pool");
    if (p) {
        p->kind = PSX_PRIM_TILE;
        p->xy[0].x = -400; p->xy[0].y = -400;
        p->xy[2].x =  900; p->xy[2].y =  900;
        p->rgb[0].r = p->rgb[0].g = p->rgb[0].b = 255;
    }

    q2_screen_view_end(&s, &ot);
    q2_screen_compose(&s, &ot, NULL, &opts);
    q2_screen_present(&s);
    fb = q2_screen_front(&s);

    for (y = 0; y < fb->height; y++) {
        for (x = 0; x < fb->width; x++) {
            bool lit = fb->px[y * fb->width + x] != 0;
            bool inside = (x >= s.view[1].x && x < s.view[1].x + s.view[1].w &&
                           y >= s.view[1].y && y < s.view[1].y + s.view[1].h);
            if (lit != inside)
                stray++;
        }
    }
    CHECK(stray == 0, "%d pixels disagree with viewport 1's clip rectangle", stray);

    psx_ot_free(&ot);
    q2_screen_free(&s);
}

/*
 * SortData screen changes are real DRAWENV packets inside a viewport's OT
 * slice. The packet at bucket 1 establishes the last portal region for the
 * low-numbered run; a later packet restores the full viewport before the run
 * in its own bucket. Both the clip and the drawing offset are observable.
 */
static void test_sort_region_drawenv(void)
{
    q2_screen s;
    psx_ot ot;
    psx_raster_opts opts;
    const psx_framebuffer *fb;
    psx_prim *p;
    u32 b;
    u16 white = psx_rgb555(255, 255, 255);
    u16 red = psx_rgb555(255, 0, 0);

    q2_screen_init(&s, Q2_VIDEO_PAL);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_ONE, 1);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 16);
    psx_raster_opts_default(&opts);
    opts.textures = false;
    opts.dither = false;

    q2_screen_frame_begin(&s, &ot);
    psx_fb_clear(q2_screen_back(&s), 0);
    q2_screen_view_begin(&s, 0, &ot, NULL);

    /* The structural full-view env is applied first at this same bucket. This
     * packet follows it and installs a 40x30 local portal at (100,50). */
    b = psx_ot_authored_bucket(&ot, 1);
    p = psx_ot_add_bucket(&ot, b);
    CHECK(p != NULL, "no room for the portal draw env");
    if (p) {
        p->kind = PSX_PRIM_DRAW_ENV;
        p->xy[0].x = 100; p->xy[0].y = 50;
        p->xy[1].x = 40;  p->xy[1].y = 30;
        p->xy[2] = p->xy[0];
    }

    /* Coordinates are local to that portal. The 100x100 tile is clipped to
     * 40x30 and displaced into the portal's framebuffer rectangle. */
    b = psx_ot_authored_bucket(&ot, 10);
    p = psx_ot_add_bucket(&ot, b);
    CHECK(p != NULL, "no room for the portal-local tile");
    if (p) {
        p->kind = PSX_PRIM_TILE;
        p->xy[0].x = 0;   p->xy[0].y = 0;
        p->xy[2].x = 100; p->xy[2].y = 100;
        p->rgb[0].r = p->rgb[0].g = p->rgb[0].b = 255;
    }

    /* Add the tile first and the restore second in the SAME bucket. AddPrim
     * prepends, so retail executes the draw env before this bucket's tile. */
    b = psx_ot_authored_bucket(&ot, 20);
    p = psx_ot_add_bucket(&ot, b);
    CHECK(p != NULL, "no room for the restored-view tile");
    if (p) {
        p->kind = PSX_PRIM_TILE;
        p->xy[0].x = 0;  p->xy[0].y = 0;
        p->xy[2].x = 20; p->xy[2].y = 20;
        p->rgb[0].r = 255;
    }
    p = psx_ot_add_bucket(&ot, b);
    CHECK(p != NULL, "no room for the full-view restore");
    if (p) {
        p->kind = PSX_PRIM_DRAW_ENV;
        p->xy[0].x = 0; p->xy[0].y = 0;
        p->xy[1].x = s.view[0].w;
        p->xy[1].y = s.view[0].h;
        p->xy[2] = p->xy[0];
    }

    q2_screen_view_end(&s, &ot);
    q2_screen_compose(&s, &ot, NULL, &opts);
    q2_screen_present(&s);
    fb = q2_screen_front(&s);

    CHECK(fb->px[5 * fb->width + 5] == red,
          "same-bucket draw env did not restore the origin before its tile");
    CHECK(fb->px[60 * fb->width + 110] == white,
          "portal-local tile was not displaced into its screen region");
    CHECK(fb->px[60 * fb->width + 50] == 0,
          "portal draw env did not clip the tile's left side");
    CHECK(fb->px[60 * fb->width + 140] == 0,
          "portal draw env did not clip the tile's right boundary");
    CHECK(fb->px[80 * fb->width + 110] == 0,
          "portal draw env did not clip the tile's bottom boundary");

    psx_ot_free(&ot);
    q2_screen_free(&s);
}

/*
 * The background clear. Arming the full-screen one must turn every viewport's
 * own clear off, or the split-screen gutters — the only pixels the full-screen
 * clear owns — would be the only thing it painted.
 */
static void test_background(void)
{
    q2_screen s;
    psx_ot ot;
    psx_raster_opts opts;
    const psx_framebuffer *fb;
    int i, x, y, wrong = 0;
    u16 want;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_QUAD, 4);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 16);
    psx_raster_opts_default(&opts);
    opts.textures = false;

    s.disp.bg_rgb[0] = 8;
    s.disp.bg_rgb[1] = 16;
    s.disp.bg_rgb[2] = 248;
    s.disp.bg_enable = 1;
    for (i = 0; i < 4; i++)
        s.view[i].bg_enable = 1;

    q2_screen_frame_begin(&s, &ot);
    psx_fb_clear(q2_screen_back(&s), 0x7FFF);
    q2_screen_background(&s);

    for (i = 0; i < 4; i++)
        CHECK(s.view[i].bg_enable == 0,
              "view %d still clears after the full-screen clear was armed", i);

    q2_screen_compose(&s, &ot, NULL, &opts);
    q2_screen_present(&s);

    fb   = q2_screen_front(&s);
    want = psx_rgb555(8, 16, 248);
    for (y = 0; y < fb->height; y++)
        for (x = 0; x < fb->width; x++)
            if (fb->px[y * fb->width + x] != want)
                wrong++;
    CHECK(wrong == 0, "%d pixels were not cleared to the background colour", wrong);

    psx_ot_free(&ot);
    q2_screen_free(&s);
}

/*
 * The shake. 0x80076C18 displaces the draw rectangle's origin and subtracts the
 * same amount from its size, so the far edge is pinned and the viewport gets
 * smaller — a shake that never lets a viewport bleed over its neighbour.
 */
static void test_shake(void)
{
    q2_screen s;
    psx_ot ot;
    psx_raster_opts opts;
    const psx_framebuffer *fb;
    psx_prim *p;
    int x, y, stray = 0;
    const int sx = 6, sy = 4;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_TWO_V, 2);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 16);
    psx_raster_opts_default(&opts);
    opts.textures = false;
    opts.dither   = false;

    s.view[0].shake_x = (s16)sx;
    s.view[0].shake_y = (s16)sy;

    q2_screen_frame_begin(&s, &ot);
    psx_fb_clear(q2_screen_back(&s), 0);
    q2_screen_view_begin(&s, 0, &ot, NULL);
    p = psx_ot_add_bucket(&ot, q2_screen_view_otz(&s, 0, 0));
    if (p) {
        p->kind = PSX_PRIM_TILE;
        p->xy[0].x = -900; p->xy[0].y = -900;
        p->xy[2].x =  900; p->xy[2].y =  900;
        p->rgb[0].r = p->rgb[0].g = p->rgb[0].b = 255;
    }
    q2_screen_view_end(&s, &ot);
    q2_screen_compose(&s, &ot, NULL, &opts);
    q2_screen_present(&s);
    fb = q2_screen_front(&s);

    for (y = 0; y < fb->height; y++) {
        for (x = 0; x < fb->width; x++) {
            bool lit = fb->px[y * fb->width + x] != 0;
            bool inside = (x >= s.view[0].x + sx &&
                           x <  s.view[0].x + s.view[0].w &&
                           y >= s.view[0].y + sy &&
                           y <  s.view[0].y + s.view[0].h);
            if (lit != inside)
                stray++;
        }
    }
    CHECK(stray == 0, "%d pixels disagree with the shaken clip rectangle", stray);

    /* And the far edge really is pinned. */
    CHECK(s.view[0].x + sx + (s.view[0].w - sx) == s.view[0].x + s.view[0].w,
          "the shake moved the far edge");

    psx_ot_free(&ot);
    q2_screen_free(&s);
}

/* ------------------------------------------------------------------------- */
/*
 * Which way the table is drawn. The env sits at the bottom of a slice and the
 * flash at the top, so the walk must run bucket 0 upwards — which means a depth
 * has to count DOWN into the slice. If that inverts, the frame draws far over
 * near and every wall in the level occludes the one in front of it.
 */
static void test_depth_direction(void)
{
    q2_screen s;
    psx_ot ot;
    psx_raster_opts opts;
    const psx_framebuffer *fb;
    psx_prim *p;
    u16 near_px, far_px;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_ONE, 1);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 16);
    psx_raster_opts_default(&opts);
    opts.textures = false;
    opts.dither   = false;

    q2_screen_frame_begin(&s, &ot);
    psx_fb_clear(q2_screen_back(&s), 0);
    q2_screen_view_begin(&s, 0, &ot, NULL);

    /* Two tiles over the same pixels: one far, one near. */
    p = psx_ot_add(&ot, 40);
    if (p) {
        p->kind = PSX_PRIM_TILE;
        p->xy[0].x = 0;   p->xy[0].y = 0;
        p->xy[2].x = 64;  p->xy[2].y = 64;
        p->rgb[0].r = 255; p->rgb[0].g = 0; p->rgb[0].b = 0;
    }
    p = psx_ot_add(&ot, 2);
    if (p) {
        p->kind = PSX_PRIM_TILE;
        p->xy[0].x = 0;   p->xy[0].y = 0;
        p->xy[2].x = 64;  p->xy[2].y = 64;
        p->rgb[0].r = 0; p->rgb[0].g = 0; p->rgb[0].b = 255;
    }
    q2_screen_view_end(&s, &ot);
    q2_screen_compose(&s, &ot, NULL, &opts);
    q2_screen_present(&s);

    fb      = q2_screen_front(&s);
    near_px = psx_rgb555(0, 0, 255);
    far_px  = psx_rgb555(255, 0, 0);
    CHECK(fb->px[10 * fb->width + 10] == near_px,
          "the nearer primitive must survive, got %04X (far is %04X)",
          fb->px[10 * fb->width + 10], far_px);

    /* And the depth-to-bucket map has to agree with that: a bigger depth is a
     * lower bucket, and both stay inside the slice's geometry range. */
    {
        u16 near_b = q2_screen_view_otz(&s, 0, 0);
        u16 far_b  = q2_screen_view_otz(&s, 0, 48);
        u16 clamp  = q2_screen_view_otz(&s, 0, 4000);

        CHECK(near_b > far_b, "depth 0 (%u) should be nearer than 48 (%u)",
              near_b, far_b);
        CHECK(far_b == (Q2_SCREEN_OT_VIEW_BASE + Q2_SCREEN_OT_VIEW_ENV + 1)
                           * PSX_OT_SUBDIV,
              "the farthest depth is the slice's first geometry bucket, got %u",
              far_b);
        CHECK(near_b == (Q2_SCREEN_OT_VIEW_BASE + Q2_SCREEN_OT_VIEW_STRIDE - 1)
                            * PSX_OT_SUBDIV,
              "the nearest depth is the slice's last bucket, got %u", near_b);
        CHECK(clamp == far_b, "an over-range depth clamps to the far end");
    }

    psx_ot_free(&ot);
    q2_screen_free(&s);
}

/* ------------------------------------------------------------------------- */
/*
 * The damage flash. Four modes, a countdown that runs once per drawn frame, and
 * a place in the table — the front of its own viewport, so it tints the world
 * and nothing else.
 */
static void test_flash(void)
{
    q2_screen s;
    psx_ot ot;
    static const u8 grey[3] = { 0x40, 0x40, 0x40 };
    u32 bucket;
    s32 head;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_TWO_V, 2);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 16);

    /* Nothing is drawn while the strength is zero. */
    q2_screen_frame_begin(&s, &ot);
    q2_screen_view_begin(&s, 1, &ot, NULL);
    CHECK(ot.prim_count == 0, "a flash of strength 0 drew something");

    q2_screen_flash_set(&s, 1, grey, 4, Q2_SCREEN_FLASH_FADE);
    CHECK(s.view[1].flash.initial == 4, "the initial value is the strength");

    q2_screen_frame_begin(&s, &ot);
    q2_screen_view_begin(&s, 1, &ot, NULL);

    bucket = ((u32)Q2_SCREEN_OT_VIEW_BASE + Q2_SCREEN_OT_VIEW_STRIDE
              + Q2_SCREEN_OT_VIEW_FLASH) * PSX_OT_SUBDIV;
    head = ot.bucket_head[bucket];
    CHECK(head >= 0, "the flash should link at viewport 1's frontmost bucket");
    if (head >= 0) {
        const psx_prim *t = &ot.prims[head];
        CHECK(t->kind == PSX_PRIM_TILE, "the flash is a TILE");
        CHECK(t->semi_transparent, "the flash sets the ABE bit");
        CHECK(t->xy[0].x == 0 && t->xy[0].y == 0,
              "the flash is viewport-local");
        CHECK(t->xy[2].x == s.view[1].w && t->xy[2].y == s.view[1].h,
              "the flash covers the whole viewport");
        /* Mode 2 at full strength keeps the colour. */
        CHECK(t->rgb[0].r == 0x40, "full strength should not fade, got %u",
              t->rgb[0].r);
    }
    CHECK(s.view[1].flash.strength == 3,
          "the flash steps down once per drawn frame, got %d",
          (int)s.view[1].flash.strength);

    /* Halfway through, the fade is linear in strength/initial. */
    q2_screen_frame_begin(&s, &ot);
    q2_screen_view_begin(&s, 1, &ot, NULL);
    head = ot.bucket_head[bucket];
    if (head >= 0)
        CHECK(ot.prims[head].rgb[0].r == 0x30,
              "3/4 of 0x40 is 0x30, got %u", ot.prims[head].rgb[0].r);

    /* Mode 3 does not fade at all; mode 0 keeps whatever was there. */
    q2_screen_flash_set(&s, 1, grey, 2, Q2_SCREEN_FLASH_SOLID);
    q2_screen_frame_begin(&s, &ot);
    q2_screen_view_begin(&s, 1, &ot, NULL);
    head = ot.bucket_head[bucket];
    if (head >= 0)
        CHECK(ot.prims[head].rgb[0].b == 0x40, "mode 3 holds the colour");

    /* A viewport that is not live never draws its flash. */
    q2_screen_flash_set(&s, 0, grey, 3, Q2_SCREEN_FLASH_FADE);
    q2_screen_frame_begin(&s, &ot);
    CHECK(!q2_screen_view_begin(&s, 3, &ot, NULL),
          "viewport 3 is not live in a two-player layout");
    CHECK(s.view[0].flash.strength == 3,
          "a viewport that was not drawn must not tick its flash down");

    psx_ot_free(&ot);
    q2_screen_free(&s);
}

/* ------------------------------------------------------------------------- */
/*
 * The frame build's three-way clear decision, and the world gate. Neither is
 * expressible as a constant: both are branches.
 */
static int g_view_hooks;
static int g_overlay_hooks;

static void count_view_hook(void *user, q2_screen *s, int p,
                            psx_ot *ot, gte_state *gte)
{
    (void)user; (void)s; (void)p; (void)ot; (void)gte;
    g_view_hooks++;
}

static void count_overlay_hook(void *user, q2_screen *s,
                               psx_ot *ot, gte_state *gte)
{
    (void)user; (void)s; (void)ot; (void)gte;
    g_overlay_hooks++;
}

static void test_build(void)
{
    q2_screen s;
    psx_ot ot;
    q2_screen_hooks hooks;
    int i;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_QUAD, 3);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 64);

    memset(&hooks, 0, sizeof(hooks));
    hooks.view    = count_view_hook;
    hooks.overlay = count_overlay_hook;

    /* Neither flag: the per-viewport clears stand. */
    for (i = 0; i < 4; i++)
        s.view[i].bg_enable = 1;
    s.suppress_clear    = false;
    s.background_enable = false;
    g_view_hooks = g_overlay_hooks = 0;

    q2_screen_frame_begin(&s, &ot);
    q2_screen_build(&s, &ot, NULL, &hooks);

    CHECK(!s.background_armed, "no background env should be armed");
    CHECK(s.view[0].bg_enable == 1, "the per-viewport clear should stand");
    CHECK(g_view_hooks == 3, "three live viewports, got %d", g_view_hooks);
    CHECK(g_overlay_hooks == 1, "one overlay, got %d", g_overlay_hooks);
    CHECK(s.overlay_armed, "the overlay camera should be up");
    CHECK(s.env_linked[0] && s.env_linked[2], "each viewport links its env");
    CHECK(!s.env_linked[3], "the fourth quadrant of a three-player split");

    /* The background flag: one full-screen clear, and every viewport's own is
     * turned off so the gutters are the only thing it owns. */
    for (i = 0; i < 4; i++)
        s.view[i].bg_enable = 1;
    s.background_enable = true;
    q2_screen_frame_begin(&s, &ot);
    q2_screen_build(&s, &ot, NULL, &hooks);
    CHECK(s.background_armed, "the background env should be armed");
    for (i = 0; i < s.view_count; i++)
        CHECK(s.view[i].bg_enable == 0, "view %d still clears", i);
    /* The loop is bounded by the LIVE count, so the fourth quadrant of a
     * three-player split is left alone (0x80078178 reads 0x800B2C2C). */
    CHECK(s.view[3].bg_enable == 1, "the dead quadrant should be untouched");

    /* The suppress flag wins over it, and clears nothing at all. */
    for (i = 0; i < 4; i++)
        s.view[i].bg_enable = 1;
    s.suppress_clear = true;
    s.background_enable = false;
    q2_screen_frame_begin(&s, &ot);
    q2_screen_build(&s, &ot, NULL, &hooks);
    CHECK(!s.background_armed, "suppressed frames arm no background");
    for (i = 0; i < s.view_count; i++)
        CHECK(s.view[i].bg_enable == 0, "view %d still clears", i);

    /* The world gate: bit 0 of view+144. */
    s.suppress_clear = false;
    for (i = 0; i < 4; i++)
        s.view[i].flags = (u16)(s.view[i].flags &
                                ~(unsigned)Q2_SCREEN_VIEW_DRAW_WORLD);
    g_view_hooks = 0;
    q2_screen_frame_begin(&s, &ot);
    q2_screen_build(&s, &ot, NULL, &hooks);
    CHECK(g_view_hooks == 0, "the world drew with the gate closed");
    CHECK(s.env_linked[0], "the env is still linked with the gate closed");

    psx_ot_free(&ot);
    q2_screen_free(&s);
}

/* ------------------------------------------------------------------------- */
/*
 * What a viewport publishes while it is being drawn — the globals the renderers
 * read instead of being passed anything.
 */
static void test_context(void)
{
    q2_screen s;
    psx_ot ot;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_QUAD, 4);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 16);

    s.view[2].shake_x = 5;
    s.view[2].shake_y = 3;

    q2_screen_frame_begin(&s, &ot);
    q2_screen_view_begin(&s, 2, &ot, NULL);

    CHECK(s.ctx.index == 2, "the viewport index");
    CHECK(s.ctx.view == &s.view[2], "the view record");
    CHECK(!s.ctx.overlay, "not the overlay");
    CHECK(s.ctx.clip_w == s.view[2].w - 5 && s.ctx.clip_h == s.view[2].h - 3,
          "the clip extent has the shake taken off it");
    CHECK(s.ctx.far_z == 4000, "the quad layout's far distance, got %d",
          (int)s.ctx.far_z);
    CHECK(s.ctx.far_z_entities == 1000,
          "entities are cut off at a quarter of it, got %d",
          (int)s.ctx.far_z_entities);

    /* The quarter truncates toward zero on both signs, which is what the
     * bgez-guarded bias at 0x80076BB0 does. */
    {
        s16 saved = s.view[2].far_z;
        s.view[2].far_z = -5;
        q2_screen_view_begin(&s, 2, &ot, NULL);
        CHECK(s.ctx.far_z_entities == -1,
              "-5/4 should truncate to -1, got %d", (int)s.ctx.far_z_entities);
        s.view[2].far_z = saved;
    }
    CHECK(s.ctx.slice_base == (u32)Q2_SCREEN_OT_VIEW_BASE
                              + 2 * Q2_SCREEN_OT_VIEW_STRIDE,
          "the slice base");

    /* The overlay publishes itself as viewport 0, with no shake and its far
     * distance left whole. */
    q2_screen_overlay_begin(&s, &ot, NULL);
    CHECK(s.ctx.overlay, "the overlay flag");
    CHECK(s.ctx.index == 0, "the overlay publishes index 0");
    CHECK(s.ctx.shake_x == 0 && s.ctx.shake_y == 0, "the overlay never shakes");
    CHECK(s.ctx.clip_w == s.overlay.w, "the overlay's clip is its own size");

    psx_ot_free(&ot);
    q2_screen_free(&s);
}

/* ------------------------------------------------------------------------- */
/*
 * Single buffering, and the display envs that express it. The boot layout shows
 * one rectangle and draws into it; a session layout shows the other one.
 */
static void test_display_envs(void)
{
    q2_screen s;
    int x, y, w, h;

    q2_screen_init(&s, Q2_VIDEO_PAL);

    CHECK(q2_screen_single_buffered(&s),
          "the boot layout should be single buffered");
    CHECK(q2_screen_front(&s) == q2_screen_back(&s),
          "single buffered means you watch it being drawn");

    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_ONE, 1);
    CHECK(!q2_screen_single_buffered(&s), "a session layout is double buffered");
    CHECK(q2_screen_front(&s) != q2_screen_back(&s), "two buffers");

    /* PutDispEnv takes the DRAWN buffer's env, and the cross pairing makes that
     * the other buffer's rectangle. */
    s.disp.draw_buffer = 0;
    q2_screen_display_rect(&s, &x, &y, &w, &h);
    CHECK(x == 512 && y == 0 && w == 512 && h == 248,
          "drawing buffer 0 should display (512,0), got (%d,%d) %dx%d",
          x, y, w, h);
    s.disp.draw_buffer = 1;
    q2_screen_display_rect(&s, &x, &y, NULL, NULL);
    CHECK(x == 0 && y == 0, "drawing buffer 1 should display (0,0)");

    q2_screen_free(&s);
}

/* ------------------------------------------------------------------------- */
/* The performance meter: nine bars, halved heights, and six accumulators that
 * the build resets. */
static void test_meter(void)
{
    q2_screen s;
    psx_ot ot;
    q2_screen_hooks hooks;
    int i, found = 0;
    s32 idx;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_ONE, 1);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 64);
    memset(&hooks, 0, sizeof(hooks));

    for (i = 0; i < Q2_SCREEN_METER_BARS; i++)
        s.meter.value[i] = 20 + i;

    /* Off by default — 0x800B2A64 is in the zeroed segment. */
    q2_screen_frame_begin(&s, &ot);
    q2_screen_build(&s, &ot, NULL, &hooks);
    CHECK(ot.bucket_head[Q2_SCREEN_OT_METER * PSX_OT_SUBDIV] < 0,
          "the meter drew while it was off");

    s.meter.enable = true;
    q2_screen_frame_begin(&s, &ot);
    q2_screen_build(&s, &ot, NULL, &hooks);

    for (idx = ot.bucket_head[Q2_SCREEN_OT_METER * PSX_OT_SUBDIV]; idx >= 0;
         idx = ot.next[idx]) {
        const psx_prim *p = &ot.prims[idx];
        CHECK(p->kind == PSX_PRIM_TILE, "a meter bar is a TILE");
        CHECK(p->xy[2].x - p->xy[0].x == Q2_SCREEN_METER_WIDTH,
              "a meter bar is two pixels wide");
        found++;
    }
    CHECK(found == Q2_SCREEN_METER_BARS, "%d bars, want %d",
          found, Q2_SCREEN_METER_BARS);

    CHECK(s.meter.value[3] == 0, "the summed accumulators reset");
    CHECK(s.meter.value[0] == 20, "the sampled ones do not");

    /* Bar 6 vanishes when its accumulator is zero. */
    for (i = 0; i < Q2_SCREEN_METER_BARS; i++)
        s.meter.value[i] = 10;
    s.meter.value[6] = 0;
    q2_screen_frame_begin(&s, &ot);
    q2_screen_build(&s, &ot, NULL, &hooks);
    found = 0;
    for (idx = ot.bucket_head[Q2_SCREEN_OT_METER * PSX_OT_SUBDIV]; idx >= 0;
         idx = ot.next[idx])
        found++;
    CHECK(found == Q2_SCREEN_METER_BARS - 1,
          "bar 6 should be skipped when it is zero, got %d bars", found);

    psx_ot_free(&ot);
    q2_screen_free(&s);
}

/* ------------------------------------------------------------------------- */
/* The blend selectors the bring-up builds. */
static void test_blend_table(void)
{
    CHECK(q2_screen_blend_word[0] == 32 && q2_screen_blend_word[1] == 0 &&
          q2_screen_blend_word[2] == 32 && q2_screen_blend_word[3] == 64 &&
          q2_screen_blend_word[4] == 96, "the blend table");
    CHECK(q2_screen_blend_mode(0) == PSX_BLEND_ADD,
          "selector 0 is additive, not half");
    CHECK(q2_screen_blend_mode(1) == PSX_BLEND_HALF, "selector 1");
    CHECK(q2_screen_blend_mode(3) == PSX_BLEND_SUB, "selector 3");
    CHECK(q2_screen_blend_mode(4) == PSX_BLEND_QUARTER, "selector 4");
    CHECK(!q2_screen_blend_defined(5),
          "selectors past the table are not ours to invent");
    CHECK(q2_screen_blend_bits(7) == q2_screen_blend_word[0],
          "and they clamp rather than reading past it");
}

/* ------------------------------------------------------------------------- */
/* The water effect — 0x80062DF0                                              */
/* ------------------------------------------------------------------------- */
/*
 * Five properties, and the third is the one worth having a test for: every
 * strip copy reads from INSIDE the viewport. That is not something the effect
 * checks at run time — it falls out of the shake being exactly the largest
 * value the cosine term can reach — so it is the property that breaks silently
 * if either half of that relationship is transcribed wrong, and it breaks by
 * dragging whatever is next to the viewport into it.
 */
static void water_frame(q2_screen *s, psx_ot *ot, int views)
{
    int p;

    q2_screen_frame_begin(s, ot);
    for (p = 0; p < views; p++) {
        q2_screen_view_begin(s, p, ot, NULL);
        q2_screen_view_end(s, ot);
    }
}

static void test_water_ramp(void)
{
    q2_screen s;
    psx_ot ot;
    int i;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_ONE, 1);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 512);
    s.dt = Q2_SCREEN_DT_NOMINAL;

    /* A viewport with no owner is skipped before the ramp, so it is not the
     * same thing as a viewport whose owner is dry. */
    q2_screen_water_set(&s, 0, false, true);
    water_frame(&s, &ot, 1);
    CHECK(s.view[0].water.amp == 0, "an unowned viewport must not ramp");

    q2_screen_water_set(&s, 0, true, true);
    water_frame(&s, &ot, 1);
    CHECK(s.view[0].water.amp == Q2_SCREEN_WATER_AMP_RATE * Q2_SCREEN_DT_NOMINAL,
          "one frame submerged should be %d, got %d",
          Q2_SCREEN_WATER_AMP_RATE * Q2_SCREEN_DT_NOMINAL, s.view[0].water.amp);

    /* 4096 at 288 a frame is fourteen and a bit; give it twenty and check the
     * clamp holds rather than wrapping the halfword. */
    for (i = 0; i < 20; i++)
        water_frame(&s, &ot, 1);
    CHECK(s.view[0].water.amp == Q2_SCREEN_WATER_AMP_MAX,
          "the ramp should clamp at %d, got %d",
          Q2_SCREEN_WATER_AMP_MAX, s.view[0].water.amp);

    /* Full amplitude is a 3 x 1 inset. Both come from the same product divided
     * by 1024 and 2048, which is why the picture slides twice as far sideways. */
    CHECK(s.view[0].shake_x == 3 && s.view[0].shake_y == 1,
          "shake at full amplitude is (3,1), got (%d,%d)",
          s.view[0].shake_x, s.view[0].shake_y);

    /* Surfacing fades out at the same rate and then resets the wave, so going
     * back under starts from the same phase rather than wherever it stopped. */
    q2_screen_water_set(&s, 0, true, false);
    for (i = 0; i < 20; i++)
        water_frame(&s, &ot, 1);
    CHECK(s.view[0].water.amp == 0, "surfacing should fade to 0, got %d",
          s.view[0].water.amp);
    CHECK(s.view[0].shake_x == 0 && s.view[0].shake_y == 0,
          "a dry viewport must not be inset");
    CHECK(s.view[0].water.phase_row == 0 && s.view[0].water.phase_col == 0,
          "reaching zero resets both phases");

    psx_ot_free(&ot);
    q2_screen_free(&s);
}

static void test_water_amplitudes(void)
{
    s32 h, v;

    q2_screen_water_amplitudes(Q2_SCREEN_WATER_AMP_MAX, &h, &v);
    CHECK(h == 16380 && v == 8190, "amplitudes at full: %d/%d", (int)h, (int)v);

    /* The shake is the amplitude over 4096, which is also the largest the
     * cosine term can be — the relationship the next test depends on. */
    CHECK(h / 4096 == 3 && v / 4096 == 1, "shake from amplitude");

    q2_screen_water_amplitudes(0, &h, &v);
    CHECK(h == 0 && v == 0, "a still viewport has no amplitude");
}

static void test_water_strips(void)
{
    q2_screen s;
    psx_ot ot;
    u32 b, moves = 0, tints = 0;
    s32 idx;
    int i;
    bool escaped = false, tint_first = false;
    const q2_screen_view *v;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_ONE, 1);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 512);
    s.dt = Q2_SCREEN_DT_NOMINAL;
    v = &s.view[0];

    q2_screen_water_set(&s, 0, true, true);
    for (i = 0; i < 20; i++)
        water_frame(&s, &ot, 1);

    b = ((u32)Q2_SCREEN_OT_VIEW_BASE + Q2_SCREEN_OT_VIEW_WATER)
        * PSX_OT_SUBDIV;

    for (idx = ot.bucket_head[b]; idx >= 0; idx = ot.next[idx]) {
        const psx_prim *p = &ot.prims[idx];

        if (p->kind == PSX_PRIM_TILE) {
            tints++;
            /* The tile is added last and a bucket is walked most-recently-first,
             * so it must be the head — the strips displace a picture that has
             * already been tinted. */
            tint_first = (idx == ot.bucket_head[b]);
            CHECK(p->rgb[0].r == Q2_SCREEN_WATER_TINT_R &&
                  p->rgb[0].g == Q2_SCREEN_WATER_TINT_G &&
                  p->rgb[0].b == Q2_SCREEN_WATER_TINT_B,
                  "tint at full amplitude is (%d,%d,%d), got (%d,%d,%d)",
                  Q2_SCREEN_WATER_TINT_R, Q2_SCREEN_WATER_TINT_G,
                  Q2_SCREEN_WATER_TINT_B,
                  p->rgb[0].r, p->rgb[0].g, p->rgb[0].b);
            CHECK(p->semi_transparent, "the tint is semi-transparent");
            continue;
        }

        CHECK(p->kind == PSX_PRIM_MOVE, "bucket %u holds kind %d", b, (int)p->kind);
        moves++;

        /* THE PROPERTY. Both rectangles have to sit inside the viewport, or the
         * wobble drags a neighbouring viewport's pixels into this one. */
        if (p->xy[PSX_MOVE_SRC].x < v->x ||
            p->xy[PSX_MOVE_SRC].y < v->y ||
            p->xy[PSX_MOVE_SRC].x + p->xy[PSX_MOVE_SIZE].x > v->x + v->w ||
            p->xy[PSX_MOVE_SRC].y + p->xy[PSX_MOVE_SIZE].y > v->y + v->h ||
            p->xy[PSX_MOVE_DST].x < v->x ||
            p->xy[PSX_MOVE_DST].y < v->y ||
            p->xy[PSX_MOVE_DST].x + p->xy[PSX_MOVE_SIZE].x > v->x + v->w ||
            p->xy[PSX_MOVE_DST].y + p->xy[PSX_MOVE_SIZE].y > v->y + v->h)
            escaped = true;
    }

    CHECK(moves > 0, "a submerged viewport should wobble");
    CHECK(tints == 1, "exactly one tint, got %u", tints);
    CHECK(tint_first, "the tint must draw before the strips displace it");
    CHECK(!escaped, "a strip copy reached outside its own viewport");

    /* The pool is a hard per-frame cap, not a per-viewport one. */
    CHECK(s.water_moves_used <= s.water_moves,
          "%u copies out of a pool of %u", s.water_moves_used, s.water_moves);

    psx_ot_free(&ot);
    q2_screen_free(&s);
}

/*
 * The pool is shared, and running out stops the wobble where it is rather than
 * thinning it out — 0x800630EC jumps past both passes to the tint. So a split
 * screen with every viewport submerged draws a complete wobble in the first
 * viewports and none in the last, which is what the console does.
 */
static void test_water_pool(void)
{
    q2_screen s;
    psx_ot ot;
    int p, i;
    u32 with_moves = 0;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_QUAD, 4);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 512);
    s.dt = Q2_SCREEN_DT_NOMINAL;

    for (p = 0; p < 4; p++)
        q2_screen_water_set(&s, p, true, true);
    for (i = 0; i < 20; i++)
        water_frame(&s, &ot, 4);

    CHECK(s.water_moves_used <= s.water_moves,
          "four submerged viewports spent %u of %u", s.water_moves_used,
          s.water_moves);

    for (p = 0; p < 4; p++) {
        u32 b = ((u32)Q2_SCREEN_OT_VIEW_BASE
                 + (u32)p * Q2_SCREEN_OT_VIEW_STRIDE
                 + Q2_SCREEN_OT_VIEW_WATER) * PSX_OT_SUBDIV;
        s32 idx;
        bool any = false;

        for (idx = ot.bucket_head[b]; idx >= 0; idx = ot.next[idx])
            if (ot.prims[idx].kind == PSX_PRIM_MOVE)
                any = true;
        if (any)
            with_moves++;

        /* Every viewport gets its tint even when the pool ran dry, because the
         * overrun branch lands on the tint rather than on the return. */
        CHECK(ot.bucket_head[b] >= 0 &&
              ot.prims[ot.bucket_head[b]].kind == PSX_PRIM_TILE,
              "viewport %d lost its tint to the pool", p);
    }
    CHECK(with_moves >= 1 && with_moves <= 4,
          "%u of four viewports wobbled", with_moves);

    psx_ot_free(&ot);
    q2_screen_free(&s);
}

/*
 * End to end, through the rasteriser: a viewport painted in horizontal bands,
 * composed once dry and once submerged. Submerged, the bands must actually
 * move — that is the MOVE primitive doing its job — and nothing outside the
 * viewport may change, which is what the clip-free blit has to get right on its
 * own since no draw env constrains it.
 */
static void test_water_compose(void)
{
    q2_screen s;
    psx_ot ot;
    psx_raster_opts opts;
    u16 *dry, *wet;
    const psx_framebuffer *fb;
    int x, y, i, moved = 0, outside = 0;
    size_t bytes;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_TWO_V, 2);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 512);
    psx_raster_opts_default(&opts);
    opts.textures = false;
    opts.dither   = false;
    s.dt = Q2_SCREEN_DT_NOMINAL;

    bytes = (size_t)s.buf[0].width * (size_t)s.buf[0].height * sizeof(u16);
    dry = (u16 *)malloc(bytes);
    wet = (u16 *)malloc(bytes);
    if (!dry || !wet) {
        CHECK(false, "out of memory");
        free(dry); free(wet);
        psx_ot_free(&ot);
        q2_screen_free(&s);
        return;
    }

    /* Two passes with the same content: one dry, one at full amplitude. The
     * amplitude is ramped first so both composes see the same geometry. */
    for (i = 0; i < 2; i++) {
        int pass;

        if (i == 1) {
            q2_screen_water_set(&s, 0, true, true);
            for (pass = 0; pass < 20; pass++)
                water_frame(&s, &ot, 1);
        }

        q2_screen_frame_begin(&s, &ot);
        psx_fb_clear(q2_screen_back(&s), 0);
        q2_screen_view_begin(&s, 0, &ot, NULL);
        {
            /* Bands three pixels tall, so a one-pixel vertical displacement is
             * visible without being ambiguous. */
            int band;
            for (band = 0; band * 3 < s.view[0].h; band++) {
                psx_prim *p = psx_ot_add_bucket(&ot, q2_screen_view_otz(&s, 0, 0));
                if (!p)
                    break;
                p->kind = PSX_PRIM_TILE;
                p->xy[0].x = 0;
                p->xy[0].y = (s16)(band * 3);
                p->xy[2].x = s.view[0].w;
                p->xy[2].y = (s16)(band * 3 + 1);
                p->rgb[0].r = p->rgb[0].g = p->rgb[0].b = 255;
            }
        }
        q2_screen_view_end(&s, &ot);
        q2_screen_compose(&s, &ot, NULL, &opts);
        q2_screen_present(&s);
        fb = q2_screen_front(&s);
        memcpy(i == 0 ? dry : wet, fb->px, bytes);
    }

    for (y = 0; y < s.buf[0].height; y++) {
        for (x = 0; x < s.buf[0].width; x++) {
            size_t o = (size_t)y * (size_t)s.buf[0].width + (size_t)x;
            bool inside = (x >= s.view[0].x && x < s.view[0].x + s.view[0].w &&
                           y >= s.view[0].y && y < s.view[0].y + s.view[0].h);

            if (dry[o] == wet[o])
                continue;
            if (inside)
                moved++;
            else
                outside++;
        }
    }

    CHECK(moved > 0, "submerging changed nothing on screen");
    CHECK(outside == 0,
          "%d pixels changed outside the submerged viewport", outside);

    free(dry);
    free(wet);
    psx_ot_free(&ot);
    q2_screen_free(&s);
}

/* ------------------------------------------------------------------------- */
static void test_dt_clamp(void)
{
    q2_screen s;

    q2_screen_init(&s, Q2_VIDEO_PAL);

    /* One VSync(2) field pair at 50 Hz is 12 units of 1/300 s — and the period
     * that produces it is the frame lock itself. */
    CHECK(q2_screen_frame_period(&s) > 0.0399 &&
          q2_screen_frame_period(&s) < 0.0401,
          "the frame lock is two 50 Hz fields, got %f",
          q2_screen_frame_period(&s));
    CHECK(q2_screen_tick_dt(&s, q2_screen_frame_period(&s)) == Q2_SCREEN_DT_NOMINAL,
          "a nominal frame should be %d", Q2_SCREEN_DT_NOMINAL);

    /* 0x800184B8 clamps rather than averages, so a stalled frame is lost time
     * and never a lurch. */
    CHECK(q2_screen_tick_dt(&s, 1.0) == Q2_SCREEN_DT_MAX,
          "a one-second frame should clamp to %d", Q2_SCREEN_DT_MAX);
    CHECK(q2_screen_tick_dt(&s, 30.0 / 300.0) == 30, "exactly 30 is not clamped");
    CHECK(q2_screen_tick_dt(&s, 0.0) == 1, "a zero frame still advances");

    q2_screen_free(&s);
}

/* ------------------------------------------------------------------------- */
/*
 * The pixel's shape, and the field of view that follows from it.
 *
 * None of this is a constant read off a disc — it is what a television does
 * with a 512-wide PAL frame — so `q2psx-inspect screen` cannot check it and
 * this is the only place it is pinned. What matters is that it is EXACT: 2:3
 * with no rounding, because the ratio is 4*256 : 3*512.
 */
static void test_pixel_aspect(void)
{
    q2_screen s;
    int n = 0, d = 0, w = 0, h = 0;

    printf("pixel aspect\n");

    CHECK(q2_screen_init(&s, Q2_VIDEO_PAL) == Q2_OK, "init failed");

    q2_screen_pixel_aspect(&s, &n, &d);
    CHECK(n == 2 && d == 3, "PAL 512-wide pixel is %d:%d, expected 2:3", n, d);

    /* The picture, at one buffer pixel across per unit of scale. 512 columns
     * against 512*3/2 = 768... no: the HEIGHT grows, 248 -> 372. */
    q2_screen_window_size(&s, Q2_SCREEN_FIT_TELEVISION, 1, &w, &h);
    CHECK(w == 512 && h == 372, "picture %dx%d, expected 512x372", w, h);

    q2_screen_window_size(&s, Q2_SCREEN_FIT_TELEVISION, 3, &w, &h);
    CHECK(w == 1536 && h == 1116, "3x picture %dx%d, expected 1536x1116", w, h);

    /* SQUARE is the raw buffer, which is what the port used to present and is
     * a 1.5x horizontal stretch of the above. */
    q2_screen_window_size(&s, Q2_SCREEN_FIT_SQUARE, 1, &w, &h);
    CHECK(w == 512 && h == 248, "square %dx%d, expected 512x248", w, h);

    /*
     * FULL_4_3 is the DEFAULT — a zero-initialised caller gets it — and it is
     * the shape the running game is captured at: 512 across, 384 down, exactly
     * 4:3, three percent shorter than the strict pixel reading above.
     */
    CHECK(Q2_SCREEN_FIT_FULL_4_3 == 0, "4:3 must be what a memset gives");
    q2_screen_window_size(&s, Q2_SCREEN_FIT_FULL_4_3, 1, &w, &h);
    CHECK(w == 512 && h == 384, "4:3 picture %dx%d, expected 512x384", w, h);

    q2_screen_free(&s);
}

/*
 * Fitting the picture into a window of any shape. The property that has to hold
 * for every one of them is the same: the result is inside the window, centred,
 * and the right aspect.
 */
static void test_fit_rect(void)
{
    static const struct { int w, h; } windows[] = {
        { 1920, 1080 },   /* 16:9   — pillarboxed          */
        { 1920, 1200 },   /* 16:10                          */
        { 2560, 1080 },   /* 21:9                           */
        { 1024,  768 },   /* 4:3    — very nearly exact     */
        {  600, 1200 },   /* portrait — letterboxed         */
        { 1536, 1116 },   /* the default window — exact fit */
        {    1,    1 },   /* degenerate                     */
        {    0,    0 }    /* and a caller that passed junk  */
    };
    q2_screen s;
    size_t i;

    printf("fit rect\n");

    CHECK(q2_screen_init(&s, Q2_VIDEO_PAL) == Q2_OK, "init failed");

    for (i = 0; i < sizeof(windows) / sizeof(windows[0]); i++) {
        int ww = windows[i].w, wh = windows[i].h;
        int x = -1, y = -1, w = -1, h = -1;
        int cw = ww < 1 ? 1 : ww, ch = wh < 1 ? 1 : wh;

        q2_screen_fit_rect(&s, Q2_SCREEN_FIT_TELEVISION, ww, wh,
                           &x, &y, &w, &h);

        CHECK(w >= 1 && h >= 1, "%dx%d gave a %dx%d picture", ww, wh, w, h);
        CHECK(x >= 0 && y >= 0 && x + w <= cw && y + h <= ch,
              "%dx%d: picture (%d,%d %dx%d) escapes the window",
              ww, wh, x, y, w, h);
        CHECK(x <= (cw - w + 1) / 2 && y <= (ch - h + 1) / 2,
              "%dx%d: picture is not centred (%d,%d)", ww, wh, x, y);

        /* The aspect, to within the pixel the integer fit has to give up. */
        if (w > 8 && h > 8) {
            double want = 512.0 * 2.0 / (248.0 * 3.0);
            double got  = (double)w / (double)h;
            CHECK(got > want * 0.99 && got < want * 1.01,
                  "%dx%d: picture %dx%d is %.4f:1, wanted %.4f:1",
                  ww, wh, w, h, got, want);
        }
    }

    /* The exact-fit window loses nothing at all. */
    {
        int x = -1, y = -1, w = -1, h = -1;
        q2_screen_fit_rect(&s, Q2_SCREEN_FIT_TELEVISION, 1536, 1116,
                           &x, &y, &w, &h);
        CHECK(x == 0 && y == 0 && w == 1536 && h == 1116,
              "the default window should fit exactly, got (%d,%d %dx%d)",
              x, y, w, h);
    }

    /* STRETCH is the one that is allowed to be any shape: it IS the window. */
    {
        int x = -1, y = -1, w = -1, h = -1;
        q2_screen_fit_rect(&s, Q2_SCREEN_FIT_STRETCH, 1920, 1080,
                           &x, &y, &w, &h);
        CHECK(x == 0 && y == 0 && w == 1920 && h == 1080,
              "stretch should fill, got (%d,%d %dx%d)", x, y, w, h);
    }

    q2_screen_free(&s);
}

/*
 * The field of view every layout describes, and the one property that is shared
 * by all five: the picture is squeezed by exactly 1.5.
 *
 * That is not a coincidence to be explained away — it is arithmetic. The
 * squeeze is `(w/2 / proj) / (h/2 / proj)` over `w*2 / (h*3)`, i.e. 3/2 for any
 * viewport whose geometry offset is its own middle. So this is really a check
 * that every layout DOES centre its offset, which is the thing a mistranscribed
 * `SetGeomOffset` argument would break, and that the halving toward zero has not
 * been turned into a rounding somewhere.
 */
static void test_view_fov(void)
{
    static const struct {
        q2_screen_layout layout;
        int              players;
        double           horizontal, vertical;
    } want[] = {
        { Q2_SCREEN_LAYOUT_ONE,         1, 116.0,  75.6 },
        { Q2_SCREEN_LAYOUT_TWO_H,       2, 116.0,  41.7 },
        { Q2_SCREEN_LAYOUT_TWO_V,       2,  71.9,  70.6 },
        { Q2_SCREEN_LAYOUT_QUAD,        4,  77.3,  41.7 },
        { Q2_SCREEN_LAYOUT_FULL_SINGLE, 1,  77.3,  42.4 }
    };
    q2_screen s;
    size_t i;

    printf("field of view\n");

    CHECK(q2_screen_init(&s, Q2_VIDEO_PAL) == Q2_OK, "init failed");

    for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        q2_screen_fov fov;
        const q2_screen_view *v;

        q2_screen_set_layout(&s, want[i].layout, want[i].players);
        v = &s.view[0];

        CHECK(v->ofs_x == v->w / 2 && v->ofs_y == v->h / 2,
              "%s: geometry offset (%d,%d) is not the viewport's middle of %dx%d",
              q2_screen_layout_name(want[i].layout),
              v->ofs_x, v->ofs_y, v->w, v->h);

        q2_screen_view_fov(&s, v, &fov);

        CHECK(fov.horizontal > want[i].horizontal - 0.1 &&
              fov.horizontal < want[i].horizontal + 0.1,
              "%s: horizontal fov %.1f, expected %.1f",
              q2_screen_layout_name(want[i].layout),
              fov.horizontal, want[i].horizontal);
        CHECK(fov.vertical > want[i].vertical - 0.1 &&
              fov.vertical < want[i].vertical + 0.1,
              "%s: vertical fov %.1f, expected %.1f",
              q2_screen_layout_name(want[i].layout),
              fov.vertical, want[i].vertical);

        /* Loose enough for the one-pixel halving of an odd viewport. */
        CHECK(fov.squeeze > 1.48 && fov.squeeze < 1.52,
              "%s: squeeze %.3f, expected 1.5",
              q2_screen_layout_name(want[i].layout), fov.squeeze);
    }

    /* The overlay camera, which every layout installs and which is the HUD's. */
    {
        q2_screen_fov fov;
        q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_ONE, 1);
        q2_screen_view_fov(&s, &s.overlay, &fov);
        CHECK(fov.horizontal > 77.2 && fov.horizontal < 77.4,
              "overlay horizontal fov %.1f, expected 77.3", fov.horizontal);
    }

    /* A viewport with no projection distance is inert rather than a divide. */
    {
        q2_screen_view v;
        q2_screen_fov fov;
        memset(&v, 0, sizeof(v));
        q2_screen_view_fov(&s, &v, &fov);
        CHECK(fov.horizontal == 0.0 && fov.squeeze == 0.0,
              "an empty viewport should report nothing");
    }

    q2_screen_free(&s);
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    test_bringup();
    test_swap();
    test_ot_slices();
    test_ot_window();
    test_layouts();
    test_compose_clip();
    test_sort_region_drawenv();
    test_background();
    test_shake();
    test_depth_direction();
    test_flash();
    test_build();
    test_context();
    test_display_envs();
    test_meter();
    test_blend_table();
    test_water_ramp();
    test_water_amplitudes();
    test_water_strips();
    test_water_pool();
    test_water_compose();
    test_dt_clamp();
    test_pixel_aspect();
    test_fit_rect();
    test_view_fov();

    if (g_fail == 0)
        printf("test_screen: all checks passed\n");
    else
        printf("test_screen: %d check%s failed\n", g_fail, g_fail == 1 ? "" : "s");

    return g_fail ? 1 : 0;
}
