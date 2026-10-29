/* CLUT replay adapted from Jakub Czekanski's MIT-licensed ps1-tests.
 * Expected pixels are upstream's published VRAM capture, not this renderer.
 * See fixtures/README.md and ps1-tests-LICENSE.txt. */
#include "raster.h"
#include <stdio.h>
#include <string.h>
#include "fixtures/gpu_reference.h"

static psx_vram vram;
static psx_framebuffer fb;
static psx_draw_state gpu;
static psx_raster_opts opts;
static int failures;
static bool uncached;
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n",__LINE__,#x); ++failures; } } while (0)

static void command(psx_prim *p)
{
    if (uncached) {
        psx_prim resolved=*p;
        if (p->kind==PSX_PRIM_TPAGE) gpu.mode=p->tpage;
        resolved.tpage=gpu.mode;
        psx_raster_prim(&fb,&resolved,&vram,&opts);
    } else psx_raster_command(&fb,p,&vram,&opts,&gpu);
}
static void mode(int depth, int page)
{
    psx_prim p={0};p.kind=PSX_PRIM_TPAGE;p.tpage=(u16)((depth<<7)|page);
    command(&p);
}
static void clear(void)
{
    psx_prim p={0};p.kind=PSX_PRIM_CACHE_CLEAR;command(&p);
}
static void sprite(int x,int y,int u,int v,int cx,int cy,int w)
{
    psx_prim p={0};p.kind=PSX_PRIM_SPRT;p.textured_blend=true;
    p.rgb[0]=(psx_rgb){128,128,128,0};p.xy[0]=(psx_xy){(s16)x,(s16)y};
    p.xy[2]=(psx_xy){(s16)(x+w),(s16)(y+1)};p.uv[0]=(psx_uv){(u8)u,(u8)v};
    p.clut=psx_make_clut(cx,cy);command(&p);
}
static void palette(int x,int y)
{
    int i;psx_vram_transfer(&vram);
    for(i=0;i<256;++i)psx_fb_store(&fb,(x+i)&1023,y,(u16)i);
}
static void white(int y, bool line)
{
    psx_prim p={0};p.kind=line?PSX_PRIM_LINE_F2:PSX_PRIM_TILE;
    p.rgb[0]=(psx_rgb){255,255,255,0};p.xy[0]=(psx_xy){0,(s16)y};
    p.xy[1]=(psx_xy){256,(s16)y};p.xy[2]=(psx_xy){256,(s16)(y+1)};command(&p);
}
static void reset(void)
{
    memset(&gpu,0,sizeof(gpu));memset(&vram,0,sizeof(vram));
    psx_raster_opts_default(&opts);opts.dither=false;
    psx_fb_bind_vram(&fb,&vram,0,0);
}
static void reference(void)
{
    int y,i,bad=0;
    reset();mode(1,0);palette(0,20);clear();
    for(y=1;y<=3;++y) {
        psx_vram_transfer(&vram);
        for(i=0;i<256;i+=2) {
            u16 a=(u16)(y==1?i:y==2?255-i:i*(i<<3));
            u16 b=(u16)(y==1?i+1:y==2?254-i:(i+1)*((i+1)<<3));
            psx_fb_store(&fb,i/2,y,(u16)(a|(b<<8)));
        }
        palette(0,20+y*16);clear();sprite(0,20+y*16,0,y,0,20+y*16,256);
    }
    for(y=84;y<=196;y+=16) {
        palette(0,y);
        if(y==132||y==196)mode(0,0);
        if(y==148)mode(1,0);
        if(y==164)mode(2,0);
        if(y==180)mode(3,0);
        sprite(0,y+2,0,1,0,y,256);
        white(y,y==100);
        if(y==100)clear();
        if(y==132||y==180||y==196)mode(1,0);
        if(y==148||y==164||y==196)mode(0,0);
        sprite(0,y+4,0,1,y==116?16:0,y,256);
    }
    palette(960,244);mode(1,0);sprite(0,246,0,1,960,244,256);
    for(y=0;y<256;++y) {
        u64 h=14695981039346656037ULL;
        for(i=0;i<1024;++i) {
            u16 p=fb.px[y*1024+i]&0x7fff;
            h=(h^(p&255))*1099511628211ULL;h=(h^(p>>8))*1099511628211ULL;
        }
        if(h!=clut_rows[y]) { printf("CLUT reference mismatch row %d\n",y);++bad; }
    }
    printf("CLUT reference: %d/256 rows match\n",256-bad);
    if(bad && !uncached)++failures;
}
static void visibility(void)
{
    psx_prim p={0};
    reset();mode(2,0);
    psx_fb_store(&fb,0,1,31);psx_fb_store(&fb,1,1,32);
    sprite(500,100,0,1,0,0,1);CHECK(fb.px[100*1024+500]==31);
    /* A miss fetches ALL FOUR words, not only the addressed texel. */
    psx_fb_store(&fb,1,1,992);sprite(501,100,1,1,0,0,1);
    CHECK(fb.px[100*1024+501]==32);
    mode(2,0x20);sprite(502,100,1,1,0,0,1); /* ABR doesn't flush */
    CHECK(fb.px[100*1024+502]==32);
    clear();sprite(503,100,1,1,0,0,1);CHECK(fb.px[100*1024+503]==992);
    psx_fb_store(&fb,1,1,1024);psx_vram_transfer(&vram);
    sprite(504,100,1,1,0,0,1);CHECK(fb.px[100*1024+504]==1024);
    /* Direct mapped eviction: x=32 aliases x=0 at 16bpp. */
    psx_fb_store(&fb,1,1,77);sprite(505,100,32,1,0,0,1);
    sprite(506,100,1,1,0,0,1);CHECK(fb.px[100*1024+506]==77);
    psx_fb_store(&fb,1,1,88);mode(2,1);mode(2,0);
    sprite(507,100,1,1,0,0,1);CHECK(fb.px[100*1024+507]==88);
    psx_fb_store(&fb,1,1,99);mode(1,0);mode(2,0);
    sprite(508,100,1,1,0,0,1);CHECK(fb.px[100*1024+508]==88);
    mode(0,0);mode(2,0);sprite(509,100,1,1,0,0,1);
    CHECK(fb.px[100*1024+509]==99);
    /* Raw MOVE flushes texture words, but NOT the palette. */
    reset();mode(0,0);psx_fb_store(&fb,0,1,0x1111);
    psx_fb_store(&fb,1,10,31);sprite(500,100,0,1,0,10,1);
    psx_fb_store(&fb,1,10,992);psx_vram_transfer(&vram);
    sprite(501,100,0,1,0,10,1);CHECK(fb.px[100*1024+501]==31);
    p.kind=PSX_PRIM_MOVE;p.xy[0]=(psx_xy){0,1};p.xy[1]=(psx_xy){8,1};
    p.xy[2]=(psx_xy){1,1};command(&p);
    sprite(502,100,0,1,0,10,1);CHECK(fb.px[100*1024+502]==31);
    p.clear_cache=true;command(&p); /* retail water's explicit GP0(01) */
    sprite(503,100,0,1,0,10,1);CHECK(fb.px[100*1024+503]==992);
    /* Clipped primitive still loads CLUT; it cannot see the later overwrite. */
    psx_fb_store(&fb,1,11,55);opts.clip_w=-1;sprite(0,0,0,1,0,11,1);
    opts.clip_w=0;psx_fb_store(&fb,1,11,66);sprite(504,100,0,1,0,11,1);
    CHECK(fb.px[100*1024+504]==55);
    psx_draw_state_gp1(&gpu,0);mode(0,0);sprite(505,100,0,1,0,11,1);
    CHECK(fb.px[100*1024+505]==66);

    /* Packed cache layouts: 64x32 texels at 8bpp, 64x64 at 4bpp. Each
     * conflict forces a reload after an otherwise invisible VRAM write. */
    reset();mode(1,0);psx_fb_store(&fb,0,1,0x0101);
    psx_fb_store(&fb,1,300,31);psx_fb_store(&fb,2,300,992);
    sprite(500,100,0,1,0,300,1);psx_fb_store(&fb,0,1,0x0202);
    sprite(501,100,0,1,0,300,1);CHECK(fb.px[100*1024+501]==31);
    sprite(502,100,0,33,0,300,1);sprite(503,100,0,1,0,300,1);
    CHECK(fb.px[100*1024+503]==992);
    psx_fb_store(&fb,0,1,0x0101);sprite(504,100,64,1,0,300,1);
    sprite(505,100,0,1,0,300,1);CHECK(fb.px[100*1024+505]==31);
    mode(0,0);psx_fb_store(&fb,0,1,0x1111);sprite(506,100,0,1,0,300,1);
    psx_fb_store(&fb,0,1,0x2222);sprite(507,100,0,33,0,300,1);
    sprite(508,100,0,1,0,300,1);CHECK(fb.px[100*1024+508]==31);
    sprite(509,100,0,65,0,300,1);sprite(510,100,0,1,0,300,1);
    CHECK(fb.px[100*1024+510]==992);

    /* E2 changes addresses, not cache contents. E1 bank selection does flush. */
    psx_fb_store(&fb,0,1,0x1111);
    p=(psx_prim){0};p.kind=PSX_PRIM_TWINDOW;p.texture_window=1|(1<<10);command(&p);
    p.texture_window=0;command(&p);sprite(511,100,0,1,0,300,1);
    CHECK(fb.px[100*1024+511]==992);
    psx_draw_state_gp1(&gpu,0x09000001);mode(0,0x800);mode(0,0);
    sprite(512,100,0,1,0,300,1);CHECK(fb.px[100*1024+512]==31);
    psx_fb_store(&fb,1,300,77);
    psx_draw_state_gp1(&gpu,0x01000000);sprite(513,100,0,1,0,300,1);
    CHECK(fb.px[100*1024+513]==77); /* command-buffer reset invalidates CLUT */
    psx_draw_state_gp1(&gpu,0);CHECK(gpu.allow_texture_bank);
}
int main(int argc,char **argv)
{
    if(psx_fb_init(&fb,1024,512)!=Q2_OK)return 2;
    uncached=argc>2 && strcmp(argv[2],"uncached")==0;
    reference();
    if(argc>1)CHECK(psx_fb_write_ppm(&fb,argv[1])==Q2_OK);
    if(!uncached)visibility();
    psx_fb_free(&fb);printf("Texture cache: %d failures\n",failures);
    return failures?1:0;
}
