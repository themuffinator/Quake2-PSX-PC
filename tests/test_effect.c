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

#include "combat.h"      /* q2_actor: the presentation pass ticks its slots */
#include "effect.h"
#include "entitydraw.h"  /* q2_projectiles_build_ot: the other emitter       */
#include "itemtable.h"   /* Q2_ITEM_GLOW_*: the materialise burst's colours */
#include "projectile.h"
#include "sim.h"
#include "levelbin.h"
#include "trig.h"        /* the spark's reference velocities                */

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

    /*
     * THE REPRIEVE FIRST. The console integrates at the tail of the draw
     * (0x80030B1C onward), so a group raised between two draws is drawn before
     * it is ever stepped; this port integrates from the sim tick, ahead of the
     * draw, and gives a freshly spawned group one tick's grace to stand in for
     * that (effect.h, q2_fx_group.fresh).
     */
    q2_fx_tick(&w);
    check_eq_i(w.group[0].origin[0], 0, "the spawn tick does not integrate");
    check_eq_i(w.group[0].life,      5, "nor age it");

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

    /* The spawn tick is the reprieve — this is the tick the console spends
     * DRAWING the group, and a life-1 burst (the quad shell, the energy
     * crackle) exists only for it. */
    q2_fx_tick(&w);
    check_eq_i(w.group[0].life, 1, "the spawn tick leaves it drawable");

    q2_fx_tick(&w);
    check_eq_i(w.group[0].life, 0, "the next tick retires it");

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
    check_eq_i(q2_fx_colour_r(c), 32 - 15,
               "the spawn tick holds the first entry");

    q2_fx_tick(&w);
    c = q2_fx_group_colour(&w.group[0], 0);
    check_eq_i(q2_fx_colour_r(c), 32 - 14, "the next tick advances it");

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

    p = q2_fx_preset_at(Q2_FX_ITEM_MATERIALISE);
    check_eq_i(p->life, 10, "a materialise burst lives 10 ticks (0x80059698)");
    check_eq_i(p->size, 10000, "and is 10000 across (0x800596A0)");
    check_eq_i(p->site, 0x800596B0u,
               "and its site is inside the ITEM think 0x80059330");

    p = q2_fx_preset_at(Q2_FX_WATER_SPLASH);
    check_eq_i(p->ramp0, 0, "the splash is the blue ramp (0x8003E040)");
    check_eq_i(p->life, 25, "it lives 25 ticks (0x8003E0A0)");
    check_eq_i(p->size, 3072, "and is 3072 across (0x8003E0A8)");
    check_eq_i(p->site, 0x8003E0C0u,
               "and its site is inside GenerateWaterSplash 0x8003E014");

    /*
     * THE TELEPORT COLUMN'S SHIFT IS 13, and this row said 9 for as long as it
     * was called "scripted". `sra v0, v0, 13` at 0x80028C5C and 0x80028C88 —
     * four units of sideways drift where a 9 would have given thirty-two, which
     * is a cloud rather than a column.
     */
    p = q2_fx_preset_at(Q2_FX_TELEPORT);
    check_eq_i(p->spread_shift, 13,
               "the teleport column drifts on shift 13 (0x80028C5C)");
    check_eq_i(p->ramp0, 1, "its first ramp is index[2], red (0x8009BAE4)");
    check_eq_i(p->ramp1, 0, "and its second index[0], blue (0x8009BA60)");
    check_eq_i(p->count, 15, "fifteen quads (0x80028DA4)");

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
    check_eq_i(q2_fx_preset_at(Q2_FX_WATER_SPLASH)->repeat, 4,
               "the splash spawns four times (0x8003E0DC)");
    check_eq_i(q2_fx_preset_at(Q2_FX_ITEM_MATERIALISE)->repeat, 1,
               "the materialise site has no outer loop");
    /* Not 4: q2_fx_laser spawns its own four groups and never comes through
     * q2_fx_spawn, so a 4 here would be sixteen. */
    check_eq_i(q2_fx_preset_at(Q2_FX_LASER_END)->repeat, 1,
               "the laser end's repeat lives in q2_fx_laser, not the table");

    /* And the Y acceleration four sites write after the spawner returns. */
    check_eq_i(q2_fx_preset_at(Q2_FX_WATER_SPLASH)->accel_y, 4,
               "the splash sags at 4 (sh 4, 98(v1) at 0x8003E0D4)");
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

/*
 * The blur trail (BLUR.C). Each AddBlurPoints prepends a sample; a sample is
 * joined to the newer one beside it by a quad per neighbouring vertex pair;
 * a trail refreshed every frame keeps life + 1 samples and one that stops
 * being refreshed is gone after `life` frames. The pool is 96 points.
 */
static void test_blur_trail(void)
{
    q2_fx_world w;
    static const u8 grey[3] = { 32, 32, 32 };
    psx_ot ot;
    gte_state gte;
    q2_camera cam;
    s32 pts[2][3];
    u32 f, quads, n;

    printf("blur: AddBlurPoints and ProcessBlurs\n");

    q2_fx_world_init(&w, &g_tab);
    check_eq_i(w.blur_free_top, Q2_FX_BLUR_POINTS, "a full pool of 96");

    /* Ten frames of a two-vertex sample moving +Z, refreshed every frame. */
    for (f = 0; f < 10; f++) {
        w.blur_clock++;
        pts[0][0] = -50; pts[0][1] = 0; pts[0][2] = 3000 + (s32)f * 100;
        pts[1][0] =  50; pts[1][1] = 0; pts[1][2] = 3000 + (s32)f * 100;
        check(q2_fx_blur_add(&w, 7, 15, (const s32 (*)[3])pts, 2, grey, 6, 0),
              "the sample takes the record");
    }
    check_eq_i(w.blur[0].key, 7u | (15u << 16), "keyed on owner and vertex");
    check_eq_i(w.blur[0].span, 1, "two vertices: one quad a step");
    check_eq_i(w.stats.blur_samples, 10, "ten samples taken");

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
    cam.sort_range = Q2_CAMERA_SORT_RANGE;

    /* The draw's housekeeping cuts the chain to life + 1 = 7 samples, so 6
     * quads; and gives 20 points back. */
    quads = w.stats.blur_quads;
    (void)q2_fx_build_ot(&w, &cam, 0, &ot, &gte);
    check_eq_i(w.stats.blur_quads - quads, 6,
               "seven samples, six quads between them");
    check_eq_i(w.blur_free_top, Q2_FX_BLUR_POINTS - 14,
               "the eighth sample on is back in the pool");

    /* The newest sample is at the full colour, the next at 5/6 of it. */
    n = 0;
    {
        u32 b;
        for (b = 0; b < ot.prim_count; b++)
            if (ot.prims[b].kind == PSX_PRIM_G4 &&
                ot.prims[b].rgb[2].r == 32 && ot.prims[b].rgb[0].r == 26)
                n++;
    }
    check(n == 1, "the newest quad shades 32 at its front and 26 behind");

    /* Six frames without a refresh and the record is gone. */
    w.blur_clock += 6;
    psx_ot_clear(&ot);
    (void)q2_fx_build_ot(&w, &cam, 0, &ot, &gte);
    check_eq_i(w.blur[0].key, 0, "a trail not refreshed for its life is freed");
    check_eq_i(w.blur_free_top, Q2_FX_BLUR_POINTS, "and every point returned");

    /* An owner gone takes its trail with it the same frame. */
    w.blur_clock++;
    check(q2_fx_blur_add(&w, 9, 0, (const s32 (*)[3])pts, 2, grey, 6, 0),
          "a new trail");
    q2_fx_blur_release(&w, 9);
    check_eq_i(w.blur[0].key, 0, "released with its owner");
    psx_ot_free(&ot);
}

/*
 * The rail's spiral (0x8004917C's tail). A 3000-unit beam is three 1024-unit
 * segments, `(3000 >> 10) + 1`, and the spawn loop runs `segs & ~1` = 2 in
 * steps of two — one pass of two groups, then the last group of fifteen —
 * so three groups cover two of the three segments, as on the console.
 */
static void test_rail_trail(void)
{
    q2_fx_world w;
    static const s32 start[3] = { 100, 200, 300 };
    static const s32 end[3]   = { 100, 200, 3300 };
    static const s16 basis[2][3] = { { 4096, 0, 0 }, { 0, 4096, 0 } };
    s32 cs, sn, pstep;
    u32 n;

    printf("rail: the spiral from the muzzle\n");

    q2_fx_world_init(&w, &g_tab);
    n = q2_fx_rail_trail(&w, start, end, basis, false, 1, NULL, NULL);
    check_eq_i(n, 3, "two groups on the pass and one at the end");
    check_eq_i(w.group[0].origin[2], 300, "the pass starts at the muzzle");
    check_eq_i(w.group[1].origin[2], 300,
               "and its second fifteen share the origin");
    check_eq_i(w.group[2].origin[2], 1300,
               "the last group is one 1000-unit step on");
    check_eq_i(w.group[0].count, 15, "fifteen particles a group");
    check_eq_i(w.group[0].life, Q2_FX_RAIL_LIFE, "sixteen ticks");
    check(w.group[0].ramp[0] == q2_fx_ramp_at(&g_tab, 0) &&
          w.group[0].ramp[1] == q2_fx_ramp_at(&g_tab, 0),
          "ramp 0 at both ends, the blue one");
    check(w.group[2].size < w.group[0].size,
          "the last group is drawn at 6000, smaller than the 7000 before it");

    /* Particle 1: a twelfth of a turn round the ring (4096 / 30 = 136), the
     * ring's (cos, sin) through the basis, a quarter of it, and 1/30 of the
     * step along the beam — all then >> 4 by the spawner. */
    cs    = q2_cos12(136);
    sn    = q2_sin12(136);
    pstep = (1000 << 4) / 30;
    check_eq_i(w.group[0].offset[0][0], (s16)(cs >> 2) >> 4,
               "the ring's x is cos, a quarter, then the spawner's >> 4");
    check_eq_i(w.group[0].offset[0][1], (s16)(sn >> 2) >> 4,
               "its y is sin");
    check_eq_i(w.group[0].offset[0][2], (s16)pstep >> 4,
               "and it is one thirtieth of the segment along");
    check_eq_i(w.group[0].vel[0], 4096 >> 10,
               "particle 0 drifts outward at 4096 >> 10");

    /* Multiplayer: fifteen to a segment, so one group a pass. */
    q2_fx_world_init(&w, &g_tab);
    n = q2_fx_rail_trail(&w, start, end, basis, true, 2, NULL, NULL);
    check_eq_i(n, 2, "a multiplayer pass raises one group, then the last");

    /* A short beam is one segment: no pass, only the last group. */
    q2_fx_world_init(&w, &g_tab);
    {
        static const s32 near_end[3] = { 100, 200, 800 };
        n = q2_fx_rail_trail(&w, start, near_end, basis, false, 1,
                             NULL, NULL);
    }
    check_eq_i(n, 1, "a beam under 1024 units is the last group alone");
}

/*
 * The THIRD spawner, 0x8002FDFC: absolute world points in, raw differences
 * stored. The one instruction that separates it from 0x8003004C is the missing
 * shift, so every offset check below uses a difference whose >> 4 is a
 * different number.
 */
static void test_spawn_points(void)
{
    q2_fx_world w;
    s32 pts[4][3] = {
        { 1000, 2000, 3000 },
        { 1010, 1980, 3030 },
        { 1020, 1960, 3060 },
        { 1030, 1940, 3090 }
    };
    s16 vel[4][3] = {
        { 3, -4,  5 }, { 7, -4,  1 }, { 3,  6,  5 }, { 0,  0,  0 }
    };
    s32 slot, slot2;

    printf("group: the point-taking spawner stores raw differences\n");

    q2_fx_world_init(&w, &g_tab);
    slot = q2_fx_group_spawn_points(&w, (const s32 (*)[3])pts,
                                    (const s16 (*)[3])vel, 4,
                                    &g_tab.ramp[18], &g_tab.ramp[18],
                                    4, 32767, 9);
    check(slot >= 0, "the point spawn took a slot");

    /* 0x8002FE88: pts[0]'s three words ARE the origin. */
    check(w.group[slot].origin[0] == 1000 && w.group[slot].origin[1] == 2000 &&
          w.group[slot].origin[2] == 3000, "origin is pts[0]");

    /* 0x8002FF44 `subu` / 0x8002FF48 `sh`: no shift. Through 0x8003004C's
     * `sll 16 / sra 20` these would be 0, -2 and 1. */
    check_eq_i(w.group[slot].offset[0][0],  10, "offset[0].x is pts[1]-pts[0]");
    check_eq_i(w.group[slot].offset[0][1], -20, "offset[0].y, unshifted");
    check_eq_i(w.group[slot].offset[0][2],  30, "offset[0].z, unshifted");
    check_eq_i(w.group[slot].offset[2][0],  30, "offset[2].x is pts[3]-pts[0]");
    check_eq_i(w.group[slot].offset[2][1], -60, "offset[2].y");
    check_eq_i(w.group[slot].offset[3][0],   0, "nothing past count-1");

    /* 0x8002FEB8 / 0x8002FF84: the velocities are the usual absolute-then-
     * relative pair. */
    check_eq_i(w.group[slot].vel[0], 3, "vel[0] is absolute");
    check_eq_i(w.group[slot].rel_vel[0][0], 4, "vel[1] - vel[0]");
    check_eq_i(w.group[slot].rel_vel[1][1], 10, "vel[2] - vel[0]");

    check_eq_i(w.group[slot].count, 4, "count 4");
    check_eq_i(w.group[slot].life, 4, "life 4");
    check_eq_i(w.group[slot].area, 9, "the area byte as passed");
    check_eq_i(w.group[slot].size, 32767, "32767 at unity scale");
    check_eq_i(w.group[slot].view_mask, 0,
               "the flags byte is cleared (0x80030000)");

    /* The difference is taken in halfwords, so it wraps rather than clamps. */
    pts[1][0] = pts[0][0] + 70000;
    slot2 = q2_fx_group_spawn_points(&w, (const s32 (*)[3])pts,
                                     (const s16 (*)[3])vel, 2,
                                     &g_tab.ramp[0], NULL, 4, 32767, 0);
    check_eq_i(w.group[slot2].offset[0][0], 70000 - 65536,   /* 70000 mod 2^16 */
               "a difference beyond 16 bits wraps modulo 2^16");

    /* One point is a group with no followers. */
    q2_fx_world_clear(&w);
    slot = q2_fx_group_spawn_points(&w, (const s32 (*)[3])pts,
                                    (const s16 (*)[3])vel, 1,
                                    &g_tab.ramp[0], NULL, 4, 32767, 0);
    check_eq_i(w.group[slot].count, 1, "count 1 spawns");
    check(w.group[slot].offset[0][0] == 0 && w.group[slot].offset[0][1] == 0 &&
          w.group[slot].offset[0][2] == 0, "and writes no offset");

    /* 0x8002FFE4: the same size rule as the first spawner, toward zero. */
    q2_fx_world_clear(&w);
    w.size_scale = 256;
    slot = q2_fx_group_spawn_points(&w, (const s32 (*)[3])pts,
                                    (const s16 (*)[3])vel, 1,
                                    &g_tab.ramp[0], NULL, 4, -3, 0);
    check_eq_i(w.group[slot].size, -1, "a negative size rounds toward zero");
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
/* 0x80028BDC — DrawTelePorts, the pad column nothing in this port drew       */
/* ------------------------------------------------------------------------- */
static void test_teleport_column(void)
{
    q2_fx_world w;
    q2_rng rng;
    s16 vel[Q2_FX_GROUP_QUADS][3];
    s32 centre[3] = { 1000, -200, 3000 };
    s32 slot;
    u32 i;

    printf("teleport column\n");

    q2_fx_world_init(&w, &g_tab);
    q2_rng_seed(&rng, 1);

    /*
     * THE SAW, 0x80028C14: `LevelTicks & 0x7F` below 63 streams up, everything
     * else streams down. It is an `sltiu` against 63, so the rise is 63 ticks
     * and the fall 65 — not a symmetric split, and a port that used 64 would be
     * one tick out every cycle for ever.
     */
    check_eq_i(q2_fx_teleport_sign(0), 1, "tick 0 streams up");
    check_eq_i(q2_fx_teleport_sign(62), 1, "and tick 62 still does");
    check_eq_i(q2_fx_teleport_sign(63), -1, "tick 63 turns over");
    check_eq_i(q2_fx_teleport_sign(127), -1, "and 127 is still falling");
    check_eq_i(q2_fx_teleport_sign(128), 1, "128 wraps to the rise again");

    /*
     * The velocities. X and Z are `(rand() - 16384) >> 13`, so -2..1; Y is
     * `(rand() >> 8) * sign`, so 0..127 with NO bias subtracted — which is what
     * makes it a column and not a cloud. The generic preset draw would have
     * given all three the same shape.
     */
    q2_fx_teleport_velocities(&rng, 1, vel);
    for (i = 0; i < Q2_FX_GROUP_QUADS; i++) {
        check(vel[i][0] >= -2 && vel[i][0] <= 1, "X drifts within -2..1");
        check(vel[i][2] >= -2 && vel[i][2] <= 1, "Z drifts within -2..1");
        check(vel[i][1] >= 0 && vel[i][1] <= 127, "Y streams up, 0..127");
    }

    q2_rng_seed(&rng, 1);
    q2_fx_teleport_velocities(&rng, -1, vel);
    for (i = 0; i < Q2_FX_GROUP_QUADS; i++)
        check(vel[i][1] <= 0 && vel[i][1] >= -127, "and down on the other arm");

    /*
     * One pad's group: the jitter is ±127 about the centre in X and Z and the
     * caller's Y is untouched, the lifetime is `(rand() + 24) & 15` and the
     * size `(rand() & 0x2047) + 12000`.
     */
    slot = q2_fx_teleport_column(&w, &rng, vel, centre, 7);
    check(slot >= 0, "a pad queues a group");
    if (slot >= 0) {
        const q2_fx_group *g = &w.group[slot];

        check(g->origin[0] >= centre[0] - 127 && g->origin[0] <= centre[0] + 128,
              "X is jittered within 127 of the centre (0x80028D64)");
        check(g->origin[2] >= centre[2] - 127 && g->origin[2] <= centre[2] + 128,
              "Z likewise (0x80028D7C)");
        check_eq_i(g->origin[1], centre[1],
                   "and Y is the face the caller chose, untouched");
        check(g->life <= 15, "the lifetime is (rand()+24)&15 (0x80028D8C)");
        check(g->size >= 12000 && g->size <= 12000 + 0x2047,
              "and the size (rand()&0x2047)+12000 (0x80028DA8)");
        check_eq_i(g->area, 7, "the volume's own ID byte is the area (0x80028DC0)");
    }

    /*
     * The light, 0x80028DFC..0x80028E24: a 512-tick triangle on the level
     * clock, mirrored about 256, times four, plus 800.
     */
    check_eq_i(q2_fx_teleport_light_radius(0), 800, "the light starts at 800");
    check_eq_i(q2_fx_teleport_light_radius(256), 1824, "peaks at 1824");
    check_eq_i(q2_fx_teleport_light_radius(512), 800, "and is back at 800");
    check_eq_i(q2_fx_teleport_light_radius(384),
               q2_fx_teleport_light_radius(128),
               "the two halves of the triangle mirror");
}

/* ------------------------------------------------------------------------- */
/* 0x800596B0 — the ITEM materialise burst, which this file called a gib      */
/* ------------------------------------------------------------------------- */
static void test_item_materialise(void)
{
    q2_fx_world w;
    q2_rng rng, ref;
    s32 at[3] = { 300, -40, 900 };
    s16 want[Q2_FX_GROUP_QUADS][3];
    s32 slot;
    int i, k, bad = 0;

    printf("item: 0x800596B0 is the materialise burst, keyed by the ITEM's glow\n");

    /*
     * The three bits are the item table's own glow channels — the word
     * 0x800596B8 `andi v0, s3, 0x70` tests one instruction after the spawn —
     * and not a creature's blood colour. Pinned against itemtable.h so the two
     * names cannot drift apart.
     */
    check_eq_i(Q2_FX_ITEM_GLOW_R, Q2_ITEM_GLOW_R, "glow R is the item's 0x10");
    check_eq_i(Q2_FX_ITEM_GLOW_G, Q2_ITEM_GLOW_G, "glow G is the item's 0x20");
    check_eq_i(Q2_FX_ITEM_GLOW_B, Q2_ITEM_GLOW_B, "glow B is the item's 0x40");

    /* 0x80059648 / 0x8005965C / 0x80059670, tested in that order. */
    check_eq_i(q2_fx_item_glow_ramp(Q2_FX_ITEM_GLOW_R),  1, "R picks ramp 1");
    check_eq_i(q2_fx_item_glow_ramp(Q2_FX_ITEM_GLOW_G), 11,
               "G picks ramp 11 (0x8009C00C), not 1");
    check_eq_i(q2_fx_item_glow_ramp(Q2_FX_ITEM_GLOW_B),  0, "B picks ramp 0");
    check_eq_i(q2_fx_item_glow_ramp(Q2_ITEM_GLOW), 1, "R is tested first");
    check_eq_i(q2_fx_item_glow_ramp(Q2_FX_ITEM_GLOW_G | Q2_FX_ITEM_GLOW_B), 11,
               "then G");
    /* The materialise bit itself (0x2) is set on every item that gets here,
     * and it must not disturb the chain. */
    check_eq_i(q2_fx_item_glow_ramp(Q2_ITEM_MATERIALISE | Q2_FX_ITEM_GLOW_B), 0,
               "the materialise bit is not a colour");
    /* No glow bit leaves the original's s4 holding the caller's value. */
    check_eq_i(q2_fx_item_glow_ramp(Q2_ITEM_MATERIALISE), 1,
               "no glow bit takes the port's defined fallback");

    q2_fx_world_init(&w, &g_tab);
    q2_rng_seed(&rng, 7);
    ref = rng;

    /* 0x80059608..0x80059644: fifteen triples of (rand() - 16384) >> 9. */
    for (i = 0; i < Q2_FX_GROUP_QUADS; i++)
        for (k = 0; k < 3; k++)
            want[i][k] = (s16)((q2_rng_next(&ref) - 16384) >> 9);

    slot = q2_fx_item_materialise(&w, &rng, at, 5,
                                  Q2_ITEM_MATERIALISE | Q2_FX_ITEM_GLOW_G);
    check(slot >= 0, "the materialise burst spawned");
    check(rng.state == ref.state, "and consumed exactly 45 draws");
    check_eq_i(q2_fx_colour_g(q2_fx_group_colour(&w.group[slot], 0)), 11,
               "on the green glow's ramp");
    check_eq_i(w.group[slot].life, 10, "life 10 (0x80059698)");
    check_eq_i(w.group[slot].count, 15, "fifteen quads (0x80059690)");
    check_eq_i(w.group[slot].area, 5, "the item's own area byte");
    check(w.group[slot].origin[0] == at[0] && w.group[slot].origin[1] == at[1] &&
          w.group[slot].origin[2] == at[2],
          "at the item's origin, entity+0xA4 (0x80059680)");

    check_eq_i(w.group[slot].vel[0], want[0][0], "vel[0].x is the first draw");
    for (i = 1; i < Q2_FX_GROUP_QUADS; i++)
        for (k = 0; k < 3; k++)
            if (w.group[slot].rel_vel[i - 1][k] !=
                (s16)(want[i][k] - want[0][k]))
                bad++;
    check_eq_i(bad, 0, "every velocity is the triple drawn in x, y, z order");
}

/* ------------------------------------------------------------------------- */
/* The mesh drawers                                                           */
/*                                                                            */
/* A stub stands in for 0x8006CC44: it records every index it is asked for     */
/* and answers with a point that is a known function of the index, so both the */
/* walk and the offsets it produces are checkable exactly.                     */
/* ------------------------------------------------------------------------- */
#define STUB_LOG 512

typedef struct stub_mesh {
    u32 calls;
    s32 index[STUB_LOG];
    s32 base[3];
} stub_mesh;

static void stub_vertex(void *ctx, s32 index, s32 out[3])
{
    stub_mesh *m = (stub_mesh *)ctx;

    if (m->calls < STUB_LOG)
        m->index[m->calls] = index;
    m->calls++;
    out[0] = m->base[0] + index * 10;
    out[1] = m->base[1] - index * 20;
    out[2] = m->base[2] + index * 30;
}

static q2_fx_mesh_src stub_src(stub_mesh *m, u32 total)
{
    q2_fx_mesh_src s;

    memset(m, 0, sizeof(*m));
    m->base[0] = 100;
    m->base[1] = 200;
    m->base[2] = 4000;
    s.vertex = stub_vertex;
    s.ctx    = m;
    s.total  = total;
    return s;
}

/* How many live groups use ramp `r` with life `life`. */
static u32 count_groups(const q2_fx_world *w, u32 r, u32 life)
{
    u32 i, n = 0;

    for (i = 0; i < w->group_count; i++) {
        if (w->group[i].life == life && w->group[i].ramp[0] == &g_tab.ramp[r])
            n++;
    }
    return n;
}

static void test_mesh_crackle(void)
{
    q2_fx_world w;
    stub_mesh m;
    q2_fx_mesh_src src;
    u32 groups, i;
    int bad = 0;

    printf("mesh: the crackle walks half the mesh, alternating by frame\n");

    /*
     * 60 vertices, frame 0: s3 = 60, `slti s3, 30` fails so s2 = 15, s3 = 30;
     * still not below 30, s2 = 15, s3 = 0; the loop at 0x80059130 leaves.
     * Two full groups, vertices 0, 2, ... 58 — a single-batch reading would
     * stop at 28.
     */
    q2_fx_world_init(&w, &g_tab);
    src = stub_src(&m, 60);
    groups = q2_fx_mesh_crackle(&w, &src, 0, 3, -1,
                                Q2_FX_CRACKLE_DAMAGE_RAMP,
                                Q2_FX_CRACKLE_DAMAGE_LIFE);
    check_eq_i(groups, 2, "60 vertices on an even frame: two groups");
    check_eq_i(m.calls, 30, "sampling thirty vertices");
    for (i = 0; i < m.calls && i < STUB_LOG; i++)
        if (m.index[i] != (s32)(2 * i))
            bad++;
    check_eq_i(bad, 0, "and they are 0, 2, 4 ... 58");
    check_eq_i(w.group[0].count, 15, "first batch 15");
    check_eq_i(w.group[1].count, 15, "second batch 15");

    /* The group is built from the samples: origin is the first point, the
     * followers are raw differences, and nothing moves. */
    check(w.group[0].origin[0] == 100 && w.group[0].origin[1] == 200 &&
          w.group[0].origin[2] == 4000, "origin is vertex 0's world point");
    check_eq_i(w.group[0].offset[0][0], 20, "follower 1 is vertex 2, +20 in x");
    check_eq_i(w.group[0].offset[0][1], -40, "and -40 in y, unshifted");
    check_eq_i(w.group[0].offset[13][2], 840, "follower 14 is vertex 28");
    check_eq_i(w.group[1].origin[0], 100 + 30 * 10,
               "the second group starts at vertex 30");
    bad = 0;
    for (i = 0; i < Q2_FX_GROUP_FOLLOWERS; i++)
        if (w.group[0].rel_vel[i][0] || w.group[0].rel_vel[i][1] ||
            w.group[0].rel_vel[i][2])
            bad++;
    check(bad == 0 && w.group[0].vel[0] == 0 && w.group[0].vel[1] == 0 &&
          w.group[0].vel[2] == 0,
          "every velocity is zero (0x80058FF8's fifteen memsets)");
    check_eq_i(w.group[0].life, 4, "damage crackle life 4");
    check(w.group[0].ramp[0] == &g_tab.ramp[18] &&
          w.group[0].ramp[1] == &g_tab.ramp[18], "on ramp 18, passed twice");
    check_eq_i(w.group[0].size, 32767, "size 32767");
    check_eq_i(w.group[0].area, 3, "the entity's area byte");
    check_eq_i(w.group[0].view_mask, 0, "no client, no skip");

    /* Frame 1: s3 = 59 -> 15 (29 left) -> 14 (1 left). The other half. */
    q2_fx_world_clear(&w);
    src = stub_src(&m, 60);
    groups = q2_fx_mesh_crackle(&w, &src, 1, 3, -1, 18, 4);
    check_eq_i(groups, 2, "60 vertices on an odd frame: two groups");
    check_eq_i(w.group[1].count, 14, "the second one of fourteen");
    bad = 0;
    for (i = 0; i < m.calls && i < STUB_LOG; i++)
        if (m.index[i] != (s32)(2 * i + 1))
            bad++;
    check_eq_i(bad, 0, "starting at vertex 1: the phase is a start, not a skip");
    check_eq_i(m.calls, 29, "vertices 1, 3 ... 57");

    /* 33 vertices, frame 0: 15 (3 left), then 3 >> 1 = 1 (1 left). */
    q2_fx_world_clear(&w);
    src = stub_src(&m, 33);
    check_eq_i(q2_fx_mesh_crackle(&w, &src, 0, 0, -1, 18, 4), 2,
               "33 vertices: two groups");
    check_eq_i(w.group[1].count, 1, "the tail group holds one quad");
    check_eq_i(m.index[15], 30, "and it is vertex 30");

    /* The player index lands in the HIGH nibble (0x8005911C..0x80059128). */
    q2_fx_world_clear(&w);
    src = stub_src(&m, 4);
    q2_fx_mesh_crackle(&w, &src, 0, 0, 2, 18, 4);
    check_eq_i(w.group[0].view_mask, 0x40, "player 2 hides it from viewport 2");

    /* Model-less: 0x8006D6AC is zero, `0 - parity` is below two, leave. */
    q2_fx_world_clear(&w);
    check_eq_i(q2_fx_mesh_crackle(&w, NULL, 0, 0, -1, 18, 4), 0,
               "a model-less entity spawns nothing");
    check_eq_i(w.group[0].life, 0, "and the pool is untouched");
    src = stub_src(&m, 2);
    check_eq_i(q2_fx_mesh_crackle(&w, &src, 1, 0, -1, 18, 4), 0,
               "two vertices on an odd frame leave one, which is too few");
}

/*
 * The renderer honours the nibble the drawers write. With the low-nibble test
 * this replaced, a crackle marked for player 1 was drawn in viewport 1 too.
 */
static void test_view_mask_skips_one_viewport(void)
{
    q2_fx_world w;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    stub_mesh m;
    q2_fx_mesh_src src;

    printf("draw: a crackle is hidden in its own player's viewport only\n");

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
    cam.sort_range = Q2_CAMERA_SORT_RANGE;

    q2_fx_world_init(&w, &g_tab);
    q2_fx_world_resize(&w, 0, 4);
    src = stub_src(&m, 4);
    check_eq_i(q2_fx_mesh_crackle(&w, &src, 0, 0, 1, 18, 4), 1,
               "one crackle group in front of the camera");
    check_eq_i(w.group[0].view_mask, 0x20, "marked for player 1");

    psx_ot_clear(&ot);
    check(q2_fx_build_ot(&w, &cam, 0, &ot, &gte) > 0, "viewport 0 draws it");
    psx_ot_clear(&ot);
    check_eq_i(q2_fx_build_ot(&w, &cam, 1, &ot, &gte), 0,
               "viewport 1 skips it (bit 4 + 1, 0x80030620)");
    psx_ot_clear(&ot);
    check(q2_fx_build_ot(&w, &cam, 2, &ot, &gte) > 0, "viewport 2 draws it");

    psx_ot_free(&ot);
}

/*
 * The creature muzzle flash, 0x80031094: a square of half-diagonal 120 turned
 * by gp+16996 in model space, one additive quad of the chars page, dropped
 * nearer than 257 and cleared every frame.
 */
static void test_creature_flash(void)
{
    q2_fx_world w;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    s32 centre[3] = { 10, 20, 30 };
    s32 corner[4][3];
    s32 at[3] = { 0, 0, 2000 };

    printf("draw: a creature's muzzle flash is one spinning additive quad\n");

    check_eq_i(q2_fx_flash_angle(1000, 1), 1000 & 0xFFF,
               "an odd frame reads the spin itself (0x80031108)");
    check_eq_i(q2_fx_flash_angle(1000, 2), (1000 - 128) & 0xFFF,
               "an even frame reads it 128 behind (0x8003111C)");

    q2_fx_flash_corners(centre, 0, Q2_FX_FLASH_SIZE, corner);
    check(corner[0][0] == 10 - 120 && corner[0][1] == 20 &&
              corner[1][0] == 10 && corner[1][1] == 20 - 120 &&
              corner[2][0] == 10 && corner[2][1] == 20 + 120 &&
              corner[3][0] == 10 + 120 && corner[3][1] == 20 &&
              corner[0][2] == 30 && corner[3][2] == 30,
          "at turn 0 the corners are left, top, bottom, right at the "
          "muzzle's own depth (0x800312D4..0x8003131C)");

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
    cam.sort_range = Q2_CAMERA_SORT_RANGE;

    q2_fx_world_init(&w, &g_tab);
    q2_fx_set_texture(&w, 0x10, 0x1234);
    q2_fx_flash_corners(at, 300, Q2_FX_FLASH_SIZE, corner);
    check(q2_fx_flash_add(&w, (const s32 (*)[3])corner, at, 0),
          "a flash is queued");

    psx_ot_clear(&ot);
    check_eq_i(q2_fx_build_ot(&w, &cam, 0, &ot, &gte), 1,
               "and drawn as exactly one primitive");
    check_eq_i(w.stats.flashes_drawn, 1, "counted as a flash");

    {
        u32 i, viewport;
        q2_fx_flash_clear(&w);
        q2_fx_world_resize(&w, 0, 4);
        /* More simultaneous actors than dynamic-light slots: retail's
         * per-actor draw must not silently lose the seventeenth sprite. */
        for (i = 0; i < 65; i++)
            check(q2_fx_flash_add(&w, (const s32 (*)[3])corner, at, 0),
                  "crowded volley keeps every flash");
        for (viewport = 0; viewport < 4; viewport++) {
            psx_ot_clear(&ot);
            check_eq_i(q2_fx_build_ot(&w, &cam, viewport, &ot, &gte), 65,
                       "every split viewport sees the entire volley");
        }
        q2_fx_world_clear(&w);
        check(w.flash_extra == NULL && w.flash_count == 0,
              "level clear releases overflow flash storage");
    }

    q2_fx_flash_clear(&w);
    psx_ot_clear(&ot);
    check_eq_i(q2_fx_build_ot(&w, &cam, 0, &ot, &gte), 0,
               "and gone once the frame's flag is dropped (0x8007EE20)");

    at[2] = 200;
    q2_fx_flash_corners(at, 300, Q2_FX_FLASH_SIZE, corner);
    q2_fx_flash_add(&w, (const s32 (*)[3])corner, at, 0);
    psx_ot_clear(&ot);
    check_eq_i(q2_fx_build_ot(&w, &cam, 0, &ot, &gte), 0,
               "a muzzle nearer than 257 is not drawn (0x80031470)");

    q2_fx_flash_corners(centre, 0, 240, corner);
    check(corner[0][0] == 10 - 240 && corner[1][1] == 20 - 240 &&
          corner[2][1] == 20 + 240 && corner[3][0] == 10 + 240,
          "Boss1 export 1 supplies a 240-unit flash");
    q2_fx_flash_clear(&w);

    psx_ot_free(&ot);
}

static void test_mesh_spark(void)
{
    q2_fx_world w;
    q2_rng rng, ref;
    stub_mesh m;
    q2_fx_mesh_src src;
    s16 want[Q2_FX_GROUP_QUADS][3];
    s32 t;
    u32 i;
    int k, bad = 0;

    printf("mesh: the spark draws 45 velocities and walks every eighth vertex\n");

    /* 0x80058A1C..0x80058A38: 32 + (12x >> 13). */
    check_eq_i(q2_fx_spark_mag(0), 8, "the slowest spark is 8");
    check_eq_i(q2_fx_spark_mag(32767), 55, "the fastest is 55");
    check_eq_i(q2_fx_spark_mag(16384), 32, "a centred draw is 32");
    check_eq_i(q2_fx_spark_mag(16383), 31, "and the shift floors below it");

    /* f(t) is the signed magic divide by 12000. */
    check_eq_i(q2_fx_spark_scale(4096 * 55), 18, "f(+4096*55) = 18");
    check_eq_i(q2_fx_spark_scale(-4096 * 55), -18, "f(-4096*55) = -18");
    check_eq_i(q2_fx_spark_scale(11999), 0, "11999 is below one step");
    check_eq_i(q2_fx_spark_scale(12000), 1, "12000 is exactly one");
    check_eq_i(q2_fx_spark_scale(-11999), 0, "and it truncates toward zero");
    check_eq_i(q2_fx_spark_scale(-12000), -1, "-12000 is exactly minus one");
    for (t = -230000; t <= 230000; t++)
        if (q2_fx_spark_scale(t) != t / 12000)
            bad++;
    check_eq_i(bad, 0, "f(t) == t / 12000 over the site's whole reach");

    /*
     * 40 vertices, frame 3: s5 = 37, `slti s5, 120` holds so s1 = 37 >> 3 = 4,
     * s5 = 5, below eight, done. One group of FOUR: 3, 11, 19, 27. Vertex 35
     * would need s5 >= 8 on the second pass.
     */
    q2_fx_world_init(&w, &g_tab);
    q2_rng_seed(&rng, 99);
    ref = rng;
    for (i = 0; i < Q2_FX_GROUP_QUADS; i++) {
        s32 a = q2_rng_next(&ref);
        s32 b = q2_rng_next(&ref);
        s32 angle = a & 0xFFF;          /* A, not B: the delay slot */
        s32 mag = 32 + ((12 * (b - 16384)) >> 13);
        s32 c;

        want[i][0] = (s16)((q2_sin12(angle) * mag) / 12000);
        c = q2_rng_next(&ref);
        want[i][1] = (s16)((c - 16384) >> 10);
        want[i][2] = (s16)((q2_cos12(angle) * mag) / 12000);
    }

    src = stub_src(&m, 40);
    check_eq_i(q2_fx_mesh_spark(&w, &rng, &src, 3, 0, 0,
                                &g_tab.ramp[14], &g_tab.ramp[14],
                                Q2_FX_MESH_SPARK_SIZE, Q2_FX_MESH_SPARK_LIFE),
               1, "40 vertices at frame 3: one group");
    check(rng.state == ref.state, "exactly 45 draws, A B C per quad");
    check_eq_i(w.group[0].count, 4, "of four quads, not five");
    check(m.calls == 4 && m.index[0] == 3 && m.index[1] == 11 &&
          m.index[2] == 19 && m.index[3] == 27, "vertices 3, 11, 19, 27");
    check_eq_i(w.group[0].offset[0][0], 80, "follower 1 is vertex 11, +80");
    check_eq_i(w.group[0].life, 4, "life 4 — the wrappers' fifth argument");
    check_eq_i(w.group[0].view_mask, 0x10, "player 0 hides it from viewport 0");

    bad = 0;
    for (k = 0; k < 3; k++)
        if (w.group[0].vel[k] != want[0][k])
            bad++;
    for (i = 1; i < 4; i++)
        for (k = 0; k < 3; k++)
            if (w.group[0].rel_vel[i - 1][k] != (s16)(want[i][k] - want[0][k]))
                bad++;
    check_eq_i(bad, 0,
               "velocities: sine into x, the third draw into y, cosine into z");

    /* 200 vertices at frame 0: 15 (80 left), then 80 >> 3 = 10. */
    q2_fx_world_clear(&w);
    src = stub_src(&m, 200);
    check_eq_i(q2_fx_mesh_spark(&w, &rng, &src, 0, 0, -1,
                                &g_tab.ramp[14], NULL, 32767, 4), 2,
               "200 vertices: two groups");
    check_eq_i(w.group[1].count, 10, "the second of ten");
    check_eq_i(m.index[15], 120, "and it starts at vertex 120");

    /* 0x800589E0 leaves on a null model BEFORE the velocity loop. */
    ref = rng;
    q2_fx_world_clear(&w);
    check_eq_i(q2_fx_mesh_spark(&w, &rng, NULL, 0, 0, -1,
                                &g_tab.ramp[14], NULL, 32767, 4), 0,
               "a model-less entity spawns nothing");
    check(rng.state == ref.state, "and costs no draws at all");

    /* A model with too few vertices is NOT that case: the draws happen. */
    src = stub_src(&m, 7);
    check_eq_i(q2_fx_mesh_spark(&w, &rng, &src, 0, 0, -1,
                                &g_tab.ramp[14], NULL, 32767, 4), 0,
               "seven vertices are fewer than one step");
    for (i = 0; i < 45; i++)
        (void)q2_rng_next(&ref);
    check(rng.state == ref.state, "but the 45 draws were still made");
}

static void test_mesh_blood(void)
{
    q2_fx_world w;
    q2_rng rng, ref;
    stub_mesh m;
    q2_fx_mesh_src src;
    s16 want[Q2_FX_GROUP_QUADS][3];
    u32 i;
    int k, bad = 0, negative = 0, positive = 0;

    printf("mesh: the blood spray tiles the mesh and rounds toward zero\n");

    /*
     * Mode 0 (0x8005B6C0's): 3 * (rand() - 16384), then 0x8005AC58's
     * `bgez; addiu 16383; sra 14` — the bias only for a NEGATIVE product.
     * C's `/` is that operation.
     */
    q2_fx_world_init(&w, &g_tab);
    q2_rng_seed(&rng, 31337);
    ref = rng;
    for (i = 0; i < Q2_FX_GROUP_QUADS; i++)
        for (k = 0; k < 3; k++) {
            s32 v = 3 * (q2_rng_next(&ref) - 16384);
            want[i][k] = (s16)(v / 16384);
            if (v < 0 && v % 16384) negative++;
            if (v > 0 && v % 16384) positive++;
        }
    check(negative > 0 && positive > 0,
          "the seed exercises both signs off a multiple of 16384");

    src = stub_src(&m, 31);
    check_eq_i(q2_fx_mesh_blood(&w, &rng, &src, 6,
                                &g_tab.ramp[10], &g_tab.ramp[0], 6144, 0), 2,
               "31 vertices: 31 / 15 = two groups");
    check(rng.state == ref.state, "one set of 45 draws for the whole spray");
    check_eq_i(m.calls, 30, "sampling vertices 0..29");
    check(m.index[0] == 0 && m.index[14] == 14 && m.index[15] == 15 &&
          m.index[29] == 29, "consecutively, the second group from 15");
    check_eq_i(w.group[0].life, 32, "mode 0 lives 32 (0x8005AC34)");
    check_eq_i(w.group[0].accel[1], 0, "and does not sag");
    check(w.group[0].ramp[0] == &g_tab.ramp[10] &&
          w.group[0].ramp[1] == &g_tab.ramp[0], "ramps as passed");

    for (i = 0; i < 2; i++) {
        const q2_fx_group *g = &w.group[i];
        u32 q;
        for (k = 0; k < 3; k++)
            if (g->vel[k] != want[0][k])
                bad++;
        for (q = 1; q < Q2_FX_GROUP_QUADS; q++)
            for (k = 0; k < 3; k++)
                if (g->rel_vel[q - 1][k] != (s16)(want[q][k] - want[0][k]))
                    bad++;
    }
    check_eq_i(bad, 0, "both groups carry the same toward-zero velocities");

    /* Mode 1 is the gib spray: 0x8005B320, life 10, >> 10, accel 3. */
    q2_fx_world_clear(&w);
    ref = rng;
    for (i = 0; i < Q2_FX_GROUP_QUADS; i++)
        for (k = 0; k < 3; k++)
            want[i][k] = (s16)((q2_rng_next(&ref) - 16384) >> 10);
    src = stub_src(&m, 15);
    check_eq_i(q2_fx_gib_spray(&w, &rng, &src, 4), 1,
               "the gib spray over 15 vertices is one group");
    check(rng.state == ref.state, "45 draws");
    check_eq_i(w.group[0].life, 10, "mode 1 lives 10, not 1 (0x8005ABE0)");
    check_eq_i(w.group[0].accel[1], 3, "and sags at 3 (0x8005AD48)");
    check(w.group[0].ramp[0] == &g_tab.ramp[2] &&
          w.group[0].ramp[1] == &g_tab.ramp[3], "on the blood ramps 2 and 3");
    check_eq_i(w.group[0].size, 6144, "size 6144");
    check_eq_i(w.group[0].area, 4, "the entity's area");
    check_eq_i(w.group[0].vel[1], want[0][1], "vel.y is the second draw >> 10");
    check_eq_i(w.group[0].view_mask, 0, "no view mask: visible to its owner");

    /*
     * A NULL model is not an exit: 0x8006D6AC returns zero, the batch count is
     * zero, and the fifteen triples are drawn anyway (0x8005ABE4, before the
     * test at 0x8005ACCC).
     */
    q2_fx_world_clear(&w);
    ref = rng;
    check_eq_i(q2_fx_gib_spray(&w, &rng, NULL, 0), 0,
               "a model-less spray spawns nothing");
    for (i = 0; i < 45; i++)
        (void)q2_rng_next(&ref);
    check(rng.state == ref.state, "but still costs its 45 draws");
}

/*
 * The five countdowns, as 0x8005B880 runs them. Only slots 0 and 2 are gated
 * on the entity being alive; 4 and 5 get a literal 1; effect[1] ticks itself.
 */
static void test_actor_present_tickers(void)
{
    q2_fx_world w;
    q2_rng rng;
    q2_actor a;
    stub_mesh m;
    q2_fx_mesh_src src;
    q2_fx_present_report rep;
    u32 fired, i;

    printf("present: the tickers' clocks and gates\n");

    q2_fx_world_init(&w, &g_tab);
    q2_rng_seed(&rng, 5);
    src = stub_src(&m, 16);

    /* A CORPSE with effect[0] armed: dt is (health > 0) = 0, so it sparks
     * every present and never runs down. */
    q2_actor_init(&a);
    a.health = 0;
    a.effect[0] = 15;
    for (fired = 0, i = 0; i < 20; i++) {
        q2_fx_world_clear(&w);
        q2_fx_actor_present(&w, &rng, &a, &src, i, 0, -1, 0, 0, &rep);
        fired += count_groups(&w, Q2_FX_MESH_SPARK_RAMP, Q2_FX_MESH_SPARK_LIFE);
    }
    check_eq_i(fired, 20, "a dead body's effect[0] sparks every present");
    check_eq_i(a.effect[0], 15, "and never counts down (0x8005B8AC)");

    /* 0x8005B624/58/8C pass ONE ramp pointer twice, so the renderer's
     * three-quad swap (0x80030A14) has nothing to alternate. */
    check(w.group[0].ramp[0] == &g_tab.ramp[Q2_FX_MESH_SPARK_RAMP] &&
          w.group[0].ramp[1] == w.group[0].ramp[0],
          "the spark's two ramps are the same record, ramp 14");
    check_eq_i(w.group[0].size, Q2_FX_MESH_SPARK_SIZE, "size 32767");

    /* Alive: fifteen sparks, then silence. */
    a.health = 10;
    for (fired = 0, i = 0; i < 20; i++) {
        q2_fx_world_clear(&w);
        q2_fx_actor_present(&w, &rng, &a, &src, i, 0, -1, 0, 0, &rep);
        fired += count_groups(&w, Q2_FX_MESH_SPARK_RAMP, Q2_FX_MESH_SPARK_LIFE);
        if (i == 14)
            check_eq_i(a.effect[0], 0, "alive, it reaches 0 on the 15th");
    }
    check_eq_i(fired, 15, "alive, effect[0] sparks fifteen times");

    /* effect[2] is gated the same way. */
    q2_actor_init(&a);
    a.health = 0;
    a.effect[2] = 30;
    q2_fx_actor_present(&w, &rng, &a, &src, 0, 0, -1, 0, 0, &rep);
    check_eq_i(a.effect[2], 30, "a dead body holds effect[2] too");

    /* effect[4] is NOT: both drivers pass `addiu a1, zero, 1` (0x8005B3EC,
     * 0x8005B8D0), so it runs down on a corpse. */
    q2_actor_init(&a);
    a.health = 0;
    a.effect[4] = 5;
    for (fired = 0, i = 0; i < 8; i++) {
        q2_fx_world_clear(&w);
        q2_fx_actor_present(&w, &rng, &a, &src, i, 0, -1, 0, 0, &rep);
        fired += count_groups(&w, Q2_FX_MESH_SPARK_RAMP, Q2_FX_MESH_SPARK_LIFE);
    }
    check_eq_i(fired, 5, "effect[4] sparks five times on a corpse");
    check_eq_i(a.effect[4], 0, "and runs out");

    /* effect[5]: the crackle on ramp 0, life 4, also a literal 1. */
    q2_actor_init(&a);
    a.health = 0;
    a.effect[5] = 3;
    for (fired = 0, i = 0; i < 6; i++) {
        q2_fx_world_clear(&w);
        q2_fx_actor_present(&w, &rng, &a, &src, i, 0, -1, 0, 0, &rep);
        fired += count_groups(&w, Q2_FX_CRACKLE_SLOT5_RAMP,
                              Q2_FX_CRACKLE_SLOT5_LIFE);
    }
    check_eq_i(fired, 3, "effect[5] crackles on ramp 0 three times");
    check_eq_i(a.effect[5], 0, "and runs out on a corpse");

    /* effect[3] has no reader and no writer on the disc. */
    q2_actor_init(&a);
    a.health = 10;
    a.effect[3] = 7;
    for (i = 0; i < 4; i++)
        q2_fx_actor_present(&w, &rng, &a, &src, i, 0, -1, 0, 0, &rep);
    check_eq_i(a.effect[3], 7, "effect[3] is never touched");

    /* The spark's cost is the generator's: 45 draws per firing with a model,
     * none without one. */
    {
        q2_rng ref;

        q2_actor_init(&a);
        a.health = 10;
        a.effect[0] = 2;
        ref = rng;
        q2_fx_world_clear(&w);
        q2_fx_actor_present(&w, &rng, &a, NULL, 0, 0, -1, 0, 0, &rep);
        check(rng.state == ref.state, "no model: no draws");
        check_eq_i(a.effect[0], 1, "but the timer still runs");
        q2_fx_actor_present(&w, &rng, &a, &src, 0, 0, -1, 0, 0, &rep);
        for (i = 0; i < 45; i++)
            (void)q2_rng_next(&ref);
        check(rng.state == ref.state, "a model: 45 draws for the one spark");
    }
}

/*
 * 0x80058638. An energy hit armed at 3 plays out over three presents, and the
 * `< 3` arm has a light of its own at exactly 2.
 */
static void test_actor_damage_effect(void)
{
    q2_fx_world w;
    q2_rng rng;
    q2_actor a;
    stub_mesh m;
    q2_fx_mesh_src src;
    q2_fx_present_report rep;

    printf("present: effect[1] ticks itself and has two lights\n");

    q2_fx_world_init(&w, &g_tab);
    q2_rng_seed(&rng, 11);
    src = stub_src(&m, 4);
    q2_actor_init(&a);
    a.health = 100;
    a.effect[1] = 3;

    q2_fx_actor_present(&w, &rng, &a, &src, 0, 0, -1, 0, 0, &rep);
    check_eq_i(count_groups(&w, Q2_FX_CRACKLE_ENERGY_RAMP,
                            Q2_FX_CRACKLE_ENERGY_LIFE), 1,
               "tick 1: the energy burst, ramp 17 life 1");
    check(rep.energy_light && rep.set_ambient && !rep.energy_pulse,
          "tick 1: the persistent light and the ambient override");
    check_eq_i(a.effect[1], 2, "tick 1 leaves 2");
    q2_fx_tick(&w);
    check_eq_i(w.group[0].life, 1,
               "the spawn tick leaves the life-1 burst drawable");
    q2_fx_tick(&w);
    check_eq_i(w.group[0].life, 0,
               "and the next integrator tick retires it");

    q2_fx_world_clear(&w);
    q2_fx_actor_present(&w, &rng, &a, &src, 1, 0, -1, 0, 0, &rep);
    check_eq_i(count_groups(&w, Q2_FX_CRACKLE_DAMAGE_RAMP,
                            Q2_FX_CRACKLE_DAMAGE_LIFE), 1,
               "tick 2: the damage crackle, ramp 18 life 4");
    check(rep.energy_pulse && !rep.energy_light && !rep.set_ambient,
          "tick 2: the OTHER light, 0x80075D14, and only it");
    check_eq_i(a.effect[1], 1, "tick 2 leaves 1");

    q2_fx_world_clear(&w);
    q2_fx_actor_present(&w, &rng, &a, &src, 2, 0, -1, 0, 0, &rep);
    check_eq_i(count_groups(&w, Q2_FX_CRACKLE_DAMAGE_RAMP,
                            Q2_FX_CRACKLE_DAMAGE_LIFE), 1,
               "tick 3: the crackle again");
    check(!rep.energy_pulse && !rep.energy_light, "tick 3: no light");
    check_eq_i(a.effect[1], 0, "tick 3 leaves 0");

    q2_fx_world_clear(&w);
    q2_fx_actor_present(&w, &rng, &a, &src, 3, 0, -1, 0, 0, &rep);
    check_eq_i(rep.groups, 0, "tick 4: nothing");
    check_eq_i(a.effect[1], 0, "and the byte does not wrap to 255");

    /* The driver passes a literal 1 here too, so a corpse's slot runs down. */
    a.health = 0;
    a.effect[1] = 2;
    q2_fx_actor_present(&w, &rng, &a, NULL, 0, 0, -1, 0, 0, &rep);
    check_eq_i(a.effect[1], 1, "effect[1] counts down on a corpse");
    check(rep.energy_pulse, "and reports its pulse with no model");

    /* The dispatcher on its own takes the caller's dt, unclamped. */
    a.effect[1] = 2;
    q2_fx_actor_damage_effect(&w, &a, NULL, 0, 0, -1, 3, &rep);
    check_eq_i(a.effect[1], 255, "a dt past the slot wraps, as `sb` does");
}

/*
 * 0x8005B7E4 and 0x80058C18. Client-gated, an unsigned strict compare, and a
 * one-tick shell on ramp 15.
 */
static void test_quad_shell(void)
{
    q2_fx_world w;
    q2_rng rng;
    q2_actor a;
    stub_mesh m;
    q2_fx_mesh_src src;
    q2_fx_present_report rep;
    u32 i;
    int even = 0, odd = 0;

    printf("present: the quad shell\n");

    q2_fx_world_init(&w, &g_tab);
    q2_rng_seed(&rng, 3);
    q2_actor_init(&a);
    a.health = 100;
    a.has_client = true;

    /* 21 vertices: frame 0 walks 0..18 (21 >> 1 = 10, one left over) and
     * frame 1 walks 1..19 (20 >> 1 = 10). */
    src = stub_src(&m, 21);
    q2_fx_actor_present(&w, &rng, &a, &src, 0, 0, 0, 99, 100, &rep);
    check_eq_i(count_groups(&w, Q2_FX_QUAD_SHELL_RAMP, Q2_FX_QUAD_SHELL_LIFE),
               1, "one tick before the deadline: a ramp-15, life-1 shell");
    for (i = 0; i < m.calls; i++) {
        if (m.index[i] & 1)
            odd++;
        else
            even++;
    }
    check(even == 10 && odd == 0, "frame 0 samples the even half");

    /* The next frame takes the other half, and the old shell is gone after
     * one integrator tick because its life is 1. */
    q2_fx_tick(&w);
    check_eq_i(count_groups(&w, Q2_FX_QUAD_SHELL_RAMP, Q2_FX_QUAD_SHELL_LIFE),
               1, "the spawn tick leaves the shell standing to be drawn");
    q2_fx_tick(&w);
    check_eq_i(count_groups(&w, Q2_FX_QUAD_SHELL_RAMP, Q2_FX_QUAD_SHELL_LIFE),
               0, "a tick later the shell has expired");
    src = stub_src(&m, 21);
    even = odd = 0;
    q2_fx_actor_present(&w, &rng, &a, &src, 1, 0, 0, 99, 100, &rep);
    for (i = 0; i < m.calls; i++) {
        if (m.index[i] & 1)
            odd++;
        else
            even++;
    }
    check(odd == 10 && even == 0, "frame 1 samples the odd half");

    q2_fx_world_clear(&w);
    q2_fx_actor_present(&w, &rng, &a, &src, 0, 0, 0, 100, 100, &rep);
    check_eq_i(rep.groups, 0, "at the deadline it is already over (strict)");

    /* `sltu`: a deadline stored negative reads as huge and the shell holds. */
    q2_fx_actor_present(&w, &rng, &a, &src, 0, 0, 0, 10, -5, &rep);
    check_eq_i(rep.groups, 1, "the compare is unsigned (0x8005B80C)");

    /* No client, no shell, whatever the inventory says. */
    a.has_client = false;
    q2_fx_world_clear(&w);
    q2_fx_actor_present(&w, &rng, &a, &src, 0, 0, 0, 0, 100, &rep);
    check_eq_i(rep.groups, 0, "an actor with no client never gets a shell");
}

/* The particle half of the gib think, 0x80059DE0. */
static void test_gib_trail(void)
{
    q2_fx_world w;
    q2_rng rng, ref;
    s32 at[3] = { -500, 60, 7000 };
    s16 vel[3] = { 150, -301, 75 };
    s16 want[Q2_FX_GIB_TRAIL_COUNT][3];
    s32 slot;
    u32 i;
    int k, bad = 0;

    printf("gib: the blood trail is a line along the gib's own motion\n");

    q2_fx_world_init(&w, &g_tab);
    q2_rng_seed(&rng, 2718);
    ref = rng;
    for (i = 0; i < Q2_FX_GIB_TRAIL_COUNT; i++)
        for (k = 0; k < 3; k++)
            want[i][k] = (s16)((q2_rng_next(&ref) + (4 * vel[k] - 8192)) >> 11);

    slot = q2_fx_gib_trail(&w, &rng, at, vel, 12);
    check(slot >= 0, "the trail spawned");
    check(rng.state == ref.state, "45 draws per gib per tick");
    check_eq_i(w.group[slot].count, 15, "fifteen quads");
    check_eq_i(w.group[slot].life, 3, "life 3 (0x8005A060)");
    check_eq_i(w.group[slot].size, 6144, "size 6144");
    check_eq_i(w.group[slot].area, 12, "the gib's area");
    check(w.group[slot].ramp[0] == &g_tab.ramp[2] &&
          w.group[slot].ramp[1] == &g_tab.ramp[3], "ramps 2 and 3");
    check(w.group[slot].origin[0] == at[0] && w.group[slot].origin[2] == at[2],
          "at the gib");

    /*
     * The step is vel / 15, TRUNCATED: -301 / 15 is -20, where a floor would
     * give -21. The line is offs[i] = i * step and the second spawner stores
     * offs[i] >> 4, arithmetic.
     */
    check_eq_i(w.group[slot].offset[0][0], 10 >> 4, "offs[1].x = 10, >> 4");
    check_eq_i(w.group[slot].offset[0][1], -20 >> 4, "offs[1].y = -20, >> 4");
    check_eq_i(w.group[slot].offset[7][0], 80 >> 4, "offs[8].x = 80, >> 4");
    check_eq_i(w.group[slot].offset[13][0], 140 >> 4, "offs[14].x = 140 >> 4");
    check_eq_i(w.group[slot].offset[13][1], -18,
               "offs[14].y = -280 >> 4 — a floored step would give -19");
    check_eq_i(w.group[slot].offset[13][2], 70 >> 4, "offs[14].z = 70 >> 4");

    for (k = 0; k < 3; k++)
        if (w.group[slot].vel[k] != want[0][k])
            bad++;
    for (i = 1; i < Q2_FX_GIB_TRAIL_COUNT; i++)
        for (k = 0; k < 3; k++)
            if (w.group[slot].rel_vel[i - 1][k] !=
                (s16)(want[i][k] - want[0][k]))
                bad++;
    check_eq_i(bad, 0, "velocities are (rand() + 4*vel - 8192) >> 11");
}

/* ------------------------------------------------------------------------- */
/* Debris                                                                     */
/* ------------------------------------------------------------------------- */

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
        /*
         * AND IT HAS A MODEL. 0x8006473C picks uniformly from the registration
         * list and hands the word to the piece spawner, so a level that filled
         * the list throws visible chunks. Nothing in this port ever filled it,
         * so every piece took the `model = -1` arm and a shattered pane threw
         * twenty-one invisible ones; the client now registers the bank's
         * Debris1..3 by name at zone load.
         */
        check(d->model >= 0, "and a model out of the registration list");
    }
    check_eq_i(up, 20, "every piece leaps");
    check_eq_i(in_box, 20, "every piece started inside the box");

    /* A list nothing registered into is still the console's answer, and it is
     * the right one on the 30 of 49 banks that carry no Debris model. */
    {
        q2_fx_world bare;

        q2_fx_world_init(&bare, &g_tab);
        q2_fx_debris_burst(&bare, &rng, bmin, bmax, NULL, 2, 5);
        check_eq_i(bare.debris[0].model, -1,
                   "with nothing registered a piece has no model");
    }

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
                           Q2_FX_TIMED_BEAM_STYLE, Q2_FX_TIMED_BEAM_LIFE,
                           5),
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
                         Q2_FX_TIMED_BEAM_STYLE, Q2_FX_TIMED_BEAM_LIFE,
                         5);
    }
    check_eq_i(q2_fx_timed_live(&w), 1, "refreshing does not allocate a second");
    check_eq_i(w.timed[0].to[0], b[0], "and it moved with its target");

    /* A different target does get its own. */
    q2_fx_beam_timed(&w, 1, 8, a, b, 64, Q2_FX_TIMED_BEAM_STYLE, 45, 5);
    check_eq_i(q2_fx_timed_live(&w), 2, "a second target takes a second slot");

    /* Twelve is the ceiling, and a full list is tolerated rather than fatal. */
    for (i = 0; i < 40; i++)
        q2_fx_beam_timed(&w, 2, (s32)i, a, b, 64, Q2_FX_TIMED_BEAM_STYLE,
                         45, 5);
    check_eq_i(q2_fx_timed_live(&w), Q2_FX_TIMED_BEAMS_MAX,
               "the list holds twelve");

    /* 0x80048CE8 subtracts the frame delta and clamps, without freeing. */
    q2_fx_world_clear(&w);
    q2_fx_beam_timed(&w, 1, 7, a, b, 64, Q2_FX_TIMED_BEAM_STYLE, 45, 5);
    q2_fx_timed_tick(&w, 20);
    check_eq_i(w.timed[0].timer, 25, "the timer takes the frame delta");
    q2_fx_timed_tick(&w, 100);
    check_eq_i(w.timed[0].timer, 0, "and clamps at zero rather than going negative");
    check_eq_i(q2_fx_timed_live(&w), 0, "an expired beam is not alive");
    check(w.timed[0].present, "the expiry tick still presents the final beam");
    q2_fx_timed_tick(&w, 1);
    check(!w.timed[0].present, "the next tick no longer presents the expired beam");

    /* A creature can hold two attachments on one target; neither replaces
     * the BFG origin entry for the same identities. Refresh each separately. */
    q2_fx_world_clear(&w);
    check(q2_fx_beam_attached(&w, 1, 7, 97, a, b, 5), "a drain attachment is held");
    check(q2_fx_beam_attached(&w, 1, 7, 98, a, b, 5), "another vertex is a separate beam");
    q2_fx_beam_attached(&w, 1, 7, 97, a, b, 5);
    check_eq_i(q2_fx_timed_live(&w), 2, "the owner/target/vertex triple refreshes");
    check(w.timed[0].attachment == 97 && w.timed[0].radius == 10 &&
          w.timed[0].timer == 45, "attached beam uses the native vertex, width and life");
    q2_fx_beam_timed(&w, 1, 7, a, b, 64, 3, 45, 5);
    check_eq_i(q2_fx_timed_live(&w), 3, "an origin beam has a distinct attachment key");

    /* And a live one reaches the ordering table every frame, unqueued. */
    if (psx_ot_init(&ot, 256, 4096) == Q2_OK) {
        gte_init(&gte);
        gte_set_projection(&gte, 256, 256, 124);
        memset(&cam, 0, sizeof(cam));
        cam.projection = 256;
        cam.far_z      = Q2_CAMERA_FAR_DEFAULT;

        q2_fx_world_clear(&w);
        q2_fx_beam_timed(&w, 1, 7, a, b, 64, Q2_FX_TIMED_BEAM_STYLE, 45, 5);

        psx_ot_clear(&ot);
        check(q2_fx_build_ot(&w, &cam, 0, &ot, &gte) > 0,
              "a timed beam draws without being queued");

        /* And again next frame, from the same record. */
        q2_fx_beams_reset(&w);
        psx_ot_clear(&ot);
        check(q2_fx_build_ot(&w, &cam, 0, &ot, &gte) > 0,
              "and again the next frame");

        /*
         * AND IT SURVIVES THE AREA ROUTING, which it did not before.
         *
         * submit_timed used to re-queue every record with a literal area 0 and
         * draw_beams culls an area with no screen-change record, so the BFG's
         * whole trail was dropped in any zone that ships a SortData stream —
         * which is nearly all of them, since no collision cell on the disc
         * carries area 0. The console resolves the owner's own area on every
         * submit: 0x80048D24 runs the point-clip helper 0x8004E920 and
         * 0x80048D50 `lh a3, 64(sp)` passes its answer to the beam queue.
         */
        q2_fx_beams_reset(&w);
        psx_ot_clear(&ot);
        psx_ot_area_register(&ot, 3, 43);
        check_eq_i(q2_fx_build_ot(&w, &cam, 0, &ot, &gte), 0,
                   "a timed beam in a stale area is culled");

        q2_fx_beams_reset(&w);
        psx_ot_clear(&ot);
        psx_ot_area_register(&ot, 5, 43);
        check(q2_fx_build_ot(&w, &cam, 0, &ot, &gte) > 0,
              "and draws once its own area is the registered one");

        psx_ot_free(&ot);
    }
}

/*
 * THE SPAWN TICK IS A DRAW TICK.
 *
 * 0x800304A8 draws every group and only then, from 0x80030B1C, ages them, so a
 * burst the gameplay code raised during the frame reaches the screen at its
 * full life. This port spawns in the sim tick, ages in the sim tick and draws
 * afterwards from the client, which killed a life-1 group outright — the quad
 * damage shell and the energy-bolt crackle are both life 1 and both re-spawned
 * every tick, so neither was ever visible.
 */
static void test_spawn_tick_is_drawable(void)
{
    q2_fx_world w;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    s16 vel[1][3] = { { 0, 0, 0 } };
    s32 at[3] = { 0, 0, 4000 };

    printf("group: a burst spawned this tick still draws this frame\n");

    q2_fx_world_init(&w, &g_tab);
    if (psx_ot_init(&ot, 256, 4096) != Q2_OK)
        return;

    gte_init(&gte);
    gte_set_projection(&gte, 256, 256, 124);
    memset(&cam, 0, sizeof(cam));
    cam.projection = 256;
    cam.far_z      = Q2_CAMERA_FAR_DEFAULT;

    /* Life 1, the quad shell's own, and the port's frame order: spawn, tick,
     * draw. */
    q2_fx_group_spawn(&w, at, vel, Q2_FX_GROUP_QUADS, &g_tab.ramp[0], NULL,
                      1, 4096, 0);
    q2_fx_tick(&w);
    psx_ot_clear(&ot);
    check_eq_i(q2_fx_build_ot(&w, &cam, 0, &ot, &gte), Q2_FX_GROUP_QUADS,
               "a life-1 burst draws its fifteen quads");

    /* And the frame after that it is gone, as 0x80030B24's zero test leaves
     * it. */
    q2_fx_tick(&w);
    psx_ot_clear(&ot);
    check_eq_i(q2_fx_build_ot(&w, &cam, 0, &ot, &gte), 0,
               "and is gone the next frame");

    /*
     * The first entry a longer burst shows is the console's 32 - L, not
     * 32 - (L - 1): 0x80030798 forms `32 - life` and reads the ramp word at
     * +4 + 4 * that.
     */
    q2_fx_world_clear(&w);
    q2_fx_group_spawn(&w, at, vel, 1, &g_tab.ramp[3], NULL, 20, 4096, 0);
    q2_fx_tick(&w);
    check_eq_i(q2_fx_colour_r(q2_fx_group_colour(&w.group[0], 0)), 32 - 20,
               "the first drawn entry is 32 - life");

    psx_ot_free(&ot);
}

static void test_glint_script_scan(void)
{
    u8 mod[256];
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

    put32(mod + 128, 0x244200A0u); /* module +160, not a fixed data offset */
    put32(mod + 132, 0x00161840u);
    put32(mod + 136, 0x00628021u);
    for (i = 0; i < 18; i++) {
        mod[160 + i * 2] = (u8)(i * 13);
        mod[161 + i * 2] = 0;
    }
    check(q2_fx_glint_scan(&s, mod, sizeof(mod)) && s.bands_found,
          "the script's angle-table operand is followed");
    check_eq_i(s.angle[5][2], 17 * 13, "all six pitch/yaw/roll triples load");
    put32(mod + 128, 0x2442FFF8u);
    check(q2_fx_glint_scan(&s, mod, sizeof(mod)) && !s.bands_found,
          "an out-of-range table does not become invented band data");

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
    for (u32 i = 3; i < Q2_FX_GLINT_BANDS_MAX; i++)
        g.band[i] = g.band[0];
    g.band_count = 200;
    psx_ot_clear(&ot);
    check_eq_i(q2_fx_glint_draw(&g, origin, 0, &cam, &ot, &gte),
               one * Q2_FX_GLINT_BANDS_MAX,
               "and a silly count stops at the array's seven");

    memset(g.band, 0, sizeof(g.band));
    psx_ot_clear(&ot);
    check_eq_i(q2_fx_glint_draw(&g, origin, 0, &cam, &ot, &gte), 0,
               "black bands emit no packets (0x80064A38)");

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

static void test_glint_attachment(void)
{
    static const s16 vert[4][4] = {
        { -1024, -1024, 0, 1024 }, { 1024, -1024, 0, 1024 },
        { 1024, 1024, 0, 1024 }, { -1024, 1024, 0, 1024 }
    };
    static const u8 index[8] = { 0, 1, 2, 3, 0, 3, 2, 1 };
    q2_fx_glint g;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    gte_matrix rotation, junk;
    s32 at[3] = { 0, 0, 4096 }, angles[3] = { 0, 0, 0 };
    s32 batch;
    psx_xy base[4];
    u32 i;

    puts("glint: camera, perimeter, scale and the owner's private chain");
    if (psx_ot_init(&ot, 256, 1024) != Q2_OK) {
        check(false, "allocate glint ordering table");
        return;
    }
    memset(&cam, 0, sizeof(cam));
    cam.projection = 256;
    cam.ofs_x = 256;
    cam.ofs_y = 124;
    cam.sort_range = Q2_CAMERA_SORT_RANGE;
    gte_init(&gte);
    gte_set_projection(&gte, 256, 256, 124);
    q2_rotation_euler(rotation.m, 0, 0, 0);
    q2_rotation_euler(junk.m, 1024, 512, 768);
    memset(&g, 0, sizeof(g));
    g.ready = true;
    g.phase = 4;
    g.tint[0] = g.tint[1] = g.tint[2] = 255;
    g.mesh.vert = vert;
    g.mesh.vert_count = 4;
    g.mesh.index = index;
    g.mesh.face_count = 2;

    psx_ot_area_register(&ot, 23, 43);
    batch = psx_ot_batch_begin_point(&ot, 23, false, 4096, at, cam.pos);
    check(psx_ot_batch_add(&ot, batch) != NULL, "owner has a body packet");
    gte_set_rotation(&gte, &junk);
    gte_set_translation(&gte, 999, -999, 2000);
    check_eq_i(q2_fx_glint_draw_attached(&g, at,
                    (const s16 (*)[3])rotation.m, 23, batch, &cam, &ot, &gte),
               2, "both windings join the owner");
    check_eq_i(ot.batch_count, 1, "bands allocate no separate sort records");
    check_eq_i(ot.batch[batch].head, 2, "negative NCLIP prepends");
    check_eq_i(ot.next[2], 0, "prepended face links to body");
    check_eq_i(ot.next[0], 1, "body links to appended positive face");
    check_eq_i(ot.batch[batch].tail, 1, "positive NCLIP appends");
    check_eq_i(ot.next[1], -1, "tail terminates the chain");
    check_eq_i(ot.prims[1].xy[0].x, -32, "768/256 scale with anamorphic camera");
    check_eq_i(ot.prims[1].xy[0].y, -68, "scaled top edge");
    check_eq_i(ot.prims[1].xy[2].x, 544, "third perimeter corner is bottom right");
    check_eq_i(ot.prims[1].xy[2].y, 316, "third perimeter corner keeps its y");
    check_eq_i(gte.tr.x, 0, "view-weapon translation cannot leak into glint");
    check_eq_i(gte.rot.m[0][0], 6144, "glint installs the frame camera");

    psx_ot_clear(&ot);
    psx_ot_area_register(&ot, 3, 43);
    check_eq_i(q2_fx_glint_draw_entity(&g, at, angles, 23, &cam, &ot, &gte),
               0, "unseen owner area emits no glint");
    check_eq_i(ot.prim_count, 0, "no orphan packets for unseen areas");

    psx_ot_clear(&ot);
    g.band_count = 1;
    g.band[0].phase = 4;
    g.band[0].colour = 0xFFFFFF;
    check_eq_i(q2_fx_glint_draw_entity(&g, at, angles, -1, &cam, &ot, &gte),
               2, "one unrotated band");
    memcpy(base, ot.prims[0].xy, sizeof(base));
    for (i = 0; i < 3; i++) {
        psx_ot_clear(&ot);
        memset(g.band[0].angle, 0, sizeof(g.band[0].angle));
        g.band[0].angle[i] = 512;
        check_eq_i(q2_fx_glint_draw_entity(&g, at, angles, -1, &cam, &ot, &gte),
                   2, "rotated band still draws");
        check(memcmp(base, ot.prims[0].xy, sizeof(base)) != 0,
              "each of pitch, yaw and roll changes projected geometry");
    }
    {
        s16 window[8][4];
        u8 faces[80 * 4];
        memcpy(window, vert, sizeof(vert));
        memcpy(window + 4, vert, sizeof(vert));
        for (i = 0; i < 4; i++)
            window[i][3] = 30000;
        for (i = 0; i < 80; i++) {
            u32 c;
            for (c = 0; c < 4; c++)
                faces[i * 4 + c] = (u8)(c + (i == 79 ? 4 : 0));
        }
        g.mesh.vert = (const s16 (*)[4])window;
        g.mesh.vert_count = 8;
        g.mesh.index = faces;
        g.mesh.face_count = 80;
        psx_ot_clear(&ot);
        check_eq_i(q2_fx_glint_draw_entity(&g, at, angles, -1, &cam, &ot, &gte),
                   0, "the 79-face limit includes black skipped faces");
    }
    {
        u8 model[64] = { 0 };
        q2_model_bank common = { NULL, 0, 0 };
        q2_model_bank zone = { model, sizeof(model), 1 };
        q2_entity e;
        q2_entity_set set;
        q2_entity_draw_ctx ctx;
        q2_entity_draw_stats stats;
        memcpy(model + 8, "Blackhole P", 11);
        model[0x20] = model[0x24] = model[0x28] = 64;
        memset(&e, 0, sizeof(e));
        memset(&set, 0, sizeof(set));
        memset(&ctx, 0, sizeof(ctx));
        e.in_use = true;
        e.model_index = -1;
        memcpy(e.model, "Blackhole P", 12);
        set.ent = &e;
        set.count = 1;
        ctx.bank = &common;
        ctx.zone_bank = &zone;
        psx_ot_clear(&ot);
        q2_entity_build_ot(&set, &ctx, &cam, &ot, &gte, &stats);
        check(e.model_bank == &zone, "item missing from common resolves in zone bank");
        check_eq_i(stats.no_model, 0, "zone-only effect models are not discarded");
    }
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

/*
 * Both emitters project through the FRAME's camera, the one the world draw uses
 * — 0x8003058C is `SetRotMatrix(view + 160)` in the effect draw and 0x80047CA4
 * is the same call in the projectile draw, and view+160 is the matrix 0x80037F38
 * builds by handing the basis at view+192 and the viewport's (vw, vh) to
 * 0x80055DE4. There is exactly one of these in the image: a scan of the whole
 * text segment finds a direct `ctc2 R11R12` in game code only at 0x800313DC,
 * the world draw's own load of the same view+160.
 *
 * The port had the effect emitter building a plain yaw/pitch basis of its own
 * and the projectile emitter installing nothing at all, so bolts and sparks
 * came out at two thirds of their horizontal offset from the screen centre and
 * did not roll.
 */
static void test_emitters_use_the_frame_camera(void)
{
    q2_fx_world w;
    q2_rng rng;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    q2_projectiles list;
    gte_matrix want;
    gte_sxy xy;
    u16 z;
    s32 at[3];
    int r, c;

    printf("draw: both emitters install the frame's camera\n");

    if (psx_ot_init(&ot, 256, 4096) != Q2_OK) {
        printf("  FAIL  could not allocate an ordering table\n");
        g_failures++;
        return;
    }

    gte_init(&gte);
    gte_set_projection(&gte, 256, 256, 124);

    memset(&cam, 0, sizeof(cam));
    cam.projection = 256;
    cam.ofs_x      = 256;
    cam.ofs_y      = 124;
    cam.far_z      = Q2_CAMERA_FAR_DEFAULT;
    cam.sort_range = Q2_CAMERA_SORT_RANGE;
    /* An eighth of a turn of roll, so the roll term is load-bearing here: the
     * basis the effect emitter used to build had no roll at all. */
    cam.yaw   = 512;
    cam.pitch = 128;
    cam.roll  = 512;

    q2_rotation_view_anamorphic(want.m, cam.yaw, cam.pitch, cam.roll);

    q2_fx_world_init(&w, &g_tab);
    q2_rng_seed(&rng, 2024);
    at[0] = 0; at[1] = 0; at[2] = 4000;
    check(q2_fx_spawn(&w, &rng, Q2_FX_EXPLOSION, at, 0) >= 0,
          "a burst to give the effect emitter something to draw");

    psx_ot_clear(&ot);
    check(q2_fx_build_ot(&w, &cam, 0, &ot, &gte) > 0,
          "the burst reached the table");

    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            char what[64];
            snprintf(what, sizeof(what),
                     "effect emitter left the frame camera in R%d%d", r + 1,
                     c + 1);
            check_eq_i(gte.rot.m[r][c], want.m[r][c], what);
        }
    }

    /* Now scribble over it, so the projectile emitter cannot pass by inheriting
     * what the effect emitter just installed — which is exactly how it passed
     * before it installed one of its own. */
    {
        gte_matrix junk;
        q2_rotation_yaw_pitch(junk.m, 1024, 0);
        gte_set_rotation(&gte, &junk);
        gte_set_translation(&gte, 111, 222, 333);
    }

    memset(&list, 0, sizeof(list));
    list.p[0].in_use = true;
    list.p[0].kind   = Q2_PROJ_BOLT;
    /* A bolt is drawn as a box only when its flags carry bit 0x4, which the
     * hyperblaster's 14 does and the blaster's 11 does not (projectile.h). */
    list.p[0].flags  = Q2_PROJ_FLAGS_HYPERBLASTER;
    list.p[0].node   = Q2_PROJ_NODE_UNKNOWN;
    list.p[0].pos[0] = 0;
    list.p[0].pos[1] = 0;
    list.p[0].pos[2] = 4000;
    list.p[0].vel[2] = 4096;          /* straight down +Z, so bolt_basis holds */
    list.live = 1;

    psx_ot_clear(&ot);
    check(q2_projectiles_build_ot(&list, NULL, &cam, &ot, &gte, NULL) > 0,
          "the bolt reached the table");

    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            char what[64];
            snprintf(what, sizeof(what),
                     "projectile emitter installed R%d%d", r + 1, c + 1);
            check_eq_i(gte.rot.m[r][c], want.m[r][c], what);
        }
    }
    check_eq_i(gte.tr.x, 0, "and zeroed TRX rather than inheriting one");
    check_eq_i(gte.tr.y, 0, "and zeroed TRY");
    check_eq_i(gte.tr.z, 0, "and zeroed TRZ");

    /*
     * And the size of the term, stated as a picture rather than as a matrix. On
     * axis (yaw = pitch = roll = 0) the camera is diag(3/2, 1, 1), so a point
     * 600 units off to the right at 2000 deep lands at
     *     256 + 256 * (600 * 3/2) / 2000 = 371
     * where the plain basis the emitter used to build put it at 332 — the two
     * thirds that had every spark and bolt pulled towards the crosshair.
     */
    memset(&cam, 0, sizeof(cam));
    cam.projection = 256;
    cam.ofs_x      = 256;
    cam.ofs_y      = 124;
    cam.far_z      = Q2_CAMERA_FAR_DEFAULT;
    cam.sort_range = Q2_CAMERA_SORT_RANGE;

    psx_ot_clear(&ot);
    q2_fx_build_ot(&w, &cam, 0, &ot, &gte);
    check(gte_project_point(&gte, 600, 0, 2000, &xy, &z),
          "an off-axis point projects through the camera the emitter left");
    check_eq_i(xy.x, 371, "and lands at the anamorphic x, not 332");

    psx_ot_free(&ot);
}

/* ------------------------------------------------------------------------- */
/*
 * THE BOLT'S BODY IS THE HYPERBLASTER'S, AND IT IS THE DISC'S OWN BOX.
 *
 * 0x80047F44 gates the whole GTE block on bit 0x4 of record+0x22; the blaster
 * passes 11 and the hyperblaster 14, so only the second has a body at all.
 * 0x800B1E28 then emits it as six gouraud, untextured, OPAQUE quads over the
 * table at 0x8009D664, culling a face whose `nclip` comes out zero or positive
 * (0x800B1EBC `bgez` skips), which on a closed box leaves the three that face
 * the camera. The colours are the table's: (255,64,0) on every corner at local
 * z = -50 and (255,255,0) on every corner at z = +50.
 */
static void test_bolt_body(void)
{
    q2_projectiles list;
    q2_camera      cam;
    psx_ot         ot;
    gte_state      gte;
    u32            blaster, hyper, i, orange = 0, yellow = 0, other = 0;
    u32            flat = 0, blended = 0, worst = 0, quiet = 0;
    int            q;

    /* Six orientations: down the view axis, across it, and four general ones.
     * A closed convex box shows at most three of its six faces from anywhere,
     * which is the property the cull has to have. */
    static const s32 cfg[6][6] = {
        {   0,    0,  600,  4096,     0,    0 },
        { 300,  200,  800,  2000,  1000, 3000 },
        { 200,    0,  700,     0,  4096,    0 },
        {-300,  150,  900,  3000, -1000, 1000 },
        {   0,    0,  600,     0,     0, 4096 },
        { 400, -200, 1200, -2000,  2000, 1000 }
    };

    printf("the bolt body is the hyperblaster's alone\n");

    memset(&gte, 0, sizeof(gte));
    if (psx_ot_init(&ot, 256, 4096) != Q2_OK) {
        check(false, "an ordering table for the bolt");
        return;
    }
    memset(&cam, 0, sizeof(cam));
    cam.projection = 256;
    cam.ofs_x      = 256;
    cam.ofs_y      = 124;
    cam.far_z      = Q2_CAMERA_FAR_DEFAULT;
    cam.sort_range = Q2_CAMERA_SORT_RANGE;
    /* q2_camera_apply_area_projection installs nothing for an unrouted area,
     * so the projection has to be set here or every corner lands on the
     * origin and every face is degenerate. */
    gte_set_projection(&gte, (u16)cam.projection, cam.ofs_x, cam.ofs_y);

    memset(&list, 0, sizeof(list));
    list.p[0].in_use = true;
    list.p[0].kind   = Q2_PROJ_BOLT;
    list.p[0].node   = Q2_PROJ_NODE_UNKNOWN;
    list.live        = 1;

    for (q = 0; q < 6; q++) {
        u32 n;
        int k;

        for (k = 0; k < 3; k++) {
            list.p[0].pos[k] = cfg[q][k];
            list.p[0].vel[k] = cfg[q][3 + k];
        }

        list.p[0].flags = Q2_PROJ_FLAGS_BLASTER;
        psx_ot_clear(&ot);
        if (q2_projectiles_build_ot(&list, NULL, &cam, &ot, &gte, NULL) == 0)
            quiet++;

        list.p[0].flags = Q2_PROJ_FLAGS_HYPERBLASTER;
        psx_ot_clear(&ot);
        n = q2_projectiles_build_ot(&list, NULL, &cam, &ot, &gte, NULL);
        if (n > worst)
            worst = n;
        printf("  orientation %d: %u faces\n", q, n);
    }

    /* And the last one again, held so its primitives can be read. */
    {
        int k;
        for (k = 0; k < 3; k++) {
            list.p[0].pos[k] = cfg[1][k];
            list.p[0].vel[k] = cfg[1][3 + k];
        }
    }
    list.p[0].flags = Q2_PROJ_FLAGS_BLASTER;
    psx_ot_clear(&ot);
    blaster = q2_projectiles_build_ot(&list, NULL, &cam, &ot, &gte, NULL);

    list.p[0].flags = Q2_PROJ_FLAGS_HYPERBLASTER;
    psx_ot_clear(&ot);
    hyper = q2_projectiles_build_ot(&list, NULL, &cam, &ot, &gte, NULL);

    for (i = 0; i < ot.prim_count; i++) {
        const psx_prim *pr = &ot.prims[i];
        int c;

        if (pr->kind != PSX_PRIM_G4)
            flat++;
        if (pr->semi_transparent)
            blended++;
        for (c = 0; c < 4; c++) {
            if (pr->rgb[c].r == 255 && pr->rgb[c].g == 64 &&
                pr->rgb[c].b == 0)
                orange++;
            else if (pr->rgb[c].r == 255 && pr->rgb[c].g == 255 &&
                     pr->rgb[c].b == 0)
                yellow++;
            else
                other++;
        }
    }

    printf("  blaster %u, hyperblaster %u; corners %u orange, %u yellow, "
           "%u neither; %u not gouraud, %u blended\n",
           blaster, hyper, orange, yellow, other, flat, blended);

    check_eq_i((int)blaster, 0,
               "a blaster bolt emits nothing: bit 0x4 is clear in its 11 "
               "(0x80047F44 draws, 0x8004D7A4 writes, both on that bit)");
    check_eq_i((int)quiet, 6,
               "and emits nothing from any of the six orientations either");
    check_eq_i((int)hyper, 3,
               "a hyperblaster bolt seen off-axis emits three faces");
    check((int)worst <= 3,
          "and never more than three from anywhere: nclip drops the far side "
          "(0x800B1EBC skips on MAC0 >= 0, so a face is drawn when it is "
          "negative)");
    check_eq_i((int)other, 0,
               "every corner takes one of the two colours in 0x8009D664");
    check(orange > 0 && yellow > 0,
          "and both appear: the box shades from its tail to its nose");
    check_eq_i((int)flat, 0,
               "the primitive is POLY_G4 — nine words, four colours, no UVs");
    check_eq_i((int)blended, 0,
               "and opaque: the command byte in the table is 0x38, not 0x3A");

    psx_ot_free(&ot);
}

static void test_glint_lightning(void)
{
    q2_fx_glint g = {0};
    q2_camera cam = {0};
    psx_ot ot;
    gte_state gte;
    q2_rng rng;
    psx_prim first;
    u32 count, i;
    puts("generator lightning: endpoints, animation, viewport repeat and shared pool");
    if (psx_ot_init(&ot, 256, 128) != Q2_OK) {
        check(false, "allocate lightning OT"); return;
    }
    cam.projection = 256; cam.ofs_x = 256; cam.ofs_y = 124;
    cam.sort_range = Q2_CAMERA_SORT_RANGE;
    gte_init(&gte);
    gte_set_projection(&gte, 256, 256, 124);
    g.ready = g.raised = g.lightning_ready = true;
    g.band_count = 6;
    /* 0x80065108 uses the same /256 scale as the glow: a local Z=512
     * attachment lies 1536 world units from its owner, at depth 4096. */
    g.origin[2] = 2560;
    for (i = 0; i < 6; i++) {
        g.lightning_target[i][0] = 2400;
        g.lightning_target[i][2] = 4200;
    }
    for (i = 0; i < 8; i++) {
        g.lightning_uv[i][0] = (u8)((i & 1) * 64);
        g.lightning_uv[i][1] = (u8)(159 + (i / 2) * 24);
    }
    count = q2_fx_glint_lightning_draw(&g, 0x3D, 17, &cam, &ot, &gte);
    check_eq_i(count, 32, "six long beams share a 32-polygon pool");
    first = ot.prims[0];
    check(first.kind == PSX_PRIM_FT4 && first.quad_zorder && first.semi_transparent,
          "lightning uses the textured semi-transparent FT4 path");
    check_eq_i(first.uv[0].v, 159, "lightning atlas begins at row 159");
    check_eq_i(abs(first.xy[0].y - first.xy[2].y), 8,
               "disc width is 2 * (256 << 6) / 4096 at the first endpoint");
    check_eq_i(first.uv[1].u, 63, "full segments span 64 texels");
    check_eq_i(first.uv[2].v, 182, "ribbon is 24 texels wide");
    psx_ot_clear(&ot);
    q2_fx_glint_lightning_draw(&g, 0x3D, 17, &cam, &ot, &gte);
    check(!memcmp(first.xy, ot.prims[0].xy, sizeof(first.xy)),
          "another viewport does not advance the noise");
    q2_rng_seed(&rng, 1);
    q2_fx_glint_lightning_tick(&g, &rng);
    g.lightning_frame = 1;
    psx_ot_clear(&ot);
    q2_fx_glint_lightning_draw(&g, 0x3D, 17, &cam, &ot, &gte);
    check_eq_i(ot.prims[0].uv[0].u, 0, "UV frame is held for two ticks");
    check(!memcmp(&first.xy[0], &ot.prims[0].xy[0], sizeof(psx_xy)),
          "the generator endpoint stays anchored while the ribbon jitters");
    check(memcmp(&first.xy[1], &ot.prims[0].xy[1], sizeof(psx_xy)) != 0,
          "interior points respond to the noise state");
    q2_fx_glint_lightning_tick(&g, &rng);
    g.lightning_frame = 2;
    psx_ot_clear(&ot);
    q2_fx_glint_lightning_draw(&g, 0x3D, 17, &cam, &ot, &gte);
    check_eq_i(ot.prims[0].uv[0].u, 64, "second UV pair follows after two ticks");
    g.lightning_frame = 14;
    psx_ot_clear(&ot);
    q2_fx_glint_lightning_draw(&g, 0x3D, 17, &cam, &ot, &gte);
    check_eq_i(ot.prims[0].uv[0].v, 231, "UV follows a jump in the shared frame clock");
    check_eq_i(q2_fx_sqrt_nr(65535), 256, "NR rounds up at a segment boundary");
    check_eq_i(q2_fx_sqrt_nr(65536), 256, "exact segment square remains exact");
    check_eq_i(q2_fx_sqrt_nr(0), 0, "zero beam length is inert");
    check_eq_i(q2_fx_sqrt_nr(3), 2, "integer NR differs from a floor root");
    psx_ot_clear(&ot);
    psx_ot_area_register(&ot, 1, 42);
    g.area = 2;
    check_eq_i(q2_fx_glint_lightning_draw(&g, 0x3D, 17, &cam, &ot, &gte), 0,
               "an invisible owner area cannot leak lightning");
    psx_ot_area_clear(&ot);
    psx_ot_clear(&ot);
    cam.yaw = 2048;
    check_eq_i(q2_fx_glint_lightning_draw(&g, 0x3D, 17, &cam, &ot, &gte), 0,
               "ribbons behind the camera are clipped");
    psx_ot_free(&ot);
}

static void test_laserwall_geometry(void)
{
    static const u8 funcs[16] = {1,0,0,0,'L','A','S','E','R','W','A','L','L',0,0,0};
    u8 common[12 + 9*24] = {0}, zone[sizeof(common)], node[52] = {0};
    q2_events ev = {0};
    q2_userfuncs uf = {0};
    q2_uf_operands ops = {common, zone, sizeof(zone)};
    q2_scene scene = {0};
    struct { q2_laserwall_set set; u32 guard[4]; } bounded;
    q2_laserwall *wall;
    s32 from[3], to[3];
    u32 i;
    printf("laser walls: resident slots, signed rows and moving bounds\n");
    q2_wr_u16(common + 8, 4 + 9*24); common[10] = 9;
    for (i = 0; i < 9; i++) {
        u8 *p = common + 12 + 24*i;
        p[0] = Q2_EVOP_CALL; p[1] = 24; p[5] = 3;
        q2_wr_u16(p + 6, (u16)-500); q2_wr_u16(p + 8, 4970);
        q2_wr_u16(p + 10, (u16)-40); q2_wr_u16(p + 12, 40);
        q2_wr_u16(p + 14, 240); q2_wr_u16(p + 16, (u16)-5);
        q2_wr_u16(p + 18, 0xFFFF);
    }
    memcpy(zone, common, sizeof(zone));
    ev.data = common; ev.size = sizeof(common); ev.record_count = 1; ev.first_record = 8;
    uf.data = funcs; uf.size = sizeof(funcs); uf.count = 1;
    check_eq_i(q2_laserwalls_build(&bounded.set, &ev, &uf, NULL), 0,
               "COMMON has no resident wall objects");
    check_eq_i(bounded.set.declined, 9, "negative slots are all declined");
    for (i = 0; i < 9; i++) q2_wr_u16(zone + 12 + 24*i + 18, 0);
    for (i = 0; i < 4; i++) bounded.guard[i] = 0xABCDEF12u;
    check_eq_i(q2_laserwalls_build(&bounded.set, &ev, &uf, &ops), 8,
               "resident operands raise at most eight walls");
    for (i = 0; i < 4; i++) check_eq_i(bounded.guard[i], 0xABCDEF12u,
                                     "a ninth wall cannot overwrite the set");
    wall = &bounded.set.wall[0];
    check_eq_i(wall->raiser, 8, "the COMMON record owns the off switch");
    scene.nodes = node; scene.node_count = 1;
    q2_wr_u32(node + 16, (u32)-5); q2_wr_u32(node + 20, (u32)-7);
    q2_wr_u32(node + 24, (u32)-9);
    check(q2_laserwall_segment(wall, &scene, 0, from, to), "first row resolves");
    check(from[0] == 38 && from[1] == -243 && from[2] == -9 &&
          to[0] == 5008 && to[1] == -243 && to[2] == -9,
          "negative odd centers truncate toward zero and Y subtracts its offset");
    check(q2_laserwall_segment(wall, &scene, 2, from, to), "third row resolves");
    check(from[0] == 118 && to[0] == 4928 && from[1] == 757 && to[1] == 757,
          "signed spread contracts X while negative rise moves down");
    wall->axis = 2; wall->length = -1000; wall->spread = 15; wall->rise = 100;
    check(q2_laserwall_segment(wall, &scene, 1, from, to), "Z wall resolves");
    check(from[0] == 38 && to[0] == 38 && from[2] == -24 && to[2] == -994 &&
          from[1] == -343 && to[1] == -343, "Z spreads with signed length");
    wall->axis = 1;
    check(q2_laserwall_segment(wall, &scene, 1, from, to), "vertical wall resolves");
    check(from[2] == -24 && to[2] == 6 && from[1] == -343 && to[1] == -1343,
          "non-X walls spread along Z, including vertical ones");
    q2_wr_u32(node + 16, 95); q2_wr_u32(node + 28, 100);
    check(q2_laserwall_segment(wall, &scene, 0, from, to) && from[0] == 137,
          "moving Scene bounds are read again, not cached");
    check(!q2_laserwall_segment(wall, &scene, 3, from, to), "row limit is exclusive");
    wall->scene_node = 1;
    check(!q2_laserwall_segment(wall, &scene, 0, from, to), "absent node cannot draw");
    for (i = 0; i < 9; i++) zone[12 + 24*i + 4] = 3;
    check_eq_i(q2_laserwalls_build(&bounded.set, &ev, &uf, &ops), 0,
               "invalid axes cannot write outside the endpoint vectors");
    check_eq_i(bounded.set.declined, 9, "invalid axes are declined");
}

static void test_scripted_effect_runtime(void)
{
    static const u8 funcs[16] = {
        1, 0, 0, 0, 'L','A','S','E','R','B','E','A','M',0,0,0
    };
    u8 raw[12 + 33 * 36] = {0};
    u8 nodes[72] = {0};
    q2_collision coll = {0};
    q2_userfuncs uf = {0};
    q2_events ev = {0};
    struct { q2_laserbeam_set set; u32 guard[16]; } bounded;
    q2_rng rng;
    q2_fx_world w;
    u32 i;
    printf("scripted effects: laser areas, pool bounds and glint ownership\n");
    /* A 33-beam record used to write beam[32] before checking its capacity. */
    raw[8] = (u8)((4 + 33 * 36) & 0xFF);
    raw[9] = (u8)((4 + 33 * 36) >> 8);
    raw[10] = 33;
    for (i = 0; i < 33; i++) {
        u8 *p = raw + 12 + i * 36;
        p[0] = Q2_EVOP_CALL;
        p[1] = 36;
        put32(p + 4, 1); /* enabled X, inside collision cell 0 */
        put32(p + 20, 2001);
    }
    for (i = 0; i < 3; i++) {
        put32(nodes + i * 4, (u32)-10000);
        put32(nodes + 12 + i * 4, 10000);
    }
    nodes[32] = 23; /* cell zero belongs to render area 23, not area zero */
    coll.nodes = nodes;
    coll.node_count = 1;
    uf.data = funcs; uf.size = sizeof(funcs); uf.count = 1;
    ev.data = raw; ev.size = sizeof(raw); ev.record_count = 1; ev.first_record = 8;
    for (i = 0; i < 16; i++) bounded.guard[i] = 0xCAFEBABEu;
    check_eq_i(q2_laserbeams_build(&bounded.set, &ev, &uf, NULL, &coll), 32,
               "only 32 beams fit");
    for (i = 0; i < 16; i++)
        check_eq_i(bounded.guard[i], 0xCAFEBABEu, "the thirty-third beam writes nothing");
    check_eq_i(bounded.set.beam[0].area, 23, "laser uses the cell's render area");
    q2_fx_world_init(&w, &g_tab);
    q2_rng_seed(&rng, 1);
    check_eq_i(q2_laserbeams_draw(&bounded.set, NULL, &w, &rng), 32,
               "all accepted beams reach the effect queue");
    check_eq_i(w.beam[0].area, 23, "the render area survives submission");

    {
        q2_sim *sim = calloc(1, sizeof(*sim));
        q2_entity e[3] = {0};
        q2_fx_glint *g;
        q2_rng expected;
        u32 rgb = 0;
        int k;
        if (!sim) { check(false, "allocate glint fixture"); return; }
        g = &sim->glint;
        sim->entities.ent = e;
        sim->entities.count = 3;
        g->ready = g->scripted = true;
        g->zone_active = true;
        g->power_left = 3600;
        g->band_count = 1;
        for (i = 0; i < 3; i++) {
            e[i].in_use = true;
            e[i].kind = Q2_ENT_KIND_ITEM;
            e[i].place_id = 20;
            e[i].surface = 23;
        }
        e[0].hidden = true;
        e[1].place_id = 19;
        e[2].origin[0] = 321;
        e[2].angles[0] = 200;
        e[2].angles[1] = -300;
        e[2].angles[2] = 500;
        g->base_angle[0][0] = 1024;
        q2_rng_seed(&g->script_rng, 0);
        q2_rng_seed(&expected, 0);
        (void)q2_rng_next(&expected); /* owner turn speed */
        (void)q2_rng_next(&expected); /* owner turn duration */
        q2_sim_glint_tick(sim);
        check(g->raised, "only the visible item 20 owns the glint");
        check_eq_i(g->origin[0], 321, "glint uses its entity origin");
        check_eq_i(g->area, 23, "glint uses its entity area");
        check(e[2].angles[1] != -300, "the generator turns before refreshing its bands");
        for (k = 0; k < 3; k++)
            check_eq_i(g->band[0].angle[k], (s16)(g->base_angle[0][k] -
                        e[2].angles[k] + (q2_rng_next(&expected) - 16384) / 256),
                       "authored angle minus owner rotation plus jitter");
        for (k = 0; k < 3; k++)
            rgb |= (u32)(u8)q2_rng_next(&expected) << (k * 8);
        check_eq_i(g->band[0].colour, rgb, "script generates each colour channel");
        for (i = 0; i < 300; i++) {
            q2_fx_glint_advance(g);
            q2_sim_glint_tick(sim);
        }
        check_eq_i(g->band[0].phase, 4, "the script refreshes past the old 256-tick blackout");
        e[2].in_use = false;
        q2_sim_glint_tick(sim);
        check(!g->raised, "removing the owner removes its effect");
        free(sim);
    }
}

static void test_authored_pool_and_shared_budget(void)
{
    q2_fx_world w;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    s32 at[3] = {0, 0, 1024};
    s16 velocity[Q2_FX_GROUP_QUADS][3] = {{0}};
    u32 i, views;
    printf("group: authored 80-slot pool and one packet budget across views\n");
    q2_fx_world_init(&w, &g_tab);
    q2_fx_world_resize(&w, 80, 1);
    check_eq_i(w.group_count, 80, "JAIL4/ZONE0 and JAIL5/ZONE2 request 80 groups");
    for (i = 0; i < 80; i++)
        check_eq_i(q2_fx_group_spawn(&w, at, velocity, 1, &g_tab.ramp[0], NULL, 32, 16384, 0),
                   i, "every authored pool slot is available");
    check_eq_i(q2_fx_group_spawn(&w, at, velocity, 1, &g_tab.ramp[0], NULL, 32, 16384, 0),
               -1, "full authored pool refuses the next group");
    q2_fx_world_resize(&w, -1, 1);
    check_eq_i(w.group_count, 0, "negative argument disables particles (80030C74)");
    q2_fx_world_clear(&w);
    check_eq_i(w.group_count, 0, "clear preserves a disabled pool");
    check_eq_i(q2_fx_group_spawn(&w, at, velocity, 1, &g_tab.ramp[0], NULL, 32, 16384, 0),
               -1, "disabled pool refuses a particle");
    check(psx_ot_init(&ot, 256, 4096) == Q2_OK, "allocate particle test packets");
    q2_camera_default(&cam, 512, 248);
    memset(cam.pos, 0, sizeof(cam.pos));
    cam.yaw = cam.pitch = cam.roll = 0;
    cam.projection = 256;
    gte_init(&gte);
    gte_set_projection(&gte, 256, 256, 124);
    for (views = 2; views <= 4; views += 2) {
        q2_fx_world_clear(&w);
        q2_fx_world_resize(&w, 4, views);
        for (i = 0; i < 4; i++)
            check(q2_fx_group_spawn(&w, at, velocity, 15, &g_tab.ramp[0], NULL, 32, 16384, 0) >= 0,
                  "fill a small pool with complete groups");
        for (i = 0; i < views; i++) {
            psx_ot_clear(&ot);
            check_eq_i(q2_fx_build_ot(&w, &cam, i, &ot, &gte), i < views / 2 ? 60 : 0,
                       "800304EC initializes once, 80030AE8 loops views without resetting");
        }
        psx_ot_clear(&ot);
        check_eq_i(q2_fx_build_ot(&w, &cam, 0, &ot, &gte), 60, "next frame restores the packet allowance");
        for (i = 0; i < 4; i++) w.group[i].view_mask = 0x10;
        psx_ot_clear(&ot);
        check_eq_i(q2_fx_build_ot(&w, &cam, 0, &ot, &gte), 0, "hidden first view consumes no packets");
        psx_ot_clear(&ot);
        check_eq_i(q2_fx_build_ot(&w, &cam, 1, &ot, &gte), 60, "next view retains the unspent allowance");
    }
    psx_ot_free(&ot);
}

static void test_resource_lookup(void)
{
    q2_common_file common = {0};
    dat_chunk chunk = {0};
    q2_zone_resources result = {17, 18, 19};
    u8 raw[76] = {0};
    memcpy(raw, "Zone2", 6);
    q2_wr_u32(raw + 12, (u32)-3000);
    q2_wr_u32(raw + 24, 80);
    q2_wr_u32(raw + 28, 48);
    memcpy(raw + 32, "abcdefghijkl", 12);
    q2_wr_u32(raw + 32 + 24, 64);
    chunk.data = raw; chunk.size = sizeof(raw);
    common.chunk[Q2_COMMON_RESOURCES] = &chunk;
    check(q2_common_resources(&common, "JAIL5/zone2", &result), "resource lookup accepts a zone path and mixed case");
    check(result.packet_control == -3000 && result.particle_groups == 80 && result.subdiv_meshes == 48,
          "resource counts retain their signed authored values");
    check(!q2_common_resources(&common, "Zone2extra", &result), "zone prefix is not a complete resource name");
    check_eq_i(result.particle_groups, 80, "failed lookup preserves output");
    check(q2_common_resources(&common, "ABCDEFGHIJKL", &result) && result.particle_groups == 64,
          "full twelve-byte resource name needs no NUL on disc");
    check(!q2_common_resources(&common, "ABCDEFGHIJKLM", &result), "overlong resource name is not truncated");
    chunk.size = 31;
    check(!q2_common_resources(&common, "Zone2", &result), "truncated record cannot supply counts");
    check(!q2_common_resources(NULL, "Zone2", &result), "missing common archive is rejected");
    check(!q2_common_resources(&common, NULL, &result), "missing resource name is rejected");
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
    test_authored_pool_and_shared_budget();
    test_resource_lookup();
    test_ramp_is_indexed_by_age();
    test_one_ramp_defaults_to_both();
    test_budget();
    test_presets();
    test_spawn_offsets_and_puff();
    test_rail_trail();
    test_blur_trail();
    test_spawn_points();
    test_spawn_preset_separates();
    test_beam_pool();
    test_beam_hull();
    test_beam_hull_vertical();
    test_laser();
    test_teleport_column();
    test_item_materialise();
    test_mesh_crackle();
    test_view_mask_skips_one_viewport();
    test_creature_flash();
    test_mesh_spark();
    test_mesh_blood();
    test_actor_present_tickers();
    test_actor_damage_effect();
    test_quad_shell();
    test_gib_trail();
    test_debris();
    test_build_ot();
    test_emitters_use_the_frame_camera();
    test_bolt_body();
    test_effect_sorts_with_the_world();
    test_texture_survives_clear();
    test_timed_beams();
    test_spawn_tick_is_drawable();
    test_glint_script_scan();
    test_glint_two_paths();
    test_glint_attachment();
    test_scripted_effect_runtime();
    test_laserwall_geometry();
    test_glint_lightning();
    test_debris_gravity();
    test_glint_phase();
    test_glintmod_decode();
    test_glint_mesh();
    test_trail_band();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
