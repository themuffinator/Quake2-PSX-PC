/*
 * menufont.h — the menu's own letterforms, and how a string becomes primitives.
 *
 * ---------------------------------------------------------------------------
 * Where the menu font was, and why it took so long to find
 * ---------------------------------------------------------------------------
 * The menu was reconstructed page by page from tables in the executable long
 * before its face was: the pages, the coordinates, the widgets and the
 * navigation are all data, and none of that data says what a letter looks like.
 * The port drew the real layout with a placeholder 5x7 face and said so.
 *
 * The face is in two files, and the answer is in one function. The text drawer
 * at `0x8001ACDC` supports three sizes — 8, 16 and 32 — and branches on the
 * drawable's own `size` field at `+0x46`:
 *
 *     size 8    tpage[15], the glyph table at 0x8009D554
 *               -> `chars.lbm`, the SAME atlas the HUD draws from
 *     size 16   tpage[13], cell 16 x 11, 15 columns per row, v origin 100
 *     size 32   tpage[13], cell 32 x 20,  7 columns per row, v origin 0
 *
 * and tpage[13] is `frontend.lbm`, registered at `0x8003FE74` into VRAM slot 13
 * — (896, 256), a whole 4bpp texture page — by the image-registration function
 * at `0x8003FE20`. Both files are on the disc in every playable map's
 * `SNDVRAM.DAT`, which is why nothing had to be invented: this module ships no
 * artwork, only the coordinates at which to sample it.
 *
 * ---------------------------------------------------------------------------
 * How a character becomes a cell — 0x8001B494
 * ---------------------------------------------------------------------------
 * The 16- and 32-pixel faces are not indexed by a table. They are laid out in
 * rows of `cols` letters and the locator computes the cell:
 *
 *     'A'..'Z'      row = (c - 'A') / cols,  col = (c - 'A') % cols
 *     '0'..'9'      row = 3,                 col = c - '0'
 *     punctuation   row = 2,                 col from a jump table at 0x800AB564
 *
 * with four overrides applied *before* any of that, and only at size 32, where
 * `cols` is 7 and rows 0..2 are already full of letters:
 *
 *     '2' -> (5,3)   '3' -> (6,3)   '4' -> (0,4)   '?' -> (1,4)
 *
 * so the big face carries A..Z, four digits and a question mark and nothing
 * else. The titles it draws — PAUSED, OPTIONS, PLAYER, SOUND, VIDEO,
 * CONTROLLER, POSITION — need no more than that.
 *
 * A CHARACTER WITH NO CELL IS NOT A BLANK. The locator's default arm at
 * `0x8001B668` returns without writing the column at all, so the caller's
 * output variable keeps the *previous* glyph's already-scaled u and the
 * multiply happens a second time. That is a real quirk of the original and it
 * is reproduced here — see `q2_menu_glyph`'s `col` in/out parameter — because a
 * port that silently substituted a space would draw a different screen than the
 * console for any string that ever hit it. None of the shipped labels do.
 *
 * ---------------------------------------------------------------------------
 * Colours
 * ---------------------------------------------------------------------------
 * Glyphs are 4bpp sprites out of the executable's own palette bank (hudtables.h
 * §"Palettes"), modulated by a flat grey:
 *
 *     normal   (128,128,128)   unity for a modulated primitive
 *     greyed   ( 32, 32, 32)   the `g` code, 0x8001B14C
 *
 * so the colour of a menu row is a *palette*, not an rgb, and the four codes
 * `b`/`d`/`g`/`u` select between palettes and modulation rather than between
 * arbitrary colours. Which palette:
 *
 *     size 8                                 72   (the HUD's font palette)
 *     size 16, drawable's +0x48 clear        68
 *     size 16, +0x48 set, `d` seen           68
 *     size 16, +0x48 set, no `d`             70
 *     size 32, +0x48 clear                   68
 *     size 32, +0x48 set                     71
 *
 * `+0x48` is the drawable's *highlight* flag: the engine sets it on the row
 * under the cursor and on the page title, and clears it on everything else.
 *
 * ---------------------------------------------------------------------------
 * Geometry
 * ---------------------------------------------------------------------------
 * Every glyph is a POLY_FT4 (code 0x2C, 40 bytes, `0x8001B0A8` onward), not a
 * sprite, and it is drawn 1:1 — the quad is `cell_w` x `cell_h` on screen and
 * `cell_w` x `cell_h` in the page. A string is centred on the drawable's x:
 *
 *     x0 = obj.x - (printable_length * advance) / 2      0x8001AE30
 *     y0 = obj.y - cell_h / 2                            0x8001AE1C
 *
 * and the pen advances by `advance` — which equals the face size, so 16 for the
 * 16-pixel face even though its cell is only 11 tall.
 *
 * The `u` code adds a SECOND quad per character, eight pixels lower, sampling
 * the cell at (0, 2 * cell_h + 100) — for the 16-pixel face that is the '-' at
 * the start of the punctuation row, which is how an underline is drawn without
 * a rule primitive. It is emitted for spaces too, so an underlined run is
 * continuous.
 */
#ifndef Q2PSX_MENUFONT_H
#define Q2PSX_MENUFONT_H

#include "gpu.h"
#include "hudtables.h"
#include "q2psx.h"
#include "vram.h"

/* ------------------------------------------------------------------------- */
/* The two atlases                                                            */
/* ------------------------------------------------------------------------- */
#define Q2_MENU_ATLAS_NAME    "frontend.lbm"
#define Q2_MENU_ATLAS_SLOT    13     /* 0x8003FE74 */
#define Q2_MENU_ATLAS_V_OFS    0
#define Q2_MENU_ATLAS_PAGE_X  14     /* 896 halfwords / 64 */
#define Q2_MENU_ATLAS_PAGE_Y   1     /* y 256              */

/*
 * The sheet registered immediately after the font, into slot 14 — and it is
 * NOT a menu frame, which is what its name and its position invite you to
 * assume. Decoding it settles the question: it is a grid of **32 x 24 item and
 * weapon icons** followed by a set of **large digits**, and the digits are the
 * interesting part, because the HUD reconstruction's headline finding is that
 * this game has no status bar (hud.h). It does not — but something in the image
 * draws numerals at this size, and this is where they live.
 *
 * What is established about it:
 *
 *   - three files, chosen by session mode at 0x8003FEAC: `qk_menu.lbm` in
 *     single player, `qk2_menu.lbm` for two players, `qkm_menu.lbm` for three
 *     or more (the player count is at 0x800B3356)
 *   - it lands at VRAM (960, 256), a whole 4bpp page, tpage[14]
 *   - `0x80033320` draws one cell from it as a POLY_GT4, taking a four-byte
 *     {u, v, w, h} record and a position
 *   - the icon rects are a five-byte table at `0x8009C478` — {u, v, w, h, id},
 *     w = 32 and h = 24 on every entry but the first, which is the 1 x 1 blank
 *   - `0x80035EA0` and `0x80035B38` are the two callers, and they sit inside a
 *     composite draw that runs five sub-draws in a row (`0x800352C0`,
 *     `0x80035554`, `0x80035B38`, `0x800359C0`, `0x80035EA0`)
 *
 * What is NOT established, and is deliberately not guessed: which screen that
 * composite belongs to. It is reached through a pointer rather than a direct
 * call, so the function sweep does not name it. Until that is followed, this
 * module uploads the sheet — because the same registration function does — and
 * exposes a way to draw one cell, and claims nothing about when.
 */
#define Q2_MENU_ICONS_SLOT     14
#define Q2_MENU_ICONS_V_OFS     0
#define Q2_MENU_ICONS_PAGE_X   15
#define Q2_MENU_ICONS_PAGE_Y    1
#define Q2_MENU_ICONS_NAME_1P  "qk_menu.lbm"
#define Q2_MENU_ICONS_NAME_2P  "qk2_menu.lbm"
#define Q2_MENU_ICONS_NAME_MP  "qkm_menu.lbm"

/* Which of the three a session uses (0x8003FEAC). */
const char *q2_menu_icons_name(bool multiplayer, int players);

/*
 * QFRONT's arena-preview sheets. The renderer at module+0x2AD4 chooses slot 8
 * for arenas 0..9 and slot 12 for 10..11. Their palettes are deliberately NOT
 * the per-image CLUT8 blocks in SNDVRAM: the calls pass built-in palette ids 0
 * and 3 to the shared textured-quad helper. Id 0 is the executable's exceptional
 * 256-entry palette at (0,255); id 3 starts one of the packed 256-entry rows
 * formed by the sixteen-entry built-in palettes.
 */
#define Q2_MENU_ARENA_SHEETS       2
#define Q2_MENU_ARENA_NAME_0       "multipics.lbm"
#define Q2_MENU_ARENA_NAME_1       "multipic2.lbm"
#define Q2_MENU_ARENA_SLOT_0       8
#define Q2_MENU_ARENA_SLOT_1      12
#define Q2_MENU_ARENA_PAGE_X_0     9    /* slot x 576 / 64 */
#define Q2_MENU_ARENA_PAGE_X_1    13    /* slot x 832 / 64 */
#define Q2_MENU_ARENA_PAGE_Y       1
#define Q2_MENU_ARENA_PALETTE_1    3

/* ------------------------------------------------------------------------- */
/* The three faces                                                            */
/* ------------------------------------------------------------------------- */
#define Q2_MENU_FACE_SMALL   8    /* chars.lbm, the HUD atlas                */
#define Q2_MENU_FACE_ITEM   16    /* frontend.lbm, what every item is drawn at */
#define Q2_MENU_FACE_TITLE  32    /* frontend.lbm, the page title            */

/* Built-in palette ids the menu names. 72 is the HUD's; the other three are
 * the menu's alone. */
#define Q2_MENU_PALETTE_TEXT      68
#define Q2_MENU_PALETTE_ITEM_HI   70
#define Q2_MENU_PALETTE_TITLE_HI  71
#define Q2_MENU_PALETTE_SMALL     72

/* The two modulations, 0x8001B14C. */
#define Q2_MENU_MOD_NORMAL 128
#define Q2_MENU_MOD_GREY    32

typedef struct q2_menu_face {
    u8 cell_w, cell_h;   /* the quad, on screen and in the page             */
    u8 cols;             /* letters per atlas row                           */
    u8 v_origin;         /* added to row * cell_h                           */
    u8 advance;          /* the pen step — equal to the face size           */
} q2_menu_face;

/* The metrics for one of the three sizes, or NULL. */
const q2_menu_face *q2_menu_face_get(int size);

/* ------------------------------------------------------------------------- */
/* Residency                                                                  */
/* ------------------------------------------------------------------------- */
typedef struct q2_menu_font {
    const q2_hud_tables *tab;    /* the 8-pixel glyph table and the palettes */

    u16  tpage_item;             /* frontend.lbm — sizes 16 and 32           */
    u16  tpage_small;            /* chars.lbm    — size 8                    */
    u16  clut_text;              /* 68                                       */
    u16  clut_item_hi;           /* 70                                       */
    u16  clut_title_hi;          /* 71                                       */
    u16  clut_small;             /* 72                                       */

    /* The icon sheet, when the map ships one. Sizes are in texels. */
    u16  tpage_icons;
    u16  icons_w, icons_h;
    bool icons_resident;

    /* QFRONT's two 8bpp arena-thumbnail sheets. */
    u16  arena_tpage[Q2_MENU_ARENA_SHEETS];
    u16  arena_clut[Q2_MENU_ARENA_SHEETS];
    bool arena_resident[Q2_MENU_ARENA_SHEETS];

    bool item_resident;          /* three maps carry no frontend.lbm         */
    bool small_resident;         /* two carry no chars.lbm                   */
} q2_menu_font;

/*
 * Upload both arena-preview sheets, both font atlases, the icon sheet, and the
 * executable's palette bank into `vram` at the addresses and in the order the
 * console used. The order is observable because preview slot 12 aliases the
 * font's slot 13; see menufont.c. Missing images are recorded rather than
 * treated as failures; the result is Q2_OK whenever either font atlas was
 * placed and Q2_ERR_NOT_FOUND when the map carries neither one.
 */
q2_result q2_menu_font_upload(q2_menu_font *out, const q2_hud_tables *tab,
                              const q2_vram_section *section, psx_vram *vram,
                              bool multiplayer, int players);

/* ------------------------------------------------------------------------- */
/* Locating a glyph                                                           */
/* ------------------------------------------------------------------------- */
/*
 * 0x8001B494 for the 16- and 32-pixel faces, and the table at 0x8009D554 for
 * the 8-pixel one. `u` is in/out: the original leaves it untouched for a
 * character it does not know, so a caller passing the previous glyph's u
 * reproduces the original's behaviour exactly. Returns false only for a size
 * that is not one of the three.
 */
bool q2_menu_glyph(const q2_menu_font *font, int size, char c, u8 *u, u8 *v);

/* Is there a cell for `c` at `size`? False means the original would have drawn
 * whatever the pen last sampled. Useful for checking, not for drawing. */
bool q2_menu_glyph_defined(int size, char c);

/* ------------------------------------------------------------------------- */
/* Drawing                                                                    */
/* ------------------------------------------------------------------------- */
/*
 * One string, centred on (cx, cy) in the console's 512 x 248, emitted into an
 * ABSOLUTE ordering-table bucket — because that is what the original does: the
 * menu names its bucket outright rather than deriving one from a depth (see
 * menudraw.h for which). Colour codes are honoured exactly as 0x8001AEF8 does:
 * `b` clears the
 * dim flag, `d` sets it, `g` sets both dim and grey, `u` turns the underline
 * pass on, and none of the four is printed.
 *
 * `highlight` is the drawable's +0x48 — set on the cursor row and the title.
 * `origin_x`/`origin_y` translate the console's space onto a larger surface,
 * the same way q2_hud_ctx does; the layout itself is never scaled, because an
 * 8 x 8 texel cell drawn at any other size would need filtering the rest of
 * the pipeline does not have.
 *
 * Returns the number of primitives emitted.
 */
u32 q2_menu_font_print(const q2_menu_font *font, psx_ot *ot, u32 bucket,
                       int size, int cx, int cy, bool highlight,
                       int origin_x, int origin_y, const char *text);

/* The pixel width of a string at `size`, colour codes excluded — the printable
 * length from 0x8001FD18 times the face's advance. */
int q2_menu_font_width(int size, const char *text);

#endif /* Q2PSX_MENUFONT_H */
