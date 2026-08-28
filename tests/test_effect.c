/*
 * test_effect.c — particles, beams, debris and glints.
 *
 * The numbers here are transcribed from the effect system's own code, so a
 * failure is a divergence from the console rather than from a guess. Each
 * check names the address that makes it checkable.
 *
 * The colour ramps live in the executable and are not available to a test that
 * has no disc, so the group tests build a synthetic ramp whose entries are
 * their own index. That is stronger than using a real one: it makes the ramp
 * LOOKUP visible in the assertion instead of hiding it behind a gradient.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "effect.h"

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
/* A table with one identifiable ramp per slot                                */
/* ------------------------------------------------------------------------- */
static q2_fx_tables g_tab;

static void build_tables(void)
{
    u32 i, c, f;

    memset(&g_tab, 0, sizeof(g_tab));
    g_tab.loaded = true;

    for (i = 0; i < Q2_FX_RAMP_COUNT; i++) {
        g_tab.ramp[i].abr = Q2_FX_ABR_ADD;
        g_tab.ramp_addr[i] = Q2_FXT_ADDR_RAMPS + i * Q2_FX_RAMP_STRIDE;
        g_tab.ramp_id_to_index[i] = (u8)i;
        for (c = 0; c < Q2_FX_RAMP_COLOURS; c++) {
            /* r = the entry index, g = the ramp index, code = FT4+abe. */
            g_tab.ramp[i].colour[c] =
                (u32)c | ((u32)i << 8) | (0x2Eu << 24);
        }
    }
    g_tab.ramp_index_is_permutation = true;

    for (i = 0; i < Q2_FX_BEAM_STYLE_COUNT; i++) {
        static const u8 tube[6][4] = {
            { 0, 1,  6,  7 }, { 1, 2,  7,  8 }, { 2, 3,  8,  9 },
            { 3, 4,  9, 10 }, { 4, 5, 10, 11 }, { 5, 0, 11,  6 }
        };
        static const u8 cn[2][4] = { { 1, 0, 2, 3 }, { 4, 3, 5, 0 } };
        static const u8 cf[2][4] = { { 0, 1, 3, 2 }, { 3, 4, 0, 5 } };

        for (f = 0; f < Q2_FX_BEAM_TUBE_FACES; f++) {
            memcpy(g_tab.beam[i].tube[f].v, tube[f], 4);
            for (c = 0; c < 4; c++)
                g_tab.beam[i].tube[f].colour[c] = 0x3A007F00u | i;
        }
        for (f = 0; f < Q2_FX_BEAM_CAP_FACES; f++) {
            memcpy(g_tab.beam[i].cap_near[f].v, cn[f], 4);
            memcpy(g_tab.beam[i].cap_far[f].v, cf[f], 4);
            for (c = 0; c < 4; c++) {
                g_tab.beam[i].cap_near[f].colour[c] = 0x3A007F00u | i;
                g_tab.beam[i].cap_far[f].colour[c]  = 0x3A007F00u | i;
            }
        }
    }

    /* The six arms, as fxtables.c transcribes them. */
    {
        static const s16 radius[6] = { 16, 16, 16, 64, 64, 64 };
        static const u8  style[6]  = {  0,  1,  2,  1,  0,  2 };
        static const u8  ramp[6]   = {  1,  0,  9,  0,  1,  9 };
        static const s16 dmg[6]    = { 512,  1, 512,  1, 512, 512 };
        static const s16 mod[6]    = { 11, 16, 12, 16, 11, 12 };

        for (i = 0; i < Q2_FX_LASER_KIND_COUNT; i++) {
            g_tab.laser[i].radius = radius[i];
            g_tab.laser[i].style  = style[i];
            g_tab.laser[i].ramp   = ramp[i];
            g_tab.laser[i].damage = dmg[i];
            g_tab.laser[i].mod    = mod[i];
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Groups                                                                     */
/* ------------------------------------------------------------------------- */
static void test_spawn_stores_relative_velocities(void)
{
    q2_fx_world w;
    s16 vel[3][3];
    s32 at[3] = { 100, 200, 300 };
    s32 slot;

    printf("group: velocities are stored relative to particle 0\n");

    q2_fx_world_init(&w, &g_tab);

    vel[0][0] = 10; vel[0][1] =  20; vel[0][2] =  30;
    vel[1][0] = 15; vel[1][1] =  20; vel[1][2] =  25;
    vel[2][0] =  5; vel[2][1] = -20; vel[2][2] =  30;

    slot = q2_fx_group_spawn(&w, at, vel, 3, &g_tab.ramp[0], &g_tab.ramp[0],
                             15, 4096, 0);
    check(slot == 0, "the first spawn takes slot 0");

    /* 0x800303C8 subtracts vel[0] from every later velocity. */
    check_eq_i(w.group[0].vel[0], 10, "vel[0].x is absolute");
    check_eq_i(w.group[0].rel_vel[0][0],  5, "vel[1].x - vel[0].x");
    check_eq_i(w.group[0].rel_vel[0][1],  0, "vel[1].y - vel[0].y");
    check_eq_i(w.group[0].rel_vel[1][1], -40, "vel[2].y - vel[0].y");

    /* 0x800303B4 and 0x8003040C clear the offsets and the acceleration. */
    check_eq_i(w.group[0].offset[0][0], 0, "offsets start at zero");
    check_eq_i(w.group[0].accel[1], 0, "acceleration starts at zero");
}

static void test_size_scale(void)
{
    q2_fx_world w;
    s16 vel[1][3] = { { 0, 0, 0 } };
    s32 at[3] = { 0, 0, 0 };

    printf("group: size is scaled and rounded toward zero\n");

    q2_fx_world_init(&w, &g_tab);

    /* 0x80030430: `(arg * scale) / 512`, and the default scale is a no-op. */
    q2_fx_group_spawn(&w, at, vel, 1, &g_tab.ramp[0], NULL, 15, 6144, 0);
    check_eq_i(w.group[0].size, 6144, "unity scale passes the size through");

    q2_fx_world_clear(&w);
    w.size_scale = 256;
    q2_fx_group_spawn(&w, at, vel, 1, &g_tab.ramp[0], NULL, 15, 6144, 0);
    check_eq_i(w.group[0].size, 3072, "half scale halves the size");

    q2_fx_world_clear(&w);
    w.size_scale = 256;
    q2_fx_group_spawn(&w, at, vel, 1, &g_tab.ramp[0], NULL, 15, -3, 0);
    /* -3 * 256 = -768; toward zero that is -1, not the -2 a shift would give. */
    check_eq_i(w.group[0].size, -1, "a negative size rounds toward zero");
}

static void test_integrator_order(void)
{
    q2_fx_world w;
    s16 vel[2][3];
    s32 at[3] = { 0, 0, 0 };

    printf("group: position uses the velocity before the acceleration\n");

    q2_fx_world_init(&w, &g_tab);

    vel[0][0] = 10; vel[0][1] = 0; vel[0][2] = 0;
    vel[1][0] = 13; vel[1][1] = 0; vel[1][2] = 0;

    q2_fx_group_spawn(&w, at, vel, 2, &g_tab.ramp[0], NULL, 5, 4096, 0);
    w.group[0].accel[0] = 4;

    q2_fx_tick(&w);

    /* 0x80030B34 adds the velocity, THEN 0x80030B74 adds the acceleration. */
    check_eq_i(w.group[0].origin[0], 10, "one tick moves by the old velocity");
    check_eq_i(w.group[0].vel[0],    14, "the velocity has taken the accel");
    check_eq_i(w.group[0].life,       4, "life counted down");

    /* The follower's offset advances by its RELATIVE velocity only. */
    check_eq_i(w.group[0].offset[0][0], 3, "the follower drifted by 3");

    q2_fx_tick(&w);
    check_eq_i(w.group[0].origin[0], 24, "the second tick uses 14");
}

static void test_life_zero_frees_the_slot(void)
{
    q2_fx_world w;
    s16 vel[1][3] = { { 0, 0, 0 } };
    s32 at[3] = { 0, 0, 0 };

    printf("group: a slot is free again when its life reaches zero\n");

    q2_fx_world_init(&w, &g_tab);
    q2_fx_group_spawn(&w, at, vel, 1, &g_tab.ramp[0], NULL, 1, 4096, 0);
    check_eq_i(w.group[0].life, 1, "spawned with life 1");

    q2_fx_tick(&w);
    check_eq_i(w.group[0].life, 0, "one tick retires it");

    /* And the next spawn reuses the slot, because 0x800302E8 takes the first
     * record whose life is zero. */
    check_eq_i(q2_fx_group_spawn(&w, at, vel, 1, &g_tab.ramp[0], NULL,
                                 5, 4096, 0),
               0, "the retired slot is reused");
}

static void test_pool_fills_and_refuses(void)
{
    q2_fx_world w;
    s16 vel[1][3] = { { 0, 0, 0 } };
    s32 at[3] = { 0, 0, 0 };
    u32 i, made = 0;

    printf("group: the pool refuses when full\n");

    q2_fx_world_init(&w, &g_tab);
    check_eq_i(w.group_count, Q2_FX_GROUPS_DEFAULT,
               "the shipped pool is 32 groups");

    for (i = 0; i < Q2_FX_GROUPS_DEFAULT + 4; i++) {
        if (q2_fx_group_spawn(&w, at, vel, 1, &g_tab.ramp[0], NULL,
                              15, 4096, 0) >= 0)
            made++;
    }
    check_eq_i(made, Q2_FX_GROUPS_DEFAULT, "exactly the pool's worth spawned");

    /* 0x80030C88: a zero argument means the default, not an empty pool. */
    q2_fx_world_resize(&w, 0, 1);
    check_eq_i(w.group_count, Q2_FX_GROUPS_DEFAULT,
               "resize(0) means the default");
}

static void test_ramp_is_indexed_by_age(void)
{
    q2_fx_world w;
    s16 vel[1][3] = { { 0, 0, 0 } };
    s32 at[3] = { 0, 0, 0 };
    u32 c;

    printf("group: the ramp entry is 32 - life\n");

    q2_fx_world_init(&w, &g_tab);
    q2_fx_group_spawn(&w, at, vel, 1, &g_tab.ramp[3], &g_tab.ramp[7],
                      15, 4096, 0);

    /* Entry index lands in the red channel, ramp index in the green. */
    c = q2_fx_group_colour(&w.group[0], 0);
    check_eq_i(q2_fx_colour_r(c), 32 - 15, "life 15 reads entry 17");
    check_eq_i(q2_fx_colour_g(c), 3, "ramp 0 is the one that was passed");

    c = q2_fx_group_colour(&w.group[0], 1);
    check_eq_i(q2_fx_colour_g(c), 7, "ramp 1 is the second one");

    q2_fx_tick(&w);
    c = q2_fx_group_colour(&w.group[0], 0);
    check_eq_i(q2_fx_colour_r(c), 32 - 14, "a tick advances the entry");

    /* The clamp: nothing on the disc spawns above 32, but a save could. */
    check_eq_i(q2_fx_ramp_index_for_life(40), 0, "life above 32 clamps to 0");
    check_eq_i(q2_fx_ramp_index_for_life(0), 31, "life 0 clamps to the tail");
}

static void test_one_ramp_defaults_to_both(void)
{
    q2_fx_world w;
    s16 vel[1][3] = { { 0, 0, 0 } };
    s32 at[3] = { 0, 0, 0 };

    printf("group: a NULL second ramp reuses the first\n");

    q2_fx_world_init(&w, &g_tab);
    q2_fx_group_spawn(&w, at, vel, 1, &g_tab.ramp[5], NULL, 15, 4096, 0);
    check(w.group[0].ramp[0] == w.group[0].ramp[1],
          "both ramp slots point at the same record");
}

static void test_budget(void)
{
    printf("group: the per-frame quad budget\n");

    /* 0x80030CB4: one viewport spends the whole pool. */
    check_eq_i(q2_fx_budget(32, 1), 32 * 15, "one viewport gets groups * 15");
    /* More than one halves the pool FIRST, so two views get the same total. */
    check_eq_i(q2_fx_budget(32, 2), 16 * 2 * 15, "two views get (n/2)*2*15");
    check_eq_i(q2_fx_budget(32, 4), 16 * 4 * 15, "four views get (n/2)*4*15");
}

/* ------------------------------------------------------------------------- */
/* Presets                                                                    */
/* ------------------------------------------------------------------------- */
static void test_presets(void)
{
    const q2_fx_preset *p;

    printf("presets: the transcribed immediates\n");

    p = q2_fx_preset_at(Q2_FX_EXPLOSION);
    check_eq_i(p->size, 8192, "the explosion's size is 8192 (0x800486DC)");
    check_eq_i(p->life, 15, "and its life 15");
    check_eq_i(p->spread_shift, 9, "and its spread shift 9");
    check_eq_i(p->ramp0, 9, "and its ramp the orange fire one (0x80048674)");

    p = q2_fx_preset_at(Q2_FX_BLOOD);
    check_eq_i(p->size, 6144, "blood's size is 6144 (0x80048BE8)");
    check_eq_i(p->spread_shift, 10, "blood is the tighter shift 10");
    check(p->ramp0 != p->ramp1, "blood is the one two-ramp effect");

    p = q2_fx_preset_at(Q2_FX_BFG_BURST);
    check_eq_i(p->size, 20000, "the BFG's size is 20000 (0x8004BDBC)");

    p = q2_fx_preset_at(Q2_FX_GIB);
    check_eq_i(p->life, 10, "a gib puff lives 10 ticks (0x80059698)");
    check_eq_i(p->size, 10000, "and is 10000 across (0x800596A0)");

    p = q2_fx_preset_at(Q2_FX_SPARK);
    check_eq_i(p->ramp0, 0, "a spark is the blue ramp (0x8003E040)");
    check_eq_i(p->life, 25, "a spark lives 25 ticks (0x8003E0A0)");
    check_eq_i(p->size, 3072, "and is 3072 across (0x8003E0A8)");

    check(q2_fx_preset_at(Q2_FX_PRESET_COUNT) == NULL,
          "an unknown preset is NULL");

    /*
     * The outer loop each site sits in — the column the table did not have, so
     * the port emitted between a half and a quarter of the console's burst
     * density and consumed the generator a different number of times.
     */
    check_eq_i(q2_fx_preset_at(Q2_FX_EXPLOSION)->repeat, 2,
               "the explosion spawns twice (slti v0,s7,2 at 0x800486F4)");
    check_eq_i(q2_fx_preset_at(Q2_FX_BLOOD)->repeat, 2,
               "blood spawns twice (0x80048C24)");
    check_eq_i(q2_fx_preset_at(Q2_FX_BFG_BURST)->repeat, 3,
               "the BFG spawns three times (0x8004BDD0)");
    check_eq_i(q2_fx_preset_at(Q2_FX_SPARK)->repeat, 4,
               "the spark spawns four times (0x8003E0DC)");
    check_eq_i(q2_fx_preset_at(Q2_FX_GIB)->repeat, 1,
               "the gib site has no outer loop");
    /* Not 4: q2_fx_laser spawns its own four groups and never comes through
     * q2_fx_spawn, so a 4 here would be sixteen. */
    check_eq_i(q2_fx_preset_at(Q2_FX_LASER_END)->repeat, 1,
               "the laser end's repeat lives in q2_fx_laser, not the table");

    /* And the Y acceleration four sites write after the spawner returns. */
    check_eq_i(q2_fx_preset_at(Q2_FX_SPARK)->accel_y, 4,
               "the spark sags at 4 (sh 4, 98(v1) at 0x8003E0D4)");
    check_eq_i(q2_fx_preset_at(Q2_FX_BLOOD)->accel_y, 2,
               "blood sags at 2 (0x80048C1C)");
    check_eq_i(q2_fx_preset_at(Q2_FX_LASER_END)->accel_y, 2,
               "a laser end sags at 2 (0x80049088 / 0x80049134)");
    check_eq_i(q2_fx_preset_at(Q2_FX_EXPLOSION)->accel_y, 0,
               "the explosion site writes none");
}

/*
 * The second spawner, 0x8003004C, and the bullet puff that is its reason for
 * existing here.
 */
static void test_spawn_offsets_and_puff(void)
{
    q2_fx_world w;
    q2_rng rng;
    s32 at[3] = { 1000, 2000, 3000 };
    s16 offs[Q2_FX_GROUP_QUADS][3], vel[Q2_FX_GROUP_QUADS][3];
    s32 slot;
    u32 i;
    int scattered = 0;

    printf("group: the offset-taking spawner and the bullet puff\n");

    q2_fx_world_init(&w, &g_tab);

    for (i = 0; i < Q2_FX_GROUP_QUADS; i++) {
        offs[i][0] = (s16)(i * 160);   /* >> 4 gives i * 10 */
        offs[i][1] = 0;
        offs[i][2] = (s16)-(s16)(i * 160);
        vel[i][0] = vel[i][1] = vel[i][2] = 0;
    }

    slot = q2_fx_group_spawn_offsets(&w, at, offs, vel, Q2_FX_GROUP_QUADS,
                                     &g_tab.ramp[6], &g_tab.ramp[4],
                                     32, 4096, 0);
    check(slot >= 0, "the offset spawn took a slot");

    /* offset[i-1] = offs[i] >> 4, and offs[0] is discarded because particle 0
     * is the origin. */
    check_eq_i(w.group[slot].offset[0][0], 10,
               "particle 1 takes offs[1] >> 4");
    check_eq_i(w.group[slot].offset[13][0], 140,
               "and particle 14 takes offs[14] >> 4");
    check_eq_i(w.group[slot].offset[0][2], -10,
               "the shift is arithmetic, so a negative offset survives");

    /* Particle 0 is still exactly on the origin. */
    {
        s32 pt[3];
        q2_fx_group_point(&w.group[slot], 0, pt);
        check(pt[0] == at[0] && pt[1] == at[1] && pt[2] == at[2],
              "particle 0 is the origin, with no offset of its own");
    }

    /* The puff itself: one group, life 32, ramps 6 and 4, pre-scattered. */
    q2_fx_world_init(&w, &g_tab);
    q2_rng_seed(&rng, 4242);

    slot = q2_fx_bullet_puff(&w, &rng, at, 0);
    check(slot >= 0, "the bullet puff spawned");
    check_eq_i(w.group[slot].life, Q2_FX_BULLET_PUFF_LIFE,
               "and lives 32 ticks, not the spark's 25");
    check_eq_i(w.group[slot].count, 15, "fifteen quads");
    check(w.group[slot].ramp[0] != w.group[slot].ramp[1],
          "grey and dark red, not one ramp twice");
    check(w.group[slot].ramp[0] == q2_fx_ramp_at(&g_tab, 6) &&
          w.group[slot].ramp[1] == q2_fx_ramp_at(&g_tab, 4),
          "ramp records 6 and 4 (0x8009BD78 and 0x8009BC70)");

    /* ONE group: this site has no outer loop. A second slot would mean the
     * repeat was applied where the disassembly shows none. */
    check(w.group[1].life == 0, "the bullet site spawns exactly one group");

    /* And it starts already scattered, which is the whole point of the second
     * spawner — the first one leaves every particle on the origin. */
    for (i = 0; i < Q2_FX_GROUP_FOLLOWERS; i++) {
        if (w.group[slot].offset[i][0] || w.group[slot].offset[i][1] ||
            w.group[slot].offset[i][2])
            scattered++;
    }
    check(scattered > 0, "the puff is pre-scattered at spawn");
}

static void test_spawn_preset_separates(void)
{
    q2_fx_world w;
    q2_rng rng;
    s32 at[3] = { 0, 0, 0 };
    s32 slot;
    s32 lo = 0x7FFFFFFF, hi = -0x7FFFFFFF;
    u32 i;
    int t;

    printf("presets: a burst actually separates\n");

    q2_fx_world_init(&w, &g_tab);
    q2_rng_seed(&rng, 12345);

    slot = q2_fx_spawn(&w, &rng, Q2_FX_EXPLOSION, at, 0);
    check(slot >= 0, "the explosion spawned");
    check_eq_i(w.group[slot].count, 15, "fifteen quads");

    for (t = 0; t < 8; t++)
        q2_fx_tick(&w);

    for (i = 0; i < w.group[slot].count; i++) {
        s32 pt[3];
        q2_fx_group_point(&w.group[slot], i, pt);
        if (pt[0] < lo) lo = pt[0];
        if (pt[0] > hi) hi = pt[0];
    }
    check(hi > lo, "the quads are no longer coincident");

    /* Shift 9 gives components in -32..31, so eight ticks can spread at most
     * 8 * 63 on an axis. A spread beyond that means the shift was misread. */
    check(hi - lo <= 8 * 63, "and the spread matches shift 9");
}

/* ------------------------------------------------------------------------- */
/* Beams                                                                      */
/* ------------------------------------------------------------------------- */
static void test_beam_pool(void)
{
    q2_fx_world w;
    s32 a[3] = { 0, 0, 0 }, b[3] = { 4096, 0, 0 };
    u32 i, made = 0;

    printf("beam: the pool is refilled every frame and is 32 deep\n");

    q2_fx_world_init(&w, &g_tab);

    for (i = 0; i < Q2_FX_BEAMS_MAX + 4; i++) {
        if (q2_fx_beam_add_style(&w, a, b, 16, 0, 0))
            made++;
    }
    check_eq_i(made, Q2_FX_BEAMS_MAX, "exactly 32 beams were accepted");
    check_eq_i(w.stats.beams_dropped, 4, "the rest were dropped, not queued");

    q2_fx_beams_reset(&w);
    check_eq_i(w.beam_count, 0, "the reset empties it");
    check(q2_fx_beam_add_style(&w, a, b, 16, 0, 0),
          "and it accepts again afterwards");

    check(!q2_fx_beam_add_style(&w, a, b, 16, 0, Q2_FX_BEAM_STYLE_COUNT),
          "an unknown style is refused");
}

static void test_beam_hull(void)
{
    q2_fx_beam b;
    s32 hull[Q2_FX_BEAM_VERTS][3];
    u32 v;
    int seen_pos = 0, seen_neg = 0;

    printf("beam: the hull is two hexagons perpendicular to the beam\n");

    memset(&b, 0, sizeof(b));
    b.to[0]  = 8192;      /* along +X */
    b.radius = 100;

    check(q2_fx_beam_hull(&b, hull), "the hull built");

    for (v = 0; v < Q2_FX_BEAM_VERTS; v++) {
        s32 anchor = (v < 6) ? b.from[0] : b.to[0];
        s32 r2;

        check_eq_i(hull[v][0], anchor, "no hull point drifts along the beam");

        r2 = hull[v][1] * hull[v][1] + hull[v][2] * hull[v][2];
        /* The hexagon's points sit on the radius, give or take the fixed-point
         * rounding of three 1.12 multiplies. */
        check(r2 > (90 * 90) && r2 < (110 * 110),
              "each hull point sits near the radius");
    }

    /*
     * 0x800635F0 stores each axis and then its negation, so index i and i+3 are
     * opposite. That is what makes the six quads fold through the axis instead
     * of wrapping a prism, and it is the single easiest thing to "fix" wrongly.
     */
    for (v = 0; v < 3; v++) {
        check_eq_i(hull[v][1], -hull[v + 3][1], "vertex i+3 is -vertex i (y)");
        check_eq_i(hull[v][2], -hull[v + 3][2], "vertex i+3 is -vertex i (z)");
    }

    for (v = 0; v < 6; v++) {
        if (hull[v][1] > 0) seen_pos = 1;
        if (hull[v][1] < 0) seen_neg = 1;
    }
    check(seen_pos && seen_neg, "the ring surrounds the beam");

    /* A zero-length beam has no direction, so 0x8006350C draws nothing. */
    b.to[0] = 0;
    check(!q2_fx_beam_hull(&b, hull), "a degenerate beam builds no hull");
}

static void test_beam_hull_vertical(void)
{
    q2_fx_beam b;
    s32 hull[Q2_FX_BEAM_VERTS][3];
    u32 v;

    printf("beam: the basis switches axis for a vertical beam\n");

    memset(&b, 0, sizeof(b));
    b.to[1]  = 8192;      /* straight up, where crossing with Y degenerates */
    b.radius = 100;

    check(q2_fx_beam_hull(&b, hull), "a vertical beam still builds a hull");

    /* 0x80056408 switches to the Y branch above 2897/4096, which is 45
     * degrees; without the switch the cross product collapses and every hull
     * point lands on the beam. */
    for (v = 0; v < Q2_FX_BEAM_VERTS; v++) {
        s32 r2 = hull[v][0] * hull[v][0] + hull[v][2] * hull[v][2];
        check(r2 > (90 * 90), "the ring did not collapse onto the axis");
    }
}

/* ------------------------------------------------------------------------- */
/* Laser                                                                      */
/* ------------------------------------------------------------------------- */
static void test_laser(void)
{
    q2_fx_world w;
    q2_rng rng;
    s32 a[3] = { 0, 0, 0 }, b[3] = { 8192, 0, 0 };
    q2_fx_laser_result r;

    printf("laser: six kinds, three bodies\n");

    q2_fx_world_init(&w, &g_tab);
    q2_rng_seed(&rng, 99);

    check(q2_fx_laser(&w, &rng, 0, a, b, 0,
                      Q2_FX_LASER_END_FROM | Q2_FX_LASER_END_TO, &r),
          "kind 0 is known");
    check(r.queued, "the beam was queued");
    check_eq_i(r.damage, 512, "kind 0 deals 512 (0x80048EAC)");
    check_eq_i(r.mod, 11, "with mod 11, the laser (0x80048EB0)");
    check_eq_i(r.groups, 2 * Q2_FX_LASER_END_GROUPS,
               "both ends throw four groups each");

    q2_fx_world_clear(&w);
    q2_fx_laser(&w, &rng, 1, a, b, 0, Q2_FX_LASER_END_FROM, &r);
    check_eq_i(r.damage, 1, "kind 1 deals 1 (0x80048EFC)");
    check_eq_i(r.mod, 16, "with mod 16 (0x80048F00)");
    check_eq_i(r.groups, Q2_FX_LASER_END_GROUPS, "one end lit is four groups");

    q2_fx_world_clear(&w);
    q2_fx_laser(&w, &rng, 2, a, b, 0, 0, &r);
    check_eq_i(r.mod, 12, "kind 2 uses mod 12 (0x80048F50)");
    check_eq_i(r.groups, 0, "no end lit is no groups");

    /* Kinds 3..5 are the same three bodies at radius 64. */
    q2_fx_world_clear(&w);
    q2_fx_laser(&w, &rng, 4, a, b, 0, 0, &r);
    check_eq_i(r.mod, 11, "kind 4 falls into kind 0's body");
    check_eq_i(w.beam[0].radius, 64, "but at radius 64 (0x80048E74)");

    check(!q2_fx_laser(&w, &rng, Q2_FX_LASER_KIND_COUNT, a, b, 0, 0, &r),
          "kind 6 is refused, matching the sltiu bound");
}

/* ------------------------------------------------------------------------- */
/* Debris                                                                     */
/* ------------------------------------------------------------------------- */
static void test_gib_blood_colour(void)
{
    q2_fx_world w;
    q2_rng rng;
    s32 at[3] = { 0, 0, 0 };
    s32 slot;

    printf("gib: blood colour comes from the creature, not the effect\n");

    /* 0x80059648 tests the three bits in this order and takes the first. */
    check_eq_i(q2_fx_gib_ramp(Q2_FX_BLOOD_RED),    1, "red bleeds ramp 1");
    check_eq_i(q2_fx_gib_ramp(Q2_FX_BLOOD_GREEN), 11, "green bleeds ramp 11");
    check_eq_i(q2_fx_gib_ramp(Q2_FX_BLOOD_BLUE),   0, "blue bleeds ramp 0");

    /* The order, not just the mapping: red wins over green wins over blue. */
    check_eq_i(q2_fx_gib_ramp(Q2_FX_BLOOD_RED | Q2_FX_BLOOD_GREEN |
                              Q2_FX_BLOOD_BLUE),
               1, "red is tested first");
    check_eq_i(q2_fx_gib_ramp(Q2_FX_BLOOD_GREEN | Q2_FX_BLOOD_BLUE),
               11, "then green");

    /* No bits set leaves the original's register undefined; the port picks. */
    check_eq_i(q2_fx_gib_ramp(0), 1, "no flags takes the port's defined value");

    q2_fx_world_init(&w, &g_tab);
    q2_rng_seed(&rng, 7);

    slot = q2_fx_gib(&w, &rng, at, 0, Q2_FX_BLOOD_GREEN);
    check(slot >= 0, "a gib burst spawned");
    /* The synthetic table puts the ramp index in the green channel. */
    check_eq_i(q2_fx_colour_g(q2_fx_group_colour(&w.group[slot], 0)), 11,
               "and it used the green ramp");
    check_eq_i(w.group[slot].life, 10, "with the gib lifetime");
}

static void test_debris(void)
{
    q2_fx_world w;
    q2_rng rng;
    s32 bmin[3] = { -500, -500, -500 }, bmax[3] = { 500, 500, 500 };
    u32 made, i, up = 0, in_box = 0;

    printf("debris: the burst scatters through the box and leaps\n");

    q2_fx_world_init(&w, &g_tab);
    q2_rng_seed(&rng, 4242);

    check(q2_fx_debris_register(&w, 3), "a model registers");
    for (i = 0; i < 40; i++)
        q2_fx_debris_register(&w, (s16)i);
    check_eq_i(w.debris_model_count, 32,
               "registration caps at 32 (0x80064F80)");

    made = q2_fx_debris_burst(&w, &rng, bmin, bmax, NULL, 20, 5);
    check_eq_i(made, 20, "twenty pieces spawned");

    for (i = 0; i < Q2_FX_DEBRIS_MAX; i++) {
        const q2_fx_debris *d = &w.debris[i];
        if (!d->in_use)
            continue;

        /* The Y bias is -1536 on a draw whose range is -1536..1535, so EVERY
         * piece starts moving upward. 0x800646DC. */
        if (d->vel[1] < 0)
            up++;
        if (d->pos[0] >= bmin[0] && d->pos[0] <= bmax[0] &&
            d->pos[1] >= bmin[1] && d->pos[1] <= bmax[1])
            in_box++;

        check(d->vel[0] >= -1536 && d->vel[0] <= 1535,
              "the X draw is in range");
        check(d->vel[1] >= -3072 && d->vel[1] <= -1,
              "the Y draw is biased entirely upward");
        check_eq_i(d->life, 2100, "a fresh piece has 2100 of life");
    }
    check_eq_i(up, 20, "every piece leaps");
    check_eq_i(in_box, 20, "every piece started inside the box");

    /* A fixed point overrides the box, which is how a scripted break puts
     * every shard at one place. */
    q2_fx_world_clear(&w);
    {
        s32 at[3] = { 7, 8, 9 };
        q2_fx_debris_burst(&w, &rng, bmin, bmax, at, 3, 0);
        check_eq_i(w.debris[0].pos[0], 7, "an explicit point wins");
        check_eq_i(w.debris[2].pos[2], 9, "for every piece");
    }

    /* Seven impacts at 300 apiece retire a piece with 2100 of life. */
    {
        int hit;
        for (hit = 0; hit < 6; hit++)
            q2_fx_debris_impact(&w, 0, NULL);
        check(w.debris[0].in_use, "six impacts are survivable");
        q2_fx_debris_impact(&w, 0, NULL);
        check(!w.debris[0].in_use, "the seventh retires it");
    }
}

/* ------------------------------------------------------------------------- */
/* Trails                                                                     */
/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */
/* Drawing                                                                    */
/* ------------------------------------------------------------------------- */
static void test_build_ot(void)
{
    q2_fx_world w;
    q2_rng rng;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    s32 at[3];
    u32 emitted, i;
    u32 quads = 0, beams = 0;

    printf("draw: groups and beams reach the ordering table\n");

    if (psx_ot_init(&ot, 256, 4096) != Q2_OK) {
        printf("  FAIL  could not allocate an ordering table\n");
        g_failures++;
        return;
    }

    gte_init(&gte);
    gte_set_projection(&gte, 256, 256, 124);

    memset(&cam, 0, sizeof(cam));
    cam.projection = 256;
    cam.far_z      = Q2_CAMERA_FAR_DEFAULT;
    /* The sort range is a separate field from the subdivision distance now, and
     * leaving it zero puts the mapping back on the fixed shift. See
     * q2_camera.sort_range. */
    cam.sort_range = Q2_CAMERA_SORT_RANGE;

    q2_fx_world_init(&w, &g_tab);
    q2_rng_seed(&rng, 2024);

    /* A burst well in front of the camera, along +Z. */
    at[0] = 0; at[1] = 0; at[2] = 4000;
    check(q2_fx_spawn(&w, &rng, Q2_FX_EXPLOSION, at, 0) >= 0,
          "a burst spawned in front of the camera");

    /*
     * With no image registered a quad would sample an empty page and show
     * nothing, so the port falls back to a flat quad and says so. Both paths
     * are checked, because the fallback is a divergence and the textured path
     * is the reconstruction.
     */
    check(w.untextured, "a fresh world has no particle image");

    psx_ot_clear(&ot);
    emitted = q2_fx_build_ot(&w, &cam, 0, &ot, &gte);
    check(emitted > 0, "something reached the table");
    check_eq_i(ot.prim_count, emitted, "and everything counted is in it");

    for (i = 0; i < ot.prim_count; i++) {
        if (ot.prims[i].kind == PSX_PRIM_F4)
            quads++;
    }
    /* THIRTY, not fifteen: the explosion site loops twice (`slti v0, s7, 2`
     * at 0x800486F4), so one q2_fx_spawn is two groups of fifteen. This check
     * pinned 15 while the port emitted half the console's burst density. */
    check_eq_i(quads, 30, "two groups of fifteen flat quads for an explosion");

    /* Register one and the quads become what the hardware drew. */
    q2_fx_set_texture(&w, 0x001Au, 0x7C10u);
    check(!w.untextured, "registering an image turns the fallback off");
    check_eq_i(w.tpage_base, 0x001Au,
               "and the ABR field is masked out of the page word");

    psx_ot_clear(&ot);
    q2_fx_build_ot(&w, &cam, 0, &ot, &gte);
    quads = 0;
    for (i = 0; i < ot.prim_count; i++) {
        const psx_prim *p = &ot.prims[i];
        if (p->kind != PSX_PRIM_FT4)
            continue;
        quads++;
        /* 0x80030DB8 bakes a 16x16 patch at (240,240) into every pool slot. */
        check_eq_i(p->uv[0].u, Q2_FX_QUAD_U0, "uv0.u");
        check_eq_i(p->uv[2].v, Q2_FX_QUAD_V1, "uv2.v");
        check_eq_i(p->uv[3].u, Q2_FX_QUAD_U0,
                   "uv3.u — the perimeter swap reached the UVs too");
        check_eq_i(p->clut, 0x7C10u, "the registered palette came through");
        /* 0x80030830 ORs the ramp's blend mode into the page word. */
        check_eq_i(p->tpage, 0x001Au | Q2_FX_ABR_ADD,
                   "page word carries the ramp's blend mode");
    }
    check_eq_i(quads, 30,
               "two groups of fifteen textured quads once an image is registered");

    w.untextured = true;
    quads = 15;

    /*
     * The clamp at 0x80030850: a distant burst becomes a 2x2 dot rather than
     * disappearing, which is what stops long-range gunfire from looking like it
     * produced no effect at all.
     */
    for (i = 0; i < ot.prim_count; i++) {
        const psx_prim *p = &ot.prims[i];
        s16 side = (s16)(p->xy[1].x - p->xy[0].x);
        if (p->kind != PSX_PRIM_F4 && p->kind != PSX_PRIM_FT4)
            continue;
        check(side >= Q2_FX_QUAD_MIN_PIXELS, "no quad is thinner than 2px");
        check_eq_i(p->xy[3].y - p->xy[0].y, side, "and every quad is square");
        check(p->semi_transparent, "the ramp's ABE bit came through");
        check_eq_i(p->tpage & Q2_FX_ABR_MASK, Q2_FX_ABR_ADD,
                   "and its blend mode did too");
    }

    /* A burst BEHIND the camera is dropped whole, not clipped. */
    q2_fx_world_clear(&w);
    at[2] = -4000;
    q2_fx_spawn(&w, &rng, Q2_FX_EXPLOSION, at, 0);
    psx_ot_clear(&ot);
    check_eq_i(q2_fx_build_ot(&w, &cam, 0, &ot, &gte), 0,
               "a burst behind the camera emits nothing");
    check(w.stats.groups_skipped_near > 0, "and is counted as near-rejected");

    /* The budget drops whole bursts rather than truncating them. */
    q2_fx_world_clear(&w);
    at[2] = 4000;
    for (i = 0; i < Q2_FX_GROUPS_DEFAULT; i++)
        q2_fx_spawn(&w, &rng, Q2_FX_EXPLOSION, at, 0);
    w.budget = 20;                       /* room for one burst and a bit */
    psx_ot_clear(&ot);
    q2_fx_build_ot(&w, &cam, 0, &ot, &gte);
    check_eq_i(w.stats.groups_drawn, 1, "the budget stopped after one burst");
    check(w.stats.groups_skipped_budget > 0, "the rest were dropped whole");

    /* Area freshness is a visibility gate, not merely a bucket hint. Retail
     * never drains an effect whose area has no screen record for this view. */
    q2_fx_world_clear(&w);
    at[2] = 4000;
    q2_fx_spawn(&w, &rng, Q2_FX_EXPLOSION, at, 7);
    psx_ot_clear(&ot);
    psx_ot_area_register(&ot, 3, 43);
    check_eq_i(q2_fx_build_ot(&w, &cam, 0, &ot, &gte), 0,
               "a particle group in a stale area is culled whole");
    check_eq_i(ot.prim_count, 0,
               "stale-area culling leaves no orphan packets");

    psx_ot_clear(&ot);
    psx_ot_area_register(&ot, 7, 43);
    check(q2_fx_build_ot(&w, &cam, 0, &ot, &gte) > 0,
          "the same group draws when its area is registered");
    psx_ot_flush_batches(&ot);
    check(ot.bucket_head[43 * PSX_OT_SUBDIV] >= 0,
          "its private chain joins the area's authored bucket");

    /* A beam long enough to have segments. */
    q2_fx_world_clear(&w);
    {
        s32 a[3] = { -2000, 0, 4000 }, b[3] = { 2000, 0, 4000 };

        check(q2_fx_beam_add_style(&w, a, b, 64, 0, 0), "a beam queued");
        psx_ot_clear(&ot);
        q2_fx_build_ot(&w, &cam, 0, &ot, &gte);

        for (i = 0; i < ot.prim_count; i++) {
            if (ot.prims[i].kind == PSX_PRIM_G4)
                beams++;
        }
        /* 4000 units at 640 to the segment is ceil(4000/640) - 1 = 6 segments,
         * six tube faces each, plus two caps at each end. */
        check_eq_i(beams, 6 * Q2_FX_BEAM_TUBE_FACES + 2 * Q2_FX_BEAM_CAP_FACES,
                   "six segments of six faces plus both caps");

        /* A beam shorter than one segment draws nothing — 0x80063BEC bails on
         * a negative count, and that is behaviour, not a bug. */
        q2_fx_beams_reset(&w);
        b[0] = a[0] + 100;
        q2_fx_beam_add_style(&w, a, b, 64, 0, 0);
        psx_ot_clear(&ot);
        check_eq_i(q2_fx_build_ot(&w, &cam, 0, &ot, &gte), 0,
                   "a beam under 640 units draws nothing");
    }

    psx_ot_free(&ot);
}

static void test_texture_survives_clear(void)
{
    q2_fx_world w;

    printf("draw: the image registration survives a clear\n");

    q2_fx_world_init(&w, &g_tab);
    q2_fx_set_texture(&w, 0x0012u, 0x1234u);
    q2_fx_world_clear(&w);

    check(!w.untextured, "still textured after a clear");
    check_eq_i(w.tpage_base, 0x0012u, "and the page survived");
    check_eq_i(w.clut, 0x1234u, "and the palette");
}

/* Little-endian word into a byte buffer, so the module is built the way a
 * MIPS assembler would leave it. */
static void put32(u8 *p, u32 w)
{
    p[0] = (u8)(w & 0xFF);       p[1] = (u8)((w >> 8) & 0xFF);
    p[2] = (u8)((w >> 16) & 0xFF); p[3] = (u8)((w >> 24) & 0xFF);
}

static void test_timed_beams(void)
{
    q2_fx_world w;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    s32 a[3] = { -2000, 0, 4000 }, b[3] = { 2000, 0, 4000 };
    u32 i;

    printf("beam: the timed list — the BFG's, and it outlives the frame\n");

    q2_fx_world_init(&w, &g_tab);

    check(q2_fx_beam_timed(&w, 1, 7, a, b, Q2_FX_TIMED_BEAM_RADIUS,
                           Q2_FX_TIMED_BEAM_STYLE, Q2_FX_TIMED_BEAM_LIFE),
          "a timed beam is held");
    check_eq_i(q2_fx_timed_live(&w), 1, "one is alive");

    /*
     * 0x80049D30 refreshes the record for a matching owner/target pair rather
     * than allocating a second. Without that a BFG in view of one creature
     * fills all twelve slots in twelve frames and then stops drawing — which is
     * the single easiest way to get this wrong.
     */
    for (i = 0; i < 40; i++) {
        b[0] += 10;
        q2_fx_beam_timed(&w, 1, 7, a, b, Q2_FX_TIMED_BEAM_RADIUS,
                         Q2_FX_TIMED_BEAM_STYLE, Q2_FX_TIMED_BEAM_LIFE);
    }
    check_eq_i(q2_fx_timed_live(&w), 1, "refreshing does not allocate a second");
    check_eq_i(w.timed[0].to[0], b[0], "and it moved with its target");

    /* A different target does get its own. */
    q2_fx_beam_timed(&w, 1, 8, a, b, 64, Q2_FX_TIMED_BEAM_STYLE, 45);
    check_eq_i(q2_fx_timed_live(&w), 2, "a second target takes a second slot");

    /* Twelve is the ceiling, and a full list is tolerated rather than fatal. */
    for (i = 0; i < 40; i++)
        q2_fx_beam_timed(&w, 2, (s32)i, a, b, 64, Q2_FX_TIMED_BEAM_STYLE, 45);
    check_eq_i(q2_fx_timed_live(&w), Q2_FX_TIMED_BEAMS_MAX,
               "the list holds twelve");

    /* 0x80048CE8 subtracts the frame delta and clamps, without freeing. */
    q2_fx_world_clear(&w);
    q2_fx_beam_timed(&w, 1, 7, a, b, 64, Q2_FX_TIMED_BEAM_STYLE, 45);
    q2_fx_timed_tick(&w, 20);
    check_eq_i(w.timed[0].timer, 25, "the timer takes the frame delta");
    q2_fx_timed_tick(&w, 100);
    check_eq_i(w.timed[0].timer, 0, "and clamps at zero rather than going negative");
    check_eq_i(q2_fx_timed_live(&w), 0, "an expired beam is not alive");

    /* And a live one reaches the ordering table every frame, unqueued. */
    if (psx_ot_init(&ot, 256, 4096) == Q2_OK) {
        gte_init(&gte);
        gte_set_projection(&gte, 256, 256, 124);
        memset(&cam, 0, sizeof(cam));
        cam.projection = 256;
        cam.far_z      = Q2_CAMERA_FAR_DEFAULT;

        q2_fx_world_clear(&w);
        q2_fx_beam_timed(&w, 1, 7, a, b, 64, Q2_FX_TIMED_BEAM_STYLE, 45);

        psx_ot_clear(&ot);
        check(q2_fx_build_ot(&w, &cam, 0, &ot, &gte) > 0,
              "a timed beam draws without being queued");

        /* And again next frame, from the same record. */
        q2_fx_beams_reset(&w);
        psx_ot_clear(&ot);
        check(q2_fx_build_ot(&w, &cam, 0, &ot, &gte) > 0,
              "and again the next frame");

        psx_ot_free(&ot);
    }
}

static void test_glint_script_scan(void)
{
    u8 mod[128];
    q2_fx_glint_script s;
    u32 i;

    printf("glint: the level script is read, not executed\n");

    /* Nothing in an empty module. */
    memset(mod, 0, sizeof(mod));
    check(!q2_fx_glint_scan(&s, mod, sizeof(mod)),
          "an empty module raises nothing");
    check(!q2_fx_glint_scan(&s, NULL, 0), "and a NULL one is refused");

    /*
     * A module shaped like BIGGUN's. The two immediates are deliberately NOT
     * adjacent to their stores and NOT near each other — the phase's `addiu`
     * sits five instructions before its `sh`, and the two sites are far apart —
     * because a scan that assumed either would pass on a tidier module and fail
     * on the disc, which is exactly what a first attempt here did.
     */
    memset(mod, 0, sizeof(mod));
    i = 0;
    put32(&mod[i], 0x24020006u); i += 4;   /* addiu v0, zero, 6        */
    put32(&mod[i], 0xA08202B7u); i += 4;   /* sb    v0, 0x2B7(a0)      */
    put32(&mod[i], 0x8C82010Cu); i += 4;   /* lw    v0, 0x10C(a0)      */
    put32(&mod[i], 0x3C030400u); i += 4;   /* lui   v1, 0x0400         */
    put32(&mod[i], 0x00431025u); i += 4;   /* or    v0, v0, v1  RAISE  */
    put32(&mod[i], 0xAC82010Cu); i += 4;   /* sw    v0, 0x10C(a0)      */

    i = 64;
    put32(&mod[i], 0x8C82010Cu); i += 4;   /* lw    v0, 0x10C(a0)      */
    put32(&mod[i], 0x3C030400u); i += 4;   /* lui   v1, 0x0400         */
    put32(&mod[i], 0x00431024u); i += 4;   /* and   v0, v0, v1  TEST   */
    put32(&mod[i], 0x104000ACu); i += 4;   /* beq   v0, zero, ...      */
    put32(&mod[i], 0x24020004u); i += 4;   /* addiu v0, zero, 4        */
    put32(&mod[i], 0x0000A021u); i += 4;   /* addu  s4, zero, zero     */
    put32(&mod[i], 0x908302B7u); i += 4;   /* lbu   v1, 0x2B7(a0)      */
    put32(&mod[i], 0x00000000u); i += 4;   /* nop                      */
    put32(&mod[i], 0x106000A7u); i += 4;   /* beq   v1, zero, ...      */
    put32(&mod[i], 0xA48202BEu); i += 4;   /* sh    v0, 0x2BE(a0)      */

    check(q2_fx_glint_scan(&s, mod, sizeof(mod)), "the raise is found");
    check_eq_i(s.raise_offset, 8, "at the `lw` that begins the triple");
    check_eq_i(s.band_count, 6, "the band count came off the sb's source");
    check_eq_i(s.phase, 4, "and the phase off the sh's, five back");

    /*
     * The TEST site alone must not read as a raise — `and` and `or` differ by
     * one function field, and taking either would turn the glint on for a map
     * that only ever asks whether it is on.
     */
    memset(mod, 0, sizeof(mod));
    put32(&mod[0], 0x8C82010Cu);
    put32(&mod[4], 0x3C030400u);
    put32(&mod[8], 0x00431024u);           /* and, not or */
    check(!q2_fx_glint_scan(&s, mod, sizeof(mod)),
          "a module that only tests the flag raises nothing");
}

static void test_glint_two_paths(void)
{
    static const s16 vert[6][4] = {
        { -50, 0, 0, 1024 }, { 50, 0, 0, 1024 },
        { -50, 0, 1, 3072 }, { 50, 0, 1, 3072 },
        { -50, 0, 2, 5120 }, { 50, 0, 2, 5120 }
    };
    static const u8 index[8] = { 0, 1, 2, 3,  2, 3, 4, 5 };
    q2_fx_glint g;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    s32 origin[3] = { 0, 0, 3000 };
    u32 one, many;

    printf("glint: the band count picks between two different draws\n");

    if (psx_ot_init(&ot, 256, 4096) != Q2_OK) {
        printf("  FAIL  could not allocate an ordering table\n");
        g_failures++;
        return;
    }

    gte_init(&gte);
    gte_set_projection(&gte, 256, 256, 124);
    memset(&cam, 0, sizeof(cam));
    cam.projection = 256;
    cam.far_z      = Q2_CAMERA_FAR_DEFAULT;
    /* The sort range is a separate field from the subdivision distance now, and
     * leaving it zero puts the mapping back on the fixed shift. See
     * q2_camera.sort_range. */
    cam.sort_range = Q2_CAMERA_SORT_RANGE;

    memset(&g, 0, sizeof(g));
    g.ready           = true;
    g.mesh.vert       = vert;
    g.mesh.vert_count = 6;
    g.mesh.index      = index;
    g.mesh.face_count = 2;
    g.tint[0] = g.tint[1] = g.tint[2] = 255;
    g.phase   = Q2_FX_GLINT_PHASE_START;

    /* 0x80064CE4: a zero count takes the entity's own colour and phase. */
    psx_ot_clear(&ot);
    one = q2_fx_glint_draw(&g, origin, 0, &cam, &ot, &gte);
    check(one > 0, "the single-band path drew something");

    /* A non-zero count draws once per band instead. */
    g.band_count = 3;
    {
        u32 i;
        for (i = 0; i < 3; i++) {
            g.band[i].phase  = (u8)(i + 1);
            g.band[i].colour = 0x3A0000FFu;
            g.band[i].angle[1] = (s16)(i * 100);
        }
    }
    psx_ot_clear(&ot);
    many = q2_fx_glint_draw(&g, origin, 0, &cam, &ot, &gte);
    check_eq_i(many, one * 3, "three bands draw the mesh three times");

    /* A count above the array's seven is clamped, not read off the end. */
    g.band_count = 200;
    psx_ot_clear(&ot);
    check_eq_i(q2_fx_glint_draw(&g, origin, 0, &cam, &ot, &gte),
               one * Q2_FX_GLINT_BANDS_MAX,
               "and a silly count stops at the array's seven");

    /* The two paths use DIFFERENT widths, which the shading formula turns into
     * different brightness — 0x80064D48 passes 8192 and 0x80064DFC 4096. */
    check(Q2_FX_GLINT_ONE_WIDTH == 2 * Q2_FX_GLINT_BAND_WIDTH,
          "the multi-band width is half the single-band one");

    /*
     * Advancing is a PLAIN byte decrement per band — 0x80064DEC is
     * `addiu v0, a3, 255`, with no clamp and no reload — so a phase that runs
     * past zero underflows to 255 and the band goes dark for ~250 ticks.
     * Refreshing it is the level script's job, not the draw's.
     */
    g.band_count = 3;
    g.band[0].phase = 1;
    g.band[1].phase = 4;
    q2_fx_glint_advance(&g);
    check_eq_i(g.band[0].phase, 0, "a band steps down to zero");
    check_eq_i(g.band[1].phase, 3, "and the others step with it");
    q2_fx_glint_advance(&g);
    check_eq_i(g.band[0].phase, 255, "and underflows rather than resetting");

    g.band_count = 0;
    g.phase = 3;
    q2_fx_glint_advance(&g);
    check_eq_i(g.phase, 2, "the single-band path advances its own phase");

    g.ready = false;
    psx_ot_clear(&ot);
    check_eq_i(q2_fx_glint_draw(&g, origin, 0, &cam, &ot, &gte), 0,
               "an unloaded glint draws nothing");

    psx_ot_free(&ot);
}

static void test_debris_gravity(void)
{
    q2_fx_world w;
    q2_rng rng;
    s32 at[3] = { 0, 0, 0 };
    q2_fx_debris_step step;
    int i;

    printf("debris: gravity, its clamp and its suppression flag\n");

    q2_fx_world_init(&w, &g_tab);
    q2_rng_seed(&rng, 5);
    q2_fx_debris_register(&w, 0);
    q2_fx_debris_burst(&w, &rng, NULL, NULL, at, 1, 0);

    /* 0x80046464 adds the caller's already-scaled step to vel.y. */
    w.debris[0].vel[1] = 0;
    q2_fx_debris_step_one(&w, 0, 100, &step);
    check_eq_i(w.debris[0].vel[1], 100, "one step of gravity");

    /* 0x80046490 clamps the FALL speed only. */
    w.debris[0].vel[1] = Q2_FX_TERMINAL_VELOCITY - 10;
    for (i = 0; i < 8; i++)
        q2_fx_debris_step_one(&w, 0, 100, &step);
    check_eq_i(w.debris[0].vel[1], Q2_FX_TERMINAL_VELOCITY,
               "and stops at terminal velocity");

    /* Nothing stops a piece being thrown upward faster than that — the clamp
     * is one-sided, which is what lets the burst's -1536 bias survive. */
    w.debris[0].vel[1] = -20000;
    q2_fx_debris_step_one(&w, 0, 0, &step);
    check_eq_i(w.debris[0].vel[1], -20000, "the clamp does not touch rising");

    /* 0x80046458: the flag skips gravity entirely. */
    w.debris[0].vel[1] = 0;
    w.debris[0].flags  = Q2_FX_ENT_NO_GRAVITY;
    q2_fx_debris_step_one(&w, 0, 100, &step);
    check_eq_i(w.debris[0].vel[1], 0, "the no-gravity flag suppresses it");
}

static void test_glint_phase(void)
{
    s16 band[9];
    u8  rgb[9][3];
    u8  tint[3] = { 255, 255, 255 };
    u32 i;
    s32 peak_at_start = -1, peak_at_end = -1;

    printf("glint: the phase runs 4..1 and sweeps the band forward\n");

    /* The band coordinate starts at 1024 on the tip, as it does on BIGGUN. */
    for (i = 0; i < 9; i++)
        band[i] = (s16)(1024 + i * 2048);

    /*
     * BIGGUN's script writes 4 into ent+0x2BE and the draw counts it down, so
     * phase 4 is the START. `(width/4) * (4 - phase)` therefore puts the band
     * at the tip when the phase is fresh and at the far end when it expires —
     * a formula with the subtraction reversed would sweep backwards and look
     * just as plausible standing still.
     */
    q2_fx_glint_shade(band, 9, tint, 8192, Q2_FX_GLINT_PHASE_START, rgb);
    for (i = 0; i < 9; i++) {
        (void)rgb[i][0];
    }
    {
        s32 best = -1;
        u32 where = 0;
        for (i = 0; i < 9; i++) {
            if ((s32)rgb[i][0] > best) { best = rgb[i][0]; where = i; }
        }
        peak_at_start = (s32)where;
    }

    q2_fx_glint_shade(band, 9, tint, 8192, 1, rgb);
    {
        s32 best = -1;
        u32 where = 0;
        for (i = 0; i < 9; i++) {
            if ((s32)rgb[i][0] > best) { best = rgb[i][0]; where = i; }
        }
        peak_at_end = (s32)where;
    }

    check(peak_at_start < peak_at_end,
          "phase 4 lights the tip and phase 1 the far end");
    check_eq_i(Q2_FX_GLINT_PHASE_START, 4,
               "the phase the script writes (LevelBin, sh 4, 702(a0))");
    check_eq_i(Q2_FX_GLINT_BANDS, 6,
               "and the band count (LevelBin, sb 6, 695(a0))");
}

static void test_glintmod_decode(void)
{
    /*
     * A synthetic chunk shaped like BIGGUN's: 864 bytes of indices then
     * vertices. The split is a literal in the original (0x800651C4), so a
     * decoder that read a header field instead would pass on a hand-made chunk
     * and fail on the disc — which is why the split is asserted directly.
     */
    static u8 chunk[Q2_FX_GLINT_INDEX_BYTES + 8 * 4];
    q2_fx_glint_mesh mesh;
    u32 i;

    printf("trail: the GlintMod chunk decodes\n");

    for (i = 0; i < Q2_FX_GLINT_INDEX_BYTES; i++)
        chunk[i] = (u8)(i % 4u);

    /* Four vertices, band coordinates 1024, 2048, 3072, 4096. */
    for (i = 0; i < 4; i++) {
        s16 *v = (s16 *)(void *)&chunk[Q2_FX_GLINT_INDEX_BYTES + i * 8];
        v[0] = (s16)(i * 10);
        v[1] = 0;
        v[2] = (s16)(i * 100);
        v[3] = (s16)(1024 * (i + 1));
    }

    check(q2_fx_glint_mesh_decode(&mesh, chunk, sizeof(chunk)),
          "a well-formed chunk decodes");
    check_eq_i(mesh.face_count, Q2_FX_GLINT_FACE_COUNT,
               "216 faces, from the 864-byte split");
    check_eq_i(mesh.vert_count, 4, "and the rest is vertices");
    check(mesh.index == chunk, "the faces are at the start");
    check_eq_i(mesh.vert[3][3], 4096,
               "the fourth halfword is the band coordinate");
    check_eq_i(mesh.vert[1][2], 100, "and the third is still Z");

    /* A chunk with no room past the split is refused rather than read off the
     * end, which is what the original's unconditional split would do. */
    check(!q2_fx_glint_mesh_decode(&mesh, chunk, Q2_FX_GLINT_INDEX_BYTES),
          "a chunk that is all indices is refused");
    check(!q2_fx_glint_mesh_decode(&mesh, chunk, 16),
          "and so is a short one");
}

static void test_glint_mesh(void)
{
    /*
     * A four-segment ribbon along Z, two vertices per station. The mesh is the
     * caller's because the original builds it at load time out of data this
     * project has not located; what is checked here is everything around it.
     */
    static const s16 vert[10][4] = {
        { -50, 0, 0, 1024 }, { 50, 0, 0, 1024 },
        { -50, 0, 1, 3072 }, { 50, 0, 1, 3072 },
        { -50, 0, 2, 5120 }, { 50, 0, 2, 5120 },
        { -50, 0, 3, 7168 }, { 50, 0, 3, 7168 },
        { -50, 0, 4, 9216 }, { 50, 0, 4, 9216 }
    };
    static const u8 index[16] = {
        0, 1, 2, 3,   2, 3, 4, 5,   4, 5, 6, 7,   6, 7, 8, 9
    };
    q2_fx_glint_mesh mesh;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    s32 origin[3] = { 0, 0, 3000 };
    u8  tint[3] = { 255, 200, 100 };
    u32 emitted, i;
    int lit = 0;

    printf("trail: the mesh reaches the ordering table\n");

    if (psx_ot_init(&ot, 256, 1024) != Q2_OK) {
        printf("  FAIL  could not allocate an ordering table\n");
        g_failures++;
        return;
    }

    gte_init(&gte);
    gte_set_projection(&gte, 256, 256, 124);

    memset(&cam, 0, sizeof(cam));
    cam.projection = 256;
    cam.far_z      = Q2_CAMERA_FAR_DEFAULT;
    /* The sort range is a separate field from the subdivision distance now, and
     * leaving it zero puts the mapping back on the fixed shift. See
     * q2_camera.sort_range. */
    cam.sort_range = Q2_CAMERA_SORT_RANGE;

    mesh.vert       = vert;
    mesh.vert_count = 10;
    mesh.index      = index;
    mesh.face_count = 4;

    psx_ot_clear(&ot);
    emitted = q2_fx_glint_build_ot(&mesh, origin, 0, tint, 8192, 3,
                                   &cam, &ot, &gte);
    check_eq_i(emitted, 4, "all four faces emitted");

    {
        int lo = 255, hi = -1;

        for (i = 0; i < ot.prim_count; i++) {
            const psx_prim *p = &ot.prims[i];
            int c;

            check_eq_i(p->kind, PSX_PRIM_G4, "a trail face is a gouraud quad");
            check(p->semi_transparent, "and is drawn additively");

            for (c = 0; c < 4; c++) {
                if (p->rgb[c].r < lo) lo = p->rgb[c].r;
                if (p->rgb[c].r > hi) hi = p->rgb[c].r;
            }
            if (p->rgb[0].r)
                lit++;
        }

        /*
         * A band 8192 wide covers this whole 8192-long ribbon, so every face is
         * lit — what makes it a BAND rather than a flat tint is that the
         * brightness varies along it. Asserting that some faces are dark would
         * be asserting a shorter band than the caller asked for.
         */
        check_eq_i(lit, 4, "every face is lit by an 8192-wide band");
        check(hi > lo, "but the brightness varies along the trail");
        check(hi > 0, "and the lit end is actually lit");

        /* The tint's channel order survives into the vertex colours. */
        check(ot.prims[0].rgb[0].r >= ot.prims[0].rgb[0].g &&
              ot.prims[0].rgb[0].g >= ot.prims[0].rgb[0].b,
              "the tint's channel order reached the primitive");
    }

    /* No mesh means no trail — the geometry is not invented. */
    mesh.vert = NULL;
    psx_ot_clear(&ot);
    check_eq_i(q2_fx_glint_build_ot(&mesh, origin, 0, tint, 8192, 3,
                                    &cam, &ot, &gte),
               0, "a NULL mesh draws nothing");

    psx_ot_free(&ot);
}

static void test_trail_band(void)
{
    s16 z[17];
    u8  rgb[17][3];
    u8  tint[3] = { 255, 128, 64 };
    u32 i, peak = 0;
    s32 best = -1, lit = 0;

    printf("trail: the band is triangular and travels\n");

    for (i = 0; i < 17; i++)
        z[i] = (s16)(i * 1024);

    q2_fx_glint_shade(z, 17, tint, 8192, 3, rgb);

    for (i = 0; i < 17; i++) {
        if ((s32)rgb[i][0] > best) {
            best = rgb[i][0];
            peak = i;
        }
        if (rgb[i][0])
            lit++;
    }
    check(best > 0, "something is lit");
    check(lit < 17, "but not everything — the band is bounded");

    /* Brightness must fall away from the peak on both sides: the weight is
     * `width - |z - centre|`, which is a triangle. */
    if (peak > 0)
        check(rgb[peak - 1][0] <= rgb[peak][0], "it falls off before the peak");
    if (peak + 1 < 17)
        check(rgb[peak + 1][0] <= rgb[peak][0], "and after it");

    /* The channels keep the tint's ratio. */
    check(rgb[peak][0] >= rgb[peak][1] && rgb[peak][1] >= rgb[peak][2],
          "the tint's channel order survives");

    /* A lower phase moves the band along and dims it: the centre is
     * `(width/4) * (4 - phase) + 1024`, so phase 1 sits further out. */
    {
        u8 rgb2[17][3];
        u32 peak2 = 0;
        s32 best2 = -1;

        q2_fx_glint_shade(z, 17, tint, 8192, 1, rgb2);
        for (i = 0; i < 17; i++) {
            if ((s32)rgb2[i][0] > best2) {
                best2 = rgb2[i][0];
                peak2 = i;
            }
        }
        check(peak2 > peak, "a lower phase moves the band along the trail");
        check(best2 < best, "and dims it, because the weight scales with phase");
    }
}

/* ------------------------------------------------------------------------- */
/*
 * An effect and the wall behind it must be measured against the SAME far
 * distance, or the sort between them means nothing.
 *
 * This is not a preference. The effects used a fixed `depth >> 2` while the
 * world and the models had both moved to the viewport's far_z, and against a
 * real viewport slice that shift saturates everything past about 200 units onto
 * the far end — which is the end drawn FIRST. A level's laser beams reached the
 * table (970 faces) and changed seven pixels, because every one of them was
 * sorted behind the walls it was in front of.
 *
 * So the check is a comparison, not a constant: the bucket an effect at depth d
 * lands in must be the bucket the world's own mapping gives d.
 */
/* screen.h is not on this test's include path; the table size is the point,
 * so it is named here and checked against the client's own constant by the
 * client build rather than pulled in. */
#define Q2_TEST_OT_ENTRIES 217

static void test_effect_sorts_with_the_world(void)
{
    q2_fx_world w;
    q2_rng rng;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    s32 from[3], to[3];
    u32 i;
    u32 near_bucket = 0, far_bucket = 0;
    bool got_near = false, got_far = false;

    printf("sort: an effect and the world share one depth scale\n");

    if (psx_ot_init(&ot, Q2_TEST_OT_ENTRIES, 4096) != Q2_OK) {
        printf("  FAIL  could not allocate an ordering table\n");
        g_failures++;
        return;
    }

    gte_init(&gte);
    gte_set_projection(&gte, 256, 256, 124);

    memset(&cam, 0, sizeof(cam));
    cam.projection = 256;
    cam.far_z      = Q2_CAMERA_FAR_DEFAULT;
    /* The sort range is a separate field from the subdivision distance now, and
     * leaving it zero puts the mapping back on the fixed shift. See
     * q2_camera.sort_range. */
    cam.sort_range = Q2_CAMERA_SORT_RANGE;

    q2_fx_world_init(&w, &g_tab);
    q2_rng_seed(&rng, 7);

    /*
     * Two beams down the view axis, one near and one far. Both are broadside
     * enough to project, and the only thing that separates them is depth.
     */
    from[0] = -600; from[1] = 0; from[2] = 700;
    to[0]   =  600; to[1]   = 0; to[2]   = 700;
    check(q2_fx_beam_add_style(&w, from, to, 16, 0, 0), "a near beam queued");

    from[2] = 5200;
    to[2]   = 5200;
    check(q2_fx_beam_add_style(&w, from, to, 16, 0, 0), "a far beam queued");

    psx_ot_clear(&ot);
    check(q2_fx_build_ot(&w, &cam, 0, &ot, &gte) > 0,
          "both reached the table");

    /*
     * Which bucket each primitive landed in, read off the table's own heads
     * rather than off the primitive — `psx_prim.otz` keeps the DEPTH it was
     * offered, and the whole question here is what that depth became.
     */
    /* The table holds PSX_OT_SUBDIV real buckets per console entry, so the
     * scan covers all of them or it finds an empty table. */
    for (i = 0; i < (u32)Q2_TEST_OT_ENTRIES * PSX_OT_SUBDIV; i++) {
        s32 head = ot.bucket_head[i];

        if (head < 0)
            continue;
        if (!got_far) { far_bucket = i; got_far = true; }
        near_bucket = i;
        got_near = true;
    }

    check(got_near && got_far, "both beams landed somewhere");

    /*
     * The world's own answer for those two depths, asked of the world's own
     * mapping rather than restated here.
     *
     * This used to restate it — scale by far_z into the span, then let
     * psx_ot_add invert — and that copy is exactly what made the test unable to
     * notice that the world and the effects were on two different mappings.
     * There is one now (q2_ot_bucket_for_depth), and `sort_range` is the range
     * it spans; far_z is the subdivision distance and no longer sorts anything.
     */
    {
        s32 wall_near = (s32)psx_ot_depth_bucket(
            &ot, (u16)q2_ot_bucket_for_depth(&ot, 700, cam.sort_range));
        s32 wall_far  = (s32)psx_ot_depth_bucket(
            &ot, (u16)q2_ot_bucket_for_depth(&ot, 5200, cam.sort_range));

        /*
         * Within a bucket, not exactly on one: a beam's depth is the mean of
         * its four projected corners and the hull's radius moves those by a
         * unit or two. Demanding equality would be pinning the hexagon, not
         * the sort.
         */
        check(labs((long)near_bucket - wall_near) <= 2,
              "the near beam lands where a wall at 700 would");
        check(labs((long)far_bucket - wall_far) <= 2,
              "the far beam lands where a wall at 5200 would");

        /* And the ordering: bucket 0 is drawn first, so far must be lower. */
        check(far_bucket < near_bucket,
              "the far beam is drawn before the near one");

        /*
         * The defect, as a value rather than a story. Under the fixed shift
         * this replaced, the NEAR beam's depth of 700 became otz 175 and landed
         * in bucket 41 — while the wall at the same 700 units landed in 193.
         * Bucket 41 is drawn first. The beam was in front of the wall in the
         * world and behind it on the screen, every frame, and that is exactly
         * how eleven queued beams and 970 emitted faces came to change seven
         * pixels.
         */
        check((s32)psx_ot_depth_bucket(&ot, (u16)(700u >> 2)) < wall_near,
              "the shift this replaced buried a near effect behind its wall");
    }

    psx_ot_free(&ot);
}

int main(void)
{
    printf("effect system\n\n");

    build_tables();

    test_spawn_stores_relative_velocities();
    test_size_scale();
    test_integrator_order();
    test_life_zero_frees_the_slot();
    test_pool_fills_and_refuses();
    test_ramp_is_indexed_by_age();
    test_one_ramp_defaults_to_both();
    test_budget();
    test_presets();
    test_spawn_offsets_and_puff();
    test_spawn_preset_separates();
    test_beam_pool();
    test_beam_hull();
    test_beam_hull_vertical();
    test_laser();
    test_gib_blood_colour();
    test_debris();
    test_build_ot();
    test_effect_sorts_with_the_world();
    test_texture_survives_clear();
    test_timed_beams();
    test_glint_script_scan();
    test_glint_two_paths();
    test_debris_gravity();
    test_glint_phase();
    test_glintmod_decode();
    test_glint_mesh();
    test_trail_band();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
