/*
 * test_gte.c — conformance tests for the fidelity core.
 *
 * Scope, stated honestly: these tests check the properties that can be
 * established without a PlayStation. They verify the reciprocal table against
 * its defining formula, the divide's documented clamping and flag behaviour,
 * the saturation limits, and the ordering table's draw order.
 *
 * What they do NOT do is compare against values captured from real hardware.
 * That is the only test that can prove bit-exactness, and until it exists the
 * claim in docs/FIDELITY.md is "built to be exact" rather than "measured to be
 * exact". The gap is deliberate and recorded rather than papered over.
 *
 * A vector capture would slot straight into gte_known_vectors() below.
 */
#include <stdio.h>
#include <string.h>

#include "gpu.h"
#include "gte.h"
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

static void check_eq_i(s64 got, s64 want, const char *what)
{
    g_checks++;
    if (got != want) {
        printf("  FAIL  %s: got %lld, want %lld\n",
               what, (long long)got, (long long)want);
        g_failures++;
    }
}

/* ------------------------------------------------------------------------- */
/* The perspective divide                                                     */
/* ------------------------------------------------------------------------- */
static void test_divide(void)
{
    gte_state g;
    u32 h;

    printf("gte_divide\n");
    gte_init(&g);

    /* A zero divisor overflows and clamps rather than trapping. Games relied on
     * the clamp, so this is behaviour, not an error path. */
    g.flag = 0;
    check_eq_i(gte_divide(&g, 320, 0), 0x1FFFF, "sz3 == 0 clamps");
    check(g.flag & GTE_FLAG_DIV_OVERFLOW, "sz3 == 0 raises the overflow flag");

    /* h >= sz3*2 is the documented overflow condition. */
    g.flag = 0;
    check_eq_i(gte_divide(&g, 200, 100), 0x1FFFF, "h == sz3*2 clamps");
    check(g.flag & GTE_FLAG_DIV_OVERFLOW, "h == sz3*2 raises the overflow flag");

    g.flag = 0;
    check_eq_i(gte_divide(&g, 201, 100), 0x1FFFF, "h > sz3*2 clamps");

    /* Just inside the boundary must NOT overflow. */
    g.flag = 0;
    gte_divide(&g, 199, 100);
    check(!(g.flag & GTE_FLAG_DIV_OVERFLOW), "h just under sz3*2 does not overflow");

    /*
     * The result approximates (h << 16) / sz3. The hardware's Newton-Raphson
     * reciprocal is deliberately imprecise, so this asserts closeness, not
     * equality -- being exactly right here would mean the implementation is
     * wrong in an interesting way.
     */
    {
        u32 worst = 0;
        int sz;

        for (sz = 1; sz <= 4096; sz++) {
            u32 exact, got, diff;

            h = (u32)sz;              /* keep h < sz3*2 so it never overflows */
            g.flag = 0;
            got = gte_divide(&g, (u16)h, (u16)sz);

            exact = (u32)(((u64)h << 16) / (u32)sz);
            diff  = got > exact ? got - exact : exact - got;
            if (diff > worst)
                worst = diff;
        }
        printf("  worst deviation from an exact divide over 4096 cases: %u\n", worst);
        check(worst <= 2, "reciprocal stays within 2 units of an exact divide");
    }

    /* Monotonicity: for fixed sz3, a larger h must never give a smaller result. */
    {
        u32 prev = 0;
        bool monotonic = true;
        int i;

        for (i = 1; i < 1000; i++) {
            u32 got;
            g.flag = 0;
            got = gte_divide(&g, (u16)i, 1000);
            if (got < prev)
                monotonic = false;
            prev = got;
        }
        check(monotonic, "the divide is monotonic in h");
    }
}

/* ------------------------------------------------------------------------- */
/* Saturation                                                                 */
/* ------------------------------------------------------------------------- */
static void test_saturation(void)
{
    gte_state g;

    printf("saturation\n");

    /* Screen coordinates clamp to the hardware's 11-bit signed range and raise
     * the corresponding flags. Geometry far off-screen must not wrap around to
     * the other side, which is what an unclamped s16 would do. */
    gte_init(&g);
    gte_set_projection(&g, 320, 160, 120);

    /* Push a vertex far to one side with a tiny Z so the projection explodes. */
    g.v[0].x = 32000;
    g.v[0].y = 0;
    g.v[0].z = 1;
    gte_set_translation(&g, 0, 0, 200);
    gte_rtps(&g, false);

    check(g.sxy[2].x >= -1024 && g.sxy[2].x <= 1023,
          "projected x stays inside the 11-bit range");
    check(g.sxy[2].y >= -1024 && g.sxy[2].y <= 1023,
          "projected y stays inside the 11-bit range");

    /* RTPS uses MAC0 as the full-precision screen-coordinate intermediate.
     * Its two overflow directions have distinct FLAG bits, just like NCLIP
     * and AVSZ below. A negative overflow used to raise MAC0_POS here. */
    gte_init(&g);
    gte_set_projection(&g, 320, 0, 0);
    g.v[0].x = 32767;
    g.v[0].z = 1;
    gte_rtps(&g, false);
    check(g.flag & GTE_FLAG_MAC0_POS,
          "positive RTPS screen intermediate raises MAC0_POS");
    check(!(g.flag & GTE_FLAG_MAC0_NEG),
          "positive RTPS screen intermediate does not raise MAC0_NEG");

    gte_init(&g);
    gte_set_projection(&g, 320, 0, 0);
    g.v[0].x = -32768;
    g.v[0].z = 1;
    gte_rtps(&g, false);
    check(g.flag & GTE_FLAG_MAC0_NEG,
          "negative RTPS screen intermediate raises MAC0_NEG");
    check(!(g.flag & GTE_FLAG_MAC0_POS),
          "negative RTPS screen intermediate does not raise MAC0_POS");
}

/* ------------------------------------------------------------------------- */
/* Vertex snapping — the wobble itself                                        */
/* ------------------------------------------------------------------------- */
static void test_vertex_snapping(void)
{
    gte_state g;
    int i;
    int distinct = 0;
    s16 last = -32768;

    printf("vertex snapping\n");

    gte_init(&g);
    gte_set_projection(&g, 320, 160, 120);
    gte_set_translation(&g, 0, 0, 1000);

    /*
     * Slide a vertex smoothly and confirm the projected position advances in
     * whole-pixel steps rather than continuously. This is the property that
     * produces the PlayStation's characteristic shimmer, so if it ever stopped
     * holding the look would be wrong even though nothing crashed.
     */
    for (i = 0; i < 200; i++) {
        g.v[0].x = (s16)i;
        g.v[0].y = 0;
        g.v[0].z = 0;
        gte_rtps(&g, false);

        if (g.sxy[2].x != last) {
            distinct++;
            last = g.sxy[2].x;
        }
    }

    check(distinct > 1, "the projected position actually moves");
    check(distinct < 200, "the projected position snaps rather than moving every step");
    printf("  200 sub-pixel input steps produced %d distinct pixel positions\n", distinct);
}

/* ------------------------------------------------------------------------- */
/* Fixed-point trig                                                           */
/* ------------------------------------------------------------------------- */
static void test_trig(void)
{
    printf("fixed-point trig\n");

    check_eq_i(q2_sin12(0), 0, "sin(0) == 0");
    check_eq_i(q2_sin12(Q2_ANGLE_90), Q2_ONE_12, "sin(90 deg) == 1.0 exactly");
    check_eq_i(q2_cos12(0), Q2_ONE_12, "cos(0) == 1.0 exactly");
    check_eq_i(q2_sin12(Q2_ANGLE_180), 0, "sin(180 deg) == 0");
    check_eq_i(q2_cos12(Q2_ANGLE_90), 0, "cos(90 deg) == 0");

    /* Wrapping must be exact in both directions, or a camera that turns past a
     * full circle would jitter. */
    check_eq_i(q2_sin12(Q2_ANGLE_360), q2_sin12(0), "angle wraps at a full turn");
    check_eq_i(q2_sin12(-Q2_ANGLE_90), -Q2_ONE_12, "negative angles wrap correctly");

    /* sin^2 + cos^2 == 1 within fixed-point rounding, swept over the circle. */
    {
        int a;
        s32 worst = 0;

        for (a = 0; a < Q2_ANGLE_360; a++) {
            s32 s = q2_sin12(a), c = q2_cos12(a);
            s32 sum = (s32)(((s64)s * s + (s64)c * c) >> Q2_FRAC_12);
            s32 err = sum > Q2_ONE_12 ? sum - Q2_ONE_12 : Q2_ONE_12 - sum;
            if (err > worst)
                worst = err;
        }
        printf("  worst sin^2+cos^2 error over the circle: %d / 4096\n", worst);
        check(worst <= 2, "the trig identity holds within 2/4096");
    }
}

/* ------------------------------------------------------------------------- */
/* Ordering table                                                             */
/* ------------------------------------------------------------------------- */
typedef struct visit_log {
    u16 order[16];
    int count;
} visit_log;

static void log_visit(const psx_prim *prim, void *user)
{
    visit_log *log = (visit_log *)user;
    if (log->count < 16)
        log->order[log->count++] = prim->otz;
}

/* The same log, keyed on `clut` — the tag the intra-bucket tests label their
 * primitives with, since those all share one otz. */
static void log_clut(const psx_prim *prim, void *user)
{
    visit_log *log = (visit_log *)user;
    if (log->count < 16)
        log->order[log->count++] = prim->clut;
}

static void test_ordering_table(void)
{
    psx_ot ot;
    visit_log log;
    psx_prim *p;

    printf("ordering table\n");

    if (psx_ot_init(&ot, 64, 16) != Q2_OK) {
        printf("  FAIL  could not allocate the ordering table\n");
        g_failures++;
        return;
    }

    /* Far to near: a higher bucket index is more distant and must draw first. */
    p = psx_ot_add(&ot, 10); check(p != NULL, "add to bucket 10");
    p = psx_ot_add(&ot, 30); check(p != NULL, "add to bucket 30");
    p = psx_ot_add(&ot, 20); check(p != NULL, "add to bucket 20");

    memset(&log, 0, sizeof(log));
    psx_ot_walk(&ot, log_visit, &log);

    check_eq_i(log.count, 3, "all three primitives were visited");
    check_eq_i(log.order[0], 30, "the most distant bucket draws first");
    check_eq_i(log.order[1], 20, "then the middle bucket");
    check_eq_i(log.order[2], 10, "then the nearest bucket");

    /*
     * Within one bucket the hardware built its list by prepending, so the LAST
     * primitive added draws FIRST. Getting this backwards produces subtly wrong
     * overlaps in coplanar geometry -- the kind of bug that looks like a
     * texture problem for a week.
     */
    psx_ot_clear(&ot);
    p = psx_ot_add(&ot, 5); p->clut = 1;
    p = psx_ot_add(&ot, 5); p->clut = 2;
    p = psx_ot_add(&ot, 5); p->clut = 3;

    {
        /* A depth is not a bucket index: the table is walked forward, so depth
         * 5 lands five buckets down from the far end. */
        u32 bucket = psx_ot_depth_bucket(&ot, 5);
        const psx_prim *first;

        /* psx_ot_init counts in CONSOLE buckets and the table holds the
         * subdivided ones, so 64 asked for is 64 * PSX_OT_SUBDIV held. */
        check_eq_i((int)bucket, 64 * PSX_OT_SUBDIV - 1 - 5,
                   "depth 5 counts back from the far end");
        first = &ot.prims[ot.bucket_head[bucket]];
        check_eq_i(first->clut, 3, "within a bucket, the last added draws first");
    }

    /*
     * A KEYED add orders the bucket by depth instead, which is what stops a
     * light or a button that shares a wall's bucket from being ordered by
     * emission order and swapping the moment the bucket boundary moves. All
     * three land in the same bucket; the farthest must come out first, and the
     * insertion order is deliberately the reverse of the depth order so that a
     * prepend cannot pass this by accident.
     */
    psx_ot_clear(&ot);
    p = psx_ot_add_depth(&ot, 5, 100); p->clut = 1;   /* nearest  */
    p = psx_ot_add_depth(&ot, 5, 900); p->clut = 2;   /* farthest */
    p = psx_ot_add_depth(&ot, 5, 500); p->clut = 3;   /* middle   */

    {
        memset(&log, 0, sizeof(log));
        psx_ot_walk(&ot, log_clut, &log);
        check_eq_i(log.count, 3, "all three keyed primitives were visited");
        check_eq_i(log.order[0], 2, "in one bucket the farthest key draws first");
        check_eq_i(log.order[1], 3, "then the middle one");
        check_eq_i(log.order[2], 1, "then the nearest");
    }

    /* Equal keys keep the hardware's rule, so a caller that keys everything the
     * same is exactly where it was before keys existed. */
    psx_ot_clear(&ot);
    p = psx_ot_add_depth(&ot, 5, 500); p->clut = 1;
    p = psx_ot_add_depth(&ot, 5, 500); p->clut = 2;
    p = psx_ot_add_depth(&ot, 5, 500); p->clut = 3;

    {
        memset(&log, 0, sizeof(log));
        psx_ot_walk(&ot, log_clut, &log);
        check_eq_i(log.order[0], 3, "equal keys still draw last-added first");
        check_eq_i(log.order[2], 1, "...and first-added last");
    }

    /* An unkeyed add is a barrier: it prepends, and a later keyed add does not
     * sort past it however far away it claims to be. */
    psx_ot_clear(&ot);
    p = psx_ot_add_depth(&ot, 5, 100); p->clut = 1;
    p = psx_ot_add_bucket(&ot, psx_ot_depth_bucket(&ot, 5)); p->clut = 2;
    p = psx_ot_add_depth(&ot, 5, 900); p->clut = 3;

    {
        memset(&log, 0, sizeof(log));
        psx_ot_walk(&ot, log_clut, &log);
        check_eq_i(log.order[0], 3, "a keyed add stops at the unkeyed one");
        check_eq_i(log.order[1], 2, "the unkeyed packet keeps its place");
        check_eq_i(log.order[2], 1, "and what it was prepended over stays put");
    }

    /* Retail's screen-area table names an authored console bucket inside the
     * active viewport slice. Registration scales both numbers exactly once,
     * and a private batch joined after the static packet is prepended ahead of
     * it while retaining depth order against other batches. */
    psx_ot_clear(&ot);
    psx_ot_set_window(&ot, 3, 51);
    check(!psx_ot_area_active(&ot),
          "a fresh viewport has no active area routing");
    check(psx_ot_area_register(&ot, 7, 43),
          "a seven-bit screen area registers");
    check(psx_ot_area_active(&ot),
          "registering the camera area activates routing");
    {
        u32 bucket = 0;

        check(psx_ot_area_bucket(&ot, 7, &bucket),
              "the registered area resolves");
        check_eq_i((int)bucket, (3 + 43) * PSX_OT_SUBDIV,
                   "area bucket is relative to and scaled with the viewport");

        p = psx_ot_add_bucket(&ot, bucket); p->clut = 1;       /* static */
        p = psx_ot_add_area_depth(&ot, 7, 100, 100); p->clut = 2;
        p = psx_ot_add_area_depth(&ot, 7, 900, 900); p->clut = 3;

        memset(&log, 0, sizeof(log));
        psx_ot_walk(&ot, log_clut, &log);
        check_eq_i(log.count, 3, "area bucket drains all private packets");
        check_eq_i(log.order[0], 3,
                   "farther area batch draws first after the final drain");
        check_eq_i(log.order[1], 2,
                   "nearer area batch follows it");
        check_eq_i(log.order[2], 1,
                   "both private batches remain ahead of authored static");
    }

    {
        u32 before = ot.prim_count;
        psx_ot_area_clear(&ot);
        check(!psx_ot_area_active(&ot),
              "the next viewport clears area freshness");
        check(!psx_ot_area_bucket(&ot, 7, NULL),
              "a cleared area record cannot be reused");
        check_eq_i((int)ot.prim_count, (int)before,
                   "clearing area records retains earlier viewport packets");
    }

    /* SortData does not address the geometry-only window. Its bucket numbers
     * are relative to the complete 51-entry viewport slice, including the two
     * structural entries below geometry. Retail also gives areas 1 and 2
     * fixed drain buckets and changes the GTE centre only for ordinary screen
     * records (2 and above). */
    psx_ot_clear(&ot);
    psx_ot_set_authored_window(&ot, 2, 51);
    psx_ot_set_window(&ot, 4, 49);
    check_eq_i(psx_ot_authored_bucket(&ot, 43),
               (2 + 43) * PSX_OT_SUBDIV,
               "authored bucket uses the full viewport slice");
    psx_ot_area_prepare(&ot, 512, 248, 6, 4, 256, 124);
    {
        static const s32 camera[3] = { 0, 0, 0 };
        static const s32 point[3] = { 0, 0, 100 };
        psx_ot_area_screen region;
        psx_ot_area_screen got;
        u32 bucket = 0;
        s32 centre_x = 0, centre_y = 0;

        memset(&region, 0, sizeof(region));
        region.min_x = 32;
        region.min_y = 16;
        region.max_x = 200;
        region.max_y = 120;
        region.add_x = 6;
        region.add_y = 4;

        check(psx_ot_area_bucket(&ot, 1, &bucket),
              "special area 1 is always live");
        check_eq_i(bucket, (2 + 46) * PSX_OT_SUBDIV,
                   "special area 1 drains at authored bucket 46");
        check(psx_ot_area_bucket(&ot, 2, &bucket),
              "special area 2 is always live");
        check_eq_i(bucket, (2 + 1) * PSX_OT_SUBDIV,
                   "special area 2 drains at authored bucket 1");

        check(psx_ot_area_register_screen(&ot, 7, 43, &region),
              "ordinary area accepts its projected screen record");
        check(psx_ot_area_bucket(&ot, 7, &bucket),
              "ordinary area resolves after registration");
        check_eq_i(bucket, (2 + 43) * PSX_OT_SUBDIV,
                   "ordinary area keeps its authored insertion bucket");
        check(psx_ot_area_get_screen(&ot, 7, &got),
              "ordinary area retains its screen rectangle");
        check_eq_i(got.min_x, 32, "screen record retains its minimum x");
        check_eq_i(got.max_y, 120, "screen record retains its maximum y");

        check(psx_ot_area_projection(&ot, 7, &centre_x, &centre_y),
              "ordinary area selects a projection centre");
        check_eq_i(centre_x, 256 - 32,
                   "ordinary area subtracts the region minimum x");
        check_eq_i(centre_y, 124 - 16,
                   "ordinary area subtracts the region minimum y");
        check(psx_ot_area_projection(&ot, 1, &centre_x, &centre_y),
              "special area 1 selects the viewport projection");
        check_eq_i(centre_x, 256,
                   "special area 1 ignores a screen-record minimum");
        check_eq_i(centre_y, 124,
                   "special area 1 retains the viewport centre");
        check(psx_ot_area_projection(&ot, 0, &centre_x, &centre_y),
              "area 0 selects the global projection path");
        check_eq_i(centre_x, 256,
                   "area 0 retains the full-view centre");
        centre_x = -77;
        centre_y = -88;
        check(!psx_ot_area_projection(&ot, -1, &centre_x, &centre_y),
              "a negative selector is retail's projection no-op");
        check_eq_i(centre_x, -77,
                   "negative selector leaves projection x untouched");
        check_eq_i(centre_y, -88,
                   "negative selector leaves projection y untouched");
        check(!psx_ot_area_projection(&ot, 8, &centre_x, &centre_y),
              "a stale ordinary area does not invent a full-view record");
        check_eq_i(psx_ot_batch_begin_point(&ot, 0, true, 100,
                                             point, camera),
                   PSX_OT_BATCH_INVALID,
                   "area 0 is not queued into the regional drain");
    }

    /* The retail regional sorter keeps whole chains atomic. Quick records are
     * stable-sorted by signed +4, then CatPrim reverses sorter output into the
     * area's final chain. */
    psx_ot_clear(&ot);
    check(psx_ot_area_register(&ot, 7, 43),
          "batch-sort area registers");
    {
        static const s32 camera[3] = { 0, 0, 0 };
        static const s32 point[3] = { 0, 0, 100 };
        u32 bucket = 43 * PSX_OT_SUBDIV;
        s32 low, high;

        p = psx_ot_add_bucket(&ot, bucket); p->clut = 1;
        low = psx_ot_batch_begin_point(&ot, 7, true, 100,
                                       point, camera);
        p = psx_ot_batch_add(&ot, low); p->clut = 10;
        p = psx_ot_batch_add(&ot, low); p->clut = 11;
        high = psx_ot_batch_begin_point(&ot, 7, true, 300,
                                        point, camera);
        p = psx_ot_batch_add(&ot, high); p->clut = 30;

        psx_ot_flush_batches(&ot);
        memset(&log, 0, sizeof(log));
        psx_ot_walk(&ot, log_clut, &log);
        check_eq_i(log.count, 4, "quick batches and static chain all drain");
        check_eq_i(log.order[0], 30,
                   "CatPrim reverses ascending Quick order");
        check_eq_i(log.order[1], 11,
                   "a private chain retains its prepended head");
        check_eq_i(log.order[2], 10,
                   "a private chain remains contiguous and atomic");
        check_eq_i(log.order[3], 1,
                   "regional chains are joined above authored static");
    }

    /* Standard records use the exact AABB/camera dependency relation. With a
     * camera at the origin, the farther positive-X box waits on the nearer one
     * in sorter-output order; CatPrim reverses that output into far-to-near draw
     * order. This pins the low-side branch at retail 0x800479E0. */
    psx_ot_clear(&ot);
    check(psx_ot_area_register(&ot, 7, 43),
          "standard-sort area registers");
    {
        static const s32 camera[3] = { 0, 0, 0 };
        static const s32 near_min[3] = { 100, -10, -10 };
        static const s32 near_max[3] = { 200,  10,  10 };
        static const s32 far_min[3]  = { 300, -10, -10 };
        static const s32 far_max[3]  = { 400,  10,  10 };
        u32 bucket = 43 * PSX_OT_SUBDIV;
        s32 near_batch, far_batch;

        p = psx_ot_add_bucket(&ot, bucket); p->clut = 1;
        near_batch = psx_ot_batch_begin_box(&ot, 7, false, 100,
                                             near_min, near_max, camera);
        p = psx_ot_batch_add(&ot, near_batch); p->clut = 20;
        far_batch = psx_ot_batch_begin_box(&ot, 7, false, 300,
                                            far_min, far_max, camera);
        p = psx_ot_batch_add(&ot, far_batch); p->clut = 40;

        /* Area clear is itself a viewport-boundary drain. */
        psx_ot_area_clear(&ot);
        memset(&log, 0, sizeof(log));
        psx_ot_walk(&ot, log_clut, &log);
        check_eq_i(log.count, 3, "standard dependency records drain on clear");
        check_eq_i(log.order[0], 40,
                   "low-side AABB dependency draws the far box first");
        check_eq_i(log.order[1], 20,
                   "low-side AABB dependency draws the near box second");
        check_eq_i(log.order[2], 1,
                   "the Standard graph remains above static geometry");
        check_eq_i((int)ot.batch_count, 0,
                   "viewport drain consumes every batch record");
    }

    /* Put the camera beyond the same pair. Retail changes which faces are
     * compared, so the old near box is now farther and must draw first. */
    psx_ot_clear(&ot);
    check(psx_ot_area_register(&ot, 7, 43),
          "high-side standard-sort area registers");
    {
        static const s32 camera[3]  = { 500, 0, 0 };
        static const s32 low_min[3] = { 100, -10, -10 };
        static const s32 low_max[3] = { 200,  10,  10 };
        static const s32 high_min[3] = { 300, -10, -10 };
        static const s32 high_max[3] = { 400,  10,  10 };
        s32 low_batch, high_batch;

        low_batch = psx_ot_batch_begin_box(&ot, 7, false, 100,
                                            low_min, low_max, camera);
        p = psx_ot_batch_add(&ot, low_batch); p->clut = 20;
        high_batch = psx_ot_batch_begin_box(&ot, 7, false, 300,
                                             high_min, high_max, camera);
        p = psx_ot_batch_add(&ot, high_batch); p->clut = 40;

        memset(&log, 0, sizeof(log));
        psx_ot_walk(&ot, log_clut, &log);
        check_eq_i(log.count, 2, "high-side AABB records drain");
        check_eq_i(log.order[0], 20,
                   "high-side AABB dependency draws the low box first");
        check_eq_i(log.order[1], 40,
                   "high-side AABB dependency draws the high box second");
    }

    /* Quick point records compare against Standard bounds with a ten-unit
     * tolerance. One point on either side of the box proves both branches of
     * 0x80047990 and the Quick-to-Standard dependency direction. */
    psx_ot_clear(&ot);
    check(psx_ot_area_register(&ot, 7, 43),
          "point-versus-box sort area registers");
    {
        static const s32 camera[3] = { 0, 0, 0 };
        static const s32 box_min[3] = { 100, -10, -10 };
        static const s32 box_max[3] = { 200,  10,  10 };
        static const s32 front_point[3] = { 50, 0, 0 };
        static const s32 back_point[3] = { 300, 0, 0 };
        s32 box_batch, front_batch, back_batch;

        box_batch = psx_ot_batch_begin_box(&ot, 7, false, 150,
                                            box_min, box_max, camera);
        p = psx_ot_batch_add(&ot, box_batch); p->clut = 15;
        front_batch = psx_ot_batch_begin_point(&ot, 7, true, 50,
                                                front_point, camera);
        p = psx_ot_batch_add(&ot, front_batch); p->clut = 5;
        back_batch = psx_ot_batch_begin_point(&ot, 7, true, 300,
                                               back_point, camera);
        p = psx_ot_batch_add(&ot, back_batch); p->clut = 30;

        memset(&log, 0, sizeof(log));
        psx_ot_walk(&ot, log_clut, &log);
        check_eq_i(log.count, 3, "point-versus-box records drain");
        check_eq_i(log.order[0], 30,
                   "a point behind the box draws before it");
        check_eq_i(log.order[1], 15,
                   "the Standard box draws between the two points");
        check_eq_i(log.order[2], 5,
                   "a point in front of the box draws last");
    }

    /* Two coincident Standard bounds satisfy the comparator in both
     * directions. 0x80047A68 suppresses the reciprocal edge established by
     * the earlier newest-first comparison; retaining both would enter the
     * cycle breaker and reverse this deterministic tie. */
    psx_ot_clear(&ot);
    check(psx_ot_area_register(&ot, 7, 43),
          "reciprocal-edge sort area registers");
    {
        static const s32 camera[3] = { 0, 0, 0 };
        static const s32 box_min[3] = { 100, -10, -10 };
        static const s32 box_max[3] = { 200,  10,  10 };
        s32 older, newer;

        older = psx_ot_batch_begin_box(&ot, 7, false, 100,
                                        box_min, box_max, camera);
        p = psx_ot_batch_add(&ot, older); p->clut = 1;
        newer = psx_ot_batch_begin_box(&ot, 7, false, 100,
                                        box_min, box_max, camera);
        p = psx_ot_batch_add(&ot, newer); p->clut = 2;

        memset(&log, 0, sizeof(log));
        psx_ot_walk(&ot, log_clut, &log);
        check_eq_i(log.count, 2, "coincident Standard records drain");
        check_eq_i(log.order[0], 2,
                   "reciprocal suppression preserves the newest-first tie");
        check_eq_i(log.order[1], 1,
                   "the older coincident record follows the newest one");
    }

    psx_ot_free(&ot);
}

/* ------------------------------------------------------------------------- */
/* Placeholder for hardware-captured vectors                                  */
/* ------------------------------------------------------------------------- */
static void test_known_vectors(void)
{
    printf("hardware-captured vectors\n");
    printf("  SKIPPED - no capture available.\n");
    printf("  This is the only test that can prove bit-exactness. Until it\n");
    printf("  exists, treat the fidelity claim as 'built to be exact', not\n");
    printf("  'measured to be exact'.\n");
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    printf("Q2PSX-PC fidelity conformance tests\n\n");

    test_divide();
    test_saturation();
    test_vertex_snapping();
    test_trig();
    test_ordering_table();
    test_known_vectors();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");

    return g_failures ? 1 : 0;
}
