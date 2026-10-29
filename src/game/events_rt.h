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
 *
 * An ITEM's op byte carries the same two top bits and the engine writes THAT
 * back too (0x80027488..0x80027498), so there is a second shadow, `item_flags`,
 * indexed by item offset. And opcode 0x09 WAIT mutates two more fields inside
 * its own item, which is what the `wait[]` slots are for. Between them they
 * cover what the record executor and its built-in item handlers write back.
 * The CALL primitives' own writes into their operands are not here; those
 * belong to whichever owner implements the primitive.
 */
#ifndef Q2PSX_EVENTS_RT_H
#define Q2PSX_EVENTS_RT_H

#include "events.h"
#include "q2psx.h"

/*
 * Capacity of the pending queue.
 *
 * WHAT THE CONSOLE'S QUEUE ACTUALLY IS, because this constant used to be
 * justified with the wrong array. TRIGGER's handler at 0x800274CC pushes u16
 * record offsets through the write cursor at gp+16944 (0x800274FC..0x80027514)
 * with NO capacity test anywhere in the loop and no scan of what is already
 * queued. Its base is 0x800C6F24; every pass writes its seed there and starts
 * the write cursor at base+2 (0x80027D5C, 0x80027F68, 0x80028094 — arithmetic
 * on the base, which is why no xref names 0x800C6F26).
 *
 * THE ROOM IS 16 HALFWORDS, NOT 40. This note used to say nothing between
 * 0x800C6F26 and the TIMER slots at 0x800C6F74 is referenced, and conclude 40.
 * Running xrefs on every byte of that range over the whole image finds two
 * other arrays inside it: 0x800C6F44, materialised at 0x80028CD8 and
 * 0x8002E41C (halfwords indexed by gp+16940), and 0x800C6F54, materialised at
 * 0x8002A068 and 0x8002E250 (4-byte pairs, count gp+16916, bounded by
 * `slti v0,v1,8` at 0x8002E240 — eight of them, up to 0x800C6F73). So the
 * queue has 0x800C6F24..0x800C6F43 to itself and entry 16 lands on 0x800C6F44.
 *
 * The disc goes past that once. Walking the TRIGGER chain from every record in
 * every container with retail's FIFO (no dedupe, gates ignored), the deepest
 * drain is SECURITY record 0x9fc at 30 entries (29 pushed by TRIGGER items,
 * plus the seed the contact pass writes at 0x80027F80), in COMMON and in each
 * of its three zones' copies — and it is the only seed on the disc deeper than
 * 16. On the console its last 14 entries would be written over those two
 * arrays. This queue shares storage with nothing, so the port cannot reproduce
 * that overlap and does not try; what the overwritten arrays would then do is
 * not established here.
 *
 * The comment this replaces read "the engine's own limit on simultaneously live
 * script objects". That limit is `slti v0,v0,48` at 0x80025E48 and it bounds
 * the 92-byte runtime-OBJECT array at 0x800D6BB0 (count at gp+16964) — a
 * different array with a different purpose, already spelled as
 * Q2_EVENT_OBJECT_MAX in formats/events.h. 64 stays because it is comfortably
 * above the measured 30, not because retail says 64.
 */
#define Q2_EVENT_RT_PENDING_MAX 64

/*
 * Live WAIT counters (opcode 0x09).
 *
 * The disc uses the opcode ZERO times — 0 of 6,646 items across all 164
 * containers — so any capacity is ample. Eight matches the console's eight
 * TIMER slots at 0x800C6F74 for want of a number retail states for this.
 */
#define Q2_EVENT_RT_WAIT_MAX 8

/*
 * The TIMER table, 0x800C6F74: eight 20-byte slots, bounded by `slti v0,v1,8`
 * at 0x8002704C and 0x80027078 in the claim loops and at 0x80027440 in the
 * sweep. Its own constant and not Q2_EVENT_RT_PENDING_MAX, which is the
 * trigger queue's 64 and must not shrink with it.
 */
#define Q2_EVENT_RT_TIMER_MAX 8

typedef struct q2_event_timer {
    u32 item;        /* slot+12: TIMER item offset, zero means free */
    u32 offset;      /* slot+4: containing record */
    u8 timer_index, next_item;
    u16 window, fires;
    s32 period, due;
} q2_event_timer;

typedef struct q2_event_wait {
    bool in_use;
    u32 offset;
    s32 stamp;
    u16 count;
} q2_event_wait;

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

    /*
     * Shadow of each ITEM's mutable op byte, indexed directly by the item's
     * chunk offset.
     *
     * The record shadow above was not enough: 0x80027468 writes the item's own
     * op byte back too, DISABLED := ONESHOT at 0x80027488..0x80027498, and
     * tests it at 0x8002747C before dispatching. 457 items on the disc carry
     * the 0x40 bit and none carries 0x80, so bit 7 there is purely runtime
     * state — which had no home in this port at all.
     *
     * Direct indexing by offset rather than a map: the largest Events chunk on
     * the disc is 2,664 bytes (SECURITY COMMON.DAT), so the whole shadow is
     * under 3 KB.
     *
     * IT OUTLIVES A ZONE CHANGE AND A SAVE, because the console's does. This
     * used to say it was deliberately not saved, and that only two items could
     * tell. Both halves were too small:
     *
     *  - A zone change on the console never reloads COMMON's Events. The zone
     *    loader 0x8007B3F8 re-points gp+376 at the zone's copy (0x8007C234)
     *    and re-runs the constructor pass 0x80026DC0 over gp+372, whose only
     *    flags write is the CAT_B default and whose four opcode constructors
     *    (0x80025D24, 0x80026050, 0x80026484, 0x80026A20) store into operands
     *    and runtime objects, never at item+0. gp+372 itself is stored only
     *    by the LEVEL load (0x8007AD54, in 0x8007A430, called solely from
     *    0x8007956C) and zeroed only at 0x8007C250, for a map whose UserFuncs
     *    pointer (gp+16920) is -1. So every bit 7 an item has latched is still
     *    there in the next zone. The zone-change callback 0x8007901C also
     *    writes these bits out — `EVE_<map>`, 0x80029078, one bit per ONESHOT
     *    item whose op has 0x80 (0x8002923C..0x80029254) — and replays them
     *    after the load (0x8002936C). q2_event_carry below is how the port
     *    keeps both.
     *  - A reload that re-arms item latches replays a one-shot item whose
     *    record an ENABLE re-armed — before the save as much as after the load.
     *    LAB 0x8ac runs ENABLE 0x6d4; saved then, 0x6d4 is 0x49 with all three
     *    of its items one-shot (CREBATCH, FXGROUP, LIFT1), and clear item bits
     *    would let it spawn its monster batch a second time. So save.c now
     *    writes this array too (the EVIT chunk, save version 6).
     */
    u8  *item_flags;
    u32  item_flags_size;
    /* Permanent object changes whose script item has no one-shot latch.
     * Indexed like item_flags; an owner sets a byte when that item must be
     * replayed to reconstruct its object (for example OBJDRAWOFF). */
    u8  *object_flags;

    u32  pending[Q2_EVENT_RT_PENDING_MAX];
    u32  pending_count;

    /*
     * What the ZONEGATE handler needs in order to REFUSE, which is the half
     * this port never had. 0x80027784 hands its 12-byte name to 0x80079178,
     * and that function answers 0 — no gate at all — in exactly two cases:
     * 0x8007917C reads 0x800AEBCC and bails in deathmatch, and 0x800791E0
     * compares the name against the resident zone's at 0x800E465C and bails
     * on a match. Both are owner knowledge, so both are owner-supplied.
     *
     * `current_zone` is the index of the zone the runtime is resident in.
     * -1 means "the owner does not know", which is the standalone default and
     * reproduces this file's previous behaviour exactly: every gate is
     * accepted AND the record runs on past it. The console's abort (the
     * handler's 1 at 0x8002783C) is applied only once this is >= 0, because
     * without the same-zone refusal it would lock every door that waits
     * behind a gate — see the ZONEGATE arm in events_rt.c for the count.
     * q2_event_rt_init resets it, so an owner must set it again after every
     * init, not once.
     */
    s32  current_zone;
    bool multiplayer;

    /*
     * THE ENGINE'S INITIAL-STATE PASS FLAG, gp+16948 (0x800B2834). ZONEGATE
     * tests it at 0x80027794 `lh v0,16948(gp)` / 0x8002779C `bne v0,zero,
     * 0x80027940`, with `addu v0,zero,zero` in the delay slot: while it is set
     * the gate returns 0 — no zone request, and the record runs on.
     *
     * It is NOT a "loader busy" flag, which is what this file used to call it.
     * xrefs over the whole image finds exactly four stores: 1 at 0x8007C2AC
     * and 0 at 0x8007C338, around the STARTLEV named-event run the zone loader
     * makes (0x8007C324 `jal 0x80027CC4` with "STARTLEV"), and 1 at 0x800294B4
     * and 0 at 0x80029754, around the EVE_ replay 0x8002936C. Both passes
     * rebuild state by re-running script, so a replayed [ZONEGATE, MOVER]
     * reopens its door without asking for another load.

     * NINE HANDLERS READ IT (xrefs 0x800B2834: nine loads, the four stores
     * above), and every owner hook the replay reaches must honour it:
     *   0x80027794  ZONEGATE     this runtime, above
     *   0x80026924  FXGROUP      on_explosive: swap the nodes, skip the
     *                            Explosion, the report sound and the debris
     *   0x8002A3C4  GLASS        after its hit burst: skip the shatter and
     *                            the sound (q2_sim_breakable_call)
     *   0x8002B880  STRING       \
     *   0x8002B96C  CREBATCH      |  on_call: return at once (userfuncs.h,
     *   0x8002BAEC  HELPCOMPUTER  |  "Two engine-wide pass flags"); CREBATCH
     *   0x8002D318  SIMPLESOUND   |  after its length check
     *   0x80028EC4  INSECRET     /
     *   0x80042E54  the message printer, reached through STRING's text
     * A replay that reached a hook ignoring it would re-detonate a spent
     * func_explosive or re-spawn a batch on every zone change.
     *
     * q2_event_rt_replay sets it for its own duration, as 0x8002936C does. The
     * STARTLEV pass is the owner's — this runtime has no load — and
     * client_load_zone raises it around its own, as 0x8007C2AC/0x8007C338 do.
     */
    bool initial_pass;

    /* Set when a zone gate fires. The caller is responsible for the load. */
    bool has_zone_change;
    u32  pending_zone;
    /* The 12-byte NAME12 the gate actually carries — "Zone0", "Zone1". The
     * index above is its trailing decimal; the name is kept so an owner can
     * check the zone exists before loading rather than trusting the parse. */
    char pending_zone_name[13];

    /* Counters, so a caller can tell "nothing happened" from "nothing is
     * implemented yet". */
    u32  resumed_count; /* deferred entries that came due and were spent —
                         * including one the prologue then REFUSED because
                         * the record was disabled while it waited */
    u32  ran_count;     /* records that passed the gate and were latched  */
    u32  replayed_count;/* items q2_event_rt_replay re-dispatched         */
    u32  skipped_movers;
    u32  catc_leave_edges;  /* CAT_C leave edges detected — see below */

    /*
     * The record offset the CAT_C LEAVE pass pushes — which is NOT the leaving
     * record's own. See q2_event_rt_contacts_end for the disassembly; the short
     * version is that 0x8002808C reads it from the trigger-array cursor the
     * ENTER loop left one element past the last volume, on the SENTINEL, whose
     * event_offset is 0 on 49 of 49 maps. Left 0 by init, which is already the
     * right answer for every map on the disc; an owner may set it from the
     * sentinel it parsed rather than trusting this default.
     */
    u32  catc_leave_offset;

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
     * ARM A TIMER SLOT — what `TIMER` means.
     *
     * The three operands 0x80026FEC copies into the slot, written by an
     * `on_call` hook for the same reason `abort_record` is: the hook reports a
     * CALL and has nowhere else to put an answer. `defer_timer` stops the
     * record even for a zero-period timer. A positive `defer_ticks` also
     * requests a slot for owners that supply their own fixed period.
     *
     *   defer_ticks   slot+0, the period: `(base + ((range * rand()) >> 15))
     *                 * 30` from item+4 and item+6 (0x800270D0..0x80027104).
     *                 The 30 is not the 300 everything else on this clock
     *                 uses. The installed period callback computes it only
     *                 after acceptance; defer_ticks is the fixed fallback.
     *   defer_fires   slot+8, from item+8 (0x800270A4). How many times the
     *                 window runs before the slot is freed; 0 is FOREVER.
     *   defer_window  slot+6, from item+10 (0x800270B4). How many items one
     *                 deadline runs.
     *
     * The executor clears the request even if the slot is refused.
     */
    bool defer_timer;
    s32 defer_ticks;
    u16 defer_fires;
    u16 defer_window;
    /* Called only after a slot is accepted, and again after each deadline
     * that leaves it armed. Install q2_event_timer_period for retail scripts;
     * NULL keeps the owner's fixed defer_ticks. Rebind after loading a map. */
    s32 (*timer_period)(const q2_event_item *item);

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
     * THE EIGHT TIMER SLOTS, 0x800C6F74, and the clock they count against.
     * `q2_event_rt_advance` moves the clock; `q2_event_rt_update` is the sweep
     * 0x80027340 and runs whatever has come due before it takes anything new
     * off the trigger queue.
     *
     * What 0x80026FEC builds is a repeating N-item WINDOW, not the "delayed
     * continuation of the rest of the record" this port used to model. It
     * claims one of eight 20-byte slots and fills it with the record offset
     * (0x800270AC -> slot+4), a FIRE COUNT from item+8 (0x800270A4 -> slot+8),
     * an ITEM WINDOW from item+10 (0x800270B4 -> slot+6), the TIMER item
     * itself (0x800270C0 -> slot+12) and the resume pointer, the item straight
     * after the TIMER (0x800270BC -> slot+16).
     *
     * THE SLOT IS KEYED ON THE TIMER ITEM, which is what makes the table safe.
     * 0x8002703C..0x80027054 scans all eight for slot+12 == this item and
     * returns having done NOTHING when it finds one (a0 = -1 at 0x8002701C,
     * refused by the `bltz` at 0x80027088) — so a CAT_B record standing under
     * the player's feet re-arms nothing, and 0x8002705C..0x80027084 drops the
     * request outright when no slot is free (a0 = -2). Either way the primitive
     * still returns the next item and still sets gp+16956, so the record stops
     * at the TIMER whether or not a slot was claimed.
     *
     * When the slot comes due the sweep calls 0x8002712C, which runs `slot+6`
     * items and no more (s3 = lh 6(s4) at 0x800271F0, loop 0x800271F4..
     * 0x80027254 — a do-while, so a window of 0 runs one item), stopping early
     * if it comes full circle to the resume pointer (0x8002724C) or if an item
     * answers non-zero (0x80027200). It WRAPS when it runs off the end of the
     * record: 0x8002721C..0x8002723C walks from item 0 up to the TIMER item and
     * 0x80027240 steps past it, so the window carries on at the item AFTER the
     * TIMER and never re-dispatches the TIMER itself. The new resume pointer is
     * stored back at 0x80027270, and the sweep then re-arms: slot+8 is
     * decremented at 0x800273BC..0x800273EC, zero frees the slot (0x800273E0)
     * and a fire count that was already 0 is clamped back to 0 — so 0 means
     * FOREVER — and 0x800273F0..0x8002742C puts a fresh period in slot+0.
     *
     * The disc uses all of it. 18 TIMER CALL items in COMMON, 11 of them
     * "forever": BOSS2 0x184 is TIMER + 8 TIMEDLIGHTs with window 1, so the
     * eight lights cycle one per deadline for the rest of the level, and BOSS1
     * 0x3e4 is fires 11 window 2 in a 13-item record — the arena destruct,
     * two items a second, eleven times. This port ran the whole remainder on
     * the first deadline and never resumed again.
     *
     * The period callback rerolls at each re-arm, including range-zero items:
     * they still advance the shared random stream on the console. Refused
     * requests and the final deadline of a finite timer consume no roll.
     *
     * `item` is the slot's identity and 0 means free, exactly as slot+12 does:
     * no item can live at chunk offset 0, which is the u32 record count.
     */
    q2_event_timer deferred[Q2_EVENT_RT_TIMER_MAX];
    u32  deferred_count;   /* slots in use, for a caller's "did anything
                            * happen"; the array is indexed, not packed */
    s32  clock;

    /*
     * Live WAIT counters (opcode 0x09), the item state 0x800276C4 mutates in
     * place: the stamp at item+4 and the countdown at item+8. Keyed by the
     * item's chunk offset, for the same reason `item_flags` is — the chunk is
     * borrowed const.
     *
     * `tick` is this runtime's equivalent of the console's frame counter at
     * 0x800B2DE4 (read by the handler at 0x800276D8; incremented once per frame
     * at 0x800705C4 inside 0x80070490 and stored back at 0x80070610). INFERRED
     * mapping: the client calls `q2_event_rt_advance` exactly once per ticked
     * frame (its only call site, in src/client/main.c's `if (ticked)` block),
     * so it steps `tick` by one whatever `ticks` is, while `clock` keeps
     * taking the real delta. Nothing on the disc exercises this, so nothing
     * on the disc can confirm the mapping.
     */
    q2_event_wait wait[Q2_EVENT_RT_WAIT_MAX];
    s32  tick;

    u32  mover_count;   /* MOVER items reported                          */
    u32  call_count;    /* CALL items reported, for the same "did anything
                         * happen" reason as the counters above */
} q2_event_rt;

q2_result q2_event_rt_init(q2_event_rt *rt, const q2_events *events);

/* Move the runtime's own clock, in the simulation's tick units. Deferred
 * records come due against it. Also steps `tick` by one — one call is one
 * frame, which is what a WAIT counts. */
void q2_event_rt_advance(q2_event_rt *rt, s32 ticks);

/*
 * RESUME A RECORD AFTER THE ITEM THAT JUST FIRED — 0x8002EF1C's second half.
 *
 * A destroyable brush group does not only break: when the hit that kills it
 * came from a WEAPON, the rest of its Events record runs. 0x800267C4 returns 0
 * while the group stands (0x80026834) and `item + 28` once the destruction
 * loop has run (0x800269FC); the weapon-impact router tests that at
 * 0x8002EFA8 and, on a non-zero answer, reads the record offset out of
 * obj+0x40 (0x8002EFB4) and calls the record executor 0x80027950 with
 * a2 = item + len — the NEXT item (0x8002EFCC).
 *
 * Shooting a crate is therefore a script event. On BASE0 that is what spawns
 * the jacket armour: COMMON.DAT record +508 is
 * [FXGROUP item+512 hp 10, CALL CREBATCH "Amour", CALL INSECRET], the batch
 * 'Amour' holds exactly one place record — id 4 Jacket P at
 * (-15402,-52,-4731), inside that group's own box — and no trigger volume,
 * directory entry or list opcode on the map names record +508. The shot is
 * the only route to it. Ten records across the disc have a shootable 0x08
 * item with a tail; six of them, BASE0's included, are reachable no other way.
 *
 * This is the SHOT route only. The script route (0x800276B0) returns 0 and
 * the record executor the item is already inside carries on by itself, so a
 * resume there would run the tail twice.
 *
 * Returns false when the record is disabled, the offsets do not resolve, or
 * the item is not in the record. `out` takes the outcome so a ZONEGATE in the
 * tail still reaches the caller.
 */
bool q2_event_rt_resume_after_item(q2_event_rt *rt, u32 record_offset,
                                   u32 item_offset, q2_event_outcome *out);

void      q2_event_rt_free(q2_event_rt *rt);

/* Queue the record at `offset` to run on the next update. */
bool q2_event_rt_trigger(q2_event_rt *rt, u32 offset);

/* Queue a record by its name in the Events directory. */
bool q2_event_rt_trigger_named(q2_event_rt *rt, const char *name);

/* 0x80027B04: change a named record's DISABLED bit without triggering it. */
bool q2_event_rt_disable_named(q2_event_rt *rt, const char *name, bool disabled);

/*
 * Feed one frame of trigger-volume contact into the record flags.
 *
 * The three authored category bits are now decoded from 0x80027E64:
 *
 *   CAT_A (0x08)  run on the first frame inside
 *   CAT_B (0x10)  run on every frame inside
 *   CAT_C (0x20)  the LEAVE edge — detected, and runs nothing on this disc
 *
 * RT1/RT2 are the current/previous-contact bits the retail dispatcher uses to
 * make those edges. Begin clears the current bit, contact ORs it into the
 * named record and queues any enter/stay run, and end detects each CAT_C leave
 * edge before shifting current into previous. Several volumes may name one
 * record; doing this on the record rather than on a volume bitmap preserves
 * retail's "inside any of them" behaviour.
 *
 * A CAT_C leave edge does NOT queue the record that was left. It is counted
 * in `catc_leave_edges` and queues `catc_leave_offset` — the TrigBounds
 * sentinel's event_offset, which 0x8002808C `lh v1,26(s6)` reads with s6 left
 * one element past the last volume — and that is 0 on every map on this disc,
 * which starts no record. So the leave record does not run, as on the console.
 */
void q2_event_rt_contacts_begin(q2_event_rt *rt);
bool q2_event_rt_contact(q2_event_rt *rt, u32 offset);
void q2_event_rt_contacts_end(q2_event_rt *rt);

/*
 * Run everything queued, which may queue more. Returns what happened.
 *
 * Bounded rather than run-to-empty: a script that triggers itself would
 * otherwise spin forever, and the original ran its queue once per tick.
 */
q2_event_outcome q2_event_rt_update(q2_event_rt *rt);

/* Current flags of the record at `offset`, or 0 if unknown. */
u8 q2_event_rt_flags(const q2_event_rt *rt, u32 offset);

/*
 * The RUNTIME bits an item has acquired — in practice Q2_EVOP_DISABLED, set by
 * the one-shot latch at 0x80027498. Zero for an offset outside the shadow. The
 * authored bits are still in the chunk; this is only the half that is written
 * back at run time, which is why it is separate from `item->op`.
 */
u8 q2_event_rt_item_flags(const q2_event_rt *rt, u32 offset);

/* ------------------------------------------------------------------------- */
/* Across a zone change                                                       */
/* ------------------------------------------------------------------------- */
/*
 * WHAT A ZONE CHANGE KEEPS, derived from the zone-change callback 0x8007901C
 * (installed by 0x80079178 at 0x80079360) and what it calls:
 *
 *   0x80079080  jal 0x80078D1C   — on a real level (+0x20 of the level record
 *               at gp+18832 non-zero, 0x80078D44; the leveltable.h census has
 *               it 1 on every real level) and outside game state 3
 *               (0x80078D58), calls 0x80029078 at 0x80078E70, which writes the
 *               `EVE_<map>` block: one bit per record with (flags & 0x81) ==
 *               0x81 (0x800291B8..0x800291D8), then one per ONESHOT item whose
 *               op has 0x80 (0x80029214..0x80029254), in chunk order, capped at
 *               128 bit positions (s2 = 128 at 0x80029094).
 *   0x80079114  jal 0x800793B8   — the zone load. It does NOT reload COMMON:
 *               gp+372, COMMON's Events, is stored only at 0x8007AD54 by the
 *               level load (xrefs, whole image; the one other store is the
 *               zeroing at 0x8007C250). It clears the eight TIMER slots
 *               (0x8002EE10, at 0x800793EC), and its zone loader 0x8007B3F8
 *               re-runs the constructor pass 0x80026DC0 over gp+372
 *               (0x8007C278), which clears them again (0x80026DF0), re-seeds
 *               every WAIT (0x80026F08) and re-applies the CAT_B default —
 *               idempotent, since nothing at run time clears bits 3-5.
 *   0x80079130  jal 0x8002F004   — outside deathmatch (0x80079128), and it
 *               calls 0x8002936C: the EVE_ replay below.
 *
 * So what survives is the chunk's own written-back bytes — every record's
 * flags byte, contact bits included, and every item's latched bit 7 — while
 * TIMER continuations and WAIT counters start over. q2_event_carry is exactly
 * that pair of arrays. The port re-initialises the runtime on every zone load
 * (client_load_zone runs q2_sim_free / q2_sim_init / q2_sim_attach_gameplay),
 * which put every spent record and item back on the shelf at every boundary:
 * walk into a one-shot volume again in the next zone and its CREBATCH or
 * STRING fired a second time.
 *
 * On this disc the EVE_ block never truncates: the most bit positions any
 * COMMON script needs is 89 (LAB: 70 records, 19 one-shot items), so the 128
 * cap never binds. The carry keeps the whole bytes anyway, because the whole
 * bytes are what the console's memory keeps.
 */

/*
 * The largest Events chunk this carries. The disc's largest is 2,664 bytes
 * (SECURITY COMMON.DAT); a chunk above this is not carried (`size` stays 0) and
 * the next zone starts fresh, which is the port's behaviour before the carry.
 */
#define Q2_EVENT_CARRY_MAX 4096

typedef struct q2_event_carry {
    u32 size;           /* the chunk it came from; 0 means nothing carried   */
    u32 record_count;
    u32 identity;       /* FNV-1a over the chunk's bytes: the same script    */

    /* Parallel to q2_event_rt.flags. A record is at least its 4-byte header
     * and they start after the u32 count and the directory terminator, so a
     * chunk of `size` bytes holds fewer than size / 4 of them. */
    u8  record_flags[Q2_EVENT_CARRY_MAX / 4];

    /* Parallel to q2_event_rt.item_flags: the runtime bits, by item offset. */
    u8  item_flags[Q2_EVENT_CARRY_MAX];

    /* Native world-state extension: FXGROUP destruction and OBJDRAWOFF need
     * not have one-shot script latches. Replay only the changed item, without
     * re-running its pickup/secret tail. Includes rt.object_flags and the
     * owner's destroyed explosive groups, indexed by item offset. */
    u8  destroyed_items[Q2_EVENT_CARRY_MAX];
} q2_event_carry;

/* Take the latches off a runtime about to be freed. Leaves `size` 0 — nothing
 * to carry — for a runtime with no records or a chunk too big to hold. */
void q2_event_rt_carry_out(const q2_event_rt *rt, q2_event_carry *out);

/*
 * Put them back on the runtime that replaced it. Applies only when the carry
 * came from the same chunk — size, record count and identity all match — and
 * otherwise leaves the fresh runtime alone, because latches from another
 * script would disable records at random. It restores state and runs nothing;
 * q2_event_rt_replay is the half that runs script.
 */
void q2_event_rt_carry_in(q2_event_rt *rt, const q2_event_carry *in);

/*
 * THE EVE_ REPLAY, 0x8002936C from 0x800294A0 on, reading its bits from the
 * carry the way 0x8002936C reads them from the block 0x80029078 wrote.
 *
 * With `initial_pass` set (0x800294B4), for each record in chunk order:
 *
 *   - a record whose bit is set gets 0x81 ORed back in (0x800294FC..
 *     0x80029508) and EVERY item of it is dispatched again — including any
 *     the original run never reached, which is how a spent [ZONEGATE, MOVER]
 *     opens its door in the zone it asked for;
 *   - otherwise only its ONESHOT items whose bit is set are dispatched;
 *   - each dispatch clears the item's bit 7 first (0x800295C8 `andi 0x7F`,
 *     stored in the delay slot at 0x800295D0), so the dispatcher's latch at
 *     0x80027498 retires it again; the answer is discarded (0x800295D4), so
 *     nothing a handler returns stops the replay of the record;
 *   - the queue is reset before the record (0x80029538/0x8002953C) and drained
 *     after it (0x80029604..0x80029698), every drained record through the
 *     usual gate and latch (0x80029620..0x80029648).
 *
 * Bit positions are counted as the writer counts them — one per record, one
 * per ONESHOT item — and a position at or past 128 has no bit (0x800294DC,
 * 0x80029570).
 *
 * Call it after the owner's hooks are installed and its movers built: that is
 * what the replay is FOR — the new zone's doors, lifts and explosives are
 * rebuilt at rest, and replaying the spent records puts them back the way the
 * script left them. What follows it on the console is the owner's: seven
 * passes (0x800296D4..0x8002974C) that each store 30000 at 0x800B2DB4, add
 * 30000 to 0x800AEBAC, and call the think at +44 of every one of the 48
 * runtime objects at 0x800D6BB0 — so those movers arrive rather than start
 * out. Retail runs this outside deathmatch only (0x80079120); that test is the
 * owner's too.
 */
void q2_event_rt_replay(q2_event_rt *rt, const q2_event_carry *carry);

/* ------------------------------------------------------------------------- */
/* The blocks themselves — one carry per map                                  */
/* ------------------------------------------------------------------------- */
/*
 * THE CARRY ABOVE IS NOT A ZONE-CHANGE THING. It is filed under a MAP.
 *
 * StoreEventStates (0x80029078) builds its save-memory block name by copying
 * the five bytes "EVE_" from 0x800AE7A0 and then eight bytes of the current
 * level name from 0x800E46B4 (0x800290A4..0x800290E8), and OpenSaveMemory
 * (0x8007FBEC at 0x80029490) makes the block if it is not already there. The
 * writer is reached from OnLeavingZone, whose two callers are the zone-change
 * callback at 0x80079080 AND OnLeavingLevel at 0x8007CB94 — a function whose
 * entire body is that one `jal`. So a LOADMAP writes the block exactly as a
 * gate does, under the map being left.
 *
 * Coming in, RestoreSavedData (0x8007CBAC), which InitLevel calls at
 * 0x80070F50, reads the block named after the level being ENTERED; 0x80029498
 * falls straight out when there is none, so a first visit replays nothing.
 *
 * And nothing empties save memory at a level or a unit boundary: the only
 * clear in the image is the new-game memset (mission.h, the sibling
 * "PlayerSave" block). That is what makes the Waste unit work — its maps
 * interconnect both ways, so WASTE2 and WASTE3 are each entered two or more
 * times in one ordinary playthrough, and on the second visit the console
 * re-arms none of the one-shots the player already spent.
 *
 * Heap, because one block is a little over five kilobytes and an owner may
 * hold twenty-two of them.
 */
#define Q2_EVENT_BLOCK_NAME_MAX 64

typedef struct q2_event_block {
    char           map[Q2_EVENT_BLOCK_NAME_MAX];
    q2_event_carry carry;
} q2_event_block;

typedef struct q2_event_store {
    q2_event_block *block;
    u32             count;
    u32             cap;
} q2_event_store;

/* The block a map left behind, or NULL. Names are matched case-blind: the
 * level table and the ISO directory disagree on case, and the console's own
 * key is whatever 0x800E46B4 happens to hold. */
q2_event_carry *q2_event_store_find(q2_event_store *s, const char *map);

/* The same, making the block on a first visit — OpenSaveMemory's own
 * behaviour. NULL only when there is no room for it. */
q2_event_carry *q2_event_store_open(q2_event_store *s, const char *map);

/* A NEW GAME, and nothing else: the memset at 0x8003DDB8 that 0x8003D62C
 * takes when no block is found. The allocation is kept for reuse. */
void q2_event_store_clear(q2_event_store *s);

void q2_event_store_free(q2_event_store *s);

/* Retail TIMER item+4/+6, in 1/300 s ticks. The caller has already identified
 * this CALL as TIMER. A valid item always consumes one gameplay random draw. */
s32 q2_event_timer_period(const q2_event_item *item);

#endif /* Q2PSX_EVENTS_RT_H */
