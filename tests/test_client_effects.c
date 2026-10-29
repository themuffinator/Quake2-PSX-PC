/* The real client effect callers, plus optional disc-backed framebuffer checks.
 * No window, SDL input device or operating-system capture is used. */
int q2_test_effects_client_main(int argc, char **argv);
#define main q2_test_effects_client_main
#include "../src/client/main.c"
#undef main

static int failures, checks;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    failures++; } } while (0)

#include "client_fixture.h"

static void screen_feedback(void)
{
    client *c = calloc(1, sizeof(*c));
    s32 feet[3] = {0, 0, 0};
    q2_screen_flash *flash;
    q2_inventory loadout;
    int pi, k;
    if (!c) exit(2);
    c->headless = c->sim_enabled = c->mp_enabled = c->hud_ready = true;
    c->pad_frame = -2;
    q2_sim_init(&c->sim[0], NULL, 50);
    loadout = c->sim[0].combat.inv;
    q2_mp_session_init(&c->mp, Q2_MP_DEATHMATCH, 4);
    CHECK(q2_screen_init(&c->screen, Q2_VIDEO_PAL) == Q2_OK);
    for (pi = 0; pi < 4; pi++) {
        feet[0] = pi * 2000;
        q2_sim_select_player(&c->sim[0], pi);
        q2_sim_spawn(&c->sim[0], feet, 0);
        q2_sim_player_loadout(&c->sim[0], pi, &loadout, 1);
        q2_hud_init(&c->hud[pi], NULL, 4);
        q2_hud_track(&c->hud[pi], c->sim[0].combat.inv.health, 0);
        c->sim_ready[pi] = pi != 0;
        q2_player_death_init(&c->death[pi]);
    }
    q2_sim_select_player(&c->sim[0], 0);
    c->sim[0].pcombat[2].inv.health -= 10;
    client_input_simulated(c, 1.0f / 30.0f);
    CHECK(c->hud_flashes[2] == 1);
    CHECK(c->screen.view[2].flash.strength > 0);
    CHECK(c->screen.view[0].flash.strength == 0);
    CHECK(c->screen.view[1].flash.strength == 0);
    CHECK(c->screen.view[3].flash.strength == 0);
    c->frame_index++;
    c->sim[0].pcombat[1].self.armour_fade = Q2_ACTOR_FADE_POWER;
    c->sim[0].pcombat[3].self.armour_fade = Q2_ACTOR_FADE_ARMOUR;
    client_input_simulated(c, 1.0f / 30.0f);
    CHECK(c->power_flashes[1] == 1 && c->armour_flashes[3] == 1);
    CHECK(c->sim[0].pcombat[1].self.armour_fade == Q2_ACTOR_FADE_NONE);
    CHECK(c->screen.view[1].flash.strength == Q2_ACTOR_FADE_POWER_STRENGTH);
    CHECK(c->screen.view[3].flash.strength == Q2_ACTOR_FADE_ARMOUR_STRENGTH);
    flash = &c->screen.view[0].flash;
    for (k = 0; k < 4; k++) {
        q2_inventory *inv = &c->sim[0].combat.inv;
        inv->quad_until = inv->invuln_until = inv->enviro_until = inv->breather_until = 0;
        if (k == 0) inv->quad_until = 30000;
        if (k == 1) inv->invuln_until = 30000;
        if (k == 2) inv->enviro_until = 30000;
        if (k == 3) inv->breather_until = 30000;
        c->frame_index++;
        client_input_simulated(c, 1.0f / 30.0f);
        CHECK(flash->mode == Q2_HUD_POWERUP_MODE);
        CHECK(flash->strength == Q2_HUD_POWERUP_STRENGTH);
        CHECK(!memcmp(flash->rgb, q2_hud_flash_powerup_rgb[k], 3));
    }
    c->frame_index++;
    c->sim[0].combat.inv.health -= 10;
    client_input_simulated(c, 1.0f / 30.0f);
    CHECK(!memcmp(flash->rgb, q2_hud_flash_health_rgb, 3));
    q2_sim_free(&c->sim[0]);
    q2_screen_free(&c->screen);
    free(c);
}

static void evacuation_damage(void)
{
    q2_sim *s = calloc(1, sizeof(*s));
    q2_actor actors[4], *targets[6];
    s32 feet[3] = {0};
    int i;
    if (!s) exit(2);
    q2_sim_init(s, NULL, 50);
    for (i = 0; i < 2; i++) {
        q2_sim_select_player(s, i);
        q2_sim_spawn(s, feet, 0);
    }
    q2_sim_select_player(s, 0);
    q2_sim_player_loadout(s, 1, &s->combat.inv, s->combat.weapon_id);
    s->glint.scripted = s->glint.zone_active = s->glint.finished = true;
    s->glint.evacuation_left = 300;
    s->level_time = 1000;
    for (i = 0; i < 4; i++) {
        q2_actor_init(&actors[i]);
        actors[i].health = 100;
        actors[i].takedamage = Q2_DAMAGE_YES;
        targets[i] = &actors[i];
    }
    actors[0].entity_kind = 37;
    actors[1].entity_kind = 38;
    actors[2].entity_kind = 47;
    actors[3].entity_kind = 12;
    actors[3].health = 0;
    targets[4] = &s->combat.self;
    targets[5] = &s->pcombat[1].self;
    q2_sim_set_world_targets(s, targets, 6);
    q2_sim_glint_evacuation_tick(s, 299);
    CHECK(s->glint.evacuation_left == 1 && actors[0].health == 100);
    q2_sim_glint_evacuation_tick(s, 1);
    CHECK(s->combat.inv.health == 95 && s->pcombat[1].inv.health == 95);
    CHECK(actors[0].health == 95 && actors[2].health == 95);
    CHECK(actors[1].health == 100 && actors[3].health == 0);
    CHECK(s->glint.evacuation_left == 300);
    CHECK(s->ent_world.events.count == 2);
    CHECK(s->ent_world.events.e[0].sound == Q2_SND_BURN);
    s->level_time = 1300;
    q2_sim_glint_evacuation_tick(s, 300);
    CHECK(s->combat.inv.health == 95); /* normal acid throttle */
    CHECK(actors[0].health == 90);
    CHECK(s->ent_world.events.count == 2); /* throttled hits are silent */
    s->level_time = 1600;
    s->pcombat[1].inv.enviro_until = 3000;
    q2_sim_glint_evacuation_tick(s, 300);
    CHECK(s->combat.inv.health == 90 && s->pcombat[1].inv.health == 95);
    CHECK(actors[0].health == 85);
    s->player[0].ent.flags |= 0x4000;
    q2_sim_glint_evacuation_tick(s, 400);
    CHECK(s->glint.evacuation_left == 300 && actors[0].health == 85);
    s->player[0].ent.flags &= ~0x4000u;
    s->glint.zone_active = false;
    q2_sim_glint_evacuation_tick(s, 400);
    CHECK(s->glint.evacuation_left == 300);
    s->glint.zone_active = true;
    s->settling = true;
    q2_sim_glint_evacuation_tick(s, 400);
    CHECK(s->glint.evacuation_left == 300);
    s->settling = false;
    s->player[1].ent.flags |= 0x4000; /* only player ZERO gates the module */
    s->level_time = 2350;
    q2_sim_glint_evacuation_tick(s, 750);
    CHECK(s->glint.evacuation_left == -150 && actors[0].health == 80);
    CHECK(s->combat.inv.health == 85); /* one attempt, no catch-up loop */
    q2_sim_free(s);
    free(s);
}

static void photograph(client *c, const char *output, const char *name)
{
    char path[1024];
    CHECK(c->headless);
    q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
    client_frame(c);
    snprintf(path, sizeof(path), "%s/%s.ppm", output, name);
    CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);
}

static bool teleport_disabled(client *c)
{
    q2_event_rt *rt = &c->sim[0].event_rt;
    u32 i, j;
    for (i = 0; i < rt->events.dir_count; i++) {
        q2_event_dir_entry e;
        if (!q2_events_get_dir_entry(&rt->events, i, &e) ||
            strcmp(e.name, "GravTeleport")) continue;
        for (j = 0; j < rt->record_count; j++) {
            if (rt->offsets[j] == e.offset)
                return (rt->flags[j] & Q2_EVREC_DISABLED) != 0;
        }
    }
    CHECK(false); /* the real named record must be present */
    return false;
}

static u32 visible_cracks(client *c)
{
    q2_map_name_table names;
    u32 i, found = 0, visible = 0;
    CHECK(q2_map_names_parse(&names, &c->zone.zone) == Q2_OK);
    for (i = 0; i < names.count; i++) {
        q2_map_name n;
        if (!q2_map_name_get(&names, i, &n) || strncmp(n.name, "Crack", 5))
            continue;
        CHECK(n.id < c->node_hidden_count);
        if (n.id >= c->node_hidden_count) continue;
        found++;
        visible += !c->node_hidden[n.id];
    }
    CHECK(found == 22);
    return visible;
}

static void round_trip(client *c)
{
    c->carry_player = c->carry_same_map = true;
    CHECK(client_load_zone(c, "BIGGUN", 2));
    CHECK(!c->sim[0].glint.zone_active);
    c->carry_player = c->carry_same_map = true;
    CHECK(client_load_zone(c, "BIGGUN", 1));
    CHECK(c->sim[0].glint.zone_active);
}

static void disc_effects(const char *disc, const char *output)
{
    client *c = open_client(disc);
    q2_event_record rec;
    q2_fx_glint *g = &c->sim[0].glint;
    q2_camera camera;
    u32 i, trigger = 0, flashes = 0, required_keys = 0;
    int f;
    CHECK(client_load_zone(c, "BIGGUN", 1));
    CHECK(g->scripted && g->lightning_ready && g->zone_active);
    CHECK(g->spark_origin[0] == -16963 && g->spark_origin[2] == 15080);
    CHECK(teleport_disabled(c));
    CHECK(visible_cracks(c) == 0);
    camera = c->cam;
    camera.pos[0] = -16928; camera.pos[1] = 1580; camera.pos[2] = 10000;
    camera.yaw = camera.pitch = camera.roll = 0;
    for (i = 0; i < c->sim[0].event_rt.record_count; i++) {
        u32 j;
        if (!q2_events_record_at(&c->sim[0].events, c->sim[0].event_rt.offsets[i], &rec)) continue;
        for (j = 0; j < rec.n_items; j++) {
            q2_event_item item;
            q2_uf_call call;
            char key[13];
            if (q2_events_get_item(&c->sim[0].events, &rec, j, &item) &&
                q2_uf_decode_call(&call, &c->sim[0].userfuncs, &item) == Q2_OK &&
                call.prim == Q2_UF_MISEVENT && q2_uf_operand_name(&call, 0, key) &&
                !strcmp(key, "Destroy Grav")) {
                trigger = rec.offset;
            }
        }
    }
    CHECK(trigger != 0);
    if (q2_events_record_at(&c->sim[0].events, trigger, &rec)) {
        for (i = 0; i < rec.n_items; i++) {
            q2_event_item item;
            q2_uf_call call;
            if (q2_events_get_item(&c->sim[0].events, &rec, i, &item)) {
                if (q2_uf_decode_call(&call, &c->sim[0].userfuncs, &item) == Q2_OK &&
                    call.prim == Q2_UF_ONKEYDO) {
                    u32 any = 0;
                    q2_uf_operand_u32(&call, 0, 0, &required_keys);
                    q2_uf_operand_u32(&call, 1, 0, &any);
                    required_keys |= any;
                }
            }
        }
    }
    for (f = 0; f < 20; f++) {
        c->frame_index++;
        client_input_simulated(c, 1.0f / 30.0f);
        c->cam = camera;
        client_frame(c);
    }
    c->cam = camera;
    {
        u8 phase = g->band[0].phase;
        photograph(c, output, "generator-powered");
        CHECK(c->glint_drawn && g->band[0].phase == (u8)(phase - 1));
    }
    {
        const q2_hud_palette *pal = q2_hud_palette_get(&c->hud_tables, 77);
        printf("Lightning: origin %d %d %d, palette %04X:",
               g->origin[0], g->origin[1], g->origin[2], pal->clut_id);
        for (i = 0; i < 16; i++) printf(" %04X", pal->entry[i]);
        puts("");
        c->cam.pos[0] = -17800; c->cam.pos[1] = 1400; c->cam.pos[2] = 15800;
        c->cam.yaw = 1900;
        {
            u8 phase = g->band[0].phase;
            photograph(c, output, "generator-reference");
            CHECK(c->glint_drawn && g->band[0].phase == (u8)(phase - 1));
        }
        c->cam = camera;
    }
    /* The real damage path and the real client-to-screen drain. */
    q2_sim_hurt_player(&c->sim[0], NULL, 15, Q2_MOD_BULLET, NULL);
    c->frame_index++;
    client_input_simulated(c, 1.0f / 30.0f);
    CHECK(c->hud_flashes[0] > 0);
    c->cam = camera;
    photograph(c, output, "screen-damage");
    c->screen.view[0].flash.strength = 0;
    c->sim[0].combat.inv.quad_until = c->sim[0].level_time + 30000;
    c->frame_index++;
    client_input_simulated(c, 1.0f / 30.0f);
    CHECK(!memcmp(c->screen.view[0].flash.rgb, q2_hud_flash_powerup_rgb[0], 3));
    c->cam = camera;
    photograph(c, output, "screen-quad");
    c->sim[0].combat.inv.quad_until = 0;
    c->screen.view[0].flash.strength = 0;
    /* Publish the owner field the underwater volume dispatcher writes. */
    c->sim[0].player[0].ent.flags |= Q2_ENT_UNDERWATER;
    for (i = 0; i < 18; i++) {
        c->cam = camera;
        q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
        client_frame(c);
    }
    CHECK(c->screen.view[0].water.amp == 4096);
    c->cam = camera;
    photograph(c, output, "screen-underwater");
    c->sim[0].player[0].ent.flags &= ~Q2_ENT_UNDERWATER;
    for (i = 0; i < 18; i++) {
        c->cam = camera;
        q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
        client_frame(c);
    }
    CHECK(c->screen.view[0].water.amp == 0);
    CHECK(c->lightning_prims > 0 && c->glint_prims > 0);
    {
        u32 before = c->glint_prims;
        for (i = 0; i < g->band_count; i++) g->band[i].phase = 0;
        c->cam = camera;
        c->frame_index++;
        client_frame(c);
        CHECK(c->glint_drawn && c->glint_prims == before);
        CHECK(g->band[0].phase == 255); /* a black draw still consumes phase */

        for (i = 0; i < g->band_count; i++) g->band[i].phase = 4;
        q2_screen_set_layout(&c->screen, Q2_SCREEN_LAYOUT_TWO_H, 2);
        c->cam = camera;
        c->frame_index++;
        client_frame(c);
        CHECK(c->screen.view_count == 2 && c->glint_drawn);
        CHECK(g->band[0].phase == 3); /* both viewports share one phase */
        q2_screen_set_layout(&c->screen, Q2_SCREEN_LAYOUT_ONE, 1);
    }
    {
        u32 gated = c->script_gated;
        CHECK(q2_event_rt_trigger(&c->sim[0].event_rt, trigger));
        /* PAL may need two rendered frames to reach a logic tick. Do not
         * mistake an undrained queue for a successful key refusal. */
        for (f = 0; f < 3 && c->sim[0].event_rt.pending_count; f++) {
            c->frame_index++;
            client_input_simulated(c, 1.0f / 30.0f);
        }
        CHECK(!g->shutting_down && c->script_gated > gated);
        CHECK(q2_event_rt_flags(&c->sim[0].event_rt, trigger) & Q2_EVREC_DISABLED);
    }
    /* Retail latches this one-shot record before its key predicate. The
     * successful case needs a fresh level, not a retry of the spent record. */
    CHECK(client_load_zone(c, "BIGGUN", 1));
    CHECK(required_keys != 0);
    c->sim[0].combat.inv.flags |= required_keys;
    CHECK(q2_event_rt_trigger(&c->sim[0].event_rt, trigger));
    for (f = 0; f < 3 && !g->shutting_down; f++) {
        c->frame_index++;
        client_input_simulated(c, 1.0f / 30.0f);
    }
    CHECK(g->shutting_down && !g->burst_done);
    {
        s32 power = g->power_left;
        round_trip(c);
        CHECK(g->shutting_down && g->power_left == power);
        CHECK(teleport_disabled(c)); /* re-entry must not open the exit early */
    }
    for (f = 0; f < 650 && !g->finished; f++) {
        c->frame_index++;
        client_input_simulated(c, 1.0f / 30.0f);
        if (c->generator_flashes > flashes) {
            flashes = c->generator_flashes;
            CHECK(!g->raised && g->dying);
            CHECK(c->screen.view[0].flash.strength == 10);
            c->cam = camera;
            photograph(c, output, "generator-flash");
        } else {
            c->cam = camera;
            client_frame(c);
        }
    }
    printf("Generator after %d frames: shutdown %d power %d dying %d death %d time %d\n",
           f, g->shutting_down, g->power_left, g->dying, g->death_time, c->sim[0].level_time);
    CHECK(g->shutting_down && g->finished && !g->raised);
    CHECK(c->generator_flashes == 1);
    CHECK(!teleport_disabled(c));
    CHECK(visible_cracks(c) == 22);
    {
        s32 hp = c->sim[0].combat.inv.health;
        s32 left = g->evacuation_left;
        u32 hurt_flashes = c->hud_flashes[0];
        /* Finished-owner tick already consumed dt; preserve that remainder. */
        CHECK(left > 0 && left < 300);
        for (f = 0; f < 29; f++) {
            c->frame_index++;
            client_input_simulated(c, 1.0f / 30.0f);
        }
        CHECK(c->sim[0].combat.inv.health == hp - 5);
        CHECK(c->hud_flashes[0] == hurt_flashes + 1);
        c->cam = camera;
        photograph(c, output, "evacuation-damage");
    }
    c->cam = camera;
    c->screen.view[0].flash.strength = 0;
    photograph(c, output, "generator-destroyed");
    round_trip(c);
    c->frame_index++;
    client_input_simulated(c, 1.0f / 30.0f);
    CHECK(g->finished && !g->owner_active && !g->raised);
    CHECK(c->generator_flashes == 1 && !teleport_disabled(c));
    for (i = 0; i < c->sim[0].entities.count; i++) {
        const q2_entity *e = &c->sim[0].entities.ent[i];
        CHECK(!(e->in_use && e->kind == Q2_FX_GLINT_TARGET_KIND &&
                e->place_id == Q2_FX_GLINT_TARGET_ID));
    }
    close_client(c);
}

static void disc_hurt_view(const char *disc, const char *output)
{
    client *c = open_client(disc);
    s32 hp, view[3];
    int f;
    CHECK(client_load_zone(c, "BASE1", 0));
    for (f = 0; f < 60; f++) {
        c->frame_index++;
        client_input_simulated(c, 1.0f / 30.0f);
        client_frame(c);
    }
    hp = c->sim[0].combat.inv.health;
    q2_sim_hurt_player(&c->sim[0], NULL, 20, Q2_MOD_MELEE, NULL);
    for (f = 0; f < 4; f++) {
        c->frame_index++;
        client_input_simulated(c, 1.0f / 30.0f);
        if (c->sim[0].player[0].pain_time > c->sim[0].level_time) break;
        client_frame(c);
    }
    CHECK(c->sim[0].player[0].pain_time > c->sim[0].level_time);
    photograph(c, output, "damage-view");
    CHECK(c->sim[0].combat.inv.health == hp - 20);
    CHECK(c->hud_flashes[0] > 0);
    q2_sim_view_angles(&c->sim[0], view);
    printf("After melee: health %d, stored hurt pitch/roll %d/%d, view %d/%d/%d\n",
           c->sim[0].combat.inv.health, c->sim[0].player[0].hurt_kick[0],
           c->sim[0].player[0].hurt_kick[1], view[0], view[1], view[2]);
    CHECK(c->sim[0].player[0].hurt_kick[0] == 0 && c->sim[0].player[0].hurt_kick[1] == 0);
    close_client(c);
}

static void laser_record(client *c, u32 raiser, bool enabled)
{
    q2_event_rt *rt = &c->sim[0].event_rt;
    u32 i;
    for (i = 0; i < rt->record_count; i++) if (rt->offsets[i] == raiser) {
        if (enabled) rt->flags[i] &= ~Q2_EVREC_DISABLED;
        else rt->flags[i] |= Q2_EVREC_DISABLED;
        return;
    }
    CHECK(false);
}

static void laser_player(client *c, const q2_laserbeam *beam, s16 health)
{
    int k;
    for (k = 0; k < 3; k++)
        c->sim[0].player[0].pos[k] = (beam->from[k] + beam->to[k]) / 2;
    c->sim[0].player[0].pos[1] += Q2_EYE_BASE;
    c->sim[0].combat.inv.health = health;
    c->sim[0].combat.inv.armour = 0;
    c->sim[0].combat.inv.flags = 0;
    c->sim[0].combat.inv.invuln_until = 0;
}

static client *laser_cold_save(client *c, const char *disc, const char *output)
{
    q2_save loaded = {0};
    char path[1024];
    snprintf(path, sizeof(path), "%s/laser-resume.q2s", output);
    CHECK(client_capture(c));
    CHECK(q2_save_write(&c->snapshot, path) == Q2_OK);
    CHECK(q2_save_read(&loaded, path) == Q2_OK);
    CHECK(remove(path) == 0);
    close_client(c);
    c = open_client(disc);
    CHECK(client_apply_save(c, &loaded));
    q2_save_free(&loaded);
    return c;
}

static void disc_static_lasers(const char *disc, const char *output)
{
    static const char *maps[] = {"POWER1", "POWER2", "JAIL2", "JAIL3", "COMMAND", "SECURITY"};
    static const int zones[] = {3,5,3,6,5,3};
    client *c = open_client(disc);
    u32 mi, i;
    int z;
    c->icons_ready = q2_icon_tables_load(&c->icons, c->disc, &c->build) == Q2_OK;
    CHECK(c->icons_ready);
    q2_statusbar_init(&c->sbar[0], &c->icons, 1);
    q2_statusbar_set_palettes(&c->sbar[0], &c->hud_tables);
    for (mi = 0; mi < Q2PSX_ARRAY_COUNT(maps); mi++) {
        u32 resident = 0;
        for (z = 0; z < zones[mi]; z++) {
            q2_laserbeam_set authored;
            CHECK(client_load_zone(c, maps[mi], z));
            authored = c->lasers;
            resident += authored.count;
            q2_sim_set_targets(&c->sim[0], NULL, 0);
            for (i = 0; i < authored.count; i++) {
                static const s16 damage[6] = {512,1,512,1,512,512};
                static const s16 mod[6] = {11,16,12,16,11,12};
                const q2_laserbeam *b = &authored.beam[i];
                q2_rng rng = c->sim[0].fx_rng;
                c->lasers.count = 1; c->lasers.beam[0] = *b;
                CHECK(b->kind >= 0 && b->kind < 6);
                if (b->kind < 0 || b->kind >= 6) continue;
                laser_record(c, b->raiser, true);
                laser_player(c, b, 2000);
                q2_fx_beams_reset(&c->sim[0].fx);
                client_laserbeams_frame(c, false);
                CHECK(c->laser_drawn == 1 && c->sim[0].combat.inv.health == 2000);
                CHECK(!memcmp(&rng, &c->sim[0].fx_rng, sizeof(rng)));
                client_laserbeams_frame(c, true);
                CHECK(c->sim[0].combat.inv.health == 2000 - damage[b->kind]);
                CHECK(c->sim[0].combat.self.last_mod == mod[b->kind]);
                c->no_lasers = true;
                q2_fx_beams_reset(&c->sim[0].fx);
                client_laserbeams_frame(c, true);
                CHECK(!c->laser_drawn && !c->sim[0].fx.beam_count);
                CHECK(c->sim[0].combat.inv.health == 2000 - 2*damage[b->kind]);
                c->no_lasers = false;
                /* Packet exhaustion must not protect an actor. */
                c->sim[0].fx.beam_count = Q2_FX_BEAMS_MAX;
                client_laserbeams_frame(c, true);
                CHECK(!c->laser_drawn && c->sim[0].combat.inv.health == 2000 - 3*damage[b->kind]);
                q2_fx_beams_reset(&c->sim[0].fx);
                laser_record(c, b->raiser, false);
                client_laserbeams_frame(c, true);
                CHECK(!c->laser_drawn && !c->sim[0].fx.beam_count &&
                      c->sim[0].combat.inv.health == 2000 - 3*damage[b->kind]);
                laser_record(c, b->raiser, true);
            }
            c->lasers = authored;
        }
        printf("Static lasers: %s, %u resident segments checked\n", maps[mi], resident);
    }
    CHECK(client_load_zone(c, "JAIL2", 1));
    CHECK(c->lasers.count > 0);
    if (c->lasers.count) {
        const q2_laserbeam beam = c->lasers.beam[0];
        s32 feet[3];
        int k;
        c->lasers.count = 1;
        for (k = 0; k < 3; k++) feet[k] = (beam.from[k] + beam.to[k]) / 2;
        feet[1] += Q2_EYE_BASE;
        q2_sim_spawn(&c->sim[0], feet, 0);
        c->sim[0].combat.inv.health = 600;
        c->sim[0].combat.inv.armour = 0;
        c->sim[0].combat.inv.flags = 0;
        for (k = 0; k < 2; k++) {
            c->frame_index++;
            client_input_simulated(c, 1.0f / 30.0f);
            if (k == 0) client_frame(c);
        }
        printf("Static laser contact: health %d, logic ticks %d, drawn %u\n",
            c->sim[0].combat.inv.health, c->sim[0].cur_dt, c->laser_drawn);
        CHECK(c->sim[0].combat.inv.health == 88);
        photograph(c, output, "static-laser-hit");
        /* A creature in front absorbs the hit. Drain its combat result now,
         * so the next actor refresh cannot restore its old health. */
        for (i = 0; i < c->creatures.set.count; i++) {
            q2_monster *m = &c->creatures.set.monsters[i];
            u32 pain = c->cre_pain_calls;
            s16 hp;
            if (m->dead || !m->takedamage || !m->motion.ready) continue;
            for (k = 0; k < 3; k++)
                m->motion.ent.pos[k] = beam.from[k] + (beam.to[k] - beam.from[k]) / 4;
            m->health = 2000;
            q2_actor_from_monster(&c->cre_actor[i], m);
            memset(c->cre_target, 0, c->creatures.set.count * sizeof(*c->cre_target));
            c->cre_target[i] = &c->cre_actor[i];
            q2_sim_set_targets(&c->sim[0], c->cre_target, c->creatures.set.count);
            laser_player(c, &beam, 600);
            client_laserbeams_frame(c, true);
            CHECK(m->health < 2000 && c->sim[0].combat.inv.health == 600);
            CHECK(m->health == c->cre_actor[i].health);
            CHECK(c->cre_actor[i].last_mod == 11);
            CHECK(!c->cre_actor[i].damage_reaction_pending);
            CHECK(c->cre_pain_calls == pain + 1);
            hp = m->health;
            q2_actor_from_monster(&c->cre_actor[i], m);
            CHECK(c->cre_actor[i].health == hp);
            break;
        }
        CHECK(i < c->creatures.set.count);
    }
    close_client(c);

    /* The authored COMMAND switch shuts off both rendering and damage,
     * including a cold disk save and a departure/return. */
    c = open_client(disc);
    CHECK(client_load_zone(c, "COMMAND", 1));
    CHECK(c->lasers.count == 2);
    if (c->lasers.count) {
        q2_laserbeam beam = c->lasers.beam[0];
        CHECK(q2_event_rt_trigger_named(&c->sim[0].event_rt, "LasersOff"));
        CHECK(q2_event_rt_update(&c->sim[0].event_rt) == Q2_EVENT_OK);
        CHECK(q2_event_rt_flags(&c->sim[0].event_rt, beam.raiser) & Q2_EVREC_DISABLED);
        c = laser_cold_save(c, disc, output);
        CHECK(c->lasers.count == 2);
        laser_player(c, &beam, 2000);
        q2_fx_beams_reset(&c->sim[0].fx);
        client_laserbeams_frame(c, true);
        CHECK(!c->laser_drawn && c->sim[0].combat.inv.health == 2000);
        c->carry_player = c->carry_same_map = true;
        CHECK(client_load_zone(c, "COMMAND", 0));
        CHECK(!c->lasers.count);
        c->carry_player = c->carry_same_map = true;
        CHECK(client_load_zone(c, "COMMAND", 1));
        CHECK(q2_event_rt_flags(&c->sim[0].event_rt, beam.raiser) & Q2_EVREC_DISABLED);
        client_laserbeams_frame(c, true);
        CHECK(!c->laser_drawn);
    }
    close_client(c);

    /* The native POWER2 clock runs after the shared beam walk. At the first
     * callback the bank still draws, then its disabled bit gates the next. */
    c = open_client(disc);
    CHECK(client_load_zone(c, "POWER2", 4));
    CHECK(c->lasers.count == 10);
    c->sim[0].cur_dt = 10;
    c->sim[0].level_time = 0;
    client_level_effects_frame(c, true);
    CHECK(c->laser_drawn == 10);
    q2_fx_beams_reset(&c->sim[0].fx);
    client_level_effects_frame(c, false);
    CHECK(!c->laser_drawn);
    c->sim[0].level_time = 47;
    client_level_effects_frame(c, true);
    CHECK(!c->laser_drawn);
    client_level_effects_frame(c, false);
    CHECK(c->laser_drawn == 6);
    CHECK(client_load_zone(c, "QFMV", 0));
    CHECK(!c->lasers.count && !c->laserwalls.count);
    close_client(c);
}

static void particle_pool_picture(client *c, const char *output)
{
    q2_camera cam;
    q2_camera saved_camera = c->cam;
    gte_state gte;
    psx_ot ot;
    psx_framebuffer *fb = q2_screen_back(&c->screen);
    s16 velocity[1][3] = {{0}};
    char path[1024];
    u32 i, made = 0;
    CHECK(c->headless);
    q2_fx_world_clear(&c->sim[0].fx);
    q2_camera_default(&cam, 512, 248);
    memset(cam.pos, 0, sizeof(cam.pos));
    cam.yaw = cam.pitch = cam.roll = 0;
    cam.projection = 256;
    CHECK(psx_ot_init(&ot, 256, 4096) == Q2_OK);
    gte_init(&gte);
    gte_set_projection(&gte, 256, 256, 124);
    for (i = 0; i < 64; i++) {
        s32 at[3] = {(s32)(i % 8) * 128 - 448, (s32)(i / 8) * 96 - 336, 1024};
        made += q2_fx_group_spawn(&c->sim[0].fx, at, velocity, 1,
                    q2_fx_ramp_by_id(&c->fx_tables, 18), NULL, 32, 16384, 0) >= 0;
    }
    psx_fb_clear(fb, 0);
    psx_ot_clear(&ot);
    CHECK(q2_fx_build_ot(&c->sim[0].fx, &cam, 0, &ot, &gte) == made);
    psx_raster_ot(fb, &ot, c->vram, &c->opts);
    snprintf(path, sizeof(path), "%s/particle-pool.ppm", output);
    q2_screen_present(&c->screen);
    c->cam = cam;
    c->shot_path = path;
    client_write_shot(c, false); /* the --shot command's engine capture path */
    c->shot_path = NULL;
    c->cam = saved_camera;
    CHECK(c->shots_written == 1);
    printf("particle pool image: %u of 64 groups\n", made);
    CHECK(made == 64);
    psx_ot_free(&ot);
}

static void disc_particle_pools(const char *disc_path, const char *output)
{
    client *c = open_client(disc_path);
    int file;
    u32 maps = 0, records = 0, loaded = 0, eighty = 0;
    for (file = 0; file < disc_file_count(c->disc); file++) {
        const disc_file *f = disc_file_at(c->disc, file);
        const char *path = f->path + (f->path[0] == '/');
        const char *end = strstr(path, "/COMMON.DAT");
        q2_common_file common = {0};
        q2_buf buf = {0};
        const dat_chunk *chunk;
        char map[64];
        u32 at;
        if (!end || strncmp(path, "Q2DATA/LEVELS/", 14)) continue;
        if ((size_t)(end - path - 14) >= sizeof(map)) continue;
        memcpy(map, path + 14, (size_t)(end - path - 14));
        map[end - path - 14] = 0;
        CHECK(disc_read_file(c->disc, f->path, &buf) == Q2_OK);
        CHECK(q2_common_open(&common, &buf) == Q2_OK);
        chunk = common.chunk[Q2_COMMON_RESOURCES];
        CHECK(chunk != NULL);
        if (!chunk) { q2_common_close(&common); continue; }
        maps++;
        for (at = 0; at + 32 <= chunk->size && chunk->data[at]; at += 32) {
            const u8 *raw = chunk->data + at;
            char zone[13], zone_path[128];
            client *loaded_client;
            s32 groups = q2_rd_s32(raw + 24);
            q2_zone_resources decoded;
            u32 made = 0, i;
            s32 point[3] = {0, 0, 1024};
            s16 velocity[1][3] = {{0}};
            memcpy(zone, raw, 12); zone[12] = 0;
            CHECK(q2_common_resources(&common, zone, &decoded));
            CHECK(decoded.particle_groups == groups &&
                  decoded.packet_control == q2_rd_s32(raw + 12) &&
                  decoded.subdiv_meshes == q2_rd_s32(raw + 28));
            records++;
            eighty += groups == 80;
            printf("resource %s %s %d %d %d\n", map, zone,
                    q2_rd_s32(raw + 12), groups, q2_rd_s32(raw + 28));
            CHECK(groups == 64 || groups == 80);
            snprintf(zone_path, sizeof(zone_path), "Q2DATA/LEVELS/%s/%s.DAT", map, zone);
            if (!disc_find(c->disc, zone_path)) continue;
            /* Each archive is an independent startup, not an invented campaign
             * transition through front-end/movie maps with gameplay history. */
            loaded_client = open_client(disc_path);
            CHECK(client_load_zone(loaded_client, map, atoi(zone + 4)));
            CHECK(loaded_client->level_memory_current && !loaded_client->level_memory_failures);
            CHECK(32u*loaded_client->render.packet_capacity == loaded_client->level_memory.packets);
            loaded++;
            if (decoded.packet_control < 0) {
                /* Both shipped forced counts are covered by native-machine
                 * vectors in test_subdiv_pool: QFRONT -3000 and QFMV -1. */
                u32 packets = 0u - (u32)decoded.packet_control;
                CHECK(loaded_client->render.packet_capacity == (packets + 31u) / 32u);
            }
            CHECK(loaded_client->sim[0].fx.group_count == (u32)groups);
            q2_fx_world_clear(&loaded_client->sim[0].fx);
            for (i = 0; i < (u32)groups + 1; i++)
                made += q2_fx_group_spawn(&loaded_client->sim[0].fx, point, velocity, 1,
                            q2_fx_ramp_by_id(&loaded_client->fx_tables, 18), NULL, 32, 16384, 0) >= 0;
            CHECK(made == (u32)groups);
            close_client(loaded_client);
        }
        q2_common_close(&common);
    }
    CHECK(maps == 49 && loaded == 115);
    printf("particle resource census: %u maps, %u records, %u zones loaded, %u records use 80\n",
            maps, records, loaded, eighty);
    CHECK(client_load_zone(c, "BASE1", 0));
    particle_pool_picture(c, output);
    q2_screen_set_layout(&c->screen, Q2_SCREEN_LAYOUT_QUAD, 4);
    client_frame(c);
    CHECK(c->sim[0].fx.group_count == 64 && c->sim[0].fx.budget == 1920);
    q2_screen_set_layout(&c->screen, Q2_SCREEN_LAYOUT_ONE, 1);
    client_frame(c);
    CHECK(c->sim[0].fx.group_count == 64 && c->sim[0].fx.budget == 960);
    close_client(c);
}

int main(int argc, char **argv)
{
    if (argc == 1) { screen_feedback(); evacuation_damage(); }
    else if (argc == 5 && !strcmp(argv[1], "--headless") && !strcmp(argv[4], "--lasers"))
        disc_static_lasers(argv[2], argv[3]);
    else if (argc == 5 && !strcmp(argv[1], "--headless") && !strcmp(argv[4], "--hurt"))
        disc_hurt_view(argv[2], argv[3]);
    else if (argc == 5 && !strcmp(argv[1], "--headless") && !strcmp(argv[4], "--pools"))
        disc_particle_pools(argv[2], argv[3]);
    else if (argc == 4 && !strcmp(argv[1], "--headless"))
        disc_effects(argv[2], argv[3]);
    else {
        fprintf(stderr, "usage: test_client_effects [--headless DISC OUTPUT_DIR [--hurt|--lasers|--pools]]\n");
        return 2;
    }
    printf("%d client effect checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

