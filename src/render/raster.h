/*
 * raster.h — a software rasteriser that obeys the PlayStation GPU's rules.
 *
 * This is the reference backend. It is not the fast one, and it is not meant to
 * be: it exists so that "what should this frame look like?" has an answer that
 * does not depend on a GPU driver's interpretation of anything. A hardware
 * backend is judged against this.
 *
 * The rules it implements, and why each one matters, are set out in
 * docs/FIDELITY.md. In brief:
 *
 *   - Vertices arrive as integers from the GTE. Nothing is re-interpolated at
 *     higher precision here, so the characteristic wobble survives into pixels.
 *   - UVs interpolate LINEARLY IN SCREEN SPACE. There is no W and no
 *     perspective correction. This is the single easiest rule to break by
 *     accident and the most visible when broken.
 *   - There is no depth buffer. Primitives are drawn in ordering-table order and
 *     later primitives overwrite earlier ones, full stop.
 *   - The framebuffer is RGB555. Colour is dithered with the GPU's 4x4 ordered
 *     matrix before truncation.
 *   - Four blend modes, no arbitrary alpha.
 *
 * Quads are drawn as two triangles, and WHICH DIAGONAL they are cut along is a
 * fidelity rule rather than a free choice: UVs interpolate affinely per
 * triangle, so the diagonal decides which way the texture warps.
 *
 * A hardware packet's corners are a "Z" and the GPU splits them (0,1,2) and
 * (1,2,3) — sharing the edge between its second and third corners. The corners
 * reaching this backend are NOT in that order: they come from MapMod and from
 * the model bank in PERIMETER order, and both of the console's linkers convert
 * perimeter to Z on the way into the packet by exchanging corners 2 and 3
 * (0x800B2410 for models, 0x800AF844 for the world; the model emitter does the
 * same to the UVs at 0x8006A3E4). Undoing that mapping, the hardware's shared
 * edge in PERIMETER indices is 1—3, so a perimeter quad splits (0,1,3) and
 * (1,3,2). The vertex order within each triangle also selects the attribute
 * origin when leftmost X values tie, so reversing winding is not equivalent.
 * Fanning it (0,1,2)+(0,2,3) covers the same area but creases it along
 * the other diagonal, and every textured quad then warps the wrong way.
 *
 * Reading perimeter corners as though they were already "Z" is a third,
 * outright broken option: it drops a triangular quarter out of every surface.
 *
 * Both windings are rasterised. Backface rejection belongs to the game, which
 * does it with NCLIP before a primitive ever gets here (see world.c), exactly
 * as the original does.
 */
#ifndef Q2PSX_RASTER_H
#define Q2PSX_RASTER_H

#include "gpu.h"
#include "q2psx.h"

typedef struct psx_framebuffer {
    u16 *px;        /* RGB555, width*height                       */
    int  width;
    int  height;
    /* Optional native VRAM target. px remains a contiguous presentation copy;
     * raster writes update both, and binding imports the addressed rectangle. */
    psx_vram *vram;
    int vram_x, vram_y;
} psx_framebuffer;

q2_result psx_fb_init(psx_framebuffer *fb, int width, int height);
void      psx_fb_free(psx_framebuffer *fb);
void      psx_fb_clear(psx_framebuffer *fb, u16 colour);
q2_result psx_fb_bind_vram(psx_framebuffer *fb, psx_vram *vram, int x, int y);
void      psx_fb_store(psx_framebuffer *fb, int x, int y, u16 colour);

/* The texture cache is 256 four-word lines (2 KB), separate from the single
 * 256-entry CLUT. Tags store address+1 so a zero-initialized GPU is invalid. */
typedef struct psx_texture_cache {
    u32 tag[256];
    u16 word[256][4];
    u16 clut[256];
    u32 clut_tag;
    const psx_vram *owner;
    u32 transfer_serial;
} psx_texture_cache;

/* Rendering options. Every one of these defaults to the faithful behaviour;
 * turning any of them off is a deliberate departure from the original. */
typedef struct psx_raster_opts {
    bool dither;             /* 4x4 ordered dither before 15-bit truncation  */
    bool affine_uv;          /* reserved diagnostic flag; backend is affine */
    bool semi_transparency;  /* honour the four blend modes                  */
    bool textures;           /* sample VRAM — off draws flat/Gouraud only    */

    /*
     * The current draw environment's clip rectangle and drawing offset —
     * DRAWENV.clip and DRAWENV.ofs. On hardware these are not per-primitive
     * state but GP0 modes that a draw-env packet in the ordering table changes
     * partway through a frame, which is exactly how split screen is expressed:
     * one table, one walk, a different clip in force over each viewport's
     * slice. See src/screen.
     *
     * A clip of zero size means "the whole framebuffer", which is what
     * psx_raster_opts_default sets, so callers that do not care are unaffected.
     */
    s16 clip_x, clip_y, clip_w, clip_h;
    s16 ofs_x, ofs_y;
    u32 texture_window;      /* GP0 E2, low 20 bits                         */
    u8  mask_mode;           /* GP0 E6, force/check bit 15                   */
    bool native_coords;     /* decode signed 11-bit packet positions       */
    bool skip_field;        /* interlaced 480-line mode with DFE clear     */
    u8 field;               /* VRAM row parity currently being displayed   */
    bool missing_texture_bank; /* enabled E1.11 selects absent second MB   */
    psx_texture_cache *cache; /* command path supplies persistent GPU cache */

    /* Optional scene-only copy and UI coverage for GPU bloom. Same extent as
     * the framebuffer; both NULL when disabled. Transparent texels do not
     * cover the scene. No second geometry/texture rasterization is needed. */
    u16 *scene;
    u8  *ui_mask;
    bool ui;
} psx_raster_opts;

void psx_raster_opts_default(psx_raster_opts *opts);

/* GP0 E1/E2/E6 registers, shared by successive packets, viewports and frames.
 * Textured polygons update bits 0..8/11; E1 updates all fourteen mode bits.
 * Bit 11 changes only when GP1(09).0 enables the additional VRAM bank.
 * Flat polygons, lines and rectangles consume it without changing it. */
typedef struct psx_draw_state {
    u16 mode;
    u32 texture_window;
    u8  mask_mode;
    bool allow_texture_bank; /* GP1(09).0                                  */
    u8 display_mode;         /* GP1(08), DFE matters only for 480i          */
    u16 display_y;           /* GP1(05), used with field parity              */
    u8 field;
    psx_texture_cache cache;
} psx_draw_state;

/* Display-side controls used by the rasterizer; no CPU/GPU timing emulation. */
void psx_draw_state_gp1(psx_draw_state *state, u32 command);
void psx_draw_state_mode(psx_draw_state *state, u16 mode);
void psx_draw_state_clear_cache(psx_draw_state *state);

/* Execute a packet against persistent GPU state, then rasterize its pixels.
 * The direct psx_raster_prim API below takes an already-resolved page and is
 * stateless/uncached by default, for offline raster diagnostics. */
void psx_raster_command(psx_framebuffer *fb, const psx_prim *prim,
                        const psx_vram *vram, const psx_raster_opts *opts,
                        psx_draw_state *state);

/* Draw one primitive. `vram` may be NULL when textures are disabled. */
void psx_raster_prim(psx_framebuffer *fb,
                     const psx_prim *prim,
                     const psx_vram *vram,
                     const psx_raster_opts *opts);

/* Walk the ordering table far-to-near and draw everything in it. */
void psx_raster_ot(psx_framebuffer *fb,
                   psx_ot *ot,
                   const psx_vram *vram,
                   const psx_raster_opts *opts);

/* Write the framebuffer as a binary PPM. The port does not need this, but the
 * offline tools do: being able to render a zone to a file without a window is
 * what makes the geometry pipeline testable before there is a client. */
q2_result psx_fb_write_ppm(const psx_framebuffer *fb, const char *path);


/*
 * The GPU drops a primitive whose vertices are further apart than this — 1023
 * horizontally, 511 vertically. Named because it is a hardware rule with a
 * visible consequence rather than a clamp of convenience: it is what stops a
 * near-plane-clamped face, whose far vertex the GTE has saturated to the screen
 * coordinate limit, from being smeared across the frame. See psx_raster_prim.
 */
#define PSX_PRIM_MAX_SPAN_X 1023
#define PSX_PRIM_MAX_SPAN_Y 511

#endif /* Q2PSX_RASTER_H */
