#include "mover.h"

#include <stdlib.h>
#include <string.h>

/* The bank keys, registered by name at 0x8002D4E0..0x8002D800. */
const char *const q2_mover_sound_name[Q2_MVSND_COUNT] = {
    "msc_keyuse",   /* 0x800B27CC */
    "msc_keytry",   /* 0x800B27D0 */
    "pt1__strt",    /* 0x800B27D8 */
    "amb_butn2"     /* 0x800B27E4 */
};

/* Ask for a sound, unless this family has none. A BUTTON has exactly one and
 * uses it for everything; PISTON and DISH have none at all. */
static void mover_sound(q2_mover *m, s8 which)
{
    if (m->silent)
        return;
    if (m->is_button)
        which = Q2_MVSND_BUTTON;
    m->sound_pending = which;
}

/*
 * The train's running sound, 0x8002C778 — asked for on EVERY tick the platform
 * is in a moving state, blocked included, because the call sits at the top of
 * the shared motion path and not behind the step.
 *
 * The arrival sound wins when both land on one tick: the console makes the
 * arrival call second (0x8002C8D8) and this field holds one id.
 */
static void mover_running(q2_mover *m)
{
    if (m->is_path)
        m->travel_sound = Q2_MOVER_TRAVEL_MOVE_ID;
}

/*
 * Snapshot the authored timers, once the build has finished decoding them.
 *
 * Run at the end of a build rather than beside each of the fourteen places
 * that write a timer, so a primitive added later cannot forget to do it.
 */
static void mover_arm_resets(q2_mover_set *set)
{
    u32 i;

    if (!set)
        return;
    for (i = 0; i < set->count; i++) {
        set->movers[i].delay_reset = set->movers[i].delay_timer;
        set->movers[i].wait_reset  = set->movers[i].wait_timer;
    }
}

s8 q2_mover_take_sound(q2_mover_set *set, u32 index)
{
    s8 s;

    if (!set || index >= set->count)
        return Q2_MVSND_NONE;
    s = set->movers[index].sound_pending;
    set->movers[index].sound_pending = Q2_MVSND_NONE;
    return s;
}

u8 q2_mover_take_travel_sound(q2_mover_set *set, u32 index)
{
    u8 s;

    if (!set || index >= set->count)
        return 0;
    s = set->movers[index].travel_sound;
    set->movers[index].travel_sound = 0;
    return s;
}


/* ------------------------------------------------------------------------- */
/* Building movers from the script                                            */
/* ------------------------------------------------------------------------- */
static q2_mover *mover_push(q2_mover_set *set)
{
    if (set->count >= set->capacity) {
        u32 want = set->capacity ? set->capacity * 2 : 32;
        q2_mover *bigger = (q2_mover *)realloc(set->movers, want * sizeof(q2_mover));
        if (!bigger)
            return NULL;
        set->movers   = bigger;
        set->capacity = want;
    }

    {
        q2_mover *m = &set->movers[set->count++];
        memset(m, 0, sizeof(*m));
        m->portal_node = -1;
        m->partner     = -1;
        m->wait_timer  = Q2_MOVER_WAIT_NEVER;
        /* Every normal mover constructor allocates its +0x28 pusher. PISTON
         * overwrites this from item[+18]'s explicit gate below. */
        m->blocks_player = 1;
        /* A mover starts fully closed, so its portal starts sealed. */
        m->sealed      = 1;
        m->sound_pending = Q2_MVSND_NONE;
        /* Not a primitive: the MOVER_A/B/C opcodes build these, and only the
         * CALL path overwrites it. Zero would read as the first table row. */
        m->prim        = Q2_MOVER_PRIM_OPCODE;
        return m;
    }
}

/* Copy up to four Scene node indices, dropping the -1 holes. */
static void collect_nodes(q2_mover *m, const u8 *payload, u32 at)
{
    int i;

    m->part_count = 0;
    for (i = 0; i < Q2_MOVER_MAX_PARTS; i++) {
        s16 n = q2_rd_s16(payload + at + (u32)i * 2);
        if (n < 0)
            continue;
        m->node[m->part_count++] = n;
    }
}

q2_result q2_movers_build(q2_mover_set *out, const q2_events *events,
                          const q2_uf_operands *ops)
{
    q2_event_record rec;

    if (!out || !events)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    if (events->record_count == 0)
        return Q2_OK;

    if (!q2_events_first_record(events, &rec))
        return Q2_OK;

    do {
        u32 i;

        for (i = 0; i < rec.n_items; i++) {
            q2_event_item item;
            const u8 *p, *q;
            u32 first;

            if (!q2_events_get_item(events, &rec, i, &item))
                break;
            if (!item.payload)
                continue;

            /* item.payload points past the two header bytes, so a documented
             * offset of +N is payload[N - 2]. Keep the disc offsets in the
             * code and subtract once, rather than pre-subtracting and losing
             * the correspondence with the header. */
            p = item.payload - 2;

            /*
             * AND THE OBJECT SLOTS COME FROM THE OTHER BUFFER.
             *
             * `0x80029794` is explicit about it: it sets s2 = item+8 in
             * COMMON's copy, forms s3 = gp+376 + (s2 - gp+372) at
             * 0x80029824/0x80029828, WRITES -1 through s2 at 0x80029850 and
             * then READS the node index through s3 at 0x80029854. COMMON.DAT's
             * Events is a snapshot that agrees with ZONE0's only; every zone
             * above zero carries its own slot values, and reading COMMON's
             * built a door with no parts and animated whichever nodes ZONE0
             * happened to name.
             *
             * ONLY the slot cursor rebases. 0x80025D70-0x80025E30 takes
             * travel, speed, the key mask and the two timers from the record
             * it is walking; the pristine copy is consulted for the portal
             * node at +6 and the node array alone. `q` is that copy.
             */
            q = q2_uf_operand_at(ops, p, item.len);

            /* Where this item's movers start, so its offset can be stamped on
             * whatever the switch pushes — one for A and B, two for C. */
            first = out->count;

            switch (item.opcode) {
            case Q2_EVOP_MOVER_A: {
                q2_mover *m;
                if (item.len < 24)
                    break;
                m = mover_push(out);
                if (!m)
                    return Q2_ERR_NO_MEMORY;

                m->axis        = 1;                          /* hard-wired */
                m->target      = (s16)-q2_rd_s16(p + 2);
                m->speed       = (s16)abs(q2_rd_s16(p + 4));
                m->portal_node = q2_rd_s16(q + 6);
                collect_nodes(m, q, 8);
                m->key_mask    = q2_rd_u16(p + 16);
                m->delay_timer = (u16)(p[18] * Q2_MOVER_TIMEBASE);
                m->wait_timer  = (p[19] == 0xFF)
                                 ? Q2_MOVER_WAIT_NEVER
                                 : (u16)(p[19] * Q2_MOVER_TIMEBASE);
                m->touch_opens = q2_rd_s16(p + 20) != 0;
                /* A opens through obstructions but not closes. */
                m->block_flags = Q2_MV_BLK_IGNORE_OPENING;
                break;
            }

            case Q2_EVOP_MOVER_B: {
                q2_mover *m;
                s16 axis_field;
                if (item.len < 24)
                    break;
                m = mover_push(out);
                if (!m)
                    return Q2_ERR_NO_MEMORY;

                axis_field     = q2_rd_s16(p + 8);
                m->axis        = (u8)(axis_field & 3);
                m->target      = (axis_field == 0)
                                 ? q2_rd_s16(p + 2)
                                 : (s16)-q2_rd_s16(p + 2);
                m->speed       = (s16)abs(q2_rd_s16(p + 4));
                m->portal_node = q2_rd_s16(q + 6);
                collect_nodes(m, q, 10);
                m->key_mask    = q2_rd_u16(p + 18);
                m->delay_timer = (u16)(p[20] * Q2_MOVER_TIMEBASE);
                m->wait_timer  = (p[21] == 0xFF)
                                 ? Q2_MOVER_WAIT_NEVER
                                 : (u16)(p[21] * Q2_MOVER_TIMEBASE);
                break;
            }

            case Q2_EVOP_MOVER_C: {
                q2_mover *leaf0, *leaf1;
                s16 travel, speed, portal, axis_field;
                u16 keys;
                u8  delay, wait;
                u32 i0;

                if (item.len < 32)
                    break;

                travel     = q2_rd_s16(p + 2);
                speed      = (s16)abs(q2_rd_s16(p + 4));
                portal     = q2_rd_s16(q + 6);
                axis_field = q2_rd_s16(p + 8);

                /*
                 * THE AXIS FIELD ALSO FLIPS THE TRAVEL, and missing that is why
                 * BASE1's first door opened with its two leaves swapped.
                 *
                 * 0x8002664C reads the FULL SIGNED halfword at item[+8] and
                 * branches on it being non-zero:
                 *
                 *     8002664C  lh   v0, 8(s0)         ; the axis field
                 *     80026654  beq  v0, zero, +0x14   ; zero: take it as-is
                 *     8002665C  lhu  v0, 2(s0)
                 *     80026664  subu v0, zero, v0      ; else NEGATE item[+2]
                 *     80026670  sh   v0, 68(s1)        ; -> obj+0x44, the travel
                 *
                 * and leaf 1's copy is then negated again at
                 * 0x80026690..0x8002669C, which is the part the port already
                 * had. Taking `travel` unnegated for leaf 0 therefore reversed
                 * BOTH panels on every door whose axis field is non-zero: each
                 * leaf drove into the other's half of the doorway.
                 *
                 * Note the port reads the axis as `axis_field & 3` — the same
                 * halfword serving two purposes, which is why its SIGN survives
                 * to mean this.
                 */
                if (axis_field != 0)
                    travel = (s16)-travel;
                keys       = q2_rd_u16(p + 26);
                delay      = p[28];
                wait       = p[29];

                leaf0 = mover_push(out);
                if (!leaf0)
                    return Q2_ERR_NO_MEMORY;
                i0 = out->count - 1;

                leaf0->axis        = (u8)(axis_field & 3);
                leaf0->target      = travel;
                leaf0->speed       = speed;
                leaf0->portal_node = -1;      /* leaf 1 owns the portal */
                collect_nodes(leaf0, q, 10);
                leaf0->key_mask    = keys;
                leaf0->delay_timer = (u16)(delay * Q2_MOVER_TIMEBASE);
                leaf0->wait_timer  = (wait == 0xFF)
                                     ? Q2_MOVER_WAIT_NEVER
                                     : (u16)(wait * Q2_MOVER_TIMEBASE);

                leaf1 = mover_push(out);
                if (!leaf1)
                    return Q2_ERR_NO_MEMORY;

                /* mover_push may have reallocated, so re-take leaf0. */
                leaf0 = &out->movers[i0];

                leaf1->axis        = leaf0->axis;
                leaf1->target      = (s16)-travel;   /* opposite leaf */
                leaf1->speed       = speed;
                leaf1->portal_node = portal;
                collect_nodes(leaf1, q, 18);
                leaf1->key_mask    = keys;
                leaf1->delay_timer = leaf0->delay_timer;
                leaf1->wait_timer  = leaf0->wait_timer;
                leaf1->partner     = (s32)i0;
                leaf0->partner     = (s32)(out->count - 1);
                break;
            }

            default:
                break;
            }

            for (; first < out->count; first++)
                out->movers[first].item_offset = item.offset;
        }
    } while (q2_events_next_record(events, &rec, &rec));

    mover_arm_resets(out);
    return Q2_OK;
}

/* The one legal item length for each of the three, from the operand table. */
static u32 info_len(q2_uf_prim prim)
{
    const q2_uf_prim_info *i = q2_uf_info(prim);
    return i ? i->item_len : 0xFFFFu;
}

/*
 * PLATFORM's PATH: the vector from the group's first node to the `origin`
 * operand, and its length.
 *
 * `0x8002CC98`..`0x8002CCE8` reads the node record's `+16/+28`, `+20/+32` and
 * `+24/+36` — the two bbox corners — halves each pair, subtracts it from the
 * item's VEC3 at `+4`, squares and sums; then `(sum >> 2)` goes through the
 * integer square root at `0x80055CBC` and the result is doubled
 * (`sll s4, v0, 1`). `isqrt(n/4) * 2` is `isqrt(n)`: the halving is to keep the
 * intermediate in range, not part of the answer.
 *
 * THE THREE DIFFERENCES ARE KEPT, and that is the whole of what a train is.
 * They are not intermediates the console throws away after squaring them —
 * `0x8002CDD0`, `0x8002CDDC` and `0x8002CDEC` write all three to obj+0x00,
 * +0x04 and +0x08 as full 32-bit words, and the per-frame handler divides them
 * by the length at `0x8002C930` to get a displacement on every axis.
 *
 * This function used to return the length alone, under a comment reasoning that
 * "the DIRECTION is not in this operand" and that a positive target must
 * therefore mean +Y. It is in this operand; it is the part of it that was being
 * discarded three instructions before the answer.
 */
static s32 platform_isqrt(s32 x)
{
    /* 0x80055CBC, bit by bit. Negative cannot reach here — the caller does the
     * console's own `bgez` first. */
    u32 v = (u32)x, r = 0, bit = 1u << 30;

    if (x <= 0)
        return 0;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (s32)r;
}

static void platform_path(const q2_scene *scene, s16 node, const u8 *item,
                          s32 dir_out[3], s16 *len_out)
{
    q2_scene_node n;
    u32 sum = 0;
    s32 root;
    int k;

    for (k = 0; k < 3; k++)
        dir_out[k] = 0;
    *len_out = 0;

    if (!scene || node < 0 || !q2_scene_get_node(scene, (u32)node, &n))
        return;

    for (k = 0; k < 3; k++) {
        s32 centre = (s32)(((s64)n.bbox_min[k] + n.bbox_max[k]) / 2);
        s32 d      = q2_rd_s32(item + 4 + 4 * k) - centre;

        dir_out[k] = d;
        /* 32-bit, wrapping, because the console's `mult`/`mflo` pair is: the
         * squares and their sum are single registers and a long enough path
         * carries out of one. */
        sum += (u32)d * (u32)d;
    }

    /*
     * `isqrt(n / 4) * 2`, and the division is the compiler's signed one —
     * `bgez; addiu 3; sra 2` at 0x8002CCEC. The halving keeps the intermediate
     * inside a 32-bit `mult` and the doubling puts it back; it is not part of
     * the answer.
     */
    {
        s32 quarter = (s32)sum;

        quarter = (quarter < 0 ? quarter + 3 : quarter) >> 2;
        root = platform_isqrt(quarter) * 2;
    }

    /*
     * AND THEN IT IS TRUNCATED TO SIXTEEN BITS, which is not a detail.
     *
     * `sh s4, 68(s0)` (0x8002CDB4) drops the top half, and the handler reads it
     * back with `lh` and takes `abs()` of it on every single frame
     * (0x8002C794..0x8002C7A0) — which only makes sense for a field that is
     * expected to arrive negative. BIGGUN's path is 33,020 units long; stored
     * as a halfword that is -32,516, and the platform runs against a target of
     * 32,516.
     *
     * The endpoint is unharmed, because the displacement divides by the same
     * number it counts up to: at `progress == target` the scale is exactly one
     * whatever the target is. What it changes is the DURATION — the ride is
     * 1.5% shorter than its own geometry — and a port that clamped to 32,767
     * instead, as this one did, got that wrong in the other direction.
     */
    root = (s16)(u16)((u32)root & 0xFFFFu);
    if (root < 0)
        root = -root;
    if (root > 32767)
        root = 32767;   /* only 0x8000 reaches here; the console leaves it
                         * negative, which is a train that never arrives. */

    *len_out = (s16)root;
}

/*
 * Where a mover has displaced its parts to, on all three axes.
 *
 * An axis mover puts `offset` on `axis`, which is the s16 the console keeps at
 * obj+0x12 + 2*axis. A train scales its direction vector by how far along it
 * is, which is `0x8002C914`..`0x8002C9F0`: three `mult` / `div` pairs against
 * obj+0x44, each truncated to s16 by the `sh` that stores it.
 *
 * The 32-bit truncation between the multiply and the divide is deliberate. The
 * console's `mflo` takes the low word of a 32x32 product and hands it to a
 * 32-bit `div`, so a long enough path wraps — reproduced here rather than
 * widened, because a divergence that only appears on one map is worse than a
 * wrap that appears on both.
 */
void q2_mover_displacement(const q2_mover *m, s32 out[3])
{
    int k;

    if (!out)
        return;

    out[0] = out[1] = out[2] = 0;
    if (!m)
        return;

    if (!m->is_path) {
        out[m->axis < 3u ? m->axis : 1u] = m->offset;
        return;
    }

    if (m->target == 0)
        return;

    for (k = 0; k < 3; k++) {
        s32 prod = (s32)(u32)((u64)(s64)m->dir[k] * (u64)(s64)m->offset);

        out[k] = (s16)(prod / m->target);
    }
}

q2_result q2_movers_build_calls(q2_mover_set *out, const q2_events *events,
                                const q2_userfuncs *uf,
                                const q2_uf_operands *ops,
                                const q2_scene *scene)
{
    q2_event_record rec;

    if (!out || !events || !uf)
        return Q2_ERR_INVALID_ARG;

    if (events->record_count == 0 || !q2_events_first_record(events, &rec))
        return Q2_OK;

    do {
        u32 i;

        for (i = 0; i < rec.n_items; i++) {
            q2_event_item item;
            q2_uf_call    call;
            const u8     *p;
            q2_mover     *m;

            if (!q2_events_get_item(events, &rec, i, &item))
                break;
            if (!item.payload)
                continue;
            if ((item.opcode & Q2_EVOP_MASK) != Q2_EVOP_CALL)
                continue;
            if (q2_uf_decode_call(&call, uf, &item) != Q2_OK)
                continue;
            if (call.prim != Q2_UF_LIFT1 &&
                call.prim != Q2_UF_CAGELIFT1 &&
                call.prim != Q2_UF_BUTTON &&
                call.prim != Q2_UF_PISTON &&
                call.prim != Q2_UF_DISH &&
                call.prim != Q2_UF_PLATFORM)
                continue;
            if (item.len < info_len(call.prim))
                continue;

            /* The operands, rebased the way a rotation call's are: an item the
             * game has already run reads -1 in COMMON's copy and lives at the
             * same offset in the zone's (#56). */
            /* Ask the rebase for the item's OWN length, not a constant 20:
             * DISH's item is eight bytes and asking for twenty would refuse to
             * rebase it and read the -1 in COMMON's copy instead. */
            p = q2_uf_operand_at(ops, item.payload - 2, item.len);

            m = mover_push(out);
            if (!m)
                return Q2_ERR_NO_MEMORY;

            m->axis        = 1;      /* all three of these are vertical */
            m->prim        = (u8)call.prim;
            m->item_offset = item.offset;
            /*
             * NO BLANKET "IGNORE OBSTRUCTION". This used to set
             * Q2_MV_BLK_IGNORE_OPENING on every CALL primitive, which is
             * MOVER_A's property and nobody else's: on the console a lift, a
             * cage lift or a platform that touches the player STOPS, and here
             * they pushed straight through whatever was in the way. Left at the
             * zero `mover_push` already memsets, exactly as MOVER_B and MOVER_C
             * are.
             */

            switch (call.prim) {
            case Q2_UF_LIFT1:
                m->target      = (s16)-(s16)q2_rd_u16(p + 4);
                m->speed       = (s16)abs(q2_rd_s16(p + 6));
                collect_nodes(m, p, 8);
                m->delay_timer = (u16)(p[16] * Q2_MOVER_TIMEBASE);
                m->wait_timer  = (p[17] == 0xFF)
                                 ? Q2_MOVER_WAIT_NEVER
                                 : (u16)(p[17] * Q2_MOVER_TIMEBASE);
                break;

            case Q2_UF_CAGELIFT1:
                /* LIFT1's constructor operand for operand — see the correction
                 * in userfuncs.c. Its two slab bytes move both timers two
                 * bytes later than LIFT1's. The exec at 0x8002DFF4 reads +18
                 * into obj+0x4C (delay), then +19 into obj+0x4E (wait), with
                 * 0xFF taking the never-return arm at 0x8002E088. Reading +18
                 * as the wait made BASE2's 0/0xFF pair return immediately. */
                m->target      = (s16)-(s16)q2_rd_u16(p + 4);
                m->speed       = (s16)abs(q2_rd_s16(p + 6));
                collect_nodes(m, p, 8);
                m->delay_timer = (u16)(p[18] * Q2_MOVER_TIMEBASE);
                m->wait_timer  = (p[19] == 0xFF)
                                 ? Q2_MOVER_WAIT_NEVER
                                 : (u16)(p[19] * Q2_MOVER_TIMEBASE);
                /*
                 * The two slab thicknesses, which nothing read. 0x80029A78 and
                 * 0x80029B1C build a ceiling of item[+17] and a floor of
                 * item[+16] out of one box; without them the cage registers
                 * solid and cannot be entered. See q2_mover.cage_top.
                 */
                m->cage_top    = p[17];
                m->cage_bottom = p[16];
                break;

            case Q2_UF_BUTTON: {
                /* Its handler 0x80029C28 plays amb_butn2 (0x80029E04) and
                 * references none of the other five handles. */
                m->is_button = 1;
                /*
                 * `travel`'s SIGN selects obj+0x3A = +1 or -1 and its magnitude
                 * goes to obj+0x44 — so a button's speed is literally one unit
                 * a tick and its target is the travel. `invert` negates the
                 * target. Both are userfuncs.c's wording, not a reading of it.
                 */
                s16 travel = q2_rd_s16(p + 14);
                s16 mag    = (s16)abs(travel);

                m->target     = (s8)p[4] ? (s16)-mag : mag;
                m->speed      = 1;
                m->node[0]    = q2_rd_s16(p + 12);
                m->part_count = (m->node[0] >= 0) ? 1 : 0;
                m->wait_timer = (u16)(q2_rd_u16(p + 8) * Q2_MOVER_TIMEBASE);
                break;
            }

            case Q2_UF_PLATFORM: {
                /*
                 * THE TRAIN. Four object slots at +20 like every other group
                 * primitive — the constructor's loop at 0x8002CD3C runs `s1`
                 * from 0 to 3 and steps its slot cursor by two each time — and
                 * the path taken from the FIRST of them, which is the node
                 * 0x8002CC24 reads before that loop starts.
                 *
                 * Reading one slot cost BIGGUN's platform two of its three
                 * parts: the item names Scene nodes 31, 32 and 30, and 32 and
                 * 30 were left standing while 31 rode off without them.
                 */
                s32 dir[3];
                s16 len;

                collect_nodes(m, p, 20);
                platform_path(scene, m->part_count ? m->node[0] : (s16)-1,
                              p, dir, &len);
                m->dir[0] = dir[0];
                m->dir[1] = dir[1];
                m->dir[2] = dir[2];
                m->target = len;
                m->is_path = 1;
                m->speed       = (s16)abs(q2_rd_s16(p + 18));
                m->delay_timer = p[28];          /* UNSCALED, per the table */
                m->wait_timer  = (p[29] == 0xFF)
                                 ? Q2_MOVER_WAIT_NEVER
                                 : (u16)(p[29] * Q2_MOVER_TIMEBASE);
                break;
            }

            case Q2_UF_DISH:
                /* Neither DISH's constructor (0x8002A880) nor PISTON's
                 * (0x8002D114) installs 0x80025658, and neither references any
                 * of the six sound handles: silent, deliberately. */
                m->silent = 1;
                /*
                 * The speed is not authored: it is the immediate ONE, written
                 * by the exec at `0x8002E314` (`addiu v1, zero, 1; sh v1,
                 * 58(a0)`) exactly as a button's is. #81 read the operand table,
                 * found no speed in it and concluded the primitive needed more
                 * reading — which was right about the table and wrong about the
                 * conclusion. There is no operand because there is no choice.
                 *
                 * The rest is `0x8002E2A0`: the object slot is at +6, the travel
                 * is `(s8)item[+5] << 5` into obj+0x44, obj+0x52 is a one-shot
                 * latch tested before anything else, and obj+0x4E is set to the
                 * clock plus 300 — one second, which is the wait.
                 */
                m->node[0]     = q2_rd_s16(p + 6);
                m->part_count  = (m->node[0] >= 0) ? 1 : 0;
                m->target      = (s16)((s16)(s8)p[5] << 5);
                m->speed       = 1;
                m->wait_timer  = Q2_MOVER_TIMEBASE;
                break;

            case Q2_UF_PISTON:
                m->silent = 1;          /* see DISH above */
                /* 0x8002D114 copies the low two axis bits, then walks ALL
                 * four slots. Its signed +18 word is a pusher gate: only a
                 * non-zero value calls 0x800555D8 and stores the resulting
                 * object at +0x28 for the per-frame 0x80051EC0 call. */
                m->axis       = p[4] & 3u;
                m->speed      = p[5];
                m->target     = q2_rd_s16(p + 6);
                collect_nodes(m, p, 8);
                m->wait_timer = q2_rd_u16(p + 16);
                m->blocks_player = q2_rd_s16(p + 18) != 0;
                break;

            default:
                break;
            }

            /*
             * BASE0's CRATES is not a malformed lift. Its zero target/speed
             * are deliberate because the LevelBin's DOCRATES handler owns the
             * four obj+0x14 displacements. Keep one port object per authored
             * slot — not one group — because the first pair moves at 16 and
             * the second at 20. The retail constructor allocates four separate
             * 92-byte objects too.
             */
            if (call.prim == Q2_UF_LIFT1 && m->target == 0 && m->speed == 0 &&
                m->part_count != 0) {
                q2_mover prototype = *m;
                s16 nodes[Q2_MOVER_MAX_PARTS];
                u32 parts = m->part_count;
                u32 part;

                memcpy(nodes, m->node, parts * sizeof(nodes[0]));
                prototype.external   = 1;
                prototype.part_count = 1;
                prototype.sealed     = 0;

                for (part = 0; part < parts; part++) {
                    q2_mover *one;

                    if (part == 0)
                        one = &out->movers[out->count - 1];
                    else {
                        one = mover_push(out);
                        if (!one)
                            return Q2_ERR_NO_MEMORY;
                    }

                    *one = prototype;
                    one->node[0]       = nodes[part];
                    one->external_part = (u8)part;
                }
                continue;
            }

            /*
             * A mover with no speed never arrives and one with no node moves
             * nothing. Either is a decode this port should not keep — and
             * saying WHICH is dropped is the measurement, because an empty
             * object slot and a primitive this port cannot build look identical
             * in a count. #81 is the entry that made that point.
             */
            if (m->speed == 0 || m->part_count == 0) {
                const q2_uf_prim_info *pi = q2_uf_info(call.prim);

                /*
                 * Say WHICH of the two, and do not print `node[0]` when there
                 * are none: a dropped mover's node field is the zero the push
                 * left, and printing it reads as "node 0" — a real index — for
                 * a mover whose four object slots were all -1.
                 */
                /*
                 * "No object slot resolves" is USUALLY NOT A FAULT and used to
                 * be reported as one. A CALL-built lift belonging to another
                 * zone genuinely has four -1 slots in the copy this zone
                 * carries — BASE1's CAGELIFT1 at item offset 704 is one, and
                 * its nodes (215 and 216) are sitting in ZONE1's copy where
                 * they belong. Every zone load printed one of these per
                 * out-of-zone lift and it read as a decode failure.
                 *
                 * Speed 0 IS a fault: such a mover triggers, ticks, and never
                 * arrives, so it keeps the loud level.
                 */
                if (m->part_count == 0)
                    Q2_DEBUG("mover %s not in this zone: all four object slots "
                             "are -1 (speed %d, target %d)",
                             pi ? pi->name : "?", m->speed, m->target);
                else
                    Q2_INFO("mover dropped: %s — speed 0, it would never "
                            "arrive (target %d)",
                            pi ? pi->name : "?", m->target);
                out->count--;
            }
        }
    } while (q2_events_next_record(events, &rec, &rec));

    mover_arm_resets(out);
    return Q2_OK;
}

void q2_movers_free(q2_mover_set *set)
{
    if (!set)
        return;
    free(set->movers);
    memset(set, 0, sizeof(*set));
}

/* ------------------------------------------------------------------------- */
u32 q2_movers_trigger_item(q2_mover_set *set, u32 item_offset)
{
    u32 i, n = 0;

    if (!set)
        return 0;

    for (i = 0; i < set->count; i++) {
        if (set->movers[i].item_offset != item_offset)
            continue;
        q2_mover_trigger(set, i);
        n++;
    }

    return n;
}

void q2_mover_trigger(q2_mover_set *set, u32 index)
{
    q2_mover *m;

    if (!set || index >= set->count)
        return;

    m = &set->movers[index];
    m->triggered = 1;

    /*
     * 0x8002752C..0x800275CC, in that order.
     *
     * A closing door reverses at once rather than waiting for the next tick,
     * and that arm does NOT re-arm the timers — a door caught on its way shut
     * carries on with what it had. An IDLE one reloads both from the item's own
     * bytes, and anything else (already opening, holding open, waiting out its
     * delay) is left alone.
     *
     * THE RELOAD IS WHY THIS IS HERE. `delay_timer` and `wait_timer` are
     * counted down destructively by the state machine, and they used to be
     * written once at build time and never again — so a door's pre-open delay
     * and its hold-open time were spent on the first use and every use after
     * that was instant. That is the "movers don't feel like retail" report:
     * the first trigger in a level behaves and none of the rest do.
     */
    /*
     * A TRAIN DOES NOT REVERSE, and that arm belongs to MOVER_A rather than to
     * movers in general. 0x8002757C tests the state for 3 and branches to the
     * reversal at 0x80027630; PLATFORM's own exec at 0x8002E8C4 has no such
     * test — it sets the trigger bit, and if the state is anything but 0 it
     * returns. Re-triggering a platform on its way back does nothing until it
     * has finished getting there.
     */
    if (m->state == Q2_MV_CLOSING && !m->is_path) {
        m->state = Q2_MV_OPENING;
    } else if (m->state == Q2_MV_IDLE) {
        m->delay_timer = m->delay_reset;
        m->wait_timer  = m->wait_reset;
    }
}

/* ------------------------------------------------------------------------- */
/* The state machine                                                          */
/* ------------------------------------------------------------------------- */
/*
 * The displacement `mover_move` is about to apply along the mover's own axis,
 * clamped to the travel exactly as the move itself clamps it. Split out so the
 * obstruction test can ask "would this step run into something" before the
 * step happens, which is the order 0x80025CBC works in.
 */
static s32 mover_step(const q2_mover *m, s32 dt, int dir)
{
    s32 step = m->speed * dt;
    s32 to;

    if (dir)
        to = (m->target > 0) ? (m->offset + step) : (m->offset - step);
    else
        to = (m->target > 0) ? (m->offset - step) : (m->offset + step);

    if (dir) {
        if (m->target > 0 && to > m->target) to = m->target;
        if (m->target < 0 && to < m->target) to = m->target;
    } else {
        if (m->target > 0 && to < 0) to = 0;
        if (m->target < 0 && to > 0) to = 0;
    }

    return to - m->offset;
}

/*
 * The same step as a world-space displacement, which is what the sweep needs.
 *
 * An axis mover's is the scalar on its own axis and nothing on the other two.
 * A train's is the difference between where its parts are now and where the
 * step would put them — all three components, and NOT `step` projected onto an
 * axis, because a train has no axis to project onto.
 */
static void mover_step_vec(const q2_mover *m, s32 step, s32 out[3])
{
    out[0] = out[1] = out[2] = 0;

    if (!m->is_path) {
        out[m->axis < 3u ? m->axis : 1u] = step;
        return;
    }

    {
        q2_mover after = *m;
        s32 now[3], then[3];
        int k;

        after.offset = m->offset + step;
        q2_mover_displacement(m, now);
        q2_mover_displacement(&after, then);
        for (k = 0; k < 3; k++)
            out[k] = then[k] - now[k];
    }
}

/*
 * A TRAIN'S BACK-OFF GOAL — 0x8002CAE0 (opening) and 0x8002CB34 (closing).
 *
 * `progress -/+ speed * 150`, clamped into [0, target]. The console's clamp is
 * an UNSIGNED compare against the target, which catches both ends with one
 * test: a goal that went below zero wraps past the target and lands on the
 * `= 0` arm, one that ran past the target lands on the `= target` arm.
 */
static void mover_path_block(q2_mover *m)
{
    s32 back = (s32)m->speed * Q2_MOVER_PATH_BACKOFF;
    s32 goal = (m->saved_state == Q2_MV_CLOSING) ? m->offset + back
                                                 : m->offset - back;

    if (goal < 0)
        goal = 0;
    if (goal > m->target)
        goal = m->target;

    m->wait_timer = (u16)goal;
}

static void mover_move(q2_mover_set *set, q2_mover *m, s32 dt, int dir)
{
    s32 old = m->offset;
    int settled = 0;

    if (dir) {
        if (m->target > 0) {
            m->offset += m->speed * dt;
            if (m->offset > m->target) {
                m->offset = m->target;
                m->state  = Q2_MV_ARRIVED;
                /* 0x8002C8D8: the train's arrival, louder than its running
                 * sound and the only time it plays. */
                if (m->is_path)
                    m->travel_sound = Q2_MOVER_TRAVEL_STOP_ID;
            }
        } else {
            m->offset -= m->speed * dt;
            if (m->offset < m->target) {
                m->offset = m->target;
                m->state  = Q2_MV_ARRIVED;
            }
        }

    } else {
        if (m->target > 0) {
            m->offset -= m->speed * dt;
            if (m->offset < 0) {
                m->offset = 0;
                settled   = 1;
                m->state  = Q2_MV_IDLE;
            }
        } else {
            m->offset += m->speed * dt;
            if (m->offset > 0) {
                m->offset = 0;
                settled   = 1;
                m->state  = Q2_MV_IDLE;
            }
        }
    }

    /*
     * The BLOCKED countdown, out of the `if (dir)` arm it used to sit in.
     *
     * 0x80025D08 reloads obj+0x56 with 16 on both arms and the decrement is
     * outside the direction test, so a door blocked while closing counted down
     * and one blocked while opening did not. And the decrement was `--` on a
     * u8 with no guard: unreachable while nothing ever assigned Q2_MV_BLOCKED,
     * and a 255-tick freeze the moment something did.
     */
    if (m->state == Q2_MV_BLOCKED) {
        if (m->block_timer)
            m->block_timer--;
        if (m->block_timer == 0)
            m->state = m->saved_state;
    }

    /*
     * The portal node's visibility bit follows the door, EXCEPT that a leaf
     * whose partner is still moving must not re-seal the opening. That is what
     * stops a double door going opaque the instant its first leaf shuts.
     *
     * 0x80025C5C-0x80025C74 writes bit 15 of the portal node's `flags08`: set
     * on the tick the leaf settles fully CLOSED, cleared otherwise, and the
     * write skipped entirely while the partner object's state byte at +0x52 is
     * non-zero (0x80025C24-0x80025C54).
     *
     * `sealed` is that bit. It is recorded on the mover rather than written
     * into the client's node-hidden array, because that array has another
     * writer — OBJDRAWOFF, which the script uses to hide nodes and which keeps
     * a count — and two writers fighting over one byte would make a door
     * un-hide whatever a script had hidden. The zone draw reads this instead.
     */
    if (m->portal_node >= 0) {
        bool partner_busy = false;

        if (m->partner >= 0 && (u32)m->partner < set->count)
            partner_busy = set->movers[m->partner].state != Q2_MV_IDLE;

        if (!partner_busy)
            m->sealed = (u8)(settled ? 1 : 0);
    }

    (void)old;
}

u32 q2_movers_tick(q2_mover_set *set, s32 dt, u16 player_keys)
{
    return q2_movers_tick_blocked(set, dt, player_keys, NULL, NULL);
}

/*
 * Would this mover's next step run into something, and is it allowed to?
 *
 * 0x80025CBC. The direction decides which bit of `block_flags` exempts it: bit
 * 0 while opening, bit 1 while closing. Only MOVER_A installs the opening bit;
 * pusher-enabled CALL movers leave both clear and therefore enter the blocked
 * state after a failed carry. The crusher damage is the generic 0x80051E74
 * rollback path, not an obstruction-ignore bit. A zero-gate PISTON owns no
 * pusher, so it never asks this question in the first place.
 */
static bool mover_obstructed(q2_mover_set *set, u32 index, q2_mover *m,
                             s32 step, q2_mover_blocked_fn blocked, void *user)
{
    u8  exempt;
    s32 sweep[3];

    if (!m->blocks_player || !blocked || step == 0)
        return false;

    mover_step_vec(m, step, sweep);
    if (!sweep[0] && !sweep[1] && !sweep[2])
        return false;
    if (!blocked(index, sweep, user))
        return false;

    exempt = (m->state == Q2_MV_CLOSING ||
              (m->state == Q2_MV_BLOCKED && m->saved_state == Q2_MV_CLOSING))
           ? Q2_MV_BLK_IGNORE_CLOSING : Q2_MV_BLK_IGNORE_OPENING;

    if (!m->is_path) {
        /* 0x80025D08 reloads the timer on BOTH arms; only the flag decides
         * whether the state changes with it. */
        m->block_timer = Q2_MOVER_BLOCK_TICKS;
    }

    if (m->block_flags & exempt)
        return false;               /* this MOVER_A arm does not care */

    if (m->state != Q2_MV_BLOCKED) {
        m->saved_state = m->state;
        m->state       = Q2_MV_BLOCKED;
        /*
         * A train picks its retreat here and never revises it. Blocked again
         * while retreating, 0x8002CB8C writes the entry state — 4 — straight
         * back over itself and changes nothing else, so the goal it is already
         * heading for stands.
         */
        if (m->is_path)
            mover_path_block(m);
    }
    (void)set;
    return true;
}

u32 q2_movers_tick_blocked(q2_mover_set *set, s32 dt, u16 player_keys,
                           q2_mover_blocked_fn blocked, void *user)
{
    u32 moved = 0, i;

    if (!set || dt <= 0)
        return 0;

    for (i = 0; i < set->count; i++) {
        q2_mover *m = &set->movers[i];
        int trig = m->triggered;
        s32 before = m->offset;

        m->triggered = 0;

        /* Its map handler writes the displacement directly. In retail that
         * handler clears obj+0x2C before the generic object tick reaches it. */
        if (m->external)
            continue;

        switch (m->state) {
        case Q2_MV_IDLE:
            if (!trig) {
                m->announced = 0;
                break;
            }
            /* Locked doors complain once, not every tick — 0x8002585C is
             * behind the same latch, so the refusal sound is once too. */
            if (m->key_mask && !(player_keys & m->key_mask)) {
                if (!m->announced)
                    mover_sound(m, Q2_MVSND_KEY_TRY);
                m->announced = 1;
                break;
            }
            m->state = Q2_MV_DELAY;
            if (m->key_mask) {
                /* 0x800257A8: the key was accepted. */
                mover_sound(m, Q2_MVSND_KEY_USE);
                m->announced = 1;
            }
            break;

        case Q2_MV_DELAY:
            if ((s16)(m->delay_timer - dt) > 0) {
                m->delay_timer = (u16)(m->delay_timer - dt);
                break;
            }
            /* 0x800258F0 -> 0x80025A5C: the motion starts. */
            m->state = Q2_MV_OPENING;
            mover_sound(m, Q2_MVSND_START);
            break;

        case Q2_MV_ARRIVED:
            m->state = Q2_MV_OPEN;
            break;

        case Q2_MV_OPEN: {
            u16 t;
            if (m->wait_timer == Q2_MOVER_WAIT_NEVER)
                break;
            t = (u16)(m->wait_timer - dt);
            if ((s16)t > 0) {
                m->wait_timer = t;
                break;
            }
            /* Standing in the doorway holds it open. */
            if (trig) {
                m->wait_timer = 1;
                break;
            }
            /* 0x800259BC -> the same 0x80025A5C. A door closing plays the
             * same start sound; there is no separate close. */
            m->state = Q2_MV_CLOSING;
            mover_sound(m, Q2_MVSND_START);
            break;
        }

        case Q2_MV_OPENING:
            mover_running(m);
            if (!mover_obstructed(set, i, m, mover_step(m, dt, 1),
                                  blocked, user))
                mover_move(set, m, dt, 1);
            break;

        case Q2_MV_CLOSING:
            mover_running(m);
            if (!mover_obstructed(set, i, m, mover_step(m, dt, 0),
                                  blocked, user))
                mover_move(set, m, dt, 0);
            break;

        case Q2_MV_BLOCKED: {
            /* A door blocked while closing reverses; one blocked opening
             * carries on in the direction it was already going. */
            int dir = m->saved_state == Q2_MV_CLOSING;

            mover_running(m);

            /*
             * A TRAIN'S STATE 4 IS NOT A PAUSE. 0x8002C7A8 sends it to
             * 0x8006FEB8, a clamped move-toward the goal `mover_path_block`
             * chose, and it resumes the state it was interrupted in only once
             * it gets there — obj+0x56, restored at 0x8002C7EC.
             *
             * Blocked again on the way, nothing commits: the entry state is
             * written back over the restore at 0x8002CB8C and the retreat picks
             * up where it left off next tick.
             */
            if (m->is_path) {
                s32 goal = (s32)m->wait_timer;
                s32 span = m->speed * dt;
                s32 step;

                if (m->offset < goal)
                    step = (m->offset + span > goal) ? goal - m->offset : span;
                else
                    step = (m->offset - span < goal) ? goal - m->offset : -span;

                if (!mover_obstructed(set, i, m, step, blocked, user)) {
                    m->offset += step;
                    if (m->offset == goal)
                        m->state = m->saved_state;
                }
                break;
            }

            if (!mover_obstructed(set, i, m, mover_step(m, dt, dir),
                                  blocked, user))
                mover_move(set, m, dt, dir);
            else if (m->block_timer == 0)
                m->state = m->saved_state;
            break;
        }

        default:
            m->state = Q2_MV_IDLE;
            break;
        }

        if (m->offset != before)
            moved++;
    }

    return moved;
}

void q2_movers_node_offset(const q2_mover_set *set, u32 scene_node, s32 out[3])
{
    u32 i, k;

    if (!out)
        return;

    out[0] = out[1] = out[2] = 0;

    if (!set)
        return;

    for (i = 0; i < set->count; i++) {
        const q2_mover *m = &set->movers[i];

        if (m->offset == 0)
            continue;

        for (k = 0; k < m->part_count; k++) {
            s32 d[3];
            int c;

            if ((u32)m->node[k] != scene_node)
                continue;

            /* Three components, because a train has three. This used to add
             * `offset` to `out[axis]`, which is right for every other family
             * and sends a platform down its own Y by the length of a diagonal
             * journey. */
            q2_mover_displacement(m, d);
            for (c = 0; c < 3; c++)
                out[c] += d[c];
            break;
        }
    }
}

/* Locate the first item in a named Events entry. CRATES is exactly one CALL,
 * but walking it keeps this tied to the authored directory rather than to the
 * PAL build's byte offset 488. */
static bool named_item_offset(const q2_events *events, const char *name,
                              u32 *out)
{
    u32 i;

    if (!events || !name || !out)
        return false;

    for (i = 0; i < events->dir_count; i++) {
        q2_event_dir_entry entry;
        q2_event_record rec;
        q2_event_item item;

        if (!q2_events_get_dir_entry(events, i, &entry) ||
            strcmp(entry.name, name) != 0)
            continue;
        if (!q2_events_record_at(events, entry.offset, &rec) ||
            rec.n_items == 0 || !q2_events_get_item(events, &rec, 0, &item))
            return false;

        *out = item.offset;
        return true;
    }

    return false;
}

u32 q2_movers_step_crates(q2_mover_set *set, const q2_events *events,
                          const q2_scene *scene, s32 dt)
{
    u32 item_offset;
    u32 i, moved = 0;

    if (!set || !scene || dt <= 0 ||
        !named_item_offset(events, "CRATES", &item_offset))
        return 0;

    for (i = 0; i < set->count; i++) {
        q2_mover *m = &set->movers[i];
        q2_scene_node node;
        s32 factor, product, step, centre;
        s32 before;

        if (!m->external || m->item_offset != item_offset ||
            m->part_count != 1 ||
            !q2_scene_get_node(scene, (u32)m->node[0], &node))
            continue;

        factor  = m->external_part < 2 ? 16 : 20;
        product = factor * dt;
        /* The MIPS adds seven before sra 3 only on a negative product: signed
         * division truncates toward zero. dt is positive in play, but retain
         * the exact arithmetic rather than relying on that. */
        if (product < 0)
            product += 7;
        step = product >> 3;

        before    = m->offset;
        m->offset = (s16)(m->offset + step);

        /* `addu; srl sign; addu; sra 1` is a signed average rounded toward
         * zero. The handler then sign-extends the translated centre to s16. */
        centre = (s32)(((s64)node.bbox_min[1] + node.bbox_max[1]) / 2);
        centre = (s16)(centre + m->offset);

        /* slti centre,-1044; bne skips the wrap. In other words the subtract
         * happens on >= -1044 — the inverse is an easy branch-reading trap. */
        if (centre >= -1044)
            m->offset = (s16)(m->offset - 3500);

        if (m->offset != before)
            moved++;
    }

    return moved;
}
