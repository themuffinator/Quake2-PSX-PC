/*
 * test_statusbar.c — the bar that FORMATS.md §11.1 said did not exist.
 *
 * What is worth pinning here is the field arithmetic, because it is the part a
 * plausible-looking mistake survives: three counters of three digits with an
 * icon each, digits 24 apart because a numeral cell is 24 wide, values right
 * aligned so the units column does not move.
 */
#include "statusbar.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
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
     * The reduction is a CLAMP, not a scale: two players force 24 x 18 and
     * three or more 16 x 12 whatever the source rect, so a 24-wide numeral and
     * a 32-wide icon come out the same size in split screen. Single player
     * passes the source through, which is what keeps numerals unstretched.
     */
    CHECK(q2_icon_draw_size_of(1, 1, 24, 24).w == 24,
          "single player keeps a numeral at 24");
    CHECK(q2_icon_draw_size_of(1, 1, 32, 24).w == 32,
          "single player keeps an icon at 32");
    CHECK(q2_icon_draw_size_of(2, 1, 24, 24).w == 24 &&
          q2_icon_draw_size_of(2, 1, 32, 24).w == 24,
          "two players clamp both to 24");
    CHECK(q2_icon_draw_size_of(4, 1, 24, 24).w == 16 &&
          q2_icon_draw_size_of(4, 1, 32, 24).h == 12,
          "four players clamp to 16x12");

    /* No weapon collapses to the blank rather than scaling a 1x1 up. */
    CHECK(q2_icon_draw_size_of(1, 0, 32, 24).w == 1,
          "no weapon draws the blank");
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
    b.health = 0;            /* the counters draw nothing, so the count is */
    b.armour = 0;            /* the pickup icon's alone                    */
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

    if (g_fail) {
        printf("\n%d status-bar check%s failed\n", g_fail,
               g_fail == 1 ? "" : "s");
        return 1;
    }
    printf("statusbar: all checks passed\n");
    return 0;
}
