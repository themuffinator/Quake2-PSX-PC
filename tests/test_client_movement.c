/* Movement through the client/sim boundary. Optional disc coverage writes
 * actual engine framebuffer captures without opening a window or input device. */
int q2_test_movement_client_main(int argc, char **argv);
#define main q2_test_movement_client_main
#include "../src/client/main.c"
#undef main

static int checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    failures++; } } while (0)

static void test_corpse_physics(void)
{
    static const s32 steps[] = { 1, 6, 10, 12, 20, 30 };
    q2_input input = {0};
    const s32 spawn[3] = {0, 0, 0};
    u32 i;

    input.forward = input.side = input.pitch = input.yaw = Q2_PAD_FULL;
    input.buttons = Q2_BTN_JUMP | Q2_BTN_SWIM_UP | Q2_BTN_MOVING;
    for (i = 0; i < Q2PSX_ARRAY_COUNT(steps); i++) {
        q2_sim sim;
        q2_player *p = &sim.player[0];
        const s32 dt = steps[i];
        const s32 slow_y = 960 > dt * 40 ? 960 - dt * 40 : 0;

        q2_sim_init(&sim, NULL, 50);
        q2_sim_spawn(&sim, spawn, 0);
        p->ground_y = 100000;
        p->ent2_flags = Q2_ENT2_DEAD;
        sim.combat.inv.health = p->prev_health = 0;
        p->view_height = 400;
        p->yaw = 200;
        p->pitch = 60;
        p->vel[0] = 1200;
        p->vel[1] = 960;
        p->vel[2] = -240;
        p->frame_delta[0] = 45;
        p->frame_delta[1] = 36;
        p->frame_delta[2] = -9;
        q2_sim_tick(&sim, &input, dt);

        /* 0x800395A0..0x800395F8: (dt * 5) << 3, on ALL axes,
         * before the shared mover's gravity and next-frame integration. */
        CHECK(p->vel[0] == (1200 > dt * 40 ? 1200 - dt * 40 : 0));
        CHECK(p->vel[1] == slow_y + dt * 32);
        CHECK(p->vel[2] == (-240 < -dt * 40 ? -240 + dt * 40 : 0));
        CHECK(p->frame_delta[1] == (slow_y * dt + 16 * dt * dt) / 320);
        CHECK(p->pos[0] == 45 && p->pos[1] == 36 && p->pos[2] == -9);
        CHECK(p->view_height == 400 && p->yaw == 200 && p->pitch == 60);
        CHECK(p->wish[0] == 0 && p->wish[2] == 0 && p->jump_hold == 0);
        q2_sim_free(&sim);
    }
}

static void test_motion_stages(void)
{
    client *c = calloc(1, sizeof(*c));
    q2_sim *sim;
    q2_input input = {0};
    const s32 spawn[3] = {0, 0, 0};
    q2_player *p;
    int pi;

    if (!c) exit(2);
    c->headless = true;
    sim = &c->sim[0];
    q2_sim_init(sim, NULL, 50);
    sim->player_count = 4;
    for (pi = 0; pi < 4; pi++) {
        q2_player_death_init(&c->death[pi]);
        q2_sim_select_player(sim, pi);
        q2_sim_spawn(sim, spawn, 0);
        q2_player_die(&c->death[pi], -1, Q2_MOD_FALLING, (s16)pi, true, false, NULL);
        sim->combat.inv.health = 0;
    }
    q2_sim_select_player(sim, 0);
    p = &sim->player[0];
    c->death[1].stage = Q2_PDEATH_DOWN;
    c->death[2].stage = Q2_PDEATH_FADING;
    c->death[3].stage = Q2_PDEATH_DISSOLVING;
    client_player_motion(c);
    CHECK(p->motion == Q2_PLAYER_MOTION_CORPSE);
    CHECK(sim->player[1].motion == Q2_PLAYER_MOTION_BODY);
    CHECK(sim->player[2].motion == Q2_PLAYER_MOTION_NONE);
    CHECK(sim->player[3].motion == Q2_PLAYER_MOTION_NONE);
    CHECK(p->corpse_separate && !sim->player[1].corpse_separate);
    c->death[0].ent2 |= Q2_PDEATH_DEAD_BIT;
    client_player_motion(c);
    CHECK(!p->corpse_separate);

    /* The single-player fly setting is read by the live think only. A
     * corpse must keep its momentum even when that setting is enabled. */
    sim->full_basis_movement = true;
    sim->env_flags = Q2_ENT_INLOWCROUCH | Q2_ENT_UNDERWATER;
    p->ground_y = 100000;
    p->vel[0] = 1200;
    p->vel[1] = -960;
    q2_sim_tick(sim, &input, 12);
    CHECK(p->vel[0] == 720 && p->vel[1] == -96);
    CHECK(!(p->ent2_flags & Q2_ENT2_FLY));
    CHECK(!(p->ent.flags & (Q2_ENT_INLOWCROUCH | Q2_ENT_UNDERWATER)));
    CHECK(sim->bodies[0].solid && !sim->bodies[1].solid &&
          !sim->bodies[2].solid && !sim->bodies[3].solid);

    /* respawn_think calls the shared mover without corpse braking. */
    q2_sim_select_player(sim, 1);
    p = &sim->player[1];
    p->ground_y = 100000;
    p->vel[0] = 1200;
    p->vel[1] = 960;
    q2_sim_tick(sim, &input, 12);
    CHECK(p->vel[0] == 1200 && p->vel[1] == 1344);
    CHECK(sim->level_time == 12); /* extra players do not advance the clock */

    /* Gib checks precede movement. Dissolve's first DOWN tick is the one
     * exception: its gate bypasses the gib test and still calls the mover. */
    sim->combat.inv.health = Q2_PDEATH_GIB_HEALTH;
    client_player_motion(c);
    CHECK(p->motion == Q2_PLAYER_MOTION_NONE);
    sim->combat.self.effect[0] = 1;
    CHECK(q2_corpse_dissolve_pick(sim->combat.self.effect) != Q2_CORPSE_DISSOLVE_NONE);
    client_player_motion(c);
    CHECK(p->motion == Q2_PLAYER_MOTION_BODY);

    q2_sim_select_player(sim, 0);
    p = &sim->player[0];
    sim->combat.inv.health = Q2_PDEATH_GIB_HEALTH;
    client_player_motion(c);
    CHECK(p->motion == Q2_PLAYER_MOTION_NONE);
    {
        s32 pos[3], vel[3];
        memcpy(pos, p->pos, sizeof(pos));
        memcpy(vel, p->vel, sizeof(vel));
        q2_sim_tick(sim, &input, 12);
        CHECK(!memcmp(pos, p->pos, sizeof(pos)) && !memcmp(vel, p->vel, sizeof(vel)));
        CHECK(sim->level_time == 24); /* the world still advances */
    }
    c->death[0].stage = Q2_PDEATH_GIBBED;
    c->death[1].stage = Q2_PDEATH_GONE;
    client_player_motion(c);
    CHECK(sim->player[0].motion == Q2_PLAYER_MOTION_NONE &&
          sim->player[1].motion == Q2_PLAYER_MOTION_NONE);

    q2_sim_spawn(sim, spawn, 0);
    CHECK(p->motion == Q2_PLAYER_MOTION_LIVE && !p->corpse_separate);
    q2_sim_free(sim);
    free(c);
}

static void test_dead_trigger_contacts(void)
{
    static const u8 raw[] = {
        2, 0, 0, 0, 0, 0, 0, 0, /* count and directory terminator */
        4, 0, 0, Q2_EVREC_CAT_B,  /* @8: run while touching */
        4, 0, 0, 0               /* @12: explicitly queued script */
    };
    u8 bounds[2 * Q2_TRIGGER_SIZE] = {0};
    dat_chunk chunk = {0};
    q2_common_file common = {0};
    q2_sim sim;
    q2_input input = {0};
    const s32 spawn[3] = {0, 0, 0};
    int axis;

    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    chunk.data = raw;
    chunk.size = sizeof(raw);
    common.chunk[Q2_COMMON_EVENTS] = &chunk;
    CHECK(q2_events_parse_common(&sim.events, &common) == Q2_OK);
    CHECK(q2_event_rt_init(&sim.event_rt, &sim.events) == Q2_OK);
    sim.events_ready = true;
    for (axis = 0; axis < 3; axis++) {
        q2_wr_u32(bounds + axis * 4, (u32)-1000);
        q2_wr_u32(bounds + 12 + axis * 4, 1000);
    }
    q2_wr_u16(bounds + 0x1A, 8);
    q2_wr_u16(bounds + 0x20, Q2_TRIGGER_NO_ID);
    q2_wr_u16(bounds + 0x22, 1);
    sim.triggers.data = bounds;
    sim.triggers.count = sim.trigger_capacity = 1;
    sim.triggers_ready = true;
    sim.trigger_inside = calloc(1, 1);
    if (!sim.trigger_inside) exit(2);

    sim.player[0].ent2_flags = Q2_ENT2_DEAD;
    sim.combat.inv.health = sim.player[0].prev_health = 0;
    q2_sim_tick(&sim, &input, 12);
    CHECK(sim.event_rt.ran_count == 0 && sim.trigger_inside[0] == 0);
    CHECK(q2_event_rt_trigger(&sim.event_rt, 12));
    q2_sim_tick(&sim, &input, 12);
    CHECK(sim.event_rt.ran_count == 1 && sim.trigger_inside[0] == 0);

    q2_sim_spawn(&sim, spawn, 0);
    sim.combat.inv.health = sim.player[0].prev_health = 100;
    q2_sim_tick(&sim, &input, 12);
    CHECK(sim.event_rt.ran_count == 2 && sim.trigger_inside[0] == 1);
    q2_sim_free(&sim);
}

static void test_corpse_separation(q2_sim *sim)
{
    q2_player *p = &sim->player[0];
    q2_input input = {0};
    s32 base[3];
    int pass;

    memcpy(base, p->pos, sizeof(base));
    sim->player_count = 2;
    memcpy(sim->player[1].pos, base, sizeof(base));
    sim->player[1].pos[0] += 100;
    memcpy(sim->player[1].ent.pos, sim->player[1].pos, sizeof(base));
    sim->player[1].ent.pos[1] = q2_sim_origin_y(base[1]);
    sim->player[1].motion = Q2_PLAYER_MOTION_LIVE;
    for (pass = 0; pass < 2; pass++) {
        memcpy(p->pos, base, sizeof(base));
        memset(p->vel, 0, sizeof(p->vel));
        memset(p->frame_delta, 0, sizeof(p->frame_delta));
        p->motion = Q2_PLAYER_MOTION_CORPSE;
        p->corpse_separate = pass == 0;
        q2_sim_tick(sim, &input, 12);
        /* The same overlapping bodies, before and after the death chain
         * raises DEAD. Only the first pass pushes this corpse away. */
        if (pass == 0) CHECK(p->pos[0] < base[0]);
        else CHECK(p->pos[0] == base[0]);
    }
    sim->player_count = 1;
}

static void capture_fall(const char *disc_path, const char *output)
{
    client *c = calloc(1, sizeof(*c));
    q2_player *p;
    q2_input input = {0};
    s32 eye[3];
    char path[1024];
    int i;

    if (!c) exit(2);
    c->headless = true;
    c->use_sort = true;
    c->sim_enabled = true;
    c->trace_cre = c->give_powerup = c->mission_row = -1;
    c->pad_frame = -2;
    if (disc_open(&c->disc, disc_path) != Q2_OK ||
        q2_identify(c->disc, &c->build) != Q2_OK ||
        q2_screen_init(&c->screen, c->build.video) != Q2_OK) exit(2);
    c->width = c->screen.disp.width;
    c->height = c->screen.disp.height;
    q2_world_render_init(&c->render);
    psx_ot_init(&c->ot, Q2_SCREEN_OT_ENTRIES, 300000);
    psx_raster_opts_default(&c->opts);
    c->vram = calloc(1, sizeof(*c->vram));
    if (!c->vram) exit(2);
    q2_camera_default(&c->cam, c->width, c->height);
    q2_screen_set_layout(&c->screen, Q2_SCREEN_LAYOUT_ONE, 1);
    q2_menu_settings_defaults(&c->settings);
    q2_menu_init(&c->menu, &c->settings, Q2_MENU_SCREEN_H);
    q2_player_death_init(&c->death[0]);
    q2_mission_init(&c->mission);
    q2_briefing_init(&c->briefing);
    q2_briefing_popup_init(&c->popup);
    q2_prompt_init(&c->prompts);
    CHECK(client_load_zone(c, "BASE0", 0));

    p = &c->sim[0].player[0];
    /* Use the real spawn room's open space, 600 units above its floor. */
    p->pos[1] -= 600;
    p->ent.pos[1] = q2_sim_origin_y(p->pos[1]);
    p->ent.node = q2_coll_find_node(&c->sim[0].coll, p->ent.pos, -1, true);
    CHECK(p->ent.node >= 0);
    p->ent.flags = 0;
    memset(p->ent.ground_normal, 0, sizeof(p->ent.ground_normal));
    memset(p->frame_delta, 0, sizeof(p->frame_delta));
    p->on_ground = false;
    p->vel[1] = 2000;
    p->ent2_flags = Q2_ENT2_DEAD;
    c->sim[0].combat.inv.health = p->prev_health = 0;
    for (i = 0; i < 8; i++)
        q2_sim_advance(&c->sim[0], &input, 1.0 / 30.0);
    CHECK(p->ent.node >= 0);
    printf("corpse capture: feet (%d,%d,%d), velocity (%d,%d,%d)\n",
           p->pos[0], p->pos[1], p->pos[2], p->vel[0], p->vel[1], p->vel[2]);
    q2_sim_eye(&c->sim[0], eye);
    memcpy(c->cam.pos, eye, sizeof(eye));
    c->cam.yaw = p->yaw;
    c->cam.pitch = p->pitch;
    client_frame(c);
    snprintf(path, sizeof(path), "%s/corpse-fall.ppm", output);
    CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);
    test_corpse_separation(&c->sim[0]);

    client_free_creatures(c);
    if (c->sfx_ready) q2_sound_bank_free(&c->sfx);
    q2_event_store_free(&c->event_store);
    q2_zone_history_free(&c->zone_history);
    q2_gib_attach(NULL);
    q2_sim_free(&c->sim[0]);
    q2_common_close(&c->common);
    q2_world_free_zone(&c->zone);
    q2_movers_free(&c->movers);
    q2_rotators_free(&c->rotators);
    q2_explosives_free(&c->explosives);
    free(c->node_hidden);
    free(c->vram);
    q2_screen_free(&c->screen);
    psx_ot_free(&c->ot);
    disc_close(c->disc);
    free(c);
}

int main(int argc, char **argv)
{
    test_corpse_physics();
    test_motion_stages();
    test_dead_trigger_contacts();
    if (argc == 4 && !strcmp(argv[1], "--headless"))
        capture_fall(argv[2], argv[3]);
    else if (argc != 1) return 2;
    printf("%d movement checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
