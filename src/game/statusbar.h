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
 * `{s16 x, s16 y, u8 u, u8 v, u8 w, u8 h, u8 palette, u8 slot}` — a rect in
 * the sheet, then a CLUT index and a tpage index. There is NO separate
 * destination size, and this line used to say there was: the emitter at
 * 0x80035EA0 adds bytes 6 and 7 to u and v for the UVs (0x80035FD8-0x8003605C)
 * and to x and y for the vertices (0x80036080-0x800360F0), then resolves byte 9
 * through the tpage table at 0x800DDD3C (0x80036108) and byte 8 through the
 * CLUT table at 0x800E3F2C (0x80036128). They initialise to the 1 x 1 blank at
 * (255, 255) and the sub-draws fill in the real rect.
 *
 * Sorted by x they fall into TWO ROWS. The main row, at the anchor, is **three
 * digits then an icon, three times**, digits 24 apart because a numeral cell is
 * 24 wide, plus a single field far right:
 *
 *     -71  -47  -23   digits, health           +0    icon   (palette 38)
 *     +64  +88  +112  digits, ammo             +135  icon
 *     +179 +203 +227  digits, armour           +250  icon
 *     +330            the WEAPON IN HAND (`0x80037CAC`)
 *
 * A second row sits **24 to 25 above** it, and capture shows what it carries: an
 * icon on the left beside a pickup caption, and a two-digit counter with its own
 * icon on the right.
 *
 *     -71             upper-left icon          (palette 9)
 *     +256 +280       two digits               +330  icon
 *
 * All seventeen start as that 1 x 1 blank — `sb v1, 206(sp)` and `207(sp)` with
 * v1 = 1 at 0x80033808 / 0x8003380C, and the same pair in every record — so
 * the emitter's 1 x 1 skip (0x80035F5C) drops any field no sub-draw fills. The
 * 38 on health's icon and the 9 on the upper-left one are their PALETTE bytes
 * (0x800337FC -> 0x80033810, 0x80033B3C -> 0x80033B50), where every other
 * record takes 8. This paragraph used to call them widths, and q2_sbar_field
 * carried them as `init_w`.
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
 * 11 is the 1 x 1 blank. The signed readouts use both — the frag counter at
 * `0x80037DA4` and, since health is signed too (0x800352A4), the health one.
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
 *
 * ---------------------------------------------------------------------------
 * DEATH STRIPS THE ONE-PLAYER BAR — 0x80033C68
 * ---------------------------------------------------------------------------
 * The one-player hook calls the health sub-draw (`0x80033C40`) and the
 * current-weapon icon (`0x80033C50`) and only THEN tests the player's health:
 *
 *     80033C60  lh   v0, 264(s0)          s0 = the entity
 *     80033C68  blez v0, 0x80033CFC       zero or less -> the emitter setup
 *
 * The branch target is the `addiu a3, zero, 1` that arms the emitter's strip
 * flag, so it jumps past all five remaining sub-draws: the mega-health bleed
 * (`0x80033C70` -> 0x80037C20), ammo (`0x80033C94` -> 0x800352C0), armour
 * (`0x80033CB8` -> 0x80035554), the powerup timer (`0x80033CDC` -> 0x80035B38)
 * and the pickup caption (`0x80033CEC` -> 0x800359C0). Their fields keep the
 * 1 x 1 blank they were initialised to, and the emitter skips a 1 x 1 field
 * outright (`0x80035F5C`), so on death the bar is health, the weapon in hand
 * and the strip — nothing else.
 *
 * It is ONE-PLAYER ONLY. `q2psx-inspect access 0x108` finds entity+264 read in
 * this hook and inside the health sub-draw and NOWHERE in 0x80033D30,
 * 0x80034288 or 0x80034830, so a split viewport keeps its whole bar.
 *
 * ---------------------------------------------------------------------------
 * TWO weapon ids, not one — client+98 and client+102
 * ---------------------------------------------------------------------------
 * The bar reads BOTH, and they are different halfwords of the same 224-byte
 * client record. Collapsing them is why two of this cluster's findings read as
 * a contradiction when set side by side:
 *
 *     client+98   the weapon whose MODEL IS RAISED
 *                 0x800352E8 `lhu s0, 98(a3)` — the ammo count and the ammo
 *                 ICON key off it; 0x80037E78 gates the pickup auto-switch on
 *                 `+98 == 1`; 0x8004F970 commits viewweapon+214 into
 *                 viewweapon+212 and into +98 at the same instant, i.e. when
 *                 the new model finishes rising.
 *     client+102  the SELECTED weapon
 *                 0x80037CE8 `lh v1, 102(v1)` — field 12's icon; 0x80037EDC /
 *                 0x80037EEC — the carousel's two slots. 0x8003D5FC seeds +98
 *                 FROM +102 at spawn (in the JAL's delay slot) and
 *                 0x8003B188/0x8003B204 restores +102 from +98.
 *
 * So during a weapon raise retail shows the OLD weapon's ammo beside the NEW
 * weapon's icon in field 12. The port has ONE `combat.weapon_id`
 * (sim.h) and feeds it to both, so its two readouts switch on the same frame.
 * INFERRED, and only inferred: the divergence is as wide as the raise
 * animation. Nothing in the bar can settle how many frames that is — it needs
 * the view-weapon state machine — so the port's single id is recorded here as
 * a known simplification rather than modelled.
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

/*
 * THE BAR IS BRIGHTENED, not drawn at unity — and this used to say the
 * opposite.
 *
 *     80035F54  addiu s4, zero, 192
 *     800360E0  sb    s4, 4(a0)      \
 *     800360F4  sb    s4, 5(v1)       > bytes 4/5/6, the FLAT colour of an FT4
 *     80036104  sb    s4, 6(v0)      /
 *
 * The field emitter's packet is a POLY_FT4 — length byte 9 at 0x80035F9C, code
 * byte 0x2C at 0x80035FA8 — and 0x2C is the texture-BLENDED code (0x2D would be
 * raw), so the colour multiplies the texel with 128 as unity. 192 is therefore
 * a deliberate 1.5x. `Q2_SBAR_MOD 128` was an assumption carried over from the
 * menu glyphs, which are a different emitter with its own constant; it left the
 * whole bar a third dark against a correctly lit world.
 */
#define Q2_SBAR_MOD 192

/*
 * The STRIP does not follow it. Its own emitter at 0x80033320 is a POLY_GT4
 * (code 0x3C at 0x80033374) whose four corners carry two different colours:
 * 0 on the edge facing away from the current weapon and 128 on the edge facing
 * it (0x800334B4 / 0x80033510 and the mirror at 0x80033568).
 *
 * And it is not opaque. 0x80033670 calls SetSemiTrans on that packet with the
 * tpage ORed to ABR 1 (additive, 0x8003363C), then copies it into a second,
 * SUBTRACTIVE packet (ABR 2, 0x80033798) whose bright edge is 62 rather than
 * 128 (0x800336CC / 0x8003372C) and whose CLUT is palette 73's
 * (`lhu 0x800E3FBE` at 0x800337A4 — the CLUT table at 0x800E3F2C, entry 73).
 * The copy is linked second and therefore drawn first: a dark silhouette
 * subtracted from the world, then the icon added over it. See emit_strip_cell.
 */
#define Q2_SBAR_STRIP_MOD        128
#define Q2_SBAR_STRIP_FADE         0
#define Q2_SBAR_STRIP_SHADOW      62
#define Q2_SBAR_PAL_STRIP_SHADOW  73

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
 * `0x800337EC` onward initialises all seventeen with +9 = 14 — the VRAM slot
 * qk_menu.lbm registers under — and +8 = 8 for every field except the health
 * icon, which gets 38 (`addiu v0, zero, 38` at 0x800337FC, stored at
 * 0x80033810), and the upper-left icon, which gets 9 (0x80033B3C, stored at
 * 0x80033B50). The sub-draws then overwrite +8 per sprite: an icon takes the
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

/*
 * And the ARMOUR counter has no threshold at all. The whole armour sub-draw
 * 0x80035554..0x800359B0 has now been read instruction by instruction: it is
 * the five-arm icon select (0x8003564C / 0x8003576C / 0x80035800 / 0x80035894 /
 * 0x80035928), the two `sb v0, 8(a2)` at 0x80035748 and 0x80035990 that put the
 * ICON's own palette into the ICON's rect record, the zero guard at
 * 0x80035994 and the `jal 0x80034F90` at 0x800359A8. There is no compare of
 * the armour or cells value against anything, and nothing writes byte 8 of a
 * DIGIT record anywhere in it — so the armour digits keep the numeral palette
 * the field initialiser left there (0x800337FC..0x80033814) at every value.
 *
 * Passed rather than special-cased so the two call sites read the same as the
 * health and ammo ones. Do not "restore" the 6 here: sharing the ammo constant
 * is what made the armour readout blink red below six points.
 */
#define Q2_SBAR_LOW_NONE     0

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
 * After walking the field records ten bytes at a time — seventeen of them for
 * one player: the count is a2's low half, and 0x80033D00-0x80033D18 load it
 * from 0x800AE800, which holds 17 (16 for the two-player hooks, 11 for the
 * quad one) — `0x80035EA0` draws TWO more sprites that are not fields at all.
 * They have their own position table and their own guards:
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
 * They are LEFT EDGES. The port used to subtract an icon width from them, on a
 * measurement rather than a read; the emitter settles it —
 *
 *     80033448  lhu  v0, 0(a1)      the table's x
 *     8003344C  lhu  v1, 4(a1)      the drawn w
 *     80033454  addu v0, v0, v1
 *     80033458  sh   v0, 20(a0)     -> vertex 1's x
 *
 * so x is the left edge and x + w the right. The three carousel cells then
 * tile the row: 388..420 (previous), 423..455 (field 12, the weapon in hand)
 * and 458..490 (next). The old capture measurement that made the strip look
 * right-anchored — ink at framebuffer columns 427..451, centre 439 — is
 * exactly centred on FIELD 12's cell, not on a strip slot, which is what it
 * would be with only a blaster held: the cycler returns 0 for both slots then
 * and the strip draws nothing at all.
 *
 * Its drawn size is the rect's OWN w and h, copied through with no
 * split-screen clamp (0x8003622C / 0x80036230 store sp+18 and sp+19, the rect
 * bytes 2 and 3, straight into the dest w/h). Only the one-player hook asks
 * for the strip — 0x80033CFC passes `a3 = 1` while 0x80034268, 0x80034810 and
 * 0x80034E4C all pass zero — so the second row of the table is dead data and
 * the missing clamp is unobservable, but it is transcribed as read.
 *
 * That still accounts for what capture shows — one icon with only the blaster
 * held, two once a second weapon is picked up — but by a different route than
 * this paragraph used to give. It said the one icon was a strip slot, drawn
 * once because the slots agreed. It is FIELD 12's: with only the blaster the
 * cycler returns zero for both slots and the strip draws nothing. With a
 * second, loaded weapon that weapon is both previous and next, slot A yields to
 * slot B (0x80036198), and the two icons are field 12 at 423 and slot B at
 * 458. A third weapon brings slot A in at 388.
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
    /* The record's bytes 8 and 9 as the hook initialises them: the palette
     * (38, 9 or 8) and the sheet's VRAM slot (14). Named init_w / init_h until
     * the emitter was read — they are not a size (see the top of this file).
     * Nothing reads them; they are transcribed so the table matches the hook
     * line for line. */
    u8  init_pal, init_slot;
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

/* The record's bytes 4..7, which are the same in all four layouts: u = v = 255
 * and w = h = 1 — the blank, which the emitter skips (0x80035F5C) until a
 * sub-draw overwrites it. Bytes 6 and 7 are the drawn size AND the UV span, so
 * there is no separate draw size to initialise. Initial values, not layout. */
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
    /*
     * HEALTH IS SIGNED, and it is the only counter that is. 0x80035224 clamps
     * it at -99 (and writes the clamp BACK into entity+264, which is a sim-side
     * side effect the port deliberately does not copy — see statusbar.c) and
     * 0x80035280 reloads it with a signed `lh` before passing signed = 1 to
     * 0x80034F90. A dead player reads "-25", not "0".
     *
     * The console has a second arm the port cannot reach: with no entity
     * (0x8003528C) all three digit fields take palette 7 and the value comes
     * from a cache at view+312 rather than from the player. A viewport in this
     * port always has a client, so it is recorded and not modelled.
     */
    s16 health;
    s16 armour;
    s16 ammo;
    s16 frags;

    /*
     * 1-based. It stands in for BOTH of the console's weapon ids — see the
     * client+98 / client+102 block at the top of this header — so it selects
     * the ammo pool and the ammo icon (which read +98) and field 12's icon and
     * the carousel (which read +102) alike.
     */
    int weapon;

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
     * Every writer makes the same pair of calls; 0x80037ECC is the plainest:
     *
     *     80037EDC  lh   a1, 102(s0)          the SELECTED weapon
     *     80037EE0  jal  0x80050758           a2 = 1, walk forwards
     *     80037EEC  lh   a1, 102(s0)
     *     80037EF4  jal  0x80050758           a2 = 0, walk backwards
     *     80037EF8  sh   v0, 100(s0)          (delay slot) +100 = NEXT
     *     80037EFC  sh   v0, 96(s0)           +96 = PREVIOUS
     *
     * So slot A at x = 388 is client+96, the PREVIOUS weapon, and slot B at
     * x = 458 is client+100, the NEXT one. That the rect index IS the weapon id
     * is read rather than assumed too: 0x80037CF0 `sll a0, v1, 2` +
     * `addu a0, a0, v1` makes field 12's rect index out of the weapon id by the
     * same *5 stride, and the strip does it at 0x800361A0.
     *
     * SIX FUNCTIONS WRITE THEM, not four. This block used to name four, and the
     * two it missed are the two that SELECT a weapon. `q2psx-inspect access
     * 0x60` and `access 0x64` scan the whole image for those displacements; the
     * halfword stores into the client record are six pairs, each in the delay
     * slot of the second call to 0x80050758 and just after it:
     *
     *     +100 / +96        in            on
     *     80037EB0 / EB4    0x80037E28    a weapon pickup (11 JAL callers)
     *     80037EF8 / EFC    0x80037ECC    an ammo pickup  (6 JAL callers)
     *     8003B220 / 224    0x8003B040    the ALL WEAPONS game variable
     *     8003D610 / 614    0x8003D4FC    the spawn loadout
     *     8004EDB4 / DBC    0x8004ECB4    weapon next / previous
     *     8004FB8C / B98    0x8004F87C    the refire pass's auto-select
     *
     * and 0x80050758's thirteen JAL callers are those twelve plus 0x8004ED20,
     * the select's own step. The other +96 / +100 stores `access` lists are
     * not these fields: 0x800459C0/C4, 0x8004673C/40 and 0x8003A57C/80 write an
     * ENTITY's contact normal (+152 is the move flags beside all three, and
     * 0x8003A578 puts 4096 in +98 between the two zeroes — the vector
     * (0, 4096, 0)); 0x80040760 is an ammo store at an index-scaled address in
     * the "PlayerSave0" block (a1 = that block + 16, 0x800406F0); 0x8006BB34
     * copies entity+704 into the block 0x8006B924 is handed in a2; 0x80098D78
     * walks an array in 0x80098C28's own table; the rest are word stores and
     * GPU packets (0x800AEDD4, 0x800B007C).
     *
     * WHEN THEY RUN is what makes the pair a latch rather than a formula.
     * Nothing else rewrites it, so between events it holds what the last one
     * saw:
     *
     *   - A POOL THAT FALLS WRITES NOTHING. A shot, or the power armour
     *     spending cells on a hit (0x80057BC8), can take a shared pool below
     *     a neighbour's one-shot minimum and leave the neighbour in its slot:
     *     the hyperblaster at 49 cells keeps the BFG (50) at 458, the shotgun
     *     at one shell keeps the super shotgun (2). It goes at the next
     *     event, which for an emptied gun is the dry trigger pull — the fire
     *     function returns 2 (the shotgun's on zero shells,
     *     0x8004C1FC-0x8004C21C), and 0x8004FB44 sends that, or the flag
     *     0x8004FEE8 raises at viewmodel+216 on the same result (0x80050230),
     *     to 0x800506C4 and the pair.
     *   - A WEAPON PICKUP WALKS BEFORE ITS AMMO IS ADDED. 0x80036410 calls
     *     0x80037E28, and only then does 0x80037DFC add the shells
     *     (0x80036444, stored at 0x80036458), so a gun picked up onto an
     *     empty pool stays out of the slots until the next event. The grenade
     *     box (0x80036A98) and the launcher's second call (0x800366D8) are
     *     the exceptions: each stores the grenades in that JAL's delay slot,
     *     so both walk with them in.
     *   - AN AMMO PICKUP WALKS AFTER: the pool is stored in 0x80037ECC's own
     *     delay slot (0x800369C4 for shells). The power shield calls it only
     *     when the cells were below the cap (0x80036DD8 branches past both).
     *   - THE BANDOLIER (0x8003701C-0x800370B0) AND THE AMMO PACK
     *     (0x80036F10-0x80037018) raise pools and write nothing: every JAL
     *     between those bounds is 0x80037DFC.
     *   - THE SELECT WRITES EVEN WHEN ITS STEP FINDS NOTHING: the delay slot
     *     of 0x8004ED3C sets a2 = 1 whatever 0x8004ED20 returned, and
     *     0x8004ED8C tests only a2.
     *   - 0x8003B040 WRITES ONLY UNDER ALL WEAPONS: 0x8003B070 branches to the
     *     epilogue unless bit 0x20 of 0x800B29EC (Q2_CHEAT_ALL_WEAPONS) is up.
     *     WHAT IT GRANTS used to be recorded here as unknown; it is not, and
     *     the whole routine is four steps:
     *       1. ids 1..11 in a loop (0x8003B0A8-0x8003B168). Each id's TWELVE
     *          NAME BYTES at 0x8009DB9C + id*12 ("Blaster G", "Shotgun G", …)
     *          are reassembled into three registers and handed to 0x8006D008,
     *          the CastList lookup (`q2_model_bank_find`). 0x8003B13C skips the
     *          id when it returns zero, so ALL WEAPONS grants ONLY the weapons
     *          whose view model this map actually ships — a shorter list than
     *          "every weapon" on most zones.
     *       2. the owned bit for a surviving id is ORed into client+104 from
     *          0x8009DC2C + id*4 (0x8003B144-0x8003B154). Nothing else in the
     *          loop writes, so no ammo is added per weapon.
     *       3. the SIX AMMO POOLS at client+108..+118 are filled from
     *          0x8009C5C8 + client+74 (0x8003B174-0x8003B20C) — the max-ammo
     *          table, `q2_ammo_max` in inventory.c, and client+74 is the
     *          element index of the player's tier row (0, 6 or 12), so every
     *          pool goes to the CAP OF THE TIER HELD, bandolier and pack
     *          included.
     *       4. client+102 = client+98 (0x8003B204), then the strip pair. It
     *          does NOT run the preference walk: the gun in your hands after an
     *          ALL WEAPONS grant is the one that was already in them.
     *
     * THE PORT. q2_statusbar_weapon_slots() below is the pair of calls, and
     * this array is the latch. The client calls it on spawn, selection,
     * auto-selection and the model-filtered ALL WEAPONS grant, including
     * respawn. The campaign reward and loaded Game Config now provide the
     * unlock tier that exposes the GOLD page. A caller that sees an event
     * calls the writer there; the pickups happen inside the
     * sim, where a once-a-frame caller cannot see them, and
     * q2_statusbar_weapon_slots_track() infers those from what they leave.
     * Recomputing every frame instead is NOT what the console does, and shows
     * in the cases above: the falling pool, the pickup onto an empty pool, the
     * bandolier and the pack. 0x80050758's own gates are transcribed in
     * q2_weapon_cycle (weapon.c).
     */
    u8 strip[Q2_SBAR_STRIP_SLOTS];

    /*
     * What the last write saw, for q2_statusbar_weapon_slots_track. Port
     * state, not the console's — the console needs none, because its writers
     * run at the events themselves.
     */
    bool strip_written;              /* a write since init or NULL inventory */
    int  strip_weapon;               /* the id that write walked from        */
    u16  strip_owned;                /* the owned set it saw                 */
    s16  strip_ammo[Q2_AMMO_COUNT];  /* the pools as the last call saw them  */

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

/*
 * The SIGNED three-cell formatter: glyph 10 is minus, 11 is blank, and the
 * value is clamped to -99 first (0x80035224 for health, 0x80037DB8 for frags).
 *
 * Formerly q2_sbar_frag_glyphs, and renamed because it is not frags-only:
 * 0x80034F90's `signed` argument is 1 for health (0x800352A4/0x800352AC) and
 * for the frag counter (0x80037DCC/0x80037DE0) and ZERO for ammo (0x80035494),
 * armour (0x800359A4) and the powerup timer (0x80035E18). The sign lands in
 * the TENS cell when the tens digit is blank and in the HUNDREDS cell
 * otherwise (0x80034F58-0x80034F84), so -5 is {blank, -, 5} and -25 is
 * {-, 2, 5}.
 */
void q2_sbar_signed_glyphs(int value, u8 out[Q2_SBAR_COUNTER_DIGITS]);

/* The frag counter's own name for it, kept so existing callers read plainly. */
void q2_sbar_frag_glyphs(int frags, u8 out[Q2_SBAR_COUNTER_DIGITS]);

/*
 * THE NUMERALS HAVE THEIR OWN SPLIT-SCREEN SIZE, and it is not the icon clamp.
 *
 *     80035054  lw   v0, 0x800AEBCC     split-screen at all?
 *     80035060  beq  v0, zero, ...E8    no  -> the record's own 24 x 24
 *     80035068  lh   v1, 0x800B3356     the player count
 *     80035070  bne  v1, 2, 0x80035084
 *     80035074  addiu v1, zero, 13      DELAY SLOT: v1 = 13 either way
 *     80035078  addiu v1, zero, 18      players == 2
 *     8003507C  j    0x80035088
 *     80035080  addiu v0, zero, 20      DELAY SLOT: h = 20
 *     80035084  addiu v0, zero, 12      players 3-4: 13 x 12
 *
 * So 1P is 24 x 24, 2P is 18 x 20 — TALLER than wide, against a square source
 * — and 3-4P is 13 x 12. The ICON clamp is other code in another function:
 * 0x800353B0 is 0x35C bytes (215 instructions) on, inside the ammo sub-draw
 * 0x800352C0 rather than this numeral row 0x80034F90, and field 12's
 * 0x80037CAC carries its own copy at 0x80037D20-0x80037D64. It writes 24/18
 * and 16/12 from the same two globals in the same instruction shape, which is
 * how the two got conflated. (This line used to put it twenty instructions
 * away.)
 *
 * The field pitches confirm it without the disassembly: the 2H digit row steps
 * 20 (46/66/86) and the quad row steps 14 (16/30/44), so the icon clamp's 24
 * and 16 would overlap and 18 and 13 fit.
 */
q2_icon_size q2_sbar_digit_size(int players);

/*
 * The ammo pool the counter reads — 0x80035424-0x80035438.
 *
 *     80035424  addu v0, sp, v0        v0 = (s16)s0, s0 = client+98, IN HAND
 *     80035428  lbu  v0, 40(v0)        ammoIdx[weapon], copied from 0x800ABEA8
 *     80035430  sll  v0, v0, 1
 *     80035434  addu v0, a3, v0
 *     80035438  lhu  v0, 108(v0)       ammo[type], six halfwords at client+108
 *
 * The index table is consumed with the 1-BASED weapon id, and its twelve bytes
 * at 0x800ABEA8 are {0,0,0,0,1,1,2,2,3,4,5,4} — byte for byte the low bytes of
 * the twelve words at 0x8009DC5C that the weapon cycler's ammo gate reads
 * (0x800507C0). So this uses `q2_weapon_tables_builtin()->ammo_type[]` — the
 * port's transcription of 0x8009DC5C (weapontables.c; `q2psx-inspect weapons`
 * diffs it against the disc), and the very table q2_weapon_cycle gates the
 * carousel on — so the counter and the carousel cannot disagree about which
 * pool a gun eats.
 *
 * The 0x800ABEA8 bytes themselves ARE loaded, into q2_icon_tables.ammo_kind
 * (icontable.c), and nothing read them before this either. They are not used
 * here only because the values are identical and one table for both readers
 * is the point; test_hud pins this function to the disc's twelve bytes.
 *
 * ID 0 IS READ LIKE ANY OTHER. Nothing between 0x800352E8's load and the read
 * tests for zero, so client+98 = 0 reads byte 0, pool 0 — the SHELLS count —
 * beside rect 0, the 1 x 1 blank (ammoIcon[0] is 0). The blanking branch at
 * 0x8003549C is for id 1 alone.
 * This used to return 0 for id 0, which no instruction here does. Ids past 11
 * are refused: they would read past the twelve-byte copy at sp+40.
 */
s16 q2_sbar_ammo_for_weapon(const q2_inventory *inv, int weapon_id);

/*
 * Write the carousel's two slots the way each of the six writers does — see
 * `strip` above. Unconditional, as they are: call it where one of the events
 * happens, not once a frame. `weapon_id` is the SELECTED weapon (client+102),
 * 1-based; zero in a slot means the walk came back to the weapon in hand and
 * draws nothing. A NULL inventory empties both slots.
 */
void q2_statusbar_weapon_slots(q2_statusbar *b, const q2_inventory *inv,
                               int weapon_id);

/*
 * The latch, for a caller that runs once a frame and cannot see the events
 * (see `strip`). It calls q2_statusbar_weapon_slots() when this call shows a
 * mark one of them leaves, and otherwise keeps the pair it has:
 *
 *   - no write since q2_statusbar_init or a NULL inventory — the spawn the
 *     bar did not see;
 *   - `weapon_id` is not the id the last write walked from — a select, an
 *     auto-select, or a pickup's switch off the blaster (0x80037E8C);
 *   - the owned set is not the one the last write saw — a weapon pickup
 *     (0x80037E7C) or a new loadout;
 *   - a pool is HIGHER than on the previous call — an ammo pickup, or the
 *     power shield's cells.
 *
 * A pool going DOWN is never a reason. That is a shot or the power armour, and
 * neither writes: it is the whole difference from recomputing every frame.
 *
 * INFERRED — the events are read off their effects rather than caught — and it
 * differs from the console where the effect and the event part company:
 *
 *   - a weapon pickup is walked with its ammo already in, where 0x80036410
 *     walks before 0x80037DFC adds it (the grenade box and the launcher
 *     excepted), so a gun picked up onto an empty pool shows at once rather
 *     than at the next event;
 *   - the bandolier and the ammo pack raise pools, so this writes where the
 *     console's two handlers do not;
 *   - an event that leaves nothing to see is missed: a select or auto-select
 *     that keeps the weapon, a weapon pickup onto a full pool, a spawn into
 *     the same loadout. The select, the auto-select and the spawn are the
 *     caller's own, and it should call q2_statusbar_weapon_slots at each;
 *   - what happens between two calls is seen as its net effect, so a pickup
 *     and a shot from one pool in the same frame show as their sum.
 *
 * Returns true when it wrote the pair.
 */
bool q2_statusbar_weapon_slots_track(q2_statusbar *b, const q2_inventory *inv,
                                     int weapon_id);

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
 * Whether death has stripped the bar — the 0x80033C68 gate described at the
 * top of this header: the one-player layout with health at or below zero.
 * q2_statusbar_build_ot applies it to the sprites. It is exposed because the
 * PICKUP CAPTION'S TEXT is the other half of the same skipped sub-draw
 * (0x800359C0 formats it at 0x80035B14 and prints it at 0x80035B20) and lives
 * in hud.c, so whoever drives q2_hud_pickup_build_ot needs the same answer.
 *
 * 0x800359C0 is also where the caption EXPIRES, so the expiry CHECK is skipped
 * while dead — but only the check. The deadline is absolute: 0x80035A34 loads
 * the level clock, 0x80035A38 the word at client+188, and 0x80035A40 `sltu`
 * compares them before 0x80035A4C clears client+84. So the time still passes;
 * a caption whose deadline went by during death is cleared by the first live
 * frame's check instead of at the deadline, and is never drawn again. (This
 * used to say a dead player's caption is not aged. It is; only the check
 * waits.)
 */
bool q2_statusbar_stripped(const q2_statusbar *b);

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
