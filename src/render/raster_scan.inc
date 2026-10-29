/* Fixed-point scan conversion adapted from Mednafen's PS1 gpu_polygon.cpp
 * and gpu_line.cpp, Copyright (C) 2011-2017 Mednafen Team.
 * https://github.com/libretro-mirrors/mednafen-git/tree/master/src/psx
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2, or (at your option) any later version.
 * It is distributed WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See LICENSE.
 *
 * Edges have 32 fractional bits with directed division and a subpixel bias.
 * UV/RGB gradients are truncated to 12 fractional bits. Unsigned 8.24
 * accumulators make wrapping explicit; the low 12 padding bits stay zero.
 * Attribute origin and scan direction depend on the ORIGINAL leftmost
 * vertex, including tie order. Winding normalization would lose that detail.
 */
typedef struct raster_attr {
    u32 base[5], dx[5], dy[5];  /* r, g, b, u, v */
} raster_attr;

static s64 edge_start(int x)
{
    return (s64)x * 4294967296LL + 4294965248LL;
}

static s64 edge_step(int dx, int dy)
{
    s64 n = (s64)dx * 4294967296LL;
    if (n < 0) n -= dy - 1;
    if (n > 0) n += dy - 1;
    return n / dy;
}

static void raster_span(psx_framebuffer *fb, const psx_prim *prim,
                         const psx_vram *vram, const psx_raster_opts *opts,
                         const raster_attr *a, int y, int left, int right,
                         bool textured, bool dither)
{
    int cx0, cy0, cx1, cy1, x, i, attr_x=left, attr_y=y;
    u32 cur[5];
    raster_clip(fb, opts, &cx0, &cy0, &cx1, &cy1);
    if (opts->native_coords) {
        int width=right-left;
        left=signed11(left+fb->vram_x)-fb->vram_x;
        right=left+width;
        y=signed11(y+fb->vram_y)-fb->vram_y;
    }
    if (y < cy0 || y >= cy1) return;
    if (left < cx0) { attr_x+=cx0-left; left = cx0; }
    if (right > cx1) right = cx1;
    for (i = 0; i < 5; ++i)
        cur[i] = a->base[i] + a->dx[i] * (u32)attr_x + a->dy[i] * (u32)attr_y;

    for (x = left; x < right; ++x) {
        int r = cur[0] >> 24, g = cur[1] >> 24, b = cur[2] >> 24;
        bool stp = false;
        if (textured) {
            int tr, tg, tb;
            if (!sample_texture(vram, opts->cache, prim->tpage, prim->clut,
                                opts->texture_window,
                                (u8)(cur[3] >> 24), (u8)(cur[4] >> 24),
                                &tr, &tg, &tb, &stp)) goto next_pixel;
            if (prim->textured_blend) {
                r = (tr * r) >> 7; g = (tg * g) >> 7; b = (tb * b) >> 7;
            } else { r = tr; g = tg; b = tb; }
        }
        /* Dither precedes saturation, including modulated values above 255. */
        if (dither) {
            int d = psx_dither_matrix[(y+fb->vram_y) & 3][(x+fb->vram_x) & 3];
            r += d; g += d; b += d;
        }
        r = q2_clamp_s32(r, 0, 255);
        g = q2_clamp_s32(g, 0, 255);
        b = q2_clamp_s32(b, 0, 255);
        write_pixel(fb, opts, prim, y * fb->width + x, r, g, b,
                    opts->semi_transparency && prim->semi_transparent &&
                    (!textured || stp), stp);
next_pixel:
        for (i = 0; i < 5; ++i) cur[i] += a->dx[i];
    }
}

static void raster_triangle(psx_framebuffer *fb,
                            const raster_vertex *v0,
                            const raster_vertex *v1,
                            const raster_vertex *v2,
                            const psx_prim *prim,
                            const psx_vram *vram,
                            const psx_raster_opts *opts)
{
    const raster_vertex *v[3] = {v0,v1,v2}, *core, *tmp;
    raster_attr attr = {0};
    s64 base, slope, upper = 0, lower = 0;
    int den, cv, i, j, vo, vp;
    bool right, textured, gouraud, dither;
    struct {
        s64 x[2], step[2];
        int y, end;
        bool up;
    } part[2];

    /* Hardware rejects each triangle separately, including halves of a quad. */
    for (i=0;i<3;++i) for (j=i+1;j<3;++j)
        if (abs(v[i]->x-v[j]->x)>PSX_PRIM_MAX_SPAN_X ||
            abs(v[i]->y-v[j]->y)>PSX_PRIM_MAX_SPAN_Y) return;

    if (v[1]->x <= v[0]->x) core = v[2]->x <= v[1]->x ? v[2] : v[1];
    else core = v[2]->x < v[0]->x ? v[2] : v[0];
    if (v[2]->y < v[1]->y) { tmp=v[2]; v[2]=v[1]; v[1]=tmp; }
    if (v[1]->y < v[0]->y) { tmp=v[1]; v[1]=v[0]; v[0]=tmp; }
    if (v[2]->y < v[1]->y) { tmp=v[2]; v[2]=v[1]; v[1]=tmp; }
    cv = core == v[0] ? 0 : core == v[1] ? 1 : 2;
    den = (v[1]->x-v[0]->x)*(v[2]->y-v[1]->y) -
          (v[2]->x-v[1]->x)*(v[1]->y-v[0]->y);
    if (!den) return;
    textured = opts->textures && vram &&
        (prim->kind == PSX_PRIM_FT3 || prim->kind == PSX_PRIM_GT3 ||
         prim->kind == PSX_PRIM_FT4 || prim->kind == PSX_PRIM_GT4);
    gouraud = prim->kind == PSX_PRIM_G3 || prim->kind == PSX_PRIM_G4 ||
              prim->kind == PSX_PRIM_GT3 || prim->kind == PSX_PRIM_GT4;
    if (textured && opts->missing_texture_bank) return;
    dither = opts->dither && (textured ? prim->textured_blend : gouraud);

    for (i=0;i<5;++i) {
        int av[3];
        for (j=0;j<3;++j) {
            switch (i) {
            case 0: av[j]=v[j]->r; break;
            case 1: av[j]=v[j]->g; break;
            case 2: av[j]=v[j]->b; break;
            case 3: av[j]=v[j]->u; break;
            default:av[j]=v[j]->v; break;
            }
        }
        if (i<3 ? gouraud : textured) {
            s64 nx=(av[1]-av[0])*(v[2]->y-v[1]->y)-
                   (av[2]-av[1])*(v[1]->y-v[0]->y);
            s64 ny=(v[1]->x-v[0]->x)*(av[2]-av[1])-
                   (v[2]->x-v[1]->x)*(av[1]-av[0]);
            attr.dx[i]=(u32)(nx*4096/den)<<12;
            attr.dy[i]=(u32)(ny*4096/den)<<12;
        }
        attr.base[i]=((u32)av[cv]<<24)+0x800000u -
                     attr.dx[i]*(u32)core->x-attr.dy[i]*(u32)core->y;
    }

    base=edge_start(v[0]->x);
    slope=edge_step(v[2]->x-v[0]->x,v[2]->y-v[0]->y);
    if (v[1]->y==v[0]->y) right=v[1]->x>v[0]->x;
    else {
        upper=edge_step(v[1]->x-v[0]->x,v[1]->y-v[0]->y);
        right=upper>slope;
    }
    if (v[2]->y!=v[1]->y)
        lower=edge_step(v[2]->x-v[1]->x,v[2]->y-v[1]->y);
    vo = cv ? 1 : 0;
    vp = cv == 2 ? 3 : 0;
    part[vo].y=v[vo]->y;
    part[vo].end=v[1^vo]->y;
    part[vo].x[right]=edge_start(v[vo]->x);
    part[vo].step[right]=upper;
    part[vo].x[!right]=base+(v[vo]->y-v[0]->y)*slope;
    part[vo].step[!right]=slope;
    part[vo].up=vo!=0;
    part[vo^1].y=v[1^vp]->y;
    part[vo^1].end=v[2^vp]->y;
    part[vo^1].x[right]=edge_start(v[1^vp]->x);
    part[vo^1].step[right]=lower;
    part[vo^1].x[!right]=base+(v[1^vp]->y-v[0]->y)*slope;
    part[vo^1].step[!right]=slope;
    part[vo^1].up=vp!=0;
    for (i=0;i<2;++i) {
        int y=part[i].y;
        s64 left=part[i].x[0], right_x=part[i].x[1];
        while (y!=part[i].end) {
            if (part[i].up) {
                --y; left-=part[i].step[0]; right_x-=part[i].step[1];
            }
            raster_span(fb,prim,vram,opts,&attr,y,(int)(left>>32),
                        (int)(right_x>>32),textured,dither);
            if (!part[i].up) {
                ++y; left+=part[i].step[0]; right_x+=part[i].step[1];
            }
        }
    }
}

static void raster_line(psx_framebuffer *fb, const psx_prim *prim,
                        const psx_raster_opts *opts)
{
    int first=0, last=1, k, i, c, x0, y0, x1, y1;
    int cx0, cy0, cx1, cy1;
    s64 x,y,dx=0,dy=0;
    u32 rgb[3], step[3]={0};
    bool gouraud=prim->kind==PSX_PRIM_LINE_G2;
    x0=prim->xy[0].x+opts->ofs_x; y0=prim->xy[0].y+opts->ofs_y;
    x1=prim->xy[1].x+opts->ofs_x; y1=prim->xy[1].y+opts->ofs_y;
    if (abs(x1-x0)>PSX_PRIM_MAX_SPAN_X || abs(y1-y0)>PSX_PRIM_MAX_SPAN_Y) return;
    k=abs(x1-x0)>abs(y1-y0)?abs(x1-x0):abs(y1-y0);
    if (x0>=x1 && k) {
        int t=x0; x0=x1; x1=t; t=y0; y0=y1; y1=t;
        first=1; last=0;
    }
    if (k) { dx=edge_step(x1-x0,k); dy=edge_step(y1-y0,k); }
    x=(s64)x0*4294967296LL+2147483648LL-1024;
    y=(s64)y0*4294967296LL+2147483648LL-(dy<0?1024:0);
    for (c=0;c<3;++c) {
        int a=c==0?prim->rgb[first].r:c==1?prim->rgb[first].g:prim->rgb[first].b;
        int b=c==0?prim->rgb[last].r:c==1?prim->rgb[last].g:prim->rgb[last].b;
        if (!gouraud) a=c==0?prim->rgb[0].r:c==1?prim->rgb[0].g:prim->rgb[0].b;
        rgb[c]=((u32)a<<12)+2048;
        if (gouraud && k) step[c]=(u32)((b-a)*4096/k);
    }
    raster_clip(fb,opts,&cx0,&cy0,&cx1,&cy1);
    for (i=0;i<=k;++i) {
        int px=(int)(x>>32), py=(int)(y>>32), colour[3];
        if (opts->native_coords) {
            px=signed11(px+fb->vram_x)-fb->vram_x;
            py=signed11(py+fb->vram_y)-fb->vram_y;
        }
        if (px>=cx0 && px<cx1 && py>=cy0 && py<cy1) {
            for (c=0;c<3;++c) {
                int value=(u8)(rgb[c]>>12);
                if (opts->dither)
                    value+=psx_dither_matrix[(py+fb->vram_y)&3][(px+fb->vram_x)&3];
                colour[c]=q2_clamp_s32(value,0,255);
            }
            write_pixel(fb,opts,prim,py*fb->width+px,colour[0],colour[1],colour[2],
                        opts->semi_transparency&&prim->semi_transparent,false);
        }
        x+=dx; y+=dy;
        for (c=0;c<3;++c) rgb[c]+=step[c];
    }
}
