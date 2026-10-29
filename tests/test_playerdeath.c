/*
 * test_playerdeath.c — the player death chain's behaviour.
 *
 * Everything asserted here was read out of five functions in SLES_015.34 —
 * 0x800396AC, 0x80039550, 0x8003E238, 0x8005B358 and 0x8003CE14 — and the
 * comments in playerdeath.h carry the addresses. What the tests are for is the
 * behaviour that is easy to get subtly wrong and impossible to see in a
 * screenshot: which deaths make a sound, which one-shots really are one-shots,
 * and the two different endings single player and deathmatch give a body.
 */
#include "playerdeath.h"

#include "monster.h"   /* q2_corpse_dissolve: the gate 0x8005B2A8 */
#include "pad.h"       /* Q2_PAD_FULL */
#include "sim.h"
#include "random.h"
#include "fixtures/player_animation_reference.h"

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

/* The frame step the sim runs at, in the 300-to-the-second clock. */
#define DT 10

static void test_native_animation(void)
{
    size_t i;
    for (i = 0; i < sizeof(native_player_animation) / sizeof(native_player_animation[0]); i++) {
        q2_player_anim_state state;
        state.move = (q2_player_move)native_player_animation[i].move;
        state.position = (s16)native_player_animation[i].position;
        state.flags = (u16)native_player_animation[i].flags;
        q2_random_seed(native_player_animation[i].seed);
        q2_player_anim_step(&state, native_player_ranges,
                           (q2_player_anim)native_player_animation[i].want,
                           native_player_animation[i].contact, native_player_animation[i].dt);
        CHECK(state.move == native_player_animation[i].result_move &&
              state.position == native_player_animation[i].result_position &&
              state.flags == native_player_animation[i].result_flags &&
              q2_random_state() == native_player_animation[i].result_seed,
              "native animation case %zu: got %d/%d/%u/%u expected %d/%d/%d/%u", i,
              state.move, state.position, state.flags, q2_random_state(),
              native_player_animation[i].result_move, native_player_animation[i].result_position,
              native_player_animation[i].result_flags, native_player_animation[i].result_seed);
    }
}

/* ------------------------------------------------------------------------- */
/* The moves                                                                  */
/* ------------------------------------------------------------------------- */

static void test_move_names(void)
{
    /* The order is the order 0x8003C5F8..0x8003CBFC looks them up in, which is
     * also their order in the pool at 0x800AC554, twelve bytes apart. */
    CHECK(strcmp(q2_player_move_name(Q2_PMOVE_STAND),  "Stand")   == 0, "0");
    CHECK(strcmp(q2_player_move_name(Q2_PMOVE_RUN),    "Run")     == 0, "1");
    CHECK(strcmp(q2_player_move_name(Q2_PMOVE_ATTAK),  "Attak")   == 0, "2");
    CHECK(strcmp(q2_player_move_name(Q2_PMOVE_DEATH1), "Death 1") == 0, "3");
    CHECK(strcmp(q2_player_move_name(Q2_PMOVE_DEATH2), "Death 2") == 0, "4");
    CHECK(strcmp(q2_player_move_name(Q2_PMOVE_DEATH3), "Death 3") == 0, "5");
    CHECK(strcmp(q2_player_move_name(Q2_PMOVE_JUMP),   "Jump")    == 0, "6");
    CHECK(strcmp(q2_player_move_name(Q2_PMOVE_PAIN1),  "Pain 1")  == 0, "7");
    CHECK(strcmp(q2_player_move_name(Q2_PMOVE_PAIN2),  "Pain 2")  == 0, "8");
    CHECK(strcmp(q2_player_move_name(Q2_PMOVE_PAIN3),  "Pain 3")  == 0, "9");
}

static void test_anim_choice(void)
{
    q2_player_move m;

    /* The three death moves are a `rand() % 3` (0x8003CEEC). */
    m = q2_player_anim_pick(Q2_PANIM_DEATH, Q2_PMOVE_STAND, 0, 0);
    CHECK(m == Q2_PMOVE_DEATH1, "roll 0 is Death 1, got %d", (int)m);
    m = q2_player_anim_pick(Q2_PANIM_DEATH, Q2_PMOVE_STAND, 0, 1);
    CHECK(m == Q2_PMOVE_DEATH2, "roll 1 is Death 2, got %d", (int)m);
    m = q2_player_anim_pick(Q2_PANIM_DEATH, Q2_PMOVE_STAND, 0, 2);
    CHECK(m == Q2_PMOVE_DEATH3, "roll 2 is Death 3, got %d", (int)m);
    m = q2_player_anim_pick(Q2_PANIM_DEATH, Q2_PMOVE_STAND, 0, 3);
    CHECK(m == Q2_PMOVE_DEATH1, "roll 3 wraps to Death 1, got %d", (int)m);

    /* And so are the pain moves (0x8003CF74). */
    m = q2_player_anim_pick(Q2_PANIM_PAIN, Q2_PMOVE_STAND, 0, 1);
    CHECK(m == Q2_PMOVE_PAIN2, "roll 1 is Pain 2, got %d", (int)m);
}

static void test_a_death_move_is_never_replaced(void)
{
    /* 0x8003CEB4 refuses to choose over one and 0x8003D158 refuses to install
     * over one. Both say the same thing, so the test asks for everything. */
    q2_player_anim a;

    for (a = Q2_PANIM_STAND; a <= Q2_PANIM_ATTACK; a++) {
        q2_player_move m = q2_player_anim_pick(a, Q2_PMOVE_DEATH2,
                                               Q2_PDEATH_ANIM_WRAPPED, 0);
        CHECK(m == Q2_PMOVE_NONE,
              "anim %d displaced a death move with %d", (int)a, (int)m);
    }
}

static void test_pain_holds_but_death_cuts_in(void)
{
    q2_player_move m;

    /* 0x8003D188: a pain move that has not wrapped holds. */
    m = q2_player_anim_pick(Q2_PANIM_RUN, Q2_PMOVE_PAIN1, 0, 0);
    CHECK(m == Q2_PMOVE_NONE, "run interrupted an unfinished pain, got %d",
          (int)m);

    /* 0x8003D1D8: DEATH is the exception. */
    m = q2_player_anim_pick(Q2_PANIM_DEATH, Q2_PMOVE_PAIN1, 0, 0);
    CHECK(m == Q2_PMOVE_DEATH1, "death did not cut into pain, got %d", (int)m);

    /* Once it has wrapped, anything may follow it. */
    m = q2_player_anim_pick(Q2_PANIM_RUN, Q2_PMOVE_PAIN1,
                            Q2_PDEATH_ANIM_WRAPPED, 0);
    CHECK(m == Q2_PMOVE_RUN, "a finished pain did not release, got %d", (int)m);
}

static void test_stand_does_not_cut_an_attack(void)
{
    /* 0x8003D008. */
    q2_player_move m = q2_player_anim_pick(Q2_PANIM_STAND, Q2_PMOVE_ATTAK,
                                           0, 0);
    CHECK(m == Q2_PMOVE_NONE, "stand cut an unwrapped attack, got %d", (int)m);

    m = q2_player_anim_pick(Q2_PANIM_STAND, Q2_PMOVE_ATTAK,
                            Q2_PDEATH_ANIM_WRAPPED, 0);
    CHECK(m == Q2_PMOVE_STAND, "a wrapped attack did not release, got %d",
          (int)m);
}

/* ------------------------------------------------------------------------- */
/* The gate                                                                   */
/* ------------------------------------------------------------------------- */

static void test_the_gate(void)
{
    /* 0x8003ADC0 is `bgtz`, so zero health is dead. */
    CHECK(!q2_player_should_die(1, 0), "1 health is alive");
    CHECK(q2_player_should_die(0, 0), "0 health is dead");
    CHECK(q2_player_should_die(-353, 0), "negative health is dead");

    /* 0x8003ADD4: once the corpse think has raised the bit, the gate is shut —
     * which is what stops the handler running twice. */
    CHECK(!q2_player_should_die(-1, Q2_PDEATH_DEAD_BIT),
          "the DEAD bit did not shut the gate");
}

/* ------------------------------------------------------------------------- */
/* The handler                                                                */
/* ------------------------------------------------------------------------- */

static void test_only_a_death_with_no_killer_cries_out(void)
{
    q2_player_death       d;
    q2_player_death_event ev;

    /* 0x80039728: shot by player 2, no sound at all. */
    q2_player_death_init(&d);
    q2_player_die(&d, 2, Q2_MP_MOD_SELF_LAST + 8 /* an ordinary weapon */,
                  0, true, false, &ev);
    CHECK(!ev.cried_out, "a death with a killer cried out");

    /* A raw -1 raises the voice. The world does not leave one — the damage
     * function writes 4, 23 or the victim's own index — and the only store of
     * -1 in the byte is 0x800396DC's own, for acid and lava; this hands the
     * handler the byte as that store would leave it. */
    q2_player_death_init(&d);
    q2_player_die(&d, -1, 0, 0, true, false, &ev);
    CHECK(ev.cried_out, "a world kill did not cry out");
    CHECK(!ev.drowned, "it should be the ordinary voice");

    /* client+0x84 picks the second one. */
    q2_player_death_init(&d);
    q2_player_die(&d, -1, 0, 0, true, true, &ev);
    CHECK(ev.drowned, "drowning did not pick the drowning voice");
}

static void test_lava_erases_the_killer_on_the_entity(void)
{
    q2_player_death       d;
    q2_player_death_event ev;

    /* 0x800396CC. Player 3 pushed you in; the lava killed you, and the lava is
     * nobody. Because the correction lands on the entity, the death also cries
     * out — which it would not have done had player 3 kept the credit. */
    q2_player_death_init(&d);
    q2_player_die(&d, 3, Q2_MP_MOD_SELF_LAST /* lava */, 0, true, false, &ev);
    CHECK(d.killer == -1, "lava left a killer of %d", (int)d.killer);
    CHECK(ev.cried_out, "an unattributed death did not cry out");
    CHECK(ev.frag_hook, "the hook is still called, with -1");
    CHECK(ev.frag_killer == -1, "the hook got killer %d", ev.frag_killer);

    /* And an ordinary means of death leaves the attacker alone. */
    q2_player_death_init(&d);
    q2_player_die(&d, 3, Q2_MP_MOD_SELF_LAST + 8, 0, true, false, &ev);
    CHECK(d.killer == 3, "an ordinary kill lost its killer, got %d",
          (int)d.killer);
}

/*
 * BOTH GATES READ THE RAW BYTE, and only 0x800396CC edits it first.
 *
 *     800396DC  sb    -1, 222(s0)     ; mods 9 and 10 only
 *     800396EC  lb    s1, 222(s0)
 *     80039728  bne   s1, -1          ; the voice
 *     80039774  slti  v0, s1, 4       ; the frag hook, signed
 *
 * The handler used to fold the byte through `q2_mp_attribute_kill`, whose
 * [0, 4) bound maps everything else to -1 too. The damage function records 4
 * for a creature's hit and 23 for a single-player world hit (combat.h,
 * `last_attacker`) — so every one of those deaths cried out, and in deathmatch
 * called the hook as a world kill, where the console does neither.
 *
 * Every CHECK below fails against that fold. The acid and lava halves are
 * paired with the same byte under an ordinary mod so that each CHECK also fails
 * an implementation that forgot 0x800396CC.
 */
static void test_the_gates_read_the_raw_byte(void)
{
    q2_player_death       d;
    q2_player_death_event ev;
    bool acid_cry, lava_cry, crush_cry, acid_frag, crush_frag;
    int  acid_killer;

    /* The voice, on its own: equality with -1 and nothing wider. */
    CHECK(!q2_player_death_cries_out((s8)Q2_MP_NOT_A_PLAYER, Q2_MOD_MELEE),
          "a creature's claw (byte 4) cried out");
    CHECK(!q2_player_death_cries_out(23, Q2_MOD_CRUSH),
          "a single-player crusher (byte 23, 0x80057EB8) cried out");

    acid_cry  = q2_player_death_cries_out((s8)Q2_MP_NOT_A_PLAYER, Q2_MOD_ACID);
    lava_cry  = q2_player_death_cries_out((s8)Q2_MP_NOT_A_PLAYER, Q2_MOD_LAVA);
    crush_cry = q2_player_death_cries_out((s8)Q2_MP_NOT_A_PLAYER, Q2_MOD_CRUSH);
    CHECK(acid_cry && lava_cry && !crush_cry,
          "byte 4 cries for acid %d, lava %d, crush %d — want 1 1 0",
          (int)acid_cry, (int)lava_cry, (int)crush_cry);

    /* Through the handler, in deathmatch, victim 1. The byte-4 death first. */
    q2_player_death_init(&d);
    q2_player_die(&d, (s8)Q2_MP_NOT_A_PLAYER, Q2_MOD_CRUSH, 1, true, false,
                  &ev);
    crush_cry  = ev.cried_out;
    crush_frag = ev.frag_hook;
    CHECK(d.killer == Q2_MP_NOT_A_PLAYER,
          "byte 4 became %d on the entity", (int)d.killer);
    CHECK(!crush_cry, "a deathmatch death credited to byte 4 cried out");
    CHECK(ev.body_recorded && !crush_frag,
          "a deathmatch death credited to byte 4 should push its body "
          "(0x8003976C, %d) and call no hook, but called it with %d",
          (int)ev.body_recorded, ev.frag_killer);

    /* The same byte under acid: 0x800396DC makes it -1, which passes. */
    q2_player_death_init(&d);
    q2_player_die(&d, (s8)Q2_MP_NOT_A_PLAYER, Q2_MOD_ACID, 1, true, false,
                  &ev);
    acid_cry    = ev.cried_out;
    acid_frag   = ev.frag_hook;
    acid_killer = ev.frag_killer;
    CHECK(acid_cry && !crush_cry,
          "acid should cry and the crusher not: acid %d, crush %d",
          (int)acid_cry, (int)crush_cry);
    CHECK(acid_frag && acid_killer == -1 && !crush_frag,
          "acid should reach the hook with -1 and the crusher not at all: "
          "acid %d (killer %d), crush %d",
          (int)acid_frag, acid_killer, (int)crush_frag);
}

static void test_a_fresh_spawn_owes_nobody_a_frag(void)
{
    q2_player_death       d;
    q2_player_death_event ev;

    /* 0x8003DE34 writes 4, not -1, and the hook's own `killer < 4` rejects it.
     * So a player who dies without ever being hit — walking into a crusher on
     * the first tick — scores for nobody rather than for player -1. */
    q2_player_death_init(&d);
    CHECK(d.killer == Q2_PDEATH_NO_KILLER, "a fresh spawn starts at %d",
          (int)d.killer);

    /*
     * THE TWO BELOW REPLACE THREE THAT ASSERTED THE OPPOSITE OF THE COMMENT
     * ABOVE: that the 4 came back as -1 and reached the hook, with player 0 as
     * its victim. It did, because the handler folded
     * its byte through `q2_mp_attribute_kill`'s [0, 4) bound. The console does
     * not: 0x800396EC loads the byte with `lb` and 0x80039774 tests it with
     * `slti s1, 4`, so a raw 4 is kept, fails the bound and calls nothing.
     * 0x8003976C's body record is ahead of that test and still happens.
     */
    q2_player_die(&d, (s8)Q2_PDEATH_NO_KILLER, 0, 0, true, false, &ev);
    CHECK(d.killer == Q2_PDEATH_NO_KILLER,
          "the handler keeps the raw byte, got %d", (int)d.killer);
    CHECK(ev.body_recorded && !ev.frag_hook,
          "the body is recorded (%d) ahead of the bound, which the sentinel "
          "then fails — but it reached the frag hook with %d",
          (int)ev.body_recorded, ev.frag_killer);
}

/*
 * THE SAME 4 FROM THE ACTOR THE CLIENT ACTUALLY HANDS OVER. The test above
 * passes Q2_PDEATH_NO_KILLER by hand; production passes the damage actor's
 * `last_attacker` and `last_mod`, and those used to come out of
 * q2_actor_init — and out of every q2_actor_from_player refresh — as -1 and 0:
 * a death that cried out and, in deathmatch, reached the hook as a suicide.
 */
static void test_a_fresh_actor_owes_nobody_a_frag(void)
{
    q2_actor              a;
    q2_inventory          inv;
    q2_player_death       d;
    q2_player_death_event ev;

    q2_inventory_init(&inv);
    q2_actor_init(&a);                       /* a new body, as 0x8003DDF8 */
    q2_actor_from_player(&a, &inv, NULL);    /* and the refresh on top    */

    q2_player_death_init(&d);
    q2_player_die(&d, a.last_attacker, a.last_mod, 0, true, false, &ev);
    CHECK(!ev.cried_out && !ev.frag_hook,
          "a fresh actor's byte %d cried out (%d) or reached the hook (%d, "
          "killer %d)", (int)a.last_attacker, (int)ev.cried_out,
          (int)ev.frag_hook, ev.frag_killer);
}

static bool death_sound_raised(const q2_sim *sim)
{
    const q2_ent_events *ev = q2_sim_entity_events(sim);
    u32 i;

    if (!ev)
        return false;
    for (i = 0; i < ev->count; i++)
        if (ev->e[i].kind == Q2_ENT_EVENT_SOUND &&
            ev->e[i].sound == Q2_SND_DEATH)
            return true;
    return false;
}

/*
 * END TO END: a refresh between the killing hit and the tick that notices it.
 *
 * A creature's shot kills the player (byte 4 in single player). Before the
 * sim's next tick, something refreshes the damage actor without hitting it —
 * q2_sim_fire does on every shot, and the owner's splash on every detonation
 * of the player's own projectile, near or far. update_pain then asks
 * q2_player_death_cries_out about the byte the refresh left. It used to leave
 * q2_actor_init's -1, and the player cried out where 0x80039728 is silent.
 */
static void test_a_refresh_does_not_give_the_player_a_voice(void)
{
    q2_sim   sim;
    q2_input in;
    q2_actor soldier;
    s32      spawn[3] = { 0, 0, 0 };

    memset(&in, 0, sizeof(in));
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    sim.combat.inv.armour = 0;

    q2_actor_init(&soldier);                 /* no client, owner -1 */
    soldier.health = 100;
    q2_sim_hurt_player(&sim, &soldier, 200, Q2_MOD_BULLET, NULL);

    q2_actor_from_player(&sim.combat.self, &sim.combat.inv,
                         sim.player[sim.cur_player].pos);
    q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);

    CHECK(sim.combat.inv.health <= 0 && !death_sound_raised(&sim),
          "a creature's kill cried out after a refresh: health %d, byte %d, "
          "mod %d", (int)sim.combat.inv.health,
          (int)sim.combat.self.last_attacker, (int)sim.combat.self.last_mod);
}

static void test_the_handler_runs_once(void)
{
    q2_player_death       d;
    q2_player_death_event ev;

    q2_player_death_init(&d);
    q2_player_die(&d, 1, 18, 0, true, false, &ev);
    CHECK(ev.frag_hook, "the first death scored");
    CHECK(d.stage == Q2_PDEATH_DYING, "the body is not dying, stage %d",
          (int)d.stage);

    /* The original swaps +0x3C, so the player think that got here is gone. A
     * second call must do nothing — not score a second frag. */
    q2_player_die(&d, 1, 18, 0, true, false, &ev);
    CHECK(!ev.frag_hook, "the handler scored twice");
    CHECK(!ev.death_page, "the handler opened a page twice");
}

static void test_single_player_opens_the_page_and_arms_the_walk_back(void)
{
    q2_player_death       d;
    q2_player_death_event ev;

    q2_player_death_init(&d);
    q2_player_die(&d, -1, 0, 0, false /* single player */, false, &ev);
    CHECK(ev.death_page, "single player did not open page 41");
    CHECK(ev.abandon_armed, "single player did not arm the walk-back");
    CHECK(!ev.frag_hook, "single player called the frag hook");
    CHECK(!ev.body_recorded, "single player pushed a body record");

    /* And deathmatch does neither of the first two. */
    q2_player_death_init(&d);
    q2_player_die(&d, -1, 0, 0, true, false, &ev);
    CHECK(!ev.death_page, "deathmatch opened the death page");
    CHECK(!ev.abandon_armed, "deathmatch armed the walk-back");
    CHECK(ev.body_recorded, "deathmatch did not push a body record");
}

static void test_the_handler_reuses_the_weapon_model_field(void)
{
    q2_player_death d;

    /* 0x800397F8: the linked weapon model is released and the same word
     * becomes the gib threshold. */
    q2_player_death_init(&d);
    CHECK(d.linked_weapon, "a live player has a linked weapon model");
    q2_player_die(&d, 1, 18, 0, false, false, NULL);
    CHECK(!d.linked_weapon, "the weapon model was not released");
    CHECK(d.gib_health == Q2_PDEATH_GIB_HEALTH, "gib_health is %d",
          (int)d.gib_health);
    CHECK(d.corpse_ticks == Q2_PDEATH_CORPSE_TICKS, "corpse timer is %d",
          (int)d.corpse_ticks);
    CHECK(!d.has_body, "the player record still points at a body");
}

/* ------------------------------------------------------------------------- */
/* The body                                                                   */
/* ------------------------------------------------------------------------- */

static void test_single_player_body_lies_where_it_fell(void)
{
    q2_player_death d;
    int             i;

    q2_player_death_init(&d);
    q2_player_die(&d, -1, 0, 0, false, false, NULL);

    /* 0x80039610 sends single player straight to `ent2 |= DEAD` and nowhere
     * else: respawn_think is only reached through the deathmatch branch, so no
     * amount of time fades the body. */
    for (i = 0; i < 1000; i++)
        CHECK(q2_player_death_tick(&d, -10, DT, false, 0),
              "the single-player body left the world on tick %d", i);

    CHECK(d.stage == Q2_PDEATH_DYING, "stage is %d", (int)d.stage);
    CHECK((d.ent2 & Q2_PDEATH_DEAD_BIT) != 0, "the DEAD bit was never raised");
    CHECK(d.scale == Q2_PDEATH_SCALE_ONE, "the body shrank in single player");
}

static void test_the_dead_bit_waits_for_the_animation_in_deathmatch(void)
{
    q2_player_death d;

    q2_player_death_init(&d);
    q2_player_die(&d, 1, 18, 0, true, false, NULL);

    /* 0x80039618: while +0x102 is clear the body is still falling. */
    q2_player_death_tick(&d, -10, DT, true, 0);
    CHECK(d.move == Q2_PMOVE_DEATH1, "no death move was installed, got %d",
          (int)d.move);
    CHECK((d.ent2 & Q2_PDEATH_DEAD_BIT) == 0,
          "DEAD was raised before the animation ended");
    CHECK(d.stage == Q2_PDEATH_DYING, "stage is %d", (int)d.stage);

    /* Asking again while it plays does not restart it. */
    q2_player_death_tick(&d, -10, DT, true, 2);
    CHECK(d.move == Q2_PMOVE_DEATH1, "the death move was restarted as %d",
          (int)d.move);

    /* 0x8003DF90 raises bit 0 when the cursor walks past the end. */
    q2_player_death_anim_ended(&d);
    q2_player_death_tick(&d, -10, DT, true, 0);
    CHECK((d.ent2 & Q2_PDEATH_DEAD_BIT) != 0, "DEAD was not raised");
    CHECK((d.ent2 & Q2_PDEATH_SETTLED_BIT) != 0, "0x8000 was not raised");
    CHECK(d.box_y == Q2_PDEATH_BODY_BOX_Y, "the box was not flattened, %d",
          (int)d.box_y);
    CHECK(d.stage == Q2_PDEATH_DOWN, "stage is %d", (int)d.stage);
}

static void test_the_body_dissolves_after_its_five_seconds(void)
{
    q2_player_death d;
    int             i;
    int             fade_started = -1;
    int             gone         = -1;

    q2_player_death_init(&d);
    q2_player_die(&d, 1, 18, 0, true, false, NULL);
    q2_player_death_anim_ended(&d);

    for (i = 0; i < 400; i++) {
        bool alive = q2_player_death_tick(&d, -10, DT, true, 0);

        if (fade_started < 0 && d.stage == Q2_PDEATH_FADING)
            fade_started = i;
        if (!alive) {
            gone = i;
            break;
        }
    }

    /* Tick 0 is the one that finds the animation over and moves the body to
     * DOWN; ticks 1..150 spend the 1500. */
    CHECK(fade_started == Q2_PDEATH_CORPSE_TICKS / DT,
          "the fade started on tick %d, expected %d", fade_started,
          Q2_PDEATH_CORPSE_TICKS / DT);

    /* 4096 of scale at dt*16 a tick — 25.6 ticks, so 26 of them. */
    {
        const int step = DT * Q2_PDEATH_FADE_RATE;
        const int fade = (Q2_PDEATH_SCALE_ONE + step - 1) / step;

        CHECK(gone == fade_started + fade,
              "the body left on tick %d, expected %d", gone,
              fade_started + fade);
    }
    CHECK(d.stage == Q2_PDEATH_GONE, "stage is %d", (int)d.stage);
}

static void test_a_body_can_still_be_gibbed(void)
{
    q2_player_death d;

    /* 0x8003E270 repeats corpse_think's own test, so a rocket into a body that
     * is already down still takes it apart. */
    q2_player_death_init(&d);
    q2_player_die(&d, 1, 18, 0, true, false, NULL);
    q2_player_death_anim_ended(&d);
    q2_player_death_tick(&d, -10, DT, true, 0);
    CHECK(d.stage == Q2_PDEATH_DOWN, "stage is %d", (int)d.stage);

    CHECK(!q2_player_death_tick(&d, Q2_PDEATH_GIB_HEALTH, DT, true, 0),
          "the body survived being gibbed");
    CHECK(d.stage == Q2_PDEATH_GIBBED, "stage is %d", (int)d.stage);

    /* -40 exactly is gibbed: `gib_health < health` is what the original calls
     * alive, so the boundary belongs to the gib. */
    q2_player_death_init(&d);
    q2_player_die(&d, 1, 18, 0, true, false, NULL);
    q2_player_death_tick(&d, Q2_PDEATH_GIB_HEALTH + 1, DT, true, 0);
    CHECK(d.stage == Q2_PDEATH_DYING, "-39 should not gib, stage %d",
          (int)d.stage);
}

/* ------------------------------------------------------------------------- */
/* The dissolve gate, 0x8005B2A8, from respawn_think (0x8003E244)             */
/* ------------------------------------------------------------------------- */

/* A deathmatch body that has finished falling: respawn_think owns it. */
static void body_down(q2_player_death *d)
{
    q2_player_death_init(d);
    q2_player_die(d, 1, 18, 0, true, false, NULL);
    q2_player_death_anim_ended(d);
    q2_player_death_tick(d, -10, DT, true, 0);
}

static void test_a_marked_body_dissolves(void)
{
    static const u8 fx0[6] = { 15, 0, 0, 0, 0, 0 };   /* Q2_MOD_2's timer */
    q2_player_death d;
    int             i;
    int             gone = -1;
    bool            alive;

    body_down(&d);

    /*
     * The gate's tick. The body is handed to 0x8005B39C with 4096 in +0xF4,
     * and 0x8003E24C's jump lands on the shared tail, so this tick's dt comes
     * off that 4096 at once: the corpse timer IS the dissolve level.
     */
    alive = q2_player_death_tick_fx(&d, -10, DT, true, 0, fx0);
    CHECK(alive && d.stage == Q2_PDEATH_DISSOLVING,
          "effect[0]: the gate's tick left stage %d (alive %d)",
          (int)d.stage, (int)alive);
    CHECK(d.dissolve_arm == Q2_CORPSE_DISSOLVE_FX0,
          "effect[0] did not install 0x8005B39C, arm %d", (int)d.dissolve_arm);
    CHECK(d.corpse_ticks == Q2_CORPSE_DISSOLVE_LEVEL - DT,
          "the tail did not take dt off the gate's 4096: %d",
          (int)d.corpse_ticks);

    /* 4086 at dt << 6 = 640 a tick: gone on the seventh run. */
    for (i = 1; i <= 20; i++) {
        if (!q2_player_death_tick_fx(&d, -10, DT, true, 0, fx0)) {
            gone = i;
            break;
        }
        CHECK(d.stage == Q2_PDEATH_DISSOLVING,
              "the dissolving body changed stage to %d on run %d",
              (int)d.stage, i);
    }
    CHECK(gone == 7, "the body was freed on run %d, expected 7", gone);
    CHECK(d.stage == Q2_PDEATH_GONE && d.scale == Q2_PDEATH_SCALE_ONE,
          "stage %d, scale %d: the dissolved body should be GONE without "
          "darkening, since only body_fade touches +0xFC",
          (int)d.stage, (int)d.scale);
    CHECK(!q2_player_death_tick_fx(&d, -10, DT, true, 0, fx0),
          "a freed body came back");

    /*
     * THE CONSOLE'S FRAME COUNT at the nominal dt of 12 (Q2_DT_NOMINAL):
     * 4096 - 12 on the gate's tick, then 768 a run, so the sixth run frees
     * the body. That is effect.h's "six frames".
     */
    body_down(&d);
    q2_player_death_tick_fx(&d, -10, 12, true, 0, fx0);
    gone = -1;
    for (i = 1; i <= 20; i++) {
        if (!q2_player_death_tick_fx(&d, -10, 12, true, 0, fx0)) {
            gone = i;
            break;
        }
    }
    CHECK(gone == 6 && d.stage == Q2_PDEATH_GONE,
          "at dt 12 the body was freed on run %d (stage %d), expected 6",
          gone, (int)d.stage);
}

static void test_an_unmarked_body_waits_as_before(void)
{
    /* effect[1], [3], [4] and [5] do not open the gate: it reads +0x2F2 and
     * +0x2F0 only. */
    static const u8 others[6] = { 0, 3, 0, 9, 5, 7 };
    q2_player_death d;
    int             i;
    int             fade_started = -1;

    body_down(&d);
    for (i = 1; i <= Q2_PDEATH_CORPSE_TICKS / DT + 5; i++) {
        q2_player_death_tick_fx(&d, -10, DT, true, 0, others);
        if (d.stage != Q2_PDEATH_DOWN) {
            fade_started = i;
            break;
        }
    }
    CHECK(d.dissolve_arm == Q2_CORPSE_DISSOLVE_NONE,
          "effect[1]/[3]/[4]/[5] opened the gate, arm %d",
          (int)d.dissolve_arm);
    CHECK(d.stage == Q2_PDEATH_FADING,
          "an unmarked body went to stage %d, not body_fade", (int)d.stage);
    CHECK(fade_started == Q2_PDEATH_CORPSE_TICKS / DT,
          "it waited %d ticks, not the 1500's %d", fade_started,
          Q2_PDEATH_CORPSE_TICKS / DT);
}

static void test_effect2_takes_the_gate_first(void)
{
    static const u8 fx2[6]  = { 0, 0, 30, 0, 0, 0 };  /* Q2_MOD_4's timer */
    static const u8 both[6] = { 15, 0, 30, 0, 0, 0 };
    q2_player_death d;

    /* 0x8005B2B4 reads +0x2F2 before 0x8005B2D4 reads +0x2F0. */
    body_down(&d);
    q2_player_death_tick_fx(&d, -10, DT, true, 0, fx2);
    CHECK(d.dissolve_arm == Q2_CORPSE_DISSOLVE_FX2,
          "effect[2] did not install 0x8005B444, arm %d", (int)d.dissolve_arm);

    body_down(&d);
    q2_player_death_tick_fx(&d, -10, DT, true, 0, both);
    CHECK(d.dissolve_arm == Q2_CORPSE_DISSOLVE_FX2,
          "with both set the arm is %d, not effect[2]'s", (int)d.dissolve_arm);
    CHECK(d.stage == Q2_PDEATH_DISSOLVING, "stage is %d", (int)d.stage);
}

static void test_the_gate_comes_before_the_gib_test(void)
{
    static const u8 fx0[6] = { 15, 0, 0, 0, 0, 0 };
    q2_player_death d;
    int             i;
    bool            alive = true;

    /* 0x8003E24C jumps past the gib test at 0x8003E270, and the handler has
     * none: a body past -40 with effect[0] set dissolves, it is not thrown. */
    body_down(&d);
    CHECK(q2_player_death_tick_fx(&d, Q2_PDEATH_GIB_HEALTH - 20, DT, true, 0,
                                  fx0),
          "a marked body past -40 was gibbed on the gate's tick");
    CHECK(d.stage == Q2_PDEATH_DISSOLVING, "stage is %d", (int)d.stage);

    for (i = 0; i < 20 && alive; i++)
        alive = q2_player_death_tick_fx(&d, Q2_PDEATH_GIB_HEALTH - 20, DT, true,
                                        0, fx0);
    CHECK(!alive && d.stage == Q2_PDEATH_GONE,
          "the dissolving body ended as stage %d, not GONE", (int)d.stage);
}

static void test_a_falling_body_never_dissolves(void)
{
    static const u8 fx0[6] = { 15, 0, 0, 0, 0, 0 };
    q2_player_death d;
    int             i;

    /* corpse_think (0x80039550) makes no 0x8005B2A8 call, so the gate is
     * never asked while the body is still falling... */
    q2_player_death_init(&d);
    q2_player_die(&d, 1, 18, 0, true, false, NULL);
    for (i = 0; i < 50; i++)
        q2_player_death_tick_fx(&d, -10, DT, true, 0, fx0);
    CHECK(d.stage == Q2_PDEATH_DYING &&
          d.dissolve_arm == Q2_CORPSE_DISSOLVE_NONE,
          "a falling body left DYING for stage %d", (int)d.stage);

    /* ...and a single-player body never reaches respawn_think at all. */
    q2_player_death_init(&d);
    q2_player_die(&d, -1, 0, 0, false, false, NULL);
    for (i = 0; i < 1000; i++)
        q2_player_death_tick_fx(&d, -10, DT, false, 0, fx0);
    CHECK(d.stage == Q2_PDEATH_DYING &&
          d.dissolve_arm == Q2_CORPSE_DISSOLVE_NONE,
          "a single-player body left DYING for stage %d", (int)d.stage);
}

static void test_the_corpse_slows_down(void)
{
    q2_player_death d;

    q2_player_death_init(&d);
    q2_player_die(&d, 1, 18, 0, true, false, NULL);
    d.velocity[0] = 1200;
    d.velocity[1] = -20;
    d.velocity[2] = 0;

    /* 0x800395A0..0x800395B8: dt * 40 toward zero, clamped at zero. */
    q2_player_death_tick(&d, -10, DT, true, 0);
    CHECK(Q2_PDEATH_FRICTION == 40, "corpse braking omits the final shift");
    CHECK(d.velocity[0] == 1200 - DT * 40, "vx is %d",
          (int)d.velocity[0]);
    CHECK(d.velocity[1] == 0, "vy overshot zero to %d", (int)d.velocity[1]);
    CHECK(d.velocity[2] == 0, "vz moved off zero to %d", (int)d.velocity[2]);
}

/* ------------------------------------------------------------------------- */
/* The two endings                                                            */
/* ------------------------------------------------------------------------- */

static void test_the_walk_back_to_the_front_end(void)
{
    s32 ticks = 0;
    int i;

    /* 0x80041D38: an unarmed deadline never fires. */
    for (i = 0; i < 100; i++)
        CHECK(!q2_player_abandon_tick(&ticks, DT), "an unarmed deadline fired");

    ticks = Q2_PDEATH_ABANDON_TICKS;
    for (i = 0; i < Q2_PDEATH_ABANDON_TICKS / DT - 1; i++)
        CHECK(!q2_player_abandon_tick(&ticks, DT), "it fired early on %d", i);
    CHECK(q2_player_abandon_tick(&ticks, DT), "it never fired");

    /* 0x80041DC8 clears it, so it fires exactly once. */
    CHECK(!q2_player_abandon_tick(&ticks, DT), "it fired twice");
}

static void test_resupply_is_spent(void)
{
    int n = 2;

    /* 0x8001FF0C, and it is the only write to 0x800B335D in the executable. */
    CHECK(q2_player_spend_resupply(&n) && n == 1, "first spend left %d", n);
    CHECK(q2_player_spend_resupply(&n) && n == 0, "second spend left %d", n);

    /* The page greys the row at zero and takes it out of the navigation, so the
     * original cannot reach the decrement from empty. Clamping rather than
     * wrapping a byte to 255 is therefore not a behaviour change. */
    CHECK(!q2_player_spend_resupply(&n), "an empty resupply was spent");
    CHECK(n == 0, "an empty resupply wrapped to %d", n);
}


/* ------------------------------------------------------------------------- */
/* Nobody is driving                                                          */
/* ------------------------------------------------------------------------- */
/*
 * The two things a corpse must not do, and both of them come out of ONE fact:
 * `player_die` installs corpse_think (0x80039550) at 0x80039834, so
 * the player think at 0x8003A1C8 is not installed any more.
 *
 * 0x8003A4A4 is the pad read's only call site and 0x8003AD98 is the view
 * weapon driver's only call site, and BOTH are inside that function. So a
 * corpse is not steered and does not hold a gun, without either being a rule
 * anybody wrote down.
 */
static void test_a_corpse_is_not_driven_by_the_pad(void)
{
    q2_sim   driven, still;
    q2_input full, none;
    s16      yaw;
    int      i;

    memset(&none, 0, sizeof(none));
    full          = none;
    full.forward  = Q2_PAD_FULL;
    full.yaw      = Q2_PAD_FULL;
    full.buttons  = Q2_BTN_MOVING;

    /* Alive, that input turns the player — otherwise the rest of this test
     * would pass on a sim that ignored input altogether. */
    q2_sim_init(&driven, NULL, 50);
    yaw = (s16)driven.player[0].yaw;
    for (i = 0; i < 20; i++)
        q2_sim_tick(&driven, &full, 12);
    CHECK(driven.player[0].yaw != yaw, "a live player did not turn");

    /*
     * Dead, it changes nothing at all. Asserted by DIFFERENCE rather than by
     * an absolute position, because the body is still falling: two identical
     * corpses, one handed the stick at full deflection and one handed nothing,
     * must end the same twenty ticks in exactly the same state. Any difference
     * is input reaching a player who has none.
     */
    q2_sim_init(&driven, NULL, 50);
    q2_sim_init(&still,  NULL, 50);
    driven.player[0].ent2_flags |= Q2_ENT2_DEAD;
    still.player[0].ent2_flags  |= Q2_ENT2_DEAD;

    for (i = 0; i < 20; i++) {
        q2_sim_tick(&driven, &full, 12);
        q2_sim_tick(&still,  &none, 12);
    }

    CHECK(driven.player[0].yaw == still.player[0].yaw,
          "the stick turned a corpse: %d against %d",
          (int)driven.player[0].yaw, (int)still.player[0].yaw);
    CHECK(driven.player[0].pos[0] == still.player[0].pos[0] &&
          driven.player[0].pos[2] == still.player[0].pos[2],
          "the stick walked a corpse: (%d,%d) against (%d,%d)",
          (int)driven.player[0].pos[0], (int)driven.player[0].pos[2],
          (int)still.player[0].pos[0],  (int)still.player[0].pos[2]);
    CHECK(driven.player[0].wish[0] == 0 && driven.player[0].wish[2] == 0,
          "a corpse still had a wish velocity");
    CHECK(driven.player[0].jump_hold == 0, "a corpse was holding a jump");
}

static void test_the_view_weapon_is_freed_with_the_player(void)
{
    q2_player_death d;

    /*
     * `entity+0x44` is a pointer to the view weapon ENTITY — 0x8004EE0C opens
     * `s6 = self->[68]; s7 = s6->[12]`. 0x800397F8 hands it to 0x8006D280,
     * which pushes it back onto the free stack at 0x800B2BAC, and then the
     * same word becomes -40. So "there is a gun" and "the gib threshold" are
     * the same field either side of the death.
     */
    q2_player_death_init(&d);
    CHECK(d.linked_weapon, "a live player holds one");
    CHECK(d.gib_health == 0, "and the field is not a threshold yet");

    q2_player_die(&d, 1, 18, 0, false, false, NULL);
    CHECK(!d.linked_weapon, "the view weapon was not freed");
    CHECK(d.gib_health == Q2_PDEATH_GIB_HEALTH,
          "the same word did not become -40, it is %d", (int)d.gib_health);

    /* And a respawn hands one back, because 0x8003B250 builds a new entity. */
    q2_player_death_init(&d);
    CHECK(d.linked_weapon, "a respawned player has no gun");
}

/* ------------------------------------------------------------------------- */


/* ------------------------------------------------------------------------- */
/* The death camera's roll                                                    */
/* ------------------------------------------------------------------------- */
/*
 * 0x800380C8 (MoveQuakeCamera's no-body branch) and 0x80038664 (QuakeCamera's
 * Q2_ENT2_DEAD branch) are the same four instructions twice: seek view+16
 * toward -384 with a fraction of `50 * dt`, through SeekAng at 0x8006FAC8.
 *
 * The port had a flat 100 there — which is what that expression happens to give
 * at about 144 fps, and a fifth of what it gives at the console's own PAL rate.
 * Two things were wrong with it: the horizon took roughly 46 frames to lie over
 * where the console takes eight, and because the step was per RENDERED frame
 * with no dt in it, the same death rolled at a different speed on a fast
 * machine and a slow one. The console's rolls identically on both.
 */
static int roll_signed(s16 roll)
{
    return (((int)roll + 2048) & 0xFFF) - 2048;
}

static void test_the_death_camera_rolls_over(void)
{
    s16 fast = 0, slow = 0;
    int i;

    /* SeekAng itself: `(d * frac + 2047) >> 11`, so the fraction is in 1/2048
     * and a full one arrives in a single step. */
    CHECK(q2_pdeath_seek_ang(0, 1024, 2048) == 1024,
          "a fraction of 2048 should arrive, got %d",
          (int)q2_pdeath_seek_ang(0, 1024, 2048));
    CHECK(q2_pdeath_seek_ang(0, 0, 500) == 0, "already there stays put");

    /*
     * One frame at the sim's own 1/30 s step. dt 10 gives frac 500, and
     * 384 * 500 / 2048 rounds to 94 of the circle's 4096 steps. The constant
     * this replaced gave 19.
     */
    CHECK(roll_signed(q2_pdeath_cam_roll(0, DT)) == -94,
          "one frame of death roll moved %d, want -94",
          roll_signed(q2_pdeath_cam_roll(0, DT)));

    /* Eight frames is most of the arc. Forty-six used to be needed. */
    for (i = 0; i < 8; i++)
        fast = q2_pdeath_cam_roll(fast, DT);
    CHECK(roll_signed(fast) <= -320,
          "eight frames reached %d of -384", roll_signed(fast));

    /*
     * AND THE SAME ROLL IN THE SAME WALL TIME AT TWICE THE FRAME RATE, which is
     * the whole reason dt is in the expression: sixteen half-length frames land
     * within a degree of eight full ones. With the old fixed fraction the two
     * were eighty-odd units apart, because the ease counted frames rather than
     * time.
     */
    for (i = 0; i < 16; i++)
        slow = q2_pdeath_cam_roll(slow, DT / 2);
    CHECK(roll_signed(fast) - roll_signed(slow) <= 12 &&
          roll_signed(slow) - roll_signed(fast) <= 12,
          "30 Hz reached %d and 60 Hz reached %d over the same quarter second",
          roll_signed(fast), roll_signed(slow));

    /* And it settles ON -384 rather than creeping past it: at a remainder of
     * zero the `+2047` rounds to nothing and the angle stops. */
    fast = 0;
    for (i = 0; i < 200; i++)
        fast = q2_pdeath_cam_roll(fast, DT);
    CHECK(roll_signed(fast) == -384, "the roll settled at %d",
          roll_signed(fast));
}

int main(void)
{
    test_native_animation();
    test_move_names();
    test_anim_choice();
    test_a_death_move_is_never_replaced();
    test_pain_holds_but_death_cuts_in();
    test_stand_does_not_cut_an_attack();
    test_the_gate();
    test_only_a_death_with_no_killer_cries_out();
    test_lava_erases_the_killer_on_the_entity();
    test_the_gates_read_the_raw_byte();
    test_a_fresh_spawn_owes_nobody_a_frag();
    test_a_fresh_actor_owes_nobody_a_frag();
    test_a_refresh_does_not_give_the_player_a_voice();
    test_the_handler_runs_once();
    test_single_player_opens_the_page_and_arms_the_walk_back();
    test_the_handler_reuses_the_weapon_model_field();
    test_single_player_body_lies_where_it_fell();
    test_the_dead_bit_waits_for_the_animation_in_deathmatch();
    test_the_body_dissolves_after_its_five_seconds();
    test_a_body_can_still_be_gibbed();
    test_a_marked_body_dissolves();
    test_an_unmarked_body_waits_as_before();
    test_effect2_takes_the_gate_first();
    test_the_gate_comes_before_the_gib_test();
    test_a_falling_body_never_dissolves();
    test_the_corpse_slows_down();
    test_the_walk_back_to_the_front_end();
    test_resupply_is_spent();
    test_the_death_camera_rolls_over();
    test_a_corpse_is_not_driven_by_the_pad();
    test_the_view_weapon_is_freed_with_the_player();

    if (g_fail) {
        printf("\n%d player-death check%s failed\n", g_fail,
               g_fail == 1 ? "" : "s");
        return 1;
    }
    printf("player death: all checks passed\n");
    return 0;
}
