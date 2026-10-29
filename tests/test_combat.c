/*
 * test_combat.c — damage, armour, knockback, splash, hitscan and firing.
 *
 * The old version of this file deliberately avoided asserting damage NUMBERS,
 * because the stats table was inferred from the PC lineage rather than read.
 * That reservation is gone: the numbers are now transcribed from the eleven
 * fire functions and the armour table is read out of the executable, so
 * asserting them is asserting a reading and a wrong one is a real regression.
 *
 * What is still NOT asserted here is anything marked INFERRED or MODELLED in
 * the headers — the grenade launcher's fuse and the bounce coefficient — for
 * exactly the old reason: pinning a guess makes the real value look like a bug
 * when it arrives.
 */
#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "combat.h"
#include "monster.h"
#include "multiplayer.h"
#include "playerdeath.h"
#include "projectile.h"
#include "shock.h"
#include "sim.h"
#include "trig.h"
#include "weapon.h"
#include "fixtures/combat_armour_reference.h"
#include "fixtures/projectile_contact_reference.h"
#include "fixtures/autoaim_reference.h"

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

static void place(q2_actor *a, s32 x, s32 y, s32 z, s16 hp)
{
    q2_actor_init(a);
    a->origin[0] = x; a->origin[1] = y; a->origin[2] = z;
    a->health = hp;
    a->gib_health = -60;

    /*
     * AND IT CAN BE HURT. The sweep filters on `takedamage`, not on health —
     * that is T_Damage's own first test at 0x80062848 and it is what lets a
     * corpse still be blown apart. A fixture that leaves it zero is building an
     * entity the engine would refuse to damage at all, which is a real state
     * (`monster_start` is what sets it) but not the one these tests mean.
     */
    a->takedamage = Q2_DAMAGE_AIM;
}

/*
 * A target that has NOT been through monster_start, i.e. the one case the
 * console really does refuse to damage.
 */
static void place_untouchable(q2_actor *a, s32 x, s32 y, s32 z, s16 hp)
{
    place(a, x, y, z, hp);
    a->takedamage = Q2_DAMAGE_NO;
}

/* ------------------------------------------------------------------------- */
/* The actor is a working projection of the player record. The two protection
 * timers live in the inventory, however, so this boundary is where an active
 * pickup either reaches T_Damage or silently becomes cosmetic. */
static void test_player_powerup_sync(void)
{
    q2_actor player;
    q2_inventory inv;
    q2_combat_rules rules;
    q2_damage_result hit;
    s32 pos[3] = { 10, 20, 30 };

    printf("player powerups\n");
    q2_actor_init(&player);
    q2_inventory_init(&inv);
    q2_combat_rules_default(&rules);
    inv.health = 100;
    inv.invuln_until = 200;
    inv.enviro_until = 300;

    q2_actor_from_player(&player, &inv, pos);
    check_eq_i(player.invuln_until, 200,
               "invulnerability deadline reaches the damage actor");
    check_eq_i(player.protect_until, 300,
               "envirosuit deadline reaches the damage actor");

    rules.level_time = 199;
    hit = q2_combat_damage(NULL, &player, 40, Q2_MOD_BULLET, pos, &rules);
    check(hit.blocked, "invulnerability blocks a hit before its deadline");
    check_eq_i(player.health, 100, "the blocked hit changes no health");

    /* The comparison in 0x80058230 is strict: the expiry tick itself hurts.
     * It has to be an ACID hit, because that is the one arm that reads the
     * suit's deadline at all. */
    inv.invuln_until = 0;
    inv.enviro_until = 300;
    q2_actor_from_player(&player, &inv, pos);
    rules.level_time = 300;
    hit = q2_combat_damage(NULL, &player, 40, Q2_MOD_ACID, pos, &rules);
    check(!hit.blocked, "the protection expiry tick is no longer protected");
    check(player.health < 100, "damage resumes on the expiry tick");

    /*
     * THE ENVIRONMENT SUIT IS NOT GENERAL PROTECTION. The damage function
     * reads client+0xB4 at exactly one instruction, 0x80058230, inside the
     * mod-9 arm; the general test at 0x80058304 reads client+0xB0 and nothing
     * else. (The status bar reads the word too, at 0x80035CB0, to draw the
     * suit's countdown; that refuses nothing.) The port used to OR the two
     * together for every mod, so wearing the suit made a rocket, a rail, a
     * bullet, melee, crush and falling all harmless — god mode with a green
     * tint.
     */
    q2_inventory_init(&inv);
    inv.health       = 100;
    inv.invuln_until = 0;      /* expired */
    inv.enviro_until = 5000;   /* worn */
    rules.level_time = 1000;

    q2_actor_from_player(&player, &inv, pos);
    hit = q2_combat_damage(NULL, &player, 30, Q2_MOD_ROCKET, pos, &rules);
    check(!hit.blocked, "the suit does not block a rocket");
    check_eq_i(player.health, 70, "which takes its health in full");

    q2_actor_from_player(&player, &inv, pos);
    hit = q2_combat_damage(NULL, &player, 30, Q2_MOD_ACID, pos, &rules);
    check(hit.blocked, "the suit blocks acid, which is all it blocks");
    check_eq_i(player.health, 100, "and acid changes no health");

    q2_actor_from_player(&player, &inv, pos);
    player.env_next = 0;
    hit = q2_combat_damage(NULL, &player, 30, Q2_MOD_LAVA, pos, &rules);
    check(!hit.blocked,
          "and it does not block lava — 0x8005828C reads neither timer");
    check_eq_i(player.health, 70, "so lava burns through the suit");

    /* Invulnerability, 0x80058304, is the one that stops all three. The suit
     * is off here so nothing else can be doing the blocking. */
    inv.invuln_until = 5000;
    inv.enviro_until = 0;
    q2_actor_from_player(&player, &inv, pos);
    check(q2_combat_damage(NULL, &player, 30, Q2_MOD_ROCKET, pos, &rules).blocked,
          "invulnerability blocks the rocket");
    q2_actor_from_player(&player, &inv, pos);
    player.env_next = 0;
    check(q2_combat_damage(NULL, &player, 30, Q2_MOD_ACID, pos, &rules).blocked,
          "and the acid");
    check_eq_i(player.env_next, 0,
               "acid leaves the throttle alone while you are invulnerable");
    q2_actor_from_player(&player, &inv, pos);
    player.env_next = 0;
    hit = q2_combat_damage(NULL, &player, 30, Q2_MOD_LAVA, pos, &rules);
    check(hit.blocked, "and the lava");
    /*
     * And the two arms differ in ORDER, which the port's single test could not
     * express: mod 9 checks both timers BEFORE the throttle (0x80058230 and
     * 0x80058244 precede 0x80058258), so acid leaves client+148 alone while you
     * are invulnerable, while mod 10 stores its deadline at 0x800582BC first
     * and only then meets 0x80058304's refusal.
     */
    check_eq_i(player.env_next, 1000 + Q2_ENV_THROTTLE_LAVA,
               "lava re-arms the throttle even on a hit invulnerability refuses");

    /*
     * The throttle's comparison is 0x80058260/0x800582A4 `sltu env_next,
     * level_time` with the branch-on-zero refusing, so the hit is allowed only
     * STRICTLY after the deadline. The tick on which the two are equal is
     * refused — the port used to write `level_time < env_next`, which allows
     * it, a one-tick divergence.
     */
    q2_inventory_init(&inv);
    inv.health = 100;
    rules.level_time = 1000;

    q2_actor_from_player(&player, &inv, pos);
    player.env_next = 1000;
    hit = q2_combat_damage(NULL, &player, 30, Q2_MOD_LAVA, pos, &rules);
    check(hit.blocked, "env_next == level_time is still throttled");
    check_eq_i(player.health, 100, "and takes nothing");

    q2_actor_from_player(&player, &inv, pos);
    player.env_next = 999;
    hit = q2_combat_damage(NULL, &player, 30, Q2_MOD_LAVA, pos, &rules);
    check(!hit.blocked, "one tick earlier it is not");
    check_eq_i(player.env_next, 1000 + Q2_ENV_THROTTLE_LAVA,
               "and 0x800582AC re-arms it 100 ticks out");

    /*
     * WHICH ARMOUR THE PLAYER IS WEARING, which this projection used to drop:
     * `armour_class` was written as a literal 0, so every class absorbed at
     * jacket's 1229/4096 and none of them absorbed energy at all. The
     * projection runs on every damage attempt, so there was no window in which
     * the field could hold anything else.
     */
    q2_inventory_init(&inv);
    inv.armour       = 200;
    inv.armour_class = Q2_ARMOUR_BODY;
    q2_actor_from_player(&player, &inv, pos);
    check_eq_i(player.armour_class, Q2_ARMOUR_BODY,
               "the armour class reaches the damage actor");

    /* `q2_combat_rules_default` leaves skill 1, so the bias is 2048 — see
     * test_armour_absorption for why that is the skill halfword's business and
     * not deathmatch's. */
    rules.level_time = 0;
    hit = q2_combat_damage(NULL, &player, 100, Q2_MOD_BULLET, pos, &rules);
    check_eq_i(hit.absorbed_armour, (2048 + 3277 * 100) >> 12,
               "body armour saves (2048 + 3277*100) >> 12, not jacket's 30");

    q2_actor_from_player(&player, &inv, pos);
    hit = q2_combat_damage(NULL, &player, 100, Q2_MOD_ENERGY_BOLT, pos, &rules);
    check_eq_i(hit.absorbed_armour, (2048 + 2458 * 100) >> 12,
               "and 2458/4096 against energy, where jacket's column is zero");

    /*
     * And the two power bits. Q2_POWERUP_POWER_ARMOUR is exactly the pair of
     * inventory flags 0x80057AC4 tests, so the whole word carries across;
     * `powerups` used to be left at zero and the shield absorbed nothing.
     */
    q2_inventory_init(&inv);
    inv.flags = Q2_INV_POWER_SHIELD;
    inv.ammo[Q2_AMMO_CELLS] = 100;
    q2_actor_from_player(&player, &inv, pos);
    check_eq_i(player.powerups & Q2_POWERUP_POWER_ARMOUR, Q2_INV_POWER_SHIELD,
               "the power shield's bit reaches the damage actor");

    hit = q2_combat_damage(NULL, &player, 30, Q2_MOD_BULLET, pos, &rules);
    check_eq_i(hit.absorbed_power, 20, "and it absorbs two thirds of the hit");
    check_eq_i(player.cells, 90, "spending one cell per two points");

    /* 0x800397FC's threshold, which playerdeath.h reads from the same
     * instruction. This projection used to carry a disagreeing -100. */
    check_eq_i(player.gib_health, Q2_PDEATH_GIB_HEALTH,
               "the player's gib threshold is the one playerdeath.h names");
}

/* ------------------------------------------------------------------------- */
static void test_ray_distance(void)
{
    s32 origin[3] = { 0, 0, 0 };
    s32 dir[3];
    s32 point[3];
    s64 along = 0, d2;

    printf("ray distance\n");

    /* The fire functions hand over a direction whose LENGTH is the range, so
     * `along` comes back as a 1.0.12 fraction of that length, not a distance. */
    dir[0] = 0; dir[1] = 0; dir[2] = 10000;

    point[0] = 0; point[1] = 0; point[2] = 5000;
    d2 = q2_combat_ray_dist_sq(origin, dir, point, &along);
    check_eq_i(d2, 0, "a point on the ray is at zero distance");
    check_eq_i(along, 2048, "halfway along reads 2048");

    point[0] = 300; point[1] = 0; point[2] = 5000;
    d2 = q2_combat_ray_dist_sq(origin, dir, point, &along);
    check_eq_i(d2, 300 * 300, "offset perpendicular gives that offset squared");

    point[0] = 0; point[1] = 0; point[2] = -5000;
    q2_combat_ray_dist_sq(origin, dir, point, &along);
    check(along < 0, "behind the shooter reads negative");
}

/* ------------------------------------------------------------------------- */
static void test_armour_absorption(void)
{
    q2_actor a;
    q2_combat_rules rules;
    s16 save;

    printf("armour\n");
    q2_combat_rules_default(&rules);
    rules.skill = 0;    /* 0x80057C20's arm: the 4095 bias */

    /* Jacket armour, 0.30 normal protection, with the EASY bias of 4095 that
     * rounds every non-zero fraction up. */
    place(&a, 0, 0, 0, 100);
    a.has_client   = true;
    a.armour       = 50;
    a.armour_class = 0;

    save = q2_combat_armour_absorb(&a, 100, false, false, &rules);
    check_eq_i(save, (4095 + 1229 * 100) >> 12, "jacket takes 30% of 100");
    check_eq_i(a.armour, (s16)(50 - save), "and spends exactly that much");

    /* The energy column: jacket protects against energy not at all, so only the
     * bias survives the shift — which is zero. */
    place(&a, 0, 0, 0, 100);
    a.has_client = true;
    a.armour = 50;
    save = q2_combat_armour_absorb(&a, 100, true, false, &rules);
    check_eq_i(save, 0, "jacket stops no energy damage");

    /* Body armour is 0.80 normal and 0.60 energy. */
    place(&a, 0, 0, 0, 100);
    a.has_client = true;
    a.armour = 200;
    a.armour_class = 2;
    save = q2_combat_armour_absorb(&a, 100, true, false, &rules);
    check_eq_i(save, (4095 + 2458 * 100) >> 12, "body takes 60% of energy");

    /* Capped at what is left. */
    place(&a, 0, 0, 0, 100);
    a.has_client = true;
    a.armour = 5;
    a.armour_class = 2;
    save = q2_combat_armour_absorb(&a, 100, false, false, &rules);
    check_eq_i(save, 5, "cannot absorb more than the armour held");
    check_eq_i(a.armour, 0, "and is emptied");

    /*
     * WHICH GLOBAL PICKS THE BIAS. 0x80057C10 is `lh v0, 0x800B334A` — the
     * skill halfword, the same one 0x800582D0 reads for the damage halving —
     * and 0x80057C18's `bne` sends skill != 0 to the 2048 in the delay slot.
     * This used to be driven by `rules.deathmatch`, which reads 0x800AEBCC and
     * has nothing to do with it. Skill is what these three cases turn on, and
     * deathmatch on its own must move nothing.
     */
    {
        q2_combat_rules easy, medium, hard, dm;

        q2_combat_rules_default(&easy);   easy.skill = 0;
        q2_combat_rules_default(&medium); medium.skill = 1;
        q2_combat_rules_default(&hard);   hard.skill = 2;
        q2_combat_rules_default(&dm);     dm.skill = 1; dm.deathmatch = true;

        place(&a, 0, 0, 0, 100);
        a.has_client = true; a.armour = 100; a.armour_class = 0;
        check_eq_i(q2_combat_armour_absorb(&a, 1, false, false, &easy),
                   (4095 + 1229) >> 12, "skill 0 rounds a 1-point jacket hit up");

        place(&a, 0, 0, 0, 100);
        a.has_client = true; a.armour = 100; a.armour_class = 0;
        check_eq_i(q2_combat_armour_absorb(&a, 1, false, false, &medium),
                   (2048 + 1229) >> 12, "skill 1 rounds it away");

        place(&a, 0, 0, 0, 100);
        a.has_client = true; a.armour = 100; a.armour_class = 0;
        check_eq_i(q2_combat_armour_absorb(&a, 1, false, false, &hard),
                   (2048 + 1229) >> 12, "and so does skill 2");

        /* Deathmatch alone changes neither, which is the whole correction. */
        place(&a, 0, 0, 0, 100);
        a.has_client = true; a.armour = 100; a.armour_class = 0;
        check_eq_i(q2_combat_armour_absorb(&a, 1, false, false, &dm),
                   (2048 + 1229) >> 12, "deathmatch at skill 1 is skill 1");

        dm.skill = 0;
        place(&a, 0, 0, 0, 100);
        a.has_client = true; a.armour = 100; a.armour_class = 0;
        check_eq_i(q2_combat_armour_absorb(&a, 1, false, false, &dm),
                   (4095 + 1229) >> 12, "and deathmatch at skill 0 is skill 0");

        /* The eight-point body-armour case, where the two biases differ by a
         * whole point rather than by rounding a zero. */
        place(&a, 0, 0, 0, 100);
        a.has_client = true; a.armour = 200; a.armour_class = 2;
        check_eq_i(q2_combat_armour_absorb(&a, 8, false, false, &easy), 7,
                   "8 points against body armour saves 7 at skill 0");

        place(&a, 0, 0, 0, 100);
        a.has_client = true; a.armour = 200; a.armour_class = 2;
        check_eq_i(q2_combat_armour_absorb(&a, 8, false, false, &medium), 6,
                   "and 6 at skill 1");
    }

    /* No client means no armour at all: a creature never has any. */
    place(&a, 0, 0, 0, 100);
    a.has_client = false;
    a.armour = 200;
    check_eq_i(q2_combat_armour_absorb(&a, 100, false, false, &rules), 0,
               "a creature has no armour");
}

static void test_power_armour(void)
{
    q2_actor a;
    s16 save;

    printf("power armour\n");

    place(&a, 0, 0, 0, 100);
    a.has_client = true;
    a.powerups = Q2_POWERUP_POWER_ARMOUR;
    a.cells = 100;

    save = q2_combat_power_armour_absorb(&a, 90);
    check_eq_i(save, 60, "absorbs two thirds");
    check_eq_i(a.cells, 100 - 30, "one cell per two points absorbed");

    /* Capped at twice the cells held. */
    place(&a, 0, 0, 0, 100);
    a.has_client = true;
    a.powerups = Q2_POWERUP_POWER_ARMOUR;
    a.cells = 5;
    save = q2_combat_power_armour_absorb(&a, 300);
    check_eq_i(save, 10, "capped at twice the cells");
    check_eq_i(a.cells, 0, "which empties them");

    /* Without the powerup bit it does nothing, however many cells are held. */
    place(&a, 0, 0, 0, 100);
    a.has_client = true;
    a.cells = 200;
    check_eq_i(q2_combat_power_armour_absorb(&a, 90), 0,
               "no power item, no absorption");
}

/* ------------------------------------------------------------------------- */
static void test_native_signed_armour(void)
{
    u32 i;
    printf("retail signed armour vectors\n");
    for (i = 0; i < Q2PSX_ARRAY_COUNT(combat_armour_reference); i++) {
        const s32 *v = combat_armour_reference[i];
        q2_actor a;
        q2_combat_rules rules;
        s16 absorbed;
        char label[80];
        place(&a, 0, 0, 0, 100);
        a.has_client = true;
        a.cells = a.armour = (s16)v[2];
        a.armour_class = (u8)v[3];
        a.powerups = v[6] ? Q2_POWERUP_POWER_ARMOUR : 0;
        a.armour_fade = (s8)v[7];
        q2_combat_rules_default(&rules);
        rules.skill = (s16)v[5];
        absorbed = v[0] == 0 ? q2_combat_power_armour_absorb(&a, v[1]) :
            q2_combat_armour_absorb(&a, v[1], v[4] != 0, v[7] != 0, &rules);
        snprintf(label, sizeof(label), "native armour vector %u absorption", i);
        check_eq_i(absorbed, v[8], label);
        snprintf(label, sizeof(label), "native armour vector %u remaining", i);
        check_eq_i(v[0] == 0 ? a.cells : a.armour, v[9], label);
        snprintf(label, sizeof(label), "native armour vector %u fade", i);
        check_eq_i(a.armour_fade, v[10], label);
    }
}

static void test_zero_and_negative_damage(void)
{
    q2_actor a, attacker;
    q2_combat_rules rules;
    q2_damage_result hit;
    const s32 point[3] = {1000, 0, 0};
    printf("zero and signed damage integration\n");
    q2_combat_rules_default(&rules);
    place(&a, 0, 0, 0, 100); a.has_client = true;
    a.knockback[0] = 7; a.knockback[1] = -9; a.knockback[2] = 11;
    hit = q2_combat_damage(NULL, &a, 0, Q2_MOD_BULLET, point, &rules);
    check(!a.knocked && a.knockback[0] == 7 && a.knockback[1] == -9 && a.knockback[2] == 11,
          "zero damage skips knockback and preserves pending impulses");
    check(hit.hit_sound_id != 0 && a.health == 100, "zero player damage still reaches feedback");
    rules.cheats = Q2_CHEAT_ONE_SHOT_KILL;
    hit = q2_combat_damage(NULL, &a, 0, Q2_MOD_BULLET, point, &rules);
    check(hit.killed && a.health == 0 && !a.knocked, "zero damage still reaches the cheat after skipping impulse");
    rules.cheats = 0;

    place(&a, 0, 0, 0, 100); a.has_client = true;
    hit = q2_combat_damage(NULL, &a, -30, Q2_MOD_BULLET, point, &rules);
    check(hit.taken == -30 && a.health == 130, "negative unarmoured damage reaches the health subtraction");
    check(a.knocked && a.knockback[0] == 800, "negative damage reverses the native impulse");
    place(&a, 0, 0, 0, 100); a.has_client = true;
    a.armour_class = Q2_ARMOUR_BODY; a.armour = 100;
    hit = q2_combat_damage(NULL, &a, -30, Q2_MOD_BULLET, NULL, &rules);
    check(hit.absorbed_armour == -24 && hit.taken == -6 && a.health == 106 && a.armour == 124,
          "negative body-armour absorption restores armour and the residual restores health");
    place(&a, 0, 0, 0, 100); a.has_client = true;
    a.powerups = Q2_POWERUP_POWER_ARMOUR; a.cells = 100;
    a.armour_class = Q2_ARMOUR_BODY; a.armour = 100;
    hit = q2_combat_damage(NULL, &a, -30, Q2_MOD_BULLET, NULL, &rules);
    check(hit.absorbed_power == -20 && hit.absorbed_armour == -8 && hit.taken == -2 &&
          a.health == 102 && a.cells == 110 && a.armour == 108 && a.armour_fade == Q2_ACTOR_FADE_POWER,
          "both signed armour stages preserve their ordering and the power fade");
    hit = q2_combat_damage(NULL, &a, -30, Q2_MOD_NO_ARMOUR, NULL, &rules);
    check(a.health == 132 && a.cells == 110 && a.armour == 108, "mod 8 bypasses signed absorption too");
    a.invuln_until = 1001; rules.level_time = 1000;
    hit = q2_combat_damage(NULL, &a, -30, Q2_MOD_ENERGY_BOLT, NULL, &rules);
    check(hit.blocked && a.health == 132 && !a.effect[1], "invulnerability still refuses negative damage");
    a.invuln_until = 0; a.armour = a.cells = 0;
    hit = q2_combat_damage(NULL, &a, -1, Q2_MOD_ENERGY_BOLT, NULL, &rules);
    check(hit.taken == -1 && a.effect[1] == 3, "negative damage reaches the effect tail");

    place(&a, 0, 0, 0, 100); a.has_client = true;
    a.powerups = Q2_POWERUP_POWER_ARMOUR; a.cells = 16384;
    a.armour_class = Q2_ARMOUR_BODY; a.armour = 100;
    hit = q2_combat_damage(NULL, &a, 30, Q2_MOD_BULLET, NULL, &rules);
    check(hit.absorbed_power == -32768 && hit.absorbed_armour == 100 &&
          hit.taken == 32698 && a.health == -32598 && a.cells == -32768 && !a.armour,
          "the power stage's halfword cap does not truncate the following armour argument");

    place(&attacker, 0, 0, 0, 100); attacker.has_client = true;
    place(&a, 0, 0, 0, 100); a.is_monster = true;
    hit = q2_combat_damage(&attacker, &a, -20, Q2_MOD_ENERGY_BOLT, NULL, &rules);
    check(hit.surprised && hit.taken == -40 && a.health == 140 && a.damage_reaction_pending,
          "a creature's nonzero signed hit reaches surprise, subtraction and reaction");
}

static void test_damage(void)
{
    q2_actor target, attacker;
    q2_combat_rules rules;
    q2_damage_result r;

    printf("damage\n");
    q2_combat_rules_default(&rules);

    place(&target, 0, 0, 0, 100);
    place(&attacker, 0, 0, -1000, 100);

    r = q2_combat_damage(&attacker, &target, 30, Q2_MOD_BULLET, NULL, &rules);
    check_eq_i(r.taken, 30, "unarmoured damage reaches health in full");
    check_eq_i(target.health, 70, "and health falls by it");
    check(!r.killed, "and this did not kill");

    r = q2_combat_damage(&attacker, &target, 70, Q2_MOD_BULLET, NULL, &rules);
    check(r.killed, "the hit that crosses zero reports the kill");
    check(!r.gibbed, "but not a gib at exactly zero");

    place(&target, 0, 0, 0, 10);
    r = q2_combat_damage(&attacker, &target, 200, Q2_MOD_BULLET, NULL, &rules);
    check(r.gibbed, "past the gib threshold reports a gib");

    /* Mod 8 is the one class armour does not touch. */
    place(&target, 0, 0, 0, 100);
    target.has_client = true;
    target.armour = 200;
    target.armour_class = 2;
    r = q2_combat_damage(&attacker, &target, 50, Q2_MOD_NO_ARMOUR, NULL, &rules);
    check_eq_i(r.absorbed_armour, 0, "mod 8 bypasses armour");
    check_eq_i(target.health, 50, "so all of it reaches health");

    /* Invulnerability refuses everything. */
    place(&target, 0, 0, 0, 100);
    target.has_client = true;
    target.invuln_until = 500;
    rules.level_time = 100;
    r = q2_combat_damage(&attacker, &target, 50, Q2_MOD_BULLET, NULL, &rules);
    check(r.blocked, "invulnerability blocks");
    check_eq_i(target.health, 100, "and health is untouched");
    rules.level_time = 600;
    r = q2_combat_damage(&attacker, &target, 50, Q2_MOD_BULLET, NULL, &rules);
    check(!r.blocked, "and expires on the clock");

    /* Skill 0 halves what a monster does to a player, and only that. */
    {
        q2_combat_rules easy;
        q2_combat_rules_default(&easy);
        easy.skill = 0;

        place(&target, 0, 0, 0, 100);
        target.has_client = true;
        place(&attacker, 0, 0, -1000, 100);   /* no client: a creature */

        /* Rounding UP, and the disc says so: 0x800582FC `addiu v0, s1, 1` into
         * 0x80058300 `sra s1, v0, 1`, with the +1 unconditional rather than the
         * sign-bit term a truncating `/2` would carry. 31 halves to 16. */
        q2_combat_damage(&attacker, &target, 31, Q2_MOD_BULLET, NULL, &easy);
        check_eq_i(target.health, 100 - 16, "skill 0 halves, rounding up");

        place(&target, 0, 0, 0, 100);
        attacker.has_client = true;           /* now a player hurt a player */
        q2_combat_damage(&attacker, &target, 31, Q2_MOD_BULLET, NULL, &easy);
        check_eq_i(target.health, 100 - 31, "but not player-on-player");

        /*
         * AND AN ABSENT ATTACKER IS NOT A MONSTER. 0x800582E0 requires the
         * attacker's entity back-pointer and 0x800582F0 its client block, so a
         * NULL attacker cannot take the halving arm at all — it takes FULL
         * damage. A REGRESSION PIN of combat.c as it already stood (commit
         * 8952fe9 transcribed the rounding): both of these pass before and
         * after this round. What they guard is the difference a caller makes
         * by passing NULL for a creature's shot: on Easy it lands at about
         * double what the console does. The client's creature hooks no longer
         * pass NULL. main.c's client_cre_melee, client_cre_fire and
         * client_cre_shot resolve the shooter's actor with client_cre_actor(),
         * which is NULL only for a monster outside the creature set or before
         * the actors exist. main.c is not linked here, so this pins the rule
         * those hooks rely on, not the hooks.
         */
        place(&target, 0, 0, 0, 100);
        target.has_client = true;
        place(&attacker, 0, 0, -1000, 100);   /* a creature: no client */
        q2_combat_damage(&attacker, &target, 25, Q2_MOD_BULLET, NULL, &easy);
        check_eq_i(target.health, 100 - 13,
                   "a creature's shot on Easy lands at 13 of 25");

        place(&target, 0, 0, 0, 100);
        target.has_client = true;
        q2_combat_damage(NULL, &target, 25, Q2_MOD_BULLET, NULL, &easy);
        check_eq_i(target.health, 100 - 25,
                   "and the same shot with no attacker lands at all 25");
    }

    /*
     * The surprise bonus, 0x800628C8. Replaces a test asserting that a
     * module-driven creature has its damage posted to the AI rather than
     * subtracted, which the disassembly does not support — T_Damage subtracts
     * at 0x80062958 and the call that claim rested on passes a different
     * entity entirely.
     */
    {
        q2_actor mob;

        place(&mob, 0, 0, 0, 100);
        mob.is_monster = true;
        mob.has_enemy  = false;
        attacker.has_client = true;
        r = q2_combat_damage(&attacker, &mob, 20, Q2_MOD_BULLET, NULL, &rules);
        check(r.surprised, "an unaware monster is surprised");
        check_eq_i(mob.health, 100 - 40, "and takes double");

        /* Once it has an enemy the bonus is gone. */
        place(&mob, 0, 0, 0, 100);
        mob.is_monster = true;
        mob.has_enemy  = true;
        r = q2_combat_damage(&attacker, &mob, 20, Q2_MOD_BULLET, NULL, &rules);
        check(!r.surprised, "a monster that has seen you is not");
        check_eq_i(mob.health, 100 - 20, "and takes the plain amount");

        /* And a monster shot by another monster never gets it. */
        place(&mob, 0, 0, 0, 100);
        mob.is_monster = true;
        attacker.has_client = false;
        r = q2_combat_damage(&attacker, &mob, 20, Q2_MOD_BULLET, NULL, &rules);
        check(!r.surprised, "nor one shot by something with no client");
    }

    /* Godmode and the knockback flag, 0x8006292C and 0x8006291C. */
    {
        q2_actor god;

        place(&god, 0, 0, 0, 100);
        god.godmode = true;
        r = q2_combat_damage(NULL, &god, 40, Q2_MOD_BULLET, NULL, &rules);
        check_eq_i(god.health, 100, "godmode takes nothing");
        check_eq_i(r.taken, 0, "and reports nothing taken");

        /* T_Damage's zero sends it to 0x80062AAC and its epilogue, not out of
         * the outer function, which still reaches the effect tail. */
        place(&god, 0, 0, 0, 100);
        god.godmode = true;
        q2_combat_damage(NULL, &god, 40, Q2_MOD_ENERGY_BOLT, NULL, &rules);
        check(god.health == 100 && god.effect[1] == 3,
              "a godmode body takes nothing and still has the bolt's +0x2F1");

        /*
         * And the test is T_DAMAGE's (0x80062924 on the word 0x80062914
         * loads), and 0x800582C8 keeps every client target out of T_Damage,
         * so the damage function never consults a player's godmode bit — the
         * client arm stores at 0x800583F8.
         */
        place(&god, 0, 0, 0, 100);
        god.has_client = true;
        god.godmode    = true;
        q2_combat_damage(NULL, &god, 40, Q2_MOD_BULLET, NULL, &rules);
        check_eq_i(god.health, 60,
                   "a client target's godmode bit does not stop the hit");
    }

    /*
     * Who did it. The engine keeps the killer's id as a signed byte at
     * entity+222 and `q2_mp_attribute_kill` has always taken one; nothing could
     * supply it, because an actor could not say which player it was. A
     * deathmatch kill had a victim and no killer.
     */
    {
        q2_actor shooter, victim;

        place(&shooter, 0, 0, 0, 100);
        place(&victim, 0, 0, 0, 100);
        shooter.owner = 2;

        q2_combat_damage(&shooter, &victim, 30, Q2_MOD_BULLET, NULL, &rules);
        check_eq_i(victim.last_attacker, 2, "the shooter's id is recorded");
        check_eq_i(q2_mp_attribute_kill(victim.last_attacker, victim.last_mod),
                   2, "and attribution gives that player the frag");

        /*
         * WORLD DAMAGE DOES NOT LEAVE -1, and this test used to assert a
         * fiction. `access 0xDE` finds one store of a literal -1 to entity+222
         * in the whole image, 0x800396DC, and it is in the death handler and
         * only for mods 9 and 10 — the damage function never writes it. What
         * 0x80057E88..0x80057EB8 stores for a NULL attacker is 23 (I evaluated
         * the ×0x55555555 / negate / `sra 8` chain rather than assuming it), and
         * what the projectile spawners store for a non-player owner is the
         * literal 4 (0x8004A208, 0x8004ABF4, 0x8004B1C4, 0x8004BF80). Both are
         * non-negative and both are outside the frag table; the port carries
         * the spawners' 4. (Single player: `rules` here is not deathmatch.)
         */
        place(&victim, 0, 0, 0, 100);
        q2_combat_damage(NULL, &victim, 30, Q2_MOD_BULLET, NULL, &rules);
        check_eq_i(victim.last_attacker, Q2_MP_NOT_A_PLAYER,
                   "world damage records a non-player, not -1");
        check_eq_i(q2_mp_attribute_kill(victim.last_attacker, victim.last_mod),
                   -1, "and stays nobody's frag");

        /* A creature is not a player either: `owner` is -1 for anything that
         * is not one, and that must not become the killer byte. */
        place(&victim, 0, 0, 0, 100);
        place(&shooter, 0, 0, 0, 100);
        q2_combat_damage(&shooter, &victim, 30, Q2_MOD_BULLET, NULL, &rules);
        check_eq_i(victim.last_attacker, Q2_MP_NOT_A_PLAYER,
                   "a creature's hit records a non-player too");
        shooter.owner = 2;

        /* A hazard is nobody's frag even when a player last touched you. */
        place(&victim, 0, 0, 0, 100);
        q2_combat_damage(&shooter, &victim, 5, Q2_MOD_LAVA, NULL, &rules);
        check_eq_i(q2_mp_attribute_kill(victim.last_attacker, victim.last_mod),
                   -1, "lava is nobody's frag");
    }

    /*
     * DEATHMATCH CREDITS THE WORLD'S HIT TO THE PLAYER IT HURT.
     *
     * 0x80057DB8 `bne s5, zero` falls through only for a NULL attacker,
     * 0x80057DC0 `beq s0, zero, 0x80057EBC` then needs the TARGET's client
     * block, and 0x80057DC8..0x80057E00 divide that block's offset from
     * 0x800C7C60 by 224 — the victim's own index, stored at 0x80057EB8. So a
     * crusher (0x80051E74, a0 = 0) that kills in deathmatch reaches the frag
     * hook as a suicide and does not cry out. Recording the stand-in 4 there
     * lost the hook altogether once the death handler began testing the raw
     * byte, as 0x80039774 does.
     */
    {
        q2_combat_rules       dm = rules;
        q2_actor              victim, mob, creature;
        q2_player_death       d;
        q2_player_death_event ev;
        s8 dm_world, sp_world, dm_creature, dm_mob;

        dm.deathmatch = true;

        place(&victim, 0, 0, 0, 100);
        victim.has_client = true;
        victim.owner      = 1;
        q2_combat_damage(NULL, &victim, 200, Q2_MOD_CRUSH, NULL, &dm);
        dm_world = victim.last_attacker;
        check_eq_i(dm_world, 1,
                   "a deathmatch world hit is credited to its victim");

        q2_player_death_init(&d);
        q2_player_die(&d, victim.last_attacker, victim.last_mod, victim.owner,
                      true, false, &ev);
        check(ev.frag_hook && ev.frag_killer == 1 && ev.frag_victim == 1 &&
              !ev.cried_out,
              "so the crusher's kill reaches the hook as a silent suicide");

        /* Only that arm: the same hit in single player records the stand-in,
         * and a deathmatch hit by a creature nobody has hurt (its own byte 4,
         * 0x80057E6C) or by the world on a client-less target (0x80057DC0)
         * stores nothing, leaving each target at q2_actor_init's 4. */
        place(&victim, 0, 0, 0, 100);
        victim.has_client = true;
        victim.owner      = 1;
        q2_combat_damage(NULL, &victim, 30, Q2_MOD_CRUSH, NULL, &rules);
        sp_world = victim.last_attacker;

        place(&creature, 0, 0, 0, 100);        /* owner -1, no client */
        place(&victim, 0, 0, 0, 100);
        victim.has_client = true;
        victim.owner      = 1;
        q2_combat_damage(&creature, &victim, 30, Q2_MOD_BULLET, NULL, &dm);
        dm_creature = victim.last_attacker;

        place(&mob, 0, 0, 0, 100);
        q2_combat_damage(NULL, &mob, 30, Q2_MOD_CRUSH, NULL, &dm);
        dm_mob = mob.last_attacker;

        check(dm_world == 1 && sp_world == Q2_MP_NOT_A_PLAYER &&
              dm_creature == Q2_MP_NOT_A_PLAYER &&
              dm_mob == Q2_MP_NOT_A_PLAYER,
              "the victim's own index belongs to deathmatch's NULL-attacker "
              "arm and nothing else");
    }

    /* A corpse floors at -9999 however hard it is hit. */
    place(&target, 0, 0, 0, 10);
    r = q2_combat_damage(NULL, &target, 30000, Q2_MOD_BULLET, NULL, &rules);
    check(r.killed, "a big hit kills");
    check_eq_i(target.health, Q2_HEALTH_FLOOR, "and health floors at -9999");

    /*
     * A PLAYER DOES NOT. The floor is 0x800629B4, inside T_Damage, and
     * 0x800582C8 sends a client target past T_Damage to the client arm's own
     * store, 0x800583EC `lhu` / 0x800583F4 `subu` / 0x800583F8 `sh`, which
     * nothing bounds from below. q2_inventory_apply_damage stores it the same.
     */
    place(&target, 0, 0, 0, 10);
    target.has_client = true;
    q2_combat_damage(NULL, &target, 30000, Q2_MOD_BULLET, NULL, &rules);
    check_eq_i(target.health, 10 - 30000, "a player's health has no floor");
}

/* ------------------------------------------------------------------------- */
/*
 * ARMOUR_FADE, gp+1184, and the two screen washes it gates — 0x80057B80's
 * green for a hit the power shield ate and 0x80057D24's grey for one ordinary
 * armour took. `xrefs 0x8007832C` lists four SetWinFade raisers and this port
 * had one of them; these are two of the missing three.
 *
 * The flag is NOT about the hit sound, which is what the code used to say. It
 * is read in exactly one place, 0x80057CB8, and the branch at 0x80057CC0 jumps
 * over CheckArmour's SetWinFade and lands on the armour subtraction — so the
 * points still come off, only the grey tint is suppressed.
 */
static void test_armour_fade_flag(void)
{
    q2_actor a;
    q2_combat_rules rules;

    printf("the armour screen wash\n");
    q2_combat_rules_default(&rules);
    rules.skill = 0;

    /* The colours, read straight out of `bytes 0x800AEAA0 16`. */
    check_eq_i(q2_actor_fade_power_rgb[0], 0x00, "0x800AEAA4 red is 0");
    check_eq_i(q2_actor_fade_power_rgb[1], 0x80, "its green is 128");
    check_eq_i(q2_actor_fade_power_rgb[2], 0x20, "its blue is 32");
    check_eq_i(q2_actor_fade_armour_rgb[1], 0x80, "0x800AEAA8 is grey 128");

    /* A hit the power shield absorbs raises the green one. */
    place(&a, 0, 0, 0, 100);
    a.has_client = true;
    a.powerups   = Q2_POWERUP_POWER_ARMOUR;
    a.cells      = 100;
    check(q2_combat_power_armour_absorb(&a, 90) > 0, "the shield absorbs");
    check_eq_i(a.armour_fade, Q2_ACTOR_FADE_POWER,
               "and 0x80057B24 raises the green wash");

    /*
     * 0x80057AB0 clears the flag as the function's FIRST instruction, before
     * every guard — so a second hit that finds nothing to absorb takes the
     * green wash away rather than leaving it latched.
     */
    a.cells = 0;
    check_eq_i(q2_combat_power_armour_absorb(&a, 90), 0, "no cells, no save");
    check_eq_i(a.armour_fade, Q2_ACTOR_FADE_NONE,
               "and the flag is cleared ahead of the guards");

    /* Ordinary armour on its own raises the grey one. */
    place(&a, 0, 0, 0, 100);
    a.has_client   = true;
    a.armour       = 50;
    a.armour_class = 0;
    check(q2_combat_armour_absorb(&a, 100, false, false, &rules) > 0,
          "jacket absorbs");
    check_eq_i(a.armour_fade, Q2_ACTOR_FADE_ARMOUR,
               "and 0x80057D24 raises the grey wash");

    /*
     * With the flag already up, CheckArmour takes its points and raises
     * nothing: one hit is one colour on the screen.
     */
    place(&a, 0, 0, 0, 100);
    a.has_client   = true;
    a.armour       = 50;
    a.armour_class = 0;
    a.armour_fade  = Q2_ACTOR_FADE_POWER;
    check_eq_i(q2_combat_armour_absorb(&a, 100, true, true, &rules), 0,
               "jacket stops no energy, so there is nothing to suppress");
    place(&a, 0, 0, 0, 100);
    a.has_client   = true;
    a.armour       = 50;
    a.armour_class = 0;
    a.armour_fade  = Q2_ACTOR_FADE_POWER;
    check(q2_combat_armour_absorb(&a, 100, false, true, &rules) > 0,
          "the armour still spends its points (past 0x80057CC0's join)");
    check_eq_i(a.armour_fade, Q2_ACTOR_FADE_POWER,
               "but the grey wash is suppressed and the green one stands");

    /*
     * And through the whole damage function: a hit the shield eats entirely
     * costs no health and no armour, which is exactly why the HUD's own
     * compare at 0x8003AE10 cannot stand in for this raiser.
     */
    {
        q2_actor target;
        q2_damage_result r;

        place(&target, 0, 0, 0, 100);
        target.has_client = true;
        target.powerups   = Q2_POWERUP_POWER_ARMOUR;
        target.cells      = 100;
        target.armour     = 0;

        r = q2_combat_damage(NULL, &target, 30, Q2_MOD_BULLET, NULL, &rules);
        check(r.absorbed_power > 0, "the shield took some of it");
        check_eq_i(r.absorbed_armour, 0, "and armour took none");
        check_eq_i(target.armour_fade, Q2_ACTOR_FADE_POWER,
                   "so the only feedback the player gets is the green wash");
    }
}

/* ------------------------------------------------------------------------- */
/*
 * T_DAMAGE'S `takedamage` GATE, 0x80062838, which this side never consulted.
 *
 * The field was carried and documented and honoured in exactly two places —
 * `nearest_hit` and the rail sweep — so a blast, a claw or a direct projectile
 * hit went straight past it. It is the FIFTH instruction of T_Damage, before
 * health, knockback, pain and die, and its one caller is 0x800584B4.
 */
static void test_takedamage_gate(void)
{
    q2_actor target, attacker;
    q2_combat_rules rules;
    q2_damage_result r;
    s32 point[3] = { 0, 0, -100 };

    printf("takedamage gate\n");
    q2_combat_rules_default(&rules);
    rules.knockback_mass = 64;

    place(&attacker, 0, 0, -1000, 100);

    place_untouchable(&target, 0, 0, 0, 100);
    r = q2_combat_damage(&attacker, &target, 50, Q2_MOD_ROCKET, point, &rules);
    check_eq_i(target.health, 100, "a takedamage-clear target takes nothing");
    check_eq_i(r.taken, 0, "and reports nothing taken");
    check(!r.blocked,
          "0x80062B54 is the bare epilogue, not the invulnerability refusal");

    /*
     * AND THE IMPULSE IS STILL RECORDED, which is why the gate goes after the
     * knockback block and not at the top of the function: retail's knockback is
     * 0x80057EC0..0x80058204 in the OUTER function and T_Damage is only reached
     * from 0x800584B4, long afterwards.
     */
    check(target.knockback[2] != 0,
          "but the outer function has already recorded the impulse");

    /*
     * AND THE EFFECT TAIL STILL RUNS AFTER IT. T_Damage's epilogue returns to
     * 0x800584BC `j 0x800584D4`, whose client test sends a client-less target
     * on to 0x800585A4, so a bolt stores +0x2F1 = 3 (0x800585E8) on a target
     * T_Damage refused. The gate used to return from the whole function.
     */
    place_untouchable(&target, 0, 0, 0, 100);
    q2_combat_damage(&attacker, &target, 50, Q2_MOD_ENERGY_BOLT, NULL, &rules);
    check(target.health == 100 && target.effect[1] == 3,
          "a bolt hurts a takedamage-clear target not at all, and lights it");

    /* The blast and the claw reach the same gate, through the same function. */
    {
        q2_actor *list[1];

        place_untouchable(&target, 0, 0, 0, 100);
        target.radius = 0;
        list[0] = &target;
        /*
         * The return still moves, and that is retail: 0x80050A60 `addiu s3,
         * zero, 1` is in the delay slot of the `jal 0x80057D54` at 0x80050A5C,
         * so the sweep records "a candidate was reached" before the damage
         * function has decided anything. Retail's s3 is a FLAG, returned by
         * 0x80050A78, where the port returns a count; with one candidate the
         * two are the same 1. What must not move is the health.
         */
        check_eq_i(q2_combat_radius_damage(&attacker, NULL, point, 200, 1300,
                                           Q2_MOD_ROCKET, list, 1, &rules),
                   1, "the blast still reaches the candidate");
        check_eq_i(target.health, 100, "but hurts nothing that cannot be hurt");

        place_untouchable(&target, 0, 0, 0, 100);
        q2_combat_melee(&attacker, &target, 30, attacker.origin, &rules);
        check_eq_i(target.health, 100, "nor does a claw");
    }

    /*
     * A PLAYER IS NEVER GATED ON IT. 0x800582C8 `beq s0, zero, 0x8005842C`
     * diverts every client target away from T_Damage, so the bit is not
     * consulted for one — and applying it there would have made a player with a
     * zero byte invincible.
     */
    place_untouchable(&target, 0, 0, 0, 100);
    target.has_client = true;
    r = q2_combat_damage(&attacker, &target, 50, Q2_MOD_BULLET, NULL, &rules);
    check_eq_i(r.taken, 50, "a client target takes damage whatever the bit says");
    check_eq_i(target.health, 50, "and loses the health");
}

/* ------------------------------------------------------------------------- */
/*
 * ONE SHOT KILL, 0x80058394. The GAME VARIABLES bit that the sim has never
 * been able to reach: `q2_combat_rules` had no field for 0x800B29EC at all.
 */
static void test_one_shot_kill(void)
{
    q2_actor player, mob;
    q2_combat_rules rules;
    q2_damage_result r;

    printf("one shot kill\n");
    q2_combat_rules_default(&rules);
    rules.cheats = Q2_CHEAT_ONE_SHOT_KILL;

    place(&player, 0, 0, 0, 100);
    player.has_client = true;
    r = q2_combat_damage(NULL, &player, 7, Q2_MOD_BULLET, NULL, &rules);
    check_eq_i(player.health, -7, "0x800583E0 stores minus the damage");
    check_eq_i(r.taken, 7, "and the hit is still worth what it was worth");
    check(r.killed, "which kills");

    /*
     * A HIT THE ARMOUR SOAKS WHOLE STILL KILLS. 0x80058390 `subu s1, s1, v0`
     * runs straight into 0x80058394's cheat test with no branch on s1, and the
     * `bgtz` at 0x800583DC stores -0 over a living player. Body armour at
     * skill 1 saves (2048 + 3277*1) >> 12 = 1 of a 1-point bullet: all of it.
     * The port used to return before the store, leaving 100 health and the
     * bullet's hit sound, where 0x800584FC's `blez` on the stored 0 is silent.
     */
    place(&player, 0, 0, 0, 100);
    player.has_client   = true;
    player.armour       = 200;
    player.armour_class = Q2_ARMOUR_BODY;
    r = q2_combat_damage(NULL, &player, 1, Q2_MOD_BULLET, NULL, &rules);
    check(r.absorbed_armour == 1 && player.health == 0,
          "armour soaks the whole point and the cheat still stores -0");
    check(r.killed, "which kills");
    check_eq_i(r.hit_sound_id, 0, "and the hit that stored 0 is silent");

    /* The five mods 0x800583AC..0x800583CC exclude, one at a time. */
    {
        static const s16 excluded[] = {
            Q2_MOD_NONE, Q2_MOD_NO_ARMOUR, Q2_MOD_19, Q2_MOD_LAVA, Q2_MOD_ACID
        };
        static const char *why[] = {
            "mod 0 is excluded",  "mod 8 is excluded",  "mod 19 is excluded",
            "lava is excluded",   "acid is excluded"
        };
        unsigned k;

        /* A clock past the throttle's deadline, so 9 and 10 get in. It was a
         * deadline of -1 against a clock of 0, which only a SIGNED compare
         * lets through; 0x80058260/0x800582A4 are `sltu`, and nothing is
         * below 0 unsigned. */
        rules.level_time = 1000;
        for (k = 0; k < sizeof(excluded) / sizeof(excluded[0]); k++) {
            place(&player, 0, 0, 0, 100);
            player.has_client = true;
            player.env_next   = 0;
            q2_combat_damage(NULL, &player, 7, excluded[k], NULL, &rules);
            check_eq_i(player.health, 93, why[k]);
        }
    }

    /*
     * IT ONLY EVER HURTS THE PLAYER. The block sits past 0x800582C8's
     * `beq s0, zero`, so a client-less target never reaches it — the cheat does
     * not make your shots fatal to anything else.
     */
    place(&mob, 0, 0, 0, 100);
    q2_combat_damage(NULL, &mob, 7, Q2_MOD_BULLET, NULL, &rules);
    check_eq_i(mob.health, 93, "a creature takes the ordinary subtract");

    /* 0x800583DC is `bgtz` on the health BEFORE the store, so an already-dead
     * body takes the ordinary subtract too. */
    place(&player, 0, 0, 0, 100);
    player.has_client = true;
    player.health = -5;
    q2_combat_damage(NULL, &player, 7, Q2_MOD_BULLET, NULL, &rules);
    check_eq_i(player.health, -12, "an already-dead client is just subtracted");

    /* And with the bit clear nothing changes. */
    rules.cheats = 0;
    place(&player, 0, 0, 0, 100);
    player.has_client = true;
    q2_combat_damage(NULL, &player, 7, Q2_MOD_BULLET, NULL, &rules);
    check_eq_i(player.health, 93, "with the bit clear it is an ordinary hit");
}

/* ------------------------------------------------------------------------- */
/*
 * The per-mod hit sound, 0x800ACE5C. Twenty-one words, dumped and decoded.
 */
static void test_hit_sound(void)
{
    q2_actor player, mob;
    q2_combat_rules rules;
    q2_damage_result r;
    s16 vol;
    int m;

    printf("hit sound\n");
    q2_combat_rules_default(&rules);

    check_eq_i(q2_mod_hit_sound(1, &vol), 13, "mod 1 plays id 13");
    check_eq_i(vol, 4096, "at full volume");

    {
        static const s16 loud[] = { 3, 7, 19, 20, 21 };
        int k;
        for (k = 0; k < 5; k++) {
            check_eq_i(q2_mod_hit_sound(loud[k], &vol), 15,
                       "0x80058544's arm is id 15");
            check_eq_i(vol, 4096, "at 4096");
        }
    }

    {
        static const s16 quiet[] = { 5, 6, 18 };
        int k;
        for (k = 0; k < 3; k++) {
            check_eq_i(q2_mod_hit_sound(quiet[k], &vol), 16,
                       "0x80058538's arm is id 16");
            check_eq_i(vol, 2048, "at half volume");
        }
    }

    for (m = 0; m <= 22; m++) {
        bool sounds = (m == 1) || (m == 3) || (m == 5) || (m == 6) ||
                      (m == 7) || (m == 18) || (m == 19) || (m == 20) ||
                      (m == 21);
        if (!sounds)
            check_eq_i(q2_mod_hit_sound((s16)m, NULL), 0,
                       "every other mod is silent");
    }

    /* And through the damage function, under 0x800584D4's two guards. */
    place(&player, 0, 0, 0, 100);
    player.has_client = true;
    r = q2_combat_damage(NULL, &player, 20, Q2_MOD_BULLET, NULL, &rules);
    check_eq_i(r.hit_sound_id, 16, "a surviving player reports the bullet's id");
    check_eq_i(r.hit_sound_vol, 2048, "and its volume");

    /* 0x800584FC's `blez` reads the health AFTER the store: this is the sound
     * of surviving a hit, so the hit that kills is silent. */
    place(&player, 0, 0, 0, 20);
    player.has_client = true;
    r = q2_combat_damage(NULL, &player, 20, Q2_MOD_BULLET, NULL, &rules);
    check_eq_i(r.hit_sound_id, 0, "the hit that takes health to zero is silent");

    /* 0x800584D4 wants a client block. */
    place(&mob, 0, 0, 0, 100);
    r = q2_combat_damage(NULL, &mob, 20, Q2_MOD_BULLET, NULL, &rules);
    check_eq_i(r.hit_sound_id, 0, "and a creature makes none of it");

    /* A silent mod through a surviving player stays silent. */
    place(&player, 0, 0, 0, 100);
    player.has_client = true;
    r = q2_combat_damage(NULL, &player, 20, Q2_MOD_GRENADE, NULL, &rules);
    check_eq_i(r.hit_sound_id, 0, "mod 13's table entry is the silent arm");

    /*
     * A hit the armour soaks whole goes on to the sound AND the effect tail:
     * nothing between 0x80058390 and 0x800585A4 branches on the amount. Body
     * armour's energy column saves (2048 + 2458) >> 12 = 1 of a 1-point bolt.
     * The port's old early return played the sound and never lit the player.
     */
    place(&player, 0, 0, 0, 100);
    player.has_client   = true;
    player.armour       = 200;
    player.armour_class = Q2_ARMOUR_BODY;
    r = q2_combat_damage(NULL, &player, 1, Q2_MOD_ENERGY_BOLT, NULL, &rules);
    check(r.taken == 0 && r.hit_sound_id == 13 && player.effect[1] == 3,
          "a bolt the armour soaks is heard, and lights the player (+0x2F1)");
}

/* ------------------------------------------------------------------------- */
/*
 * AND THE "SOUND" IS A RUMBLE: 0x8005859C hands those operands to
 * SetShockData(victim's window, id, strength, NULL). A spy on the shock sink
 * sees every request the damage function makes, in order.
 */
typedef struct shock_call {
    int  player, id;
    s32  strength;
    bool positional;
} shock_call;

static shock_call g_shock[8];
static int        g_shock_n;

static void spy_shock(void *user, int player, int id, s32 strength,
                      const s32 pos[3], s16 near_r, s16 far_r)
{
    (void)user; (void)near_r; (void)far_r;
    if (g_shock_n < (int)(sizeof(g_shock) / sizeof(g_shock[0]))) {
        g_shock[g_shock_n].player     = player;
        g_shock[g_shock_n].id         = id;
        g_shock[g_shock_n].strength   = strength;
        g_shock[g_shock_n].positional = pos != NULL;
    }
    g_shock_n++;
}

static void test_hit_rumble(void)
{
    q2_actor player, mob;
    q2_combat_rules rules;

    printf("a hit on a player rumbles that player's pad (0x8005859C)\n");
    q2_combat_rules_default(&rules);
    q2_shock_attach(spy_shock, NULL);

    /* A bullet on player 2, who survives it: id 16 at 2048, on window 2,
     * not positional. */
    g_shock_n = 0;
    place(&player, 0, 0, 0, 100);
    player.has_client = true;
    player.owner      = 2;
    q2_combat_damage(NULL, &player, 20, Q2_MOD_BULLET, NULL, &rules);
    check_eq_i(g_shock_n, 1, "one request for one surviving hit");
    check_eq_i(g_shock[0].player, 2, "on the victim's own window");
    check_eq_i(g_shock[0].id, Q2_SHOCK_IMPACT_MERGEABLE,
               "ShockData_Impact_Mergeable for a bullet");
    check_eq_i(g_shock[0].strength, 2048, "at half strength");
    check(!g_shock[0].positional, "and with a3 = NULL, not positional");

    /* Mod 1 and a contact hit take the other two arms. */
    g_shock_n = 0;
    place(&player, 0, 0, 0, 100);
    player.has_client = true;
    player.owner      = 0;
    q2_combat_damage(NULL, &player, 5, Q2_MOD_ENERGY_BOLT, NULL, &rules);
    q2_combat_damage(NULL, &player, 5, Q2_MOD_RAIL, NULL, &rules);
    check_eq_i(g_shock_n, 2, "two hits, two requests");
    check(g_shock[0].id == Q2_SHOCK_EXPLOSION && g_shock[0].strength == 4096,
          "mod 1 is ShockData_Explosion at 4096");
    check(g_shock[1].id == Q2_SHOCK_IMPACT_DISTINCT &&
          g_shock[1].strength == 4096,
          "mod 3 is ShockData_Impact_Distinct at 4096");

    /* The three things that raise nothing: the killing hit (0x800584FC reads
     * the health after the store), a silent mod, and a target without a
     * client (0x800584D4). */
    g_shock_n = 0;
    place(&player, 0, 0, 0, 20);
    player.has_client = true;
    player.owner      = 0;
    q2_combat_damage(NULL, &player, 20, Q2_MOD_BULLET, NULL, &rules);
    check_eq_i(g_shock_n, 0, "the hit that kills shakes nothing");

    place(&player, 0, 0, 0, 100);
    player.has_client = true;
    player.owner      = 0;
    q2_combat_damage(NULL, &player, 20, Q2_MOD_ROCKET, NULL, &rules);
    q2_combat_damage(NULL, &player, 20, Q2_MOD_GRENADE, NULL, &rules);
    check_eq_i(g_shock_n, 0, "a rocket's and a grenade's mods are silent");

    place(&mob, 0, 0, 0, 100);
    q2_combat_damage(NULL, &mob, 20, Q2_MOD_BULLET, NULL, &rules);
    check_eq_i(g_shock_n, 0, "and a creature has no pad");

    /* Splash reaches the same tail: the BFG's blast is mod 1 and hurts its
     * own shooter by half (0x80050A0C), who survives it and feels it. */
    {
        q2_actor *list[1];

        g_shock_n = 0;
        place(&player, 0, 0, 0, 100);
        player.has_client = true;
        player.owner      = 1;
        list[0] = &player;
        q2_combat_radius_damage(&player, NULL, player.origin, 50, 1300,
                                Q2_MOD_ENERGY_BOLT, list, 1, &rules);
        check(g_shock_n == 1 && g_shock[0].player == 1 &&
              g_shock[0].id == Q2_SHOCK_EXPLOSION,
              "a player inside his own BFG blast rumbles once, as id 13");
    }

    q2_shock_attach(NULL, NULL);
    g_shock_n = 0;
    place(&player, 0, 0, 0, 100);
    player.has_client = true;
    player.owner      = 0;
    q2_combat_damage(NULL, &player, 20, Q2_MOD_BULLET, NULL, &rules);
    check_eq_i(g_shock_n, 0, "with no sink attached the request goes nowhere");
}

/* ------------------------------------------------------------------------- */
/*
 * EVERY DEADLINE IS COMPARED UNSIGNED. The five tests are `sltu`:
 * 0x80058238 (the suit, acid only), 0x8005824C (invulnerability, acid),
 * 0x80058260 and 0x800582A4 (the two throttles) and 0x80058314 (the general
 * invulnerability test). A signed `<` agrees with them only while both words
 * are below 0x80000000; each case below sits on the far side of that.
 */
static void test_deadlines_are_unsigned(void)
{
    q2_actor player;
    q2_combat_rules rules;
    q2_damage_result r;

    printf("unsigned deadlines\n");
    q2_combat_rules_default(&rules);
    rules.level_time = 1000;

    /* 0x80058314: a deadline of 0xFFFFFFFF is still ahead of the clock. */
    place(&player, 0, 0, 0, 100);
    player.has_client   = true;
    player.invuln_until = -1;
    r = q2_combat_damage(NULL, &player, 30, Q2_MOD_ROCKET, NULL, &rules);
    check(r.blocked && player.health == 100,
          "invulnerability until 0xFFFFFFFF refuses the rocket");

    /* 0x80058238: the suit's deadline, the same way. */
    place(&player, 0, 0, 0, 100);
    player.has_client    = true;
    player.protect_until = -1;
    r = q2_combat_damage(NULL, &player, 30, Q2_MOD_ACID, NULL, &rules);
    check(r.blocked && player.health == 100,
          "the suit until 0xFFFFFFFF refuses the acid");

    /* 0x80058260 and 0x800582A4: a throttle of 0xFFFFFFFF is not below the
     * clock, so both arms refuse. */
    place(&player, 0, 0, 0, 100);
    player.has_client = true;
    player.env_next   = -1;
    r = q2_combat_damage(NULL, &player, 30, Q2_MOD_ACID, NULL, &rules);
    check(r.blocked && player.health == 100,
          "acid's throttle at 0xFFFFFFFF refuses");

    place(&player, 0, 0, 0, 100);
    player.has_client = true;
    player.env_next   = -1;
    r = q2_combat_damage(NULL, &player, 30, Q2_MOD_LAVA, NULL, &rules);
    check(r.blocked && player.health == 100,
          "and so does lava's");

    /* And the clock itself: at 0xFFFFFFFF it is past a deadline of 1000. */
    rules.level_time = -1;
    place(&player, 0, 0, 0, 100);
    player.has_client   = true;
    player.invuln_until = 1000;
    r = q2_combat_damage(NULL, &player, 30, Q2_MOD_ROCKET, NULL, &rules);
    check(!r.blocked && player.health == 70,
          "a clock of 0xFFFFFFFF is past invulnerability until 1000");
}

/* ------------------------------------------------------------------------- */
/*
 * ENTITY+222 OUTLIVES THE REFRESH, and deathmatch's two "no store" arms mean
 * what the console means by them.
 *
 * q2_actor_init used to seed -1 — the one value the death voice cries for —
 * and both refreshes re-ran it, so a raw -1 came back on every refresh that was
 * not followed by a hit (q2_sim_fire's, the owner's own splash). The seed is
 * now 0x8003DE34's 4, and the byte, the mod and the effect timers are carried.
 */
static void test_killer_byte_survives(void)
{
    q2_actor player, creature, mob, victim, shooter;
    q2_inventory inv;
    q2_monster m;
    q2_combat_rules rules, dm;
    s32 pos[3] = { 0, 0, 0 };

    printf("killer byte\n");
    q2_combat_rules_default(&rules);
    dm = rules;
    dm.deathmatch = true;

    /* 0x8003DE24 `addiu v0, zero, 4` / 0x8003DE34 `sb v0, 222(s1)`. */
    q2_actor_init(&player);
    check_eq_i(player.last_attacker, Q2_MP_NOT_A_PLAYER,
               "a fresh actor starts at 4, as 0x8003DE34 places a player");

    /*
     * A Soldier's bolt kills the player: byte 4 (single player, an attacker
     * with no client), mod 1, +0x2F1 = 3. Then the sim refreshes the actor for
     * something that is not a hit — what q2_sim_fire does on every shot.
     */
    q2_inventory_init(&inv);
    inv.health = 10;
    q2_actor_from_player(&player, &inv, pos);
    place(&creature, 0, 0, -1000, 100);           /* owner -1, no client */
    q2_combat_damage(&creature, &player, 30, Q2_MOD_ENERGY_BOLT, NULL, &rules);
    q2_actor_to_player(&player, &inv);
    q2_actor_from_player(&player, &inv, pos);

    check_eq_i(player.last_attacker, Q2_MP_NOT_A_PLAYER,
               "the player refresh keeps +222");
    check_eq_i(player.last_mod, Q2_MOD_ENERGY_BOLT,
               "and +223, which the death handler reads at 0x800396C4");
    check_eq_i(player.effect[1], 3,
               "and the effect timer the hit armed");
    check(!q2_player_death_cries_out(player.last_attacker, player.last_mod),
          "so the Soldier's kill still dies in silence (0x80039728)");

    /*
     * The creature refresh keeps the same two things. Player 2 shoots a
     * creature in deathmatch (0x80057E04 writes 2 into the creature), and the
     * next frame rebuilds its actor from the monster.
     */
    q2_monster_init(&m);
    m.health     = 100;
    m.takedamage = Q2_DAMAGE_AIM;
    q2_actor_init(&mob);
    q2_actor_from_monster(&mob, &m);
    place(&shooter, 0, 0, -1000, 100);
    shooter.has_client = true;
    shooter.owner      = 2;
    q2_combat_damage(&shooter, &mob, 10, Q2_MOD_ENERGY_BOLT, NULL, &dm);
    q2_actor_from_monster(&mob, &m);

    check_eq_i(mob.effect[1], 3,
               "a creature's damage effect outlives the frame it was armed in");
    check_eq_i(mob.last_attacker, 2, "and so does who last hurt it");

    /*
     * 0x80057E54..0x80057E68: the creature hands its own byte on. It kills
     * player 1 after player 2 shot it, and player 2 is credited.
     */
    place(&victim, 0, 0, 0, 100);
    victim.has_client = true;
    victim.owner      = 1;
    q2_combat_damage(&mob, &victim, 30, Q2_MOD_BULLET, NULL, &dm);
    check_eq_i(victim.last_attacker, 2,
               "a creature's hit in deathmatch credits whoever last hurt it");

    /* 0x80057E5C..0x80057E84: a creature whose own byte is 4 prints and
     * stores nothing, so the victim keeps what the last hit left. */
    victim.last_attacker = 3;
    place(&creature, 0, 0, -1000, 100);
    q2_combat_damage(&creature, &victim, 5, Q2_MOD_BULLET, NULL, &dm);
    check_eq_i(victim.last_attacker, 3,
               "a creature nobody has hurt leaves the victim's byte alone");

    /* 0x80057DC0: the world's hit on a client-less target stores nothing. */
    q2_combat_damage(NULL, &mob, 5, Q2_MOD_CRUSH, NULL, &dm);
    check_eq_i(mob.last_attacker, 2,
               "a deathmatch world hit leaves a creature's byte alone");
}

static void test_knockback(void)
{
    q2_actor target, attacker;
    q2_combat_rules rules;
    s32 point[3] = { 0, 0, -100 };

    printf("knockback\n");
    q2_combat_rules_default(&rules);

    /*
     * BLAST FORCE's reset value, 0x80020494/0x80020498. A memset struct gives
     * mass 0, and the scale is 125*(mass+64)>>6 — 125 instead of 250 — so every
     * impulse in the game was exactly half the disc's until this was seeded.
     * The tests below set 64 by hand and so never saw it.
     */
    check_eq_i(rules.knockback_mass, 64,
               "q2_combat_rules_default seeds BLAST FORCE with 0x80020498's 64");

    /*
     * And the halving is measurable: the two scales are 125*(0+64)>>6 = 125
     * and 125*(64+64)>>6 = 250, so the same hit pushes exactly twice as far.
     * Measured on Z, which has no ceiling at either skill, and at 48 points of
     * damage, which divides the 2400 so neither answer is truncated.
     */
    {
        q2_actor half, full;
        q2_combat_rules zero = rules;
        q2_actor from;
        s32 p[3] = { 0, 0, -100 };

        zero.knockback_mass = 0;
        place(&from, 0, 0, -1000, 100);

        place(&half, 0, 0, 0, 100);
        q2_combat_damage(&from, &half, 48, Q2_MOD_ROCKET, p, &zero);
        place(&full, 0, 0, 0, 100);
        q2_combat_damage(&from, &full, 48, Q2_MOD_ROCKET, p, &rules);

        printf("  mass 0 -> %d, mass 64 -> %d\n",
               (int)half.knockback[2], (int)full.knockback[2]);
        check(half.knockback[2] == 640 && full.knockback[2] == 1280,
              "mass 64 pushes exactly twice as hard as the zero the rules "
              "used to carry");
    }

    check(q2_mod_knocks_back(Q2_MOD_ROCKET), "the rocket pushes");
    check(q2_mod_knocks_back(Q2_MOD_BULLET), "so does a bullet");
    check(!q2_mod_knocks_back(Q2_MOD_LAVA), "lava does not");
    check(!q2_mod_knocks_back(Q2_MOD_CRUSH), "nor does being crushed");

    place(&target, 0, 0, 0, 100);
    place(&attacker, 0, 0, -1000, 100);
    rules.knockback_mass = 64;

    q2_combat_damage(&attacker, &target, 100, Q2_MOD_ROCKET, point, &rules);
    check(target.knocked, "a living target records the impulse");
    check(target.knockback[2] > 0, "pushed away from the blast");

    /* A mod that does not knock back leaves it alone. */
    place(&target, 0, 0, 0, 100);
    q2_combat_damage(&attacker, &target, 100, Q2_MOD_LAVA, point, &rules);
    check_eq_i(target.knockback[2], 0, "lava imparts nothing");

    /*
     * FL_NO_KNOCKBACK GATES NOTHING ON THIS DISC. The outer function's impulse
     * block, 0x80057EC0..0x80058204, reads no flags word at all — its only
     * guards are `beq s4, zero` (zero damage) and `beq s6, zero` (no point).
     * The bit's one reader is 0x8006291C, inside T_Damage, and it zeroes
     * T_Damage's own `knockback` argument, which its single caller passes as
     * zero at 0x80058468. This side used to apply it to the outer impulse.
     */
    place(&target, 0, 0, 0, 100);
    target.no_knockback = true;
    q2_combat_damage(&attacker, &target, 100, Q2_MOD_ROCKET, point, &rules);
    check(target.knockback[2] > 0,
          "FL_NO_KNOCKBACK does not stop the outer impulse");

    /*
     * Self-damage is 3.2 times as strong: the rocket jump. Measured on a
     * horizontal axis, because the vertical one is capped at -3072 outside
     * deathmatch and both cases would hit the cap.
     */
    {
        q2_actor self, other;
        s32 p[3] = { 0, 0, -100 };

        place(&self, 0, 0, 0, 100);
        self.has_client = true;
        q2_combat_damage(&self, &self, 20, Q2_MOD_ROCKET, p, &rules);

        place(&other, 0, 0, 0, 100);
        other.has_client = true;
        q2_combat_damage(&attacker, &other, 20, Q2_MOD_ROCKET, p, &rules);

        check(self.knockback[2] > other.knockback[2] * 3,
              "self-knockback is more than three times as strong");
    }

    /* The upward cap, which only single player has. */
    {
        q2_actor under;
        q2_combat_rules dm;
        s32 below[3] = { 0, 4000, 0 };   /* Y grows downward: this is beneath */

        q2_combat_rules_default(&dm);
        dm.deathmatch = true;
        dm.knockback_mass = 64;

        place(&under, 0, 0, 0, 400);
        under.has_client = true;
        q2_combat_damage(&under, &under, 100, Q2_MOD_ROCKET, below, &rules);
        check_eq_i(under.knockback[1], -3072, "single player caps the lift");

        place(&under, 0, 0, 0, 400);
        under.has_client = true;
        q2_combat_damage(&under, &under, 100, Q2_MOD_ROCKET, below, &dm);
        check(under.knockback[1] < -3072, "deathmatch does not");
    }
}

/* ------------------------------------------------------------------------- */
/* A stand-in for the pair of gates, 0x80044C44's swept move and 0x80053974's
 * entity-box clip: the blast cannot see anything above it. */
static bool block_positive_z(void *ctx, const s32 from[3], const s32 to[3])
{
    (void)ctx;
    return to[2] <= from[2];
}

static void test_splash(void)
{
    q2_actor a[3];
    q2_actor *list[3] = { &a[0], &a[1], &a[2] };
    q2_combat_rules rules;
    s32 centre[3] = { 0, 0, 0 };
    u32 hurt;

    printf("splash\n");
    q2_combat_rules_default(&rules);

    place(&a[0], 0, 0, 0, 500);        /* on top of it        */
    place(&a[1], 0, 0, 500, 500);      /* half a radius away  */
    place(&a[2], 0, 0, 9000, 500);     /* well outside        */
    a[0].radius = a[1].radius = a[2].radius = 0;

    hurt = q2_combat_radius_damage(NULL, NULL, centre, 200, 1300,
                                   Q2_MOD_ROCKET, list, 3, &rules);
    check_eq_i(hurt, 2, "two of three are inside the blast");
    check_eq_i(a[0].health, 300, "the one at the centre takes all of it");
    check(a[1].health > 300 && a[1].health < 500, "the near one takes less");
    check_eq_i(a[2].health, 500, "the far one takes none");

    /* The falloff itself, which is a read constant. */
    check_eq_i(q2_combat_splash_at(200, 0), 200, "no loss at the centre");
    check_eq_i(q2_combat_splash_at(200, 4096), 200 - 170, "170/4096 per unit");
    check_eq_i(q2_combat_splash_at(10, 100000), 0, "never goes negative");

    /* An actor's own radius extends the blast's reach. */
    place(&a[0], 0, 0, 1400, 500);
    a[0].radius = 0;
    hurt = q2_combat_radius_damage(NULL, NULL, centre, 200, 1300,
                                   Q2_MOD_ROCKET, list, 1, &rules);
    check_eq_i(hurt, 0, "a point target just outside is missed");

    place(&a[0], 0, 0, 1400, 500);
    a[0].radius = 286;
    hurt = q2_combat_radius_damage(NULL, NULL, centre, 200, 1300,
                                   Q2_MOD_ROCKET, list, 1, &rules);
    check_eq_i(hurt, 1, "a body-sized one at the same place is caught");

    /*
     * THE OWNER TAKES HALF, 0x80050A0C. `bne a2, s4` at 0x80050A04 compares the
     * candidate against the owner the caller passed in a0, and the `sra` is on
     * the owner's arm only; 0x80050A08's subtraction is in the delay slot and
     * therefore runs either way. All six projectile call sites store zero in
     * the exclude-owner slot, so the owner IS swept — the halving is the whole
     * of what makes self-splash different, and this side had none of it.
     */
    {
        q2_actor owner, bystander;
        q2_actor *pair[2];
        s16 full;

        place(&owner, 0, 0, 400, 500);
        place(&bystander, 0, 0, -400, 500);
        owner.radius = bystander.radius = 0;
        pair[0] = &owner;
        pair[1] = &bystander;

        full = q2_combat_splash_at(200, 400);
        hurt = q2_combat_radius_damage(&owner, NULL, centre, 200, 1300,
                                       Q2_MOD_ROCKET, pair, 2, &rules);
        check_eq_i(hurt, 2, "both are inside the blast");
        check_eq_i(500 - owner.health, full >> 1,
                   "the blast's own owner takes half");
        check_eq_i(500 - bystander.health, full,
                   "and an equidistant bystander takes all of it");

        /*
         * The shift is BEFORE 0x80050A10's `blez`, not after, so a self-hit
         * that falls off to exactly one point halves to zero and is rejected
         * outright — the owner is not even counted.
         */
        {
            s32 far_dist = 0;
            s16 pts;

            /* Find the distance at which the falloff leaves exactly 1. */
            for (far_dist = 0; far_dist < 40000; far_dist++) {
                pts = q2_combat_splash_at(200, far_dist);
                if (pts == 1)
                    break;
            }
            check_eq_i(q2_combat_splash_at(200, far_dist), 1,
                       "a distance exists where the falloff leaves 1 point");

            place(&owner, 0, 0, far_dist, 500);
            place(&bystander, 0, 0, -far_dist, 500);
            owner.radius = bystander.radius = 0;
            hurt = q2_combat_radius_damage(&owner, NULL, centre, 200,
                                           (s16)(far_dist + 1), Q2_MOD_ROCKET,
                                           pair, 2, &rules);
            check_eq_i(hurt, 1, "the owner's 1-point self-hit is rejected");
            check_eq_i(owner.health, 500, "and takes nothing at all");
            check_eq_i(bystander.health, 499, "while the bystander takes its 1");
        }
    }

    /*
     * OCCLUSION, 0x80050A24 and 0x80050A3C. Retail runs the swept move
     * 0x80044C44 through the hull and then clips the same segment against the
     * entity boxes (0x80053974) between the falloff and the damage call, and a
     * zero from either skips the candidate. `q2_combat_radius_damage` has no
     * world, so the pair is injected; the plain entry point passes NULL and
     * keeps its old distance-only behaviour for headless callers.
     */
    {
        q2_actor near_pair[2];
        q2_actor *pair[2] = { &near_pair[0], &near_pair[1] };

        place(&near_pair[0], 0, 0, 400, 500);
        place(&near_pair[1], 0, 0, -400, 500);
        near_pair[0].radius = near_pair[1].radius = 0;

        hurt = q2_combat_radius_damage_traced(NULL, NULL, centre, 200, 1300,
                                              Q2_MOD_ROCKET, pair, 2, &rules,
                                              block_positive_z, NULL);
        check_eq_i(hurt, 1, "an occluded candidate is not counted");
        check_eq_i(near_pair[0].health, 500, "and takes nothing through a wall");
        check(near_pair[1].health < 500, "while the visible one is hurt");
    }
}

/* ------------------------------------------------------------------------- */
static void test_hitscan(void)
{
    q2_actor a[2];
    q2_actor *list[2] = { &a[0], &a[1] };
    q2_actor shooter;
    q2_combat_rules rules;
    s32 origin[3] = { 0, 0, 0 };
    s32 dir[3]    = { 0, 0, 16384 };   /* the bullet path's own range */
    q2_damage_result r;
    s32 idx;

    printf("hitscan\n");
    q2_combat_rules_default(&rules);
    place(&shooter, 0, 0, -1000, 100);

    place(&a[0], 0, 0, 8000, 100);
    place(&a[1], 0, 0, 4000, 100);
    a[0].radius = a[1].radius = 100;

    idx = q2_combat_fire_bullet(&shooter, origin, dir, 8, 4096, 100,
                                list, 2, &rules, &r);
    check_eq_i(idx, 1, "a bullet stops at the nearer target");
    check_eq_i(a[1].health, 92, "which takes the damage");
    check_eq_i(a[0].health, 100, "and the far one is shielded");

    /*
     * THE SWEEP FILTERS ON `takedamage`, NOT ON HEALTH — which is the whole
     * reason a corpse can be gibbed. A body at negative health is still a
     * target; one whose `takedamage` is clear is not, and the bullet passes
     * through it to whatever is behind.
     */
    place(&a[0], 0, 0, 8000, 100);
    place(&a[1], 0, 0, 4000, -50);        /* a corpse, and still shootable */
    a[0].radius = a[1].radius = 100;
    idx = q2_combat_fire_bullet(&shooter, origin, dir, 8, 4096, 100,
                                list, 2, &rules, &r);
    check_eq_i(idx, 1, "a corpse is still a target");
    check_eq_i(a[1].health, -58, "and takes the hit");

    /*
     * BUT `takedamage` IS DoDamage'S TEST, NOT THE SWEEP'S. 0x800544EC skips
     * only its ignore argument, so a body that cannot be hurt still stops the
     * round — and shields whatever stands behind it. DoDamage's first test
     * (0x80057D54) is what refuses the damage.
     */
    place(&a[0], 0, 0, 8000, 100);
    place_untouchable(&a[1], 0, 0, 4000, 100);
    a[0].radius = a[1].radius = 100;
    idx = q2_combat_fire_bullet(&shooter, origin, dir, 8, 4096, 100,
                                list, 2, &rules, &r);
    check_eq_i(idx, 1, "a body with takedamage clear still stops the round");
    check_eq_i(a[1].health, 100, "takes nothing from it");
    check_eq_i(a[0].health, 100, "and shields the one behind");

    /* The rail is one query and one DoDamage (0x80049318, 0x80049334): the
     * PC game's penetration is not this console's. */
    place(&a[0], 0, 0, 8000, 200);
    place(&a[1], 0, 0, 4000, 200);
    a[0].radius = a[1].radius = 100;
    check_eq_i(q2_combat_fire_rail(&shooter, origin, dir, 100, 4096, 100,
                                   list, 2, &rules), 1,
               "the rail stops in the nearer body");
    check_eq_i(a[1].health, 100, "which takes the hit");
    check_eq_i(a[0].health, 200, "and the far one is shielded");

    /* A world surface in the way shortens the trace. */
    place(&a[0], 0, 0, 8000, 100);
    place(&a[1], 0, 0, 4000, 100);
    a[0].radius = a[1].radius = 100;
    idx = q2_combat_fire_bullet(&shooter, origin, dir, 8, 512, 100,
                                list, 2, &rules, &r);
    check_eq_i(idx, -1, "a wall at 1/8 of the range stops the bullet");
    check_eq_i(a[1].health, 100, "and nothing behind it is hit");

    /* Nothing behind the shooter is ever hit. */
    place(&a[0], 0, 0, -4000, 100);
    a[0].radius = 100;
    idx = q2_combat_fire_bullet(&shooter, origin, dir, 8, 4096, 100,
                                list, 1, &rules, &r);
    check_eq_i(idx, -1, "a target behind is not hit");
}

/* ------------------------------------------------------------------------- */
/* 0x800544EC does not measure a 3-D sphere. It solves an X/Z cylinder and a
 * Y slab separately, then uses the first fraction common to both intervals. */
static void test_actor_cylinder_trace(void)
{
    q2_actor corner, inside;
    q2_actor broad, narrow;
    q2_actor *list[2];
    q2_combat_rules rules;
    q2_damage_result damage;
    q2_monster corpse;
    s32 corner_origin[3] = { -1000, 0, 5550 };
    s32 corner_dir[3]    = { 2000, 0, -2000 };
    s32 beam_origin[3]   = { 0, 0, 0 };
    s32 beam_dir[3]      = { 0, 0, 10000 };
    s32 idx;

    printf("actor cylinder trace\n");
    q2_combat_rules_default(&rules);

    /* x+z(relative to the centre) stays 550: the line clips the old 572-unit
     * AABB at its X/Z corner, but remains 550/sqrt(2) outside radius 286. */
    place(&corner, 0, 0, 4000, 100);
    list[0] = &corner;
    idx = q2_combat_nearest_on_segment(corner_origin, corner_dir,
                                        Q2_HITSCAN_RADIUS, list, 1);
    check_eq_i(idx, -1, "an X/Z box corner is outside the retail cylinder");

    /* Move the parallel line inward: 400/sqrt(2) is just inside the circle. */
    place(&inside, 0, 0, 4000, 100);
    corner_origin[2] = 5400;
    list[0] = &inside;
    idx = q2_combat_nearest_on_segment(corner_origin, corner_dir,
                                        Q2_HITSCAN_RADIUS, list, 1);
    check_eq_i(idx, 0, "the parallel sweep hits inside the X/Z circle");

    /*
     * The monster projection must carry the corpse resize. A standing sphere
     * of radius 429 would catch y=100; the anchored quarter-height slab does
     * not.
     *
     * THE THREE NUMBERS BELOW CHANGED, and both changes are the same fix.
     * This test used to want height 143 and a hull shortened to +/-71, because
     * the port derived the hit cylinder from `mins`/`maxs` and scaled those.
     * The console does neither. `GetCreHitSize` (0x8006DC1C) takes the pair
     * from the bound MODEL's header at +0x18/+0x1A and falls back to a literal
     * 286 and **600** at 0x8006DC6C/0x8006DC74 — not 572, which is what the
     * old 143 was a quarter of. This corpse has no model bound, so it takes
     * that fallback and its quarter-height is 600/4 = 150.
     *
     * And `QuakeDeadHitCollision` (0x8007F77C) writes exactly two halfwords:
     * obj+0x42 at 0x8007F798 and obj+0x40 at 0x8007F7BC, which are the hit
     * cylinder's height and radius. obj+0x18..0x22 — the +/-286 movement hull
     * — appears nowhere in the function, so a body keeps the box it walked
     * around in and the Y bounds stay where they were.
     */
    q2_monster_init(&corpse);
    corpse.pos[2] = 4000;
    corpse.health = -20;
    corpse.gib_health = -60;
    q2_monster_corpse_detach(&corpse);
    corpse.takedamage = Q2_DAMAGE_YES;
    q2_actor_from_monster(&corner, &corpse);
    check_eq_i(corner.radius, 429, "a detached body projects its wider radius");
    check_eq_i(corner.height, 150, "and the quarter of GetCreHitSize's 600");
    check_eq_i(corner.mins[1], -286, "and the movement hull it is NOT shrunk by");
    check_eq_i(corner.maxs[1], 286, "...on both faces");

    beam_origin[1] = 100;
    list[0] = &corner;
    idx = q2_combat_nearest_on_segment(beam_origin, beam_dir,
                                        Q2_HITSCAN_RADIUS, list, 1);
    check_eq_i(idx, -1, "a ray above the corpse slab passes over the body");
    beam_origin[1] = 144;
    idx = q2_combat_nearest_on_segment(beam_origin, beam_dir,
                                        Q2_HITSCAN_RADIUS, list, 1);
    check_eq_i(idx, 0, "one unit inside the anchored corpse slab hits it");

    /* Nearest means earliest surface entry, not nearest centre and not target
     * array order. The broad far-centred actor starts at z=3000; the narrow
     * near-centred one starts at z=3900. */
    place(&broad, 0, 0, 6000, 200);
    broad.radius = 3000;
    place(&narrow, 0, 0, 4000, 200);
    narrow.radius = 100;
    list[0] = &narrow;
    list[1] = &broad;
    beam_origin[1] = 0;
    idx = q2_combat_nearest_on_segment(beam_origin, beam_dir,
                                        Q2_HITSCAN_RADIUS, list, 2);
    check_eq_i(idx, 1, "nearest selection uses cylinder entry fraction");

    /* A world stop after that near face but before the actor's centre must not
     * occlude it. Conversely, moving the stop before the face must. */
    idx = q2_combat_fire_bullet(NULL, beam_origin, beam_dir, 8, 1300,
                                Q2_HITSCAN_RADIUS, list + 1, 1,
                                &rules, &damage);
    check_eq_i(idx, 0, "a near face before the wall is hittable");
    check_eq_i(broad.health, 192, "the pre-wall hit delivers damage");
    idx = q2_combat_fire_bullet(NULL, beam_origin, beam_dir, 8, 1200,
                                Q2_HITSCAN_RADIUS, list + 1, 1,
                                &rules, &damage);
    check_eq_i(idx, -1, "a wall before the entry face occludes the actor");

    /* The rail asks the same query once: whichever cylinder is entered
     * first takes it, whatever the list order. */
    broad.health = narrow.health = 200;
    check_eq_i(q2_combat_fire_rail(NULL, beam_origin, beam_dir, 25, 4096,
                                   Q2_HITSCAN_RADIUS, list, 2, &rules), 1,
               "the rail takes one body");
    check_eq_i(broad.health, 175, "the broad cylinder, entered first");
    check_eq_i(narrow.health, 200, "and not the narrow one listed before it");

    /* The retail paths never approach this range, but the host API accepts
     * s32 coordinates. This makes b^2 and 4ac exceed s64 in the naive formula
     * while the short local geometry is still an ordinary centre hit. */
    place(&inside, INT_MAX - 200000, 0, INT_MAX - 200000, 100);
    beam_origin[0] = INT_MAX - 300000;
    beam_origin[1] = 0;
    beam_origin[2] = INT_MAX - 200000;
    beam_dir[0] = 200000;
    beam_dir[1] = 0;
    beam_dir[2] = 0;
    list[0] = &inside;
    idx = q2_combat_nearest_on_segment(beam_origin, beam_dir,
                                        Q2_HITSCAN_RADIUS, list, 1);
    check_eq_i(idx, 0, "large legal s32 coordinates do not overflow tracing");

    /* The exact arm's broad box can admit a large-radius centre which is still
     * outside the cylinder. Reject the negative discriminant before its x4:
     * scaling first exceeds s64 for this otherwise legal public-API input. */
    beam_origin[0] = beam_origin[1] = beam_origin[2] = 0;
    beam_dir[0] = 30000;
    beam_dir[1] = 0;
    beam_dir[2] = 30000;
    place(&inside, 30000, 0, -29999, 100);
    inside.radius = 30000;
    list[0] = &inside;
    idx = q2_combat_nearest_on_segment(beam_origin, beam_dir,
                                        Q2_HITSCAN_RADIUS, list, 1);
    check_eq_i(idx, -1, "large negative discriminant is rejected without overflow");
}

/* ------------------------------------------------------------------------- */
static void test_mod_classification(void)
{
    printf("means of death\n");

    /* The sixteen-entry table at 0x800ACE1C. */
    check(q2_mod_is_energy(Q2_MOD_ENERGY_BOLT), "a bolt is energy damage");
    check(q2_mod_is_energy(Q2_MOD_LASER), "so is a laser");
    check(!q2_mod_is_energy(Q2_MOD_RAIL), "the rail is not");
    check(!q2_mod_is_energy(Q2_MOD_GRENADE), "nor is a grenade");
    check(!q2_mod_is_energy(Q2_MOD_ROCKET), "nor a rocket");
    check(!q2_mod_is_energy(Q2_MOD_BULLET),
          "and a bullet is past the table's bound, so ordinary");

    {
        int slot = -1;
        check_eq_i(q2_mod_effect_timer(Q2_MOD_2, &slot), 15, "mod 2 arms 15");
        check_eq_i(slot, 0, "in the first slot");
        check_eq_i(q2_mod_effect_timer(Q2_MOD_4, &slot), 30, "mod 4 arms 30");
        check_eq_i(q2_mod_effect_timer(Q2_MOD_BULLET, &slot), 0,
                   "a bullet arms nothing");
    }
}

/* ------------------------------------------------------------------------- */

/*
 * The energy-bolt effect's light gate. 0x80058660 is `sltiu v0, v0, 3` — below
 * three takes a different branch — so the lit arm is `>= 3`, and MOD_ENERGY_BOLT
 * arms slot 1 with exactly 3. The boundary is the whole content of this: an
 * implementation using `> 0` would light the entire tail of the effect.
 */
static void test_energy_light_gate(void)
{
    q2_actor a;

    memset(&a, 0, sizeof(a));
    check(!q2_actor_energy_lit(&a), "unarmed is dark");

    a.effect[1] = 1;
    check(!q2_actor_energy_lit(&a), "1 is below the gate");
    a.effect[1] = 2;
    check(!q2_actor_energy_lit(&a), "2 is below the gate");
    a.effect[1] = 3;
    check(q2_actor_energy_lit(&a),  "3 lights, the value the mod arms");
    a.effect[1] = 255;
    check(q2_actor_energy_lit(&a),  "and anything above it");

    /* Slot 1 only: the other three mods arm other slots and must not light. */
    memset(&a, 0, sizeof(a));
    a.effect[0] = 15;
    a.effect[2] = 30;
    a.effect[4] = 5;
    check(!q2_actor_energy_lit(&a), "the other three slots do not light");

    {
        int slot = -1;
        check_eq_i(q2_mod_effect_timer(Q2_MOD_ENERGY_BOLT, &slot), 3,
                 "the bolt arms with 3");
        check_eq_i(slot, 1, "...in slot 1");
    }
}

/* ------------------------------------------------------------------------- */
/*
 * WHAT A PANE DOES AFTER IT BREAKS — PSXGLASS's tail, 0x8002A4AC..0x8002A4F4.
 *
 * Everything asserted here used to be missing. The port threw the debris, took
 * the pane out of the collision set and stopped: the geometry went on drawing
 * for as long as the zone was loaded, and no window in the game ever made a
 * sound. The console does three more things on a break, and two of them are
 * BELOW the branch at 0x8002A3CC so they happen on the EVE_ replay pass too:
 *
 *   0x8002A490/0x8002A4A4  play Sound_GlassSmash — `amb_brkglas` — at the
 *                          centre of the pane's Scene node box (gated)
 *   0x8002A4C8..0x8002A4E0 set bit 15, do-not-draw, in that node's flags
 *   0x8002A4E4..0x8002A4F4 free the damageable box and clear the callback
 *
 * The box is BASE1's own pane, node 221, so the centre is a real one: all three
 * sums are odd, and the console's `addu`/`srl 31`/`addu`/`sra 1` rounds each
 * towards zero. A shift by one would floor and give (-10784, -1298, -16959),
 * which is why the numbers below are written out rather than computed.
 */
static void wr_le32(u8 *p, s32 v)
{
    u32 u = (u32)v;

    p[0] = (u8)(u & 0xFF);
    p[1] = (u8)((u >> 8) & 0xFF);
    p[2] = (u8)((u >> 16) & 0xFF);
    p[3] = (u8)((u >> 24) & 0xFF);
}

/* The sim is far larger than a test's stack frame, so both of these are file
 * scope rather than automatic. */
static q2_sim  g_glass_sim;
static u8      g_glass_node[Q2_SCENE_NODE_SIZE];

static void test_glass_break_tail(void)
{
    q2_sim *sim = &g_glass_sim;
    q2_scene scene;
    q2_breakable *b;
    s16 hidden_node = -1;
    u8  hidden = 0;
    s32 at[3];
    /* BASE1 zone 0: "breakable 0: GLASS node 221, 10 hp, 1+20 pieces". */
    const s32 bmin[3] = { -10879, -1918, -17919 };
    const s32 bmax[3] = { -10688,  -677, -15998 };
    const s32 centre[3] = { -10783, -1297, -16958 };
    /* Straight through the pane, along +z, from outside its box. */
    s32 from[3] = { -10783, -1297, -19000 };
    s32 to[3]   = { -10783, -1297, -15000 };
    int k;

    printf("glass break tail\n");

    memset(g_glass_node, 0, sizeof(g_glass_node));
    for (k = 0; k < 3; k++) {
        wr_le32(g_glass_node + 0x10 + k * 4, bmin[k]);
        wr_le32(g_glass_node + 0x1C + k * 4, bmax[k]);
    }
    memset(&scene, 0, sizeof(scene));
    scene.nodes      = g_glass_node;
    scene.node_count = 1;

    q2_sim_init(sim, NULL, 50);
    sim->breakable_scene = &scene;
    sim->breakable_count = 1;

    b = &sim->breakable[0];
    memset(b, 0, sizeof(*b));
    b->solid_target = -1;      /* no movement box in this fixture */
    b->scene_node   = 0;       /* index 0 of the one-node scene above */
    b->health       = 10;
    b->kind         = Q2_BREAKABLE_GLASS;
    b->count_a      = 1;
    b->count_b      = 20;
    for (k = 0; k < 3; k++) {
        b->bmin[k] = bmin[k];
        b->bmax[k] = bmax[k];
    }

    /* A SURVIVING HIT CHANGES NOTHING. 0x8002A3AC returns while the hit points
     * are above zero, long before either queue. */
    (void)q2_sim_breakable_shot(sim, from, to, 9);
    check(!b->broken, "a surviving hit leaves the pane standing");
    check(!q2_sim_next_node_vis(sim, &hidden_node, &hidden),
          "...and queues no hide");
    check(!q2_sim_next_glass_smash(sim, at), "...and no smash");

    /* The fatal one. */
    (void)q2_sim_breakable_shot(sim, from, to, 1);
    check(b->broken, "the fatal hit frees the pane's box (0x8002A4E4)");
    check(q2_sim_next_node_vis(sim, &hidden_node, &hidden),
          "and queues the node's visibility change (0x8002A4DC)");
    check_eq_i(hidden_node, 0, "...for the pane's own node");
    check_eq_i(hidden, 1, "...hidden, which is what bit 15 means");
    check(!q2_sim_next_node_vis(sim, &hidden_node, &hidden),
          "...exactly once");

    check(q2_sim_next_glass_smash(sim, at),
          "and queues the smash (0x8002A4A4)");
    check_eq_i(at[0], centre[0], "at the box centre in x, rounded towards zero");
    check_eq_i(at[1], centre[1], "...in y");
    check_eq_i(at[2], centre[2], "...in z");
    check(!q2_sim_next_glass_smash(sim, at), "...exactly once");

    /*
     * THE SCRIPT PATH DOES THE SAME, because it is the same function: a CALL
     * carries damage 0 and 0x8002A390 falls straight through to the shatter.
     * This is the path a zone seam replays, and the hide below the branch is
     * why a pane the script already broke comes back broken and invisible.
     */
    {
        /* One UserFuncs record, "GLASS", at per-map index 0. */
        static u8 uf[4 + Q2_UF_RECORD_SIZE];
        /* A 16-byte GLASS item: object slot at +4, hit points at +6, the two
         * piece counts at +10 and +12. */
        static u8 raw[16];
        q2_event_item item;

        memset(uf, 0, sizeof(uf));
        uf[0] = 1;                       /* count, low halfword of the u32 */
        memcpy(uf + 4, "GLASS", 5);

        memset(raw, 0, sizeof(raw));
        raw[4]  = 0;  raw[5] = 0;        /* slot 0 */
        raw[6]  = 10; raw[7] = 0;        /* hit points */
        raw[10] = 1;                     /* param_a, the hit burst */
        raw[12] = 20;                    /* param_b, the shatter */

        memset(&item, 0, sizeof(item));
        item.op      = Q2_EVOP_CALL;
        item.opcode  = Q2_EVOP_CALL;
        item.len     = 16;
        item.payload = raw + 2;

        sim->userfuncs.data  = uf;
        sim->userfuncs.size  = sizeof(uf);
        sim->userfuncs.count = 1;
        sim->userfuncs_ready = true;

        b->broken = false;               /* a fresh pane for the script to break */

        (void)q2_sim_breakable_call(sim, &scene, NULL, &item, 0);
        check(b->broken, "a scripted GLASS call frees the box as well");
        check(q2_sim_next_node_vis(sim, &hidden_node, &hidden),
              "...and hides the node, which the port never did");
        check_eq_i(hidden, 1, "...hidden");
        check(q2_sim_next_glass_smash(sim, at), "...and smashes audibly");
        check_eq_i(at[2], centre[2], "...at the same centre");

        /*
         * AND ON THE REPLAY PASS the hide still happens and the smash does not:
         * 0x8002A3CC branches TO 0x8002A4AC, so the sound is skipped and the
         * tail is not. Event_MuteLevFlag (0x800B2834) is `initial_pass` here.
         */
        b->broken = false;
        sim->event_rt.initial_pass = true;
        (void)q2_sim_breakable_call(sim, &scene, NULL, &item, 0);
        sim->event_rt.initial_pass = false;

        check(b->broken, "the replay frees the box too (below the branch)");
        check(q2_sim_next_node_vis(sim, &hidden_node, &hidden),
              "...and hides the node too");
        check(!q2_sim_next_glass_smash(sim, at),
              "...but plays nothing, which is what the mute flag is for");
    }

    q2_sim_free(sim);
}

static void test_native_projectile_contacts(void)
{
    size_t i;
    int mismatches = 0;
    for (i = 0; i < Q2PSX_ARRAY_COUNT(native_projectile_contacts); i++) {
        q2_actor actors[4], *list[4];
        s32 point[3], hit;
        s32 end[3];
        int j, k;
        for (j = 0; j < 4; j++) {
            q2_actor_init(&actors[j]);
            memcpy(actors[j].origin, native_projectile_contacts[i].actors[j].centre,
                   sizeof(actors[j].origin));
            actors[j].radius = native_projectile_contacts[i].actors[j].radius;
            actors[j].height = (s16)native_projectile_contacts[i].actors[j].height;
            actors[j].takedamage = (u8)native_projectile_contacts[i].actors[j].damageable;
            list[j] = &actors[j];
        }
        for (k = 0; k < 3; k++) end[k] = native_projectile_contacts[i].start[k] +
                                       native_projectile_contacts[i].delta[k];
        hit = q2_combat_trace_actors(native_projectile_contacts[i].start, end,
                  native_projectile_contacts[i].skip >= 0 ? list[native_projectile_contacts[i].skip] : NULL,
                  list, (u32)native_projectile_contacts[i].count, point);
        g_checks++;
        if (hit != native_projectile_contacts[i].hit ||
            memcmp(point, native_projectile_contacts[i].point, sizeof(point))) {
            if (mismatches < 5) printf("  native contact %zu: hit %d/%d, point %d,%d,%d / %d,%d,%d\n",
                i, hit, native_projectile_contacts[i].hit, point[0], point[1], point[2],
                native_projectile_contacts[i].point[0], native_projectile_contacts[i].point[1],
                native_projectile_contacts[i].point[2]);
            mismatches++;
        }
    }
    g_failures += mismatches;
    printf("Native actor queries: %zu cases, %d mismatches\n",
           Q2PSX_ARRAY_COUNT(native_projectile_contacts), mismatches);
}

typedef struct {
    const native_autoaim_case *row;
    int count;
} aim_visibility;

static bool native_aim_visible(void *user, const s32 start[3], const s32 end[3], int ray)
{
    aim_visibility *state = user;
    const native_autoaim_case *r = state->row;
    int n = state->count++, k;
    check(n < r->visible_count, "autoaim visibility count bounded");
    if (n < r->visible_count) {
        check_eq_i(ray, r->visibility[n].ray, "autoaim visibility ray");
        for (k = 0; k < 3; k++) {
            check_eq_i(start[k], r->visibility[n].start[k], "autoaim visibility start");
            check_eq_i(end[k], r->visibility[n].end[k], "autoaim visibility end");
        }
    }
    return !(r->blocked & (1u << ray));
}

static void test_native_autoaim(void)
{
    size_t i;
    for (i = 0; i < Q2PSX_ARRAY_COUNT(native_autoaim); i++) {
        const native_autoaim_case *r = &native_autoaim[i];
        q2_actor actors[5], *targets[5];
        aim_visibility visibility = {r, 0};
        s16 direction[3] = {111, -222, 333}, pitch = -44;
        s32 hit;
        int j, k;
        memset(actors, 0, sizeof(actors));
        for (j = 0; j < 5; j++) {
            targets[j] = &actors[j];
            for (k = 0; k < 3; k++) actors[j].origin[k] = r->actors[j][k];
            actors[j].radius = r->actors[j][3];
            actors[j].height = (s16)r->actors[j][4];
            /* No health, damageability, or screen visibility admission. */
        }
        hit = q2_combat_find_target(r->missing_view ? NULL : r->eye, r->angles,
                                   &actors[0], targets, 5, native_aim_visible,
                                   &visibility, direction, &pitch);
        check_eq_i(hit, r->result[3], "autoaim selected actor");
        check_eq_i(pitch, r->result[4], "autoaim pitch");
        check_eq_i(visibility.count, r->visible_count, "autoaim visibility count");
        for (k = 0; k < 3; k++) check_eq_i(direction[k], r->result[k], "autoaim direction");
    }
    printf("Native auto-aim: %zu PAL/US cases\n", Q2PSX_ARRAY_COUNT(native_autoaim));
}

int main(void)
{
    printf("combat behaviour\n\n");

    test_player_powerup_sync();
    test_ray_distance();
    test_armour_absorption();
    test_power_armour();
    test_native_signed_armour();
    test_zero_and_negative_damage();
    test_armour_fade_flag();
    test_damage();
    test_takedamage_gate();
    test_one_shot_kill();
    test_hit_sound();
    test_hit_rumble();
    test_deadlines_are_unsigned();
    test_killer_byte_survives();
    test_knockback();
    test_splash();
    test_hitscan();
    test_actor_cylinder_trace();
    test_native_projectile_contacts();
    test_native_autoaim();
    test_mod_classification();

    test_energy_light_gate();
    test_glass_break_tail();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
