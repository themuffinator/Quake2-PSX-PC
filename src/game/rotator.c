#include "rotator.h"

#include <stdlib.h>
#include <string.h>

static q2_rotator *rotator_push(q2_rotator_set *set)
{
    if (set->count == set->capacity) {
        u32 cap = set->capacity ? set->capacity * 2 : 16;
        q2_rotator *grown = (q2_rotator *)realloc(set->rotators,
                                                  cap * sizeof(*grown));
        if (!grown)
            return NULL;
        set->rotators = grown;
        set->capacity = cap;
    }

    memset(&set->rotators[set->count], 0, sizeof(set->rotators[0]));
    set->rotators[set->count].node = -1;
    return &set->rotators[set->count++];
}

q2_rotator *q2_rotators_add(q2_rotator_set *set, q2_rot_kind kind,
                            s16 node, u8 axis, s16 speed)
{
    q2_rotator *r;

    if (!set)
        return NULL;

    r = rotator_push(set);
    if (!r)
        return NULL;

    r->kind  = kind;
    r->node  = node;
    r->axis  = (u8)(axis & 3u);
    r->speed = speed;
    r->sound_pending = Q2_ROTSND_NONE;
    return r;
}

/* The three handles, registered by name alongside the linear mover's at
 * 0x8002D644 / 0x8002D6D8 / 0x8002D76C. */
const char *const q2_rot_sound_name[Q2_ROTSND_COUNT] = {
    "pt1__strt",   /* 0x800B27D8 */
    "pt1__mid",    /* 0x800B27DC */
    "pt1__end",    /* 0x800B27E0 */
    "amb_butn2"    /* 0x800B27E4 */
};

s8 q2_rotator_take_sound(q2_rotator_set *set, u32 index)
{
    s8 s;

    if (!set || index >= set->count)
        return Q2_ROTSND_NONE;
    s = set->rotators[index].sound_pending;
    set->rotators[index].sound_pending = Q2_ROTSND_NONE;

    /* Track the loop across the drain, so the owner is told to start it once
     * and stop it once. */
    if (s == Q2_ROTSND_MID)
        set->rotators[index].loop_running = true;
    else if (s == Q2_ROTSND_END)
        set->rotators[index].loop_running = false;

    return s;
}

void q2_rotators_set_operand_source(q2_rotator_set *set, const u8 *base_a,
                                    const u8 *base_b, u32 b_size)
{
    if (!set)
        return;
    set->operand_base_a = base_a;
    set->operand_base_b = base_b;
    set->operand_b_size = b_size;
}

/*
 * The address an operand is actually read from — `p` rebased from chunk A into
 * chunk B, per 0x800285F0. Falls back to `p` when no second chunk is set or the
 * offset does not fit in it, so a caller that never calls
 * q2_rotators_set_operand_source() behaves exactly as before.
 */
static const u8 *operand_at(const q2_rotator_set *set, const u8 *p, u32 need)
{
    q2_uf_operands src;

    if (!set)
        return p;

    src.base_a = set->operand_base_a;
    src.base_b = set->operand_base_b;
    src.b_size = set->operand_b_size;

    return q2_uf_operand_at(&src, p, need);
}

/*
 * The constructor's obj+0x18 write, shared by SIMROT/SIMROT2 and ROTBUTTON
 * and adjusted by ROTHATCH. This is deliberately based on the RAW Scene box:
 * q2_scene_node_bounds() adds four units for conservative render culling, and
 * retail reads +0x10/+0x1C directly.
 *
 * `midpoint` reproduces `addu; srl 31; addu; sra 1`: the add wraps to 32 bits
 * and division rounds toward zero. `store_s16` reproduces the final `sh`
 * without relying on an out-of-range signed cast on the host.
 */
static s32 midpoint(s32 a, s32 b)
{
    u32 raw = (u32)a + (u32)b;
    s32 sum = (raw & 0x80000000u) ? -1 - (s32)~raw : (s32)raw;

    return sum / 2;
}

static s16 store_s16(u32 raw)
{
    u16 low = (u16)raw;

    if (low < 0x8000u)
        return (s16)low;
    return (s16)(-(s32)(0x10000u - low));
}

static void rotator_set_pivot(q2_rotator *r, const q2_scene *scene,
                              const s32 adjustment[3])
{
    q2_scene_node node;
    int c;

    if (!r || !scene || r->node < 0 ||
        !q2_scene_get_node(scene, (u32)r->node, &node))
        return;

    for (c = 0; c < 3; c++) {
        s32 centre = midpoint(node.bbox_min[c], node.bbox_max[c]);
        u32 value = (u32)centre - (u32)node.origin[c];

        if (adjustment)
            value += (u32)adjustment[c];
        r->pivot[c] = store_s16(value);
    }
}

q2_result q2_rotators_build(q2_rotator_set *out, const q2_events *events,
                            const q2_userfuncs *uf, const q2_scene *scene)
{
    q2_event_record rec;

    const u8 *keep_a, *keep_b;
    u32 keep_size;

    if (!out || !events)
        return Q2_ERR_INVALID_ARG;

    /*
     * Carry the operand source across the memset. Zeroing it here would make
     * q2_rotators_set_operand_source() a no-op whenever it is called before
     * building — which is the only order that works, since the build itself
     * reads the slots. The creature binds lost a whole pass to exactly this:
     * a memset downstream of the setter silently discarded it.
     */
    keep_a    = out->operand_base_a;
    keep_b    = out->operand_base_b;
    keep_size = out->operand_b_size;

    memset(out, 0, sizeof(*out));

    out->operand_base_a = keep_a;
    out->operand_base_b = keep_b;
    out->operand_b_size = keep_size;

    /* No UserFuncs chunk means no CALL can resolve, which is a real state on
     * the 17 maps that declare none — not an error. */
    if (!uf || events->record_count == 0)
        return Q2_OK;

    if (!q2_events_first_record(events, &rec))
        return Q2_OK;

    do {
        u32 i;

        for (i = 0; i < rec.n_items; i++) {
            q2_event_item item;
            q2_uf_call call;
            const u8 *p;
            u32 slot;
            s16 speed;
            u8  axis;

            if (!q2_events_get_item(events, &rec, i, &item))
                break;
            if (!item.payload || (item.opcode & Q2_EVOP_MASK) != Q2_EVOP_CALL)
                continue;

            /* Range and length faults are what the engine silently skips, so
             * skip them here too rather than failing the map. */
            if (q2_uf_decode_call(&call, uf, &item) != Q2_OK)
                continue;

            /* item.payload points past the two header bytes, so a documented
             * offset of +N is payload[N - 2]. Same convention as mover.c. */
            p = item.payload - 2;

            switch (call.prim) {
            case Q2_UF_SIMROT:
            case Q2_UF_SIMROT2:
                if (item.len < 24)
                    break;

                /* 0x8002867C: item+4 is the angular speed, into obj+0x3A.
                 * 0x80028664: item+20 & 3 is the axis, into obj+0x50 bits
                 * 14-15. Neither was in the operand table before this pass. */
                speed = q2_rd_s16(p + 4);
                axis  = (u8)(q2_rd_u16(p + 20) & 3u);

                /*
                 * Four object slots at +12, +14, +16, +18. A negative one is
                 * SKIPPED, not a terminator.
                 *
                 * `0x80028628` is `bltz v0, 0x8002875C`, and that target is the
                 * loop's own increment — `s4++`, `if (s4 < 4) loop` — so the
                 * constructor moves to the next slot and keeps going. This read
                 * it as a break, which is the difference between "the rest of
                 * this call is empty" and "this one slot is", and it silently
                 * discarded every rotator whose call had a gap before it.
                 */
                for (slot = 0; slot < 4; slot++) {
                    const u8 *q = operand_at(out, p, 24);
                    s16 node = q2_rd_s16(q + 12 + 2 * (s32)slot);
                    q2_rotator *r;

                    if (node < 0)
                        continue;
                    r = q2_rotators_add(out, Q2_ROT_ACCUM, node, axis, speed);
                    if (!r) {
                        q2_rotators_free(out);
                        return Q2_ERR_NO_MEMORY;
                    }
                    /* 0x800284C8..0x80028548 and
                     * 0x800286C0..0x80028740: both constructors rotate about
                     * the raw Scene-box centre, relative to Scene.origin. */
                    rotator_set_pivot(r, scene, NULL);
                }
                break;

            case Q2_UF_ROTHATCH: {
                q2_rotator *r;
                s16 node, target;

                if (item.len < 20)
                    break;

                node   = q2_rd_s16(operand_at(out, p, 20) + 18);
                target = q2_rd_s16(p + 6);
                if (node < 0)
                    break;

                /* 0x8002B6EC: the axis is a BYTE at item+8, not a halfword at
                 * +20 as SIMROT's is. */
                axis = (u8)(q2_rd_u8(p + 8) & 3u);

                /*
                 * 0x8002B70C: the magnitude comes from item+4 and the SIGN from
                 * which half of the circle the target is in — positive when the
                 * target is below 2048, negative otherwise, so the hatch always
                 * turns the short way round.
                 */
                speed = q2_rd_s16(p + 4);
                if (speed < 0)
                    speed = (s16)-speed;
                if (target >= 2048)
                    speed = (s16)-speed;

                r = q2_rotators_add(out, Q2_ROT_TARGET, node, axis, speed);
                if (!r) {
                    q2_rotators_free(out);
                    return Q2_ERR_NO_MEMORY;
                }
                r->target = target;
                {
                    /* The X adjustment is subtracted; Y and Z are added.
                     * Loads are `lhu` in retail, but the result is stored by
                     * `sh`, so reading these as authored s16 produces the same
                     * low halfword without obscuring their meaning. */
                    s32 hinge[3] = {
                        -(s32)q2_rd_s16(p + 10),
                         (s32)q2_rd_s16(p + 12),
                         (s32)q2_rd_s16(p + 14)
                    };
                    rotator_set_pivot(r, scene, hinge);
                }
                break;
            }

            case Q2_UF_ROTBUTTON: {
                q2_rotator *r;
                s16 node, hold;

                if (item.len < 12)
                    break;

                node = q2_rd_s16(operand_at(out, p, 12) + 10);
                if (node < 0)
                    break;

                /*
                 * Neither the axis nor the target is an operand. The exec at
                 * 0x8002BFD8 stores a literal 2048 and the handler at
                 * 0x8002C078 stores zero back, both to obj+0x0E — the Y slot.
                 */
                r = q2_rotators_add(out, Q2_ROT_SNAP, node, 1, 0);
                if (!r) {
                    q2_rotators_free(out);
                    return Q2_ERR_NO_MEMORY;
                }
                r->target = Q2_ROT_BUTTON_TARGET;
                /* 0x8002C210..0x8002C288: the button uses the same raw
                 * Scene-box midpoint as SIMROT, with no authored adjustment. */
                rotator_set_pivot(r, scene, NULL);

                hold = q2_rd_s16(p + 6);
                r->hold_reset = (hold < 0) ? Q2_UF_TIME_NEVER
                                           : (u16)(hold * Q2_UF_TIME_UNIT);
                break;
            }

            default:
                break;
            }
        }
    } while (q2_events_next_record(events, &rec, &rec));

    return Q2_OK;
}

void q2_rotators_free(q2_rotator_set *set)
{
    if (!set)
        return;
    free(set->rotators);
    memset(set, 0, sizeof(*set));
}

static void rotator_fire(q2_rotator *r)
{
    switch (r->kind) {
    case Q2_ROT_ACCUM:
        /* 0x8002DF08: `or v1, v1, a3` with a3 = 0x01000000. Setting an
         * already-set bit is a no-op, so two SIMROTs in one tick are one step. */
        r->step_pending = true;
        break;

    case Q2_ROT_TARGET:
        /* A hatch sweeps until it passes its target; re-triggering a moving one
         * changes nothing — including its sound, which is why the raise is
         * gated on the rotator not already running. 0x8002B3C8. */
        if (!r->running) {
            r->running        = true;
            r->sound_pending  = Q2_ROTSND_START;
        }
        break;

    case Q2_ROT_SNAP:
        /* 0x8002BFD8: pressed, the angle IS the target immediately. */
        r->angle = r->target;
        r->hold  = r->hold_reset;
        /*
         * AND IT CLICKS. 0x8002BFB8 loads amb_butn2 (0x800B27E4) and plays it
         * through 0x80073704 at 0x8002BFCC, positioned at the midpoint of the
         * object's two node bounds. This is the DOOR SWITCH, and the port
         * raised nothing at all: 58 ROTBUTTON items on the disc against the 3
         * BUTTON items that were its only source of this sound, so a player
         * walked up to a switch, pressed it in silence, and then heard the door.
         */
        r->sound_pending = Q2_ROTSND_BUTTON;
        break;
    }
}

void q2_rotator_trigger(q2_rotator_set *set, u32 index)
{
    if (!set || index >= set->count)
        return;
    rotator_fire(&set->rotators[index]);
}

u32 q2_rotator_trigger_node(q2_rotator_set *set, u32 node)
{
    u32 i, fired = 0;

    if (!set)
        return 0;

    for (i = 0; i < set->count; i++)
        if (set->rotators[i].node >= 0 && (u32)set->rotators[i].node == node) {
            rotator_fire(&set->rotators[i]);
            fired++;
        }

    return fired;
}

u32 q2_rotators_call(q2_rotator_set *set, const q2_userfuncs *uf,
                     const q2_event_item *item, u8 call_index)
{
    const u8 *p;
    u32 slot, made = 0;
    s16 node;

    if (!set || !uf || !item || !item->payload)
        return 0;

    /* item.payload points past the two header bytes, so a documented offset of
     * +N is payload[N - 2]. Same convention as the builder above. */
    p = item->payload - 2;

    switch (q2_userfuncs_prim(uf, call_index)) {
    case Q2_UF_SIMROT:
    case Q2_UF_SIMROT2:
        if (item->len < 24)
            break;
        for (slot = 0; slot < 4; slot++) {
            const u8 *q = operand_at(set, p, 24);
            node = q2_rd_s16(q + 12 + 2 * (s32)slot);
            if (node < 0)
                continue;           /* 0x80028628 SKIPS an empty slot */
            made += q2_rotator_trigger_node(set, (u32)node);
        }
        break;

    case Q2_UF_ROTHATCH:
        if (item->len < 20)
            break;
        node = q2_rd_s16(operand_at(set, p, 20) + 18);
        if (node >= 0) {
            made += q2_rotator_trigger_node(set, (u32)node);
        }
        break;

    case Q2_UF_ROTBUTTON:
        if (item->len < 12)
            break;
        node = q2_rd_s16(operand_at(set, p, 12) + 10);
        if (node >= 0) {
            made += q2_rotator_trigger_node(set, (u32)node);
        }
        break;

    default:
        break;
    }

    return made;
}

u32 q2_rotators_tick(q2_rotator_set *set, s32 dt)
{
    u32 i, moved = 0;

    if (!set)
        return 0;

    for (i = 0; i < set->count; i++) {
        q2_rotator *r = &set->rotators[i];

        switch (r->kind) {
        case Q2_ROT_ACCUM:
            /* 0x8002F1B8: nothing happens without a pending step. */
            if (!r->step_pending)
                continue;

            /* 0x8002F1D0 - 0x8002F1F8. The accumulator is deliberately not
             * wrapped: the original does not wrap it either, and the angle is
             * taken from its middle bits, so wrapping would introduce a
             * discontinuity the original does not have. */
            r->accum += (s32)r->speed * dt;
            r->angle  = (s16)((r->accum >> Q2_ROT_ACCUM_SHIFT)
                              & Q2_ROT_ANGLE_MASK);

            /* 0x8002F204: the step is consumed. One request, one step. */
            r->step_pending = false;
            moved++;
            break;

        case Q2_ROT_TARGET: {
            s32 step;

            if (!r->running)
                continue;

            /*
             * 0x8002B460 - 0x8002B490. The step is (speed * dt) / 8 with the
             * division rounded TOWARD ZERO — `bgez; addiu 7; sra 3` — which is
             * not what a plain arithmetic shift does for a closing hatch, and
             * getting it wrong makes one direction creep by a step per frame.
             */
            step = (s32)r->speed * dt;
            if (step < 0)
                step += 7;
            step >>= 3;

            r->angle = (s16)((r->angle + step) & Q2_ROT_ANGLE_MASK);

            /*
             * 0x8002B48C splits on the direction of travel and each arm tests
             * whether the angle has gone past the target. Comparing on the
             * wrapped angle is the original's own behaviour.
             */
            if (r->speed > 0 ? (r->angle > r->target)
                             : (r->angle < r->target)) {
                r->angle   = r->target;
                r->running = false;
                /* 0x8002B534: arrival stops the loop and plays pt1__end. */
                r->sound_pending = Q2_ROTSND_END;
            }
            /*
             * THE MID LOOP IS DECODED AND DELIBERATELY NOT RAISED YET.
             *
             * 0x8002B3DC plays pt1__mid through the LOOPING entry point
             * 0x80073734 and 0x8002B534 stops it with 0x8007398C, so the motor
             * runs for exactly as long as the hatch turns. This port can start
             * such a voice — the VAG decoder honours the SPU's End|Repeat flag
             * and pt1__mid is a 0.23 s loop body — but it has NO WAY TO STOP
             * ONE: the mixer has no per-voice handle and `client_voices_stop`
             * is all-or-nothing (vag.h says the same thing about scripted
             * loops). Starting it would leave the motor running for the rest of
             * the level, which is worse than the silence it replaces.
             *
             * So Q2_ROTSND_MID exists, is named and is wired through
             * `q2_rotator_take_sound`, and nothing raises it until the mixer
             * grows a stop. START and END are both correct and both play.
             */
            moved++;
            break;
        }

        case Q2_ROT_SNAP:
            /* 0x8002C054: the hold counts down by dt, and 0x8002C078 puts the
             * angle back to zero when it runs out. Q2_UF_TIME_NEVER holds. */
            if (r->hold == 0 || r->hold == Q2_UF_TIME_NEVER)
                continue;

            if ((s32)r->hold > dt) {
                r->hold = (u16)(r->hold - dt);
            } else {
                r->hold  = 0;
                r->angle = 0;
                /* 0x8002C11C: the release plays amb_butn2 a second time, from
                 * the handler 0x8002C020 that puts the angle back. A switch
                 * clicks twice — down and up. */
                r->sound_pending = Q2_ROTSND_BUTTON;
                moved++;
            }
            break;
        }
    }

    return moved;
}

bool q2_rotators_node_transform(const q2_rotator_set *set, u32 node,
                                s16 angles[3], s16 pivot[3])
{
    u32 i;
    int c;

    for (c = 0; c < 3; c++) {
        if (angles) angles[c] = 0;
        if (pivot)  pivot[c]  = 0;
    }

    if (!set)
        return false;

    for (i = 0; i < set->count; i++) {
        const q2_rotator *r = &set->rotators[i];

        if (r->node < 0 || (u32)r->node != node)
            continue;

        /* Only one of the three Euler slots is ever written — the integrator
         * stores to obj[0x0C + 2*axis] and nothing clears the other two, which
         * is why a node turns about exactly one axis. */
        if (angles)
            angles[r->axis & 3] = r->angle;
        if (pivot) {
            pivot[0] = r->pivot[0];
            pivot[1] = r->pivot[1];
            pivot[2] = r->pivot[2];
        }
        return true;
    }

    return false;
}
