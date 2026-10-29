#include "sdkmath.h"
#include "fixed.h"

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

u32 q2_sdk_sqrt0(u32 squared)
{
    u32 leading, normalized, seed;
    if (!squared || squared > 0x7FFFFFFFu) return floor_root(squared);
    leading = (u32)q2_count_leading_zeros_u32(squared) & ~1u;
    normalized = leading >= 24 ? squared << (leading - 24) : squared >> (24 - leading);
    seed = floor_root((u64)normalized << 18);
    return (seed << ((31 - leading) / 2)) >> 12;
}

u32 q2_sdk_sqrt_refined(u32 squared)
{
    u32 root = q2_sdk_sqrt0(squared);
    int i;
    if (!root || squared > 0x7FFFFFFFu) return root;
    for (i = 0; i < 2; i++) {
        s32 error = (s32)(root * root - squared);
        root = (u32)((s32)root - error / (s32)(root * 2u));
    }
    return root;
}

void q2_sdk_vector_normal(const s32 input[3], s32 output[3])
{
    s16 v[3];
    u32 squared = 0, leading, normalized, seed, shift;
    int k;
    /* MTC2 IR1..3 truncates to signed halfwords before SQR. */
    for (k = 0; k < 3; k++) {
        v[k] = (s16)input[k];
        squared += (u32)((s32)v[k] * v[k]);
    }
    if (!squared || squared > 0x7FFFFFFFu) {
        u32 length = floor_root(squared);
        for (k = 0; k < 3; k++) output[k] = length ? (s32)((s64)v[k] * 4096 / length) : 0;
        return;
    }
    leading = (u32)q2_count_leading_zeros_u32(squared) & ~1u;
    shift = (31 - leading) / 2;
    normalized = leading >= 24 ? squared << (leading - 24) : squared >> (24 - leading);
    seed = floor_root(((u64)1 << 30) / normalized);
    for (k = 0; k < 3; k++) output[k] = ((s32)v[k] * (s32)seed) >> shift;
}
