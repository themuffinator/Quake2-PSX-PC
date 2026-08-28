/*
 * lighting.c — the three-light selector and the GTE state it produces.
 *
 * Transcribed from 0x8006AFE8 (the gather), 0x8006AD4C (one candidate),
 * 0x80076040 (the attenuation) and 0x8008A5E8 (libgte's VectorNormal). Where
 * the original's arithmetic is observable it is reproduced rather than tidied:
 * the 16-bit wrapping delta, the truncating divides, the order the three axes
 * are rejected in, and the fact that the ranking never actually sorts — it
 * rotates a four-entry list and lets the loser be overwritten.
 */
#include "lighting.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* 0x80076040 — distance attenuation                                          */
/* ------------------------------------------------------------------------- */
s32 q2_light_attenuation(const q2_light *l, const s16 d[3], s32 scale)
{
    u32 outer, inner, num, den;
    u32 dist;

    if (!l || !d)
        return 0;

    /*
     * `mult` then `srl 6`. The multiply is allowed to overflow into the low
     * word and the shift is LOGICAL, which is load-bearing: the fallback
     * light's radiusSq of 0x3FFFFFFF times 64 wraps to 0xFFFFFFC0 and comes
     * back as 0x03FFFFFF. A port that widened this to 64 bits would give that
     * light a radius sixteen times too large.
     */
    outer = (l->radius_sq * (u32)scale) >> Q2_LIGHT_ATTEN_SHIFT;

    dist = (u32)((s32)d[0] * d[0] + (s32)d[1] * d[1] + (s32)d[2] * d[2]);
    if (dist >= outer)
        return 0;

    num   = outer - dist;
    inner = (l->inner_radius_sq * (u32)scale) >> Q2_LIGHT_ATTEN_SHIFT;
    den   = outer - inner;

    /* Make room for the << 12 without losing the top of the numerator. Both
     * sides shift together, so the ratio survives. */
    while (num & 0xFFF00000u) {
        num >>= 2;
        den >>= 2;
    }

    if (den == 0)
        return Q2_LIGHT_ONE;   /* inner == outer: a flat fill, not a falloff */

    return (s32)((num << 12) / den);
}

/* ------------------------------------------------------------------------- */
/* The 16-bit wrapping delta                                                  */
/* ------------------------------------------------------------------------- */
void q2_light_delta(const q2_light *l, const s32 origin[3], s16 out[3])
{
    const s32 lp[3] = { l->x, l->y, l->z };
    int i;

    for (i = 0; i < 3; i++)
        out[i] = (s16)(u16)((u16)(u32)lp[i] - (u16)(u32)origin[i]);
}

/* ------------------------------------------------------------------------- */
/* 0x8006AD4C — offer one light                                               */
/* ------------------------------------------------------------------------- */
static s32 abs_s32(s32 v) { return v < 0 ? -v : v; }

/* `mult` then `sra 8` with a +255 bias for negatives: a divide by 256 that
 * truncates toward zero rather than flooring. */
static s32 div256_trunc(s32 v)
{
    return (v < 0 ? v + 255 : v) >> 8;
}

bool q2_light_set_add(q2_light_set *set, const s32 origin[3], const q2_light *l)
{
    q2_light_slot *slot;
    s32 atten, total;
    int i;

    if (!set || !origin || !l)
        return false;

    slot = &set->slot[set->rank[3]];

    q2_light_delta(l, origin, slot->d);

    /*
     * Axis reject, and the order is the original's: x, then Z, then y
     * (0x8006ADB4, 0x8006ADCC, 0x8006ADEC). It only matters for which
     * comparison runs first, but transcribing it costs nothing and a later
     * reader comparing against the disassembly should not have to wonder.
     */
    if (abs_s32(slot->d[0]) >= (s32)l->radius) return false;
    if (abs_s32(slot->d[2]) >= (s32)l->radius) return false;
    if (abs_s32(slot->d[1]) >= (s32)l->radius) return false;

    atten = q2_light_attenuation(l, slot->d, Q2_LIGHT_ATTEN_SCALE);
    if (atten == 0)
        return false;
    if (atten > Q2_LIGHT_ONE)
        atten = Q2_LIGHT_ONE;

    {
        const u8 rgb[3] = { l->r, l->g, l->b };
        for (i = 0; i < 3; i++) {
            s32 c = div256_trunc((s32)rgb[i] * atten);
            if (c > Q2_LIGHT_ONE)
                c = Q2_LIGHT_ONE;
            slot->rgb[i] = (s16)c;
        }
    }

    total = slot->rgb[0] + slot->rgb[1] + slot->rgb[2];
    slot->total = (s16)total;

    /*
     * Insertion by rotation. There is no sort and no compaction: the candidate
     * already occupies the slot ranked last, so accepting it means moving its
     * rank up and pushing the others down. Rejecting it means doing nothing,
     * and the next candidate overwrites it.
     */
    if (set->slot[set->rank[0]].total < slot->total) {
        s16 cand = set->rank[3];
        set->rank[3] = set->rank[2];
        set->rank[2] = set->rank[1];
        set->rank[1] = set->rank[0];
        set->rank[0] = cand;
        set->count++;
        return true;
    }
    if (set->slot[set->rank[1]].total < slot->total) {
        s16 cand = set->rank[3];
        set->rank[3] = set->rank[2];
        set->rank[2] = set->rank[1];
        set->rank[1] = cand;
        set->count++;
        return true;
    }
    if (set->slot[set->rank[2]].total < slot->total) {
        s16 cand = set->rank[3];
        set->rank[3] = set->rank[2];
        set->rank[2] = cand;
        set->count++;
        return true;
    }

    return false;
}

/* ------------------------------------------------------------------------- */
/* 0x8006AFE8 — the gather                                                    */
/* ------------------------------------------------------------------------- */
void q2_light_set_begin(q2_light_set *set)
{
    int i;

    if (!set)
        return;

    memset(set, 0, sizeof(*set));
    for (i = 0; i < Q2_LIGHT_SLOTS; i++)
        set->rank[i] = (s16)i;
}

/* 0x8006B150 — the record the engine builds at 0x800A1C48 when an entity has no
 * collision node. Position from the entity, everything else fixed. */
static void fallback_light(q2_light *out, const s32 origin[3])
{
    memset(out, 0, sizeof(*out));

    out->x = origin[0] + Q2_LIGHT_FALLBACK_OFS_X;
    out->y = origin[1] + Q2_LIGHT_FALLBACK_OFS_Y;
    out->z = origin[2] + Q2_LIGHT_FALLBACK_OFS_Z;

    out->r = out->g = out->b = Q2_LIGHT_FALLBACK_GREY;
    out->always_255 = 0;      /* really 0 in this record, not 255 */
    out->type       = 0;      /* style 0: no flare                */
    out->radius     = 0x7FFF;
    out->inner_radius_sq = 0x3FFFFFFFu;
    out->radius_sq       = 0x3FFFFFFFu;
}

void q2_light_gather(q2_light_set *set, const q2_light_world *w,
                     const s32 origin[3], s32 coll_node, s16 entity_f4)
{
    u32 i;

    if (!set || !origin)
        return;

    q2_light_set_begin(set);

    if (!w)
        return;

    if (entity_f4 < 0) {
        /* 0x8006B040 is `bgez`: only a negative +0xF4 reaches the view list at
         * 0x8006B048, and then nothing else — no world or static lights. */
        for (i = 0; i < w->dynamic_view_count && i < Q2_DYNLIGHT_MAX; i++)
            q2_light_set_add(set, origin, &w->dynamic_view[i]);
        return;
    }

    /* 0x8006B08C: the world dynamic list first. */
    for (i = 0; i < w->dynamic_world_count && i < Q2_DYNLIGHT_MAX; i++)
        q2_light_set_add(set, origin, &w->dynamic_world[i]);

    if (coll_node < 0) {
        q2_light fb;
        fallback_light(&fb, origin);
        q2_light_set_add(set, origin, &fb);
        return;
    }

    /* 0x8006B0C8: the static lights of the node the entity stands in. */
    if (w->space && w->statics) {
        u32 first, count;

        if (q2_spacelights_range(w->space, (u32)coll_node, &first, &count)) {
            for (i = 0; i < count; i++) {
                u16 index;
                q2_light l;

                if (!q2_spacelights_entry(w->space, first + i, &index))
                    break;
                if (!q2_light_get(w->statics, index, &l))
                    continue;
                q2_light_set_add(set, origin, &l);
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */
/* FLKLIGHT                                                                   */
/* ------------------------------------------------------------------------- */
s32 q2_flklight_inner_radius(s32 rand_0_32767)
{
    /* 0x8002882C: r*500 >> 15, + 400 -> a2's low half. */
    return ((rand_0_32767 * 500) >> 15) + 400;
}

s32 q2_flklight_outer_radius(s32 rand_0_32767)
{
    /* 0x8002885C: the same chain, + 1000 -> a2's high half. */
    return ((rand_0_32767 * 500) >> 15) + 1000;
}

/* 0x80075C34 — append a runtime light                                        */
/* ------------------------------------------------------------------------- */
bool q2_light_add_dynamic(q2_light_world *w, const s32 pos[3],
                          const u8 rgb[3], s32 inner_radius, s32 outer_radius,
                          u32 style, u32 size_shift)
{
    q2_light *l;

    if (!w || !pos || !rgb)
        return false;

    /* The original's full test is a pointer compare against the base of the
     * VIEW array, which is what makes sixteen the limit. Past it the light is
     * dropped and the caller is told so; nothing is replaced. */
    if (w->dynamic_world_count >= Q2_DYNLIGHT_MAX)
        return false;

    l = &w->dynamic_world[w->dynamic_world_count++];
    memset(l, 0, sizeof(*l));

    l->x = pos[0];
    l->y = pos[1];
    l->z = pos[2];

    l->r = rgb[0];
    l->g = rgb[1];
    l->b = rgb[2];

    /*
     * The type byte, assembled the way the original assembles it: the style
     * into bits 3..5 and the size shift into 6..7, with bits 0..2 left alone.
     * On a zeroed array that means they stay clear, where every light on the
     * disc has them set — the clearest possible statement that nothing reads
     * them.
     */
    l->type = (u8)(((style & 7u) << 3) | ((size_shift & 3u) << 6));

    /* Radii in, squares out. */
    l->radius          = (u16)(s16)outer_radius;
    l->inner_radius_sq = (u32)((s16)inner_radius * (s16)inner_radius);
    l->radius_sq       = (u32)((s16)outer_radius * (s16)outer_radius);

    return true;
}

void q2_light_world_begin_frame(q2_light_world *w)
{
    if (!w)
        return;

    w->dynamic_world_count = 0;
    w->dynamic_view_count  = 0;
}

/* ------------------------------------------------------------------------- */
/* 0x8008A5E8 — VectorNormal                                                  */
/* ------------------------------------------------------------------------- */
static s16  g_rsqrt[Q2_LIGHT_RSQRT_ENTRIES];
static bool g_rsqrt_ready;

/*
 * The original's table at 0x800A9C54 is 192 entries indexed by the magnitude's
 * mantissa in [64,256) — a four-fold window rather than a two-fold one, because
 * the exponent is rounded down to even so that halving it is exact. Entry i is
 * floor(32768 / sqrt(i + 64)), so entry 0 is exactly 4096 and entry 191 is
 * 2052. Built here from the formula for the same reason trig.c builds its sine
 * table: the derivation is checkable and everything after it is pure integer
 * arithmetic.
 */
static void build_rsqrt(void)
{
    int i;

    if (g_rsqrt_ready)
        return;

    for (i = 0; i < Q2_LIGHT_RSQRT_ENTRIES; i++)
        g_rsqrt[i] = (s16)(32768.0 / sqrt((double)(i + 64)));

    g_rsqrt_ready = true;
}

s16 q2_light_rsqrt_table(u32 index)
{
    build_rsqrt();
    return index < Q2_LIGHT_RSQRT_ENTRIES ? g_rsqrt[index] : 0;
}

void q2_vector_normal(const s16 in[3], s16 out[3])
{
    s32 sum, lzc, shift, mant;
    s16 recip;
    int i;

    build_rsqrt();

    if (!in || !out)
        return;

    /* SQR with sf = 0 (opcode 0x0A00428, bit 19 clear): the squares are NOT
     * pre-divided, so this is the magnitude squared at full scale. */
    sum = (s32)in[0] * in[0] + (s32)in[1] * in[1] + (s32)in[2] * in[2];

    if (sum <= 0) {
        out[0] = out[1] = out[2] = 0;
        return;
    }

    /*
     * LZCR with the low bit cleared. Forcing the exponent even is what makes
     * halving it exact, and it is also why the mantissa window is [64,256)
     * rather than [64,128): the top bit can land in either of two places.
     */
    lzc = (s32)q2_count_leading_zeros_u32((u32)sum) & ~1;
    shift = (31 - lzc) >> 1;

    mant = (lzc < 24) ? (sum >> (24 - lzc)) : (sum << (lzc - 24));
    if (mant < 64)  mant = 64;
    if (mant > 255) mant = 255;

    recip = g_rsqrt[mant - 64];

    /* GPF with sf = 0 (opcode 0x190003D): MAC = IR0 * IR with no shift of its
     * own, so the only shift is the exponent going back out. */
    for (i = 0; i < 3; i++)
        out[i] = (s16)(((s32)recip * in[i]) >> shift);
}

/* ------------------------------------------------------------------------- */
/* 0x8006B184 — the gather becomes GTE state                                  */
/* ------------------------------------------------------------------------- */

/* `mult` then `sra 11` with the +2047 bias for negatives. */
static s32 scale_11(s32 v)
{
    return (v < 0 ? v + 2047 : v) >> 11;
}

void q2_light_env_build(q2_light_env *env, const q2_light_set *set,
                        s32 intensity_a, s32 intensity_b, const u8 glow[3])
{
    s32 intensity;
    u32 active;
    u32 j;

    if (!env)
        return;

    memset(env, 0, sizeof(*env));

    if (!set)
        return;

    /* 0x8006B18C: four or more accepted still means three used. */
    active = set->count;
    if (active > Q2_LIGHT_ACTIVE_MAX)
        active = Q2_LIGHT_ACTIVE_MAX;
    env->active = active;

    intensity = scale_11(intensity_a * intensity_b);

    /*
     * The colour matrix is filled by COLUMN: light j's red, green and blue go
     * to rows 0, 1 and 2 of column j (0x8006B1E0 onward writes +0, +6, +12 for
     * the first, +2, +8, +14 for the second, +4, +10, +16 for the third).
     * Columns past `active` stay zero, which is why the count==1 and count==2
     * paths explicitly clear them.
     */
    for (j = 0; j < active; j++) {
        const q2_light_slot *s = &set->slot[set->rank[j]];
        int c;

        for (c = 0; c < 3; c++)
            env->colour.m[c][j] = s->rgb[c];

        q2_vector_normal(s->d, env->dir[j]);

        for (c = 0; c < 3; c++)
            env->dir[j][c] = (s16)(((s32)env->dir[j][c] * intensity) >> Q2_FRAC_12);
    }

    /*
     * The back colour is the entity's own glow, scaled by the SAME product but
     * shifted 24 instead of 11 and with no rounding bias (0x8006B468). With
     * both intensities at 4096 that is the identity, which is what makes 4096
     * the neutral value for each.
     */
    if (glow) {
        s32 product = intensity_a * intensity_b;
        int c;
        for (c = 0; c < 3; c++)
            env->back[c] = ((s32)glow[c] * product) >> 24;
    }
}

void q2_light_env_apply(const q2_light_env *env, gte_state *g)
{
    gte_matrix light;
    u32 j;
    int c;

    if (!env || !g)
        return;

    memset(&light, 0, sizeof(light));
    for (j = 0; j < Q2_LIGHT_ACTIVE_MAX; j++)
        for (c = 0; c < 3; c++)
            light.m[j][c] = (j < env->active) ? env->dir[j][c] : 0;

    gte_set_light_matrix(g, &light);
    gte_set_colour_matrix(g, &env->colour);
    gte_set_back_colour(g, env->back[0], env->back[1], env->back[2]);
}

/* ------------------------------------------------------------------------- */
/* 0x80075E14 — the ambient fade                                              */
/* ------------------------------------------------------------------------- */
void q2_light_glow_fade(u8 current[3], const u8 target[3], s32 steps)
{
    int i;

    if (!current || !target)
        return;

    if (steps == 0)
        steps = 1;

    for (i = 0; i < 3; i++) {
        s32 cur  = current[i];
        s32 diff = (s32)target[i] - cur;

        /*
         * `slt` against the step count first: a gap smaller than one step is
         * closed outright rather than truncating to no movement at all.
         *
         * The comparison is signed and the difference can be negative, so a
         * glow that is FALLING arrives immediately while one that is rising
         * eases. That asymmetry is the original's, not an oversight here.
         */
        if (diff < steps)
            current[i] = (u8)(cur + diff);
        else
            current[i] = (u8)(cur + diff / steps);
    }
}
