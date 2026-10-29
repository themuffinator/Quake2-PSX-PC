#include "raster.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(test) do { if (!(test)) { printf("FAIL line %d: %s\n",__LINE__,#test); ++failures; } } while(0)

static void triangle_halves(void)
{
    psx_framebuffer fb;
    psx_raster_opts o;
    psx_prim p={0};
    psx_fb_init(&fb,16,16); psx_raster_opts_default(&o);
    p.kind=PSX_PRIM_F4;p.quad_zorder=true;p.rgb[0].r=248;
    p.xy[0]=(psx_xy){0,0};p.xy[1]=(psx_xy){8,0};
    p.xy[2]=(psx_xy){0,8};p.xy[3]=(psx_xy){1100,8};
    psx_raster_prim(&fb,&p,NULL,&o);
    CHECK(fb.px[2*16+2]==31);CHECK(fb.px[7*16+7]==0);
    psx_fb_clear(&fb,0);
    p.xy[0].x=1100;p.xy[3].x=8;
    psx_raster_prim(&fb,&p,NULL,&o);
    CHECK(fb.px[2*16+2]==0);CHECK(fb.px[7*16+7]==31);

    /* A logical host position 2052 is distinct from native packet x=4. */
    psx_fb_clear(&fb,0); p.kind=PSX_PRIM_F3;
    p.xy[0]=(psx_xy){2052,4};p.xy[1]=(psx_xy){2060,4};p.xy[2]=(psx_xy){2052,12};
    psx_raster_prim(&fb,&p,NULL,&o);CHECK(fb.px[5*16+5]==0);
    o.native_coords=true;
    psx_raster_prim(&fb,&p,NULL,&o);CHECK(fb.px[5*16+5]==31);
    /* Wrapping after draw offset must preserve the unwrapped attribute origin. */
    psx_fb_clear(&fb,0);
    p.xy[0]=(psx_xy){-1020,4};p.xy[1]=(psx_xy){-1012,4};p.xy[2]=(psx_xy){-1020,12};
    o.ofs_x=-1024;
    psx_raster_prim(&fb,&p,NULL,&o);CHECK(fb.px[5*16+5]==31);
    psx_fb_free(&fb);
}

static void native_vram(void)
{
    psx_framebuffer fb;
    psx_vram *v=calloc(1,sizeof(*v));
    psx_raster_opts o;
    psx_prim p={0};
    int i;
    psx_fb_init(&fb,32,32);psx_raster_opts_default(&o);
    CHECK(psx_fb_bind_vram(&fb,v,512,0)==Q2_OK);
    p.kind=PSX_PRIM_TILE;p.xy[2]=(psx_xy){2,2};p.rgb[0].r=248;
    psx_raster_prim(&fb,&p,v,&o);CHECK(v->px[0][512]==31);
    p.kind=PSX_PRIM_SPRT;p.xy[0]=(psx_xy){4,0};p.xy[2]=(psx_xy){6,2};
    p.tpage=psx_make_tpage(8,0,PSX_BLEND_HALF,PSX_TEX_16BIT);
    psx_raster_prim(&fb,&p,v,&o);CHECK(fb.px[4]==31);CHECK(v->px[0][516]==31);
    p.kind=PSX_PRIM_MOVE;p.xy[0]=(psx_xy){4,0};
    p.xy[1]=(psx_xy){-512,0};p.xy[2]=(psx_xy){2,2};
    psx_raster_prim(&fb,&p,v,&o);CHECK(v->px[0][0]==31);
    CHECK(psx_fb_bind_vram(&fb,v,1000,0)==Q2_ERR_INVALID_ARG);
    psx_fb_free(&fb);

    psx_fb_init(&fb,1024,512);psx_fb_bind_vram(&fb,v,0,0);
    for(i=0;i<140;++i) psx_fb_store(&fb,i,0,(u16)(i+1));
    p.xy[0]=(psx_xy){0,0};p.xy[1]=(psx_xy){1,0};p.xy[2]=(psx_xy){129,1};
    psx_raster_prim(&fb,&p,v,&o);
    CHECK(v->px[0][128]==128);CHECK(v->px[0][129]==128);
    psx_fb_store(&fb,1023,511,0x1234);psx_fb_store(&fb,0,511,0x4321);
    p.xy[0]=(psx_xy){1023,511};p.xy[1]=(psx_xy){1023,512};p.xy[2]=(psx_xy){2,1};
    psx_raster_prim(&fb,&p,v,&o);
    CHECK(v->px[0][1023]==0x1234);CHECK(v->px[0][0]==0x4321);
    o.mask_mode=3;psx_fb_store(&fb,1023,0,0x8001);
    psx_raster_prim(&fb,&p,v,&o);
    CHECK(v->px[0][1023]==0x8001);CHECK(v->px[0][0]==0xc321);
    /* Encoded zero width/height transfers the full VRAM extent. */
    o.mask_mode=0;p.xy[0]=(psx_xy){0,0};p.xy[1]=(psx_xy){0,0};p.xy[2]=(psx_xy){0,0};
    psx_raster_prim(&fb,&p,v,&o);CHECK(v->px[511][1023]==0x1234);
    psx_fb_free(&fb);free(v);
}

static void fields(void)
{
    psx_framebuffer fb;
    psx_draw_state s={0};
    psx_raster_opts o;
    psx_prim p={0},mode={0};
    psx_fb_init(&fb,8,8);psx_raster_opts_default(&o);
    p.kind=PSX_PRIM_TILE;p.xy[2]=(psx_xy){8,8};p.rgb[0].r=248;
    psx_draw_state_gp1(&s,0x08000024u);
    psx_raster_command(&fb,&p,NULL,&o,&s);
    CHECK(fb.px[0]==0);CHECK(fb.px[8]==31);
    s.field=1;p.rgb[0].g=248;
    psx_raster_command(&fb,&p,NULL,&o,&s);
    CHECK(fb.px[0]==0x3ff);CHECK(fb.px[8]==31);
    mode.kind=PSX_PRIM_TPAGE;mode.tpage=0x400;
    psx_raster_command(&fb,&mode,NULL,&o,&s);
    psx_raster_command(&fb,&p,NULL,&o,&s);CHECK(fb.px[8]==0x3ff);
    mode.tpage=0;psx_raster_command(&fb,&mode,NULL,&o,&s);
    psx_draw_state_gp1(&s,0x08000000u);p.rgb[0].g=0;
    psx_raster_command(&fb,&p,NULL,&o,&s);
    CHECK(fb.px[0]==31);CHECK(fb.px[8]==31);
    psx_fb_free(&fb);
}

static void vram_dither_origin(void)
{
    psx_framebuffer fb;
    psx_vram *v=calloc(1,sizeof(*v));
    psx_raster_opts o;
    psx_prim p={0};
    psx_fb_init(&fb,8,8);psx_raster_opts_default(&o);
    psx_fb_bind_vram(&fb,v,1,1);
    p.kind=PSX_PRIM_G3;
    p.xy[1]=(psx_xy){4,0};p.xy[2]=(psx_xy){0,4};
    p.rgb[0].r=p.rgb[1].r=p.rgb[2].r=10;
    psx_raster_prim(&fb,&p,NULL,&o);
    CHECK(fb.px[0]==1); /* VRAM (1,1): -2; host (0,0) would be -4. */
    CHECK(fb.px[1]==1); /* VRAM (2,1): +3 */
    psx_fb_clear(&fb,0);p.kind=PSX_PRIM_LINE_G2;
    p.rgb[0].r=p.rgb[1].r=10;
    psx_raster_prim(&fb,&p,NULL,&o);
    CHECK(fb.px[0]==1);
    psx_fb_free(&fb);free(v);
}

int main(void)
{
    triangle_halves();native_vram();fields();vram_dither_origin();
    printf("Raster edge cases: %d failures\n",failures);
    return failures?1:0;
}
