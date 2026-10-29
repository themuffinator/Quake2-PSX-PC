#include "spatial.h"

static u32 floor_root(u64 n)
{
    u64 bit = (u64)1 << 62, root = 0;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= root + bit) { n -= root + bit; root = (root >> 1) + bit; }
        else root >>= 1;
        bit >>= 2;
    }
    return (u32)root;
}

u32 q2_sfx_distance_root(u32 n)
{
    u32 bits = n, leading = 0, normalized, seed;
    if (!n) return 0;
    /* Retail's signed LZCS/table lookup is undefined outside its positive
     * domain. Keep extreme host coordinates bounded without an invalid read. */
    if (n > 0x7FFFFFFFu) return floor_root(n);
    while (!(bits & 0x80000000u)) { leading++; bits <<= 1; }
    leading &= ~1u;
    normalized = leading >= 24 ? n << (leading - 24) : n >> (24 - leading);
    /* Reconstruct the mathematical table entry; no copied SDK table. */
    seed = floor_root((u64)normalized << 18);
    return (seed << ((31 - leading) / 2)) >> 12;
}

q2_sfx_spatial q2_sfx_spatialize(const s32 at[3], u16 volume,
                                const q2_sfx_listener *listeners, u32 count)
{
    q2_sfx_spatial out = {0};
    u32 p;
    if (!at || !listeners || !count) { out.distance = 1; out.pan = 15; return out; }
    for (p = 0; p < count; p++) {
        s16 d[3];
        u32 r, k;
        for (k = 0; k < 3; k++)
            d[k] = (s16)((u32)at[k] - (u32)listeners[p].pos[k]);
        for (r = 0; r < 3; r++) {
            u32 sum = 0;
            for (k = 0; k < 3; k++)
                sum += (u32)((s32)d[k] * listeners[p].matrix[r][k]);
            out.projected[r] = (s32)sum >> 12;
        }
        out.distance += (s32)q2_sfx_distance_root((u32)(
            (s64)out.projected[0] * out.projected[0] +
            (s64)out.projected[2] * out.projected[2]));
    }
    out.distance /= (s32)count;
    if (out.distance < 1) out.distance = 1;
    out.level = (s32)(((s64)12288 - out.distance) * volume / 4096);
    if (out.level < 0) out.level = 0;
    if (out.level > 255) out.level = 255;
    out.pan = 15 + out.projected[0] * 15 / out.distance;
    /* With several windows the last vector can exceed the averaged distance.
     * Clamp the lookup to its defined 31-entry domain. */
    if (out.pan < 0) out.pan = 0;
    if (out.pan > 30) out.pan = 30;
    return out;
}

s32 q2_sfx_volume_register(u16 level, u8 pan, u8 slider, bool mono)
{
    u32 value = mono ? (u32)level * slider / 2 :
        (u32)level * pan * slider / 512;
    if (value > 22000) value = 22000;
    /* SpuSetVoiceAttr masks bit 15, without halving. Direct volume is a
     * signed 15-bit register; all normal game levels stay below 0x4000. */
    return (s32)(value & 0x3FFF) - (s32)(value & 0x4000);
}
