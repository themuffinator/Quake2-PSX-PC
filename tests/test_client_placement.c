/* Creature placement through the actual client. Disc runs use headless fixed
 * frames and enumerate the disc rather than maintaining a sample map list. */
int q2_test_placement_client_main(int argc, char **argv);
#define main q2_test_placement_client_main
#include "../src/client/main.c"
#undef main
#include "aimove.h"

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
#include "client_fixture.h"

typedef struct placement_fixture {
    u8 nodes[72], planes[72], links[4];
    q2_collision coll;
    q2_move_target boxes[3];
    q2_move_world world;
} placement_fixture;

static void placement_init(placement_fixture *f)
{
    u32 i;
    memset(f, 0, sizeof(*f));
    for (i = 0; i < 3; i++) q2_wr_u32(f->nodes + 12 + i * 4, 4000);
    q2_wr_u16(f->nodes + 36 + 24, 6);
    for (i = 0; i < 6; i++) {
        u32 axis = i / 2;
        q2_wr_u16(f->planes + i * 12 + axis * 2, (u16)((i & 1) ? 4000 : 0));
        q2_wr_u16(f->planes + i * 12 + 6 + axis * 2, (u16)((i & 1) ? 4096 : -4096));
    }
    f->coll.nodes = f->nodes; f->coll.planes = f->planes; f->coll.links = f->links;
    f->coll.node_count = 1; f->coll.plane_count = 6;
    f->boxes[0].kind = Q2_MOVE_KIND_ENTITY; f->boxes[0].active = true;
    f->boxes[0].min[1] = f->boxes[0].max[1] = 1500;
    f->boxes[0].max[0] = f->boxes[0].max[2] = 2000;
    f->world.targets = f->boxes; f->world.count = 1;
}

static void test_support_edges(void)
{
    placement_fixture f;
    s32 pos[3] = {1000, 1500 - Q2_EYE_BASE, 1000};
    placement_init(&f);
    CHECK(q2_entity_drop_to_floor(&f.coll, &f.world, pos, 1024, NULL));
    CHECK(pos[1] == 1214); /* Beginning on the support plane must not fall through. */
    f.boxes[1] = f.boxes[0]; f.boxes[1].min[0] = 1250; f.boxes[1].max[0] = 1270;
    f.boxes[1].min[1] = 0; f.boxes[1].max[1] = 3500; f.world.count = 2;
    pos[1] = 684;
    CHECK(q2_entity_drop_to_floor(&f.coll, &f.world, pos, 1024, NULL));
    CHECK(pos[1] == 1214); /* A side overlap cannot hide the lower support. */
    f.boxes[1] = f.boxes[0]; f.boxes[1].min[1] = f.boxes[1].max[1] = 1800;
    pos[1] = 684;
    CHECK(q2_entity_drop_to_floor(&f.coll, &f.world, pos, 1024, NULL) && pos[1] == 1214);
    f.boxes[0].active = false; pos[1] = 684;
    CHECK(q2_entity_drop_to_floor(&f.coll, &f.world, pos, 1024, NULL) && pos[1] == 1514);
}

static void test_class_independent_placement(void)
{
    enum { LIST = Q2_POP_GROUP_SIZE + 4, SIZE = LIST + Q2_POP_SPAWN_SIZE + 4 };
    placement_fixture f;
    q2_population pop = {0};
    u8 bytes[SIZE] = {0};
    u32 id;
    placement_init(&f);
    memcpy(bytes, "Batch", 5); q2_wr_u32(bytes + 12, LIST);
    q2_wr_u32(bytes + LIST + 4, 1000); q2_wr_u32(bytes + LIST + 8, 1000);
    q2_wr_u32(bytes + LIST + 12, 1000);
    pop.data = bytes; pop.size = sizeof(bytes); pop.group_count = 1;
    for (id = 1; id < Q2_MONSTER_CLASS_COUNT; id++) {
        q2_monster_set set = {0};
        q2_wr_u32(bytes + LIST, id); q2_monster_set_register(&set, id);
        CHECK(q2_spawn_from_population(&set, &pop, &f.coll, &f.world, NULL) == Q2_OK);
        CHECK(set.count == 1 && set.monsters[0].pos[1] == 1214);
        q2_monster_set_free(&set);
    }
}

static void test_delayed_support_lifecycle(void)
{
    enum { LIST = Q2_POP_GROUP_SIZE + 4, SIZE = LIST + Q2_POP_SPAWN_SIZE + 4 };
    q2_creature_world *w = calloc(1, sizeof(*w));
    placement_fixture f;
    q2_move_target replacement;
    q2_monster m;
    u8 bytes[SIZE] = {0};
    unsigned scenario;
    if (!w) exit(2);
    memcpy(bytes, "Batch", 5); q2_wr_u32(bytes + 12, LIST);
    q2_wr_u32(bytes + LIST, 1);
    q2_wr_u32(bytes + LIST + 4, 1000); q2_wr_u32(bytes + LIST + 8, 1000);
    q2_wr_u32(bytes + LIST + 12, 1000);
    w->pop.data = bytes; w->pop.size = sizeof(bytes); w->pop.group_count = 1;
    w->pop_ready = true; w->set.monsters = &m; w->set.count = 1;
    w->placement_coll = &f.coll; w->placement_world = &f.world;
    for (scenario = 0; scenario < 7; scenario++) {
        s32 expected = 1214;
        placement_init(&f); q2_monster_init(&m); q2_level_reset();
        m.in_use = false; m.spawn_pending = true;
        m.pos[0] = m.pos[2] = 1000; m.pos[1] = 1214;
        switch (scenario) {
        case 0: /* A lift descends after the placeholder was placed. */
            f.boxes[0].min[1] += 20; f.boxes[0].max[1] += 20;
            expected = 1234; break;
        case 1: /* Re-drop from the authored hint, above the raised lift. */
            f.boxes[0].min[1] = f.boxes[0].max[1] = 1200;
            expected = 914; break;
        case 2: /* A removed hatch no longer supports a new spawn. */
            f.boxes[0].active = false; expected = 1738; break;
        case 3: /* Rebuilding solid targets must not leave a stale array. */
            replacement = f.boxes[0];
            replacement.min[1] = replacement.max[1] = 1600;
            f.world.targets = &replacement; expected = 1314; break;
        case 4: /* A resumed actor keeps its actual runtime origin. */
            m.spawn_initialized = true; m.pos[1] = expected = 1333; break;
        case 5: /* Invalid geometry must not activate an unplaced actor. */
            f.coll.node_count = 0;
            CHECK(q2_creature_world_summon(w, "Batch") == 0);
            CHECK(!m.in_use && m.spawn_pending && m.pos[1] == 1214);
            f.coll.node_count = 1; break;
        case 6: /* A dead actor must neither move nor respawn. */
            m.dead = true;
            CHECK(q2_creature_world_summon(w, "Batch") == 0);
            CHECK(!m.in_use && !m.spawn_pending && m.pos[1] == 1214);
            continue;
        }
        CHECK(q2_creature_world_summon(w, "Batch") == 1);
        CHECK(m.in_use && !m.spawn_pending && m.pos[1] == expected);
        CHECK(q2_creature_world_summon(w, "Batch") == 0);
        CHECK(m.pos[1] == expected);
        m.in_use = false;
        CHECK(q2_creature_world_summon(w, "Batch") == 0 && !m.in_use);
    }
    free(w);
}

/* An independent vertical-plane oracle: static hull contact is already in
 * origin coordinates; entity boxes need the fixed physical body inflation. */
static s32 platform_floor(const q2_move_world *world, const s32 from[3],
                           s32 mesh_y, s32 *box)
{
    s32 y = mesh_y;
    u32 i;
    *box = -1;
    for (i = 0; world && i < world->count; i++) {
        const q2_move_target *t = &world->targets[i];
        s32 top = t->min[1] - Q2_EYE_BASE;
        if (!t->active || t->kind != Q2_MOVE_KIND_ENTITY ||
            from[0] < t->min[0] - Q2_EYE_BASE || from[0] > t->max[0] + Q2_EYE_BASE ||
            from[2] < t->min[2] - Q2_EYE_BASE || from[2] > t->max[2] + Q2_EYE_BASE ||
            top < from[1] || top > y) continue;
        y = top; *box = (s32)i;
    }
    return y;
}

static void audit_disc(const char *path, int skill)
{
    disc *d = NULL;
    int fi;
    unsigned zones = 0, resident = 0, caught = 0, misses = 0, overlaps = 0;
    char maps[128][32] = {{0}};
    unsigned map_count = 0, class_count = 0;
    unsigned classes[Q2_MONSTER_CLASS_COUNT] = {0};
    CHECK(disc_open(&d, path) == Q2_OK);
    if (!d) return;
    q2_cre_set_skill(skill);
    for (fi = 0; fi < disc_file_count(d); fi++) {
        const char *p = disc_file_at(d, fi)->path, *slash;
        char map[32];
        int zone, consumed = 0;
        client *c;
        u32 i;
        if (*p == '/') p++;
        if (strncmp(p, "Q2DATA/LEVELS/", 14)) continue;
        p += 14; slash = strchr(p, '/');
        if (!slash || slash - p >= (ptrdiff_t)sizeof(map) ||
            sscanf(slash + 1, "ZONE%d.DAT%n", &zone, &consumed) != 1 ||
            !consumed || slash[1 + consumed]) continue;
        memcpy(map, p, (size_t)(slash - p)); map[slash - p] = 0;
        for (i = 0; i < map_count && strcmp(maps[i], map); i++) {}
        if (i == map_count && map_count < 128) strcpy(maps[map_count++], map);
        c = open_client(path);
        CHECK(client_load_zone(c, map, zone));
        zones++;
        for (i = 0; i < c->creatures.set.count; i++) {
            q2_monster *m = &c->creatures.set.monsters[i];
            q2_pop_group group;
            q2_pop_spawn spawn;
            s32 from[3], mesh[3], expected, box;
            u32 j;
            if ((!m->in_use && !m->spawn_pending) ||
                !q2_pop_get_group(&c->creatures.pop, m->group, &group) ||
                !q2_pop_get_spawn(&c->creatures.pop, &group, m->population_slot, &spawn)) continue;
            resident++; classes[m->pop_class_id]++;
            from[0] = spawn.x; from[1] = spawn.y - Q2_EYE_BASE - Q2_ENTITY_SPAWN_NUDGE;
            from[2] = spawn.z; memcpy(mesh, from, sizeof(mesh));
            if (!q2_entity_drop_to_floor(&c->sim[0].coll, NULL, mesh,
                                         Q2_ENTITY_DROP_DIST, NULL)) {
                printf("UNPLACED %s/%d %s slot %u class %u at %d,%d,%d\n",
                    map, zone, group.name, m->population_slot, m->pop_class_id,
                    m->pos[0], m->pos[1], m->pos[2]);
                CHECK(false);
                continue;
            }
            expected = platform_floor(&c->sim[0].move_world, from, mesh[1], &box);
            if (!m->cre_bind || !m->cre_bind->impl->scripted) {
                if (m->pos[1] != expected)
                    printf("MISPLACED %s/%d %s slot %u class %u actual %d expected %d\n",
                        map,zone,group.name,m->population_slot,m->pop_class_id,m->pos[1],expected);
                CHECK(m->pos[1] == expected);
            }
            if (box >= 0 && expected < mesh[1]) {
                caught++;
                printf("PLATFORM %s/%d %s slot %u %s delta %d actual %d expected %d\n",
                    map, zone, group.name, m->population_slot,
                    q2_creature_world_model_name(&c->creatures, m),
                    mesh[1] - expected, m->pos[1], expected);
                if (m->pos[1] != expected) misses++;
            }
            for (j = 0; j < c->sim[0].move_world.count; j++) {
                const q2_move_target *t = &c->sim[0].move_world.targets[j];
                if (!t->active || t->kind != Q2_MOVE_KIND_ENTITY ||
                    m->pos[0] <= t->min[0] || m->pos[0] >= t->max[0] ||
                    m->pos[2] <= t->min[2] || m->pos[2] >= t->max[2] ||
                    m->pos[1] + 286 <= t->min[1] || m->pos[1] - 286 >= t->max[1]) continue;
                overlaps++;
                printf("OVERLAP %s/%d %s slot %u %s at %d,%d,%d box %u top %d\n",
                    map, zone, group.name, m->population_slot,
                    q2_creature_world_model_name(&c->creatures, m),
                    m->pos[0], m->pos[1], m->pos[2], j, t->min[1]);
                CHECK(false);
                break;
            }
        }
        close_client(c);
    }
    for (fi = 0; fi < Q2_MONSTER_CLASS_COUNT; fi++) if (classes[fi]) class_count++;
    CHECK(zones > 0 && resident > 0 && caught > 0 && misses == 0 && overlaps == 0);
    printf("PLACEMENT AUDIT: skill %d maps %u zones %u resident %u classes %u platforms %u missed %u floor overlaps %u\n",
           skill,map_count,zones,resident,class_count,caught,misses,overlaps);
    disc_close(d); q2_cre_set_skill(1);
}

static void test_delayed_platform(const char *disc_path, const char *map,
                                   int zone, const char *batch)
{
    client *c = open_client(disc_path);
    q2_monster *m = NULL;
    q2_pop_group group;
    q2_pop_spawn spawn;
    s32 from[3], mesh[3], box = -1, expected;
    u32 i;
    CHECK(client_load_zone(c, map, zone));
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *candidate = &c->creatures.set.monsters[i];
        if (candidate->spawn_pending &&
            q2_pop_get_group(&c->creatures.pop, candidate->group, &group) &&
            !strcmp(group.name, batch)) { m = candidate; break; }
    }
    CHECK(m != NULL);
    if (!m) { close_client(c); return; }
    CHECK(q2_pop_get_spawn(&c->creatures.pop, &group, m->population_slot, &spawn));
    from[0] = spawn.x; from[1] = spawn.y - Q2_EYE_BASE - Q2_ENTITY_SPAWN_NUDGE;
    from[2] = spawn.z; memcpy(mesh, from, sizeof(mesh));
    CHECK(q2_entity_drop_to_floor(&c->sim[0].coll, NULL, mesh, Q2_ENTITY_DROP_DIST, NULL));
    expected = platform_floor(&c->sim[0].move_world, from, mesh[1], &box);
    CHECK(box >= 0 && expected < mesh[1]);
    if (box >= 0) {
        /* The resident target array is live state, just as after a lift tick.
         * Exercise delayed spawn with a lower support, then a repeated call. */
        c->sim[0].volumes[box].min[1] += 20;
        c->sim[0].volumes[box].max[1] += 20;
        CHECK(q2_creature_world_summon(&c->creatures, batch) > 0);
        CHECK(m->in_use && m->spawn_initialized && m->pos[1] == expected + 20);
        CHECK(q2_cre_origin(m)[1] == expected + 20);
        CHECK(q2_creature_world_summon(&c->creatures, batch) == 0);
        CHECK(m->pos[1] == expected + 20);
    }
    close_client(c);
}

static void capture_placement(const char *disc_path, const char *output,
                               const char *map, int zone, const char *batch,
                               u32 slot, const char *name)
{
    client *c = open_client(disc_path);
    q2_monster *m = NULL;
    q2_pop_group group;
    q2_pop_spawn spawn;
    s32 mesh[3], placed[3];
    u32 i, index = 0, frame;
    CHECK(client_load_zone(c, map, zone));
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *candidate = &c->creatures.set.monsters[i];
        if (candidate->population_slot == slot &&
            q2_pop_get_group(&c->creatures.pop, candidate->group, &group) &&
            !strcmp(group.name, batch)) { m = candidate; index = i; break; }
    }
    CHECK(m && c->cre_model_ok[index]);
    if (!m) { close_client(c); return; }
    if (m->spawn_pending) CHECK(q2_creature_world_summon(&c->creatures, batch) > 0);
    CHECK(m->in_use && m->spawn_initialized);
    CHECK(q2_pop_get_spawn(&c->creatures.pop, &group, slot, &spawn));
    CHECK(q2_spawn_place(&spawn, &c->sim[0].coll, NULL, mesh));
    memcpy(placed, q2_cre_origin(m), sizeof(placed));
    CHECK(mesh[1] > placed[1]);
    for (i = 0; i < c->creatures.set.count; i++)
        if (i != index) c->creatures.set.monsters[i].in_use = false;
    c->sim_enabled = false;
    c->frame_index = 1;
    c->cam.pos[0] = placed[0] + 1000;
    c->cam.pos[1] = placed[1] - 200;
    c->cam.pos[2] = placed[2] + 1300;
    c->cam.yaw = 2560; c->cam.pitch = c->cam.roll = 0;
    for (frame = 0; frame < 2; frame++) {
        char path[1024];
        /* Reproduce the old static-only drop in the same actual renderer and
         * camera. The second image uses the current authored spawn result. */
        memcpy(m->pos, frame ? placed : mesh, sizeof(m->pos));
        q2_link_entity(m, 1);
        CHECK(c->headless);
        q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
        client_frame(c);
        if (frame) CHECK(c->cre_drawn > 0);
        snprintf(path, sizeof(path), "%s/%s-%s.ppm", output, name,
                 frame ? "placed" : "static-only");
        CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);
    }
    close_client(c);
}

int main(int argc, char **argv)
{
    test_support_edges();
    test_class_independent_placement();
    test_delayed_support_lifecycle();
    if (argc >= 3 && !strcmp(argv[1], "--headless")) {
        int skill;
        if (argc < 5 || strcmp(argv[4], "--capture-only"))
            for (skill = 0; skill < 4; skill++) audit_disc(argv[2], skill);
        test_delayed_platform(argv[2], "SECURITY", 1, "Boss1");
        test_delayed_platform(argv[2], "LAB", 0, "Mercedes");
        if (argc >= 4) {
            capture_placement(argv[2], argv[3], "JAIL4", 2, "Zone2", 0, "insane");
            capture_placement(argv[2], argv[3], "LAB", 0, "Mercedes", 0, "ironmaiden");
            capture_placement(argv[2], argv[3], "LAB", 2, "Zone2", 6, "medic");
        }
    }
    printf("%u placement checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
