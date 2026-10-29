/*
 * test_statusbar.c — the bar that FORMATS.md §11.1 said did not exist.
 *
 * What is worth pinning here is the field arithmetic, because it is the part a
 * plausible-looking mistake survives: three counters of three digits with an
 * icon each, digits 24 apart because a numeral cell is 24 wide, values right
 * aligned so the units column does not move.
 */
#include "statusbar.h"
#include "hudtables.h"     /* a palette bank, so the flash is observable */
#include "raster.h"        /* the bar drawn, not just emitted            */
#include "weapon.h"        /* Q2_WID_* — the 1-based live ids */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
static int g_checks;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

static void test_field_groups(void)
{
    int c, d;

    /* Every counter has three digit fields and one icon field, and no field is
     * claimed twice — the grouping is what makes the layout legible at all. */
    {
        int seen[Q2_SBAR_FIELDS];
        memset(seen, 0, sizeof(seen));

        for (c = 0; c < Q2_SBAR_COUNTERS; c++) {
            int icon = q2_sbar_icon_field((q2_sbar_counter)c);
            CHECK(icon >= 0 && icon < Q2_SBAR_FIELDS, "counter %d has an icon", c);
            if (icon >= 0) {
                CHECK(!seen[icon], "field %d claimed twice", icon);
                seen[icon] = 1;
            }
            for (d = 0; d < Q2_SBAR_COUNTER_DIGITS; d++) {
                int f = q2_sbar_digit_field((q2_sbar_counter)c, d);
                CHECK(f >= 0 && f < Q2_SBAR_FIELDS, "counter %d digit %d", c, d);
                if (f >= 0) {
                    CHECK(!seen[f], "field %d claimed twice", f);
                    seen[f] = 1;
                }
            }
        }
    }

    CHECK(q2_sbar_digit_field((q2_sbar_counter)Q2_SBAR_COUNTERS, 0) < 0,
          "an out-of-range counter has no field");
    CHECK(q2_sbar_digit_field(Q2_SBAR_HEALTH, Q2_SBAR_COUNTER_DIGITS) < 0,
          "an out-of-range digit has no field");
}

static void test_digit_pitch(void)
{
    int c, d;

    /*
     * Within a counter the digits step by exactly one numeral width. That is
     * the invariant tying the field table (read from 0x800337EC) to the numeral
     * table (read from 0x8009C598) — two independent reads that have to agree,
     * and if either were misread they would not.
     */
    for (c = 0; c < Q2_SBAR_COUNTERS; c++) {
        for (d = 1; d < Q2_SBAR_COUNTER_DIGITS; d++) {
            int a = q2_sbar_digit_field((q2_sbar_counter)c, d - 1);
            int b = q2_sbar_digit_field((q2_sbar_counter)c, d);
            int step;

            if (a < 0 || b < 0)
                continue;
            step = q2_sbar_fields[b].dx - q2_sbar_fields[a].dx;
            CHECK(step == Q2_SBAR_DIGIT_PITCH,
                  "counter %d digits %d..%d step %d, want %d",
                  c, d - 1, d, step, Q2_SBAR_DIGIT_PITCH);
        }
    }

    /* And the numeral cell really is that wide. */
    CHECK(Q2_SBAR_DIGIT_W == Q2_SBAR_DIGIT_PITCH,
          "the cell and the pitch are the same 24");
    CHECK(Q2_SBAR_DIGIT_V == 168, "the numerals are the row at v = 168");
}

static void test_counters_are_ordered(void)
{
    /* Left to right: health, ammo, armour — from retail capture, and the only
     * thing here that is not a transcription. If this ever has to change it is
     * this test that should fail first. */
    int h = q2_sbar_digit_field(Q2_SBAR_HEALTH, 0);
    int a = q2_sbar_digit_field(Q2_SBAR_AMMO, 0);
    int r = q2_sbar_digit_field(Q2_SBAR_ARMOUR, 0);

    CHECK(q2_sbar_fields[h].dx < q2_sbar_fields[a].dx,
          "health is left of ammo");
    CHECK(q2_sbar_fields[a].dx < q2_sbar_fields[r].dx,
          "ammo is left of armour");

    /* Each counter's icon sits past its last digit. */
    {
        int c;
        for (c = 0; c < Q2_SBAR_COUNTERS; c++) {
            int last = q2_sbar_digit_field((q2_sbar_counter)c,
                                           Q2_SBAR_COUNTER_DIGITS - 1);
            int icon = q2_sbar_icon_field((q2_sbar_counter)c);
            CHECK(q2_sbar_fields[icon].dx > q2_sbar_fields[last].dx,
                  "counter %d's icon follows its digits", c);
        }
    }
}

static void test_two_rows(void)
{
    /* The upper row sits 24 to 25 above the main one — a second row, not a
     * continuation of the first. Capture shows a pickup caption's icon on its
     * left and a two-digit counter on its right. */
    CHECK(q2_sbar_fields[Q2_SBAR_FIELD_UP_LEFT].dy == -25,
          "the upper-left icon is 25 above");
    CHECK(q2_sbar_fields[Q2_SBAR_FIELD_UP_DIGIT0].dy == -24 &&
          q2_sbar_fields[Q2_SBAR_FIELD_UP_DIGIT1].dy == -24,
          "the upper digits are 24 above");
    CHECK(q2_sbar_fields[Q2_SBAR_FIELD_UP_DIGIT1].dx -
          q2_sbar_fields[Q2_SBAR_FIELD_UP_DIGIT0].dx == Q2_SBAR_DIGIT_PITCH,
          "and they step by one numeral");
    /* The one-player auxiliary icon is on the main row, far right. Numeric
     * frags use three fields only in the split hooks. */
    CHECK(q2_sbar_fields[Q2_SBAR_FIELD_AUX_ICON].dy == 0,
          "the auxiliary icon is on the main row");
    CHECK(q2_sbar_fields[Q2_SBAR_FIELD_AUX_ICON].dx >
          q2_sbar_fields[q2_sbar_icon_field(Q2_SBAR_ARMOUR)].dx,
          "and right of everything else");
}

static void test_two_player_layout(void)
{
    q2_sbar_field side[Q2_SBAR_FIELDS_2V];
    int i;

    /*
     * Both two-player hooks build sixteen fields. The earlier decode stopped
     * the stacked hook after nine and mislabelled the side-by-side hook as
     * quad; the selector at 0x8003FA10 proves which callback belongs to which.
     */
    for (i = 1; i < 3; i++)
        CHECK(q2_sbar_fields_2h[i + 1].dx - q2_sbar_fields_2h[i].dx == 20,
              "2H health digits step 20");
    CHECK(q2_sbar_fields_2h[8].dx == 354 &&
          q2_sbar_fields_2h[9].dx == 294,
          "2H includes the armour group omitted by the old transcription");
    CHECK(q2_sbar_fields_2h[13].dx == 422 &&
          q2_sbar_fields_2h[15].dx == 462,
          "2H signed frags occupy records 13..15");

    q2_sbar_2v_fields(248, side);
    CHECK(side[0].dx == 76 && side[4].dx == 230,
          "2V puts health and ammo along the bottom");
    CHECK(side[8].dy == 40 - 248 && side[13].dy == 40 - 248,
          "2V uses the live framebuffer height for armour and frags");
    CHECK(side[12].dy == 0,
          "2V record 12 does not take the height subtraction");
}

static void test_quad_layout(void)
{
    q2_sbar_field f[Q2_SBAR_FIELDS_QUAD];

    /*
     * The real quad hook is 0x80034830: eleven fields PER VIEWPORT, indexed
     * through the 44 halfwords at 0x8009C600 and four y values at 0x800AE808.
     */
    CHECK(q2_sbar_fields_quad[0][0].dx == 56 &&
          q2_sbar_fields_quad[0][10].dx == 238,
          "quad view 0 consumes the first eleven x offsets");
    CHECK(q2_sbar_fields_quad[1][0].dx == 208 &&
          q2_sbar_fields_quad[1][8].dx == 6,
          "quad view 1 consumes the second eleven x offsets");
    CHECK(q2_sbar_fields_quad[0][0].dy == 110 &&
          q2_sbar_fields_quad[2][0].dy == 1,
          "top views use y 110 and bottom views y 1");

    q2_sbar_quad_fields(1, 7, f);
    CHECK(f[10].dx == 6, "a one-digit right-view frag hugs the inner edge");
    q2_sbar_quad_fields(1, -7, f);
    CHECK(f[9].dx == 6 && f[10].dx == 20,
          "a signed right-view frag reserves minus plus one digit");
    q2_sbar_quad_fields(1, 100, f);
    CHECK(f[8].dx == 6 && f[9].dx == 20 && f[10].dx == 34,
          "three-digit right-view frags retain all three fields");
}

static void test_icon_vocabulary(void)
{
    /*
     * These check the ITEM table: effect 18 is Shells P, and so on. That is a
     * fact about `0x8009F5CC` and is unaffected by what follows.
     *
     * They used to be presented as proof that a rect record's fifth byte is an
     * effect id, on the strength of the weapon-to-ammo table lining up when
     * read that way. It is not — the byte is a palette index and the ammo table
     * holds rect indices, both straight off the disassembly (icontable.h).
     * The agreement these numbers show is real and means less than it looked
     * like: two near-monotonic sequences a small constant apart will line up
     * over any window you pick.
     */
    static const struct { u8 effect; const char *name; } ammo[] = {
        { 18, "Shells P"  },
        { 19, "Bullets P" },
        { 20, "Grenade P" },
        { 21, "Rockets P" },
        { 22, "Cells P"   },
        { 23, "Slugs P"   }
    };
    size_t i;

    for (i = 0; i < sizeof(ammo) / sizeof(ammo[0]); i++) {
        const char *got = q2_icon_name_for_id(ammo[i].effect);
        CHECK(got && strcmp(got, ammo[i].name) == 0,
              "effect %u is %s, got %s", ammo[i].effect, ammo[i].name,
              got ? got : "(none)");
    }

    /*
     * The icons the bar names for itself are RECT INDICES, not effect ids.
     *
     * These are the hard-coded offsets in the sub-draws divided by the
     * five-byte record. Pinned here because the reading that shipped before
     * treated them as effect ids and the mistake was invisible — scanning for
     * effect 34 finds rect 30, which is a real icon, just the wrong one.
     *
     * ONLY HEALTH IS UNCONDITIONAL. The armour field is a five-way select on
     * the flag word (0x80035554); this test used to pin the power shield's
     * rect as "the armour icon", which is the misreading that put a power
     * shield on the bar for every armoured player, and pinning it is what let
     * 28 of 28 tests pass while the bug shipped.
     */
    CHECK(Q2_SBAR_ICON_HEALTH == 170 / Q2_ICON_RECORD,
          "the health icon is rect 34, the offset 170 divided by the record");
    CHECK(Q2_SBAR_ICON_ARMOUR_BODY == 130 / Q2_ICON_RECORD,
          "body armour is rect 26, from offset 130 (flag 0x4000)");
    CHECK(Q2_SBAR_ICON_ARMOUR_COMBAT == 135 / Q2_ICON_RECORD,
          "combat armour is rect 27, from offset 135 (flag 0x2000)");
    CHECK(Q2_SBAR_ICON_ARMOUR_JACKET == 140 / Q2_ICON_RECORD,
          "jacket armour is rect 28, from offset 140 (flag 0x1000)");
    CHECK(Q2_SBAR_ICON_POWER_SHIELD == 150 / Q2_ICON_RECORD,
          "the power shield is rect 30, from offset 150 (flag 0x8000)");

    /* All five distinct: an off-by-one in the offsets above would otherwise
     * collide two fields onto one cell and still pass. */
    CHECK(Q2_SBAR_ICON_HEALTH != Q2_SBAR_ICON_ARMOUR_BODY &&
          Q2_SBAR_ICON_ARMOUR_BODY != Q2_SBAR_ICON_ARMOUR_COMBAT &&
          Q2_SBAR_ICON_ARMOUR_COMBAT != Q2_SBAR_ICON_ARMOUR_JACKET &&
          Q2_SBAR_ICON_ARMOUR_JACKET != Q2_SBAR_ICON_POWER_SHIELD &&
          Q2_SBAR_ICON_POWER_SHIELD != Q2_SBAR_ICON_HEALTH,
          "the five icons the bar names are five different cells");

    /* And the three palettes the bar selects between are three distinct
     * entries of the built-in bank — 8 cyan, 38 blue, 7 red. */
    CHECK(Q2_SBAR_PAL_DIGITS != Q2_SBAR_PAL_LOW,
          "the low-value flash is not the numerals' own palette");

    /* Zero is "no icon" and must not match the front of the table. */
    CHECK(q2_icon_name_for_id(0) == NULL, "effect 0 names nothing");
}

static void test_digits_of(void)
{
    u8 d[Q2_SBAR_COUNTER_DIGITS];

    CHECK(q2_sbar_digits_of(100, d) == 3 && d[0] == 1 && d[1] == 0 && d[2] == 0,
          "100 is three digits");
    CHECK(q2_sbar_digits_of(50, d) == 2 && d[0] == 5 && d[1] == 0,
          "50 is two");
    /* No leading zeroes: capture shows "2", not "002". */
    CHECK(q2_sbar_digits_of(2, d) == 1 && d[0] == 2, "2 is one");
    CHECK(q2_sbar_digits_of(0, d) == 1 && d[0] == 0, "zero still shows");

    /* Three cells is the ceiling, and a negative must not underflow. */
    CHECK(q2_sbar_digits_of(1234, d) == 3, "over 999 clamps");
    CHECK(q2_sbar_digits_of(-5, d) == 1 && d[0] == 0, "negative reads zero");
}

static void test_frag_glyphs(void)
{
    u8 g[Q2_SBAR_COUNTER_DIGITS];

    q2_sbar_frag_glyphs(7, g);
    CHECK(g[0] == Q2_SBAR_GLYPH_BLANK &&
          g[1] == Q2_SBAR_GLYPH_BLANK && g[2] == 7,
          "a positive single frag is right aligned");
    q2_sbar_frag_glyphs(-7, g);
    CHECK(g[0] == Q2_SBAR_GLYPH_BLANK &&
          g[1] == Q2_SBAR_GLYPH_MINUS && g[2] == 7,
          "-7 is blank, minus, seven");
    q2_sbar_frag_glyphs(-15, g);
    CHECK(g[0] == Q2_SBAR_GLYPH_MINUS && g[1] == 1 && g[2] == 5,
          "-15 is minus, one, five");
    q2_sbar_frag_glyphs(-200, g);
    CHECK(g[0] == Q2_SBAR_GLYPH_MINUS && g[1] == 9 && g[2] == 9,
          "retail clamps the negative end to -99");
}

static void test_split_screen_sizes(void)
{
    /*
     * THERE ARE TWO CLAMPS, and this test used to assert there was one. It
     * said "a 24-wide numeral and a 32-wide icon come out the same size in
     * split screen", which is the ICON clamp (0x800353B0: 24 x 18 / 16 x 12)
     * applied to numerals as well. The numerals have their own, twenty
     * instructions away at 0x80035054: 18 x 20 for two players — TALLER than
     * wide against a square source — and 13 x 12 for three or four.
     *
     * The whole point of what follows is that the two DIFFER.
     */
    CHECK(q2_sbar_digit_size(1).w == 24 && q2_sbar_digit_size(1).h == 24,
          "one player draws a numeral at its own 24 x 24");
    CHECK(q2_sbar_digit_size(2).w == 18 && q2_sbar_digit_size(2).h == 20,
          "two players give the numerals 18 x 20 (0x80035078 / 0x80035080)");
    CHECK(q2_sbar_digit_size(4).w == 13 && q2_sbar_digit_size(4).h == 12,
          "three or four give them 13 x 12 (0x80035074 / 0x80035084)");
    CHECK(q2_sbar_digit_size(3).w == 13 && q2_sbar_digit_size(3).h == 12,
          "three players take the same arm as four");

    /* The ICON clamp is unchanged, and is a different pair of numbers. */
    CHECK(q2_icon_draw_size_of(1, 1, 32, 24).w == 32,
          "single player keeps an icon at 32");
    CHECK(q2_icon_draw_size_of(2, 1, 32, 24).w == 24 &&
          q2_icon_draw_size_of(2, 1, 32, 24).h == 18,
          "two players clamp an icon to 24 x 18");
    CHECK(q2_icon_draw_size_of(4, 1, 32, 24).w == 16 &&
          q2_icon_draw_size_of(4, 1, 32, 24).h == 12,
          "four players clamp an icon to 16 x 12");
    CHECK(q2_sbar_digit_size(2).w != q2_icon_draw_size_of(2, 1, 24, 24).w ||
          q2_sbar_digit_size(2).h != q2_icon_draw_size_of(2, 1, 24, 24).h,
          "the numeral clamp and the icon clamp are NOT the same rule");

    /* No weapon collapses to the blank rather than scaling a 1x1 up. */
    CHECK(q2_icon_draw_size_of(1, 0, 32, 24).w == 1,
          "no weapon draws the blank");

    /*
     * And the pitch invariant that proves it without the disassembly: a
     * numeral has to fit between its neighbours. Both of these fail with the
     * icon clamp's 24 and 16 and pass with 18 and 13.
     */
    CHECK(q2_sbar_digit_size(2).w < q2_sbar_fields_2h[2].dx -
                                    q2_sbar_fields_2h[1].dx,
          "an 18-wide numeral fits the 2H row's 20-pixel step");
    CHECK(q2_sbar_digit_size(4).w < q2_sbar_fields_quad[0][2].dx -
                                    q2_sbar_fields_quad[0][1].dx,
          "a 13-wide numeral fits the quad row's 14-pixel step");

    /*
     * And on the bar itself, not just the helper: a stacked two-player
     * viewport draws its health numerals 18 x 20 at the 2H fields (46/66/86,
     * dy 0), while its health ICON beside them keeps the icon clamp, 24 x 18.
     */
    {
        q2_icon_tables icons;
        q2_statusbar b;
        psx_ot ot;
        u32 i;
        int digits = 0, sized = 0, icon_ok = 0;

        memset(&icons, 0, sizeof(icons));
        icons.rect_count = Q2_ICON_COUNT;
        icons.rect[0].w = 1;
        icons.rect[0].h = 1;
        icons.rect[34].u = 0;  icons.rect[34].v = 24;
        icons.rect[34].w = 32; icons.rect[34].h = 24;

        if (psx_ot_init(&ot, 64, 256) != Q2_OK) {
            CHECK(0, "an ordering table for the 2H numerals");
            return;
        }
        q2_statusbar_init(&b, &icons, 2);
        q2_statusbar_layout(&b, Q2_SBAR_LAYOUT_TWO_H, 0, 248);
        q2_statusbar_anchor(&b, 0, 95);
        b.health = 100;
        b.health_icon = Q2_SBAR_ICON_HEALTH;
        psx_ot_clear(&ot);
        q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);

        for (i = 0; i < ot.prim_count; i++) {
            const psx_prim *p = &ot.prims[i];
            int x = p->xy[0].x, w = p->xy[1].x - p->xy[0].x;
            int h = p->xy[3].y - p->xy[0].y;

            if (x == 46 || x == 66 || x == 86) {
                digits++;
                sized += (w == 18 && h == 20);
            } else if (x == q2_sbar_fields_2h[0].dx) {
                icon_ok = (w == 24 && h == 18);
            }
        }
        CHECK(digits == 3 && sized == 3,
              "2H draws \"100\" as three 18 x 20 numerals (%d of %d)", sized,
              digits);
        CHECK(icon_ok, "while its health icon stays on the icon clamp, 24 x 18");
        psx_ot_free(&ot);
    }
}

/* ------------------------------------------------------------------------- */
/* The ammo pool the counter reads — 0x80035424..0x80035438                   */
/* ------------------------------------------------------------------------- */
static void test_ammo_pool(void)
{
    q2_inventory inv;
    /* ids 2..11, spelled out from the twelve bytes at 0x800ABEA8:
     * {0,0,0,0,1,1,2,2,3,4,5,4} with slot order shells, bullets, grenades,
     * rockets, cells, slugs. */
    static const s16 want[] = { 11, 11, 22, 22, 33, 33, 44, 55, 66, 55 };
    int id;

    memset(&inv, 0, sizeof(inv));
    inv.ammo[Q2_AMMO_SHELLS]   = 11;
    inv.ammo[Q2_AMMO_BULLETS]  = 22;
    inv.ammo[Q2_AMMO_GRENADES] = 33;
    inv.ammo[Q2_AMMO_ROCKETS]  = 44;
    inv.ammo[Q2_AMMO_CELLS]    = 55;
    inv.ammo[Q2_AMMO_SLUGS]    = 66;

    for (id = 2; id <= 11; id++)
        CHECK(q2_sbar_ammo_for_weapon(&inv, id) == want[id - 2],
              "weapon %d reads %d, want %d", id,
              (int)q2_sbar_ammo_for_weapon(&inv, id), (int)want[id - 2]);

    /*
     * The two the 0-based indexing got wrong most visibly. The super shotgun
     * eats SHELLS, not bullets; the BFG eats CELLS and used to fall off the end
     * of `q2_weapon_ammo[]` (11 < Q2_WEAPON_COUNT is false) and read zero.
     */
    CHECK(q2_sbar_ammo_for_weapon(&inv, Q2_WID_SUPER_SHOTGUN) == 11,
          "the super shotgun reads the SHELL count");
    CHECK(q2_sbar_ammo_for_weapon(&inv, Q2_WID_BFG) == 55,
          "the BFG reads the CELL count and not zero");
    CHECK(q2_sbar_ammo_for_weapon(&inv, Q2_WID_RAILGUN) == 66,
          "the railgun reads slugs");

    /*
     * Id 0 is read, not refused: nothing between 0x800352E8 and 0x80035428
     * tests for zero, and byte 0 of 0x800ABEA8 is pool 0. So a +98 of zero
     * shows the SHELL count (beside the blank rect 0). This used to pin a
     * zero the console never produces.
     */
    CHECK(q2_sbar_ammo_for_weapon(&inv, 0) == 11,
          "id 0 reads pool 0, the shells (got %d)",
          (int)q2_sbar_ammo_for_weapon(&inv, 0));

    /* Past the table and null are silent zeroes, not reads past the copy. */
    CHECK(q2_sbar_ammo_for_weapon(&inv, 12) == 0, "id 12 is the phantom slot");
    CHECK(q2_sbar_ammo_for_weapon(NULL, 2) == 0, "no inventory reads zero");
}

/* ------------------------------------------------------------------------- */
/* The carousel — 0x80037ECC, through 0x80050758's two gates                  */
/* ------------------------------------------------------------------------- */
static u16 owned(int id) { return (u16)(1u << (id - 1)); }   /* 0x8009DC2C */

static void test_weapon_slots(void)
{
    q2_statusbar b;
    q2_icon_tables icons;
    q2_inventory inv;
    psx_ot ot;

    memset(&icons, 0, sizeof(icons));
    icons.rect_count = Q2_ICON_COUNT;
    icons.rect[0].u = 255; icons.rect[0].v = 255;
    icons.rect[0].w = 1;   icons.rect[0].h = 1;

    q2_statusbar_init(&b, &icons, 1);

    /*
     * (1) THE AMMO GATE. Blaster and shotgun owned, no shells: 0x800507C0
     * rejects the shotgun, the walk comes back to the blaster and 0x800507F0
     * returns ZERO. The port's old hand-rolled loop never looked at ammo, and
     * it did not offer the shotgun either: testing `1u << id` it read the
     * shotgun's bit (bit 1) as id 1, which it reached only through the k = 11
     * wrap, so it returned the BLASTER in both slots — {1, 1}.
     */
    memset(&inv, 0, sizeof(inv));
    inv.weapons = (u16)(owned(Q2_WID_BLASTER) | owned(Q2_WID_SHOTGUN));
    q2_statusbar_weapon_slots(&b, &inv, Q2_WID_BLASTER);
    CHECK(b.strip[0] == 0 && b.strip[1] == 0,
          "an unusable shotgun leaves both slots empty (%d, %d)",
          b.strip[0], b.strip[1]);

    /* (2) One shell is enough: `slt ammo, need` fails only on strictly less. */
    inv.ammo[Q2_AMMO_SHELLS] = 1;
    q2_statusbar_weapon_slots(&b, &inv, Q2_WID_BLASTER);
    CHECK(b.strip[0] == Q2_WID_SHOTGUN && b.strip[1] == Q2_WID_SHOTGUN,
          "exactly enough ammo passes, and both slots find the shotgun");

    /* (3) Only the blaster: nothing to walk to, both slots zero. */
    memset(&inv, 0, sizeof(inv));
    inv.weapons = owned(Q2_WID_BLASTER);
    q2_statusbar_weapon_slots(&b, &inv, Q2_WID_BLASTER);
    CHECK(b.strip[0] == 0 && b.strip[1] == 0,
          "a blaster-only player has an empty strip — which is why the retail "
          "capture's single icon is field 12's, not a strip slot's");

    /* (4) A railgun with no slugs is skipped in both directions. */
    memset(&inv, 0, sizeof(inv));
    inv.weapons = (u16)(owned(Q2_WID_BLASTER) | owned(Q2_WID_SHOTGUN) |
                        owned(Q2_WID_RAILGUN));
    inv.ammo[Q2_AMMO_SHELLS] = 10;
    q2_statusbar_weapon_slots(&b, &inv, Q2_WID_BLASTER);
    CHECK(b.strip[0] == Q2_WID_SHOTGUN && b.strip[1] == Q2_WID_SHOTGUN,
          "a dry railgun is never offered (%d, %d)", b.strip[0], b.strip[1]);

    /*
     * (5) THE OFF-BY-ONE PROBE. Blaster and machinegun: the owned bit for id N
     * is 1 << (N-1), so the machinegun is bit 3. The old loop tested
     * `1u << hi` with a 1-based id, and bit 3 is set, so it stopped at hi = 3:
     * it named the weapon one slot EARLIER than the one owned — the super
     * shotgun, 3 — in both directions ({3, 3}; the backward walk reaches 3 at
     * k = 9). The console gives {4, 4}.
     */
    memset(&inv, 0, sizeof(inv));
    inv.weapons = (u16)(owned(Q2_WID_BLASTER) | owned(Q2_WID_MACHINEGUN));
    inv.ammo[Q2_AMMO_BULLETS] = 50;
    q2_statusbar_weapon_slots(&b, &inv, Q2_WID_BLASTER);
    CHECK(b.strip[1] == Q2_WID_MACHINEGUN,
          "next from the blaster is 4, the machinegun (got %d)", b.strip[1]);
    CHECK(b.strip[0] == Q2_WID_MACHINEGUN,
          "and so is previous (got %d)", b.strip[0]);
    CHECK(b.strip[1] != Q2_WID_SUPER_SHOTGUN,
          "and NOT 3, the super shotgun, which `1u << hi` on a 1-based id "
          "picked");

    /* No inventory clears rather than leaves stale slots. */
    b.strip[0] = b.strip[1] = 9;
    q2_statusbar_weapon_slots(&b, NULL, Q2_WID_BLASTER);
    CHECK(b.strip[0] == 0 && b.strip[1] == 0, "no inventory empties the strip");

    /*
     * And the guard the emitter carries for case (2): prev == next draws ONE
     * icon, not two (0x80036198) — and it is SLOT B's, at 458, because the
     * guard skips slot A. An icon is two packets (the additive one and its
     * subtractive shadow), so count the additive ones.
     */
    icons.rect[Q2_WID_SHOTGUN].u = 32;
    icons.rect[Q2_WID_SHOTGUN].v = 0;
    icons.rect[Q2_WID_SHOTGUN].w = 32;
    icons.rect[Q2_WID_SHOTGUN].h = 24;
    if (psx_ot_init(&ot, 64, 256) != Q2_OK) {
        CHECK(0, "an ordering table for the strip");
        return;
    }
    q2_statusbar_anchor(&b, 93, 201);
    b.health = 100;
    b.weapon = Q2_WID_BLASTER;
    b.strip[0] = b.strip[1] = Q2_WID_SHOTGUN;
    psx_ot_clear(&ot);
    q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
    {
        u32 i, icons_drawn = 0;
        int at_458 = 0, at_388 = 0;

        for (i = 0; i < ot.prim_count; i++) {
            const psx_prim *p = &ot.prims[i];

            if (p->kind != PSX_PRIM_GT4 || !p->semi_transparent ||
                ((p->tpage >> 5) & 3) != PSX_BLEND_ADD)
                continue;
            icons_drawn++;
            at_458 += (p->xy[0].x == 458);
            at_388 += (p->xy[0].x == 388);
        }
        CHECK(icons_drawn == 1,
              "prev == next draws one strip icon, not two (%u)", icons_drawn);
        CHECK(at_458 == 1 && at_388 == 0,
              "and it is slot B's, at 458 — slot A yields (%d at 458, %d at "
              "388)", at_458, at_388);
    }
    psx_ot_free(&ot);
}

/* ------------------------------------------------------------------------- */
/* The carousel is a LATCH: six writers, and a shot is not one of them        */
/* ------------------------------------------------------------------------- */
/*
 * The u of the cell the additive strip packet at screen x `x` samples, or -1
 * when there is no such packet. The shadow packet copies the same UVs, so the
 * additive one is enough.
 */
static int strip_u_at(const psx_ot *ot, int x)
{
    u32 i;

    for (i = 0; i < ot->prim_count; i++) {
        const psx_prim *p = &ot->prims[i];

        if (p->kind == PSX_PRIM_GT4 && p->semi_transparent &&
            ((p->tpage >> 5) & 3) == PSX_BLEND_ADD && p->xy[0].x == x)
            return p->uv[0].u;
    }
    return -1;
}

static void test_weapon_slots_latch(void)
{
    q2_statusbar b;
    q2_icon_tables icons;
    q2_inventory inv;
    psx_ot ot;
    bool wrote;
    int f;

    memset(&icons, 0, sizeof(icons));
    icons.rect_count = Q2_ICON_COUNT;
    icons.rect[0].u = 255; icons.rect[0].v = 255;
    icons.rect[0].w = 1;   icons.rect[0].h = 1;
    /* Three weapon cells the draw can tell apart by u. */
    {
        static const int ids[] = { Q2_WID_BLASTER, Q2_WID_HYPERBLASTER,
                                   Q2_WID_BFG };
        static const u8 us[] = { 0, 64, 96 };
        int k;

        for (k = 0; k < 3; k++) {
            icons.rect[ids[k]].u = us[k];
            icons.rect[ids[k]].v = 0;
            icons.rect[ids[k]].w = 32;
            icons.rect[ids[k]].h = 24;
        }
    }

    q2_statusbar_init(&b, &icons, 1);
    q2_statusbar_anchor(&b, 93, 201);
    b.health = 100;

    /*
     * (1) Hyperblaster in hand, BFG and blaster owned, FIFTY cells — exactly
     * the BFG's one shot (0x8009DB4C[11] = 50). Nothing has written the pair
     * yet, which reads as the spawn the bar did not see, so the first call
     * writes it: next is the BFG, previous the blaster.
     */
    memset(&inv, 0, sizeof(inv));
    inv.weapons = (u16)(owned(Q2_WID_BLASTER) | owned(Q2_WID_HYPERBLASTER) |
                        owned(Q2_WID_BFG));
    inv.ammo[Q2_AMMO_CELLS] = 50;
    wrote = q2_statusbar_weapon_slots_track(&b, &inv, Q2_WID_HYPERBLASTER);
    CHECK(wrote, "the first call writes the pair");
    CHECK(b.strip[1] == Q2_WID_BFG && b.strip[0] == Q2_WID_BLASTER,
          "fifty cells: next is the BFG, previous the blaster (%d, %d)",
          b.strip[0], b.strip[1]);

    /*
     * (2) ONE SHOT, 49 cells. None of the six writers runs on a shot, so the
     * console's +100 still says BFG though the BFG can no longer fire — it
     * goes at the next event. Recomputing, as the client did every frame,
     * walks past the BFG to the blaster at once.
     */
    inv.ammo[Q2_AMMO_CELLS] = 49;
    wrote = q2_statusbar_weapon_slots_track(&b, &inv, Q2_WID_HYPERBLASTER);
    CHECK(!wrote, "a shot is not an event");
    CHECK(b.strip[1] == Q2_WID_BFG,
          "the BFG keeps slot B at 49 cells (got %d)", b.strip[1]);

    /* And it is what the bar DRAWS at 458: the BFG's cell, the blaster at
     * 388 beside it. */
    b.weapon = Q2_WID_HYPERBLASTER;
    if (psx_ot_init(&ot, 64, 256) != Q2_OK) {
        CHECK(0, "an ordering table for the strip");
        return;
    }
    psx_ot_clear(&ot);
    q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
    CHECK(strip_u_at(&ot, 458) == icons.rect[Q2_WID_BFG].u,
          "slot B at 458 samples the BFG's cell (u %d)", strip_u_at(&ot, 458));
    CHECK(strip_u_at(&ot, 388) == icons.rect[Q2_WID_BLASTER].u,
          "slot A at 388 samples the blaster's (u %d)", strip_u_at(&ot, 388));
    psx_ot_free(&ot);

    /* (3) Down to ten over many frames: still nothing writes. */
    for (f = 48; f >= 10; f--) {
        inv.ammo[Q2_AMMO_CELLS] = (s16)f;
        if (q2_statusbar_weapon_slots_track(&b, &inv, Q2_WID_HYPERBLASTER))
            break;
    }
    CHECK(f < 10 && b.strip[1] == Q2_WID_BFG,
          "no frame of the shooting writes (stopped at %d cells, slot B %d)",
          f, b.strip[1]);

    /*
     * (4) AN AMMO PICKUP IS AN EVENT — 0x80037ECC, after 0x80037DFC has
     * stored the pool. Ten cells to thirty-five is still below the fifty the
     * last write saw: the rise is measured against the previous call, which is
     * what lets it through.
     */
    inv.ammo[Q2_AMMO_CELLS] = 35;
    wrote = q2_statusbar_weapon_slots_track(&b, &inv, Q2_WID_HYPERBLASTER);
    CHECK(wrote, "a pool that rises is a pickup, and writes");
    CHECK(b.strip[1] == Q2_WID_BLASTER && b.strip[0] == Q2_WID_BLASTER,
          "and at 35 cells the BFG is walked past at last (%d, %d)",
          b.strip[0], b.strip[1]);

    /*
     * (5) A SELECT IS AN EVENT — the id the pair was walked from changed.
     * From the blaster, both walks find the hyperblaster; the BFG at 35 is
     * skipped both ways.
     */
    wrote = q2_statusbar_weapon_slots_track(&b, &inv, Q2_WID_BLASTER);
    CHECK(wrote, "a new selected weapon writes");
    CHECK(b.strip[0] == Q2_WID_HYPERBLASTER &&
          b.strip[1] == Q2_WID_HYPERBLASTER,
          "walked from the blaster: the hyperblaster both ways (%d, %d)",
          b.strip[0], b.strip[1]);

    /*
     * (6) A WEAPON PICKUP IS AN EVENT — the owned set changed (0x80037E7C).
     * Slugs first, from a box, with no railgun: that writes too, and changes
     * nothing. Then the railgun bit alone, with the pools untouched.
     */
    inv.ammo[Q2_AMMO_SLUGS] = 10;
    wrote = q2_statusbar_weapon_slots_track(&b, &inv, Q2_WID_BLASTER);
    CHECK(wrote && b.strip[0] == Q2_WID_HYPERBLASTER,
          "a slug box writes, and with no railgun the pair stands");
    inv.weapons = (u16)(inv.weapons | owned(Q2_WID_RAILGUN));
    wrote = q2_statusbar_weapon_slots_track(&b, &inv, Q2_WID_BLASTER);
    CHECK(wrote, "a new owned bit writes");
    CHECK(b.strip[0] == Q2_WID_RAILGUN && b.strip[1] == Q2_WID_HYPERBLASTER,
          "and the railgun is previous, the hyperblaster next (%d, %d)",
          b.strip[0], b.strip[1]);

    /* (7) Nothing new, nothing written. */
    CHECK(!q2_statusbar_weapon_slots_track(&b, &inv, Q2_WID_BLASTER),
          "a frame with no event keeps the pair");

    /*
     * (8) The caller's own writes — the select, the auto-select, the spawn —
     * go through q2_statusbar_weapon_slots, and the latch takes its baseline
     * from them: the next frame over the same state is not an event.
     */
    inv.ammo[Q2_AMMO_CELLS] = 60;
    q2_statusbar_weapon_slots(&b, &inv, Q2_WID_BLASTER);
    CHECK(!q2_statusbar_weapon_slots_track(&b, &inv, Q2_WID_BLASTER),
          "a direct write is the baseline for the next frame");

    /* (9) No inventory empties the slots and forgets the baseline, so the
     * next inventory writes. */
    CHECK(q2_statusbar_weapon_slots_track(&b, NULL, Q2_WID_BLASTER) &&
          b.strip[0] == 0 && b.strip[1] == 0,
          "no inventory empties the strip");
    CHECK(q2_statusbar_weapon_slots_track(&b, &inv, Q2_WID_BLASTER),
          "and the next inventory writes");
}

/* ------------------------------------------------------------------------- */
/* The armour field's five-way select and the state machine in front of it    */
/* ------------------------------------------------------------------------- */
static void test_armour_icon_select(void)
{
    q2_statusbar b;

    /* The select alone — 0x8003564C / 0x8003576C / 0x80035800 / 0x80035894. */
    CHECK(q2_sbar_armour_icon(Q2_INV_ARMOUR_BODY, false) ==
          Q2_SBAR_ICON_ARMOUR_BODY, "body armour draws rect 26");
    CHECK(q2_sbar_armour_icon(Q2_INV_ARMOUR_COMBAT, false) ==
          Q2_SBAR_ICON_ARMOUR_COMBAT, "combat armour draws rect 27");
    CHECK(q2_sbar_armour_icon(Q2_INV_ARMOUR_JACKET, false) ==
          Q2_SBAR_ICON_ARMOUR_JACKET, "jacket armour draws rect 28");
    CHECK(q2_sbar_armour_icon(0, false) == Q2_SBAR_ICON_NONE,
          "no armour draws the blank");
    CHECK(q2_sbar_armour_icon(Q2_INV_POWER_SHIELD, true) ==
          Q2_SBAR_ICON_POWER_SHIELD, "the power state draws rect 30");

    /*
     * BODY BEFORE COMBAT BEFORE JACKET. The pickup handlers only raise the bit
     * for their own class when no stronger bit is up, so a player who has worn
     * body armour keeps 0x4000 while wearing combat — and testing the weak bit
     * first would draw the grey vest for a player in red.
     */
    CHECK(q2_sbar_armour_icon(Q2_INV_ARMOUR_BODY | Q2_INV_ARMOUR_JACKET,
                              false) == Q2_SBAR_ICON_ARMOUR_BODY,
          "body wins over jacket when both bits are up");

    /* The power shield does NOT displace the vest by itself: only the state
     * machine's own flag selects the power arm (0x80035634). */
    CHECK(q2_sbar_armour_icon(Q2_INV_POWER_SHIELD | Q2_INV_ARMOUR_COMBAT,
                              false) == Q2_SBAR_ICON_ARMOUR_COMBAT,
          "holding a shield while not in the power state still draws the vest");

    /* --- the state machine, 0x80035594 onward ---------------------------- */
    q2_statusbar_init(&b, NULL, 1);

    /* No cells: the power arm cannot be entered at all (0x80035630). */
    b.cells = 0;
    b.ticks = 0;
    q2_statusbar_armour_state(&b, Q2_INV_POWER_SHIELD | Q2_INV_ARMOUR_BODY);
    CHECK(!b.showing_power && b.armour_icon == Q2_SBAR_ICON_ARMOUR_BODY,
          "an empty shield falls back to the vest");

    /* Cells and a power item but NO vest: pinned to the power arm. */
    b.cells = 50;
    q2_statusbar_armour_state(&b, Q2_INV_POWER_SHIELD);
    CHECK(b.showing_power && b.armour_icon == Q2_SBAR_ICON_POWER_SHIELD,
          "a shield with no vest pins the power readout");

    /* Both: alternates, and only once the 300-tick deadline passes. */
    b.showing_power   = false;
    b.power_toggle_at = 0;
    b.ticks           = 1;
    q2_statusbar_armour_state(&b, Q2_INV_POWER_SHIELD | Q2_INV_ARMOUR_BODY);
    CHECK(b.showing_power, "with both held the field flips to the shield");
    b.ticks = 1 + Q2_SBAR_POWER_ALTERNATE - 1;
    q2_statusbar_armour_state(&b, Q2_INV_POWER_SHIELD | Q2_INV_ARMOUR_BODY);
    CHECK(b.showing_power, "and holds it for the whole 300 ticks");
    b.ticks = 1 + Q2_SBAR_POWER_ALTERNATE + 1;
    q2_statusbar_armour_state(&b, Q2_INV_POWER_SHIELD | Q2_INV_ARMOUR_BODY);
    CHECK(!b.showing_power && b.armour_icon == Q2_SBAR_ICON_ARMOUR_BODY,
          "then flips back to the vest");
}

/*
 * The stale class bit. Body armour shot down to zero and then a shard picked
 * up leaves the player in JACKET armour, and the bar must say so — 0x80035580
 * clears the class bits every frame the armour reads zero.
 */
static void test_armour_class_upkeep(void)
{
    q2_inventory inv;

    q2_inventory_init(&inv);
    inv.flags  |= Q2_INV_ARMOUR_BODY;
    inv.armour  = 40;

    q2_inventory_armour_upkeep(&inv);
    CHECK((inv.flags & Q2_INV_ARMOUR_BODY) != 0,
          "armour still worn keeps its class bit");

    inv.armour = 0;
    q2_inventory_armour_upkeep(&inv);
    CHECK((inv.flags & Q2_INV_ARMOUR_MASK) == 0,
          "armour driven to zero drops every class bit");

    /* And nothing else in the word: 0x00078FFF keeps the keys and the three
     * bits above the classes. */
    q2_inventory_init(&inv);
    inv.flags  = Q2_KEY_BLUE | Q2_INV_ARMOUR_BODY | Q2_INV_POWER_SHIELD |
                 Q2_INV_MEGA_HEALTH;
    inv.armour = 0;
    q2_inventory_armour_upkeep(&inv);
    CHECK(inv.flags == (u32)(Q2_KEY_BLUE | Q2_INV_POWER_SHIELD |
                             Q2_INV_MEGA_HEALTH),
          "the clear takes the classes and nothing else");

    /* The selector then picks the shard's jacket rather than the dead body
     * bit — which is the wrong-icon bug this upkeep exists to prevent. */
    inv.flags |= Q2_INV_ARMOUR_JACKET;
    CHECK(q2_sbar_armour_icon(inv.flags, false) == Q2_SBAR_ICON_ARMOUR_JACKET,
          "a shard after losing body armour draws the grey vest");
}

/* The fifth sub-draw, 0x80035B38: the three fields at the upper right are not
 * part of the pickup caption. They are a first-live-deadline selector over the
 * four powerup expiry words, with a strict unsigned expiry edge. */
static void test_powerup_timer(void)
{
    q2_inventory inv;
    q2_statusbar b;
    q2_icon_tables icons;
    psx_ot ot;
    u32 with, without;

    q2_inventory_init(&inv);
    q2_statusbar_init(&b, NULL, 1);
    b.ticks = 1200;

    q2_statusbar_powerup_state(&b, &inv);
    CHECK(b.powerup_icon == Q2_SBAR_ICON_NONE && b.powerup_seconds == 0,
          "no live deadline leaves the upper-right timer blank");

    /* Equality is expired: the retail code is `sltu now, deadline`, rather
     * than a signed comparison or a <= test. */
    inv.quad_until = (s32)b.ticks;
    q2_statusbar_powerup_state(&b, &inv);
    CHECK(b.powerup_icon == Q2_SBAR_ICON_NONE,
          "the deadline tick itself is no longer active");

    inv.quad_until     = (s32)(b.ticks + 30 * Q2_SBAR_POWERUP_SECONDS_TICKS);
    inv.invuln_until   = (s32)(b.ticks + 12 * Q2_SBAR_POWERUP_SECONDS_TICKS);
    inv.enviro_until   = (s32)(b.ticks + 8 * Q2_SBAR_POWERUP_SECONDS_TICKS);
    inv.breather_until = (s32)(b.ticks + 5 * Q2_SBAR_POWERUP_SECONDS_TICKS);
    q2_statusbar_powerup_state(&b, &inv);
    CHECK(b.powerup_icon == Q2_SBAR_ICON_POWERUP_QUAD &&
          b.powerup_seconds == 30,
          "quad wins even when another live timer has less time left");

    inv.quad_until = (s32)b.ticks;
    q2_statusbar_powerup_state(&b, &inv);
    CHECK(b.powerup_icon == Q2_SBAR_ICON_POWERUP_INVULN &&
          b.powerup_seconds == 12,
          "the walk advances to invulnerability once quad expires");

    inv.invuln_until = (s32)(b.ticks + 1);
    inv.enviro_until = (s32)b.ticks;
    inv.breather_until = (s32)b.ticks;
    q2_statusbar_powerup_state(&b, &inv);
    CHECK(b.powerup_icon == Q2_SBAR_ICON_POWERUP_INVULN &&
          b.powerup_seconds == 0,
          "a fraction of a second is active and draws zero after flooring");

    memset(&icons, 0, sizeof(icons));
    icons.rect_count = Q2_ICON_COUNT;
    icons.rect[0].u = 255; icons.rect[0].v = 255;
    icons.rect[0].w = 1;   icons.rect[0].h = 1;
    icons.rect[Q2_SBAR_ICON_POWERUP_QUAD].u = 64;
    icons.rect[Q2_SBAR_ICON_POWERUP_QUAD].v = 96;
    icons.rect[Q2_SBAR_ICON_POWERUP_QUAD].w = 32;
    icons.rect[Q2_SBAR_ICON_POWERUP_QUAD].h = 24;
    icons.rect[Q2_SBAR_ICON_POWERUP_QUAD].id = 8;

    if (psx_ot_init(&ot, 64, 256) != Q2_OK) {
        CHECK(0, "an ordering table for the powerup timer");
        return;
    }

    q2_statusbar_init(&b, &icons, 1);
    q2_statusbar_anchor(&b, 93, 201);
    b.weapon = Q2_SBAR_WEAPON_NO_AMMO;
    /* Alive, or the 0x80033C68 gate suppresses the timer along with everything
     * else below it — see test_dead_strips_the_bar. */
    b.health = 100;
    psx_ot_clear(&ot);
    without = q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);

    b.powerup_icon = Q2_SBAR_ICON_POWERUP_QUAD;
    b.powerup_seconds = 30;
    psx_ot_clear(&ot);
    with = q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
    CHECK(with == without + 3,
          "a two-digit powerup timer emits its icon and two numerals (%u vs %u)",
          with, without);

    psx_ot_free(&ot);
}

/*
 * The pickup caption's icon — field 16, filled by the fourth sub-draw at
 * `0x800359C0`.
 *
 * The thing to pin is which NUMBER selects it. The sub-draw does
 * `index * 5 + 0x8009C478` on `client+84`, and `client+84` is where the touch
 * dispatch stores the EFFECT (`sb s7, 84(s1)` at 0x800372F0) — so for an item
 * the effect id and the icon rect index are one number, and the same number
 * indexes the 57-name table the caption's `%s` comes from. Reading it as
 * anything else puts somebody else's icon beside the right word.
 */
static void test_pickup_icon(void)
{
    q2_icon_tables icons;
    q2_statusbar b;
    psx_ot ot;
    u32 with, without;

    /*
     * Built by hand rather than loaded: the rect table lives in the executable
     * and the rest of this file is disc-free. Rect 0 is the 1x1 blank the real
     * table opens with and rect 18 is a 32x24 cell, which is all the emit
     * cares about.
     */
    memset(&icons, 0, sizeof(icons));
    icons.rect_count = Q2_ICON_COUNT;
    icons.rect[0].u = 255; icons.rect[0].v = 255;
    icons.rect[0].w = 1;   icons.rect[0].h = 1;
    icons.rect[18].u = 64; icons.rect[18].v = 48;
    icons.rect[18].w = 32; icons.rect[18].h = 24;
    icons.rect[18].id = 8;

    if (psx_ot_init(&ot, 64, 256) != Q2_OK) {
        CHECK(0, "an ordering table for the emit");
        return;
    }

    q2_statusbar_init(&b, &icons, 1);
    q2_statusbar_anchor(&b, 93, 201);
    /* ALIVE. This used to be health 0 to keep the counters quiet; the
     * 0x80033C68 death gate now suppresses the pickup icon itself at zero, so
     * the health digits are simply present in both counts instead. */
    b.health = 100;
    b.armour = 0;            /* armour at zero draws nothing (0x80035994) */
    b.ammo   = 0;
    b.weapon = Q2_SBAR_WEAPON_NO_AMMO;

    psx_ot_clear(&ot);
    b.pickup_icon = 0;
    without = q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);

    psx_ot_clear(&ot);
    b.pickup_icon = 18;      /* Shells, and rect 18 is the shells box */
    with = q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);

    CHECK(with == without + 1,
          "a pickup icon adds exactly one sprite (%u vs %u)", with, without);

    /* Rect 18 is a real cell rather than the 1x1 blank, which is what makes
     * the count above mean anything. */
    {
        const q2_icon_rect *r = q2_icon_rect_get(&icons, 18);
        CHECK(r && !(r->w == 1 && r->h == 1),
              "rect 18 is a real cell, not the blank");
    }

    /* And it is the UPPER-LEFT field, not one of the counters'. */
    CHECK(q2_sbar_fields[Q2_SBAR_FIELD_UP_LEFT].dx == -71 &&
          q2_sbar_fields[Q2_SBAR_FIELD_UP_LEFT].dy == -25,
          "field 16 sits at the anchor less 71, 25 above");

    psx_ot_free(&ot);
}

/* ------------------------------------------------------------------------- */
/* A one-player bar with real cells, shared by the emit-level tests below.    */
/* ------------------------------------------------------------------------- */
#define BAR_ANCHOR_X 93
#define BAR_ANCHOR_Y 201

static void real_cell(q2_icon_tables *t, int index, u8 u, u8 v, u8 id)
{
    t->rect[index].u  = u;
    t->rect[index].v  = v;
    t->rect[index].w  = 32;
    t->rect[index].h  = 24;
    t->rect[index].id = id;
}

static void build_icons(q2_icon_tables *t)
{
    memset(t, 0, sizeof(*t));
    t->rect_count = Q2_ICON_COUNT;
    t->rect[0].u = 255; t->rect[0].v = 255;
    t->rect[0].w = 1;   t->rect[0].h = 1;
    real_cell(t, 2, 32, 0, 3);         /* the shotgun's cell            */
    real_cell(t, 4, 96, 0, 3);         /* the machinegun's             */
    real_cell(t, 34, 0, 24, 4);        /* the health cross, rect 34    */
    real_cell(t, 18, 64, 48, 8);       /* a shell box                  */
}

/* How many cells landed at exactly this x, and where the first one is. */
static const psx_prim *cell_at(const psx_ot *ot, int x, int *count)
{
    const psx_prim *found = NULL;
    u32 i;

    *count = 0;
    for (i = 0; i < ot->prim_count; i++) {
        if (ot->prims[i].xy[0].x != (s16)x)
            continue;
        if (!found)
            found = &ot->prims[i];
        (*count)++;
    }
    return found;
}

/* ------------------------------------------------------------------------- */
/* Field 12 is the WEAPON IN HAND — 0x80033C50 -> 0x80037CAC                  */
/* ------------------------------------------------------------------------- */
static void test_weapon_icon_field(void)
{
    q2_icon_tables icons;
    q2_statusbar b;
    psx_ot ot;
    u32 without, with;
    const psx_prim *p;
    int n;

    build_icons(&icons);
    if (psx_ot_init(&ot, 64, 256) != Q2_OK) {
        CHECK(0, "an ordering table for field 12");
        return;
    }

    q2_statusbar_init(&b, &icons, 1);
    q2_statusbar_anchor(&b, BAR_ANCHOR_X, BAR_ANCHOR_Y);
    b.health = 100;
    b.health_icon = 34;

    b.weapon = 0;
    psx_ot_clear(&ot);
    without = q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
    CHECK(cell_at(&ot, BAR_ANCHOR_X + 330, &n) == NULL && n == 0,
          "no weapon leaves field 12 blank");

    b.weapon = 2;                       /* the shotgun, rect 2 */
    psx_ot_clear(&ot);
    with = q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
    CHECK(with == without + 1,
          "the weapon icon adds exactly one sprite (%u vs %u)", with, without);

    p = cell_at(&ot, BAR_ANCHOR_X + 330, &n);
    CHECK(n == 1, "exactly one cell at anchor + 330 (%d)", n);
    CHECK(p != NULL && p->xy[0].y == BAR_ANCHOR_Y,
          "field 12 has NO dy — 0x80033A64 stores view+306 unmodified");
    if (p) {
        CHECK(p->xy[1].x - p->xy[0].x == 32 && p->xy[3].y - p->xy[0].y == 24,
              "and is drawn at the rect's own 32 x 24 in one-player");
        CHECK(p->uv[0].u == icons.rect[2].u && p->uv[0].v == icons.rect[2].v,
              "from rect[weapon] — 0x80037CF0 uses the weapon id as the index");
    }
    /* 423 with the (93, 201) anchor, which is what "+330" means on screen. */
    CHECK(BAR_ANCHOR_X + q2_sbar_fields[Q2_SBAR_FIELD_AUX_ICON].dx == 423,
          "field 12 lands at column 423");

    /*
     * THE WHOLE CAROUSEL, pinned as one geometry: 388..420, 423..455, 458..490
     * tile the row without overlapping. Any future change to one of the three
     * trips this.
     */
    CHECK(q2_sbar_strip[0].x + 32 < BAR_ANCHOR_X +
              q2_sbar_fields[Q2_SBAR_FIELD_AUX_ICON].dx,
          "the previous-weapon cell ends before field 12 begins");
    CHECK(BAR_ANCHOR_X + q2_sbar_fields[Q2_SBAR_FIELD_AUX_ICON].dx + 32 <
              q2_sbar_strip[1].x,
          "field 12 ends before the next-weapon cell begins");

    psx_ot_free(&ot);
}

/* ------------------------------------------------------------------------- */
/* The strip: LEFT edges, the rect's own size, and the gouraud fade           */
/* ------------------------------------------------------------------------- */

/* The semi-transparent GT4 starting at x whose tpage carries blend `mode`. */
static const psx_prim *strip_packet(const psx_ot *ot, int x, psx_blend mode)
{
    u32 i;

    for (i = 0; i < ot->prim_count; i++) {
        const psx_prim *p = &ot->prims[i];

        if (p->kind == PSX_PRIM_GT4 && p->semi_transparent &&
            p->xy[0].x == (s16)x && (psx_blend)((p->tpage >> 5) & 3) == mode)
            return p;
    }
    return NULL;
}

/* Which of a slot's two packets the ordering table hands the rasteriser
 * first, slot A (x 388) in [0] and slot B (x 458) in [1]. */
typedef struct strip_order {
    int count;
    bool seen[2];
    psx_blend first_blend[2];
} strip_order;

static void strip_order_visit(const psx_prim *p, void *user)
{
    strip_order *o = (strip_order *)user;
    int slot;

    if (p->kind != PSX_PRIM_GT4)
        return;
    slot = (p->xy[0].x == 388) ? 0 : (p->xy[0].x == 458) ? 1 : -1;
    if (slot < 0)
        return;
    o->count++;
    if (!o->seen[slot]) {
        o->seen[slot] = true;
        o->first_blend[slot] = (psx_blend)((p->tpage >> 5) & 3);
    }
}

static void test_strip_geometry(void)
{
    static q2_hud_tables pal;
    q2_icon_tables icons;
    q2_statusbar b;
    psx_ot ot;
    const psx_prim *a, *as, *bb, *bs;
    int n;

    /* The table itself, before anything is drawn with it. */
    CHECK(q2_sbar_strip[0].x == 388 && q2_sbar_strip[0].y == 201 &&
          q2_sbar_strip[1].x == 458 && q2_sbar_strip[1].y == 201,
          "0x8009C658's first pair is (388,201)/(458,201)");
    CHECK(q2_sbar_strip_2p[0].x == 381 && q2_sbar_strip_2p[0].y == 95 &&
          q2_sbar_strip_2p[1].x == 419 && q2_sbar_strip_2p[1].y == 95,
          "and its second is (381,95)/(419,95) — parsed, and dead data: only "
          "0x80033CFC passes a3 = 1");

    build_icons(&icons);
    if (psx_ot_init(&ot, 64, 256) != Q2_OK) {
        CHECK(0, "an ordering table for the strip");
        return;
    }

    /* A palette bank, so the icon's CLUT and the shadow's can be told apart:
     * both strip cells here carry palette 3 in their fifth byte. */
    memset(&pal, 0, sizeof(pal));
    pal.palette_count = Q2_HUD_PALETTE_MAX;
    pal.palette[3].present = true;
    pal.palette[3].clut_id = 0x0033;
    pal.palette[Q2_SBAR_PAL_STRIP_SHADOW].present = true;
    pal.palette[Q2_SBAR_PAL_STRIP_SHADOW].clut_id = 0x0073;

    q2_statusbar_init(&b, &icons, 1);
    q2_statusbar_set_palettes(&b, &pal);
    q2_statusbar_anchor(&b, BAR_ANCHOR_X, BAR_ANCHOR_Y);
    b.health = 100;
    b.health_icon = 34;
    b.weapon = 0;                 /* keep field 12 out of the way */
    b.strip[0] = 2;
    b.strip[1] = 4;
    psx_ot_clear(&ot);
    q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);

    /*
     * LEFT EDGES. Drawn with the old `- is.w` these would be at 356 and 426.
     * 0x80033448..0x80033458 build vertex 1 as `table.x + rect.w`, so the table
     * value is where the sprite starts. Each slot is TWO packets at the same
     * place — the icon and its subtractive shadow (0x8003368C onward).
     */
    a = strip_packet(&ot, 388, PSX_BLEND_ADD);
    as = strip_packet(&ot, 388, PSX_BLEND_SUB);
    CHECK(cell_at(&ot, 388, &n) != NULL && n == 2 && a && as,
          "slot A is an icon and its shadow, both starting at x = 388 (%d)", n);
    bb = strip_packet(&ot, 458, PSX_BLEND_ADD);
    bs = strip_packet(&ot, 458, PSX_BLEND_SUB);
    CHECK(cell_at(&ot, 458, &n) != NULL && n == 2 && bb && bs,
          "slot B likewise at x = 458 (%d)", n);
    CHECK(cell_at(&ot, 356, &n) == NULL,
          "and nothing at 356, where subtracting a width used to put it");
    CHECK(cell_at(&ot, 426, &n) == NULL, "nor at 426");

    if (a) {
        CHECK(a->xy[0].y == 201, "slot A sits on the counters' own row");
        CHECK(a->xy[1].x - a->xy[0].x == 32 && a->xy[3].y - a->xy[0].y == 24,
              "at the rect's own size, with no clamp (0x8003622C/0x80036230)");
        CHECK(a->kind == PSX_PRIM_GT4 && a->textured_blend,
              "slot A is a POLY_GT4 — 0x80033374's code byte 0x3C, modulated");
        CHECK(a->clut == 0x0033,
              "and the icon takes its own rect's palette (%04X)", a->clut);
        /*
         * 0x800334B4 onward: slot A (PREVIOUS, the left cell) is 0 on its
         * LEFT edge and 128 on the edge facing the centre. In this backend's
         * perimeter order those two corners are rgb[0] (TL) and rgb[3] (BL).
         */
        CHECK(a->rgb[0].r == 0 && a->rgb[3].r == 0,
              "slot A's left edge is 0 (%d, %d)",
              a->rgb[0].r, a->rgb[3].r);
        CHECK(a->rgb[1].r == 128 && a->rgb[2].r == 128,
              "and its right edge is 128 (%d, %d)",
              a->rgb[1].r, a->rgb[2].r);
        CHECK(a->rgb[0].r != Q2_SBAR_MOD && a->rgb[1].r != Q2_SBAR_MOD,
              "the strip does NOT follow the fields' 192");
    }
    if (as) {
        /* The copy: same quad, same UVs, bright edge 62 (0x800336CC), the
         * CLUT at 0x800E3FBE = palette 73, and ABR 2 (0x80033798). */
        CHECK(as->xy[0].y == 201 && as->xy[2].x == 420 && as->xy[2].y == 225,
              "the shadow covers exactly the icon's quad");
        CHECK(a && as->uv[0].u == a->uv[0].u && as->uv[2].v == a->uv[2].v,
              "and samples the same texels");
        CHECK(as->rgb[0].r == 0 && as->rgb[3].r == 0 &&
              as->rgb[1].r == 62 && as->rgb[2].r == 62,
              "slot A's shadow runs 0 to 62 on the same side (%d..%d)",
              as->rgb[0].r, as->rgb[1].r);
        CHECK(as->clut == 0x0073,
              "through palette 73, not the icon's (%04X)", as->clut);
    }
    if (bb) {
        /* 0x80033568: the exact mirror. */
        CHECK(bb->rgb[0].r == 128 && bb->rgb[3].r == 128,
              "slot B's left edge — the one facing the centre — is 128");
        CHECK(bb->rgb[1].r == 0 && bb->rgb[2].r == 0,
              "and its right, outer, edge is 0");
    }
    if (bs) {
        /* 0x8003372C: the copy's mirror writes 62 into v0 and v2. */
        CHECK(bs->rgb[0].r == 62 && bs->rgb[3].r == 62 &&
              bs->rgb[1].r == 0 && bs->rgb[2].r == 0,
              "slot B's shadow is 62 facing the centre and 0 outside");
    }

    /*
     * THE DRAW ORDER. Both packets go to one ordering-table entry and AddPrim
     * prepends (0x800B0EE0), so the shadow — linked second — is DRAWN FIRST
     * and the icon lands on top of the darkened patch. Walk the table the way
     * the rasteriser does and see which one of each pair comes out first.
     */
    {
        strip_order order;

        memset(&order, 0, sizeof(order));
        psx_ot_walk(&ot, strip_order_visit, &order);
        CHECK(order.count == 4, "four strip packets drawn (%d)", order.count);
        CHECK(order.first_blend[0] == PSX_BLEND_SUB &&
              order.first_blend[1] == PSX_BLEND_SUB,
              "and in each slot the shadow is drawn before the icon");
    }

    q2_statusbar_set_palettes(&b, NULL);
    psx_ot_free(&ot);
}

/* ------------------------------------------------------------------------- */
/* Every field sprite is a FLAT quad modulated at 192 — 0x80035EA0            */
/* ------------------------------------------------------------------------- */
static void test_field_modulation(void)
{
    q2_icon_tables icons;
    q2_statusbar b;
    psx_ot ot;
    u32 i, fields = 0, strips = 0;

    CHECK(Q2_SBAR_MOD == 192,
          "0x80035F54 loads 192 into s4 and 0x800360E0/F4/104 store it");
    CHECK(Q2_SBAR_STRIP_MOD == 128 && Q2_SBAR_STRIP_FADE == 0,
          "the strip's own two constants are 0x80033510's 128 and 0x800334B4's 0");
    CHECK(Q2_SBAR_STRIP_SHADOW == 62 && Q2_SBAR_PAL_STRIP_SHADOW == 73,
          "and its shadow's are 0x800336CC's 62 and 0x800E3FBE's palette 73");
    /* Whether the 192 actually BRIGHTENS is asserted on pixels, in
     * test_bar_on_screen, rather than on this constant. */

    build_icons(&icons);
    if (psx_ot_init(&ot, 64, 256) != Q2_OK) {
        CHECK(0, "an ordering table for the modulation check");
        return;
    }

    q2_statusbar_init(&b, &icons, 1);
    q2_statusbar_anchor(&b, BAR_ANCHOR_X, BAR_ANCHOR_Y);
    b.health = 100;
    b.health_icon = 34;
    b.weapon = 2;
    b.strip[0] = 2;
    b.strip[1] = 4;
    psx_ot_clear(&ot);
    q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);

    for (i = 0; i < ot.prim_count; i++) {
        const psx_prim *p = &ot.prims[i];

        if (p->kind == PSX_PRIM_GT4) {
            strips++;
            continue;
        }
        fields++;
        CHECK(p->kind == PSX_PRIM_FT4,
              "a field cell is a POLY_FT4 — length 9 at 0x80035F9C, code 0x2C");
        CHECK(p->textured_blend,
              "code 0x2C blends the texture; 0x2D would be raw");
        CHECK(!p->semi_transparent,
              "and has ABE clear — the fields are opaque, only the strip blends");
        CHECK(p->rgb[0].r == 192 && p->rgb[0].g == 192 && p->rgb[0].b == 192,
              "and carries 192 in all three colour bytes");
    }
    CHECK(fields > 0, "some field cells were emitted at all");
    CHECK(strips == 4,
          "and the strip's two icons, two packets each, are the only gouraud "
          "ones (%u)", strips);

    psx_ot_free(&ot);
}

/* ------------------------------------------------------------------------- */
/* The bar DRAWN — the brightening and the fade, on pixels                    */
/* ------------------------------------------------------------------------- */
/*
 * The two things above that are only meaningful once rasterised: that 192 on
 * a blended packet is a real 1.5x, and that the strip fades into the WORLD
 * rather than into black. Asserted on a framebuffer, through raster.c, so a
 * constant that is right but never reaches the pixels still fails.
 *
 * The sheet is synthetic: texture page 1 (the tpage the other tests pass) is
 * filled with texel index 1 everywhere and CLUT (0,0) — clut word 0, the
 * fallback every sprite takes with no palette bank — maps index 1 to grey 64
 * with STP set, as the real blended-strip palette requires.
 * The backdrop is grey 96. Every value is a multiple of 8, so the 5-bit
 * framebuffer holds each exactly, and dithering is off so a pixel is a number.
 */
#define SCREEN_W 512
#define SCREEN_H 248
#define TEXEL_GREY 64
#define BACK_GREY  96

static int grey_at(const psx_framebuffer *fb, int x, int y)
{
    return (fb->px[y * fb->width + x] & 0x1F) << 3;   /* red channel */
}

/* Multiplayer sheets keep the cell origins but author smaller sprites in
 * them. A coloured last row/column makes sampling the surrounding padding
 * observable: scaling the single-player cell loses those border texels. */
static void paint_test_cell(psx_vram *vram, int u, int v, int w, int h)
{
    int x, y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int index = y == h - 1 ? 3 : (x == w - 1 ? 2 : 1);
            int shift = ((u + x) & 3) * 4;
            u16 *word = &vram->px[v + y][64 + (u + x) / 4];
            *word = (u16)((*word & ~(0xFu << shift)) | (index << shift));
        }
    }
}

static void test_split_sprite_texels(void)
{
    static const struct { q2_sbar_layout layout; int players; } cases[] = {
        { Q2_SBAR_LAYOUT_TWO_H, 2 }, { Q2_SBAR_LAYOUT_TWO_V, 2 },
        { Q2_SBAR_LAYOUT_QUAD, 3 }, { Q2_SBAR_LAYOUT_QUAD, 4 }
    };
    psx_vram *vram = (psx_vram *)calloc(1, sizeof(*vram));
    psx_framebuffer fb;
    psx_ot ot;
    psx_raster_opts opts;
    unsigned c;
    int height, view;

    if (!vram) { CHECK(0, "VRAM for split-screen sprite test"); return; }
    if (psx_ot_init(&ot, 64, 256) != Q2_OK) {
        CHECK(0, "ordering table for split-screen sprite test");
        free(vram);
        return;
    }
    if (psx_fb_init(&fb, SCREEN_W, SCREEN_H) != Q2_OK) {
        CHECK(0, "framebuffer for split-screen sprite test");
        psx_ot_free(&ot);
        free(vram);
        return;
    }
    psx_raster_opts_default(&opts);
    opts.dither = false;

    for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        int dw = cases[c].players == 2 ? 18 : 13;
        int dh = cases[c].players == 2 ? 20 : 12;
        int iw = cases[c].players == 2 ? 24 : 16;
        int ih = cases[c].players == 2 ? 18 : 12;

        for (height = 240; height <= 248; height += 8) {
            for (view = 0; view < cases[c].players; view++) {
                q2_icon_tables icons;
                q2_statusbar b;
                u32 i;
                int painted = 0;

                memset(vram, 0, sizeof(*vram));
                vram->px[0][1] = psx_rgb555(64, 64, 64);
                vram->px[0][2] = psx_rgb555(64, 0, 0);
                vram->px[0][3] = psx_rgb555(0, 64, 0);
                paint_test_cell(vram, 7 * 24, 168, dw, dh);
                paint_test_cell(vram, 0, 24, iw, ih);
                build_icons(&icons);
                icons.rect[34].u = 0; icons.rect[34].v = 24;
                q2_statusbar_init(&b, &icons, cases[c].players);
                q2_statusbar_layout(&b, cases[c].layout, view, height);
                q2_statusbar_anchor(&b, 0,
                    cases[c].layout == Q2_SBAR_LAYOUT_QUAD ? 0 :
                    (s16)(cases[c].layout == Q2_SBAR_LAYOUT_TWO_H
                              ? (height / 2 - 29) : height - 32));
                b.health = 7;
                b.health_icon = 34;
                b.weapon = Q2_SBAR_WEAPON_NO_AMMO;
                b.frags = -7;
                psx_fb_clear(&fb, 0);
                psx_ot_clear(&ot);
                q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
                psx_raster_ot(&fb, &ot, vram, &opts);

                for (i = 0; i < ot.prim_count; i++) {
                    const psx_prim *p = &ot.prims[i];
                    int x = p->xy[0].x, y = p->xy[0].y;
                    int w = p->xy[1].x - x, h = p->xy[3].y - y;

                    CHECK((u8)(p->uv[1].u - p->uv[0].u) == w &&
                          (u8)(p->uv[3].v - p->uv[0].v) == h,
                          "layout %u, view %d, %d lines: UVs are texel-for-pixel",
                          c, view, height);
                    if ((p->uv[0].u == 7 * 24 && p->uv[0].v == 168) ||
                        (p->uv[0].u == 0 && p->uv[0].v == 24)) {
                        CHECK(fb.px[(y + h / 2) * fb.width + x + w - 1] ==
                              psx_rgb555(96, 0, 0),
                              "layout %u, view %d: sprite's right border survives",
                              c, view);
                        CHECK(fb.px[(y + h - 1) * fb.width + x + w / 2] ==
                              psx_rgb555(0, 96, 0),
                              "layout %u, view %d: sprite's bottom border survives",
                              c, view);
                        painted++;
                    }
                }
                CHECK(painted == 3, "health, icon and signed frag digit drawn");
            }
        }
    }
    psx_fb_free(&fb);
    psx_ot_free(&ot);
    free(vram);
}

static void test_bar_on_screen(void)
{
    q2_icon_tables icons;
    q2_statusbar b;
    psx_ot ot;
    psx_framebuffer fb;
    psx_raster_opts opts;
    psx_vram *vram;
    int x, y, units, left, right, mid;

    vram = (psx_vram *)calloc(1, sizeof(*vram));
    if (!vram || psx_ot_init(&ot, 64, 256) != Q2_OK ||
        psx_fb_init(&fb, SCREEN_W, SCREEN_H) != Q2_OK) {
        CHECK(0, "a framebuffer, a table and a VRAM to draw the bar with");
        free(vram);
        return;
    }
    for (y = 0; y < 256; y++)
        for (x = 64; x < 128; x++)          /* page 1, 4bpp: 4 texels a word */
            vram->px[y][x] = 0x1111;
    vram->px[0][1] = psx_rgb555(TEXEL_GREY, TEXEL_GREY, TEXEL_GREY) | 0x8000u;

    psx_raster_opts_default(&opts);
    opts.dither = false;

    build_icons(&icons);
    q2_statusbar_init(&b, &icons, 1);
    q2_statusbar_anchor(&b, BAR_ANCHOR_X, BAR_ANCHOR_Y);
    b.health = 7;                 /* one numeral, in the units field (70..94) */
    b.weapon = 0;
    b.strip[0] = 2;               /* slot A at 388..420 */
    b.strip[1] = 4;               /* slot B at 458..490 */

    psx_fb_clear(&fb, psx_rgb555(BACK_GREY, BACK_GREY, BACK_GREY));
    psx_ot_clear(&ot);
    q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
    psx_raster_ot(&fb, &ot, vram, &opts);

    /*
     * 192, BRIGHTENED. A field packet is opaque and modulated, so a texel of 64
     * comes out at 64 * 192 / 128 = 96 — where unity would have left it at 64.
     * Sampled mid-cell, inside the numeral at (70..94, 202..226).
     */
    units = grey_at(&fb, BAR_ANCHOR_X - 23 + 12, BAR_ANCHOR_Y + 1 + 12);
    CHECK(units == TEXEL_GREY * 3 / 2,
          "a field texel of %d is drawn at %d — 1.5x (got %d)", TEXEL_GREY,
          TEXEL_GREY * 3 / 2, units);
    CHECK(units != TEXEL_GREY, "and not at unity, which 128 would give");

    /*
     * THE FADE IS INTO THE WORLD. Slot A's left column has 0 in both of its
     * packets, so nothing is added and nothing subtracted: the backdrop shows
     * through untouched. Drawn opaque, that column was a black silhouette.
     */
    mid   = BAR_ANCHOR_Y + 12;
    left  = grey_at(&fb, 388, mid);
    right = grey_at(&fb, 419, mid);
    CHECK(left == BACK_GREY,
          "slot A's outer (left) column is the backdrop, %d, not black (%d)",
          BACK_GREY, left);
    CHECK(right > BACK_GREY,
          "and its inner column is the backdrop plus the icon (%d > %d)",
          right, BACK_GREY);
    CHECK(right > left, "so it fades toward the centre, not the outside");

    /* Slot B, the mirror: bright at 458, the backdrop again at 489. */
    left  = grey_at(&fb, 458, mid);
    right = grey_at(&fb, 489, mid);
    CHECK(right == BACK_GREY,
          "slot B's outer (right) column is the backdrop (%d)", right);
    CHECK(left > BACK_GREY, "and its inner column is lit (%d)", left);

    /*
     * The shadow is real. At slot A's inner column the gouraud is 124 of 128,
     * so the icon adds 62 and the shadow, at 60 of 128, takes 30 away FIRST:
     * 96 - 30 = 66 (64 in five bits), + 62 = 126, stored as 120. Additive
     * alone would store 152, and opaque would store 56 — so the window
     * (96, 128] admits only the two-packet draw.
     */
    right = grey_at(&fb, 419, mid);
    CHECK(right > BACK_GREY && right <= BACK_GREY + TEXEL_GREY / 2,
          "the subtractive shadow darkens under the icon (%d in (%d, %d])",
          right, BACK_GREY, BACK_GREY + TEXEL_GREY / 2);

    psx_fb_free(&fb);
    psx_ot_free(&ot);
    free(vram);
}

/* ------------------------------------------------------------------------- */
/* Health is SIGNED and floors at -99 — 0x80035224 / 0x800352A4              */
/* ------------------------------------------------------------------------- */
static void test_signed_health(void)
{
    q2_icon_tables icons;
    q2_statusbar b;
    psx_ot ot;
    const psx_prim *hundreds, *tens, *units;
    int nh, nt, nu;

    build_icons(&icons);
    if (psx_ot_init(&ot, 64, 256) != Q2_OK) {
        CHECK(0, "an ordering table for the health readout");
        return;
    }

    q2_statusbar_init(&b, &icons, 1);
    q2_statusbar_anchor(&b, BAR_ANCHOR_X, BAR_ANCHOR_Y);
    b.health_icon = 34;
    b.weapon = 0;

    /* The three digit fields, in screen columns. */
#define HX_HUNDREDS (BAR_ANCHOR_X - 71)
#define HX_TENS     (BAR_ANCHOR_X - 47)
#define HX_UNITS    (BAR_ANCHOR_X - 23)

    /* -25 is {minus, 2, 5}: the sign goes in the HUNDREDS cell because the
     * tens digit is not blank (0x80034F80). */
    b.health = -25;
    psx_ot_clear(&ot);
    q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
    hundreds = cell_at(&ot, HX_HUNDREDS, &nh);
    tens     = cell_at(&ot, HX_TENS,     &nt);
    units    = cell_at(&ot, HX_UNITS,    &nu);
    CHECK(nh == 1 && nt == 1 && nu == 1,
          "-25 fills all three health cells (%d, %d, %d)", nh, nt, nu);
    CHECK(hundreds && hundreds->uv[0].u == 0 && hundreds->uv[0].v == 192,
          "the minus is glyph 10 at (0, 192)");
    CHECK(tens && tens->uv[0].u == 2 * 24 && tens->uv[0].v == 168,
          "then the 2");
    CHECK(units && units->uv[0].u == 5 * 24 && units->uv[0].v == 168,
          "then the 5");

    /* -5 is {blank, minus, 5}: with the tens blank the sign moves into it
     * (0x80034F70), and the hundreds cell stays empty. */
    b.health = -5;
    psx_ot_clear(&ot);
    q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
    CHECK(cell_at(&ot, HX_HUNDREDS, &nh) == NULL && nh == 0,
          "-5 leaves the hundreds cell empty");
    tens  = cell_at(&ot, HX_TENS,  &nt);
    units = cell_at(&ot, HX_UNITS, &nu);
    CHECK(nt == 1 && tens && tens->uv[0].u == 0 && tens->uv[0].v == 192,
          "the minus sits in the TENS cell");
    CHECK(nu == 1 && units && units->uv[0].u == 5 * 24,
          "with the 5 beside it");

    /* The floor: -400 reads -99, not -400 and not 0. */
    b.health = -400;
    psx_ot_clear(&ot);
    q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
    hundreds = cell_at(&ot, HX_HUNDREDS, &nh);
    tens     = cell_at(&ot, HX_TENS,     &nt);
    units    = cell_at(&ot, HX_UNITS,    &nu);
    CHECK(hundreds && hundreds->uv[0].v == 192, "-400 clamps to -99: minus");
    CHECK(tens && tens->uv[0].u == 9 * 24, "nine");
    CHECK(units && units->uv[0].u == 9 * 24, "nine");

    /* And a live player is unchanged — 100 is still {1, 0, 0}. */
    b.health = 100;
    psx_ot_clear(&ot);
    q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
    hundreds = cell_at(&ot, HX_HUNDREDS, &nh);
    tens     = cell_at(&ot, HX_TENS,     &nt);
    units    = cell_at(&ot, HX_UNITS,    &nu);
    CHECK(hundreds && hundreds->uv[0].u == 1 * 24 && hundreds->uv[0].v == 168,
          "100 still reads one");
    CHECK(tens && tens->uv[0].u == 0 && tens->uv[0].v == 168, "zero");
    CHECK(units && units->uv[0].u == 0 && units->uv[0].v == 168, "zero");

    /* A single-digit live value is still right-aligned into the units cell. */
    b.health = 7;
    psx_ot_clear(&ot);
    q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
    CHECK(cell_at(&ot, HX_HUNDREDS, &nh) == NULL &&
          cell_at(&ot, HX_TENS, &nt) == NULL,
          "7 uses the units cell alone");
    units = cell_at(&ot, HX_UNITS, &nu);
    CHECK(units && units->uv[0].u == 7 * 24, "and it is the 7");

    /*
     * 0x80035248 `blez v1, 0x80035268` jumps PAST the 0x800AEBAC bit-7 test, so
     * a dead readout holds the flash solid instead of winking; at 10 it blinks.
     * The palette lands in the CLUT, so compare the two parities of the clock.
     */
    {
        static q2_hud_tables pal;
        const psx_prim *p;
        u16 dead_a, dead_b, live_a, live_b, healthy;

        memset(&pal, 0, sizeof(pal));
        pal.palette_count = Q2_HUD_PALETTE_MAX;
        pal.palette[Q2_SBAR_PAL_LOW].present    = true;
        pal.palette[Q2_SBAR_PAL_LOW].clut_id    = 0x0077;
        pal.palette[Q2_SBAR_PAL_DIGITS].present = true;
        pal.palette[Q2_SBAR_PAL_DIGITS].clut_id = 0x0088;
        q2_statusbar_set_palettes(&b, &pal);

        b.health = 100;
        b.ticks = 0;
        psx_ot_clear(&ot);
        q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
        p = cell_at(&ot, HX_UNITS, &nu);
        healthy = p ? p->clut : 0;
        CHECK(healthy == 0x0088, "a healthy readout uses the numerals' own 8");

        b.health = -25;
        b.ticks = 0;
        psx_ot_clear(&ot);
        q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
        p = cell_at(&ot, HX_UNITS, &nu);
        dead_a = p ? p->clut : 0;
        b.ticks = Q2_SBAR_BLINK_BIT;
        psx_ot_clear(&ot);
        q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
        p = cell_at(&ot, HX_UNITS, &nu);
        dead_b = p ? p->clut : 0;
        CHECK(dead_a == 0x0077 && dead_b == 0x0077,
              "a dead readout holds palette 7 solid — 0x80035248's `blez` jumps "
              "past the 0x800AEBAC test (%04X, %04X)", dead_a, dead_b);

        b.health = 10;
        b.ticks = 0;
        psx_ot_clear(&ot);
        q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
        p = cell_at(&ot, HX_UNITS, &nu);
        live_a = p ? p->clut : 0;
        b.ticks = Q2_SBAR_BLINK_BIT;
        psx_ot_clear(&ot);
        q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
        p = cell_at(&ot, HX_UNITS, &nu);
        live_b = p ? p->clut : 0;
        CHECK(live_a == 0x0077 && live_b == 0x0088,
              "and low health above zero blinks between 7 and 8 (%04X, %04X)",
              live_a, live_b);

        q2_statusbar_set_palettes(&b, NULL);
        b.ticks = 0;
    }

    psx_ot_free(&ot);
}

/* ------------------------------------------------------------------------- */
/* Death strips the one-player bar — 0x80033C68                              */
/* ------------------------------------------------------------------------- */
static void test_dead_strips_the_bar(void)
{
    q2_icon_tables icons;
    q2_statusbar b;
    psx_ot ot;
    u32 alive, dead, split_alive, split_dead;
    int n;

    build_icons(&icons);
    real_cell(&icons, 26, 128, 24, 5);        /* body armour, rect 26 */
    real_cell(&icons, Q2_SBAR_ICON_POWERUP_QUAD, 64, 96, 8);
    if (psx_ot_init(&ot, 128, 256) != Q2_OK) {
        CHECK(0, "an ordering table for the death gate");
        return;
    }

    q2_statusbar_init(&b, &icons, 1);
    q2_statusbar_anchor(&b, BAR_ANCHOR_X, BAR_ANCHOR_Y);
    b.health_icon     = 34;
    b.armour          = 50;
    b.armour_icon     = 26;
    b.ammo            = 30;
    b.weapon          = 2;
    b.powerup_icon    = Q2_SBAR_ICON_POWERUP_QUAD;
    b.powerup_seconds = 30;
    b.pickup_icon     = 18;
    b.strip[0]        = 2;
    b.strip[1]        = 4;

    b.health = 100;
    psx_ot_clear(&ot);
    alive = q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);

    b.health = 0;
    psx_ot_clear(&ot);
    dead = q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);

    /*
     * What survives: the health cross, the health digits, field 12 and the two
     * strip cells. What goes: the ammo group (two digits, no icon here because
     * rect[ammoIcon[2]] is not populated), the armour group (two digits and its
     * icon), the powerup group (icon + two digits) and the pickup icon.
     */
    CHECK(dead < alive, "death removes sprites (%u -> %u)", alive, dead);
    /*
     * Exactly. Alive: "100" (3) + cross + field 12 + "30" ammo (2) + "50"
     * armour (2) + its vest + the quad icon + "30" seconds (2) + the pickup
     * icon + two strip icons of two packets each (4) = 18. Dead: "0" + cross +
     * field 12 + the strip's 4 = 7.
     */
    CHECK(alive == 18 && dead == 7,
          "the dead bar is the health readout, field 12 and the strip and "
          "nothing else (%u of %u)", dead, alive);
    CHECK(cell_at(&ot, BAR_ANCHOR_X, &n) != NULL,
          "the health cross survives — 0x80033C40 runs above the gate");
    CHECK(cell_at(&ot, BAR_ANCHOR_X - 23, &n) != NULL,
          "and so does the health readout");
    CHECK(cell_at(&ot, BAR_ANCHOR_X + 330, &n) != NULL,
          "the weapon in hand survives — 0x80033C50 is also above it");
    CHECK(cell_at(&ot, 388, &n) != NULL && cell_at(&ot, 458, &n) != NULL,
          "the strip survives: 0x80033C68 branches TO the flag that arms it");

    CHECK(cell_at(&ot, BAR_ANCHOR_X + 112, &n) == NULL,
          "the ammo units digit is gone (0x800352C0 is below the gate)");
    CHECK(cell_at(&ot, BAR_ANCHOR_X + 250, &n) == NULL,
          "the armour icon is gone (0x80035554)");
    CHECK(cell_at(&ot, BAR_ANCHOR_X + 227, &n) == NULL,
          "and its digits with it");
    CHECK(cell_at(&ot, BAR_ANCHOR_X + 280, &n) == NULL,
          "the powerup timer is gone (0x80035B38)");
    CHECK(cell_at(&ot, BAR_ANCHOR_X - 71, &n) == NULL,
          "and the pickup icon (0x800359C0) — nothing is left on the upper row");

    /*
     * The gate is `<= 0`, and the readout still shows the negative figure. The
     * count is two higher than at zero only because "-25" fills three digit
     * cells where "0" fills one, so the assertion is on WHICH cells survive.
     */
    b.health = -25;
    psx_ot_clear(&ot);
    CHECK(q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0) == dead + 2,
          "-25 strips the same groups, with two more health digits");
    CHECK(cell_at(&ot, BAR_ANCHOR_X + 112, &n) == NULL &&
          cell_at(&ot, BAR_ANCHOR_X + 250, &n) == NULL &&
          cell_at(&ot, BAR_ANCHOR_X + 280, &n) == NULL,
          "negative health suppresses exactly what zero does");
    {
        const psx_prim *p = cell_at(&ot, BAR_ANCHOR_X - 71, &n);
        CHECK(p != NULL && p->uv[0].v == 192,
              "and the minus sign is still drawn");
    }

    /* The same rule, as the caption's text half asks for it (hud.c). */
    CHECK(q2_statusbar_stripped(&b), "-25 in one-player is stripped");
    b.health = 0;
    CHECK(q2_statusbar_stripped(&b), "so is 0 — the branch is `blez`");
    b.health = 1;
    CHECK(!q2_statusbar_stripped(&b), "and 1 is not");

    /*
     * IT MUST NOT LEAK INTO THE SPLITS. `access 0x108` finds entity+264 read
     * nowhere in 0x80033D30, 0x80034288 or 0x80034830.
     */
    q2_statusbar_layout(&b, Q2_SBAR_LAYOUT_TWO_H, 0, 248);
    b.players = 2;
    /* 1 and 0 both draw a single health digit, so any difference in the count
     * is the gate and nothing else. */
    b.health = 1;
    psx_ot_clear(&ot);
    split_alive = q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
    b.health = 0;
    psx_ot_clear(&ot);
    split_dead = q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
    CHECK(split_dead == split_alive,
          "a stacked viewport keeps its whole bar at zero health (%u vs %u)",
          split_alive, split_dead);
    CHECK(!q2_statusbar_stripped(&b),
          "and is never stripped, whatever its health");
    CHECK(cell_at(&ot, BAR_ANCHOR_X + q2_sbar_fields_2h[8].dx, &n) != NULL,
          "including the armour icon the one-player gate would have removed");

    psx_ot_free(&ot);
}

/* ------------------------------------------------------------------------- */
/* The armour counter does NOT flash — 0x80035554..0x800359B0                 */
/* ------------------------------------------------------------------------- */
/*
 * The whole armour sub-draw has been read: the five-arm icon select, one
 * `sb v0, 8(a2)` into the ICON's rect record at 0x80035748 / 0x80035990, the
 * zero guard at 0x80035994 and the `jal 0x80034F90` at 0x800359A8. Nothing in
 * it compares the armour or cells value against a threshold and nothing writes
 * byte 8 of a DIGIT record, so the digits keep the numeral palette the field
 * initialiser left there. Only health (0x8003523C/0x8003524C) and ammo
 * (0x80035440/0x80035460) carry the low-value flash.
 *
 * Both arms are covered: the regular one is guarded on `armour > 0`, the power
 * one (0x80035754 jumping straight to the shared draw) is guarded on nothing
 * at all, so a bare threshold change would leave that arm free to regress.
 * The flash is only observable through a palette bank — with `b.hud` NULL
 * pal_clut hands back the caller's single clut for both indices and the two
 * frames would match whatever the code did.
 */
static void test_armour_never_flashes(void)
{
    static q2_hud_tables pal;
    q2_icon_tables icons;
    q2_statusbar b;
    psx_ot ot;
    const psx_prim *p;
    const int ax = BAR_ANCHOR_X + q2_sbar_fields[11].dx;   /* armour units */
    u16 vest_a, vest_b, cells_a, cells_b;
    int n;

    build_icons(&icons);
    real_cell(&icons, Q2_SBAR_ICON_ARMOUR_JACKET, 128, 24, 33);
    real_cell(&icons, Q2_SBAR_ICON_POWER_SHIELD, 160, 24, 34);
    if (psx_ot_init(&ot, 128, 256) != Q2_OK) {
        CHECK(0, "an ordering table for the armour readout");
        return;
    }

    memset(&pal, 0, sizeof(pal));
    pal.palette_count = Q2_HUD_PALETTE_MAX;
    pal.palette[Q2_SBAR_PAL_LOW].present    = true;
    pal.palette[Q2_SBAR_PAL_LOW].clut_id    = 0x0077;
    pal.palette[Q2_SBAR_PAL_DIGITS].present = true;
    pal.palette[Q2_SBAR_PAL_DIGITS].clut_id = 0x0088;

    q2_statusbar_init(&b, &icons, 1);
    q2_statusbar_anchor(&b, BAR_ANCHOR_X, BAR_ANCHOR_Y);
    q2_statusbar_set_palettes(&b, &pal);
    b.health = 100;
    b.health_icon = 34;
    b.weapon = 0;

    /* Three points of armour, both parities of the blink clock. */
    b.armour = 3;
    b.armour_icon = Q2_SBAR_ICON_ARMOUR_JACKET;
    b.showing_power = false;
    b.ticks = 0;
    psx_ot_clear(&ot);
    q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
    p = cell_at(&ot, ax, &n);
    vest_a = p ? p->clut : 0;
    CHECK(p && p->uv[0].u == 3 * 24 && p->uv[0].v == 168,
          "3 points of armour reads a 3 in the units cell");
    b.ticks = Q2_SBAR_BLINK_BIT;
    psx_ot_clear(&ot);
    q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
    p = cell_at(&ot, ax, &n);
    vest_b = p ? p->clut : 0;
    CHECK(vest_a == 0x0088 && vest_b == 0x0088,
          "and holds the numerals' palette 8 on both parities — the armour "
          "sub-draw has no threshold compare (%04X, %04X)", vest_a, vest_b);

    /* And the power arm, which is the one with no value guard in front of it. */
    b.armour = 0;
    b.cells = 3;
    b.armour_icon = Q2_SBAR_ICON_POWER_SHIELD;
    b.showing_power = true;
    b.ticks = 0;
    psx_ot_clear(&ot);
    q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
    p = cell_at(&ot, ax, &n);
    cells_a = p ? p->clut : 0;
    b.ticks = Q2_SBAR_BLINK_BIT;
    psx_ot_clear(&ot);
    q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
    p = cell_at(&ot, ax, &n);
    cells_b = p ? p->clut : 0;
    CHECK(cells_a == 0x0088 && cells_b == 0x0088,
          "three cells under a power shield do not flash either (%04X, %04X)",
          cells_a, cells_b);

    /* Health at 10 still does, so the change did not disable the mechanism. */
    b.health = 10;
    b.ticks = 0;
    psx_ot_clear(&ot);
    q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
    p = cell_at(&ot, BAR_ANCHOR_X - 23, &n);
    vest_a = p ? p->clut : 0;
    b.ticks = Q2_SBAR_BLINK_BIT;
    psx_ot_clear(&ot);
    q2_statusbar_build_ot(&b, 1, 0, &ot, 8, 0, 0);
    p = cell_at(&ot, BAR_ANCHOR_X - 23, &n);
    vest_b = p ? p->clut : 0;
    CHECK(vest_a == 0x0077 && vest_b == 0x0088,
          "while health at 10 still blinks 7/8 (%04X, %04X)", vest_a, vest_b);

    q2_statusbar_set_palettes(&b, NULL);
    psx_ot_free(&ot);
}

int main(void)
{
    test_field_groups();
    test_digit_pitch();
    test_counters_are_ordered();
    test_two_rows();
    test_two_player_layout();
    test_quad_layout();
    test_icon_vocabulary();
    test_digits_of();
    test_frag_glyphs();
    test_split_screen_sizes();
    test_armour_icon_select();
    test_armour_class_upkeep();
    test_powerup_timer();
    test_pickup_icon();
    test_ammo_pool();
    test_weapon_slots();
    test_weapon_slots_latch();
    test_weapon_icon_field();
    test_strip_geometry();
    test_field_modulation();
    test_bar_on_screen();
    test_split_sprite_texels();
    test_signed_health();
    test_armour_never_flashes();
    test_dead_strips_the_bar();

    if (g_fail) {
        printf("\n%d of %d status-bar check%s failed\n", g_fail, g_checks,
               g_checks == 1 ? "" : "s");
        return 1;
    }
    printf("statusbar: %d checks, 0 failures\n", g_checks);
    return 0;
}
