/*
 * monster.h — the creature entity model and the animation frame driver.
 *
 * The PSX AI is a port of PC Quake II's monster code, and this is no longer an
 * inference: the whole framework has been read out of the executable and the
 * numbers land on id's own constants once the world scale is applied. Every
 * address quoted below is in `SLES_015.34`.
 *
 * ---------------------------------------------------------------------------
 * How a creature is driven
 * ---------------------------------------------------------------------------
 * `monster_start_go` (0x80061BA4) points the entity's think at
 * `M_MoveFrame` (0x8006175C) and sets `nextthink = level.time + 1`. From then
 * on the engine calls it once per AI tick, and it does three things:
 *
 *   1. advance the animation by one frame within the current move,
 *   2. run the frame's movement verb with the frame's distance,
 *   3. run the frame's think index against the creature's own method table.
 *
 * There is no separate `monster_think` wrapper — the PSX put the ground and
 * water checks in the generic entity physics, so the think IS the frame driver.
 *
 * ---------------------------------------------------------------------------
 * The method table, which is what makes a creature a creature
 * ---------------------------------------------------------------------------
 * A creature module does not export named callbacks. It registers handlers
 * into a two-level table built by 0x80061D10:
 *
 *     void (*classMethods[256][32])(edict_t *)      base 0x800D519C
 *
 * Every one of the 256 class slots starts out pointing at one shared 32-entry
 * table (0x800D559C) filled with a do-nothing handler (0x80062624), so an
 * unregistered class is inert rather than a crash. A module claims a slot by
 * calling the setter at 0x80061DE4 — which the loader hands it as import
 * +0x118 — once per method it implements.
 *
 * The 32 slots split in two at index 26:
 *
 *     [0..25]   `think` handlers, selected by an animation frame's think byte
 *     [26..31]  the creature's OWN movement verbs, selected by a frame whose
 *               ai byte has bit 7 set: index = 26 + (ai & 0x7F)
 *
 * That is why the frame census finds `ai` in {0..5} and {129..133} and `think`
 * in 0..19: two disjoint ranges into one table. A local verb is called with
 * only the entity — it gets no distance, because a creature that steers itself
 * does not want the frame's.
 *
 * ---------------------------------------------------------------------------
 * Animation
 * ---------------------------------------------------------------------------
 * A move is a frame range plus a per-frame script and an optional end callback:
 *
 *     q2_mmove   16 bytes  { s32 first; s32 last; frames*; endfunc }
 *     q2_mframe   3 bytes  { u8 ai; s8 dist; u8 think }
 *
 * The three-byte frame is worth noticing: it is not padded to four. A reader
 * that assumes alignment walks off the end of every animation. 0x80061890
 * computes the frame address as `frames + index*3` with an explicit
 * shift-and-add, which is the proof.
 *
 * Distance travelled on a frame (0x80061930..0x8006196C):
 *
 *     dist = (aiflags & AI_HOLD_FRAME) ? 0 : (dist * scale * 12) / 10
 *
 * The /10 is a magic-number division by 10 in the original, not a shift.
 *
 * ---------------------------------------------------------------------------
 * Scale, angles and axes — three things a port gets wrong silently
 * ---------------------------------------------------------------------------
 * **World scale is 12, not 10.** Every length constant in the AI is id's own
 * number times twelve, and they were read independently: melee 1020, near 6000
 * (id 500), mid 12000 (id 1000), step height 216 (id 18), chase-direction
 * deadband 120 (id 10), flyer height bands 480 and 360 (id 40 and 30), the
 * corner-peek sidestep 192 (id 16). Six unrelated constants agreeing on one
 * factor is not a coincidence. Note this is the AI's scale; other subsystems
 * were established at 10, so the two are genuinely different and neither is a
 * mistake in the other's reading.
 *
 * **Angles are 4096 to the turn.** `anglemod` (0x8005C7B8) is one instruction:
 * `and 0xFFF`. 180 degrees is 2048 and 90 is 1024, and the sidestep in
 * ai_run_slide is literally `± 1024`.
 *
 * **The vertical axis is Y, and it points DOWN.** Movement is built as
 * (sin, 0, cos) so the ground plane is XZ, yaw is atan2(x, z), and the height
 * comparisons in SV_movestep have exactly id's four branches with their signs
 * mirrored. A port that assumes Z-up gets monsters that will not climb steps.
 *
 * ---------------------------------------------------------------------------
 * The clock
 * ---------------------------------------------------------------------------
 * 10 ticks per second. Established arithmetically before and confirmed by the
 * AI: `idle_time = level.time + 150 + random()*150` where id waits 15 + 15
 * seconds, `search_time = level.time + 50` where id waits 5, the dead-monster
 * pause of 1e9 where id uses 1e8. This is NOT the 25 Hz simulation tick.
 */
#ifndef Q2PSX_MONSTER_H
#define Q2PSX_MONSTER_H

#include "q2psx.h"
#include "cremotion.h"

/* The AI clock: 10 ticks per second. */
#define Q2_AI_HZ 10
#define Q2_AI_SECONDS(s) ((s) * Q2_AI_HZ)

/*
 * One AI tick in the engine's dt units: 300 to the second (Q2_DT_HZ,
 * worldscale.h) over the ten ticks above. Whole-AI-tick callers use this
 * interval; the physical actor and corpse handlers accept the actual frame dt.
 * It is also Q2_DT_MAX, the console's own long-frame clamp (0x800184B8), so
 * no dt the console could have passed is larger.
 */
#define Q2_AI_TICK_DT 30

/* The AI's world scale. id's units times this gives the console's. */
#define Q2_AI_SCALE 12
#define Q2_AI_UNITS(pc) ((pc) * Q2_AI_SCALE)

/* ------------------------------------------------------------------------- */
/* Animation records                                                          */
/* ------------------------------------------------------------------------- */
#define Q2_MFRAME_SIZE 3        /* NOT 4 — the record is unpadded */
#define Q2_MMOVE_SIZE 16

/* ai >= this indexes the creature's own verb table rather than the shared one */
#define Q2_AI_LOCAL_FLAG 0x80

/* Where the local verbs start in a class's 32-entry method table. Read from
 * the `lw v0, 104(v0)` at 0x800618D8 — 104 bytes is 26 pointers. */
#define Q2_CLASS_METHOD_COUNT 32
#define Q2_CLASS_VERB_BASE    26
#define Q2_CLASS_COUNT        256

typedef struct q2_mframe {
    u8 ai;
    s8 dist;
    u8 think;
} q2_mframe;

struct q2_monster;
struct q2_cre_bind;

/* A move's end callback. Returning is enough; it may install a new move. */
typedef void (*q2_endfunc)(struct q2_monster *m);

typedef struct q2_mmove {
    s32 first_frame;
    s32 last_frame;
    const q2_mframe *frames;
    q2_endfunc endfunc;         /* NULL when the move just loops */

    /* Kept so a move decoded from a module image can still be traced back. */
    u32 image_offset;
    u32 frames_offset;
    u32 endfunc_offset;
} q2_mmove;

/* The six shared movement verbs, in the executable's own table order
 * (0x800D561C, filled at 0x80061D98). Slot 0 is null, and there is
 * deliberately no turn verb — a precise negative, since the PC lineage has one
 * and its absence would otherwise look like an oversight. */
typedef enum q2_ai_verb {
    Q2_AI_NONE = 0,
    Q2_AI_STAND,        /* 0x8005CDA8 */
    Q2_AI_WALK,         /* 0x8005ED6C */
    Q2_AI_RUN,          /* 0x8005E350 */
    Q2_AI_CHARGE,       /* 0x8005EE84 */
    Q2_AI_MOVE,         /* 0x8005ED44 */
    Q2_AI_VERB_COUNT
} q2_ai_verb;

extern const char *const q2_ai_verb_names[Q2_AI_VERB_COUNT];

/* ------------------------------------------------------------------------- */
/* aiflags — entity+0xDC. Values are PC Quake II's, confirmed one by one       */
/* against the branches that test them.                                       */
/* ------------------------------------------------------------------------- */
enum {
    Q2_AI_STAND_GROUND      = 0x00000001,  /* ai_stand   0x8005CDD8 */
    Q2_AI_TEMP_STAND_GROUND = 0x00000002,  /* ai_stand   0x8005CE5C */
    Q2_AI_SOUND_TARGET      = 0x00000004,  /* ai_run     0x8005E398 */
    Q2_AI_LOST_SIGHT        = 0x00000008,  /* ai_run     0x8005E638 */
    Q2_AI_PURSUIT_LAST_SEEN = 0x00000010,  /* ai_run     0x8005E6C0 */
    Q2_AI_PURSUE_NEXT       = 0x00000020,  /* ai_run     0x8005E660 */
    Q2_AI_PURSUE_TEMP       = 0x00000040,  /* ai_run     0x8005E688 */
    Q2_AI_HOLD_FRAME        = 0x00000080,  /* M_MoveFrame 0x8006184C */
    Q2_AI_GOOD_GUY          = 0x00000100,  /* FindTarget 0x8005D354 */
    Q2_AI_BRUTAL            = 0x00000200,  /* checkattack 0x8005DDE8 */
    Q2_AI_NOSTEP            = 0x00000400,  /* SV_movestep 0x8005FF0C */
    Q2_AI_DUCKED            = 0x00000800,
    Q2_AI_COMBAT_POINT      = 0x00001000,  /* ai_run     0x8005E390 */
    Q2_AI_MEDIC             = 0x00002000,  /* checkattack 0x8005DDE0 */
    Q2_AI_RESURRECTING      = 0x00004000,
    /* Not in vanilla 3.20's header but plainly present here: while it is set,
     * ai_charge and ai_stand leave the facing alone. 0x8005EEF4. */
    Q2_AI_MANUAL_STEERING   = 0x00010000
};

/* ------------------------------------------------------------------------- */
/* Entity flags — entity+0x20, a u16. id's FL_* values exactly.               */
/* ------------------------------------------------------------------------- */
enum {
    Q2_FL_FLY            = 0x0001,
    Q2_FL_SWIM           = 0x0002,
    Q2_FL_IMMUNE_LASER   = 0x0004,
    Q2_FL_INWATER        = 0x0008,
    Q2_FL_GODMODE        = 0x0010,
    Q2_FL_NOTARGET       = 0x0020,  /* FindTarget 0x8005D50C */
    Q2_FL_IMMUNE_SLIME   = 0x0040,
    Q2_FL_IMMUNE_LAVA    = 0x0080,
    Q2_FL_PARTIALGROUND  = 0x0100,  /* SV_NewChaseDir 0x800607C8 */
    /*
     * Both of these are read out of T_Damage rather than assumed from id's
     * header, and both land on id's own bit anyway: 0x8006291C tests 0x800 and
     * forces knockback to zero, and 0x80062994 SETS it on anything that dies
     * with SVF_MONSTER or a client block. 0x200 and 0x400 sit between and have
     * no reader found, so they stay unnamed.
     */
    Q2_FL_NO_KNOCKBACK   = 0x0800   /* T_Damage 0x8006291C, 0x80062994 */
};

/* spawnflags — entity+0x1C. Only the three the game layer reads are named. */
#define Q2_SPAWNFLAG_AMBUSH  0x00040000   /* bit 18, FindTarget 0x8005D3D4 */
/*
 * BIT 26 — "this creature leaves something behind when it dies".
 *
 * Read as bit 0x100 of the Population record's flags halfword, which is what
 * `monster_death_use` actually tests: 0x80062328 `lhu v0, 242(a0)` on the LINK
 * object, 0x80062330 `andi v0, v0, 0x100`, 0x80062334 `beq` skips the drop.
 *
 * obj+0xF2 IS the record's flags field — 0x800566DC `lhu v0, 20(s1)` /
 * 0x800566E4 `sh v0, 242(s0)` copies spawn record +20 straight into it — and
 * 0x8007E618..0x8007E62C republishes the same nine bits into spawnflags:
 * `spawnflags = (spawnflags & 0xF803FFFF) | ((obj[0xF2] & 0x1FF) << 18)`.
 * 0x100 << 18 is 0x04000000, so the record bit and this spawnflag are one bit
 * seen from two places.
 */
#define Q2_SPAWNFLAG_DROP_ITEM 0x04000000 /* bit 26, tested at 0x80062330   */
#define Q2_SVFLAG_INUSE      0x20000000   /* bit 29, tested everywhere      */

/* svflags — entity+0x40. */
/*
 * Bit 1 is id's SVF_DEADMONSTER, and it went unnamed here for as long as
 * nothing wrote it. Five of the seven transcribed modules do — every `*_dead`
 * and every gib arm sets it, and `q2_M_MoveFrame` was already TESTING the bare
 * literal 2. A body with it set stops being solid to other monsters, which is
 * how a corpse in a doorway does not block the creature behind it.
 */
#define Q2_SVF_DEADMONSTER   0x00000002
#define Q2_SVF_MONSTER       0x00000004   /* FindTarget 0x8005D4D0 */

/* ------------------------------------------------------------------------- */
/*
 * entity+0x20 IS A PACKED WORD, not a `u16 flags`
 * ------------------------------------------------------------------------- *
 * Every one of the seven creature modules writes it as a bitfield, and the
 * layout came out the same from seven independent reads:
 *
 *     bits  0..15   flags       FL_* — what this header already names
 *     bits 16..17   solid       SOLID_BBOX 2 at spawn
 *     bits 18..21   movetype    MOVETYPE_STEP 5 at spawn, MOVETYPE_TOSS 7 dead
 *     bits 22..23   deadflag    DEAD_DEAD 2, which every `die` tests to refuse
 *                               a second entry
 *
 * and entity+0x1C carries one more above the SVFLAG_INUSE bit this header
 * already names:
 *
 *     bits 30..31   takedamage  DAMAGE_YES 1, DAMAGE_AIM 2
 *
 * Six values, and all six are id's own enumerants — SOLID_BBOX 2,
 * MOVETYPE_STEP 5, MOVETYPE_TOSS 7, DEAD_DEAD 2, DAMAGE_YES 1, DAMAGE_AIM 2.
 * Nothing in the reads was arranged to produce that; the bit POSITIONS were
 * found first, from the shift-and-mask each module uses, and the values fell
 * out afterwards. `T_Damage` reads two of them independently (0x800629D8 for
 * deadflag, 0x80062A34 for movetype) and agrees.
 */
#define Q2_SOLID_SHIFT       16
#define Q2_SOLID_MASK        0x3u
#define Q2_MOVETYPE_SHIFT    18
#define Q2_MOVETYPE_MASK     0xFu
#define Q2_DEADFLAG_SHIFT    22
#define Q2_DEADFLAG_MASK     0x3u
#define Q2_TAKEDAMAGE_SHIFT  30
#define Q2_TAKEDAMAGE_MASK   0x3u

enum { Q2_SOLID_NOT = 0, Q2_SOLID_TRIGGER = 1, Q2_SOLID_BBOX = 2,
       Q2_SOLID_BSP = 3 };
enum { Q2_MOVETYPE_NONE = 0, Q2_MOVETYPE_NOCLIP = 1, Q2_MOVETYPE_PUSH = 2,
       Q2_MOVETYPE_STOP = 3, Q2_MOVETYPE_WALK = 4, Q2_MOVETYPE_STEP = 5,
       Q2_MOVETYPE_FLY = 6, Q2_MOVETYPE_TOSS = 7 };
enum { Q2_DEAD_NO = 0, Q2_DEAD_DYING = 1, Q2_DEAD_DEAD = 2,
       Q2_DEAD_RESPAWNABLE = 3 };
enum { Q2_DAMAGE_NO = 0, Q2_DAMAGE_YES = 1, Q2_DAMAGE_AIM = 2 };

/* attack_state, using the PC enum's values. Stored as a 3-bit field at bits
 * 18..20 of the word at entity+0x138 (0x8005E254). */
typedef enum q2_attack_state {
    Q2_AS_NONE     = 0,
    Q2_AS_STRAIGHT = 1,
    Q2_AS_SLIDING  = 2,
    Q2_AS_MELEE    = 3,
    Q2_AS_MISSILE  = 4
} q2_attack_state;

/* Content masks, read from the trace call sites. Both are id's own values. */
#define Q2_MASK_PLAYERSOLID   0x02010003u   /* ai_run corner peek 0x8005E808 */
#define Q2_MASK_MONSTERSOLID  0x02020003u   /* SV_movestep        0x8005FE7C */

/*
 * The bit both of them share, and the only one the port reads: it is what
 * turns the ENTITY clip on. A trace whose mask carries it is clipped against
 * the entity list at 0x800544EC after the hull walk, which is how a creature
 * is stopped by a door rather than walking through it. Both AI masks set it,
 * so in practice every AI trace asks for entities — the gate exists so a
 * caller that does not is still answered correctly.
 */
#define Q2_MASK_ENTITY_BIT    0x02000000u

/* ------------------------------------------------------------------------- */
/* Range bands                                                                */
/* ------------------------------------------------------------------------- */
/*
 * Compared against SQUARED distance, which is why the executable's immediates
 * are one less than a perfect square: the compiler turned `d2 < k*k` into
 * `k*k-1 < d2`. Read at 0x8005D56C and again, identically, at 0x8005E19C.
 */
#define Q2_MELEE_DISTANCE       1020   /* 0x000FE00F + 1 = 1020^2 */
#define Q2_MELEE_DISTANCE_BIG   2040   /* 0x003F803F + 1 = 2040^2 */
#define Q2_RANGE_NEAR_DIST      6000   /* 0x022550FF + 1 = 6000^2, id's 500 */
#define Q2_RANGE_MID_DIST      12000   /* 0x089543FF + 1 = 12000^2, id's 1000 */

/* The one class that gets the doubled melee reach. Read as an equality against
 * the class id byte at entity+0x23. */
#define Q2_CLASS_LONG_MELEE      68

/* ------------------------------------------------------------------------- */
/* Class bytes — entity+0x23                                                  */
/* ------------------------------------------------------------------------- */
/*
 * The class byte is a THIRD numbering, distinct from the class-table row id
 * (18, 19, 20 for the three Soldiers) and from the module's own name. It is the
 * index into `classMethods[256]`, and it is what the engine compares when it
 * wants to know which creature it is holding.
 *
 * Where it comes from was not written down anywhere until now: each 48-byte
 * record of the class descriptor table at **0x800A3518** carries it at **+0x20**,
 * and the module loader copies it into the entity with `lbu v0, 0x20(s4)` /
 * `sb v0, 0x23(s0)` at 0x8007E660. Reading that column out gives the whole map,
 * and the seven bytes the disc's own modules register — Arachner 64, Berserk 70,
 * Gunner 79, Infantry 81, Soldier 87/88/89, Tankcomm 91, Insane 94 — all land on
 * it exactly, which is the check that says the column is the right one.
 *
 * Named here are only the bytes some engine path actually tests for.
 */
/*
 * 47 IS THE CORPSE. `0x8007F0F0` writes it into the actor's class field
 * (+0xD2) at detach and saves the creature's own class to +0xDA, so a body has
 * its own class for the rest of its life and can be told from the thing it was.
 */
#define Q2_CLASS_CORPSE          47

#define Q2_CLASS_JORG            82   /* health 3000 — id's monster_jorg      */
#define Q2_CLASS_RIDER           83   /* health 3000 — id's monster_makron    */
#define Q2_CLASS_MEDIC           84   /* health  310 — id's monster_medic     */
#define Q2_CLASS_BOSS1           90   /* health 1500 — id's monster_supertank */
#define Q2_CLASS_TANKCOMM        91   /* health  750 — id's monster_tank      */

/*
 * Which of the console's three start wrappers a creature was brought up
 * through. WALK is zero so a memset entity is a walker, which is what the
 * console's default is too — the walk wrapper is the one that sets no flag.
 */
typedef enum q2_cre_start_kind {
    Q2_CRE_START_WALK = 0,   /* 0x80062240 */
    Q2_CRE_START_FLY,        /* 0x8006229C, and it sets FL_FLY  */
    Q2_CRE_START_SWIM,       /* 0x80062268, and it sets FL_SWIM */
    Q2_CRE_START_SCRIPT      /* export 0 installs its own think; no start wrapper */
} q2_cre_start_kind;

typedef enum q2_range_band {
    Q2_RANGE_MELEE = 0,
    Q2_RANGE_NEAR,
    Q2_RANGE_MID,
    Q2_RANGE_FAR
} q2_range_band;

/* The forward-cone threshold: a dot product of 1230/4096, about 0.30, so the
 * cone is roughly 72 degrees either side. That is id's own 0.3 and is why
 * monsters notice you from well off to the side. 0x8005D76C. */
#define Q2_INFRONT_DOT 1230

/* ------------------------------------------------------------------------- */
/* A live creature                                                            */
/* ------------------------------------------------------------------------- */
/*
 * One structure serves for creatures and for whatever they are chasing, which
 * is how the original does it too — `enemy`, `goalentity` and `movetarget` are
 * all plain entity pointers and the code never asks what kind of thing it has.
 * A player is an entity with `client` set.
 *
 * Offsets in the comments are the executable's, so a field can be checked
 * against a disassembly without hunting for it.
 */
typedef struct q2_monster {
    /* Runtime incarnation, never a saved identity. Reinitializing a roster
     * slot (including a Medic revive) invalidates voices owned by the old body. */
    u32 audio_generation;
    u32 sound_duration; /* entity+0x110; zero selects the retail 9000 units */
    q2_cre_motion motion;
    /* --- placement ------------------------------------------------------- */
    s32 pos[3];             /* +0x00  origin; index 1 is up, and points DOWN */
    s32 velocity[3];
    s16 angles[3];          /* +0x10  yaw is angles[2] (+0x14)               */
    s16 mins[3];            /* the movement hull, from 0x8005CCF4            */
    s16 maxs[3];

    /*
     * THE HIT CYLINDER, AND IT IS A DIFFERENT VOLUME FROM THE HULL ABOVE.
     *
     * `SetupBoxDrop` (COLL.C 0x80050FA0) writes two independent boxes from two
     * independent arguments. Its `a2` pair — 286 / 572 at 0x800AECEC for every
     * creature — becomes the symmetric movement hull at obj+0x18..0x22, i.e.
     * `mins`/`maxs` above (0x80050FD4..0x80051022). Its FIFTH argument, the
     * word the caller leaves at sp+0x10, becomes obj+0x40 / obj+0x42 —
     * entity+0x94 and entity+0x96, because the collision object is cre+0x54 —
     * at 0x80051030..0x80051044, and nothing else ever writes them.
     *
     * The creature spawner fills that fifth argument from the creature's own
     * MODEL: 0x8007E50C `jal GetCreHitSize` (MODELS.C 0x8006DC1C) with the
     * creature in a1, and the result word is shuffled into the outgoing
     * argument slot at 0x8007E534..0x8007E544. GetCreHitSize is two facts:
     * with a model bound at cre+0x10 it returns the model header's `lhu +0x18`
     * and `lhu +0x1A` — q2_model_header's ext0 and ext1 — and with no model it
     * returns the literals 286 and 600 (0x8006DC6C..0x8006DC78). Six hundred,
     * not 572: the fallback is not the hull's pair.
     *
     * LineIntersectCreCylinder (COLL.C 0x800544EC) reads only this pair.
     * 0x8005474C `lh v1, 148(v0)` is the horizontal radius and
     * 0x80054830..0x80054840 builds the vertical slab as
     * [origin.y + 286 - height, origin.y + 286] — from the feet upward by the
     * model's own height. So a Soldier is shot at as radius 124, not 286, and
     * a Tank Commander reaches far above the 286 a fixed cylinder stops at.
     *
     * This port used to derive both numbers from `mins`/`maxs`, which are the
     * same ±286 for every creature in the game, so every creature was shot at
     * as one 286-radius, 572-tall cylinder. See q2_monster_bind_hit_size.
     */
    s16 hit_radius;         /* +0x94  GetCreHitSize's first halfword          */
    s16 hit_height;         /* +0x96  its second; the slab hangs off the feet */
    s16 live_hit_radius, live_hit_height; /* immutable model header, for revival */

    s16 view_height;        /* +0x4C  added to origin[1] by visible()        */

    /* --- identity -------------------------------------------------------- */
    u32 spawnflags;         /* +0x1C  bit 18 ambush, bit 29 in use           */
    u16 flags;              /* +0x20  FL_*, the low half of the packed word  */

    /*
     * The other three fields of that same word, and the one above spawnflags.
     * Kept as separate members rather than packed back into one, because the
     * port has no reason to reproduce the packing and every reason to make a
     * `movetype` read like a movetype. See the layout note above for where
     * each one lives on the console and why the values are id's.
     *
     * `deadflag` is the field the console's `die` handlers test to refuse a
     * second entry; this port's `dead` bool is the same guard and the two are
     * kept in step by `q2_monster_damage_reaction`.
     */
    u8  solid;              /* +0x20 bits 16..17 */
    u8  movetype;           /* +0x20 bits 18..21 */
    u8  deadflag;           /* +0x20 bits 22..23 */
    u8  takedamage;         /* +0x1C bits 30..31 */

    /*
     * +0x4E, and it was decoded per creature and then dropped on the floor.
     * `q2_creature` has carried `mass` since the module decoder was written —
     * Soldier 100, Infantry 200, Gunner 200, Berserk 250, Arachner 400,
     * Tankcomm 500, all id's own numbers — and there was nowhere on the live
     * entity to put it.
     */
    s16 mass;               /* +0x4E */

    /*
     * +0x3A. `hurt` is only its LOW BIT (the wounded half of a skin pair); the
     * base index is per creature, and a module can set the whole field —
     * the Tank Commander's second class byte spawns with `skinnum = 2`.
     */
    u8  skinnum;            /* +0x3A */
    u8  class_id;           /* +0x23  indexes the class method table         */
    u8  class_byte;         /* the local descriptor key, 64..94 for monsters */

    /*
     * THE CLASS TABLE ROW this creature was placed from — 18, 19, 20 for the
     * three Soldiers — kept because `class_id` does not survive the spawn.
     *
     * `q2_creature_spawn` overwrites `class_id` with the module's own local
     * descriptor key (crebind.c), and a module that registers several class
     * bytes against ONE shared method table then has no way back to the row.
     * The Soldier is exactly that case: its spawn function dispatches on the
     * class-table id at 0x80101604 — `lh v1, 0xD2(entity+0x24)`, branching on
     * 19 / 18 / 20 — and each arm writes a different skinnum and health. Losing
     * the row and reconstructing the variant from `class_byte` instead
     * permuted all three: see soldier_skin.
     */
    u8  pop_class_id;
    struct q2_cre_bind *cre_bind; /* owning module, including classes without methods */

    /*
     * WHICH START WRAPPER THIS CREATURE WENT THROUGH, and its model's ext2.
     *
     * The console has three of them and they are distinct entry points that
     * differ in exactly two things — the go-routine they park at entity+0x94
     * and a flags bit:
     *
     *   0x80062240 walkmonster_start  think 0x8006241C,  no flag
     *   0x8006229C flymonster_start   think 0x800624BC,  ori 0x1  (FL_FLY)
     *   0x80062268 swimmonster_start  think 0x8006255C,  ori 0x2  (FL_SWIM)
     *
     * The kind is per RECORD and not per module, which is why it lives on the
     * creature: the Insane picks between two of them at spawn on its own prone
     * bit (module+0x974, `moddisasm LAB 0x80100974`), taking import +0x100
     * (fly) for a crawler and +0xFC (walk) for an upright one. It is written by
     * q2_monster_walk_start / _fly_start / _swim_start, which stand for the
     * three wrappers, and read by q2_monster_start_go, which stands for the
     * go-routine the wrapper parked.
     *
     * `model_ext2` is the halfword at obj+0xF8, which the loader fills from the
     * creature's own model — 0x80056700 `jal 0x8006D100` / 0x80056710 `sh v0,
     * 248(s0)`, and 0x8006D100 is `lw v0, 16(a0); if (!v0) return 0; return
     * lh v0[+0x1C]`. walkmonster_go complements it into the eye height. Zero
     * means "nobody filled it in": see q2_monster_start_go.
     */
    u8  start_kind;         /* q2_cre_start_kind                             */
    s16 model_ext2;         /* +0xF8 on the link object                      */

    /*
     * WHICH POPULATION GROUP PLACED THIS CREATURE.
     *
     * A group is not spawned because it exists; it is spawned because a script
     * SELECTED it — `0x80056C60` takes a name and sets bit 0 of the group's
     * flags, and the spawn pass runs only the selected ones (population.h).
     * The flags word is zero on disc for all 222 groups of all 49 maps, so
     * nothing is standing there at load until something asks.
     *
     * `CREBATCH` is that ask, and 58 of the disc's 89 resolvable calls name a
     * group claiming NO zone — `ShotgunRoom`, `BerserkHide` — which is an
     * ambush waiting to be summoned. Keeping the group index on the creature is
     * what lets one be held back and then released.
     */
    u16 group;
    u32 population_slot;  /* authored slot, stable across zone module rosters */
    bool population_active; /* actually spawned here, even after being freed */
    u32 svflags;            /* +0x40  SVF_MONSTER and the freed bit          */
    bool client;            /* +0x3C  non-NULL for a player                  */

    /* --- health ---------------------------------------------------------- */
    s16 health;             /* the original keeps this in the link object    */
    s16 max_health;
    s16 gib_health;         /* negative; below this the body is destroyed    */

    /* --- animation ------------------------------------------------------- */
    s16 frame;              /* +0x38                                         */
    s32 next_think;         /* +0x90  on the 10 Hz AI clock                  */
    s16 render_scale;       /* actor+0xFC, 4096 normally; RiderStand shrinks it */
    bool script_pain_used;  /* RiderStand replaces pain with a no-op after one hit */

    /*
     * The pain handler's own clock, on the same 10 Hz tick as `next_think`.
     *
     * A monster shot twice in a frame must not restart its flinch twice, and
     * without this every hit of a burst re-entered the pain move from frame
     * zero — a soldier under machinegun fire never advanced past the first
     * pose and never got back to running. Three seconds is the interval the
     * class uses, which is 30 of these ticks.
     */
    s32 pain_debounce;

    /*
     * BLOODIED. `skinnum |= 1` at half health: every class ships its skins in
     * pairs, clean on the even index and wounded on the odd one, and the pain
     * handler sets the low bit rather than choosing a skin. Kept as a flag
     * because the base index is derived from the class row per creature
     * (soldier_skin), so the two cannot be folded into one stored number.
     */
    bool hurt;
    void (*think)(struct q2_monster *m);   /* +0x94 — M_MoveFrame when awake */
    void (*use)(struct q2_monster *m);     /* +0x9C — scripted creature activation */

    /* --- targeting ------------------------------------------------------- */
    struct q2_monster *enemy;        /* +0xBC                                */
    struct q2_monster *oldenemy;     /* +0xC0                                */
    struct q2_monster *goalentity;   /* +0x84                                */
    struct q2_monster *movetarget;   /* +0x88                                */

    /*
     * +0x48, and the ONE thing that writes it on a creature is the kill
     * bookkeeping: `Killed` sets `targ->owner = attacker` when the attacker's
     * class byte is 84 (0x80062A18), which is id's "medics won't heal monsters
     * that they kill themselves". Unreachable on this disc — the Medic has a
     * class-table row and no CreAI module ships for it — and transcribed
     * anyway, because a branch that cannot fire here is still the branch the
     * original has.
     */
    struct q2_monster *owner;        /* +0x48                                */
    s32 show_hostile;                /* +0xB0                                */
    s32 teleport_time;               /* +0xD0                                */
    s16 targetname;                  /* +0x18  script name id, 0 when none   */
    s16 target;                      /* +0x16                                */
    s16 combattarget;                /* +0x56                                */
    s16 yaw_speed;                   /* +0x8C  degrees per tick, 4096/turn   */
    s16 ideal_yaw;                   /* +0x8E                                */
    s32 blind_target[3];             /* +0x5C  kept when the blindfire bit is set */

    /* --- monsterinfo, entity+0xD8 onward --------------------------------- */
    const q2_mmove *currentmove;     /* +0xD8                                */
    u32 aiflags;                     /* +0xDC                                */

    void (*stand)(struct q2_monster *m);                       /* +0xE0 */
    void (*idle)(struct q2_monster *m);                        /* +0xE4 */
    void (*search)(struct q2_monster *m);                      /* +0xE8 */
    void (*walk)(struct q2_monster *m);                        /* +0xEC */
    void (*run)(struct q2_monster *m);                         /* +0xF0 */
    void (*dodge)(struct q2_monster *m, struct q2_monster *other, s32 eta);
                                                               /* +0xF4 */
    void (*attack)(struct q2_monster *m);                      /* +0xF8 */
    void (*melee)(struct q2_monster *m);                       /* +0xFC */
    void (*sight)(struct q2_monster *m, struct q2_monster *other);
                                                               /* +0x100 */
    bool (*checkattack)(struct q2_monster *m);                 /* +0x104 */

    /*
     * PAIN and DIE — entity+0xA0 and +0xA4, written by the Soldier's module at
     * 0x80101684 and 0x80101690.
     *
     * These had no home at all, so `crebind`'s install list skipped slots 11
     * and 12 and `soldier_pain` / `soldier_die` were dead code: damage reached
     * a creature through `q2_actor_to_monster`, which sets health and a bool
     * and calls nothing. No pain animation, no pain sound, no death sound, no
     * random choice among the death moves.
     *
     * The original passes T_Damage's attacker, inflictor, damage and point to
     * both.
     *
     * AND THE DAMAGE IS NOW PASSED, because four of the seven transcriptions
     * cannot work without it. This used to say "a `soldier_pain` that wanted
     * the damage to pick pain4 would need the argument added here first", and
     * that turned out to understate it:
     *
     *     Tankcomm  pain   `damage <= 10` returns outright, `damage <= 30`
     *                      adds a roll, and 30 / 60 pick among three moves
     *     Gunner    pain   three flinch animations split at 10 and 25
     *     Berserk   pain   `damage < 20 || random() < 0.5` gates the flinch
     *     Berserk   die    `damage >= 50` picks the long death
     *
     * Every one of those was reaching a sentinel and taking one arm forever.
     * The attacker, the inflictor and the point are still dropped: nothing
     * reconstructed reads them, and adding a parameter nothing consumes is how
     * a signature stops meaning anything.
     */
    void (*pain)(struct q2_monster *m, s16 damage);            /* +0xA0 */
    void (*die)(struct q2_monster *m, s16 damage);             /* +0xA4 */
    /*
     * +0x108, with its threshold at +0x140. Not in the PC lineage: M_ChangeYaw
     * and SV_StepDirection both call it INSTEAD of turning when the turn they
     * were about to make exceeds the threshold, so a creature that needs to
     * swing a long way plays an animation rather than spinning on the spot.
     * Zero threshold disables it, which is what most creatures use.
     */
    void (*bigturn)(struct q2_monster *m, s32 turn);           /* +0x108 */
    s32  bigturn_threshold;                                    /* +0x140 */

    s32 pausetime;          /* +0x10C */
    s32 attack_finished;    /* +0x110 */
    s32 saved_goal[3];      /* +0x114 */
    s32 search_time;        /* +0x120 */
    s32 trail_time;         /* +0x124 */
    s32 last_sighting[3];   /* +0x128 */
    s32 idle_time;          /* +0x134 */

    u16 nextframe;          /* +0x138 */
    u8  attack_state;       /* +0x13A bits 2..4                             */
    bool lefty;             /* +0x13A bit 5                                 */
    bool blindfire;         /* +0x13A bit 1                                 */
    u8  speed_scale;        /* +0x13B multiplies every frame's dist         */

    /* --- port bookkeeping ------------------------------------------------ */
    bool in_use;
    bool dead;
    /* An unselected Population record accepted by the resident geometry and
     * module/skill filters, eligible for one CREBATCH. Excluded and freed
     * creatures are not pending; a group-name suffix is not a residency test. */
    bool spawn_pending;
    bool spawn_initialized; /* export 0 ran when this record was activated */
    bool resume_pending;    /* session-only: restore runtime after zone wake */

    /*
     * Blown apart rather than killed. Damage past `gib_health` destroys the
     * body outright: no death animation, because there is nothing left to
     * animate. Separate from `dead` because a body already dead can still be
     * gibbed by a later explosion, and the two arms play different sounds.
     */
    bool gibbed;

    /*
     * DETACHED — the body is a corpse and no longer a creature. See
     * `q2_monster_corpse_detach`.
     */
    bool corpse;
    u8   corpse_was_class;      /* actor+0xDA: what it was before */

    /*
     * THE DAMAGE-EFFECT BYTES, entity+0x2F0..0x2F5, for the corpse gate.
     *
     * The hit arms them (0x800585A4..0x80058604) and the presentation pass
     * runs them down, and in this port both of those act on the combat actor
     * (q2_actor.effect, combat.h), which the corpse tick never sees. The gate
     * 0x8005B2A8 reads +0x2F2 and +0x2F0 off the body's own record, so the
     * record carries a copy: this one. Nothing in monster.c writes it. It is
     * the actor's bytes as of the last sync, so whoever copies the actor back
     * into the creature (q2_actor_to_monster) has to copy these too, all six,
     * overwriting.
     */
    u8   effect[6];

    /*
     * THE DISSOLVE — what 0x8005B2A8 did to this corpse. `dissolve_arm` is
     * which of the two handlers it put into +0x3C (q2_corpse_dissolve, below)
     * and `dissolve` is actor+0xF4, the halfword that handler drains. Both are
     * zero until the gate fires. See "The dissolve" in the corpse section.
     */
    u8   dissolve_arm;
    s16  dissolve;

    bool on_ground;
    s32  ground_height;     /* the floor the stand-in world puts under it    */
} q2_monster;

/* The registry is 38 slots, which is exactly Population's class_id range. */
#define Q2_MONSTER_CLASS_COUNT 38

/* ------------------------------------------------------------------------- */
/* Level state — 0x800E46D8 onward                                            */
/* ------------------------------------------------------------------------- */
/*
 * Nine consecutive words in the original, in this exact order, which is id's
 * own `level_locals_t` tail. FindTarget reads all of them (0x8005D3AC..
 * 0x8005D478) and that ordering is what identifies them.
 */
typedef struct q2_level {
    s32 framenum;                   /* 0x800E46D8 */
    s32 time;                       /* 0x800E46DC — the 10 Hz clock */
    q2_monster *sight_client;       /* 0x800E46E0 */
    q2_monster *sight_entity;       /* 0x800E46E4 */
    s32 sight_entity_framenum;      /* 0x800E46E8 */
    q2_monster *sound_entity;       /* 0x800E46EC */
    s32 sound_entity_framenum;      /* 0x800E46F0 */
    q2_monster *sound2_entity;      /* 0x800E46F4 */
    s32 sound2_entity_framenum;     /* 0x800E46F8 */

    /*
     * 0x800E46FC — `level.total_monsters`, and it is the counter's other half:
     * `monster_start` increments it for every creature it spawns that is not
     * AI_GOOD_GUY, which is the same exclusion `Killed` applies to the kill
     * count below. id keeps the pair in exactly this order.
     */
    s32 total_monsters;

    /*
     * 0x800E4700 — `level.killed_monsters`, incremented by `Killed` at
     * 0x80062A10 for any creature that is not a good guy.
     *
     * The MISSION screen's kill column used to be a scan for bodies with the
     * `dead` flag set, which is a reconstruction of the effect rather than of
     * the counter: it counts a good guy, it counts a creature killed twice
     * differently from the console, and it cannot count one that has been
     * removed. This is the number the original keeps.
     */
    s32 killed_monsters;
} q2_level;

extern q2_level q2_level_state;

void q2_level_reset(void);

void q2_monster_init(q2_monster *m);

/* True while the entity is a live participant: the original's
 * `flags & 0x20000000` test, which every AI path opens with. */
static inline bool q2_ent_inuse(const q2_monster *m)
{
    return m != NULL && m->in_use && (m->spawnflags & Q2_SVFLAG_INUSE) != 0;
}

/* Positional advance for one animation frame, with the speed scale and the
 * hold-frame flag applied. 0x80061930. */
s32 q2_monster_frame_dist(const q2_monster *m, const q2_mframe *frame);

/* Squared distance between two entities, and the band it falls in. */
s64 q2_monster_dist_sq(const q2_monster *m, const s32 target[3]);
q2_range_band q2_range(const q2_monster *self, const q2_monster *other);

/* True when `other` lies inside `self`'s forward cone. 0x8005D608. */
bool q2_infront(const q2_monster *self, const q2_monster *other);

/* Apply damage. Returns true when this killed the creature. */
bool q2_monster_damage(q2_monster *m, s16 amount);

/*
 * Skill, which the pain path gates on. Defined in cre_soldier.c and declared
 * again here because the damage reaction needs it and `crebind.h` — where it
 * otherwise lives — sits ABOVE this header and cannot be included from it.
 */
s32 q2_cre_skill(void);

/*
 * The tail of T_Damage, 0x80062940..0x80062B54 — everything the original does
 * AFTER the health subtraction, which is where a creature's reaction lives.
 *
 * `combat.c` already carries the arithmetic half (the takedamage gate, the
 * friendly-fire halving, the surprise bonus, godmode and the knockback
 * suppression). What had no port at all was this half: which handler is called,
 * with what guards, and what the world records about the death. Call it once
 * per creature per frame in which its health changed, with the health already
 * applied and `dead` not yet raised.
 *
 * `damage` is the amount that actually landed — zero is meaningful, because
 * the original still runs M_ReactToDamage for it and then skips the flinch.
 */
void q2_monster_damage_reaction(q2_monster *targ, q2_monster *attacker,
                                s16 damage);

/*
 * monster_death_use — 0x800622E8. Run once, from the kill path, before the
 * creature's own `die`.
 */
void q2_monster_death_use(q2_monster *self);

/* ------------------------------------------------------------------------- */
/* The death drop — 0x80020D60 record, 0x80020E24 flush, 0x80020680 pick      */
/* ------------------------------------------------------------------------- */
/*
 * A FLAGGED CREATURE LEAVES AMMO, HEALTH OR A WEAPON, and it is decided by a
 * table indexed on its POPULATION CLASS ROW, not by an `item` field.
 *
 * The chain, disassembled end to end:
 *
 *  (a) 0x800622E8 monster_death_use. 0x80062318 `lw a0, 36(a0)` takes the link
 *      object, 0x80062330 tests bit 0x100 of its +0xF2 (Q2_SPAWNFLAG_DROP_ITEM
 *      above) and 0x8006233C `jal 0x80020D60`. `xrefs 0x80020D60` gives exactly
 *      two JALs — 0x8003976C, the player-death drop, and this — so it is a real
 *      entry and not a label.
 *
 *  (b) 0x80020D60 only RECORDS; it spawns nothing. Four 88-byte slots run from
 *      0x800C6D70 to 0x800C6ED0 (0x80020D6C `addiu v1, a1, 352`, 0x80020D8C
 *      `addiu a1, a1, 88`; 352/88 = 4 exactly). It takes the first slot whose
 *      halfword at +0 is zero, and IF NONE IS FREE IT RETURNS HAVING DONE
 *      NOTHING — 0x80020D74 and 0x80020DA4 both branch to the `jr ra` at
 *      0x80020E1C. So a fifth creature dying in one frame drops nothing, and
 *      that silent overflow is reproduced here rather than papered over.
 *      Then slot[+0] = 1, slot[+2] = `lhu obj+0xD2` (the class row), slot[+4] =
 *      0 in single player, and 80 bytes of obj+0x54..0xA3 — the collision
 *      volume, which begins with the position — are copied into slot+8.
 *
 *  (c) 0x80020E24 runs once a frame from 0x80038FE0, walks the four slots and
 *      hands each used one to 0x80020680 (0x80020E5C).
 *
 *  (d) 0x80020680 clears slot[+0] and picks an item id through the 30-entry
 *      jump table at 0x800AB79C, indexed `(s16)(slot[+2] - 5)` and bounded by
 *      `sltiu v0, v1, 30` at 0x800206E8. See q2_monster_drop_item_for_class.
 *      A zero id stops there (0x800207D0 `beq s0, zero, 0x80020848`); anything
 *      else draws ONE rand() (0x80020820 `jal 0x80089E28`), keeps its low
 *      twelve bits (0x8002082C `andi a1, v0, 0xFFF`) and calls the spawner.
 *
 *  (e) 0x8002085C is the DROP spawner, and it is not the placed-item one.
 *      `xrefs 0x8002085C` gives exactly one caller, 0x80020840 above. It does
 *      not go through the placement routine 0x80050FA0 at all: the recorded
 *      position is copied straight to the new item's +0x54 (0x800209C4), with
 *      no 286 lift and no floor sweep, because it is already a live entity's
 *      origin. The twelve random bits are not a facing — the item's angles are
 *      zeroed (0x80020AC8) — they index the {sin, cos} table at 0x800A5430 as
 *      the HEADING OF A TOSS (0x80020A2C), and two more rand() draws inside the
 *      spawner give its speed and upward kick. Its think is 0x80020C48, which
 *      lets the item fall and settles it into the ordinary item think
 *      0x80059330 once it lands. What the port hands over is the REQUEST —
 *      q2_monster_drop_request, below — and the spawning is the host's.
 *
 * entity+0xD4, which in id's monster_death_use is `self->item`, is CLEARED at
 * 0x80062314 and the chain above never reads it: 0x80020D60 is handed the
 * link object and reads only its +0x0C, +0xD2 and +0x54..+0xA3. id's
 * `Drop_Item(self, self->item)` was replaced by the class-table path, so there
 * is no second drop route to be missing.
 */

/*
 * Which item id a creature of population class row `pop_class` drops, or 0 for
 * "nothing". `weapon_id` and `weapon_bits` are the player's, which two arms of
 * the table read; see q2_monster_set_player_weapon.
 *
 * Exposed rather than static because the whole point of the table is that it
 * was transcribed byte for byte and the test drives it: every row but 3, which
 * takes the same below-the-window path as rows 0, 1, 2 and 4 (`(s16)(3 - 5)`
 * fails `sltiu v0, v1, 30` at 0x800206E8), so a line for it would pin nothing
 * those four do not.
 */
s8 q2_monster_drop_item_for_class(u8 pop_class, s16 weapon_id, u32 weapon_bits);

/*
 * The player's current weapon and weapons bitmask, which 0x80020680 reads from
 * 0x800C7CC6 (`lh a1`) and 0x800C7CC8 (`lw a0`) at 0x8002069C/0x800206A0.
 * Handed in so monster.c does not have to reach into the sim.
 *
 * IN THE CONSOLE'S NUMBERING, which is not the port's inventory's. The weapon
 * id is ONE-based — Blaster 1 .. BFG 11, `q2psx-inspect weapons` — and the
 * Gunner arm's `(u32)(id - 4) < 2` is Machinegun 4 or Chaingun 5 only in that
 * numbering. The bitmask is `1 << (id - 1)`, which is the same bit
 * `q2_inventory.weapons` already keeps for the zero-based `q2_weapon`. So from
 * a q2_inventory the call is `(current_weapon + 1, weapons)`.
 */
void q2_monster_set_player_weapon(s16 weapon_id, u32 weapon_bits);

/*
 * ONE DROP, as 0x80020680 hands it to the spawner 0x8002085C.
 *
 *   a0  item_id   the table's pick, never 0 — a 0 spawns nothing and is not
 *                 handed over at all
 *   a1  heading   rand() & 0xFFF (0x8002082C), a 4096-step angle
 *   a3  pos       the first three words of the 80 bytes the record copied from
 *                 obj+0x54 at the moment of death — the OBJECT's position,
 *                 which the port records as q2_monster.pos, the origin, in the
 *                 frame the dropped item keeps. That the two agree is
 *                 INFERRED, to within a frame of object physics.
 *
 *                 link_entity is NOT what keeps them together. Its copy of
 *                 the edict origin into +0x54 (0x8005C91C..0x8005C93C) is
 *                 skipped when bit 7 of its flags is set (0x8005C904 `andi v0,
 *                 a1, 0x80` / 0x8005C908 `bne`), and every movement link in
 *                 M_walkmove sets it (0x81 and 0x85, aimove.c), so for a
 *                 creature that has moved that copy is almost never taken.
 *                 What keeps +0x54 current is the object's per-frame handler
 *                 0x8007EBC4 (materialised at 0x8007E5A8): it turns the gap
 *                 from +0x54 to +0x20 — where link_entity's bit-0 copy DOES
 *                 put the edict origin, 0x8005C8E0..0x8005C900 — into the
 *                 halfwords at +0xEC..+0xF0 (0x8007ED08..0x8007EDC4), runs the
 *                 object mover 0x8004583C (`jal` at 0x8007EE00), whose stores
 *                 at 0x80045C54, 0x80045C90 and 0x80045E14 are the writes to
 *                 +0x54, and copies +0x54 back over +0x20 at
 *                 0x8007EE48..0x8007EE5C. A mover that stops short of the
 *                 edict leaves +0x54 behind it; the port does not model that.
 *
 * The fifth argument, `lh 78(a3)` at 0x80020838, is obj+0xA2 — the cell the
 * creature's origin was last found in — and becomes the item's own +0xA2. The
 * port keeps no such field on a creature, so it is not carried; the host finds
 * the cell for `pos` itself. a2 points at six zero bytes (0x800207D8) that the
 * spawner never reads.
 */
typedef struct q2_monster_drop_request {
    u8  item_id;
    u16 heading;
    s32 pos[3];
} q2_monster_drop_request;

/* Where a resolved drop goes — one call per spawning slot, in slot order. */
void q2_monster_set_drop_hook(void (*fn)(const q2_monster_drop_request *req,
                                         void *user),
                              void *user);

/* 0x80020D60 — record one death. Silently does nothing when all four slots are
 * in use. Called from q2_monster_death_use; exposed for the test. */
void q2_monster_drop_record(const q2_monster *self);

/*
 * 0x80020E24 — one frame's worth of drops: every used slot is emptied and each
 * one whose class picks an item reaches the drop hook as a request. The
 * console calls it once a game frame, from the frame routine at 0x80038FE0,
 * so the four-slot cap is "four deaths between two flushes"; call it on the
 * same cadence and after the frame's damage has been dealt.
 */
void q2_monster_drop_flush(void);

/* Empty the queue without spawning anything. For level changes and tests. */
void q2_monster_drop_reset(void);

/* How many of the four slots are in use. For the test and the census. */
u32 q2_monster_drop_pending(void);

/*
 * The value standing in gp+276 (0x800AE714), which is where the Tank
 * Commander's drop actually comes from. Exposed because the whole finding is
 * that this is a STATIC and not a table lookup, and a test that could only see
 * the returned 23 could not tell the two apart.
 */
s32 q2_monster_drop_ammo_static(void);

/* ------------------------------------------------------------------------- */
/* Corpses — the detach at 0x8007F098 and the handler at 0x8007F71C           */
/* ------------------------------------------------------------------------- */
/*
 * A BODY STOPS BEING A CREATURE, and this port had no such step at all.
 *
 * The console keeps two linked structures: the ACTOR (health at +0x108,
 * gib_health at +0x44, collision volume at +0x54, per-tick handler at +0x3C,
 * entity at +0x2EC) and the ENTITY. When a creature's own `*_dead` raises
 * SVF_DEADMONSTER, `0x8007EC28` calls `0x8007F098` and the two come apart:
 *
 *     0x8007F0A8/B0  gib_health  is copied from entity+0x52 onto actor+0x44
 *     0x8007F0B4/BC  max_health  is copied from entity+0x50 onto actor+0x48
 *     0x8007F0C8     entity+0x24 = 0            — the entity loses its actor
 *     0x8007F0D0     entity+0x1C &= 0xDFFFFFFF  — bit 29, the edict is FREED
 *     0x8007F0DC     actor+0x2EC = 0            — the actor loses its entity
 *     0x8007F0E4     actor+0x3C  = 0x8007F71C   — the per-tick handler becomes
 *                                                 the CORPSE handler
 *     0x8007F0E0     the collision volume is rescaled (below)
 *     0x8007F0F4     actor+0xD2 = 47, with the old class saved to +0xDA
 *
 * (The port writes that 47 into `class_id`, the module's class byte, and saves
 * the byte in `corpse_was_class`; `pop_class_id`, its copy of +0xD2, keeps the
 * row. So the value the console saved at +0xDA is `pop_class_id` here, still
 * in place — which is what anything wanting the dead creature's class row
 * reads.)
 *
 * From that tick on there is no edict, which is why `movetype` is written on
 * death by every module and read by nothing: there is nothing left to read it
 * off. A console corpse does not fall because a console corpse is not an
 * entity any more.
 *
 * THE VOLUME RESCALE, 0x8007F77C — MAIN.SYM names it `QuakeDeadHitCollision`
 * (QENTS.C line 1810), and the name is the point. It is two divides and they
 * land on the HIT CYLINDER, not on the movement hull: `+0x42 /= 4`
 * (0x8007F794, an arithmetic shift with the usual +3 bias) and
 * `+0x40 = (3 * +0x40) / 2` (0x8007F79C..0x8007F7B4) on the collision object
 * it is handed at 0x8007F0E0 (a0 = cre+0x54), i.e. entity+0x96 and
 * entity+0x94 — `hit_height` and `hit_radius`. It never touches obj+0x18..0x22,
 * so a corpse keeps the ±286 movement hull it stood up in. So a body becomes
 * half again as wide and a quarter as tall as it was — id's
 * `self->maxs[2] = -8` generalised. That is what stops a corpse being shot at
 * at head height.
 *
 * This port used to apply those two divides to `mins`/`maxs` instead, because
 * that is where it derived the cylinder from; the hull was the wrong box and
 * the numbers it produced were the wrong numbers (143 tall rather than a
 * quarter of the model's own height).
 *
 * THE HANDLER, 0x8007F71C, makes four calls, in this order:
 *
 *     0x8007F728  jal 0x8005B2A8   the DISSOLVE GATE (below). If it answers
 *                                  non-zero, 0x8007F730 `bne v0, zero,
 *                                  0x8007F76C` returns at once: none of the
 *                                  other three runs on that tick.
 *     0x8007F738  jal 0x8005B880   the presentation chain — the ambient fade
 *                                  0x80075E14, the damage-effect hooks
 *                                  0x80058638/0x8005B6F4/0x8005B744/0x8005B794/
 *                                  0x8005B830 and the quad shell 0x8005B7E4.
 *                                  effect.h has it as q2_fx_actor_present. It
 *                                  advances no animation.
 *     0x8007F740  jal 0x800552B4   append the body to the ACTOR LIST: a
 *                                  32-entry buffer at gp+17824 that 0x8005525C
 *                                  swaps each frame into 0x800B2B90..
 *                                  0x800B2B98. The hitscan hands those two
 *                                  ends to the entity sweep 0x800544EC
 *                                  (0x80048908..0x8004891C), and radius
 *                                  damage walks them (0x80050884). It is what
 *                                  keeps a body SHOOTABLE. It is not a draw
 *                                  list.
 *     0x8007F764  jal 0x8007CEB4   the destruction dispatcher, taken when
 *                                  `slt v0, v0, v1` at 0x8007F754 (gib_health
 *                                  < health) is 0: health <= gib_health.
 *
 * So a settled corpse is still checked for gibbing every tick, against the
 * threshold it copied at detach, unless the gate has taken it first.
 *
 * THE DISSOLVE GATE, 0x8005B2A8. Both corpse handlers call it first: this one
 * at 0x8007F728, the player's 0x8003E238 at 0x8003E244 (playerdeath.h). `xrefs`
 * finds those two JALs and no other reference.
 *
 *     8005B2B4  lbu  v0, 754(a0)   effect[2] (+0x2F2) set: v0 = 0x8005B444
 *     8005B2D4  lbu  v0, 752(a0)   else effect[0] (+0x2F0) set: v0 = 0x8005B39C
 *                                  neither set: branch to 0x8005B2F8 and write
 *                                  nothing
 *     8005B2EC  sw   v0, 60(a0)    the handler goes into +0x3C
 *     8005B2F4  sh   v0, 244(a0)   +0xF4 = 4096 (0x8005B2F0 addiu)
 *     8005B304  jal  0x8007F288    on the +0x2EC record
 *     and it returns s0, 1 if either byte was set.
 *
 * The two handlers are the same 168 bytes (`q2psx-inspect bytes` of
 * 0x8005B39C and 0x8005B444, compared), so which one goes in records only
 * WHICH BYTE WAS SET. effect[2] is read first and wins when both are.
 * effect[1], [3], [4] and [5] do not open the gate. Each handler runs the
 * presentation chain (the seven calls of 0x8005B880, inlined, dt_alive
 * included) and then:
 *
 *     8005B404  lw   v1, [0x800B2DB4]   the frame's dt
 *     8005B410  sll  v1, v1, 6
 *     8005B418  sh   v0, 244(s1)       +0xF4 -= dt << 6, as a halfword
 *     8005B420  bgtz                   still positive: that is all
 *     8005B428  jal  0x8006D280(self)  otherwise the body is FREED
 *
 * There is no 0x800552B4 call and no gib test in either handler. From the
 * gate on, the body is off the actor list, so nothing can hit it and nothing
 * can gib it. It is still drawn, because the per-frame entity walk below
 * still takes it, and it still sparks: effect[0] and [2] tick by dt_alive,
 * which is 0 on a body, so they never run out. That lasts until
 * 4096 has drained at 64 per dt, which is 64 dt: six frames at the nominal
 * 12 (Q2_DT_NOMINAL). Then the body is gone. A corpse whose killing blow
 * armed effect[0] or [2] (Q2_MOD_2, Q2_MOD_4; combat.c q2_mod_effect_timer)
 * therefore dissolves on its first corpse tick. It does NOT gib, however far
 * past gib_health it is, because the gate comes before the gib test.
 *
 * +0xF4 IS NOT A DRAW PARAMETER. `q2psx-inspect access 0xF4` lists every load
 * and store with immediate offset 244 in the main executable (the relocated
 * modules are not scanned). The loads that run over every entity in a frame
 * test only zero or sign:
 *
 *     0x8006ABC0  `lh` / `beq zero`  the per-frame entity walk 0x8006AB8C (one
 *                                    caller, 0x80077420) skips a zero +0xF4,
 *                                    which is a freed slot: 0x8006D280 zeroes
 *                                    it at 0x8006D2D8. It skips an entity with
 *                                    no model at +0x10 too.
 *     0x8006DCC4  `lh` / `blez`      in 0x8006DC88, which that walk calls
 *     0x8006B038  `lh` / `bgez`      the light gather (0x8006B040, lighting.c)
 *
 * Nothing the presentation chain calls touches it. The census has no access
 * between 0x8004BF28 and 0x800597D4 (0x80058638, the mesh emitters, and the
 * effect[5] emitter 0x80059168..0x8005932C; 0x800597D4 is the item think
 * 0x80059330's), none between the drain at 0x8005B4C0 and 0x80064234
 * (0x8005B6F4..0x8005B880 and their emitters), and none between 0x80072EBC
 * and 0x80079AD4 (0x80075E14). In 0x8005B2A8..0x8005B4EC the only accesses
 * are the gate's store and the two drains. A dissolving body's +0xF4 is in
 * (0, 4096], so it takes the same arms it took before. Nothing fades,
 * shrinks or turns translucent. What the player sees is the body vanish when
 * the free lands.
 *
 * 0x8007F288 clears bit 29 (in use) of the +0x2EC record's +0x1C and zeroes
 * its +0x24: the edict free that the detach does inline at 0x8007F0C8..
 * 0x8007F0D0. On a creature corpse that pointer has been null since
 * 0x8007F0DC. Unlike 0x8007F12C, the null-checking wrapper that 0x8006D280
 * calls, the gate does not test it, so the call writes the two words at
 * physical 0x1C and 0x24. What, if anything, the BIOS keeps there is not
 * established. The port has no memory there and models none of it.
 *
 * THIS PORT. The gate reads `effect`, the record's copy of the actor's bytes
 * (above). The handler's state is `dissolve_arm` and `dissolve`. Leaving the
 * actor list is `takedamage = Q2_DAMAGE_NO`, because the port's sweeps skip a
 * body on that and not on list membership (combat.c, nearest_hit). The
 * presentation is not the corpse tick's in this port. simcombat.c presents
 * every creature actor every frame, and the mesh hook stops handing a posed
 * mesh over once `in_use` goes, so the sparks end with the free. One
 * difference: the console skips the chain on the gate's own tick, and the
 * port's pass does not.
 */
void q2_monster_corpse_detach(q2_monster *m);

/*
 * GetCreHitSize (MODELS.C 0x8006DC1C) as the spawner uses it, 0x8007E50C.
 *
 * `has_model` is `cre+0x10 != NULL`, the test at 0x8006DC24; `ext0` and `ext1`
 * are the model header's two halfwords at +0x18 and +0x1A, which
 * q2_model_header already parses (src/formats/model.h). With no model the pair
 * is 286 / 600, the literals at 0x8006DC6C and 0x8006DC74 — NOT the movement
 * hull's 286 / 572, which is a different constant in a different argument.
 *
 * Call it once when the creature's model is bound and before anything shoots
 * at it. A creature nobody calls it for keeps q2_monster_init's seed, which is
 * the same no-model fallback, so the worst an unbound creature gets is the
 * cylinder the console gives a creature with no model.
 */
void q2_monster_bind_hit_size(q2_monster *m, bool has_model,
                              s16 ext0, s16 ext1);

typedef enum q2_corpse_dissolve {
    Q2_CORPSE_DISSOLVE_NONE = 0,  /* the gate answered 0: the handler goes on */
    Q2_CORPSE_DISSOLVE_FX0,       /* effect[0] set: +0x3C = 0x8005B39C        */
    Q2_CORPSE_DISSOLVE_FX2        /* effect[2] set: +0x3C = 0x8005B444        */
} q2_corpse_dissolve;

/* +0xF4 as the gate leaves it (0x8005B2F0 `addiu v0, zero, 4096`) and the
 * shift the handlers apply to dt before taking it off (0x8005B410/0x8005B4B8
 * `sll v1, v1, 6`). */
#define Q2_CORPSE_DISSOLVE_LEVEL 4096
#define Q2_CORPSE_DISSOLVE_SHIFT 6

/*
 * 0x8005B2A8's choice: which handler a body with these damage-effect bytes
 * (entity+0x2F0.., six of them) is handed to. NONE means the gate answers 0
 * and writes nothing. NULL is a body with none set.
 */
q2_corpse_dissolve q2_corpse_dissolve_pick(const u8 *effect);

/*
 * One run of either handler's drain, 0x8005B404..0x8005B418: `level - (dt <<
 * 6)`, stored as a halfword. The handler frees the body when the result is
 * not positive (0x8005B41C `sll v0, v0, 16` / 0x8005B420 `bgtz`).
 */
s16 q2_corpse_dissolve_drain(s16 level, s32 dt);

/*
 * One tick of a corpse — 0x8007F71C, or the dissolve handler that has
 * replaced it. The _dt entry accepts the engine frame delta; the compatibility
 * wrapper spends Q2_AI_TICK_DT for whole-AI-tick callers.
 *
 * Returns true when the body was destroyed this tick, which is the caller's
 * cue to stop drawing it. That happens two ways, and on both the record is
 * FREED here: `in_use` goes false and `takedamage` goes to Q2_DAMAGE_NO. That
 * is this record's version of 0x8006D280, so the body is destroyed exactly
 * once, and a record that is not in use is not ticked at all.
 *
 *   - past gib_health, after q2_gib_monster_corpse (modelent.h; 0x8007F764 ->
 *     0x8007CEB4), for the dispatcher's own tail (0x8007D140 `jal
 *     0x8006D280`);
 *   - at the end of a dissolve, when the handler's level runs out
 *     (0x8005B428 `jal 0x8006D280`). Nothing is thrown.
 *
 * On the tick the gate fires it returns false: the body is still there.
 */
bool q2_monster_corpse_tick(q2_monster *m);
bool q2_monster_corpse_tick_dt(q2_monster *m, s32 dt);

/* ------------------------------------------------------------------------- */
/* The class method table                                                     */
/* ------------------------------------------------------------------------- */
/*
 * Two levels, exactly as 0x80061D10 builds it: a 256-entry table of pointers
 * to 32-entry method tables, every slot starting out at the shared inert one.
 * Registering a method clones the shared table for that class on first write,
 * which is the port's way of getting the same observable behaviour without the
 * original's fixed BSS arena.
 */
typedef void (*q2_class_method)(q2_monster *m);

void q2_class_table_reset(void);

/* The setter the loader hands a module as import +0x118 (0x80061DE4). */
void q2_class_method_set(u32 class_id, u32 index, q2_class_method fn);
q2_class_method q2_class_method_get(u32 class_id, u32 index);

/* Convenience for the two halves, so callers do not open-code the +26. */
void q2_class_think_set(u32 class_id, u32 think_index, q2_class_method fn);
void q2_class_verb_set(u32 class_id, u32 verb_index, q2_class_method fn);

/* ------------------------------------------------------------------------- */
/* The frame driver — M_MoveFrame, 0x8006175C                                 */
/* ------------------------------------------------------------------------- */
void q2_M_MoveFrame(q2_monster *m);

/* ------------------------------------------------------------------------- */
/* The three go-routines — 0x8006241C, 0x800624BC, 0x8006255C                 */
/* ------------------------------------------------------------------------- */
/*
 * EACH CREATURE'S EYE AND TURN RATE COME FROM ITS GO-ROUTINE, not from one
 * constant, and this port used to give every creature -290 and 200.
 *
 * All three do the same two things before falling into monster_start_go, and
 * differ only in the numbers:
 *
 *   walkmonster_go 0x8006241C
 *     0x8006242C  lh v0, 140(s0)          yaw_speed
 *     0x80062434  bne v0, zero, +2        \ the 228 is materialised in the
 *     0x80062438  addiu v0, zero, 228     / DELAY SLOT, so only the STORE is
 *     0x8006243C  sh v0, 140(s0)            conditional — id's
 *                                           `if (!yaw_speed) yaw_speed = 20`,
 *                                           and 228/4096 of a turn is 20.04
 *                                           degrees a tick.
 *     0x80062440  lw v0, 36(s0)           the link object
 *     0x80062448  lhu v0, 248(v0)         its model's ext2
 *     0x80062450  nor v0, zero, v0        view_height = ~ext2
 *     0x80062458  sh v0, 76(s0)           (in the jal's delay slot)
 *
 *   flymonster_go 0x800624BC  yaw_speed 114 (0x800624E8), view_height -250
 *                             (0x800624F0). 114/4096 is 10.02 degrees, id's 10.
 *   swimmonster_go 0x8006255C yaw_speed 114 (0x8006257C), view_height -100 —
 *                             and note 0x80062578 loads -100 in the delay slot
 *                             of the yaw branch and 0x80062584 loads it again
 *                             on the fall-through, so BOTH paths store -100.
 *
 * `q2psx-inspect access 0x8C` finds exactly three `sh` sites for entity+0x8C in
 * the whole executable and these are all three; nothing else writes it. That
 * could only be checked on the main executable, not on the seven relocated
 * module images, so "no module sets its own yaw_speed" is INFERRED — but the
 * `bne` exists precisely so that one could.
 *
 * The eye is `~(u16)ext2` stored through an `sh`, so it TRUNCATES: ext2 251
 * gives -252 and not -251, and ext2 0 gives -1 rather than 0. Measured off the
 * disc with `q2psx-inspect models`: Soldier 251, Tankcomm 507, Arachner 217,
 * Gunner 380, Insane 304 — so -252, -508, -218, -381, -305.
 */
void q2_monster_walk_start_go(q2_monster *m, s16 model_ext2);
void q2_monster_fly_start_go(q2_monster *m);
void q2_monster_swim_start_go(q2_monster *m);

/*
 * THE THREE START WRAPPERS — 0x80062240 walk, 0x8006229C fly, 0x80062268 swim.
 *
 * What a module's spawn function calls last, through import +0xFC, +0x100 and
 * +0x104 (the loader fills those three slots at 0x8007DCD4, 0x8007DCE0 and
 * 0x8007DCEC). Each does exactly two things before `jal 0x800619E0`
 * monster_start:
 *
 *   0x80062240  sw 0x8006241C, 148(a0)       parks the walk go-routine
 *   0x8006229C  sw 0x800624BC, 148(a0)       parks the fly go-routine
 *               lhu/ori 0x1/sh +0x20         FL_FLY, 0x800622B4, the `sh` in
 *                                            the jal's delay slot
 *   0x80062268  sw 0x8006255C, 148(a0)       parks the swim go-routine
 *               lhu/ori 0x2/sh +0x20         FL_SWIM, 0x80062280
 *
 * So the flag arrives AT SPAWN, from the wrapper, before the creature has ever
 * thought — not when it wakes. These record which go-routine was parked in
 * `start_kind` and raise the wrapper's flag.
 *
 * AND THEN THEY DRAW A RANDOM NUMBER, because the jal is not the end of them.
 * monster_start's last act (0x80061B1C..0x80061B8C) picks a random start frame
 * in the move the module installed: `if (currentmove) frame = first + rand()
 * % (last - first + 1)`, the rand() being `jal 0x80089E28` at 0x80061B2C —
 * the same BIOS routine as the module's import +0x14. So a wrapper call
 * consumes ONE draw whenever the module has set `currentmove`, and it
 * consumes it at the call site, in the middle of the module's own draws.
 * These three do that too (monster.c, monster_start_frame).
 *
 * The rest of what this port models of monster_start is not here but in
 * q2_creature_spawn (crebind.c): SVF_MONSTER and takedamage before the
 * module's spawn hook, and the total_monsters count after it, because the
 * count reads the AI_GOOD_GUY bit the module sets.
 *
 * A module that says nothing is a walker: the other six of the disc's seven
 * modules end their spawn with +0xFC (each transcription cites its own site —
 * the Soldier's is `lw v0, 252(s0)` at module+0x173C), and the Insane takes
 * +0xFC or +0x100 on its prone bit (cre_insane.c).
 *
 * ONLY THE INSANE CALLS THESE YET. The other six transcriptions do not call
 * q2_monster_walk_start at their +0xFC site, so on this port they get neither
 * the random start frame nor the draw that makes it: the shared stream is one
 * draw short for each of them whose module has a move installed by then (the
 * Soldier's own stand call at +0x1734 and the Arachner's `currentmove = Stand`
 * both come first). Each module's spawn has to be checked for draws either
 * side of its call before the call can simply be added; that is not done
 * here.
 */
void q2_monster_walk_start(q2_monster *m);
void q2_monster_fly_start(q2_monster *m);
void q2_monster_swim_start(q2_monster *m);

/*
 * monster_start_go, 0x80061BA4: wake a placed creature up.
 *
 * ON THE DISC THIS FUNCTION DOES NOT TOUCH yaw_speed OR view_height — its three
 * callers do, immediately before jumping in. `xrefs 0x80061BA4` returns exactly
 * three JALs and they are 0x80062454, 0x800624F4 and 0x8006258C, the tails of
 * the three go-routines above and nothing else. This port has ONE wake entry
 * rather than three, so the go-routine half is folded in here and dispatched on
 * `m->start_kind` — the go-routine the wrapper parked; because the console can
 * only ever reach 0x80061BA4 through one of the three, the two are
 * behaviourally the same thing. It does not write the flags word: that was the
 * wrapper's, at spawn.
 */
void q2_monster_start_go(q2_monster *m);

/*
 * G_PickTarget, 0x8005F708 — resolve a script `targetname` to an entity.
 *
 * The AI needs it in two places (a creature's path target at spawn and its
 * combat point on first contact) and neither should drag the script namespace
 * into this module, so the resolver is installed from outside. With none
 * installed every lookup fails, which leaves creatures standing where they
 * were placed rather than pathing — the same thing the original does with an
 * unresolvable target.
 */
void q2_ai_set_pick_target(q2_monster *(*fn)(s16 targetname, void *user),
                           void *user);
q2_monster *q2_pick_target(s16 targetname);

/*
 * The class byte the allocator at 0x8005F848 stamps on a path corner
 * (`addiu v1, zero, 114` / `sb v1, 35(v0)` at 0x8005F870/0x8005F874), read
 * back as the equality at 0x80061C10 and again at 0x8005F580.
 *
 * It is in the SECOND class namespace — the 256-entry entity class byte at
 * entity+0x23 — not the 0..37 Population class that indexes class_module[].
 * A path corner never goes in a q2_monster_set for exactly that reason.
 */
#define Q2_CLASS_PATH_CORNER 114

/*
 * The dead-monster pause, 0x3B9ACA00 at 0x8005F338 and again in
 * monster_start_go. It is 1e9 ticks rather than id's 1e8 because the clock is
 * ten times faster; not "forever" in either engine, just longer than any level
 * lasts, and reproducing the number matters for a save that round-trips.
 */
#define Q2_PAUSE_FOREVER 1000000000

/* Decode a move and a frame out of a relocated module image. `offset` is
 * module-relative. Returns false when out of range. */
bool q2_mmove_read(const u8 *image, size_t size, u32 offset, q2_mmove *out);
bool q2_mframe_read(const u8 *image, size_t size, u32 offset, q2_mframe *out);

#endif /* Q2PSX_MONSTER_H */
