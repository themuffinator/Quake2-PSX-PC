/*
 * test_loading.c — the loading screen's behaviour and its geometry.
 *
 * `q2psx-inspect menu <disc>` already reads `0x800A3314` off a real executable
 * and compares it record by record, so nothing here re-asserts the word LOADING
 * or where it sits. What it pins down is everything that is NOT in a table: the
 * half-second floor, that the floor is a floor and not a target, that a
 * transition which loads twice is still one screen, the turn rate, and the
 * arithmetic that puts the logo in the corner — which is the one thing on this
 * screen that has a wrong answer as well as a right one, because the view basis
 * scales x by 3/2 and a placement that ignores it lands off the screen.
 */
#include "loading.h"

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

/*
 * A screen with no disc behind it.
 *
 * `ready` is what q2_loading_open sets, and it is set here directly: whether
 * QDUMMY is on the disc is a disc question and the inspect tool's own to ask.
 * Everything below is about what the screen DOES once it is there.
 */
static void armed(q2_loading *l)
{
    memset(l, 0, sizeof(*l));
    l->ready = true;
}

/* ------------------------------------------------------------------------- */
/* The page                                                                   */
/* ------------------------------------------------------------------------- */
static void test_page(void)
{
    const q2_menu_page *p = q2_menu_page_find(Q2_PAGE_LOADING);

    CHECK(p != NULL, "no page for Q2_PAGE_LOADING");
    if (!p)
        return;

    CHECK(p->count == 1, "the page holds %u records, not 1", p->count);
    CHECK(p->title == NULL, "the loading screen installs no page title");
    CHECK(strcmp(p->items[0].label, "LOADING") == 0,
          "the row reads \"%s\"", p->items[0].label);
    CHECK(p->items[0].x == 256 && p->items[0].y == 124,
          "the row is at (%d,%d), not (256,124)",
          (int)p->items[0].x, (int)p->items[0].y);

    /*
     * PURE TEXT, which is what keeps the selection bar off it. `0x80079184`'s
     * second install is `0x800A3344`, a NULL record, so the last call to the
     * item loader leaves nothing navigable — the same shape RESTARTING and
     * QUITTING have (menu.h's `first`). A page with a navigable row would draw
     * a blue bar across a screen that has one word on it.
     */
    CHECK(p->first == p->count, "the page is navigable (first %u of %u)",
          p->first, p->count);
    CHECK(p->items[0].action == Q2_ACT_NONE, "the row carries an action");
    CHECK(p->back == Q2_ACT_NONE, "the screen answers TRIANGLE");
}

/* ------------------------------------------------------------------------- */
/* The hold                                                                   */
/* ------------------------------------------------------------------------- */
static void test_hold_is_half_a_second(void)
{
    q2_loading l;
    double     shown = 0.0;
    int        frames = 0;

    armed(&l);
    CHECK(!l.open, "a screen nobody raised is up");
    CHECK(!q2_loading_step(&l, 1.0 / 30.0), "an unraised screen owns a frame");

    q2_loading_raise(&l);
    CHECK(l.open, "raising it did not open it");

    /* The headless step, which is the console's own 1/30 s. */
    while (q2_loading_step(&l, 1.0 / 30.0) && frames < 1000) {
        shown += 1.0 / 30.0;
        frames++;
    }

    CHECK(frames == 15, "%d frames at 1/30 s, not 15", frames);
    CHECK(shown >= 0.5 - 1e-9, "the screen was up for %.4f s, under half",
          shown);
    CHECK(!l.open, "it never closed");
}

static void test_hold_is_a_floor_at_any_rate(void)
{
    /*
     * The number is in the level clock's 1/300 s units precisely so that it is
     * half a second of REAL time whatever the frame rate is — the same reason
     * `Q2_START_BEAT_UNITS` is. A frame count would be half a second at 30 Hz
     * and a tenth at 144.
     *
     * And it is a FLOOR: the frame that exhausts the hold is still the
     * screen's, so a coarse step overshoots rather than cutting the screen
     * short. Testing the other way round — after the subtract — cost a frame,
     * and at 1/30 s one frame is 7% of the whole screen.
     */
    static const double step[] = {
        1.0 / 144.0, 1.0 / 60.0, 1.0 / 50.0, 1.0 / 30.0, 1.0 / 24.0, 0.1
    };
    unsigned i;

    for (i = 0; i < sizeof(step) / sizeof(step[0]); i++) {
        q2_loading l;
        double     shown = 0.0;
        int        frames = 0;

        armed(&l);
        q2_loading_raise(&l);
        while (q2_loading_step(&l, step[i]) && frames < 100000) {
            shown += step[i];
            frames++;
        }
        CHECK(shown >= 0.5 - 1e-9,
              "at %.5f s a frame the screen lasted %.4f s, under half",
              step[i], shown);
        CHECK(shown < 0.5 + step[i] + 1e-9,
              "at %.5f s a frame the screen lasted %.4f s, over the floor by "
              "more than one frame", step[i], shown);
        CHECK(frames > 0, "at %.5f s a frame the screen never drew", step[i]);
    }
}

static void test_raise_restarts_rather_than_accumulating(void)
{
    q2_loading l;
    int        frames = 0;

    armed(&l);

    /*
     * ONE TRANSITION IS ONE SCREEN. A level change whose arrival lands in
     * another zone loads twice, and both loads come through
     * `client_load_zone`, so both raise this. Adding the two would make that
     * transition linger for a second while every other one took half.
     */
    q2_loading_raise(&l);
    (void)q2_loading_step(&l, 1.0 / 30.0);
    (void)q2_loading_step(&l, 1.0 / 30.0);
    q2_loading_raise(&l);

    while (q2_loading_step(&l, 1.0 / 30.0) && frames < 1000)
        frames++;

    CHECK(frames == 15, "a second raise left %d frames, not a fresh 15",
          frames);
}

static void test_never_raised_without_assets(void)
{
    q2_loading l;

    memset(&l, 0, sizeof(l));       /* `ready` false: no QDUMMY on the disc */
    q2_loading_raise(&l);
    CHECK(!l.open, "a screen with no assets was raised anyway");
    CHECK(!q2_loading_step(&l, 1.0 / 30.0), "...and it owned a frame");
}

/* ------------------------------------------------------------------------- */
/* The turn                                                                   */
/* ------------------------------------------------------------------------- */
static void test_spin(void)
{
    q2_loading l;
    u32 seen[Q2_LOADING_CELLS];
    u32 i, distinct = 0;

    armed(&l);
    q2_loading_raise(&l);

    /* Backwards along the sheet: a fresh screen starts at the LAST cell, which
     * is the broadside one. See q2_loading_cell. */
    CHECK(q2_loading_cell(&l) == Q2_LOADING_CELLS - 1,
          "a fresh screen starts at cell %u, not %u", q2_loading_cell(&l),
          (u32)Q2_LOADING_CELLS - 1);

    /*
     * One cell every Q2_LOADING_CELL_UNITS of the level clock. At the headless
     * 1/30 s step a frame is 10 of those units, so the cell index is the frame
     * count times 10 over the pitch.
     */
    for (i = 1; i <= 8; i++) {
        u32 want = Q2_LOADING_CELLS - 1 -
                   (u32)(i * 10) / Q2_LOADING_CELL_UNITS;

        (void)q2_loading_step(&l, 1.0 / 30.0);
        CHECK(q2_loading_cell(&l) == want,
              "after %u frames the strip is on cell %u, not %u", i,
              q2_loading_cell(&l), want);
    }

    /* Every cell of the strip is reached, and none outside it: an index past
     * the fifteenth is the word RETRY, which is in the sixteenth slot. */
    memset(seen, 0, sizeof(seen));
    armed(&l);
    q2_loading_show(&l, Q2_LOADING_PAGE_LOADING);
    for (i = 0; i < 400; i++) {
        u32 c = q2_loading_cell(&l);

        CHECK(c < Q2_LOADING_CELLS, "cell %u is off the end of the strip", c);
        if (c < Q2_LOADING_CELLS && !seen[c]) {
            seen[c] = 1;
            distinct++;
        }
        (void)q2_loading_step(&l, 1.0 / 60.0);
    }
    CHECK(distinct == Q2_LOADING_CELLS,
          "%u of the strip's %u cells were reached", distinct,
          (u32)Q2_LOADING_CELLS);
}

/* ------------------------------------------------------------------------- */
/* The corner                                                                 */
/* ------------------------------------------------------------------------- */
static void test_logo_lands_in_the_top_right(void)
{
    /*
     * The quad is 32 x 30 texels drawn 1:1 in the console's own 512 x 248
     * pixels, so this is a rectangle check and not a projection: it has to be
     * in the top right, inside the screen, and clear of the word.
     */
    int x0 = Q2_LOADING_X;
    int y0 = Q2_LOADING_Y;
    int x1 = x0 + Q2_LOADING_CELL_W;
    int y1 = y0 + Q2_LOADING_CELL_H;

    CHECK(x0 > Q2_MENU_SCREEN_W / 2, "the logo is not in the right half");
    CHECK(y1 < Q2_MENU_SCREEN_H / 2, "the logo is not in the top half");
    CHECK(x0 >= 0 && x1 <= Q2_MENU_SCREEN_W,
          "the logo spans %d..%d across a %d-wide screen", x0, x1,
          Q2_MENU_SCREEN_W);
    CHECK(y0 >= 0 && y1 <= Q2_MENU_SCREEN_H,
          "the logo spans %d..%d down a %d-tall screen", y0, y1,
          Q2_MENU_SCREEN_H);

    /* Clear of both rows either screen can carry: LOADING is centred at y=124
     * and STARTING at y=111, each in an 11-pixel cell. */
    CHECK(y1 < 111 - 6, "the logo reaches y %d and STARTING starts at 105", y1);
}

/* ------------------------------------------------------------------------- */
/* The strip's cells                                                          */
/* ------------------------------------------------------------------------- */
static void test_cells_stay_on_the_sheet(void)
{
    u32 i;

    /*
     * A 4bpp texture page is 256 texels square and a POLY_FT4's uv are BYTES,
     * so a cell that runs past 255 does not clip, it WRAPS — and the strip's
     * last column ends at exactly 256. Checking the far corner of every cell is
     * what says the grid and the sheet agree.
     */
    for (i = 0; i < Q2_LOADING_CELLS; i++) {
        int u = (int)(i % Q2_LOADING_CELL_COLS) * Q2_LOADING_CELL_W;
        int v = Q2_LOADING_CELL_V +
                (int)(i / Q2_LOADING_CELL_COLS) * Q2_LOADING_CELL_H;

        CHECK(u + Q2_LOADING_CELL_W <= 256,
              "cell %u runs to u %d", i, u + Q2_LOADING_CELL_W);
        CHECK(v + Q2_LOADING_CELL_H <= 256,
              "cell %u runs to v %d", i, v + Q2_LOADING_CELL_H);
    }

    /*
     * And clear of the letterforms, which are what the rest of the sheet is:
     * the 32-pixel face runs to row 109 and the 16-pixel face to 142
     * (menufont.h), so the strip starting at 144 is the first free row.
     */
    CHECK(Q2_LOADING_CELL_V > 142,
          "the strip starts at row %d, inside the 16-pixel face",
          Q2_LOADING_CELL_V);
    CHECK(Q2_LOADING_CELL_V + 3 * Q2_LOADING_CELL_H <= 213,
          "the strip reaches row %d, into the panel art at 213",
          Q2_LOADING_CELL_V + 3 * Q2_LOADING_CELL_H);
}

/* ------------------------------------------------------------------------- */
/* The two pages                                                              */
/* ------------------------------------------------------------------------- */
static void test_starting_page(void)
{
    const q2_menu_page *p = q2_menu_page_find(Q2_PAGE_STARTING);
    q2_loading l;

    CHECK(p != NULL, "no page for Q2_PAGE_STARTING");
    if (p) {
        CHECK(p->count == 2, "the page holds %u records, not 2", p->count);
        CHECK(p->first == p->count, "the page is navigable");
        CHECK(strcmp(p->items[0].label, "STARTING") == 0 &&
              strcmp(p->items[1].label, "GAME") == 0,
              "the rows read \"%s\" / \"%s\"", p->items[0].label,
              p->items[1].label);
        CHECK(p->items[0].x == 256 && p->items[0].y == 111 &&
              p->items[1].x == 256 && p->items[1].y == 137,
              "the rows are at (%d,%d) and (%d,%d)",
              (int)p->items[0].x, (int)p->items[0].y,
              (int)p->items[1].x, (int)p->items[1].y);
    }

    /*
     * A SHOWN screen does not own the frame, which is the whole difference
     * between the two ways of raising one: the opening reel's beat is the clock
     * for this page, so `q2_loading_step` must turn the logo and then hand the
     * frame back rather than swallowing it.
     */
    armed(&l);
    q2_loading_show(&l, Q2_LOADING_PAGE_STARTING);
    CHECK(l.open, "showing it did not open it");
    CHECK(!q2_loading_step(&l, 1.0 / 30.0), "a shown screen owns the frame");
    CHECK(l.open, "...and stepping it took it down");
    CHECK(l.menu.page == p, "the shown page is not STARTING");

    q2_loading_hide(&l);
    CHECK(!l.open, "hiding it left it up");
}

int main(void)
{
    test_page();
    test_hold_is_half_a_second();
    test_hold_is_a_floor_at_any_rate();
    test_raise_restarts_rather_than_accumulating();
    test_never_raised_without_assets();
    test_spin();
    test_logo_lands_in_the_top_right();
    test_cells_stay_on_the_sheet();
    test_starting_page();

    if (g_fail) {
        printf("\n%d loading-screen check%s failed\n", g_fail,
               g_fail == 1 ? "" : "s");
        return 1;
    }
    printf("loading: all checks passed\n");
    return 0;
}
