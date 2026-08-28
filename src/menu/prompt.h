/*
 * prompt.h — the button prompts along the bottom of the front end.
 *
 * ---------------------------------------------------------------------------
 * What they are
 * ---------------------------------------------------------------------------
 * `△ BACK`, `□ RULES` and `✕ SELECT`. openquestions.md #42 had both halves of
 * this open — "neither the glyph source nor the placement is located". Both are
 * located, and neither is where a search for the words would look: **the
 * prompts are not text**. There is no "BACK" string in the executable. Each
 * prompt is one 76 x 16 pre-rendered cell in `frontend.lbm`, PlayStation glyph
 * and word together, and the whole thing is a single textured quad.
 *
 * That is why the string sweep never found them, and it is the same shape of
 * mistake §11.1 made about the status bar: enumerating text cannot see art.
 *
 * ---------------------------------------------------------------------------
 * The table — 0x8009B4D8, three records of 18 bytes
 * ---------------------------------------------------------------------------
 *     +0   u16  u          atlas column
 *     +2   u16  v          atlas row
 *     +4   u16  w          76 for all three
 *     +6   u16  h          16 for all three
 *     +8   u16  centre     x + 38; the drawer never reads it
 *     +12  s16  y_target   where the prompt is sliding TO
 *     +14  s16  x          where it is drawn
 *     +16  s16  y          where it is now — animated towards +12
 *
 *     i  u    v    x    what
 *     0  96   224  346  X SELECT
 *     1  176  213  90   triangle BACK
 *     2  176  235  218  square RULES
 *
 * The three x values are 128 apart and each cell is 76 wide, so the centres land
 * on 128, 256 and 384 — the quarter marks of the 512-pixel framebuffer. Left to
 * right the reader sees BACK, RULES, SELECT, which is the order the capture
 * shows and NOT the order the records are stored in.
 *
 * ---------------------------------------------------------------------------
 * They slide, and that is why there are two y fields
 * ---------------------------------------------------------------------------
 * `0x8001C34C` steps the current y towards the target by at most **3 pixels a
 * frame**, in either direction, then draws the quad wherever it has got to. A
 * port that stored one y and set it directly would look right in a screenshot
 * and wrong in motion — the prompts rise into view when a screen opens and drop
 * out when it closes.
 *
 * The setter is `0x8001F938(index, y)`, and it stores **y - 8**, not y. That
 * bias is in the setter, not the caller, so every caller passes the number it
 * means and the table holds the number the drawer wants.
 *
 * `q2_menu_open` calls it for all three with y = 260 — target 252, which is
 * below the 248-line screen, so **opening any page starts the prompts leaving**.
 * A screen that wants a prompt has to ask for it again. On page 46, and only
 * page 46, the open also copies the target straight into the current y
 * (`0x8001A418`), so the front end's first screen starts with them already gone
 * rather than sliding away from a position they never occupied.
 *
 * ---------------------------------------------------------------------------
 * The ordinary-menu rules
 * ---------------------------------------------------------------------------
 * The core loop at 0x8001A280..0x8001A348 asks for SELECT when the current
 * object's +0x4C action is non-NULL, and BACK when the page's +0x28C back
 * handler is non-NULL. QFRONT uses caller y = 216; an in-game menu uses 220.
 * QFRONT's multiplayer hook at module+0x459C adds RULES at y = 220 while one
 * of the first three mode rows is selected, and parks it on the two settings
 * rows. `q2_prompt_sync_menu` is that per-frame policy in port terms.
 *
 * The one unrelated placement special case is still in the retail drawer: on
 * page 11, record 1 is drawn at a hard-coded (230, 114) rather than from the
 * table. Page 11 is not one of the ordinary menu pages reconstructed here.
 */
#ifndef Q2PSX_MENU_PROMPT_H
#define Q2PSX_MENU_PROMPT_H

#include "menufont.h"
#include "menu.h"
#include "gpu.h"
#include "q2psx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define Q2_PROMPT_COUNT         3
#define Q2_PROMPT_W             76
#define Q2_PROMPT_H             16

/* The setter's bias, and the y that parks a prompt off the bottom. */
#define Q2_PROMPT_Y_BIAS        8
#define Q2_PROMPT_Y_HIDDEN      252

/* At most this many pixels of travel per frame, either way (0x8001C3D0). */
#define Q2_PROMPT_SPEED         3

/* Indices into the table, in storage order — not left-to-right order. */
typedef enum q2_prompt_id {
    Q2_PROMPT_SELECT = 0,   /* X SELECT        */
    Q2_PROMPT_BACK   = 1,   /* triangle BACK   */
    Q2_PROMPT_RULES  = 2    /* square RULES    */
} q2_prompt_id;

typedef struct q2_prompt_rec {
    u16 u, v;          /* +0, +2  — the cell in frontend.lbm  */
    u16 w, h;          /* +4, +6  — 76 x 16                   */
    u16 centre;        /* +8      — x + 38, unread by the drawer */
    s16 y_target;      /* +12                                 */
    s16 x;             /* +14                                 */
    s16 y;             /* +16                                 */
} q2_prompt_rec;

typedef struct q2_prompt_bar {
    q2_prompt_rec rec[Q2_PROMPT_COUNT];
} q2_prompt_bar;

/* The shipped table, verbatim. */
extern const q2_prompt_rec q2_prompt_table[Q2_PROMPT_COUNT];

/* Reset to the shipped state: parked at y = 252, targets there too. */
void q2_prompt_init(q2_prompt_bar *b);

/*
 * Ask for a prompt at `y` — the same call the console makes through
 * `0x8001F938`, bias included, so callers pass the y they mean.
 */
void q2_prompt_show(q2_prompt_bar *b, q2_prompt_id id, int y);

/* Park every prompt, as opening a page does. */
void q2_prompt_hide_all(q2_prompt_bar *b);

/* Snap the current y to the target, as opening page 46 does. */
void q2_prompt_snap(q2_prompt_bar *b);

/* One frame of travel, at most Q2_PROMPT_SPEED pixels each. */
void q2_prompt_step(q2_prompt_bar *b);

/*
 * Apply the retail ordinary-menu prompt policy for this frame. A closed or
 * incomplete menu parks the whole bar. `front_end` selects QFRONT's y = 216;
 * in-game pages use y = 220. This changes targets only — `q2_prompt_step`
 * remains the sole owner of the three-pixel slide.
 */
void q2_prompt_sync_menu(q2_prompt_bar *b, const q2_menu *m, bool front_end);

/*
 * Draw whatever is currently on screen. Steps nothing — call q2_prompt_step
 * once a frame and this as often as you like.
 *
 * Returns the number of quads emitted. Prompts fully off the bottom still emit;
 * so does the console, and the clip is the drawing rectangle's job.
 */
u32 q2_prompt_build_ot(const q2_prompt_bar *b, const q2_menu_font *font,
                       psx_ot *ot, u32 bucket);

#ifdef __cplusplus
}
#endif

#endif /* Q2PSX_MENU_PROMPT_H */
