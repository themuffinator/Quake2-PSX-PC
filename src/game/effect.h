/*
 * effect.h — the presentation layer: particles, beams, debris and glints.
 *
 * ---------------------------------------------------------------------------
 * Four machines, not one
 * ---------------------------------------------------------------------------
 * There is no generic "effect" in this engine. What looks like one system is
 * four, with different pools, different lifetimes and different renderers.
 * `src/build/fxtables.h` owns their data; this owns their behaviour.
 *
 *   GROUPS   0x80030284, updated and drawn by 0x800304A8.
 *            Up to fifteen screen-aligned quads sharing one origin, one
 *            velocity, one acceleration and a pair of colour ramps. Every
 *            spark, blood spray, explosion flame and gib puff in the game is
 *            this, differing only in ramp, size and lifetime.
 *
 *   BEAMS    0x80064E64, drawn and reset by 0x80064F10.
 *            A 32-slot pool that is refilled from empty EVERY frame. A beam is
 *            a hexagonal tube between two points with two end caps; the twelve
 *            hull vertices are generated at draw time from the beam's own
 *            direction, so the pool stores only the endpoints and a radius.
 *
 *            Behind it sits a TIMED list of twelve (0x800CABC0), whose entries
 *            outlive the frame and are re-submitted until their timers expire.
 *            That is the BFG's beam — the game's weapon trail. See below.
 *
 *   DEBRIS   0x80064558 spawns a burst, 0x80064398 spawns one piece,
 *            0x80064124 thinks. Real entities with physics and a lifetime.
 *
 *   GLINTS   0x80064C00 / 0x80064780.
 *            A faceted body attached to an entity carrying flag 0x04000000,
 *            with a bright band sweeping across its surface.
 *
 *            THIS IS NOT A WEAPON TRAIL. The obvious reading of "a mesh dragged
 *            behind an entity with a band running along it" is a trail, and it
 *            is wrong: the mesh is the `GlintMod` level chunk, and drawing
 *            BIGGUN's — the only one on the disc — puts a 216-quad SPHERE on
 *            screen, not a ribbon. The band sweeps its surface, which is a
 *            shimmer. It is also not the game's weapon trail — the BFG's beam
 *            is, and that lives in the timed list above.
 *
 * ---------------------------------------------------------------------------
 * The group pool is not a particle pool
 * ---------------------------------------------------------------------------
 * A slot is a whole burst, not one particle, and this is load-bearing: the
 * fifteen quads in a group cannot outlive each other, cannot be culled
 * separately, and all read the same ramp entry, so a burst fades as one object.
 * That is why the original's explosions pulse rather than dissolve.
 *
 * Particle 0 carries the group's absolute position and velocity. Particles
 * 1..14 carry an offset and a velocity RELATIVE to particle 0, which is why the
 * whole burst drifts together while spreading — and why the spawner subtracts
 * `vel[0]` from every other velocity before storing it (0x800303C8).
 *
 * The record's field order is preserved here because the offsets are how the
 * layout was established:
 *
 *     +0x00  s32 origin[3]        particle 0, absolute
 *     +0x0C  s16 offset[14][3]    particles 1..14, relative to particle 0
 *     +0x60  s16 accel[3]         added to vel each tick; zeroed by the SPAWNER
 *     +0x66  s16 vel[3]           particle 0, absolute
 *     +0x6C  s16 rel_vel[14][3]   particles 1..14, relative to particle 0
 *     +0xC0  s16 size             world size, scaled by [0x800D5D46] at spawn
 *     +0xC4  u8  view_mask        high nibble: skip this group in viewport n
 *     +0xC5  u8  life             0 means the slot is free
 *     +0xC6  u8  count
 *     +0xC7  u8  area             visibility key, from the area record's +0x20
 *     +0xC8  ramp[0], +0xCC ramp[1]
 *
 * The accel slot sits inside what would otherwise be a fifteenth offset, which
 * is exactly why the burst is capped at fifteen quads and not sixteen.
 *
 * "ALWAYS ZERO at spawn" used to be stated here as a property of the EFFECT.
 * It is a property of the SPAWNER — 0x800301F0 and 0x8003040C memset it — and
 * four call sites overwrite it the instant the spawner returns:
 *
 *     0x8003E0D4  sh v0(=4), 98(v1)     the spark
 *     0x80048C1C  sh v0(=2), 98(v1)     the blood spray
 *     0x80049088  sh v0(=2), 98(v1)     the laser end, first site
 *     0x80049134  sh v0(=2), 98(v1)     the laser end, second site
 *
 * 98 = 0x62 = accel[1], and every one of the four is guarded on the spawner's
 * return value being non-null. So sparks, blood and laser ends SAG; taking the
 * comment at face value made them drift in a straight line forever.
 *
 * ---------------------------------------------------------------------------
 * There are THREE spawners, not one
 * ---------------------------------------------------------------------------
 * This header used to say the engine has one group spawner. It has three, and
 * they differ in their argument lists rather than in what they build:
 *
 *     0x80030284   8 call sites   velocities only; every burst starts at a point
 *     0x8003004C   6 call sites   velocities AND a fifteen-entry OFFSET array,
 *                                 stored as offset[i-1] = offs[i] >> 4, so the
 *                                 burst starts already scattered
 *     0x8002FDFC   6 call sites   an array of ABSOLUTE WORLD POINTS at stride
 *                                 12 instead of one origin — the caller hands
 *                                 it a point per quad and it turns points
 *                                 1..count-1 into offsets itself
 *
 * All three are modelled now. The bullet's world impact is a 0x8003004C site
 * (0x80048AC4) and its absence is why a bullet hitting a wall used to raise the
 * PLAYER-STATE spark — ramp 0, pure blue — instead of a grey smoke puff.
 *
 * THE THIRD ONE'S DISTINGUISHING FEATURE is not "an interleaved record", which
 * is what this header used to guess. It is that the CALLER supplies absolute
 * world points, which is what lets the five mesh drawers below sample vertices
 * off a posed model and hand the results straight in. And the difference from
 * 0x8003004C is one instruction wide and load-bearing:
 *
 *     0x8002FF44  subu v0, v0, v1     0x80030174  sll 16 / sra 20
 *     0x8002FF48  sh   v0, 12(a0)
 *
 * — the third spawner stores the raw 16-bit difference with NO >>4, so a mesh
 * sampled at world scale keeps its shape. Shifting here would collapse a
 * soldier's crackle into a point.
 *
 * ---------------------------------------------------------------------------
 * Two ramps, alternating in threes
 * ---------------------------------------------------------------------------
 * The renderer swaps its two ramps every three quads (0x80030A14), so a group
 * given two different ramps stripes them 3-3-3-3-3 rather than blending. Every
 * spawner on the disc but one passes the same pointer twice; the blood spray
 * (0x80048BFC) is the exception, and it is why blood has two tones.
 *
 * ---------------------------------------------------------------------------
 * Age indexes the ramp, so lifetime IS colour
 * ---------------------------------------------------------------------------
 * `colour = ramp[32 - life]`. Life counts down from what the spawner asked for,
 * so a 15-tick group only ever uses entries 17..31 — the dim tail. Changing a
 * lifetime changes the colour the effect starts at. See fxtables.h.
 *
 * ---------------------------------------------------------------------------
 * What is READ and what is MODELLED
 * ---------------------------------------------------------------------------
 * READ from the executable: the record layout and every offset in it; the
 * integrator; the ramp lookup; the perspective size divide and its minimum of
 * two pixels; the quad's corner order; the two-ramp alternation; the beam pool's
 * size, its per-frame reset and its 40-byte record; the timed list's 28-byte
 * record, its timer and its dedup; the hexagon generator; the
 * beam face sets; the glint's colour band, its mesh, its two draw paths and its
 * band records; the debris burst's velocity draw, the hull it moves in, and the
 * mover's gravity, terminal velocity and no-gravity flag; every constant in
 * `q2_fx_preset`.
 *
 * MODELLED: the debris pool's capacity (the console shares its entity pool).
 * The caller supplies the gravity step and the glint's band data. BIGGUN's
 * recognized script tables and frame driver supply the latter in simcombat.c;
 * the port does not interpret arbitrary relocated level code.
 */
#ifndef Q2PSX_EFFECT_H
#define Q2PSX_EFFECT_H

#include "fxtables.h"
#include "gpu.h"
#include "gte.h"
#include "weapon.h"
#include "world.h"

/* ------------------------------------------------------------------------- */
/* Pool sizes                                                                 */
/* ------------------------------------------------------------------------- */
/*
 * Fifteen. Every spawner on the disc passes exactly fifteen, and the record
 * cannot hold more: the sixteenth offset slot is the acceleration.
 */
#define Q2_FX_GROUP_QUADS      15
#define Q2_FX_GROUP_FOLLOWERS  (Q2_FX_GROUP_QUADS - 1)

/*
 * 0x80030C70 sizes the pool: a negative argument gives zero groups, zero gives
 * thirty-two, anything else is taken as-is. The loader supplies Resources+0x18:
 * the PAL disc requests 64 groups except JAIL4/ZONE0 and JAIL5/ZONE2, with 80.
 */
#define Q2_FX_GROUPS_DEFAULT   32
#define Q2_FX_GROUPS_MAX       80

/* 0x800D5730..0x800D5C30, forty bytes each. */
#define Q2_FX_BEAMS_MAX        32

/* ------------------------------------------------------------------------- */
/* The timed beam list — and this IS the weapon trail                         */
/* ------------------------------------------------------------------------- */
/*
 * `0x800CABC0`..`0x800CAD10` is twelve 28-byte records, walked every frame by
 * `0x80048CA8`, which ticks each timer down by the frame delta and re-submits
 * the live ones into the transient pool. So unlike a queued beam — which lives
 * one frame — a timed beam PERSISTS, and it is the only effect in the game that
 * outlives the frame that made it without being an entity.
 *
 *     +0x00  s16 timer     down by [0x800B2DB4] per frame, clamped at 0
 *     +0x04  s16 clip      handed to the pose lookup at 0x8006CC44
 *     +0x06  s16 radius
 *     +0x08  ptr           the thing the near end is derived from
 *     +0x0C  ptr           the entity the far end sits on
 *     +0x10  tube, +0x14 cap_near, +0x18 cap_far
 *
 * **The BFG fills it.** `0x80049CF0`, inside `0x80049B9C`, which the BFG's tick
 * calls at `0x8004BD04` every frame while the ball flies. It runs a visibility
 * test (`0x80051874`) and then either refreshes the existing entry for that
 * pair of things or takes a free slot, with a timer of **45** and a radius of
 * **64**, drawn in beam style **3** — the green one.
 *
 * That is the classic BFG behaviour: the ball shoots a beam at everything it
 * can see, and each beam lingers after the ball has moved on because its timer
 * has to run down first. It is a weapon trail in every sense that matters, and
 * an earlier pass here concluded the game had none — because every *particle*
 * spawn site is an impact or a death, which is true and is not the whole story.
 *
 * It also corrects the claim that beam styles 3 and 4 are unreachable. They are
 * not reached from the laser dispatcher; they are reached from here. Style 4 —
 * the opaque red one — is filled at `0x8004E9F4` (`addiu t1, v0, -9644`, the
 * style-4 record 0x8009DA54), behind the same visibility test (0x8004E9D4).
 * Parasite calls this import (+0x7C) with posed vertex 97, radius 10 and
 * lifetime 45. The attachment and both endpoints are refreshed each frame.
 *
 * 0x8004E9F4 is 0x5C into `0x8004E998`, a separate function whose only
 * reference is its address materialised at 0x8007DB4C. It is NOT inside
 * `0x8004E920`, which this note used to say: 0x8004E920 is a small complete
 * function that ends at 0x8004E994 (`jr ra`), and it is the point-clip-and-area
 * helper — `*area = 0x80055054(pos, entity+0x54, entity+0xA2)`, then the clipped
 * point back from [0x800C8EB8] and the area from [0x800C8EAA] — that both gib
 * throwers use to place a chunk (0x8005A504, 0x8005B1E8). It constructs no
 * effect.
 */
#define Q2_FX_TIMED_BEAMS_MAX    12   /* 0x800CABC0..0x800CAD10, 28 each  */
#define Q2_FX_TIMED_BEAM_LIFE    45   /* 0x80049DC0                       */
#define Q2_FX_TIMED_BEAM_RADIUS  64   /* 0x80049D20                       */
#define Q2_FX_TIMED_BEAM_STYLE    3   /* 0x80049D00, the green style      */

typedef struct q2_fx_timed_beam {
    s16 timer;                    /* +0x00 */
    s16 attachment;               /* +0x04; -1 for a projectile's origin */
    s16 radius;                   /* +0x06 */
    bool present;                 /* the expiry tick still submits its beam */

    /*
     * WHICH AREA THE RE-SUBMITTED BEAM IS FILED UNDER, and the record has no
     * console counterpart because the console does not store it — it resolves
     * it afresh on every submit. 0x80048D24 calls the point-clip-and-area
     * helper 0x8004E920 with the record's OWNER (record+0x08) and a scratch
     * halfword, and 0x80048D50 `lh a3, 64(sp)` hands that halfword to the beam
     * queue 0x80064E64 as its area argument. The helper's answer is the area
     * record PrimaryColl leaves at 0x800C8EAA after clipping the owner's
     * position — a live cell byte, never 0.
     *
     * The port used to pass a literal 0 at submit and the beam draw culls an
     * area with no screen-change record, so the BFG's whole trail was invisible
     * in any zone with a SortData stream. DEVIATION: this is resolved once per
     * refresh, by the caller, rather than once per submit from the owner
     * entity, so a beam that outlives its ball keeps the area it was last
     * refreshed in instead of following an owner that no longer exists.
     */
    s16 area;

    /*
     * The original keys a record on the two POINTERS it stores at +0x08 and
     * +0x0C and refreshes the matching one rather than allocating a second.
     * The port keys on a caller-supplied pair of ids for the same reason: a
     * BFG ball that can see one target must hold one beam on it, not add a
     * beam every frame until the list is full.
     */
    s32 owner;
    s32 target;

    s32 from[3];
    s32 to[3];

    const q2_fx_face *tube;
    const q2_fx_face *cap_near;
    const q2_fx_face *cap_far;
} q2_fx_timed_beam;

/* MODELLED. The original allocates debris out of the shared entity pool, so it
 * has no private ceiling; this port gives it one so the frame cost is bounded. */
#define Q2_FX_DEBRIS_MAX       48

/* The glint mesh's transform window — 0x80064780 walks 96 vertices in threes
 * and its face loop is bounded at 94. */
#define Q2_FX_GLINT_VERTS      96

/* ------------------------------------------------------------------------- */
/* A glint is also a LIGHT, and the only style-1 flare the engine ever raises  */
/* ------------------------------------------------------------------------- */
/*
 * 0x800648B8, near the top of the glint renderer: `AddDynamicLight` with the
 * four-halfword record at 0x800AEB28 — inner 300, outer 800, style 1, size
 * shift 3 — coloured by the glint's own tint and positioned at the entity's
 * origin (ent+164). It is gated on the renderer's fifth argument and both of
 * its callers (0x80064D7C, 0x80064E2C) pass 1, so every drawn glint raises one.
 *
 * It is the only such request in the image. Every other AddDynamicLight site
 * carries style 0 and is therefore never seen as a flare — the four constant
 * records at 0x800AE7D8, 0x800AE95C, 0x800AE994 and 0x800AEAB4 are all zero —
 * so the glint alone asks for a runtime flare, and it asks for the largest one
 * there is: all six of style 1's elements at a reach of `64 << 3` rather than
 * the disc's uniform 64.
 *
 * AND IT NEVER GETS ONE. The frame at 0x80038F5C runs the flare stage
 * (0x80075BDC) at 0x80039008, the entity draw (0x800304A8) at 0x80039010, and
 * the dynamic-light list's RESET (0x80075B94, which parks the write pointer
 * back at 0x800E3D18) at 0x8003903C — last, after every stage. The glint is an
 * entity draw, so its light is raised one stage too late for this frame's flare
 * pass and is cleared before the next one. The light is real and lights whatever
 * the entity stage draws after it; the style and the shift are dead operands.
 *
 * The port raises it in the same place for the same reason and reproduces the
 * same unreachability, rather than moving it earlier to make the flare appear —
 * that would put an effect on screen the console never puts there. See flare.h
 * for what the style and the shift would have bought.
 */
#define Q2_FX_GLINT_LIGHT_INNER  300
#define Q2_FX_GLINT_LIGHT_OUTER  800
#define Q2_FX_GLINT_LIGHT_STYLE    1
#define Q2_FX_GLINT_LIGHT_SHIFT    3

/* ------------------------------------------------------------------------- */
/* What turns a glint on, and it is not the executable                        */
/* ------------------------------------------------------------------------- */
/*
 * The draw at 0x8006AA44 gates on bit 0x04000000 of entity +0x10C, and NOTHING
 * IN THE EXECUTABLE EVER SETS IT. All thirty-nine writes to +0x10C were checked
 * and the largest bit any of them raises is 0x8000; the only site in the whole
 * image that materialises 0x04000000 into a general register is the test
 * itself. Read from the executable alone the glint is dead code.
 *
 * It is not dead. BIGGUN's `LevelBin` — the level script, which is a
 * RELOCATABLE MIPS MODULE, not bytecode — carries both halves, at file offsets
 * 0x18650 and 0x187B8 of its COMMON.DAT:
 *
 *     lw   v0, 268(a0)      ; ent+0x10C
 *     lui  v1, 0x0400
 *     or   v0, v0, v1       ; raise the glint flag
 *     sw   v0, 268(a0)
 *
 * and, four instructions earlier, `sb 6, 695(a0)` — ent+0x2B7, the BAND COUNT.
 * The second site tests the same bit and resets `sh 4, 702(a0)` — ent+0x2BE,
 * the PHASE.
 *
 * That last one is the check that the band formula was read right: the phase
 * starts at 4 and 0x80064D3C counts it down, which is exactly what makes
 * `(width / 4) * (4 - phase)` sweep the band from one end of the mesh to the
 * other. A formula with the subtraction the other way round would have looked
 * just as plausible and swept backwards.
 *
 * The level script owns the flag and the band data. q2_sim_glint_tick binds
 * the decoded effect to its live Blackhole P owner; no camera-owned glint is
 * substituted when that item is absent.
 */
#define Q2_FX_GLINT_BANDS       6   /* ent+0x2B7, set by BIGGUN's script */
#define Q2_FX_GLINT_PHASE_START 4   /* ent+0x2BE, and the 4 in the formula */
/* BIGGUN+0x410 passes slot 12's X (832) as BOTH coordinates to GetTPage.
 * Keep bit 11 in GPU state; the retail 1 MiB VRAM address wraps to Y=256. */
#define Q2_FX_GLINT_LIGHTNING_TPAGE 0x083Du

/* ------------------------------------------------------------------------- */
/* One band, and where the bands come from                                    */
/* ------------------------------------------------------------------------- */
/*
 * 0x80064C00 has TWO paths and they are not variations of each other.
 *
 *   ent+0x2B7 == 0   one band, using the entity's own colour (+0x2B4), its own
 *                    phase (+0x2BE) and its own matrix (+0x2C0), at width 8192.
 *
 *   ent+0x2B7 != 0   that many bands, each a 12-byte record in a SHARED array
 *                    at 0x800D565C with its own orientation, its own phase and
 *                    its own colour, at width 4096 — so a band in the multi
 *                    path is half as wide and twice as bright as one in the
 *                    single path (see q2_fx_glint_shade: the two uses of width
 *                    do not cancel).
 *
 * Each band's record is
 *
 *     +0x00  s16 angle[3]   its own orientation, composed with the entity's
 *     +0x06  u8  phase      decremented by the DRAW, per band
 *     +0x07  u8  pad
 *     +0x08  u32 colour     {r,g,b,code}, as everywhere else here
 *
 * and 0x800D565C..0x800D56B0 holds SEVEN of them, which is the ceiling however
 * many the count asks for.
 *
 * The array is not filled by the executable either: 0x8007A074 exports it to
 * relocated modules through their import table at +0x3BC, next to `T_Damage`
 * and the rest of the engine's services. So the level script owns both the flag
 * and the band data — the effect system's job is to consume them, and that is
 * what is reconstructed here.
 *
 * The per-band phase decrement is a PLAIN byte step that underflows: 0x80064DEC
 * is `addiu v0, a3, 255` with no clamp and no reload, so a band that runs past
 * zero wraps to 255 and lights nothing for the next ~250 ticks. Refreshing it is
 * the script's job too, and wrapping it back to the start here would be the port
 * inventing a reload the console does not perform.
 *
 * It happens inside the DRAW in the original, which is why a two-viewport frame
 * would advance every band twice. This port steps it in `q2_fx_glint_advance`
 * after the rendered frame, sharing one phase across all viewports.
 */
#define Q2_FX_GLINT_BANDS_MAX   7
#define Q2_FX_GLINT_BAND_WIDTH  4096   /* 0x80064DFC, the multi-band path  */
#define Q2_FX_GLINT_ONE_WIDTH   8192   /* 0x80064D48, the single-band path */

/* The decoded `GlintMod` mesh. Defined here rather than beside its decoder
 * because the glint state below holds one by value; the decode contract is
 * documented with q2_fx_glint_mesh_decode. */
typedef struct q2_fx_glint_mesh {
    const s16 (*vert)[4];   /* {x, y, z, band} */
    u32         vert_count;
    const u8   *index;      /* 4 per face */
    u32         face_count;
} q2_fx_glint_mesh;

typedef struct q2_fx_glint_band {
    s16 angle[3];   /* +0x00 composed with the entity's own matrix */
    u8  phase;      /* +0x06 */
    u8  pad;        /* +0x07 */
    u32 colour;     /* +0x08 */
} q2_fx_glint_band;

typedef struct q2_fx_glint {
    q2_fx_glint_mesh mesh;
    bool             ready;

    /* True when the map's own LevelBin raises the flag. See q2_fx_glint_scan. */
    bool             raised;

    /* ent+0x2B7. Zero selects the single-band path. */
    u8               band_count;

    /* The shared array at 0x800D565C. */
    q2_fx_glint_band band[Q2_FX_GLINT_BANDS_MAX];

    /* The single-band path's own state: ent+0x2B4 and ent+0x2BE. */
    u8               tint[3];
    u16             phase;

    /* LevelBin's authored directions, before its per-frame jitter and the
     * subtraction of the owner's angles (BIGGUN +0x1B40..+0x1DA0). */
    bool             scripted;
    s16              base_angle[Q2_FX_GLINT_BANDS_MAX][3];
    q2_rng           script_rng;
    u32              script_time;
    s32              origin[3];
    s32              angles[3];
    u8               area;

    /* BIGGUN's private ribbon renderer (+0x138, +0x38C), separate from the
     * engine's solid laser prisms. Data is recovered from the level module. */
    bool             lightning_ready;
    s32              lightning_target[Q2_FX_GLINT_BANDS_MAX][3];
    u8               lightning_uv[8][2];
    u32              lightning_noise[8];
    s32              lightning_offset[8];
    u32              lightning_frame;
    s16              turn_left, turn_speed;
    /* BIGGUN's generator lifecycle (+0x11F4, +0x131C, +0x182C). */
    bool             zone_active, owner_active, shutting_down, dying, finished;
    bool             burst_pending, burst_done, sparks_visible;
    s32              power_left, death_time, crack_time;
    s32              evacuation_left; /* module+0x35B4, initially 300 */
    s32              spark_origin[3];
} q2_fx_glint;

/* ------------------------------------------------------------------------- */
/* Asking the level script whether it raises a glint                          */
/* ------------------------------------------------------------------------- */
/*
 * `LevelBin` is a relocatable MIPS module, and this project does not execute
 * one. It does not have to: the question "does this level raise a glint, and
 * with what" is answerable by READING the module, and reading modules is
 * something the project already does.
 *
 * `q2_fx_glint_scan` looks for the instruction triple that raises the flag —
 *
 *     lw   rX, 0x10C(rY)
 *     lui  rZ, 0x0400
 *     or   rX, rX, rZ
 *
 * — and, when it finds one, picks up the two immediates the surrounding code
 * writes: `sb <n>, 0x2B7(rY)` for the band count and `sh <n>, 0x2BE(rY)` for
 * the phase. So the port takes the script's own numbers rather than a
 * hardcoded 6 and 4, and a map whose script uses different ones gets those.
 *
 * The authored angle table is recovered from BIGGUN's +0x1B54 address/index
 * sequence. This recognises that script, not arbitrary MIPS programs. A map
 * with no recognised table does not activate a guessed arrangement of bands.
 *
 * The +0x1868 owner search takes kind 46, item 20. The runtime entity fields
 * now carry both keys, and simcombat.c performs that lookup each tick. The
 * script's private RNG supplies three angular jitters and three colour bytes
 * per band; the script refreshes their phases to four before each draw.
 */
#define Q2_FX_GLINT_TARGET_KIND  46   /* ent+0xD2, the search key   */
#define Q2_FX_GLINT_TARGET_ID    20   /* ent+0xDA                   */
#define Q2_FX_GLINT_ENT_STRIDE  768   /* the script's own step      */
#define Q2_FX_GLINT_ENT_MAX      48   /* and its own bound          */

typedef struct q2_fx_glint_script {
    bool raises;        /* the flag is raised somewhere in this module  */
    u32  raise_offset;  /* where, so the finding can be re-checked      */
    u8   band_count;    /* the `sb` near it, or 0 if none was found     */
    u8   phase;         /* the `sh` near it, or 0                       */
    bool bands_found;
    s16  angle[Q2_FX_GLINT_BANDS_MAX][3];
    bool lightning_found;
    s32  target[Q2_FX_GLINT_BANDS_MAX][3];
    u8   uv[8][2];
    s32  spark_origin[3];
} q2_fx_glint_script;

/*
 * Read a `LevelBin` chunk and report whether its script turns a glint on.
 * Returns false when it does not, leaving `*out` zeroed.
 */
bool q2_fx_glint_scan(q2_fx_glint_script *out, const u8 *levelbin, u32 size);

/* Consume the displayed phase, with byte/halfword underflow. Call after a
 * rendered frame in which the owner was drawn, including wholly black bands.
 * All viewports share that phase; logic ticks only refresh it. */
void q2_fx_glint_advance(q2_fx_glint *g);

/* Refresh the module-owned band records once per simulation frame. The
 * script has its own rand(), seeded at zero, separate from gameplay. */
void q2_fx_glint_refresh(q2_fx_glint *g, const s32 angles[3]);
/* Advance the level renderer's eight signed, wrapping noise accumulators. */
void q2_fx_glint_lightning_tick(q2_fx_glint *g, q2_rng *rng);
s32 q2_fx_sqrt_nr(u32 n); /* 0x80055CBC, positive signed-32-bit domain */
u32 q2_fx_glint_lightning_draw(const q2_fx_glint *g, u16 tpage, u16 clut,
                               const q2_camera *cam, psx_ot *ot, gte_state *gte);

/*
 * Draw a glint, taking whichever path its band count selects. Returns the
 * number of primitives emitted, and zero when the mesh is not loaded.
 */
u32 q2_fx_glint_draw(const q2_fx_glint *g, const s32 origin[3], s32 yaw,
                     const q2_camera *cam, psx_ot *ot, gte_state *gte);

/* Entity-bound draw: full rotation, retail 768/256 scale and area routing. */
u32 q2_fx_glint_draw_entity(const q2_fx_glint *g, const s32 origin[3],
                            const s32 angles[3], s32 area,
                            const q2_camera *cam, psx_ot *ot, gte_state *gte);

/* Join the glint to its owner's packet chain, using the owner's actual matrix.
 * NCLIP selects AddPrim/AddPrimEnd as at 0x80064B64. */
u32 q2_fx_glint_draw_attached(const q2_fx_glint *g, const s32 origin[3],
                              const s16 rotation[3][3], s32 area, s32 batch,
                              const q2_camera *cam, psx_ot *ot, gte_state *gte);

/* ------------------------------------------------------------------------- */
/* The per-frame particle budget                                              */
/* ------------------------------------------------------------------------- */
/*
 * 0x80030CB4 sets a quad allowance the renderer spends and the spawner ignores:
 * one viewport gets `groups * 15`, more than one gets `(groups / 2) * views *
 * 15`. A group whose count exceeds what is left is skipped ENTIRELY rather than
 * partially drawn (0x80030650), so a crowded frame drops whole bursts. Keeping
 * that is the difference between the original's behaviour under load and a
 * smooth degradation it never had.
 */
u32 q2_fx_budget(u32 group_count, u32 viewport_count);

/* ------------------------------------------------------------------------- */
/* One group                                                                  */
/* ------------------------------------------------------------------------- */
typedef struct q2_fx_group {
    s32 origin[3];                              /* +0x00 */
    s16 offset[Q2_FX_GROUP_FOLLOWERS][3];       /* +0x0C */
    s16 accel[3];                               /* +0x60 */
    s16 vel[3];                                 /* +0x66 */
    s16 rel_vel[Q2_FX_GROUP_FOLLOWERS][3];      /* +0x6C */
    s16 size;                                   /* +0xC0 */
    u8  view_mask;                              /* +0xC4 high nibble */
    u8  life;                                   /* +0xC5 */
    u8  count;                                  /* +0xC6 */
    u8  area;                                   /* +0xC7 */
    const q2_fx_ramp *ramp[2];                  /* +0xC8, +0xCC */
    /*
     * NO CONSOLE COUNTERPART, and it exists because the port splits one console
     * routine in two.
     *
     * 0x800304A8 draws every group and THEN, at its own tail (0x80030B1C
     * onward), decrements each life and integrates each position — one
     * function, draw first, integrate second. This port draws from the client
     * (q2_fx_build_ot) and integrates from the sim tick (q2_fx_tick), and the
     * sim tick runs first, so a burst raised by the gameplay code this tick was
     * aged before anything could draw it: a life-1 group (the quad shell and
     * the energy crackle, both re-spawned every tick) never appeared at all,
     * and every other burst lost its first ramp entry.
     *
     * Set by the spawners and cleared by the first q2_fx_tick that sees it,
     * which skips that group's integration exactly once. The net per-frame
     * order is then the console's.
     */
    bool fresh;
} q2_fx_group;

/* ------------------------------------------------------------------------- */
/* One transient beam                                                         */
/* ------------------------------------------------------------------------- */
/*
 * The forty-byte record at 0x800D5730. The original stores the three face
 * lists as three independent pointers, so a caller could in principle mix a
 * tube from one style with caps from another. Nothing does, but the three
 * pointers are kept rather than collapsed to a style index, because collapsing
 * them would quietly remove a degree of freedom the format has.
 */
typedef struct q2_fx_beam {
    s32 from[3];                  /* +0x00 */
    s32 to[3];                    /* +0x0C */
    s16 radius;                   /* +0x18 */
    s16 area;                     /* +0x1A */
    const q2_fx_face *tube;       /* +0x1C, six faces  */
    const q2_fx_face *cap_near;   /* +0x20, two faces  */
    const q2_fx_face *cap_far;    /* +0x24, two faces  */
} q2_fx_beam;

/* ------------------------------------------------------------------------- */
/* One debris piece                                                           */
/* ------------------------------------------------------------------------- */
typedef struct q2_fx_debris {
    bool in_use;
    s32  pos[3];       /* entity +0xA4 and +0x54, both set to the spawn point */
    s16  vel[3];       /* entity +0xE0 */
    s16  spin[3];      /* entity +0xE6, three raw rand() draws               */
    s16  life;         /* entity +0xF4, 2100 at spawn, -300 per impact       */
    s16  model;        /* which registered debris model this piece uses      */
    u32  flags;        /* entity +0x10C; Q2_FX_ENT_NO_GRAVITY lives here    */
    u8   area;         /* entity +0x9E */
    s32  node;         /* collision cell, -1 = unknown (same as projectiles) */
} q2_fx_debris;

/* ------------------------------------------------------------------------- */
/* A creature's muzzle flash                                                  */
/* ------------------------------------------------------------------------- */
/*
 * 0x80031094, import +0xA0, which the Soldier's, the Infantry's and the
 * Gunner's and Boss1's export 1 call while their object is drawn — and ONLY while the
 * sign bit of object+0x10C is up. `monster_fire_bullet` and
 * `monster_fire_shotgun` raise it (0x80061E28, 0x80061F18; the blaster, rail,
 * rocket and grenade spawners do not), and the object's per-frame handler
 * drops it before the next think (0x8007EE20, `and 0x7FFFFFFF`). So a flash is
 * one frame per round, from those four creatures and from those two weapons.
 *
 * It is a light — q2_creature_muzzle_light's radii at the muzzle — and this
 * quad: a POLY_FT4, code 0x2C with the semi-transparency bit (0x800314C8),
 * colour (255, 255, 255), UVs (64,208) (95,208) (64,239) (95,239) — a 32x32
 * cell of the chars.lbm page, `GetTPage(0, 0, 0, 256)` with ABR 1, ADDITIVE
 * (0x80031590..0x800315A8) — and CLUT `[0x800E3FAC]`, built-in palette 64.
 *
 * The corners are built in MODEL space around the posed muzzle: a square of
 * half-diagonal 120 (Boss1: 240) turned by a free-running angle, (x - c, y - s), (x + s,
 * y - c), (x - s, y + c), (x + c, y + s) with c and s the angle's cosine and
 * sine times 120 (0x80031270..0x80031320), all at the muzzle's own z. Rotated
 * by the object's matrix, they lie across the model's X/Y plane — square to
 * the way the creature faces, so a guard firing at you shows the whole star
 * and one seen side-on shows a sliver. The caller does that part (it owns the
 * pose) and hands over world corners in hardware Z order.
 *
 * THE ANGLE is gp+16996, advanced by twice the frame delta once per new frame
 * (0x800310E0..0x800310F8) and read as itself on an odd frame and 128 behind
 * on an even one (0x800310FC..0x80031120): a slow spin with an eleven-degree
 * flicker. q2_fx_flash_angle is that.
 *
 * The flash is skipped in a viewport whose camera-space depth to the muzzle
 * is under 257 (0x80031470). Sorted at the third corner's depth (`swc2 SZ3`).
 */
/* Inline staging capacity, not a retail limit. Retail emits directly for
 * each firing actor; additional flashes use overflow storage for this frame. */
#define Q2_FX_FLASH_MAX         16
#define Q2_FX_FLASH_SIZE       120     /* a3 at 0x8010216C, 0x801014xx */
#define Q2_FX_FLASH_U0          64
#define Q2_FX_FLASH_V0         208
#define Q2_FX_FLASH_U1          95
#define Q2_FX_FLASH_V1         239
#define Q2_FX_FLASH_PALETTE_ID  64
#define Q2_FX_FLASH_ABR       0x20u     /* ABR 1, additive                  */
#define Q2_FX_FLASH_NEAR       257

typedef struct q2_fx_flash {
    s32 corner[4][3];   /* world space, hardware Z order */
    s32 centre[3];      /* the muzzle, for the near test */
    u8  area;
} q2_fx_flash;

/*
 * THE BLUR TRAIL — BLUR.C: AddBlurPoints 0x8002F238, ProcessBlurs 0x8002F51C,
 * InitBlurPoints 0x8002FD98.
 *
 * Six 32-byte records at 0x800C73F4 and a pool of 96 eight-byte points at
 * 0x800C70EC (index 1..96; index 0 is the slot an exhausted pool writes into),
 * with a byte stack of free indices at 0x800C7094 that InitBlurPoints fills
 * 1..96 and AddBlurPoints pops from the top.
 *
 *     record +0   u16 owner (entity+0xD0), u16 first vertex   — the key
 *            +4   the entity
 *            +8   s32[3] origin: the first vertex where the record was made
 *            +20  colour word, three bytes and a code byte
 *            +24  the frame it was last refreshed, [0x800B2DE4]
 *            +28  s16 life, in frames — the caller's fourth argument
 *            +30  u8  head point
 *            +31  u8  vertices per sample, less one
 *     point  +0   s16[3] offset from the record's origin
 *            +6   s8  next: > 0 the same sample, < 0 the head of the older
 *                     sample negated, 0 the end
 *
 * Every call samples the listed model vertices (0x8006CC44, world space) and
 * PREPENDS them as a new sample — a "generation" — linked to the previous one
 * by a negated index. The draw joins consecutive samples into gouraud,
 * semi-transparent POLY_G4s, one per neighbouring vertex pair: a ribbon swept
 * out by the line through the vertices. The newest sample is lit at the full
 * colour scaled by `remaining / life`, where remaining is the refresh frame
 * plus the life less now, and each older one at one less over life; a sample
 * whose share would fall below zero is cut off and its points returned to the
 * pool. So a trail refreshed every frame is life + 1 samples long and fades
 * to black at its tail, and one no longer refreshed fades out over `life`
 * frames. A record whose entity has gone (its +0xF4 zero, or its +0xD0 no
 * longer the key's) is freed at once — q2_fx_blur_release.
 *
 * The quads carry no texture page, so their blend is inherited from the
 * preceding draw-mode command or textured polygon. Every caller on this
 * disc passes (32, 32, 32).
 */
#define Q2_FX_BLURS             6    /* 0x800C73F4..0x800C74B4, 32 each   */
#define Q2_FX_BLUR_POINTS      96    /* InitBlurPoints' `slti 96`          */
#define Q2_FX_BLUR_SPAN_MAX    16    /* the two 64-byte xy buffers, sp+56  */
#define Q2_FX_BLUR_GREY      0x20    /* 0x800AE96C, every caller's colour  */

typedef struct q2_fx_blur_point {
    s16 d[3];
    s8  next;
} q2_fx_blur_point;

typedef struct q2_fx_blur {
    u32 key;          /* owner | first vertex << 16; 0 is a free record */
    s32 origin[3];
    u8  rgb[3];
    s32 stamp;
    s16 life;
    u8  head;
    s8  span;
    u8  area;         /* the draw area the trail sorts in (entity+0x9E) */
} q2_fx_blur;

/* ------------------------------------------------------------------------- */
/* The effect world                                                           */
/* ------------------------------------------------------------------------- */
typedef struct q2_fx_stats {
    u32 groups_live;
    u32 groups_drawn;
    u32 groups_skipped_budget;  /* the whole-burst drop described above */
    u32 groups_skipped_near;    /* projected in front of the near plane */
    u32 quads_emitted;
    u32 beams_queued;
    u32 beams_dropped;          /* the pool was full when one was offered */
    u32 beam_faces_emitted;
    u32 debris_live;
    u32 ot_overflow;
    u32 flashes_drawn;          /* creature muzzle-flash quads emitted      */
    u32 blur_samples;           /* AddBlurPoints calls that took a record   */
    u32 blur_quads;             /* ribbon quads emitted                     */
} q2_fx_stats;

typedef struct q2_fx_world {
    const q2_fx_tables *tab;

    q2_fx_group  group[Q2_FX_GROUPS_MAX];
    u32          group_count;      /* the pool size, not the live count */

    q2_fx_beam   beam[Q2_FX_BEAMS_MAX];
    u32          beam_count;       /* how many are queued THIS frame    */

    /* The persistent list at 0x800CABC0. Unlike `beam`, these survive the
     * frame and are re-submitted until their timers run out. */
    q2_fx_timed_beam timed[Q2_FX_TIMED_BEAMS_MAX];

    q2_fx_debris debris[Q2_FX_DEBRIS_MAX];

    /*
     * The registration list at 0x800D56B0, capped at 32 by 0x80064F80. A level
     * fills it from its own class table ("Debris1".."Debris3" all register
     * through 0x80064F70) and the burst picks uniformly from it.
     */
    s16          debris_model[32];
    u32          debris_model_count;

    u32          viewport_count;   /* [0x800B2C2C] */
    u32          budget;           /* recomputed by q2_fx_world_resize   */
    u32          budget_left;      /* shared by every viewport this frame */

    /*
     * 0x800D5D46, the multiplier every spawner applies to its size argument
     * before storing it: `size = (arg * scale) / 512`, rounding toward zero.
     * It is a level-scale knob, and the port defaults it to the value that
     * makes the divide a no-op so a caller that never sets it gets the
     * argument through unchanged.
     */
    s16          size_scale;

    /*
     * The particle image's registration, from the level's VRAM upload. The
     * ramp's ABR field is OR-ed into `tpage_base` at draw time, exactly as
     * 0x80030830 does — which is why the base is stored without it.
     */
    u16          tpage_base;    /* [0x800DDD5A] */
    u16          clut;          /* [0x800E3F2C + 150] = palette 75 */

    /*
     * Draw flat quads instead of textured ones. Set automatically when no
     * image has been registered, because the alternative is a burst that emits
     * fifteen primitives and shows nothing. Clearing it without registering an
     * image gives you the console's behaviour against an empty texture page,
     * which is nothing at all.
     */
    bool         untextured;

    /* This frame's creature muzzle flashes; see q2_fx_flash_add. */
    q2_fx_flash  flash[Q2_FX_FLASH_MAX];
    q2_fx_flash *flash_extra;
    u32          flash_extra_capacity;
    u32          flash_count;
    u16          flash_clut;    /* [0x800E3F2C + 128] = palette 64 */

    /* The blur trails, BLUR.C — see q2_fx_blur_add. */
    q2_fx_blur        blur[Q2_FX_BLURS];
    q2_fx_blur_point  blur_pt[Q2_FX_BLUR_POINTS + 1];  /* [0] is the dummy */
    u8                blur_free[Q2_FX_BLUR_POINTS];
    u32               blur_free_top;
    s32               blur_clock;     /* [0x800B2DE4], a frame counter */

    q2_fx_stats  stats;
} q2_fx_world;

/*
 * Register the particle image. `tpage` is the page word WITHOUT its
 * semi-transparency field — the ramps supply that per effect — and `clut` is
 * the palette. Turns `untextured` off.
 */
void q2_fx_set_texture(q2_fx_world *w, u16 tpage_base, u16 clut);

/*
 * The same, from a map's uploaded HUD atlas — which is the page the particles
 * are actually on, with built-in palette 75 as their CLUT. Does nothing when the
 * font is not resident, so a map with no `chars.lbm` (two of the forty-nine)
 * keeps the flat-quad fallback.
 */
#define Q2_FX_CLUT_PALETTE_ID 75
struct q2_hud_font;
void q2_fx_use_hud_atlas(q2_fx_world *w, const struct q2_hud_font *font);

/*
 * AddBlurPoints for one owner: `pts` are the listed vertices' world positions
 * this frame, `count` of them (two or three on this disc), keyed on `owner`
 * and `first_vert` as the console keys on entity+0xD0 and the list's first
 * index. A new record takes its origin from pts[0]; an existing one is
 * refreshed. False when every record is taken ("Too many blurs", 0x800ABDF0).
 */
bool q2_fx_blur_add(q2_fx_world *w, u16 owner, u16 first_vert,
                    const s32 (*pts)[3], u32 count, const u8 rgb[3],
                    s16 life, u8 area);

/* Free every record of `owner` — the entity-gone arm of ProcessBlurs. */
void q2_fx_blur_release(q2_fx_world *w, u16 owner);

/* Queue one creature muzzle flash for this frame's draw. False on allocation
 * failure. Sprite count is independent of the sixteen dynamic-light slots. */
bool q2_fx_flash_add(q2_fx_world *w, const s32 corner[4][3],
                     const s32 centre[3], u8 area);

/* Drop this frame's flashes — the object handler's 0x8007EE20, before the
 * thinks that may raise new ones. */
void q2_fx_flash_clear(q2_fx_world *w);

/*
 * The flash's turn on the 4096-step circle. `spin` is the running gp+16996
 * total, `frame` the frame counter whose parity picks the arm.
 */
s32 q2_fx_flash_angle(s32 spin, u32 frame);

/* The four model-space corners around `centre`, in hardware Z order. */
void q2_fx_flash_corners(const s32 centre[3], s32 angle, s32 size, s32 out[4][3]);

/* 512, so that `size_scale == Q2_FX_SIZE_SCALE_UNITY` passes the argument
 * through. The disc's own value lives in level data, not in the executable. */
#define Q2_FX_SIZE_SCALE_UNITY 512

/* ------------------------------------------------------------------------- */
/* What a particle quad is textured with                                      */
/* ------------------------------------------------------------------------- */
/*
 * A quad is a POLY_FT4, and the renderer at 0x80030958 writes only its four
 * corners and its draw-mode word. The UVs and the CLUT are baked into the
 * primitive pool once, by the allocator at 0x80030DB8:
 *
 *     uv0 = (240, 240)   uv1 = (255, 240)
 *     uv2 = (240, 255)   uv3 = (255, 255)
 *
 * — a 16x16 patch in the bottom-right corner of a page. Both the page and the
 * palette are zero-filled bss, written when images are registered at load time,
 * and both are resolvable anyway.
 *
 * THE PAGE. `0x8001AD14` picks between two of those globals by a menu item's
 * face size: size 8 takes `[0x800DDD5A]` and anything else `[0x800DDD56]`. The
 * 8-pixel face is `chars.lbm` — hudtables.h — so the particle and the small
 * menu font are on the SAME PAGE, and a particle is the 16x16 corner of the HUD
 * atlas rather than an image of its own.
 *
 * THE PALETTE. `0x800E3FC2` is not a standalone global, which is why an
 * address-level sweep finds no writer for it: the allocator materialises
 * `0x800E3F2C` and reads `+150`. `0x800E3F2C` is the CLUT-id table the boot
 * palette loop at `0x8007610C` fills — one entry per built-in palette, indexed
 * by the record's own id, and hudtables.h already decodes the whole bank. So
 * the particle's palette is **built-in palette 75**, three below the box ramp's
 * 76 and three above the font's 72.
 *
 * `q2_fx_use_hud_atlas` wires up both from a map's own VRAM.
 *
 * A caller with no VRAM at all gets `untextured`, which emits a flat quad of
 * the ramp's colour instead — visible, honest, and NOT what the hardware drew.
 * It is a fallback rather than a reconstruction, and it is a separate flag so
 * nobody has to guess which they are looking at.
 */
#define Q2_FX_QUAD_U0 240
#define Q2_FX_QUAD_V0 240
#define Q2_FX_QUAD_U1 255
#define Q2_FX_QUAD_V1 255

void q2_fx_world_init(q2_fx_world *w, const q2_fx_tables *tab);

/* Change the group pool size and recompute the budget. `groups` follows
 * 0x80030C70: zero means thirty-two, negative disables the pool. */
void q2_fx_world_resize(q2_fx_world *w, s32 groups, u32 viewports);

/* Free every group, beam and debris piece. 0x80048C54 + 0x80064EF4. */
void q2_fx_world_clear(q2_fx_world *w);

/* ------------------------------------------------------------------------- */
/* Groups                                                                     */
/* ------------------------------------------------------------------------- */
/*
 * Spawn one group. Faithful to 0x80030284:
 *
 *   - the first free slot wins, scanning from the bottom of the pool
 *   - `vel[0]` is stored absolute; `vel[1..]` are stored as differences
 *   - offsets and the acceleration start at zero
 *   - `size = (size_arg * world->size_scale) / 512`, rounding toward zero
 *   - the low byte of the flags word is cleared, so view_mask starts empty
 *
 * `vel` must have `count` entries. Returns the slot, or -1 when the pool is
 * full — which the original also has to cope with, and which it handles by
 * silently dropping the effect.
 */
s32 q2_fx_group_spawn(q2_fx_world *w,
                      const s32 origin[3],
                      const s16 (*vel)[3], u32 count,
                      const q2_fx_ramp *ramp0, const q2_fx_ramp *ramp1,
                      u32 life, s32 size, u8 area);

/*
 * The SECOND spawner, 0x8003004C — six call sites, none of which this port had.
 *
 * Identical to the above except that it also takes a fifteen-entry offset
 * array and stores `offset[i-1] = offs[i] >> 4` for i in 1..count-1
 * (0x80030174, `sll 16 / sra 20`). `offs[0]` is discarded, because particle 0
 * IS the origin and has no offset slot.
 *
 * The practical difference is small and real: a burst raised through this
 * spawner starts already scattered instead of at a point. At the bullet site
 * the offsets are drawn with shift 7 and then shifted a further 4, so the
 * pre-scatter is about +/-8 world units — a jitter, not a cloud.
 */
s32 q2_fx_group_spawn_offsets(q2_fx_world *w,
                              const s32 origin[3],
                              const s16 (*offs)[3], const s16 (*vel)[3],
                              u32 count,
                              const q2_fx_ramp *ramp0, const q2_fx_ramp *ramp1,
                              u32 life, s32 size, u8 area);

/* ------------------------------------------------------------------------- */
/* The THIRD spawner — 0x8002FDFC, an array of absolute world points          */
/* ------------------------------------------------------------------------- */
/*
 * Six call sites, and every one of them is a MESH DRAWER in this file:
 * 0x80058B5C, 0x80058D24, 0x80058EE8, 0x800590AC, 0x80059274 and 0x8005AD30.
 *
 * Instruction for instruction it is 0x80030284 with one difference. The free
 * slot scan is the same (0x8002FE08 `lh [0x800B285C]` for the count, `lw
 * [0x800B2860]` for the base, stride 288, first slot whose +0xC5 life is zero);
 * particle 0's three WORDS come from `pts[0]` at 0x8002FE88; the velocity block
 * at +0x66 comes from `vel[0]` as six bytes at 0x8002FEB8; the accel memset
 * (0x8002FFC4), the size divide (0x8002FFE4, `mult` by [0x800D5D46] then
 * `bgez; addiu 511; sra 9`) and the view_mask clear (0x80030000, the low byte
 * knocked out as two nibble masks so +0xC5..+0xC7 survive) are all identical.
 *
 * The difference is the offset loop at 0x8002FF38:
 *
 *     8002FF38  lhu  v0, 0(a1)        ; a1 walks pts + 12*i
 *     8002FF3C  lhu  v1, 16(sp)       ; sp+16..24 hold pts[0]'s three words
 *     8002FF44  subu v0, v0, v1
 *     8002FF48  sh   v0, 12(a0)
 *
 * `offset[i-1][k] = (s16)(pts[i][k] - pts[0][k])` — a truncated 16-bit
 * difference with NO SHIFT, where 0x8003004C's equivalent (0x80030174) is
 * `sll 16 / sra 20`, an arithmetic >>4. That is the whole reason this spawner
 * exists: a mesh sampled in world units would lose fifteen sixteenths of its
 * extent through the other one. The velocities are relative the usual way
 * (0x8002FF84 `sh v0, 102(a3)`).
 *
 * `pts` must have `count` entries and `vel` must have `count` entries.
 *
 * AREA. 0x8002FED0 calls 0x800686C4 with the caller's area byte and the point
 * array, and that helper is `if (a0) return a0; else return the area record for
 * whichever cell the point is in` (0x800686D0 `bne a0, zero` -> return; else
 * 0x80044F54 against 0x800C8E90 and `lbu 32(v0)`). The port's spawners take a
 * u8 area and store it, exactly as the non-zero arm does. Every call site in
 * this cluster passes entity+0x9E, which is non-zero in practice, so the
 * auto-resolve arm is a pre-existing divergence rather than a new one; plumbing
 * a world query into this module to reach it would be a bigger change than the
 * behaviour is worth. Recorded here rather than papered over.
 *
 * Returns the slot, or -1 when the pool is full.
 */
s32 q2_fx_group_spawn_points(q2_fx_world *w,
                             const s32 (*pts)[3], const s16 (*vel)[3],
                             u32 count,
                             const q2_fx_ramp *ramp0, const q2_fx_ramp *ramp1,
                             u32 life, s32 size, u8 area);

/* ------------------------------------------------------------------------- */
/* The bullet's world impact — 0x800489D8, spawning through 0x8003004C        */
/* ------------------------------------------------------------------------- */
/*
 * What a hitscan pellet leaves on a WALL. Reached from the bullet trace at
 * 0x8004874C: 0x80048928 tests whether the sweep found an entity, 0x80048980
 * takes the blood branch and 0x80048990 this one.
 *
 * It is NOT the spark, and there is no spark. Q2_FX_WATER_SPLASH's site
 * (0x8003E0C0) lives in `GenerateWaterSplash` (QPLAYER.C, 0x8003E014), whose
 * only caller in the entire executable is 0x8003D4C4, inside the player's
 * per-frame state think — no weapon reaches it. Raising it for every bullet
 * impact is what covered the screen in pure-blue additive discs, and it was the
 * reported bug: ramp 0 is (64,64,255), which is water.
 *
 * That note used to stop one step short of naming the function and left the
 * preset with no caller at all. It has one now: the wade arm in sim.c.
 *
 * These two ramps are grey (record 6, 64/64/64, B+F) and dark red (record 4,
 * 64/32/32, B-F), striped three quads at a time — a smoke puff.
 */
#define Q2_FX_BULLET_PUFF_COUNT      15
#define Q2_FX_BULLET_PUFF_LIFE       32
#define Q2_FX_BULLET_PUFF_SIZE       4096
#define Q2_FX_BULLET_PUFF_RAMP0      6    /* 0x8009BD78 */
#define Q2_FX_BULLET_PUFF_RAMP1      4    /* 0x8009BC70 */
#define Q2_FX_BULLET_PUFF_OFFS_SHIFT 7    /* sra 7  at 0x80048A0C onward */
#define Q2_FX_BULLET_PUFF_VEL_SHIFT  11   /* sra 11 at the same site     */

s32 q2_fx_bullet_puff(q2_fx_world *w, q2_rng *rng, const s32 at[3], u8 area);

/* ------------------------------------------------------------------------- */
/* THE MESH DRAWERS — five effects that sample an actor's own posed vertices  */
/* ------------------------------------------------------------------------- */
/*
 * 0x80058C18, 0x80058DDC, 0x80058FA0, 0x80059168 and 0x8005899C/0x8005AB70.
 * None of them existed in this port; nothing drew a damage effect at all.
 *
 * They all work the same way. Take the entity's model, ask 0x8006D6AC how many
 * vertices it has in total, walk them with a STEP and a PHASE taken from the
 * engine's tick counter [0x800B2DE4], turn each sampled index into a world
 * point through 0x8006CC44, and hand a batch of at most fifteen points to
 * q2_fx_group_spawn_points. The phase is what makes the effect crawl: on
 * successive frames a different subset of the mesh lights up.
 *
 * THE PHASE IS NOT A SKIP. Reading `s4 = tick & 1` as "every other frame it
 * skips one" loses the effect: the walk STARTS at vertex (tick & 1) and steps
 * by two, so the two halves of the mesh alternate frame by frame and the
 * result is a crawl over the whole body. Same for the spark's `tick & 7`.
 *
 * THE LOOP DOES NOT STOP AT ONE BATCH. 0x80059130 branches back to 0x80059034
 * while two or more vertices remain, so a sixty-vertex soldier spawns TWO
 * groups per tick, not one. A model big enough spends several pool slots a
 * frame out of a pool of 32 (INFERRED, from the arithmetic rather than seen:
 * a crowded scene will run the pool dry and drop later bursts whole).
 *
 * VELOCITIES. The four "crackle" drawers zero all fifteen velocity triples
 * (0x80058FF8: fifteen memsets of six bytes) so the quads sit still on the
 * body. The spark draws random ones; see q2_fx_mesh_spark.
 *
 * THE VIEW MASK IS A SKIP MASK. 0x800590D8 reads the word at group+0xC4, clears
 * bits 4..7 with 0xFFFFFF0F and ORs in `((1 << p) & 0xF) << 4` where
 * `p = (client - 0x800C7C60) / 224` (0x800590E4..0x80059110 is the exact-divide
 * idiom: multiply by 0xB6DB6DB7, which is inv(7) mod 2^32, then `sra 5` — the
 * same divide by 224 playerdeath.h already uses). So the burst is hidden in the
 * viewport of the player it belongs to and visible in everyone else's: you do
 * not see your own damage crackle, you see the other player's. Pass the player
 * index as `viewport_skip`, or -1 for an actor with no client.
 *
 * The renderer's side is 0x80030614..0x80030628: `lbu +0xC4`, `srl 4`, `srav`
 * by the viewport, `andi 1` — bit 4 + n hides a group in viewport n, which is
 * exactly the nibble written here.
 */

/*
 * Where a world point comes from.
 *
 * 0x8006CC44(entity, s16 index, s32 out[3]) is the posed WORLD vertex: it walks
 * the model's 8-byte part records at obj+0x28 to find which part owns the index,
 * applies that part's live pose through 0x8006C6C8, rotates the result by the
 * ENTITY's own matrix (0x8006FC1C on entity+0x2C0) and adds entity+0xA4. A
 * negative index short-circuits to the entity origin (0x8006CC64 `bltz`). The
 * part walk only picks the POSE — the vertex itself is fetched by the original
 * global index (0x8006CCA4 uses the pre-walk value), not by the residue.
 *
 * q2_model_world_vertex supplies this lookup. The client binds posed player,
 * creature and pickup meshes through q2_fx_mesh_src. A NULL source remains
 * the original model-less branch; it must not replace an available model,
 * because that also skips the spark's 45 velocity draws from the RNG.
 *
 * `total` is 0x8006D6AC: the SUM of each part's `lbu +3` over the `lh 22(obj)`
 * records at `lw 40(obj)`, not the model header's own vertex count — the
 * original sums rather than trusting the header and the two are not guaranteed
 * equal. A NULL model gives zero (0x8006D6AC returns 0 for a null argument),
 * and every drawer below then does exactly what the console does with a
 * model-less entity, which is documented at each one.
 */
typedef void (*q2_fx_vertex_fn)(void *ctx, s32 index, s32 out[3]);

typedef struct q2_fx_mesh_src {
    q2_fx_vertex_fn vertex;   /* 0x8006CC44 */
    void           *ctx;      /* the entity it belongs to */
    u32             total;    /* 0x8006D6AC */
} q2_fx_mesh_src;

/* Every crackle site passes 32767 (0x80058FBC and 0x80059184 load it into fp
 * up front; 0x80058D04 and 0x80058EC8 load it into t0 beside the call) and
 * steps the vertex index by two (0x80059068). */
#define Q2_FX_CRACKLE_SIZE   32767
#define Q2_FX_CRACKLE_STEP   2

/*
 * The four crackle sites, as (ramp, life) pairs. They are the SAME FUNCTION
 * four times over — the compiler emitted one copy per call site and only two
 * immediates differ, the ramp pointer and the life. Every other instruction,
 * including the batch rule and the view-mask nibble, is identical.
 *
 * Because `colour = ramp[32 - life]`, the life decides where in the ramp the
 * burst starts, so these two numbers together are the whole visual. The
 * renderer reads it at 0x80030798..0x800307B4 (`addiu a0, zero, 32`, `lbu
 * -1(s3)` for the +0xC5 life, `subu`, `sll 2`, `lwl/lwr 7/4(v1)`), i.e. the
 * word at ramp + 4 + 4 * (32 - life), and the integrator only decrements the
 * life at the tail of the same draw. So a burst of life L shows entries 32 - L
 * through 31 and nothing earlier. The colours below are those entries, read
 * from the ramp bytes at their records, in R, G, B order:
 *
 *   0x80058FA0  ramp 18  life 4   the damage crackle, effect[1] == 1 or 2.
 *                                 Entries 28..31 (0x8009C41C..0x8009C428) are
 *                                 (0,255,0), (0,191,0), (0,127,0), (0,63,0):
 *                                 it starts at FULL green and fades out over
 *                                 its four ticks.
 *   0x80058DDC  ramp 17  life 1   the energy-bolt hit, effect[1] >= 3. Every
 *                                 entry of ramp 17 is (0,255,0), and life 1
 *                                 reads entry 31 alone: one tick of undimmed
 *                                 green, re-spawned every frame while the
 *                                 timer holds.
 *   0x80058C18  ramp 15  life 1   the QUAD DAMAGE shell. Life 1 reads ONLY
 *                                 entry 31 (0x8009C29C), which is (0,0,63),
 *                                 the dimmest value in the ramp; the
 *                                 (0,0,255) of entries 0..28 is never shown.
 *                                 So the shell is a single tick of dim blue,
 *                                 one frame deep, and exists only because the
 *                                 gate re-spawns it every frame.
 *   0x80059168  ramp  0  life 4   effect[5]'s crackle. Ramp 0 opens at
 *                                 (64,64,255), but life 4 shows entries 28..31
 *                                 (0x8009BAD4..0x8009BAE0), (8,8,31) fading
 *                                 to (2,2,7): a faint blue.
 *
 * The ramp indices come from the pointer immediates: 0x8009C3A8, 0x8009C324,
 * 0x8009C21C and 0x8009BA60, minus the table base 0x8009BA60, over 132 bytes
 * a record, gives 18, 17, 15 and 0.
 */
#define Q2_FX_CRACKLE_DAMAGE_RAMP  18   /* 0x80058FB4 */
#define Q2_FX_CRACKLE_DAMAGE_LIFE   4   /* 0x80059090 */
#define Q2_FX_CRACKLE_ENERGY_RAMP  17   /* 0x80058DF0 */
#define Q2_FX_CRACKLE_ENERGY_LIFE   1   /* 0x80058E68 */
#define Q2_FX_QUAD_SHELL_RAMP      15   /* 0x80058C2C */
#define Q2_FX_QUAD_SHELL_LIFE       1   /* 0x80058CA4 */
#define Q2_FX_CRACKLE_SLOT5_RAMP    0   /* 0x8005917C */
#define Q2_FX_CRACKLE_SLOT5_LIFE    4   /* 0x80059258 */

/*
 * One crackle pass. Returns how many groups it spawned.
 *
 * `frame` is the engine's tick counter [0x800B2DE4]; only its low bit is used.
 * A NULL or empty `src` returns 0 without touching the pool or the generator,
 * which is what the console does too: 0x8006D6AC hands back zero for a
 * model-less entity, `s3 = 0 - parity` is below two, and 0x80059028 leaves.
 */
u32 q2_fx_mesh_crackle(q2_fx_world *w, const q2_fx_mesh_src *src,
                       u32 frame, u8 area, s32 viewport_skip,
                       u8 ramp, u8 life);

/* ------------------------------------------------------------------------- */
/* The mesh spark — 0x8005899C, ramp 14, every EIGHTH vertex                  */
/* ------------------------------------------------------------------------- */
/*
 * Three byte-identical wrappers reach it — 0x8005B624, 0x8005B658 and
 * 0x8005B68C — and they exist only because the compiler emitted one per call
 * site. All three pass a1 = a2 = 0x8009C198 (ramp 14), a3 = 32767 and a fifth
 * argument of 4.
 *
 * Ramp 14 opens at (255,255,125) and passes through orange, but the colour a
 * burst shows is ramp[32 - life] (see the crackle table above), and at life 4
 * that is entries 28..31, 0x8009C20C..0x8009C218: (96,16,16), (76,16,16),
 * (56,16,16) and (36,16,16). So the spark is a DARK-RED ember that dims over
 * four ticks. The yellow and orange part of the ramp is never reached.
 *
 * THAT 4 IS THE LIFE, NOT A QUAD COUNT. It lands at the wrapper's sp+16, which
 * 0x80058B2C reads back as sp+0x170 and 0x80058B4C forwards to 0x8002FDFC's
 * sp+20, which 0x8003001C stores at +0xC5. The burst is up to FIFTEEN quads
 * sourced from the mesh, not four. Every downstream reading of "a four-particle
 * burst" inherits the same misread.
 *
 * Both ramp arguments are the SAME pointer, which matters: the renderer swaps
 * its two ramps every three quads (0x80030A14), so a site that wanted stripes
 * would pass two. This one deliberately does not.
 *
 * THE VELOCITY DRAW is the expensive part and the part that has to be exact,
 * because the generator is shared. Fifteen quads, THREE draws each, 45 per
 * burst, in this order (0x80058A08..0x80058AB4):
 *
 *     A = rand()                     ; the delay slot at 0x80058A14 uses A
 *     B = rand()
 *     angle = A & 0xFFF              ; 4096-step circle
 *     mag   = 32 + ((12 * (B - 16384)) >> 13)      ; ARITHMETIC shift, floor
 *     vel.x = f(sin12(angle) * mag)
 *     C = rand()
 *     vel.y = (C - 16384) >> 10                    ; arithmetic, -16..15
 *     vel.z = f(cos12(angle) * mag)
 *
 * `mag` is 32 +/- 24, giving [8, 55]. `sll 1; addu; sll 2; sra 13` is
 * (x*12)>>13 — dropping the final `sll 2` and reading it as (x*3)>>13 gives
 * [26, 37], a burst with a quarter of the console's spread in speed.
 *
 * f(t) is 0x80058A4C..0x80058A64: `mult t, 0x057619F1 / mfhi / sra 8 / subu
 * (t>>31)`, i.e. the top half of a 64-bit product shifted a further eight, then
 * the sign correction — the compiler's whole signed magic-division sequence.
 *
 * IT IS A DIVIDE BY 12000, and this had to be re-derived because an earlier
 * reading of the constant was wrong. 0x057619F1 is 91625969 decimal, and
 * ceil(2^40 / 12000) is 91625969 exactly; 91625969 * 12000 overshoots 2^40 by
 * 224, which is the residue a magic for 12000 at shift 40 is supposed to have.
 * The misread value, 91760113, would have implied a divisor of 11982.457 and
 * no integer at all — and "no integer divisor fits" was then taken as proof of
 * a hand-rolled fixed-point scale. The bytes at 0x800589FC (`lui s6, 0x576`)
 * and 0x80058A00 (`ori s6, s6, 0x19F1`) settle it.
 *
 * `t / 12000` in C, truncating toward zero, was measured against the multiply
 * over every t in +/-300000 (the site's own reach is +/-225280, from a 1.3.12
 * sine times a magnitude of at most 55) and agrees on all 600001 of them. It is
 * still written as the multiply below, because that is the instruction sequence
 * and the equivalence is a measurement rather than a definition.
 *
 * These are VELOCITIES, not offsets: they go into 0x8002FDFC's a1 array, which
 * is the velocity block. The POINTS all come from the mesh.
 */
#define Q2_FX_MESH_SPARK_RAMP    14      /* 0x8005B698, 0x8009C198          */
#define Q2_FX_MESH_SPARK_SIZE 32767      /* 0x8005B6A0                      */
#define Q2_FX_MESH_SPARK_LIFE     4      /* 0x8005B690, the fifth argument  */
#define Q2_FX_MESH_SPARK_STEP     8      /* 0x80058B10                      */
#define Q2_FX_MESH_SPARK_MAG      32     /* 0x80058A38                      */
#define Q2_FX_MESH_SPARK_MAG_MUL  12     /* 0x80058A1C..0x80058A24          */
#define Q2_FX_MESH_SPARK_MAG_SHIFT 13    /* 0x80058A28                      */
#define Q2_FX_MESH_SPARK_Y_SHIFT  10     /* 0x80058A74                      */
#define Q2_FX_MESH_SPARK_RECIP 0x057619F1 /* 0x800589FC/0x80058A00, ceil(2^40/12000) */
#define Q2_FX_MESH_SPARK_DIV      12000   /* what that magic divides by      */

/* The scale f(t) above, exposed so a test can measure it rather than restate
 * it: `(t * 0x057619F1) >> 40` with the round-toward-zero correction. */
s32 q2_fx_spark_scale(s32 t);

/* The magnitude `mag` above for a raw draw `b` in 0..32767, exposed for the
 * same reason. [8, 55]; the *3 misreading would give [26, 37]. */
s32 q2_fx_spark_mag(s32 b);

/*
 * One spark pass. Returns how many groups it spawned.
 *
 * A model-less entity returns 0 AND DRAWS NOTHING FROM `rng`: 0x800589E0 tests
 * entity+0x10 and leaves before the velocity loop. That ordering is why this
 * takes the mesh before the generator.
 */
u32 q2_fx_mesh_spark(q2_fx_world *w, q2_rng *rng, const q2_fx_mesh_src *src,
                     u32 frame, u8 area, s32 viewport_skip,
                     const q2_fx_ramp *ramp0, const q2_fx_ramp *ramp1,
                     s32 size, u8 life);

/* ------------------------------------------------------------------------- */
/* The mesh blood spray — 0x8005AB70, and its two wrappers                    */
/* ------------------------------------------------------------------------- */
/*
 * The odd one out of the family: it walks EVERY vertex in order, fifteen at a
 * time, `total / 15` groups' worth (0x8005ABBC, the signed divide by 15 with
 * magic 0x88888889), and it draws its fifteen velocities ONCE up front and
 * reuses them for every group.
 *
 * Its fifth argument picks between two whole personalities:
 *
 *   mode != 0   life 10, velocities `(rand() - 16384) >> 10`, and the returned
 *               group gets accel[1] = 3 written on top (0x8005AD48 `sh 3,
 *               98(v1)`) — so the spray SAGS, the same way the four sites named
 *               at the top of this header do.
 *   mode == 0   life 32, velocities `3 * (rand() - 16384)` rounded TOWARD ZERO
 *               by 16384 (0x8005AC58 `bgez; addiu 16383; sra 14`), no accel.
 *
 * Its two callers are 0x8005B320 — ramps 2 and 3 (the blood pair), size 6144,
 * mode 1, and the first thing a gib throw does (0x8005A440) — and 0x8005B6C0 —
 * ramps 10 and 0, size 6144, mode 0, called twice from inside the item think
 * 0x80059330 (0x800598CC, 0x8005998C) after its live-player bounds test at
 * 0x8005984C. The FIFTH argument of 0x8005B320 is that `1` at sp+16, the
 * mode: reading it as a life gives a one-tick spray where the console's lives
 * ten ticks and sags.
 *
 * A MODEL-LESS ENTITY STILL COSTS 45 DRAWS HERE, unlike the spark: 0x8005ABB4
 * calls 0x8006D6AC, gets zero, computes `s3 = 0/15 = 0`, draws the fifteen
 * velocity triples anyway, decrements s3 to -1 and only then finds the loop
 * already over. Reproduced, because the generator is shared and a divergence
 * here moves every later draw.
 *
 * Unlike the crackle and the spark this one writes NO view mask — there is no
 * nibble block in 0x8005AB70 at all — so a blood spray is visible in every
 * viewport including its own.
 */
#define Q2_FX_MESH_BLOOD_RAMP0     2    /* 0x8005B330, 0x8009BB68 */
#define Q2_FX_MESH_BLOOD_RAMP1     3    /* 0x8005B334, 0x8009BBEC */
#define Q2_FX_MESH_BLOOD_SIZE   6144    /* 0x8005B338             */
#define Q2_FX_MESH_BLOOD_LIFE     10    /* 0x8005ABE0, mode != 0  */
#define Q2_FX_MESH_BLOOD_ACCEL_Y   3    /* 0x8005AD44             */
#define Q2_FX_MESH_BLOOD_ALT_LIFE 32    /* 0x8005AC34, mode == 0  */
#define Q2_FX_MESH_BLOOD_BATCH    15    /* 0x8005ACFC             */

u32 q2_fx_mesh_blood(q2_fx_world *w, q2_rng *rng, const q2_fx_mesh_src *src,
                     u8 area, const q2_fx_ramp *ramp0, const q2_fx_ramp *ramp1,
                     s32 size, s32 mode);

/*
 * 0x8005B320, the gib throw's opening spray: q2_fx_mesh_blood on ramps 2 and 3,
 * size 6144, mode 1. `area` is the entity's +0x9E byte, which 0x8005AD28 reads
 * for every group. ThrowGibs (0x8005A3D4) calls it once, at 0x8005A440, after
 * its starting-yaw draw and its `4096 / count` and before the first chunk.
 */
u32 q2_fx_gib_spray(q2_fx_world *w, q2_rng *rng, const q2_fx_mesh_src *src,
                    u8 area);

/* ------------------------------------------------------------------------- */
/* The gib's blood trail — the particle half of the gib think 0x80059DE0      */
/* ------------------------------------------------------------------------- */
/*
 * A thrown gib (0x8005A0AC and 0x8005AD8C both install 0x80059DE0 at +0x3C)
 * lays a line of blood behind itself every tick. The think is an entity's and
 * belongs beside the other thrown models; the burst it raises is a particle
 * group like every other in this file, so it lives here and the think calls it
 * with the gib's position (entity+0xA4, just refreshed from +0x54 at
 * 0x80059E78) and velocity (+0xE0..+0xE4).
 *
 *   velocity   base[k] = (s16)(4 * vel[k] - 8192), then fifteen triples of
 *              `(rand() + base[k]) >> 11`, x/y/z in that order — 45 draws per
 *              gib per tick, so a sixteen-piece boss burst costs 720 a tick
 *   offsets    step[k] = vel[k] / 15 (truncating); offs[0] = 0 and
 *              offs[i] = offs[i-1] + step — the gib's own path for the frame
 *   spawn      0x8003004C, which stores each offset >> 4
 *
 * The think skips all of this when bit 0x20 of entity+0x98 is set (0x80059E8C)
 * and moves the gib afterwards (0x8005A088, 0x80054DD4); both are the caller's.
 * Returns the slot, or -1 when the pool is full.
 */
#define Q2_FX_GIB_TRAIL_RAMP0     2      /* 0x8005A078, 0x8009BB68 */
#define Q2_FX_GIB_TRAIL_RAMP1     3      /* 0x8005A050, 0x8009BBEC */
#define Q2_FX_GIB_TRAIL_COUNT    15      /* 0x8005A058             */
#define Q2_FX_GIB_TRAIL_LIFE      3      /* 0x8005A060             */
#define Q2_FX_GIB_TRAIL_SIZE   6144      /* 0x8005A068             */
#define Q2_FX_GIB_TRAIL_BIAS  (-8192)    /* 0x80059EA8             */
#define Q2_FX_GIB_TRAIL_SHIFT    11      /* 0x80059EF0             */
#define Q2_FX_GIB_TRAIL_DIV      15      /* 0x80059F4C, 0x88888889 */

s32 q2_fx_gib_trail(q2_fx_world *w, q2_rng *rng, const s32 at[3],
                    const s16 vel[3], u8 area);

/* ------------------------------------------------------------------------- */
/* 0x80048588 — the energy bolt's trail                                        */
/* ------------------------------------------------------------------------- */
/*
 * A BLASTER BOLT'S ONLY BODY.
 *
 * The missile sweep's per-tick arms are all gated on one halfword, the flags at
 * record+0x22 that the spawner writes from its fifth argument (0x8004D7BC).
 * Five bits, five arms:
 *
 *     0x01  0x800482B0   THIS — a particle group, every tick
 *     0x02  0x800481C0   the dynamic light (0x80075C34)
 *     0x04  0x80047F44   the eight-corner body (0x800B1E28)
 *     0x08  0x80048660   the impact burst
 *     0x10  0x80048238   a second, larger light — no caller on this disc sets it
 *
 * and the three callers of the spawner pass exactly two values: 11 for the
 * blaster (0x8004C11C) and for a monster's (0x800620D4), 14 for the
 * hyperblaster (0x8004D3E8) and for a monster's hyper variant (0x800620BC,
 * chosen by `andi v0, 0x40` at 0x800620A8). 11 has bit 0x1 and not 0x4; 14 has
 * 0x4 and not 0x1. So the two are MUTUALLY EXCLUSIVE: the blaster's bolt is
 * this trail and nothing else, and the hyperblaster's is the box and has no
 * trail. A port that drew the box for both and trailed neither had them
 * exactly the wrong way round.
 *
 * WHAT THE ARM BUILDS, 0x800482B8..0x800485DC, per tick:
 *
 *     disp      = velocity * the frame's dt   (0x80047D50, [0x800B2DB4])
 *     origin    = pos - disp/2                (0x80048328 `sra 17`)
 *     step      = (disp << 4) / record+0x4A   (0x80048384 `sra 12`, then div)
 *     offs[0]   = -(disp/2)                   (0x80048434)
 *     offs[i]   = offs[i-1] + step            (0x800484A8)
 *     vel[0]    = disp >> 4                   (0x80048364 `sra 20`)
 *     vel[i]    = (disp >> 4) + ((rand() - 16384) >> 12)   three draws an axis
 *
 * and hands them to the offsets spawner with count = record+0x4A, both ramps
 * 0x8009BF04 and the operands below (0x80048550..0x8004858C). `offs[0]` is
 * discarded by that spawner — particle 0 IS the origin — so the first entry
 * exists to seed the chain and nothing else.
 *
 * The outer loop runs record+0x42 times, a counter clamped to record+0x3A; the
 * spawner writes 1 there for every caller (0x8004D7B4), so it is always one
 * pass and the counter is a mechanism with no live second case on this disc.
 * Modelled as one pass, with the clamp stated rather than reproduced.
 */
#define Q2_FX_BOLT_TRAIL_COUNT    6      /* record+0x4A, 0x8004D7A0   */
#define Q2_FX_BOLT_TRAIL_LIFE    23      /* 0x8004856C                */
#define Q2_FX_BOLT_TRAIL_SIZE  8192      /* 0x80048574                */
#define Q2_FX_BOLT_TRAIL_RAMP     9      /* 0x8009BF04, both ends     */
#define Q2_FX_BOLT_TRAIL_SHIFT   12      /* 0x80048500, the jitter    */

/*
 * `vel` is the bolt's velocity already multiplied by the frame's dt — the
 * `disp` above — because that is what the sweep has in hand when it builds
 * this and the caller is the only one who knows the tick length.
 */
s32 q2_fx_bolt_trail(q2_fx_world *w, q2_rng *rng, const s32 at[3],
                     const s16 disp[3], u8 area);

/* ------------------------------------------------------------------------- */
/* The rail's spiral — the tail of AnyoneFireInstantRail, 0x8004917C          */
/* ------------------------------------------------------------------------- */
/*
 * The railgun's trail, and it is particles: the three `jal 0x8003004C` at
 * 0x800497B8, 0x800497F8 and 0x80049964 are the offsets spawner, and nothing
 * else in the function draws. The port resolved the rail's damage and left the
 * rest of the function out, so a railgun shot was a sound and a hole.
 *
 * WHAT IT BUILDS, after the damage pass, from the trace's start and end:
 *
 *     len    = SquareRootNR(|end - start|^2)            0x800493B4
 *     segs   = (len >> 10) + 1, at least 1              0x80049420
 *     step   = (end - start) / segs                     0x8004943C..0x800494A4
 *     per    = 30 single-player, 15 in a multiplayer    0x800493C8: [0x800AEBCC]
 *     turns  = 1, or 2 with three or four windows       0x800493E0: [0x800B2C2C]
 *     offs[i]= i * ((step << 4) / per)                  halfword sums, 0x800495E4
 *     ring   = rcossin(i * (turns << 12) / per)         0x80049678, x cos, y sin
 *     vel[i] = shooter matrix * ring, Y negated         0x800496B0, 0x800496C4
 *     offs[i] += vel[i] >> 2;  vel[i] >>= 10            0x800496D4, 0x80049724
 *
 * so one segment's particles start on a ring 64 units out (4096 >> 2, then the
 * spawner's >> 4) around the beam, one full turn of it spaced along 1024 units,
 * and drift outward at four a tick: a corkscrew, perpendicular to where the
 * SHOOTER faces rather than to the beam — the matrix is entity+0x2C0.
 *
 * THE SPAWN LOOP COVERS ROUGHLY HALF THE BEAM, and that is the console's. It
 * runs `for (n = 0; n < (segs & ~1); n += 2)`, advancing the origin ONE segment
 * per pass (0x80049800..0x80049838), then spawns once more where it stopped
 * (0x80049964). So a two-segment shot is drawn whole, and a long one from the
 * muzzle for segs / 2 + 1 segments. Each pass raises the first fifteen
 * particles and, at thirty, the second fifteen as a group of their own
 * (0x800497C4); the last raises only the first fifteen, at size 6000 rather
 * than 7000. Every group is ramp 0 at both ends, the blue one, for 16 ticks.
 *
 * `basis` is the shooter's matrix's first two columns with the Y row negated,
 * in 1.12: what `ring` (1, 0) and (0, 1) become. `area_at` names the draw area
 * for a segment's origin — the console walks the trace's own area list to the
 * last entry behind it (0x80049850..0x80049914), which is the area of the cell
 * holding that point.
 */
#define Q2_FX_RAIL_SEG_SHIFT     10     /* 0x80049420 `sra fp, v1, 10`      */
#define Q2_FX_RAIL_PER_SEG       30     /* 0x800493F8, single-player        */
#define Q2_FX_RAIL_PER_SEG_MP    15     /* 0x800493D0, [0x800AEBCC] != 0    */
#define Q2_FX_RAIL_MP_TWO_TURNS   3     /* 0x800493E0, `slti v0, v0, 3`     */
#define Q2_FX_RAIL_GROUP         15     /* sp+20 at all three spawns        */
#define Q2_FX_RAIL_LIFE          16     /* sp+24                            */
#define Q2_FX_RAIL_SIZE        7000     /* 0x800497A4                       */
#define Q2_FX_RAIL_SIZE_LAST   6000     /* 0x80049958                       */
#define Q2_FX_RAIL_RAMP           0     /* 0x8009BA60, both ends            */
#define Q2_FX_RAIL_RING_SHIFT     2     /* 0x800496D4 `sra 18` of a halfword */
#define Q2_FX_RAIL_DRIFT_SHIFT   10     /* 0x80049728 `sra 26` of a halfword */

u32 q2_fx_rail_trail(q2_fx_world *w, const s32 start[3], const s32 end[3],
                     const s16 basis[2][3], bool multiplayer, int windows,
                     u8 (*area_at)(void *user, const s32 at[3]), void *user);

/* ------------------------------------------------------------------------- */
/* The per-actor presentation pass — 0x8005B880                               */
/* ------------------------------------------------------------------------- */
/*
 * Six calls in a fixed order, and the order and the second arguments are the
 * behaviour:
 *
 *     8005B894  jal 0x80075E14(entity, 7)     ; the ambient fade, lighting.c
 *     8005B8A0  lh  s1, 264(s0)               ; entity+0x108, health
 *     8005B8A8  jal 0x80058638(entity, 1)     ; effect[1], and it ticks ITSELF
 *     8005B8AC  slt s1, zero, s1              ; (delay slot) dt_alive, taken
 *                                             ; BEFORE 0x80058638 runs
 *     8005B8B4  jal 0x8005B6F4(entity, s1)    ; effect[0] -> mesh spark
 *     8005B8C0  jal 0x8005B744(entity, s1)    ; effect[2] -> mesh spark
 *     8005B8CC  jal 0x8005B794(entity, 1)     ; effect[4] -> mesh spark
 *     8005B8D4  jal 0x8005B7E4(entity)        ; the quad shell
 *     8005B8E0  jal 0x8005B830(entity, 1)     ; effect[5] -> crackle ramp 0
 *
 * EACH TICKER IS `if (slot) { emit(); slot -= dt; }` (0x8005B708..0x8005B72C
 * and its three clones). The emit is unconditional on the timer being non-zero
 * and the decrement uses the CALLER's dt, so:
 *
 *   - slots 0 and 2 get dt = (health > 0). On a DEAD body zero is subtracted
 *     every tick, so an armed effect[0] or effect[2] does not run down at all:
 *     the spark keeps firing for as long as the body is presented.
 *   - slots 4 and 5 get a literal 1 (0x8005B8D0, 0x8005B8E4 — and the same in
 *     the dissolve handlers at 0x8005B3EC/0x8005B400 and 0x8005B494/
 *     0x8005B4A8), so they run down whether the entity is alive or not.
 *
 * WHAT ENDS A SPARKING CORPSE is not this chain; it is the corpse think's gate
 * 0x8005B2A8. Both corpse thinks call it BEFORE they would call 0x8005B880 —
 * the player's 0x8003E238 at 0x8003E244, the creature's 0x8007F71C at
 * 0x8007F728 — and skip the chain when it returns non-zero:
 *
 *     8005B2B4  lbu  v0, 754(a0)     ; effect[2] set -> think = 0x8005B444
 *     8005B2D4  lbu  v0, 752(a0)     ; else effect[0] set -> think = 0x8005B39C
 *     8005B2EC  sw   v0, 60(a0)      ; (either arm) install it at +0x3C,
 *     8005B2F4  sh   v0, 244(a0)     ; +0xF4 = 4096 (0x8005B2F0 addiu),
 *     8005B304  jal  0x8007F288      ; on the +0x2EC record, and return 1
 *
 * (0x8007F288 zeroes that record's +0x24 and clears bit 0x20000000 of its
 * +0x1C.) The two handlers are byte-for-byte the same code. Each runs the
 * whole chain above, dt_alive included, and then drains +0xF4 by
 * `[0x800B2DB4] << 6` a frame (0x8005B408..0x8005B418, 0x8005B4B0..
 * 0x8005B4C0), freeing the entity through 0x8006D280 once the halfword is no
 * longer positive (0x8005B420 `bgtz`, 0x8005B428). So on the console a corpse
 * with effect[0] or effect[2] armed sparks for ceil(4096 / (64 * dt)) more
 * frames, six at the nominal dt of 12, and is then GONE. It does not spark
 * for the rest of the level. effect[1], [4] and [5] do not trigger the swap;
 * they run down on their own.
 *
 * This function is the chain only. The gate, the swap and the free belong to
 * the corpse's owner, and are modelled there: q2_monster_corpse_tick
 * (0x8007F728) and q2_player_death_tick_fx (0x8003E244) run 0x8005B2A8 on
 * the body's own effect bytes before anything else.
 *
 * Reading the health gate as applying to all four is still a real mistake:
 * it would latch effect[4] and effect[5] on a dying body, where the console
 * runs them down.
 *
 * effect[3] (+0x2F3): `q2psx-inspect access 0x2F3` finds no load or store
 * whose immediate is 0x2F3 anywhere in SLES_015.34, so nothing here reads or
 * decrements it. That is a measurement of the MAIN EXECUTABLE's immediate-
 * offset accesses only: the tool does not scan the relocated creature and
 * level modules, and it cannot see base+index addressing. No module is known
 * to touch the byte, and 0x8005B880 certainly does not, which is all this
 * function needs. effect[5] (+0x2F5) IS real: 0x8005B844 reads it and
 * 0x8005B868 writes it back.
 *
 * THE QUAD SHELL'S GATE (0x8005B7E4) is five instructions: entity+0x0C must be
 * non-null (0x8005B7EC, so an actor with NO CLIENT never gets a shell however
 * its inventory reads), then client+0xAC against the level clock at
 * [0x800AEBAC] with `sltu` (0x8005B80C) — an UNSIGNED compare, and strict, so
 * `level_time == quad_until` is already over.
 *
 * WHAT THIS DOES NOT DO. The two lights and the ambient write that 0x80058638
 * raises belong to lighting.c, so they are REPORTED rather than raised: fill in
 * a q2_fx_present_report and let the driver act on it. That keeps effect.c
 * presentation-only, which is the shape the rest of this module already has.
 */

/*
 * 0x80058718 `addiu a3, zero, 4` — how many ticks the energy pulse's timed
 * light lives. The colour and the two radii are the same words the `>= 3` arm
 * uses, 0x800AEAAC and 0x800AEAB0/0x800AEAB2, which combat.h already names
 * Q2_ENERGY_LIGHT_*; only the lifetime is new, so only the lifetime is here.
 */
#define Q2_ENERGY_PULSE_TICKS 4

typedef struct q2_fx_present_report {
    /*
     * 0x800586D0 — 0x80075C34 with colour 0x800AEAAC and the radii at
     * 0x800AEAB0/0x800AEAB4. combat.h already names it Q2_ENERGY_LIGHT_*.
     * Raised on the effect[1] >= 3 arm only.
     */
    bool energy_light;

    /*
     * 0x80058758 — a DIFFERENT function, 0x80075D14, with the same colour, the
     * radii at 0x800AEAB0 and a fourth argument of 4. Raised only when
     * effect[1] is EXACTLY 2. This arm was missed entirely by the port's
     * `effect[1] >= 3` reading, which treated the `< 3` branch as silent.
     *
     * 0x80075D14 IS NOW NAMED: AddTimedLight, and its fourth argument is a
     * LIFETIME IN TICKS (lighting.h). So the two arms are not two spellings of
     * the same light. The `>= 3` arm's 0x80075C34 raises one green light for
     * one frame at 800/1300; this arm parks a slot that re-emits the SAME
     * colour at 800/1300 scaled 4/4, 3/4, 2/4, 1/4 over the four frames that
     * follow — the tail of the glow, which is why it fires on the tick the
     * bigger crackle stops. Q2_ENERGY_PULSE_TICKS below is that 4.
     *
     * The caller raises it with q2_light_add_timed; this only reports that the
     * site fired, because the light lists are lighting.c's.
     */
    bool energy_pulse;

    /*
     * 0x800586E8 — the four bytes at 0x800AEAAC copied into entity+0x2AC, so
     * the actor's own ambient colour becomes the light's colour for that tick.
     * Only on the >= 3 arm.
     */
    bool set_ambient;

    u32  groups;    /* how many pool slots the whole pass took */
} q2_fx_present_report;

struct q2_actor;

/*
 * 0x80058638. Runs the effect[1] three-way gate, spawns whichever crackle the
 * arm calls for, reports the lights, and — this is the part the port did not
 * have anywhere — DECREMENTS effect[1] BY `dt` ITSELF, but only when the slot
 * was non-zero on entry (0x80058658 leaves through the exit that skips the
 * subtraction).
 *
 * `a` is a q2_actor; combat.h owns it and this only touches `effect[1]`.
 */
void q2_fx_actor_damage_effect(q2_fx_world *w, struct q2_actor *a,
                               const q2_fx_mesh_src *src,
                               u32 frame, u8 area, s32 viewport_skip,
                               u8 dt, q2_fx_present_report *out);

/*
 * The whole 0x8005B880 chain. `src` may be NULL for an actor with no model, in
 * which case every drawer takes the console's own model-less path (see each
 * one). `out` may be NULL.
 *
 * `frame` is the frame counter [0x800B2DE4], +1 a frame, never the level
 * clock. `viewport_skip` and `quad_until` are the actor's CLIENT's, and the
 * console reads both off the entity it is presenting, whoever that is. Every
 * drawer writes the view nibble for any entity whose +0x0C client is non-null
 * (0x800590C0 `lw v1, 12(s5)` / 0x800590C8 `beq`), from that client's own
 * index (0x800590D0..0x80059110, the divide of client - 0x800C7C60 by 224).
 * The shell reads the same entity's client+0xAC (0x8005B7EC, 0x8005B7FC). So a
 * player, live OR parked, passes its own player index and its own inventory's
 * deadline, and only an actor with no client passes -1 and 0.
 *
 * The ambient fade at 0x80075E14 is NOT run here — lighting.c already owns it
 * and it is the driver's to call in the same place.
 */
void q2_fx_actor_present(q2_fx_world *w, q2_rng *rng, struct q2_actor *a,
                         const q2_fx_mesh_src *src,
                         u32 frame, u8 area, s32 viewport_skip,
                         s32 level_time, s32 quad_until,
                         q2_fx_present_report *out);

/*
 * One tick of the integrator, exactly as the tail of 0x800304A8 runs it:
 *
 *     life--
 *     origin += vel
 *     vel    += accel
 *     offset[i] += rel_vel[i]      for i in 1..count-1
 *
 * Note the order: the position uses the velocity BEFORE the acceleration is
 * applied, so a group is always one tick behind a naive integrator. That is
 * visible on the long-lived bursts and is reproduced rather than corrected.
 *
 * The original runs this inside the draw, once per frame rather than once per
 * viewport. Splitting it out is what lets a headless caller advance effects
 * without a screen.
 */
void q2_fx_tick(q2_fx_world *w);

/* Where quad `i` of a group is this tick, in world units. */
void q2_fx_group_point(const q2_fx_group *g, u32 i, s32 out[3]);

/* The colour word a group reads this tick from ramp `which` (0 or 1). */
u32 q2_fx_group_colour(const q2_fx_group *g, u32 which);

/* ------------------------------------------------------------------------- */
/* Beams                                                                      */
/* ------------------------------------------------------------------------- */
/*
 * Queue one beam for this frame. 0x80064E64. Returns false when the 32-slot
 * pool is full, which is what the original returns too — the caller is expected
 * to carry on regardless.
 */
bool q2_fx_beam_add(q2_fx_world *w,
                    const s32 from[3], const s32 to[3],
                    s16 radius, s16 area,
                    const q2_fx_face *tube,
                    const q2_fx_face *cap_near,
                    const q2_fx_face *cap_far);

/* The same, naming a style out of the tables. */
bool q2_fx_beam_add_style(q2_fx_world *w,
                          const s32 from[3], const s32 to[3],
                          s16 radius, s16 area, u32 style);

/* Empty the beam pool. 0x80064EF4, and the tail of 0x80064F10. */
void q2_fx_beams_reset(q2_fx_world *w);

/*
 * Hold a persistent beam between two things — the BFG's, and the mechanism for
 * any weapon trail.
 *
 * `owner` and `target` are the caller's ids for the two ends. 0x80049D30
 * searches the list for a record whose stored pair matches and REFRESHES it
 * rather than allocating a second, so a ball that can see one creature holds
 * one beam on it however many frames it is in view. Passing a fresh pair every
 * frame would fill all twelve slots in twelve frames and then stop drawing.
 *
 * `life` is in the same units the frame delta is subtracted in; pass
 * Q2_FX_TIMED_BEAM_LIFE for the BFG's own 45. Returns false when the list is
 * full of live beams, which the original also just tolerates.
 */
/*
 * `area` is the byte 0x80048D24's helper would have produced for the owner —
 * the cell the beam's origin sits in. It is stored on the record and spent on
 * every submit; see q2_fx_timed_beam.area.
 */
bool q2_fx_beam_timed(q2_fx_world *w, s32 owner, s32 target,
                      const s32 from[3], const s32 to[3],
                      s16 radius, u32 style, s16 life, u8 area);
/* 0x8004E998: style 4, radius 10, life 45, keyed on owner/target/vertex.
 * The host updates posed endpoints and their clipped area before drawing. */
bool q2_fx_beam_attached(q2_fx_world *w, s32 owner, s32 target, s16 vertex,
                         const s32 from[3], const s32 to[3], u8 area);

/*
 * Age the timed list by one frame's worth. 0x80048CE8 subtracts the frame delta
 * and clamps at zero — it does not free the slot, so a record stays matchable
 * by its owner/target pair until something else claims it.
 */
void q2_fx_timed_tick(q2_fx_world *w, s32 frame_delta);

/* How many timed beams are still alive. */
u32 q2_fx_timed_live(const q2_fx_world *w);

/*
 * The twelve hull vertices of a beam, in world units.
 *
 * 0x800634E4 builds an orthonormal basis around the beam's direction, then
 * walks the 4096-step circle in thirds — three axes at 120 degrees — and emits
 * each axis and its negation, giving a hexagon. The near hexagon is written to
 * out[0..5] and the far one to out[6..11].
 *
 * Returns false when `from` and `to` coincide, which is when the basis cannot
 * be built; the original bails at 0x8006350C for the same reason and draws
 * nothing.
 */
bool q2_fx_beam_hull(const q2_fx_beam *b, s32 out[Q2_FX_BEAM_VERTS][3]);

/* ------------------------------------------------------------------------- */
/* Debris                                                                     */
/* ------------------------------------------------------------------------- */
/* Register a debris model. 0x80064F70: appends, capped at 32, silently. */
bool q2_fx_debris_register(q2_fx_world *w, s16 model);

/*
 * The burst at 0x80064558.
 *
 * `bmin`/`bmax` are the Scene node's bounding box. When `at` is non-NULL every
 * piece starts there; when it is NULL each piece starts at a uniformly random
 * point inside the box — which is how a shattering window scatters along its
 * whole surface rather than out of its centre.
 *
 * Each piece's velocity is drawn as
 *
 *     x = (3 * (rand() - 16384)) >> 5                 in -1536..1535
 *     y = (3 * (rand() - 16384)) >> 5  - 1536         biased upward
 *     z = (3 * (rand() - 16384)) >> 5
 *
 * The Y bias is the whole reason debris arcs: -1536 is up in this engine's
 * frame. Its model is picked uniformly from the registration list.
 *
 * Returns how many pieces were actually spawned, which is less than `count`
 * when the pool fills.
 */
u32 q2_fx_debris_burst(q2_fx_world *w, q2_rng *rng,
                       const s32 bmin[3], const s32 bmax[3],
                       const s32 *at, u32 count, u8 area);

/*
 * WHICH HULL DEBRIS MOVES IN, and it is not the player's.
 *
 * The think hands each piece to `0x80046DA0`, which parks `0x800C8E90` —
 * PrimaryColl — in the mover's hull slot and the entity's `+0xA0` in its node
 * slot, then calls the shared mover. Its twin at `0x80046DDC` is the one that
 * uses SecondaryCol and `+0xA2`, and that is the player's path.
 *
 * So debris is traced against the UNERODED hull with its own cell index, while
 * the player is traced against the hull eroded by 286 (sim.h). Using the
 * player's hull for debris would stop every shard 286 units short of the floor
 * and leave it hanging in mid-air — which reads as a physics bug rather than a
 * hull mix-up.
 *
 * The radius is `a2` at the call site: 2048.
 *
 * GRAVITY IS READ, not modelled. The shared mover applies it at 0x80046464:
 *
 *     if (ent+0x10C & 0x2000) skip          ; a per-entity no-gravity flag
 *     vel.y += [0x800AE924] * frame_delta   ; the SAME global the player uses
 *     if (vel.y > 8192) vel.y = 8192        ; terminal velocity
 *
 * Three things follow that a plausible model would have got wrong: the step is
 * gravity times the FRAME DELTA rather than a flat per-tick add; the fall speed
 * is capped, so a shard dropped down a lift shaft stops accelerating; and the
 * value is the simulation's own, which is why `q2_fx_debris_step_one` takes it
 * from the caller instead of keeping a second copy that could drift from the
 * player's when the GAME VARIABLES menu changes it (sim.h).
 */
#define Q2_FX_DEBRIS_RADIUS 2048

/* 0x80046490. The downward speed a falling entity is clamped to. */
#define Q2_FX_TERMINAL_VELOCITY 8192

/* 0x80046458. Set in ent+0x10C, this suppresses gravity entirely. */
#define Q2_FX_ENT_NO_GRAVITY 0x2000u

/* What a debris piece wants to do this tick. Same contract as q2_proj_step:
 * the caller traces `from`..`to` and either commits or reports an impact. */
typedef struct q2_fx_debris_step {
    s32  from[3];
    s32  to[3];
    bool expired;
} q2_fx_debris_step;

void q2_fx_debris_step_one(q2_fx_world *w, u32 index, s32 gravity,
                           q2_fx_debris_step *out);
void q2_fx_debris_commit(q2_fx_world *w, u32 index, const s32 to[3]);

/*
 * Resolve an impact. 0x80064228: a piece that hits something advances its spin
 * by one and loses 300 of its lifetime, and is freed when that reaches zero. It
 * does NOT bounce off geometry in the original — it slides, because it is
 * handed to the shared mover — so this stops it at the contact point.
 */
void q2_fx_debris_impact(q2_fx_world *w, u32 index, const s32 point[3]);

/* ------------------------------------------------------------------------- */
/* Glints                                                                     */
/* ------------------------------------------------------------------------- */
/*
 * The colour band, 0x80064780.
 *
 * A glint is a fixed mesh whose vertices carry a surface coordinate in
 * their Z. The renderer lights a band of it and slides the band along as the
 * entity's counter runs down:
 *
 *     centre = (width / 4) * (4 - phase) + 1024
 *     d      = |vertex.z - centre|
 *     w      = (d >= width) ? 0 : (width - d) * phase
 *     rgb    = (tint * w) >> 15,  tint = (channel << 13) / width
 *
 * `width` is the caller's 8192 or 4096 and `phase` the entity's own countdown.
 * Writing it out matters because the shape is not obvious from the arithmetic:
 * the band is triangular, its brightness scales with the phase, and the two
 * `width` uses do not cancel — halving the width both narrows the band and
 * doubles the tint.
 *
 * `vertex_band` is the FOURTH halfword of each mesh vertex, not its Z. See the
 * GlintMod notes below.
 *
 * Fills `out_rgb[i]` for each of `count` vertices.
 */
void q2_fx_glint_shade(const s16 *vertex_band, u32 count,
                       const u8 tint_rgb[3], s32 width, s32 phase,
                       u8 (*out_rgb)[3]);

/* ------------------------------------------------------------------------- */
/* The glint mesh — the `GlintMod` chunk                                      */
/* ------------------------------------------------------------------------- */
/*
 * The mesh is not in the executable, and for a while that looked like a dead
 * end. It is a NAMED LEVEL CHUNK: `0x8007AB44` looks up "GlintMod" in the
 * .DAT directory and hands the chunk pointer to `0x800651BC`, which splits it
 * at a fixed offset and parks the halves at `gp+0x4614` and `gp+0x4610`.
 *
 *     [0, 864)      216 faces, four u8 indices each, in perimeter order
 *     [864, end)    (size - 864) / 8 vertices: s16 x, y, z, band
 *
 * The split is a literal 864 (`addiu v0, a0, 864`), not a header field, so a
 * chunk of any size has its faces in the first 864 bytes and its vertices after.
 *
 * THE FOURTH HALFWORD IS THE BAND COORDINATE, not padding. `0x80064938` reads
 * `+6` off each source vertex — the slot an SVECTOR normally wastes — and that
 * is what the travelling band is measured along. On BIGGUN it starts at 1024 on
 * the tip vertex and rises with distance, and 1024 is exactly the constant the
 * band's centre is offset by (`0x800648EC`), which is the check that says the
 * field was read right rather than plausibly.
 *
 * Reading the Z instead gets you a band that sweeps the wrong axis and looks
 * like a lighting bug.
 *
 * `GlintMod` is OPTIONAL and present on exactly ONE map of the forty-nine —
 * BIGGUN — where the chunk is 2,608 bytes: 216 quads over 218 vertices, with
 * the highest index reaching exactly 217. So the glint is one effect on one
 * level, not a general weapon trail, and a port that never loads BIGGUN will
 * never draw one.
 */
#define Q2_FX_GLINT_INDEX_BYTES 864
#define Q2_FX_GLINT_FACE_COUNT  (Q2_FX_GLINT_INDEX_BYTES / 4)   /* 216 */

/*
 * What one call to the renderer can actually cover. `0x800649C8` transforms
 * vertices in threes while its counter is under 94 — 96 vertices — into the
 * shared scratch at 0x800B2DFC, and `0x80064BC4` stops after 79 faces visited,
 * including the black-face skip at 0x80064A38. Both are literal bounds, so the port honours them
 * rather than drawing the whole 216-face mesh and calling it an improvement.
 */
#define Q2_FX_GLINT_MAX_FACES   79

/*
 * Decode a `GlintMod` chunk. `data`/`size` are the chunk's own bytes; the mesh
 * points into them, so they must outlive it.
 *
 * Fails for anything at or under 864 bytes, which is a chunk with no vertices —
 * the split is unconditional in the original and would hand the renderer a
 * pointer past the end.
 */
bool q2_fx_glint_mesh_decode(q2_fx_glint_mesh *out, const u8 *data, u32 size);

/*
 * One glint, appended to `ot`.
 *
 * `origin` and `yaw` place the mesh; `tint_rgb` and `width` are the entity's
 * own colour and band width (`0x04000000` entities carry them at +0x2B4 and
 * are handed 8192 or 4096); `phase` is the countdown at +0x2BE, which the
 * caller decrements — 0x80064D3C does it in the draw, but doing it there would
 * make a two-viewport frame advance the band twice.
 *
 * Returns the number of primitives emitted, and zero for a NULL or empty mesh
 * rather than inventing one.
 */
u32 q2_fx_glint_build_ot(const q2_fx_glint_mesh *mesh,
                         const s32 origin[3], s32 yaw,
                         const u8 tint_rgb[3], s32 width, s32 phase,
                         const q2_camera *cam, psx_ot *ot, gte_state *gte);

/* ------------------------------------------------------------------------- */
/* The named effects                                                          */
/* ------------------------------------------------------------------------- */
/*
 * Every burst in the game is `q2_fx_group_spawn` with a different set of five
 * numbers. They are immediates inside their spawn sites, so they are
 * transcribed with the address each came from rather than read from a table.
 *
 * `spread_shift` is the shift applied to `rand() - 16384` when drawing each
 * quad's velocity, so a smaller shift is a wider burst: shift 9 gives
 * components in -32..31, shift 10 gives -16..15.
 */
typedef enum q2_fx_preset_id {
    Q2_FX_EXPLOSION = 0,   /* 0x800486EC — grenades, rockets, dying monsters */
    Q2_FX_BLOOD,           /* 0x80048C08 — the only two-ramp effect          */
    Q2_FX_BFG_BURST,       /* 0x8004BDC4 — the biggest quads in the game     */
    Q2_FX_ITEM_MATERIALISE,/* 0x800596B0 — an ITEM fading in, not a gib      */
    Q2_FX_TELEPORT,        /* 0x80028DC8 — the UserFuncs effect primitive    */
    Q2_FX_WATER_SPLASH,    /* 0x8003E0C0 — wading in, four bursts of fifteen */
    Q2_FX_LASER_END,       /* 0x80049074 — both ends of a laser beam         */
    Q2_FX_PRESET_COUNT
} q2_fx_preset_id;

typedef struct q2_fx_preset {
    u8   count;          /* quads in the burst                        */
    u8   life;           /* ticks, and therefore where the ramp starts */
    s32  size;           /* world size before size_scale               */
    u8   spread_shift;
    u8   ramp0;          /* ARRAY index into the ramp table            */
    u8   ramp1;

    /*
     * HOW MANY GROUPS THE SITE SPAWNS.
     *
     * Six of the eight sites of 0x80030284 sit inside an outer loop that
     * re-draws all fifteen velocities and spawns again, and this table used to
     * have no column for it — so the port emitted between a half and a quarter
     * of the original's burst density, and every seeded replay diverged after
     * the first burst because each repeat consumes 45 more `rand()` draws.
     *
     *     0x800486F4  slti v0, s7, 2   explosion  2
     *     0x80048C24  slti v0, s2, 2   blood      2
     *     0x8004BDD0  slti v0, s3, 3   BFG        3
     *     0x8003E0DC  slti v0, s2, 4   spark      4
     *     0x800596B0                   item mat.  1 (falls through, no loop)
     *
     * LASER_END is 1 here on purpose even though its two sites (0x80049090 and
     * 0x8004913C) both loop four times: q2_fx_laser spawns its four groups
     * itself and never goes through q2_fx_spawn. Putting 4 in this row would be
     * dead today and a sixteen-group bug the moment anyone routed the laser
     * through the generic path.
     */
    u8   repeat;

    /*
     * The Y ACCELERATION the site writes into the group after the spawner
     * returns — `sh v0, 98(v1)`, accel[1]. Zero for the sites that do not.
     * See the record layout at the top of this header.
     */
    s16  accel_y;

    u32  site;           /* the spawn site this was read out of        */
} q2_fx_preset;

const q2_fx_preset *q2_fx_preset_at(q2_fx_preset_id id);

/*
 * Spawn a named effect at `at`. Draws `count` velocities from `rng` with the
 * preset's shift, exactly as the spawn sites do, and hands them to
 * q2_fx_group_spawn. Returns the slot or -1.
 */
s32 q2_fx_spawn(q2_fx_world *w, q2_rng *rng, q2_fx_preset_id id,
                const s32 at[3], u8 area);

/*
 * The teleport column (0x80028DC8) is the one preset whose life and size are
 * not constant: life is `(rand() + 24) & 15` and size is
 * `(rand() & 0x2047) + 12000`. Both draws happen before the velocities, so the
 * generator's sequence matters; q2_fx_spawn reproduces the order.
 */

/* ------------------------------------------------------------------------- */
/* DrawTelePorts — 0x80028BDC, one of QuakeLoop's three per-frame draws        */
/* ------------------------------------------------------------------------- */
/*
 * A TELEPORT PAD IS MARKED BY A COLUMN, and this port drew nothing.
 *
 * `DrawTelePorts` (EVENT.C, named by the NTSC symbol table) is called from
 * QuakeLoop at 0x80038FA4, immediately before `DrawLaserWalls` (0x8002A02C)
 * and `DrawLaserBeams` (0x8002EE38) — the third of three unconditional
 * per-frame walks, and the only one this port had no counterpart for. Its list
 * is `TelePortData` at 0x800C6F44 with `NumTelePorts` at gp+16940, built at
 * zone load by `PSXTELEPORT_T` (0x8002E3F0), which is the fn_load half of the
 * TELEPORT binding record at 0x8009B9C0.
 *
 * Each frame it draws FIFTEEN velocity triples ONCE (0x80028C34), then walks
 * the list. Per pad it takes the volume's bbox centre in X and Z, the mins or
 * maxs face in Y depending on the direction of travel, jitters X and Z by
 * ±127, draws a lifetime and a size, and spawns one fifteen-quad group on
 * ramps 1 (red) and 0 (blue) with the volume's own area byte. Then it adds a
 * pulsing blue light at the same place (0x80028E6C, AddLight).
 *
 * Split into four pieces here because the pieces belong to two modules: the
 * walk needs the map's trigger volumes and lives in levelbin.c, while the
 * draws have to stay in the order the console makes them or a seeded replay
 * diverges on the first frame.
 */

/* +1 while the column streams up, -1 while it falls. 0x80028C14. */
s32 q2_fx_teleport_sign(s32 level_time);

/* The fifteen shared velocities for this frame. 0x80028C34..0x80028C9C. */
void q2_fx_teleport_velocities(q2_rng *rng, s32 sign,
                               s16 vel[Q2_FX_GROUP_QUADS][3]);

/*
 * One pad's group. `centre` is (x centre, the chosen Y face, z centre); the
 * ±127 jitter, the lifetime and the size are drawn here because they are drawn
 * per pad on the console, in that order, out of the same generator.
 *
 * Returns the slot, or -1 when the pool is full — which the console's walk
 * ignores and carries on from, so this one does too.
 */
s32 q2_fx_teleport_column(q2_fx_world *w, q2_rng *rng,
                          const s16 vel[Q2_FX_GROUP_QUADS][3],
                          const s32 centre[3], u8 area);

/* The outer radius of the pad's light this frame, 800..1824. 0x80028E1C. */
s32 q2_fx_teleport_light_radius(s32 level_time);

/* The inner radius, and the light's colour and type word. 0x80028E14 writes
 * 400; the colour is 0x00FF1020 at 0x800AE798 (r 32, g 16, b 255) and the
 * type word 0x00020001 at 0x800AE79C — style 1, size shift 2. */
#define Q2_FX_TELEPORT_LIGHT_INNER  400
#define Q2_FX_TELEPORT_LIGHT_R       32
#define Q2_FX_TELEPORT_LIGHT_G       16
#define Q2_FX_TELEPORT_LIGHT_B      255
#define Q2_FX_TELEPORT_LIGHT_STYLE    1
#define Q2_FX_TELEPORT_LIGHT_SIZE     2

/* ------------------------------------------------------------------------- */
/* 0x800596B0 is the ITEM MATERIALISE burst, and its ramp is an ITEM's glow    */
/* ------------------------------------------------------------------------- */
/*
 * THIS ROW WAS NAMED WRONG, and the wrong name carried a wrong story with it.
 *
 * 0x800596B0 sits inside 0x80059330, which is unambiguously the item think:
 * 0x800595AC advances entity+0xFC by 2*[0x800B2DB4] and 0x800595C8 clamps it at
 * 4096, which is the materialise SCALE ramp item.c already models; 0x800595DC
 * writes 127 into entity+0x2AC..0x2AE the moment it reaches full size. The
 * fifteen velocity triples at 0x80059608 are `(rand() - 16384) >> 9` into the
 * fixed block at 0x800D4B24, and the spawn at 0x800596B0 hands them over with
 * count 15, life 10, size 10000 and area entity+0x9E. The numbers in the table
 * were right; only the name and the ramp rule were wrong.
 *
 * The ramp comes from bits of `s3`, and `s3` is `lw 68(s2)` — entity+0x44, the
 * ITEM's own flag word. The instruction immediately after the spawn,
 * 0x800596B8 `andi v0, s3, 0x70`, is the glow-light test that item.c already
 * cites at its own glow block, and 0x10/0x20/0x40 are Q2_ITEM_GLOW_R/G/B in
 * src/build/itemtable.h. So this is an item's glow colour, not a creature's
 * blood colour, and there is no creature blood table on the disc at all.
 *
 * WHERE THE REAL GIB BLOOD IS: 0x8005A3D4 opens a gib throw with 0x8005B320,
 * which is the MESH blood spray on ramps 2 and 3 at size 6144 — see
 * q2_fx_mesh_blood above — and the gib entity's own per-tick trail is
 * 0x8003004C on the same two ramps. Neither is a point burst, so nothing was
 * lost by renaming this one: `xrefs 0x80030284` gives eight sites and every
 * other one is already a named preset here.
 *
 * The selection chain has no final else (0x80059648 / 0x8005965C / 0x80059670
 * each `beq` forward to the next test, and the last falls through to the spawn
 * with s4 untouched). An item with none of the three bits reaches the spawn
 * with the ramp register still holding whatever it last held, which is an
 * engine defect of the same kind as the uninitialised T_Damage argument in
 * userfuncs.h. `q2_fx_item_glow_ramp` returns ramp 1 in that case and says so;
 * the alternative is not reproducible.
 */
#define Q2_FX_ITEM_GLOW_R  0x10u   /* 0x80059648 -> ramp 1  (0x8009BAE4) */
#define Q2_FX_ITEM_GLOW_G  0x20u   /* 0x8005965C -> ramp 11 (0x8009C00C) */
#define Q2_FX_ITEM_GLOW_B  0x40u   /* 0x80059670 -> ramp 0  (0x8009BA60) */

/* Which ramp an item with these glow bits materialises in. */
u8 q2_fx_item_glow_ramp(u32 item_flags);

/*
 * The materialise burst. `item_flags` is the item's entity+0x44 word; only bits
 * 0x10/0x20/0x40 are read. Consumes 45 draws whatever it returns, because
 * 0x80059608 runs before the ramp is chosen.
 */
s32 q2_fx_item_materialise(q2_fx_world *w, q2_rng *rng, const s32 at[3],
                           u8 area, u32 item_flags);

/* ------------------------------------------------------------------------- */
/* The laser                                                                  */
/* ------------------------------------------------------------------------- */
/*
 * 0x80048DC8. Queues the beam, reports the damage the caller should apply
 * along it, and spawns a burst at each end.
 *
 * The original traces for a victim itself and calls T_Damage; here the trace
 * and the damage stay with the caller, because this module has no collision
 * hull and should not grow one. `out_damage` and `out_mod` carry what to apply.
 *
 * `ends` selects which end bursts fire, matching the two independent halfwords
 * the fifth argument packs (0x80048FF0 and 0x8004909C test them separately):
 * bit 0 is the `from` end, bit 1 the `to` end. Each end that fires spawns FOUR
 * groups of fifteen, so a laser with both ends lit costs eight pool slots.
 *
 * Returns false for an unknown kind, in which case nothing was queued.
 */
typedef struct q2_fx_laser_result {
    bool queued;        /* the beam made it into the pool                */
    s16  damage;
    s16  mod;
    u32  groups;        /* how many end-burst groups were spawned        */
} q2_fx_laser_result;

#define Q2_FX_LASER_END_FROM 1u
#define Q2_FX_LASER_END_TO   2u

/* Four groups per lit end. 0x8004908C / 0x80049138 both loop `s2 < 4`. */
#define Q2_FX_LASER_END_GROUPS 4

/*
 * `area` is s16 and not u8 because the original's is: the script's own beams
 * reach here with a raw collision-node index (0x8002EEB4 loads it with `lh`),
 * and a level with more than 256 nodes would fold two rooms onto one otherwise.
 * The end bursts still take a byte, which is the original's own narrowing.
 */
bool q2_fx_laser(q2_fx_world *w, q2_rng *rng, u32 kind,
                 const s32 from[3], const s32 to[3],
                 s16 area, u32 ends, q2_fx_laser_result *out);

/* ------------------------------------------------------------------------- */
/* Drawing                                                                    */
/* ------------------------------------------------------------------------- */
/*
 * Append every live group and every queued beam to `ot`.
 *
 * The table is NOT cleared: effects share the frame's table with the world and
 * the models, which is the whole point of an ordering table and the reason a
 * spark can sort behind a crate.
 *
 * Call viewports in ascending order. Viewport zero starts the frame's shared
 * particle packet allowance; later views spend what remains (0x80030AE8).
 * `viewport` selects which view_mask bit is honoured. Beams are drawn once and
 * the pool is left alone; call q2_fx_beams_reset after the last viewport, which
 * is where 0x80064F10 does it.
 *
 * Returns the number of primitives emitted.
 */
u32 q2_fx_build_ot(q2_fx_world *w,
                   const q2_camera *cam,
                   u32 viewport,
                   psx_ot *ot,
                   gte_state *gte);

/*
 * The minimum on-screen size of a quad. 0x80030850 clamps the perspective
 * divide up to two pixels, so a distant spark never disappears — it becomes a
 * 2x2 dot and stays one. Reproducing the clamp is what stops long-range
 * gunfire from looking like it produced no effect at all.
 */
#define Q2_FX_QUAD_MIN_PIXELS 2

/*
 * The near cutoff. A group whose projected origin lands closer than this is
 * dropped whole (0x80030708). It is in GTE depth units, not world units.
 */
#define Q2_FX_NEAR_DEPTH 128

#endif /* Q2PSX_EFFECT_H */
