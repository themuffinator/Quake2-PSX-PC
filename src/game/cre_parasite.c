/* Parasite, BASE3/ZONE0.DAT, relocated at 0x80100000. */
#include "ai.h"
#include "aimove.h"
#include "combat.h"
#include "crebind.h"
#include "random.h"

#include <string.h>

extern void (*q2_cre_sound_fn)(q2_monster *, int, void *);
extern void *q2_cre_sound_user;
static void parasite_play(q2_monster *m, u32 handle)
{
    if (q2_cre_sound_fn) q2_cre_sound_fn(m, (int)handle, q2_cre_sound_user);
}
static void parasite_stand(q2_monster *m) { q2_cre_set_move(m, 83); }
static void parasite_run(q2_monster *m)
{
    if (m->aiflags & Q2_AI_STAND_GROUND) parasite_stand(m);
    else q2_cre_set_move_at(m, 0x8010168Cu);
}
static void parasite_run_loop(q2_monster *m)
{
    if (m->aiflags & Q2_AI_STAND_GROUND) parasite_stand(m);
    else q2_cre_set_move_at(m, 0x80101674u);
}
static void parasite_walk(q2_monster *m) { q2_cre_set_move_at(m, 0x801016F0u); }
static void parasite_walk_loop(q2_monster *m) { q2_cre_set_move_at(m, 0x801016D8u); }
static void parasite_idle(q2_monster *m) { q2_cre_set_move(m, 100); }
static void parasite_fidget(q2_monster *m) { q2_cre_set_move(m, 104); }
static void parasite_end_fidget(q2_monster *m) { q2_cre_set_move(m, 110); }
static void parasite_refidget(q2_monster *m)
{
    q2_cre_set_move(m, q2_random_next() < 26215 ? 104 : 110);
}
static void parasite_scratch(q2_monster *m) { parasite_play(m, 0x801017ECu); }
static void parasite_tap(q2_monster *m) { parasite_play(m, 0x801017E8u); }
static void parasite_launch(q2_monster *m) { parasite_play(m, 0x801017D4u); }
static void parasite_reel(q2_monster *m) { parasite_play(m, 0x801017E0u); }
static void parasite_sight(q2_monster *m, q2_monster *other)
{
    (void)other;
    parasite_play(m, 0x801017E4u);
}
static void parasite_attack(q2_monster *m) { q2_cre_set_move(m, 39); }

/* +0xBAC..0xE10: the pitch comparison is signed and one-sided. The module
 * tries centre, max-Y minus 96, then min-Y plus 96. It subsequently traces to
 * the centre again, regardless of which of those three passed. */
static bool parasite_drain_angle(const s32 start[3], const s32 end[3])
{
    s32 delta[3];
    s16 angles[3];
    int k;
    for (k = 0; k < 3; k++) delta[k] = start[k] - end[k];
    if (q2_vector_length(delta) > 3072) return false;
    q2_vectoangles(delta, angles);
    if (angles[0] < -2048) angles[0] = (s16)(angles[0] + 4096);
    return angles[0] < 342;
}

static void parasite_drain(q2_monster *m)
{
    static const s16 offset[3] = { 288, 72, 0 };
    const q2_ai_world *w = q2_ai_get_world();
    q2_ai_trace trace;
    s32 forward[3], right[3], start[3], end[3];
    s16 damage;
    int mod;
    if (!m->enemy) return;
    q2_angle_vectors(m->angles, forward, right, NULL);
    q2_project_source(m->pos, offset, forward, right, start);
    memcpy(end, m->enemy->pos, sizeof(end));
    if (!parasite_drain_angle(start, end)) {
        end[1] = m->enemy->pos[1] + m->enemy->maxs[1] - 96;
        if (!parasite_drain_angle(start, end)) {
            end[1] = m->enemy->pos[1] + m->enemy->mins[1] + 96;
            if (!parasite_drain_angle(start, end)) return;
        }
    }
    memcpy(end, m->enemy->pos, sizeof(end));
    w->trace(w->user, start, NULL, NULL, end, m, 0x06000003u, &trace);
    if (trace.ent != m->enemy) return;
    damage = m->frame == 41 ? 5 : 2;
    mod = m->frame == 41 ? Q2_MOD_21 : Q2_MOD_NONE;
    parasite_play(m, m->frame == 41 ? 0x801017D8u : 0x801017DCu);
    /* +0xF38 passes NULL as the damage point. +0xF58 attaches the visible
     * beam to posed vertex 97; neither call uses the earlier projected start. */
    q2_cre_hurt(m, m->enemy, damage, mod, NULL);
    q2_cre_beam(m, m->enemy, 97);
}

static void parasite_dead(q2_monster *m)
{
    m->next_think = 0;
    m->movetype = Q2_MOVETYPE_TOSS;
    m->svflags |= Q2_SVF_DEADMONSTER;
}
static void parasite_die(q2_monster *m, s16 damage)
{
    (void)damage;
    if (m->health <= m->gib_health) {
        m->dead = true; m->deadflag = Q2_DEAD_DEAD;
        parasite_dead(m);
        return;
    }
    if (m->deadflag == Q2_DEAD_DEAD) return;
    parasite_play(m, 0x801017D0u);
    m->dead = true; m->deadflag = Q2_DEAD_DEAD;
    m->takedamage = Q2_DAMAGE_YES;
    q2_cre_set_move(m, 32);
}
static void parasite_pain(q2_monster *m, s16 damage)
{
    (void)damage;
    if (m->health < m->max_health / 2) m->skinnum = 1;
    if (q2_level_state.time < m->pain_debounce) return;
    m->pain_debounce = q2_level_state.time + 30;
    if (q2_cre_skill() == 3) return;
    parasite_play(m, 0x801017CCu);
    q2_cre_set_move(m, 57);
}
static void parasite_spawn(q2_monster *m)
{
    m->mass = 250;
    m->movetype = Q2_MOVETYPE_STEP;
    m->solid = Q2_SOLID_BBOX;
    q2_link_entity(m, 5); q2_link_entity(m, 0x85);
    parasite_stand(m);
    m->speed_scale = 10;
    q2_monster_walk_start(m);
}

const q2_cre_impl q2_cre_parasite = {
    "Parasite",
    { parasite_stand, parasite_idle, NULL, parasite_walk, parasite_run, NULL,
      parasite_attack, NULL, (q2_class_method)(void *)parasite_sight, NULL, NULL,
      (q2_class_method)(void *)parasite_pain,
      (q2_class_method)(void *)parasite_die },
    { NULL, parasite_stand, parasite_run, parasite_run_loop, parasite_walk_loop,
      parasite_walk, parasite_end_fidget, parasite_fidget, parasite_refidget,
      parasite_scratch, parasite_tap, parasite_launch, parasite_drain,
      parasite_reel, parasite_dead },
    parasite_spawn
};
