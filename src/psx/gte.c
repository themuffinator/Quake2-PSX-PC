#include "gte.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Reciprocal table for the perspective divide.                               */
/*                                                                            */
/* The GTE does not divide. It normalises the divisor, looks up a seed in a    */
/* 257-entry table, runs two Newton-Raphson refinement steps, and multiplies.  */
/* The table is small enough that the result is measurably approximate, and    */
/* that approximation is part of why PlayStation geometry shimmers. We build   */
/* the table from its defining formula at init rather than hard-coding it.     */
/* ------------------------------------------------------------------------- */
static u8   g_unr_table[257];
static bool g_unr_ready;

static void gte_build_unr_table(void)
{
    int i;

    if (g_unr_ready)
        return;

    for (i = 0; i <= 0x100; i++) {
        s32 v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        g_unr_table[i] = (u8)(v < 0 ? 0 : v);
    }
    g_unr_ready = true;
}

/* ------------------------------------------------------------------------- */
/* Flag handling                                                              */
/* ------------------------------------------------------------------------- */
static void gte_flag_reset(gte_state *g)
{
    g->flag = 0;
}

static void gte_flag_finish(gte_state *g)
{
    if (g->flag & GTE_FLAG_ERROR_MASK)
        g->flag |= GTE_FLAG_ERROR;
}

/* MAC1..MAC3 are 44-bit accumulators; writing a wider value sets the
 * positive/negative overflow flags but the stored value simply truncates. */
static s32 gte_set_mac(gte_state *g, int idx, s64 value)
{
    static const u32 pos_flag[3] = { GTE_FLAG_MAC1_POS, GTE_FLAG_MAC2_POS, GTE_FLAG_MAC3_POS };
    static const u32 neg_flag[3] = { GTE_FLAG_MAC1_NEG, GTE_FLAG_MAC2_NEG, GTE_FLAG_MAC3_NEG };

    if (value >  (s64)0x7FFFFFFFFFLL) g->flag |= pos_flag[idx];
    if (value < -(s64)0x8000000000LL) g->flag |= neg_flag[idx];

    g->mac[idx] = (s32)value;
    return g->mac[idx];
}

/* IR1..IR3 saturate to s16, or to [0,32767] when the limit-negative bit is set. */
static void gte_set_ir(gte_state *g, int idx, s32 value, bool lm)
{
    static const u32 sat_flag[3] = { GTE_FLAG_IR1_SAT, GTE_FLAG_IR2_SAT, GTE_FLAG_IR3_SAT };
    const s32 lo = lm ? 0 : -32768;

    if (value < lo)      { value = lo;    g->flag |= sat_flag[idx]; }
    else if (value > 32767) { value = 32767; g->flag |= sat_flag[idx]; }

    g->ir[idx] = (s16)value;
}

/* The colour FIFO saturates each component to a byte and flags it. Unlike IR,
 * there is no limit-negative bit: the range is always 0..255. */
static u8 gte_colour_sat(gte_state *g, int idx, s32 value)
{
    static const u32 sat_flag[3] = {
        GTE_FLAG_COLOR_R_SAT, GTE_FLAG_COLOR_G_SAT, GTE_FLAG_COLOR_B_SAT
    };

    if (value < 0)        { value = 0;   g->flag |= sat_flag[idx]; }
    else if (value > 255) { value = 255; g->flag |= sat_flag[idx]; }

    return (u8)value;
}

/* RGB0..RGB2, three deep, newest in RGB2 — the same shape as the SXY FIFO.
 * The `c` byte is carried through from RGBC unchanged; it is the GPU primitive
 * code the caller loaded, not a colour. */
static void gte_push_rgb(gte_state *g)
{
    g->rgb_fifo[0] = g->rgb_fifo[1];
    g->rgb_fifo[1] = g->rgb_fifo[2];

    g->rgb_fifo[2].r = gte_colour_sat(g, 0, g->mac[0] >> 4);
    g->rgb_fifo[2].g = gte_colour_sat(g, 1, g->mac[1] >> 4);
    g->rgb_fifo[2].b = gte_colour_sat(g, 2, g->mac[2] >> 4);
    g->rgb_fifo[2].c = g->rgbc.c;
}

static void gte_push_sz(gte_state *g, s32 value)
{
    g->sz[0] = g->sz[1];
    g->sz[1] = g->sz[2];
    g->sz[2] = g->sz[3];

    if (value < 0)          { value = 0;     g->flag |= GTE_FLAG_SZ3_SAT; }
    else if (value > 65535) { value = 65535; g->flag |= GTE_FLAG_SZ3_SAT; }

    g->sz[3] = (u16)value;
}

static void gte_push_sxy(gte_state *g, s32 x, s32 y)
{
    g->sxy[0] = g->sxy[1];
    g->sxy[1] = g->sxy[2];

    if (x < -1024)     { x = -1024; g->flag |= GTE_FLAG_SX2_SAT; }
    else if (x > 1023) { x = 1023;  g->flag |= GTE_FLAG_SX2_SAT; }

    if (y < -1024)     { y = -1024; g->flag |= GTE_FLAG_SY2_SAT; }
    else if (y > 1023) { y = 1023;  g->flag |= GTE_FLAG_SY2_SAT; }

    /* Note: no fractional part is retained. This truncation *is* the wobble. */
    g->sxy[2].x = (s16)x;
    g->sxy[2].y = (s16)y;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */
void gte_init(gte_state *g)
{
    gte_build_unr_table();
    memset(g, 0, sizeof(*g));

    /* Identity rotation in 1.3.12. */
    g->rot.m[0][0] = Q2_ONE_12;
    g->rot.m[1][1] = Q2_ONE_12;
    g->rot.m[2][2] = Q2_ONE_12;

    g->h = 320;

    /* InitGeom's average-Z scale factors, which nothing overrides. */
    g->zsf3 = GTE_ZSF3_INIT;
    g->zsf4 = GTE_ZSF4_INIT;
}

void gte_set_projection(gte_state *g, u16 h, s32 centre_x, s32 centre_y)
{
    g->h   = h;
    g->ofx = centre_x << Q2_FRAC_16;
    g->ofy = centre_y << Q2_FRAC_16;
}

void gte_set_rotation(gte_state *g, const gte_matrix *m)
{
    g->rot = *m;
}

void gte_set_translation(gte_state *g, s32 x, s32 y, s32 z)
{
    g->tr.x = x;
    g->tr.y = y;
    g->tr.z = z;
}

void gte_set_light_matrix(gte_state *g, const gte_matrix *m)
{
    g->light = *m;
}

void gte_set_colour_matrix(gte_state *g, const gte_matrix *m)
{
    g->colour = *m;
}

void gte_set_back_colour(gte_state *g, s32 r, s32 gr, s32 b)
{
    /* libgte's SetBackColor, 0x8008AD0C: three `sll 4` and three `ctc2`. */
    g->bk.x = r  << 4;
    g->bk.y = gr << 4;
    g->bk.z = b  << 4;
}

/* ------------------------------------------------------------------------- */
/* Perspective divide                                                         */
/* ------------------------------------------------------------------------- */
u32 gte_divide(gte_state *g, u16 h, u16 sz3)
{
    u32 n, d, u;
    s32 z;

    gte_build_unr_table();

    /* Anything at or beyond twice the projection distance overflows. The
     * hardware clamps rather than trapping, and games relied on the clamp. */
    if (sz3 == 0 || (u32)h >= (u32)sz3 * 2) {
        g->flag |= GTE_FLAG_DIV_OVERFLOW;
        return 0x1FFFF;
    }

    z = q2_count_leading_zeros_u32((u32)sz3) - 16;   /* normalise to bit 15 */
    n = (u32)h   << z;
    d = (u32)sz3 << z;

    u = (u32)g_unr_table[(d - 0x7FC0u) >> 7] + 0x101u;

    d = (0x2000080u - (d * u)) >> 8;
    d = (0x0000080u + (d * u)) >> 8;

    n = (u32)(((u64)n * (u64)d + 0x8000u) >> 16);

    if (n > 0x1FFFF) {
        n = 0x1FFFF;
        g->flag |= GTE_FLAG_DIV_OVERFLOW;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* RTPS / RTPT                                                                */
/* ------------------------------------------------------------------------- */
static void gte_rtp_vertex(gte_state *g, const gte_vec16 *v, bool last)
{
    s64 mx, my, mz;
    s32 sx, sy;
    u32 quot;

    /* Rotate and translate. Translation is 1.19.12, so it is shifted up to meet
     * the 12-bit fractional product of the matrix multiply. */
    mx = ((s64)g->tr.x << Q2_FRAC_12)
       + (s64)g->rot.m[0][0] * v->x + (s64)g->rot.m[0][1] * v->y + (s64)g->rot.m[0][2] * v->z;
    my = ((s64)g->tr.y << Q2_FRAC_12)
       + (s64)g->rot.m[1][0] * v->x + (s64)g->rot.m[1][1] * v->y + (s64)g->rot.m[1][2] * v->z;
    mz = ((s64)g->tr.z << Q2_FRAC_12)
       + (s64)g->rot.m[2][0] * v->x + (s64)g->rot.m[2][1] * v->y + (s64)g->rot.m[2][2] * v->z;

    gte_set_mac(g, 0, mx >> Q2_FRAC_12);
    gte_set_mac(g, 1, my >> Q2_FRAC_12);
    gte_set_mac(g, 2, mz >> Q2_FRAC_12);

    gte_set_ir(g, 0, g->mac[0], false);
    gte_set_ir(g, 1, g->mac[1], false);

    /* IR3's saturation flag is computed from the *unshifted* MAC3 — a hardware
     * quirk that matters because games watch FLAG to reject bad geometry. */
    {
        s32 mac3_unshifted = (s32)(mz >> Q2_FRAC_12);
        s32 check = (s32)(mz >> Q2_FRAC_12);
        (void)mac3_unshifted;
        if (check < -32768 || check > 32767)
            g->flag |= GTE_FLAG_IR3_SAT;
        g->ir[2] = (s16)q2_clamp_s32(check, 0, 32767);
    }

    gte_push_sz(g, (s32)(mz >> Q2_FRAC_12));

    quot = gte_divide(g, g->h, g->sz[3]);

    /* Screen position: quotient * IR / 65536, plus the screen-centre offset.
     * MAC0 is the full-precision intermediate; SXY keeps only whole pixels. */
    {
        s64 fx = (s64)quot * (s64)g->ir[0] + (s64)g->ofx;
        s64 fy = (s64)quot * (s64)g->ir[1] + (s64)g->ofy;

        if (fx >  (s64)0x7FFFFFFF) g->flag |= GTE_FLAG_MAC0_POS;
        if (fx < -(s64)0x80000000LL) g->flag |= GTE_FLAG_MAC0_NEG;
        if (fy >  (s64)0x7FFFFFFF) g->flag |= GTE_FLAG_MAC0_POS;
        if (fy < -(s64)0x80000000LL) g->flag |= GTE_FLAG_MAC0_NEG;

        sx = (s32)(fx >> Q2_FRAC_16);
        sy = (s32)(fy >> Q2_FRAC_16);
        gte_push_sxy(g, sx, sy);
    }

    /* Depth cueing runs only on the final vertex of an RTPT. */
    if (last) {
        s64 dc = (s64)quot * (s64)g->dqa + (s64)g->dqb;
        g->mac0 = (s32)dc;

        {
            s32 ir0 = (s32)(dc >> Q2_FRAC_12);
            if (ir0 < 0)          { ir0 = 0;    g->flag |= GTE_FLAG_IR0_SAT; }
            else if (ir0 > 4096)  { ir0 = 4096; g->flag |= GTE_FLAG_IR0_SAT; }
            g->ir0 = (s16)ir0;
        }
    }
}

void gte_rtps(gte_state *g, bool set_mac0_from_depth_cue)
{
    gte_flag_reset(g);
    gte_rtp_vertex(g, &g->v[0], set_mac0_from_depth_cue);
    gte_flag_finish(g);
}

void gte_rtpt(gte_state *g)
{
    gte_flag_reset(g);
    gte_rtp_vertex(g, &g->v[0], false);
    gte_rtp_vertex(g, &g->v[1], false);
    gte_rtp_vertex(g, &g->v[2], true);
    gte_flag_finish(g);
}

/* ------------------------------------------------------------------------- */
/* NCLIP — signed area, used for backface rejection                           */
/* ------------------------------------------------------------------------- */
void gte_nclip(gte_state *g)
{
    s64 area;

    gte_flag_reset(g);

    area = (s64)g->sxy[0].x * (s64)g->sxy[1].y
         + (s64)g->sxy[1].x * (s64)g->sxy[2].y
         + (s64)g->sxy[2].x * (s64)g->sxy[0].y
         - (s64)g->sxy[0].x * (s64)g->sxy[2].y
         - (s64)g->sxy[1].x * (s64)g->sxy[0].y
         - (s64)g->sxy[2].x * (s64)g->sxy[1].y;

    if (area >  (s64)0x7FFFFFFF) g->flag |= GTE_FLAG_MAC0_POS;
    if (area < -(s64)0x80000000LL) g->flag |= GTE_FLAG_MAC0_NEG;

    g->mac0 = (s32)area;
    gte_flag_finish(g);
}

/* ------------------------------------------------------------------------- */
/* AVSZ3 / AVSZ4 — ordering-table bucket selection                            */
/* ------------------------------------------------------------------------- */
static void gte_avsz(gte_state *g, s16 scale, int count)
{
    s64 sum = 0;
    int i;

    gte_flag_reset(g);

    for (i = 4 - count; i < 4; i++)
        sum += g->sz[i];

    sum *= (s64)scale;

    if (sum >  (s64)0x7FFFFFFF) g->flag |= GTE_FLAG_MAC0_POS;
    if (sum < -(s64)0x80000000LL) g->flag |= GTE_FLAG_MAC0_NEG;
    g->mac0 = (s32)sum;

    {
        s64 otz = sum >> Q2_FRAC_12;
        if (otz < 0)          { otz = 0;     g->flag |= GTE_FLAG_SZ3_SAT; }
        else if (otz > 65535) { otz = 65535; g->flag |= GTE_FLAG_SZ3_SAT; }
        g->otz = (u16)otz;
    }

    gte_flag_finish(g);
}

void gte_avsz3(gte_state *g) { gte_avsz(g, g->zsf3, 3); }
void gte_avsz4(gte_state *g) { gte_avsz(g, g->zsf4, 4); }

/* ------------------------------------------------------------------------- */
/* MVMVA                                                                      */
/* ------------------------------------------------------------------------- */
void gte_mvmva(gte_state *g, int sf, int mx, int vx, int tx, int lm)
{
    const gte_matrix *m;
    s32 vec[3];
    const s32 *t;
    static const s32 zero3[3] = { 0, 0, 0 };
    const int shift = sf ? Q2_FRAC_12 : 0;
    int i;

    gte_flag_reset(g);

    switch (mx) {
    case 0:  m = &g->rot;    break;
    case 1:  m = &g->light;  break;
    case 2:  m = &g->colour; break;
    default: m = &g->rot;    break;   /* mx==3 selects a garbage matrix on HW */
    }

    if (vx < 3) {
        vec[0] = g->v[vx].x;
        vec[1] = g->v[vx].y;
        vec[2] = g->v[vx].z;
    } else {
        vec[0] = g->ir[0];
        vec[1] = g->ir[1];
        vec[2] = g->ir[2];
    }

    switch (tx) {
    case 0:  t = (const s32 *)&g->tr; break;
    case 1:  t = (const s32 *)&g->bk; break;
    case 2:  t = (const s32 *)&g->fc; break;
    default: t = zero3;               break;
    }

    for (i = 0; i < 3; i++) {
        s64 acc = ((s64)t[i] << Q2_FRAC_12)
                + (s64)m->m[i][0] * vec[0]
                + (s64)m->m[i][1] * vec[1]
                + (s64)m->m[i][2] * vec[2];
        gte_set_mac(g, i, acc >> shift);
        gte_set_ir(g, i, g->mac[i], lm != 0);
    }

    gte_flag_finish(g);
}

/* ------------------------------------------------------------------------- */
/* The lighting operations — NCS/NCT, NCCS/NCCT, NCDS/NCDT, CC, CDP           */
/*                                                                            */
/* All eight share one pipeline and differ only in which stages run. Written   */
/* once, selected by the two flags below, because writing it eight times is    */
/* how the saturation behaviour ends up subtly different in one of them.       */
/* ------------------------------------------------------------------------- */
enum { GTE_NC_PLAIN = 0, GTE_NC_COLOUR = 1, GTE_NC_DEPTH_CUE = 2 };

/* Stage 1: the light matrix turns a normal into three N-dot-L terms.
 * `lm` is set, so a surface facing away from a light contributes exactly zero
 * rather than a negative amount — the hardware's own "unlit" rule. */
static void gte_nc_light(gte_state *g, const gte_vec16 *v)
{
    int i;

    for (i = 0; i < 3; i++) {
        s64 acc = (s64)g->light.m[i][0] * v->x
                + (s64)g->light.m[i][1] * v->y
                + (s64)g->light.m[i][2] * v->z;
        gte_set_mac(g, i, acc >> Q2_FRAC_12);
        gte_set_ir(g, i, g->mac[i], true);
    }
}

/* Stage 2: the colour matrix turns those three terms into RGB around the back
 * colour. BK is 1.19.12, hence the shift up to meet the 12-bit products.
 *
 * THE INPUT VECTOR IS SNAPSHOTTED, and it has to be. `gte_set_ir` writes back
 * into `g->ir`, so reading `g->ir[]` inside the loop meant iteration 0 replaced
 * light 0's N-dot-L with the RED RESULT before iterations 1 and 2 had read it.
 * Green was then `m[1][0] * red` instead of `m[1][0] * (N.L)`, i.e. scaled by a
 * further colour/4096.
 *
 * It is a matrix multiply: all three rows read the same input. The hardware
 * latches it; `gte_nc_depth_cue` below already snapshots for the same reason.
 *
 * What it looked like: with ONE light reaching a model — the common case, and
 * true of BASE1's and BASE2's nearest Soldier — columns 1 and 2 of the colour
 * matrix are zero, so the corrupted ir[0] is the only term green and blue have
 * and the model comes out PURE RED. Measured against BASE2's actual light
 * (slot colour 2250/1895/1235) at N.L = 0.5, this returned (140, 65, 42) where
 * the answer is (140, 118, 77) — and 65/140 = 1895/4096, 42/140 = 1235/4096,
 * the extra divide exactly. After the rasteriser's modulate and RGB555's
 * truncation the body pixels were literally (16,0,0), (24,0,0), (32,0,0).
 *
 * With two or more lights the uncorrupted ir[1] term survives and the model
 * looks nearly right, which is why this read as intermittent rather than as a
 * systematic fault.
 *
 * `gte_ncs` has exactly one caller in the port — modeldraw.c's per-model light
 * pass — so this was a model-rendering bug and touched nothing else.
 */
static void gte_nc_colour(gte_state *g)
{
    const s32 bk[3] = { g->bk.x, g->bk.y, g->bk.z };
    const s32 ir[3] = { g->ir[0], g->ir[1], g->ir[2] };
    int i;

    for (i = 0; i < 3; i++) {
        s64 acc = ((s64)bk[i] << Q2_FRAC_12)
                + (s64)g->colour.m[i][0] * ir[0]
                + (s64)g->colour.m[i][1] * ir[1]
                + (s64)g->colour.m[i][2] * ir[2];
        gte_set_mac(g, i, acc >> Q2_FRAC_12);
        gte_set_ir(g, i, g->mac[i], true);
    }
}

/* Stage 3, NCC and NCD only: modulate by the primitive's own colour. The <<4
 * is the hardware's, and it is what makes RGBC 128 mean "unchanged". */
static void gte_nc_modulate(gte_state *g)
{
    const s32 rgb[3] = { g->rgbc.r, g->rgbc.g, g->rgbc.b };
    int i;

    for (i = 0; i < 3; i++) {
        s64 acc = ((s64)rgb[i] * (s64)g->ir[i]) << 4;
        gte_set_mac(g, i, acc >> Q2_FRAC_12);
        gte_set_ir(g, i, g->mac[i], true);
    }
}

/*
 * Stage 4, NCD and CDP only: MAC += (FC - MAC) * IR0, the depth cue.
 *
 * The difference passes through IR *without* the limit-negative bit, which is
 * what lets the far colour pull a vertex down as well as up. IR0 is the 0.8.8
 * factor RTPS left behind on its last vertex.
 */
static void gte_nc_depth_cue(gte_state *g)
{
    const s32 fc[3] = { g->fc.x, g->fc.y, g->fc.z };
    s32 base[3];
    int i;

    for (i = 0; i < 3; i++)
        base[i] = g->mac[i];

    for (i = 0; i < 3; i++) {
        gte_set_mac(g, i, (s64)fc[i] - (s64)base[i]);
        gte_set_ir(g, i, g->mac[i], false);
    }

    for (i = 0; i < 3; i++) {
        gte_set_mac(g, i, (((s64)g->ir[i] * g->ir0) >> Q2_FRAC_12) + base[i]);
        gte_set_ir(g, i, g->mac[i], true);
    }
}

static void gte_nc_vertex(gte_state *g, const gte_vec16 *v, int mode)
{
    gte_nc_light(g, v);
    gte_nc_colour(g);

    if (mode != GTE_NC_PLAIN)
        gte_nc_modulate(g);
    if (mode == GTE_NC_DEPTH_CUE)
        gte_nc_depth_cue(g);

    gte_push_rgb(g);
}

static void gte_nc_run(gte_state *g, int mode, int count)
{
    int i;

    gte_flag_reset(g);
    for (i = 0; i < count; i++)
        gte_nc_vertex(g, &g->v[i], mode);
    gte_flag_finish(g);
}

void gte_ncs(gte_state *g)  { gte_nc_run(g, GTE_NC_PLAIN, 1); }
void gte_nct(gte_state *g)  { gte_nc_run(g, GTE_NC_PLAIN, 3); }
void gte_nccs(gte_state *g) { gte_nc_run(g, GTE_NC_COLOUR, 1); }
void gte_ncct(gte_state *g) { gte_nc_run(g, GTE_NC_COLOUR, 3); }
void gte_ncds(gte_state *g) { gte_nc_run(g, GTE_NC_DEPTH_CUE, 1); }
void gte_ncdt(gte_state *g) { gte_nc_run(g, GTE_NC_DEPTH_CUE, 3); }

void gte_cc(gte_state *g)
{
    gte_flag_reset(g);
    gte_nc_colour(g);
    gte_nc_modulate(g);
    gte_push_rgb(g);
    gte_flag_finish(g);
}

void gte_cdp(gte_state *g)
{
    gte_flag_reset(g);
    gte_nc_colour(g);
    gte_nc_modulate(g);
    gte_nc_depth_cue(g);
    gte_push_rgb(g);
    gte_flag_finish(g);
}

/* ------------------------------------------------------------------------- */
/* SQR                                                                        */
/* ------------------------------------------------------------------------- */
void gte_sqr(gte_state *g, int sf)
{
    const int shift = sf ? Q2_FRAC_12 : 0;
    int i;

    gte_flag_reset(g);

    for (i = 0; i < 3; i++) {
        s64 v = (s64)g->ir[i] * (s64)g->ir[i];
        gte_set_mac(g, i, v >> shift);
        gte_set_ir(g, i, g->mac[i], true);
    }

    gte_flag_finish(g);
}

/* ------------------------------------------------------------------------- */
/* Convenience wrapper                                                        */
/* ------------------------------------------------------------------------- */
bool gte_project_point(gte_state *g, s32 wx, s32 wy, s32 wz,
                       gte_sxy *out_screen, u16 *out_z)
{
    g->v[0].x = (s16)wx;
    g->v[0].y = (s16)wy;
    g->v[0].z = (s16)wz;

    gte_rtps(g, false);

    if (out_screen) *out_screen = g->sxy[2];
    if (out_z)      *out_z      = g->sz[3];

    return (g->flag & GTE_FLAG_DIV_OVERFLOW) == 0;
}
