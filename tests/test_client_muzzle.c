/* Muzzle effects through the real client, with no window or host input. */
int q2_test_muzzle_client_main(int argc, char **argv);
#define main q2_test_muzzle_client_main
#include "../src/client/main.c"
#undef main
#include "aimove.h"

static int failures, checks;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    failures++; } } while (0)

#include "client_fixture.h"

static void player_delivery(void)
{
    static const q2_vm_key idle = { .duration = 30, .event = Q2_VM_EVENT_NONE };
    int weapon, phase;
    for (weapon = 1; weapon <= Q2_WID_COUNT; weapon++) {
        for (phase = 0; phase < 2; phase++) {
            client *c = calloc(1, sizeof(*c));
            q2_muzzle_light preset;
            bool flashes = q2_light_muzzle_preset((u8)weapon, &preset);
            s32 feet[3] = {0};
            int ammo;
            if (!c) exit(2);
            c->headless = c->sim_enabled = c->vm_ready = c->lights_ready = true;
            c->shoot = true;
            c->pad_frame = -2;
            c->give_powerup = -1;
            q2_sim_init(&c->sim[0], NULL, 50);
            q2_sim_spawn(&c->sim[0], feet, 0);
            c->sim[0].fire_from_input = false;
            c->sim[0].combat.weapon_id = weapon;
            c->sim[0].combat.inv.weapons = 0x7ff;
            for (ammo = 0; ammo < Q2_AMMO_COUNT; ammo++)
                c->sim[0].combat.inv.ammo[ammo] = 200;
            c->sim[0].dt_accum = phase ? c->sim[0].frame_dt - 1 : 0;
            c->vm_tables.clip[weapon][Q2_VM_IDLE].key = &idle;
            c->vm_tables.clip[weapon][Q2_VM_IDLE].count = 1;
            q2_vw_init(&c->vw[0], &c->vm_tables, weapon);
            c->vw[0].state = Q2_VM_IDLE;
            c->vw[0].left = c->vw[0].total = 30;
            c->vw_last_weapon[0] = weapon;
            q2_player_death_init(&c->death[0]);
            CHECK(q2_screen_init(&c->screen, Q2_VIDEO_PAL) == Q2_OK);

            /* A frame between logic ticks runs no player think, so the
             * driver (0x8003AD98) neither fires nor animates; the frame
             * that takes the tick does both. */
            client_input_simulated(c, 1.0f / 300.0f);
            CHECK((c->sim[0].level_time != 0) == (phase != 0));
            if (!phase) {
                CHECK(c->mp_shots[0] == 0 && c->ent_light_added == 0);
                c->frame_index++;
                c->sim[0].dt_accum = c->sim[0].frame_dt - 1;
                client_input_simulated(c, 1.0f / 300.0f);
                CHECK(c->sim[0].level_time != 0);
            }
            CHECK(c->mp_shots[0] == 1);
            CHECK(c->ent_light_added == (flashes ? 1u : 0u));
            if (flashes) {
                CHECK(q2_light_timed_active(&c->light_world) == (preset.ticks ? 1u : 0u));
                CHECK(c->light_world.dynamic_world_count == (preset.ticks ? 0u : 1u));
            }
            /* A second drain must not replay a shot or restart its light. */
            client_entity_events(c);
            CHECK(c->ent_light_added == (flashes ? 1u : 0u));
            if (flashes && preset.ticks)
                CHECK(q2_light_timed_active(&c->light_world) == 1);

            c->shoot = false;
            c->frame_index++;
            client_input_simulated(c, 1.0f / 300.0f);
            if (flashes) CHECK(c->light_world.dynamic_world_count == 0);
            CHECK(c->ent_light_added == (flashes ? 1u : 0u));
            if (weapon == Q2_WID_BLASTER) {
                q2_light light;
                /* Continuous bolt lighting survives display frames between
                 * logic ticks even though muzzle lighting expires there. */
                client_input_simulated(c, 1.0f / 25.0f);
                CHECK(c->light_world.dynamic_world_count == 1);
                light = c->light_world.dynamic_world[0];
                c->frame_index++;
                client_input_simulated(c, 1.0f / 300.0f);
                CHECK(c->light_world.dynamic_world_count == 1);
                CHECK(!memcmp(&light, &c->light_world.dynamic_world[0], sizeof(light)));
            }
            if (flashes) {
                int type = q2_weapon_tables_builtin()->ammo_type[weapon];
                u32 added = c->ent_light_added;
                c->sim[0].combat.inv.ammo[type] = 0;
                c->sim[0].combat.next_fire = 0;
                c->vw[0].state = Q2_VM_IDLE;
                c->vw[0].fire_latch = false;
                c->shoot = true;
                c->frame_index++;
                c->sim[0].dt_accum = c->sim[0].frame_dt - 1;
                client_input_simulated(c, 1.0f / 300.0f);
                CHECK(c->mp_dry[0] == 1);
                CHECK(c->ent_light_added == added);
            }
            q2_sim_free(&c->sim[0]);
            q2_screen_free(&c->screen);
            free(c);
        }
    }
}

static void split_delivery(void)
{
    client *c = calloc(1, sizeof(*c));
    static const q2_vm_key idle = { .duration = 30, .event = Q2_VM_EVENT_NONE };
    q2_inventory loadout;
    int pi;
    if (!c) exit(2);
    c->headless = c->sim_enabled = c->vm_ready = c->lights_ready = true;
    c->mp_enabled = c->mp_stage = c->shoot = true;
    c->pad_frame = -2; c->give_powerup = -1;
    q2_sim_init(&c->sim[0], NULL, 50);
    loadout = c->sim[0].combat.inv;
    c->sim[0].fire_from_input = false;
    q2_mp_session_init(&c->mp, Q2_MP_DEATHMATCH, 4);
    CHECK(q2_screen_init(&c->screen, Q2_VIDEO_PAL) == Q2_OK);
    c->vm_tables.clip[Q2_WID_MACHINEGUN][Q2_VM_IDLE].key = &idle;
    c->vm_tables.clip[Q2_WID_MACHINEGUN][Q2_VM_IDLE].count = 1;
    for (pi = 0; pi < 4; pi++) {
        s32 feet[3] = { pi * 2000, 0, 0 };
        q2_sim_select_player(&c->sim[0], pi);
        q2_sim_spawn(&c->sim[0], feet, 0);
        q2_sim_player_loadout(&c->sim[0], pi, &loadout, Q2_WID_MACHINEGUN);
        c->sim[0].combat.weapon_id = Q2_WID_MACHINEGUN;
        c->sim[0].combat.inv.weapons = 0x7ff;
        c->sim[0].combat.inv.ammo[Q2_AMMO_BULLETS] = 100;
        q2_vw_init(&c->vw[pi], &c->vm_tables, Q2_WID_MACHINEGUN);
        c->vw[pi].state = Q2_VM_IDLE;
        c->vw[pi].left = c->vw[pi].total = 30;
        c->vw_last_weapon[pi] = Q2_WID_MACHINEGUN;
        q2_player_death_init(&c->death[pi]);
        c->sim_ready[pi] = true;
    }
    q2_sim_select_player(&c->sim[0], 0);
    /* All four drivers run on the one tick every player shares. */
    c->sim[0].dt_accum = c->sim[0].frame_dt - 1;
    client_input_simulated(c, 1.0f / 300.0f);
    CHECK(c->sim[0].level_time != 0);
    CHECK(c->ent_light_added == 4 && c->light_world.dynamic_world_count == 4);
    for (pi = 0; pi < 4; pi++) {
        CHECK(c->mp_shots[pi] == 1);
        CHECK(c->light_world.dynamic_world[pi].x == c->sim[0].player[pi].pos[0]);
        CHECK(c->light_world.dynamic_world[pi].z == c->sim[0].player[pi].pos[2]);
    }
    q2_sim_free(&c->sim[0]); q2_screen_free(&c->screen); free(c);
}

static void capture(client *c, const char *output, const char *name)
{
    char path[1024];
    CHECK(c->headless);
    q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
    client_frame(c);
    snprintf(path, sizeof(path), "%s/%s.ppm", output, name);
    CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);
}

static void disc_player(const char *disc_path, const char *output)
{
    client *c = open_client(disc_path);
    int frame;
    CHECK(client_load_zone(c, "BASE1", 0));
    c->god = true;
    c->sim[0].combat.inv.weapons = 0x7ff;
    c->sim[0].combat.inv.ammo[Q2_AMMO_BULLETS] = 200;
    c->sim[0].combat.weapon_id = Q2_WID_MACHINEGUN;
    for (frame = 0; frame < 80; frame++) {
        client_input_simulated(c, 1.0f / 30.0f);
        c->frame_index++;
    }
    CHECK(c->vw[0].weapon == Q2_WID_MACHINEGUN);
    CHECK(c->vw[0].state == Q2_VM_IDLE);
    c->sim[0].dt_accum = c->sim[0].frame_dt - 1;   /* this frame ticks */
    c->shoot = true;
    {
        u32 lights = c->ent_light_added;
        client_input_simulated(c, 1.0f / 30.0f);
        CHECK(c->mp_shots[0] == 1);
        CHECK(c->ent_light_added == lights + 1);
    }
    capture(c, output, "machinegun");
    close_client(c);
}

static void disc_creature(const char *disc_path, const char *output,
                           const char *map, int zone, const char *name,
                           s16 va, s16 vb, s32 size)
{
    client *c = open_client(disc_path);
    q2_monster *m = NULL;
    const q2_model_instance *inst;
    q2_cre_shot shot = {0};
    s32 local[3], expected[3];
    u32 i, index = 0;
    CHECK(client_load_zone(c, map, zone));
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *candidate = &c->creatures.set.monsters[i];
        const q2_cre_bind *bind = q2_cre_bind_for(candidate);
        if (bind && bind->cre && !strcmp(bind->cre->name, name) && c->cre_model_ok[i]) {
            m = candidate; index = i; break;
        }
    }
    CHECK(m != NULL);
    if (!m) { close_client(c); return; }
    if (!m->in_use) {
        q2_pop_group group;
        CHECK(q2_pop_get_group(&c->creatures.pop, m->group, &group));
        CHECK(q2_creature_world_summon(&c->creatures, group.name) > 0);
    }
    CHECK(m->in_use && m->spawn_initialized);
    for (i = 0; i < c->creatures.set.count; i++)
        if (i != index) c->creatures.set.monsters[i].in_use = false;
    memset(m->angles, 0, sizeof(m->angles));
    q2_link_entity(m, 4);
    m->enemy = NULL; /* Death bursts and un-aimed volleys still flash. */
    c->frame_index = 1; c->flash_spin = 0;
    c->cam.pos[0] = m->pos[0] + (size == 240 ? 1800 : 0);
    c->cam.pos[1] = m->pos[1] - (size == 240 ? 400 : 100);
    c->cam.pos[2] = m->pos[2] + (size == 240 ? 2200 : 1000);
    c->cam.yaw = size == 240 ? 2560 : 2048;
    c->cam.pitch = c->cam.roll = 0;
    q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
    client_frame(c); /* Populate the engine's last-drawn attachment poses. */
    inst = client_cre_pose(c, m);
    CHECK(inst && q2_model_blend_local(inst, va, vb, local));
    if (!inst) { close_client(c); return; }
    if (size == 240) {
        s32 bottom = INT32_MIN, vertex[3];
        /* SECURITY's thin platform is an entity box, absent from SecondaryCol.
         * Check both physics placement and the actual posed track vertices. */
        CHECK(m->pos[1] == -11326);
        CHECK(q2_cre_origin(m)[1] + Q2_EYE_BASE == -11040);
        for (i = 0; i < inst->model->hdr.num_verts; i++) {
            if (!q2_model_world_vertex(inst, (s32)i, vertex)) break;
            if (vertex[1] > bottom) bottom = vertex[1];
        }
        CHECK(i == inst->model->hdr.num_verts);
        /* Retail uses the constant ext2 bias; idle poses vary by a few units.
         * The old missed platform put this mesh 855 units below its floor. */
        CHECK(abs(bottom - (-11040)) <= 32);
        printf("Boss1 platform: entity Y %d, lowest posed vertex Y %d\n", m->pos[1], bottom);
    }
    local[0] -= size;
    q2_model_local_to_world(inst, local, expected);
    q2_cre_muzzle(m, va, vb, shot.start);
    shot.slot = Q2_IMP_FIRE_BULLET; shot.damage = 1;
    shot.count = 1; shot.aimed = true; shot.dir[2] = 4096;
    q2_fx_flash_clear(&c->sim[0].fx);
    q2_light_world_begin_frame(&c->light_world);
    client_cre_shot(m, &shot, c);
    CHECK(c->sim[0].fx.flash_count == 1);
    CHECK(c->light_world.dynamic_world_count == 1);
    CHECK(!memcmp(c->sim[0].fx.flash[0].corner[0], expected, sizeof(expected)));
    capture(c, output, name);
    CHECK(c->sim[0].fx.stats.flashes_drawn > 0);
    q2_fx_flash_clear(&c->sim[0].fx);
    shot.slot = Q2_IMP_FIRE_SHOTGUN;
    client_cre_shot(m, &shot, c);
    CHECK(c->sim[0].fx.flash_count == 1);
    q2_fx_flash_clear(&c->sim[0].fx);
    shot.slot = Q2_IMP_FIRE_BLASTER; shot.speed = 800;
    client_cre_shot(m, &shot, c);
    CHECK(c->sim[0].fx.flash_count == 0);
    if (size == 240) {
        q2_cre_motion_world world = {0};
        world.coll = &c->sim[0].coll;
        world.world = &c->sim[0].move_world;
        world.self_id = (s32)index;
        m->think = NULL; /* Isolate continued physical support from combat AI. */
        for (i = 0; i < 30; i++) q2_cre_motion_tick(m, 10, &world);
        CHECK(q2_cre_origin(m)[1] + Q2_EYE_BASE == -11040);
    }
    close_client(c);
}

int main(int argc, char **argv)
{
    player_delivery();
    split_delivery();
    if (argc == 4 && !strcmp(argv[1], "--headless")) {
        disc_player(argv[2], argv[3]);
        disc_creature(argv[2], argv[3], "BASE1", 0, "Soldier", 228, 234, 120);
        disc_creature(argv[2], argv[3], "BASE1", 1, "Infantry", 200, 205, 120);
        disc_creature(argv[2], argv[3], "BASE2", 2, "Gunner", 227, 231, 120);
        disc_creature(argv[2], argv[3], "SECURITY", 1, "Boss1", 412, 419, 240);
    }
    printf("%d muzzle client checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
