#include "trig.h"

#include <math.h>

static s16  g_sin_table[Q2_ANGLE_360];
static s16  g_atan_table[1025];
static bool g_table_ready;

static void build_table(void)
{
    int i;

    if (g_table_ready)
        return;

    for (i = 0; i < Q2_ANGLE_360; i++) {
        double radians = (double)i * (2.0 * 3.14159265358979323846) / (double)Q2_ANGLE_360;
        double value   = sin(radians) * (double)Q2_ONE_12;

        /* Round half away from zero, then clamp: sin(90 degrees) must land on
         * exactly +4096 rather than 4095, or an identity rotation would shrink
         * geometry very slightly on every frame it is applied. */
        value = value < 0.0 ? value - 0.5 : value + 0.5;

        if (value >  32767.0) value =  32767.0;
        if (value < -32768.0) value = -32768.0;

        g_sin_table[i] = (s16)value;
    }

    /* A 3/8-bin offset reproduces all 1025 PAL halfwords at 0x800A9430.
     * This is a numerical reconstruction, verified against the user's disc,
     * rather than a copy of the executable's lookup data. */
    for (i = 0; i <= 1024; i++)
        g_atan_table[i] = (s16)(atan(((double)i + 0.375) / 1024.0) *
                                2048.0 / 3.14159265358979323846 + 0.5);
    g_table_ready = true;
}

static s32 wrap_angle(s32 angle)
{
    angle %= Q2_ANGLE_360;
    if (angle < 0)
        angle += Q2_ANGLE_360;
    return angle;
}

s32 q2_sin12(s32 angle)
{
    build_table();
    return g_sin_table[wrap_angle(angle)];
}

s32 q2_cos12(s32 angle)
{
    build_table();
    return g_sin_table[wrap_angle(angle + Q2_ANGLE_90)];
}

s32 q2_acos12(s32 cos12)
{
    double radians, angle;

    if (cos12 >= Q2_ONE_12) cos12 = Q2_ONE_12 - 2;
    if (cos12 < -Q2_ONE_12) cos12 = -Q2_ONE_12;

    /* 0x80069E64: the table has one entry per TWO cosine units. Its
     * negative endpoint and zero entry truncate one unit below libm's exact
     * multiples of pi. These two boundary values plus this quantization
     * reproduce all 4096 PAL entries without distributing the table. */
    cos12 = (cos12 >> 1) * 2;
    if (cos12 == -Q2_ONE_12) return 2047;
    if (!cos12) return 1023;

    radians = acos((double)cos12 / (double)Q2_ONE_12);
    angle   = radians * (double)Q2_ANGLE_360 /
              (2.0 * 3.14159265358979323846);

    /* Truncate rather than round: the original's table does, visibly so at the
     * ends — its entry for cos = 4094/4096 is 20 where rounding gives 21. */
    return (s32)angle;
}

void q2_quat_slerp(const s16 a[4], const s16 b[4], s32 t, s16 out[4])
{
    s32 bq[4], dot, wa = 4096 - t, wb = t;
    u32 sum = 0;
    bool same = true;
    int i;
    for (i = 0; i < 4; i++) {
        sum += (u32)((s32)a[i] * b[i]);
        same = same && a[i] == b[i];
    }
    dot = (s32)sum;
    for (i = 0; i < 4; i++) bq[i] = dot < 0 ? -(s32)b[i] : b[i];
    if (dot < 0) dot = (s32)(0u - (u32)dot);

    /* 0x80069D34..0x80069E04 copies the ORIGINAL b at the endpoint,
     * even when the interior takes the short arc through -b. Equal inputs
     * also copy directly, avoiding two independently rounded products. */
    if (!t || t == 4096 || same) {
        for (i = 0; i < 4; i++) out[i] = t ? b[i] : a[i];
        return;
    }
    if (4096 - (dot >> 12) > 128) {
        s32 ang = q2_acos12(dot >> 12), sn = q2_sin12(ang);
        if (sn) {
            wa = q2_sin12((s32)((u32)wa * (u32)ang) >> 12) * 4096 / sn;
            wb = q2_sin12((s32)((u32)wb * (u32)ang) >> 12) * 4096 / sn;
        }
    }
    for (i = 0; i < 4; i++)
        out[i] = (s16)(((s32)((u32)wa * (u32)(s32)a[i]) >> 12) +
                       ((s32)((u32)wb * (u32)bq[i]) >> 12));
}

s32 q2_ratan2(s32 y, s32 x)
{
    u32 ax = x < 0 ? 0u - (u32)x : (u32)x;
    u32 ay = y < 0 ? 0u - (u32)y : (u32)y;
    u32 lo = ax < ay ? ax : ay, hi = ax < ay ? ay : ax, index;
    s32 angle;
    if (x == 0 && y == 0)
        return 0;
    build_table();
    /* 0x8008A398/420 avoids overflowing the numerator by shifting the
     * denominator instead. Keep the resulting ratio truncation in both paths.
     * Unsigned magnitudes also define INT_MIN, outside native valid input. */
    index = lo >= 0x200000u ? lo / (hi >> 10) : (lo << 10) / hi;
    if (index > 1024) index = 1024;
    angle = g_atan_table[index];
    if (ay >= ax) angle = 1024 - angle;
    if (x < 0) angle = 2048 - angle;
    if (y < 0) angle = -angle;
    return angle;
}

void q2_rotation_yaw_pitch(s16 m[3][3], s32 yaw, s32 pitch)
{
    s32 sy = q2_sin12(yaw),   cy = q2_cos12(yaw);
    s32 sp = q2_sin12(pitch), cp = q2_cos12(pitch);

    /* R = Rx(pitch) * Ry(yaw). Products of two 1.3.12 values are shifted back
     * down by 12 to stay in 1.3.12. */
    m[0][0] = (s16)cy;
    m[0][1] = 0;
    m[0][2] = (s16)(-sy);

    m[1][0] = (s16)(((s64)sp * sy) >> Q2_FRAC_12);
    m[1][1] = (s16)cp;
    m[1][2] = (s16)(((s64)sp * cy) >> Q2_FRAC_12);

    m[2][0] = (s16)(((s64)cp * sy) >> Q2_FRAC_12);
    m[2][1] = (s16)(-sp);
    m[2][2] = (s16)(((s64)cp * cy) >> Q2_FRAC_12);
}

void q2_rotation_view(s16 m[3][3], s32 yaw, s32 pitch, s32 roll)
{
    s16 base[3][3];
    s32 sr, cr;
    int c;

    q2_rotation_yaw_pitch(base, yaw, pitch);

    if (roll == 0) {
        int r;
        for (r = 0; r < 3; r++)
            for (c = 0; c < 3; c++)
                m[r][c] = base[r][c];
        return;
    }

    sr = q2_sin12(roll);
    cr = q2_cos12(roll);

    /*
     * Rz(roll) * base. Only the first two rows change: a rotation about the view
     * axis mixes screen X and screen Y and leaves depth alone, which is exactly
     * why the roll can be applied here instead of inside the basis and still be
     * the console's.
     */
    for (c = 0; c < 3; c++) {
        s32 x = base[0][c];
        s32 y = base[1][c];

        m[0][c] = (s16)((( (s64)cr * x) - ((s64)sr * y)) >> Q2_FRAC_12);
        m[1][c] = (s16)((( (s64)sr * x) + ((s64)cr * y)) >> Q2_FRAC_12);
        m[2][c] = base[2][c];
    }
}

void q2_rotation_euler(s16 m[3][3], s32 rx, s32 ry, s32 rz)
{
    s32 sx = q2_sin12(rx), cx = q2_cos12(rx);
    s32 sy = q2_sin12(ry), cy = q2_cos12(ry);
    s32 sz = q2_sin12(rz), cz = q2_cos12(rz);

    /*
     * R = Ry * Rx * Rz, WHICH IS NOT WHAT THIS USED TO BUILD.
     *
     * `RotMatrix` at 0x80089E38 was transcribed element by element and it
     * composes Ry(y) * Rx(x) * Rz(z):
     *
     *     m00 =  cy*cz + sy*sx*sz     0x8008A014
     *     m01 = -cy*sz + sy*sx*cz     0x8008A044
     *     m02 =  sy*cx                0x80089F20
     *     m10 =  sz*cx                0x80089FAC
     *     m11 =  cz*cx                0x80089FCC
     *     m12 = -sx                   0x80089F0C
     *     m20 = -sy*cz + cy*sx*sz     0x8008A0B8
     *     m21 =  sy*sz + cy*sx*cz     0x8008A08C
     *     m22 =  cy*cx                0x80089F34
     *
     * This built Rz * Ry * Rx, and the note above it said the order was
     * "unobservable on this disc" because the rotation integrator writes one
     * axis and leaves the other two at zero. That was TRUE FOR ROTATING
     * BRUSHES and false the moment the VIEW WEAPON started calling it: a clip's
     * rotation has all three angles large — the blaster's idle is
     * (2078, 2110, 1985), a half circle either side — and the two orders agree
     * only when the roll is zero. Nobody re-checked when the caller changed.
     *
     * The intermediate `>> 12` roundings are the original's own: it forms
     * `sy*sx` and `cy*sx` once each, shifts, and multiplies those by sz and cz
     * — a two-step rounding a single fused product would not reproduce.
     */
    s32 sysx = (sy * sx) >> Q2_FRAC_12;      /* 0x80089FD8 */
    s32 cysx = (cy * sx) >> Q2_FRAC_12;      /* 0x8008A050 */

    m[0][0] = (s16)(((cy * cz) >> Q2_FRAC_12) + ((sysx * sz) >> Q2_FRAC_12));
    m[0][1] = (s16)(((sysx * cz) >> Q2_FRAC_12) - ((cy * sz) >> Q2_FRAC_12));
    m[0][2] = (s16)((sy * cx) >> Q2_FRAC_12);

    m[1][0] = (s16)((sz * cx) >> Q2_FRAC_12);
    m[1][1] = (s16)((cz * cx) >> Q2_FRAC_12);
    m[1][2] = (s16)(-sx);

    m[2][0] = (s16)(((cysx * sz) >> Q2_FRAC_12) - ((sy * cz) >> Q2_FRAC_12));
    m[2][1] = (s16)(((sy * sz) >> Q2_FRAC_12) + ((cysx * cz) >> Q2_FRAC_12));
    m[2][2] = (s16)((cy * cx) >> Q2_FRAC_12);
}

void q2_quat_to_matrix(s16 m[3][3], const s16 q[4])
{
    s32 x=q[0], y=q[1], z=q[2], w=q[3];
    /* 0x80069B54 doubles each operand BEFORE multiplying and shifting.
     * Doubling an already shifted product loses the odd matrix coefficients. */
    s32 xx=(s32)((u32)x*(u32)(2*x))>>12, yy=(s32)((u32)y*(u32)(2*y))>>12;
    s32 zz=(s32)((u32)z*(u32)(2*z))>>12, xy=(s32)((u32)x*(u32)(2*y))>>12;
    s32 xz=(s32)((u32)x*(u32)(2*z))>>12, yz=(s32)((u32)y*(u32)(2*z))>>12;
    s32 wx=(s32)((u32)w*(u32)(2*x))>>12, wy=(s32)((u32)w*(u32)(2*y))>>12;
    s32 wz=(s32)((u32)w*(u32)(2*z))>>12;
    m[0][0]=(s16)(4096-yy-zz); m[0][1]=(s16)(xy-wz); m[0][2]=(s16)(xz+wy);
    m[1][0]=(s16)(xy+wz); m[1][1]=(s16)(4096-xx-zz); m[1][2]=(s16)(yz-wx);
    m[2][0]=(s16)(xz-wy); m[2][1]=(s16)(yz+wx); m[2][2]=(s16)(4096-xx-yy);
}

void q2_rotation_view_anamorphic(s16 m[3][3], s32 yaw, s32 pitch, s32 roll)
{
    int c;

    q2_rotation_view(m, yaw, pitch, roll);

    /* 0x80055DE4's row scales, expressed on the horizontal so the vertical --
     * which already matches the console to the pixel -- is left alone. */
    for (c = 0; c < 3; c++)
        m[0][c] = (s16)(((s32)m[0][c] * 3) / 2);
}
