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
    opts->texture_window = 0;
    opts->mask_mode = 0;
    opts->native_coords = false;
    opts->skip_field = false;
    opts->field = 0;
    opts->missing_texture_bank = false;
    opts->cache = NULL;
    opts->scene = NULL;
    opts->ui_mask = NULL;
    opts->ui = false;
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
    fb->vram = NULL;
    fb->vram_x = fb->vram_y = 0;
    return Q2_OK;
}

void psx_fb_free(psx_framebuffer *fb)
{
    if (!fb)
        return;
    free(fb->px);
    fb->px = NULL;
    fb->width = fb->height = 0;
    fb->vram = NULL;
}

q2_result psx_fb_bind_vram(psx_framebuffer *fb, psx_vram *vram, int x, int y)
{
    int row;
    if (!fb || !fb->px) return Q2_ERR_INVALID_ARG;
    if (vram && (x < 0 || y < 0 || x + fb->width > PSX_VRAM_WIDTH ||
                 y + fb->height > PSX_VRAM_HEIGHT)) return Q2_ERR_INVALID_ARG;
    fb->vram = vram;
    fb->vram_x = x; fb->vram_y = y;
    if (vram) for (row = 0; row < fb->height; ++row)
        memcpy(fb->px + row * fb->width, &vram->px[y+row][x],
               (size_t)fb->width * sizeof(u16));
    return Q2_OK;
}

void psx_fb_store(psx_framebuffer *fb, int x, int y, u16 colour)
{
    fb->px[y * fb->width + x] = colour;
    if (fb->vram) fb->vram->px[fb->vram_y+y][fb->vram_x+x] = colour;
}

void psx_fb_clear(psx_framebuffer *fb, u16 colour)
{
    int i, n;

    if (!fb || !fb->px)
        return;

    n = fb->width * fb->height;
    for (i = 0; i < n; i++)
        fb->px[i] = colour;
    if (fb->vram) {
        int x, y;
        for (y=0;y<fb->height;++y) for (x=0;x<fb->width;++x)
            fb->vram->px[fb->vram_y+y][fb->vram_x+x] = colour;
    }
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

    /* Blending consumes RGB555, after modulation/dither/reduction. Keeping
     * fractional foreground bits until afterwards makes subtraction too dark
     * (B=8, F=7 must leave channel 1, not turn it into channel 0). */
    br = back & 31; bg = (back >> 5) & 31; bb = (back >> 10) & 31;
    r >>= 3; g >>= 3; b >>= 3;

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

    if (r < 0) r = 0; else if (r > 31) r = 31;
    if (g < 0) g = 0; else if (g > 31) g = 31;
    if (b < 0) b = 0; else if (b > 31) b = 31;

    return (u16)(r | (g << 5) | (b << 10));
}

/* ------------------------------------------------------------------------- */
/* Texture sampling                                                           */
/*                                                                            */
/* Texture pages are 256x256 texels inside the 1024x512 16-bit VRAM. In 4- and */
/* 8-bit modes a texel is an index into a CLUT that also lives in VRAM. UVs    */
/* are u8 and therefore wrap within the page — a UV of 260 is 4, not clamped.  */
/* ------------------------------------------------------------------------- */
/* Keep the ordinary RGB555 result untouched. The optional scene copy blends
 * against its own background, so UI never leaks into bloom even when a later
 * damage flash or water move follows the HUD in the ordering table. */
static void write_pixel(psx_framebuffer *fb, const psx_raster_opts *opts,
                        const psx_prim *prim, int at, int r, int g, int b,
                        bool semi, bool tex_stp)
{
    psx_blend mode = (psx_blend)((prim->tpage >> 5) & 3);
    u16 colour = psx_rgb555((u8)r, (u8)g, (u8)b);
    u16 mask = ((opts->mask_mode & 1) || tex_stp) ? 0x8000u : 0;
    if (opts->skip_field && (((at / fb->width + fb->vram_y) & 1) == opts->field)) return;
    if ((opts->mask_mode & 2) && (fb->px[at] & 0x8000u)) return;
    fb->px[at] = (u16)((semi ? blend_pixel(fb->px[at], r, g, b, mode) : colour) | mask);
    if (fb->vram)
        fb->vram->px[fb->vram_y+at/fb->width][fb->vram_x+at%fb->width] = fb->px[at];
    if (opts->scene) {
        if (opts->ui || prim->ui) {
            opts->ui_mask[at] = 255;
        } else {
            opts->scene[at] = (u16)((semi ? blend_pixel(opts->scene[at], r, g, b, mode) : colour) | mask);
            if (!semi) opts->ui_mask[at] = 0;
        }
    }
}

/* Cache indexing/invalidation follows Mednafen's GPL-2.0-or-later PS1 GPU
 * (gpu_common.inc / gpu.cpp). See README credits and the fixture provenance.
 * In particular 8-bit mode caches 64x32 texels, not the old 32x64 description. */
static u16 texture_word(const psx_vram *vram, psx_texture_cache *cache,
                        unsigned x, unsigned y, unsigned bpp)
{
    unsigned address=y*1024+x, line, base=address&~3u;
    if (!cache) return vram->px[y][x];
    line=bpp==0 ? ((address>>2)&3)|((address>>8)&252)
                : ((address>>2)&7)|((address>>7)&248);
    if (cache->tag[line]!=base+1) {
        memcpy(cache->word[line],&vram->px[y][x&~3u],8);
        cache->tag[line]=base+1;
    }
    return cache->word[line][address&3];
}

static void cache_palette(psx_texture_cache *cache, const psx_vram *vram,
                           u16 clut, u16 mode)
{
    unsigned bpp=(mode>>7)&3, i, tag;
    if (!vram || bpp>=2) return;
    tag=((clut&0x7FFFu)|(bpp<<16))+1;
    /* A full 256-entry load satisfies a later 4-bit request for the same
     * palette. This is visible in ps1-tests/clut-cache row 152; comparing the
     * depth bits for equality (as Mednafen does) reloads that row incorrectly. */
    if (cache->clut_tag==tag || (!bpp && cache->clut_tag==(tag|0x10000u))) return;
    for (i=0;i<(bpp?256u:16u);++i)
        cache->clut[i]=vram->px[(clut>>6)&511][((clut&63)*16+i)&1023];
    cache->clut_tag=tag;
}

static bool sample_texture(const psx_vram *vram, psx_texture_cache *cache, u16 tpage, u16 clut,
                           u32 window, u8 u, u8 v,
                           int *r, int *g, int *b, bool *stp)
{
    int page_x = (tpage & 0x0F) * 64;      /* in 16-bit words */
    int page_y = ((tpage >> 4) & 1) * 256;
    int bpp    = (tpage >> 7) & 3;
    int clut_x = (clut & 0x3F) * 16;
    int clut_y = (clut >> 6) & 0x1FF;
    u16 texel;

    if (!vram)
        return false;

    /* E2 replaces selected UV bits before addressing either packed texels or
     * direct colour. Non-contiguous masks are legal; this is not a rectangle
     * clamp. Texture and CLUT fetches wrap on the same VRAM row. */
    {
        u32 mx = (window & 31u) << 3, my = ((window >> 5) & 31u) << 3;
        u = (u8)((u & ~mx) | (((window >> 10) << 3) & mx));
        v = (u8)((v & ~my) | (((window >> 15) << 3) & my));
    }

    if (bpp == 0) {              /* 4 bits per texel: 4 texels per word */
        int vx = (page_x + (u >> 2)) & 1023;
        int vy = page_y + v;
        u16 word;
        int index;

        word  = texture_word(vram,cache,(unsigned)vx,(unsigned)vy,0);
        index = (word >> ((u & 3) * 4)) & 0x0F;

        texel = cache ? cache->clut[index] : vram->px[clut_y][(clut_x + index) & 1023];

    } else if (bpp == 1) {       /* 8 bits per texel: 2 texels per word */
        int vx = (page_x + (u >> 1)) & 1023;
        int vy = page_y + v;
        u16 word;
        int index;

        word  = texture_word(vram,cache,(unsigned)vx,(unsigned)vy,1);
        index = (word >> ((u & 1) * 8)) & 0xFF;

        texel = cache ? cache->clut[index] : vram->px[clut_y][(clut_x + index) & 1023];

    } else {                     /* 16 bits per texel, direct colour */
        int vx = (page_x + u) & 1023;
        int vy = page_y + v;

        texel = texture_word(vram,cache,(unsigned)vx,(unsigned)vy,2);
    }

    /* An all-zero texel is fully transparent on this hardware — it is not
     * black. Skipping the pixel entirely is the correct behaviour. */
    if (texel == 0)
        return false;

    unpack555(texel, r, g, b);
    *stp = (texel & 0x8000u) != 0;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Triangle rasterisation                                                     */
/*                                                                            */
/* Fixed-point edges and UV/RGB accumulators; see raster_scan.inc.           */
/* ------------------------------------------------------------------------- */
typedef struct raster_vertex {
    int x, y;
    int r, g, b;
    int u, v;
} raster_vertex;

static int signed11(int value)
{
    return (value & 1023) - (value & 1024);
}

#include "raster_scan.inc"

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
    bool textured = prim->kind == PSX_PRIM_SPRT && opts->textures && vram;
    int px, py;
    int cx0, cy0, cx1, cy1;

    if (textured && opts->missing_texture_bank) return;
    if (opts->native_coords) {
        int nx=signed11(x0+fb->vram_x)-fb->vram_x;
        int ny=signed11(y0+fb->vram_y)-fb->vram_y;
        x1+=nx-x0; y1+=ny-y0; x0=nx; y0=ny;
    }
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
            bool stp = false;

            if (px < cx0 || px >= cx1)
                continue;

            if (textured) {
                int tr, tg, tb;
                int du = (prim->sprite_w
                            ? (px - x0) * prim->sprite_w / (x1 - x0)
                            : (px - x0));
                int dv = (prim->sprite_h
                            ? (py - y0) * prim->sprite_h / (y1 - y0)
                            : (py - y0));
                u8 u = (u8)(prim->uv[0].u + ((prim->tpage & 0x1000u) ? -du : du));
                u8 v = (u8)(prim->uv[0].v + ((prim->tpage & 0x2000u) ? -dv : dv));

                if (!sample_texture(vram, opts->cache, prim->tpage, prim->clut, opts->texture_window, u, v,
                                    &tr, &tg, &tb, &stp))
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

            write_pixel(fb, opts, prim, py * fb->width + px, r, g, b,
                        opts->semi_transparency && prim->semi_transparent && (!textured || stp), stp);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* VRAM copies buffer 128 words at a time, left-to-right and top-to-bottom.
 * Vertical overlap intentionally feeds earlier destination rows back into
 * later source rows. Mednafen gpu.cpp (GPL-2.0-or-later, Mednafen Team
 * 2011-2017) documents the chunk size; ps1-tests supplies overlap captures.
 * A bound native target wraps independently at 1024/512. Standalone/adaptive
 * targets retain host clipping, with the same transfer order. */
static void raster_move(psx_framebuffer *fb, const psx_prim *prim,
                        const psx_raster_opts *opts)
{
    int sx=prim->xy[0].x, sy=prim->xy[0].y;
    int dx=prim->xy[1].x, dy=prim->xy[1].y;
    int w=prim->xy[2].x, h=prim->xy[2].y;
    int y, x, j;
    if (fb->vram) {
        w=((w-1)&1023)+1; h=((h-1)&511)+1;
        /* Logical screen packets are relative to this buffer's origin. */
        sx+=fb->vram_x; dx+=fb->vram_x;
        sy+=fb->vram_y; dy+=fb->vram_y;
    } else {
        if (w<=0 || h<=0) return;
        if (sx<0) { w+=sx; dx-=sx; sx=0; }
        if (dx<0) { w+=dx; sx-=dx; dx=0; }
        if (sy<0) { h+=sy; dy-=sy; sy=0; }
        if (dy<0) { h+=dy; sy-=dy; dy=0; }
        if (sx+w>fb->width) w=fb->width-sx;
        if (dx+w>fb->width) w=fb->width-dx;
        if (sy+h>fb->height) h=fb->height-sy;
        if (dy+h>fb->height) h=fb->height-dy;
    }
    for (y=0;y<h;++y) for (x=0;x<w;x+=128) {
        u16 pixels[128], scene[128];
        u8 ui[128];
        int n=w-x<128?w-x:128;
        for (j=0;j<n;++j) {
            int rx=sx+x+j, ry=sy+y, local_x=rx, local_y=ry;
            if (fb->vram) {
                rx&=1023; ry&=511;
                pixels[j]=fb->vram->px[ry][rx];
                local_x=rx-fb->vram_x; local_y=ry-fb->vram_y;
            } else pixels[j]=fb->px[ry*fb->width+rx];
            scene[j]=pixels[j]; ui[j]=0;
            if (opts->scene && local_x>=0 && local_x<fb->width &&
                local_y>=0 && local_y<fb->height) {
                int at=local_y*fb->width+local_x;
                scene[j]=opts->scene[at]; ui[j]=opts->ui_mask[at];
            }
        }
        for (j=0;j<n;++j) {
            int wx=dx+x+j, wy=dy+y, local_x=wx, local_y=wy;
            u16 *dst, mask=(opts->mask_mode&1)?0x8000:0;
            if (fb->vram) {
                wx&=1023; wy&=511;
                dst=&fb->vram->px[wy][wx];
                local_x=wx-fb->vram_x; local_y=wy-fb->vram_y;
            } else dst=&fb->px[wy*fb->width+wx];
            if ((opts->mask_mode&2) && (*dst&0x8000)) continue;
            *dst=pixels[j]|mask;
            if (local_x>=0 && local_x<fb->width && local_y>=0 && local_y<fb->height) {
                int at=local_y*fb->width+local_x;
                fb->px[at]=pixels[j]|mask;
                if (opts->scene) {
                    opts->scene[at]=scene[j]|mask; opts->ui_mask[at]=ui[j];
                }
            }
        }
    }
}

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
    psx_prim decoded;
    bool gouraud;
    int i;

    if (!fb || !fb->px || !prim || !opts)
        return;

    if (opts->native_coords && prim->kind <= PSX_PRIM_TILE) {
        decoded = *prim;
        if (prim->kind == PSX_PRIM_SPRT || prim->kind == PSX_PRIM_TILE) {
            /* These are host endpoint extents, not encoded GP0 size words.
             * Keep extended UI/diagnostic rectangles; decode positions only. */
            int w=prim->xy[2].x-prim->xy[0].x;
            int h=prim->xy[2].y-prim->xy[0].y;
            decoded.xy[0].x=(s16)signed11(prim->xy[0].x);
            decoded.xy[0].y=(s16)signed11(prim->xy[0].y);
            decoded.xy[2].x=(s16)(decoded.xy[0].x+w);
            decoded.xy[2].y=(s16)(decoded.xy[0].y+h);
        } else for (i=0;i<4;++i) {
            decoded.xy[i].x=(s16)signed11(prim->xy[i].x);
            decoded.xy[i].y=(s16)signed11(prim->xy[i].y);
        }
        prim=&decoded;
    }

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
        /* GPU packet order is (0,1,2), then (1,2,3). The game linkers
         * exchange perimeter vertices/UVs 2 and 3 (model 0x800B2410 and
         * 0x8006A3E4; world 0x800AF844), so perimeter triangles are (0,1,3)
         * and (1,3,2). Preserve their vertex order as well as the diagonal:
         * the leftmost-vertex tie selects the fixed-point attribute origin.
         * Flares and other already-encoded packets set quad_zorder. */
        if (prim->quad_zorder) {
            raster_triangle(fb, &v[0], &v[1], &v[2], prim, vram, opts);
            raster_triangle(fb, &v[1], &v[2], &v[3], prim, vram, opts);
        } else {
            raster_triangle(fb, &v[0], &v[1], &v[3], prim, vram, opts);
            raster_triangle(fb, &v[1], &v[3], &v[2], prim, vram, opts);
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
        raster_move(fb, prim, opts);
        break;

    /* Consumed by q2_screen_compose while walking the ordering table. */
    case PSX_PRIM_DRAW_ENV:
        break;

    default:
        break;      /* mode-setting packets carry no pixels */
    }
}

void psx_draw_state_gp1(psx_draw_state *state, u32 command)
{
    if (!state) return;
    switch (command >> 24) {
    case 0: {
        /* GP1(09)'s permission latch survives a GPU soft reset. */
        bool allow=state->allow_texture_bank;
        memset(state,0,sizeof(*state));
        state->allow_texture_bank=allow;
        break;
    }
    case 1: state->cache.clut_tag=0; break; /* reset command buffer */
    case 5: state->display_y=(u16)((command>>10)&511); break;
    case 8: state->display_mode=(u8)command; break;
    case 9: state->allow_texture_bank=(command&1)!=0; break;
    default: break;
    }
}

static void draw_mode(psx_draw_state *state, u16 value, u16 mask)
{
    u16 previous=state->mode;
    if (!state->allow_texture_bank) mask &= ~0x0800u;
    state->mode=(u16)((state->mode & ~mask) | (value & mask));
    if (((previous^state->mode)&0x081Fu) ||
        (!(previous&0x0180u)!=!(state->mode&0x0180u)))
        memset(state->cache.tag,0,sizeof(state->cache.tag));
}

void psx_draw_state_mode(psx_draw_state *state, u16 mode)
{
    if (state) draw_mode(state,mode,0x3FFFu);
}

void psx_draw_state_clear_cache(psx_draw_state *state)
{
    if (!state) return;
    memset(state->cache.tag,0,sizeof(state->cache.tag));
    state->cache.clut_tag=0;
}

void psx_raster_command(psx_framebuffer *fb, const psx_prim *prim,
                        const psx_vram *vram, const psx_raster_opts *opts,
                        psx_draw_state *state)
{
    psx_prim resolved;
    psx_raster_opts local;
    bool polygon_page;
    if (!prim || !opts || !state) return;
    if (vram && state->cache.owner!=vram) {
        psx_draw_state_clear_cache(state);
        state->cache.owner=vram;
        state->cache.transfer_serial=vram->transfer_serial;
    }
    if (vram && state->cache.transfer_serial!=vram->transfer_serial) {
        memset(state->cache.tag,0,sizeof(state->cache.tag));
        state->cache.transfer_serial=vram->transfer_serial;
    }
    if (prim->clear_cache || prim->kind==PSX_PRIM_CACHE_CLEAR)
        psx_draw_state_clear_cache(state);
    if (prim->kind==PSX_PRIM_CACHE_CLEAR) return;
    if (prim->kind==PSX_PRIM_MOVE)
        memset(state->cache.tag,0,sizeof(state->cache.tag));
    if (prim->kind == PSX_PRIM_TPAGE) {
        draw_mode(state,prim->tpage,0x3FFFu);
        return;
    }
    if (prim->kind == PSX_PRIM_TWINDOW) {
        state->texture_window = prim->texture_window & 0xFFFFFu;
        return;
    }
    if (prim->kind == PSX_PRIM_MASK) {
        state->mask_mode = prim->mask_mode & 3u;
        return;
    }
    polygon_page = prim->kind == PSX_PRIM_FT3 || prim->kind == PSX_PRIM_FT4 ||
                   prim->kind == PSX_PRIM_GT3 || prim->kind == PSX_PRIM_GT4;
    /* This happens even for an opaque, fully clipped or oversized polygon:
     * packet decoding changes the register before rasterization can reject it. */
    if (prim->set_tpage)
        draw_mode(state,prim->tpage,0x3FFFu);
    else if (polygon_page)
        draw_mode(state,prim->tpage,0x09FFu);
    /* CLUT fetch is part of packet dispatch, even if clipping later rejects
     * every pixel. It must finish before a primitive draws over its palette. */
    if (polygon_page || prim->kind==PSX_PRIM_SPRT)
        cache_palette(&state->cache,vram,prim->clut,state->mode);
    resolved = *prim;
    resolved.tpage = state->mode;
    local = *opts;
    local.cache=&state->cache;
    local.dither = opts->dither && (state->mode & 0x0200u) != 0;
    local.texture_window = state->texture_window;
    local.mask_mode = state->mask_mode;
    local.missing_texture_bank = (state->mode & 0x0800u) != 0;
    local.skip_field = (state->display_mode & 0x24u) == 0x24u &&
                      !(state->mode & 0x0400u);
    local.field = (u8)((state->display_y + state->field) & 1);
    psx_raster_prim(fb, &resolved, vram, &local);
}

typedef struct walk_ctx {
    psx_framebuffer       *fb;
    const psx_vram        *vram;
    const psx_raster_opts *opts;
    psx_draw_state        state;
} walk_ctx;

static void walk_visit(const psx_prim *prim, void *user)
{
    walk_ctx *ctx = (walk_ctx *)user;
    psx_raster_command(ctx->fb, prim, ctx->vram, ctx->opts, &ctx->state);
}

void psx_raster_ot(psx_framebuffer *fb,
                   psx_ot *ot,
                   const psx_vram *vram,
                   const psx_raster_opts *opts)
{
    walk_ctx ctx = {0};

    ctx.fb   = fb;
    ctx.vram = vram;
    ctx.opts = opts;
    ctx.state.mode = opts->dither ? 0x0200u : 0;
    ctx.state.texture_window = opts->texture_window;
    ctx.state.mask_mode = opts->mask_mode;

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
