/* Actual saved-game load ordering. With no arguments this checks early refusal;
 * pass --headless DISC OUTPUT_DIR to exercise a real disc and write engine
 * framebuffer captures. No SDL devices, windows or user input are opened. */
int q2_test_save_client_main(int argc, char **argv);
#define main q2_test_save_client_main
#include "../src/client/main.c"
#undef main

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)

static client *open_client(const char *disc)
{
    client *c = calloc(1, sizeof(*c));
    if (!c) exit(2);
    c->headless = true;
    c->use_sort = true;
    c->sim_enabled = true;
    c->trace_cre = -1;
    c->give_powerup = -1;
    c->mission_row = -1;
    c->pad_frame = -2;
    if (disc_open(&c->disc, disc) != Q2_OK ||
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
    q2_menu_reset_video_for(&c->settings, c->height);
    q2_menu_init(&c->menu, &c->settings, Q2_MENU_SCREEN_H);
    q2_menu_set_fb_height(&c->menu, c->height);
    q2_player_death_init(&c->death[0]);
    q2_mission_init(&c->mission);
    q2_briefing_init(&c->briefing);
    q2_briefing_popup_init(&c->popup);
    q2_prompt_init(&c->prompts);
    c->item_table_ready = q2_item_table_load(&c->item_table, c->disc,
                                             &c->build) == Q2_OK;
    c->vm_ready = q2_vm_tables_load(&c->vm_tables, c->disc, &c->build) == Q2_OK;
    CHECK(c->vm_ready);
    return c;
}

static void close_client(client *c)
{
    client_free_creatures(c);
    client_voices_stop(c);
    client_film_stop(c);
    if (c->sfx_ready) q2_sound_bank_free(&c->sfx);
    if (c->level_table_ready) q2_level_table_free(&c->level_table);
    q2_save_free(&c->snapshot);
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
    q2_vm_tables_free(&c->vm_tables);
    psx_ot_free(&c->ot);
    disc_close(c->disc);
    free(c);
}

static void photograph(client *c, const q2_camera *camera,
                         const char *output, const char *name)
{
    char path[1024];
    CHECK(c->headless);
    c->cam = *camera;
    c->frame_index++;
    client_frame(c);
    snprintf(path, sizeof(path), "%s/%s.ppm", output, name);
    CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);
}

static void test_wrong_disc(void)
{
    client *c = calloc(1, sizeof(*c));
    q2_save save = {0};
    if (!c) exit(2);
    c->headless = true;
    c->in_front_end = true;
    c->mp_enabled = true;
    snprintf(c->build.serial, sizeof(c->build.serial), "SLES-01534");
    snprintf(save.serial, sizeof(save.serial), "SLUS-00757");
    snprintf(save.map, sizeof(save.map), "BASE0");
    CHECK(!client_apply_save(c, &save));
    CHECK(c->in_front_end && c->mp_enabled);
    CHECK(c->level_change_serial == 0);
    free(c);
}

static bool node_blocks(client *c, s16 node, s32 y_offset)
{
    q2_scene_node n;
    u32 i;
    if (!q2_scene_get_node(&c->zone.scene, (u32)node, &n)) return false;
    for (i = 0; i < c->sim[0].volume_count; i++) {
        const q2_move_target *t = &c->sim[0].volumes[i];
        if (t->kind == Q2_MOVE_KIND_ENTITY && t->active &&
            t->min[0] == n.bbox_min[0] && t->max[0] == n.bbox_max[0] &&
            t->min[1] == n.bbox_min[1] + y_offset &&
            t->max[1] == n.bbox_max[1] + y_offset &&
            t->min[2] == n.bbox_min[2] && t->max[2] == n.bbox_max[2])
            return true;
    }
    return false;
}

static void test_disc_restore(const char *disc, const char *output)
{
    client *c = open_client(disc);
    client *cold;
    q2_save before = {0}, after = {0};
    q2_camera camera;
    q2_event_record rec;
    q2_explosive *group;
    u32 i, record = 0, item = 0;
    s16 node = -1;
    char path[1024];

    CHECK(client_load_zone(c, "BASE0", 0));
    q2_menu_set_cheat_level(&c->menu, 3);
    /* BASE0's armour crate has an otherwise unreachable CREBATCH/INSECRET
     * tail. Run that real script, including its one-shot latches and effects. */
    for (i = 0; i < c->explosives.count; i++) {
        group = &c->explosives.items[i];
        if (!group->damageable || !group->part_count ||
            !q2_events_record_at(&c->sim[0].events, group->record_offset, &rec) ||
            rec.n_items < 2) continue;
        record = group->record_offset;
        item = group->item_offset;
        node = group->node[0];
        break;
    }
    CHECK(node >= 0 && record && item);
    if (node < 0) { close_client(c); return; }
    CHECK(node_blocks(c, node, 0));
    /* Look directly at the authored crate from a fixed position in front. */
    {
        q2_scene_node scene;
        CHECK(q2_scene_get_node(&c->zone.scene, (u32)node, &scene));
        camera = c->cam;
        camera.pos[0] = (scene.bbox_min[0] + scene.bbox_max[0]) / 2;
        camera.pos[1] = (scene.bbox_min[1] + scene.bbox_max[1]) / 2;
        camera.pos[2] = scene.bbox_min[2] - 1800;
        camera.yaw = camera.pitch = camera.roll = 0;
    }
    {
        q2_camera moved = camera;
        q2_move_contact hit;
        s32 dest[3] = { camera.pos[0], camera.pos[1], camera.pos[2] + 2000 };
        CHECK(!q2_move_sweep_world(&c->sim[0].move_world, camera.pos, dest, &hit));
        memcpy(moved.pos, hit.pos, sizeof(moved.pos));
        photograph(c, &moved, output, "crate-collision");
    }
    CHECK(client_capture(c));
    snprintf(path, sizeof(path), "%s/before.sav", output);
    CHECK(q2_save_write(&c->snapshot, path) == Q2_OK);
    CHECK(q2_save_read(&before, path) == Q2_OK);
    photograph(c, &camera, output, "saved-intact");

    CHECK(q2_event_rt_trigger(&c->sim[0].event_rt, record));
    q2_event_rt_update(&c->sim[0].event_rt);
    /* Drain the geometry changes as the client's normal tick does. */
    {
        s16 n; u8 hidden;
        while (q2_sim_next_node_vis(&c->sim[0], &n, &hidden))
            if (n >= 0 && (u32)n < c->node_hidden_count)
                c->node_hidden[n] = hidden;
    }
    CHECK(c->node_hidden[node]);
    CHECK(!node_blocks(c, node, 0));
    CHECK(client_capture(c));
    snprintf(path, sizeof(path), "%s/after.sav", output);
    CHECK(q2_save_write(&c->snapshot, path) == Q2_OK);
    CHECK(q2_save_read(&after, path) == Q2_OK);

    CHECK(client_apply_save(c, &before));
    CHECK(!c->node_hidden[node]);
    CHECK(node_blocks(c, node, 0));
    photograph(c, &camera, output, "restored-intact");

    /* A different session has no resident latches or in-memory EVE_ block. */
    cold = open_client(disc);
    CHECK(client_apply_save(cold, &after));
    CHECK(cold->menu.cheat_level == 3);
    CHECK(cold->node_hidden[node]);
    CHECK(!node_blocks(cold, node, 0));
    photograph(cold, &camera, output, "restored-destroyed");
    for (i = 0; i < cold->explosives.count; i++)
        if (cold->explosives.items[i].item_offset == item)
            CHECK(cold->explosives.items[i].destroyed);
    close_client(cold);

    /* Save on another map, restart, then revisit the one with the crate. */
    CHECK(client_apply_save(c, &after));
    CHECK(client_load_zone(c, "BASE1", 0));
    CHECK(client_capture(c));
    snprintf(path, sizeof(path), "%s/visited.sav", output);
    CHECK(q2_save_write(&c->snapshot, path) == Q2_OK);
    q2_save_free(&after);
    CHECK(q2_save_read(&after, path) == Q2_OK);
    cold = open_client(disc);
    CHECK(client_apply_save(cold, &after));
    CHECK(client_load_zone(cold, "BASE0", 0));
    CHECK(cold->node_hidden[node]);
    close_client(cold);

    q2_save_free(&before);
    q2_save_free(&after);
    close_client(c);
}

static void test_disc_behavior(const char *disc, const char *output)
{
    client *c = open_client(disc), *cold;
    q2_save saved = {0};
    q2_monster *m;
    q2_rotator *r;
    q2_camera camera;
    q2_scene_node node;
    s16 frame, angle;
    u16 hold, delay;
    u8 state;
    u32 i, count, target = 0;
    s32 attack_frame[16], random_after[16];
    char path[1024];
    q2_cre_set_skill(2);
    CHECK(client_load_zone(c, "BASE3", 0));
    CHECK(c->creatures.set.count > 1 && c->rotators.count > 0);
    if (!c->creatures.set.count || !c->rotators.count) {
        close_client(c); return;
    }
    count = c->creatures.set.count;
    for (i = 0; i < c->rotators.count; i++)
        if (c->rotators.rotators[i].kind == Q2_ROT_TARGET) { target = i; break; }
    r = &c->rotators.rotators[target];
    CHECK(node_blocks(c, r->node, 0));
    q2_rotator_trigger(&c->rotators, target);
    for (i = 0; i < 3; i++) q2_rotators_tick(&c->rotators, 1);
    /* Stop around half travel so the frame also exposes an endpoint reset. */
    for (i = 0; i < 10000; i++) {
        s32 travel = r->target <= 2048 ? r->target : 4096 - r->target;
        s32 moved = r->angle <= 2048 ? r->angle : 4096 - r->angle;
        if (moved >= travel / 2) break;
        q2_rotators_tick(&c->rotators, 1);
    }
    angle = r->angle; hold = r->hold; delay = r->delay; state = r->state;
    CHECK(r->kind == Q2_ROT_TARGET && angle != 0 && angle != r->target);
    CHECK(q2_scene_get_node(&c->zone.scene, (u32)r->node, &node));
    camera = c->cam;
    camera.pos[0] = (node.bbox_min[0] + node.bbox_max[0]) / 2;
    camera.pos[1] = (node.bbox_min[1] + node.bbox_max[1]) / 2;
    camera.pos[2] = node.bbox_min[2] - 1800;
    camera.yaw = camera.pitch = camera.roll = 0;

    q2_level_state.time = q2_level_state.framenum = 200;
    m = &c->creatures.set.monsters[0];
    CHECK(m->run != NULL);
    if (m->run) m->run(m);
    m->enemy = m->goalentity = &c->creatures.sight;
    m->attack_finished = 230; m->pain_debounce = 210;
    m->next_think = 201; m->think = q2_M_MoveFrame;
    if (m->currentmove) m->frame = (s16)m->currentmove->first_frame;
    frame = m->frame;
    c->ai_accum = 0.066666;
    CHECK(client_capture(c));
    snprintf(path, sizeof(path), "%s/behavior.sav", output);
    CHECK(q2_save_write(&c->snapshot, path) == Q2_OK);
    CHECK(q2_save_read(&saved, path) == Q2_OK);
    photograph(c, &camera, output, "saved-motion");
    CHECK(m->attack != NULL);
    for (i = 0; i < 16; i++) {
        if (m->attack) m->attack(m);
        attack_frame[i] = m->currentmove ? m->currentmove->first_frame : -1;
    }
    for (i = 0; i < 16; i++) random_after[i] = q2_random_next();

    cold = open_client(disc);
    q2_cre_set_skill(0); /* Load from a session that would spawn fewer monsters. */
    CHECK(client_apply_save(cold, &saved));
    CHECK(q2_cre_skill() == 2 && cold->menu.skill == 2);
    CHECK(cold->creatures.set.count == count);
    m = &cold->creatures.set.monsters[0];
    CHECK(m->enemy == &cold->creatures.sight && m->goalentity == m->enemy);
    CHECK(m->currentmove && m->frame == frame &&
          m->currentmove->first_frame == frame);
    CHECK(m->next_think == 201 && m->attack_finished == 230 &&
          m->pain_debounce == 210 && q2_level_state.time == 200);
    CHECK(cold->ai_accum > 0.066665 && cold->ai_accum < 0.066667);
    r = &cold->rotators.rotators[target];
    CHECK(r->angle == angle && r->hold == hold && r->delay == delay &&
          r->state == state);
    CHECK(node_blocks(cold, r->node, -10000));
    CHECK(!node_blocks(cold, r->node, 0));
    photograph(cold, &camera, output, "restored-motion");
    /* Map spawning and view-weapon reconstruction consume random values. The
     * saved stream must resume after those constructors, at the same choice. */
    for (i = 0; i < 16; i++) {
        if (m->attack) m->attack(m);
        CHECK(m->currentmove && m->currentmove->first_frame == attack_frame[i]);
    }
    for (i = 0; i < 16; i++) CHECK(q2_random_next() == random_after[i]);
    q2_save_free(&saved);
    close_client(cold); close_client(c);
    q2_cre_set_skill(1);
}

static void test_disc_hidden_boxes(const char *disc, const char *output)
{
    static const struct { const char *map; u32 zone; } cases[] = {
        {"BASE2", 0}, {"JAIL4", 3}, {"JAIL5", 2}, {"SECURITY", 2}
    };
    u32 ci;
    for (ci = 0; ci < sizeof(cases)/sizeof(cases[0]); ci++) {
        client *c = open_client(disc), *cold;
        q2_save saved = {0};
        q2_event_record rec, prev;
        s16 nodes[16];
        u32 count = 0, i;
        bool more;
        char path[1024];
        CHECK(client_load_zone(c, cases[ci].map, cases[ci].zone));
        for (more = q2_events_first_record(&c->sim[0].events, &rec); more;
             more = q2_events_next_record(&c->sim[0].events, &prev, &rec)) {
            prev = rec;
            for (i = 0; i < rec.n_items; i++) {
                q2_event_item item;
                q2_uf_call call;
                const u8 *p;
                u32 k, before = count;
                if (!q2_events_get_item(&c->sim[0].events, &rec, i, &item)) break;
                if (q2_uf_decode_call(&call, &c->sim[0].userfuncs, &item) != Q2_OK ||
                    call.prim != Q2_UF_OBJDRAWOFF) continue;
                p = q2_uf_operand_at(&c->ev_operands, item.payload - 2, item.len);
                for (k = 0; k < 4; k++) {
                    s16 n = q2_rd_s16(p + 4 + 2*k);
                    if (n < 0 || (u32)n >= c->node_hidden_count) continue;
                    CHECK(count < sizeof(nodes)/sizeof(nodes[0]));
                    if (count >= sizeof(nodes)/sizeof(nodes[0])) continue;
                    nodes[count++] = n;
                    CHECK(node_blocks(c, n, 0));
                }
                if (count > before) {
                    CHECK(q2_event_rt_trigger(&c->sim[0].event_rt, rec.offset));
                    q2_event_rt_update(&c->sim[0].event_rt);
                }
            }
        }
        CHECK(count > 0);
        for (i = 0; i < count; i++) {
            CHECK(c->node_hidden[nodes[i]]);
            CHECK(!node_blocks(c, nodes[i], 0));
        }
        CHECK(client_capture(c));
        snprintf(path, sizeof(path), "%s/hidden-%s.sav", output, cases[ci].map);
        CHECK(q2_save_write(&c->snapshot, path) == Q2_OK);
        CHECK(q2_save_read(&saved, path) == Q2_OK);
        cold = open_client(disc);
        CHECK(client_apply_save(cold, &saved));
        for (i = 0; i < count; i++) {
            CHECK(cold->node_hidden[nodes[i]]);
            CHECK(!node_blocks(cold, nodes[i], 0));
        }
        /* The change must also survive saving on another map and returning
         * in a fresh session; SECURITY's item has no one-shot latch. */
        CHECK(client_load_zone(cold, "BASE0", 0));
        CHECK(client_capture(cold));
        CHECK(q2_save_write(&cold->snapshot, path) == Q2_OK);
        q2_save_free(&saved);
        CHECK(q2_save_read(&saved, path) == Q2_OK);
        close_client(cold);
        cold = open_client(disc);
        CHECK(client_apply_save(cold, &saved));
        CHECK(client_load_zone(cold, cases[ci].map, cases[ci].zone));
        for (i = 0; i < count; i++) {
            CHECK(cold->node_hidden[nodes[i]]);
            CHECK(!node_blocks(cold, nodes[i], 0));
        }
        q2_save_free(&saved);
        close_client(cold); close_client(c);
    }
}

static bool same_weapon(q2_viewweapon a, q2_viewweapon b)
{
    a.tab = b.tab = NULL;
    a.model = b.model = NULL;
    return memcmp(&a, &b, sizeof(a)) == 0;
}

static bool same_projectiles(const q2_projectile *a, const q2_projectile *b)
{
    u32 i;
    for (i = 0; i < Q2_PROJ_MAX; i++) {
        q2_projectile x = a[i], y = b[i];
        if (x.in_use != y.in_use) return false;
        if (!x.in_use) continue; /* Free slots have no state to resume. */
        if ((x.shooter == NULL) != (y.shooter == NULL) ||
            (x.node == Q2_PROJ_NODE_HELD) != (y.node == Q2_PROJ_NODE_HELD)) return false;
        /* A cold load rebuilds collision caches and actor addresses. */
        x.shooter = y.shooter = NULL;
        x.node = y.node = Q2_PROJ_NODE_UNKNOWN;
        if (memcmp(&x, &y, sizeof(x))) return false;
    }
    return true;
}

static void weapon_frame(client *c, bool attack)
{
    client_advance_view_weapon(c, attack, 10);
    /* Production advances the mixer every frame, including headless runs.
     * Without this, one-shots never finish and the fixture fills 24 voices;
     * the cold client then alone draws pitch RNG for successful starts. */
    client_audio_pump(c, 1.0 / 30.0);
}

static void test_disc_weapon_restore(const char *disc, const char *output)
{
    static const int weapons[] = { Q2_WID_BLASTER, Q2_WID_CHAINGUN,
                                   Q2_WID_HAND_GRENADE, Q2_WID_SHOTGUN };
    unsigned ci;
    for (ci = 0; ci < sizeof(weapons) / sizeof(weapons[0]); ci++) {
        client *c = open_client(disc), *cold;
        q2_save saved = {0};
        q2_viewweapon at_save, timeline[45];
        q2_projectile (*projectiles)[Q2_PROJ_MAX] = calloc(45, sizeof(*projectiles));
        q2_camera camera;
        u32 rng, random_after[45], serial_after[45], shown, heard;
        s16 ammo_after[45][Q2_AMMO_COUNT];
        int i, selected;
        bool matches = true;
        char path[1024], name[64];
        if (!projectiles) exit(2);
        CHECK(client_load_zone(c, ci == 1 || ci == 2 ? "BOSS2" : "BASE0", 0));
        CHECK(c->vw_model_ready[0]);
        c->sim[0].combat.inv.weapons = 0xFFFF;
        for (i = 0; i < Q2_AMMO_COUNT; i++) c->sim[0].combat.inv.ammo[i] = 500;
        c->sim[0].combat.weapon_id = ci == 3 ? Q2_WID_BLASTER : weapons[ci];
        c->sim[0].combat.inv.current_weapon = (s8)c->sim[0].combat.weapon_id;
        client_reset_view_model(c, 0);
        CHECK(c->vw_model_ready[0]);
        for (i = 0; i < 120 && c->vw[0].state != Q2_VM_IDLE; i++)
            weapon_frame(c, false);
        CHECK(c->vw[0].state == Q2_VM_IDLE);
        if (ci == 3) {
            c->sim[0].combat.weapon_id = weapons[ci];
            c->sim[0].combat.inv.current_weapon = (s8)weapons[ci];
            weapon_frame(c, false);
            CHECK(c->vw[0].pending != c->vw[0].weapon);
        } else {
            int steps = ci == 0 ? 2 : (ci == 1 ? 24 : 80);
            for (i = 0; i < steps && (ci != 2 || !c->vw[0].cook); i++)
                weapon_frame(c, true);
            CHECK(c->vw[0].state == Q2_VM_FIRE);
            if (ci == 2) {
                CHECK(c->vw[0].cook);
                CHECK(q2_projectile_hand_held_index(&c->sim[0].combat.projectiles, 0) >= 0);
            }
        }
        CHECK(c->voice_dropped == 0);
        camera = c->cam;
        snprintf(name, sizeof(name), "weapon-%u-saved", ci);
        photograph(c, &camera, output, name);
        at_save = c->vw[0]; selected = c->vw_last_weapon[0];
        shown = c->shot_serial_shown[0]; heard = c->shot_serial_heard[0];
        rng = q2_random_state();
        CHECK(client_capture(c));
        snprintf(path, sizeof(path), "%s/weapon-%u.q2s", output, ci);
        CHECK(q2_save_write(&c->snapshot, path) == Q2_OK);
        CHECK(q2_save_read(&saved, path) == Q2_OK);
        for (i = 0; i < 45; i++) {
            weapon_frame(c, i < 10);
            timeline[i] = c->vw[0];
            random_after[i] = q2_random_state();
            serial_after[i] = c->sim[0].combat.shot_serial;
            memcpy(ammo_after[i], c->sim[0].combat.inv.ammo, sizeof(ammo_after[i]));
            memcpy(projectiles[i], c->sim[0].combat.projectiles.p, sizeof(projectiles[i]));
        }
        CHECK(c->voice_dropped == 0);
        close_client(c);
        cold = open_client(disc);
        CHECK(client_apply_save(cold, &saved));
        CHECK(same_weapon(cold->vw[0], at_save));
        CHECK(cold->vw_last_weapon[0] == selected);
        CHECK(cold->shot_serial_shown[0] == shown && cold->shot_serial_heard[0] == heard);
        CHECK(q2_random_state() == rng);
        if (ci == 2)
            CHECK(q2_projectile_hand_held_index(&cold->sim[0].combat.projectiles, 0) >= 0);
        snprintf(name, sizeof(name), "weapon-%u-restored", ci);
        photograph(cold, &camera, output, name);
        for (i = 0; i < 45; i++) {
            weapon_frame(cold, i < 10);
            if (!same_weapon(cold->vw[0], timeline[i]) ||
                q2_random_state() != random_after[i] ||
                cold->sim[0].combat.shot_serial != serial_after[i] ||
                memcmp(ammo_after[i], cold->sim[0].combat.inv.ammo,
                       sizeof(ammo_after[i])) != 0 ||
                !same_projectiles(projectiles[i], cold->sim[0].combat.projectiles.p)) {
                if (matches) fprintf(stderr,
                    "weapon continuation mismatch: weapon %d step %d, RNG %u/%u, shot serial %u/%u\n",
                    weapons[ci], i, q2_random_state(), random_after[i],
                    cold->sim[0].combat.shot_serial, serial_after[i]);
                matches = false;
            }
        }
        CHECK(matches);
        CHECK(cold->voice_dropped == 0);
        if (ci == 3) {
            saved.viewweapon_present = false;
            CHECK(q2_save_write(&saved, path) == Q2_OK);
            q2_save_free(&saved);
            CHECK(q2_save_read(&saved, path) == Q2_OK);
            CHECK(!saved.viewweapon_present && client_apply_save(cold, &saved));
            CHECK(cold->vw[0].weapon == Q2_WID_SHOTGUN && cold->vw[0].state == Q2_VM_LOWER);
        }
        q2_save_free(&saved);
        free(projectiles);
        close_client(cold);
    }
}

static void test_disc_timer_restore(const char *disc, const char *output)
{
    client *c = open_client(disc), *cold;
    q2_save saved = {0};
    q2_event_timer original[Q2_EVENT_RT_TIMER_MAX];
    s32 clock;
    u32 state, calls, timers;
    char path[1024];
    CHECK(client_load_zone(c, "BOSS2", 0));
    /* The real infinite, one-light-per-window TIMER. Save partway through
     * its second period so a reconstruction from STARTLEV cannot pass. */
    q2_event_rt_trigger(&c->sim[0].event_rt, 0x184);
    q2_event_rt_update(&c->sim[0].event_rt);
    CHECK(c->sim[0].event_rt.deferred_count > 0);
    q2_event_rt_advance(&c->sim[0].event_rt, 100);
    q2_event_rt_update(&c->sim[0].event_rt);
    q2_event_rt_advance(&c->sim[0].event_rt, 5);
    CHECK(client_capture(c));
    snprintf(path, sizeof(path), "%s/timer.q2s", output);
    CHECK(q2_save_write(&c->snapshot, path) == Q2_OK);
    CHECK(q2_save_read(&saved, path) == Q2_OK);
    clock = c->sim[0].event_rt.clock;
    timers = c->sim[0].event_rt.deferred_count;
    memcpy(original, c->sim[0].event_rt.deferred, sizeof(original));
    q2_event_rt_advance(&c->sim[0].event_rt, 100);
    calls = c->sim[0].event_rt.call_count;
    q2_event_rt_update(&c->sim[0].event_rt);
    calls = c->sim[0].event_rt.call_count - calls;
    state = q2_random_state();
    close_client(c);
    cold = open_client(disc);
    CHECK(client_apply_save(cold, &saved));
    CHECK(cold->sim[0].event_rt.clock == clock);
    CHECK(cold->sim[0].event_rt.deferred_count == timers);
    CHECK(memcmp(original, cold->sim[0].event_rt.deferred, sizeof(original)) == 0);
    q2_event_rt_advance(&cold->sim[0].event_rt, 100);
    {
        u32 before = cold->sim[0].event_rt.call_count;
        q2_event_rt_update(&cold->sim[0].event_rt);
        CHECK(cold->sim[0].event_rt.call_count - before == calls);
        CHECK(q2_random_state() == state);
    }
    q2_save_free(&saved);
    close_client(cold);
}

int main(int argc, char **argv)
{
    if (argc == 1) test_wrong_disc();
    else if (argc == 4 && !strcmp(argv[1], "--headless")) {
        test_disc_restore(argv[2], argv[3]);
        test_disc_behavior(argv[2], argv[3]);
        test_disc_hidden_boxes(argv[2], argv[3]);
        test_disc_weapon_restore(argv[2], argv[3]);
        test_disc_timer_restore(argv[2], argv[3]);
    }
    else {
        fprintf(stderr, "usage: test_client_save [--headless DISC OUTPUT_DIR]\n");
        return 2;
    }
    printf("%d client save checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
