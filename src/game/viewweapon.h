/*
 * viewweapon.h — the weapon in the player's hands.
 *
 * ---------------------------------------------------------------------------
 * It is a model, not an overlay
 * ---------------------------------------------------------------------------
 * The console does not paste a sprite over the frame. The view weapon is a
 * `CastList` model — the bank names it "Blaster G", "Supershot G" — placed at
 * the player's eye and drawn through the same GTE, into the same ordering
 * table, as everything else. That is why it sorts against the world instead of
 * always winning, and it is why this module produces primitives rather than
 * pixels.
 *
 * ---------------------------------------------------------------------------
 * The state machine — 0x8004F87C
 * ---------------------------------------------------------------------------
 * Four states, and every transition is an animation event rather than a timer:
 *
 *        RAISE ──clip ends──> IDLE ──fire pressed──> FIRE
 *          ^                   │                      │
 *          │                   └──selection changed───┤ clip ends
 *          │                            │             v
 *          └── model swapped <── LOWER <┘            IDLE
 *
 * The whole "has the animation finished" test is `clip_start + 20*frame ==
 * clip_end` (0x8004F93C) — the clip bank stores no length, only neighbouring
 * pointers, so the end IS the next clip's start (vmtables.h).
 *
 * Two details that a port gets wrong by simplifying:
 *
 *   - **LOWER holds rather than loops.** When the lower clip runs out, the
 *     70-tick switch countdown at weapon-block +222 is checked (0x8004F944); if
 *     it has not expired the frame index is *rewound by one* and the state
 *     stays, so the weapon hangs at the bottom of its arc. The model is only
 *     swapped when the countdown reaches zero.
 *   - **Running dry does not just stop the gun.** If the fire function reports
 *     it could not fire, the machine clears the fire latch and recomputes the
 *     next and previous weapons (0x800506C4 / 0x80050758 at 0x8004FB5C), which
 *     is what makes the console auto-switch off an empty weapon.
 *
 * ---------------------------------------------------------------------------
 * Where it sits, which is the part ports get visibly wrong
 * ---------------------------------------------------------------------------
 * Read at 0x8004F5E8…0x8004F640:
 *
 *     pos.x = player.x + rotated_offset.x
 *     pos.y = player.y + rotated_offset.y + 286 - view_offset
 *     pos.z = player.z + rotated_offset.z
 *
 * The `286 - view_offset` is not a fudge — it is the *same expression the
 * camera uses* (FORMATS §9.12: `eye.y = pos.y + 286 - viewOffset`). The weapon
 * hangs off the eye, so it rises and falls with a crouch exactly as the view
 * does, and no separate bob is needed to make it feel attached.
 *
 * The rotation is `RotMatrix(angles)` (0x80089E38 at 0x8004F464) where the
 * angles are the player's aim and the second angle triple, with **x negated**
 * (0x8004F41C) — the animation's own rotation is NOT in that matrix; it reaches
 * the model separately (0x8004F474's MulMatrix). Then the offset from the
 * animation is rotated before the eye is added, so the weapon swings with the
 * view rather than sliding across it.
 *
 * ---------------------------------------------------------------------------
 * The model is authored in VIEW space, and that is the whole trick
 * ---------------------------------------------------------------------------
 * `Blaster G` has bounds `[-45 -105 0] .. [67 70 482]`: the grip sits on the
 * origin and the barrel runs out along +Z. Its own axes ARE the camera's, so
 * the matrix it must be drawn with is the camera's **inverse**, composed with
 * the clip's rotation:
 *
 *     camera * (camera^T * R_clip)  ==  R_clip
 *
 * which is the port's form of the original composing the view rotation into the
 * entity's matrix and letting the camera undo it. Two mistakes follow from
 * missing this, and both were made here before they were fixed: rotating the
 * offset by the camera's FORWARD matrix sends the weapon out along a world axis
 * and puts it behind the eye, where every face is rejected at the projection
 * plane; and folding the clip's angles into that same matrix does it again,
 * because several clips carry a half-circle component.
 *
 * ---------------------------------------------------------------------------
 * Two more things the model does, and one thing it does not
 * ---------------------------------------------------------------------------
 * The clip bank animates where the weapon IS; the MODEL animates the weapon.
 * Six of the eleven view models carry a named move in their own block D, and
 * 0x80050454 starts it off a key event — see `anim_pos` below. The weapon was
 * drawn UNPOSED before that was wired, which is not "no animation" but "the raw
 * vertex block": `HandGren G` came out as a fist-sized lump instead of an arm.
 *
 * The hyperblaster has a second, finer one: 0x8004FC78 runs only for weapon 9
 * and drives a 1.0.12 ramp — 4096 in IDLE, then 4096/3296/2096/1296 minus forty
 * per elapsed tick over fire frames 1..4, then `(left << 12) / duration` — into
 * 0x8006D43C, which PATCHES one component of part 6's quaternion in the model's
 * own key data. That is the barrel turning. `q2_viewweapon_build_ot` now applies
 * the same low-eleven-bit patch to the per-instance pose rather than mutating
 * the shared model bank; the result is identical and remains player-local in
 * split screen.
 *
 * And the thing it does not do: 0x8004F644…0x8004F694 rotates a vector by the
 * composed matrix and adds the result to the weapon's world position, which
 * looks like a per-model offset and is DEAD. The vector is `sp+24`, and the
 * only write to it in the whole function is the `memset(sp+24, 0, 6)` at
 * 0x8004EE18. It is a zero rotated by a matrix, added to a position. Nothing
 * here reproduces it because there is nothing to reproduce.
 */
#ifndef Q2PSX_VIEWWEAPON_H
#define Q2PSX_VIEWWEAPON_H

#include "gpu.h"
#include "gte.h"
#include "model.h"
#include "modeldraw.h"
#include "q2psx.h"
#include "vmtables.h"
#include "weapontables.h"   /* Q2_WSND_*: the chaingun arm names its own */
#include "world.h"

/*
 * The packed {sine, cosine} table `RotMatrix` reads — 0x80089E60's
 * `lw t9, 0x800A5430(angle*4)`, four bytes an entry. Named here because the
 * weapon's rotation is the chain that starts at it, and because the tool checks
 * `src/common/trig.c`'s generated table against it entry for entry.
 */
#define Q2_VW_SIN_TABLE  0x800A5430u

/* 0x8004ECEC — the switch countdown the LOWER state waits on. */
#define Q2_VW_SWITCH_TICKS  70

/* FORMATS §9.12. The weapon hangs off the eye, so it uses the eye's own base. */
#define Q2_VW_EYE_BASE     286

/*
 * 0x800503F8. The hand grenade will not prime until its model's `Set` move has
 * reached this position — the arm has to have the grenade up before the hold
 * can start. It is a position on the model timeline, so it is in the same
 * units as `anim_pos`, and it sits inside `Set`'s own 0..470 span.
 */
#define Q2_VW_COOK_POSITION  380

/* Grenade3's think compares the owner's current model position with its
 * previous one and raises these two one-shot crossings. The first plays
 * wep_hgrenc1b; the second reveals/releases the grenade and plays
 * wep_hgrent1a (0x8004A474 and 0x8004A4A8). */
#define Q2_VW_HAND_PRIME_POSITION    261
#define Q2_VW_HAND_RELEASE_POSITION  411

/* Parts in the biggest view model on the disc is 19 (`HandGren G`); the bound
 * is the same one entitydraw.c uses, and a model past it draws unposed rather
 * than not at all. */
#define Q2_VW_POSE_MAX     64

/*
 * The fire function's verdict, as the state machine consumes it. The original's
 * fire functions return a small integer; 2 is the "could not fire" the machine
 * compares against (0x8004FB44), and it is what makes an empty weapon
 * auto-switch rather than click.
 */
typedef enum q2_vw_fire_result {
    Q2_VW_FIRED = 0,
    Q2_VW_FIRE_DENIED = 2,

    /*
     * NOT A CONSOLE VALUE, and negative so it cannot be confused with one.
     *
     * On the console the idle state CALLS the fire function and branches on
     * what it returns, so "the caller decided not to shoot" is not a case that
     * can arise. In this port the sim fires from its own tick and the machine
     * is told afterwards, which introduces a third outcome the original has no
     * encoding for: the trigger was held, the weapon was not dry, and the
     * refire gate said no.
     *
     * Reporting that as Q2_VW_FIRED plays a fire clip for a shot that never
     * happened; reporting it as Q2_VW_FIRE_DENIED switches the player off a
     * perfectly loaded gun. It has to be its own value, and the machine's
     * response to it is to leave the idle state alone.
     */
    Q2_VW_FIRE_NONE = -1
} q2_vw_fire_result;

typedef struct q2_viewweapon {
    const q2_vm_tables *tab;

    /*
     * The ordinary entity fields the view model retains.
     *
     * It is allocated by 0x8006C0F0, so +0xFC/+0xFE and both ambient colours
     * begin at 4096/4096 and 0x40/0x40/0x40.  The constructor at 0x8004F750
     * then writes 1 to +0xF4.  The light gather tests that field with `bgez`
     * at 0x8006B040, so literal 1 selects the ordinary WORLD list and static
     * lamps; only a negative value selects the alternate list.  Treating the
     * write as a boolean view-space flag was a signedness error.
     *
     * 0x8004EE70 refreshes +0xFC from the player each frame; the port's player
     * scale is currently always the allocator default, but keeping the field
     * here preserves the ownership and makes that future copy explicit.
     */
    s16         scale;           /* entity +0xFC light intensity, 4096 = 1.0 */
    s16         fade;            /* entity +0xFE second intensity factor      */
    s16         light_selector;  /* entity +0xF4, constructor writes +1       */
    u8          glow[3];         /* entity +0x2AC, allocator default 0x40      */

    /* What is in the hands now, and what has been asked for. Both are 1-based
     * weapon ids; they differ exactly while the weapon is being swapped. */
    int         weapon;          /* view model +212 */
    int         pending;         /* view model +214 */

    q2_vm_state state;           /* +72  */
    u32         frame;           /* +76  */

    /*
     * The key clock. `total` is the current key's duration and `left` counts
     * down by dt; the interpolation runs on (total - left) / total, so a key
     * with a zero duration is consumed in one step rather than dividing by
     * zero.
     */
    s32         total;           /* +48  */
    s32         left;            /* +80  */

    /*
     * Where the interpolation started — the value the previous key left.
     *
     * THE TWO ADDRESSES USED TO BE THE WRONG WAY ROUND HERE, and it is worth
     * saying which is which because the loop reads like the opposite. The six
     * accumulators at 0x8004EF74…0x8004F1C0 pair the key's fields with the
     * viewmodel's in this order:
     *
     *     key +0/+2/+4  (rotation)     -> viewmodel +224/+226/+228
     *     key +6/+8/+10 (translation)  -> viewmodel +236/+238/+240
     *
     * so the ROTATION base is at +224 and the TRANSLATION base at +236. The
     * field names here follow the key's own layout (vmtables.h has `r` at +0
     * and `t` at +6), which is right; only the addresses in the comment were
     * swapped. Reading the disc settles it either way — the blaster's fire keys
     * hold (1623, 2116, 2348) in the first triple, a hair off a half circle,
     * against (96, 73, 60) in the second, which is a hand's width of offset.
     *
     * There is only ONE such triple per field on the console: the loop
     * accumulates into it in place, and the DISPLAY value is computed fresh
     * after the loop into +230/+232/+234 without being written back
     * (0x8004F2F8). `cur_t` / `cur_r` below are that display value.
     */
    s16         from_t[3];       /* +236 / +238 / +240 — the translation base */
    s16         from_r[3];       /* +224 / +226 / +228 — the rotation base    */

    /* The interpolated present, which is what the transform uses. */
    s16         cur_t[3];
    s16         cur_r[3];

    bool        fire_latch;      /* +216: a shot is in flight this state      */

    /*
     * Set on the pass that releases the latch, which is also the pass on which
     * the original recomputes the player's next and previous weapons
     * (0x8004FB5C). A caller drains it to do the auto-switch off an empty gun —
     * the thing that makes running dry feel like the console rather than like a
     * click.
     */
    bool        refire;
    s16         switch_ticks;    /* weapon block +222, the 70-tick countdown  */

    /* The last event a key raised, and the frame it happened on. A caller
     * drains this to make a muzzle flash or a shell eject happen on the frame
     * the animation says, not on the frame the trigger was pressed. */
    s16         event;
    bool        event_pending;

    /* The model, resolved from the bank by name when the weapon is swapped. */
    const q2_model *model;

    /* ---------------------------------------------------------------------
     * The MODEL's own animation, which is a second clock — 0x80050454
     * ------------------------------------------------------------------- */
    /*
     * The clip bank above animates where the weapon IS. It does not animate the
     * weapon: six of the eleven view models carry a named move in their own
     * block D, and the engine starts it from a key event.
     *
     * 0x80050454 runs on every substep beside the fire driver (0x8004EF38) and
     * is a switch on the weapon id over exactly five weapons, each naming one
     * move by a 12-byte string it hands to 0x8006D330:
     *
     *     3  Supershot G   "Fire"     0x800505EC
     *     6  HandGren G    "Set"      0x800505AC
     *     7  GrenLaunch G  "Spin"     0x800505CC
     *     8  RockLaunch G  "Fire"     0x800505EC
     *    11  Bfg G         "Fire"     0x800505EC
     *
     * and the model banks carry those names on exactly those models and on no
     * others. The trigger is `key.event == 1` while the state is FIRE, and each
     * of those five fire clips has exactly one key carrying it.
     *
     * `anim_pos` IS viewmodel+256, the halfword the pose selector at 0x8006B924
     * consumes as an animation position. The move record reaches it unscaled and
     * openquestions #51f found no load-time multiply anywhere in the image, so
     * the five applied in `anim_step` is an inference — one the hand grenade's
     * own cook threshold forces, because 380 does not fit inside a `Set` move
     * that ends at 94. The argument is written out at the call site.
     *
     * At five the Supershot's `Fire` spans 90 position units and the position
     * advances one per tick, so the break-open takes 0.3 s of a 354-tick clip.
     *
     * With no move playing the position is 0 (0x8004FA44 on the swap), which is
     * the rest pose and NOT the raw vertices — the difference is dramatic on
     * `HandGren G`, whose raw bounds are [-46 -41 -33]..[59 48 69] against a
     * posed [-43 -82 -418]..[248 118 125]: unposed it is a fist-sized lump
     * instead of an arm.
     */
    /*
     * And the hyperblaster's own, which is a different mechanism again —
     * 0x8004FC78, called once per substep and gated on weapon 9 alone.
     *
     * It drives a 1.0.12 ramp into combat+152 and hands it to 0x8006D43C, which
     * walks to PART 6's key in the model's animation and overwrites the low
     * eleven bits of its packed rotation word — the first Euler half-angle — in
     * place. That is the barrel turning, and it is the only animation on the
     * disc driven by patching key data rather than by choosing a frame.
     *
     * The ramp is exact: 4096 while IDLE, and over fire frames 1..4 it counts
     * down from 4096, 3296, 2096 and 1296 by forty per elapsed tick
     * (0x8004FD18 and its three siblings, each `(duration - left + 2) * 40`).
     * Frame 6 is `(left << 12) / duration`, and frames 0, 5 and 7 upward leave
     * it standing. Nothing but IDLE and those frames writes it, so a raise or a
     * lower holds whatever the last shot left.
     */
    s16         hyper_ramp;      /* combat +152, 1.0.12                      */

    /* ---------------------------------------------------------------------
     * The powerup that changes what every weapon sounds like
     * ------------------------------------------------------------------- */
    /*
     * Four sites do the same thing after a shot that succeeded — 0x8004FB9C in
     * the state machine's own fire arm, and 0x80050004, 0x80050234 and
     * 0x80050300 in the machinegun's, chaingun's and hyperblaster's. Each tests
     * the level clock at 0x800AEBAC against the deadline at combat+172, asks
     * 0x800739B8 whether the handle at 0x800B2B80 is already sounding, and if
     * not plays [0x800B28B0] at the player's position.
     *
     * `0x800B28B0` is filled at 0x80037AA0 from the name at 0x800AC2AC:
     * **`itm_damage3`**. It is not one of the twenty-two `wep_*` sounds, which
     * is why it has its own signal rather than riding `frame_sound`.
     *
     * `combat+172` is `q2_inventory.quad_until` and the clock is
     * `q2_combat.level_time`, both of which the port already keeps — so the
     * caller sets `quad_active` each tick and drains `quad_sound` after.
     */
    bool        quad_active;     /* clock < combat+172, set by the caller     */
    bool        quad_sound;      /* a shot wants itm_damage3; drained         */

    s16         anim_pos;        /* +256, the position on the model timeline */
    s16         anim_end;        /* the playing move's last position, -1 idle */
    u16         anim_flags;      /* +258: bit 1 playing, bit 0 has played     */

    /* Diagnostics: how many keys have been consumed since the last reset. */
    u32         keys_played;
    u32         anims_started;   /* how many named moves have been started    */

    /*
     * And how many times the fire clip has been STARTED, which is the number a
     * caller has to be able to see: the machine is told about a shot, but it is
     * the latch and the state that decide whether that becomes a clip. A caller
     * feeding it one report per shot should find this equal to its own count of
     * shots; a caller sampling a latch instead of consuming an event finds it
     * several times higher, at whatever rate it happens to call `advance`.
     */
    u32         fires_started;

    /* ---------------------------------------------------------------------
     * The per-weapon FIRE-state driver's own state — 0x8004FEE8
     * ------------------------------------------------------------------- */
    /*
     * A fifth thing the machine does that had no counterpart here at all.
     * 0x8004FEE8 runs on EVERY substep of the key loop (called at 0x8004EF4C),
     * is gated on `state == FIRE` (0x8004FF38), and then switches on the
     * weapon id to drive the frame directly. Four weapons have an arm:
     *
     *   4  machinegun    while held, frame 2 wraps to 0 — a 3-key cycle
     *   5  chaingun      while held, frame 17 wraps to 9 — the LOOP; on
     *                    release from the loop it jumps to 27, the spin-down.
     *                    Sounds at frames 0, 10 and 18 (up / loop / down) and
     *                    a spin rate of 1, 2 or 3 by frame band.
     *   9  hyperblaster  while held, frame 5 wraps to 1, skipping frame 6
     *   6  hand grenade  the COOK: the frame is pinned at 1 and the key's
     *                    remaining time grows, so it holds while primed
     *
     * Without it the chaingun played spin-up, loop and spin-down once and
     * stopped, the machinegun played its three keys once, the hyperblaster did
     * not hold, and a grenade could not be cooked.
     */
    s32         spin_accum;      /* +52: the shot accumulator, threshold 30  */
    s32         spin_rate;       /* +44: 1, 2 or 3 by the chaingun's band    */
    s32         last_fire_frame; /* +218: fire once per NEW frame            */
    bool        cook;            /* the grenade is primed and held           */

    /* Grenade3 observes the model timeline from its own think. These are the
     * port-side bridge: elapsed time at the pinned 380 position charges the
     * hidden projectile, and crossing 411 releases it. */
    s16         hand_prev_anim;
    s32         hand_cook_ticks;
    bool        hand_release;

    /* The frame-driven shot the arms ask for, drained by the caller: on the
     * console these arms `jalr` the fire function themselves. */
    u32         frame_fires;

    /*
     * And the sound one of them asks for. The chaingun's arm plays a different
     * clip at each band boundary — frame 0 spin-up, frame 10 the loop, frame 18
     * spin-down (0x800B2B38 / 0x800B2B3C / 0x800B2B40) — and all three are in
     * the weapon table and were never emitted by anything.
     *
     * -1 when nothing is pending; the caller drains it.
     */
    s16         frame_sound;
} q2_viewweapon;

/*
 * Take the shot the FIRE-state driver asked for, if it asked for one.
 *
 * The four per-weapon arms call the weapon's fire function directly rather
 * than going through the idle check, which is how a chaingun fires once per
 * animation frame while its loop runs. A caller drains this and performs the
 * shot; returns how many were asked for since the last drain.
 */
u32 q2_vw_take_frame_fires(q2_viewweapon *vw);

/* The sound a frame boundary asked for, or -1. Drains it. */
s16 q2_vw_take_frame_sound(q2_viewweapon *vw);

/* Drain Grenade3's two gameplay outputs. Cook time is in the same 300 Hz dt
 * units passed to q2_vw_advance; release is a one-shot 411 crossing. */
s32 q2_vw_take_hand_grenade_cook(q2_viewweapon *vw);
bool q2_vw_take_hand_grenade_release(q2_viewweapon *vw);

/* The held fuse reached zero. Retail forces fire frame 2, clears the model
 * move, rewinds its position and gives the key 150 ticks (0x8004AA1C). */
void q2_vw_hand_grenade_expired(q2_viewweapon *vw);

/* The quad's own firing sound — `itm_damage3` — if a shot asked for one while
 * `quad_active` was set. Drains it. The console gates a second one on the
 * first still sounding (0x800739B8), which is the caller's mixer to answer. */
bool q2_vw_take_quad_sound(q2_viewweapon *vw);

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */
/*
 * Start with `weapon` in hand, already raised — which is what a level start
 * does. Pass 0 for "no weapon", which is a live state rather than an error:
 * slot 0 aliases slot 1 in the clip table exactly as it does in the fire-
 * function table.
 */
void q2_vw_init(q2_viewweapon *vw, const q2_vm_tables *tab, int weapon);

/* Ask for a different weapon. Takes effect through LOWER; the model does not
 * change until the lower clip has run and the countdown has expired. */
void q2_vw_select(q2_viewweapon *vw, int weapon);

/* Bind the resolved model for the weapon currently in hand. A caller does this
 * after `q2_vw_take_model_name` reports the swap. */
void q2_vw_set_model(q2_viewweapon *vw, const q2_model *model);

/*
 * The 12-byte model name the weapon in hand wants, or NULL when the module has
 * no table. The name is not NUL-terminated at the source, so this returns the
 * table's own fixed-width copy.
 */
const char *q2_vw_model_name(const q2_viewweapon *vw);

/* ------------------------------------------------------------------------- */
/* Per tick                                                                   */
/* ------------------------------------------------------------------------- */
/*
 * Advance by `dt` in the console's 1/300 s units, consuming as many keys as it
 * spans — the original's loop at 0x8004EEA4 takes `min(left, dt)` and repeats,
 * so a long frame plays a short clip through rather than skipping it.
 *
 * `fire_held` is the trigger. `fired` is what the caller's fire function
 * decided when the machine asked it to shoot; pass Q2_VW_FIRED normally and
 * Q2_VW_FIRE_DENIED when the shot could not happen, which is what triggers the
 * auto-switch.
 *
 * Returns true when the weapon in hand changed, which is a caller's cue to
 * resolve the new model name.
 */
bool q2_vw_advance(q2_viewweapon *vw, s32 dt, bool fire_held,
                   q2_vw_fire_result fired);

/*
 * Take the "the latch was released" signal — the pass on which the original
 * recomputes the player's next and previous weapons, and therefore the moment a
 * caller should auto-switch off an empty gun.
 */
bool q2_vw_take_refire(q2_viewweapon *vw);

/* Take the pending animation event, or false when there is none. */
bool q2_vw_take_event(q2_viewweapon *vw, s16 *event_out);

/* True while the machine wants the caller's fire function called this tick. */
bool q2_vw_wants_fire(const q2_viewweapon *vw);

/* ------------------------------------------------------------------------- */
/* Placement and drawing                                                      */
/* ------------------------------------------------------------------------- */
/*
 * Where the weapon is, given the player. `feet` is the player's own position
 * (not the eye), `view_offset` is the crouch easing value the camera uses, and
 * `aim` / `kick` are the two angle triples the original sums — see the header
 * note about which is which.
 *
 * Writes the world position and the three angles the model should be drawn at.
 */
/*
 * ATTRIBUTED, and it is no longer the open question #41 recorded.
 *
 * `aim` is `player+230`, which the call site at 0x8004F40C reads straight out
 * of the entity with `lhu` and uses. `kick` is what 0x80038260 returns, and
 * that function is three near-identical blocks, each loading a DEADLINE
 * (32/36/40 off obj+0xAC), subtracting the clock at 0x800B2BAC, dropping out if
 * it has passed, and otherwise scaling a stored angle pair by the remaining
 * fraction and accumulating into the SAME output. Three contributions, each
 * decaying over its own period, summed - which is a kick and nothing else is.
 * The periods are in the reciprocal constants: 0x88888889 is /30 (firing),
 * 0x1B4E81B5 is /150 (damage), 0xB60B60B7 is /90 (landing).
 */
void q2_vw_place(const q2_viewweapon *vw,
                 const s32 feet[3], s32 view_offset,
                 const s16 aim[3], const s16 kick[3],
                 s32 origin_out[3], s32 angles_out[3]);

/*
 * Emit the weapon into `ot`. The ordering table is not cleared and the GTE's
 * projection is not touched, so this drops into a frame the world has already
 * been built into — which is the only way the weapon can sort against a wall it
 * is pushed into.
 *
 * `proto` supplies the shared render state a caller already has (the texture
 * page table, the CLUT split, lighting); its origin, angles and model are
 * overwritten. Returns the number of primitives emitted.
 */
u32 q2_vw_build_ot(const q2_viewweapon *vw,
                   const q2_model_instance *proto,
                   const s32 feet[3], s32 view_offset,
                   const s16 aim[3], const s16 kick[3],
                   const q2_camera *cam,
                   psx_ot *ot, gte_state *gte,
                   q2_model_draw_stats *stats);

#endif /* Q2PSX_VIEWWEAPON_H */
