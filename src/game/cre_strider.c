/* Strider, BIGGUN/ZONE3.DAT, relocated at 0x80100000. */
#include "ai.h"
#include "aimove.h"
#include "crebind.h"
#include "random.h"
#include "shock.h"

#include <string.h>

extern void (*q2_cre_sound_fn)(q2_monster *, int, void *);
extern void *q2_cre_sound_user;
static void strider_play(q2_monster *m, u32 address)
{
    if (q2_cre_sound_fn) q2_cre_sound_fn(m, (int)address, q2_cre_sound_user);
}
static void strider_stand(q2_monster *m)
{ q2_cre_set_move(m, q2_random_next() < 16384 ? 18 : 44); }
static void strider_walk(q2_monster *m) { q2_cre_set_move_at(m, 0x80102738u); }
static void strider_run(q2_monster *m)
{
    s32 first = m->currentmove ? m->currentmove->first_frame : -1;
    if (m->enemy && m->enemy->client) m->aiflags |= Q2_AI_BRUTAL;
    else m->aiflags &= ~Q2_AI_BRUTAL;
    if (m->aiflags & Q2_AI_STAND_GROUND) strider_stand(m);
    else if (first == 18 || first == 44 || first == 216) q2_cre_set_move(m, 70);
    else if (first == 170 || first == 151) q2_cre_set_move(m, 216);
    else q2_cre_set_move_at(m, 0x801027BCu);
}
static bool strider_can_attack(q2_monster *m, bool aligned)
{
    s32 delta[3], yaw, current, turn;
    int k;
    if (!m->enemy || !q2_infront(m, m->enemy)) return false;
    for (k = 0; k < 3; k++) delta[k] = m->enemy->pos[k] - m->pos[k];
    current = q2_anglemod(m->angles[2]); yaw = q2_vectoyaw(delta);
    if (yaw == current) return aligned && q2_visible(m, m->enemy);
    turn = yaw - current;
    if (current < yaw) { if (turn >= 2048) turn -= 4096; }
    else if (turn < -2047) turn += 4096;
    if (turn < 0) turn = -turn;
    /* Exact alignment skips the s2 assignment at +0x12C0. The callers
     * supply its inherited value rather than inventing an inside-cone result. */
    return turn <= m->yaw_speed && q2_visible(m, m->enemy);
}
static void strider_repeat_blaster(q2_monster *m)
{
    q2_cre_set_move(m, m->enemy && q2_range(m, m->enemy) != Q2_RANGE_MELEE &&
                      strider_can_attack(m, false) ? 140 : 145);
}
static void strider_repeat_laser(q2_monster *m)
{
    q2_cre_set_move(m, strider_can_attack(m, false) &&
                      q2_range(m, m->enemy) == Q2_RANGE_MID ? 195 : 199);
}
static void strider_target(q2_monster *m)
{
    if (m->enemy) memcpy(m->blind_target, m->enemy->pos, sizeof(m->blind_target));
}
static void strider_fire(q2_monster *m, bool laser)
{
    q2_cre_shot s = { 0 };
    int k;
    s.slot = laser ? Q2_IMP_FIRE_LASER : Q2_IMP_FIRE_BLASTER;
    s.damage = laser ? 50 : 25; s.count = 1; s.aimed = true;
    if (laser) { s.kick = 25; q2_cre_muzzle(m, 456, 457, s.start); }
    else { s.speed = 100; s.effects = 64; q2_cre_muzzle(m, 145, 158, s.start); }
    for (k = 0; k < 3; k++) s.dir[k] = m->blind_target[k] - s.start[k];
    q2_vector_normalize(s.dir);
    if (!laser) for (k = 0; k < 3; k++) s.dir[k] /= 2;
    q2_cre_fire_shot(m, &s);
}
static void strider_blaster(q2_monster *m) { strider_fire(m, false); }
static void strider_laser(q2_monster *m) { strider_fire(m, true); }
static void strider_melee(q2_monster *m)
{
    static const s32 aim[3] = { 1020, -48, 0 };
    if (m->enemy) q2_cre_fire_hit(m, aim, 50 + q2_random_next() % 50, 200);
}
static void strider_stomp(q2_monster *m)
{
    static const s32 aim[3] = { 2040, -48, 0 };
    if (m->enemy) q2_cre_fire_hit(m, aim, 100 + q2_random_next() % 50, 500);
}
static void strider_step(q2_monster *m)
{
    q2_shock_request(Q2_SHOCK_ALL_PLAYERS, Q2_SHOCK_STOMP, 4096,
                      m->pos, 2048, 8192);
    strider_play(m, 0x80102BD0u);
}
static void strider_run_step(q2_monster *m) { strider_melee(m); strider_step(m); }
static void strider_explode(q2_monster *m)
{
    q2_cre_bind *bind = q2_cre_bind_for(m);
    s32 p[3];
    if (!bind || (++bind->strider_explosion_phase & 1)) return;
    p[0] = m->pos[0] + (q2_random_next() - 16384) / 32;
    p[1] = m->pos[1] + (q2_random_next() - 16384) / 64 - 60;
    p[2] = m->pos[2] + (q2_random_next() - 16384) / 32;
    q2_cre_explosion(m, p, 4096);
}
static void strider_finish_turn(q2_monster *m)
{
    q2_cre_bind *bind = q2_cre_bind_for(m);
    if (bind && (bind->strider_turn == 1 || bind->strider_turn == 2)) {
        m->angles[2] = (s16)q2_anglemod(m->angles[2] +
                                         (bind->strider_turn == 1 ? -512 : 512));
        bind->strider_turn = 0;
        q2_link_entity(m, 0x84);
    }
}
static void strider_melee_finish_turn(q2_monster *m)
{ strider_melee(m); strider_finish_turn(m); }
static void strider_right(q2_monster *m)
{ m->angles[2] = (s16)q2_anglemod(m->angles[2] + 93); q2_link_entity(m, 4); }
static void strider_left(q2_monster *m)
{ m->angles[2] = (s16)q2_anglemod(m->angles[2] - 93); q2_link_entity(m, 4); }
static void strider_empty(q2_monster *m) { (void)m; }
static void strider_bigturn(q2_monster *m, s32 turn)
{
    if (m->enemy && q2_visible(m, m->enemy)) {
        if (turn == m->yaw_speed) { q2_cre_set_move(m, 170); return; }
        if (turn == -m->yaw_speed) { q2_cre_set_move(m, 151); return; }
    }
    m->angles[2] = (s16)q2_anglemod(q2_anglemod(m->angles[2]) + turn);
    q2_link_entity(m, 4);
}
static void strider_idle(q2_monster *m) { strider_play(m, 0x80102BC8u); }
static void strider_sight(q2_monster *m, q2_monster *other)
{ (void)other; strider_play(m, 0x80102BD4u); }
static void strider_attack(q2_monster *m)
{
    int range;
    if (!strider_can_attack(m, q2_ai_native_s2 != 0)) return;
    if (m->health < 0) m->aiflags &= ~Q2_AI_BRUTAL;
    range = q2_range(m, m->enemy);
    if (range == Q2_RANGE_MELEE) q2_cre_set_move(m, 206);
    else if (range == Q2_RANGE_NEAR || q2_random_next() < 16384) q2_cre_set_move(m, 137);
    else q2_cre_set_move(m, 189);
}
static void strider_pain(q2_monster *m, s16 damage)
{
    if (m->health < m->max_health / 2) m->skinnum |= 1;
    if (damage <= 10 || q2_level_state.time < m->pain_debounce) return;
    if (damage <= 30 && q2_random_next() >= 6554) return;
    if (q2_cre_skill() >= 2 && ((m->frame >= 137 && m->frame <= 147) ||
                               (m->frame >= 189 && m->frame <= 201))) return;
    m->pain_debounce = q2_level_state.time + 30;
    strider_play(m, 0x80102BC4u);
    if (q2_cre_skill() != 3) q2_cre_set_move(m, damage <= 30 ? 95 : 103);
}
static void strider_dead(q2_monster *m)
{
    m->health = m->gib_health; m->next_think = 0;
    m->movetype = Q2_MOVETYPE_TOSS; m->svflags |= Q2_SVF_DEADMONSTER;
    m->takedamage = Q2_DAMAGE_NO;
}
static void strider_die(q2_monster *m, s16 damage)
{
    (void)damage;
    if (m->health <= m->gib_health) {
        strider_play(m, 0x80102BE0u);
        m->dead = true; m->deadflag = Q2_DEAD_DEAD; strider_dead(m); return;
    }
    if (m->deadflag == Q2_DEAD_DEAD) return;
    strider_play(m, 0x80102BCCu);
    m->dead = true; m->deadflag = Q2_DEAD_DEAD;
    m->takedamage = Q2_DAMAGE_YES; q2_cre_set_move(m, 111);
}
static void strider_spawn(q2_monster *m)
{
    m->mass = 500; m->movetype = Q2_MOVETYPE_STEP; m->solid = Q2_SOLID_BBOX;
    m->bigturn_threshold = 1024;
    q2_link_entity(m, 5); q2_link_entity(m, 0x85);
    strider_stand(m); m->speed_scale = 10; q2_monster_walk_start(m);
}
const q2_cre_impl q2_cre_strider = {
    "Strider",
    { strider_stand, strider_idle, NULL, strider_walk, strider_run, NULL,
      strider_attack, NULL, (q2_class_method)(void *)strider_sight, NULL,
      (q2_class_method)(void *)strider_bigturn, (q2_class_method)(void *)strider_pain,
      (q2_class_method)(void *)strider_die },
    { NULL, strider_repeat_blaster, strider_repeat_laser, strider_walk, strider_run,
      strider_stand, strider_target, strider_blaster, strider_laser, strider_dead,
      strider_melee, strider_stomp, strider_step, strider_run_step, strider_explode,
      strider_finish_turn, strider_melee_finish_turn, strider_right, strider_left,
      strider_empty },
    strider_spawn
};
