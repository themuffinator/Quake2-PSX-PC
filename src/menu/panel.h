/*
 * panel.h — the light-blue rounded frame the UI screens sit inside.
 *
 * ---------------------------------------------------------------------------
 * What it is
 * ---------------------------------------------------------------------------
 * Every full-screen text panel — the mission briefing, MISSION COMPLETE, the
 * memory-card questions — is drawn inside the same rounded border with a
 * darkened translucent body behind it. openquestions.md listed that border as
 * unlocated for a long time, with the note that it is *not* in `qk_menu.lbm`.
 * It is in **`frontend.lbm`** (VRAM slot 13), in the strip below the 32-pixel
 * title face, at rows 223 to 255 — the part of the sheet the menu font never
 * reaches because its own tables stop at v = 100 + 11 * rows.
 *
 * Two functions build it:
 *
 *     0x8003F0E4   q2_panel_draw(x, y, w, h)  — the body, then the frame
 *     0x8003E8D0   the frame itself, eight POLY_FT4
 *
 * ---------------------------------------------------------------------------
 * The body
 * ---------------------------------------------------------------------------
 * Two TILE primitives, both exactly (x, y, w, h), both **rgb (0, 0, 0)** with
 * the semi-transparency bit set (`code |= 2` at `0x8003F1C8`), bracketed by two
 * draw-mode packets carrying `0xE1000600` — texture page 0, abr **0**, which on
 * this hardware is B/2 + F/2.
 *
 * Black at half strength darkens; drawing it **twice** darkens to a quarter.
 * That is the whole effect, and it is why the panel reads as smoked glass
 * rather than a flat fill: the scene behind it stays visible at 25%.
 *
 * ---------------------------------------------------------------------------
 * The frame — eight patches, four of them mirrored
 * ---------------------------------------------------------------------------
 * The sheet holds exactly three pieces of art:
 *
 *     (32, 224) 46 x 29   one corner
 *     (112, 247) 19 x 6   one horizontal edge
 *     (81, 241) 11 x 14   one vertical edge
 *
 * and the builder places eight quads from them. The mirroring is done the way
 * the hardware wants it — by **writing the screen corners in reverse order**
 * against ascending texture coordinates, not by flipping the UVs. The top-right
 * corner's x runs from `x + w + 10` down to `x + w - 36` while its u runs 32 up
 * to 78; that is the same instruction sequence as the top-left with two
 * expressions swapped, and it is why one corner of art draws four corners.
 *
 * Every quad is code **0x2D** — POLY_FT4 with the *raw* bit, so the texture is
 * not modulated by the primitive's colour, and without the ABE bit, so the
 * frame itself is opaque over the body it just darkened.
 *
 * The frame overhangs the rectangle it is given: 10 pixels left and right, 6
 * top and bottom. A caller that wants the border flush with a box has to inset
 * the box, and the console does exactly that — the briefing's box is (96, 32,
 * 336, 100) inside a 512-wide framebuffer, leaving 86 either side.
 *
 * ---------------------------------------------------------------------------
 * Ordering
 * ---------------------------------------------------------------------------
 * The body and the frame go into two different ordering-table slots 24 bytes
 * apart (`base + 11824` and `base + 11848`), which is six buckets. The port
 * takes the same shape as two bucket arguments rather than hard-coding the
 * gap, because this port's table is sliced differently (screen.h).
 */
#ifndef Q2PSX_MENU_PANEL_H
#define Q2PSX_MENU_PANEL_H

#include "menufont.h"
#include "gpu.h"
#include "q2psx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* The art, as it sits in frontend.lbm                                        */
/* ------------------------------------------------------------------------- */
#define Q2_PANEL_CORNER_U       32
#define Q2_PANEL_CORNER_V       224
#define Q2_PANEL_CORNER_W       46
#define Q2_PANEL_CORNER_H       29

#define Q2_PANEL_EDGE_H_U       112   /* the horizontal edge, stretched */
#define Q2_PANEL_EDGE_H_V       247
#define Q2_PANEL_EDGE_H_W       19
#define Q2_PANEL_EDGE_H_H       6

#define Q2_PANEL_EDGE_V_U       81    /* the vertical edge, stretched   */
#define Q2_PANEL_EDGE_V_V       241
#define Q2_PANEL_EDGE_V_W       11
#define Q2_PANEL_EDGE_V_H       14

/* How far the border reaches outside the rectangle it is handed. */
#define Q2_PANEL_OVERHANG_X     10
#define Q2_PANEL_OVERHANG_Y     6

/* Eight quads of frame, two tiles of body. */
#define Q2_PANEL_FRAME_PRIMS    8
#define Q2_PANEL_BODY_PRIMS     2

/* The body's colour and how many times it is laid down (0x8003F1B0..0x8003F218). */
#define Q2_PANEL_BODY_SHADE     0

/* ------------------------------------------------------------------------- */
/* A rectangle, in the framebuffer's own pixels                               */
/* ------------------------------------------------------------------------- */
typedef struct q2_panel_rect {
    s16 x, y, w, h;
} q2_panel_rect;

/*
 * The rectangle the console keeps at `0x800B29D8` — `q2_panel_draw` stores its
 * arguments there before drawing, and later code reads them back to place text
 * inside the panel it just drew. Callers that need that behaviour can use this;
 * `q2_panel_build_ot` writes it if `last` is non-NULL.
 */

/*
 * Draw one panel. `body_bucket` takes the two darkening tiles, `frame_bucket`
 * the eight border quads; on the console the frame is the nearer of the two.
 *
 * `font` supplies the texture page and palette: the border shares both with the
 * menu's 16- and 32-pixel faces, which is what identifies the sheet — the CLUT
 * the frame builder reads at `0x800E3FB4` is the same halfword the glyph
 * emitter reads at `0x8001B058`.
 *
 * Returns the number of primitives emitted, so a caller can tell a full panel
 * from one the ordering table had no room for.
 */
u32 q2_panel_build_ot(const q2_menu_font *font, const q2_panel_rect *r,
                      psx_ot *ot, u32 body_bucket, u32 frame_bucket,
                      q2_panel_rect *last);

/* The frame alone, for a caller that has already darkened the area. */
u32 q2_panel_frame_ot(const q2_menu_font *font, const q2_panel_rect *r,
                      psx_ot *ot, u32 bucket);

/* The body alone. */
u32 q2_panel_body_ot(const q2_panel_rect *r, psx_ot *ot, u32 bucket);

/*
 * The outer bounds the frame actually covers, overhang included. Useful for
 * deciding whether a panel fits, and for the tests.
 */
void q2_panel_bounds(const q2_panel_rect *r, q2_panel_rect *out);

#ifdef __cplusplus
}
#endif

#endif /* Q2PSX_MENU_PANEL_H */
