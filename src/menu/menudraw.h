/*
 * menudraw.h — a menu page, drawn the way the console drew it.
 *
 * ---------------------------------------------------------------------------
 * What changed, and why it matters
 * ---------------------------------------------------------------------------
 * This module used to rasterise the page straight into a framebuffer with a
 * placeholder 5x7 face, because the menu's own letterforms had not been found.
 * They have been (menufont.h): the 8-pixel face is `chars.lbm` and the 16- and
 * 32-pixel faces are `frontend.lbm`, both already on the disc. So the page is
 * now built as PlayStation primitives in an ordering table and composed by the
 * same rasteriser the world goes through — which is not a tidiness argument. It
 * is the only way the menu can be *correct*: the glyphs are 4bpp CLUT sprites,
 * the selection bar is two semi-transparent gouraud quads, and the slider's fill
 * is a flat quad, and all three of those are things a framebuffer blitter with
 * its own font cannot express.
 *
 * ---------------------------------------------------------------------------
 * Where it lands in the ordering table — 0x8001AE34..0x8001AEC0
 * ---------------------------------------------------------------------------
 * The menu does not sort by depth. It names its bucket:
 *
 *     three or more viewports   that viewport's frontmost bucket,
 *                               db + 11192 + 204*p  ==  OT[2 + 51*p + 50]
 *     otherwise, size 16        db + 11836          ==  OT[213]
 *     otherwise                 db + 11828          ==  OT[211]
 *
 * 211 and 213 are inside the eleven-bucket overlay slice at OT[206..216], which
 * is the slice drawn after every viewport — so in one- and two-player sessions
 * the menu is genuinely an overlay, and in a three- or four-way split each
 * player gets their own copy inside their own viewport. Screen.h has the OT's
 * shape; the two constants below are that arithmetic done once.
 *
 * ---------------------------------------------------------------------------
 * What is on a page
 * ---------------------------------------------------------------------------
 *   the title          size 32, x = 256, y = (fb_h - 188)/2 + 10, always
 *                      "highlighted" so it takes palette 71 (0x8001CFAC)
 *   each item          size 16, centred on the record's own x and y
 *   the selection bar  two POLY_G4 with the semi-transparency bit, spanning the
 *                      viewport, bright in the middle, in the player's colour
 *                      (0x8001A7A8) — suppressed on a row whose text is
 *                      "PAUSED", which is how the page title escapes it
 *   a slider           a 133x8 frame with a fill ending at bar_x + value + 3
 *                      (0x8001BBB4, 0x8001BD28)
 *   the status line    the pause page's KILLS/SECRETS row at (256, 204)
 */
#ifndef Q2PSX_MENUDRAW_H
#define Q2PSX_MENUDRAW_H

#include "menu.h"
#include "menufont.h"
#include "gpu.h"
#include "raster.h"

/*
 * The two absolute buckets the overlay path uses, derived from the OT shape in
 * screen.h: base 10984, so (11828 - 10984)/4 and (11836 - 10984)/4.
 *
 * SCALED, because these are the CONSOLE's indices and the table holds
 * PSX_OT_SUBDIV real buckets per one (gpu.h). Handed to psx_ot_add_bucket
 * unscaled they no longer name the overlay slice at all — real bucket 211 sits
 * inside viewport 0's own range, which put the whole menu underneath the world
 * and let the title screen's logo draw straight through START and OPTIONS.
 */
#define Q2_MENU_OT_BUCKET       (211 * PSX_OT_SUBDIV)
#define Q2_MENU_OT_BUCKET_ITEM  (213 * PSX_OT_SUBDIV)

/* Viewport p's frontmost bucket, for a three- or four-way split. */
#define Q2_MENU_OT_BUCKET_VIEW(p)  ((2 + 51 * (p) + 50) * PSX_OT_SUBDIV)

/* ------------------------------------------------------------------------- */
/* The selection bar — 0x8001A7A8                                             */
/* ------------------------------------------------------------------------- */
/*
 * Four colours, chosen by bits 6..8 of the drawable's flags, which the page
 * setup fills from the per-player configuration block (0x8001CFF0). Each is a
 * gradient from a dark edge to a bright centre and back; the value 4 is the
 * sentinel that suppresses the bar entirely (`(flags & 0x1C0) == 0x100` at
 * 0x8001A828).
 *
 * The port exposes the index rather than the colour so a caller can say which
 * player is looking at the menu; single player is 0, which is blue.
 */
#define Q2_MENU_BAR_BLUE    0
#define Q2_MENU_BAR_RED     1
#define Q2_MENU_BAR_PURPLE  2
#define Q2_MENU_BAR_GREEN   3
#define Q2_MENU_BAR_NONE    4

/* The bright end of each; the dark end is the same hue at 32. */
extern const u8 q2_menu_bar_rgb[4][3];

/* ------------------------------------------------------------------------- */
typedef struct q2_menu_draw_opts {
    const q2_menu_font *font;

    /*
     * Everything the page draws goes in ONE bucket — the text (0x8001AE94),
     * the slider (0x8001BB90) and the selection bar (0x8001A910) all resolve
     * to db + 11836 in a one- or two-player session. What separates them is
     * insertion order, since the hardware walks a bucket's list most-recent
     * first: the page emits text, then the slider, then the bar, so the bar
     * draws first and ends up behind. This module keeps that order.
     */
    u32 bucket;

    int origin_x, origin_y;   /* the console's 512x248 inside a bigger surface */
    int view_x, view_w;       /* the bar's horizontal extent; 0 and 512        */

    int bar_colour;    /* Q2_MENU_BAR_*                                        */

    /* The backdrop the page is drawn over. The original draws the menu over
     * the frozen world with no backdrop of its own; a caller that wants one
     * asks for it here. Alpha 0 leaves the world visible. */
    u16 backdrop;
    int backdrop_alpha;
} q2_menu_draw_opts;

void q2_menu_draw_opts_default(q2_menu_draw_opts *o, const q2_menu_font *font);

/*
 * Build the current page into `ot`. Does nothing when the menu is closed.
 * Returns the number of primitives emitted.
 */
u32 q2_menu_build_ot(const q2_menu *m, psx_ot *ot,
                     const q2_menu_draw_opts *opts);

/*
 * One cell of the icon sheet — `qk_menu.lbm` and its two multiplayer variants
 * (menufont.h). The console draws these as POLY_GT4 at `0x80033320`, from a
 * four-byte {u, v, w, h} record and a position, so that is the interface: the
 * caller supplies the rect, because which rect belongs to what is a table in
 * the executable and which SCREEN draws them has not been followed.
 *
 * Returns the number of primitives emitted — 1, or 0 if the sheet is not
 * resident.
 */
u32 q2_menu_draw_icon(const q2_menu_font *font, psx_ot *ot, u32 bucket,
                      int x, int y, u8 u, u8 v, u8 w, u8 h);

#endif /* Q2PSX_MENUDRAW_H */
