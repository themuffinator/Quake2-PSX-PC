/*
 * flare.c — the lens flares, transcribed from 0x80075BDC and everything below it.
 *
 * Six routines meet here and each is named for its address:
 *
 *   0x80075BDC   the frame entry: the gate, then one pass per viewport
 *   0x800759F0   the per-viewport pass: every dynamic light, then every static
 *                light of the node the camera is in
 *   0x80075708   one light: cull, attenuate, pick a colour and a size
 *   0x800752F8   one flare: walk its element list, place each along the
 *                centre-to-light line and draw it
 *   0x80074E6C   the n-gon the BURST's glow is made of
 *   0x80074C4C   the hexagon the DISC is made of — a separate, unrolled routine
 *   0x80074FF4   the eight lines the starburst is made of
 *
 * Everything is untextured and additive. There is no
 * flare bitmap on the disc.
 */
#include "flare.h"
#include "surface.h"

#include "trig.h"

#include <string.h>

/* Host display-state setup, not a field in the retail flare packets. The
 * explicit GPU-state walk exposed a regression: BASE1 reaches the flare pass
 * with mode 0x061f (ABR 0), after intervening model/portal packets. Half blending
 * makes the black rim darken the wall. Preserve the port's intended additive
 * flare pass independently of that incidental predecessor. set_tpage models
 * a preceding draw-mode command, including the usual dither/draw enables. */
static void flare_additive(psx_prim *p)
{
    p->set_tpage = true;
    p->tpage = (u16)(0x0600u | q2_surf_blend_table[Q2_SURF_WORLD_BLEND_SELECTOR]);
}

/* ------------------------------------------------------------------------- */
/* The style table — 0x800A1FDC, 0x800A2014, 0x800A2024, 0x800A203C           */
/*                                                                            */
/* Read out of the executable's data segment; `q2psx-inspect lights` reads the */
/* same bytes back off the disc and compares them element by element.         */
/* ------------------------------------------------------------------------- */
static const q2_flare_element k_style1[] = {   /* 0x800A1FDC — six elements */
    { Q2_FLARE_KIND_BURST, 0x1000,  0x1000, 0x1000 },
    { Q2_FLARE_KIND_DISC,  0x0800, -0x0ED8, 0x0600 },
    { Q2_FLARE_KIND_DISC,  0x044C, -0x0400, 0x0400 },
    { Q2_FLARE_KIND_DISC,  0x0320,  0x0600, 0x0400 },
    { Q2_FLARE_KIND_DISC,  0x0200,  0x1800, 0x0400 },
    { Q2_FLARE_KIND_DISC,  0x0300, -0x1400, 0x0400 }
};

static const q2_flare_element k_style2[] = {   /* 0x800A2014 — the core alone */
    { Q2_FLARE_KIND_BURST, 0x1000,  0x1000, 0x1000 }
};

static const q2_flare_element k_style3[] = {   /* 0x800A2024 */
    { Q2_FLARE_KIND_BURST, 0x1000,  0x1000, 0x1000 },
    { Q2_FLARE_KIND_DISC,  0x0800, -0x0ED8, 0x0600 }
};

static const q2_flare_element k_style4[] = {   /* 0x800A203C — style 1 less one */
    { Q2_FLARE_KIND_BURST, 0x1000,  0x1000, 0x1000 },
    { Q2_FLARE_KIND_DISC,  0x0800, -0x0ED8, 0x0600 },
    { Q2_FLARE_KIND_DISC,  0x044C, -0x0400, 0x0400 },
    { Q2_FLARE_KIND_DISC,  0x0320,  0x0600, 0x0400 },
    { Q2_FLARE_KIND_DISC,  0x0200,  0x1800, 0x0400 }
};

static const q2_flare_style k_styles[Q2_FLARE_STYLE_COUNT] = {
    { NULL,     0 },
    { k_style1, (u32)(sizeof k_style1 / sizeof k_style1[0]) },
    { k_style2, (u32)(sizeof k_style2 / sizeof k_style2[0]) },
    { k_style3, (u32)(sizeof k_style3 / sizeof k_style3[0]) },
    { k_style4, (u32)(sizeof k_style4 / sizeof k_style4[0]) }
};

u32 q2_flare_style_of(u8 type) { return (u32)((type >> 3) & 7u); }
u32 q2_flare_size_of(u8 type)
{
    return (u32)Q2_FLARE_BASE_SIZE << ((type >> 6) & 3u);
}

const q2_flare_style *q2_flare_style_table(u32 style)
{
    static const q2_flare_style empty = { NULL, 0 };
    return style < Q2_FLARE_STYLE_COUNT ? &k_styles[style] : &empty;
}

/* ------------------------------------------------------------------------- */
/* The compiler-folded divides                                                */
/*                                                                            */
/* Six of them, all `mult` / `mfhi` / `sra` / `subu` magic sequences, and all  */
/* six solve to an exact integer divisor — the magic M and post-shift p of a   */
/* signed divide by d satisfy M == floor(2^(32+p)/d) + 1, which pins d.        */
/*                                                                            */
/*   ring    0x66666667 >> 19 -> 1310720 = 320 * 4096                         */
/*           0x88888889 >> 19 ->  983040 = 240 * 4096                         */
/*   spikes  0x14F8B589 >> 16 ->  800000 = 320 * 2500                         */
/*           0x6FD91D85 >> 18 ->  600000 = 240 * 2500                         */
/*   diags   0x1DF5959F >> 17 -> 1120000 = 320 * 3500                         */
/*           0x09FC8735 >> 15 ->  840000 = 240 * 3500                         */
/*                                                                            */
/* So all six are `x * extent / (REF * k)` against the same 320x240 reference  */
/* screen, and only k changes: 4096 for the rings, 2500 for the starburst's    */
/* axis arms, 3500 for its diagonals. That is where the star's proportions     */
/* come from — an arm reaches 4096/2500 = 1.6384x the glow's radius and a      */
/* diagonal 4096/3500 = 1.1703x — and it is why they were measured at 1.637    */
/* and 1.170 back when the arithmetic was reproduced without its closed form.  */
/*                                                                            */
/* C's `/` on s32 truncates toward zero, which is exactly what the magic       */
/* sequence's trailing `- (n >> 31)` produces, so these are the same numbers   */
/* the console computes and not an approximation of them.                     */
/* ------------------------------------------------------------------------- */
#define Q2_FLARE_RING_DIV_X   ((s32)Q2_FLARE_REF_W * Q2_LIGHT_ONE)   /* 1310720 */
#define Q2_FLARE_RING_DIV_Y   ((s32)Q2_FLARE_REF_H * Q2_LIGHT_ONE)   /*  983040 */
#define Q2_FLARE_SPIKE_DIV_X  ((s32)Q2_FLARE_REF_W * Q2_FLARE_SPIKE_REF) /*  800000 */
#define Q2_FLARE_SPIKE_DIV_Y  ((s32)Q2_FLARE_REF_H * Q2_FLARE_SPIKE_REF) /*  600000 */
#define Q2_FLARE_DIAG_DIV_X   ((s32)Q2_FLARE_REF_W * Q2_FLARE_DIAG_REF)  /* 1120000 */
#define Q2_FLARE_DIAG_DIV_Y   ((s32)Q2_FLARE_REF_H * Q2_FLARE_DIAG_REF)  /*  840000 */

/*
 * `sra 12` with a `+4095` bias on negatives (0x80075368, 0x800753A4,
 * 0x800753D0): a divide by 4096 that truncates toward zero rather than
 * flooring. Every element with a negative `pos` — and styles 1, 3 and 4 all
 * have one — spends its whole life on the negative arm, so this is a pixel of
 * difference on a flare that is only a few tens of pixels across.
 */
static s32 div4096_trunc(s32 v)
{
    return (v < 0 ? v + (Q2_LIGHT_ONE - 1) : v) >> Q2_FRAC_12;
}

/*
 * The two multiplies are 32-bit and are allowed to wrap, because the original's
 * are: `mult` / `mflo` twice, with no widening. A flare large enough to
 * overflow would tear the same way there.
 */
static s32 ring_offset_x(s32 angle, s32 radius, s16 extent_x)
{
    s32 v = (s32)(q2_sin12(angle) * radius);
    return (v * extent_x) / Q2_FLARE_RING_DIV_X;
}

static s32 ring_offset_y(s32 angle, s32 radius, s16 extent_y)
{
    s32 v = (s32)(q2_cos12(angle) * radius);
    return (v * extent_y) / Q2_FLARE_RING_DIV_Y;
}

/* ------------------------------------------------------------------------- */
/* 0x80074E6C — a regular n-gon around a centre, plus the centre itself        */
/*                                                                            */
/* out[0] is the centre; out[1..n] the rim; out[n+1] and out[n+2] repeat       */
/* out[1] and out[2] so the caller's fan can read three points past the last   */
/* without wrapping.                                                          */
/*                                                                            */
/* The mirror index is the subtle part. The loop keeps two cursors — one       */
/* walking up from out[0] and one walking DOWN from out[n] — and writes at     */
/* +4 from each, so iteration k fills out[k+1] and out[n+1-k], not out[n-k].   */
/* Off by one there and the ring silently loses a vertex and doubles another:  */
/* at n = 12 it becomes an 11-gon whose rim jumps from the fourth vertex to    */
/* the sixth, which is a visibly lopsided glow.                                */
/*                                                                            */
/* The x offsets mirror and the y offsets do not, which is what makes it       */
/* symmetric about the vertical axis.                                         */
/* ------------------------------------------------------------------------- */
static void flare_ring(psx_xy *out, int n, psx_xy centre, s32 radius,
                       const q2_flare_view *view)
{
    const int step = Q2_ANGLE_360 / n;
    int i;

    out[0] = centre;

    /*
     * `n / 2` truncates, and the loop is inclusive of it — half+1 iterations
     * (0x80074FAC compares before the increment lands). At an even n the last
     * iteration writes the same slot twice; the mirror is second, so it wins,
     * and the order is kept for that reason alone.
     */
    for (i = 0; i <= n / 2; i++) {
        s32 angle = (s32)i * step;
        s32 dx = ring_offset_x(angle, radius, view->extent[0]);
        s32 dy = ring_offset_y(angle, radius, view->extent[1]);

        out[i + 1].x = (s16)(centre.x + dx);
        out[i + 1].y = (s16)(centre.y + dy);

        out[n + 1 - i].x = (s16)(centre.x - dx);
        out[n + 1 - i].y = (s16)(centre.y + dy);
    }

    out[n + 1] = out[1];
    out[n + 2] = out[2];
}

/* ------------------------------------------------------------------------- */
/* 0x80074C4C — the DISC's hexagon, which is its own routine                   */
/*                                                                            */
/* Not a call to the generator above with n = 6. It is written out flat, it    */
/* reads ONE table entry — index 682, the 60 degrees a hexagon turns by — and  */
/* it mirrors that entry into all six vertices instead of looking three up.    */
/* The vertical radius skips the table entirely and divides by 240 rather than */
/* by 240*4096, which is the same number the generator would reach with a      */
/* cosine of 4096 but a different expression of it.                            */
/*                                                                            */
/* The vertex order differs too: this one starts at the TOP and the generator  */
/* starts at the bottom. A hexagon has 180-degree symmetry so the shape and    */
/* the fan's coverage are identical either way, and only the arithmetic is     */
/* distinguishable — which is exactly why it is transcribed rather than        */
/* aliased onto flare_ring.                                                   */
/* ------------------------------------------------------------------------- */
static void flare_hex(psx_xy *out, psx_xy centre, s32 radius,
                      const q2_flare_view *view)
{
    const s32 ry = (radius * view->extent[1]) / Q2_FLARE_REF_H;
    const s32 dx = ring_offset_x(Q2_FLARE_HEX_ANGLE, radius, view->extent[0]);
    const s32 dy = ring_offset_y(Q2_FLARE_HEX_ANGLE, radius, view->extent[1]);

    out[0] = centre;

    out[1].x = centre.x;             out[1].y = (s16)(centre.y - ry);
    out[2].x = (s16)(centre.x + dx); out[2].y = (s16)(centre.y - dy);
    out[3].x = (s16)(centre.x + dx); out[3].y = (s16)(centre.y + dy);
    out[4].x = centre.x;             out[4].y = (s16)(centre.y + ry);
    out[5].x = (s16)(centre.x - dx); out[5].y = (s16)(centre.y + dy);
    out[6].x = (s16)(centre.x - dx); out[6].y = (s16)(centre.y - dy);

    out[7] = out[1];
    out[8] = out[2];
}

/* ------------------------------------------------------------------------- */
/* 0x80074FF4 — the eight spikes                                              */
/*                                                                            */
/* Four iterations, each emitting a pair of Gouraud lines in opposite          */
/* directions from the centre: coloured at the centre, black at the tip. The   */
/* even iterations (0 and 90 degrees) divide by the 2500 reference and the odd */
/* ones (45 and 135) by 3500, which is what gives the star its long axis arms  */
/* and its short diagonals.                                                    */
/*                                                                            */
/* The room test is up front and covers all eight: 0x80075028 compares the     */
/* arena against `cur + 36*n` and returns outright, so a starburst is drawn    */
/* whole or not at all.                                                        */
/* ------------------------------------------------------------------------- */
static u32 flare_burst(psx_xy centre, s32 radius, const u8 rgb[3],
                       const q2_flare_view *view, psx_ot *ot,
                       q2_flare_stats *stats)
{
    const int n = Q2_FLARE_BURST_LINES;
    const int step = Q2_ANGLE_360 / n;
    u32 emitted = 0;
    int i;

    if (ot->prim_count + (u32)n > ot->prim_capacity) {
        if (stats) stats->ot_overflow++;
        return 0;
    }

    for (i = 0; i < n / 2; i++) {
        s32 angle = (s32)i * step;
        s32 vx = (s32)(q2_sin12(angle) * radius) * view->extent[0];
        s32 vy = (s32)(q2_cos12(angle) * radius) * view->extent[1];
        s32 dx, dy;
        int side;

        /* The parity is read BEFORE the counter advances (0x800750F8 against
         * the increment at 0x80075184), so iteration 0 is an axis arm. */
        if (i & 1) {
            dx = vx / Q2_FLARE_DIAG_DIV_X;
            dy = vy / Q2_FLARE_DIAG_DIV_Y;
        } else {
            dx = vx / Q2_FLARE_SPIKE_DIV_X;
            dy = vy / Q2_FLARE_SPIKE_DIV_Y;
        }

        for (side = 0; side < 2; side++) {
            psx_prim *p = psx_ot_add_bucket(ot, view->bucket);
            s32 sign = side ? 1 : -1;

            if (!p) {
                if (stats) stats->ot_overflow++;
                return emitted;
            }

            p->kind  = PSX_PRIM_LINE_G2;
            p->xy[0] = centre;
            p->xy[1].x = (s16)(centre.x + sign * dx);
            p->xy[1].y = (s16)(centre.y + sign * dy);

            p->rgb[0].r = rgb[0];
            p->rgb[0].g = rgb[1];
            p->rgb[0].b = rgb[2];
            p->rgb[1].r = p->rgb[1].g = p->rgb[1].b = 0;

            p->semi_transparent = true;
            p->textured_blend   = false;
            flare_additive(p);
            emitted++;
        }
    }

    return emitted;
}

/* ------------------------------------------------------------------------- */
/* Colour helpers                                                             */
/* ------------------------------------------------------------------------- */
/*
 * `mult` then `sra 16` then `sb` (0x800753EC): the element's 1.12 colour factor
 * applied to a 1.12 component, landing back in 0..255. There is no clamp in the
 * original and none is needed — the component tops out at 255 << 4 and the
 * factor at 4096, and 4080 * 4096 >> 16 is exactly 255 — so this truncates the
 * way the byte store does rather than saturating, and a modded table with a
 * factor above 1.0 would wrap here as it would there.
 */
static u8 element_colour(s16 component, s16 factor)
{
    return (u8)(((s32)component * factor) >> 16);
}

/* ------------------------------------------------------------------------- */
/* 0x800752F8 — one flare's element list                                      */
/* ------------------------------------------------------------------------- */
/*
 * The room tests are the original's, expressed in primitive slots rather than
 * in bytes because this port's ordering table is a slot pool and the console's
 * is a byte arena. On the console a POLY_G4 is 36 bytes and a POLY_F4 24, the
 * glow checks for 6*36 = 216 and the disc for 72 — and then the disc's loop
 * advances the arena by 36 per primitive anyway, so its check covers two of the
 * three it goes on to write. Reproducing that over-run would mean modelling the
 * arena; refusing the whole group is what it buys you either way, since neither
 * path can emit a partial fan.
 *
 * The one behaviour that IS observable is the glow's: when the fan does not fit
 * the routine falls through to 0x80075580 and draws the starburst regardless.
 * A crowded frame loses the disc under the star, not the star.
 */
static u32 flare_emit(const q2_flare_style *style, psx_xy at, s32 scale,
                      const s16 colour[3], const q2_flare_view *view,
                      psx_ot *ot, q2_flare_stats *stats)
{
    u32 emitted = 0;
    u32 e;

    for (e = 0; e < style->count; e++) {
        const q2_flare_element *el = &style->element[e];
        psx_xy ring[Q2_FLARE_RING_MAX + 3];
        psx_xy centre;
        s32 radius;
        u8  rgb[3];
        int i, n;
        bool burst;

        /*
         * Place the element along the line from the screen centre to the
         * light. This — not a corona around the light — is what makes the
         * effect a lens flare: pos 4096 sits on the light, 0 on the centre and
         * a negative value on the far side of it.
         */
        centre.x = (s16)(view->centre[0]
                 + div4096_trunc((s32)(at.x - view->centre[0]) * el->pos));
        centre.y = (s16)(view->centre[1]
                 + div4096_trunc((s32)(at.y - view->centre[1]) * el->pos));

        radius = div4096_trunc(scale * el->size);

        for (i = 0; i < 3; i++)
            rgb[i] = element_colour(colour[i], el->colour);

        /* Anything that is neither kind is skipped and the list continues
         * (0x80075448 falls to the loop tail without touching the table). */
        burst = (el->kind == Q2_FLARE_KIND_BURST);
        if (!burst && el->kind != Q2_FLARE_KIND_DISC)
            continue;

        n = burst ? Q2_FLARE_BURST_SIDES : Q2_FLARE_DISC_SIDES;

        if (ot->prim_count + (u32)(n / 2) <= ot->prim_capacity) {
            if (burst)
                flare_ring(ring, n, centre, radius, view);
            else
                flare_hex(ring, centre, radius, view);

            /*
             * The fan. Each primitive spans two sectors: corners 0 and 1 are the
             * near pair and 2 and 3 the far, which is libgpu's Z order, so a quad
             * of {rim, centre, rim, rim} draws as two triangles both touching the
             * centre.
             */
            for (i = 0; i < n; i += 2) {
                psx_prim *p = psx_ot_add_bucket(ot, view->bucket);
                int c;

                if (!p) {
                    if (stats) stats->ot_overflow++;
                    return emitted;
                }

                p->xy[0] = ring[i + 1];
                p->xy[1] = ring[0];
                p->xy[2] = ring[i + 2];
                p->xy[3] = ring[i + 3];

                if (burst) {
                    /* POLY_G4, code 0x3A: bright at the centre corner and black
                     * at the other three. The gradient IS the glow. */
                    p->kind = PSX_PRIM_G4;
                    for (c = 0; c < 4; c++)
                        p->rgb[c].r = p->rgb[c].g = p->rgb[c].b = 0;
                    p->rgb[1].r = rgb[0];
                    p->rgb[1].g = rgb[1];
                    p->rgb[1].b = rgb[2];
                } else {
                    /* POLY_F4: SetPolyF4 writes code 0x28 and 0x80075640 ORs
                     * in the 0x02 that makes it semi-transparent. */
                    p->kind = PSX_PRIM_F4;
                    p->rgb[0].r = rgb[0];
                    p->rgb[0].g = rgb[1];
                    p->rgb[0].b = rgb[2];
                }

                p->semi_transparent = true;
                p->textured_blend   = false;
                flare_additive(p);
                /* These four corners really are in libgpu's Z order — see the
                 * fan note above — so the backend must split them on the other
                 * diagonal. Without this the second triangle of every sector
                 * came out as a black sliver and the 12-gon glow drew as a
                 * six-spoke pinwheel. */
                p->quad_zorder      = true;
                emitted++;
            }
        } else if (stats) {
            stats->ot_overflow++;
        }

        if (burst)
            emitted += flare_burst(centre, radius, rgb, view, ot, stats);
    }

    if (stats)
        stats->prims_emitted += emitted;

    return emitted;
}

/* ------------------------------------------------------------------------- */
/* 0x80075708 — one light                                                     */
/* ------------------------------------------------------------------------- */
u32 q2_flare_draw(const q2_light *l, const q2_camera *cam,
                  const q2_flare_view *view, psx_ot *ot, gte_state *gte,
                  q2_flare_stats *stats)
{
    const q2_flare_style *style;
    s16 d[3];
    s32 size, atten;
    s16 colour[3];
    s32 scale;
    psx_xy at;
    s32 origin[3];

    if (!l || !cam || !view || !ot || !gte)
        return 0;

    if (stats)
        stats->lights_considered++;

    style = q2_flare_style_table(q2_flare_style_of(l->type));
    if (style->count == 0)
        return 0;

    if (stats)
        stats->lights_styled++;

    size = (s32)q2_flare_size_of(l->type);

    origin[0] = cam->pos[0];
    origin[1] = cam->pos[1];
    origin[2] = cam->pos[2];
    q2_light_delta(l, origin, d);

    /*
     * MVMVA against the view rotation with NO translation vector — the delta
     * is already relative to the camera. The near reject is on the resulting
     * camera-space Z, before any of the attenuation work.
     */
    gte->v[0].x = d[0];
    gte->v[0].y = d[1];
    gte->v[0].z = d[2];
    gte_mvmva(gte, 1, 0, 0, 3, 0);

    if (gte->mac[2] < Q2_FLARE_MIN_Z) {
        if (stats) stats->rejected_near++;
        return 0;
    }

    atten = q2_light_attenuation(l, d, size);
    if (atten == 0) {
        if (stats) stats->rejected_dark++;
        return 0;
    }

    /*
     * Project. The GTE still holds the delta in V0, so RTPS reuses it — and
     * because it goes through the same rotation, projection and integer
     * truncation the world does, a flare sits on exactly the pixel its light
     * would.
     */
    gte_rtps(gte, false);
    at.x = gte->sxy[2].x;
    at.y = gte->sxy[2].y;

    /*
     * The two sides of 4096. Inside the inner radius the colour is the light's
     * own and the flare GROWS; outside it the size pins to its floor and the
     * colour DIMS. (0x80075904 branches on exactly this.)
     */
    if (atten >= Q2_LIGHT_ONE) {
        colour[0] = (s16)((s32)l->r << 4);
        colour[1] = (s16)((s32)l->g << 4);
        colour[2] = (s16)((s32)l->b << 4);
        scale = (atten < 0 ? atten + 127 : atten) >> Q2_FLARE_SCALE_SHIFT;
    } else {
        const u8 rgb[3] = { l->r, l->g, l->b };
        int i;
        for (i = 0; i < 3; i++) {
            s32 v = (s32)rgb[i] * atten;
            colour[i] = (s16)((v < 0 ? v + 255 : v) >> 8);
        }
        scale = Q2_FLARE_MIN_SCALE;
    }

    if (stats)
        stats->flares_drawn++;

    return flare_emit(style, at, scale, colour, view, ot, stats);
}

/* ------------------------------------------------------------------------- */
/* 0x800759F0 — the per-viewport pass                                         */
/* ------------------------------------------------------------------------- */
u32 q2_flare_draw_all(const q2_light_world *w, const q2_camera *cam,
                      s32 coll_node, const q2_flare_view *view,
                      psx_ot *ot, gte_state *gte, q2_flare_stats *stats)
{
    u32 emitted = 0;
    u32 i;

    if (!w || !cam || !view || !ot || !gte)
        return 0;

    for (i = 0; i < w->dynamic_world_count && i < Q2_DYNLIGHT_MAX; i++)
        emitted += q2_flare_draw(&w->dynamic_world[i], cam, view, ot, gte, stats);

    if (coll_node < 0 || !w->space || !w->statics)
        return emitted;

    {
        u32 first, count;

        if (!q2_spacelights_range(w->space, (u32)coll_node, &first, &count))
            return emitted;

        for (i = 0; i < count; i++) {
            u16 index;
            q2_light l;

            if (!q2_spacelights_entry(w->space, first + i, &index))
                break;
            if (!q2_light_get(w->statics, index, &l))
                continue;
            emitted += q2_flare_draw(&l, cam, view, ot, gte, stats);
        }
    }

    return emitted;
}
