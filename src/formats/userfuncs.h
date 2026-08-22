/*
 * userfuncs.h — the UserFuncs chunk and the CALL (0x16) primitives behind it.
 *
 * CALL is 3,760 of the 6,646 Events items on this disc — 56.6% — so this is
 * where the game's actual behaviour lives. Doors, lifts, buttons, damage
 * volumes, level changes, teleports, secrets, on-screen text, sound and monster
 * spawning are all CALLs. Everything else in Events is plumbing around them.
 *
 * ---------------------------------------------------------------------------
 * The chunk, and why it looks empty
 * ---------------------------------------------------------------------------
 *     u32  count                        0..18 observed; the EXE reads the LOW
 *                                       HALFWORD only (lhu at 0x8002ECDC)
 *     char name[12][count]              NUL-padded primitive names
 *
 * len == 4 + 12*count on 49/49 maps. 17 maps carry count 0 (a bare 4-byte
 * chunk); the rest declare 1..18 names, 38 distinct names game-wide.
 *
 * This chunk is NOT a list — it is an UNRESOLVED DISPATCH TABLE, and the binder
 * at 0x8002EC9C patches it IN PLACE at load time:
 *
 *     for each 12-byte record:
 *         look the name up in the EXE binding table (43 entries, 20 bytes,
 *         { char name[12]; void *fn_exec; void *fn_load }, at 0x8009B6F0,
 *         terminated by a zeroed entry; the compare at 0x8006DBC0 is an exact
 *         three-word match, so names are matched on all 12 bytes)
 *         record[0..3] = fn_exec        (entry +0x0C)
 *         record[4..7] = fn_load        (entry +0x10)
 *         a name that does not match is bound to two do-nothing stubs
 *         (0x8002F228 / 0x8002F230) rather than being an error
 *
 * Bytes 8..11 of each record — the last four characters of the name — are left
 * alone. So AFTER the binder runs the chunk is an array of 12-byte records
 * { void *exec; void *load; char name_tail[4] }, and THAT is what the 12-byte
 * dispatch stride at the CALL site indexes. The base pointer is stashed at
 * gp+0x4218 (pointing at chunk+4, i.e. past the count) and the count at
 * gp+0x4224. There is no separate runtime table and no allocation.
 *
 * The consequence for a port: resolve a CALL by taking the primitive NAME out
 * of the map's UserFuncs chunk at the CALL's index, and dispatch on that. Do
 * not persist the index — it is a per-map ordinal with no global meaning, and
 * the same index means different primitives on different maps.
 *
 * ---------------------------------------------------------------------------
 * How a CALL resolves — CONFIRMED at 0x80027730 (exec) and 0x80026F28 (load)
 * ---------------------------------------------------------------------------
 *     item = { u8 op; u8 len; u8 func_index; u8 pad; u8 operands[len - 4] }
 *
 * func_index is a u8 at +2, read with lbu in BOTH paths. Byte +3 is zero on
 * 3,760/3,760 items, so a u16 read happens to work here and only here.
 *
 *     exec:  if (func_index >= gp[0x4224]) return 0;          // no-op, not an error
 *            fn = *(void **)(gp[0x4218] + 12 * func_index);   // +0 = exec
 *            ret = fn(item, entity, 0);
 *            return gp[0x423C] != 0;                          // abort-record flag
 *
 *     load:  fn = *(void **)(gp[0x4218] + 12 * func_index + 4);   // +4 = load
 *            if (fn) fn(item);
 *
 * The bounds check passes on 3,760/3,760 items on this disc. Note the exec
 * handler also does a `bltz` on the lbu result at 0x80027738, which can never
 * be taken — do not model func_index as signed.
 *
 * The primitive receives THREE arguments: the item pointer, the entity that
 * caused the record to run (the touching player, from record_exec's second
 * argument at 0x80027950), and a scalar that the CALL site always passes as
 * zero. That third argument is a damage amount on the two primitives that
 * double as damage callbacks (GLASS, SHOOTTHEN).
 *
 * CORRECTION — only SHOOTTHEN no-ops on a zero third argument (it returns at
 * 0x8002E840). GLASS does NOT. It spawns its effect at 0x8002A384 before ever
 * examining the argument, and a zero value makes the branch at 0x8002A390 skip
 * the hit-point subtract and fall straight through to the destruction path. So
 * calling GLASS from a script breaks the glass immediately. Treating it as a
 * no-op leaves every scripted glass pane intact.
 *
 * Every primitive returns `item + len` — a cursor advance the CALL site does
 * not use. Length-checked primitives return the item unchanged on a mismatch.
 *
 * ---------------------------------------------------------------------------
 * *** fn_load IS NOT A PER-FRAME INTEGRATOR ***
 * ---------------------------------------------------------------------------
 * An earlier pass recorded the second pointer as a per-frame motion integrator.
 * It is not. It is the LOAD-TIME CONSTRUCTOR, called once per item by the
 * pre-pass at 0x80026DC0 through offset +4, and it does three things:
 *
 *   1. Restores operand bytes from the pristine second Events buffer at
 *      gp+0x178 (the working copy is gp+0x174) — because execution MUTATES the
 *      item in place. SETWIBBLE's and FLKLIGHT's constructors do nothing else.
 *      This settles what those two pointers are: gp+0x174 is the mutable
 *      working copy and gp+0x178 a pristine copy of the same bytes.
 *   2. Allocates 92-byte runtime objects out of the 48-entry array at
 *      0x800D6BB0 and rewrites the item's s16 slots from Scene node indices
 *      into runtime object indices, exactly as documented for opcodes 3/4/5.
 *   3. Installs the object's callbacks: obj+0x2C is the real per-frame tick
 *      (called for all 48 objects from 0x8002DC04 when non-NULL) and obj+0x24
 *      is the on-damage callback. Those are THIRD function pointers, distinct
 *      from both table entries — e.g. LIFT1 and CAGELIFT1 both install
 *      0x80025658, whose first act is to index a 7-entry state jump table at
 *      0x800ABCD8 with obj+0x52.
 *
 * 16 of the 43 primitives have no constructor. Those are exactly the ones with
 * no runtime object: the eight environment flags, MISCOMPLETE, TIMER,
 * TIMEDLIGHT, ONKEYDO, DONTJUMP, DISABLEME, SIMPLESOUND, OBJDRAWON.
 *
 * ---------------------------------------------------------------------------
 * Conditional execution — gp+0x423C
 * ---------------------------------------------------------------------------
 * The record runners clear the byte at gp+0x423C before dispatching items — six
 * `sb $zero` sites, at 0x800272BC, 0x800273B0, 0x80027A8C, 0x80027DCC,
 * 0x80027FD8, 0x80028104 and 0x8002964C — and the CALL handler returns it as
 * "stop this record". A primitive that sets it aborts every item after itself.
 * Exactly two primitives set it, and they are the whole conditional mechanism
 * in the scripting language:
 *
 *     ONKEYDO — four u16 masks tested against the global inventory/key bitfield
 *               at 0x800B29D0; a failed test aborts the record.
 *     TIMER   — parks `item + len` in one of eight 20-byte timer slots at
 *               0x800C6F74 and aborts, so the rest of the record runs later.
 *               That makes TIMER a delayed continuation, not a sleep.
 *
 *               Its delay scale is x30, NOT x300. The sequence at
 *               0x800270F8-0x80027100 is `sll 4; subu; sll 1`, i.e. 15*2.
 *               The movers' genuine x300 is a different idiom entirely
 *               (`sll 2; addu; sll 4; subu; sll 2`, 5 -> 75 -> 300, e.g. LIFT1
 *               at 0x8002DF94), and conflating the two makes every scripted
 *               delay ten times too long — which would look like sluggish
 *               pacing rather than a decoding error.
 *
 * ---------------------------------------------------------------------------
 * Two engine-wide pass flags gate several primitives
 * ---------------------------------------------------------------------------
 *     gp+0x4234  set to 1 around the load-time named-event run (0x8007C2AC)
 *                and around a second internal pass (0x800294B4). While set,
 *                STRING, CREBATCH, HELPCOMPUTER, SIMPLESOUND and INSECRET all
 *                return immediately. It is the "no presentation side effects"
 *                flag: the graph is being evaluated to establish initial state.
 *     gp+0x421C  set to 1 ONLY at 0x8007C2A4, alongside the above. LASERBEAM
 *                registers itself in a 32-entry active list when it is set;
 *                LASERWALL registers when set and deals damage when clear;
 *                MISEVENT fires only when clear. Nothing in the executable
 *                writes it non-zero outside that one pass.
 *
 *                CORRECTION — do NOT conclude from that that LASERBEAM's
 *                registration is unreachable. The pass at 0x80027CC4 is not a
 *                single-record runner; it is a TRANSITIVE QUEUE DRAIN. It seeds
 *                the queue at 0x800C6F24 and loops while the read cursor
 *                (gp+0x4220) trails the write cursor (gp+0x4230), so TRIGGER
 *                items enqueued DURING the pass are drained in the same call,
 *                still with the flag set. All 332 LASERBEAM and 10 LASERWALL
 *                calls on the disc sit in records reachable from the STARTLEV
 *                entry through that closure — 188 of 188 laser-bearing records.
 *                Registration is not merely reachable; it is the only thing
 *                LASERBEAM ever does.
 *
 * ---------------------------------------------------------------------------
 * Operand widths are fixed per primitive — CONFIRMED twice over
 * ---------------------------------------------------------------------------
 * Each primitive has exactly ONE item length across all 3,760 CALL items, with
 * zero exceptions. 22 of the 43 primitives assert that length in code
 * (lbu item[1], compare, bail), and every asserted value equals the observed
 * one. The table below is the union of both sources; primitives never used on
 * this disc carry only the code-asserted value.
 *
 * (An earlier pass said 27, which is the count of non-null CONSTRUCTORS — a
 * different set that happens to be nearby in the same table.)
 *
 * ---------------------------------------------------------------------------
 * A warning about how these operand offsets were established
 * ---------------------------------------------------------------------------
 * They come from the disassembly, NOT from the corpus checks, and that
 * distinction is load-bearing.
 *
 * Decoding all 3,760 items and finding no out-of-range operand sounds like
 * strong confirmation. It is not. Re-running every operand at offsets +/-1,
 * +/-2 and +/-4 gives 438 perturbations of which only 150 are caught: 288
 * (65.8%) pass every check the corpus harness applies. Broken down, the
 * harness catches NAME12 54/54 and OBJSLOT 57/90, but U16 only 15/108, VEC3
 * 0/36, U8 9/72 and S16 8/42.
 *
 * So for every scalar and every VEC3, "the corpus decodes cleanly" would hold
 * for two thirds of WRONG tables and is worth nothing as evidence. Trust the
 * instruction-level derivations for those; trust the corpus only for the name
 * and object-slot fields, where it genuinely discriminates.
 */
#ifndef Q2PSX_USERFUNCS_H
#define Q2PSX_USERFUNCS_H

#include "events.h"
#include "level.h"
#include "q2psx.h"

#define Q2_UF_NAME_LEN     12
#define Q2_UF_RECORD_SIZE  12   /* on disc a name; after binding, two pointers */
#define Q2_UF_MAX_DECLARED 18   /* highest count observed on this disc         */

/*
 * The 43 primitives of the EXE binding table at 0x8009B6F0, in table order.
 * The enum value IS the binding-table index — stable across the whole disc,
 * unlike the per-map UserFuncs index a CALL item carries.
 *
 * Five are bound but declared by no map: UNDERLAVA, SIMBUTTON, B3ROCKS, EXPLO,
 * OBJDRAWON. EXPLO is a pure stub in both slots.
 */
typedef enum q2_uf_prim {
    Q2_UF_STRING = 0,     /* show a Strings entry by 12-byte key              */
    Q2_UF_LIFT1,          /* linear mover, up to 4 objects                    */
    Q2_UF_CAGELIFT1,      /* linear mover variant                             */
    Q2_UF_ROTHATCH,       /* rotating mover                                   */
    Q2_UF_SIMROT,         /* start rotation on up to 4 objects                */
    Q2_UF_SIMROT2,        /* as SIMROT, different constructor                 */
    Q2_UF_INWATER,        /* entity env flag 0x0004                           */
    Q2_UF_INCROUCH,       /* entity env flag 0x0200                           */
    Q2_UF_INLOWCROUCH,    /* entity env flag 0x0400                           */
    Q2_UF_UNDERWATER,     /* entity env flag 0x0100                           */
    Q2_UF_INACID,         /* 1 damage, means-of-death 9                       */
    Q2_UF_UNDERACID,      /* flags 0x1100 + 1 damage, mod 9                   */
    Q2_UF_INLAVA,         /* flag 0x1000 + 20 damage, mod 10                  */
    Q2_UF_UNDERLAVA,      /* flags 0x1100 + 20 damage, mod 10 (unused)        */
    Q2_UF_SIMBUTTON,      /* fires a fixed named event (unused)               */
    Q2_UF_ROTBUTTON,      /* rotate 0x800 units, play button sound            */
    Q2_UF_BUTTON,         /* linear button                                    */
    Q2_UF_LASERWALL,      /* damage volume, mod 11                            */
    Q2_UF_LASERBEAM,      /* beam; registers in a 32-entry active list        */
    Q2_UF_GLASS,          /* breakable; also the object's damage callback     */
    Q2_UF_DISH,           /* one-shot timed object                            */
    Q2_UF_B3ROCKS,        /* timed object + two global object pointers        */
    Q2_UF_PISTON,         /* linear piston; +18 optionally installs a pusher */
    Q2_UF_LOADMAP,        /* change level                                     */
    Q2_UF_INSECRET,       /* secret found: message, sound, counter++          */
    Q2_UF_CREBATCH,       /* spawn a Population group by name                 */
    Q2_UF_MISEVENT,       /* mission event by 12-byte key                     */
    Q2_UF_EXPLO,          /* stub in both slots (unused)                      */
    Q2_UF_HELPCOMPUTER,   /* two Strings keys + a u32                         */
    Q2_UF_FLKLIGHT,       /* flickering light with randomised on/off times    */
    Q2_UF_SHOOTTHEN,      /* hit-point counter; also the damage callback      */
    Q2_UF_MISCOMPLETE,    /* mission complete: fixed start point, state 7     */
    Q2_UF_PLATFORM,       /* linear platform                                  */
    Q2_UF_TIMER,          /* delayed continuation of the rest of the record   */
    Q2_UF_TIMEDLIGHT,     /* timed light                                      */
    Q2_UF_ONKEYDO,        /* inventory/key predicate; aborts the record       */
    Q2_UF_TELEPORT,       /* move the entity to a named StartPos              */
    Q2_UF_SETWIBBLE,      /* write Scene.flags08 bits 10..13                  */
    Q2_UF_DONTJUMP,       /* entity env flag 0x00020000                       */
    Q2_UF_DISABLEME,      /* set DISABLED on the running record               */
    Q2_UF_SIMPLESOUND,    /* play a named sound at a world position           */
    Q2_UF_OBJDRAWOFF,     /* hide up to 4 objects                             */
    Q2_UF_OBJDRAWON,      /* show up to 4 Scene nodes (unused)                */
    Q2_UF_PRIM_COUNT,
    Q2_UF_PRIM_UNKNOWN = -1
} q2_uf_prim;

/*
 * Operand kinds. Every operand of every primitive is one of these five, which
 * is what makes a generic decoder possible at all.
 *
 * NAME12 is a 12-byte NUL-padded key. The EXE passes these BY VALUE in
 * a0/a1/a2 using synthesised unaligned word loads, which is why the argument
 * setup looks like twelve lbu instructions. The namespace differs per operand
 * and is recorded in q2_uf_operand.note.
 *
 * OBJSLOT is the load-mutated s16: a zone-local Scene node index on disc, a
 * runtime object index 0..47 after the constructor runs, and -1 for "empty".
 * A port MUST NOT treat these as stable data.
 */
typedef enum q2_uf_optype {
    Q2_UF_OP_NONE = 0,
    Q2_UF_OP_U8,
    Q2_UF_OP_S8,
    Q2_UF_OP_U16,
    Q2_UF_OP_S16,
    Q2_UF_OP_U32,
    Q2_UF_OP_S32,
    Q2_UF_OP_NAME12,     /* 12-byte key, passed by value               */
    Q2_UF_OP_VEC3_S32,   /* three s32 world coordinates, ABSOLUTE      */
    Q2_UF_OP_OBJSLOT,    /* s16 Scene node on disc / object at runtime */
    Q2_UF_OP_PAD         /* read by nothing; preserve, do not act on   */
} q2_uf_optype;

typedef struct q2_uf_operand {
    u8             offset;   /* byte offset within the item          */
    u8             count;    /* array length; 1 for a scalar         */
    q2_uf_optype   type;
    const char    *name;
    const char    *note;     /* namespace or semantics, may be NULL  */
} q2_uf_operand;

#define Q2_UF_MAX_OPERANDS 8

typedef struct q2_uf_prim_info {
    q2_uf_prim           prim;
    const char          *name;        /* the 12-byte key, NUL-terminated */
    u8                   item_len;    /* the ONE legal item length       */
    bool                 has_ctor;    /* fn_load is non-NULL             */
    bool                 asserts_len; /* the EXE checks item[1] itself   */
    u8                   operand_count;
    q2_uf_operand        operands[Q2_UF_MAX_OPERANDS];
} q2_uf_prim_info;

/* ------------------------------------------------------------------------- */
/* The map's UserFuncs chunk — the per-map index space                        */
/* ------------------------------------------------------------------------- */
typedef struct q2_userfuncs {
    const u8 *data;     /* the whole chunk, borrowed */
    u32       size;
    u32       count;    /* low halfword of the u32 at +0 */
} q2_userfuncs;

/*
 * Parse the chunk. A 4-byte chunk with count 0 parses successfully — that is a
 * real state on 17 maps, not an error.
 */
q2_result q2_userfuncs_parse(q2_userfuncs *out, const q2_common_file *f);

/* Name at a per-map index. `out` must hold Q2_UF_NAME_LEN + 1 bytes. */
bool q2_userfuncs_name(const q2_userfuncs *uf, u32 index, char *out);

/* Per-map index -> stable primitive id, or Q2_UF_PRIM_UNKNOWN. */
q2_uf_prim q2_userfuncs_prim(const q2_userfuncs *uf, u32 index);

/* Static description of a primitive; NULL if `prim` is out of range. */
const q2_uf_prim_info *q2_uf_info(q2_uf_prim prim);

/* Name lookup against the EXE binding table. Returns Q2_UF_PRIM_UNKNOWN for a
 * name this build does not bind — the engine treats that as a silent no-op, so
 * a port should warn and continue rather than fail the map. */
q2_uf_prim q2_uf_prim_from_name(const char *name);

/* ------------------------------------------------------------------------- */
/* Decoding one CALL item                                                     */
/* ------------------------------------------------------------------------- */
typedef struct q2_uf_call {
    u8                     func_index;   /* the per-map index, u8 at item+2 */
    q2_uf_prim             prim;
    const q2_uf_prim_info *info;         /* NULL when prim is unknown       */
    const u8              *item;         /* item start, for operand reads   */
    u8                     item_len;
} q2_uf_call;

/*
 * Resolve a CALL item against a map's UserFuncs chunk.
 *
 * Returns Q2_ERR_RANGE when func_index is past the chunk's count — which is
 * what the engine silently ignores, so a caller that wants engine behaviour
 * should treat it as "skip this item", not as a corrupt file. Returns
 * Q2_ERR_BAD_FORMAT when the item's length disagrees with the primitive's one
 * legal length: the engine's own handler bails in that case, and no item on
 * this disc does it.
 */
q2_result q2_uf_decode_call(q2_uf_call *out, const q2_userfuncs *uf,
                            const q2_event_item *item);

/*
 * Typed operand reads. Each returns false when the operand does not exist,
 * has the wrong type, or would read past the item — never a partial value.
 *
 * `element` selects within an array operand (OBJSLOT arrays and the VEC3s);
 * pass 0 for scalars.
 */
bool q2_uf_operand_u32(const q2_uf_call *c, u32 op, u32 element, u32 *out);
bool q2_uf_operand_s32(const q2_uf_call *c, u32 op, u32 element, s32 *out);

/* 12-byte key operand. `out` must hold Q2_UF_NAME_LEN + 1 bytes. */
bool q2_uf_operand_name(const q2_uf_call *c, u32 op, char *out);

/* Three absolute world coordinates. CONFIRMED absolute, not zone-rebased: all
 * 253 cross-zone deltas measured in LASERBEAM and SIMPLESOUND are +/-1, which
 * is nothing but the authoring-grid quantisation. */
bool q2_uf_operand_vec3(const q2_uf_call *c, u32 op, s32 out[3]);

/*
 * An OBJSLOT element, as it sits in the file. -1 means empty. Whether the value
 * is a Scene node index or a runtime object index depends on whether the
 * load-time constructor has run over this buffer, which this module cannot
 * know — hence the name.
 */
bool q2_uf_operand_slot_raw(const q2_uf_call *c, u32 op, u32 element, s16 *out);

/* ------------------------------------------------------------------------- */
/* WHERE AN OPERAND IS ACTUALLY READ FROM — the two-buffer rebase             */
/* ------------------------------------------------------------------------- */
/*
 * `0x800285CC` sets up two cursors over the Events bytes and they are not the
 * same chunk: it STAMPS -1 into the working copy at `gp+372` and READS the
 * operand from the same offset in the pristine copy at `gp+376`. So an operand
 * belonging to a record the game has already run is -1 wherever a port looks
 * for it, and the value it wanted is sitting in the other buffer.
 *
 * This lived as a static inside rotator.c until the breakables needed exactly
 * the same rebase — GLASS and SHOOTTHEN carry a single object slot at `+4`
 * where the rotators carry four at `+12`, and 4 of the disc's 10 breakable
 * calls are only reachable through it. A second copy of the arithmetic would
 * rot apart from the first, which is the same argument the rotator's own
 * operand offsets are kept in one place for.
 *
 * `base_b` NULL, or an offset that does not fit in it, reads `p` in place.
 */
typedef struct q2_uf_operands {
    const u8 *base_a;      /* the chunk the caller is parsing   */
    const u8 *base_b;      /* the chunk the engine reads from   */
    u32       b_size;
} q2_uf_operands;

const u8 *q2_uf_operand_at(const q2_uf_operands *src, const u8 *p, u32 need);

/* ------------------------------------------------------------------------- */
/* MISEVENT — the mission events, and where their names live                  */
/* ------------------------------------------------------------------------- */
/*
 * `MISEVENT` names a 12-byte key and the port resolved 0 of 93 of them against
 * the Strings chunk, which is why its namespace was recorded as UNLOCATED. It
 * was the wrong chunk. It is not a chunk at all.
 *
 * The exec at 0x8002BA1C gathers item +4..+15 BYTE BY BYTE into three words —
 * an unaligned 12-byte load, not three numbers — and hands them to 0x800419A0,
 * which searches a list with the 12-byte compare at 0x8006DB10:
 *
 *     800419FC  lw   s4, 17480(gp)        ; pass 0: the first namespace
 *     80041A08  lw   s4, 12516(v0)        ; pass 1: [0x800B30E4], the level's
 *     80041A1C  jal  0x8006DB10           ; (key, list, stride 16)
 *     80041AD8  lw   v0, 12(s2)           ; the found record's +12...
 *     80041AE8  jalr v0                   ; ...IS A HANDLER, and it is called
 *
 * So the namespace is a table of `name[12] + handler`, sixteen bytes a record,
 * NUL-terminated — the UserFuncs binding table's shape with one pointer instead
 * of two. And the executable carries one, at 0x8009B680, three records long,
 * sitting immediately before the UserFuncs table itself:
 *
 *     Pump1On      0x80024134
 *     Pump2On      0x80024170
 *     CheckPumps   0x800238AC
 *     <zero>                          -- 0x8006DB10 stops on a zero first word
 *
 * which is WASTE3's coolant pumps, and it pairs exactly with the line #74 found
 * in that map's own Strings: `Find and activate both coolant pumps.`
 *
 * The second namespace — `[0x800B30E4]`, zeroed at teardown by 0x8007075C — is
 * the level's own, and no code in the executable ever stores a non-zero into
 * it, so it is filled by a module rather than by the engine. That is the same
 * shape as the LevelBin group selector (#87) and is where the rest of a map's
 * mission events must live.
 */
typedef struct q2_misevent {
    const char *name;       /* the 12-byte key, NUL-padded on disc */
    u32         handler;    /* the record's +12: what running it calls */
} q2_misevent;

#define Q2_MISEVENT_COUNT 3

/* The executable's own table, 0x8009B680. */
const q2_misevent *q2_misevent_table(void);

/* Which entry `name` selects, or NULL. The compare is 12 bytes, not a strcmp:
 * 0x8006DB10 reads three words and a name that fills the field has no
 * terminator to find. */
const q2_misevent *q2_misevent_find(const char *name);

/* ------------------------------------------------------------------------- */
/* Constants a port needs to reproduce behaviour, all read out of the EXE.    */
/* ------------------------------------------------------------------------- */

/* Entity environment flags, OR-ed into entity+0x98 by the predicate family. */
enum {
    Q2_ENV_INWATER     = 0x00000004,
    Q2_ENV_UNDERWATER  = 0x00000100,
    Q2_ENV_INCROUCH    = 0x00000200,
    Q2_ENV_INLOWCROUCH = 0x00000400,
    Q2_ENV_INLAVA      = 0x00001000,
    Q2_ENV_DONTJUMP    = 0x00020000
};

/*
 * T_Damage (0x80057D54) takes five arguments and no hidden middle ones: the
 * frame is -96 and the only stack read is `lw $s6,112($sp)`, i.e. argument 5.
 *
 * ENGINE BUG worth knowing about: opcode 0x13 sets up no fifth argument at all
 * (it loads only a2 and a3 at 0x80027840), so it hands T_Damage an
 * uninitialised stack slot. A port must decide whether to mirror that — the
 * original's behaviour there depends on whatever the caller left on the stack —
 * or to pass a defined value. Mirroring it faithfully is not reproducible;
 * guarding it is a deliberate divergence. Either way it should be a choice, not
 * an accident.
 */

/* Means-of-death codes passed to T_Damage (0x80057D54) by the CALL family.
 * The full set indexes two tables in the EXE — a 16-entry armour-absorption
 * table at 0x800ACE1C (mod - 1) and a 21-entry hit-effect table at 0x800ACE5C
 * — so the space is at least 21 wide; only three are reachable from a script:
 * 9 (acid), 10 (lava) and 11 (laser).
 *
 * The canonical list now lives in `src/game/combat.h`, which carries all
 * twenty-one along with what each one does to armour and to knockback. They
 * are deliberately not redeclared here, so the two cannot drift apart. */

/* Scaling the movers apply to authoring units: obj+0x4C and obj+0x4E are set
 * from a u8 operand times 300, with 0xFF meaning 0xFFFF ("never"). DISH and
 * B3ROCKS instead set obj+0x4E to (global clock at 0x800AEBAC) + 300, which is
 * how we know 300 is one authoring unit of time and not a velocity scale. */
#define Q2_UF_TIME_UNIT   300
#define Q2_UF_TIME_NEVER  0xFFFF

/* SETWIBBLE writes a 4-bit field into Scene.flags08 bits 10..13. That accounts
 * for the on-disc flags08 values 0x400/0x800/0x1000/0x1400 the Scene decoder
 * records as unknown. */
#define Q2_SCENE_WIBBLE_SHIFT 10
#define Q2_SCENE_WIBBLE_MASK  0x3C00

#endif /* Q2PSX_USERFUNCS_H */
