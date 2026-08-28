/*
 * test_ai.c — the reconstructed creature AI, checked against the executable.
 *
 * These are no longer behaviour-only assertions. The AI's constants were read
 * out of `SLES_015.34` rather than inferred from the world scale, so pinning
 * them here is the point: if a future change quietly retunes a range band or
 * a step height, that is a regression against the disc and not a judgement
 * call. Each pinned number carries the address it came from.
 *
 * The parts that genuinely remain open — how a creature module's think indices
 * map to behaviour — are not asserted, because there is nothing yet to be
 * right or wrong about.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "aimove.h"
#include "spawn.h"
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

static void check_vec3(const s32 got[3], s32 x, s32 y, s32 z,
                       const char *what)
{
    g_checks++;
    if (got[0] != x || got[1] != y || got[2] != z) {
        printf("  FAIL  %s: got {%d, %d, %d}, want {%d, %d, %d}\n",
               what, (int)got[0], (int)got[1], (int)got[2],
               (int)x, (int)y, (int)z);
        g_failures++;
    }
}

static void place(q2_monster *m, s32 x, s32 z, s32 yaw)
{
    q2_monster_init(m);
    m->in_use      = true;
    m->spawnflags |= Q2_SVFLAG_INUSE;
    m->health      = 200;
    m->max_health  = 200;
    m->pos[0] = x; m->pos[1] = 0; m->pos[2] = z;
    m->angles[2] = (s16)yaw;
    m->ideal_yaw = (s16)yaw;
}

static void make_player(q2_monster *p, s32 x, s32 z)
{
    place(p, x, z, 0);
    p->client = true;
}

/* Callback spies, so a test can prove a monsterinfo hook was reached. */
static int g_stand_calls, g_walk_calls, g_run_calls, g_idle_calls;
static int g_search_calls, g_attack_calls, g_melee_calls, g_sight_calls;
static int g_bigturn_calls;
static bool g_checkattack_result;
static int  g_checkattack_calls;

static void spy_stand(q2_monster *m)  { (void)m; g_stand_calls++;  }
static void spy_walk(q2_monster *m)   { (void)m; g_walk_calls++;   }
static void spy_run(q2_monster *m)    { (void)m; g_run_calls++;    }
static void spy_idle(q2_monster *m)   { (void)m; g_idle_calls++;   }
static void spy_search(q2_monster *m) { (void)m; g_search_calls++; }
static void spy_attack(q2_monster *m) { (void)m; g_attack_calls++; }
static void spy_melee(q2_monster *m)  { (void)m; g_melee_calls++;  }
static void spy_bigturn(q2_monster *m){ (void)m; g_bigturn_calls++;}
static void spy_sight(q2_monster *m, q2_monster *o)
{
    (void)m; (void)o; g_sight_calls++;
}
static bool spy_checkattack(q2_monster *m)
{
    (void)m;
    g_checkattack_calls++;
    return g_checkattack_result;
}

static void hook_up(q2_monster *m)
{
    m->stand  = spy_stand;
    m->walk   = spy_walk;
    m->run    = spy_run;
    m->idle   = spy_idle;
    m->search = spy_search;
    m->attack = spy_attack;
    m->melee  = spy_melee;
    m->sight  = spy_sight;
    m->checkattack = spy_checkattack;
}

static void reset_spies(void)
{
    g_stand_calls = g_walk_calls = g_run_calls = g_idle_calls = 0;
    g_search_calls = g_attack_calls = g_melee_calls = g_sight_calls = 0;
    g_bigturn_calls = g_checkattack_calls = 0;
    g_checkattack_result = false;
}

/* ------------------------------------------------------------------------- */
/* A world with one wall, so line of sight can actually fail                   */
/* ------------------------------------------------------------------------- */
static bool  g_wall_on;
static s32   g_wall_z;          /* a plane at this Z blocks everything        */
static bool  g_bottom_answer = true;

static bool walled_los(void *user, const s32 a[3], const s32 b[3])
{
    (void)user;
    if (!g_wall_on)
        return true;
    /* Crossing the plane in either direction is blocked. */
    return !((a[2] < g_wall_z && b[2] > g_wall_z)
          || (a[2] > g_wall_z && b[2] < g_wall_z));
}

static void walled_trace(void *user, const s32 start[3], const s16 mins[3],
                         const s16 maxs[3], const s32 end[3],
                         const q2_monster *ignore, u32 mask, q2_ai_trace *out)
{
    (void)user; (void)mins; (void)maxs; (void)ignore; (void)mask;

    out->allsolid   = false;
    out->startsolid = false;
    out->ent        = NULL;

    if (g_wall_on && ((start[2] < g_wall_z && end[2] > g_wall_z)
                   || (start[2] > g_wall_z && end[2] < g_wall_z))) {
        out->fraction  = Q2_TRACE_ONE / 2;
        out->endpos[0] = (start[0] + end[0]) / 2;
        out->endpos[1] = (start[1] + end[1]) / 2;
        out->endpos[2] = (start[2] + end[2]) / 2;
        return;
    }

    out->fraction  = Q2_TRACE_ONE;
    out->endpos[0] = end[0];
    out->endpos[1] = end[1];
    out->endpos[2] = end[2];
}

static bool walled_bottom(void *user, const q2_monster *m)
{
    (void)user; (void)m;
    return g_bottom_answer;
}

static const q2_ai_world g_test_world = {
    NULL, walled_trace, walled_los, walled_bottom
};

/*
 * A floor at Y = 0. SV_movestep pushes down from a step height above the wish
 * position and takes the trace's landing point, so a world that answers with
 * the floor is what proves the step logic rather than the open stand-in.
 */
static s32  g_floor_y = 0;
static s32  g_ledge_x = 0x7FFFFFFF;  /* beyond this X there is no floor */
static bool g_no_floor;              /* or no floor anywhere at all      */

static void floor_trace(void *user, const s32 start[3], const s16 mins[3],
                        const s16 maxs[3], const s32 end[3],
                        const q2_monster *ignore, u32 mask, q2_ai_trace *out)
{
    (void)user; (void)mins; (void)maxs; (void)ignore; (void)mask;

    out->allsolid   = false;
    out->startsolid = false;
    out->ent        = NULL;

    /* Only a downward probe can land; a horizontal one runs clear. */
    if (!g_no_floor && end[1] > start[1] && end[0] <= g_ledge_x
        && end[1] >= g_floor_y) {
        s32 span = end[1] - start[1];
        s32 want = g_floor_y - start[1];
        out->fraction  = (want <= 0) ? 0
                       : (s32)(((s64)want * Q2_TRACE_ONE) / span);
        out->endpos[0] = end[0];
        out->endpos[1] = g_floor_y;
        out->endpos[2] = end[2];
        if (out->fraction > Q2_TRACE_ONE)
            out->fraction = Q2_TRACE_ONE;
        return;
    }

    out->fraction  = Q2_TRACE_ONE;
    out->endpos[0] = end[0];
    out->endpos[1] = end[1];
    out->endpos[2] = end[2];
}

static bool floor_bottom(void *user, const q2_monster *m)
{
    (void)user;
    if (g_no_floor)
        return false;
    return m->pos[0] <= g_ledge_x;
}

static const q2_ai_world g_floor_world = {
    NULL, floor_trace, walled_los, floor_bottom
};

/* ------------------------------------------------------------------------- */
/* A scripted trace world for SV_movestep's retail start-solid recovery.      */
/* ------------------------------------------------------------------------- */
#define RECOVERY_TRACE_MAX 4
#define RECOVERY_EVENT_MAX 8

enum recovery_event {
    RECOVERY_EV_TRACE = 1,
    RECOVERY_EV_BOTTOM,
    RECOVERY_EV_LINK,
    RECOVERY_EV_TOUCH
};

typedef struct recovery_trace_call {
    s32 start[3];
    s32 end[3];
    const q2_monster *ignore;
    u32 mask;
} recovery_trace_call;

typedef struct recovery_spy {
    q2_ai_trace response[RECOVERY_TRACE_MAX];
    recovery_trace_call call[RECOVERY_TRACE_MAX];
    int response_count;
    int trace_count;
    int bottom_count;
    int link_count;
    int touch_count;
    int events[RECOVERY_EVENT_MAX];
    int event_count;
    bool bottom_answer;
    s32 bottom_pos[3];
    q2_monster *linked;
    u32 link_what;
} recovery_spy;

static recovery_spy g_recovery;

static void recovery_event_push(recovery_spy *s, enum recovery_event event)
{
    if (s->event_count < RECOVERY_EVENT_MAX)
        s->events[s->event_count++] = event;
}

static void recovery_trace(void *user, const s32 start[3], const s16 mins[3],
                           const s16 maxs[3], const s32 end[3],
                           const q2_monster *ignore, u32 mask,
                           q2_ai_trace *out)
{
    recovery_spy *s = (recovery_spy *)user;
    int n = s->trace_count++;

    (void)mins;
    (void)maxs;
    recovery_event_push(s, RECOVERY_EV_TRACE);

    if (n < RECOVERY_TRACE_MAX) {
        memcpy(s->call[n].start, start, sizeof(s->call[n].start));
        memcpy(s->call[n].end, end, sizeof(s->call[n].end));
        s->call[n].ignore = ignore;
        s->call[n].mask = mask;
    }

    memset(out, 0, sizeof(*out));
    if (n < s->response_count) {
        *out = s->response[n];
    } else {
        out->fraction = Q2_TRACE_ONE;
        memcpy(out->endpos, end, sizeof(out->endpos));
    }
}

static bool recovery_bottom(void *user, const q2_monster *m)
{
    recovery_spy *s = (recovery_spy *)user;
    s->bottom_count++;
    memcpy(s->bottom_pos, m->pos, sizeof(s->bottom_pos));
    recovery_event_push(s, RECOVERY_EV_BOTTOM);
    return s->bottom_answer;
}

static void recovery_link(q2_monster *m, u32 what, void *user)
{
    recovery_spy *s = (recovery_spy *)user;
    s->link_count++;
    s->linked = m;
    s->link_what = what;
    recovery_event_push(s, RECOVERY_EV_LINK);
}

static void recovery_touch(q2_monster *m, void *user)
{
    recovery_spy *s = (recovery_spy *)user;
    (void)m;
    s->touch_count++;
    recovery_event_push(s, RECOVERY_EV_TOUCH);
}

static const q2_ai_world g_recovery_world = {
    &g_recovery, recovery_trace, walled_los, recovery_bottom
};

static void recovery_reset(int responses)
{
    memset(&g_recovery, 0, sizeof(g_recovery));
    g_recovery.response_count = responses;
    g_recovery.bottom_answer = true;
}

/* ------------------------------------------------------------------------- */
static void test_constants(void)
{
    printf("constants read from the executable\n");

    /* Angles: anglemod is one instruction, `and 0xFFF`. 0x8005C7B8. */
    check_eq_i(q2_anglemod(4096), 0, "a full turn is 4096 (0x8005C7B8)");
    check_eq_i(q2_anglemod(-1), 4095, "anglemod masks rather than wraps");
    check_eq_i(Q2_ANGLE_180, 2048, "180 degrees is 2048");

    /* The world scale is 12, from six independent constants. */
    check_eq_i(Q2_RANGE_NEAR_DIST, 500 * 12, "near band is id's 500 x 12");
    check_eq_i(Q2_RANGE_MID_DIST, 1000 * 12, "mid band is id's 1000 x 12");
    check_eq_i(Q2_STEPSIZE, 18 * 12, "step height is id's 18 x 12");
    check_eq_i(Q2_CHASE_DEADBAND, 10 * 12, "chase deadband is id's 10 x 12");

    /* Content masks are id's own values, straight off the call sites. */
    check_eq_i(Q2_MASK_PLAYERSOLID, 0x02010003, "MASK_PLAYERSOLID (0x8005E808)");
    check_eq_i(Q2_MASK_MONSTERSOLID, 0x02020003, "MASK_MONSTERSOLID (0x8005FE7C)");

    /* The forward cone. 0x8005D76C. */
    check_eq_i(Q2_INFRONT_DOT, 1230, "infront threshold is 1230/4096");
}

/* ------------------------------------------------------------------------- */
static void test_range_bands(void)
{
    q2_monster m, t;

    printf("range bands\n");

    place(&m, 0, 0, 0);
    place(&t, 0, 0, 0);

    /* The immediates in the image are one less than a perfect square, so the
     * boundary is exact and worth pinning on both sides. */
    t.pos[0] = Q2_MELEE_DISTANCE - 1;
    check_eq_i(q2_range(&m, &t), Q2_RANGE_MELEE, "1019 units is melee");
    t.pos[0] = Q2_MELEE_DISTANCE;
    check_eq_i(q2_range(&m, &t), Q2_RANGE_NEAR, "1020 units is already near");

    t.pos[0] = Q2_RANGE_NEAR_DIST - 1;
    check_eq_i(q2_range(&m, &t), Q2_RANGE_NEAR, "5999 units is near");
    t.pos[0] = Q2_RANGE_NEAR_DIST;
    check_eq_i(q2_range(&m, &t), Q2_RANGE_MID, "6000 units is mid");

    t.pos[0] = Q2_RANGE_MID_DIST - 1;
    check_eq_i(q2_range(&m, &t), Q2_RANGE_MID, "11999 units is mid");
    t.pos[0] = Q2_RANGE_MID_DIST;
    check_eq_i(q2_range(&m, &t), Q2_RANGE_FAR, "12000 units is far");

    /* One class gets double melee reach — the only per-class number in the
     * band table. 0x8005D56C. */
    m.class_id = Q2_CLASS_LONG_MELEE;
    t.pos[0] = Q2_MELEE_DISTANCE_BIG - 1;
    check_eq_i(q2_range(&m, &t), Q2_RANGE_MELEE,
               "class 68 still melees at 2039 units");
    m.class_id = 1;
    check_eq_i(q2_range(&m, &t), Q2_RANGE_NEAR,
               "every other class does not");

    /* Bands must be monotonic in distance, with no gaps. */
    {
        s32 d;
        int last = Q2_RANGE_MELEE;
        int ok = 1;
        place(&m, 0, 0, 0);
        for (d = 0; d < 14000; d += 37) {
            int band;
            t.pos[0] = d;
            band = (int)q2_range(&m, &t);
            if (band < last)
                ok = 0;
            last = band;
        }
        check(ok, "bands never go backwards as distance grows");
    }
}

/* ------------------------------------------------------------------------- */
static void test_vectors(void)
{
    s32 v[3];
    s32 fwd[3], right[3];

    printf("vectors and angles\n");

    /* Yaw 0 faces +Z and a quarter turn faces +X. That convention is what
     * M_walkmove's (sin, 0, cos) move vector fixes. */
    v[0] = 0; v[1] = 0; v[2] = 1000;
    check_eq_i(q2_vectoyaw(v), 0, "+Z is yaw 0");
    v[0] = 1000; v[1] = 0; v[2] = 0;
    check_eq_i(q2_vectoyaw(v), Q2_ANGLE_90, "+X is yaw 1024");
    v[0] = 0; v[1] = 0; v[2] = -1000;
    check_eq_i(q2_vectoyaw(v), Q2_ANGLE_180, "-Z is yaw 2048");
    v[0] = -1000; v[1] = 0; v[2] = 0;
    check_eq_i(q2_vectoyaw(v), Q2_ANGLE_90 * 3, "-X is yaw 3072");

    /* Zero offset returns zero rather than an arbitrary angle. 0x8005F8FC. */
    v[0] = v[1] = v[2] = 0;
    check_eq_i(q2_vectoyaw(v), 0, "a zero offset yaws to zero");

    /* Y is the vertical axis and does not affect yaw at all. */
    v[0] = 0; v[1] = 9999; v[2] = 1000;
    check_eq_i(q2_vectoyaw(v), 0, "height does not tilt the yaw");

    v[0] = 3000; v[1] = 4000; v[2] = 0;
    check_eq_i(q2_vector_length(v), 5000, "length is a real 3D length");
    check_eq_i(q2_vector_length_sq(v), 25000000, "and its square agrees");

    /* Both saturate rather than overflow, which is observable as two distant
     * entities comparing equal. 0x8005C50C. */
    v[0] = 26755; v[1] = 0; v[2] = 0;
    check_eq_i(q2_vector_length(v), 0xB504, "an oversized component saturates");

    v[0] = 4096; v[1] = 0; v[2] = 0;
    check_eq_i(q2_vector_normalize(v), 4096, "normalize returns the old length");
    check_eq_i(v[0], 4096, "and leaves a 1.12 unit vector");

    {
        s16 a[3] = { 0, 0, Q2_ANGLE_90 };
        q2_angle_vectors(a, fwd, right, NULL);
        check_eq_i(fwd[0], 4096, "at yaw 1024 forward is +X");
        check(fwd[2] < 64 && fwd[2] > -64, "and has no Z component");
        check(right[2] < -4000, "with right a quarter turn clockwise of it");
    }
}

/* ------------------------------------------------------------------------- */
static void test_facing(void)
{
    q2_monster m, t;

    printf("facing\n");

    place(&m, 0, 0, 0);
    place(&t, 0, 5000, 0);
    check(q2_infront(&m, &t), "a target straight ahead is in front");

    t.pos[2] = -5000;
    check(!q2_infront(&m, &t), "one directly behind is not");

    /* The cone is wide — about 72 degrees either side — which is why monsters
     * spot you from well off to the axis. */
    t.pos[0] = 5000; t.pos[2] = 5000;
    check(q2_infront(&m, &t), "45 degrees off axis is still in front");

    t.pos[0] = 5000; t.pos[2] = 1800;
    check(q2_infront(&m, &t), "70 degrees off axis is still in front");

    t.pos[0] = 5000; t.pos[2] = 1000;
    check(!q2_infront(&m, &t), "79 degrees off axis is past the cone");

    t.pos[0] = 5000; t.pos[2] = -1000;
    check(!q2_infront(&m, &t), "and so is anything behind the shoulder");

    /* FacingIdeal is the 45-degree test the attack states gate on. */
    place(&m, 0, 0, 0);
    m.ideal_yaw = 0;
    check(q2_facing_ideal(&m), "already facing counts as facing");
    m.angles[2] = 512;
    check(q2_facing_ideal(&m), "45 degrees off still counts");
    m.angles[2] = 513;
    check(!q2_facing_ideal(&m), "one unit past 45 degrees does not");
    m.angles[2] = 3583;
    check(!q2_facing_ideal(&m), "and neither does the other side");
    m.angles[2] = 3584;
    check(q2_facing_ideal(&m), "back inside the cone at 3584");
}

/* ------------------------------------------------------------------------- */
static void test_turning(void)
{
    q2_monster m;

    printf("M_ChangeYaw\n");
    reset_spies();

    place(&m, 0, 0, 0);
    m.yaw_speed = 200;
    m.ideal_yaw = Q2_ANGLE_90;

    check(q2_M_ChangeYaw(&m), "a turn that has work to do reports it");
    check_eq_i(m.angles[2], 200, "and moves by exactly yaw_speed");

    /* It must take the short way round: turning from 0 to 3072 goes negative,
     * not the long way through 2048. */
    place(&m, 0, 0, 0);
    m.yaw_speed = 200;
    m.ideal_yaw = 3072;
    q2_M_ChangeYaw(&m);
    check_eq_i(m.angles[2], 4096 - 200, "the long way round is not taken");

    /* Arriving is exact, not approximate. */
    place(&m, 0, 0, 0);
    m.yaw_speed = 200;
    m.ideal_yaw = 100;
    q2_M_ChangeYaw(&m);
    check_eq_i(m.angles[2], 100, "a turn inside one step lands exactly");
    check(!q2_M_ChangeYaw(&m), "and reports nothing left to do");

    /* The PSX big-turn hook: over the threshold the callback replaces the
     * turn rather than following it. */
    place(&m, 0, 0, 0);
    m.yaw_speed = 200;
    m.ideal_yaw = Q2_ANGLE_90;
    m.bigturn_threshold = 512;
    m.bigturn = spy_bigturn;
    q2_M_ChangeYaw(&m);
    check_eq_i(g_bigturn_calls, 1, "a turn past the threshold calls the hook");
    check_eq_i(m.angles[2], 0, "and does NOT turn this tick");

    m.ideal_yaw = 100;
    g_bigturn_calls = 0;
    q2_M_ChangeYaw(&m);
    check_eq_i(g_bigturn_calls, 0, "a small turn does not");
    check_eq_i(m.angles[2], 100, "and happens normally");

    /* Turning over many ticks must converge and stay converged. */
    {
        int i;
        place(&m, 0, 0, 0);
        m.yaw_speed = 200;
        m.ideal_yaw = 1500;
        for (i = 0; i < 64; i++)
            q2_M_ChangeYaw(&m);
        check_eq_i(m.angles[2], 1500, "repeated turning settles on the ideal");
    }
}

/* ------------------------------------------------------------------------- */
static void test_acquisition(void)
{
    q2_monster m, player;

    printf("FindTarget\n");
    reset_spies();
    q2_level_reset();
    q2_ai_set_world(&g_test_world);
    g_wall_on = false;

    q2_level_state.framenum = 100;
    q2_level_state.time     = 100;

    place(&m, 0, 0, 0);
    hook_up(&m);
    make_player(&player, 0, 3000);
    q2_level_state.sight_client = &player;

    check(q2_find_target(&m), "notices a player in front and in range");
    check(m.enemy == &player, "and takes them as the enemy");
    check_eq_i(g_sight_calls, 1, "and runs the sight callback once");
    check_eq_i(g_run_calls, 1, "and starts running (HuntTarget)");

    /*
     * FoundTarget makes the monster itself the level's sight entity, and the
     * very next FindTarget reads that sighting back, sees its own enemy in it
     * and bails. That is the original's behaviour, not a defect: it is what
     * stops a pack from re-alerting each other every tick.
     */
    g_sight_calls = 0;
    check(q2_level_state.sight_entity == &m,
          "finding a client makes the monster the level's sight entity");
    check(!q2_find_target(&m), "and its own sighting does not re-alert it");

    /* With that sighting stale, the same client is reported found again and
     * FoundTarget is not repeated. */
    q2_level_state.sight_entity_framenum = 0;
    check(q2_find_target(&m), "re-finding the same enemy reports true");
    check_eq_i(g_sight_calls, 0, "but does not re-run the sight callback");

    /* Each case below starts from a level with no second-hand alerts pending,
     * or the sight-entity path answers first and masks what is being tested. */
    q2_level_state.sight_entity          = NULL;
    q2_level_state.sight_entity_framenum = 0;

    /* Out of range at all. */
    place(&m, 0, 0, 0);
    hook_up(&m);
    player.pos[2] = 40000;
    check(!q2_find_target(&m), "a player past the far band is not noticed");
    check(m.enemy == NULL, "and no enemy is taken");

    /* Behind, at mid range: the cone rejects it. */
    place(&m, 0, 0, 0);
    hook_up(&m);
    make_player(&player, 0, -8000);
    check(!q2_find_target(&m), "a player behind at mid range is not noticed");

    /* Behind at near range is also rejected while not hostile... */
    place(&m, 0, 0, 0);
    hook_up(&m);
    make_player(&player, 0, -3000);
    player.show_hostile = 0;
    check(!q2_find_target(&m), "nor one behind at near range");

    /* ...but show_hostile is exactly what gets you noticed from behind. */
    place(&m, 0, 0, 0);
    hook_up(&m);
    q2_level_state.sight_entity_framenum = 0;
    q2_level_state.sight_client = &player;
    player.show_hostile = q2_level_state.time + 10;
    check(q2_find_target(&m),
          "unless they have recently been hostile (show_hostile)");

    /* A wall blocks acquisition outright. */
    place(&m, 0, 0, 0);
    hook_up(&m);
    make_player(&player, 0, 3000);
    q2_level_state.sight_client = &player;
    q2_level_state.sight_entity_framenum = 0;
    g_wall_on = true;
    g_wall_z  = 1500;
    check(!q2_find_target(&m), "a wall in the way blocks acquisition");
    g_wall_on = false;

    /* notarget. */
    place(&m, 0, 0, 0);
    hook_up(&m);
    make_player(&player, 0, 3000);
    q2_level_state.sight_client = &player;
    q2_level_state.sight_entity_framenum = 0;
    player.flags |= Q2_FL_NOTARGET;
    check(!q2_find_target(&m), "FL_NOTARGET makes a player invisible to AI");
    player.flags &= (u16)~Q2_FL_NOTARGET;

    /* A good guy never picks a fight. */
    place(&m, 0, 0, 0);
    hook_up(&m);
    m.aiflags |= Q2_AI_GOOD_GUY;
    check(!q2_find_target(&m), "AI_GOOD_GUY never acquires");
    m.aiflags = 0;

    /* Nor does one already committed to a combat point. */
    m.aiflags |= Q2_AI_COMBAT_POINT;
    check(!q2_find_target(&m), "AI_COMBAT_POINT never acquires");
    m.aiflags = 0;

    q2_ai_set_world(NULL);
}

/* ------------------------------------------------------------------------- */
static void test_alerting(void)
{
    q2_monster m, player, noisy;

    printf("second-hand alerting and the ambush flag\n");
    reset_spies();
    q2_level_reset();
    q2_ai_set_world(&g_test_world);
    g_wall_on = false;

    q2_level_state.framenum = 100;
    q2_level_state.time     = 100;

    make_player(&player, 0, 3000);

    /* A loud noise: the monster turns toward it and takes it as a target,
     * flagged as a sound target rather than a sighting. */
    place(&m, 0, 0, 0);
    hook_up(&m);
    make_player(&noisy, 0, -3000);      /* behind, so sight cannot explain it */
    q2_level_state.sound_entity          = &noisy;
    q2_level_state.sound_entity_framenum = q2_level_state.framenum;

    check(q2_find_target(&m), "a loud noise alerts a monster behind it");
    check((m.aiflags & Q2_AI_SOUND_TARGET) != 0, "and flags it as a sound");
    check_eq_i(g_sight_calls, 0, "the sight callback does NOT run for a noise");

    /* The ambush flag suppresses the quiet channel but not the loud one. */
    place(&m, 0, 0, 0);
    hook_up(&m);
    m.spawnflags |= Q2_SPAWNFLAG_AMBUSH;
    q2_level_state.sound_entity_framenum  = 0;
    q2_level_state.sound2_entity          = &noisy;
    q2_level_state.sound2_entity_framenum = q2_level_state.framenum;
    q2_level_state.sight_client           = NULL;
    check(!q2_find_target(&m), "an ambush monster ignores a quiet noise");

    place(&m, 0, 0, 0);
    hook_up(&m);
    q2_level_state.sound2_entity_framenum = 0;
    q2_level_state.sound_entity           = &noisy;
    q2_level_state.sound_entity_framenum  = q2_level_state.framenum;
    m.spawnflags |= Q2_SPAWNFLAG_AMBUSH;
    check(q2_find_target(&m), "but still wakes for a loud one it can see");

    /* Stale alerts are ignored: the framenum window is one tick wide. */
    place(&m, 0, 0, 0);
    hook_up(&m);
    q2_level_state.sound_entity_framenum = q2_level_state.framenum - 5;
    q2_level_state.sight_client = NULL;
    check(!q2_find_target(&m), "an alert more than a tick old is stale");

    q2_ai_set_world(NULL);
}

/* ------------------------------------------------------------------------- */
static void test_checkattack(void)
{
    q2_monster m, player;

    printf("ai_checkattack\n");
    reset_spies();
    q2_level_reset();
    q2_ai_set_world(&g_test_world);
    g_wall_on = false;

    q2_level_state.framenum = 100;
    q2_level_state.time     = 100;

    /* A dead enemy with nothing else to do sends the creature back to
     * standing, with the pause that stops it wandering. */
    place(&m, 0, 0, 0);
    hook_up(&m);
    make_player(&player, 0, 3000);
    m.enemy = &player;
    m.goalentity = &player;
    player.health = 0;

    check(q2_ai_checkattack(&m, 0), "a dead enemy takes over the tick");
    check(m.enemy == NULL, "and is dropped");
    check_eq_i(g_stand_calls, 1, "the creature goes back to standing");
    check(m.pausetime > q2_level_state.time + 100000000,
          "with the pause that stops it hunting the world");

    /* An old enemy is picked back up rather than standing down. */
    place(&m, 0, 0, 0);
    hook_up(&m);
    {
        q2_monster old;
        make_player(&old, 0, 2000);
        old.health = 50;
        make_player(&player, 0, 3000);
        player.health = 0;
        m.enemy = &player;
        m.oldenemy = &old;
        m.goalentity = &player;

        /* Unlike the stand-down case this does NOT take over the tick: the
         * original falls straight through to the visibility and range work
         * with the resumed enemy in hand. */
        g_checkattack_result = false;
        check(!q2_ai_checkattack(&m, 0),
              "resuming an old enemy falls through to the normal path");
        check(m.enemy == &old, "and the old enemy is resumed");
        check(m.oldenemy == NULL, "and cleared from the slot");
        check_eq_i(g_run_calls, 1, "with the creature set running at it");
    }

    /* AI_BRUTAL keeps hitting a corpse until it is well past dead. */
    place(&m, 0, 0, 0);
    hook_up(&m);
    make_player(&player, 0, 3000);
    m.enemy = &player;
    m.goalentity = &player;
    m.aiflags |= Q2_AI_BRUTAL;
    player.health = -50;
    g_checkattack_result = false;
    q2_ai_checkattack(&m, 0);
    check(m.enemy == &player, "AI_BRUTAL still fights at -50 health");
    player.health = -80;
    q2_ai_checkattack(&m, 0);
    check(m.enemy == NULL, "and gives up at -80");

    /* The published globals are what a creature's own checkattack reads. */
    reset_spies();
    place(&m, 0, 0, 0);
    hook_up(&m);
    make_player(&player, 0, 3000);
    player.health = 100;
    m.enemy = &player;
    m.goalentity = &player;
    g_checkattack_result = false;
    q2_ai_checkattack(&m, 0);
    check(q2_enemy_vis, "an unobstructed enemy is visible");
    check(q2_enemy_infront, "and in front");
    check_eq_i(q2_enemy_range, Q2_RANGE_NEAR, "and at near range");
    check_eq_i(q2_enemy_yaw, 0, "with the yaw toward it published");
    check_eq_i(g_checkattack_calls, 1,
               "and the creature's own checkattack is consulted");

    /* Nothing attacks what it cannot see. */
    reset_spies();
    g_wall_on = true;
    g_wall_z  = 1500;
    q2_ai_checkattack(&m, 0);
    check(!q2_enemy_vis, "a blocked enemy is not visible");
    check_eq_i(g_checkattack_calls, 0,
               "and the creature's checkattack is never reached");
    g_wall_on = false;

    /* AS_MISSILE fires once facing, and resets the state so it does not
     * fire every tick. */
    reset_spies();
    place(&m, 0, 0, 0);
    hook_up(&m);
    make_player(&player, 0, 3000);
    m.enemy = &player;
    m.goalentity = &player;
    m.attack_state = Q2_AS_MISSILE;
    check(q2_ai_checkattack(&m, 0), "AS_MISSILE takes over the tick");
    check_eq_i(g_attack_calls, 1, "and fires");
    check_eq_i(m.attack_state, Q2_AS_STRAIGHT, "then drops back to straight");

    reset_spies();
    m.attack_state = Q2_AS_MELEE;
    check(q2_ai_checkattack(&m, 0), "AS_MELEE takes over the tick");
    check_eq_i(g_melee_calls, 1, "and swings");
    check_eq_i(m.attack_state, Q2_AS_STRAIGHT, "then drops back to straight");

    /* Facing gates both: a creature aimed the wrong way turns first. */
    reset_spies();
    place(&m, 0, 0, 0);
    hook_up(&m);
    m.angles[2] = Q2_ANGLE_180;
    m.yaw_speed = 100;
    make_player(&player, 0, 3000);
    m.enemy = &player;
    m.goalentity = &player;
    m.attack_state = Q2_AS_MISSILE;
    q2_ai_checkattack(&m, 0);
    check_eq_i(g_attack_calls, 0, "facing away, nothing fires");
    check(m.angles[2] != Q2_ANGLE_180, "but the creature turns toward");

    q2_ai_set_world(NULL);
}

/* ------------------------------------------------------------------------- */
static void test_stand_and_walk(void)
{
    q2_monster m, player;

    printf("ai_stand and ai_walk\n");
    reset_spies();
    q2_level_reset();
    q2_ai_set_world(&g_test_world);
    g_wall_on = false;
    srand(1);

    q2_level_state.framenum = 100;
    q2_level_state.time     = 100;

    /* Past the pause, standing turns into walking. */
    place(&m, 0, 0, 0);
    hook_up(&m);
    m.pausetime = 50;
    q2_level_state.sight_client = NULL;
    q2_ai_stand(&m, 0);
    check_eq_i(g_walk_calls, 1, "past its pausetime a creature starts walking");

    /* Inside the pause, it idles instead — and the very first tick only arms
     * the timer, so it does not grunt the instant the level starts. */
    reset_spies();
    place(&m, 0, 0, 0);
    hook_up(&m);
    m.pausetime = 100000;
    m.idle_time = 0;
    q2_ai_stand(&m, 0);
    check_eq_i(g_idle_calls, 0, "the first idle tick only arms the timer");
    check(m.idle_time > 0, "and the timer is armed");

    m.idle_time = q2_level_state.time - 1;
    q2_ai_stand(&m, 0);
    check_eq_i(g_idle_calls, 1, "the next one plays the idle sound");
    check(m.idle_time >= q2_level_state.time + 150,
          "and re-arms at least 15 seconds out");
    check(m.idle_time <= q2_level_state.time + 300,
          "and at most 30");

    /* AI_TEMP_STAND_GROUND releases the moment the creature has to turn. */
    reset_spies();
    place(&m, 0, 0, 0);
    hook_up(&m);
    make_player(&player, 5000, 0);      /* off to one side */
    m.enemy = &player;
    m.goalentity = &player;
    player.health = 100;
    m.aiflags |= Q2_AI_STAND_GROUND | Q2_AI_TEMP_STAND_GROUND;
    q2_ai_stand(&m, 0);
    check_eq_i(g_run_calls, 1, "a temporary stand-ground breaks on a turn");
    check((m.aiflags & (Q2_AI_STAND_GROUND | Q2_AI_TEMP_STAND_GROUND)) == 0,
          "and both flags clear together");

    /* A permanent stand-ground does not. */
    reset_spies();
    place(&m, 0, 0, 0);
    hook_up(&m);
    m.enemy = &player;
    m.goalentity = &player;
    m.aiflags |= Q2_AI_STAND_GROUND;
    q2_ai_stand(&m, 0);
    check_eq_i(g_run_calls, 0, "a permanent stand-ground holds its post");
    check((m.aiflags & Q2_AI_STAND_GROUND) != 0, "and keeps the flag");

    /* ai_walk uses `search`, not `idle` — two different sounds. */
    reset_spies();
    place(&m, 0, 0, 0);
    hook_up(&m);
    q2_level_state.sight_client = NULL;
    m.idle_time = q2_level_state.time - 1;
    q2_ai_walk(&m, 0);
    check_eq_i(g_search_calls, 1, "walking plays the search sound");
    check_eq_i(g_idle_calls, 0, "and never the idle one");

    q2_ai_set_world(NULL);
}

/* ------------------------------------------------------------------------- */
static void test_movement_verbs(void)
{
    q2_monster m, player;
    int i;

    printf("movement verbs\n");
    reset_spies();
    q2_level_reset();
    q2_ai_set_world(&g_floor_world);
    g_ledge_x = 0x7FFFFFFF;
    g_floor_y = 0;
    srand(3);

    q2_level_state.framenum = 100;
    q2_level_state.time     = 100;

    /* ai_move does not turn: it is for pain and death animations. */
    place(&m, 0, 0, 0);
    m.ideal_yaw = Q2_ANGLE_90;
    q2_ai_move(&m, 1000);
    check_eq_i(m.angles[2], 0, "ai_move does not turn the creature");
    check(m.pos[2] > 0, "but does advance it along its facing");

    /* ai_charge turns toward the enemy and closes. */
    place(&m, 0, 0, 0);
    m.yaw_speed = 4096;
    make_player(&player, 0, 6000);
    m.enemy = &player;
    for (i = 0; i < 5; i++)
        q2_ai_charge(&m, 500);
    check(m.pos[2] > 0, "charging closes the distance");
    check_eq_i(m.angles[2], 0, "while facing the enemy");

    /* AI_MANUAL_STEERING leaves the facing to the creature's own code. */
    place(&m, 0, 0, 0);
    m.yaw_speed = 4096;
    m.aiflags |= Q2_AI_MANUAL_STEERING;
    m.ideal_yaw = Q2_ANGLE_90;
    make_player(&player, 0, 6000);
    m.enemy = &player;
    q2_ai_charge(&m, 500);
    check_eq_i(m.ideal_yaw, Q2_ANGLE_90,
               "AI_MANUAL_STEERING keeps the creature's own ideal yaw");

    /* M_walkmove moves along the yaw given, not the creature's own. */
    place(&m, 0, 0, 0);
    check(q2_M_walkmove(&m, Q2_ANGLE_90, 1200), "a clear walkmove succeeds");
    check(m.pos[0] > 1000, "and moves along +X for yaw 1024");
    check(m.pos[2] > -64 && m.pos[2] < 64, "with no Z component");

    /* A ledge stops a walker, because M_CheckBottom refuses the landing. */
    place(&m, 0, 0, 0);
    g_ledge_x = 500;
    check(!q2_M_walkmove(&m, Q2_ANGLE_90, 2000), "a ledge refuses the step");
    check_eq_i(m.pos[0], 0, "and the creature does not move at all");

    /* Unless its floor was pulled out from under it. */
    m.flags |= Q2_FL_PARTIALGROUND;
    check(q2_M_walkmove(&m, Q2_ANGLE_90, 2000),
          "a partially grounded creature is allowed to fall");
    g_ledge_x = 0x7FFFFFFF;

    /* Flying creatures never step: they move directly and correct height. */
    place(&m, 0, 0, 0);
    m.flags |= Q2_FL_FLY;
    make_player(&player, 0, 6000);
    player.pos[1] = -2000;              /* Y is down, so this is above */
    m.enemy = &player;
    m.goalentity = &player;
    q2_M_walkmove(&m, 0, 1200);
    check(m.pos[1] < 0, "a flyer chasing something above it climbs");
    check(m.pos[2] > 1000, "while still closing the distance");

    q2_ai_set_world(NULL);
}

/* ------------------------------------------------------------------------- */
static void test_movestep_trace_paths(void)
{
    q2_monster m;
    s32 move[3];
    bool moved;
    int i;

    printf("SV_movestep ordinary and start-solid trace paths\n");
    q2_ai_set_world(&g_recovery_world);
    q2_ai_set_link_hooks(recovery_link, recovery_touch, &g_recovery);

    /*
     * The ordinary arm carries the ACTUAL lift delta into origin+move at
     * 0x8005FFCC..0x8005FFE0. Its drop still ends at original-Y + stepsize,
     * and 0x800600D8..0x800600E8 decrements the accepted trace endpoint.
     */
    recovery_reset(3);
    g_recovery.response[0].fraction = Q2_TRACE_ONE / 2;
    g_recovery.response[0].endpos[0] = 100;
    g_recovery.response[0].endpos[1] = 100;
    g_recovery.response[0].endpos[2] = 300;
    g_recovery.response[1].fraction = Q2_TRACE_ONE;
    g_recovery.response[1].endpos[0] = 500;
    g_recovery.response[1].endpos[1] = 124;
    g_recovery.response[1].endpos[2] = 800;
    g_recovery.response[2].fraction = Q2_TRACE_ONE / 2;
    g_recovery.response[2].endpos[0] = 500;
    g_recovery.response[2].endpos[1] = 300;
    g_recovery.response[2].endpos[2] = 800;

    place(&m, 100, 300, 0);
    m.pos[1] = 200;
    move[0] = 400;
    move[1] = 24;
    move[2] = 500;
    moved = q2_SV_movestep(&m, move, false);

    check(moved, "ordinary step succeeds after a partial lift");
    check_eq_i(g_recovery.trace_count, 3,
               "ordinary step makes lift, move and drop traces");
    check_vec3(g_recovery.call[0].start, 100, 200, 300,
               "ordinary lift starts at origin");
    check_vec3(g_recovery.call[0].end, 100, 200 - Q2_STEPSIZE, 300,
               "ordinary lift requests one full stepsize");
    check_vec3(g_recovery.call[1].start, 100, 100, 300,
               "ordinary move starts at the actual partial-lift endpoint");
    check_vec3(g_recovery.call[1].end, 500, 124, 800,
               "ordinary wish Y combines move[1] with the lift delta");
    check_vec3(g_recovery.call[2].start, 500, 124, 800,
               "ordinary drop starts at the carried wish position");
    check_vec3(g_recovery.call[2].end, 500, 200 + Q2_STEPSIZE, 800,
               "ordinary drop excludes move[1] from its end Y");
    check_vec3(m.pos, 500, 299, 800,
               "ordinary landing stores trace end Y minus one");
    check_vec3(g_recovery.bottom_pos, 500, 299, 800,
               "bottom check observes the decremented landing position");
    check(m.on_ground, "ordinary accepted landing sets on-ground state");
    check_eq_i(g_recovery.event_count, 4,
               "ordinary path traces three times then checks support");
    check_eq_i(g_recovery.events[0], RECOVERY_EV_TRACE,
               "ordinary event 1 is lift");
    check_eq_i(g_recovery.events[1], RECOVERY_EV_TRACE,
               "ordinary event 2 is move");
    check_eq_i(g_recovery.events[2], RECOVERY_EV_TRACE,
               "ordinary event 3 is drop");
    check_eq_i(g_recovery.events[3], RECOVERY_EV_BOTTOM,
               "ordinary support check follows the decremented landing");
    check_eq_i(g_recovery.link_count + g_recovery.touch_count, 0,
               "relink=false keeps ordinary path side-effect free");

    /* 0x8005FFE4..0x80060010 bypass the middle trace when move is all zero,
     * leaving the lift result in `tr` for the exact-fraction gate. A partial
     * lift must therefore fail immediately rather than being hidden by a
     * synthetic zero-length trace that reports 4096. */
    recovery_reset(1);
    g_recovery.response[0].fraction = Q2_TRACE_ONE / 2;
    g_recovery.response[0].endpos[0] = 10;
    g_recovery.response[0].endpos[1] = -88;
    g_recovery.response[0].endpos[2] = 30;

    place(&m, 10, 30, 0);
    m.pos[1] = 20;
    move[0] = move[1] = move[2] = 0;
    moved = q2_SV_movestep(&m, move, false);

    check(!moved, "ordinary zero move preserves a partial-lift rejection");
    check_eq_i(g_recovery.trace_count, 1,
               "partial zero move stops after the lift trace");
    check_vec3(m.pos, 10, 20, 30,
               "rejected zero move leaves the origin unchanged");
    check_eq_i(g_recovery.bottom_count, 0,
               "rejected zero move never checks a landing");

    /* A fully clear lift does proceed, but directly to the drop: exactly two
     * traces and no lifted->lifted middle query. */
    recovery_reset(2);
    g_recovery.response[0].fraction = Q2_TRACE_ONE;
    g_recovery.response[0].endpos[0] = 10;
    g_recovery.response[0].endpos[1] = 20 - Q2_STEPSIZE;
    g_recovery.response[0].endpos[2] = 30;
    g_recovery.response[1].fraction = Q2_TRACE_ONE / 2;
    g_recovery.response[1].endpos[0] = 10;
    g_recovery.response[1].endpos[1] = 80;
    g_recovery.response[1].endpos[2] = 30;

    place(&m, 10, 30, 0);
    m.pos[1] = 20;
    moved = q2_SV_movestep(&m, move, false);

    check(moved, "ordinary clear zero move reaches its drop");
    check_eq_i(g_recovery.trace_count, 2,
               "clear zero move makes lift and drop traces only");
    check_vec3(g_recovery.call[1].start,
               10, 20 - Q2_STEPSIZE, 30,
               "zero-move drop starts at the lifted endpoint");
    check_vec3(g_recovery.call[1].end,
               10, 20 + Q2_STEPSIZE, 30,
               "zero-move drop ends below the original position");
    check_vec3(m.pos, 10, 79, 30,
               "zero-move landing keeps the ordinary minus-one bias");
    check_eq_i(g_recovery.bottom_count, 1,
               "accepted zero move checks supporting corners once");

    /*
     * 0x8005FFC4 branches on the FIRST (lift) trace's startsolid flag. The
     * retail arm retries origin -> origin+move, then traces downward from that
     * unstepped wish position. Keep move[1] non-zero: the end built at
     * 0x8005FF58..0x8005FF5C remains original-Y + stepsize when reused by the
     * recovery trace at 0x80060214, not wish-Y + stepsize.
     */
    recovery_reset(3);
    g_recovery.response[0].startsolid = true;
    g_recovery.response[0].allsolid = true;
    g_recovery.response[1].fraction = Q2_TRACE_ONE;
    g_recovery.response[1].endpos[0] = 500;
    g_recovery.response[1].endpos[1] = 224;
    g_recovery.response[1].endpos[2] = 800;
    g_recovery.response[2].fraction = Q2_TRACE_ONE / 2;
    g_recovery.response[2].endpos[0] = 500;
    g_recovery.response[2].endpos[1] = 320;
    g_recovery.response[2].endpos[2] = 800;

    place(&m, 100, 300, 0);
    m.pos[1] = 200;
    move[0] = 400;
    move[1] = 24;
    move[2] = 500;
    moved = q2_SV_movestep(&m, move, true);

    check(moved, "a clear retail retry recovers an initial start-solid lift");
    check_eq_i(g_recovery.trace_count, 3,
               "recovery makes lift, direct-move and descent traces");
    check_vec3(g_recovery.call[0].start, 100, 200, 300,
               "lift starts at the unchanged origin");
    check_vec3(g_recovery.call[0].end, 100, 200 - Q2_STEPSIZE, 300,
               "lift probes one retail step upward");
    check_vec3(g_recovery.call[1].start, 100, 200, 300,
               "direct retry starts at the unchanged origin");
    check_vec3(g_recovery.call[1].end, 500, 224, 800,
               "direct retry includes all three move components");
    check_vec3(g_recovery.call[2].start, 500, 224, 800,
               "recovery descent starts at the unstepped wish position");
    check_vec3(g_recovery.call[2].end, 500, 200 + Q2_STEPSIZE, 800,
               "recovery descent ends at original-Y plus stepsize");
    check_vec3(m.pos, 500, 320, 800,
               "successful recovery installs the descent endpoint");
    check(m.on_ground, "successful recovery restores on-ground state");
    check_eq_i(g_recovery.bottom_count, 1,
               "recovered position is checked for supporting corners");
    check_eq_i(g_recovery.link_count, 1,
               "successful relink recovery links exactly once");
    check_eq_i(g_recovery.touch_count, 1,
               "successful relink recovery touches triggers exactly once");
    check(g_recovery.linked == &m, "the recovered monster is the linked entity");
    check_eq_i(g_recovery.link_what, 0x81,
               "recovery uses retail origin/relink flags");
    check_eq_i(g_recovery.event_count, 6,
               "clear recovery has six ordered world events");
    check_eq_i(g_recovery.events[0], RECOVERY_EV_TRACE,
               "event 1 is the lift trace");
    check_eq_i(g_recovery.events[1], RECOVERY_EV_TRACE,
               "event 2 is the direct retry trace");
    check_eq_i(g_recovery.events[2], RECOVERY_EV_TRACE,
               "event 3 is the recovery descent");
    check_eq_i(g_recovery.events[3], RECOVERY_EV_BOTTOM,
               "support is checked after all traces");
    check_eq_i(g_recovery.events[4], RECOVERY_EV_LINK,
               "link follows the support check");
    check_eq_i(g_recovery.events[5], RECOVERY_EV_TOUCH,
               "trigger touching is last");
    for (i = 0; i < 3; i++) {
        check(g_recovery.call[i].ignore == &m,
              "every recovery trace ignores the moving monster");
        check_eq_i(g_recovery.call[i].mask, Q2_MASK_MONSTERSOLID,
                   "every recovery trace uses MASK_MONSTERSOLID");
    }

    /* 0x800601B0 rejects a direct retry one fraction unit short of clear. */
    recovery_reset(2);
    g_recovery.response[0].startsolid = true;
    g_recovery.response[1].fraction = Q2_TRACE_ONE - 1;
    g_recovery.response[1].endpos[0] = 109;
    g_recovery.response[1].endpos[1] = 20;
    g_recovery.response[1].endpos[2] = 229;

    place(&m, 10, 30, 0);
    m.pos[1] = 20;
    move[0] = 100;
    move[1] = 0;
    move[2] = 200;
    moved = q2_SV_movestep(&m, move, true);

    check(!moved, "a clipped direct retry cannot recover start-solid");
    check_eq_i(g_recovery.trace_count, 2,
               "a clipped retry stops before the recovery descent");
    check_vec3(m.pos, 10, 20, 30,
               "a clipped retry leaves origin byte-for-byte unchanged");
    check(!m.on_ground, "a failed retry does not change ground state");
    check_eq_i(g_recovery.bottom_count, 0,
               "a clipped retry never checks the rejected destination");
    check_eq_i(g_recovery.link_count, 0,
               "a clipped retry is never linked");
    check_eq_i(g_recovery.touch_count, 0,
               "a clipped retry never touches triggers");
    check_eq_i(g_recovery.event_count, 2,
               "clipped retry has exactly two trace events");

    /* A clear direct retry can still be rejected by a start-solid descent. */
    recovery_reset(3);
    g_recovery.response[0].startsolid = true;
    g_recovery.response[1].fraction = Q2_TRACE_ONE;
    g_recovery.response[2].startsolid = true;
    g_recovery.response[2].allsolid = true;

    place(&m, 10, 30, 0);
    m.pos[1] = 20;
    moved = q2_SV_movestep(&m, move, true);

    check(!moved, "a start-solid recovery descent rejects the move");
    check_eq_i(g_recovery.trace_count, 3,
               "blocked descent happens after the clear direct retry");
    check_vec3(m.pos, 10, 20, 30,
               "a blocked descent also preserves the original position");
    check_eq_i(g_recovery.bottom_count, 0,
               "a blocked descent does not run the bottom check");
    check_eq_i(g_recovery.link_count + g_recovery.touch_count, 0,
               "a blocked descent has no relink side effects");

    /* 0x80060128..0x80060150 skip the direct retry for an all-zero move. */
    recovery_reset(2);
    g_recovery.response[0].startsolid = true;
    g_recovery.response[1].fraction = Q2_TRACE_ONE / 2;
    g_recovery.response[1].endpos[0] = 10;
    g_recovery.response[1].endpos[1] = 80;
    g_recovery.response[1].endpos[2] = 30;

    place(&m, 10, 30, 0);
    m.pos[1] = 20;
    move[0] = move[1] = move[2] = 0;
    moved = q2_SV_movestep(&m, move, false);

    check(moved, "a zero move can recover through its descent alone");
    check_eq_i(g_recovery.trace_count, 2,
               "zero move makes only the lift and descent traces");
    check_vec3(g_recovery.call[1].start, 10, 20, 30,
               "zero-move recovery descent starts at origin");
    check_vec3(g_recovery.call[1].end, 10, 20 + Q2_STEPSIZE, 30,
               "zero-move recovery still descends one stepsize");
    check_eq_i(g_recovery.event_count, 3,
               "zero-move recovery traces twice then checks support");
    check_eq_i(g_recovery.events[2], RECOVERY_EV_BOTTOM,
               "zero-move support check follows both traces");
    check_eq_i(g_recovery.link_count + g_recovery.touch_count, 0,
               "relink=false suppresses zero-move link side effects");

    q2_ai_set_link_hooks(NULL, NULL, NULL);
    q2_ai_set_world(NULL);
}

/* ------------------------------------------------------------------------- */
static void test_chase_directions(void)
{
    q2_monster m, goal;
    int i;

    printf("SV_NewChaseDir and SV_StepDirection\n");
    q2_level_reset();
    q2_ai_set_world(&g_floor_world);
    g_ledge_x = 0x7FFFFFFF;
    srand(7);

    /* A step that would leave the creature facing more than 45 degrees off
     * the direction it asked for is thrown away — but still reports true. */
    place(&m, 0, 0, 0);
    m.yaw_speed = 10;                   /* far too slow to turn in one tick */
    check(q2_SV_StepDirection(&m, Q2_ANGLE_180, 1200),
          "a blocked-by-facing step still reports a move");
    check_eq_i(m.pos[2], 0, "but the position is put back");

    /* With enough turn rate the same step lands. */
    place(&m, 0, 0, 0);
    m.yaw_speed = 4096;
    check(q2_SV_StepDirection(&m, Q2_ANGLE_180, 1200), "a fast turner steps");
    check(m.pos[2] < -1000, "and moves along -Z for yaw 2048");

    /* Chasing something on +X should end up moving on +X. */
    place(&m, 0, 0, 0);
    m.yaw_speed = 4096;
    place(&goal, 20000, 0, 0);
    m.goalentity = &goal;
    for (i = 0; i < 40; i++)
        q2_M_MoveToGoal(&m, 400);
    check(m.pos[0] > 2000, "chasing a goal on +X moves toward it");

    /* The deadband: a goal inside 120 units on an axis produces no direction
     * on that axis at all. */
    place(&m, 0, 0, 0);
    m.yaw_speed = 4096;
    place(&goal, 100, 20000, 0);
    q2_SV_NewChaseDir(&m, &goal, 400);
    check(m.pos[2] > 0, "inside the deadband on X, the chase goes on Z alone");

    /* Close enough: a creature already touching its enemy does not shuffle. */
    place(&m, 0, 0, 0);
    place(&goal, 200, 0, 0);
    m.enemy = &goal;
    m.goalentity = &goal;
    check(q2_SV_CloseEnough(&m, &goal, 100),
          "overlapping boxes are close enough");
    {
        s32 before = m.pos[0];
        q2_M_MoveToGoal(&m, 400);
        check_eq_i(m.pos[0], before, "and no move is attempted");
    }

    /* A creature with nowhere to stand is marked partially grounded so the
     * step code will let it fall rather than freezing it. */
    place(&m, 0, 0, 0);
    place(&goal, 20000, 0, 0);
    g_no_floor = true;
    q2_SV_NewChaseDir(&m, &goal, 400);
    check((m.flags & Q2_FL_PARTIALGROUND) != 0,
          "a creature with no footing is marked partially grounded");
    g_no_floor = false;

    q2_ai_set_world(NULL);
}

/* ------------------------------------------------------------------------- */
static void test_lost_sight(void)
{
    q2_monster m, player;
    int i;

    printf("losing the player: last sighting, trail and corners\n");
    reset_spies();
    q2_level_reset();
    q2_ai_set_world(&g_test_world);
    g_wall_on = false;
    srand(11);

    q2_level_state.framenum = 100;
    q2_level_state.time     = 100;

    place(&m, 0, 0, 0);
    hook_up(&m);
    m.yaw_speed = 4096;
    make_player(&player, 0, 6000);
    player.health = 100;
    m.enemy = &player;
    m.goalentity = &player;

    /* While visible, the last sighting tracks the player exactly. */
    q2_ai_run(&m, 400);
    check_eq_i(m.last_sighting[2], 6000, "a visible enemy updates last_sighting");
    check((m.aiflags & Q2_AI_LOST_SIGHT) == 0, "and clears AI_LOST_SIGHT");
    check_eq_i(m.trail_time, q2_level_state.time, "and stamps the trail time");

    /* Break line of sight and the creature commits to the last known spot. */
    g_wall_on = true;
    g_wall_z  = 3000;
    q2_ai_run(&m, 400);
    check((m.aiflags & Q2_AI_LOST_SIGHT) != 0, "losing sight sets AI_LOST_SIGHT");
    check((m.aiflags & Q2_AI_PURSUIT_LAST_SEEN) != 0,
          "and aims it at the last place it saw them");
    check((m.aiflags & (Q2_AI_PURSUE_NEXT | Q2_AI_PURSUE_TEMP)) == 0,
          "clearing the two pursuit-continuation flags");

    /* Walking onto the last sighting asks for the next waypoint. */
    place(&m, 0, 0, 0);
    hook_up(&m);
    m.yaw_speed = 4096;
    g_wall_on = true;
    g_wall_z  = 1000;                  /* the player stays out of sight */
    m.enemy = &player;
    m.goalentity = &player;
    m.aiflags |= Q2_AI_LOST_SIGHT;
    m.last_sighting[0] = 0;
    m.last_sighting[1] = 0;
    m.last_sighting[2] = 10;           /* all but standing on it */
    q2_ai_run(&m, 400);
    check((m.aiflags & Q2_AI_PURSUE_NEXT) != 0,
          "arriving at the waypoint asks for the next one");
    g_wall_on = false;

    /* The trail: laying spots and picking them back up. */
    q2_trail_init();
    check(q2_trail_pick_first(&m) == NULL, "an empty trail has no first spot");

    for (i = 0; i < 4; i++) {
        s32 p[3];
        p[0] = 0; p[1] = 0; p[2] = 1000 * (i + 1);
        q2_level_state.time = 200 + i;
        q2_trail_add(p, (s16)0);
    }
    q2_level_state.time = 300;

    place(&m, 0, 0, 0);
    m.trail_time = 0;
    {
        const q2_trail_spot *s = q2_trail_pick_first(&m);
        check(s != NULL, "a laid trail yields a spot");
        if (s)
            check(s->timestamp >= 200, "and it is one that was actually laid");
    }

    /*
     * A creature already past every spot wraps back to the head rather than
     * being told the trail is exhausted — the original only answers NULL when
     * no trail has been laid at all.
     */
    m.trail_time = 100000;
    {
        const q2_trail_spot *s = q2_trail_pick_next(&m);
        check(s != NULL, "a creature past the whole trail wraps to the head");
        if (s)
            check_eq_i(s->origin[2], 4000, "which is the newest spot laid");
    }

    /* Corner peeking: with the straight line blocked, ai_run picks a sidestep
     * waypoint and flags it as temporary. */
    reset_spies();
    q2_trail_init();
    q2_level_reset();
    q2_level_state.framenum = 100;
    q2_level_state.time     = 100;
    place(&m, 0, 0, 0);
    hook_up(&m);
    m.yaw_speed = 4096;
    make_player(&player, 0, 6000);
    player.health = 100;
    m.enemy = &player;
    m.goalentity = &player;
    g_wall_on = true;
    g_wall_z  = 3000;
    q2_ai_run(&m, 400);            /* first tick: notices the loss */
    q2_ai_run(&m, 400);            /* second: walks the memory     */
    check((m.aiflags & Q2_AI_LOST_SIGHT) != 0,
          "a creature behind a wall stays in lost-sight pursuit");
    g_wall_on = false;

    q2_ai_set_world(NULL);
}

/* ------------------------------------------------------------------------- */
static int g_think_hits[8];
static int g_verb_hits[8];

static void think0(q2_monster *m) { (void)m; g_think_hits[0]++; }
static void think3(q2_monster *m) { (void)m; g_think_hits[3]++; }
static void verb1(q2_monster *m)  { (void)m; g_verb_hits[1]++;  }

static int g_endfunc_hits;
static void endfunc(q2_monster *m) { (void)m; g_endfunc_hits++; }

static void test_frame_driver(void)
{
    q2_monster m;
    /* ai byte 3 is the shared run verb; 0x81 is the creature's own verb 1. */
    static const q2_mframe frames[4] = {
        { 0, 0, 0 },
        { 0, 0, 3 },
        { 0x81, 0, 0 },
        { 0, 0, 0 }
    };
    q2_mmove move;

    printf("M_MoveFrame and the class method table\n");
    q2_level_reset();
    q2_ai_set_world(&g_test_world);
    g_wall_on = false;
    memset(g_think_hits, 0, sizeof(g_think_hits));
    memset(g_verb_hits, 0, sizeof(g_verb_hits));
    g_endfunc_hits = 0;

    q2_class_table_reset();
    q2_class_think_set(5, 0, think0);
    q2_class_think_set(5, 3, think3);
    q2_class_verb_set(5, 1, verb1);

    memset(&move, 0, sizeof(move));
    move.first_frame = 10;
    move.last_frame  = 13;
    move.frames      = frames;
    move.endfunc     = endfunc;

    place(&m, 0, 0, 0);
    m.class_id    = 5;
    m.currentmove = &move;
    m.frame       = 0;              /* outside the move */

    q2_M_MoveFrame(&m);
    check_eq_i(m.frame, 10, "a frame outside the move snaps to its first");

    q2_M_MoveFrame(&m);
    check_eq_i(m.frame, 11, "and then advances by one");
    check_eq_i(g_think_hits[3], 1, "running the frame's think index");

    q2_M_MoveFrame(&m);
    check_eq_i(m.frame, 12, "advancing again");
    check_eq_i(g_verb_hits[1], 1,
               "an ai byte with bit 7 set calls the creature's own verb");

    q2_M_MoveFrame(&m);
    check_eq_i(m.frame, 13, "reaching the last frame");
    check_eq_i(g_endfunc_hits, 0, "the endfunc has not fired yet");

    q2_M_MoveFrame(&m);
    check_eq_i(g_endfunc_hits, 1, "it fires on the tick AFTER the last frame");
    check_eq_i(m.frame, 10, "and the move loops back to its first");

    /* nextframe overrides the advance, once. */
    m.frame     = 10;
    m.nextframe = 12;
    q2_M_MoveFrame(&m);
    check_eq_i(m.frame, 12, "nextframe jumps straight to that frame");
    check_eq_i(m.nextframe, 0, "and is consumed");

    /* A nextframe outside the move is ignored rather than obeyed. */
    m.frame     = 10;
    m.nextframe = 99;
    q2_M_MoveFrame(&m);
    check_eq_i(m.frame, 11, "a nextframe outside the move is ignored");

    /* AI_HOLD_FRAME freezes the animation. */
    m.frame     = 11;
    m.nextframe = 0;
    m.aiflags  |= Q2_AI_HOLD_FRAME;
    q2_M_MoveFrame(&m);
    check_eq_i(m.frame, 11, "AI_HOLD_FRAME freezes the frame");
    m.aiflags &= ~(u32)Q2_AI_HOLD_FRAME;

    /* An unregistered class is inert, not fatal. */
    m.class_id = 200;
    m.frame    = 10;
    q2_M_MoveFrame(&m);
    check(true, "an unregistered class runs without crashing");

    /* The think re-arms itself every tick. */
    q2_level_state.framenum = 500;
    m.class_id = 5;
    q2_M_MoveFrame(&m);
    check_eq_i(m.next_think, 501, "the think re-arms for the next tick");

    /*
     * A corpse animates and does not think. The set tick used to skip anything
     * with `dead` set, which left a killed creature standing in whatever pose
     * the shot caught it in — drawn, because the draw loop only checks
     * `in_use`, but frozen mid-stride forever.
     */
    {
        q2_monster_set set;
        q2_monster *d;

        memset(&set, 0, sizeof(set));
        set.monsters = &m;
        set.count    = 1;

        m.class_id    = 5;
        m.currentmove = &move;
        m.frame       = 10;
        m.dead        = true;
        m.in_use      = true;
        m.think       = NULL;       /* a corpse has no AI to run */
        d = &m;

        q2_monster_set_tick(&set);
        check_eq_i(d->frame, 11, "a dead monster's death move still advances");

        q2_monster_set_tick(&set);
        q2_monster_set_tick(&set);
        check_eq_i(d->frame, 13, "up to its last frame");

        /* And holds there: the move has an endfunc that installs nothing. */
        q2_monster_set_tick(&set);
        check_eq_i(d->frame, 10, "then the move restarts, as this one loops");

        m.dead = false;
    }

    q2_class_table_reset();
    q2_ai_set_world(NULL);
}

/* ------------------------------------------------------------------------- */
static void test_frame_distance(void)
{
    q2_monster m;
    q2_mframe f;

    printf("per-frame distance\n");

    q2_monster_init(&m);
    m.speed_scale = 10;

    f.ai = 3; f.dist = 10; f.think = 0;
    check_eq_i(q2_monster_frame_dist(&m, &f), 120,
               "dist * scale * 12 / 10 at the neutral scale");

    m.speed_scale = 20;
    check_eq_i(q2_monster_frame_dist(&m, &f), 240, "double scale, double step");

    m.speed_scale = 10;
    m.aiflags |= Q2_AI_HOLD_FRAME;
    check_eq_i(q2_monster_frame_dist(&m, &f), 0, "a held frame never advances");
    m.aiflags = 0;

    /* Negative distances back a creature up, which several pain animations
     * use; the sign must survive the scaling. */
    f.dist = -10;
    check_eq_i(q2_monster_frame_dist(&m, &f), -120, "a negative dist backs up");
}

/* ------------------------------------------------------------------------- */
static void test_start_go(void)
{
    q2_monster m;

    printf("monster_start_go\n");
    reset_spies();
    q2_level_reset();
    q2_ai_set_pick_target(NULL, NULL);

    q2_level_state.time = 42;

    place(&m, 0, 0, 0);
    hook_up(&m);
    m.health = 100;
    q2_monster_start_go(&m);

    check(m.think == q2_M_MoveFrame, "a woken creature thinks with M_MoveFrame");
    check_eq_i(m.next_think, 43, "and is scheduled for the next tick");
    check_eq_i(g_stand_calls, 1, "with no target it stands");
    check(m.pausetime >= 1000000000, "under the never-expiring pause");

    /* A dead creature is not woken at all. */
    reset_spies();
    place(&m, 0, 0, 0);
    hook_up(&m);
    m.health = 0;
    m.think = NULL;
    q2_monster_start_go(&m);
    check(m.think == NULL, "a dead creature is not woken");
    check_eq_i(g_stand_calls, 0, "and nothing is called on it");
}

/* ------------------------------------------------------------------------- */
/* End to end: a miniature creature, driven only by the frame driver           */
/* ------------------------------------------------------------------------- */
/*
 * Everything below this point is what a real creature module would supply —
 * three moves and three callbacks — and nothing else. The test then does what
 * the engine does: advance the clock and call the think. If the creature
 * notices the player, turns, closes the distance and strikes, the whole chain
 * is working, because none of those steps is written here.
 */
#define MINI_CLASS 9

static const q2_mframe mini_stand_frames[4] = {
    { Q2_AI_STAND, 0, 0 }, { Q2_AI_STAND, 0, 0 },
    { Q2_AI_STAND, 0, 0 }, { Q2_AI_STAND, 0, 0 }
};
static const q2_mframe mini_run_frames[4] = {
    { Q2_AI_RUN, 20, 0 }, { Q2_AI_RUN, 20, 0 },
    { Q2_AI_RUN, 20, 0 }, { Q2_AI_RUN, 20, 0 }
};
/* think index 1 is the swing; the class table routes it to mini_hit. */
static const q2_mframe mini_melee_frames[3] = {
    { Q2_AI_CHARGE, 0, 0 }, { Q2_AI_CHARGE, 0, 1 }, { Q2_AI_CHARGE, 0, 0 }
};

static q2_mmove mini_stand_move, mini_run_move, mini_melee_move;

static int g_mini_hits;
static void mini_hit(q2_monster *m) { (void)m; g_mini_hits++; }

/* A real creature's attack move ends by handing control back to its run move.
 * Without that it charges forever and never re-checks its target, because
 * ai_charge — faithfully — does not run ai_checkattack. Reproducing it here is
 * what makes the stand-down below a test of the framework rather than of this
 * stub. */
static void mini_melee_end(q2_monster *m) { if (m->run) m->run(m); }

static void mini_stand(q2_monster *m) { m->currentmove = &mini_stand_move; }
static void mini_run(q2_monster *m)   { m->currentmove = &mini_run_move;   }
static void mini_melee(q2_monster *m) { m->currentmove = &mini_melee_move; }

static bool mini_checkattack(q2_monster *m)
{
    /* The decision a creature's own checkattack makes, from the globals
     * ai_checkattack publishes rather than from anything passed in. */
    if (q2_enemy_range == Q2_RANGE_MELEE && q2_enemy_infront) {
        m->attack_state = Q2_AS_MELEE;
        return false;
    }
    return false;
}

static void mini_setup(void)
{
    memset(&mini_stand_move, 0, sizeof(mini_stand_move));
    mini_stand_move.first_frame = 0;
    mini_stand_move.last_frame  = 3;
    mini_stand_move.frames      = mini_stand_frames;

    memset(&mini_run_move, 0, sizeof(mini_run_move));
    mini_run_move.first_frame = 10;
    mini_run_move.last_frame  = 13;
    mini_run_move.frames      = mini_run_frames;

    memset(&mini_melee_move, 0, sizeof(mini_melee_move));
    mini_melee_move.first_frame = 20;
    mini_melee_move.last_frame  = 22;
    mini_melee_move.frames      = mini_melee_frames;
    mini_melee_move.endfunc     = mini_melee_end;

    q2_class_table_reset();
    q2_class_think_set(MINI_CLASS, 1, mini_hit);
}

static void test_end_to_end(void)
{
    q2_monster m, player;
    int tick;
    bool acquired = false;
    s32 start_gap;

    printf("end to end: notice, turn, close, strike\n");

    q2_level_reset();
    q2_trail_init();
    q2_ai_set_world(&g_floor_world);
    g_ledge_x  = 0x7FFFFFFF;
    g_no_floor = false;
    g_floor_y  = 0;
    g_mini_hits = 0;
    srand(17);
    mini_setup();

    q2_level_state.framenum = 1;
    q2_level_state.time     = 1;

    /* The creature is looking away from the player to start with, so noticing
     * has to come from the forward cone widening as the player closes rather
     * than from the very first tick. */
    place(&m, 0, 0, 0);
    m.class_id    = MINI_CLASS;
    m.yaw_speed   = 300;
    m.speed_scale = 12;
    m.stand       = mini_stand;
    m.walk        = mini_stand;
    m.run         = mini_run;
    m.melee       = mini_melee;
    m.checkattack = mini_checkattack;
    m.health      = 100;
    q2_monster_start_go(&m);

    check(m.currentmove == &mini_stand_move, "a woken creature stands");
    check(m.think == q2_M_MoveFrame, "with the frame driver as its think");

    make_player(&player, 0, 9000);
    player.health = 100;
    q2_level_state.sight_client = &player;

    start_gap = player.pos[2] - m.pos[2];

    for (tick = 0; tick < 400; tick++) {
        q2_level_state.framenum++;
        q2_level_state.time++;

        if (m.next_think <= q2_level_state.time)
            m.think(&m);

        if (!acquired && m.enemy == &player) {
            acquired = true;
            check(m.currentmove == &mini_run_move,
                  "noticing the player switches it to its run move");
        }
    }

    check(acquired, "the creature notices the player unprompted");
    check(m.pos[2] > start_gap / 4,
          "and closes most of the distance to them");
    check(q2_monster_dist_sq(&m, player.pos)
              < (s64)Q2_MELEE_DISTANCE * Q2_MELEE_DISTANCE,
          "ending up inside melee range");
    /* The attack cycle runs continuously once in range — decide, swing, back
     * to running — so the state is one of the two ends of it rather than a
     * fixed value. */
    check(m.attack_state == Q2_AS_MELEE || m.attack_state == Q2_AS_STRAIGHT,
          "cycling between deciding to swing and having swung");
    check(g_mini_hits > 0, "and it actually strikes");

    /*
     * Kill the player and the creature must stand down rather than keep
     * swinging at a corpse. Note that FindTarget does NOT test health — only
     * ai_checkattack does — so the dead player has to stop being the level's
     * sight client too, exactly as it would in the game.
     */
    {
        int i;
        player.health = 0;
        q2_level_state.sight_client = NULL;
        for (i = 0; i < 20; i++) {
            q2_level_state.framenum++;
            q2_level_state.time++;
            if (m.next_think <= q2_level_state.time)
                m.think(&m);
        }
        check(m.enemy == NULL, "a dead player is dropped");
        check(m.currentmove == &mini_stand_move,
              "and the creature goes back to standing");
    }

    q2_class_table_reset();
    q2_ai_set_world(NULL);
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    printf("Q2PSX-PC AI tests\n\n");

    test_constants();
    test_range_bands();
    test_vectors();
    test_facing();
    test_turning();
    test_acquisition();
    test_alerting();
    test_checkattack();
    test_stand_and_walk();
    test_movement_verbs();
    test_movestep_trace_paths();
    test_chase_directions();
    test_lost_sight();
    test_frame_driver();
    test_frame_distance();
    test_start_go();
    test_end_to_end();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");

    return g_failures ? 1 : 0;
}
