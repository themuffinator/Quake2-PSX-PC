/* Map population through the real CREBATCH client hook. Optional disc checks
 * exercise shipped suffix groups and render deterministic engine frames. */
int q2_test_population_client_main(int argc, char **argv);
#define main q2_test_population_client_main
#include "../src/client/main.c"
#undef main

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    failures++; } } while (0)

#include "client_fixture.h"

static void test_synthetic_batch(void)
{
    client *c = calloc(1, sizeof(*c));
    q2_monster monsters[2];
    u8 pop[72] = {0}, funcs[16] = {1, 0, 0, 0}, call[16] = {0};
    q2_common_file common = {0};
    dat_chunk chunk = {0};
    q2_event_item item = {0};
    int i;
    if (!c) exit(2);

    memcpy(pop, "Zone0Key", 8);
    q2_wr_u32(pop + 16, 28);
    q2_wr_u16(pop + 28 + 14, 27); /* one Shells P */
    q2_wr_u32(pop + 44, Q2_POP_TERM_FFFF);
    chunk.data = pop; chunk.size = sizeof(pop);
    common.chunk[Q2_COMMON_POPULATION] = &chunk;
    q2_sim_init(&c->sim[0], NULL, 50);
    CHECK(q2_sim_attach_items(&c->sim[0], &common, 0, NULL, NULL) == Q2_OK);
    CHECK(c->sim[0].entities.count == 0 && !c->sim[0].item_group_run[0]);
    c->creatures.pop = c->sim[0].item_population;
    c->creatures.pop_ready = c->creatures_ready = true;
    c->creatures.set.monsters = monsters;
    c->creatures.set.count = 2;
    for (i = 0; i < 2; i++) {
        q2_monster_init(&monsters[i]);
        monsters[i].health = 20;
        monsters[i].in_use = i == 0; /* second record failed the zone hull */
    }
    CHECK(q2_creature_world_hold_batches(&c->creatures, 0, NULL, 0) == 1);
    memcpy(funcs + 4, "CREBATCH", 8);
    c->sim[0].userfuncs.data = funcs;
    c->sim[0].userfuncs.size = sizeof(funcs);
    c->sim[0].userfuncs.count = 1;
    c->sim[0].userfuncs_ready = true;
    call[0] = Q2_EVOP_CALL; call[1] = sizeof(call);
    memcpy(call + 4, "Zone0Key", 8);
    item.op = item.opcode = Q2_EVOP_CALL;
    item.len = sizeof(call); item.payload = call + 2;

    /* EVE_ reconstruction must not consume either half of the batch. */
    c->sim[0].event_rt.initial_pass = true;
    client_event_call(c, &item, 0);
    CHECK(!monsters[0].in_use && monsters[0].spawn_pending);
    CHECK(c->sim[0].entities.count == 0);
    c->sim[0].event_rt.initial_pass = false;
    client_event_call(c, &item, 0);
    CHECK(monsters[0].in_use && !monsters[0].spawn_pending);
    CHECK(monsters[0].think == q2_M_MoveFrame && !monsters[1].in_use);
    CHECK(c->sim[0].entities.count == 1 && c->sim[0].item_group_run[0]);
    CHECK(c->script_summoned == 1);
    monsters[0].in_use = false;
    c->sim[0].entities.ent[0].in_use = false;
    client_event_call(c, &item, 0);
    CHECK(!monsters[0].in_use && !monsters[1].in_use);
    CHECK(!c->sim[0].entities.ent[0].in_use && c->script_summoned == 1);
    q2_sim_free(&c->sim[0]);
    free(c);
    q2_level_reset();
}

static bool named_call(client *c, q2_uf_prim prim, const char *name,
                         q2_event_item *out, u8 *index)
{
    const q2_events *events = &c->sim[0].events;
    q2_event_record record;
    if (!q2_events_first_record(events, &record)) return false;
    do {
        u32 i;
        for (i = 0; i < record.n_items; i++) {
            q2_uf_call call;
            char key[13];
            if (q2_events_get_item(events, &record, i, out) &&
                q2_uf_decode_call(&call, &c->sim[0].userfuncs, out) == Q2_OK &&
                call.prim == prim &&
                q2_uf_operand_name(&call, 0, key) && !strcmp(key, name)) {
                *index = call.func_index;
                return true;
            }
        }
    } while (q2_events_next_record(events, &record, &record));
    return false;
}

static void capture_batch(client *c, const char *output, const char *name)
{
    char path[1024];
    if (!output) return;
    CHECK(c->headless);
    c->sim_enabled = false; /* fixed diagnostic camera, no input device */
    c->cam.pos[0] = -13480; c->cam.pos[1] = 8000; c->cam.pos[2] = 3750;
    c->cam.yaw = 2048; c->cam.pitch = -600; c->cam.roll = 0;
    q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
    client_frame(c);
    CHECK(c->sort_cell >= 0 && c->zone.sort != NULL);
    snprintf(path, sizeof(path), "%s/%s.ppm", output, name);
    CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);
}

static void test_disc_batches(const char *disc_path, const char *output)
{
    static const struct { const char *map, *group; int zone; } cases[] = {
        { "BASE2", "Zone0Room2", 0 },
        { "BASE3", "Zone1bob", 1 },
        { "BASE3", "Zone1baz", 1 }, { "BASE3", "Zone1jeff", 1 },
        { "BASE3", "Zone1fred", 1 },
        { "BIGGUN", "Zone0_3", 0 },
        { "JAIL4", "Zone0Key", 0 }, { "JAIL4", "Zone1Key", 1 },
        { "JAIL4", "Zone3Key", 3 },
        { "JAIL5", "Zone2_1", 2 }, { "JAIL5", "Zone1Key", 1 },
        { "JAIL5", "Zone3Extra", 3 }, { "LAB", "Zone1_2", 1 },
        { "MAGDEMO", "Zone1bob", 1 },
        { "MAGDEMO", "Zone1baz", 1 }, { "MAGDEMO", "Zone1jeff", 1 },
        { "MAGDEMO", "Zone1fred", 1 }
    };
    u32 ci;
    for (ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
        client *c = open_client(disc_path);
        q2_sim *sim = &c->sim[0];
        q2_event_item call;
        q2_pop_group group;
        u32 gi, i, pending = 0, alive = 0, places = 0, before;
        u8 index;
        bool found;
        printf("%s zone %d: %s\n", cases[ci].map, cases[ci].zone, cases[ci].group);
        CHECK(client_load_zone(c, cases[ci].map, cases[ci].zone));
        for (gi = 0; gi < sim->item_population.group_count; gi++) {
            if (q2_pop_get_group(&sim->item_population, gi, &group) &&
                !strcmp(group.name, cases[ci].group)) break;
        }
        CHECK(gi < sim->item_population.group_count);
        if (gi == sim->item_population.group_count) { close_client(c); continue; }
        CHECK(!sim->item_group_run[gi]);
        for (i = 0; i < c->creatures.set.count; i++) {
            q2_monster *m = &c->creatures.set.monsters[i];
            if (m->group == gi) {
                CHECK(!m->in_use);
                pending += m->spawn_pending;
            }
        }
        for (i = 0; ; i++) {
            q2_pop_place place;
            if (!q2_pop_get_place(&sim->item_population, &group, i, &place)) break;
            if (q2_pop_place_allows_skill(place.angle_flags, q2_cre_skill())) places++;
        }
        found = named_call(c, Q2_UF_CREBATCH, group.name, &call, &index);
        CHECK(found);
        if (!found) { close_client(c); continue; }
        if (!strcmp(group.name, "Zone3Key")) capture_batch(c, output, "jail4-waiting");
        before = sim->entities.count;
        client_event_call(c, &call, index);
        CHECK(sim->item_group_run[gi]);
        CHECK(sim->entities.count == before + places);
        for (i = 0; i < c->creatures.set.count; i++) {
            q2_monster *m = &c->creatures.set.monsters[i];
            if (m->group == gi) {
                CHECK(!m->spawn_pending);
                if (m->in_use) { alive++; CHECK(m->think == q2_M_MoveFrame); }
            }
        }
        CHECK(alive == pending);
        CHECK(c->script_summoned == pending);
        client_event_call(c, &call, index);
        CHECK(c->script_summoned == pending && sim->entities.count == before + places);
        if (!strcmp(group.name, "Zone3Key")) {
            capture_batch(c, output, "jail4-summoned");
            for (i = before; i < sim->entities.count; i++) {
                q2_entity *e = &sim->entities.ent[i];
                CHECK(e->node >= 0 && e->pos[1] == 8674);
                CHECK(e->spawn_origin[1] == 8960);
            }
        }
        printf("  activated %u creatures and %u items; repeated call is inert\n", alive, places);
        close_client(c);
    }
}

static u32 live_group(const client *c, const char *name)
{
    u32 i, live = 0;
    for (i = 0; i < c->creatures.set.count; i++) {
        const q2_monster *m = &c->creatures.set.monsters[i];
        q2_pop_group g;
        if (m->in_use && !m->dead && q2_pop_get_group(&c->creatures.pop, m->group, &g) &&
            !strcmp(g.name, name)) live++;
    }
    return live;
}

static u32 live_items(const client *c)
{
    u32 i, live = 0;
    for (i = 0; i < c->sim[0].entities.count; i++)
        if (c->sim[0].entities.ent[i].in_use) live++;
    return live;
}

static bool group_selected(const client *c, const char *name)
{
    const q2_sim *sim = &c->sim[0];
    u32 i;
    for (i = 0; i < sim->item_population.group_count; i++) {
        q2_pop_group g;
        if (q2_pop_get_group(&sim->item_population, i, &g) && !strcmp(g.name, name))
            return sim->item_group_run[i] != 0;
    }
    return false;
}

static u8 named_flags(const client *c, const char *name)
{
    const q2_event_rt *rt = &c->sim[0].event_rt;
    u32 i;
    for (i = 0; i < rt->events.dir_count; i++) {
        q2_event_dir_entry e;
        if (q2_events_get_dir_entry(&rt->events, i, &e) && !strcmp(e.name, name))
            return q2_event_rt_flags(rt, e.offset);
    }
    CHECK(false);
    return 0;
}

static u32 hidden_nodes(const client *c)
{
    u32 i, count = 0;
    for (i = 0; i < c->node_hidden_count; i++)
        if (c->node_hidden[i]) count++;
    return count;
}

static void reload_snapshot(client *c, const char *output)
{
    CHECK(client_capture(c));
    if (output) {
        q2_save loaded = {0};
        char path[1024];
        snprintf(path, sizeof(path), "%s/population-resume.q2s", output);
        CHECK(q2_save_write(&c->snapshot, path) == Q2_OK);
        CHECK(q2_save_read(&loaded, path) == Q2_OK);
        CHECK(client_apply_save(c, &loaded));
        q2_save_free(&loaded);
        CHECK(remove(path) == 0);
    } else {
        CHECK(client_apply_save(c, &c->snapshot));
    }
}

static void test_disc_dish(const char *disc_path, const char *map,
                             const char *output)
{
    client *c = open_client(disc_path);
    q2_event_item call;
    u8 index;
    u32 first, survivors, returning, items, i;
    bool found;
    printf("%s: conditional dish population and save/zone round trips\n", map);
    CHECK(client_load_zone(c, map, 1));
    first = live_group(c, "Zone1");
    CHECK(first > 0 && live_group(c, "Zone1a") == 0);
    CHECK(!strcmp(client_dish_batch(c, 1), "Zone1"));
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *m = &c->creatures.set.monsters[i];
        q2_pop_group g;
        if (m->in_use && q2_pop_get_group(&c->creatures.pop, m->group, &g) &&
            !strcmp(g.name, "Zone1")) {
            q2_monster_damage(m, 30000);
            break;
        }
    }
    survivors = live_group(c, "Zone1");
    CHECK(survivors + 1 == first);
    items = live_items(c);
    CHECK(items > 0);
    if (items) {
        for (i = 0; i < c->sim[0].entities.count; i++) {
            if (c->sim[0].entities.ent[i].in_use) {
                q2_entity_remove(&c->sim[0].entities.ent[i]);
                break;
            }
        }
        items--;
    }
    found = named_call(c, Q2_UF_MISEVENT, "DoneDish", &call, &index);
    CHECK(found);
    if (!found) { close_client(c); return; }
    client_event_call(c, &call, index);
    CHECK(!strcmp(client_dish_batch(c, 1), "Zone1a"));
    CHECK(live_group(c, "Zone1") == survivors && live_group(c, "Zone1a") == 0);
    reload_snapshot(c, output);
    CHECK(live_group(c, "Zone1") == survivors && live_group(c, "Zone1a") == 0);
    CHECK(!strcmp(client_dish_batch(c, 1), "Zone1a"));
    CHECK(client_load_zone(c, map, 0));
    CHECK(client_load_zone(c, map, 1));
    returning = live_group(c, "Zone1a");
    CHECK(returning > 0 && live_group(c, "Zone1") == survivors);
    CHECK(live_items(c) == items); /* Zone1a adds no pickups; history survives. */
    reload_snapshot(c, output);
    CHECK(live_group(c, "Zone1a") == returning && live_group(c, "Zone1") == survivors);
    CHECK(live_items(c) == items);
    CHECK(client_load_zone(c, "BASE2", 0));
    CHECK(client_load_zone(c, map, 1));
    CHECK(live_group(c, "Zone1a") == returning && live_group(c, "Zone1") == survivors);
    CHECK(live_items(c) == items);
    close_client(c);

    c = open_client(disc_path); /* No mission state leaks into a fresh game. */
    CHECK(client_load_zone(c, map, 1));
    CHECK(live_group(c, "Zone1") == first && live_group(c, "Zone1a") == 0);
    close_client(c);
}

static void test_disc_pyramid(const char *disc_path, const char *output)
{
    u32 keys;
    int zone;
    printf("JAIL3: return populations across six zones and four key states\n");
    for (zone = 0; zone < 6; zone++) {
        for (keys = 0; keys < 4; keys++) {
            client *c = open_client(disc_path);
            bool red_return = zone == 4 && (keys & 1);
            bool purple_return = zone == 5 && (keys & 2);
            c->sim[0].combat.inv.flags =
                ((keys & 1) ? Q2_KEY_PYRAMID_RED : 0) |
                ((keys & 2) ? Q2_KEY_PYRAMID_PURPLE : 0);
            c->carry_player = true;
            CHECK(client_load_zone(c, "JAIL3", zone));
            CHECK(group_selected(c, "Jail5Return") == red_return);
            CHECK(group_selected(c, "Jail4Return") == purple_return);
            if (!red_return) CHECK(live_group(c, "Jail5Return") == 0);
            if (!purple_return) CHECK(live_group(c, "Jail4Return") == 0);
            if ((zone == 4 && keys == 1) || (zone == 5 && keys == 2)) {
                const char *name = red_return ? "Jail5Return" : "Jail4Return";
                u32 live = live_group(c, name);
                CHECK(live > 0);
                c->sim[0].combat.inv.flags = 0;
                reload_snapshot(c, output);
                CHECK(group_selected(c, name) && live_group(c, name) == live);
            }
            close_client(c);
        }
    }
    {
        client *c = open_client(disc_path);
        q2_event_item call;
        u8 index;
        bool found;
        const char *orders[] = { "Unit2Curr3", "Unit2Curr3", "Unit2Curr6", "Unit2CurrA" };
        CHECK(client_load_zone(c, "JAIL3", 2));
        found = named_call(c, Q2_UF_MISEVENT, "BothKeys", &call, &index);
        CHECK(found);
        if (found) {
            for (keys = 0; keys < 4; keys++) {
                c->sim[0].combat.inv.flags =
                    ((keys & 1) ? Q2_KEY_PYRAMID_RED : 0) |
                    ((keys & 2) ? Q2_KEY_PYRAMID_PURPLE : 0);
                client_event_call(c, &call, index);
                CHECK(((named_flags(c, "PyramidOpen") & Q2_EVREC_DISABLED) != 0) == (keys == 0));
                CHECK(!strcmp(c->popup.orders_key, orders[keys]));
                if (keys == 0) {
                    u32 raised = c->popup_raises;
                    client_event_call(c, &call, index);
                    CHECK(c->popup_raises == raised);
                }
            }
            reload_snapshot(c, output);
            CHECK(!(named_flags(c, "PyramidOpen") & Q2_EVREC_DISABLED));
        }
        close_client(c);
    }
}

static bool mission_call(client *c, const char *key)
{
    q2_event_item item;
    u8 index;
    bool found = named_call(c, Q2_UF_MISEVENT, key, &item, &index);
    CHECK(found);
    if (found) client_event_call(c, &item, index);
    return found;
}

static bool queued_named(const client *c, const char *name)
{
    const q2_event_rt *rt = &c->sim[0].event_rt;
    u32 i, j;
    for (i = 0; i < rt->events.dir_count; i++) {
        q2_event_dir_entry e;
        if (!q2_events_get_dir_entry(&rt->events, i, &e) || strcmp(e.name, name)) continue;
        for (j = 0; j < rt->pending_count; j++) if (rt->pending[j] == e.offset) return true;
    }
    return false;
}

static s32 named_mover(const client *c, const char *name)
{
    const q2_events *events = &c->sim[0].events;
    u32 i, j, k;
    for (i = 0; i < events->dir_count; i++) {
        q2_event_dir_entry entry;
        q2_event_record rec;
        if (!q2_events_get_dir_entry(events, i, &entry) || strcmp(entry.name, name) ||
            !q2_events_record_at(events, entry.offset, &rec)) continue;
        for (j = 0; j < rec.n_items; j++) {
            q2_event_item item;
            if (!q2_events_get_item(events, &rec, j, &item)) continue;
            for (k = 0; k < c->movers.count; k++)
                if (c->movers.movers[k].item_offset == item.offset) return (s32)k;
        }
    }
    return -1;
}

static void capture_native(client *c, const char *output, const char *name,
                             const s32 eye[3], const s32 target[3])
{
    char path[1024];
    s32 at[3];
    double dx, dy, dz;
    u32 i;
    if (!output) return;
    memcpy(at, eye, sizeof(at));
    if (q2_coll_find_node(&c->sim[0].coll_primary, at, -1, true) < 0) {
        s64 best = INT64_MAX;
        for (i = 0; i < c->sim[0].coll_primary.node_count; i++) {
            q2_coll_node n;
            s32 p[3];
            s64 dist = 0;
            int k;
            if (!q2_collision_get_node(&c->sim[0].coll_primary, i, &n) ||
                q2_collision_node_is_solid(&c->sim[0].coll_primary, i)) continue;
            for (k = 0; k < 3; k++) {
                p[k] = (n.bbox_min[k] + n.bbox_max[k]) / 2;
                dist += (s64)(p[k] - eye[k]) * (p[k] - eye[k]);
            }
            if (dist < best && q2_coll_find_node(&c->sim[0].coll_primary, p, (s32)i, false) >= 0) {
                best = dist; memcpy(at, p, sizeof(at));
            }
        }
    }
    CHECK(c->headless);
    c->sim_enabled = false;
    memcpy(c->cam.pos, at, sizeof(at));
    dx = (double)target[0] - at[0]; dy = (double)target[1] - at[1]; dz = (double)target[2] - at[2];
    c->cam.yaw = (s32)(atan2(dx, dz) * 4096.0 / (2.0 * 3.141592653589793));
    c->cam.pitch = -(s32)(atan2(dy, sqrt(dx*dx + dz*dz)) * 4096.0 / (2.0 * 3.141592653589793));
    c->cam.roll = 0;
    client_native_mission_frame(c, false);
    q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
    client_frame(c);
    CHECK(c->zone.sort != NULL && c->sort_cell >= 0);
    snprintf(path, sizeof(path), "%s/%s.ppm", output, name);
    CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);
}

static void check_bridge(client *c)
{
    q2_scene_node scene;
    q2_trace trace;
    s32 node = client_named_node(c, "DrawBridge");
    const s32 start[3] = {-9000,-7600,7200}, end[3] = {-9000,-6800,7200};
    CHECK(node >= 0 && q2_scene_get_node(&c->zone.scene, (u32)node, &scene));
    CHECK(scene.bbox_min[0] == -10878 && scene.bbox_min[1] == -7039 && scene.bbox_min[2] == 6639);
    CHECK(scene.bbox_max[0] == -7839 && scene.bbox_max[1] == -6839 && scene.bbox_max[2] == 7919);
    q2_sim_trace(&c->sim[0], start, end, &trace);
    CHECK(trace.hit && trace.ent >= 0 && trace.end[1] == -7039);
}

static void test_disc_bridge(const char *disc, const char *output)
{
    client *c = open_client(disc);
    const s32 eye[3] = {-9500,-8000,5800}, target[3] = {-9500,-7000,7400};
    u32 count, i;
    printf("JAIL3: native bridge solidity, culling bounds and restoration\n");
    CHECK(client_load_zone(c, "JAIL3", 1));
    CHECK(!(c->native_mission.flags & Q2_NATIVE_BRIDGE));
    capture_native(c, output, "jail3-bridge-before", eye, target);
    /* Exercise the authored record through normal event dispatch. It must
     * reach both the rotation primitive and the native Bridge callback. */
    CHECK(q2_event_rt_trigger_named(&c->sim[0].event_rt, "DrawBridge"));
    q2_event_rt_update(&c->sim[0].event_rt);
    CHECK(c->native_mission.flags & Q2_NATIVE_BRIDGE);
    check_bridge(c);
    count = c->sim[0].volume_count;
    mission_call(c, "Bridge");
    CHECK(c->sim[0].volume_count == count);
    /* The script owns the rotation; Bridge owns the final solid and bounds. */
    for (i = 0; i < 300; i++) q2_rotators_tick(&c->rotators, 10);
    capture_native(c, output, "jail3-bridge-after", eye, target);
    reload_snapshot(c, output);
    check_bridge(c);
    CHECK(client_load_zone(c, "JAIL3", 0));
    CHECK(client_load_zone(c, "JAIL3", 1));
    check_bridge(c);
    close_client(c);
}

static void test_disc_native_switches(const char *disc, const char *output)
{
    client *c = open_client(disc);
    u32 i;
    s32 platform;
    printf("Native computers, pumps, stairs and platform stop\n");
    CHECK(client_load_zone(c, "COMMAND", 0));
    mission_call(c, "Comp1");
    CHECK(!(c->native_mission.flags & Q2_NATIVE_COMP2));
    reload_snapshot(c, output);
    mission_call(c, "Comp2");
    CHECK(named_flags(c, "TheLasers") & Q2_EVREC_DISABLED);
    CHECK(!(named_flags(c, "LasersOff") & Q2_EVREC_DISABLED));
    CHECK(!strcmp(c->popup.orders_key, "Unit4Curr2"));
    reload_snapshot(c, output);
    CHECK(named_flags(c, "TheLasers") & Q2_EVREC_DISABLED);
    CHECK(client_load_zone(c, "WASTE2", 0));
    mission_call(c, "Pump1On");
    CHECK(!strcmp(c->popup.orders_key, "Unit3Curr3"));
    CHECK(client_load_zone(c, "WASTE3", 0));
    CHECK(c->native_mission.flags & Q2_NATIVE_PUMP1);
    mission_call(c, "CheckPumps");
    CHECK(!queued_named(c, "WasteFill"));
    reload_snapshot(c, output);
    mission_call(c, "Pump2On");
    CHECK(queued_named(c, "WasteFill"));
    CHECK(!strcmp(c->popup.orders_key, "Unit3Curr4"));
    c->sim[0].event_rt.pending_count = 0;
    mission_call(c, "CheckPumps");
    CHECK(queued_named(c, "WasteFill"));
    CHECK(client_load_zone(c, "BOSS1", 0));
    mission_call(c, "Stairs0");
    CHECK(!queued_named(c, "Steps"));
    reload_snapshot(c, output);
    mission_call(c, "Stairs1");
    CHECK(queued_named(c, "Steps"));
    CHECK(client_load_zone(c, "BIGGUN", 0));
    platform = named_mover(c, "PLATFORM");
    for (i = 1; platform < 0 && i < 4; i++) {
        CHECK(client_load_zone(c, "BIGGUN", (int)i));
        platform = named_mover(c, "PLATFORM");
    }
    CHECK(platform >= 0);
    if (platform >= 0) {
        c->movers.movers[platform].wait_timer = 100;
        c->movers.movers[platform].state = Q2_MV_OPENING;
        c->movers.movers[platform].offset = 0;
    }
    mission_call(c, "STOPPLATFORM");
    if (platform >= 0) {
        q2_mover *m = &c->movers.movers[platform];
        CHECK(m->wait_timer == Q2_MOVER_WAIT_NEVER && m->state == Q2_MV_OPENING && m->offset == 0);
        for (i = 0; i < 2000; i++) q2_movers_tick(&c->movers, 10, 0);
        CHECK(m->offset == m->target && m->wait_timer == Q2_MOVER_WAIT_NEVER);
    }
    reload_snapshot(c, output);
    CHECK(c->native_mission.flags & Q2_NATIVE_STOP_PLATFORM);
    CHECK(client_load_zone(c, "JAIL5", 0));
    {
        q2_native_mission before = c->native_mission;
        q2_event_item unused = {0};
        /* Exported by JAIL5 but never referenced by its shipped Events. */
        client_native_mission_call(c, "BUTPRESSED", &unused);
        CHECK(!memcmp(&before, &c->native_mission, sizeof(before)));
    }
    close_client(c);
}

static void test_disc_native_beams(const char *disc, const char *output)
{
    client *c = open_client(disc);
    static const s32 from[3] = {-1521,-1665,4062};
    const s32 eye[3] = {-2700,-1800,3500}, target[3] = {-1511,-1100,3750};
    int zone;
    u32 beams;
    printf("LAB/BOSS1/SECURITY: native beam clocks, independent switches and orb completion\n");
    for (zone = 0; zone < 5; zone++) {
        CHECK(client_load_zone(c, "LAB", zone));
        if (q2_coll_find_node(&c->sim[0].coll_primary, from, -1, true) >= 0) break;
    }
    CHECK(zone < 5);
    mission_call(c, "Laser0");
    CHECK(c->native_mission.lab_phase[0] == 8192);
    c->sim[0].cur_dt = 10;
    client_native_mission_frame(c, true);
    CHECK(c->native_mission.lab_phase[0] == 8172 && c->sim[0].fx.beam_count == 1);
    {
        q2_coll_node cell = {0};
        s32 index = q2_coll_find_node(&c->sim[0].coll_primary, c->sim[0].fx.beam[0].from, -1, true);
        CHECK(index >= 0 && q2_collision_get_node(&c->sim[0].coll_primary, (u32)index, &cell));
        CHECK(index != cell.contents); /* This disc distinguishes the two namespaces. */
        CHECK(c->sim[0].fx.beam[0].area == cell.contents);
    }
    mission_call(c, "Laser0");
    CHECK(c->native_mission.lab_phase[0] == 8172);
    reload_snapshot(c, output);
    CHECK(c->native_mission.lab_phase[0] == 8172);
    mission_call(c, "Laser1");
    CHECK(c->native_mission.lab_phase[1] == 8192);
    capture_native(c, output, "lab-native-sweeps", eye, target);
    {
        s32 phase = c->native_mission.lab_phase[0] - 20;
        s32 end[3] = {-1511 + q2_sin12(phase) * 400 / 4096, -619,
                       3441 + q2_sin12(phase * 8) * 100 / 4096};
        int k;
        for (k = 0; k < 3; k++) c->sim[0].player[0].pos[k] = (from[k] + end[k]) / 2;
        c->sim[0].player[0].pos[1] += Q2_EYE_BASE;
        c->sim[0].combat.inv.health = 100;
        c->sim[0].combat.inv.armour = 0;
        c->sim[0].combat.inv.flags = 0;
        c->sim[0].cur_dt = 10;
        client_native_mission_frame(c, true);
        CHECK(c->sim[0].combat.inv.health == 99);
        client_native_mission_frame(c, false);
        CHECK(c->sim[0].combat.inv.health == 99); /* no second hit on a render-only frame */
    }
    c->native_mission.lab_phase[0] = c->native_mission.lab_phase[1] = 1;
    c->sim[0].cur_dt = 10;
    client_native_mission_frame(c, true);
    CHECK(c->native_mission.lab_phase[0] == -19 && c->native_mission.lab_phase[1] == -19);
    mission_call(c, "Laser0");
    CHECK(c->native_mission.lab_phase[0] == 8192);
    CHECK(client_load_zone(c, "BOSS1", 1));
    c->sim[0].cur_dt = 10;
    c->sim[0].fx.beam_count = 0;
    client_native_mission_frame(c, true);
    beams = c->sim[0].fx.beam_count;
    CHECK(beams == 4);
    {
        const s32 boss_eye[3] = {18000,-18800,-4000}, boss_target[3] = {16300,-17000,640};
        capture_native(c, output, "boss1-native-lasers", boss_eye, boss_target);
    }
    mission_call(c, "LaserButton0");
    c->sim[0].fx.beam_count = 0;
    client_native_mission_frame(c, true);
    CHECK(c->sim[0].fx.beam_count == 2);
    reload_snapshot(c, output);
    mission_call(c, "LaserButton1");
    c->sim[0].fx.beam_count = 0;
    client_native_mission_frame(c, true);
    CHECK(c->sim[0].fx.beam_count == 0);
    CHECK(client_load_zone(c, "SECURITY", 2));
    mission_call(c, "OrbActive");
    c->sim[0].cur_dt = 10;
    c->sim[0].level_time = 0;
    client_native_mission_frame(c, true);
    CHECK(c->sim[0].fx.beam_count == 7);
    CHECK(c->native_mission.orb_arc[0].life > 0);
    {
        const s32 orb_eye[3] = {-3500,-10800,-21700}, orb_target[3] = {-1918,-10000,-23996};
        capture_native(c, output, "security-native-orb", orb_eye, orb_target);
    }
    {
        s32 offset[3];
        q2_rotators_node_offset(&c->rotators, (u32)client_named_node(c, "Orb"), offset);
        CHECK(offset[1] == -100); /* time zero, the native sin/sin bob */
    }
    reload_snapshot(c, output);
    CHECK(c->native_mission.orb_arc[0].life > 0);
    {
        s32 offset[3];
        q2_rotators_node_offset(&c->rotators, (u32)client_named_node(c, "Orb"), offset);
        CHECK(offset[1] == -100); /* reconstructed before the first post-load draw */
    }
    {
        s32 node = client_named_node(c, "Orb");
        u32 raises = c->popup_raises;
        CHECK(node >= 0);
        if (node >= 0) c->node_hidden[node] = 1;
        client_native_mission_frame(c, true);
        CHECK(c->native_mission.flags & Q2_NATIVE_ORB_DONE);
        CHECK(c->popup_raises == raises + 1 && !strcmp(c->popup.orders_key, "Unit2Curr9"));
        client_native_mission_frame(c, true);
        CHECK(c->popup_raises == raises + 1);
    }
    mission_call(c, "LOCKLIFTS");
    CHECK(c->native_mission.flags & Q2_NATIVE_LOCK_LIFTS);
    c->sim[0].event_rt.pending_count = 0;
    client_native_mission_queue(c);
    CHECK(queued_named(c, "LIFTA") && queued_named(c, "LIFTB"));
    reload_snapshot(c, output);
    CHECK(c->native_mission.flags & Q2_NATIVE_LOCK_LIFTS);
    close_client(c);
}

static void test_disc_selectors(const char *disc_path, const char *output)
{
    static const char *const maps[] = { "COMMAND", "SECURITY", "BOSS2" };
    u32 mi;
    for (mi = 0; mi < Q2PSX_ARRAY_COUNT(maps); mi++) {
        client *c = open_client(disc_path);
        u32 gi, slot;
        CHECK(client_load_zone(c, maps[mi], mi ? 1 : 4));
        for (gi = 0; gi < c->creatures.pop.group_count; gi++) {
            q2_pop_group group;
            q2_pop_spawn spawn;
            CHECK(q2_pop_get_group(&c->creatures.pop, gi, &group));
            for (slot = 0; q2_pop_get_spawn(&c->creatures.pop, &group, slot, &spawn); slot++)
                if (spawn.flags & 0x600)
                    printf("TAG %s %s/%u class %u flags %04X at %d,%d,%d\n",
                           maps[mi], group.name, slot, spawn.class_id, spawn.flags,
                           spawn.x, spawn.y, spawn.z);
        }
        close_client(c);
    }
    {
        client *c = open_client(disc_path);
        u32 i;
        c->carry_player = true;
        c->sim[0].combat.inv.flags = Q2_KEY_WHITE;
        CHECK(client_load_zone(c, "COMMAND", 4));
        CHECK(!group_selected(c, "Zone4"));
        CHECK(live_group(c, "Zone4") == 0);
        CHECK(live_items(c) == 0);
        reload_snapshot(c, output);
        CHECK(!group_selected(c, "Zone4") && live_items(c) == 0);
        printf("COMMAND white key: %u live creatures, %u items\n",
               live_group(c, "Zone4"), live_items(c));
        for (i = 0; i < c->creatures.set.count; i++) {
            q2_monster *m = &c->creatures.set.monsters[i];
            q2_pop_group group;
            if (q2_pop_get_group(&c->creatures.pop, m->group, &group) &&
                !strcmp(group.name, "Zone4")) {
                s32 eye[3] = { m->pos[0] + 1000, m->pos[1] - 400, m->pos[2] + 1500 };
                s32 target[3] = { m->pos[0], m->pos[1] - 100, m->pos[2] };
                capture_native(c, output, "command-white-key", eye, target);
                break;
            }
        }
        close_client(c);
    }
    {
        client *c = open_client(disc_path);
        u32 creatures, items;
        c->carry_player = true;
        c->sim[0].combat.inv.flags = Q2_KEY_MASK & ~Q2_KEY_WHITE;
        CHECK(client_load_zone(c, "COMMAND", 4));
        creatures = live_group(c, "Zone4"); items = live_items(c);
        CHECK(group_selected(c, "Zone4") && creatures == 4 && items == 3);
        /* Acquiring the key after entry does not despawn the room or change
         * the resident save's logical allocation prefix. */
        c->sim[0].combat.inv.flags |= Q2_KEY_WHITE;
        reload_snapshot(c, output);
        CHECK(group_selected(c, "Zone4") && live_group(c, "Zone4") == creatures);
        CHECK(live_items(c) == items);
        c->carry_player = c->carry_same_map = true;
        CHECK(client_load_zone(c, "COMMAND", 3));
        c->carry_player = c->carry_same_map = true;
        CHECK(client_load_zone(c, "COMMAND", 4));
        CHECK(live_group(c, "Zone4") == creatures && live_items(c) == items);
        close_client(c);
    }
}

static q2_monster *population_actor(client *c, const char *name, u32 slot)
{
    u32 i;
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *m = &c->creatures.set.monsters[i];
        q2_pop_group g;
        if (m->population_slot == slot &&
            q2_pop_get_group(&c->creatures.pop, m->group, &g) && !strcmp(g.name, name))
            return m;
    }
    CHECK(false);
    return NULL;
}

static void finish_population_death(q2_monster *m)
{
    q2_monster_set one = {0};
    int tick;
    if (!m) return;
    CHECK(m->in_use && m->dead);
    one.monsters = m; one.count = 1;
    for (tick = 0; tick < 180 && m->in_use && !m->corpse; tick++)
        q2_monster_set_tick(&one);
    CHECK(m->dead && (m->corpse || !m->in_use));
}

static void kill_population_actor(q2_monster *m)
{
    s16 damage;
    CHECK(m && m->in_use && !m->dead && m->health > 0);
    if (!m || !m->in_use || m->dead) return;
    damage = (s16)(m->health + 1);
    m->health = -1;
    q2_monster_damage_reaction(m, NULL, damage);
    CHECK(m->dead && m->deadflag == Q2_DEAD_DEAD);
}

static client *cold_population_save(client *c, const char *disc_path,
                                       const char *output)
{
    q2_save saved = {0};
    reload_snapshot(c, output); /* Include the disk codec when requested. */
    CHECK(client_capture(c));
    saved = c->snapshot;
    memset(&c->snapshot, 0, sizeof(c->snapshot));
    close_client(c);
    c = open_client(disc_path);
    CHECK(client_apply_save(c, &saved));
    q2_save_free(&saved);
    return c;
}

static void test_disc_laserwall(const char *disc_path, const char *map, const char *output)
{
    static const s32 from[2][3] = {{7048,2320,4000}, {7048,2820,4000}};
    static const s32 to[2][3] = {{12018,2320,4000}, {12018,2820,4000}};
    const s32 eye[3] = {9500,2000,2000}, target[3] = {9500,2600,4000};
    client *c = open_client(disc_path);
    q2_monster *gunner;
    q2_rng rng;
    q2_coll_node cell;
    q2_input idle = {0};
    char name[64];
    int zone, k;
    u32 i, hazards = 0;
    printf("%s: laser-wall rows, damage, tagged Gunner and cold saves\n", map);
    for (zone = 0; zone < 3; zone++) {
        CHECK(client_load_zone(c, map, zone));
        CHECK(c->laserwalls.count == 0 && c->laserwalls.declined == 1);
        for (i = 0; i < c->sim[0].triggers.count; i++)
            CHECK(c->sim[0].volume_mod[i] != Q2_MOD_LASER);
        client_native_mission_frame(c, true);
        client_laserwalls_frame(c, false);
        CHECK(!c->sim[0].fx.beam_count);
        CHECK(!(c->native_mission.flags & Q2_NATIVE_BASE3_WALL_OFF));
    }
    CHECK(client_load_zone(c, map, 3));
    CHECK(c->laserwalls.count == 1 && c->laserwalls.declined == 0);
    CHECK(!(named_flags(c, "LASERWALL1") & Q2_EVREC_DISABLED));
    rng = c->sim[0].fx_rng;
    client_laserwalls_frame(c, false);
    CHECK(c->sim[0].fx.beam_count == 2);
    CHECK(!memcmp(&rng, &c->sim[0].fx_rng, sizeof(rng)));
    CHECK(q2_collision_get_node(&c->sim[0].coll_primary,
        (u32)q2_coll_find_node(&c->sim[0].coll_primary, from[0], -1, true), &cell));
    for (i = 0; i < c->sim[0].fx.beam_count && i < 2; i++) {
        const q2_fx_beam *beam = &c->sim[0].fx.beam[i];
        CHECK(!memcmp(beam->from, from[i], sizeof(beam->from)));
        CHECK(!memcmp(beam->to, to[i], sizeof(beam->to)));
        CHECK(beam->radius == 16 && beam->tube == c->fx_tables.beam[0].tube);
        CHECK(beam->area == cell.contents);
    }
    snprintf(name, sizeof(name), "%s-laserwall-after", map);
    capture_native(c, output, name, eye, target);
    if (output) {
        printf("  capture eye %d,%d,%d; %u beam faces\n", c->cam.pos[0], c->cam.pos[1],
            c->cam.pos[2], c->sim[0].fx.stats.beam_faces_emitted);
        CHECK(c->sim[0].fx.stats.beam_faces_emitted > 0);
    }
    c->sim_enabled = true;
    c->sim[0].combat.inv.health = 2000;
    c->sim[0].combat.inv.armour = 0;
    c->sim[0].combat.inv.flags = 0;
    for (k = 0; k < 3; k++) c->sim[0].player[0].pos[k] = (from[0][k] + to[0][k]) / 2;
    c->sim[0].player[0].pos[1] += Q2_EYE_BASE;
    c->sim[0].fx.beam_count = 0;
    client_laserwalls_frame(c, true);
    CHECK(c->sim[0].combat.inv.health == 1488);
    client_laserwalls_frame(c, false);
    CHECK(c->sim[0].combat.inv.health == 1488);
    c->no_lasers = true;
    c->sim[0].fx.beam_count = 0;
    client_laserwalls_frame(c, true);
    CHECK(!c->sim[0].fx.beam_count && c->sim[0].combat.inv.health == 976);
    c->no_lasers = false;
    CHECK(q2_event_rt_disable_named(&c->sim[0].event_rt, "LASERWALL1", true));
    client_laserwalls_frame(c, true);
    CHECK(!c->sim[0].fx.beam_count && c->sim[0].combat.inv.health == 976);
    CHECK(q2_event_rt_disable_named(&c->sim[0].event_rt, "LASERWALL1", false));
    /* The trigger contact is distinct from the two 512-damage actor sweeps.
     * Exercise the normal simulation walk without the client's beam pass. */
    for (i = 0; i < c->sim[0].triggers.count; i++) {
        q2_trigger t;
        if (!q2_trigger_get(&c->sim[0].triggers, i, &t) ||
            !c->laserwalls.count || t.event_offset != c->laserwalls.wall[0].raiser) continue;
        hazards++;
        CHECK(c->sim[0].volume_damage[i] == 200 && c->sim[0].volume_mod[i] == Q2_MOD_LASER);
        for (k = 0; k < 3; k++) c->sim[0].player[0].pos[k] = (t.min[k] + t.max[k]) / 2;
        c->sim[0].player[0].pos[1] += Q2_EYE_BASE;
        c->sim[0].combat.inv.health = 2000;
        q2_sim_tick(&c->sim[0], &idle, 10);
        CHECK(c->sim[0].combat.inv.health == 1800);
        CHECK(q2_event_rt_disable_named(&c->sim[0].event_rt, "LASERWALL1", true));
        q2_sim_tick(&c->sim[0], &idle, 10);
        CHECK(c->sim[0].combat.inv.health == 1800);
        CHECK(q2_event_rt_disable_named(&c->sim[0].event_rt, "LASERWALL1", false));
    }
    CHECK(hazards == 1);
    c = cold_population_save(c, disc_path, output);
    c->sim[0].fx.beam_count = 0;
    client_laserwalls_frame(c, false);
    CHECK(c->sim[0].fx.beam_count == 2);
    CHECK(!(c->native_mission.flags & Q2_NATIVE_BASE3_WALL_OFF));
    gunner = population_actor(c, "Zone3", 1);
    if (gunner) kill_population_actor(gunner);
    client_native_mission_frame(c, true);
    CHECK(!(named_flags(c, "LASERWALL1") & Q2_EVREC_DISABLED));
    c = cold_population_save(c, disc_path, output);
    gunner = population_actor(c, "Zone3", 1);
    CHECK(gunner && gunner->dead && !gunner->corpse);
    client_native_mission_frame(c, true);
    CHECK(!(named_flags(c, "LASERWALL1") & Q2_EVREC_DISABLED));
    if (gunner) finish_population_death(gunner);
    client_native_mission_frame(c, false);
    CHECK(!(named_flags(c, "LASERWALL1") & Q2_EVREC_DISABLED));
    client_native_mission_frame(c, true);
    CHECK(c->native_mission.flags & Q2_NATIVE_BASE3_WALL_OFF);
    CHECK(named_flags(c, "LASERWALL1") & Q2_EVREC_DISABLED);
    c->sim[0].fx.beam_count = 0;
    client_laserwalls_frame(c, true);
    CHECK(!c->sim[0].fx.beam_count);
    CHECK(q2_event_rt_disable_named(&c->sim[0].event_rt, "LASERWALL1", false));
    client_native_mission_frame(c, true);
    CHECK(named_flags(c, "LASERWALL1") & Q2_EVREC_DISABLED);
    c = cold_population_save(c, disc_path, output);
    CHECK(named_flags(c, "LASERWALL1") & Q2_EVREC_DISABLED);
    CHECK(client_load_zone(c, map, 2));
    CHECK(!c->laserwalls.count);
    CHECK(client_load_zone(c, "BASE1", 0));
    CHECK(!c->laserwalls.count);
    CHECK(client_load_zone(c, map, 3));
    CHECK(named_flags(c, "LASERWALL1") & Q2_EVREC_DISABLED);
    client_laserwalls_frame(c, false);
    CHECK(!c->sim[0].fx.beam_count);
    snprintf(name, sizeof(name), "%s-laserwall-complete", map);
    capture_native(c, output, name, eye, target);
    close_client(c);

    /* Retail's departure sweep completes a dying actor's tagged kill too. */
    c = open_client(disc_path);
    CHECK(client_load_zone(c, map, 3));
    c->sim_enabled = true;
    client_input_simulated(c, 1.0f / 30.0f);
    CHECK(c->sim[0].fx.beam_count == 2);
    q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
    client_frame(c); /* The render pass consumes the transient beam pool. */
    client_input_simulated(c, 1.0f / 30.0f);
    CHECK(c->sim[0].fx.beam_count == 2);
    gunner = population_actor(c, "Zone3", 1);
    if (gunner) kill_population_actor(gunner);
    CHECK(client_load_zone(c, map, 2));
    client_native_mission_frame(c, true);
    CHECK(c->native_mission.flags & Q2_NATIVE_BASE3_WALL_OFF);
    c = cold_population_save(c, disc_path, output);
    CHECK(client_load_zone(c, map, 3));
    CHECK(named_flags(c, "LASERWALL1") & Q2_EVREC_DISABLED);
    close_client(c);
}

static void test_disc_boss_entrance(const char *disc_path, const char *output)
{
    static const char *const names[5] = {"LaserNW", "LaserNE", "LaserSW", "LaserSE", "LaserMid"};
    const s32 eye[3] = {-1500,-10700,2000}, target[3] = {650,-10450,650};
    client *c = open_client(disc_path);
    const dat_chunk *lb;
    s32 from[5][3], to[5][3];
    u32 i, mask, expected;
    int k;
    printf("BOSS1 entrance: five native lasers, script gates, damage and cold saves\n");
    CHECK(client_load_zone(c, "BOSS1", 0));
    lb = c->common.chunk[Q2_COMMON_LEVEL_BIN];
    CHECK(lb && lb->size >= 0x1F24);
    if (!lb || lb->size < 0x1F24) { close_client(c); return; }
    /* The real module supplies the order and endpoints, independently of
     * the client's implementation. +0x1708 emits kind 4 with ends=0. */
    for (i = 0; i < 5; i++) {
        CHECK(!strcmp((const char *)lb->data + 0x1E6C + 12*i, names[i]));
        for (k = 0; k < 3; k++) {
            from[i][k] = q2_rd_s32(lb->data + 0x1EA8 + 12*i + 4*k);
            to[i][k] = q2_rd_s32(lb->data + 0x1EE4 + 12*i + 4*k);
        }
        CHECK(q2_coll_find_node(&c->sim[0].coll_primary, from[i], -1, true) >= 0);
    }
    c->sim[0].cur_dt = 10;
    CHECK(q2_event_rt_disable_named(&c->sim[0].event_rt, "E_NearMakron", true));
    CHECK(q2_event_rt_disable_named(&c->sim[0].event_rt, "DoorBlown", false));
    for (i = 0; i < 5; i++) CHECK(q2_event_rt_disable_named(&c->sim[0].event_rt, names[i], false));
    /* Exercise all 32 combinations: a disabled named record turns its beam
     * ON, opposite to ordinary LASERBEAM raisers. */
    for (mask = 0; mask < 32; mask++) {
        q2_rng rng = c->sim[0].fx_rng;
        s32 phases[2];
        memcpy(phases, c->native_mission.boss_phase, sizeof(phases));
        for (i = 0; i < 5; i++)
            CHECK(q2_event_rt_disable_named(&c->sim[0].event_rt, names[i], (mask & (1u << i)) != 0));
        c->sim[0].fx.beam_count = 0;
        client_native_mission_frame(c, false);
        expected = 0;
        for (i = 0; i < 5; i++) if (mask & (1u << i)) {
            if (expected < c->sim[0].fx.beam_count) {
                const q2_fx_beam *b = &c->sim[0].fx.beam[expected];
                q2_coll_node node;
                s32 cell = q2_coll_find_node(&c->sim[0].coll_primary, from[i], -1, true);
                CHECK(!memcmp(b->from, from[i], sizeof(b->from)));
                CHECK(!memcmp(b->to, to[i], sizeof(b->to)));
                CHECK(b->radius == 64 && b->tube == c->fx_tables.beam[0].tube);
                CHECK(q2_collision_get_node(&c->sim[0].coll_primary, (u32)cell, &node));
                CHECK(b->area == node.contents);
            }
            expected++;
        }
        CHECK(c->sim[0].fx.beam_count == expected);
        CHECK(!memcmp(&rng, &c->sim[0].fx_rng, sizeof(rng)));
        CHECK(!memcmp(phases, c->native_mission.boss_phase, sizeof(phases)));
        CHECK(!(c->native_mission.flags & Q2_NATIVE_PROJECTION_USED));
    }
    capture_native(c, output, "boss1-entrance", eye, target);
    if (output) printf("  capture eye %d,%d,%d; %u beam faces\n",
        c->cam.pos[0], c->cam.pos[1], c->cam.pos[2], c->sim[0].fx.stats.beam_faces_emitted);
    if (output) CHECK(c->sim[0].fx.stats.beam_faces_emitted > 0);
    c->sim_enabled = true;
    CHECK(q2_event_rt_disable_named(&c->sim[0].event_rt, "E_NearMakron", false));
    for (i = 0; i < 5; i++)
        CHECK(q2_event_rt_disable_named(&c->sim[0].event_rt, names[i], i == 0));
    for (k = 0; k < 3; k++) c->sim[0].player[0].pos[k] = (from[0][k] + to[0][k]) / 2;
    c->sim[0].player[0].pos[1] += Q2_EYE_BASE;
    c->sim[0].combat.inv.health = 2000;
    c->sim[0].combat.inv.armour = 0;
    c->sim[0].combat.inv.flags = 0;
    c->sim[0].fx.beam_count = 0;
    client_native_mission_frame(c, true);
    CHECK(c->sim[0].fx.beam_count == 1 && c->sim[0].combat.inv.health == 1488);
    client_native_mission_frame(c, false);
    CHECK(c->sim[0].combat.inv.health == 1488);
    c->no_lasers = true;
    c->sim[0].fx.beam_count = 0;
    client_native_mission_frame(c, true);
    CHECK(!c->sim[0].fx.beam_count && c->sim[0].combat.inv.health == 976);
    c->no_lasers = false;
    c = cold_population_save(c, disc_path, output);
    c->sim[0].fx.beam_count = 0;
    client_native_mission_frame(c, false);
    CHECK(c->sim[0].fx.beam_count == 1 && c->sim[0].combat.inv.health == 976);
    CHECK(!(c->native_mission.flags & Q2_NATIVE_PROJECTION_USED));
    CHECK(q2_event_rt_disable_named(&c->sim[0].event_rt, "DoorBlown", true));
    c->sim[0].fx.beam_count = 0;
    client_native_mission_frame(c, true);
    CHECK(!c->sim[0].fx.beam_count && c->sim[0].combat.inv.health == 976);
    c = cold_population_save(c, disc_path, output);
    c->sim[0].fx.beam_count = 0;
    client_native_mission_frame(c, false);
    CHECK(!c->sim[0].fx.beam_count);
    close_client(c);

    /* Reach the same states through the disc's one-shot records, then carry
     * them through disk saves and a departure/return before blowing the door. */
    c = open_client(disc_path);
    CHECK(client_load_zone(c, "BOSS1", 0));
    CHECK(!(named_flags(c, "DoorBlown") & Q2_EVREC_DISABLED));
    for (i = 0; i < 5; i++) {
        CHECK(!(named_flags(c, names[i]) & Q2_EVREC_DISABLED));
        CHECK(q2_event_rt_trigger_named(&c->sim[0].event_rt, names[i]));
        CHECK(q2_event_rt_update(&c->sim[0].event_rt) == Q2_EVENT_OK);
        CHECK(named_flags(c, names[i]) & Q2_EVREC_DISABLED);
        c->sim[0].fx.beam_count = 0;
        client_native_mission_frame(c, false);
        CHECK(c->sim[0].fx.beam_count == i + 1);
        if (i == 1) c = cold_population_save(c, disc_path, output);
        if (i == 2) {
            c->carry_player = c->carry_same_map = true;
            CHECK(client_load_zone(c, "BOSS1", 1));
            c->carry_player = c->carry_same_map = true;
            CHECK(client_load_zone(c, "BOSS1", 0));
        }
        c->sim[0].fx.beam_count = 0;
        client_native_mission_frame(c, false);
        CHECK(c->sim[0].fx.beam_count == i + 1);
    }
    CHECK(q2_event_rt_trigger_named(&c->sim[0].event_rt, "DoorBlown"));
    CHECK(q2_event_rt_update(&c->sim[0].event_rt) == Q2_EVENT_OK);
    CHECK(named_flags(c, "DoorBlown") & Q2_EVREC_DISABLED);
    c->sim[0].fx.beam_count = 0;
    client_native_mission_frame(c, false);
    CHECK(!c->sim[0].fx.beam_count);
    c = cold_population_save(c, disc_path, output);
    c->sim[0].fx.beam_count = 0;
    client_native_mission_frame(c, false);
    CHECK(!c->sim[0].fx.beam_count);
    close_client(c);
}

static void test_disc_security_progression(const char *disc_path, const char *output)
{
    client *c = open_client(disc_path);
    q2_monster *m;
    u32 spawned, hidden;
    s32 timers[4];
    const s32 eye[3] = { -2850, -11900, -10900 };
    const s32 target[3] = { -1917, -11900, -13452 };
    printf("SECURITY: two guard deaths, boss arrival, wall sequence and exit hatch\n");
    CHECK(client_load_zone(c, "SECURITY", 1));
    CHECK(!group_selected(c, "Boss1") && live_group(c, "Boss1") == 0);
    c->sim[0].cur_dt = 10;
    client_native_mission_frame(c, true);
    CHECK(!group_selected(c, "Boss1"));
    capture_native(c, output, "security-before-boss", eye, target);
    m = population_actor(c, "Zone1", 0);
    if (!m) { close_client(c); return; }
    kill_population_actor(m);
    finish_population_death(m);
    client_native_mission_frame(c, true);
    CHECK(!group_selected(c, "Boss1") && client_population_tag_deaths(c, 0x200) == 1);
    c = cold_population_save(c, disc_path, output);
    CHECK(client_load_zone(c, "SECURITY", 0));
    CHECK(client_load_zone(c, "SECURITY", 1));
    CHECK(client_population_tag_deaths(c, 0x200) == 1 && !group_selected(c, "Boss1"));
    m = population_actor(c, "Zone1", 1);
    if (!m) { close_client(c); return; }
    kill_population_actor(m);
    c->sim[0].cur_dt = 10;
    client_native_mission_frame(c, true);
    CHECK(!group_selected(c, "Boss1")); /* Dying is not a completed kill. */
    finish_population_death(m);
    client_native_mission_frame(c, false);
    CHECK(!group_selected(c, "Boss1")); /* Rendering does not advance gameplay. */
    client_native_mission_frame(c, true);
    CHECK(group_selected(c, "Boss1") && live_group(c, "Boss1") == 1);
    CHECK((c->native_mission.flags & Q2_NATIVE_SECURITY_BOSS) != 0);
    CHECK(c->native_mission.security_wall[0] == 590 &&
          c->native_mission.security_wall[3] == 1940);
    capture_native(c, output, "security-boss-arrived", eye, target);
    spawned = c->script_summoned;
    client_native_mission_frame(c, true);
    CHECK(c->script_summoned == spawned && live_group(c, "Boss1") == 1);
    memcpy(timers, c->native_mission.security_wall, sizeof(timers));
    c = cold_population_save(c, disc_path, output);
    CHECK(!memcmp(timers, c->native_mission.security_wall, sizeof(timers)));
    CHECK(live_group(c, "Boss1") == 1);
    c->carry_player = c->carry_same_map = true;
    CHECK(client_load_zone(c, "SECURITY", 0));
    c->sim[0].cur_dt = 200;
    client_native_mission_frame(c, true);
    CHECK(!memcmp(timers, c->native_mission.security_wall, sizeof(timers)));
    c->carry_player = c->carry_same_map = true;
    CHECK(client_load_zone(c, "SECURITY", 1));
    c->sim[0].event_rt.pending_count = 0;
    c->sim[0].cur_dt = timers[0] - 1;
    client_native_mission_frame(c, true);
    CHECK(!queued_named(c, "ExplodeWall1") && c->native_mission.security_wall[0] == 1);
    c->sim[0].cur_dt = 1;
    client_native_mission_frame(c, true);
    CHECK(queued_named(c, "ExplodeWall1") && !queued_named(c, "ExplodeWall2"));
    c->sim[0].cur_dt = 450;
    client_native_mission_frame(c, true);
    CHECK(queued_named(c, "ExplodeWall2") && !queued_named(c, "ExplodeWall3"));
    client_native_mission_frame(c, true);
    CHECK(queued_named(c, "ExplodeWall3") && !queued_named(c, "ExplodeWall4"));
    client_native_mission_frame(c, true);
    CHECK(queued_named(c, "ExplodeWall4"));
    c = cold_population_save(c, disc_path, output);
    CHECK(queued_named(c, "ExplodeWall1") && queued_named(c, "ExplodeWall4"));
    hidden = hidden_nodes(c);
    CHECK(q2_event_rt_update(&c->sim[0].event_rt) == Q2_EVENT_OK);
    CHECK((named_flags(c, "ExplodeWall1") & Q2_EVREC_HASRUN) &&
          (named_flags(c, "ExplodeWall4") & Q2_EVREC_HASRUN));
    /* Two headless frames cross a PAL logic tick and drain geometry swaps. */
    client_input_simulated(c, 1.0f / 30.0f);
    client_input_simulated(c, 1.0f / 30.0f);
    CHECK(hidden_nodes(c) > hidden);
    client_native_mission_frame(c, true);
    CHECK(!queued_named(c, "ExplodeWall1") && !queued_named(c, "ExplodeWall4"));
    m = population_actor(c, "Boss1", 0);
    if (!m) { close_client(c); return; }
    kill_population_actor(m);
    c->sim[0].cur_dt = 10;
    client_native_mission_frame(c, true);
    CHECK(!queued_named(c, "ExplodeHatch"));
    finish_population_death(m);
    client_native_mission_frame(c, true);
    CHECK(queued_named(c, "ExplodeHatch") &&
          (c->native_mission.flags & Q2_NATIVE_SECURITY_HATCH));
    c = cold_population_save(c, disc_path, output);
    CHECK(queued_named(c, "ExplodeHatch") && live_group(c, "Boss1") == 0);
    hidden = hidden_nodes(c);
    CHECK(q2_event_rt_update(&c->sim[0].event_rt) == Q2_EVENT_OK);
    CHECK(named_flags(c, "ExplodeHatch") & Q2_EVREC_HASRUN);
    client_input_simulated(c, 1.0f / 30.0f);
    client_input_simulated(c, 1.0f / 30.0f);
    CHECK(hidden_nodes(c) > hidden);
    c = cold_population_save(c, disc_path, output);
    client_native_mission_frame(c, true);
    CHECK(!queued_named(c, "ExplodeHatch") && live_group(c, "Boss1") == 0);
    CHECK(named_flags(c, "ExplodeHatch") & Q2_EVREC_HASRUN);
    close_client(c);
}

static void test_disc_train_unlock(const char *disc_path, const char *output,
                                  bool depart_before_callback)
{
    client *c = open_client(disc_path);
    q2_monster *jorg, *rider = NULL;
    u32 i, raises;
    printf("BOSS2: second-stage defeat unlocks the train, once, %s\n",
           depart_before_callback ? "after departing with a dying rider" :
                                    "through save and zone return");
    CHECK(client_load_zone(c, "BOSS2", 1));
    jorg = population_actor(c, "Zone1", 0);
    if (!jorg) { close_client(c); return; }
    for (i = 0; i < c->creatures.set.count; i++)
        if (c->creatures.set.monsters[i].owner == jorg &&
            c->creatures.set.monsters[i].group >= c->creatures.pop.group_count)
            rider = &c->creatures.set.monsters[i];
    CHECK(rider != NULL && !rider->population_active);
    if (!rider) { close_client(c); return; }
    c->sim[0].cur_dt = 10;
    client_native_mission_frame(c, true);
    CHECK(!(c->native_mission.flags & Q2_NATIVE_TRAIN_OPEN));
    CHECK(named_flags(c, "DoorToTrain") & Q2_EVREC_DISABLED);
    kill_population_actor(jorg);
    finish_population_death(jorg);
    CHECK(rider->population_active && rider->in_use);
    client_native_mission_frame(c, true);
    CHECK(!(c->native_mission.flags & Q2_NATIVE_TRAIN_OPEN));
    CHECK(named_flags(c, "DoorToTrain") & Q2_EVREC_DISABLED);
    /* Finish the child through its own die path; no synthetic event trigger. */
    kill_population_actor(rider);
    if (depart_before_callback) {
        CHECK(client_load_zone(c, "BOSS2", 0));
        CHECK(!(c->native_mission.flags & Q2_NATIVE_TRAIN_OPEN));
        c = cold_population_save(c, disc_path, output);
        CHECK(client_rider_death_complete(c));
    } else {
        finish_population_death(rider);
    }
    raises = c->popup_raises;
    client_native_mission_frame(c, true);
    CHECK(c->native_mission.flags & Q2_NATIVE_TRAIN_OPEN);
    CHECK(!(named_flags(c, "DoorToTrain") & Q2_EVREC_DISABLED));
    CHECK(named_flags(c, "DoorString") & Q2_EVREC_DISABLED);
    CHECK(group_selected(c, "Zone4"));
    CHECK(c->popup_raises == raises + 1 && !strcmp(c->popup.orders_key, "Unit5Curr4"));
    client_native_mission_frame(c, true);
    CHECK(c->popup_raises == raises + 1);
    c = cold_population_save(c, disc_path, output);
    CHECK(c->native_mission.flags & Q2_NATIVE_TRAIN_OPEN);
    CHECK(client_load_zone(c, "BOSS2", 0));
    CHECK(!(named_flags(c, "DoorToTrain") & Q2_EVREC_DISABLED));
    CHECK(named_flags(c, "DoorString") & Q2_EVREC_DISABLED);
    CHECK(client_load_zone(c, "BOSS1", 0));
    CHECK(client_load_zone(c, "BOSS2", 0));
    CHECK(!(named_flags(c, "DoorToTrain") & Q2_EVREC_DISABLED));
    CHECK(named_flags(c, "DoorString") & Q2_EVREC_DISABLED);
    close_client(c);
}

static u32 native_explosions(const client *c)
{
    u32 i, count = 0;
    for (i = 0; i < c->sim[0].entities.count; i++) {
        const q2_entity *e = &c->sim[0].entities.ent[i];
        if (e->in_use && !strcmp(e->model, "Explosion")) count++;
    }
    return count;
}

static void test_disc_power1(const char *disc_path, const char *output)
{
    const s32 entrance_at[2][3] = {{1975,-6500,9653}, {-1880,-6500,9653}};
    const s32 shaft_eye[3] = {-600,4000,-11000}, shaft_at[3] = {-600,4600,-13120};
    client *c = open_client(disc_path);
    u32 count, i, j;
    printf("POWER1: recurring entrance explosions and climbing shaft hazard\n");
    CHECK(client_load_zone(c, "POWER1", 0));
    {
        const dat_chunk *lb = c->common.chunk[Q2_COMMON_LEVEL_BIN];
        CHECK(lb && lb->size >= 0xC5C);
        if (lb && lb->size >= 0xC5C) {
            for (i = 0; i < 2; i++) for (j = 0; j < 3; j++)
                CHECK(q2_rd_s32(lb->data + 0xBD0 + i*12 + j*4) == entrance_at[i][j]);
            for (i = 0; i < 8; i++) {
                CHECK(q2_rd_s32(lb->data + 0xBFC + i*12) == -600);
                CHECK(q2_rd_s32(lb->data + 0xC00 + i*12) == 5200 - (s32)i*600);
                CHECK(q2_rd_s32(lb->data + 0xC04 + i*12) == -13120);
            }
        }
    }
    /* The counter's named target is absent, so enabling it cannot advance
     * this map. Do not manufacture an event or a kill-counter dependency. */
    for (i = 0; i < c->sim[0].events.dir_count; i++) {
        q2_event_dir_entry entry;
        CHECK(q2_events_get_dir_entry(&c->sim[0].events, i, &entry));
        CHECK(strcmp(entry.name, "MakeBlitzes") != 0);
    }
    for (i = 0; i < 2; i++)
        CHECK(q2_coll_find_node(&c->sim[0].coll_primary, entrance_at[i], -1, true) == -1);
    c->sim[0].cur_dt = 10;
    c->sim[0].level_time = 0;
    count = native_explosions(c);
    client_native_mission_frame(c, true);
    CHECK(native_explosions(c) == count);
    c->sim[0].level_time = 1;
    client_native_mission_frame(c, true);
    CHECK(native_explosions(c) == count + 2);
    CHECK(c->native_mission.power1_next_blast == 901);
    CHECK(c->sim[0].explosion_sounds.count == 2);
    for (i = 0, j = 0; i < c->sim[0].entities.count; i++) {
        const q2_entity *e = &c->sim[0].entities.ent[i];
        if (!e->in_use || strcmp(e->model, "Explosion")) continue;
        CHECK(e->surface == (u8)c->sim[0].coll_primary.node_count);
        CHECK(j < 2 && !memcmp(e->pos, entrance_at[j], sizeof(e->pos)));
        j++;
    }
    c->sim[0].level_time = 901;
    client_native_mission_frame(c, true);
    CHECK(native_explosions(c) == count + 2);
    c->sim[0].level_time = 902;
    client_native_mission_frame(c, true);
    CHECK(native_explosions(c) == count + 4);
    c->sim[0].level_time = 100000;
    client_native_mission_frame(c, true);
    CHECK(native_explosions(c) == count + 6 && c->native_mission.power1_next_blast == 100900);
    {
        q2_native_mission state = c->native_mission;
        count = native_explosions(c);
        c->sim[0].level_time = 200000;
        client_native_mission_frame(c, false);
        c->mp_enabled = true;
        client_native_mission_frame(c, true);
        c->mp_enabled = false;
        c->restoring_save = &c->snapshot;
        client_native_mission_frame(c, true);
        c->restoring_save = NULL;
        c->sim[0].event_rt.initial_pass = true;
        client_native_mission_frame(c, true);
        c->sim[0].event_rt.initial_pass = false;
        c->sim[0].cur_dt = 0;
        client_native_mission_frame(c, true);
        CHECK(!memcmp(&state, &c->native_mission, sizeof(state)) && native_explosions(c) == count);
    }
    CHECK(client_load_zone(c, "POWER1", 1));
    c->sim[0].cur_dt = 10;
    c->sim[0].level_time = 1;
    memcpy(c->sim[0].player[0].pos, shaft_at, sizeof(shaft_at));
    c->sim[0].player[0].pos[1] += Q2_EYE_BASE;
    c->sim[0].combat.inv.health = 100;
    count = native_explosions(c);
    client_native_mission_frame(c, true);
    CHECK(native_explosions(c) == count + 1);
    CHECK(c->sim[0].combat.inv.health == 70);
    CHECK(c->sim[0].combat.self.last_mod == Q2_MOD_LAVA);
    CHECK(c->sim[0].combat.self.env_next == 101);
    CHECK(c->sim[0].combat.self.damage_source == &c->sim[0].combat.self);
    CHECK(c->native_mission.power1_blast_phase == 1 && c->native_mission.power1_next_blast == 101);
    for (i = 0; i < c->sim[0].entities.count; i++) {
        const q2_entity *e = &c->sim[0].entities.ent[i];
        if (e->in_use && !strcmp(e->model, "Explosion")) {
            q2_coll_node node = {0};
            s32 cell = q2_coll_find_node(&c->sim[0].coll_primary, shaft_at, -1, true);
            CHECK(!memcmp(e->pos, shaft_at, sizeof(e->pos)));
            CHECK(cell >= 0 && q2_collision_get_node(&c->sim[0].coll_primary, (u32)cell, &node));
            CHECK(e->surface == node.contents);
        }
    }
    capture_native(c, output, "power1-shaft", shaft_eye, shaft_at);
    c = cold_population_save(c, disc_path, output);
    CHECK(c->native_mission.power1_blast_phase == 1 && c->native_mission.power1_next_blast == 101);
    CHECK(c->sim[0].combat.inv.health == 70 && c->sim[0].level_time == 1);
    count = native_explosions(c);
    c->sim[0].cur_dt = 10;
    c->sim[0].level_time = 101;
    client_native_mission_frame(c, true);
    CHECK(native_explosions(c) == count && c->sim[0].combat.inv.health == 70);
    c->sim[0].level_time++;
    client_native_mission_frame(c, true);
    CHECK(native_explosions(c) == count + 1 && c->sim[0].combat.inv.health == 40);
    CHECK(c->native_mission.power1_blast_phase == 2);
    c->sim[0].player[0].pos[0] = 2000; /* Keep the timing fixture outside damage. */
    for (i = 3; i <= 16; i++) {
        count = native_explosions(c);
        c->sim[0].level_time = (s32)c->native_mission.power1_next_blast + 1;
        client_native_mission_frame(c, true);
        CHECK(c->native_mission.power1_blast_phase == (i & 15));
        CHECK(native_explosions(c) == count + (i < 8 ? 1u : 0u));
    }
    for (j = 0; j < 2; j++) {
        c->sim[0].player[0].pos[1] = 5200 - (s32)j + Q2_EYE_BASE;
        for (i = 1; i <= 16; i++) {
            count = native_explosions(c);
            c->sim[0].level_time = (s32)c->native_mission.power1_next_blast + 1;
            client_native_mission_frame(c, true);
            CHECK(native_explosions(c) == count + (i < (j ? 8u : 3u) ? 1u : 0u));
        }
    }
    /* Every face of the damage box is strict, in ENTITY-origin coordinates. */
    for (i = 0; i < 3; i++) for (j = 0; j < 4; j++) {
        s32 limit = i == 1 ? 1500 : 500;
        bool inside = (j & 1) != 0;
        c->native_mission.power1_blast_phase = c->native_mission.power1_next_blast = 0;
        memcpy(c->sim[0].player[0].pos, shaft_at, sizeof(shaft_at));
        c->sim[0].player[0].pos[i] += (j < 2 ? -1 : 1) * (limit - (inside ? 1 : 0));
        c->sim[0].player[0].pos[1] += Q2_EYE_BASE;
        c->sim[0].combat.inv.health = 100;
        c->sim[0].combat.self.env_next = 0;
        c->sim[0].level_time = 5000;
        client_native_mission_frame(c, true);
        CHECK(c->sim[0].combat.inv.health == (inside ? 70 : 100));
    }
    /* A pre-existing lava deadline still suppresses this scripted hit. */
    c->native_mission.power1_blast_phase = c->native_mission.power1_next_blast = 0;
    c->sim[0].combat.self.env_next = c->sim[0].level_time;
    c->sim[0].combat.inv.health = 100;
    client_native_mission_frame(c, true);
    CHECK(c->sim[0].combat.inv.health == 100);
    c->native_mission.power1_blast_phase = 5;
    c->native_mission.power1_next_blast = 7777;
    CHECK(!client_load_zone(c, "POWER1", 99));
    CHECK(c->native_mission.power1_next_blast == 7777);
    c->carry_player = c->carry_same_map = true;
    CHECK(client_load_zone(c, "POWER1", 2));
    CHECK(c->native_mission.power1_blast_phase == 5 && !c->native_mission.power1_next_blast);
    c->sim[0].cur_dt = 10;
    client_native_mission_frame(c, true);
    CHECK(c->native_mission.power1_blast_phase == 5 && !c->native_mission.power1_next_blast);
    c->carry_player = c->carry_same_map = true;
    CHECK(client_load_zone(c, "POWER1", 1));
    CHECK(c->native_mission.power1_blast_phase == 5 && !c->native_mission.power1_next_blast);
    c->sim_enabled = true;
    client_input_simulated(c, 1.0f / 30.0f);
    client_input_simulated(c, 1.0f / 30.0f); /* PAL's first 12-tick quantum. */
    CHECK(c->native_mission.power1_blast_phase == 6);
    CHECK(client_load_zone(c, "POWER2", 0));
    CHECK(client_load_zone(c, "POWER1", 1));
    CHECK(!c->native_mission.power1_blast_phase && !c->native_mission.power1_next_blast);
    close_client(c);
}

static void test_disc_power_callbacks(const char *disc_path, const char *output)
{
    static const s32 glass_time[] = {4100,4200,4700,4750,4800,5100};
    const s32 eye[3] = {-20120,-500,-8190}, target[3] = {-21760,280,-10242};
    client *c = open_client(disc_path);
    u32 i, popup, vis_count;
    s16 vis_node[32];
    u8 vis_hidden[32];
    s32 drain;
    q2_monster *guard;
    printf("POWER2: reactor core, pump collapse and timed exit\n");
    CHECK(client_load_zone(c, "POWER2", 0));
    q2_fx_beams_reset(&c->sim[0].fx);
    client_native_mission_frame(c, false);
    CHECK(c->sim[0].fx.beam_count == 1);
    CHECK(c->reactor_beam.tube[0].colour[0] == 0x3A000064u);
    CHECK(c->reactor_beam.tube[0].v[2] == 6 && c->reactor_beam.tube[0].v[3] == 7);
    {
        q2_coll_node cell = {0};
        s32 index = q2_coll_find_node(&c->sim[0].coll_primary, client_reactor_top, -1, true);
        CHECK(index >= 0 && q2_collision_get_node(&c->sim[0].coll_primary, (u32)index, &cell));
        CHECK(c->sim[0].fx.beam[0].area == cell.contents);
    }
    capture_native(c, output, "power2-core", eye, target);
    if (output) CHECK(c->sim[0].fx.stats.beam_faces_emitted > 0);
    CHECK(client_load_zone(c, "WASTE2", 0));
    mission_call(c, "Pump1On");
    CHECK(client_load_zone(c, "POWER2", 0));
    c->sim[0].cur_dt = 10;
    client_native_mission_frame(c, true);
    CHECK(!(c->native_mission.flags & Q2_NATIVE_REACTOR_ACTIVE));
    CHECK(client_load_zone(c, "WASTE3", 0));
    mission_call(c, "Pump2On");
    CHECK(client_load_zone(c, "POWER2", 0));
    c->sim[0].cur_dt = 10; c->sim[0].level_time = 1000;
    client_native_mission_frame(c, true);
    CHECK(queued_named(c, "DrainCool"));
    CHECK(!strcmp(c->popup.orders_key, "Unit3Curr5"));
    CHECK(!strcmp(c->popup.objective_key, "Unit3Miss2"));
    CHECK(c->native_mission.reactor_start == 1000 && c->native_mission.reactor_alarm == 0);
    popup = c->popup_raises;
    CHECK(popup == 3); /* Two pumps, then the reactor objective. */
    {
        q2_native_mission state = c->native_mission;
        c->sim[0].level_time = 2000;
        client_native_mission_frame(c, false);
        CHECK(!memcmp(&state, &c->native_mission, sizeof(state)));
        c->mp_enabled = true;
        client_native_mission_frame(c, true);
        c->mp_enabled = false;
        CHECK(!memcmp(&state, &c->native_mission, sizeof(state)));
    }
    c = cold_population_save(c, disc_path, output);
    CHECK(c->native_mission.reactor_start == 1000 && queued_named(c, "DrainCool"));
    CHECK(c->popup_raises == 0);
    c->sim[0].cur_dt = 10;
    drain = named_mover(c, "DrainCool");
    CHECK(drain >= 0);
    CHECK(q2_event_rt_update(&c->sim[0].event_rt) == Q2_EVENT_OK);
    if (drain >= 0) {
        /* A one-half-stroke PISTON lowers both coolant surfaces by 314.
         * Use playable dt: a giant step overshoots the original sine path. */
        CHECK(c->movers.movers[drain].prim == Q2_UF_PISTON);
        for (i = 0; i < 45; i++) q2_movers_tick(&c->movers, 12, 0);
        q2_sim_movers_update(&c->sim[0], &c->movers);
        CHECK(c->movers.movers[drain].part_count == 2);
        CHECK(c->movers.movers[drain].offset >= 312 && c->movers.movers[drain].offset <= 314);
        CHECK(!c->movers.movers[drain].running && c->movers.movers[drain].strokes_done == 1);
    }
    for (i = 0; i < 30; i++) client_native_mission_frame(c, true);
    CHECK(c->native_mission.reactor_alarm == 300);
    client_native_mission_frame(c, true);
    CHECK(c->native_mission.reactor_alarm == 10);
    for (i = 0; i < 6; i++) {
        char name[7];
        s32 node;
        snprintf(name, sizeof(name), "Glass%u", i + 1);
        node = client_named_node(c, name);
        CHECK(node >= 0);
        if (node < 0) continue;
        c->sim[0].level_time = 1000 + glass_time[i] - 1;
        client_native_mission_frame(c, true);
        CHECK(!c->node_hidden[node]);
        c->sim[0].level_time++;
        client_native_mission_frame(c, true);
        CHECK(c->node_hidden[node]);
        if (i == 2) {
            q2_native_mission state = c->native_mission;
            c = cold_population_save(c, disc_path, output);
            CHECK(!memcmp(&state, &c->native_mission, sizeof(state)));
            drain = named_mover(c, "DrainCool");
            CHECK(drain >= 0 && c->movers.movers[drain].offset >= 312);
            CHECK(c->node_hidden[client_named_node(c, "Glass1")] &&
                  c->node_hidden[client_named_node(c, "Glass3")] &&
                  !c->node_hidden[client_named_node(c, "Glass4")]);
            c->sim[0].cur_dt = 10;
        }
    }
    capture_native(c, output, "power2-collapse", eye, target);
    c->sim[0].level_time = 10999;
    client_native_mission_frame(c, true);
    CHECK(!queued_named(c, "ExploDoor"));
    c->sim[0].level_time++;
    client_native_mission_frame(c, true);
    CHECK(queued_named(c, "ExploDoor"));
    CHECK(c->native_mission.flags & Q2_NATIVE_REACTOR_DONE);
    CHECK(q2_event_rt_update(&c->sim[0].event_rt) == Q2_EVENT_OK);
    CHECK(c->sim[0].explosive_destroyed == 1);
    vis_count = c->sim[0].node_vis_count;
    CHECK(vis_count == 2); /* Hide the intact exit and show its rubble. */
    for (i = 0; i < vis_count; i++) {
        vis_node[i] = c->sim[0].node_vis[i].node;
        vis_hidden[i] = c->sim[0].node_vis[i].hidden;
    }
    c->sim_enabled = true;
    client_input_simulated(c, 1.0f / 30.0f);
    client_input_simulated(c, 1.0f / 30.0f);
    for (i = 0; i < vis_count; i++)
        CHECK(c->node_hidden[vis_node[i]] == vis_hidden[i]);
    c = cold_population_save(c, disc_path, output);
    CHECK(c->native_mission.flags & Q2_NATIVE_REACTOR_DONE);
    c->carry_player = c->carry_same_map = true;
    CHECK(client_load_zone(c, "POWER2", 1));
    c->carry_player = c->carry_same_map = true;
    CHECK(client_load_zone(c, "POWER2", 0));
    CHECK(!(c->native_mission.flags & Q2_NATIVE_REACTOR_ACTIVE));
    for (i = 0; i < 6; i++) {
        char name[7];
        snprintf(name, sizeof(name), "Glass%u", i + 1);
        CHECK(c->node_hidden[client_named_node(c, name)]);
    }
    CHECK(named_flags(c, "DrainCool") & Q2_EVREC_HASRUN);
    CHECK(client_load_zone(c, "WASTE1", 0));
    CHECK(client_load_zone(c, "POWER2", 0));
    CHECK(c->native_mission.flags & Q2_NATIVE_REACTOR_DONE);
    CHECK(c->node_hidden[client_named_node(c, "Glass6")]);
    c = cold_population_save(c, disc_path, output);
    CHECK(c->node_hidden[client_named_node(c, "Glass6")]);
    q2_fx_beams_reset(&c->sim[0].fx);
    client_native_mission_frame(c, false);
    CHECK(c->sim[0].fx.beam_count == 0);
    CHECK(client_load_zone(c, "POWER2", 3));
    CHECK(!(named_flags(c, "RockLaser") & Q2_EVREC_DISABLED));
    guard = population_actor(c, "Zone3", 2);
    if (guard) {
        kill_population_actor(guard);
        client_native_mission_frame(c, true);
        CHECK(!(named_flags(c, "RockLaser") & Q2_EVREC_DISABLED));
        finish_population_death(guard);
        client_native_mission_frame(c, true);
        CHECK(named_flags(c, "RockLaser") & Q2_EVREC_DISABLED);
        c = cold_population_save(c, disc_path, output);
        CHECK(named_flags(c, "RockLaser") & Q2_EVREC_DISABLED);
    }
    CHECK(client_load_zone(c, "POWER2", 4));
    {
        static const struct { s32 time; bool off1, off2; } phases[] = {
            {0,true,true}, {46,true,true}, {47,false,true}, {255,false,true},
            {256,true,true}, {302,true,true}, {303,true,false}, {511,true,false},
            {512,true,true}, {558,true,true}, {559,false,true}, {1023,true,false}
        };
        for (i = 0; i < Q2PSX_ARRAY_COUNT(phases); i++) {
            c->sim[0].level_time = phases[i].time;
            client_native_mission_frame(c, true);
            CHECK(((named_flags(c, "Laser1") & Q2_EVREC_DISABLED) != 0) == phases[i].off1);
            CHECK(((named_flags(c, "Laser2") & Q2_EVREC_DISABLED) != 0) == phases[i].off2);
        }
    }
    c = cold_population_save(c, disc_path, output);
    CHECK(c->sim[0].level_time == 1023);
    CHECK(named_flags(c, "Laser1") & Q2_EVREC_DISABLED);
    CHECK(!(named_flags(c, "Laser2") & Q2_EVREC_DISABLED));
    close_client(c);

    /* Leaving early completes the reactor; returning must also open the
     * exit even though the timed ExploDoor record never ran before departure. */
    c = open_client(disc_path);
    CHECK(client_load_zone(c, "WASTE2", 0));
    mission_call(c, "Pump1On");
    CHECK(client_load_zone(c, "WASTE3", 0));
    mission_call(c, "Pump2On");
    CHECK(client_load_zone(c, "POWER2", 0));
    c->sim[0].cur_dt = 10; c->sim[0].level_time = 500;
    client_native_mission_frame(c, true);
    c->sim[0].level_time = 5600;
    client_native_mission_frame(c, true);
    CHECK(c->native_mission.reactor_glass == 1); /* One per frame, not all due. */
    CHECK(!(c->native_mission.flags & Q2_NATIVE_REACTOR_DONE));
    CHECK(!client_load_zone(c, "POWER2", 99));
    CHECK(!(c->native_mission.flags & Q2_NATIVE_REACTOR_DONE));
    c->carry_player = c->carry_same_map = true;
    CHECK(client_load_zone(c, "POWER2", 1));
    CHECK(c->native_mission.flags & Q2_NATIVE_REACTOR_DONE);
    c->carry_player = c->carry_same_map = true;
    CHECK(client_load_zone(c, "POWER2", 0));
    CHECK(named_flags(c, "ExploDoor") & Q2_EVREC_HASRUN);
    CHECK(c->node_hidden[client_named_node(c, "Glass6")]);
    CHECK(c->sim[0].explosive_destroyed > 0);
    CHECK(named_flags(c, "DrainCool") & Q2_EVREC_HASRUN);
    close_client(c);
}

static q2_mover *lab_lift(client *c, const char *name)
{
    s32 node = client_named_node(c, name);
    u32 i, p;
    for (i = 0; node >= 0 && i < c->movers.count; i++)
        for (p = 0; p < c->movers.movers[i].part_count; p++)
            if (c->movers.movers[i].node[p] == node) return &c->movers.movers[i];
    return NULL;
}

static q2_move_target *lab_lift_box(client *c, const char *name)
{
    q2_scene_node n;
    s32 node = client_named_node(c, name);
    u32 i;
    if (node < 0 || !q2_scene_get_node(&c->zone.scene, (u32)node, &n)) return NULL;
    for (i = 0; i < c->sim[0].mover_count; i++) {
        q2_move_target *box = &c->sim[0].volumes[i];
        if (box->min[0] == n.bbox_min[0] && box->max[0] == n.bbox_max[0] &&
            box->min[2] == n.bbox_min[2] && box->max[2] == n.bbox_max[2]) return box;
    }
    return NULL;
}

static void check_lab_lift_box(client *c, const char *name, const q2_move_target *expected)
{
    q2_move_target *box = lab_lift_box(c, name);
    q2_trace trace;
    s32 from[3], to[3];
    CHECK(box != NULL);
    if (!box) return;
    CHECK(!memcmp(box->min, expected->min, sizeof(box->min)));
    CHECK(!memcmp(box->max, expected->max, sizeof(box->max)));
    CHECK(!memcmp(box->env_min, expected->env_min, sizeof(box->env_min)));
    CHECK(!memcmp(box->env_max, expected->env_max, sizeof(box->env_max)));
    CHECK(box->dy == expected->dy && box->active);
    from[0] = to[0] = (box->min[0] + box->max[0]) / 2;
    from[2] = to[2] = (box->min[2] + box->max[2]) / 2;
    from[1] = box->min[1] - 100; to[1] = box->min[1] + 100;
    q2_sim_trace(&c->sim[0], from, to, &trace);
    CHECK(trace.hit && trace.ent >= 0 && trace.end[1] == box->min[1]);
}

static void test_disc_lab_lifts(const char *disc_path, const char *output)
{
    static const char *names[] = {"Lift A", "Lift C"};
    client *c = open_client(disc_path);
    int z, i;
    for (z = 0; z < 5; z++) {
        CHECK(client_load_zone(c, "LAB", z));
        for (i = 0; i < 2; i++) {
            q2_mover *m = lab_lift(c, names[i]);
            s32 node = client_named_node(c, names[i]);
            q2_scene_node n;
            printf("LAB/%d %s: node %d", z, names[i], node);
            if (m) printf(" item %u state %u offset %d target %d speed %d wait %u", m->item_offset,
                m->state, m->offset, m->target, m->speed, m->wait_timer);
            if (node >= 0 && q2_scene_get_node(&c->zone.scene, (u32)node, &n))
                printf(" bounds (%d,%d,%d)..(%d,%d,%d)", n.bbox_min[0], n.bbox_min[1],
                    n.bbox_min[2], n.bbox_max[0], n.bbox_max[1], n.bbox_max[2]);
            putchar('\n');
        }
    }
    close_client(c);
    for (i = 0; i < 2; i++) {
        q2_mover *m;
        q2_move_target box;
        q2_native_mission saved;
        s32 expected;
        int f, start = i ? 2 : 0, next = i ? 4 : 3;
        c = open_client(disc_path);
        CHECK(client_load_zone(c, "LAB", start));
        m = lab_lift(c, names[i]);
        CHECK(m != NULL);
        if (!m) { close_client(c); continue; }
        CHECK(q2_movers_trigger_item(&c->movers, m->item_offset) == 1);
        for (f = 0; f < 100 && m->state != Q2_MV_OPENING; f++)
            q2_movers_tick(&c->movers, 20, 0);
        CHECK(m->state == Q2_MV_OPENING);
        for (f = 0; f < 45; f++) {
            q2_movers_tick(&c->movers, 20, 0);
            q2_sim_movers_update(&c->sim[0], &c->movers);
        }
        expected = m->offset;
        CHECK(expected == -3600 && m->state == Q2_MV_OPENING);
        CHECK(lab_lift_box(c, names[i]) != NULL);
        if (!lab_lift_box(c, names[i])) { close_client(c); continue; }
        box = *lab_lift_box(c, names[i]);
        CHECK(box.min[1] == 880 && box.dy == -80);
        c->sim[0].cur_dt = 20;
        client_native_mission_frame(c, true);
        CHECK(c->native_mission.lift[i].valid && c->native_mission.lift[i].offset == expected);
        saved = c->native_mission;
        CHECK(!client_load_zone(c, "LAB", 99));
        CHECK(!memcmp(&saved, &c->native_mission, sizeof(saved)));
        c->carry_player = c->carry_same_map = true;
        CHECK(client_load_zone(c, "LAB", next));
        m = lab_lift(c, names[i]);
        CHECK(m != NULL);
        if (m) {
            printf("LAB %s seam %d -> %d: offset %d (expected %d), state %u\n",
                names[i], start, next, m->offset, expected, m->state);
            CHECK(m->offset == expected && m->state == Q2_MV_OPENING);
        }
        check_lab_lift_box(c, names[i], &box);
        if (i == 0) {
            const s32 eye[3] = {8800,-1600,-6500}, target[3] = {8700,1600,-7800};
            capture_native(c, output, "lab-lift-return", eye, target);
        }
        c = cold_population_save(c, disc_path, output);
        m = lab_lift(c, names[i]);
        CHECK(m && m->offset == expected && m->state == Q2_MV_OPENING);
        check_lab_lift_box(c, names[i], &box);
        if (!m) { close_client(c); continue; }
        if (i == 0) {
            q2_save legacy;
            /* Older saves have the resident MOVR state but no NMIS lift
             * history. Its visible pose must still have a matching floor. */
            CHECK(client_capture(c));
            legacy = c->snapshot;
            memset(&c->snapshot, 0, sizeof(c->snapshot));
            legacy.native_mission_present = false;
            close_client(c);
            c = open_client(disc_path);
            CHECK(client_apply_save(c, &legacy));
            q2_save_free(&legacy);
            m = lab_lift(c, names[i]);
            CHECK(m && m->offset == expected && m->state == Q2_MV_OPENING);
            box.dy = 0; /* MOVR never stored the previous frame's motion. */
            check_lab_lift_box(c, names[i], &box);
        }
        q2_movers_tick(&c->movers, 20, 0);
        q2_sim_movers_update(&c->sim[0], &c->movers);
        CHECK(m->offset == expected - 80 && lab_lift_box(c, names[i])->dy == -80);
        expected = m->offset;
        box = *lab_lift_box(c, names[i]);
        c->carry_player = c->carry_same_map = true;
        CHECK(client_load_zone(c, "LAB", start));
        m = lab_lift(c, names[i]);
        CHECK(m && m->offset == expected && m->state == Q2_MV_OPENING);
        check_lab_lift_box(c, names[i], &box);
        if (!m) { close_client(c); continue; }
        for (f = 0; f < 100 && m->state != Q2_MV_OPEN; f++) {
            q2_movers_tick(&c->movers, 20, 0);
            q2_sim_movers_update(&c->sim[0], &c->movers);
        }
        CHECK(m->state == Q2_MV_OPEN && m->offset == m->target && m->wait_timer == 900);
        q2_movers_tick(&c->movers, 20, 0);
        q2_sim_movers_update(&c->sim[0], &c->movers);
        CHECK(m->wait_timer == 880);
        box = *lab_lift_box(c, names[i]);
        CHECK(client_load_zone(c, "LAB", 1)); /* Neither lift is resident. */
        CHECK(!lab_lift(c, names[i]));
        c = cold_population_save(c, disc_path, output);
        CHECK(client_load_zone(c, "LAB", next));
        m = lab_lift(c, names[i]);
        CHECK(m && m->state == Q2_MV_OPEN && m->wait_timer == 880 && m->offset == m->target);
        check_lab_lift_box(c, names[i], &box);
        if (!m) { close_client(c); continue; }
        q2_movers_tick(&c->movers, 881, 0);
        CHECK(m->state == Q2_MV_CLOSING);
        q2_movers_tick(&c->movers, 20, 0);
        q2_sim_movers_update(&c->sim[0], &c->movers);
        expected = m->target + 80;
        CHECK(m->offset == expected);
        box = *lab_lift_box(c, names[i]);
        CHECK(client_load_zone(c, "JAIL2", 0));
        c = cold_population_save(c, disc_path, output);
        CHECK(client_load_zone(c, "LAB", start));
        m = lab_lift(c, names[i]);
        CHECK(m && m->state == Q2_MV_CLOSING && m->offset == expected);
        check_lab_lift_box(c, names[i], &box);
        CHECK(!c->native_mission.lift[1 - i].valid);
        snprintf(c->first_map, sizeof(c->first_map), "BASE1");
        client_start_game(c);
        CHECK(!c->native_mission.lift[0].valid && !c->native_mission.lift[1].valid);
        CHECK(client_load_zone(c, "LAB", start));
        m = lab_lift(c, names[i]);
        CHECK(m && m->state == Q2_MV_IDLE && !m->offset);
        close_client(c);
    }

}

static u32 native_lift_boxes(client *c, const char *name, q2_move_target boxes[Q2_MOVER_MAX_PARTS])
{
    q2_mover *m = lab_lift(c, name);
    u32 i, count = 0;
    CHECK(m != NULL);
    if (!m) return 0;
    for (i = 0; i < c->sim[0].mover_count; i++)
        if (c->sim[0].volumes[i].id == (s32)(m - c->movers.movers) && count < Q2_MOVER_MAX_PARTS)
            boxes[count++] = c->sim[0].volumes[i];
    CHECK(count == m->part_count);
    return count;
}

static void check_native_lift_boxes(client *c, const char *name,
                                    const q2_move_target *expected, u32 count)
{
    q2_move_target boxes[Q2_MOVER_MAX_PARTS];
    q2_trace trace;
    s32 from[3], to[3];
    u32 i, top = 0, actual = native_lift_boxes(c, name, boxes);
    CHECK(actual == count);
    if (actual != count || !count) return;
    for (i = 0; i < count; i++) {
        CHECK(!memcmp(boxes[i].min, expected[i].min, sizeof(boxes[i].min)));
        CHECK(!memcmp(boxes[i].max, expected[i].max, sizeof(boxes[i].max)));
        CHECK(!memcmp(boxes[i].env_min, expected[i].env_min, sizeof(boxes[i].env_min)));
        CHECK(!memcmp(boxes[i].env_max, expected[i].env_max, sizeof(boxes[i].env_max)));
        CHECK(boxes[i].dy == expected[i].dy && boxes[i].active);
        if (boxes[i].min[1] < boxes[top].min[1]) top = i;
    }
    /* Trace the uppermost part: BOSS2's platform covers its named stem. */
    from[0] = to[0] = (boxes[top].min[0] + boxes[top].max[0]) / 2;
    from[2] = to[2] = (boxes[top].min[2] + boxes[top].max[2]) / 2;
    from[1] = boxes[top].min[1] - 100; to[1] = boxes[top].min[1] + 100;
    q2_sim_trace(&c->sim[0], from, to, &trace);
    CHECK(trace.hit && trace.ent >= 0 && trace.end[1] == boxes[top].min[1]);
}

static void test_disc_native_lifts(const char *disc_path, const char *output)
{
    static const struct { const char *map, *name; int zones; } cases[] = {
        {"SECURITY", "Lift A", 3}, {"SECURITY", "Lift B", 3},
        {"BOSS1", "Lift A", 2},
        {"BOSS2", "LiftStemA", 3}, {"BOSS2", "LiftStemB", 3}
    };
    u32 i;
    for (i = 0; i < Q2PSX_ARRAY_COUNT(cases); i++) {
        client *c = open_client(disc_path);
        int z;
        for (z = 0; z < cases[i].zones; z++) {
            q2_mover *m;
            q2_scene_node n;
            s32 node;
            CHECK(client_load_zone(c, cases[i].map, z));
            m = lab_lift(c, cases[i].name);
            node = client_named_node(c, cases[i].name);
            CHECK((m != NULL) == (z < 2));
            printf("%s/%d %s: node %d", cases[i].map, z, cases[i].name, node);
            if (m) printf(" item %u prim %u parts %u state %u offset %d target %d speed %d wait %u",
                m->item_offset, m->prim, m->part_count, m->state, m->offset, m->target, m->speed, m->wait_timer);
            if (node >= 0 && q2_scene_get_node(&c->zone.scene, (u32)node, &n))
                printf(" bounds (%d,%d,%d)..(%d,%d,%d)", n.bbox_min[0], n.bbox_min[1],
                    n.bbox_min[2], n.bbox_max[0], n.bbox_max[1], n.bbox_max[2]);
            putchar('\n');
        }
        close_client(c);
    }
    for (i = 0; i < Q2PSX_ARRAY_COUNT(cases); i++) {
        client *c = open_client(disc_path);
        q2_mover *m;
        q2_move_target boxes[Q2_MOVER_MAX_PARTS];
        q2_native_mission saved;
        s32 expected, step;
        u32 count, p;
        u16 wait;
        int f;
        CHECK(client_load_zone(c, cases[i].map, 0));
        m = lab_lift(c, cases[i].name);
        CHECK(m != NULL);
        if (!m) { close_client(c); continue; }
        CHECK(q2_movers_trigger_item(&c->movers, m->item_offset) == 1);
        for (f = 0; f < 100 && m->state != Q2_MV_OPENING; f++)
            q2_movers_tick(&c->movers, 20, 0);
        CHECK(m->state == Q2_MV_OPENING);
        for (f = 0; f < 30; f++) {
            q2_movers_tick(&c->movers, 20, 0);
            q2_sim_movers_update(&c->sim[0], &c->movers);
        }
        expected = -m->speed * 600;
        step = m->speed * 20;
        CHECK(m->offset == expected && m->state == Q2_MV_OPENING);
        count = native_lift_boxes(c, cases[i].name, boxes);
        c->sim[0].cur_dt = 20;
        client_native_mission_frame(c, true);
        CHECK(c->native_mission.lift[Q2_NATIVE_LIFT_SECURITY_A + i].valid &&
              c->native_mission.lift[Q2_NATIVE_LIFT_SECURITY_A + i].offset == expected);
        saved = c->native_mission;
        client_native_mission_frame(c, false);
        CHECK(!memcmp(&saved, &c->native_mission, sizeof(saved)));
        c->mp_enabled = true;
        client_native_mission_frame(c, true);
        c->mp_enabled = false;
        CHECK(!memcmp(&saved, &c->native_mission, sizeof(saved)));
        CHECK(!client_load_zone(c, cases[i].map, 99));
        CHECK(!memcmp(&saved, &c->native_mission, sizeof(saved)));
        c->carry_player = c->carry_same_map = true;
        CHECK(client_load_zone(c, cases[i].map, 1));
        m = lab_lift(c, cases[i].name);
        CHECK(m != NULL);
        if (m) {
            printf("%s %s seam 0 -> 1: offset %d (expected %d), state %u\n",
                cases[i].map, cases[i].name, m->offset, expected, m->state);
            CHECK(m->offset == expected && m->state == Q2_MV_OPENING);
        }
        check_native_lift_boxes(c, cases[i].name, boxes, count);
        if (i == 0) {
            const s32 eye[3] = {1920,-11500,-19200}, target[3] = {1920,-7000,-19201};
            capture_native(c, output, "security-lift-return", eye, target);
            printf("SECURITY lift camera (%d,%d,%d)\n", c->cam.pos[0], c->cam.pos[1], c->cam.pos[2]);
        }
        c = cold_population_save(c, disc_path, output);
        m = lab_lift(c, cases[i].name);
        CHECK(m && m->offset == expected && m->state == Q2_MV_OPENING);
        check_native_lift_boxes(c, cases[i].name, boxes, count);
        if (!m) { close_client(c); continue; }
        q2_movers_tick(&c->movers, 20, 0);
        q2_sim_movers_update(&c->sim[0], &c->movers);
        expected -= step;
        CHECK(m->offset == expected);
        count = native_lift_boxes(c, cases[i].name, boxes);
        for (p = 0; p < count; p++) CHECK(boxes[p].dy == -step);
        c->carry_player = c->carry_same_map = true;
        CHECK(client_load_zone(c, cases[i].map, 0));
        m = lab_lift(c, cases[i].name);
        CHECK(m && m->offset == expected && m->state == Q2_MV_OPENING);
        check_native_lift_boxes(c, cases[i].name, boxes, count);
        if (!m) { close_client(c); continue; }
        for (f = 0; f < 200 && m->state != Q2_MV_OPEN; f++) {
            q2_movers_tick(&c->movers, 20, 0);
            q2_sim_movers_update(&c->sim[0], &c->movers);
        }
        CHECK(m->state == Q2_MV_OPEN && m->offset == m->target);
        wait = m->wait_timer;
        CHECK(wait == (i < 2 ? 600 : i == 2 ? 300 : 900));
        q2_movers_tick(&c->movers, 20, 0);
        q2_sim_movers_update(&c->sim[0], &c->movers);
        CHECK(m->wait_timer == wait - 20);
        count = native_lift_boxes(c, cases[i].name, boxes);
        if (cases[i].zones > 2) CHECK(client_load_zone(c, cases[i].map, 2));
        else CHECK(client_load_zone(c, "BASE1", 0));
        CHECK(!lab_lift(c, cases[i].name));
        c = cold_population_save(c, disc_path, output);
        CHECK(client_load_zone(c, cases[i].map, 1));
        m = lab_lift(c, cases[i].name);
        CHECK(m && m->state == Q2_MV_OPEN && m->wait_timer == wait - 20 && m->offset == m->target);
        check_native_lift_boxes(c, cases[i].name, boxes, count);
        if (!m) { close_client(c); continue; }
        q2_movers_tick(&c->movers, wait, 0);
        CHECK(m->state == Q2_MV_CLOSING);
        q2_movers_tick(&c->movers, 20, 0);
        q2_sim_movers_update(&c->sim[0], &c->movers);
        expected = m->target + step;
        CHECK(m->offset == expected);
        count = native_lift_boxes(c, cases[i].name, boxes);
        CHECK(client_load_zone(c, "BASE1", 0));
        c = cold_population_save(c, disc_path, output);
        CHECK(client_load_zone(c, cases[i].map, 0));
        m = lab_lift(c, cases[i].name);
        CHECK(m && m->state == Q2_MV_CLOSING && m->offset == expected);
        check_native_lift_boxes(c, cases[i].name, boxes, count);
        snprintf(c->first_map, sizeof(c->first_map), "BASE1");
        client_start_game(c);
        for (p = 0; p < Q2_NATIVE_LIFT_COUNT; p++) CHECK(!c->native_mission.lift[p].valid);
        CHECK(client_load_zone(c, cases[i].map, 0));
        m = lab_lift(c, cases[i].name);
        CHECK(m && m->state == Q2_MV_IDLE && !m->offset);
        close_client(c);
    }
    {
        static const struct { const char *map, *name; int zone; } shared[] = {
            {"LAB", "Lift A", 3}, {"LAB", "Lift C", 4},
            {"SECURITY", "Lift A", 1}, {"SECURITY", "Lift B", 1},
            {"BOSS1", "Lift A", 1}, {"BOSS2", "LiftStemA", 1}, {"BOSS2", "LiftStemB", 1}
        };
        client *c = open_client(disc_path);
        s32 offsets[Q2_NATIVE_LIFT_COUNT];
        q2_native_lift saved[Q2_NATIVE_LIFT_COUNT];
        for (i = 0; i < Q2_NATIVE_LIFT_COUNT; i++) {
            q2_mover *m;
            int f;
            CHECK(client_load_zone(c, shared[i].map, shared[i].zone));
            m = lab_lift(c, shared[i].name);
            CHECK(m != NULL);
            if (!m) continue;
            CHECK(q2_movers_trigger_item(&c->movers, m->item_offset) == 1);
            for (f = 0; f < 100 && m->state != Q2_MV_OPENING; f++)
                q2_movers_tick(&c->movers, 20, 0);
            q2_movers_tick(&c->movers, 20, 0);
            q2_sim_movers_update(&c->sim[0], &c->movers);
            CHECK(m->offset < 0 && m->state == Q2_MV_OPENING);
        }
        CHECK(client_load_zone(c, "BASE1", 0));
        memcpy(saved, c->native_mission.lift, sizeof(saved));
        for (i = 0; i < Q2_NATIVE_LIFT_COUNT; i++) {
            offsets[i] = saved[i].offset;
            CHECK(saved[i].valid && offsets[i] < 0);
        }
        c = cold_population_save(c, disc_path, output);
        CHECK(!memcmp(saved, c->native_mission.lift, sizeof(saved)));
        for (i = 0; i < Q2_NATIVE_LIFT_COUNT; i++) {
            q2_mover *m;
            CHECK(client_load_zone(c, shared[i].map, shared[i].zone));
            m = lab_lift(c, shared[i].name);
            CHECK(m && m->offset == offsets[i] && m->state == Q2_MV_OPENING);
        }
        close_client(c);
    }
}

/* Run the shipped kill gates, so a quake cannot accidentally start at load,
 * on Jorg's first death, or before the two SECURITY commanders finish dying. */
static bool start_native_quake(client *c)
{
    q2_monster *m, *rider = NULL;
    u32 i;
    c->sim[0].cur_dt = 10;
    if (client_name_eq(c->map, "SECURITY")) {
        for (i = 0; i < 2; i++) {
            m = population_actor(c, "Zone1", i);
            if (!m) return false;
            kill_population_actor(m);
            finish_population_death(m);
        }
    } else {
        m = population_actor(c, "Zone1", 0);
        if (!m) return false;
        for (i = 0; i < c->creatures.set.count; i++)
            if (c->creatures.set.monsters[i].owner == m &&
                c->creatures.set.monsters[i].group >= c->creatures.pop.group_count)
                rider = &c->creatures.set.monsters[i];
        CHECK(rider != NULL);
        if (!rider) return false;
        kill_population_actor(m);
        finish_population_death(m);
        client_native_mission_frame(c, true);
        CHECK(!(c->native_mission.flags & Q2_NATIVE_TRAIN_OPEN));
        CHECK(!c->sim[0].player[0].impulse_armed);
        kill_population_actor(rider);
        finish_population_death(rider);
    }
    client_native_mission_frame(c, true);
    CHECK(!c->sim[0].player[0].impulse_armed); /* Start follows quake processing. */
    return true;
}

static void capture_quake(client *c, const char *output, const char *map)
{
    char path[1024];
    if (!output) return;
    CHECK(c->headless && c->sim_enabled);
    q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
    client_frame(c);
    snprintf(path, sizeof(path), "%s/%s-quake.ppm", output, map);
    CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);
    printf("%s quake camera %d,%d,%d; player %d,%d,%d\n", map,
           c->cam.pos[0], c->cam.pos[1], c->cam.pos[2],
           c->sim[0].player[0].pos[0], c->sim[0].player[0].pos[1], c->sim[0].player[0].pos[2]);
}

static void reset_quake_probe(client *c, bool boss, u32 seed)
{
    c->native_mission.security_quake_phase = c->native_mission.boss2_quake_phase = 0;
    if (boss) c->native_mission.boss2_quake_time = 300;
    else c->native_mission.security_wall[3] = 1950;
    memset(c->sim[0].player[0].impulse, 0, sizeof(c->sim[0].player[0].impulse));
    c->sim[0].player[0].impulse_armed = false;
    c->sim[0].player[0].ent2_flags &= ~0x4000u;
    memset(c->shock, 0, sizeof(c->shock));
    c->shock_requests = c->shock_taken = 0;
    client_voices_stop(c);
    q2_rng_seed(&c->sim[0].fx_rng, seed);
    c->sim[0].cur_dt = 1;
}

static s32 quake_probe_phase(const client *c, bool boss)
{
    return boss ? c->native_mission.boss2_quake_phase : c->native_mission.security_quake_phase;
}

static void test_quake_boundaries(client *c, bool boss, const char *disc_path, const char *output)
{
    const char *map = boss ? "BOSS2" : "SECURITY";
    s32 velocity[3];
    s16 impulse[3];
    q2_vag explosion;
    u32 sounds, shocks, seed, n;
    q2_native_mission saved;
    /* Zone 0 keeps SECURITY's wall countdown paused but still shakes. */
    c->carry_player = c->carry_same_map = true;
    CHECK(client_load_zone(c, map, 0));
    reset_quake_probe(c, boss, 1);
    memcpy(velocity, c->sim[0].player[0].vel, sizeof(velocity));
    c->sim[0].combat.inv.health = 0; /* Native pointer guard, no health gate. */
    c->sim[0].player[0].impulse[0] = 32760;
    c->sim[0].player[0].impulse[1] = -32760;
    c->sim[0].player[0].impulse[2] = 32760;
    sounds = c->voice_started;
    client_native_mission_frame(c, true);
    /* BIOS seed 1: 16838,5758,10113,17515,31051. MIPS signed /6 or /12,
     * /16 and halfword wrap: catches both negative rounding and saturation. */
    CHECK(c->sim[0].player[0].impulse[0] == (boss ? -32739 : -32701));
    CHECK(c->sim[0].player[0].impulse[1] == 32112);
    CHECK(c->sim[0].player[0].impulse[2] == (boss ? 32238 : 31715));
    CHECK(c->sim[0].player[0].impulse_armed && (c->sim[0].player[0].ent2_flags & 0x4000));
    CHECK(!memcmp(velocity, c->sim[0].player[0].vel, sizeof(velocity)));
    CHECK(c->sim[0].fx_rng.state == 0xF94BDF32u);
    CHECK(c->voice_started == sounds && c->shock_requests == 1 && c->shock_taken == 1);
    CHECK(c->shock[0].slot[3].data == q2_shock_find(Q2_SHOCK_EXPLOSION) &&
          c->shock[0].slot[3].strength == 2155);
    CHECK(quake_probe_phase(c, boss) == -29);
    c->sim[0].combat.inv.health = 100;
    c->sim[0].cur_dt = 29;
    client_native_mission_frame(c, true);
    CHECK(quake_probe_phase(c, boss) == 0 && c->shock_requests == 1);
    c->sim[0].cur_dt = 61;
    client_native_mission_frame(c, true);
    CHECK(quake_probe_phase(c, boss) == -29 && c->shock_requests == 4);

    /* Neither render-only frames, restoration, EVE reconstruction, absent
     * players nor multiplayer may consume the quake clock or randomness. */
    reset_quake_probe(c, boss, 1);
    for (n = 0; n < 5; n++) {
        c->restoring_save = n == 1 ? &c->snapshot : NULL;
        c->sim[0].event_rt.initial_pass = n == 2;
        c->sim[0].player_count = n == 3 ? 0 : 1;
        c->mp_enabled = n == 4;
        client_native_mission_frame(c, n != 0);
        CHECK(!c->sim[0].player[0].impulse_armed && !c->shock_requests);
        CHECK(quake_probe_phase(c, boss) == 0 && c->sim[0].fx_rng.state == 1);
        if (boss) CHECK(c->native_mission.boss2_quake_time == 300);
    }
    c->restoring_save = NULL;
    c->sim[0].event_rt.initial_pass = c->mp_enabled = false;
    c->sim[0].player_count = 1;

    /* Seed 0's fifth draw is 3498: real bank voice, at the entity origin,
     * with no extra random draw for a direct positional sound request. */
    reset_quake_probe(c, boss, 0);
    CHECK(client_find_sound(c, "wep_grenlx1a", &explosion));
    sounds = c->voice_started;
    client_native_mission_frame(c, true);
    CHECK(c->sim[0].fx_rng.state == 0x0DAA96F5u && c->voice_started == sounds + 1);
    CHECK(c->voice_last && c->voice_last->spu.dec.adpcm == explosion.body);
    CHECK(c->voice_last && c->voice_last->source == CLIENT_SOUND_POINT);
    CHECK(c->voice_last && c->voice_last->pos_world[0] == c->sim[0].player[0].pos[0] &&
          c->voice_last->pos_world[1] == q2_sim_origin_y(c->sim[0].player[0].pos[1]) &&
          c->voice_last->pos_world[2] == c->sim[0].player[0].pos[2]);
    {
        s16 pcm[4096];
        u32 nonzero = 0;
        c->music_open = c->music_playing = false;
        c->bed_frames = c->bed_pos = 0;
        client_voices_update(c);
        client_audio_mix(c, pcm, Q2PSX_ARRAY_COUNT(pcm) / XA_CHANNELS);
        for (n = 0; n < Q2PSX_ARRAY_COUNT(pcm); n++) if (pcm[n]) nonzero++;
        CHECK(nonzero > 0);
    }

    reset_quake_probe(c, boss, 1);
    client_native_mission_frame(c, true);
    saved = c->native_mission;
    memcpy(impulse, c->sim[0].player[0].impulse, sizeof(impulse));
    seed = c->sim[0].fx_rng.state;
    c = cold_population_save(c, disc_path, output);
    CHECK(!memcmp(&saved, &c->native_mission, sizeof(saved)));
    CHECK(!memcmp(impulse, c->sim[0].player[0].impulse, sizeof(impulse)) &&
          c->sim[0].player[0].impulse_armed && c->sim[0].fx_rng.state == seed);
    c->sim[0].cur_dt = 29;
    shocks = c->shock_requests;
    client_native_mission_frame(c, true);
    CHECK(c->shock_requests == shocks && quake_probe_phase(c, boss) == 0);
    c->sim[0].cur_dt = 1;
    client_native_mission_frame(c, true);
    CHECK(c->shock_requests == shocks + 1 && quake_probe_phase(c, boss) == -29);
    saved = c->native_mission;
    c->carry_player = c->carry_same_map = true;
    CHECK(client_load_zone(c, map, 2));
    CHECK(quake_probe_phase(c, boss) == (boss ? saved.boss2_quake_phase : saved.security_quake_phase));
    if (boss) CHECK(c->native_mission.boss2_quake_time == saved.boss2_quake_time);
    c->carry_player = c->carry_same_map = true;
    CHECK(client_load_zone(c, map, 1));
    if (boss) {
        reset_quake_probe(c, true, 1);
        c->native_mission.boss2_quake_time = 299;
        client_native_mission_frame(c, true);
        CHECK(c->native_mission.boss2_quake_time == 300 && !c->shock_requests);
        client_native_mission_frame(c, true);
        CHECK(c->native_mission.boss2_quake_time == 301 && c->shock_requests == 1);
        c->native_mission.boss2_quake_time = 2999;
        c->native_mission.boss2_quake_phase = 0;
        client_native_mission_frame(c, true);
        CHECK(c->native_mission.boss2_quake_time == 3000 && c->shock_requests == 2);
        c->native_mission.boss2_quake_phase = 0;
        client_native_mission_frame(c, true);
        CHECK(c->native_mission.boss2_quake_time == -3000 && c->shock_requests == 3);
        CHECK(c->native_mission.boss2_quake_phase == -29);
        c = cold_population_save(c, disc_path, output);
        CHECK(c->native_mission.boss2_quake_time == -3000 && c->native_mission.boss2_quake_phase == -29);
        shocks = c->shock_requests;
        c->sim[0].cur_dt = 300;
        for (n = 0; n < 11; n++) client_native_mission_frame(c, true);
        CHECK(c->native_mission.boss2_quake_time == 300 && c->shock_requests == shocks);
        CHECK(c->native_mission.boss2_quake_phase == -29);
        c->sim[0].cur_dt = 29;
        client_native_mission_frame(c, true);
        CHECK(c->native_mission.boss2_quake_phase == 0 && c->shock_requests == shocks);
        c->sim[0].cur_dt = 1;
        client_native_mission_frame(c, true);
        CHECK(c->shock_requests == shocks + 1);
    } else {
        reset_quake_probe(c, false, 1);
        memset(c->native_mission.security_wall, 0, sizeof(c->native_mission.security_wall));
        c->native_mission.security_wall[3] = 1;
        client_native_mission_frame(c, true);
        CHECK(c->shock_requests == 1 && queued_named(c, "ExplodeWall4"));
        CHECK(c->native_mission.security_wall[3] == 0);
        c->sim[0].cur_dt = 300;
        client_native_mission_frame(c, true);
        CHECK(c->shock_requests == 1 && c->native_mission.security_quake_phase == -29);
        c->native_mission.security_wall[3] = 500;
    }
    CHECK(client_load_zone(c, "BASE1", 0));
    CHECK(c->native_mission.boss2_quake_time == Q2_NATIVE_QUAKE_INACTIVE &&
          !c->native_mission.boss2_quake_phase && !c->native_mission.security_quake_phase &&
          !c->native_mission.security_wall[3]);
    CHECK(client_load_zone(c, map, 1));
    shocks = c->shock_requests;
    c->sim[0].cur_dt = 600;
    client_native_mission_frame(c, true);
    CHECK(c->shock_requests == shocks); /* Completed unit gate cannot restart. */
    if (boss) CHECK(c->native_mission.boss2_quake_time == Q2_NATIVE_QUAKE_INACTIVE);
    else CHECK(!c->native_mission.security_wall[3]);
    CHECK(client_capture(c));
    c->snapshot.native_mission_present = false;
    CHECK(client_apply_save(c, &c->snapshot));
    CHECK(c->native_mission.boss2_quake_time == Q2_NATIVE_QUAKE_INACTIVE);
    snprintf(c->first_map, sizeof(c->first_map), "%s", map);
    client_start_game(c);
    CHECK(c->native_mission.boss2_quake_time == Q2_NATIVE_QUAKE_INACTIVE &&
          !c->native_mission.security_wall[3] && !c->native_mission.security_quake_phase &&
          !c->native_mission.boss2_quake_phase);
    close_client(c);
}

static void test_disc_quakes(const char *disc_path, const char *output)
{
    const char *maps[] = { "SECURITY", "BOSS2" };
    u32 i;
    for (i = 0; i < Q2PSX_ARRAY_COUNT(maps); i++) {
        client *c = open_client(disc_path);
        u32 shock;
        int n;
        CHECK(client_load_zone(c, maps[i], 1));
        CHECK(c->native_mission.boss2_quake_time == Q2_NATIVE_QUAKE_INACTIVE);
        if (!start_native_quake(c)) { close_client(c); continue; }
        shock = c->shock_requests;
        q2_random_seed(1);
        for (n = 0; n < (i ? 31 : 1); n++) client_native_mission_frame(c, true);
        CHECK(c->sim[0].player[0].impulse_armed);
        CHECK(c->sim[0].player[0].ent2_flags & 0x4000);
        CHECK(c->shock_requests > shock);
        /* Real movement and camera, fixed headless frames, no user input. */
        for (n = 0; n < 30; n++) client_input_simulated(c, 1.0f / 30.0f);
        capture_quake(c, output, maps[i]);
        test_quake_boundaries(c, i != 0, disc_path, output);
    }
}

static u32 security_laser_mask(const client *c)
{
    const char *groups[] = {"LaserBank", "LaserRing", "LaserGrid"};
    const int counts[] = {6,4,2};
    u32 mask = 0, bit = 0;
    int group, side, row;
    for (group = 0; group < 3; group++) for (side = 0; side < 2; side++)
        for (row = 0; row < counts[group]; row++, bit++) {
            char name[13];
            snprintf(name, sizeof(name), "%s %c%c", groups[group], side ? 'R' : 'L', 'A' + row);
            if (!(named_flags(c, name) & Q2_EVREC_DISABLED)) mask |= 1u << bit;
        }
    return mask;
}

static void security_beam_player(client *c, const q2_fx_beam *beam)
{
    int k;
    for (k = 0; k < 3; k++)
        c->sim[0].player[0].pos[k] = (beam->from[k] + beam->to[k]) / 2;
    c->sim[0].player[0].pos[1] += Q2_EYE_BASE;
    c->sim[0].combat.inv.health = 2000;
    c->sim[0].combat.inv.armour = 0;
    c->sim[0].combat.inv.flags = 0;
    c->sim[0].combat.inv.invuln_until = 0;
}

static void test_disc_security_lasers(const char *disc_path, const char *output)
{
    /* Literal masks and endpoints from SECURITY+0xE90..0x1428. Cover each
     * bank on the outward/return scan, ring/grid edges, and unsigned wrap. */
    static const struct { u32 time, left, right; } gates[] = {
        {0,0x101000,0x410000}, {29,0x101000,0x410000},
        {30,0x101001,0x410040}, {49,0x101001,0x410040},
        {50,0x102001,0x420040}, {59,0x102001,0x420040},
        {60,0x102000,0x420000}, {89,0x102000,0x420000},
        {90,0x102002,0x420080}, {99,0x102002,0x420080},
        {100,0x104002,0x440080}, {119,0x104002,0x440080},
        {120,0x104000,0x440000}, {149,0x104000,0x440000},
        {150,0x208004,0x880100}, {179,0x208004,0x880100},
        {180,0x208000,0x880000}, {199,0x208000,0x880000},
        {200,0x201000,0x810000}, {209,0x201000,0x810000},
        {210,0x201008,0x810200}, {239,0x201008,0x810200},
        {240,0x201000,0x810000}, {269,0x202000,0x820000},
        {270,0x202010,0x820400}, {299,0x202010,0x820400},
        {300,0x104000,0x440000}, {329,0x104000,0x440000},
        {330,0x104020,0x440800}, {359,0x108020,0x480800},
        {360,0x108000,0x480000}, {389,0x108000,0x480000},
        {390,0x108010,0x480400}, {419,0x101010,0x410400},
        {420,0x101000,0x410000}, {449,0x101000,0x410000},
        {450,0x202008,0x820200}, {479,0x202008,0x820200},
        {480,0x202000,0x820000}, {509,0x204000,0x840000},
        {510,0x204004,0x840100}, {539,0x204004,0x840100},
        {540,0x204000,0x840000}, {569,0x208000,0x880000},
        {570,0x208002,0x880080}, {599,0x208002,0x880080},
        {600,0x101000,0x410000}, {UINT32_MAX,0x202000,0x820000}
    };
    static const struct { u32 time; s32 y, z0, z1; } paths[] = {
        {0,-5121,-15040,-11840}, {179,-5916,-15040,-11840},
        {180,-5921,-15040,-11840}, {181,-5921,-15036,-11844},
        {899,-5921,-11845,-15035}, {900,-5921,-11840,-15040},
        {901,-5921,-11844,-15036}, {1619,-5921,-15035,-11845},
        {1620,-5921,-15040,-11840}, {1621,-5917,-15040,-11840},
        {1799,-5126,-15040,-11840}, {1800,-5121,-15040,-11840},
        {UINT32_MAX,-5588,-15040,-11840}
    };
    client *c = open_client(disc_path);
    const s32 eye[3] = {-1920,-5600,-15200}, target[3] = {-1920,-5600,-11800};
    s32 spawn[3];
    q2_rng rng;
    q2_fx_beam moving[2];
    u8 centre[2];
    u32 i, side, row, seen = 0;
    int k;
    CHECK(client_load_zone(c, "SECURITY", 0));
    memcpy(spawn, c->sim[0].player[0].pos, sizeof(spawn));
    q2_sim_set_targets(&c->sim[0], NULL, 0);
    centre[0] = named_flags(c, "LaserRing CA");
    centre[1] = named_flags(c, "LaserRing CB");
    rng = c->sim[0].fx_rng;
    for (i = 0; i < Q2PSX_ARRAY_COUNT(gates); i++) for (side = 0; side < 2; side++) {
        u32 want = side ? gates[i].left : gates[i].right;
        c->sim[0].level_time = (s32)gates[i].time;
        c->sim[0].tick_count = 42 + side;
        c->frame_index = 99 - (int)side; /* Intentionally opposite parity. */
        q2_fx_beams_reset(&c->sim[0].fx);
        client_native_mission_frame(c, true);
        CHECK(security_laser_mask(c) == want && c->sim[0].fx.beam_count == 2);
        CHECK(named_flags(c, "LaserRing CA") == centre[0] &&
              named_flags(c, "LaserRing CB") == centre[1]);
        seen |= security_laser_mask(c);
        q2_fx_beams_reset(&c->sim[0].fx);
        client_native_mission_frame(c, false);
        CHECK(security_laser_mask(c) == want && c->sim[0].fx.beam_count == 2);
        CHECK((u32)c->sim[0].level_time == gates[i].time && c->sim[0].tick_count == 42 + side);
    }
    CHECK(seen == 0xFFFFFFu);
    CHECK(!memcmp(&rng, &c->sim[0].fx_rng, sizeof(rng)));
    for (i = 0; i < Q2PSX_ARRAY_COUNT(paths); i++) for (side = 0; side < 2; side++) {
        c->sim[0].level_time = (s32)paths[i].time;
        c->sim[0].tick_count = side;
        q2_fx_beams_reset(&c->sim[0].fx);
        client_native_mission_frame(c, false);
        CHECK(c->sim[0].fx.beam_count == 2);
        for (row = 0; row < 2; row++) {
            const q2_fx_beam *beam = &c->sim[0].fx.beam[row];
            q2_coll_node cell;
            s32 from[3] = {side ? -3920 : 79, -5921, row ? -11840 : -15040};
            s32 to[3] = {side ? -5120 : 1276, paths[i].y, row ? paths[i].z1 : paths[i].z0};
            CHECK(!memcmp(beam->from, from, sizeof(from)) && !memcmp(beam->to, to, sizeof(to)));
            CHECK(q2_collision_get_node(&c->sim[0].coll_primary,
                (u32)q2_coll_find_node(&c->sim[0].coll_primary, from, -1, true), &cell));
            CHECK(beam->area == cell.contents && beam->radius == 16 &&
                  beam->tube == c->fx_tables.beam[1].tube);
        }
    }
    c->sim[0].level_time = 30;
    c->sim[0].tick_count = 1;
    q2_fx_beams_reset(&c->sim[0].fx);
    client_native_mission_frame(c, true);
    printf("SECURITY laser cycle: mask %06X, native beams %u\n", security_laser_mask(c), c->sim[0].fx.beam_count);
    CHECK(security_laser_mask(c) == 0x101001u);
    CHECK(c->sim[0].fx.beam_count == 2);
    q2_fx_beams_reset(&c->sim[0].fx);
    client_laserbeams_frame(c, false);
    capture_native(c, output, "security-laser-cycle", eye, target);
    printf("SECURITY laser camera %d,%d,%d\n", c->cam.pos[0], c->cam.pos[1], c->cam.pos[2]);
    c->sim_enabled = true;
    /* Each moving segment is a kind-1 damaging beam, independent of visual
     * suppression and pool exhaustion. Render-only callbacks never hurt. */
    for (side = 0; side < 2; side++) {
        c->sim[0].level_time = 0;
        c->sim[0].tick_count = side;
        q2_fx_beams_reset(&c->sim[0].fx);
        client_native_mission_frame(c, false);
        memcpy(moving, c->sim[0].fx.beam, sizeof(moving));
        for (row = 0; row < 2; row++) {
            security_beam_player(c, &moving[row]);
            client_native_mission_frame(c, false);
            CHECK(c->sim[0].combat.inv.health == 2000);
            client_native_mission_frame(c, true);
            CHECK(c->sim[0].combat.inv.health == 1999 && c->sim[0].combat.self.last_mod == 16);
            c->no_lasers = true;
            q2_fx_beams_reset(&c->sim[0].fx);
            client_native_mission_frame(c, true);
            CHECK(!c->sim[0].fx.beam_count && c->sim[0].combat.inv.health == 1998);
            c->no_lasers = false;
            c->sim[0].fx.beam_count = Q2_FX_BEAMS_MAX;
            client_native_mission_frame(c, true);
            CHECK(c->sim[0].fx.beam_count == Q2_FX_BEAMS_MAX && c->sim[0].combat.inv.health == 1997);
        }
        /* At the far end of the triangular sweep the two segments cross;
         * the same actor receives both independent hits there. */
        c->sim[0].level_time = 900;
        q2_fx_beams_reset(&c->sim[0].fx);
        client_native_mission_frame(c, false);
        security_beam_player(c, &c->sim[0].fx.beam[0]);
        client_native_mission_frame(c, true);
        CHECK(c->sim[0].combat.inv.health == 1998);
    }
    memcpy(c->sim[0].player[0].pos, spawn, sizeof(spawn));
    /* DrawLaserBeams precedes the callback's DisableEvent calls. A bank
     * crossing its dark boundary is still submitted once, then disappears. */
    {
        q2_laserbeam_set authored = c->lasers;
        q2_event_dir_entry e;
        u32 raiser = 0, found = 0;
        for (i = 0; i < c->sim[0].events.dir_count; i++)
            if (q2_events_get_dir_entry(&c->sim[0].events, i, &e) && !strcmp(e.name, "LaserBank LA"))
                raiser = e.offset;
        for (i = 0; i < authored.count; i++) if (authored.beam[i].raiser == raiser) {
            c->lasers.count = 1; c->lasers.beam[0] = authored.beam[i]; found++;
            break;
        }
        CHECK(found == 1);
        c->sim[0].level_time = 30; c->sim[0].tick_count = 1;
        client_native_mission_frame(c, false);
        c->sim[0].level_time = 60;
        q2_fx_beams_reset(&c->sim[0].fx);
        client_level_effects_frame(c, false);
        CHECK(c->laser_drawn == 1 && c->sim[0].fx.beam_count == 3);
        CHECK(security_laser_mask(c) == 0x102000u);
        CHECK(!memcmp(c->sim[0].fx.beam[0].from, c->lasers.beam[0].from, sizeof(moving[0].from)));
        q2_fx_beams_reset(&c->sim[0].fx);
        client_level_effects_frame(c, false);
        CHECK(!c->laser_drawn && c->sim[0].fx.beam_count == 2);
        c->lasers = authored;
    }
    /* Creature hits must be drained before the next actor refresh. */
    c->sim[0].level_time = 0;
    q2_fx_beams_reset(&c->sim[0].fx);
    client_native_mission_frame(c, false);
    moving[0] = c->sim[0].fx.beam[0];
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *m = &c->creatures.set.monsters[i];
        q2_monster saved;
        if (!m->in_use || m->dead || !m->takedamage || !m->motion.ready) continue;
        saved = *m;
        for (k = 0; k < 3; k++)
            m->motion.ent.pos[k] = moving[0].from[k] + (moving[0].to[k] - moving[0].from[k]) / 4;
        m->health = 2000;
        q2_actor_from_monster(&c->cre_actor[i], m);
        memset(c->cre_target, 0, c->creatures.set.count * sizeof(*c->cre_target));
        c->cre_target[i] = &c->cre_actor[i];
        q2_sim_set_targets(&c->sim[0], c->cre_target, c->creatures.set.count);
        security_beam_player(c, &moving[0]);
        client_native_mission_frame(c, true);
        CHECK(m->health == 1999 && c->sim[0].combat.inv.health == 2000);
        CHECK(m->health == c->cre_actor[i].health && c->cre_actor[i].last_mod == 16);
        CHECK(!c->cre_actor[i].damage_reaction_pending);
        *m = saved;
        q2_actor_from_monster(&c->cre_actor[i], m);
        break;
    }
    CHECK(i < c->creatures.set.count);
    q2_sim_set_targets(&c->sim[0], NULL, 0);
    memcpy(c->sim[0].player[0].pos, spawn, sizeof(spawn));
    c->sim[0].level_time = 575;
    c->sim[0].tick_count = 17;
    q2_fx_beams_reset(&c->sim[0].fx);
    client_native_mission_frame(c, false);
    memcpy(moving, c->sim[0].fx.beam, sizeof(moving));
    CHECK(security_laser_mask(c) == 0x208002u);
    c = cold_population_save(c, disc_path, output);
    CHECK(c->sim[0].level_time == 575 && c->sim[0].tick_count == 17);
    CHECK(security_laser_mask(c) == 0x208002u);
    q2_fx_beams_reset(&c->sim[0].fx);
    client_native_mission_frame(c, false);
    CHECK(c->sim[0].fx.beam_count == 2);
    for (row = 0; row < 2; row++) {
        CHECK(!memcmp(c->sim[0].fx.beam[row].from, moving[row].from, sizeof(moving[row].from)));
        CHECK(!memcmp(c->sim[0].fx.beam[row].to, moving[row].to, sizeof(moving[row].to)));
    }
    /* No private NMIS state is needed: both phase inputs predate that chunk. */
    CHECK(client_capture(c));
    c->snapshot.native_mission_present = false;
    CHECK(client_apply_save(c, &c->snapshot));
    CHECK(c->sim[0].level_time == 575 && c->sim[0].tick_count == 17);
    q2_fx_beams_reset(&c->sim[0].fx);
    client_native_mission_frame(c, false);
    CHECK(security_laser_mask(c) == 0x208002u && c->sim[0].fx.beam_count == 2);
    /* The same module stays loaded at the two seams. Neither other zone runs
     * this course callback, and the shared clock survives the return. */
    for (k = 1; k <= 2; k++) {
        c->carry_player = c->carry_same_map = true;
        CHECK(client_load_zone(c, "SECURITY", k));
        q2_fx_beams_reset(&c->sim[0].fx);
        client_native_mission_frame(c, false);
        CHECK(!c->sim[0].fx.beam_count && c->sim[0].tick_count == 17);
    }
    c->carry_player = c->carry_same_map = true;
    CHECK(client_load_zone(c, "SECURITY", 0));
    q2_fx_beams_reset(&c->sim[0].fx);
    client_native_mission_frame(c, false);
    CHECK(c->sim[0].level_time == 575 && security_laser_mask(c) == 0x208002u);
    CHECK(c->sim[0].fx.beam_count == 2);
    for (k = 0; k < 3; k++) {
        c->mp_enabled = k == 0;
        c->restoring_save = k == 1 ? &c->snapshot : NULL;
        c->sim[0].event_rt.initial_pass = k == 2;
        c->sim[0].level_time = 0;
        q2_fx_beams_reset(&c->sim[0].fx);
        client_native_mission_frame(c, false);
        CHECK(!c->sim[0].fx.beam_count && security_laser_mask(c) == 0x208002u);
    }
    c->mp_enabled = c->sim[0].event_rt.initial_pass = false;
    c->restoring_save = NULL;
    close_client(c);
    /* Exercise the normal fixed-step headless path, rather than only calling
     * the callback with clocks supplied by the fixture. */
    c = open_client(disc_path);
    CHECK(client_load_zone(c, "SECURITY", 0));
    c->sim[0].level_time = 0; c->sim[0].tick_count = 0;
    c->sim[0].dt_accum = 0; c->sim[0].dt_frac = 0;
    for (k = 0; k < 30; k++, c->frame_index++)
        client_input_simulated(c, 1.0f / 30.0f);
    printf("SECURITY fixed frames: clock %d, ticks %u, mask %06X\n",
        c->sim[0].level_time, c->sim[0].tick_count, security_laser_mask(c));
    CHECK(c->sim[0].level_time == 300 && c->sim[0].tick_count == 15);
    CHECK(security_laser_mask(c) == 0x104000u);
    close_client(c);
}

static void test_disc_security_orb(const char *disc_path, const char *output)
{
    client *c = open_client(disc_path);
    q2_mover *lift;
    s32 ring, hex;
    q2_scene_node node;
    q2_move_target base_box;
    u32 i;
    int k;
    CHECK(client_load_zone(c, "SECURITY", 2));
    lift = lab_lift(c, "HexLift");
    ring = client_named_node(c, "LiftRing1");
    hex = client_named_node(c, "HexLift");
    printf("SECURITY bindings: HexLift node %d, LiftRing1 node %d, mover %d\n", hex, ring, lift != NULL);
    {
        q2_map_name_table names;
        q2_map_name entry;
        CHECK(q2_map_names_parse(&names, &c->zone.zone) == Q2_OK);
        for (i = 0; i < names.count; i++) if (q2_map_name_get(&names, i, &entry))
            printf("SECURITY name %s -> %u of %u\n", entry.name, entry.id, c->zone.scene.node_count);
    }
    CHECK(lift && ring == -1 && hex >= 0); /* PAL has "ring", not "LiftRing1". */
    if (!lift || hex < 0) { close_client(c); return; }
    CHECK(q2_scene_get_node(&c->zone.scene, (u32)hex, &node));
    CHECK(lab_lift_box(c, "HexLift") != NULL);
    base_box = *lab_lift_box(c, "HexLift");
    printf("SECURITY HexLift: node %d ring %d prim %u offset %d target %d state %u; bounds %d,%d,%d to %d,%d,%d\n",
        hex, ring, lift->prim, lift->offset, lift->target, lift->state,
        node.bbox_min[0], node.bbox_min[1], node.bbox_min[2], node.bbox_max[0], node.bbox_max[1], node.bbox_max[2]);
    lift->offset = lift->target;
    lift->state = Q2_MV_OPEN;
    q2_sim_movers_update(&c->sim[0], &c->movers);
    c->sim[0].player[0].pos[1] = -8899 + Q2_EYE_BASE;
    client_native_mission_frame(c, true);
    CHECK(!lift->external);
    c->sim[0].player[0].pos[1]--;
    client_native_mission_frame(c, true);
    CHECK(lift->external);
    printf("SECURITY HexLift control: held %u\n", lift->external);
    mission_call(c, "OrbActive");
    q2_random_seed(1);
    c->sim[0].cur_dt = 10;
    c->sim[0].level_time = 0;
    q2_fx_beams_reset(&c->sim[0].fx);
    client_native_mission_frame(c, true);
    for (i = 0; i < 5; i++) c->native_mission.orb_arc[i].life = 10;
    q2_fx_beams_reset(&c->sim[0].fx);
    client_native_mission_frame(c, true);
    CHECK(c->sim[0].fx.beam_count == 7);
    printf("SECURITY expiring orb arcs: %u beams\n", c->sim[0].fx.beam_count);
    {
        const s32 eye[3] = {-3500,-10800,-21700}, at[3] = {-1918,-10000,-23996};
        q2_fx_beams_reset(&c->sim[0].fx);
        capture_native(c, output, "security-orb-expiry", eye, at);
    }
    c->sim_enabled = true;
    /* The held lift's unit word survives cold saves even while its zone is
     * absent. The generic mover cannot close or retrigger the owned object. */
    CHECK(c->native_mission.flags & Q2_NATIVE_SECURITY_LIFT_HELD);
    for (k = 0; k < 5; k++) {
        lift->triggered = 1;
        q2_movers_tick(&c->movers, 30000, 0);
        q2_sim_movers_update(&c->sim[0], &c->movers);
        CHECK(lift->offset == -1440 && lift->external);
    }
    c = cold_population_save(c, disc_path, output);
    lift = lab_lift(c, "HexLift");
    CHECK(lift && lift->external && lift->offset == -1440);
    CHECK(lab_lift_box(c, "HexLift") != NULL);
    for (k = 0; k < 3; k++) {
        CHECK(lab_lift_box(c, "HexLift")->min[k] == base_box.min[k] + (k == 1 ? -1440 : 0));
        CHECK(lab_lift_box(c, "HexLift")->max[k] == base_box.max[k] + (k == 1 ? -1440 : 0));
    }
    CHECK(lab_lift_box(c, "HexLift")->dy == 0);
    CHECK(client_load_zone(c, "SECURITY", 1));
    c = cold_population_save(c, disc_path, output);
    CHECK(c->native_mission.flags & Q2_NATIVE_SECURITY_LIFT_HELD);
    CHECK(client_load_zone(c, "SECURITY", 2));
    lift = lab_lift(c, "HexLift");
    CHECK(lift && lift->external && lift->offset == -1440);
    CHECK(client_load_zone(c, "JAIL2", 0));
    c = cold_population_save(c, disc_path, output);
    CHECK(client_load_zone(c, "SECURITY", 2));
    lift = lab_lift(c, "HexLift");
    CHECK(lift && lift->external && lift->offset == -1440);
    CHECK(client_capture(c));
    c->snapshot.native_mission_present = false;
    CHECK(client_apply_save(c, &c->snapshot));
    lift = lab_lift(c, "HexLift");
    CHECK(lift && !lift->external && !(c->native_mission.flags & Q2_NATIVE_SECURITY_LIFT_HELD));
    /* The ring name is absent from PAL. A temporary in-memory name binding
     * checks its conditional visibility without changing the shipped asset. */
    {
        const dat_chunk *names = c->zone.zone.chunk[Q2_ZONE_MAP_NAMES];
        u8 old_name[12], old_hidden;
        u8 *entry = (u8 *)names->data + 4*16;
        memcpy(old_name, entry, sizeof(old_name));
        memset(entry, 0, sizeof(old_name)); memcpy(entry, "LiftRing1", 9);
        ring = client_named_node(c, "LiftRing1");
        CHECK(ring == 208);
        old_hidden = c->node_hidden[ring];
        for (k = 0; k < 6; k++) {
            c->mp_enabled = k == 0;
            c->restoring_save = k == 1 ? &c->snapshot : NULL;
            c->sim[0].event_rt.initial_pass = k == 2;
            c->sim[0].player_count = k == 3 ? 0 : 1;
            lift->offset = k == 4 ? lift->target + 1 : lift->target;
            c->sim[0].player[0].pos[1] = -8900 + Q2_EYE_BASE;
            client_native_mission_frame(c, k != 5);
            CHECK(!lift->external && !(c->native_mission.flags & Q2_NATIVE_SECURITY_LIFT_HELD));
            if (k) CHECK(c->node_hidden[ring] == (k != 4));
        }
        c->mp_enabled = c->sim[0].event_rt.initial_pass = false;
        c->restoring_save = NULL;
        c->sim[0].player_count = 1;
        c->sim[0].player[0].pos[1] = -8899 + Q2_EYE_BASE;
        client_native_mission_frame(c, true);
        CHECK(c->node_hidden[ring] && !lift->external);
        c->sim[0].player[0].pos[1]--;
        client_native_mission_frame(c, true);
        CHECK(lift->external && (c->native_mission.flags & Q2_NATIVE_SECURITY_LIFT_HELD));
        memcpy(entry, old_name, sizeof(old_name));
        c->node_hidden[ring] = old_hidden;
    }
    /* Expiry keeps the last segment, including damage. Negative residuals
     * round-trip, then renew on the next tick rather than leaving a gap. */
    mission_call(c, "OrbActive");
    c->sim[0].level_time = 0; c->sim[0].cur_dt = 30;
    q2_sim_set_targets(&c->sim[0], NULL, 0);
    for (i = 0; i < 5; i++) {
        c->native_mission.orb_arc[i].life = 1;
        memset(c->native_mission.orb_arc[i].velocity, 0, sizeof(c->native_mission.orb_arc[i].velocity));
        c->native_mission.orb_arc[i].to[0] = i ? 6000 : -7000;
        c->native_mission.orb_arc[i].to[1] = -9997;
        c->native_mission.orb_arc[i].to[2] = -23996 + 1000*(s32)i;
    }
    q2_fx_beams_reset(&c->sim[0].fx);
    client_native_mission_frame(c, false);
    CHECK(c->sim[0].fx.beam_count == 7);
    security_beam_player(c, &c->sim[0].fx.beam[2]);
    for (k = 0; k < 3; k++) {
        for (i = 0; i < 5; i++) c->native_mission.orb_arc[i].life = 1;
        c->sim[0].combat.inv.health = 2000;
        c->no_lasers = k == 1;
        q2_fx_beams_reset(&c->sim[0].fx);
        if (k == 2) c->sim[0].fx.beam_count = Q2_FX_BEAMS_MAX;
        client_native_mission_frame(c, true);
        CHECK(c->sim[0].fx.beam_count == (k == 2 ? Q2_FX_BEAMS_MAX : k == 1 ? 0u : 7u));
        CHECK(c->sim[0].combat.inv.health == 1999 && c->sim[0].combat.self.last_mod == 16);
        for (i = 0; i < 5; i++) CHECK(c->native_mission.orb_arc[i].life == -29);
    }
    c->no_lasers = false;
    c = cold_population_save(c, disc_path, output);
    q2_sim_set_targets(&c->sim[0], NULL, 0);
    for (i = 0; i < 5; i++) CHECK(c->native_mission.orb_arc[i].life == -29);
    q2_fx_beams_reset(&c->sim[0].fx);
    client_native_mission_frame(c, false);
    CHECK(c->sim[0].fx.beam_count == 7 && c->sim[0].combat.inv.health == 1999);
    c->no_lasers = true;
    client_native_mission_frame(c, true);
    CHECK(c->native_mission.orb_arc[0].life > 0);
    c->no_lasers = false;
    /* A hit on arc 0 consumes blood-effect RNG. Every later renewal must
     * nevertheless match a no-hit run, because all five update first. */
    {
        q2_native_mission initial, no_hit;
        u32 rng_after;
        memset(c->native_mission.orb_arc, 0, sizeof(c->native_mission.orb_arc));
        c->native_mission.orb_arc[0].life = 100;
        c->native_mission.orb_arc[0].to[0] = -7000;
        c->native_mission.orb_arc[0].to[1] = -9997;
        c->native_mission.orb_arc[0].to[2] = -23996;
        initial = c->native_mission;
        c->sim[0].player[0].pos[0] = 20000;
        q2_rng_seed(&c->sim[0].fx_rng, 1);
        q2_fx_beams_reset(&c->sim[0].fx);
        client_native_mission_frame(c, true);
        no_hit = c->native_mission;
        rng_after = c->sim[0].fx_rng.state;
        security_beam_player(c, &c->sim[0].fx.beam[2]);
        c->native_mission = initial;
        q2_rng_seed(&c->sim[0].fx_rng, 1);
        q2_fx_beams_reset(&c->sim[0].fx);
        client_native_mission_frame(c, true);
        CHECK(c->sim[0].combat.inv.health == 1999 && c->sim[0].fx_rng.state != rng_after);
        CHECK(!memcmp(c->native_mission.orb_arc, no_hit.orb_arc, sizeof(no_hit.orb_arc)));
    }
    /* Every native beam uses the fixed origin's cell even while the sphere
     * bobs. Clock changes must not move existing endpoints during a draw. */
    {
        const s32 origin[3] = {-1918,-10178,-23996};
        const s32 times[] = {0,149,300,599,900,4095,123456};
        q2_coll_node cell;
        q2_native_mission before = c->native_mission;
        CHECK(q2_collision_get_node(&c->sim[0].coll_primary,
            (u32)q2_coll_find_node(&c->sim[0].coll_primary, origin, -1, true), &cell));
        for (k = 0; k < (int)Q2PSX_ARRAY_COUNT(times); k++) {
            c->sim[0].level_time = times[k];
            q2_fx_beams_reset(&c->sim[0].fx);
            client_native_mission_frame(c, false);
            CHECK(c->sim[0].fx.beam_count == 7);
            CHECK(!memcmp(before.orb_arc, c->native_mission.orb_arc, sizeof(before.orb_arc)));
            for (i = 0; i < 7; i++)
                CHECK(c->sim[0].fx.beam[i].area == cell.contents &&
                      c->sim[0].fx.beam[i].radius == (i < 2 ? 64 : 16));
        }
    }
    CHECK(client_load_zone(c, "SECURITY", 1));
    CHECK(client_load_zone(c, "SECURITY", 2));
    for (i = 0; i < 5; i++) CHECK(c->native_mission.orb_arc[i].life == 0);
    snprintf(c->first_map, sizeof(c->first_map), "SECURITY");
    client_start_game(c);
    CHECK(!(c->native_mission.flags & Q2_NATIVE_SECURITY_LIFT_HELD));
    CHECK(client_load_zone(c, "SECURITY", 2));
    CHECK(!lab_lift(c, "HexLift")->external);
    close_client(c);
}

static client *sweep_use_client;
static u32 sweep_use_calls, sweep_use_beams, sweep_use_groups;

static void sweep_use_probe(q2_monster *m)
{
    u32 i;
    (void)m;
    sweep_use_calls++;
    sweep_use_beams = sweep_use_client->sim[0].fx.beam_count;
    sweep_use_groups = 0;
    for (i = 0; i < sweep_use_client->sim[0].fx.group_count; i++)
        if (sweep_use_client->sim[0].fx.group[i].life) sweep_use_groups++;
    q2_rng_next(&sweep_use_client->sim[0].fx_rng);
}

static void test_disc_native_sweeps(const char *disc_path, const char *output)
{
    const char *maps[] = {"LAB", "BOSS1"};
    /* BIOS seed 1, LAB+0xBA4/BOSS1+0xD70: X/Z signed >>8, -rand >>13. */
    static const s16 first_velocity[4][3] = {{1,-2,-42},{4,-1,57},{25,-2,-36},{-49,-2,-54}};
    int map, k;
    u32 i, j;
    for (map = 0; map < 2; map++) {
        client *c = open_client(disc_path);
        const dat_chunk *lb;
        s32 from[2][3], end[2][3];
        q2_coll_node source[2];
        CHECK(client_load_zone(c, maps[map], 1));
        lb = c->common.chunk[Q2_COMMON_LEVEL_BIN];
        CHECK(lb && lb->size >= (map ? 0x1E60u : 0x16C0u));
        if (!lb) { close_client(c); continue; }
        for (i = 0; i < 2; i++) {
            u32 offset = map ? (i ? 0x1E24 : 0x1E40) : 0x1680 + 24*i;
            s32 cell;
            for (k = 0; k < 3; k++) {
                from[i][k] = q2_rd_s32(lb->data + offset + 4*k);
                end[i][k] = q2_rd_s32(lb->data + offset + 12 + 4*k);
            }
            cell = q2_coll_find_node(&c->sim[0].coll_primary, from[i], -1, true);
            CHECK(cell >= 0 && q2_collision_get_node(&c->sim[0].coll_primary, (u32)cell, &source[i]));
            printf("%s sweep %u: origin cell %d, area %u\n", maps[map], i, cell, source[i].contents);
        }
        c->sim[0].cur_dt = 10;
        c->sim[0].player[0].pos[0] = 30000;
        q2_sim_set_targets(&c->sim[0], NULL, 0);
        c->native_mission.lab_phase[0] = c->native_mission.lab_phase[1] = 8192;
        c->native_mission.flags &= ~(Q2_NATIVE_LASER0_OFF | Q2_NATIVE_LASER1_OFF);
        memset(c->sim[0].fx.group, 0, sizeof(c->sim[0].fx.group));
        q2_fx_beams_reset(&c->sim[0].fx);
        q2_rng_seed(&c->sim[0].fx_rng, 1);
        client_native_mission_frame(c, true);
        CHECK(c->sim[0].fx.beam_count == (map ? 4u : 2u));
        for (i = 0; i < 2; i++) {
            const q2_fx_beam *beam = &c->sim[0].fx.beam[map ? 1+2*i : i];
            s32 expected[3], phase = map ? (i ? 120 : 1150) : 8172;
            u32 groups = 0;
            memcpy(expected, end[i], sizeof(expected));
            expected[map ? 0 : i ? 2 : 0] += q2_sin12(phase) * (map ? 250 : 400) / 4096;
            expected[map ? 2 : i ? 0 : 2] += (map ? q2_cos12(phase) : q2_sin12(phase*8)) * (map ? 250 : 100) / 4096;
            CHECK(!memcmp(beam->to, expected, sizeof(expected)));
            CHECK(!memcmp(beam->from, from[i], sizeof(from[i])) && beam->area == source[i].contents);
            for (j = 0; j < c->sim[0].fx.group_count; j++) {
                const q2_fx_group *group = &c->sim[0].fx.group[j];
                if (!group->life || memcmp(group->origin, beam->to, sizeof(group->origin))) continue;
                groups++;
                printf("%s sweep %u: endpoint group area %u, native area %u\n",
                    maps[map], i, group->area, source[map ? 1 : i].contents);
                CHECK(group->area == source[map ? 1 : i].contents);
                CHECK(group->life == (map ? 5 : 10) && group->count == 15);
                CHECK(group->ramp[0] == q2_fx_ramp_at(&c->fx_tables, 0) && group->ramp[1] == group->ramp[0]);
                for (k = 0; k < 3; k++) {
                    u32 v;
                    CHECK(group->vel[k] == first_velocity[0][k] && group->accel[k] == 0);
                    for (v = 1; v < 4; v++)
                        CHECK(group->rel_vel[v-1][k] == first_velocity[v][k] - first_velocity[0][k]);
                }
            }
            printf("%s sweep %u: endpoint groups %u\n", maps[map], i, groups);
            CHECK(groups == 1);
        }
        if (map) {
            const s32 eye[3] = {18000,-18800,-4000}, at[3] = {16300,-17000,640};
            capture_native(c, output, "boss1-sweep-sparks", eye, at);
        } else {
            const s32 eye[3] = {-2700,-1800,3500}, at[3] = {-1511,-1100,3750};
            capture_native(c, output, "lab-sweep-sparks", eye, at);
        }
        /* Advance the actual particle integrator and render target at a fixed
         * 10-tick step so the restored point bursts have time to spread. */
        if (output) {
            char path[1024];
            for (k = 0; k < 6; k++) {
                q2_fx_tick(&c->sim[0].fx);
                c->sim[0].level_time += 10;
                client_native_mission_frame(c, true);
                q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
                client_frame(c);
            }
            snprintf(path, sizeof(path), "%s/%s-sweep-spread.ppm", output, map ? "boss1" : "lab");
            CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);
        }
        c->sim_enabled = true;
        if (!map) {
            c->native_mission.lab_phase[0] = c->native_mission.lab_phase[1] = 1;
            c->sim[0].cur_dt = 30;
            q2_fx_beams_reset(&c->sim[0].fx);
            client_native_mission_frame(c, true);
            CHECK(c->native_mission.lab_phase[0] == -59 && c->native_mission.lab_phase[1] == -59);
            CHECK(c->sim[0].fx.beam_count == 2);
            c = cold_population_save(c, disc_path, output);
            CHECK(c->native_mission.lab_phase[0] == -59 && c->native_mission.lab_phase[1] == -59);
            q2_fx_beams_reset(&c->sim[0].fx);
            client_native_mission_frame(c, false);
            CHECK(!c->sim[0].fx.beam_count && c->native_mission.lab_phase[0] == -59);
            client_native_mission_frame(c, true);
            CHECK(!c->sim[0].fx.beam_count && c->native_mission.lab_phase[1] == -59);
            mission_call(c, "Laser0"); mission_call(c, "Laser1");
            CHECK(c->native_mission.lab_phase[0] == 8192 && c->native_mission.lab_phase[1] == 8192);
        }
        /* LAB's final tick still hits and suppresses its endpoint burst.
         * BOSS1 emits them even
         * after hitting an actor. Drawing flags and a full beam pool cannot
         * suppress the one point of native kind-1 damage. */
        for (k = 0; k < 3; k++) {
            q2_fx_beam beam;
            u32 groups = 0;
            c->sim[0].cur_dt = 10;
            c->native_mission.lab_phase[0] = c->native_mission.lab_phase[1] = 1;
            c->native_mission.boss_phase[0] = 1000; c->native_mission.boss_phase[1] = 0;
            c->sim[0].player[0].pos[0] = 30000;
            q2_sim_set_targets(&c->sim[0], NULL, 0);
            q2_fx_beams_reset(&c->sim[0].fx);
            client_native_mission_frame(c, true);
            beam = c->sim[0].fx.beam[map ? 1 : 0];
            security_beam_player(c, &beam);
            c->native_mission.lab_phase[0] = c->native_mission.lab_phase[1] = 1;
            c->native_mission.boss_phase[0] = 1000; c->native_mission.boss_phase[1] = 0;
            memset(c->sim[0].fx.group, 0, sizeof(c->sim[0].fx.group));
            q2_fx_beams_reset(&c->sim[0].fx);
            c->no_lasers = k == 1;
            if (k == 2) c->sim[0].fx.beam_count = Q2_FX_BEAMS_MAX;
            client_native_mission_frame(c, true);
            CHECK(c->sim[0].combat.inv.health == 1999 && c->sim[0].combat.self.last_mod == 16);
            for (j = 0; j < c->sim[0].fx.group_count; j++)
                if (c->sim[0].fx.group[j].life && !memcmp(c->sim[0].fx.group[j].origin, beam.to, sizeof(beam.to))) groups++;
            CHECK(groups == (map && !c->no_lasers ? 1u : 0u));
            c->no_lasers = false;
        }
        /* Native use is at callback tail. Its RNG and side effects cannot
         * precede either sweep, and renderer-only refreshes cannot consume it. */
        {
            const char *model = map ? "RiderStand" : "DeadComm";
            const char *event = map ? "E_NearMakron" : "E_GotHead";
            u32 bit = map ? Q2_NATIVE_PROJECTION_USED : Q2_NATIVE_COMMANDER_USED;
            q2_monster *actor = NULL;
            for (i = 0; i < c->creatures.set.count; i++) {
                const char *name = q2_creature_world_model_name(&c->creatures, &c->creatures.set.monsters[i]);
                if (name && !strcmp(name, model)) { actor = &c->creatures.set.monsters[i]; break; }
            }
            CHECK(!map || actor != NULL); /* DeadComm is absent from LAB zone 1. */
            CHECK(q2_event_rt_disable_named(&c->sim[0].event_rt, event, true));
            if (!actor) {
                c->native_mission.flags &= ~bit;
                client_native_mission_frame(c, false);
                CHECK(!(c->native_mission.flags & bit));
                client_native_mission_frame(c, true);
                CHECK(c->native_mission.flags & bit); /* An absent actor still consumes the attempt. */
            } else {
                void (*use)(q2_monster *) = actor->use;
                actor->use = sweep_use_probe;
                actor->in_use = true;
                sweep_use_client = c; sweep_use_calls = 0;
                c->native_mission.flags &= ~bit;
                c->sim[0].player[0].pos[0] = 30000;
                c->native_mission.lab_phase[0] = c->native_mission.lab_phase[1] = 8192;
                memset(c->sim[0].fx.group, 0, sizeof(c->sim[0].fx.group));
                q2_fx_beams_reset(&c->sim[0].fx);
                client_native_mission_frame(c, false);
                CHECK(!sweep_use_calls && !(c->native_mission.flags & bit));
                q2_fx_beams_reset(&c->sim[0].fx);
                client_native_mission_frame(c, true);
                CHECK(sweep_use_calls == 1 && (c->native_mission.flags & bit));
                CHECK(sweep_use_beams == (map ? 4u : 2u) && sweep_use_groups == 2);
                client_native_mission_frame(c, true);
                CHECK(sweep_use_calls == 1);
                actor->use = use; sweep_use_client = NULL;
            }
        }
        close_client(c);
    }
}

/* The retail full-load route calls LevelBin export 0 at 0x80070F64.
 * The zone-gate route (0x8007901C) calls only its zone entry at 0x80079004.
 * Compare module storage separately from unit memory, whose flags/lifts survive. */
static q2_native_mission native_module_state(const client *c)
{
    q2_native_mission s = c->native_mission;
    s.flags &= Q2_NATIVE_PROJECTION_USED | Q2_NATIVE_COMMANDER_USED |
               Q2_NATIVE_REACTOR_ACTIVE | Q2_NATIVE_REACTOR_DRAINED |
               Q2_NATIVE_REACTOR_BEAM_OFF;
    memset(s.lift, 0, sizeof(s.lift));
    return s;
}

static void test_disc_native_lifecycle(const char *disc_path, const char *output)
{
    static const struct { const char *map; int zone, next; } cases[] = {
        {"LAB",1,0}, {"BOSS1",1,0}, {"SECURITY",0,1},
        {"BOSS2",1,0}, {"POWER1",1,2}, {"POWER2",3,4}
    };
    u32 i;
    for (i = 0; i < Q2PSX_ARRAY_COUNT(cases); i++) {
        client *c = open_client(disc_path);
        q2_native_mission fresh, armed, state;
        int request;
        printf("%s native lifetime: gates, cold saves, refused loads, restart/resupply\n", cases[i].map);
        CHECK(client_load_zone(c, cases[i].map, cases[i].zone));
        fresh = native_module_state(c);
        switch (i) {
        case 0:
            mission_call(c, "Laser0"); mission_call(c, "Laser1");
            c->native_mission.flags |= Q2_NATIVE_COMMANDER_USED;
            break;
        case 1:
            c->native_mission.boss_phase[0] = 1234;
            c->native_mission.boss_phase[1] = 2345;
            c->native_mission.flags |= Q2_NATIVE_PROJECTION_USED;
            break;
        case 2:
            c->native_mission.security_wall[0] = 300;
            c->native_mission.security_wall[3] = 750;
            c->native_mission.security_quake_phase = -17;
            c->native_mission.orb_arc[0].life = 10;
            c->native_mission.orb_arc[0].velocity[0] = -8;
            c->native_mission.orb_arc[0].to[2] = 1234;
            c->native_mission.flags |= Q2_NATIVE_SECURITY_BOSS;
            break;
        case 3:
            c->native_mission.boss2_quake_time = 401;
            c->native_mission.boss2_quake_phase = -17;
            c->native_mission.flags |= Q2_NATIVE_TRAIN_OPEN;
            break;
        case 4:
            c->native_mission.power1_next_blast = 1111;
            c->native_mission.power1_blast_phase = 5;
            break;
        case 5:
            c->native_mission.reactor_start = 1234;
            c->native_mission.reactor_alarm = 250;
            c->native_mission.reactor_glass = 7;
            c->native_mission.flags |= Q2_NATIVE_REACTOR_ACTIVE | Q2_NATIVE_REACTOR_DRAINED |
                                       Q2_NATIVE_REACTOR_BEAM_OFF;
            break;
        }
        /* An unrelated unit save word must survive every module reload. */
        c->native_mission.flags |= Q2_NATIVE_COMP1;
        CHECK(q2_native_mission_valid(&c->native_mission));
        armed = native_module_state(c);
        c = cold_population_save(c, disc_path, output);
        state = native_module_state(c);
        CHECK(!memcmp(&armed, &state, sizeof(state)));
        CHECK(!client_load_zone(c, cases[i].map, 99));
        state = native_module_state(c);
        CHECK(!memcmp(&armed, &state, sizeof(state)));
        c->carry_player = c->carry_same_map = true;
        CHECK(client_load_zone(c, cases[i].map, cases[i].next));
        /* POWER1's zone callbacks explicitly clear its deadline, not phase. */
        if (i == 4) armed.power1_next_blast = 0;
        state = native_module_state(c);
        CHECK(!memcmp(&armed, &state, sizeof(state)));
        c->carry_player = c->carry_same_map = true;
        CHECK(client_load_zone(c, cases[i].map, cases[i].zone));
        state = native_module_state(c);
        CHECK(!memcmp(&armed, &state, sizeof(state)));
        for (request = 0; request < 2; request++) {
            u32 unit_flags = c->native_mission.flags & ~state.flags;
            u32 serial = c->level_change_serial;
            /* Invoke the real menu request path without a host input device. */
            c->continues = 2;
            c->menu.request = request ? Q2_MREQ_RESUPPLY : Q2_MREQ_RESTART;
            client_menu_requests(c);
            CHECK(c->level_change_serial == serial + 1 && c->zone_index == cases[i].zone);
            CHECK(c->continues == (request ? 1 : 2));
            state = native_module_state(c);
            CHECK(!memcmp(&fresh, &state, sizeof(state)));
            CHECK((c->native_mission.flags & unit_flags) == unit_flags);
            if (i == 0 && !request) {
                const s32 eye[3] = {-2700,-1800,3500}, at[3] = {-1511,-1100,3750};
                q2_fx_beams_reset(&c->sim[0].fx);
                client_native_mission_frame(c, false);
                printf("LAB after restart: sweep phases %d,%d, beams %u\n",
                    c->native_mission.lab_phase[0], c->native_mission.lab_phase[1], c->sim[0].fx.beam_count);
                CHECK(!c->sim[0].fx.beam_count);
                capture_native(c, output, "lab-restart", eye, at);
                c->sim_enabled = true;
            }
            if (!request) {
                /* Re-arm the same pre-reload state to cover resupply too. */
                q2_native_mission *s = &c->native_mission;
                q2_native_lift lift[Q2_NATIVE_LIFT_COUNT];
                memcpy(lift, s->lift, sizeof(lift));
                *s = armed;
                s->flags |= unit_flags;
                memcpy(s->lift, lift, sizeof(lift));
                state = native_module_state(c);
            }
        }
        close_client(c);
    }
}

int main(int argc, char **argv)
{
    test_synthetic_batch();
    if (argc >= 3 && !strcmp(argv[1], "--native-lifecycle"))
        test_disc_native_lifecycle(argv[2], argc >= 4 ? argv[3] : NULL);
    if (argc >= 3 && !strcmp(argv[1], "--native-sweeps"))
        test_disc_native_sweeps(argv[2], argc >= 4 ? argv[3] : NULL);
    if (argc >= 3 && !strcmp(argv[1], "--security-orb"))
        test_disc_security_orb(argv[2], argc >= 4 ? argv[3] : NULL);
    if (argc >= 3 && !strcmp(argv[1], "--security-lasers"))
        test_disc_security_lasers(argv[2], argc >= 4 ? argv[3] : NULL);
    if (argc >= 3 && !strcmp(argv[1], "--quakes"))
        test_disc_quakes(argv[2], argc >= 4 ? argv[3] : NULL);
    if (argc >= 3 && !strcmp(argv[1], "--nativelifts"))
        test_disc_native_lifts(argv[2], argc >= 4 ? argv[3] : NULL);
    if (argc >= 3 && !strcmp(argv[1], "--lablifts"))
        test_disc_lab_lifts(argv[2], argc >= 4 ? argv[3] : NULL);
    if (argc >= 3 && !strcmp(argv[1], "--laserwalls")) {
        test_disc_laserwall(argv[2], "BASE3", argc >= 4 ? argv[3] : NULL);
        test_disc_laserwall(argv[2], "MAGDEMO", argc >= 4 ? argv[3] : NULL);
    }
    if (argc >= 3 && !strcmp(argv[1], "--boss1"))
        test_disc_boss_entrance(argv[2], argc >= 4 ? argv[3] : NULL);
    if (argc >= 3 && !strcmp(argv[1], "--power1"))
        test_disc_power1(argv[2], argc >= 4 ? argv[3] : NULL);
    if (argc >= 3 && !strcmp(argv[1], "--callbacks"))
        test_disc_power_callbacks(argv[2], argc >= 4 ? argv[3] : NULL);
    if (argc >= 3 && !strcmp(argv[1], "--selectors")) {
        test_disc_selectors(argv[2], argc >= 4 ? argv[3] : NULL);
        test_disc_security_progression(argv[2], argc >= 4 ? argv[3] : NULL);
        test_disc_train_unlock(argv[2], argc >= 4 ? argv[3] : NULL, false);
        test_disc_train_unlock(argv[2], argc >= 4 ? argv[3] : NULL, true);
    }
    if (argc >= 3 && !strcmp(argv[1], "--disc")) {
        test_disc_native_lifecycle(argv[2], argc >= 4 ? argv[3] : NULL);
        test_disc_native_sweeps(argv[2], argc >= 4 ? argv[3] : NULL);
        test_disc_security_orb(argv[2], argc >= 4 ? argv[3] : NULL);
        test_disc_security_lasers(argv[2], argc >= 4 ? argv[3] : NULL);
        test_disc_quakes(argv[2], argc >= 4 ? argv[3] : NULL);
        test_disc_batches(argv[2], argc >= 4 ? argv[3] : NULL);
        test_disc_dish(argv[2], "BASE3", argc >= 4 ? argv[3] : NULL);
        test_disc_dish(argv[2], "MAGDEMO", argc >= 4 ? argv[3] : NULL);
        test_disc_laserwall(argv[2], "BASE3", argc >= 4 ? argv[3] : NULL);
        test_disc_laserwall(argv[2], "MAGDEMO", argc >= 4 ? argv[3] : NULL);
        test_disc_pyramid(argv[2], argc >= 4 ? argv[3] : NULL);
        test_disc_bridge(argv[2], argc >= 4 ? argv[3] : NULL);
        test_disc_native_switches(argv[2], argc >= 4 ? argv[3] : NULL);
        test_disc_native_beams(argv[2], argc >= 4 ? argv[3] : NULL);
        test_disc_lab_lifts(argv[2], argc >= 4 ? argv[3] : NULL);
        test_disc_native_lifts(argv[2], argc >= 4 ? argv[3] : NULL);
        test_disc_boss_entrance(argv[2], argc >= 4 ? argv[3] : NULL);
        test_disc_power1(argv[2], argc >= 4 ? argv[3] : NULL);
        test_disc_power_callbacks(argv[2], argc >= 4 ? argv[3] : NULL);
        test_disc_selectors(argv[2], argc >= 4 ? argv[3] : NULL);
        test_disc_security_progression(argv[2], argc >= 4 ? argv[3] : NULL);
        test_disc_train_unlock(argv[2], argc >= 4 ? argv[3] : NULL, false);
        test_disc_train_unlock(argv[2], argc >= 4 ? argv[3] : NULL, true);
    }
    printf("%d population client checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
