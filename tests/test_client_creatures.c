/* Exercise creature fire through the actual client, without a window, disc,
 * audio device or physical input. Module-only tests cannot catch a hook that
 * converts a grenade or a missed rail into immediate player damage. */
int q2_test_creature_client_main(int argc, char **argv);
#define main q2_test_creature_client_main
#include "../src/client/main.c"
#undef main
#include "aimove.h"

static int checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    failures++; } } while (0)

#include "client_fixture.h"
#include "fixtures/actor_light_reference.h"
#include "aimove.h"

static void test_fire_routes(void)
{
    client *c = calloc(1, sizeof(*c));
    q2_monster m;
    q2_actor actor, *target = &actor;
    const q2_cre_impl *gunner = q2_cre_impl_find("Gunner");
    const q2_cre_impl *arachner = q2_cre_impl_find("Arachner");
    const s32 feet[3] = { 0, 0, 0 };
    s16 health;

    if (!c) exit(2);
    c->headless = true;
    c->trace_cre = -1;
    c->creatures_ready = true;
    c->creatures.set.monsters = &m;
    c->creatures.set.count = 1;
    c->cre_actor = &actor;
    q2_sim_init(&c->sim[0], NULL, 50);
    q2_sim_spawn(&c->sim[0], feet, 0);
    q2_cre_set_skill(1);
    q2_cre_bind_reset();
    q2_monster_init(&m);
    m.health = m.max_health = 100;
    m.in_use = true;
    m.pos[1] = -286;
    m.pos[2] = 2000;
    m.angles[2] = 2048;
    q2_actor_init(&actor);
    q2_actor_from_monster(&actor, &m);
    q2_sim_set_targets(&c->sim[0], &target, 1);
    q2_monster_init(&c->creatures.sight);
    c->creatures.sight.health = 100;
    c->creatures.sight.pos[1] = -286;
    c->creatures.sight.view_height = -290;
    m.enemy = &c->creatures.sight;
    q2_cre_set_shot_hook(client_cre_shot, c);
    q2_cre_set_muzzle_hook(NULL, NULL);

    gunner->method[2](&m);
    CHECK(c->cre_shots == 1 && c->cre_rounds == 1);
    CHECK(c->cre_rounds_player == 1 && c->sim[0].combat.inv.health < 100);

    /* A grenade continues its volley after losing the enemy. */
    m.enemy = NULL;
    health = c->sim[0].combat.inv.health;
    gunner->method[1](&m);
    CHECK(c->cre_launched == 1 && c->sim[0].combat.projectiles.live == 1);
    CHECK(c->sim[0].combat.projectiles.p[0].kind == Q2_PROJ_GRENADE);
    CHECK(c->sim[0].combat.projectiles.p[0].shooter == &actor);
    CHECK(c->sim[0].combat.inv.health == health);

    /* The saved aim still fires without an enemy, and stepping sideways
     * avoids it even when the AI's sight entity is at the old position. */
    memcpy(m.blind_target, c->creatures.sight.pos, sizeof(m.blind_target));
    c->sim[0].player[0].pos[0] = 2000;
    arachner->method[4](&m);
    CHECK(c->cre_rounds == 2 && c->cre_rounds_world == 1);
    CHECK(c->sim[0].combat.inv.health == health);
    c->sim[0].player[0].pos[0] = 0;
    arachner->method[4](&m);
    CHECK(c->cre_rounds_player == 2);
    CHECK(c->sim[0].combat.inv.health == health - 50);

    {
        const q2_cre_impl *gladiator = q2_cre_impl_find("Gladiator");
        u32 rounds = c->cre_rounds, hits = c->cre_rounds_player;
        c->sim[0].combat.inv.health = 100;
        c->sim[0].player[0].pos[0] = 2000;
        gladiator->method[4](&m);
        CHECK(c->cre_rounds == rounds + 1 && c->cre_rounds_player == hits &&
              c->sim[0].combat.inv.health == 100);
        c->sim[0].player[0].pos[0] = 0;
        gladiator->method[4](&m);
        CHECK(c->cre_rounds_player == hits + 1 &&
              c->sim[0].combat.inv.health == 50);
    }

    /* Flyer's actual methods choose ordinary/hyper bolts at their native
     * frames. The raw target delta reaches the /64 engine wrapper intact. */
    {
        const q2_cre_impl *flyer = q2_cre_impl_find("Flyer");
        q2_projectile *p;
        m.enemy = &c->creatures.sight;
        health = c->sim[0].combat.inv.health;
        m.frame = 82;
        flyer->method[8](&m);
        p = &c->sim[0].combat.projectiles.p[1];
        CHECK(c->cre_launched == 2 && p->damage == 1 && p->shooter == &actor);
        CHECK(p->flags == Q2_PROJ_FLAGS_HYPERBLASTER && p->mod == Q2_MOD_BOLT_HYPER);
        CHECK(q2_projectile_raw_velocity(p, 1) == -4 &&
              q2_projectile_raw_velocity(p, 2) == -31);
        m.frame = 83;
        flyer->method[9](&m);
        p = &c->sim[0].combat.projectiles.p[2];
        CHECK(c->cre_launched == 3 && p->flags == Q2_PROJ_FLAGS_BLASTER &&
              p->mod == Q2_MOD_BOLT && p->damage == 1);
        CHECK(c->sim[0].combat.inv.health == health);
    }

    /* The boss BFG wrapper launches along actor rotation, even though the
     * module supplies a different aim vector and nominal speed. */
    {
        q2_cre_shot shot = {0};
        q2_projectile *p;
        shot.slot = Q2_IMP_FIRE_BFG; shot.damage = 50; shot.speed = 300;
        shot.aimed = true; shot.count = 1; shot.dir[0] = 4096;
        memcpy(shot.start, m.pos, sizeof(shot.start));
        health = c->sim[0].combat.inv.health;
        client_cre_shot(&m, &shot, c);
        p = &c->sim[0].combat.projectiles.p[3];
        CHECK(p->kind == Q2_PROJ_BFG && p->damage == 50 && p->shooter == &actor);
        CHECK(q2_projectile_raw_velocity(p, 0) == 0 &&
              q2_projectile_raw_velocity(p, 1) == 0 &&
              q2_projectile_raw_velocity(p, 2) == -Q2_BFG_RAW_SPEED);
        CHECK(c->sim[0].combat.inv.health == health &&
              !memcmp(m.blind_target, shot.start, sizeof(m.blind_target)));
    }

    c->sim[0].player[0].view_height = Q2_VIEW_CROUCH;
    c->sim[0].player[0].vel[0] = 1200;
    c->sim[0].player[0].vel[2] = -300;
    client_creatures_tick(c, 0.0f, c->creatures.sight.pos);
    CHECK(c->creatures.sight.view_height == Q2_EYE_BASE - Q2_VIEW_CROUCH);
    CHECK(c->creatures.sight.velocity[0] == 1200 &&
          c->creatures.sight.velocity[2] == -300);
    q2_cre_set_shot_hook(NULL, NULL);
    q2_sim_set_targets(&c->sim[0], NULL, 0);
    q2_sim_free(&c->sim[0]);
    free(c);
}

static void test_melee_delivery(void)
{
    client *c = calloc(1, sizeof(*c));
    q2_monster m;
    q2_actor actor;
    q2_input input = { 0 };
    const s32 feet[3] = { 0, 0, 0 };
    const s32 aim[3] = { 1020, 0, 0 };
    int i;
    if (!c) exit(2);
    c->headless = c->creatures_ready = true;
    c->creatures.set.monsters = &m; c->creatures.set.count = 1;
    c->cre_actor = &actor;
    q2_sim_init(&c->sim[0], NULL, 50);
    q2_sim_spawn(&c->sim[0], feet, 0);
    q2_monster_init(&m); q2_monster_init(&c->creatures.sight);
    m.pos[1] = -286; m.pos[2] = 800; m.angles[2] = 2048;
    m.enemy = &c->creatures.sight;
    c->creatures.sight.pos[1] = -286;
    q2_actor_init(&actor); q2_actor_from_monster(&actor, &m);
    c->sim[0].player[0].vel[0] = 123;
    c->sim[0].player[0].impulse[2] = 11;
    client_cre_melee(&m, aim, 20, 400, c);
    CHECK(c->sim[0].combat.inv.health == 80 && c->cre_swings == 1);
    CHECK(c->sim[0].player[0].vel[0] == 0 &&
          c->sim[0].player[0].impulse[2] == -6389 &&
          c->sim[0].player[0].impulse_armed);
    for (i = 0; i < 3; i++) q2_sim_tick(&c->sim[0], &input, 10);
    CHECK(c->sim[0].player[0].pos[2] < 0);
    q2_sim_free(&c->sim[0]); free(c);
}

static int signed_pain_calls;
static s16 signed_pain_amount;
static void signed_pain(q2_monster *m, s16 damage)
{
    (void)m;
    signed_pain_calls++;
    signed_pain_amount = damage;
}

static void test_infighting(void)
{
    client *c = calloc(1, sizeof(*c));
    q2_monster m[2];
    q2_actor actor[2], *targets[2] = { &actor[0], &actor[1] };
    const s32 feet[3] = { 0, 0, 0 };
    const s32 start[3] = { 0, -286, 2000 };
    const s32 dir[3] = { 0, 0, -4000 };
    const s32 aim[3] = { Q2_MELEE_DISTANCE, 0, 0 };
    int i;

    if (!c) exit(2);
    q2_level_reset();
    q2_ai_set_world(NULL);
    c->headless = true;
    c->trace_cre = -1;
    c->creatures_ready = true;
    c->creatures.set.monsters = m;
    c->creatures.set.count = 2;
    c->cre_actor = actor;
    q2_sim_init(&c->sim[0], NULL, 50);
    q2_sim_spawn(&c->sim[0], feet, 0);
    q2_monster_init(&c->creatures.sight);
    c->creatures.sight.client = true;
    c->creatures.sight.health = 100;
    for (i = 0; i < 2; i++) {
        q2_monster_init(&m[i]);
        m[i].health = 100;
        m[i].in_use = true;
        m[i].svflags = Q2_SVF_MONSTER;
        m[i].takedamage = Q2_DAMAGE_AIM;
        m[i].class_id = (u8)(79 + i * 2); /* Gunner and Infantry */
        m[i].pos[1] = -286;
        m[i].pos[2] = 2000 - i * 1000;
        q2_actor_init(&actor[i]);
        q2_actor_from_monster(&actor[i], &m[i]);
    }
    q2_sim_set_targets(&c->sim[0], targets, 2);
    client_cre_round(c, &m[0], start, dir, 10, Q2_MOD_BULLET, false);
    CHECK(m[1].health == 90);
    CHECK(m[1].enemy == &m[0]);
    CHECK(actor[1].damage_source == NULL);
    CHECK(c->sim[0].combat.inv.health == 100);

    /* A retaliating melee must damage the creature it acquired. */
    client_cre_melee(&m[1], aim, 7, 50, c);
    CHECK(m[0].health == 93 && m[0].enemy == &m[1]);
    CHECK(m[0].velocity[2] == 50 && actor[0].knockback[2] == 800);
    CHECK(c->sim[0].combat.inv.health == 100);
    q2_actor_from_monster(&actor[0], &m[0]);
    CHECK(actor[0].knocked && actor[0].knockback[2] == 800);
    client_creatures_tick(c, 1.0f / 30.0f, c->creatures.sight.pos);
    CHECK(m[0].motion.velocity[2] == 800 && !m[0].motion.impulse[2]);
    CHECK(!actor[0].knocked && !(m[0].motion.flags & 0x4000u));

    /* Player-origin damage still resolves to the player stand-in. */
    q2_combat_damage(&c->sim[0].combat.self, &actor[1], 5,
                      Q2_MOD_BULLET, m[1].pos, &c->sim[0].combat.rules);
    client_cre_drain_one(c, 1);
    CHECK(m[1].enemy == &c->creatures.sight);

    /* T_Damage still reacts when godmode or a zero hit leaves health alone.
     * A refused hit and a second drain without another hit do not react. */
    {
        s16 hp = m[1].health;
        u32 pains = c->cre_pain_calls;
        m[1].enemy = NULL;
        m[1].flags |= Q2_FL_GODMODE;
        q2_actor_from_monster(&actor[1], &m[1]);
        q2_combat_damage(&actor[0], &actor[1], 10, Q2_MOD_BULLET,
                          m[0].pos, &c->sim[0].combat.rules);
        CHECK(actor[1].damage_reaction_pending);
        client_cre_drain_one(c, 1);
        CHECK(m[1].health == hp && m[1].enemy == &m[0] &&
              c->cre_pain_calls == pains && !actor[1].damage_reaction_pending);
        m[1].enemy = NULL;
        client_cre_drain_one(c, 1);
        CHECK(m[1].enemy == NULL);
        m[1].flags &= ~Q2_FL_GODMODE;
        q2_actor_from_monster(&actor[1], &m[1]);
        q2_combat_damage(&actor[0], &actor[1], 0, Q2_MOD_BULLET,
                          m[0].pos, &c->sim[0].combat.rules);
        client_cre_drain_one(c, 1);
        CHECK(m[1].health == hp && m[1].enemy == &m[0] &&
              c->cre_pain_calls == pains);
        m[1].enemy = NULL; m[1].takedamage = Q2_DAMAGE_NO;
        q2_actor_from_monster(&actor[1], &m[1]);
        q2_combat_damage(&actor[0], &actor[1], 10, Q2_MOD_BULLET,
                          m[0].pos, &c->sim[0].combat.rules);
        client_cre_drain_one(c, 1);
        CHECK(m[1].health == hp && m[1].enemy == NULL);
    }
    {
        s16 hp = m[1].health;
        u32 pains = c->cre_pain_calls;
        m[1].takedamage = Q2_DAMAGE_AIM;
        m[1].pain = signed_pain;
        signed_pain_calls = 0;
        q2_actor_from_monster(&actor[1], &m[1]);
        q2_combat_damage(&actor[0], &actor[1], -6, Q2_MOD_ENERGY_BOLT,
                          NULL, &c->sim[0].combat.rules);
        client_cre_drain_one(c, 1);
        CHECK(m[1].health == hp + 6 && signed_pain_calls == 1 && signed_pain_amount == -6);
        CHECK(c->cre_pain_calls == pains + 1 && !actor[1].damage_reaction_pending);
        client_cre_drain_one(c, 1);
        CHECK(signed_pain_calls == 1);
        actor[1].health += 7; /* Healing outside T_Damage has no pain callback. */
        client_cre_drain_one(c, 1);
        CHECK(m[1].health == hp + 13 && signed_pain_calls == 1 && c->cre_pain_calls == pains + 1);
    }
    q2_sim_set_targets(&c->sim[0], NULL, 0);
    q2_sim_free(&c->sim[0]);
    free(c);
    q2_level_reset();
}

static void capture_creature(client *c, q2_monster *m,
                              const char *output, const char *name)
{
    char path[1024];
    CHECK(c->headless);
    /* These method fixtures explicitly select AI frames between captures. */
    m->motion.seed_frame = m->frame; m->motion.seed_pending = true;
    client_cre_seed_animation(c, (u32)(m - c->creatures.set.monsters));
    c->cam.pos[0] = m->pos[0];
    c->cam.pos[1] = m->pos[1] - (m->pop_class_id == 9 || m->pop_class_id == 5 ? 200 : 100);
    c->cam.pos[2] = m->pos[2] + (m->pop_class_id == 9 || m->pop_class_id == 5 ? 1100 : 700);
    c->cam.yaw = 2048; c->cam.pitch = 0; c->cam.roll = 0;
    if (m->pop_class_id == 17) {
        c->cam.pos[0] += 750;
        c->cam.pos[2] = m->pos[2] + 850;
        c->cam.yaw = 2560;
    }
    if (m->pop_class_id == 32 || m->pop_class_id == 24) {
        c->cam.pos[0] += 1800;
        c->cam.pos[1] -= 400;
        c->cam.pos[2] = m->pos[2] + 2200;
        c->cam.yaw = 2560;
    }
    if (m->pop_class_id == 2) {
        c->cam.pos[1] = m->pos[1] - 300;
        c->cam.pos[2] = m->pos[2] + 1500;
    }
    if (m->pop_class_id == 13 || m->pop_class_id == 14) {
        c->cam.pos[1] = m->pos[1] - 450;
        c->cam.pos[2] = m->pos[2] + (m->pop_class_id == 13 ? 2400 : 1800);
    }
    if (m->pop_class_id == 15) {
        c->cam.pos[0] += 350;
        c->cam.pos[1] -= 220;
        c->cam.pos[2] = m->pos[2] + 650;
    }
    if (m->pop_class_id == 37) {
        c->cam.pos[1] = m->pos[1] - 200;
        c->cam.pos[2] = m->pos[2] + 1250;
    }
    c->frame_index++;
    q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
    client_frame(c);
    snprintf(path, sizeof(path), "%s/%s.ppm", output, name);
    CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);
}

static void test_disc_ambient(const char *disc_path, const char *output)
{
    client *c = open_client(disc_path);
    q2_monster *m = NULL;
    u32 i;
    char path[1024];
    CHECK(client_load_zone(c, "BASE1", 1));
    for (i = 0; i < c->creatures.set.count; i++) {
        if (!c->creatures.set.monsters[i].in_use || !c->cre_model_ok[i]) continue;
        m = &c->creatures.set.monsters[i];
        CHECK(c->cre_actor[i].ambient[0] == native_actor_ambient);
        break;
    }
    CHECK(m != NULL);
    if (m) capture_creature(c, m, output, "creature-ambient");
    close_client(c);

    c = open_client(disc_path);
    client_mp_configure(c, Q2_MP_DEATHMATCH, 2, 10, 10, 3);
    CHECK(client_load_zone(c, "MATRIX5", 0));
    CHECK(c->headless && c->player_anim_base_ok && c->sim_ready[1]);
    /* A single observation viewport keeps the body large enough to compare;
     * it uses the normal multiplayer body renderer and the real map/model. */
    q2_screen_set_layout(&c->screen, Q2_SCREEN_LAYOUT_ONE, 1);
    memcpy(c->sim[0].player[1].pos, c->sim[0].player[0].pos, sizeof(c->sim[0].player[1].pos));
    c->sim[0].player[1].yaw = 0;
    c->cam.pos[0] = c->sim[0].player[1].pos[0];
    c->cam.pos[1] = c->sim[0].player[1].pos[1] - 350;
    c->cam.pos[2] = c->sim[0].player[1].pos[2] + 1000;
    c->cam.yaw = 2048; c->cam.pitch = c->cam.roll = 0;
    CHECK(c->sim[0].pcombat[1].self.ambient[0] == native_player_ambient);
    c->frame_index++;
    q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
    client_frame(c);
    CHECK(c->player_drawn > 0);
    snprintf(path, sizeof(path), "%s/player-ambient.ppm", output);
    CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);
    close_client(c);
}

/*
 * A DRY CLICK POSES THE BODY. The view-weapon driver returns its last
 * substep's fire word (0x8004F6D4), a dry shot's 2 included, and the player
 * think requests ATTACK for any nonzero return (0x8003ADA0..0x8003ADB4). In a
 * MATRIX5 deathmatch, player 1 stands at its spawn with an empty shotgun and
 * holds fire on its own pad, so the first pull after the raise is dry.
 */
static void test_disc_dry_attack(const char *disc_path, const char *output)
{
    client *c = open_client(disc_path);
    q2_sim *sim = &c->sim[0];
    q2_pad_bindings bind;
    char path[1024];
    int frame;
    u32 shots;
    client_mp_configure(c, Q2_MP_DEATHMATCH, 2, 10, 10, 3);
    CHECK(client_load_zone(c, "MATRIX5", 0));
    CHECK(c->headless && c->player_anim_base_ok && c->sim_ready[1]);
    q2_screen_set_layout(&c->screen, Q2_SCREEN_LAYOUT_ONE, 1);
    /* Past the match start, which hands out the opening loadout. */
    for (frame = 0; frame < 10; frame++) {
        client_input_simulated(c, 1.0f / 30.0f);
        c->frame_index++;
    }
    q2_sim_select_player(sim, 1);
    sim->combat.inv.weapons |= (u16)q2_weapon_tables_builtin()->owned_bit[Q2_WID_SHOTGUN];
    sim->combat.inv.ammo[Q2_AMMO_SHELLS] = 0;
    sim->combat.weapon_id = Q2_WID_SHOTGUN;
    sim->player[1].yaw = 0;
    q2_sim_select_player(sim, 0);
    CHECK(q2_pad_style_bindings(sim->player[1].look_scheme, &bind) && bind.fire);
    for (frame = 0; frame < 120; frame++) {
        client_input_simulated(c, 1.0f / 30.0f);
        c->frame_index++;
    }
    CHECK(c->vw[1].weapon == Q2_WID_SHOTGUN && c->vw[1].state == Q2_VM_IDLE);
    shots = c->mp_shots[1];
    for (frame = 0; frame < 60 && c->mp_dry[1] == 0; frame++) {
        c->mp_pad_pend[1] |= (u16)bind.fire;
        client_input_simulated(c, 1.0f / 30.0f);
        c->frame_index++;
    }
    printf("Dry click: frame %d dry %u shots %u move %d position %d health %d\n",
           frame, (unsigned)c->mp_dry[1], (unsigned)(c->mp_shots[1] - shots),
           (int)c->player_anim[1].state.move, c->player_anim[1].state.position,
           sim->pcombat[1].inv.health);
    CHECK(c->mp_dry[1] == 1 && c->mp_shots[1] == shots);
    CHECK(client_player_visual_move(c, 1) == Q2_PMOVE_ATTAK);
    /* One more frame into the clip, and look at player 1 from its front. */
    c->mp_pad_pend[1] |= (u16)bind.fire;
    client_input_simulated(c, 1.0f / 30.0f);
    c->frame_index++;
    c->cam.pos[0] = sim->player[1].pos[0];
    c->cam.pos[1] = sim->player[1].pos[1] - 350;
    c->cam.pos[2] = sim->player[1].pos[2] + 1000;
    c->cam.yaw = 2048; c->cam.pitch = c->cam.roll = 0;
    q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
    client_frame(c);
    CHECK(c->player_drawn > 0);
    snprintf(path, sizeof(path), "%s/player-dry-attack.ppm", output);
    CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);
    close_client(c);
}

static void test_disc_player_animation(const char *disc_path, const char *output)
{
    client *c = open_client(disc_path);
    q2_model_pose before[64], after[64];
    char path[1024];
    client_mp_configure(c, Q2_MP_DEATHMATCH, 2, 10, 10, 3);
    CHECK(client_load_zone(c, "MATRIX5", 0));
    CHECK(c->headless && c->player_anim_base_ok && c->sim_ready[1]);
    q2_screen_set_layout(&c->screen, Q2_SCREEN_LAYOUT_ONE, 1);
    memcpy(c->sim[0].player[1].pos, c->sim[0].player[0].pos, sizeof(c->sim[0].player[1].pos));
    c->sim[0].player[1].yaw = 0;
    c->sim[0].player[1].on_ground = true;
    c->sim[0].player[1].ent.flags |= Q2_ENT_ON_GROUND;
    c->sim[0].player[1].wish[0] = 1000;
    c->sim[0].player[1].vel[0] = 1000;
    c->vw[1].result = 1;      /* a shot while running keeps RUN */
    client_player_anim_tick(c, 1, 10, false);
    printf("Player animation run: move=%d position=%d range=%u/%u/%u base=%d\n",
           c->player_anim[1].state.move, c->player_anim[1].state.position,
           c->player_moves[Q2_PMOVE_RUN].start, c->player_moves[Q2_PMOVE_RUN].end,
           c->player_moves[Q2_PMOVE_RUN].rest, c->player_anim_base_ok);
    CHECK(client_player_visual_move(c, 1) == Q2_PMOVE_RUN);
    c->cam.pos[0] = c->sim[0].player[1].pos[0];
    c->cam.pos[1] = c->sim[0].player[1].pos[1] - 350;
    c->cam.pos[2] = c->sim[0].player[1].pos[2] + 1000;
    c->cam.yaw = 2048; c->cam.pitch = c->cam.roll = 0;
    memset(&before, 0, sizeof(before)); memset(&after, 0, sizeof(after));
    CHECK(client_player_pose(c, 1, before));
    c->frame_index++;
    q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
    CHECK(client_player_pose(c, 1, after));
    CHECK(!memcmp(&before, &after, sizeof(before)));
    client_frame(c);
    CHECK(c->player_drawn > 0);
    snprintf(path, sizeof(path), "%s/player-run-fire.ppm", output);
    CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);
    /* Real Male2 binding rewinds Stand/Run's rest positions. A repeated run
     * request must continue the clip, not reinstall its first key each tick. */
    {
        q2_player_anim_state saved;
        int move, sample, view;
        u32 random;
        for (sample = 0; sample < 3; sample++) client_player_anim_tick(c, 1, 10, false);
        CHECK(c->player_anim[1].state.position == (s16)(c->player_moves[Q2_PMOVE_RUN].start + 30));
        saved = c->player_anim[1].state;
        for (move = 0; move < Q2_PMOVE_COUNT; move++) {
            c->player_anim[1].state.move = (q2_player_move)move;
            for (sample = 0; sample < 3; sample++) {
                c->player_anim[1].state.position = (s16)(c->player_moves[move].start +
                    (c->player_moves[move].end - c->player_moves[move].start) * sample / 2);
                CHECK(client_player_pose(c, 1, before));
                random = q2_random_state();
                for (view = 0; view < 4; view++) {
                    c->frame_index++;
                    q2_screen_tick_dt(&c->screen, 1.0 / (30.0 * (view + 1)));
                    CHECK(client_player_pose(c, 1, after));
                    CHECK(!memcmp(before, after, sizeof(before)) && q2_random_state() == random);
                }
            }
        }
        c->player_anim[1].state = saved;
    }
    close_client(c);
}

static void check_motion(const q2_cre_motion *a, const q2_cre_motion *b)
{
    int k;
    for (k = 0; k < 3; k++) {
        CHECK(a->ent.pos[k] == b->ent.pos[k] && a->target[k] == b->target[k]);
        CHECK(a->velocity[k] == b->velocity[k] && a->delta[k] == b->delta[k]);
        CHECK(a->impulse[k] == b->impulse[k]);
        CHECK(a->ent.ground_normal[k] == b->ent.ground_normal[k] &&
              a->ent.last_normal[k] == b->ent.last_normal[k]);
    }
    CHECK(a->ent.flags == b->ent.flags && a->ent.max_slope_ny == b->ent.max_slope_ny);
    CHECK(a->yaw == b->yaw && a->target_yaw == b->target_yaw && a->flags == b->flags);
    CHECK(a->clock == b->clock && a->phase == b->phase && a->phase_base == b->phase_base);
    CHECK(a->anim_base == b->anim_base && a->anim_previous == b->anim_previous &&
          a->anim_position == b->anim_position && a->anim_flags == b->anim_flags);
    CHECK(a->queued_yaw == b->queued_yaw && a->animation_ready == b->animation_ready);
    CHECK(a->ready == b->ready);
    CHECK(a->seed_frame == b->seed_frame && a->seed_pending == b->seed_pending);
}

static void test_disc_motion(const char *disc_path, const char *output)
{
    client *c = open_client(disc_path);
    q2_monster *m = NULL;
    q2_cre_motion pending, continued;
    q2_camera camera;
    q2_save saved = {0};
    char path[1024];
    u32 i, index = 0;
    int f;
    CHECK(client_load_zone(c, "BASE1", 1));
    for (i = 0; i < c->creatures.set.count; i++)
        if (c->creatures.set.monsters[i].in_use) { index = i; m = &c->creatures.set.monsters[i]; break; }
    CHECK(m != NULL);
    if (!m) { close_client(c); return; }
    m->think = NULL; m->next_think = 0;
    capture_creature(c, m, output, "motion-start");
    m->pos[0] += 180; m->angles[2] += 256;
    q2_link_entity(m, 0x85);
    client_creatures_tick(c, 1.0f / 30.0f, c->creatures.sight.pos);
    CHECK(m->motion.phase == 10);
    CHECK(m->motion.ent.pos[0] != m->pos[0] && m->motion.target[0] == m->pos[0]);
    CHECK(m->motion.yaw != m->angles[2] && m->motion.target_yaw == m->angles[2]);
    /* Capture through the engine with the original camera still in place. */
    c->frame_index++; q2_screen_tick_dt(&c->screen, 1.0 / 30.0); client_frame(c);
    snprintf(path, sizeof(path), "%s/motion-subframe.ppm", output);
    CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);
    m->motion.impulse[0] = 901; m->motion.impulse[1] = -4444;
    m->motion.flags |= 0x4000u;
    q2_actor_from_monster(&c->cre_actor[index], m);
    pending = m->motion;
    camera = c->cam;
    CHECK(client_capture(c));
    snprintf(path, sizeof(path), "%s/motion-pending.sav", output);
    CHECK(q2_save_write(&c->snapshot, path) == Q2_OK);
    CHECK(q2_save_read(&saved, path) == Q2_OK);
    for (f = 0; f < 9; f++) client_creatures_tick(c, 1.0f / 30.0f, c->creatures.sight.pos);
    continued = m->motion;
    close_client(c); c = open_client(disc_path);
    CHECK(client_apply_save(c, &saved));
    m = &c->creatures.set.monsters[index];
    check_motion(&pending, &m->motion);
    /* The fixture's observation camera is external to the saved game. */
    c->cam = camera; c->frame_index = 2;
    client_frame(c);
    for (f = 0; f < 9; f++) client_creatures_tick(c, 1.0f / 30.0f, c->creatures.sight.pos);
    check_motion(&continued, &m->motion);
    CHECK(client_apply_save(c, &saved));
    c->carry_player = c->carry_same_map = true;
    CHECK(client_load_zone(c, "BASE1", 1));
    m = &c->creatures.set.monsters[index];
    check_motion(&pending, &m->motion);
    q2_save_free(&saved);
    close_client(c);
}

static void test_disc_flyer(const char *disc_path, const char *output)
{
    client *c = open_client(disc_path);
    q2_monster *m = NULL;
    u32 i, flyer_index = 0, before;
    int frame;
    CHECK(client_load_zone(c, "BASE2", 1));
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *candidate = &c->creatures.set.monsters[i];
        if (candidate->pop_class_id == 8 && c->cre_model_ok[i]) {
            m = candidate;
            flyer_index = i;
            break;
        }
    }
    CHECK(m != NULL);
    if (!m) { close_client(c); return; }
    printf("Flyer fixture: population %u, position %d,%d,%d\n", flyer_index,
           m->pos[0], m->pos[1], m->pos[2]);
    /* These placed Flyers are a scripted batch. Wake the real population
     * group through the same function as CREBATCH, then isolate one volley. */
    {
        q2_pop_group group;
        CHECK(q2_pop_get_group(&c->creatures.pop, m->group, &group));
        CHECK(q2_creature_world_summon(&c->creatures, group.name) > 0);
    }
    CHECK(m->in_use && m->start_kind == Q2_CRE_START_FLY);
    for (i = 0; i < c->creatures.set.count; i++)
        if (i != flyer_index) c->creatures.set.monsters[i].in_use = false;
    m->enemy = m->goalentity = &c->creatures.sight;
    m->aiflags |= Q2_AI_STAND_GROUND;
    m->angles[2] = 0;
    /* Actual map and model, with a fixed camera in front of the authored
     * placement. No OS input or screen capture participates in this fixture. */
    c->cam.pos[0] = m->pos[0];
    c->cam.pos[1] = m->pos[1] - 100;
    c->cam.pos[2] = m->pos[2] + 1300;
    c->cam.yaw = 2048; c->cam.pitch = 0; c->cam.roll = 0;
    memcpy(c->creatures.sight.pos, c->cam.pos, sizeof(c->cam.pos));
    c->creatures.sight.view_height = 0;
    c->creatures.sight.health = 100;
    capture_creature(c, m, output, "flyer-idle");
    m->attack(m);
    m->frame = 78;
    before = c->cre_launched;
    for (frame = 0; frame < 17; frame++) {
        q2_M_MoveFrame(m);
        capture_creature(c, m, output, "flyer-volley");
    }
    CHECK(c->cre_launched - before == 8);
    CHECK(c->cre_fire_no_figures == 0);
    /* Move the target into slash range; the real animation drives both
     * melee methods, and export-1-equivalent blade trails sample the pose. */
    memcpy(c->creatures.sight.pos, m->pos, sizeof(m->pos));
    m->melee(m);
    m->frame = 57;
    before = c->cre_swings;
    for (frame = 0; frame < 18; frame++) {
        q2_M_MoveFrame(m);
        client_cre_blurs(c);
        capture_creature(c, m, output, "flyer-melee");
    }
    CHECK(c->cre_swings - before == 2);
    CHECK(c->sim[0].fx.stats.blur_samples > 0);
    q2_actor_from_monster(&c->cre_actor[flyer_index], m);
    q2_combat_damage(&c->sim[0].combat.self, &c->cre_actor[flyer_index],
                      60, Q2_MOD_BULLET, m->pos, &c->sim[0].combat.rules);
    client_cre_drain_one(c, flyer_index);
    CHECK(m->dead && m->health == -10 && m->next_think == 0 &&
          (m->svflags & Q2_SVF_DEADMONSTER));
    CHECK(c->cre_die_calls == 1 && q2_level_state.killed_monsters == 1);
    capture_creature(c, m, output, "flyer-dead");
    close_client(c);
}

static void test_disc_arachner_pitch(const char *disc_path)
{
    client *c = open_client(disc_path);
    q2_monster *m = NULL;
    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, 44100 };
    q2_vag sample;
    u32 i;
    int arm, seed;
    CHECK(client_load_zone(c, "POWER1", 0));
    for (i = 0; i < c->creatures.set.count; i++)
        if (c->creatures.set.monsters[i].pop_class_id == 28) {
            m = &c->creatures.set.monsters[i]; break;
        }
    CHECK(m != NULL);
    CHECK(client_find_sound(c, "ara_pain2", &sample));
    /* An unbound memory stream: no device, speaker output or physical input. */
    c->audio = SDL_CreateAudioStream(&spec, &spec);
    CHECK(c->audio != NULL);
    if (m && c->audio) {
        q2_cre_set_skill(3);
        for (arm = 0; arm < 2; arm++) {
            for (seed = 0; ; seed++) {
                q2_random_seed((u32)seed);
                if (!!(q2_random_next() & 4) == arm) break;
            }
            q2_random_seed((u32)seed);
            m->pain_debounce = 0;
            m->pain(m, 1);
            CHECK(c->voice_last != NULL);
            if (c->voice_last) {
                CHECK(c->voice_last->spu.pitch == q2_sfx_spu_pitch(sample.sample_rate,
                           (u8)(arm ? 35 : 50)));
                CHECK(c->voice_last->positional &&
                      c->voice_last->req_vol == Q2_SFX_VOLUME_DEFAULT);
            }
        }
        q2_cre_set_skill(1);
    }
    SDL_DestroyAudioStream(c->audio); c->audio = NULL;
    close_client(c);
}

static void test_disc_gladiator(const char *disc_path, const char *output)
{
    client *c = open_client(disc_path);
    q2_monster *m = NULL;
    u32 i, index = 0, before;
    int frame;
    CHECK(client_load_zone(c, "BIGGUN", 1));
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *candidate = &c->creatures.set.monsters[i];
        if (candidate->pop_class_id == 9 && c->cre_model_ok[i]) {
            m = candidate; index = i; break;
        }
    }
    CHECK(m != NULL);
    if (!m) { close_client(c); return; }
    printf("Gladiator fixture: population %u, position %d,%d,%d\n", index,
           m->pos[0], m->pos[1], m->pos[2]);
    if (!m->in_use) {
        q2_pop_group group;
        CHECK(q2_pop_get_group(&c->creatures.pop, m->group, &group));
        CHECK(q2_creature_world_summon(&c->creatures, group.name) > 0);
    }
    CHECK(m->in_use && m->start_kind == Q2_CRE_START_WALK);
    for (i = 0; i < c->creatures.set.count; i++)
        if (i != index) c->creatures.set.monsters[i].in_use = false;
    m->enemy = m->goalentity = &c->creatures.sight;
    m->aiflags |= Q2_AI_STAND_GROUND;
    m->angles[2] = 0;
    memcpy(c->creatures.sight.pos, m->pos, sizeof(m->pos));
    c->creatures.sight.pos[2] += 2000;
    c->creatures.sight.view_height = -250;
    c->creatures.sight.health = 100;
    capture_creature(c, m, output, "gladiator-idle");
    m->attack(m); m->frame = 45;
    before = c->cre_rounds;
    for (frame = 0; frame < 9; frame++) {
        q2_M_MoveFrame(m);
        if (m->frame == 49) capture_creature(c, m, output, "gladiator-rail");
    }
    CHECK(c->cre_rounds - before == 1 && c->cre_fire_no_figures == 0);
    memcpy(c->creatures.sight.pos, m->pos, sizeof(m->pos));
    m->melee(m); m->frame = 28;
    before = c->cre_swings;
    for (frame = 0; frame < 17; frame++) q2_M_MoveFrame(m);
    CHECK(c->cre_swings - before == 2);
    capture_creature(c, m, output, "gladiator-melee");
    q2_actor_from_monster(&c->cre_actor[index], m);
    q2_combat_damage(&c->sim[0].combat.self, &c->cre_actor[index],
                      410, Q2_MOD_BULLET, m->pos, &c->sim[0].combat.rules);
    client_cre_drain_one(c, index);
    CHECK(m->dead && m->health == -10 && m->currentmove->first_frame == 61);
    CHECK(c->cre_die_calls == 1 && q2_level_state.killed_monsters == 1);
    for (frame = 0; frame < 23; frame++) q2_M_MoveFrame(m);
    CHECK(m->next_think == 0 && (m->svflags & Q2_SVF_DEADMONSTER));
    capture_creature(c, m, output, "gladiator-dead");
    close_client(c);
}

static void test_disc_hover(const char *disc_path, const char *output)
{
    client *c = open_client(disc_path);
    q2_monster *m = NULL;
    u32 i, index = 0, before, destroyed;
    int frame;
    CHECK(client_load_zone(c, "JAIL3", 1));
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *candidate = &c->creatures.set.monsters[i];
        if (candidate->pop_class_id == 11 && c->cre_model_ok[i]) {
            m = candidate; index = i; break;
        }
    }
    CHECK(m != NULL);
    if (!m) { close_client(c); return; }
    printf("Hover fixture: population %u, position %d,%d,%d\n", index,
           m->pos[0], m->pos[1], m->pos[2]);
    if (!m->in_use) {
        q2_pop_group group;
        CHECK(q2_pop_get_group(&c->creatures.pop, m->group, &group));
        CHECK(q2_creature_world_summon(&c->creatures, group.name) > 0);
    }
    CHECK(m->in_use && m->start_kind == Q2_CRE_START_FLY);
    for (i = 0; i < c->creatures.set.count; i++)
        if (i != index) c->creatures.set.monsters[i].in_use = false;
    m->enemy = m->goalentity = &c->creatures.sight;
    m->aiflags |= Q2_AI_STAND_GROUND;
    m->angles[2] = 0;
    memcpy(c->creatures.sight.pos, m->pos, sizeof(m->pos));
    c->creatures.sight.pos[2] += 2000;
    c->creatures.sight.view_height = -250;
    c->creatures.sight.health = 100;
    capture_creature(c, m, output, "hover-idle");
    m->attack(m); m->frame = 196;
    before = c->cre_launched;
    for (frame = 0; frame < 5; frame++) q2_M_MoveFrame(m);
    CHECK(m->frame == 201 && c->cre_launched - before == 2);
    CHECK(c->cre_fire_no_figures == 0);
    CHECK(c->sim[0].combat.projectiles.p[0].flags == Q2_PROJ_FLAGS_HYPERBLASTER &&
          c->sim[0].combat.projectiles.p[1].flags == Q2_PROJ_FLAGS_BLASTER);
    capture_creature(c, m, output, "hover-volley");
    q2_actor_from_monster(&c->cre_actor[index], m);
    q2_combat_damage(&c->sim[0].combat.self, &c->cre_actor[index],
                      (s16)(m->health + 1), Q2_MOD_BULLET, m->pos,
                      &c->sim[0].combat.rules);
    client_cre_drain_one(c, index);
    CHECK(m->dead && m->health == -1 && m->currentmove->first_frame == 162);
    CHECK(c->cre_die_calls == 1 && q2_level_state.killed_monsters == 1);
    destroyed = q2_gib_counters.destroyed;
    for (frame = 0; frame < 11; frame++) q2_monster_set_tick(&c->creatures.set);
    capture_creature(c, m, output, "hover-death");
    q2_monster_set_tick(&c->creatures.set);
    CHECK(m->health == m->gib_health && m->next_think == 0 &&
          (m->svflags & Q2_SVF_DEADMONSTER));
    q2_monster_set_tick(&c->creatures.set);
    CHECK(m->gibbed && !m->in_use && m->takedamage == Q2_DAMAGE_NO);
    CHECK(q2_gib_counters.destroyed == destroyed + 1);
    capture_creature(c, m, output, "hover-explosion");
    close_client(c);
}

static void test_disc_parasite(const char *disc_path, const char *output)
{
    client *c = open_client(disc_path);
    q2_monster *m = NULL;
    q2_move_body body[2] = { 0 };
    q2_move_bodies bodies = { 0 };
    const q2_ai_world *world;
    q2_ai_trace trace;
    s32 feet[3], from[3];
    u32 i, index = 0, blocker;
    CHECK(client_load_zone(c, "BASE3", 0));
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *candidate = &c->creatures.set.monsters[i];
        if (candidate->pop_class_id == 17 && c->cre_model_ok[i]) {
            m = candidate; index = i; break;
        }
    }
    CHECK(m != NULL);
    if (!m) { close_client(c); return; }
    printf("Parasite fixture: population %u, position %d,%d,%d\n", index,
           m->pos[0], m->pos[1], m->pos[2]);
    if (!m->in_use) {
        q2_pop_group group;
        CHECK(q2_pop_get_group(&c->creatures.pop, m->group, &group));
        CHECK(q2_creature_world_summon(&c->creatures, group.name) > 0);
    }
    CHECK(m->in_use && m->start_kind == Q2_CRE_START_WALK);
    for (i = 0; i < c->creatures.set.count; i++)
        if (i != index) c->creatures.set.monsters[i].in_use = false;
    m->enemy = m->goalentity = &c->creatures.sight;
    m->angles[2] = 0; m->aiflags |= Q2_AI_STAND_GROUND;
    memcpy(c->creatures.sight.pos, m->pos, sizeof(m->pos));
    c->creatures.sight.pos[2] += 1300;
    c->creatures.sight.health = 100;
    memcpy(feet, c->creatures.sight.pos, sizeof(feet)); feet[1] += 286;
    q2_sim_spawn(&c->sim[0], feet, 0);
    memcpy(body[0].pos, c->creatures.sight.pos, sizeof(body[0].pos));
    body[0].id = -2; body[0].solid = true;
    body[0].sweep_radius = body[0].radius = 286; body[0].height = 572;
    bodies.list = body; bodies.count = 1;
    q2_ai_world_bind_bodies(&c->ai_world, &bodies, c->creatures.set.monsters,
                            c->creatures.set.count);
    world = q2_ai_get_world();
    memcpy(from, m->pos, sizeof(from)); from[1] += 72; from[2] += 288;
    world->trace(world->user, from, NULL, NULL, c->creatures.sight.pos,
                  m, 0x06000003u, &trace);
    CHECK(trace.ent == &c->creatures.sight && trace.fraction < 4096);
    capture_creature(c, m, output, "parasite-idle");
    m->attack(m); m->frame = 41;
    q2_cre_impl_find("Parasite")->method[12](m);
    CHECK(c->sim[0].combat.inv.health == 95);
    CHECK(q2_fx_timed_live(&c->sim[0].fx) == 1 &&
          c->sim[0].fx.timed[0].attachment == 97 &&
          c->sim[0].fx.timed[0].radius == 10);
    capture_creature(c, m, output, "parasite-drain");
    m->frame = 42;
    q2_cre_impl_find("Parasite")->method[12](m);
    CHECK(c->sim[0].combat.inv.health == 93 && q2_fx_timed_live(&c->sim[0].fx) == 1);
    /* A nearer body must replace the farther target identity in the trace. */
    blocker = index == 0 ? 1 : 0;
    body[1] = body[0]; body[1].id = (s32)blocker;
    body[1].pos[2] = m->pos[2] + 700; body[1].sweep_radius = 100;
    bodies.count = 2;
    world->trace(world->user, from, NULL, NULL, c->creatures.sight.pos,
                  m, 0x06000003u, &trace);
    CHECK(trace.ent == &c->creatures.set.monsters[blocker]);
    q2_cre_impl_find("Parasite")->method[12](m);
    CHECK(c->sim[0].combat.inv.health == 93);
    bodies.count = 1;
    c->creatures.sight.pos[0] += 100;
    client_cre_beams_update(c);
    CHECK(c->sim[0].fx.timed[0].to[0] == c->creatures.sight.pos[0] &&
          c->sim[0].fx.timed[0].to[1] == c->creatures.sight.pos[1]);
    q2_fx_timed_tick(&c->sim[0].fx, 45);
    CHECK(q2_fx_timed_live(&c->sim[0].fx) == 0);
    close_client(c);
}

static void test_disc_ironmaiden(const char *disc_path, const char *output)
{
    client *c = open_client(disc_path);
    q2_monster *m = NULL;
    u32 i, index = 0, before;
    int frame;
    CHECK(client_load_zone(c, "BOSS1", 0));
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *candidate = &c->creatures.set.monsters[i];
        if (candidate->pop_class_id == 5 && c->cre_model_ok[i]) {
            m = candidate; index = i; break;
        }
    }
    CHECK(m != NULL);
    if (!m) { close_client(c); return; }
    printf("Ironmaiden fixture: population %u, position %d,%d,%d\n", index,
           m->pos[0], m->pos[1], m->pos[2]);
    if (!m->in_use) {
        q2_pop_group group;
        CHECK(q2_pop_get_group(&c->creatures.pop, m->group, &group));
        CHECK(q2_creature_world_summon(&c->creatures, group.name) > 0);
    }
    CHECK(m->in_use && m->start_kind == Q2_CRE_START_WALK);
    for (i = 0; i < c->creatures.set.count; i++)
        if (i != index) c->creatures.set.monsters[i].in_use = false;
    m->enemy = m->goalentity = &c->creatures.sight;
    m->aiflags |= Q2_AI_STAND_GROUND; m->angles[2] = 0;
    memcpy(c->creatures.sight.pos, m->pos, sizeof(m->pos));
    c->creatures.sight.pos[2] += 2000;
    c->creatures.sight.view_height = -250;
    c->creatures.sight.health = 100;
    capture_creature(c, m, output, "ironmaiden-idle");
    m->attack(m); m->frame = -1; before = c->cre_launched;
    for (frame = 0; frame < 14; frame++) q2_M_MoveFrame(m);
    CHECK(m->frame == 13 && c->cre_launched - before == 1);
    CHECK(c->cre_fire_no_figures == 0 &&
          c->sim[0].combat.projectiles.p[0].kind == Q2_PROJ_ROCKET);
    capture_creature(c, m, output, "ironmaiden-rocket");
    memcpy(c->creatures.sight.pos, m->pos, sizeof(m->pos));
    m->melee(m); m->frame = 31; before = c->cre_swings;
    for (frame = 0; frame < 11; frame++) {
        q2_M_MoveFrame(m);
        capture_creature(c, m, output, "ironmaiden-slash");
        client_cre_blurs(c);
    }
    CHECK(c->cre_swings - before == 1 && c->sim[0].fx.stats.blur_samples > 0);
    q2_actor_from_monster(&c->cre_actor[index], m);
    q2_combat_damage(&c->sim[0].combat.self, &c->cre_actor[index],
                      (s16)(m->health + 1), Q2_MOD_BULLET, m->pos,
                      &c->sim[0].combat.rules);
    client_cre_drain_one(c, index);
    CHECK(m->dead && m->health == -1 &&
          (m->currentmove->first_frame == 48 || m->currentmove->first_frame == 60));
    CHECK(c->cre_die_calls == 1 && q2_level_state.killed_monsters == 1);
    for (frame = 0; frame < 24; frame++) q2_monster_set_tick(&c->creatures.set);
    CHECK(m->next_think == 0 && (m->svflags & Q2_SVF_DEADMONSTER));
    capture_creature(c, m, output, "ironmaiden-dead");
    close_client(c);
}

static void test_disc_strider(const char *disc_path, const char *output)
{
    client *c = open_client(disc_path);
    q2_monster *m = NULL;
    u32 i, index = 0, before;
    int frame;
    CHECK(client_load_zone(c, "BIGGUN", 3));
    for (i = 0; i < c->creatures.set.count; i++)
        if (c->creatures.set.monsters[i].pop_class_id == 32 && c->cre_model_ok[i]) {
            m = &c->creatures.set.monsters[i]; index = i; break;
        }
    CHECK(m != NULL);
    if (!m) { close_client(c); return; }
    printf("Strider fixture: population %u, position %d,%d,%d\n", index,
           m->pos[0], m->pos[1], m->pos[2]);
    if (!m->in_use) {
        q2_pop_group group;
        CHECK(q2_pop_get_group(&c->creatures.pop, m->group, &group));
        CHECK(q2_creature_world_summon(&c->creatures, group.name) > 0);
    }
    CHECK(m->in_use && m->bigturn);
    for (i = 0; i < c->creatures.set.count; i++)
        if (i != index) c->creatures.set.monsters[i].in_use = false;
    m->enemy = m->goalentity = &c->creatures.sight;
    m->aiflags |= Q2_AI_STAND_GROUND; m->angles[2] = 0;
    memcpy(c->creatures.sight.pos, m->pos, sizeof(m->pos));
    c->creatures.sight.pos[2] += 3000; c->creatures.sight.health = 100;
    capture_creature(c, m, output, "strider-idle");
    q2_cre_set_move(m, 137); m->frame = 136; before = c->cre_launched;
    for (frame = 0; frame < 7; frame++) q2_M_MoveFrame(m);
    CHECK(c->cre_launched - before == 4 && c->cre_fire_no_figures == 0);
    CHECK(c->sim[0].combat.projectiles.p[0].flags == Q2_PROJ_FLAGS_HYPERBLASTER);
    capture_creature(c, m, output, "strider-blaster");
    q2_cre_set_move(m, 189); m->frame = 188; before = c->cre_rounds;
    for (frame = 0; frame < 9; frame++) {
        q2_M_MoveFrame(m);
        if (m->frame == 196) capture_creature(c, m, output, "strider-laser");
    }
    CHECK(c->cre_rounds - before == 2 && c->sim[0].fx.stats.beams_queued >= 2);
    q2_actor_from_monster(&c->cre_actor[index], m);
    q2_combat_damage(&c->sim[0].combat.self, &c->cre_actor[index],
                      (s16)(m->health + 1), Q2_MOD_BULLET, m->pos,
                      &c->sim[0].combat.rules);
    client_cre_drain_one(c, index);
    CHECK(m->dead && m->currentmove->first_frame == 111);
    for (frame = 0; frame < 27; frame++) q2_monster_set_tick(&c->creatures.set);
    CHECK(m->health == m->gib_health && (m->svflags & Q2_SVF_DEADMONSTER));
    /* The frame end callback marks the corpse; the next shared tick detaches
     * it and dispatches the native gib/explosion destruction. */
    q2_monster_set_tick(&c->creatures.set);
    CHECK(m->health == m->gib_health && m->gibbed && !m->in_use);
    CHECK(c->sim[0].explosion_sounds.count > 0);
    capture_creature(c, m, output, "strider-death");
    close_client(c);
}

static void test_disc_boss(const char *disc_path, const char *output, bool flying)
{
    client *c = open_client(disc_path);
    q2_monster *m = NULL;
    u32 i, index = 0, rounds, launches, flashes;
    int frame;
    CHECK(client_load_zone(c, flying ? "COMMAND" : "SECURITY", flying ? 4 : 1));
    for (i = 0; i < c->creatures.set.count; i++)
        if (c->creatures.set.monsters[i].pop_class_id == (flying ? 2 : 24) &&
            c->cre_model_ok[i]) { m = &c->creatures.set.monsters[i]; index = i; break; }
    CHECK(m != NULL);
    if (!m) { close_client(c); return; }
    printf("Boss%d fixture: population %u, position %d,%d,%d\n", flying ? 2 : 1,
           index, m->pos[0], m->pos[1], m->pos[2]);
    if (!m->in_use) {
        q2_pop_group group;
        CHECK(q2_pop_get_group(&c->creatures.pop, m->group, &group));
        CHECK(q2_creature_world_summon(&c->creatures, group.name) > 0);
    }
    CHECK(m->in_use && m->attack);
    for (i = 0; i < c->creatures.set.count; i++)
        if (i != index) c->creatures.set.monsters[i].in_use = false;
    if (flying) {
        m->frame = -1;
        q2_M_MoveFrame(m); q2_M_MoveFrame(m);
        CHECK(m->health == 1999);
    }
    m->enemy = m->goalentity = &c->creatures.sight;
    m->aiflags |= Q2_AI_STAND_GROUND; m->angles[2] = 0;
    memcpy(c->creatures.sight.pos, m->pos, sizeof(m->pos));
    c->creatures.sight.pos[2] += 3000; c->creatures.sight.health = 100;
    capture_creature(c, m, output, flying ? "boss2-idle" : "boss1-idle");
    rounds = c->cre_rounds; flashes = c->cre_flashes;
    q2_cre_set_move(m, flying ? 79 : 0); m->frame = flying ? 78 : -1;
    for (frame = 0; frame < (flying ? 5 : 6); frame++) q2_M_MoveFrame(m);
    CHECK(c->cre_rounds - rounds == (flying ? 10u : 6u));
    CHECK(c->cre_flashes - flashes == (flying ? 0u : 6u));
    capture_creature(c, m, output, flying ? "boss2-chainguns" : "boss1-chaingun");
    launches = c->cre_launched;
    q2_cre_set_move(m, flying ? 89 : 20); m->frame = flying ? 88 : 19;
    for (frame = 0; frame < (flying ? 13 : 14); frame++) q2_M_MoveFrame(m);
    CHECK(c->cre_launched - launches == (flying ? 4u : 3u) && !c->cre_fire_no_figures);
    capture_creature(c, m, output, flying ? "boss2-rockets" : "boss1-rockets");
    CHECK(c->pose_by_name > 0 && c->pose_name_absent == 0 && c->pose_no_name == 0);
    q2_actor_from_monster(&c->cre_actor[index], m);
    q2_combat_damage(&c->sim[0].combat.self, &c->cre_actor[index],
                      (s16)(m->health + 1), Q2_MOD_BULLET, m->pos,
                      &c->sim[0].combat.rules);
    client_cre_drain_one(c, index);
    CHECK(m->dead && m->takedamage == Q2_DAMAGE_NO);
    for (frame = 0; frame < (flying ? 1 : 36); frame++) q2_monster_set_tick(&c->creatures.set);
    CHECK(m->gibbed && !m->in_use);
    CHECK(c->sim[0].explosion_sounds.count > 0);
    capture_creature(c, m, output, flying ? "boss2-death" : "boss1-death");
    close_client(c);
}

static void test_disc_script(const char *disc_path, const char *output,
                              const char *map, int zone, const char *name)
{
    client *c = open_client(disc_path);
    q2_monster *m = NULL;
    q2_save saved = {0};
    q2_zone_runtime runtime = {0};
    u32 i, index = 0, phase = 0, sounds;
    u8 think;
    s16 frame, scale;
    char shot[64], path[1024];
    bool projection = !strcmp(name, "RiderStand");
    CHECK(client_load_zone(c, map, zone));
    for (i = 0; i < c->creatures.set.count; i++) {
        const q2_cre_bind *b = q2_cre_bind_for(&c->creatures.set.monsters[i]);
        if (b && !strcmp(b->impl->name, name) && c->cre_model_ok[i]) {
            m = &c->creatures.set.monsters[i]; index = i; break;
        }
    }
    CHECK(m != NULL);
    if (!m) { close_client(c); return; }
    printf("%s fixture: population %u, class %u, position %d,%d,%d\n", name,
           index, m->pop_class_id, m->pos[0], m->pos[1], m->pos[2]);
    if (!m->in_use) {
        q2_pop_group group;
        CHECK(q2_pop_get_group(&c->creatures.pop, m->group, &group));
        CHECK(q2_creature_world_summon(&c->creatures, group.name) > 0);
    }
    CHECK(m->in_use && m->think && m->start_kind == Q2_CRE_START_SCRIPT);
    for (i = 0; i < c->creatures.set.count; i++)
        if (i != index) c->creatures.set.monsters[i].in_use = false;
    m->angles[2] = 0;
    snprintf(shot, sizeof(shot), "%s-start", name);
    capture_creature(c, m, output, shot);
    CHECK(c->pose_by_name > 0 && !c->pose_name_absent && !c->pose_no_name);
    sounds = c->cre_sounds;
    if (m->use) {
        const char *event = projection ? "E_NearMakron" : "E_GotHead";
        u8 idle_think = q2_cre_think_id(m);
        CHECK(q2_event_rt_disable_named(&c->sim[0].event_rt, event, false));
        client_native_creature_use(c);
        CHECK(q2_cre_think_id(m) == idle_think);
        CHECK(q2_event_rt_disable_named(&c->sim[0].event_rt, event, true));
        client_native_creature_use(c);
        client_native_creature_use(c);
        CHECK(c->cre_sounds == sounds + 1);
        if (projection) { m->pain(m, 1); sounds++; }
    }
    for (i = 0; i < 4; i++) q2_monster_set_tick(&c->creatures.set);
    snprintf(shot, sizeof(shot), "%s-active", name);
    capture_creature(c, m, output, shot);
    if (projection) {
        CHECK(m->render_scale == 2048 && m->script_pain_used);
        CHECK(c->cre_fx[index].inst.scale == 2048);
        CHECK(c->cre_sounds - sounds == 2 && !c->cre_sound_unnamed);
    } else if (m->use) {
        CHECK(m->frame == 4 && c->cre_sounds - sounds == 5);
    }
    frame = m->frame; scale = m->render_scale; think = q2_cre_think_id(m);
    CHECK(client_capture(c));
    snprintf(path, sizeof(path), "%s/%s-script.sav", output, name);
    CHECK(q2_save_write(&c->snapshot, path) == Q2_OK);
    CHECK(q2_save_read(&saved, path) == Q2_OK);
    CHECK(q2_zone_capture_monster(&runtime, &c->creatures, m));
    runtime.ai_time = q2_level_state.time;
    m->think = NULL; m->render_scale = 4096; m->frame = -1; m->script_pain_used = false;
    CHECK(q2_save_apply_ai(&saved, &c->creatures, &phase) == Q2_OK);
    CHECK(m->frame == frame && m->render_scale == scale && q2_cre_think_id(m) == think);
    if (projection) CHECK(m->script_pain_used);
    m->think = NULL; m->render_scale = 4096; m->frame = -1;
    CHECK(q2_zone_apply_monster(&runtime, &c->creatures, m));
    CHECK(m->frame == frame && m->render_scale == scale && q2_cre_think_id(m) == think);
    q2_save_free(&saved);
    close_client(c);
}

static void test_disc_jorg_rider(const char *disc_path, const char *output)
{
    client *c = open_client(disc_path);
    q2_monster *jorg = NULL, *rider = NULL;
    q2_save saved = {0}; u32 i, ji = 0, ri = 0, shots, phase = 0; int f;
    u8 explosion_phase;
    char path[1024];
    CHECK(client_load_zone(c, "BOSS2", 1));
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *m = &c->creatures.set.monsters[i];
        if (m->pop_class_id == 13 && m->cre_bind && c->cre_model_ok[i]) { jorg = m; ji = i; }
    }
    CHECK(jorg != NULL);
    if (!jorg) { close_client(c); return; }
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *m = &c->creatures.set.monsters[i];
        if (m->group >= c->creatures.pop.group_count && m->owner == jorg) { rider = m; ri = i; }
        if (i != ji) m->in_use = false;
    }
    CHECK(rider && c->cre_model_ok[ri] && !rider->in_use && rider->spawn_pending);
    if (!rider) { close_client(c); return; }
    if (!jorg->in_use) {
        q2_pop_group group;
        CHECK(q2_pop_get_group(&c->creatures.pop, jorg->group, &group));
        CHECK(q2_creature_world_summon(&c->creatures, group.name) > 0);
    }
    jorg->enemy = jorg->goalentity = &c->creatures.sight;
    jorg->aiflags |= Q2_AI_STAND_GROUND; jorg->angles[2] = 0;
    memcpy(c->creatures.sight.pos, jorg->pos, sizeof(jorg->pos));
    c->creatures.sight.pos[2] += 4000; c->creatures.sight.health = 100;
    capture_creature(c, jorg, output, "jorg-idle");
    shots = c->cre_rounds; q2_cre_set_move(jorg, 8); jorg->frame = 7;
    for (f = 0; f < 6; f++) q2_M_MoveFrame(jorg);
    CHECK(c->cre_rounds == shots + 12);
    capture_creature(c, jorg, output, "jorg-machineguns");
    shots = c->cre_launched; q2_cre_set_move(jorg, 18); jorg->frame = 17;
    for (f = 0; f < 13; f++) q2_M_MoveFrame(jorg);
    CHECK(c->cre_launched == shots + 1);
    capture_creature(c, jorg, output, "jorg-bfg");
    jorg->health = 0; jorg->die(jorg, 1);
    for (f = 0; f < 60 && !rider->population_active; f++) {
        q2_level_state.time++; q2_M_MoveFrame(jorg);
    }
    CHECK(rider->population_active && rider->in_use && rider->render_scale == 0 &&
          rider->think == q2_cre_rider_launch && rider->next_think == q2_level_state.time + 2);
    CHECK(!strcmp(rider->cre_bind->impl->name, "JorgRider"));
    explosion_phase = jorg->cre_bind->jorg_explosion_phase;
    CHECK(client_capture(c));
    snprintf(path, sizeof(path), "%s/jorg-transition.sav", output);
    CHECK(q2_save_write(&c->snapshot, path) == Q2_OK);
    CHECK(q2_save_read(&saved, path) == Q2_OK);
    rider->think = NULL; rider->render_scale = 4096;
    jorg->cre_bind->jorg_explosion_phase = 0;
    CHECK(q2_save_apply_ai(&saved, &c->creatures, &phase) == Q2_OK);
    CHECK(rider->think == q2_cre_rider_launch && !rider->render_scale);
    CHECK(jorg->cre_bind->jorg_explosion_phase == explosion_phase);
    close_client(c); c = open_client(disc_path);
    CHECK(client_apply_save(c, &saved));
    jorg = &c->creatures.set.monsters[ji]; rider = &c->creatures.set.monsters[ri];
    CHECK(rider->owner == jorg && rider->think == q2_cre_rider_launch && !rider->render_scale &&
          jorg->cre_bind->jorg_explosion_phase == explosion_phase);
    /* A zone rebuild must restore this pending launch without replaying
     * either spawn hook or waking the hidden child as an ordinary monster. */
    {
        s32 deadline = rider->next_think;
        c->carry_player = c->carry_same_map = true;
        CHECK(client_load_zone(c, "BOSS2", 1));
        jorg = &c->creatures.set.monsters[ji]; rider = &c->creatures.set.monsters[ri];
        CHECK(rider->in_use && rider->owner == jorg && !rider->render_scale &&
              rider->think == q2_cre_rider_launch && rider->next_think == deadline &&
              jorg->cre_bind->jorg_explosion_phase == explosion_phase);
    }
    q2_monster_set_tick(&c->creatures.set); q2_monster_set_tick(&c->creatures.set);
    CHECK(rider->render_scale == 4096 && rider->think == q2_M_MoveFrame &&
          rider->currentmove && rider->currentmove->first_frame == 188 && rider->health == 3000);
    jorg->in_use = false; rider->angles[2] = 0;
    rider->enemy = rider->goalentity = &c->creatures.sight;
    rider->aiflags |= Q2_AI_STAND_GROUND;
    capture_creature(c, rider, output, "rider-emerging");
    shots = c->cre_launched; q2_cre_set_move(rider, 209); rider->frame = 208;
    for (f = 0; f < 26; f++) q2_M_MoveFrame(rider);
    CHECK(c->cre_launched == shots + 17);
    capture_creature(c, rider, output, "rider-blaster");
    shots = c->cre_rounds; q2_cre_set_move(rider, 235); rider->frame = 234;
    for (f = 0; f < 16; f++) q2_M_MoveFrame(rider);
    CHECK(c->cre_rounds == shots + 1);
    capture_creature(c, rider, output, "rider-rail");
    shots = c->cre_launched; q2_cre_set_move(rider, 201); rider->frame = 200;
    for (f = 0; f < 8; f++) q2_M_MoveFrame(rider);
    CHECK(c->cre_launched == shots + 1);
    CHECK(c->pose_by_name && !c->pose_name_absent && !c->pose_no_name && !c->cre_fire_no_figures);
    printf("Jorg/Rider fixture: rounds=%u launches=%u unnamed sounds=%u missing sounds=%u\n",
           c->cre_rounds, c->cre_launched, c->cre_sound_unnamed, c->cre_sound_missing);
    q2_save_free(&saved); close_client(c);
}

static void test_disc_medic(const char *disc_path, const char *output)
{
    client *c = open_client(disc_path); q2_monster *medic = NULL, *corpse = NULL;
    q2_save saved = {0}; u32 i, mi = 0, ci = 0, phase = 0, shots, destroyed; s16 height;
    char path[1024];
    CHECK(client_load_zone(c, "JAIL3", 3));
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *m = &c->creatures.set.monsters[i];
        if (m->pop_class_id == 15 && m->cre_bind && c->cre_model_ok[i] &&
            (m->spawn_pending || m->in_use)) { medic = m; mi = i; break; }
    }
    CHECK(medic != NULL);
    if (!medic) { close_client(c); return; }
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *m = &c->creatures.set.monsters[i];
        if (i != mi && m->cre_bind && !m->cre_bind->impl->scripted && c->cre_model_ok[i]) {
            corpse = m; ci = i; break;
        }
    }
    CHECK(corpse != NULL);
    if (!corpse) { close_client(c); return; }
    for (i = 0; i < c->creatures.set.count; i++) c->creatures.set.monsters[i].in_use = false;
    medic->in_use = corpse->in_use = true; medic->spawn_pending = corpse->spawn_pending = false;
    medic->population_active = corpse->population_active = true;
    q2_monster_start_go(medic); q2_monster_start_go(corpse);
    medic->angles[2] = 0; memcpy(corpse->pos, medic->pos, sizeof(corpse->pos));
    corpse->pos[2] += 1100; height = corpse->hit_height;
    corpse->health = 0; corpse->die(corpse, 1);
    for (i = 0; i < 100 && !(corpse->svflags & Q2_SVF_DEADMONSTER); i++) q2_M_MoveFrame(corpse);
    q2_monster_corpse_detach(corpse);
    CHECK(corpse->corpse && corpse->health > corpse->gib_health);
    /* Place the test body in real, visible geometry beside this map's Medic.
     * A fixed +Z offset crosses a wall at the authored spawn. */
    {
        static const s32 offsets[8][2] = {
            {700,0}, {-700,0}, {0,-700}, {0,700},
            {800,800}, {-800,800}, {800,-800}, {-800,-800}
        };
        for (i = 0; i < 8; i++) {
            corpse->pos[0] = medic->pos[0] + offsets[i][0];
            corpse->pos[2] = medic->pos[2] + offsets[i][1];
            if (q2_visible(medic, corpse)) break;
        }
        CHECK(i < 8);
        q2_link_entity(medic, 5);
        q2_link_entity(corpse, 5);
        for (i = 0; i < c->creatures.set.count; i++) {
            q2_monster *m = &c->creatures.set.monsters[i];
            q2_move_body *b = &c->cre_body[i];
            q2_actor_from_monster(&c->cre_actor[i], m);
            b->id = (s32)i; b->solid = m->in_use && !m->dead;
            memcpy(b->pos, m->pos, sizeof(b->pos));
            b->sweep_radius = m->hit_radius; b->height = m->hit_height;
        }
        q2_sim_set_bodies(&c->sim[0], c->cre_body, c->creatures.set.count);
        c->sim[0].body_world.list = c->cre_body;
        c->sim[0].body_world.count = c->creatures.set.count;
    }
    capture_creature(c, medic, output, "medic-before-revival");
    {
        q2_ai_trace tr; s32 to[3];
        memcpy(to, corpse->pos, sizeof(to)); to[1] += (572 - corpse->hit_height) / 2;
        q2_ai_get_world()->trace(q2_ai_get_world()->user, medic->pos, NULL, NULL, to,
                                 medic, 0x06000003u, &tr);
        printf("Medic setup %d,%d,%d corpse %d,%d,%d hp=%d/%d cylinder=%d/%d visible=%d trace=%d hit=%d\n",
               medic->pos[0],medic->pos[1],medic->pos[2],corpse->pos[0],corpse->pos[1],corpse->pos[2],
               corpse->health,corpse->gib_health,corpse->hit_radius,corpse->hit_height,
               q2_visible(medic,corpse),tr.fraction,tr.ent==corpse);
    }
    medic->enemy = &c->creatures.sight; c->creatures.sight.health = 100;
    medic->idle(medic);
    CHECK(medic->enemy == corpse && (medic->aiflags & Q2_AI_MEDIC));
    q2_cre_set_move(medic, 209); medic->frame = 219;
    medic->cre_bind->impl->method[10](medic);
    CHECK(corpse->effect[5] == 30 && (corpse->aiflags & Q2_AI_RESURRECTING));
    CHECK(c->cre_actor[ci].effect[5] == 30);
    capture_creature(c, medic, output, "medic-cable");
    medic->frame = 226; medic->cre_bind->impl->method[10](medic);
    CHECK(corpse->think == q2_cre_medic_revive);
    CHECK(client_capture(c));
    snprintf(path, sizeof(path), "%s/medic-revival.sav", output);
    CHECK(q2_save_write(&c->snapshot, path) == Q2_OK && q2_save_read(&saved, path) == Q2_OK);
    corpse->think = NULL;
    CHECK(q2_save_apply_ai(&saved, &c->creatures, &phase) == Q2_OK);
    CHECK(corpse->think == q2_cre_medic_revive && corpse->owner == medic);
    close_client(c); c = open_client(disc_path);
    CHECK(client_apply_save(c, &saved));
    medic = &c->creatures.set.monsters[mi]; corpse = &c->creatures.set.monsters[ci];
    CHECK(corpse->think == q2_cre_medic_revive && corpse->owner == medic);
    capture_creature(c, medic, output, "medic-restored-cable");
    /* The fixture's default player arrival is in zone 2. Carry a resident
     * point through this zone-3 rebuild so it tests actor resume, not fallback
     * placement for a gate fired remotely from its doorway. */
    memcpy(c->sim[0].player->ent.pos, medic->pos, sizeof(medic->pos));
    memcpy(c->sim[0].player->pos, medic->pos, sizeof(medic->pos));
    c->sim[0].player->pos[1] = q2_sim_feet_y(medic->pos[1]);
    c->carry_player = c->carry_same_map = true;
    CHECK(client_load_zone(c, "JAIL3", 3));
    medic = &c->creatures.set.monsters[mi]; corpse = &c->creatures.set.monsters[ci];
    CHECK(corpse->in_use && corpse->corpse && corpse->think == q2_cre_medic_revive &&
          corpse->owner == medic && medic->enemy == corpse);
    destroyed = q2_gib_counters.destroyed;
    {
        s32 time = q2_level_state.time;
        q2_creature_world_actor_tick(&c->creatures);
        CHECK(q2_level_state.time == time);
    }
    CHECK(corpse->in_use && !corpse->corpse && !corpse->dead && corpse->health == corpse->max_health &&
          corpse->hit_height == height && corpse->enemy == &c->creatures.sight);
    client_cre_drain_one(c, ci);
    CHECK(corpse->health == corpse->max_health && !c->cre_actor[ci].effect[5]);
    CHECK(q2_gib_counters.destroyed == destroyed + 1);
    capture_creature(c, medic, output, "medic-after-revival");
    q2_ai_checkattack(medic, 0);
    CHECK(medic->enemy == &c->creatures.sight && !(medic->aiflags & Q2_AI_MEDIC));
    medic->aiflags &= ~(u32)Q2_AI_MEDIC; medic->enemy = &c->creatures.sight;
    shots = c->cre_launched; q2_cre_set_move(medic, 191); medic->frame = 190;
    for (i = 0; i < 16; i++) q2_M_MoveFrame(medic);
    CHECK(c->cre_launched == shots + 12 && !c->cre_fire_no_figures);
    CHECK(c->pose_by_name && !c->pose_name_absent && !c->pose_no_name);
    printf("Medic fixture: class %u revived, unnamed sounds=%u missing sounds=%u\n",
           corpse->pop_class_id, c->cre_sound_unnamed, c->cre_sound_missing);
    q2_save_free(&saved); close_client(c);
}

int main(int argc, char **argv)
{
    test_fire_routes();
    test_melee_delivery();
    test_infighting();
    if (argc == 4 && (!strcmp(argv[1], "--headless-player-animation") || !strcmp(argv[1], "--headless")))
        test_disc_player_animation(argv[2], argv[3]);
    if (argc == 4 && (!strcmp(argv[1], "--headless-dry-attack") || !strcmp(argv[1], "--headless")))
        test_disc_dry_attack(argv[2], argv[3]);
    if (argc == 4 && (!strcmp(argv[1], "--headless-ambient") || !strcmp(argv[1], "--headless")))
        test_disc_ambient(argv[2], argv[3]);
    if (argc == 4 && (!strcmp(argv[1], "--headless") || !strcmp(argv[1], "--headless-new"))) {
        test_disc_jorg_rider(argv[2], argv[3]);
        test_disc_medic(argv[2], argv[3]);
    }
    if (argc == 4 && !strcmp(argv[1], "--headless")) {
        test_disc_motion(argv[2], argv[3]);
        test_disc_flyer(argv[2], argv[3]);
        test_disc_gladiator(argv[2], argv[3]);
        test_disc_hover(argv[2], argv[3]);
        test_disc_parasite(argv[2], argv[3]);
        test_disc_ironmaiden(argv[2], argv[3]);
        test_disc_strider(argv[2], argv[3]);
        test_disc_boss(argv[2], argv[3], false);
        test_disc_boss(argv[2], argv[3], true);
        test_disc_script(argv[2], argv[3], "BOSS1", 1, "RiderStand");
        test_disc_script(argv[2], argv[3], "BOSS2", 1, "Rider2");
        test_disc_script(argv[2], argv[3], "BOSS2", 1, "Rider3");
        test_disc_script(argv[2], argv[3], "LAB", 2, "DeadComm");
        test_disc_arachner_pitch(argv[2]);
    }
    printf("%d creature client checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
