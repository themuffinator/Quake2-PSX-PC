#include "cremotion.h"
#include "monster.h"
#include "aimove.h"
#include "random.h"
#include "weapon.h"
#include "worldscale.h"
#include "area.h"
#include "scene.h"
#include <string.h>

void q2_cre_active_areas(const q2_area_graph *g, const q2_scene *scene,
                         const u8 *hidden, u32 hidden_count,
                         u8 camera_area, bool all, u32 mask[3])
{
    u8 queue[96];
    u32 head = 0, tail = 0;
    memset(mask, all ? 255 : 0, 3 * sizeof(*mask));
    if (all || !camera_area || camera_area >= 96) return;
    queue[tail++] = camera_area;
    mask[camera_area / 32] |= 1u << (camera_area & 31);
    while (head < tail) {
        u32 area = queue[head++], i, count = q2_area_link_count(g, area);
        for (i = 0; i < count; i++) {
            q2_area_link link;
            q2_scene_node node;
            u32 neighbour, marker;
            if (!q2_area_get_link(g, area, i, &link)) continue;
            neighbour = link.neighbour;
            if (neighbour >= 96 || (mask[neighbour / 32] & (1u << (neighbour & 31)))) continue;
            /* The flood uses this payload halfword as a Scene index
             * (0x80065410), irrespective of its geometric interpretation. */
            marker = (u32)(s32)link.dist;
            if (!q2_scene_get_node(scene, marker, &node) || (node.flags & 0x8000u) ||
                (hidden && marker < hidden_count && hidden[marker])) continue;
            mask[neighbour / 32] |= 1u << (neighbour & 31);
            queue[tail++] = (u8)neighbour;
        }
    }
}

bool q2_cre_motion_suspend(q2_monster *m, const u32 mask[3], s32 area,
                            bool visible, const s32 camera[3])
{
    const s32 *p = q2_cre_origin(m);
    bool suspended = area < 0 || area >= 96 || !(mask[area / 32] & (1u << (area & 31)));
    if (!suspended && !visible)
        suspended = ((u32)p[0] - (u32)camera[0] + 8192u > 16384u) ||
                    ((u32)p[2] - (u32)camera[2] + 8192u > 16384u);
    m->motion.flags = (m->motion.flags & ~0x02000000u) |
                       (suspended ? 0x02000000u : 0u);
    return suspended;
}

const s32 *q2_cre_origin(const q2_monster *m)
{ return m->motion.ready ? m->motion.ent.pos : m->pos; }
s16 q2_cre_yaw(const q2_monster *m)
{ return m->motion.ready ? m->motion.yaw : m->angles[2]; }

void q2_cre_motion_init(q2_monster *m)
{
    q2_cre_motion *p = &m->motion;
    memset(p, 0, sizeof(*p));
    memcpy(p->ent.pos, m->pos, sizeof(p->ent.pos));
    memcpy(p->target, m->pos, sizeof(p->target));
    p->yaw = p->target_yaw = m->angles[2];
    p->ent.node = -1;
    p->ent.max_slope_ny = Q2_MAX_SLOPE_NY;
    p->ent.flags = 1u | (m->on_ground ? Q2_ENT_ON_GROUND : 0u);
    if (m->on_ground) p->ent.ground_normal[1] = 4096;
    p->ent.last_normal[1] = 4096;
    p->flags = 1;
    p->clock = q2_level_state.time;
    /* G_Spawn's second memset (0x8005F8B4) clears the allocator's stagger. */
    p->ready = true;
}

void q2_cre_motion_link(q2_monster *m, u32 what)
{
    q2_cre_motion *p = &m->motion;
    if (!p->ready) return;
    if (what & 1u) {
        memcpy(p->target, m->pos, sizeof(p->target));
        if (!(what & 0x80u)) memcpy(p->ent.pos, m->pos, sizeof(p->ent.pos));
    }
    if (what & 4u) {
        p->target_yaw = m->angles[2];
        if (!(what & 0x80u)) p->yaw = m->angles[2];
    }
}

/* mult/mflo followed by the negative-value bias and sra at 0x8007ED2C.
 * Multiplication wraps at 32 bits; division truncates toward zero. */
static s32 fraction(s32 distance, s32 f)
{ return (s32)((u32)distance * (u32)f) / 4096; }

static void interpolate(q2_monster *m, s32 ticks)
{
    q2_cre_motion *p = &m->motion;
    s32 f = (ticks * 4096) / 30;
    s32 yaw = ((p->target_yaw - p->yaw + 2048) & 4095) - 2048;
    int k;
    for (k = 0; k < 3; k++)
        p->delta[k] = (s16)fraction((s32)((u32)p->target[k] -
                                          (u32)p->ent.pos[k]), f);
    p->yaw = (s16)(p->yaw + fraction(yaw, f));
}

static void move_actor(q2_monster *m, s32 dt, bool integrate,
                       const q2_cre_motion_world *w)
{
    q2_cre_motion *p = &m->motion;
    q2_move_world boxes = {0};
    int k;
    if (w && w->world) boxes = *w->world;
    boxes.half_extent = 286;
    boxes.mask = (p->ent.flags & 1u) ? 0x810u : 0;
    /* 0x80045890: only actors with the contents-query flag run this arm. */
    if (p->ent.flags & 0x10000u) {
        u16 contents = q2_move_contents(&boxes, p->ent.pos, Q2_CONTENTS_MASK);
        s16 ny = p->ent.last_normal[1];
        if (!(contents & 0x1000u) || ny < -1023) p->ent.flags &= ~0x800u;
        else if (ny < 1024) p->ent.flags |= 0x800u;
        /* ny >= 1024 leaves the old bit untouched (0x800458CC). */
        p->ent.flags = (p->ent.flags & ~8u) | ((contents & 0x200u) ? 8u : 0u);
        if (contents & 0x2000u) {
            if (p->ent.flags & Q2_ENT_GROUNDED_MASK) {
                p->impulse[1] = (s16)(p->impulse[1] - 9216);
                p->flags |= 0x4000u;
            }
            p->ent.flags |= 0x10u;
        } else p->ent.flags &= ~0x10u;
    }
    if (integrate && !(p->flags & 0x80000u)) {
        q2_rng rng = { q2_random_state() };
        u32 result = q2_move_separate(w ? w->bodies : NULL, w ? w->self_id : -1,
            p->ent.pos, m->mins, m->maxs, Q2_BODY_RADIUS, p->delta, &rng);
        q2_random_seed(rng.state);
        if (result & Q2_SEPARATE_STOOD_ON) {
            p->ent.ground_normal[0] = p->ent.ground_normal[2] = 0;
            p->ent.ground_normal[1] = 4096;
            p->ent.flags |= 0x2000u;
        } else p->ent.flags &= ~0x2000u;
    }
    q2_move_ground_project(&p->ent, p->velocity);
    p->ent.last_normal[0] = p->ent.last_normal[2] = 0;
    p->ent.last_normal[1] = 4096;
    if (w && w->coll) {
        q2_move_actor(w->coll, &p->ent, p->delta, &boxes, NULL, w->self_id,
            m->mins, m->maxs, NULL, !(m->flags & (Q2_FL_FLY | Q2_FL_SWIM)));
    } else {
        /* Geometry-free harnesses still execute the native arithmetic. */
        for (k = 0; k < 3; k++)
            p->ent.pos[k] = (s32)((u32)p->ent.pos[k] + (u32)(s32)p->delta[k]);
    }
    if (integrate)
        q2_move_integrate(&p->ent, p->velocity, p->impulse, &p->flags,
            w && w->gravity ? w->gravity : Q2_GRAVITY, dt,
            w && w->multiplayer, p->delta);
}

u32 q2_cre_motion_tick(q2_monster *m, s32 dt, const q2_cre_motion_world *w)
{
    q2_cre_motion *p;
    s32 phase, remaining, saved_time, saved_frame;
    u32 ran = 0;
    if (!m || !m->in_use || dt <= 0) return 0;
    if (!m->motion.ready) q2_cre_motion_init(m);
    p = &m->motion;
    p->advanced = false;
    if (m->health <= 0) p->flags |= 0x8000u;
    if ((m->svflags & Q2_SVF_DEADMONSTER) && !m->corpse)
        q2_monster_corpse_detach(m);
    if (m->corpse) { q2_monster_corpse_tick_dt(m, dt); return 0; }
    if (w && w->suspended) { p->clock = q2_level_state.time; return 0; }
    saved_time = q2_level_state.time;
    saved_frame = q2_level_state.framenum;
    phase = (s32)p->phase - p->phase_base;
    if (phase < 0) phase += 30;
    remaining = phase + dt;
    while (remaining >= 30) {
        p->advanced = true;
        q2_level_state.time = q2_level_state.framenum = ++p->clock;
        interpolate(m, 30 - phase);
        move_actor(m, dt, false, w);
        phase = 0;
        remaining -= 30;
        p->flags &= 0x7FFFFFFFu; /* independent of the 0x4000 impulse arm */
        if (m->in_use) {
            /* 0x8005C980(edict,1): actual origin feeds back before thinking. */
            memcpy(m->pos, p->ent.pos, sizeof(m->pos));
            memcpy(p->target, p->ent.pos, sizeof(p->target));
            if (m->movetype == Q2_MOVETYPE_STEP) q2_touch_triggers(m);
            if (m->think && m->next_think > 0 && m->next_think <= p->clock) {
                m->next_think = 0;
                m->think(m);
                ran++;
            }
        }
    }
    /* The residual is divided by 30, not by the time left in this AI tick.
     * Even an exact boundary runs this call with zero interpolated delta. */
    interpolate(m, remaining - phase);
    move_actor(m, dt, true, w);
    p->phase = (u8)(p->phase_base + remaining);
    if (p->phase >= 30) p->phase -= 30;
    q2_level_state.time = saved_time;
    q2_level_state.framenum = saved_frame;
    return ran;
}

void q2_cre_motion_animate(q2_monster *m, s16 first, s16 last, s16 timeline)
{
    q2_cre_motion *p = &m->motion;
    s32 phase = (s32)p->phase - p->phase_base;
    bool was_transition = (p->anim_flags & 2u) != 0;
    if (phase < 0) phase += 30;
    if (p->advanced && first >= 0 && m->frame >= first && m->frame <= last) {
        s16 target = (s16)(timeline + (m->frame - first) * 30);
        s16 end = (s16)(timeline + (last - first) * 30);
        s32 old = p->anim_base;
        if (old == target) p->flags |= 0x40u;
        else p->flags &= ~0x40u;
        p->anim_flags = (u8)((p->anim_flags & ~2u) |
            ((old < timeline || old > end || target < old) ? 2u : 0u));
        p->anim_position = p->anim_previous = (s16)old;
        p->anim_base = (u16)target & 0x7FFFu;
    }
    if (!(p->flags & 0x40u)) p->anim_position = (s16)(p->anim_previous + phase);
    p->anim_flags = (u8)((p->anim_flags & ~1u) |
        (((p->anim_flags & 2u) && phase >= 20) ? 1u : 0u));
    if (p->advanced && was_transition && p->queued_yaw) {
        p->target_yaw = (s16)(p->target_yaw + (u16)p->queued_yaw);
        p->yaw = (s16)(p->yaw + (u16)p->queued_yaw);
        m->angles[2] = (s16)(m->angles[2] + (u16)p->queued_yaw);
        p->queued_yaw = 0;
    }
    p->animation_ready = true;
}
