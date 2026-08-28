/*
 * modeldraw.h — turning a CastList model into PlayStation GPU primitives.
 *
 * The world renderer in world.c draws brush geometry, which is already in world
 * space and needs no articulation. A model is different in three ways, and all
 * three come straight from how the original does it (0x800B1F90):
 *
 *   - Vertices are PART-LOCAL. Each part carries its own rotation and
 *     translation from the animation, and parts do not inherit from each other,
 *     so the transform is v' = R(q) * v + t applied once per part.
 *   - Faces do not index the vertex array. They index a shared per-model
 *     SCRATCH WINDOW into which parts write their transformed vertices at
 *     vert_base — which is why a face can legitimately reference a vertex a
 *     different part put there.
 *   - Texturing is the world's rule with one difference: a face's CLUT index is
 *     offset by the map's clut4_count_a, because model palettes live in the
 *     second section of the CLUT array.
 *
 * This module deliberately does the transform in the same order as the original
 * rather than the order that would be natural on a PC: per part, matrix first,
 * translation into the GTE's translation registers, vertices through RTPT. The
 * point of the project is that the wobble and the sort order come out of the
 * same arithmetic, and that only holds if the arithmetic is arranged the same
 * way.
 */
#ifndef Q2PSX_MODELDRAW_H
#define Q2PSX_MODELDRAW_H

#include "gpu.h"
#include "gte.h"
#include "lighting.h"
#include "model.h"
#include "q2psx.h"
#include "world.h"

typedef struct q2_model_draw_stats {
    u32 parts;
    u32 faces_total;
    u32 faces_emitted;
    u32 faces_rejected_near;   /* GTE divide overflow: at or behind the plane */
    u32 faces_rejected_bad;    /* index outside the scratch window            */
    u32 faces_rejected_back;   /* both NCLIP halves faced away                */
    u32 ot_overflow;
    u32 faces_semi;            /* blend selector 1..4: drawn with ABE         */
    u32 shadows_emitted;       /* 0x800784CC floor-shadow POLY_FT4            */
} q2_model_draw_stats;

/* The packet template built at 0x800783B8. It samples the lower-right 16x16
 * tile of chars.lbm from texture-page slot 15 and palette id 1 from the
 * executable's built-in bank. UV endpoints are the packet's exact inclusive
 * values, not atlas dimensions. */
#define Q2_MODEL_SHADOW_VERTEX_MAX 4
#define Q2_MODEL_SHADOW_U0       224
#define Q2_MODEL_SHADOW_V0       224
#define Q2_MODEL_SHADOW_U1       239
#define Q2_MODEL_SHADOW_V1       239
#define Q2_MODEL_SHADOW_FADE     600
#define Q2_MODEL_SHADOW_CLUT_X     0
#define Q2_MODEL_SHADOW_CLUT_Y   248

/*
 * BACKFACE REJECTION for a model face, and why its sign is the WORLD'S
 * INVERTED.
 *
 * The model linker at 0x800B2410 loads the projected corners into the GTE in
 * the same shape the world linker does, and runs the same pair of NCLIPs:
 *
 *     SXY0,SXY1,SXY2 = v0,v1,v3 ; NCLIP ; MAC0 <= 0 -> draw   (0x800B24A0)
 *     SXY0           = v2       ; NCLIP ; MAC0 >  0 -> draw   (0x800B24D0)
 *                                        otherwise   -> drop
 *
 * Compare `q2_world_quad_faces_camera`, whose two tests are `MAC0 > 0 -> draw`
 * and `MAC0 < 0 -> draw` (0x800AF8A8 / 0x800AF8C8). The comparisons are exactly
 * exchanged, so MODEL QUADS ARE WOUND THE OPPOSITE WAY ROUND FROM WORLD QUADS.
 * That is not an inference from how it looks: it is `bgtz` in one linker and
 * `blez` in the other, on the identical register.
 *
 * It is also measurable, which is what settles it independently of the sign
 * convention this port's GTE happens to use. Averaging the projected depth of
 * each group over the Soldier: the faces this rule KEEPS mean 1174 and the ones
 * it drops mean 1233, and the same split holds on all fourteen of its parts.
 * The kept set is the nearer one, which is what "front facing" means.
 *
 * The corner order is the file's — the perimeter — exactly as the world's is;
 * the linker's 2/3 swap is the file-to-hardware Z-order conversion and applies
 * to the UVs in the same breath (0x8006A3E4 stores face uv3 into POLY uv2).
 *
 * RESIDUE. The linker has an escape: `bltz t3` at 0x800B2498 skips the test
 * entirely, and `t3` is a 32-bit mask shifted left once per face, so it is one
 * FORCE-DRAW BIT PER FACE, MSB first, over the same 32-face batches the
 * attribute emitter walks in. Nothing here builds that mask, so this port culls
 * unconditionally; what sets a bit is unread. See openquestions #10b.
 *
 * Clobbers the GTE's SXY registers and MAC0.
 */
Q2PSX_INLINE bool q2_model_quad_faces_camera(gte_state *g, const gte_sxy screen[4])
{
    g->sxy[0] = screen[0];
    g->sxy[1] = screen[1];
    g->sxy[2] = screen[3];
    gte_nclip(g);
    if (g->mac0 <= 0)
        return true;

    g->sxy[0] = screen[2];
    gte_nclip(g);
    return g->mac0 > 0;
}

/*
 * Where and how a model instance sits in the world.
 *
 * `pose` holds one entry per part, or NULL to draw the model unposed — which is
 * what the 1,324 static models want, and what an articulated model falls back
 * to when its animation has not been selected yet.
 */
typedef struct q2_model_instance {
    const q2_model      *model;
    const q2_model_pose *pose;        /* num_parts entries, or NULL */
    s32                  origin[3];
    s32                  yaw;         /* 4096-step circle */

    /*
     * The other two angles, for the one thing on this disc that needs them.
     *
     * Nothing placed in the world tilts: the rotation integrator writes exactly
     * one axis and the zone draw's own RotMatrix call is effectively yaw-only,
     * which is why `yaw` was enough for every caller until now. The VIEW WEAPON
     * is the exception — it is rotated by `RotMatrix(view angles + the
     * animation's own)` at 0x8004F464, all three of them, with pitch negated at
     * the sum. Leaving these zero reproduces the previous behaviour exactly.
     */
    s32                  pitch;
    s32                  roll;

    /*
     * An explicit 3x3 rotation, 1.3.12, overriding the three angles above.
     *
     * The view weapon needs it and nothing else does. Its model is authored in
     * VIEW space — `Blaster G` spans z 0..482 with the grip at the origin — so
     * the matrix it wants is the camera's own inverse composed with the clip's
     * rotation, and that is not expressible as three Euler angles the camera
     * will then re-rotate. NULL keeps the angle path exactly as it was.
     */
    const s16          (*rot)[3];
    u32                  clut4_count_a;

    /*
     * Optional port-side uniform transform, 1.0.12. This is not an entity
     * field: retail entity+0xFC/+0xFE affect only the GTE light matrix and back
     * colour. Q2_ONE_12 is unscaled; use q2_model_instance_init for it.
     */
    s32                  scale;

    /*
     * Per-vertex colour for an explicitly unlit instance. The ordinary entity
     * path instead feeds entity+0x2AC to the back colour below. On retail the
     * pool allocator starts that triplet at 0x40, the item spawner replaces it
     * with 0x30, and a materialise ramp can drive it to 127.
     */
    u8                   tint[3];

    /*
     * The lights reaching this instance, or NULL to draw it at `tint`.
     *
     * When it is present the vertices shade through the GTE's NCT path exactly
     * as the original's do: the three directions become the light matrix (
     * composed with the instance's own matrix and the part's, per part), the
     * three colours become the colour matrix, and the entity's glow becomes the
     * back colour. NCT does NOT multiply by the primitive's colour, so `tint`
     * is unused in that case — the light IS the colour, and the ambient reaches
     * the vertex through the back colour instead.
     *
     * Build one with q2_light_gather + q2_light_env_build.
     */
    const struct q2_light_env *light;

    /*
     * The engine's texture-page table, or NULL for a private one at ABR 0.
     *
     * Models and the world share it (0x800B36D8 is indexed by both emitters),
     * and the world's opaque path mutates it, so a model drawn into a world
     * frame should be handed the same q2_world_render's table. A model face's
     * own blend selector is OR-ed on top without writing back (0x8006DE50).
     */
    const q2_tpage_table *tpage;

    /*
     * Entity +0x9E: the live draw area selected from collision-cell byte +32.
     * Retail routes the model's private face chain through this area's
     * authored insertion point. Negative means no area information, preserving
     * the ordinary depth path used by offline renders and the view weapon.
     */
    s32                  sort_area;

    /*
     * Entity +0x78..+0x8F: the absolute AABB retail puts at batch record +8.
     * A live entity always supplies it. `sort_bounds_valid` is false for
     * structural/offline models; if such a caller nevertheless selects an
     * area, a degenerate box at `origin` is used rather than changing the
     * record to point type.
     *
     * Entity render flag 0x00800000 selects the area's Quick (+12) list. It
     * does NOT set record flag bit 0: models stay bounds records on either
     * list (0x8006DCD0..0x8006DD28).
     */
    s32                  sort_bounds_min[3];
    s32                  sort_bounds_max[3];
    bool                 sort_bounds_valid;
    bool                 sort_quick;

    /*
     * The floor shadow drawn immediately after this model's face chain.
     *
     * Retail starts the local X/Z footprint at +/- `shadow_radius`, then poses
     * up to four GLOBAL STORAGE vertex indices from the model wrapper's +4
     * list and expands the extrema with them. The result shrinks linearly to
     * zero at `shadow_height == 600`, is yawed, based at `shadow_origin`, and
     * textured with the packet template above. A zero radius is meaningful:
     * item spawns leave +0x94 at zero and rely entirely on their table list,
     * which is why only twelve item records cast a non-degenerate shadow.
     *
     * `shadow_enabled` mirrors entity render-flag bit 0. `shadow_clut` may be
     * overridden for another executable build; q2_model_instance_init selects
     * the catalogued PAL build's palette-1 location at (0,248).
     */
    bool                 shadow_enabled;
    const u16           *shadow_vertex;
    u32                  shadow_vertex_count;
    s32                  shadow_radius;
    s32                  shadow_height;
    s32                  shadow_origin[3];
    u16                  shadow_clut;

    /*
     * NAME THE BUCKET OUTRIGHT, or < 0 to take it from the model's own origin.
     *
     * A model is ONE THING in the ordering table, not N, and that is not a
     * simplification: the console's vertex loop (0x800B23A0) stores SXY and RGB
     * and no SZ at all, so no per-face depth exists on the console to sort by.
     * The face loop appends packet pointers to a flat array at 0x800DDDCC and
     * the tail chains every one of them into a SINGLE link point taken from the
     * entity's projected origin — `rtps` then `swc2 SZ3` at 0x8006BF34, floored
     * to 1 at 0x8006BF40. Which ORDER they go in is the model's own; see block
     * A in model.h.
     *
     * This port used to bucket per face, and the cost was not subtle. A model
     * spans a range of depths, so its faces scattered across buckets and any
     * world polygon whose depth fell between them drew after part of the model
     * and over it — no depth buffer, so the wall wins. Standing directly in
     * front of BASE1's first Soldier drew the wall and no Soldier; the menu's
     * logo did not appear at all.
     *
     * The override remains for a packet whose place is STRUCTURAL rather than a
     * depth. The view weapon is the one: it belongs in front of the world and
     * behind the status bar, and naming that bucket is what stops the gun's
     * arithmetic and the bar's from disagreeing (gpu.h).
     */
    s32                  bucket_override;
} q2_model_instance;

/* Zero the instance and set the fields whose neutral value is not zero. */
void q2_model_instance_init(q2_model_instance *inst);

/*
 * Transform one model instance and append its primitives to `ot`.
 *
 * The ordering table is NOT cleared and the GTE's projection is NOT
 * reconfigured, so a caller can build the world first and then drop models into
 * the same table — which is the only way the sort can interleave them with the
 * geometry they stand in front of.
 */
u32 q2_model_build_ot(const q2_model_instance *inst,
                      const q2_camera *cam,
                      psx_ot *ot,
                      gte_state *gte,
                      q2_model_draw_stats *stats);

#endif /* Q2PSX_MODELDRAW_H */
