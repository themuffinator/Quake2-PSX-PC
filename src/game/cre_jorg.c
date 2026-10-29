/* Jorg, BOSS2/ZONE1.DAT. Addresses refer to the relocated PAL module. */
#include "ai.h"
#include "aimove.h"
#include "crebind.h"
#include "random.h"
#include "shock.h"

static void sound(q2_monster *m, u32 offset)
{ q2_cre_play_sound_owner(m, 0x80100000u + offset, m); }
static void jorg_stand(q2_monster *m) { q2_cre_set_move(m, 112); }
static void jorg_walk(q2_monster *m)
{ q2_cre_set_move_at(m, 0x801043DCu); }
static void jorg_run(q2_monster *m)
{
    if (m->aiflags & Q2_AI_STAND_GROUND) jorg_stand(m);
    else q2_cre_set_move_at(m, 0x80104380u);
}
static void jorg_search(q2_monster *m) { sound(m, 0x4A4C); }
static void jorg_idle(q2_monster *m) { sound(m, 0x4A44); }
static bool jorg_checkattack(q2_monster *m) { return q2_cre_boss_checkattack(m, 20); }
static void jorg_attack(q2_monster *m)
{
    if (q2_random_next() < 24576) q2_cre_set_move(m, 0);
    else { sound(m, 0x4A5C); q2_cre_set_move(m, 18); }
}
static void jorg_start_fire(q2_monster *m) { q2_cre_set_move(m, 8); }
static void jorg_repeat(q2_monster *m)
{
    q2_cre_set_move(m, m->enemy && q2_visible(m, m->enemy) &&
                      q2_random_next() < 29490 ? 8 : 14);
}
static void jorg_bullet(q2_monster *m, s16 a, s16 b)
{
    q2_cre_shot s = { 0 };
    s32 target[3];
    int k;
    if (!m->enemy) return;
    s.slot = Q2_IMP_FIRE_BULLET; s.damage = 6; s.kick = 4;
    s.hspread = 300; s.vspread = 500; s.count = 1; s.aimed = true;
    q2_cre_muzzle(m, a, b, s.start);
    q2_vector_ma(m->enemy->pos, -2, m->enemy->velocity, target);
    target[1] += m->enemy->view_height;
    for (k = 0; k < 3; k++) s.dir[k] = target[k] - s.start[k];
    q2_vector_normalize(s.dir); q2_cre_fire_shot(m, &s);
}
static void jorg_machineguns(q2_monster *m)
{ jorg_bullet(m, 419, 422); jorg_bullet(m, 226, 228); }
static void jorg_bfg(q2_monster *m)
{
    q2_cre_shot s = { 0 };
    int k;
    if (!m->enemy) return;
    s.slot = Q2_IMP_FIRE_BFG; s.damage = 50; s.speed = 300; s.kick = 100;
    s.flash = 132; s.count = 1; s.aimed = true;
    q2_cre_muzzle(m, 165, 161, s.start);
    for (k = 0; k < 3; k++) s.dir[k] = m->enemy->pos[k] - s.start[k];
    s.dir[1] += m->enemy->view_height;
    q2_vector_normalize(s.dir); sound(m, 0x4A5C); q2_cre_fire_shot(m, &s);
}
static void jorg_pain(q2_monster *m, s16 damage)
{
    if (m->health < m->max_health / 2) m->skinnum = 1;
    if (q2_level_state.time < m->pain_debounce) return;
    if (damage <= 40 && q2_random_next() < 19961) return;
    if ((u16)m->frame < 8 && q2_random_next() < 164) return;
    if ((u16)(m->frame - 8) < 6 && q2_random_next() < 3) return;
    if ((u16)(m->frame - 18) < 8 && q2_random_next() < 164) return;
    m->pain_debounce = q2_level_state.time + 30;
    if (q2_cre_skill() == 3) return;
    if (damage <= 50) { sound(m, 0x4A38); q2_cre_set_move(m, 81); }
    else if (damage <= 100) { sound(m, 0x4A3C); q2_cre_set_move(m, 84); }
    else if (q2_random_next() < 9831) { sound(m, 0x4A40); q2_cre_set_move(m, 87); }
}
static void jorg_step(q2_monster *m, u32 offset)
{
    q2_shock_request(Q2_SHOCK_ALL_PLAYERS, Q2_SHOCK_STOMP, 4096, m->pos, 2048, 8192);
    sound(m, offset);
}
static void jorg_left(q2_monster *m) { jorg_step(m, 0x4A64); }
static void jorg_right(q2_monster *m) { jorg_step(m, 0x4A68); }
static void jorg_hit(q2_monster *m) { sound(m, 0x4A6C); }
static void jorg_explode(q2_monster *m)
{
    q2_cre_bind *b = q2_cre_bind_for(m);
    s32 point[3];
    if (!b || (++b->jorg_explosion_phase & 1)) return;
    point[0] = m->pos[0] + (q2_random_next() - 16384) / 32;
    point[1] = m->pos[1] + (q2_random_next() - 16384) / 64 - 60;
    point[2] = m->pos[2] + (q2_random_next() - 16384) / 32;
    q2_cre_explosion(m, point, 4096);
}
static void jorg_toss_rider(q2_monster *m)
{
    q2_monster *rider = q2_cre_spawn_child(m, 14);
    if (!rider) return;
    rider->target = m->target;
    rider->think = q2_cre_rider_launch;
    rider->next_think = q2_level_state.time + 2;
    rider->render_scale = 0;
}
static void jorg_dead(q2_monster *m)
{
    m->health = m->gib_health; m->next_think = 0;
    m->movetype = Q2_MOVETYPE_TOSS; m->svflags |= Q2_SVF_DEADMONSTER;
    m->takedamage = Q2_DAMAGE_NO;
}
static void jorg_die(q2_monster *m, s16 damage)
{
    (void)damage;
    sound(m, 0x4A48); q2_cre_set_move(m, 31);
    m->dead = true; m->deadflag = Q2_DEAD_DEAD; m->takedamage = Q2_DAMAGE_NO;
}
static void jorg_spawn(q2_monster *m)
{
    m->mass = 1000; m->movetype = Q2_MOVETYPE_STEP; m->solid = Q2_SOLID_BBOX;
    q2_link_entity(m, 1); q2_link_entity(m, 0x81);
    jorg_stand(m); m->speed_scale = 10; q2_monster_walk_start(m);
}
const q2_cre_impl q2_cre_jorg = {
    "Jorg", { jorg_stand, NULL, jorg_search, jorg_walk, jorg_run, NULL,
    jorg_attack, NULL, NULL, (q2_class_method)(void *)jorg_checkattack, NULL,
    (q2_class_method)(void *)jorg_pain, (q2_class_method)(void *)jorg_die },
    { NULL, NULL, jorg_toss_rider, jorg_dead, jorg_bfg, NULL,
      jorg_machineguns, jorg_repeat, jorg_start_fire, jorg_idle,
      jorg_left, jorg_right, jorg_hit, jorg_run, jorg_explode }, jorg_spawn
};
