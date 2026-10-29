/* Zone seams exercised through the actual loader, without a window or input. */
int q2_test_zones_client_main(int argc, char **argv);
#define main q2_test_zones_client_main
#include "../src/client/main.c"
#undef main

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; \
} } while (0)
#include "client_fixture.h"

static void test_population_history(void)
{
    q2_sim *sim = calloc(1, sizeof(*sim));
    q2_zone_history history = {0}, copy = {0};
    q2_common_file common = {0};
    q2_creature_world *world = calloc(1, sizeof(*world));
    q2_monster monsters[2];
    dat_chunk chunk = {0};
    u8 data[72] = {0};
    u32 i;
    if (!sim || !world) exit(2);
    memcpy(data, "Zone0Key", 8);
    q2_wr_u32(data + 16, 28);
    q2_wr_u16(data + 42, 27);
    q2_wr_u32(data + 44, Q2_POP_TERM_FFFF);
    chunk.data = data; chunk.size = sizeof(data);
    common.chunk[Q2_COMMON_POPULATION] = &chunk;
    q2_sim_init(sim, NULL, 50);
    CHECK(q2_sim_attach_items(sim, &common, 0, NULL, NULL) == Q2_OK);
    CHECK(q2_sim_activate_item_group(sim, "Zone0Key") == 1);
    q2_entity_remove(&sim->entities.ent[0]);
    CHECK(q2_entity_alloc(&sim->entities) == &sim->entities.ent[0]); /* reused slot */
    world->pop = sim->item_population;
    world->pop_ready = world->ready = true;
    world->set.monsters = monsters; world->set.count = 2;
    for (i = 0; i < 2; i++) {
        q2_monster_init(&monsters[i]);
        monsters[i].population_slot = i + 7;
        monsters[i].population_active = true;
        monsters[i].pop_class_id = 18;
        monsters[i].in_use = i == 1;
        monsters[i].dead = i == 0;
        monsters[i].health = i == 0 ? -100 : 13;
        monsters[i].pos[0] = 345;
        monsters[i].target = 9;
    }
    CHECK(q2_zone_history_capture(&history, "TEST", 0, sim, world, NULL, NULL, 0) == Q2_OK);
    CHECK(history.count == 1 && history.map[0].count == 3);
    CHECK(q2_zone_history_copy(&copy, &history) == Q2_OK);
    CHECK(copy.map[0].object != history.map[0].object);
    q2_sim_free(sim);
    q2_sim_init(sim, NULL, 50);
    CHECK(q2_sim_attach_items(sim, &common, 0, NULL, NULL) == Q2_OK);
    q2_zone_history_items(&history, "test", 0, sim, NULL);
    CHECK(sim->item_group_run[0] && sim->entities.count == 1 && !sim->entities.ent[0].in_use);
    CHECK(q2_sim_activate_item_group(sim, "Zone0Key") == 0);
    for (i = 0; i < 2; i++) {
        q2_monster_init(&monsters[i]);
        monsters[i].population_slot = 8 - i; /* changed module/roster order */
        monsters[i].pop_class_id = 18;
        monsters[i].spawn_pending = true;
    }
    q2_zone_history_creatures(&history, "TEST", 0, sim, world);
    CHECK(monsters[0].in_use && monsters[0].health == 13 && monsters[0].pos[0] == 345);
    CHECK(monsters[0].target == 9 && !monsters[0].spawn_pending);
    CHECK(!monsters[1].in_use && monsters[1].dead && !monsters[1].spawn_pending);
    CHECK(q2_creature_world_summon(world, "Zone0Key") == 0);
    q2_zone_history_creatures(&history, "TEST", 1, sim, world);
    CHECK(!monsters[0].in_use && !monsters[0].population_active);
    CHECK(!monsters[1].dead && !monsters[1].population_active);
    CHECK(q2_zone_history_capture(&history, "TEST", 1, sim, world, NULL, NULL, 0) == Q2_OK);
    q2_zone_history_creatures(&history, "TEST", 0, sim, world);
    CHECK(monsters[0].health == 13 && monsters[1].dead);
    data[8] = 'x'; /* different Population must never inherit old objects */
    q2_sim_free(sim);
    q2_sim_init(sim, NULL, 50);
    CHECK(q2_sim_attach_items(sim, &common, 0, NULL, NULL) == Q2_OK);
    q2_zone_history_items(&history, "TEST", 0, sim, NULL);
    CHECK(!sim->item_group_run[0]);
    CHECK(q2_zone_history_valid(&history));
    free(history.map[0].object[1].runtime);
    history.map[0].object[1] = history.map[0].object[0];
    CHECK(!q2_zone_history_valid(&history));
    CHECK(q2_zone_history_copy(&copy, &history) == Q2_ERR_INVALID_ARG);
    CHECK(copy.count == 1 && copy.map[0].object[1].kind == Q2_ZONE_CREATURE);
    q2_zone_history_free(&history); q2_zone_history_free(&copy);
    q2_sim_free(sim); free(sim); free(world);
}

static bool gate(client *c, int zone)
{
    c->carry_player = c->carry_same_map = true;
    return client_load_zone(c, c->map, zone);
}

static void test_ambush_return(const char *disc_path)
{
    client *c = open_client(disc_path);
    u32 i, group = UINT32_MAX, slot = 0;
    s32 pos[3] = {0};
    s16 health = 0;
    CHECK(client_load_zone(c, "BASE1", 1));
    CHECK(q2_creature_world_summon(&c->creatures, "LiftRoom") > 0);
    q2_sim_activate_item_group(&c->sim[0], "LiftRoom");
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *m = &c->creatures.set.monsters[i];
        q2_pop_group g;
        if (!m->in_use || !q2_pop_get_group(&c->creatures.pop, m->group, &g) ||
            strcmp(g.name, "LiftRoom")) continue;
        group = m->group; slot = m->population_slot;
        q2_monster_damage(m, 1);
        m->pos[0] += 3;
        health = m->health;
        memcpy(pos, m->pos, sizeof(pos));
        break;
    }
    CHECK(group != UINT32_MAX);
    CHECK(gate(c, 0));
    CHECK(gate(c, 1));
    for (i = 0; i < c->creatures.set.count; i++) {
        const q2_monster *m = &c->creatures.set.monsters[i];
        if (m->group != group || m->population_slot != slot) continue;
        CHECK(m->in_use && !m->spawn_pending && m->health == health);
        CHECK(!memcmp(pos, m->pos, sizeof(pos)));
        CHECK(m->think == q2_M_MoveFrame);
        break;
    }
    CHECK(i < c->creatures.set.count);
    CHECK(q2_creature_world_summon(&c->creatures, "LiftRoom") == 0);
    close_client(c);
}

static void capture(client *c, const char *dir, const char *name, const q2_camera *camera)
{
    char path[1024];
    if (!dir) return;
    CHECK(c->headless);
    c->sim_enabled = false;
    c->cam = *camera;
    q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
    client_frame(c);
    printf("capture %s: loading %d, views %d, quads %u, camera %d %d %d\n",
           c->map, c->loading.open, c->screen.view_count, c->shot_stats.quads_emitted,
           c->cam.pos[0], c->cam.pos[1], c->cam.pos[2]);
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);
}

static void test_return(const char *disc_path, const char *dir)
{
    client *c = open_client(disc_path);
    u32 i, removed = 0, killed = 0, gi;
    q2_pop_group group;
    q2_camera camera;
    CHECK(client_load_zone(c, "JAIL4", 3));
    for (gi = 0; gi < c->sim[0].item_population.group_count; gi++) {
        if (q2_pop_get_group(&c->sim[0].item_population, gi, &group) &&
            !strcmp(group.name, "Zone3")) break;
    }
    CHECK(gi < c->sim[0].item_population.group_count);
    camera = c->cam;
    for (i = 0; i < c->sim[0].entities.count; i++) {
        const q2_entity *e = &c->sim[0].entities.ent[i];
        if (e->in_use && e->population_group == (s32)gi && e->effect) {
            camera.pos[0] = e->pos[0]; camera.pos[1] = e->pos[1] - 300;
            camera.pos[2] = e->pos[2] + 1200;
            camera.yaw = 2048; camera.pitch = -160; camera.roll = 0;
            if (q2_coll_find_node(&c->sim[0].coll_primary, camera.pos, -1, true) >= 0) break;
        }
    }
    CHECK(i < c->sim[0].entities.count);
    capture(c, dir, "before-collection", &camera);
    if (i < c->sim[0].entities.count) {
        q2_entity *e = &c->sim[0].entities.ent[i];
        c->sim[0].combat.inv.health = 1;
        q2_entity_world_move_player(&c->sim[0].ent_world, 0, e->pos);
        q2_item_think(e, &c->sim[0].ent_world);
        CHECK(!e->in_use); /* exercise the actual pickup/removal path */
        if (!e->in_use) removed++;
    }
    for (i = 0; i < c->sim[0].entities.count; i++) {
        q2_entity *e = &c->sim[0].entities.ent[i];
        if (e->in_use && e->population_group == (s32)gi && e->effect) {
            q2_entity_remove(e);
            removed++;
        }
    }
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *m = &c->creatures.set.monsters[i];
        if (m->in_use && m->group == gi) {
            q2_monster_damage(m, 30000);
            killed++;
        }
    }
    CHECK(removed > 0);
    CHECK(killed > 0);
    c->sim[0].level_time = 1234;
    c->sim[0].tick_count = 321;
    c->sim[0].dt_accum = 7;
    c->sim[0].dt_frac = 0.375;
    CHECK(gate(c, 0));
    CHECK(c->sim[0].level_time == 1234 && c->sim[0].tick_count == 321);
    CHECK(c->sim[0].dt_accum == 7 && c->sim[0].dt_frac == 0.375);
    /* Save in another zone, reopen it in a fresh client, and return. */
    if (dir) {
        char path[1024];
        q2_save saved = {0};
        client *cold;
        CHECK(client_capture(c));
        snprintf(path, sizeof(path), "%s/zone-history.q2s", dir);
        CHECK(q2_save_write(&c->snapshot, path) == Q2_OK);
        CHECK(q2_save_read(&saved, path) == Q2_OK);
        CHECK(saved.zone_history_present && saved.zone_history.count == 1);
        close_client(c);
        cold = open_client(disc_path);
        CHECK(client_apply_save(cold, &saved));
        q2_save_free(&saved);
        c = cold;
    }
    CHECK(gate(c, 3));
    for (i = 0; i < c->sim[0].entities.count; i++) {
        const q2_entity *e = &c->sim[0].entities.ent[i];
        if (e->population_group == (s32)gi && e->effect) CHECK(!e->in_use);
    }
    for (i = 0; i < c->creatures.set.count; i++) {
        const q2_monster *m = &c->creatures.set.monsters[i];
        if (m->group == gi && m->in_use) CHECK(m->dead);
    }
    capture(c, dir, "after-return", &camera);
    /* A failed load must not clear mission/screen state or consume history. */
    {
        u32 serial = c->level_change_serial, loads = c->loads_done;
        u32 history_count = c->zone_history.count;
        q2_player player = c->sim[0].player[0];
        c->running = true;
        c->mission_row = 2;
        c->level_frames_drawn = true;
        c->loading_raises = 4;
        CHECK(!client_load_zone(c, "MISSING", 0));
        CHECK(c->zone_index == 3 && !strcmp(c->map, "JAIL4"));
        CHECK(c->mission_row == 2 && c->level_frames_drawn);
        CHECK(c->level_change_serial == serial && c->loads_done == loads);
        CHECK(c->loading_raises == 4 && !c->loading.open);
        CHECK(c->zone_history.count == history_count);
        CHECK(!memcmp(&player, &c->sim[0].player[0], sizeof(player)));
        CHECK(!gate(c, 99));
        CHECK(!c->carry_player && !c->carry_same_map && !c->carry_pos_valid);
    }
    /* A genuine level trip also keeps the visited map's population. */
    CHECK(client_load_zone(c, "BASE0", 0));
    CHECK(client_load_zone(c, "JAIL4", 3));
    for (i = 0; i < c->creatures.set.count; i++)
        if (c->creatures.set.monsters[i].group == gi)
            CHECK(!c->creatures.set.monsters[i].in_use || c->creatures.set.monsters[i].dead);
    snprintf(c->first_map, sizeof(c->first_map), "%s", "JAIL4");
    client_start_game(c);
    CHECK(!c->zone_history.count && !c->event_store.count);
    CHECK(gate(c, 3));
    {
        u32 alive = 0;
        for (i = 0; i < c->creatures.set.count; i++)
            if (c->creatures.set.monsters[i].group == gi &&
                c->creatures.set.monsters[i].in_use && !c->creatures.set.monsters[i].dead) alive++;
        CHECK(alive == killed);
    }
    close_client(c);
}

static int drop_rand(void) { return 12345; }

static void test_deferred_drops(void)
{
    q2_sim *sim = calloc(1, sizeof(*sim));
    q2_zone_history h = {0};
    q2_common_file common = {0};
    dat_chunk chunk = {0};
    u8 data[72] = {0};
    u32 i, live = 0;
    if (!sim) exit(2);
    memcpy(data, "Zone0Key", 8); q2_wr_u32(data + 16, 28);
    q2_wr_u16(data + 42, 27); q2_wr_u32(data + 44, Q2_POP_TERM_FFFF);
    chunk.data = data; chunk.size = sizeof(data); common.chunk[Q2_COMMON_POPULATION] = &chunk;
    q2_sim_init(sim, NULL, 50);
    CHECK(q2_sim_attach_items(sim, &common, 0, NULL, NULL) == Q2_OK);
    CHECK(q2_zone_history_capture(&h, "TEST", 0, sim, NULL, NULL, NULL, 0) == Q2_OK);
    h.map[0].count = h.map[0].capacity = h.map[0].next_serial = Q2_ITEM_TOSS_MAX + 1;
    h.map[0].object = calloc(h.map[0].count, sizeof(*h.map[0].object));
    if (!h.map[0].object) exit(2);
    for (i = 0; i < h.map[0].count; i++) {
        q2_zone_object *o = &h.map[0].object[i];
        q2_zone_runtime *r = o->runtime = calloc(1, sizeof(*r));
        if (!r) exit(2);
        o->kind = Q2_ZONE_DYNAMIC; o->slot = i + 1; o->id = 27;
        q2_entity_init(&r->entity.state);
        r->entity.state.in_use = true; r->entity.state.kind = Q2_ENT_KIND_ITEM;
        r->entity.state.place_id = 27; r->entity.state.zone_serial = o->slot;
        r->entity.think = Q2_ZTHINK_DROP; r->entity.source_slot = UINT32_MAX;
        r->entity.velocity[1] = -2000;
    }
    CHECK(q2_zone_history_valid(&h));
    q2_zone_history_items(&h, "TEST", 0, sim, NULL);
    CHECK(q2_item_drop_in_flight() == Q2_ITEM_TOSS_MAX);
    CHECK(!h.map[0].object[Q2_ITEM_TOSS_MAX].runtime->materialized);
    CHECK(q2_zone_history_capture(&h, "TEST", 0, sim, NULL, NULL, NULL, 0) == Q2_OK);
    CHECK(h.map[0].count == Q2_ITEM_TOSS_MAX + 1); /* capacity did not erase it */
    q2_entity_remove(&sim->entities.ent[0]);
    CHECK(q2_zone_history_capture(&h, "TEST", 0, sim, NULL, NULL, NULL, 0) == Q2_OK);
    CHECK(h.map[0].count == Q2_ITEM_TOSS_MAX);
    q2_item_drop_reset(); q2_sim_free(sim); q2_sim_init(sim, NULL, 50);
    CHECK(q2_sim_attach_items(sim, &common, 0, NULL, NULL) == Q2_OK);
    q2_zone_history_items(&h, "TEST", 0, sim, NULL);
    for (i = 0; i < sim->entities.count; i++)
        if (sim->entities.ent[i].in_use && sim->entities.ent[i].zone_serial == Q2_ITEM_TOSS_MAX + 1) live++;
    CHECK(live == 1 && q2_item_drop_in_flight() == Q2_ITEM_TOSS_MAX);
    q2_zone_history_free(&h); q2_item_drop_reset(); q2_sim_free(sim);
    q2_sim_init(sim, NULL, 50);
    CHECK(q2_sim_attach_items(sim, &common, 0, NULL, NULL) == Q2_OK);
    CHECK(q2_sim_activate_item_group(sim, "Zone0Key") == 1);
    CHECK(q2_zone_history_capture(&h, "TEST", 0, sim, NULL, NULL, NULL, 0) == Q2_OK);
    h.map[0].object[0].runtime->entity.model_bound = true;
    q2_sim_free(sim); q2_sim_init(sim, NULL, 50);
    CHECK(q2_sim_attach_items(sim, &common, 0, NULL, NULL) == Q2_OK);
    q2_zone_history_items(&h, "TEST", 0, sim, NULL); /* missing destination CastList */
    CHECK(!sim->entities.ent[0].in_use && !h.map[0].object[0].runtime->materialized);
    CHECK(q2_zone_history_capture(&h, "TEST", 0, sim, NULL, NULL, NULL, 0) == Q2_OK);
    CHECK(!h.map[0].object[0].removed && h.map[0].object[0].runtime != NULL);
    q2_zone_history_free(&h); q2_sim_free(sim); free(sim);
}

static q2_entity *dynamic_entity(client *c, u32 serial)
{
    u32 i;
    for (i = 0; i < c->sim[0].entities.count; i++)
        if (c->sim[0].entities.ent[i].in_use && c->sim[0].entities.ent[i].zone_serial == serial)
            return &c->sim[0].entities.ent[i];
    return NULL;
}

static q2_projectile *dynamic_projectile(client *c, u32 serial)
{
    u32 i;
    for (i = 0; i < Q2_PROJ_MAX; i++)
        if (c->sim[0].combat.projectiles.p[i].in_use && c->sim[0].combat.projectiles.p[i].zone_serial == serial)
            return &c->sim[0].combat.projectiles.p[i];
    return NULL;
}

static q2_monster *authored_creature(client *c, u32 group, u32 slot, u32 *index)
{
    u32 i;
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *m = &c->creatures.set.monsters[i];
        if (m->group == group && m->population_slot == slot) {
            if (index) *index = i;
            return m;
        }
    }
    return NULL;
}

static client *cold_save(client *c, const char *disc, const char *dir, const char *name)
{
    q2_save saved = {0};
    char path[1024];
    if (!dir) return c;
    CHECK(client_capture(c));
    snprintf(path, sizeof(path), "%s/%s.q2s", dir, name);
    CHECK(q2_save_write(&c->snapshot, path) == Q2_OK);
    CHECK(q2_save_read(&saved, path) == Q2_OK);
    close_client(c);
    c = open_client(disc);
    CHECK(client_apply_save(c, &saved));
    q2_save_free(&saved);
    return c;
}

static void test_overlap(const char *disc)
{
    client *c = open_client(disc);
    q2_world_zone destination = {0};
    q2_collision hull;
    s32 pos[3] = {0};
    bool found = false;
    u32 i, j, serial, shot_serial, shooter = 0, shooter_group, shooter_slot;
    const q2_model_bank *banks[2];
    q2_entity *e;
    CHECK(client_load_zone(c, "BASE1", 0));
    CHECK(q2_world_load_zone(&destination, c->disc, "BASE1", 1) == Q2_OK);
    CHECK(q2_collision_parse(&hull, &destination.zone, Q2_COLL_PRIMARY) == Q2_OK);
    for (i = 0; i < c->sim[0].triggers.count; i++) {
        q2_trigger t;
        if (!q2_trigger_get(&c->sim[0].triggers, i, &t)) continue;
        for (j = 0; j < 3; j++) pos[j] = (t.min[j] + t.max[j]) / 2;
        if (q2_coll_find_node(&c->sim[0].coll_primary, pos, -1, true) >= 0 &&
            q2_coll_find_node(&hull, pos, -1, true) >= 0) { found = true; break; }
    }
    CHECK(found);
    if (!found) { q2_world_free_zone(&destination); close_client(c); return; }
    for (i = 0; i < c->creatures.set.count; i++)
        if (c->creatures.set.monsters[i].in_use &&
            q2_coll_find_node(&hull, c->creatures.set.monsters[i].pos, -1, true) < 0) { shooter = i; break; }
    CHECK(i < c->creatures.set.count);
    shooter_group = c->creatures.set.monsters[shooter].group;
    shooter_slot = c->creatures.set.monsters[shooter].population_slot;
    q2_world_free_zone(&destination);
    banks[0] = c->sim[0].item_bank; banks[1] = &c->zone_bank;
    e = q2_item_drop_spawn(&c->sim[0].entities, c->sim[0].item_table, banks, 2, 27, 0, pos, -1, drop_rand);
    CHECK(e != NULL);
    if (!e) { close_client(c); return; }
    q2_projectiles_init(&c->sim[0].combat.projectiles);
    c->sim[0].combat.projectiles.p[0].in_use = true;
    c->sim[0].combat.projectiles.p[0].kind = Q2_PROJ_ROCKET;
    c->sim[0].combat.projectiles.p[0].owner = -1;
    c->sim[0].combat.projectiles.p[0].shooter = &c->cre_actor[shooter];
    memcpy(c->sim[0].combat.projectiles.p[0].pos, pos, sizeof(pos));
    c->sim[0].combat.projectiles.live = 1;
    CHECK(client_capture(c));
    serial = e->zone_serial; shot_serial = c->sim[0].combat.projectiles.p[0].zone_serial;
    for (j = 0; j < 4; j++) {
        u32 count = 0;
        CHECK(gate(c, (int)(1 - j % 2)));
        CHECK(dynamic_entity(c, serial) != NULL && dynamic_projectile(c, shot_serial) != NULL);
        for (i = 0; i < c->sim[0].entities.count; i++)
            if (c->sim[0].entities.ent[i].in_use && c->sim[0].entities.ent[i].zone_serial == serial) count++;
        CHECK(count == 1 && c->sim[0].combat.projectiles.live == 1);
        CHECK(authored_creature(c, shooter_group, shooter_slot, &shooter) != NULL);
        CHECK(dynamic_projectile(c, shot_serial)->shooter == &c->cre_actor[shooter]);
    }
    CHECK(gate(c, 1));
    q2_entity_remove(dynamic_entity(c, serial));
    CHECK(q2_projectile_expire(&c->sim[0].combat.projectiles, 0));
    CHECK(gate(c, 0));
    CHECK(dynamic_entity(c, serial) == NULL && dynamic_projectile(c, shot_serial) == NULL);
    close_client(c);
}

static void test_runtime_return(const char *disc, const char *dir)
{
    client *c = open_client(disc);
    q2_entity entities[4];
    q2_entity drop_step;
    q2_projectile shots[6];
    q2_zone_runtime creature[2] = {0};
    q2_zone_animation animation[2];
    u32 group[2], slot[2], indices[2], entity_slots[4], i, j, n = 0;
    q2_camera camera;
    s16 velocity[3], actual_velocity[3];
    s16 step_velocity[3];
    bool step_flying;
    q2_gib_body body;
    s32 anchor[3] = {-7349, 6113, -4798};
    const q2_model_bank *banks[2];
    q2_entity *e;
    CHECK(client_load_zone(c, "JAIL4", 3));
    c->sim[0].level_time = 1000; c->sim[0].ent_world.level_time = 1000;
    q2_level_state.time = q2_level_state.framenum = 30;
    c->ai_accum = 0.037;
    banks[0] = c->sim[0].item_bank; banks[1] = &c->zone_bank;
    for (i = 0; i < 2; i++) {
        s32 pos[3] = {anchor[0] - 300 + (s32)i * 600, anchor[1] - 100, anchor[2]};
        e = q2_item_drop_spawn(&c->sim[0].entities, c->sim[0].item_table, banks, 2,
                               27, 768, pos, -1, drop_rand);
        CHECK(e != NULL);
        if (!e) { close_client(c); return; }
        entity_slots[i] = (u32)(e - c->sim[0].entities.ent);
        if (i == 0) CHECK(q2_item_drop_velocity(e, velocity));
        else {
            /* A settled drop uses the ordinary pickup think with a timed life. */
            e->think = q2_item_think; e->remove_in = 1234;
            e->origin[1] = e->pos[1] + Q2_EYE_BASE - e->model_offset;
            q2_entity_set_bounds(e, Q2_ITEM_TOUCH_HALF);
        }
        e->frame = 37 + (s32)i; e->scale = 2048 + (s16)i;
    }
    e = q2_model_ent_spawn(&c->sim[0].entities, c->sim[0].item_bank,
                            0, anchor, 0, 2048, NULL);
    CHECK(e != NULL);
    if (!e) { close_client(c); return; }
    entity_slots[2] = (u32)(e - c->sim[0].entities.ent);
    e->frame = 173; e->fade = 3000;
    {
        q2_gib_parent parent;
        s16 impulse[3] = {20, -40, 10};
        q2_gib_parent_init(&parent);
        e = q2_gib_spawn_one(q2_gib_attached(), &parent, Q2_GIB_STYLE_MEAT,
                              768, impulse, anchor, -1, 1200, "Gib0");
        /* Retail allows a gib with an unresolved model; it must keep moving
         * and expiring across loads just like a visible body. */
        CHECK(e != NULL);
        if (!e) { close_client(c); return; }
        entity_slots[3] = (u32)(e - c->sim[0].entities.ent);
        CHECK(q2_gib_body_of(e) != NULL);
        body = *q2_gib_body_of(e);
    }
    for (i = 0; i < c->creatures.set.count && n < 2; i++) {
        q2_monster *m = &c->creatures.set.monsters[i];
        q2_pop_group g;
        if (!m->in_use || !m->attack || !q2_pop_get_group(&c->creatures.pop, m->group, &g) ||
            strcmp(g.name, "Zone3")) continue;
        group[n] = m->group; slot[n] = m->population_slot; indices[n] = i;
        m->enemy = m->goalentity = &c->creatures.sight;
        if (n == 0) m->attack(m);
        else q2_monster_damage(m, m->health + 1); /* dying, not gibbed */
        CHECK(m->currentmove != NULL);
        if (m->currentmove) m->frame = (s16)(m->currentmove->first_frame + 1);
        m->next_think = q2_level_state.time + 4;
        m->attack_finished = q2_level_state.time + 9;
        m->pain_debounce = q2_level_state.time + 6;
        m->aiflags |= Q2_AI_HOLD_FRAME;
        n++;
    }
    CHECK(n == 2);
    if (n != 2) { close_client(c); return; }
    q2_projectiles_init(&c->sim[0].combat.projectiles);
    for (i = 0; i < 6; i++) {
        q2_projectile *p = &c->sim[0].combat.projectiles.p[i];
        p->in_use = true; p->kind = (q2_proj_kind)(i < 5 ? i + 1 : Q2_PROJ_HAND_GRENADE);
        p->owner = i == 3 ? -1 : 0;
        if (i == 3) p->shooter = &c->cre_actor[indices[0]];
        memcpy(p->pos, anchor, sizeof(p->pos));
        p->pos[0] += (s32)i * 80 - 200; p->pos[1] -= 400; p->pos[2] += 200;
        p->vel[0] = 8192; p->vel[1] = -4096; p->vel[2] = 12288;
        p->node = i == 5 ? Q2_PROJ_NODE_HELD : Q2_PROJ_NODE_UNKNOWN;
        p->expires = i == 5 ? 6000 : 1456 + (s32)i; p->damage = 21; p->mod = 5;
        p->splash_radius = 200; p->flags = 11; p->bounced = i == 1;
        p->angles[0] = 123; p->angles[1] = 789; p->angles[2] = 11;
        p->base[0] = 101; p->base[1] = 2000; p->phase = 1357;
        p->anim = 77; p->anim_end = 320; p->beam_time = 17;
    }
    c->sim[0].combat.projectiles.live = 6;
    camera = c->cam;
    camera.pos[0] = anchor[0]; camera.pos[1] = anchor[1] - 300; camera.pos[2] = anchor[2] + 1200;
    camera.yaw = 2048; camera.pitch = -160; camera.roll = 0;
    capture(c, dir, "before-zone-change", &camera);
    for (i = 0; i < 2; i++) {
        q2_monster *m = &c->creatures.set.monsters[indices[i]];
        /* Two display samples between AI ticks; retain the fractional pose. */
        client_model_anim_sample(&c->cre_anim[indices[i]], m->currentmove, 2, m->frame * 30, 10, 100);
        client_model_anim_sample(&c->cre_anim[indices[i]], m->currentmove, 2, m->frame * 30, 10, 101);
        CHECK(c->cre_anim[indices[i]].cursor.ready);
    }
    e = &c->sim[0].entities.ent[entity_slots[0]];
    {
        q2_entity original = *e;
        s32 dt = c->sim[0].ent_world.dt;
        c->sim[0].ent_world.dt = 10;
        q2_item_drop_think(e, &c->sim[0].ent_world);
        drop_step = *e;
        step_flying = e->think == q2_item_drop_think;
        if (step_flying) CHECK(q2_item_drop_velocity(e, step_velocity));
        *e = original;
        CHECK(q2_item_drop_restore(&c->sim[0].entities, entity_slots[0], velocity));
        c->sim[0].ent_world.dt = dt;
    }
    CHECK(client_capture(c)); /* gives dynamics stable IDs */
    for (i = 0; i < 4; i++) entities[i] = c->sim[0].entities.ent[entity_slots[i]];
    for (i = 0; i < 4; i++) printf("runtime entity %u: serial %u, kind %u, place %u, model %s, live %d\n",
        i, entities[i].zone_serial, entities[i].kind, entities[i].place_id, entities[i].model, entities[i].in_use);
    memcpy(shots, c->sim[0].combat.projectiles.p, sizeof(shots));
    for (i = 0; i < 2; i++) {
        CHECK(q2_zone_capture_monster(&creature[i], &c->creatures, &c->creatures.set.monsters[indices[i]]));
        animation[i] = c->cre_anim[indices[i]];
    }
    CHECK(gate(c, 0));
    CHECK(dynamic_entity(c, entities[0].zone_serial) == NULL);
    CHECK(dynamic_projectile(c, shots[3].zone_serial) == NULL);
    CHECK(q2_projectile_hand_held_index(&c->sim[0].combat.projectiles, 0) >= 0);
    /* The active zone advances; suspended objects retain their remaining life. */
    c->sim[0].level_time += 1000; c->sim[0].ent_world.level_time = c->sim[0].level_time;
    q2_level_state.time += 100; q2_level_state.framenum += 100;
    c->ai_accum = 0.037;
    c = cold_save(c, disc, dir, "unloaded-runtime");
    CHECK(gate(c, 3));
    CHECK(q2_level_state.time == 130 && q2_level_state.framenum == 130);
    CHECK(c->ai_accum > 0.036999 && c->ai_accum < 0.037001);
    for (j = 0; j < 2; j++) {
        /* Also exercise a cold native save in the zone containing the drops. */
        if (j) c = cold_save(c, disc, dir, "resident-runtime");
        for (i = 0; i < 4; i++) {
            e = dynamic_entity(c, entities[i].zone_serial);
            CHECK(e != NULL);
            if (!e) continue;
            CHECK(e->think == entities[i].think && e->def == (i < 2 ? q2_item_find(c->sim[0].item_table, 27) : NULL));
            CHECK(i == 3 ? !e->model_bank && e->model_index < 0 : e->model_bank && e->model_index >= 0);
            CHECK(!memcmp(e->pos, entities[i].pos, sizeof(e->pos)));
            CHECK(!memcmp(e->origin, entities[i].origin, sizeof(e->origin)));
            CHECK(!memcmp(e->bounds_min, entities[i].bounds_min, sizeof(e->bounds_min)));
            CHECK(e->frame == entities[i].frame && e->remove_in == entities[i].remove_in);
            CHECK(e->fade == entities[i].fade && e->render_flags == entities[i].render_flags);
        }
        e = dynamic_entity(c, entities[0].zone_serial);
        CHECK(e && q2_item_drop_velocity(e, actual_velocity) && !memcmp(velocity, actual_velocity, sizeof(velocity)));
        e = dynamic_entity(c, entities[3].zone_serial);
        CHECK(e && q2_gib_body_of(e) && !memcmp(body.vel, q2_gib_body_of(e)->vel, sizeof(body.vel)));
        for (i = 0; i < 6; i++) {
            q2_projectile *p = dynamic_projectile(c, shots[i].zone_serial);
            CHECK(p != NULL);
            if (!p) continue;
            CHECK(p->kind == shots[i].kind && p->flags == shots[i].flags && p->owner == shots[i].owner);
            CHECK(p->expires == shots[i].expires + (i == 5 ? 0 : 1000));
            CHECK(!memcmp(p->pos, shots[i].pos, sizeof(p->pos)) && !memcmp(p->vel, shots[i].vel, sizeof(p->vel)));
            CHECK(!memcmp(p->angles, shots[i].angles, sizeof(p->angles)) && !memcmp(p->base, shots[i].base, sizeof(p->base)));
            CHECK(p->phase == shots[i].phase && p->anim == shots[i].anim && p->anim_end == shots[i].anim_end);
            CHECK(p->beam_time == shots[i].beam_time && p->bounced == shots[i].bounced);
            if (i == 5) CHECK(p->node == Q2_PROJ_NODE_HELD);
            if (i == 3) {
                u32 shooter_index = 0;
                CHECK(authored_creature(c, group[0], slot[0], &shooter_index) != NULL);
                CHECK(p->shooter == &c->cre_actor[shooter_index]);
            }
        }
        for (i = 0; i < 2; i++) {
            u32 index = 0;
            q2_zone_runtime actual = {0};
            q2_monster *m = authored_creature(c, group[i], slot[i], &index);
            CHECK(m && m->in_use);
            if (!m) continue;
            CHECK(q2_zone_capture_monster(&actual, &c->creatures, m));
            CHECK(actual.creature.move_offset == creature[i].creature.move_offset &&
                  !strcmp(actual.creature.move_module, creature[i].creature.move_module));
            CHECK(m->frame == creature[i].creature.state.frame && m->dead == creature[i].creature.state.dead);
            CHECK(m->next_think == creature[i].creature.state.next_think + 100);
            CHECK(m->attack_finished == creature[i].creature.state.attack_finished + 100);
            CHECK(m->enemy == &c->creatures.sight && m->goalentity == &c->creatures.sight);
            CHECK(m->aiflags == creature[i].creature.state.aiflags);
            CHECK(c->cre_anim[index].cursor.position == animation[i].cursor.position &&
                  c->cre_anim[index].cursor.target == animation[i].cursor.target &&
                  c->cre_anim[index].cursor.ready == animation[i].cursor.ready);
        }
    }
    capture(c, dir, "after-zone-return", &camera);
    /* A resumed drop takes the same next collision/gravity step as it would
     * have taken without the unload. This exercises the restored side table. */
    e = dynamic_entity(c, entities[0].zone_serial);
    if (e) {
        c->sim[0].ent_world.dt = 10;
        q2_item_drop_think(e, &c->sim[0].ent_world);
        CHECK(!memcmp(e->pos, drop_step.pos, sizeof(e->pos)) && e->think == drop_step.think);
        CHECK(!memcmp(e->origin, drop_step.origin, sizeof(e->origin)) && e->remove_in == drop_step.remove_in);
        if (step_flying) CHECK(q2_item_drop_velocity(e, actual_velocity) && !memcmp(actual_velocity, step_velocity, sizeof(step_velocity)));
    }
    for (i = 0; i < 6; i++) {
        q2_projectile *p = dynamic_projectile(c, shots[i].zone_serial);
        q2_projectiles control = {0};
        q2_proj_step expected, actual;
        if (!p) continue;
        control.p[0] = shots[i]; control.live = 1;
        q2_projectile_step(&control, 0, Q2_GRAVITY, 10, 1001, &expected);
        q2_projectile_step(&c->sim[0].combat.projectiles,
                            (u32)(p - c->sim[0].combat.projectiles.p), Q2_GRAVITY, 10, 2001, &actual);
        CHECK(!memcmp(actual.to, expected.to, sizeof(actual.to)) && actual.expired == expected.expired);
        CHECK(!memcmp(p->vel, control.p[0].vel, sizeof(p->vel)) && p->phase == control.p[0].phase);
        CHECK(p->anim == control.p[0].anim && p->beam_time == control.p[0].beam_time);
    }
    for (i = 0; i < 2; i++) {
        q2_monster *m = authored_creature(c, group[i], slot[i], NULL);
        if (m && m->currentmove) {
            const q2_mmove *move = m->currentmove;
            s16 frame = m->frame;
            m->aiflags &= ~Q2_AI_HOLD_FRAME;
            q2_M_MoveFrame(m);
            CHECK(m->frame != frame || m->currentmove != move);
        }
    }
    /* Detached corpses have their own dissolve timer and callback policy. */
    {
        q2_monster *m = authored_creature(c, group[1], slot[1], NULL);
        CHECK(m != NULL);
        if (m) {
            q2_monster_corpse_detach(m);
            m->dissolve = 1007; m->dissolve_arm = true;
        }
    }
    /* Consumed dynamics stay gone even after their allocator slot is reused. */
    e = dynamic_entity(c, entities[0].zone_serial);
    if (e) q2_entity_remove(e);
    e = q2_entity_alloc(&c->sim[0].entities);
    CHECK(e && e->zone_serial == 0);
    for (i = 0; i < Q2_PROJ_MAX; i++)
        if (c->sim[0].combat.projectiles.p[i].zone_serial == shots[3].zone_serial)
            CHECK(q2_projectile_expire(&c->sim[0].combat.projectiles, i));
    CHECK(gate(c, 0)); CHECK(gate(c, 3));
    CHECK(dynamic_entity(c, entities[0].zone_serial) == NULL);
    CHECK(dynamic_projectile(c, shots[3].zone_serial) == NULL);
    {
        q2_monster *m = authored_creature(c, group[1], slot[1], NULL);
        CHECK(m && m->corpse && m->dissolve == 1007 && m->dissolve_arm);
        if (m) {
            q2_monster expected = *m;
            q2_monster_corpse_tick(&expected);
            q2_monster_corpse_tick(m);
            CHECK(m->dissolve == expected.dissolve && m->in_use == expected.in_use);
        }
    }
    close_client(c);
}

int main(int argc, char **argv)
{
    test_population_history();
    test_deferred_drops();
    if (argc >= 3 && !strcmp(argv[1], "--headless")) {
        test_return(argv[2], argc > 3 ? argv[3] : NULL);
        test_ambush_return(argv[2]);
        test_runtime_return(argv[2], argc > 3 ? argv[3] : NULL);
        test_overlap(argv[2]);
    }
    else if (argc != 1) return 2;
    printf("%d zone checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
