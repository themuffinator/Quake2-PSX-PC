/*
 * test_item.c — the item table, the item entity and the touch dispatch.
 *
 * Every number asserted here came out of the executable, so these tests pin the
 * VALUES as well as the behaviour: if a future pass re-transcribes a handler
 * wrongly, a medkit that gives 12 health fails here rather than quietly
 * changing how the game plays.
 *
 * The addresses in the comments are the instructions each expectation was read
 * from, so a failure can be taken straight back to the disassembly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crebind.h"
#include "effect.h"
#include "entity.h"
#include "entitydraw.h"
#include "item.h"
#include "itemtable.h"
#include "levelbin.h"
#include "modeldraw.h"
#include "modelent.h"     /* Q2_GIB_RENDER_FLAGS, what 0x8005A1A4 writes */
#include "trig.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what)
{
    g_checks++;
    if (!condition) {
        printf("  FAIL  %s\n", what);
        g_failures++;
    }
}

static void check_eq_i(s64 got, s64 want, const char *what)
{
    g_checks++;
    if (got != want) {
        printf("  FAIL  %s: got %lld, want %lld\n",
               what, (long long)got, (long long)want);
        g_failures++;
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    g_checks++;
    if (!got || strcmp(got, want) != 0) {
        printf("  FAIL  %s: got \"%s\", want \"%s\"\n",
               what, got ? got : "(null)", want);
        g_failures++;
    }
}

/* ------------------------------------------------------------------------- */
static const q2_item_def *find(const char *model)
{
    const q2_item_table *t = q2_item_table_builtin();
    u32 i;

    for (i = 0; i < t->count; i++)
        if (strcmp(t->def[i].model, model) == 0)
            return &t->def[i];
    return NULL;
}

static void test_table(void)
{
    const q2_item_table *t = q2_item_table_builtin();
    const q2_item_def *e;

    printf("item table (0x8009F5CC)\n");

    check_eq_i(t->count, 64, "64 records before the 0xFF terminator");

    /* The first-match scan, and the one duplicate. Place id 6 appears twice and
     * the engine takes the first, so the second record is dead. */
    e = q2_item_find(t, 6);
    check(e != NULL && e == &t->def[14], "place id 6 resolves to the FIRST Bandol P");

    /* Two names use all twelve bytes with no NUL. */
    e = find("Large Medi P");
    check(e != NULL, "Large Medi P present");
    check(e && strlen(e->model) == 12, "a 12-byte name is not truncated");

    /* Weapons, by the effect index each names. */
    check_eq_i(find("Shotgun P")->effect,   2, "Shotgun P  -> effect 2");
    check_eq_i(find("Sshotgun P")->effect,  3, "Sshotgun P -> effect 3");
    check_eq_i(find("Machgun P")->effect,   4, "Machgun P  -> effect 4");
    check_eq_i(find("Chaingun P")->effect,  5, "Chaingun P -> effect 5");
    check_eq_i(find("Glaunch P")->effect,   7, "Glaunch P  -> effect 7");
    check_eq_i(find("Rocketl P")->effect,   8, "Rocketl P  -> effect 8");
    check_eq_i(find("Hyperbl P")->effect,   9, "Hyperbl P  -> effect 9");
    check_eq_i(find("Railgun P")->effect,  10, "Railgun P  -> effect 10");
    check_eq_i(find("Bfg P")->effect,      11, "Bfg P      -> effect 11");

    /* The final eight bytes are global storage vertices for 0x800784CC's
     * posed shadow footprint, not animation clips. Values such as 63 are
     * deliberately well beyond these single-clip item models' clip index. */
    e = find("Shotgun P");
    check_eq_i(e->shadow_vertex_count, 2, "Shotgun carries two shadow vertices");
    check_eq_i(e->shadow_vertex[0], 13, "Shotgun shadow vertex 0 is 13");
    check_eq_i(e->shadow_vertex[1], 63, "Shotgun shadow vertex 1 is 63");
    check_eq_i(find("Medi P")->shadow_vertex_count, 0,
               "Medi has no shadow-vertex expansion list");

    /*
     * The eight items whose effect index lands on the dispatch's failure exit.
     * This is a load-bearing negative: a port that invented an effect for them
     * would be more complete than the game.
     */
    check(!q2_item_effect_is_live(t, find("Ionripper P")->effect),
          "Ionripper P is inert");
    check(!q2_item_effect_is_live(t, find("Plasmagun P")->effect),
          "Plasmagun P is inert");
    check(!q2_item_effect_is_live(t, find("Flame P")->effect),
          "Flame P is inert");
    check(!q2_item_effect_is_live(t, find("Tesla P")->effect),
          "Tesla P is inert");
    check(!q2_item_effect_is_live(t, find("Discharge P")->effect),
          "Discharge P is inert");
    check(!q2_item_effect_is_live(t, find("Flame Fuel P")->effect),
          "Flame Fuel P is inert");
    check(!q2_item_effect_is_live(t, find("Screen P")->effect),
          "Screen P (power screen) is inert");
    check(!q2_item_effect_is_live(t, find("Stimpack P")->effect),
          "Stimpack P is inert");

    /* And the ones that are not. */
    check(q2_item_effect_is_live(t, find("Shield P")->effect),
          "Shield P (power shield) is live");
    check(q2_item_effect_is_live(t, find("Medi P")->effect), "Medi P is live");

    /* Effect 0 means "cannot be collected", not "collect for nothing". */
    check_eq_i(find("Barrel P")->effect, 0, "Barrel P is scenery");
    check_eq_i(find("Q2LOGO")->effect,   0, "Q2LOGO is scenery");
}

static void test_glow_colours(void)
{
    printf("\nthe glow bits name the keys' own colours\n");

    /*
     * This is the argument that makes bits 4/5/6 CONFIRMED rather than
     * plausible: decoded as one dynamic-light channel each, the twelve key
     * records spell out the colours their names claim.
     */
    check_eq_i(find("Redkey P")->flags & Q2_ITEM_GLOW, Q2_ITEM_GLOW_R,
               "Redkey glows red");
    check_eq_i(find("Greenkey P")->flags & Q2_ITEM_GLOW, Q2_ITEM_GLOW_G,
               "Greenkey glows green");
    check_eq_i(find("Bluekey P")->flags & Q2_ITEM_GLOW, Q2_ITEM_GLOW_B,
               "Bluekey glows blue");
    check_eq_i(find("Yellowkey P")->flags & Q2_ITEM_GLOW,
               Q2_ITEM_GLOW_R | Q2_ITEM_GLOW_G, "Yellowkey glows red+green");
    check_eq_i(find("Pkeypurp P")->flags & Q2_ITEM_GLOW,
               Q2_ITEM_GLOW_R | Q2_ITEM_GLOW_B, "Pkeypurp glows red+blue");
    check_eq_i(find("Whitekey P")->flags & Q2_ITEM_GLOW, Q2_ITEM_GLOW,
               "Whitekey glows all three");

    /* Bit 3 partitions exactly the twelve key/objective records. */
    {
        const q2_item_table *t = q2_item_table_builtin();
        u32 i, objective = 0, keyfx = 0;

        for (i = 0; i < t->count; i++) {
            if (t->def[i].flags & Q2_ITEM_OBJECTIVE)
                objective++;
            if (q2_item_effect_def(t->def[i].effect)->kind == Q2_ITEMFX_KEY)
                keyfx++;
        }
        check_eq_i(objective, 12, "twelve records carry bit 3");
        check_eq_i(keyfx, 12, "twelve records carry a key effect");
    }

    /* Only Bluekey materialises, only Blackhole suppresses its animation. */
    check(find("Bluekey P")->flags & Q2_ITEM_MATERIALISE,
          "Bluekey materialises");
    check(find("Blackhole P")->flags & Q2_ITEM_NO_ANIM,
          "Blackhole holds frame 0");
    check(find("Blackhole P")->flags & Q2_ITEM_NO_DROP,
          "Blackhole clears the spawner's 0x400 argument");
}

/* ------------------------------------------------------------------------- */
static void world_with_player(q2_entity_world *w, q2_inventory *inv)
{
    static const s32 origin[3] = { 0, 0, 0 };

    q2_entity_world_init(w);
    q2_inventory_init(inv);
    q2_entity_world_add_player(w, 0, inv, origin);
    w->dt = 12;                 /* one nominal logic step */
}

static void test_ammo(void)
{
    q2_entity_world w;
    q2_inventory inv;
    q2_touch_result r;

    printf("\nammo boxes (0x8003696C onward)\n");

    world_with_player(&w, &inv);

    r = q2_item_touch(18, &inv, inv.health ? w.player[0].pos : NULL, &w);
    check_eq_i(r, Q2_TOUCH_COLLECTED, "shells box is collected");
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 10, "shells box gives 10 in SP");

    inv.ammo[Q2_AMMO_SHELLS] = 100;      /* the base tier's cap */
    r = q2_item_touch(18, &inv, w.player[0].pos, &w);
    check_eq_i(r, Q2_TOUCH_NOTHING, "a full player leaves the shells box");

    /* Deathmatch doubles the ammo boxes. */
    world_with_player(&w, &inv);
    w.deathmatch = true;
    q2_item_touch(18, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 20, "shells box gives 20 in DM");

    world_with_player(&w, &inv);
    q2_item_touch(19, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo[Q2_AMMO_BULLETS], 50, "bullets box gives 50");
    q2_item_touch(21, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo[Q2_AMMO_ROCKETS], 5, "rockets box gives 5");
    q2_item_touch(22, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo[Q2_AMMO_CELLS], 50, "cells box gives 50");
    q2_item_touch(23, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo[Q2_AMMO_SLUGS], 10, "slugs box gives 10");

    /* 0x80036A90: the grenade box is also the hand grenade weapon. */
    q2_item_touch(20, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo[Q2_AMMO_GRENADES], 5, "grenade box gives 5");
    check(q2_inventory_has_weapon(&inv, Q2_WEAPON_HAND_GRENADE),
          "the grenade box also grants the hand grenade");
}

static void test_weapons(void)
{
    q2_entity_world w;
    q2_inventory inv;
    q2_touch_result r;

    printf("\nweapon pickups (0x800363BC onward)\n");

    world_with_player(&w, &inv);
    r = q2_item_touch(2, &inv, w.player[0].pos, &w);
    check_eq_i(r, Q2_TOUCH_WEAPON, "a weapon returns 1, not 0");
    check(q2_inventory_has_weapon(&inv, Q2_WEAPON_SHOTGUN), "shotgun granted");
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 10, "shotgun brings 10 shells");
    check_eq_i(inv.current_weapon, Q2_WEAPON_SHOTGUN,
               "the blaster switches away (0x80037E84)");

    /* Picking it up again with a better weapon out must NOT switch back. */
    q2_item_touch(11, &inv, w.player[0].pos, &w);      /* the BFG */
    check_eq_i(inv.current_weapon, Q2_WEAPON_SHOTGUN,
               "a second pickup does not switch while off the blaster");

    /* 0x8003668C / 0x800366D0: the launcher grants hand grenades too. */
    world_with_player(&w, &inv);
    q2_item_touch(7, &inv, w.player[0].pos, &w);
    check(q2_inventory_has_weapon(&inv, Q2_WEAPON_GRENADE_LAUNCHER),
          "launcher granted");
    check(q2_inventory_has_weapon(&inv, Q2_WEAPON_HAND_GRENADE),
          "and the hand grenade with it");
    check_eq_i(inv.current_weapon, Q2_WEAPON_GRENADE_LAUNCHER,
               "and the launcher is selected");

    /* 0x800363C8: INFINITE AMMO grants the type's capacity instead. */
    world_with_player(&w, &inv);
    w.cheats = Q2_CHEAT_INFINITE_AMMO;
    q2_item_touch(2, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 100,
               "INFINITE AMMO fills the shotgun's shells to capacity");

    /* 0x80037E4C: only deathmatch AND weapons-stay refuses a second pickup. */
    world_with_player(&w, &inv);
    w.deathmatch = true;
    w.weapons_stay = true;
    q2_item_touch(2, &inv, w.player[0].pos, &w);
    r = q2_item_touch(2, &inv, w.player[0].pos, &w);
    check_eq_i(r, Q2_TOUCH_NOTHING,
               "weapons-stay refuses a weapon already held");
}

static void test_armour(void)
{
    q2_entity_world w;
    q2_inventory inv;
    q2_touch_result r;

    printf("\narmour (0x80036C14 / 0x80037338)\n");

    /* Jacket first: base 25, cap 50. */
    world_with_player(&w, &inv);
    q2_item_touch(28, &inv, w.player[0].pos, &w);
    check_eq_i(inv.armour, 25, "jacket armour gives its base 25");
    check_eq_i(inv.armour_class, Q2_ARMOUR_JACKET, "and sets the class");
    check(inv.flags & Q2_INV_ARMOUR_JACKET, "and its flag bit");

    /* A second jacket tops up to the class cap and then refuses. */
    q2_item_touch(28, &inv, w.player[0].pos, &w);
    check_eq_i(inv.armour, 50, "a second jacket reaches the 50 cap");
    r = q2_item_touch(28, &inv, w.player[0].pos, &w);
    check_eq_i(r, Q2_TOUCH_NOTHING, "a full jacket refuses (0x80036D40)");

    /*
     * Body armour over a full jacket: the old 50 converts down by
     * jacket.normal / body.normal = 1229/3277, in two-bit fixed point, and the
     * body base is added. (1229<<2)/3277 == 1, so 50 * 1 >> 2 == 12.
     */
    q2_item_touch(26, &inv, w.player[0].pos, &w);
    check_eq_i(inv.armour_class, Q2_ARMOUR_BODY, "body armour upgrades the class");
    check_eq_i(inv.armour, 112, "and salvages the jacket into 100 + 12");

    /* 0x80036CB0: combat armour has no full test at all. */
    world_with_player(&w, &inv);
    inv.armour_class = Q2_ARMOUR_COMBAT;
    inv.armour = 100;                    /* the combat cap */
    inv.flags |= Q2_INV_ARMOUR_COMBAT;
    r = q2_item_touch(27, &inv, w.player[0].pos, &w);
    check_eq_i(r, Q2_TOUCH_COLLECTED,
               "combat armour is collected even at the cap");
    check_eq_i(inv.armour, 100, "and clamps rather than overflowing");

    /* Shards: +2 with a 999 cap, and they establish jacket armour. */
    world_with_player(&w, &inv);
    q2_item_touch(29, &inv, w.player[0].pos, &w);
    check_eq_i(inv.armour, 2, "a shard gives 2");
    check(inv.flags & Q2_INV_ARMOUR_JACKET,
          "a shard with nothing held becomes jacket armour");
    inv.armour = 998;
    q2_item_touch(29, &inv, w.player[0].pos, &w);
    check_eq_i(inv.armour, 999, "and clamps at the literal 999");
}

static void test_health(void)
{
    q2_entity_world w;
    q2_inventory inv;
    q2_touch_result r;

    printf("\nhealth (0x80036E20 onward)\n");

    world_with_player(&w, &inv);
    inv.health = 50;
    q2_item_touch(34, &inv, w.player[0].pos, &w);
    check_eq_i(inv.health, 60, "a medkit gives 10");
    q2_item_touch(35, &inv, w.player[0].pos, &w);
    check_eq_i(inv.health, 85, "a large medkit gives 25");
    q2_item_touch(35, &inv, w.player[0].pos, &w);
    check_eq_i(inv.health, 100, "and clamps at the player's max");
    r = q2_item_touch(34, &inv, w.player[0].pos, &w);
    check_eq_i(r, Q2_TOUCH_NOTHING, "a full player leaves the medkit");

    /* 0x80036E20: 200 and 100 are immediates, so mega health always applies. */
    r = q2_item_touch(32, &inv, w.player[0].pos, &w);
    check_eq_i(r, Q2_TOUCH_COLLECTED, "mega health is never refused");
    check_eq_i(inv.health, 200, "and overheals to the literal 200");
    check(inv.flags & Q2_INV_MEGA_HEALTH, "and raises its flag");

    /* And it bleeds back down, one point per 1500 ticks. */
    {
        s32 t = w.level_time;
        inv.mega_health_next = t + Q2_ITEM_MEGA_DECAY_TICKS;
        q2_item_mega_health_tick(&inv, t + Q2_ITEM_MEGA_DECAY_TICKS);
        check_eq_i(inv.health, 199, "mega health decays a point");
        inv.health = 100;
        q2_item_mega_health_tick(&inv, t + 2 * Q2_ITEM_MEGA_DECAY_TICKS);
        check(!(inv.flags & Q2_INV_MEGA_HEALTH),
              "and the flag drops once back at the cap");
    }

    /* 0x80036E64: adrenaline raises the max by one and tops health up to it. */
    world_with_player(&w, &inv);
    inv.health = 40;
    q2_item_touch(33, &inv, w.player[0].pos, &w);
    check_eq_i(inv.health_max, 101, "adrenaline raises max health to 101");
    check_eq_i(inv.health, 101, "and fills health to the new max");
}

static void test_capacity_and_powerups(void)
{
    q2_entity_world w;
    q2_inventory inv;

    printf("\ncapacity and powerups\n");

    /* 0x8003701C: bandolier, and a pack is not downgraded by one. */
    world_with_player(&w, &inv);
    q2_item_touch(38, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo_tier, Q2_AMMO_TIER_BANDOLIER, "bandolier raises the tier");
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 10, "and brings 10 shells");
    check_eq_i(inv.ammo[Q2_AMMO_BULLETS], 50, "and 50 bullets");
    check_eq_i(q2_inventory_ammo_max(&inv, Q2_AMMO_SHELLS), 150,
               "the shells cap rises to 150");

    q2_item_touch(37, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo_tier, Q2_AMMO_TIER_PACK, "the pack raises it again");
    check_eq_i(q2_inventory_ammo_max(&inv, Q2_AMMO_SHELLS), 200,
               "the shells cap rises to 200");
    q2_item_touch(38, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo_tier, Q2_AMMO_TIER_PACK,
               "a bandolier does not downgrade a pack (0x80037028)");

    /* 0x80036F10: the pack tops up all six types. */
    world_with_player(&w, &inv);
    q2_item_touch(37, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS],   10, "pack: 10 shells");
    check_eq_i(inv.ammo[Q2_AMMO_BULLETS],  50, "pack: 50 bullets");
    check_eq_i(inv.ammo[Q2_AMMO_GRENADES],  5, "pack: 5 grenades");
    check_eq_i(inv.ammo[Q2_AMMO_ROCKETS],   5, "pack: 5 rockets");
    check_eq_i(inv.ammo[Q2_AMMO_CELLS],    50, "pack: 50 cells");
    check_eq_i(inv.ammo[Q2_AMMO_SLUGS],    10, "pack: 10 slugs");

    /* 0x800370D0: 30 seconds, extended from whichever is later. */
    world_with_player(&w, &inv);
    w.level_time = 1000;
    q2_item_touch(40, &inv, w.player[0].pos, &w);
    check_eq_i(inv.quad_until, 1000 + 9000, "quad runs 9000 ticks");
    check(q2_item_powerup_active(&inv, Q2_POWERUP_QUAD, 5000),
          "and is active in the middle");
    check(!q2_item_powerup_active(&inv, Q2_POWERUP_QUAD, 10001),
          "and lapses at the end");
    q2_item_touch(40, &inv, w.player[0].pos, &w);
    check_eq_i(inv.quad_until, 1000 + 18000, "a second quad stacks");

    q2_item_touch(41, &inv, w.player[0].pos, &w);
    check_eq_i(inv.invuln_until, 1000 + 9000, "invulnerability runs 9000");
    q2_item_touch(42, &inv, w.player[0].pos, &w);
    check_eq_i(inv.enviro_until, 1000 + 9000, "the enviro suit runs 9000");
    q2_item_touch(43, &inv, w.player[0].pos, &w);
    check_eq_i(inv.breather_until, 1000 + 9000, "the rebreather runs 9000");

    /* 0x800370BC: the silencer gives shots, not time. */
    q2_item_touch(39, &inv, w.player[0].pos, &w);
    check_eq_i(inv.silencer_shots, 30, "the silencer gives 30 shots");

    /* 0x80036DB4: the shield's bit goes up and cells come with it. */
    world_with_player(&w, &inv);
    q2_item_touch(30, &inv, w.player[0].pos, &w);
    check(inv.flags & Q2_INV_POWER_SHIELD, "the power shield raises its bit");
    check_eq_i(inv.ammo[Q2_AMMO_CELLS], 50, "and brings 50 cells");
    /* 0x00018000 is the pair combat.h's damage path tests; only the shield's
     * half can ever be set, because the screen has no handler. */
    check_eq_i(inv.flags & 0x00018000u, Q2_INV_POWER_SHIELD,
               "which is one of the two bits the damage path tests");
}

static void test_keys(void)
{
    q2_entity_world w;
    q2_inventory inv;
    u32 all;

    printf("\nkeys (0x800371C0 onward)\n");

    world_with_player(&w, &inv);

    q2_item_touch(44, &inv, w.player[0].pos, &w);
    check_eq_i(inv.flags & Q2_KEY_MASK, Q2_KEY_BLUE, "effect 44 is the blue key");
    q2_item_touch(56, &inv, w.player[0].pos, &w);
    check(inv.flags & Q2_KEY_WHITE, "effect 56 is the white key");

    /* 0x80037238 / 0x80037250: 50 is the PURPLE pyramid key and 51 the RED —
     * the pair is crossed relative to the table order, which is exactly the
     * sort of thing that has to be read rather than assumed. */
    world_with_player(&w, &inv);
    q2_item_touch(50, &inv, w.player[0].pos, &w);
    check_eq_i(inv.flags & Q2_KEY_MASK, Q2_KEY_PYRAMID_PURPLE,
               "effect 50 is the purple pyramid key");
    q2_item_touch(51, &inv, w.player[0].pos, &w);
    check(inv.flags & Q2_KEY_PYRAMID_RED,
          "effect 51 is the red pyramid key");

    /* All twelve fit the mask the script sees, and nothing else does. */
    world_with_player(&w, &inv);
    {
        u32 fx;
        for (fx = Q2_ITEM_EFFECT_FIRST; fx <= (u32)Q2_ITEM_EFFECT_LAST; fx++)
            if (q2_item_effect_def(fx)->kind == Q2_ITEMFX_KEY)
                q2_item_touch(fx, &inv, w.player[0].pos, &w);
    }
    all = q2_inventory_script_keys(&inv);
    check_eq_i(all, Q2_KEY_MASK, "all twelve keys fill the script's mask");
    check_eq_i(inv.flags & ~Q2_KEY_MASK, 0u,
               "and nothing outside it is touched");
}

/* ------------------------------------------------------------------------- */
static void test_spawn_and_think(void)
{
    q2_entity_set set;
    q2_entity_world w;
    q2_inventory inv;
    q2_pop_place place;
    q2_entity *e;

    printf("\nspawn (0x800599DC) and think (0x80059330)\n");

    memset(&set, 0, sizeof(set));
    world_with_player(&w, &inv);

    memset(&place, 0, sizeof(place));
    place.x = 1000;
    place.y = 2000;
    place.z = 3000;
    place.angle_flags = 0x9400;  /* marker + NOT_HARD, plus yaw 0x400 */
    place.id = 39;               /* Shotgun P */

    e = q2_item_spawn(&set, &place, NULL, 0, NULL);
    check(e != NULL, "a known place id spawns");
    check_eq_i(e->effect, 2, "and carries the table's effect index");
    check_eq_i(e->kind, Q2_ENT_KIND_ITEM, "and the item kind, 46");
    check(strcmp(e->model, "Shotgun P") == 0, "and the table's model name");
    check_eq_i(e->glow[0], 0x30, "item ambient red is retail's 0x30");
    check_eq_i(e->glow[1], 0x30, "item ambient green is retail's 0x30");
    check_eq_i(e->glow[2], 0x30, "item ambient blue is retail's 0x30");
    check_eq_i(e->shadow_vertex_count, 2,
               "spawn copies the table's shadow-vertex count");
    check_eq_i(e->shadow_vertex[0], 13,
               "spawn copies the first global shadow vertex");
    check_eq_i(e->shadow_vertex[1], 63,
               "spawn copies the second global shadow vertex");

    /* 0x80058930: the heading is the low twelve bits only. */
    check_eq_i(e->angles[1], 0x400,
               "the yaw is place->angle_flags & 0xFFF");

    /*
     * 0x80051068 / 0x800510A4 raise the record by 286 and then 30 before the
     * hull query; 0x800588F8 lowers the draw origin by 286 again. So the stored
     * position is 316 above the record and the draw origin is 30 above it.
     */
    check_eq_i(e->pos[1], 2000 - 316, "the position is the record raised by 316");
    check_eq_i(e->origin[1], 2000 - 30, "and the draw origin 30 above it");

    /* And the touch box is a 286-cube about the POSITION, not the origin. */
    check_eq_i(e->bounds_min[1], e->pos[1] - 286, "the box is about the position");
    check_eq_i(e->bounds_max[1], e->pos[1] + 286, "and is 572 tall");

    check_eq_i(e->scale, 4096, "a non-materialising item spawns full size");

    /* An id no record names spawns nothing at all. */
    place.id = 22;               /* the one gap inside the used range */
    check(q2_item_spawn(&set, &place, NULL, 0, NULL) == NULL,
          "an unnamed place id spawns nothing");

    /* 0x8005947C: the spin is -3 per tick of the level clock. */
    {
        s32 before = e->angles[1];
        w.dt = 6;
        e->think(e, &w);
        check_eq_i(e->angles[1], before - 3 * 6, "the spin is -3 * dt");
    }

    /* 0x80059488: a materialising item ramps up and announces itself. */
    {
        q2_entity *m;

        place.id = 45;           /* Bluekey P — the only record with the bit */
        m = q2_item_spawn(&set, &place, NULL, 0, NULL);
        check(m != NULL, "Bluekey P spawns");
        check_eq_i(m->scale, 0, "and starts at nothing");

        q2_ent_events_clear(&w.events);
        w.dt = 12;
        m->think(m, &w);
        check_eq_i(m->scale, 2 * 12, "and grows by 2 * dt");
        check(w.events.count > 0, "and plays a sound on the first tick");
        check_eq_i(w.events.e[0].sound, Q2_SND_TELEPORT,
                   "which is msc_tele1");

        /* Its glow is blue, and the light block writes only that channel. */
        check_eq_i(m->glow[0], 0, "no red");
        check_eq_i(m->glow[1], 0, "no green");
        check(m->glow[2] > 0, "and a blue channel that is lit");
    }

    q2_entity_set_free(&set);
}

/* ------------------------------------------------------------------------- */
static void wr16(u8 *p, s16 v)
{
    p[0] = (u8)((u16)v & 0xFFu);
    p[1] = (u8)((u16)v >> 8);
}

static void put_vertex(u8 *p, s16 x, s16 y, s16 z)
{
    wr16(p + 0, x);
    wr16(p + 2, y);
    wr16(p + 4, z);
    wr16(p + 6, 0);
    wr16(p + 8, 0);
    wr16(p + 10, 0);
}

static void test_shadow_packet(void)
{
    u8 image[64];
    q2_model model;
    q2_model_pose pose;
    q2_model_instance inst;
    q2_model_draw_stats stats;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    u16 selected[2] = { 0, 1 };
    psx_prim *p;

    printf("\nitem shadow (0x800783B8 / 0x800784CC)\n");

    memset(image, 0, sizeof(image));
    memset(&model, 0, sizeof(model));
    memset(&pose, 0, sizeof(pose));

    /* Two global storage vertices, then one four-byte part record. The pose
     * moves them before the X/Z extrema are accumulated. */
    put_vertex(image + 0,  -20, 0, -30);
    put_vertex(image + 12,  40, 0,  50);
    image[32 + 2] = 0;                    /* scratch-window base */
    image[32 + 3] = 2;                    /* storage vertex count */

    model.base          = image;
    model.size          = sizeof(image);
    model.hdr.num_parts = 1;
    model.hdr.num_verts = 2;
    model.hdr.num_faces = 0;
    model.hdr.ofs_verts = 0;
    model.hdr.ofs_parts = 32;
    model.scratch_size  = 2;

    pose.q[3] = Q2_ONE_12;                /* identity quaternion */
    pose.t[0] = 10;
    pose.t[2] = -5;

    if (psx_ot_init(&ot, 256, 8) != Q2_OK) {
        check(false, "shadow test allocates an ordering table");
        return;
    }

    q2_camera_default(&cam, 512, 248);
    cam.pos[1] = -1000;                   /* look down onto the floor */
    gte_init(&gte);
    gte_set_projection(&gte, cam.projection, 256, 124);

    q2_model_instance_init(&inst);
    inst.model               = &model;
    inst.pose                = &pose;
    inst.origin[2]           = 4000;
    inst.shadow_enabled      = true;
    inst.shadow_vertex       = selected;
    inst.shadow_vertex_count = 2;
    inst.shadow_origin[2]    = 4000;

    psx_ot_clear(&ot);
    check_eq_i(q2_model_build_ot(&inst, &cam, &ot, &gte, &stats), 1,
               "a posed vertex list emits one primitive");
    check_eq_i(stats.faces_emitted, 0, "the synthetic model has no faces");
    check_eq_i(stats.shadows_emitted, 1, "the emitted primitive is a shadow");
    check_eq_i(ot.prim_count, 1, "the shadow consumes one packet");

    p = &ot.prims[0];
    check_eq_i(p->kind, PSX_PRIM_FT4, "shadow is POLY_FT4");
    check(p->semi_transparent, "shadow sets ABE");
    check(p->textured_blend, "shadow modulates its texture");
    check(p->quad_zorder, "shadow packet keeps libgpu Z order");
    check_eq_i(p->rgb[0].r, 128, "shadow modulation red is 128");
    check_eq_i(p->rgb[0].g, 128, "shadow modulation green is 128");
    check_eq_i(p->rgb[0].b, 128, "shadow modulation blue is 128");
    check_eq_i(p->tpage, psx_make_tpage(0, 1, PSX_BLEND_SUB, PSX_TEX_4BIT),
               "shadow samples slot 15's 4bpp page with ABR 2");
    check_eq_i(p->clut, psx_make_clut(0, 248),
               "shadow samples executable palette 1 at (0,248)");
    check_eq_i(p->uv[0].u, 224, "shadow uv0.u");
    check_eq_i(p->uv[0].v, 224, "shadow uv0.v");
    check_eq_i(p->uv[1].u, 239, "shadow uv1.u");
    check_eq_i(p->uv[1].v, 224, "shadow uv1.v");
    check_eq_i(p->uv[2].u, 224, "shadow uv2.u");
    check_eq_i(p->uv[2].v, 239, "shadow uv2.v");
    check_eq_i(p->uv[3].u, 239, "shadow uv3.u");
    check_eq_i(p->uv[3].v, 239, "shadow uv3.v");
    check(p->xy[0].x != p->xy[1].x && p->xy[0].y != p->xy[2].y,
          "posed extrema project to a non-degenerate floor quad");

    /* Height 600 is the exact retail cutoff; a zero-radius item with no list
     * likewise collapses and spends no primitive. */
    psx_ot_clear(&ot);
    inst.shadow_height = Q2_MODEL_SHADOW_FADE;
    check_eq_i(q2_model_build_ot(&inst, &cam, &ot, &gte, &stats), 0,
               "height 600 suppresses the shadow");
    check_eq_i(ot.prim_count, 0, "the height cutoff allocates no packet");

    psx_ot_clear(&ot);
    inst.shadow_height = 0;
    inst.shadow_vertex_count = 0;
    check_eq_i(q2_model_build_ot(&inst, &cam, &ot, &gte, &stats), 0,
               "zero radius plus no list is degenerate");

    /* A terminator ends the list even if the supplied capacity includes
     * later valid vertices; otherwise those vertices invent a footprint. */
    selected[0] = 0xFFFFu;
    inst.shadow_vertex_count = 2;
    check_eq_i(q2_model_build_ot(&inst, &cam, &ot, &gte, &stats), 0,
               "shadow vertices stop at the first FFFF");

    psx_ot_free(&ot);
}

static void test_touch_sweep(void)
{
    q2_entity_set set;
    q2_entity_world w;
    q2_inventory inv;
    q2_pop_place place;
    q2_entity *e;
    s32 pos[3] = { 0, 0, 0 };

    printf("\nthe touch sweep (0x80059810)\n");

    memset(&set, 0, sizeof(set));
    world_with_player(&w, &inv);

    memset(&place, 0, sizeof(place));
    place.id = 27;               /* Shells P */
    e = q2_item_spawn(&set, &place, NULL, 0, NULL);
    check(e != NULL, "Shells P spawns");

    /* Out of reach: nothing happens. */
    pos[0] = 100000;
    q2_entity_world_move_player(&w, 0, pos);
    e->think(e, &w);
    check(e->in_use, "an item out of reach survives");
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 0, "and gives nothing");

    /* In reach, single player: collected and freed. */
    pos[0] = 0;
    q2_entity_world_move_player(&w, 0, pos);
    e->think(e, &w);
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 10, "walking over it collects it");
    check(!e->in_use, "and in single player the entity is freed");

    /* A dead player collects nothing (0x8005983C). */
    memset(&set, 0, sizeof(set));
    world_with_player(&w, &inv);
    inv.health = 0;
    e = q2_item_spawn(&set, &place, NULL, 0, NULL);
    q2_entity_world_move_player(&w, 0, pos);
    e->think(e, &w);
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 0, "a dead player collects nothing");
    check(e->in_use, "and the item stays");

    /* Deathmatch: hidden, then back after 9000 ticks (0x800598FC). */
    memset(&set, 0, sizeof(set));
    world_with_player(&w, &inv);
    w.deathmatch = true;
    e = q2_item_spawn(&set, &place, NULL, 0, NULL);
    q2_entity_world_move_player(&w, 0, pos);
    e->think(e, &w);
    check(e->in_use, "in deathmatch the entity survives");
    check(e->taken[0], "but is taken");
    check_eq_i(e->respawn_at, Q2_ITEM_RESPAWN_TICKS, "with a 9000-tick timer");

    /* Run the clock down, with the player stepped off it — otherwise it is
     * collected again the tick after it returns, which is also correct and
     * would hide the thing being tested. */
    {
        int i;
        pos[0] = 100000;
        q2_entity_world_move_player(&w, 0, pos);
        w.dt = 300;
        for (i = 0; i < 31; i++)
            e->think(e, &w);
        check(!e->taken[0], "and comes back when the timer expires");
        check(!e->hidden, "and is visible again");
    }

    q2_entity_set_free(&set);
}

/*
 * 0x8005998C — the two-instruction branch weapons-stay takes. It bursts and
 * returns, so the gun is still standing there for the next player; the port
 * used to hide it before the early-out and the rule did nothing but leave a
 * live entity where a freed one belonged.
 */
static void test_weapons_stay(void)
{
    q2_entity_set set;
    q2_entity_world w;
    q2_inventory inv;
    q2_pop_place place;
    q2_entity *e;
    q2_model_bank zone_bank;
    u32 event_index;
    bool saw_burst = false;
    s32 pos[3] = { 0, 0, 0 };

    printf("\nweapons-stay keeps the gun VISIBLE (0x8005998C)\n");

    memset(&set, 0, sizeof(set));
    world_with_player(&w, &inv);
    w.deathmatch   = true;
    w.weapons_stay = true;

    memset(&place, 0, sizeof(place));
    place.id = 39;                       /* Shotgun P, effect 2 */
    e = q2_item_spawn(&set, &place, NULL, 0, NULL);
    check(e != NULL, "Shotgun P spawns");
    memset(&zone_bank, 0, sizeof(zone_bank));
    e->model_bank = &zone_bank;
    e->model_index = 7;

    q2_entity_world_move_player(&w, 0, pos);
    e->think(e, &w);

    check(q2_inventory_has_weapon(&inv, Q2_WEAPON_SHOTGUN), "the gun is taken");
    check(e->in_use, "and the entity survives");
    check(!e->hidden, "and is STILL VISIBLE — the point of the rule");
    check(!e->taken[0], "and nothing marks it taken");
    check_eq_i(e->respawn_at, 0, "and no respawn timer is armed");
    for (event_index = 0; event_index < w.events.count; event_index++) {
        const q2_ent_event *event = &w.events.e[event_index];
        if (event->kind != Q2_ENT_EVENT_BURST)
            continue;
        saw_burst = true;
        check(event->model_bank == &zone_bank,
              "pickup burst keeps the item's zone model bank");
        check_eq_i(event->model_index, 7,
                   "pickup burst keeps the bank-local model index");
    }
    check(saw_burst, "collecting the visible item emits its burst");

    /* Standing on it a second time gives nothing: give_weapon refuses, the
     * dispatch returns 2, and the sweep moves on. */
    inv.ammo[Q2_AMMO_SHELLS] = 100;
    e->think(e, &w);
    check(e->in_use && !e->hidden, "a second touch changes nothing");

    /* Without weapons-stay the same pickup folds back to an ordinary one. */
    memset(&set, 0, sizeof(set));
    world_with_player(&w, &inv);
    w.deathmatch   = true;
    w.weapons_stay = false;
    e = q2_item_spawn(&set, &place, NULL, 0, NULL);
    q2_entity_world_move_player(&w, 0, pos);
    e->think(e, &w);
    check(e->hidden, "weapons-stay off hides it");
    check(e->taken[0], "and marks it taken");
}

/*
 * 0x800359C0's prologue and the 57-name table it indexes.
 */
static void test_pickup_caption(void)
{
    q2_entity_world w;
    q2_inventory inv;
    const char *name;
    u8 icon;

    printf("\nthe pickup caption (0x800359C0, names at 0x800AC144)\n");

    /* The table itself: the caption is NOT the item record's model name. */
    check_str(q2_item_display_name(NULL, 3), "Super Shotgun",
              "effect 3 reads out as Super Shotgun, not Sshotgun P");
    check_str(q2_item_display_name(NULL, 18), "Shells", "effect 18 is Shells");
    check_str(q2_item_display_name(NULL, 34), "Health", "effect 34 is Health");
    check_str(q2_item_display_name(NULL, 35), "Health",
              "and so is 35 — one pointer, twice");
    check_str(q2_item_display_name(NULL, 50), "Purple Pyramid Key",
              "the longest name survives the field");
    check_str(q2_item_display_name(NULL, 0), "", "effect 0 is the empty name");
    check_str(q2_item_display_name(NULL, 999), "",
              "and an index off the end is too");

    world_with_player(&w, &inv);

    /* Nothing collected: the sub-draw takes its early-out at 0x80035A28. */
    icon = 0xFF;
    name = NULL;
    check(!q2_item_pickup_caption(&inv, w.level_time, NULL, &icon, &name),
          "with nothing collected there is no caption");

    /* Collect one. The touch writes both halves at 0x800372F0. */
    q2_item_touch(18, &inv, w.player[0].pos, &w);
    check_eq_i(inv.last_item, 18, "the touch records the effect");
    check_eq_i(inv.item_name_until, w.level_time + Q2_ITEM_CAPTION_TICKS,
               "and a 900-tick deadline");

    check(q2_item_pickup_caption(&inv, w.level_time, NULL, &icon, &name),
          "which the caption reads");
    check_eq_i(icon, 18, "the icon is the effect id used as a rect index");
    check_str(name, "Shells", "and the name is that effect's");

    /* It survives the tick it expires ON — `sltu deadline, now`. */
    check(q2_item_pickup_caption(&inv, inv.item_name_until, NULL, &icon, &name),
          "it is still drawn on the deadline tick");
    check_eq_i(icon, 18, "with its icon");

    /* One tick later the expiry runs INSIDE the draw: cleared, then reloaded,
     * so the frame it dies on still draws — blank rect, empty name. */
    check(q2_item_pickup_caption(&inv, inv.item_name_until + 1, NULL,
                                 &icon, &name),
          "the frame it dies on still draws");
    check_eq_i(icon, 0, "with the blank rect");
    check_str(name, "", "and the empty name");
    check_eq_i(inv.last_item, 0, "and last_item is cleared in place");

    /* And the frame after that takes the early-out. */
    check(!q2_item_pickup_caption(&inv, inv.item_name_until + 2, NULL,
                                  &icon, &name),
          "and the frame after draws nothing at all");
}

static void test_timed_removal(void)
{
    q2_entity_set set;
    q2_entity_world w;
    q2_inventory inv;
    q2_pop_place place;
    q2_entity *e;
    s32 away[3] = { 100000, 0, 0 };
    int i;

    printf("\ntimed removal (0x800597C8 / 0x8005B358)\n");

    memset(&set, 0, sizeof(set));
    world_with_player(&w, &inv);
    q2_entity_world_move_player(&w, 0, away);   /* nobody standing on it */

    memset(&place, 0, sizeof(place));
    place.id = 27;
    e = q2_item_spawn(&set, &place, NULL, 0, NULL);
    check_eq_i(e->remove_in, Q2_ITEM_DROP_LIFE,
               "an item spawns with a 1500-tick life");

    /* The flag is never set on disc; the runtime sets it on a dropped item. */
    e->flags |= Q2_ITEM_TIMED;
    w.dt = 300;
    for (i = 0; i < 20 && e->think == q2_item_think; i++)
        e->think(e, &w);
    check_eq_i(i, 5, "the 1500-tick life expires after five 300-tick steps");
    check(e->think == q2_item_shrink_think,
          "the countdown switches the think to the shrink");
    check_eq_i(e->scale, 4096, "and resets the scale to full");

    /* 4096 / 16 == 256 ticks of shrink, so a 64-tick step takes four. */
    w.dt = 64;
    for (i = 0; i < 8 && e->in_use; i++)
        e->think(e, &w);
    check(!e->in_use, "which then shrinks it away");
    check_eq_i(i, 4, "in 4096 / (16 * 64) steps");

    q2_entity_set_free(&set);
}

static void test_entity_set(void)
{
    q2_entity_set set;
    q2_entity *a, *b, *c;

    printf("\nthe entity set\n");

    memset(&set, 0, sizeof(set));

    a = q2_entity_alloc(&set);
    b = q2_entity_alloc(&set);
    check(a && b && a != b, "two allocations are distinct");
    check_eq_i(set.count, 2, "and both are live");

    q2_entity_remove(a);
    c = q2_entity_alloc(&set);
    check(c == a, "a freed slot is reused (0x8006C098)");
    check_eq_i(set.count, 2, "without growing the set");

    /* An entity with no think is skipped rather than crashing the sweep. */
    {
        q2_entity_world w;
        q2_inventory inv;
        world_with_player(&w, &inv);
        check_eq_i(q2_entity_run(&set, &w), 0, "thinkless entities run nothing");
        check_eq_i(w.level_time, w.dt, "but the clock still advances");
    }

    q2_entity_set_free(&set);
}

/* ------------------------------------------------------------------------- */
/*
 * Which groups a zone load spawns.
 *
 * A group is spawned because a script SELECTED it by name (0x80056C60 sets bit
 * 0 of its flags word; the pass at 0x80056D68 runs only what has bit 0 set and
 * bit 1 clear). `q2_levelbin_selected` reads those calls from the level module,
 * so a category or script-batch group appears only when selected. A group whose
 * NAME claims the resident zone remains the fallback, as it does for creatures.
 *
 * The population is built by hand rather than read off a disc, so this runs in
 * the self-contained suite and pins the rule rather than one map's data.
 */
static void put_u32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static void put_u16(u8 *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void put_group(u8 *buf, u32 at, const char *name, u32 place_offset)
{
    memset(buf + at, 0, Q2_POP_GROUP_SIZE);
    memcpy(buf + at, name, strlen(name));
    put_u32(buf + at + 0x0C, 0);              /* no spawn list */
    put_u32(buf + at + 0x10, place_offset);
}

/* `count` places then the list's own 0xFFFFFFFF terminator. */
static void put_places(u8 *buf, u32 at, u32 count, u16 id)
{
    u32 i;
    for (i = 0; i < count; i++) {
        u8 *p = buf + at + i * Q2_POP_PLACE_SIZE;
        memset(p, 0, Q2_POP_PLACE_SIZE);
        p[14] = (u8)id;
        p[15] = (u8)(id >> 8);
    }
    put_u32(buf + at + count * Q2_POP_PLACE_SIZE, Q2_POP_TERM_FFFF);
}

/* One synthetic `select(name)` call in the exact instruction shape decoded by
 * q2_levelbin_selected: form a pointer to a twelve-byte field, load import slot
 * +36, then call it through the loaded register. */
static void put_select(u8 *buf, u32 call_at, u32 name_at, const char *name)
{
    memset(buf + name_at, 0, 12);
    memcpy(buf + name_at, name, strlen(name));
    put_u32(buf + call_at + 0,
            0x24040000u | name_at);              /* addiu a0,zero,name_at */
    put_u32(buf + call_at + 4,
            0x8C620000u | Q2_LEVELBIN_SLOT_SELECT); /* lw v0,+36(v1)      */
    put_u32(buf + call_at + 8, 0x00400009u);      /* jalr v0              */
    put_u32(buf + call_at + 12, 0);               /* delay slot            */
}

static u32 spawn_count(const q2_population *pop, int zone,
                       const u8 *levelbin, u32 levelbin_size,
                       q2_item_spawn_stats *out)
{
    q2_entity_set set;
    q2_item_spawn_stats st;
    u32 n;

    memset(&set, 0, sizeof(set));
    q2_item_spawn_zone(&set, pop, zone, levelbin, levelbin_size,
                       NULL, NULL, NULL, &st);
    n = set.count;
    if (out)
        *out = st;
    q2_entity_set_free(&set);
    return n;
}

static void test_zone_groups(void)
{
    /* Four groups: two named after zones and two category/script batches. */
    enum { P0 = 112, P1 = P0 + 2 * 16 + 4, P2 = P1 + 2 * 16 + 4,
           P3 = P2 + 1 * 16 + 4, SIZE = P3 + 1 * 16 + 4 };
    u8 buf[SIZE];
    u8 levelbin[96];
    q2_population pop;
    q2_pop_group g;
    q2_item_spawn_stats st;

    printf("zone groups\n");

    memset(buf, 0, sizeof(buf));
    put_group(buf,  0, "Zone0",   P0);
    put_group(buf, 24, "Zone1",   P1);
    put_group(buf, 48, "Weapons", P2);
    put_group(buf, 72, "Ammo",    P3);
    put_u32(buf + 96, 0);                     /* the group table's terminator */
    put_places(buf, P0, 2, 27);               /* Shells P */
    put_places(buf, P1, 2, 27);
    put_places(buf, P2, 1, 27);
    put_places(buf, P3, 1, 27);

    pop.data        = buf;
    pop.size        = SIZE;
    pop.group_count = 4;

    /* The module selects one unzoned category and one group belonging to zone
     * 1. The latter must not leak across the resident-zone boundary. */
    memset(levelbin, 0, sizeof(levelbin));
    put_select(levelbin, 0, 64, "Weapons");
    put_select(levelbin, 16, 80, "Zone1");

    /* The name rule itself. */
    memset(&g, 0, sizeof(g));
    snprintf(g.name, sizeof(g.name), "%s", "Zone0");
    check_eq_i(q2_pop_group_zone(&g), 0, "'Zone0' names zone 0");
    check(q2_pop_group_is_zone(&g), "Zone0 is startup population");
    snprintf(g.name, sizeof(g.name), "%s", "Zone4");
    check_eq_i(q2_pop_group_zone(&g), 4, "'Zone4' names zone 4");
    /* A suffix is a batch WITHIN that zone, so the digits still decide. */
    snprintf(g.name, sizeof(g.name), "%s", "Zone1bob");
    check_eq_i(q2_pop_group_zone(&g), 1, "'Zone1bob' names zone 1");
    check(!q2_pop_group_is_zone(&g), "Zone1bob is a deferred batch");
    snprintf(g.name, sizeof(g.name), "%s", "Zone2_1");
    check_eq_i(q2_pop_group_zone(&g), 2, "'Zone2_1' names zone 2");
    check(!q2_pop_group_is_zone(&g), "Zone2_1 is a deferred batch");
    snprintf(g.name, sizeof(g.name), "%s", "zone3");
    check_eq_i(q2_pop_group_zone(&g), 3, "the match is case-insensitive");
    check(q2_pop_group_is_zone(&g), "the exact lowercase zone also starts");
    /* The deathmatch maps' groups, which partition by category instead. */
    snprintf(g.name, sizeof(g.name), "%s", "Weapons");
    check_eq_i(q2_pop_group_zone(&g), -1, "'Weapons' names no zone");
    snprintf(g.name, sizeof(g.name), "%s", "PathCorner");
    check_eq_i(q2_pop_group_zone(&g), -1, "'PathCorner' names no zone");
    snprintf(g.name, sizeof(g.name), "%s", "Zone");
    check_eq_i(q2_pop_group_zone(&g), -1, "'Zone' with no digits names none");

    /* A resident zone gets its own group plus the unzoned group the LevelBin
     * selected — not every category that happens to have a place list. */
    check_eq_i(spawn_count(&pop, 0, levelbin, sizeof(levelbin), &st), 3,
               "zone 0 spawns its 2 plus selected Weapons");
    check_eq_i(st.other_zone, 1, "zone 0 rejects Zone1 even though it was selected");
    check_eq_i(st.not_selected, 1, "zone 0 skips unselected Ammo");

    check_eq_i(spawn_count(&pop, 1, levelbin, sizeof(levelbin), &st), 3,
               "zone 1 spawns its 2 plus selected Weapons");
    check_eq_i(st.other_zone, 1, "zone 1 rejects Zone0");
    check_eq_i(st.not_selected, 1, "zone 1 skips unselected Ammo");

    check_eq_i(spawn_count(&pop, 2, levelbin, sizeof(levelbin), &st), 1,
               "a zone with no group of its own gets selected Weapons only");
    check_eq_i(st.other_zone, 2, "zone 2 skips both zone-named groups");
    check_eq_i(st.not_selected, 1, "zone 2 still skips unselected Ammo");

    /* Without a module, match the creature world's conservative fallback: the
     * resident Zone<N> group is known; unzoned batches are not guessed live. */
    check_eq_i(spawn_count(&pop, 0, NULL, 0, &st), 2,
               "without LevelBin only the resident zone group spawns");
    check_eq_i(st.not_selected, 2, "both unzoned groups are skipped");

    /* A suffix carries ownership only. The same rule must reach items as
     * creatures, including a script batch explicitly selected by LevelBin. */
    put_group(buf, 72, "Zone0Room2", P3);
    check_eq_i(spawn_count(&pop, 0, NULL, 0, &st), 2,
               "a suffixed resident batch does not appear at load");
    memset(levelbin, 0, sizeof(levelbin));
    put_select(levelbin, 0, 64, "Zone0Room2");
    check_eq_i(spawn_count(&pop, 0, levelbin, sizeof(levelbin), &st), 3,
               "explicit selection can still start a suffixed batch");
    check_eq_i(spawn_count(&pop, 1, levelbin, sizeof(levelbin), &st), 2,
               "explicit selection cannot cross the zone boundary");
    put_group(buf, 72, "Ammo", P3);
    memset(levelbin, 0, sizeof(levelbin));
    put_select(levelbin, 0, 64, "Weapons");
    put_select(levelbin, 16, 80, "Zone1");

    {
        u32 selected[2] = { 0, 0xDEADBEEFu };
        check_eq_i(q2_levelbin_selected(levelbin, sizeof(levelbin), selected, 1),
                   1, "selector returns the number actually stored");
        check_eq_i(selected[1], 0xDEADBEEFu,
                   "selector never writes past its output capacity");
        check_eq_i(q2_levelbin_selected(levelbin, sizeof(levelbin), selected, 0),
                   0, "zero-capacity selection cannot expose unstored offsets");
    }

    /* -1 is the census: every group, whichever zone it names. */
    check_eq_i(spawn_count(&pop, -1, levelbin, sizeof(levelbin), &st), 6,
               "zone -1 spawns every group");
    check_eq_i(st.other_zone, 0, "zone -1 skips no other-zone group");
    check_eq_i(st.not_selected, 0, "zone -1 skips no unselected group");

    /* The mutable group flag's bit-1 shadow spans both passes: startup marks
     * what it ran, a later CREBATCH can add a deferred group, and neither kind
     * can run the same place list twice. */
    {
        q2_entity_set set;
        u8 group_run[4] = { 0, 0, 0, 0 };

        memset(&set, 0, sizeof(set));
        check_eq_i(q2_item_spawn_zone(&set, &pop, 0, levelbin,
                                      sizeof(levelbin), NULL, NULL,
                                      group_run, &st), Q2_OK,
                   "the latched startup pass succeeds");
        check_eq_i(set.count, 3, "the latched pass spawns the same selection");
        check(group_run[0] && !group_run[1] && group_run[2] && !group_run[3],
              "only resident Zone0 and selected Weapons are marked run");
        check_eq_i(set.ent[0].population_group, 0,
                   "a startup item keeps its Population group identity");
        check_eq_i(set.ent[1].population_slot, 1,
                   "and its stable place-list slot");
        check_eq_i(set.ent[2].population_group, 2,
                   "the selected batch is keyed independently of set order");

        check_eq_i(q2_item_spawn_group(&set, &pop, "Ammo", NULL, NULL,
                                       group_run, &st), Q2_OK,
                   "CREBATCH can run the deferred Ammo group");
        check_eq_i(st.spawned, 1, "the deferred group reports its one item");
        check_eq_i(set.count, 4, "the deferred item joins the entity set");
        check(group_run[3] != 0, "the deferred group is latched as run");
        check_eq_i(set.ent[3].population_group, 3,
                   "the deferred item carries the group CREBATCH named");
        check_eq_i(set.ent[3].population_slot, 0,
                   "and its slot within that group's place list");

        check_eq_i(q2_item_spawn_group(&set, &pop, "Ammo", NULL, NULL,
                                       group_run, &st), Q2_OK,
                   "repeating CREBATCH is a successful no-op");
        check_eq_i(st.spawned, 0, "the repeated group reports no new item");
        check_eq_i(set.count, 4, "the repeated group cannot duplicate items");

        check_eq_i(q2_item_spawn_group(&set, &pop, "Weapons", NULL, NULL,
                                       group_run, &st), Q2_OK,
                   "a startup group also rejects later activation");
        check_eq_i(set.count, 4, "startup items cannot be duplicated either");
        q2_entity_set_free(&set);
    }
}

static void test_place_difficulty_flags(void)
{
    enum { PLACE = 28, COUNT = 4,
           SIZE = PLACE + COUNT * Q2_POP_PLACE_SIZE + 4 };
    static const u16 flags[COUNT] = {
        Q2_POP_PLACE_UNUSED_1000,
        Q2_POP_PLACE_NOT_EASY,
        Q2_POP_PLACE_NOT_MEDIUM,
        Q2_POP_PLACE_NOT_HARD
    };
    u8 buf[SIZE];
    q2_population pop;
    q2_item_spawn_stats st;
    u32 i;

    printf("place difficulty flags (0x8007F538)\n");

    memset(buf, 0, sizeof(buf));
    put_group(buf, 0, "Zone0", PLACE);
    put_u32(buf + Q2_POP_GROUP_SIZE, 0);
    put_places(buf, PLACE, COUNT, 27); /* Shells P */
    for (i = 0; i < COUNT; i++)
        put_u16(buf + PLACE + i * Q2_POP_PLACE_SIZE + 12, flags[i]);

    pop.data        = buf;
    pop.size        = SIZE;
    pop.group_count = 1;

    check(q2_pop_place_allows_skill(Q2_POP_PLACE_UNUSED_1000, 0),
          "bit 12 does not exclude easy");
    check(q2_pop_place_allows_skill(Q2_POP_PLACE_UNUSED_1000, 1),
          "bit 12 does not exclude medium");
    check(q2_pop_place_allows_skill(Q2_POP_PLACE_UNUSED_1000, 2),
          "bit 12 does not exclude hard");

    for (i = 0; i < 3; i++) {
        q2_cre_set_skill((s32)i);
        check_eq_i(spawn_count(&pop, 0, NULL, 0, &st), 3,
                   i == 0 ? "easy skips NOT_EASY only" :
                   i == 1 ? "medium skips NOT_MEDIUM only" :
                            "hard skips NOT_HARD only");
        check_eq_i(st.skill_filtered, 1,
                   "the rejected place is counted separately");
    }

    /* Exact default arm of 0x8007F57C: an out-of-menu skill is unfiltered. */
    q2_cre_set_skill(3);
    check_eq_i(spawn_count(&pop, 0, NULL, 0, &st), 4,
               "an out-of-range skill takes retail's unfiltered default");
    check_eq_i(st.skill_filtered, 0,
               "the unfiltered default rejects no place");
    q2_cre_set_skill(1);
}

/* ------------------------------------------------------------------------- */
/* 0x8006CC44 / 0x8006D6AC — a posed vertex in the world                      */
/* ------------------------------------------------------------------------- */
/*
 * Here and not in test_model.c, which links only the formats library:
 * modeldraw.c is in the game library, and this suite links that.
 *
 * Two parts so the GLOBAL index walk is exercised: part 0 owns storage
 * vertices 0 and 1, part 1 owns vertex 2, and only part 1's pose moves it. A
 * half turn is used for the rotation because Ry(180) is diag(-1, 1, -1) under
 * either sign convention, so the expectation does not borrow the one under
 * test.
 */
static void test_world_vertex(void)
{
    u8 image[64];
    q2_model model;
    q2_model_pose pose[2];
    q2_model_instance inst;
    s32 v[3];
    static const s16 swap_xz[3][3] = {
        {     0, 0, 4096 },
        {     0, 4096, 0 },
        { -4096, 0,    0 }
    };

    printf("\nthe world vertex (0x8006CC44) and the vertex total (0x8006D6AC)\n");

    memset(image, 0, sizeof(image));
    memset(&model, 0, sizeof(model));
    memset(pose, 0, sizeof(pose));

    put_vertex(image + 0,  -20,  0, -30);
    put_vertex(image + 12,  40,  0,  50);
    put_vertex(image + 24, 100, -7,   5);
    /* Part 1's scratch-window base overlaps part 0's, as real models' do: the
     * STORAGE index is what 0x8006CC80 walks, and the window base must not
     * enter into it. */
    image[48 + 2] = 0;  image[48 + 3] = 2;    /* part 0: slots 0..1, 2 verts */
    image[56 + 2] = 0;  image[56 + 3] = 1;    /* part 1: slot 0, 1 vert      */

    model.base          = image;
    model.size          = sizeof(image);
    model.hdr.num_parts = 2;
    model.hdr.num_verts = 3;
    model.hdr.ofs_verts = 0;
    model.hdr.ofs_parts = 48;
    model.scratch_size  = 3;

    pose[0].q[3] = Q2_ONE_12;
    pose[1].q[3] = Q2_ONE_12;
    pose[1].t[0] = 10;
    pose[1].t[1] = 20;
    pose[1].t[2] = -5;

    check_eq_i(q2_model_total_verts(&model), 3,
               "the total is the SUM of each part's +3 (2 + 1)");
    check_eq_i(q2_model_total_verts(NULL), 0,
               "a model-less entity has none (0x8006D6AC beq a0, zero)");

    q2_model_instance_init(&inst);
    inst.model     = &model;
    inst.pose      = pose;
    inst.origin[0] = 1000;
    inst.origin[1] = 2000;
    inst.origin[2] = 3000;

    check(q2_model_world_vertex(&inst, -1, v), "a negative index answers");
    check(v[0] == 1000 && v[1] == 2000 && v[2] == 3000,
          "with the origin itself (0x8006CC64 bltz -> +0xA4)");

    check(q2_model_world_vertex(&inst, 0, v), "vertex 0 answers");
    check(v[0] == 980 && v[1] == 2000 && v[2] == 2970,
          "part 0's vertex is its own position plus the origin");

    check(q2_model_world_vertex(&inst, 2, v), "vertex 2 answers");
    check(v[0] == 1110 && v[1] == 2013 && v[2] == 3000,
          "the GLOBAL index finds part 1, whose pose moves it by (10,20,-5)");

    check(!q2_model_world_vertex(&inst, 3, v), "past the last vertex refuses");

    inst.yaw = 2048;
    check(q2_model_world_vertex(&inst, 2, v), "vertex 2 at a half turn");
    check(v[0] == 890 && v[1] == 2013 && v[2] == 3000,
          "is x and z negated about the origin, y untouched");

    inst.yaw = 0;
    inst.rot = swap_xz;
    check(q2_model_world_vertex(&inst, 2, v), "vertex 2 under an explicit rot");
    check(v[0] == 1000 && v[1] == 2013 && v[2] == 3000 - 110,
          "rot overrides the angles, row by row: (z, y, -x)");

    inst.rot  = NULL;
    inst.pose = NULL;
    check(q2_model_world_vertex(&inst, 2, v), "an unposed instance answers");
    check(v[0] == 1100 && v[1] == 1993 && v[2] == 3005,
          "with the stored vertex, as the unposed draw does");
}

/* ------------------------------------------------------------------------- */
/* A gib is drawn through all three of its angles (+0xE6 -> +0x2C0)           */
/* ------------------------------------------------------------------------- */
/*
 * The console draws an entity through the matrix at +0x2C0 (0x8006BB28), and
 * a gib rebuilds it as RotMatrix(+0xE6) at its spawn and on every toss
 * (0x8005A31C, 0x80046CA8). The entity draw used to hand the model only +0xE8,
 * so a chunk kept its heading and lost its tumble.
 *
 * The first expectation is worked from RotMatrix's own element list
 * (0x80089E38, transcribed in trig.c) at quarter turns, where every sine and
 * cosine is exactly 0 or 4096, so it does not borrow the function the draw
 * calls. The second pins the heading's SENSE against the yaw-only path every
 * other entity is drawn with (modeldraw.c's instance_spin builds it as
 * q2_rotation_yaw_pitch(-yaw, 0)): a chunk that turned the other way from the
 * item beside it would be the mirrored-yaw mistake the instance's own
 * pitch/roll fields make. The third is the promise that nothing else changes.
 */
static void mat_fill(s16 m[3][3], s16 v)
{
    int r, c;

    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            m[r][c] = v;
}

static bool mat_same(const s16 a[3][3], const s16 b[3][3])
{
    int r, c;

    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            if (a[r][c] != b[r][c])
                return false;
    return true;
}

static void test_gib_draw_rotation(void)
{
    static const s16 pitch_roll[3][3] = {
        /*
         * sx = sz = 4096, cx = cz = 0, sy = 0, cy = 4096:
         *   m00 =  cy*cz + sy*sx*sz = 0      m01 = -cy*sz + sy*sx*cz = -4096
         *   m02 =  sy*cx            = 0      m10 =  sz*cx            = 0
         *   m11 =  cz*cx            = 0      m12 = -sx               = -4096
         *   m20 = -sy*cz + cy*sx*sz = 4096   m21 =  sy*sz + cy*sx*cz = 0
         *   m22 =  cy*cx            = 0
         */
        {    0, -4096,     0 },
        {    0,     0, -4096 },
        { 4096,     0,     0 }
    };
    q2_entity e;
    s16 m[3][3], yaw_only[3][3], untouched[3][3];
    s32 yaw;
    u32 drawn = 0, agree = 0;

    printf("\na gib's tumble reaches the draw (RotMatrix(+0xE6) -> +0x2C0)\n");

    /* A chunk as 0x8005A0AC leaves it: the flags 0x8005A1A4 writes, and a
     * quarter turn of pitch (+0xE6) and of roll (+0xEA). */
    q2_entity_init(&e);
    e.render_flags = Q2_GIB_RENDER_FLAGS;
    e.angles[0]    = 1024;
    e.angles[1]    = 0;
    e.angles[2]    = 1024;
    mat_fill(m, 0x5555);
    check(q2_entity_draw_rotation(&e, m),
          "a gib is drawn through its own matrix, not the yaw alone");
    check(mat_same((const s16 (*)[3])m, pitch_roll),
          "which is RotMatrix of all three angles: rows (0,-1,0) (0,0,-1) "
          "(1,0,0) for a quarter turn of pitch and of roll");

    /* Every heading, with no pitch or roll: the chunk turns exactly as the
     * yaw-only path turns everything else. */
    e.angles[0] = 0;
    e.angles[2] = 0;
    for (yaw = 0; yaw < 4096; yaw++) {
        e.angles[1] = yaw;
        if (!q2_entity_draw_rotation(&e, m))
            continue;
        drawn++;
        q2_rotation_yaw_pitch(yaw_only, -yaw, 0);
        if (mat_same((const s16 (*)[3])m, (const s16 (*)[3])yaw_only))
            agree++;
    }
    check_eq_i(drawn, 4096, "a gib takes its own matrix at every heading");
    check_eq_i(agree, 4096,
               "and its heading turns the same way, to the unit, as the yaw "
               "path every other entity is drawn with");

    /* An item (q2_entity_init's 0x08000001, no transient bit) keeps the yaw
     * path however its angles read, and the matrix it was handed is left as
     * it was. */
    q2_entity_init(&e);
    e.angles[0] = 1024;
    e.angles[1] = 1000;
    e.angles[2] = 1024;
    mat_fill(m, 0x5555);
    mat_fill(untouched, 0x5555);
    check(!q2_entity_draw_rotation(&e, m),
          "an item is not a transient and keeps the yaw-only draw");
    check(mat_same((const s16 (*)[3])m, (const s16 (*)[3])untouched),
          "and the matrix it was handed is not written");
}

/* ------------------------------------------------------------------------- */
/* 0x800596B0 — the materialise burst, spawned without moving the stream      */
/* ------------------------------------------------------------------------- */
static q2_fx_tables g_fx_tab;

static void build_fx_tables(void)
{
    u32 i, c;

    memset(&g_fx_tab, 0, sizeof(g_fx_tab));
    g_fx_tab.loaded = true;
    for (i = 0; i < Q2_FX_RAMP_COUNT; i++) {
        g_fx_tab.ramp_id_to_index[i] = (u8)i;
        for (c = 0; c < Q2_FX_RAMP_COLOURS; c++)
            g_fx_tab.ramp[i].colour[c] = (u32)i << 8;   /* g = the ramp */
    }
    g_fx_tab.ramp_index_is_permutation = true;
}

static u32 fx_live_groups(const q2_fx_world *fx, const q2_fx_group **last)
{
    u32 i, n = 0;

    for (i = 0; i < fx->group_count; i++) {
        if (fx->group[i].life) {
            n++;
            if (last)
                *last = &fx->group[i];
        }
    }
    return n;
}

static void test_materialise_burst(void)
{
    q2_entity_set set_a, set_b;
    q2_entity_world wa, wb;
    q2_inventory inva, invb;
    q2_pop_place place;
    q2_entity *a, *b;
    q2_rng ref;
    q2_fx_world *fx;
    q2_item_env env;
    const q2_fx_group *g = NULL;
    s32 away[3] = { 100000, 0, 0 };
    int i;

    printf("\nthe materialise burst (0x80059608 / 0x800596B0)\n");

    fx = (q2_fx_world *)calloc(1, sizeof(*fx));
    if (!fx) {
        check(false, "the test allocates an effect world");
        return;
    }
    build_fx_tables();
    q2_fx_world_init(fx, &g_fx_tab);

    memset(&set_a, 0, sizeof(set_a));
    memset(&set_b, 0, sizeof(set_b));
    world_with_player(&wa, &inva);
    world_with_player(&wb, &invb);
    q2_entity_world_move_player(&wa, 0, away);
    q2_entity_world_move_player(&wb, 0, away);
    q2_rng_seed(&wa.rng, 0x1234);
    q2_rng_seed(&wb.rng, 0x1234);
    q2_rng_seed(&ref, 0x1234);

    memset(&place, 0, sizeof(place));
    place.id = 45;                       /* Bluekey P: MATERIALISE, glow B */

    /* A: no pool bound, which is how the port ran before. */
    q2_item_bind_env(NULL);
    a = q2_item_spawn(&set_a, &place, NULL, 0, NULL);
    check(a != NULL, "Bluekey P spawns");
    a->surface = 0x21;
    wa.dt = 12;
    a->think(a, &wa);

    /* B: the pool bound, which is what the client now does. */
    memset(&env, 0, sizeof(env));
    env.fx = fx;
    q2_item_bind_env(&env);
    b = q2_item_spawn(&set_b, &place, NULL, 0, NULL);
    b->surface = 0x21;
    wb.dt = 12;
    b->think(b, &wb);

    for (i = 0; i < Q2_ITEM_SPARKLES * 3; i++)
        (void)q2_rng_next(&ref);

    check_eq_i(wb.rng.state, wa.rng.state,
               "binding the pool leaves the generator where it was");
    check_eq_i(wb.rng.state, ref.state,
               "and both consumed exactly the 45 draws 0x80059608 makes");

    check_eq_i(fx_live_groups(fx, &g), 1,
               "and ONE group is now spawned where none was");
    if (g) {
        check_eq_i(g->count, 15, "of fifteen quads (0x80059690)");
        check_eq_i(g->life, 10, "life 10 (0x80059698)");
        check_eq_i(g->area, 0x21, "on the item's +0x9E (0x800596A8)");
        check(g->ramp[0] == q2_fx_ramp_at(&g_fx_tab, 0),
              "in ramp 0, which a GLOW_B item picks (0x80059670)");
        check_eq_i(g->origin[1], b->origin[1],
                   "at the draw origin (addiu a0, s2, 164)");
    }

    /* The next tick of a materialising item draws 45 more, burst and all. */
    b->think(b, &wb);
    for (i = 0; i < Q2_ITEM_SPARKLES * 3; i++)
        (void)q2_rng_next(&ref);
    check_eq_i(wb.rng.state, ref.state, "and 45 more on the next tick");

    q2_item_bind_env(NULL);
    q2_entity_set_free(&set_a);
    q2_entity_set_free(&set_b);
    free(fx);
}

/* ------------------------------------------------------------------------- */
/* 0x8002085C / 0x80020C48 — a creature's death drop                          */
/* ------------------------------------------------------------------------- */
static int g_rand_script[8];
static int g_rand_next;
static int g_rand_calls;

static int scripted_rand(void)
{
    g_rand_calls++;
    if (g_rand_next < (int)(sizeof(g_rand_script) / sizeof(g_rand_script[0])))
        return g_rand_script[g_rand_next++];
    return 0;
}

static void script_rand(int r1, int r2)
{
    g_rand_script[0] = r1;
    g_rand_script[1] = r2;
    g_rand_next      = 0;
    g_rand_calls     = 0;
}

static void test_drop_spawn(void)
{
    q2_entity_set set;
    q2_entity *e;
    const q2_item_def *def;
    q2_model_bank empty;
    const q2_model_bank *banks[1];
    s32 pos[3] = { 1000, -2000, 3000 };
    s16 vel[3];
    u32 before;

    printf("\na creature's drop (0x8002085C)\n");

    q2_item_drop_reset();
    q2_item_bind_env(NULL);
    memset(&set, 0, sizeof(set));

    def = q2_item_find(q2_item_table_builtin(), 27);
    check(def != NULL, "place id 27 is Shells P");

    script_rand(32767, 32767);
    e = q2_item_drop_spawn(&set, NULL, NULL, 0, 27, 0, pos, -1, scripted_rand);
    check(e != NULL, "a Soldier's Shells drop spawns");
    if (!e)
        return;
    check(e->think == q2_item_drop_think, "with the toss think 0x80020C48");
    check_eq_i(e->kind, Q2_ENT_KIND_ITEM, "as item kind 46 (0x800208C4)");
    check_eq_i(g_rand_calls, 2, "drawing exactly two rand()s");

    check(e->pos[0] == 1000 && e->pos[1] == -2000 && e->pos[2] == 3000,
          "at the recorded origin EXACTLY: no 316 lift, no floor sweep");
    check_eq_i(e->origin[1], -2000 - 286,
               "the spawn frame draws 286 + bias ABOVE it (0x800209A4)");
    check_eq_i(e->flags, (u32)(def->flags | Q2_ITEM_TIMED),
               "the record's flags with TIMED ORed in (0x80020B38)");
    check_eq_i(e->effect, def->effect, "and its touch index");
    check_eq_i(e->remove_in, 96, "+0xF4 is 96 while it flies (0x80020ADC)");
    check_eq_i(e->field90, 128, "+0x90 is 128 (0x80020BB4)");
    check_eq_i(e->render_flags, 0,
               "render flags stay the allocator's zero: no shadow");
    check_eq_i(e->glow[0], 0x40, "ambient 0x40 from 0x800AE718");
    check(e->angles[0] == 0 && e->angles[1] == 0 && e->angles[2] == 0,
          "angles zeroed (0x80020AC8)");
    check_eq_i(e->node, -1, "the cell handed in");
    check(e->bounds_min[0] == 1000 - 256 && e->bounds_max[0] == 1000 + 256 &&
          e->bounds_min[1] == -2000 - 512 && e->bounds_max[1] == -2000 &&
          e->bounds_min[2] == 3000 - 256 && e->bounds_max[2] == 3000 + 256,
          "the drop's own box, (-256, ext2-512, -256)..(256, ext2, 256)");

    check(q2_item_drop_velocity(e, vel), "it has a velocity");
    check_eq_i(vel[0], 1023,
               "heading 0 puts the whole speed on x: 256 + (32767*768 >> 15)");
    check_eq_i(vel[2], 0, "and none on z");
    check_eq_i(vel[1], -2303,
               "the kick (-3072 - 1535) halved toward zero: up, +Y being down");

    /* The other end of both ranges, heading a quarter turn round. */
    script_rand(0, 0);
    e = q2_item_drop_spawn(&set, NULL, NULL, 0, 27, 1024, pos, -1,
                           scripted_rand);
    check(e != NULL && q2_item_drop_velocity(e, vel), "a second drop");
    check(vel[0] == 0 && vel[2] == 256,
          "heading 1024 puts the base 256 on z (the FIRST halfword)");
    check_eq_i(vel[1], -1536, "and the least kick is -3072 / 2");

    /* A negative component that does not divide: truncated, not floored. */
    {
        s32 c = q2_cos12(2560);
        s32 want = -((-c * 257) / 4096);

        check(c < 0 && ((-c * 257) % 4096) != 0,
              "precondition: heading 2560's cosine is negative and inexact");
        script_rand(43, 0);            /* 43 * 768 >> 15 == 1: speed 257 */
        e = q2_item_drop_spawn(&set, NULL, NULL, 0, 27, 2560, pos, -1,
                               scripted_rand);
        check(e != NULL && q2_item_drop_velocity(e, vel), "a third drop");
        check_eq_i(vel[0], want,
                   "x rounds toward zero (the bgez/+4095 arm), not down");
    }

    /* Item 21 keeps its record's word (0x80020B1C). */
    def = q2_item_find(q2_item_table_builtin(), 21);
    script_rand(0, 0);
    e = q2_item_drop_spawn(&set, NULL, NULL, 0, 21, 0, pos, -1,
                           scripted_rand);
    check(def != NULL && e != NULL, "the Nuke drops");
    if (def && e)
        check_eq_i(e->flags, def->flags,
                   "item 21 keeps the record's flags: no TIMED");

    /* No record: nothing, and no random number (0x80020894). */
    before = set.count;
    script_rand(0, 0);
    check(q2_item_drop_spawn(&set, NULL, NULL, 0, 22, 0, pos, -1,
                             scripted_rand) == NULL,
          "an id no record names spawns nothing");
    check_eq_i(g_rand_calls, 0, "and draws nothing");
    check_eq_i(set.count, before, "and allocates nothing");

    /* A bank that does not carry the model: freed, no random number
     * (0x80020968 jal 0x8006D280, before 0x80020A00). */
    memset(&empty, 0, sizeof(empty));
    banks[0] = &empty;
    script_rand(0, 0);
    e = q2_item_drop_spawn(&set, NULL, banks, 1, 27, 0, pos, -1,
                           scripted_rand);
    check(e == NULL, "a model no bank carries spawns nothing");
    check_eq_i(g_rand_calls, 0, "and draws nothing");

    q2_item_drop_reset();
    q2_entity_set_free(&set);
}

static void test_drop_flight(void)
{
    q2_entity_set set;
    q2_entity_world w;
    q2_inventory inv;
    q2_entity *e, *nuke;
    q2_item_env env;
    q2_move_target floor_box;
    q2_move_world floor_world;
    s32 gravity = Q2_GRAVITY;
    s32 pos[3] = { 1000, -2000, 3000 };
    s32 away[3] = { 100000, 0, 0 };
    s16 vel[3];
    int i;

    printf("\na drop's flight (0x80020C48 through 0x800463E8)\n");

    q2_item_drop_reset();
    memset(&set, 0, sizeof(set));
    world_with_player(&w, &inv);
    q2_entity_world_move_player(&w, 0, away);

    memset(&env, 0, sizeof(env));
    env.gravity = &gravity;
    q2_item_bind_env(&env);

    script_rand(0, 0);
    e = q2_item_drop_spawn(&set, NULL, NULL, 0, 27, 1024, pos, -1,
                           scripted_rand);
    check(e != NULL, "a drop to fly");
    if (!e)
        return;

    /*
     * One tick at dt 12: vel.y -1536 + 32 * 12 = -1152, and the step is
     * vel * 12 / 320 truncated — (0, -43, 9). -43.2 truncates to -43; a floor
     * would say -44.
     */
    w.dt = 12;
    e->think(e, &w);
    check(q2_item_drop_velocity(e, vel), "still flying after one tick");
    check_eq_i(vel[1], -1152, "gravity is [0x800AE924] * dt on vel.y");
    check(e->pos[0] == 1000 && e->pos[1] == -2000 - 43 && e->pos[2] == 3009,
          "the step is vel * dt / 320, truncated toward zero");
    check_eq_i(e->origin[1], e->pos[1] + 286,
               "and the draw origin is 286 BELOW it from now on (0x80020CE4)");
    check(e->think == q2_item_drop_think, "it keeps the toss think");

    /* Free fall runs into the terminal velocity (0x80046490). */
    for (i = 0; i < 60; i++)
        e->think(e, &w);
    check(q2_item_drop_velocity(e, vel), "a long fall is still a flight");
    check_eq_i(vel[1], 8192, "capped at 8192, slti 8193");

    /*
     * Landing, on an ENTITY box — the hull half needs a SecondaryCol this
     * suite does not carry, and 0x80053974 is a contact all the same.
     */
    q2_item_drop_reset();
    memset(&floor_box, 0, sizeof(floor_box));
    floor_box.min[0] = -100000; floor_box.max[0] = 100000;
    floor_box.min[1] = -1000;   floor_box.max[1] = 1000;
    floor_box.min[2] = -100000; floor_box.max[2] = 100000;
    floor_box.kind   = Q2_MOVE_KIND_ENTITY;
    floor_box.active = true;
    memset(&floor_world, 0, sizeof(floor_world));
    floor_world.targets = &floor_box;
    floor_world.count   = 1;
    env.ents = &floor_world;
    q2_item_bind_env(&env);

    script_rand(0, 0);
    e = q2_item_drop_spawn(&set, NULL, NULL, 0, 27, 1024, pos, -1,
                           scripted_rand);
    script_rand(0, 0);
    nuke = q2_item_drop_spawn(&set, NULL, NULL, 0, 21, 1024, pos, -1,
                              scripted_rand);
    check(e != NULL && nuke != NULL, "two drops over a floor");
    if (!e || !nuke)
        return;
    check_eq_i(q2_item_drop_in_flight(), 2, "both in the air");

    for (i = 0; i < 200 && e->think == q2_item_drop_think; i++)
        e->think(e, &w);
    check(e->think == q2_item_think,
          "a contact at rest hands over to the item think (0x80020D1C)");
    check(i > 1, "and not on its first tick: it rises before it falls");
    check_eq_i(e->remove_in, 8700, "with an 8700-tick life (0x80020CFC)");
    check((e->flags & Q2_ITEM_TIMED) != 0, "and TIMED set");
    check(!q2_item_drop_velocity(e, vel), "and no flight record left");
    check(e->pos[1] < -1000, "resting on the box, not inside it");
    check_eq_i(e->origin[1], e->pos[1] + 286, "drawn 286 below its position");
    check(e->bounds_min[1] == -2000 - 512 && e->bounds_max[1] == -2000,
          "the touch box never followed it (nothing writes +0x78)");

    for (i = 0; i < 200 && nuke->think == q2_item_drop_think; i++)
        nuke->think(nuke, &w);
    check(nuke->think == q2_item_think, "the Nuke lands too");
    check(!(nuke->flags & Q2_ITEM_TIMED),
          "and stays untimed: OBJECTIVE skips the ori (0x80020CF4)");
    check_eq_i(q2_item_drop_in_flight(), 0, "nothing left in the air");

    /* Once landed it is an ordinary item: a TIMED one counts 8700 down. */
    w.dt = 300;
    for (i = 0; i < 40 && e->think == q2_item_think; i++)
        e->think(e, &w);
    check_eq_i(i, 29, "8700 / 300 = 29 ticks, then the shrink");

    q2_item_bind_env(NULL);
    q2_item_drop_reset();
    q2_entity_set_free(&set);
}

/* ------------------------------------------------------------------------- */
static void test_spawn_placement(void)
{
    u8 nodes[72] = {0}, planes[72] = {0}, links[4] = {0};
    q2_collision coll = {0};
    q2_entity_set set = {0};
    q2_entity *e;
    q2_pop_place place = {0};
    q2_item_table table = *q2_item_table_builtin();
    s32 pos[3] = {500,500,500}, node = -1;
    u32 i;
    for (i = 0; i < 3; i++) q2_wr_u32(nodes + 12 + i * 4, 2000);
    q2_wr_u16(nodes + 36 + 24, 6);
    for (i = 0; i < 6; i++) {
        u32 axis = i / 2;
        q2_wr_u16(planes + i * 12 + axis * 2, (u16)((i & 1) ? 2000 : 0));
        q2_wr_u16(planes + i * 12 + 6 + axis * 2, (u16)((i & 1) ? 4096 : -4096));
    }
    coll.nodes = nodes; coll.planes = planes; coll.links = links;
    coll.node_count = 1; coll.plane_count = 6;
    check(q2_entity_drop_to_floor(&coll, NULL, pos, 1024, &node), "airborne placement completes");
    check_eq_i(pos[1], 1554, "a completed sweep keeps its final Y, including the 30-unit nudge");
    check_eq_i(node, 0, "completed sweep still links its cell");
    pos[1] = 1900;
    check(q2_entity_drop_to_floor(&coll, NULL, pos, 1024, &node), "floor contact is success");
    check_eq_i(pos[1], 2000, "floor clips the sweep");
    {
        q2_move_target platform = {0};
        q2_move_world world = {0};
        platform.kind = Q2_MOVE_KIND_ENTITY;
        platform.active = true;
        platform.max[0] = platform.max[2] = 1000;
        platform.min[1] = platform.max[1] = 900; /* a thin scene floor */
        world.targets = &platform; world.count = 1;
        pos[1] = 500;
        check(q2_entity_drop_to_floor(&coll, &world, pos, 1024, &node),
              "solid platform participates in spawn placement");
        check_eq_i(pos[1], 900 - Q2_EYE_BASE, "body rests on platform top");
        check_eq_i(node, 0, "platform contact retains the resident collision cell");
        check(pos[0] == 500 && pos[2] == 500, "drop preserves authored horizontal position");
        platform.active = false; pos[1] = 500;
        check(q2_entity_drop_to_floor(&coll, &world, pos, 1024, NULL) && pos[1] == 1554,
              "inactive platform does not catch a spawn");
        platform.active = true; platform.kind = Q2_MOVE_KIND_VOLUME; pos[1] = 500;
        check(q2_entity_drop_to_floor(&coll, &world, pos, 1024, NULL) && pos[1] == 1554,
              "trigger volume does not catch a spawn");
        platform.kind = Q2_MOVE_KIND_ENTITY;
        platform.min[1] = platform.max[1] = 2600; pos[1] = 1900;
        check(q2_entity_drop_to_floor(&coll, &world, pos, 1024, NULL) && pos[1] == 2000,
              "nearer static floor wins over a lower platform");
    }
    place.x = place.z = 500; place.y = 1500; place.id = 27;
    e = q2_item_spawn(&set, &place, &table, 0, &coll);
    check(e && e->node == 0 && e->pos[1] == 2000, "pickup settles and links to the resident floor");
    for (i = 0; i < table.count; i++) table.def[i].flags |= Q2_ITEM_NO_DROP;
    place.y = 816; /* initial origin 500, target 530 */
    e = q2_item_spawn(&set, &place, &table, 0, &coll);
    check(e && e->node == 0 && e->pos[1] == 530, "no-drop still resolves the placement nudge and cell");
    place.x = 10000;
    i = set.count;
    check(!q2_item_spawn(&set, &place, &table, 0, &coll), "an unplaceable pickup is rejected");
    check_eq_i(set.count, i, "rejected placement does not consume a live entity slot");
    q2_entity_set_free(&set);
}

int main(void)
{
    /* Unbuffered, so a crash mid-suite still shows which section reached the
     * console. Cheap, and it has already paid for itself once. */
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("test_item\n\n");

    test_table();
    test_spawn_placement();
    test_glow_colours();
    test_ammo();
    test_weapons();
    test_armour();
    test_health();
    test_capacity_and_powerups();
    test_keys();
    test_spawn_and_think();
    test_shadow_packet();
    test_touch_sweep();
    test_weapons_stay();
    test_pickup_caption();
    test_timed_removal();
    test_entity_set();
    test_zone_groups();
    test_place_difficulty_flags();
    test_world_vertex();
    test_gib_draw_rotation();
    test_materialise_burst();
    test_drop_spawn();
    test_drop_flight();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
