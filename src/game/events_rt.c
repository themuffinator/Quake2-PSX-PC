#include "events_rt.h"

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

    if (events->record_count == 0)
        return Q2_OK;                 /* a real state on 36 chunks */

    rt->flags   = (u8 *)calloc(events->record_count, sizeof(u8));
    rt->offsets = (u32 *)calloc(events->record_count, sizeof(u32));
    if (!rt->flags || !rt->offsets) {
        q2_event_rt_free(rt);
        return Q2_ERR_NO_MEMORY;
    }

    /* Seed the shadow from the file. Bits 0-2 are runtime state and are clear
     * on disc, so the stored byte is a valid initial value as-is. */
    if (q2_events_first_record(&rt->events, &rec)) {
        do {
            if (n >= events->record_count)
                break;
            rt->offsets[n] = rec.offset;
            rt->flags[n]   = rec.flags;
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

bool q2_event_rt_trigger(q2_event_rt *rt, u32 offset)
{
    u32 i;

    if (!rt || rt->pending_count >= Q2_EVENT_RT_PENDING_MAX)
        return false;
    if (record_slot(rt, offset) < 0)
        return false;

    /* Queueing the same record twice in one update would run it twice. */
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

        /* 0x80028070..0x80028084: CAT_C plus previous contact and no current
         * contact is the leave edge. */
        if ((flags & Q2_EVREC_CAT_C) && was && !now)
            q2_event_rt_trigger(rt, rt->offsets[i]);

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

    case Q2_EVOP_ZONEGATE:
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
         */
        if (item->payload && item->len >= Q2_EVENT_ITEM_HEADER_SIZE + 12) {
            u32 i, digits = 0, value = 0;

            memset(rt->pending_zone_name, 0, sizeof(rt->pending_zone_name));
            for (i = 0; i < 12; i++) {
                char ch = (char)item->payload[i];
                if (ch == '\0')
                    break;
                rt->pending_zone_name[i] = ch;
                if (ch >= '0' && ch <= '9') {
                    value = value * 10u + (u32)(ch - '0');
                    digits++;
                } else if (digits) {
                    /* Digits then a non-digit: not a trailing number. */
                    digits = 0;
                    value  = 0;
                }
            }

            if (!digits)
                return Q2_EVENT_OK;      /* nothing to resolve; do not guess */

            rt->has_zone_change = true;
            rt->pending_zone    = value;
        }
        return Q2_EVENT_ZONE_CHANGE;

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

    default:
        /* WAIT and anything else: recognised but not yet implemented.
         * Counting them separately from movers would imply more certainty
         * about them than we have. */
        return Q2_EVENT_OK;
    }
}

/* ------------------------------------------------------------------------- */
/*
 * Run a record's items from `from`. Returns true when it DEFERRED — the record
 * is unfinished, so the caller must not latch it as having run.
 *
 * Split out of the update loop because a deferred record resumes at an
 * arbitrary item, and the two paths must otherwise behave identically: the
 * same disabled-item skip, the same abort, the same zone-change result.
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
        if (item.op & Q2_EVOP_DISABLED)
            continue;

        r = run_item(rt, &item);
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

        /* A TIMER. The rest of the record waits. */
        if (rt->defer_ticks > 0) {
            if (rt->deferred_count < Q2_EVENT_RT_PENDING_MAX) {
                rt->deferred[rt->deferred_count].offset    = offset;
                rt->deferred[rt->deferred_count].next_item = (u8)(i + 1);
                rt->deferred[rt->deferred_count].due =
                    rt->clock + rt->defer_ticks;
                rt->deferred_count++;
            }
            rt->defer_ticks = 0;
            return true;
        }
    }

    return false;
}

void q2_event_rt_advance(q2_event_rt *rt, s32 ticks)
{
    if (rt)
        rt->clock += ticks;
}

q2_event_outcome q2_event_rt_update(q2_event_rt *rt)
{
    q2_event_outcome result = Q2_EVENT_OK;
    u32 guard = 0;

    if (!rt)
        return Q2_EVENT_OK;

    /* Anything that has come due resumes BEFORE new triggers are taken, so a
     * timer that has expired runs on the frame it expires rather than behind
     * whatever else the player has just walked into. */
    {
        u32 d = 0;

        while (d < rt->deferred_count) {
            q2_event_record rec;
            s32 due_slot;

            if (rt->deferred[d].due > rt->clock) {
                d++;
                continue;
            }

            if (q2_events_record_at(&rt->events, rt->deferred[d].offset, &rec)) {
                u32 from = rt->deferred[d].next_item;
                u32 off  = rt->deferred[d].offset;

                /* Drop the entry before running, or a record that timers twice
                 * would push onto a list it is being walked out of. */
                rt->deferred_count--;
                memmove(&rt->deferred[d], &rt->deferred[d + 1],
                        (size_t)(rt->deferred_count - d) *
                        sizeof(rt->deferred[0]));

                rt->resumed_count++;
                if (!run_items(rt, &rec, from, off, &result)) {
                    due_slot = record_slot(rt, off);
                    if (due_slot >= 0) {
                        rt->flags[due_slot] |= Q2_EVREC_HASRUN;
                        rt->ran_count++;
                    }
                }
                continue;
            }

            rt->deferred_count--;
            memmove(&rt->deferred[d], &rt->deferred[d + 1],
                    (size_t)(rt->deferred_count - d) * sizeof(rt->deferred[0]));
        }
    }

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

        if (rt->flags[slot] & Q2_EVREC_DISABLED)
            continue;

        /* A one-shot that has already run does not run again. */
        if ((rt->flags[slot] & Q2_EVOP_ONESHOT) &&
            (rt->flags[slot] & Q2_EVREC_HASRUN))
            continue;

        if (!q2_events_record_at(&rt->events, offset, &rec))
            continue;

        if (run_items(rt, &rec, 0, offset, &result))
            continue;   /* deferred: it is not finished, so do not latch it */

        rt->flags[slot] |= Q2_EVREC_HASRUN;
        rt->ran_count++;
    }

    return result;
}
