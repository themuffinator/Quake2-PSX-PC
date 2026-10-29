#include "events_rt.h"
#include "random.h"

#include <stdlib.h>
#include <string.h>

/* How many records may run in one update before we stop. The original ran its
 * queue once per tick, so a self-triggering script simply ran again next tick
 * rather than locking up. */
#define Q2_EVENT_RT_MAX_PER_UPDATE 256

q2_result q2_event_rt_init(q2_event_rt *rt, const q2_events *events)
{
    q2_event_record rec;
    u32 n = 0;

    if (!rt || !events)
        return Q2_ERR_INVALID_ARG;

    memset(rt, 0, sizeof(*rt));
    rt->events = *events;

    /* Not zero: zero is a real zone index. -1 is "the owner has not told us
     * which zone is resident", which makes every ZONEGATE acceptable and
     * non-aborting — the behaviour this file had before the gate learned how
     * to refuse, and the only safe one without the refusal (see the arm). */
    rt->current_zone = -1;

    if (events->record_count == 0)
        return Q2_OK;                 /* a real state on 36 chunks */

    rt->flags   = (u8 *)calloc(events->record_count, sizeof(u8));
    rt->offsets = (u32 *)calloc(events->record_count, sizeof(u32));
    if (!rt->flags || !rt->offsets) {
        q2_event_rt_free(rt);
        return Q2_ERR_NO_MEMORY;
    }

    /* The per-item shadow, indexed straight by chunk offset — see the field
     * comment. Under 3 KB on the largest chunk the disc has. */
    if (events->size > 0) {
        rt->item_flags = (u8 *)calloc(events->size, 1);
        rt->object_flags = (u8 *)calloc(events->size, 1);
        if (!rt->item_flags || !rt->object_flags) {
            q2_event_rt_free(rt);
            return Q2_ERR_NO_MEMORY;
        }
        rt->item_flags_size = events->size;
    }

    /*
     * Seed the shadow from the file. Bits 0-2 are runtime state and are clear
     * on disc, so the stored byte is a valid initial value as-is.
     *
     * THE LOADER'S DEFAULT CATEGORY, reproduced here rather than left silent.
     * 0x80026E60..0x80026E74 is
     *   lbu v1,3(s1) / andi v0,v1,0x28 / bne v0,zero,skip / ori v0,v1,0x10 /
     *   sb v0,3(s1)
     * — at load time, a record carrying neither CAT_A (0x08) nor CAT_C (0x20)
     * gets CAT_B (0x10) forced into its flags byte, written back into the
     * chunk. The chunk here is borrowed const, so the shadow is where it goes;
     * the shadow IS this port's copy of that byte, so this is the same edit in
     * the same place in the sequence.
     *
     * It changes nothing on this disc, which is the point of writing it down:
     * the flags values that occur across all 4,179 records are exactly {0x08,
     * 0x10, 0x18, 0x20, 0x28, 0x48, 0x50, 0x58}, and every one of them either
     * already satisfies (flags & 0x28) or already carries 0x10, so the `ori` is
     * a no-op 4,179 times out of 4,179. Checked, not assumed. Reproducing it
     * costs one line and stops the next reader wondering whether the omission
     * was a decision or an oversight.
     */
    if (q2_events_first_record(&rt->events, &rec)) {
        do {
            u8 f;

            if (n >= events->record_count)
                break;
            f = rec.flags;
            if ((f & (Q2_EVREC_CAT_A | Q2_EVREC_CAT_C)) == 0)
                f |= Q2_EVREC_CAT_B;   /* 0x80026E68/0x80026E70 */
            rt->offsets[n] = rec.offset;
            rt->flags[n]   = f;
            n++;
        } while (q2_events_next_record(&rt->events, &rec, &rec));
    }

    rt->record_count = n;
    return Q2_OK;
}

void q2_event_rt_free(q2_event_rt *rt)
{
    if (!rt)
        return;
    free(rt->flags);
    free(rt->offsets);
    free(rt->item_flags);
    free(rt->object_flags);
    memset(rt, 0, sizeof(*rt));
}

static s32 record_slot(const q2_event_rt *rt, u32 offset)
{
    u32 i;

    for (i = 0; i < rt->record_count; i++) {
        if (rt->offsets[i] == offset)
            return (s32)i;
    }
    return -1;
}

u8 q2_event_rt_flags(const q2_event_rt *rt, u32 offset)
{
    s32 slot;

    if (!rt)
        return 0;
    slot = record_slot(rt, offset);
    return (slot < 0) ? 0 : rt->flags[slot];
}

u8 q2_event_rt_item_flags(const q2_event_rt *rt, u32 offset)
{
    if (!rt || !rt->item_flags || offset >= rt->item_flags_size)
        return 0;
    return rt->item_flags[offset];
}

bool q2_event_rt_trigger(q2_event_rt *rt, u32 offset)
{
    u32 i;

    if (!rt || rt->pending_count >= Q2_EVENT_RT_PENDING_MAX)
        return false;
    if (record_slot(rt, offset) < 0)
        return false;

    /*
     * DE-DUPLICATION IS THIS PORT'S, NOT THE CONSOLE'S — a deliberate,
     * measured divergence rather than an accident, so here is the measurement.
     *
     * 0x800274CC's push loop (0x800274FC..0x80027514) writes every list entry
     * unconditionally: no scan of what is already queued, no capacity test.
     * A record named twice in one drain therefore RUNS twice on the console.
     *
     * How often that matters on this disc: I walked the TRIGGER chain from
     * every record in all 164 containers. 198 TRIGGER items, longest list 16
     * entries, and NO record lists the same target twice. Exactly one seed
     * anywhere reaches a record twice through two different lists — BOSS1
     * COMMON record 0x374 (flags 0x10, a single TRIGGER naming 0x14c, 0x140,
     * 0x198, 0x1b4 and 0x388), whose chain arrives at record 0x1e0 twice.
     * 0x1e0 is flags 0x10 with one MOVER_A item, so the console issues that
     * one mover command twice and this port issues it once — a door told to
     * open that is already opening.
     *
     * That is the whole cost, stated with the record numbers so the next
     * reader can overturn the decision on the same evidence rather than
     * re-deriving it.
     */
    for (i = 0; i < rt->pending_count; i++) {
        if (rt->pending[i] == offset)
            return true;
    }

    rt->pending[rt->pending_count++] = offset;
    return true;
}

bool q2_event_rt_trigger_named(q2_event_rt *rt, const char *name)
{
    u32 i;

    if (!rt || !name)
        return false;

    for (i = 0; i < rt->events.dir_count; i++) {
        q2_event_dir_entry e;
        if (!q2_events_get_dir_entry(&rt->events, i, &e))
            continue;
        if (strcmp(e.name, name) == 0)
            return q2_event_rt_trigger(rt, e.offset);
    }
    return false;
}

bool q2_event_rt_disable_named(q2_event_rt *rt, const char *name, bool disabled)
{
    u32 i;
    if (!rt || !name || !rt->flags)
        return false;
    for (i = 0; i < rt->events.dir_count; i++) {
        q2_event_dir_entry e;
        s32 slot;
        if (!q2_events_get_dir_entry(&rt->events, i, &e) || strcmp(e.name, name))
            continue;
        slot = record_slot(rt, e.offset);
        if (slot < 0) return false;
        if (disabled) rt->flags[slot] |= Q2_EVREC_DISABLED;
        else rt->flags[slot] &= (u8)~Q2_EVREC_DISABLED;
        return true;
    }
    return false;
}

void q2_event_rt_contacts_begin(q2_event_rt *rt)
{
    u32 i;

    if (!rt || !rt->flags)
        return;

    for (i = 0; i < rt->record_count; i++)
        rt->flags[i] = (u8)(rt->flags[i] & ~(unsigned)Q2_EVREC_RT1);
}

bool q2_event_rt_contact(q2_event_rt *rt, u32 offset)
{
    s32 slot;
    u8 flags;

    if (!rt || !rt->flags)
        return false;

    slot = record_slot(rt, offset);
    if (slot < 0)
        return false;

    flags = rt->flags[slot];
    rt->flags[slot] = flags | Q2_EVREC_RT1;

    /* 0x80027F38..0x80027F54: CAT_B wins and runs continuously. Without it,
     * CAT_A runs only while the previous-contact bit is clear. */
    if ((flags & Q2_EVREC_CAT_B) ||
        ((flags & (Q2_EVREC_CAT_A | Q2_EVREC_RT2)) == Q2_EVREC_CAT_A))
        return q2_event_rt_trigger(rt, offset);

    return true;
}

void q2_event_rt_contacts_end(q2_event_rt *rt)
{
    u32 i;

    if (!rt || !rt->flags)
        return;

    for (i = 0; i < rt->record_count; i++) {
        u8 flags = rt->flags[i];
        bool now = (flags & Q2_EVREC_RT1) != 0;
        bool was = (flags & Q2_EVREC_RT2) != 0;

        /*
         * 0x80028070..0x80028084: CAT_C plus previous contact and no current
         * contact is the leave edge. The edge test and the bit shift below are
         * retail's; what the edge QUEUES was not.
         *
         * THIS IS RETAIL'S BUG AND NOT OURS, and reproducing it is the point.
         * The offset the leave pass pushes comes from 0x8002808C `lh v1,26(s6)`
         * — s6, not s3. s6 is the TRIGGER-array cursor: initialised at
         * 0x80027E9C `lw s6,4(a3)` and advanced ONLY in the enter loop's branch
         * delay slot, 0x8002804C `addiu s6,s6,36`, which executes on the final
         * iteration too. The leave loop advances s3 (the record cursor) and
         * never touches s6, so s6 sits at base + 36*count for the whole pass.
         *
         * That is NOT a read past the end of the array. trigger.h documents the
         * array as trigger[count + 1] with a sentinel last, and the size
         * identity 4 + 36*(n+1) + 12*planes holds on all 49 maps — so s6 lands
         * exactly on the SENTINEL and +26 is the sentinel's event_offset. I
         * read that field out of all 49 COMMON.DAT TrigBounds chunks: it is 0
         * in 49 of 49 (the rest of the sentinel is 0xCD fill; only plane_start,
         * which equals the total plane count in 49 of 49, and this zero are
         * written).
         *
         * Chunk offset 0 is the Events chunk's u32 record count, not a record
         * start. On the console the drain at 0x800280B0 reads `lbu v1,3(base)`
         * — byte 3 of a count below 256, i.e. 0 — passes the DISABLED gate,
         * writes 1 into that byte with the latch at 0x800280FC, then reads
         * `lbu v0,2(s2)` = 0 items and stops. (The count is only ever read as
         * `lh`, at 0x80027EB0 and at the loader's 0x80026DEC, which is what
         * makes the corrupted top byte invisible.) SO RETAIL'S CAT_C LEAVE EDGE
         * RUNS NOTHING, on every map.
         *
         * The port therefore queues `catc_leave_offset`, which init leaves 0
         * and q2_event_rt_trigger rejects because 0 starts no record. Deleting
         * the queue entirely would be indistinguishable on this disc and would
         * lose the reason, so the call stays and the edge is counted.
         *
         * What this replaces — `q2_event_rt_trigger(rt, rt->offsets[i])`, the
         * LEAVING record's own offset — is observable on exactly one map. Of
         * the two CAT_C records on the disc, JAIL2 COMMON 0x6f4 (flags 0x20) is
         * named by no trigger volume at all, while POWER2 COMMON 0x30c (flags
         * 0x28, items [CALL ROTBUTTON, CALL ROTBUTTON, CALL LIFT1]) is named by
         * volumes 32 and 33. So the port was firing two rotating buttons and a
         * lift a second time as the player stepped OUT — a script event the
         * game never had.
         */
        if ((flags & Q2_EVREC_CAT_C) && was && !now) {
            rt->catc_leave_edges++;
            q2_event_rt_trigger(rt, rt->catc_leave_offset);
        }

        /* 0x80028160..0x80028180: RT1 is copied to RT2, then RT1 is cleared. */
        flags = (u8)(flags & ~(unsigned)(Q2_EVREC_RT1 | Q2_EVREC_RT2));
        if (now)
            flags |= Q2_EVREC_RT2;
        rt->flags[i] = flags;
    }
}

/* ------------------------------------------------------------------------- */
/* Item handlers                                                              */
/* ------------------------------------------------------------------------- */
static void set_disabled_on_list(q2_event_rt *rt, const q2_event_item *item,
                                 bool disabled)
{
    const u8 *offsets = NULL;
    u32 count = 0, i;

    if (!q2_events_get_list(item, &count, &offsets))
        return;

    for (i = 0; i < count; i++) {
        u32 target = q2_events_list_entry(offsets, i);
        s32 slot   = record_slot(rt, target);

        if (slot < 0)
            continue;

        if (disabled)
            rt->flags[slot] |=  Q2_EVREC_DISABLED;
        else
            rt->flags[slot] = (u8)(rt->flags[slot] & ~(unsigned)Q2_EVREC_DISABLED);
    }
}

/*
 * The live counter for one WAIT item, created on first arrival.
 *
 * Seeded exactly as the load-time constructor 0x80026F08 seeds the item:
 * 0x80026F14/0x80026F1C writes -1 into item+4 (the stamp, so the first arrival
 * can never collide with a real tick) and 0x80026F18/0x80026F24 copies the u16
 * at item+10 into item+8 (the countdown). Doing it lazily rather than in
 * q2_event_rt_init is a port choice: the console runs its constructor pass over
 * every item at load, but nothing can observe the difference for an item that
 * has never been dispatched.
 */
static s32 wait_slot(q2_event_rt *rt, const q2_event_item *item, u16 reload)
{
    u32 i;

    for (i = 0; i < Q2_EVENT_RT_WAIT_MAX; i++) {
        if (rt->wait[i].in_use && rt->wait[i].offset == item->offset)
            return (s32)i;
    }
    for (i = 0; i < Q2_EVENT_RT_WAIT_MAX; i++) {
        if (!rt->wait[i].in_use) {
            rt->wait[i].in_use = true;
            rt->wait[i].offset = item->offset;
            rt->wait[i].stamp  = -1;      /* 0x80026F14 */
            rt->wait[i].count  = reload;  /* 0x80026F18 -> 0x80026F24 */
            return (s32)i;
        }
    }
    return -1;
}

static q2_event_outcome run_item(q2_event_rt *rt, const q2_event_item *item)
{
    switch (item->opcode) {
    case Q2_EVOP_TRIGGER: {
        const u8 *offsets = NULL;
        u32 count = 0, i;

        if (q2_events_get_list(item, &count, &offsets)) {
            for (i = 0; i < count; i++)
                q2_event_rt_trigger(rt, q2_events_list_entry(offsets, i));
        }
        return Q2_EVENT_OK;
    }

    case Q2_EVOP_ENABLE:
        set_disabled_on_list(rt, item, false);
        return Q2_EVENT_OK;

    case Q2_EVOP_DISABLE:
        set_disabled_on_list(rt, item, true);
        return Q2_EVENT_OK;

    case Q2_EVOP_ZONEGATE: {
        /*
         * THE DESTINATION IS A NAME, NOT AN INDEX.
         *
         * This read `item->payload[0]` under the comment "the destination is
         * the first byte of the payload". The 0x0F item carries a 12-byte
         * NAME12 operand — "Zone0", "Zone1" — so the first byte is the ASCII
         * 'Z', 90. Every one of the disc's 619 zone gates therefore asked the
         * client to load ZONE90.DAT, which does not exist, and intra-level
         * zone streaming was dead across the whole game.
         *
         * It was worse than dead. The failed load left the transition's carry
         * flags armed (client_load_zone), so the next RESTART handed the
         * player back whatever the sim held — which after a death is a corpse.
         *
         * The names really are "Zone0"/"Zone1" and the files really are
         * ZONE0.DAT/ZONE1.DAT, so the trailing decimal is the index. The name
         * is reported alongside it so the owner can verify the zone exists
         * before asking for it rather than trusting this parse.
         *
         * AN ACCEPTED GATE STOPS THE RECORD. 0x80027830..0x8002783C returns 1
         * from the handler when 0x80079178 accepted, and every executor site
         * treats a non-zero handler result as "stop this record": 0x80027A18,
         * 0x80027AAC, 0x80027DEC, 0x80027FF8, 0x80028124, and 0x80027200 in
         * the TIMER continuation. It is one of only three opcodes that can —
         * scanning 0x8002752C..0x800276C4 for a non-zero return finds only
         * comparison constants, and every exit of the mover and FXGROUP
         * handlers returns 0: either through 0x8002793C, which zeroes v0, or
         * by going straight to 0x80027940 with `addu v0,zero,zero` in the
         * delay slot (MOVER_A/B's early exits at 0x80027534, 0x8002758C,
         * 0x800275D8 and 0x80027638; MOVER_C's and FXGROUP's only exits, the
         * `j 0x80027940` at 0x800276A8 and 0x800276BC). This file used
         * to return Q2_EVENT_ZONE_CHANGE and keep iterating, so the 552 disc
         * gates that are not their record's last item went on to command 461
         * MOVER_A, 83 MOVER_C, 5 DISABLE and 3 CALL on the very frame the zone
         * was being torn down. (It still does, deliberately, while the owner
         * has not said which zone is resident — see the end of this arm.)
         */
        u32 i, digits = 0, value = 0;
        char name[13];

        /*
         * 0x80027788/0x8002778C: `addiu v0,zero,16` then `bne v1,v0` — the
         * length must be EXACTLY 16, not merely big enough. This arm accepted
         * anything >= 14. All 770 gates on the disc are 16, so nothing real
         * changes; the point is that a longer gate would be dispatched here
         * and refused there.
         */
        if (!item->payload || item->len != Q2_EVENT_ITEM_HEADER_SIZE + 14)
            return Q2_EVENT_OK;

        /*
         * 0x80027794 `lh v0,16948(gp)` / 0x8002779C `bne v0,zero,0x80027940`
         * with `addu v0,zero,zero` in the delay slot: while the initial-state
         * pass flag is up the gate returns 0 — a third way retail answers
         * "no gate", tested after the length and before the name is read.
         *
         * This used to be left out on the grounds that gp+16948 was a "loader
         * busy" flag and the port's load window is one frame. The variable was
         * misidentified. Its only stores (xrefs, whole image) are around the
         * STARTLEV named-event run (0x8007C2AC / 0x8007C338) and around the
         * EVE_ replay (0x800294B4 / 0x80029754); it suppresses gates that
         * those synchronous passes reach, so a replayed spent [ZONEGATE,
         * MOVER] reopens its door without requesting another load. It has
         * nothing to do with a player-driven gate arriving mid-load.
         * q2_event_rt_replay raises it; see `initial_pass`.
         */
        if (rt->initial_pass)
            return Q2_EVENT_OK;

        memset(name, 0, sizeof(name));
        for (i = 0; i < 12; i++) {
            char ch = (char)item->payload[i];
            if (ch == 0)
                break;
            name[i] = ch;
            if (ch >= '0' && ch <= '9') {
                value = value * 10u + (u32)(ch - '0');
                digits++;
            } else if (digits) {
                /* Digits then a non-digit: not a trailing number. */
                digits = 0;
                value  = 0;
            }
        }

        /*
         * Nothing to resolve. A PORT-SIDE refusal, not retail's: the console
         * hands the raw name to 0x80079178 and would abort the record on it.
         * We cannot ask for a zone we could not name, so we decline and let
         * the record continue rather than guess. All 770 gates on the disc
         * carry "Zone0".."Zone5", so this arm is unreachable on this disc.
         */
        if (!digits)
            return Q2_EVENT_OK;

        /*
         * The two refusals inside 0x80079178, which this port did not have:
         *
         *  - 0x8007917C `lw v0,0x800AEBCC` / 0x8007919C `bne v0,zero` — in
         *    deathmatch the request returns 0 outright and no gate happens.
         *  - 0x800791E0 `jal 0x8006DBC0(sp+16, 0x800E465C)` / 0x800791E8
         *    `xori v0,v0,1` — the name is compared against the RESIDENT zone's
         *    and a match returns 0 (the compare answers 1 on equal; the xori
         *    turns that into the fall-through to 0x800791F4, which returns 0).
         *
         * Anything else runs straight through to 0x8007939C `addiu v0,zero,1`
         * — 0x80079200..0x80079394 has no other early exit — so an accepted
         * gate always answers 1.
         *
         * The same-zone half was already in the port, but in the wrong place
         * and too late: main.c discards the request AFTER the record has
         * finished, which makes the refused case right by accident and leaves
         * the accepted case wrong. It belongs here, where 0x80079178 makes it.
         */
        if (rt->multiplayer || (s32)value == rt->current_zone)
            return Q2_EVENT_OK;

        memcpy(rt->pending_zone_name, name, sizeof(rt->pending_zone_name));
        rt->has_zone_change = true;
        rt->pending_zone    = value;

        /*
         * 0x8002783C: the handler returns 1 and the record stops here — but
         * ONLY once the owner has said which zone is resident. With
         * `current_zone` still -1 the gate raises its request and the record
         * runs on, which is this file's behaviour before the abort existed.
         * THAT FALLBACK IS THE PORT'S, NOT THE CONSOLE'S — the console's own
         * "no zone resident" state (0x8006DBC8: an empty resident name makes
         * the compare answer 2, which the xori passes as accepted) accepts
         * AND aborts — and here is why it cannot be skipped.
         *
         * The abort is only safe paired with the same-zone refusal above. Of
         * the 151 gates in the 49 COMMON scripts — the ones the trigger
         * volumes fire — 107 are not their record's last item, all 107 of
         * those records are CAT_B (0x10, re-run on every frame the player
         * stands in the volume), and 105 of them command a MOVER_A or MOVER_C
         * after the gate; 74 are exactly [ZONEGATE, MOVER_A]. That is a door
         * that waits for the zone behind it: on the console the gate aborts
         * while the zone it names is not resident and is refused once it is,
         * so the door opens on the first frame after the load. A runtime that
         * cannot make the refusal accepts that gate on every frame forever,
         * aborts every time, and never opens the door. So the abort waits on
         * the owner saying which zone is resident. The playable client does —
         * client_load_zone sets `current_zone` from its `zone_index` after
         * every q2_sim_attach_gameplay, as the inspect tool's `events` sweep
         * does from each ZONEn.DAT name — so there the console's abort is in
         * force; an owner that has not set it keeps the accept-and-carry-on
         * behaviour rather than all 105 of those doors and lifts shut for good.
         */
        if (rt->current_zone >= 0)
            rt->abort_record = true;
        return Q2_EVENT_ZONE_CHANGE;
    }

    case Q2_EVOP_MOVER_A:
    case Q2_EVOP_MOVER_B:
    case Q2_EVOP_MOVER_C:
        /*
         * REPORTED, not interpreted — the same split as a CALL.
         *
         * The note below was right about the hazard and wrong about the
         * conclusion. The disc values ARE Scene node indices, and `mover.h`
         * deliberately reads them as such rather than reproducing the
         * console's load-time rewrite into runtime object indices — so acting
         * on them is correct. What this file cannot do is say WHICH mover,
         * because the set belongs to the owner.
         */
        if (rt->on_mover) {
            rt->on_mover(rt->on_mover_user, item);
            rt->mover_count++;
            return Q2_EVENT_OK;
        }
        /* A standalone runtime has no mover set to own this item. The playable
         * client installs the callback above and resolves the immutable item
         * offset through q2_movers_trigger_item instead. */
        rt->skipped_movers++;
        return Q2_EVENT_UNSUPPORTED;

    case Q2_EVOP_CALL: {
        u8 index;

        /* Reported, not interpreted — see the note on `on_call`. */
        if (q2_events_get_call_index(item, &index)) {
            rt->call_count++;
            if (rt->on_call)
                rt->on_call(rt->on_call_user, item, index);
        }
        return Q2_EVENT_OK;
    }

    case Q2_EVOP_FXGROUP:
        /*
         * A `func_explosive`. Reported, not interpreted — the same split as a
         * CALL and a MOVER, and for the same reason: the set that maps an item
         * offset to a destroyable group belongs to the owner.
         *
         * The console reaches this arm with damage zero (0x800276B4 sets a2 to
         * zero itself), which its handler treats as "destroy now".
         */
        if (rt->on_explosive) {
            rt->on_explosive(rt->on_explosive_user, item);
            rt->explosive_count++;
        }
        return Q2_EVENT_OK;

    case Q2_EVOP_FX:
        /*
         * 0x80027840 only acts on the fixed, 8-byte form. Its target is the
         * current player, a question this generic runtime cannot answer, so
         * the owner gets the validated item and applies the damage.
         */
        if (q2_events_get_fx_damage(item, NULL, NULL)) {
            rt->fx_count++;
            if (rt->on_fx)
                rt->on_fx(rt->on_fx_user, item);
        }
        return Q2_EVENT_OK;

    case Q2_EVOP_WAIT: {
        /*
         * Opcode 0x09 — a live handler and a live constructor with ZERO uses on
         * the shipped disc, so everything below is read from the instructions
         * and NOT observed running. Nothing here can be checked against a map.
         *
         * That it is opcode 9 is not a guess. The exec table at 0x800ABD48 is
         * indexed by opcode-2 (0x8002749C `andi 0x3F` / 0x800274A0 `addiu -2` /
         * 0x800274A4 `sltiu 21`) and entry 7 (0x800ABD64) is 0x800276C4. The
         * LOAD table's base is 0x800ABCF8 (0x80026E54 `addiu s6,v0,-17160`),
         * it is indexed by opcode-3 (0x80026EA4 `addiu v1,v0,-3` / 0x80026EA8
         * `sltiu v0,v1,20`), and entry 6 (0x800ABD10) is 0x80026F08 —
         * different bias, same opcode.
         *
         * State lives in the item on the console (item+4 the stamp, item+8 the
         * countdown, item+10 the reload value) and in rt->wait[] here, because
         * the chunk is borrowed const. Only +4 and +8 are mutable; +10 is read
         * from the item both times.
         */
        s32 w;
        u16 reload;

        /* 0x800276C8/0x800276CC: len must be exactly 12, and the constructor
         * at 0x80026F0C/0x80026F10 demands the same before it seeds anything.
         * A rejected item returns 0 — it does NOT stop the record. */
        if (!item->payload || item->len != 12)
            return Q2_EVENT_OK;

        reload = q2_rd_u16(item->payload + 8);   /* item+10 */

        w = wait_slot(rt, item, reload);
        if (w < 0)
            return Q2_EVENT_OK;   /* out of slots; behave as the reject does */

        if (rt->wait[w].stamp == rt->tick) {
            /*
             * 0x800276E4 `beq a1,v1,0x80027838`, and 0x80027838 is
             * `j 0x80027940` with `addiu v0,zero,1` in ITS delay slot — so an
             * arrival on the tick the stamp already names returns 1 and stops
             * the record without touching the counter.
             *
             * AND THE STAMP IS STILL WRITTEN. 0x800276E8 `sw v0,4(t0)` is the
             * branch's delay slot, so item+4 := tick+1 happens on BOTH paths.
             * That is what makes WAIT count NON-CONSECUTIVE arrivals rather
             * than frames: a record that reaches the item on tick N and again
             * on N+1 finds the stamp already equal to N+1, re-stamps N+2, and
             * can never decrement again while it keeps arriving every tick.
             */
            rt->wait[w].stamp = rt->tick + 1;
            rt->abort_record  = true;
            return Q2_EVENT_OK;
        }
        rt->wait[w].stamp = rt->tick + 1;        /* 0x800276E8, again */

        /* 0x800276EC..0x80027700: decrement the u16 at item+8 and, while it is
         * non-zero, return 1 (`addiu v0,zero,1` in the delay slot at
         * 0x80027704) — the record stops here. */
        rt->wait[w].count = (u16)(rt->wait[w].count - 1u);
        if (rt->wait[w].count != 0) {
            rt->abort_record = true;
            return Q2_EVENT_OK;
        }

        /*
         * At zero: 0x80027724 `lhu v0,10(t0)` / 0x8002772C `sh v0,8(t0)`
         * reloads the counter from item+10 and returns 0, so the record
         * carries on past the WAIT and the item is armed again.
         *
         * NOT IMPLEMENTED, ON PURPOSE — 0x80027708..0x80027720 takes a
         * different branch first when the item's own ONESHOT bit is set: it
         * ORs 0x80 into the item with a WORD store and returns 0 without
         * reloading. It cannot change anything. The dispatcher at 0x80027498
         * has already written DISABLED := ONESHOT into that byte before the
         * handler was entered, so the bit the branch sets is already set; and
         * because the item is retired, whether the counter reloads is moot.
         * (It IS reachable — precisely when the count reaches zero on the
         * item's first and only dispatch — it is just a no-op when it is.)
         */
        rt->wait[w].count = reload;
        return Q2_EVENT_OK;
    }

    default:
        /* Anything else: recognised by the parser and not implemented.
         * Counting them separately from movers would imply more certainty
         * about them than we have. */
        return Q2_EVENT_OK;
    }
}

/* ------------------------------------------------------------------------- */
/* The TIMER table at 0x800C6F74                                              */
/* ------------------------------------------------------------------------- */
/*
 * CLAIM A SLOT, 0x80026FEC — what the TIMER primitive does with the three
 * operands the owner's hook has just left in `defer_ticks`/`defer_fires`/
 * `defer_window`.
 *
 * Two refusals, and they are the whole reason this is a keyed table rather
 * than a queue:
 *
 *   - 0x8002703C..0x80027054 scans all eight slots for slot+12 == this item
 *     and, on a hit, leaves with a0 = -1 (0x8002701C) which the `bltz` at
 *     0x80027088 turns into "fill nothing". An already-armed TIMER is NOT
 *     re-armed, re-rolled or reset. Without that, COMMAND 0x718, POWER1 0x408
 *     and POWER2 0x7a8 — CAT_B records with one trigger volume each and a fire
 *     count of 0 — would claim a fresh slot on every frame the player stood in
 *     the volume, and eight frames later no TIMER anywhere on the map could
 *     arm again.
 *   - 0x8002705C..0x80027084 then looks for a free slot (slot+12 == 0) and
 *     leaves with a0 = -2 when there is none. The request is dropped; nothing
 *     is evicted.
 */
s32 q2_event_timer_period(const q2_event_item *item)
{
    u32 base, range, roll;

    if (!item || item->len != 12 || !item->payload)
        return 0;
    base = (u32)item->payload[2] | ((u32)item->payload[3] << 8);
    range = (u32)item->payload[4] | ((u32)item->payload[5] << 8);
    roll = q2_random_next();
    return (s32)((base + ((range * roll) >> 15)) * 30u);
}

static void timer_arm(q2_event_rt *rt, u32 rec_offset,
                      const q2_event_item *item, u32 index)
{
    u32 s, slot = Q2_EVENT_RT_TIMER_MAX;

    for (s = 0; s < Q2_EVENT_RT_TIMER_MAX; s++) {
        if (rt->deferred[s].item == item->offset) {   /* 0x80027044 */
            rt->defer_timer  = false;
            rt->defer_ticks  = 0;
            rt->defer_fires  = 0;
            rt->defer_window = 0;
            return;
        }
    }
    for (s = 0; s < Q2_EVENT_RT_TIMER_MAX; s++) {
        if (rt->deferred[s].item == 0) {              /* 0x8002706C */
            slot = s;
            break;
        }
    }

    if (slot < Q2_EVENT_RT_TIMER_MAX) {
        s32 period = rt->timer_period ? rt->timer_period(item) : rt->defer_ticks;
        rt->deferred[slot].item        = item->offset;   /* 0x800270C0 */
        rt->deferred[slot].offset      = rec_offset;     /* 0x800270C4 */
        rt->deferred[slot].timer_index = (u8)index;
        rt->deferred[slot].next_item   = (u8)(index + 1u); /* 0x800270BC */
        rt->deferred[slot].window      = rt->defer_window; /* 0x800270C8 */
        rt->deferred[slot].fires       = rt->defer_fires;  /* 0x800270B0 */
        rt->deferred[slot].period      = period;  /* 0x80027104 */
        rt->deferred[slot].due         = rt->clock + period;
        rt->deferred_count++;
    }

    rt->defer_timer  = false;
    rt->defer_ticks  = 0;
    rt->defer_fires  = 0;
    rt->defer_window = 0;
}

/* 0x800273E0 `sw zero,12(s0)`: the fire count reached zero and the slot is
 * free again. */
static void timer_free(q2_event_rt *rt, u32 slot)
{
    memset(&rt->deferred[slot], 0, sizeof(rt->deferred[slot]));
    if (rt->deferred_count)
        rt->deferred_count--;
}

/*
 * 0x800273F0..0x8002742C: a fresh period into slot+0.
 *
 * The console re-rolls here — `jal 0x80089E28` then
 * `(base + ((range * rand()) >> 15)) * 30` off the TIMER item at slot+12.
 * Looking up the immutable item also works after a saved slot is restored.
 */
static void timer_rearm(q2_event_rt *rt, u32 slot)
{
    q2_event_record rec;
    q2_event_item item;

    if (rt->timer_period &&
        q2_events_record_at(&rt->events, rt->deferred[slot].offset, &rec) &&
        q2_events_get_item(&rt->events, &rec, rt->deferred[slot].timer_index, &item) &&
        item.offset == rt->deferred[slot].item)
        rt->deferred[slot].period = rt->timer_period(&item);
    rt->deferred[slot].due = rt->clock + rt->deferred[slot].period;
}

/* ------------------------------------------------------------------------- */
/*
 * ONE ITEM THROUGH THE DISPATCHER, 0x80027468 — its prologue and its call.
 *
 * Split out because the TIMER window enters the SAME function the record's own
 * item loop does — 0x800271F8 `jal 0x80027468` — so the disabled skip and the
 * one-shot latch have to be identical on both paths, and having two copies of
 * them was how they would stop being.
 *
 * Returns false for an item the prologue skipped, which is the dispatcher's 0
 * answer at 0x80027480: the caller CONTINUES rather than stopping. `*out` is
 * the answer of an item that did run.
 */
static bool dispatch_item(q2_event_rt *rt, const q2_event_item *item,
                          q2_event_outcome *out)
{
    /*
     * 0x80027474 `lbu v1,0(t0)` reads the op byte OUT OF THE CHUNK, which the
     * console writes back to. Here the chunk is borrowed const, so the mutable
     * half of that byte lives in item_flags[], keyed by the item's immutable
     * chunk offset. This used to read `item.op` straight from the file, which
     * meant the bit could be tested but never set.
     */
    u8 op = item->op;

    *out = Q2_EVENT_OK;

    if (rt->item_flags && item->offset < rt->item_flags_size)
        op = (u8)(op | rt->item_flags[item->offset]);

    /* 0x8002747C `andi v0,v1,0x80` / 0x80027480 `bne v0,zero,0x80027940`
     * with `addu v0,zero,zero` in the delay slot: a DISABLED item returns
     * 0, so the record CONTINUES to the next item rather than stopping. */
    if (op & Q2_EVOP_DISABLED)
        return false;

    /*
     * 0x80027488 `sll v0,v1,1` / 0x8002748C `andi v0,v0,0x80` /
     * 0x80027490 `andi v1,v1,0x7F` / 0x80027494 `or v1,v1,v0` /
     * 0x80027498 `sb v1,0(t0)` — op := (op & 0x7F) | ((op & 0x40) << 1),
     * i.e. DISABLED := ONESHOT. The item retires itself.
     *
     * IT HAPPENS HERE AND NOT INSIDE run_item(), because on the console it
     * happens BEFORE the opcode bounds check at 0x8002749C..0x800274A8 and
     * therefore before any handler's own length test. A one-shot item of a
     * dead opcode, or of a live opcode carrying a length its handler
     * rejects, still retires on its first dispatch and is never dispatched
     * again. 457 items on the disc carry the bit — 316 CALL, 73 FXGROUP,
     * 24 ENABLE, 29 MOVER_A, 9 DISABLE, 6 MOVER_C across 243 records —
     * and none carries 0x80, which is what makes bit 7 purely runtime.
     */
    if ((op & Q2_EVOP_ONESHOT) && rt->item_flags &&
        item->offset < rt->item_flags_size)
        rt->item_flags[item->offset] |= Q2_EVOP_DISABLED;

    *out = run_item(rt, item);
    return true;
}

/*
 * Run a record's items from `from`. Returns true when it DEFERRED — the record
 * is unfinished.
 *
 * That answer no longer decides whether the record is latched: the latch is
 * applied by record_begin() BEFORE this is called, exactly as 0x800279BC is
 * applied before the item loop at 0x80027A10. It is kept because it says
 * something true about what happened and both callers read as documentation.
 *
 * Split out of the update loop because a deferred record resumes at an
 * arbitrary item, and the two paths must otherwise behave identically: the
 * same disabled-item skip and latch, the same abort, the same zone-change
 * result.
 */
static bool run_items(q2_event_rt *rt, const q2_event_record *rec, u32 from,
                      u32 offset, q2_event_outcome *result)
{
    u32 i;

    for (i = from; i < rec->n_items; i++) {
        q2_event_item item;
        q2_event_outcome r;

        if (!q2_events_get_item(&rt->events, rec, i, &item))
            break;

        if (!dispatch_item(rt, &item, &r))
            continue;
        if (r == Q2_EVENT_ZONE_CHANGE)
            *result = Q2_EVENT_ZONE_CHANGE;

        /* A predicate said no. Everything after it in this record is what it
         * was guarding, so the record stops here - and it is still marked as
         * having run, exactly as the console's does, or a locked door would
         * re-test on every touch. */
        if (rt->abort_record) {
            rt->abort_record = false;
            break;
        }

        /*
         * A DISABLEME. The bit goes on now and the record still finishes: the
         * primitive sets it and returns, so what follows it in the record runs
         * this once and never again.
         */
        if (rt->disable_self) {
            s32 slot = record_slot(rt, offset);

            rt->disable_self = false;
            if (slot >= 0)
                rt->flags[slot] |= Q2_EVREC_DISABLED;
        }

        /*
         * A TIMER. The record stops here whether or not a slot was free —
         * 0x8002710C sets gp+16956 on every exit of 0x80026FEC, including the
         * two refusals, and 0x80027778 is where the executor reads it.
         */
        if (rt->defer_timer || rt->defer_ticks > 0) {
            timer_arm(rt, offset, &item, i);
            return true;
        }
    }

    return false;
}

/*
 * THE RECORD EXECUTOR'S PROLOGUE, shared by every path that starts or resumes
 * a record. Returns false when the record refuses to run.
 *
 * Retail has eight copies of this — the same gate and the same latch:
 *
 *   0x80027994..0x800279BC   the record executor (named trigger / queue seed)
 *   0x80027A60..0x80027A88   its own queue drain
 *   0x80027DA0..0x80027DC8   the name-trigger path
 *   0x80027FAC..0x80027FD4   the contact ENTER pass
 *   0x800280D8..0x80028100   the contact LEAVE pass
 *   0x80027174..0x800271C0   the TIMER continuation executor, 0x8002712C —
 *                            not contiguous: its gate (0x80027174..0x80027180)
 *                            and its latch (0x800271A0..0x800271C0) sit either
 *                            side of the queue reset at 0x80027190..0x8002719C
 *   0x80027290..0x800272B8   that continuation's own queue drain
 *   0x80029620..0x80029648   the EVE_ replay's queue drain, in 0x8002936C
 *
 * THE GATE IS THE DISABLED BIT AND NOTHING ELSE. 0x8002799C `andi v0,a0,0x80` /
 * 0x800279A0 `bne v0,zero` — HASRUN (bit 0) is never tested, anywhere. This
 * file used to gate on `ONESHOT && HASRUN`, which is a test the console does
 * not have, and it is what made ENABLE useless: 0x800278B0/0x800278B4 clears
 * ONLY bit 7 (`andi v0,v0,0x7F`; `sb`), so a re-armed record still carried
 * HASRUN and was still refused. 117 ENABLE list targets on the disc, 26 of them
 * records whose own flags carry 0x40 — every one of those re-arms was silently
 * doing nothing. The four in COMMON scripts: JAIL3 0x7b4 (flags 0x50) enabling
 * 0x384 and 0x370, both flags 0x48 with a single plain CALL STRING, which now
 * run that CALL again; and LAB 0x8ac -> 0x6d4 and 0x8fc -> 0x990, both 0x48,
 * which are re-armed too but whose every item carries 0x40 — so the ITEM latch
 * (0x80027498, in run_items) keeps a re-armed LAB record running nothing. That
 * is why the two latches have to land together: 447 of the disc's 457 one-shot
 * items (90 of the 92 in COMMON scripts) sit inside one-shot records, where the
 * record latch used to be all that kept them spent, and re-arming records
 * without latching items would replay every one an ENABLE brings back.
 *
 * THE LATCH IS DISABLED := ONESHOT, PLUS HASRUN. 0x800279A8 `sll v1,a0,1` /
 * 0x800279AC `andi v1,v1,0x80` / 0x800279B0 `andi v0,a0,0x7F` / 0x800279B4
 * `or v0,v0,v1` / 0x800279B8 `ori v0,v0,0x1` / 0x800279BC `sb v0,3(s2)`.
 * "One-shot" is not a separate mechanism: a one-shot record disables itself the
 * first time it runs, which is why ENABLE can bring it back.
 *
 * AND IT IS APPLIED BEFORE THE ITEM LOOP. The store is at 0x800279BC; the
 * skip-to-resume walk does not start until 0x800279C4 and the first item is not
 * dispatched until 0x80027A10. So a record that aborts on its first item, or
 * defers, or has no items at all, is latched exactly the same. This file used
 * to OR HASRUN in AFTER run_items and only when the record had not deferred,
 * which was wrong in both of those ways at once.
 *
 * (The refusal returns 1, not 0 — 0x800279A4 `addiu v0,zero,1` is the branch
 * delay slot. Neither of the two callers reads the value, so nothing here needs
 * to model it.)
 */
static bool record_begin(q2_event_rt *rt, s32 slot)
{
    u8 f;

    if (slot < 0 || (u32)slot >= rt->record_count)
        return false;

    f = rt->flags[slot];

    if (f & Q2_EVREC_DISABLED)      /* 0x8002799C, 0x8002717C */
        return false;

    rt->flags[slot] = (u8)((f & 0x7Fu) |
                           ((u32)(f & Q2_EVREC_ONESHOT) << 1) |
                           Q2_EVREC_HASRUN);   /* 0x800279A8..0x800279BC */
    rt->ran_count++;
    return true;
}

/*
 * THE TIMER CONTINUATION EXECUTOR, 0x8002712C, called from the sweep at
 * 0x800273B4 with the slot in a0.
 *
 * It is not "run the rest of the record", which is what this port used to do.
 * It is a WINDOW of `slot+6` items that walks round the record for ever,
 * stepping over the TIMER item on each lap.
 */
static void timer_fire(q2_event_rt *rt, u32 slot, q2_event_outcome *result)
{
    q2_event_record rec;
    u32 start, idx;
    s32 remaining;

    if (!q2_events_record_at(&rt->events, rt->deferred[slot].offset, &rec))
        return;

    /* 0x80027174..0x800271C0: the same gate and the same latch as every other
     * executor (record_begin), ahead of the walk to the resume pointer. The
     * sweep never reads the answer, so a refused resume still spends a fire. */
    if (!record_begin(rt, record_slot(rt, rt->deferred[slot].offset)))
        return;

    start = rt->deferred[slot].next_item;

    /*
     * 0x800271BC..0x800271D4: turning slot+16 back into an index is a walk
     * bounded by the record's item count, and it RETURNS 0 when it runs out
     * rather than wrapping. That is what a TIMER with nothing after it in its
     * record does — the resume pointer 0x800270BC stored is the record's end —
     * so such a slot runs nothing, ever, and only burns its fire count.
     */
    if (start >= rec.n_items)
        return;

    idx       = start;
    remaining = (s32)rt->deferred[slot].window;   /* s3 = lh 6(s4), 0x800271F0 */

    /*
     * A do-while: 0x800271F4 is the loop head and the decrement and test are
     * at 0x80027250/0x80027254, so a window of 0 or 1 runs exactly one item.
     */
    do {
        q2_event_item item;
        q2_event_outcome r = Q2_EVENT_OK;

        if (!q2_events_get_item(&rt->events, &rec, idx, &item))
            break;

        if (dispatch_item(rt, &item, &r)) {
            if (r == Q2_EVENT_ZONE_CHANGE)
                *result = Q2_EVENT_ZONE_CHANGE;

            /* A DISABLEME inside the window disables the record being
             * continued: 0x80027170 put its offset in gp+16936 for the whole
             * loop, and 0x8002EAA8 reads it from there. */
            if (rt->disable_self) {
                s32 s = record_slot(rt, rt->deferred[slot].offset);

                rt->disable_self = false;
                if (s >= 0)
                    rt->flags[s] |= Q2_EVREC_DISABLED;
            }

            /* And a TIMER inside the window claims its own slot, keyed on its
             * own item — which is how a record can carry two of them. */
            if (rt->defer_timer || rt->defer_ticks > 0)
                timer_arm(rt, rt->deferred[slot].offset, &item, idx);
        }

        /*
         * THE WINDOW DOES NOT READ THE ABORT FLAG. 0x8002712C's loop tests the
         * dispatcher's return value (0x80027200) and nothing else — gp+16956
         * is read only by the record executor, at 0x80027778 — so an ONKEYDO
         * that refuses inside a window stops nothing. Dropped rather than
         * honoured, and unreachable on this disc: no record that carries a
         * TIMER carries an ONKEYDO.
         */
        rt->abort_record = false;

        /*
         * 0x8002720C..0x80027240: step to the next item. Running off the end
         * goes back to item 0, walks up to the TIMER item at slot+12
         * (0x8002721C..0x8002723C) and steps past it (0x80027240), so the lap
         * resumes after the TIMER and never dispatches it again.
         */
        if (++idx >= rec.n_items)
            idx = (u32)rt->deferred[slot].timer_index + 1u;

        if (idx == start)                    /* 0x8002724C: full circle */
            break;
        if (r == Q2_EVENT_ZONE_CHANGE)       /* 0x80027200 */
            break;
    } while (--remaining > 0);

    rt->deferred[slot].next_item = (u8)idx;  /* 0x80027270 */
}

/* ------------------------------------------------------------------------- */
/* 0x8002EF1C -> 0x80027950 — the weapon router's resume                      */
/* ------------------------------------------------------------------------- */
bool q2_event_rt_resume_after_item(q2_event_rt *rt, u32 record_offset,
                                   u32 item_offset, q2_event_outcome *out)
{
    q2_event_record rec;
    u32 i;

    if (out)
        *out = Q2_EVENT_OK;
    if (!rt || !rt->events.data)
        return false;
    if (!q2_events_record_at(&rt->events, record_offset, &rec))
        return false;

    /* 0x80027994's DISABLED gate and 0x800279A8's latch, both applied before
     * the walk to the resume pointer, exactly as the ordinary executor does.
     * A record already DISABLED loses its crate and runs no tail. */
    if (!record_begin(rt, record_slot(rt, record_offset)))
        return false;

    /*
     * 0x800279C0..0x800279EC. The router passes `item + len`, the item AFTER
     * the one that fired, and the executor walks to it before entering the
     * item loop. Walking by offset rather than by index because that is what
     * the console has in hand: obj+0x3E is the item's own byte offset.
     */
    for (i = 0; i < rec.n_items; i++) {
        q2_event_item item;

        if (!q2_events_get_item(&rt->events, &rec, i, &item))
            return false;
        if (item.offset == item_offset) {
            q2_event_outcome r = Q2_EVENT_OK;

            run_items(rt, &rec, i + 1, record_offset, &r);
            if (out)
                *out = r;
            return true;
        }
    }
    return false;
}

void q2_event_rt_advance(q2_event_rt *rt, s32 ticks)
{
    if (rt) {
        rt->clock += ticks;
        /* The console's per-frame counter at 0x800B2DE4 (0x800705C4 `addiu
         * a1,a1,1`, stored back at 0x80070610), which is what a WAIT compares
         * against at 0x800276D8. One call here is one frame — see the field
         * comment; the mapping is INFERRED from the single call site. */
        rt->tick++;
    }
}

static void drain_pending(q2_event_rt *rt, q2_event_outcome *result);

q2_event_outcome q2_event_rt_update(q2_event_rt *rt)
{
    q2_event_outcome result = Q2_EVENT_OK;

    if (!rt)
        return Q2_EVENT_OK;

    /*
     * THE TIMER SWEEP, 0x80027340. Anything that has come due runs BEFORE new
     * triggers are taken, so a timer that has expired runs on the frame it
     * expires rather than behind whatever else the player has just walked into.
     *
     * A slot with slot+12 == 0 is free (0x8002737C). The console holds a
     * COUNTDOWN in slot+0 and subtracts the frame's delta from it (0x8002738C..
     * 0x800273A4, due when it goes negative); this runtime holds a deadline
     * against the same clock instead. Equality must still wait: the original
     * branches past the fire on `bgez`, including a countdown of exactly zero.
     */
    {
        u32 s;

        for (s = 0; s < Q2_EVENT_RT_TIMER_MAX; s++) {
            if (rt->deferred[s].item == 0)
                continue;
            if (rt->deferred[s].due >= rt->clock)
                continue;

            rt->resumed_count++;
            timer_fire(rt, s, &result);

            /*
             * 0x800273BC..0x800273EC: the fire count is spent whatever the
             * continuation did — v0 is reloaded from slot+8 straight after the
             * `jal`, so the sweep never reads its answer and a refused resume
             * spends a fire exactly as a completed one does. Zero frees the
             * slot; a count that was ALREADY zero underflows to -1 and is
             * clamped back to 0 at 0x800273EC, which is what makes 0 mean
             * forever.
             */
            if (rt->deferred[s].fires == 0) {
                timer_rearm(rt, s);
            } else if (--rt->deferred[s].fires == 0) {
                timer_free(rt, s);           /* 0x800273E0 */
            } else {
                timer_rearm(rt, s);
            }
        }
    }

    drain_pending(rt, &result);
    return result;
}

/*
 * Run everything queued, which may queue more. Shared by q2_event_rt_update
 * and the EVE_ replay, whose drain at 0x80029604..0x80029698 is the same gate,
 * latch and item loop as every other (see record_begin).
 */
static void drain_pending(q2_event_rt *rt, q2_event_outcome *result)
{
    u32 guard = 0;

    while (rt->pending_count > 0 && guard < Q2_EVENT_RT_MAX_PER_UPDATE) {
        u32 offset;
        s32 slot;
        q2_event_record rec;

        /* Take from the front so triggers fire in the order they were queued. */
        offset = rt->pending[0];
        memmove(rt->pending, rt->pending + 1,
                (size_t)(rt->pending_count - 1) * sizeof(u32));
        rt->pending_count--;
        guard++;

        slot = record_slot(rt, offset);
        if (slot < 0)
            continue;

        /* Gate and latch, both before the first item — see record_begin. */
        if (!record_begin(rt, slot))
            continue;

        if (!q2_events_record_at(&rt->events, offset, &rec))
            continue;

        run_items(rt, &rec, 0, offset, result);
    }
}

/* ------------------------------------------------------------------------- */
/* Across a zone change — see the note above q2_event_carry                  */
/* ------------------------------------------------------------------------- */
/*
 * FNV-1a over the chunk. Only an identity check: two scripts that happened to
 * share a size and a record count must not trade latches.
 */
static u32 chunk_identity(const q2_events *ev)
{
    u32 h = 2166136261u;
    u32 i;

    for (i = 0; ev->data && i < ev->size; i++) {
        h ^= ev->data[i];
        h *= 16777619u;
    }
    return h;
}

static bool carry_matches(const q2_event_rt *rt, const q2_event_carry *c)
{
    return c->size != 0 && rt->flags &&
           c->size == rt->events.size &&
           c->record_count == rt->record_count &&
           c->identity == chunk_identity(&rt->events);
}

void q2_event_rt_carry_out(const q2_event_rt *rt, q2_event_carry *out)
{
    if (!out)
        return;

    out->size         = 0;
    out->record_count = 0;
    out->identity     = 0;

    if (!rt || !rt->flags || rt->record_count == 0 ||
        rt->events.size > Q2_EVENT_CARRY_MAX ||
        rt->record_count > Q2_EVENT_CARRY_MAX / 4)
        return;

    memset(out->record_flags, 0, sizeof(out->record_flags));
    memset(out->item_flags, 0, sizeof(out->item_flags));
    memset(out->destroyed_items, 0, sizeof(out->destroyed_items));

    /* The whole byte, contact bits too: the console's copy is not touched by
     * the zone load, so RT2 still says "inside last frame" in the next zone
     * and a CAT_A volume the player is standing in does not fire again. */
    memcpy(out->record_flags, rt->flags, rt->record_count);
    if (rt->item_flags && rt->item_flags_size <= Q2_EVENT_CARRY_MAX)
        memcpy(out->item_flags, rt->item_flags, rt->item_flags_size);
    if (rt->object_flags && rt->item_flags_size <= Q2_EVENT_CARRY_MAX)
        memcpy(out->destroyed_items, rt->object_flags, rt->item_flags_size);

    out->size         = rt->events.size;
    out->record_count = rt->record_count;
    out->identity     = chunk_identity(&rt->events);
}

void q2_event_rt_carry_in(q2_event_rt *rt, const q2_event_carry *in)
{
    if (!rt || !in || !carry_matches(rt, in))
        return;

    memcpy(rt->flags, in->record_flags, rt->record_count);
    if (rt->item_flags && rt->item_flags_size == in->size)
        memcpy(rt->item_flags, in->item_flags, rt->item_flags_size);
    if (rt->object_flags && rt->item_flags_size == in->size)
        memcpy(rt->object_flags, in->destroyed_items, rt->item_flags_size);
}

/*
 * One item of the replay: 0x800295C0..0x800295D0 and the dispatcher it calls.
 *
 * The replay clears bit 7 BEFORE the dispatch (`andi v0,v0,0x7F`, stored by
 * the `sb` in the delay slot of the jal at 0x800295CC), so the dispatcher's
 * DISABLED test at 0x8002747C always passes here and is not repeated; its
 * latch at 0x80027498 then puts DISABLED := ONESHOT back. (Clearing only the
 * runtime half: the authored bit 7 lives in the const chunk and no item on the
 * disc has one.)
 *
 * The answer is discarded — 0x800295D4 overwrites v0 — so an abort does not
 * stop the replay of this record, and the abort flag is dropped with it.
 * DISABLEME and TIMER still act, on the record being replayed: gp+16936 holds
 * its offset for the whole item loop (0x800294D0).
 */
static void replay_item(q2_event_rt *rt, const q2_event_item *item,
                        u32 index, u32 rec_offset)
{
    if (rt->item_flags && item->offset < rt->item_flags_size) {
        rt->item_flags[item->offset] =
            (u8)(rt->item_flags[item->offset] & ~(unsigned)Q2_EVOP_DISABLED);
        if (item->op & Q2_EVOP_ONESHOT)
            rt->item_flags[item->offset] |= Q2_EVOP_DISABLED;
    }

    rt->replayed_count++;
    (void)run_item(rt, item);
    rt->abort_record = false;

    if (rt->disable_self) {
        s32 slot = record_slot(rt, rec_offset);

        rt->disable_self = false;
        if (slot >= 0)
            rt->flags[slot] |= Q2_EVREC_DISABLED;
    }

    /* A TIMER the replay re-dispatches claims a slot the same way, with the
     * same fire count and window — otherwise a timer that survives a zone seam
     * would come back as a window-less one-shot. */
    if (rt->defer_timer || rt->defer_ticks > 0)
        timer_arm(rt, rec_offset, item, index);
}

/* 0x80029094 `addiu s2,zero,128` in the writer; `slti v0,v0,128` at
 * 0x800294DC and 0x80029570 in the reader. */
#define EVE_BIT_POSITIONS 128

void q2_event_rt_replay(q2_event_rt *rt, const q2_event_carry *carry)
{
    q2_event_outcome ignored = Q2_EVENT_OK;
    q2_event_record rec;
    u32 pos = 0, n = 0;

    if (!rt || !carry || !carry_matches(rt, carry))
        return;

    rt->initial_pass = true;                        /* 0x800294B4 */

    if (q2_events_first_record(&rt->events, &rec)) {
        do {
            bool rec_bit = false;
            u32 i;

            /* The runtime seeds `offsets` in this same walk, so the n-th
             * record walked is slot n — the carry is indexed the same way. */
            if (n >= rt->record_count || rt->offsets[n] != rec.offset)
                break;

            /* 0x800294D4..0x80029508: the writer's test was (flags & 0x81) ==
             * 0x81 (0x800291B8..0x800291C4); a set bit ORs 0x81 back in. */
            if (pos < EVE_BIT_POSITIONS &&
                (carry->record_flags[n] &
                 (Q2_EVREC_DISABLED | Q2_EVREC_HASRUN)) ==
                    (Q2_EVREC_DISABLED | Q2_EVREC_HASRUN)) {
                rec_bit = true;
                rt->flags[n] |= (u8)(Q2_EVREC_DISABLED | Q2_EVREC_HASRUN);
            }
            pos++;                                  /* 0x80029524 */

            /* 0x80029538/0x8002953C: both cursors back to the base. */
            rt->pending_count = 0;

            for (i = 0; i < rec.n_items; i++) {
                q2_event_item item;
                bool item_bit = false;

                if (!q2_events_get_item(&rt->events, &rec, i, &item))
                    break;

                /* 0x8002955C: an item owns a bit position only if it is
                 * ONESHOT — authored bit 6, which nothing writes at run time.
                 * Its bit is what its bit 7 was when the block was written
                 * (0x8002923C), which is what the carry kept. */
                if (item.op & Q2_EVOP_ONESHOT) {
                    if (pos < EVE_BIT_POSITIONS && item.offset < carry->size &&
                        (carry->item_flags[item.offset] & Q2_EVOP_DISABLED))
                        item_bit = true;
                    pos++;                          /* 0x800295A8 */
                }

                /* 0x800295AC: a spent record replays every item; otherwise
                 * only a spent one-shot item does. */
                if (rec_bit || item_bit ||
                    (item.offset < carry->size &&
                     carry->destroyed_items[item.offset]))
                    replay_item(rt, &item, i, rec.offset);
            }

            drain_pending(rt, &ignored);            /* 0x8002969C */
            n++;
        } while (q2_events_next_record(&rt->events, &rec, &rec));
    }

    rt->initial_pass = false;                       /* 0x80029754 */
}

/* ------------------------------------------------------------------------- */
/* The blocks themselves — one carry per map (events_rt.h)                     */
/* ------------------------------------------------------------------------- */
/*
 * The key compare. Case-blind for the reason every other name compare in this
 * port is: the executable's level table and the ISO directory do not agree on
 * case, so an exact compare would file the same map under two names and the
 * second visit would find nothing.
 */
static bool block_name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = (int)(unsigned char)*a++;
        int cb = (int)(unsigned char)*b++;

        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb)
            return false;
    }
    return *a == '\0' && *b == '\0';
}

q2_event_carry *q2_event_store_find(q2_event_store *s, const char *map)
{
    u32 i;

    if (!s || !map || !map[0])
        return NULL;

    for (i = 0; i < s->count; i++)
        if (block_name_eq(s->block[i].map, map))
            return &s->block[i].carry;
    return NULL;
}

q2_event_carry *q2_event_store_open(q2_event_store *s, const char *map)
{
    q2_event_carry *have = q2_event_store_find(s, map);
    q2_event_block *grown;
    u32 cap, n;

    if (have)
        return have;
    if (!s || !map || !map[0])
        return NULL;

    if (s->count == s->cap) {
        cap = s->cap ? s->cap * 2u : 8u;
        grown = (q2_event_block *)realloc(s->block,
                                          (size_t)cap * sizeof(*grown));
        if (!grown)
            return NULL;
        s->block = grown;
        s->cap   = cap;
    }

    grown = &s->block[s->count++];
    memset(grown, 0, sizeof(*grown));
    for (n = 0; n + 1 < Q2_EVENT_BLOCK_NAME_MAX && map[n]; n++)
        grown->map[n] = map[n];
    grown->map[n] = '\0';
    return &grown->carry;
}

void q2_event_store_clear(q2_event_store *s)
{
    if (s)
        s->count = 0;
}

void q2_event_store_free(q2_event_store *s)
{
    if (!s)
        return;
    free(s->block);
    s->block = NULL;
    s->count = 0;
    s->cap   = 0;
}
