/* Boss2 (the flying boss), COMMAND/ZONE4.DAT, base 0x80100000. */
#include "ai.h"
#include "aimove.h"
#include "crebind.h"
#include "random.h"

static void boss2_empty(q2_monster *m) { (void)m; }
static void boss2_stand(q2_monster *m) { q2_cre_set_move(m, 0); }
static void boss2_walk(q2_monster *m) { q2_cre_set_move_at(m, 0x801019E8u); }
static void boss2_run(q2_monster *m)
{
    if (m->aiflags & Q2_AI_STAND_GROUND) boss2_stand(m);
    else q2_cre_set_move_at(m, 0x80101A34u);
}
static void boss2_start_fire(q2_monster *m) { q2_cre_set_move(m, 79); }
static void boss2_repeat(q2_monster *m)
{
    q2_cre_set_move(m, m->enemy && q2_infront(m, m->enemy) &&
                      q2_random_next() < 22937 ? 79 : 85);
}
static void boss2_attack(q2_monster *m)
{
    s32 delta[3];
    int k;
    if (!m->enemy) return;
    for (k = 0; k < 3; k++) delta[k] = m->enemy->pos[k] - m->pos[k];
    q2_cre_set_move(m, q2_vector_length_sq(delta) <= 2250000 ||
                      q2_random_next() < 19961 ? 70 : 89);
}
static bool boss2_checkattack(q2_monster *m)
{
    s32 delta[3], from[3], to[3], chance;
    q2_range_band range;
    int k;
    if (!m->enemy) return false;
    if (m->enemy->health > 0) {
        const q2_ai_world *w = q2_ai_get_world();
        q2_ai_trace tr;
        for (k = 0; k < 3; k++) { from[k] = m->pos[k]; to[k] = m->enemy->pos[k]; }
        from[1] += m->view_height; to[1] += m->enemy->view_height;
        w->trace(w->user, from, NULL, NULL, to, m, 0x02000019u, &tr);
        if (tr.ent != m->enemy) return false;
    }
    /* The infront call's result is discarded by the module. */
    range = q2_range(m, m->enemy);
    for (k = 0; k < 3; k++) delta[k] = m->enemy->pos[k] - m->pos[k];
    m->ideal_yaw = q2_vectoyaw(delta);
    if (range == Q2_RANGE_MELEE) {
        m->attack_state = m->melee ? Q2_AS_MELEE : Q2_AS_MISSILE;
        return true;
    }
    if (!m->attack || q2_level_state.time < m->attack_finished || range == Q2_RANGE_FAR)
        return false;
    chance = (m->aiflags & Q2_AI_STAND_GROUND) ? 13106 : 26213;
    if (q2_random_next() < chance) {
        m->attack_state = Q2_AS_MISSILE;
        m->attack_finished = q2_level_state.time + 20 * q2_random_next() / 32767;
        return true;
    }
    if (m->flags & Q2_FL_FLY)
        m->attack_state = q2_random_next() < 9830 ? Q2_AS_SLIDING : Q2_AS_STRAIGHT;
    return false;
}
static void boss2_bullet(q2_monster *m, s16 a, s16 b, s32 flash)
{
    q2_cre_shot s = { 0 };
    s32 target[3];
    int k;
    if (!m->enemy) return;
    s.slot = Q2_IMP_FIRE_BULLET; s.damage = 6; s.kick = 4;
    s.hspread = 300; s.vspread = 500; s.flash = flash; s.count = 1; s.aimed = true;
    q2_cre_muzzle(m, a, b, s.start);
    q2_vector_ma(m->enemy->pos, -2, m->enemy->velocity, target);
    target[1] += m->enemy->view_height;
    for (k = 0; k < 3; k++) s.dir[k] = target[k] - s.start[k];
    q2_vector_normalize(s.dir); q2_cre_fire_shot(m, &s);
}
static void boss2_machineguns(q2_monster *m)
{
    boss2_bullet(m, 440, 444, 73);
    boss2_bullet(m, 339, 344, 133);
}
static void boss2_rockets(q2_monster *m)
{
    q2_cre_attach a, b, tube;
    bool attachments;
    int i, k;
    if (!m->enemy) return;
    attachments = q2_cre_attach_blend(m, 237, 239, &a) &&
                  q2_cre_attach_blend(m, 238, 240, &b);
    for (i = 0; i < 4; i++) {
        q2_cre_shot s = { 0 };
        if (attachments) {
            for (k = 0; k < 3; k++) {
                s16 step = (s16)(((s32)b.off[k] - a.off[k]) * 8);
                tube.off[k] = (s16)(a.off[k] + (step / 2 + i * step) / 32);
            }
            tube.part = a.part; q2_cre_attach_place(m, &tube, s.start);
        } else q2_cre_attach_place(m, NULL, s.start);
        s.slot = Q2_IMP_FIRE_ROCKET; s.damage = 50; s.speed = 500;
        s.count = 1; s.aimed = true;
        for (k = 0; k < 3; k++) s.dir[k] = m->enemy->pos[k] - s.start[k];
        s.dir[1] += m->enemy->view_height;
        q2_vector_normalize(s.dir); q2_cre_fire_shot(m, &s);
    }
}
static void boss2_activate(q2_monster *m)
{
    s32 delta[3];
    int k;
    if (m->health != 2000) return;
    m->pos[1] -= 1536; q2_link_entity(m, 0x81); m->health--;
    if (!q2_level_state.sight_entity) return;
    for (k = 0; k < 3; k++) delta[k] = q2_level_state.sight_entity->pos[k] - m->pos[k];
    m->angles[2] = q2_vectoyaw(delta);
    q2_link_entity(m, 4); q2_link_entity(m, 0x84);
}
static void boss2_pain(q2_monster *m, s16 damage)
{
    if (m->health < m->max_health / 2) m->skinnum = 1;
    if (q2_level_state.time < m->pain_debounce) return;
    m->pain_debounce = q2_level_state.time + 30;
    q2_cre_set_move(m, damage < 30 ? 128 : 110);
    /* No skill gate and no sound call in the native callback. */
}
static void boss2_dead(q2_monster *m)
{
    m->health = m->gib_health; m->next_think = 0;
    m->movetype = Q2_MOVETYPE_TOSS; m->svflags |= Q2_SVF_DEADMONSTER;
    m->takedamage = Q2_DAMAGE_NO;
}
static void boss2_die(q2_monster *m, s16 damage)
{
    (void)damage;
    q2_cre_set_move(m, 132); m->dead = true; m->deadflag = Q2_DEAD_DEAD;
    boss2_dead(m); /* +0x14D4 immediately does the end-of-death work. */
}
static void boss2_spawn(q2_monster *m)
{
    m->mass = 1000; m->movetype = Q2_MOVETYPE_STEP; m->solid = Q2_SOLID_BBOX;
    m->flags |= Q2_FL_IMMUNE_LASER;
    q2_link_entity(m, 1); q2_link_entity(m, 0x81);
    boss2_stand(m); m->speed_scale = 10; q2_monster_fly_start(m);
}
const q2_cre_impl q2_cre_boss2 = {
    "Boss2",
    { boss2_stand, NULL, boss2_empty, boss2_walk, boss2_run, NULL,
      boss2_attack, NULL, NULL, (q2_class_method)(void *)boss2_checkattack, NULL,
      (q2_class_method)(void *)boss2_pain, (q2_class_method)(void *)boss2_die },
    { NULL, NULL, boss2_run, boss2_stand, boss2_dead, boss2_attack,
      boss2_start_fire, boss2_repeat, NULL, boss2_machineguns, boss2_rockets,
      boss2_activate },
    boss2_spawn
};
