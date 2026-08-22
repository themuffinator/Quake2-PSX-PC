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
#include <stdio.h>
#include <string.h>

#include "ai.h"
#include "crebind.h"
#include "creature.h"
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
    check_eq_i(q2_spawn_from_population(&set, &pop, NULL, NULL), Q2_OK,
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

static void spy_sound(q2_monster *m, int which, void *u)
{
    (void)m; (void)u; g_snd_calls++; g_snd_last = which;
}

static void spy_melee(q2_monster *m, const s32 aim[3], s32 dmg, s32 kick,
                      void *u)
{
    (void)m; (void)u;
    g_mel_calls++; g_mel_dmg = dmg; g_mel_kick = kick; g_mel_aim0 = aim[0];
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
int main(void)
{
    printf("Q2PSX-PC creature tests\n\n");

    test_bind();
    test_population_spawn_flags();
    test_move_lookup();
    test_endfunc_resolution();
    test_frames_drive();
    test_generic_fallback();
    test_soldier_present();
    test_actions();

    q2_cre_bind_reset();
    q2_class_table_reset();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");

    return g_failures ? 1 : 0;
}
