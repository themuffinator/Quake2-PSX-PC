#include "spu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixtures/spu_reference.h"

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static void test_hardware(void)
{
    u32 c;
    for (c = 0; c < Q2PSX_ARRAY_COUNT(spu_references); c++) {
        const spu_reference *r = &spu_references[c];
        q2_spu_player player;
        s16 output[1536], matched[1536];
        u32 i, phase, initial, best = 0, best_initial = 0, best_bad = 0xffffffffu;
        /* The capture uses ADSR 000f/1fc0. After the fast attack/decay it
         * holds ENVX=3fff, not 7fff. No main/voice-volume gain is in these
         * pre-volume SPU-RAM captures. Startup CPU/SPU phase is excluded. */
        CHECK(r->count > 0 && r->count <= 512);
        if (!r->count || r->count > 512) continue;
        /* Two upstream captures cannot absorb their unknown key-on source
         * phase by an output rotation: gcd(21,28)=7 and gcd(2,28)=2. Test
         * those documented phase classes explicitly, without any amplitude
         * tolerance. This establishes steady DSP, not key-on bus timing. */
        for (initial = 0; initial < r->source_phases; initial++) {
            q2_spu_player_start(&player, r->adpcm, 64, r->pitch, 0x000f, 0x1fc0);
            player.counter = initial << 12;
            for (i = 0; i < 4096; i++) q2_spu_player_next(&player);
            CHECK(player.active && player.env.level == 0x3fff);
            for (i = 0; i < Q2PSX_ARRAY_COUNT(output); i++) output[i] = q2_spu_player_next(&player);
            for (phase = 0; phase < 512; phase++) {
                u32 bad = 0;
                for (i = 0; i < r->count; i++) bad += output[phase + i] != r->pcm[i];
                if (bad < best_bad) {
                    best_bad = bad; best = phase; best_initial = initial;
                    memcpy(matched, output, sizeof(matched));
                }
            }
        }
        printf("hardware %-38s %u/%u exact (phase %u, source %u)\n",
               r->name, r->count - best_bad, r->count, best, best_initial);
        CHECK(best_bad == 0);
        /* Check repeated periods too. A short vector must not pass merely
         * because one accidental match can be found somewhere in a ring. */
        for (i = 0; i < 768; i++) CHECK(matched[best + i] == r->pcm[i % r->count]);
    }
}

static void test_envelope(void)
{
    /* First nonzero values independently measured by Nugget's
     * adsr_attack_linear_shift test. Slow clocks are checked on either side
     * of their first update, so a phase transition cannot skip the wait. */
    const struct { u16 adsr; u32 ticks; s32 value; } first[] = {
        {0x0000, 1, 0x3800}, {0x0100, 1, 0x3000},
        {0x2c00, 1, 7}, {0x2d00, 1, 6}, {0x3200, 2, 5},
        {0x3300, 2, 4}, {0x6200, 8192, 5}, {0x6300, 8192, 4}
    };
    const s32 native[] = {14336, 28672, 32767, 16383, 8191, 4095,
                          2047, 16383, 30719, 32767, 32767};
    q2_spu_envelope e = {0};
    u32 i, n;
    for (i = 0; i < Q2PSX_ARRAY_COUNT(first); i++) {
        memset(&e, 0, sizeof(e)); e.adsr1 = first[i].adsr; e.phase = Q2_SPU_ATTACK;
        for (n = 1; n < first[i].ticks; n++) q2_spu_envelope_step(&e);
        CHECK(e.level == 0);
        q2_spu_envelope_step(&e); CHECK(e.level == first[i].value);
    }
    memset(&e, 0, sizeof(e)); e.phase = Q2_SPU_ATTACK;
    for (i = 0; i < Q2PSX_ARRAY_COUNT(native); i++) {
        q2_spu_envelope_step(&e); CHECK(e.level == native[i]);
    }
    e.phase = Q2_SPU_RELEASE; q2_spu_envelope_step(&e);
    CHECK(e.level == 16383 && e.phase == Q2_SPU_RELEASE);
    q2_spu_envelope_step(&e); CHECK(e.level == 0 && e.phase == Q2_SPU_OFF);
    memset(&e, 0, sizeof(e)); e.adsr1 = 0x7f00; e.phase = Q2_SPU_ATTACK;
    for (i = 0; i < 65536; i++) q2_spu_envelope_step(&e);
    CHECK(e.level == 0 && e.phase == Q2_SPU_ATTACK);
    /* Decay checks its threshold only when the slow rate takes a step. */
    e.adsr1 = 0xff; e.level = 0x7fff; e.phase = Q2_SPU_DECAY;
    for (i = 0; i < 15; i++) q2_spu_envelope_step(&e);
    CHECK(e.level == 0x7fff && e.phase == Q2_SPU_DECAY);
    q2_spu_envelope_step(&e);
    CHECK(e.level == 0x7ff7 && e.phase == Q2_SPU_SUSTAIN);
}

static void test_boundaries(void)
{
    q2_spu_player p, control;
    u8 data[32] = {0};
    u32 i;
    s16 last = 0;
    memset(data + 2, 0x11, 14); data[1] = SPU_FLAG_END;
    q2_spu_player_start(&p, data, 16, 0x800, 0, 0);
    for (i = 0; i < 56; i++) last = q2_spu_player_next(&p);
    CHECK(last != 0 && p.active);
    CHECK(q2_spu_player_next(&p) == 0 && !p.active && p.env.level == 0);
    data[1] = SPU_FLAG_END | SPU_FLAG_REPEAT | SPU_FLAG_LOOP_START;
    q2_spu_player_start(&p, data, 16, 0x1000, 0, 0);
    for (i = 0; i < 100; i++) q2_spu_player_next(&p);
    CHECK(p.active && p.dec.looped);
    q2_spu_player_key_off(&p);
    CHECK(q2_spu_player_next(&p) != 0);
    CHECK(q2_spu_player_next(&p) != 0);
    CHECK(q2_spu_player_next(&p) == 0 && !p.active);
    /* Empty/malformed data cannot retain stale output from a prior voice. */
    q2_spu_player_start(&p, NULL, 0, 0x1000, 0, 0);
    CHECK(q2_spu_player_sample(&p, 37800) == 0 && !q2_spu_player_alive(&p));
    for (i = 0; i < 65536; i += 4096) {
        u32 t;
        q2_spu_player_start(&p, data, 16, (u16)i, 0, 0);
        q2_spu_player_start(&control, data, 16, (u16)(i > 0x4000 ? 0x4000 : i), 0, 0);
        for (t = 0; t < 97; t++) CHECK(q2_spu_player_next(&p) == q2_spu_player_next(&control));
        CHECK(p.counter == control.counter && p.index == control.index);
    }
    q2_spu_player_start(&p, data, 16, 0, 0, 0);
    for (i = 0; i < 100; i++) q2_spu_player_next(&p);
    CHECK(p.index == 1 && p.counter == 0 && p.env.level == 32767);
}

static void test_output_clock(void)
{
    q2_spu_player p, native;
    s16 source[1200];
    u32 i;
    const spu_reference *r = &spu_references[0];
    q2_spu_player_start(&p, r->adpcm, 64, 0x1379, 0, 0);
    native = p;
    for (i = 0; i < Q2PSX_ARRAY_COUNT(source); i++) source[i] = q2_spu_player_next(&native);
    for (i = 0; i < 1000; i++) {
        u32 j = i * 7 / 6, fraction = i * 7 % 6;
        s32 expected = source[j] + ((s32)source[j + 1] - source[j]) * (s32)fraction / 6;
        CHECK(q2_spu_player_sample(&p, 37800) == expected);
    }
    CHECK(p.output_phase == 4 * 6300); /* exact rational phase, no 16.16 drift */
}

int main(void)
{
    test_hardware(); test_envelope(); test_boundaries(); test_output_clock();
    printf("SPU: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
