/*
 * mover.h — doors, lifts, double doors and the train.
 *
 * MOVER_A, MOVER_B and MOVER_C are not three motions. They are ONE motion —
 * an axis-aligned translation of a group of Scene nodes — with three
 * parameterisations. This integrator contains no rotation: no matrix, no angle
 * field, and no GTE call.
 *
 *     MOVER_A  1,006 uses. Vertical, axis hard-wired to Y. Lifts and most doors.
 *     MOVER_B     20 uses. Horizontal, axis taken from the payload.
 *     MOVER_C    292 uses. Double door: two leaves moving opposite ways.
 *
 * ---------------------------------------------------------------------------
 * PLATFORM is the exception, and PLATFORM is `func_train`
 * ---------------------------------------------------------------------------
 * Every other mover in this engine slides along ONE axis by a signed number of
 * units. `PLATFORM` does not. Its payload names an absolute world POINT — a
 * `path_corner`, in retail Quake II's vocabulary — and the brush group
 * travels along the straight line from where it stands to that point.
 *
 * The constructor at `0x8002CBB0` is unambiguous about it. It takes the VEC3 at
 * item `+4`, subtracts the bounding-box centre of the group's FIRST Scene node,
 * and stores the three signed differences as full 32-bit words:
 *
 *     8002CC6C  lw   a0, 4(a1)     ; origin.x        (and 8(a1), 12(a1))
 *     8002CC74  subu a0, a0, v0    ; minus the node's box centre
 *     8002CDD0  sw   t1, 0(s3)     ; -> obj+0x00  |
 *     8002CDDC  sw   t2, 4(s0)     ; -> obj+0x04  +- the DIRECTION VECTOR
 *     8002CDEC  sw   t1, 8(s0)     ; -> obj+0x08  |
 *     8002CDB4  sh   s4, 68(s0)    ; -> obj+0x44, its LENGTH via isqrt
 *
 * and the per-frame handler at `0x8002C2D4` — which is PLATFORM's own, not the
 * `0x80025658` everything else installs — divides that vector by the length and
 * scales it by how far along the travel the platform is:
 *
 *     8002C914  lw   v0, 0(s0)     ; dir[k]
 *     8002C920  mult v0, v1        ; * progress          (obj+0x20)
 *     8002C930  div  t4, v0        ; / length            (obj+0x44)
 *     8002C960  sh   a1, 40(sp)    ; -> a s16 per axis, all THREE of them
 *
 * So the displacement is `dir * progress / length` on x, y AND z, written down
 * the `+0x30` chain to every part's `+0x12` triple. `obj+0x44` is `abs()`-ed
 * every frame (`0x8002C794`) because it is a distance and never a direction:
 * unlike a door's `target`, its sign carries no meaning.
 *
 * That is the whole difference between a lift and a train, and this port used
 * to model PLATFORM as a lift — one node, one axis, moving |dir| units along Y.
 * On BIGGUN, the only map that ships one, that sent a three-part platform
 * straight down through the floor by the length of a journey it should have
 * made mostly sideways.
 *
 * Four more things the lift model got wrong about it:
 *
 *   - PLATFORM has FOUR object slots at +20/+22/+24/+26, not one. BIGGUN's
 *     names three (Scene nodes 31, 32 and 30) and the port read only the first.
 *     The direction and the length come from slot 0 alone; the rest just ride.
 *   - `obj+0x44` is stored with `sh` and read with `lh`, so a path longer than
 *     32,767 units arrives NEGATIVE and the per-frame `abs()` is what makes it
 *     usable again. BIGGUN's 33,020 becomes 32,516. The endpoint survives — the
 *     scale is exactly one at `progress == target` whatever the target is — so
 *     this is invisible except as duration, and a clamp to 32,767 gets that
 *     wrong in the opposite direction.
 *   - Blocked, it does not wait sixteen ticks and try again. It BACKS OFF —
 *     `0x8002CAE0` loads `obj+0x4E` with `progress -/+ speed * 150` and state 4
 *     runs `0x8006FEB8`, a clamped move-toward, until it gets there. Only then
 *     does it resume the state it was interrupted in (`obj+0x56`).
 *   - It does not REVERSE when re-triggered on the way back. MOVER_A's exec
 *     tests the state for 3 and branches to a reversal (`0x8002757C` ->
 *     `0x80027630`); PLATFORM's exec at `0x8002E8C4` has no such test.
 *
 * BIGGUN calls its own script entry point `PLATFORM` and carries a LevelBin
 * mission event called `STOPPLATFORM`, so the level author's name for the thing
 * agrees with the primitive's.
 *
 * ---------------------------------------------------------------------------
 * CORRECTION — "no rotation" is true of the three above and NOT of the engine
 * ---------------------------------------------------------------------------
 *
 * This file used to say "there is no rotation anywhere in the engine". The zone
 * draw refutes it: at 0x800678B4 it calls `RotMatrix` on three s16 Euler angles
 * at the node's runtime object +0x0C. The +0x12 triple is linear displacement;
 * +0x18 is a pivot used as `-R.p+p`, not another independent translation.
 * `ROTHATCH`, `SIMROT`, `SIMROT2` and `ROTBUTTON` drive those rotation slots.
 *
 * What is implemented here is the linear family and its +0x12 triple. The
 * rotating family, its pivot constructors and its integrators live in
 * rotator.[ch]; surface.h maps the shared object binding out of Scene.flags08.
 *
 * ---------------------------------------------------------------------------
 * Why this port does not reproduce the load-time pre-pass
 * ---------------------------------------------------------------------------
 * The original rewrites the script's s16 slots in place at load, turning Scene
 * node indices into indices into a fixed 48-entry runtime array. That array
 * exists because the console had 2 MB; it is an allocation strategy, not
 * meaning.
 *
 * A native port wants the opposite: read the DISC values, which are Scene node
 * indices, and build objects directly. So this module never performs the
 * rewrite, and the disc payload is the only input. That also sidesteps the
 * subtlety that made movers unimplementable for so long — the same bytes mean
 * different things before and after the pre-pass.
 *
 * ---------------------------------------------------------------------------
 * On-disc payloads
 * ---------------------------------------------------------------------------
 * Offsets are from the start of the event item (op at +0, len at +1).
 *
 *   MOVER_A, len 24:
 *     +2  s16  travel        target displacement is -travel
 *     +4  s16  speed         absolute value taken
 *     +6  s16  portal_node   Scene node carrying the visibility bit; -1 none
 *     +8  s16  node[4]       Scene nodes translated; -1 unused
 *     +16 u16  key_mask      required key bits; 0 means unlocked
 *     +18 u8   delay         seconds before opening
 *     +19 u8   wait          seconds held open; 0xFF never auto-closes
 *     +20 s16  touch         non-zero also opens on touch (65 of 1,006)
 *
 *   MOVER_B, len 24: as A but +8 is the axis (& 3; only 0 and 2 occur), the
 *     nodes move to +10, key_mask to +18, delay/wait to +20/+21, and the
 *     target is +travel when the axis field is 0, -travel otherwise.
 *
 *   MOVER_C, len 32: two leaves at +10 and +18, key_mask +26, delay/wait
 *     +28/+29. Leaf 1's target is the negation of leaf 0's, and leaf 1 owns the
 *     portal node and points at leaf 0 as its partner.
 *
 *   PLATFORM is a CALL and not an opcode, so its payload lives with the other
 *   primitives in userfuncs.c; repeated here because it is the odd one out:
 *
 *     +4  s32[3] origin        the destination, ABSOLUTE world coordinates
 *     +18 s16    speed         abs() taken
 *     +20 s16    node[4]       Scene nodes carried; -1 unused
 *     +28 u8     delay         UNSCALED, unlike everything else here
 *     +29 u8     wait          x300; 0xFF never returns
 *
 *   and the one item on the disc (BIGGUN, Events+476) reads
 *   `origin (-55731, 11143, 289)  speed -4  nodes 31, 32, 30, -1  delay 0
 *   wait 2`.
 *
 * ---------------------------------------------------------------------------
 * Timing
 * ---------------------------------------------------------------------------
 * delay and wait are bytes multiplied by 300 and counted down by the same dt
 * the simulation uses, so one byte is one second at the 25 Hz tick.
 *
 * Note this 300 is genuine, unlike TIMER's, which is x30 — see userfuncs.h.
 * The two use different instruction idioms and conflating them makes every
 * door ten times too slow.
 */
#ifndef Q2PSX_MOVER_H
#define Q2PSX_MOVER_H

#include "events.h"
#include "userfuncs.h"
#include "q2psx.h"
#include "scene.h"

#define Q2_MOVER_MAX_PARTS  4
#define Q2_MOVER_TIMEBASE 300
#define Q2_MOVER_WAIT_NEVER 0xFFFFu

/* ------------------------------------------------------------------------- */
/* The sounds — 0x80025658's three, and they are per OPCODE FAMILY            */
/* ------------------------------------------------------------------------- */
/*
 * A mover made no sound at all, and the reason is that this module never had a
 * sound path: `q2_movers_tick` reaches all three of the transitions the
 * original plays on and plays nothing at any of them.
 *
 * The linear handler at 0x80025658 makes exactly three `jal 0x80073704` calls:
 *
 *   0x800257A8  msc_keyuse   a keyed door accepted the player's key
 *   0x8002585C  msc_keytry   it refused, and only the first time — the same
 *                            once-only latch `announced` already models
 *   0x80025A5C  pt1__strt    the motion starts, reached from BOTH the
 *                            DELAY->OPENING arm (0x800258F0) and the
 *                            OPEN->CLOSING arm (0x800259BC)
 *
 * There is NO arrival sound and NO looping move sound on a linear door. Do not
 * assume the start/loop/stop shape: only ROTHATCH has it (0x8002B250 plays
 * pt1__strt, then pt1__mid through the LOOPING entry point 0x80073734, and
 * pt1__end on arrival, stopping the loop with 0x8007398C).
 *
 * The six handles are all `lw a0, N(gp)` GLOBALS registered by name at
 * 0x8002D4E0..0x8002D800 — not one is loaded from the runtime object, and no
 * payload carries a sound operand. So the sound is a property of the opcode
 * family, and this port folds several families into one set:
 *
 *   MOVER_A/B/C, LIFT1, CAGELIFT1           -> the three above
 *   PLATFORM                                -> those three AND two more
 *   BUTTON                                  -> amb_butn2 and nothing else
 *   PISTON, DISH                            -> silent, deliberately
 *
 * PLATFORM REACHES THE SAME THREE BY A DIFFERENT ROUTE, and the folding above
 * is a convenience rather than a reading of the engine. 0x80025658 is installed
 * by four constructors — 0x80025E84 (MOVER_A), 0x800265F0 (MOVER_B/C) and the
 * two 20-byte lift constructors 0x800282A4 and 0x80029898. PLATFORM's own
 * constructor 0x8002CBB0 installs a different handler, 0x8002C2D4, at
 * 0x8002CD80; that handler plays the same three — msc_keyuse at 0x8002C40C,
 * msc_keytry at 0x8002C4C0, pt1__strt at 0x8002C5C0 and 0x8002C6A0 — so the
 * bank-key set agrees even though the code path does not.
 *
 * IT PLAYS TWO MORE, and this file used to end the paragraph at "the outcome
 * agrees". It does not agree: a train is the one mover with a running sound and
 * an arrival sound, and it reaches them through a different call entirely.
 *
 *   0x8002C778  id 13, volume 1024, EVERY TICK the platform is moving
 *   0x8002C8D8  id 14, volume 3072, on the tick it arrives
 *
 * Both go to `0x80040800(4, id, volume, &pos, 2048, 4096)`, which is not
 * 0x80073704's by-handle path at all. It walks the entity array at 0x800D5C30,
 * rejects anything further than the outer radius, attenuates linearly between
 * the two (`vol * (far - d) / (far - near)`, dropped under 513), and resolves
 * `id` against a 16-record table of SPU parameter pairs at 0x8009D420 — ids 1
 * through 0x10, each `{ const void *a; const void *b; u16 flags; u16 id; }`.
 *
 * There is no mixer in this port yet, so `travel_sound` records WHICH of the
 * two a tick asked for and nothing consumes it. Recorded rather than dropped,
 * because a silent train is a thing somebody will eventually go looking for.
 *
 * WHERE it comes from: every call passes the CENTRE of the mover's collision
 * box, and the CLOSING one adds the live displacement (0x80025A00 onward), so
 * the sound follows the door. A mover with no box in this zone is not in this
 * zone, and is silent rather than listener-local.
 */
typedef enum q2_mover_sound {
    Q2_MVSND_NONE = -1,
    Q2_MVSND_KEY_USE = 0,   /* msc_keyuse, 0x800B27CC */
    Q2_MVSND_KEY_TRY,       /* msc_keytry, 0x800B27D0 */
    Q2_MVSND_START,         /* pt1__strt,  0x800B27D8 */
    Q2_MVSND_BUTTON,        /* amb_butn2,  0x800B27E4 */
    Q2_MVSND_COUNT
} q2_mover_sound;

/* The bank keys, in that order. */
extern const char *const q2_mover_sound_name[Q2_MVSND_COUNT];

/*
 * PLATFORM's two, which are NOT bank keys — they are indices into the SPU
 * parameter table at 0x8009D420 and go through 0x80040800. Kept as the raw
 * numbers the executable passes so that a future mixer has the same operand the
 * console had, rather than a name this port invented for it.
 */
#define Q2_MOVER_TRAVEL_MOVE_ID   13u
#define Q2_MOVER_TRAVEL_MOVE_VOL  1024u
#define Q2_MOVER_TRAVEL_STOP_ID   14u
#define Q2_MOVER_TRAVEL_STOP_VOL  3072u
#define Q2_MOVER_TRAVEL_NEAR      2048   /* full volume within this radius */
#define Q2_MOVER_TRAVEL_FAR       4096   /* silent beyond it               */

/* obj+0x52 in the original. */
typedef enum q2_mover_state {
    Q2_MV_IDLE = 0,
    Q2_MV_OPENING,
    Q2_MV_ARRIVED,
    Q2_MV_CLOSING,
    Q2_MV_BLOCKED,
    Q2_MV_DELAY,
    Q2_MV_OPEN
} q2_mover_state;

enum {
    Q2_MV_BLK_IGNORE_OPENING = 1,
    Q2_MV_BLK_IGNORE_CLOSING = 2
};

/* `prim` for a mover built from a MOVER_A/B/C opcode rather than a CALL. */
#define Q2_MOVER_PRIM_OPCODE 0xFFu

typedef struct q2_mover {
    s16 node[Q2_MOVER_MAX_PARTS];   /* Scene nodes this group translates */
    u32 part_count;

    s16 portal_node;    /* -1 when the group has none */
    s16 target;         /* signed displacement along `axis` */
    s16 speed;
    u16 key_mask;

    u16 delay_timer;
    u16 wait_timer;     /* Q2_MOVER_WAIT_NEVER means it never auto-closes */

    u8  axis;           /* 0 = X, 1 = Y, 2 = Z */

    /*
     * Whether this mover owns the retail +0x28 pusher object and therefore
     * puts its parts in the collision world.  The ordinary mover constructors
     * all do; PISTON takes this from its signed item[+18] gate.  The PAL disc's
     * thirteen PISTON calls all leave that word zero, so they animate their
     * Scene nodes without becoming solid or entering 0x80051EC0.
     */
    u8  blocks_player;

    /*
     * THE TRAIN, and the only mover in this engine that is not axis-aligned.
     *
     * `is_path` selects the displacement rule. Clear, and the mover slides
     * `offset` units along `axis`, which is every door, lift, button, piston
     * and dish. Set, and the displacement is `dir * offset / target` on all
     * three axes — `dir` being the signed delta from the group's first node to
     * the destination the script authored, and `target` its length, exactly as
     * obj+0x00..0x08 and obj+0x44 hold them (0x8002CDD0, 0x8002CDB4).
     *
     * `axis` is still Y on a train, because the constructor writes 1 into the
     * two-bit axis field at obj+0x50 bits 14-15 (`ori 0x4000`, 0x8002CDA0) and
     * because the rider test wants a vertical component to work with. Nothing
     * in the motion reads it.
     */
    u8  is_path;
    s32 dir[3];

    /*
     * A zero-speed LIFT1 can be an object BINDING rather than a broken lift.
     * BASE0's named CRATES record is the concrete case: its four slots allocate
     * four ordinary 92-byte objects, then the map's DOCRATES handler clears
     * each obj+0x2C tick callback and writes obj+0x14 itself. `external` keeps
     * the generic seven-state mover from consuming that object; `external_part`
     * is the original slot index, because the handler gives slots 0/1 and 2/3
     * different speeds.
     */
    u8  external;
    u8  external_part;

    /*
     * The sound this tick asked for from 0x80040800's numeric table, or 0.
     *
     * Q2_MOVER_TRAVEL_MOVE_ID while it is moving, Q2_MOVER_TRAVEL_STOP_ID on
     * the tick it arrives. Trains only — no other family reaches that call.
     */
    u8  travel_sound;

    u8  state;
    u8  saved_state;
    u8  block_timer;
    u8  block_flags;
    u8  triggered;      /* latch, cleared each tick */
    u8  announced;      /* so a locked door only complains once */
    u8  touch_opens;

    /*
     * A CAGE LIFT IS TWO SLABS, and this is what makes it one.
     *
     * CAGELIFT1's constructor at 0x80029794 calls the slot allocator TWICE
     * (0x80029A78, 0x80029B1C) and chains the pair through +0x3C: a top slab
     * whose max[1] is min[1] + item[+17], and a bottom slab whose min[1] is
     * max[1] - item[+16]. It is a cage with a ceiling and a floor and nothing
     * in between — you ride inside it.
     *
     * The port registered one box per Scene node instead, so the cage was
     * solid through its middle and could not be entered. On BASE1 that is the
     * lift out of zone 1: node 215's box is 1,239 units of air the static hull
     * lets a player fall straight through, and the mover made it a wall.
     *
     * `cage_top` is item[+17] and `cage_bottom` item[+16]; both zero on
     * anything that is not a cage.
     */
    u8  cage_top;
    u8  cage_bottom;

    /*
     * The portal node's visibility bit — `flags08` bit 15, written at
     * 0x80025C5C when the leaf settles fully closed and cleared otherwise,
     * with the write skipped while the partner leaf is still moving.
     *
     * Kept here rather than pushed into the caller's hide array because that
     * array has a second writer (the script's OBJDRAWOFF) and a per-tick
     * unconditional write from here would undo it.
     *
     * NO CONSUMER YET, and said so rather than left looking wired: the port's
     * zone draw has no portal-visibility test for it to feed. What is fixed is
     * that the value is now COMPUTED — `partner_busy` was calculated and then
     * `(void)`-cast away under a comment claiming the renderer consumed it,
     * and no renderer did.
     */
    u8  sealed;

    /*
     * Which family's sounds this mover uses. BUTTON has its own single sound;
     * PISTON and DISH have none. Set at build from the primitive, because the
     * handler a constructor installs is what decides it.
     */
    u8  silent;        /* PISTON, DISH: no sound at all      */
    u8  is_button;     /* amb_butn2 instead of the three     */

    /*
     * The sound this tick asked for, or Q2_MVSND_NONE. Drained by the owner,
     * which is also the only thing that knows where the mover's box is and can
     * therefore position the voice.
     */
    s8  sound_pending;

    /*
     * The AUTHORED values of the two timers, snapshotted once the build has
     * decoded them.
     *
     * `delay_timer` and `wait_timer` are counted down destructively, so a
     * mover that has opened once has spent both. The console reloads them from
     * the item's own bytes whenever an IDLE mover is triggered (0x800275CC),
     * which needs somewhere to reload them FROM — this is it.
     */
    u16 delay_reset;
    u16 wait_reset;

    s32 offset;         /* current displacement along the axis */

    /* Which primitive built it, or Q2_MOVER_PRIM_OPCODE for the MOVER_A/B/C
     * opcodes, which are not UserFuncs calls at all. Only for reporting: a
     * build that drops a mover and one that never saw it look identical in a
     * count. */
    u8  prim;

    s32 partner;        /* index of the other leaf, -1 when single */

    /*
     * The byte offset of the event item this mover was built from — its
     * IDENTITY.
     *
     * The runtime reports a MOVER item when a script reaches one, and the
     * owner has to say which of the built movers that is. An ordinal would
     * work only while build order and execution order agree and nothing
     * guarantees they do; the item's own offset (`q2_event_item.offset`)
     * cannot drift.
     */
    u32 item_offset;
} q2_mover;

typedef struct q2_mover_set {
    q2_mover *movers;
    u32       count;
    u32       capacity;
} q2_mover_set;

/*
 * Build every mover the MOVER_A/B/C opcodes declare.
 *
 * `ops` carries the two-buffer rebase (#56, userfuncs.h): the object SLOTS are
 * read out of the pristine copy of the Events chunk, not the one being walked.
 * Pass NULL to read in place, which is right only for zone 0 — COMMON.DAT's
 * Events snapshot agrees with ZONE0's and with no other, so 239 mover items
 * disc-wide otherwise build with the wrong nodes or with none at all.
 */
q2_result q2_movers_build(q2_mover_set *out, const q2_events *events,
                          const q2_uf_operands *ops);
void      q2_movers_free(q2_mover_set *set);

/* Latch a mover so it acts on the next tick. Reversing a closing door is
 * immediate, which is why this is not just a flag set. */
void q2_mover_trigger(q2_mover_set *set, u32 index);

/*
 * Trigger every mover built from the item at `item_offset`.
 *
 * "Every" because MOVER_C builds TWO movers from one item — the two leaves of
 * a double door — and a script that opens the door means both.
 *
 * This is the piece that was missing. `q2_movers_build` had no caller anywhere
 * in the port and `q2_mover_trigger` had none either, so every door and lift
 * on the disc stood still: 1,006 MOVER_A items, 20 MOVER_B and 292 MOVER_C.
 * Returns how many were triggered.
 */
u32 q2_movers_trigger_item(q2_mover_set *set, u32 item_offset);

/*
 * THE OTHER HALF OF THE LIFTS: the ones a CALL builds.
 *
 * `MOVER_A/B/C` are opcodes in the record stream and `q2_movers_build` reads
 * them. `LIFT1` is a CALL PRIMITIVE that builds the same kind of runtime
 * object, and userfuncs.c's operand table maps one onto the other exactly:
 *
 *     LIFT1, len 20        this module's field
 *     +4  u16 param_a      target, NEGATED (the ctor writes -value to obj+0x44)
 *     +6  s16 param_b      speed, abs() (obj+0x3A)
 *     +8  s16 objects[4]   node[], negative terminates
 *     +16 u8  time_a       delay,  x300 (obj+0x4C)
 *     +17 u8  time_b       wait,   x300 (obj+0x4E); 0xFF means never
 *
 *     CAGELIFT1, len 20
 *     +4..+15              target, speed and objects exactly as LIFT1
 *     +16 u8 bottom        bottom collision-slab thickness
 *     +17 u8 top           top collision-slab thickness
 *     +18 u8 time_a        delay,  x300 (obj+0x4C)
 *     +19 u8 time_b        wait,   x300 (obj+0x4E); 0xFF means never
 *
 * The axis is Y, as `MOVER_A`'s is: these are lifts. The object slots are
 * OBJSLOTs and therefore subject to the two-buffer rebase (#56), so an `ops` is
 * taken; pass NULL to read them in place.
 *
 * BUTTON and PISTON are built here too: both name a target AND a speed, which
 * is what a mover needs. A button's speed is literally one unit a tick — the
 * table says `travel`'s SIGN selects obj+0x3A = +1 or -1 and its magnitude goes
 * to obj+0x44. A PISTON has four object slots and a signed +18 pusher gate.
 * Only a non-zero gate allocates its +0x28 pusher, which is what can reach the
 * generic carry/rollback MOD_CRUSH path. All thirteen PAL PISTON calls leave
 * that gate zero: they are visual actuators, not solid crushers.
 *
 * STALE TEXT REMOVED. This block used to say PLATFORM, DISH and CAGELIFT1 were
 * "deliberately NOT built here" because none of them names both a target and a
 * speed. All three ARE built — the code below has read each constructor since
 * — and the paragraph survived to contradict it, which is the sort of thing
 * that makes the next reader distrust the file rather than the sentence.
 *
 * What is still true of CAGELIFT1: it takes TWO boxes, not one. Its
 * constructor at 0x80029794 calls the slot allocator twice (0x80029A78 and
 * 0x80029B1C) and chains them through +0x3C — a top slab whose max[1] is
 * min[1] + item[+17] and a bottom slab whose min[1] is max[1] - item[+16]. It
 * is a cage with a floor and a ceiling. `q2_sim_attach_movers` preserves those
 * two slabs through `cage_top` and `cage_bottom` rather than registering the
 * whole airy Scene-node bounds as solid.
 *
 * PLATFORM has the same shape of problem and the console has it too: its
 * constructor registers ONE box per part, the whole Scene node bounding box,
 * and BIGGUN's deck is 939 units tall because the box includes the air you
 * stand in. A player inside the carriage is inside the box rather than on top
 * of it, so they neither ride it nor are pushed by it — they stand on the
 * static hull underneath while it leaves. That is what `0x800555D8` does with
 * `scene_node_record + 16` and there is no slab-cutting anywhere in
 * `0x8002CBB0`, so it is not a gap here; it is written down so the next reader
 * does not go looking for the missing carry.
 *
 * Appends to `set`, so call it after `q2_movers_build` with the same set: the
 * two families share a tick, a draw offset and a trigger.
 */
q2_result q2_movers_build_calls(q2_mover_set *out, const q2_events *events,
                                const q2_userfuncs *uf,
                                const q2_uf_operands *ops,
                                const q2_scene *scene);

/*
 * Advance every mover by `dt` in the simulation's own units.
 *
 * `player_keys` gates locked doors. Returns the number that moved, so a caller
 * can tell a static frame from a busy one.
 */
u32 q2_movers_tick(q2_mover_set *set, s32 dt, u16 player_keys);

/* Take the sound a mover asked for this tick, or Q2_MVSND_NONE. */
s8 q2_mover_take_sound(q2_mover_set *set, u32 index);

/* And the train's, from the other table — Q2_MOVER_TRAVEL_*_ID or 0. */
u8 q2_mover_take_travel_sound(q2_mover_set *set, u32 index);

/*
 * The displacement this mover is currently applying, on all three axes.
 *
 * One place, because four callers need it and three of them used to reach into
 * `axis` and `offset` themselves: the zone draw, the collision boxes, the
 * obstruction sweep and the report. An axis mover fills one component; a train
 * fills three.
 */
void q2_mover_displacement(const q2_mover *m, s32 out[3]);

/*
 * The same tick, with an OBSTRUCTION TEST — the half of the state machine that
 * was decoded and dead.
 *
 * `Q2_MV_BLOCKED` had a `case` and a countdown and nothing anywhere assigned
 * it, and `block_flags` had three writers and no reader, so a door closing on
 * the player neither stopped, nor reversed, nor crushed: it passed through
 * them, which is the other half of "movers are non-solid".
 *
 * `blocked(index, step, user)` is asked, before a step is committed, whether
 * moving mover `index` by `step` along its own axis would sweep through
 * something. That is 0x80051EC0's return — it reports 0 when 0x800519B0 cannot
 * carry an entity out of the swept box — and 0x80025CBC is what acts on it:
 *
 *   opening (state 1) and bit 0 of obj+0x58 clear -> stop
 *   closing (state 3) and bit 1 clear             -> stop
 *   the matching bit SET                          -> carry on regardless
 *
 * and "stop" means: save the state, enter state 4, load the timer at obj+0x56
 * with 16. A pusher-enabled PISTON sets neither bit; its generic failed-carry
 * path inflicts 30 MOD_CRUSH damage before this state handler receives the
 * veto. A zero-gate PISTON has no pusher and never calls this path.
 *
 * Passing NULL is the old behaviour and never blocks.
 */
/*
 * `step` is a THREE-VECTOR because a train's is. Everything else fills one
 * component and leaves the other two zero, which is what an axis mover's sweep
 * has always been; a PLATFORM fills all three, and a caller that projected it
 * back onto one axis would test a box the mover never sweeps.
 */
typedef bool (*q2_mover_blocked_fn)(u32 index, const s32 step[3], void *user);

u32 q2_movers_tick_blocked(q2_mover_set *set, s32 dt, u16 player_keys,
                           q2_mover_blocked_fn blocked, void *user);

/* How long a blocked mover waits before trying again — obj+0x56, loaded with
 * 16 at 0x80025D08 on both the stopped and the crushing arm. */
#define Q2_MOVER_BLOCK_TICKS 16

/*
 * A TRAIN DOES NOT WAIT, IT BACKS OFF — 0x8002CAE0 and 0x8002CB34.
 *
 * Blocked, PLATFORM's handler takes the state it was in, saves it to obj+0x56,
 * loads obj+0x4E with `progress -/+ speed * 150` clamped into [0, target], and
 * enters state 4. State 4 does nothing but run 0x8006FEB8 — a clamped
 * move-toward — until progress reaches that goal, then restores the saved
 * state and carries on. Blocked while opening it retreats; blocked while
 * closing it advances; blocked again mid-retreat it simply does not move.
 *
 * The goal lives in the WAIT field because that is where the console puts it:
 * obj+0x4E is the hold-open countdown in state 6 and the back-off goal in state
 * 4, and the two never overlap. Reusing `wait_timer` for it keeps the save
 * format unchanged, since `wait_reset` still carries the authored value.
 */
#define Q2_MOVER_PATH_BACKOFF 150

/*
 * The displacement to add to a Scene node's origin when drawing it, or zero.
 *
 * This is how movement reaches the renderer, and it is the original's own
 * mechanism rather than a convenience: the per-frame handler at 0x80025658
 * accumulates the displacement in the mover's runtime object at +0x12, and the
 * zone draw at 0x800678EC adds that triple to the node's camera-space position
 * as it draws it. The geometry is never modified, which is why every node in a
 * zone can share one origin and doors still move.
 *
 * Linear scan because a zone has at most a few dozen movers and this is called
 * once per node per frame.
 */
void q2_movers_node_offset(const q2_mover_set *set, u32 scene_node, s32 out[3]);

/*
 * BASE0 LevelBin +0x0094, the DOCRATES mission-event handler.
 *
 * The named CRATES record is a zero-speed LIFT1 whose four Scene slots exist
 * only to allocate runtime objects. Every call advances slots 0/1 by
 * (16*dt)/8, slots 2/3 by (20*dt)/8, then wraps an object 3500 units backward
 * when its translated Scene-box centre reaches -1044. Returns objects moved.
 */
u32 q2_movers_step_crates(q2_mover_set *set, const q2_events *events,
                          const q2_scene *scene, s32 dt);

#endif /* Q2PSX_MOVER_H */
