/*
 * menudraw.c — the menu, built as primitives.
 *
 * Everything below carries the address it came from. The three that are easiest
 * to get subtly wrong, and are therefore worth naming here:
 *
 *   0x8001CF74  the title's y is (framebuffer_height - 188) / 2 + 10 and its
 *               x is a hard 256; it is drawn at size 32 with the drawable's
 *               highlight flag set, which is what gives it palette 71
 *   0x8001BBB4  a slider's frame is four LINE_F2 over a 133 x 8 box whose top
 *               is the row's y, and the fill is one POLY_F4 ending at
 *               bar_x + value + 3 (0x8001BD28) — which is why every slider
 *               label in the tables is space-padded to exactly 11 characters
 *   0x8001A7A8  the selection bar is TWO semi-transparent POLY_G4, each half
 *               the viewport wide, dark at the outside and bright where they
 *               meet, and it is suppressed on any row whose text is "PAUSED"
 *
 * An item's y is the vertical *centre* of its row, not its top: the title sits
 * at 40 with a 32-pixel face while the first item sits at 64 with a 16-pixel
 * one, which only avoids overlapping if both are centred.
 */
#include "menudraw.h"

#include <string.h>

/*
 * The bright end of each bar colour, from the four arms at 0x8001AA0C,
 * 0x8001AA44, 0x8001AA78 and 0x8001AABC. The dark end is the same hue with
 * every non-zero component replaced by 32, which is exactly what the arms
 * write, so it is derived rather than tabulated twice.
 */
const u8 q2_menu_bar_rgb[4][3] = {
    {   0,   0, 255 },   /* 0  blue   */
    { 255,   0,   0 },   /* 1  red    */
    { 127,   0, 255 },   /* 2  purple */
    {   0, 255,   0 }    /* 3  green  */
};

/* The arms write a literal 32 into the components the bright end lights, and
 * zero into the rest, so the dark end is not "the bright one scaled". */
static void bar_dark(int colour, u8 out[3])
{
    int i;
    for (i = 0; i < 3; i++)
        out[i] = q2_menu_bar_rgb[colour][i] ? 32 : 0;
}

void q2_menu_draw_opts_default(q2_menu_draw_opts *o, const q2_menu_font *font)
{
    if (!o)
        return;
    memset(o, 0, sizeof(*o));
    o->font       = font;
    o->bucket     = Q2_MENU_OT_BUCKET_ITEM;
    o->view_x     = 0;
    o->view_w     = Q2_MENU_SCREEN_W;
    o->bar_colour = Q2_MENU_BAR_BLUE;
    o->backdrop   = 0;
    o->backdrop_alpha = 0;
    o->highlight_row  = -1;
}

/* ------------------------------------------------------------------------- */
/* Flat primitives                                                            */
/*                                                                            */
/* Corners are INCLUSIVE here — see the coverage note in menufont.c. The       */
/* console's packets carry the half-open corner; this rasteriser's edge test   */
/* admits it, so passing it through would draw one pixel too many on each      */
/* axis.                                                                      */
/* ------------------------------------------------------------------------- */
static psx_prim *emit_flat_quad(psx_ot *ot, u32 bucket, int x0, int y0,
                                int x1, int y1, const u8 rgb[3], bool semi)
{
    psx_prim *p = psx_ot_add_bucket(ot, bucket);
    int i;

    if (!p)
        return NULL;

    /* Perimeter order — see the note in menufont.c's emit_quad. */
    p->kind = PSX_PRIM_F4;
    p->semi_transparent = semi;
    p->xy[0].x = (s16)x0; p->xy[0].y = (s16)y0;
    p->xy[1].x = (s16)x1; p->xy[1].y = (s16)y0;
    p->xy[2].x = (s16)x1; p->xy[2].y = (s16)y1;
    p->xy[3].x = (s16)x0; p->xy[3].y = (s16)y1;
    for (i = 0; i < 4; i++) {
        p->rgb[i].r = rgb[0];
        p->rgb[i].g = rgb[1];
        p->rgb[i].b = rgb[2];
    }
    return p;
}

/* LINE_F2, code 0x40 — one edge of a slider's frame (0x8001BBAC). */
static void emit_line(psx_ot *ot, u32 bucket, int x0, int y0, int x1, int y1,
                      const u8 rgb[3])
{
    psx_prim *p = psx_ot_add_bucket(ot, bucket);

    if (!p)
        return;

    p->kind = PSX_PRIM_LINE_F2;
    p->xy[0].x = (s16)x0; p->xy[0].y = (s16)y0;
    p->xy[1].x = (s16)x1; p->xy[1].y = (s16)y1;
    p->rgb[0].r = rgb[0];
    p->rgb[0].g = rgb[1];
    p->rgb[0].b = rgb[2];
}

/* ------------------------------------------------------------------------- */
/* The selection bar — 0x8001A7A8                                             */
/* ------------------------------------------------------------------------- */
/*
 * Two POLY_G4 with the semi-transparency bit set, spanning the viewport from
 * y - 2 to y + cell_h + 1, dark at the two ends and bright where they meet.
 * The gouraud shading is the whole point: a single flat quad would be a block,
 * and what the console shows is a soft band under the row.
 */
static void emit_select_bar(psx_ot *ot, u32 bucket, int colour,
                            int view_x, int view_w, int cy, int cell_h,
                            int origin_x, int origin_y)
{
    u8 dark[3];
    int half = view_w / 2;
    int y0 = origin_y + cy - cell_h / 2 - 2;
    int y1 = origin_y + cy - cell_h / 2 + cell_h + 1;
    int x0 = origin_x + view_x;
    int xm = x0 + half;
    /* Half-open, like every other far corner in the port: raster.c implements
     * the GPU's fill rule, so the `- 1` that used to compensate for an
     * inclusive edge test would now leave the bar a pixel short. */
    int x1 = x0 + view_w;
    psx_prim *p;
    int i;

    if (colour < 0 || colour > 3)
        return;

    bar_dark(colour, dark);

    for (i = 0; i < 2; i++) {
        int lx = i ? xm : x0;
        int rx = i ? x1 : xm;
        const u8 *left  = i ? q2_menu_bar_rgb[colour] : dark;
        const u8 *right = i ? dark : q2_menu_bar_rgb[colour];
        int v;

        p = psx_ot_add_bucket(ot, bucket);
        if (!p)
            return;

        p->kind = PSX_PRIM_G4;
        p->semi_transparent = true;
        p->xy[0].x = (s16)lx; p->xy[0].y = (s16)y0;
        p->xy[1].x = (s16)rx; p->xy[1].y = (s16)y0;
        p->xy[2].x = (s16)rx; p->xy[2].y = (s16)y1;
        p->xy[3].x = (s16)lx; p->xy[3].y = (s16)y1;

        /* Perimeter order, so vertices 1 and 2 are the right-hand pair. */
        for (v = 0; v < 4; v++) {
            const u8 *c = (v == 1 || v == 2) ? right : left;
            p->rgb[v].r = c[0];
            p->rgb[v].g = c[1];
            p->rgb[v].b = c[2];
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Runtime menu rows (match banners and results)                              */
/* ------------------------------------------------------------------------- */
void q2_menu_draw_text_row(psx_ot *ot, const q2_menu_draw_opts *opts,
                           int size, int x, int y, bool highlight,
                           int colour, const char *text)
{
    const q2_menu_face *face = q2_menu_face_get(size);
    if (!ot || !opts || !opts->font || !face || !text) return;
    q2_menu_font_print(opts->font, ot, opts->bucket, size, x, y, highlight,
                       opts->origin_x, opts->origin_y, text);
    if (highlight)
        emit_select_bar(ot, opts->bucket, colour, opts->view_x, opts->view_w,
                          y, face->cell_h, opts->origin_x, opts->origin_y);
}

/* ------------------------------------------------------------------------- */
/* The slider — 0x8001BA14                                                    */
/* ------------------------------------------------------------------------- */
#define Q2_MENU_BAR_W 133
#define Q2_MENU_BAR_H   8

static void emit_slider(psx_ot *ot, u32 bucket, int bar_x, int bar_y,
                        int value, bool selected, bool greyed,
                        int origin_x, int origin_y)
{
    /* 0x8001BAA8: white when the row is under the cursor, (74,143,170) when
     * not, (32,32,32) when the label is greyed. */
    static const u8 sel[3]  = { 255, 255, 255 };
    static const u8 norm[3] = {  74, 143, 170 };
    static const u8 grey[3] = {  32,  32,  32 };
    const u8 *rgb = greyed ? grey : (selected ? sel : norm);

    int x0 = origin_x + bar_x;
    int y0 = origin_y + bar_y;
    int x1 = x0 + Q2_MENU_BAR_W - 1;
    int y1 = y0 + Q2_MENU_BAR_H - 1;
    int fill;

    /* The four edges, each its own LINE_F2. */
    emit_line(ot, bucket, x0, y0, x1, y0, rgb);
    emit_line(ot, bucket, x0, y1, x1, y1, rgb);
    emit_line(ot, bucket, x0, y0, x0, y1, rgb);
    emit_line(ot, bucket, x1, y0, x1, y1, rgb);

    /* 0x8001BD28: the fill's right edge is bar_x + value + 3. */
    if (value < 0) value = 0;
    if (value > Q2_MENU_SLIDER_MAX) value = Q2_MENU_SLIDER_MAX;
    fill = x0 + value + 3;
    if (fill > x1) fill = x1;

    emit_flat_quad(ot, bucket, x0 + 1, y0 + 1, fill, y0 + 6, rgb, false);
}

/* ------------------------------------------------------------------------- */
u32 q2_menu_draw_icon(const q2_menu_font *font, psx_ot *ot, u32 bucket,
                      int x, int y, u8 u, u8 v, u8 w, u8 h)
{
    psx_prim *p;
    int i;

    if (!font || !ot || !font->icons_resident || w == 0 || h == 0)
        return 0;

    p = psx_ot_add_bucket(ot, bucket);
    if (!p)
        return 0;

    /* 0x80033320 fills a POLY_GT4 — gouraud, so all four corners carry a
     * colour — and the caller it has passes the same value to each. */
    p->kind  = PSX_PRIM_GT4;
    p->tpage = font->tpage_icons;
    p->clut  = font->clut_text;
    p->textured_blend = true;

    /* Half-open, as everywhere else — see menufont.c. The `- 1` here used to
     * compensate for a rasteriser that filled both edges. */
    p->xy[0].x = (s16)x;             p->xy[0].y = (s16)y;
    p->xy[1].x = (s16)(x + w);       p->xy[1].y = (s16)y;
    p->xy[2].x = (s16)(x + w);       p->xy[2].y = (s16)(y + h);
    p->xy[3].x = (s16)x;             p->xy[3].y = (s16)(y + h);

    p->uv[0].u = u;                  p->uv[0].v = v;
    p->uv[1].u = (u8)(u + w);        p->uv[1].v = v;
    p->uv[2].u = (u8)(u + w);        p->uv[2].v = (u8)(v + h);
    p->uv[3].u = u;                  p->uv[3].v = (u8)(v + h);

    for (i = 0; i < 4; i++) {
        p->rgb[i].r = Q2_MENU_MOD_NORMAL;
        p->rgb[i].g = Q2_MENU_MOD_NORMAL;
        p->rgb[i].b = Q2_MENU_MOD_NORMAL;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
u32 q2_menu_build_ot(const q2_menu *m, psx_ot *ot,
                     const q2_menu_draw_opts *opts)
{
    q2_menu_draw_opts fallback;
    const q2_menu_face *item_face;
    u32 n = 0;
    int i;

    if (!m || !m->open || !m->page || !ot)
        return 0;

    if (!opts) {
        q2_menu_draw_opts_default(&fallback, NULL);
        opts = &fallback;
    }
    if (!opts->font)
        return 0;

    if (m->page_id == Q2_PAGE_FRONT_PLAYERS) {
        int p;
        u32 before = ot->prim_count;
        for (p = 0; p < m->mp_setup.players && p < 4; p++) {
            q2_menu_player_row rows[7];
            q2_menu_draw_opts panel = *opts;
            int x, y;
            int count = q2_menu_player_rows(m, p, rows);
            int colour = m->mp_setup.mode == Q2_MENU_MP_TEAM_DEATHMATCH
                           ? m->mp_team[p] : m->mp_colour[p];
            q2_menu_player_origin(m, p, &x, &y);
            panel.origin_x += x;
            panel.origin_y += y;
            panel.view_x = m->mp_setup.players == 2 ? -x : 0;
            panel.view_w = m->mp_setup.players == 2 ? 512 : 256;
            for (i = 0; i < count; i++)
                q2_menu_draw_text_row(ot, &panel, rows[i].size, 128, rows[i].y,
                    rows[i].selected && !rows[i].disabled, colour & 3, rows[i].text);
        }
        return ot->prim_count - before;
    }

    item_face = q2_menu_face_get(Q2_MENU_FACE_ITEM);

    /*
     * A backdrop the original does not have: it draws the menu straight over
     * the frozen world. It is here because an offline render has nothing behind
     * it, and because a caller may want the world dimmed. Alpha 0 is the
     * console's behaviour and is the default.
     */
    if (opts->backdrop_alpha > 0) {
        u8 rgb[3];
        rgb[0] = (u8)((opts->backdrop & 0x1F) << 3);
        rgb[1] = (u8)(((opts->backdrop >> 5) & 0x1F) << 3);
        rgb[2] = (u8)(((opts->backdrop >> 10) & 0x1F) << 3);
        if (emit_flat_quad(ot, opts->bucket,
                           opts->origin_x, opts->origin_y,
                           opts->origin_x + Q2_MENU_SCREEN_W - 1,
                           opts->origin_y + Q2_MENU_SCREEN_H - 1,
                           rgb, opts->backdrop_alpha < 255))
            n++;
    }

    /* Items first, then their furniture, so the furniture ends up behind. */
    for (i = 0; i < (int)m->page->count; i++) {
        const q2_menu_item *it = &m->page->items[i];
        const int y = q2_menu_item_y(m, i);
        char line[80];
        const char *label;
        bool selected, greyed;

        label = q2_menu_item_text(m, i);
        if (!label[0] && it->widget != Q2_WIDGET_CHOICE)
            continue;                     /* the empty-string records */

        selected = (i == m->cursor) && q2_menu_item_selectable(m, i);
        /* ...or the row the CALLER has lit, which is how 0x80079178 makes the
         * word LOADING bright on a page with nothing selectable on it. */
        if (i == opts->highlight_row)
            selected = true;
        greyed   = !q2_menu_item_selectable(m, i) &&
                   i >= (int)m->page->first;

        q2_menu_item_display(m, i, line, (u32)sizeof(line));

        n += q2_menu_font_print(opts->font, ot, opts->bucket,
                                Q2_MENU_FACE_ITEM, it->x, y,
                                selected, opts->origin_x, opts->origin_y,
                                line);

        if (it->widget == Q2_WIDGET_SLIDER) {
            int value = 0;
            /* The bar begins where the label ends. The label is centred on
             * it->x, so its right edge is half its printable width further on. */
            int bar_x = it->x + q2_menu_font_width(Q2_MENU_FACE_ITEM, line) / 2;
            int bar_y = y - (item_face ? item_face->cell_h : 11) / 2;

            if (m->set && it->setting > Q2_SET_NONE &&
                it->setting < Q2_SET_COUNT)
                value = m->set->v[q2_menu_player_setting(it->setting, m->pauser)];

            emit_slider(ot, opts->bucket, bar_x, bar_y, value, selected,
                        greyed, opts->origin_x, opts->origin_y);
            n += 5;
        }

        /*
         * The bar, last, so it draws first.
         *
         * It is suppressed on a row whose text is EMPTY. The test is a `strcmp`
         * against `0x800AE634` at `0x8001A7FC`, and that address is the NUL
         * that terminates the string before it — so the comparand is "", not a
         * label. It matters: the memory-card SAVE FILE screen is four
         * deliberately empty rows (memcard.h), and an unfilled slot must draw
         * no bar. The sentinel bar colour suppresses it too.
         */
        if (selected && opts->bar_colour != Q2_MENU_BAR_NONE && line[0]) {
            emit_select_bar(ot, opts->bucket, opts->bar_colour,
                            opts->view_x, opts->view_w, y,
                            item_face ? item_face->cell_h : 11,
                            opts->origin_x, opts->origin_y);
            n += 2;
        }
    }

    /*
     * The title, at size 32 and always highlighted (0x8001CFAC) — and it gets a
     * bar of its own. That follows from the rule rather than being a special
     * case: the bar is drawn for any drawable whose +0x48 is set and whose text
     * is not empty, and the title sets +0x48 unconditionally. Retail
     * screenshots confirm it — every page banner sits on the same full-width
     * band as the cursor row, in the same colour.
     *
     * Its bar is emitted after its text, so it lands behind, and the title's
     * cell is 20 tall against an item's 11.
     *
     * Its COLOUR is its own, not the page's. The bar colour is bits 6..8 of a
     * drawable's flag word, and FrontEndAddPausedTitle is the only thing in the
     * image that ever sets them to something other than zero: it fills them from
     * the pausing player's configuration record (0x8001CFF0..0x8001D044), while
     * FrontEndCreateMenu clears them on every row it appends (0x8001A524). So in
     * a split-screen match the banner wears the pauser's colour and the selected
     * row's band stays blue. This drew the page's one colour for both, which was
     * right for the rows by accident and wrong for the banner always.
     */
    if (m->page->title) {
        int ty = q2_menu_title_y(m->screen_h);
        int tc = (int)m->title_bar_colour;
        const q2_menu_face *tf = q2_menu_face_get(Q2_MENU_FACE_TITLE);

        n += q2_menu_font_print(opts->font, ot, opts->bucket,
                                Q2_MENU_FACE_TITLE, 256, ty, true,
                                opts->origin_x, opts->origin_y,
                                m->page->title);

        /*
         * The caller's own suppression still wins — a screen that asks for no
         * bars at all gets none — and 4 in the field suppresses this one
         * (0x8001A828). The field is three bits wide, but 5..7 cannot arise in
         * the shipped build: colour is the player index over four players and
         * team is `p & 1` (0x8001BEDC, 0x8001BF04). They are folded in with 4
         * here so the palette lookup stays in range.
         */
        if (opts->bar_colour != Q2_MENU_BAR_NONE && tc < Q2_MENU_BAR_NONE) {
            emit_select_bar(ot, opts->bucket, tc,
                            opts->view_x, opts->view_w, ty,
                            tf ? tf->cell_h : 20,
                            opts->origin_x, opts->origin_y);
            n += 2;
        }
    }

    /* The single-player pause menu's status line, at (256, 204). */
    if (m->status[0])
        n += q2_menu_font_print(opts->font, ot, opts->bucket,
                                Q2_MENU_FACE_ITEM, 256, 204, false,
                                opts->origin_x, opts->origin_y, m->status);

    return n;
}
