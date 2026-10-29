/* Direct-think creature modules from SLES_015.34's disc. These four props
 * never call monster_start or register an animation-frame method table. */
#include "crebind.h"
#include "ai.h"
#include "aimove.h"
#include "random.h"
#include <string.h>

static u32 handle(q2_monster *m, u32 offset)
{
    q2_cre_bind *b = q2_cre_bind_for(m);
    return (b && b->cre ? b->cre->base : 0x80100000u) + offset;
}
static void noop_pain(q2_monster *m, s16 damage) { (void)m; (void)damage; }

/* RiderStand+0x838; also the final arm of +0x49C. */
static void riderstand_die(q2_monster *m, s16 damage)
{
    (void)damage;
    m->health = m->gib_health;
    m->dead = true; m->deadflag = Q2_DEAD_DEAD;
    m->movetype = Q2_MOVETYPE_TOSS;
    m->svflags |= Q2_SVF_DEADMONSTER;
    m->takedamage = Q2_DAMAGE_NO; m->next_think = 0;
}
static void riderstand_idle(q2_monster *m) /* +0x6CC */
{
    m->frame = (s16)(m->frame + 1);
    if (m->frame >= 474) m->frame = 414;
    m->next_think = q2_level_state.time + 1;
}
static void riderstand_sound(q2_monster *m, u32 offset)
{
    /* Import+0x50 is the player actor, not this projection. */
    const q2_monster *player = q2_level_state.sight_client;
    q2_cre_play_sound_owner(m, handle(m, offset), player ? player : m);
}
static void riderstand_shrink(q2_monster *m) /* +0x49C */
{
    m->render_scale = (s16)(m->render_scale - 512);
    if (m->render_scale == 2048) riderstand_sound(m, 0x990);
    if (!m->render_scale) riderstand_die(m, 0);
    else riderstand_idle(m);
}
static void riderstand_use(q2_monster *m) /* +0x76C */
{
    m->think = riderstand_shrink;
    m->next_think = q2_level_state.time + 1;
    riderstand_sound(m, 0x99C);
}
static void riderstand_pain(q2_monster *m, s16 damage) /* +0x7D0 */
{
    (void)damage;
    if (m->script_pain_used) return;
    riderstand_use(m);
    m->script_pain_used = true;
}
static void riderstand_spawn(q2_monster *m) /* +0x5C4 */
{
    m->start_kind = Q2_CRE_START_SCRIPT;
    m->pain = riderstand_pain; m->die = riderstand_die; m->use = riderstand_use;
    m->movetype = Q2_MOVETYPE_NONE; m->solid = Q2_SOLID_BBOX;
    m->takedamage = Q2_DAMAGE_YES;
    q2_link_entity(m, 5); q2_link_entity(m, 0x85);
    m->frame = 414; m->think = riderstand_idle;
    m->solid = Q2_SOLID_NOT;
    m->next_think = q2_level_state.time + 2;
}

static void rider2_think(q2_monster *m) /* Rider2+0x458 */
{
    m->frame = (s16)(m->frame + 1);
    m->next_think = m->frame < 345 ? q2_level_state.time + 1 : 0;
}
static void rider2_spawn(q2_monster *m) /* Rider2+0x200 */
{
    m->start_kind = Q2_CRE_START_SCRIPT;
    m->angles[2] = (s16)q2_anglemod(q2_random_next());
    m->pain = m->die = noop_pain;
    m->movetype = Q2_MOVETYPE_NONE; m->solid = Q2_SOLID_NOT;
    m->frame = m->health == 5000 ? 344 : 251;
    m->health = 5000;
    m->think = rider2_think; m->next_think = q2_level_state.time + 2;
    q2_link_entity(m, 1); q2_link_entity(m, 0x81);
}

static void rider3_think(q2_monster *m) /* Rider3+0x430 */
{
    m->frame = (s16)(m->frame + 1);
    if (m->frame >= 20) m->frame = 0;
    m->next_think = q2_level_state.time + 1;
}
static void rider3_spawn(q2_monster *m) /* Rider3+0x200 */
{
    m->start_kind = Q2_CRE_START_SCRIPT;
    m->angles[2] = (s16)q2_anglemod(q2_random_next());
    m->movetype = Q2_MOVETYPE_STEP; m->solid = Q2_SOLID_BBOX;
    q2_link_entity(m, 1); q2_link_entity(m, 0x81);
    m->pain = m->die = noop_pain;
    m->frame = 0; m->think = rider3_think;
    m->movetype = Q2_MOVETYPE_NONE; m->solid = Q2_SOLID_NOT;
    m->next_think = q2_level_state.time + 2;
}

static void deadcomm_drop(q2_monster *m) /* DeadComm+0x814 */
{
    m->movetype = Q2_MOVETYPE_TOSS;
    m->pos[1] -= 24;
}
static void deadcomm_animate(q2_monster *m) /* +0x72C */
{
    m->frame = (s16)(m->frame + 1);
    m->next_think = m->frame < 23 ? q2_level_state.time + 1 : 0;
    q2_cre_play_sound_owner(m, handle(m, 0x870), m);
}
static void deadcomm_use(q2_monster *m) /* +0x7A8 */
{
    q2_cre_play_sound_owner(m, handle(m, 0x87C), m);
    m->think = deadcomm_animate;
    m->next_think = q2_level_state.time + 1;
}
static void deadcomm_spawn(q2_monster *m) /* +0x63C */
{
    m->start_kind = Q2_CRE_START_SCRIPT;
    m->use = deadcomm_use; m->pain = noop_pain;
    m->movetype = Q2_MOVETYPE_NONE; m->solid = Q2_SOLID_BBOX;
    m->takedamage = Q2_DAMAGE_YES; m->flags = Q2_FL_GODMODE;
    q2_link_entity(m, 5); q2_link_entity(m, 0x85);
    m->takedamage = Q2_DAMAGE_NO;
    m->think = deadcomm_drop; m->next_think = q2_level_state.time + 5;
}

/* Callback identities persist through save/load and zone revisits without
 * storing host function pointers. Existing saves use 0/1. */
void q2_cre_script_callbacks(q2_monster *m)
{
    const q2_cre_bind *b = m->cre_bind;
    if (!b || !b->impl->scripted) return;
    if (!strcmp(b->impl->name, "RiderStand")) {
        m->pain = riderstand_pain; m->die = riderstand_die; m->use = riderstand_use;
    } else if (!strcmp(b->impl->name, "DeadComm")) {
        m->pain = noop_pain; m->use = deadcomm_use;
    } else m->pain = m->die = noop_pain;
}

static const q2_class_method thinks[] = {
    NULL, q2_M_MoveFrame, riderstand_idle, riderstand_shrink,
    rider2_think, rider3_think, deadcomm_drop, deadcomm_animate, q2_cre_rider_launch,
    q2_cre_medic_revive
};
u8 q2_cre_think_id(const q2_monster *m)
{
    u8 i;
    for (i = 0; i <= Q2_CRE_THINK_LAST; i++)
        if (m->think == thinks[i]) return i;
    return 255;
}
bool q2_cre_think_valid(const q2_monster *m, u8 id)
{
    const q2_cre_bind *b = q2_cre_bind_for(m);
    if (id <= 1) return true;
    if (!b || !b->impl || id > Q2_CRE_THINK_LAST) return false;
    if (id == 9) return !b->impl->scripted;
    if (id == 2 || id == 3) return !strcmp(b->impl->name, "RiderStand");
    if (id == 4) return !strcmp(b->impl->name, "Rider2");
    if (id == 5) return !strcmp(b->impl->name, "Rider3");
    if (id == 8) return !strcmp(b->impl->name, "JorgRider") || !strcmp(b->impl->name, "Rider");
    return !strcmp(b->impl->name, "DeadComm");
}
void q2_cre_restore_think(q2_monster *m, u8 id)
{
    m->think = id <= Q2_CRE_THINK_LAST ? thinks[id] : NULL;
}

const q2_cre_impl q2_cre_riderstand = {
    .name = "RiderStand", .spawn = riderstand_spawn, .scripted = true
};
const q2_cre_impl q2_cre_rider2 = {
    .name = "Rider2", .spawn = rider2_spawn, .scripted = true
};
const q2_cre_impl q2_cre_rider3 = {
    .name = "Rider3", .spawn = rider3_spawn, .scripted = true
};
const q2_cre_impl q2_cre_deadcomm = {
    .name = "DeadComm", .spawn = deadcomm_spawn, .scripted = true
};
