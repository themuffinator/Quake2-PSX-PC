/* OpenGL 3.3 final display pass. No changes to the PSX software renderer.
 * All captures read the engine-owned output FBO, never the OS/window. */
#ifndef Q2PSX_CRT_GL_H
#define Q2PSX_CRT_GL_H
#include <SDL3/SDL.h>
#include "crt.h"

typedef struct q2_gl_display q2_gl_display;
typedef struct q2_gl_scene {
    const psx_framebuffer *colour;
    const u8 *ui_mask;
    int view_count;
    int view[4][4];             /* native x, y, width, height; isolate splits */
} q2_gl_scene;
void q2_gl_attributes(void);
q2_gl_display *q2_gl_create(SDL_Window *window);
q2_gl_display *q2_gl_create_hidden(void);
void q2_gl_destroy(q2_gl_display *g);
bool q2_gl_render(q2_gl_display *g, const psx_framebuffer *src,
                   int width, int height, bool crt);
/* A non-NULL scene enables bloom. UI is excluded both from extraction and
 * from the composite. CRT, if enabled, follows the scene/UI composite. */
bool q2_gl_render_effects(q2_gl_display *g, const psx_framebuffer *src,
                           int width, int height, bool crt, const q2_gl_scene *scene);
bool q2_gl_upload(q2_gl_display *g, const q2_crt *cpu);
bool q2_gl_present(q2_gl_display *g, int window_w, int window_h, int x, int y);
bool q2_gl_read(q2_gl_display *g, u8 *rgba);
q2_result q2_gl_write_ppm(q2_gl_display *g, const char *path);
/* Synchronization is for benchmarking only; never in the frame loop. */
void q2_gl_finish(q2_gl_display *g);
#endif
