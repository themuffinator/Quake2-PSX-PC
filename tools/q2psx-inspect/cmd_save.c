/*
 * cmd_save.c — the save system, exercised against a real disc.
 *
 * tests/test_save.c drives the same code against a synthetic simulation, which
 * is the right place to check a corner. This is the other half: a real zone, a
 * real event script, real trigger volumes and a real item population, walked
 * forward and then round-tripped through a file. A field that survives a
 * hand-built struct and not a map is a field the unit test cannot see.
 *
 * It also drives the memory-card front end, printing the transitions by the
 * console's own state numbers (memcard.h) — and, with an output path, draws
 * every screen it passes through with the console's own font.
 */
#include "cmd_save.h"

/* The FORMATS entity header, for StartPos. From here `entity.h` resolves down
 * the include path rather than beside the includer, so this is the on-disc one
 * and not the game's — see the two guards. */
#include "entity.h"

#include "hudtables.h"
#include "ident.h"
#include "memcard.h"
#include "menu.h"
#include "menudraw.h"
#include "menufont.h"
#include "mission.h"
#include "raster.h"
#include "save.h"
#include "saveui.h"
#include "sim.h"
#include "vram.h"
#include "world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_bad;

static void ok(bool cond, const char *what)
{
    printf("  %-44s %s\n", what, cond ? "ok" : "MISMATCH");
    if (!cond)
        g_bad++;
}

static void ok_eq(s64 got, s64 want, const char *what)
{
    if (got == want) {
        printf("  %-44s ok\n", what);
    } else {
        printf("  %-44s MISMATCH: %lld != %lld\n", what,
               (long long)got, (long long)want);
        g_bad++;
    }
}

/* ------------------------------------------------------------------------- */
/* The world, brought up the way the client brings one up                     */
/* ------------------------------------------------------------------------- */
typedef struct fixture {
    q2_world_zone  zone;
    q2_common_file common;
    q2_sim         sim;
    bool           zone_ready;
    bool           common_ready;
} fixture;

static bool fixture_open(fixture *f, const disc *d, const char *map, int zone)
{
    q2_buf buf;
    char path[256];
    s32 feet[3] = { 0, 0, 0 };
    bool placed = false;

    memset(f, 0, sizeof(*f));

    if (q2_world_load_zone(&f->zone, d, map, zone) != Q2_OK) {
        fprintf(stderr, "cannot load %s zone %d\n", map, zone);
        return false;
    }
    f->zone_ready = true;

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (disc_read_file(d, path, &buf) == Q2_OK) {
        if (q2_common_open(&f->common, &buf) == Q2_OK) {
            q2_start_pos_list spawns;

            f->common_ready = true;
            if (q2_start_pos_parse(&spawns, &f->common) == Q2_OK) {
                u32 i;
                for (i = 0; i < spawns.count; i++) {
                    q2_start_pos sp;
                    if (!q2_start_pos_get(&spawns, i, &sp) || sp.zone != zone)
                        continue;
                    feet[0] = sp.x;
                    feet[1] = sp.y;
                    feet[2] = sp.z;
                    placed  = true;
                    break;
                }
            }
        } else {
            q2_buf_free(&buf);
        }
    }

    q2_sim_init(&f->sim, &f->zone, 50);
    if (f->common_ready) {
        q2_sim_attach_gameplay(&f->sim, &f->common);
        /* The item population, so the save's entity chunk has real records to
         * carry rather than an empty set. This zone's, as a session would have
         * it — the round trip is about what a save holds, and a save holds the
         * set the player is standing in. */
        q2_sim_attach_items(&f->sim, &f->common, zone, NULL, NULL);
    }
    q2_sim_spawn(&f->sim, feet, 0);
    f->sim.player[0].ground_y = feet[1];

    printf("%s zone %d: %u nodes, %u entities, %u events, %u triggers%s\n",
           map, zone, f->zone.scene.node_count, f->sim.entities.count,
           f->sim.event_rt.record_count, f->sim.triggers.count,
           placed ? "" : " (no StartPos here — using the origin)");
    return true;
}

static void fixture_close(fixture *f)
{
    q2_sim_free(&f->sim);
    if (f->common_ready)
        q2_common_close(&f->common);
    if (f->zone_ready)
        q2_world_free_zone(&f->zone);
    memset(f, 0, sizeof(*f));
}

/* Walk forward so the state being saved is a played state rather than a spawn:
 * the clock advances, the mover caches a cell, and any item under the spawn
 * point is collected. */
static void fixture_run(fixture *f, int ticks)
{
    q2_input in;
    int i;

    memset(&in, 0, sizeof(in));
    in.forward = Q2_INPUT_FULL;

    for (i = 0; i < ticks; i++)
        q2_sim_tick(&f->sim, &in, f->sim.dt_per_field * 2);
}

/* ------------------------------------------------------------------------- */
/* The round trip                                                             */
/* ------------------------------------------------------------------------- */
static u32 count_collected(const q2_sim *sim)
{
    u32 i, n = 0;
    for (i = 0; i < sim->entities.count; i++)
        if (!sim->entities.ent[i].in_use)
            n++;
    return n;
}

/*
 * How many records carry RUNTIME state — bits 0-2, which are the ones clear on
 * disc (events.h). Counting any non-zero byte instead would report every record
 * on every map, because the category bits above them are always set.
 */
#define EVREC_RUNTIME_BITS (Q2_EVREC_HASRUN | Q2_EVREC_RT1 | Q2_EVREC_RT2)

static u32 count_fired(const q2_save *s)
{
    u32 i, n = 0;
    for (i = 0; i < s->event_count; i++)
        if (s->event_flags[i] & EVREC_RUNTIME_BITS)
            n++;
    return n;
}

static long file_size(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long n;

    if (!fp)
        return -1;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fclose(fp);
    return n;
}

static void report_capture(const q2_save *s, const q2_sim *sim,
                           const char *path)
{
    long size = file_size(path);

    printf("\ncaptured\n");
    printf("  player      %d %d %d  yaw %d pitch %d  cell %d\n",
           (int)s->player.pos[0], (int)s->player.pos[1], (int)s->player.pos[2],
           (int)s->player.yaw, (int)s->player.pitch, (int)s->player.ent.node);
    printf("  clock       %d dt (%d:%02d), tick %u\n",
           (int)s->level_time, (int)(s->level_time / 300 / 60),
           (int)(s->level_time / 300 % 60), s->tick_count);
    printf("  condition   %d health, %d armour, weapon %d\n",
           (int)s->inventory.health, (int)s->inventory.armour,
           (int)s->weapon_id);
    printf("  entities    %u slots, %u collected\n",
           s->entity_count, count_collected(sim));
    printf("  script      %u records, %u with runtime state\n",
           s->event_count, count_fired(s));
    printf("  triggers    %u volumes\n", s->trigger_count);
    printf("  label       \"%s\"\n", s->label);
    if (size >= 0)
        printf("  file        %ld bytes\n", size);
}

static void compare(const q2_save *a, const q2_save *b)
{
    u32 i;
    bool same;

    printf("\nround trip\n");

    ok(strcmp(a->map, b->map) == 0, "the map name survives");
    ok(strcmp(a->serial, b->serial) == 0, "the disc serial survives");
    ok(strcmp(a->label, b->label) == 0, "the label survives");
    ok_eq(b->zone, a->zone, "the zone survives");

    ok_eq(b->player.pos[0], a->player.pos[0], "the player's x survives");
    ok_eq(b->player.pos[1], a->player.pos[1], "the player's y survives");
    ok_eq(b->player.pos[2], a->player.pos[2], "the player's z survives");
    ok_eq(b->player.vel[1], a->player.vel[1], "their velocity survives");
    ok_eq(b->player.ent.node, a->player.ent.node,
          "the mover's cached cell survives");
    ok_eq((s64)b->player.ent.flags, (s64)a->player.ent.flags,
          "the mover's flags word survives");
    ok_eq(b->player.view_height, a->player.view_height,
          "the view height survives");

    ok_eq(b->level_time, a->level_time, "the level clock survives");
    ok_eq(b->tick_count, a->tick_count, "the tick count survives");
    ok_eq(b->gravity, a->gravity, "gravity survives");

    ok_eq(b->inventory.health, a->inventory.health, "health survives");
    ok_eq(b->inventory.weapons, a->inventory.weapons,
          "the weapons held survive");
    ok_eq((s64)b->inventory.flags, (s64)a->inventory.flags,
          "the client flags word survives");
    ok_eq(b->weapon_id, a->weapon_id, "the held weapon survives");
    ok_eq((s64)b->rng_state, (s64)a->rng_state,
          "the weapon generator survives");

    ok_eq(b->event_count, a->event_count, "every script record survives");
    same = (a->event_count == b->event_count);
    for (i = 0; same && i < a->event_count; i++)
        same = (a->event_flags[i] == b->event_flags[i]);
    ok(same, "every script FLAG survives");

    ok_eq(b->trigger_count, a->trigger_count, "every trigger volume survives");
    same = (a->trigger_count == b->trigger_count);
    for (i = 0; same && i < a->trigger_count; i++)
        same = (a->trigger_inside[i] == b->trigger_inside[i]);
    ok(same, "the trigger residency survives");

    ok_eq(b->entity_count, a->entity_count, "every entity slot survives");
    same = (a->entity_count == b->entity_count);
    for (i = 0; same && i < a->entity_count; i++) {
        const q2_save_entity *x = &a->entities[i], *y = &b->entities[i];
        same = x->place_id == y->place_id && x->in_use == y->in_use &&
               x->taken == y->taken && x->hidden == y->hidden &&
               x->scale == y->scale && x->frame == y->frame &&
               x->think == y->think && x->respawn_at == y->respawn_at;
    }
    ok(same, "every entity's state survives");
}

/* ------------------------------------------------------------------------- */
/* Rendering one front-end screen                                             */
/* ------------------------------------------------------------------------- */
typedef struct shooter {
    q2_hud_tables    tab;
    q2_menu_font     font;
    psx_vram        *vram;
    q2_menu_settings set;
    bool             ready;
} shooter;

static bool shooter_open(shooter *sh, const disc *d, const char *map)
{
    q2_build_id id;
    q2_vram_section vs;
    q2_result r;

    memset(sh, 0, sizeof(*sh));

    if (q2_identify(d, &id) != Q2_OK)
        return false;
    if (q2_hud_tables_load(&sh->tab, d, &id) != Q2_OK)
        return false;
    if (q2_vram_load(&vs, d, map) != Q2_OK) {
        q2_hud_tables_free(&sh->tab);
        return false;
    }

    sh->vram = (psx_vram *)calloc(1, sizeof(psx_vram));
    if (!sh->vram) {
        q2_vram_free(&vs);
        q2_hud_tables_free(&sh->tab);
        return false;
    }

    r = q2_menu_font_upload(&sh->font, &sh->tab, &vs, sh->vram, false, 1);
    q2_vram_free(&vs);
    if (r != Q2_OK) {
        fprintf(stderr, "%s carries no menu font\n", map);
        free(sh->vram);
        q2_hud_tables_free(&sh->tab);
        return false;
    }

    q2_menu_settings_defaults(&sh->set);
    sh->ready = true;
    return true;
}

static void shooter_close(shooter *sh)
{
    if (!sh->ready)
        return;
    free(sh->vram);
    q2_hud_tables_free(&sh->tab);
    sh->ready = false;
}

static void shooter_shoot(shooter *sh, const q2_menu *m, const char *path)
{
    const int W = Q2_MENU_SCREEN_W, H = Q2_MENU_SCREEN_H;
    q2_menu_draw_opts opts;
    psx_framebuffer fb;
    psx_raster_opts ropts;
    psx_ot ot;
    u32 prims;

    if (!sh->ready || !m->page)
        return;

    if (psx_ot_init(&ot, 256, 8192) != Q2_OK)
        return;
    if (psx_fb_init(&fb, W, H) != Q2_OK) {
        psx_ot_free(&ot);
        return;
    }

    q2_menu_draw_opts_default(&opts, &sh->font);

    /* Nothing behind it here; the console has the frozen world there. */
    psx_fb_clear(&fb, psx_rgb555(16, 16, 40));

    prims = q2_menu_build_ot(m, &ot, &opts);

    psx_raster_opts_default(&ropts);
    psx_raster_ot(&fb, &ot, sh->vram, &ropts);

    if (psx_fb_write_ppm(&fb, path) == Q2_OK)
        printf("  wrote %s (%d primitives)\n", path, prims);
    else
        fprintf(stderr, "cannot write %s\n", path);

    psx_fb_free(&fb);
    psx_ot_free(&ot);
}

/* ------------------------------------------------------------------------- */
/* The front end, driven                                                      */
/* ------------------------------------------------------------------------- */
/*
 * The shadow menu the client uses to navigate and draw a card screen. Built
 * here the same way, so what the tool draws is what the client shows.
 */
static void card_page(q2_menu *m, const q2_save_ui *ui, int state)
{
    const q2_menu_page *page =
        q2_mcard_page(q2_mcard_screen_for_state_port(state));
    int i;

    m->page   = page;
    m->open   = (page != NULL);
    m->cursor = page ? (int)page->first : 0;
    memset(m->text, 0, sizeof(m->text));
    memset(m->disabled, 0, sizeof(m->disabled));

    if (!page)
        return;

    if (page == q2_mcard_page(Q2_MCARD_SAVE_FILE)) {
        for (i = 0; i < Q2_SAVE_SLOTS; i++) {
            if (ui->info[i].used)
                snprintf(m->text[i + 1], Q2_MENU_TEXT_MAX, "%s",
                         q2_save_ui_row(ui, i));
            else if (ui->mode == Q2_SAVE_UI_SAVE)
                snprintf(m->text[i + 1], Q2_MENU_TEXT_MAX, "%d", i + 1);
            else
                m->disabled[i + 1] = 1;
        }
    } else if (page == q2_mcard_page(Q2_MCARD_LOAD_MESSAGE)) {
        /* Both rows, because an empty override falls back to the table's own
         * label and the second one says HERE. */
        snprintf(m->text[0], Q2_MENU_TEXT_MAX, "%.*s",
                 Q2_MENU_TEXT_MAX - 1, ui->message);
        snprintf(m->text[1], Q2_MENU_TEXT_MAX, "%.*s", Q2_MENU_TEXT_MAX - 1,
                 ui->detail[0] ? ui->detail : " ");
    }
}

static const char *shot_name(int state)
{
    switch (state) {
    case Q2_SAVEUI_STATE_LIST:   return "list";
    case Q2_SAVEUI_STATE_CHOICE: return "overwrite";
    case Q2_SAVEUI_STATE_BUSY:   return "busy";
    case Q2_SAVEUI_STATE_REPORT: return "report";
    default:                     return "state";
    }
}

static void shoot_state(shooter *sh, const q2_save_ui *ui, int state,
                        const char *out, const char *tag)
{
    q2_menu m;
    char path[512];

    if (!sh->ready || !out)
        return;

    memset(&m, 0, sizeof(m));
    m.set      = &sh->set;
    m.screen_h = Q2_MENU_SCREEN_H;
    card_page(&m, ui, state);
    if (!m.page)
        return;

    /* Put the cursor on the row the flow is about, so the selection bar is
     * where a player would have left it. */
    if (state == Q2_SAVEUI_STATE_LIST && ui->slot >= 0)
        m.cursor = (int)m.page->first + ui->slot;
    else if (state == Q2_SAVEUI_STATE_CHOICE)
        m.cursor = (int)m.page->first + 1;   /* YES */

    snprintf(path, sizeof(path), "%s.%s-%s.ppm", out, tag, shot_name(state));
    shooter_shoot(sh, &m, path);
}

static void drive(q2_save_ui *ui, const q2_save *snapshot, shooter *sh,
                  const char *out, bool saving, int slot)
{
    const char *tag = saving ? "save" : "load";
    int guard = 0;

    printf("\nthe front end, %s\n", saving ? "saving" : "loading");

    if (saving)
        q2_save_ui_open_save(ui, snapshot);
    else
        q2_save_ui_open_load(ui);

    printf("  state %2d  %-14s the four rows\n", ui->state,
           q2_mcard_screen_name(q2_mcard_screen_for_state_port(ui->state)));
    ui->slot = slot;
    shoot_state(sh, ui, ui->state, out, tag);

    /* The row is chosen: `choose` then `request` with the list's own number,
     * which is what 0x8001F140 does. */
    q2_save_ui_choose(ui, slot);
    q2_save_ui_request(ui, Q2_SAVEUI_STATE_LIST);

    while (ui->open && guard++ < 8) {
        int state = ui->state;

        printf("  state %2d  %-14s %s\n", state,
               q2_mcard_screen_name(q2_mcard_screen_for_state_port(state)),
               state == Q2_SAVEUI_STATE_CHOICE ? "the overwrite question" :
               state == Q2_SAVEUI_STATE_BUSY   ? "the work" :
               state == Q2_SAVEUI_STATE_REPORT ? ui->message : "");

        shoot_state(sh, ui, state, out, tag);

        if (state == Q2_SAVEUI_STATE_CHOICE) {
            /* Row 1 is YES: 0x8001F790 requests state 6. */
            q2_save_ui_request(ui, Q2_SAVEUI_STATE_BUSY);
        } else if (state == Q2_SAVEUI_STATE_BUSY) {
            q2_save_ui_update(ui);
        } else if (state == Q2_SAVEUI_STATE_REPORT) {
            q2_save_ui_acknowledge(ui);
        } else {
            break;
        }
    }

    printf("  outcome   %s\n", q2_save_ui_status_name(ui->status));
    ok(ui->status == (saving ? Q2_SAVE_UI_SAVED : Q2_SAVE_UI_LOADED),
       saving ? "the front end saved" : "the front end loaded");
}

/* ------------------------------------------------------------------------- */
int cmd_save(const disc *d, const char *map, const char *out)
{
    fixture f;
    shooter sh;
    q2_save captured, reloaded, taken;
    q2_save_ui ui;
    q2_save_info info[Q2_SAVE_SLOTS];
    q2_build_id id;
    q2_mission mission;
    char dir[512];
    char path[512];
    const char *tmp = getenv("TEMP");
    u32 in_use;

    if (!map || !*map)
        map = "BASE1";

    g_bad = 0;

    printf("Q2PSX-PC save system\n\n");
    printf("format      %s version %d, chunked\n",
           Q2_SAVE_MAGIC, Q2_SAVE_VERSION);
    printf("chunks      HEAD PLYR SIMS INVN CMBT PROJ EVNT TRIG ENTS ITEM "
           "BRKS MOVR CREA MISN SETT\n");
    printf("slots       %d, the four rows of SAVE FILE\n", Q2_SAVE_SLOTS);

    /* Somewhere to work that is not the player's own save directory. */
    if (!tmp || !*tmp)
        tmp = ".";
    snprintf(dir, sizeof(dir), "%s/q2psx_inspect_saves", tmp);
    q2_save_set_dir(dir);
    printf("directory   %s\n\n", q2_save_dir());

    {
        int i;
        for (i = 0; i < Q2_SAVE_SLOTS; i++)
            q2_save_slot_delete(i);
    }

    if (q2_identify(d, &id) != Q2_OK) {
        fprintf(stderr, "cannot identify this disc\n");
        return 1;
    }

    if (!fixture_open(&f, d, map, 0))
        return 1;

    fixture_run(&f, 200);

    /*
     * Fire a few script records by hand.
     *
     * Walking forward for 200 ticks does not necessarily trip a trigger, and a
     * save whose event flags are all still their on-disc values would compare
     * equal even if the flags chunk were dropped entirely. Setting the runtime
     * bits is what makes "every script FLAG survives" a claim with teeth.
     */
    if (f.sim.events_ready) {
        u32 i;
        for (i = 0; i < f.sim.event_rt.record_count && i < 3; i++)
            q2_event_rt_trigger(&f.sim.event_rt, f.sim.event_rt.offsets[i]);
        q2_event_rt_update(&f.sim.event_rt);
    }
    /* And a volume the player is standing in, for the same reason. */
    if (f.sim.trigger_inside && f.sim.trigger_capacity > 1)
        f.sim.trigger_inside[1] = 1;

    /* A played state: something in the inventory that a spawn does not have. */
    q2_sim_give_weapon(&f.sim, 4);
    q2_inventory_add_ammo(&f.sim.combat.inv, Q2_AMMO_BULLETS, 50);
    q2_inventory_give_key(&f.sim.combat.inv, Q2_KEY_BLUE);
    f.sim.combat.inv.health = 63;

    q2_mission_init(&mission);
    mission.unit = 1;
    q2_mission_set_row(&mission, 0, map, 1, 4, 7, 19);

    if (q2_save_capture(&captured, &f.sim, NULL, id.serial, map, 0) != Q2_OK) {
        fprintf(stderr, "cannot capture\n");
        fixture_close(&f);
        return 1;
    }
    q2_save_capture_mission(&captured, &mission);

    q2_save_slot_path(0, path, (u32)sizeof(path));
    if (q2_save_slot_write(&captured, 0) != Q2_OK) {
        fprintf(stderr, "cannot write %s\n", path);
        q2_save_free(&captured);
        fixture_close(&f);
        return 1;
    }

    report_capture(&captured, &f.sim, path);

    if (q2_save_slot_read(&reloaded, 0) != Q2_OK) {
        fprintf(stderr, "cannot read %s back\n", path);
        q2_save_free(&captured);
        fixture_close(&f);
        return 1;
    }

    compare(&captured, &reloaded);

    /* And it goes back into a live simulation. The zone is the same one, so
     * the entity set matches and the whole thing applies. */
    {
        fixture g;
        q2_result rc;

        printf("\napplied to a fresh simulation\n");
        if (fixture_open(&g, d, map, 0)) {
            rc = q2_save_apply(&reloaded, &g.sim, NULL, id.serial, map);
            ok(rc == Q2_OK, "the save applies to the same map");
            if (rc == Q2_OK) {
                ok_eq(g.sim.player[0].pos[0], captured.player.pos[0],
                      "the player is back where they were");
                ok_eq(g.sim.level_time, captured.level_time,
                      "the clock is back where it was");
                ok_eq(g.sim.combat.inv.health, 63, "and so is their health");
                ok_eq(count_collected(&g.sim), count_collected(&f.sim),
                      "the same items are still collected");
            }
            fixture_close(&g);
        }
    }

    /* A second slot, so the listing has something to show. */
    q2_save_slot_write(&captured, 2);
    in_use = q2_save_slots_scan(info, Q2_SAVE_SLOTS);
    printf("\nslots (%u in use)\n", in_use);
    {
        int i;
        char row[64];
        for (i = 0; i < Q2_SAVE_SLOTS; i++) {
            q2_save_slot_row(&info[i], i, row, (u32)sizeof(row));
            printf("  %d  %-24s %s\n", i + 1,
                   row[0] ? row : "(empty — the row draws nothing)",
                   info[i].used ? info[i].map : "");
        }
    }

    if (out && !shooter_open(&sh, d, map))
        memset(&sh, 0, sizeof(sh));
    else if (!out)
        memset(&sh, 0, sizeof(sh));

    q2_save_ui_init(&ui);
    drive(&ui, &captured, &sh, out, true, 2);      /* an occupied slot */
    drive(&ui, &captured, &sh, out, false, 0);

    if (q2_save_ui_take_loaded(&ui, &taken)) {
        ok(strcmp(taken.map, captured.map) == 0,
           "what the front end loaded is what was saved");
        q2_save_free(&taken);
    }

    q2_save_ui_free(&ui);
    shooter_close(&sh);

    {
        int i;
        for (i = 0; i < Q2_SAVE_SLOTS; i++)
            q2_save_slot_delete(i);
    }

    q2_save_free(&captured);
    q2_save_free(&reloaded);
    fixture_close(&f);

    printf("\n%s\n", g_bad == 0 ? "every field survived"
                                : "SOMETHING DID NOT SURVIVE");
    return g_bad == 0 ? 0 : 1;
}
