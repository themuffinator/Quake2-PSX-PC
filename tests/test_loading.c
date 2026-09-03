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
    s32 first;

    armed(&l);
    q2_loading_raise(&l);

    /*
     * `yaw -= 4 * dt` with dt in 1/300 s units — module+0x9D24, the front end's
     * own rate for the same model, and a third faster than a pickup's
     * Q2_ITEM_SPIN_RATE of 3. One 1/30 s frame is 10 units of clock, so 40 of
     * the circle's 4096.
     */
    (void)q2_loading_step(&l, 1.0 / 30.0);
    first = l.yaw;
    CHECK(first == (s32)(s16)(0 - Q2_LB_SCENE_SPIN * 10),
          "one frame turned it to %d, not %d", (int)first,
          (int)(s16)(0 - Q2_LB_SCENE_SPIN * 10));

    /* And it keeps turning the same way. A full 4096 takes 3.4 s, which is
     * longer than the screen is up — so the logo never repeats a pose. */
    (void)q2_loading_step(&l, 1.0 / 30.0);
    CHECK(l.yaw == (s32)(s16)(first - Q2_LB_SCENE_SPIN * 10),
          "the second frame turned it to %d", (int)l.yaw);
    CHECK(l.yaw != 0, "two frames left the logo where it started");
}

/* ------------------------------------------------------------------------- */
/* The corner                                                                 */
/* ------------------------------------------------------------------------- */
static void test_logo_lands_in_the_top_right(void)
{
    /*
     * Project the world position back the way the frame does and check it comes
     * out at the pixel offsets the constants ask for.
     *
     * x carries the 3/2 of `q2_rotation_view_anamorphic`'s row 0 and y does
     * not, which is the whole reason this is worth a test: the same divide on
     * both axes put 200 pixels of x at 300 and the logo 44 pixels off the right
     * edge of a 512-wide screen, where it drew nothing and looked like a
     * missing model.
     */
    int sx = Q2_LOADING_WORLD_X * 3 / 2 * Q2_LOADING_PROJ / Q2_LOADING_DIST;
    int sy = Q2_LOADING_WORLD_Y * Q2_LOADING_PROJ / Q2_LOADING_DIST;

    CHECK(sx >= Q2_LOADING_OFS_X - 2 && sx <= Q2_LOADING_OFS_X + 2,
          "x projects to %d, not %d", sx, Q2_LOADING_OFS_X);
    CHECK(sy >= Q2_LOADING_OFS_Y - 2 && sy <= Q2_LOADING_OFS_Y + 2,
          "y projects to %d, not %d", sy, Q2_LOADING_OFS_Y);

    /* Top RIGHT, and inside the screen: the logo is a quarter scale, so its
     * posed half-extents reach 182 world units across and 250 down. */
    {
        int half_w = 727 * Q2_LOADING_SCALE / Q2_ONE_12 * 3 / 2 *
                     Q2_LOADING_PROJ / Q2_LOADING_DIST;
        int half_h = 1081 * Q2_LOADING_SCALE / Q2_ONE_12 *
                     Q2_LOADING_PROJ / Q2_LOADING_DIST;
        int cx = Q2_MENU_SCREEN_W / 2 + sx;
        int cy = Q2_MENU_SCREEN_H / 2 + sy;

        CHECK(sx > 0 && sy < 0, "the logo is not in the top right");
        CHECK(cx - half_w >= 0 && cx + half_w < Q2_MENU_SCREEN_W,
              "the logo spans %d..%d across a %d-wide screen",
              cx - half_w, cx + half_w, Q2_MENU_SCREEN_W);
        CHECK(cy - half_h >= 0 && cy + half_h < Q2_MENU_SCREEN_H,
              "the logo spans %d..%d down a %d-tall screen",
              cy - half_h, cy + half_h, Q2_MENU_SCREEN_H);

        /* And clear of the word it shares the screen with, which is centred at
         * y = 124 in an 11-pixel cell. */
        CHECK(cy + half_h < 124 - 8,
              "the logo reaches y %d and LOADING starts at 116",
              cy + half_h);
    }
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

    if (g_fail) {
        printf("\n%d loading-screen check%s failed\n", g_fail,
               g_fail == 1 ? "" : "s");
        return 1;
    }
    printf("loading: all checks passed\n");
    return 0;
}
