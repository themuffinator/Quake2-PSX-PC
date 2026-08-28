#include "menufont.h"

#include "menu.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* The three faces                                                            */
/* ------------------------------------------------------------------------- */
/*
 * Cell sizes from the size switch at 0x8001AD6C — `s6` is the cell width and
 * the pen advance, `s2` the cell height — and confirmed independently by the
 * selection bar at 0x8001A830, which re-derives the same three heights to work
 * out how tall to draw itself. Column counts and the v origin are from the
 * locator at 0x8001B494 and its two call sites.
 */
static const q2_menu_face g_face_small = { 8,   8,  0,   0,  8 };
static const q2_menu_face g_face_item  = { 16, 11, 15, 100, 16 };
static const q2_menu_face g_face_title = { 32, 20,  7,   0, 32 };

const q2_menu_face *q2_menu_face_get(int size)
{
    switch (size) {
    case Q2_MENU_FACE_SMALL: return &g_face_small;
    case Q2_MENU_FACE_ITEM:  return &g_face_item;
    case Q2_MENU_FACE_TITLE: return &g_face_title;
    default:                 return NULL;
    }
}

const char *q2_menu_icons_name(bool multiplayer, int players)
{
    /* 0x8003FEAC: the session mode picks the file, and the branch order is
     * "not multiplayer" first, then three-or-more, then two. */
    if (!multiplayer)
        return Q2_MENU_ICONS_NAME_1P;
    return (players >= 3) ? Q2_MENU_ICONS_NAME_MP : Q2_MENU_ICONS_NAME_2P;
}

/* ------------------------------------------------------------------------- */
/* Residency                                                                  */
/* ------------------------------------------------------------------------- */
q2_result q2_menu_font_upload(q2_menu_font *out, const q2_hud_tables *tab,
                              const q2_vram_section *section, psx_vram *vram,
                              bool multiplayer, int players)
{
    q2_vram_rect rect;
    const char *icons;

    if (!out || !tab || !section || !vram)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    out->tab = tab;

    /*
     * Keep 0x8003FE20's registration order. The 256-byte-wide 8bpp image in
     * slot 12 spans VRAM x 832..959, while frontend.lbm occupies x 896..959.
     * This is intentional page aliasing: the final two previews sample only
     * the left half of their page, then the later font upload replaces its
     * unused right half. Uploading the preview after the font turns every
     * 16/32-pixel glyph into its vertical byte pattern.
     *
     * QFRONT module+0x2AD4: the first ten previews use the sheet in slot 8;
     * the final two use slot 12. They are optional on a normal map, just like
     * every other front-end-only image.
     */
    if (q2_vram_upload_named(section, Q2_MENU_ARENA_NAME_0,
                             Q2_MENU_ARENA_SLOT_0, 0, vram, NULL) == Q2_OK)
        out->arena_resident[0] = true;
    if (q2_vram_upload_named(section, Q2_MENU_ARENA_NAME_1,
                             Q2_MENU_ARENA_SLOT_1, 0, vram, NULL) == Q2_OK)
        out->arena_resident[1] = true;

    /* The font atlas follows both preview registrations in retail, which is
     * semantically significant because of the slot-12 alias above. */
    if (q2_vram_upload_named(section, Q2_MENU_ATLAS_NAME, Q2_MENU_ATLAS_SLOT,
                             Q2_MENU_ATLAS_V_OFS, vram, NULL) == Q2_OK)
        out->item_resident = true;

    if (q2_vram_upload_named(section, Q2_HUD_ATLAS_NAME, 15,
                             Q2_HUD_ATLAS_VRAM_Y - Q2_VRAM_TEXPAGE_Y,
                             vram, NULL) == Q2_OK)
        out->small_resident = true;

    icons = q2_menu_icons_name(multiplayer, players);
    if (q2_vram_upload_named(section, icons, Q2_MENU_ICONS_SLOT,
                             Q2_MENU_ICONS_V_OFS, vram, &rect) == Q2_OK) {
        out->icons_resident = true;
        /* 4bpp: a halfword of VRAM is four texels. */
        out->icons_w = (u16)(rect.w * 4);
        out->icons_h = (u16)rect.h;
    }

    /* The font's colours are in the executable, not on the disc. */
    q2_hud_palettes_upload(tab, vram);

    out->tpage_item  = psx_make_tpage(Q2_MENU_ATLAS_PAGE_X,
                                      Q2_MENU_ATLAS_PAGE_Y,
                                      PSX_BLEND_HALF, PSX_TEX_4BIT);
    out->tpage_small = psx_make_tpage(Q2_HUD_ATLAS_PAGE_X, Q2_HUD_ATLAS_PAGE_Y,
                                      PSX_BLEND_HALF, PSX_TEX_4BIT);
    out->tpage_icons = psx_make_tpage(Q2_MENU_ICONS_PAGE_X,
                                      Q2_MENU_ICONS_PAGE_Y,
                                      PSX_BLEND_HALF, PSX_TEX_4BIT);
    out->arena_tpage[0] = psx_make_tpage(Q2_MENU_ARENA_PAGE_X_0,
                                         Q2_MENU_ARENA_PAGE_Y,
                                         PSX_BLEND_HALF, PSX_TEX_8BIT);
    out->arena_tpage[1] = psx_make_tpage(Q2_MENU_ARENA_PAGE_X_1,
                                         Q2_MENU_ARENA_PAGE_Y,
                                         PSX_BLEND_HALF, PSX_TEX_8BIT);

    out->clut_text     = q2_hud_palette_clut(tab, Q2_MENU_PALETTE_TEXT);
    out->clut_item_hi  = q2_hud_palette_clut(tab, Q2_MENU_PALETTE_ITEM_HI);
    out->clut_title_hi = q2_hud_palette_clut(tab, Q2_MENU_PALETTE_TITLE_HI);
    out->clut_small    = q2_hud_palette_clut(tab, Q2_MENU_PALETTE_SMALL);
    out->arena_clut[0] = tab->palette256_id;
    out->arena_clut[1] = q2_hud_palette_clut(tab, Q2_MENU_ARENA_PALETTE_1);

    if (!out->item_resident && !out->small_resident)
        return Q2_ERR_NOT_FOUND;
    return Q2_OK;
}

/* ------------------------------------------------------------------------- */
/* Locating a glyph                                                           */
/* ------------------------------------------------------------------------- */
/*
 * The punctuation row, from the 85-entry jump table at 0x800AB564. Row 2 is set
 * before the dispatch (0x8001B5A8), so every entry here is a column on that row.
 * The twelve letters listed are the only ones with an arm of their own; the
 * other seventy-three land on 0x8001B668, which returns without writing a
 * column at all.
 */
static bool punctuation_column(char c, u8 *col)
{
    switch (c) {
    case '-':  *col =  0; return true;
    case ':':  *col =  1; return true;
    case '/':  *col =  2; return true;
    case '.':  *col =  3; return true;
    case '?':  *col =  4; return true;
    case 'i':  *col =  5; return true;
    case '\'': *col =  6; return true;
    case '!':  *col =  9; return true;
    case ',':  *col = 10; return true;
    case '&':  *col = 11; return true;
    case '(':  *col = 12; return true;
    case ')':  *col = 13; return true;
    default:              return false;
    }
}

/*
 * 0x8001B494. `col` is in/out for the reason the header explains: the default
 * arm leaves it alone, and the caller's value is the previous glyph's already
 * multiplied u. Returns whether a cell was actually chosen.
 */
static bool locate(int size, char c, u8 *col, u8 *row)
{
    const q2_menu_face *f = q2_menu_face_get(size);
    unsigned uc = (unsigned char)c;

    if (!f)
        return false;

    *row = 0;

    if (size == Q2_MENU_FACE_TITLE) {
        /* The four overrides, applied before anything else because rows 0..2
         * of the seven-wide face are already full of letters. */
        switch (c) {
        case '2': *col = 5; *row = 3; return true;
        case '3': *col = 6; *row = 3; return true;
        case '4': *col = 0; *row = 4; return true;
        case '?': *col = 1; *row = 4; return true;
        default:  break;
        }
    }

    if (uc >= 'A' && uc <= 'Z') {
        unsigned n = uc - 'A';
        *row = (u8)(n / f->cols);
        *col = (u8)(n % f->cols);
        return true;
    }

    if (uc >= '0' && uc <= '9') {
        *row = 3;
        *col = (u8)(uc - '0');
        return true;
    }

    *row = 2;
    return punctuation_column(c, col);
}

bool q2_menu_glyph_defined(int size, char c)
{
    u8 col = 0, row = 0;

    if (size == Q2_MENU_FACE_SMALL) {
        unsigned uc = (unsigned char)c;
        return uc >= Q2_HUD_GLYPH_FIRST &&
               uc <  Q2_HUD_GLYPH_FIRST + Q2_HUD_GLYPH_COUNT;
    }
    return locate(size, c, &col, &row);
}

bool q2_menu_glyph(const q2_menu_font *font, int size, char c, u8 *u, u8 *v)
{
    const q2_menu_face *f = q2_menu_face_get(size);
    u8 col, row;

    if (!f || !u || !v)
        return false;

    if (size == Q2_MENU_FACE_SMALL) {
        /* The 8-pixel face is the HUD's, and it IS a table: index c - 32 into
         * 0x8009D554, two bytes each (0x8001AFA4). */
        unsigned idx = (unsigned char)c;
        if (!font || !font->tab)
            return false;
        if (idx < Q2_HUD_GLYPH_FIRST)
            return false;
        idx -= Q2_HUD_GLYPH_FIRST;
        if (idx >= Q2_HUD_GLYPH_COUNT)
            return false;
        *u = font->tab->glyph[idx].u;
        *v = font->tab->glyph[idx].v;
        return true;
    }

    /* Seeded with the caller's u so an unknown character reproduces the
     * original's stale-column behaviour rather than a substitution. */
    col = *u;
    (void)locate(size, c, &col, &row);

    *u = (u8)(col * f->cell_w);
    *v = (u8)(row * f->cell_h + f->v_origin);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Drawing                                                                    */
/* ------------------------------------------------------------------------- */
int q2_menu_font_width(int size, const char *text)
{
    const q2_menu_face *f = q2_menu_face_get(size);

    if (!f || !text)
        return 0;
    return q2_menu_text_length(text) * f->advance;
}

/* One POLY_FT4, 1:1 between the page and the screen (0x8001B0A8). */
static psx_prim *emit_quad(psx_ot *ot, u32 bucket, u16 tpage, u16 clut,
                           int x, int y, int w, int h, u8 uu, u8 vv, u8 mod)
{
    psx_prim *p = psx_ot_add_bucket(ot, bucket);
    int i;

    if (!p)
        return NULL;

    p->kind  = PSX_PRIM_FT4;
    p->tpage = tpage;
    p->clut  = clut;
    p->textured_blend = true;
    p->semi_transparent = false;

    /*
     * PERIMETER ORDER, not the packet's.
     *
     * A libgpu POLY_FT4 stores its corners in Z order — top-left, top-right,
     * bottom-left, bottom-right — and that is how the original's glyph packet
     * is filled at 0x8001B0A8. This port's `psx_prim` does not: the rasteriser
     * splits a quad as the fan (0,1,2) + (0,2,3) because `MapMod` stores world
     * quads around the perimeter (scene.h). Handing it a Z-ordered quad does
     * not fail loudly — it draws the two triangles of a bowtie, which for a
     * glyph means the right quarter of every letter goes missing and 'B' comes
     * out as 'E'. So the corners are transposed here, once, at the seam.
     *
     * HALF-OPEN, as on the hardware. The console's packet spans
     * (x, y)…(x + 16, y + 11) and the GPU covers the 16 x 11 pixels *inside*
     * that, because its fill rule drops the right and bottom edges.
     *
     * This used to hand the rasteriser the INCLUSIVE corner (`+ w - 1`) to
     * compensate for an edge test that admitted both edges; without that, a
     * glyph covered 17 x 12 and sampled the first column of the next letter out
     * of the atlas's 16-pitch cells. The rasteriser implements the real fill
     * rule now (raster.c), so the console's own numbers are handed over
     * unaltered and the coverage comes out the same: w pixels, w texels.
     */
    p->xy[0].x = (s16)x;             p->xy[0].y = (s16)y;
    p->xy[1].x = (s16)(x + w);       p->xy[1].y = (s16)y;
    p->xy[2].x = (s16)(x + w);       p->xy[2].y = (s16)(y + h);
    p->xy[3].x = (s16)x;             p->xy[3].y = (s16)(y + h);

    p->uv[0].u = uu;                 p->uv[0].v = vv;
    p->uv[1].u = (u8)(uu + w);       p->uv[1].v = vv;
    p->uv[2].u = (u8)(uu + w);       p->uv[2].v = (u8)(vv + h);
    p->uv[3].u = uu;                 p->uv[3].v = (u8)(vv + h);

    for (i = 0; i < 4; i++) {
        p->rgb[i].r = mod;
        p->rgb[i].g = mod;
        p->rgb[i].b = mod;
    }
    return p;
}

u32 q2_menu_font_print(const q2_menu_font *font, psx_ot *ot, u32 bucket,
                       int size, int cx, int cy, bool highlight,
                       int origin_x, int origin_y, const char *text)
{
    const q2_menu_face *f = q2_menu_face_get(size);
    bool dim = false, grey = false, underline = false;
    u32 emitted = 0;
    int pen_x, pen_y;
    u8  last_u = 0;
    const char *s;
    u16 tpage;

    if (!font || !ot || !f || !text)
        return 0;

    tpage = (size == Q2_MENU_FACE_SMALL) ? font->tpage_small
                                         : font->tpage_item;

    /* 0x8001AE24..0x8001AE30: the run is centred on the drawable's x, and its
     * top is half a cell above the drawable's y. */
    pen_x = origin_x + cx - (q2_menu_text_length(text) * f->advance) / 2;
    pen_y = origin_y + cy - f->cell_h / 2;

    for (s = text; *s; s++) {
        char c = *s;
        u8 uu, vv;
        u8 mod;
        u16 clut;

        /* The four codes, in the original's own branch order (0x8001AEF8).
         * `b` is a full reset, not just "bright". */
        if (c == Q2_MENU_CODE_DIM)   { dim = true;  continue; }
        if (c == Q2_MENU_CODE_BRIGHT){ dim = false; grey = false;
                                       underline = false; continue; }
        if (c == Q2_MENU_CODE_GREY)  { grey = true; dim = true; continue; }
        if (c == Q2_MENU_CODE_UNDER) { underline = true; continue; }

        mod = grey ? Q2_MENU_MOD_GREY : Q2_MENU_MOD_NORMAL;

        if (c != ' ') {
            bool have = true;

            uu = last_u;
            vv = 0;

            if (size == Q2_MENU_FACE_SMALL) {
                clut = font->clut_small;
                have = q2_menu_glyph(font, size, c, &uu, &vv);
            } else if (size == Q2_MENU_FACE_TITLE) {
                clut = highlight ? font->clut_title_hi : font->clut_text;
                q2_menu_glyph(font, size, c, &uu, &vv);
            } else {
                clut = (highlight && !dim) ? font->clut_item_hi
                                           : font->clut_text;
                q2_menu_glyph(font, size, c, &uu, &vv);
            }

            if (have) {
                if (emit_quad(ot, bucket, tpage, clut, pen_x, pen_y,
                              f->cell_w, f->cell_h, uu, vv, mod))
                    emitted++;
                last_u = uu;
            }
        }

        /*
         * The `u` pass. It is a second quad eight pixels lower sampling
         * (0, 2*cell_h + 100) — for the 16-pixel face, the '-' at the start of
         * the punctuation row — and it is emitted for spaces too, which is what
         * makes an underlined run continuous. Its palette is the title's
         * highlight rather than the item's (0x8001B3E0), which the original
         * does at size 16 and nowhere else.
         */
        if (underline) {
            u16 uclut;

            if (size == Q2_MENU_FACE_ITEM)
                uclut = highlight ? font->clut_title_hi : font->clut_text;
            else if (size == Q2_MENU_FACE_TITLE)
                uclut = highlight ? font->clut_title_hi : font->clut_text;
            else
                uclut = font->clut_small;

            if (emit_quad(ot, bucket, tpage, uclut, pen_x, pen_y + 8,
                          f->cell_w, f->cell_h, 0,
                          (u8)(2 * f->cell_h + 100), mod))
                emitted++;
        }

        pen_x += f->advance;
    }

    return emitted;
}
