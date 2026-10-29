/*
 * test_raster.c — which diagonal a quad is cut along, and why it is not free.
 *
 * The backend holds a quad's four corners in the FILE's perimeter order. The
 * console does not: both of its linkers convert perimeter to the hardware's Z
 * order on the way into the packet, and they do it by exchanging corners 2 and
 * 3 — vertices and UVs together, so the pairing survives.
 *
 *     model linker  0x800B2410  SXY0,SXY1,SXY2 <= file v0,v1,v3, then SXY0 <=
 *                              file v2 for the second NCLIP; stored at packet
 *                              +8,+20,+32,+44. The shift counts on the packed
 *                              index word are 3, 5, 21, 13 — that is 0,1,3,2.
 *     world linker  0x800AF844  the identical four shifts, same order.
 *     model emitter 0x8006A3CC..0x8006A3F8  POLY uv0..uv3 <= file uv0,uv1,uv3,
 *                              uv2, stored to +12,+24,+36,+48.
 *
 * The GPU then splits a four-point polygon on the edge between its SECOND and
 * THIRD corners. Undo the conversion and that edge, in perimeter indices, is
 * 1—3: the two triangles are (0,1,3) and (1,3,2), NOT the fan (0,1,2)+(0,2,3).
 *
 * Both cover a convex quad, so nothing about a wall's silhouette catches this.
 * What it changes is everything that interpolates: UVs are affine per triangle,
 * so the diagonal decides which way a texture creases, and a folded quad's
 * COVERAGE differs outright. Those are the two things pinned here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"
#include "raster.h"

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

static u16 pixel_at(const psx_framebuffer *fb, int x, int y)
{
    return fb->px[(size_t)y * (size_t)fb->width + (size_t)x];
}

/* Every option off but the one under test: no dither, no blending, so a
 * sampled texel reaches the buffer unchanged and a comparison is exact. */
static void opts_exact(psx_raster_opts *o)
{
    psx_raster_opts_default(o);
    o->dither            = false;
    o->semi_transparency = false;
}

/* ------------------------------------------------------------------------- */
/* 1. The affine warp — which pair of corners the interpolation is exact on   */
/* ------------------------------------------------------------------------- */
/*
 * A 64x64 square, perimeter order, carrying UVs that are EQUAL on the opposite
 * pairs: corners 0 and 2 both sample u=0, corners 1 and 3 both sample u=2.
 *
 * Along whichever diagonal the quad is actually split, both triangles share
 * that edge and therefore agree exactly along it — so the texel on the shared
 * diagonal is the one its two endpoints name, with no interpolation left in it.
 *
 * Probe (48,16), which lies on the 1—3 diagonal (x + y == 64):
 *
 *     split 1—3 (the hardware's)  u interpolates 2 -> 2, so u = 2  GREEN
 *     split 0—2 (a fan)           (48,16) falls in triangle (0,1,2) with
 *                                 weights 1/4, 1/2, 1/4 on u = 0, 2, 0,
 *                                 so u = 1                        BLUE
 *
 * Three distinct texels rather than two, so a failure says which rule ran
 * instead of only that the expected one did not.
 */
static void test_uv_diagonal(void)
{
    psx_framebuffer fb;
    psx_raster_opts opts;
    psx_vram       *vram;
    psx_prim        prim;
    const u16       RED   = psx_rgb555(248, 0, 0);
    const u16       BLUE  = psx_rgb555(0, 0, 248);
    const u16       GREEN = psx_rgb555(0, 248, 0);
    int i;

    puts("\nthe affine warp runs along the hardware's diagonal");

    vram = (psx_vram *)calloc(1, sizeof(*vram));
    if (!vram || psx_fb_init(&fb, 128, 128) != Q2_OK) {
        printf("  FAIL  out of memory\n");
        g_failures++;
        free(vram);
        return;
    }
    psx_fb_clear(&fb, 0);
    opts_exact(&opts);

    /* Three texels in a row of a 16-bit page: u = 0, 1, 2. */
    vram->px[0][0] = RED;
    vram->px[0][1] = BLUE;
    vram->px[0][2] = GREEN;

    memset(&prim, 0, sizeof(prim));
    prim.kind  = PSX_PRIM_GT4;
    prim.tpage = 2 << 7;          /* 16 bits per texel, page (0,0) */

    prim.xy[0].x = 0;   prim.xy[0].y = 0;
    prim.xy[1].x = 64;  prim.xy[1].y = 0;
    prim.xy[2].x = 64;  prim.xy[2].y = 64;
    prim.xy[3].x = 0;   prim.xy[3].y = 64;

    prim.uv[0].u = 0;   prim.uv[0].v = 0;
    prim.uv[1].u = 2;   prim.uv[1].v = 0;
    prim.uv[2].u = 0;   prim.uv[2].v = 0;
    prim.uv[3].u = 2;   prim.uv[3].v = 0;

    /* Modulation is (texel * rgb) >> 7, so 128 passes the texel through. */
    for (i = 0; i < 4; i++) {
        prim.rgb[i].r = prim.rgb[i].g = prim.rgb[i].b = 128;
    }
    prim.textured_blend = true;

    psx_raster_prim(&fb, &prim, vram, &opts);

    check(pixel_at(&fb, 48, 16) == GREEN,
          "a point on the 1-3 diagonal samples the texel that diagonal names");
    check(pixel_at(&fb, 48, 16) != BLUE,
          "...and not the one the 0-2 fan would have interpolated");

    /* The mirrored probe on the other diagonal must NOT be pinned to u=0: if
     * it were, the split would be 0—2 after all. (16,16) lies on y == x. */
    check(pixel_at(&fb, 16, 16) != RED,
          "a point on the 0-2 diagonal is interpolated, not pinned");

    /* The quad is still fully covered — the corners themselves prove the two
     * triangles between them left no gap. */
    check(pixel_at(&fb, 2, 2)   != 0, "the 0 corner is covered");
    check(pixel_at(&fb, 61, 2)  != 0, "the 1 corner is covered");
    check(pixel_at(&fb, 61, 61) != 0, "the 2 corner is covered");
    check(pixel_at(&fb, 2, 61)  != 0, "the 3 corner is covered");

    psx_fb_free(&fb);
    free(vram);
}

/* ------------------------------------------------------------------------- */
/* 2. Coverage — a folded quad, where the two rules disagree outright         */
/* ------------------------------------------------------------------------- */
/*
 * A dart: the perimeter walks (0,0) -> (64,24) -> (128,0) -> (64,96), so corner
 * 1 is REFLEX and the polygon has a notch bitten out of its top edge.
 *
 * The hardware's triangles (0,1,3) and (1,3,2) meet along the vertical segment
 * from (64,24) to (64,96) — inside the shape — and cover the dart and nothing
 * else. The fan's diagonal 0—2 runs along y = 0, which lies OUTSIDE the
 * polygon, so its two triangles overlap and between them fill the whole
 * triangle (0,0),(128,0),(64,96) — notch included.
 *
 * A quad that folds in screen space is not hypothetical here: the second NCLIP
 * in both linkers exists precisely because a quad can fold, and test_world
 * pins that behaviour.
 */
static void test_folded_quad_coverage(void)
{
    psx_framebuffer fb;
    psx_raster_opts opts;
    psx_prim        prim;
    int i;

    puts("\na folded quad covers the dart, not its convex hull");

    if (psx_fb_init(&fb, 160, 128) != Q2_OK) {
        printf("  FAIL  out of memory\n");
        g_failures++;
        return;
    }
    psx_fb_clear(&fb, 0);
    opts_exact(&opts);

    memset(&prim, 0, sizeof(prim));
    prim.kind = PSX_PRIM_G4;

    prim.xy[0].x = 0;   prim.xy[0].y = 0;
    prim.xy[1].x = 64;  prim.xy[1].y = 24;
    prim.xy[2].x = 128; prim.xy[2].y = 0;
    prim.xy[3].x = 64;  prim.xy[3].y = 96;

    for (i = 0; i < 4; i++) {
        prim.rgb[i].r = prim.rgb[i].g = prim.rgb[i].b = 248;
    }

    psx_raster_prim(&fb, &prim, NULL, &opts);

    /* Inside the notch — above the V, below the hull's top edge. The fan draws
     * here; the hardware does not. */
    check(pixel_at(&fb, 64, 8)  == 0,
          "the notch above the reflex corner is left empty");
    check(pixel_at(&fb, 64, 16) == 0,
          "...all the way down to the reflex corner");

    /* Inside the dart proper — drawn under either rule, so this is the control
     * that says the primitive was rasterised at all. */
    check(pixel_at(&fb, 64, 60) != 0, "the body of the dart is drawn");
    check(pixel_at(&fb, 40, 40) != 0, "and its left half");
    check(pixel_at(&fb, 88, 40) != 0, "and its right half");

    psx_fb_free(&fb);
}

/* ------------------------------------------------------------------------- */
/* 3. The flare path still gets the console's own order                       */
/* ------------------------------------------------------------------------- */
/*
 * `quad_zorder` marks a packet that is ALREADY in hardware order because it was
 * built as a console packet (flare.c). Those must keep the literal
 * (0,1,2)+(1,2,3), and the fix to the perimeter rule must not have quietly
 * become one rule for both.
 *
 * The same dart in Z order is corners 0,1,3,2 — so feeding the perimeter dart's
 * corners through the exchange gives a packet that must rasterise IDENTICALLY
 * to the perimeter one above.
 */
static void test_zorder_unchanged(void)
{
    psx_framebuffer perimeter, zorder;
    psx_raster_opts opts;
    psx_prim        a, b;
    int i, x, y, differing = 0;

    puts("\na Z-order packet still describes the same shape");

    if (psx_fb_init(&perimeter, 160, 128) != Q2_OK) {
        printf("  FAIL  out of memory\n");
        g_failures++;
        return;
    }
    if (psx_fb_init(&zorder, 160, 128) != Q2_OK) {
        printf("  FAIL  out of memory\n");
        g_failures++;
        psx_fb_free(&perimeter);
        return;
    }
    psx_fb_clear(&perimeter, 0);
    psx_fb_clear(&zorder, 0);
    opts_exact(&opts);

    memset(&a, 0, sizeof(a));
    a.kind = PSX_PRIM_G4;
    a.xy[0].x = 0;   a.xy[0].y = 0;
    a.xy[1].x = 64;  a.xy[1].y = 24;
    a.xy[2].x = 128; a.xy[2].y = 0;
    a.xy[3].x = 64;  a.xy[3].y = 96;
    for (i = 0; i < 4; i++) {
        a.rgb[i].r = a.rgb[i].g = a.rgb[i].b = 248;
    }

    /* The console's own conversion: corners 2 and 3 exchanged. */
    b = a;
    b.quad_zorder = true;
    b.xy[2] = a.xy[3];
    b.xy[3] = a.xy[2];
    b.rgb[2] = a.rgb[3];
    b.rgb[3] = a.rgb[2];

    psx_raster_prim(&perimeter, &a, NULL, &opts);
    psx_raster_prim(&zorder, &b, NULL, &opts);

    for (y = 0; y < perimeter.height; y++) {
        for (x = 0; x < perimeter.width; x++) {
            if (pixel_at(&perimeter, x, y) != pixel_at(&zorder, x, y))
                differing++;
        }
    }
    check(differing == 0,
          "the same quad in either convention rasterises to the same pixels");

    psx_fb_free(&perimeter);
    psx_fb_free(&zorder);
}

/* ------------------------------------------------------------------------- */
/* 4. A sample must not step off the primitive's own UV edge                  */
/* ------------------------------------------------------------------------- */
/*
 * The blaster's top strip, taken verbatim out of a BASE0 frame: part 7 face 56
 * of `Blaster G`, whose UV quad is a SLANTED parallelogram —
 *
 *     screen (370,157) (348,151) (326,151) (341,158)
 *     uv     (161,128) (119,128) (108,139) (148,139)
 *
 * Its top edge runs from (119,128) to (108,139), so in texture space that edge
 * lies on the anti-diagonal u + v == 247, and the whole quad sits at u + v >=
 * 247. Interpolating the corners with a TRUNCATING divide walked the top row of
 * pixels along u + v == 246 instead — one texel outside the primitive, every
 * pixel of it — and on the real texture that row is the emitter's orange
 * grille, so the gun grew a dashed orange line along its top edge that the
 * console does not draw.
 *
 * The existing clamp cannot catch this: it bounds u and v to the corners'
 * ranges SEPARATELY, which is an axis-aligned box, and every escaping sample
 * here is inside that box. What was wrong is the rounding, not the bound.
 *
 * So: poison the anti-diagonal just outside the edge and assert nothing samples
 * it. This fails on the truncating interpolation and passes on the rounded one.
 */
static void test_sample_stays_inside_uv(void)
{
    psx_framebuffer fb;
    psx_raster_opts opts;
    psx_vram       *vram;
    psx_prim        prim;
    const u16       POISON = psx_rgb555(248, 96, 8);    /* the emitter orange */
    const u16       BODY   = psx_rgb555(64, 64, 72);    /* anything else      */
    int u, v, x, y, poisoned = 0;

    puts("\na sample stays inside the primitive's own UV quad");

    vram = (psx_vram *)calloc(1, sizeof(*vram));
    if (!vram || psx_fb_init(&fb, 512, 248) != Q2_OK) {
        printf("  FAIL  out of memory\n");
        g_failures++;
        free(vram);
        return;
    }
    psx_fb_clear(&fb, 0);
    opts_exact(&opts);

    /* A 16-bit page, so a texel is just vram[v][u] and no CLUT is involved. */
    for (v = 100; v < 160; v++) {
        for (u = 90; u < 180; u++)
            vram->px[v][u] = (u + v <= 246) ? POISON : BODY;
    }

    memset(&prim, 0, sizeof(prim));
    prim.kind  = PSX_PRIM_GT4;
    prim.tpage = 2 << 7;

    prim.xy[0].x = 370; prim.xy[0].y = 157;
    prim.xy[1].x = 348; prim.xy[1].y = 151;
    prim.xy[2].x = 326; prim.xy[2].y = 151;
    prim.xy[3].x = 341; prim.xy[3].y = 158;

    prim.uv[0].u = 161; prim.uv[0].v = 128;
    prim.uv[1].u = 119; prim.uv[1].v = 128;
    prim.uv[2].u = 108; prim.uv[2].v = 139;
    prim.uv[3].u = 148; prim.uv[3].v = 139;

    for (x = 0; x < 4; x++) {
        prim.rgb[x].r = prim.rgb[x].g = prim.rgb[x].b = 128;
    }
    prim.textured_blend = true;

    psx_raster_prim(&fb, &prim, vram, &opts);

    for (y = 145; y < 165; y++) {
        for (x = 320; x < 375; x++) {
            if (pixel_at(&fb, x, y) == POISON)
                poisoned++;
        }
    }
    check(poisoned == 0, "no pixel samples the texel row outside the top edge");
    if (poisoned)
        printf("        %d pixels sampled u+v <= 246\n", poisoned);

    /* ...and the strip is still drawn, so "nothing poisoned" is not "nothing
     * drawn at all". */
    {
        int drawn = 0;
        for (y = 145; y < 165; y++) {
            for (x = 320; x < 375; x++) {
                if (pixel_at(&fb, x, y) != 0)
                    drawn++;
            }
        }
        check(drawn > 100, "and the strip is still rasterised");
    }

    psx_fb_free(&fb);
    free(vram);
}

static void test_draw_mode_inheritance(void)
{
    static const u16 result[4] = {0x318C, 0x6318, 0x2108, 0x4A52};
    psx_framebuffer fb;
    psx_raster_opts opts;
    psx_draw_state state = {0};
    psx_prim p = {0};
    psx_vram *vram = calloc(1, sizeof(*vram));
    int mode;
    if (!vram || psx_fb_init(&fb, 8, 8) != Q2_OK) exit(2);
    psx_raster_opts_default(&opts);
    opts.dither = false;
    for (mode = 0; mode < 4; mode++) {
        psx_fb_clear(&fb, 0x4210);
        memset(&p, 0, sizeof(p));
        /* Opaque, completely offscreen: its TPage still changes the GPU. */
        p.kind = PSX_PRIM_FT4;
        p.tpage = (u16)(mode << 5);
        p.xy[0].x = p.xy[1].x = p.xy[2].x = p.xy[3].x = -900;
        state.mode = 0x3600;
        psx_raster_command(&fb, &p, vram, &opts, &state);
        check(state.mode == (u16)(0x3600 | mode << 5),
              "polygon page preserves draw-mode high bits despite clipping");
        p.kind = PSX_PRIM_TILE;
        p.tpage = 0x60; /* not a field of a hardware TILE; must be ignored */
        p.xy[0].x = p.xy[0].y = 0;
        p.xy[2].x = p.xy[2].y = 8;
        p.rgb[0].r = p.rgb[0].g = p.rgb[0].b = 64;
        p.semi_transparent = true;
        psx_raster_command(&fb, &p, vram, &opts, &state);
        check(pixel_at(&fb, 2, 2) == result[mode],
              "tile pixels inherit each of the four hardware ABR modes");
        check((state.mode & 0x60) == (u16)(mode << 5),
              "untextured packets cannot overwrite the mode");
    }
    p.kind = PSX_PRIM_TPAGE;
    p.tpage = 0x0101; /* 16-bit texture, X=64, no dither */
    psx_raster_command(&fb, &p, vram, &opts, &state);
    check(state.mode == 0x0101, "E1 replaces the complete mode");
    vram->px[0][64] = 0x03E0;
    p.kind = PSX_PRIM_SPRT;
    p.tpage = 0;
    p.semi_transparent = p.textured_blend = false;
    psx_raster_command(&fb, &p, vram, &opts, &state);
    check(pixel_at(&fb, 0, 0) == 0x03E0, "sprite inherits texture page from E1");
    p.kind = PSX_PRIM_TILE;
    p.set_tpage = true;
    p.tpage = 0x0600;
    psx_raster_command(&fb, &p, vram, &opts, &state);
    check(state.mode == 0x0600, "folded panel DR_TPAGE replaces the full mode");
    memset(&p, 0, sizeof(p));
    p.kind = PSX_PRIM_FT4;
    p.tpage = 0x083D; /* BIGGUN's GetTPage(0,1,832,832) */
    p.xy[1].x = p.xy[2].x = 8;
    p.xy[2].y = p.xy[3].y = 8;
    p.uv[0].v = p.uv[1].v = p.uv[2].v = p.uv[3].v = 159;
    vram->px[415][832] = 0x1111;
    vram->px[0][1] = 0x7FFF;
    /* New fixture palette replaces the one loaded by the clipped polygons. */
    psx_draw_state_clear_cache(&state);
    psx_raster_command(&fb, &p, vram, &opts, &state);
    check(state.mode == 0x063D, "GP1 defaults to ignoring the second-VRAM-bank bit");
    check(pixel_at(&fb, 2, 2) == 0x7FFF,
          "retail ignores high texture Y without GP1 enabling the second bank");
    psx_draw_state_gp1(&state, 0x09000001u);
    psx_fb_clear(&fb, 0x001f);
    psx_raster_command(&fb, &p, vram, &opts, &state);
    check(state.mode == 0x0E3D, "GP1 enables polygon writes to the second-VRAM-bank bit");
    check(pixel_at(&fb, 2, 2) == 0x001f,
          "enabled second bank samples absent VRAM and leaves the target alone");
    p.tpage = 0x003D;
    psx_raster_command(&fb, &p, vram, &opts, &state);
    check(state.mode == 0x063D, "the next textured polygon can clear bit 11");
    psx_fb_free(&fb);
    free(vram);
}

static void test_texture_transparency_bit(void)
{
    psx_framebuffer fb;
    psx_raster_opts opts;
    psx_prim p = {0};
    psx_vram *vram = calloc(1, sizeof(*vram));
    int bpp, sprite, stp, x, y;
    if (!vram || psx_fb_init(&fb, 8, 8) != Q2_OK) exit(2);
    psx_raster_opts_default(&opts);
    opts.dither = false;
    for (bpp = 0; bpp < 3; bpp++) {
        for (sprite = 0; sprite < 2; sprite++) {
            memset(&p, 0, sizeof(p));
            p.kind = sprite ? PSX_PRIM_SPRT : PSX_PRIM_FT4;
            p.tpage = (u16)((bpp << 7) | 0x20);
            p.clut = 16 << 6;
            p.xy[1].x = p.xy[2].x = 8;
            p.xy[2].y = p.xy[3].y = 8;
            p.semi_transparent = true;
            for (stp = 0; stp < 2; stp++) {
                u16 texel = (u16)(0x001F | (stp << 15));
                for (y = 0; y < 8; y++)
                    for (x = 0; x < 8; x++)
                        vram->px[y][x] = bpp == 0 ? 0x1111 : bpp == 1 ? 0x0101 : texel;
                vram->px[16][1] = texel;
                psx_fb_clear(&fb, 0x4210);
                psx_raster_prim(&fb, &p, vram, &opts);
                check(pixel_at(&fb, 1, 1) == (stp ? 0xC21F : 0x001F),
                      "textured ABE blends only texels with STP, in every colour depth");
                p.semi_transparent = false;
                psx_raster_prim(&fb, &p, vram, &opts);
                check(pixel_at(&fb, 1, 1) == (stp ? 0x801F : 0x001F),
                      "opaque packets preserve STP in framebuffer bit 15 without blending");
                p.semi_transparent = true;
            }
        }
    }
    psx_fb_free(&fb);
    free(vram);
}

static void test_scaled_ui_sprite(void)
{
    psx_framebuffer fb = {0};
    psx_vram *vram = calloc(1, sizeof(*vram));
    psx_raster_opts opts;
    psx_prim p = {0};
    int x, y;
    psx_fb_init(&fb, 16, 16);
    opts_exact(&opts);
    p.kind = PSX_PRIM_SPRT;
    p.tpage = 2u << 7; /* 15-bit texels */
    p.xy[0].x = p.xy[0].y = 2;
    p.xy[2].x = 10; p.xy[2].y = 6;
    p.sprite_w = 4; p.sprite_h = 2;
    for (y = 0; y < 2; y++) for (x = 0; x < 4; x++)
        vram->px[y][x] = (u16)(1+x+4*y);
    psx_raster_prim(&fb, &p, vram, &opts);
    for (y = 2; y < 6; y++) for (x = 2; x < 10; x++)
        check(pixel_at(&fb, x, y) == 1+(x-2)/2+4*((y-2)/2),
              "scaled sprites sample their original texels, not adjacent atlas cells");
    check(pixel_at(&fb, 10, 5) == 0, "scaled sprite stays within destination");
    /* Clip after scaling: clipping must not restart the sprite's UV origin. */
    opts.clip_x = 5; opts.clip_y = 3; opts.clip_w = 4; opts.clip_h = 3;
    psx_fb_clear(&fb, 0);
    psx_raster_prim(&fb, &p, vram, &opts);
    check(pixel_at(&fb, 5, 3) == 2 && pixel_at(&fb, 4, 3) == 0,
          "viewport clipping retains scaled sprite sampling");
    p.xy[2].x = 4; p.xy[2].y = 3;
    opts.clip_w = opts.clip_h = 0;
    psx_fb_clear(&fb, 0);
    psx_raster_prim(&fb, &p, vram, &opts);
    check(pixel_at(&fb, 2, 2) == 1 && pixel_at(&fb, 3, 2) == 3,
          "small viewports downsample within the source sprite");
    psx_fb_free(&fb);
    free(vram);
}

static void test_texture_window_and_addressing(void)
{
    psx_framebuffer fb;
    psx_raster_opts opts;
    psx_draw_state state = {0};
    psx_prim p = {0}, command = {0};
    psx_vram *vram = calloc(1, sizeof(*vram));
    int bpp, sprite, x, y;
    if (!vram || psx_fb_init(&fb, 8, 8) != Q2_OK) exit(2);
    opts_exact(&opts);
    p.xy[1].x = p.xy[2].x = 8; p.xy[2].y = p.xy[3].y = 8;
    p.clut = psx_make_clut(0, 64);
    for (bpp = 0; bpp < 3; bpp++) for (sprite = 0; sprite < 2; sprite++) {
        memset(vram, 0, sizeof(*vram));
        psx_draw_state_clear_cache(&state); /* independent addressing fixture */
        for (y = 24; y < 32; y++) for (x = 0; x < 8; x++)
            vram->px[y][(16 >> (2-bpp))+x] = bpp == 0 ? 0x1111 : bpp == 1 ? 0x0101 : 0x03E0;
        vram->px[64][1] = 0x03E0;
        command.kind = PSX_PRIM_TWINDOW;
        command.texture_window = 31u | (31u << 5) | (2u << 10) | (3u << 15);
        psx_raster_command(&fb, &command, vram, &opts, &state);
        command.kind = PSX_PRIM_TPAGE; command.tpage = (u16)(bpp << 7);
        psx_raster_command(&fb, &command, vram, &opts, &state);
        p.kind = sprite ? PSX_PRIM_SPRT : PSX_PRIM_FT4; p.tpage = command.tpage;
        psx_fb_clear(&fb, 0);
        psx_raster_command(&fb, &p, vram, &opts, &state);
        check(pixel_at(&fb, 2, 2) == 0x03E0,
              "E2 window persists across E1 and applies to all texture depths and primitive paths");
    }
    command.kind = PSX_PRIM_TWINDOW; command.texture_window = 0;
    psx_raster_command(&fb, &command, vram, &opts, &state);
    p.kind = PSX_PRIM_FT4;
    p.tpage = (2 << 7) | 15;
    for (x = 0; x < 4; x++) { p.uv[x].u = 64; p.uv[x].v = 20; }
    vram->px[20][0] = 0x001F;
    psx_raster_command(&fb, &p, vram, &opts, &state);
    check(pixel_at(&fb, 2, 2) == 0x001F, "direct texture X wraps within the VRAM row");
    p.tpage = (1 << 7) | 15; p.clut = psx_make_clut(1008,64);
    for (x = 0; x < 4; x++) p.uv[x].u = 128;
    vram->px[20][0] = 0xC8C8;
    vram->px[64][184] = 0x7C00;
    psx_vram_transfer(vram); /* CPU upload; 16->8 bit E1 alone does not flush */
    psx_raster_command(&fb, &p, vram, &opts, &state);
    check(pixel_at(&fb, 2, 2) == 0x7C00, "8-bit texel fetch and CLUT index both wrap without carrying to Y");

    p.kind = PSX_PRIM_SPRT; p.tpage = 0x3100; p.set_tpage = true;
    p.uv[0].u = 3; p.uv[0].v = 2;
    p.xy[2].x = 4; p.xy[2].y = 3;
    for (y = 0; y < 3; y++) for (x = 0; x < 4; x++) vram->px[y][x] = (u16)(1+x+8*y);
    psx_raster_command(&fb, &p, vram, &opts, &state);
    check(pixel_at(&fb, 0, 0) == 20 && pixel_at(&fb, 3, 2) == 1,
          "E1 rectangle flip bits decrement U and V from the sprite origin");
    psx_fb_free(&fb);
    free(vram);
}

static void test_framebuffer_masks(void)
{
    psx_framebuffer fb;
    psx_raster_opts opts;
    psx_draw_state state = {0};
    psx_prim p = {0}, command = {0};
    u16 scene[16] = {0}; u8 ui[16] = {0};
    psx_vram *vram = calloc(1, sizeof(*vram));
    if (!vram || psx_fb_init(&fb, 4, 4) != Q2_OK) exit(2);
    opts_exact(&opts); opts.scene = scene; opts.ui_mask = ui;
    p.kind = PSX_PRIM_TILE; p.xy[2].x = p.xy[2].y = 4; p.rgb[0].r = 248;
    command.kind = PSX_PRIM_MASK; command.mask_mode = 1;
    psx_raster_command(&fb, &command, vram, &opts, &state);
    psx_raster_command(&fb, &p, vram, &opts, &state);
    check(fb.px[0] == 0x801F && scene[0] == 0x801F, "E6 force mask covers untextured and scene output");
    command.mask_mode = 2;
    psx_raster_command(&fb, &command, vram, &opts, &state);
    p.rgb[0].r = 0; p.rgb[0].g = 248; p.ui = true;
    psx_raster_command(&fb, &p, vram, &opts, &state);
    check(fb.px[0] == 0x801F && scene[0] == 0x801F && ui[0] == 0,
          "mask-protected UI writes change neither pixels nor bloom coverage");
    command.mask_mode = 0;
    psx_raster_command(&fb, &command, vram, &opts, &state);
    p.ui = false; p.kind = PSX_PRIM_SPRT; p.set_tpage = true; p.tpage = 2 << 7;
    vram->px[0][0] = 0x8000;
    psx_raster_command(&fb, &p, vram, &opts, &state);
    check(fb.px[0] == 0x8000, "STP-only black texel is opaque black with mask, not transparent");
    vram->px[0][0] = 0;
    psx_vram_transfer(vram);
    command.mask_mode = 1;
    psx_raster_command(&fb, &command, vram, &opts, &state);
    fb.px[0] = 0x03E0;
    psx_raster_command(&fb, &p, vram, &opts, &state);
    check(fb.px[0] == 0x03E0, "zero texels skip writes even with force-mask enabled");

    memset(&p, 0, sizeof(p));
    p.kind = PSX_PRIM_MOVE;
    p.xy[PSX_MOVE_DST].x = 1; p.xy[PSX_MOVE_SIZE].x = 3; p.xy[PSX_MOVE_SIZE].y = 1;
    fb.px[0] = 0x001F; fb.px[1] = 0x03E0; fb.px[2] = 0x8001; fb.px[3] = 0;
    command.mask_mode = 3;
    psx_raster_command(&fb, &command, vram, &opts, &state);
    psx_raster_command(&fb, &p, vram, &opts, &state);
    check(fb.px[1] == 0x801F && fb.px[2] == 0x8001 && fb.px[3] == 0x8001,
          "overlapping MOVE honours force/check masks without corrupting source pixels");
    psx_fb_free(&fb);
    free(vram);
}

static void test_rgb555_blending(void)
{
    psx_framebuffer fb;
    psx_raster_opts opts;
    psx_prim p = {0};
    int mode, back, front;
    if (psx_fb_init(&fb, 1, 1) != Q2_OK) exit(2);
    psx_raster_opts_default(&opts);
    opts.dither = false;
    p.kind = PSX_PRIM_TILE;
    p.xy[2].x = p.xy[2].y = 1;
    p.semi_transparent = true;
    for (mode = 0; mode < 4; mode++) {
        int mismatches = 0;
        p.tpage = (u16)(mode << 5);
        for (back = 0; back < 32; back++) for (front = 0; front < 256; front++) {
            int f = front >> 3, result;
            switch (mode) {
            case 0: result = (back + f) / 2; break;
            case 1: result = back + f; break;
            case 2: result = back - f; break;
            default: result = back + f / 4; break;
            }
            if (result < 0) result = 0;
            if (result > 31) result = 31;
            fb.px[0] = (u16)(back | back << 5 | back << 10);
            p.rgb[0].r = p.rgb[0].g = p.rgb[0].b = (u8)front;
            psx_raster_prim(&fb, &p, NULL, &opts);
            if ((fb.px[0] & 0x7FFF) != (u16)(result | result << 5 | result << 10)) mismatches++;
        }
        check(mismatches == 0, "all 32x256 channel pairs blend after RGB555 reduction");
    }
    psx_fb_free(&fb);
}

static void test_dither_eligibility(void)
{
    psx_framebuffer fb;
    psx_raster_opts opts;
    psx_prim p = {0};
    psx_vram *vram = calloc(1, sizeof(*vram));
    int i;
    if (!vram || psx_fb_init(&fb, 8, 8) != Q2_OK) exit(2);
    psx_raster_opts_default(&opts);
    p.xy[1].x = p.xy[2].x = 8;
    p.xy[2].y = p.xy[3].y = 8;
    p.tpage = 2 << 7;
    for (i = 0; i < 4; i++) p.rgb[i].r = p.rgb[i].g = p.rgb[i].b = 128;
    vram->px[0][0] = psx_rgb555(128,128,128);
    p.kind = PSX_PRIM_F4;
    psx_raster_prim(&fb, &p, vram, &opts);
    check(fb.px[0] == 0x4210, "flat untextured polygons bypass dithering");
    p.kind = PSX_PRIM_FT4;
    psx_raster_prim(&fb, &p, vram, &opts);
    check(fb.px[0] == 0x4210, "raw texture polygons preserve texels without dithering");
    p.textured_blend = true;
    psx_raster_prim(&fb, &p, vram, &opts);
    check(fb.px[0] == 0x3DEF, "modulated flat polygons use the dither matrix");
    p.kind = PSX_PRIM_G4;
    psx_raster_prim(&fb, &p, vram, &opts);
    check(fb.px[0] == 0x3DEF, "Gouraud polygons use the dither matrix");
    p.kind = PSX_PRIM_TILE;
    psx_raster_prim(&fb, &p, vram, &opts);
    check(fb.px[0] == 0x4210, "tiles never dither");
    p.kind = PSX_PRIM_LINE_F2;
    p.xy[1].x = 7;
    psx_raster_prim(&fb, &p, vram, &opts);
    check(fb.px[0] == 0x3DEF, "flat lines still dither");
    psx_fb_clear(&fb, 0);
    p.xy[0].y = -300; p.xy[1].y = 300;
    psx_raster_prim(&fb, &p, vram, &opts);
    for (i = 0; i < 64; i++) if (fb.px[i]) break;
    check(i == 64, "GPU rejects lines spanning more than 511 rows before clipping");
    psx_fb_free(&fb);
    free(vram);
}

int main(void)
{
    puts("quad split");
    puts("==========");

    test_uv_diagonal();
    test_folded_quad_coverage();
    test_zorder_unchanged();
    test_sample_stays_inside_uv();
    test_draw_mode_inheritance();
    test_texture_transparency_bit();
    test_scaled_ui_sprite();
    test_rgb555_blending();
    test_dither_eligibility();
    test_texture_window_and_addressing();
    test_framebuffer_masks();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
