/* The entity boxes outside the linear-mover and GLASS/SHOOTTHEN registries.
 *
 * All nonzero slot flags participate in both movement (0x80053E20) and point
 * clipping (0x80053AC4): FXGROUP's flag 4 is solid as well as damageable.
 * Its health-zero arm allocates the same box with no damage callback.
 */
#include <stdlib.h>
#include <string.h>

#include "sim.h"
#include "rotator.h"

typedef struct q2_scene_solid {
    s32 node;
    s32 rotator;    /* -1 unless this is a ROTHATCH */
    s32 explosive;  /* -1 unless this is an intact FXGROUP part */
    bool freed;     /* 0x80068818: hiding a node frees its box */
} q2_scene_solid;

static bool append_solid(q2_scene_solid **list, u32 *count, u32 *capacity,
                          const q2_scene *scene, s32 node, s32 rot, s32 exp)
{
    q2_scene_node n;
    q2_scene_solid *s;
    if (node < 0 || !q2_scene_get_node(scene, (u32)node, &n)) return true;
    if (*count == *capacity) {
        u32 cap = *capacity ? *capacity * 2u : 16u;
        q2_scene_solid *grown = realloc(*list, cap * sizeof(*grown));
        if (!grown) return false;
        *list = grown;
        *capacity = cap;
    }
    s = &(*list)[(*count)++];
    s->node = node; s->rotator = rot; s->explosive = exp; s->freed = false;
    return true;
}

q2_result q2_sim_attach_scene_solids(q2_sim *sim, const q2_scene *scene,
                                      const q2_uf_operands *ops,
                                      const q2_rotator_set *rotators)
{
    q2_scene_solid *list = NULL;
    q2_move_target *grown;
    u32 count = 0, capacity = 0, first, tail, i;
    q2_event_record rec, prev;
    bool more;

    if (!sim) return Q2_ERR_INVALID_ARG;
    first = sim->mover_count + sim->breakable_solid_count;
    if (first > sim->volume_count ||
        sim->scene_solid_count > sim->volume_count - first)
        return Q2_ERR_INVALID_ARG;
    tail = sim->volume_count - first - sim->scene_solid_count;

    if (scene && rotators) {
        for (i = 0; i < rotators->count; i++) {
            const q2_rotator *r = &rotators->rotators[i];
            if (r->kind == Q2_ROT_TARGET &&
                !append_solid(&list, &count, &capacity, scene, r->node,
                                (s32)i, -1)) goto no_memory;
        }
    }
    if (scene && sim->explosives) {
        for (i = 0; i < sim->explosives->count; i++) {
            const q2_explosive *e = &sim->explosives->items[i];
            int k;
            for (k = 0; k < Q2_EXPLOSIVE_MAX_PARTS; k++)
                if (!append_solid(&list, &count, &capacity, scene, e->node[k],
                                     -1, (s32)i)) goto no_memory;
        }
    }
    if (scene && sim->events_ready && sim->userfuncs_ready) {
        for (more = q2_events_first_record(&sim->events, &rec); more;
             more = q2_events_next_record(&sim->events, &prev, &rec)) {
            prev = rec;
            for (i = 0; i < rec.n_items; i++) {
                q2_event_item item;
                q2_uf_call call;
                const u8 *p;
                int k;
                if (!q2_events_get_item(&sim->events, &rec, i, &item)) break;
                if (!item.payload || item.len < 12 ||
                    q2_uf_decode_call(&call, &sim->userfuncs, &item) != Q2_OK ||
                    call.prim != Q2_UF_OBJDRAWOFF) continue;
                p = q2_uf_operand_at(ops, item.payload - 2, item.len);
                /* Constructor 0x8002BD58 rebases these four Scene slots. */
                for (k = 0; k < 4; k++)
                    if (!append_solid(&list, &count, &capacity, scene,
                                         q2_rd_s16(p + 4 + 2*k), -1, -1))
                        goto no_memory;
            }
        }
    }

    grown = calloc(first + count + tail ? first + count + tail : 1u,
                     sizeof(*grown));
    if (!grown) goto no_memory;
    if (first) memcpy(grown, sim->volumes, first * sizeof(*grown));
    if (tail) memcpy(grown + first + count,
                       sim->volumes + first + sim->scene_solid_count,
                       tail * sizeof(*grown));
    for (i = 0; i < count; i++) {
        q2_scene_node n;
        q2_move_target *t = &grown[first + i];
        int k;
        q2_scene_get_node(scene, (u32)list[i].node, &n);
        for (k = 0; k < 3; k++) {
            t->min[k] = t->env_min[k] = n.bbox_min[k];
            t->max[k] = t->env_max[k] = n.bbox_max[k];
        }
        t->kind = Q2_MOVE_KIND_ENTITY;
        t->id = (s32)(first + i);
        t->active = true;
    }
    free(sim->volumes);
    free(sim->scene_solids);
    sim->volumes = grown;
    sim->volume_count = first + count + tail;
    sim->scene_solids = list;
    sim->scene_solid_count = count;
    sim->move_world.targets = sim->volumes;
    sim->move_world.count = sim->volume_count;
    q2_sim_scene_solids_update(sim, rotators);
    return Q2_OK;

no_memory:
    free(list);
    return Q2_ERR_NO_MEMORY;
}

void q2_sim_scene_solids_update(q2_sim *sim, const q2_rotator_set *rotators)
{
    u32 i, first;
    if (!sim) return;
    first = sim->mover_count + sim->breakable_solid_count;
    for (i = 0; i < sim->scene_solid_count; i++) {
        const q2_scene_solid *s = &sim->scene_solids[i];
        q2_move_target *t = &sim->volumes[first + i];
        s32 offset[3];
        int axis;
        q2_rotators_node_offset(rotators, (u32)s->node, offset);
        for (axis = 0; axis < 3; axis++) {
            t->min[axis] = t->env_min[axis] + offset[axis];
            t->max[axis] = t->env_max[axis] + offset[axis];
        }
        t->active = !s->freed;
        if (s->explosive >= 0 && sim->explosives &&
            (u32)s->explosive < sim->explosives->count)
            t->active = t->active && !sim->explosives->items[s->explosive].destroyed;
        if (s->rotator >= 0 && rotators && (u32)s->rotator < rotators->count) {
            const q2_rotator *r = &rotators->rotators[s->rotator];
            s32 lift = r->state == Q2_ROTST_IDLE || r->state == Q2_ROTST_DELAY
                         ? 0 : -10000;
            /* 0x8002B3B4 / 0x8002B5F8 move only the LIVE box's Y bounds.
             * The original envelope and motion vector remain untouched; this
             * is a relocated slab, not a rotated box or a lift carrying riders. */
            t->min[1] += lift;
            t->max[1] += lift;
        }
    }
}

void q2_sim_scene_solids_hide(q2_sim *sim, s32 node)
{
    u32 i;
    if (!sim) return;
    for (i = 0; i < sim->scene_solid_count; i++)
        if (sim->scene_solids[i].node == node) sim->scene_solids[i].freed = true;
    q2_sim_scene_solids_update(sim, NULL);
}

bool q2_sim_scene_solid_set(q2_sim *sim, s32 node,
                             const s32 min[3], const s32 max[3])
{
    u32 i, first;
    q2_move_target *t;
    int axis;
    if (!sim || node < 0 || !min || !max) return false;
    for (axis = 0; axis < 3; axis++) if (min[axis] > max[axis]) return false;
    first = sim->mover_count + sim->breakable_solid_count;
    if (first + sim->scene_solid_count > sim->volume_count) return false;
    for (i = 0; i < sim->scene_solid_count; i++)
        if (sim->scene_solids[i].node == node &&
            sim->scene_solids[i].rotator < 0 && sim->scene_solids[i].explosive < 0)
            break;
    if (i == sim->scene_solid_count) {
        q2_scene_solid *list = realloc(sim->scene_solids, (i + 1u) * sizeof(*list));
        q2_move_target *volumes;
        if (!list) return false;
        sim->scene_solids = list;
        volumes = realloc(sim->volumes, (sim->volume_count + 1u) * sizeof(*volumes));
        if (!volumes) return false;
        sim->volumes = volumes;
        memmove(volumes + first + i + 1u, volumes + first + i,
                (sim->volume_count - first - i) * sizeof(*volumes));
        sim->volume_count++;
        sim->scene_solid_count++;
    }
    sim->scene_solids[i] = (q2_scene_solid){ node, -1, -1, false };
    t = &sim->volumes[first + i];
    memset(t, 0, sizeof(*t));
    for (axis = 0; axis < 3; axis++) {
        t->min[axis] = t->env_min[axis] = min[axis];
        t->max[axis] = t->env_max[axis] = max[axis];
    }
    t->id = (s32)(first + i);
    t->kind = Q2_MOVE_KIND_ENTITY;
    t->active = true;
    sim->move_world.targets = sim->volumes;
    sim->move_world.count = sim->volume_count;
    return true;
}
