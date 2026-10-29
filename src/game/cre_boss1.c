/* Supertank, SECURITY/ZONE1.DAT Boss1, relocated at 0x80100000. */
#include "ai.h"
#include "aimove.h"
#include "crebind.h"
#include "random.h"

static void boss1_empty(q2_monster *m) { (void)m; }
static void boss1_stand(q2_monster *m) { q2_cre_set_move(m, 194); }
static void boss1_walk(q2_monster *m) { q2_cre_set_move_at(m, 0x801014B8u); }
static void boss1_run(q2_monster *m)
{
    if (m->aiflags & Q2_AI_STAND_GROUND) boss1_stand(m);
    else q2_cre_set_move_at(m, 0x80101470u);
}
static void boss1_repeat(q2_monster *m)
{
    q2_cre_set_move(m, m->enemy && q2_visible(m, m->enemy) &&
                      q2_random_next() < 29490 ? 0 : 6);
}
static void boss1_rocket(q2_monster *m)
{
    q2_cre_attach a, b, tube;
    q2_cre_shot s = { 0 };
    int k, index = m->frame == 27 ? 0 : m->frame == 30 ? 1 : 2;
    if (!m->enemy) return; /* The native callback requires an enemy. */
    if (q2_cre_attach_blend(m, 564, 571, &a) &&
        q2_cre_attach_blend(m, 570, 572, &b)) {
        for (k = 0; k < 3; k++) {
            s16 step = (s16)(((s32)b.off[k] - a.off[k]) * 32 / 3);
            tube.off[k] = (s16)(a.off[k] + (step / 2 + index * step) / 32);
        }
        tube.part = a.part;
        q2_cre_attach_place(m, &tube, s.start);
    } else q2_cre_attach_place(m, NULL, s.start);
    s.slot = Q2_IMP_FIRE_ROCKET; s.damage = 50; s.speed = 500;
    s.count = 1; s.aimed = true;
    for (k = 0; k < 3; k++) s.dir[k] = m->enemy->pos[k] - s.start[k];
    s.dir[1] += m->enemy->view_height;
    q2_vector_normalize(s.dir); q2_cre_fire_shot(m, &s);
}
static void boss1_machinegun(q2_monster *m)
{
    q2_cre_shot s = { 0 };
    int k;
    s.slot = Q2_IMP_FIRE_BULLET; s.damage = 6; s.kick = 4;
    s.hspread = 300; s.vspread = 500; s.count = 1; s.aimed = true;
    q2_cre_muzzle(m, 412, 419, s.start);
    if (m->enemy) {
        for (k = 0; k < 3; k++) s.dir[k] = m->enemy->pos[k] - s.start[k];
        s.dir[1] += m->enemy->view_height;
        q2_vector_normalize(s.dir);
    } else {
        /* +0xB4C skips all three sp+0x48 aim stores; +0xBF0 passes that stale
         * stack directly to fire_bullet. Native-instruction tests demonstrate
         * different directions for identical actors with different stack
         * history. This explicit forward fallback is not native parity. */
        s16 angles[3] = { 0, 0, m->angles[2] };
        q2_angle_vectors(angles, s.dir, NULL, NULL);
    }
    q2_cre_fire_shot(m, &s);
}
static void boss1_attack(q2_monster *m)
{
    s32 delta[3];
    int k;
    if (!m->enemy) return;
    for (k = 0; k < 3; k++) delta[k] = m->enemy->pos[k] - m->pos[k];
    q2_cre_set_move(m, q2_vector_length_sq(delta) <= 3686400 ||
                      q2_random_next() < 9830 ? 0 : 20);
}
static void boss1_pain(q2_monster *m, s16 damage)
{
    if (m->health < m->max_health / 2) m->skinnum = 1;
    if (q2_level_state.time < m->pain_debounce) return;
    if (damage <= 25 && q2_random_next() < 6553) return;
    if (q2_cre_skill() >= 2 && m->frame >= 20 && m->frame <= 33) return;
    m->pain_debounce = q2_level_state.time + 30;
    if (q2_cre_skill() != 3)
        q2_cre_set_move(m, damage <= 10 ? 164 : damage <= 25 ? 168 : 172);
    /* The disc registers pain sounds but never plays them here. */
}
static void boss1_explode(q2_monster *m)
{
    q2_cre_bind *b = q2_cre_bind_for(m);
    s32 point[3];
    if (!b || (++b->boss1_explosion_phase & 1)) return;
    point[0] = m->pos[0] + (q2_random_next() - 16384) / 32;
    point[1] = m->pos[1] + (q2_random_next() - 16384) / 64 - 60;
    point[2] = m->pos[2] + (q2_random_next() - 16384) / 32;
    q2_cre_explosion(m, point, 4096);
}
static void boss1_dead(q2_monster *m)
{
    m->health = m->gib_health; m->next_think = 0;
    m->movetype = Q2_MOVETYPE_TOSS; m->svflags |= Q2_SVF_DEADMONSTER;
    m->takedamage = Q2_DAMAGE_NO;
}
static void boss1_die(q2_monster *m, s16 damage)
{
    (void)damage;
    q2_cre_set_move(m, 300); m->dead = true; m->deadflag = Q2_DEAD_DEAD;
    m->takedamage = Q2_DAMAGE_NO;
}
static void boss1_spawn(q2_monster *m)
{
    m->mass = 800; m->movetype = Q2_MOVETYPE_STEP; m->solid = Q2_SOLID_BBOX;
    q2_link_entity(m, 1); q2_link_entity(m, 0x81);
    boss1_stand(m); m->speed_scale = 10; q2_monster_walk_start(m);
}
const q2_cre_impl q2_cre_boss1 = {
    "Boss1",
    { boss1_stand, NULL, boss1_empty, boss1_walk, boss1_run, NULL,
      boss1_attack, NULL, NULL, NULL, NULL, (q2_class_method)(void *)boss1_pain,
      (q2_class_method)(void *)boss1_die },
    { NULL, NULL, boss1_dead, boss1_rocket, boss1_machinegun, boss1_repeat,
      boss1_empty, boss1_run, boss1_explode },
    boss1_spawn
};
