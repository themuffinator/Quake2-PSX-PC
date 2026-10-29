/* Exercise the real animation/fire/sound glue without a window or host input. */
int q2_test_client_main(int argc, char **argv);
#define main q2_test_client_main
#include "../src/client/main.c"
#undef main

static int failures, checks;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    failures++; } } while (0)

#include "client_fixture.h"

static q2_vm_key keys[Q2_VM_SLOTS][Q2_VM_STATES][28];

static client *fixture(int weapon, int owner)
{
    client *c = calloc(1, sizeof(*c));
    int w, s, k;
    s32 feet[3] = {0, 0, 0};
    if (!c) exit(2);
    c->headless = true;
    c->vm_ready = true;
    q2_sim_init(&c->sim[0], NULL, 50);
    q2_sim_select_player(&c->sim[0], owner);
    q2_sim_spawn(&c->sim[0], feet, 0);
    q2_inventory_init(&c->sim[0].combat.inv);
    c->sim[0].fire_from_input = false;
    c->sim[0].combat.weapon_id = weapon;
    c->sim[0].combat.inv.weapons = 0x7ff;
    for (k = 0; k < Q2_AMMO_COUNT; k++) c->sim[0].combat.inv.ammo[k] = 200;
    q2_player_death_init(&c->death[owner]);
    memset(keys, 0, sizeof(keys));
    for (w = 0; w < Q2_VM_SLOTS; w++) {
        for (s = 0; s < Q2_VM_STATES; s++) {
            int n = s == Q2_VM_FIRE ? (w == 5 ? 28 : w == 9 ? 7 : 3) : 1;
            for (k = 0; k < n; k++) {
                keys[w][s][k].duration = 30;
                keys[w][s][k].event = Q2_VM_EVENT_NONE;
            }
            c->vm_tables.clip[w][s].count = (u32)n;
            c->vm_tables.clip[w][s].key = keys[w][s];
        }
    }
    q2_vw_init(&c->vw[owner], &c->vm_tables, weapon);
    c->vw[owner].state = Q2_VM_IDLE;
    c->vw[owner].frame = 0;
    c->vw[owner].left = c->vw[owner].total = 30;
    c->vw_last_weapon[owner] = weapon;
    return c;
}

static void release_fixture(client *c)
{
    q2_sim_free(&c->sim[0]);
    free(c);
}

static void step(client *c, bool attack)
{
    c->sim[0].level_time += 10;
    client_advance_view_weapon(c, attack, 10);
}

static void park_fire(client *c, int frame)
{
    q2_viewweapon *vw = &c->vw[c->sim[0].cur_player];
    vw->state = Q2_VM_FIRE;
    vw->frame = (u32)frame;
    vw->left = vw->total = 30;
    vw->last_fire_frame = -1;
    vw->spin_accum = 20;
}

static void test_all_weapons(void)
{
    int w, pi;
    for (pi = 0; pi < Q2_MP_MAX_PLAYERS; pi++) {
        for (w = 1; w <= Q2_WID_COUNT; w++) {
            const q2_weapon_tables *tab = q2_weapon_tables_builtin();
            client *c = fixture(w, pi);
            q2_sim *sim = &c->sim[0];
            int a, type = tab->ammo_type[w];
            step(c, true);
            CHECK(sim->combat.last_shot.fired);
            CHECK(sim->combat.last_shot.kind == q2_weapon_behaviour_table[w].kind);
            CHECK(c->vw[pi].state == Q2_VM_FIRE);
            CHECK(c->mp_shots[pi] == 1);
            for (a = 0; a < Q2_AMMO_COUNT; a++) {
                int spent = a == type && w != Q2_WID_HAND_GRENADE
                            ? (int)tab->ammo_per_shot[w] : 0;
                CHECK(sim->combat.inv.ammo[a] == 200 - spent);
            }
            /* Releasing an ordinary shot must not replay its result when
             * the fire animation returns to idle. */
            if (w != Q2_WID_HAND_GRENADE) {
                u32 serial = sim->combat.shot_serial;
                int n;
                for (n = 0; n < 120; n++) step(c, false);
                CHECK(c->vw[pi].state == Q2_VM_IDLE);
                if (w != Q2_WID_CHAINGUN) CHECK(sim->combat.shot_serial == serial);
            }
            if (tab->ammo_per_shot[w]) {
                c->vw[pi].state = Q2_VM_IDLE;
                c->vw[pi].fire_latch = false;
                sim->combat.inv.ammo[type] = (s16)(tab->ammo_per_shot[w] - 1);
                sim->combat.next_fire = 0;
                step(c, true);
                CHECK(sim->combat.last_shot.dry);
                CHECK(sim->combat.inv.ammo[type] == (s16)(tab->ammo_per_shot[w] - 1));
                CHECK(c->mp_dry[pi] == 1);
            }
            release_fixture(c);
        }
    }
}

static void test_switch_during_burst(void)
{
    static const int automatic[] = {4, 5, 9};
    unsigned i;
    int pi, next;
    for (pi = 0; pi < Q2_MP_MAX_PLAYERS; pi++) {
        for (i = 0; i < sizeof(automatic) / sizeof(automatic[0]); i++) {
            int held = automatic[i];
            for (next = 1; next <= Q2_WID_COUNT; next++) {
                const q2_weapon_tables *tab = q2_weapon_tables_builtin();
                client *c;
                if (next == held) continue;
                c = fixture(held, pi);
                park_fire(c, held == 5 ? 12 : 1);
                c->sim[0].combat.weapon_id = next;
                step(c, true);
                CHECK(c->vw[pi].weapon == held);
                CHECK(c->sim[0].combat.weapon_id == next);
                CHECK(c->sim[0].combat.last_shot.kind == q2_weapon_behaviour_table[held].kind);
                CHECK(c->sim[0].combat.inv.ammo[tab->ammo_type[held]] < 200);
                CHECK(c->sim[0].combat.projectiles.live == (held == 9 ? 1u : 0u));
                release_fixture(c);
            }
        }
    }
    /* An idle selection must lower before any shot from the new weapon. */
    {
        client *c = fixture(4, 0);
        int n;
        c->sim[0].combat.weapon_id = 8;
        step(c, true);
        CHECK(c->sim[0].combat.shot_serial == 0);
        CHECK(c->vw[0].state == Q2_VM_LOWER);
        for (n = 0; n < 30 && !c->sim[0].combat.shot_serial; n++) step(c, true);
        CHECK(c->vw[0].weapon == 8);
        CHECK(c->sim[0].combat.last_shot.kind == Q2_FK_ROCKET);
        release_fixture(c);
    }
}

static void test_chaingun_burst(void)
{
    int ammo;
    for (ammo = 1; ammo <= 4; ammo++) {
        client *c = fixture(5, 0);
        int shots = ammo < 3 ? ammo : 3;
        park_fire(c, 12);
        c->sim[0].combat.inv.ammo[Q2_AMMO_BULLETS] = (s16)ammo;
        step(c, true);
        CHECK(c->sim[0].combat.inv.ammo[Q2_AMMO_BULLETS] == ammo - shots);
        CHECK(c->sim[0].combat.shot_serial == 1);
        CHECK(c->sim[0].combat.last_shot.fired && !c->sim[0].combat.last_shot.dry);
        CHECK(c->sim[0].combat.last_shot.shot_count == (u32)shots);
        CHECK(c->chaingun_bangs == (u32)shots);
        CHECK(c->weapon_sounds == 1);
        CHECK(c->mp_shots[0] == 1);
        CHECK(c->mp_dry[0] == 0);
        release_fixture(c);
    }
}

static void test_automatic_exhaustion(void)
{
    static const int automatic[] = {4, 5, 9};
    const q2_weapon_tables *tab = q2_weapon_tables_builtin();
    int pi;
    unsigned i;
    for (pi = 0; pi < Q2_MP_MAX_PLAYERS; pi++) {
        for (i = 0; i < sizeof(automatic) / sizeof(automatic[0]); i++) {
            int weapon = automatic[i], n, other;
            int ammo = tab->ammo_type[weapon];
            client *c = fixture(weapon, pi);
            c->sim[0].combat.inv.weapons = (u16)(tab->owned_bit[weapon] | 1u);
            c->sim[0].combat.inv.ammo[ammo] = 5;
            for (n = 0; n < 200; n++) step(c, true);
            CHECK(c->sim[0].combat.inv.ammo[ammo] == 0);
            CHECK(c->sim[0].combat.weapon_id == Q2_WID_BLASTER);
            CHECK(c->vw[pi].weapon == Q2_WID_BLASTER);
            CHECK(c->mp_dry[pi] > 0 && c->mp_dry[pi] < 4);
            CHECK(c->vw[pi].frame_fires == 0);
            for (other = 0; other < Q2_MP_MAX_PLAYERS; other++)
                if (other != pi) CHECK(c->mp_shots[other] == 0);
            if (weapon == Q2_WID_CHAINGUN) CHECK(c->chaingun_bangs == 5);
            release_fixture(c);
        }
    }
}

static void test_frame_result_is_consumed(void)
{
    client *c = fixture(Q2_WID_MACHINEGUN, 0);
    park_fire(c, 1);
    step(c, true);
    CHECK(c->mp_shots[0] == 1);
    step(c, false);
    CHECK(c->mp_shots[0] == 1);
    CHECK(c->weapon_sounds == 1);
    CHECK(c->vw[0].fires_started == 0);
    release_fixture(c);
}

static void test_disc_grenade(const char *disc, const char *output)
{
    client *c = open_client(disc);
    client *cold;
    q2_sim *sim = &c->sim[0];
    q2_fire_result_v2 shot = {0};
    q2_save saved = {0};
    q2_projectile timeline[13], resting, moving;
    u32 random_after[13], saved_rng;
    q2_projectile *p;
    s32 start[3], down[3], floor[3], node;
    char path[1024];
    int i, slot;
    client_mp_configure(c, Q2_MP_DEATHMATCH, 2, 10, 10, 3);
    CHECK(client_load_zone(c, "MATRIX5", 0));
    CHECK(c->headless && sim->coll_primary_ready);
    q2_screen_set_layout(&c->screen, Q2_SCREEN_LAYOUT_ONE, 1);
    memcpy(start, sim->player[0].pos, sizeof(start));
    start[1] -= 500;
    memcpy(down, start, sizeof(down)); down[1] += 5000;
    CHECK(!q2_coll_move(&sim->coll_primary, start, down, -1, floor, &node));
    CHECK(node >= 0);
    memcpy(shot.shot[0].origin, floor, sizeof(floor));
    shot.shot[0].origin[1] -= 600;
    shot.shot[0].dir[0] = 1000;
    shot.shot[0].dir[1] = 1000;
    shot.shot[0].damage = 120;
    shot.fired = true; shot.shot_count = 1;
    shot.kind = Q2_FK_GRENADE; shot.mod = 13; shot.projectile_timer = 240;
    slot = q2_projectile_launch(&sim->combat.projectiles, &shot, 0, sim->level_time);
    CHECK(slot >= 0);
    if (slot < 0) { close_client(c); return; }
    p = &sim->combat.projectiles.p[slot];
    sim->invulnerable = true;
    q2_random_seed(1);
    for (i = 0; i < 8; i++) {
        sim->cur_dt = 12; sim->level_time += 12;
        q2_sim_combat_tick(sim);
        printf("Grenade step %d: live=%d bounced=%d at=%d,%d,%d raw=%d,%d,%d blasts=%u\n",
               i + 1, p->in_use, p->bounced, p->pos[0], p->pos[1], p->pos[2],
               q2_projectile_raw_velocity(p, 0), q2_projectile_raw_velocity(p, 1),
               q2_projectile_raw_velocity(p, 2), sim->projectile_blasts);
    }
    CHECK(p->in_use && p->bounced);
    CHECK(sim->projectile_blasts == 0);
    c->cam.pos[0] = floor[0] + 150;
    c->cam.pos[1] = floor[1] - 500;
    c->cam.pos[2] = floor[2] - 1200;
    c->cam.yaw = c->cam.pitch = c->cam.roll = 0;
    c->frame_index++;
    q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
    client_frame(c);
    client_grenade_bounce_audio(c);
    CHECK(c->grenade_bounce_sounds == 1 && c->grenade_bounce_sounds_missed == 0);
    CHECK(sim->grenade_bounce_sounds.count == 0);
    snprintf(path, sizeof(path), "%s/grenade-bounce.ppm", output);
    CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);

    moving = *p;
    for (; i < 21; i++) {
        sim->cur_dt = 12; sim->level_time += 12;
        q2_sim_combat_tick(sim);
    }
    CHECK(!p->in_use && sim->projectile_blasts == 1);
    close_client(c);

    /* Game saves are a campaign feature. Carry the measured rebound state
     * into BASE1, then compare its uninterrupted and cold-loaded histories. */
    c = open_client(disc);
    CHECK(client_load_zone(c, "BASE1", 0));
    sim = &c->sim[0]; sim->invulnerable = true;
    q2_projectiles_init(&sim->combat.projectiles);
    slot = 0; p = &sim->combat.projectiles.p[slot]; *p = moving;
    memcpy(p->pos, sim->player[0].pos, sizeof(p->pos));
    p->pos[1] -= 500;
    p->node = Q2_PROJ_NODE_UNKNOWN;
    p->expires = sim->level_time + 144;
    sim->combat.projectiles.live = 1;

    /* Save both a real moving rebound and a resting toss. Both reuse the
     * existing projectile flags/attitude payload, including in cold saves. */
    CHECK(slot + 1 < Q2_PROJ_MAX && !sim->combat.projectiles.p[slot + 1].in_use);
    resting = *p;
    resting.flags |= Q2_PROJ_TOSS_REST;
    resting.expires += 2000;
    resting.pos[0] += 1500;
    sim->combat.projectiles.p[slot + 1] = resting;
    sim->combat.projectiles.live++;
    saved_rng = q2_random_state();
    CHECK(client_capture(c));
    snprintf(path, sizeof(path), "%s/grenade.q2s", output);
    CHECK(q2_save_write(&c->snapshot, path) == Q2_OK);
    CHECK(q2_save_read(&saved, path) == Q2_OK);
    for (i = 0; i < 13; i++) {
        sim->cur_dt = 12; sim->level_time += 12;
        q2_sim_combat_tick(sim);
        timeline[i] = *p;
        random_after[i] = q2_random_state();
    }
    CHECK(!p->in_use && sim->projectile_blasts == 1);
    close_client(c);
    cold = open_client(disc);
    {
        bool applied = client_apply_save(cold, &saved);
        CHECK(applied);
        if (!applied) { q2_save_free(&saved); close_client(cold); return; }
    }
    sim = &cold->sim[0];
    sim->invulnerable = true;
    p = &sim->combat.projectiles.p[slot];
    CHECK(p->in_use && p->bounced && !(p->flags & Q2_PROJ_TOSS_REST));
    CHECK(q2_random_state() == saved_rng);
    CHECK(sim->combat.projectiles.p[slot + 1].flags == resting.flags);
    CHECK(memcmp(sim->combat.projectiles.p[slot + 1].angles,
                 resting.angles, sizeof(resting.angles)) == 0);
    for (i = 0; i < 13; i++) {
        sim->cur_dt = 12; sim->level_time += 12;
        q2_sim_combat_tick(sim);
        CHECK(p->in_use == timeline[i].in_use && p->flags == timeline[i].flags);
        CHECK(memcmp(p->pos, timeline[i].pos, sizeof(p->pos)) == 0);
        CHECK(memcmp(p->vel, timeline[i].vel, sizeof(p->vel)) == 0);
        CHECK(memcmp(p->angles, timeline[i].angles, sizeof(p->angles)) == 0);
        CHECK(p->expires == timeline[i].expires && q2_random_state() == random_after[i]);
        CHECK(memcmp(sim->combat.projectiles.p[slot + 1].pos,
                     resting.pos, sizeof(resting.pos)) == 0);
    }
    CHECK(!p->in_use && sim->projectile_blasts == 1);
    q2_save_free(&saved);
    close_client(cold);
}

static void test_disc_projectile_contact(const char *disc, const char *output)
{
    static const int axes[4] = {2, 0, 2, 0}, signs[4] = {1, 1, -1, -1};
    client *c = open_client(disc);
    q2_sim *sim = &c->sim[0];
    q2_fire_result_v2 shot = {0};
    q2_projectile *p;
    q2_projectile timeline[8][2];
    q2_save saved = {0};
    u32 random_after[8], saved_rng;
    s16 health_after[8];
    s32 owner[3], end[3], node, slot;
    char path[1024];
    int direction, axis = 2, sign = 1;
    CHECK(client_load_zone(c, "BASE1", 0));
    memcpy(owner, sim->player[0].pos, sizeof(owner));
    owner[1] = q2_sim_origin_y(owner[1]);
    for (direction = 0; direction < 4; direction++) {
        axis = axes[direction]; sign = signs[direction];
        memcpy(shot.shot[0].origin, owner, sizeof(owner));
        shot.shot[0].origin[axis] += sign * 500;
        if (q2_coll_move(&sim->coll_primary, shot.shot[0].origin, owner, -1, end, &node)) break;
    }
    CHECK(direction < 4);
    if (direction == 4) { close_client(c); return; }
    sim->gravity = 0;
    sim->combat.inv.health = 100; sim->combat.inv.armour = 0;
    sim->combat.inv.invuln_until = 0;
    shot.fired = true; shot.shot_count = 1; shot.kind = Q2_FK_GRENADE;
    shot.mod = Q2_MOD_GRENADE; shot.projectile_timer = 1200;
    shot.shot[0].dir[axis] = -sign * 8000; shot.shot[0].damage = 120;
    slot = q2_projectile_launch(&sim->combat.projectiles, &shot, 0, sim->level_time);
    CHECK(slot >= 0);
    if (slot < 0) { close_client(c); return; }
    p = &sim->combat.projectiles.p[slot];
    sim->cur_dt = 20; sim->level_time += 20;
    q2_sim_combat_tick(sim);
    printf("Returning grenade: axis=%d sign=%d alive=%d relative=%d,%d,%d health=%d blasts=%u\n",
           axis, sign, p->in_use, p->pos[0] - owner[0], p->pos[1] - owner[1],
           p->pos[2] - owner[2], sim->combat.inv.health, sim->projectile_blasts);
    CHECK(!p->in_use && sim->projectile_blasts == 1);
    CHECK(p->pos[axis] == owner[axis] + sign * 287);
    CHECK(sim->combat.inv.health == 46);
    memcpy(c->cam.pos, owner, sizeof(owner));
    c->cam.pos[1] -= 100;
    c->cam.yaw = q2_ratan2(axis == 0 ? sign : 0, axis == 2 ? sign : 0);
    c->cam.pitch = c->cam.roll = 0;
    c->frame_index++;
    q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
    client_frame(c);
    snprintf(path, sizeof(path), "%s/grenade-owner-contact.ppm", output);
    CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);

    /* Leave the owner, then reverse the controlled flight and save inside
     * the expanded box but outside the collision cylinder. Resetting the
     * latch on load would incorrectly make the returning grenade harmless. */
    q2_projectiles_init(&sim->combat.projectiles);
    sim->projectile_blasts = 0;
    sim->combat.inv.health = 100; sim->combat.inv.armour = 0;
    memcpy(shot.shot[0].origin, owner, sizeof(owner));
    shot.shot[0].origin[axis] += sign * 100;
    shot.shot[0].dir[axis] = sign * 1600;
    slot = q2_projectile_launch(&sim->combat.projectiles, &shot, 0, sim->level_time);
    CHECK(slot == 0);
    p = &sim->combat.projectiles.p[slot];
    for (direction = 0; direction < 4; direction++) {
        sim->cur_dt = 20; sim->level_time += 20;
        q2_sim_combat_tick(sim);
    }
    CHECK(p->in_use && (p->flags & Q2_PROJ_OWNER_CLEAR));
    CHECK(p->pos[axis] == owner[axis] + sign * 500);
    p->vel[axis] = -p->vel[axis];
    for (direction = 0; direction < 2; direction++) {
        sim->cur_dt = 20; sim->level_time += 20;
        q2_sim_combat_tick(sim);
    }
    CHECK(p->in_use && p->pos[axis] == owner[axis] + sign * 300);
    CHECK(q2_projectile_launch(&sim->combat.projectiles, &shot, 0, sim->level_time) == 1);
    CHECK(!(sim->combat.projectiles.p[1].flags & Q2_PROJ_OWNER_CLEAR));
    saved_rng = q2_random_state();
    CHECK(client_capture(c));
    snprintf(path, sizeof(path), "%s/grenade-owner.q2s", output);
    CHECK(q2_save_write(&c->snapshot, path) == Q2_OK);
    CHECK(q2_save_read(&saved, path) == Q2_OK);
    for (direction = 0; direction < 8; direction++) {
        sim->cur_dt = 20; sim->level_time += 20;
        q2_sim_combat_tick(sim);
        memcpy(timeline[direction], sim->combat.projectiles.p, sizeof(timeline[direction]));
        random_after[direction] = q2_random_state();
        health_after[direction] = sim->combat.inv.health;
    }
    CHECK(!timeline[0][0].in_use && timeline[0][1].in_use);
    CHECK(health_after[0] == 46);
    CHECK(timeline[3][1].flags & Q2_PROJ_OWNER_CLEAR);
    close_client(c);
    c = open_client(disc);
    if (!client_apply_save(c, &saved)) {
        CHECK(false); q2_save_free(&saved); close_client(c); return;
    }
    sim = &c->sim[0]; sim->gravity = 0;
    CHECK(sim->combat.projectiles.p[0].flags & Q2_PROJ_OWNER_CLEAR);
    CHECK(!(sim->combat.projectiles.p[1].flags & Q2_PROJ_OWNER_CLEAR));
    CHECK(q2_random_state() == saved_rng);
    for (direction = 0; direction < 8; direction++) {
        int j;
        sim->cur_dt = 20; sim->level_time += 20;
        q2_sim_combat_tick(sim);
        for (j = 0; j < 2; j++) {
            p = &sim->combat.projectiles.p[j];
            if (memcmp(p->pos, timeline[direction][j].pos, sizeof(p->pos)))
                printf("Owner save tick %d slot %d: live=%d flags=%u node=%d pos=%d,%d,%d expected=%d,%d,%d\n",
                       direction, j, p->in_use, p->flags, p->node,
                       p->pos[0], p->pos[1], p->pos[2], timeline[direction][j].pos[0],
                       timeline[direction][j].pos[1], timeline[direction][j].pos[2]);
            CHECK(p->in_use == timeline[direction][j].in_use);
            CHECK(p->flags == timeline[direction][j].flags);
            CHECK(p->node == timeline[direction][j].node);
            CHECK(memcmp(p->pos, timeline[direction][j].pos, sizeof(p->pos)) == 0);
            CHECK(memcmp(p->vel, timeline[direction][j].vel, sizeof(p->vel)) == 0);
            CHECK(memcmp(p->angles, timeline[direction][j].angles, sizeof(p->angles)) == 0);
            CHECK(p->expires == timeline[direction][j].expires);
        }
        CHECK(q2_random_state() == random_after[direction]);
        CHECK(sim->combat.inv.health == health_after[direction]);
    }
    CHECK(sim->projectile_blasts == 1);
    q2_save_free(&saved);
    close_client(c);
}

static void test_disc_bolt_sequence(const char *disc, const char *output)
{
    static const int axes[4] = {0, 2, 0, 2}, signs[4] = {1, 1, -1, -1};
    client *c = open_client(disc);
    q2_sim *sim = &c->sim[0];
    q2_fire_result_v2 shot = {0};
    q2_projectile *p;
    s32 start[3], end[3], clipped[3], node;
    q2_save saved = {0};
    q2_projectile timeline[32];
    u32 bolt_rng[32], effect_rng[32];
    char path[1024];
    int direction, axis = 0, sign = 1, k;
    u32 i, trails = 0, lights = 0;
    CHECK(client_load_zone(c, "BASE1", 0));
    CHECK(c->headless && sim->coll_primary_ready);
    memcpy(start, sim->player[0].pos, sizeof(start));
    start[1] = q2_sim_origin_y(start[1]);
    for (direction = 0; direction < 4; direction++) {
        axis = axes[direction]; sign = signs[direction];
        memcpy(end, start, sizeof(end)); end[axis] += sign * 800;
        if (q2_coll_move(&sim->coll_primary, start, end, -1, clipped, &node) &&
            !q2_move_clip_segment(&sim->move_world, start, end, NULL, NULL)) break;
    }
    CHECK(direction < 4);
    if (direction == 4) { close_client(c); return; }
    q2_projectiles_init(&sim->combat.projectiles);
    shot.fired = true; shot.shot_count = 1; shot.kind = Q2_FK_BOLT;
    shot.mod = Q2_MOD_BOLT; shot.shot[0].dir[axis] = sign * 40;
    memcpy(shot.shot[0].origin, start, sizeof(start));
    CHECK(q2_projectile_launch(&sim->combat.projectiles, &shot, 0, sim->level_time) == 0);
    p = &sim->combat.projectiles.p[0];
    q2_random_seed(1);
    sim->cur_dt = 12; sim->level_time += 12;
    q2_sim_combat_tick(sim);
    CHECK(p->in_use && p->pos[axis] == start[axis] + sign * 480);
    for (i = 0; i < sim->fx.group_count; i++) {
        const q2_fx_group *g = &sim->fx.group[i];
        if (g->life != Q2_FX_BOLT_TRAIL_LIFE || g->count != Q2_FX_BOLT_TRAIL_COUNT) continue;
        trails++;
        CHECK(g->origin[axis] == start[axis] + sign * 240);
        printf("Bolt trail axis=%d sign=%d origin=%d expected=%d\n", axis, sign,
               g->origin[axis], start[axis] + sign * 240);
    }
    for (i = 0; i < sim->ent_world.events.count; i++) {
        const q2_ent_event *light = &sim->ent_world.events.e[i];
        if (light->kind != Q2_ENT_EVENT_LIGHT || light->radius != Q2_PROJ_LIGHT_OUTER) continue;
        lights++;
        for (k = 0; k < 3; k++) CHECK(light->pos[k] == p->pos[k]);
    }
    CHECK(trails == 1 && lights == 1);
    memcpy(c->cam.pos, start, sizeof(start));
    c->cam.pos[axis] += sign * 240;
    c->cam.pos[axis == 0 ? 2 : 0] -= 1200;
    c->cam.pos[1] -= 100;
    c->cam.yaw = axis == 0 ? 0 : 1024;
    c->cam.pitch = c->cam.roll = 0;
    c->frame_index++;
    q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
    client_frame(c);
    snprintf(path, sizeof(path), "%s/bolt-flight.ppm", output);
    CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);

    /* Save while the bolt is alive; its trail/impact stream must resume at
     * the same draw, including the later wall impact and retirement. */
    CHECK(client_capture(c));
    snprintf(path, sizeof(path), "%s/bolt-flight.q2s", output);
    CHECK(q2_save_write(&c->snapshot, path) == Q2_OK);
    CHECK(q2_save_read(&saved, path) == Q2_OK);
    for (i = 0; i < Q2PSX_ARRAY_COUNT(timeline); i++) {
        sim->cur_dt = 12; sim->level_time += 12;
        q2_sim_combat_tick(sim);
        timeline[i] = *p;
        bolt_rng[i] = sim->combat.rng.state;
        effect_rng[i] = sim->fx_rng.state;
    }
    close_client(c);
    c = open_client(disc);
    if (!client_apply_save(c, &saved)) {
        CHECK(false); q2_save_free(&saved); close_client(c); return;
    }
    sim = &c->sim[0]; p = &sim->combat.projectiles.p[0];
    for (i = 0; i < Q2PSX_ARRAY_COUNT(timeline); i++) {
        sim->cur_dt = 12; sim->level_time += 12;
        q2_sim_combat_tick(sim);
        CHECK(p->in_use == timeline[i].in_use);
        CHECK(p->flags == timeline[i].flags);
        CHECK(p->node == timeline[i].node);
        CHECK(memcmp(p->pos, timeline[i].pos, sizeof(p->pos)) == 0);
        CHECK(sim->combat.rng.state == bolt_rng[i]);
        CHECK(sim->fx_rng.state == effect_rng[i]);
    }
    CHECK(!p->in_use);
    q2_save_free(&saved);
    close_client(c);
}

static void test_player_autoaim(void)
{
    int owner, k;
    for (owner = 0; owner < Q2_SIM_MAX_PLAYERS; owner++) {
        client *c = fixture(Q2_WID_BLASTER, owner);
        q2_sim *sim = &c->sim[0];
        q2_player *p = &sim->player[owner];
        q2_actor actor = {0}, *targets[] = {&actor};
        q2_input input = {0};
        s32 eye[3];
        s16 direction[3], before;
        q2_sim_eye(sim, eye);
        actor.origin[0] = eye[0]; actor.origin[1] = eye[1] - 1260;
        actor.origin[2] = eye[2] + 4096;
        actor.radius = 80; actor.height = 100;
        actor.health = 1000; actor.takedamage = Q2_DAMAGE_AIM;
        q2_sim_set_targets(sim, targets, 1);
        q2_sim_set_aim_view(sim, owner, eye, -1);
        sim->full_basis_movement = true;
        p->pos[0] += 10000; /* acquisition uses the published view, not this new position */
        input.yaw = 4096;
        q2_sim_tick(sim, &input, 12);
        CHECK(p->aim_pitch == -160 && p->aim_assist == -48);
        CHECK(p->yaw != 0); /* acquisition preceded this tick's look input */
        q2_sim_aim(sim, direction);
        CHECK(direction[0] == 0 && direction[1] < 0 && direction[2] > 0);
        p->yaw = 1024; /* all fire substeps still use the acquired direction */
        step(c, true);
        CHECK(c->vw[owner].aim_assist == -48);
        CHECK(sim->combat.last_shot.fired);
        for (k = 0; k < 3; k++)
            CHECK(sim->combat.last_shot.shot[0].dir[k] == direction[k] / 64);
        before = c->vw[owner].aim_assist;
        step(c, false); step(c, false);
        CHECK(c->vw[owner].aim_assist == before); /* display-only updates */
        p->yaw = p->pitch = p->roll = 0;
        memset(&input, 0, sizeof(input));
        q2_sim_set_targets(sim, NULL, 0);
        q2_sim_tick(sim, &input, 6);
        CHECK(p->aim_pitch == 0 && p->aim_assist == -24);
        q2_sim_tick(sim, &input, 6);
        CHECK(p->aim_assist == 0);
        p->motion = Q2_PLAYER_MOTION_CORPSE;
        p->aim_assist = -81;
        q2_sim_set_targets(sim, targets, 1);
        q2_sim_tick(sim, &input, 12);
        CHECK(p->aim_assist == -81); /* corpse think does not acquire/ease */
        release_fixture(c);
    }
}

static void test_disc_autoaim(const char *disc, const char *output)
{
    static const s32 yaws[] = {0, 1024, 2048, 3072};
    client *c = open_client(disc);
    q2_sim *sim = &c->sim[0];
    q2_actor actor = {0}, *targets[] = {&actor};
    q2_save saved = {0};
    q2_input input = {0};
    s32 eye[3], endpoint[3], clipped[3], node;
    s16 direction[3], pitch, assist;
    char path[1024];
    int attempt, k;
    CHECK(client_load_zone(c, "BASE1", 0));
    q2_sim_eye(sim, eye);
    for (attempt = 0; attempt < 4; attempt++) {
        endpoint[0] = eye[0] + (q2_sin12(yaws[attempt]) * 800 >> 12);
        endpoint[1] = eye[1] - 200;
        endpoint[2] = eye[2] + (q2_cos12(yaws[attempt]) * 800 >> 12);
        if (q2_coll_move(&sim->coll_primary, eye, endpoint, -1, clipped, &node)) break;
    }
    CHECK(attempt < 4);
    if (attempt == 4) { close_client(c); return; }
    memcpy(actor.origin, endpoint, sizeof(endpoint));
    actor.origin[1] -= 256;
    actor.radius = 40; actor.height = 60;
    actor.health = 1000; actor.takedamage = Q2_DAMAGE_AIM;
    sim->player[0].yaw = yaws[attempt];
    sim->full_basis_movement = true;
    sim->invulnerable = true;
    q2_sim_set_targets(sim, targets, 1);
    q2_sim_set_aim_view(sim, 0, eye, -1);
    q2_vw_init(&c->vw[0], &c->vm_tables, Q2_WID_BLASTER);
    c->vw[0].state = Q2_VM_IDLE;
    c->vw_last_weapon[0] = Q2_WID_BLASTER;
    client_bind_view_model(c, 0);
    for (k = 0; k < 4; k++) {
        q2_sim_tick(sim, &input, 12);
        client_advance_view_weapon(c, false, 12);
    }
    pitch = sim->player[0].aim_pitch; assist = sim->player[0].aim_assist;
    q2_sim_aim(sim, direction);
    CHECK(pitch < 0 && assist == pitch);
    CHECK(c->vw[0].aim_assist == assist);
    q2_sim_eye(sim, c->cam.pos);
    c->cam.yaw = yaws[attempt]; c->cam.pitch = c->cam.roll = 0;
    c->frame_index++;
    q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
    client_frame(c);
    snprintf(path, sizeof(path), "%s/autoaim.ppm", output);
    CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);
    printf("Autoaim yaw=%d pitch=%d barrel=%d direction=%d,%d,%d\n",
           c->cam.yaw, pitch, c->vw[0].aim_assist, direction[0], direction[1], direction[2]);
    CHECK(client_capture(c));
    snprintf(path, sizeof(path), "%s/autoaim.q2s", output);
    CHECK(q2_save_write(&c->snapshot, path) == Q2_OK);
    CHECK(q2_save_read(&saved, path) == Q2_OK);
    close_client(c);
    c = open_client(disc);
    CHECK(client_apply_save(c, &saved));
    sim = &c->sim[0];
    CHECK(sim->player[0].aim_pitch == pitch && sim->player[0].aim_assist == assist);
    CHECK(c->vw[0].aim_assist == assist);
    q2_sim_aim(sim, direction);
    for (k = 0; k < 3; k++) CHECK(direction[k] == saved.player.aim_direction[k]);
    client_advance_view_weapon(c, true, 12);
    CHECK(sim->combat.last_shot.fired);
    for (k = 0; k < 3; k++)
        CHECK(sim->combat.last_shot.shot[0].dir[k] == direction[k] / 64);
    q2_save_free(&saved);
    close_client(c);
}

int main(int argc, char **argv)
{
    test_all_weapons();
    test_switch_during_burst();
    test_chaingun_burst();
    test_automatic_exhaustion();
    test_frame_result_is_consumed();
    test_player_autoaim();
    if (argc == 4 && !strcmp(argv[1], "--headless-grenade"))
        test_disc_grenade(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "--headless-projectile-contact"))
        test_disc_projectile_contact(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "--headless-bolt"))
        test_disc_bolt_sequence(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "--headless-autoaim"))
        test_disc_autoaim(argv[2], argv[3]);
    printf("weapon client: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
