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
     * AND THE STRIP IS LEFT ALONE, which is the opposite of what this used to
     * do and of what the comment here used to argue.
     *
     * The argument was that every screen is a new one — 0x80079364 enters page
     * 46 and 0x80079374 / 0x80079384 re-install both records from scratch at
     * every transition — so the strip should restart with it. The PAGE is new.
     * The counter behind the logo is not: it lives at `gp+612` (0x800AE864),
     * and `xrefs` on it returns the two reads in SpriteAnim, the increment at
     * 0x80038CE8 and the wrap at 0x80038CFC, and nothing else in the image.
     * InitLoadingAnim (0x80038DFC), which is the function TestIt calls to set
     * this animation up, clears `gp+17100` and leaves this halfword where it
     * was. A second screen therefore picks the rotation up mid-turn, and it is
     * SUPPOSED to.
     *
     * The old behaviour was itself a fix for a real complaint — the second
     * screen of a session did not open the way the first did — but the fix was
     * on the wrong side: what made the first screen special was the backwards
     * walk, and the walk is what the executable contradicts.
     */

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
    /* A fused multiply/subtract (notably on ARM) can leave a few positive
     * ulps after an exact frame count. Do not turn that residue into a whole
     * extra frame. This clock-unit tolerance is under four picoseconds. */
    if (l->timed && l->hold <= 1e-9) {
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

/*
 * FORWARDS ALONG THE COUNTER — 0x80038CDC…0x80038CFC, `c = c + 1` wrapping at
 * 31 — and the screen opens on whatever the last one left behind.
 *
 * This used to walk BACKWARDS, from the last cell of the 8 x 3 block to the
 * first, and the argument for it was a retail capture: read forwards the grid
 * block's widths run 17 16 14 13 11 9 7 5 4 ..., which is a third of the way
 * into the turn and edge-on within half a second, where the capture's logo is
 * a wide one. The observation was right and the conclusion was not. The turn
 * does start wide — it starts at (224, 0), a 24-wide broadside cell that is
 * not in the block at all and is held for five steps — and then narrows
 * through the block exactly as it is laid out. Reversing the block was a way
 * of getting a wide first frame out of a strip whose first EIGHT steps — the
 * five-step hold and the three cells after it — had not been found.
 *
 * The counter's own range is 0..30: the increment at 0x80038CE4 runs before
 * the wrap test at 0x80038CF4, so 31 is written and immediately replaced and
 * is never a value the draw sees.
 */
u32 q2_loading_cell(const q2_loading *l)
{
    u32 step;

    if (!l || l->spin < 0.0)
        return 0;

    step = (u32)(l->spin / (double)Q2_LOADING_CELL_UNITS);
    if (step >= Q2_LOADING_CELLS)
        step = Q2_LOADING_CELLS - 1;
    return step;
}

/*
 * SpriteAnim's cell map, 0x80038840…0x800388DC, arm for arm.
 *
 * Four of the thirty-one steps are not on the 8 x 3 block: the author parked
 * further rotation frames in spare corners of the sheet, and the switch names
 * their corners one at a time. The `c < 5` arm is a HOLD — five consecutive
 * steps on the same broadside cell — which is why the widths measured in
 * counter order come out as one smooth sweep with a pause at the top.
 */
void q2_loading_cell_uv(u32 cell, u8 *u_out, u8 *v_out)
{
    u8 u, v;

    if (cell >= Q2_LOADING_CELLS)
        cell = Q2_LOADING_CELLS - 1;

    if (cell < 5) {                  /* 0x80038848 slti v0, v1, 5           */
        u = 224; v = 0;              /* 0x80038850 / 0x80038854 (sb zero)   */
    } else if (cell == 5) {          /* 0x80038864                          */
        u = 192; v = 80;             /* 0x8003886C / 0x80038874             */
    } else if (cell == 6) {          /* 0x80038880                          */
        u = 224; v = 120;            /* 0x80038888 / 0x80038890             */
    } else if (cell == 7) {          /* 0x8003889C                          */
        u = 0;   v = 220;            /* 0x800388A4 (sb zero) / 0x800388A0   */
    } else {
        /*
         * 0x800388B8: `andi v0, v1, 7` then `sll v0, v0, 5` — the column
         * times the 32-texel pitch. 0x800388C0: `andi v1, v1, 0x18` then
         * `srl v1, v1, 3` — the row, which for 8..30 is 1, 2 or 3 and never 0,
         * so the base the row is measured from is 124 and not 144. The `* 20`
         * is 0x800388CC…0x800388D4's `(r * 4 + r) * 4`.
         */
        u = (u8)((cell & 7u) * Q2_LOADING_CELL_W);
        v = (u8)(124u + 20u * ((cell >> 3) & 3u));
    }

    if (u_out)
        *u_out = u;
    if (v_out)
        *v_out = v;
}

/*
 * The logo, as one textured SPRITE off the menu's own atlas.
 *
 * A SPRT rather than the gouraud quad `q2_menu_draw_icon` emits, because
 * nothing modulates this: the strip is already the colour it should be.
 * `Q2_MENU_MOD_NORMAL` is unity for a modulated primitive, which is the same
 * thing said the way the hardware says it (menufont.h).
 *
 * AND A SPRT RATHER THAN A POLY_FT4, which is not a taste. SpriteAnim builds a
 * libgpu `SPRT` and nothing else: 0x80038B24 writes `{r,g,b,code}` from
 * gp+616 into prim+4, 0x80038B38 the corner into prim+8, 0x80038B48 the two
 * texel bytes into prim+12/+13, 0x80038B50 the CLUT into prim+14 and
 * 0x80038B60 the 32 x 20 size into prim+16 — the SPRT layout field for field —
 * and 0x80038B68 calls `SetSprt` (0x8008A338, named in the NTSC symbol table),
 * whose whole body is `len = 4` and `code = 0x64`.
 *
 * The difference is visible in exactly the frames this pass added. A rectangle
 * steps one texel per pixel from its (u, v) and WRAPS in eight bits; a polygon
 * interpolates between corner coordinates that are also eight bits. Six of the
 * counter's thirty-one steps sit at u = 224 — the five-step hold at (224, 0)
 * and the cell at (224, 120) — and a quad's far corner there is 256, which is
 * 0 in a byte, so the polygon path would have run the texture backwards across
 * the whole sheet. The port never noticed because the two grid cells at u = 224
 * are mid-turn and the old backwards walk reached them a second in.
 */
static u32 loading_draw_logo(const q2_loading *l, psx_ot *ot,
                             int origin_x, int origin_y)
{
    psx_prim *p;
    u32 cell = q2_loading_cell(l);
    u8  u, v;
    int x = origin_x + Q2_LOADING_X;
    int y = origin_y + Q2_LOADING_Y;
    int i;

    /* SpriteAnim's own switch, not a grid: four of the thirty-one steps are
     * off the block at v = 144. 0x80038840…0x800388DC. */
    q2_loading_cell_uv(cell, &u, &v);

    p = psx_ot_add_bucket(ot, Q2_LOADING_OT_BUCKET);
    if (!p)
        return 0;

    p->kind  = PSX_PRIM_SPRT;    /* SetSprt, 0x80038B68 */
    p->tpage = l->font.tpage_item;
    p->tpage |= 0x0600u;
    p->set_tpage = true; /* loading screen's paired texture-page setup */
    /*
     * PALETTE 4, THE GREY RAMP — not the sheet's own blue one.
     *
     * The logo is GREY on the console, and the bank has exactly the palette for
     * that. The sprite is authored against 68: its sixty rows use sixteen
     * colours and they are 68's sixteen entries, a monotonic blue ramp dark to
     * light. Palette 4 is the same shape with the colour taken out —
     * `000000 080808 181818 ... D8D8D8 E8E8E8`, sixteen evenly spaced greys in
     * the same index order — so drawing the strip through it gives the same
     * image, same weight, same anti-aliasing, in grey. Nothing else in the bank
     * does: 75 is the same ramp stopped at 0x78 and comes out too dark, 76 runs
     * bright-to-dark and inverts the art, and 73 is a flat white.
     *
     * Palette 70 was tried and is wrong here for a reason worth keeping:
     * hudtables.h calls it "a white-only mask palette the menu uses" and the
     * bank confirms 0xF8F8F8 at indices 8..11 with the other twelve black, so
     * it turns a sixteen-level ramp into four and the logo comes out a stippled
     * fragment. It is right for a GLYPH, whose art sits in that band.
     *
     * WHICH palette the console binds is not read — the strip is data with no
     * located reader — so this is chosen to match the capture, the same way the
     * corner and the size are.
     */
    p->clut  = q2_hud_palette_clut(l->font.tab, Q2_LOADING_PALETTE);
    p->textured_blend = true;

    p->xy[0].x = (s16)x;
    p->xy[0].y = (s16)y;
    p->xy[1].x = (s16)(x + Q2_LOADING_CELL_W);
    p->xy[1].y = (s16)y;
    p->xy[2].x = (s16)(x + Q2_LOADING_CELL_W);
    p->xy[2].y = (s16)(y + Q2_LOADING_CELL_H);
    p->xy[3].x = (s16)x;
    p->xy[3].y = (s16)(y + Q2_LOADING_CELL_H);

    /*
     * The rectangle path reads `uv[0]` alone and steps from it, which is the
     * console's `u0`/`v0` at prim+12/+13 (0x80038B48). The other three corners
     * are filled anyway, the way `emit_sprite` fills them in hud.c, so that a
     * reader is not left wondering what a half-built primitive means — and the
     * far ones DO wrap to zero on the six steps at u = 224, which is exactly
     * why the rectangle path and not the polygon one.
     */
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

    /*
     * The turning strip is InitLoadingAnim's (0x80038DFC), which TestIt calls
     * and nothing else does — so it belongs to the level screen, which is the
     * only screen this port raises for a load.
     */
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
         * AND THE WORD IS BRIGHT — 0x80079398.
         *
         * The transition sets drawable 0's highlight flag right after it enters
         * page 46, which at size 16 selects palette 70 (menufont.h, pages.c).
         * The port had the observation written down in two places and the draw
         * in neither: `k_loading` has `first == count == 1`, so its only row is
         * index 0, the cursor is 1, `selected` is never true, and the word came
         * out in `clut_text` — the sheet's blue. Measured off a capture of a
         * real zone gate it was (64, 128, 160).
         *
         * Only the executable's LOADING page. QFRONT's STARTING / GAME pair is
         * installed by `0x80101E4C` and nothing there writes the flag, so those
         * two rows stay in the text palette.
         */
        if (l->menu.page && l->menu.page->id == Q2_PAGE_LOADING)
            mo.highlight_row = 0;

        n += q2_menu_build_ot(&l->menu, ot, &mo);
    }

    return n;
}
