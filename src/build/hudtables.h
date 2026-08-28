/*
 * hudtables.h — the HUD's data tables, read out of the boot executable.
 *
 * The console's HUD is not a status bar. There is no health number, no ammo
 * counter and no armour readout anywhere in the game: a sweep of every format
 * string the executable ever hands to its text layer — all 30 call sites of the
 * printf wrapper at 0x80043518 and all 3 of the queueing wrapper at 0x800434B8
 * — turns up mission text, debug overlays, pickup names and a weapon-selected
 * line, and nothing that formats a player statistic. What Hammerhead shipped
 * instead is a message overlay, a crosshair and a screen flash whose colour
 * says whether the hit landed on armour or on flesh. That negative result is
 * load-bearing: a port that "restores" a status bar is adding a feature, not
 * reconstructing one.
 *
 * Everything the overlay draws comes from three places, and all three are in
 * the executable or in a file already on the disc:
 *
 *   - the glyph and icon atlas          `chars.lbm`, a VRAM image in SNDVRAM.DAT
 *   - where each glyph lives in it      the tables described below
 *   - what colour it comes out          the built-in palette bank, also below
 *
 * So this module ships no artwork and no letterforms. It reads coordinates.
 *
 * ---------------------------------------------------------------------------
 * The atlas
 * ---------------------------------------------------------------------------
 * `chars.lbm` is a standalone image in 47 of the 49 `SNDVRAM.DAT` files, 128
 * bytes per row by 128 rows. It is **4bpp**, so 256 x 128 texels, and it is
 * uploaded to VRAM at **(0 halfwords, 384)** — that is texture page (0, 256)
 * with a v origin of 128, which is why every glyph's stored v is >= 0x80.
 *
 * The placement is not a guess. `0x8003FEA4` registers it with slot 15 and a v
 * offset of 128; `0x8006901C` turns the slot into x = tblX[15] = 0 and
 * y = tblY[15] = 256 from the tables at `0x800A3274` / `0x800A329C`; and
 * `0x800691A8` adds the v offset to y before calling `LoadImage`, so the rect
 * is {0, 384, width>>1, 128}. The world's CLUT array occupies rows 256..~285 of
 * the same page and the 13 texture pages start at x = 64, so nothing collides.
 *
 * ---------------------------------------------------------------------------
 * Glyphs — 0x8009D554, two bytes each, index = c - 32
 * ---------------------------------------------------------------------------
 * Every glyph is an **8 x 8 sprite** (`SPRT_8`, primitive code 0x74, emitted at
 * 0x800426BC). The table gives its (u, v) inside the page; there is no width
 * field because there is no proportional spacing — the pen advances by a fixed
 * 8 after a drawn glyph.
 *
 * 92 entries, ' ' (32) through '{' (123), bounded by the icon table that starts
 * immediately after. The rows it addresses are: lowercase at v = 0x80, the
 * word-graphics at 0x98/0xA0/0xA8, digits at 0xB0, 'A'..'P' at 0xB8, and the
 * rest of upper case plus punctuation at 0xC0. '@' maps to (0, 0), which is
 * blank — the markup layer consumes '@' before it can ever be drawn.
 *
 * ---------------------------------------------------------------------------
 * Icons — 0x8009D60C, four bytes each: u, v, w, h
 * ---------------------------------------------------------------------------
 * Fifteen entries, drawn as free-size sprites (code 0x64, 0x8004327C). They are
 * not pictures: they are pre-rendered **words** cut out of atlas rows 0x98,
 * 0xA0 and 0xA8, and several of them deliberately overlap so that one bitmap
 * serves two names. "SuperShotgun" spans u = 0x90..0xF0 and "Shotgun" is its
 * last 56 pixels; "gun" is the last 24 of that; "GrenadeLauncher" is "Grenade"
 * followed by "Launcher" and all three are separate entries over one run of
 * pixels. That is what makes an eleven-weapon vocabulary fit in fifteen boxes.
 *
 * ---------------------------------------------------------------------------
 * The shaded backdrop — 0x8009D524 (uv) and 0x8009D534 (rgb), eight each
 * ---------------------------------------------------------------------------
 * `*N` in a string does not draw anything at the time it is read. It arms a
 * second pass (0x8004288C) that walks the glyph positions recorded during the
 * first pass and drops a 16 x 16 sprite behind every one of them, four pixels
 * up and left. The tiles sit at v = 0xF0 and get progressively smaller as N
 * falls, so decrementing N once per frame — which the message layer does — is
 * how the box behind a centred message fades out.
 *
 * ---------------------------------------------------------------------------
 * Palettes — 0x800A2FEC, eight-byte records, NULL-terminated
 * ---------------------------------------------------------------------------
 * The font's colours are in the *executable*, not in `SNDVRAM.DAT`. A boot-time
 * loop at `0x8007610C` walks the table, copies each 16-entry palette through a
 * per-record 16-bit mask that decides which entries get the semi-transparency
 * bit, uploads it as a 16 x 1 rect, and records the hardware CLUT id in a table
 * at `0x800E3F2C` indexed by the record's own id field:
 *
 *     record  { const u16 *entries; u16 stp_mask; u16 index; }
 *
 * Placement walks columns of seven: record k (1-based) goes to
 * x = 16 * ((k-1) / 7), y = 248 + ((k-1) % 7). Rows 248..254 are the gap
 * between the two 512 x 248 framebuffers and the texture half of VRAM, which is
 * why that band is free. Record 0 is different: a 256-entry palette uploaded to
 * (0, 255) as a 256 x 1 rect.
 *
 * Four of the eighty matter to the HUD:
 *
 *     72   the font, and the crosshair          `|0`
 *     74   the alternate font palette           `|1`, and the default
 *     76   the ramp the `*N` backdrop shades with
 *     70   a white-only mask palette the menu uses
 *
 * ---------------------------------------------------------------------------
 * Weapon glyphs — 0x8009DC8C, three bytes each, CONSUMED 1-BASED
 * ---------------------------------------------------------------------------
 * Twelve three-byte strings, indexed directly by the live weapon id, which runs
 * 1..11 with 0 meaning "no weapon" (FORMATS.md §9.6). Slot 0 is "  ", so an
 * unarmed player's weapon-selected line prints blanks rather than reading off
 * the front of the table. Every other slot is an icon escape: "&B" for the
 * blaster, "&U" for the super shotgun, "&F" for the BFG. They are markup, not
 * text — which is how `Selected %s` at 0x8004EDF0 prints a picture of a word.
 */
#ifndef Q2PSX_HUDTABLES_H
#define Q2PSX_HUDTABLES_H

#include "ident.h"
#include "q2psx.h"

/* ------------------------------------------------------------------------- */
/* Where the tables are in the catalogued PAL build. A localised release moves
 * them, which is why loading is gated on identification rather than attempted
 * blind — the addresses below would still read *something* on another disc. */
#define Q2_HUD_ADDR_GLYPH_UV    0x8009D554u
#define Q2_HUD_ADDR_ICON        0x8009D60Cu
#define Q2_HUD_ADDR_BOX_UV      0x8009D524u
#define Q2_HUD_ADDR_BOX_RGB     0x8009D534u
#define Q2_HUD_ADDR_MSG_LINES   0x8009D648u
#define Q2_HUD_ADDR_PALETTES    0x800A2FECu
#define Q2_HUD_ADDR_WEAPON_GLYPH 0x8009DC8Cu
#define Q2_HUD_ADDR_WEAPON_NAME  0x8009DBA8u

#define Q2_HUD_GLYPH_FIRST   32     /* ' ' */
#define Q2_HUD_GLYPH_COUNT   92     /* ' ' .. '{'                            */
#define Q2_HUD_GLYPH_W        8
#define Q2_HUD_GLYPH_H        8
#define Q2_HUD_ICON_COUNT    15
#define Q2_HUD_BOX_LEVELS     8     /* `*0` .. `*7`                          */
#define Q2_HUD_BOX_SIZE      16     /* the backdrop tile is SPRT_16          */
#define Q2_HUD_WEAPON_SLOTS  12     /* slot 0 blank, then weapon ids 1..11   */
#define Q2_HUD_MSG_TIERS      6     /* message lines by player count         */
#define Q2_HUD_PALETTE_MAX   96     /* 80 on this build; headroom, not a claim */
#define Q2_HUD_PALETTE_SIZE  16

/* The four palette ids the overlay names. See the header comment. */
#define Q2_HUD_PALETTE_FONT      72
#define Q2_HUD_PALETTE_FONT_ALT  74
#define Q2_HUD_PALETTE_BOX       76
#define Q2_HUD_PALETTE_MASK      70

/* Where chars.lbm lands, in the PSX's own units: x counts 16-bit halfwords. */
#define Q2_HUD_ATLAS_VRAM_X    0
#define Q2_HUD_ATLAS_VRAM_Y  384
#define Q2_HUD_ATLAS_PAGE_X    0    /* x / 64 halfwords                      */
#define Q2_HUD_ATLAS_PAGE_Y    1    /* 0 => row 0, 1 => row 256              */
#define Q2_HUD_ATLAS_NAME    "chars.lbm"

/* Two bytes: where an 8x8 glyph, or a 16x16 backdrop tile, sits in the page. */
typedef struct q2_hud_uv { u8 u, v; } q2_hud_uv;

/* Four bytes: a word-graphic cut out of the atlas. */
typedef struct q2_hud_icon { u8 u, v, w, h; } q2_hud_icon;

/*
 * One built-in palette, already fixed up. `entry` holds what the boot loop
 * uploads — the source colours with the semi-transparency bit ORed in wherever
 * `stp_mask` has a set bit — and `clut_id` is the hardware CLUT word for its
 * VRAM position, so a caller can put it straight into a primitive.
 */
typedef struct q2_hud_palette {
    u16 entry[Q2_HUD_PALETTE_SIZE];
    u16 stp_mask;
    s16 vram_x, vram_y;
    u16 clut_id;
    bool present;
} q2_hud_palette;

typedef struct q2_hud_tables {
    q2_buf exe;                 /* owns the executable image */

    q2_hud_uv   glyph[Q2_HUD_GLYPH_COUNT];
    q2_hud_icon icon[Q2_HUD_ICON_COUNT];
    q2_hud_uv   box[Q2_HUD_BOX_LEVELS];
    u8          box_rgb[Q2_HUD_BOX_LEVELS][3];

    /* "&B", "&U", ... indexed by the 1-based weapon id. Slot 0 is "  ". */
    char        weapon_glyph[Q2_HUD_WEAPON_SLOTS][4];

    /*
     * The parallel 12-byte name table, same 1-based indexing. These are the
     * executable's own names — "Blaster G", "Supershot G" — and they are also
     * what the CastList bank calls the view models, which is the only link
     * between the model in the player's hands and the glyph that names it.
     * Slot 0 is empty. Two entries fill all twelve bytes with no NUL, so the
     * copy here is width-limited rather than NUL-terminated at the source.
     *
     * weapontables.h reads the same array from its 1-based base at 0x8009DB9C
     * alongside the rest of the weapon parallel arrays. It is duplicated here so
     * that resolving a view model to its glyph needs only this module; if the
     * two ever disagree, weapontables.h is the fuller home and wins.
     */
    char        weapon_name[Q2_HUD_WEAPON_SLOTS][13];

    /* How many notification lines are shown, by player count. Index 1 is the
     * single-player case and reads 4; four-way split gets 1. */
    u8          message_lines[Q2_HUD_MSG_TIERS];

    q2_hud_palette palette[Q2_HUD_PALETTE_MAX];
    u32            palette_count;

    u16         palette256[256];   /* record 0, uploaded to (0, 255) */
    u16         palette256_id;
} q2_hud_tables;

/*
 * Read the HUD tables out of the disc's boot executable.
 *
 * Fails with Q2_ERR_UNSUPPORTED on a build whose table locations are not
 * catalogued rather than reading a plausible-looking wrong address: a font
 * table read at the wrong offset does not fail, it produces a legible-looking
 * alphabet made of the wrong letters.
 */
q2_result q2_hud_tables_load(q2_hud_tables *out, const disc *d,
                             const q2_build_id *id);
void      q2_hud_tables_free(q2_hud_tables *t);

/* The palette with the given built-in id, or NULL. */
const q2_hud_palette *q2_hud_palette_get(const q2_hud_tables *t, u32 id);

/*
 * Put the whole bank in VRAM, at the (x, y) each record's position gives it —
 * the 256-entry record zero at (0,255), then the sixteen-entry records placed
 * by the boot loop at `0x8007610C`. Both the overlay and the menu draw out of
 * it, so it lives here rather than in either of them; calling it twice is
 * harmless because it writes the same halfwords to the same places.
 */
struct psx_vram;
void q2_hud_palettes_upload(const q2_hud_tables *t, struct psx_vram *vram);

/* The hardware CLUT word for a built-in palette id, or 0 when it is absent. */
u16 q2_hud_palette_clut(const q2_hud_tables *t, u32 id);

/*
 * Which weapon a view model belongs to, matched case-insensitively against the
 * name table. Returns the 1-based weapon id, or 0 for "not a weapon" — which is
 * also the id that selects the blank glyph, so a caller can pass the result
 * straight on.
 */
int q2_hud_weapon_by_model(const q2_hud_tables *t, const char *model_name);

/* ------------------------------------------------------------------------- */
/* The icon escape                                                            */
/*                                                                            */
/* `&X` is dispatched through a 23-entry jump table at 0x800ACA1C covering     */
/* 'A'..'W' (0x80043158). Six of the letters land on the no-op arm, and five   */
/* emit TWO icons, which is how a name that the atlas stores as two adjacent   */
/* words — "Machine" + "gun" — draws as one. This is control flow rather than  */
/* a data table, so it is transcribed here with the address of each arm rather */
/* than read from the image.                                                   */
/* ------------------------------------------------------------------------- */
#define Q2_HUD_ICON_ESCAPE_MAX 2

/*
 * Resolve `letter` to up to two icon indices. Returns how many were written,
 * and 0 for a letter that draws nothing — which is six of the twenty-three as
 * well as everything outside the range, since the original bounds-checks
 * `(unsigned)(c - 'A') < 23` before it branches.
 */
int q2_hud_icon_escape(char letter, u8 out[Q2_HUD_ICON_ESCAPE_MAX]);

#endif /* Q2PSX_HUDTABLES_H */
