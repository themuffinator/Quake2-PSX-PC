#include "statusbar.h"

#include "hudtables.h"

#include <string.h>

/*
 * The seventeen fields, in the order the original builds them on the stack at
 * `0x800337EC`…`0x80033A5C`. The order is the executable's, not sorted by x —
 * kept that way so the table can be checked against the disassembly line for
 * line rather than after a rearrangement.
 */
const q2_sbar_field q2_sbar_fields[Q2_SBAR_FIELDS] = {
    /* --- the main row, at the anchor ---------------------------------- */
    {   0,  0, 38, 14 },   /*  0  health icon, initialised 38 wide      */
    { -71,  1,  8, 14 },   /*  1  health digit 0                        */
    { -47,  1,  8, 14 },   /*  2  health digit 1                        */
    { -23,  1,  8, 14 },   /*  3  health digit 2                        */
    { 135,  0,  8, 14 },   /*  4  ammo icon                             */
    {  64,  1,  8, 14 },   /*  5  ammo digit 0                          */
    {  88,  1,  8, 14 },   /*  6  ammo digit 1                          */
    { 112,  1,  8, 14 },   /*  7  ammo digit 2                          */
    { 250,  0,  8, 14 },   /*  8  armour icon                           */
    { 179,  1,  8, 14 },   /*  9  armour digit 0                        */
    { 203,  1,  8, 14 },   /* 10  armour digit 1                        */
    { 227,  1,  8, 14 },   /* 11  armour digit 2                        */
    { 330,  0,  8, 14 },   /* 12  far right — the FRAG count in DM      */
    /* --- the upper row, 24 to 25 above it ----------------------------- */
    { 330, -25,  8, 14 },  /* 13  upper-right icon                      */
    { 256, -24,  8, 14 },  /* 14  upper-right digit 0                   */
    { 280, -24,  8, 14 },  /* 15  upper-right digit 1                   */
    { -71, -25,  9, 14 }   /* 16  upper-LEFT icon, initialised 9 wide   */
};

/*
 * The stacked two-player layout, from `0x80033D30`. The previous transcription
 * stopped after record eight; the function actually builds sixteen and passes
 * groups 0..3, 4..7, 8..11 and 13..15 to health, ammo, armour and signed frags.
 */
const q2_sbar_field q2_sbar_fields_2h[Q2_SBAR_FIELDS_2H] = {
    { 106,  1, 38, 14 }, {  46, 0, 8, 14 },
    {  66,  0,  8, 14 }, {  86, 0, 8, 14 },
    { 230,  1, 38, 14 }, { 170, 0, 8, 14 },
    { 190,  0,  8, 14 }, { 210, 0, 8, 14 },
    { 354,  1,  8, 14 }, { 294, 0, 8, 14 },
    { 314,  0,  8, 14 }, { 334, 0, 8, 14 },
    { 400,  0,  8, 14 }, { 422, 0, 8, 14 },
    { 442,  0,  8, 14 }, { 462, 0, 8, 14 }
};

/*
 * The side-by-side two-player layout, from `0x80034288` — formerly mislabelled
 * as quad. The selector at 0x8003FA10 hands this callback only to layout_two_v.
 * Records 8..11 and 13..15 use `anchor_y + 40 - screen_height`; `dy` below is
 * the constant part and q2_sbar_2v_fields() performs the live subtraction.
 */
const q2_sbar_field q2_sbar_fields_2v[Q2_SBAR_FIELDS_2V] = {
    {  76, 1, 38, 14 }, {  16, 0, 8, 14 },
    {  36, 0,  8, 14 }, {  56, 0, 8, 14 },
    { 230, 1, 38, 14 }, { 170, 0, 8, 14 },
    { 190, 0,  8, 14 }, { 210, 0, 8, 14 },
    {  76, Q2_SBAR_2V_UPPER_DY, 8, 14 },
    {  16, Q2_SBAR_2V_UPPER_DY, 8, 14 },
    {  36, Q2_SBAR_2V_UPPER_DY, 8, 14 },
    {  56, Q2_SBAR_2V_UPPER_DY, 8, 14 },
    { 400, 0, 8, 14 },
    { 210, Q2_SBAR_2V_UPPER_DY, 8, 14 },
    { 170, Q2_SBAR_2V_UPPER_DY, 8, 14 },
    { 190, Q2_SBAR_2V_UPPER_DY, 8, 14 }
};

/* 0x80034830 indexes these as `view * 11 + field`. The first and third rows
 * share x, as do the second and fourth; the y table is {110,110,1,1}. */
const q2_sbar_field
q2_sbar_fields_quad[Q2_SBAR_QUAD_VIEWS][Q2_SBAR_FIELDS_QUAD] = {
    {
        { 56,110,38,14 }, { 16,110,8,14 }, { 30,110,8,14 },
        { 44,110, 8,14 }, {142,110,38,14 }, {100,110,8,14 },
        {114,110, 8,14 }, {128,110,8,14 }, {198,110,8,14 },
        {212,110, 8,14 }, {238,110,8,14 }
    },
    {
        {208,110,38,14 }, {168,110,8,14 }, {182,110,8,14 },
        {196,110, 8,14 }, {112,110,38,14 }, { 70,110,8,14 },
        { 84,110, 8,14 }, { 98,110,8,14 }, {  6,110,8,14 },
        { 20,110, 8,14 }, { 34,110,8,14 }
    },
    {
        { 56,1,38,14 }, { 16,1,8,14 }, { 30,1,8,14 },
        { 44,1, 8,14 }, {142,1,38,14 }, {100,1,8,14 },
        {114,1, 8,14 }, {128,1,8,14 }, {198,1,8,14 },
        {212,1, 8,14 }, {238,1,8,14 }
    },
    {
        {208,1,38,14 }, {168,1,8,14 }, {182,1,8,14 },
        {196,1, 8,14 }, {112,1,38,14 }, { 70,1,8,14 },
        { 84,1, 8,14 }, { 98,1,8,14 }, {  6,1,8,14 },
        { 20,1, 8,14 }, { 34,1,8,14 }
    }
};

/*
 * The weapon strip's absolute positions, the four s16 pairs at 0x8009C658.
 * Absolute, not anchor-relative — see statusbar.h.
 */
const q2_sbar_strip_pos q2_sbar_strip[Q2_SBAR_STRIP_SLOTS] = {
    { 388, 201 }, { 458, 201 }
};
const q2_sbar_strip_pos q2_sbar_strip_2p[Q2_SBAR_STRIP_SLOTS] = {
    { 381, 95 }, { 419, 95 }
};

bool q2_sbar_field_2v_is_upper(int field)
{
    return (field >= 8 && field <= 11) || (field >= 13 && field <= 15);
}

void q2_sbar_2v_fields(int screen_h,
                       q2_sbar_field out[Q2_SBAR_FIELDS_2V])
{
    int i;

    if (!out)
        return;
    memcpy(out, q2_sbar_fields_2v, sizeof(q2_sbar_fields_2v));
    for (i = 0; i < Q2_SBAR_FIELDS_2V; i++)
        if (q2_sbar_field_2v_is_upper(i))
            out[i].dy = (s16)(out[i].dy - screen_h);
}

void q2_sbar_quad_fields(int view, int frags,
                         q2_sbar_field out[Q2_SBAR_FIELDS_QUAD])
{
    if (!out)
        return;
    if (view < 0 || view >= Q2_SBAR_QUAD_VIEWS)
        view = 0;
    memcpy(out, q2_sbar_fields_quad[view],
           sizeof(q2_sbar_fields_quad[view]));

    /* 0x80034D24..0x80034DA8: on the right-hand views only, keep one- and
     * two-character frag values against the inner edge. A negative value uses
     * two characters (minus and magnitude), including -5. */
    if (view == 1 || view == 3) {
        if (frags >= 0 && frags < 10) {
            out[10].dx = out[8].dx;
        } else if (frags < 100) {
            out[9].dx  = out[8].dx;
            out[10].dx = q2_sbar_fields_quad[view][9].dx;
        }
    }
}

/*
 * Counter n's three digits and its icon, as indices into the table above. The
 * grouping is what makes the layout legible: sorted by x the fields read
 * digit, digit, digit, icon three times over, with the icon 24 past the last
 * digit exactly as the digits are 24 apart.
 */
static const u8 k_digit_field[Q2_SBAR_COUNTERS][Q2_SBAR_COUNTER_DIGITS] = {
    { 1, 2, 3 },
    { 5, 6, 7 },
    { 9, 10, 11 }
};
static const u8 k_icon_field[Q2_SBAR_COUNTERS] = { 0, 4, 8 };

int q2_sbar_digit_field(q2_sbar_counter c, int digit)
{
    if ((int)c < 0 || (int)c >= Q2_SBAR_COUNTERS)
        return -1;
    if (digit < 0 || digit >= Q2_SBAR_COUNTER_DIGITS)
        return -1;
    return k_digit_field[c][digit];
}

int q2_sbar_icon_field(q2_sbar_counter c)
{
    if ((int)c < 0 || (int)c >= Q2_SBAR_COUNTERS)
        return -1;
    return k_icon_field[c];
}

/* ------------------------------------------------------------------------- */
int q2_sbar_digits_of(int value, u8 out[Q2_SBAR_COUNTER_DIGITS])
{
    int n = 0, i;
    u8 tmp[Q2_SBAR_COUNTER_DIGITS];

    if (!out)
        return 0;

    if (value < 0)
        value = 0;
    /* Three cells is the whole counter, so 999 is the ceiling the layout
     * imposes. The console's own maxima are lower than that everywhere. */
    if (value > 999)
        value = 999;

    do {
        tmp[n++] = (u8)(value % 10);
        value /= 10;
    } while (value > 0 && n < Q2_SBAR_COUNTER_DIGITS);

    /* Most significant first, and no leading zeroes — capture shows "2" as one
     * numeral rather than "002". */
    for (i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

void q2_sbar_frag_glyphs(int frags, u8 out[Q2_SBAR_COUNTER_DIGITS])
{
    int negative;
    int value;

    if (!out)
        return;

    if (frags < -99)
        frags = -99;                 /* 0x80037DB8..0x80037DC4 */
    if (frags > 999)
        frags = 999;                 /* outside the authored match range */

    negative = frags < 0;
    value = negative ? -frags : frags;

    out[0] = value >= 100 ? (u8)(value / 100) : Q2_SBAR_GLYPH_BLANK;
    value %= 100;
    out[1] = (out[0] != Q2_SBAR_GLYPH_BLANK || value >= 10)
                 ? (u8)(value / 10)
                 : Q2_SBAR_GLYPH_BLANK;
    out[2] = (u8)(value % 10);

    /* 0x80034F58..0x80034F84: put the sign immediately before the first
     * significant digit, so -5 is {blank, minus, 5} and -15 is {minus,1,5}. */
    if (negative) {
        if (out[1] == Q2_SBAR_GLYPH_BLANK)
            out[1] = Q2_SBAR_GLYPH_MINUS;
        else
            out[0] = Q2_SBAR_GLYPH_MINUS;
    }
}

/* ------------------------------------------------------------------------- */
void q2_statusbar_init(q2_statusbar *b, const q2_icon_tables *icons,
                       int players)
{
    if (!b)
        return;
    memset(b, 0, sizeof(*b));
    b->icons   = icons;
    b->players = players > 0 ? players : 1;
    b->layout  = b->players >= 3 ? Q2_SBAR_LAYOUT_QUAD
                                 : (b->players == 2 ? Q2_SBAR_LAYOUT_TWO_H
                                                    : Q2_SBAR_LAYOUT_ONE);
    b->screen_h = 248;
    b->visible = true;
}

void q2_statusbar_set_palettes(q2_statusbar *b, const struct q2_hud_tables *t)
{
    if (!b)
        return;
    b->hud = t;
}

void q2_statusbar_anchor(q2_statusbar *b, s16 x, s16 y)
{
    if (!b)
        return;
    b->anchor_x = x;
    b->anchor_y = y;
}

void q2_statusbar_layout(q2_statusbar *b, q2_sbar_layout layout,
                         int view_index, int screen_h)
{
    if (!b)
        return;
    if (layout < Q2_SBAR_LAYOUT_ONE || layout > Q2_SBAR_LAYOUT_QUAD)
        layout = Q2_SBAR_LAYOUT_ONE;
    b->layout = layout;
    b->view_index = (view_index >= 0 && view_index < Q2_SBAR_QUAD_VIEWS)
                        ? view_index : 0;
    if (screen_h > 0)
        b->screen_h = screen_h;
}

/* ------------------------------------------------------------------------- */
/* The armour field — 0x80035554                                              */
/* ------------------------------------------------------------------------- */
/*
 * The five-way select, in the console's own test order.
 *
 *     8003564C  andi 0x8000  -> 8003565C  lbu 150(a0)   rect 30  power shield
 *     8003576C  andi 0x4000  -> 8003577C  lbu 130(t0)   rect 26  body
 *     80035800  andi 0x2000  -> 80035810  lbu 135(t0)   rect 27  combat
 *     80035894  andi 0x1000  -> 800358A4  lbu 140(t0)   rect 28  jacket
 *     80035928  (fall through)              base + 0    rect 0   blank
 *
 * BODY FIRST is not a stylistic choice. The pickup handlers only raise the bit
 * for their own class when no STRONGER bit is up (item.c, and 0x80036C28 /
 * 0x80036CA0 / 0x80036D08), so a player who has worn body armour keeps 0x4000
 * raised while wearing combat. Testing weakest-first would draw the grey vest
 * for a player in red.
 *
 * The power arm's `else` is not the blank by omission: 0x800356E0 reads the
 * rect table's byte 0, i.e. rect 0, explicitly. So the power state with the
 * shield bit down — reachable only through Q2_INV_POWER_SCREEN, which has no
 * touch handler on this disc — draws nothing.
 */
u8 q2_sbar_armour_icon(u32 inv_flags, bool show_power)
{
    if (show_power)
        return (inv_flags & Q2_INV_POWER_SHIELD) ? Q2_SBAR_ICON_POWER_SHIELD
                                                 : Q2_SBAR_ICON_NONE;

    if (inv_flags & Q2_INV_ARMOUR_BODY)   return Q2_SBAR_ICON_ARMOUR_BODY;
    if (inv_flags & Q2_INV_ARMOUR_COMBAT) return Q2_SBAR_ICON_ARMOUR_COMBAT;
    if (inv_flags & Q2_INV_ARMOUR_JACKET) return Q2_SBAR_ICON_ARMOUR_JACKET;
    return Q2_SBAR_ICON_NONE;
}

void q2_statusbar_armour_state(q2_statusbar *b, u32 inv_flags)
{
    const u32 power = Q2_INV_POWER_SHIELD | Q2_INV_POWER_SCREEN;

    if (!b)
        return;

    /*
     * 0x80035594 / 0x8003559C: no cells, no power state. This is the FIRST
     * test after the class-bit clear, so an empty shield falls back to the
     * vest rather than showing an empty power readout.
     */
    if (b->cells == 0) {
        b->showing_power = false;
    }
    else if (inv_flags & Q2_INV_ARMOUR_MASK) {
        /*
         * Both held: alternate. 0x800355C8 compares the level clock against
         * the deadline at client+192 and, when it has passed, flips
         * client+88 and rewrites the deadline to clock + 300 — one second.
         *
         * `(s32)(ticks - deadline) > 0` rather than `ticks > deadline` so a
         * clock that wraps does not park the toggle for two billion ticks.
         */
        if ((inv_flags & power) &&
            (s32)(b->ticks - b->power_toggle_at) > 0) {
            b->showing_power   = !b->showing_power;
            b->power_toggle_at = b->ticks + Q2_SBAR_POWER_ALTERNATE;
        }
    }
    else if (inv_flags & power) {
        /* 0x80035618: a power item and no armour class pins the power arm. */
        b->showing_power = true;
    }

    b->armour_icon = q2_sbar_armour_icon(inv_flags, b->showing_power);
}

/* ------------------------------------------------------------------------- */
/* The powerup timer — 0x80035B38                                             */
/* ------------------------------------------------------------------------- */
void q2_statusbar_powerup_state(q2_statusbar *b, const q2_inventory *inv)
{
    static const u8 k_icon[] = {
        Q2_SBAR_ICON_POWERUP_QUAD,
        Q2_SBAR_ICON_POWERUP_INVULN,
        Q2_SBAR_ICON_POWERUP_ENVIRO,
        Q2_SBAR_ICON_POWERUP_BREATHER
    };
    s32 until[sizeof(k_icon) / sizeof(k_icon[0])];
    size_t i;

    if (!b)
        return;

    b->powerup_icon    = Q2_SBAR_ICON_NONE;
    b->powerup_seconds = 0;
    if (!inv)
        return;

    /* These are client+0xAC through +0xB8, in the exact walk order at
     * 0x80035B58. Do not reorder them by the strength of an effect: four live
     * timers still display only the first word in this sequence. */
    until[0] = inv->quad_until;
    until[1] = inv->invuln_until;
    until[2] = inv->enviro_until;
    until[3] = inv->breather_until;

    for (i = 0; i < sizeof(until) / sizeof(until[0]); i++) {
        u32 left;

        /* `sltu now, deadline`: the deadline tick itself is inactive. */
        if (b->ticks >= (u32)until[i])
            continue;

        left = (u32)until[i] - b->ticks;
        b->powerup_icon = k_icon[i];
        b->powerup_seconds = (u8)(left / Q2_SBAR_POWERUP_SECONDS_TICKS);
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* One cell of the sheet, as the emitter at 0x80033320 draws it: a POLY_GT4    */
/* whose four corners carry the same colour.                                  */
/* ------------------------------------------------------------------------- */
static u32 emit_cell(psx_ot *ot, u32 bucket, u16 tpage, u16 clut,
                     int x, int y, u8 u, u8 v, u8 sw, u8 sh, u8 dw, u8 dh)
{
    psx_prim *p;
    int i;

    if (sw == 0 || sh == 0 || dw == 0 || dh == 0)
        return 0;

    p = psx_ot_add_bucket(ot, bucket);
    if (!p)
        return 0;

    p->kind  = PSX_PRIM_GT4;
    p->tpage = tpage;
    p->clut  = clut;
    p->textured_blend = true;

    /* Perimeter order and HALF-OPEN corners — the same two conventions the
     * menu's glyphs need (menufont.c). The `- 1` these used to carry
     * compensated for a rasteriser that filled both edges; raster.c has the
     * GPU's own fill rule now, so the console's numbers go through as they
     * are. */
    p->xy[0].x = (s16)x;              p->xy[0].y = (s16)y;
    p->xy[1].x = (s16)(x + dw);       p->xy[1].y = (s16)y;
    p->xy[2].x = (s16)(x + dw);       p->xy[2].y = (s16)(y + dh);
    p->xy[3].x = (s16)x;              p->xy[3].y = (s16)(y + dh);

    p->uv[0].u = u;                   p->uv[0].v = v;
    p->uv[1].u = (u8)(u + sw);        p->uv[1].v = v;
    p->uv[2].u = (u8)(u + sw);        p->uv[2].v = (u8)(v + sh);
    p->uv[3].u = u;                   p->uv[3].v = (u8)(v + sh);

    for (i = 0; i < 4; i++) {
        p->rgb[i].r = Q2_SBAR_MOD;
        p->rgb[i].g = Q2_SBAR_MOD;
        p->rgb[i].b = Q2_SBAR_MOD;
    }
    return 1;
}

/*
 * A sprite's palette index to the CLUT word the primitive carries.
 *
 * `fallback` is what the bar drew with before per-sprite palettes existed —
 * the single clut the caller passes — and it is still what comes out when no
 * palette bank has been supplied, or when the bank has no such entry. Falling
 * back rather than dropping the sprite matters: a missing palette should show
 * the bar in the wrong colours, not delete the player's health readout.
 */
static u16 pal_clut(const q2_statusbar *b, u32 index, u16 fallback)
{
    u16 c;

    if (!b->hud)
        return fallback;
    c = q2_hud_palette_clut(b->hud, index);
    return c ? c : fallback;
}

/*
 * Whether a counter's digits are showing the low-value flash this frame.
 *
 * Below the threshold the digits take palette 7 while bit 7 of the frame
 * counter at `0x800AEBAC` is CLEAR, and their own palette while it is set — so
 * the readout blinks rather than simply turning red.
 *
 * `solid_at_zero` is HEALTH'S ALONE. Its sub-draw carries a `blez` at
 * 0x80035248 that jumps past the blink test, so a dead player's readout holds
 * the flash steady instead of winking. The ammo path at 0x80035440 has no such
 * branch and — being `sltiu`, unsigned — does not treat zero specially at all,
 * so folding the two into one rule would make an empty magazine stop blinking.
 *
 * NOT MODELLED, and recorded rather than invented: the health sub-draw writes
 * the flash to all three of its digit fields (0x80035268..0x80035270) while the
 * ammo one writes to a single field (0x80035464). Whether that is a deliberate
 * difference or a detail of how the ammo sub-draw is handed its fields has not
 * been read, so the flash is applied per counter here.
 */
static bool counter_is_low(const q2_statusbar *b, int value, int threshold,
                           bool solid_at_zero)
{
    if (value >= threshold)
        return false;
    if (solid_at_zero && value <= 0)
        return true;
    return (b->ticks & Q2_SBAR_BLINK_BIT) == 0;
}

/* One counter: its digits, then its icon. */
static u32 emit_counter(const q2_statusbar *b, const q2_sbar_field *fields,
                        u16 tpage, u16 clut,
                        psx_ot *ot, u32 bucket, int ox, int oy,
                        q2_sbar_counter which, int value, int icon_index,
                        int low_threshold, bool solid_at_zero,
                        bool digits_visible)
{
    u8 digits[Q2_SBAR_COUNTER_DIGITS];
    int n = q2_sbar_digits_of(value, digits);
    /* A numeral is 24 x 24 in the sheet, not the icons' 32 x 24; sizing it as
     * an icon stretches every digit by a third. */
    q2_icon_size size = q2_icon_draw_size_of(b->players, 1,
                                             Q2_SBAR_DIGIT_W,
                                             Q2_SBAR_DIGIT_H);
    u32 emitted = 0;
    int i;

    /* The numerals' own palette, or the flash while the counter is low. */
    u16 digit_clut = pal_clut(b,
                              counter_is_low(b, value, low_threshold,
                                             solid_at_zero)
                                  ? Q2_SBAR_PAL_LOW
                                  : Q2_SBAR_PAL_DIGITS,
                              clut);

    /*
     * Right-aligned inside the three cells: a two-digit figure uses the second
     * and third, which is what puts the units column in the same place whatever
     * the value. Capture confirms it — "2" sits where the last digit of "100"
     * sits, not where its first does.
     */
    for (i = 0; digits_visible && i < n; i++) {
        int slot = Q2_SBAR_COUNTER_DIGITS - n + i;
        int f = q2_sbar_digit_field(which, slot);
        const q2_sbar_field *fd;

        if (f < 0)
            continue;
        fd = &fields[f];

        emitted += emit_cell(ot, bucket, tpage, digit_clut,
                             ox + b->anchor_x + fd->dx,
                             oy + b->anchor_y + fd->dy,
                             (u8)(digits[i] * Q2_SBAR_DIGIT_PITCH),
                             Q2_SBAR_DIGIT_V,
                             Q2_SBAR_DIGIT_W, Q2_SBAR_DIGIT_H,
                             (u8)(size.w ? size.w : Q2_SBAR_DIGIT_W),
                             (u8)(size.h ? size.h : Q2_SBAR_DIGIT_H));
    }

    /*
     * The icon beside it. INDEXED, not scanned — the three sub-draws all do
     * `base + index * 5` and never compare the fifth byte (statusbar.h). The
     * fifth byte is instead this sprite's palette, which is how the cross comes
     * out blue while the numerals beside it come out cyan.
     */
    {
        const q2_icon_rect *r = q2_icon_rect_get(b->icons, (u32)icon_index);
        int f = q2_sbar_icon_field(which);

        if (r && f >= 0 && !(r->w == 1 && r->h == 1)) {
            const q2_sbar_field *fd = &fields[f];
            q2_icon_size is = q2_icon_draw_size_of(b->players, 1, r->w, r->h);

            emitted += emit_cell(ot, bucket, tpage,
                                 pal_clut(b, r->id, clut),
                                 ox + b->anchor_x + fd->dx,
                                 oy + b->anchor_y + fd->dy,
                                 r->u, r->v, r->w, r->h, is.w, is.h);
        }
    }

    return emitted;
}

/* 0x80037DA4 — the signed three-cell frag counter used by every split hook. */
static u32 emit_frags(const q2_statusbar *b, const q2_sbar_field *fields,
                      const u8 field_index[Q2_SBAR_COUNTER_DIGITS],
                      u16 tpage, u16 clut, psx_ot *ot, u32 bucket,
                      int ox, int oy)
{
    u8 glyph[Q2_SBAR_COUNTER_DIGITS];
    q2_icon_size size = q2_icon_draw_size_of(b->players, 1,
                                             Q2_SBAR_DIGIT_W,
                                             Q2_SBAR_DIGIT_H);
    u16 glyph_clut = pal_clut(b, b->frags < 0 ? Q2_SBAR_PAL_LOW
                                               : Q2_SBAR_PAL_DIGITS,
                              clut);
    u32 emitted = 0;
    int i;

    q2_sbar_frag_glyphs(b->frags, glyph);
    for (i = 0; i < Q2_SBAR_COUNTER_DIGITS; i++) {
        const q2_sbar_field *fd;
        u8 u, v, w, h;

        if (glyph[i] == Q2_SBAR_GLYPH_BLANK)
            continue;
        if (glyph[i] == Q2_SBAR_GLYPH_MINUS) {
            u = 0;
            v = 192;
            w = Q2_SBAR_DIGIT_W;
            h = Q2_SBAR_DIGIT_H;
        } else {
            u = (u8)(glyph[i] * Q2_SBAR_DIGIT_PITCH);
            v = Q2_SBAR_DIGIT_V;
            w = Q2_SBAR_DIGIT_W;
            h = Q2_SBAR_DIGIT_H;
        }

        fd = &fields[field_index[i]];
        emitted += emit_cell(ot, bucket, tpage, glyph_clut,
                             ox + b->anchor_x + fd->dx,
                             oy + b->anchor_y + fd->dy,
                             u, v, w, h,
                             (u8)(size.w ? size.w : Q2_SBAR_DIGIT_W),
                             (u8)(size.h ? size.h : Q2_SBAR_DIGIT_H));
    }
    return emitted;
}

/* The two upper-right digit fields belong to the powerup timer alone. Unlike
 * the three main counters they never flash, and their ICON field precedes them
 * in the console's assembled record list (13, 14, 15). */
static u32 emit_powerup_timer(const q2_statusbar *b, u16 tpage, u16 clut,
                              psx_ot *ot, u32 bucket, int ox, int oy)
{
    const q2_icon_rect *r;
    const q2_sbar_field *fi;
    q2_icon_size is;
    u8 seconds;
    u32 emitted = 0;
    int digit, first, last;

    if (b->powerup_icon == Q2_SBAR_ICON_NONE)
        return 0;

    r = q2_icon_rect_get(b->icons, b->powerup_icon);
    if (!r || (r->w == 1 && r->h == 1))
        return 0;

    fi = &q2_sbar_fields[Q2_SBAR_FIELD_UP_ICON];
    is = q2_icon_draw_size_of(b->players, b->powerup_icon, r->w, r->h);
    emitted += emit_cell(ot, bucket, tpage, pal_clut(b, r->id, clut),
                         ox + b->anchor_x + fi->dx,
                         oy + b->anchor_y + fi->dy,
                         r->u, r->v, r->w, r->h, is.w, is.h);

    /* The console's pickup duration is thirty seconds and the UI owns two
     * numeral fields. A value below ten therefore occupies only the units
     * field, keeping its right edge fixed just like the three main counters. */
    seconds = b->powerup_seconds;
    first = seconds >= 10 ? Q2_SBAR_FIELD_UP_DIGIT0
                          : Q2_SBAR_FIELD_UP_DIGIT1;
    last = Q2_SBAR_FIELD_UP_DIGIT1;
    for (digit = first; digit <= last; digit++) {
        const q2_sbar_field *fd = &q2_sbar_fields[digit];
        u8 value = (digit == Q2_SBAR_FIELD_UP_DIGIT0) ? seconds / 10
                                                       : seconds % 10;
        q2_icon_size ds = q2_icon_draw_size_of(b->players, 1,
                                                Q2_SBAR_DIGIT_W,
                                                Q2_SBAR_DIGIT_H);

        emitted += emit_cell(ot, bucket, tpage,
                             pal_clut(b, Q2_SBAR_PAL_DIGITS, clut),
                             ox + b->anchor_x + fd->dx,
                             oy + b->anchor_y + fd->dy,
                             (u8)(value * Q2_SBAR_DIGIT_PITCH),
                             Q2_SBAR_DIGIT_V,
                             Q2_SBAR_DIGIT_W, Q2_SBAR_DIGIT_H,
                             (u8)(ds.w ? ds.w : Q2_SBAR_DIGIT_W),
                             (u8)(ds.h ? ds.h : Q2_SBAR_DIGIT_H));
    }

    return emitted;
}

u32 q2_statusbar_build_ot(const q2_statusbar *b, u16 tpage, u16 clut,
                          psx_ot *ot, u32 bucket, int origin_x, int origin_y)
{
    static const u8 split_frag_fields[Q2_SBAR_COUNTER_DIGITS] = { 13, 14, 15 };
    static const u8 quad_frag_fields[Q2_SBAR_COUNTER_DIGITS] = { 8, 9, 10 };
    q2_sbar_field scratch[Q2_SBAR_FIELDS_2V];
    const q2_sbar_field *fields = q2_sbar_fields;
    const u8 *frag_fields = split_frag_fields;
    bool show_armour = true;
    bool show_frags = false;
    bool show_one_extras = false;
    u32 n = 0;

    if (!b || !ot || !b->visible || tpage == 0)
        return 0;

    switch (b->layout) {
    case Q2_SBAR_LAYOUT_TWO_H:
        fields = q2_sbar_fields_2h;
        show_frags = true;
        break;
    case Q2_SBAR_LAYOUT_TWO_V:
        q2_sbar_2v_fields(b->screen_h, scratch);
        fields = scratch;
        show_frags = true;
        break;
    case Q2_SBAR_LAYOUT_QUAD:
        q2_sbar_quad_fields(b->view_index, b->frags, scratch);
        fields = scratch;
        frag_fields = quad_frag_fields;
        show_armour = false;
        show_frags = true;
        break;
    case Q2_SBAR_LAYOUT_ONE:
    default:
        show_one_extras = true;
        break;
    }

    n += emit_counter(b, fields, tpage, clut, ot, bucket,
                      origin_x, origin_y,
                      Q2_SBAR_HEALTH, b->health, b->health_icon,
                      Q2_SBAR_LOW_HEALTH, true, true);

    /*
     * `q2_icon_ammo_for_weapon` returns `ammoIcon[weapon]` verbatim, and that
     * table holds rect INDICES — 0x80035374 multiplies its entry by five and
     * adds the rect base. Feeding it to the effect-id scan, as this used to,
     * gave every weapon somebody else's icon.
     *
     * THE BLASTER SHOWS NO AMMO COUNTER, and the mechanism is not a zero test.
     * Last round left this open because the ammo sub-draw has no early-out and
     * the splitter always emits a units digit, so by the code as read a
     * blaster-only player should see a "0" that retail does not show. The
     * branch that does it is at the END of the sub-draw:
     *
     *     80035498  addiu v0, zero, 1
     *     8003549C  bne   s0, v0, 0x80035538      weapon != 1, leave it alone
     *     800354A8  lbu   v0, 66(v1)              v1 = 0x8009C598
     *     800354D4  sb    v0, 7(s3)               ... into the field's h
     *
     * It overwrites the digit fields with the four bytes at `0x8009C5DA`,
     * which are `{50, 0, 250, 0}` — and the last of those is the HEIGHT. A
     * zero-height sprite draws nothing. So weapon 1 blanks its own digits by
     * writing a degenerate rect over them, after computing them normally.
     *
     * That address is past the end of the numeral table and inside the
     * unidentified structure that follows it, so the console is reading bytes
     * that are not numeral records at all. Whether the original meant to point
     * at a blank cell or simply landed on one is not knowable from here; what
     * is knowable is the effect, and all three retail captures agree with it —
     * blaster shows no ammo, shotgun shows "10". Modelled as the behaviour
     * rather than by reproducing the out-of-bounds read.
     */
    n += emit_counter(b, fields, tpage, clut, ot, bucket,
                      origin_x, origin_y,
                      Q2_SBAR_AMMO, b->ammo,
                      q2_icon_ammo_for_weapon(b->icons, b->weapon),
                      Q2_SBAR_LOW_AMMO, false,
                      b->weapon != Q2_SBAR_WEAPON_NO_AMMO);

    /*
     * ARMOUR AT ZERO DRAWS NOTHING AT ALL — not a zero, not a blank icon.
     *
     * CORRECTION to the three addresses this note used to cite. It said
     * "`0x80035594` loads the armour halfword and branches straight to
     * `0x80035630`, which zeroes the sub-draw's own live flag; the test at
     * `0x80035634` then skips the entire body". All three clauses are wrong,
     * and the conclusion was right for a different reason:
     *
     *   0x80035570  lh   v0, 78(a0)    the armour halfword is HERE
     *   0x80035594  lhu  v0, 116(a0)   0x80035594 is the CELLS count
     *   0x80035630  sw   zero, 88(a0)  clears the POWER-STATE flag, on cells=0
     *   0x80035634  beq  v0, zero, 0x80035764   branches INTO the regular arm,
     *                                           it does not skip the body
     *   0x80035994  beq  a0, zero, 0x800359B0   THIS is what skips the draw
     *
     * So: the regular arm computes normally and then declines to call the
     * counter emitter when armour is zero. Which is the behaviour below, and
     * is what retail capture shows at a level start — the port used to
     * contradict it by parking a "0" there.
     *
     * The POWER arm has no such guard: 0x80035754 loads the cells count and
     * jumps straight to the shared draw at 0x800359A8, so a power item with
     * cells showing reads out even at zero armour.
     *
     * The ammo counter is NOT known to do the same. Its sub-draw has no such
     * early-out — the only zero test in it collapses the ICON to the 1x1 blank
     * (0x800353B0) and leaves the digits alone — yet capture shows no ammo
     * readout at all with the blaster in hand. Something suppresses it that has
     * not been found, so nothing is done about it here rather than guessing at
     * a threshold; it is openquestions material, not a fix.
     */
    if (show_armour && b->showing_power)
        n += emit_counter(b, fields, tpage, clut, ot, bucket,
                          origin_x, origin_y,
                          Q2_SBAR_ARMOUR, b->cells, b->armour_icon,
                          Q2_SBAR_LOW_AMMO, false, true);
    else if (show_armour && b->armour > 0)
        n += emit_counter(b, fields, tpage, clut, ot, bucket,
                          origin_x, origin_y,
                          Q2_SBAR_ARMOUR, b->armour, b->armour_icon,
                          Q2_SBAR_LOW_AMMO, false, true);

    if (show_frags)
        n += emit_frags(b, fields, frag_fields, tpage, clut, ot, bucket,
                        origin_x, origin_y);

    /* The console invokes this before the pickup-caption sub-draw. It is an
     * independent timer: an item caption cannot suppress it, and its source
     * is the powerup expiry words rather than `last_item`. */
    if (show_one_extras)
        n += emit_powerup_timer(b, tpage, clut, ot, bucket,
                                origin_x, origin_y);

    /*
     * The pickup caption's icon — field 16, from the fourth sub-draw at
     * `0x800359C0`. The index is the effect the touch dispatch left behind and
     * it selects the rect directly (statusbar.h); zero is the blank and the
     * `beq a0, zero` at 0x80035A94 collapses the drawn size to 1x1 with it,
     * which is what passing the index to q2_icon_draw_size_of reproduces.
     *
     * The CAPTION beside it is text, so it is not emitted here — see
     * q2_hud_pickup_build_ot, and the note there on why the two halves of one
     * sub-draw live in two modules.
     */
    if (show_one_extras && b->pickup_icon != 0) {
        const q2_icon_rect *r = q2_icon_rect_get(b->icons, b->pickup_icon);

        if (r && !(r->w == 1 && r->h == 1)) {
            const q2_sbar_field *fd = &q2_sbar_fields[Q2_SBAR_FIELD_UP_LEFT];
            q2_icon_size is = q2_icon_draw_size_of(b->players, b->pickup_icon,
                                                   r->w, r->h);

            n += emit_cell(ot, bucket, tpage, pal_clut(b, r->id, clut),
                           origin_x + b->anchor_x + fd->dx,
                           origin_y + b->anchor_y + fd->dy,
                           r->u, r->v, r->w, r->h, is.w, is.h);
        }
    }

    /*
     * The weapon strip, last — two sprites at absolute positions, with the
     * console's own two guards: nothing for a zero index, and one sprite when
     * both slots name the same weapon (0x80036188 / 0x80036198).
     */
    if (show_one_extras) {
        const q2_sbar_strip_pos *pos = q2_sbar_strip;
        int i;

        for (i = 0; i < Q2_SBAR_STRIP_SLOTS; i++) {
            const q2_icon_rect *r;
            q2_icon_size is;

            if (b->strip[i] == 0)
                continue;
            /* Slot A yields to slot B when they name the same weapon. */
            if (i == 0 && b->strip[0] == b->strip[1])
                continue;

            r = q2_icon_rect_get(b->icons, b->strip[i]);
            if (!r || (r->w == 1 && r->h == 1))
                continue;

            is = q2_icon_draw_size_of(b->players, 1, r->w, r->h);
            /*
             * RIGHT-ANCHORED, and this one is from capture rather than code.
             * Drawn with the table's x as the left edge the icon lands exactly
             * one icon-width right of where retail puts it: the blaster-only
             * capture has it spanning 427..451 in framebuffer columns and the
             * table says 458, so 458 is the right edge. The emitter's XY setup
             * was not followed far enough to say whether it subtracts the
             * width itself or is handed a different number; the offset is a
             * measurement, and if it is ever read properly this is the line to
             * replace.
             */
            n += emit_cell(ot, bucket, tpage, pal_clut(b, r->id, clut),
                           origin_x + pos[i].x - is.w, origin_y + pos[i].y,
                           r->u, r->v, r->w, r->h, is.w, is.h);
        }
    }

    return n;
}
