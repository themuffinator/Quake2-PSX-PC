/*
 * multiplayer.c — QMULTI.C, reconstructed.
 *
 * Every rule here is read out of the relocated LevelBin module the thirteen
 * arenas share; the module offsets in the comments are into that image, and the
 * 0x800B…. addresses are the engine block it is handed. See multiplayer.h for
 * how the two fit together.
 */
#include "multiplayer.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Modes                                                                      */
/* ------------------------------------------------------------------------- */

/* QFRONT +0x1DC / +0x1E8 / +0x1F8, and +0x1184 / +0x119C / +0x11B0 / +0x11B4
 * for the three the selector never reaches. */
static const char *const k_mode_name[Q2_MP_MODE_COUNT] = {
    "DEATHMATCH",
    "TEAM DEATHMATCH",
    "CAPTURE THE FLAG",
    "TAG",
    "TEAM TAG",
    "VERSUS"
};

/* QMRESULT +0x1FC onward, in this order — which is what fixes the numbering. */
static const char *const k_score_title[Q2_MP_MODE_COUNT] = {
    "DM SCORES",
    "TEAM DM SCORES",
    "CTF SCORES",
    "TAG SCORES",
    "TEAM TAG SCORES",
    "VERSUS SCORES"
};

bool q2_mp_mode_selectable(q2_mp_mode mode)
{
    /* 0x8010459C writes 0, 1 and 5 and nothing else. */
    return mode == Q2_MP_DEATHMATCH ||
           mode == Q2_MP_TEAM_DEATHMATCH ||
           mode == Q2_MP_VERSUS;
}

const char *q2_mp_mode_name(q2_mp_mode mode)
{
    if ((u32)mode >= Q2_MP_MODE_COUNT)
        return "";
    return k_mode_name[mode];
}

const char *q2_mp_score_title(q2_mp_mode mode)
{
    if ((u32)mode >= Q2_MP_MODE_COUNT)
        return "";
    return k_score_title[mode];
}

/* ------------------------------------------------------------------------- */
/* Limits                                                                     */
/* ------------------------------------------------------------------------- */

/* QFRONT LevelBin +0xEB88, +0xEBAC, +0xEBA0. The trailing -1 on two of them is
 * the "NONE" the front end prints as an infinity glyph ("FRAG LIMIT  i"). */
const s16 q2_mp_time_options[Q2_MP_TIME_OPTION_COUNT] = {
    1, 2, 3, 4, 5, 10, 15, 20, 25, 30, 60, Q2_MP_NO_LIMIT
};
const s16 q2_mp_frag_options[Q2_MP_FRAG_OPTION_COUNT] = {
    1, 2, 3, 4, 5, 10, 15, 20, Q2_MP_NO_LIMIT
};
const s16 q2_mp_round_options[Q2_MP_ROUND_OPTION_COUNT] = {
    1, 2, 3, 5, 10
};

/* ------------------------------------------------------------------------- */
/* Population batches                                                         */
/* ------------------------------------------------------------------------- */

u32 q2_mp_batches(q2_mp_mode mode, const char *out[Q2_MP_MAX_BATCHES])
{
    u32 n = 0;

    if (!out)
        return 0;

    /* 0x80100140. "Weapons" is spawned before the VERSUS test, so the mode that
     * has no health, armour or ammo still has guns. */
    out[n++] = "Weapons";
    if (mode != Q2_MP_VERSUS) {
        out[n++] = "Health";
        out[n++] = "Armour";
        out[n++] = "Ammo";
    }
    out[n++] = "Specials";

    /* (unsigned)(mode - 2) < 3, i.e. CTF, TAG or TEAM TAG. */
    if (mode >= Q2_MP_CTF && mode <= Q2_MP_TEAM_TAG)
        out[n++] = "RedFlag";
    if (mode == Q2_MP_CTF)
        out[n++] = "BlueFlag";

    return n;
}

/* ------------------------------------------------------------------------- */
/* Spawn selection — 0x80071004                                               */
/* ------------------------------------------------------------------------- */

/* The original's `bgez; addiu +7; sra 3`: a divide by eight that rounds toward
 * zero rather than toward negative infinity. */
static s32 div8_toward_zero(s32 v)
{
    return (v >= 0 ? v : v + 7) >> 3;
}

s32 q2_mp_spawn_dist2(const s32 a[3], const s32 b[3])
{
    s32 d0 = div8_toward_zero(a[0] - b[0]);
    s32 d1 = div8_toward_zero(a[1] - b[1]);
    s32 d2 = div8_toward_zero(a[2] - b[2]);

    return d0 * d0 + d1 * d1 + d2 * d2;
}

int q2_mp_select_spawn(const q2_mp_spawn spawns[Q2_MP_MAX_SPAWNS],
                       const q2_mp_player_view *players, u32 player_count,
                       q2_mp_rng_fn rng, void *rng_user)
{
    s32 nearest[Q2_MP_MAX_SPAWNS];
    s32 best = -1;
    int chosen = -1;
    u32 i, p;

    if (!spawns)
        return -1;

    /* Pass one: the closest living player to each spawn, or -1 if the spawn is
     * absent or nobody living was found. */
    for (i = 0; i < Q2_MP_MAX_SPAWNS; i++) {
        nearest[i] = -1;
        if (!spawns[i].present)
            continue;

        for (p = 0; p < player_count && p < Q2_MP_MAX_PLAYERS; p++) {
            s32 d;

            if (!players || !players[p].alive)
                continue;

            d = q2_mp_spawn_dist2(spawns[i].pos, players[p].pos);
            if (nearest[i] < 0 || d < nearest[i])
                nearest[i] = d;
        }
    }

    /* Pass two: the spawn whose closest player is farthest away. Strictly
     * greater, so the lowest index wins a tie — the original's `slt`. */
    for (i = 0; i < Q2_MP_MAX_SPAWNS; i++) {
        if (best < nearest[i]) {
            best = nearest[i];
            chosen = (int)i;
        }
    }

    if (chosen >= 0)
        return chosen;

    /*
     * Nothing qualified. The original draws `rand() & 7` and retries forever
     * until the name resolves, which on a map with no MultiSpawn at all is a
     * hang. We keep the draw and bound the retries, because a library that hangs
     * is not a faithful reproduction of anything a player could observe.
     */
    for (i = 0; i < Q2_MP_MAX_SPAWNS; i++)
        if (spawns[i].present)
            break;
    if (i == Q2_MP_MAX_SPAWNS)
        return -1;

    if (rng) {
        u32 tries;
        for (tries = 0; tries < 256; tries++) {
            u32 pick = rng(rng_user) & 7u;
            if (spawns[pick].present)
                return (int)pick;
        }
    }

    return (int)i;
}

/* ------------------------------------------------------------------------- */
/* Session                                                                    */
/* ------------------------------------------------------------------------- */

void q2_mp_session_init(q2_mp_session *s, q2_mp_mode mode, int players)
{
    int i;

    if (!s)
        return;

    memset(s, 0, sizeof(*s));

    if (players < 0)
        players = 0;
    if (players > Q2_MP_MAX_PLAYERS)
        players = Q2_MP_MAX_PLAYERS;

    s->mode         = (s16)mode;
    s->player_count = (s16)players;

    /* 0x801021A0: VERSUS has no clock, and spends the round limit rather than
     * the frag limit. The other modes take both from the option tables. */
    s->time_limit  = (mode == Q2_MP_VERSUS)
                       ? Q2_MP_NO_LIMIT
                       : q2_mp_time_options[Q2_MP_TIME_OPTION_DEFAULT];
    s->frag_limit  = q2_mp_frag_options[Q2_MP_FRAG_OPTION_DEFAULT];
    s->round_limit = q2_mp_round_options[Q2_MP_ROUND_OPTION_DEFAULT];

    /* FrontEndResetMultiplayerDefaults, 0x8001BED4 / 0x8001BF04. The
     * individual colour is p; the separate team halfword is p & 1. */
    for (i = 0; i < Q2_MP_MAX_PLAYERS; i++)
        s->team[i] = (s16)(i & 1);

    s->end          = Q2_MP_RUNNING;
    s->last_alive   = 0;
    s->banner_armed = false;
    s->banner_ticks = Q2_MP_BANNER_TICKS;
    s->request      = Q2_MP_REQ_NONE;
}

void q2_mp_round_start(q2_mp_session *s)
{
    if (!s) return;
    s->end = Q2_MP_RUNNING;
    s->last_alive = 0;
    s->banner_armed = false;
    s->banner_ticks = Q2_MP_BANNER_TICKS;
    s->request = Q2_MP_REQ_NONE;
}

void q2_mp_results_init(q2_mp_results *results)
{
    if (!results) return;
    results->ready = 0;
    results->countdown = 150;
}

bool q2_mp_results_tick(q2_mp_results *results, int players,
                         u32 fire_pressed, s32 dt)
{
    u32 all;
    if (!results || players < 1 || players > Q2_MP_MAX_PLAYERS || dt < 0)
        return false;
    all = (1u << players) - 1;
    results->ready = (results->ready | fire_pressed) & all;
    if (results->ready == all) {
        results->countdown -= dt;
        return results->countdown < 0;
    }
    results->countdown = 150;
    return false;
}

void q2_mp_player_killed(q2_mp_session *s, int killer, int victim)
{
    s16 score;

    if (!s)
        return;

    /* The hook returns immediately once the match is over (+0x0BF4). */
    if (s->end != Q2_MP_RUNNING)
        return;

    if (victim < 0 || victim >= Q2_MP_MAX_PLAYERS)
        return;

    /* A world kill is charged to the victim. */
    if (killer < 0)
        killer = victim;
    if (killer >= Q2_MP_MAX_PLAYERS)
        return;

    /* `1220 + victim*8 + victim*2`. The diagonal really is what it writes. */
    s->kills[victim][victim]++;

    if (s->mode == Q2_MP_TEAM_DEATHMATCH) {
        s16 kt = s->team[killer];
        s16 vt = s->team[victim];

        if (kt < 0 || kt >= Q2_MP_MAX_TEAMS)
            kt = 0;

        if (kt == vt) {
            s->frags[killer]--;
            s->team_frags[kt]--;
        } else {
            s->frags[killer]++;
            s->team_frags[kt]++;
        }
        score = s->team_frags[kt];
    } else {
        if (killer == victim)
            s->frags[victim]--;
        else
            s->frags[killer]++;
        score = s->frags[killer];
    }

    /* Only modes 0 and 1 have a frag limit: `(unsigned)mode < 2`. */
    if ((u16)s->mode < 2) {
        if (s->frag_limit == Q2_MP_NO_LIMIT)
            return;
        if (score != s->frag_limit)
            return;
        s->end = Q2_MP_END_FRAG_LIMIT;
        return;
    }

    /* Modes 2..4 score no further here; VERSUS is q2_mp_versus_check. */
}

void q2_mp_versus_check(q2_mp_session *s, const q2_mp_player_view *players,
                        u32 player_count)
{
    u32 i;
    int alive = 0;

    if (!s || s->mode != Q2_MP_VERSUS || s->end != Q2_MP_RUNNING)
        return;

    for (i = 0; i < player_count && i < Q2_MP_MAX_PLAYERS; i++) {
        if (!players || !players[i].alive)
            continue;
        s->last_alive = (s16)i;
        alive++;
    }

    if (alive == 1) {
        s16 won;

        /* Round wins live in the team array; in VERSUS it is indexed by player.
         * That is the original's own reuse, not a transcription slip. */
        s->team_frags[s->last_alive]++;
        won = s->team_frags[s->last_alive];

        s->end = (won != s->round_limit) ? Q2_MP_END_ROUND_OVER
                                         : Q2_MP_END_MATCH_OVER;
    } else if (alive == 0) {
        s->end = Q2_MP_END_ROUND_DRAWN;
    }
}

q2_mp_request q2_mp_frame(q2_mp_session *s, s32 level_time, s32 dt)
{
    if (!s)
        return Q2_MP_REQ_NONE;

    /*
     * The clock. 18000 dt units is a minute; the original builds the constant
     * out of shifts (`x*5`, `<<4`, `-x`, `<<4`, `-x`, `<<4`) and compares
     * unsigned, so a negative clock would read as enormous and end the match
     * instantly. We keep the unsigned compare.
     */
    if (s->end == Q2_MP_RUNNING && s->time_limit != Q2_MP_NO_LIMIT) {
        u32 limit = (u32)((s32)s->time_limit * 18000);
        if (limit < (u32)level_time)
            s->end = Q2_MP_END_TIME_UP;
    }

    if (s->end == Q2_MP_RUNNING)
        return Q2_MP_REQ_NONE;

    /* The banner counts down every frame the match is over, armed or not. */
    s->banner_ticks -= dt;

    if (!s->banner_armed)
        s->banner_armed = true;

    if (s->banner_ticks >= 0)
        return Q2_MP_REQ_NONE;

    /*
     * 0x80101130: a round that ended goes round again, anything else goes to
     * the scoreboard. Written every frame past the expiry, as the original
     * writes its state word every frame past the expiry.
     */
    s->request = (s->end == Q2_MP_END_ROUND_OVER ||
                  s->end == Q2_MP_END_ROUND_DRAWN)
                   ? Q2_MP_REQ_RESTART_ROUND
                   : Q2_MP_REQ_RESULTS;

    return s->request;
}

q2_mp_request q2_mp_take_request(q2_mp_session *s)
{
    q2_mp_request r;

    if (!s)
        return Q2_MP_REQ_NONE;

    r = s->request;
    s->request = Q2_MP_REQ_NONE;
    return r;
}

const char *q2_mp_banner(const q2_mp_session *s)
{
    if (!s)
        return NULL;

    /* Module +0x1518 / +0x1548 / +0x1578 / +0x15A8, each a one-item menu table
     * centred at (256, 124). */
    switch (s->end) {
    case Q2_MP_END_TIME_UP:     return "TIME UP";
    case Q2_MP_END_ROUND_OVER:  return "ROUND OVER";
    case Q2_MP_END_ROUND_DRAWN: return "ROUND DRAWN";
    case Q2_MP_END_FRAG_LIMIT:
    case Q2_MP_END_MATCH_OVER:  return "GAME OVER";
    default:                    return NULL;
    }
}

bool q2_mp_may_respawn(const q2_mp_session *s)
{
    /* `if (mode != 5) place_player(...)` — 0x8003DEB4. */
    return s && s->mode != Q2_MP_VERSUS;
}

/* ------------------------------------------------------------------------- */
/* Attribution                                                                */
/* ------------------------------------------------------------------------- */

s8 q2_mp_killer_field(int killer_field, int means_of_death)
{
    /* 0x800396C4 reads the mod with `lbu`, so the test is on its low byte;
     * 0x800396CC/0x800396D0 are `(unsigned)(mod - 9) < 2`, and the `sb` at
     * 0x800396DC is the only store of a literal -1 to entity+222 anywhere in
     * the image. */
    if ((u32)(((u32)means_of_death & 0xFFu) - Q2_MP_MOD_SELF_FIRST) <
        (u32)(Q2_MP_MOD_SELF_LAST - Q2_MP_MOD_SELF_FIRST + 1))
        return (s8)-1;

    /* 0x800396EC `lb s1, 222(s0)`: whatever the byte holds, sign-extended.
     * Nothing between the store and this load narrows it. */
    return (s8)killer_field;
}

int q2_mp_attribute_kill(int killer_field, int means_of_death)
{
    /* `(unsigned)(mod - 9) < 2`, at 0x800396CC. */
    if ((u32)(means_of_death - Q2_MP_MOD_SELF_FIRST) <
        (u32)(Q2_MP_MOD_SELF_LAST - Q2_MP_MOD_SELF_FIRST + 1))
        return -1;

    /* The SCORING fold, not the handler's gate (multiplayer.h). The field is
     * read with `lb`, so 0xFF is already -1 by the time the death handler sees
     * it; anything at or past the player count — the "not a player" 4 among
     * them — is folded to -1 here as well, which q2_mp_player_killed charges
     * to the victim as a suicide. The console does not: its hook bound,
     * 0x80039774 `slti s1, 4`, rejects such a byte and calls nothing. That is
     * q2_mp_killer_field's reading, and the death chain uses it. */
    if (killer_field < 0 || killer_field >= Q2_MP_MAX_PLAYERS)
        return -1;

    return killer_field;
}

/* ------------------------------------------------------------------------- */
/* Presentation                                                               */
/* ------------------------------------------------------------------------- */

const char *q2_mp_hud_image(bool multiplayer, int players)
{
    if (!multiplayer)
        return "qk_menu.lbm";
    /* `slti v0, players, 3` at 0x8003FEC8. */
    return (players < 3) ? "qk2_menu.lbm" : "qkm_menu.lbm";
}

/* ------------------------------------------------------------------------- */
/* Who won — 0x80100660                                                       */
/* ------------------------------------------------------------------------- */

int q2_mp_find_winner(const q2_mp_session *s)
{
    s16 score[Q2_MP_MAX_PLAYERS];
    s16 id[Q2_MP_MAX_PLAYERS];
    u32 n = 0, i, count;
    bool swapped;

    if (!s)
        return Q2_MP_WINNER_DRAW;

    /* VERSUS reports the survivor the round check recorded, before any sort. */
    if (s->mode == Q2_MP_VERSUS)
        return s->last_alive;

    memset(score, 0, sizeof(score));
    memset(id, 0, sizeof(id));

    count = (u32)s->player_count;
    if (count > Q2_MP_MAX_PLAYERS)
        count = Q2_MP_MAX_PLAYERS;

    if (s->mode == Q2_MP_TEAM_DEATHMATCH) {
        int used[Q2_MP_MAX_TEAMS];
        s16 total[Q2_MP_MAX_PLAYERS];

        memset(used, 0, sizeof(used));
        memset(total, 0, sizeof(total));

        for (i = 0; i < count; i++) {
            s16 t = s->team[i];
            if (t >= 0 && t < Q2_MP_MAX_TEAMS)
                used[t] = 1;
            /* `sum[i] += frags[i]` — indexed by PLAYER, paired below with the
             * mark for the TEAM of the same index. This retail banner bug
             * remains observable with the alternating default teams. The
             * results page independently uses the real team totals. */
            total[i] = (s16)(total[i] + s->frags[i]);
        }

        for (i = 0; i < Q2_MP_MAX_TEAMS; i++) {
            if (!used[i])
                continue;
            score[n] = total[i];
            id[n]    = (s16)i;
            n++;
        }
    } else {
        for (i = 0; i < count; i++) {
            score[n] = s->frags[i];
            id[n]    = (s16)i;
            n++;
        }
    }

    /* The original's bubble sort: descending, ids carried along, repeat until a
     * pass makes no swap. */
    do {
        swapped = false;
        for (i = 0; n > 0 && i + 1 < n; i++) {
            if (score[i] < score[i + 1]) {
                s16 ts = score[i];
                s16 ti = id[i];
                score[i]     = score[i + 1];
                score[i + 1] = ts;
                id[i]        = id[i + 1];
                id[i + 1]    = ti;
                swapped = true;
            }
        }
    } while (swapped);

    /* A tie for the lead is a draw, whatever is below it. */
    if (score[0] == score[1])
        return Q2_MP_WINNER_DRAW;

    return (s->mode == Q2_MP_TEAM_DEATHMATCH) ? id[0] + 4 : id[0];
}

static const char *const k_team_name[Q2_MP_MAX_TEAMS] = {
    "BLUE", "RED", "PURPLE", "GREEN"
};

static void score_order(const s16 *scores, int *ids, int count)
{
    int i, j;
    for (i = 1; i < count; i++) {
        int id = ids[i];
        for (j = i; j > 0 && scores[ids[j - 1]] < scores[id]; j--)
            ids[j] = ids[j - 1];
        ids[j] = id;
    }
}

u32 q2_mp_score_rows(const q2_mp_session *s, const char *const *names,
                      u32 ready, q2_mp_score_row *rows, u32 max)
{
    static const char *const k_default_name[Q2_MP_MAX_PLAYERS] = {
        "PLAYER 1", "PLAYER 2", "PLAYER 3", "PLAYER 4"
    };
    u32 n = 0;
    int i, t, players, teams = 0, y;
    int order[Q2_MP_MAX_PLAYERS], team_order[Q2_MP_MAX_TEAMS];
    bool used[Q2_MP_MAX_TEAMS] = {false};
    const s16 *scores;
    bool team_mode;

    if (!s || !rows || max == 0)
        return 0;

    players = s->player_count;
    if (players < 1)
        return 0;
    if (players > Q2_MP_MAX_PLAYERS)
        players = Q2_MP_MAX_PLAYERS;

    /* QMRESULT +0x0F3C / +0x1064 only group mode 1. */
    team_mode = s->mode == Q2_MP_TEAM_DEATHMATCH;
    scores = s->mode == Q2_MP_VERSUS ? s->team_frags : s->frags;
    for (i = 0; i < players; i++) {
        order[i] = i;
        if (s->team[i] >= 0 && s->team[i] < Q2_MP_MAX_TEAMS)
            used[s->team[i]] = true;
    }
    score_order(scores, order, players);
    for (t = 0; t < Q2_MP_MAX_TEAMS; t++)
        if (used[t]) team_order[teams++] = t;
    score_order(s->team_frags, team_order, teams);
    y = (252 - 11 * (players + (team_mode ? 1 + 2 * teams : 0))) / 2;

    for (t = 0; t < (team_mode ? teams : 1); t++) {
        int team = team_mode ? team_order[t] : -1;
        if (team_mode && n < max) {
            q2_mp_score_row *row = &rows[n++];
            memset(row, 0, sizeof(*row));
            snprintf(row->text, sizeof(row->text), "%s TEAM SCORED %d",
                     q2_mp_team_name(team), s->team_frags[team]);
            row->y = (s16)y;
            row->player = -1;
            row->colour = (s8)team;
            row->highlight = true;
            y += 15;
        }
        for (i = 0; i < players && n < max; i++) {
            int p = order[i];
            const char *who = names && names[p] && names[p][0]
                                  ? names[p] : k_default_name[p];
            q2_mp_score_row *row;
            if (team_mode && s->team[p] != team) continue;
            row = &rows[n++];
            memset(row, 0, sizeof(*row));
            /* QMRESULT +0x1214 / +0x141C: 31 columns, with READY at 2,
             * the name at 12, and the score at 26. Bound host-supplied names. */
            snprintf(row->text, sizeof(row->text), "  %-5s     %-12.12s  %-5d",
                     ready & (1u << p) ? "READY" : "", who, scores[p]);
            row->y = (s16)y;
            row->player = (s8)p;
            row->colour = (s8)(team_mode ? team : p);
            row->highlight = !team_mode;
            y += 15;
        }
        y += 15;
    }
    return n;
}

u32 q2_mp_scoreboard(const q2_mp_session *s, const char *const *names,
                     char lines[][Q2_MP_SCORE_LINE], u32 max)
{
    q2_mp_score_row rows[Q2_MP_SCORE_ROWS];
    u32 i, count, n = 0;
    if (!s || !lines || !max) return 0;
    snprintf(lines[n++], Q2_MP_SCORE_LINE, "%s", q2_mp_score_title(s->mode));
    count = q2_mp_score_rows(s, names, 0, rows, Q2_MP_SCORE_ROWS);
    for (i = 0; i < count && n < max; i++)
        snprintf(lines[n++], Q2_MP_SCORE_LINE, "%s", rows[i].text);
    return n;
}

const char *q2_mp_team_name(int team)
{
    if (team < 0 || team >= Q2_MP_MAX_TEAMS)
        return "";
    return k_team_name[team];
}

const char *q2_mp_winner_text(const q2_mp_session *s, int winner,
                              const char *const *names,
                              char *out, u32 out_size)
{
    static const char *const k_default_name[Q2_MP_MAX_PLAYERS] = {
        "PLAYER 1", "PLAYER 2", "PLAYER 3", "PLAYER 4"
    };

    (void)s;

    if (!out || out_size == 0)
        return out;

    out[0] = '\0';

    if (winner >= 0 && winner < Q2_MP_MAX_PLAYERS) {
        const char *who = NULL;
        if (names && names[winner])
            who = names[winner];
        if (!who || !who[0])
            who = k_default_name[winner];
        snprintf(out, out_size, "%s WINS", who);
    } else if (winner >= 4 && winner < 8) {
        snprintf(out, out_size, "%s TEAM WIN", k_team_name[winner - 4]);
    } else if (winner == Q2_MP_WINNER_DRAW) {
        snprintf(out, out_size, "DRAWN MATCH");
    }

    return out;
}
