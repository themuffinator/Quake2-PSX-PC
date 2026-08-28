/*
 * test_light.c — the lighting model's behaviour, where behaviour can be pinned
 * down without a PlayStation.
 *
 * The disc-side claims — that SpaceLights is partitioned by the secondary
 * collision node, that the flare tables match the executable's, that the
 * reciprocal square root table matches — are checked by `q2psx-inspect lights`,
 * because they need a disc. What is checked here is the arithmetic those claims
 * feed: the attenuation curve and its two boundary cases, the three-light
 * ranking, the 16-bit wrapping delta, the colour matrix's transposition, and
 * the GTE's NCS producing the colour that arithmetic implies.
 */
#include <stdio.h>
#include <string.h>

#include "flare.h"
#include "gte.h"
#include "lighting.h"
#include "trig.h"
#include "weapon.h"

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

static void check_near(s64 got, s64 want, s64 tol, const char *what)
{
    s64 d = got - want;

    g_checks++;
    if (d < 0)
        d = -d;
    if (d > tol) {
        printf("  FAIL  %s: got %lld, want %lld +/- %lld\n",
               what, (long long)got, (long long)want, (long long)tol);
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

static void make_light(q2_light *l, s32 x, s32 y, s32 z,
                       u8 r, u8 g, u8 b, u32 radius)
{
    memset(l, 0, sizeof(*l));
    l->x = x; l->y = y; l->z = z;
    l->r = r; l->g = g; l->b = b;
    l->always_255 = 0xFF;
    l->type   = 7;                 /* style 0: lights, no flare */
    l->radius = (u16)radius;
    l->radius_sq       = radius * radius;
    l->inner_radius_sq = 0;
}

/* ------------------------------------------------------------------------- */
static void test_attenuation(void)
{
    q2_light l;
    s16 d[3];

    printf("attenuation\n");

    make_light(&l, 0, 0, 0, 255, 255, 255, 1000);

    /* At the centre the numerator is the whole outer radius squared. */
    d[0] = d[1] = d[2] = 0;
    check_eq_i(q2_light_attenuation(&l, d, Q2_LIGHT_ATTEN_SCALE), Q2_LIGHT_ONE,
               "full strength at zero distance");

    /* Outside the radius nothing at all, and the test is on the SQUARE, so a
     * light exactly at its radius is already dark. */
    d[0] = 1000;
    check_eq_i(q2_light_attenuation(&l, d, Q2_LIGHT_ATTEN_SCALE), 0,
               "dark exactly at the radius");

    /*
     * The falloff is linear in distance SQUARED, not in distance: at half the
     * radius, three quarters of the light remains rather than half.
     */
    d[0] = 500;
    check_eq_i(q2_light_attenuation(&l, d, Q2_LIGHT_ATTEN_SCALE),
               (Q2_LIGHT_ONE * 3) / 4,
               "three quarters at half the radius");

    /* inner == outer makes the denominator zero, which the original answers
     * with a flat 1.0 rather than a divide. That is what the fallback light
     * relies on. */
    l.inner_radius_sq = l.radius_sq;
    d[0] = 900;
    check_eq_i(q2_light_attenuation(&l, d, Q2_LIGHT_ATTEN_SCALE), Q2_LIGHT_ONE,
               "inner == outer is a flat fill");

    /* Inside the inner radius the result EXCEEDS 1.0 and is not clamped here.
     * The entity path clamps it; the flare path grows the flare with it. */
    l.inner_radius_sq = (u32)(250 * 250);
    d[0] = 100;
    check(q2_light_attenuation(&l, d, Q2_LIGHT_ATTEN_SCALE) > Q2_LIGHT_ONE,
          "inside the inner radius the attenuation exceeds one");

    /* The scale argument widens the reach: a flare at 4x sees a light whose
     * squared distance is four times its radius squared. */
    l.inner_radius_sq = 0;
    d[0] = 1500;
    check_eq_i(q2_light_attenuation(&l, d, Q2_LIGHT_ATTEN_SCALE), 0,
               "out of reach at the entity scale");
    check(q2_light_attenuation(&l, d, Q2_LIGHT_ATTEN_SCALE * 4) > 0,
          "in reach at four times the scale");
}

/* ------------------------------------------------------------------------- */
static void test_wrapping_delta(void)
{
    q2_light l;
    s32 origin[3] = { 0, 0, 0 };
    s16 d[3];

    printf("the delta is 16-bit\n");

    make_light(&l, 100, -200, 300, 255, 255, 255, 1000);
    q2_light_delta(&l, origin, d);
    check_eq_i(d[0], 100,  "x within range");
    check_eq_i(d[1], -200, "y within range");
    check_eq_i(d[2], 300,  "z within range");

    /*
     * 40,000 units away is 40,000 in 32 bits and -25,536 in 16. The original
     * computes the second, so a light far along an axis lights from the WRONG
     * SIDE rather than not at all. Reproducing that is the point.
     */
    make_light(&l, 40000, 0, 0, 255, 255, 255, 32767);
    q2_light_delta(&l, origin, d);
    {
        /* The truncation is the point: cast a variable, so the deliberate
         * wrap is not read as a constant that does not fit (MSVC C4310). */
        s32 beyond_s16 = 40000;

        check_eq_i(d[0], (s16)beyond_s16,
                   "a far light wraps rather than saturating");
    }
    check(d[0] < 0, "and comes out on the opposite side");
}

/* ------------------------------------------------------------------------- */
static void test_ranking(void)
{
    q2_light_set set;
    q2_light l;
    s32 origin[3] = { 0, 0, 0 };
    int i;

    printf("the ranking keeps the three brightest\n");

    q2_light_set_begin(&set);
    check_eq_i(set.count, 0, "no lights to begin with");

    /* Four lights of increasing brightness at the same distance. The three
     * brightest must survive and the dimmest must not. */
    for (i = 1; i <= 4; i++) {
        make_light(&l, 100, 0, 0, (u8)(i * 40), (u8)(i * 40), (u8)(i * 40), 1000);
        q2_light_set_add(&set, origin, &l);
    }

    check(set.count >= 3, "at least three were accepted");

    {
        s16 first  = set.slot[set.rank[0]].total;
        s16 second = set.slot[set.rank[1]].total;
        s16 third  = set.slot[set.rank[2]].total;

        check(first >= second && second >= third,
              "the ranking is in descending brightness");
        check(first > 0, "the brightest slot holds a light");
    }

    /* A light out of range is not offered a slot at all. */
    {
        u32 before = set.count;
        make_light(&l, 100000, 0, 0, 255, 255, 255, 10);
        check(!q2_light_set_add(&set, origin, &l), "a distant light is refused");
        check_eq_i(set.count, before, "and does not bump the count");
    }
}

/* ------------------------------------------------------------------------- */
static void test_signed_f4_branch(void)
{
    q2_light_world world;
    q2_light_set set;
    const q2_light_slot *slot;
    s32 origin[3] = { 0, 0, 0 };

    printf("the +0xF4 lighting branch is signed\n");

    memset(&world, 0, sizeof(world));
    make_light(&world.dynamic_world[0], 0, 0, 100,
               255, 0, 0, 1000);
    make_light(&world.dynamic_view[0], 0, 0, 100,
               0, 0, 255, 1000);
    world.dynamic_world_count = 1;
    world.dynamic_view_count  = 1;

    /* The viewmodel constructor writes literal +1.  0x8006B040 is `bgez`, so
     * +1 must retain the world light rather than behaving like a true flag. */
    q2_light_gather(&set, &world, origin, 0, 1);
    slot = &set.slot[set.rank[0]];
    check(slot->rgb[0] > 0 && slot->rgb[2] == 0,
          "+1 takes the red world list");

    q2_light_gather(&set, &world, origin, 0, 0);
    slot = &set.slot[set.rank[0]];
    check(slot->rgb[0] > 0 && slot->rgb[2] == 0,
          "zero also takes the world list");

    q2_light_gather(&set, &world, origin, 0, -1);
    slot = &set.slot[set.rank[0]];
    check(slot->rgb[0] == 0 && slot->rgb[2] > 0,
          "only a negative value takes the blue alternate list");
}

/* ------------------------------------------------------------------------- */
static void test_env(void)
{
    q2_light_set set;
    q2_light_env env;
    q2_light l;
    s32 origin[3] = { 0, 0, 0 };
    u8 glow[3] = { 16, 32, 48 };

    printf("the environment the GTE gets\n");

    q2_light_set_begin(&set);
    make_light(&l, 0, 0, 200, 255, 0, 0, 1000);   /* red, ahead      */
    q2_light_set_add(&set, origin, &l);
    make_light(&l, 200, 0, 0, 0, 255, 0, 1000);   /* green, to the side */
    q2_light_set_add(&set, origin, &l);

    q2_light_env_build(&env, &set, Q2_LIGHT_ONE, Q2_LIGHT_ONE, glow);

    check_eq_i(env.active, 2, "two lights survived");

    /*
     * The colour matrix is filled by COLUMN: column j is light j's colour, so
     * row 0 is the three reds. With one red light and one green one, exactly
     * one entry of row 0 and one of row 1 are non-zero, and row 2 is empty.
     */
    check((env.colour.m[0][0] != 0) != (env.colour.m[0][1] != 0),
          "exactly one light contributes red");
    check((env.colour.m[1][0] != 0) != (env.colour.m[1][1] != 0),
          "exactly one light contributes green");
    check_eq_i(env.colour.m[2][0], 0, "neither contributes blue (light 0)");
    check_eq_i(env.colour.m[2][1], 0, "neither contributes blue (light 1)");
    check_eq_i(env.colour.m[0][2], 0, "the third column is empty");

    /* Both intensity fields neutral means the back colour is the glow itself. */
    check_eq_i(env.back[0], glow[0], "back colour red is the glow");
    check_eq_i(env.back[1], glow[1], "back colour green is the glow");
    check_eq_i(env.back[2], glow[2], "back colour blue is the glow");

    /* The directions are unit length in 1.3.12, scaled by the intensity, which
     * at neutral is 2.0 — the product of two 4096s shifted by 11. */
    {
        s32 len = 0;
        int c;
        for (c = 0; c < 3; c++)
            len += (s32)env.dir[0][c] * env.dir[0][c];
        check(len > 6000 * 6000 && len < 10000 * 10000,
              "the light direction is scaled unit length");
    }

    /* +0xFC/+0xFE are lighting factors, not a model transform. Halving either
     * one halves both the directional rows and the ambient back colour. The
     * geometry path has no reader of either field. */
    q2_light_env_build(&env, &set, Q2_LIGHT_ONE / 2, Q2_LIGHT_ONE, glow);
    check_eq_i(env.back[0], glow[0] / 2,
               "half +0xFC halves the ambient red");
    check_eq_i(env.back[1], glow[1] / 2,
               "half +0xFC halves the ambient green");
    {
        s32 len = 0;
        int c;
        for (c = 0; c < 3; c++)
            len += (s32)env.dir[0][c] * env.dir[0][c];
        check(len > 3000 * 3000 && len < 5000 * 5000,
              "half +0xFC halves the light-matrix row");
    }

    q2_light_env_build(&env, &set, Q2_LIGHT_ONE, 0, glow);
    check_eq_i(env.back[0], 0, "zero +0xFE blackens the ambient");
    check_eq_i(env.dir[0][0], 0, "zero +0xFE clears light row x");
    check_eq_i(env.dir[0][1], 0, "zero +0xFE clears light row y");
    check_eq_i(env.dir[0][2], 0, "zero +0xFE clears light row z");
}

/* ------------------------------------------------------------------------- */
static void test_normalise(void)
{
    s16 in[3], out[3];
    s32 len;
    int c;

    printf("VectorNormal\n");

    /* The table's first entry is exactly 4096, so an axis-aligned vector
     * normalises exactly. */
    in[0] = 1000; in[1] = 0; in[2] = 0;
    q2_vector_normal(in, out);
    check(out[0] > 4000 && out[0] <= 4096, "an axis vector normalises to one");
    check_eq_i(out[1], 0, "and leaves the other axes alone");

    in[0] = 300; in[1] = -400; in[2] = 1200;
    q2_vector_normal(in, out);
    len = 0;
    for (c = 0; c < 3; c++)
        len += (s32)out[c] * out[c];
    check(len > 3900 * 3900 && len < 4300 * 4300,
          "an oblique vector lands within a few percent of unit");
    check(out[1] < 0, "and keeps its signs");
}

/* ------------------------------------------------------------------------- */
static void test_ncs(void)
{
    gte_state g;
    q2_light_env env;
    q2_light_set set;
    q2_light l;
    s32 origin[3] = { 0, 0, 0 };
    u8 glow[3] = { 0, 0, 0 };

    printf("NCS shades a normal\n");

    gte_init(&g);

    q2_light_set_begin(&set);
    make_light(&l, 0, 0, 400, 255, 255, 255, 1000);
    q2_light_set_add(&set, origin, &l);
    q2_light_env_build(&env, &set, Q2_LIGHT_ONE, Q2_LIGHT_ONE, glow);
    q2_light_env_apply(&env, &g);

    /* A normal pointing straight at the light picks up its colour. */
    g.v[0].x = 0; g.v[0].y = 0; g.v[0].z = 4096;
    gte_ncs(&g);
    check(g.rgb_fifo[2].r > 128, "a normal facing the light is bright");
    check_eq_i(g.rgb_fifo[2].r, g.rgb_fifo[2].g, "a white light stays white");

    /* Facing away, the light matrix's limit-negative clamp zeroes the term and
     * only the back colour is left — which here is black. */
    g.v[0].x = 0; g.v[0].y = 0; g.v[0].z = -4096;
    gte_ncs(&g);
    check_eq_i(g.rgb_fifo[2].r, 0, "a normal facing away gets nothing");

    /* With a back colour, that is exactly what an unlit vertex gets. The
     * hardware stores r << 4 and the colour stage divides by 16 again. */
    gte_set_back_colour(&g, 64, 64, 64);
    gte_ncs(&g);
    check_eq_i(g.rgb_fifo[2].r, 64, "an unlit vertex is the back colour");
}

/*
 * A COLOURED light, dim enough not to saturate — the case the white-light check
 * above cannot see.
 *
 * The colour stage is a matrix multiply and all three rows read the same input
 * vector. `gte_set_ir` writes back into that vector, so a loop that reads
 * `g->ir[]` as it goes feeds the RED result into the green and blue rows, and
 * both come out scaled by a further colour/4096. With one light reaching a
 * model the other two columns are zero, so the model goes pure red.
 *
 * The white-light check passes straight through it: at 255/255/255 both
 * channels saturate at 255 in gte_colour_sat, so `r == g` holds while the
 * underlying values are 428 and 358. Non-white and non-saturating is the only
 * shape that catches it.
 *
 * The figures are BASE2 creature 0's real light — record 136, rgb 184/155/101,
 * attenuated slot colour 2250/1895/1235 — at half deflection.
 */
static void test_ncs_coloured(void)
{
    gte_state g;
    int i;

    printf("NCS does not feed red into green and blue\n");

    gte_init(&g);

    /* One light along +Z at the magnitude q2_light_env_build produces (the
     * normalised direction doubled), and its colour in column 0. */
    for (i = 0; i < 3; i++) {
        int k;
        for (k = 0; k < 3; k++) {
            g.light.m[i][k]  = 0;
            g.colour.m[i][k] = 0;
        }
    }
    g.light.m[0][2]  = 8192;
    g.colour.m[0][0] = 2250;
    g.colour.m[1][0] = 1895;
    g.colour.m[2][0] = 1235;
    gte_set_back_colour(&g, 0, 0, 0);

    /* Half deflection, so nothing saturates. */
    g.v[0].x = 0; g.v[0].y = 0; g.v[0].z = 2048;
    gte_ncs(&g);

    check_eq_i(g.rgb_fifo[2].r, 140, "red is the light's own red term");
    check_eq_i(g.rgb_fifo[2].g, 118, "green is its GREEN term, not red again");
    check_eq_i(g.rgb_fifo[2].b,  77, "and blue its blue term");

    /* The signature of the aliasing, stated as a ratio so a future regression
     * is recognisable: green would come out at green/4096 of red. */
    check(g.rgb_fifo[2].g != (s32)(((s64)140 * 1895) >> 12),
          "green is not red scaled by the green column");
}

/* ------------------------------------------------------------------------- */
static void test_flare_styles(void)
{
    printf("flare styles\n");

    /* The five type values on the disc and what they select. */
    check_eq_i(q2_flare_style_of(7),  0, "type 7 has no flare");
    check_eq_i(q2_flare_style_of(15), 1, "type 15 is style 1");
    check_eq_i(q2_flare_style_of(23), 2, "type 23 is style 2");
    check_eq_i(q2_flare_style_of(31), 3, "type 31 is style 3");
    check_eq_i(q2_flare_style_of(39), 4, "type 39 is style 4");

    check_eq_i(q2_flare_size_of(39), Q2_FLARE_BASE_SIZE,
               "bits 6-7 are clear on the disc, so the size is the base");
    check_eq_i(q2_flare_size_of(39 | 0x40), Q2_FLARE_BASE_SIZE * 2,
               "and one step up doubles it");

    check_eq_i(q2_flare_style_table(0)->count, 0, "style 0 draws nothing");
    check_eq_i(q2_flare_style_table(1)->count, 6, "style 1 has six elements");
    check_eq_i(q2_flare_style_table(2)->count, 1, "style 2 is the core alone");
    check_eq_i(q2_flare_style_table(3)->count, 2, "style 3 has two");
    check_eq_i(q2_flare_style_table(4)->count, 5, "style 4 has five");

    /*
     * The styles are nested: 2 is the first element of 3, 3 the first two of 4,
     * 4 the first five of 1. If that ever stops holding, the table was
     * misread.
     */
    {
        const q2_flare_element *s1 = q2_flare_style_table(1)->element;
        const q2_flare_element *s4 = q2_flare_style_table(4)->element;
        const q2_flare_element *s3 = q2_flare_style_table(3)->element;
        const q2_flare_element *s2 = q2_flare_style_table(2)->element;

        check(memcmp(s1, s4, sizeof(q2_flare_element) * 5) == 0,
              "style 4 is style 1's first five");
        check(memcmp(s4, s3, sizeof(q2_flare_element) * 2) == 0,
              "style 3 is style 4's first two");
        check(memcmp(s3, s2, sizeof(q2_flare_element) * 1) == 0,
              "style 2 is style 3's first one");
    }

    /* The first element sits on the light itself; the rest are ghosts along the
     * line back through the screen centre, and most of them are behind it. */
    {
        const q2_flare_element *e = q2_flare_style_table(1)->element;
        int behind = 0, i;

        check_eq_i(e[0].pos, 4096, "the core sits on the light");
        for (i = 1; i < 6; i++)
            if (e[i].pos < 0)
                behind++;
        check(behind == 3, "three of the five ghosts are past the centre");
    }
}

/* ------------------------------------------------------------------------- */
static void test_glow_fade(void)
{
    u8 cur[3] = { 0, 0, 0 };
    const u8 target[3] = { 100, 100, 100 };
    int i;

    printf("the ambient fade\n");

    q2_light_glow_fade(cur, target, 4);
    check_eq_i(cur[0], 25, "a quarter of the way in one step");

    for (i = 0; i < 64; i++)
        q2_light_glow_fade(cur, target, 4);
    check_eq_i(cur[0], 100, "and it does arrive");

    /* Falling is immediate: the signed comparison against the step count is
     * always true for a negative difference. */
    {
        const u8 down[3] = { 10, 10, 10 };
        q2_light_glow_fade(cur, down, 4);
        check_eq_i(cur[0], 10, "a falling glow arrives at once");
    }

    /* A zero step count is read as one rather than dividing by zero. */
    {
        const u8 up[3] = { 200, 200, 200 };
        q2_light_glow_fade(cur, up, 0);
        check_eq_i(cur[0], 200, "zero steps means one step");
    }
}

/* ------------------------------------------------------------------------- */
static void test_dynamic(void)
{
    q2_light_world w;
    s32 pos[3] = { 100, 200, 300 };
    u8  rgb[3] = { 255, 128, 0 };
    int i;

    printf("the runtime light list\n");

    memset(&w, 0, sizeof(w));

    check(q2_light_add_dynamic(&w, pos, rgb, 100, 400, 4, 1),
          "a light is appended");
    check_eq_i(w.dynamic_world_count, 1, "and the list grows");

    {
        const q2_light *l = &w.dynamic_world[0];

        check_eq_i(l->radius, 400, "the radius is the outer one");
        check_eq_i(l->radius_sq, 400 * 400, "which is squared into radiusSq");
        check_eq_i(l->inner_radius_sq, 100 * 100, "and the inner likewise");

        /* Style and size come back out through the same accessors the disc
         * lights use, which is the point of packing them the same way. */
        check_eq_i(q2_flare_style_of(l->type), 4, "the style round-trips");
        check_eq_i(q2_flare_size_of(l->type), Q2_FLARE_BASE_SIZE * 2,
                   "and so does the size shift");
        check_eq_i(l->type & 7, 0,
                   "bits 0-2 are left clear, unlike every light on the disc");
    }

    /* Sixteen and no more; the seventeenth is dropped rather than replacing. */
    for (i = 1; i < Q2_DYNLIGHT_MAX; i++)
        check(q2_light_add_dynamic(&w, pos, rgb, 100, 400, 1, 0), "fills up");
    check(!q2_light_add_dynamic(&w, pos, rgb, 100, 400, 1, 0),
          "the seventeenth is refused");
    check_eq_i(w.dynamic_world_count, Q2_DYNLIGHT_MAX, "and nothing is lost");

    q2_light_world_begin_frame(&w);
    check_eq_i(w.dynamic_world_count, 0, "a new frame empties the list");
}

/* ------------------------------------------------------------------------- */

/*
 * The muzzle flash's radii, against the shift-add chain 0x8004C978 actually
 * emits. The compiler wrote `r*100` as ((r*2 + r) << 3 + r) << 2 and `r*200` as
 * the same with one more shift; this recomputes both the long way and checks the
 * port's multiply agrees at the ends of the range and at a value in the middle.
 *
 * The bases are the two `addiu` immediates, 250 and 700, and one rand draw feeds
 * both radii -- so a shot's inner and outer always move together.
 */
static void test_muzzle_light(void)
{
    static const s32 draws[] = { 0, 1, 16384, 32767 };
    u32 i;

    for (i = 0; i < sizeof(draws) / sizeof(draws[0]); i++) {
        s32 r = draws[i], inner = -1, outer = -1;
        s32 want_in, want_out, t;

        /* r * 100, as the shift-add chain builds it. */
        t = r << 1; t += r; t <<= 3; t += r; t <<= 2;
        want_in = (t >> 15) + 250;

        /* r * 200 -- the same chain with the last shift one wider. */
        t = r << 1; t += r; t <<= 3; t += r; t <<= 3;
        want_out = (t >> 15) + 700;

        q2_weapon_muzzle_light(r, &inner, &outer);
        check_eq_i(inner, want_in,  "muzzle inner radius");
        check_eq_i(outer, want_out, "muzzle outer radius");
    }

    /* The documented ranges, which are what a reader will sanity-check against. */
    {
        s32 lo_in, lo_out, hi_in, hi_out;
        q2_weapon_muzzle_light(0, &lo_in, &lo_out);
        q2_weapon_muzzle_light(32767, &hi_in, &hi_out);
        check_eq_i(lo_in, 250,  "inner at rand 0");
        check_eq_i(hi_in, 349,  "inner at rand 32767");
        check_eq_i(lo_out, 700, "outer at rand 0");
        check_eq_i(hi_out, 899, "outer at rand 32767");
    }

    /* The creature flash: two draws, bigger bases, same colour. */
    {
        s32 in0, out0, in1, out1;
        q2_creature_muzzle_light(0, 0, &in0, &out0);
        q2_creature_muzzle_light(32767, 32767, &in1, &out1);
        check_eq_i(in0,   850, "creature inner at rand 0");
        check_eq_i(in1,  1049, "creature inner at rand 32767");
        check_eq_i(out0, 1200, "creature outer at rand 0");
        check_eq_i(out1, 1599, "creature outer at rand 32767");

        /* Two independent draws: the second must not move the first. */
        q2_creature_muzzle_light(0, 32767, &in0, &out0);
        check_eq_i(in0,   850, "inner ignores the second draw");
        check_eq_i(out0, 1599, "outer ignores the first");
    }

    check(q2_weapon_has_muzzle_light(Q2_WID_MACHINEGUN), "machinegun flashes");
    check(q2_weapon_has_muzzle_light(Q2_WID_CHAINGUN),   "chaingun flashes");
    check(!q2_weapon_has_muzzle_light(Q2_WID_BFG),       "the BFG does not");
}


/*
 * FLKLIGHT's phase. The durations are the operand table's own formulas, and the
 * behaviour worth pinning is the turn-over: a flicker starts lit, flips when its
 * time comes, and a long frame that crosses several turn-overs must not leave
 * the phase behind the clock.
 */
static void test_flklight(void)
{

    /* These two are RADII, not durations. 0x80028858 stores the first into a2's
     * low half and 0x8002888C the second into its high half, and 0x800288C8
     * hands that a2 straight to the light. An earlier version of this test
     * asserted them as on/off times and passed, because the arithmetic is the
     * same either way — only the destination distinguishes them. */
    check_eq_i(q2_flklight_inner_radius(0),      400, "inner at rand 0");
    check_eq_i(q2_flklight_inner_radius(32767),  899, "inner at rand 32767");
    check_eq_i(q2_flklight_outer_radius(0),     1000, "outer at rand 0");
    check_eq_i(q2_flklight_outer_radius(32767), 1499, "outer at rand 32767");

    /* No phase is tested because the original has none: the exec at 0x800287A0
     * adds a light and returns. The radii above are the whole of it. */
}

/* ------------------------------------------------------------------------- */
/*
 * The flare's GEOMETRY, read back out of the ordering table.
 *
 * This is here because the alternative is looking at a screenshot, and a
 * screenshot cannot tell a twelve-sided glow from an eleven-sided one with a
 * doubled vertex — which is exactly the bug the ring generator had. Its two
 * cursors walk from opposite ends and write at +4 from each, so iteration k
 * fills out[k+1] and out[n+1-k]; the port had out[n-k], which silently drops a
 * vertex and duplicates another, and additively blended over a bright wall it
 * looks like nothing at all.
 *
 * Everything below is checked through q2_flare_draw rather than against the
 * generators directly, so the wiring — kinds, blend mode, colours, bucket — is
 * on trial with the arithmetic.
 */
static void flare_test_view(q2_flare_view *v, u16 bucket)
{
    memset(v, 0, sizeof(*v));
    v->centre[0] = Q2_FLARE_REF_W / 2;
    v->centre[1] = Q2_FLARE_REF_H / 2;
    v->extent[0] = Q2_FLARE_REF_W;      /* the console's own viewport, so the */
    v->extent[1] = Q2_FLARE_REF_H;      /* two ring divisors cancel to 4096   */
    v->bucket    = bucket;
}

static void test_flare_geometry(void)
{
    q2_flare_view  view;
    q2_flare_stats stats;
    q2_camera cam;
    gte_state gte;
    psx_ot    ot;
    q2_light  l;
    gte_matrix rot;

    printf("flare geometry\n");

    if (psx_ot_init(&ot, 64, 4096) != Q2_OK) {
        check(false, "the test ordering table allocates");
        return;
    }

    flare_test_view(&view, 32);
    memset(&stats, 0, sizeof(stats));
    memset(&cam, 0, sizeof(cam));

    /*
     * Straight ahead and close enough to be inside the inner radius, which is
     * what puts the light on the GROW side of 4096 — full colour and a scale
     * that is the attenuation rather than the floor.
     */
    /*
     * A big flare on purpose. The attenuation only exceeds 4096 inside the
     * inner radius, and the flare's screen radius is `atten >> 7`, so a light
     * with its inner radius close to its outer one gives a few hundred pixels
     * instead of the floor's 32 — and at a few hundred pixels the one-pixel
     * truncations below are noise rather than the whole measurement.
     */
    make_light(&l, 0, 0, 1000, 255, 255, 255, 4000);
    l.type = (2 << 3) | 7;                 /* style 2: the core alone */
    l.inner_radius_sq = 3900u * 3900u;

    gte_init(&gte);
    /* Identity: the camera looks down +z with no rotation, so a light on the
     * axis lands on the geometry offset. */
    memset(&rot, 0, sizeof(rot));
    rot.m[0][0] = rot.m[1][1] = rot.m[2][2] = Q2_ONE_12;
    gte_set_rotation(&gte, &rot);
    gte_set_translation(&gte, 0, 0, 0);
    gte_set_projection(&gte, Q2_FLARE_REF_W, view.centre[0], view.centre[1]);

    q2_flare_draw(&l, &cam, &view, &ot, &gte, &stats);

    check_eq_i(stats.flares_drawn, 1, "a light in range draws its flare");

    /*
     * Style 2 is one BURST element and nothing else: six Gouraud quads for the
     * twelve-sided glow, then eight Gouraud lines for the star.
     */
    check_eq_i(stats.prims_emitted, Q2_FLARE_BURST_SIDES / 2 + Q2_FLARE_BURST_LINES,
               "the core is six quads and eight lines");

    {
        u32 quads = 0, lines = 0, wrong_blend = 0, opaque = 0, i;
        s32 rim_x = 0, rim_y = 0;      /* the widest rim offset seen  */
        s32 arm_x = 0, arm_y = 0;      /* the longest arm             */
        s32 diag_x = 0;
        /* `have_centre` is what guards the reads below, but the compiler
         * does not correlate a flag with the value it guards. */
        psx_xy centre = { 0, 0 };
        bool have_centre = false;

        for (i = 0; i < ot.prim_count; i++) {
            const psx_prim *p = &ot.prims[i];

            if (!p->semi_transparent)
                opaque++;
            if (((p->tpage >> 5) & 3u) != PSX_BLEND_ADD)
                wrong_blend++;

            if (p->kind == PSX_PRIM_G4) {
                quads++;
                /* xy[1] is the centre corner — the bright one. */
                centre = p->xy[1];
                have_centre = true;
            } else if (p->kind == PSX_PRIM_LINE_G2) {
                lines++;
            }
        }

        check_eq_i(quads, Q2_FLARE_BURST_SIDES / 2, "six quads make the glow");
        check_eq_i(lines, Q2_FLARE_BURST_LINES, "eight lines make the star");
        check_eq_i(opaque, 0, "every flare primitive is semi-transparent");
        check_eq_i(wrong_blend, 0,
                   "and every one blends additively, as the world leaves the "
                   "draw mode");
        check(have_centre, "the glow has a centre corner");

        /* The rim, and the arms, measured as offsets from that centre. */
        for (i = 0; i < ot.prim_count; i++) {
            const psx_prim *p = &ot.prims[i];
            int c;

            if (p->kind == PSX_PRIM_G4) {
                for (c = 0; c < 4; c++) {
                    s32 dx = p->xy[c].x - centre.x;
                    s32 dy = p->xy[c].y - centre.y;

                    if (dx < 0) dx = -dx;
                    if (dy < 0) dy = -dy;
                    if (dx > rim_x) rim_x = dx;
                    if (dy > rim_y) rim_y = dy;
                }
            } else if (p->kind == PSX_PRIM_LINE_G2) {
                s32 dx = p->xy[1].x - centre.x;
                s32 dy = p->xy[1].y - centre.y;

                check(p->xy[0].x == centre.x && p->xy[0].y == centre.y,
                      "every arm starts at the glow's centre");
                check(p->rgb[1].r == 0 && p->rgb[1].g == 0 && p->rgb[1].b == 0,
                      "and fades to black at its tip");

                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;

                if (dy == 0 && dx > arm_x) arm_x = dx;
                if (dx == 0 && dy > arm_y) arm_y = dy;
                if (dx != 0 && dy != 0 && dx > diag_x) diag_x = dx;
            }
        }

        /*
         * The rim is a circle at 320x240, because that is the reference both
         * ring divisors are taken against and the viewport above is exactly it.
         * Anywhere else it is an ellipse, which is the console's own behaviour
         * and not a fault: the divisors are per-axis.
         */
        check_eq_i(rim_x, rim_y, "the glow is round on a 320x240 viewport");
        check(rim_x > 100, "and big enough for the ratios below to mean something");

        /*
         * The arms, against the six divisors solved out of the instruction
         * stream. An axis arm reaches 4096/2500 of the rim and a diagonal
         * 4096/3500 of it — each measured against the rim's OWN reach in that
         * direction, which for the diagonal is not a vertex of a twelve-gon and
         * so is recomputed here from the ring divisor.
         *
         * Within a pixel rather than exactly, because the two paths truncate at
         * different points: the rim divides by 320*4096 and the arm by 320*2500
         * from the same product. A wrong divisor would be out by a third, not
         * by one.
         */
        {
            s32 rim45 = (s32)(q2_sin12(Q2_ANGLE_360 / 8) * rim_x) * view.extent[0]
                      / ((s32)Q2_FLARE_REF_W * Q2_ONE_12);

            check_near(arm_x, (s32)((s64)rim_x * Q2_ONE_12 / Q2_FLARE_SPIKE_REF), 1,
                       "an axis arm reaches 4096/2500 of the rim");
            check_near(arm_y, (s32)((s64)rim_y * Q2_ONE_12 / Q2_FLARE_SPIKE_REF), 1,
                       "in both axes");
            check_near(diag_x, (s32)((s64)rim45 * Q2_ONE_12 / Q2_FLARE_DIAG_REF), 1,
                       "and a diagonal reaches 4096/3500 of it");
            check(diag_x < arm_x,
                  "so a diagonal is the shorter of the two, 5/7 of an axis arm");
        }

        /*
         * The regression the ring's off-by-one caused: with out[n-k] instead of
         * out[n+1-k] the rim visits only eleven distinct directions and doubles
         * one, so the widest LEFT offset stops matching the widest right one.
         * Count the distinct rim points instead of trusting the extremes.
         */
        {
            psx_xy seen[Q2_FLARE_BURST_SIDES * 4];
            u32 n = 0, j;

            for (i = 0; i < ot.prim_count; i++) {
                const psx_prim *p = &ot.prims[i];
                int c;

                if (p->kind != PSX_PRIM_G4)
                    continue;
                for (c = 0; c < 4; c++) {
                    if (p->xy[c].x == centre.x && p->xy[c].y == centre.y)
                        continue;               /* the centre corner */
                    for (j = 0; j < n; j++)
                        if (seen[j].x == p->xy[c].x && seen[j].y == p->xy[c].y)
                            break;
                    if (j == n && n < (u32)(sizeof seen / sizeof seen[0]))
                        seen[n++] = p->xy[c];
                }
            }

            check_eq_i(n, Q2_FLARE_BURST_SIDES,
                       "the rim has twelve distinct vertices, not eleven and a "
                       "duplicate");
        }
    }

    /*
     * The GHOSTS, which is the half of the effect that makes it a lens flare.
     *
     * Style 1 is the same core followed by five DISC elements, each placed at
     * its own fraction along the line from the screen centre to the light —
     * three of them past the centre, on the far side. Redraw the same light as
     * style 1 and check that the five hexagons land where their `pos` says and
     * that they are hexagons, which is `flare_hex` (0x80074C4C) rather than the
     * twelve-gon generator the port used to reuse here.
     */
    psx_ot_clear(&ot);
    memset(&stats, 0, sizeof(stats));
    l.type = (1 << 3) | 7;

    q2_flare_draw(&l, &cam, &view, &ot, &gte, &stats);

    {
        const q2_flare_style *style = q2_flare_style_table(1);
        u32 flat = 0, i;
        u32 e;

        for (i = 0; i < ot.prim_count; i++)
            if (ot.prims[i].kind == PSX_PRIM_F4)
                flat++;

        check_eq_i(flat, (style->count - 1) * (Q2_FLARE_DISC_SIDES / 2),
                   "five ghosts, three flat quads each");

        /*
         * How big each one is. The light is dead ahead here, so every element
         * collapses onto the geometry offset and the PLACEMENT cannot be read
         * off the primitives — but the widths can, and a ghost's width is
         * `scale * size >> 12` run through the ring divisor, so the five must
         * come out in the same order as the table's five sizes.
         *
         * That ordering is not monotone: style 1 runs 2048, 1100, 800, 512,
         * 768, so the last ghost is WIDER than the one before it. A test that
         * only asserted "they shrink" would pass on a build that ignored `size`
         * for the tail.
         */
        {
            s32 width[8];
            u32 n = 0;

            for (i = 0; i < ot.prim_count && n < 8; i++) {
                const psx_prim *p = &ot.prims[i];
                s32 lo = 1 << 30, hi = -(1 << 30);
                int c;

                if (p->kind != PSX_PRIM_F4)
                    continue;
                for (c = 0; c < 4; c++) {
                    if (p->xy[c].x < lo) lo = p->xy[c].x;
                    if (p->xy[c].x > hi) hi = p->xy[c].x;
                }
                width[n++] = hi - lo;
            }

            /* Three quads per ghost, emitted together, so element e's width is
             * at index 3e. */
            for (e = 1; e + 1 < style->count; e++) {
                u32 a = (e - 1) * (Q2_FLARE_DISC_SIDES / 2);
                u32 b = e * (Q2_FLARE_DISC_SIDES / 2);

                if (b >= n)
                    break;
                check((width[a] > width[b]) ==
                      (style->element[e].size > style->element[e + 1].size),
                      "each ghost's width follows its own `size`, including "
                      "the last one, which grows again");
            }

            check(n >= 3 && width[0] > width[n - 1],
                  "and the first ghost is the widest of them");
        }
    }

    psx_ot_free(&ot);
}

int main(void)
{
    printf("light model tests\n\n");

    test_attenuation();
    test_wrapping_delta();
    test_ranking();
    test_signed_f4_branch();
    test_env();
    test_normalise();
    test_ncs();
    test_ncs_coloured();
    test_flare_styles();
    test_flare_geometry();
    test_glow_fade();
    test_dynamic();
    test_muzzle_light();
    test_flklight();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
