/* Original GPLv2 implementation of a progressive RGB CRT display model.
 *
 * Decode RGB555 into linear light, reconstruct the horizontal beam, add a
 * small, energy-conserving optical spread, then sample the curved faceplate.
 * Scanlines follow SOURCE rows, phosphors follow OUTPUT pixels. Two Gaussian
 * beam profiles broaden with luminance. Integrating their profiles across an
 * output pixel and fading below 2x avoids undersampling/resize moire.
 *
 * Geometry and gamma tables are cached; the steady-state loop has no powers,
 * exponentials, trigonometry or allocations. No temporal flicker, artificial
 * channel offsets or history buffer: paused pictures and captures stay stable.
 */
#include "crt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CRT_LIMIT 4096
#define CRT_OUTPUT_AXIS_LIMIT 32768
#define CRT_OUTPUT_PIXELS ((size_t)4096 * 4096)
#define CRT_PHASES Q2_CRT_PHASES
#define CRT_GAMMA Q2_CRT_GAMMA

typedef struct crt_colour { float c[3]; } crt_colour;
typedef struct crt_sample {
    u32 index;
    u16 x, y, shade, narrow, wide;
} crt_sample;

struct q2_crt_data {
    int sw, sh, stride;
    crt_sample *map;
    crt_colour *linear, *beam, *blur;
    float decode[32];
    u8 encode[CRT_GAMMA + 1];
};

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static int clampi(int x, int hi) { return x < 0 ? 0 : x > hi ? hi : x; }

void q2_crt_source_uv(double *u, double *v)
{
    double x = *u * 2 - 1, y = *v * 2 - 1;
    /* Small barrel curvature, inset just enough to preserve the full HUD. */
    *u = 0.5 + x * (0.985 + 0.025 * y * y) * 0.5;
    *v = 0.5 + y * (0.985 + 0.025 * x * x) * 0.5;
}

void q2_crt_free(q2_crt *crt)
{
    if (!crt) return;
    if (crt->data) {
        free(crt->data->map);
        free(crt->data->linear);
        free(crt->data->beam);
        free(crt->data->blur);
        free(crt->data);
    }
    free(crt->pixels);
    memset(crt, 0, sizeof(*crt));
}

static float profile(float phase, float sigma, float footprint)
{
    float sum = 0;
    int line;
    /* Integral of a unit-energy Gaussian over one destination pixel. */
    for (line = -2; line <= 2; line++) {
        float d = phase - (float)line;
        float s = 1.41421356237f * sigma;
        sum += 0.5f * (erff((d + footprint * 0.5f) / s) -
                       erff((d - footprint * 0.5f) / s)) / footprint;
    }
    return sum;
}

void q2_crt_gamma_tables(float decode[32], u8 encode[CRT_GAMMA + 1])
{
    int i;
    for (i = 0; i < 32; i++) {
        float s = (float)i / 31.0f;
        decode[i] = s <= 0.04045f ? s / 12.92f :
                         powf((s + 0.055f) / 1.055f, 2.4f);
    }
    for (i = 0; i <= CRT_GAMMA; i++) {
        float l = (float)i / CRT_GAMMA;
        float s = l <= 0.0031308f ? l * 12.92f :
                         1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
        encode[i] = (u8)(clamp01(s) * 255 + 0.5f);
    }
}

void q2_crt_beam_table(int sh, int h, float beams[CRT_PHASES][2])
{
    float footprint = (float)sh / (float)h;
    float visibility = clamp01((float)h / (float)sh - 1.0f);
    int i;
    for (i = 0; i < CRT_PHASES; i++) {
        float p = (float)i / CRT_PHASES;
        beams[i][0] = (float)(u16)((1 + 0.8f * visibility *
                      (profile(p, 0.22f, footprint) - 1)) * 16384);
        beams[i][1] = (float)(u16)((1 + 0.8f * visibility *
                      (profile(p, 0.34f, footprint) - 1)) * 16384);
    }
}

static q2_result prepare(q2_crt *crt, int sw, int sh, int w, int h)
{
    q2_crt next = {0};
    struct q2_crt_data *d;
    float beams[CRT_PHASES][2];
    size_t source_count = (size_t)(sw + 2) * (size_t)(sh + 2);
    int x, y;

    if (crt->data && crt->width == w && crt->height == h &&
        crt->data->sw == sw && crt->data->sh == sh) return Q2_OK;
    next.width = w;
    next.height = h;
    next.data = d = calloc(1, sizeof(*d));
    if (!d) return Q2_ERR_NO_MEMORY;
    d->sw = sw; d->sh = sh; d->stride = sw + 2;
    d->map = malloc((size_t)w * h * sizeof(*d->map));
    next.pixels = malloc((size_t)w * h * sizeof(*next.pixels));
    d->linear = calloc(source_count, sizeof(*d->linear));
    d->beam = calloc(source_count, sizeof(*d->beam));
    d->blur = calloc(source_count, sizeof(*d->blur));
    if (!d->map || !next.pixels || !d->linear || !d->beam || !d->blur) {
        q2_crt_free(&next);
        return Q2_ERR_NO_MEMORY;
    }
    q2_crt_gamma_tables(d->decode, d->encode);
    q2_crt_beam_table(sh, h, beams);
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        crt_sample *m = &d->map[(size_t)y * w + x];
        double u = (x + 0.5) / w, v = (y + 0.5) / h;
        float sx, sy, edge, shade;
        int ix, iy, phase;
        q2_crt_source_uv(&u, &v);
        sx = (float)(u * sw - 0.5);
        sy = (float)(v * sh - 0.5);
        /* The padded border makes all four bilinear taps safe. */
        sx = fmaxf(-1, fminf((float)sw - 0.001f, sx));
        sy = fmaxf(-1, fminf((float)sh - 0.001f, sy));
        ix = (int)floorf(sx); iy = (int)floorf(sy);
        m->index = (u32)((iy + 1) * d->stride + ix + 1);
        m->x = (u16)((sx - ix) * 65535);
        m->y = (u16)((sy - iy) * 65535);
        phase = (int)((sy - floorf(sy)) * CRT_PHASES) % CRT_PHASES;
        m->narrow = (u16)beams[phase][0];
        m->wide = (u16)beams[phase][1];
        edge = (float)fmin(fmin(u, 1-u) * w, fmin(v, 1-v) * h);
        shade = clamp01(edge + 0.5f);  /* antialiased glass boundary */
        shade *= 1 - 0.065f * (float)((u*2-1)*(u*2-1) + (v*2-1)*(v*2-1));
        m->shade = (u16)(shade * 65535);
    }
    q2_crt_free(crt);
    *crt = next;
    return Q2_OK;
}

q2_result q2_crt_render(q2_crt *crt, const psx_framebuffer *src, int w, int h)
{
    struct q2_crt_data *d;
    q2_result rc;
    int x, y, c, k, stride;
    static const float kernel[5] = {1/16.0f, 4/16.0f, 6/16.0f, 4/16.0f, 1/16.0f};
    if (!crt || !src || !src->px || w < 1 || h < 1 ||
        src->width < 1 || src->height < 1) return Q2_ERR_INVALID_ARG;
    /* Bound memory by area, not a square's side: 5120x1440 and 7680x2160
     * both cost less than the previously supported 4096x4096 target. */
    if (w > CRT_OUTPUT_AXIS_LIMIT || h > CRT_OUTPUT_AXIS_LIMIT ||
        (size_t)w * h > CRT_OUTPUT_PIXELS || src->width > CRT_LIMIT ||
        src->height > CRT_LIMIT) return Q2_ERR_RANGE;
    rc = prepare(crt, src->width, src->height, w, h);
    if (rc != Q2_OK) return rc;
    d = crt->data; stride = d->stride;
    for (y = 0; y < d->sh; y++) for (x = 0; x < d->sw; x++) {
        u16 p = src->px[(size_t)y * d->sw + x];
        crt_colour *l = &d->linear[(size_t)(y+1) * stride + x+1];
        for (c = 0; c < 3; c++) l->c[c] = d->decode[(p >> (5*c)) & 31];
    }
    /* Horizontal beam and first half of the wider optical spread. Clamped
     * taps preserve constant colours, including the outermost source rows. */
    for (y = 0; y < d->sh; y++) for (x = 0; x < d->sw; x++) {
        int at = (y+1) * stride + x+1;
        for (c = 0; c < 3; c++) {
            float sum = 0;
            for (k = -2; k <= 2; k++)
                sum += kernel[k+2] * d->linear[(y+1)*stride +
                         clampi(x+k, d->sw-1)+1].c[c];
            d->blur[at].c[c] = sum;
            d->beam[at].c[c] = 0.76f * d->linear[at].c[c] +
                0.12f * (d->linear[(y+1)*stride+clampi(x-1,d->sw-1)+1].c[c] +
                         d->linear[(y+1)*stride+clampi(x+1,d->sw-1)+1].c[c]);
        }
    }
    for (y = 0; y < d->sh; y++) for (x = 0; x < d->sw; x++) {
        int at = (y+1)*stride+x+1;
        for (c = 0; c < 3; c++) {
            float glow = 0;
            for (k = -2; k <= 2; k++)
                glow += kernel[k+2] * d->blur[(clampi(y+k,d->sh-1)+1)*stride+x+1].c[c];
            d->linear[at].c[c] = 0.94f*d->beam[at].c[c] + 0.06f*glow;
        }
    }
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        size_t at = (size_t)y*w+x;
        const crt_sample *m = &d->map[at];
        const crt_colour *a = &d->linear[m->index];
        float fx = m->x / 65535.0f, fy = m->y / 65535.0f;
        float rgb[3], luma, scan, shade = m->shade / 65535.0f;
        u32 pixel = 0xFF000000u;
        /* A low-strength RGB aperture grille, one triad per three physical
         * pixels. Fade it at small sizes rather than turning text into stripes. */
        float mask = 0.12f * clamp01((float)w / d->sw - 1);
        for (c = 0; c < 3; c++) {
            float top = a[0].c[c] + fx*(a[1].c[c]-a[0].c[c]);
            float bot = a[stride].c[c] + fx*(a[stride+1].c[c]-a[stride].c[c]);
            rgb[c] = top + fy*(bot-top);
        }
        luma = 0.2126f*rgb[0] + 0.7152f*rgb[1] + 0.0722f*rgb[2];
        scan = (m->narrow + (m->wide - (float)m->narrow)*sqrtf(luma)) / 16384.0f;
        for (c = 0; c < 3; c++) {
            float phosphor = x % 3 == c ? 1 + mask*2 : 1-mask;
            int level = (int)(rgb[c]*scan*shade*phosphor*CRT_GAMMA + 0.5f);
            pixel |= (u32)d->encode[clampi(level, CRT_GAMMA)] << (8*c);
        }
        crt->pixels[at] = pixel;
    }
    return Q2_OK;
}

q2_result q2_crt_write_ppm(q2_crt *crt, const psx_framebuffer *src,
                          bool enabled, int w, int h, const char *path)
{
    FILE *f;
    u8 *row;
    int x, y;
    q2_result rc = Q2_OK;
    if (!crt || !src || !src->px || !path || w < 1 || h < 1 ||
        src->width < 1 || src->height < 1) return Q2_ERR_INVALID_ARG;
    if (w > CRT_OUTPUT_AXIS_LIMIT || h > CRT_OUTPUT_AXIS_LIMIT ||
        (size_t)w * h > CRT_OUTPUT_PIXELS) return Q2_ERR_RANGE;
    if (enabled && (rc = q2_crt_render(crt, src, w, h)) != Q2_OK) return rc;
    row = malloc((size_t)w*3);
    if (!row) return Q2_ERR_NO_MEMORY;
    f = fopen(path, "wb");
    if (!f) { free(row); return Q2_ERR_IO; }
    if (fprintf(f, "P6\n%d %d\n255\n", w, h) < 0) rc = Q2_ERR_IO;
    for (y = 0; y < h && rc == Q2_OK; y++) {
        for (x = 0; x < w; x++) {
            int c;
            u32 p;
            if (enabled) p = crt->pixels[(size_t)y*w+x];
            else {
                int sx = (int)(((s64)x*2+1)*src->width/(2*w));
                int sy = (int)(((s64)y*2+1)*src->height/(2*h));
                u16 v = src->px[(size_t)sy*src->width+sx];
                p = 0;
                for (c = 0; c < 3; c++) {
                    u32 v5 = (v >> (5*c)) & 31;
                    p |= ((v5 << 3) | (v5 >> 2)) << (8*c);
                }
            }
            for (c = 0; c < 3; c++) row[x*3+c] = (u8)(p >> (8*c));
        }
        if (fwrite(row, 3, (size_t)w, f) != (size_t)w) rc = Q2_ERR_IO;
    }
    if (fclose(f) != 0) rc = Q2_ERR_IO;
    free(row);
    return rc;
}
