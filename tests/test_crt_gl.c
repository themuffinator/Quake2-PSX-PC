/* Real GPU qualification using a hidden, windowed GL context and engine FBOs.
 * No input polling, desktop capture or visible/fullscreen window. */
#include "crt_gl.h"
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s (%s)\n",__LINE__,#x,SDL_GetError()); failures++; } } while (0)

static void fill(psx_framebuffer *fb)
{
    int x, y;
    for (y = 0; y < fb->height; y++) for (x = 0; x < fb->width; x++) {
        int r = x*255/fb->width, g = y*255/fb->height;
        int b = ((x/8+y/8)&1) ? 255 : 0;
        fb->px[y*fb->width+x] = psx_rgb555((u8)r, (u8)g, (u8)b);
    }
}

static void compare(q2_gl_display *gpu, q2_crt *cpu, const psx_framebuffer *fb,
                      int w, int h, bool enabled)
{
    size_t n = (size_t)w*h, i, bad = 0;
    int max_error = 0;
    u8 *pixels = malloc(n*4);
    bool ok;
    if (!pixels) exit(2);
    ok = q2_gl_render(gpu, fb, w, h, enabled);
    CHECK(ok);
    if (!ok) { free(pixels); return; }
    CHECK(q2_gl_read(gpu, pixels));
    if (enabled) CHECK(q2_crt_render(cpu, fb, w, h) == Q2_OK);
    for (i = 0; i < n; i++) {
        int c;
        u32 reference = enabled ? cpu->pixels[i] : 0;
        if (!enabled) {
            int x = (int)(i%w), y = (int)(i/w);
            int sx = (int)(((s64)x*2+1)*fb->width/(2*w));
            int sy = (int)(((s64)y*2+1)*fb->height/(2*h));
            u16 p = fb->px[sy*fb->width+sx];
            for (c = 0; c < 3; c++) {
                u32 v = (p>>(5*c))&31;
                reference |= ((v<<3)|(v>>2))<<(8*c);
            }
        }
        for (c = 0; c < 3; c++) {
            int error = abs((int)pixels[i*4+c] - (int)((reference>>(8*c))&255));
            if (error > max_error) max_error = error;
            if (error > 1) bad++;
        }
        if (pixels[i*4+3] != 255) { failures++; break; }
    }
    printf("%dx%d -> %dx%d CRT %s: max error %d, channels >1: %zu/%zu\n",
           fb->width,fb->height,w,h,enabled?"on":"off",max_error,bad,n*3);
    CHECK(max_error <= (enabled ? 2 : 0));
    CHECK(bad <= (enabled ? n/10000 : 0));
    free(pixels);
}

static void benchmark(q2_gl_display *gpu, q2_crt *cpu, const psx_framebuffer *fb,
                        int w, int h)
{
    u64 begin, end;
    double gpu_ms, cpu_ms, frequency = (double)SDL_GetPerformanceFrequency();
    int i;
    /* Each iteration waits for completion: queued submission alone is NOT a
     * GPU speed measurement. Screenshot readback is outside the timed region. */
    for (i = 0; i < 10; i++) { CHECK(q2_gl_render(gpu,fb,w,h,true)); q2_gl_finish(gpu); }
    begin = SDL_GetPerformanceCounter();
    for (i = 0; i < 60; i++) { CHECK(q2_gl_render(gpu,fb,w,h,true)); q2_gl_finish(gpu); }
    end = SDL_GetPerformanceCounter();
    gpu_ms = (end-begin)*1000.0/frequency/60;
    CHECK(q2_crt_render(cpu,fb,w,h) == Q2_OK);
    begin = SDL_GetPerformanceCounter();
    for (i = 0; i < 30; i++) CHECK(q2_crt_render(cpu,fb,w,h) == Q2_OK);
    end = SDL_GetPerformanceCounter();
    cpu_ms = (end-begin)*1000.0/frequency/30;
    printf("CRT benchmark %dx%d: GPU %.3f ms (upload + render + completion), CPU %.3f ms; %.1fx faster\n",
           w,h,gpu_ms,cpu_ms,cpu_ms/gpu_ms);
}

int main(void)
{
    q2_gl_display *gpu = q2_gl_create_hidden();
    q2_crt cpu = {0};
    psx_framebuffer fb = {0};
    int i;
    if (!gpu) { printf("SKIP: OpenGL 3.3 unavailable: %s\n",SDL_GetError()); return 77; }
    CHECK(psx_fb_init(&fb,512,248) == Q2_OK); fill(&fb);
    compare(gpu,&cpu,&fb,1024,768,false);
    compare(gpu,&cpu,&fb,1024,768,true);
    compare(gpu,&cpu,&fb,853,479,true);
    compare(gpu,&cpu,&fb,853,479,false);
    compare(gpu,&cpu,&fb,1920,1080,true);
    compare(gpu,&cpu,&fb,3840,2160,true);
    compare(gpu,&cpu,&fb,512,248,true);
    compare(gpu,&cpu,&fb,127,61,true);
    /* Same lifecycle after the native raster changes (NTSC / adaptive aspect). */
    psx_fb_free(&fb); CHECK(psx_fb_init(&fb,640,240) == Q2_OK); fill(&fb);
    compare(gpu,&cpu,&fb,1280,720,true);
    compare(gpu,&cpu,&fb,1280,720,false);
    compare(gpu,&cpu,&fb,5120,1440,true);
    CHECK(!q2_gl_render(gpu,&fb,0,0,true));
    psx_fb_free(&fb); CHECK(psx_fb_init(&fb,512,248) == Q2_OK); fill(&fb);
    benchmark(gpu,&cpu,&fb,1024,768);
    benchmark(gpu,&cpu,&fb,1920,1080);
    /* A GPU filter failure can upload the CPU fallback into the same display. */
    CHECK(q2_gl_upload(gpu,&cpu));
    {
        size_t n = (size_t)cpu.width*cpu.height;
        u8 *pixels = malloc(n*4);
        if (!pixels) exit(2);
        CHECK(q2_gl_read(gpu,pixels));
        for (i = 0; i < (int)n; i++) {
            int c;
            for (c = 0; c < 4; c++) if (pixels[i*4+c] != (u8)(cpu.pixels[i]>>(8*c))) {
                CHECK(false); i = (int)n; break;
            }
        }
        free(pixels);
    }
    /* Exercise the actual window blit/swap on the hidden window as well. */
    CHECK(q2_gl_present(gpu,64,64,0,0));
    q2_gl_destroy(gpu);
    gpu = q2_gl_create_hidden(); CHECK(gpu != NULL);
    if (gpu) { compare(gpu,&cpu,&fb,640,480,true); q2_gl_destroy(gpu); }
    q2_crt_free(&cpu); psx_fb_free(&fb);
    printf("GPU CRT: %d failures\n",failures);
    return failures ? 1 : 0;
}
