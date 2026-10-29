/* Optional host display emulation. The PSX framebuffer is never modified.
 * Zero-initialize the context, then free it with q2_crt_free. No SDL dependency:
 * this is the fallback and numerical reference for the GPU display pass. */
#ifndef Q2PSX_CRT_H
#define Q2PSX_CRT_H

#include "raster.h"

typedef struct q2_crt {
    struct q2_crt_data *data;
    u32 *pixels;                 /* opaque ABGR8888, R in the low byte */
    int width, height;
} q2_crt;

#define Q2_CRT_PHASES 1024
#define Q2_CRT_GAMMA 8192
/* Shared small lookup tables keep the CPU reference and GPU display identical.
 * Beam entries are already quantized to the reference's fixed-point precision. */
void q2_crt_gamma_tables(float decode[32], u8 encode[Q2_CRT_GAMMA + 1]);
void q2_crt_beam_table(int source_height, int output_height,
                       float beams[Q2_CRT_PHASES][2]);

q2_result q2_crt_render(q2_crt *crt, const psx_framebuffer *source,
                       int width, int height);
void q2_crt_free(q2_crt *crt);

/* Display UV -> source UV, also used for menu pointer hit testing. */
void q2_crt_source_uv(double *u, double *v);

/* Captures the fitted picture, without window borders or screen-position
 * offsets. Disabled mode is the ordinary nearest-neighbour display. */
q2_result q2_crt_write_ppm(q2_crt *crt, const psx_framebuffer *source,
                          bool enabled, int width, int height, const char *path);

#endif
