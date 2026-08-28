/*
 * statusbar.h — the status bar. Health, ammo, armour, and their icons.
 *
 * ---------------------------------------------------------------------------
 * The bar this project spent a long time proving did not exist
 * ---------------------------------------------------------------------------
 * FORMATS.md §11.1 asserted, as a *finding*, that Quake II PSX shows no health,
 * ammo or armour readout. It was reached by enumerating every `printf` and
 * `sprintf` site and every reader of the font table, exhaustively — and it was
 * wrong, because the bar renders **sprites**, not characters, and neither
 * instrument can see a sprite. Retail capture settled it.
 *
 * The retraction is in §11.1. What follows is the thing itself.
 *
 * ---------------------------------------------------------------------------
 * Where it is drawn from — and it is not a screen
 * ---------------------------------------------------------------------------
 * `0x800337D0` is not "some composite reached through a pointer". It is the
 * **per-viewport draw hook** — the function the one-player layout stores in the
 * view record at `+308`, alongside `0x80033D30` and `0x80034288` for the two
 * splits (screen.h). So the bar is drawn once per viewport, by the same call
 * that draws that viewport's world, and everything about it follows from that:
 * it is anchored to the viewport rather than the screen, it is drawn per player
 * in split screen, and its icons shrink with the viewport.
 *
 * ---------------------------------------------------------------------------
 * The anchor — view+304 and view+306
 * ---------------------------------------------------------------------------
 * screen.h carried these two halfwords as `pad_a` / `pad_b`, unknown. They are
 * the bar's origin: every field's position is a literal offset from them
 * (`0x800337EC` onward), so a layout positions the whole bar by writing two
 * numbers and the fields follow.
 *
 * The layouts already wrote them, which is the confirmation: one player gets
 * **(93, 201)** (`0x80077E60` / `0x80077E6C`) and a split viewport (0, 95)
 * (`0x80077A98`). At (93, 201) the health digits land at x = 22, 46 and 70 with
 * the cross at 93, and the row sits at y = 201 of a 248-line screen — the
 * bottom-left corner retail capture shows, arrived at from the code alone.
 *
 * ---------------------------------------------------------------------------
 * The fields
 * ---------------------------------------------------------------------------
 * Seventeen 10-byte records are built on the stack at `sp+200`, each
 * `{s16 x, s16 y, u8 u, u8 v, u8 w, u8 h, u8 dst_w, u8 dst_h}` — a source rect
 * in the sheet plus a destination size. They initialise to the 1 x 1 blank at
 * (255, 255) and the sub-draws fill in the real rect.
 *
 * Sorted by x they fall into TWO ROWS. The main row, at the anchor, is **three
 * digits then an icon, three times**, digits 24 apart because a numeral cell is
 * 24 wide, plus a single field far right:
 *
 *     -71  -47  -23   digits, health           +0    icon   (38 wide)
 *     +64  +88  +112  digits, ammo             +135  icon
 *     +179 +203 +227  digits, armour           +250  icon
 *     +330            the one-player auxiliary icon (`0x80037CAC`)
 *
 * A second row sits **24 to 25 above** it, and capture shows what it carries: an
 * icon on the left beside a pickup caption, and a two-digit counter with its own
 * icon on the right.
 *
 *     -71             upper-left icon          (9 wide)
 *     +256 +280       two digits               +330  icon
 *
 * Health's icon is initialised 38 wide and the upper-left one 9, where the rest
 * are 8; every one of those is overwritten by the sub-draw with either the icon
 * record's own size or the split-screen clamp, so an initialiser only shows for
 * a field that is never filled.
 *
 * ---------------------------------------------------------------------------
 * The numerals — 0x8009C598
 * ---------------------------------------------------------------------------
 * Ten four-byte `{u, v, w, h}` digit records, all at **v = 168**, **24 x 24**, with
 * `u = 24 * digit`. They sit in the same sheet as the icons, on the row below
 * the icon grid — which is why the sheet decoded to "32 x 24 item icons
 * followed by a set of large digits" and why that was the tell nobody read.
 *
 * Two more records follow: glyph 10 is the minus sign at (0, 192), and glyph
 * 11 is the 1 x 1 blank. The signed frag formatter at `0x80037DA4` uses both.
 *
 * ---------------------------------------------------------------------------
 * FOUR LAYOUTS, and this is the one-player one
 * ---------------------------------------------------------------------------
 * The installed hooks build four DIFFERENT layouts, not one table drawn four
 * ways: `0x800337D0` is one player (17 fields), `0x80033D30` two stacked (16),
 * `0x80034288` two side-by-side (16), and `0x80034830` three/four players (11
 * fields per viewport).
 *
 * That last identity is load-bearing. `0x80034288` used to be labelled the
 * quad hook, even though the selector passes it only to `0x80077AEC`, the
 * side-by-side constructor. The actual quad hook is passed to `0x8007771C` and
 * derives x from 44 halfwords at `0x8009C600` (11 per view) and y from four at
 * `0x800AE808`. All four layouts are transcribed below.
 */
#ifndef Q2PSX_STATUSBAR_H
#define Q2PSX_STATUSBAR_H

#include "gpu.h"
#include "icontable.h"
#include "inventory.h"   /* the armour field selects on the Q2_INV_* flags */

/* Forward-declared so a caller that never colours the bar need not pull the
 * whole executable-table module in. */
struct q2_hud_tables;

/* ------------------------------------------------------------------------- */
/* The numeral cells — 0x8009C598                                             */
/* ------------------------------------------------------------------------- */
#define Q2_SBAR_DIGIT_ADDR  0x8009C598u
#define Q2_SBAR_DIGITS      10
#define Q2_SBAR_GLYPHS      12
#define Q2_SBAR_GLYPH_MINUS 10
#define Q2_SBAR_GLYPH_BLANK 11
#define Q2_SBAR_DIGIT_W     24
#define Q2_SBAR_DIGIT_H     24
#define Q2_SBAR_DIGIT_V    168
#define Q2_SBAR_DIGIT_PITCH 24   /* and the field table's own x stride */

/* Unity for a modulated primitive — the same 0x80 every other UI sprite uses. */
#define Q2_SBAR_MOD 128

/* ------------------------------------------------------------------------- */
/* The icons — RECT INDICES, hard-coded, NOT item effect ids                  */
/* ------------------------------------------------------------------------- */
/*
 * CORRECTION — the bar indexes the rect table. It does not scan it.
 *
 * This block used to name item `effect` ids and resolve them through
 * `q2_icon_rect_for_id()`, on icontable.h's reading that a rect record's fifth
 * byte is the item's touch-dispatch index. The three sub-draws say otherwise,
 * in the plainest way a disassembly can:
 *
 *     80035190  lbu  v0, 170(t0)     t0 = 0x8009C478   health, ALWAYS 170
 *     80035374  sll  v0, a0, 2       a0 = ammoIcon[weapon]
 *     80035378  addu v0, v0, a0                        ammo,   a0 * 5
 *     80035380  addu v1, a0, t0                        &rect[a0]
 *
 * 170 is a byte offset into a five-byte record: rect 34. The ammo path
 * multiplies its table entry by five and adds the same base, so `ammoIcon[]`
 * is a rect index too. There is no strcmp, no scan, no compare of the fifth
 * byte anywhere in any of the three.
 *
 * WHAT THE OLD READING DREW, and why it looked plausible enough to ship:
 * scanning for effect 34 finds rect **30**, because rect 30's fifth byte
 * happens to be 34. So the health field drew a chunky machine-looking sprite
 * where retail shows a blue cross — and because it drew *something*
 * recognisable, it read as a mis-picked icon rather than as a whole mis-read
 * table. Rect 34 is the cross.
 *
 * The fifth byte is a PALETTE INDEX (see below), which is why joining it to
 * the item table produced a near-monotonic and therefore convincing-looking
 * correspondence. Two independent tells confirm the index reading against it:
 * the cell the effect reading assigns to rect 25 is EMPTY, and the cells the
 * index reading assigns to the six ammo types are six ammo boxes.
 *
 * ---------------------------------------------------------------------------
 * CORRECTION, the second one — THE ARMOUR ICON IS NOT ONE RECT
 * ---------------------------------------------------------------------------
 * This block used to carry a third line beside the two above:
 *
 *     8003565C  lbu  v0, 150(a0)     a0 = 0x8009C478   armour, ALWAYS 150
 *
 * "ALWAYS" was the error, and it is the same shape as the one it replaced: the
 * instruction is real, the offset is real, and the premise that it is
 * unconditional was never tested. It is not. It sits inside ONE OF FIVE ARMS
 * of the armour sub-draw at `0x80035554`, and the arm is guarded by the POWER
 * SHIELD bit:
 *
 *     8003564C  andi v0, v0, 0x8000     Q2_INV_POWER_SHIELD
 *     80035650  beq  v0, zero, 0x800356E0
 *     8003565C  lbu  v0, 150(a0)        -> rect 30, the power shield
 *     8003576C  andi v1, v1, 0x4000     Q2_INV_ARMOUR_BODY
 *     8003577C  lbu  v0, 130(t0)        -> rect 26, the red vest
 *     80035800  andi v1, v1, 0x2000     Q2_INV_ARMOUR_COMBAT
 *     80035810  lbu  v0, 135(t0)        -> rect 27, the gold vest
 *     80035894  andi v1, v1, 0x1000     Q2_INV_ARMOUR_JACKET
 *     800358A4  lbu  v0, 140(t0)        -> rect 28, the grey vest
 *     80035928  (falls through)         -> rect 0, the blank
 *
 * The five-byte stride is proved inside the function rather than assumed: the
 * body arm reads 130, 131, then 132, 133, then `addiu v0, zero, 130 / addu
 * v0, v0, t0 / lbu v0, 4(v0)` = 134. Records run 130..134, 135..139, 140..144
 * and 150..154 — rects 26, 27, 28 and 30.
 *
 * Rendering those cells out of `qk_menu.lbm` with each rect's OWN palette (the
 * fifth byte: 31, 32, 33 and 34) settles what they are: 26/27/28 are three
 * armour vests in red, gold and grey, and 30 is a red-lamp device that is not
 * a vest at all. Reading 150 unconditionally therefore put the POWER SHIELD on
 * the bar for every player wearing any armour, which is the reported bug.
 *
 * The test order matters and is the console's: body, combat, jacket. A player
 * who has held body armour keeps 0x4000 up while wearing a weaker class, so
 * testing weakest-first would show the wrong vest.
 */
#define Q2_SBAR_ICON_HEALTH        34  /* 0x80035178, offset 170 — the cross  */
#define Q2_SBAR_ICON_ARMOUR_BODY   26  /* 0x8003577C, offset 130 — flag 0x4000 */
#define Q2_SBAR_ICON_ARMOUR_COMBAT 27  /* 0x80035810, offset 135 — flag 0x2000 */
#define Q2_SBAR_ICON_ARMOUR_JACKET 28  /* 0x800358A4, offset 140 — flag 0x1000 */
#define Q2_SBAR_ICON_POWER_SHIELD  30  /* 0x8003565C, offset 150 — flag 0x8000 */
#define Q2_SBAR_ICON_POWERUP_QUAD  40  /* 0x80035B90 — client+0xAC              */
#define Q2_SBAR_ICON_POWERUP_INVULN 41 /* 0x80035BEC — client+0xB0              */
#define Q2_SBAR_ICON_POWERUP_ENVIRO 42 /* 0x80035C48 — client+0xB4              */
#define Q2_SBAR_ICON_POWERUP_BREATHER 43 /* 0x80035CA4 — client+0xB8            */
#define Q2_SBAR_ICON_NONE           0  /* rect 0 is the 1x1 blank             */

/*
 * The armour field's five-way select, as a pure function of the inventory's
 * flag word. `show_power` is the sub-draw's own live state (see
 * q2_statusbar_armour_state below), not a property of the inventory.
 */
u8 q2_sbar_armour_icon(u32 inv_flags, bool show_power);

/* ------------------------------------------------------------------------- */
/* The palettes — one per sprite, and the port used to ignore all of them     */
/* ------------------------------------------------------------------------- */
/*
 * A field record is TEN bytes and the last two are not padding:
 *
 *     +0 s16 x   +2 s16 y   +4 u  +5 v  +6 w  +7 h  +8 PALETTE  +9 SLOT
 *
 * `0x800337EC` onward initialises all thirteen with +9 = 14 — the VRAM slot
 * qk_menu.lbm registers under — and +8 = 8 for every field except the health
 * icon, which gets 38 (`addiu v0, zero, 38` at 0x800337FC, stored at
 * 0x80033810). The sub-draws then overwrite +8 per sprite: an icon takes the
 * fifth byte of its own rect record (0x80035210 / 0x8003540C), and a counter
 * running low takes 7.
 *
 * Those three indices are the built-in palette bank's (hudtables.h), and
 * reading them out settles what they are beyond argument:
 *
 *     8   pale cyan ramp to (160,200,224)   the numerals
 *     38  blue ramp to near-white           the health cross
 *     7   red/orange ramp to (248,64,0)     the low-value flash
 *
 * The port passed ONE clut — the menu font's — for every sprite in the bar,
 * so the numerals, the icons and the flash all came out in the font's colours.
 * That is what made the HUD look wrong while the world looked right, and it is
 * a separate defect from the framebuffer-format swap in client/main.c: this one
 * is visible in a `--shot` capture, and that one is not.
 */
#define Q2_SBAR_PAL_DIGITS   8   /* field init, 0x800337FC..0x80033814      */
#define Q2_SBAR_PAL_LOW      7   /* 0x8003524C / 0x80035460                 */

/*
 * When a counter flashes. Health is `slti v1, 26` at 0x8003523C and ammo is
 * `sltiu v0, 6` at 0x80035440 — different numbers, so they are kept apart
 * rather than folded into one "low" constant.
 *
 * Below the threshold the digits alternate between their own palette and 7 on
 * bit 7 of the frame counter at `0x800AEBAC`, except that health at or below
 * zero holds 7 solid (the `blez` at 0x80035248 skips the blink test).
 */
#define Q2_SBAR_LOW_HEALTH  26
#define Q2_SBAR_LOW_AMMO     6
#define Q2_SBAR_BLINK_BIT 0x80u

/*
 * How long the armour field holds each of its two readouts when the player has
 * both a power item and a vest — `addiu v0, v0, 300` at 0x8003560C, on the
 * level clock, so exactly one second.
 */
#define Q2_SBAR_POWER_ALTERNATE 300u

/* The fifth sub-draw divides its selected powerup deadline by this, on the
 * same 300 Hz level clock as the armour alternation. */
#define Q2_SBAR_POWERUP_SECONDS_TICKS 300u

/*
 * The one weapon whose ammo digits are blanked — the blaster, slot 1.
 * `0x8003549C` tests for exactly this id and overwrites the three digit fields
 * with a zero-height rect; see the note at the ammo counter in statusbar.c.
 */
#define Q2_SBAR_WEAPON_NO_AMMO 1

/* ------------------------------------------------------------------------- */
/* The weapon strip — 0x80035EA0, positions from 0x8009C658                   */
/* ------------------------------------------------------------------------- */
/*
 * After walking the thirteen field records ten bytes at a time, `0x80035EA0`
 * draws TWO more sprites that are not fields at all. They have their own
 * position table and their own guards:
 *
 *     80036180  lh   v1, 96(s7)          slot A's rect index
 *     80036188  beq  v1, zero, skip      nothing selected, nothing drawn
 *     80036190  lh   v0, 100(s7)         slot B's
 *     80036198  beq  v1, v0, skip        the same weapon twice draws once
 *     800361A0  sll  a3, v1, 2
 *     800361A4  addu a3, a3, v1          index * 5, the rect stride again
 *     80036258  lh   v1, 100(s7)         then slot B, guarded only on zero
 *
 * `0x8009C658` is the head of the structure icontable.h records as "a
 * different structure that has not been identified" — it is this table, four
 * `s16` pairs read as consecutive halfwords (`tbl[fp]`, `tbl[fp+1]`):
 *
 *     (388, 201)  (458, 201)     one player
 *     (381,  95)  (419,  95)     a split viewport
 *
 * These are ABSOLUTE screen positions, not offsets from the bar's anchor —
 * and the y is 201, which is the one-player anchor's own row, so the strip
 * sits on the same line as the counters rather than being part of them.
 *
 * That accounts for what capture shows exactly: one icon with only the blaster
 * held, because slot A is skipped when the two indices agree, and two once a
 * second weapon is picked up.
 */
#define Q2_SBAR_STRIP_SLOTS 2

typedef struct q2_sbar_strip_pos { s16 x, y; } q2_sbar_strip_pos;

extern const q2_sbar_strip_pos q2_sbar_strip[Q2_SBAR_STRIP_SLOTS];
extern const q2_sbar_strip_pos q2_sbar_strip_2p[Q2_SBAR_STRIP_SLOTS];

/* ------------------------------------------------------------------------- */
/* The field table — 0x800337EC onward                                        */
/* ------------------------------------------------------------------------- */
#define Q2_SBAR_FIELDS      17
#define Q2_SBAR_COUNTERS     3
#define Q2_SBAR_COUNTER_DIGITS 3

/* The main row's far-right field, and the upper row's four. */
#define Q2_SBAR_FIELD_AUX_ICON   12
#define Q2_SBAR_FIELD_UP_ICON    13
#define Q2_SBAR_FIELD_UP_DIGIT0  14
#define Q2_SBAR_FIELD_UP_DIGIT1  15
#define Q2_SBAR_FIELD_UP_LEFT    16

typedef struct q2_sbar_field {
    s16 dx, dy;      /* from the viewport anchor at view+304 / view+306 */
    u8  init_w, init_h;
} q2_sbar_field;

extern const q2_sbar_field q2_sbar_fields[Q2_SBAR_FIELDS];

/* Both two-player hooks build sixteen records. The first is the stacked
 * layout at 0x80033D30; the second is side-by-side at 0x80034288. */
#define Q2_SBAR_FIELDS_2H 16
#define Q2_SBAR_FIELDS_2V 16
extern const q2_sbar_field q2_sbar_fields_2h[Q2_SBAR_FIELDS_2H];
extern const q2_sbar_field q2_sbar_fields_2v[Q2_SBAR_FIELDS_2V];

/* In the side-by-side layout, armour and frags move to the top through the
 * literal expression `anchor_y + 40 - framebuffer_height`. The table carries
 * the constant 40; this predicate identifies the records that subtract the
 * live height. */
#define Q2_SBAR_2V_UPPER_DY 40
bool q2_sbar_field_2v_is_upper(int field);
void q2_sbar_2v_fields(int screen_h,
                       q2_sbar_field out[Q2_SBAR_FIELDS_2V]);

/* The actual quad hook at 0x80034830: eleven fields for each viewport. X comes
 * from 0x8009C600 and y from 0x800AE808. Views 1 and 3 additionally slide a
 * one- or two-character frag value to the inner edge; the helper applies that
 * value-dependent adjustment. */
#define Q2_SBAR_QUAD_VIEWS 4
#define Q2_SBAR_FIELDS_QUAD 11
extern const q2_sbar_field
    q2_sbar_fields_quad[Q2_SBAR_QUAD_VIEWS][Q2_SBAR_FIELDS_QUAD];
void q2_sbar_quad_fields(int view, int frags,
                         q2_sbar_field out[Q2_SBAR_FIELDS_QUAD]);

typedef enum q2_sbar_layout {
    Q2_SBAR_LAYOUT_ONE = 0,
    Q2_SBAR_LAYOUT_TWO_H,
    Q2_SBAR_LAYOUT_TWO_V,
    Q2_SBAR_LAYOUT_QUAD
} q2_sbar_layout;

/* The record's four trailing bytes, which are the same in all four layouts:
 * a source rect of (255, 255) size 1 x 1 — the blank — and a draw size the
 * caller overwrites. They are initial values, not layout. */
#define Q2_SBAR_FIELD_INIT_U    255
#define Q2_SBAR_FIELD_INIT_V    255
#define Q2_SBAR_FIELD_INIT_RECT 1

/*
 * Which of the three counters is which.
 *
 * The call sites prove the order directly: `0x80035178` receives group zero
 * (health), `0x800352C0` group one (ammo), and `0x80035554` group two (armour).
 * Retail capture independently agrees.
 */
typedef enum q2_sbar_counter {
    Q2_SBAR_HEALTH = 0,
    Q2_SBAR_AMMO,
    Q2_SBAR_ARMOUR
} q2_sbar_counter;

/* The field index of counter `c`'s first digit, and of its icon. */
int q2_sbar_digit_field(q2_sbar_counter c, int digit);
int q2_sbar_icon_field(q2_sbar_counter c);

/* ------------------------------------------------------------------------- */
/* State                                                                      */
/* ------------------------------------------------------------------------- */
typedef struct q2_statusbar {
    const q2_icon_tables *icons;      /* not owned */

    /* Where the viewport put it — view+304 / view+306. */
    s16 anchor_x, anchor_y;

    /* What it shows. These are the port's inputs; the console reads them out
     * of the player record the same way. */
    s16 health;
    s16 armour;
    s16 ammo;
    s16 frags;
    int weapon;                       /* 1-based; selects the ammo icon */

    /*
     * The health and armour icons, as RECT INDICES into the table at
     * 0x8009C478 — see the correction above. Only HEALTH is hard-coded (34);
     * the armour field is a five-way select on the inventory's flag word and
     * on the sub-draw's own live power state, which is what
     * q2_statusbar_armour_state() below computes.
     *
     * Q2_SBAR_ICON_NONE is rect 0, the 1x1 blank, and draws nothing.
     */
    u8 health_icon;
    u8 armour_icon;

    /*
     * The fifth sub-draw at 0x80035B38: its own upper-right icon and two
     * decimal fields. It reads the four expiry words rather than inventory
     * flags, selects the first live one, and shows floor((until - ticks)/300).
     * Zero icon is rect 0 and means no timer is present.
     */
    u8  powerup_icon;
    u8  powerup_seconds;

    /*
     * The pickup caption's icon — field 16, the upper-left one, filled by the
     * fourth sub-draw at `0x800359C0` from `client+84`.
     *
     * A RECT INDEX, and for an item that is the EFFECT ID itself: the sub-draw
     * does `index * 5 + 0x8009C478` on the very byte the touch dispatch stores
     * the effect into (`sb s7, 84(s1)` at 0x800372F0). So the same number picks
     * the icon here and the caption out of the 57-name table, and that is the
     * join icontable.h's retraction was circling: it is by INDEX, not by the
     * rect record's fifth byte.
     *
     * Zero is rect 0, the 1x1 blank, and draws nothing — which is both "no
     * pickup" and the frame a caption expires on. `q2_item_pickup_caption`
     * computes it.
     */
    u8 pickup_icon;

    /*
     * The armour field's other half. In the POWER state the counter shows the
     * CELLS count and is drawn unconditionally (`lhu a0, 116(v0)` at
     * 0x80035754, straight into the shared draw at 0x800359A8); in the regular
     * state it shows armour points and is skipped at zero (0x80035994).
     */
    s16 cells;

    /*
     * The sub-draw's own retained state: client+88, the flag saying the power
     * item is being shown, and client+192, the deadline it alternates on.
     *
     *   cells == 0                       -> regular, always (0x80035630)
     *   a power bit and no armour class  -> power, pinned  (0x80035628)
     *   a power bit and an armour class  -> alternate every 300 ticks of
     *                                       0x800AEBAC   (0x800355C8-0x80035614)
     *
     * 300 is ONE SECOND: 0x800AEBAC is the level clock at 300 ticks to the
     * second (combat.h, entity.h), not a frame counter. Feeding this bar a
     * render-frame index instead makes both this and the low-value blink ten
     * times too slow.
     */
    bool showing_power;
    u32  power_toggle_at;

    /*
     * The palette bank, for resolving a sprite's palette index to a CLUT word.
     * Optional: with no tables the bar falls back to the single clut passed to
     * q2_statusbar_build_ot(), which is what it always used to do.
     */
    const struct q2_hud_tables *hud;

    /*
     * The clock the flash is phased on — `0x800AEBAC`, tested against
     * Q2_SBAR_BLINK_BIT. Supplied by the caller because it is the engine's
     * global tick, not something the bar owns.
     *
     * It is the LEVEL CLOCK, 300 ticks to the second (combat.h's
     * Q2_TICKS_PER_SECOND, entity.h's note on 0x800AEBAC, and sim.c's
     * `level_time += dt`) — NOT a rendered-frame counter. The half-period of
     * the blink is 128/300 = 0.43 s; against a 30 Hz frame index it would be
     * 4.3 s, and the armour alternation above would be ten seconds instead of
     * one. Both were wrong for the same one-line reason.
     */
    u32 ticks;

    /*
     * The weapon strip's two slots, as RECT INDICES. Zero draws nothing, and
     * equal values draw once — both guards are the console's (see above).
     *
     * INFERRED, and flagged because the rest of this header is not: the two
     * fields the console reads live at +96 and +100 of a 224-byte record in the
     * array at `0x800C7C60`, and what writes them has not been traced. What the
     * port puts here is the weapon id used directly as a rect index, which is
     * the reading the sheet supports — rects 1..11 land on the eleven weapon
     * cells, with slot 6 (hand grenades) sitting off the gun row exactly as an
     * authoring pass would put it. It is checkable against capture and it is
     * not read out of the executable; if the strip ever shows the wrong gun,
     * this mapping is the thing to doubt first.
     */
    u8 strip[Q2_SBAR_STRIP_SLOTS];

    int players;                      /* drives the icon size reduction */
    int view_index;                   /* 0..3, selects the quad offset row */
    int screen_h;                     /* live 0x800B2DA2 for TWO_V's top row */
    q2_sbar_layout layout;
    bool visible;
} q2_statusbar;

void q2_statusbar_init(q2_statusbar *b, const q2_icon_tables *icons,
                       int players);

/*
 * Give the bar the built-in palette bank, so each sprite can be drawn in its
 * own colours rather than all of them in one. Optional — see `hud` above.
 */
void q2_statusbar_set_palettes(q2_statusbar *b, const struct q2_hud_tables *t);

/* Place it. A viewport's own anchor — `sbar_x`/`sbar_y` in screen.h. */
void q2_statusbar_anchor(q2_statusbar *b, s16 x, s16 y);

/* Select the exact callback installed by the screen layout and the viewport
 * whose callback is running. */
void q2_statusbar_layout(q2_statusbar *b, q2_sbar_layout layout,
                         int view_index, int screen_h);

/* The signed split-screen frag formatter: glyph 10 is minus, 11 is blank.
 * Retail clamps the negative end to -99 before formatting. */
void q2_sbar_frag_glyphs(int frags, u8 out[Q2_SBAR_COUNTER_DIGITS]);

/*
 * Run the armour field's state machine and choose its icon — 0x80035554's
 * prologue, everything before the five-way select.
 *
 * Separate from the emit because it MUTATES: the console keeps the power flag
 * and its deadline in the player record and rewrites them from inside the bar.
 * Call it once per tick, before q2_statusbar_build_ot; `b->ticks` must already
 * hold the level clock. It sets `armour_icon`, `showing_power` and the counter
 * the emit draws.
 *
 * `inv_flags` is the inventory's flag word (Q2_INV_*). Armour and cells are
 * taken from the bar's own `armour` and `cells` fields, which the caller fills
 * from the same inventory.
 */
void q2_statusbar_armour_state(q2_statusbar *b, u32 inv_flags);

/*
 * Run 0x80035B38's powerup selector. `b->ticks` must already be the level
 * clock. The deadline order is deliberately not an inventory-bit priority:
 * quad, invulnerability, environment suit, then rebreather are the four
 * client words in memory order, and the first strict unsigned `now < until`
 * wins.
 */
void q2_statusbar_powerup_state(q2_statusbar *b, const q2_inventory *inv);

/*
 * Emit the bar into bucket `bucket`.
 *
 * The sheet is VRAM slot 14 and something else does the uploading — the menu's
 * font loader happens to, because the console registers all its UI images in
 * one function — so this takes the resulting `tpage` and `clut` rather than a
 * loader's struct. The bar does not care who put the sheet in VRAM.
 *
 * Returns the number of primitives emitted.
 */
u32 q2_statusbar_build_ot(const q2_statusbar *b, u16 tpage, u16 clut,
                          psx_ot *ot, u32 bucket,
                          int origin_x, int origin_y);

/*
 * A counter's digits, most significant first, with leading zeroes suppressed
 * the way the capture shows them — "2" is one digit, not "002". Returns how
 * many were written; `out` takes Q2_SBAR_COUNTER_DIGITS.
 */
int q2_sbar_digits_of(int value, u8 out[Q2_SBAR_COUNTER_DIGITS]);

#endif /* Q2PSX_STATUSBAR_H */
