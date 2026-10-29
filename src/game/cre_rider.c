/* Rider (Makron), BOSS2/ZONE1.DAT. The Jorg module contains a second
 * copy with its own method table, sounds, moves and sight callback. */
#include "ai.h"
#include "aimove.h"
#include "crebind.h"
#include "random.h"
#include <string.h>

static bool embedded(const q2_monster *m)
{
    const q2_cre_bind *b = q2_cre_bind_for(m);
    return b && b->impl && !strcmp(b->impl->name, "JorgRider");
}
static void sound(q2_monster *m, u32 offset)
{
    q2_cre_play_sound_owner(m, 0x80100000u + offset +
                             (embedded(m) ? 0x2060u : 0), m);
}
static void rider_stand(q2_monster *m) { q2_cre_set_move(m, 414); }
static void rider_walk(q2_monster *m)
{
    q2_cre_set_move_at(m, 0x80100000u +
                         (embedded(m) ? 0x4700 : 0x26DC));
}
static void rider_run(q2_monster *m)
{
    if (m->aiflags & Q2_AI_STAND_GROUND) rider_stand(m);
    else q2_cre_set_move_at(m, 0x80100000u +
                              (embedded(m) ? 0x46D0 : 0x26AC));
}
static void rider_sight(q2_monster *m, q2_monster *other)
{
    (void)other; q2_cre_set_move(m, 188);
}
static void rider_attack(q2_monster *m)
{
    s32 r;
    if (!m->enemy) return;
    r = q2_random_next();
    q2_cre_set_move(m, r < 9831 ? 201 : r < 19961 ? 209 : 235);
}

/* Jorg+0x2114 / Rider+0x17D8. Unlike M_CheckAttack, these do not scale
 * probability by difficulty. Rider's cooldown is 0..2 ticks, Jorg's 0..20. */
bool q2_cre_boss_checkattack(q2_monster *m, s32 cooldown)
{
    q2_range_band range;
    s32 delta[3], chance;
    int k;
    if (!m->enemy) return false;
    if (m->enemy->health > 0) {
        const q2_ai_world *w = q2_ai_get_world();
        q2_ai_trace tr;
        s32 from[3], to[3];
        for (k = 0; k < 3; k++) { from[k] = m->pos[k]; to[k] = m->enemy->pos[k]; }
        from[1] += m->view_height; to[1] += m->enemy->view_height;
        w->trace(w->user, from, NULL, NULL, to, m, 0x02000019u, &tr);
        if (tr.ent != m->enemy) return false;
    }
    range = q2_range(m, m->enemy);
    for (k = 0; k < 3; k++) delta[k] = m->enemy->pos[k] - m->pos[k];
    m->ideal_yaw = q2_vectoyaw(delta);
    if (range == Q2_RANGE_MELEE) {
        m->attack_state = m->melee ? Q2_AS_MELEE : Q2_AS_MISSILE;
        return true;
    }
    if (!m->attack || q2_level_state.time < m->attack_finished || range == Q2_RANGE_FAR)
        return false;
    chance = (m->aiflags & Q2_AI_STAND_GROUND) || range == Q2_RANGE_NEAR ? 13106 : 6553;
    if (q2_random_next() < chance) {
        m->attack_state = Q2_AS_MISSILE;
        m->attack_finished = q2_level_state.time + cooldown * q2_random_next() / 32767;
        return true;
    }
    if (m->flags & Q2_FL_FLY)
        m->attack_state = q2_random_next() < 9830 ? Q2_AS_SLIDING : Q2_AS_STRAIGHT;
    return false;
}
static bool rider_checkattack(q2_monster *m) { return q2_cre_boss_checkattack(m, 2); }

static void rider_bfg(q2_monster *m)
{
    q2_cre_shot s = { 0 };
    int k;
    if (!m->enemy) return;
    s.slot = Q2_IMP_FIRE_BFG; s.damage = 50; s.speed = 300; s.kick = 100;
    s.flash = 101; s.count = 1; s.aimed = true;
    q2_cre_muzzle(m, 431, 434, s.start);
    for (k = 0; k < 3; k++) s.dir[k] = m->enemy->pos[k] - s.start[k];
    s.dir[1] += m->enemy->view_height;
    q2_vector_normalize(s.dir);
    sound(m, 0x2A28);
    q2_cre_fire_shot(m, &s);
}
static void rider_save_target(q2_monster *m)
{
    if (!m->enemy) return;
    memcpy(m->blind_target, m->enemy->pos, sizeof(m->blind_target));
    m->blind_target[1] += m->enemy->view_height;
}
static void rider_rail(q2_monster *m)
{
    q2_cre_shot s = { 0 };
    int k;
    s.slot = Q2_IMP_FIRE_RAILGUN; s.damage = 50; s.kick = 100;
    s.flash = 119; s.count = 1; s.aimed = true;
    q2_cre_muzzle(m, 225, 227, s.start);
    for (k = 0; k < 3; k++) s.dir[k] = m->blind_target[k] - s.start[k];
    q2_vector_normalize(s.dir);
    q2_cre_fire_shot(m, &s);
    q2_cre_play_named_sound(m, "wep_railgf1a");
}
static void rider_blaster(q2_monster *m)
{
    q2_cre_shot s = { 0 };
    s16 angles[3] = { 0, 0, 0 };
    s32 delta[3], sweep;
    int k;
    s.slot = Q2_IMP_FIRE_BLASTER; s.damage = 15; s.speed = 1000;
    s.flash = 102; s.effects = 8; s.count = 1; s.aimed = true;
    q2_cre_muzzle(m, 419, 422, s.start);
    if (m->enemy) {
        for (k = 0; k < 3; k++) delta[k] = m->enemy->pos[k] - s.start[k];
        delta[1] += m->enemy->view_height;
        q2_vectoangles(delta, angles);
    }
    sweep = (m->frame < 222 ? m->frame - 217 : m->frame - 226) * 5 * 8192 / 360;
    angles[2] = (s16)(m->angles[2] + (m->frame < 222 ? sweep : -sweep));
    angles[1] = 0;
    q2_angle_vectors(angles, s.dir, NULL, NULL);
    q2_cre_fire_shot(m, &s);
}
static void rider_pain(q2_monster *m, s16 damage)
{
    if (m->health < m->max_health / 2) m->skinnum = 1;
    if (q2_level_state.time < m->pain_debounce) return;
    if (damage <= 25 && q2_random_next() < 6553) return;
    m->pain_debounce = q2_level_state.time + 30;
    if (q2_cre_skill() == 3) return;
    if (damage <= 40) { sound(m, 0x2A10); q2_cre_set_move(m, 379); }
    else if (damage <= 110) { sound(m, 0x2A14); q2_cre_set_move(m, 383); }
    else if (damage <= 150 && (q2_random_next() < 14746 || q2_random_next() < 11469)) {
        sound(m, 0x2A18); q2_cre_set_move(m, 387);
    }
}
static void rider_dead(q2_monster *m)
{
    m->next_think = 0; m->movetype = Q2_MOVETYPE_TOSS;
    m->svflags |= Q2_SVF_DEADMONSTER; m->takedamage = Q2_DAMAGE_NO;
    /* The native callback publishes the defeat position to the player. */
    if (q2_level_state.sight_client)
        memcpy(q2_level_state.sight_client->blind_target, m->pos, sizeof(m->pos));
}
static void rider_die(q2_monster *m, s16 damage)
{
    (void)damage;
    if (m->health <= m->gib_health) {
        m->dead = true; m->deadflag = Q2_DEAD_DEAD; rider_dead(m); return;
    }
    if (m->deadflag == Q2_DEAD_DEAD) return;
    sound(m, 0x2A1C);
    m->dead = true; m->deadflag = Q2_DEAD_DEAD; m->health = m->gib_health;
    rider_dead(m);
}
static void rider_step_left(q2_monster *m) { sound(m, 0x2A20); }
static void rider_step_right(q2_monster *m) { sound(m, 0x2A24); }
static void rider_popup(q2_monster *m) { sound(m, 0x2A34); }
static void rider_taunt(q2_monster *m) { sound(m, 0x2A40); }
static void rider_hit(q2_monster *m) { sound(m, 0x2A44); }
static void rider_brain(q2_monster *m) { sound(m, 0x2A2C); }
static void rider_rail_up(q2_monster *m) { sound(m, 0x2A30); }
static void rider_spawn(q2_monster *m)
{
    m->mass = 500; m->movetype = Q2_MOVETYPE_STEP; m->solid = Q2_SOLID_BBOX;
    q2_link_entity(m, 1); q2_link_entity(m, 0x81);
    if (embedded(m)) q2_cre_set_move(m, 188); else rider_stand(m);
    m->speed_scale = 10; q2_monster_walk_start(m);
}
/* Jorg+0x2FC0: the two-tick delayed second-stage activation. */
void q2_cre_rider_launch(q2_monster *m)
{
    s32 delta[3], zero[3] = { 0, 0, 0 };
    int k;
    m->render_scale = 4096;
    rider_spawn(m); q2_monster_start_go(m);
    if (embedded(m)) q2_cre_set_move(m, 188);
    if (!q2_level_state.sight_client) return;
    for (k = 0; k < 3; k++) delta[k] = q2_level_state.sight_client->pos[k] - m->pos[k];
    m->angles[2] = q2_vectoyaw(delta);
    q2_vector_normalize(delta);
    q2_vector_ma(zero, 400, delta, m->velocity);
    m->velocity[1] = 200; m->on_ground = false;
}
#define RIDER_METHODS { NULL, rider_rail, rider_save_target, rider_blaster, \
    rider_step_left, rider_step_right, rider_bfg, rider_dead, rider_popup, \
    rider_taunt, rider_run, rider_hit, rider_brain, rider_rail_up }
const q2_cre_impl q2_cre_rider = {
    "Rider", { rider_stand, NULL, NULL, rider_walk, rider_run, NULL,
    rider_attack, NULL, NULL, (q2_class_method)(void *)rider_checkattack, NULL,
    (q2_class_method)(void *)rider_pain, (q2_class_method)(void *)rider_die },
    RIDER_METHODS, rider_spawn
};
const q2_cre_impl q2_cre_jorg_rider = {
    "JorgRider", { rider_stand, NULL, NULL, rider_walk, rider_run, NULL,
    rider_attack, NULL, (q2_class_method)(void *)rider_sight,
    (q2_class_method)(void *)rider_checkattack, NULL,
    (q2_class_method)(void *)rider_pain, (q2_class_method)(void *)rider_die },
    RIDER_METHODS, rider_spawn
};
