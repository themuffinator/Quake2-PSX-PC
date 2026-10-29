#include "credits.h"
#include "levelbin.h"
#include "screen.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static void row(u8 *module, u32 off, u32 text)
{
    q2_wr_u32(module + off, text);
    q2_wr_u16(module + off + 4, 256);
    q2_wr_u16(module + off + 6, 210);
}

static void tables(void)
{
    u8 module[512] = {0};
    q2_credit_row out[4];
    u32 base, i;
    memcpy(module + 16, "DEVELOPED BY", 13);
    memcpy(module + 48, "HAMMERHEAD LTD", 14);
    /* The text pool is deliberately out of table order. Duplicate labels
     * and empty strings survive; the leading cursor sentinel does not. */
    for (base = 0; ; base = 0x80100000u) {
        row(module, 128, base + 16);
        row(module, 176, base + 40);
        row(module, 200, base + 40);
        row(module, 224, base + 48);
        row(module, 248, base + 48);
        CHECK(q2_levelbin_credits(module, sizeof(module), base, out, 4) == 4);
        CHECK(!strcmp(out[0].text, "DEVELOPED BY"));
        CHECK(!strcmp(out[1].text, ""));
        CHECK(out[2].text == out[3].text && !strcmp(out[2].text, "HAMMERHEAD LTD"));
        CHECK(q2_levelbin_credits(module, sizeof(module), base, NULL, 0) == 4);
        out[1].text = "GUARD";
        CHECK(q2_levelbin_credits(module, sizeof(module), base, out, 1) == 4);
        CHECK(!strcmp(out[1].text, "GUARD"));
        for (i = 0; i < 296; i++)
            CHECK(q2_levelbin_credits(module, i, base, NULL, 0) == 0);
        q2_wr_u32(module + 224, base + 512);
        CHECK(q2_levelbin_credits(module, sizeof(module), base, NULL, 0) == 0);
        row(module, 224, base + 48);
        q2_wr_u32(module + 232, base + sizeof(module));
        CHECK(q2_levelbin_credits(module, sizeof(module), base, NULL, 0) == 0);
        q2_wr_u32(module + 232, 0);
        q2_wr_u16(module + 254, 223);
        q2_wr_u32(module + 256, base + 300);
        CHECK(q2_levelbin_credits(module, sizeof(module), base, out, 4) == 4);
        q2_wr_u32(module + 256, 0);
        if (base) break;
    }
}

static void timing(void)
{
    q2_credit_row rows[40];
    q2_credits r, other;
    int i, hz, ticks;
    for (i = 0; i < 40; i++) {
        rows[i].text = i == 1 ? "" : "NAME";
        rows[i].x = 256; rows[i].y = 210;
    }
    q2_credits_start(&r, rows, 40);
    CHECK(r.slot[0].y == 210 && r.next == 1);
    CHECK(q2_credits_advance(&r, 1.0/50, 50));
    CHECK(r.slot[0].y == 209);
    CHECK(q2_credits_advance(&r, 15.0/50, 50));
    CHECK(r.next == 2 && r.slot[1].y == 209 && !r.slot[1].text[0]);
    CHECK(q2_credits_advance(&r, 16.0/50, 50));
    CHECK(r.next == 3 && r.slot[2].y == 209 && r.slot[0].y == 178);
    CHECK(q2_credits_advance(&r, 224.0/50, 50));
    CHECK(r.frame == 0 && r.next == 17 && r.slot[0].y == 209);
    q2_credits_start(&r, rows, 40);
    CHECK(q2_credits_advance(&r, 6.0, 50));
    for (hz = 30; hz <= 144; hz += hz == 30 ? 30 : 84) {
        q2_credits_start(&other, rows, 40);
        for (i = 0; i < hz * 6; i++) q2_credits_advance(&other, 1.0/hz, 50);
        CHECK(other.frame == r.frame && other.next == r.next);
        for (i = 0; i < 16; i++) {
            CHECK(other.slot[i].text == r.slot[i].text);
            CHECK(other.slot[i].y == r.slot[i].y);
        }
    }
    q2_credits_start(&r, rows, 40);
    ticks = 0;
    do { ticks++; } while (q2_credits_advance(&r, 1.0/60, 60) && ticks < 1000);
    CHECK(ticks == 39 * 16 + 166);
    CHECK(!r.active && !q2_credits_advance(&r, 10.0, 50));
}

static void clipping(void)
{
    static const int sizes[][2] = {{512,248}, {1024,496}, {700,248}, {512,400}};
    int mode;
    for (mode = 0; mode < 4; mode++) {
        q2_screen s;
        psx_ot ot;
        psx_raster_opts opts;
        q2_menu_draw_opts mo;
        q2_menu_font font = {0};
        q2_credit_row row0 = { "NAME", 256, 210 };
        q2_credits r;
        psx_prim *p;
        const psx_framebuffer *fb;
        int x, y, ux, uy, uw, uh, top, bottom, wrong = 0;
        CHECK(q2_screen_init(&s, Q2_VIDEO_PAL) == Q2_OK);
        q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_ONE, 1);
        CHECK(q2_screen_resize(&s, mode != 0, sizes[mode][0], sizes[mode][1]) == Q2_OK);
        CHECK(psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 128) == Q2_OK);
        psx_raster_opts_default(&opts); opts.textures = false; opts.dither = false;
        q2_menu_draw_opts_default(&mo, &font);
        q2_credits_start(&r, &row0, 1);
        q2_screen_frame_begin(&s, &ot);
        psx_fb_clear(q2_screen_back(&s), 0);
        q2_screen_overlay_begin(&s, &ot, NULL);
        q2_credits_build_ot(&r, &ot, &mo, 248);
        /* Paint a full-height red strip in the credits' own bucket. It must
         * be clipped at both ends without acquiring a +60 draw offset. */
        p = psx_ot_add_bucket(&ot, Q2_MENU_OT_BUCKET_ITEM);
        p->kind = PSX_PRIM_TILE;
        p->xy[0].x = 20; p->xy[0].y = 0;
        p->xy[2].x = 60; p->xy[2].y = 248;
        p->rgb[0].r = 255;
        p = psx_ot_add_bucket(&ot, 215 * PSX_OT_SUBDIV);
        p->kind = PSX_PRIM_TILE;
        p->xy[0].x = 80; p->xy[0].y = 220;
        p->xy[2].x = 100; p->xy[2].y = 230;
        p->rgb[0].b = 255;
        q2_screen_overlay_end(&s, &ot);
        q2_screen_compose(&s, &ot, NULL, &opts);
        q2_screen_present(&s);
        fb = q2_screen_front(&s);
        q2_screen_ui_rect(&s, -1, &ux, &uy, &uw, &uh);
        x = ux + 40 * uw / 512;
        top = uy + (int)floor(60.0 * uh / 248 + 0.5);
        bottom = uy + (int)floor(204.0 * uh / 248 + 0.5);
        for (y = 0; y < fb->height; y++)
            if ((fb->px[y * fb->width + x] & 0x7FFF) !=
                (y >= top && y < bottom ? psx_rgb555(255,0,0) : 0)) wrong++;
        if (wrong) fprintf(stderr, "clip mode %d: %d wrong pixels, bounds %d..%d, fb %dx%d\n",
                             mode, wrong, top, bottom, fb->width, fb->height);
        CHECK(wrong == 0);
        x = ux + 90 * uw / 512; y = uy + 225 * uh / 248;
        CHECK((fb->px[y * fb->width + x] & 0x7FFF) == psx_rgb555(0,0,255));
        CHECK(r.frame == 0 && r.slot[0].y == 210); /* drawing is read-only */
        psx_ot_free(&ot); q2_screen_free(&s);
    }
}

int main(void)
{
    tables(); timing(); clipping();
    printf("credits: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
