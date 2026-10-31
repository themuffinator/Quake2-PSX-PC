/*
 * test_menu.c — the menu's behaviour, not its data.
 *
 * `q2psx-inspect menu <disc>` already checks the page tables against a real
 * executable, so nothing here re-asserts a coordinate. What it pins down is the
 * behaviour that was read out of the code and cannot be checked against a table:
 * which items the cursor may land on, where it wraps, when an action fires,
 * what left and right do to each kind of widget, and what the GAME VARIABLES
 * page actually changes.
 */
#include "memcard.h"
#include "menu.h"
#include "menufont.h"
#include "menumouse.h"
#include "prompt.h"

#include <stdlib.h>
#include <stdio.h>
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

/* One frame with `pad` held, then one with nothing, so a press-and-release
 * lands whichever way an item wants it. */
static void tap(q2_menu *m, u16 pad)
{
    q2_menu_advance(m, pad);
    q2_menu_advance(m, 0);
}

static void open_menu(q2_menu *m, q2_menu_settings *s, bool multiplayer)
{
    q2_menu_init(m, s, Q2_MENU_SCREEN_H);
    q2_menu_set_multiplayer(m, multiplayer);
    q2_menu_open(m);
    q2_menu_advance(m, 0);   /* burn the settle frame */
    (void)q2_menu_take_request(m);
}

/*
 * 0x8003FE20 registers multipic2.lbm before frontend.lbm. Their placement
 * overlaps: slot 12's 256-byte 8bpp row reaches x 832..959, while slot 13's
 * 128-byte 4bpp row replaces x 896..959. The preview renderer samples only
 * the surviving left half. This order is therefore data, not housekeeping.
 */
static void test_font_upload_alias_order(void)
{
    static u8 packed[] = {
        0x80, 0x11, 0x82, 0x11, /* 129 + 127 bytes: multipic2 */
        0x81, 0x22              /* 128 bytes: frontend font  */
    };
    q2_vram_image image[2];
    q2_vram_section section;
    q2_hud_tables tab;
    q2_menu_font font;
    psx_vram *vram;

    memset(image, 0, sizeof(image));
    memset(&section, 0, sizeof(section));
    memset(&tab, 0, sizeof(tab));

    image[0].offset       = 0;
    image[0].packed_size  = 4;
    image[0].width        = 256;
    image[0].height       = 1;
    image[0].decoded_size = 256;
    image[0].name         = Q2_MENU_ARENA_NAME_1;
    image[1].offset       = 4;
    image[1].packed_size  = 2;
    image[1].width        = 128;
    image[1].height       = 1;
    image[1].decoded_size = 128;
    image[1].name         = Q2_MENU_ATLAS_NAME;

    section.buf.data    = packed;
    section.buf.size    = sizeof(packed);
    section.images      = image;
    section.image_count = 2;

    vram = (psx_vram *)calloc(1, sizeof(*vram));
    CHECK(vram != NULL, "allocate synthetic VRAM");
    if (!vram)
        return;

    CHECK(q2_menu_font_upload(&font, &tab, &section, vram, false, 1) == Q2_OK,
          "synthetic font upload succeeds");
    CHECK(font.arena_resident[1] && font.item_resident,
          "both aliased images are resident");
    CHECK(vram->px[256][832] == 0x1111,
          "preview remains in the left half: %04x", vram->px[256][832]);
    CHECK(vram->px[256][895] == 0x1111,
          "preview reaches its sampled edge: %04x", vram->px[256][895]);
    CHECK(vram->px[256][896] == 0x2222,
          "font replaces preview's unused right half: %04x",
          vram->px[256][896]);
    CHECK(vram->px[256][959] == 0x2222,
          "font survives through the shared page edge: %04x",
          vram->px[256][959]);

    free(vram);
}

/* ------------------------------------------------------------------------- */
static void test_defaults(void)
{
    q2_menu_settings s;

    q2_menu_settings_defaults(&s);

    /* 0x8001FA50 */
    CHECK(s.v[Q2_SET_MUSIC] == 48, "music default %d", s.v[Q2_SET_MUSIC]);
    CHECK(s.v[Q2_SET_SFX] == 96, "sfx default %d", s.v[Q2_SET_SFX]);
    CHECK(s.v[Q2_SET_STEREO] == 1, "stereo default %d", s.v[Q2_SET_STEREO]);
    /* 0x8001FA18 */
    CHECK(s.v[Q2_SET_SCREEN_Y] == 24, "screen y default %d", s.v[Q2_SET_SCREEN_Y]);
    CHECK(s.v[Q2_SET_HORIZONTAL_SPLIT] == 1, "split default");
    /* 0x8002048C */
    CHECK(s.v[Q2_SET_GRAVITY] == 64, "gravity default %d", s.v[Q2_SET_GRAVITY]);
    CHECK(s.v[Q2_SET_GAME_SPEED] == 64, "game speed default");
    CHECK(s.v[Q2_SET_BLAST_FORCE] == 64, "blast force default");
    CHECK(s.v[Q2_SET_FALLING_DAMAGE] == 1, "falling damage default");
    CHECK(s.v[Q2_SET_ONE_SHOT_KILL] == 0, "one shot kill default");
    /* 0x8001BDA8 */
    CHECK(s.v[Q2_SET_CROSSHAIR] == 1, "crosshair default");
    CHECK(s.v[Q2_SET_PAD_STYLE] == 6, "pad style default %d", s.v[Q2_SET_PAD_STYLE]);
    CHECK(strcmp(q2_menu_pad_style_name(6), "STANDARD A") == 0,
          "style 6 is %s", q2_menu_pad_style_name(6));
}

/*
 * 0x8001C698 — the defaults have to come out as the constants the game used
 * before the menu existed, or enabling the page would change the physics of a
 * session that never touched it.
 */
static void test_variables_apply(void)
{
    q2_menu_settings s;
    q2_menu_rules r;

    q2_menu_settings_defaults(&s);

    q2_menu_apply_variables(&s, true, 50, &r);
    CHECK(r.gravity == 32, "default gravity maps to %d, not 32", r.gravity);
    CHECK(r.tick_rate == 50, "default game speed maps to %d, not 50", r.tick_rate);
    CHECK(r.cheats == 0, "default cheats %04X", r.cheats);

    q2_menu_apply_variables(&s, false, 50, &r);
    CHECK(r.gravity == 32 && r.tick_rate == 50 && r.cheats == 0,
          "the disabled path must match the constants");

    /* Falling damage off is a cheat *bit*, the others are set when on. */
    s.v[Q2_SET_FALLING_DAMAGE] = 0;
    s.v[Q2_SET_INFINITE_AMMO]  = 1;
    s.v[Q2_SET_ALL_WEAPONS]    = 1;
    s.v[Q2_SET_ONE_SHOT_KILL]  = 1;
    q2_menu_apply_variables(&s, true, 50, &r);
    CHECK(r.cheats == (Q2_CHEAT_NO_FALL_DAMAGE | Q2_CHEAT_INFINITE_AMMO |
                       Q2_CHEAT_ALL_WEAPONS | Q2_CHEAT_ONE_SHOT_KILL),
          "cheat mask %04X", r.cheats);

    s.v[Q2_SET_GRAVITY]    = 0;
    s.v[Q2_SET_GAME_SPEED] = 127;
    q2_menu_apply_variables(&s, true, 50, &r);
    CHECK(r.gravity == 16, "minimum gravity %d", r.gravity);
    CHECK(r.tick_rate == (50 * 191) >> 7, "maximum speed %d", r.tick_rate);
}

/* ------------------------------------------------------------------------- */
static void test_navigation(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    open_menu(&m, &s, false);

    CHECK(m.page_id == Q2_PAGE_PAUSE_SP, "single player opens page %d", m.page_id);
    CHECK(m.page->count == 5,
          "the single-player pause menu keeps %u items, not 5", m.page->count);
    CHECK(m.cursor == 0, "cursor starts at %d", m.cursor);

    tap(&m, Q2_PAD_DOWN);
    CHECK(m.cursor == 1, "down -> %d", m.cursor);
    tap(&m, Q2_PAD_DOWN);
    tap(&m, Q2_PAD_DOWN);
    tap(&m, Q2_PAD_DOWN);
    CHECK(m.cursor == 4, "four downs -> %d", m.cursor);

    /* 0x80019E8C: past the end wraps to the first selectable item. */
    tap(&m, Q2_PAD_DOWN);
    CHECK(m.cursor == 0, "wrap down -> %d", m.cursor);
    tap(&m, Q2_PAD_UP);
    CHECK(m.cursor == 4, "wrap up -> %d", m.cursor);
}

/* The multiplayer pause menu is a different table with a different graph. */
static void test_multiplayer_pause(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    open_menu(&m, &s, true);

    CHECK(m.page_id == Q2_PAGE_PAUSE_MP, "multiplayer opens page %d", m.page_id);
    CHECK(m.page->count == 6, "%u items", m.page->count);
    CHECK(strcmp(m.page->items[3].label, "GAME VARIABLES") == 0,
          "item 3 is %s", m.page->items[3].label);
}

/*
 * 0x80019CD4 — a label beginning 'g' is greyed *and* skipped. The death screen
 * is where that matters: with no resupplies left the middle line is unreachable
 * and the cursor steps straight over it.
 */
static void test_grey_is_skipped(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_set_resupplies(&m, 0);
    q2_menu_goto(&m, Q2_PAGE_DEATH);
    m.arm_ticks = 0;              /* the 600-tick countdown at 0x800205B0 */

    CHECK(!q2_menu_item_selectable(&m, 1),
          "the greyed resupply line must not be selectable");
    CHECK(m.cursor == 0, "cursor starts at %d", m.cursor);
    tap(&m, Q2_PAD_DOWN);
    CHECK(m.cursor == 2, "down from 0 skips to %d, not 2", m.cursor);

    /* With resupplies in hand the same line is live. A fresh menu, because the
     * engine remembers the cursor per page (0x8001A3B0) and would otherwise
     * restore the one the first half left behind. */
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_set_resupplies(&m, 3);
    q2_menu_goto(&m, Q2_PAGE_DEATH);
    m.arm_ticks = 0;
    CHECK(q2_menu_item_selectable(&m, 1), "an affordable resupply is selectable");
    CHECK(strcmp(q2_menu_item_text(&m, 1), "RESUPPLY AND RESTART (3 LEFT)") == 0,
          "runtime label is \"%s\"", q2_menu_item_text(&m, 1));
    tap(&m, Q2_PAD_DOWN);
    CHECK(m.cursor == 1, "down from 0 -> %d", m.cursor);
}

/* The death screen ignores the pad until its countdown expires. */
static void test_death_is_inert_at_first(void)
{
    q2_menu_settings s;
    q2_menu m;
    int i;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_set_resupplies(&m, 1);
    q2_menu_goto(&m, Q2_PAGE_DEATH);

    tap(&m, Q2_PAD_DOWN);
    CHECK(m.cursor == 0, "the screen moved while still inert");

    for (i = 0; i < 700; i++)
        q2_menu_advance(&m, 0);

    tap(&m, Q2_PAD_DOWN);
    CHECK(m.cursor == 1, "once armed, down -> %d", m.cursor);
}

/* ------------------------------------------------------------------------- */
/* 0x8001B720: left means ON because the row reads "LABEL  ON  OFF". */
static void test_toggle(void)
{
    q2_menu_settings s;
    q2_menu m;
    char line[80];

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_goto(&m, Q2_PAGE_PLAYER);

    CHECK(m.cursor == 0, "cursor %d", m.cursor);
    s.v[Q2_SET_CROSSHAIR] = 0;

    tap(&m, Q2_PAD_LEFT);
    CHECK(s.v[Q2_SET_CROSSHAIR] == 1, "left must select ON");

    tap(&m, Q2_PAD_RIGHT);
    CHECK(s.v[Q2_SET_CROSSHAIR] == 0, "right must select OFF");

    /* The composed line is what the screen shows, codes and all. */
    s.v[Q2_SET_CROSSHAIR] = 1;
    q2_menu_item_display(&m, 0, line, (u32)sizeof(line));
    CHECK(strcmp(line, "bCROSSHAIR bON dOFF") == 0, "selected line \"%s\"", line);

    m.cursor = 1;
    q2_menu_item_display(&m, 0, line, (u32)sizeof(line));
    CHECK(strcmp(line, "CROSSHAIR ON") == 0, "unselected line \"%s\"", line);
}

/* 0x8001C018: two units a frame while held, clamped to 0..127. */
static void test_slider(void)
{
    q2_menu_settings s;
    q2_menu m;
    int i;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_goto(&m, Q2_PAGE_SOUND);

    CHECK(m.page->items[0].widget == Q2_WIDGET_SLIDER, "item 0 is not a slider");
    CHECK(s.v[Q2_SET_MUSIC] == 48, "music starts at %d", s.v[Q2_SET_MUSIC]);

    q2_menu_advance(&m, Q2_PAD_RIGHT);
    CHECK(s.v[Q2_SET_MUSIC] == 50, "one held frame -> %d", s.v[Q2_SET_MUSIC]);
    q2_menu_advance(&m, Q2_PAD_RIGHT);
    CHECK(s.v[Q2_SET_MUSIC] == 52, "two held frames -> %d", s.v[Q2_SET_MUSIC]);

    for (i = 0; i < 200; i++)
        q2_menu_advance(&m, Q2_PAD_RIGHT);
    CHECK(s.v[Q2_SET_MUSIC] == Q2_MENU_SLIDER_MAX,
          "clamps high at %d", s.v[Q2_SET_MUSIC]);

    for (i = 0; i < 200; i++)
        q2_menu_advance(&m, Q2_PAD_LEFT);
    CHECK(s.v[Q2_SET_MUSIC] == 0, "clamps low at %d", s.v[Q2_SET_MUSIC]);
}

/*
 * CHECK_FOR_SLIDER's tail, 0x8001C218..0x8001C25C: the MUSIC row auditions the
 * music and every other row on the page silences it.
 *
 * The hook (0x8001BF1C, installed as the SOUND page's per-frame handler at
 * 0x80020268) seeds s7 = -1, walks the page for the drawable bound to
 * &musicvol (0x800B3348) and then writes the CD volume target three ways:
 * nothing at all if the page carries no such row, `musicvol * 2` if that row is
 * the cursor row, zero if it is not. Before this the port scaled the bed
 * straight off the slider, so the track played through every page and on every
 * row of SOUND.
 */
static void test_music_audition(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);

    /* A closed menu is no page at all: CHECK_FOR_SLIDER is a page hook. */
    CHECK(q2_menu_music_audition(&m, 96) == -1, "a closed menu writes nothing");

    m.open = true;
    q2_menu_goto(&m, Q2_PAGE_SOUND);
    CHECK(m.page->items[0].setting == Q2_SET_MUSIC, "row 0 is not MUSIC");

    /* The page opens on the MUSIC row, so the track is audible. */
    m.cursor = 0;
    CHECK(q2_menu_music_audition(&m, 96) == 96,
          "the MUSIC row should audition at 96, got %d",
          q2_menu_music_audition(&m, 96));

    /* SOUND FX, STEREO and RESET TO DEFAULTS all silence it (0x8001C25C). */
    m.cursor = 1;
    CHECK(q2_menu_music_audition(&m, 96) == 0, "SOUND FX should silence it");
    m.cursor = 2;
    CHECK(q2_menu_music_audition(&m, 96) == 0, "STEREO should silence it");
    m.cursor = 3;
    CHECK(q2_menu_music_audition(&m, 96) == 0, "RESET should silence it");

    /* The doubling belongs to the caller, so a moved slider is reflected by
     * what it passes in and not by anything read here. */
    m.cursor = 0;
    CHECK(q2_menu_music_audition(&m, 0) == 0,
          "a MUSIC slider at zero auditions silence");
    CHECK(q2_menu_music_audition(&m, 254) == 254,
          "a MUSIC slider at the top auditions 254");

    /*
     * And a page with no MUSIC row leaves the target ALONE — the -1 answer,
     * which is s7 surviving the walk at its 0x8001BF2C seed and the `bltz` at
     * 0x8001C220 branching past both stores. This is the half that makes the
     * pause duck work: the pause page must not have its zero overwritten with
     * the slider every frame.
     */
    q2_menu_goto(&m, Q2_PAGE_PLAYER);
    CHECK(q2_menu_music_audition(&m, 96) == -1,
          "PLAYER carries no MUSIC row and must write nothing");
    q2_menu_goto(&m, Q2_PAGE_PAUSE_SP);
    CHECK(q2_menu_music_audition(&m, 96) == -1,
          "the pause page carries no MUSIC row and must write nothing");
}

/* 0x8001C944: the controller style wraps inside its class's three names. */
static void test_choice(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_goto(&m, Q2_PAGE_CONTROLLER);

    CHECK(m.page->items[0].widget == Q2_WIDGET_CHOICE, "item 0 is not a choice");
    CHECK(s.v[Q2_SET_PAD_STYLE] == 6, "style starts at %d", s.v[Q2_SET_PAD_STYLE]);

    tap(&m, Q2_PAD_RIGHT);
    CHECK(s.v[Q2_SET_PAD_STYLE] == 7, "right -> %d", s.v[Q2_SET_PAD_STYLE]);
    tap(&m, Q2_PAD_RIGHT);
    tap(&m, Q2_PAD_RIGHT);
    CHECK(s.v[Q2_SET_PAD_STYLE] == 6, "wraps back to %d", s.v[Q2_SET_PAD_STYLE]);
    tap(&m, Q2_PAD_LEFT);
    CHECK(s.v[Q2_SET_PAD_STYLE] == 8, "left wraps to %d", s.v[Q2_SET_PAD_STYLE]);

    /* A digital pad has no sticks, so those two rows are out (0x8001CA28). */
    CHECK(!q2_menu_item_selectable(&m, 2), "SWAP Y AXIS should be disabled");
    CHECK(!q2_menu_item_selectable(&m, 3), "USE MOUSE should be disabled");
}

/* ------------------------------------------------------------------------- */
/* 0x8001A0D8: the pause items fire on release, the options items on press. */
static void test_press_versus_release(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    open_menu(&m, &s, false);

    /* RETURN TO GAME is an on-release item: holding must not fire it. */
    q2_menu_advance(&m, Q2_PAD_CROSS);
    CHECK(m.open, "an on-release item fired on the press");
    q2_menu_advance(&m, 0);
    CHECK(!m.open, "an on-release item did not fire on the release");
    CHECK(q2_menu_take_request(&m) == Q2_MREQ_RESUME, "resume was not requested");

    /* RESET TO DEFAULTS on the sound page is an on-press item. */
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_goto(&m, Q2_PAGE_SOUND);
    m.cursor = 3;
    s.v[Q2_SET_MUSIC] = 5;
    q2_menu_advance(&m, Q2_PAD_CROSS);
    CHECK(s.v[Q2_SET_MUSIC] == 48, "the press did not reset (%d)", s.v[Q2_SET_MUSIC]);
}

/* The page graph: down into the options tree and back out again. */
static void test_page_graph(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    open_menu(&m, &s, false);

    m.cursor = 2;                       /* OPTIONS */
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_OPTIONS, "cross on OPTIONS -> page %d", m.page_id);

    tap(&m, Q2_PAD_CROSS);              /* PLAYER OPTIONS */
    CHECK(m.page_id == Q2_PAGE_PLAYER, "-> page %d", m.page_id);

    tap(&m, Q2_PAD_TRIANGLE);
    CHECK(m.page_id == Q2_PAGE_OPTIONS, "triangle -> page %d", m.page_id);
    tap(&m, Q2_PAD_TRIANGLE);
    CHECK(m.page_id == Q2_PAGE_PAUSE_SP, "triangle -> page %d", m.page_id);

    /* The root has no parent, so triangle there does nothing. */
    tap(&m, Q2_PAD_TRIANGLE);
    CHECK(m.page_id == Q2_PAGE_PAUSE_SP && m.open,
          "triangle at the root left page %d", m.page_id);

    /*
     * The PAUSE page does NOT come back where you left it.
     *
     * This check used to read `m.cursor == 2` — "the pause cursor came back as
     * 2" — and that was the port's own invention. The save at 0x8001A3B0 is
     * unconditional, but the RESTORE is opt-in per page and the pause pages
     * opt out twice over: InGame_GOTO_PAUSEDBACK never calls
     * FrontEndRestoreCursor, and its tail re-reads `first` and stores it over
     * the cursor for both page 26 and page 43 (`lhu v1, 714(v0)` at 0x8001D714,
     * `sh v1, 712(v0)` at 0x8001D71C). So pressing TRIANGLE out of OPTIONS puts
     * you back on RETURN TO GAME, not on OPTIONS.
     */
    m.cursor = 2;
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_OPTIONS, "back into OPTIONS");
    tap(&m, Q2_PAD_TRIANGLE);
    CHECK(m.cursor == 0, "the pause cursor came back as %d, not RETURN TO GAME",
          m.cursor);

    /* OPTIONS, on the other hand, is one of the seven pages that do ask
     * (0x8001FDF4 stores what FrontEndRestoreCursor returns). */
    m.cursor = 2;
    tap(&m, Q2_PAD_CROSS);              /* -> OPTIONS, first visit was above  */
    CHECK(m.page_id == Q2_PAGE_OPTIONS, "back into OPTIONS again");
    m.cursor = 2;                       /* VIDEO OPTIONS */
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_VIDEO, "-> page %d", m.page_id);
    tap(&m, Q2_PAD_TRIANGLE);
    CHECK(m.page_id == Q2_PAGE_OPTIONS && m.cursor == 2,
          "OPTIONS came back on row %d", m.cursor);
}

/*
 * Which pages ask for their cursor back, and what a FIRST visit gets.
 *
 * The seven restoring sites are listed in menu.c; the shape here is the one
 * FrontEndRestoreCursor itself has (0x8001FBC4): CursorMemory[page].byte1 is
 * seeded to 1 for all 47 records by FrontEndClearCursorMemory (0x8001FC0C), and
 * while it is set the function returns the CURRENT cursor and clears the flag
 * (0x8001FBF0/0x8001FBF4) rather than the saved byte at 0x8001FC00. A page that
 * has never been open therefore opens on the builder's default, not on zero.
 */
static void test_cursor_policy(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    open_menu(&m, &s, false);

    /*
     * A first visit to a restoring page. Seed cursor_save[] with something a
     * restore would pick up; the never-visited flag must beat it.
     */
    m.cursor_save[Q2_PAGE_SOUND] = 3;
    q2_menu_goto(&m, Q2_PAGE_SOUND);
    CHECK(m.cursor == 0, "the first visit to SOUND opened on %d", m.cursor);

    /* Now it has been visited, so the save is live. */
    m.cursor = 3;
    q2_menu_goto(&m, Q2_PAGE_PLAYER);
    q2_menu_goto(&m, Q2_PAGE_SOUND);
    CHECK(m.cursor == 3, "SOUND came back on %d", m.cursor);

    /* A page that never restores ignores its save however often it is used —
     * the confirmation opens on NO every time (the answer table's first row),
     * which is what makes "back out of QUIT GAME?" safe. */
    q2_menu_goto(&m, Q2_PAGE_QUIT_CONFIRM);
    m.cursor = 2;                        /* YES */
    q2_menu_goto(&m, Q2_PAGE_PAUSE_SP);
    q2_menu_goto(&m, Q2_PAGE_QUIT_CONFIRM);
    CHECK(m.cursor == 1 &&
          strcmp(q2_menu_item_text(&m, m.cursor), "NO") == 0,
          "the confirmation reopened on %s", q2_menu_item_text(&m, m.cursor));

    /* And the multiplayer pause page resets like the single-player one
     * (0x8001D5D8's arm reaches the same 0x8001D71C). */
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    q2_menu_set_multiplayer(&m, true);
    q2_menu_open(&m);
    q2_menu_advance(&m, 0);
    CHECK(m.page_id == Q2_PAGE_PAUSE_MP, "multiplayer opens page %d", m.page_id);
    m.cursor = 1;
    q2_menu_goto(&m, Q2_PAGE_OPTIONS);
    q2_menu_goto(&m, Q2_PAGE_PAUSE_MP);
    CHECK(m.cursor == 0, "the multiplayer pause cursor came back as %d",
          m.cursor);
}

/*
 * VIBRATION is a row the pad has to earn — 0x8001C9D0.
 *
 * InGame_CHECK_FOR_CONTROLLER asks IsPlayerPadShockable (0x80033250) about the
 * pausing player and, when the answer is zero, greys drawable 2 and pulls the
 * cursor off it (0x8001C9E0..0x8001CA44). Drawable 2 is item 1 here, because
 * FrontEndAddPausedTitle takes drawable 0 (0x8001D07C) — the same arithmetic
 * that makes 216 = 2*92 + 32 the row's text and 258 its disable halfword.
 *
 * Note that this is a PAD CAPABILITY and not the VIBRATION setting: the setting
 * is the halfword at 0x800B32BC + p*34, whose only reader in the whole image is
 * PadDualShock's `lh v0, 728(v0)` at 0x8003179C.
 */
static void test_vibration_needs_a_shockable_pad(void)
{
    q2_menu_settings s;
    q2_menu m;
    char line[64];

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_goto(&m, Q2_PAGE_CONTROLLER);

    /* Nothing has said the pad rumbles, which is IsPlayerPadShockable's own
     * answer for a pad with no actuators (0x80031878 clears the bit). */
    CHECK(!q2_menu_item_selectable(&m, 1),
          "VIBRATION must be out on a pad with no motors");
    q2_menu_item_display(&m, 1, line, (u32)sizeof(line));
    CHECK(line[0] == Q2_MENU_CODE_GREY,
          "VIBRATION must draw grey, got \"%s\"", line);

    /* And the cursor is not left on it. */
    m.cursor = 1;
    q2_menu_goto(&m, Q2_PAGE_PLAYER);
    q2_menu_goto(&m, Q2_PAGE_CONTROLLER);
    CHECK(m.cursor != 1, "the cursor stayed on the grey VIBRATION row");

    /* Say the pad has motors and the row comes back, bright and landable —
     * 0x8001CA48's arm strips the prefix and clears the halfword. */
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_set_pad_shockable(&m, 0, true);
    q2_menu_goto(&m, Q2_PAGE_CONTROLLER);
    CHECK(q2_menu_item_selectable(&m, 1),
          "VIBRATION must be live on a DualShock");
    q2_menu_item_display(&m, 1, line, (u32)sizeof(line));
    CHECK(line[0] != Q2_MENU_CODE_GREY,
          "VIBRATION must draw bright on a DualShock, got \"%s\"", line);

    /* The other two blocks grey their labels as well as disabling them —
     * 0x8001CAC0 and 0x8001CB9C sprintf through the same "g%s". The port had
     * been setting only the halfword, so they went unreachable while still
     * drawing in the bright colour. */
    q2_menu_item_display(&m, 2, line, (u32)sizeof(line));
    CHECK(line[0] == Q2_MENU_CODE_GREY,
          "SWAP Y AXIS must draw grey, got \"%s\"", line);
    q2_menu_item_display(&m, 3, line, (u32)sizeof(line));
    CHECK(line[0] == Q2_MENU_CODE_GREY,
          "USE MOUSE must draw grey, got \"%s\"", line);
}

/*
 * The banner wears the pausing player's colour — 0x8001CFC8..0x8001D05C.
 *
 * Single player branches straight to the clear at 0x8001D054 and the band is
 * blue. In a match FrontEndAddPausedTitle indexes the 34-byte record with
 * `Pauser` and takes the TEAM halfword when the mode is 1 and the player-colour
 * halfword otherwise. Menu ROWS never carry it — FrontEndCreateMenu clears bits
 * 6..8 on every record it appends (0x8001A524) — which is what makes the
 * banner's colour read as an identifier.
 */
static void test_paused_banner_colour(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);

    /* Single player: blue, whatever the per-player table says. */
    open_menu(&m, &s, false);
    CHECK(q2_menu_title_bar_colour(&m) == Q2_MENU_BAR_BLUE,
          "single player banner is colour %d", q2_menu_title_bar_colour(&m));

    /* Deathmatch, player 2 paused: the seed at 0x8001BEDC is colour = p. */
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    q2_menu_set_multiplayer(&m, true);
    q2_menu_set_pauser(&m, 1);
    q2_menu_open(&m);
    CHECK(q2_menu_title_bar_colour(&m) == Q2_MENU_BAR_RED,
          "player 2's banner is colour %d", q2_menu_title_bar_colour(&m));
    CHECK(m.page_id == Q2_PAGE_PAUSE_MP, "on page %d", m.page_id);

    /* Player 3 keeps its own, and every page that FrontEndAddPausedTitle builds
     * carries it — there are nine such callers, OPTIONS among them. */
    q2_menu_set_pauser(&m, 2);
    q2_menu_goto(&m, Q2_PAGE_OPTIONS);
    CHECK(q2_menu_title_bar_colour(&m) == Q2_MENU_BAR_PURPLE,
          "player 3's OPTIONS banner is colour %d",
          q2_menu_title_bar_colour(&m));

    /* Page 42's banner is the first record of its own table, appended by
     * FrontEndCreateMenu, so it is blue even in a match. */
    q2_menu_goto(&m, Q2_PAGE_VARIABLES);
    CHECK(q2_menu_title_bar_colour(&m) == Q2_MENU_BAR_BLUE,
          "MPCHEAT's banner is colour %d", q2_menu_title_bar_colour(&m));

    /* TEAM DEATHMATCH takes the team halfword instead: the seed is p & 1, so
     * players 2 and 4 share a colour and players 1 and 3 share the other. */
    q2_menu_set_mp_mode(&m, 1);
    q2_menu_set_pauser(&m, 2);
    q2_menu_goto(&m, Q2_PAGE_PAUSE_MP);
    CHECK(q2_menu_title_bar_colour(&m) == Q2_MENU_BAR_BLUE,
          "team 0's banner is colour %d", q2_menu_title_bar_colour(&m));
    q2_menu_set_pauser(&m, 3);
    q2_menu_goto(&m, Q2_PAGE_PAUSE_MP);
    CHECK(q2_menu_title_bar_colour(&m) == Q2_MENU_BAR_RED,
          "team 1's banner is colour %d", q2_menu_title_bar_colour(&m));

    /* A host that names its own colours is obeyed, masked to three bits. */
    q2_menu_set_mp_mode(&m, 0);
    q2_menu_set_player_colour(&m, 3, 0, Q2_MENU_BAR_GREEN);
    q2_menu_goto(&m, Q2_PAGE_PAUSE_MP);
    CHECK(q2_menu_title_bar_colour(&m) == Q2_MENU_BAR_GREEN,
          "the named colour came back as %d", q2_menu_title_bar_colour(&m));
}

static void test_quit_flow(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    open_menu(&m, &s, false);

    m.cursor = 4;                        /* QUIT GAME */
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_QUIT_CONFIRM, "-> page %d", m.page_id);
    CHECK(m.cursor == 1, "the confirmation starts on %s",
          q2_menu_item_text(&m, m.cursor));
    CHECK(strcmp(q2_menu_item_text(&m, m.cursor), "NO") == 0,
          "the confirmation must start on NO");

    /* NO goes back where it came from — and onto RETURN TO GAME, because the
     * pause page takes the builder's default rather than its saved cursor
     * (0x8001D71C). Walking back down to QUIT GAME is part of the journey. */
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_PAUSE_SP, "NO -> page %d", m.page_id);
    CHECK(m.cursor == 0, "NO left the pause cursor on %d", m.cursor);

    m.cursor = 4;                        /* QUIT GAME, again */
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_QUIT_CONFIRM, "-> page %d", m.page_id);
    tap(&m, Q2_PAD_DOWN);                /* NO -> YES, which is drawn above */
    CHECK(strcmp(q2_menu_item_text(&m, m.cursor), "YES") == 0,
          "down from NO must reach YES");
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_QUITTING, "YES -> page %d", m.page_id);
    CHECK(q2_menu_take_request(&m) == Q2_MREQ_QUIT, "quit was not requested");
}

static void test_restart_flow(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    open_menu(&m, &s, false);

    m.cursor = 3;                        /* RESTART LEVEL */
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_RESTART_CONFIRM, "-> page %d", m.page_id);
    tap(&m, Q2_PAD_DOWN);
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_RESTARTING, "YES -> page %d", m.page_id);
    CHECK(q2_menu_take_request(&m) == Q2_MREQ_RESTART, "restart was not requested");
}

/* A text-only page has nothing to land on and swallows the pad. */
static void test_text_only_page(void)
{
    q2_menu_settings s;
    q2_menu m;
    const q2_menu_page *p;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_goto(&m, Q2_PAGE_RESTARTING);

    p = m.page;
    CHECK(p->first == p->count, "a text page must have no navigable range");
    CHECK(!q2_menu_item_selectable(&m, 0), "item 0 must not be selectable");

    (void)q2_menu_take_request(&m);
    tap(&m, Q2_PAD_DOWN);
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_RESTARTING, "the page moved to %d", m.page_id);
}

/* The SCREEN POSITION page has no items: the d-pad moves the display. */
static void test_screen_position(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_goto(&m, Q2_PAGE_VIDEO);
    m.cursor = 0;                        /* SCREEN POSITION */
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_SCREEN_POSITION, "-> page %d", m.page_id);

    tap(&m, Q2_PAD_RIGHT);
    CHECK(s.v[Q2_SET_SCREEN_X] == 1, "right -> %d", s.v[Q2_SET_SCREEN_X]);
    tap(&m, Q2_PAD_UP);
    CHECK(s.v[Q2_SET_SCREEN_Y] == 23, "up -> %d", s.v[Q2_SET_SCREEN_Y]);

    /*
     * 0x8001CD64 reads the HELD pad through a counter per direction: a step on
     * the first frame, none on the next three, then one every frame. Eight
     * frames of LEFT from 1 is steps on frames 1, 5, 6, 7 and 8.
     */
    {
        int i;
        for (i = 0; i < 8; i++)
            q2_menu_advance(&m, Q2_PAD_LEFT);
        q2_menu_advance(&m, 0);
        CHECK(s.v[Q2_SET_SCREEN_X] == -4, "8 frames of left from 1 -> %d, expected -4",
              s.v[Q2_SET_SCREEN_X]);

        /* The page's own clamps, -20..20 and -32..64 — not the offsets the
         * video reset writes. Negative y is reachable, which is what
         * Config_InstallVideo's y < 0 branch is for. */
        for (i = 0; i < 100; i++)
            q2_menu_advance(&m, Q2_PAD_LEFT);
        q2_menu_advance(&m, 0);
        CHECK(s.v[Q2_SET_SCREEN_X] == Q2_MENU_SCREEN_X_MIN, "x floor %d",
              s.v[Q2_SET_SCREEN_X]);
        for (i = 0; i < 100; i++)
            q2_menu_advance(&m, Q2_PAD_RIGHT);
        q2_menu_advance(&m, 0);
        CHECK(s.v[Q2_SET_SCREEN_X] == Q2_MENU_SCREEN_X_MAX, "x ceiling %d",
              s.v[Q2_SET_SCREEN_X]);
        for (i = 0; i < 200; i++)
            q2_menu_advance(&m, Q2_PAD_UP);
        q2_menu_advance(&m, 0);
        CHECK(s.v[Q2_SET_SCREEN_Y] == Q2_MENU_SCREEN_Y_MIN, "y floor %d",
              s.v[Q2_SET_SCREEN_Y]);
        for (i = 0; i < 200; i++)
            q2_menu_advance(&m, Q2_PAD_DOWN);
        q2_menu_advance(&m, 0);
        CHECK(s.v[Q2_SET_SCREEN_Y] == Q2_MENU_SCREEN_Y_MAX, "y ceiling %d",
              s.v[Q2_SET_SCREEN_Y]);
        CHECK(Q2_MENU_SCREEN_X_MIN == -20 && Q2_MENU_SCREEN_X_MAX == 20 &&
              Q2_MENU_SCREEN_Y_MIN == -32 && Q2_MENU_SCREEN_Y_MAX == 64,
              "the page clamps are the executable's");
    }

    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_VIDEO, "cross leaves to page %d", m.page_id);
}

/* ------------------------------------------------------------------------- */
/* 0x8001D510: the number of variables you get is how many cheats you have. */
static void test_variables_pages(void)
{
    static const u8 want[4] = { 3, 5, 7, 9 };
    int lvl;

    for (lvl = 0; lvl < 4; lvl++) {
        const q2_menu_page *p = q2_menu_variables_page(lvl);
        CHECK(p->count == want[lvl], "cheat level %d gives %u items, not %u",
              lvl, p->count, want[lvl]);
    }

    CHECK(strcmp(q2_menu_cheat_level_name(0), "NONE") == 0, "level 0 name");
    CHECK(strcmp(q2_menu_cheat_level_name(3), "GOLD") == 0, "level 3 name");
}

/* The VIDEO page only offers HORIZONTAL SPLIT in a multiplayer session. */
static void test_video_variant(void)
{
    const q2_menu_page *sp = q2_menu_video_page(false);
    const q2_menu_page *mp = q2_menu_video_page(true);

    CHECK(sp->count == 2, "single player video has %u items", sp->count);
    CHECK(mp->count == 3, "multiplayer video has %u items", mp->count);
    CHECK(strcmp(mp->items[0].label, "HORIZONTAL SPLIT") == 0,
          "multiplayer item 0 is %s", mp->items[0].label);
}

/*
 * 0x8001D638: the single-player pause page composes its KILLS/SECRETS row as
 * part of its own install, so the row has to survive the page entry that
 * follows a caller filling the numbers in — and has to be absent everywhere
 * else, because the placement at (256, 204) belongs to page 26 alone.
 */
static void test_pause_status_line(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    q2_menu_set_stats(&m, 3, 10, 1, 4);
    q2_menu_open(&m);

    CHECK(m.page_id == Q2_PAGE_PAUSE_SP, "opened on page %d", m.page_id);
    CHECK(strcmp(m.status, "KILLS 3/10    1/4 SECRETS") == 0,
          "pause status reads [%s]", m.status);

    /* Every sub-page clears it: the console never places the row on one. */
    q2_menu_goto(&m, Q2_PAGE_OPTIONS);
    CHECK(m.status[0] == '\0', "OPTIONS carries [%s]", m.status);
    q2_menu_goto(&m, Q2_PAGE_VIDEO);
    CHECK(m.status[0] == '\0', "VIDEO carries [%s]", m.status);

    /* Coming back re-composes it from the numbers still held. */
    q2_menu_goto(&m, Q2_PAGE_PAUSE_SP);
    CHECK(strcmp(m.status, "KILLS 3/10    1/4 SECRETS") == 0,
          "returning to PAUSED reads [%s]", m.status);

    /* 0x8001D5D8, the multiplayer arm, does none of it. */
    q2_menu_close(&m);
    q2_menu_set_multiplayer(&m, true);
    q2_menu_open(&m);
    CHECK(m.page_id == Q2_PAGE_PAUSE_MP, "multiplayer opens page %d", m.page_id);
    CHECK(m.status[0] == '\0', "the multiplayer pause page carries [%s]",
          m.status);
}

/*
 * RESET TO DEFAULTS stores the FRAMEBUFFER's neutral y, not the block's
 * reference height: zero on a 240-line raster, 24 on a 248-line one
 * (0x8001FA18). The helper was always right; the menu action fed it 248 on
 * every build, which put SLUS-00757's picture 24 lines down.
 */
static void test_reset_video_uses_the_framebuffer(void)
{
    q2_menu_settings set;
    q2_menu m;
    int i, reset = -1;

    q2_menu_settings_defaults(&set);
    q2_menu_init(&m, &set, Q2_MENU_SCREEN_H);
    q2_menu_set_fb_height(&m, 240);
    q2_menu_reset_video_for(&set, 240);
    q2_menu_open(&m);
    q2_menu_advance(&m, 0);             /* the settle frame q2_menu_open asks for */
    q2_menu_goto(&m, Q2_PAGE_VIDEO);

    for (i = 0; i < (int)m.page->count; i++)
        if (strcmp(m.page->items[i].label, "RESET TO DEFAULTS") == 0)
            reset = i;
    CHECK(reset >= 0, "the VIDEO page has a RESET TO DEFAULTS row");
    if (reset < 0)
        return;

    q2_menu_point_at(&m, reset);
    tap(&m, Q2_PAD_CROSS);
    CHECK(set.v[Q2_SET_SCREEN_Y] == 0,
          "NTSC RESET leaves SCREEN_Y at %d", set.v[Q2_SET_SCREEN_Y]);

    /* The 248-line build still gets its 24. */
    q2_menu_init(&m, &set, Q2_MENU_SCREEN_H);
    q2_menu_open(&m);
    q2_menu_advance(&m, 0);
    q2_menu_goto(&m, Q2_PAGE_VIDEO);
    q2_menu_point_at(&m, reset);
    tap(&m, Q2_PAD_CROSS);
    CHECK(set.v[Q2_SET_SCREEN_Y] == 24,
          "PAL RESET leaves SCREEN_Y at %d", set.v[Q2_SET_SCREEN_Y]);
}

/*
 * The front end runs QFRONT's tables, and two of them are not the
 * executable's: VIDEO is the three-row one (module+0x0EEF4, the only video
 * table in the module) and CONTROLLER abbreviates four of its five labels
 * (module+0x0FA4C).
 */
static void test_front_end_tables(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    q2_menu_set_front_end(&m, true);
    q2_menu_open(&m);
    q2_menu_goto(&m, Q2_PAGE_FRONT_OPTIONS);
    q2_menu_advance(&m, 0);             /* the settle frame q2_menu_open asks for */

    CHECK(q2_menu_point_at(&m, 2), "the front-end OPTIONS page has a third row");
    CHECK(strcmp(m.page->items[2].label, "VIDEO OPTIONS") == 0,
          "row 2 is %s", m.page->items[2].label);
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_VIDEO, "VIDEO OPTIONS landed on page %d",
          m.page_id);
    CHECK(m.page->count == 3, "the front-end VIDEO page has %u rows",
          m.page->count);
    CHECK(strcmp(m.page->items[0].label, "HORIZONTAL SPLIT") == 0,
          "its first row is %s", m.page->items[0].label);

    /* SCREEN POSITION's back action names the VIDEO page rather than popping,
     * so the page it lands on must still be the front end's three-row one.
     * CROSS is what leaves that page (0x8001CD64 hands the d-pad to the offset
     * and only the select button calls the back handler). */
    q2_menu_goto(&m, Q2_PAGE_SCREEN_POSITION);
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_VIDEO && m.page->count == 3,
          "backing out of SCREEN POSITION gives page %d with %u rows",
          m.page_id, m.page ? m.page->count : 0);

    q2_menu_goto(&m, Q2_PAGE_CONTROLLER);
    CHECK(m.page->count == 5, "the front-end CONTROLLER page has %u rows",
          m.page->count);
    CHECK(strcmp(m.page->items[1].label, "VIBRATE") == 0 &&
          strcmp(m.page->items[2].label, "SWAP Y") == 0 &&
          strcmp(m.page->items[3].label, "MOUSE") == 0 &&
          strcmp(m.page->items[4].label, "MOUSE SPEED") == 0,
          "QFRONT's controller labels");

    /* In game it is the executable's table, with the long words. */
    q2_menu_set_front_end(&m, false);
    q2_menu_goto(&m, Q2_PAGE_CONTROLLER);
    CHECK(strcmp(m.page->items[1].label, "VIBRATION") == 0,
          "the in-game controller row 1 is %s", m.page->items[1].label);
    q2_menu_goto(&m, Q2_PAGE_VIDEO);
    CHECK(m.page->count == 2, "the in-game single-player VIDEO page has %u rows",
          m.page->count);
}

/*
 * SQUARE on a mode row opens that mode's rules page — QFRONT module+0x4654
 * reads the bit and module+0x4868 installs the page the mode's jump-table arm
 * names. The page is pure text with the MULTIPLAYER page as its back handler.
 */
static void test_front_rules_pages(void)
{
    q2_menu_settings s;
    q2_menu m;
    const q2_menu_page *p;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    q2_menu_set_front_end(&m, true);
    q2_menu_open(&m);
    q2_menu_goto(&m, Q2_PAGE_FRONT_MULTI);
    q2_menu_advance(&m, 0);

    /* Row 1 is TEAM DEATHMATCH, mode 1, page module+0x0F4DC. */
    q2_menu_point_at(&m, (int)m.page->first + 1);
    CHECK(q2_menu_front_rules_row(&m), "row 1 advertises RULES");
    tap(&m, Q2_PAD_SQUARE);
    CHECK(m.page_id == Q2_PAGE_FRONT_RULES, "SQUARE landed on page %d",
          m.page_id);
    CHECK(m.page && m.page->title && strcmp(m.page->title, "TEAM DEATHMATCH") == 0,
          "its banner is %s", m.page && m.page->title ? m.page->title : "(none)");
    CHECK(m.page && m.page->count == 7 &&
          strcmp(m.page->items[0].label, "RULES") == 0,
          "it has %u rows starting %s", m.page ? m.page->count : 0,
          m.page && m.page->count ? m.page->items[0].label : "");
    CHECK(m.page && m.page->first == m.page->count,
          "nothing on it is navigable");

    /* TRIANGLE comes straight back, as engine+0x28C being the MULTIPLAYER
     * builder says it does. */
    tap(&m, Q2_PAD_TRIANGLE);
    CHECK(m.page_id == Q2_PAGE_FRONT_MULTI, "TRIANGLE landed on page %d",
          m.page_id);

    /* The settings rows carry no RULES prompt and no rules page. */
    q2_menu_point_at(&m, (int)m.page->first + 3);
    CHECK(!q2_menu_front_rules_row(&m), "LOAD SETTINGS advertises RULES");
    tap(&m, Q2_PAD_SQUARE);
    CHECK(m.page_id == Q2_PAGE_FRONT_MULTI, "SQUARE moved off page %d",
          m.page_id);

    /* All six arms of module+0x4868, in its own order. Three of them are nine
     * rows, which the module-page reader's eight-row cap hides. */
    p = q2_menu_front_rules_page(Q2_MENU_MP_DEATHMATCH);
    CHECK(p->count == 6 && p->addr == 0x8010F434u, "DEATHMATCH rules");
    p = q2_menu_front_rules_page(Q2_MENU_MP_TEAM_DEATHMATCH);
    CHECK(p->count == 7 && p->addr == 0x8010F4DCu, "TEAM DEATHMATCH rules");
    p = q2_menu_front_rules_page(2);
    CHECK(p->count == 9 && p->addr == 0x8010F59Cu &&
          strcmp(p->items[8].label, "CAPTURES WINS.") == 0,
          "CAPTURE THE FLAG rules end on %s", p->items[p->count - 1].label);
    p = q2_menu_front_rules_page(3);
    CHECK(p->count == 6 && p->addr == 0x8010F68Cu, "TAG rules");
    p = q2_menu_front_rules_page(4);
    CHECK(p->count == 9 && p->addr == 0x8010F734u &&
          strcmp(p->items[8].label, "FLAG THE LONGEST WINS.") == 0,
          "TEAM TAG rules end on %s", p->items[p->count - 1].label);
    p = q2_menu_front_rules_page(Q2_MENU_MP_VERSUS);
    CHECK(p->count == 9 && p->addr == 0x8010F824u &&
          strcmp(p->items[8].label, "REACH SCORE LIMIT WINS.") == 0,
          "VERSUS rules end on %s", p->items[p->count - 1].label);

    /* module+0x4990: a mode the table does not cover installs the empty one. */
    p = q2_menu_front_rules_page(6);
    CHECK(p->count == 0, "the fall-through page has %u rows", p->count);
}

/* QFRONT+0x4AD8/+0x50D0/+0x49F8: the complete local-match front end. */
static void test_front_multiplayer_setup(void)
{
    static const u8 vars_count[4] = { 3, 5, 7, 9 };
    q2_menu_settings s;
    q2_menu m;
    const q2_menu_page *p;
    int level;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    q2_menu_set_controller_count(&m, 3);
    q2_menu_open(&m);
    q2_menu_advance(&m, 0);
    q2_menu_goto(&m, Q2_PAGE_FRONT_MULTI);

    /* The shared mode action derives the mode from the selected row. */
    m.cursor = 0;
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_FRONT_DMSETUP, "DM opens page %d", m.page_id);
    CHECK(m.mp_setup.mode == Q2_MENU_MP_DEATHMATCH, "DM mode is %d",
          m.mp_setup.mode);
    CHECK(strcmp(m.page->title, "DEATHMATCH") == 0, "DM banner is %s",
          m.page->title);
    CHECK(m.page->count == 6 && m.page->addr == 0x8010F914u,
          "DM setup is %u rows at %08x", m.page->count, m.page->addr);
    CHECK(strcmp(q2_menu_item_text(&m, 0), "2 PLAYERS") == 0,
          "player row is %s", q2_menu_item_text(&m, 0));
    CHECK(strcmp(q2_menu_item_text(&m, 1), "COLD STORAGE") == 0,
          "arena row is %s", q2_menu_item_text(&m, 1));
    CHECK(strcmp(q2_menu_item_text(&m, 2), "TIME LIMIT 10") == 0,
          "time row is %s", q2_menu_item_text(&m, 2));
    CHECK(strcmp(q2_menu_item_text(&m, 3), "FRAG LIMIT 10") == 0,
          "frag row is %s", q2_menu_item_text(&m, 3));

    /* Player count clamps, while every indexed option wraps. */
    m.cursor = 0;
    tap(&m, Q2_PAD_LEFT);
    CHECK(m.mp_setup.players == 2, "players clamp low at %d", m.mp_setup.players);
    tap(&m, Q2_PAD_RIGHT);
    tap(&m, Q2_PAD_RIGHT);
    CHECK(m.mp_setup.players == 3, "players clamp to controllers at %d",
          m.mp_setup.players);

    m.cursor = 1;
    tap(&m, Q2_PAD_LEFT);
    CHECK(m.mp_setup.arena == 11 &&
          strcmp(q2_menu_item_text(&m, 1), "BADLANDS") == 0,
          "arena wraps left to %d/%s", m.mp_setup.arena,
          q2_menu_item_text(&m, 1));
    tap(&m, Q2_PAD_RIGHT);
    CHECK(m.mp_setup.arena == 0, "arena wraps right to %d", m.mp_setup.arena);

    m.cursor = 2;
    m.mp_setup.time_option = 0;
    tap(&m, Q2_PAD_LEFT);
    CHECK(m.mp_setup.time_option == 11 &&
          strcmp(q2_menu_item_text(&m, 2), "TIME LIMIT  i") == 0,
          "time wraps to NONE: %d/%s", m.mp_setup.time_option,
          q2_menu_item_text(&m, 2));
    m.cursor = 3;
    m.mp_setup.frag_option = 0;
    tap(&m, Q2_PAD_LEFT);
    CHECK(m.mp_setup.frag_option == 8 &&
          strcmp(q2_menu_item_text(&m, 3), "FRAG LIMIT  i") == 0,
          "frag wraps to NONE: %d/%s", m.mp_setup.frag_option,
          q2_menu_item_text(&m, 3));

    /* This is QFRONT's own page-12 layout, not PAUSED's page 42. */
    q2_menu_set_cheat_level(&m, 3);
    m.cursor = 4;
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_FRONT_VARIABLES &&
          strcmp(m.page->title, "GAME VARIABLES") == 0,
          "front variables page/banner is %d/%s", m.page_id, m.page->title);
    CHECK(m.page->count == 9 && m.page->items[0].y == 56 &&
          m.page->items[8].y == 192,
          "gold front variables layout is %u rows, y %d..%d", m.page->count,
          m.page->items[0].y, m.page->items[8].y);
    tap(&m, Q2_PAD_TRIANGLE);
    CHECK(m.page_id == Q2_PAGE_FRONT_DMSETUP && m.cursor == 4,
          "back restores setup row %d on page %d", m.cursor, m.page_id);

    m.cursor = 5;
    tap(&m, Q2_PAD_CROSS);
    CHECK(q2_menu_take_request(&m) == Q2_MREQ_MP_PROCEED,
          "PROCEED raises its own request");

    /* TEAM and VERSUS install different live setup variants. */
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_goto(&m, Q2_PAGE_FRONT_MULTI);
    m.cursor = 1;
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.mp_setup.mode == Q2_MENU_MP_TEAM_DEATHMATCH &&
          strcmp(m.page->title, "TEAM DEATHMATCH") == 0,
          "team row gives mode/banner %d/%s", m.mp_setup.mode, m.page->title);

    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_goto(&m, Q2_PAGE_FRONT_MULTI);
    m.cursor = 2;
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.mp_setup.mode == Q2_MENU_MP_VERSUS &&
          strcmp(m.page->title, "VERSUS") == 0,
          "versus row gives mode/banner %d/%s", m.mp_setup.mode, m.page->title);
    CHECK(m.page->count == 5 && m.page->addr == 0x8010F9BCu &&
          m.page->items[0].y == 63 && m.page->items[4].y == 191,
          "versus table is %u rows at %08x, y %d..%d", m.page->count,
          m.page->addr, m.page->items[0].y, m.page->items[4].y);
    CHECK(strcmp(q2_menu_item_text(&m, 2), "ROUNDS  3") == 0,
          "round row is %s", q2_menu_item_text(&m, 2));
    m.cursor = 2;
    m.mp_setup.round_option = 0;
    tap(&m, Q2_PAD_LEFT);
    CHECK(m.mp_setup.round_option == 4 &&
          strcmp(q2_menu_item_text(&m, 2), "ROUNDS 10") == 0,
          "rounds wrap to %d/%s", m.mp_setup.round_option,
          q2_menu_item_text(&m, 2));

    /* The four formerly-collapsed leaves are distinguishable to the host. */
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_goto(&m, Q2_PAGE_FRONT_NEWLOAD);
    m.cursor = 1;
    tap(&m, Q2_PAD_CROSS);
    CHECK(q2_menu_take_request(&m) == Q2_MREQ_LOAD_GAME,
          "LOAD GAME raises its own request");

    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_goto(&m, Q2_PAGE_FRONT_MULTI);
    m.cursor = 3;
    tap(&m, Q2_PAD_CROSS);
    CHECK(q2_menu_take_request(&m) == Q2_MREQ_MP_LOAD_SETTINGS,
          "LOAD SETTINGS raises load");

    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_goto(&m, Q2_PAGE_FRONT_MULTI);
    m.cursor = 4;
    tap(&m, Q2_PAD_CROSS);
    CHECK(q2_menu_take_request(&m) == Q2_MREQ_MP_SAVE_SETTINGS,
          "SAVE SETTINGS raises save");

    for (level = 0; level < 4; level++) {
        p = q2_menu_front_variables_page(level);
        CHECK(p->count == vars_count[level],
              "front cheat level %d gives %u rows", level, p->count);
    }
    CHECK(strcmp(q2_menu_mp_arena_directory(0), "MATRIX6") == 0 &&
          strcmp(q2_menu_mp_arena_name(11), "BADLANDS") == 0 &&
          strcmp(q2_menu_mp_arena_directory(11), "MATRIX5") == 0,
          "arena endpoints match the level table");
}

/* 0x8001FD18 counts everything but the four control letters. */
static void test_text_length(void)
{
    CHECK(q2_menu_text_length("bON") == 2, "codes must not count");
    CHECK(q2_menu_text_length("gRESUPPLY") == 8, "grey prefix must not count");
    CHECK(q2_menu_text_length("QUIT GAME") == 9, "plain text");
    CHECK(q2_menu_is_code('b') && q2_menu_is_code('d') &&
          q2_menu_is_code('g') && q2_menu_is_code('u'), "the four codes");
    CHECK(!q2_menu_is_code('a') && !q2_menu_is_code('G'), "nothing else is one");
}

/* 0x8001CF74 on a PAL framebuffer. */
static void test_title_y(void)
{
    CHECK(q2_menu_title_y(248) == 40, "PAL title y is %d", q2_menu_title_y(248));
    /* The NTSC build derives it from its own 240 lines: PAL's less four, which
     * is exactly where centring the 248-line block puts PAL's 40. */
    CHECK(q2_menu_title_y(240) == 36, "NTSC title y is %d", q2_menu_title_y(240));
}

/*
 * The NTSC build moved every row of its page tables up four lines — except the
 * memory card's SAVE FILE screen (0x8009B114), which it left where PAL had it.
 * Centring the block does the four for every page, so that page's rows are put
 * back down by the same four in block coordinates. And two words are spelt the
 * North American way.
 */
static void test_ntsc(void)
{
    q2_menu m;
    q2_menu_settings set;
    u32 count, k;
    const q2_menu_page *mc = q2_mcard_pages(&count);
    const q2_menu_page *save_file = NULL;

    for (k = 0; k < count; k++)
        if (mc[k].addr == 0x8009B114u)
            save_file = &mc[k];
    CHECK(save_file != NULL, "the SAVE FILE screen is transcribed");
    if (!save_file)
        return;

    q2_menu_settings_defaults(&set);
    q2_menu_init(&m, &set, Q2_MENU_SCREEN_H);

    m.page = save_file;
    CHECK(q2_menu_item_y(&m, 0) == 40, "PAL: SAVE FILE heading at its table's 40");
    q2_menu_set_fb_height(&m, 240);
    CHECK(q2_menu_item_y(&m, 0) == 44,
          "NTSC: +4 in the block, so 40 on the screen once centred, got %d",
          q2_menu_item_y(&m, 0));

    m.page = q2_menu_page_find(Q2_PAGE_OPTIONS);
    CHECK(m.page && q2_menu_item_y(&m, 0) == m.page->items[0].y,
          "every other page keeps its table's y inside the block");

    CHECK(strcmp(q2_menu_word("AUTOCENTRE", true), "AUTOCENTER") == 0, "AUTOCENTER");
    CHECK(strcmp(q2_menu_word("COLOSSEUM", true), "COLISEUM") == 0, "COLISEUM");
    CHECK(strcmp(q2_menu_word("AUTOCENTRE", false), "AUTOCENTRE") == 0,
          "PAL keeps its own spelling");
    CHECK(strcmp(q2_menu_word("SOUND", true), "SOUND") == 0,
          "and nothing else is respelt");

    /* SCREEN POSITION: 24 lines down PAL's raster, 0 on NTSC's. */
    CHECK(q2_menu_screen_y_default(248) == 24 && q2_menu_screen_y_default(240) == 0,
          "screen Y defaults");
    q2_menu_reset_video_for(&set, 240);
    CHECK(set.v[Q2_SET_SCREEN_Y] == 0, "NTSC reset leaves Y at %d", set.v[Q2_SET_SCREEN_Y]);
    q2_menu_reset_video(&set);
    CHECK(set.v[Q2_SET_SCREEN_Y] == 24, "PAL reset puts Y at %d", set.v[Q2_SET_SCREEN_Y]);
}

/* ------------------------------------------------------------------------- */
/*
 * The font's cell arithmetic (FORMATS.md §10.7).
 *
 * These are the numbers a wrong reading gets *plausibly* wrong: the locator at
 * 0x8001B494 computes a cell rather than looking one up, so a bad column count
 * or v origin still lands inside the atlas and still draws letters — the wrong
 * halves of the right ones. Nothing here needs a disc, because the arithmetic
 * is code, not data.
 */
static void test_font_metrics(void)
{
    const q2_menu_face *item  = q2_menu_face_get(Q2_MENU_FACE_ITEM);
    const q2_menu_face *title = q2_menu_face_get(Q2_MENU_FACE_TITLE);
    const q2_menu_face *small = q2_menu_face_get(Q2_MENU_FACE_SMALL);

    CHECK(item && title && small, "all three faces exist");
    CHECK(q2_menu_face_get(24) == NULL, "there is no fourth size");
    if (!item || !title || !small)
        return;

    /* 0x8001AD6C: s6 is the cell width and the advance, s2 the height. */
    CHECK(item->cell_w == 16 && item->cell_h == 11 && item->advance == 16,
          "the 16 face is 16x11 with a 16 advance, got %ux%u/%u",
          item->cell_w, item->cell_h, item->advance);
    CHECK(title->cell_w == 32 && title->cell_h == 20 && title->advance == 32,
          "the 32 face is 32x20 with a 32 advance, got %ux%u/%u",
          title->cell_w, title->cell_h, title->advance);
    CHECK(small->cell_w == 8 && small->cell_h == 8 && small->advance == 8,
          "the 8 face is 8x8");

    /* Rows of 15 and 7, and the 16 face sits 100 rows down the page. */
    CHECK(item->cols == 15 && item->v_origin == 100,
          "the 16 face is 15 columns at v 100, got %u/%u",
          item->cols, item->v_origin);
    CHECK(title->cols == 7 && title->v_origin == 0,
          "the 32 face is 7 columns at v 0, got %u/%u",
          title->cols, title->v_origin);

    /* An advance is the face size even where the cell is not square: the
     * 16 face steps 16 across while being only 11 tall. */
    CHECK(q2_menu_font_width(Q2_MENU_FACE_ITEM, "ABCDEFGHIJKLMNO") == 240,
          "fifteen glyphs at 16 is 240 wide");
    CHECK(q2_menu_font_width(Q2_MENU_FACE_TITLE, "PAUSED") == 192,
          "six glyphs at 32 is 192 wide");
    /* Colour codes are consumed, not printed (0x8001FD18), so a toggle's
     * recomposed row measures as "ON OFF" — six cells, not eight. */
    CHECK(q2_menu_font_width(Q2_MENU_FACE_ITEM, "bON dOFF") == 6 * 16,
          "the codes do not take space, got %d",
          q2_menu_font_width(Q2_MENU_FACE_ITEM, "bON dOFF"));
}

static void test_font_coverage(void)
{
    int c;

    /* Letters and digits exist at both frontend faces; punctuation is row 2,
     * and only the twelve marks with a jump-table arm of their own. */
    for (c = 'A'; c <= 'Z'; c++) {
        CHECK(q2_menu_glyph_defined(Q2_MENU_FACE_ITEM, (char)c),
              "'%c' exists at 16", c);
        CHECK(q2_menu_glyph_defined(Q2_MENU_FACE_TITLE, (char)c),
              "'%c' exists at 32", c);
    }
    for (c = '0'; c <= '9'; c++)
        CHECK(q2_menu_glyph_defined(Q2_MENU_FACE_ITEM, (char)c),
              "'%c' exists at 16", c);

    {
        static const char *have = "-:/.?'!,&()i";
        const char *s;
        for (s = have; *s; s++)
            CHECK(q2_menu_glyph_defined(Q2_MENU_FACE_ITEM, *s),
                  "'%c' has a punctuation column", *s);
    }

    /* The default arm draws no cell — and a port must not quietly turn these
     * into spaces, because the original does something else entirely. */
    CHECK(!q2_menu_glyph_defined(Q2_MENU_FACE_ITEM, '#'), "'#' has no cell");
    CHECK(!q2_menu_glyph_defined(Q2_MENU_FACE_ITEM, '"'), "'\"' has no cell");
    CHECK(!q2_menu_glyph_defined(Q2_MENU_FACE_ITEM, '$'), "'$' has no cell");

    /*
     * The four size-32 overrides, which exist because rows 0..2 of the
     * seven-wide face are already letters. Everything else about the big face
     * is A..Z, so '5' must NOT resolve while '4' must.
     */
    CHECK(q2_menu_glyph_defined(Q2_MENU_FACE_TITLE, '2'), "'2' at 32");
    CHECK(q2_menu_glyph_defined(Q2_MENU_FACE_TITLE, '3'), "'3' at 32");
    CHECK(q2_menu_glyph_defined(Q2_MENU_FACE_TITLE, '4'), "'4' at 32");
    CHECK(q2_menu_glyph_defined(Q2_MENU_FACE_TITLE, '?'), "'?' at 32");

    /* Every one of the seven page titles must be drawable at size 32, which is
     * the property the four overrides were added to preserve. */
    {
        static const char *titles[] = {
            "PAUSED", "OPTIONS", "PLAYER", "SOUND", "VIDEO", "CONTROLLER",
            "POSITION"
        };
        size_t i;
        for (i = 0; i < sizeof(titles) / sizeof(titles[0]); i++) {
            const char *s;
            for (s = titles[i]; *s; s++)
                CHECK(*s == ' ' ||
                      q2_menu_glyph_defined(Q2_MENU_FACE_TITLE, *s),
                      "'%c' of \"%s\" is drawable at 32", *s, titles[i]);
        }
    }
}

static void test_icons_variant(void)
{
    /* 0x8003FEAC picks the sheet by session mode, then by player count. */
    CHECK(strcmp(q2_menu_icons_name(false, 1), "qk_menu.lbm") == 0,
          "single player uses qk_menu.lbm");
    CHECK(strcmp(q2_menu_icons_name(true, 2), "qk2_menu.lbm") == 0,
          "two players use qk2_menu.lbm");
    CHECK(strcmp(q2_menu_icons_name(true, 4), "qkm_menu.lbm") == 0,
          "four players use qkm_menu.lbm");
    /* The branch order is mode first: a single-player session with a stale
     * player count still gets the single-player sheet. */
    CHECK(strcmp(q2_menu_icons_name(false, 4), "qk_menu.lbm") == 0,
          "the session mode decides before the player count");
}

/* ------------------------------------------------------------------------- */
/* The memory-card front end (FORMATS.md §10.10).                             */
/* ------------------------------------------------------------------------- */
static int g_poll_state;
static int g_requested = -1;
static int g_chosen    = -1;

static int  mc_poll(void *user)               { (void)user; return g_poll_state; }
static void mc_request(void *user, int state) { (void)user; g_requested = state; }
static void mc_choose(void *user, int row)    { (void)user; g_chosen = row; }

static void mc_open(q2_mcard *m, q2_mcard_host *h)
{
    memset(h, 0, sizeof(*h));
    h->poll    = mc_poll;
    h->request = mc_request;
    h->choose  = mc_choose;
    q2_mcard_init(m, h);
    g_requested = -1;
    g_chosen    = -1;
}

/* Cross down for a frame, then up: the release is what every arm tests. */
static void mc_release_cross(q2_mcard *m)
{
    q2_mcard_advance(m, Q2_PAD_CROSS);
    q2_mcard_advance(m, 0);
}

static void test_mcard_screens(void)
{
    u32 count = 0;
    const q2_menu_page *p = q2_mcard_pages(&count);
    u32 i;

    CHECK(p != NULL && count == 9, "nine memory-card screens, got %u", count);
    if (!p)
        return;

    for (i = 0; i < count; i++) {
        CHECK(p[i].id == 0, "screen %u has no page id", i);
        CHECK(p[i].addr != 0, "screen %u names its table", i);
        CHECK(p[i].first <= p[i].count, "screen %u's first is in range", i);
    }

    /* The two-table screens are question-then-answers, so only the answers
     * are navigable — the same idiom as the pause confirmations. */
    {
        const q2_menu_page *nf = q2_mcard_page(Q2_MCARD_NOT_FORMATTED);
        const q2_menu_page *ow = q2_mcard_page(Q2_MCARD_OVERWRITE);
        CHECK(nf && nf->addr2 == 0x8009B204u && nf->first == 3,
              "NOT FORMATTED's answers start at 3");
        CHECK(ow && ow->addr2 == 0x8009B36Cu && ow->first == 2,
              "OVERWRITE's answers start at 2");
    }

    /* SAVE FILE is a heading plus four empty, navigable slot rows. */
    {
        const q2_menu_page *sf = q2_mcard_page(Q2_MCARD_SAVE_FILE);
        CHECK(sf && sf->count == 1 + Q2_MCARD_SLOTS && sf->first == 1,
              "SAVE FILE is a heading and four slots");
        if (sf) {
            int k;
            for (k = 1; k <= Q2_MCARD_SLOTS; k++)
                CHECK(sf->items[k].label[0] == '\0',
                      "slot row %d ships empty", k);
        }
    }

    /* A pure-text screen has nothing selectable, which the engine expresses
     * as first == count. */
    {
        const q2_menu_page *fm = q2_mcard_page(Q2_MCARD_FORMATTING);
        CHECK(fm && fm->first == fm->count,
              "FORMATTING is not navigable");
    }

    CHECK(q2_mcard_page(Q2_MCARD_NONE) == NULL, "NONE has no page");
    CHECK(q2_mcard_page(Q2_MCARD_SCREEN_COUNT) == NULL, "the end has no page");
}

static void test_mcard_states(void)
{
    int s;
    int live = 0;

    for (s = 1; s <= Q2_MCARD_STATE_MAX; s++)
        if (q2_mcard_state_live(s))
            live++;
    CHECK(live == 6, "six of the nineteen entries have an arm, got %d", live);

    CHECK(q2_mcard_state_live(Q2_MCARD_STATE_LIST), "3 is live");
    CHECK(q2_mcard_state_live(Q2_MCARD_STATE_CHOICE_19), "19 is live");
    CHECK(!q2_mcard_state_live(1) && !q2_mcard_state_live(12),
          "the fall-through states are not");

    /* Only the two the arms settle are mapped; guessing the rest would look
     * identical in the port and be wrong on the console. */
    CHECK(q2_mcard_screen_for_state(Q2_MCARD_STATE_LIST) == Q2_MCARD_SAVE_FILE,
          "state 3 is the slot list");
    CHECK(q2_mcard_screen_for_state(Q2_MCARD_STATE_REPORT) ==
          Q2_MCARD_LOAD_MESSAGE, "state 13 is the load message");
    CHECK(q2_mcard_screen_for_state(Q2_MCARD_STATE_CHOICE_5) == Q2_MCARD_NONE,
          "state 5's screen is not established");
}

static void test_mcard_fires_on_release(void)
{
    q2_mcard m;
    q2_mcard_host h;

    mc_open(&m, &h);
    g_poll_state = Q2_MCARD_STATE_LIST;
    m.cursor = 2;

    /* Holding is not firing. */
    q2_mcard_advance(&m, Q2_PAD_CROSS);
    CHECK(!m.fired && g_chosen == -1, "the press does nothing");

    /* Letting go is. */
    q2_mcard_advance(&m, 0);
    CHECK(m.fired, "the release fires");
    CHECK(g_chosen == 2, "the chosen row is the cursor, got %d", g_chosen);
    CHECK(g_requested == Q2_MCARD_STATE_LIST,
          "a transition is requested after the row");
}

static void test_mcard_choices(void)
{
    q2_mcard m;
    q2_mcard_host h;

    /* 0x8001F790: row 1 goes on to 6, row 0 goes back to 1. */
    mc_open(&m, &h);
    g_poll_state = Q2_MCARD_STATE_CHOICE_5;
    m.cursor = 1;
    mc_release_cross(&m);
    CHECK(g_requested == 6, "state 5 row 1 asks for 6, got %d", g_requested);

    mc_open(&m, &h);
    g_poll_state = Q2_MCARD_STATE_CHOICE_5;
    m.cursor = 0;
    mc_release_cross(&m);
    CHECK(g_requested == 1, "state 5 row 0 asks for 1, got %d", g_requested);

    /* 0x8001F718: row 1 goes to 20; row 0 falls through with no transition,
     * which is a return rather than a request for state zero. */
    mc_open(&m, &h);
    g_poll_state = Q2_MCARD_STATE_CHOICE_19;
    m.cursor = 1;
    mc_release_cross(&m);
    CHECK(g_requested == 20, "state 19 row 1 asks for 20, got %d", g_requested);

    mc_open(&m, &h);
    g_poll_state = Q2_MCARD_STATE_CHOICE_19;
    m.cursor = 0;
    mc_release_cross(&m);
    CHECK(g_requested == -1, "state 19 row 0 asks for nothing");
}

static void test_mcard_accept(void)
{
    q2_mcard m;
    q2_mcard_host h;
    bool done;

    /* 14 and 16 share one arm, and both mean "apply and leave". */
    mc_open(&m, &h);
    g_poll_state = Q2_MCARD_STATE_ACCEPT_A;
    q2_mcard_advance(&m, Q2_PAD_CROSS);
    done = q2_mcard_advance(&m, 0);
    CHECK(done, "state 14 accepts");

    mc_open(&m, &h);
    g_poll_state = Q2_MCARD_STATE_ACCEPT_B;
    q2_mcard_advance(&m, Q2_PAD_CROSS);
    done = q2_mcard_advance(&m, 0);
    CHECK(done, "state 16 accepts through the same arm");

    /* A fall-through state does nothing at all, however the pad moves. */
    mc_open(&m, &h);
    g_poll_state = 12;
    q2_mcard_advance(&m, Q2_PAD_CROSS);
    done = q2_mcard_advance(&m, 0);
    CHECK(!done && !m.fired && g_requested == -1,
          "a state with no arm is inert");
}

static void test_mcard_names(void)
{
    q2_mcard m;
    q2_mcard_host h;

    mc_open(&m, &h);
    q2_mcard_set_name(&m, 0, "QUAKE2 BASE1");
    CHECK(strcmp(m.name[0], "QUAKE2 BASE1") == 0, "a slot name is kept");
    q2_mcard_set_name(&m, 0, NULL);
    CHECK(m.name[0][0] == '\0', "NULL empties a slot");
    /* Out of range is ignored rather than scribbling. */
    q2_mcard_set_name(&m, Q2_MCARD_SLOTS, "X");
    q2_mcard_set_name(&m, -1, "X");
    CHECK(m.name[0][0] == '\0', "an out-of-range slot writes nothing");
}

/* ------------------------------------------------------------------------- */
/* The pointer — menumouse.c, which is the inverse of menudraw.c              */
/* ------------------------------------------------------------------------- */

static int last_selectable_index(const q2_menu *m)
{
    int i, found = -1;

    for (i = (int)m->page->first; i < (int)m->page->count; i++)
        if (q2_menu_item_selectable(m, i))
            found = i;
    return found;
}

/* The centre of an item's row, which is where a pointer aiming at it lands. */
static void item_centre(const q2_menu *m, int index, int *x, int *y)
{
    int x0, y0, x1, y1;

    if (!q2_menu_item_rect(m, index, &x0, &y0, &x1, &y1)) {
        *x = *y = -10000;
        return;
    }
    *x = (x0 + x1) / 2;
    *y = (y0 + y1) / 2;
}

static void test_hit_rows(void)
{
    q2_menu_settings s;
    q2_menu m;
    q2_menu_hit hit;
    int i, x, y;

    q2_menu_settings_defaults(&s);
    open_menu(&m, &s, false);   /* the single-player pause page */

    /* Every navigable row can be hit at its own centre, and reports itself. */
    for (i = (int)m.page->first; i < (int)m.page->count; i++) {
        if (!q2_menu_item_selectable(&m, i))
            continue;
        item_centre(&m, i, &x, &y);
        CHECK(q2_menu_hit_test(&m, x, y, &hit) && hit.index == i,
              "row %d hits itself (got %d)", i, hit.index);
    }

    /* The rows are 24 apart with an 11-pixel face, so a point well above the
     * first is on nothing at all. */
    item_centre(&m, (int)m.page->first, &x, &y);
    CHECK(!q2_menu_hit_test(&m, x, y - 40, &hit) && hit.index == -1,
          "the gap above the first row hits nothing");

    /* And so is a point far off to the side of a centred label. */
    CHECK(!q2_menu_hit_test(&m, x - 240, y, &hit),
          "beside the label hits nothing");
}

static void test_hit_skips_the_unreachable(void)
{
    q2_menu_settings s;
    q2_menu m;
    q2_menu_hit hit;
    int x, y;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    q2_menu_open(&m);
    q2_menu_goto(&m, Q2_PAGE_RESTART_CONFIRM);
    q2_menu_advance(&m, 0);

    /* Item 0 is the question, which sits above the navigable group. The d-pad
     * cannot reach it and neither may the pointer. */
    item_centre(&m, 0, &x, &y);
    CHECK(!q2_menu_hit_test(&m, x, y, &hit),
          "the static question is not clickable");

    /* Both answers are. */
    item_centre(&m, 1, &x, &y);
    CHECK(q2_menu_hit_test(&m, x, y, &hit) && hit.index == 1, "NO is clickable");
    item_centre(&m, 2, &x, &y);
    CHECK(q2_menu_hit_test(&m, x, y, &hit) && hit.index == 2, "YES is clickable");

    /* The death page greys its middle row when there are no resupplies, which
     * takes it out of the navigation — and out of reach of the pointer. */
    q2_menu_set_resupplies(&m, 0);
    q2_menu_goto(&m, Q2_PAGE_DEATH);
    item_centre(&m, 1, &x, &y);
    CHECK(!q2_menu_hit_test(&m, x, y, &hit), "a greyed row is not clickable");
}

static void test_hit_toggle_words(void)
{
    q2_menu_settings s;
    q2_menu m;
    q2_menu_hit hit;
    int x0, y0, x1, y1, y;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    q2_menu_open(&m);
    q2_menu_goto(&m, Q2_PAGE_SOUND);
    q2_menu_advance(&m, 0);

    /* STEREO is item 2: "STEREO ON OFF" when the cursor is on it. */
    CHECK(q2_menu_item_rect(&m, 2, &x0, &y0, &x1, &y1), "the toggle has a rect");
    y = (y0 + y1) / 2;

    /* The row is hit at its widest whether or not the cursor is on it, which is
     * what stops the box growing as the pointer enters. */
    CHECK(q2_menu_hit_test(&m, x1 - 1, y, &hit) &&
          hit.index == 2 && hit.part == Q2_MENU_HIT_OFF,
          "the right end of the row is OFF (part %d)", (int)hit.part);

    /* Six columns back from the end is ON; the face advances 16 a column. */
    CHECK(q2_menu_hit_test(&m, x1 - 1 - 5 * 16, y, &hit) &&
          hit.index == 2 && hit.part == Q2_MENU_HIT_ON,
          "five columns back is ON (part %d)", (int)hit.part);

    /* And the label itself is neither. */
    CHECK(q2_menu_hit_test(&m, x0 + 8, y, &hit) &&
          hit.index == 2 && hit.part == Q2_MENU_HIT_LABEL,
          "the label is the label (part %d)", (int)hit.part);
}

static void test_hit_slider(void)
{
    q2_menu_settings s;
    q2_menu m;
    q2_menu_hit hit;
    int x0, y0, x1, y1, y, v;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    q2_menu_open(&m);
    q2_menu_goto(&m, Q2_PAGE_SOUND);
    q2_menu_advance(&m, 0);

    /* MUSIC is item 0, and its rect runs out over the 133-pixel track. */
    CHECK(q2_menu_item_rect(&m, 0, &x0, &y0, &x1, &y1), "the slider has a rect");
    y = (y0 + y1) / 2;

    CHECK(q2_menu_hit_test(&m, x1, y, &hit) &&
          hit.index == 0 && hit.part == Q2_MENU_HIT_SLIDER,
          "the track is the slider (part %d)", (int)hit.part);

    /* 0x8001BD28 fills to bar_x + value + 3, so the last pixel of the 133-wide
     * track is past the maximum and pins to it. */
    CHECK(hit.value == Q2_MENU_SLIDER_MAX, "the far end is %d, want %d",
          hit.value, Q2_MENU_SLIDER_MAX);

    /* And the mapping in between is that formula read backwards: the track
     * starts at x1 - 132, so 50 units along it is 53 pixels in. */
    CHECK(q2_menu_hit_test(&m, x1 - 132 + 53, y, &hit) && hit.value == 50,
          "53 pixels along the track is 50 (got %d)", hit.value);

    CHECK(q2_menu_slider_at(&m, 0, -10000, &v) && v == 0,
          "a drag off the left pins to 0 (got %d)", v);
    CHECK(q2_menu_slider_at(&m, 0, 10000, &v) && v == Q2_MENU_SLIDER_MAX,
          "a drag off the right pins to the max (got %d)", v);

    /* STEREO is not a slider, and saying so is how the client knows a drag
     * belongs to something else. */
    CHECK(!q2_menu_slider_at(&m, 2, 0, &v), "a toggle is not a slider");

    /* And setting one goes through the same clamp. */
    CHECK(q2_menu_set_slider(&m, 0, 200) &&
          s.v[Q2_SET_MUSIC] == Q2_MENU_SLIDER_MAX,
          "an over-range set clamps (got %d)", s.v[Q2_SET_MUSIC]);
    CHECK(q2_menu_set_slider(&m, 0, -5) && s.v[Q2_SET_MUSIC] == 0,
          "an under-range set clamps (got %d)", s.v[Q2_SET_MUSIC]);
    CHECK(!q2_menu_set_slider(&m, 2, 10), "a toggle cannot be set as a slider");
}

static void test_hit_choice_halves(void)
{
    q2_menu_settings s;
    q2_menu m;
    q2_menu_hit hit;
    int x0, y0, x1, y1, y;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    q2_menu_open(&m);
    q2_menu_goto(&m, Q2_PAGE_CONTROLLER);
    q2_menu_advance(&m, 0);

    CHECK(q2_menu_item_rect(&m, 0, &x0, &y0, &x1, &y1), "the choice has a rect");
    y = (y0 + y1) / 2;

    CHECK(q2_menu_hit_test(&m, x0 + 2, y, &hit) &&
          hit.part == Q2_MENU_HIT_PREV, "the left half steps back");
    CHECK(q2_menu_hit_test(&m, x1 - 2, y, &hit) &&
          hit.part == Q2_MENU_HIT_NEXT, "the right half steps on");
}

static void test_point_at(void)
{
    q2_menu_settings s;
    q2_menu m;
    int last;

    q2_menu_settings_defaults(&s);
    open_menu(&m, &s, false);

    last = last_selectable_index(&m);
    CHECK(m.cursor != last, "the cursor does not start on the last row");

    CHECK(q2_menu_point_at(&m, last) && m.cursor == last,
          "pointing moves the cursor straight there");
    CHECK(q2_menu_take_sound(&m) == Q2_MSND_MOVE,
          "and it sounds like a cursor move");

    /* Pointing at where it already is is not a move, and must not click. */
    CHECK(!q2_menu_point_at(&m, last), "pointing at the current row does nothing");
    CHECK(q2_menu_take_sound(&m) == Q2_MSND_NONE, "and makes no sound");

    /* The static group above a page's navigable part is not a place to land. */
    q2_menu_goto(&m, Q2_PAGE_RESTART_CONFIRM);
    CHECK(!q2_menu_point_at(&m, 0), "the static question cannot be pointed at");
}

/* 0x8001A280..0x8001A348, plus QFRONT module+0x4618..0x4738. */
static void test_prompt_menu_policy(void)
{
    q2_menu_settings s;
    q2_prompt_bar b;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    q2_menu_open(&m);
    q2_prompt_init(&b);

    /* The retail title capture has SELECT alone. */
    q2_menu_goto(&m, Q2_PAGE_FRONT_TITLE);
    q2_prompt_sync_menu(&b, &m, true);
    CHECK(b.rec[Q2_PROMPT_SELECT].y_target == 208, "title SELECT target");
    CHECK(b.rec[Q2_PROMPT_BACK].y_target == Q2_PROMPT_Y_HIDDEN,
          "title has no BACK");
    CHECK(b.rec[Q2_PROMPT_RULES].y_target == Q2_PROMPT_Y_HIDDEN,
          "title has no RULES");

    /* START, SINGLE PLAYER and DIFFICULTY all show BACK + SELECT in the
     * captured retail page sequence. */
    q2_menu_goto(&m, Q2_PAGE_FRONT_START);
    q2_prompt_sync_menu(&b, &m, true);
    CHECK(b.rec[Q2_PROMPT_SELECT].y_target == 208, "START SELECT target");
    CHECK(b.rec[Q2_PROMPT_BACK].y_target == 208, "START BACK target");
    q2_menu_goto(&m, Q2_PAGE_FRONT_NEWLOAD);
    q2_prompt_sync_menu(&b, &m, true);
    CHECK(b.rec[Q2_PROMPT_SELECT].y_target == 208, "SINGLE PLAYER SELECT");
    CHECK(b.rec[Q2_PROMPT_BACK].y_target == 208, "SINGLE PLAYER BACK");
    q2_menu_goto(&m, Q2_PAGE_FRONT_SKILL);
    q2_prompt_sync_menu(&b, &m, true);
    CHECK(b.rec[Q2_PROMPT_SELECT].y_target == 208, "DIFFICULTY SELECT");
    CHECK(b.rec[Q2_PROMPT_BACK].y_target == 208, "DIFFICULTY BACK");

    /* RULES is lower than QFRONT's other two prompts and follows only the
     * first three MULTIPLAYER rows. */
    q2_menu_goto(&m, Q2_PAGE_FRONT_MULTI);
    q2_prompt_sync_menu(&b, &m, true);
    CHECK(b.rec[Q2_PROMPT_RULES].y_target == 212, "mode row RULES target");
    m.cursor = (int)m.page->first + 3;
    q2_prompt_sync_menu(&b, &m, true);
    CHECK(b.rec[Q2_PROMPT_RULES].y_target == Q2_PROMPT_Y_HIDDEN,
          "settings row parks RULES");

    /* In-game widgets have no action pointer, while their page still has a
     * back handler; the retail in-game bar uses caller y = 220. */
    q2_menu_goto(&m, Q2_PAGE_PLAYER);
    q2_prompt_sync_menu(&b, &m, false);
    CHECK(b.rec[Q2_PROMPT_SELECT].y_target == Q2_PROMPT_Y_HIDDEN,
          "toggle row has no SELECT");
    CHECK(b.rec[Q2_PROMPT_BACK].y_target == 212, "in-game BACK target");

    q2_menu_close(&m);
    q2_prompt_sync_menu(&b, &m, false);
    CHECK(b.rec[Q2_PROMPT_SELECT].y_target == Q2_PROMPT_Y_HIDDEN &&
          b.rec[Q2_PROMPT_BACK].y_target == Q2_PROMPT_Y_HIDDEN &&
          b.rec[Q2_PROMPT_RULES].y_target == Q2_PROMPT_Y_HIDDEN,
          "closing the menu parks the whole bar");
}

/* ------------------------------------------------------------------------- */
/*
 * THE SAVE? PROMPT, and what puts it up.
 *
 * On the console arriving at a new level stops the game and asks; the port
 * used to walk straight in, and the screen existed only behind a function key
 * because "what SHOWS that prompt is not established". It is established:
 * GameCheckPaused's arm at 0x8003E6FC..0x8003E75C, transcribed as
 * q2_mcard_save_prompt_due.
 *
 * The table is the other half. `first` is 1, so the cursor lands on YES, and
 * the port reads the answer POSITIONALLY the way the card front end does
 * (`cursor - first`, 0x800B32AC minus 0x800B32AE) — which means the row order
 * decides whether a player who said NO gets a save written. It is checked
 * against the addresses the two tables live at.
 */
static void test_save_prompt(void)
{
    const q2_menu_page *sp = q2_mcard_page(Q2_MCARD_SAVE_PROMPT);
    u32 latch = 0;

    CHECK(sp && sp->addr == 0x8009B3B4u && sp->addr2 == 0x8009B3E4u,
          "SAVE? is the caption table plus the answers");
    if (sp) {
        CHECK(sp->count == 3 && sp->first == 1,
              "one caption and two answers, the cursor starting on the first");
        CHECK(sp->items[0].y == 98 && sp->items[1].y == 124 &&
              sp->items[2].y == 150,
              "at 98, 124 and 150 (0x8009B3B4, 0x8009B3E4)");
        CHECK(sp->items[0].x == 256 && sp->items[1].x == 256 &&
              sp->items[2].x == 256, "all three centred at 256");
        /* Row 0 relative to `first` is YES and row 1 is NO. Swap them and a
         * player who answers NO has their game written over. */
        CHECK(sp->items[sp->first + 0].label &&
              strcmp(sp->items[sp->first + 0].label, "YES") == 0,
              "the first answer is YES (0x80020428)");
        CHECK(sp->items[sp->first + 1].label &&
              strcmp(sp->items[sp->first + 1].label, "NO") == 0,
              "the second is NO (0x80020140, InGame_GOTO_RESUME)");
        CHECK(sp->items[sp->first + 0].on_release &&
              sp->items[sp->first + 1].on_release,
              "and both fire on the release");
    }

    /* 0x8003E738: exit code 2, a LOADMAP, is what asks. */
    CHECK(q2_mcard_save_prompt_due(1, &latch, false, 2),
          "a level reached by LOADMAP asks");
    CHECK(latch == 1, "and the latch follows the counter (0x8003E728)");

    /* 0x8003E70C: the same load is not asked about twice. This is what stops
     * the per-frame checker re-raising the page it has just raised. */
    CHECK(!q2_mcard_save_prompt_due(1, &latch, false, 2),
          "the same load is asked about once");

    /* 0x8003E740: 4 is the only other exit code that raises it. */
    latch = 0;
    CHECK(q2_mcard_save_prompt_due(7, &latch, false, 4), "exit code 4 asks");

    /*
     * AND THE LATCH IS WRITTEN BEFORE THE EXIT CODE IS READ (0x8003E728 then
     * 0x8003E730), which is the whole reason a RESTART does not ask: the load
     * it arrived on spends the counter, so the question is never put, then or
     * on any later frame of that level.
     */
    latch = 0;
    CHECK(!q2_mcard_save_prompt_due(3, &latch, false, 21),
          "a level reached by RESTART does not ask");
    CHECK(latch == 3, "but it spends the counter all the same");
    CHECK(!q2_mcard_save_prompt_due(3, &latch, false, 2),
          "so nothing can ask about that load afterwards");

    /* 0x8003E720, and it comes BEFORE the latch write: an arena leaves the
     * latch where it was. */
    latch = 0;
    CHECK(!q2_mcard_save_prompt_due(5, &latch, true, 2),
          "deathmatch never asks");
    CHECK(latch == 0, "and does not spend the counter either");
}

int main(void)
{
    test_font_upload_alias_order();
    test_defaults();
    test_variables_apply();
    test_navigation();
    test_multiplayer_pause();
    test_grey_is_skipped();
    test_death_is_inert_at_first();
    test_toggle();
    test_slider();
    test_music_audition();
    test_choice();
    test_press_versus_release();
    test_page_graph();
    test_cursor_policy();
    test_vibration_needs_a_shockable_pad();
    test_paused_banner_colour();
    test_quit_flow();
    test_restart_flow();
    test_text_only_page();
    test_screen_position();
    test_variables_pages();
    test_video_variant();
    test_pause_status_line();
    test_reset_video_uses_the_framebuffer();
    test_front_end_tables();
    test_front_rules_pages();
    test_front_multiplayer_setup();
    test_text_length();
    test_title_y();
    test_ntsc();
    test_font_metrics();
    test_font_coverage();
    test_icons_variant();
    test_mcard_screens();
    test_mcard_states();
    test_mcard_fires_on_release();
    test_mcard_choices();
    test_mcard_accept();
    test_mcard_names();
    test_save_prompt();

    test_hit_rows();
    test_hit_skips_the_unreachable();
    test_hit_toggle_words();
    test_hit_slider();
    test_hit_choice_halves();
    test_point_at();
    test_prompt_menu_policy();

    if (g_fail) {
        printf("\n%d menu check%s failed\n", g_fail, g_fail == 1 ? "" : "s");
        return 1;
    }
    printf("menu: all checks passed\n");
    return 0;
}
