#include "crt_gl.h"
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_opengl_glext.h>
#include <stdio.h>
#include <stdlib.h>
#include "crt_gl_shaders.h"

/* Load every entry point through SDL. No platform GL link library or vendored
 * loader, and no sharing/interleaving with SDL_Renderer's private GL state. */
#define GL_FUNCTIONS(X) \
 X(void,GenTextures,(GLsizei n,GLuint *t)) \
 X(void,DeleteTextures,(GLsizei n,const GLuint *t)) \
 X(void,BindTexture,(GLenum target,GLuint texture)) \
 X(void,TexParameteri,(GLenum target,GLenum name,GLint value)) \
 X(void,TexImage2D,(GLenum target,GLint level,GLint internal,GLsizei w,GLsizei h,GLint border,GLenum format,GLenum type,const void *data)) \
 X(void,TexSubImage2D,(GLenum target,GLint level,GLint x,GLint y,GLsizei w,GLsizei h,GLenum format,GLenum type,const void *data)) \
 X(void,ActiveTexture,(GLenum texture)) \
 X(void,PixelStorei,(GLenum name,GLint value)) \
 X(void,GenFramebuffers,(GLsizei n,GLuint *f)) \
 X(void,DeleteFramebuffers,(GLsizei n,const GLuint *f)) \
 X(void,BindFramebuffer,(GLenum target,GLuint f)) \
 X(void,FramebufferTexture2D,(GLenum target,GLenum attachment,GLenum textarget,GLuint tex,GLint level)) \
 X(GLenum,CheckFramebufferStatus,(GLenum target)) \
 X(void,DrawBuffers,(GLsizei n,const GLenum *buffers)) \
 X(void,BlitFramebuffer,(GLint sx0,GLint sy0,GLint sx1,GLint sy1,GLint dx0,GLint dy0,GLint dx1,GLint dy1,GLbitfield mask,GLenum filter)) \
 X(GLuint,CreateShader,(GLenum type)) \
 X(void,ShaderSource,(GLuint shader,GLsizei count,const GLchar *const *text,const GLint *length)) \
 X(void,CompileShader,(GLuint shader)) \
 X(void,GetShaderiv,(GLuint shader,GLenum name,GLint *value)) \
 X(void,GetShaderInfoLog,(GLuint shader,GLsizei size,GLsizei *length,GLchar *text)) \
 X(void,DeleteShader,(GLuint shader)) \
 X(GLuint,CreateProgram,(void)) \
 X(void,AttachShader,(GLuint program,GLuint shader)) \
 X(void,LinkProgram,(GLuint program)) \
 X(void,GetProgramiv,(GLuint program,GLenum name,GLint *value)) \
 X(void,GetProgramInfoLog,(GLuint program,GLsizei size,GLsizei *length,GLchar *text)) \
 X(void,UseProgram,(GLuint program)) \
 X(void,DeleteProgram,(GLuint program)) \
 X(GLint,GetUniformLocation,(GLuint program,const GLchar *name)) \
 X(void,Uniform1i,(GLint loc,GLint v)) \
 X(void,Uniform2i,(GLint loc,GLint x,GLint y)) \
 X(void,Uniform2f,(GLint loc,GLfloat x,GLfloat y)) \
 X(void,Uniform4iv,(GLint loc,GLsizei n,const GLint *v)) \
 X(void,GenVertexArrays,(GLsizei n,GLuint *v)) \
 X(void,DeleteVertexArrays,(GLsizei n,const GLuint *v)) \
 X(void,BindVertexArray,(GLuint v)) \
 X(void,Viewport,(GLint x,GLint y,GLsizei w,GLsizei h)) \
 X(void,DrawArrays,(GLenum mode,GLint first,GLsizei count)) \
 X(void,Disable,(GLenum cap)) \
 X(void,ClearColor,(GLfloat r,GLfloat g,GLfloat b,GLfloat a)) \
 X(void,Clear,(GLbitfield mask)) \
 X(void,ReadPixels,(GLint x,GLint y,GLsizei w,GLsizei h,GLenum format,GLenum type,void *pixels)) \
 X(void,GetIntegerv,(GLenum name,GLint *value)) \
 X(const GLubyte *,GetString,(GLenum name)) \
 X(GLenum,GetError,(void)) \
 X(void,Finish,(void))

enum { TEX_SOURCE, TEX_BEAM, TEX_BLUR, TEX_LINEAR, TEX_OUTPUT, TEX_DECODE,
       TEX_ENCODE, TEX_PROFILES, TEX_SCENE, TEX_MASK, TEX_BRIGHT, TEX_TEMP,
       TEX_NEAR, TEX_WIDE, TEX_COMPOSITE, TEX_COUNT };
enum { PASS_NEAREST, PASS_HORIZONTAL, PASS_VERTICAL, PASS_CRT,
       PASS_EXTRACT, PASS_BLUR, PASS_COMPOSITE, PASS_BLOOM_OUTPUT, PASS_COUNT };
struct q2_gl_display {
    SDL_Window *window;
    SDL_GLContext context;
    bool owned_window;
    bool crt_failed;
    bool bloom_failed, gamma_ready;
    GLuint texture[TEX_COUNT], fbo, vao, program[PASS_COUNT];
    int sw, sh, width, height, profile_height, profile_source_height, max_texture;
#define DECLARE(ret,name,args) ret (APIENTRY *name) args;
    GL_FUNCTIONS(DECLARE)
#undef DECLARE
};

void q2_gl_attributes(void)
{
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
}

static bool current(q2_gl_display *g)
{
    return g && SDL_GL_MakeCurrent(g->window, g->context);
}

static GLuint shader(q2_gl_display *g, GLenum type, const char *text)
{
    GLuint s = g->CreateShader(type);
    GLint ok = 0;
    char log[2048] = {0};
    g->ShaderSource(s, 1, &text, NULL);
    g->CompileShader(s);
    g->GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (ok) return s;
    g->GetShaderInfoLog(s, sizeof(log), NULL, log);
    Q2_WARN("GPU display shader: %s", log);
    g->DeleteShader(s);
    return 0;
}

static GLuint program(q2_gl_display *g, const char *fragment)
{
    GLuint v = shader(g, GL_VERTEX_SHADER, gl_vertex);
    GLuint f = shader(g, GL_FRAGMENT_SHADER, fragment), p = 0;
    GLint ok = 0;
    char log[2048] = {0};
    if (v && f) {
        p = g->CreateProgram();
        g->AttachShader(p, v); g->AttachShader(p, f); g->LinkProgram(p);
        g->GetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            g->GetProgramInfoLog(p, sizeof(log), NULL, log);
            Q2_WARN("GPU display link: %s", log);
            g->DeleteProgram(p); p = 0;
        }
    }
    if (v) g->DeleteShader(v);
    if (f) g->DeleteShader(f);
    return p;
}

static void texture(q2_gl_display *g, int slot, int w, int h, GLint internal,
                     GLenum format, GLenum type, const void *data)
{
    g->BindTexture(GL_TEXTURE_2D, g->texture[slot]);
    g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g->TexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, data);
}

static bool target(q2_gl_display *g, int slot, int second, int w, int h)
{
    const GLenum buffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    g->BindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    g->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            g->texture[slot], 0);
    g->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                            second >= 0 ? g->texture[second] : 0, 0);
    g->DrawBuffers(second >= 0 ? 2 : 1, buffers);
    g->Viewport(0, 0, w, h);
    return g->CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static void sampler(q2_gl_display *g, GLuint p, const char *name, int unit, int slot)
{
    g->ActiveTexture(GL_TEXTURE0 + unit);
    g->BindTexture(GL_TEXTURE_2D, g->texture[slot]);
    g->Uniform1i(g->GetUniformLocation(p, name), unit);
}

q2_gl_display *q2_gl_create(SDL_Window *window)
{
    q2_gl_display *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->window = window;
    g->context = SDL_GL_CreateContext(window);
    if (!g->context) { free(g); return NULL; }
#define LOAD(ret,name,args) g->name = (ret (APIENTRY *) args)SDL_GL_GetProcAddress("gl" #name); if (!g->name) goto fail;
    GL_FUNCTIONS(LOAD)
#undef LOAD
    g->GetIntegerv(GL_MAX_TEXTURE_SIZE, &g->max_texture);
    if (g->max_texture < Q2_CRT_PHASES) goto fail;
    g->GenTextures(TEX_COUNT, g->texture);
    g->GenFramebuffers(1, &g->fbo);
    g->GenVertexArrays(1, &g->vao);
    g->program[PASS_NEAREST] = program(g, gl_nearest);
    if (!g->program[PASS_NEAREST]) goto fail;
    g->Disable(GL_DITHER); g->Disable(GL_BLEND); g->Disable(GL_DEPTH_TEST);
    g->Disable(GL_FRAMEBUFFER_SRGB);
    g->PixelStorei(GL_UNPACK_ALIGNMENT, 1); g->PixelStorei(GL_PACK_ALIGNMENT, 1);
    SDL_GL_SetSwapInterval(1);
    Q2_INFO("GPU display: %s, %s", g->GetString(GL_RENDERER), g->GetString(GL_VERSION));
    return g;
fail:
    q2_gl_destroy(g);
    return NULL;
}

q2_gl_display *q2_gl_create_hidden(void)
{
    SDL_Window *window;
    q2_gl_display *g;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return NULL;
    q2_gl_attributes();
    window = SDL_CreateWindow("Q2PSX GPU capture", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    g = window ? q2_gl_create(window) : NULL;
    if (!g) {
        if (window) SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return NULL;
    }
    g->owned_window = true;
    SDL_GL_SetSwapInterval(0);
    return g;
}

void q2_gl_destroy(q2_gl_display *g)
{
    int i;
    if (!g) return;
    if (current(g)) {
        if (g->DeleteProgram) for (i = 0; i < PASS_COUNT; i++)
            if (g->program[i]) g->DeleteProgram(g->program[i]);
        if (g->DeleteTextures) g->DeleteTextures(TEX_COUNT, g->texture);
        if (g->DeleteFramebuffers && g->fbo) g->DeleteFramebuffers(1, &g->fbo);
        if (g->DeleteVertexArrays && g->vao) g->DeleteVertexArrays(1, &g->vao);
    }
    SDL_GL_DestroyContext(g->context);
    if (g->owned_window) {
        SDL_DestroyWindow(g->window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    free(g);
}

static void views(q2_gl_display *g, GLuint p, const q2_gl_scene *scene)
{
    g->Uniform1i(g->GetUniformLocation(p, "ViewCount"), scene->view_count);
    if (scene->view_count)
        g->Uniform4iv(g->GetUniformLocation(p, "Views"), scene->view_count, &scene->view[0][0]);
}

static bool bloom(q2_gl_display *g, const q2_gl_scene *scene, int w, int h)
{
    GLuint p;
    int radius, axis;
    g->ActiveTexture(GL_TEXTURE0);
    g->BindTexture(GL_TEXTURE_2D, g->texture[TEX_SCENE]);
    g->TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g->sw, g->sh, GL_RED_INTEGER,
                     GL_UNSIGNED_SHORT, scene->colour->px);
    g->BindTexture(GL_TEXTURE_2D, g->texture[TEX_MASK]);
    g->TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g->sw, g->sh, GL_RED_INTEGER,
                     GL_UNSIGNED_BYTE, scene->ui_mask);
    p = g->program[PASS_EXTRACT]; g->UseProgram(p);
    if (!target(g, TEX_BRIGHT, -1, g->sw, g->sh)) return false;
    sampler(g, p, "Scene", 0, TEX_SCENE); sampler(g, p, "Decode", 1, TEX_DECODE);
    views(g, p, scene);
    g->DrawArrays(GL_TRIANGLES, 0, 3);
    p = g->program[PASS_BLUR]; g->UseProgram(p);
    views(g, p, scene);
    for (radius = 0; radius < 2; radius++) for (axis = 0; axis < 2; axis++) {
        float r = (radius ? 3.5f : 1.0f) * g->sh / 248.0f;
        /* Match radius in displayed pixels, including anamorphic PSX pixels. */
        float rx = r * g->sw / g->sh * h / w;
        int output = axis ? (radius ? TEX_WIDE : TEX_NEAR) : TEX_TEMP;
        if (!target(g, output, -1, g->sw, g->sh)) return false;
        sampler(g, p, "Image", 0, axis ? TEX_TEMP : TEX_BRIGHT);
        g->Uniform2f(g->GetUniformLocation(p, "Step"), axis ? 0 : rx, axis ? r : 0);
        g->DrawArrays(GL_TRIANGLES, 0, 3);
    }
    return true;
}

static void composite_samplers(q2_gl_display *g, GLuint p, const q2_gl_scene *scene)
{
    sampler(g, p, "Source", 0, TEX_SOURCE); sampler(g, p, "Decode", 1, TEX_DECODE);
    sampler(g, p, "Mask", 2, TEX_MASK); sampler(g, p, "Near", 3, TEX_NEAR);
    sampler(g, p, "Wide", 4, TEX_WIDE); sampler(g, p, "Encode", 5, TEX_ENCODE);
    views(g, p, scene);
}

bool q2_gl_render(q2_gl_display *g, const psx_framebuffer *src, int w, int h, bool crt)
{
    return q2_gl_render_effects(g, src, w, h, crt, NULL);
}

bool q2_gl_render_effects(q2_gl_display *g, const psx_framebuffer *src,
                           int w, int h, bool crt, const q2_gl_scene *scene)
{
    GLuint p;
    if (!g || !src || !src->px || w < 1 || h < 1 || src->width < 1 || src->height < 1 ||
        w > g->max_texture || h > g->max_texture || src->width > g->max_texture ||
        src->height > g->max_texture || !current(g)) return false;
    if (crt && g->crt_failed) return false;
    if (scene && (g->bloom_failed || !scene->colour || !scene->colour->px || !scene->ui_mask ||
        scene->colour->width != src->width || scene->colour->height != src->height ||
        scene->view_count < 0 || scene->view_count > 4)) return false;
    g->ActiveTexture(GL_TEXTURE0);
    g->BindVertexArray(g->vao);
    if ((crt || scene) && !g->gamma_ready) {
        float decode[32]; u8 encode[128*65] = {0};
        q2_crt_gamma_tables(decode, encode);
        texture(g, TEX_DECODE, 32, 1, GL_R32F, GL_RED, GL_FLOAT, decode);
        texture(g, TEX_ENCODE, 128, 65, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, encode);
        g->gamma_ready = true;
    }
    if (scene && !g->program[PASS_BLOOM_OUTPUT]) {
        g->program[PASS_EXTRACT] = program(g, gl_bloom_extract);
        g->program[PASS_BLUR] = program(g, gl_bloom_blur);
        g->program[PASS_COMPOSITE] = program(g, gl_bloom_composite);
        g->program[PASS_BLOOM_OUTPUT] = program(g, gl_bloom_output);
        if (!g->program[PASS_EXTRACT] || !g->program[PASS_BLUR] ||
            !g->program[PASS_COMPOSITE] || !g->program[PASS_BLOOM_OUTPUT]) {
            g->bloom_failed = true; return false;
        }
        g->sw = g->sh = 0;
    }
    if (crt && !g->program[PASS_CRT]) {
        g->program[PASS_HORIZONTAL] = program(g, gl_horizontal);
        g->program[PASS_VERTICAL] = program(g, gl_vertical);
        g->program[PASS_CRT] = program(g, gl_crt);
        if (!g->program[PASS_HORIZONTAL] || !g->program[PASS_VERTICAL] ||
            !g->program[PASS_CRT]) { g->crt_failed = true; return false; }
        /* Allocate the native CRT targets even when the source size is unchanged
         * after switching on CRT in an already running unfiltered session. */
        g->sw = g->sh = 0;
    }
    if (src->width != g->sw || src->height != g->sh) {
        int i;
        texture(g, TEX_SOURCE, src->width, src->height, GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, NULL);
        if (g->program[PASS_CRT]) for (i = TEX_BEAM; i <= TEX_LINEAR; i++)
            texture(g, i, src->width, src->height, GL_RGBA32F, GL_RGBA, GL_FLOAT, NULL);
        if (g->program[PASS_BLOOM_OUTPUT]) {
            texture(g, TEX_SCENE, src->width, src->height, GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, NULL);
            texture(g, TEX_MASK, src->width, src->height, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, NULL);
            for (i = TEX_BRIGHT; i <= TEX_COMPOSITE; i++) {
                texture(g, i, src->width, src->height, GL_RGBA16F, GL_RGBA, GL_FLOAT, NULL);
                g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            }
        }
        g->sw = src->width; g->sh = src->height;
    }
    if (w != g->width || h != g->height) {
        texture(g, TEX_OUTPUT, w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        g->width = w; g->height = h;
    }
    g->BindTexture(GL_TEXTURE_2D, g->texture[TEX_SOURCE]);
    g->TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g->sw, g->sh, GL_RED_INTEGER, GL_UNSIGNED_SHORT, src->px);
    if (scene) {
        if (!bloom(g, scene, w, h)) return false;
        if (crt) {
            p = g->program[PASS_COMPOSITE]; g->UseProgram(p);
            if (!target(g, TEX_COMPOSITE, -1, g->sw, g->sh)) return false;
            composite_samplers(g, p, scene);
            g->DrawArrays(GL_TRIANGLES, 0, 3);
        }
    }
    if (crt) {
        if (g->profile_height != h || g->profile_source_height != g->sh) {
            float beams[Q2_CRT_PHASES][2];
            q2_crt_beam_table(g->sh, h, beams);
            texture(g, TEX_PROFILES, Q2_CRT_PHASES, 1, GL_RG32F, GL_RG, GL_FLOAT, beams);
            g->profile_height = h; g->profile_source_height = g->sh;
        }
        p = g->program[PASS_HORIZONTAL]; g->UseProgram(p);
        if (!target(g, TEX_BEAM, TEX_BLUR, g->sw, g->sh)) return false;
        sampler(g, p, "Source", 0, TEX_SOURCE); sampler(g, p, "Decode", 1, TEX_DECODE);
        sampler(g, p, "Filtered", 2, TEX_COMPOSITE);
        g->Uniform1i(g->GetUniformLocation(p, "UseFiltered"), scene != NULL);
        g->DrawArrays(GL_TRIANGLES, 0, 3);
        p = g->program[PASS_VERTICAL]; g->UseProgram(p);
        if (!target(g, TEX_LINEAR, -1, g->sw, g->sh)) return false;
        sampler(g, p, "Beam", 0, TEX_BEAM); sampler(g, p, "Blur", 1, TEX_BLUR);
        g->DrawArrays(GL_TRIANGLES, 0, 3);
        p = g->program[PASS_CRT]; g->UseProgram(p);
        sampler(g, p, "Linear", 0, TEX_LINEAR); sampler(g, p, "Profiles", 1, TEX_PROFILES);
        sampler(g, p, "Encode", 2, TEX_ENCODE);
    } else if (scene) {
        p = g->program[PASS_BLOOM_OUTPUT]; g->UseProgram(p);
        composite_samplers(g, p, scene);
    } else {
        p = g->program[PASS_NEAREST]; g->UseProgram(p);
        sampler(g, p, "Source", 0, TEX_SOURCE);
    }
    if (!target(g, TEX_OUTPUT, -1, w, h)) return false;
    g->Uniform2i(g->GetUniformLocation(p, "Size"), w, h);
    g->DrawArrays(GL_TRIANGLES, 0, 3);
    return g->GetError() == GL_NO_ERROR;
}

bool q2_gl_present(q2_gl_display *g, int ww, int wh, int x, int y)
{
    if (!current(g)) return false;
    g->BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    g->Viewport(0, 0, ww, wh);
    g->ClearColor(0, 0, 0, 1); g->Clear(GL_COLOR_BUFFER_BIT);
    g->BindFramebuffer(GL_READ_FRAMEBUFFER, g->fbo);
    g->BlitFramebuffer(0, 0, g->width, g->height,
                       x, wh-y, x+g->width, wh-y-g->height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    g->BindFramebuffer(GL_FRAMEBUFFER, 0);
    return g->GetError() == GL_NO_ERROR && SDL_GL_SwapWindow(g->window);
}

bool q2_gl_upload(q2_gl_display *g, const q2_crt *cpu)
{
    if (!g || !cpu || !cpu->pixels || cpu->width > g->max_texture ||
        cpu->height > g->max_texture || !current(g)) return false;
    g->ActiveTexture(GL_TEXTURE0);
    texture(g, TEX_OUTPUT, cpu->width, cpu->height, GL_RGBA8, GL_RGBA,
              GL_UNSIGNED_INT_8_8_8_8_REV, cpu->pixels);
    g->width = cpu->width; g->height = cpu->height;
    return target(g, TEX_OUTPUT, -1, g->width, g->height) && g->GetError() == GL_NO_ERROR;
}

bool q2_gl_read(q2_gl_display *g, u8 *rgba)
{
    if (!rgba || !current(g)) return false;
    if (!target(g, TEX_OUTPUT, -1, g->width, g->height)) return false;
    g->ReadPixels(0, 0, g->width, g->height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return g->GetError() == GL_NO_ERROR;
}

q2_result q2_gl_write_ppm(q2_gl_display *g, const char *path)
{
    u8 *pixels;
    FILE *f;
    size_t i, count;
    q2_result rc = Q2_OK;
    if (!g || !path || g->width < 1 || g->height < 1) return Q2_ERR_INVALID_ARG;
    count = (size_t)g->width*g->height;
    pixels = malloc(count*4);
    if (!pixels) return Q2_ERR_NO_MEMORY;
    if (!q2_gl_read(g, pixels)) { free(pixels); return Q2_ERR_IO; }
    /* Compact in place; PNG/PPM channel order is independent of host endian. */
    for (i = 0; i < count; i++) {
        pixels[i*3] = pixels[i*4]; pixels[i*3+1] = pixels[i*4+1]; pixels[i*3+2] = pixels[i*4+2];
    }
    f = fopen(path, "wb");
    if (!f) { free(pixels); return Q2_ERR_IO; }
    if (fprintf(f, "P6\n%d %d\n255\n", g->width, g->height) < 0 ||
        fwrite(pixels, 3, count, f) != count) rc = Q2_ERR_IO;
    if (fclose(f) != 0) rc = Q2_ERR_IO;
    free(pixels);
    return rc;
}

void q2_gl_finish(q2_gl_display *g) { if (current(g)) g->Finish(); }
