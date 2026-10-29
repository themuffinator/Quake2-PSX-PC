#include "crt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)

int main(void)
{
    q2_crt crt = {0};
    psx_framebuffer fb = {0};
    u32 *copy;
    int x, y;
    double sum = 0, min = 255, max = 0;
    clock_t start;
    CHECK(psx_fb_init(&fb, 512, 248) == Q2_OK);
    CHECK(q2_crt_render(&crt, &fb, 0, 100) == Q2_ERR_INVALID_ARG);
    psx_fb_clear(&fb, 0);
    CHECK(q2_crt_render(&crt, &fb, 1024, 768) == Q2_OK);
    for (x = 0; x < 1024*768; x++) CHECK(crt.pixels[x] == 0xFF000000u);
    psx_fb_clear(&fb, psx_rgb555(128, 128, 128));
    CHECK(q2_crt_render(&crt, &fb, 1024, 768) == Q2_OK);
    /* Across complete phosphor triads, a neutral patch stays neutral and
     * maintains its brightness while still resolving the source scanlines. */
    for (y = 200; y < 560; y++) for (x = 350; x < 650; x++) {
        u32 p = crt.pixels[y*1024+x];
        double v = ((p&255)+((p>>8)&255)+((p>>16)&255))/3.0;
        sum += v;
        if (v < min) min = v;
        if (v > max) max = v;
    }
    CHECK(sum/(360*300) > 115 && sum/(360*300) < 140);
    CHECK(max-min > 15);
    copy = malloc(1024*768*sizeof(*copy));
    if (!copy) return 2;
    memcpy(copy, crt.pixels, 1024*768*sizeof(*copy));
    CHECK(q2_crt_render(&crt, &fb, 1024, 768) == Q2_OK);
    CHECK(!memcmp(copy, crt.pixels, 1024*768*sizeof(*copy)));
    for (x = 0; x < fb.width*fb.height; x++) CHECK(fb.px[x] == psx_rgb555(128,128,128));
    free(copy);
    psx_fb_clear(&fb, psx_rgb555(255, 0, 0));
    CHECK(q2_crt_render(&crt, &fb, 641, 479) == Q2_OK);
    for (x = 0; x < 641*479; x++) CHECK((crt.pixels[x] & 0x00FFFF00u) == 0);
    CHECK(q2_crt_render(&crt, &fb, 1, 1) == Q2_OK);
    CHECK(q2_crt_render(&crt, &fb, 5000, 1) == Q2_OK);
    CHECK(q2_crt_render(&crt, &fb, 1, 5000) == Q2_OK);
    CHECK(q2_crt_render(&crt, &fb, 32769, 1) == Q2_ERR_RANGE);
    CHECK(q2_crt_render(&crt, &fb, 5000, 5000) == Q2_ERR_RANGE);
    CHECK(crt.width == 1 && crt.height == 5000);
    psx_fb_free(&fb);
    CHECK(psx_fb_init(&fb, 1, 1) == Q2_OK);
    fb.px[0] = 0x7FFF;
    CHECK(q2_crt_render(&crt, &fb, 7, 9) == Q2_OK);
    psx_fb_free(&fb);
    CHECK(psx_fb_init(&fb, 512, 240) == Q2_OK);
    psx_fb_clear(&fb, psx_rgb555(128,128,128));
    CHECK(q2_crt_render(&crt, &fb, 1024, 768) == Q2_OK);
    start = clock();
    for (x = 0; x < 30; x++) CHECK(q2_crt_render(&crt, &fb, 1024, 768) == Q2_OK);
    printf("CRT 1024x768: %.2f ms/frame (30 frames); %d failures\n",
           1000.0*(clock()-start)/CLOCKS_PER_SEC/30, failures);
    q2_crt_free(&crt);
    q2_crt_free(&crt);
    psx_fb_free(&fb);
    return failures ? 1 : 0;
}
