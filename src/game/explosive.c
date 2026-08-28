/*
 * explosive.c — opcode 0x08. See explosive.h for the executable addresses this
 * transcribes and for the three branches that decide the behaviour.
 */
#include "explosive.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
static bool set_grow(q2_explosive_set *set)
{
    u32 want;
    q2_explosive *grown;

    if (set->count < set->capacity)
        return true;

    want  = set->capacity ? set->capacity * 2 : 16;
    grown = (q2_explosive *)realloc(set->items, want * sizeof(*grown));
    if (!grown)
        return false;

    set->items    = grown;
    set->capacity = want;
    return true;
}

static void vis_add(s16 *list, u32 *count, s16 node)
{
    u32 i;

    if (node < 0 || *count >= Q2_EXPLOSIVE_MAX_VIS)
        return;
    /* The console writes the same flag twice without minding; a list has to
     * mind, because the caller applies it to a byte array. */
    for (i = 0; i < *count; i++)
        if (list[i] == node)
            return;
    list[(*count)++] = node;
}

/* ------------------------------------------------------------------------- */
u16 q2_explosive_node_area(const q2_scene *scene, s16 node)
{
    q2_scene_node n;

    if (!scene || node < 0 ||
        !q2_scene_get_node(scene, (u32)node, &n))
        return 0;

    /*
     * `lhu a1, 14(a1)` at 0x80026948, masked with 0x7F at 0x80026954. The
     * halfword spans scene.h's `area` and the byte after it, which is zero on
     * every node on the disc, so the mask makes the two readings agree.
     */
    return (u16)(n.area & 0x7Fu);
}

static bool node_ok(const q2_scene *scene, s16 node)
{
    if (node < 0)
        return false;
    if (!scene)
        return true;
    return (u32)node < scene->node_count;
}

/* ------------------------------------------------------------------------- */
q2_result q2_explosives_build(q2_explosive_set *out, const q2_events *events,
                              const q2_uf_operands *ops,
                              const q2_scene *scene)
{
    q2_event_record rec, prev;
    bool more;

    if (!out || !events)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    for (more = q2_events_first_record(events, &rec);
         more;
         more = q2_events_next_record(events, &prev, &rec)) {
        u32 i;

        prev = rec;

        for (i = 0; i < rec.n_items; i++) {
            q2_event_item item;
            q2_explosive *e;
            const u8 *p;
            int k;

            if (!q2_events_get_item(events, &rec, i, &item))
                break;
            if (item.opcode != Q2_EVOP_FXGROUP || !item.payload)
                continue;

            /* 0x800267E8 and 0x80026A40 both bail on a length mismatch, so a
             * short item is not an explosive at all. */
            if (item.len != Q2_EXPLOSIVE_ITEM_LEN)
                continue;

            if (!set_grow(out)) {
                q2_explosives_free(out);
                return Q2_ERR_NO_MEMORY;
            }

            /*
             * The payload points past the two header bytes, so a documented
             * +N is payload[N - 2] — the convention mover.c and simcombat.c
             * both use. The slots come through the rebase; nothing else does,
             * because nothing else is an OBJSLOT.
             */
            p = q2_uf_operand_at(ops, item.payload - 2, Q2_EXPLOSIVE_ITEM_LEN);

            e = &out->items[out->count];
            memset(e, 0, sizeof(*e));

            for (k = 0; k < Q2_EXPLOSIVE_MAX_PARTS; k++) {
                s16 a = q2_rd_s16(p + 6 + 2 * k);
                s16 b = q2_rd_s16(p + 14 + 2 * k);

                e->node[k]   = node_ok(scene, a) ? a : (s16)-1;
                e->rubble[k] = node_ok(scene, b) ? b : (s16)-1;
                if (e->node[k] >= 0)
                    e->part_count++;
            }
            {
                s16 r = q2_rd_s16(p + 26);
                e->reveal = node_ok(scene, r) ? r : (s16)-1;
            }

            /*
             * Health is read from the WALKED copy, not the rebased one: the
             * exec's `lhu v0, 22(s2)` has s2 pointing at the item in the
             * record being run, and only the slots are rebased. The same split
             * simcombat.c documents for a shootable MOVER_A.
             */
            e->health       = q2_rd_s16(item.payload - 2 + 22);
            e->health_reset = e->health;
            e->hit_pieces   = q2_rd_u8(item.payload - 2 + 24);
            e->destroy      = (s8)q2_rd_u8(item.payload - 2 + 25);

            /* 0x80026B10: health zero takes the arm that installs no damage
             * callback, so nothing a weapon does can reach it. */
            e->damageable   = (e->health != 0);

            e->item_offset   = item.offset;
            e->record_offset = rec.offset;

            out->count++;
        }
    }

    return Q2_OK;
}

void q2_explosives_free(q2_explosive_set *set)
{
    if (!set)
        return;
    free(set->items);
    set->items    = NULL;
    set->count    = 0;
    set->capacity = 0;
}

/* ------------------------------------------------------------------------- */
u32 q2_explosive_initial_vis(const q2_explosive_set *set, u32 index,
                             q2_explosive_result *res)
{
    const q2_explosive *e;
    int k;

    if (!res)
        return 0;

    memset(res, 0, sizeof(*res));
    res->hit_node = -1;

    if (!set || index >= set->count)
        return 0;

    e = &set->items[index];

    /*
     * The constructor's three writes, in its order: `reveal` hidden
     * (0x80026ACC), array A shown (0x80026B84), array B hidden (0x80026C60).
     * The A show happens on BOTH arms — the health-zero path at 0x80026CE4
     * clears the same bit — so it is unconditional here.
     */
    vis_add(res->hide, &res->hide_count, e->reveal);

    for (k = 0; k < Q2_EXPLOSIVE_MAX_PARTS; k++) {
        vis_add(res->show, &res->show_count, e->node[k]);
        vis_add(res->hide, &res->hide_count, e->rubble[k]);
    }

    return res->hide_count + res->show_count;
}

/* ------------------------------------------------------------------------- */
int q2_explosive_find(const q2_explosive_set *set, u32 item_offset)
{
    u32 i;

    if (!set)
        return -1;
    for (i = 0; i < set->count; i++)
        if (set->items[i].item_offset == item_offset)
            return (int)i;
    return -1;
}

static void node_centre(const q2_scene *scene, s16 node, s32 out[3])
{
    q2_scene_node n;
    int k;

    out[0] = out[1] = out[2] = 0;
    if (!scene || node < 0 || !q2_scene_get_node(scene, (u32)node, &n))
        return;

    /*
     * `(min + max) / 2` with the console's own rounding: `srl 31; addu; sra 1`
     * at 0x800268C4, which rounds toward zero rather than toward -inf. The
     * node's RAW box, without q2_scene_node_bounds' culling slop — 0x80026898
     * reads the record's +16 and +28 directly.
     */
    for (k = 0; k < 3; k++) {
        s32 sum = n.bbox_min[k] + n.bbox_max[k];
        out[k] = sum / 2;
    }
}

bool q2_explosive_damage(q2_explosive_set *set, u32 index, int part,
                         s16 damage, bool suppress, const q2_scene *scene,
                         q2_explosive_result *res)
{
    q2_explosive_result local;
    q2_explosive *e;
    int k;

    if (!res)
        res = &local;
    memset(res, 0, sizeof(*res));
    res->hit_node = -1;

    if (!set || index >= set->count)
        return false;

    e = &set->items[index];
    if (e->destroyed)
        return false;

    /*
     * 0x80026804: a zero damage argument skips the counter AND the hit burst
     * and falls straight into the destruction. That is what a script call is,
     * and it is why an FXGROUP in a record does not need the player to have
     * shot anything.
     */
    if (damage != 0) {
        s16 hit = -1;

        if (part >= 0 && part < Q2_EXPLOSIVE_MAX_PARTS)
            hit = e->node[part];

        e->health = (s16)(e->health - damage);

        /*
         * The burst runs BEFORE the survival test (0x80026820 precedes the
         * `bgtz` at 0x80026830), and its origin argument is zero — so the
         * pieces scatter through the whole node rather than out of the impact
         * point. GLASS does the opposite; see the header.
         */
        if (hit >= 0) {
            res->hit_node   = hit;
            res->hit_pieces = e->hit_pieces;
        }

        if (e->health > 0)
            return false;
    }

    /* ------------------------------------------------------------------- */
    /* Destruction                                                          */
    /* ------------------------------------------------------------------- */
    e->destroyed = 1;
    res->destroyed = 1;

    /* 0x80026868, before the loop and outside it. */
    vis_add(res->show, &res->show_count, e->reveal);

    for (k = 0; k < Q2_EXPLOSIVE_MAX_PARTS; k++) {
        s16 n = e->node[k];

        if (n >= 0) {
            if (!suppress) {
                q2_explosive_burst *b = &res->burst[res->burst_count];

                memset(b, 0, sizeof(*b));
                b->node = n;
                node_centre(scene, n, b->at);

                /*
                 * `bgez` at 0x80026938. A negative count means DETONATE, and
                 * the count itself is the one's complement — `nor s0, zero,
                 * s0` at 0x80026944 — so -1 explodes and throws nothing.
                 */
                if (e->destroy < 0) {
                    b->explode = 1;
                    b->pieces  = (u8)(~e->destroy);
                    b->area    = q2_explosive_node_area(scene, n);
                } else {
                    b->pieces  = (u8)e->destroy;
                }
                res->burst_count++;
            }

            /* 0x80068818: hide the node and free its damageable box. Runs
             * whether or not the effects were suppressed. */
            vis_add(res->hide, &res->hide_count, n);
        }

        /* 0x80026990's arm is OUTSIDE the `bltz` above, so a slot with no
         * intact node still reveals its wreckage. */
        vis_add(res->show, &res->show_count, e->rubble[k]);
    }

    return true;
}

bool q2_explosive_trigger_item(q2_explosive_set *set, u32 item_offset,
                               bool suppress, const q2_scene *scene,
                               q2_explosive_result *res)
{
    int idx = q2_explosive_find(set, item_offset);

    if (idx < 0)
        return false;

    /* A script reaches the exec with damage 0 and no part — 0x800276B0 sets
     * a2 to zero at the dispatch arm itself. */
    return q2_explosive_damage(set, (u32)idx, -1, 0, suppress, scene, res);
}
