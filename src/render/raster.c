#include "raster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void psx_raster_opts_default(psx_raster_opts *opts)
{
    if (!opts)
        return;
    opts->dither            = true;
    opts->affine_uv         = true;
    opts->semi_transparency = true;
    opts->textures          = true;
    opts->clip_x = opts->clip_y = opts->clip_w = opts->clip_h = 0;
    opts->ofs_x  = opts->ofs_y  = 0;
}

/*
 * The pixel range the draw environment allows. A zero-size clip is "no draw env
 * has been applied", which means the whole framebuffer.
 */
static void raster_clip(const psx_framebuffer *fb, const psx_raster_opts *opts,
                        int *x0, int *y0, int *x1, int *y1)
{
    *x0 = 0;
    *y0 = 0;
    *x1 = fb->width;
    *y1 = fb->height;

    /* A negative extent is the composer's explicit EMPTY draw area.  Zero is
     * retained as the historical "no draw env installed" sentinel. */
    if (opts->clip_w < 0 || opts->clip_h < 0) {
        *x0 = *y0 = *x1 = *y1 = 0;
        return;
    }

    if (opts->clip_w > 0 && opts->clip_h > 0) {
        if (opts->clip_x > *x0) *x0 = opts->clip_x;
        if (opts->clip_y > *y0) *y0 = opts->clip_y;
        if (opts->clip_x + opts->clip_w < *x1) *x1 = opts->clip_x + opts->clip_w;
        if (opts->clip_y + opts->clip_h < *y1) *y1 = opts->clip_y + opts->clip_h;
    }
}

q2_result psx_fb_init(psx_framebuffer *fb, int width, int height)
{
    if (!fb || width <= 0 || height <= 0)
        return Q2_ERR_INVALID_ARG;

    fb->px = (u16 *)calloc((size_t)width * (size_t)height, sizeof(u16));
    if (!fb->px)
        return Q2_ERR_NO_MEMORY;

    fb->width  = width;
    fb->height = height;
    return Q2_OK;
}

void psx_fb_free(psx_framebuffer *fb)
{
    if (!fb)
        return;
    free(fb->px);
    fb->px = NULL;
    fb->width = fb->height = 0;
}

void psx_fb_clear(psx_framebuffer *fb, u16 colour)
{
    int i, n;

    if (!fb || !fb->px)
        return;

    n = fb->width * fb->height;
    for (i = 0; i < n; i++)
        fb->px[i] = colour;
}

/* ------------------------------------------------------------------------- */
/* Blending                                                                   */
/* ------------------------------------------------------------------------- */
static void unpack555(u16 c, int *r, int *g, int *b)
{
    *r = (c & 0x1F) << 3;
    *g = ((c >> 5) & 0x1F) << 3;
    *b = ((c >> 10) & 0x1F) << 3;
}

static u16 blend_pixel(u16 back, int r, int g, int b, psx_blend mode)
{
    int br, bg, bb;

    unpack555(back, &br, &bg, &bb);

    switch (mode) {
    case PSX_BLEND_HALF:
        r = (br + r) / 2; g = (bg + g) / 2; b = (bb + b) / 2;
        break;
    case PSX_BLEND_ADD:
        r = br + r; g = bg + g; b = bb + b;
        break;
    case PSX_BLEND_SUB:
        r = br - r; g = bg - g; b = bb - b;
        break;
    case PSX_BLEND_QUARTER:
        r = br + r / 4; g = bg + g / 4; b = bb + b / 4;
        break;
    }

    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;

    return psx_rgb555((u8)r, (u8)g, (u8)b);
}

/* ------------------------------------------------------------------------- */
/* Texture sampling                                                           */
/*                                                                            */
/* Texture pages are 256x256 texels inside the 1024x512 16-bit VRAM. In 4- and */
/* 8-bit modes a texel is an index into a CLUT that also lives in VRAM. UVs    */
/* are u8 and therefore wrap within the page — a UV of 260 is 4, not clamped.  */
/* ------------------------------------------------------------------------- */
static bool sample_texture(const psx_vram *vram, u16 tpage, u16 clut,
                           u8 u, u8 v, int *r, int *g, int *b)
{
    int page_x = (tpage & 0x0F) * 64;      /* in 16-bit words */
    int page_y = ((tpage >> 4) & 1) * 256;
    int bpp    = (tpage >> 7) & 3;
    int clut_x = (clut & 0x3F) * 16;
    int clut_y = (clut >> 6) & 0x1FF;
    u16 texel;

    if (!vram)
        return false;

    if (bpp == 0) {              /* 4 bits per texel: 4 texels per word */
        int vx = page_x + (u >> 2);
        int vy = page_y + v;
        u16 word;
        int index;

        if (vx < 0 || vx >= PSX_VRAM_WIDTH || vy < 0 || vy >= PSX_VRAM_HEIGHT)
            return false;

        word  = vram->px[vy][vx];
        index = (word >> ((u & 3) * 4)) & 0x0F;

        if (clut_y >= PSX_VRAM_HEIGHT || clut_x + index >= PSX_VRAM_WIDTH)
            return false;
        texel = vram->px[clut_y][clut_x + index];

    } else if (bpp == 1) {       /* 8 bits per texel: 2 texels per word */
        int vx = page_x + (u >> 1);
        int vy = page_y + v;
        u16 word;
        int index;

        if (vx < 0 || vx >= PSX_VRAM_WIDTH || vy < 0 || vy >= PSX_VRAM_HEIGHT)
            return false;

        word  = vram->px[vy][vx];
        index = (word >> ((u & 1) * 8)) & 0xFF;

        if (clut_y >= PSX_VRAM_HEIGHT || clut_x + index >= PSX_VRAM_WIDTH)
            return false;
        texel = vram->px[clut_y][clut_x + index];

    } else {                     /* 16 bits per texel, direct colour */
        int vx = page_x + u;
        int vy = page_y + v;

        if (vx < 0 || vx >= PSX_VRAM_WIDTH || vy < 0 || vy >= PSX_VRAM_HEIGHT)
            return false;
        texel = vram->px[vy][vx];
    }

    /* An all-zero texel is fully transparent on this hardware — it is not
     * black. Skipping the pixel entirely is the correct behaviour. */
    if (texel == 0)
        return false;

    unpack555(texel, r, g, b);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Triangle rasterisation                                                     */
/*                                                                            */
/* Edge functions in integer arithmetic, matching the integer vertex positions */
/* the GTE produced. Barycentric weights drive Gouraud colour and affine UVs.  */
/* ------------------------------------------------------------------------- */
typedef struct raster_vertex {
    int x, y;
    int r, g, b;
    int u, v;
} raster_vertex;

static int edge(const raster_vertex *a, const raster_vertex *b, int px, int py)
{
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

/*
 * Is a->b a TOP or LEFT edge of the (already winding-normalised) triangle?
 *
 * With screen y increasing downwards and the weights normalised positive, an
 * edge that travels UP the screen is a left edge, and a horizontal edge that
 * travels RIGHT is a top edge. Those two are the ones the GPU fills; the right
 * and bottom edges are excluded, so a pixel on the seam between two triangles
 * belongs to exactly one of them. See the note in raster_triangle.
 */
static bool edge_is_top_left(const raster_vertex *a, const raster_vertex *b)
{
    if (b->y == a->y)
        return b->x > a->x;     /* horizontal: top when it runs right */
    return b->y < a->y;         /* travels up the screen: a left edge  */
}

static void raster_triangle(psx_framebuffer *fb,
                            const raster_vertex *v0,
                            const raster_vertex *v1,
                            const raster_vertex *v2,
                            const psx_prim *prim,
                            const psx_vram *vram,
                            const psx_raster_opts *opts)
{
    int min_x, max_x, min_y, max_y;
    int area;
    int px, py;
    const raster_vertex *a = v0, *b = v1, *c = v2;

    area = edge(a, b, c->x, c->y);
    if (area == 0)
        return;                     /* degenerate */

    /* The GPU draws both windings. Rather than cull, normalise so the
     * barycentric weights stay positive. Backface rejection is the game's job,
     * via NCLIP, not the rasteriser's. */
    if (area < 0) {
        const raster_vertex *t = b;
        b = c;
        c = t;
        area = -area;
    }

    min_x = a->x < b->x ? (a->x < c->x ? a->x : c->x) : (b->x < c->x ? b->x : c->x);
    max_x = a->x > b->x ? (a->x > c->x ? a->x : c->x) : (b->x > c->x ? b->x : c->x);
    min_y = a->y < b->y ? (a->y < c->y ? a->y : c->y) : (b->y < c->y ? b->y : c->y);
    max_y = a->y > b->y ? (a->y > c->y ? a->y : c->y) : (b->y > c->y ? b->y : c->y);

    {
        int cx0, cy0, cx1, cy1;
        raster_clip(fb, opts, &cx0, &cy0, &cx1, &cy1);
        if (min_x < cx0) min_x = cx0;
        if (min_y < cy0) min_y = cy0;
        if (max_x >= cx1) max_x = cx1 - 1;
        if (max_y >= cy1) max_y = cy1 - 1;
    }

    /*
     * THE FILL RULE, and why a transparent surface used to have its own mesh
     * printed on it.
     *
     * The edge test was inclusive on all three edges (`w >= 0`), so a pixel
     * lying exactly on an edge SHARED by two triangles was rasterised twice.
     * For opaque geometry the second write lays down the same colour and
     * nothing shows, which is why this survived; for a semi-transparent one the
     * blend runs twice and the pixel comes out at B + 2F. Every quad is split
     * into two triangles, so its own diagonal is a shared edge; neighbouring
     * quads share their borders; and a subdivided near quad has 24 of them. The
     * result was the tessellation drawn onto every transparent surface as a
     * bright grid with a diagonal through each cell.
     *
     * The hardware's rule — stated three times in this tree already
     * (raster.h, menufont.c, panel.c) and never implemented here — is that the
     * right and bottom edges of a primitive do not draw. Expressed on the edge
     * function, an edge is "top or left" when it goes up the screen, or is
     * horizontal and runs to the right; those keep `>= 0` and every other edge
     * takes `> 0`. Applied AFTER the winding normalisation above, because the
     * swap changes which edge is which.
     */
    /*
     * THE UV RANGE THIS PRIMITIVE IS ALLOWED TO SAMPLE, and why it has to be
     * bounded rather than cast.
     *
     * The interpolation below is `(w0*u0 + w1*u1 + w2*u2) / area` in integers.
     * The barycentric weights are exact only on the vertices; everywhere else
     * the truncating divide lands the result up to a texel outside the range the
     * three corners span, and the result was then cast straight to `u8`.
     *
     * Both halves of that are visible. One texel over an edge samples whatever
     * is packed NEXT TO this face's texture in the same 256x256 page, which is
     * the stitching along every seam. And the cast WRAPS: a `tu` of 256 becomes
     * 0 and a -1 becomes 255, so a face whose UVs sit against a page boundary
     * jumps to the far side of the page and picks up an unrelated texture
     * entirely. On the blaster that is the emitter's orange arriving as speckles
     * across the body, which is how this was spotted.
     *
     * The hardware does neither. Its texture coordinate unit walks a fixed-point
     * accumulator between the primitive's own endpoints — the artefacts it has
     * are sub-texel swim and the affine warp above, not a jump into a different
     * texture — so clamping to the corners' own range is the faithful bound as
     * well as the correct one. Hoisted out of the pixel loop: it is a property
     * of the triangle.
     *
     * WHAT THIS BOUND CANNOT DO, and what had to be fixed instead: it clamps u
     * and v SEPARATELY, so it is an axis-aligned box round the corners. A UV
     * quad that is not axis-aligned has a footprint smaller than its own box,
     * and a sample can walk off a DIAGONAL edge while staying inside the box —
     * which is precisely what the truncating divide did.
     *
     * Measured on `Blaster G` part 7 face 56, the strip along the top of the
     * gun in BASE0:
     *
     *     screen (370,157) (348,151) (326,151) (341,158)
     *     uv     (161,128) (119,128) (108,139) (148,139)
     *
     * Its top edge runs (119,128) to (108,139), so that edge lies on the
     * anti-diagonal u + v == 247 and the whole quad sits at u + v >= 247. The
     * truncating divide walked the top row of pixels along u + v == 246 — one
     * texel outside the primitive, for all eleven of them — and that row of the
     * real texture is the emitter's orange grille. The gun grew a dashed orange
     * line along its top edge; the console's own frame has no such line, and
     * `test_raster` pins that with the same numbers.
     *
     * Rounding the interpolant to nearest rather than toward zero is what
     * removes it. The truncation is a systematic bias of up to a whole texel in
     * one direction; rounding halves the worst case and centres it, so a sample
     * on an edge stays on the edge instead of stepping off it. This is the one
     * place the backend departs from "reproduce the integer arithmetic" on a
     * judgement rather than a read instruction: no capture settles what the
     * GPU's texture unit rounds. What settles it here is that the old behaviour
     * demonstrably left the primitive and the console demonstrably does not
     * draw the result.
     */
    {
        int umin = a->u < b->u ? (a->u < c->u ? a->u : c->u)
                               : (b->u < c->u ? b->u : c->u);
        int umax = a->u > b->u ? (a->u > c->u ? a->u : c->u)
                               : (b->u > c->u ? b->u : c->u);
        int vmin = a->v < b->v ? (a->v < c->v ? a->v : c->v)
                               : (b->v < c->v ? b->v : c->v);
        int vmax = a->v > b->v ? (a->v > c->v ? a->v : c->v)
                               : (b->v > c->v ? b->v : c->v);

        int bias0 = edge_is_top_left(b, c) ? 0 : -1;
        int bias1 = edge_is_top_left(c, a) ? 0 : -1;
        int bias2 = edge_is_top_left(a, b) ? 0 : -1;

    for (py = min_y; py <= max_y; py++) {
        for (px = min_x; px <= max_x; px++) {
            int w0 = edge(b, c, px, py);
            int w1 = edge(c, a, px, py);
            int w2 = edge(a, b, px, py);
            int r, g, bl;
            u16 *dst;

            if (w0 + bias0 < 0 || w1 + bias1 < 0 || w2 + bias2 < 0)
                continue;

            /* Gouraud: barycentric interpolation of the per-vertex colours. */
            r  = (w0 * a->r + w1 * b->r + w2 * c->r) / area;
            g  = (w0 * a->g + w1 * b->g + w2 * c->g) / area;
            bl = (w0 * a->b + w1 * b->b + w2 * c->b) / area;

            if (opts->textures && vram &&
                (prim->kind == PSX_PRIM_FT3 || prim->kind == PSX_PRIM_GT3 ||
                 prim->kind == PSX_PRIM_FT4 || prim->kind == PSX_PRIM_GT4)) {
                int tu, tv, tr, tg, tb;

                /* Affine: the UVs interpolate in screen space with no 1/w term.
                 * This is the warp, and it is intentional. */
                tu = (w0 * a->u + w1 * b->u + w2 * c->u + area / 2) / area;
                tv = (w0 * a->v + w1 * b->v + w2 * c->v + area / 2) / area;

                /* Bounded by this primitive's own corners — see above. */
                if (tu < umin) tu = umin; else if (tu > umax) tu = umax;
                if (tv < vmin) tv = vmin; else if (tv > vmax) tv = vmax;

                if (!sample_texture(vram, prim->tpage, prim->clut,
                                    (u8)tu, (u8)tv, &tr, &tg, &tb))
                    continue;       /* transparent texel */

                if (prim->textured_blend) {
                    /* Texture modulated by the vertex colour. 0x80 is unity, so
                     * a mid-grey vertex colour leaves the texture unchanged. */
                    r  = (tr * r)  >> 7;
                    g  = (tg * g)  >> 7;
                    bl = (tb * bl) >> 7;
                } else {
                    r = tr; g = tg; bl = tb;
                }
            }

            if (opts->dither) {
                r  = psx_dither_channel((u8)(r  < 0 ? 0 : (r  > 255 ? 255 : r)),  px, py);
                g  = psx_dither_channel((u8)(g  < 0 ? 0 : (g  > 255 ? 255 : g)),  px, py);
                bl = psx_dither_channel((u8)(bl < 0 ? 0 : (bl > 255 ? 255 : bl)), px, py);
            }

            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (bl < 0) bl = 0; else if (bl > 255) bl = 255;

            dst = &fb->px[py * fb->width + px];

            if (opts->semi_transparency && prim->semi_transparent) {
                *dst = blend_pixel(*dst, r, g, bl,
                                   (psx_blend)((prim->tpage >> 5) & 3));
            } else {
                *dst = psx_rgb555((u8)r, (u8)g, (u8)bl);
            }
        }
    }
    }
}

/* ------------------------------------------------------------------------- */
/* Rectangles: sprites and tiles                                              */
/*                                                                            */
/* The GPU rasterises rectangles on a separate path from polygons, and the     */
/* differences are visible rather than academic:                              */
/*                                                                            */
/*   - No interpolation. A sprite's texture coordinates step exactly one texel */
/*     per pixel from its (u, v) origin, so a sprite is never stretched and    */
/*     never warps. This is why the HUD is pin-sharp while the walls swim.     */
/*   - Coordinates wrap in 8 bits, so a sprite that runs off the right edge of */
/*     its texture page reappears at the left. The overlay's glyphs never do,  */
/*     but the wrap is the hardware's and is preserved.                        */
/*   - No dithering. The GPU does not dither rectangle fills, which is why a   */
/*     flat HUD panel is flat and a Gouraud wall is not.                       */
/* ------------------------------------------------------------------------- */
static void raster_rect(psx_framebuffer *fb,
                        const psx_prim *prim,
                        const psx_vram *vram,
                        const psx_raster_opts *opts)
{
    int x0 = prim->xy[0].x + opts->ofs_x;
    int y0 = prim->xy[0].y + opts->ofs_y;
    int x1 = prim->xy[2].x + opts->ofs_x;
    int y1 = prim->xy[2].y + opts->ofs_y;
    bool textured = (prim->kind == PSX_PRIM_SPRT);
    int px, py;
    int cx0, cy0, cx1, cy1;

    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }

    raster_clip(fb, opts, &cx0, &cy0, &cx1, &cy1);

    for (py = y0; py < y1; py++) {
        if (py < cy0 || py >= cy1)
            continue;
        for (px = x0; px < x1; px++) {
            int r = prim->rgb[0].r;
            int g = prim->rgb[0].g;
            int b = prim->rgb[0].b;
            u16 *dst;

            if (px < cx0 || px >= cx1)
                continue;

            if (textured && opts->textures && vram) {
                int tr, tg, tb;
                u8 u = (u8)(prim->uv[0].u + (px - x0));
                u8 v = (u8)(prim->uv[0].v + (py - y0));

                if (!sample_texture(vram, prim->tpage, prim->clut, u, v,
                                    &tr, &tg, &tb))
                    continue;      /* index 0 is transparent, not black */

                if (prim->textured_blend) {
                    r = (tr * r) >> 7;
                    g = (tg * g) >> 7;
                    b = (tb * b) >> 7;
                } else {
                    r = tr; g = tg; b = tb;
                }
            }

            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;

            dst = &fb->px[py * fb->width + px];

            if (opts->semi_transparency && prim->semi_transparent) {
                *dst = blend_pixel(*dst, r, g, b,
                                   (psx_blend)((prim->tpage >> 5) & 3));
            } else {
                *dst = psx_rgb555((u8)r, (u8)g, (u8)b);
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* VRAM -> VRAM copy — GP0(0x80), libgpu's DR_MOVE                            */
/*                                                                            */
/* The only primitive that reads pixels back out of the framebuffer. It is a  */
/* blit and not a draw, so three of the rasteriser's rules do not apply to it: */
/* the drawing offset is not added, the drawing area does not clip it, and    */
/* neither the blend modes nor the dither are involved — the halfwords are     */
/* copied verbatim, mask bit included.                                        */
/*                                                                            */
/* Only the framebuffer bounds constrain it, and the ORIGINAL DOES NOT CHECK   */
/* THEM either: on hardware a copy that runs off the edge wraps within VRAM.   */
/* This port's framebuffer is not the whole of VRAM (see src/screen), so a     */
/* rectangle that leaves it is clipped rather than wrapped. That is a          */
/* divergence, and it is the safe direction: the water warp is the only        */
/* emitter and it keeps its rectangles inside the viewport by construction —   */
/* which is exactly what the screen shake's inset is for.                      */
/*                                                                            */
/* Source and destination overlap on every single use, so the row copy runs    */
/* in whichever direction keeps it non-destructive.                            */
/* ------------------------------------------------------------------------- */
static void raster_move(psx_framebuffer *fb, const psx_prim *prim)
{
    int sx = prim->xy[PSX_MOVE_SRC].x;
    int sy = prim->xy[PSX_MOVE_SRC].y;
    int dx = prim->xy[PSX_MOVE_DST].x;
    int dy = prim->xy[PSX_MOVE_DST].y;
    int w  = prim->xy[PSX_MOVE_SIZE].x;
    int h  = prim->xy[PSX_MOVE_SIZE].y;
    int step, y;

    if (w <= 0 || h <= 0)
        return;

    /* Clip both rectangles together, so a copy never shears. */
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (dx < 0) { w += dx; sx -= dx; dx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (dy < 0) { h += dy; sy -= dy; dy = 0; }

    if (sx + w > fb->width)  w = fb->width  - sx;
    if (dx + w > fb->width)  w = fb->width  - dx;
    if (sy + h > fb->height) h = fb->height - sy;
    if (dy + h > fb->height) h = fb->height - dy;

    if (w <= 0 || h <= 0)
        return;

    /* Rows overlap only when the two rectangles share columns; copying away
     * from the direction of travel is what makes an overlapping move behave as
     * though the source had been read whole. */
    if (dy > sy) {
        y    = h - 1;
        step = -1;
    } else {
        y    = 0;
        step = 1;
    }

    for (; y >= 0 && y < h; y += step)
        memmove(&fb->px[(dy + y) * fb->width + dx],
                &fb->px[(sy + y) * fb->width + sx],
                (size_t)w * sizeof(u16));
}

/* ------------------------------------------------------------------------- */
/* Lines                                                                      */
/*                                                                            */
/* LINE_F2 (code 0x40) and LINE_G2 (0x50). The GPU walks a line with a         */
/* Bresenham-style DDA on the major axis and writes one pixel per step, so a   */
/* line is exactly one pixel thick and its endpoints are BOTH drawn — unlike a */
/* polygon, whose right and bottom edges are dropped. Gouraud lines            */
/* interpolate the two endpoint colours along the same parameter.              */
/*                                                                            */
/* Nothing in the world renderer emits one. The menu does: a slider's frame is */
/* four of them around a 133 x 8 box (0x8001BBAC), so before this existed a    */
/* slider drew as a bare fill with no outline.                                 */
/* ------------------------------------------------------------------------- */
static void raster_line(psx_framebuffer *fb,
                        const psx_prim *prim,
                        const psx_raster_opts *opts)
{
    int x0 = prim->xy[0].x + opts->ofs_x;
    int y0 = prim->xy[0].y + opts->ofs_y;
    int x1 = prim->xy[1].x + opts->ofs_x;
    int y1 = prim->xy[1].y + opts->ofs_y;
    bool gouraud = (prim->kind == PSX_PRIM_LINE_G2);
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    int cx0, cy0, cx1, cy1;
    int i;

    raster_clip(fb, opts, &cx0, &cy0, &cx1, &cy1);

    for (i = 0; i <= steps; i++) {
        /* Rounded so the two ends land exactly on the given endpoints. */
        int px = steps ? x0 + (dx * i + (dx < 0 ? -steps : steps) / 2) / steps : x0;
        int py = steps ? y0 + (dy * i + (dy < 0 ? -steps : steps) / 2) / steps : y0;
        int r, g, b;
        u16 *dst;

        if (px < cx0 || px >= cx1 || py < cy0 || py >= cy1)
            continue;

        if (gouraud && steps) {
            r = prim->rgb[0].r + (prim->rgb[1].r - prim->rgb[0].r) * i / steps;
            g = prim->rgb[0].g + (prim->rgb[1].g - prim->rgb[0].g) * i / steps;
            b = prim->rgb[0].b + (prim->rgb[1].b - prim->rgb[0].b) * i / steps;
        } else {
            r = prim->rgb[0].r;
            g = prim->rgb[0].g;
            b = prim->rgb[0].b;
        }

        if (opts->dither) {
            r = psx_dither_channel((u8)(r < 0 ? 0 : (r > 255 ? 255 : r)), px, py);
            g = psx_dither_channel((u8)(g < 0 ? 0 : (g > 255 ? 255 : g)), px, py);
            b = psx_dither_channel((u8)(b < 0 ? 0 : (b > 255 ? 255 : b)), px, py);
        }
        if (r < 0) r = 0; else if (r > 255) r = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (b < 0) b = 0; else if (b > 255) b = 255;

        dst = &fb->px[py * fb->width + px];

        if (opts->semi_transparency && prim->semi_transparent)
            *dst = blend_pixel(*dst, r, g, b,
                               (psx_blend)((prim->tpage >> 5) & 3));
        else
            *dst = psx_rgb555((u8)r, (u8)g, (u8)b);
    }
}

/* ------------------------------------------------------------------------- */
static void fill_vertex(raster_vertex *rv, const psx_prim *prim, int i, int colour_index)
{
    rv->x = prim->xy[i].x;
    rv->y = prim->xy[i].y;
    rv->r = prim->rgb[colour_index].r;
    rv->g = prim->rgb[colour_index].g;
    rv->b = prim->rgb[colour_index].b;
    rv->u = prim->uv[i].u;
    rv->v = prim->uv[i].v;
}

void psx_raster_prim(psx_framebuffer *fb,
                     const psx_prim *prim,
                     const psx_vram *vram,
                     const psx_raster_opts *opts)
{
    raster_vertex v[4];
    bool gouraud;
    int i;

    if (!fb || !fb->px || !prim || !opts)
        return;

    gouraud = (prim->kind == PSX_PRIM_G3  || prim->kind == PSX_PRIM_GT3 ||
               prim->kind == PSX_PRIM_G4  || prim->kind == PSX_PRIM_GT4);

    for (i = 0; i < 4; i++) {
        fill_vertex(&v[i], prim, i, gouraud ? i : 0);
        /* DRAWENV.ofs — the drawing offset the current draw env installed. The
         * GTE produced viewport-local pixels; this is what puts the viewport
         * where it belongs in the buffer. */
        v[i].x += opts->ofs_x;
        v[i].y += opts->ofs_y;
    }

    /*
     * THE GPU'S OWN SIZE LIMIT, and it is what keeps a near-plane smear off the
     * screen without anything having to test for one.
     *
     * The hardware does not draw a polygon whose vertices are more than 1023
     * apart horizontally or 511 apart vertically — it drops the primitive
     * silently. That is not an optimisation to skip: it is the rule that makes
     * the console's picture clean while its GTE is clamping near vertices to the
     * screen coordinate limits.
     *
     * `gte_divide` raises its overflow when the projection distance reaches twice
     * the depth, returns the clamped 0x1FFFF, and `gte_push_sxy` saturates the
     * result to +/-1024. A face with one vertex out there and the rest in frame
     * spans the whole buffer, and drawing it paints a bright sliver with the
     * texture smeared along it. On the view weapon — whose grip sits 44 units
     * from the eye — that was a dashed orange bar above the blaster and orange
     * streaks down its edges, the emitter's texels stretched across faces that
     * should never have been rasterised.
     *
     * The port already got the first half of this right for the right reason:
     * the executable contains no `cfc2 rX, $31`, so the game never tests the
     * overflow and cannot be discarding these faces itself. It does not need to.
     * The GPU discards them.
     */
    /*
     * POLYGONS ONLY. A rectangle's extent is a width/height field in its packet,
     * not a pair of independent vertices, so it cannot express a span past the
     * limit and the rule has nothing to say about it — the drawing area clips it
     * instead. Applying the span test to tiles as well drops a legitimately
     * oversized one, which is exactly what test_screen builds to check that the
     * viewport's clip rectangle holds.
     */
    if (prim->kind == PSX_PRIM_F3  || prim->kind == PSX_PRIM_FT3 ||
        prim->kind == PSX_PRIM_G3  || prim->kind == PSX_PRIM_GT3 ||
        prim->kind == PSX_PRIM_F4  || prim->kind == PSX_PRIM_FT4 ||
        prim->kind == PSX_PRIM_G4  || prim->kind == PSX_PRIM_GT4) {
        int lox = v[0].x, hix = v[0].x, loy = v[0].y, hiy = v[0].y;
        int n = (prim->kind == PSX_PRIM_F3  || prim->kind == PSX_PRIM_FT3 ||
                 prim->kind == PSX_PRIM_G3  || prim->kind == PSX_PRIM_GT3) ? 3 : 4;

        for (i = 1; i < n; i++) {
            if (v[i].x < lox) lox = v[i].x;
            if (v[i].x > hix) hix = v[i].x;
            if (v[i].y < loy) loy = v[i].y;
            if (v[i].y > hiy) hiy = v[i].y;
        }
        if (hix - lox > PSX_PRIM_MAX_SPAN_X || hiy - loy > PSX_PRIM_MAX_SPAN_Y)
            return;
    }

    switch (prim->kind) {
    case PSX_PRIM_F3:
    case PSX_PRIM_FT3:
    case PSX_PRIM_G3:
    case PSX_PRIM_GT3:
        raster_triangle(fb, &v[0], &v[1], &v[2], prim, vram, opts);
        break;

    case PSX_PRIM_F4:
    case PSX_PRIM_FT4:
    case PSX_PRIM_G4:
    case PSX_PRIM_GT4:
        /*
         * WHICH DIAGONAL A QUAD IS SPLIT ALONG, and why the fan is not it.
         *
         * The GPU draws a four-point polygon as two triangles sharing the edge
         * between its SECOND and THIRD corners: (0,1,2) and (1,3,2). That much
         * was already right here. What was missing is that the corners reaching
         * this backend are NOT in the order the hardware would have seen them.
         *
         * Both linkers convert the file's perimeter order into the hardware's Z
         * order on the way into the packet, and they do it by exchanging corners
         * 2 and 3 — vertices and UVs together, so the pairing survives:
         *
         *   model linker  0x800B2410  SXY0,SXY1,SXY2 <= file v0,v1,v3, then
         *                             SXY0 <= file v2 for the second NCLIP;
         *                             stored at packet +8,+20,+32,+44, so
         *                             x2y2 <= v3 and x3y3 <= v2. The four shift
         *                             counts on the packed index word are
         *                             3, 5, 21, 13 — 0, 1, 3, 2.
         *   world linker  0x800AF844  the identical four shifts, same order.
         *   model emitter 0x8006A3CC..0x8006A3F8
         *                             POLY uv0,uv1,uv2,uv3 <= file uv0,uv1,uv3,
         *                             uv2 (stores to +12,+24,+36,+48).
         *
         * So the hardware's shared edge, expressed in the PERIMETER indices this
         * backend actually holds, is 1—3 — and its two triangles are (0,1,3) and
         * (1,2,3). Fanning (0,1,2)+(0,2,3) covers the same quad but creases it
         * along the OTHER diagonal, 0—2.
         *
         * That is not cosmetic. UVs interpolate affinely per triangle, so the
         * diagonal is what decides which way the texture warps: the same quad
         * split the two ways bends in visibly different directions. A port whose
         * whole point is reproducing the console's affine warp cannot have that
         * backwards.
         *
         * `quad_zorder` says this particular packet is ALREADY in the hardware's
         * order — the flares are built as console packets — so it takes the
         * literal (0,1,2)+(1,3,2) instead.
         *
         * Reading perimeter data as Z-ordered produces a bowtie per quad
         * instead: walls come out as a regular lattice of diamond holes that
         * reads like a clipping bug rather than an index-order one. See the
         * correction in scene.h.
         */
        if (prim->quad_zorder) {
            raster_triangle(fb, &v[0], &v[1], &v[2], prim, vram, opts);
            raster_triangle(fb, &v[1], &v[3], &v[2], prim, vram, opts);
        } else {
            raster_triangle(fb, &v[0], &v[1], &v[3], prim, vram, opts);
            raster_triangle(fb, &v[1], &v[2], &v[3], prim, vram, opts);
        }
        break;

    case PSX_PRIM_SPRT:
    case PSX_PRIM_TILE:
        raster_rect(fb, prim, vram, opts);
        break;

    case PSX_PRIM_LINE_F2:
    case PSX_PRIM_LINE_G2:
        raster_line(fb, prim, opts);
        break;

    /* Deliberately outside the offset the loop above applied to `v`: a copy
     * takes absolute coordinates. */
    case PSX_PRIM_MOVE:
        raster_move(fb, prim);
        break;

    /* Consumed by q2_screen_compose while walking the ordering table. */
    case PSX_PRIM_DRAW_ENV:
        break;

    default:
        break;      /* mode-setting packets carry no pixels */
    }
}

typedef struct walk_ctx {
    psx_framebuffer       *fb;
    const psx_vram        *vram;
    const psx_raster_opts *opts;
} walk_ctx;

static void walk_visit(const psx_prim *prim, void *user)
{
    walk_ctx *ctx = (walk_ctx *)user;
    psx_raster_prim(ctx->fb, prim, ctx->vram, ctx->opts);
}

void psx_raster_ot(psx_framebuffer *fb,
                   psx_ot *ot,
                   const psx_vram *vram,
                   const psx_raster_opts *opts)
{
    walk_ctx ctx;

    ctx.fb   = fb;
    ctx.vram = vram;
    ctx.opts = opts;

    psx_ot_walk(ot, walk_visit, &ctx);
}

/* ------------------------------------------------------------------------- */
q2_result psx_fb_write_ppm(const psx_framebuffer *fb, const char *path)
{
    FILE *f;
    int i, n;

    if (!fb || !fb->px || !path)
        return Q2_ERR_INVALID_ARG;

    f = fopen(path, "wb");
    if (!f)
        return Q2_ERR_IO;

    fprintf(f, "P6\n%d %d\n255\n", fb->width, fb->height);

    n = fb->width * fb->height;
    for (i = 0; i < n; i++) {
        int r, g, b;
        u8 rgb[3];

        unpack555(fb->px[i], &r, &g, &b);
        rgb[0] = (u8)r;
        rgb[1] = (u8)g;
        rgb[2] = (u8)b;
        fwrite(rgb, 1, 3, f);
    }

    fclose(f);
    return Q2_OK;
}
