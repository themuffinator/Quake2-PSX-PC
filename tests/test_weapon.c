/*
 * test_weapon.c — firing, selection and projectiles.
 *
 * The numbers asserted here are transcribed from the eleven fire functions, so
 * a failure is a divergence from the console rather than from a guess. The
 * comment beside each check names what makes it checkable.
 */
#include <stdio.h>
#include <string.h>

#include "projectile.h"
#include "weapon.h"
#include "random.h"
#include "shock.h"
#include "worldscale.h"   /* Q2_DT_NOMINAL, Q2_GRAVITY, Q2_VEL_DIV */
#include "fixtures/toss_reference.h"
#include "fixtures/projectile_contact_reference.h"

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

static void give_all(q2_inventory *inv)
{
    int i;
    q2_inventory_init(inv);
    inv->weapons = 0x7FF;                    /* all eleven bits             */
    for (i = 0; i < Q2_AMMO_COUNT; i++)
        inv->ammo[i] = 200;
}

/* ------------------------------------------------------------------------- */
static void test_bios_random(void)
{
    static const s32 expected[] = {16838, 5758, 10113, 17515, 31051, 5627, 23010, 7419};
    q2_rng rng;
    u32 i;
    printf("portable BIOS random sequence\n");
    q2_rng_seed(&rng, 1);
    q2_random_seed(1);
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        check_eq_i(q2_rng_next(&rng), expected[i], "BIOS seed-1 known answer");
        check_eq_i(q2_random_next(), expected[i], "shared stream uses BIOS sequence");
    }
    q2_random_seed(1);
    (void)q2_random_next();
    check_eq_i(q2_random_state(), 0x41C67EA6u, "state retains all 32 bits");
    q2_random_seed(0);
    check_eq_i(q2_random_state(), 0, "zero is a valid seed");
    check_eq_i(q2_random_next(), 0, "zero seed's first answer");
    check_eq_i(q2_random_next(), 21468, "zero seed advances instead of sticking");
}

static void test_tables(void)
{
    const q2_weapon_tables *t = q2_weapon_tables_builtin();

    printf("weapon tables\n");

    /* The one-based reading, from the column that proves it. */
    check_eq_i(t->ammo_per_shot[Q2_WID_BFG], 50, "the BFG costs fifty cells");
    check_eq_i(t->ammo_per_shot[Q2_WID_BLASTER], 0, "the blaster costs none");
    check_eq_i(t->ammo_per_shot[Q2_WID_SUPER_SHOTGUN], 2,
               "the super shotgun costs two shells");

    check_eq_i(t->ammo_type[Q2_WID_RAILGUN], Q2_AMMO_SLUGS, "railgun: slugs");
    check_eq_i(t->ammo_type[Q2_WID_BFG], Q2_AMMO_CELLS, "BFG: cells");
    check_eq_i(t->ammo_type[Q2_WID_CHAINGUN], Q2_AMMO_BULLETS,
               "chaingun: bullets");

    check_eq_i(t->owned_bit[Q2_WID_BLASTER], 1, "the blaster is bit 0");
    check_eq_i(t->owned_bit[Q2_WID_BFG], 0x400, "the BFG is bit 10");
    check_eq_i(t->owned_bit[12], 0,
               "the phantom twelfth slot can never be owned");

    /* The armour classes, which are PC Quake II's own numbers. */
    check_eq_i(t->armour[0].normal_protection, 1229, "jacket 0.30");
    check_eq_i(t->armour[1].normal_protection, 2458, "combat 0.60");
    check_eq_i(t->armour[2].normal_protection, 3277, "body 0.80");
    check_eq_i(t->armour[0].energy_protection, 0, "jacket stops no energy");
    check_eq_i(t->armour[2].max_count, 200, "body armour caps at 200");

    /* The auto-switch order, explosives absent. */
    check_eq_i(t->autoswitch_count, 8, "eight weapons auto-switch");
    check_eq_i(t->autoswitch[0], Q2_WID_RAILGUN, "the railgun is preferred");
    check_eq_i(t->autoswitch[7], Q2_WID_BLASTER, "the blaster is last");
    {
        u32 i;
        bool has_bfg = false;
        for (i = 0; i < t->autoswitch_count; i++)
            if (t->autoswitch[i] == Q2_WID_BFG)
                has_bfg = true;
        check(!has_bfg, "and the BFG is not in the list at all");
    }
}

/* ------------------------------------------------------------------------- */
static void test_selection(void)
{
    q2_inventory inv;

    printf("selection\n");

    /* Only the blaster: cycling finds nothing else and says so. */
    q2_inventory_init(&inv);
    inv.weapons = 1;
    check_eq_i(q2_weapon_cycle(&inv, Q2_WID_BLASTER, +1), Q2_WID_NONE,
               "with one weapon there is nothing to cycle to");
    check_eq_i(q2_weapon_autoselect(&inv), Q2_WID_BLASTER,
               "and auto-select falls back to it");

    give_all(&inv);
    check_eq_i(q2_weapon_autoselect(&inv), Q2_WID_RAILGUN,
               "with everything, the railgun wins");
    check_eq_i(q2_weapon_cycle(&inv, Q2_WID_BLASTER, +1), Q2_WID_SHOTGUN,
               "next from the blaster is the shotgun");
    check_eq_i(q2_weapon_cycle(&inv, Q2_WID_BFG, +1), Q2_WID_BLASTER,
               "next from the BFG wraps past the phantom slot to the blaster");
    check_eq_i(q2_weapon_cycle(&inv, Q2_WID_BLASTER, -1), Q2_WID_BFG,
               "previous from the blaster wraps to the BFG");

    /* A weapon with no ammo is skipped. */
    give_all(&inv);
    inv.ammo[Q2_AMMO_SHELLS] = 0;
    check_eq_i(q2_weapon_cycle(&inv, Q2_WID_BLASTER, +1), Q2_WID_MACHINEGUN,
               "both shotguns are skipped when the shells run out");

    /* Exactly one shot's worth still counts. */
    give_all(&inv);
    inv.ammo[Q2_AMMO_SHELLS] = 2;
    check(q2_weapon_usable(&inv, Q2_WID_SUPER_SHOTGUN),
          "exactly two shells is enough for the super shotgun");
    inv.ammo[Q2_AMMO_SHELLS] = 1;
    check(!q2_weapon_usable(&inv, Q2_WID_SUPER_SHOTGUN),
          "one is not");
    check(q2_weapon_usable(&inv, Q2_WID_SHOTGUN),
          "but it is enough for the shotgun");
}

/* ------------------------------------------------------------------------- */
static void test_firing(void)
{
    q2_inventory inv;
    q2_rng rng;
    q2_fire_result_v2 r;
    s32 eye[3] = { 0, 0, 0 };
    s16 aim[3] = { 0, 0, 4096 };     /* straight ahead, 1.3.12               */

    printf("firing\n");
    q2_rng_seed(&rng, 1);

    /* The blaster: a bolt, no ammo cost, damage 8. */
    give_all(&inv);
    r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_BLASTER, eye, 0, 0, 0, aim,
                       0, 0, false, false, 0);
    check(r.fired, "the blaster fires");
    check_eq_i(r.kind, Q2_FK_BOLT, "as a bolt");
    check_eq_i(r.shot_count, 1, "one shot");
    check_eq_i(r.shot[0].damage, 8, "for 8");
    /*
     * NO GATE. The fire functions write level_time + 30 to client+204 and
     * nothing in the weapon path ever reads it back — the rate of fire is the
     * fire CLIP, because the console's idle state calls the fire function
     * itself and then enters FIRE. This check used to pin the invented gate.
     */
    check_eq_i(r.next_fire, 0, "and gates nothing: the animation is the rate");
    check_eq_i(r.kick[0], -11, "with a kick of -11");

    /* Quad is a multiply at the fire site, and it is always four. */
    r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_BLASTER, eye, 0, 0, 0, aim,
                       0, 0, true, false, 0);
    check_eq_i(r.shot[0].damage, 32, "quad makes it 32");

    /* The shotgun: five pellets at 6, one shell. */
    give_all(&inv);
    r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_SHOTGUN, eye, 0, 0, 0, aim,
                       0, 0, false, false, 0);
    check_eq_i(r.shot_count, 5, "the shotgun throws five pellets");
    check_eq_i(r.shot[0].damage, 6, "at 6 each");
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 199, "for one shell");

    /* The super shotgun: ten at 8, two shells, and twice the lateral spread. */
    give_all(&inv);
    r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_SUPER_SHOTGUN, eye, 0, 0, 0, aim,
                       0, 0, false, false, 0);
    check_eq_i(r.shot_count, 10, "the super shotgun throws ten");
    check_eq_i(r.shot[0].damage, 8, "at 8 each");
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 198, "for two shells");

    /* The railgun: 100, or 150 in deathmatch. */
    give_all(&inv);
    r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_RAILGUN, eye, 0, 0, 0, aim,
                       0, 0, false, false, 0);
    check_eq_i(r.shot[0].damage, 100, "the railgun does 100");
    check_eq_i(r.kick[0], -34, "with the heaviest kick in the game");
    check_eq_i(r.shot[0].dir[2], 4 * 4096,
               "and reaches 16384, the rail's own `dir << 2` (0x80049224)");

    give_all(&inv);
    r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_RAILGUN, eye, 0, 0, 0, aim,
                       0, 0, false, true, 0);
    check_eq_i(r.shot[0].damage, 150, "and 150 in deathmatch");

    /* The rocket: 100 + rand() % 20, so always in range and never constant. */
    {
        int i;
        bool varied = false;
        s16 first = 0;

        for (i = 0; i < 32; i++) {
            give_all(&inv);
            r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_ROCKET_LAUNCHER,
                               eye, 0, 0, 0, aim, 0, 0, false, false, 0);
            check_eq_i(r.shot[0].damage >= 100 && r.shot[0].damage < 120, 1,
                       "rocket damage stays in 100..119");
            if (i == 0) first = r.shot[0].damage;
            else if (r.shot[0].damage != first) varied = true;
        }
        check(varied, "and is not the same every time");
    }

    /* The BFG's fifty cells, and what happens with forty-nine. */
    give_all(&inv);
    inv.ammo[Q2_AMMO_CELLS] = 50;
    r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_BFG, eye, 0, 0, 0, aim,
                       0, 0, false, false, 0);
    check(r.fired, "fifty cells fires the BFG");
    check_eq_i(inv.ammo[Q2_AMMO_CELLS], 0, "and spends all of them");

    give_all(&inv);
    inv.ammo[Q2_AMMO_CELLS] = 49;
    r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_BFG, eye, 0, 0, 0, aim,
                       0, 0, false, false, 0);
    check(!r.fired, "forty-nine does not");
    check(r.dry, "and reports the dry click");
    check_eq_i(r.sound, Q2_WSND_NO_AMMO, "which is wep_noammo");
    check_eq_i(inv.ammo[Q2_AMMO_CELLS], 49, "spending nothing");

    /* The refire gate is checked before ammo, so a blocked shot is free. */
    give_all(&inv);
    r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_SHOTGUN, eye, 0, 0, 0, aim,
                       10, 30, false, false, 0);
    check(!r.fired, "a shot inside the refire window is refused");
    check(!r.dry, "without claiming to be out of ammo");
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 200, "and costs nothing");

    /* The hyperblaster sets no gate: its animation paces it. */
    give_all(&inv);
    r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_HYPERBLASTER, eye, 0, 0, 0, aim,
                       100, 0, false, false, 0);
    check_eq_i(r.next_fire, 0, "the hyperblaster arms no refire gate");
    check_eq_i(r.shot[0].damage, 20, "and does 20 a bolt");

    /* The chaingun fires as many as its spin state says. */
    give_all(&inv);
    r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_CHAINGUN, eye, 0, 0, 0, aim,
                       0, 0, false, false, 3);
    check_eq_i(r.shot_count, 3, "three bullets from a spun-up chaingun");
    check_eq_i(inv.ammo[Q2_AMMO_BULLETS], 197, "three rounds cost three bullets");
    inv.ammo[Q2_AMMO_BULLETS] = 2;
    r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_CHAINGUN, eye, 0, 0, 0, aim,
                       0, 0, false, false, 3);
    check(r.fired && !r.dry, "the last two chaingun rounds still fire");
    check_eq_i(r.shot_count, 2, "the burst is clamped to available rounds");
    check_eq_i(inv.ammo[Q2_AMMO_BULLETS], 0, "the partial burst spends only its rounds");

    /* Slot 0 is a call that returns. */
    give_all(&inv);
    r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_NONE, eye, 0, 0, 0, aim,
                       0, 0, false, false, 0);
    check(!r.fired, "firing no weapon does nothing");
}

/* ------------------------------------------------------------------------- */
static void test_all_weapon_contracts(void)
{
    static const int damage[] = {0, 8, 6, 8, 8, 8, 120, 120, 100, 20, 100, 200};
    static const int pellets[] = {0, 1, 5, 10, 1, 3, 1, 1, 1, 1, 1, 1};
    const q2_weapon_tables *tab = q2_weapon_tables_builtin();
    const s32 eye[3] = {0, 0, 0};
    const s16 aim[3] = {0, 0, 4096};
    int w, quad, dm;
    printf("all weapon ammo and damage contracts\n");
    for (w = 1; w <= Q2_WID_COUNT; w++) {
        for (quad = 0; quad < 2; quad++) {
            for (dm = 0; dm < 2; dm++) {
                q2_inventory inv;
                q2_rng rng;
                q2_fire_result_v2 r;
                int i, expected = damage[w];
                give_all(&inv);
                q2_rng_seed(&rng, 9);
                r = q2_weapon_fire(&inv, &rng, NULL, w, eye, 0, 0, 0, aim,
                                   100, 0, quad != 0, dm != 0, 3);
                if (w == Q2_WID_RAILGUN && dm) expected = 150;
                if (quad) expected *= 4;
                check(r.fired && !r.dry, "every owned, fed weapon fires");
                check_eq_i(r.shot_count, pellets[w], "correct projectile or pellet count");
                for (i = 0; i < (int)r.shot_count; i++) {
                    if (w == Q2_WID_ROCKET_LAUNCHER)
                        check(r.shot[i].damage >= expected &&
                              r.shot[i].damage <= expected + (quad ? 76 : 19),
                              "rocket roll and quad stay within retail limits");
                    else
                        check_eq_i(r.shot[i].damage, expected,
                                   "normal, quad and deathmatch damage");
                }
            }
        }
        {
            q2_inventory inv;
            q2_rng rng;
            q2_fire_result_v2 r;
            int type = tab->ammo_type[w], cost = (int)tab->ammo_per_shot[w];
            give_all(&inv);
            q2_rng_seed(&rng, 9);
            inv.weapons &= (u16)~tab->owned_bit[w];
            r = q2_weapon_fire(&inv, &rng, NULL, w, eye, 0, 0, 0, aim,
                               100, 0, false, false, 3);
            check(!r.fired && !r.dry && r.shot_count == 0,
                  "an unowned weapon cannot fire or dry-click");
            if (!cost) continue;
            give_all(&inv);
            inv.ammo[type] = (s16)(cost - 1);
            r = q2_weapon_fire(&inv, &rng, NULL, w, eye, 0, 0, 0, aim,
                               100, 0, false, false, 3);
            check(r.dry && !r.fired && r.shot_count == 0,
                  "insufficient ammo produces no damage");
            check_eq_i(inv.ammo[type], cost - 1, "a dry attempt spends nothing");
            r = q2_weapon_fire(&inv, &rng, NULL, w, eye, 0, 0, 0, aim,
                               101, r.next_fire, false, false, 3);
            check(!r.dry && !r.fired && r.sound == -1,
                  "holding an empty trigger respects the dry-click gate");
        }
    }
}

static void test_muzzle(void)
{
    const q2_weapon_tables *t = q2_weapon_tables_builtin();
    s32 eye[3] = { 1000, 2000, 3000 };
    s32 out[3];

    printf("muzzle\n");

    /*
     * Facing +Z with no pitch, the blaster's triple lands on the axes it names.
     *
     * THE MIDDLE COMPONENT IS DOWN. This check used to expect `2000 - 56` under
     * the comment "56 up, which is -56 in world Y", and that expectation is
     * where the misaligned muzzle came from. weapontables.h saw the fire
     * function's `subu` on the way IN to the rotation and concluded the stored
     * value was down-and-must-be-flipped; there is a second `subu` at
     * 0x8004C04C on the rotated Y coming OUT, and the two cancel. The stored
     * triple is (right, DOWN, forward) and it stays down.
     */
    q2_weapon_muzzle_origin(t->muzzle[Q2_WID_BLASTER], eye, 0, 0, 0, out);
    check_eq_i(out[0], 1000 + 80, "80 to the right");
    check_eq_i(out[1], 2000 + 56, "56 DOWN — the executable negates it twice");
    check_eq_i(out[2], 3000 + 250, "250 forward");

    /* Turned a quarter circle, right and forward trade places. */
    q2_weapon_muzzle_origin(t->muzzle[Q2_WID_BLASTER], eye, 1024, 0, 0, out);
    check_eq_i(out[0], 1000 + 250, "forward is now +X");
    check_eq_i(out[2], 3000 - 80, "and right is -Z");

    /*
     * AND THE BASIS IS ORTHONORMAL AT EVERY PITCH, which is the half of the bug
     * that pitch 0 could never show. The old hand-rolled basis negated one
     * component of the up row, so dot(up, fwd) = 2 sin p cos p: at a 45-degree
     * pitch `up` and `fwd` were the SAME vector and the vertical part of the
     * offset was applied along forward.
     *
     * A rotation preserves length, so the offset's distance from the eye is the
     * same at every angle. One unit of tolerance for the 1.3.12 rounding.
     */
    {
        static const s32 k_pitch[4] = { 0, 512, 1024, -512 };
        s64 len0 = 0;
        int p;

        for (p = 0; p < 4; p++) {
            s64 dx, dy, dz, len;

            q2_weapon_muzzle_origin(t->muzzle[Q2_WID_BLASTER], eye,
                                    700, k_pitch[p], 0, out);
            dx = out[0] - eye[0];
            dy = out[1] - eye[1];
            dz = out[2] - eye[2];
            len = dx * dx + dy * dy + dz * dz;

            if (p == 0)
                len0 = len;
            else
                check(len > len0 - 4 * 4096 && len < len0 + 4 * 4096,
                      "the muzzle offset keeps its length as the pitch turns");
        }
    }

    /*
     * A pure forward offset must land exactly where the aim points, because
     * both are the same rotation applied to +Z. That is the property the shot
     * and the drawn weapon have to share, and it is what fails when the two
     * transforms differ.
     */
    {
        s16 fwd_only[3] = { 0, 0, 4096 };
        s32 a[3], b[3];

        q2_weapon_muzzle_origin(fwd_only, eye, 700, 512, 0, a);
        q2_weapon_muzzle_origin(fwd_only, eye, 700, 512, 0, b);
        check(a[0] == b[0] && a[1] == b[1] && a[2] == b[2],
              "the transform is a pure function of the angles");
        /* +Z through a yaw-only rotation is (sin, 0, cos) — no vertical term
         * at zero pitch, which the broken basis also satisfied, so pitch is
         * where it has to be checked. */
        q2_weapon_muzzle_origin(fwd_only, eye, 0, 1024, 0, a);
        check_eq_i(a[1] - eye[1], -4096,
                   "pitched a quarter circle, forward is straight up");
        check(a[2] - eye[2] > -8 && a[2] - eye[2] < 8,
              "and nothing is left along Z");
    }
}

/* ------------------------------------------------------------------------- */
static void test_projectiles(void)
{
    q2_projectiles list;
    q2_inventory inv;
    q2_rng rng;
    q2_fire_result_v2 r;
    s32 eye[3] = { 0, 0, 0 };
    s16 aim[3] = { 0, 0, 4096 };
    s32 idx;

    printf("projectiles\n");
    q2_rng_seed(&rng, 7);
    q2_projectiles_init(&list);
    give_all(&inv);

    /* Hitscan produces no entity. */
    r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_SHOTGUN, eye, 0, 0, 0, aim,
                       0, 0, false, false, 0);
    check_eq_i(q2_projectile_launch(&list, &r, 0, 0), -1,
               "a shotgun leaves nothing in the world");

    /* A rocket does, with the read splash radius. */
    give_all(&inv);
    r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_ROCKET_LAUNCHER, eye, 0, 0, 0,
                       aim, 0, 0, false, false, 0);
    idx = q2_projectile_launch(&list, &r, 0, 0);
    check(idx >= 0, "a rocket does");
    check_eq_i(list.p[idx].splash_radius, Q2_SPLASH_RADIUS_ROCKET,
               "with a blast radius of 1300");
    check_eq_i(list.p[idx].mod, Q2_MOD_ROCKET, "and mod 15");
    check(list.p[idx].vel[2] > 0, "moving the way it was aimed");
    check_eq_i(list.p[idx].expires, Q2_ROCKET_LIFETIME,
               "and the spawner's 20000-tick lifetime");
    check_eq_i(list.p[idx].vel[2],
               ((s64)(aim[2] * Q2_AIM_SCALE_ROCKET) * 4096) / Q2_VEL_DIV,
               "using the doubled aim as shared-entity velocity");

    /* A rocket that meets a wall damages what is nearby and is consumed. */
    {
        q2_actor near_by, shooter;
        q2_actor *targets[1];
        q2_combat_rules rules;
        s32 point[3] = { 0, 0, 5000 };

        q2_combat_rules_default(&rules);
        q2_actor_init(&near_by);
        near_by.origin[0] = 0; near_by.origin[1] = 0; near_by.origin[2] = 5200;
        near_by.health = 500;
        /* Spawned creatures all have this; 0x80062848 refuses one without. */
        near_by.takedamage = Q2_DAMAGE_YES;
        q2_actor_init(&shooter);
        shooter.health = 100;
        targets[0] = &near_by;

        check(q2_projectile_impact(&list, (u32)idx, point,
                                   &shooter, NULL, targets, 1, &rules),
              "the rocket is consumed by the wall");
        check(near_by.health < 500, "and the blast reaches a bystander");
        check(!list.p[idx].in_use, "the slot is freed");
    }

    /* Its 20000-tick field is only a safety lifetime. At 0x8004ADB0 retail
     * frees the rocket; it does not run the impact/explosion arm. */
    {
        give_all(&inv);
        r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_ROCKET_LAUNCHER,
                           eye, 0, 0, 0, aim, 0, 0, false, false, 0);
        idx = q2_projectile_launch(&list, &r, 0, 0);

        check(q2_projectile_expire(&list, (u32)idx),
              "an elapsed rocket safety lifetime frees the slot");
        check(!list.p[idx].in_use, "the expired rocket is gone quietly");
    }

    /* A bolt's direction is its velocity, and the hyperblaster's is half the
     * blaster's because its shift is one bit deeper. */
    {
        s32 blaster_idx, hyper_idx;

        give_all(&inv);
        r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_BLASTER, eye, 0, 0, 0, aim,
                           0, 0, false, false, 0);
        blaster_idx = q2_projectile_launch(&list, &r, 0, 0);
        check(blaster_idx >= 0, "the blaster spawns a bolt");
        check_eq_i(list.p[blaster_idx].expires, Q2_LIFETIME_BOLT,
                   "with a lifetime of 2560 ticks");
        check_eq_i(list.p[blaster_idx].splash_radius, 0,
                   "and no blast at all");

        give_all(&inv);
        r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_HYPERBLASTER, eye, 0, 0, 0,
                           aim, 0, 0, false, false, 0);
        hyper_idx = q2_projectile_launch(&list, &r, 0, 0);
        check(hyper_idx >= 0, "the hyperblaster spawns one too");
        check_eq_i(list.p[hyper_idx].vel[2],
                   list.p[blaster_idx].vel[2] / 2,
                   "at exactly half the speed");
    }

    /* A grenade is fused and falls. */
    give_all(&inv);
    r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_GRENADE_LAUNCHER, eye, 0, 0, 0,
                       aim, 0, 0, false, false, 0);
    idx = q2_projectile_launch(&list, &r, 0, 100);
    check(idx >= 0, "the launcher spawns a grenade");
    check_eq_i(list.p[idx].expires, 100 + Q2_GRENADE_LAUNCH_FUSE,
               "with the 900 argument as its fuse, not its speed");
    check_eq_i(list.p[idx].splash_radius, Q2_SPLASH_RADIUS_GRENADE,
               "and a blast radius of 1000");
    check(list.p[idx].vel[1] < 0,
          "thrown upward, because the launcher ignores the aim and uses its "
          "own fixed 2048-up-6144-forward direction");

    {
        q2_proj_step step;
        s32 before = list.p[idx].vel[1];

        q2_projectile_step(&list, (u32)idx, 40, Q2_DT_NOMINAL, 101, &step);
        check(list.p[idx].vel[1] > before, "gravity pulls it back down");
        check_eq_i(step.to[1] - step.from[1],
                   ((s64)(-Q2_GRENADE_LAUNCH_UP + 40 * Q2_DT_NOMINAL) *
                    Q2_DT_NOMINAL) / Q2_VEL_DIV,
                   "with the shared mover's signed divide toward zero");
        check(step.to[2] > step.from[2], "while it keeps going forward");
        check(!step.expired, "and the fuse has not run out");
        check_eq_i(step.to[2] - step.from[2],
                   ((s64)Q2_GRENADE_LAUNCH_FORWARD * Q2_DT_NOMINAL) /
                       Q2_VEL_DIV,
                   "and raw forward velocity advances by vel * dt / 320");
    }

    /* Grenade3 is not Grenade2 with another fuse. 0x8004AA6C creates it in
     * hidden state 1, owner-attached with raw charge 4096; 0x8004A368 charges
     * and releases it later, and only that release spends ammo. */
    {
        q2_projectiles hand;
        q2_proj_step st;
        s32 attached[3] = { 111, 222, 333 };
        s32 released_at[3] = { 80, -50, 200 };
        s32 raw_dir[3] = { 0, -Q2_HAND_GRENADE_RELEASE_UP,
                           Q2_HAND_GRENADE_CHARGE_START + 120 };
        s32 before_ammo, h;

        q2_projectiles_init(&hand);
        give_all(&inv);
        before_ammo = inv.ammo[Q2_AMMO_GRENADES];
        r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_HAND_GRENADE,
                           eye, 0, 0, 0, aim, 100, 0,
                           false, false, 0);
        check(r.fired, "a hand grenade primes");
        check_eq_i(inv.ammo[Q2_AMMO_GRENADES], before_ammo,
                   "priming does not spend its grenade");
        check_eq_i(r.sound, -1,
                   "prime/throw sounds belong to model crossings, not fire");

        h = q2_projectile_launch(&hand, &r, 3, 100);
        check(h >= 0, "Grenade3 allocates a held entity");
        check_eq_i(q2_projectile_hand_held_index(&hand, 3), h,
                   "identified by owner while held");
        check_eq_i(hand.p[h].node, Q2_PROJ_NODE_HELD,
                   "with the hidden held-state sentinel");
        check_eq_i(q2_projectile_hand_charge(&hand, 3),
                   Q2_HAND_GRENADE_CHARGE_START,
                   "and raw charge 4096");
        check_eq_i(hand.p[h].expires, 100 + Q2_HAND_GRENADE_FUSE,
                   "while its 1650-tick fuse is already running");

        q2_projectile_step(&hand, (u32)h, Q2_GRAVITY,
                           Q2_DT_NOMINAL, 101, &st);
        check_eq_i(st.to[0], st.from[0], "held Grenade3 does not move in X");
        check_eq_i(st.to[1], st.from[1], "held Grenade3 does not fall");
        check_eq_i(st.to[2], st.from[2], "held Grenade3 does not move forward");

        check(q2_projectile_hand_update(&hand, 3, attached, 20),
              "the owner attaches and cooks it");
        check_eq_i(hand.p[h].pos[0], attached[0], "attachment copies X");
        check_eq_i(hand.p[h].pos[1], attached[1], "attachment copies Y");
        check_eq_i(hand.p[h].pos[2], attached[2], "attachment copies Z");
        check_eq_i(q2_projectile_hand_charge(&hand, 3),
                   Q2_HAND_GRENADE_CHARGE_START + 120,
                   "twenty cook ticks add exactly 6*20 charge");

        check(q2_projectile_hand_release(&hand, 3, released_at, raw_dir),
              "the 411 crossing releases it");
        check_eq_i(hand.p[h].node, Q2_PROJ_NODE_UNKNOWN,
                   "release restores the fresh-entity collision sentinel");
        check_eq_i(q2_projectile_hand_held_index(&hand, 3), -1,
                   "and it is no longer owner-attached");
        check_eq_i(hand.p[h].vel[1],
                   ((s64)raw_dir[1] * 4096) / Q2_VEL_DIV,
                   "release converts raw upward velocity through /320");
        check_eq_i(hand.p[h].vel[2],
                   ((s64)raw_dir[2] * 4096) / Q2_VEL_DIV,
                   "and preserves the charged forward component");

        check(q2_weapon_consume(&inv, Q2_WID_HAND_GRENADE),
              "release can charge the deferred ammo");
        check_eq_i(inv.ammo[Q2_AMMO_GRENADES], before_ammo - 1,
                   "exactly one grenade is spent at release");
    }

    /* BFGBlast rotates a raw length-768 vector, advances it through a private
     * /64 mover, and counts down 2400 at entity+0xF4. */
    {
        q2_projectiles l4;
        q2_proj_step st;
        s32 bfg;

        q2_projectiles_init(&l4);
        give_all(&inv);
        r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_BFG, eye, 0, 0, 0,
                           aim, 0, 0, false, false, 0);
        bfg = q2_projectile_launch(&l4, &r, 0, 37);
        check(bfg >= 0, "the BFG spawns its dedicated blast");
        check_eq_i(l4.p[bfg].expires, 37 + Q2_BFG_LIFETIME,
                   "with the read 2400-tick lifetime");

        q2_projectile_step(&l4, (u32)bfg, Q2_GRAVITY, Q2_DT_NOMINAL, 38, &st);
        check_eq_i(st.to[2] - st.from[2],
                   (Q2_BFG_RAW_SPEED * Q2_DT_NOMINAL) / Q2_BFG_VEL_DIV,
                   "and its custom mover advances raw 768 velocity through /64");
        check_eq_i(st.to[1] - st.from[1], 0,
                   "without shared-entity gravity");
        check(q2_projectile_expire(&l4, (u32)bfg),
              "and the BFG safety lifetime uses the quiet free path");
        check(!l4.p[bfg].in_use, "rather than inventing a delayed BFG blast");
    }

    /* RotMatrix already supplies a fixed-point forward column. Retail rotates
     * local {0,0,768} componentwise and never normalises that column. A short,
     * oblique test vector makes the old Euclidean-normalisation substitution
     * impossible to hide; the negative odd component also pins arithmetic
     * right-shift rounding. */
    {
        q2_projectiles l5;
        s16 matrix_aim[3] = { 2048, -2049, 2048 };
        s32 bfg;
        int k;

        q2_projectiles_init(&l5);
        give_all(&inv);
        r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_BFG, eye, 0, 0, 0,
                           matrix_aim, 0, 0, false, false, 0);
        bfg = q2_projectile_launch(&l5, &r, 0, 0);
        check(bfg >= 0, "the BFG accepts an oblique matrix column");
        for (k = 0; k < 3; k++) {
            s32 raw = (s32)(((s64)matrix_aim[k] * Q2_BFG_RAW_SPEED) >> 12);
            check_eq_i(l5.p[bfg].vel[k],
                       ((s64)raw * 4096) / Q2_BFG_VEL_DIV,
                       "BFG component is fixed multiply, not normalisation");
        }
    }

    /* The fuse does run out. */
    {
        q2_proj_step step;
        q2_projectile_step(&list, (u32)idx, 40, Q2_DT_NOMINAL, 100000, &step);
        check(step.expired, "eventually");
    }

    /*
     * THE STEP IS SCALED BY dt, which it was not — every projectile flew at a
     * twelfth of its speed at the nominal tick and a twentieth in the headless
     * 1/30 s step. 0x80047D40 forms the destination as `pos += vel * dt`.
     */
    {
        q2_projectiles l2;
        q2_proj_step   s1, s2;
        s32 b;

        q2_projectiles_init(&l2);
        give_all(&inv);
        r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_BLASTER, eye, 0, 0, 0,
                           aim, 0, 0, false, false, 0);
        b = q2_projectile_launch(&l2, &r, 0, 0);
        check(b >= 0, "a bolt to measure");

        q2_projectile_step(&l2, (u32)b, 0, Q2_DT_NOMINAL, 1, &s1);
        q2_projectile_step(&l2, (u32)b, 0, Q2_DT_NOMINAL * 2, 1, &s2);

        {
            s64 d1 = (s64)s1.to[2] - s1.from[2];
            s64 d2 = (s64)s2.to[2] - s2.from[2];

            check(d1 != 0, "a bolt actually moves in one tick");
            check(d2 == d1 * 2, "and twice the dt moves it exactly twice as far");
        }

        /* And the absolute figure: a bolt's raw velocity IS its direction,
         * while the list stores the same value in 1.0.12. */
        check_eq_i((s64)s1.to[2] - s1.from[2],
                   ((s64)l2.p[b].vel[2] * Q2_DT_NOMINAL) / 4096,
                   "one tick recovers the raw velocity and multiplies by dt");
    }

    /*
     * And the grenade's gravity is in the PROJECTILE's scale, not the player's.
     * `+= gravity` added 32 to a 1.0.12 velocity — 0.008 units a tick — so the
     * launcher had no arc at all.
     */
    {
        q2_projectiles l3;
        q2_proj_step   st;
        s32 g, before, tick;

        q2_projectiles_init(&l3);
        give_all(&inv);
        r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_GRENADE_LAUNCHER, eye, 0, 0, 0,
                           aim, 0, 0, false, false, 0);
        g = q2_projectile_launch(&l3, &r, 0, 0);
        check(g >= 0, "a grenade to measure");

        before = l3.p[g].vel[1];
        q2_projectile_step(&l3, (u32)g, Q2_GRAVITY, Q2_DT_NOMINAL, 1, &st);
        check_eq_i(l3.p[g].vel[1] - before,
                   ((s64)Q2_GRAVITY * Q2_DT_NOMINAL * 4096) / Q2_VEL_DIV,
                   "one tick of gravity is g * dt * 4096 / Q2_VEL_DIV");

        /* Which is the same fall the PLAYER integrator produces: the player
         * adds g*dt to a velocity that moves it by vel*dt/Q2_VEL_DIV, so the
         * position delta changes by g*dt*dt/Q2_VEL_DIV either way. */
        check_eq_i(((s64)(l3.p[g].vel[1] - before) * Q2_DT_NOMINAL) >> 12,
                   ((s64)Q2_GRAVITY * Q2_DT_NOMINAL * Q2_DT_NOMINAL) / Q2_VEL_DIV,
                   "and it falls at the same rate the player does");

        for (tick = 0; tick < 100; tick++)
            q2_projectile_step(&l3, (u32)g, Q2_GRAVITY,
                               Q2_DT_NOMINAL, 1, &st);
        check_eq_i(l3.p[g].vel[1],
                   ((s64)Q2_TERMINAL_VY * 4096) / Q2_VEL_DIV,
                   "the shared mover caps grenade fall speed at raw +8192");

        q2_projectile_step(&l3, (u32)g, Q2_GRAVITY,
                           Q2_DT_NOMINAL, 1, &st);
        check_eq_i(l3.p[g].vel[1],
                   ((s64)Q2_TERMINAL_VY * 4096) / Q2_VEL_DIV,
                   "and the one-sided terminal clamp remains stable");
    }
}

/* ------------------------------------------------------------------------- */
/*
 * The machinegun as QPlayerFireMachineGun (0x8004C744) builds a shot, draw by
 * draw: three kick draws (pitch >>> 11, yaw and roll >>> 12), then three
 * spread draws, and a SetShockData request for pattern 5. The chaingun takes
 * two kick draws, both >>> 11, into yaw and roll, and does not write pitch.
 */
static s16 kick_of(s32 draw, int shift)
{
    return (s16)(u16)((u32)((draw & 0x7FFF) - 16384) >> shift);
}

static void test_kick_and_shock(void)
{
    static const struct { int id; s16 shock; u8 axes; } want[] = {
        { Q2_WID_BLASTER,          Q2_SHOCK_BLASTER,      Q2_KICK_PITCH },
        { Q2_WID_SHOTGUN,          Q2_SHOCK_SHOTGUN,      Q2_KICK_PITCH },
        { Q2_WID_SUPER_SHOTGUN,    Q2_SHOCK_SSHOTGUN,     Q2_KICK_PITCH },
        { Q2_WID_MACHINEGUN,       Q2_SHOCK_MACHINEGUN,   Q2_KICK_ALL },
        { Q2_WID_CHAINGUN,         Q2_SHOCK_CHAINGUN,
                                   Q2_KICK_YAW | Q2_KICK_ROLL },
        { Q2_WID_HAND_GRENADE,     Q2_SHOCK_GRENADE_OPEN, 0 },
        { Q2_WID_GRENADE_LAUNCHER, Q2_SHOCK_GRENADEL,     Q2_KICK_PITCH },
        { Q2_WID_ROCKET_LAUNCHER,  Q2_SHOCK_ROCKETL,      Q2_KICK_PITCH },
        { Q2_WID_HYPERBLASTER,     Q2_SHOCK_HYPERBLASTER, 0 },
        { Q2_WID_RAILGUN,          Q2_SHOCK_RAILGUN,      Q2_KICK_PITCH },
        { Q2_WID_BFG,              Q2_SHOCK_BFG,          0 },
    };
    q2_inventory inv;
    q2_rng rng, ref;
    q2_fire_result_v2 r;
    s32 eye[3] = { 0, 0, 0 };
    s16 aim[3] = { 0, 0, 4096 };
    s32 d;
    u32 i;

    printf("machinegun kick and shock\n");

    q2_rng_seed(&rng, 7);
    ref = rng;
    give_all(&inv);
    r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_MACHINEGUN, eye, 0, 0, 0, aim,
                       0, 0, false, false, 0);
    check(r.fired, "the machinegun fires");
    check_eq_i(inv.ammo[Q2_AMMO_BULLETS], 199, "one bullet");
    d = q2_rng_next(&ref);
    check_eq_i(r.kick[0], kick_of(d, 11), "pitch is the first draw, >>> 11");
    d = q2_rng_next(&ref);
    check_eq_i(r.kick[1], kick_of(d, 12), "yaw the second, >>> 12");
    d = q2_rng_next(&ref);
    check_eq_i(r.kick[2], kick_of(d, 12), "roll the third, >>> 12");
    d = q2_rng_next(&ref);
    check_eq_i(r.shot[0].dir[0], (d - 16384) >> 5,
               "then x's spread, +-512 on an aim times four");
    d = q2_rng_next(&ref);
    check_eq_i(r.shot[0].dir[1], (d - 16384) >> 5, "y's");
    d = q2_rng_next(&ref);
    check_eq_i(r.shot[0].dir[2], 4 * 4096 + ((d - 16384) >> 5), "and z's");
    check_eq_i(r.shot[0].damage, 8, "for 8");
    check_eq_i(r.mod, Q2_MOD_BULLET, "as a bullet");
    check_eq_i(r.shock, Q2_SHOCK_MACHINEGUN, "and pattern 5 on the pad");

    /* The truncated unsigned shift keeps the kick small and signed. */
    for (i = 0; i < 256; i++) {
        give_all(&inv);
        r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_MACHINEGUN, eye, 0, 0, 0,
                           aim, 0, 0, false, false, 0);
        check(r.kick[0] >= -8 && r.kick[0] <= 7, "pitch kick within -8..7");
        check(r.kick[1] >= -4 && r.kick[1] <= 3, "yaw kick within -4..3");
        check(r.kick[2] >= -4 && r.kick[2] <= 3, "roll kick within -4..3");
    }

    /* The chaingun: two draws, yaw and roll, pitch untouched. */
    q2_rng_seed(&rng, 7);
    ref = rng;
    give_all(&inv);
    r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_CHAINGUN, eye, 0, 0, 0, aim,
                       0, 0, false, false, 2);
    check_eq_i(r.kick_axes, Q2_KICK_YAW | Q2_KICK_ROLL,
               "the chaingun writes yaw and roll only");
    check_eq_i(r.kick[0], 0, "and leaves pitch alone");
    d = q2_rng_next(&ref);
    check_eq_i(r.kick[1], kick_of(d, 11), "yaw is its first draw, >>> 11");
    d = q2_rng_next(&ref);
    check_eq_i(r.kick[2], kick_of(d, 11), "roll its second, also >>> 11");
    check_eq_i(r.shock, Q2_SHOCK_CHAINGUN, "a spin that fires shakes the pad");

    give_all(&inv);
    r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_CHAINGUN, eye, 0, 0, 0, aim,
                       0, 0, false, false, 0);
    check_eq_i(r.shock, Q2_SHOCK_NONE,
               "one that fires nothing does not (0x8004CCD0)");

    /* Every weapon's id and kick fields, against its call site. */
    for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        const q2_weapon_behaviour *b = &q2_weapon_behaviour_table[want[i].id];

        check_eq_i(b->shock, want[i].shock, "the SetShockData id");
        check_eq_i(b->kick_axes, want[i].axes, "the kick fields written");
    }

    /* A dry trigger asks the pad for nothing. */
    give_all(&inv);
    inv.ammo[Q2_AMMO_BULLETS] = 0;
    r = q2_weapon_fire(&inv, &rng, NULL, Q2_WID_MACHINEGUN, eye, 0, 0, 0, aim,
                       0, 0, false, false, 0);
    check(r.dry, "an empty machinegun is dry");
    check_eq_i(r.shock, Q2_SHOCK_NONE, "and shakes nothing");
    check_eq_i(r.kick_axes, 0, "nor kicks");
}

/* ------------------------------------------------------------------------- */
static void test_native_toss(void)
{
    size_t i;
    for (i = 0; i < Q2PSX_ARRAY_COUNT(native_toss); i++) {
        q2_projectiles list;
        q2_projectile *p;
        q2_proj_step step;
        s32 intent[3], hit[3];
        s16 normal[3];
        int k;
        q2_projectiles_init(&list);
        p = &list.p[0]; p->in_use = true; list.live = 1;
        p->kind = Q2_PROJ_GRENADE;
        p->flags = (u16)native_toss[i].flags;
        for (k = 0; k < 3; k++) {
            p->pos[k] = native_toss[i].start[k];
            p->vel[k] = native_toss[i].velocity[k] * 4096 / Q2_VEL_DIV;
            p->angles[k] = (s16)native_toss[i].angles[k];
            normal[k] = (s16)native_toss[i].normal[k];
        }
        q2_random_seed(native_toss[i].seed);
        q2_projectile_step(&list, 0, native_toss[i].gravity, native_toss[i].dt, 0, &step);
        for (k = 0; k < 3; k++) {
            s32 delta = step.to[k] - step.from[k];
            intent[k] = step.to[k];
            if (!(p->flags & Q2_PROJ_TOSS_STOPPED) && native_toss[i].mode == 2)
                intent[k] = step.from[k] + delta * native_toss[i].fraction / 4096 - delta;
            hit[k] = intent[k];
            if (!(p->flags & Q2_PROJ_TOSS_STOPPED) && native_toss[i].mode == 1)
                hit[k] = step.from[k] + (intent[k] - step.from[k]) * native_toss[i].fraction / 4096;
            check_eq_i(intent[k], native_toss[i].intent[k], "native toss hull destination");
            check_eq_i(hit[k], native_toss[i].hit[k], "explicit collision service output");
        }
        if (native_toss[i].contact)
            q2_projectile_rebound(p, intent, hit, normal);
        else
            q2_projectile_commit(&list, 0, step.to);
        for (k = 0; k < 3; k++) {
            check_eq_i(p->pos[k], native_toss[i].position[k], "native toss position");
            check_eq_i(q2_projectile_raw_velocity(p, k), native_toss[i].result_velocity[k], "native toss velocity");
            check_eq_i(p->angles[k], native_toss[i].result_angles[k], "native toss tumble");
        }
        check_eq_i(p->flags, native_toss[i].result_flags, "native toss rest flags");
        check_eq_i(q2_random_state(), native_toss[i].result_seed, "native toss shared RNG");
    }
}

static void test_native_projectile_owners(void)
{
    size_t i;
    q2_actor owner;
    q2_projectile p;
    for (i = 0; i < Q2PSX_ARRAY_COUNT(native_projectile_owners); i++) {
        int k;
        bool ignore;
        q2_actor_init(&owner);
        memset(&p, 0, sizeof(p));
        p.kind = native_projectile_owners[i].hand ? Q2_PROJ_HAND_GRENADE : Q2_PROJ_GRENADE;
        p.flags = Q2_PROJ_TOSS_REST | (native_projectile_owners[i].cleared ? Q2_PROJ_OWNER_CLEAR : 0);
        for (k = 0; k < 3; k++) {
            owner.origin[k] = native_projectile_owners[i].centre[k];
            owner.mins[k] = (s16)native_projectile_owners[i].mins[k];
            owner.maxs[k] = (s16)native_projectile_owners[i].maxs[k];
            p.pos[k] = native_projectile_owners[i].point[k];
        }
        ignore = q2_projectile_ignore_owner(&p, native_projectile_owners[i].present ? &owner : NULL);
        check_eq_i(ignore, native_projectile_owners[i].result_ignored, "native grenade ignored owner");
        check_eq_i((p.flags & Q2_PROJ_OWNER_CLEAR) != 0,
                   native_projectile_owners[i].result_cleared, "native grenade owner-mask lifetime");
        check((p.flags & Q2_PROJ_TOSS_REST) != 0, "owner state preserves toss flags");
    }
    q2_actor_init(&owner);
    memset(&p, 0, sizeof(p)); p.kind = Q2_PROJ_GRENADE;
    p.pos[0] = 326; p.pos[1] = 1000000;
    check(q2_projectile_ignore_owner(&p, &owner), "expanded boundary is inclusive and ignores Y");
    p.pos[0]++;
    check(!q2_projectile_ignore_owner(&p, &owner), "leaving the box releases the owner");
    p.pos[0] = 0;
    check(!q2_projectile_ignore_owner(&p, &owner), "returning cannot re-arm owner exclusion");
    owner.origin[0] = 10000;
    check(!q2_projectile_ignore_owner(&p, &owner), "moving the owner cannot re-arm exclusion");
    p.flags = 0; p.kind = Q2_PROJ_HAND_GRENADE; p.node = Q2_PROJ_NODE_HELD;
    check(q2_projectile_ignore_owner(&p, &owner) && !(p.flags & Q2_PROJ_OWNER_CLEAR),
          "a held hand grenade has not entered the owner-release gate");
    p.kind = Q2_PROJ_ROCKET; p.node = Q2_PROJ_NODE_UNKNOWN;
    check(q2_projectile_ignore_owner(&p, &owner), "rockets retain their separate owner rule");
}

int main(void)
{
    printf("weapon behaviour\n\n");

    test_bios_random();
    test_tables();
    test_selection();
    test_firing();
    test_all_weapon_contracts();
    test_kick_and_shock();
    test_muzzle();
    test_projectiles();
    test_native_toss();
    test_native_projectile_owners();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
