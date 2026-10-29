/*
 * test_multiplayer.c — the multiplayer runtime's behaviour, not its data.
 *
 * `q2psx-inspect multi <disc>` already checks the arena census, the module's
 * identity across the thirteen arenas and the front end's limit tables against
 * a real disc, so nothing here re-asserts a number that lives on the disc. What
 * it pins down is the behaviour read out of QMULTI.C's code: who gains and
 * loses a frag, when a match ends and how, which spawn a player arrives at, and
 * the two quirks the reconstruction deliberately keeps.
 */
#include "multiplayer.h"

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
/* Scoring                                                                    */
/* ------------------------------------------------------------------------- */

static void test_deathmatch_scoring(void)
{
    q2_mp_session s;

    q2_mp_session_init(&s, Q2_MP_DEATHMATCH, 4);

    q2_mp_player_killed(&s, 0, 1);
    CHECK(s.frags[0] == 1, "a kill is one frag, got %d", s.frags[0]);
    CHECK(s.frags[1] == 0, "the victim is untouched, got %d", s.frags[1]);

    /* A suicide costs the victim a frag. */
    q2_mp_player_killed(&s, 2, 2);
    CHECK(s.frags[2] == -1, "a suicide costs a frag, got %d", s.frags[2]);

    /* A world kill arrives as killer -1 and is charged to the victim, which
     * makes it a suicide by the same path. */
    q2_mp_player_killed(&s, -1, 3);
    CHECK(s.frags[3] == -1, "a world kill costs the victim a frag, got %d",
          s.frags[3]);

    /* The kill matrix is written on the diagonal — the original indexes it with
     * the victim twice. Keeping it is the point of the test. */
    CHECK(s.kills[1][1] == 1, "the matrix counts the victim's death");
    CHECK(s.kills[0][1] == 0, "the matrix is NOT [killer][victim]");
}

static void test_team_scoring(void)
{
    q2_mp_session s;

    q2_mp_session_init(&s, Q2_MP_TEAM_DEATHMATCH, 4);
    s.team[0] = 0;
    s.team[1] = 0;
    s.team[2] = 1;
    s.team[3] = 1;

    /* Across the line: the killer and the killer's team both gain. */
    q2_mp_player_killed(&s, 0, 2);
    CHECK(s.frags[0] == 1, "cross-team kill gains a frag, got %d", s.frags[0]);
    CHECK(s.team_frags[0] == 1, "and a team frag, got %d", s.team_frags[0]);

    /* A teammate: charged twice, personally and for the team. */
    q2_mp_player_killed(&s, 0, 1);
    CHECK(s.frags[0] == 0, "a team kill costs the killer a frag, got %d",
          s.frags[0]);
    CHECK(s.team_frags[0] == 0, "and costs the team one, got %d",
          s.team_frags[0]);
}

static void test_frag_limit(void)
{
    q2_mp_session s;
    int i;

    q2_mp_session_init(&s, Q2_MP_DEATHMATCH, 2);
    s.frag_limit = 3;

    for (i = 0; i < 2; i++)
        q2_mp_player_killed(&s, 0, 1);
    CHECK(s.end == Q2_MP_RUNNING, "two of three frags does not end it");

    q2_mp_player_killed(&s, 0, 1);
    CHECK(s.end == Q2_MP_END_FRAG_LIMIT, "the third frag ends it, end=%d",
          s.end);
    CHECK(strcmp(q2_mp_banner(&s), "GAME OVER") == 0, "the banner is GAME OVER");

    /* Once over, further kills are ignored outright. */
    q2_mp_player_killed(&s, 1, 0);
    CHECK(s.frags[1] == 0, "no scoring after the match ends, got %d",
          s.frags[1]);
}

static void test_frag_limit_is_equality(void)
{
    q2_mp_session s;

    /* The original tests `score != limit`, not `score < limit`, so a score that
     * jumps the limit — which a team frag lost to a teammate can arrange —
     * does not end the match. */
    q2_mp_session_init(&s, Q2_MP_DEATHMATCH, 2);
    s.frag_limit = 2;
    s.frags[0] = 5;

    q2_mp_player_killed(&s, 0, 1);
    CHECK(s.frags[0] == 6, "the frag still lands");
    CHECK(s.end == Q2_MP_RUNNING,
          "passing the limit without hitting it does not end the match");
}

static void test_no_frag_limit(void)
{
    q2_mp_session s;
    int i;

    q2_mp_session_init(&s, Q2_MP_DEATHMATCH, 2);
    s.frag_limit = Q2_MP_NO_LIMIT;

    for (i = 0; i < 50; i++)
        q2_mp_player_killed(&s, 0, 1);
    CHECK(s.end == Q2_MP_RUNNING, "NONE means the frag limit never fires");
}

/* ------------------------------------------------------------------------- */
/* The clock                                                                  */
/* ------------------------------------------------------------------------- */

static void test_time_limit(void)
{
    q2_mp_session s;

    q2_mp_session_init(&s, Q2_MP_DEATHMATCH, 2);
    s.time_limit = 1;                 /* one minute = 18000 dt units */

    CHECK(q2_mp_frame(&s, 17999, 6) == Q2_MP_REQ_NONE, "under the limit");
    CHECK(s.end == Q2_MP_RUNNING, "under the limit, still running");

    CHECK(q2_mp_frame(&s, 18000, 6) == Q2_MP_REQ_NONE,
          "exactly on the limit is not past it");
    CHECK(s.end == Q2_MP_RUNNING, "the compare is strict");

    q2_mp_frame(&s, 18001, 6);
    CHECK(s.end == Q2_MP_END_TIME_UP, "one unit past ends it, end=%d", s.end);
    CHECK(strcmp(q2_mp_banner(&s), "TIME UP") == 0, "the banner is TIME UP");
}

static void test_versus_has_no_clock(void)
{
    q2_mp_session s;

    /* 0x801021A0 forces the time limit to NONE when the mode is VERSUS. */
    q2_mp_session_init(&s, Q2_MP_VERSUS, 4);
    CHECK(s.time_limit == Q2_MP_NO_LIMIT, "VERSUS ships with no time limit");

    q2_mp_frame(&s, 99999999, 6);
    CHECK(s.end == Q2_MP_RUNNING, "and the clock never ends a VERSUS match");
}

/* ------------------------------------------------------------------------- */
/* Exit                                                                       */
/* ------------------------------------------------------------------------- */

static q2_mp_request run_banner(q2_mp_session *s)
{
    int i;
    q2_mp_request r = Q2_MP_REQ_NONE;

    /* 450 units at 6 per field is 75 fields; run a comfortable margin. */
    for (i = 0; i < 200 && r == Q2_MP_REQ_NONE; i++)
        r = q2_mp_frame(s, 0, 6);
    return r;
}

static void test_match_over_goes_to_the_scoreboard(void)
{
    q2_mp_session s;
    q2_mp_request r;

    q2_mp_session_init(&s, Q2_MP_DEATHMATCH, 2);
    s.frag_limit = 1;
    q2_mp_player_killed(&s, 0, 1);
    CHECK(s.end == Q2_MP_END_FRAG_LIMIT, "the match ended");

    /* The banner runs first; nothing is asked for while it is on screen. */
    CHECK(q2_mp_frame(&s, 0, 6) == Q2_MP_REQ_NONE, "the banner holds the frame");
    CHECK(s.banner_armed, "and arms itself");

    r = run_banner(&s);
    CHECK(r == Q2_MP_REQ_RESULTS,
          "a finished match asks for state 11, the MPResults screen, got %d", r);
    CHECK(q2_mp_take_request(&s) == Q2_MP_REQ_RESULTS, "and hands it over once");
    CHECK(q2_mp_take_request(&s) == Q2_MP_REQ_NONE, "and only once");
}

static void test_round_over_goes_round_again(void)
{
    q2_mp_session s;
    q2_mp_player_view v[Q2_MP_MAX_PLAYERS];
    q2_mp_request r;

    memset(v, 0, sizeof(v));

    q2_mp_session_init(&s, Q2_MP_VERSUS, 3);
    s.round_limit = 2;

    /* Player 1 is the only one left alive. */
    v[1].alive = true;
    q2_mp_versus_check(&s, v, 3);
    CHECK(s.team_frags[1] == 1, "the survivor takes the round, got %d",
          s.team_frags[1]);
    CHECK(s.end == Q2_MP_END_ROUND_OVER, "one round short of the limit, end=%d",
          s.end);
    CHECK(strcmp(q2_mp_banner(&s), "ROUND OVER") == 0, "the banner is ROUND OVER");

    r = run_banner(&s);
    CHECK(r == Q2_MP_REQ_RESTART_ROUND,
          "a finished round asks for state 19, the next round, got %d", r);

    /* Next round: the same survivor reaches the round limit and the match ends. */
    s.end = Q2_MP_RUNNING;
    s.banner_armed = false;
    s.banner_ticks = Q2_MP_BANNER_TICKS;
    q2_mp_take_request(&s);

    q2_mp_versus_check(&s, v, 3);
    CHECK(s.team_frags[1] == 2, "the second round win, got %d", s.team_frags[1]);
    CHECK(s.end == Q2_MP_END_MATCH_OVER, "which meets the round limit, end=%d",
          s.end);
    CHECK(strcmp(q2_mp_banner(&s), "GAME OVER") == 0, "the banner is GAME OVER");
    CHECK(run_banner(&s) == Q2_MP_REQ_RESULTS, "and it goes to the scoreboard");
}

static void test_drawn_round(void)
{
    q2_mp_session s;
    q2_mp_player_view v[Q2_MP_MAX_PLAYERS];

    memset(v, 0, sizeof(v));
    q2_mp_session_init(&s, Q2_MP_VERSUS, 2);

    q2_mp_versus_check(&s, v, 2);      /* nobody alive */
    CHECK(s.end == Q2_MP_END_ROUND_DRAWN, "nobody left is a drawn round");
    CHECK(strcmp(q2_mp_banner(&s), "ROUND DRAWN") == 0, "the banner says so");
    CHECK(run_banner(&s) == Q2_MP_REQ_RESTART_ROUND,
          "and a drawn round is replayed, not scored");
    CHECK(s.team_frags[0] == 0 && s.team_frags[1] == 0,
          "with nobody given a point");
}

static void test_two_alive_ends_nothing(void)
{
    q2_mp_session s;
    q2_mp_player_view v[Q2_MP_MAX_PLAYERS];

    memset(v, 0, sizeof(v));
    v[0].alive = true;
    v[2].alive = true;

    q2_mp_session_init(&s, Q2_MP_VERSUS, 3);
    q2_mp_versus_check(&s, v, 3);
    CHECK(s.end == Q2_MP_RUNNING, "two alive is a round still in progress");
}

static void test_respawn_gate(void)
{
    q2_mp_session s;

    q2_mp_session_init(&s, Q2_MP_DEATHMATCH, 2);
    CHECK(q2_mp_may_respawn(&s), "deathmatch respawns");

    q2_mp_session_init(&s, Q2_MP_TEAM_DEATHMATCH, 2);
    CHECK(q2_mp_may_respawn(&s), "team deathmatch respawns");

    q2_mp_session_init(&s, Q2_MP_VERSUS, 2);
    CHECK(!q2_mp_may_respawn(&s), "VERSUS does not: death is out for the round");
}

/* ------------------------------------------------------------------------- */
/* Who won                                                                    */
/* ------------------------------------------------------------------------- */

static void test_winner(void)
{
    q2_mp_session s;
    char buf[64];

    q2_mp_session_init(&s, Q2_MP_DEATHMATCH, 3);
    s.frags[0] = 2;
    s.frags[1] = 7;
    s.frags[2] = 5;
    CHECK(q2_mp_find_winner(&s) == 1, "the highest score wins");

    q2_mp_winner_text(&s, 1, NULL, buf, sizeof(buf));
    CHECK(strcmp(buf, "PLAYER 2 WINS") == 0, "the winner line, got '%s'", buf);

    /* A tie for the lead is a draw, whatever is below it. */
    s.frags[2] = 7;
    CHECK(q2_mp_find_winner(&s) == Q2_MP_WINNER_DRAW, "a tied lead is a draw");
    q2_mp_winner_text(&s, Q2_MP_WINNER_DRAW, NULL, buf, sizeof(buf));
    CHECK(strcmp(buf, "DRAWN MATCH") == 0, "the draw line, got '%s'", buf);

    /* A tie further down is not. */
    s.frags[0] = 5;
    s.frags[1] = 9;
    s.frags[2] = 5;
    CHECK(q2_mp_find_winner(&s) == 1, "a tie below the lead is not a draw");
}

static void test_team_winner(void)
{
    q2_mp_session s;
    char buf[64];
    int w;

    q2_mp_session_init(&s, Q2_MP_TEAM_DEATHMATCH, 2);
    s.team[0] = 0;
    s.team[1] = 1;
    s.frags[0] = 3;
    s.frags[1] = 8;

    w = q2_mp_find_winner(&s);
    CHECK(w == 5, "a team win is the team plus four, got %d", w);

    q2_mp_winner_text(&s, w, NULL, buf, sizeof(buf));
    CHECK(strcmp(buf, "RED TEAM WIN") == 0, "the team line, got '%s'", buf);
    CHECK(strcmp(q2_mp_team_name(0), "BLUE") == 0, "team 0 is BLUE");
    CHECK(strcmp(q2_mp_team_name(3), "GREEN") == 0, "team 3 is GREEN");
}

static void test_versus_winner_is_the_last_survivor(void)
{
    q2_mp_session s;

    /* VERSUS returns the recorded survivor before it sorts anything, so the
     * winner is whoever took the final round. */
    q2_mp_session_init(&s, Q2_MP_VERSUS, 4);
    s.last_alive = 2;
    s.team_frags[0] = 99;
    CHECK(q2_mp_find_winner(&s) == 2,
          "VERSUS reports the last survivor, not the round leader");
}

/* ------------------------------------------------------------------------- */
/* Spawning                                                                   */
/* ------------------------------------------------------------------------- */

static void set_spawn(q2_mp_spawn *s, s32 x, s32 y, s32 z)
{
    s->present = true;
    s->pos[0] = x;
    s->pos[1] = y;
    s->pos[2] = z;
}

static void test_spawn_selection(void)
{
    q2_mp_spawn sp[Q2_MP_MAX_SPAWNS];
    q2_mp_player_view v[Q2_MP_MAX_PLAYERS];
    int pick;

    memset(sp, 0, sizeof(sp));
    memset(v, 0, sizeof(v));

    set_spawn(&sp[0], 0, 0, 0);
    set_spawn(&sp[1], 0, 0, 4000);
    set_spawn(&sp[3], 0, 0, 40000);

    v[0].alive = true;
    v[0].pos[0] = 0;
    v[0].pos[1] = 0;
    v[0].pos[2] = 0;

    pick = q2_mp_select_spawn(sp, v, 1, NULL, NULL);
    CHECK(pick == 3, "the farthest spawn from the only player, got %d", pick);

    /* A second player next to the far spawn moves the answer to the middle. */
    v[1].alive = true;
    v[1].pos[0] = 0;
    v[1].pos[1] = 0;
    v[1].pos[2] = 40000;
    pick = q2_mp_select_spawn(sp, v, 2, NULL, NULL);
    CHECK(pick == 1, "the spawn farthest from BOTH players, got %d", pick);

    /* Dead players are not avoided. */
    v[1].alive = false;
    pick = q2_mp_select_spawn(sp, v, 2, NULL, NULL);
    CHECK(pick == 3, "a dead player does not push a spawn away, got %d", pick);

    /* No living player at all: every distance stays at the sentinel, so the
     * original falls through to its random draw. Without an RNG we take the
     * first present spawn, which is deterministic and never index -1. */
    v[0].alive = false;
    pick = q2_mp_select_spawn(sp, v, 2, NULL, NULL);
    CHECK(pick == 0, "the fallback takes the first present spawn, got %d", pick);

    /* A map with no MultiSpawn at all reports it rather than hanging. */
    memset(sp, 0, sizeof(sp));
    CHECK(q2_mp_select_spawn(sp, v, 2, NULL, NULL) == -1,
          "no MultiSpawn is -1, not a hang");
}

static void test_spawn_distance_is_divided_first(void)
{
    s32 a[3] = {0, 0, 0};
    s32 b[3] = {7, 0, 0};
    s32 c[3] = {8, 0, 0};
    s32 d[3] = {-7, 0, 0};

    /* Each axis is divided by eight toward zero BEFORE it is squared, so
     * anything closer than eight units reads as zero distance. */
    CHECK(q2_mp_spawn_dist2(a, b) == 0, "seven units rounds to nothing");
    CHECK(q2_mp_spawn_dist2(a, c) == 1, "eight units is one");
    CHECK(q2_mp_spawn_dist2(a, d) == 0, "and the same going the other way");
}

/* ------------------------------------------------------------------------- */
/* Modes                                                                      */
/* ------------------------------------------------------------------------- */

static bool batch_present(q2_mp_mode mode, const char *name)
{
    const char *b[Q2_MP_MAX_BATCHES];
    u32 n = q2_mp_batches(mode, b), i;

    for (i = 0; i < n; i++)
        if (strcmp(b[i], name) == 0)
            return true;
    return false;
}

static void test_batches(void)
{
    CHECK(batch_present(Q2_MP_DEATHMATCH, "Weapons"), "DM spawns weapons");
    CHECK(batch_present(Q2_MP_DEATHMATCH, "Health"), "DM spawns health");
    CHECK(batch_present(Q2_MP_DEATHMATCH, "Specials"), "DM spawns specials");
    CHECK(!batch_present(Q2_MP_DEATHMATCH, "RedFlag"), "DM has no flag");

    /* VERSUS keeps its guns and loses everything that would sustain you, which
     * is the rule its own description states. */
    CHECK(batch_present(Q2_MP_VERSUS, "Weapons"), "VERSUS still spawns weapons");
    CHECK(!batch_present(Q2_MP_VERSUS, "Health"), "VERSUS has no health");
    CHECK(!batch_present(Q2_MP_VERSUS, "Armour"), "VERSUS has no armour");
    CHECK(!batch_present(Q2_MP_VERSUS, "Ammo"), "VERSUS has no ammo");
    CHECK(batch_present(Q2_MP_VERSUS, "Specials"), "VERSUS still spawns specials");

    /* Only CTF has two flags. */
    CHECK(batch_present(Q2_MP_CTF, "RedFlag") &&
          batch_present(Q2_MP_CTF, "BlueFlag"), "CTF has both flags");
    CHECK(batch_present(Q2_MP_TAG, "RedFlag") &&
          !batch_present(Q2_MP_TAG, "BlueFlag"), "TAG has one flag");
    CHECK(batch_present(Q2_MP_TEAM_TAG, "RedFlag") &&
          !batch_present(Q2_MP_TEAM_TAG, "BlueFlag"), "TEAM TAG has one flag");
}

static void test_modes(void)
{
    CHECK(q2_mp_mode_selectable(Q2_MP_DEATHMATCH), "DM is selectable");
    CHECK(q2_mp_mode_selectable(Q2_MP_TEAM_DEATHMATCH), "TEAM DM is selectable");
    CHECK(q2_mp_mode_selectable(Q2_MP_VERSUS), "VERSUS is selectable");
    CHECK(!q2_mp_mode_selectable(Q2_MP_CTF), "CTF is cut");
    CHECK(!q2_mp_mode_selectable(Q2_MP_TAG), "TAG is cut");
    CHECK(!q2_mp_mode_selectable(Q2_MP_TEAM_TAG), "TEAM TAG is cut");

    /* QMRESULT's six titles are what fix the numbering, so hold them. */
    CHECK(strcmp(q2_mp_score_title(Q2_MP_DEATHMATCH), "DM SCORES") == 0, "0");
    CHECK(strcmp(q2_mp_score_title(Q2_MP_TEAM_DEATHMATCH),
                 "TEAM DM SCORES") == 0, "1");
    CHECK(strcmp(q2_mp_score_title(Q2_MP_CTF), "CTF SCORES") == 0, "2");
    CHECK(strcmp(q2_mp_score_title(Q2_MP_TAG), "TAG SCORES") == 0, "3");
    CHECK(strcmp(q2_mp_score_title(Q2_MP_TEAM_TAG), "TEAM TAG SCORES") == 0, "4");
    CHECK(strcmp(q2_mp_score_title(Q2_MP_VERSUS), "VERSUS SCORES") == 0, "5");
}

static void test_attribution(void)
{
    /* An ordinary kill keeps its attacker. */
    CHECK(q2_mp_attribute_kill(2, 0) == 2, "an attacker id passes through");

    /* Means of death 9 and 10 erase the attacker: the kill becomes the
     * victim's own, whoever fired. */
    CHECK(q2_mp_attribute_kill(2, 9) == -1, "means 9 is self-inflicted");
    CHECK(q2_mp_attribute_kill(2, 10) == -1, "means 10 is self-inflicted");
    CHECK(q2_mp_attribute_kill(2, 8) == 2, "means 8 is not");
    CHECK(q2_mp_attribute_kill(2, 11) == 2, "means 11 is not");

    /* 4 is the engine's "not a player" sentinel and never scores. */
    CHECK(q2_mp_attribute_kill(4, 0) == -1, "the sentinel is a world kill");
    CHECK(q2_mp_attribute_kill(-1, 0) == -1, "and so is -1");
}

/*
 * The byte the death handler reads is NOT the scoring fold above.
 *
 * 0x800396DC is the handler's only edit — -1 for mods 9 and 10 — and
 * 0x800396EC reads the byte back with `lb` for both gates. So a 4 stays a 4,
 * and 0x80039774's `slti s1, 4` refuses it: the hook is never called. Folding
 * it to -1 first, as `q2_mp_attribute_kill` does, sends the module a world
 * kill it then charges to the victim. The two readings are asserted side by
 * side, through the module, so the difference is a frag and not a number.
 */
static void test_the_handler_reads_the_raw_byte(void)
{
    q2_mp_session raw, folded;

    /* 0x800396CC: the override is the only edit, so the SAME byte 4 comes back
     * as -1 under mods 9 and 10 and as 4 under their neighbours. */
    CHECK(q2_mp_killer_field(Q2_MP_NOT_A_PLAYER, 9) == -1 &&
          q2_mp_killer_field(Q2_MP_NOT_A_PLAYER, 10) == -1 &&
          q2_mp_killer_field(Q2_MP_NOT_A_PLAYER, 8) == Q2_MP_NOT_A_PLAYER &&
          q2_mp_killer_field(Q2_MP_NOT_A_PLAYER, 11) == Q2_MP_NOT_A_PLAYER,
          "byte 4 should read -1 for mods 9 and 10 and 4 for 8 and 11");
    CHECK(q2_mp_killer_field(2, 9 + 256) == -1,
          "0x800396C4 reads the mod with lbu, so 265 is acid");

    /* 0x800396EC: everything else comes back as the byte, 4 and 23 included,
     * which is where this and the fold part company. */
    CHECK(q2_mp_killer_field(Q2_MP_NOT_A_PLAYER, 0) == Q2_MP_NOT_A_PLAYER &&
          q2_mp_attribute_kill(Q2_MP_NOT_A_PLAYER, 0) == -1,
          "the raw byte keeps a 4 that the scoring fold makes -1");
    CHECK(q2_mp_killer_field(23, 20) == 23,
          "single player's NULL-attacker byte comes back as 23");

    /*
     * Fed to the module: the raw 4 fails `killer >= Q2_MP_MAX_PLAYERS` inside
     * `q2_mp_player_killed` exactly as it fails 0x80039774, so nobody's score
     * moves; the folded -1 costs player 3 a frag for a death the console
     * never scores.
     */
    q2_mp_session_init(&raw, Q2_MP_DEATHMATCH, 4);
    q2_mp_session_init(&folded, Q2_MP_DEATHMATCH, 4);
    q2_mp_player_killed(&raw, q2_mp_killer_field(Q2_MP_NOT_A_PLAYER, 0), 3);
    q2_mp_player_killed(&folded, q2_mp_attribute_kill(Q2_MP_NOT_A_PLAYER, 0),
                        3);
    CHECK(raw.frags[3] == 0 && folded.frags[3] == -1,
          "a byte-4 death: raw scores %d, folded scores %d — want 0 and -1",
          raw.frags[3], folded.frags[3]);
}

static void test_hud_set(void)
{
    CHECK(strcmp(q2_mp_hud_image(false, 1), "qk_menu.lbm") == 0, "single player");
    CHECK(strcmp(q2_mp_hud_image(true, 2), "qk2_menu.lbm") == 0, "two players");
    CHECK(strcmp(q2_mp_hud_image(true, 3), "qkm_menu.lbm") == 0, "three players");
    CHECK(strcmp(q2_mp_hud_image(true, 4), "qkm_menu.lbm") == 0, "four players");
}

static void test_defaults(void)
{
    q2_mp_session s;

    q2_mp_session_init(&s, Q2_MP_DEATHMATCH, 2);
    CHECK(s.time_limit == 10, "TIME LIMIT   10, got %d", s.time_limit);
    CHECK(s.frag_limit == 10, "FRAG LIMIT   10, got %d", s.frag_limit);
    CHECK(s.round_limit == 3, "ROUND LIMIT   3, got %d", s.round_limit);
    CHECK(s.end == Q2_MP_RUNNING, "a fresh session is running");
    CHECK(s.frags[0] == 0 && s.team_frags[0] == 0 && s.kills[0][0] == 0,
          "and starts from a cleared scoreboard");
}

static void test_scoreboard(void)
{
    q2_mp_session s;
    char lines[12][Q2_MP_SCORE_LINE];
    u32 n;

    /*
     * Deathmatch: the title, then one line per player. And NOTHING ELSE —
     * `ALL PLAYERS PRESS` / `FIRE TO CONTINUE` used to come off the end of this
     * list and they are not scores. QMRESULT's module holds them as a static
     * two-row page at 256,180 and 256,200 (openquestions #101), so appending
     * them here made their position depend on the player count: four players
     * pushed the prompt two rows further down the screen than two did.
     *
     * The line count is the assertion that keeps them out, because a caller
     * that draws every line it is given would put them back by accident.
     */
    q2_mp_session_init(&s, Q2_MP_DEATHMATCH, 2);
    s.frags[0] = 7;
    s.frags[1] = 3;
    n = q2_mp_scoreboard(&s, NULL, lines, 12);
    CHECK(n == 3, "DM scoreboard is a title and two players, got %u", n);
    CHECK(strcmp(lines[0], "DM SCORES") == 0, "titled DM SCORES, got '%s'",
          lines[0]);
    CHECK(strstr(lines[1], "PLAYER 1") && strstr(lines[1], "7"),
          "player 1 scored 7, got '%s'", lines[1]);
    CHECK(strstr(lines[2], "PLAYER 2") && strstr(lines[2], "3"),
          "player 2 scored 3, got '%s'", lines[2]);

    /* Four players move the last SCORE row and nothing else; the prompt is not
     * in here to be moved. */
    q2_mp_session_init(&s, Q2_MP_DEATHMATCH, 4);
    n = q2_mp_scoreboard(&s, NULL, lines, 12);
    CHECK(n == 5, "four players give a title and four rows, got %u", n);
    CHECK(strstr(lines[4], "PLAYER 4") != NULL,
          "the last line is a player, not a prompt: '%s'", lines[4]);

    /* Team headings precede their players, ordered by team total. */
    q2_mp_session_init(&s, Q2_MP_TEAM_DEATHMATCH, 2);
    s.frags[0] = 4;
    s.team_frags[0] = 4;
    s.team_frags[1] = 9;
    n = q2_mp_scoreboard(&s, NULL, lines, 12);
    CHECK(strcmp(lines[0], "TEAM DM SCORES") == 0, "team title, got '%s'",
          lines[0]);
    CHECK(strcmp(lines[3], "BLUE TEAM SCORED 4") == 0,
          "blue team line, got '%s'", lines[3]);
    CHECK(strcmp(lines[1], "RED TEAM SCORED 9") == 0,
          "red team line, got '%s'", lines[1]);

    /* VERSUS scores ROUNDS, which live in the team array. */
    q2_mp_session_init(&s, Q2_MP_VERSUS, 2);
    s.frags[0] = 99;
    s.team_frags[0] = 2;
    n = q2_mp_scoreboard(&s, NULL, lines, 12);
    CHECK(strcmp(lines[0], "VERSUS SCORES") == 0, "versus title");
    CHECK(strstr(lines[1], " 2") && !strstr(lines[1], "99"),
          "versus shows rounds won, not frags, got '%s'", lines[1]);

    /* Names, when the caller has them. */
    {
        const char *const names[4] = { "ADA", "GRACE", NULL, NULL };

        q2_mp_session_init(&s, Q2_MP_DEATHMATCH, 2);
        q2_mp_scoreboard(&s, names, lines, 12);
        CHECK(strstr(lines[1], "ADA") != NULL, "a supplied name is used");
    }

    /* A caller with room for two lines gets two, not a smashed stack. */
    q2_mp_session_init(&s, Q2_MP_DEATHMATCH, 4);
    n = q2_mp_scoreboard(&s, NULL, lines, 2);
    CHECK(n == 2, "the line count is a limit, got %u", n);
}

static void test_retail_teams_and_results(void)
{
    q2_mp_session s;
    q2_mp_score_row rows[Q2_MP_SCORE_ROWS];
    u32 n;
    int i;
    q2_mp_session_init(&s, Q2_MP_TEAM_DEATHMATCH, 4);
    for (i = 0; i < 4; i++)
        CHECK(s.team[i] == (i & 1), "retail default team for player %d", i);
    q2_mp_player_killed(&s, 2, 0);
    CHECK(s.frags[2] == -1 && s.team_frags[0] == -1,
          "default blue teammates incur both penalties");
    q2_mp_player_killed(&s, 3, 0);
    CHECK(s.frags[3] == 1 && s.team_frags[1] == 1,
          "player four contributes to red's score");
    n = q2_mp_score_rows(&s, NULL, 1u << 3, rows, Q2_MP_SCORE_ROWS);
    CHECK(n == 6, "four players plus both team headings, got %u", n);
    CHECK(rows[0].player == -1 && rows[0].colour == 1 && rows[0].y == 76,
          "leading red team is the first heading at retail y");
    CHECK(rows[1].player == 3 && rows[2].player == 1 &&
          rows[4].player == 0 && rows[5].player == 2,
          "teams and their members sort by signed score");
    CHECK(strstr(rows[1].text, "READY") && !strstr(rows[2].text, "READY"),
          "READY follows player four after sorting");
    CHECK(rows[3].y == 136 && rows[5].y == 166,
          "15-pixel team gap and rows clear the fixed prompts");
    s.team_frags[0] = 0;
    CHECK(q2_mp_score_rows(&s, NULL, 0, rows, Q2_MP_SCORE_ROWS) == 6 &&
          strcmp(rows[3].text, "BLUE TEAM SCORED 0") == 0,
          "an active zero-scoring team stays on the board");

    q2_mp_session_init(&s, Q2_MP_DEATHMATCH, 4);
    s.frags[0] = -1; s.frags[1] = 5; s.frags[2] = 9; s.frags[3] = 5;
    n = q2_mp_score_rows(&s, NULL, 1u << 3, rows, Q2_MP_SCORE_ROWS);
    CHECK(n == 4 && rows[0].player == 2 && rows[1].player == 1 &&
          rows[2].player == 3 && rows[3].player == 0,
          "descending player scores, stable ties and negative scores");
    CHECK(rows[0].y == 104 && rows[3].y == 149 && rows[0].highlight,
          "four-player DM layout and coloured bands");
    CHECK(strlen(rows[2].text) == 31 && !strncmp(rows[2].text + 2, "READY", 5) &&
          !strncmp(rows[2].text + 12, "PLAYER 4", 8) && rows[2].text[26] == '5',
          "retail fixed name, score and readiness columns");
    s.mode = Q2_MP_VERSUS;
    s.team_frags[0] = 2;
    q2_mp_score_rows(&s, NULL, 0, rows, Q2_MP_SCORE_ROWS);
    CHECK(rows[0].player == 0, "Versus sorts rounds won rather than frags");
    CHECK(q2_mp_score_rows(&s, NULL, 0, rows, 1) == 1,
          "bounded row buffer");
}

int main(void)
{
    test_deathmatch_scoring();
    test_team_scoring();
    test_frag_limit();
    test_frag_limit_is_equality();
    test_no_frag_limit();
    test_time_limit();
    test_versus_has_no_clock();
    test_match_over_goes_to_the_scoreboard();
    test_round_over_goes_round_again();
    test_drawn_round();
    test_two_alive_ends_nothing();
    test_respawn_gate();
    test_winner();
    test_team_winner();
    test_versus_winner_is_the_last_survivor();
    test_spawn_selection();
    test_spawn_distance_is_divided_first();
    test_batches();
    test_modes();
    test_attribution();
    test_the_handler_reads_the_raw_byte();
    test_hud_set();
    test_defaults();
    test_scoreboard();
    test_retail_teams_and_results();

    if (g_fail) {
        printf("\n%d multiplayer check%s failed\n", g_fail,
               g_fail == 1 ? "" : "s");
        return 1;
    }
    printf("multiplayer: all checks passed\n");
    return 0;
}
