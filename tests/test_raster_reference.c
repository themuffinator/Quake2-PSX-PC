/* Packet sequences adapted from Jakub Czekanski's MIT-licensed ps1-tests.
 * See fixtures/ps1-tests-LICENSE.txt and fixtures/README.md. Expected row
 * hashes come from the upstream VRAM images, never from this renderer. */
#include "raster.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fixtures/gpu_reference.h"

static psx_framebuffer fb;
static psx_vram vram;
static psx_raster_opts opts;
static int failures;

static void compare(const char *name, int x, int y, int w, int h,
                    const u64 *expected)
{
    int row, col, bad = 0;
    for (row = 0; row < h; ++row) {
        u64 hash = 14695981039346656037ULL;
        for (col = 0; col < w; ++col) {
            u16 p = fb.px[(y + row) * fb.width + x + col] & 0x7fff;
            hash = (hash ^ (p & 255)) * 1099511628211ULL;
            hash = (hash ^ (p >> 8)) * 1099511628211ULL;
        }
        if (hash != expected[row]) ++bad;
    }
    printf("%s: %d/%d rows match reference\n", name, h-bad, h);
    if (bad) ++failures;
}

static void triangle(int cx, int cy, int size)
{
    float h = size * 0.866025f;
    psx_prim p = {0};
    p.kind = PSX_PRIM_G3;
    p.xy[0] = (psx_xy){(s16)(cx-size/2), (s16)(cy+h/2)};
    p.xy[1] = (psx_xy){(s16)(cx+size/2), (s16)(cy+h/2)};
    p.xy[2] = (psx_xy){(s16)cx, (s16)(cy-h/2)};
    p.rgb[0].r = p.rgb[1].g = p.rgb[2].b = 255;
    psx_raster_prim(&fb, &p, &vram, &opts);
}

static void quad_rows(int x, int y, bool textured)
{
    int i;
    psx_prim p = {0};
    p.kind = textured ? PSX_PRIM_FT4 : PSX_PRIM_G4;
    p.quad_zorder = true;
    p.textured_blend = true;
    p.tpage = psx_make_tpage(8, 0, PSX_BLEND_HALF, PSX_TEX_16BIT);
    p.uv[1].u = p.uv[3].u = 1;
    if (textured) p.rgb[0] = (psx_rgb){128,128,128,0};
    else {
        p.rgb[0].r = p.rgb[2].r = 255;
        p.rgb[1].g = p.rgb[3].g = 255;
    }
    for (i = 0; i < 256; ++i) {
        p.xy[0] = (psx_xy){(s16)x, (s16)(y+i)};
        p.xy[1] = (psx_xy){(s16)(x+i), (s16)(y+i)};
        p.xy[2] = (psx_xy){(s16)x, (s16)(y+i+1)};
        p.xy[3] = (psx_xy){(s16)(x+i), (s16)(y+i+1)};
        psx_raster_prim(&fb, &p, &vram, &opts);
    }
}

static void line(int x0, int y0, int x1, int y1, bool gouraud, int red)
{
    psx_prim p = {0};
    p.kind = gouraud ? PSX_PRIM_LINE_G2 : PSX_PRIM_LINE_F2;
    p.xy[0] = (psx_xy){(s16)x0,(s16)y0};
    p.xy[1] = (psx_xy){(s16)x1,(s16)y1};
    p.rgb[0].r = (u8)red;
    p.rgb[1].r = 255;
    psx_raster_prim(&fb, &p, &vram, &opts);
}

static void copies(void)
{
    const int sizes[7]={2,8,15,16,16,16,16};
    int t,i,x,y,ox,oy,bad=0;
    psx_prim p={0};
    memset(&vram,0,sizeof(vram));
    for(t=0;t<7;++t) for(i=1;i<=21;++i)
        for(y=0;y<sizes[t];++y) for(x=0;x<sizes[t];++x)
            vram.px[t*42+4+y][i*42+4+x]=(u16)(y*64+x*2);
    psx_fb_bind_vram(&fb,&vram,0,0);
    p.kind=PSX_PRIM_MOVE;
    for(t=0;t<7;++t) {
        i=1;
        for(oy=-1;oy<=1;++oy) for(ox=-3;ox<=3;++ox,++i) {
            u64 hash=14695981039346656037ULL;
            p.xy[0]=(psx_xy){(s16)(i*42+4+(t==4||t==6)),
                             (s16)(t*42+4+(t==5||t==6))};
            p.xy[1]=(psx_xy){(s16)(p.xy[0].x+ox),(s16)(p.xy[0].y+oy)};
            p.xy[2]=(psx_xy){(s16)sizes[t],(s16)sizes[t]};
            psx_raster_prim(&fb,&p,&vram,&opts);
            for(y=t*42+1;y<t*42+27;++y) for(x=i*42+1;x<i*42+41;++x) {
                u16 pixel=fb.px[y*1024+x]&0x7fff;
                hash=(hash^(pixel&255))*1099511628211ULL;
                hash=(hash^(pixel>>8))*1099511628211ULL;
            }
            if(hash!=move_hashes[t*21+i-1]) ++bad;
        }
    }
    printf("VRAM overlapping transfers: %d/147 match reference\n",147-bad);
    if(bad) ++failures;
    psx_fb_bind_vram(&fb,NULL,0,0);
}

int main(int argc, char **argv)
{
    int i, d;
    if (psx_fb_init(&fb, 1024, 512) != Q2_OK) return 1;
    psx_raster_opts_default(&opts);
    opts.native_coords = true;
    psx_fb_clear(&fb, 0x7fff);
    opts.dither = false;
    triangle(160,120,240);
    triangle(768,256,500);
    opts.dither = true;
    triangle(160,360,240);
    compare("Gouraud triangles",0,0,1024,512,triangle_rows);
    if (argc > 1) psx_fb_write_ppm(&fb,argv[1]);

    psx_fb_clear(&fb,0);
    vram.px[0][512] = fb.px[512] = 0x001f;
    vram.px[0][513] = fb.px[513] = 0x03e0;
    opts.dither = false;
    quad_rows(0,0,true);
    quad_rows(0,256,false);
    opts.dither = true;
    quad_rows(256,256,false);
    compare("UV/colour gradients, widths 0..255",0,0,1024,512,uv_rows);
    if (argc > 2) psx_fb_write_ppm(&fb,argv[2]);

    psx_fb_clear(&fb,0x7fff);
    opts.dither = false;
    for (i=0;i<10;++i) {
        line(16,16+i*8,96,16+i*8+i,false,0);
        line(110+i*8,16,110+i*8+i,96,false,0);
    }
    for (i=0;i<20;++i) line(200,16+i*4,200+i,17+i*4,false,0);
    for (d=0;d<2;++d) {
        opts.dither = d != 0;
        for (i=0;i<64;++i) {
            line(16+68*d,100+i,16+68*d+i,100+i,false,170);
            line(16+68*d,166+i,16+68*d+i,166+i,true,0);
        }
    }
    compare("Shallow/steep lines",0,0,320,100,line_coverage_rows);
    compare("Flat/Gouraud lines with/without dither",0,100,148,132,line_colour_rows);
    copies();
    psx_fb_free(&fb);
    return failures ? 1 : 0;
}
