#include "cmd_pmove.h"

#include <stdio.h>
#include <string.h>

#include "entity.h"      /* q2_start_pos_*                                   */
#include "level.h"       /* q2_common_open                                   */
#include "pad.h"
#include "combat.h"
#include "sim.h"
#include "trigger.h"
#include "userfuncs.h"
#include "weapon.h"
#include "world.h"
#include "worldscale.h"

/* ------------------------------------------------------------------------- */
/* The nine control styles                                                    */
/* ------------------------------------------------------------------------- */
/*
 * Drive q2_pad_read with one pad button at a time and report what came out. The
 * point is not that the table is transcribed — it is that the table is what the
 * jump table at 0x800AB53C actually dispatches to, exercised rather than read.
 */
static const struct { const char *name; u32 mask; } k_buttons[] = {
    {"UP",       Q2_PAD_UP},       {"DOWN",   Q2_PAD_DOWN},
    {"LEFT",     Q2_PAD_LEFT},     {"RIGHT",  Q2_PAD_RIGHT},
    {"L1",       Q2_PAD_L1},       {"R1",     Q2_PAD_R1},
    {"L2",       Q2_PAD_L2},       {"R2",     Q2_PAD_R2},
    {"TRIANGLE", Q2_PAD_TRIANGLE}, {"CIRCLE", Q2_PAD_CIRCLE},
    {"CROSS",    Q2_PAD_CROSS},    {"SQUARE", Q2_PAD_SQUARE},
    {"MOUSE L",  Q2_PAD_MOUSE_L},  {"MOUSE R",Q2_PAD_MOUSE_R}
};

/* What one button does under one style, as a short label. */
static void button_role(int style, u32 mask, char *out, size_t n)
{
    q2_pad_state pad;
    q2_pad_config cfg;
    q2_input a, b;

    out[0] = '\0';

    q2_pad_config_default(&cfg);
    cfg.style = style;

    /* Frame one: pressed from nothing, which is what shows the press edges. */
    memset(&pad, 0, sizeof(pad));
    pad.buttons = mask;
    q2_pad_read(&pad, &cfg, &a);

    /* Frame two: still held, which is what shows the held bits. */
    pad.prev = mask;
    q2_pad_read(&pad, &cfg, &b);

    if (a.forward > 0) snprintf(out, n, "forward");
    else if (a.forward < 0) snprintf(out, n, "back");
    else if (a.side > 0) snprintf(out, n, "strafe R");
    else if (a.side < 0) snprintf(out, n, "strafe L");
    else if (a.yaw > 0) snprintf(out, n, "turn R");
    else if (a.yaw < 0) snprintf(out, n, "turn L");
    else if (a.pitch > 0) snprintf(out, n, "look up");
    else if (a.pitch < 0) snprintf(out, n, "look dn");
    else if (a.buttons & Q2_BTN_JUMP) snprintf(out, n, "jump/swim");
    else if (b.buttons & Q2_BTN_ATTACK) snprintf(out, n, "fire");
    else if (a.buttons & Q2_BTN_WEAP_NEXT) snprintf(out, n, "weap +");
    else if (a.buttons & Q2_BTN_WEAP_PREV) snprintf(out, n, "weap -");
    else snprintf(out, n, "-");
}

static void report_styles(void)
{
    int style, i;

    printf("Control styles -- 0x80019154, jump table 0x800AB53C\n\n");
    printf("  %-14s", "");
    for (i = 0; i < 6; i++)
        printf(" %-9s", k_buttons[i].name);
    printf("\n");

    for (style = 0; style < Q2_PAD_STYLE_COUNT; style++) {
        char role[32];
        printf("  %-14s", q2_pad_style_name(style));
        for (i = 0; i < 6; i++) {
            button_role(style, k_buttons[i].mask, role, sizeof(role));
            printf(" %-9s", role);
        }
        printf("\n");
    }

    printf("\n  %-14s", "");
    for (i = 6; i < 14; i++)
        printf(" %-9s", k_buttons[i].name);
    printf("\n");

    for (style = 0; style < Q2_PAD_STYLE_COUNT; style++) {
        char role[32];
        printf("  %-14s", q2_pad_style_name(style));
        for (i = 6; i < 14; i++) {
            button_role(style, k_buttons[i].mask, role, sizeof(role));
            printf(" %-9s", role);
        }
        printf("\n");
    }

    /*
     * The two claims that are easy to state and easy to get wrong, checked
     * rather than described.
     */
    {
        q2_pad_state pad;
        q2_pad_config cfg;
        q2_input in;
        bool jump_is_swim = true, full_is_127 = true;
        int s;

        for (s = 0; s < Q2_PAD_STYLE_COUNT; s++) {
            q2_pad_config_default(&cfg);
            cfg.style = s;

            /* Held for two frames: the press edge is gone, the held bit is
             * not — one button, both meanings. */
            memset(&pad, 0, sizeof(pad));
            pad.buttons = pad.prev = 0xFFFFu;
            q2_pad_read(&pad, &cfg, &in);
            if (!(in.buttons & Q2_BTN_SWIM_UP) || (in.buttons & Q2_BTN_JUMP))
                jump_is_swim = false;

            memset(&pad, 0, sizeof(pad));
            pad.buttons = 0xFFFFu;
            pad.lx = pad.ly = pad.rx = pad.ry = 127;
            q2_pad_read(&pad, &cfg, &in);
            if (in.forward != 0 &&
                in.forward != Q2_PAD_FULL && in.forward != -Q2_PAD_FULL)
                full_is_127 = false;
        }

        printf("\n  jump held becomes swim-up on every style : %s\n",
               jump_is_swim ? "yes" : "NO");
        printf("  digital full deflection is %3d           : %s\n",
               Q2_PAD_FULL, full_is_127 ? "yes" : "NO");
        printf("  styles that SET the look rate            : 0..%d\n",
               Q2_PAD_STYLE_EASED_FROM - 1);
        printf("  styles that EASE it                      : %d..%d\n",
               Q2_PAD_STYLE_EASED_FROM, Q2_PAD_STYLE_COUNT - 1);
    }
}

/* ------------------------------------------------------------------------- */
/* The jump                                                                   */
/* ------------------------------------------------------------------------- */
/*
 * On a flat floor with no hull, so the arc is the integrator's alone. The
 * numbers to look at are the apex and the airtime: both fall out of one impulse
 * of -3072 clamped to -3072 against gravity 32 per dt, and neither is a constant
 * anywhere in the executable.
 */
static void report_jump(void)
{
    q2_sim sim;
    q2_input in;
    s32 spawn[3] = { 0, 0, 0 };
    s32 apex = 0;
    int i, apex_tick = 0, land_tick = -1, leave_tick = -1;

    printf("\n\nThe jump -- 0x8003E110 impulse, 0x80045E74 integrator\n\n");

    memset(&in, 0, sizeof(in));
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = 0;

    /* Settle on the floor first: the jump reads the ground the previous move
     * left behind, so a jump on tick zero is a jump in mid-air. */
    for (i = 0; i < 4; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);

    in.buttons = Q2_BTN_JUMP;

    printf("  tick   height   vel.y   on ground\n");
    for (i = 0; i < 40; i++) {
        s32 height;

        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
        in.buttons = 0;   /* a tap, not a hold */

        height = -(sim.player[0].pos[1] - spawn[1]);   /* +Y is down */

        if (height > apex) { apex = height; apex_tick = i; }
        if (leave_tick < 0 && !sim.player[0].on_ground) leave_tick = i;
        if (leave_tick >= 0 && land_tick < 0 && sim.player[0].on_ground)
            land_tick = i;

        if (i < 20 || (land_tick >= 0 && i <= land_tick))
            printf("  %4d   %6d   %5d   %s\n", i, height,
                   (s32)(s16)sim.player[0].vel[1],
                   sim.player[0].on_ground ? "yes" : "");
        if (land_tick >= 0 && i > land_tick)
            break;
    }

    printf("\n  apex        : %d world units on tick %d\n", apex, apex_tick);
    printf("  airtime     : %d ticks", land_tick >= 0 ? land_tick - leave_tick + 1 : -1);
    if (land_tick >= 0)
        printf("  (%.2f s at %d dt/tick)",
               (double)((land_tick - leave_tick + 1) * Q2_DT_NOMINAL) / Q2_DT_HZ,
               Q2_DT_NOMINAL);
    printf("\n");
    printf("  apex in PC units (S=%d): %.1f\n",
           Q2_WORLD_SCALE, (double)apex / Q2_WORLD_SCALE);

    q2_sim_free(&sim);
}

/* ------------------------------------------------------------------------- */
/* The view height                                                            */
/* ------------------------------------------------------------------------- */
/*
 * Three targets and one ease, and the ease is the same clamped approach that is
 * the whole of the movement model. Driven here through the environment flags a
 * volume would set, which is exactly how the game drives it.
 */
static void report_view_height(void)
{
    q2_sim sim;
    q2_input in;
    s32 spawn[3] = { 0, 0, 0 };
    int i, settle_low = -1, settle_stand = -1;

    printf("\n\nView height -- 0x8003A208, targets at 0x8003A214..0x8003A228\n\n");
    printf("  standing %4d   crouch %4d   low crouch %4d   ease %d*dt\n",
           Q2_VIEW_STAND, Q2_VIEW_MID, Q2_VIEW_CROUCH, Q2_VIEW_EASE_RATE);

    memset(&in, 0, sizeof(in));
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = 0;

    /* Into a low-crouch volume. */
    sim.env_flags = Q2_ENT_INLOWCROUCH;
    for (i = 0; i < 60; i++) {
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
        if (settle_low < 0 && sim.player[0].view_height == Q2_VIEW_CROUCH)
            settle_low = i;
    }

    sim.env_flags = 0;
    for (i = 0; i < 60; i++) {
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
        if (settle_stand < 0 && sim.player[0].view_height == Q2_VIEW_STAND)
            settle_stand = i;
    }

    printf("\n  down to low crouch : %d ticks\n", settle_low);
    printf("  back to standing   : %d ticks\n", settle_stand);
    printf("  eye above feet     : %d..%d world units\n",
           Q2_VIEW_CROUCH, Q2_VIEW_STAND);

    /* And the speed cap that comes with it, which is the other half of what a
     * crouch volume does. */
    printf("\n  max speed  standing %4d   in water/crouch %4d   low crouch %4d\n",
           Q2_SPEED_NORMAL, Q2_SPEED_WET, Q2_SPEED_LOWCROUCH);

    q2_sim_free(&sim);
}

/* ------------------------------------------------------------------------- */
/* Fall damage                                                                */
/* ------------------------------------------------------------------------- */
static void report_fall(void)
{
    static const s32 heights[] = { 500, 1000, 2000, 3000, 4000, 6000, 8000,
                                   12000, 16000, 24000 };
    size_t h;

    printf("\n\nFall damage -- 0x80039CB4\n\n");
    printf("  severity = ((dv >> %d)^2) / %d, thresholds %d / %d\n\n",
           Q2_FALL_SHIFT, Q2_FALL_DIV, Q2_FALL_MIN, Q2_FALL_SOFT);
    printf("  drop      landing   severity  kick   damage\n");

    for (h = 0; h < sizeof(heights) / sizeof(heights[0]); h++) {
        q2_sim sim;
        q2_input in;
        s32 spawn[3] = { 0, 0, 0 };
        s16 before;
        int i;
        s32 sev = 0, kick = 0, impact = 0;

        memset(&in, 0, sizeof(in));
        q2_sim_init(&sim, NULL, 50);

        spawn[1] = -heights[h];        /* +Y is down, so start above the floor */
        q2_sim_spawn(&sim, spawn, 0);
        sim.player[0].ground_y = 0;

        before = sim.combat.inv.health;

        for (i = 0; i < 400; i++) {
            s32 vy = (s32)(s16)sim.player[0].vel[1];
            q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
            if (sim.player[0].on_ground) {
                if (!impact)
                    impact = vy;
                /* The damage arrives on the tick AFTER the landing, because it
                 * is the ground projection that removes the velocity. One more
                 * tick is all it needs. */
                if (sim.player[0].fall_value || i > 2) {
                    q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
                    break;
                }
            }
        }

        kick = sim.player[0].fall_value;
        {
            s32 dv = impact >> Q2_FALL_SHIFT;
            sev = (dv * dv) / Q2_FALL_DIV;
        }

        printf("  %6d    %6d    %6d   %4d   %d%s\n",
               heights[h], impact, sev, kick,
               before - sim.combat.inv.health,
               (before - sim.combat.inv.health) ? "" : "");

        q2_sim_free(&sim);
    }

    printf("\n  the kick caps at %d (%d degrees) and lasts %d ticks\n",
           Q2_FALL_KICK_MAX, (Q2_FALL_KICK_MAX * 360) / 4096, Q2_FALL_KICK_TIME);
}

/* ------------------------------------------------------------------------- */
/* The environment volumes                                                    */
/* ------------------------------------------------------------------------- */
/*
 * The census that needs a disc. INCROUCH, INLOWCROUCH, INWATER, UNDERWATER and
 * DONTJUMP are UserFuncs primitives a trigger volume calls, so "does this game
 * have crouching" is a property of the level data and not of the executable.
 */
typedef struct env_tally {
    u32 maps;
    u32 crouch, lowcrouch, water, underwater, dontjump;
    u32 acid, lava, laser;
    u32 laser_seen, laser_noslot;
    u32 volumes;
} env_tally;

/* Set by report_env so cmd_pmove can end on a verdict rather than a table. */
static env_tally g_env;

static void census_map(const disc *d, const char *map, env_tally *t,
                       bool verbose, bool only_one)
{
    q2_buf buf;
    q2_common_file cf;
    q2_sim sim;
    char path[256];
    u32 i, crouch = 0, low = 0, water = 0, under = 0, nojump = 0;
    u32 acid = 0, lava = 0, laser = 0, hurt_total = 0;
    u32 laser_seen = 0, laser_noslot = 0;

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (disc_read_file(d, path, &buf) != Q2_OK)
        return;

    if (q2_common_open(&cf, &buf) != Q2_OK) {
        q2_buf_free(&buf);
        return;
    }

    memset(&sim, 0, sizeof(sim));
    q2_sim_init(&sim, NULL, 50);
    q2_sim_attach_gameplay(&sim, &cf);

    for (i = 0; i < (sim.triggers_ready ? sim.triggers.count : 0); i++) {
        u32 e = sim.volume_env ? sim.volume_env[i] : 0;

        /* The damage volumes, which the same pass reads and the same pass
         * applies — the amount is the primitive's own constant except for
         * LASERWALL, which carries its own. */
        if (sim.volume_damage && sim.volume_damage[i]) {
            hurt_total += (u32)sim.volume_damage[i];
            switch (sim.volume_mod[i]) {
            case Q2_MOD_ACID:  acid++;  break;
            case Q2_MOD_LAVA:  lava++;  break;
            case Q2_MOD_LASER: laser++; break;
            default: break;
            }
            /* One map at a time: print where to STAND, so the effect can be
             * driven rather than asserted. `--at` takes exactly this. */
            if (verbose && only_one) {
                q2_trigger tg;

                if (q2_trigger_get(&sim.triggers, i, &tg))
                    printf("      volume %3u  %2d damage mod %2d  centre "
                           "%d,%d,%d  floor y %d\n", i, sim.volume_damage[i],
                           sim.volume_mod[i],
                           (tg.min[0] + tg.max[0]) / 2,
                           (tg.min[1] + tg.max[1]) / 2,
                           (tg.min[2] + tg.max[2]) / 2,
                           tg.max[1]);
            }
        }

        /*
         * LASERWALL separately, because the census found ZERO armed and the
         * question is whether that is the reader or the disc. Its handler
         * returns before the damage when the object slot at +18 is negative
         * (`bltz` at 0x8002E220), so a wall with no object never fires — the
         * same shape as OBJDRAWOFF's empty slots (#78). Counting declared
         * against armed is what tells those two apart.
         */
        {
            q2_trigger tg;
            q2_event_record rec;
            u32 k;

            if (q2_trigger_get(&sim.triggers, i, &tg) &&
                tg.event_offset != Q2_TRIGGER_NO_EVENT &&
                q2_events_record_at(&sim.events, tg.event_offset, &rec)) {
                for (k = 0; k < rec.n_items; k++) {
                    q2_event_item it;
                    u8 ci;
                    q2_uf_call call;
                    s16 slot = 0;

                    if (!q2_events_get_item(&sim.events, &rec, k, &it))
                        break;
                    if (it.opcode != Q2_EVOP_CALL)
                        continue;
                    if (!q2_events_get_call_index(&it, &ci))
                        continue;
                    if (q2_userfuncs_prim(&sim.userfuncs, ci) !=
                        Q2_UF_LASERWALL)
                        continue;
                    laser_seen++;
                    if (q2_uf_decode_call(&call, &sim.userfuncs, &it) == Q2_OK
                        && q2_uf_operand_slot_raw(&call, 0, 0, &slot)
                        && slot < 0)
                        laser_noslot++;
                }
            }
        }

        if (!e)
            continue;
        if (e & Q2_ENV_INCROUCH)    crouch++;
        if (e & Q2_ENV_INLOWCROUCH) low++;
        if (e & Q2_ENV_INWATER)     water++;
        if (e & Q2_ENV_UNDERWATER)  under++;
        if (e & Q2_ENV_DONTJUMP)    nojump++;
    }

    t->maps++;
    t->volumes    += sim.triggers_ready ? sim.triggers.count : 0;
    t->crouch     += crouch;
    t->lowcrouch  += low;
    t->water      += water;
    t->underwater += under;
    t->dontjump   += nojump;
    t->acid        += acid;
    t->lava        += lava;
    t->laser       += laser;
    t->laser_seen  += laser_seen;
    t->laser_noslot += laser_noslot;

    if (verbose && (crouch | low | water | under | nojump | acid | lava | laser))
        printf("  %-10s %5u %5u %5u %5u %5u %5u %5u %5u\n",
               map, crouch, low, water, under, nojump, acid, lava, laser);
    (void)hurt_total;

    q2_sim_free(&sim);
    q2_common_close(&cf);
}

static void report_env(const disc *d, const char *only_map)
{
    env_tally t;
    int i, n;
    char current[64];

    memset(&t, 0, sizeof(t));
    current[0] = '\0';

    printf("\n\nEnvironment volumes -- the UserFuncs the dispatcher at "
           "0x80027E64 runs\n\n");
    printf("  %-10s %5s %5s %5s %5s %5s %5s %5s %5s\n",
           "map", "crch", "low", "watr", "under", "nojmp",
           "acid", "lava", "lasr");

    if (only_map) {
        census_map(d, only_map, &t, true, true);
    } else {
        n = disc_file_count(d);
        for (i = 0; i < n; i++) {
            const disc_file *f = disc_file_at(d, i);
            const char *p = f->path, *rest, *slash;
            char dir[64];
            size_t len;

            if (*p == '/')
                p++;
            if (strncmp(p, "Q2DATA/LEVELS/", 14) != 0)
                continue;

            rest  = p + 14;
            slash = strchr(rest, '/');
            if (!slash || strcmp(slash + 1, "COMMON.DAT") != 0)
                continue;

            len = (size_t)(slash - rest);
            if (len >= sizeof(dir))
                len = sizeof(dir) - 1;
            memcpy(dir, rest, len);
            dir[len] = '\0';

            if (strcmp(dir, current) == 0)
                continue;
            strncpy(current, dir, sizeof(current) - 1);
            current[sizeof(current) - 1] = '\0';

            census_map(d, dir, &t, true, false);
        }
    }

    printf("\n  maps scanned      : %u\n", t.maps);
    printf("  trigger volumes   : %u\n", t.volumes);
    printf("  INCROUCH          : %u\n", t.crouch);
    printf("  INLOWCROUCH       : %u\n", t.lowcrouch);
    printf("  INWATER           : %u\n", t.water);
    printf("  UNDERWATER        : %u\n", t.underwater);
    printf("  DONTJUMP          : %u   (entity flag 0x20000 -- the jump gate\n"
           "                       worldscale.h recorded as never set)\n",
           t.dontjump);
    printf("  INACID/UNDERACID  : %u   1 damage, mod 9, throttled to 400 ticks\n",
           t.acid);
    printf("  INLAVA/UNDERLAVA  : %u   20 damage, mod 10, throttled to 100\n",
           t.lava);
    printf("  LASERWALL         : %u armed of %u declared; %u name an EMPTY\n"
           "                       object slot, and its handler returns before\n"
           "                       the damage when that slot is negative\n",
           t.laser, t.laser_seen, t.laser_noslot);

    g_env = t;
}

/* ------------------------------------------------------------------------- */
/* Walking a real map                                                         */
/* ------------------------------------------------------------------------- */
/*
 * The whole frame against real geometry: stand, walk, jump, and watch the view
 * angles the camera would actually use. `q2_sim_view_angles` is the thing to
 * look at — the aim and the view are not the same numbers, and the difference is
 * three decaying kicks.
 */
/* ------------------------------------------------------------------------- */
/* Standing in it                                                             */
/* ------------------------------------------------------------------------- */
/*
 * The census says which volumes hurt and for how much. This says what that
 * DOES, which is a different claim and the one that can be wrong.
 *
 * The player is placed at each damage volume's centre and the sim is ticked for
 * 900 units of level clock — three seconds — with no input. What is being
 * measured is the RATE: the damage function throttles mods 9 and 10 per target
 * (`env_next`, 400 and 100 ticks), so acid should land about twice in three
 * seconds and lava about nine times, whatever the tick rate of the host.
 *
 * That throttle is also the reason this check exists. It was silently broken
 * for as long as the port had one: `q2_actor_from_player` rebuilt the actor
 * from the inventory on every hit and cleared the deadline it had just armed,
 * so lava landed once per tick. The failure is invisible in the census and
 * obvious here — a full-health player dies in under a fifth of a second.
 */
static void report_hazards(const disc *d, const char *map, int zone_index)
{
    q2_world_zone zone;
    q2_sim sim;
    q2_input in;
    q2_buf buf;
    q2_common_file cf;
    char path[256];
    u32 i, shown = 0;

    if (q2_world_load_zone(&zone, d, map, zone_index) != Q2_OK)
        return;

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (disc_read_file(d, path, &buf) != Q2_OK) {
        q2_world_free_zone(&zone);
        return;
    }
    if (q2_common_open(&cf, &buf) != Q2_OK) {
        q2_buf_free(&buf);
        q2_world_free_zone(&zone);
        return;
    }

    printf("\n\nStanding in %s's damage volumes for three seconds of level "
           "clock\n\n", map);
    printf("  %-6s %-6s %6s %7s %7s   %s\n",
           "volume", "mod", "amount", "hits", "health", "expected");

    for (i = 0; ; i++) {
        s32 feet[3];
        q2_trigger tg;
        u32 hits;
        const char *what;
        int want;

        q2_sim_init(&sim, &zone, 50);
        q2_sim_attach_gameplay(&sim, &cf);

        if (!sim.triggers_ready || i >= sim.triggers.count) {
            q2_sim_free(&sim);
            break;
        }
        if (!sim.volume_damage || !sim.volume_damage[i] ||
            !q2_trigger_get(&sim.triggers, i, &tg)) {
            q2_sim_free(&sim);
            continue;
        }

        feet[0] = (tg.min[0] + tg.max[0]) / 2;
        feet[1] = (tg.min[1] + tg.max[1]) / 2;
        feet[2] = (tg.min[2] + tg.max[2]) / 2;

        /*
         * Spawned WITHOUT settling. The settle loop drives real ticks, so a
         * volume the player cannot stand in would take its damage before the
         * measurement started and the health column would report a corpse.
         */
        q2_sim_spawn(&sim, feet, 0);
        sim.hazard_hits = 0;

        memset(&in, 0, sizeof(in));
        while (sim.level_time < 900) {
            /*
             * HELD in the volume. Without this the player falls out of it —
             * these centres are points in space, not ledges — and the count
             * measures how long gravity took rather than the throttle. The
             * first run of this gave 1, 2 and 3 hits for identical acid
             * volumes, which is exactly what that looks like.
             */
            sim.player[0].pos[0] = feet[0];
            sim.player[0].pos[1] = feet[1];
            sim.player[0].pos[2] = feet[2];
            q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
        }

        hits = sim.hazard_hits;
        switch (sim.volume_mod[i]) {
        /*
         * One hit per throttle window over the three seconds, rounded up: the
         * first lands at t=0 and each later one at the first TICK past its
         * deadline, so nine for lava and three for acid.
         */
        case Q2_MOD_ACID:
            what = "acid";
            want = (900 + Q2_ENV_THROTTLE_ACID - 1) / Q2_ENV_THROTTLE_ACID;
            break;
        case Q2_MOD_LAVA:
            what = "lava";
            want = (900 + Q2_ENV_THROTTLE_LAVA - 1) / Q2_ENV_THROTTLE_LAVA;
            break;
        default:
            what = "laser";
            want = -1;
            break;
        }

        printf("  %6u %-6s %6d %7u %7d   %d\n", i, what, sim.volume_damage[i],
               hits, sim.combat.inv.health, want);
        shown++;

        q2_sim_free(&sim);
    }

    if (!shown)
        printf("  (none in this zone)\n");

    q2_common_close(&cf);
    q2_buf_free(&buf);
    q2_world_free_zone(&zone);
}

/* ------------------------------------------------------------------------- */
/* Shooting the breakables                                                    */
/* ------------------------------------------------------------------------- */
/*
 * The registry and the sweep, exercised: each box is registered, a segment is
 * fired straight through its middle, and what came back is reported.
 *
 * GLASS answers in DEBRIS and SHOOTTHEN answers by RAISING ITS RECORD, and the
 * two numbers are the two halves of `0x8002EF1C`'s job. Firing a segment rather
 * than driving a weapon is deliberate: where a pane sits relative to a floor
 * the player can stand on is a property of the map, and this is a check on the
 * route, not on the level design.
 *
 * The operands are read IN PLACE — no zone rebase — so a map whose slot the
 * engine has already consumed reports fewer boxes here than in the client. The
 * client logs its own count at load and applies the rebase.
 */
static void report_breakables(const disc *d, const char *map, int zone_index)
{
    q2_world_zone zone;
    q2_sim sim;
    q2_buf buf;
    q2_common_file cf;
    char path[256];
    u32 i, n;

    if (q2_world_load_zone(&zone, d, map, zone_index) != Q2_OK)
        return;

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (disc_read_file(d, path, &buf) != Q2_OK) {
        q2_world_free_zone(&zone);
        return;
    }
    if (q2_common_open(&cf, &buf) != Q2_OK) {
        q2_buf_free(&buf);
        q2_world_free_zone(&zone);
        return;
    }

    q2_sim_init(&sim, &zone, 50);
    q2_sim_attach_gameplay(&sim, &cf);
    n = q2_sim_attach_breakables(&sim, &zone.scene, NULL);

    if (n) {
        printf("\n\n%s's breakables, each shot through the middle of its box\n\n",
               map);
        printf("  %-3s %-10s %5s %5s %7s %8s   %s\n",
               "#", "kind", "node", "hp", "pieces", "records", "box");
        printf("  (hp is what the shot LEFT; pieces read zero here because"
               " this harness attaches no\n   effect tables — the client"
               " reports them, 11 on LAB's second pane)\n");
    }

    for (i = 0; i < n; i++) {
        const q2_breakable *b = &sim.breakable[i];
        s32 from[3], to[3];
        u32 pieces, before = sim.breakable_fired;
        int k;

        /* Straight through the box on its widest axis, starting well outside
         * it so the segment genuinely crosses. */
        for (k = 0; k < 3; k++) {
            s32 mid = (b->bmin[k] + b->bmax[k]) / 2;

            from[k] = mid;
            to[k]   = mid;
        }
        from[0] = b->bmin[0] - 4096;
        to[0]   = b->bmax[0] + 4096;

        pieces = q2_sim_breakable_shot(&sim, from, to, 100);

        printf("  %-3u %-10s %5d %5d %7u %8u   (%d,%d,%d)-(%d,%d,%d)\n", i,
               b->kind == Q2_BREAKABLE_SHOOTTHEN ? "SHOOTTHEN" : "GLASS",
               b->scene_node, b->health, pieces,
               sim.breakable_fired - before,
               b->bmin[0], b->bmin[1], b->bmin[2],
               b->bmax[0], b->bmax[1], b->bmax[2]);
    }

    q2_sim_free(&sim);
    q2_common_close(&cf);
    q2_buf_free(&buf);
    q2_world_free_zone(&zone);
}

static void report_walk(const disc *d, const char *map, int zone_index)
{
    q2_world_zone zone;
    q2_sim sim;
    q2_input in;
    s32 feet[3] = { 0, 0, 0 };
    s32 apex = 0, ground_y;
    int i, jumped = -1;
    u32 steps = 0;
    q2_start_pos_list spawns;
    q2_buf buf;
    q2_common_file cf;
    char path[256];

    if (q2_world_load_zone(&zone, d, map, zone_index) != Q2_OK) {
        printf("\n\n  (cannot load %s zone %d)\n", map, zone_index);
        return;
    }

    printf("\n\nOne frame at a time in %s/ZONE%d\n\n", map, zone_index);

    q2_sim_init(&sim, &zone, 50);

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (disc_read_file(d, path, &buf) == Q2_OK) {
        if (q2_common_open(&cf, &buf) == Q2_OK) {
            u32 k;
            if (q2_start_pos_parse(&spawns, &cf) == Q2_OK) {
                for (k = 0; k < spawns.count; k++) {
                    q2_start_pos sp;
                    if (!q2_start_pos_get(&spawns, k, &sp) ||
                        sp.zone != zone_index)
                        continue;
                    feet[0] = sp.x; feet[1] = sp.y; feet[2] = sp.z;
                    break;
                }
            }
            q2_sim_attach_gameplay(&sim, &cf);
            /* cf/buf are borrowed by the sim for the rest of this command. */
        } else {
            q2_buf_free(&buf);
        }
    }

    q2_sim_spawn(&sim, feet, 0);

    memset(&in, 0, sizeof(in));
    for (i = 0; i < 60; i++)          /* land */
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);

    ground_y = sim.player[0].pos[1];
    printf("  landed at y=%d, view height %d, %s\n",
           ground_y, sim.player[0].view_height,
           sim.player[0].on_ground ? "on the ground" : "STILL FALLING");

    printf("\n  tick  height  view  pitch   yaw  roll   speed  sounds\n");

    in.forward = Q2_PAD_FULL;
    for (i = 0; i < 90; i++) {
        s32 view[3];
        s32 height;
        const q2_ent_events *ev;
        char snd[64];

        /* A jump a third of the way in, while already running. */
        in.buttons = (i == 30) ? Q2_BTN_JUMP : 0;
        if (i == 30) jumped = i;

        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);

        height = ground_y - sim.player[0].pos[1];
        if (height > apex) apex = height;

        q2_sim_view_angles(&sim, view);

        snd[0] = '\0';
        ev = q2_sim_entity_events(&sim);
        if (ev) {
            u32 k;
            for (k = 0; k < ev->count; k++) {
                if (ev->e[k].kind != Q2_ENT_EVENT_SOUND)
                    continue;
                switch (ev->e[k].sound) {
                case Q2_SND_FOOTSTEP_A:   snprintf(snd, sizeof(snd), "step L"); steps++; break;
                case Q2_SND_FOOTSTEP_B:   snprintf(snd, sizeof(snd), "step R"); steps++; break;
                case Q2_SND_FOOTSTEP_WET: snprintf(snd, sizeof(snd), "splash"); steps++; break;
                case Q2_SND_LAND:         snprintf(snd, sizeof(snd), "LAND");  break;
                default: break;
                }
            }
        }

        if (i % 5 == 0 || snd[0] || i == 30 || (i > 30 && i < 40))
            printf("  %4d  %6d  %4d  %5d %5d %5d  %6d  %s\n",
                   i, height, (s32)sim.player[0].view_height,
                   view[0], view[1], view[2],
                   (s32)(s16)sim.player[0].vel[0] + (s32)(s16)sim.player[0].vel[2],
                   snd);
    }

    printf("\n  jumped on tick %d, apex %d world units\n", jumped, apex);
    printf("  footstep sounds emitted : %u\n", steps);
    printf("  aim pitch/yaw/roll      : %d %d %d\n",
           sim.player[0].pitch, sim.player[0].yaw, sim.player[0].roll);
    {
        s32 view[3];
        q2_sim_view_angles(&sim, view);
        printf("  view pitch/yaw/roll     : %d %d %d\n",
               view[0], view[1], view[2]);
    }

    /*
     * The four camera states this frame actually passes through, as the two
     * numbers `q2psx-inspect fps` takes: how far the eye has moved from where a
     * standing player's sits, and the roll. Printed rather than described so a
     * picture of any of them is a picture of a measured player.
     */
    {
        struct { const char *what; s32 dy; s32 roll; } shot[4];
        int k;

        /* Standing, having walked to a stop. */
        memset(&in, 0, sizeof(in));
        for (i = 0; i < 40; i++)
            q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
        shot[0].what = "standing";
        shot[0].dy   = Q2_VIEW_STAND - sim.player[0].view_height;
        shot[0].roll = 0;

        /* Crouched, driven by the same flag a volume asserts. */
        sim.env_flags = Q2_ENT_INLOWCROUCH;
        for (i = 0; i < 40; i++)
            q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
        shot[1].what = "low crouch";
        shot[1].dy   = Q2_VIEW_STAND - sim.player[0].view_height;
        shot[1].roll = 0;
        sim.env_flags = 0;
        for (i = 0; i < 40; i++)
            q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);

        /* Strafing hard, for the lean. */
        {
            s32 view[3];
            in.side = Q2_PAD_FULL;
            for (i = 0; i < 40; i++)
                q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
            q2_sim_view_angles(&sim, view);
            shot[2].what = "strafing";
            shot[2].dy   = Q2_VIEW_STAND - sim.player[0].view_height;
            shot[2].roll = view[2];
            in.side = 0;
            for (i = 0; i < 40; i++)
                q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
        }

        /* And the apex of a standing jump, measured rather than assumed. */
        {
            s32 base = sim.player[0].pos[1];
            s32 best = 0;

            in.buttons = Q2_BTN_JUMP;
            for (i = 0; i < 40; i++) {
                s32 h;
                q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
                in.buttons = 0;
                h = base - sim.player[0].pos[1];
                if (h > best) best = h;
                if (i > 2 && sim.player[0].on_ground) break;
            }
            shot[3].what = "jump apex";
            shot[3].dy   = -best;      /* the eye rises, and +Y is down */
            shot[3].roll = 0;
        }

    /*
     * The recoil reaching the camera. `player.pitch` is the aim and does not
     * move; the whole of the difference below is the firing kick decaying over
     * its 30 ticks, which is the output 0x80038260 exists to produce.
     */
    {
        s32 view[3];
        int w;

        memset(&in, 0, sizeof(in));
        for (i = 0; i < 20; i++)
            q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);

        printf("\n  recoil into the view — the aim never moves:\n");
        printf("    weapon        aim   view over the next 5 ticks\n");

        for (w = 1; w <= 3; w++) {
            int n;

            sim.combat.inv.weapons = 0x7FF;
            for (n = 0; n < Q2_AMMO_COUNT; n++)
                sim.combat.inv.ammo[n] = 200;
            sim.combat.weapon_id = w;
            sim.combat.next_fire = 0;

            in.attack = true;
            q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
            in.attack = false;

            printf("    %-13s %4d ", q2_weapon_tables_builtin()->name[w],
                   sim.player[0].pitch);
            for (k = 0; k < 5; k++) {
                q2_sim_view_angles(&sim, view);
                printf(" %5d", view[0]);
                q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
            }
            printf("\n");

            for (k = 0; k < 40; k++)
                q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
        }
    }

    /*
     * And the environment volumes end to end: stand the player in each one and
     * report what the frame does about it. This is the step that turns "the
     * record calls INCROUCH" into "the player crouches" — the two are separated
     * by the whole dispatcher, and only this shows both.
     */
    if (sim.volume_env && sim.triggers_ready) {
        u32 v;
        bool any = false;

        for (v = 0; v < sim.triggers.count; v++) {
            q2_trigger t;
            s32 at[3];
            u32 env;

            if (!sim.volume_env[v])
                continue;
            if (!q2_trigger_get(&sim.triggers, v, &t))
                continue;

            if (!any) {
                printf("\n  standing in each environment volume:\n");
                printf("    vol  asserts                  at                 view  speed\n");
                any = true;
            }

            /*
             * The box centre. A volume whose exact plane hull excludes its own
             * AABB centre — they are convex hulls, not boxes — reports "(centre
             * outside the hull)", which is a property of the probe rather than
             * of the dispatcher: the record's CALL was still resolved, or this
             * line would not be printed at all.
             */
            at[0] = (t.min[0] + t.max[0]) / 2;
            at[1] = (t.min[1] + t.max[1]) / 2;
            at[2] = (t.min[2] + t.max[2]) / 2;

            q2_sim_spawn(&sim, at, 0);
            memset(&in, 0, sizeof(in));
            for (i = 0; i < 60; i++)
                q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);

            env = sim.player[0].ent.flags;
            printf("    %3u  %-24s %6d,%6d,%6d  %4d  %5d\n", v,
                   (env & Q2_ENV_INLOWCROUCH) ? "INLOWCROUCH" :
                   (env & Q2_ENV_INCROUCH)    ? "INCROUCH"    :
                   (env & Q2_ENV_UNDERWATER)  ? "UNDERWATER"  :
                   (env & Q2_ENV_INWATER)     ? "INWATER"     :
                   (env & Q2_ENT_JUMP_LATCH)  ? "DONTJUMP"    :
                                                "(centre outside the hull)",
                   at[0], at[1], at[2],
                   (s32)sim.player[0].view_height,
                   (env & Q2_ENV_INLOWCROUCH) ? Q2_SPEED_LOWCROUCH :
                   (env & (Q2_ENV_INWATER | Q2_ENV_UNDERWATER | Q2_ENV_INCROUCH))
                       ? Q2_SPEED_WET : Q2_SPEED_NORMAL);
        }
    }

        printf("\n  camera states, as `fps ... [yaw] [gunyaw] [eye_dy] [roll]`:\n");
        for (k = 0; k < 4; k++)
            printf("    %-11s eye_dy %5d   roll %4d\n",
                   shot[k].what, shot[k].dy, shot[k].roll);
    }

    q2_sim_free(&sim);
}

/* ------------------------------------------------------------------------- */
int cmd_pmove(const disc *d, const char *map, int zone_index)
{
    report_styles();
    report_jump();
    report_view_height();
    report_fall();

    if (!d)
        return 0;

    report_env(d, map);
    if (map) {
        report_hazards(d, map, zone_index);
        report_breakables(d, map, zone_index);
        report_walk(d, map, zone_index);
    }

    /*
     * The verdict, and it is deliberately about the DATA rather than about the
     * arithmetic above: the constants can be checked in a test with no disc,
     * but "the game contains crouch tunnels and no-jump zones" is a claim only
     * the level files can settle. A whole-disc scan finding none of them would
     * mean the CALL resolution is wrong, not that the game has no crouching.
     */
    if (map) {
        printf("\n(scanned one map; run without a map name for the whole disc)\n");
        return 0;
    }

    if (g_env.crouch && g_env.lowcrouch && g_env.water && g_env.underwater &&
        g_env.dontjump) {
        printf("\nPASS - every environment primitive is authored somewhere on "
               "the disc.\n");
        return 0;
    }

    printf("\nFAIL - an environment primitive resolves nowhere on the disc.\n");
    return 1;
}
