/*
 * rotator.h — brush geometry that turns: ROTHATCH, SIMROT, SIMROT2, ROTBUTTON.
 *
 * `mover.h` covers the linear family and says, correctly, that ITS integrator
 * contains no rotation. The engine does: a Scene node bound to a runtime object
 * (`Scene.flags08` bits 0-9, see surface.h) picks up a full rotate-about-pivot
 * transform at draw time, and this module is the half that produces the angle.
 *
 * ---------------------------------------------------------------------------
 * The transform the zone draw applies (0x800678B4 - 0x8006793C)
 * ---------------------------------------------------------------------------
 *     R = RotMatrix(obj[0x0C], obj[0x0E], obj[0x10])     libgte, 0x80089E38
 *     p =           obj[0x18], obj[0x1A], obj[0x1C]      the PIVOT
 *     d =           obj[0x12], obj[0x14], obj[0x16]      the linear displacement
 *
 *     node position = origin - camera - (R . p) + p + d
 *
 * with `R . p` computed by the 1.3.12 matrix-vector routine at 0x8006FB18 (each
 * component summed with `mult` and then `>> 12`).
 *
 * That `- R.p + p` is the giveaway, and it is why `obj+0x18` is a pivot rather
 * than the "second independent displacement" an earlier reading of this file
 * called it: rotating a point v about p is `R.(v - p) + p = R.v - R.p + p`, so
 * the vertices go through R and the translation carries the rest.
 *
 * All four constructors write that pivot; leaving the zero from the object's
 * memset makes an otherwise-correct matrix orbit the brush around local zero:
 *
 *   SIMROT/SIMROT2  0x800284C8..0x80028548 / 0x800286C0..0x80028740
 *   ROTBUTTON       0x8002C210..0x8002C288
 *       p = midpoint(node.bbox_min, node.bbox_max) - node.origin
 *
 *   ROTHATCH        0x8002B798..0x8002B830
 *       p.x = midpoint_x - origin_x - item.s16(+10)
 *       p.y = midpoint_y - origin_y + item.s16(+12)
 *       p.z = midpoint_z - origin_z + item.s16(+14)
 *
 * The midpoint is the raw Scene box, without the renderer's culling slop, and
 * the `srl 31; addu; sra 1` sequence rounds an odd negative sum toward zero.
 * Each result is stored with `sh`, so `p` is an s16 local coordinate exactly as
 * the draw expects.
 *
 * ---------------------------------------------------------------------------
 * The integrator (0x8002F1A8), reached from the 48-object sweep at 0x8002DC04
 * ---------------------------------------------------------------------------
 *     if (!(obj[0x50] & 0x01000000)) return;          // not stepping
 *     obj[0x20] += (s16)obj[0x3A] * dt;               // 32-bit accumulator
 *     obj[0x0C + 2*axis] = (obj[0x20] >> 8) & 0xFFF;  // 4096-step angle
 *     obj[0x50] &= ~0x01000000;                       // consume the step
 *
 * with `axis = (obj[0x50] >> 14) & 3` and `dt` the frame delta at 0x800B2DB4.
 *
 * **It is one step per request, not a free spin.** The handler clears its own
 * enable bit, and `SIMROT`'s exec (0x8002DEC8) is what sets it — a loop over
 * four object slots doing `obj[0x50] |= 0x01000000`. So continuous rotation is
 * a script firing SIMROT every tick, and a single call turns the geometry by
 * exactly `speed * dt`. A port that implements it as a constant angular
 * velocity spins doors that should have nudged.
 *
 * The accumulator is 32-bit and the angle is its bits 8..19, so `speed` is in
 * 1/256 of an angle step per unit of dt and a full turn takes 4096*256/speed.
 * Nothing wraps the accumulator, which is the original's behaviour: it takes
 * about 2^31/speed*dt to overflow and no level runs that long.
 *
 * ---------------------------------------------------------------------------
 * Where the parameters come from
 * ---------------------------------------------------------------------------
 * `SIMROT` / `SIMROT2` constructor, 0x800285A4:
 *     item+4  u16  -> obj+0x3A   angular speed
 *     item+20 u16  -> obj+0x50 bits 14-15, masked with 3: the axis
 *     item+12..18  four object slots, Scene node indices on disc
 *
 * `ROTHATCH` constructor, 0x8002B634, shares every field: the same axis bits,
 * the same `obj+0x3A` from its own `item+4`, the same `obj+0x38` node, plus the
 * three hinge adjustments described above. Its two time operands are already
 * in userfuncs.c.
 *
 * `ROTBUTTON` is the one with a target: its rotation stops at a fixed 0x800
 * (half a turn), which userfuncs.c records.
 */
#ifndef Q2PSX_ROTATOR_H
#define Q2PSX_ROTATOR_H

#include "events.h"
#include "q2psx.h"
#include "scene.h"
#include "userfuncs.h"

/* obj+0x50 bit 24 — "advance one step this frame". */
#define Q2_ROT_STEP_PENDING 0x01000000u

/* obj+0x50 bits 14-15. */
#define Q2_ROT_AXIS_SHIFT 14
#define Q2_ROT_AXIS_MASK  0x0000C000u

/* The accumulator's fractional shift and the circle it feeds. */
#define Q2_ROT_ACCUM_SHIFT 8
#define Q2_ROT_ANGLE_MASK  0xFFF

/* ROTBUTTON's fixed rotation target, from 0x8002C150. */
#define Q2_ROT_BUTTON_TARGET 0x800

/*
 * There are THREE rotation integrators, not one, and they scale the angle
 * differently. Reading either family's arithmetic onto the other turns a hatch
 * 256 times too fast or a SIMROT 8 times too slow.
 */
typedef enum q2_rot_kind {
    /*
     * SIMROT, SIMROT2 — 0x8002F1A8.
     *   accum += speed * dt;  angle = (accum >> 8) & 0xFFF
     * One step per request; the handler clears its own enable bit.
     */
    Q2_ROT_ACCUM = 0,

    /*
     * ROTHATCH — 0x8002B460, inside a seven-state machine whose jump table is
     * at 0x800ABDA0, the same shape as the linear mover's.
     *   angle = (angle + (speed * dt) / 8) & 0xFFF     rounded TOWARD ZERO
     * and it runs until it passes `target`, then arrives. No accumulator.
     */
    Q2_ROT_TARGET,

    /*
     * ROTBUTTON — 0x8002BFD8 sets the angle to a literal 2048 when pressed and
     * the handler at 0x8002C078 stores zero back when its timer expires. It is
     * a snap, not a sweep, and it is hard-wired to obj+0x0E, the Y slot.
     */
    Q2_ROT_SNAP
} q2_rot_kind;

typedef struct q2_rotator {
    q2_rot_kind kind;

    s16  node;          /* obj+0x38: the Scene node this turns, -1 if unbound */
    u8   axis;          /* 0 = X, 1 = Y, 2 = Z                                */
    s16  speed;         /* obj+0x3A                                           */
    s16  target;        /* obj+0x44: where a TARGET rotation stops            */

    s32  accum;         /* obj+0x20, the 32-bit accumulator (ACCUM only)      */
    s16  angle;         /* obj+0x0C + 2*axis, a 4096-step angle               */
    s16  pivot[3];      /* obj+0x18, RELATIVE to the node's origin            */

    bool step_pending;  /* obj+0x50 bit 24                                    */
    bool running;       /* a TARGET rotation is sweeping                      */
    u16  hold;          /* obj+0x4E: a SNAP's remaining hold, 0 = released    */
    u16  hold_reset;    /* what a press reloads `hold` with                   */

    /*
     * THE ONE MOVER FAMILY WITH A START / LOOP / STOP SOUND.
     *
     * A linear door plays one sound and nothing else — `pt1__strt`, once, at
     * 0x80025960 and 0x80025A40. A ROTHATCH plays all three, and the disc's own
     * cross-references say so plainly: of the three handles,
     *
     *   pt1__strt  0x800B27D8  read at 0x80025960, 0x80025A40 (linear door),
     *                          0x8002B3C8 (this), 0x8002C5C0, 0x8002C6A0
     *                          (PLATFORM's own handler 0x8002C2D4)
     *   pt1__mid   0x800B27DC  read at 0x8002B3DC and NOWHERE ELSE
     *   pt1__end   0x800B27E0  read at 0x8002B534 and NOWHERE ELSE
     *
     * — so the loop and the arrival belong to the rotating hatch alone, and the
     * port played neither, because this module had no sound path at all. That is
     * the residue behind "door sounds use platform sounds": the hatches that
     * should open with a motor spinning up, running and stopping were silent,
     * leaving only the linear door's single plat thud to be heard.
     *
     * `sound_pending` is drained by the owner, which is the only thing that
     * knows where the rotator's node is and can therefore position the voice.
     * `looping` says the drained sound is the MID one, which the caller must
     * start as a loop and stop on arrival (0x8002B3DC goes through the looping
     * entry point 0x80073734; 0x8002B534 stops it with 0x8007398C).
     */
    s8   sound_pending;
    bool loop_running;  /* the MID loop has been started and not yet stopped */
} q2_rotator;

/* Which sound a rotator is asking for. Indices into q2_rot_sound_name. */
typedef enum q2_rot_sound {
    Q2_ROTSND_NONE = -1,
    Q2_ROTSND_START = 0,   /* pt1__strt, 0x8002B3C8 — the turn begins       */
    Q2_ROTSND_MID,         /* pt1__mid,  0x8002B3DC — looping while it runs */
    Q2_ROTSND_END,         /* pt1__end,  0x8002B534 — it has arrived        */
    /* ROTBUTTON's own, played on the press (0x8002BFB8, exec 0x8002BEB0)
     * AND again on the release (0x8002C11C, handler 0x8002C020). */
    Q2_ROTSND_BUTTON,      /* amb_butn2, 0x800B27E4                          */
    Q2_ROTSND_COUNT
} q2_rot_sound;

extern const char *const q2_rot_sound_name[Q2_ROTSND_COUNT];

typedef struct q2_rotator_set {
    q2_rotator *rotators;
    u32         count;
    u32         capacity;

    /*
     * Where an object slot is READ from, which is not where the script is.
     *
     * `0x800285CC` walks the item in the chunk at `gp+372` but reads each slot
     * from the same offset in the chunk at `gp+376`, and stamps -1 into the
     * first as it goes:
     *
     *     800285F0  subu s2, s1, baseA      ; the offset within A
     *     800285F4  addu s2, s2, baseB      ; the same offset within B
     *     8002861C  sh   -1, 0(s1)          ; consume the slot in A
     *     80028620  lh   v0, 0(s2)          ; read the operand from B
     *
     * A is COMMON's Events chunk; **B is the ZONE's** — the zone loader looks
     * "Events" up between "CastList" and "CreAIBin" (`0x8007C14C`) and stores it
     * at `gp+376` (`0x8007C234`). Parsing only A therefore sees -1 in every slot
     * the game has already consumed, which is why 69 of 95 rotation calls looked
     * inert. See openquestions #56.
     *
     * Leave both NULL to read operands in place, which is what tests want.
     */
    const u8   *operand_base_a;   /* the chunk the item pointer lives in */
    const u8   *operand_base_b;   /* the chunk its operands live in      */
    u32         operand_b_size;
} q2_rotator_set;

/* Take the sound this rotator is asking for, or Q2_ROTSND_NONE. See the note on
 * q2_rotator.sound_pending for why only this family has three. */
s8 q2_rotator_take_sound(q2_rotator_set *set, u32 index);

/*
 * Point a set's operand reads at a second chunk, as the engine does.
 * Passing NULL for either side restores reading operands in place.
 */
void q2_rotators_set_operand_source(q2_rotator_set *set,
                                    const u8 *base_a,
                                    const u8 *base_b, u32 b_size);

/*
 * Build the set from a map's Events chunk: every SIMROT, SIMROT2, ROTHATCH and
 * ROTBUTTON item, with the operands each constructor actually reads.
 *
 *   SIMROT / SIMROT2   0x800285A4
 *     item+4   s16  speed        -> obj+0x3A
 *     item+12  s16[4] objects
 *     item+20  u16  axis & 3     -> obj+0x50 bits 14-15
 *
 *   ROTHATCH           0x8002B634
 *     item+4   s16  speed magnitude; the SIGN is chosen by the target:
 *              positive when target < 2048, negative otherwise (0x8002B70C)
 *     item+6   s16  target       -> obj+0x44
 *     item+8   u8   axis & 3     -> obj+0x50 bits 14-15   (a BYTE, unlike SIMROT)
 *     item+10  s16[3] hinge adjustment -> obj+0x18 after the node-centre rule
 *     item+18  s16  object
 *
 *   ROTBUTTON          0x8002C150
 *     item+6   s16  hold time    -> obj+0x4E, *300; -1 means never release
 *     item+10  s16  object
 *     axis and target are not operands: the exec hard-wires obj+0x0E and 2048.
 *
 * Like the mover builder, this reads the DISC values — the slots are Scene node
 * indices before the load-time pre-pass rewrites them — so no rewrite is
 * performed and none is needed. `scene` supplies the raw boxes and origins for
 * the retail obj+0x18 pivot writes. Analysis-only callers may pass NULL when
 * they need the rotator census but will never draw the resulting set.
 */
q2_result q2_rotators_build(q2_rotator_set *out, const q2_events *events,
                            const q2_userfuncs *uf, const q2_scene *scene);
void      q2_rotators_free(q2_rotator_set *set);

/* Append one rotator directly. For callers that drive rotation from something
 * other than a script item, and for tests. Returns NULL when out of memory. */
q2_rotator *q2_rotators_add(q2_rotator_set *set, q2_rot_kind kind,
                            s16 node, u8 axis, s16 speed);

/* What SIMROT's exec does (0x8002DEC8): request one step. */
void q2_rotator_trigger(q2_rotator_set *set, u32 index);

/* Request a step on every rotator bound to `node`. Returns how many were
 * actually asked, which is 0 when the script names a node this map binds no
 * rotator to — a distinction a caller counting its own requests cannot make. */
u32 q2_rotator_trigger_node(q2_rotator_set *set, u32 node);

/*
 * The other half of the builder: a running script has reached a CALL item, so
 * ask the nodes it names to take a step. Returns how many ROTATORS were asked —
 * not how many slots the item names, which is a different and more flattering
 * number: a script may name a node this map binds no rotator to, and counting
 * the request instead of the hit reports rotation where there is none.
 *
 * This lives beside `q2_rotators_build` because it must read the object slots
 * from exactly the same operand offsets, and those differ per primitive —
 * SIMROT names four objects at +12..+18, ROTHATCH one at +18, ROTBUTTON one at
 * +10. Two copies of that table would rot apart and turn the wrong geometry.
 *
 * Nothing turns without this. Every kind sits still until a step is requested
 * (0x8002F1B8), so a map whose rotators are built but never called reports
 * `rot moved 0` forever, which is what it did until this existed.
 */
u32 q2_rotators_call(q2_rotator_set *set, const q2_userfuncs *uf,
                     const q2_event_item *item, u8 call_index);

/*
 * One pass of the 48-object sweep. Advances every rotator with a pending step
 * and consumes the request. Returns how many actually moved, so a caller can
 * tell a still frame from a turning one.
 */
u32 q2_rotators_tick(q2_rotator_set *set, s32 dt);

/*
 * The rotation to apply when drawing `node`: three Euler angles on the
 * 4096-step circle, and the pivot they turn about.
 *
 * Returns false when the node has no rotator, in which case `angles` and
 * `pivot` are zeroed and the caller draws it unrotated. A node binds to at most
 * one runtime object, so the first match is the only match.
 */
bool q2_rotators_node_transform(const q2_rotator_set *set, u32 node,
                                s16 angles[3], s16 pivot[3]);

#endif /* Q2PSX_ROTATOR_H */
