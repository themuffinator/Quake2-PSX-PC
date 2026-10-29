#include "panel.h"

/*
 * One patch of border.
 *
 * The console writes four corners in the packet's Z order (top-left,
 * top-right, bottom-left, bottom-right) and mirrors a piece by making the
 * screen corners run backwards against ascending texture coordinates. Both of
 * those have to be translated once, here:
 *
 *   - PERIMETER ORDER. This port's rasteriser fans a quad as (0,1,2)+(0,2,3),
 *     so slot 2 must hold the far corner and slot 3 the bottom-left. Handing it
 *     a Z-ordered quad draws a bowtie (menufont.c says the same thing about
 *     glyphs, and there it turns 'B' into 'E').
 *
 *   - HALF-OPEN CORNERS. The GPU's fill rule drops the right and bottom edges,
 *     so its (x0, y0)..(x1, y1) covers the pixels strictly inside. The corners
 *     used to be pulled one pixel back towards the near one to compensate for a
 *     rasteriser that admitted both edges; raster.c implements the real rule
 *     now, so the console's coordinates go through untouched.
 */

static psx_prim *emit_patch(psx_ot *ot, u32 bucket, u16 tpage, u16 clut,
                            int x0, int y0, int x1, int y1,
                            int u0, int v0, int u1, int v1)
{
    psx_prim *p = psx_ot_add_bucket(ot, bucket);
    int i;

    if (!p)
        return NULL;

    p->kind  = PSX_PRIM_FT4;
    p->tpage = tpage;
    p->clut  = clut;
    /* code 0x2D: the raw bit is set, so the texture is NOT modulated, and the
     * ABE bit is clear, so the frame is opaque over the body it darkened. */
    p->textured_blend   = false;
    p->semi_transparent = false;

    p->xy[0].x = (s16)x0;  p->xy[0].y = (s16)y0;
    p->xy[1].x = (s16)x1;  p->xy[1].y = (s16)y0;
    p->xy[2].x = (s16)x1;  p->xy[2].y = (s16)y1;
    p->xy[3].x = (s16)x0;  p->xy[3].y = (s16)y1;

    p->uv[0].u = (u8)u0;   p->uv[0].v = (u8)v0;
    p->uv[1].u = (u8)u1;   p->uv[1].v = (u8)v0;
    p->uv[2].u = (u8)u1;   p->uv[2].v = (u8)v1;
    p->uv[3].u = (u8)u0;   p->uv[3].v = (u8)v1;

    for (i = 0; i < 4; i++) {
        p->rgb[i].r = 0x80;
        p->rgb[i].g = 0x80;
        p->rgb[i].b = 0x80;
    }
    return p;
}

u32 q2_panel_frame_ot(const q2_menu_font *font, const q2_panel_rect *r,
                      psx_ot *ot, u32 bucket)
{
    /*
     * Every number below is the expression the builder at 0x8003E8D0 stores,
     * in the order it stores them. The asymmetries are the original's and are
     * left alone: the bottom-left corner samples v 223..253 where the
     * bottom-right samples 223..252, and the right edge starts at x + w - 1
     * where the left ends at x + 2.
     */
    const int X = r ? r->x : 0, Y = r ? r->y : 0;
    const int W = r ? r->w : 0, H = r ? r->h : 0;
    const int CU = Q2_PANEL_CORNER_U, CU2 = Q2_PANEL_CORNER_U + Q2_PANEL_CORNER_W;
    u16 tpage, clut;
    u32 n = 0;

    if (!font || !r || !ot)
        return 0;

    tpage = font->tpage_item;   /* frontend.lbm, VRAM slot 13 */
    clut  = font->clut_text;    /* the halfword at 0x800E3FB4 */

    /* Four corners from one 46 x 29 patch, mirrored by corner order. */
    if (emit_patch(ot, bucket, tpage, clut,
                   X - 10,     Y - 6,    X + 36,     Y + 23,
                   CU, 224, CU2, 253)) n++;
    if (emit_patch(ot, bucket, tpage, clut,
                   X + W + 10, Y - 6,    X + W - 36, Y + 23,
                   CU, 224, CU2, 253)) n++;
    if (emit_patch(ot, bucket, tpage, clut,
                   X + W + 10, Y + H + 6, X + W - 36, Y + H - 24,
                   CU, 223, CU2, 252)) n++;
    if (emit_patch(ot, bucket, tpage, clut,
                   X - 10,     Y + H + 6, X + 36,     Y + H - 24,
                   CU, 223, CU2, 253)) n++;

    /* Top and bottom edges: 19 texels stretched across the span between the
     * corners, six rows tall, drawn 1:1 vertically. */
    if (emit_patch(ot, bucket, tpage, clut,
                   X + 36, Y - 6, X + W - 34, Y + 1,
                   Q2_PANEL_EDGE_H_U, Q2_PANEL_EDGE_H_V,
                   Q2_PANEL_EDGE_H_U + Q2_PANEL_EDGE_H_W,
                   Q2_PANEL_EDGE_H_V + Q2_PANEL_EDGE_H_H)) n++;
    if (emit_patch(ot, bucket, tpage, clut,
                   X + 36, Y + H, X + W - 34, Y + H + 7,
                   Q2_PANEL_EDGE_H_U, Q2_PANEL_EDGE_H_V,
                   Q2_PANEL_EDGE_H_U + Q2_PANEL_EDGE_H_W,
                   Q2_PANEL_EDGE_H_V + Q2_PANEL_EDGE_H_H)) n++;

    /* Left and right edges: 11 texels across 12 pixels, stretched vertically
     * between the corners. Neither is mirrored — the art is symmetric enough
     * that the console reuses it as it stands. */
    if (emit_patch(ot, bucket, tpage, clut,
                   X - 10, Y + 22, X + 2, Y + H - 22,
                   Q2_PANEL_EDGE_V_U, Q2_PANEL_EDGE_V_V,
                   Q2_PANEL_EDGE_V_U + Q2_PANEL_EDGE_V_W,
                   Q2_PANEL_EDGE_V_V + Q2_PANEL_EDGE_V_H)) n++;
    if (emit_patch(ot, bucket, tpage, clut,
                   X + W - 1, Y + 22, X + W + 10, Y + H - 22,
                   Q2_PANEL_EDGE_V_U, Q2_PANEL_EDGE_V_V,
                   Q2_PANEL_EDGE_V_U + Q2_PANEL_EDGE_V_W,
                   Q2_PANEL_EDGE_V_V + Q2_PANEL_EDGE_V_H)) n++;

    return n;
}

u32 q2_panel_body_ot(const q2_panel_rect *r, psx_ot *ot, u32 bucket)
{
    u32 n;

    if (!r || !ot)
        return 0;

    /*
     * The same black tile twice. Once is B/2 + 0/2 — half brightness; twice is
     * a quarter, which is the smoked-glass level the screens actually show.
     * Emitting one tile at 25% would look right in a still and wrong the moment
     * anything else semi-transparent overlapped it.
     */
    for (n = 0; n < Q2_PANEL_BODY_PRIMS; n++) {
        psx_prim *p = psx_ot_add_bucket(ot, bucket);

        if (!p)
            break;

        p->kind = PSX_PRIM_TILE;
        p->semi_transparent = true;
        p->textured_blend   = false;
        p->tpage = 0x0600u;
        p->set_tpage = true; /* the paired E1000600 draw-mode packets */
        p->clut  = 0;

        /*
         * OPPOSITE CORNERS, not position-and-size.
         *
         * A hardware TILE packet carries {xy, wh}, and this used to write that
         * — which is faithful to the packet and wrong for this port, because
         * `raster_rect` takes the rectangle from `xy[0]` and `xy[2]`. `xy[2]`
         * was never written, so the darkening tile came out as a small box in
         * the corner of the screen at (0,0)-(x,y) and the panel had no body at
         * all. Every other rectangle in the port — the HUD's sprites
         * (`hud.c`), the damage flash and the viewport tiles (`screen.c`) —
         * already writes corners; this was the one that did not, and it is the
         * one nothing else called.
         */
        p->xy[0].x = r->x;                  p->xy[0].y = r->y;
        p->xy[2].x = (s16)(r->x + r->w);    p->xy[2].y = (s16)(r->y + r->h);

        p->rgb[0].r = Q2_PANEL_BODY_SHADE;
        p->rgb[0].g = Q2_PANEL_BODY_SHADE;
        p->rgb[0].b = Q2_PANEL_BODY_SHADE;
    }
    return n;
}

u32 q2_panel_build_ot(const q2_menu_font *font, const q2_panel_rect *r,
                      psx_ot *ot, u32 body_bucket, u32 frame_bucket,
                      q2_panel_rect *last)
{
    u32 n;

    if (!font || !r || !ot)
        return 0;

    /* 0x8003F124: the rectangle is stashed before anything is drawn, and the
     * screens that place text inside a panel read it back from there. */
    if (last)
        *last = *r;

    n  = q2_panel_body_ot(r, ot, body_bucket);
    n += q2_panel_frame_ot(font, r, ot, frame_bucket);
    return n;
}

void q2_panel_bounds(const q2_panel_rect *r, q2_panel_rect *out)
{
    if (!r || !out)
        return;

    out->x = (s16)(r->x - Q2_PANEL_OVERHANG_X);
    out->y = (s16)(r->y - Q2_PANEL_OVERHANG_Y);
    out->w = (s16)(r->w + 2 * Q2_PANEL_OVERHANG_X);
    out->h = (s16)(r->h + 2 * Q2_PANEL_OVERHANG_Y);
}
