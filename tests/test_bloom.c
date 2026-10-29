/* Scene separation and GPU bloom qualification. Hidden GL, engine FBO only. */
#include "crt_gl.h"
#include "screen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); failures++; } } while (0)

static void layers(void)
{
    psx_framebuffer fb = {0}, scene = {0}, reference = {0};
    psx_raster_opts opts, plain;
    psx_prim p = {0};
    psx_vram *vram = calloc(1, sizeof(*vram));
    u8 mask[32*16] = {0};
    int at = 4*32+4, i;
    CHECK(psx_fb_init(&fb,32,16) == Q2_OK);
    CHECK(psx_fb_init(&scene,32,16) == Q2_OK);
    CHECK(psx_fb_init(&reference,32,16) == Q2_OK);
    if (!vram || !fb.px || !scene.px || !reference.px) exit(2);
    psx_fb_clear(&fb,0x2108); psx_fb_clear(&scene,0x2108); psx_fb_clear(&reference,0x2108);
    psx_raster_opts_default(&opts); opts.dither = false;
    plain = opts; opts.scene = scene.px; opts.ui_mask = mask;
    p.kind = PSX_PRIM_TILE; p.ui = true;
    p.xy[0].x = p.xy[0].y = 4; p.xy[2].x = p.xy[2].y = 8;
    p.rgb[0].r = p.rgb[0].g = p.rgb[0].b = 255;
    psx_raster_prim(&fb,&p,NULL,&opts); psx_raster_prim(&reference,&p,NULL,&plain);
    CHECK(fb.px[at] == 0x7fff && scene.px[at] == 0x2108 && mask[at] == 255);
    /* An additive scene flash after the HUD must not copy HUD colour into the
     * scene, and must leave the covered UI protected in the final composite. */
    p.ui = false; p.semi_transparent = true; p.tpage = 1<<5;
    p.rgb[0].r = p.rgb[0].g = p.rgb[0].b = 32;
    psx_raster_prim(&fb,&p,NULL,&opts); psx_raster_prim(&reference,&p,NULL,&plain);
    CHECK(scene.px[at] == 0x318c && mask[at] == 255);
    p.kind = PSX_PRIM_MOVE;
    p.xy[0].x = p.xy[0].y = 4; p.xy[1].x = 5; p.xy[1].y = 5;
    p.xy[2].x = p.xy[2].y = 4;
    psx_raster_prim(&fb,&p,NULL,&opts); psx_raster_prim(&reference,&p,NULL,&plain);
    CHECK(scene.px[5*32+5] == 0x318c && mask[5*32+5] == 255);
    /* Transparent sprite texels must not punch holes in bloom. */
    memset(&p,0,sizeof(p)); p.kind = PSX_PRIM_SPRT; p.ui = true;
    p.tpage = 2<<7; p.xy[2].x = 2; p.xy[2].y = 1;
    vram->px[0][1] = 0x7fff;
    psx_raster_prim(&fb,&p,vram,&opts); psx_raster_prim(&reference,&p,vram,&plain);
    CHECK(!mask[0] && mask[1] == 255 && scene.px[1] == 0x2108);
    /* Opaque black UI still covers bloom; colour comparisons alone miss it. */
    p.kind = PSX_PRIM_TILE;
    psx_raster_prim(&fb,&p,NULL,&opts); psx_raster_prim(&reference,&p,NULL,&plain);
    CHECK(mask[0] == 255 && fb.px[0] == 0 && scene.px[0] == 0x2108);
    p.ui = false;
    psx_raster_prim(&fb,&p,NULL,&opts); psx_raster_prim(&reference,&p,NULL,&plain);
    CHECK(mask[0] == 0 && scene.px[0] == 0);
    for (i=0;i<32*16;i++) CHECK(fb.px[i] == reference.px[i]);
    free(vram); psx_fb_free(&fb); psx_fb_free(&scene); psx_fb_free(&reference);
}

static void screen_layers(void)
{
    q2_screen s;
    psx_ot ot;
    psx_raster_opts opts;
    int view, index;
    CHECK(q2_screen_init(&s,Q2_VIDEO_PAL) == Q2_OK);
    CHECK(q2_screen_capture_scene(&s,true) == Q2_OK);
    q2_screen_set_layout(&s,Q2_SCREEN_LAYOUT_TWO_V,2);
    CHECK(psx_ot_init(&ot,Q2_SCREEN_OT_ENTRIES,32) == Q2_OK);
    psx_raster_opts_default(&opts); opts.dither = false;
    q2_screen_frame_begin(&s,&ot); q2_screen_background(&s);
    for (view=0;view<2;view++) {
        u32 first;
        psx_prim *p;
        q2_screen_view_begin(&s,view,&ot,NULL);
        p = psx_ot_add_bucket(&ot,q2_screen_view_otz(&s,view,2));
        p->kind = PSX_PRIM_TILE; p->xy[2].x = p->xy[2].y = 32;
        p->rgb[0].r = 64;
        first = ot.prim_count;
        p = psx_ot_add_bucket(&ot,q2_screen_view_otz(&s,view,0));
        p->kind = PSX_PRIM_TILE; p->xy[2].x = p->xy[2].y = 8;
        p->rgb[0].g = 255;
        q2_screen_ui_range(&s,view,&ot,first,false);
        CHECK(p->ui); /* native as well as adaptive layouts */
        q2_screen_view_end(&s,&ot);
    }
    q2_screen_compose(&s,&ot,NULL,&opts); q2_screen_present(&s);
    index = s.disp.draw_buffer;
    for (view=0;view<2;view++) {
        int at=(s.view[view].y+2)*s.buf[index].width+s.view[view].x+2;
        CHECK(s.ui_mask[index][at] == 255);
        CHECK(s.scene[index].px[at] == psx_rgb555(64,0,0));
        CHECK(s.buf[index].px[at] == psx_rgb555(0,255,0));
    }
    CHECK(q2_screen_resize(&s,true,1920,1080) == Q2_OK);
    CHECK(q2_screen_capture_scene(&s,true) == Q2_OK);
    CHECK(s.scene[0].width == s.buf[0].width && s.scene[1].height == s.buf[1].height);
    CHECK(q2_screen_capture_scene(&s,false) == Q2_OK);
    CHECK(!s.scene[0].px && !s.scene[1].px && !s.ui_mask[0] && !s.ui_mask[1]);
    psx_ot_free(&ot); q2_screen_free(&s);
}

static void gpu_bloom(q2_gl_display *gpu, int w, int h)
{
    psx_framebuffer fb = {0}, scene = {0};
    q2_gl_scene layer = {0};
    u8 *mask = calloc(256*128,1), *off = malloc((size_t)w*h*4), *on = malloc((size_t)w*h*4);
    int x,y,changed=0,covered=0;
    CHECK(psx_fb_init(&fb,256,128) == Q2_OK);
    CHECK(psx_fb_init(&scene,256,128) == Q2_OK);
    if (!mask || !off || !on || !fb.px || !scene.px) exit(2);
    psx_fb_clear(&fb,0x2108); psx_fb_clear(&scene,0x2108);
    layer.colour=&scene; layer.ui_mask=mask;
    for (y=20;y<40;y++) for (x=20;x<60;x++) {
        fb.px[y*256+x]=0x7fff; mask[y*256+x]=255;
    }
    CHECK(q2_gl_render(gpu,&fb,w,h,false)); CHECK(q2_gl_read(gpu,off));
    CHECK(q2_gl_render_effects(gpu,&fb,w,h,false,&layer)); CHECK(q2_gl_read(gpu,on));
    CHECK(!memcmp(off,on,(size_t)w*h*4)); /* bright UI never emits a halo */
    /* A bright scene source under opaque UI must still bloom around it. */
    for (y=20;y<40;y++) for (x=20;x<60;x++) scene.px[y*256+x]=0x7fff;
    CHECK(q2_gl_render_effects(gpu,&fb,w,h,false,&layer)); CHECK(q2_gl_read(gpu,on));
    for (y=0;y<h;y++) for (x=0;x<w;x++) {
        int sx=(x*2+1)*256/(2*w),sy=(y*2+1)*128/(2*h),at=(y*w+x)*4,c;
        if (mask[sy*256+sx]) { CHECK(!memcmp(off+at,on+at,4)); covered++; }
        else for (c=0;c<3;c++) { CHECK(on[at+c]>=off[at+c]); if(on[at+c]>off[at+c])changed++; }
    }
    CHECK(changed>0 && covered>0);
    /* Splits: a source against one viewport's border cannot illuminate the other. */
    psx_fb_clear(&fb,0); psx_fb_clear(&scene,0); memset(mask,0,256*128);
    layer.view_count=2;
    layer.view[0][2]=128; layer.view[0][3]=128;
    layer.view[1][0]=128; layer.view[1][2]=128; layer.view[1][3]=128;
    for (y=40;y<88;y++) for (x=120;x<128;x++) fb.px[y*256+x]=scene.px[y*256+x]=0x7fff;
    CHECK(q2_gl_render_effects(gpu,&fb,w,h,false,&layer)); CHECK(q2_gl_read(gpu,on));
    for (y=0;y<h;y++) for (x=(w+1)/2;x<w;x++) {
        int at=(y*w+x)*4; CHECK(on[at]==0 && on[at+1]==0 && on[at+2]==0);
    }
    CHECK(q2_gl_render_effects(gpu,&fb,w,h,true,&layer));
    CHECK(q2_gl_render(gpu,&fb,w,h,true)); /* toggling bloom restores reference */
    CHECK(q2_gl_render(gpu,&fb,w,h,false));
    layer.colour=NULL;
    CHECK(!q2_gl_render_effects(gpu,&fb,w,h,false,&layer));
    printf("Bloom %dx%d: UI unchanged, scene halo present, split isolated\n",w,h);
    free(mask); free(off); free(on); psx_fb_free(&fb); psx_fb_free(&scene);
}

int main(void)
{
    q2_gl_display *gpu;
    layers(); screen_layers();
    if (failures) return 1;
    gpu=q2_gl_create_hidden();
    if (!gpu) { puts("SKIP: OpenGL unavailable"); return 77; }
    gpu_bloom(gpu,1024,768); gpu_bloom(gpu,853,479); gpu_bloom(gpu,127,61);
    q2_gl_destroy(gpu);
    printf("Bloom: %d failures\n",failures);
    return failures ? 1 : 0;
}
