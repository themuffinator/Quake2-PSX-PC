/* Hover, transcribed from JAIL3/ZONE1.DAT at 0x80100000. */
#include "ai.h"
#include "aimove.h"
#include "crebind.h"
#include "random.h"

extern void (*q2_cre_sound_fn)(q2_monster *, int, void *);
extern void *q2_cre_sound_user;

static void hover_play(q2_monster *m, u32 handle)
{
    if (q2_cre_sound_fn)
        q2_cre_sound_fn(m, (int)handle, q2_cre_sound_user);
}
static void hover_stand(q2_monster *m) { q2_cre_set_move(m, 0); }
static void hover_walk(q2_monster *m) { q2_cre_set_move_at(m, 0x801012F4u); }
static void hover_run(q2_monster *m)
{
    if (m->aiflags & Q2_AI_STAND_GROUND) hover_stand(m);
    else q2_cre_set_move_at(m, 0x80101370u);
}
static void hover_attack(q2_monster *m) { q2_cre_set_move(m, 197); }
static void hover_fire_start(q2_monster *m) { q2_cre_set_move(m, 200); }

/* +0x9FC: alive and visible gates precede the 60% repeat draw. */
static void hover_refire(q2_monster *m)
{
    if (m->enemy && m->enemy->health > 0 && q2_visible(m, m->enemy) &&
        q2_random_next() < 19661)
        hover_fire_start(m);
    else
        q2_cre_set_move(m, 203);
}

/* +0x790: raw eye delta, no normalise and no velocity lead. Even frames use
 * the second muzzle; only frame 200 passes effect 0x40 to the engine wrapper.
 */
static void hover_fire(q2_monster *m)
{
    q2_cre_shot shot = { 0 };
    int k;
    shot.slot = Q2_IMP_FIRE_BLASTER;
    shot.damage = 1; shot.speed = 1000; shot.count = 1; shot.aimed = true;
    shot.effects = m->frame == 200 ? 64 : 0;
    if (m->frame & 1) q2_cre_muzzle(m, 275, 277, shot.start);
    else q2_cre_muzzle(m, 267, 269, shot.start);
    if (m->enemy) {
        for (k = 0; k < 3; k++) shot.dir[k] = m->enemy->pos[k] - shot.start[k];
        shot.dir[1] += m->enemy->view_height;
    } else {
        q2_angle_vectors(m->angles, shot.dir, NULL, NULL);
    }
    q2_cre_fire_shot(m, &shot);
}

/* +0x99C: the completed death explicitly reaches gib health and disables
 * damage. The shared corpse dispatcher handles its resulting body effect.
 */
static void hover_dead(q2_monster *m)
{
    m->health = m->gib_health;
    m->next_think = 0;
    m->movetype = Q2_MOVETYPE_TOSS;
    m->svflags |= Q2_SVF_DEADMONSTER;
    m->takedamage = Q2_DAMAGE_NO;
}

static void hover_die(q2_monster *m, s16 damage)
{
    (void)damage;
    if (m->health <= m->gib_health) {
        hover_play(m, 0x8010147Cu);
        m->dead = true; m->deadflag = Q2_DEAD_DEAD;
        hover_dead(m);
        return;
    }
    if (m->deadflag == Q2_DEAD_DEAD) return;
    hover_play(m, 0x8010146Cu);
    m->dead = true; m->deadflag = Q2_DEAD_DEAD;
    m->takedamage = Q2_DAMAGE_YES;
    q2_cre_set_move(m, 162);
}

static void hover_pain(q2_monster *m, s16 damage)
{
    s32 first;
    if (m->health < m->max_health / 2) m->skinnum = 1;
    if (q2_level_state.time < m->pain_debounce) return;
    m->pain_debounce = q2_level_state.time + 30;
    if (q2_cre_skill() == 3) return;
    first = damage <= 25 ? (q2_random_next() < 16384 ? 153 : 141) : 113;
    /* Init +0x480 aliases +0x1460 to this registered handle. */
    hover_play(m, 0x80101464u);
    q2_cre_set_move(m, first);
}

static void hover_sight(q2_monster *m, q2_monster *other)
{
    (void)other;
    hover_play(m, 0x80101470u);
}
/* Init +0x664 aliases the search handle at +0x1474 to +0x1478. */
static void hover_search(q2_monster *m) { hover_play(m, 0x80101478u); }

static void hover_spawn(q2_monster *m)
{
    m->mass = 150;
    m->movetype = Q2_MOVETYPE_STEP;
    m->solid = Q2_SOLID_BBOX;
    q2_link_entity(m, 1); q2_link_entity(m, 0x81);
    hover_stand(m);
    m->speed_scale = 10;
    q2_monster_fly_start(m);
}

const q2_cre_impl q2_cre_hover = {
    "Hover",
    {
        hover_stand, NULL, hover_search, hover_walk, hover_run, NULL,
        hover_attack, NULL, (q2_class_method)(void *)hover_sight,
        NULL, NULL, (q2_class_method)(void *)hover_pain,
        (q2_class_method)(void *)hover_die
    },
    { NULL, hover_run, hover_stand, hover_dead, hover_fire_start,
      hover_refire, hover_fire },
    hover_spawn
};
