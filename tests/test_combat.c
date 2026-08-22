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

#include "combat.h"
#include "monster.h"
#include "multiplayer.h"
#include "projectile.h"
#include "trig.h"
#include "weapon.h"

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

    /* The comparison in 0x80058244 is strict: the expiry tick itself hurts. */
    inv.invuln_until = 0;
    inv.enviro_until = 300;
    q2_actor_from_player(&player, &inv, pos);
    rules.level_time = 300;
    hit = q2_combat_damage(NULL, &player, 40, Q2_MOD_LAVA, pos, &rules);
    check(!hit.blocked, "the protection expiry tick is no longer protected");
    check(player.health < 100, "damage resumes on the expiry tick");
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

    /* Jacket armour, 0.30 normal protection, with the single-player bias of
     * 4095 that rounds every non-zero fraction up. */
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

    /* Deathmatch swaps the bias to 2048, which rounds to nearest instead of
     * up — very slightly weaker armour against small hits. */
    {
        q2_combat_rules dm;
        q2_combat_rules_default(&dm);
        dm.deathmatch = true;

        place(&a, 0, 0, 0, 100);
        a.has_client = true;
        a.armour = 100;
        a.armour_class = 0;
        save = q2_combat_armour_absorb(&a, 1, false, false, &dm);
        check_eq_i(save, (2048 + 1229) >> 12, "deathmatch bias rounds down");

        place(&a, 0, 0, 0, 100);
        a.has_client = true;
        a.armour = 100;
        a.armour_class = 0;
        save = q2_combat_armour_absorb(&a, 1, false, false, &rules);
        check_eq_i(save, 1, "single-player bias rounds a 1-point hit up");
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

        q2_combat_damage(&attacker, &target, 31, Q2_MOD_BULLET, NULL, &easy);
        check_eq_i(target.health, 100 - 16, "skill 0 halves, rounding up");

        place(&target, 0, 0, 0, 100);
        attacker.has_client = true;           /* now a player hurt a player */
        q2_combat_damage(&attacker, &target, 31, Q2_MOD_BULLET, NULL, &easy);
        check_eq_i(target.health, 100 - 31, "but not player-on-player");
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

        /* World damage leaves -1, and attribution keeps it that way. */
        place(&victim, 0, 0, 0, 100);
        q2_combat_damage(NULL, &victim, 30, Q2_MOD_BULLET, NULL, &rules);
        check_eq_i(victim.last_attacker, -1, "world damage has no attacker");
        check_eq_i(q2_mp_attribute_kill(victim.last_attacker, victim.last_mod),
                   -1, "and stays nobody's frag");

        /* A hazard is nobody's frag even when a player last touched you. */
        place(&victim, 0, 0, 0, 100);
        q2_combat_damage(&shooter, &victim, 5, Q2_MOD_LAVA, NULL, &rules);
        check_eq_i(q2_mp_attribute_kill(victim.last_attacker, victim.last_mod),
                   -1, "lava is nobody's frag");
    }

    /* A corpse floors at -9999 however hard it is hit. */
    place(&target, 0, 0, 0, 10);
    r = q2_combat_damage(NULL, &target, 30000, Q2_MOD_BULLET, NULL, &rules);
    check(r.killed, "a big hit kills");
    check_eq_i(target.health, Q2_HEALTH_FLOOR, "and health floors at -9999");
}

static void test_knockback(void)
{
    q2_actor target, attacker;
    q2_combat_rules rules;
    s32 point[3] = { 0, 0, -100 };

    printf("knockback\n");
    q2_combat_rules_default(&rules);

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

    place(&a[0], 0, 0, 8000, 100);
    place_untouchable(&a[1], 0, 0, 4000, 100);
    a[0].radius = a[1].radius = 100;
    idx = q2_combat_fire_bullet(&shooter, origin, dir, 8, 4096, 100,
                                list, 2, &rules, &r);
    check_eq_i(idx, 0, "a target with takedamage clear is passed through");
    check_eq_i(a[1].health, 100, "and is untouched");

    /* The rail does not stop. */
    place(&a[0], 0, 0, 8000, 200);
    place(&a[1], 0, 0, 4000, 200);
    a[0].radius = a[1].radius = 100;
    check_eq_i(q2_combat_fire_rail(&shooter, origin, dir, 100, 4096, 100,
                                   list, 2, &rules), 2,
               "the rail passes through both");
    check_eq_i(a[0].health, 100, "hurting the far one too");

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

int main(void)
{
    printf("combat behaviour\n\n");

    test_player_powerup_sync();
    test_ray_distance();
    test_armour_absorption();
    test_power_armour();
    test_damage();
    test_knockback();
    test_splash();
    test_hitscan();
    test_mod_classification();

    test_energy_light_gate();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
