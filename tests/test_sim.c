/*
 * test_sim.c — the simulation's timing and physics behaviour.
 *
 * The properties worth pinning are the ones that would change how the game
 * *feels* without ever crashing: the tick rate, the long-frame clamp, and the
 * fact that everything stays integer.
 */
#include <stdio.h>
#include <string.h>

#include "aiworld.h"
#include "levelbin.h"
#include "sim.h"
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

/* ------------------------------------------------------------------------- */
/* Item attachment consumes the map module's Population selections. */
static void item_put_u32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static void item_put_group(u8 *buf, u32 at, const char *name, u32 places)
{
    memset(buf + at, 0, Q2_POP_GROUP_SIZE);
    memcpy(buf + at, name, strlen(name));
    item_put_u32(buf + at + 0x10, places);
}

static void item_put_place(u8 *buf, u32 at, u16 id)
{
    memset(buf + at, 0, Q2_POP_PLACE_SIZE);
    buf[at + 14] = (u8)id;
    buf[at + 15] = (u8)(id >> 8);
    item_put_u32(buf + at + Q2_POP_PLACE_SIZE, Q2_POP_TERM_FFFF);
}

static void item_put_select(u8 *buf, u32 name_at, const char *name)
{
    memset(buf + name_at, 0, 12);
    memcpy(buf + name_at, name, strlen(name));
    item_put_u32(buf + 0, 0x24040000u | name_at); /* addiu a0,zero,name */
    item_put_u32(buf + 4,
                 0x8C620000u | Q2_LEVELBIN_SLOT_SELECT); /* lw v0,+36(v1) */
    item_put_u32(buf + 8, 0x00400009u);          /* jalr v0            */
    item_put_u32(buf + 12, 0);                   /* delay slot          */
}

static void test_item_group_selection(void)
{
    enum { ZONE_PLACES = 80,
           BATCH_PLACES = ZONE_PLACES + Q2_POP_PLACE_SIZE + 4,
           SECOND_PLACES = BATCH_PLACES + Q2_POP_PLACE_SIZE + 4,
           POP_SIZE = SECOND_PLACES + Q2_POP_PLACE_SIZE + 4 };
    u8 population[POP_SIZE];
    u8 levelbin[64];
    dat_chunk pop_chunk, levelbin_chunk;
    q2_common_file common;
    q2_sim sim;

    printf("item LevelBin selections\n");

    memset(population, 0, sizeof(population));
    item_put_group(population, 0,  "Zone0", ZONE_PLACES);
    item_put_group(population, 24, "Weapons", BATCH_PLACES);
    item_put_group(population, 48, "Ammo", SECOND_PLACES);
    item_put_u32(population + 72, 0);            /* group terminator */
    item_put_place(population, ZONE_PLACES, 27); /* Shells P */
    item_put_place(population, BATCH_PLACES, 27);
    item_put_place(population, SECOND_PLACES, 27);

    memset(levelbin, 0, sizeof(levelbin));
    item_put_select(levelbin, 48, "Weapons");

    memset(&pop_chunk, 0, sizeof(pop_chunk));
    pop_chunk.data = population;
    pop_chunk.size = sizeof(population);
    memset(&levelbin_chunk, 0, sizeof(levelbin_chunk));
    levelbin_chunk.data = levelbin;
    levelbin_chunk.size = sizeof(levelbin);

    memset(&common, 0, sizeof(common));
    common.chunk[Q2_COMMON_POPULATION] = &pop_chunk;
    common.chunk[Q2_COMMON_LEVEL_BIN]  = &levelbin_chunk;

    q2_sim_init(&sim, NULL, 50);
    check_eq_i(q2_sim_attach_items(&sim, &common, 0, NULL, NULL), Q2_OK,
               "item attachment accepts the synthetic map");
    check_eq_i(sim.entities.count, 2,
               "attachment spawns resident Zone0 and selected Weapons");
    check_eq_i(sim.item_group_order_count, 2,
               "startup records both groups that ran");
    check_eq_i(sim.item_group_order[0], 0,
               "resident Zone0 is first in startup order");
    check_eq_i(sim.item_group_order[1], 1,
               "selected Weapons follows in Population order");
    check_eq_i(q2_sim_activate_item_group(&sim, "Weapons"), 0,
               "CREBATCH cannot repeat a LevelBin-selected item group");
    check_eq_i(sim.entities.count, 2,
               "the selected group remains one copy after activation");

    /* The fallback must not restore the old over-population: with no module,
     * only the resident zone group is known to be live. */
    common.chunk[Q2_COMMON_LEVEL_BIN] = NULL;
    check_eq_i(q2_sim_attach_items(&sim, &common, 0, NULL, NULL), Q2_OK,
               "item attachment also accepts a missing LevelBin");
    check_eq_i(sim.entities.count, 1,
               "without LevelBin the unzoned batch is not guessed live");
    check_eq_i(q2_sim_activate_item_group(&sim, "Ammo"), 1,
               "a later group can activate before an earlier Population row");
    check_eq_i(sim.item_group_order[1], 2,
               "the runtime order records Ammo's actual first call");
    check_eq_i(q2_sim_activate_item_group(&sim, "Weapons"), 1,
               "CREBATCH spawns the deferred item's group");
    check_eq_i(sim.entities.count, 3,
               "both deferred items join the live entity set");
    check_eq_i(sim.item_group_order_count, 3,
               "each group appears in the first-run order once");
    check_eq_i(sim.item_group_order[2], 1,
               "Weapons remains after Ammo rather than sorting by index");
    check_eq_i(q2_sim_activate_item_group(&sim, "Weapons"), 0,
               "the retail group latch makes CREBATCH one-shot");
    check_eq_i(sim.entities.count, 3,
               "repeated CREBATCH cannot duplicate the deferred item");

    q2_sim_free(&sim);
}

/*
 * Did the tick just past put this sound on the queue? The queue is cleared at
 * the top of every world tick, so this only ever answers for the most recent
 * one — which is what the callers want.
 */
static bool sound_raised(const q2_sim *sim, q2_ent_sound which)
{
    const q2_ent_events *ev = q2_sim_entity_events(sim);
    u32 i;

    if (!ev)
        return false;
    for (i = 0; i < ev->count; i++)
        if (ev->e[i].kind == Q2_ENT_EVENT_SOUND && ev->e[i].sound == which)
            return true;
    return false;
}

typedef struct fx_probe {
    u32 count;
    s16 mod;
    s16 damage;
} fx_probe;

static void on_fx(void *user, const q2_event_item *item)
{
    fx_probe *probe = (fx_probe *)user;

    if (!probe || !q2_events_get_fx_damage(item, &probe->mod,
                                            &probe->damage))
        return;
    probe->count++;
}

/* ------------------------------------------------------------------------- */
/* Opcode 0x13 is not an effect-group: the retail dispatcher turns it directly
 * into T_Damage for the player who entered the record. Its eight-byte shape is
 * confirmed by all seven PAL items; this one is WASTE3's mod 0, damage 65. */
static void test_script_fx_damage(void)
{
    static const u8 raw[] = {
        12, 0, 1, 0,             /* one 12-byte record */
        Q2_EVOP_FX, 8,           /* opcode + fixed item length */
        0, 0, 65, 0, 0xF8, 0x77 /* mod, damage, unused tail */
    };
    q2_events events;
    q2_event_rt rt;
    q2_event_item item;
    fx_probe probe;
    q2_sim sim;
    s32 spawn[3] = { 0, 0, 0 };
    s16 health;

    printf("script FX damage\n");
    memset(&events, 0, sizeof(events));
    memset(&probe, 0, sizeof(probe));
    events.data         = raw;
    events.size         = sizeof(raw);
    events.record_count = 1;
    events.first_record = 0;

    check(q2_event_rt_init(&rt, &events) == Q2_OK,
          "the fixed-size FX record starts a runtime");
    rt.on_fx      = on_fx;
    rt.on_fx_user = &probe;
    check(q2_event_rt_trigger(&rt, 0), "the FX record queues");
    check(q2_event_rt_update(&rt) == Q2_EVENT_OK,
          "the FX record completes without changing zones");
    check_eq_i(rt.fx_count, 1, "the runtime records the decoded FX");
    check_eq_i(probe.count, 1, "the FX callback receives it");
    check_eq_i(probe.mod, Q2_MOD_NONE, "FX reads its signed mod at payload +0");
    check_eq_i(probe.damage, 65, "FX reads its signed damage at payload +2");
    q2_event_rt_free(&rt);

    item.op      = Q2_EVOP_FX;
    item.opcode  = Q2_EVOP_FX;
    item.len     = 8;
    item.offset  = 4;
    item.payload = raw + 6;

    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.combat.inv.armour = 0;
    health = sim.combat.inv.health;
    check(q2_sim_apply_event_fx(&sim, &item),
          "a retail-form FX item reaches the player damage path");
    check_eq_i(sim.combat.inv.health, health - 65,
               "WASTE3's FX takes its complete 65 health points");
    check_eq_i(sim.combat.self.last_attacker, -1,
               "script FX is a world hit, never credited to a player");

    item.len = 4;
    check(!q2_sim_apply_event_fx(&sim, &item),
          "a non-retail FX length is rejected before it can damage");
    check_eq_i(sim.combat.inv.health, health - 65,
               "the rejected FX leaves health unchanged");
}

/* ------------------------------------------------------------------------- */
/* 0x80027E64: record categories are enter/stay/leave, not three unknown bits
 * flattened into one edge-triggered volume rule. */
static void test_event_contact_categories(void)
{
    static const u8 raw[] = {
        4, 0, 0, Q2_EVREC_CAT_A,
        4, 0, 0, Q2_EVREC_CAT_B,
        4, 0, 0, Q2_EVREC_CAT_C
    };
    q2_events events;
    q2_event_rt rt;

    printf("event contact categories\n");

    memset(&events, 0, sizeof(events));
    events.data         = raw;
    events.size         = sizeof(raw);
    events.record_count = 3;
    events.first_record = 0;

    check(q2_event_rt_init(&rt, &events) == Q2_OK,
          "three category records start a runtime");

    q2_event_rt_contacts_begin(&rt);
    check(q2_event_rt_contact(&rt, 0), "enter record accepts contact");
    check(q2_event_rt_contact(&rt, 4), "stay record accepts contact");
    check(q2_event_rt_contact(&rt, 8), "leave record accepts contact");
    q2_event_rt_contacts_end(&rt);
    q2_event_rt_update(&rt);
    check_eq_i(rt.ran_count, 2,
               "first inside frame runs enter and stay, not leave");

    q2_event_rt_contacts_begin(&rt);
    /* Two volumes may name one record; it still runs only once this tick. */
    q2_event_rt_contact(&rt, 0);
    q2_event_rt_contact(&rt, 4);
    q2_event_rt_contact(&rt, 4);
    q2_event_rt_contact(&rt, 8);
    q2_event_rt_contacts_end(&rt);
    q2_event_rt_update(&rt);
    check_eq_i(rt.ran_count, 3,
               "second inside frame repeats stay only once");

    q2_event_rt_contacts_begin(&rt);
    q2_event_rt_contacts_end(&rt);
    q2_event_rt_update(&rt);
    check_eq_i(rt.ran_count, 4,
               "first outside frame runs the leave category");

    q2_event_rt_contacts_begin(&rt);
    q2_event_rt_contacts_end(&rt);
    q2_event_rt_update(&rt);
    check_eq_i(rt.ran_count, 4,
               "remaining outside does not repeat leave");

    q2_event_rt_free(&rt);
}

/* ------------------------------------------------------------------------- */
/* The original's underwater timer is a separate life-support system, not a
 * variant of the shallow-water movement flags. It uses the level dt, bypasses
 * armour with MOD 8, and a rebreather resets air rather than blocking damage. */
static void test_underwater_air(void)
{
    q2_sim sim;
    q2_input in;
    s32 spawn[3] = { 0, 0, 0 };
    s16 health;
    int i;

    printf("underwater air\n");
    memset(&in, 0, sizeof(in));
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = INT32_MAX;
    sim.env_flags = Q2_ENT_UNDERWATER;
    sim.combat.inv.armour = 200;
    health = sim.combat.inv.health;

    /* Twelve hundred dt units is where `air >> 10` first becomes one on the
     * 300-tick drowning pass. Armour must not absorb MOD 8. */
    for (i = 0; i < 130; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(sim.player[0].water_air >= 1200,
          "submersion accumulates the engine dt into air");
    check(sim.combat.inv.health < health,
          "the drowning clock eventually applies damage");
    check_eq_i(sim.combat.inv.armour, 200,
               "drowning bypasses armour through MOD 8");

    /* A new life starts a fresh breath and splash sequence.  This also makes
     * command-line placement in an authored water volume deterministic. */
    sim.player[0].wade        = 4;
    sim.player[0].water_air   = 4096;
    sim.player[0].water_next  = 9000;
    sim.player[0].splash_time = 8000;
    sim.player[0].water_voice = true;
    q2_sim_spawn(&sim, spawn, 0);
    check_eq_i(sim.player[0].wade, 0,
               "spawning clears the shallow-water edge latch");
    check_eq_i(sim.player[0].water_air, 0,
               "spawning clears the previous life's air counter");
    check_eq_i(sim.player[0].water_next, 0,
               "spawning clears the previous life's drowning deadline");
    check_eq_i(sim.player[0].splash_time, 0,
               "spawning clears the previous life's splash cooldown");
    check(!sim.player[0].water_voice,
          "spawning clears the alternating drowning-voice latch");

    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = INT32_MAX;
    sim.env_flags = Q2_ENT_UNDERWATER;
    sim.combat.inv.breather_until = 100000;
    health = sim.combat.inv.health;
    for (i = 0; i < 400; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].water_air, 1,
               "a live rebreather resets air to the retail sentinel");
    check_eq_i(sim.combat.inv.health, health,
               "a live rebreather prevents drowning damage");
}

/* ------------------------------------------------------------------------- */
static void test_tick_rate(void)
{
    q2_sim sim;
    q2_input in;
    u32 ticks;

    printf("tick rate\n");

    memset(&in, 0, sizeof(in));
    q2_sim_init(&sim, NULL, 50);

    /* The logic step is 12 dt units out of 300 per second, i.e. 25 Hz. One
     * second of real time must therefore produce 25 ticks -- not 50, not 60.
     * Getting this wrong changes every jump arc in the game. */
    ticks = 0;
    {
        int i;
        for (i = 0; i < 50; i++)              /* 50 fields == one PAL second */
            ticks += q2_sim_advance(&sim, &in, 1.0 / 50.0);
    }
    check_eq_i(ticks, 25, "one second of PAL fields runs 25 logic ticks");

    /* A long frame is clamped rather than sub-stepped. The original slowed the
     * world down instead of taking a huge step, and that is the behaviour. */
    q2_sim_init(&sim, NULL, 50);
    ticks = q2_sim_advance(&sim, &in, 10.0);   /* an absurd stall */
    check(ticks <= 3, "a 10-second stall is clamped, not integrated in full");
    printf("  a 10s stall produced %u ticks (clamped at %d dt)\n", ticks, Q2_DT_MAX);
}

/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */
/*
 * The TITLE SCREEN's five lights — module+0x2BD8, transcribed in levelbin.h.
 *
 * Worth pinning because none of it is checkable against a table: the rig is
 * code, the two big lights share one pseudo-random generator, and the order
 * they draw from it decides every value after the first frame. A reordering
 * would still produce five plausible lights and a visibly different screen.
 */
static void test_scene_lights(void)
{
    q2_lb_light l[Q2_LB_LIGHT_MAX];
    s32 wander[2] = { 0, 0 };
    q2_rng rng;
    u32 n, i, f;

    q2_rng_seed(&rng, 1);
    n = q2_levelbin_scene_lights(l, wander, &rng);
    check(n == Q2_LB_LIGHT_MAX, "the rig is five lights");

    /* The three small ones are constants: 0x80102BFC packs their radii and
     * 0x80102C9C their colour, and only y moves. */
    for (i = 0; i < 3; i++) {
        check(l[i].pos[0] == 0 && l[i].pos[2] == 500,
              "a small light stands on the axis at z 500");
        check(l[i].rgb[0] == 16 && l[i].rgb[1] == 255 && l[i].rgb[2] == 64,
              "a small light is green");
        check(l[i].inner == 100 && l[i].outer == 200,
              "a small light is 100/200");
    }
    check(l[0].pos[1] == -200 && l[1].pos[1] == 0 && l[2].pos[1] == 200,
          "the three small lights are a column 200 apart");

    /*
     * ...and being 200 across at 1200 from the logo, NONE of them reaches it:
     * `q2_light_set_add` rejects on any axis delta at or beyond the radius. The
     * rig places them anyway and the gather throws them out, which is the
     * behaviour, not a bug to be tidied.
     */
    check(l[0].outer < Q2_LB_SCENE_DIST - l[0].pos[2],
          "a small light cannot reach the logo");

    /* The two big ones are 500/1500 at y 600, z 900, mirrored in x. */
    for (i = 3; i < 5; i++) {
        check(l[i].pos[1] == 600 && l[i].pos[2] == 900,
              "a big light sits above and in front");
        check(l[i].rgb[0] == 64 && l[i].rgb[2] == 127,
              "a big light holds red and blue");
        check(l[i].rgb[1] >= 64 && l[i].rgb[1] <= 127,
              "a big light's green is (rand & 63) + 64");
        check(l[i].inner == 500 && l[i].outer == 1500,
              "a big light is 500/1500");
    }
    check(l[3].pos[0] >= 0 && l[4].pos[0] <= 0,
          "the big pair drifts apart, one each way");

    /*
     * The ease is a quarter of the way per frame toward a target in
     * ±(200..711), so x is bounded by the target range however long it runs —
     * and the left one's delta is negative every frame, which is the arm
     * `(d + 3) >> 2` exists for.
     */
    for (f = 0; f < 400; f++)
        q2_levelbin_scene_lights(l, wander, &rng);
    check(l[3].pos[0] > 0 && l[3].pos[0] <= 711,
          "the right light stays inside its target range");
    check(l[4].pos[0] < 0 && l[4].pos[0] >= -711,
          "the left light stays inside its target range");

    /* Same seed, same sequence — the generator is the console's, so a replay
     * of the title screen is reproducible. */
    {
        q2_lb_light a[Q2_LB_LIGHT_MAX], b[Q2_LB_LIGHT_MAX];
        s32 wa[2] = { 0, 0 }, wb[2] = { 0, 0 };
        q2_rng ra, rb;

        q2_rng_seed(&ra, 7);
        q2_rng_seed(&rb, 7);
        for (f = 0; f < 20; f++) {
            q2_levelbin_scene_lights(a, wa, &ra);
            q2_levelbin_scene_lights(b, wb, &rb);
        }
        check(memcmp(a, b, sizeof(a)) == 0 && wa[0] == wb[0] && wa[1] == wb[1],
              "one seed gives one title screen");
    }
}

static void test_gravity(void)
{
    q2_sim sim;
    q2_input in;
    s32 spawn[3] = { 0, 0, 0 };
    int i;
    s32 last_y;

    printf("gravity\n");

    memset(&in, 0, sizeof(in));
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);

    /* Put the ground far below so the fall is unobstructed. World Y increases
     * downward on this disc, so "far below" is a large positive Y. */
    sim.player[0].ground_y  = 100000;
    sim.player[0].on_ground = false;

    last_y = sim.player[0].pos[1];
    for (i = 0; i < 10; i++) {
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
        check(sim.player[0].pos[1] >= last_y, "falling moves the player downward");
        last_y = sim.player[0].pos[1];
    }
    check(sim.player[0].vel[1] > 0, "downward velocity accumulates");

    /*
     * Terminal velocity must actually bound the fall.
     *
     * The ground has to be effectively unreachable for this to mean anything.
     * An earlier version of this test used ground_y = 100000, the player landed
     * within a few ticks, velocity was zeroed on landing, and the assertion
     * "vel <= terminal" passed against a velocity of zero — green, and proving
     * nothing. Assert that terminal is REACHED as well as not exceeded, so the
     * test fails if the clamp is removed or if the player lands early.
     */
    sim.player[0].ground_y  = INT32_MAX;
    sim.player[0].on_ground = false;

    for (i = 0; i < 200; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);

    check(sim.player[0].vel[1] == Q2_TERMINAL_VY,
          "a long fall reaches exactly terminal velocity and stops there");
    printf("  velocity after 200 falling ticks: %d (terminal %d)\n",
           sim.player[0].vel[1], Q2_TERMINAL_VY);

    /* And it must not creep past it on further ticks. */
    for (i = 0; i < 50; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].vel[1], Q2_TERMINAL_VY, "terminal velocity is stable");
}

/* ------------------------------------------------------------------------- */
static void test_ground_and_view(void)
{
    q2_sim sim;
    q2_input in;
    s32 spawn[3] = { 0, 0, 0 };
    s32 eye[3];
    int i;

    printf("ground and view height\n");

    memset(&in, 0, sizeof(in));
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = 0;

    q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(sim.player[0].on_ground, "the player lands on the ground plane");
    check_eq_i(sim.player[0].pos[1], 0, "the player rests exactly on the ground");

    /*
     * The eye is above the feet, which with Y-down means a smaller Y.
     *
     * The console computes `entity_origin + 286 - viewOffset` (0x80038638) and
     * the origin is 286 above the feet, so the constants cancel and a standing
     * eye is `viewOffset` above the feet — 576, the player's own height. Pinned
     * at both offsets, because the standing case alone survives adding the 286
     * a second time (it just makes a shorter player) while the crouch case does
     * not: at 286 the eye would land ON the feet.
     */
    q2_sim_eye(&sim, eye);
    check(eye[1] < sim.player[0].pos[1], "the eye sits above the feet");
    check_eq_i(sim.player[0].pos[1] - eye[1], Q2_VIEW_STAND,
               "a standing eye is viewOffset above the feet");

    /*
     * Crouching is an ENVIRONMENT flag, not a button: INCROUCH and INLOWCROUCH
     * are event-script primitives (0x8002E5B4 / 0x8002F214) that a trigger volume
     * runs. So the test sets the flag the dispatcher would set, which is also the
     * only way the console can produce a crouch.
     */
    sim.env_flags |= Q2_ENT_INLOWCROUCH;

    /*
     * The offset is chosen from the flags as they stand at the TOP of the tick,
     * before they are cleared and re-asserted, so the first tick after a volume
     * is entered still eases toward the old target. That one-tick lag is the
     * console's and the second tick is where the movement starts.
     */
    q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(sim.player[0].view_height < Q2_VIEW_STAND, "a crouch volume lowers the view");
    check(sim.player[0].view_height > Q2_VIEW_CROUCH,
          "the view eases rather than snapping");
    check(sim.player[0].crouching, "and the player reports crouching");

    for (i = 0; i < 60; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].view_height, Q2_VIEW_CROUCH,
               "the view settles at low-crouch height");

    /* The mid crouch is a different flag and a different height. */
    sim.env_flags = Q2_ENT_INCROUCH;
    for (i = 0; i < 60; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].view_height, Q2_VIEW_MID,
               "INCROUCH settles at the mid height, not the low one");

    sim.env_flags = 0;
    for (i = 0; i < 80; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].view_height, Q2_VIEW_STAND, "and returns to standing");
    check(!sim.player[0].crouching, "and stops reporting a crouch");
}

/* ------------------------------------------------------------------------- */
/*
 * The wish velocity, which is where the whole of the engine's acceleration and
 * deceleration lives. There is no separate friction term to test because there
 * is none in the executable: one clamped approach does both jobs.
 */
static void test_wish_velocity(void)
{
    q2_sim sim;
    q2_input in;
    s32 spawn[3] = { 0, 0, 0 };
    int i;

    printf("wish velocity\n");

    memset(&in, 0, sizeof(in));
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = 0;

    /* Full deflection must reach exactly the speed the executable's table names
     * — the >>7 is calibrated so Q2_INPUT_FULL lands on it and not near it. */
    in.forward = Q2_INPUT_FULL;
    for (i = 0; i < 200; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].wish[2], Q2_SPEED_NORMAL,
               "full forward deflection reaches exactly the normal max speed");

    /* Half deflection, half the speed. A shift, not a curve. */
    in.forward = Q2_INPUT_FULL / 2;
    for (i = 0; i < 200; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].wish[2], Q2_SPEED_NORMAL / 2,
               "half deflection reaches half speed");

    /* Releasing decelerates at the same rate it accelerated, and reaches a hard
     * zero rather than an asymptote — that is the difference between a clamped
     * approach and the exponential decay a naive friction term gives. */
    in.forward = 0;
    for (i = 0; i < 200; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].wish[2], 0, "releasing brings the wish to exactly zero");
    check_eq_i(sim.player[0].vel[2], 0, "and the world velocity with it");

    /* The crouch flags cap the speed. */
    sim.env_flags = Q2_ENT_INLOWCROUCH;
    in.forward = Q2_INPUT_FULL;
    for (i = 0; i < 300; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].wish[2], Q2_SPEED_LOWCROUCH,
               "a low crouch caps the wish at the crouch speed");

    /* Shallow water is the middle case, and it groups with UNDERWATER and
     * INCROUCH as the single mask 0x304. */
    sim.env_flags = Q2_ENT_INWATER;
    for (i = 0; i < 300; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].wish[2], Q2_SPEED_WET,
               "shallow water caps the wish at the wet speed");
}

/* ------------------------------------------------------------------------- */
/*
 * Looking. The stick asks for a turn RATE, the rate is eased, and the angle
 * integrates from it — so the two things worth pinning are that the view keeps
 * moving for a moment after release, and that it does eventually stop.
 */
static void test_look(void)
{
    q2_sim sim;
    q2_input in;
    s32 spawn[3] = { 0, 0, 0 };
    s32 yaw_at_release, yaw_after;
    int i;

    printf("look\n");

    memset(&in, 0, sizeof(in));
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = 0;

    in.yaw = Q2_INPUT_FULL;
    for (i = 0; i < 40; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(sim.player[0].yaw > 0, "holding right turns right");
    check_eq_i(sim.player[0].yaw_rate,
               (Q2_INPUT_FULL * Q2_LOOK_SCALE_NUM) >> Q2_LOOK_SCALE_SHIFT,
               "the rate settles at three quarters of the stick");

    /* Glide: the rate decays over several ticks, so the view overshoots the
     * moment of release. A port that adds the stick straight to the angle has
     * no glide at all, which is the single most obvious way to feel wrong. */
    yaw_at_release = sim.player[0].yaw;
    in.yaw = 0;
    q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(sim.player[0].yaw > yaw_at_release, "the view glides on after release");

    for (i = 0; i < 40; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].yaw_rate, 0, "and the rate reaches exactly zero");
    yaw_after = sim.player[0].yaw;
    for (i = 0; i < 20; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].yaw, yaw_after, "after which the view is still");

    /* Pitch is clamped to a quarter circle, and hitting the clamp kills the
     * rate so the view stops dead rather than pressing against the limit. */
    in.pitch = Q2_INPUT_FULL;
    for (i = 0; i < 400; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].pitch, Q2_PITCH_LIMIT, "pitch clamps at +90 degrees");
    check_eq_i(sim.player[0].pitch_rate, 0, "and the clamp zeroes the pitch rate");

    in.pitch = -Q2_INPUT_FULL;
    for (i = 0; i < 400; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].pitch, -Q2_PITCH_LIMIT, "and at -90 the other way");

    /*
     * A mouse or stick style sets the rate outright instead of easing it, and
     * the sense of that test is the thing to pin down: 0x8003A670 branches to
     * the EASED arm when `style < 6` FAILS, so the six analogue styles are the
     * ones that snap and the three STANDARD ones glide. Everything above ran on
     * STANDARD A, which is what q2_sim_init leaves configured.
     */
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].look_scheme = Q2_LOOK_SCHEME_ANALOGUE - 1;   /* BOTH STICKS */
    in.pitch = 0;
    in.yaw   = Q2_INPUT_FULL;
    q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].yaw_rate,
               (Q2_INPUT_FULL * Q2_LOOK_SCALE_NUM) >> Q2_LOOK_SCALE_SHIFT,
               "an analogue style reaches the full rate in one tick");

    /* And the digital styles do not, which is the same claim from the other
     * side and the one the port used to have backwards. */
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].look_scheme = Q2_LOOK_SCHEME_ANALOGUE;       /* STANDARD A  */
    q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(sim.player[0].yaw_rate <
              ((Q2_INPUT_FULL * Q2_LOOK_SCALE_NUM) >> Q2_LOOK_SCALE_SHIFT),
          "a digital style is still ramping after one tick");
}

/* ------------------------------------------------------------------------- */
/*
 * The jump. It posts an impulse rather than writing velocity, and the impulse
 * ceiling is the same figure the jump posts — so the height is pinned twice.
 */
static void test_jump(void)
{
    q2_sim sim;
    q2_input in;
    s32 spawn[3] = { 0, 0, 0 };
    s32 peak;
    int i;

    printf("jump\n");

    memset(&in, 0, sizeof(in));
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = 0;

    /* Land first, so there is a ground normal to jump from. */
    for (i = 0; i < 4; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(sim.player[0].on_ground, "the player is on the ground before jumping");

    in.buttons = Q2_BTN_JUMP;
    q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);

    /*
     * -Y is up. The jump posts -3072 but the integrator adds one tick of gravity
     * BEFORE applying the impulse (0x80046000 stores the gravity sum, 0x80046120
     * adds the accumulator), so the velocity a jump actually starts with is
     * `impulse + gravity*dt` — and the -3072 ceiling is therefore NOT reached by
     * a plain jump. It exists to bound stacked impulses, which is what makes the
     * rocket jump finite.
     */
    check_eq_i(sim.player[0].vel[1], Q2_JUMP_IMPULSE + Q2_GRAVITY * Q2_DT_NOMINAL,
               "a jump starts at the impulse plus one tick of gravity");
    check(sim.player[0].vel[1] > Q2_IMPULSE_CEILING,
          "so a plain jump stays inside the impulse ceiling");
    check(!sim.player[0].on_ground, "and the ground flag is dropped");
    check_eq_i(sim.player[0].jump_hold, Q2_JUMP_HOLD, "the hold timer is armed");

    /*
     * AND IT MAKES A NOISE. 0x8003E210 loads the handle at 0x800B2900 —
     * registered from "pla_jump1" at 0x800AC488 — and 0x8003E214 plays it at
     * the entity, on the success path only. The port raised no event at all,
     * which with the soft landing also missing (see test_fall_damage) meant an
     * ordinary hop was silent at both ends of it.
     */
    check(sound_raised(&sim, Q2_SND_JUMP), "and the jump raises pla_jump1");

    /* Holding the button must not produce a second jump while rising. */
    q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(sim.player[0].jump_hold != Q2_JUMP_HOLD,
          "holding the button does not re-arm the jump");

    /*
     * The arc must come back down. The button is released first, because the hold
     * is cancelled the moment velocity turns downward — so a HELD button jumps
     * again on the way down and the player never settles. That is real behaviour
     * (it is how you bunny-hop on the console), which is exactly why it has to be
     * taken out of the way to measure a single arc.
     */
    in.buttons = 0;
    peak = sim.player[0].pos[1];
    for (i = 0; i < 80; i++) {
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
        if (sim.player[0].pos[1] < peak)
            peak = sim.player[0].pos[1];
    }
    check(peak < 0, "the jump actually leaves the floor");
    check(sim.player[0].on_ground, "and the player comes back down");
    printf("  peak height %d world units (%d PC units)\n", -peak,
           -peak / Q2_WORLD_SCALE);

    /* The hold is cancelled by the descent, not by running its 576 ticks out. */
    check_eq_i(sim.player[0].jump_hold, 0,
               "the hold timer is cancelled once the fall starts");

    /* Holding the button off the ground bunny-hops rather than doing nothing. */
    in.buttons = Q2_BTN_JUMP;
    {
        int airborne = 0;
        for (i = 0; i < 120; i++) {
            q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
            if (!sim.player[0].on_ground)
                airborne++;
        }
        check(airborne > 60, "a held jump button keeps the player hopping");
    }

    /* Crouching blocks the jump outright (flags & 0x700 at 0x8003A904). */
    in.buttons = 0;
    for (i = 0; i < 60; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(sim.player[0].on_ground, "the player settles once the button is released");

    sim.env_flags = Q2_ENT_INLOWCROUCH;
    in.buttons    = Q2_BTN_JUMP;
    {
        s32 y = sim.player[0].pos[1];
        for (i = 0; i < 20; i++)
            q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
        check_eq_i(sim.player[0].pos[1], y, "a crouching player cannot jump");
    }
}

/* ------------------------------------------------------------------------- */
/*
 * Standing still. The mover never clears velocity on contact; the ground
 * projection on the following tick does. If that rule is missing, vertical
 * velocity runs away to terminal while the player stands motionless, and every
 * later landing measures the wrong delta.
 */
static void test_ground_projection(void)
{
    q2_sim sim;
    q2_input in;
    s32 spawn[3] = { 0, 0, 0 };
    int i;

    printf("ground projection\n");

    memset(&in, 0, sizeof(in));
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = 0;

    for (i = 0; i < 100; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);

    check(sim.player[0].on_ground, "the player is still standing after 100 ticks");
    check_eq_i(sim.player[0].pos[1], 0, "and has not sunk");

    /*
     * The steady state is ONE tick of gravity, not zero: the projection snaps the
     * velocity to the surface at the top of the tick and the integrator adds
     * gravity back at the bottom. Without the projection this would have climbed
     * to Q2_TERMINAL_VY within a couple of seconds, so the figure to assert is
     * the fixed point rather than a bound.
     */
    check_eq_i(sim.player[0].vel[1], Q2_GRAVITY * Q2_DT_NOMINAL,
               "standing settles at exactly one tick of gravity, not terminal");
}

/* ------------------------------------------------------------------------- */
/*
 * The liquid rules. The 0x08 arm in particular: the executable pushes UP while
 * the fall is slower than 1024 and caps it at 1024 when faster, which is the
 * opposite of what both FORMATS.md and this port used to say.
 */
/*
 * A single volume covering everywhere, with the contents bits under test.
 *
 * Driven through the real path — `q2_move_contents` and the flag derivation at
 * 0x800458A0 — rather than by poking the entity's flags, because the tick clears
 * and re-derives them and a poked flag would simply be wiped. This is the port's
 * stand-in for the engine's own volume table at 0x800C9114.
 */
static void liquid_volume(q2_sim *sim, q2_move_target *vol, u16 contents)
{
    memset(vol, 0, sizeof(*vol));
    vol->min[0] = vol->min[1] = vol->min[2] = -1000000;
    vol->max[0] = vol->max[1] = vol->max[2] =  1000000;
    vol->mask   = contents;
    vol->kind   = Q2_MOVE_KIND_VOLUME;
    vol->active = true;

    sim->volumes      = NULL;             /* not owned; q2_sim_free must not free it */
    sim->volume_count = 1;
    memset(&sim->move_world, 0, sizeof(sim->move_world));
    sim->move_world.half_extent = Q2_SWEEP_HALF_EXTENT;
    sim->move_world.targets     = vol;
    sim->move_world.count       = 1;
    sim->move_world.mask        = 0;
}

static void test_liquid(void)
{
    q2_sim sim;
    q2_input in;
    q2_move_target vol;
    s32 spawn[3] = { 0, 0, 0 };
    int i;

    printf("liquid\n");

    memset(&in, 0, sizeof(in));

    /* Buoyant (contents 0x2000): eased toward -3072, i.e. upward. */
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = INT32_MAX;
    liquid_volume(&sim, &vol, 0x2000);

    for (i = 0; i < 100; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);

    check(sim.player[0].ent.flags & Q2_ENT_LIQUID_FLOAT,
          "contents 0x2000 sets the float flag");
    check_eq_i(sim.player[0].vel[1], Q2_LIQUID_FLOAT_VY,
               "a buoyant volume settles at the float velocity");
    check(sim.player[0].pos[1] < 0, "and carries the player upward");

    /*
     * The slow-sink arm (contents 0x0200). +1024 is a terminal velocity reached
     * from BOTH directions: from rest the constant dt*24 push loses to gravity's
     * dt*32 and the fall creeps up to it, and from a fast fall the ease brings it
     * back down to it. Asserting both directions is what catches the branches
     * being inverted, which is how FORMATS.md described them.
     */
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = INT32_MAX;
    liquid_volume(&sim, &vol, 0x0200);

    for (i = 0; i < 4; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);

    check(sim.player[0].ent.flags & Q2_ENT_LIQUID_SINK,
          "contents 0x0200 sets the sink flag");
    check_eq_i(sim.player[0].vel[1],
               4 * (Q2_GRAVITY - Q2_LIQUID_SINK_PUSH) * Q2_DT_NOMINAL,
               "from rest it gains gravity minus the push each tick");

    for (i = 0; i < 400; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].vel[1], Q2_LIQUID_SINK_VY,
               "and converges on the sink velocity from below");

    /* From above, the ease brings a fast fall back down to the same figure. */
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = INT32_MAX;
    liquid_volume(&sim, &vol, 0x0200);
    sim.player[0].vel[1] = 6000;

    for (i = 0; i < 200; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].vel[1], Q2_LIQUID_SINK_VY,
               "and on the same figure from above");
    check(Q2_LIQUID_SINK_VY * 8 == Q2_TERMINAL_VY,
          "a 0x0200 volume falls eight times slower than open air");

    /*
     * Swimming. UNDERWATER takes the integrator's no-gravity arm (0x80045EF4's
     * 0x180 test), so a swimmer does not accelerate downward — but the ground
     * stick at 0x8003AD0C still applies, because 0x8003AD1C tests UNDERWATER on
     * its own and reaches the ease regardless of whether the stick is being
     * pushed. So a swimmer sinks at a CONSTANT +768 rather than falling.
     *
     * That is the whole swimming model: constant slow sink, and a button that
     * counteracts it. The two constants are even symmetric, +-768.
     */
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = INT32_MAX;
    sim.env_flags       = Q2_ENT_UNDERWATER;

    for (i = 0; i < 100; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].vel[1], Q2_GROUND_STICK_VY,
               "a swimmer sinks at a constant rate, not an accelerating one");
    check(sim.player[0].vel[1] < Q2_TERMINAL_VY / 8,
          "which is nowhere near a fall — gravity really is off");

    /*
     * The swim-up button. Its own ease reaches -768 and the ground stick then
     * takes one step of dt*6 back the other way within the same tick, so the
     * velocity observed at a tick boundary is the sum of the two. Asserting the
     * emergent figure rather than -768 is the point: it only comes out right if
     * both rules run, in this order (0x8003AB98 before 0x8003AD58).
     */
    in.buttons = Q2_BTN_SWIM_UP;
    for (i = 0; i < 40; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].vel[1],
               Q2_SWIM_UP_VY + Q2_GROUND_STICK_RATE * Q2_DT_NOMINAL,
               "swimming up settles at the swim-up velocity less one stick step");

    {
        s32 y = sim.player[0].pos[1];
        for (i = 0; i < 40; i++)
            q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
        check(sim.player[0].pos[1] < y, "and carries the swimmer upward");
    }
}

/* ------------------------------------------------------------------------- */
/*
 * Fall damage. Driven by how much LANDING changed vertical velocity, which is
 * produced by the ground projection a tick after the impact — so this also
 * pins the ordering the two rules depend on.
 */
static void test_fall_damage(void)
{
    q2_sim sim;
    q2_input in;
    s32 spawn[3] = { 0, 0, 0 };
    s32 health_before;
    int i;

    printf("fall damage\n");

    memset(&in, 0, sizeof(in));

    /* A short drop must not hurt. */
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = 200;
    health_before = sim.combat.self.health;

    for (i = 0; i < 40; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.combat.self.health, health_before,
               "a short drop does no damage");

    /*
     * ...but it is NOT silent, which is the half of 0x80039D50 the port had
     * backwards. `slti v0, s0, 31` / `bne v0, zero, 0x80039DBC` takes the
     * BELOW-31 band to a play of 0x800B2904 ("pla_land1"), so the quiet band is
     * the one with a sound of its own and 31 chooses between two sounds rather
     * than gating one. A drop this short is exactly the landing at the end of an
     * ordinary jump.
     */
    {
        int    tick;
        bool   heard = false;

        q2_sim_init(&sim, NULL, 50);
        q2_sim_spawn(&sim, spawn, 0);
        sim.player[0].ground_y = 900;

        for (tick = 0; tick < 60 && !heard; tick++) {
            q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
            heard = sound_raised(&sim, Q2_SND_LAND_SOFT);
        }
        check(heard, "and a landing under the damage threshold raises pla_land1");
        check_eq_i(sim.combat.self.health, 100,
                   "without costing any health");
    }

    /* A long one must, and must leave a view kick behind. */
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = 60000;
    health_before = sim.combat.self.health;

    for (i = 0; i < 300; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(sim.player[0].on_ground, "the long drop lands");
    check(sim.combat.self.health < health_before, "and a long drop hurts");
    check(sim.player[0].fall_value > 0, "and leaves a view kick");
    check(sim.player[0].fall_value <= Q2_FALL_KICK_MAX,
          "which is capped at forty degrees");
    printf("  fell to terminal velocity: %d damage, kick %d/4096\n",
           health_before - sim.combat.self.health, sim.player[0].fall_value);

    /* Water suppresses it entirely — mask 0x104 at 0x80039CF4. */
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = 60000;
    sim.env_flags       = Q2_ENT_INWATER;
    health_before = sim.combat.self.health;

    for (i = 0; i < 300; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.combat.self.health, health_before,
               "landing in water does no fall damage");

    /* And so does the cheat flag. */
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y   = 60000;
    sim.no_fall_damage    = true;
    health_before = sim.combat.self.health;

    for (i = 0; i < 300; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.combat.self.health, health_before,
               "the no-fall-damage flag suppresses it");
}

/* ------------------------------------------------------------------------- */
static void test_movement(void)
{
    q2_sim sim;
    q2_input in;
    s32 spawn[3] = { 0, 0, 0 };
    int i;

    printf("movement\n");

    memset(&in, 0, sizeof(in));
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = 0;

    /* Facing yaw 0, forward is +Z. */
    in.forward = Q2_INPUT_FULL;
    for (i = 0; i < 20; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);

    /*
     * Top speed has to be REACHED, not merely approached. The wish eases at
     * 38*dt and world velocity at 40*dt, so both saturate within about a dozen
     * ticks; anything much slower means one of the two rates is wrong, and
     * anything that never arrives means the clamped approach is not snapping.
     */
    check_eq_i(sim.player[0].vel[2], Q2_SPEED_NORMAL,
               "twenty ticks of forward reaches exactly the top speed");
    check_eq_i(sim.player[0].frame_delta[2],
               Q2_SPEED_NORMAL * Q2_DT_NOMINAL / Q2_VEL_DIV,
               "and the frame delta is that speed over the tick");

    check(sim.player[0].pos[2] > 0, "holding forward at yaw 0 moves along +Z");
    check_eq_i(sim.player[0].pos[0], 0, "and does not drift sideways");
    printf("  20 ticks of forward moved %d world units (%d PC units)\n",
           sim.player[0].pos[2], sim.player[0].pos[2] / Q2_WORLD_SCALE);

    /*
     * Top speed in real units, as a sanity check against the PC game rather than
     * against the port's own arithmetic. Q2_SPEED_NORMAL is a velocity in
     * Q2_VEL_DIV-scaled units per dt, so the ground speed is
     * speed / Q2_VEL_DIV * Q2_DT_HZ world units a second.
     */
    printf("  top speed %d world units/s == %d PC units/s (PC Quake II: 300)\n",
           Q2_SPEED_NORMAL * Q2_DT_HZ / Q2_VEL_DIV,
           Q2_SPEED_NORMAL * Q2_DT_HZ / Q2_VEL_DIV / Q2_WORLD_SCALE);

    /* Releasing input must bring the player to rest, not coast forever. */
    in.forward = 0;
    for (i = 0; i < 200; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].vel[2], 0, "releasing brings the player to rest");

    /* Strafe must run along the perpendicular, and the roll must follow it. */
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = 0;
    in.side = Q2_INPUT_FULL;
    for (i = 0; i < 40; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(sim.player[0].pos[0] > 0, "strafing right at yaw 0 moves along +X");
    check(sim.player[0].roll < 0, "and rolls the view the other way");
    check_eq_i(sim.player[0].roll, -(s32)sim.player[0].wish[0] / Q2_ROLL_DIV,
               "by exactly the side wish over 192");
    in.side = 0;

    /* Turning right by a quarter circle must make forward run along +X. */
    q2_sim_spawn(&sim, spawn, Q2_ANGLE_90);
    sim.player[0].ground_y = 0;
    in.forward = Q2_INPUT_FULL;
    for (i = 0; i < 20; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(sim.player[0].pos[0] > 0, "at yaw 90 forward moves along +X");
}

/* ------------------------------------------------------------------------- */
/*
 * Movement against a real hull.
 *
 * Everything above runs with no collision, which is enough for the integrator
 * and the rate rules but leaves the two things that only exist on contact
 * untested: that a wall actually stops the player, and that the velocity clip at
 * 0x80039AE4 fires. One synthetic box, built the way test_coll builds one, is
 * enough for both.
 *
 * The box is the hull the mover works in, so its faces bound the player's
 * ORIGIN, not their feet — SecondaryCol is already eroded by the 286 half-extent
 * (§3.4). A room `sy` tall therefore lets the origin travel `sy`.
 */
#define HULL_SX 4000
#define HULL_SY  600
#define HULL_SZ 4000

static u8 g_hull[4 + 2 * Q2_COLL_NODE_SIZE + 6 * Q2_COLL_PLANE_SIZE];

static void hwr16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void hwr32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static bool open_box_hull(q2_collision *out)
{
    /* Outward normals, in the plane order -X +X -Y +Y -Z +Z. +Y is down, so the
     * +Y face is the FLOOR and the -Y face is the ceiling. */
    static const s16 n[6][3] = {
        {-4096, 0, 0}, {4096, 0, 0},
        {0, -4096, 0}, {0, 4096, 0},
        {0, 0, -4096}, {0, 0, 4096}
    };
    s32 pt[6][3];
    u8 *nodes, *planes;
    dat_chunk chunk;
    q2_zone_file zf;
    int i, k;

    memset(g_hull, 0, sizeof(g_hull));
    memset(pt, 0, sizeof(pt));
    pt[1][0] = HULL_SX;
    pt[3][1] = HULL_SY;
    pt[5][2] = HULL_SZ;

    hwr16(g_hull + 0, 1);       /* one node  */
    hwr16(g_hull + 2, 6);       /* six planes */

    nodes  = g_hull + 4;
    planes = nodes + 2 * Q2_COLL_NODE_SIZE;

    hwr32(nodes + 0,  0);  hwr32(nodes + 4,  0);  hwr32(nodes + 8,  0);
    hwr32(nodes + 12, HULL_SX); hwr32(nodes + 16, HULL_SY);
    hwr32(nodes + 20, HULL_SZ);
    hwr16(nodes + 24, 0);       /* first plane */
    hwr16(nodes + 26, 0);       /* first link  */
    hwr32(nodes + 28, 0);

    /* The sentinel node terminates both the plane and the link run. */
    hwr16(nodes + Q2_COLL_NODE_SIZE + 24, 6);
    hwr16(nodes + Q2_COLL_NODE_SIZE + 26, 0);

    for (i = 0; i < 6; i++) {
        for (k = 0; k < 3; k++)
            hwr16(planes + i * 12 + k * 2, (u32)pt[i][k]);
        for (k = 0; k < 3; k++)
            hwr16(planes + i * 12 + 6 + k * 2, (u32)(u16)n[i][k]);
    }

    memset(&chunk, 0, sizeof(chunk));
    chunk.data = g_hull;
    chunk.size = (u32)sizeof(g_hull);

    memset(&zf, 0, sizeof(zf));
    zf.chunk[Q2_ZONE_SECONDARY_COL] = &chunk;

    return q2_collision_parse(out, &zf, Q2_COLL_SECONDARY) == Q2_OK;
}

/* ------------------------------------------------------------------------- */
/* PISTON's +18 word gates its pusher; all four slots remain visual parts. */
static void test_piston_decode(void)
{
    static const u8 funcs[16] = {
        1, 0, 0, 0, 'P', 'I', 'S', 'T', 'O', 'N', 0, 0, 0, 0, 0, 0
    };
    u8 raw[32];
    q2_userfuncs uf;
    q2_events events;
    q2_mover_set set;

    printf("piston decode\n");

    memset(raw, 0, sizeof(raw));
    /* Events header: no directory, then one 24-byte record at +8. */
    hwr16(raw + 8, 24);
    raw[10] = 1;
    raw[12] = Q2_EVOP_CALL;
    raw[13] = 20;
    /* raw+14 is UserFuncs index zero and raw+15 the CALL pad. */
    raw[16] = 2;              /* +4: axis, deliberately not the old Y default */
    raw[17] = 10;             /* +5: speed */
    hwr16(raw + 18, (u16)-314); /* +6: signed target */
    hwr16(raw + 20, 115);     /* +8 */
    hwr16(raw + 22, 129);     /* +10 */
    hwr16(raw + 24, (u16)-1); /* +12 */
    hwr16(raw + 26, (u16)-1); /* +14 */
    hwr16(raw + 28, 1);       /* +16: time */
    hwr16(raw + 30, 0);       /* +18: no pusher */

    memset(&uf, 0, sizeof(uf));
    uf.data  = funcs;
    uf.size  = (u32)sizeof(funcs);
    uf.count = 1;
    memset(&events, 0, sizeof(events));
    events.data         = raw;
    events.size         = (u32)sizeof(raw);
    events.record_count = 1;
    events.first_record = 8;
    memset(&set, 0, sizeof(set));

    check(q2_movers_build_calls(&set, &events, &uf, NULL, NULL) == Q2_OK,
          "a PISTON call decodes");
    check_eq_i(set.count, 1, "and produces one visual mover");
    if (set.count) {
        check_eq_i(set.movers[0].axis, 2, "PISTON keeps its authored axis bits");
        check_eq_i(set.movers[0].target, -314, "and keeps its signed target");
        check_eq_i(set.movers[0].part_count, 2, "and reads all populated object slots");
        check_eq_i(set.movers[0].node[0], 115, "its first visual node survives");
        check_eq_i(set.movers[0].node[1], 129, "its second visual node survives");
        check(!set.movers[0].blocks_player,
              "a zero +18 leaves the PISTON without a collision pusher");
    }
    q2_movers_free(&set);

    hwr16(raw + 30, 1);
    memset(&set, 0, sizeof(set));
    check(q2_movers_build_calls(&set, &events, &uf, NULL, NULL) == Q2_OK,
          "a pusher-enabled PISTON call decodes");
    check(set.count && set.movers[0].blocks_player,
          "a non-zero +18 enables the collision pusher");
    q2_movers_free(&set);
}

/* BASE2 Events+888: the cage at the level start drops once and stays there. */
static void test_cagelift_timers(void)
{
    static const u8 funcs[16] = {
        1, 0, 0, 0, 'C', 'A', 'G', 'E', 'L', 'I', 'F', 'T', '1', 0, 0, 0
    };
    u8 raw[32];
    q2_userfuncs uf;
    q2_events events;
    q2_mover_set set;
    u32 tick;

    printf("BASE2 cage-lift timers\n");

    memset(raw, 0, sizeof(raw));
    hwr16(raw + 8, 24);              /* one 24-byte record */
    raw[10] = 1;
    raw[12] = Q2_EVOP_CALL;
    raw[13] = 20;
    hwr16(raw + 16, (u16)-1491);     /* item +4: BASE2's travel */
    hwr16(raw + 18, (u16)-3);        /* item +6: speed */
    hwr16(raw + 20, 23);             /* item +8/+10: cage slabs */
    hwr16(raw + 22, 24);
    hwr16(raw + 24, (u16)-1);
    hwr16(raw + 26, (u16)-1);
    raw[28] = 32;                    /* item +16: bottom thickness */
    raw[29] = 32;                    /* item +17: top thickness */
    raw[30] = 0;                     /* item +18: no delay */
    raw[31] = 0xFF;                  /* item +19: never return */

    memset(&uf, 0, sizeof(uf));
    uf.data = funcs;
    uf.size = (u32)sizeof(funcs);
    uf.count = 1;
    memset(&events, 0, sizeof(events));
    events.data = raw;
    events.size = (u32)sizeof(raw);
    events.record_count = 1;
    events.first_record = 8;
    memset(&set, 0, sizeof(set));

    check(q2_movers_build_calls(&set, &events, &uf, NULL, NULL) == Q2_OK,
          "BASE2's CAGELIFT1 record decodes");
    check_eq_i(set.count, 1, "and builds one cage mover");
    if (set.count) {
        q2_mover *m = &set.movers[0];

        check_eq_i(m->delay_reset, 0, "+18 is the zero pre-move delay");
        check_eq_i(m->wait_reset, Q2_MOVER_WAIT_NEVER,
                   "+19 is the 0xFF never-return wait");
        check_eq_i(m->cage_bottom, 32, "+16 remains the bottom slab");
        check_eq_i(m->cage_top, 32, "+17 remains the top slab");

        q2_mover_trigger(&set, 0);
        for (tick = 0; tick < 200; tick++)
            q2_movers_tick(&set, 12, 0);

        check_eq_i(m->offset, 1491, "the cage reaches its lower stop");
        check_eq_i(m->state, Q2_MV_OPEN,
                   "and remains there instead of immediately rising");
    }

    q2_movers_free(&set);
}

/* BASE0 Events named CRATES plus LevelBin +0x0094 (DOCRATES). The LIFT1 is a
 * four-object binding with zero target/speed; the module writes each object's
 * Y displacement itself. */
static void test_crate_conveyor(void)
{
    static const u8 funcs[16] = {
        1, 0, 0, 0, 'L', 'I', 'F', 'T', '1', 0, 0, 0, 0, 0, 0, 0
    };
    u8 raw[48];
    u8 nodes[4 * Q2_SCENE_NODE_SIZE];
    q2_userfuncs uf;
    q2_events events;
    q2_mover_set set;
    q2_scene scene;
    s32 shift[3];

    printf("BASE0 crate conveyor\n");

    memset(raw, 0, sizeof(raw));
    hwr32(raw + 0, 1);                 /* one record */
    memcpy(raw + 4, "CRATES", 6);
    hwr32(raw + 16, 24);               /* named record at +24 */
    /* raw+20 is the directory terminator. */
    hwr16(raw + 24, 24);
    raw[26] = 1;
    raw[27] = Q2_EVREC_CAT_B;
    raw[28] = Q2_EVOP_CALL;
    raw[29] = 20;
    /* +2 is UserFuncs index zero; target/speed at +4/+6 remain zero. */
    hwr16(raw + 36, 0);                /* item +8: slots 0..3 */
    hwr16(raw + 38, 1);
    hwr16(raw + 40, 2);
    hwr16(raw + 42, 3);

    memset(&uf, 0, sizeof(uf));
    uf.data  = funcs;
    uf.size  = sizeof(funcs);
    uf.count = 1;

    memset(&events, 0, sizeof(events));
    events.data         = raw;
    events.size         = sizeof(raw);
    events.record_count = 1;
    events.dir_count    = 1;
    events.dir_offset   = 4;
    events.first_record = 24;

    memset(nodes, 0, sizeof(nodes));
    /* Scene +20/+32 are bbox min/max Y. The last centre crosses -1044 on its
     * first 30-unit step and therefore takes the -3500 wrap arm. */
    hwr32(nodes + 0 * Q2_SCENE_NODE_SIZE + 20, (u32)-2000);
    hwr32(nodes + 0 * Q2_SCENE_NODE_SIZE + 32, (u32)-1800);
    hwr32(nodes + 1 * Q2_SCENE_NODE_SIZE + 20, (u32)-2200);
    hwr32(nodes + 1 * Q2_SCENE_NODE_SIZE + 32, (u32)-2000);
    hwr32(nodes + 2 * Q2_SCENE_NODE_SIZE + 20, (u32)-1600);
    hwr32(nodes + 2 * Q2_SCENE_NODE_SIZE + 32, (u32)-1400);
    hwr32(nodes + 3 * Q2_SCENE_NODE_SIZE + 20, (u32)-1100);
    hwr32(nodes + 3 * Q2_SCENE_NODE_SIZE + 32, (u32)-1000);
    memset(&scene, 0, sizeof(scene));
    scene.nodes      = nodes;
    scene.node_count = 4;

    memset(&set, 0, sizeof(set));
    check(q2_movers_build_calls(&set, &events, &uf, NULL, &scene) == Q2_OK,
          "the zero-speed CRATES binding decodes");
    check_eq_i(set.count, 4,
               "its four slots become four independent runtime objects");
    if (set.count == 4) {
        check(set.movers[0].external && set.movers[3].external,
              "the generic lift state machine leaves the objects to LevelBin");
        check_eq_i(q2_movers_step_crates(&set, &events, &scene, 12), 4,
                   "DOCRATES writes all four objects");
        check_eq_i(set.movers[0].offset, 24,
                   "slot 0 advances by (16*12)/8");
        check_eq_i(set.movers[1].offset, 24,
                   "slot 1 uses the same conveyor speed");
        check_eq_i(set.movers[2].offset, 30,
                   "slot 2 advances by (20*12)/8");
        check_eq_i(set.movers[3].offset, -3470,
                   "slot 3 wraps 3500 back after crossing -1044");

        q2_movers_node_offset(&set, 2, shift);
        check(shift[0] == 0 && shift[1] == 30 && shift[2] == 0,
              "the authored object displacement reaches Scene rendering");
    }
    q2_movers_free(&set);
}

static void test_hull_movement(void)
{
    q2_sim sim;
    q2_input in;
    s32 spawn[3];
    s32 rise;
    int i;

    printf("movement against a hull\n");

    memset(&in, 0, sizeof(in));
    q2_sim_init(&sim, NULL, 50);

    if (!open_box_hull(&sim.coll)) {
        check(false, "the synthetic hull parses");
        return;
    }
    sim.coll_ready = true;

    /* Feet in the middle of the room; the origin is Q2_EYE_BASE above them. */
    spawn[0] = 2000;
    spawn[1] = q2_sim_feet_y(HULL_SY / 2);
    spawn[2] = 2000;
    q2_sim_spawn(&sim, spawn, 0);
    check(sim.player[0].ent.node == 0, "and the spawn lands in the one cell");

    /* Fall to the floor. The origin can reach the +Y face, so the feet settle
     * Q2_EYE_BASE below it. */
    for (i = 0; i < 60; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(sim.player[0].on_ground, "the player lands on the hull's floor");
    check_eq_i(sim.player[0].pos[1], q2_sim_feet_y(HULL_SY),
               "and rests with the origin exactly on the floor plane");
    check_eq_i(sim.player[0].vel[1], Q2_GRAVITY * Q2_DT_NOMINAL,
               "with the ground projection holding vertical velocity down");

    /*
     * Walk into the +X wall. It must stop the player, not let them through —
     * and it must get there at the speed the flat-ground tests measured. A
     * collision path that walks at a fraction of the free-movement speed is a
     * mover bug, and one that only shows up with a hull.
     */
    {
        s32 from = sim.player[0].ent.pos[0];
        int arrived = -1;

        in.forward = 0;
        in.side    = Q2_INPUT_FULL;      /* +X at yaw 0 */
        for (i = 0; i < 400; i++) {
            q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
            if (arrived < 0 && sim.player[0].vel[0] == Q2_SPEED_NORMAL)
                arrived = i;
        }
        check(sim.player[0].ent.pos[0] < HULL_SX,
              "walking into a wall stops inside the hull");
        check(sim.player[0].ent.pos[0] > HULL_SX - 200,
              "and gets all the way up to it");
        check(arrived >= 0 && arrived < 30,
              "reaching top speed takes the same dozen ticks it does in the open");
        printf("  walked %d units to origin x = %d of %d, top speed at tick %d\n",
               sim.player[0].ent.pos[0] - from, sim.player[0].ent.pos[0], HULL_SX,
               arrived);
    }

    /*
     * Jump into the ceiling. The ceiling's outward normal is (0, -4096, 0), so
     * `last_normal.y < max_slope_ny` holds — max_slope_ny is zero, §9.12.7 —
     * and the clip fires. The dot product removes the whole vertical component,
     * and since `keep` is zero for a rising entity the rescale does not run. So
     * the velocity ends at exactly zero rather than bouncing or sticking.
     */
    in.side = 0;
    for (i = 0; i < 60; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);

    in.buttons = Q2_BTN_JUMP;
    q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    in.buttons = 0;
    check(sim.player[0].vel[1] < 0, "the jump is rising");

    rise = sim.player[0].ent.pos[1];
    for (i = 0; i < 12; i++) {
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
        if (sim.player[0].ent.pos[1] < rise)
            rise = sim.player[0].ent.pos[1];
    }
    check(rise >= 0, "the jump never leaves the hull through the ceiling");

    /*
     * And it must actually leave the floor. This is the assertion that caught
     * the missing airborne branch at 0x80045CA4: with the step sequence running
     * every tick, the 216-unit step down swamps the ~100 units of upward delta a
     * jump produces and the player never moves at all — while every no-collision
     * test still passes, because there is no step sequence without a hull.
     */
    check(rise < HULL_SY, "and the jump does leave the floor");
    printf("  jump reached origin y = %d from %d (ceiling at 0)\n",
           rise, HULL_SY);

    /* It must come back down and land again. */
    for (i = 0; i < 60; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(sim.player[0].on_ground, "and lands again");
    check_eq_i(sim.player[0].ent.pos[1], HULL_SY, "back on the floor plane");

    /* q2_collision borrows the chunk it was parsed from; nothing to release. */
    sim.coll_ready = false;
}

/* ------------------------------------------------------------------------- */
/* 0x80046234 is not an obstruction test: it first tries to carry the player. */
static void test_mover_push(void)
{
    q2_sim sim;
    s32 spawn[3];
    s32 step[3] = { 180, 0, 0 };
    s32 before;
    s16 health;

    printf("mover push and crush\n");

    q2_sim_init(&sim, NULL, 50);
    if (!open_box_hull(&sim.coll)) {
        check(false, "the synthetic hull parses for a mover push");
        return;
    }
    sim.coll_ready = true;

    spawn[0] = 2000;
    spawn[1] = q2_sim_feet_y(HULL_SY / 2);
    spawn[2] = 2000;
    q2_sim_spawn(&sim, spawn, 0);

    check(q2_sim_mover_push(&sim, step),
          "a mover carries a player when the destination is clear");
    check_eq_i(sim.player[0].ent.pos[0], 2180,
               "the carry uses the full mover X step");
    check_eq_i(sim.player[0].pos[0], 2180,
               "and synchronises the feet-space player position");

    /* A pusher does not leave a partly slid player embedded in the wall. The
     * retail helper restores the origin and lets its caller deal MOD_CRUSH. */
    sim.player[0].ent.pos[0] = HULL_SX - 100;
    sim.player[0].pos[0]     = HULL_SX - 100;
    sim.player[0].ent.node   = 0;
    before = sim.player[0].ent.pos[0];
    check(!q2_sim_mover_push(&sim, step),
          "a pinned player vetoes the mover instead of receiving a partial push");
    check_eq_i(sim.player[0].ent.pos[0], before,
               "a failed carry restores the entity origin");
    check_eq_i(sim.player[0].pos[0], before,
               "and leaves the feet-space origin in lockstep");

    health = sim.combat.inv.health;
    q2_sim_mover_crush(&sim);
    check_eq_i(sim.combat.inv.health, health - Q2_MOVER_CRUSH_DAMAGE,
               "the mover rollback deals the retail 30 points of crush damage");
    check_eq_i(sim.combat.self.last_mod, Q2_MOD_CRUSH,
               "and records MOD_CRUSH rather than inventing knockback");

    sim.coll_ready = false;
}

/* ------------------------------------------------------------------------- */
/*
 * The clamped approach's own boundary. It must SNAP when a step would overshoot,
 * and the overshoot test is a 32-bit comparison against a 16-bit store — so a
 * rate large enough to wrap the s16 range must still snap rather than wrapping
 * to the far side and being stored.
 *
 * Reached through the view-height ease, whose rate is dt*4: a large dt gives a
 * large step, and the height must land on the target and stay there.
 */
static void test_ease_boundary(void)
{
    q2_sim sim;
    q2_input in;
    s32 spawn[3] = { 0, 0, 0 };
    int i;

    printf("clamped approach\n");

    memset(&in, 0, sizeof(in));
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = 0;

    /*
     * The view offset has 290 units to travel and the biggest legal step is
     * dt*4 = 120, so the third step would overshoot by 70. It must SNAP to 286
     * and never read 216 — and it must never read a value below the target on
     * any tick, which is what an unsnapped implementation would produce.
     */
    sim.env_flags = Q2_ENT_INLOWCROUCH;
    for (i = 0; i < 10; i++) {
        q2_sim_tick(&sim, &in, Q2_DT_MAX);
        if (sim.player[0].view_height < Q2_VIEW_CROUCH) {
            check(false, "the view offset never undershoots the crouch target");
            break;
        }
    }
    check_eq_i(sim.player[0].view_height, Q2_VIEW_CROUCH,
               "a step past the target snaps to it");

    for (i = 0; i < 10; i++)
        q2_sim_tick(&sim, &in, Q2_DT_MAX);
    check_eq_i(sim.player[0].view_height, Q2_VIEW_CROUCH,
               "and stays there rather than oscillating");

    /* The same from the other direction. */
    sim.env_flags = 0;
    for (i = 0; i < 10; i++)
        q2_sim_tick(&sim, &in, Q2_DT_MAX);
    check_eq_i(sim.player[0].view_height, Q2_VIEW_STAND,
               "and snaps to the target coming back up");

    /* And the wish velocity: a full-deflection target reached with the biggest
     * legal step must be the exact table value, not one unit either side. */
    in.forward = Q2_INPUT_FULL;
    for (i = 0; i < 20; i++)
        q2_sim_tick(&sim, &in, Q2_DT_MAX);
    check_eq_i(sim.player[0].wish[2], Q2_SPEED_NORMAL,
               "a large step still lands exactly on the max speed");
}

/* ------------------------------------------------------------------------- */
/*
 * One tick with dt = 2n must not equal two ticks with dt = n.
 *
 * This is the whole reason q2_sim_advance runs ONE variable-dt tick instead of
 * several nominal ones: every rate is k*dt through a clamped approach, and a
 * clamped approach is not additive. If a refactor "helpfully" reintroduces
 * sub-stepping, this is what catches it.
 */
static void test_variable_dt(void)
{
    q2_sim big, small;
    q2_input in;
    s32 spawn[3] = { 0, 0, 0 };

    printf("variable dt\n");

    memset(&in, 0, sizeof(in));
    in.forward = Q2_INPUT_FULL / 4;   /* a target the first step overshoots */

    q2_sim_init(&big, NULL, 50);
    q2_sim_spawn(&big, spawn, 0);
    big.player[0].ground_y = 0;

    q2_sim_init(&small, NULL, 50);
    q2_sim_spawn(&small, spawn, 0);
    small.player[0].ground_y = 0;

    q2_sim_tick(&big, &in, 24);
    q2_sim_tick(&small, &in, 12);
    q2_sim_tick(&small, &in, 12);

    check(big.player[0].wish[2] == small.player[0].wish[2],
          "the wish converges to the same target either way");

    /*
     * The angles are where it shows: the look rate is eased and then integrated,
     * so a single big step and two small ones land the view in different places.
     */
    in.forward = 0;
    in.yaw     = Q2_INPUT_FULL;
    q2_sim_tick(&big, &in, 24);
    q2_sim_tick(&small, &in, 12);
    q2_sim_tick(&small, &in, 12);

    check(big.player[0].yaw != small.player[0].yaw,
          "but one big step and two small ones do NOT give the same view angle");
    printf("  dt=24 once: yaw %d   dt=12 twice: yaw %d\n",
           big.player[0].yaw, small.player[0].yaw);
}

/* ------------------------------------------------------------------------- */
/*
 * Four players in one sim: their own health and weapon, one world.
 *
 * The point of the split is that the world half of a tick runs once a frame
 * however many players there are, while everything a player owns is theirs.
 * Both halves of that are checked here.
 */
static void test_four_players(void)
{
    q2_sim sim;
    q2_input in;
    s32 spawn[3] = { 0, 0, 0 };
    u32 ticks_after_one, ticks_after_three;

    printf("four players share one world\n");

    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);

    /* Player 1 gets its own place and its own inventory. */
    sim.cur_player = 1;
    q2_sim_spawn(&sim, spawn, 0);
    sim.cur_player = 0;
    q2_sim_player_reset_combat(&sim, 1);

    sim.combat.inv.health = 90;
    check_eq_i(sim.pcombat[1].inv.health, 100,
               "player 1 keeps its own health while player 0 is hurt");

    /* And hurting player 1 leaves player 0 alone. */
    memset(&in, 0, sizeof(in));
    q2_sim_advance_player(&sim, 1, &in, 12);
    check_eq_i(sim.combat.inv.health, 90,
               "player 0's health survives another player's tick");

    /*
     * The world half runs once a frame. Player 0's advance ticks the clock;
     * players 1..3 must not tick it again.
     */
    ticks_after_one = sim.tick_count;
    q2_sim_advance_player(&sim, 1, &in, 12);
    q2_sim_advance_player(&sim, 2, &in, 12);
    q2_sim_advance_player(&sim, 3, &in, 12);
    ticks_after_three = sim.tick_count;
    check_eq_i((int)(ticks_after_three - ticks_after_one), 0,
               "three more players do not advance the world clock");

    /* A weapon is a player's, not the world's. */
    sim.combat.weapon_id = 5;
    q2_sim_advance_player(&sim, 1, &in, 12);
    check_eq_i(sim.combat.weapon_id, 5,
               "player 0's weapon survives another player's tick");
}

/* ------------------------------------------------------------------------- */
/*
 * A creature's contact hit lands AT THE CREATURE.
 *
 * `0x800612F0` passes the attacker's own origin as the damage point, which is
 * the one line `q2_combat_melee` is, and `q2_sim_hurt_player` now enforces it
 * rather than trusting whatever point the caller supplies. The client used to
 * supply the player's own position, and this is the test that would have caught
 * it: with the point at the player, the difference the roll is computed from is
 * the zero vector, so every claw rolled the view the same way whichever side it
 * came from.
 */
static void test_melee_point(void)
{
    q2_sim   sim;
    q2_actor left, right;
    s32      wrong_point[3];
    s16      roll_left, roll_right;

    printf("melee\n");

    q2_sim_init(&sim, NULL, 50);
    sim.player[0].yaw = 0;

    wrong_point[0] = sim.player[0].pos[0];
    wrong_point[1] = sim.player[0].pos[1];
    wrong_point[2] = sim.player[0].pos[2];

    /* Two attackers, one either side along the view's right vector at yaw 0. */
    q2_actor_init(&left);
    left.health = 100;
    left.owner  = -1;
    left.origin[0] = sim.player[0].pos[0] - 500;
    left.origin[1] = sim.player[0].pos[1];
    left.origin[2] = sim.player[0].pos[2];

    right = left;
    right.origin[0] = sim.player[0].pos[0] + 500;

    /* Both are handed the SAME deliberately wrong point — the player's own
     * position — so anything but an override gives both the same roll. */
    q2_sim_hurt_player(&sim, &left, 20, Q2_MOD_MELEE, wrong_point);
    roll_left = sim.player[0].hurt_kick[1];

    q2_sim_init(&sim, NULL, 50);
    sim.player[0].yaw = 0;
    q2_sim_hurt_player(&sim, &right, 20, Q2_MOD_MELEE, wrong_point);
    roll_right = sim.player[0].hurt_kick[1];

    check(roll_left != 0 && roll_right != 0, "a claw that lands rolls the view");
    check((roll_left > 0) != (roll_right > 0),
          "and the two sides roll it opposite ways, so the point was overridden");
    printf("  roll from the left %d, from the right %d\n",
           (int)roll_left, (int)roll_right);

    /*
     * The control. A melee with no attacker has no origin to take, so the
     * caller's point stands and both sides collapse to the same answer — which
     * is exactly the behaviour the client had for every claw in the game.
     */
    q2_sim_init(&sim, NULL, 50);
    sim.player[0].yaw = 0;
    q2_sim_hurt_player(&sim, NULL, 20, Q2_MOD_MELEE, wrong_point);
    check(sim.player[0].hurt_kick[1] == roll_left ||
          sim.player[0].hurt_kick[1] == roll_right,
          "with no attacker the caller's point stands, as it must for lava");

    /* And the killer is recorded, which is what the scoring rule reads. */
    q2_sim_init(&sim, NULL, 50);
    left.owner = 2;
    q2_sim_hurt_player(&sim, &left, 20, Q2_MOD_MELEE, wrong_point);
    check_eq_i(sim.combat.self.last_attacker, 2,
               "a melee records who swung it");
}

/*
 * Picking a weapon up. The disc leaves the blaster and nothing else
 * (0x80037E78); autoswitch promotes to anything ranked higher on the console's
 * own preference list at 0x8009DB7C, and to nothing that is not on it.
 */
static void test_autoswitch(void)
{
    q2_sim sim;

    /* Off: the disc's rule, exactly. */
    q2_sim_init(&sim, NULL, 30);
    sim.autoswitch = false;
    sim.combat.weapon_id = Q2_WID_BLASTER;
    q2_sim_give_weapon(&sim, Q2_WID_SHOTGUN);
    check_eq_i(sim.combat.weapon_id, Q2_WID_SHOTGUN,
               "off: a pickup takes you out of the blaster");

    q2_sim_init(&sim, NULL, 30);
    sim.autoswitch = false;
    sim.combat.weapon_id = Q2_WID_SUPER_SHOTGUN;
    q2_sim_give_weapon(&sim, Q2_WID_SHOTGUN);
    check_eq_i(sim.combat.weapon_id, Q2_WID_SUPER_SHOTGUN,
               "off: holding anything else, the pickup is only stored");

    /* On: a better gun is taken up, one it cannot feed is not. */
    q2_sim_init(&sim, NULL, 30);
    sim.combat.weapon_id = Q2_WID_BLASTER;
    sim.combat.inv.ammo[Q2_AMMO_SHELLS] = 20;
    q2_sim_give_weapon(&sim, Q2_WID_SHOTGUN);
    check_eq_i(sim.combat.weapon_id, Q2_WID_SHOTGUN,
               "on: the shotgun beats the blaster");

    q2_sim_give_weapon(&sim, Q2_WID_SUPER_SHOTGUN);
    check_eq_i(sim.combat.weapon_id, Q2_WID_SUPER_SHOTGUN,
               "on: the super shotgun beats the shotgun on the same shells");

    /* An explosive is off the list and must never be promoted to. */
    q2_sim_init(&sim, NULL, 30);
    sim.combat.weapon_id = Q2_WID_BLASTER;
    sim.combat.inv.ammo[Q2_AMMO_GRENADES] = 5;
    q2_sim_give_weapon(&sim, Q2_WID_HAND_GRENADE);
    check_eq_i(sim.combat.weapon_id, Q2_WID_BLASTER,
               "on: a grenade never arms itself in your hand");
}

/* Grenade3's state lives across the weapon, view-model and projectile layers.
 * This pins the sim-side join: prime is free, cook follows the attached hand,
 * release spends one grenade, and a fuse that wins detonates without spending
 * it or leaving an invisible projectile behind. */
static void test_held_hand_grenade(void)
{
    const q2_weapon_tables *wt = q2_weapon_tables_builtin();
    q2_sim sim;
    q2_fire_result_v2 fire;
    s32 spawn[3] = { 0, 0, 0 };
    s32 attached[3] = { 100, -200, 300 };
    s32 eye[3];
    s32 h;

    printf("held hand grenade\n");

    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.fire_from_input = false;
    sim.combat.weapon_id = Q2_WID_HAND_GRENADE;
    sim.combat.inv.weapons |= wt->owned_bit[Q2_WID_HAND_GRENADE];
    sim.combat.inv.ammo[Q2_AMMO_GRENADES] = 3;

    fire = q2_sim_fire(&sim);
    check(fire.fired, "the view-model path primes Grenade3");
    check_eq_i(sim.combat.inv.ammo[Q2_AMMO_GRENADES], 3,
               "without spending ammo at prime");
    h = q2_projectile_hand_held_index(&sim.combat.projectiles, 0);
    check(h >= 0, "and leaves a hidden owner-attached entity");

    check_eq_i(q2_sim_hand_grenade_update(&sim, attached, 20, false),
               Q2_HAND_GRENADE_HELD,
               "the model update keeps it held");
    check_eq_i(sim.combat.projectiles.p[h].pos[0], attached[0],
               "at the view weapon's X");
    check_eq_i(sim.combat.projectiles.p[h].pos[1], attached[1],
               "at the view weapon's Y");
    check_eq_i(sim.combat.projectiles.p[h].pos[2], attached[2],
               "at the view weapon's Z");
    check_eq_i(q2_projectile_hand_charge(&sim.combat.projectiles, 0),
               Q2_HAND_GRENADE_CHARGE_START + 120,
               "with charge advanced by 6*dt");

    q2_sim_eye(&sim, eye);
    check_eq_i(q2_sim_hand_grenade_update(&sim, attached, 0, true),
               Q2_HAND_GRENADE_RELEASED,
               "the 411 signal releases it");
    check_eq_i(sim.combat.inv.ammo[Q2_AMMO_GRENADES], 2,
               "and only then spends one grenade");
    check_eq_i(q2_projectile_hand_held_index(&sim.combat.projectiles, 0), -1,
               "the entity is now in flight");
    check_eq_i(sim.combat.projectiles.p[h].pos[0] - eye[0],
               Q2_HAND_GRENADE_RELEASE_RIGHT,
               "from the read +80 right offset at yaw zero");
    check_eq_i(sim.combat.projectiles.p[h].pos[1] - eye[1],
               Q2_HAND_GRENADE_RELEASE_DOWN,
               "and the read -50 vertical offset");
    check_eq_i(sim.combat.projectiles.p[h].pos[2] - eye[2],
               Q2_HAND_GRENADE_RELEASE_FORWARD,
               "and the read +200 forward offset");

    /* No-view-model callers still get an immediate minimum-charge throw. */
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.combat.weapon_id = Q2_WID_HAND_GRENADE;
    sim.combat.inv.weapons |= wt->owned_bit[Q2_WID_HAND_GRENADE];
    sim.combat.inv.ammo[Q2_AMMO_GRENADES] = 3;
    fire = q2_sim_fire(&sim);
    check(fire.fired && fire.sound == Q2_WSND_HANDGREN_THROW,
          "a harness without a view model throws immediately and audibly");
    check_eq_i(sim.combat.inv.ammo[Q2_AMMO_GRENADES], 2,
               "that compatibility throw still spends one grenade");
    check_eq_i(q2_projectile_hand_held_index(&sim.combat.projectiles, 0), -1,
               "rather than orphaning a held state no model can drive");

    /* Fuse expiry wins over release and hurts the owner at the hand. */
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.fire_from_input = false;
    sim.combat.weapon_id = Q2_WID_HAND_GRENADE;
    sim.combat.inv.weapons |= wt->owned_bit[Q2_WID_HAND_GRENADE];
    sim.combat.inv.ammo[Q2_AMMO_GRENADES] = 3;
    fire = q2_sim_fire(&sim);
    h = q2_projectile_hand_held_index(&sim.combat.projectiles, 0);
    check(h >= 0, "a second held grenade exists for the fuse test");
    sim.level_time = sim.combat.projectiles.p[h].expires;
    check_eq_i(q2_sim_hand_grenade_update(&sim, attached, 0, true),
               Q2_HAND_GRENADE_EXPIRED,
               "an elapsed held fuse wins over release");
    check_eq_i(sim.combat.projectiles.live, 0,
               "and consumes the hidden projectile");
    check_eq_i(sim.combat.inv.ammo[Q2_AMMO_GRENADES], 3,
               "without charging ammo for a grenade never thrown");
    check(sim.combat.inv.health < 100,
          "the in-hand blast damages its owner");
}


/* ------------------------------------------------------------------------- */
/* Every step is in the way. */
static bool train_always_blocked(u32 index, const s32 step[3], void *user)
{
    (void)index; (void)step; (void)user;
    return true;
}

/*
 * THE TRAIN -- PLATFORM, the one mover in this engine that is not axis-aligned.
 *
 * The numbers are BIGGUN's, the only PLATFORM on the disc: Events+476 names the
 * destination (-55731, 11143, 289), speed -4 and Scene nodes 31, 32 and 30, and
 * node 31's box centre in that zone is (-23727, 3012, 287). Everything below
 * follows from those and from mover.h's reading of 0x8002C2D4.
 */
static void test_train(void)
{
    q2_mover      m;
    q2_mover_set  set;
    s32           d[3];
    u32           ticks;

    printf("train\n");

    memset(&m, 0, sizeof(m));
    m.is_path     = 1;
    m.part_count  = 3;
    m.node[0]     = 31;
    m.node[1]     = 32;
    m.node[2]     = 30;
    m.dir[0]      = -32004;   /* origin - node 31's box centre */
    m.dir[1]      = 8131;
    m.dir[2]      = 2;
    m.target      = 32516;    /* isqrt of the above, TRUNCATED to s16 */
    m.speed       = 4;
    m.axis        = 1;
    m.blocks_player = 1;
    m.wait_timer  = Q2_MOVER_WAIT_NEVER;
    m.wait_reset  = Q2_MOVER_WAIT_NEVER;
    m.sound_pending = Q2_MVSND_NONE;
    m.partner     = -1;
    m.portal_node = -1;

    set.movers   = &m;
    set.count    = 1;
    set.capacity = 1;

    /* At rest it displaces nothing, and at the far end it displaces exactly the
     * authored vector -- the division cancels whatever the target is. */
    m.offset = 0;
    q2_mover_displacement(&m, d);
    check(d[0] == 0 && d[1] == 0 && d[2] == 0, "train: at rest, no offset");

    m.offset = m.target;
    q2_mover_displacement(&m, d);
    check_eq_i(d[0], -32004, "train: arrives on the authored X");
    check_eq_i(d[1], 8131,   "train: arrives on the authored Y");
    check_eq_i(d[2], 2,      "train: arrives on the authored Z");

    /* Halfway is halfway on every axis, not on one of them. */
    m.offset = m.target / 2;
    q2_mover_displacement(&m, d);
    check(d[0] < -15900 && d[0] > -16100, "train: half the X at half the path");
    check(d[1] >  4000  && d[1] <  4100,  "train: half the Y at half the path");

    /* Every part reads the same displacement, and a node it does not own reads
     * none. This is the write down the +0x30 chain. */
    q2_movers_node_offset(&set, 32, d);
    check(d[0] < -15900 && d[0] > -16100, "train: part 2 moves with part 1");
    q2_movers_node_offset(&set, 30, d);
    check(d[0] < -15900 && d[0] > -16100, "train: part 3 moves with part 1");
    q2_movers_node_offset(&set, 99, d);
    check(d[0] == 0 && d[1] == 0 && d[2] == 0,
          "train: a node it does not own stays put");

    /* Triggered from rest it takes one tick to leave IDLE, one to spend a zero
     * delay, and then target/(speed*dt) to arrive. */
    m.offset      = 0;
    m.state       = Q2_MV_IDLE;
    m.delay_timer = 0;
    m.delay_reset = 0;
    q2_mover_trigger(&set, 0);
    for (ticks = 0; ticks < 2000 && m.state != Q2_MV_OPEN; ticks++)
        q2_movers_tick(&set, 20, 0);
    check_eq_i(m.offset, m.target, "train: it arrives exactly on the target");
    check(ticks < 2000, "train: and gets there in finite time");

    /* Blocked while opening it BACKS OFF speed*150 and heads for that, rather
     * than waiting out a sixteen-tick retry (0x8002CAE0). */
    m.offset      = 20000;
    m.state       = Q2_MV_OPENING;
    m.saved_state = Q2_MV_OPENING;
    m.wait_timer  = Q2_MOVER_WAIT_NEVER;
    q2_movers_tick_blocked(&set, 20, 0, train_always_blocked, NULL);
    check_eq_i(m.state, Q2_MV_BLOCKED, "train: an obstruction blocks it");
    check_eq_i(m.wait_timer, 20000 - 4 * Q2_MOVER_PATH_BACKOFF,
               "train: and it heads back 150 ticks' worth of travel");

    /* Clamped at the near end rather than wrapping: the console's test is an
     * unsigned compare against the target, which catches both ends. */
    m.offset      = 100;
    m.state       = Q2_MV_OPENING;
    m.saved_state = Q2_MV_OPENING;
    q2_movers_tick_blocked(&set, 20, 0, train_always_blocked, NULL);
    check_eq_i(m.wait_timer, 0, "train: the back-off stops at the start");

    /* And left alone, it walks back to that goal and resumes what it was
     * doing. */
    m.offset      = 20000;
    m.state       = Q2_MV_BLOCKED;
    m.saved_state = Q2_MV_OPENING;
    m.wait_timer  = 19000;
    for (ticks = 0; ticks < 200 && m.state == Q2_MV_BLOCKED; ticks++)
        q2_movers_tick(&set, 20, 0);
    check_eq_i(m.offset, 19000, "train: the retreat lands on its goal");
    check_eq_i(m.state, Q2_MV_OPENING, "train: and then carries on opening");

    /* A door is untouched by all of this: one axis, and the other two zero. */
    memset(&m, 0, sizeof(m));
    m.axis    = 1;
    m.offset  = 512;
    m.target  = 512;
    q2_mover_displacement(&m, d);
    check(d[0] == 0 && d[1] == 512 && d[2] == 0,
          "a lift still moves on its axis alone");
}

/* ------------------------------------------------------------------------- */
/*
 * A DOOR IS SOLID TO MORE THAN THE PLAYER'S FEET.
 *
 * The mover boxes had exactly one reader — the player's own step sweep — so a
 * closed door stopped you walking and stopped nothing else. Shots went through
 * it, rockets went through it, and creatures saw and shot through it, because
 * every one of those queries is answered out of the collision HULL and a mover
 * is a runtime entity that no hull contains.
 *
 * Both halves are checked here because they are one fix: `q2_sim_trace` gained
 * the entity pass and the AI binding gained the same pass over the same boxes.
 */
static void test_movers_block_sight_and_shots(void)
{
    q2_sim sim;
    q2_ai_world_bind bind;
    q2_move_target door[2];
    q2_trace tr;
    s32 from[3] = { 2000, 300, 500 };
    s32 to[3]   = { 2000, 300, 3500 };

    printf("doors are solid to shots and to sight\n");

    q2_sim_init(&sim, NULL, 50);
    if (!open_box_hull(&sim.coll)) {
        check(false, "the synthetic hull parses");
        return;
    }
    sim.coll_ready = true;

    /* The room is open along Z, so the hull alone lets the whole shot through:
     * this is the arm that used to return before the entity pass ever ran. */
    q2_sim_trace(&sim, from, to, &tr);
    check(!tr.hit, "with no door, the shot crosses the room");
    check_eq_i(tr.fraction, 4096, "for its whole length");
    check_eq_i(tr.ent, -1, "and names no mover");

    /* A door across it, and a trigger volume in front of the door. */
    memset(door, 0, sizeof(door));
    door[0].min[0] = 0;       door[0].min[1] = 0;       door[0].min[2] = 2000;
    door[0].max[0] = HULL_SX; door[0].max[1] = HULL_SY; door[0].max[2] = 2200;
    door[0].kind   = Q2_MOVE_KIND_ENTITY;
    door[0].id     = 4;
    door[0].active = true;

    door[1] = door[0];
    door[1].min[2] = 1000; door[1].max[2] = 1200;
    door[1].kind   = Q2_MOVE_KIND_VOLUME;
    door[1].mask   = 0x2000;
    door[1].id     = 6;

    sim.volumes      = NULL;    /* not owned; q2_sim_free must not free it */
    sim.volume_count = 2;
    sim.mover_count  = 1;
    memset(&sim.move_world, 0, sizeof(sim.move_world));
    sim.move_world.half_extent = Q2_SWEEP_HALF_EXTENT;
    sim.move_world.targets     = door;
    sim.move_world.count       = 2;

    q2_sim_trace(&sim, from, to, &tr);
    check(tr.hit, "with the door shut, the shot is stopped");
    check_eq_i(tr.end[2], 2000, "at the door's near face");
    check_eq_i(tr.ent, 4, "naming the mover it hit");
    check_eq_i(tr.fraction, (2000 - 500) * 4096 / (3500 - 500),
               "half way down the room, which is where the door is");
    check(tr.end[2] > door[1].max[2],
          "and the trigger volume in front of it stopped nothing");

    /* Opened — the box moves out of the way, exactly as q2_sim_movers_update
     * slides it — and the shot goes through again. */
    door[0].min[2] = 100000; door[0].max[2] = 100200;
    q2_sim_trace(&sim, from, to, &tr);
    check(!tr.hit, "with the door open the shot crosses again");
    door[0].min[2] = 2000; door[0].max[2] = 2200;

    /*
     * A shot that stops on the HULL before it reaches the door keeps the
     * hull's answer: the pass takes the nearer of the two, and it is the door
     * that is further here.
     */
    {
        s32 wall_to[3] = { 2000, 300, HULL_SZ + 5000 };

        door[0].min[2] = HULL_SZ + 1000;
        door[0].max[2] = HULL_SZ + 1200;
        q2_sim_trace(&sim, from, wall_to, &tr);
        check(tr.hit, "a shot at the far wall is stopped");
        check_eq_i(tr.ent, -1, "by the hull, not by the door behind it");
        check_eq_i(tr.end[2], HULL_SZ, "at the wall");
        door[0].min[2] = 2000; door[0].max[2] = 2200;
    }

    /*
     * AND THE SIGHT LINE, over the same boxes. This is the one that makes
     * creatures stop attacking through a shut door: every fire hook gates each
     * shot on `q2_visible`, which ends in this call.
     */
    q2_ai_world_bind_init(&bind, &sim.coll, NULL);
    check(bind.world.line_of_sight(bind.world.user, from, to),
          "with no doors bound, the creature can see across the room");

    q2_ai_world_bind_entities(&bind, &sim.move_world);
    check(!bind.world.line_of_sight(bind.world.user, from, to),
          "with the door bound, it cannot");
    check_eq_i(bind.stats.los_blocked_ent, 1,
               "and the block is recorded as a door rather than as geometry");

    /* Sight along the room but not across the door is still clear, so the clip
     * is the door and not the binding refusing everything. */
    {
        s32 near_to[3] = { 2000, 300, 1500 };

        check(bind.world.line_of_sight(bind.world.user, from, near_to),
              "a creature on this side of the door still sees this side");
    }

    door[0].min[2] = 100000; door[0].max[2] = 100200;
    check(bind.world.line_of_sight(bind.world.user, from, to),
          "and once the door opens, sight is restored");

    q2_ai_set_world(NULL);
}

/* ------------------------------------------------------------------------- */
/* GLASS owns a normal entity box until the fatal hit frees that box. */
static void test_glass_solidity_lifetime(void)
{
    u8 raw[Q2_SCENE_NODE_SIZE];
    q2_scene scene;
    q2_sim sim;
    q2_move_target pane;
    q2_move_contact hit;
    q2_trace tr;
    q2_breakable *b;
    s32 from[3] = { 2000, 300,  500 };
    s32 to[3]   = { 2000, 300, 3500 };
    const s32 bmin[3] = { 1900,   0, 2000 };
    const s32 bmax[3] = { 2100, 600, 2100 };
    int k;

    printf("breakable glass solidity lifetime\n");

    memset(raw, 0, sizeof(raw));
    for (k = 0; k < 3; k++) {
        hwr32(raw + 0x10 + k * 4, (u32)bmin[k]);
        hwr32(raw + 0x1C + k * 4, (u32)bmax[k]);
    }
    memset(&scene, 0, sizeof(scene));
    scene.nodes      = raw;
    scene.node_count = 1;

    q2_sim_init(&sim, NULL, 50);
    check(open_box_hull(&sim.coll), "the glass test's open hull parses");
    sim.coll_ready = true;
    memset(&pane, 0, sizeof(pane));
    for (k = 0; k < 3; k++) {
        pane.min[k] = pane.env_min[k] = bmin[k];
        pane.max[k] = pane.env_max[k] = bmax[k];
    }
    pane.kind   = Q2_MOVE_KIND_ENTITY;
    pane.id     = 0;
    pane.active = true;

    sim.volumes               = &pane; /* borrowed for this synthetic case */
    sim.volume_count          = 1;
    sim.breakable_solid_count = 1;
    sim.move_world.targets    = &pane;
    sim.move_world.count      = 1;
    sim.move_world.half_extent = 0;
    sim.breakable_scene       = &scene;
    sim.breakable_count       = 1;

    b = &sim.breakable[0];
    memset(b, 0, sizeof(*b));
    b->scene_node   = 0;
    b->health       = 10;
    b->kind         = Q2_BREAKABLE_GLASS;
    b->solid_target = 0;
    for (k = 0; k < 3; k++) {
        b->bmin[k] = bmin[k];
        b->bmax[k] = bmax[k];
    }

    check(!q2_move_sweep_world(&sim.move_world, from, to, &hit),
          "an intact pane blocks movement");
    check_eq_i(hit.kind, Q2_MOVE_KIND_ENTITY,
               "the pane is an entity solid, not a trigger volume");
    q2_sim_trace(&sim, from, to, &tr);
    check(tr.hit, "an intact pane also stops a point trace");
    check_eq_i(tr.ent, 0,
               "even when the map has zero movers to arm the entity pass");
    check_eq_i(tr.end[2], bmin[2], "the trace stops at the pane's near face");

    (void)q2_sim_breakable_shot(&sim, from, to, 9);
    check(!b->broken, "a surviving hit leaves the pane intact");
    check(pane.active, "and leaves its solid box active");

    (void)q2_sim_breakable_shot(&sim, from, to, 1);
    check(b->broken, "the fatal hit shatters the pane");
    check_eq_i(sim.breakable_hits, 2,
               "each shot reaching the pane is counted exactly once");
    check(!pane.active, "and frees its solid box in the same call");
    check(q2_move_sweep_world(&sim.move_world, from, to, &hit),
          "movement passes through the broken pane");
    q2_sim_trace(&sim, from, to, &tr);
    check(!tr.hit, "a point trace also passes after the pane is broken");

    /* Save restore changes `broken` in bulk; the public sync is its bridge to
     * the runtime box table. Exercise both directions so loading an old save
     * cannot resurrect an invisible wall or remove an intact one. */
    b->broken = false;
    q2_sim_breakables_sync_solidity(&sim);
    check(pane.active, "restoring intact state restores solidity");
    b->broken = true;
    q2_sim_breakables_sync_solidity(&sim);
    check(!pane.active, "restoring broken state removes solidity");

    /* The local target is borrowed, whereas q2_sim_free owns normal arrays. */
    sim.volumes = NULL;
    sim.volume_count = 0;
    sim.move_world.targets = NULL;
    sim.move_world.count = 0;
    q2_sim_free(&sim);
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    printf("Q2PSX-PC simulation tests\n\n");

    test_item_group_selection();
    test_script_fx_damage();
    test_event_contact_categories();
    test_underwater_air();
    test_autoswitch();
    test_held_hand_grenade();
    test_tick_rate();
    test_gravity();
    test_scene_lights();
    test_ground_and_view();
    test_movement();
    test_wish_velocity();
    test_look();
    test_jump();
    test_ground_projection();
    test_liquid();
    test_fall_damage();
    test_piston_decode();
    test_cagelift_timers();
    test_crate_conveyor();
    test_hull_movement();
    test_mover_push();
    test_ease_boundary();
    test_variable_dt();
    test_four_players();
    test_melee_point();
    test_train();
    test_movers_block_sight_and_shots();
    test_glass_solidity_lifetime();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");

    return g_failures ? 1 : 0;
}
