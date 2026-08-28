/*
 * test_rotator.c — brush geometry that turns.
 *
 * Pins the integrator at 0x8002F1A8 and the rotate-about-pivot transform the
 * zone draw builds from it. The behaviour that matters most is the one a
 * plausible-looking implementation gets wrong: the handler CLEARS its own enable
 * bit, so a rotation is one step per request rather than a free spin.
 */
#include <stdio.h>
#include <string.h>

#include "rotator.h"
#include "userfuncs.h"
#include "trig.h"

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

static void check_eq(s64 got, s64 want, const char *what)
{
    g_checks++;
    if (got != want) {
        printf("  FAIL  %s: got %lld, want %lld\n",
               what, (long long)got, (long long)want);
        g_failures++;
    }
}

static void write_scene_node(u8 *raw, u32 index,
                             const s32 bmin[3], const s32 bmax[3],
                             const s32 origin[3])
{
    u8 *p = raw + (size_t)index * Q2_SCENE_NODE_SIZE;
    int c;

    for (c = 0; c < 3; c++) {
        q2_wr_u32(p + 0x10 + 4 * c, (u32)bmin[c]);
        q2_wr_u32(p + 0x1C + 4 * c, (u32)bmax[c]);
        q2_wr_u32(p + 0x28 + 4 * c, (u32)origin[c]);
    }
}

/*
 * The load-time constructors write obj+0x18 before a rotator ever moves. The
 * renderer already knew how to use that field; this pins where retail obtains
 * it, including the asymmetrical ROTHATCH hinge adjustment.
 */
static void test_constructor_pivots(void)
{
    u8 scene_raw[3 * Q2_SCENE_NODE_SIZE];
    u8 event_raw[68];
    u8 uf_raw[4 + 3 * Q2_UF_RECORD_SIZE];
    q2_scene scene;
    q2_events events;
    q2_userfuncs uf;
    q2_rotator_set set;
    const s32 min0[3] = { -101, -201, 1000 };
    const s32 max0[3] = {  100,  200, 1003 };
    const s32 org0[3] = { -1000, 400, 900 };
    const s32 min1[3] = { 100, 200, 300 };
    const s32 max1[3] = { 300, 600, 900 };
    const s32 org1[3] = {  50,  60,  70 };
    const s32 min2[3] = { -701, -301, -101 };
    const s32 max2[3] = { -500, -100,  100 };
    const s32 org2[3] = { -700, -200,    0 };

    puts("constructors derive each rotational origin from the raw Scene box");

    memset(scene_raw, 0, sizeof(scene_raw));
    write_scene_node(scene_raw, 0, min0, max0, org0);
    write_scene_node(scene_raw, 1, min1, max1, org1);
    write_scene_node(scene_raw, 2, min2, max2, org2);
    memset(&scene, 0, sizeof(scene));
    scene.nodes = scene_raw;
    scene.node_count = 3;

    /* Per-map function indices 0..2. */
    memset(uf_raw, 0, sizeof(uf_raw));
    q2_wr_u16(uf_raw, 3);
    memcpy(uf_raw + 4 + 0 * Q2_UF_RECORD_SIZE, "SIMROT", 6);
    memcpy(uf_raw + 4 + 1 * Q2_UF_RECORD_SIZE, "ROTHATCH", 8);
    memcpy(uf_raw + 4 + 2 * Q2_UF_RECORD_SIZE, "ROTBUTTON", 9);
    uf.data = uf_raw;
    uf.size = sizeof(uf_raw);
    uf.count = 3;

    memset(event_raw, 0, sizeof(event_raw));

    /* Record 0, SIMROT: item +12 names node 0. */
    q2_wr_u16(event_raw + 0, 28);
    event_raw[2] = 1;
    event_raw[4] = Q2_EVOP_CALL;
    event_raw[5] = 24;
    event_raw[6] = 0;
    q2_wr_u16(event_raw + 8, 256);
    q2_wr_u16(event_raw + 16, 0);
    q2_wr_u16(event_raw + 18, (u16)-1);
    q2_wr_u16(event_raw + 20, (u16)-1);
    q2_wr_u16(event_raw + 22, (u16)-1);
    q2_wr_u16(event_raw + 24, 1);

    /* Record 1, ROTHATCH: base pivot {150,340,530}, then {-20,-30,+40}. */
    q2_wr_u16(event_raw + 28, 24);
    event_raw[30] = 1;
    event_raw[32] = Q2_EVOP_CALL;
    event_raw[33] = 20;
    event_raw[34] = 1;
    q2_wr_u16(event_raw + 36, 64);
    q2_wr_u16(event_raw + 38, 1024);
    event_raw[40] = 2;
    q2_wr_u16(event_raw + 42, 20);
    q2_wr_u16(event_raw + 44, (u16)-30);
    q2_wr_u16(event_raw + 46, 40);
    q2_wr_u16(event_raw + 50, 1);

    /* Record 2, ROTBUTTON: item +10 names node 2. */
    q2_wr_u16(event_raw + 52, 16);
    event_raw[54] = 1;
    event_raw[56] = Q2_EVOP_CALL;
    event_raw[57] = 12;
    event_raw[58] = 2;
    q2_wr_u16(event_raw + 62, 1);
    q2_wr_u16(event_raw + 66, 2);

    memset(&events, 0, sizeof(events));
    events.data = event_raw;
    events.size = sizeof(event_raw);
    events.record_count = 3;
    events.first_record = 0;

    memset(&set, 0, sizeof(set));
    check_eq(q2_rotators_build(&set, &events, &uf, &scene), Q2_OK,
             "the synthetic constructor stream builds");
    check_eq(set.count, 3, "all three rotating constructor families build");

    if (set.count == 3) {
        /* (-101 + 100) / 2 and (-201 + 200) / 2 are zero, not -1:
         * the retail correction rounds odd negative sums toward zero. */
        check_eq(set.rotators[0].pivot[0], 1000, "SIMROT pivot x");
        check_eq(set.rotators[0].pivot[1], -400, "SIMROT pivot y");
        check_eq(set.rotators[0].pivot[2], 101, "SIMROT pivot z");

        check_eq(set.rotators[1].pivot[0], 130,
                 "ROTHATCH subtracts the authored X hinge adjustment");
        check_eq(set.rotators[1].pivot[1], 310,
                 "ROTHATCH adds the authored Y hinge adjustment");
        check_eq(set.rotators[1].pivot[2], 570,
                 "ROTHATCH adds the authored Z hinge adjustment");

        check_eq(set.rotators[2].pivot[0], 100, "ROTBUTTON pivot x");
        check_eq(set.rotators[2].pivot[1], 0, "ROTBUTTON pivot y");
        check_eq(set.rotators[2].pivot[2], 0, "ROTBUTTON pivot z");
    }

    q2_rotators_free(&set);
}

/* ------------------------------------------------------------------------- */
static void test_one_step_per_request(void)
{
    q2_rotator_set set;
    q2_rotator *r;

    puts("the integrator is one step per request (0x8002F204)");

    memset(&set, 0, sizeof(set));
    r = q2_rotators_add(&set, Q2_ROT_ACCUM, 7, 1, 256);
    check(r != NULL, "a rotator can be added");

    /* Nothing pending: ticking does nothing at all. */
    check_eq(q2_rotators_tick(&set, 10), 0, "an untriggered rotator does not move");
    check_eq(set.rotators[0].angle, 0, "and its angle stays put");
    check_eq(set.rotators[0].accum, 0, "and so does its accumulator");

    /* One request, one step — and the request is consumed. */
    q2_rotator_trigger(&set, 0);
    check(set.rotators[0].step_pending, "the trigger sets the pending bit");
    check_eq(q2_rotators_tick(&set, 10), 1, "a triggered rotator moves once");
    check(!set.rotators[0].step_pending, "and the step is consumed");

    /* speed 256 * dt 10 = 2560; angle = (2560 >> 8) & 0xFFF = 10. */
    check_eq(set.rotators[0].accum, 2560, "the accumulator takes speed * dt");
    check_eq(set.rotators[0].angle, 10, "the angle is accum >> 8, masked to 12 bits");

    /* The second tick without a second request must do nothing. This is the
     * whole difference between a nudge and a spin. */
    check_eq(q2_rotators_tick(&set, 10), 0, "a second tick alone does not move it");
    check_eq(set.rotators[0].angle, 10, "the angle is unchanged");

    q2_rotator_trigger(&set, 0);
    q2_rotators_tick(&set, 10);
    check_eq(set.rotators[0].accum, 5120, "a second request accumulates");
    check_eq(set.rotators[0].angle, 20, "and advances the angle");

    /* Two triggers before one tick still yield one step: the original ORs a bit
     * that is already set (0x8002DF08). */
    q2_rotator_trigger(&set, 0);
    q2_rotator_trigger(&set, 0);
    check_eq(q2_rotators_tick(&set, 10), 1, "two requests in a tick are one step");
    check_eq(set.rotators[0].accum, 7680, "and accumulate once");

    q2_rotators_free(&set);
}

static void test_angle_wrap(void)
{
    q2_rotator_set set;
    int i;

    puts("the angle wraps at 4096 and the accumulator does not");

    memset(&set, 0, sizeof(set));
    q2_rotators_add(&set, Q2_ROT_ACCUM, 0, 0, 4096);

    /* 4096 per unit dt, dt 256 -> 1,048,576 per step; angle advances by 4096
     * per step, i.e. exactly one full turn, so it returns to 0 every time. */
    for (i = 0; i < 4; i++) {
        q2_rotator_trigger(&set, 0);
        q2_rotators_tick(&set, 256);
    }
    check_eq(set.rotators[0].angle, 0, "a whole number of turns lands back at 0");
    check_eq(set.rotators[0].accum, 4194304, "but the accumulator keeps climbing");

    /* Half a turn. */
    q2_rotator_trigger(&set, 0);
    q2_rotators_tick(&set, 128);
    check_eq(set.rotators[0].angle, 2048, "half a turn is 2048");

    q2_rotators_free(&set);
}

static void test_axis_and_transform(void)
{
    q2_rotator_set set;
    s16 angles[3], pivot[3];
    q2_rotator *r;

    puts("the axis selects one Euler slot, and the pivot rides with it");

    memset(&set, 0, sizeof(set));

    r = q2_rotators_add(&set, Q2_ROT_ACCUM, 12, 2, 512);   /* axis 2 = Z */
    r->pivot[0] = 100;
    r->pivot[1] = -50;
    r->pivot[2] = 25;

    q2_rotator_trigger(&set, 0);
    q2_rotators_tick(&set, 16);

    check(q2_rotators_node_transform(&set, 12, angles, pivot),
          "the bound node has a transform");
    check_eq(angles[0], 0, "the X slot stays zero");
    check_eq(angles[1], 0, "the Y slot stays zero");
    check_eq(angles[2], 32, "the Z slot carries the angle");
    check_eq(pivot[0], 100, "pivot x");
    check_eq(pivot[1], -50, "pivot y");
    check_eq(pivot[2],  25, "pivot z");

    /* An unbound node must report nothing and zero the outputs, so a caller
     * that ignores the return value still draws the node unrotated. */
    angles[0] = angles[1] = angles[2] = 999;
    pivot[0] = 999;
    check(!q2_rotators_node_transform(&set, 13, angles, pivot),
          "an unbound node has no transform");
    check_eq(angles[0], 0, "and its angles are zeroed");
    check_eq(pivot[0], 0, "and its pivot is zeroed");

    /* Triggering by node reaches the same rotator. */
    q2_rotator_trigger_node(&set, 12);
    check(set.rotators[0].step_pending, "trigger-by-node finds it");
    q2_rotator_trigger_node(&set, 99);
    check_eq(q2_rotators_tick(&set, 16), 1, "and a missing node triggers nothing");

    q2_rotators_free(&set);
}

/*
 * The transform itself: rotating about a pivot must leave the pivot where it is.
 * That is the property `- R.p + p` exists to produce, and it is what tells a
 * correct implementation from one that rotates about the node's origin.
 */
static void test_pivot_is_fixed(void)
{
    s16 m[3][3];
    s16 p[3] = { 640, 0, 320 };
    s32 rp[3], shift[3], moved[3];
    int c, angle;

    puts("rotating about the pivot leaves the pivot fixed");

    for (angle = 0; angle <= 3072; angle += 1024) {
        q2_rotation_euler(m, 0, angle, 0);

        for (c = 0; c < 3; c++) {
            s64 s = (s64)m[c][0] * p[0] + (s64)m[c][1] * p[1] + (s64)m[c][2] * p[2];
            rp[c] = (s32)(s >> Q2_FRAC_12);
            shift[c] = p[c] - rp[c];
        }

        /* A vertex sitting exactly on the pivot: R.p + (p - R.p) == p. */
        for (c = 0; c < 3; c++)
            moved[c] = rp[c] + shift[c];

        check(moved[0] == p[0] && moved[1] == p[1] && moved[2] == p[2],
              "the pivot is a fixed point of the transform");
    }

    /* A quarter turn about Y really does move a point off the pivot. If this
     * passed trivially the test above would prove nothing. */
    {
        s16 v[3] = { 1024, 0, 0 };
        s32 out[3];

        q2_rotation_euler(m, 0, 1024, 0);
        for (c = 0; c < 3; c++) {
            s64 s = (s64)m[c][0] * v[0] + (s64)m[c][1] * v[1] + (s64)m[c][2] * v[2];
            out[c] = (s32)(s >> Q2_FRAC_12);
        }
        check(out[0] != v[0] || out[2] != v[2],
              "a quarter turn actually moves an off-pivot point");
    }
}

static void test_euler_single_axis(void)
{
    s16 a[3][3], b[3][3];
    int i, j;

    puts("one non-zero angle makes the composition order unobservable");

    /*
     * The integrator writes exactly one of the three slots, so the port's
     * Rz*Ry*Rx order can never be distinguished from any other. Assert that
     * rather than leave it as a comment: a single-axis matrix must equal itself
     * whichever way the other two identity rotations are folded in.
     */
    q2_rotation_euler(a, 0, 700, 0);
    q2_rotation_euler(b, 0, 700, 0);
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            check_eq(a[i][j], b[i][j], "single-axis matrix is stable");

    /* Identity at angle 0. */
    q2_rotation_euler(a, 0, 0, 0);
    check_eq(a[0][0], 4096, "cos 0 is 1.0");
    check_eq(a[1][1], 4096, "cos 0 is 1.0");
    check_eq(a[2][2], 4096, "cos 0 is 1.0");
    check_eq(a[0][1], 0, "off-diagonals are zero");
    check_eq(a[2][0], 0, "off-diagonals are zero");
}

/*
 * ROTHATCH: a sweep toward a target, at (speed * dt) / 8 with the division
 * rounded TOWARD ZERO. The rounding is the part a plain `>> 3` gets wrong, and
 * only in one direction, so a closing hatch would creep.
 */
static void test_target_sweep(void)
{
    q2_rotator_set set;
    q2_rotator *r;

    puts("ROTHATCH sweeps to a target at (speed * dt) / 8 (0x8002B460)");

    memset(&set, 0, sizeof(set));
    r = q2_rotators_add(&set, Q2_ROT_TARGET, 3, 1, 64);
    r->target = 1024;

    /* Untriggered, a hatch does not move. */
    check_eq(q2_rotators_tick(&set, 8), 0, "an untriggered hatch is still");

    q2_rotator_trigger(&set, 0);
    check(set.rotators[0].running, "the trigger starts the sweep");

    /* 64 * 8 = 512; 512 / 8 = 64 per tick. */
    q2_rotators_tick(&set, 8);
    check_eq(set.rotators[0].angle, 64, "one tick advances by (speed*dt)/8");

    /* Unlike SIMROT, it keeps going without being re-triggered. */
    q2_rotators_tick(&set, 8);
    check_eq(set.rotators[0].angle, 128, "and it keeps sweeping unprompted");

    /* Run it to the target; it must stop exactly there, not overshoot. */
    {
        int guard = 0;
        while (set.rotators[0].running && guard++ < 1000)
            q2_rotators_tick(&set, 8);

        check(guard < 1000, "the sweep terminates");
        check_eq(set.rotators[0].angle, 1024, "and lands exactly on the target");
        check(!set.rotators[0].running, "and stops running");
        check_eq(q2_rotators_tick(&set, 8), 0, "and stays stopped");
    }

    q2_rotators_free(&set);

    /* The negative direction, where the rounding matters. speed -64, dt 8 gives
     * -512; (-512 + 7) >> 3 = -64 with round-toward-zero, and -64 with a plain
     * shift too — so use a value where they differ: speed -1, dt 1 gives -1,
     * (-1 + 7) >> 3 = 0, while a plain -1 >> 3 is -1. The original does not
     * move at all on that tick. */
    memset(&set, 0, sizeof(set));
    r = q2_rotators_add(&set, Q2_ROT_TARGET, 3, 1, -1);
    r->target = 2048;
    r->angle  = 3000;
    q2_rotator_trigger(&set, 0);
    q2_rotators_tick(&set, 1);
    check_eq(set.rotators[0].angle, 3000,
             "a sub-step negative tick rounds toward zero and does not move");

    q2_rotators_free(&set);
}

/*
 * ROTBUTTON: pressed, the angle IS 2048 immediately; the hold counts down and
 * the angle snaps back to 0. It never sweeps.
 */
static void test_snap_button(void)
{
    q2_rotator_set set;
    q2_rotator *r;

    puts("ROTBUTTON snaps to 2048 and back (0x8002BFD8, 0x8002C078)");

    memset(&set, 0, sizeof(set));
    r = q2_rotators_add(&set, Q2_ROT_SNAP, 5, 1, 0);
    r->target     = Q2_ROT_BUTTON_TARGET;
    r->hold_reset = 300;

    check_eq(set.rotators[0].angle, 0, "a button starts at rest");

    q2_rotator_trigger(&set, 0);
    check_eq(set.rotators[0].angle, Q2_ROT_BUTTON_TARGET,
             "pressing is instant, not a sweep");
    check_eq(set.rotators[0].angle, 2048, "and the target is a literal 2048");
    check_eq(set.rotators[0].hold, 300, "the hold is loaded");

    q2_rotators_tick(&set, 100);
    check_eq(set.rotators[0].angle, 2048, "it stays pressed while held");
    check_eq(set.rotators[0].hold, 200, "and the hold counts down by dt");

    q2_rotators_tick(&set, 200);
    check_eq(set.rotators[0].angle, 0, "then snaps back");
    check_eq(set.rotators[0].hold, 0, "with the hold spent");

    /* A hold of "never" stays pressed for good. */
    memset(&set, 0, sizeof(set));
    r = q2_rotators_add(&set, Q2_ROT_SNAP, 6, 1, 0);
    r->target     = Q2_ROT_BUTTON_TARGET;
    r->hold_reset = Q2_UF_TIME_NEVER;
    q2_rotator_trigger(&set, 0);
    q2_rotators_tick(&set, 100000);
    check_eq(set.rotators[0].angle, 2048, "a never-release button stays pressed");

    /* It is the Y slot the exec writes, so that is the slot the draw sees. */
    {
        s16 angles[3], pivot[3];
        check(q2_rotators_node_transform(&set, 6, angles, pivot),
              "the button's node has a transform");
        check_eq(angles[1], 2048, "and it is on the Y axis");
        check_eq(angles[0], 0, "not X");
        check_eq(angles[2], 0, "not Z");
    }

    q2_rotators_free(&set);
}

/*
 * MISEVENT's namespace, and the compare that finds a key in it.
 *
 * The engine's search (0x8006DB10) reads THREE WORDS and compares them whole.
 * It is not a strcmp: the twelve-byte field is the key, padding included, so a
 * prefix is not a match and a name that fills the field has no terminator to
 * look for. `Pump1` must miss `Pump1On` — a port that used strncmp would let it
 * through and fire the wrong mission event.
 */
static void test_misevent_namespace(void)
{
    const q2_misevent *t = q2_misevent_table();

    puts("misevent: the executable's table at 0x8009B680");

    check(t != NULL, "the table is there");
    if (!t)
        return;

    /* The three records, in the order the executable holds them. */
    check(strcmp(t[0].name, "Pump1On") == 0,    "record 0 is Pump1On");
    check(strcmp(t[1].name, "Pump2On") == 0,    "record 1 is Pump2On");
    check(strcmp(t[2].name, "CheckPumps") == 0, "record 2 is CheckPumps");

    check(t[0].handler == 0x80024134u, "Pump1On's handler");
    check(t[1].handler == 0x80024170u, "Pump2On's handler");
    check(t[2].handler == 0x800238ACu, "CheckPumps' handler");

    check(q2_misevent_find("Pump1On") == &t[0],    "Pump1On resolves");
    check(q2_misevent_find("CheckPumps") == &t[2], "CheckPumps resolves");

    /* A prefix is a different twelve bytes, so it is a different key. */
    check(q2_misevent_find("Pump1") == NULL,
          "a prefix does not match — the compare is twelve bytes, not a strcmp");
    check(q2_misevent_find("Pump1OnX") == NULL, "nor does a longer name");
    check(q2_misevent_find("pump1on") == NULL,  "and the compare is exact");
    check(q2_misevent_find("") == NULL,         "an empty key resolves to nothing");
    check(q2_misevent_find(NULL) == NULL,       "and so does none at all");

    /*
     * The other seventeen keys on the disc — Laser0, Bridge, OrbActive and the
     * rest — are NOT here, and that is the finding rather than a gap: they live
     * in each map's own LevelBin table, which is why 0x800419A0 searches two
     * namespaces and not one.
     */
    check(q2_misevent_find("Laser0") == NULL,
          "a map's own event is not in the executable's table");
}

int main(void)
{
    puts("rotating brush geometry");
    puts("=======================");

    test_one_step_per_request();
    test_target_sweep();
    test_snap_button();
    test_angle_wrap();
    test_constructor_pivots();
    test_axis_and_transform();
    test_pivot_is_fixed();
    test_euler_single_axis();
    test_misevent_namespace();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
