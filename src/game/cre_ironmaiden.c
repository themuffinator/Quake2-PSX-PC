/* Ironmaiden, BOSS1/ZONE0.DAT, relocated at 0x80100000. */
#include "ai.h"
#include "aimove.h"
#include "crebind.h"
#include "random.h"

extern void (*q2_cre_sound_fn)(q2_monster *, int, void *);
extern void *q2_cre_sound_user;
static void maiden_play(q2_monster *m, u32 address)
{
    if (q2_cre_sound_fn) q2_cre_sound_fn(m, (int)address, q2_cre_sound_user);
}
static void maiden_stand(q2_monster *m) { q2_cre_set_move(m, 121); }
static void maiden_walk(q2_monster *m) { q2_cre_set_move_at(m, 0x801017C0u); }
static void maiden_run(q2_monster *m)
{
    if (m->aiflags & Q2_AI_STAND_GROUND) maiden_stand(m);
    else if (m->currentmove == q2_cre_find_move_at(m, 0x801017C0u) ||
             m->currentmove == q2_cre_find_move(m, 181))
        q2_cre_set_move_at(m, 0x80101790u);
    else q2_cre_set_move(m, 181);
}
static void maiden_attack(q2_monster *m) { q2_cre_set_move(m, 0); }
static void maiden_fire_start(q2_monster *m) { q2_cre_set_move(m, 13); }
static void maiden_melee(q2_monster *m) { q2_cre_set_move(m, 32); }
static void maiden_slash_start(q2_monster *m) { q2_cre_set_move(m, 35); }
static void maiden_reslash(q2_monster *m)
{
    q2_cre_set_move(m, m->enemy && m->enemy->health > 0 &&
                      q2_range(m, m->enemy) == Q2_RANGE_MELEE &&
                      q2_random_next() < 29491 ? 35 : 44);
}
static void maiden_refire(q2_monster *m)
{
    /* +0xEC4 is 19961, not the usual 60% threshold 19661. */
    q2_cre_set_move(m, m->enemy && m->enemy->health > 0 &&
                      q2_range(m, m->enemy) > Q2_RANGE_MELEE &&
                      q2_visible(m, m->enemy) && q2_random_next() < 19961 ? 13 : 27);
}
static void maiden_moan(q2_monster *m) { maiden_play(m, 0x80101B84u); }
static void maiden_fidget(q2_monster *m)
{
    if (!(m->aiflags & Q2_AI_STAND_GROUND) && q2_random_next() < 9831)
        q2_cre_set_move(m, 151);
}
static void maiden_dead(q2_monster *m)
{
    m->next_think = 0;
    m->movetype = Q2_MOVETYPE_TOSS;
    m->svflags |= Q2_SVF_DEADMONSTER;
}
static void maiden_duck_down(q2_monster *m)
{
    if (m->aiflags & Q2_AI_DUCKED) return;
    m->aiflags |= Q2_AI_DUCKED;
    m->takedamage = Q2_DAMAGE_YES;
    m->pausetime = q2_level_state.time + 10;
}
static void maiden_duck_hold(q2_monster *m)
{
    if (q2_level_state.time < m->pausetime) m->aiflags |= Q2_AI_HOLD_FRAME;
    else m->aiflags &= ~Q2_AI_HOLD_FRAME;
}
static void maiden_duck_up(q2_monster *m)
{
    m->takedamage = Q2_DAMAGE_AIM;
    m->aiflags &= ~Q2_AI_DUCKED;
}
static void maiden_dodge(q2_monster *m, q2_monster *attacker, s32 eta)
{
    (void)eta;
    if (q2_random_next() >= 8192) return;
    if (!m->enemy) m->enemy = attacker;
    q2_cre_set_move(m, 83);
}
static void maiden_launch_sound(q2_monster *m) { maiden_play(m, 0x80101B60u); }
static void maiden_reload(q2_monster *m) { (void)m; } /* +0x10D4: empty */
static void maiden_sight(q2_monster *m, q2_monster *other)
{
    (void)other; maiden_play(m, 0x80101B94u);
}
static void maiden_rocket(q2_monster *m)
{
    q2_cre_shot shot = { 0 };
    int k;
    shot.slot = Q2_IMP_FIRE_ROCKET;
    shot.damage = 50; shot.speed = 500; shot.count = 1; shot.aimed = true;
    q2_cre_muzzle(m, 233, 240, shot.start);
    if (m->enemy) {
        for (k = 0; k < 3; k++) shot.dir[k] = m->enemy->pos[k] - shot.start[k];
        shot.dir[1] += m->enemy->view_height;
        q2_vector_normalize(shot.dir);
    } else q2_angle_vectors(m->angles, shot.dir, NULL, NULL);
    q2_cre_fire_shot(m, &shot);
}
static void maiden_slash(q2_monster *m)
{
    const s32 aim[3] = { 1020, -120, -(s32)m->mins[0] };
    q2_cre_fire_hit(m, aim, 10 + q2_random_next() % 6, 100);
    maiden_play(m, 0x80101B68u);
}
static void maiden_pain(q2_monster *m, s16 damage)
{
    if (m->health < m->max_health / 2) m->skinnum = 1;
    if (q2_level_state.time < m->pain_debounce) return;
    m->pain_debounce = q2_level_state.time + 30;
    /* +0x9C0..0x9D0 stores the same handle at 1B90, 1B8C and 1B88. */
    maiden_play(m, 0x80101B90u);
    if (q2_cre_skill() == 3) return;
    q2_cre_set_move(m, damage <= 10 ? 90 : damage <= 25 ? 95 : 100);
}
static void maiden_die(q2_monster *m, s16 damage)
{
    (void)damage;
    if (m->health <= m->gib_health) {
        maiden_play(m, 0x80101B9Cu);
        m->dead = true; m->deadflag = Q2_DEAD_DEAD;
        maiden_dead(m); return;
    }
    if (m->deadflag == Q2_DEAD_DEAD) return;
    m->dead = true; m->deadflag = Q2_DEAD_DEAD;
    m->takedamage = Q2_DAMAGE_YES;
    q2_cre_set_move(m, (q2_random_next() & 1) ? 60 : 48);
    /* +0x878/+0x880 aliases the death handles. */
    maiden_play(m, 0x80101B78u);
}
static void maiden_spawn(q2_monster *m)
{
    m->mass = 200; m->movetype = Q2_MOVETYPE_STEP; m->solid = Q2_SOLID_BBOX;
    q2_link_entity(m, 1); q2_link_entity(m, 0x81);
    maiden_stand(m); m->speed_scale = 10; q2_monster_walk_start(m);
}
const q2_cre_impl q2_cre_ironmaiden = {
    "Ironmaiden",
    { maiden_stand, NULL, NULL, maiden_walk, maiden_run,
      (q2_class_method)(void *)maiden_dodge, maiden_attack, maiden_melee,
      (q2_class_method)(void *)maiden_sight, NULL, NULL,
      (q2_class_method)(void *)maiden_pain, (q2_class_method)(void *)maiden_die },
    { NULL, maiden_stand, maiden_run, maiden_reslash, maiden_refire,
      maiden_fire_start, maiden_moan, maiden_fidget, maiden_dead,
      maiden_duck_down, maiden_duck_hold, maiden_duck_up, maiden_launch_sound,
      maiden_rocket, maiden_reload, maiden_slash_start, maiden_slash },
    maiden_spawn
};
