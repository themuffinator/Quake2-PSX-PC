/*
 * test_creature.c — the per-creature layer: binding a decoded module to code.
 *
 * The DECODER is checked against the disc by `q2psx-inspect creatures`, which
 * runs it over all fifteen module instances and reports what it found; there
 * is no point duplicating that here with a synthetic module image.
 *
 * What is checked here is the join: that a decoded creature's moves reach the
 * frame driver with their end callbacks resolved, that a move is found by its
 * first frame rather than its position, and that a creature with no hand
 * written implementation still animates instead of standing still.
 */
#include "random.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "crebind.h"
#include "creworld.h"
#include "creature.h"
#include "mission.h"
#include "shock.h"
#include "spawn.h"

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

static void put_u16(u8 *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void put_u32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

/* ------------------------------------------------------------------------- */
static void test_frame_name_table(void)
{
    q2_creature c = { 0 };
    u8 image[200] = { 0 };
    q2_cre_frame_name names[4];
    c.base = 0x80100000u;
    /* Plausible strings before the export must not become an animation table. */
    memcpy(image + 24, "Bogus", 6); memcpy(image + 44, "Wrong", 6);
    put_u32(image + 12, c.base + 80);
    put_u16(image + 80, 0); put_u16(image + 82, 20);
    memcpy(image + 84, "Stand", 6);
    put_u16(image + 100, 21); put_u16(image + 102, 0);
    memcpy(image + 104, "Stand2", 7);
    put_u16(image + 120, 50); put_u16(image + 122, 69);
    memcpy(image + 124, "Walk", 5);
    put_u16(image + 140, 0xFFFF);
    put_u16(image + 160, 70); put_u16(image + 162, 78);
    memcpy(image + 164, "After end", 10);
    check(q2_creature_frame_names(&c, image, sizeof(image), names, 4) == 2 &&
          names[0].first == 0 && names[0].last == 20 &&
          !strcmp(names[0].name, "Stand") && names[1].first == 50 &&
          !strcmp(names[1].name, "Walk"),
          "frame names follow export 3, skip reversed ranges and stop at the sentinel");
    put_u32(image + 12, 0);
    check(q2_creature_frame_names(&c, image, sizeof(image), names, 4) == 0,
          "an absent name export does not search arbitrary module strings");
    put_u32(image + 12, c.base + sizeof(image) - 4);
    check(q2_creature_frame_names(&c, image, sizeof(image), names, 4) == 0,
          "a truncated exported record cannot be read past its image");
}

static int g_batch_stands;

static void batch_stand(q2_monster *m)
{
    g_batch_stands++;
    m->frame = 123;
}

static void test_summoned_batch_is_started(void)
{
    q2_creature_world w;
    q2_monster monster;
    u8 population[28];

    puts("a CREBATCH wake starts its formerly dormant monsters");

    memset(&w, 0, sizeof(w));
    memset(population, 0, sizeof(population));
    memcpy(population, "LiftRoom", 8);
    w.pop.data = population;
    w.pop.size = sizeof(population);
    w.pop.group_count = 1;
    w.pop_ready = true;

    q2_monster_init(&monster);
    monster.group = 0;
    monster.health = 20;
    monster.in_use = false;       /* held by q2_creature_world_hold_batches */
    monster.spawn_pending = true;
    monster.spawnflags |= Q2_SVFLAG_INUSE;
    monster.stand = batch_stand;
    w.set.monsters = &monster;
    w.set.count = 1;

    q2_level_reset();
    g_batch_stands = 0;
    check_eq_i(q2_creature_world_summon(&w, "LiftRoom"), 1,
               "the dormant record is selected");
    check(monster.in_use, "its port-side in-use latch is raised");
    check_eq_i(g_batch_stands, 1,
               "monster_start_go installs the creature's standing move");
    check(monster.think == q2_M_MoveFrame,
          "and installs the retail frame driver");
    check_eq_i(monster.next_think, 1,
               "with a live next-think deadline");
    check_eq_i(q2_creature_world_summon(&w, "LiftRoom"), 0,
               "a second selection does not restart an active creature");
    monster.in_use = false;
    check_eq_i(q2_creature_world_summon(&w, "LiftRoom"), 0,
               "a freed creature cannot be mistaken for an unspawned batch");
    check(!monster.in_use && !monster.spawn_pending,
          "summoning is one-shot even after the record becomes inactive");
}

static void test_batch_zone_eligibility(void)
{
    q2_creature_world w;
    q2_monster monsters[5];
    u8 population[5 * Q2_POP_GROUP_SIZE + 4] = {0};
    const char *names[] = { "Zone0", "Zone0Room2", "Zone1", "Zone1Key", "Ambush" };
    u32 i;

    memset(&w, 0, sizeof(w));
    for (i = 0; i < 5; i++) {
        memcpy(population + i * Q2_POP_GROUP_SIZE, names[i], strlen(names[i]));
        q2_monster_init(&monsters[i]);
        monsters[i].group = (u16)i;
        monsters[i].in_use = true;
        monsters[i].health = 20;
        monsters[i].stand = batch_stand;
    }
    w.pop.data = population;
    w.pop.size = sizeof(population);
    w.pop.group_count = 5;
    w.pop_ready = true;
    w.set.monsters = monsters;
    w.set.count = 5;

    /* The client already rejected this unzoned record against the hull. */
    monsters[4].in_use = false;
    q2_level_reset();
    check_eq_i(q2_creature_world_hold_batches(&w, 0, NULL, 0), 3,
               "only the exact resident zone group starts");
    check(monsters[0].in_use && !monsters[0].spawn_pending,
          "ordinary zone population stays live");
    check(!monsters[1].in_use && monsters[1].spawn_pending,
          "a resident suffixed group waits for its script");
    check(monsters[2].spawn_pending && monsters[3].spawn_pending &&
          !monsters[4].spawn_pending,
          "only records accepted by the resident hull become pending");
    check_eq_i(q2_creature_world_summon(&w, "Zone1"), 1,
               "an explicit batch may select a group overlapping the resident zone");
    check_eq_i(q2_creature_world_summon(&w, "Ambush"), 0,
               "CREBATCH cannot bypass the client's hull exclusion");
    check_eq_i(q2_creature_world_summon(&w, "Zone0Room2"), 1,
               "the resident ambush is still reachable");
    check(monsters[1].think == q2_M_MoveFrame &&
          !monsters[1].spawn_pending, "the ambush starts and consumes its latch");
}

/* ------------------------------------------------------------------------- */
static void test_player_noise_stamps_the_window(void)
{
    q2_creature_world w;

    puts("PlayerNoise stamps the noise so the five-second window can run");

    memset(&w, 0, sizeof(w));
    q2_monster_init(&w.sight);
    w.ready = true;

    q2_level_reset();
    q2_level_state.time     = 137;
    q2_level_state.framenum = 137;

    q2_creature_world_player_noise(&w, true);
    check(q2_level_state.sound_entity == &w.sight,
          "a weapon noise goes to the loud channel");
    check_eq_i(q2_level_state.sound_entity_framenum, 137,
               "with this tick's framenum");
    check_eq_i(w.sight.teleport_time, 137,
               "and 0x80062CD4's level.time stamp, which ai_checkattack "
               "measures staleness against");

    q2_level_state.time     = 200;
    q2_level_state.framenum = 200;
    q2_creature_world_player_noise(&w, false);
    check(q2_level_state.sound2_entity == &w.sight,
          "the player's own noise goes to the quiet channel");
    check_eq_i(w.sight.teleport_time, 200, "and is stamped the same way");
}

/* ------------------------------------------------------------------------- */
/* A creature shaped like a decoded one, without needing a disc               */
/* ------------------------------------------------------------------------- */
#define TEST_CLASS 77
#define ADDR_STAND 0x80101000u
#define ADDR_RUN   0x80102000u
#define ADDR_FIRE  0x80103000u

static q2_creature g_cre;

static void build_creature(void)
{
    memset(&g_cre, 0, sizeof(g_cre));

    memcpy(g_cre.name, "Testbeast", 10);
    g_cre.base           = 0x80100000u;
    g_cre.class_byte[0]  = TEST_CLASS;
    g_cre.class_count    = 1;
    g_cre.speed_scale    = 14;
    g_cre.mass           = 250;

    g_cre.callback[0] = ADDR_STAND;     /* stand */
    g_cre.callback[4] = ADDR_RUN;       /* run   */

    g_cre.method[5]   = ADDR_RUN;       /* the run callback is also a think */
    g_cre.method[6]   = ADDR_FIRE;
    g_cre.method_count = 7;

    /* Two moves: a looping stand and a run whose end callback is the run
     * function, which is the shape every creature on the disc uses. */
    g_cre.move[0].addr         = 0x80104000u;
    g_cre.move[0].first_frame  = 146;
    g_cre.move[0].last_frame   = 149;
    g_cre.move[0].frame_index  = 0;
    g_cre.move[0].frame_count  = 4;
    g_cre.move[0].endfunc_addr = 0;
    g_cre.move[0].via          = 0;

    g_cre.move[1].addr         = 0x80104010u;
    g_cre.move[1].first_frame  = 99;
    g_cre.move[1].last_frame   = 101;
    g_cre.move[1].frame_index  = 4;
    g_cre.move[1].frame_count  = 3;
    g_cre.move[1].endfunc_addr = ADDR_RUN;
    g_cre.move[1].via          = 4;

    g_cre.move_count = 2;

    /* stand frames */
    g_cre.frames[0].ai = Q2_AI_STAND; g_cre.frames[0].dist = 0;
    g_cre.frames[0].think = 0;
    g_cre.frames[1] = g_cre.frames[0];
    g_cre.frames[2] = g_cre.frames[0];
    g_cre.frames[3] = g_cre.frames[0];
    /* run frames, the middle one firing */
    g_cre.frames[4].ai = Q2_AI_RUN; g_cre.frames[4].dist = 10;
    g_cre.frames[4].think = 0;
    g_cre.frames[5].ai = Q2_AI_RUN; g_cre.frames[5].dist = 10;
    g_cre.frames[5].think = 6;
    g_cre.frames[6] = g_cre.frames[4];

    g_cre.frame_count = 7;
}

/* ------------------------------------------------------------------------- */
static int g_stand_calls, g_run_calls, g_fire_calls;

static void t_stand(q2_monster *m) { g_stand_calls++; q2_cre_set_move(m, 146); }
static void t_run(q2_monster *m)   { g_run_calls++;   q2_cre_set_move(m, 99);  }
static void t_fire(q2_monster *m)  { (void)m; g_fire_calls++; }

static const q2_cre_impl g_impl = {
    "Testbeast",
    { t_stand, NULL, NULL, NULL, t_run, NULL, NULL, NULL,
      NULL, NULL, NULL, NULL, NULL },
    { NULL, NULL, NULL, NULL, NULL, t_run, t_fire, NULL,
      NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
      NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
      NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
    NULL
};

/* ------------------------------------------------------------------------- */
static void test_bind(void)
{
    q2_cre_bind bind;
    q2_monster m;

    printf("binding a decoded creature\n");

    build_creature();
    q2_cre_bind_reset();
    q2_class_table_reset();

    check(q2_creature_bind(&bind, &g_cre, &g_impl), "a creature binds");
    check_eq_i(bind.move_count, 2, "with both its moves");

    /* The think handlers reach the engine's class table, which is what the
     * module's own init does through import +0x118. */
    check(q2_class_method_get(TEST_CLASS, 6) == t_fire,
          "think handlers are registered for the class byte");

    q2_monster_init(&m);
    q2_creature_spawn(&bind, &m, 0);

    check_eq_i(m.class_byte, TEST_CLASS, "spawning takes the class byte");
    check_eq_i(m.speed_scale, 14, "and the module's animation speed scale");
    check(m.stand == t_stand, "and the stand callback");
    check(m.run == t_run, "and the run callback");
    check((m.svflags & Q2_SVF_MONSTER) != 0, "and is flagged a monster");
    check(q2_ent_inuse(&m), "and is in use");
}

/* ------------------------------------------------------------------------- */
/* The shared population-to-entity flag hand-off, before module spawn runs. */
static void test_population_spawn_flags(void)
{
    enum { SPAWN_LIST = Q2_POP_GROUP_SIZE + 4,
           SIZE = SPAWN_LIST + Q2_POP_SPAWN_SIZE + 4 };
    u8 bytes[SIZE];
    q2_population pop;
    q2_monster_set set;
    q2_monster *m;

    printf("population spawn flags\n");

    memset(bytes, 0, sizeof(bytes));
    memcpy(bytes, "Zone0", 5);
    put_u32(bytes + 0x0C, SPAWN_LIST);
    /* bytes + 24 is the group's four-byte terminator. */

    put_u32(bytes + SPAWN_LIST, 34);  /* Insane class id: module reads flags */
    put_u16(bytes + SPAWN_LIST + 0x12, 0xFFFEu);
    put_u16(bytes + SPAWN_LIST + 0x14, 0x81D8u);
    /* One zero class-id word after this record terminates its spawn list. */

    memset(&pop, 0, sizeof(pop));
    pop.data        = bytes;
    pop.size        = sizeof(bytes);
    pop.group_count = 1;

    memset(&set, 0, sizeof(set));
    q2_monster_set_register(&set, 34);
    check_eq_i(q2_spawn_from_population(&set, &pop, NULL, NULL, NULL), Q2_OK,
               "one hand-built record spawns");
    check_eq_i(set.count, 1, "the record produced one monster");

    m = set.count ? &set.monsters[0] : NULL;
    check(m != NULL, "the spawned monster exists");
    if (m) {
        check_eq_i(m->target, -2,
                   "the link halfword reaches the entity target without a sentinel rewrite");
        check_eq_i((m->spawnflags >> Q2_POP_SPAWN_FLAGS_SHIFT) &
                       Q2_POP_SPAWN_FLAGS_MASK,
                   0x1D8,
                   "only the record's low nine flag bits reach entity bits 18..26");
        check((m->spawnflags & Q2_SVFLAG_INUSE) != 0,
              "the shared copy preserves the independent in-use bit");
    }

    q2_monster_set_free(&set);
}

/*
 * THE DIFFICULTY FILTER ON A SPAWN RECORD — SpawnGeneral, 0x800565C4.
 *
 * Nothing in this port consulted it: every skill put out the hard setting's
 * full roster. The predicate is the rule; wiring it into the record loop is
 * spawn.c's (see the note there). The masks are 0x800 / 0x1000 / 0x2000 on the
 * record's +0x14, NOT a place record's 0x2000 / 0x4000 / 0x8000.
 */
static void test_spawn_difficulty_masks(void)
{
    printf("the spawn record's difficulty bits\n");

    check(q2_pop_spawn_allows_skill(0x0000, 0), "an unmarked record spawns on easy");
    check(q2_pop_spawn_allows_skill(0x0000, 1), "on medium");
    check(q2_pop_spawn_allows_skill(0x0000, 2), "and on hard");

    /* 0x80056620/0x80056628: skill 0 reads +0x14 and tests 0x800. */
    check(!q2_pop_spawn_allows_skill(Q2_POP_SPAWN_NOT_EASY, 0),
          "the 0x800 bit removes the record on easy");
    check(q2_pop_spawn_allows_skill(Q2_POP_SPAWN_NOT_EASY, 1),
          "and only on easy");
    check(q2_pop_spawn_allows_skill(Q2_POP_SPAWN_NOT_EASY, 2),
          "hard included");

    /* 0x8005663C/0x80056644 and 0x80056658/0x80056660. */
    check(!q2_pop_spawn_allows_skill(Q2_POP_SPAWN_NOT_MEDIUM, 1),
          "0x1000 removes it on medium");
    check(q2_pop_spawn_allows_skill(Q2_POP_SPAWN_NOT_MEDIUM, 0),
          "and leaves easy alone");
    check(!q2_pop_spawn_allows_skill(Q2_POP_SPAWN_NOT_HARD, 2),
          "0x2000 removes it on hard");
    check(q2_pop_spawn_allows_skill(Q2_POP_SPAWN_NOT_HARD, 1),
          "and leaves medium alone");

    /*
     * The 0x81D8 record test_population_spawn_flags builds carries 0x8000 and
     * the low nine bits 0x1D8, and NONE of the three difficulty bits, which
     * sit at 11, 12 and 13 between them. It spawns at every skill — the high
     * bits the entity filler discards are not the filter's, and reading the
     * word as a place record's would wrongly refuse it on hard.
     */
    check(q2_pop_spawn_allows_skill(0x81D8u, 0) &&
          q2_pop_spawn_allows_skill(0x81D8u, 1) &&
          q2_pop_spawn_allows_skill(0x81D8u, 2),
          "0x81D8 carries no difficulty bit and spawns at every skill");
    check(!q2_pop_place_allows_skill(0x81D8u, 2),
          "while the same word read as a place record's would be not-hard");

    /*
     * The fall-through arm, 0x800565F0..0x8005661C: skill 3 and anything
     * negative reach 0x8005666C past all three tests.
     */
    check(q2_pop_spawn_allows_skill(Q2_POP_SPAWN_DIFFICULTY, 3),
          "skill 3 filters nothing");
    check(q2_pop_spawn_allows_skill(Q2_POP_SPAWN_DIFFICULTY, -1),
          "and neither does a negative skill");

    /* The two record layouts must not be conflated: a place record's NOT_EASY
     * bit means nothing in a spawn record's flags word and vice versa. */
    check(q2_pop_spawn_allows_skill(Q2_POP_PLACE_NOT_MEDIUM, 1),
          "a place record's 0x4000 is not a spawn record's medium bit");
    check(q2_pop_place_allows_skill(Q2_POP_SPAWN_NOT_EASY, 0),
          "and a spawn record's 0x800 is not a place record's easy bit");
}

/*
 * AND THE RECORD LOOP ACTUALLY APPLIES IT. The predicate above was transcribed
 * and tested with no caller, so every skill still put out the hard setting's
 * whole roster. SpawnGeneral asks GetDifficultySetting at 0x800565E0 BEFORE it
 * allocates anything and returns from the epilogue at 0x800567E0 on a hit, so a
 * refused record produces no monster at all — not a hidden one, not a sleeping
 * one. `skill_filtered` counts them apart from the failures so the "N of M
 * placed" line stays honest.
 */
static void test_spawn_loop_honours_difficulty(void)
{
    enum { SPAWN_LIST = Q2_POP_GROUP_SIZE + 4,
           SIZE = SPAWN_LIST + 2 * Q2_POP_SPAWN_SIZE + 4 };
    u8 bytes[SIZE];
    q2_population pop;
    q2_monster_set set;
    q2_spawn_stats st;
    s32 saved = q2_cre_skill();

    printf("difficulty removes a creature from the map\n");

    memset(bytes, 0, sizeof(bytes));
    memcpy(bytes, "Zone0", 5);
    put_u32(bytes + 0x0C, SPAWN_LIST);

    /* Two records of the same class. The first is marked NOT_MEDIUM at +0x14,
     * the second carries no difficulty bit at all. */
    put_u32(bytes + SPAWN_LIST, 18);
    put_u16(bytes + SPAWN_LIST + 0x14, (u16)Q2_POP_SPAWN_NOT_MEDIUM);
    put_u32(bytes + SPAWN_LIST + Q2_POP_SPAWN_SIZE, 18);
    put_u16(bytes + SPAWN_LIST + Q2_POP_SPAWN_SIZE + 0x14, 0);

    memset(&pop, 0, sizeof(pop));
    pop.data        = bytes;
    pop.size        = sizeof(bytes);
    pop.group_count = 1;

    q2_cre_set_skill(1);
    memset(&set, 0, sizeof(set));
    memset(&st, 0, sizeof(st));
    q2_monster_set_register(&set, 18);
    check_eq_i(q2_spawn_from_population(&set, &pop, NULL, NULL, &st), Q2_OK,
               "the population spawns on medium");
    check_eq_i(st.records, 2, "both records were seen");
    check_eq_i(st.skill_filtered, 1,
               "the 0x1000 record is REMOVED on medium, not merely hidden");
    check_eq_i(set.count, 1, "and only one creature exists");
    check_eq_i(st.no_module, 0,
               "a skill-filtered record is not miscounted as a missing module");
    q2_monster_set_free(&set);

    /* The same population on easy keeps both: 0x1000 is the medium bit alone. */
    q2_cre_set_skill(0);
    memset(&set, 0, sizeof(set));
    memset(&st, 0, sizeof(st));
    q2_monster_set_register(&set, 18);
    check_eq_i(q2_spawn_from_population(&set, &pop, NULL, NULL, &st), Q2_OK,
               "and on easy");
    check_eq_i(st.skill_filtered, 0, "nothing is removed on easy");
    check_eq_i(set.count, 2, "both creatures exist");
    q2_monster_set_free(&set);

    q2_cre_set_skill(saved);
}

/*
 * ARRIVING AT A PATH CORNER — CalcAbsFromCreBox 0x8005CB00 and VectBoxOverlap
 * 0x800552D8, as BoxEdicts pairs them at 0x8005C81C/0x8005C828.
 *
 * A corner is not a point. Its absolute bounds are made up on the spot by the
 * class-114 arm at 0x8005CB20 — origin ∓ 96 on every axis — so the window is
 * the creature's hull grown by 96, 382 per axis rather than 286.
 */
static void test_corner_trigger_box(void)
{
    q2_monster m, c;

    printf("the path corner's trigger cube\n");

    q2_monster_init(&m);
    q2_monster_init(&c);
    m.pos[0] = m.pos[1] = m.pos[2] = 0;
    c.pos[0] = c.pos[1] = c.pos[2] = 0;

    check(q2_creature_corner_overlaps(&m, &c),
          "a corner under the creature's origin is reached");

    /* The old point-in-hull test accepted out to 286 and no further. The
     * console's box reaches 286 + 96. */
    c.pos[0] = 300;
    check(q2_creature_corner_overlaps(&m, &c),
          "and one 300 units away, which the point test dropped");

    c.pos[0] = 382;
    check(q2_creature_corner_overlaps(&m, &c),
          "382 is inside: VectBoxOverlap's tests are strict, so touching "
          "faces overlap");

    c.pos[0] = 383;
    check(!q2_creature_corner_overlaps(&m, &c),
          "383 is outside — the window is closed, not open");

    /* Every axis, including the vertical one where the port had no slack at
     * all: a corner authored below the floor the creature settles on. */
    c.pos[0] = 0;
    c.pos[1] = 350;
    check(q2_creature_corner_overlaps(&m, &c),
          "the same 96 applies on the vertical axis");
    c.pos[1] = 0;
    c.pos[2] = -370;
    check(q2_creature_corner_overlaps(&m, &c),
          "and on Z, in both directions");

    /* It is the MOVER's own box, not a constant: a corpse-sized hull would
     * shrink the window with it. The diagonal is not a sphere — 382 on each
     * axis at once is still an overlap, because this is a box test. */
    c.pos[0] = 380; c.pos[1] = 380; c.pos[2] = 380;
    check(q2_creature_corner_overlaps(&m, &c),
          "and the test is a box, so the corner of the box counts");

    check(!q2_creature_corner_overlaps(NULL, &c) &&
          !q2_creature_corner_overlaps(&m, NULL),
          "a missing end overlaps nothing");
}

/*
 * WHICH CLASSES THE KILL TALLY IS ABOUT — CreatureCount 0x80057860 for the
 * denominator, ProcessQuakeDeath 0x8007CD5C for the numerator.
 *
 * Both halves of the port's tally were class-blind, so the board asked the
 * player to kill the wounded marines that are scenery. The two rules are NOT
 * the same rule and the difference is the console's: three classes are refused
 * the denominator and one the numerator.
 */
static void test_kill_tally_classes(void)
{
    printf("which classes the kill tally counts\n");

    /* `(u32)(class - 34) < 3` skips the increment at 0x80057868. */
    check(!q2_mission_class_counts(34), "the Insane is not in the denominator");
    check(!q2_mission_class_counts(35), "nor Rider2");
    check(!q2_mission_class_counts(36), "nor Rider3");
    check(q2_mission_class_counts(33), "33 is");
    check(q2_mission_class_counts(37), "and so is 37 — the window is exactly 3");
    check(q2_mission_class_counts(0),
          "and class 0 is, which an unsigned subtract is what makes true");

    /* 0x8007CD60 is one `beq` against 34 and nothing more. */
    check(!q2_mission_class_kills(34), "shooting an Insane raises nothing");
    check(q2_mission_class_kills(35),
          "but shooting a Rider2 does, though it was never counted — the "
          "console's own asymmetry, left alone");
    check(q2_mission_class_kills(36), "and a Rider3 the same");
    check(q2_mission_class_kills(18), "a Soldier counts on both halves");
}

static void test_move_lookup(void)
{
    q2_cre_bind bind;
    q2_monster m;

    printf("moves are found by first frame, not by position\n");

    build_creature();
    q2_cre_bind_reset();
    q2_class_table_reset();
    q2_creature_bind(&bind, &g_cre, &g_impl);
    q2_monster_init(&m);
    q2_creature_spawn(&bind, &m, 0);

    check(q2_cre_find_move(&m, 146) != NULL, "the stand move is found at 146");
    check(q2_cre_find_move(&m, 99) != NULL, "the run move is found at 99");
    check(q2_cre_find_move(&m, 4242) == NULL,
          "a frame the disc does not have is not found");

    check(q2_cre_set_move(&m, 99), "installing by first frame works");
    check_eq_i(m.currentmove->first_frame, 99, "and installs that move");
    check_eq_i(m.currentmove->last_frame, 101, "with its own range");

    check(!q2_cre_set_move(&m, 4242),
          "and a missing animation fails visibly rather than substituting");
}

static void test_endfunc_resolution(void)
{
    q2_cre_bind bind;
    q2_monster m;

    printf("end callbacks resolve through the implementation\n");

    build_creature();
    q2_cre_bind_reset();
    q2_class_table_reset();
    q2_creature_bind(&bind, &g_cre, &g_impl);
    q2_monster_init(&m);
    q2_creature_spawn(&bind, &m, 0);

    /* The run move's end callback is the module's run function, which the
     * implementation supplies — so it must come out as t_run. */
    q2_cre_set_move(&m, 99);
    check(m.currentmove->endfunc == (q2_endfunc)t_run,
          "an endfunc that is also a callback resolves to it");

    /* The stand move has none, and must loop rather than pick one up. */
    q2_cre_set_move(&m, 146);
    check(m.currentmove->endfunc == NULL, "a move with no endfunc loops");
}

static void test_frames_drive(void)
{
    q2_cre_bind bind;
    q2_monster m;
    int i;

    printf("the decoded frames actually drive the creature\n");

    build_creature();
    q2_cre_bind_reset();
    q2_class_table_reset();
    q2_ai_set_world(NULL);
    q2_level_reset();
    q2_creature_bind(&bind, &g_cre, &g_impl);

    g_stand_calls = g_run_calls = g_fire_calls = 0;

    q2_monster_init(&m);
    q2_creature_spawn(&bind, &m, 0);

    /*
     * A creature with no enemy does not run — ai_checkattack stands it down
     * on the first tick, which is correct and would make this a test of
     * nothing. So give it something to chase.
     */
    {
        static q2_monster target;
        q2_monster_init(&target);
        target.in_use      = true;
        target.spawnflags |= Q2_SVFLAG_INUSE;
        target.client      = true;
        target.health      = 100;
        target.pos[2]      = 4000;
        m.enemy      = &target;
        m.goalentity = &target;
        m.yaw_speed  = 4096;
        m.health     = 100;
    }

    q2_cre_set_move(&m, 99);
    m.frame = 99;

    /* Five ticks over a three-frame move: enough to reach the last frame and
     * then run the end callback on the tick after it. */
    for (i = 0; i < 5; i++) {
        q2_level_state.framenum++;
        q2_level_state.time++;
        q2_M_MoveFrame(&m);
    }

    check(g_fire_calls > 0,
          "a frame's think index reaches the creature's own handler");
    check(g_run_calls > 0,
          "and the move's end callback hands control back to run");

    /* The run frames carry a distance, so the creature must have advanced. */
    check(m.pos[0] != 0 || m.pos[2] != 0,
          "and the per-frame distances move it");
}

/* Records what the fire hook was handed, for the decoded-fire check below. */
static int g_test_fire_count;
static int g_test_fire_flash;

static void test_fire_hook(q2_monster *m, int flash, void *user)
{
    (void)m; (void)user;
    g_test_fire_count++;
    g_test_fire_flash = flash;
}

static void test_generic_fallback(void)
{
    q2_cre_bind bind;
    q2_monster m;
    const q2_cre_impl *impl;

    printf("a creature with no transcription still animates\n");

    build_creature();
    q2_cre_bind_reset();
    q2_class_table_reset();

    impl = q2_cre_impl_find("Nosuchbeast");
    check(impl != NULL, "an unknown module gets the generic implementation");
    check(impl->name == NULL, "which is not pretending to be that creature");

    q2_creature_bind(&bind, &g_cre, impl);
    q2_monster_init(&m);
    q2_creature_spawn(&bind, &m, 0);

    check(m.stand != NULL, "it still has a stand callback");
    m.stand(&m);
    check(m.currentmove != NULL, "which installs a real decoded move");
    check_eq_i(m.currentmove->first_frame, 146,
               "the one the module's own stand callback installs");

    m.run(&m);
    check_eq_i(m.currentmove->first_frame, 99, "and likewise for run");

    /* But it performs no per-frame action, which is the whole of what is
     * missing and must not be papered over. */
    /* Its think handlers are the decoded-action trampolines, not NULL: the
     * creature acts from what was read off the disc. */
    check(q2_class_method_get(TEST_CLASS, 6) != NULL,
          "and a think handler that runs the decoded action");

    /*
     * And a decoded FIRE reaches the fire hook. The five import slots the
     * projectile spawners live in were named out of the loader at 0x8007DA00
     * and confirmed by what each calls — the rocket at +0x98 goes through
     * 0x8004AF28, which combat.h already records as being called from that very
     * address. Before this, a CALL step of any kind did nothing, which is why
     * six of the seven modules hunted the player and never shot.
     */
    {
        static q2_cre_think think[8];

        memset(think, 0, sizeof(think));
        think[6].step_count      = 1;
        think[6].step[0].op      = Q2_CRE_OP_CALL;
        think[6].step[0].import_ofs = 0x98;          /* the rocket */

        q2_creature_bind_thinks(&bind, think, 8);

        q2_cre_set_fire_hook(test_fire_hook, NULL);
        g_test_fire_count = 0;
        g_test_fire_flash = -1;

        /* No enemy: a decoded creature must not shoot at nothing. */
        m.enemy = NULL;
        q2_cre_run_think(&m, 6);
        check_eq_i(g_test_fire_count, 0, "no enemy, no shot");

        /* An enemy that is alive: the shot goes. */
        {
            static q2_monster foe;

            q2_monster_init(&foe);
            foe.health = 100;
            m.enemy = &foe;
            q2_cre_run_think(&m, 6);
            check_eq_i(g_test_fire_count, 1, "a decoded fire reaches the hook");
            check_eq_i(g_test_fire_flash, 0x98,
                       "carrying the import slot it came from");

            /* A dead enemy stops it, the same guard every refire has. */
            foe.health = 0;
            q2_cre_run_think(&m, 6);
            check_eq_i(g_test_fire_count, 1, "and a dead enemy stops it");
        }

        /* The vector-maths imports are NOT shots: 40 of the disc's 107 call
         * steps are these, and treating them as fire would have every creature
         * shoot three times an animation frame. */
        m.enemy = NULL;
        {
            static q2_monster foe2;

            q2_monster_init(&foe2);
            foe2.health = 100;
            m.enemy = &foe2;
            think[6].step[0].import_ofs = 0xC0;
            q2_cre_run_think(&m, 6);
            check_eq_i(g_test_fire_count, 1,
                       "muzzle arithmetic is not a shot");
        }

        q2_cre_set_fire_hook(NULL, NULL);
    }
}

static void test_soldier_present(void)
{
    const q2_cre_impl *impl;

    printf("the transcribed creatures are registered\n");

    impl = q2_cre_impl_find("Soldier");
    check(impl != NULL && impl->name != NULL, "the Soldier is transcribed");

    if (impl && impl->name) {
        /* The fourteen think indices its animations actually use. */
        static const int used[] = { 2, 3, 4, 6, 7, 8, 9, 10, 11,
                                    16, 17, 18, 20, 21 };
        u32 i;
        int have = 0;
        for (i = 0; i < sizeof(used) / sizeof(used[0]); i++)
            if (impl->method[used[i]])
                have++;
        check_eq_i(have, 14, "with all fourteen of its think indices covered");

        check(impl->callback[7] == NULL,
              "and no melee, which is what its module declares");
        check(impl->callback[0] != NULL, "but a stand callback");
        check(impl->callback[11] != NULL, "a pain callback");
        check(impl->callback[12] != NULL, "and a die callback");
    }
}

/* ------------------------------------------------------------------------- */
/* The decoded-action executor                                                */
/* ------------------------------------------------------------------------- */
static int g_snd_calls; static int g_snd_last;
static int g_mel_calls; static s32 g_mel_dmg, g_mel_kick, g_mel_aim0;
static s32 g_mel_aim[3];

static void spy_sound(q2_monster *m, int which, void *u)
{
    (void)m; (void)u; g_snd_calls++; g_snd_last = which;
}

static void spy_melee(q2_monster *m, const s32 aim[3], s32 dmg, s32 kick,
                      void *u)
{
    (void)m; (void)u;
    g_mel_calls++; g_mel_dmg = dmg; g_mel_kick = kick; g_mel_aim0 = aim[0];
    memcpy(g_mel_aim, aim, sizeof(g_mel_aim));
}

static void test_actions(void)
{
    q2_cre_bind bind;
    q2_monster m;
    static q2_cre_think th[Q2_CLASS_METHOD_COUNT];

    printf("running a think function that was decoded rather than written\n");

    build_creature();
    q2_cre_bind_reset();
    q2_class_table_reset();
    q2_creature_bind(&bind, &g_cre, q2_cre_impl_find("Nosuchbeast"));

    memset(th, 0, sizeof(th));

    /* Index 6, shaped like the Berserk's club frame: a grunt and a swing. */
    th[6].step_count          = 2;
    th[6].step[0].op          = Q2_CRE_OP_SOUND;
    th[6].step[0].addr        = 0x80101758u;
    th[6].step[1].op          = Q2_CRE_OP_MELEE;
    th[6].step[1].aim[0]      = 1020;
    th[6].step[1].damage_base = 5;
    th[6].step[1].damage_rand = 3;
    th[6].step[1].kick        = 400;

    /* Index 7, a refire — gated, as every refire on the disc is. */
    th[7].step_count    = 1;
    th[7].step[0].op    = Q2_CRE_OP_NEXTFRAME;
    th[7].step[0].frame = 42;
    th[7].step[0].gated = true;

    /* Index 8, the duck helpers' aiflags store. */
    th[8].step_count       = 1;
    th[8].step[0].op       = Q2_CRE_OP_AIFLAG;
    th[8].step[0].flag_set = Q2_AI_HOLD_FRAME;

    q2_creature_bind_thinks(&bind, th, Q2_CLASS_METHOD_COUNT);

    q2_monster_init(&m);
    q2_creature_spawn(&bind, &m, 0);

    q2_cre_set_sound_hook(spy_sound, NULL);
    q2_cre_set_melee_hook(spy_melee, NULL);
    g_snd_calls = g_mel_calls = 0;
    memset(&q2_cre_actions, 0, sizeof(q2_cre_actions));

    /*
     * SOMETHING TO SWING AT, in reach. `fire_hit` (0x80061118) measures
     * `enemy->origin - self->origin` before it does anything else, so a melee
     * step with no enemy or one past `aim[0]` is refused — which is what the
     * two arms below pin.
     */
    {
        static q2_monster victim;

        q2_monster_init(&victim);
        victim.health = 100;
        victim.pos[0] = m.pos[0];
        victim.pos[1] = m.pos[1];
        victim.pos[2] = m.pos[2] + 900;    /* inside the 1020 reach */
        m.enemy = &victim;

        q2_cre_run_think(&m, 6);
        check_eq_i(g_snd_calls, 1, "a decoded sound step plays a sound");
        check_eq_i(g_snd_last, (int)0x80101758,
                   "identified by the module address of its handle");
        check_eq_i(g_mel_calls, 1, "and a decoded melee step swings");
        check_eq_i(g_mel_aim0, 1020, "with the module's own aim distance");
        check_eq_i(g_mel_kick, 400, "and its own kick");
        check(g_mel_dmg >= 5 && g_mel_dmg < 8,
              "and damage in the module's own base-plus-spread range");

        /*
         * 0x80061198 `slt v1, aim[0], range` and the jump to the epilogue at
         * 0x800614B4: past the reach, the swing does nothing. This is the
         * player who backs off during the wind-up, which used to be clubbed
         * from any distance at all.
         */
        victim.pos[2] = m.pos[2] + 5000;
        q2_cre_run_think(&m, 6);
        check_eq_i(g_mel_calls, 1, "a swing past the aim's reach lands nothing");
        check_eq_i((int)q2_cre_actions.melee_short, 1,
                   "and is counted as short rather than lost");

        /*
         * STRICTLY greater: `range == aim[0]` still hits, because the console
         * compares `aim[0] < range` and not `<=`.
         */
        victim.pos[2] = m.pos[2] + 1020;
        q2_cre_run_think(&m, 6);
        check_eq_i(g_mel_calls, 2, "a swing at exactly the reach still lands");

        /* And the four counters partition the calls. */
        check_eq_i((int)q2_cre_actions.melee_calls,
                   (int)(q2_cre_actions.melee_sent +
                         q2_cre_actions.melee_no_hook +
                         q2_cre_actions.melee_no_enemy +
                         q2_cre_actions.melee_short),
                   "and every swing is counted exactly once");

        m.enemy = NULL;
        q2_cre_run_think(&m, 6);
        check_eq_i(g_mel_calls, 2, "a swing with no enemy swings at nothing");
    }

    /*
     * A gated refire must not fire at a corpse. Every refire on the disc opens
     * with that guard, so it is read rather than invented — and without it a
     * creature jumps back into its firing frame forever.
     */
    m.nextframe = 0;
    m.enemy     = NULL;
    q2_cre_run_think(&m, 7);
    check_eq_i(m.nextframe, 0, "a gated refire does nothing with no enemy");

    {
        static q2_monster live;
        q2_monster_init(&live);
        live.health = 50;
        m.enemy = &live;
        q2_cre_run_think(&m, 7);
        check_eq_i(m.nextframe, 42, "and does fire at a live one");

        live.health = 0;
        m.nextframe = 0;
        q2_cre_run_think(&m, 7);
        check_eq_i(m.nextframe, 0, "but not once it is dead");
    }

    m.aiflags = 0;
    q2_cre_run_think(&m, 8);
    check((m.aiflags & Q2_AI_HOLD_FRAME) != 0,
          "a decoded aiflags step reaches the flags word");

    g_snd_calls = 0;
    q2_cre_run_think(&m, 20);
    check_eq_i(g_snd_calls, 0, "an index that decoded to nothing does nothing");

    q2_cre_set_sound_hook(NULL, NULL);
    q2_cre_set_melee_hook(NULL, NULL);
}

/* ------------------------------------------------------------------------- */
/* Module sound fallbacks — Soldier module+0xE50, Tankcomm module+0x808       */
/* ------------------------------------------------------------------------- */
#define TEST_MOD_BASE 0x80100000u

static const q2_cre_sound_bind k_soldier_binds[] = {
    { 0x801032A4u, "sol_sght1" },
    { 0x801032ACu, "sol_pain1" },
    { 0x801032B0u, "sol_pain2" },
    { 0x801032B4u, "sol_pain3" },
    { 0x801032B8u, "sol_deth1" },
    { 0x801032BCu, "sol_deth2" },
    { 0x801032C0u, "sol_deth3" }
};

static const q2_cre_sound_bind k_tank_binds[] = {
    { 0x80102100u, "tnk_idle1" },
    { 0x80102110u, "tnk_step" },
    { 0x80102104u, "tnk_pain" }
};

/* The bank, as a NULL-terminated list of the names it carries. */
static bool stub_bank_has(const char *name, void *user)
{
    const char *const *bank = (const char *const *)user;
    u32 i;

    for (i = 0; bank && bank[i]; i++)
        if (strcmp(bank[i], name) == 0)
            return true;

    return false;
}

static void check_name(const char *got, const char *want, const char *what)
{
    g_checks++;
    if ((got == NULL) != (want == NULL) ||
        (got && want && strcmp(got, want) != 0)) {
        printf("  FAIL  %s: got %s, want %s\n", what,
               got ? got : "(null)", want ? want : "(null)");
        g_failures++;
    }
}

static void test_sound_fallback(void)
{
    static const char *bank_pain1[] = { "sol_pain1", "sol_deth2", NULL };
    static const char *bank_both[]  = { "sol_pain1", "sol_pain2", NULL };
    static const char *bank_own[]   = { "sol_pain2", "sol_pain3", NULL };
    static const char *bank_empty[] = { NULL };
    static const char *bank_step[]  = { "tnk_step", "tnk_pain", NULL };
    static const char *bank_idle[]  = { "tnk_idle1", "tnk_step", NULL };
    static const char *bank_d12[]   = { "sol_deth1", "sol_deth2", NULL };
    static const char *bank_d13[]   = { "sol_deth1", "sol_deth3", NULL };
    static const char *bank_d23[]   = { "sol_deth2", "sol_deth3", NULL };
    static const char *bank_p13[]   = { "sol_pain1", "sol_pain3", NULL };
    const u32 nsol = (u32)(sizeof(k_soldier_binds) /
                           sizeof(k_soldier_binds[0]));
    const u32 ntnk = (u32)(sizeof(k_tank_binds) / sizeof(k_tank_binds[0]));
    u32 nfb = 0;

    printf("module sound fallbacks\n");

    check(q2_creature_sound_fallbacks(&nfb) != NULL && nfb == 23,
          "fallback slots cover native conditional sound registrations");

    /*
     * soldier_pain plays +0x32B4 and sol_pain3 is in NO bank on the disc, so
     * this address is the one every Soldier's flinch goes through. Before this
     * it resolved to "sol_pain3", the bank missed, and every Soldier on every
     * map that places one was silent when hurt.
     */
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032B4u,
                                         stub_bank_has, (void *)bank_pain1),
               "sol_pain1",
               "the pain slot falls back to sol_pain1 on a COMMAND-like bank");

    /* The die handler plays +0x32C0, sol_deth3 — absent only on COMMAND. */
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032C0u,
                                         stub_bank_has, (void *)bank_pain1),
               "sol_deth2",
               "and the death slot falls back to sol_deth2");

    /* pain2 is preferred: 0x80100E50 loads +0x32B0 first. */
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032B4u,
                                         stub_bank_has, (void *)bank_both),
               "sol_pain2",
               "with both present the module's own preference wins");

    /*
     * ...but a slot whose OWN name resolved is never overwritten: 0x80100EAC
     * `bne v0, zero` guards the store into +0x32B4.
     */
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032B4u,
                                         stub_bank_has, (void *)bank_own),
               "sol_pain3",
               "a slot the bank does carry keeps its own name");

    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032B4u,
                                         stub_bank_has, (void *)bank_empty),
               NULL,
               "a bank with none of the trio is silence, not a wrong name");

    /* Not in any group: the registration stands, bank or no bank. */
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032A4u,
                                         stub_bank_has, (void *)bank_empty),
               "sol_sght1",
               "an ungrouped address still resolves to its registration");

    /*
     * The Tank Commander: tnk_idle1 is in ZERO banks and tnk_step in the
     * thirteen that carry a Tank, so this substitution fires on every map.
     */
    check_name(q2_creature_sound_resolve("Tankcomm", k_tank_binds, ntnk,
                                         TEST_MOD_BASE, 0x80102100u,
                                         stub_bank_has, (void *)bank_step),
               "tnk_step",
               "the Tank's idle handle is overwritten with its footstep");
    check_name(q2_creature_sound_resolve("Tankcomm", k_tank_binds, ntnk,
                                         TEST_MOD_BASE, 0x80102100u,
                                         stub_bank_has, (void *)bank_idle),
               "tnk_idle1",
               "and would keep tnk_idle1 if any bank carried it");

    /*
     * The four names that really ARE silent by design must stay silent: they
     * have no group, so an absent one resolves to its registration and the
     * caller's bank lookup misses, exactly as before.
     */
    check_name(q2_creature_sound_resolve("Tankcomm", k_tank_binds, ntnk,
                                         TEST_MOD_BASE, 0x80102104u,
                                         stub_bank_has, (void *)bank_empty),
               "tnk_pain",
               "an ungrouped Tank sound is not substituted");

    /* A NULL bank means "carries everything", so nothing is substituted. */
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032B4u,
                                         NULL, NULL),
               "sol_pain3",
               "with no bank predicate the registration stands");

    /*
     * BY NAME, which is how the host actually meets the Soldier. soldier_pain
     * sends index 4 through the sound hook, not an address, and the host turns
     * that into "sol_pain3" through the transcribed table — so the address
     * form above can never see it. Every Soldier on every map that places one
     * was silent when hurt because that name went straight to the bank.
     */
    check_name(q2_creature_sound_fallback("Soldier",
                                          q2_cre_soldier_sound_name(4),
                                          stub_bank_has, (void *)bank_pain1),
               "sol_pain1",
               "and by name it falls back to sol_pain1 on a COMMAND-like bank");
    check_name(q2_creature_sound_fallback("Soldier",
                                          q2_cre_soldier_sound_name(4),
                                          stub_bank_has, (void *)bank_both),
               "sol_pain2", "or to the preferred sol_pain2 where it exists");
    check_name(q2_creature_sound_fallback("Soldier",
                                          q2_cre_soldier_sound_name(7),
                                          stub_bank_has, (void *)bank_pain1),
               "sol_deth2", "the die index falls back to sol_deth2");
    check_name(q2_creature_sound_fallback("Soldier", "sol_pain3",
                                          stub_bank_has, (void *)bank_empty),
               NULL, "and a bank with none of the trio is silence");
    check_name(q2_creature_sound_fallback("Tankcomm", "tnk_idle1",
                                          stub_bank_has, (void *)bank_step),
               "tnk_step", "the Tank's idle name becomes its footstep");
    check_name(q2_creature_sound_fallback("Tankcomm", "tnk_pain",
                                          stub_bank_has, (void *)bank_empty),
               "tnk_pain", "an ungrouped name passes through untouched");
    check_name(q2_creature_sound_fallback("Arachner", "sol_pain3",
                                          stub_bank_has, (void *)bank_pain1),
               "sol_pain3",
               "and a group belongs to its own module, not to the name");

    /*
     * THE SLOT KEEPS ITS OWN NAME BY NAME TOO. The name form is the one the
     * host is told to use, and every store in the fill pass is guarded on its
     * DESTINATION — 0x80100F1C `bne v0, zero` skips the store into +0x32C0,
     * 0x80100EAC the one into +0x32B4 — so a slot the bank carries is never
     * overwritten, even by a higher preference the bank also carries. Each
     * bank below carries the slot's own name AND a name the module prefers.
     */
    check_name(q2_creature_sound_fallback("Soldier", "sol_deth3",
                                          stub_bank_has, (void *)bank_d23),
               "sol_deth3",
               "by name, sol_deth3 stays sol_deth3 beside the preferred "
               "sol_deth2 (0x80100F1C)");
    check_name(q2_creature_sound_fallback("Soldier", "sol_pain3",
                                          stub_bank_has, (void *)bank_own),
               "sol_pain3",
               "and sol_pain3 stays sol_pain3 beside the preferred sol_pain2 "
               "(0x80100EAC)");
    check_name(q2_creature_sound_fallback("Soldier", "sol_pain1",
                                          stub_bank_has, (void *)bank_both),
               "sol_pain1",
               "and sol_pain1 stays sol_pain1 beside the preferred sol_pain2 "
               "(0x80100E7C)");

    /*
     * THE DEATH TRIO'S ORDER, deth2 then deth1 then deth3: 0x80100EB8 loads
     * [+0x32BC], 0x80100ECC [+0x32B8] if that was zero, 0x80100EDC [+0x32C0]
     * if that was too. Each pair is pinned with a bank that carries exactly
     * those two and not the slot asked about, so every other order gives a
     * different answer to at least one line — by address and by name.
     */
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032C0u,
                                         stub_bank_has, (void *)bank_d12),
               "sol_deth2",
               "deth2 before deth1: the +0x32C0 slot falls to sol_deth2");
    check_name(q2_creature_sound_fallback("Soldier", "sol_deth3",
                                          stub_bank_has, (void *)bank_d12),
               "sol_deth2", "and by name");
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032BCu,
                                         stub_bank_has, (void *)bank_d13),
               "sol_deth1",
               "deth1 before deth3: the +0x32BC slot falls to sol_deth1");
    check_name(q2_creature_sound_fallback("Soldier", "sol_deth2",
                                          stub_bank_has, (void *)bank_d13),
               "sol_deth1", "and by name");
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032B8u,
                                         stub_bank_has, (void *)bank_d23),
               "sol_deth2",
               "deth2 before deth3: the +0x32B8 slot falls to sol_deth2, not "
               "sol_deth3");
    check_name(q2_creature_sound_fallback("Soldier", "sol_deth1",
                                          stub_bank_has, (void *)bank_d23),
               "sol_deth2", "and by name");

    /*
     * AND THE PAIN TRIO'S, pain2 then pain1 then pain3 (0x80100E50, 0x80100E60,
     * 0x80100E70). pain2-before-pain1 is the bank_both line above; these are
     * the two pairs that involve pain3, which no line pinned.
     */
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032B0u,
                                         stub_bank_has, (void *)bank_p13),
               "sol_pain1",
               "pain1 before pain3: the +0x32B0 slot falls to sol_pain1");
    check_name(q2_creature_sound_fallback("Soldier", "sol_pain2",
                                          stub_bank_has, (void *)bank_p13),
               "sol_pain1", "and by name");
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032ACu,
                                         stub_bank_has, (void *)bank_own),
               "sol_pain2",
               "pain2 before pain3: the +0x32AC slot falls to sol_pain2");
    check_name(q2_creature_sound_fallback("Soldier", "sol_pain1",
                                          stub_bank_has, (void *)bank_own),
               "sol_pain2", "and by name");

    /*
     * AND THROUGH THE BIND, which is the call the host makes: it has the
     * creature and the registered name, and the module comes from the class
     * byte. A monster of another module is not substituted.
     */
    {
        static q2_creature sol;
        q2_cre_bind sb, tb;
        q2_monster s, t;

        memset(&sol, 0, sizeof(sol));
        memcpy(sol.name, "Soldier", 8);
        sol.base          = TEST_MOD_BASE;
        sol.class_byte[0] = 87;
        sol.class_count   = 1;

        build_creature();
        q2_cre_bind_reset();
        q2_class_table_reset();
        if (!q2_creature_bind(&sb, &sol, &g_impl) ||
            !q2_creature_bind(&tb, &g_cre, &g_impl)) {
            check(false, "the two test binds could not be made");
            return;
        }

        q2_monster_init(&s);
        s.class_byte = 87;
        q2_monster_init(&t);
        t.class_byte = TEST_CLASS;

        check_name(q2_cre_sound_resolve(&s, "sol_pain3", stub_bank_has,
                                        (void *)bank_pain1),
                   "sol_pain1", "a Soldier's pain resolves to what plays");
        check_name(q2_cre_sound_resolve(&t, "sol_pain3", stub_bank_has,
                                        (void *)bank_pain1),
                   "sol_pain3", "a Testbeast's is left alone");
        check_name(q2_cre_sound_resolve(NULL, "sol_pain3", stub_bank_has,
                                        (void *)bank_pain1),
                   "sol_pain3", "and no creature at all is no substitution");

        q2_cre_bind_reset();
        q2_class_table_reset();
    }
}

/* ------------------------------------------------------------------------- */
/* The enemy-alive guards belong to the refire callbacks, not to the shot     */
/* ------------------------------------------------------------------------- */
static int g_shot_calls;

static void spy_shot(q2_monster *m, const q2_cre_shot *shot, void *user)
{
    (void)m; (void)shot; (void)user;
    g_shot_calls++;
}

static void test_fire_shot_has_no_enemy_guard(void)
{
    static const q2_cre_shot shot = { 0x98, 0x98, 50, 550, 0, 0, 0, 1,
                                      false, { 0, 0, 0 }, { 0, 0, 0 } };
    q2_monster m, foe;
    u32 i;

    printf("a transcribed fire think shoots with no enemy\n");

    q2_monster_init(&m);
    q2_cre_set_shot_hook(spy_shot, NULL);

    /*
     * The Tank's machine-gun think loads the enemy at 0x80101074 and, on
     * `beq a3, zero`, writes a literal zero pitch at 0x801010F4 and falls
     * through to the fire at 0x80101160. It fires at nothing, aimed flat.
     */
    memset(&q2_cre_actions, 0, sizeof(q2_cre_actions));
    g_shot_calls = 0;
    m.enemy = NULL;
    q2_cre_fire_shot(&m, &shot);
    check_eq_i(g_shot_calls, 1, "a shot with no enemy still reaches the hook");
    check_eq_i(q2_cre_actions.fire_sent, 1, "and counts as sent");
    check(q2_cre_actions.shot_no_enemy == 1 && g_shot_calls == 1,
          "with shot_no_enemy an observation beside the shot, not a refusal");

    /*
     * AND NOT AS A REFUSAL AS WELL. fire_no_enemy is the decoded path's refusal
     * count (cre_actions.c), and the census prints it beside fire_sent as a
     * share of fire_calls. A sent shot counted in it too would be counted twice.
     */
    check(q2_cre_actions.fire_no_enemy == 0 &&
              q2_cre_actions.fire_calls ==
                  q2_cre_actions.fire_sent + q2_cre_actions.fire_no_hook +
                  q2_cre_actions.fire_no_enemy +
                  q2_cre_actions.fire_dead_enemy,
          "fire_no_enemy stays a refusal count, so sent + no_hook + no_enemy + "
          "dead_enemy is still fire_calls");

    /* And nowhere in the think is obj+0x108, the health, loaded at all. */
    q2_monster_init(&foe);
    foe.health = -10;
    memset(&q2_cre_actions, 0, sizeof(q2_cre_actions));
    g_shot_calls = 0;
    m.enemy = &foe;
    q2_cre_fire_shot(&m, &shot);
    check_eq_i(g_shot_calls, 1, "and a shot at a corpse reaches it too");
    check(q2_cre_actions.shot_dead_enemy == 1 && q2_cre_actions.fire_sent == 1,
          "observed in shot_dead_enemy and still sent");
    check(q2_cre_actions.fire_dead_enemy == 0 &&
              q2_cre_actions.fire_calls ==
                  q2_cre_actions.fire_sent + q2_cre_actions.fire_no_hook +
                  q2_cre_actions.fire_no_enemy +
                  q2_cre_actions.fire_dead_enemy,
          "and not in fire_dead_enemy, the refusal count, as well");
    q2_cre_set_shot_hook(NULL, NULL);

    /*
     * The guard used to be the only thing between a NULL enemy and a
     * dereference, so every transcribed think is run with one to prove none of
     * them needs it. A crash here is the finding, not a flake.
     */
    {
        const q2_cre_impl *const *impls = q2_cre_impls();
        u32 n_run = 0;

        q2_cre_set_shot_hook(spy_shot, NULL);
        g_shot_calls = 0;
        for (i = 0; impls[i]; i++) {
            u32 j;

            for (j = 0; j < Q2_CLASS_METHOD_COUNT; j++) {
                q2_monster t;

                if (!impls[i]->method[j])
                    continue;

                q2_monster_init(&t);
                t.health     = 100;
                t.max_health = 100;
                t.enemy      = NULL;
                impls[i]->method[j](&t);
                n_run++;
            }
        }
        q2_cre_set_shot_hook(NULL, NULL);

        check(n_run > 0 && g_shot_calls > 0,
              "every transcribed think ran with a NULL enemy without "
              "crashing, and at least one of them fired anyway — reaching "
              "the hook with no enemy is what the guard prevented");
    }
}

/* ------------------------------------------------------------------------- */
/* The Tank Commander's two halves of the rule, on its own code               */
/* ------------------------------------------------------------------------- */
/*
 * A Tankcomm-shaped creature with just the three blaster moves, so the refire
 * callback has somewhere to go: 55..70 attack_blast (module+0x1D18, think 8 on
 * 64, 67 and 70, ending in tank_reattack_blaster), 65..70 reattack_blast
 * (module+0x1D3C) and 71..76 post_blast (module+0x1D60). Addresses and ranges
 * as `q2psx-inspect creatures` prints them for COMMAND.
 */
static q2_creature g_tank;

static void build_tank(void)
{
    static const struct { u32 addr; s32 first, last; } k[3] = {
        { 0x80101D18u, 55, 70 },
        { 0x80101D3Cu, 65, 70 },
        { 0x80101D60u, 71, 76 }
    };
    u32 i, f = 0;

    memset(&g_tank, 0, sizeof(g_tank));
    memcpy(g_tank.name, "Tankcomm", 9);
    g_tank.base          = TEST_MOD_BASE;
    g_tank.class_byte[0] = 91;
    g_tank.class_count   = 1;

    for (i = 0; i < 3; i++) {
        u32 n = (u32)(k[i].last - k[i].first + 1);

        g_tank.move[i].addr        = k[i].addr;
        g_tank.move[i].first_frame = k[i].first;
        g_tank.move[i].last_frame  = k[i].last;
        g_tank.move[i].frame_index = f;
        g_tank.move[i].frame_count = n;
        f += n;
    }
    g_tank.move_count  = 3;
    g_tank.frame_count = f;
}

static void test_tank_burst_rule(void)
{
    const q2_cre_impl *impl = q2_cre_impl_find("Tankcomm");
    q2_cre_bind bind;
    q2_monster m, foe;
    int i, extended;

    printf("the Tank finishes a burst and decides at the refire\n");

    if (!impl || !impl->method[8] || !impl->method[3] || !impl->method[13]) {
        check(false, "the Tank's blaster, reattack and machine-gun thinks");
        return;
    }

    build_tank();
    q2_cre_bind_reset();
    q2_class_table_reset();
    q2_ai_set_world(NULL);
    if (!q2_creature_bind(&bind, &g_tank, impl)) {
        check(false, "a Tankcomm-shaped creature could not be bound");
        return;
    }

    q2_monster_init(&m);
    q2_creature_spawn(&bind, &m, 0);
    m.health = 750;

    q2_monster_init(&foe);
    foe.in_use = true;
    foe.health = -10;                       /* died on the volley's first shot */
    m.enemy = &foe;

    q2_cre_set_skill(3);
    q2_cre_set_shot_hook(spy_shot, NULL);

    /*
     * FIRST HALF: the volley already under way finishes. TankBlaster
     * (module+0xC4C) builds its aim off entity+0xBC and never loads obj+0x108,
     * so frames 64, 67 and 70 all go out at a target that is already dead.
     */
    memset(&q2_cre_actions, 0, sizeof(q2_cre_actions));
    g_shot_calls = 0;
    q2_cre_set_move(&m, 55);
    for (i = 0; i < 3; i++)
        impl->method[8](&m);
    check_eq_i(g_shot_calls, 3,
               "all three blaster frames fire at a dead target");

    /*
     * BOTH HALVES AT ONCE, over thirty-two volleys at a corpse: each volley
     * sends its three shots, then tank_reattack_blaster decides — and its
     * `lh v0, 264(v0)` / `blez` at module+0x144C sends a dead enemy to
     * post_blast every time, even on nightmare with the target in view. A
     * refire that DID extend would play reattack_blast (65..70), whose think 8
     * on 67 and 70 fires twice more. So the console's rule is exactly three
     * shots a volley: never none (the old shared guard) and never five.
     */
    memset(&q2_cre_actions, 0, sizeof(q2_cre_actions));
    g_shot_calls = 0;
    for (i = 0; i < 32; i++) {
        int k;

        q2_cre_set_move(&m, 55);
        for (k = 0; k < 3; k++)
            impl->method[8](&m);            /* frames 64, 67, 70 */
        impl->method[3](&m);                /* the endfunc of 55..70 */
        if (m.currentmove && m.currentmove->first_frame == 65) {
            impl->method[8](&m);            /* frames 67 and 70 of 65..70 */
            impl->method[8](&m);
        }
    }
    check_eq_i(g_shot_calls, 32 * 3,
               "every volley at a corpse finishes and none is extended");
    check(q2_cre_actions.shot_dead_enemy == 32 * 3 &&
              q2_cre_actions.fire_dead_enemy == 0 &&
              q2_cre_actions.fire_calls == q2_cre_actions.fire_sent,
          "all 96 are observed in shot_dead_enemy and none is counted as "
          "refused, so the census's sent share is the whole of fire_calls");

    /*
     * The CONTROL that keeps "none is extended" from being vacuous: the same
     * loop against a live target does extend, on id's `random() <= 0.6`, so
     * the refire really is reachable from here with skill, sight and the move
     * table all in place. Thirty-two tries at 0.6 all missing has odds of
     * about 2e-13.
     */
    foe.health = 100;
    extended = 0;
    for (i = 0; i < 32; i++) {
        q2_cre_set_move(&m, 55);
        impl->method[3](&m);
        if (m.currentmove && m.currentmove->first_frame == 65)
            extended++;
    }
    check(extended > 0, "control: a live target can be re-attacked");

    /*
     * AND THE MACHINE GUN WITH NO TARGET AT ALL. TankMachineGun's `beq a3,
     * zero, 0x801010F4` (module+0x107C) writes a zero pitch and falls through
     * to the fire at module+0x1160.
     */
    memset(&q2_cre_actions, 0, sizeof(q2_cre_actions));
    g_shot_calls = 0;
    m.enemy = NULL;
    impl->method[13](&m);
    check(g_shot_calls == 1 && q2_cre_actions.shot_no_enemy == 1,
          "TankMachineGun fires with no enemy, and is counted doing it");

    q2_cre_set_shot_hook(NULL, NULL);
    q2_cre_set_skill(1);
    q2_cre_bind_reset();
    q2_class_table_reset();
}

/* ------------------------------------------------------------------------- */
/* The machinegun guard's burst, and where a dying creature's rounds go       */
/* ------------------------------------------------------------------------- */
static q2_cre_shot g_last_shot;

static void spy_last_shot(q2_monster *m, const q2_cre_shot *shot, void *user)
{
    (void)m; (void)user;
    g_last_shot = *shot;
    g_shot_calls++;
}

static s16 g_muzzle_a, g_muzzle_b;

static bool spy_muzzle(const q2_monster *m, s16 a, s16 b, s32 out[3], void *u)
{
    (void)m; (void)u;
    g_muzzle_a = a;
    g_muzzle_b = b;
    out[0] = a == 239 ? -100 : 100;
    out[1] = -200;
    out[2] = 300;
    return true;
}

static void test_melee_geometry(void)
{
    q2_monster m, foe;
    s32 aim[3] = { 1020, 0, 0 }, point[3];
    s16 impulse[3];
    q2_monster_init(&m); q2_monster_init(&foe);
    m.enemy = &foe; foe.pos[2] = 1000;
    check(q2_cre_melee_contact(&m, aim, point) && point[0] == 0 &&
          point[1] == 0 && point[2] == 714,
          "central melee subtracts the target hull radius before projecting");
    q2_cre_melee_impulse(&foe, point, 400, impulse);
    check(foe.velocity[2] == 400 && impulse[2] == 6400,
          "the kick sets AI velocity and adds a separately scaled mover impulse");
    aim[1] = -48; aim[2] = -286;
    check(q2_cre_melee_contact(&m, aim, point) && point[0] == -286 &&
          point[1] == -48 && point[2] == 1000,
          "a hull-edge swing uses the target's side and the native vertical sign");
    point[0] = 0; point[1] = 500; point[2] = 1000;
    foe.on_ground = true;
    q2_cre_melee_impulse(&foe, point, 400, impulse);
    check(foe.velocity[1] == -3072 && impulse[1] == -3072 && !foe.on_ground,
          "vertical kick scales in place, caps upward lift and ungrounds the target");
    foe.pos[2] = 1021;
    check(!q2_cre_melee_contact(&m, aim, point), "an out-of-reach swing has no contact");
}

static void test_flyer_aim(void)
{
    const q2_cre_impl *impl = q2_cre_impl_find("Flyer");
    q2_monster m, foe;
    int frame;
    puts("Flyer alternating muzzles, raw eye aim, hyper flags and slash bounds");
    check(impl != &q2_cre_generic, "the Flyer has a native implementation");
    if (impl == &q2_cre_generic) return;
    q2_monster_init(&m);
    q2_monster_init(&foe);
    m.enemy = &foe;
    foe.pos[0] = 101; foe.pos[1] = -223; foe.pos[2] = 800;
    foe.view_height = -41;
    foe.velocity[2] = 10000;
    q2_cre_set_muzzle_hook(spy_muzzle, NULL);
    q2_cre_set_shot_hook(spy_last_shot, NULL);
    for (frame = 82; frame <= 89; frame++) {
        m.frame = (s16)frame;
        impl->method[(frame & 1) ? 9 : 8](&m);
        check(g_last_shot.slot == Q2_IMP_FIRE_BLASTER && g_last_shot.aimed &&
              g_last_shot.damage == 1 && g_last_shot.speed == 1000,
              "each Flyer round carries its native figures");
        check(g_last_shot.dir[0] == 1 && g_last_shot.dir[1] == -64 &&
              g_last_shot.dir[2] == 500,
              "the raw target-eye delta survives without velocity lead");
        check(g_last_shot.effects ==
                  (u32)((frame == 82 || frame == 85 || frame == 88) ? 64 : 0),
              "only frames 82, 85 and 88 request hyper flags");
        check(g_muzzle_a == ((frame & 1) ? 121 : 112) &&
              g_muzzle_b == ((frame & 1) ? 128 : 115),
              "the two posed muzzles alternate in native order");
    }
    q2_cre_set_sound_hook(spy_sound, NULL);
    q2_cre_set_melee_hook(spy_melee, NULL);
    m.mins[0] = -123; m.maxs[0] = 456;
    g_mel_calls = 0;
    impl->method[11](&m);
    check(g_mel_calls == 1 && g_mel_dmg == 5 && g_mel_kick == 0 &&
          g_mel_aim[0] == 1020 && g_mel_aim[1] == 0 && g_mel_aim[2] == -123,
          "the left slash uses the live left hull bound");
    impl->method[12](&m);
    check(g_mel_calls == 2 && g_mel_aim[2] == 456,
          "the right slash uses the live right hull bound");
    foe.pos[2] = 5000;
    impl->method[11](&m);
    check(g_mel_calls == 2 && (u32)g_snd_last == 0x80101778u,
          "a missed slash still plays its aliased swing sound");
    m.next_think = 100;
    ((void (*)(q2_monster *, s16))(void *)impl->callback[12])(&m, 100);
    check(m.dead && m.deadflag == Q2_DEAD_DEAD && m.next_think == 0 &&
          m.movetype == Q2_MOVETYPE_TOSS && m.takedamage == Q2_DAMAGE_YES &&
          (m.svflags & Q2_SVF_DEADMONSTER),
          "Flyer death immediately installs the native corpse state");
    q2_cre_set_sound_hook(NULL, NULL);
    q2_cre_set_melee_hook(NULL, NULL);
    q2_cre_set_muzzle_hook(NULL, NULL);
    q2_cre_set_shot_hook(NULL, NULL);
}

static void test_gunner_and_arachner_aim(void)
{
    const q2_cre_impl *gunner = q2_cre_impl_find("Gunner");
    const q2_cre_impl *arachner = q2_cre_impl_find("Arachner");
    q2_monster m, foe;

    printf("Gunner muzzle/lead and Arachner barrel target snapshots\n");
    q2_monster_init(&m);
    q2_monster_init(&foe);
    q2_cre_set_muzzle_hook(spy_muzzle, NULL);
    q2_cre_set_shot_hook(spy_last_shot, NULL);
    m.enemy = &foe;
    foe.pos[0] = 1100; foe.pos[1] = -100; foe.pos[2] = 300;
    foe.view_height = -100;
    foe.velocity[2] = 1000;

    gunner->method[2](&m);
    check(g_muzzle_a == 227 && g_muzzle_b == 231,
          "the chain gun uses its blended muzzle");
    check(g_last_shot.aimed && g_last_shot.start[0] == 100 &&
          g_last_shot.start[1] == -200 && g_last_shot.start[2] == 300,
          "the posed muzzle reaches the shot hook");
    check(g_last_shot.dir[0] > 3900 && g_last_shot.dir[1] == 0 &&
          g_last_shot.dir[2] < -700,
          "the shot includes the enemy's eye and negative velocity lead");

    m.enemy = NULL;
    gunner->method[1](&m);
    check(g_muzzle_a == 273 && g_muzzle_b == 273 && g_last_shot.aimed,
          "the grenade uses vertex 273 alone, even without an enemy");
    check(g_last_shot.dir[0] == 0 && g_last_shot.dir[1] == 0 &&
          g_last_shot.dir[2] == 4096 && g_last_shot.speed == 600,
          "grenades leave along body forward, with the module's speed");

    m.enemy = &foe;
    m.blind_target[0] = 100; m.blind_target[1] = -200;
    m.blind_target[2] = 2300;
    foe.pos[0] = 2100;
    arachner->method[3](&m);
    check(g_muzzle_a == 290 && g_muzzle_b == 307 && g_last_shot.aimed,
          "the right rail uses its own muzzle and initialized aim");
    check(g_last_shot.dir[0] == 0 && g_last_shot.dir[1] == 0 &&
          g_last_shot.dir[2] == 4096,
          "the first barrel fires at the earlier target, not its new position");
    check(m.blind_target[0] == 2100 && m.blind_target[1] == -200 &&
          m.blind_target[2] == 300,
          "the right barrel samples the enemy again only after firing");

    m.enemy = NULL;
    arachner->method[4](&m);
    check(g_muzzle_a == 239 && g_muzzle_b == 253 && g_last_shot.aimed,
          "the left rail uses its own muzzle after the enemy disappears");
    check(g_last_shot.dir[0] == 4096 && g_last_shot.dir[1] == 0 &&
          g_last_shot.dir[2] == 0,
          "the second barrel retains the right barrel's target snapshot");
    q2_cre_set_muzzle_hook(NULL, NULL);
    q2_cre_set_shot_hook(NULL, NULL);
}

static void test_soldier_burst_and_death_aim(void)
{
    const q2_cre_impl *impl = q2_cre_impl_find("Soldier");
    q2_monster m, foe;
    s32 fwd[3];
    s32 hold_until;
    int k;

    printf("the machinegun guard holds its fire frame; Death1 fires ahead\n");

    if (!impl || !impl->method[16] || !impl->method[20] || !impl->method[6]) {
        check(false, "the Soldier's fire thinks are registered");
        return;
    }
    q2_cre_set_shot_hook(spy_last_shot, NULL);

    /*
     * Attack4's frame 41 (think 16, flash 3) on a machinegun guard — class row
     * 20. module+0x14E8..+0x1540: pausetime = time + 3 + rand() % 8, only with
     * AI_HOLD_FRAME clear; module+0x159C..+0x15CC holds while time < pausetime.
     */
    q2_monster_init(&m);
    q2_monster_init(&foe);
    m.pop_class_id = 20;
    m.health = m.max_health = 40;
    m.angles[2] = 1024;                       /* facing +X */
    foe.pos[0] = 4000;
    foe.health = 100;
    m.enemy = &foe;
    q2_level_state.time = 100;

    g_shot_calls = 0;
    impl->method[16](&m);
    check_eq_i(g_shot_calls, 1, "frame 41 fires one round");
    check_eq_i(g_last_shot.slot, Q2_IMP_FIRE_BULLET,
               "through monster_fire_bullet, import +0x84");
    check(g_last_shot.aimed && g_last_shot.damage == 2,
          "aimed, damage 2 (module+0x1574)");
    check((m.aiflags & Q2_AI_HOLD_FRAME) != 0 &&
              m.pausetime >= 103 && m.pausetime <= 110,
          "and holds the frame for 3 + rand() % 8 ticks");

    hold_until = m.pausetime;
    q2_level_state.time = 101;
    impl->method[16](&m);
    check_eq_i(m.pausetime, hold_until,
               "a held frame does not rearm the pause (the AI_HOLD_FRAME test)");
    check((m.aiflags & Q2_AI_HOLD_FRAME) != 0, "and keeps holding");

    q2_level_state.time = hold_until;
    impl->method[16](&m);
    check_eq_i(g_shot_calls, 3, "every held tick is another round");
    check((m.aiflags & Q2_AI_HOLD_FRAME) == 0,
          "and the round fired at the deadline releases it");

    /*
     * Death1's frame 293, think 20, flash 5: `aim = forward`, at nobody —
     * module+0x1274's `addiu v0, s4, -5; sltiu v0, v0, 2`. A blaster guard
     * here, so the burst arm does not run.
     */
    q2_monster_init(&m);
    m.pop_class_id = 19;
    m.health = -5;
    m.dead = true;
    m.angles[2] = 1024;
    m.enemy = &foe;
    foe.pos[0] = -4000;                       /* BEHIND it */
    q2_angle_vectors(m.angles, fwd, NULL, NULL);
    impl->method[20](&m);
    check_eq_i(g_last_shot.slot, Q2_IMP_FIRE_BLASTER,
               "a blaster guard's dying shot is a bolt");
    for (k = 0; k < 3; k++)
        if (g_last_shot.dir[k] != fwd[k])
            break;
    check(k == 3, "and it goes straight along the body's own forward vector, "
                  "not at the enemy behind it");
    check(g_last_shot.start[0] == m.pos[0] && g_last_shot.start[2] == m.pos[2],
          "from the origin when nothing can pose the muzzle");

    /* An ordinary aimed flash goes AT the enemy, inside its crandom spread. */
    q2_monster_init(&m);
    m.pop_class_id = 19;
    m.health = 20;
    m.angles[2] = 1024;
    m.enemy = &foe;
    /* Not exactly 0: an X-aligned aim trips vectoangles' own quirk
     * (0x8005F998) and comes back a quarter turn off, on the console too. */
    foe.pos[0] = 37;
    foe.pos[2] = 6000;                        /* off to +Z */
    impl->method[6](&m);
    check(g_last_shot.aimed && g_last_shot.dir[2] > 3800,
          "flash 0 aims at the enemy within the module's spread");

    q2_cre_set_shot_hook(NULL, NULL);
    q2_level_state.time = 0;
}

static void test_infantry_death_sweep(void)
{
    const q2_cre_impl *impl = q2_cre_impl_find("Infantry");
    q2_monster m, foe;
    s32 fwd[3];
    s16 a[3];

    printf("the dying Infantry sweeps its burst up the wall\n");

    if (!impl || !impl->method[1]) {
        check(false, "InfantryMachineGun is registered");
        return;
    }
    q2_cre_set_shot_hook(spy_last_shot, NULL);

    q2_monster_init(&m);
    m.health = -10;
    m.dead = true;
    m.angles[2] = 2048;

    /* Frame 155: `angles - aimangles[0]`, (0, 0, 57) — five degrees across. */
    m.frame = 155;
    g_shot_calls = 0;
    impl->method[1](&m);
    a[0] = 0; a[1] = 0; a[2] = (s16)(2048 - 57);
    q2_angle_vectors(a, fwd, NULL, NULL);
    check_eq_i(g_shot_calls, 1, "frame 155 fires");
    check(g_last_shot.aimed && g_last_shot.dir[0] == fwd[0] &&
              g_last_shot.dir[1] == fwd[1] && g_last_shot.dir[2] == fwd[2],
          "along angles - aimangles[0]");

    /* Frame 166: aimangles[11] is (1024, 0, 398) — 90 degrees of pitch, so
     * straight up, and up is -Y on this console. */
    m.frame = 166;
    impl->method[1](&m);
    check(g_last_shot.dir[1] == -4096,
          "and frame 166 fires straight at the ceiling");

    /* A frame the table does not cover does not read off its end. */
    m.frame = 170;
    g_shot_calls = 0;
    impl->method[1](&m);
    check_eq_i(g_shot_calls, 0, "a frame past the table fires nothing");

    /* The attack frame, 194, leads the enemy by -819/4096 of its velocity. */
    q2_monster_init(&m);
    q2_monster_init(&foe);
    m.health = 100;
    m.frame = 194;
    m.enemy = &foe;
    foe.pos[2] = 8192;
    foe.velocity[0] = 4096 * 5;               /* moving +X */
    impl->method[1](&m);
    check(g_last_shot.dir[2] > 0 && g_last_shot.dir[0] < 0,
          "frame 194 aims where the enemy was, not where it is going");

    q2_cre_set_shot_hook(NULL, NULL);
}

/* ------------------------------------------------------------------------- */
/* The Tank's footstep shakes every pad — module+0x153C, import +0x12C        */
/* ------------------------------------------------------------------------- */
static int        g_step_log[4];     /* 1 = a shock, 2 = a sound, in order */
static int        g_step_n;
static int        g_step_player, g_step_id;
static s32        g_step_strength, g_step_pos[3];
static s16        g_step_near, g_step_far;
static bool       g_step_positional;

static void step_shock(void *u, int player, int id, s32 strength,
                       const s32 pos[3], s16 near_r, s16 far_r)
{
    (void)u;
    if (g_step_n < 4)
        g_step_log[g_step_n] = 1;
    g_step_n++;
    g_step_player = player; g_step_id = id; g_step_strength = strength;
    g_step_positional = pos != NULL;
    if (pos)
        memcpy(g_step_pos, pos, sizeof(g_step_pos));
    g_step_near = near_r; g_step_far = far_r;
}

static void step_sound(q2_monster *m, int which, void *u)
{
    (void)m; (void)u;
    if (g_step_n < 4)
        g_step_log[g_step_n] = (u32)which == 0x80102110u ? 2 : 3;
    g_step_n++;
}

static void test_tank_footstep_rumble(void)
{
    const q2_cre_impl *impl = q2_cre_impl_find("Tankcomm");
    q2_monster m;

    printf("the Tank's footstep is SetShockData, then tnk_step\n");

    if (!impl || !impl->method[6]) {
        check(false, "the Tank's think 6, tank_footstep");
        return;
    }

    q2_monster_init(&m);
    m.pos[0] = 1234; m.pos[1] = -567; m.pos[2] = 8910;

    g_step_n = 0;
    q2_shock_attach(step_shock, NULL);
    q2_cre_set_sound_hook(step_sound, NULL);
    impl->method[6](&m);
    q2_cre_set_sound_hook(NULL, NULL);
    q2_shock_attach(NULL, NULL);

    check_eq_i(g_step_n, 2, "one rumble and one sound");
    check(g_step_log[0] == 1 && g_step_log[1] == 2,
          "the rumble first (module+0x157C), then tnk_step (module+0x1598)");
    check_eq_i(g_step_player, 4, "a0 = 4: every window");
    check_eq_i(g_step_id, 14, "a1 = 14: ShockData_Stomp");
    check_eq_i(g_step_strength, 4096, "a2 = 4096");
    check(g_step_positional && g_step_pos[0] == 1234 &&
          g_step_pos[1] == -567 && g_step_pos[2] == 8910,
          "a3 = self, read as the Tank's origin");
    check(g_step_near == 2048 && g_step_far == 8192,
          "sp+16 = 2048 and sp+18 = 8192");
}

/* ------------------------------------------------------------------------- */
/* A prone Insane is a flyer — module+0x974, import +0x100                    */
/* ------------------------------------------------------------------------- */
static void test_insane_prone_flies(void)
{
    const q2_cre_impl *impl = q2_cre_impl_find("Insane");
    q2_monster m;

    printf("a prone Insane is a flyer for life\n");

    if (!impl || !impl->spawn) {
        check(false, "the Insane has a spawn hook");
        return;
    }

    q2_cre_bind_reset();
    q2_level_reset();

    /*
     * Map flag 8 — bit 3 of the record's flags, spawnflags bit 21 after the
     * filler's `<< 18` — is the prone variant. Its arm ORs FL_NO_KNOCKBACK
     * and then calls import +0x100, flymonster_start, which raises FL_FLY
     * itself (0x800622B4). Before, the port set the knockback bit alone and
     * the crawler walked the stepping path with a walker's eye.
     */
    q2_monster_init(&m);
    m.spawnflags = 0x08u << 18;
    impl->spawn(&m);
    check((m.flags & (Q2_FL_NO_KNOCKBACK | Q2_FL_FLY)) ==
              (Q2_FL_NO_KNOCKBACK | Q2_FL_FLY),
          "a prone Insane takes no knockback AND is FL_FLY from spawn");
    check_eq_i(m.start_kind, Q2_CRE_START_FLY, "with the fly go-routine parked");

    q2_monster_start_go(&m);
    check_eq_i(m.view_height, -250, "so its eye is the flyer's flat -250");
    check_eq_i(m.yaw_speed, 114, "and it turns at the flyer's 114");
    check((m.flags & Q2_FL_FLY) != 0, "and is still flying once awake");

    /* The upright one calls +0xFC, walkmonster_start, and gets its model's
     * eye: ext2 304 on LAB is ~304 = -305. */
    q2_monster_init(&m);
    m.spawnflags = 0;
    impl->spawn(&m);
    m.model_ext2 = 304;
    q2_monster_start_go(&m);
    check(m.view_height == -305 && (m.flags & Q2_FL_FLY) == 0,
          "an upright Insane walks, with its eye from its model");
    check_eq_i(m.yaw_speed, 228, "and turns at the walker's 228");
}

/* ------------------------------------------------------------------------- */
/* The Insane's draws, and its place in the monster count                     */
/* ------------------------------------------------------------------------- */
/*
 * An Insane-shaped creature carrying just "Stand N", module+0x1388 {59, 64} —
 * the move its spawn installs at 0x80100960 (`sw v0, 216(s0)`) before the
 * prone/upright fork — and the Insane's own class byte, 94, so that its spawn
 * hook finds the move through the bind.
 */
static q2_creature g_insane;

static void build_insane(void)
{
    u32 i;

    memset(&g_insane, 0, sizeof(g_insane));
    memcpy(g_insane.name, "Insane", 7);
    g_insane.base          = TEST_MOD_BASE;
    g_insane.class_byte[0] = 94;
    g_insane.class_count   = 1;

    g_insane.move[0].addr        = 0x80101388u;
    g_insane.move[0].first_frame = 59;
    g_insane.move[0].last_frame  = 64;
    g_insane.move[0].frame_index = 0;
    g_insane.move[0].frame_count = 6;
    g_insane.move_count          = 1;

    for (i = 0; i < 6; i++)
        g_insane.frames[i].ai = Q2_AI_STAND;
    g_insane.frame_count = 6;
}

static void test_insane_start_draws(void)
{
    const q2_cre_impl *impl = q2_cre_impl_find("Insane");
    q2_cre_bind bind, beast;
    q2_monster m, t;
    int r1, r2, r3, next;

    printf("the Insane's start call draws, and it is not counted\n");

    if (!impl || !impl->spawn) {
        check(false, "the Insane has a spawn hook");
        return;
    }

    build_insane();
    build_creature();
    q2_cre_bind_reset();
    q2_class_table_reset();
    if (!q2_creature_bind(&bind, &g_insane, impl) ||
        !q2_creature_bind(&beast, &g_cre, &g_impl)) {
        check(false, "the Insane and Testbeast binds could not be made");
        return;
    }
    q2_level_reset();

    /* The stream the spawn will read, taken out in advance. */
    q2_random_seed(4321);
    r1 = q2_random_next();
    r2 = q2_random_next();
    r3 = q2_random_next();

    /*
     * PRONE: flymonster_start through import +0x100 (0x80100994), whose
     * monster_start draws at 0x80061B2C for the start frame — and then
     * 0x8010099C jumps past the skin. ONE draw, and the frame it makes.
     */
    q2_random_seed(4321);
    q2_monster_init(&m);
    m.spawnflags = 0x08u << 18;
    q2_creature_spawn(&bind, &m, 0);
    next = q2_random_next();
    check(r1 != r2 && next == r2,
          "a prone Insane's spawn draws exactly once: the start frame inside "
          "flymonster_start (0x800622B8 -> 0x80061B2C)");
    check_eq_i(m.frame, 59 + (r1 & 0x7FFF) % 6,
               "and that draw is Stand N's start frame, 59 + r % 6 "
               "(0x80061B8C)");

    /*
     * UPRIGHT: walkmonster_start (0x801009AC) draws the start frame first,
     * THEN the module draws the skin at 0x801009BC. Two draws, in that order.
     */
    q2_random_seed(4321);
    q2_monster_init(&m);
    m.spawnflags = 0;
    q2_creature_spawn(&bind, &m, 0);
    next = q2_random_next();
    check(r2 != r3 && next == r3,
          "an upright Insane's spawn draws exactly twice");
    check(m.frame == 59 + (r1 & 0x7FFF) % 6 &&
              m.skinnum == (u8)((r2 & 0x7FFF) % 3),
          "the start frame first, then the skin — `rand() % 3` on the SECOND "
          "draw, as 0x801009B4 follows the wrapper call");

    /*
     * AND NEITHER IS COUNTED. The Insane raises AI_GOOD_GUY at 0x80100904,
     * before either start call, so monster_start's 0x80061A50 `andi v0, v0,
     * 0x100` skips the total_monsters increment. The Testbeast, which sets no
     * such bit, is the control: the count is live, it just leaves these out.
     */
    q2_monster_init(&t);
    q2_creature_spawn(&beast, &t, 0);
    check_eq_i(q2_level_state.total_monsters, 1,
               "two Insanes and one Testbeast make a level total of one");

    q2_cre_bind_reset();
    q2_class_table_reset();
}

/* Optional real-disc integration: bind every supported population class on
 * every difficulty, drive its decoded attack and death moves, and verify
 * every frame method is reachable. Assets stay on the user's disc. */
static void roster_shot(q2_monster *m, const q2_cre_shot *shot, void *user)
{
    (void)m; (void)user;
    g_shot_calls++;
    check(shot->aimed && shot->damage > 0,
          "every disc-driven shot has a muzzle, direction and damage");
}

static void test_disc_flyer(q2_cre_bind *bind)
{
    q2_monster m, foe;
    const q2_cre_impl *impl = bind->impl;
    const s32 pains[3] = { 134, 143, 147 };
    int i, seed, first, next;
    q2_level_reset();
    q2_monster_init(&m);
    q2_monster_init(&foe);
    m.health = m.max_health = 50;
    q2_creature_spawn(bind, &m, 0);
    q2_monster_start_go(&m);
    check((m.flags & Q2_FL_FLY) && m.mass == 50 && m.view_height == -250,
          "the disc Flyer starts through flymonster_start");
    m.walk(&m);
    check(m.currentmove == q2_cre_find_move_at(&m, 0x80101484u),
          "Walk selects the walking copy of frames 13..57");
    m.aiflags &= ~Q2_AI_STAND_GROUND;
    m.run(&m);
    check(m.currentmove == q2_cre_find_move_at(&m, 0x8010151Cu),
          "Run selects the running copy of the same frames");
    m.aiflags |= Q2_AI_STAND_GROUND;
    m.run(&m);
    check(m.currentmove == q2_cre_find_move_at(&m, 0x801013ECu),
          "stand-ground Run selects the stationary copy");
    m.aiflags = 0;
    m.enemy = m.goalentity = &foe;
    foe.health = 10000;
    foe.in_use = foe.client = true;
    foe.spawnflags |= Q2_SVFLAG_INUSE;
    foe.pos[2] = 5000;
    m.attack(&m);
    m.frame = 78;
    g_shot_calls = 0;
    for (i = 0; i < 17; i++) q2_M_MoveFrame(&m);
    check(g_shot_calls == 8 && m.frame == 95,
          "the real disc attack frames fire exactly eight shots");
    check(m.currentmove->endfunc != NULL, "Attack2 binds its native run endfunc");
    m.currentmove->endfunc(&m);
    check(m.currentmove == q2_cre_find_move_at(&m, 0x8010151Cu),
          "Attack2 ends by running, not the decoder's first possible branch");
    m.melee(&m);
    check(m.currentmove->endfunc != NULL, "melee start binds its loop installer");
    m.currentmove->endfunc(&m);
    check(m.currentmove->first_frame == 64 && m.currentmove->endfunc != NULL,
          "melee chains into its native loop with a decision endfunc");
    q2_random_seed(987);
    first = q2_random_next();
    q2_random_seed(987);
    impl->method[1](&m);
    check(m.currentmove->first_frame == 76 && q2_random_next() == first,
          "a distant enemy ends melee without drawing the repeat chance");
    foe.pos[0] = m.pos[0]; foe.pos[1] = m.pos[1]; foe.pos[2] = m.pos[2];
    for (seed = 0; seed < 100; seed++) {
        q2_random_seed((u32)seed);
        first = q2_random_next(); next = q2_random_next();
        q2_random_seed((u32)seed);
        impl->method[1](&m);
        check(m.currentmove->first_frame == (first < 26214 ? 64 : 76) &&
              q2_random_next() == next,
              "near melee repeats at the native threshold with one draw");
    }
    for (i = 0; i < 3; i++) {
        for (seed = 0; ; seed++) {
            q2_random_seed((u32)seed);
            if (q2_random_next() % 3 == i) break;
        }
        q2_cre_set_skill(1);
        q2_level_state.time = 100;
        m.pain_debounce = 0;
        m.health = 24;
        q2_random_seed((u32)seed);
        m.pain(&m, 1);
        check(m.currentmove->first_frame == pains[i] && m.skinnum == 1 &&
              m.pain_debounce == 130, "each native pain branch and deadline works");
    }
    m.attack(&m);
    m.skinnum = 0;
    m.pain_debounce = 0;
    q2_cre_set_skill(3);
    q2_random_seed(987);
    first = q2_random_next();
    q2_random_seed(987);
    m.pain(&m, 40);
    check(m.currentmove->first_frame == 79 && m.skinnum == 1 &&
          m.pain_debounce == 130 && q2_random_next() == first,
          "nightmare changes skin/debounce without interrupting or drawing RNG");
    impl->method[4](&m);
    check(m.currentmove->first_frame == 0 && bind->flyer_transition == 3,
          "registered start writes the shared module transition selector");
    impl->method[6](&m);
    check(m.currentmove == q2_cre_find_move_at(&m, 0x8010151Cu),
          "Start's final frame dispatches to Run");
    q2_cre_set_skill(1);
}

static void test_disc_gladiator(q2_creature_module *mod)
{
    q2_cre_bind *bind = &mod->bind;
    const q2_cre_impl *impl = bind->impl;
    q2_monster m, foe;
    q2_cre_sound_bind sounds[48];
    u32 count, i;
    int first, second;
    static const q2_cre_sound_bind expected[] = {
        { 0x801013D0u, "gld_pain2" }, { 0x801013D4u, "gld_pain2" },
        { 0x801013D8u, "gld_death" }, { 0x801013DCu, "wep_railgf1a" },
        { 0x801013E0u, "gld_melee3" }, { 0x801013ECu, "gld_idle1" },
        { 0x801013F0u, "gld_srch1" }, { 0x801013F4u, "gld_sight" },
        { 0x801013F8u, "msc_udeath" }
    };
    count = q2_creature_sound_bindings(mod->image, mod->size, Q2_CREWORLD_BASE,
                                        sounds, 48);
    check(count <= 48, "Gladiator sound registrations fit the binding table");
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        const char *name = q2_creature_sound_for_addr(sounds, count,
                                                      expected[i].addr);
        check(name && !strcmp(name, expected[i].name),
              "every Gladiator handle resolves, including both scheduled pain calls");
    }
    q2_level_reset();
    q2_monster_init(&m); q2_monster_init(&foe);
    m.health = m.max_health = 400; m.gib_health = -175;
    q2_creature_spawn(bind, &m, 0);
    q2_monster_start_go(&m);
    check(m.mass == 400 && m.speed_scale == 10 && !m.dodge &&
          m.movetype == Q2_MOVETYPE_STEP && m.solid == Q2_SOLID_BBOX,
          "Gladiator spawn installs the native physical state");
    m.enemy = m.goalentity = &foe;
    foe.health = 1000; foe.in_use = true;
    m.blind_target[0] = 123;
    foe.pos[2] = 1052;
    m.attack(&m);
    check(m.currentmove->first_frame == 0 && m.blind_target[0] == 123,
          "the exact rail range boundary preserves the previous move and target");
    foe.pos[2] = 1053; foe.view_height = -200;
    m.attack(&m);
    check(m.currentmove->first_frame == 46 && m.blind_target[0] == 0 &&
          m.blind_target[1] == -200 && m.blind_target[2] == 1053,
          "a farther target starts the rail move and snapshots its eye");
    q2_cre_set_shot_hook(spy_last_shot, NULL);
    q2_cre_set_muzzle_hook(spy_muzzle, NULL);
    q2_cre_set_sound_hook(spy_sound, NULL);
    m.enemy = NULL; foe.pos[0] = 10000;
    impl->method[4](&m);
    check(g_muzzle_a == 158 && g_muzzle_b == 169 &&
          g_last_shot.slot == Q2_IMP_FIRE_RAILGUN &&
          g_last_shot.damage == 50 && g_last_shot.kick == 100 &&
          g_last_shot.flash == 0 && g_last_shot.aimed,
          "the rail uses the posed barrel and native weapon parameters");
    check(g_last_shot.dir[0] < 0 && g_last_shot.dir[1] == 0 &&
          g_last_shot.dir[2] > 4000 && (u32)g_snd_last == 0x801013DCu,
          "the shot uses its saved eye after enemy loss and plays the rail sound");
    m.enemy = &foe; foe.pos[0] = foe.pos[2] = 0; m.mins[0] = -99;
    q2_random_seed(123); first = q2_random_next(); second = q2_random_next();
    q2_random_seed(123); g_mel_calls = 0;
    impl->method[2](&m);
    check(g_mel_calls == 1 && g_mel_dmg == 20 + first % 5 &&
          g_mel_kick == 150 && g_mel_aim[0] == 1020 &&
          g_mel_aim[1] == -48 && g_mel_aim[2] == -99 &&
          q2_random_next() == second, "melee uses native damage, kick and hull aim");
    foe.pos[2] = 3000; q2_random_seed(123);
    impl->method[2](&m);
    check(g_mel_calls == 1 && q2_random_next() == second,
          "a missed melee still draws its damage roll");
    q2_cre_set_skill(1); q2_level_state.time = 100;
    m.health = 199; m.velocity[1] = 1200; g_snd_calls = 0;
    q2_random_seed(123);
    m.pain(&m, 1);
    check(m.currentmove->first_frame == 55 && m.skinnum == 1 &&
          m.pain_debounce == 130 && g_snd_calls == 1 &&
          q2_random_next() == first, "ordinary pain at velocity 1200 consumes no RNG");
    m.velocity[1] = 1201;
    m.pain(&m, 1);
    check(m.currentmove->first_frame == 83 && m.pain_debounce == 130 &&
          g_snd_calls == 1, "debounce allows only normal pain to become upward pain");
    m.attack(&m); m.pain(&m, 1);
    check(m.currentmove->first_frame == 46 && g_snd_calls == 1,
          "debounce does not interrupt an attack with upward pain");
    q2_level_state.time = 130; q2_cre_set_skill(3);
    m.pain(&m, 1);
    check(m.currentmove->first_frame == 46 && g_snd_calls == 2 &&
          m.pain_debounce == 160, "nightmare pain plays sound and sets debounce");
    q2_cre_set_skill(1); q2_level_state.time = 160;
    m.pain(&m, 1);
    check(m.currentmove->first_frame == 83, "fresh pain also honors vertical velocity");
    m.health = 0; m.die(&m, 10);
    check(m.currentmove->first_frame == 61 && m.dead &&
          m.takedamage == Q2_DAMAGE_YES, "normal death starts the full animation");
    first = g_snd_calls; m.die(&m, 10);
    check(g_snd_calls == first, "repeated corpse damage does not replay death");
    m.currentmove->endfunc(&m);
    check(m.next_think == 0 && m.movetype == Q2_MOVETYPE_TOSS &&
          (m.svflags & Q2_SVF_DEADMONSTER), "death ends in the native corpse state");
    m.health = m.gib_health; m.takedamage = Q2_DAMAGE_AIM;
    m.die(&m, 200);
    check(g_snd_calls == first && m.takedamage == Q2_DAMAGE_AIM,
          "gib threshold is silent and preserves the damage mode");
    q2_cre_set_sound_hook(NULL, NULL);
    q2_cre_set_muzzle_hook(NULL, NULL);
    q2_cre_set_shot_hook(roster_shot, NULL);
}

static bool blocked_creature_sight(void *user, const s32 a[3], const s32 b[3])
{
    (void)user; (void)a; (void)b;
    return false;
}

static void test_disc_hover(q2_creature_module *mod)
{
    q2_monster m, foe;
    const q2_cre_impl *impl = mod->bind.impl;
    q2_cre_sound_bind sounds[48];
    q2_ai_world blocked = { 0 };
    u32 count;
    const char *name;
    int seed, first, second, i;
    count = q2_creature_sound_bindings(mod->image, mod->size, Q2_CREWORLD_BASE,
                                        sounds, 48);
    check(count == 8, "Hover registers five samples and copies three additional handle slots");
    name = q2_creature_sound_for_addr(sounds, count, 0x8010147Cu);
    check(name && !strcmp(name, "msc_udeath"), "Hover's gib sound resolves");
    q2_level_reset(); q2_monster_init(&m); q2_monster_init(&foe);
    m.health = m.max_health = 240; m.gib_health = -100;
    q2_creature_spawn(&mod->bind, &m, 0); q2_monster_start_go(&m);
    check((m.flags & Q2_FL_FLY) && m.mass == 150, "Hover starts through flymonster_start");
    m.walk(&m);
    check(m.currentmove == q2_cre_find_move_at(&m, 0x801012F4u),
          "walk selects the walking copy of Forward");
    m.run(&m);
    check(m.currentmove == q2_cre_find_move_at(&m, 0x80101370u),
          "run selects the running copy of Forward");
    m.aiflags |= Q2_AI_STAND_GROUND; m.run(&m);
    check(m.currentmove->first_frame == 0, "stand-ground run stands");
    m.enemy = m.goalentity = &foe;
    foe.health = 100; foe.in_use = foe.client = true;
    foe.spawnflags |= Q2_SVFLAG_INUSE;
    foe.pos[0] = 164; foe.pos[1] = -200; foe.pos[2] = 4000;
    foe.view_height = -64;
    q2_cre_set_shot_hook(spy_last_shot, NULL);
    q2_cre_set_muzzle_hook(spy_muzzle, NULL);
    m.attack(&m); m.frame = 196; g_shot_calls = 0;
    for (i = 0; i < 5; i++) q2_M_MoveFrame(&m);
    check(g_shot_calls == 2 && m.frame == 201,
          "start attack chains into exactly two shot frames");
    check(g_muzzle_a == 275 && g_muzzle_b == 277 && g_last_shot.effects == 0,
          "odd attack frame uses the first muzzle and ordinary bolt");
    m.frame = 200; impl->method[6](&m);
    check(g_muzzle_a == 267 && g_muzzle_b == 269 && g_last_shot.effects == 64 &&
          g_last_shot.dir[0] == 64 && g_last_shot.dir[1] == -64 &&
          g_last_shot.dir[2] == 3700 && g_last_shot.damage == 1,
          "frame 200 uses the second muzzle and raw eye delta for a hyper bolt");
    for (seed = 0; seed < 100; seed++) {
        q2_random_seed((u32)seed); first = q2_random_next(); second = q2_random_next();
        q2_random_seed((u32)seed); impl->method[5](&m);
        check(m.currentmove->first_frame == (first < 19661 ? 200 : 203) &&
              q2_random_next() == second, "visible living target repeats at native 60% threshold");
    }
    foe.health = 0; q2_random_seed(123); first = q2_random_next();
    q2_random_seed(123); impl->method[5](&m);
    check(m.currentmove->first_frame == 203 && q2_random_next() == first,
          "a dead target ends the burst before drawing RNG");
    foe.health = 100; blocked.line_of_sight = blocked_creature_sight;
    q2_ai_set_world(&blocked); q2_random_seed(123); impl->method[5](&m);
    check(m.currentmove->first_frame == 203 && q2_random_next() == first,
          "lost visibility also ends the burst without RNG");
    q2_ai_set_world(NULL); q2_cre_set_sound_hook(spy_sound, NULL);
    q2_cre_set_skill(1); m.health = 119;
    for (seed = 0; seed < 32; seed++) {
        q2_random_seed((u32)seed); first = q2_random_next(); second = q2_random_next();
        q2_random_seed((u32)seed); m.pain_debounce = 0; m.pain(&m, 25);
        check(m.currentmove->first_frame == (first < 16384 ? 153 : 141) &&
              m.skinnum == 1 && q2_random_next() == second,
              "damage 25 selects between the two short pain moves");
    }
    q2_random_seed(123); first = q2_random_next(); q2_random_seed(123);
    m.pain_debounce = 0; m.pain(&m, 26);
    check(m.currentmove->first_frame == 113 && q2_random_next() == first,
          "damage 26 selects long pain without a random draw");
    m.attack(&m); m.pain_debounce = 0; g_snd_calls = 0;
    q2_cre_set_skill(3); q2_random_seed(123); m.pain(&m, 50);
    check(m.currentmove->first_frame == 197 && !g_snd_calls &&
          m.pain_debounce == 30 && q2_random_next() == first,
          "nightmare sets debounce but has no pain sound or animation");
    m.health = 0; m.die(&m, 10);
    check(m.currentmove->first_frame == 162 && m.dead &&
          m.takedamage == Q2_DAMAGE_YES, "ordinary Hover death plays its full move");
    m.currentmove->endfunc(&m);
    check(m.health == m.gib_health && m.takedamage == Q2_DAMAGE_NO &&
          (m.svflags & Q2_SVF_DEADMONSTER) && m.next_think == 0,
          "completed death sets gib health and disables damage for corpse dispatch");
    m.health = -300; m.die(&m, 200);
    check(m.health == m.gib_health && (u32)g_snd_last == 0x8010147Cu,
          "immediate gib death clamps to gib health and plays its own sound");
    q2_cre_set_skill(1); q2_cre_set_sound_hook(NULL, NULL);
    q2_cre_set_muzzle_hook(NULL, NULL); q2_cre_set_shot_hook(roster_shot, NULL);
}

static q2_ai_world g_parasite_base;
static q2_monster *g_parasite_target;
static int g_parasite_hits, g_parasite_damage, g_parasite_mod, g_parasite_beams;
static void parasite_trace(void *user, const s32 start[3], const s16 mins[3],
                           const s16 maxs[3], const s32 end[3],
                           const q2_monster *ignore, u32 mask, q2_ai_trace *out)
{
    (void)user;
    g_parasite_base.trace(g_parasite_base.user, start, mins, maxs, end, ignore, mask, out);
    if (!mins && !maxs) {
        check(mask == 0x06000003u, "Parasite drain uses the native trace mask");
        out->ent = g_parasite_target;
    }
}
static void parasite_hurt(q2_monster *self, q2_monster *target, s16 damage,
                          int mod, const s32 *point, void *user)
{
    (void)user;
    check(target == self->enemy && point == NULL,
          "drain damages the traced enemy with no damage point");
    g_parasite_hits++; g_parasite_damage += damage; g_parasite_mod = mod;
}
static void parasite_beam(q2_monster *self, q2_monster *target, s16 vertex, void *user)
{
    (void)user;
    check(target == self->enemy && vertex == 97, "drain beam uses posed vertex 97");
    g_parasite_beams++;
}
static void parasite_test_world(q2_monster *target)
{
    q2_ai_world world;
    q2_ai_set_world(NULL);
    g_parasite_base = *q2_ai_get_world();
    world = g_parasite_base; world.trace = parasite_trace;
    g_parasite_target = target;
    q2_ai_set_world(&world);
    q2_cre_set_contact_hooks(parasite_hurt, parasite_beam, NULL);
}

static void test_disc_parasite(q2_cre_bind *bind)
{
    q2_monster m, foe;
    int i, seed, draw, next;
    q2_level_reset(); q2_cre_set_skill(1);
    q2_monster_init(&m); q2_monster_init(&foe);
    m.health = m.max_health = 175; m.gib_health = -50;
    q2_creature_spawn(bind, &m, 0);
    check(m.mass == 250 && m.start_kind == Q2_CRE_START_WALK,
          "Parasite spawn enters the native walking state");
    m.run(&m); check(m.currentmove == q2_cre_find_move_at(&m, 0x8010168Cu),
                    "run starts with the run-specific duplicate frame range");
    bind->impl->method[3](&m);
    check(m.currentmove == q2_cre_find_move_at(&m, 0x80101674u), "run loops its own move");
    m.walk(&m); check(m.currentmove == q2_cre_find_move_at(&m, 0x801016F0u),
                     "walk starts with its distinct duplicate frame range");
    bind->impl->method[4](&m);
    check(m.currentmove == q2_cre_find_move_at(&m, 0x801016D8u), "walk loops its own move");
    m.aiflags |= Q2_AI_STAND_GROUND;
    m.run(&m); check(m.currentmove->first_frame == 83, "stand-ground keeps Parasite still");
    m.enemy = m.goalentity = &foe; foe.health = 10000; foe.pos[2] = 1000;
    parasite_test_world(&foe);
    q2_cre_set_sound_hook(spy_sound, NULL);
    g_parasite_hits = g_parasite_damage = g_parasite_beams = 0;
    m.attack(&m); m.frame = 38;
    for (i = 0; i < 18; i++) q2_M_MoveFrame(&m);
    check(g_parasite_hits == 11 && g_parasite_damage == 25 && g_parasite_beams == 11,
          "the native drain move delivers one five-point and ten two-point hits");
    g_parasite_hits = g_parasite_damage = 0;
    m.frame = 41; bind->impl->method[12](&m);
    check(g_parasite_hits == 1 && g_parasite_mod == 21, "first contact carries MOD 21");
    m.frame = 42; bind->impl->method[12](&m);
    check(g_parasite_hits == 2 && g_parasite_mod == 0, "later contact carries MOD 0");
    g_parasite_target = NULL;
    bind->impl->method[12](&m);
    check(g_parasite_hits == 2, "a world or other-body obstruction refuses drain damage");
    g_parasite_target = &foe;
    memcpy(foe.pos, m.pos, sizeof(m.pos)); foe.pos[2] += 3360;
    bind->impl->method[12](&m);
    check(g_parasite_hits == 3, "3072 from the projected start is still in reach");
    foe.pos[2]++;
    bind->impl->method[12](&m);
    check(g_parasite_hits == 3, "3073 from the projected start is out of reach");
    for (i = 0; i < 2; i++) {
        for (seed = 0; ; seed++) {
            q2_random_seed((u32)seed); draw = q2_random_next();
            if ((draw < 26215) == i) break;
        }
        next = q2_random_next(); q2_random_seed((u32)seed);
        bind->impl->method[8](&m);
        check(m.currentmove->first_frame == (i ? 104 : 110) && q2_random_next() == next,
              "fidget repetition uses one native 26215-threshold random draw");
    }
    q2_cre_set_skill(3); m.health = 10; m.pain_debounce = 0;
    g_snd_calls = 0; m.pain(&m, 1);
    check(m.skinnum == 1 && m.pain_debounce == 30 && g_snd_calls == 0,
          "nightmare still updates skin/debounce but skips pain sound and move");
    q2_cre_set_skill(1); q2_level_state.time = 30; m.pain(&m, 100);
    check(m.currentmove->first_frame == 57 && g_snd_calls == 1,
          "pain at the debounce boundary enters the native flinch");
    m.health = -1; m.die(&m, 10);
    check(m.dead && m.currentmove->first_frame == 32 && m.takedamage == Q2_DAMAGE_YES,
          "ordinary death retains damage and the seven-frame move");
    for (i = 0; i < 8; i++) q2_M_MoveFrame(&m);
    check(m.next_think == 0 && m.movetype == Q2_MOVETYPE_TOSS &&
          (m.svflags & Q2_SVF_DEADMONSTER), "Parasite death reaches its corpse state");
    q2_cre_set_contact_hooks(NULL, NULL, NULL);
    q2_cre_set_sound_hook(NULL, NULL); q2_ai_set_world(NULL);
}

static void test_disc_ironmaiden(q2_creature_module *mod)
{
    q2_monster m, foe;
    const q2_cre_impl *impl = mod->bind.impl;
    q2_cre_sound_bind sounds[48];
    q2_ai_world blocked = { 0 };
    u32 count, i;
    int seed, first, next;
    static const q2_cre_sound_bind expected[] = {
        { 0x80101B60u, "chk_atck1" }, { 0x80101B64u, "wep_rocklf1a" },
        { 0x80101B68u, "ara_melee1" }, { 0x80101B6Cu, "chk_atck4" },
        { 0x80101B78u, "chk_deth2" }, { 0x80101B84u, "chk_idle2" },
        { 0x80101B90u, "chk_pain3" }, { 0x80101B94u, "chk_sght1" },
        { 0x80101B9Cu, "msc_udeath" }
    };
    count = q2_creature_sound_bindings(mod->image, mod->size, Q2_CREWORLD_BASE,
                                        sounds, 48);
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        const char *name = q2_creature_sound_resolve("Ironmaiden", sounds, count,
                             Q2_CREWORLD_BASE, expected[i].addr, NULL, NULL);
        check(name && !strcmp(name, expected[i].name),
              "Ironmaiden resolves scheduled stores and the overwritten sight handle");
    }
    q2_level_reset(); q2_monster_init(&m); q2_monster_init(&foe);
    m.health = m.max_health = 175; m.gib_health = -70;
    q2_creature_spawn(&mod->bind, &m, 0); q2_monster_start_go(&m);
    check(m.mass == 200 && m.speed_scale == 10 && m.dodge &&
          m.currentmove->first_frame == 121, "Ironmaiden starts with native physical state");
    m.run(&m);
    check(m.currentmove->first_frame == 181, "stand enters the run start move");
    m.run(&m);
    check(m.currentmove == q2_cre_find_move_at(&m, 0x80101790u),
          "run start switches to the running copy of Walk");
    m.run(&m);
    check(m.currentmove->first_frame == 181, "calling run while running restarts the lead-in");
    m.walk(&m); m.run(&m);
    check(m.currentmove == q2_cre_find_move_at(&m, 0x80101790u),
          "walking enters the run loop directly");
    m.aiflags |= Q2_AI_STAND_GROUND; m.run(&m);
    check(m.currentmove->first_frame == 121, "stand-ground run stands");
    m.aiflags &= ~Q2_AI_STAND_GROUND;
    m.enemy = m.goalentity = &foe;
    foe.health = 100; foe.in_use = foe.client = true;
    foe.spawnflags |= Q2_SVFLAG_INUSE; foe.pos[2] = 4000;
    q2_cre_set_shot_hook(spy_last_shot, NULL);
    q2_cre_set_muzzle_hook(spy_muzzle, NULL);
    q2_cre_set_sound_hook(spy_sound, NULL);
    m.attack(&m); m.frame = -1; g_shot_calls = 0;
    for (i = 0; i < 14; i++) q2_M_MoveFrame(&m);
    check(m.frame == 13 && g_shot_calls == 1 && g_muzzle_a == 233 &&
          g_muzzle_b == 240 && g_last_shot.slot == Q2_IMP_FIRE_ROCKET &&
          g_last_shot.damage == 50 && g_last_shot.speed == 500 &&
          g_last_shot.aimed && g_last_shot.flash == 0,
          "the real attack lead-in launches one native rocket at frame 13");
    for (seed = 0; seed < 100; seed++) {
        q2_random_seed((u32)seed); first = q2_random_next(); next = q2_random_next();
        q2_random_seed((u32)seed); impl->method[4](&m);
        check(m.currentmove->first_frame == (first < 19961 ? 13 : 27) &&
              q2_random_next() == next, "rocket repeat uses the disc's 19961 threshold");
        foe.pos[2] = 0;
        q2_random_seed((u32)seed); impl->method[3](&m);
        check(m.currentmove->first_frame == (first < 29491 ? 35 : 44) &&
              q2_random_next() == next, "close melee repeats at the disc's 90% threshold");
        foe.pos[2] = 4000;
    }
    q2_random_seed(123); first = q2_random_next(); q2_random_seed(123);
    blocked.line_of_sight = blocked_creature_sight; q2_ai_set_world(&blocked);
    impl->method[4](&m);
    check(m.currentmove->first_frame == 27 && q2_random_next() == first,
          "obstruction ends the rocket loop without consuming its random draw");
    q2_ai_set_world(NULL);
    foe.pos[2] = 0; m.mins[0] = -99; g_mel_calls = 0;
    q2_random_seed(123); first = q2_random_next(); next = q2_random_next();
    q2_random_seed(123); impl->method[16](&m);
    check(g_mel_calls == 1 && g_mel_dmg == 10 + first % 6 &&
          g_mel_kick == 100 && g_mel_aim[0] == 1020 &&
          g_mel_aim[1] == -120 && g_mel_aim[2] == 99 &&
          q2_random_next() == next && (u32)g_snd_last == 0x80101B68u,
          "slash uses the negated hull extent, native damage and contact sound");
    for (seed = 0; ; seed++) {
        q2_random_seed((u32)seed);
        if (q2_random_next() < 8192) break;
    }
    m.enemy = NULL; q2_random_seed((u32)seed); m.dodge(&m, &foe, 999);
    check(m.currentmove->first_frame == 83 && m.enemy == &foe,
          "accepted dodge adopts the attacker and starts Duck");
    q2_level_state.time = 100; impl->method[9](&m);
    check((m.aiflags & Q2_AI_DUCKED) && m.pausetime == 110 &&
          m.takedamage == Q2_DAMAGE_YES, "duck starts its ten-tick hold");
    q2_level_state.time = 109; impl->method[9](&m); impl->method[10](&m);
    check(m.pausetime == 110 && (m.aiflags & Q2_AI_HOLD_FRAME),
          "repeated duck-down does not extend the deadline");
    q2_level_state.time = 110; impl->method[10](&m); impl->method[11](&m);
    check(!(m.aiflags & (Q2_AI_DUCKED | Q2_AI_HOLD_FRAME)) &&
          m.takedamage == Q2_DAMAGE_AIM, "duck releases exactly at its deadline");
    q2_cre_set_skill(1); m.health = 80;
    for (i = 0; i < 3; i++) {
        static const s16 damage[] = { 10, 25, 26 };
        static const s16 move[] = { 90, 95, 100 };
        m.pain_debounce = 0; q2_random_seed(123); m.pain(&m, damage[i]);
        check(m.currentmove->first_frame == move[i] && m.skinnum == 1 &&
              m.pain_debounce == 140 && q2_random_next() == first,
              "pain boundaries choose the three moves without random draws");
    }
    q2_cre_set_skill(3); m.attack(&m); m.pain_debounce = 0; g_snd_calls = 0;
    m.pain(&m, 26);
    check(m.currentmove->first_frame == 0 && g_snd_calls == 1 &&
          m.pain_debounce == 140, "nightmare retains the pain sound and deadline");
    m.health = 0; q2_random_seed(123); m.die(&m, 10);
    check(m.currentmove->first_frame == ((first & 1) ? 60 : 48) &&
          m.takedamage == Q2_DAMAGE_YES, "death picks one of the two native moves");
    m.currentmove->endfunc(&m);
    check(m.next_think == 0 && m.movetype == Q2_MOVETYPE_TOSS &&
          (m.svflags & Q2_SVF_DEADMONSTER), "death ends as a tossed corpse");
    m.health = m.gib_health; m.die(&m, 100);
    check((u32)g_snd_last == 0x80101B9Cu, "gib death resolves its delayed init store");
    q2_cre_set_skill(1); q2_cre_set_sound_hook(NULL, NULL);
    q2_cre_set_muzzle_hook(NULL, NULL); q2_cre_set_shot_hook(roster_shot, NULL);
}

static int g_explosions;
static s32 g_explosion_point[3];
static void spy_cre_explosion(q2_monster *m, const s32 *point, s32 scale, void *user)
{
    (void)m; (void)user;
    check(scale == 4096, "Strider death uses the native explosion scale");
    memcpy(g_explosion_point, point, sizeof(g_explosion_point)); g_explosions++;
}
static void test_disc_strider(q2_creature_module *mod)
{
    q2_monster m, other, foe;
    const q2_cre_impl *impl = mod->bind.impl;
    q2_cre_sound_bind sounds[48];
    q2_ai_world blocked = { 0 };
    static const char *native_bank[] = { "str_deth", "str_step", "tnk_step", NULL };
    static const char *fallback_bank[] = { "tnk_death", "tnk_step", NULL };
    s32 expected[3];
    u32 n;
    int i, first, next;
    for (i = 10; i <= 11; i++) {
        u32 step; bool found = false;
        for (step = 0; step < mod->think[i].step_count; step++)
            if (mod->think[i].step[step].op == Q2_CRE_OP_MELEE) {
                found = true;
                check(mod->think[i].step[step].damage_rand == 50,
                      "Strider's chained multiply-back decodes rand modulo 50");
            }
        check(found, "Strider's native melee instruction is decoded");
    }
    n = q2_creature_sound_bindings(mod->image, mod->size, Q2_CREWORLD_BASE, sounds, 48);
    if (n > 48) n = 48;
    check_name(q2_creature_sound_resolve("Strider", sounds, n, Q2_CREWORLD_BASE,
                   0x80102BD0u, stub_bank_has, (void *)native_bank), "str_step",
               "Strider's native step handle wins over its conditional Tank fallback");
    check_name(q2_creature_sound_resolve("Strider", sounds, n, Q2_CREWORLD_BASE,
                   0x80102BD0u, stub_bank_has, (void *)fallback_bank), "tnk_step",
               "the step slot has its own fallback despite sharing the idle name");
    check_name(q2_creature_sound_resolve("Strider", sounds, n, Q2_CREWORLD_BASE,
                   0x80102BCCu, stub_bank_has, (void *)fallback_bank), "tnk_death",
               "the death slot selects the death fallback");
    q2_level_reset(); q2_monster_init(&m); q2_monster_init(&foe);
    m.health = m.max_health = 1000; m.gib_health = -500;
    q2_creature_spawn(&mod->bind, &m, 0); q2_monster_start_go(&m);
    check(m.bigturn && m.bigturn_threshold == 1024 && m.mass == 500,
          "Strider installs its native turn callback and threshold");
    m.enemy = m.goalentity = &foe; foe.in_use = foe.client = true; foe.health = 100;
    foe.pos[2] = 3000; foe.view_height = -500; m.yaw_speed = 200;
    m.bigturn(&m, 200);
    check(m.currentmove->first_frame == 170 && m.angles[2] == 0,
          "visible positive big turn starts Right without snapping yaw");
    for (i = 0; i < 11; i++) impl->method[17](&m);
    check(m.angles[2] == 1023, "the eleven native right steps total 1023 units");
    m.run(&m);
    check(m.currentmove->first_frame == 216 && (m.aiflags & Q2_AI_BRUTAL),
          "turn completion passes through Standendwalk and marks a player target brutal");
    m.run(&m);
    check(m.currentmove->first_frame == 70, "Standendwalk enters StartWalk");
    m.run(&m);
    check(m.currentmove == q2_cre_find_move_at(&m, 0x801027BCu),
          "StartWalk completes into the running Walk record");
    blocked.line_of_sight = blocked_creature_sight; q2_ai_set_world(&blocked);
    m.bigturn(&m, -200);
    check(m.angles[2] == 823, "an unseen target permits the ordinary clamped turn");
    q2_ai_set_world(NULL); m.angles[2] = 0;
    /* Native exact-yaw decisions depend on the caller's preserved s2.
     * End-of-move repeat inherits zero; a running attack inherits the player
     * pointer, while standing AI carries M_MoveFrame's local frame index. */
    q2_cre_set_move(&m, 140); m.frame=144; memset(m.pos,0,sizeof(m.pos)); foe.pos[0]=0; foe.pos[2]=3000;
    q2_M_MoveFrame(&m);
    check(m.currentmove->first_frame==145,"exactly aligned blaster ends its burst");
    q2_cre_set_move(&m, 195); m.frame=198; foe.pos[2]=9000;
    q2_M_MoveFrame(&m);
    check(m.currentmove->first_frame==199,"exactly aligned laser ends its burst");
    q2_cre_set_move(&m, 140); m.frame=144; m.angles[2]=1; foe.pos[2]=3000;
    q2_M_MoveFrame(&m);
    check(m.currentmove->first_frame==140,"nonzero yaw inside the cone repeats");
    m.angles[2]=0; q2_cre_set_move(&m,18); q2_ai_native_s2=0; m.attack(&m);
    check(m.currentmove->first_frame==18,"zero inherited s2 refuses aligned attack");
    q2_ai_native_s2=1; m.attack(&m);
    check(m.currentmove->first_frame==137,"nonzero inherited s2 permits aligned attack");
    impl->method[6](&m);
    check(m.blind_target[1] == 0 && m.blind_target[2] == 3000,
          "Strider snapshots the enemy centre, without eye offset");
    q2_cre_set_shot_hook(spy_last_shot, NULL); q2_cre_set_muzzle_hook(spy_muzzle, NULL);
    foe.pos[0] = 6000; m.enemy = NULL;
    impl->method[7](&m);
    for (i = 0; i < 3; i++) expected[i] = m.blind_target[i] - g_last_shot.start[i];
    q2_vector_normalize(expected);
    check(g_muzzle_a == 145 && g_muzzle_b == 158 && g_last_shot.damage == 25 &&
          g_last_shot.effects == 64 && g_last_shot.dir[2] == expected[2] / 2 &&
          g_last_shot.dir[0] == expected[0] / 2,
          "Strider's blaster uses a half-speed normalized saved aim after enemy loss");
    impl->method[8](&m);
    check(g_muzzle_a == 456 && g_muzzle_b == 457 &&
          g_last_shot.slot == Q2_IMP_FIRE_LASER && g_last_shot.damage == 50 &&
          g_last_shot.kick == 25 && g_last_shot.dir[2] == expected[2],
          "laser keeps the full normalized saved aim and the other barrel");
    m.enemy = &foe; foe.pos[0] = foe.pos[2] = 0;
    q2_random_seed(2); first = q2_random_next(); next = q2_random_next();
    q2_random_seed(2); g_mel_calls = 0; impl->method[11](&m);
    check(g_mel_calls == 1 && g_mel_dmg == 100 + first % 50 &&
          g_mel_kick == 500 && g_mel_aim[0] == 2040 && q2_random_next() == next,
          "stomp's native modulus is 50, with doubled reach and a 500 kick");
    q2_cre_set_sound_hook(spy_sound, NULL); q2_cre_set_skill(2);
    q2_level_state.time = 100; m.health = 400; m.frame = 147; m.pain_debounce = 0;
    g_snd_calls = 0; m.pain(&m, 40);
    check(m.skinnum == 1 && m.pain_debounce == 0 && !g_snd_calls,
          "hard skill refuses pain during the last blaster end frame");
    m.frame = 148; m.pain(&m, 40);
    check(m.currentmove->first_frame == 103 && m.pain_debounce == 130 && g_snd_calls == 1,
          "the next frame accepts pain and the thirty-tick deadline");
    q2_cre_set_explosion_hook(spy_cre_explosion, NULL); g_explosions = 0;
    q2_monster_init(&other); other.class_byte = m.class_byte;
    mod->bind.strider_explosion_phase = 0;
    q2_random_seed(123); expected[0] = (q2_random_next() - 16384) / 32;
    expected[1] = (q2_random_next() - 16384) / 64 - 60;
    expected[2] = (q2_random_next() - 16384) / 32;
    q2_random_seed(123); impl->method[14](&m);
    check(g_explosions == 0, "the first shared death phase emits no explosion");
    impl->method[14](&other);
    check(g_explosions == 1 && !memcmp(expected, g_explosion_point, sizeof(expected)),
          "another Strider shares the alternating phase and exact three random offsets");
    m.health = 0; m.die(&m, 1);
    check(m.currentmove->first_frame == 111 && m.takedamage == Q2_DAMAGE_YES,
          "normal death starts the explosion animation while remaining damageable");
    m.currentmove->endfunc(&m);
    check(m.health == m.gib_health && m.takedamage == Q2_DAMAGE_NO &&
          (m.svflags & Q2_SVF_DEADMONSTER), "death completion queues shared corpse destruction");
    q2_cre_set_explosion_hook(NULL, NULL); q2_cre_set_sound_hook(NULL, NULL);
    q2_cre_set_muzzle_hook(NULL, NULL); q2_cre_set_shot_hook(roster_shot, NULL);
    q2_cre_set_skill(1);
}

static bool boss1_test_blend(const q2_monster *m, s16 a, s16 b,
                              q2_cre_attach *out, void *user)
{
    (void)m; (void)user;
    check((a == 564 && b == 571) || (a == 570 && b == 572),
          "Supertank rocket attachments use both native vertex pairs");
    out->off[0] = a == 564 ? 100 : 109;
    out->off[1] = a == 564 ? -101 : -109;
    out->off[2] = a == 564 ? 12 : -2;
    out->part = 7;
    return true;
}
static bool boss1_test_place(const q2_monster *m, const q2_cre_attach *a,
                              s32 out[3], void *user)
{
    int k;
    (void)m; (void)user;
    check(a && a->part == 7, "interpolated rocket tube retains the submodel");
    if (!a) return false;
    for (k = 0; k < 3; k++) out[k] = a->off[k];
    return true;
}
static void test_disc_boss1(q2_cre_bind *b)
{
    static const s32 expected[3][3] = { {101,-102,10}, {104,-104,6}, {107,-107,1} };
    q2_monster m, foe;
    q2_ai_world blocked = { 0 };
    int i, next;
    q2_level_reset(); q2_monster_init(&m); q2_monster_init(&foe);
    m.health = m.max_health = 1500; m.gib_health = -500;
    q2_creature_spawn(b, &m, 0); q2_monster_start_go(&m);
    m.enemy = m.goalentity = &foe; foe.in_use = foe.client = true; foe.health = 100;
    foe.pos[2] = 1920;
    check(m.mass == 800 && m.currentmove->first_frame == 194,
          "Supertank starts in its native stand with mass 800");
    m.walk(&m); check(m.currentmove == q2_cre_find_move_at(&m, 0x801014B8u),
                      "Supertank walk selects the walking Forward record");
    m.run(&m); check(m.currentmove == q2_cre_find_move_at(&m, 0x80101470u),
                     "Supertank run selects the running Forward record");
    q2_random_seed(2); next = q2_random_next(); q2_random_seed(2);
    m.attack(&m);
    check(m.currentmove->first_frame == 0 && q2_random_next() == next,
          "the inclusive 1920-unit attack boundary selects bullets without RNG");
    q2_cre_set_shot_hook(spy_last_shot, NULL); q2_cre_set_muzzle_hook(spy_muzzle, NULL);
    m.frame = -1; g_shot_calls = 0;
    for (i = 0; i < 6; i++) q2_M_MoveFrame(&m);
    check(g_shot_calls == 6 && g_muzzle_a == 412 && g_muzzle_b == 419 &&
          g_last_shot.damage == 6 && g_last_shot.kick == 4 &&
          g_last_shot.hspread == 300 && g_last_shot.vspread == 500,
          "each of six chaingun frames fires from the native attachment");
    blocked.line_of_sight = blocked_creature_sight; q2_ai_set_world(&blocked);
    q2_M_MoveFrame(&m);
    check(m.currentmove->first_frame == 6 && g_shot_calls == 6,
          "losing visibility exits the burst through Attack a");
    q2_ai_set_world(NULL);
    q2_cre_set_attach_hooks(boss1_test_blend, boss1_test_place, NULL);
    for (i = 0; i < 3; i++) {
        m.frame = (s16)(27 + 3 * i); b->impl->method[3](&m);
        check(!memcmp(g_last_shot.start, expected[i], sizeof(expected[i])) &&
              g_last_shot.slot == Q2_IMP_FIRE_ROCKET && g_last_shot.damage == 50 &&
              g_last_shot.speed == 500,
              "rocket tubes retain signed truncation at all three positions");
    }
    q2_cre_set_attach_hooks(NULL, NULL, NULL);
    q2_cre_set_sound_hook(spy_sound, NULL); q2_cre_set_skill(2);
    q2_level_state.time = 100; m.health = 500; m.frame = 33;
    m.pain_debounce = 0; g_snd_calls = 0; m.pain(&m, 30);
    check(m.skinnum == 1 && m.pain_debounce == 0 && g_snd_calls == 0,
          "hard skill protects rocket frame 33 while still updating the skin");
    m.frame = 34; m.pain(&m, 30);
    check(m.currentmove->first_frame == 172 && m.pain_debounce == 130 && !g_snd_calls,
          "the next frame accepts the silent native large pain move");
    q2_cre_set_skill(3); q2_level_state.time = 140; m.stand(&m); m.pain(&m, 30);
    check(m.currentmove->first_frame == 194 && m.pain_debounce == 170,
          "nightmare updates the pain deadline without changing animation");
    m.health = -999; m.die(&m, 1000);
    check(m.currentmove->first_frame == 300 && m.dead && m.takedamage == Q2_DAMAGE_NO,
          "even overkill starts Supertank's invulnerable death animation");
    q2_cre_set_explosion_hook(spy_cre_explosion, NULL);
    b->boss1_explosion_phase = 0; g_explosions = 0; m.frame = 299;
    for (i = 0; i < 34; i++) q2_M_MoveFrame(&m);
    check(g_explosions == 6, "the last twelve death frames emit six explosions");
    q2_M_MoveFrame(&m);
    check(m.health == m.gib_health && m.next_think == 0 &&
          (m.svflags & Q2_SVF_DEADMONSTER), "death completion queues shared destruction");
    q2_cre_set_explosion_hook(NULL, NULL); q2_cre_set_sound_hook(NULL, NULL);
    q2_cre_set_muzzle_hook(NULL, NULL); q2_cre_set_shot_hook(roster_shot, NULL);
    q2_cre_set_skill(1);
}

static void test_disc_jorg(q2_cre_bind *b)
{
    q2_monster m, foe;
    int i, seed, r, counts[2] = {0};
    q2_monster_init(&m); q2_monster_init(&foe); q2_level_reset();
    m.health = m.max_health = 3000; m.gib_health = -2000;
    q2_creature_spawn(b, &m, 0); q2_monster_start_go(&m);
    m.enemy = &foe; foe.health = 100; foe.pos[2] = 3000; foe.view_height = -200;
    check(b->cre->class_count == 2 && b->cre->class_method_table[0] !=
          b->cre->class_method_table[1] && b->cre->move_count < 20,
          "Jorg's two registered classes keep their distinct method tables");
    m.walk(&m); check(m.currentmove->image_offset == 0x43DC, "Jorg walk binds its own native move");
    m.run(&m); check(m.currentmove->image_offset == 0x4380, "Jorg run binds its own native move");
    for (seed = 1; seed < 100; seed++) {
        q2_random_seed(seed); r = q2_random_next(); q2_random_seed(seed); m.attack(&m);
        check(m.currentmove->first_frame == (r < 24576 ? 0 : 18), "Jorg attack uses the native 24576 threshold");
        counts[r >= 24576]++;
    }
    check(counts[0] && counts[1], "both Jorg attacks were exercised");
    q2_cre_set_shot_hook(spy_last_shot, NULL); q2_cre_set_muzzle_hook(spy_muzzle, NULL);
    g_shot_calls = 0; q2_cre_set_move(&m, 8); m.frame = 7;
    for (i = 0; i < 6; i++) q2_M_MoveFrame(&m);
    check(g_shot_calls == 12 && g_last_shot.damage == 6 && g_last_shot.kick == 4 &&
          g_last_shot.hspread == 300 && g_last_shot.vspread == 500 &&
          g_last_shot.flash == 0 && g_muzzle_a == 226 && g_muzzle_b == 228,
          "Jorg's six native firing frames emit both guns, with no muzzle-flash ID");
    b->impl->method[4](&m);
    check(g_last_shot.slot == Q2_IMP_FIRE_BFG && g_last_shot.damage == 50 &&
          g_last_shot.flash == 132 && g_muzzle_a == 165 && g_muzzle_b == 161,
          "Jorg BFG retains its native origin and parameters");
    q2_cre_set_skill(3); m.pain_debounce = 0; m.frame = 112;
    m.pain(&m, 101); check(m.pain_debounce == 30, "nightmare still sets Jorg's pain deadline");
    q2_cre_set_skill(1); q2_cre_set_shot_hook(roster_shot, NULL); q2_cre_set_muzzle_hook(NULL, NULL);
}

static void test_disc_rider(q2_cre_bind *b)
{
    q2_monster m, foe; s32 target[3]; int seed, r, i, seen[3] = {0};
    q2_monster_init(&m); q2_monster_init(&foe); q2_level_reset();
    m.health = m.max_health = 3000; m.gib_health = -2000;
    q2_creature_spawn(b, &m, 0); q2_monster_start_go(&m);
    m.enemy = &foe; foe.health = 100; foe.pos[0] = 100; foe.pos[2] = 3000;
    foe.view_height = -200;
    for (seed = 1; seed < 150; seed++) {
        q2_random_seed(seed); r = q2_random_next(); q2_random_seed(seed); m.attack(&m);
        i = r < 9831 ? 0 : r < 19961 ? 1 : 2; seen[i]++;
        check(m.currentmove->first_frame == (i == 0 ? 201 : i == 1 ? 209 : 235),
              "Rider attack probabilities match both native boundaries");
    }
    check(seen[0] && seen[1] && seen[2], "all three Rider attacks were exercised");
    q2_cre_set_shot_hook(spy_last_shot, NULL); q2_cre_set_muzzle_hook(spy_muzzle, NULL);
    g_shot_calls = 0; q2_cre_set_move(&m, 209); m.frame = 208;
    for (i = 0; i < 26; i++) q2_M_MoveFrame(&m);
    check(g_shot_calls == 17 && g_last_shot.damage == 15 && g_last_shot.speed == 1000 &&
          g_muzzle_a == 419 && g_muzzle_b == 422,
          "Rider's sweep fires all seventeen blaster frames from its native muzzle");
    b->impl->method[2](&m); memcpy(target, m.blind_target, sizeof(target));
    foe.pos[0] += 4000; b->impl->method[1](&m);
    check(!memcmp(target, m.blind_target, sizeof(target)) &&
          g_last_shot.slot == Q2_IMP_FIRE_RAILGUN && g_last_shot.damage == 50 &&
          g_last_shot.kick == 100 && g_last_shot.dir[0] == 0 &&
          g_muzzle_a == 225 && g_muzzle_b == 227,
          "the railgun fires at the saved eye position after the target moves");
    b->impl->method[6](&m);
    check(g_last_shot.slot == Q2_IMP_FIRE_BFG && g_last_shot.flash == 101 &&
          g_muzzle_a == 431 && g_muzzle_b == 434, "Rider BFG keeps its separate muzzle");
    q2_level_state.sight_client = &foe; m.health = 0; m.die(&m, 1);
    check(m.health == m.gib_health && m.takedamage == Q2_DAMAGE_NO &&
          (m.svflags & Q2_SVF_DEADMONSTER) && !m.next_think &&
          !memcmp(foe.blind_target, m.pos, sizeof(m.pos)),
          "Rider death publishes its position and immediately queues destruction");
    q2_level_state.sight_client = NULL;
    q2_cre_set_shot_hook(roster_shot, NULL); q2_cre_set_muzzle_hook(NULL, NULL);
}

static q2_monster *g_medic_trace_target;
static int g_medic_beams, g_medic_revives;
static void medic_test_trace(void *u, const s32 from[3], const s16 lo[3],
    const s16 hi[3], const s32 to[3], const q2_monster *ignore, u32 mask, q2_ai_trace *tr)
{
    (void)u; (void)from; (void)lo; (void)hi; (void)ignore;
    check(mask == 0x06000003u, "Medic cable trace includes native dead-actor mask");
    memset(tr, 0, sizeof(*tr)); memcpy(tr->endpos, to, sizeof(tr->endpos));
    tr->fraction = 4096; tr->ent = g_medic_trace_target;
}
static void medic_test_beam(q2_monster *m, q2_monster *c, s16 vertex, void *u)
{ (void)m; (void)c; (void)u; check(vertex == 308, "Medic cable uses model vertex 308"); g_medic_beams++; }
static bool medic_test_revive(q2_monster *m, q2_monster *c, void *u)
{ (void)m; (void)u; g_medic_revives++; c->health = c->max_health; c->corpse = c->dead = false; return true; }
static void test_disc_medic(q2_cre_bind *b)
{
    q2_monster set[3], foe; q2_ai_world w = {0}; int i;
    q2_monster *m = &set[0], *c = &set[2];
    q2_level_reset(); q2_monster_init(&foe);
    for (i = 0; i < 3; i++) q2_monster_init(&set[i]);
    m->health = m->max_health = 300; m->gib_health = -130;
    q2_creature_spawn(b, m, 0); q2_monster_start_go(m);
    foe.health = 100; foe.in_use = foe.client = true; foe.spawnflags |= Q2_SVFLAG_INUSE;
    for (i = 1; i < 3; i++) {
        set[i].in_use = set[i].dead = set[i].corpse = true;
        set[i].spawnflags |= Q2_SVFLAG_INUSE; set[i].class_id = Q2_CLASS_CORPSE;
        set[i].gib_health = -100; set[i].max_health = (s16)(i * 100);
        set[i].pos[2] = i * 1000; set[i].hit_height = 150;
    }
    q2_cre_set_lifecycle_hooks(set, 3, NULL, medic_test_revive, NULL);
    q2_ai_set_world(NULL); m->enemy = &foe; m->idle(m);
    check(m->enemy == c && m->oldenemy == &foe && (m->aiflags & Q2_AI_MEDIC),
          "Medic prefers the highest max-health corpse and retains its player enemy");
    check(m->checkattack(m) && m->currentmove->first_frame == 209,
          "a corpse inside 3072 immediately starts the cable sequence");
    w.trace = medic_test_trace; q2_ai_set_world(&w);
    q2_cre_set_contact_hooks(NULL, medic_test_beam, NULL);
    g_medic_beams = g_medic_revives = 0; g_medic_trace_target = NULL;
    m->frame = 219; b->impl->method[10](m);
    check(!g_medic_beams && !c->effect[5], "blocked cable does not heal or draw a beam");
    g_medic_trace_target = c; b->impl->method[10](m);
    check(g_medic_beams == 1 && c->effect[5] == 30 &&
          (c->aiflags & Q2_AI_RESURRECTING), "cable hit arms native healing presentation");
    m->frame = 226; b->impl->method[10](m);
    check(!g_medic_revives && c->think == q2_cre_medic_revive &&
          c->oldenemy == &foe && c->owner == m, "frame 226 defers revival to the actor handler");
    q2_cre_medic_revive(c);
    check(g_medic_revives == 1 && c->health == 200, "the deferred handler revives the selected body");
    b->impl->method[11](m); check(!(c->aiflags & Q2_AI_RESURRECTING), "retraction clears the healing flag");
    m->enemy = &foe; q2_cre_set_shot_hook(spy_last_shot, NULL); q2_cre_set_muzzle_hook(spy_muzzle, NULL);
    m->frame = 185; b->impl->method[7](m);
    check(g_last_shot.damage == 2 && g_last_shot.speed == 1000 && g_last_shot.flash == 0 &&
          g_muzzle_a == 239 && g_muzzle_b == 246, "Medic blaster uses the recovered attachment and figures");
    q2_cre_set_lifecycle_hooks(NULL, 0, NULL, NULL, NULL); q2_ai_set_world(NULL);
    q2_cre_set_contact_hooks(NULL, NULL, NULL); q2_cre_set_muzzle_hook(NULL, NULL);
    q2_cre_set_shot_hook(roster_shot, NULL);
}

static q2_monster *g_boss2_trace_target;
static void boss2_test_trace(void *user, const s32 start[3], const s16 mins[3],
                              const s16 maxs[3], const s32 end[3],
                              const q2_monster *ignore, u32 mask, q2_ai_trace *out)
{
    (void)user; (void)start; (void)end; (void)ignore;
    check(!mins && !maxs && mask == 0x02000019u,
          "Boss2 uses its native point trace mask for the attack decision");
    memset(out, 0, sizeof(*out)); out->fraction = 4096;
    out->ent = g_boss2_trace_target;
}
static bool boss2_test_blend(const q2_monster *m, s16 a, s16 b,
                              q2_cre_attach *out, void *user)
{
    (void)m; (void)user;
    check((a == 237 && b == 239) || (a == 238 && b == 240),
          "Boss2 uses the two native rocket-span attachments");
    out->off[0] = a == 237 ? -8 : 8;
    out->off[1] = out->off[2] = 0; out->part = 7; return true;
}
static q2_cre_shot g_boss2_shots[8];
static void boss2_test_shot(q2_monster *m, const q2_cre_shot *s, void *user)
{
    (void)m; (void)user;
    if (g_shot_calls < 8) g_boss2_shots[g_shot_calls] = *s;
    g_shot_calls++;
}
static void test_disc_boss2(q2_cre_bind *b)
{
    q2_monster m, foe;
    q2_ai_world w = { 0 };
    int k, seed, first, second;
    q2_level_reset(); q2_monster_init(&m); q2_monster_init(&foe);
    m.health = m.max_health = 2000; m.gib_health = -500;
    q2_creature_spawn(b, &m, 0); q2_monster_start_go(&m);
    check((m.flags & (Q2_FL_FLY | Q2_FL_IMMUNE_LASER)) ==
          (Q2_FL_FLY | Q2_FL_IMMUNE_LASER) && m.mass == 1000,
          "Boss2 starts flying with its laser-immunity flag");
    for (k = 0; k < (int)b->move_count; k++)
        check(b->move_name[k] && b->move_name[k][0],
              "every Boss2 move keeps its exported name past the reversed Stand2 entry");
    m.enemy = m.goalentity = &foe; foe.in_use = foe.client = true; foe.health = 100;
    foe.pos[0] = 3000; q2_level_state.sight_entity = &foe;
    b->impl->method[11](&m);
    check(m.pos[1] == -1536 && m.health == 1999 && m.angles[2] == 1024,
          "native activation raises Boss2 once and faces the sight entity");
    b->impl->method[11](&m);
    check(m.pos[1] == -1536 && m.health == 1999, "the health latch prevents another rise");
    q2_level_state.sight_entity = NULL;
    m.pos[1] = 0; m.angles[2] = 0; foe.pos[0] = 0; foe.pos[2] = 1500;
    q2_random_seed(2); first = q2_random_next(); q2_random_seed(2); m.attack(&m);
    check(m.currentmove->first_frame == 70 && q2_random_next() == first,
          "the inclusive 1500-unit boundary chooses chainguns without RNG");
    q2_cre_set_shot_hook(boss2_test_shot, NULL); q2_cre_set_muzzle_hook(spy_muzzle, NULL);
    g_shot_calls = 0; foe.velocity[0] = 4096; foe.view_height = -200;
    b->impl->method[9](&m);
    check(g_shot_calls == 2 && g_boss2_shots[0].flash == 73 &&
          g_boss2_shots[1].flash == 133 && g_muzzle_a == 339 && g_muzzle_b == 344 &&
          g_boss2_shots[0].damage == 6 && g_boss2_shots[0].kick == 4 &&
          g_boss2_shots[0].dir[0] < 0 && g_boss2_shots[0].dir[1] == 0,
          "both guns retain their flash IDs, eye aim and negative velocity lead");
    q2_cre_set_attach_hooks(boss2_test_blend, boss1_test_place, NULL);
    g_shot_calls = 0; b->impl->method[10](&m);
    check(g_shot_calls == 4, "one Boss2 rocket method launches four projectiles");
    for (k = 0; k < 4; k++)
        check(g_boss2_shots[k].start[0] == -6 + 4 * k &&
              g_boss2_shots[k].slot == Q2_IMP_FIRE_ROCKET &&
              g_boss2_shots[k].damage == 50 && g_boss2_shots[k].speed == 500,
              "the four native rocket tubes occupy the centres of four equal intervals");
    q2_cre_set_attach_hooks(NULL, NULL, NULL);
    w.trace = boss2_test_trace; q2_ai_set_world(&w); g_boss2_trace_target = NULL;
    check(!m.checkattack(&m), "a clear-looking trace that did not hit the enemy cannot fire");
    g_boss2_trace_target = &foe; foe.pos[2] = 100; q2_cre_set_skill(0);
    q2_random_seed(2); m.checkattack(&m);
    check(m.attack_state == Q2_AS_MISSILE && q2_random_next() == first,
          "Boss2's easy melee-range decision consumes no probability roll");
    foe.pos[2] = 2000;
    for (seed = 1; seed < 1000; seed++) {
        q2_random_seed(seed); first = q2_random_next();
        if (first >= 13106 && first < 26213) break;
    }
    second = q2_random_next(); q2_level_state.time = 100; m.attack_finished = 0;
    q2_random_seed(seed); m.aiflags = 0;
    check(m.checkattack(&m) && m.attack_finished == 100 + 20 * second / 32767,
          "mobile Boss2 uses its 26213 threshold and exact 0..20-tick cooldown");
    m.attack_finished = 0; m.aiflags = Q2_AI_STAND_GROUND; q2_random_seed(seed);
    check(!m.checkattack(&m), "stand-ground uses the lower 13106 threshold");
    q2_ai_set_world(NULL); q2_cre_set_skill(3); m.health = 900;
    m.pain_debounce = 0; m.pain(&m, 29);
    check(m.currentmove->first_frame == 128 && m.skinnum == 1,
          "Boss2 still takes its short pain animation on nightmare");
    q2_level_state.time += 31; m.pain(&m, 30);
    check(m.currentmove->first_frame == 110, "damage 30 selects the long pain move");
    m.health = 0; m.die(&m, 1);
    check(m.dead && m.health == m.gib_health && m.currentmove->first_frame == 132 &&
          !m.next_think && m.takedamage == Q2_DAMAGE_NO &&
          (m.svflags & Q2_SVF_DEADMONSTER),
          "death immediately queues destruction rather than waiting for 48 frames");
    q2_cre_set_skill(1); q2_cre_set_muzzle_hook(NULL, NULL);
    q2_cre_set_shot_hook(roster_shot, NULL);
}

static int script_sound_count;
static u32 script_sound_handle;
static const q2_monster *script_sound_owner;
static void script_sound(q2_monster *m, u32 handle, const q2_monster *owner, void *user)
{
    (void)m; (void)user;
    script_sound_count++; script_sound_handle = handle;
    script_sound_owner = owner;
}

static void test_disc_script(q2_creature_module *mod)
{
    q2_monster m, player;
    q2_monster *players[1] = { &player };
    q2_monster_set set = {0};
    q2_cre_sound_bind sound[48];
    u32 n;
    int i, start;
    const char *name = mod->name;
    q2_level_reset(); q2_monster_init(&m); q2_monster_init(&player);
    m.health = m.max_health = 3000; m.gib_health = -500;
    m.class_id = m.class_byte = 99;
    player.in_use = player.client = true;
    player.spawnflags = Q2_SVFLAG_INUSE; player.health = 100;
    player.pos[0] = 1234; player.pos[1] = -321; player.pos[2] = 567;
    q2_ai_set_clients(players, 1); q2_level_state.sight_client = &player;
    {
        u32 before = q2_random_state(), expected = before;
        if (!strcmp(name, "Rider2") || !strcmp(name, "Rider3")) {
            q2_random_next(); expected = q2_random_state(); q2_random_seed(before);
        }
        q2_creature_prepare(&mod->bind, &m, 0);
        check(q2_random_state() == before && !m.spawn_initialized,
              "preparing a dormant script consumes no random draw");
        q2_monster_start_go(&m);
        check(m.spawn_initialized && q2_random_state() == expected,
              "script initialization consumes its native draws only when activated");
        q2_monster_start_go(&m);
        check(q2_random_state() == expected, "a second wake does not repeat script initialization");
    }
    set.monsters = &m; set.count = 1;
    check(m.start_kind == Q2_CRE_START_SCRIPT && m.think && !m.currentmove,
          "scripted module retains its direct think through wake");
    check(q2_level_state.total_monsters == 0 && !(m.svflags & Q2_SVF_MONSTER),
          "scripted props never enter monster_start's counters or flags");
    check(m.class_byte == 99 && q2_cre_bind_for(&m) == &mod->bind,
          "classless modules preserve the descriptor byte and bind identity");
    check(mod->frame_name_count > 0, "scripted pose names come from export 3");
    q2_cre_set_sound_owner_hook(script_sound, NULL); script_sound_count = 0;
    if (!strcmp(name, "RiderStand")) {
        check(m.frame == 414 && m.next_think == 2 && m.use && m.pain && m.die,
              "RiderStand installs its native 414-frame loop and activation callbacks");
        q2_monster_set_tick(&set); check(m.frame == 414, "projection waits two ticks");
        q2_monster_set_tick(&set); check(m.frame == 415, "projection starts on its deadline");
        m.frame = 473; q2_monster_set_tick(&set);
        check(m.frame == 414, "projection wraps at frame 474");
        n = q2_creature_sound_bindings(mod->image, mod->size, mod->cre.base, sound, 48);
        check(n == 2, "RiderStand's register-copied sound handles decode");
        if (n == 2) {
            check(!strcmp(q2_creature_sound_for_addr(sound,n,mod->cre.base+0x990), "msc_tele1"),
                  "projection shrink uses the native teleport sound");
            check(!strcmp(q2_creature_sound_for_addr(sound,n,mod->cre.base+0x99C), "mak_laf2"),
                  "projection activation uses the native laugh");
        }
        m.pain(&m, 1); m.pain(&m, 1);
        check(script_sound_count == 1 && m.script_pain_used && q2_cre_think_id(&m) == 3,
              "only the first hit activates projection pain");
        check(script_sound_owner == &player,
              "projection sound remains owned by the player");
        for (i = 0; i < 4; i++) q2_monster_set_tick(&set);
        check(m.render_scale == 2048 && script_sound_count == 2 &&
              script_sound_handle == mod->cre.base + 0x990,
              "four shrink ticks halve scale and play the teleport sound");
        for (i = 0; i < 4; i++) q2_monster_set_tick(&set);
        check(m.render_scale == 0 && m.health == m.gib_health && m.dead &&
              m.next_think == 0 && m.takedamage == Q2_DAMAGE_NO &&
              (m.svflags & Q2_SVF_DEADMONSTER), "eight shrink ticks destroy the projection");
    } else if (!strcmp(name, "Rider2")) {
        check(m.frame == 251 && m.health == 5000 && m.next_think == 2,
              "fresh Rider2 begins Deatha at 251 and sets health 5000");
        for (i = 0; i < 95; i++) q2_monster_set_tick(&set);
        check(m.frame == 345 && !m.next_think && !m.dead, "Rider2 holds final frame 345");
        for (i = 0; i < 5; i++) q2_monster_set_tick(&set);
        check(m.frame == 345, "a disabled living timer cannot keep advancing");
        mod->bind.impl->spawn(&m);
        check(m.frame == 344, "restored 5000-health Rider2 takes its one-frame tail");
        q2_monster_set_tick(&set); q2_monster_set_tick(&set);
        check(m.frame == 345 && !m.next_think, "Rider2 tail stops on its own deadline");
    } else if (!strcmp(name, "Rider3")) {
        check(m.frame == 0 && m.health == 3000 && m.next_think == 2,
              "Rider3 preserves health and schedules Death2");
        for (i = 0; i < 21; i++) q2_monster_set_tick(&set);
        check(m.frame == 0 && m.next_think == q2_level_state.time + 1,
              "Rider3 loops exactly twenty frames");
    } else {
        check(m.frame == 0 && m.next_think == 5 && m.use &&
              m.flags == Q2_FL_GODMODE && m.takedamage == Q2_DAMAGE_NO,
              "DeadComm starts invulnerable with its five-tick drop timer");
        for (i = 0; i < 5; i++) q2_monster_set_tick(&set);
        check(m.pos[1] == -24 && !m.next_think && m.movetype == Q2_MOVETYPE_TOSS,
              "DeadComm's drop changes Y by -24 and does not rearm");
        for (i = 0; i < 5; i++) q2_monster_set_tick(&set);
        check(m.pos[1] == -24, "the drop is not called again");
        m.use(&m); start = script_sound_count;
        for (i = 0; i < 23; i++) q2_monster_set_tick(&set);
        check(start == 1 && script_sound_count == 24 && m.frame == 23 && !m.next_think,
              "DeadComm activates with sight sound then plays idle on all 23 frames");
    }
    check(q2_cre_think_valid(&m, q2_cre_think_id(&m)), "script callback has a stable save id");
    check(!q2_cre_think_valid(&m, 255), "invalid callback ids are rejected");
    q2_cre_set_sound_owner_hook(NULL, NULL); q2_ai_set_clients(NULL, 0);
}

static void test_disc_roster(const char *path, const char *map,
                              const char *only_module, int zone_index,
                              u32 expected_cases)
{
    disc *d = NULL;
    q2_build_id id;
    q2_common_file common;
    q2_zone_file zone;
    q2_buf buf;
    char common_path[160];
    q2_creature_world *w = calloc(1, sizeof(*w));
    u32 i, j, cases = 0;
    u32 load_rng;
    int skill;

    printf("disc-backed native creature roster (%s)\n", map);
    if (!w) exit(2);
    snprintf(common_path, sizeof(common_path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (disc_open(&d, path) != Q2_OK || q2_identify(d, &id) != Q2_OK ||
        disc_read_file(d, common_path, &buf) != Q2_OK)
        exit(2);
    if (q2_common_open(&common, &buf) != Q2_OK) exit(2);
    if (zone_index >= 0) {
        snprintf(common_path, sizeof(common_path), "Q2DATA/LEVELS/%s/ZONE%d.DAT",
                 map, zone_index);
        if (disc_read_file(d, common_path, &buf) != Q2_OK ||
            q2_zone_open(&zone, &buf) != Q2_OK) exit(2);
    }
    load_rng = q2_random_state();
    if (q2_creature_world_load(w, d, &id, &common,
                               zone_index >= 0 ? &zone : NULL, NULL, NULL) != Q2_OK)
        exit(2);
    check(q2_random_state() == load_rng,
          "loading population metadata does not run dormant creatures' spawn RNG");
    check(w->mod_count >= (zone_index < 0 ? 7u : 8u),
               "COMMON donors and the zone's native module load together");
    q2_ai_set_world(NULL);
    q2_cre_set_shot_hook(roster_shot, NULL);
    q2_cre_set_melee_hook(spy_melee, NULL);

    for (i = 0; i < w->mod_count; i++) {
        const q2_creature_module *mod = &w->mod[i];
        if (only_module && strcmp(mod->name, only_module)) continue;
        for (j = 0; j < mod->cre.move_count; j++) {
            const q2_mmove *mv = &mod->bind.move[j];
            s32 f;
            if (mod->cre.move[j].clip_piece) continue;
            check(mv->frames && mv->last_frame >= mv->first_frame,
                  "every real move has a valid frame range");
            for (f = 0; f <= mv->last_frame - mv->first_frame; f++) {
                u32 think = mv->frames[f].think;
                if (think)
                    check(mod->bind.impl->method[think] != NULL,
                          "every nonempty frame has a native think implementation");
            }
        }
        if (!strcmp(mod->name, "Flyer")) {
            check(mod->image[0x2F2] == (!strcmp(map, "MAGDEMO") ? 0x5F : 0x69),
                  "the active zone's own module variant was loaded");
            test_disc_flyer(&w->mod[i].bind);
        }
        if (!strcmp(mod->name, "Gladiator"))
            test_disc_gladiator(&w->mod[i]);
        if (!strcmp(mod->name, "Hover"))
            test_disc_hover(&w->mod[i]);
        if (!strcmp(mod->name, "Parasite"))
            test_disc_parasite(&w->mod[i].bind);
        if (!strcmp(mod->name, "Ironmaiden"))
            test_disc_ironmaiden(&w->mod[i]);
        if (!strcmp(mod->name, "Strider"))
            test_disc_strider(&w->mod[i]);
        if (!strcmp(mod->name, "Boss1"))
            test_disc_boss1(&w->mod[i].bind);
        if (!strcmp(mod->name, "Boss2"))
            test_disc_boss2(&w->mod[i].bind);
        if (!strcmp(mod->name, "Jorg")) test_disc_jorg(&w->mod[i].bind);
        if (!strcmp(mod->name, "Rider")) test_disc_rider(&w->mod[i].bind);
        if (!strcmp(mod->name, "Medic")) test_disc_medic(&w->mod[i].bind);
        if (mod->bind.impl->scripted) test_disc_script(&w->mod[i]);
    }

    for (skill = 0; skill < 4; skill++) {
        q2_cre_set_skill(skill);
        for (i = 0; i < w->classes.count; i++) {
            const q2_class_entry *entry = &w->classes.entries[i];
            q2_monster m, foe;
            q2_creature_module *mod;
            int tick;
            if (entry->is_player || entry->id >= Q2_MONSTER_CLASS_COUNT ||
                w->class_module[entry->id] < 0) continue;
            if (only_module && strcmp(entry->name, only_module)) continue;
            mod = &w->mod[w->class_module[entry->id]];
            if (mod->bind.impl->scripted) continue;
            q2_level_reset();
            q2_random_seed(1234);
            q2_monster_init(&m);
            q2_monster_init(&foe);
            m.pop_class_id = (u8)entry->id;
            m.health = m.max_health = entry->health;
            m.gib_health = entry->gib_health;
            q2_creature_spawn(&mod->bind, &m, w->class_variant[entry->id]);
            q2_monster_start_go(&m);
            check(m.currentmove && m.stand && m.walk && m.run,
                  "every class starts with live locomotion callbacks");
            foe.health = 10000;
            foe.in_use = foe.client = true;
            foe.spawnflags |= Q2_SVFLAG_INUSE;
            foe.pos[2] = m.attack ? 3000 : 100;
            m.enemy = m.goalentity = &foe;
            if (!strcmp(mod->name, "Parasite")) parasite_test_world(&foe);
            else q2_ai_set_world(NULL);
            g_parasite_hits = 0;
            g_shot_calls = g_mel_calls = 0;
            if (m.attack) m.attack(&m);
            else if (m.melee) m.melee(&m);
            for (tick = 0; tick < 150; tick++) {
                q2_level_state.time++;
                q2_level_state.framenum++;
                q2_M_MoveFrame(&m);
                check(m.currentmove != NULL, "animation chains retain a move");
            }
            if (!strcmp(mod->name, "Parasite"))
                check(g_parasite_hits > 0, "the Parasite's ranged attack drains");
            else if (m.attack)
                check(g_shot_calls > 0, "a ranged creature's attack fires");
            else if (m.melee)
                check(g_mel_calls > 0, "a melee creature's attack swings");
            else
                check(g_shot_calls == 0 && g_mel_calls == 0,
                      "the Insane remains a non-combatant");

            m.health = 0;
            if (m.die) m.die(&m, 10);
            check(m.dead && m.currentmove, "death retains a valid native pose");
            for (tick = 0; tick < 200 && !(m.svflags & Q2_SVF_DEADMONSTER); tick++) {
                q2_level_state.time++;
                q2_level_state.framenum++;
                q2_M_MoveFrame(&m);
            }
            check((m.svflags & Q2_SVF_DEADMONSTER) != 0,
                  "the death animation reaches its corpse state");
            printf("  skill %d class %u %-10s: %d shots, %d swings, corpse %d\n",
                   skill, entry->id, entry->name, g_shot_calls, g_mel_calls,
                   !!(m.svflags & Q2_SVF_DEADMONSTER));
            cases++;
        }
    }
    check_eq_i(cases, expected_cases,
               "native population variants run at four difficulties");
    q2_cre_set_shot_hook(NULL, NULL);
    q2_cre_set_melee_hook(NULL, NULL);
    q2_cre_set_contact_hooks(NULL, NULL, NULL);
    q2_ai_set_world(NULL);
    q2_cre_set_skill(1);
    q2_creature_world_free(w);
    if (zone_index >= 0) q2_zone_close(&zone);
    q2_common_close(&common);
    disc_close(d);
    free(w);
}

/* ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Q2PSX-PC creature tests\n\n");

    test_frame_name_table();
    test_summoned_batch_is_started();
    test_batch_zone_eligibility();
    test_player_noise_stamps_the_window();
    test_bind();
    test_population_spawn_flags();
    test_spawn_difficulty_masks();
    test_spawn_loop_honours_difficulty();
    test_corner_trigger_box();
    test_kill_tally_classes();
    test_move_lookup();
    test_endfunc_resolution();
    test_frames_drive();
    test_generic_fallback();
    test_soldier_present();
    test_actions();
    test_sound_fallback();
    test_fire_shot_has_no_enemy_guard();
    test_tank_burst_rule();
    test_soldier_burst_and_death_aim();
    test_infantry_death_sweep();
    test_gunner_and_arachner_aim();
    test_flyer_aim();
    test_melee_geometry();
    test_tank_footstep_rumble();
    test_insane_prone_flies();
    test_insane_start_draws();
    if (argc == 3 && strcmp(argv[1], "--disc") == 0)
    {
        test_disc_roster(argv[2], "BASE0", NULL, -1, 36);
        test_disc_roster(argv[2], "MAGDEMO", "Soldier", -1, 12);
        test_disc_roster(argv[2], "BASE2", "Flyer", 1, 4);
        test_disc_roster(argv[2], "MAGDEMO", "Flyer", 1, 4);
        test_disc_roster(argv[2], "BIGGUN", "Gladiator", 1, 4);
        test_disc_roster(argv[2], "JAIL3", "Hover", 1, 4);
        test_disc_roster(argv[2], "BASE3", "Parasite", 0, 4);
        test_disc_roster(argv[2], "BOSS1", "Ironmaiden", 0, 4);
        test_disc_roster(argv[2], "BIGGUN", "Strider", 3, 4);
        test_disc_roster(argv[2], "SECURITY", "Boss1", 1, 4);
        test_disc_roster(argv[2], "COMMAND", "Boss2", 4, 4);
        test_disc_roster(argv[2], "BOSS2", "Jorg", 1, 4);
        test_disc_roster(argv[2], "BOSS2", "Rider", 1, 4);
        test_disc_roster(argv[2], "JAIL3", "Medic", 3, 4);
        test_disc_roster(argv[2], "BOSS1", "RiderStand", 1, 0);
        test_disc_roster(argv[2], "BOSS2", "Rider2", 1, 0);
        test_disc_roster(argv[2], "BOSS2", "Rider3", 1, 0);
        test_disc_roster(argv[2], "LAB", "DeadComm", 2, 0);
    }

    q2_cre_bind_reset();
    q2_class_table_reset();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");

    return g_failures ? 1 : 0;
}
