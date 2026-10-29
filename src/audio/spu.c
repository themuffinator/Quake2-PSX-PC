#include "spu.h"

#include <string.h>

#include "spu_gauss.h"

s16 q2_spu_gaussian(const s16 history[4], u32 fraction)
{
    u32 i = (fraction >> 4) & 255;
    /* Retain one guard bit per product, then discard it after addition.
     * Shifting every tap by 15 independently loses that bit four times.
     * The hardware captures in test_spu.c distinguish these operations. */
    s32 sum = ((s32)spu_gauss[255 - i] * history[0]) >> 14;
    sum += ((s32)spu_gauss[511 - i] * history[1]) >> 14;
    sum += ((s32)spu_gauss[256 + i] * history[2]) >> 14;
    sum += ((s32)spu_gauss[i] * history[3]) >> 14;
    return (s16)(sum >> 1);
}

void q2_spu_envelope_step(q2_spu_envelope *e)
{
    u32 shift, bits = 0, increment;
    bool decrease = false, exponential = false, frozen = false;
    s32 step;
    switch (e->phase) {
    case Q2_SPU_ATTACK:
        shift = (e->adsr1 >> 10) & 31;
        bits = (e->adsr1 >> 8) & 3;
        exponential = (e->adsr1 & 0x8000) != 0;
        frozen = (e->adsr1 & 0x7f00) == 0x7f00;
        break;
    case Q2_SPU_DECAY:
        shift = (e->adsr1 >> 4) & 15;
        decrease = exponential = true;
        break;
    case Q2_SPU_SUSTAIN:
        shift = (e->adsr2 >> 8) & 31;
        bits = (e->adsr2 >> 6) & 3;
        decrease = (e->adsr2 & 0x4000) != 0;
        exponential = (e->adsr2 & 0x8000) != 0;
        frozen = (e->adsr2 & 0x1fc0) == 0x1fc0;
        break;
    case Q2_SPU_RELEASE:
        shift = e->adsr2 & 31;
        decrease = true;
        exponential = (e->adsr2 & 0x20) != 0;
        frozen = shift == 31;
        break;
    default:
        return;
    }
    if (frozen) return;
    step = decrease ? (s32)bits - 8 : 7 - (s32)bits;
    if (shift < 11) step *= 1 << (11 - shift);
    increment = 0x8000u >> (shift > 11 ? shift - 11 : 0);
    if (exponential) {
        if (decrease) step = (step * e->level) >> 15;
        else if (e->level > 0x6000) {
            if (shift < 10) step >>= 2;
            else if (shift >= 11) increment >>= 2;
            else { step >>= 1; increment >>= 1; }
        }
    }
    if (!increment) increment = 1;
    e->counter += increment;
    if (e->counter < 0x8000) return;
    e->counter = 0;
    e->level += step;
    if (e->level < 0) e->level = 0;
    if (e->level > 0x7fff) e->level = 0x7fff;
    if (e->phase == Q2_SPU_ATTACK && e->level == 0x7fff)
        e->phase = Q2_SPU_DECAY;
    else if (e->phase == Q2_SPU_DECAY && e->level <= ((e->adsr1 & 15) + 1) * 0x800)
        e->phase = Q2_SPU_SUSTAIN;
    else if (e->phase == Q2_SPU_RELEASE && !e->level)
        e->phase = Q2_SPU_OFF;
}

void q2_spu_player_start(q2_spu_player *p, const u8 *data, u32 size,
                         u16 pitch, u16 adsr1, u16 adsr2)
{
    memset(p, 0, sizeof(*p));
    q2_spu_voice_start(&p->dec, data, size);
    p->pitch = pitch;
    p->env.adsr1 = adsr1;
    p->env.adsr2 = adsr2;
    p->env.phase = Q2_SPU_ATTACK;
    p->active = !p->dec.done;
}

void q2_spu_player_key_off(q2_spu_player *p)
{
    if (p->active && p->env.phase != Q2_SPU_RELEASE) {
        p->env.phase = Q2_SPU_RELEASE;
        p->env.counter = 0;
    }
}

s16 q2_spu_player_next(q2_spu_player *p)
{
    s32 sample;
    if (!p->active) return 0;
    /* The pitch register is 4.12 in units of source samples per output
     * clock; bits 4..11 of its fractional accumulator index the filter.
     * Hardware caps a 16-bit pitch at four source samples per clock. */
    while (!p->primed || p->counter >= 0x1000) {
        if (p->primed) p->counter -= 0x1000;
        p->primed = true;
        if (p->index == p->have) {
            p->have = q2_spu_voice_block(&p->dec, p->block);
            p->index = 0;
            if (!p->have) {
                /* END without REPEAT mutes after its block's final interval;
                 * Gaussian history must not extend a one-shot past END. */
                p->env.level = 0;
                p->env.phase = Q2_SPU_OFF;
                p->active = false;
                return 0;
            }
        }
        memmove(p->history, p->history + 1, 3 * sizeof(s16));
        p->history[3] = p->block[p->index++];
    }
    sample = ((s32)q2_spu_gaussian(p->history, p->counter) * p->env.level) >> 15;
    q2_spu_envelope_step(&p->env);
    if (p->env.phase == Q2_SPU_OFF) p->active = false;
    p->counter += p->pitch > 0x4000 ? 0x4000 : p->pitch;
    return (s16)sample;
}

s16 q2_spu_player_sample(q2_spu_player *p, u32 output_rate)
{
    s32 out;
    if (!output_rate) return 0;
    if (output_rate == Q2_SPU_OUTPUT_RATE) return q2_spu_player_next(p);
    if (!p->output_ready) {
        p->previous = q2_spu_player_next(p);
        p->next = q2_spu_player_next(p);
        p->output_ready = true;
    }
    out = p->previous + (s32)(((s64)p->next - p->previous) *
                             (s64)p->output_phase / output_rate);
    p->output_phase += Q2_SPU_OUTPUT_RATE;
    while (p->output_phase >= output_rate) {
        p->output_phase -= output_rate;
        p->previous = p->next;
        p->next = q2_spu_player_next(p);
    }
    return (s16)out;
}

bool q2_spu_player_alive(const q2_spu_player *p)
{
    return p->active || p->previous != 0 || p->next != 0;
}
