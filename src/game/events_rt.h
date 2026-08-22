/*
 * events_rt.h — the event runtime: triggers, arming, and zone gates.
 *
 * The Events chunks are a compiled script. events.h parses them; this executes
 * them. Together they give the game its trigger graph, which is what turns a
 * walkable level into something with progression.
 *
 * ---------------------------------------------------------------------------
 * What works and what does not, and why
 * ---------------------------------------------------------------------------
 * WORKS: trigger, enable, disable, zone gates, movers, direct script damage,
 * and therefore level progression, teleports, doors and lifts. The generic
 * runtime reports the owner-specific operations through callbacks; the client
 * installs those callbacks when it owns the matching mover and simulation
 * sets.
 *
 * Mover operands are Scene-node indices on disc and runtime-object indices
 * after the console's load-time pre-pass. The port deliberately does not
 * reproduce that in-place rewrite: mover.h builds its own objects from the
 * pristine data, then the callback keys them by the item's immutable chunk
 * offset. That preserves the retail mapping without making borrowed Events
 * bytes mutable.
 *
 * ---------------------------------------------------------------------------
 * Mutable state
 * ---------------------------------------------------------------------------
 * A record's flags byte carries runtime state — has-run, one-shot, disabled —
 * and the engine writes it back. The chunk here is borrowed and read-only, so
 * this keeps a shadow array of flags indexed by record offset. Bits 0-2 are
 * runtime and are clear on disc, which is what makes the shadow safe to seed
 * straight from the file.
 */
#ifndef Q2PSX_EVENTS_RT_H
#define Q2PSX_EVENTS_RT_H

#include "events.h"
#include "q2psx.h"

/* The engine's own limit on simultaneously live script objects. */
#define Q2_EVENT_RT_PENDING_MAX 64

typedef enum q2_event_outcome {
    Q2_EVENT_OK = 0,
    Q2_EVENT_ZONE_CHANGE,   /* a zone gate fired; see pending_zone */
    Q2_EVENT_UNSUPPORTED    /* an owner-specific item had no hook  */
} q2_event_outcome;

typedef struct q2_event_rt {
    q2_events events;

    u8  *flags;         /* shadow of each record's mutable flags byte      */
    u32 *offsets;       /* record offsets, parallel to flags               */
    u32  record_count;

    u32  pending[Q2_EVENT_RT_PENDING_MAX];
    u32  pending_count;

    /* Set when a zone gate fires. The caller is responsible for the load. */
    bool has_zone_change;
    u32  pending_zone;
    /* The 12-byte NAME12 the gate actually carries — "Zone0", "Zone1". The
     * index above is its trailing decimal; the name is kept so an owner can
     * check the zone exists before loading rather than trusting the parse. */
    char pending_zone_name[13];

    /* Counters, so a caller can tell "nothing happened" from "nothing is
     * implemented yet". */
    u32  resumed_count; /* deferred records that came due and finished */
    u32  ran_count;
    u32  skipped_movers;

    /*
     * Called for each Q2_EVOP_CALL item a running record reaches, with the
     * item and its UserFuncs index.
     *
     * The runtime deliberately knows nothing about primitives: which index is
     * SIMROT is a per-map question that `userfuncs.[ch]` answers and this
     * module has no map. So a CALL is reported rather than interpreted, and
     * the owner — who does hold the map's UserFuncs and its rotator set —
     * decides what it means. Without this a rotator never receives the step
     * request `q2_rotators_tick` waits for, and a level's turning geometry
     * stands still: `rot moved 0` on every map.
     */
    void (*on_call)(void *user, const q2_event_item *item, u8 call_index);
    void  *on_call_user;

    /*
     * A MOVER opcode, reported for the same reason a CALL is: the runtime
     * knows a door was asked to open and not WHICH door, because a mover's
     * identity is the item's chunk offset and the SET that maps it lives with
     * the owner (mover.h). Without this `q2_movers_build` and
     * `q2_mover_trigger` had no callers at all and every door and lift on the
     * disc stood still — 1,006 MOVER_A items, 20 MOVER_B, 292 MOVER_C.
     */
    void (*on_mover)(void *user, const q2_event_item *item);
    void  *on_mover_user;

    /*
     * An FXGROUP opcode — a `func_explosive` a script has reached, which the
     * console destroys on the spot.
     *
     * 0x800276B0 dispatches the item with a2 zero, and 0x80026808 makes a zero
     * damage argument skip the hit-point subtract and fall straight into the
     * destruction. So a record naming one of these blows it up, the same way a
     * record naming GLASS shatters it.
     *
     * Reported rather than interpreted, for the third time and the same reason:
     * the item's identity is its chunk offset and the SET that maps it belongs
     * to the owner (explosive.h). Without this the opcode was counted as
     * "recognised but not implemented" and 224 destroyable groups stood intact.
     */
    void (*on_explosive)(void *user, const q2_event_item *item);
    void  *on_explosive_user;

    u32   explosive_count;   /* FXGROUP items the runtime has reached */

    /*
     * An FX opcode — direct T_Damage from a script rather than an effect
     * group. The operands are native to Events, but its target is the live
     * player, so the runtime reports it to its simulation owner. Unlike the
     * graphical-sounding name suggests, the seven PAL-disc items are hazards:
     * lava in BIGGUN and a 65-point world hit in WASTE3.
     */
    void (*on_fx)(void *user, const q2_event_item *item);
    void  *on_fx_user;

    u32   fx_count;          /* valid FX items the runtime has reached */

    /*
     * ABORT THE REST OF THIS RECORD — the engine's own `gp[0x423C]`.
     *
     * `ONKEYDO` is a predicate: it tests the player's key bits and, when they
     * do not satisfy it, stops the record it sits in so the items AFTER it do
     * not run. The console expresses that as a flag the primitive sets and the
     * record executor reads, which is why this is a field rather than a return
     * value — a hook that only reports a CALL has nowhere to put an answer.
     *
     * Set it from an `on_call` hook; the executor clears it.
     */
    bool abort_record;

    /*
     * DEFER THE REST OF THIS RECORD — what `TIMER` means.
     *
     * `TIMER` is "a delayed continuation of the rest of the record": the items
     * before it run now and the ones after it run later. Expressed the same way
     * the abort is, as a field an `on_call` hook writes, because the hook
     * reports a CALL and has nowhere else to put an answer. Non-zero stops the
     * record HERE and queues its remainder for `clock + defer_ticks`.
     *
     * The delay is the primitive's: `(base + ((range * rand()) >> 15)) * 30`,
     * and the 30 is not the 300 everything else on this clock uses — which
     * `userfuncs.c` calls out and which is why the caller computes it.
     */
    s32 defer_ticks;

    /*
     * DISABLE THE RECORD THAT IS RUNNING — what `DISABLEME` means.
     *
     * `0x8002EAA8` reads the record currently being executed out of gp+16936,
     * adds the Events base and ORs 0x80 into its header byte at +3. That bit is
     * the DISABLED flag the dispatcher tests at 0x8002799C before it runs
     * anything, so the record never runs again.
     *
     * Written by an `on_call` hook for the same reason `defer_ticks` is: the
     * hook reports a CALL and has nowhere else to put an answer. Unlike the
     * defer it does not stop the record — the console's primitive sets a bit
     * and returns, so the items after it still run this once.
     */
    bool disable_self;

    /*
     * Records waiting to resume, and the clock they are waiting against.
     * `q2_event_rt_advance` moves the clock; `q2_event_rt_update` runs whatever
     * has come due before it takes anything new off the queue.
     */
    struct {
        u32 offset;
        u8  next_item;
        s32 due;
    }    deferred[Q2_EVENT_RT_PENDING_MAX];
    u32  deferred_count;
    s32  clock;

    u32  mover_count;   /* MOVER items reported                          */
    u32  call_count;    /* CALL items reported, for the same "did anything
                         * happen" reason as the counters above */
} q2_event_rt;

q2_result q2_event_rt_init(q2_event_rt *rt, const q2_events *events);

/* Move the runtime's own clock, in the simulation's tick units. Deferred
 * records come due against it. */
void q2_event_rt_advance(q2_event_rt *rt, s32 ticks);
void      q2_event_rt_free(q2_event_rt *rt);

/* Queue the record at `offset` to run on the next update. */
bool q2_event_rt_trigger(q2_event_rt *rt, u32 offset);

/* Queue a record by its name in the Events directory. */
bool q2_event_rt_trigger_named(q2_event_rt *rt, const char *name);

/*
 * Run everything queued, which may queue more. Returns what happened.
 *
 * Bounded rather than run-to-empty: a script that triggers itself would
 * otherwise spin forever, and the original ran its queue once per tick.
 */
q2_event_outcome q2_event_rt_update(q2_event_rt *rt);

/* Current flags of the record at `offset`, or 0 if unknown. */
u8 q2_event_rt_flags(const q2_event_rt *rt, u32 offset);

#endif /* Q2PSX_EVENTS_RT_H */
