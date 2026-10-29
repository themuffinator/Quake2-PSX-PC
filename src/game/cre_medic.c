/* Medic, JAIL3/ZONE3.DAT (all campaign copies agree). Native addresses are
 * module-relative. The host keeps the actor and edict together, so a corpse
 * can be referenced directly instead of through the module's scratch edict. */
#include "ai.h"
#include "aimove.h"
#include "crebind.h"
#include "random.h"
#include <string.h>

static void sound(q2_monster *m, u32 offset)
{ q2_cre_play_sound_owner(m, 0x80100000u + offset, m); }
static void medic_stand(q2_monster *m) { q2_cre_set_move(m, 12); }
static void medic_walk(q2_monster *m) { q2_cre_set_move(m, 0); }

static bool corpse_valid(const q2_monster *c)
{
    return c && c->in_use && c->corpse && !c->gibbed && !c->dissolve_arm &&
           c->health <= 0 && c->health > c->gib_health;
}
/* +0xBDC: refresh the actor behind the temporary corpse edict before each
 * local AI verb. Stable roster slots replace the console's generation check. */
static void medic_refresh(q2_monster *m)
{
    if (!(m->aiflags & Q2_AI_MEDIC) || !m->enemy || m->enemy->health > 0) return;
    if (!corpse_valid(m->enemy)) {
        if (m->goalentity == m->enemy) m->goalentity = NULL;
        m->enemy = NULL;
        m->aiflags &= ~(u32)Q2_AI_MEDIC;
    }
}
/* +0xDA0: the largest original max_health wins, not the nearest corpse. */
static q2_monster *medic_find_dead(q2_monster *m)
{
    u32 i, count;
    q2_monster *set = q2_cre_roster(&count), *best = NULL;
    for (i = 0; i < count; i++) {
        q2_monster *c = &set[i];
        s32 delta[3]; int k;
        if (c == m || !corpse_valid(c) || c->pop_class_id == 34 ||
            (best && c->max_health <= best->max_health)) continue;
        for (k = 0; k < 3; k++) delta[k] = c->pos[k] - m->pos[k];
        if (q2_vector_length(delta) > 12288 || !q2_visible(m, c)) continue;
        best = c;
    }
    return best;
}
static bool medic_acquire(q2_monster *m)
{
    q2_monster *c = medic_find_dead(m);
    if (!c) return false;
    if (m->enemy && !(m->aiflags & Q2_AI_MEDIC)) m->oldenemy = m->enemy;
    m->enemy = c; m->aiflags |= Q2_AI_MEDIC;
    m->show_hostile = q2_level_state.time + 10;
    memcpy(m->last_sighting, c->pos, sizeof(m->last_sighting));
    m->trail_time = q2_level_state.time;
    q2_hunt_target(m);
    return true;
}
static void medic_idle(q2_monster *m)
{ sound(m, 0x3408); medic_acquire(m); }
static void medic_search(q2_monster *m)
{ sound(m, 0x341C); if (!m->oldenemy) medic_acquire(m); }
static void medic_run(q2_monster *m)
{
    medic_refresh(m);
    if (!(m->aiflags & Q2_AI_MEDIC) && medic_acquire(m)) return;
    q2_cre_set_move(m, m->aiflags & Q2_AI_STAND_GROUND ? 12 : 102);
}
static void medic_attack(q2_monster *m)
{ q2_cre_set_move(m, m->aiflags & Q2_AI_MEDIC ? 209 : 177); }
static bool medic_checkattack(q2_monster *m)
{
    if (m->aiflags & Q2_AI_MEDIC) {
        s32 delta[3]; int k;
        for (k = 0; k < 3; k++) delta[k] = m->pos[k] - m->last_sighting[k];
        if (q2_vector_length(delta) > 3072) return false;
        medic_attack(m); return true;
    }
    return q2_M_CheckAttack(m);
}
static void medic_fire(q2_monster *m)
{
    q2_cre_shot s = { 0 }; int k;
    s.slot = Q2_IMP_FIRE_BLASTER; s.damage = 2; s.speed = 1000;
    s.count = 1; s.aimed = true;
    q2_cre_muzzle(m, 239, 246, s.start);
    if (m->enemy) {
        for (k = 0; k < 3; k++) s.dir[k] = m->enemy->pos[k] - s.start[k];
        s.dir[1] += m->enemy->view_height;
        q2_vector_normalize(s.dir);
    } else q2_angle_vectors(m->angles, s.dir, NULL, NULL);
    /* +0x1574 computes an effect in s3 but never passes it to import+0x80.
     * The call explicitly passes flash zero at +0x1724. */
    q2_cre_fire_shot(m, &s);
}
static void medic_refire(q2_monster *m)
{
    if (m->enemy && q2_visible(m, m->enemy) && q2_random_next() < 31129)
        q2_cre_set_move(m, 191);
}
static void medic_launch(q2_monster *m) { sound(m, 0x3420); }
/* +0x174C is an ACTOR handler installed by cable frame 226. It must run
 * outside that frame callback; its identity also travels through saves. */
void q2_cre_medic_revive(q2_monster *corpse)
{
    q2_monster *medic = corpse->owner;
    corpse->think = NULL; corpse->next_think = 0;
    if (!corpse_valid(corpse) || !q2_cre_revive(medic, corpse)) {
        if (medic && medic->enemy == corpse) {
            medic->enemy = medic->goalentity = NULL;
            medic->aiflags &= ~(u32)Q2_AI_MEDIC;
        }
    }
}
static void medic_cable(q2_monster *m)
{
    const q2_ai_world *w = q2_ai_get_world();
    q2_monster *c;
    q2_ai_trace tr;
    s32 delta[3], to[3]; s16 angles[3]; s32 pitch; int k;
    medic_refresh(m); c = m->enemy;
    if (!c || !q2_ent_inuse(c)) { m->aiflags &= ~(u32)Q2_AI_MEDIC; return; }
    for (k = 0; k < 3; k++) delta[k] = m->pos[k] - m->last_sighting[k];
    if (q2_vector_length(delta) > 3072) return;
    q2_vectoangles(delta, angles); pitch = angles[0];
    if (pitch < -2048) pitch += 4096;
    if (pitch > 2048) pitch -= 4096;
    if (pitch < -512 || pitch > 512) return;
    memcpy(to, c->pos, sizeof(to)); to[1] += (572 - c->hit_height) / 2;
    w->trace(w->user, m->pos, NULL, NULL, to, m, 0x06000003u, &tr);
    if (tr.ent != c) return;
    if (m->frame == 219) {
        sound(m, 0x3424); q2_cre_actor_effect(c, 5, 30); c->aiflags |= Q2_AI_RESURRECTING;
    } else if (m->frame == 226 && corpse_valid(c)) {
        c->owner = m;
        c->oldenemy = m->oldenemy && m->oldenemy->client ? m->oldenemy : NULL;
        c->think = q2_cre_medic_revive; c->next_think = q2_level_state.time + 1;
    } else sound(m, 0x3428);
    q2_cre_beam(m, c, 308);
}
static void medic_retract(q2_monster *m)
{
    sound(m, 0x342C);
    if (m->enemy) m->enemy->aiflags &= ~(u32)Q2_AI_RESURRECTING;
}
static void medic_duck_down(q2_monster *m)
{
    if (m->aiflags & Q2_AI_DUCKED) return;
    m->aiflags |= Q2_AI_DUCKED; m->takedamage = Q2_DAMAGE_YES;
    m->pausetime = q2_level_state.time + 10;
}
static void medic_duck_hold(q2_monster *m)
{
    if (q2_level_state.time < m->pausetime) m->aiflags |= Q2_AI_HOLD_FRAME;
    else m->aiflags &= ~(u32)Q2_AI_HOLD_FRAME;
}
static void medic_duck_up(q2_monster *m)
{ m->aiflags &= ~(u32)Q2_AI_DUCKED; m->takedamage = Q2_DAMAGE_AIM; }
static void medic_dodge(q2_monster *m, q2_monster *attacker, s32 eta)
{
    (void)eta;
    if (q2_random_next() > 8192) return;
    if (!m->enemy) m->enemy = attacker;
    q2_cre_set_move(m, 131);
}
static void medic_dead(q2_monster *m)
{ m->next_think = 0; m->movetype = Q2_MOVETYPE_TOSS; m->svflags |= Q2_SVF_DEADMONSTER; }
static void medic_die(q2_monster *m, s16 damage)
{
    (void)damage;
    if (m->enemy && m->enemy->owner == m) m->enemy->owner = NULL;
    if (m->health <= m->gib_health) {
        sound(m, 0x3430); m->dead = true; m->deadflag = Q2_DEAD_DEAD;
        medic_dead(m); return;
    }
    if (m->deadflag == Q2_DEAD_DEAD) return;
    sound(m, 0x3414); q2_cre_set_move(m, 147);
    m->dead = true; m->deadflag = Q2_DEAD_DEAD; m->takedamage = Q2_DAMAGE_YES;
}
static void medic_pain(q2_monster *m, s16 damage)
{
    bool second; (void)damage;
    if (m->health < m->max_health / 2) m->skinnum = 1;
    if (q2_level_state.time < m->pain_debounce) return;
    m->pain_debounce = q2_level_state.time + 30;
    if (q2_cre_skill() == 3) return;
    second = q2_random_next() >= 16384;
    sound(m, second ? 0x3410 : 0x340C); q2_cre_set_move(m, second ? 116 : 108);
}
static void medic_sight(q2_monster *m, q2_monster *other)
{ (void)other; sound(m, 0x3418); }
static void medic_verb(q2_monster *m, u8 verb)
{
    s32 index, dist;
    if (!m->currentmove) return;
    index = m->frame - m->currentmove->first_frame;
    if (index < 0 || m->frame > m->currentmove->last_frame) return;
    dist = q2_monster_frame_dist(m, &m->currentmove->frames[index]);
    medic_refresh(m); q2_ai_verbs[verb](m, dist);
}
static void medic_ai_stand(q2_monster *m) { medic_verb(m, 1); }
static void medic_ai_walk(q2_monster *m) { medic_verb(m, 2); }
static void medic_ai_run(q2_monster *m) { medic_verb(m, 3); }
static void medic_ai_charge(q2_monster *m) { medic_verb(m, 4); }
static void medic_ai_move(q2_monster *m) { medic_verb(m, 5); }
static void medic_spawn(q2_monster *m)
{
    m->mass = 400; m->movetype = Q2_MOVETYPE_STEP; m->solid = Q2_SOLID_BBOX;
    q2_link_entity(m, 1); q2_link_entity(m, 0x81);
    medic_stand(m); m->speed_scale = 10; q2_monster_walk_start(m);
}
const q2_cre_impl q2_cre_medic = {
    "Medic", { medic_stand, medic_idle, medic_search, medic_walk, medic_run,
    (q2_class_method)(void *)medic_dodge, medic_attack, NULL,
    (q2_class_method)(void *)medic_sight, (q2_class_method)(void *)medic_checkattack,
    NULL, (q2_class_method)(void *)medic_pain, (q2_class_method)(void *)medic_die },
    { [1] = medic_idle, [2] = medic_run, [3] = medic_dead,
      [4] = medic_duck_down, [5] = medic_duck_hold, [6] = medic_duck_up,
      [7] = medic_fire, [8] = medic_refire, [9] = medic_launch,
      [10] = medic_cable, [11] = medic_retract, [27] = medic_ai_stand,
      [28] = medic_ai_walk, [29] = medic_ai_run, [30] = medic_ai_charge,
      [31] = medic_ai_move }, medic_spawn
};
