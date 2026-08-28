/*
 * test_sound_pitch.c — retail sound-request pitch and host-mixer rate.
 *
 * These checks pin the byte-state transitions at 0x80071BF8/0x80073B14/
 * 0x8007270C and the two integer truncations at 0x800724EC/0x80071CA0.
 * No fixture or host audio device is involved.
 */
#include <stdio.h>
#include <string.h>

#include "vag.h"

static int g_checks;
static int g_failures;

static void check(bool condition, const char *what)
{
    g_checks++;
    if (!condition) {
        printf("  FAIL  %s\n", what);
        g_failures++;
    }
}

static void check_eq_u32(u32 got, u32 want, const char *what)
{
    g_checks++;
    if (got != want) {
        printf("  FAIL  %s: got %u, want %u\n", what, got, want);
        g_failures++;
    }
}

static bool pitch_eq(const q2_sfx_pitch *a, const q2_sfx_pitch *b)
{
    return a->current == b->current && a->upper == b->upper &&
           a->lower == b->lower && a->varying == b->varying;
}

static void test_request_state(void)
{
    q2_sfx_pitch pitch, before;

    puts("sound request defaults and setters");

    memset(&pitch, 0xA5, sizeof(pitch));
    q2_sfx_pitch_init(&pitch);
    check_eq_u32(pitch.current, 35, "default current modifier");
    check_eq_u32(pitch.upper,   35, "default upper byte");
    check_eq_u32(pitch.lower,   35, "default lower byte");
    check(!pitch.varying, "default request has no variation flag");

    q2_sfx_pitch_set(&pitch, 0);
    check_eq_u32(pitch.current, 35, "zero setter is the unchanged sentinel");
    q2_sfx_pitch_set(&pitch, 40);
    check_eq_u32(pitch.current, 40, "nonzero setter replaces current pitch");
    q2_sfx_pitch_add(&pitch, -5);
    check_eq_u32(pitch.current, 35, "signed adjustment changes current pitch");
    q2_sfx_pitch_add(&pitch, 0);
    check_eq_u32(pitch.current, 35, "zero adjustment is ignored");
    q2_sfx_pitch_add(&pitch, -40);
    check_eq_u32(pitch.current, 251, "adjustment retains retail byte wrap");
    q2_sfx_pitch_set(&pitch, 40);

    check(q2_sfx_pitch_set_range(&pitch, 4, -5),
          "valid signed offsets configure a range");
    check_eq_u32(pitch.upper, 44, "upper is relative to current");
    check_eq_u32(pitch.lower, 35, "lower is relative to current");
    check(pitch.varying, "range sets the variation flag");

    before = pitch;
    check(!q2_sfx_pitch_set_range(&pitch, 0, 0),
          "empty range is rejected");
    check(pitch_eq(&pitch, &before),
          "a rejected range leaves the request unchanged");
    check(!q2_sfx_pitch_set_range(&pitch, 300, -300),
          "range outside byte storage is rejected");
    check(pitch_eq(&pitch, &before),
          "out-of-byte range also leaves the request unchanged");
}

static void test_start_paths(void)
{
    q2_sfx_pitch pitch;

    puts("randomising 0x8007270C and direct 0x80072A00 start paths");

    q2_sfx_pitch_init(&pitch);
    check(q2_sfx_pitch_set_range(&pitch, 4, -5),
          "gasp/drown offsets form a valid request");
    check(!q2_sfx_pitch_needs_random(&pitch, false),
          "direct start consumes no random number");
    check(q2_sfx_pitch_needs_random(&pitch, true),
          "randomising start consumes one random number");

    check_eq_u32(q2_sfx_pitch_begin(&pitch, false, 8), 35,
                 "direct start ignores a configured range");
    check_eq_u32(pitch.current, 35,
                 "direct start does not mutate current pitch");

    check_eq_u32(q2_sfx_pitch_begin(&pitch, true, 0), 30,
                 "random zero selects the inclusive lower bound");
    check_eq_u32(q2_sfx_pitch_begin(&pitch, true, 8), 38,
                 "range width minus one selects the last value");
    check_eq_u32(q2_sfx_pitch_begin(&pitch, true, 9), 30,
                 "upper bound is exclusive and modulo wraps");
    check_eq_u32(q2_sfx_pitch_begin(&pitch, false, 1234), 30,
                 "direct start keeps the last current byte");

    q2_sfx_pitch_init(&pitch);
    check(!q2_sfx_pitch_needs_random(&pitch, true),
          "fixed request consumes no random number on either path");
    check_eq_u32(q2_sfx_pitch_begin(&pitch, true, 1234), 35,
                 "fixed request remains at the retail default");
}

static void test_observed_ranges(void)
{
    q2_sfx_pitch pitch;

    puts("core executable's observed signed range configurations");

    q2_sfx_pitch_init(&pitch);
    check(q2_sfx_pitch_set_range(&pitch, 4, -5), "gasp/drown range");
    check_eq_u32(pitch.lower, 30, "gasp/drown lower");
    check_eq_u32(pitch.upper, 39, "gasp/drown exclusive upper");

    q2_sfx_pitch_init(&pitch);
    check(q2_sfx_pitch_set_range(&pitch, 0, -2), "burn range");
    check_eq_u32(pitch.lower, 33, "burn lower");
    check_eq_u32(pitch.upper, 35, "burn exclusive upper");

    q2_sfx_pitch_init(&pitch);
    check(q2_sfx_pitch_set_range(&pitch, 3, -3), "wade range");
    check_eq_u32(pitch.lower, 32, "wade lower");
    check_eq_u32(pitch.upper, 38, "wade exclusive upper");

    q2_sfx_pitch_init(&pitch);
    check(q2_sfx_pitch_set_range(&pitch, 8, 0),
          "second machinegun request range");
    check_eq_u32(pitch.lower, 35, "machinegun request lower");
    check_eq_u32(pitch.upper, 43, "machinegun request exclusive upper");

    q2_sfx_pitch_init(&pitch);
    check(q2_sfx_pitch_set_range(&pitch, 24, 16),
          "direct-path machinegun clone still stores its range");
    check_eq_u32(q2_sfx_pitch_begin(&pitch, false, 0), 35,
                 "direct path bypasses the clone's 51..58 variation");
}

static void test_rate_conversion(void)
{
    puts("VAG rate -> SPU pitch -> 16.16 host-mixer step");

    check_eq_u32(q2_sfx_spu_pitch(11025, Q2_SFX_PITCH_UNITY), 1024,
                 "11025 Hz at unity is SPU pitch 0x0400");
    check_eq_u32(q2_sfx_spu_pitch(22050, Q2_SFX_PITCH_UNITY), 2048,
                 "22050 Hz at unity is SPU pitch 0x0800");
    check_eq_u32(q2_sfx_spu_pitch(11025, Q2_SFX_PITCH_DEFAULT), 1120,
                 "11025 Hz at default modifier is SPU pitch 0x0460");
    check_eq_u32(q2_sfx_spu_pitch(22050, Q2_SFX_PITCH_DEFAULT), 2240,
                 "22050 Hz at default modifier is SPU pitch 0x08C0");

    check_eq_u32(q2_sfx_step_16_16(11025, Q2_SFX_PITCH_UNITY, 37800), 19114,
                 "11025 unity step into the XA-rate mixer");
    check_eq_u32(q2_sfx_step_16_16(11025, Q2_SFX_PITCH_DEFAULT, 37800), 20906,
                 "11025 default step into the XA-rate mixer");
    check_eq_u32(q2_sfx_step_16_16(22050, Q2_SFX_PITCH_DEFAULT, 37800), 41813,
                 "22050 default step into the XA-rate mixer");
    check_eq_u32(q2_sfx_step_16_16(11025, Q2_SFX_PITCH_DEFAULT, 44100), 17920,
                 "SPU-rate host step preserves the hardware pitch value");

    /* This non-disc rate pins the intermediate SPU quantisation rather than a
     * tempting single ideal-ratio division: base 0x5CE, scaled pitch 1625. */
    check_eq_u32(q2_sfx_spu_pitch(16000, Q2_SFX_PITCH_DEFAULT), 1625,
                 "arbitrary rate keeps the sample-load truncation");
    check_eq_u32(q2_sfx_step_16_16(16000, Q2_SFX_PITCH_DEFAULT, 37800), 30333,
                 "host step is derived from the quantised SPU pitch");

    check_eq_u32(q2_sfx_step_16_16(11025, Q2_SFX_PITCH_DEFAULT, 0), 0,
                 "zero host output rate is rejected");
    check_eq_u32(q2_sfx_step_16_16(0xFFFFFFFFu, 255, 1), 0xFFFFFFFFu,
                 "unrepresentable host step saturates instead of wrapping");
}

int main(void)
{
    test_request_state();
    test_start_paths();
    test_observed_ranges();
    test_rate_conversion();

    if (g_failures) {
        printf("\n%d/%d sound-pitch checks failed\n", g_failures, g_checks);
        return 1;
    }

    printf("\nall %d sound-pitch checks passed\n", g_checks);
    return 0;
}
