/*
 * explosive.h — opcode 0x08, which is this engine's `func_explosive`.
 *
 * Retail Quake II has a brush entity you shoot until it dies, at which point it
 * vanishes, throws debris and optionally detonates. Hammerhead has one too. It
 * is not a UserFuncs primitive and it is not `GLASS`: it is EVENT OPCODE 0x08,
 * 224 items disc-wide, and until this module existed the port recognised it by
 * name (`Q2_EVOP_FXGROUP`) and did nothing with it.
 *
 *     exec        0x800267C4, reached from the dispatch arm at 0x800276B0
 *                 (table 0x800ABD48, index opcode - 2)
 *     constructor 0x80026A20, reached from 0x80026EF8
 *                 (table 0x800ABCF8, index opcode - 3)
 *     item length 28, asserted by BOTH (0x800267E8, 0x80026A40)
 *
 * ---------------------------------------------------------------------------
 * WHY IT IS THE `func_explosive` AND `GLASS` IS NOT
 * ---------------------------------------------------------------------------
 * `GLASS` is a pane: one Scene node, a hit-point counter, two debris counts and
 * `amb_brkglas`. It has no explosion, no sound but that one, and nothing to put
 * in the hole it leaves.
 *
 * Opcode 0x08 carries TWO arrays of four Scene nodes and swaps one for the
 * other. Array A (+6) is the INTACT geometry: shown at load, given a damageable
 * box each, hidden when the thing dies. Array B (+14) is the WRECKAGE: hidden at
 * load, shown when the thing dies. That is a func_explosive with a `_destroyed`
 * brush behind it, expressed the only way a console with no BSP could express
 * it — as a visibility swap between two pre-baked node groups.
 *
 * And it detonates. `0x80026950` calls `0x8005A778` — the same spawner the
 * creature import table calls `spawn_explosion` (creature.h +0x120) — which
 * allocates an entity, binds the model named "Explosion", and gives it the
 * think at `0x8005A5F8`. Then `0x8002695C` plays `wep_grenlx1a`, the grenade
 * report, through the by-handle path at `0x80073704`.
 *
 * ---------------------------------------------------------------------------
 * On-disc layout — 28 bytes, offsets from the start of the item
 * ---------------------------------------------------------------------------
 *   +0   u8      op          0x08, plus the one-shot and disabled bits
 *   +1   u8      len         28 on every item on the disc
 *   +2   s16     -           READ BY NEITHER HANDLER, and see the note below
 *   +4   s16     -           READ BY NEITHER HANDLER
 *   +6   s16[4]  node        the INTACT nodes; -1 terminates a slot, not the
 *                            array — a hole is skipped, not a stop
 *   +14  s16[4]  rubble      the DESTROYED nodes, parallel to the above
 *   +22  s16     health      MUTATED IN PLACE by the exec (0x80026824), which
 *                            is how a group that has taken two shots remembers
 *   +24  u8      hit_pieces  debris thrown by every damaging hit
 *   +25  s8      destroy     SIGNED, and the sign is the whole point:
 *                              >= 0  throw `destroy` pieces, no explosion
 *                              <  0  DETONATE, and throw ~destroy pieces
 *                            (`nor s0, zero, s0` at 0x80026944, so -1 means
 *                            "explode and throw none")
 *   +26  s16     reveal      one more node, hidden at load and shown on death
 *
 * WHY THIS LOOKED LIKE A MOVER FOR SO LONG. Bytes +2..+5 are read by neither
 * handler — grep the two functions and there is no load at those offsets — but
 * they are non-zero on all 288 items on the disc, and they take only SEVEN
 * distinct values: 00800000, FF800001, 0002FFFF, 00020001, 00800002, 00780000,
 * 00800001. Every one of those is a pair of small s16, which is exactly
 * MOVER_A's `travel` at +2 and `speed` at +4 (mover.h). The opcode inherits the
 * movers' header shape and ignores both fields. They are decoded here as two
 * s16 for that reason and acted on by nothing.
 *
 * The four s16 arrays and `reveal` are OBJSLOTs in the userfuncs.h sense: the
 * console rewrites them in place at load and reads the originals out of the
 * second Events buffer. `q2_explosives_build` takes a `q2_uf_operands` for
 * exactly that reason and never performs the rewrite — see mover.h for why a
 * native port wants the disc values rather than the runtime ones.
 *
 * ---------------------------------------------------------------------------
 * What the constructor does — 0x80026A20
 * ---------------------------------------------------------------------------
 *   1. Restore +6..+21 and +26 from the pristine buffer (0x80026A88, 0x80026AD8).
 *   2. `reveal` >= 0            -> flags08 |= 0x8000   (HIDE)   0x80026ACC
 *   3. If health != 0, for each present node of array A:
 *        - flags08 &= ~0x8000  (SHOW)                           0x80026B84
 *        - bind a 92-byte runtime object, slot+1 into flags08 bits 0-9
 *        - `0x80068730` allocates a damageable box from the node's own
 *          bbox at +16 and stores it in obj+0x28
 *        - obj+0x24 = 0x800267C4, the damage callback
 *        - the box's +0x36 gets bit 0x4, which is the ONLY thing a weapon
 *          impact gates on (sim.h)
 *   4. Each present node of array B -> flags08 |= 0x8000 (HIDE) 0x80026C60
 *
 * `health == 0` takes the arm at 0x80026C8C, which shows array A and allocates
 * its boxes but installs NO callback. Such a group cannot be shot at all; it
 * can still be destroyed by a script running the opcode, because the exec's
 * damage argument is then zero and the hit-point branch is skipped entirely.
 * `q2psx-inspect explosives` counts how many of the disc's items are in that
 * state; it is an authored distinction, not an accident.
 *
 * ---------------------------------------------------------------------------
 * What the exec does — 0x800267C4, signature (item, scene_node, damage)
 * ---------------------------------------------------------------------------
 * That signature is the ROUTER's, not a guess: `0x8002EF1C` finds the object
 * whose +0x28 is the box that was hit, stores the damage point in obj+0x00,
 * and calls obj+0x24 with `(item, obj+0x38, damage)` — the item, the node, the
 * amount. So the second argument names WHICH of the four parts took the shot.
 *
 *     if (damage != 0) {
 *         item[+22] -= damage;                      // in the item
 *         debris(scene_node, item[+24], NULL);      // 0x80026820
 *         if ((s16)item[+22] > 0) return;           // still standing
 *     }
 *     if (reveal >= 0) show(reveal);                // 0x80026868
 *     for (i = 0; i < 4; i++) {
 *         n = node[i];
 *         if (n >= 0) {
 *             if (!suppressed) {                    // gp+0x4234, see below
 *                 if (destroy < 0) {
 *                     spawn_explosion(centre(n), scene[n].area & 0x7F,
 *                                     4096, 0);     // 0x80026950
 *                     sound(wep_grenlx1a, centre(n));// 0x8002695C
 *                 }
 *                 debris(n, destroy < 0 ? ~destroy : destroy, NULL);
 *             }
 *             hide(n); free_box(n);                 // 0x80068818
 *         }
 *         if (rubble[i] >= 0) show(rubble[i]);      // 0x800269D0
 *     }
 *
 * Three things a plausible implementation gets wrong, all of them checked
 * against the branch structure rather than inferred:
 *
 *  - THE HIT BURST IS NOT FROM THE IMPACT POINT. `0x80026810` zeroes a2 before
 *    the call, and a zero third argument makes `0x80064558` scatter the pieces
 *    uniformly through the node's box. GLASS does the opposite — it passes the
 *    runtime object, whose +0x00 holds the damage point — so the two primitives
 *    genuinely differ here and copying GLASS's behaviour across is wrong.
 *
 *  - A NEGATIVE SLOT SKIPS THE HIDE TOO. `bltz` at 0x80026890 jumps past both
 *    the effects AND `0x80068818`, but NOT past array B's arm at 0x80026990. So
 *    slot i can name no intact node and still reveal wreckage.
 *
 *  - THE SUPPRESSION FLAG DOES NOT SUPPRESS THE SWAP. `gp+0x4234` is the "no
 *    presentation side effects" pass userfuncs.h documents for STRING, CREBATCH
 *    and friends. Here it skips the explosion, the sound and the debris
 *    (0x8002692C) and leaves the hide, the box free and the whole of array B
 *    running. A group destroyed during the load-time graph evaluation is
 *    therefore already wreckage when the player arrives, silently.
 *
 * ---------------------------------------------------------------------------
 * WHAT A DETONATION SPAWNS, AND WHAT IT DOES NOT
 * ---------------------------------------------------------------------------
 * `0x800267C4` makes exactly FIVE calls and this is all of them:
 *
 *     0x80064558   the hit burst          (0x80026820)
 *     0x8005A778   the Explosion MODEL    (0x80026950)
 *     0x80073704   the report             (0x8002695C)
 *     0x80064558   the destruction debris (0x80026970)
 *     0x80068818   hide the node          (0x80026988)
 *
 * A PARTICLE BURST IS NOT AMONG THEM. `Q2_FX_EXPLOSION` is the burst at
 * 0x800486EC that a rocket or a grenade throws, reached from a different site
 * entirely, and nothing in this handler goes near it.
 *
 * This block used to describe spawning one anyway. When the port had no model
 * entities, `fx_at(sim, Q2_FX_EXPLOSION, ...)` ran here as a deliberate,
 * documented stand-in — and then it SURVIVED the arrival of the real thing, so
 * a detonation drew a bright cyan particle ball the console never puts there,
 * on top of the model it does. A stand-in is only honest while the thing it
 * stands in for is missing; this one outlived that and became a lie about what
 * the engine does.
 *
 * The model is `modelent.h`. `q2_explosive_burst` carries `area` — the Scene
 * node's `area & 0x7F`, which the spawner writes to ent+0x9E — because
 * that is the operand the console passes and this is where it comes from.
 */
#ifndef Q2PSX_EXPLOSIVE_H
#define Q2PSX_EXPLOSIVE_H

#include "events.h"
#include "scene.h"
#include "userfuncs.h"
#include "q2psx.h"

/* Four intact nodes and four rubble nodes — the loop bound at 0x800269F0. */
#define Q2_EXPLOSIVE_MAX_PARTS 4

/* The item length both handlers assert. */
#define Q2_EXPLOSIVE_ITEM_LEN  28

/*
 * `0x80026940` and `0x8002694C`: the third and fourth arguments to
 * spawn_explosion are immediates, the same on every one of the disc's items.
 * Kept as the numbers the executable passes rather than as a name this port
 * invented for them.
 */
#define Q2_EXPLOSIVE_BLAST_RADIUS 4096
#define Q2_EXPLOSIVE_BLAST_KIND   0      /* 0 = "Explosion", 1 = "Hexplosion" */

/*
 * The detonation report: the handle at 0x800B27F8, played at 0x8002695C, which
 * is that handle's ONLY reader in the whole executable.
 *
 * TWELVE CHARACTERS AND NO TERMINATOR. The name is registered from
 * 0x800ABCBC and the next name, `amb_brkglas`, starts at 0x800ABCC8 — twelve
 * bytes later — so the field is full and a disassembler listing it as a C
 * string runs the two together as `wep_grenlx1aamb_brkglas`. Reading it as
 * `wep_grenlx1` and stopping at the `a` looks right and finds nothing: the
 * bank's entry is `wep_grenlx1a` and the port reported "1 not in bank" on
 * every detonation until the last character was put back.
 *
 * This is the same 12-byte-no-strcmp rule userfuncs.h states for MISEVENT.
 */
#define Q2_EXPLOSIVE_SOUND "wep_grenlx1a"

typedef struct q2_explosive {
    s16 node[Q2_EXPLOSIVE_MAX_PARTS];    /* item +6:  intact   */
    s16 rubble[Q2_EXPLOSIVE_MAX_PARTS];  /* item +14: wreckage */
    s16 reveal;                          /* item +26           */

    s16 health;         /* item +22, counted down destructively */
    s16 health_reset;   /* the authored value, so a report can tell them apart */

    u8  hit_pieces;     /* item +24 */
    s8  destroy;        /* item +25, sign-carrying — see the header */

    u8  part_count;     /* present entries of node[], NOT the array length */
    u8  destroyed;

    /*
     * SHOOTABLE AT ALL. False when the authored health is zero: the
     * constructor's 0x80026C8C arm allocates the boxes but installs no
     * callback, so a weapon impact finds a box and has nothing to call.
     */
    u8  damageable;

    /* The item's own chunk offset — its identity, for the same reason
     * q2_mover.item_offset is (mover.h). An ordinal drifts; this cannot. */
    u32 item_offset;
    u32 record_offset;
} q2_explosive;

typedef struct q2_explosive_set {
    q2_explosive *items;
    u32           count;
    u32           capacity;
} q2_explosive_set;

/* ------------------------------------------------------------------------- */
/* What a destruction asks the world for                                      */
/* ------------------------------------------------------------------------- */
/*
 * One entry per intact node that came apart. The console emits these inline;
 * this port collects them because the effects, the sound and the hide array
 * have three different owners and only the caller holds all three.
 */
typedef struct q2_explosive_burst {
    s16  node;          /* the intact node this came out of      */
    s32  at[3];         /* the centre of its box — 0x80026898 on */
    u8   pieces;        /* debris count, already sign-decoded    */
    u8   explode;       /* spawn_explosion + wep_grenlx1a         */
    u16  area;          /* scene[node].area & 0x7F, spawn_explosion's second */
} q2_explosive_burst;

/*
 * Nodes whose visibility changed, as a list rather than a write, because the
 * port's hide array belongs to the client and has a second writer already —
 * the script's OBJDRAWOFF. Same argument as q2_mover.sealed.
 */
#define Q2_EXPLOSIVE_MAX_VIS (Q2_EXPLOSIVE_MAX_PARTS * 2 + 1)

typedef struct q2_explosive_result {
    q2_explosive_burst burst[Q2_EXPLOSIVE_MAX_PARTS];
    u32                burst_count;

    s16                hide[Q2_EXPLOSIVE_MAX_VIS];
    u32                hide_count;
    s16                show[Q2_EXPLOSIVE_MAX_VIS];
    u32                show_count;

    u32                hit_pieces;  /* the per-hit burst, 0 when it was fatal
                                     * or when the hit did no damage          */
    s16                hit_node;    /* which part took it, -1 when none       */

    u8                 destroyed;   /* this call is what killed it            */
} q2_explosive_result;

/* ------------------------------------------------------------------------- */
/* Build                                                                      */
/* ------------------------------------------------------------------------- */
/*
 * Walk `events` and build one entry per opcode-0x08 item — the constructor at
 * 0x80026A20, minus the runtime-object allocation this port does not do.
 *
 * `ops` carries the two-buffer rebase (userfuncs.h #56): the eight object slots
 * and `reveal` are read out of the PRISTINE copy of the Events chunk, which is
 * the zone's, not the one being walked. Pass NULL to read in place — right only
 * for zone 0, exactly as it is for the movers.
 *
 * `scene` may be NULL; when it is given, a slot naming a node the zone does not
 * have is dropped rather than carried, because the console's own scale-by-52
 * would index past the chunk.
 */
q2_result q2_explosives_build(q2_explosive_set *out, const q2_events *events,
                              const q2_uf_operands *ops,
                              const q2_scene *scene);

void q2_explosives_free(q2_explosive_set *set);

/*
 * The visibility the constructor leaves behind for ONE entry: array A shown,
 * array B and `reveal` hidden. Fills `res` the same way a destruction does, so
 * a caller applies both through one path.
 *
 * Per entry rather than per set because a result's lists are sized for what a
 * single item can name. Loop over `set->count` after building. Returns the
 * number of nodes mentioned.
 */
u32 q2_explosive_initial_vis(const q2_explosive_set *set, u32 index,
                             q2_explosive_result *res);

/* ------------------------------------------------------------------------- */
/* Damage                                                                     */
/* ------------------------------------------------------------------------- */
/*
 * The exec at 0x800267C4.
 *
 * `part` is the index into `node[]` whose box was hit — the router's second
 * argument. Pass -1 for a script call, which the console reaches with damage 0
 * and which therefore names no part.
 *
 * `damage` 0 destroys the group outright: the branch at 0x80026808 skips the
 * hit-point subtract and falls straight through, exactly as it does for GLASS.
 *
 * `suppress` is gp+0x4234. Set it while evaluating the graph at load; it costs
 * the effects and keeps the swap.
 *
 * Returns true when this call destroyed the group. `res` may be NULL.
 */
bool q2_explosive_damage(q2_explosive_set *set, u32 index, int part,
                         s16 damage, bool suppress, const q2_scene *scene,
                         q2_explosive_result *res);

/* Index of the entry built from `item_offset`, or -1. */
int q2_explosive_find(const q2_explosive_set *set, u32 item_offset);

/*
 * Destroy whatever the item at `item_offset` built — what a SCRIPT running an
 * opcode-0x08 item means. Returns true when something was destroyed.
 */
bool q2_explosive_trigger_item(q2_explosive_set *set, u32 item_offset,
                               bool suppress, const q2_scene *scene,
                               q2_explosive_result *res);

/*
 * The second argument spawn_explosion is given: the low seven bits of the
 * Scene node record's area byte at +0x0E. Returns 0 for a node the scene does
 * not have.
 */
u16 q2_explosive_node_area(const q2_scene *scene, s16 node);

#endif /* Q2PSX_EXPLOSIVE_H */
