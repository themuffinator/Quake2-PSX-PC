/* Gladiator, transcribed from BIGGUN/ZONE1.DAT at 0x80100000.
 * Animation distances, frame methods and transitions come from the disc.
 */
#include "ai.h"
#include "aimove.h"
#include "crebind.h"
#include "random.h"

extern void (*q2_cre_sound_fn)(q2_monster *, int, void *);
extern void *q2_cre_sound_user;

static void glad_play(q2_monster *m, u32 handle)
{
    if (q2_cre_sound_fn)
        q2_cre_sound_fn(m, (int)handle, q2_cre_sound_user);
}

static void glad_stand(q2_monster *m) { q2_cre_set_move(m, 0); }
static void glad_walk(q2_monster *m)  { q2_cre_set_move(m, 7); }
static void glad_run(q2_monster *m)
{
    q2_cre_set_move(m, (m->aiflags & Q2_AI_STAND_GROUND) ? 0 : 23);
}
static void glad_idle(q2_monster *m)   { glad_play(m, 0x801013ECu); }
static void glad_search(q2_monster *m) { glad_play(m, 0x801013F0u); }
static void glad_sight(q2_monster *m, q2_monster *other)
{
    (void)other;
    glad_play(m, 0x801013F4u);
}

/* +0xEAC: the range gate precedes both the move and the saved target.
 * The rail fires at this snapshot, even if the enemy subsequently moves.
 */
static void glad_attack(q2_monster *m)
{
    s32 delta[3];
    int k;
    if (!m->enemy) return;
    for (k = 0; k < 3; k++) delta[k] = m->enemy->pos[k] - m->pos[k];
    if (q2_vector_length_sq(delta) <= 1052 * 1052) return;
    q2_cre_set_move(m, 46);
    for (k = 0; k < 3; k++) m->blind_target[k] = m->enemy->pos[k];
    m->blind_target[1] += m->enemy->view_height;
}

/* +0x9F8, muzzle cache initialized at +0x96C from vertices 158/169. */
static void glad_fire(q2_monster *m)
{
    q2_cre_shot shot = { 0 };
    int k;
    shot.slot = Q2_IMP_FIRE_RAILGUN;
    shot.damage = 50;
    shot.kick = 100;
    shot.count = 1;
    shot.aimed = true;
    q2_cre_muzzle(m, 158, 169, shot.start);
    for (k = 0; k < 3; k++) shot.dir[k] = m->blind_target[k] - shot.start[k];
    q2_vector_normalize(shot.dir);
    glad_play(m, 0x801013DCu);
    q2_cre_fire_shot(m, &shot);
}

static void glad_melee(q2_monster *m) { q2_cre_set_move(m, 29); }
static void glad_swing(q2_monster *m) { glad_play(m, 0x801013E0u); }
static void glad_hit(q2_monster *m)
{
    const s32 aim[3] = { 1020, -48, m->mins[0] };
    s32 damage = 20 + q2_random_next() % 5;
    q2_cre_fire_hit(m, aim, damage, 150);
}

static void glad_dead(q2_monster *m)
{
    m->next_think = 0;
    m->movetype = Q2_MOVETYPE_TOSS;
    m->svflags |= Q2_SVF_DEADMONSTER;
}

/* +0xF84: a falling Gladiator can switch out of the ordinary pain move
 * during debounce. The signed Y velocity threshold is positive 1200 on PSX.
 * Sound precedes the nightmare check, and neither path draws the RNG.
 */
static void glad_pain(q2_monster *m, s16 damage)
{
    (void)damage;
    if (m->health < m->max_health / 2) m->skinnum = 1;
    if (q2_level_state.time < m->pain_debounce) {
        if (m->velocity[1] > 1200 && m->currentmove == q2_cre_find_move(m, 55))
            q2_cre_set_move(m, 83);
        return;
    }
    m->pain_debounce = q2_level_state.time + 30;
    glad_play(m, 0x801013D0u);
    if (q2_cre_skill() == 3) return;
    q2_cre_set_move(m, m->velocity[1] > 1200 ? 83 : 55);
}

/* +0x1094: the gib threshold is a silent immediate corpse, with no native
 * gib-spawn call. The ordinary death keeps its complete 61..82 animation.
 */
static void glad_die(q2_monster *m, s16 damage)
{
    (void)damage;
    if (m->health <= m->gib_health) {
        m->dead = true;
        m->deadflag = Q2_DEAD_DEAD;
        glad_dead(m);
        return;
    }
    if (m->deadflag == Q2_DEAD_DEAD) return;
    glad_play(m, 0x801013D8u);
    q2_cre_set_move(m, 61);
    m->dead = true;
    m->deadflag = Q2_DEAD_DEAD;
    m->takedamage = Q2_DAMAGE_YES;
}

static void glad_spawn(q2_monster *m)
{
    m->mass = 400;
    m->movetype = Q2_MOVETYPE_STEP;
    m->solid = Q2_SOLID_BBOX;
    q2_link_entity(m, 5);
    q2_link_entity(m, 0x85);
    glad_stand(m);
    m->speed_scale = 10;
    q2_monster_walk_start(m);
}

const q2_cre_impl q2_cre_gladiator = {
    "Gladiator",
    {
        glad_stand, glad_idle, glad_search, glad_walk, glad_run, NULL,
        glad_attack, glad_melee, (q2_class_method)(void *)glad_sight,
        NULL, NULL, (q2_class_method)(void *)glad_pain,
        (q2_class_method)(void *)glad_die
    },
    { NULL, glad_swing, glad_hit, glad_run, glad_fire, glad_dead },
    glad_spawn
};
