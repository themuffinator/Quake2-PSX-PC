/*
 * test_sound_pitch.c — retail sound-request pitch and host-mixer rate.
 *
 * These checks pin the byte-state transitions at 0x80071BF8/0x80073B14/
 * 0x8007270C and the two integer truncations at 0x800724EC/0x80071CA0.
 * CD register fixtures come from executing the PAL setup/SDK routines.
 * No host audio device is involved.
 */
#include <stdio.h>
#include <string.h>

#include "vag.h"
#include "xa.h"
#include "fixtures/audio_register_reference.h"

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

static void check_eq_s32(s32 got, s32 want, const char *what)
{
    g_checks++;
    if (got != want) {
        printf("  FAIL  %s: got %d, want %d\n", what, (int)got, (int)want);
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

/*
 * CdCheckMusic's volume ramp — 0x800718F8..0x8007194C.
 *
 * The console keeps a target at 0x800B2E50 and a live value at 0x800B2E74 and
 * moves the live one by FOUR a call, clamping to the target on overshoot. It is
 * the live value the CD mix is built from (0x80071958), which is why a pause
 * slides the music away instead of cutting it. The port had no such pair at
 * all: it scaled the bed straight off the MUSIC slider, so nothing ever ramped.
 */
static void test_cd_volume_ramp(void)
{
    s16 v;
    int steps;

    puts("CD volume ramp");

    check_eq_s32(q2_xa_cd_volume_step(0, 96), 4,
                 "the first step off silence is four");
    check_eq_s32(q2_xa_cd_volume_step(96, 96), 96,
                 "an arrived value does not move");
    check_eq_s32(q2_xa_cd_volume_step(96, 0), 92,
                 "and it comes down by four");

    /* 0x80071944/0x8007194C: overshoot in either direction takes the target
     * outright rather than stepping past it. */
    check_eq_s32(q2_xa_cd_volume_step(94, 96), 96, "an over-step clamps up");
    check_eq_s32(q2_xa_cd_volume_step(2, 0), 0, "an under-step clamps down");
    check_eq_s32(q2_xa_cd_volume_step(252, 254), 254,
                 "the maximum clamps the same way");

    /* The default MUSIC slider is 48 and the target is that doubled, so a fade
     * is twenty-four calls — under half a second of 50 Hz fields. */
    for (v = 0, steps = 0; v != 96 && steps < 1000; steps++)
        v = q2_xa_cd_volume_step(v, 96);
    check_eq_s32(steps, 24, "silence to the default 96 takes 24 calls");

    for (v = 96, steps = 0; v != 0 && steps < 1000; steps++)
        v = q2_xa_cd_volume_step(v, 0);
    check_eq_s32(steps, 24, "and the duck back to silence takes 24");

    /* The top of the slider is 127, doubled to 254: sixty-four, which is also
     * the width of the two track fades and not a coincidence. */
    for (v = 0, steps = 0; v != 254 && steps < 1000; steps++)
        v = q2_xa_cd_volume_step(v, 254);
    check_eq_s32(steps, 64, "silence to the maximum 254 takes 64 calls");
}

/*
 * The four mix bytes CdCheckMusic builds before handing the stream to the
 * drive — 0x800719EC..0x80071A20, branching on the STEREO setting at
 * 0x800B333E. Stereo scales each channel on its own; mono puts volume/2 into
 * all four entries, so both channels are summed into both outputs at half
 * level. The port applied the volume and ignored the setting.
 */
static void test_cd_mix_stereo_fold(void)
{
    s16 pcm[4];

    puts("CD mix and the STEREO fold");

    /* The CD controller's unity byte is 128; the game's ceiling 255 is
     * nearly double gain, so it can saturate before reaching the SPU. */
    pcm[0] = 1000; pcm[1] = -1000; pcm[2] = 32767; pcm[3] = -32768;
    q2_xa_cd_mix(pcm, 4, 128, true);
    check_eq_s32(pcm[0],  1000, "stereo at 128 leaves the left channel");
    check_eq_s32(pcm[1], -1000, "stereo at 128 leaves the right channel");
    check_eq_s32(pcm[3], -32768, "stereo at 128 leaves the extreme");

    /* Stereo below unity scales the two independently and keeps them apart. */
    pcm[0] = 1000; pcm[1] = -1000;
    q2_xa_cd_mix(pcm, 2, 96, true);
    check_eq_s32(pcm[0],  750, "stereo at 96 scales the left channel");
    check_eq_s32(pcm[1], -750, "stereo at 96 scales the right channel");

    /*
     * Mono sums both channels at half level, so a hard-panned pair collapses
     * to the centre and an out-of-phase pair cancels. That is the whole of the
     * symptom: with STEREO off the effects went centre and the music did not.
     */
    pcm[0] = 1000; pcm[1] = -1000;
    q2_xa_cd_mix(pcm, 2, 128, false);
    check_eq_s32(pcm[0], 0, "mono cancels an out-of-phase pair, left");
    check_eq_s32(pcm[1], 0, "mono cancels an out-of-phase pair, right");

    pcm[0] = 1000; pcm[1] = 0;
    q2_xa_cd_mix(pcm, 2, 96, false);
    check_eq_s32(pcm[0], 375, "mono at 96 sums a hard-left pair");
    check_eq_s32(pcm[1], 375, "...into both outputs");

    /*
     * AND THE HALVING IS ON THE VOLUME, not on the sum. 0x80071A08 computes
     * volume/2 truncated toward zero and writes that byte four times, so an odd
     * volume loses half a unit. Each input's fixed-point product truncates
     * before summation; an out-of-phase pair can therefore retain a -1 LSB.
     */
    pcm[0] = 1000; pcm[1] = 1000;
    q2_xa_cd_mix(pcm, 2, 255, false);
    check_eq_s32(pcm[0], 1984, "odd volume halves the register before multiplication");

    /* A zero-length buffer and a null one are both no-ops rather than faults. */
    q2_xa_cd_mix(pcm, 0, 96, false);
    check_eq_s32(pcm[0], 1984, "an empty buffer is untouched");
    q2_xa_cd_mix(NULL, 2, 96, false);

    pcm[0] = 1000; pcm[1] = -1000;
    q2_xa_cd_mix(pcm, 2, 255, true);
    check_eq_s32(pcm[0], 1992, "255 is nearly double gain");
    check_eq_s32(pcm[1], -1993, "negative products use arithmetic truncation");
    pcm[0] = 20000; pcm[1] = -20000;
    q2_xa_cd_mix(pcm, 2, 255, true);
    check_eq_s32(pcm[0], 32767, "CD matrix saturates positive overflow");
    check_eq_s32(pcm[1], -32768, "CD matrix saturates negative overflow");
}

static s32 reference_cd_product(s32 sample, u8 gain)
{
    s32 product = sample * gain;
    return product >= 0 ? product / 128 : -((127 - product) / 128);
}

static void test_native_cd_registers(void)
{
    static const s16 input[][2] = {
        {1000,-1000}, {0,32767}, {-32768,0}, {16384,16384},
        {-16384,16384}, {1,-1}, {129,-129}, {12345,-23456}
    };
    u32 stereo, volume, i, ch;
    puts("512 native CD register matrices with fixed-point scaling and saturation");
    check(audio_reference_main[0] == 0x3fc0 && audio_reference_main[1] == 0x3fc0,
          "retail SDK writes both main volume registers");
    check(audio_reference_cd_input[0] == 0x7fff && audio_reference_cd_input[1] == 0x7fff,
          "retail SDK writes both CD input volume registers");
    for (stereo = 0; stereo < 2; stereo++) for (volume = 0; volume < 256; volume++) {
        const u8 *atv = audio_reference_atv[stereo][volume];
        for (i = 0; i < Q2PSX_ARRAY_COUNT(input); i++) {
            s16 pcm[2] = {input[i][0], input[i][1]};
            s32 expected[2] = {
                reference_cd_product(input[i][0], atv[0]) + reference_cd_product(input[i][1], atv[3]),
                reference_cd_product(input[i][0], atv[1]) + reference_cd_product(input[i][1], atv[2])
            };
            q2_xa_cd_mix(pcm, 2, (s32)volume, stereo != 0);
            for (ch = 0; ch < 2; ch++) {
                s32 value = expected[ch];
                if (value > 32767) value = 32767;
                if (value < -32768) value = -32768;
                check_eq_s32(pcm[ch], value, "native ATV bytes produce the expected channel");
            }
        }
    }
}

/*
 * BYTE +2 OF THE REQUEST RECORD — its relative volume, and the one request on
 * the disc that does not keep the default.
 *
 * PreInitChaingun (0x8004E050) clones wep_machgf1b and takes the clone down by
 * 40; QPlayerFireChainGun then plays THAT clone once per bullet at the player's
 * origin (0x8004CCB4) and the untouched machinegun request once at the end of
 * the loop (0x8004CCD8). This port had no per-request volume at all, so the
 * chaingun's bullets — when it raised any — were as loud as a machinegun shot.
 */
static void test_request_volume(void)
{
    printf("request volume\n");

    check_eq_s32(Q2_SFX_VOLUME_DEFAULT, 63,
                 "a fresh request is 63 (0x80071C0C / 0x80071C5C)");

    /* The chaingun's clone, 0x8004E2D8. */
    check_eq_s32(q2_sfx_volume_add((u8)Q2_SFX_VOLUME_DEFAULT,
                                   Q2_SFX_VOLUME_CHAINGUN_DELTA),
                 23, "minus forty leaves the chaingun's clone at 23");

    /*
     * And the three `+40` calls that follow it are no-ops, because the clamp
     * at 0x80072BE8 pins anything that reached 64 back to 63. A port that
     * clamped BEFORE adding, or that saturated at 255, would make the chaingun
     * spin-up louder than every other sound in the game.
     */
    check_eq_s32(q2_sfx_volume_add((u8)Q2_SFX_VOLUME_DEFAULT, 40), 63,
                 "plus forty on a default request changes nothing");
    check_eq_s32(q2_sfx_volume_add(23, 40), 63,
                 "and on the clone it pins at 63 rather than reaching 63 by sum");
    check_eq_s32(q2_sfx_volume_add(10, 40), 50,
                 "an addition that stays below 64 is kept");

    /* The `a1 == 128` early-out at 0x80072BB8. */
    check_eq_s32(q2_sfx_volume_add(23, Q2_SFX_VOLUME_UNCHANGED), 23,
                 "a delta of 128 is the leave-alone sentinel");

    /* The byte WRAPS before it is tested: 63 + 200 is 263, stored as 7. */
    check_eq_s32(q2_sfx_volume_add(63, 200), 7,
                 "the store is a byte, so it wraps before the clamp");

    /* 0x80072AF0: what reaches the voice. SoundAtLocation's scale is 16, so a
     * default request is 63 and the chaingun's clone 23 — 36%. */
    check_eq_s32(q2_sfx_voice_volume(63, Q2_SFX_VOLUME_SCALE_AT_LOCATION), 63,
                 "a default request reaches the voice at 63");
    check_eq_s32(q2_sfx_voice_volume(23, Q2_SFX_VOLUME_SCALE_AT_LOCATION), 23,
                 "and the chaingun's clone at 23");
}

int main(void)
{
    test_request_state();
    test_start_paths();
    test_observed_ranges();
    test_rate_conversion();
    test_request_volume();
    test_cd_volume_ramp();
    test_cd_mix_stereo_fold();
    test_native_cd_registers();

    if (g_failures) {
        printf("\n%d/%d sound-pitch checks failed\n", g_failures, g_checks);
        return 1;
    }

    printf("\nall %d sound-pitch checks passed\n", g_checks);
    return 0;
}
