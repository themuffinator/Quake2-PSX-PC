/* Actual selector rendering and ownership checks. No host input is read or
 * injected. GPU checks use a hidden window and engine render targets. */
#include "../src/client/launcher.c"
#include "controller_fixture.h"

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static void fixture_sources(launcher *l)
{
    int i;
    l->count = 12;
    for (i = 0; i < l->count; i++) {
        src_entry *s = &l->src[i];
        s->kind = i < 6 ? SRC_IMAGE : SRC_DRIVE;
        s->state = (src_state)(i % 5);
        snprintf(s->title, sizeof(s->title), "Quake II source %02d", i+1);
        snprintf(s->path, sizeof(s->path), "E:/A long disc folder/Quake II %02d.cue", i+1);
        copy_str(s->tag, sizeof(s->tag), "SLES-01534 - PAL");
    }
}

static void layout_checks(launcher *l)
{
    static const int sizes[][2] = {{960,600}, {480,300}, {1440,900},
                                   {2560,1080}, {600,960}};
    size_t i;
    fixture_sources(l);
    l->sel = 2;
    CHECK(!activate(l));
    l->sel = 0;
    l->dialog_open = true;
    CHECK(!activate(l)); /* browsing never launches the previous source */
    l->dialog_open = false;
    for (i = 0; i < Q2PSX_ARRAY_COUNT(sizes); i++) {
        SDL_FRect b;
        CHECK(resize_surface(l, sizes[i][0], sizes[i][1]));
        l->sel = 0;
        l->scroll = 0;
        draw(l);
        CHECK(present(l));
        CHECK(fabsf(px_x(l, L_W/2)-sizes[i][0]/2.0f) < 0.01f);
        CHECK(fabsf(px_y(l, L_H/2)-sizes[i][1]/2.0f) < 0.01f);
        /* Logical points, never the user's pointer. */
        l->mx = L_MARGIN+L_LIST_W/2;
        l->my = L_LIST_Y+L_CARD_H/2;
        hit_test(l);
        CHECK(l->hover == 0);
        l->mx = L_MARGIN+L_LIST_W+1;
        hit_test(l);
        CHECK(l->hover == -1);
        move_selection(l, 100);
        CHECK(l->sel == 11);
        CHECK(list_top(l, 11)+L_CARD_H <= L_LIST_Y+L_LIST_H+0.01f);
        l->mx = L_MARGIN+20;
        l->my = list_top(l, 11)+L_CARD_H/2;
        hit_test(l);
        CHECK(l->hover == 11);
        move_selection(l, -100);
        CHECK(l->sel == 0 && l->scroll == 0);
        button_rect(BTN_PLAY, &b);
        l->mx = b.x+b.w/2; l->my = b.y+b.h/2;
        hit_test(l);
        CHECK(l->hover_button == BTN_PLAY);
        CHECK(l->hover == -1);
        if (l->ox > 2 || l->oy > 2) {
            u8 *pixel = l->surface->pixels;
            CHECK(pixel[0] == 8 && pixel[1] == 10 && pixel[2] == 12);
        }
    }
    l->sel = 2; CHECK(!activate(l) && l->status_error);
    l->sel = 3; CHECK(!activate(l) && l->status_error);
    l->sel = 4; CHECK(!activate(l) && l->status_error);
    l->count = 0; l->sel = 0;
    CHECK(!activate(l));
    draw(l);
    CHECK(present(l));
    {
        char fitted[16];
        fit_middle(l, "E:/a/long/path/disc.cue", 1, 12*8, fitted, sizeof(fitted));
        CHECK(strlen(fitted) == 12 && strstr(fitted, "...") && strstr(fitted, ".cue"));
        fit_middle(l, "a", 1, 0, fitted, sizeof(fitted));
        CHECK(fitted[0] == 0);
    }
}

static void controller_checks(launcher *l)
{
    SDL_JoystickID id, other;
    SDL_Event e = {0};
    char hint[256];
    int before;
    CHECK(SDL_InitSubSystem(SDL_INIT_GAMEPAD));
    id = test_controller(SDL_GAMEPAD_TYPE_PS4);
    other = test_controller(SDL_GAMEPAD_TYPE_XBOXONE);
    CHECK(id && other);
    open_pad(l, id); open_pad(l, id); open_pad(l, other);
    CHECK(pad_index(l, id) == 0 && pad_index(l, other) == 1);
    CHECK(l->pads[2].device == NULL); /* initial enumeration + added event */
    control_hint(l, hint, sizeof(hint));
    CHECK(strstr(hint, "CROSS start") && strstr(hint, "CIRCLE quit"));
    fixture_sources(l); l->sel = 0;
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION; e.gaxis.which = id;
    e.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTY; e.gaxis.value = 18000;
    CHECK(handle(l, &e) == RUN_ON && l->sel == 1);
    pad_direction(l, 0, l->pads[0].repeat_at-1); CHECK(l->sel == 1);
    pad_direction(l, 0, l->pads[0].repeat_at); CHECK(l->sel == 2);
    e.gaxis.value = 12000; handle(l, &e); CHECK(l->pads[0].direction == 1);
    e.gaxis.value = 7000; handle(l, &e); CHECK(l->pads[0].direction == 0);
    e.gaxis.value = -18000; handle(l, &e); CHECK(l->sel == 1);
    e.gaxis.value = 0; handle(l, &e);
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; e.gbutton.which = id;
    e.gbutton.button = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    handle(l, &e); CHECK(l->sel == 2);
    pad_direction(l, 0, l->pads[0].repeat_at); CHECK(l->sel == 3);
    e.type = SDL_EVENT_GAMEPAD_BUTTON_UP; handle(l, &e);
    CHECK(l->pads[0].direction == 0);
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; e.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    handle(l, &e); CHECK(l->armed_pad == id);
    e.type = SDL_EVENT_GAMEPAD_BUTTON_UP; e.gbutton.which = other;
    handle(l, &e); CHECK(l->armed_pad == id);
    e.gbutton.which = id; e.gbutton.button = SDL_GAMEPAD_BUTTON_START;
    handle(l, &e); CHECK(l->armed_pad == id);
    e.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    CHECK(handle(l, &e) == RUN_ON && !l->armed_pad && l->status_error);
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    handle(l, &e);
    e.type = SDL_EVENT_WINDOW_FOCUS_LOST; handle(l, &e);
    CHECK(!l->armed_pad && !l->pads[0].direction);
    before = l->sel;
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; e.gbutton.button = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    handle(l, &e); CHECK(l->sel == before);
    e.type = SDL_EVENT_WINDOW_FOCUS_GAINED; handle(l, &e);
    /* Browser starts at the executable, whose parent is a real directory.
     * Read-only traversal tests actual file enumeration, never user input. */
    l->count = 0;
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; e.gbutton.button = SDL_GAMEPAD_BUTTON_WEST;
    handle(l, &e);
    CHECK(l->dialog_open && l->file_count > 0 && !strcmp(l->files[0].name, ".."));
    control_hint(l, hint, sizeof(hint));
    CHECK(strstr(hint, "CROSS open") && strstr(hint, "CIRCLE cancel"));
    e.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH; handle(l, &e);
    e.type = SDL_EVENT_GAMEPAD_BUTTON_UP; handle(l, &e);
    CHECK(l->dialog_open && !l->file_error);
    move_selection(l, 100000); CHECK(l->file_sel == l->file_count-1);
    CHECK(l->file_sel < l->file_top+L_FILE_ROWS);
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; e.gbutton.button = SDL_GAMEPAD_BUTTON_EAST;
    CHECK(handle(l, &e) == RUN_ON && !l->dialog_open);
    e.type = SDL_EVENT_GAMEPAD_REMOVED; e.gdevice.which = id; handle(l, &e);
    control_hint(l, hint, sizeof(hint));
    CHECK(strstr(hint, "A start") && strstr(hint, "B quit"));
    e.gdevice.which = other; handle(l, &e);
    control_hint(l, hint, sizeof(hint)); CHECK(strstr(hint, "ENTER start"));
    CHECK(SDL_DetachVirtualJoystick(id)); CHECK(SDL_DetachVirtualJoystick(other));
}

static void gpu_handoff(void)
{
    launcher *l = SDL_calloc(1, sizeof(*l));
    q2_launcher_host host;
    SDL_WindowID id;
    psx_framebuffer fb = {0};
    u8 rgba[64*64*4];
    int w, h;
    if (!l || !SDL_InitSubSystem(SDL_INIT_VIDEO)) { SDL_free(l); return; }
    q2_gl_attributes();
    l->window = SDL_CreateWindow("Selector handoff test", 960, 600,
                                  SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);
    l->display = l->window ? q2_gl_create(l->window) : NULL;
    if (!l->display) {
        fprintf(stderr, "SKIP GPU handoff: %s\n", SDL_GetError());
        shutdown_ui(l, false); SDL_free(l); return;
    }
    CHECK((SDL_GetWindowFlags(l->window) & SDL_WINDOW_HIDDEN) != 0);
    CHECK(resize_surface(l, 960, 600));
    draw(l);
    CHECK(present(l));
    id = SDL_GetWindowID(l->window);
    host.window = l->window; host.display = l->display;
    shutdown_ui(l, true);
    SDL_free(l);
    /* The handed-over host must still render after all UI surfaces are freed. */
    CHECK(SDL_GetWindowID(host.window) == id);
    SDL_GetWindowSize(host.window, &w, &h);
    CHECK(w == 960 && h == 600);
    CHECK(psx_fb_init(&fb, 64, 64) == Q2_OK);
    if (fb.px) {
        int i;
        for (i = 0; i < 64*64; i++) fb.px[i] = psx_rgb555(255, 0, 0);
        CHECK(q2_gl_render(host.display, &fb, 64, 64, false));
        CHECK(q2_gl_read(host.display, rgba));
        CHECK(rgba[0] > 240 && rgba[1] == 0 && rgba[2] == 0);
        psx_fb_free(&fb);
    }
    q2_launcher_host_close(&host);
    CHECK(!host.window && !host.display);
}

static void disc_controller_flow(launcher *l, const char *disc_path)
{
    SDL_Event e = {0};
    SDL_JoystickID id = SDL_GetGamepadID(prompt_pad(l));
    int i;
    CHECK(id != 0 && l->dialog_open);
    l->cfg_path = SDL_getenv("Q2PSX_LAUNCHER_CFG");
    CHECK(l->cfg_path != NULL);
    if (!l->cfg_path || !id) return;
    for (i = 0; i < l->file_count; i++)
        if (!strcmp(l->files[i].name, path_tail(disc_path))) break;
    CHECK(i < l->file_count);
    if (i >= l->file_count) return;
    /* Navigate the real directory and accept the actual disc using only
     * internal controller events; no input is sent to the user's computer. */
    while (l->file_sel < i) {
        e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; e.gbutton.which = id;
        e.gbutton.button = SDL_GAMEPAD_BUTTON_DPAD_DOWN; handle(l, &e);
        e.type = SDL_EVENT_GAMEPAD_BUTTON_UP; handle(l, &e);
    }
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; e.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    CHECK(handle(l, &e) == RUN_ON && l->dialog_open);
    e.type = SDL_EVENT_GAMEPAD_BUTTON_UP;
    CHECK(handle(l, &e) == RUN_ON && !l->dialog_open);
    CHECK(l->count > 0 && l->src[l->sel].state == SRC_READY);
    CHECK(q2_launch_path_same(l->src[l->sel].path, disc_path));
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    CHECK(handle(l, &e) == RUN_ON);
    e.type = SDL_EVENT_GAMEPAD_BUTTON_UP;
    CHECK(handle(l, &e) == RUN_PLAY);
    CHECK(q2_launch_path_same(l->cfg.last, disc_path));
}

int main(int argc, char **argv)
{
    launcher *l = SDL_calloc(1, sizeof(*l));
    int width = 960, height = 600;
    if (argc > 1 && !strcmp(argv[1], "--headless")) { argc--; argv++; }
    if (argc > 4) { width = atoi(argv[3]); height = atoi(argv[4]); }
    if (!l || !SDL_Init(0)) return 1;
    CHECK(resize_surface(l, width, height));
    if (!l->r) return 1;
    l->hover = l->press_card = -1;
    load_art(l);
    if (argc > 1) {
        if (argc > 2 && strcmp(argv[2], "empty") && strcmp(argv[2], "sources")) {
            CHECK(add_image(l, argv[2]) != NULL);
            load_font(l);
        } else if (argc > 2 && !strcmp(argv[2], "sources")) fixture_sources(l);
        if (argc > 5) {
            SDL_JoystickID id;
            CHECK(SDL_InitSubSystem(SDL_INIT_GAMEPAD));
            id = test_controller(!strcmp(argv[5], "ps") ? SDL_GAMEPAD_TYPE_PS4
                    : !strcmp(argv[5], "switch") ? SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO
                    : SDL_GAMEPAD_TYPE_XBOXONE);
            CHECK(id != 0); open_pad(l, id);
            if (argc > 6 && !strcmp(argv[6], "browse")) browse(l);
        }
        draw(l);
        CHECK(present(l));
        CHECK(SDL_SaveBMP(l->surface, argv[1]));
        if (argc > 7 && !strcmp(argv[7], "exercise")) disc_controller_flow(l, argv[2]);
    } else { layout_checks(l); controller_checks(l); }
    shutdown_ui(l, false);
    SDL_free(l);
    if (argc == 1) gpu_handoff();
    SDL_Quit();
    printf("launcher: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
