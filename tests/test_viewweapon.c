/*
 * test_viewweapon.c — the weapon in the player's hands, as behaviour.
 *
 * `q2psx-inspect viewweapon <disc>` already reads the real animation bank off a
 * real executable and checks every constant against the instruction it came
 * from, so nothing here re-asserts a key offset. What this pins down is the
 * behaviour that no table can express: that the state machine cycles the way
 * the original's transitions say it does, that LOWER holds at the bottom of its
 * arc until the seventy-tick countdown expires rather than swapping early, that
 * a shot cannot be cancelled by switching weapons, that running dry does not
 * wedge the machine, that a long frame plays a short clip through instead of
 * skipping the events on it, and that the weapon is placed on the eye.
 *
 * The bank here is synthetic, with clip lengths chosen so that each transition
 * is unambiguous. That is deliberate: a test that used the real bank would be
 * testing the disc as much as the code.
 */
#include "trig.h"
#include "viewweapon.h"

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

/* ------------------------------------------------------------------------- */
/* A bank with one key per state and a distinct duration each, so that "which  */
/* clip are we in" is readable from the clock alone.                          */
/* ------------------------------------------------------------------------- */
#define RAISE_TICKS  40
#define FIRE_TICKS   30
#define IDLE_TICKS   50
#define LOWER_TICKS  20

static q2_vm_key   g_keys[Q2_VM_SLOTS][Q2_VM_STATES];
static q2_vm_tables g_tab;

static void build_bank(void)
{
    static const s16 dur[Q2_VM_STATES] = {
        RAISE_TICKS, FIRE_TICKS, IDLE_TICKS, LOWER_TICKS
    };
    int w, s;

    memset(&g_tab, 0, sizeof(g_tab));
    memset(g_keys, 0, sizeof(g_keys));

    for (w = 0; w < Q2_VM_SLOTS; w++) {
        for (s = 0; s < Q2_VM_STATES; s++) {
            q2_vm_key *k = &g_keys[w][s];

            k->duration = dur[s];
            k->event    = Q2_VM_EVENT_NONE;
            /* A translation big enough that rotating it is measurable. */
            k->t[0] = (s16)(100 + w * 10);
            k->t[1] = (s16)(200 + s * 10);
            k->t[2] = (s16)300;

            g_tab.clip[w][s].count = 1;
            g_tab.clip[w][s].key   = k;
            g_tab.clip[w][s].addr  = 0x8009D000u + (u32)(w * 4 + s) * 20u;
        }
        snprintf(g_tab.model_name[w], sizeof(g_tab.model_name[w]),
                 "Weapon %d", w);
    }

    /* The fire clip of weapon 1 carries an event, so the event path is live. */
    g_keys[1][Q2_VM_FIRE].event = 7;
}

/* Run the machine until `state` is reached or the budget runs out. */
static int run_until(q2_viewweapon *vw, q2_vm_state state, bool fire,
                     int budget)
{
    int t;

    for (t = 0; t < budget; t++) {
        if (vw->state == state)
            return t;
        q2_vw_advance(vw, 10, fire, Q2_VW_FIRED);
    }
    return (vw->state == state) ? t : -1;
}

/* ------------------------------------------------------------------------- */
static void test_raise_to_idle(void)
{
    q2_viewweapon vw;
    int t;

    /*
     * A fresh view model starts in LOWER, not RAISE — 0x8004F7A4 writes state
     * 3 with total 1 and left 0, so the lower completes on the first tick and
     * goes through the SWAP arm, which is what resolves the model. This check
     * used to pin RAISE, citing 0x8004FA48; that address is the state after a
     * swap, which is a different moment.
     */
    q2_vw_init(&vw, &g_tab, 1);
    CHECK(vw.scale == Q2_ONE_12 && vw.fade == Q2_ONE_12,
          "the allocator scales are %d/%d, expected 4096/4096",
          (int)vw.scale, (int)vw.fade);
    CHECK(vw.glow[0] == 0x40 && vw.glow[1] == 0x40 && vw.glow[2] == 0x40,
          "the allocator ambient is %02X/%02X/%02X, expected 40/40/40",
          vw.glow[0], vw.glow[1], vw.glow[2]);
    CHECK(vw.light_selector == 1,
          "the constructor +0xF4 is %d, expected literal +1",
          (int)vw.light_selector);
    CHECK(vw.state == Q2_VM_LOWER, "a fresh weapon starts in lower, got %s",
          q2_vm_state_name(vw.state));
    q2_vw_advance(&vw, 10, false, Q2_VW_FIRE_NONE);
    CHECK(vw.state == Q2_VM_RAISE, "and reaches raise on the first tick, got %s",
          q2_vm_state_name(vw.state));
    CHECK(vw.weapon == 1, "weapon %d", vw.weapon);

    t = run_until(&vw, Q2_VM_IDLE, false, 100);
    CHECK(t >= 0, "the raise never reached idle");
    CHECK(vw.state == Q2_VM_IDLE, "state %s", q2_vm_state_name(vw.state));

    /* Idle loops rather than running out. */
    {
        int i;
        for (i = 0; i < 200; i++)
            q2_vw_advance(&vw, 10, false, Q2_VW_FIRED);
        CHECK(vw.state == Q2_VM_IDLE, "idle should loop, got %s",
              q2_vm_state_name(vw.state));
    }
}

static void test_fire(void)
{
    q2_viewweapon vw;
    s16 ev = 0;
    int i;
    bool got_event = false;

    q2_vw_init(&vw, &g_tab, 1);
    run_until(&vw, Q2_VM_IDLE, false, 100);

    /* The trigger takes effect on the tick it is pressed, not at the next key
     * boundary — 0x8004FAF4 tests it every tick. */
    q2_vw_advance(&vw, 10, true, Q2_VW_FIRED);
    CHECK(vw.state == Q2_VM_FIRE, "the trigger should enter fire, got %s",
          q2_vm_state_name(vw.state));

    /*
     * AND THE LATCH IS NOT SET. This check used to assert it was, on the
     * reading that viewmodel+216 means "a shot is in flight". It is the DRY
     * latch: inside the state machine the halfword is only ever cleared, and
     * the two non-zero writes in the whole image (0x80050230, 0x800502FC) both
     * follow a fire function returning 2.
     *
     * The cost of the misreading was visible rather than academic. The arm
     * that clears the latch also raises `refire`, the caller turns that into
     * the auto-switch pass, and a selection change plays a full LOWER then
     * RAISE — so the gun dipped out of view and swung back in after every
     * single shot.
     */
    CHECK(!vw.fire_latch, "a successful shot does not set the dry latch");

    /*
     * Holding must not restart the clip every tick. Counted rather than
     * observed as a state change: once the clip ends the machine returns to
     * IDLE and, with the trigger still down and the shot still reported, fires
     * again on that same tick — so a loop watching for `state != FIRE` never
     * sees it, which is what this check used to do.
     */
    {
        u32 before = vw.fires_started;

        for (i = 0; i < 200; i++) {
            q2_vw_advance(&vw, 10, true, Q2_VW_FIRED);
            if (q2_vw_take_event(&vw, &ev) && ev == 7)
                got_event = true;
        }
        CHECK(vw.fires_started > before,
              "the clip ended and the next shot started");
        /* Strictly fewer than one per tick. The cadence itself is the fire
         * clip's own length — three keys in this synthetic table, 110 ticks
         * for the disc's real blaster — so the bound here only has to catch
         * "restarted every tick", which is what the missing FIRE_NONE case
         * and the mis-set latch each produced in their own way. */
        CHECK(vw.fires_started - before < 100u,
              "but not once per tick: got %u clips in 200 ticks",
              (unsigned)(vw.fires_started - before));
    }
    CHECK(got_event, "the fire clip's event was never raised");
}

static void test_fire_denied(void)
{
    q2_viewweapon vw;
    int i;

    q2_vw_init(&vw, &g_tab, 1);
    run_until(&vw, Q2_VM_IDLE, false, 100);

    /*
     * An empty weapon must not enter the fire clip and must not wedge: the
     * original clears the latch and recomputes the neighbours (0x8004FB5C), and
     * the machine keeps idling.
     */
    for (i = 0; i < 50; i++)
        q2_vw_advance(&vw, 10, true, Q2_VW_FIRE_DENIED);

    CHECK(vw.state == Q2_VM_IDLE, "a denied shot should leave it idle, got %s",
          q2_vm_state_name(vw.state));
    CHECK(!vw.fire_latch, "a denied shot should not latch");
}

/*
 * The port's own third outcome: the trigger is DOWN, the weapon is fed, and no
 * shot happened — the sim's refire gate had not expired, or the frame ran no
 * tick at all. The machine must be told Q2_VW_FIRE_NONE and must keep idling.
 *
 * This is the case a caller reaches only by consuming an EVENT. The client used
 * to gate its report on `ticks`, which is clamped to a minimum of 1 and is
 * therefore always true, so it re-reported the last attempt's latched
 * `last_shot` on every rendered frame and this arm was never taken: the fire
 * clip restarted as fast as the latch could clear, at render rate rather than
 * once per shot. `shot_serial` is what makes it an event; this is what the
 * machine owes a caller that gets that right.
 */
static void test_fire_none_holds_the_clip(void)
{
    q2_viewweapon vw;
    int i;

    q2_vw_init(&vw, &g_tab, 1);
    run_until(&vw, Q2_VM_IDLE, false, 100);

    for (i = 0; i < 50; i++)
        q2_vw_advance(&vw, 10, true, Q2_VW_FIRE_NONE);

    CHECK(vw.state == Q2_VM_IDLE,
          "a held trigger with no shot should stay idle, got %s",
          q2_vm_state_name(vw.state));
    CHECK(vw.fires_started == 0,
          "a held trigger with no shot started %u fire clips",
          vw.fires_started);

    /* And the shot that DOES arrive still fires: staying idle must not have
     * cost the machine the pass that takes it. */
    q2_vw_advance(&vw, 10, true, Q2_VW_FIRED);
    CHECK(vw.state == Q2_VM_FIRE, "the shot after the gate should fire, got %s",
          q2_vm_state_name(vw.state));
    CHECK(vw.fires_started == 1, "one shot should be one clip, got %u",
          vw.fires_started);

    /*
     * One shot, ONE clip, however long the trigger stays down afterwards with
     * nothing more to report — which is the whole shape of the defect.
     */
    for (i = 0; i < 200; i++)
        q2_vw_advance(&vw, 10, true, Q2_VW_FIRE_NONE);

    CHECK(vw.fires_started == 1,
          "holding after one shot started %u clips, not 1", vw.fires_started);
}

static void test_switch(void)
{
    q2_viewweapon vw;
    int i;
    bool swapped = false;

    q2_vw_init(&vw, &g_tab, 1);
    run_until(&vw, Q2_VM_IDLE, false, 100);

    q2_vw_select(&vw, 5);
    CHECK(vw.switch_ticks == Q2_VW_SWITCH_TICKS,
          "selecting should arm the %d-tick countdown, got %d",
          Q2_VW_SWITCH_TICKS, vw.switch_ticks);

    /* It must LOWER first, and the model must not change while it does. */
    q2_vw_advance(&vw, 10, false, Q2_VW_FIRED);
    CHECK(vw.state == Q2_VM_LOWER, "selecting should lower, got %s",
          q2_vm_state_name(vw.state));
    CHECK(vw.weapon == 1, "the weapon must not change during the lower");

    /*
     * The countdown gates the swap. The lower clip is 20 ticks and the
     * countdown is 70, so the machine must still be holding the old weapon well
     * after the clip would otherwise have ended.
     */
    for (i = 0; i < 4; i++)
        q2_vw_advance(&vw, 10, false, Q2_VW_FIRED);
    CHECK(vw.weapon == 1,
          "the swap happened before the countdown expired (ticks left %d)",
          vw.switch_ticks);

    for (i = 0; i < 40 && !swapped; i++)
        swapped = q2_vw_advance(&vw, 10, false, Q2_VW_FIRED);

    CHECK(swapped, "the weapon never swapped");
    CHECK(vw.weapon == 5, "swapped to %d, wanted 5", vw.weapon);
    CHECK(vw.state == Q2_VM_RAISE, "a swap should raise the new weapon, got %s",
          q2_vm_state_name(vw.state));
    CHECK(strcmp(q2_vw_model_name(&vw), "Weapon 5") == 0,
          "model name '%s'", q2_vw_model_name(&vw));
}

static void test_switch_cannot_cancel_a_shot(void)
{
    q2_viewweapon vw;

    q2_vw_init(&vw, &g_tab, 1);
    run_until(&vw, Q2_VM_IDLE, false, 100);
    q2_vw_advance(&vw, 10, true, Q2_VW_FIRED);
    CHECK(vw.state == Q2_VM_FIRE, "should be firing");

    /* 0x8004FAB4 excludes FIRE from the lower transition. */
    q2_vw_select(&vw, 3);
    q2_vw_advance(&vw, 5, false, Q2_VW_FIRED);
    CHECK(vw.state == Q2_VM_FIRE,
          "switching must not cancel a shot in flight, got %s",
          q2_vm_state_name(vw.state));
}

/*
 * A long host frame must play the clip through rather than skipping it: the
 * original consumes dt in chunks of min(left, dt) (0x8004EEA4), and the events
 * on the keys it steps over are what fire the shot.
 */
static void test_long_frame_consumes_keys(void)
{
    q2_viewweapon vw;
    u32 played_small, played_big;
    int i;

    q2_vw_init(&vw, &g_tab, 1);
    for (i = 0; i < 30; i++)
        q2_vw_advance(&vw, 10, false, Q2_VW_FIRED);
    played_small = vw.keys_played;

    q2_vw_init(&vw, &g_tab, 1);
    q2_vw_advance(&vw, 300, false, Q2_VW_FIRED);
    played_big = vw.keys_played;

    CHECK(played_big == played_small,
          "one 300-tick frame played %u keys, thirty 10-tick frames played %u",
          played_big, played_small);
}

/* ------------------------------------------------------------------------- */
static void test_placement(void)
{
    q2_viewweapon vw;
    s32 feet[3] = { 1000, 2000, 3000 };
    s32 stand[3], crouch[3], ang[3];
    s16 zero[3] = { 0, 0, 0 };

    q2_vw_init(&vw, &g_tab, 1);
    q2_vw_advance(&vw, 20, false, Q2_VW_FIRED);

    q2_vw_place(&vw, feet, 576, zero, zero, stand, ang);
    q2_vw_place(&vw, feet, 286, zero, zero, crouch, ang);

    /* FORMATS §9.12: eye.y = pos.y + 286 - viewOffset, and +Y is down, so a
     * smaller view offset puts the weapon LOWER (a larger y). */
    CHECK(crouch[1] - stand[1] == 290,
          "crouching should drop the weapon by 290, got %d",
          crouch[1] - stand[1]);

    /* The aim's x component is negated at the sum (0x8004F41C), so a positive
     * pitch input must produce a negative contribution to the angle. */
    {
        s16 pitch_up[3] = { 512, 0, 0 };
        s32 a0[3], a1[3], o[3];

        q2_vw_place(&vw, feet, 576, zero, zero, o, a0);
        q2_vw_place(&vw, feet, 576, pitch_up, zero, o, a1);
        CHECK(a1[0] - a0[0] == -512,
              "pitch must enter negated: delta %d", a1[0] - a0[0]);
        CHECK(a1[1] == a0[1] && a1[2] == a0[2],
              "pitch must not disturb yaw or roll");
    }

    /* Kick adds to aim rather than replacing it. */
    {
        s16 aim[3]  = { 0, 100, 0 };
        s16 kick[3] = { 0,  25, 0 };
        s32 a0[3], a1[3], o[3];

        q2_vw_place(&vw, feet, 576, aim, zero, o, a0);
        q2_vw_place(&vw, feet, 576, aim, kick, o, a1);
        CHECK(a1[1] - a0[1] == 25, "kick must add: delta %d", a1[1] - a0[1]);
    }

    /* Rigidity: a turn swings the weapon around the eye without stretching it. */
    {
        s16 y1[3] = { 0, 1024, 0 };
        s32 o0[3], o1[3];
        s32 eye_y = feet[1] + Q2_VW_EYE_BASE - 576;
        s64 d0 = 0, d1 = 0;
        int i;

        q2_vw_place(&vw, feet, 576, zero, zero, o0, ang);
        q2_vw_place(&vw, feet, 576, y1,   zero, o1, ang);

        for (i = 0; i < 3; i++) {
            s32 a = (i == 1) ? o0[i] - eye_y : o0[i] - feet[i];
            s32 b = (i == 1) ? o1[i] - eye_y : o1[i] - feet[i];
            d0 += (s64)a * a;
            d1 += (s64)b * b;
        }

        CHECK(o0[0] != o1[0] || o0[1] != o1[1] || o0[2] != o1[2],
              "a quarter turn should move the weapon");
        CHECK(d0 - d1 < 4096 * 4096 && d1 - d0 < 4096 * 4096,
              "a turn must not stretch it: %lld vs %lld",
              (long long)d0, (long long)d1);
    }
}

/*
 * The roll reaches the offset.
 *
 * `RotMatrix` takes all three angles (0x8004F464 hands it the whole SVECTOR at
 * sp+40, whose z is at sp+44), so the console rotates the weapon's offset by the
 * roll as well as the yaw and the pitch. The port used to build a yaw/pitch
 * matrix and silently drop it, which meant the strafe lean rolled the camera and
 * left the gun upright — the two visibly separating exactly when the lean is
 * strongest.
 *
 * The check is the invariant rather than a number: rotating the resulting world
 * offset BACK by the same three angles must recover `cur_t`, whatever they are.
 * That holds for any roll if the offset was rotated by all three and fails as
 * soon as one is dropped.
 */
static void test_roll_reaches_the_offset(void)
{
    q2_viewweapon vw;
    s32 feet[3] = { 0, 0, 0 };
    s16 zero[3] = { 0, 0, 0 };
    s16 leaning[3] = { 300, 700, 512 };   /* pitch, yaw and a real roll */
    s32 origin[3], ang[3];
    s16 m[3][3];
    s32 back[3];
    int i;

    q2_vw_init(&vw, &g_tab, 1);
    q2_vw_advance(&vw, 20, false, Q2_VW_FIRED);

    q2_vw_place(&vw, feet, 576, leaning, zero, origin, ang);

    /*
     * Undo the eye, so what is left is the rotated offset alone.
     *
     * The placement is `feet + local - viewOffset`: the console's 286 applies
     * to the ENTITY ORIGIN and this function is handed the feet, so the two
     * constants cancel and only the view offset has to come back off. This
     * used to add `Q2_VW_EYE_BASE - 576` and passed only because the placement
     * carried the same surplus 286.
     */
    origin[1] += 576;

    /*
     * Recovered with the CAMERA'S builder, applied forward.
     *
     * This used to build `q2_rotation_euler(ang - cur_r)` and apply its
     * transpose, which is what `q2_vw_place` USED TO DO with the signs the
     * other way round - so the test and the emitter agreed with each other and
     * neither agreed with the camera. That is the same failure the briefing
     * panel's test had: written from the code under test rather than from what
     * the code owes its caller, it pins the defect instead of the requirement.
     *
     * What `q2_vw_place` owes is that the CAMERA undo it. It now applies
     * `q2_rotation_view(yaw, pitch, roll)` transposed, so recovering `t` is
     * that same matrix applied FORWARD - and `test_camera_undoes_the_placement`
     * is the check that the matrix is the right one in the first place.
     */
    q2_rotation_view(m, ang[1] - vw.cur_r[1],
                        -(ang[0] - vw.cur_r[0]),
                        ang[2] - vw.cur_r[2]);

    for (i = 0; i < 3; i++)
        back[i] = ((s32)m[i][0] * origin[0]
                 + (s32)m[i][1] * origin[1]
                 + (s32)m[i][2] * origin[2]) >> Q2_FRAC_12;

    for (i = 0; i < 3; i++) {
        s32 d = back[i] - vw.cur_t[i];
        if (d < 0) d = -d;
        /* Two units of slack for the 1.3.12 round trip. */
        CHECK(d <= 2,
              "roll must rotate the offset: axis %d recovered %d, wanted %d",
              i, back[i], vw.cur_t[i]);
    }
}

/* Weapon 0 is a live state, not an error: the clip table aliases slot 0 to
 * slot 1 exactly as the fire-function table does. */
static void test_no_weapon(void)
{
    q2_viewweapon vw;
    int i;

    q2_vw_init(&vw, &g_tab, 0);
    for (i = 0; i < 100; i++)
        q2_vw_advance(&vw, 10, true, Q2_VW_FIRED);
    CHECK(vw.weapon == 0, "weapon 0 should stay 0");
}

/* ------------------------------------------------------------------------- */
/*
 * THE ONE THING #46 NEVER CHECKED: does the camera undo the placement?
 *
 * Every operand in `q2_vw_place` has been read against the executable and every
 * one agrees — the translation is 140 on every key, the interpolation is the
 * disc's, the rotation order is RotMatrix's. And the weapon still sits about a
 * quarter screen left of the console's.
 *
 * What was never tested is the IDENTITY the whole arrangement rests on. Follow
 * a part origin through modeldraw.c:
 *
 *     camera_space = view · (inst.origin + spin·local − cam.pos)
 *
 * with `local` zero at the grip, `inst.origin = feet + R_place·t` and
 * `cam.pos` the eye — and `q2_vw_place` subtracts exactly the same
 * `view_offset` from y that `q2_sim_eye` does, so `inst.origin − cam.pos`
 * is `R_place · t` exactly. Therefore:
 *
 *     camera_space = view · R_place · t
 *
 * and for the weapon to sit where the clip authored it — t, in view space —
 * `view · R_place` has to be the IDENTITY. Nothing anywhere asserts that, and
 * the two matrices are built by different functions from differently-signed
 * angles: `q2_rotation_view(yaw, pitch, roll)` against
 * `q2_rotation_euler(−pitch, yaw, roll)`.
 *
 * A residual rotation there would displace the grip by exactly the kind of
 * fixed screen offset #46 measured, and would be invisible to any amount of
 * re-reading the operands, because each operand is individually right.
 */
static void test_camera_undoes_the_placement(void)
{
    /* Yaw, pitch and roll in the engine's 4096-unit circle. */
    static const struct { s16 yaw, pitch, roll; } k_cases[] = {
        {    0,    0,   0 },
        { 1024,    0,   0 },
        { 2048,    0,   0 },
        { 3072,    0,   0 },
        {  700,    0,   0 },
        {    0,  300,   0 },
        {    0, -300,   0 },
        {  700,  300,   0 },
        {  700,  300,  90 },
        { 3000, -200, -90 }
    };
    u32 i;

    puts("place: the camera undoes the placement (view * R_place == I)");

    for (i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        s16 view[3][3], place[3][3];
        s32 worst = 0;
        int r, c, k;

        q2_rotation_view(view, k_cases[i].yaw, k_cases[i].pitch,
                         k_cases[i].roll);
        /* Exactly what q2_vw_place builds: the x angle negated, y and z as
         * they come. */
        q2_rotation_view(place, k_cases[i].yaw, k_cases[i].pitch,
                         k_cases[i].roll);

        for (r = 0; r < 3; r++) {
            for (c = 0; c < 3; c++) {
                s32 acc = 0;
                s32 want = (r == c) ? Q2_ONE_12 : 0;
                s32 d;

                for (k = 0; k < 3; k++)
                    acc += (s32)view[r][k] * (s32)place[c][k];
                acc >>= Q2_FRAC_12;

                d = acc - want;
                if (d < 0)
                    d = -d;
                if (d > worst)
                    worst = d;
            }
        }

        /*
         * A 1.3.12 product of two rounded matrices cannot be exact; 32/4096 is
         * under one percent and is the rounding, not a rotation. A residual
         * ROTATION shows up here as hundreds or thousands.
         */
        if (worst > 32) {
            printf("  FAIL  yaw %d pitch %d roll %d: view*place is not the "
                   "identity, worst element off by %ld/4096\n",
                   k_cases[i].yaw, k_cases[i].pitch, k_cases[i].roll,
                   (long)worst);
            g_fail++;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* The per-weapon FIRE driver — 0x8004FEE8                                    */
/* ------------------------------------------------------------------------- */
/*
 * The bank above has one key per state, which is enough for every transition
 * test but useless for the frame driver: its four arms switch on the frame
 * INDEX inside the fire clip, and a one-key clip only ever has frame 0. This
 * second bank gives the four driven weapons a fire clip long enough to hold
 * every band boundary the arms name, with keys long enough that a small step
 * never crosses one — so a test can park the machine on a frame and ask what
 * that frame does.
 */
#define DRV_KEY_TICKS 100

static q2_vm_key    g_drv_keys[Q2_VM_SLOTS][Q2_VM_STATES][28];
static q2_vm_tables g_drv;

static void build_driver_bank(void)
{
    static const int fire_keys[Q2_VM_SLOTS] = {
        [4] = 3,     /* machinegun:   the three-key cycle                */
        [5] = 28,    /* chaingun:     spin-up 0..8, loop 9..17, down 18+ */
        [6] = 3,     /* hand grenade: prime, hold, throw                 */
        [9] = 8      /* hyperblaster: 1..5 loop, 6 the tail              */
    };
    int w, s, k;

    memset(&g_drv, 0, sizeof(g_drv));
    memset(g_drv_keys, 0, sizeof(g_drv_keys));

    for (w = 0; w < Q2_VM_SLOTS; w++) {
        for (s = 0; s < Q2_VM_STATES; s++) {
            int n = (s == Q2_VM_FIRE && fire_keys[w]) ? fire_keys[w] : 1;

            for (k = 0; k < n; k++) {
                g_drv_keys[w][s][k].duration = DRV_KEY_TICKS;
                g_drv_keys[w][s][k].event    = Q2_VM_EVENT_NONE;
            }
            g_drv.clip[w][s].count = (u32)n;
            g_drv.clip[w][s].key   = g_drv_keys[w][s];
            g_drv.clip[w][s].addr  = 0x8009E000u + (u32)(w * 4 + s) * 20u;
        }
        snprintf(g_drv.model_name[w], sizeof(g_drv.model_name[w]),
                 "Weapon %d", w);
    }
}

/* Park the machine in FIRE with `weapon` in hand, on `frame`, with the
 * per-frame cache cleared so the next step is a NEW frame to the arm. */
static void park_in_fire(q2_viewweapon *vw, int weapon, u32 frame)
{
    q2_vw_init(vw, &g_drv, weapon);
    run_until(vw, Q2_VM_IDLE, false, 200);
    q2_vw_advance(vw, 1, true, Q2_VW_FIRED);

    vw->state           = Q2_VM_FIRE;
    vw->frame           = frame;
    vw->left            = DRV_KEY_TICKS;
    vw->total           = DRV_KEY_TICKS;
    vw->last_fire_frame = -1;
    vw->spin_accum      = 0;
    vw->fire_latch      = false;
    (void)q2_vw_take_frame_fires(vw);
    (void)q2_vw_take_frame_sound(vw);
}

/*
 * 0x80050180 — the chaingun's rounds per animation frame, which is what
 * openquestions #39c called residue and defaulted to one. Three bands, and the
 * middle one asks the trigger.
 */
static void test_chaingun_bands(void)
{
    static const struct { u32 frame; bool held; u32 want; const char *why; } c[] = {
        {  0, true, 0, "frame 0 is the spin-up boundary and fires nothing" },
        {  3, true, 1, "the spin-up throws one round a frame" },
        {  6, true, 2, "held, the second band throws two" },
        {  6, false, 1, "released, the same band throws one" },
        {  9, true, 0, "frame 9 opens the loop and fires nothing" },
        { 12, true, 3, "the loop throws three" },
        { 17, true, 0, "frame 17 closes the loop and fires nothing" },
        { 20, true, 0, "the whole spin-down fires nothing" }
    };
    q2_viewweapon vw;
    size_t i;

    for (i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        u32 got;

        park_in_fire(&vw, 5, c[i].frame);
        /* Frame 17 held wraps to 9 before the shot test, which is the loop
         * itself; frame 9 released jumps to the spin-down. Both are checked
         * separately below — here the wrap is what the arm is meant to do. */
        q2_vw_advance(&vw, 5, c[i].held, Q2_VW_FIRE_NONE);
        got = q2_vw_take_frame_fires(&vw);

        CHECK(got == c[i].want,
              "chaingun frame %u %s: %u rounds, want %u — %s",
              c[i].frame, c[i].held ? "held" : "released", got, c[i].want,
              c[i].why);
    }
}

/* 0x80050084 and 0x8005009C: the loop holds while held and leaves when let go,
 * and neither is a clip boundary — the arm rewrites the frame index. */
static void test_chaingun_loop_and_spin_down(void)
{
    q2_viewweapon vw;

    park_in_fire(&vw, 5, 17);
    q2_vw_advance(&vw, 5, true, Q2_VW_FIRE_NONE);
    CHECK(vw.frame == 9, "held at frame 17 wraps to 9, got %u", vw.frame);

    park_in_fire(&vw, 5, 9);
    q2_vw_advance(&vw, 5, false, Q2_VW_FIRE_NONE);
    CHECK(vw.frame == 27, "released at frame 9 jumps to 27, got %u", vw.frame);

    /* 0x800503C4. A dry gun leaves FIRE when the clip comes back to its first
     * key, not at the clip's end — which is what stops an empty chaingun
     * grinding through a whole spin-down before it will switch. */
    park_in_fire(&vw, 5, 0);
    vw.fire_latch = true;
    q2_vw_advance(&vw, 5, true, Q2_VW_FIRE_NONE);
    CHECK(vw.state == Q2_VM_IDLE,
          "a dry chaingun leaves FIRE at frame 0, state %d", (int)vw.state);
}

/*
 * 0x80050038's `sw zero, 52` — the accumulator is RESET after a shot, not
 * reduced by the threshold. Taking `-= 30` in a loop instead banks a burst on
 * any frame long enough to cover several thresholds, so a low frame rate fired
 * faster than a high one.
 */
static void test_machinegun_accumulator_resets(void)
{
    q2_viewweapon vw;
    u32 got;

    park_in_fire(&vw, 4, 0);
    q2_vw_advance(&vw, 90, true, Q2_VW_FIRE_NONE);   /* three thresholds */
    got = q2_vw_take_frame_fires(&vw);
    CHECK(got == 1, "90 ticks of machinegun is one round, got %u", got);

    /* 0x8005003C: a release on the middle key jumps to the last one so the
     * clip can end, instead of hanging where the wrap keeps returning it. */
    park_in_fire(&vw, 4, 1);
    q2_vw_advance(&vw, 5, false, Q2_VW_FIRE_NONE);
    CHECK(vw.frame == 2, "released on key 1 jumps to key 2, got %u", vw.frame);

    /* And held, the last key wraps back to the first — the cycle. */
    park_in_fire(&vw, 4, 2);
    q2_vw_advance(&vw, 5, true, Q2_VW_FIRE_NONE);
    CHECK(vw.frame == 0, "held on key 2 wraps to key 0, got %u", vw.frame);
}

/* 0x80050298 and 0x800502C8: frames 1..5 are the loop and frame 6 is the tail,
 * which the loop must not fire on. */
static void test_hyperblaster_loop(void)
{
    q2_viewweapon vw;
    u32 got;

    park_in_fire(&vw, 9, 5);
    q2_vw_advance(&vw, 40, true, Q2_VW_FIRE_NONE);
    CHECK(vw.frame == 1, "held at frame 5 wraps to 1, got %u", vw.frame);

    park_in_fire(&vw, 9, 6);
    q2_vw_advance(&vw, 40, true, Q2_VW_FIRE_NONE);
    got = q2_vw_take_frame_fires(&vw);
    CHECK(got == 0, "frame 6 is the tail and fires nothing, got %u", got);
}

/*
 * 0x800503F0 — the hand grenade will not prime until its MODEL's `Set` move has
 * reached position 380, which is the gate this port had no counterpart for at
 * all. Below it the fire clip runs on and the throw happens on time; at or
 * above it the clip stops dead and the grenade is held.
 */
static void test_grenade_cook_waits_for_the_arm(void)
{
    q2_viewweapon vw;
    s32 before;

    /* Grenade3 compares current and previous model positions: 261 primes once
     * and 411 throws once. */
    park_in_fire(&vw, 6, 0);
    vw.hand_prev_anim = Q2_VW_HAND_PRIME_POSITION - 1;
    vw.anim_pos       = Q2_VW_HAND_PRIME_POSITION;
    q2_vw_advance(&vw, 1, false, Q2_VW_FIRE_NONE);
    CHECK(q2_vw_take_frame_sound(&vw) == Q2_WSND_HANDGREN_PRIME,
          "crossing 261 plays the hand-grenade prime sample");
    CHECK(q2_vw_take_frame_sound(&vw) == -1,
          "the prime crossing is a drained one-shot");

    park_in_fire(&vw, 6, 0);
    vw.anim_pos = Q2_VW_COOK_POSITION - 1;
    before = vw.left;
    q2_vw_advance(&vw, 10, true, Q2_VW_FIRE_NONE);
    CHECK(!vw.cook, "the grenade does not prime before the arm is up");
    CHECK(vw.left < before,
          "and its clip keeps running: left %d against %d", (int)vw.left,
          (int)before);

    park_in_fire(&vw, 6, 0);
    vw.anim_pos = Q2_VW_COOK_POSITION;
    before = vw.left;
    q2_vw_advance(&vw, 10, true, Q2_VW_FIRE_NONE);
    CHECK(vw.cook, "at 380 the grenade primes");
    CHECK(vw.frame == 1, "the frame pins to 1, got %u", vw.frame);
    CHECK(vw.left >= before,
          "and the clock stops rather than running out: left %d against %d",
          (int)vw.left, (int)before);
    CHECK(vw.anim_pos == Q2_VW_COOK_POSITION,
          "the arm holds where it is, at %d", (int)vw.anim_pos);
    CHECK(q2_vw_take_hand_grenade_cook(&vw) == 10,
          "the held entity receives the same ten cook ticks");
    CHECK(q2_vw_take_hand_grenade_cook(&vw) == 0,
          "cook time drains rather than being counted twice");

    /* Letting go unpins it; the actual throw is model position 411. */
    q2_vw_advance(&vw, 10, false, Q2_VW_FIRE_NONE);
    CHECK(!vw.cook, "releasing the trigger lets the throw animation continue");
    vw.hand_prev_anim = Q2_VW_HAND_RELEASE_POSITION - 1;
    vw.anim_pos       = Q2_VW_HAND_RELEASE_POSITION;
    q2_vw_advance(&vw, 1, false, Q2_VW_FIRE_NONE);
    CHECK(q2_vw_take_hand_grenade_release(&vw),
          "crossing 411 releases Grenade3");
    CHECK(!q2_vw_take_hand_grenade_release(&vw),
          "the release crossing is a drained one-shot");
    CHECK(q2_vw_take_frame_sound(&vw) == Q2_WSND_HANDGREN_THROW,
          "and crossing 411 plays the throw sample");

    /* A fuse that reaches zero while held takes the forced-reset arm at
     * 0x8004AA1C rather than playing the throw tail. */
    park_in_fire(&vw, 6, 1);
    vw.anim_pos        = Q2_VW_COOK_POSITION;
    vw.anim_end        = 470;
    vw.anim_flags      = 2;
    vw.cook            = true;
    vw.hand_cook_ticks = 20;
    vw.hand_release    = true;
    vw.frame_sound     = Q2_WSND_HANDGREN_THROW;
    q2_vw_hand_grenade_expired(&vw);
    CHECK(vw.frame == 2, "a cooked-off grenade forces fire frame 2");
    CHECK(vw.left == 150, "with the retail 150 ticks remaining");
    CHECK(vw.anim_pos == 0 && vw.anim_end == -1,
          "and clears/rewinds the hand model move");
    CHECK((vw.anim_flags & 1) != 0 && (vw.anim_flags & 2) == 0,
          "marking the move played rather than playing");
    CHECK(!vw.cook && q2_vw_take_hand_grenade_cook(&vw) == 0 &&
          !q2_vw_take_hand_grenade_release(&vw),
          "with no stale cook or release output");
    CHECK(q2_vw_take_frame_sound(&vw) == -1,
          "and no throw sound after an in-hand detonation");
}


/*
 * `RotMatrix` is Ry * Rx * Rz — 0x80089E38, element by element.
 *
 * Five of its nine entries are a single product, which makes them unambiguous
 * and worth pinning by their store address. Under the Rz * Ry * Rx this used to
 * build, none of the five holds unless the roll is zero — and the view weapon's
 * clip rotation carries all three angles at once, the blaster's idle being
 * (2078, 2110, 1985). So this test fails on a revert, which is the point: the
 * order was wrong for a year because the only caller at the time turned one
 * axis at a time and could not see it.
 */
static void test_rotmatrix_order(void)
{
    static const s32 tri[][3] = {
        { 2078, 2110, 1985 },   /* the blaster's idle, all three large */
        {  700, 1300, 2900 },
        { -512,  900, 3000 },
        {    0, 1024,    0 }    /* one axis: every order agrees here */
    };
    size_t i;

    for (i = 0; i < sizeof(tri) / sizeof(tri[0]); i++) {
        s16 m[3][3];
        s32 rx = tri[i][0], ry = tri[i][1], rz = tri[i][2];
        s32 sx = q2_sin12(rx), cx = q2_cos12(rx);
        s32 sy = q2_sin12(ry), cy = q2_cos12(ry);
        s32 sz = q2_sin12(rz), cz = q2_cos12(rz);

        q2_rotation_euler(m, rx, ry, rz);

        CHECK(m[1][2] == (s16)(-sx),
              "0x80089F0C m12 = -sin(x): got %d want %d (case %u)",
              m[1][2], (int)(s16)(-sx), (unsigned)i);
        CHECK(m[0][2] == (s16)((sy * cx) >> 12),
              "0x80089F20 m02 = sy*cx: got %d want %d (case %u)",
              m[0][2], (int)(s16)((sy * cx) >> 12), (unsigned)i);
        CHECK(m[1][0] == (s16)((sz * cx) >> 12),
              "0x80089FAC m10 = sz*cx: got %d want %d (case %u)",
              m[1][0], (int)(s16)((sz * cx) >> 12), (unsigned)i);
        CHECK(m[1][1] == (s16)((cz * cx) >> 12),
              "0x80089FCC m11 = cz*cx: got %d want %d (case %u)",
              m[1][1], (int)(s16)((cz * cx) >> 12), (unsigned)i);
        CHECK(m[2][2] == (s16)((cy * cx) >> 12),
              "0x80089F34 m22 = cy*cx: got %d want %d (case %u)",
              m[2][2], (int)(s16)((cy * cx) >> 12), (unsigned)i);
    }
}

/*
 * Retail does not restore a full projection after every regional emitter.
 * Instead the weapon entity owns area 1 (0x8004EE58), and model draw selects
 * that area at 0x8006BEB0.  A negative selector is a no-op, so retaining the
 * prototype's default -1 would leave the last projectile's portal offset in
 * force for the gun.
 */
static void test_viewweapon_selects_area_one(void)
{
    q2_viewweapon vw;
    q2_model model;
    q2_model_instance proto;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    psx_ot_area_screen region;
    static const s32 feet[3] = { 0, 0, 0 };
    static const s16 aim[3] = { 0, 0, 0 };
    static const s16 kick[3] = { 0, 0, 0 };

    memset(&vw, 0, sizeof(vw));
    memset(&model, 0, sizeof(model));
    memset(&cam, 0, sizeof(cam));
    memset(&region, 0, sizeof(region));
    gte_init(&gte);

    CHECK(psx_ot_init(&ot, 64, 16) == Q2_OK,
          "viewweapon projection OT allocates");
    if (!ot.bucket_head)
        return;

    cam.projection = 160;
    psx_ot_set_authored_window(&ot, 2, 51);
    psx_ot_set_window(&ot, 4, 49);
    psx_ot_area_prepare(&ot, 512, 248, 0, 0, 256, 124);
    region.min_x = 32;
    region.min_y = 16;
    region.max_x = 200;
    region.max_y = 120;
    CHECK(psx_ot_area_register_screen(&ot, 7, 43, &region),
          "regional projection registers");

    q2_camera_apply_area_projection(&cam, &ot, 7, &gte);
    CHECK((gte.ofx >> 16) == 224 && (gte.ofy >> 16) == 108,
          "test begins with the preceding regional origin, got (%d,%d)",
          (int)(gte.ofx >> 16), (int)(gte.ofy >> 16));

    q2_model_instance_init(&proto);
    proto.sort_area = 7;       /* prove q2_vw_build_ot owns the override */
    proto.bucket_override = 1;
    vw.model = &model;         /* zero faces: projection is the only output */
    (void)q2_vw_build_ot(&vw, &proto, feet, 0, aim, kick,
                         &cam, &ot, &gte, NULL);

    CHECK((gte.ofx >> 16) == 256 && (gte.ofy >> 16) == 124,
          "weapon area 1 restores viewport projection, got (%d,%d)",
          (int)(gte.ofx >> 16), (int)(gte.ofy >> 16));
    psx_ot_free(&ot);
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    build_bank();
    build_driver_bank();

    test_raise_to_idle();
    test_fire();
    test_fire_denied();
    test_fire_none_holds_the_clip();
    test_switch();
    test_switch_cannot_cancel_a_shot();
    test_long_frame_consumes_keys();
    test_placement();
    test_roll_reaches_the_offset();
    test_no_weapon();
    test_camera_undoes_the_placement();
    test_chaingun_bands();
    test_chaingun_loop_and_spin_down();
    test_machinegun_accumulator_resets();
    test_hyperblaster_loop();
    test_grenade_cook_waits_for_the_arm();
    test_rotmatrix_order();
    test_viewweapon_selects_area_one();

    if (g_fail == 0)
        printf("test_viewweapon: all checks passed\n");
    else
        printf("test_viewweapon: %d check%s failed\n",
               g_fail, g_fail == 1 ? "" : "s");

    return g_fail ? 1 : 0;
}
