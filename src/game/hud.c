#include "hud.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 0x800AE884 and 0x800AE888: grey for a hit the armour took, red for one it
 * did not. The fourth byte of each is padding the flash writer copies along
 * with the colour and never reads back. */
const u8 q2_hud_flash_armour_rgb[3] = { 0x40, 0x40, 0x40 };
const u8 q2_hud_flash_health_rgb[3] = { 0x40, 0x00, 0x00 };

/* ------------------------------------------------------------------------- */
/* Font residency                                                             */
/* ------------------------------------------------------------------------- */
static void upload_palette(psx_vram *vram, const q2_hud_palette *p)
{
    int i;

    if (!p->present)
        return;
    if (p->vram_y < 0 || p->vram_y >= PSX_VRAM_HEIGHT)
        return;
    psx_vram_transfer(vram);
    for (i = 0; i < Q2_HUD_PALETTE_SIZE; i++) {
        int x = p->vram_x + i;
        if (x < 0 || x >= PSX_VRAM_WIDTH)
            break;
        vram->px[p->vram_y][x] = p->entry[i];
    }
}

q2_result q2_hud_font_upload(q2_hud_font *out, const q2_hud_tables *tab,
                             const q2_vram_section *section, psx_vram *vram)
{
    u32 index;
    size_t need;
    u8 *pixels;
    q2_result r;
    size_t got = 0;
    u32 row, halfwords;
    const q2_hud_palette *pal;
    u32 i;

    if (!out || !tab || !section || !vram)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    out->tab = tab;

    if (!q2_vram_find_by_name(section, Q2_HUD_ATLAS_NAME, &index))
        return Q2_ERR_NOT_FOUND;

    need = q2_vram_decoded_size(section, index);
    if (need == 0)
        return Q2_ERR_BAD_FORMAT;

    pixels = (u8 *)malloc(need);
    if (!pixels)
        return Q2_ERR_NO_MEMORY;

    r = q2_vram_decode(section, index, pixels, need, &got);
    if (r != Q2_OK) {
        free(pixels);
        return r;
    }

    /*
     * The record's `width` is BYTES per row, and the upload rect is width>>1
     * halfwords — the same rule the texture pages follow (vram.h). At 4bpp that
     * makes chars.lbm 256 texels across.
     */
    psx_vram_transfer(vram);
    halfwords = (u32)section->images[index].width >> 1;
    for (row = 0; row < section->images[index].height; row++) {
        int vy = Q2_HUD_ATLAS_VRAM_Y + (int)row;
        const u8 *src = pixels + (size_t)row * section->images[index].width;
        u32 hw;

        if (vy < 0 || vy >= PSX_VRAM_HEIGHT)
            break;
        for (hw = 0; hw < halfwords; hw++) {
            int vx = Q2_HUD_ATLAS_VRAM_X + (int)hw;
            if (vx < 0 || vx >= PSX_VRAM_WIDTH)
                break;
            vram->px[vy][vx] = (u16)(src[hw * 2] | ((u16)src[hw * 2 + 1] << 8));
        }
    }
    free(pixels);

    /* The executable's built-in palette bank, into the 248..254 band. */
    for (i = 0; i < Q2_HUD_PALETTE_MAX; i++)
        upload_palette(vram, &tab->palette[i]);

    out->tpage = psx_make_tpage(Q2_HUD_ATLAS_PAGE_X, Q2_HUD_ATLAS_PAGE_Y,
                                PSX_BLEND_HALF, PSX_TEX_4BIT);

    pal = q2_hud_palette_get(tab, Q2_HUD_PALETTE_FONT);
    out->clut_font = pal ? pal->clut_id : 0;
    pal = q2_hud_palette_get(tab, Q2_HUD_PALETTE_FONT_ALT);
    out->clut_alt  = pal ? pal->clut_id : out->clut_font;
    pal = q2_hud_palette_get(tab, Q2_HUD_PALETTE_BOX);
    out->clut_box  = pal ? pal->clut_id : out->clut_font;

    out->resident = true;
    return Q2_OK;
}

/* ------------------------------------------------------------------------- */
/* Context and pen                                                            */
/* ------------------------------------------------------------------------- */
void q2_hud_ctx_default(q2_hud_ctx *ctx, int width, int height)
{
    if (!ctx)
        return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->width  = (s16)width;
    ctx->height = (s16)height;
    ctx->home_x = 0;
    ctx->home_y = 0;
    /* 0x80 is unity for a modulated sprite, so this is "leave the palette
     * alone". The message layer overrides it with (0x78,0x78,0x78). */
    ctx->rgb[0] = ctx->rgb[1] = ctx->rgb[2] = 0x80;
}

void q2_hud_ctx_centre_in(q2_hud_ctx *ctx, int out_w, int out_h)
{
    if (!ctx)
        return;
    q2_hud_ctx_default(ctx, Q2_HUD_SPACE_W, Q2_HUD_SPACE_H);
    ctx->origin_x = (s16)((out_w - Q2_HUD_SPACE_W) / 2);
    ctx->origin_y = (s16)((out_h - Q2_HUD_SPACE_H) / 2);
}

void q2_hud_pen_default(q2_hud_pen *pen)
{
    if (!pen)
        return;
    memset(pen, 0, sizeof(*pen));
    /* The initialised values in the data image at 0x800AE8DC..0x800AE8F4. */
    pen->wrap          = false;
    pen->left          = 0x6A;
    pen->right         = 0x196;
    pen->space_advance = 4;
    pen->glyph_advance = 8;
    pen->palette       = Q2_HUD_PALETTE_FONT_ALT;
    pen->palette_sub   = 0;
}

/* ------------------------------------------------------------------------- */
/* The markup interpreter — 0x80042328                                        */
/* ------------------------------------------------------------------------- */
/*
 * Hex digits, with the original's classification: anything >= 'a' folds as
 * lower case, 58..96 as upper case, and everything below '9'+1 as decimal. It
 * is sloppy — ':' decodes as 0x0A — but it is what the parsers at 0x80041DD4,
 * 0x80041F94 and 0x80042110 do, and one of them is fed `%2X` output.
 */
static int hexval(u8 c)
{
    if (c >= 'a')
        return (int)c - 87;
    if (c >= 58)
        return (int)c - 55;
    return (int)c - 48;
}

/* Where the glyph cells went, for the `*N` backdrop pass. The original keeps
 * this at 0x800C8CDC and never bounds it; 256 cells is more than any string it
 * ships and the overflow is reported rather than written past. */
#define HUD_CELL_MAX 256

typedef struct hud_cell { s16 x, y; } hud_cell;

static psx_prim *emit_sprite(psx_ot *ot, u16 otz, const q2_hud_font *font,
                             const q2_hud_ctx *ctx,
                             int x, int y, int w, int h, int u, int v,
                             u16 clut, const u8 rgb[3])
{
    psx_prim *p = psx_ot_add(ot, otz);

    if (!p)
        return NULL;

    x += ctx->origin_x;
    y += ctx->origin_y;

    p->kind  = PSX_PRIM_SPRT;
    p->tpage = font->tpage | 0x0600u;
    p->set_tpage = true; /* the text renderer's preceding DR_TPAGE */
    p->clut  = clut;

    p->xy[0].x = (s16)x;          p->xy[0].y = (s16)y;
    p->xy[1].x = (s16)(x + w);    p->xy[1].y = (s16)y;
    p->xy[2].x = (s16)(x + w);    p->xy[2].y = (s16)(y + h);
    p->xy[3].x = (s16)x;          p->xy[3].y = (s16)(y + h);

    p->uv[0].u = (u8)u;                p->uv[0].v = (u8)v;
    p->uv[1].u = (u8)(u + w);          p->uv[1].v = (u8)v;
    p->uv[2].u = (u8)(u + w);          p->uv[2].v = (u8)(v + h);
    p->uv[3].u = (u8)u;                p->uv[3].v = (u8)(v + h);

    p->rgb[0].r = rgb[0];
    p->rgb[0].g = rgb[1];
    p->rgb[0].b = rgb[2];

    /* Codes 0x74 / 0x64 / 0x7C: textured, modulated by the primitive colour,
     * ABE clear. None of the overlay's sprites are semi-transparent. */
    p->semi_transparent = false;
    p->textured_blend   = true;
    return p;
}

/* The `&` arm at 0x800430F4. Draws one or two icons and advances the pen by
 * each icon's own width. */
static const char *draw_icons(const q2_hud_font *font, const q2_hud_ctx *ctx,
                              q2_hud_pen *pen, psx_ot *ot, u16 otz,
                              const char *p)
{
    u8 icons[Q2_HUD_ICON_ESCAPE_MAX];
    int n, i;
    u16 clut;
    const q2_hud_palette *pal;

    n = q2_hud_icon_escape(*p, icons);
    p++;

    pal  = q2_hud_palette_get(font->tab, (u32)pen->palette);
    clut = pal ? pal->clut_id : font->clut_font;

    for (i = 0; i < n; i++) {
        const q2_hud_icon *ic;

        if (icons[i] >= Q2_HUD_ICON_COUNT)
            continue;
        ic = &font->tab->icon[icons[i]];

        if (!emit_sprite(ot, otz, font, ctx, pen->x, pen->y, ic->w, ic->h,
                         ic->u, ic->v, clut, ctx->rgb)) {
            pen->overflow = true;
            break;
        }
        pen->x = (s16)(pen->x + ic->w);
    }
    return p;
}

static bool is_word_char(u8 c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9');
}

u32 q2_hud_print(const q2_hud_font *font, q2_hud_ctx *ctx, q2_hud_pen *pen,
                 psx_ot *ot, u16 otz, const char *text)
{
    hud_cell cell[HUD_CELL_MAX];
    u32 cells = 0;
    int box_level = 0;          /* s7 */
    bool at_word_start = true;  /* s4 */
    const int line_height = 8;  /* fp */
    const int y_reset = 16;     /* s6 */
    const char *p;

    if (!font || !font->tab || !ctx || !pen || !ot || !text)
        return 0;

    pen->overflow = false;
    pen->x = ctx->home_x;
    pen->y = ctx->home_y;

    for (p = text; *p; ) {
        u8 c = (u8)*p++;
        int index;
        bool newline = false;

        switch (c) {
        case '*':                               /* 0x80042524 */
            box_level = (int)((u8)*p++) - '0';
            continue;

        case '#':                               /* 0x80042110 */
            pen->left  = (hexval((u8)p[0]) << 8) | (hexval((u8)p[1]) << 4) |
                          hexval((u8)p[2]);
            pen->right = (hexval((u8)p[3]) << 8) | (hexval((u8)p[4]) << 4) |
                          hexval((u8)p[5]);
            pen->wrap  = (pen->left | pen->right) != 0;
            p += 6;
            continue;

        case '@':                               /* 0x80041F94 */
            pen->x = (s16)((hexval((u8)p[0]) << 8) | (hexval((u8)p[1]) << 4) |
                            hexval((u8)p[2]));
            pen->y = (s16)((hexval((u8)p[3]) << 4) | hexval((u8)p[4]));
            p += 5;
            continue;

        case '^':                               /* 0x80041DD4 */
            ctx->rgb[0] = (u8)((hexval((u8)p[0]) << 4) | hexval((u8)p[1]));
            ctx->rgb[1] = (u8)((hexval((u8)p[2]) << 4) | hexval((u8)p[3]));
            ctx->rgb[2] = (u8)((hexval((u8)p[4]) << 4) | hexval((u8)p[5]));
            p += 6;
            continue;

        case '&':                               /* 0x800430F4 */
            p = draw_icons(font, ctx, pen, ot, otz, p);
            continue;

        case '|':                               /* 0x800424DC */
            if ((u8)*p == '0') {
                pen->palette     = Q2_HUD_PALETTE_FONT;
                pen->palette_sub = 0;
            } else {
                pen->palette     = Q2_HUD_PALETTE_FONT_ALT;
                pen->palette_sub = ((u8)*p == '1') ? 2 : 1;
            }
            p++;
            continue;

        case '~':                               /* 0x800424B0 */
            {
                u8 d = (u8)*p++;
                pen->space_advance = (unsigned)(d - '1') < 15u ? (int)d - '0' : 4;
            }
            continue;

        case '\r':                              /* 0x80042544 — falls into \n */
        case '\n':                              /* 0x80042560 */
            if (pen->wrap) {
                pen->x = (s16)pen->left;
                if ((u8)*p == ' ')
                    p++;
            } else {
                pen->x = Q2_HUD_MSG_X;
            }
            newline = true;
            break;

        default:
            break;
        }

        if (newline)
            goto advance_line;

        index = (int)c - Q2_HUD_GLYPH_FIRST;

        /*
         * Index 0 is the space, and anything below it is a control character
         * the original also treats as one. `|1` (sub-mode 2) makes the space
         * draw its glyph instead of merely advancing.
         */
        if (index <= 0 && pen->palette_sub != 2) {
            pen->x = (s16)(pen->x + pen->space_advance);
            at_word_start = true;
            goto wrap_check;
        }

        /*
         * Word wrap: at the start of a word, measure ahead over the run of
         * alphanumerics and break the line before drawing if it will not fit.
         * The measure uses the fixed glyph advance, so a word containing an
         * icon escape measures short — as it does on the console.
         */
        if (at_word_start && pen->wrap) {
            const char *q = p - 1;
            int width = pen->x;

            while (is_word_char((u8)*q)) {
                width += pen->glyph_advance;
                q++;
            }
            if ((s16)width >= (s16)pen->right) {
                pen->y = (s16)(pen->y + line_height);
                pen->x = (s16)pen->left;
                if (pen->y >= ctx->height)
                    pen->y = (s16)y_reset;
            }
        }

        at_word_start = false;

        if (index >= Q2_HUD_GLYPH_COUNT) {
            /* The original reads past its 92-entry table here. No shipped
             * string does; we draw nothing rather than sample garbage. */
            pen->x = (s16)(pen->x + pen->glyph_advance);
            goto wrap_check;
        }

        {
            const q2_hud_uv *g = &font->tab->glyph[index];
            const q2_hud_palette *pal =
                q2_hud_palette_get(font->tab, (u32)pen->palette);
            u16 clut = pal ? pal->clut_id : font->clut_font;

            if (!emit_sprite(ot, otz, font, ctx, pen->x, pen->y,
                             Q2_HUD_GLYPH_W, Q2_HUD_GLYPH_H,
                             g->u, g->v, clut, ctx->rgb)) {
                pen->overflow = true;
                break;
            }
        }

        if (cells < HUD_CELL_MAX) {
            cell[cells].x = (s16)(pen->x - 4);
            cell[cells].y = (s16)(pen->y - 4);
            cells++;
        }
        pen->x = (s16)(pen->x + pen->glyph_advance);

    wrap_check:
        if (pen->wrap) {
            if (pen->x < (s16)pen->right)
                continue;
            if (index != 0)     /* only break on a space */
                continue;
            pen->x = (s16)pen->left;
            if ((u8)*p == ' ')
                p++;
        } else {
            if (pen->x < ctx->width)
                continue;
            pen->x = Q2_HUD_MSG_X;
        }

    advance_line:
        pen->y = (s16)(pen->y + line_height);
        if (pen->y >= ctx->height)
            pen->y = (s16)y_reset;
    }

    /*
     * The backdrop pass (0x8004288C). Added after every glyph, so within this
     * bucket it draws first and ends up behind them. Walking the cells
     * backwards is the original's order and is preserved because primitives
     * within a bucket draw in reverse insertion order.
     */
    if (box_level > 0 && box_level < Q2_HUD_BOX_LEVELS && cells > 0) {
        const q2_hud_uv *uv = &font->tab->box[box_level];
        const u8 *rgb = font->tab->box_rgb[box_level];
        u32 i;

        for (i = cells; i-- > 0; ) {
            if (!emit_sprite(ot, otz, font, ctx, cell[i].x, cell[i].y,
                             Q2_HUD_BOX_SIZE, Q2_HUD_BOX_SIZE,
                             uv->u, uv->v, font->clut_box, rgb)) {
                pen->overflow = true;
                break;
            }
        }
    }

    /* The original writes the pen back into the context on the way out. */
    ctx->home_x = pen->x;
    ctx->home_y = pen->y;
    return cells;
}

/*
 * 0x800702A0. Returns strlen - 1 for a non-empty string and 0 for an empty
 * one. This is off by one and it is not corrected: the centre-message layout
 * is built on it, so a "fixed" measure moves every centred line four pixels.
 */
int q2_hud_measure(const char *s)
{
    const char *q;

    if (!s || !*s)
        return 0;
    q = s + 1;
    while (*q)
        q++;
    return (int)(q - s) - 1;
}

/* ------------------------------------------------------------------------- */
/* Overlay state                                                              */
/* ------------------------------------------------------------------------- */
void q2_hud_init(q2_hud *hud, const q2_hud_tables *tab, int players)
{
    if (!hud)
        return;
    memset(hud, 0, sizeof(*hud));

    /* The by-player-count table at 0x8009D648: {0, 4, 2, 1, 1, 0}. */
    hud->msg_max = 4;
    if (tab && players >= 0 && players < Q2_HUD_MSG_TIERS)
        hud->msg_max = tab->message_lines[players];
    if (hud->msg_max == 0)
        hud->msg_max = 1;
    if (hud->msg_max > Q2_HUD_MSG_SLOTS)
        hud->msg_max = Q2_HUD_MSG_SLOTS;

    hud->centre_age = Q2_HUD_CENTRE_TICKS;   /* expired: nothing to draw */

    /*
     * The crosshair. `0x800B3340` is the menu's on/off switch (0x8003A8C4 gates
     * the draw on it). The style pair at gp+796/797 and the colour at
     * gp+800..802 are read but never written anywhere in the image, so the
     * values below are the shipped ones and there is only ever one crosshair:
     * a 16x16 sprite at (0, 0x88) in the atlas, drawn at (127,127,127).
     */
    hud->crosshair = true;
    hud->crosshair_u = 0x00;
    hud->crosshair_v = 0x88;
    hud->crosshair_rgb[0] = 0x7F;
    hud->crosshair_rgb[1] = 0x7F;
    hud->crosshair_rgb[2] = 0x7F;

    q2_hud_pen_default(&hud->pen);
}

void q2_hud_message(q2_hud *hud, const char *text)
{
    int slot;

    if (!hud || !text)
        return;

    /* 0x80042D4C: count++, and if it passes the per-player-count limit the
     * oldest is dropped and the retirement timer restarts. */
    hud->msg_count++;
    if (hud->msg_count > (s16)hud->msg_max) {
        if (hud->msg_count != 0)
            hud->msg_count--;
        hud->msg_expire = 0;
    }

    slot = hud->msg_write;
    memset(hud->msg[slot], 0, Q2_HUD_MSG_LEN);
    strncpy(hud->msg[slot], text, Q2_HUD_MSG_LEN - 1);
    hud->msg[slot][Q2_HUD_MSG_LEN - 1] = '\0';
    hud->msg_age[slot] = 0;

    hud->msg_write = (s16)((hud->msg_write + 1) % Q2_HUD_MSG_SLOTS);
}

/*
 * 0x80042E14. Centre the body on the view and prefix it with a position and a
 * backdrop level.
 *
 * The width arithmetic is the original's, quirk included: the measure is in
 * characters, but when the body starts with an icon escape the icon's PIXEL
 * width is folded into that character count before the whole thing is
 * multiplied by four. A line beginning with "&O" therefore centres as though it
 * were about eighty characters wide. Reproduced, not corrected.
 */
void q2_hud_centre(q2_hud *hud, const q2_hud_tables *tab,
                   const q2_hud_ctx *ctx, const char *body)
{
    int len, x, y;
    const char *amp;
    const char *fmt = "*7@%03X%02X%s";

    if (!hud || !ctx || !body)
        return;

    len = q2_hud_measure(body);

    /* A body that already opens with `|0` picks its own palette and gets no
     * backdrop. */
    if (body[0] == '|' && body[1] == '0')
        fmt = "@%03X%02X%s";

    /*
     * Only the FIRST icon escape is accounted for, and it is accounted for
     * wrongly: the two escape characters come off a character count and the
     * icon's width in PIXELS goes back on, before the whole thing is scaled by
     * four. "Rocket Launcher" is 64 pixels wide, so a line that opens with `&O`
     * centres as though it were sixty-odd characters long. This is the
     * original's arithmetic at 0x80042EB4 and it is left alone.
     */
    amp = strchr(body, '&');
    if (amp && amp[1] && tab) {
        u8 icons[Q2_HUD_ICON_ESCAPE_MAX];
        int n = q2_hud_icon_escape(amp[1], icons);

        if (n > 0 && icons[0] < Q2_HUD_ICON_COUNT)
            len = len - 2 + (int)tab->icon[icons[0]].w;
    }

    if (body[0] == '^')
        len -= 7;

    x = ctx->width / 2 - (len * 4);
    y = ctx->height / 2 + Q2_HUD_CENTRE_DROP;
    if (x < 0)
        x = 0;

    /*
     * "String too long!" — 0x80042F18..0x80042F70. The measure of the body
     * plus the measure of the format plus five must stay under 63 (`slti v0,
     * v0, 63`), or the line is formatted with the module's apology in place of
     * the body, at the position the body would have had. Both measures are
     * 0x800702A0's, one short, so a boxed line takes 46 characters and a `|0`
     * one 48. No string the disc centre-prints is longer than 37 (`q2psx-inspect
     * text`), so retail never shows it; a port-side caller that did would.
     */
    if (q2_hud_measure(body) + q2_hud_measure(fmt) + 5 >= 63)
        body = Q2_HUD_CENTRE_TOO_LONG;

    snprintf(hud->centre, sizeof(hud->centre), fmt, (unsigned)x, (unsigned)y,
             body);
    hud->centre_age = 0;
    hud->centre_live = true;
}

void q2_hud_pickup(q2_hud *hud, const char *item_name)
{
    if (!hud)
        return;

    if (!item_name) {
        hud->pickup[0] = '\0';
        return;
    }

    /*
     * 0x80035B14 formats it and 0x80035B20 draws it, with the position already
     * in the literal: x = 0x03B, y = 0xB8, colour 0x828282, palette 0, wide
     * spaces around the name and tight ones after.
     *
     * The empty name is a real case rather than a guard: the frame a caption
     * expires still formats and draws `@03BB8^828282|0~8~4`, which lays out no
     * glyphs but does leave the pen's palette at 72 and its space width at 4 —
     * and that state persists into whatever draws next (hud.h).
     */
    snprintf(hud->pickup, sizeof(hud->pickup), "@03BB8^828282|0~8%s~4",
             item_name);
}

u32 q2_hud_pickup_build_ot(q2_hud *hud, const q2_hud_font *font,
                           q2_hud_ctx *ctx, psx_ot *ot, u16 otz)
{
    if (!hud || !font || !font->resident || !ctx || !ot)
        return 0;
    if (!hud->pickup[0])
        return 0;

    /*
     * No home, and no colour swap. The `@` sets the pen outright so `home_x` /
     * `home_y` never come into it, and the (0x78,0x78,0x78) the notification
     * layer forces at 0x80042B50 is that layer's alone — this string carries
     * its own `^828282`.
     *
     * The pen is the HUD's own, not a fresh one, because it is $gp state in the
     * original: the `|0` and the `~4` this string ends on are still in force
     * for whatever is drawn after it.
     */
    return q2_hud_print(font, ctx, &hud->pen, ot, otz, hud->pickup);
}

void q2_hud_weapon_selected(q2_hud *hud, const q2_hud_tables *tab, int weapon_id)
{
    char buf[Q2_HUD_MSG_LEN];
    const char *glyph = "  ";

    if (!hud)
        return;
    /* The glyph table is consumed 1-based and slot 0 is deliberately blank, so
     * an unarmed player prints spaces rather than reading off the front. */
    if (tab && weapon_id >= 0 && weapon_id < Q2_HUD_WEAPON_SLOTS)
        glyph = tab->weapon_glyph[weapon_id];

    snprintf(buf, sizeof(buf), "Selected %s", glyph);
    q2_hud_message(hud, buf);
}

/*
 * 0x800254EC's own switch, key mask -> name.
 *
 * It switches on the WHOLE halfword, not on individual bits (`sll 16; sra 16`
 * at 0x800254F0 and then a chain of `beq a0, v0`), so a door authored with two
 * key bits set falls to the default like any other unrecognised value. Bit 3
 * has no case at all and is unused on the disc.
 *
 * The strings are the image's, quoted exactly — including the joke default,
 * which is what the console prints and therefore what this does. Two of them
 * live in the other pool (0x800AE77C, 0x800AE784); the rest run from
 * 0x800ABB60 to 0x800ABBE8.
 */
const char *q2_hud_key_name(u16 key_mask)
{
    switch (key_mask) {
    case 0x0001: return "Blue Key";             /* 0x800ABB60 */
    case 0x0002: return "Red Key";              /* 0x800AE77C */
    case 0x0004: return "Security Pass";        /* 0x800ABB6C */
    case 0x0010: return "Commander's Head";     /* 0x800ABB7C */
    case 0x0020: return "Red Pyramid Key";      /* 0x800ABB90 */
    case 0x0040: return "Purple Pyramid Key";   /* 0x800ABBA0 */
    case 0x0080: return "Data CD";              /* 0x800AE784 */
    case 0x0100: return "Data Spinner";         /* 0x800ABBB4 */
    case 0x0200: return "Green Key";            /* 0x800ABBC4 */
    case 0x0400: return "Yellow Key";           /* 0x800ABBD0 */
    case 0x0800: return "White Key";            /* 0x800ABBDC */
    default:     return "<<Key Error!!>>";      /* 0x800ABBE8 */
    }
}

/*
 * The rest of 0x800254EC: "You need the %s" (0x800ABBF8) formatted into the
 * scratch buffer at [0x800B2DFC] and handed to 0x80043570, which re-formats it
 * into 0x800C8DDC and calls 0x80042E14 with it.
 *
 * 0x80042E14 IS the centre line, not the notification ring — that is
 * 0x80042D4C, a different entry — and this used to route here through
 * q2_hud_message, so a refusal that did reach the HUD would have queued behind
 * pickups in the corner instead of being put in front of the player. Nothing
 * reached it at all: the caller did not exist.
 */
void q2_hud_need_key(q2_hud *hud, const q2_hud_tables *tab,
                     const q2_hud_ctx *ctx, u16 key_mask)
{
    char buf[Q2_HUD_CENTRE_LEN];

    if (!hud || !ctx)
        return;
    snprintf(buf, sizeof(buf), "You need the %s", q2_hud_key_name(key_mask));
    q2_hud_centre(hud, tab, ctx, buf);
}

bool q2_hud_track(q2_hud *hud, s16 health, s16 armour)
{
    int strength;
    bool raised = false;

    if (!hud)
        return false;

    if (!hud->have_last) {
        hud->last_health = health;
        hud->last_armour = armour;
        hud->have_last   = true;
        return false;
    }

    /*
     * 0x8003AE10. The health test gates the whole thing, then the armour test
     * picks which of the two arms runs — so a hit that costs both health and
     * armour flashes grey, not red.
     */
    if (health < hud->last_health || armour < hud->last_armour) {
        if (armour < hud->last_armour) {
            strength = (armour - hud->last_armour) / 2 + 2;
            if ((u8)strength >= 6)
                strength = Q2_HUD_FLASH_MAX;
            memcpy(hud->flash.rgb, q2_hud_flash_armour_rgb, 3);
        } else {
            strength = (hud->last_health - health) / 2 + 2;
            if ((u8)strength >= 6)
                strength = Q2_HUD_FLASH_MAX;
            memcpy(hud->flash.rgb, q2_hud_flash_health_rgb, 3);
        }
        if (strength > 0) {
            hud->flash.strength = (s16)strength;
            hud->flash.initial  = (s16)strength;
            hud->flash.mode     = Q2_HUD_FLASH_MODE_SCALE;
            raised = true;
        }
    }

    hud->last_health = health;
    hud->last_armour = armour;
    return raised;
}

/* ------------------------------------------------------------------------- */
/* FlashPowerUp — 0x80039870                                                  */
/* ------------------------------------------------------------------------- */
const u8 q2_hud_flash_powerup_rgb[Q2_HUD_POWERUP_SLOTS][3] = {
    { 0x28, 0x00, 0x28 },   /* 0x800AE874 — quad damage, purple              */
    { 0x1E, 0x1E, 0x00 },   /* 0x800AE878 — invulnerability, yellow          */
    { 0x00, 0x23, 0x00 },   /* 0x800AE87C — environment suit, green          */
    { 0x00, 0x0A, 0x00 }    /* 0x800AE880 — rebreather, a fainter green      */
};

q2_hud_powerup_flash q2_hud_powerup_flash_tick(
    const s32 until[Q2_HUD_POWERUP_SLOTS], s32 now,
    s32 *quiet_until, s16 *warn_secs)
{
    q2_hud_powerup_flash out;
    s32 secs;
    int i;

    memset(&out, 0, sizeof(out));
    out.slot = -1;
    if (!until || !quiet_until || !warn_secs)
        return out;

    /*
     * 0x800398AC, 0x800398FC, 0x80039948, 0x80039990 — four `sltu`s against the
     * level clock, each jumping to the shared tail. The FIRST live deadline
     * wins and the chain stops there, so a player holding quad and the suit at
     * once is tinted purple and never green. Unsigned, as the compares are.
     */
    for (i = 0; i < Q2_HUD_POWERUP_SLOTS; i++) {
        if ((u32)now < (u32)until[i]) {
            out.slot = i;
            break;
        }
    }

    /*
     * 0x80039994 `beq v0, zero, 0x80039A8C`: with nothing held the fourth test
     * branches past everything to the epilogue. Neither carried field is
     * touched, which is why a powerup that ends inside its own blink leaves
     * client+0xC4 armed — harmlessly, since nothing reads it again until the
     * next powerup, by which time the clock has passed it.
     */
    if (out.slot < 0)
        return out;

    memcpy(out.rgb, q2_hud_flash_powerup_rgb[out.slot], 3);

    /*
     * 0x800399C4..0x800399D0: `multu` by 0x1B4E81B5, `mfhi`, `srl 5` — the
     * compiler's unsigned divide by 300, which is whole seconds of life left,
     * truncated toward zero.
     */
    secs = (s32)(((u32)until[out.slot] - (u32)now) / Q2_HUD_POWERUP_SECOND);

    /*
     * 0x800399D4 `slti v0, v1, 3` and 0x800399E8's equality against the
     * halfword at client+0xDC: under three seconds, and only on the frame the
     * count first changes. The store, the 150-unit quiet window and the sound
     * are 0x800399F8, 0x800399FC and 0x80039A00.
     */
    if (secs < Q2_HUD_POWERUP_WARN && secs != (s32)*warn_secs) {
        *warn_secs   = (s16)secs;
        *quiet_until = now + Q2_HUD_POWERUP_QUIET;
        out.warn     = true;
        return out;             /* 0x80039A08 — and no tint on this frame */
    }

    /*
     * 0x80039A20 `sltu v0, v0, v1` with client+0xC4 on the left: STRICTLY
     * less, so the frame the quiet window ends on is still dark. 0x80039A88
     * clears the field in the flashing branch's delay slot, which is what makes
     * the ordinary case — a zero deadline against a running clock — re-arm the
     * tile every single frame.
     */
    if ((u32)*quiet_until < (u32)now) {
        out.tint     = true;
        *quiet_until = 0;
    }

    return out;
}

void q2_hud_tick(q2_hud *hud, int frames, s32 dt)
{
    int i;

    if (!hud)
        return;

    if (frames > 0) {
        /* 0x80042BE4: one line retires every 60 FRAMES — 0x80042BEC is
         * `addiu v1, v0, 1`, a flat increment, not the frame delta. */
        hud->msg_expire = (s16)(hud->msg_expire + frames);
        while (hud->msg_expire >= Q2_HUD_MSG_TICKS) {
            hud->msg_expire = (s16)(hud->msg_expire - Q2_HUD_MSG_TICKS);
            if (hud->msg_count > 0)
                hud->msg_count--;
            else
                break;
        }
        if (hud->msg_count == 0)
            hud->msg_expire = 0;

        for (i = 0; i < Q2_HUD_MSG_SLOTS; i++)
            if (hud->msg_age[i] < 0x7000)
                hud->msg_age[i] = (s16)(hud->msg_age[i] + frames);
    }

    /*
     * 0x80042B90: the centre line ages BY THE FRAME DELTA, not by one.
     * 0x80042BA8 loads the halfword at [0x800B2DB4] and 0x80042BB0 adds it, so
     * the 1200 at 0x80042B98 is four seconds of the 300-to-the-second clock —
     * and at the PAL rate of 12 per frame, a hundred drawn frames.
     *
     * This used to advance by the same flat one the ring does, which at the
     * port's 30 Hz step made every level message last 1200 frames: forty
     * seconds on screen instead of four, with the `*7` backdrop sitting at `*0`
     * for thirty-nine of them.
     *
     * AND THE TEST COMES FIRST. 0x80042B98 compares the age against 1200,
     * 0x80042BB0 adds the delta, and 0x80042BB4 draws — so the frame on which
     * the age reaches 1200 is still drawn, and the line is up for 1200 / dt
     * frames, 120 at this port's 30 Hz. Testing the aged value at the draw, as
     * this did, dropped that last frame. `centre_live` is the test as the
     * console makes it.
     */
    hud->centre_live = hud->centre_age < Q2_HUD_CENTRE_TICKS;
    if (dt > 0 && hud->centre_age < Q2_HUD_CENTRE_TICKS)
        hud->centre_age += dt;
}

/* Decrement a leading `*N` towards `*0`, which is how the backdrop fades. The
 * original does this in place in the message buffer after drawing it. */
static void fade_box(char *s)
{
    if (s[0] == '*' && (u8)s[1] >= '1')
        s[1] = (char)(s[1] - 1);
}

/*
 * The damage flash is NOT drawn here, and that is a correction rather than an
 * omission. `0x80076764` is called by the per-viewport draw (`0x80076CC8`), not
 * by the overlay: the tile is sized to the viewport, linked into the viewport's
 * own slice at its frontmost bucket, and its state lives in the view record at
 * +672…+680. Drawing it with the overlay puts it in the wrong slice, at the
 * wrong size in a split, and in front of the wrong things.
 *
 * What stays here is what raises it — q2_hud_track — because that is the HUD's
 * own arithmetic. The tile itself is in src/screen/screen.c, which also carries
 * the three flash modes this never implemented. A caller joins the two with
 * q2_screen_flash_set.
 */

/*
 * THE CROSSHAIR IS THE BACK OF THE OVERLAY, NOT THE FRONT.
 *
 * It is the one overlay element that sits at the middle of the screen, which is
 * exactly where every framed screen this port draws puts its panel: the
 * briefing, the end-of-mission placard and the objectives pop-up all compose
 * through q2_briefing_build_ot at depths 2/1/0, and the centre line and the
 * notifications of this very function land at the caller's own depth. Emitted
 * at that same depth the crosshair won a bucket it should never win — within
 * one bucket the LAST primitive added is drawn FIRST (gpu.h), so the crosshair,
 * which goes in before any of them, came out in front of all of them, a grey
 * cross sitting on top of the objectives board.
 *
 * So it is pushed back by Q2_HUD_CROSSHAIR_DEPTH. That is a depth, larger being
 * farther, and it clears the deepest thing the overlay screens use (the panel
 * body, at 2) with a slot to spare. It stays well inside the overlay slice —
 * the depths that resolve in front of the overlay's own draw env run to dozens,
 * not to four — so the crosshair is still in front of the world and of the
 * status bar, which is where it belongs. What it is now behind is everything
 * the overlay draws over it, which is what the pop-up needs.
 */
static void emit_crosshair(const q2_hud *hud, const q2_hud_font *font,
                           const q2_hud_ctx *ctx, psx_ot *ot, u16 otz)
{
    int x, y;

    if (!hud->crosshair)
        return;

    /* 0x80043A98: half the view, less half a sprite, plus the view's own
     * offset pair. */
    x = ctx->width / 2 - 8 + hud->crosshair_dx;
    y = ctx->height / 2 - 8 + hud->crosshair_dy;

    emit_sprite(ot, (u16)(otz + Q2_HUD_CROSSHAIR_DEPTH), font, ctx,
                x, y, 16, 16,
                hud->crosshair_u, hud->crosshair_v,
                font->clut_font, hud->crosshair_rgb);
}

void q2_hud_build_ot(q2_hud *hud, const q2_hud_font *font, q2_hud_ctx *ctx,
                     psx_ot *ot, u16 otz)
{
    q2_hud_pen *pen = &hud->pen;
    u8 saved_rgb[3];
    int i;

    if (!hud || !font || !font->resident || !ctx || !ot)
        return;

    /* 0x80042B50: the message layer swaps the context colour for (0x78,0x78,
     * 0x78) and restores it on the way out, so a `^` inside a message does not
     * leak into whatever draws next. */
    memcpy(saved_rgb, ctx->rgb, 3);
    ctx->rgb[0] = ctx->rgb[1] = ctx->rgb[2] = 0x78;

    emit_crosshair(hud, font, ctx, ot, otz);

    /* The centre line, while its age was under 1200 before this frame's delta
     * went on (q2_hud_tick). */
    if (hud->centre_live && hud->centre[0]) {
        ctx->home_x = 0;
        ctx->home_y = 0;
        q2_hud_print(font, ctx, pen, ot, otz, hud->centre);
        fade_box(hud->centre);
    }

    /* Notifications, newest at the top of the stack. */
    for (i = hud->msg_count - 1; i >= 0; i--) {
        int slot = ((hud->msg_write - 1 - (hud->msg_count - 1 - i)) % Q2_HUD_MSG_SLOTS
                    + Q2_HUD_MSG_SLOTS) % Q2_HUD_MSG_SLOTS;

        ctx->home_x = Q2_HUD_MSG_X;
        ctx->home_y = (s16)(Q2_HUD_MSG_SPACING * i + (ctx->height >> 5) +
                            Q2_HUD_MSG_TOP);
        q2_hud_print(font, ctx, pen, ot, otz, hud->msg[slot]);
        fade_box(hud->msg[slot]);
    }

    memcpy(ctx->rgb, saved_rgb, 3);
}
