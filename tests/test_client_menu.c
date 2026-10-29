/* Real menu input and render targets, with SDL-local virtual controllers.
 * All captures are headless; no keyboard/mouse input or OS capture is used. */
int q2_test_menu_client_main(int argc, char **argv);
#define main q2_test_menu_client_main
#include "../src/client/main.c"
#undef main
static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)
#include "client_fixture.h"
#include "controller_fixture.h"

static void button(client *c, SDL_JoystickID id, SDL_GamepadButton b, bool down)
{
    SDL_Event e = {0};
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.which = id; e.gbutton.button = (Uint8)b; e.gbutton.down = down;
    CHECK(q2_gamepads_event(&c->gamepads, &e) >= 0);
}

static void axis(client *c, SDL_JoystickID id, SDL_GamepadAxis a, Sint16 v)
{
    SDL_Event e = {0};
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.which = id; e.gaxis.axis = (Uint8)a; e.gaxis.value = v;
    CHECK(q2_gamepads_event(&c->gamepads, &e) >= 0);
}

static void tick(client *c)
{
    q2_menu_advance(&c->menu, client_menu_pad(c));
}

static void input_checks(void)
{
    client *c = calloc(1, sizeof(*c));
    SDL_JoystickID id, second;
    SDL_Event e = {0};
    char labels[Q2_PROMPT_COUNT][32];
    int i;
    c->headless = c->in_front_end = true;
    q2_menu_settings_defaults(&c->settings);
    q2_menu_init(&c->menu, &c->settings, 248);
    q2_menu_set_front_end(&c->menu, true);
    q2_menu_open(&c->menu);
    q2_menu_goto(&c->menu, Q2_PAGE_FRONT_TITLE);
    tick(c);
    client_prompt_labels(c, labels);
    CHECK(!strcmp(labels[0], "ENTER SELECT") && !strcmp(labels[1], "ESC BACK"));
    CHECK(!strcmp(labels[2], "R RULES"));
    c->fit = Q2_SCREEN_FIT_STRETCH;
    CHECK(client_display_fit(c) == Q2_SCREEN_FIT_AUTO);
    c->fit = Q2_SCREEN_FIT_SQUARE;
    CHECK(client_display_fit(c) == Q2_SCREEN_FIT_AUTO);
    c->fit = Q2_SCREEN_FIT_FULL_4_3;
    CHECK(client_display_fit(c) == Q2_SCREEN_FIT_FULL_4_3);
    id = test_controller(SDL_GAMEPAD_TYPE_PS4); CHECK(id != 0);
    e.type = SDL_EVENT_GAMEPAD_ADDED; e.gdevice.which = id;
    CHECK(q2_gamepads_event(&c->gamepads, &e) == 0);
    client_prompt_labels(c, labels);
    CHECK(!strcmp(labels[0], "CROSS SELECT") && !strcmp(labels[1], "CIRCLE BACK"));
    CHECK(!strcmp(labels[2], "SQUARE RULES"));
    /* A complete tap arrives between renders. Both the press and subsequent
     * release must reach the menu, without becoming held forever. */
    button(c, id, SDL_GAMEPAD_BUTTON_SOUTH, true);
    button(c, id, SDL_GAMEPAD_BUTTON_SOUTH, false);
    tick(c); CHECK(c->menu.page_id == Q2_PAGE_FRONT_START);
    tick(c); CHECK(!(c->menu.pad_cur & Q2_PAD_CROSS));
    tick(c);
    button(c, id, SDL_GAMEPAD_BUTTON_EAST, true);
    button(c, id, SDL_GAMEPAD_BUTTON_EAST, false);
    tick(c); CHECK(c->menu.page_id == Q2_PAGE_FRONT_TITLE);
    tick(c); tick(c);
    axis(c, id, SDL_GAMEPAD_AXIS_LEFTY, 17000);
    tick(c); CHECK(c->menu.cursor == 1);
    axis(c, id, SDL_GAMEPAD_AXIS_LEFTY, 10000);
    CHECK(client_menu_pad(c) & Q2_PAD_DOWN);
    axis(c, id, SDL_GAMEPAD_AXIS_LEFTY, 7000);
    tick(c); CHECK(!(c->menu.pad_cur & Q2_PAD_DOWN));
    axis(c, id, SDL_GAMEPAD_AXIS_LEFTY, -17000);
    tick(c); CHECK(c->menu.cursor == 0);
    axis(c, id, SDL_GAMEPAD_AXIS_LEFTY, 0); tick(c);
    axis(c, id, SDL_GAMEPAD_AXIS_LEFTY, 19000);
    axis(c, id, SDL_GAMEPAD_AXIS_LEFTY, 0);
    tick(c); CHECK(c->menu.cursor == 1);
    tick(c); CHECK(!(c->menu.pad_cur & Q2_PAD_DOWN));
    axis(c, id, SDL_GAMEPAD_AXIS_LEFTY, -19000);
    axis(c, id, SDL_GAMEPAD_AXIS_LEFTY, 0);
    tick(c); CHECK(c->menu.cursor == 0);
    tick(c);
    button(c, id, SDL_GAMEPAD_BUTTON_START, true);
    button(c, id, SDL_GAMEPAD_BUTTON_START, false);
    tick(c); CHECK(c->menu.page_id == Q2_PAGE_FRONT_START);
    tick(c); tick(c);
    q2_menu_goto(&c->menu, Q2_PAGE_FRONT_MULTI); tick(c); tick(c);
    button(c, id, SDL_GAMEPAD_BUTTON_WEST, true);
    button(c, id, SDL_GAMEPAD_BUTTON_WEST, false);
    tick(c); CHECK(c->menu.page_id == Q2_PAGE_FRONT_RULES);
    tick(c); tick(c);
    button(c, id, SDL_GAMEPAD_BUTTON_EAST, true);
    button(c, id, SDL_GAMEPAD_BUTTON_EAST, false);
    tick(c); CHECK(c->menu.page_id == Q2_PAGE_FRONT_MULTI);
    q2_menu_goto(&c->menu, Q2_PAGE_SOUND); tick(c); tick(c);
    for (i = 0; i < (int)c->menu.page->count; i++)
        if (c->menu.page->items[i].widget == Q2_WIDGET_SLIDER) break;
    CHECK(i < (int)c->menu.page->count);
    if (i < (int)c->menu.page->count) {
        int setting = c->menu.page->items[i].setting;
        int previous;
        c->menu.cursor = i;
        c->settings.v[setting] = 5; previous = c->settings.v[setting];
        axis(c, id, SDL_GAMEPAD_AXIS_LEFTX, 20000);
        for (i = 0; i < 12; i++) tick(c);
        CHECK(c->settings.v[setting] > previous);
        axis(c, id, SDL_GAMEPAD_AXIS_LEFTX, 0);
    }
    second = test_controller(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO); CHECK(second != 0);
    e.type = SDL_EVENT_GAMEPAD_ADDED; e.gdevice.which = second;
    CHECK(q2_gamepads_event(&c->gamepads, &e) == 1);
    button(c, second, SDL_GAMEPAD_BUTTON_SOUTH, true);
    client_prompt_labels(c, labels);
    CHECK(!strcmp(labels[0], "B SELECT") && !strcmp(labels[1], "A BACK"));
    e.type = SDL_EVENT_GAMEPAD_REMOVED; e.gdevice.which = second;
    CHECK(q2_gamepads_event(&c->gamepads, &e) == 1);
    client_prompt_labels(c, labels); CHECK(!strcmp(labels[0], "CROSS SELECT"));
    e.gdevice.which = id; CHECK(q2_gamepads_event(&c->gamepads, &e) == 0);
    CHECK(client_menu_pad(c) == 0);
    client_prompt_labels(c, labels); CHECK(!strcmp(labels[0], "ENTER SELECT"));
    CHECK(SDL_DetachVirtualJoystick(id)); CHECK(SDL_DetachVirtualJoystick(second));
    q2_gamepads_close(&c->gamepads);
    free(c);
}

static void write_display(client *c, const char *path, int w, int h)
{
    const psx_framebuffer *src = q2_screen_front(&c->screen);
    psx_framebuffer dst = {0};
    int x, y, dx, dy, dw, dh, ux, uy, uw, uh;
    CHECK(psx_fb_init(&dst, w, h) == Q2_OK);
    if (!dst.px) return;
    /* Exactly the presentation transform used by the game window. The PSX's
     * 512 x 248 storage is not a square-pixel image. */
    q2_screen_fit_rect(&c->screen, client_display_fit(c), w, h, &dx, &dy, &dw, &dh);
    q2_screen_ui_rect(&c->screen, -1, &ux, &uy, &uw, &uh);
    CHECK(fabs((double)uw*dw/src->width / ((double)uh*dh/src->height)-4.0/3.0) < 0.015);
    psx_fb_clear(&dst, 0);
    for (y = 0; y < dh; y++) for (x = 0; x < dw; x++)
        dst.px[(dy+y)*w+dx+x] = src->px[(y*src->height/dh)*src->width+x*src->width/dw];
    CHECK(psx_fb_write_ppm(&dst, path) == Q2_OK);
    psx_fb_free(&dst);
}

static void capture_menu(const char *disc, const char *dir)
{
    client *c = open_client(disc);
    SDL_JoystickID id = 0;
    SDL_Event e = {0};
    int mode, frame;
    static const char *names[] = {"keyboard", "playstation", "xbox", "nintendo"};
    static const SDL_GamepadType types[] = {SDL_GAMEPAD_TYPE_UNKNOWN,
        SDL_GAMEPAD_TYPE_PS4, SDL_GAMEPAD_TYPE_XBOXONE, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO};
    client_enter_front_end(c);
    c->fit = Q2_SCREEN_FIT_AUTO;
    c->output_width = 960; c->output_height = 720;
    CHECK(c->in_front_end && c->menu_font_ready);
    /* The actual title, sub-page and all three prompts, from QFRONT. */
    for (mode = 0; mode < 4; mode++) {
        char path[1024];
        if (mode) {
            id = test_controller(types[mode]);
            e.type = SDL_EVENT_GAMEPAD_ADDED; e.gdevice.which = id;
            CHECK(q2_gamepads_event(&c->gamepads, &e) == 0);
        }
        q2_menu_goto(&c->menu, Q2_PAGE_FRONT_MULTI);
        q2_prompt_sync_menu(&c->prompts, &c->menu, true);
        q2_prompt_snap(&c->prompts);
        for (frame = 0; frame < 30; frame++) {
            const psx_framebuffer *fb;
            int x, y, bright = 0;
            q2_screen_tick_dt(&c->screen, 1.0/30.0);
            client_frame(c);
            c->frame_index++;
            fb = q2_screen_front(&c->screen);
            /* Hotplugging must never erase the selected row or its glyphs. */
            for (y = 73; y < 85; y++) for (x = 160; x < 352; x++) {
                u16 pixel = fb->px[y*fb->width+x];
                if ((pixel&31) > 20 && ((pixel>>5)&31) > 20 && ((pixel>>10)&31) > 20) bright++;
            }
            CHECK(bright > 150);
        }
        snprintf(path, sizeof(path), "%s/menu-%s.ppm", dir, names[mode]);
        write_display(c, path, c->output_width, c->output_height);
        if (id) {
            e.type = SDL_EVENT_GAMEPAD_REMOVED; e.gdevice.which = id;
            CHECK(q2_gamepads_event(&c->gamepads, &e) == 0);
            CHECK(SDL_DetachVirtualJoystick(id)); id = 0;
        }
    }
    {
        static const int sizes[][2] = {{1280,720}, {1440,600}, {540,960}};
        int i;
        for (i = 0; i < 3; i++) {
            char path[1024];
            c->output_width = sizes[i][0]; c->output_height = sizes[i][1];
            q2_screen_tick_dt(&c->screen, 1.0/30.0); client_frame(c);
            snprintf(path, sizeof(path), "%s/menu-aspect-%dx%d.ppm", dir, sizes[i][0], sizes[i][1]);
            write_display(c, path, sizes[i][0], sizes[i][1]);
        }
        c->fit = Q2_SCREEN_FIT_STRETCH;
        c->output_width = 1280; c->output_height = 720;
        client_frame(c);
        CHECK(c->screen.adaptive);
        {
            char path[1024];
            snprintf(path, sizeof(path), "%s/menu-stretch-guard.ppm", dir);
            write_display(c, path, 1280, 720);
        }
    }
    close_client(c);
}

static void capture_controller_profiles(const char *disc, const char *dir)
{
    client *c = open_client(disc);
    int pi, frame;
    client_mp_configure(c, Q2_MP_DEATHMATCH, 4, 20, 10, 3);
    CHECK(client_load_zone(c, "MATRIX5", 0));
    for (pi = 0; pi < 4; pi++) {
        c->gamepads.id[pi] = (SDL_JoystickID)(100 + pi);
        c->settings.v[q2_menu_player_setting(Q2_SET_PAD_STYLE, pi)] =
            (s16)(Q2_PAD_STYLE_RIGHT_STICK + pi % 3);
        c->settings.v[q2_menu_player_setting(Q2_SET_VIBRATION, pi)] = (s16)(pi & 1);
        c->settings.v[q2_menu_player_setting(Q2_SET_SWAP_Y, pi)] = (s16)(pi & 1);
        c->settings.v[q2_menu_player_setting(Q2_SET_MOUSE_SPEED, pi)] = (s16)(24 + pi * 24);
        q2_menu_set_pad_shockable(&c->menu, pi, true);
    }
    client_apply_input(c);
    for (pi = 0; pi < 4; pi++) {
        char path[1024];
        CHECK(client_pause_open(c, pi));
        q2_menu_goto(&c->menu, Q2_PAGE_CONTROLLER);
        for (frame = 0; frame < 10; frame++) {
            client_menu_frame(c);
            q2_prompt_snap(&c->prompts);
            q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
            client_frame(c);
            c->frame_index++;
        }
        CHECK(c->menu.pauser == pi);
        CHECK(c->menu.disabled[3] == (pi != 0));
        snprintf(path, sizeof(path), "%s/controller-player-%d.ppm", dir, pi + 1);
        write_display(c, path, 960, 720);
        client_menu_close(c);
    }
    memset(c->gamepads.id, 0, sizeof(c->gamepads.id));
    close_client(c);
}

static void setup_tap(client *c, SDL_JoystickID id, SDL_GamepadButton b)
{
    client_menu_frame(c); /* Consume a newly opened page's input-settle frame. */
    button(c, id, b, true);
    client_menu_frame(c);
    client_apply_input(c);
    client_audio_pump(c, 1.0/30.0);
    button(c, id, b, false);
    client_menu_frame(c);
    client_apply_input(c);
}

static void setup_capture(client *c, const char *dir, const char *name)
{
    char path[1024];
    int frame;
    for (frame = 0; frame < 12; frame++) {
        q2_input neutral = {0};
        q2_sim_advance(&c->sim[0], &neutral, 1.0/30.0);
        q2_prompt_snap(&c->prompts);
        q2_screen_tick_dt(&c->screen, 1.0/30.0);
        client_frame(c);
        c->frame_index++;
    }
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    write_display(c, path, 960, 720);
}

static void capture_player_setup(const char *disc, const char *dir)
{
    static const char *const names[4] = { "RANGER  ", "SARGE   ", "MAJOR   ", "GRUNT   " };
    client *c = open_client(disc);
    SDL_JoystickID ids[4];
    SDL_Event e = {0};
    int p, letter, steps;
    client_enter_front_end(c);
    c->output_width = 960; c->output_height = 720;
    for (p = 0; p < 4; p++) {
        ids[p] = test_controller(SDL_GAMEPAD_TYPE_PS4); CHECK(ids[p] != 0);
        e.type = SDL_EVENT_GAMEPAD_ADDED; e.gdevice.which = ids[p];
        CHECK(q2_gamepads_event(&c->gamepads, &e) == p);
    }
    client_apply_input(c);
    c->menu.mp_setup.players = 4;
    c->menu.mp_setup.mode = Q2_MENU_MP_TEAM_DEATHMATCH;
    c->menu.mp_setup.frag_option = 0;
    q2_menu_goto(&c->menu, Q2_PAGE_FRONT_DMSETUP);
    c->menu.cursor = 5;
    setup_tap(c, ids[0], SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(c->in_front_end && !c->mp_enabled && c->menu.page_id == Q2_PAGE_FRONT_PLAYERS);
    CHECK(c->screen.view_count == 4);
    setup_capture(c, dir, "player-setup-open");
    /* Release/repress can arrive between two renders, including host aliases.
     * It must affect only this pad, and each edge must survive the latch. */
    setup_tap(c, ids[3], SDL_GAMEPAD_BUTTON_DPAD_UP);
    button(c, ids[3], SDL_GAMEPAD_BUTTON_START, true); client_menu_frame(c);
    CHECK(c->menu.mp_panel[3].page == Q2_MP_PLAYER_OPTIONS);
    button(c, ids[3], SDL_GAMEPAD_BUTTON_START, false);
    button(c, ids[3], SDL_GAMEPAD_BUTTON_START, true); client_menu_frame(c);
    CHECK(c->menu.mp_panel[3].page == Q2_MP_PLAYER_NAME);
    button(c, ids[3], SDL_GAMEPAD_BUTTON_START, false);
    button(c, ids[3], SDL_GAMEPAD_BUTTON_EAST, true); client_menu_frame(c);
    CHECK(c->menu.mp_panel[3].page == Q2_MP_PLAYER_OPTIONS);
    button(c, ids[3], SDL_GAMEPAD_BUTTON_EAST, false);
    button(c, ids[3], SDL_GAMEPAD_BUTTON_EAST, true); client_menu_frame(c);
    CHECK(c->menu.mp_panel[3].page == Q2_MP_PLAYER_MAIN &&
          c->menu.mp_panel[0].page == Q2_MP_PLAYER_MAIN);
    button(c, ids[3], SDL_GAMEPAD_BUTTON_EAST, false); client_menu_frame(c);
    setup_tap(c, ids[3], SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    for (p = 0; p < 4; p++) {
        setup_tap(c, ids[p], SDL_GAMEPAD_BUTTON_DPAD_UP);
        setup_tap(c, ids[p], SDL_GAMEPAD_BUTTON_SOUTH);
        setup_tap(c, ids[p], SDL_GAMEPAD_BUTTON_SOUTH);
        CHECK(c->menu.mp_panel[p].page == Q2_MP_PLAYER_NAME);
        for (letter = 0; letter < 8; letter++) {
            for (steps = 0; steps < 37 && c->menu.mp_name[p][letter] != names[p][letter]; steps++)
                setup_tap(c, ids[p], SDL_GAMEPAD_BUTTON_DPAD_UP);
            CHECK(c->menu.mp_name[p][letter] == names[p][letter]);
            setup_tap(c, ids[p], SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        }
        setup_tap(c, ids[p], SDL_GAMEPAD_BUTTON_EAST);
        setup_tap(c, ids[p], SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        setup_tap(c, ids[p], SDL_GAMEPAD_BUTTON_SOUTH);
        setup_tap(c, ids[p], SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        CHECK(c->menu.mp_team[p] == ((p & 1) ^ 1));
        setup_tap(c, ids[p], SDL_GAMEPAD_BUTTON_EAST);
        setup_tap(c, ids[p], SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        setup_tap(c, ids[p], SDL_GAMEPAD_BUTTON_SOUTH);
        setup_tap(c, ids[p], SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        CHECK(c->settings.v[q2_menu_player_setting(Q2_SET_PAD_STYLE, p)] == 4);
    }
    setup_capture(c, dir, "player-setup-controllers");
    for (p = 0; p < 4; p++) {
        setup_tap(c, ids[p], SDL_GAMEPAD_BUTTON_EAST);
        setup_tap(c, ids[p], SDL_GAMEPAD_BUTTON_EAST);
        setup_tap(c, ids[p], SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        if (p < 3) setup_tap(c, ids[p], SDL_GAMEPAD_BUTTON_SOUTH);
    }
    CHECK(c->in_front_end && !c->mp_enabled);
    setup_capture(c, dir, "player-setup-ready");
    setup_tap(c, ids[3], SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(!c->in_front_end && c->mp_enabled && c->screen.view_count == 4);
    for (p = 0; p < 4; p++) {
        CHECK(c->mp.team[p] == ((p & 1) ^ 1));
        CHECK(!strcmp(c->menu.mp_name[p], names[p]));
        CHECK(client_player_colour(c, p) == ((p & 1) ^ 1));
    }
    c->mp.frags[0] = 4; c->mp.frags[1] = 3;
    c->mp.frags[2] = 2; c->mp.frags[3] = 1;
    c->mp.team_frags[0] = 4; c->mp.team_frags[1] = 6;
    c->mp.time_limit = 1;
    c->sim[0].level_time = 300 * 61;
    for (steps = 0; steps < 100 && !c->mp_scoreboard; steps++) client_mp_tick(c, 10);
    CHECK(c->mp_scoreboard);
    CHECK(client_mp_load_results(c));
    setup_capture(c, dir, "player-setup-results");
    client_mp_return_to_menu(c);
    CHECK(c->in_front_end && c->menu.page_id == Q2_PAGE_FRONT_MULTI);
    CHECK(c->screen.view_count == 1 && !strcmp(c->menu.mp_name[0], names[0]));
    for (steps = 2; steps <= 3; steps++) {
        c->menu.mp_setup.players = (s16)steps;
        c->menu.mp_setup.mode = steps == 2 ? Q2_MENU_MP_DEATHMATCH : Q2_MENU_MP_VERSUS;
        c->settings.v[Q2_SET_HORIZONTAL_SPLIT] = 0;
        q2_menu_goto(&c->menu, Q2_PAGE_FRONT_DMSETUP);
        c->menu.cursor = (int)c->menu.page->count - 1;
        setup_tap(c, ids[0], SDL_GAMEPAD_BUTTON_SOUTH);
        CHECK(c->in_front_end && !c->mp_enabled && c->screen.view_count == steps);
        CHECK(c->screen.layout == (steps == 2 ? Q2_SCREEN_LAYOUT_TWO_H : Q2_SCREEN_LAYOUT_QUAD));
        setup_capture(c, dir, steps == 2 ? "player-setup-two" : "player-setup-three");
        for (p = 0; p < steps; p++) {
            CHECK(c->in_front_end && !c->mp_enabled);
            setup_tap(c, ids[p], SDL_GAMEPAD_BUTTON_SOUTH);
        }
        CHECK(c->mp_enabled && c->mp.player_count == steps && !c->in_front_end);
        CHECK(c->screen.layout == (steps == 2 ? Q2_SCREEN_LAYOUT_TWO_V : Q2_SCREEN_LAYOUT_QUAD));
        client_mp_return_to_menu(c);
        CHECK(c->screen.view_count == 1 && !strcmp(c->menu.mp_name[0], names[0]));
    }
    for (p = 0; p < 4; p++) {
        e.type = SDL_EVENT_GAMEPAD_REMOVED; e.gdevice.which = ids[p];
        CHECK(q2_gamepads_event(&c->gamepads, &e) == p);
        CHECK(SDL_DetachVirtualJoystick(ids[p]));
    }
    close_client(c);
}

static void capture_credits(const char *disc, const char *dir)
{
    client *c = open_client(disc);
    q2_ai_module relocated = {0};
    q2_credit_row rows[Q2_CREDITS_MAX];
    q2_credits saved;
    SDL_JoystickID id;
    SDL_Event e = {0};
    int frame, nonempty = 0;
    char path[1024];
    client_enter_front_end(c);
    c->output_width = 960; c->output_height = 720;
    c->menu.request = Q2_MREQ_CREDITS;
    client_menu_requests(c);
    CHECK(c->credits_open && !c->menu.open);
    CHECK(c->credits.count == 177);
    if (!c->credits_open || c->credits.count != 177) { close_client(c); return; }
    CHECK(!strcmp(c->credit_rows[0].text, "DEVELOPED BY"));
    CHECK(!c->credit_rows[1].text[0]);
    CHECK(!strcmp(c->credit_rows[2].text, "HAMMERHEAD LTD"));
    CHECK(!strcmp(c->credit_rows[175].text, "DAVID GRIJNS"));
    for (frame = 0; frame < (int)c->credits.count; frame++)
        if (c->credit_rows[frame].text[0]) nonempty++;
    CHECK(nonempty == 103);
    CHECK(q2_level_module_load(&relocated, &c->common, 0x80100000) == Q2_OK);
    CHECK(q2_levelbin_credits(relocated.image.data, (u32)relocated.image.size,
                               relocated.base, rows, Q2_CREDITS_MAX) == c->credits.count);
    for (frame = 0; frame < (int)c->credits.count; frame++)
        CHECK(!strcmp(rows[frame].text, c->credit_rows[frame].text));
    q2_ai_module_free(&relocated);
    for (frame = 1; frame <= 150; frame++) {
        client_credits_frame(c, 1.0/30.0);
        q2_sim_scene_advance(&c->sim[0], 1.0/30.0);
        client_scene_lights(c);
        q2_screen_tick_dt(&c->screen, 1.0/30.0);
        client_frame(c); c->frame_index++;
        if (frame == 60 || frame == 150) {
            snprintf(path, sizeof(path), "%s/credits-after-%d.ppm", dir, frame);
            write_display(c, path, 960, 720);
        }
    }
    saved = c->credits;
    client_frame(c); client_frame(c);
    CHECK(!memcmp(&saved, &c->credits, sizeof(saved)));
    c->output_width = 1440; c->output_height = 600;
    client_frame(c);
    snprintf(path, sizeof(path), "%s/credits-wide.ppm", dir);
    write_display(c, path, 1440, 600);
    CHECK(!memcmp(&saved, &c->credits, sizeof(saved)));
    /* PAL's last name is injected at 2800; it clears at 2965 and the
     * completion callback runs on 2966. The first 150 host frames used 250. */
    for (frame = 250; frame < 2965; frame++) client_credits_frame(c, 1.0/50.0);
    CHECK(c->credits_open && !c->menu.open);
    client_credits_frame(c, 1.0/50.0);
    CHECK(!c->credits_open && c->menu.open && c->menu.page_id == Q2_PAGE_FRONT_OPTIONS);
    c->output_width = 960; c->output_height = 720;
    client_frame(c);
    snprintf(path, sizeof(path), "%s/credits-complete.ppm", dir);
    write_display(c, path, 960, 720);
    id = test_controller(SDL_GAMEPAD_TYPE_PS4); CHECK(id != 0);
    e.type = SDL_EVENT_GAMEPAD_ADDED; e.gdevice.which = id;
    CHECK(q2_gamepads_event(&c->gamepads, &e) == 0);
    button(c, id, SDL_GAMEPAD_BUTTON_EAST, true);
    client_credits_open(c, false);
    client_credits_frame(c, 1.0/30.0);
    CHECK(c->credits_open); /* An already held Back does not dismiss entry. */
    button(c, id, SDL_GAMEPAD_BUTTON_EAST, false);
    memset(c->gamepads.menu_pressed, 0, sizeof(c->gamepads.menu_pressed));
    client_credits_frame(c, 1.0/30.0);
    button(c, id, SDL_GAMEPAD_BUTTON_SOUTH, true);
    client_credits_frame(c, 1.0/30.0);
    CHECK(c->credits_open); /* Native Confirm has no action on credits. */
    button(c, id, SDL_GAMEPAD_BUTTON_SOUTH, false);
    button(c, id, SDL_GAMEPAD_BUTTON_EAST, true);
    button(c, id, SDL_GAMEPAD_BUTTON_EAST, false);
    client_credits_frame(c, 1.0/30.0);
    CHECK(!c->credits_open && c->menu.page_id == Q2_PAGE_FRONT_OPTIONS);
    e.type = SDL_EVENT_GAMEPAD_REMOVED;
    CHECK(q2_gamepads_event(&c->gamepads, &e) == 0);
    CHECK(SDL_DetachVirtualJoystick(id));
    close_client(c);
}

static SDL_JoystickID unlock_card_init(client *c)
{
    SDL_Event e = {0};
    SDL_JoystickID id = test_controller(SDL_GAMEPAD_TYPE_PS4);
    CHECK(id != 0);
    e.type = SDL_EVENT_GAMEPAD_ADDED; e.gdevice.which = id;
    CHECK(q2_gamepads_event(&c->gamepads, &e) == 0);
    q2_save_ui_init(&c->save_ui);
    c->mcard_host.poll = q2_save_ui_poll;
    c->mcard_host.request = q2_save_ui_request;
    c->mcard_host.choose = q2_save_ui_choose;
    c->mcard_host.user = &c->save_ui;
    q2_menu_init(&c->card_menu, &c->settings, Q2_MENU_SCREEN_H);
    q2_menu_set_fb_height(&c->card_menu, c->height);
    return id;
}

static void unlock_card_tap(client *c, SDL_JoystickID id, SDL_GamepadButton b)
{
    button(c, id, b, true); client_card_frame(c);
    button(c, id, b, false);
    if (c->mcard_open) client_card_frame(c);
    if (c->mcard_open) client_card_frame(c);
    /* Drain the menu tap latch even when the first frame closed the card. */
    (void)client_menu_pad(c);
}

static void unlock_yes(client *c)
{
    CHECK(c->menu.page_id == Q2_PAGE_FRONT_UNLOCK);
    q2_menu_advance(&c->menu, 0);
    q2_menu_advance(&c->menu, 0);
    (void)q2_menu_point_at(&c->menu, 4);
    q2_menu_advance(&c->menu, Q2_PAD_CROSS);
    CHECK(c->menu.request == Q2_MREQ_UNLOCK_SAVE);
    client_menu_requests(c);
    CHECK(c->mcard_open && c->card_mode == Q2_SAVE_UI_UNLOCK_SAVE);
    client_card_frame(c);
}

static void capture_unlocks(const char *disc, const char *dir)
{
    client *c = open_client(disc), *cold;
    q2_settings_blob saved, other;
    SDL_JoystickID id = unlock_card_init(c), cold_id;
    q2_ai_module module = {0};
    const q2_menu_page *page = q2_menu_page_find(Q2_PAGE_FRONT_UNLOCK);
    char path[1024];
    int skill, i;
    s32 old_skill = q2_cre_skill();
    q2_save_set_dir(dir);
    for (i = 0; i < Q2_SAVE_SLOTS; i++) q2_settings_slot_delete(i);
    client_enter_front_end(c);
    /* Verify the six rows against the actual relocated QFRONT records. */
    CHECK(q2_level_module_load(&module, &c->common, 0x80100000) == Q2_OK);
    for (i = 0; i < 6; i++) {
        u32 address = i < 4 ? 0xF044 + i * 24 : 0xF0BC + (i - 4) * 24;
        const u8 *record = module.image.data + address;
        u32 text = q2_rd_u32(record) - module.base;
        CHECK(text < module.image.size);
        CHECK(!strcmp((const char *)module.image.data + text, page->items[i].label));
        CHECK((s16)q2_rd_u16(record + 4) == page->items[i].x);
        CHECK((s16)q2_rd_u16(record + 6) == page->items[i].y);
    }
    q2_ai_module_free(&module);
    for (skill = 0; skill < 3; skill++) {
        q2_cre_set_skill(skill);
        q2_menu_set_cheat_level(&c->menu, 3); /* Award is assignment, not max. */
        client_campaign_complete(c);
        CHECK(c->in_front_end && c->credits_open && c->credits_campaign);
        CHECK(!c->menu.open);
        if (skill == 1) {
            for (i = 0; i < 2966; i++) client_credits_frame(c, 1.0/50.0);
        } else {
            button(c, id, SDL_GAMEPAD_BUTTON_EAST, true);
            button(c, id, SDL_GAMEPAD_BUTTON_EAST, false);
            client_credits_frame(c, 1.0/30.0);
        }
        CHECK(!c->credits_open && !c->credits_campaign && c->menu.open);
        CHECK(c->menu.page_id == Q2_PAGE_FRONT_UNLOCK && c->menu.cursor == 4);
        CHECK(c->menu.cheat_level == skill + 1);
        CHECK(strstr(q2_menu_item_text(&c->menu, 3),
                     q2_menu_cheat_level_name(skill + 1)) != NULL);
        c->output_width = 960; c->output_height = 720;
        client_frame(c);
        snprintf(path, sizeof(path), "%s/award-%d.ppm", dir, skill + 1);
        write_display(c, path, 960, 720);
        /* The question has no Back callback. No preserves the live award. */
        q2_menu_advance(&c->menu, 0);
        q2_menu_advance(&c->menu, Q2_PAD_TRIANGLE);
        CHECK(c->menu.page_id == Q2_PAGE_FRONT_UNLOCK);
        q2_menu_advance(&c->menu, 0);
        CHECK(q2_menu_point_at(&c->menu, 5));
        q2_menu_advance(&c->menu, Q2_PAD_CROSS);
        CHECK(c->menu.page_id == Q2_PAGE_FRONT_TITLE && c->menu.cheat_level == skill + 1);
        CHECK(q2_settings_slot_read(&saved, skill) == Q2_ERR_NOT_FOUND);
        q2_menu_goto(&c->menu, Q2_PAGE_FRONT_VARIABLES);
        CHECK(c->menu.page->count == (u32)(5 + 2 * skill));
        q2_menu_goto(&c->menu, Q2_PAGE_FRONT_UNLOCK);
        unlock_yes(c);
        unlock_card_tap(c, id, SDL_GAMEPAD_BUTTON_EAST);
        CHECK(!c->mcard_open && c->menu.page_id == Q2_PAGE_FRONT_UNLOCK);
        CHECK(c->menu.cursor == 4 && c->menu.cheat_level == skill + 1);
        unlock_yes(c);
        (void)q2_menu_point_at(&c->card_menu, skill + 1);
        unlock_card_tap(c, id, SDL_GAMEPAD_BUTTON_SOUTH);
        CHECK(c->mcard_open && c->save_ui.status == Q2_SAVE_UI_SAVED);
        CHECK(!strcmp(c->save_ui.message, "UNLOCK FILE SAVED"));
        CHECK(q2_settings_slot_read(&saved, skill) == Q2_OK);
        CHECK(saved.cheat_level == skill + 1 && saved.unlock_file);
        unlock_card_tap(c, id, SDL_GAMEPAD_BUTTON_SOUTH);
        CHECK(!c->mcard_open && c->menu.page_id == Q2_PAGE_FRONT_MULTI);

        /* A fresh client sees no unlock until LOAD SETTINGS applies the file. */
        cold = open_client(disc); cold_id = unlock_card_init(cold);
        client_enter_front_end(cold);
        CHECK(cold->menu.cheat_level == 0);
        q2_menu_goto(&cold->menu, Q2_PAGE_FRONT_MULTI);
        q2_menu_advance(&cold->menu, 0);
        CHECK(q2_menu_point_at(&cold->menu, 3));
        q2_menu_advance(&cold->menu, Q2_PAD_CROSS);
        client_menu_requests(cold);
        CHECK(cold->mcard_open && cold->card_mode == Q2_SAVE_UI_SETTINGS_LOAD);
        client_card_frame(cold);
        CHECK(strstr(q2_save_ui_row(&cold->save_ui, skill), "MULTI UNLOCK") != NULL);
        (void)q2_menu_point_at(&cold->card_menu, skill + 1);
        unlock_card_tap(cold, cold_id, SDL_GAMEPAD_BUTTON_SOUTH);
        CHECK(cold->save_ui.status == Q2_SAVE_UI_LOADED && cold->menu.cheat_level == 0);
        unlock_card_tap(cold, cold_id, SDL_GAMEPAD_BUTTON_SOUTH);
        CHECK(!cold->mcard_open && cold->menu.cheat_level == skill + 1);
        q2_menu_goto(&cold->menu, Q2_PAGE_FRONT_VARIABLES);
        CHECK(cold->menu.page->count == (u32)(5 + 2 * skill));
        if (skill == 2) {
            cold->output_width = 960; cold->output_height = 720;
            client_frame(cold);
            snprintf(path, sizeof(path), "%s/gold-variables.ppm", dir);
            write_display(cold, path, 960, 720);
            /* The restored tier exposes the real ALL WEAPONS toggle. */
            q2_menu_advance(&cold->menu, 0);
            (void)q2_menu_point_at(&cold->menu, 5);
            CHECK(cold->menu.page->items[5].setting == Q2_SET_ALL_WEAPONS);
            q2_menu_advance(&cold->menu, Q2_PAD_LEFT);
            CHECK(cold->settings.v[Q2_SET_ALL_WEAPONS] == 1);
            cold->menu.mp_setup.players = 4;
            CHECK(client_mp_start_from_menu(cold));
            CHECK(cold->sim[0].player_count == 4);
            for (i = 0; i < 4; i++) {
                q2_inventory *inv = i == cold->sim[0].cur_player
                    ? &cold->sim[0].combat.inv : &cold->sim[0].pcombat[i].inv;
                inv->ammo_tier = (u8)(i % 3);
                memset(inv->ammo, 0, sizeof(inv->ammo));
                inv->weapons = 1; inv->current_weapon = Q2_WEAPON_BLASTER;
            }
            cold->menu.request = Q2_MREQ_RESUME;
            client_menu_requests(cold);
            for (i = 1; i <= Q2_WID_COUNT; i++) {
                const q2_weapon_tables *t = q2_weapon_tables_builtin();
                if (q2_model_bank_find(&cold->model_bank, t->name[i]) >= 0)
                    CHECK(cold->sim[0].combat.inv.weapons & t->owned_bit[i]);
            }
            for (i = 0; i < Q2_AMMO_COUNT; i++)
                CHECK(cold->sim[0].combat.inv.ammo[i] ==
                      q2_inventory_ammo_max(&cold->sim[0].combat.inv, (q2_ammo)i));
            cold->sim[0].combat.inv.ammo[0]--;
            client_apply_settings(cold);
            CHECK(cold->sim[0].combat.inv.ammo[0] ==
                  q2_inventory_ammo_max(&cold->sim[0].combat.inv, (q2_ammo)0) - 1);
            for (i = 1; i < 4; i++) {
                q2_inventory *inv = &cold->sim[0].pcombat[i].inv;
                int a;
                CHECK(inv->current_weapon == Q2_WEAPON_BLASTER);
                for (a = 0; a < Q2_AMMO_COUNT; a++)
                    CHECK(inv->ammo[a] == q2_inventory_ammo_max(inv, (q2_ammo)a));
                CHECK(inv->weapons == cold->sim[0].combat.inv.weapons);
            }
            CHECK(client_mp_respawn(cold, 2));
            CHECK(cold->sim[0].pcombat[2].inv.weapons == cold->sim[0].combat.inv.weapons);
            for (i = 0; i < Q2_AMMO_COUNT; i++)
                CHECK(cold->sim[0].pcombat[2].inv.ammo[i] ==
                      q2_inventory_ammo_max(&cold->sim[0].pcombat[2].inv, (q2_ammo)i));
            cold->settings.v[Q2_SET_ALL_WEAPONS] = 0;
            client_install_game_variables(cold);
            CHECK(cold->sim[0].combat.inv.ammo[0] ==
                  q2_inventory_ammo_max(&cold->sim[0].combat.inv, (q2_ammo)0) - 1);
        }
        /* Ordinary settings retain the tier too, but use their usual label. */
        client_settings_pack(cold, &other);
        CHECK(!other.unlock_file && other.cheat_level == skill + 1);
        CHECK(q2_settings_slot_write(&other, 3) == Q2_OK);
        CHECK(q2_settings_slot_read(&saved, skill) == Q2_OK && saved.unlock_file);
        CHECK(saved.cheat_level == skill + 1);
        q2_save_ui_free(&cold->save_ui);
        close_client(cold); CHECK(SDL_DetachVirtualJoystick(cold_id));
    }
    /* Overwrite refusal leaves an older reward intact. */
    q2_menu_set_cheat_level(&c->menu, 3);
    q2_menu_open(&c->menu); q2_menu_goto(&c->menu, Q2_PAGE_FRONT_UNLOCK);
    unlock_yes(c);
    unlock_card_tap(c, id, SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(c->save_ui.state == Q2_SAVEUI_STATE_CHOICE);
    q2_save_ui_request(&c->save_ui, Q2_SAVEUI_STATE_START);
    CHECK(q2_settings_slot_read(&saved, 0) == Q2_OK && saved.cheat_level == 1);
    client_card_close(c);
    /* A failed write is reported and Back returns to the reward for retry. */
    q2_settings_slot_path(0, path, sizeof(path));
    q2_save_set_dir(path); /* An existing file cannot be the save directory. */
    unlock_yes(c);
    unlock_card_tap(c, id, SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(c->save_ui.status == Q2_SAVE_UI_FAILED && c->mcard_open);
    unlock_card_tap(c, id, SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(c->menu.page_id == Q2_PAGE_FRONT_UNLOCK && c->menu.cheat_level == 3);
    q2_save_set_dir(dir);
    CHECK(q2_settings_slot_read(&saved, 0) == Q2_OK && saved.cheat_level == 1);
    /* Options credits keep their own return path after a campaign run. */
    client_credits_open(c, false); client_credits_close(c);
    CHECK(c->menu.page_id == Q2_PAGE_FRONT_OPTIONS && c->menu.cheat_level == 3);
    for (i = 0; i < Q2_SAVE_SLOTS; i++) q2_settings_slot_delete(i);
    q2_save_set_dir(NULL);
    q2_save_ui_free(&c->save_ui);
    close_client(c); CHECK(SDL_DetachVirtualJoystick(id));
    /* BASE1 has a smaller CastList than an arena: absent weapons must not be
     * fabricated, and the current weapon is retained even with more ammo. */
    c = open_client(disc);
    CHECK(client_load_zone(c, "BASE1", 0));
    {
        const q2_weapon_tables *t = q2_weapon_tables_builtin();
        u16 expected = c->sim[0].combat.inv.weapons;
        int held = c->sim[0].combat.weapon_id, missing = 0;
        for (i = 1; i <= Q2_WID_COUNT; i++) {
            if (q2_model_bank_find(&c->model_bank, t->name[i]) >= 0)
                expected |= (u16)t->owned_bit[i];
            else missing++;
        }
        CHECK(missing > 0);
        c->settings.v[Q2_SET_ALL_WEAPONS] = 1;
        q2_menu_set_multiplayer(&c->menu, true);
        client_install_game_variables(c);
        CHECK(c->sim[0].combat.inv.weapons == expected);
        CHECK(c->sim[0].combat.weapon_id == held);
    }
    close_client(c);
    q2_cre_set_skill(old_skill);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--headless")) { argc--; argv++; }
    CHECK(SDL_Init(SDL_INIT_GAMEPAD));
    input_checks();
    if (argc > 3 && !strcmp(argv[3], "--unlocks"))
        capture_unlocks(argv[1], argv[2]);
    else if (argc > 3 && !strcmp(argv[3], "--credits"))
        capture_credits(argv[1], argv[2]);
    else if (argc > 3 && !strcmp(argv[3], "--players"))
        capture_player_setup(argv[1], argv[2]);
    else if (argc > 3 && !strcmp(argv[3], "--profiles"))
        capture_controller_profiles(argv[1], argv[2]);
    else if (argc > 2) capture_menu(argv[1], argv[2]);
    SDL_Quit();
    printf("menu client: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
