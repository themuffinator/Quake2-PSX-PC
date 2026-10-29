/* Aspect/layout regression tests use the real client and its framebuffer.
 * No host window, input device, or operating-system capture is involved. */
int q2_test_aspect_client_main(int argc, char **argv);
#define main q2_test_aspect_client_main
#include "../src/client/main.c"
#undef main

static int failures, checks;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    failures++; } } while (0)
#include "client_fixture.h"

static const int sizes[][2] = {
    {1280,1024}, {1024,768}, {1920,1200}, {1920,1080},
    {2560,1080}, {5120,1440}, {1080,1920}, {1080,1080}
};
static const char *names[] = {"5x4", "4x3", "16x10", "16x9",
                             "21x9", "32x9", "9x16", "square"};

/* Nearest-neighbour presentation of the engine render target, with the same
 * fit rectangle as client_present. These evidence files have square pixels. */
static void capture(client *c, const char *dir, const char *name, int aspect)
{
    psx_framebuffer image = {0};
    const psx_framebuffer *src = q2_screen_front(&c->screen);
    char path[1024];
    int x, y, dx, dy, dw, dh;
    int width = sizes[aspect][0] > sizes[aspect][1] ? 960 : 540;
    int height = width * sizes[aspect][1] / sizes[aspect][0];
    if (!dir) return;
    CHECK(psx_fb_init(&image, width, height) == Q2_OK);
    if (!image.px) return;
    q2_screen_fit_rect(&c->screen, c->fit, width, height, &dx, &dy, &dw, &dh);
    for (y = 0; y < dh; y++) for (x = 0; x < dw; x++)
        image.px[(y+dy)*width+x+dx] = src->px[(y*src->height/dh)*src->width + x*src->width/dw];
    snprintf(path, sizeof(path), "%s/%s-%s.ppm", dir, name, names[aspect]);
    CHECK(psx_fb_write_ppm(&image, path) == Q2_OK);
    psx_fb_free(&image);
}

static void menu_pages(void)
{
    q2_screen s;
    q2_menu menu;
    q2_menu_settings settings;
    const q2_menu_page *pages;
    u32 count, page;
    int a, i, video;
    q2_menu_settings_defaults(&settings);
    for (video = 0; video < 2; video++) {
        CHECK(q2_screen_init(&s, video ? Q2_VIDEO_NTSC : Q2_VIDEO_PAL) == Q2_OK);
        q2_menu_init(&menu, &settings, Q2_MENU_SCREEN_H);
        q2_menu_set_fb_height(&menu, s.disp.height);
        /* All authored front-end, in-game and memory-card page tables. */
        for (a = 0; a < 8; a++) {
            int bank;
            q2_screen_resize(&s, true, sizes[a][0], sizes[a][1]);
            for (bank = 0; bank < 2; bank++) {
                pages = bank ? q2_mcard_pages(&count) : q2_menu_pages(&count);
                for (page = 0; page < count; page++) {
                    int ux, uy, uw, uh;
                    menu.page = &pages[page];
                    menu.page_id = pages[page].id;
                    q2_screen_ui_rect(&s, -1, &ux, &uy, &uw, &uh);
                    for (i = pages[page].first; i < pages[page].count; i++) {
                        int x0, y0, x1, y1;
                        q2_menu_hit hit;
                        if (!q2_menu_item_rect(&menu, i, &x0, &y0, &x1, &y1)) continue;
                        /* A row's center survives display->menu inversion and
                         * still hits the same widget, including slider rows. */
                        {
                            double px = ux + ((x0+x1)/2.0) * uw / s.disp.width;
                            double py = uy + ((y0+y1)/2.0 + (s.disp.height-248)/2) * uh / s.disp.height;
                            int mx = (int)floor((px-ux)*s.disp.width/uw + 0.5);
                            int my = (int)floor((py-uy)*s.disp.height/uh - (s.disp.height-248)/2 + 0.5);
                            if (q2_menu_hit_test(&menu, (x0+x1)/2, (y0+y1)/2, &hit)) {
                                int original = hit.index;
                                CHECK(q2_menu_hit_test(&menu, mx, my, &hit));
                                CHECK(hit.index == original);
                            }
                        }
                        CHECK(ux >= 0 && uy >= 0 && ux+uw <= s.buf[0].width && uy+uh <= s.buf[0].height);
                    }
                }
            }
        }
        q2_screen_free(&s);
    }
}

static void disc_frames(const char *disc, const char *dir)
{
    client *c = open_client(disc);
    int a, p;
    c->fit = Q2_SCREEN_FIT_AUTO;
    c->icons_ready = q2_icon_tables_load(&c->icons, c->disc, &c->build) == Q2_OK;
    CHECK(c->icons_ready);
    for (p = 0; p < Q2_MP_MAX_PLAYERS; p++) {
        q2_statusbar_init(&c->sbar[p], &c->icons, 1);
        q2_statusbar_set_palettes(&c->sbar[p], &c->hud_tables);
    }
    CHECK(client_load_zone(c, "BASE0", 0));
    CHECK(c->menu_font_ready && c->hud_font_ready);
    c->settings.v[Q2_SET_CROSSHAIR] = 1;
    for (a = 0; a < 8; a++) {
        static const int pages[] = {Q2_PAGE_PAUSE_SP, Q2_PAGE_VIDEO,
            Q2_PAGE_CONTROLLER, Q2_PAGE_SOUND, Q2_PAGE_PLAYER, Q2_PAGE_QUIT_CONFIRM};
        static const char *page_names[] = {"pause", "video", "controls", "sound", "player", "confirm"};
        c->output_width = sizes[a][0]; c->output_height = sizes[a][1];
        q2_menu_close(&c->menu);
        client_frame(c);
        capture(c, dir, "game", a);
        for (p = 0; p < 6; p++) {
            q2_menu_open(&c->menu);
            q2_menu_goto(&c->menu, pages[p]);
            client_frame(c);
            CHECK(c->ot.prim_count > 50);
            if (a == 4 || a == 6) capture(c, dir, page_names[p], a);
        }
        q2_menu_close(&c->menu);
        c->mission_open = true;
        client_frame(c);
        if (a == 4 || a == 6) capture(c, dir, "mission", a);
        c->mission_open = false;
    }
    CHECK(q2_loading_open(&c->loading, c->disc, &c->hud_tables, &c->settings) == Q2_OK);
    for (a = 4; a <= 6; a += 2) {
        c->output_width = sizes[a][0]; c->output_height = sizes[a][1];
        q2_loading_raise(&c->loading);
        client_frame(c);
        capture(c, dir, "loading", a);
    }
    q2_loading_close(&c->loading);
    for (p = 2; p <= 4; p++) {
        client_mp_configure(c, Q2_MP_DEATHMATCH, p, 10, 10, 3);
        CHECK(client_load_zone(c, "MATRIX5", 0));
        for (a = 0; a < 8; a++) {
            char label[32];
            c->output_width = sizes[a][0]; c->output_height = sizes[a][1];
            client_frame(c);
            CHECK(c->screen.view_count == p);
            snprintf(label, sizeof(label), "split-%d", p);
            if (a == 4 || a == 6) capture(c, dir, label, a);
            if (p == 2) {
                q2_screen_set_layout(&c->screen, Q2_SCREEN_LAYOUT_TWO_H, 2);
                client_frame(c);
                if (a == 4 || a == 6) capture(c, dir, "split-horizontal", a);
                q2_screen_set_layout(&c->screen, Q2_SCREEN_LAYOUT_TWO_V, 2);
            }
        }
    }
    c->mp_scoreboard = true;
    c->mp.end = Q2_MP_END_FRAG_LIMIT;
    CHECK(client_mp_load_results(c));
    for (a = 4; a <= 6; a += 2) {
        c->output_width = sizes[a][0]; c->output_height = sizes[a][1];
        client_frame(c);
        capture(c, dir, "results", a);
    }
    q2_icon_tables_free(&c->icons);
    close_client(c);
}

int main(int argc, char **argv)
{
    menu_pages();
    if (argc == 4 && !strcmp(argv[1], "--headless")) disc_frames(argv[2], argv[3]);
    else if (argc != 1) return 2;
    printf("%d aspect checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
