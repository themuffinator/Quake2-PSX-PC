#include "loading.h"

#include "menudraw.h"
#include "worldscale.h"

#include <stdlib.h>
#include <string.h>

/* Point the shadow menu at one of the two screens. Both are pure-text pages
 * with nothing navigable, so the cursor is only ever the terminator. */
static void loading_set_page(q2_loading *l, q2_loading_page page)
{
    const q2_menu_page *p =
        q2_menu_page_find(page == Q2_LOADING_PAGE_STARTING
                              ? Q2_PAGE_STARTING : Q2_PAGE_LOADING);

    if (l->menu.page == p)
        return;

    l->menu.page   = p;
    l->menu.cursor = p ? (int)p->first : 0;
    l->menu.open   = (p != NULL);
}

q2_result q2_loading_open(q2_loading *l, const disc *d,
                          const q2_hud_tables *tab,
                          q2_menu_settings *settings)
{
    q2_vram_section vs;
    q2_result r;

    if (!l || !d || !tab)
        return Q2_ERR_INVALID_ARG;

    memset(l, 0, sizeof(*l));

    if (q2_vram_load(&vs, d, Q2_LOADING_MAP) != Q2_OK)
        return Q2_ERR_NOT_FOUND;

    l->vram = (psx_vram *)calloc(1, sizeof(psx_vram));
    if (!l->vram) {
        q2_vram_free(&vs);
        return Q2_ERR_NO_MEMORY;
    }

    /*
     * The bank into an image of this screen's own, then the atlas out of it.
     *
     * Single player, one player: QDUMMY carries no icon sheet in any of the
     * three flavours, and this screen draws no status bar to want one. The
     * upload reports Q2_OK on the strength of `FrontEnd.lbm` alone, which is
     * the whole of what this screen needs — the word and the logo strip are
     * both cut from it, which is why `item_resident` is the thing checked.
     */
    (void)q2_vram_upload(&vs, l->vram);
    r = q2_menu_font_upload(&l->font, tab, &vs, l->vram, false, 1);
    q2_vram_free(&vs);

    if (r != Q2_OK || !l->font.item_resident) {
        free(l->vram);
        l->vram = NULL;
        return Q2_ERR_NOT_FOUND;
    }
    l->font_ready = true;

    q2_menu_init(&l->menu, settings, Q2_MENU_SCREEN_H);
    loading_set_page(l, Q2_LOADING_PAGE_LOADING);

    l->ready = true;
    return Q2_OK;
}

void q2_loading_close(q2_loading *l)
{
    if (!l)
        return;
    free(l->vram);
    memset(l, 0, sizeof(*l));
}

void q2_loading_raise(q2_loading *l)
{
    if (!l || !l->ready)
        return;

    loading_set_page(l, Q2_LOADING_PAGE_LOADING);

    /*
     * The hold RESTARTS rather than accumulating. A transition that loads twice
     * — a level change whose arrival lands in another zone — is one screen to
     * the player, and adding the two would make it linger for a second.
     */
    l->hold  = (double)Q2_LOADING_HOLD_UNITS;
    l->timed = true;
    l->open  = true;
}

void q2_loading_show(q2_loading *l, q2_loading_page page)
{
    if (!l || !l->ready)
        return;

    loading_set_page(l, page);
    l->hold  = 0.0;
    l->timed = false;
    l->open  = true;
}

void q2_loading_hide(q2_loading *l)
{
    if (!l)
        return;
    l->open  = false;
    l->timed = false;
    l->hold  = 0.0;
}

bool q2_loading_step(q2_loading *l, double dt)
{
    const double cycle =
        (double)(Q2_LOADING_CELLS * Q2_LOADING_CELL_UNITS);

    if (!l || !l->open)
        return false;

    /*
     * THE HOLD IS A MINIMUM, so the frame that exhausts it is still the
     * screen's and the one after it is the first the world gets back.
     *
     * Testing before the subtract rather than after is the whole of that.
     * After, the last frame's remainder was spent on a frame that then drew the
     * level, which made half a second fourteen frames of screen at the headless
     * 1/30 s step instead of fifteen — 0.467 s for a floor of 0.5. It also
     * keeps `open` in step with what this returns, which matters because the
     * frame loop reads the return and `client_frame` reads the flag.
     */
    if (l->timed && l->hold <= 0.0) {
        q2_loading_hide(l);
        return false;
    }

    /* The strip runs on the level clock whoever is holding the screen up, so a
     * screen the caller is timing turns at the same rate; only the countdown
     * belongs to one of them. */
    l->spin += dt * (double)Q2_DT_HZ;
    while (l->spin >= cycle)
        l->spin -= cycle;

    if (!l->timed)
        return false;

    l->hold -= dt * (double)Q2_DT_HZ;
    return true;
}

u32 q2_loading_cell(const q2_loading *l)
{
    u32 cell;

    if (!l || l->spin < 0.0)
        return 0;

    cell = (u32)(l->spin / (double)Q2_LOADING_CELL_UNITS);
    return cell < Q2_LOADING_CELLS ? cell : Q2_LOADING_CELLS - 1;
}

/*
 * The logo, as one textured quad off the menu's own atlas.
 *
 * A POLY_FT4 rather than the gouraud quad `q2_menu_draw_icon` emits, because
 * nothing modulates this: the strip is already the colour it should be.
 * `Q2_MENU_MOD_NORMAL` on all four corners is unity for a modulated primitive,
 * which is the same thing said the way the hardware says it (menufont.h).
 */
static u32 loading_draw_logo(const q2_loading *l, psx_ot *ot,
                             int origin_x, int origin_y)
{
    psx_prim *p;
    u32 cell = q2_loading_cell(l);
    u8  u = (u8)((cell % Q2_LOADING_CELL_COLS) * Q2_LOADING_CELL_W);
    u8  v = (u8)(Q2_LOADING_CELL_V +
                 (cell / Q2_LOADING_CELL_COLS) * Q2_LOADING_CELL_H);
    int x = origin_x + Q2_LOADING_X;
    int y = origin_y + Q2_LOADING_Y;
    int i;

    p = psx_ot_add_bucket(ot, Q2_LOADING_OT_BUCKET);
    if (!p)
        return 0;

    p->kind  = PSX_PRIM_FT4;
    p->tpage = l->font.tpage_item;
    /*
     * PALETTE 68, THE STRIP'S OWN RAMP — and this is the one thing on this
     * screen that is a choice rather than a reading.
     *
     * The sprite is authored FOR 68. Its sixty rows use exactly sixteen
     * colours and they are 68's sixteen entries, expanded from 5-bit:
     * `000000 002942 083152 ... ADE7E7 C6F7F7` against the bank's
     * `000000 002840 083050 ... A8E0E0 C0F0F0`. So the ramp it was drawn
     * against is the ramp it is drawn through, which is what any other sprite
     * cut from this sheet gets.
     *
     * The alternative was tried and is worse. Palette 70 — which hudtables.h
     * calls "a white-only mask palette the menu uses", and which the bank
     * confirms is 0xF8F8F8 at indices 8..11 and black at the other twelve —
     * turns a sixteen-level ramp into four, and the logo comes out as a
     * stippled fragment rather than an outline. It is right for a GLYPH, whose
     * art sits in that band, and wrong for this.
     *
     * WHAT WOULD SETTLE IT is the code that binds the CLUT for this quad, and
     * that has not been found: the strip is data with no located reader. If a
     * capture ever shows the logo lighter than the word beside it, the answer
     * is a modulation on this primitive rather than another palette — there
     * is no third white ramp in the bank.
     */
    p->clut  = l->font.clut_text;
    p->textured_blend = true;

    p->xy[0].x = (s16)x;
    p->xy[0].y = (s16)y;
    p->xy[1].x = (s16)(x + Q2_LOADING_CELL_W);
    p->xy[1].y = (s16)y;
    p->xy[2].x = (s16)(x + Q2_LOADING_CELL_W);
    p->xy[2].y = (s16)(y + Q2_LOADING_CELL_H);
    p->xy[3].x = (s16)x;
    p->xy[3].y = (s16)(y + Q2_LOADING_CELL_H);

    p->uv[0].u = u;
    p->uv[0].v = v;
    p->uv[1].u = (u8)(u + Q2_LOADING_CELL_W);
    p->uv[1].v = v;
    p->uv[2].u = (u8)(u + Q2_LOADING_CELL_W);
    p->uv[2].v = (u8)(v + Q2_LOADING_CELL_H);
    p->uv[3].u = u;
    p->uv[3].v = (u8)(v + Q2_LOADING_CELL_H);

    for (i = 0; i < 4; i++) {
        p->rgb[i].r = Q2_MENU_MOD_NORMAL;
        p->rgb[i].g = Q2_MENU_MOD_NORMAL;
        p->rgb[i].b = Q2_MENU_MOD_NORMAL;
    }
    return 1;
}

u32 q2_loading_build_ot(const q2_loading *l, psx_ot *ot, int width, int height)
{
    q2_menu_draw_opts mo;
    int origin_x, origin_y;
    u32 n = 0;

    if (!l || !l->open || !ot || !l->font_ready)
        return 0;

    /* The console's 512 x 248 block, centred in whatever this buffer is — the
     * same placement the session's own menu gets (client_menu_origin). */
    origin_x = (width  - Q2_MENU_SCREEN_W) / 2;
    origin_y = (height - Q2_MENU_SCREEN_H) / 2;

    n += loading_draw_logo(l, ot, origin_x, origin_y);

    /*
     * And the page over it, through the same builder every other page goes
     * through — this one IS a menu page (0x800A3314), so it gets the menu's
     * font, the menu's centring and the menu's bucket.
     */
    if (l->menu.page) {
        q2_menu_draw_opts_default(&mo, &l->font);
        mo.origin_x = origin_x;
        mo.origin_y = origin_y;
        mo.view_x   = 0;
        mo.view_w   = width < Q2_MENU_SCREEN_W ? width : Q2_MENU_SCREEN_W;

        /*
         * AND THE WORD IS WHITE ON THE ENGINE'S PAGE AND BLUE ON THE MODULE'S.
         *
         * `0x80079398` sets drawable 0's `+0x48` right after installing the
         * LOADING record, so that page is highlighted and takes palette 70.
         * The front end's STARTING / GAME is built by the ordinary page builder
         * and sets nothing, so it stays on 68 — which is what the capture
         * shows: blue rows under a white logo.
         */
        mo.highlight_all = (l->menu.page == q2_menu_page_find(Q2_PAGE_LOADING));

        n += q2_menu_build_ot(&l->menu, ot, &mo);
    }

    return n;
}
