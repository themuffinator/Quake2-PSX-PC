#include "modeldraw.h"
#include "effect.h"
#include "trace.h"
#include "worldscale.h"

#include "trig.h"
#include "vram.h"

#include <string.h>

/* A transformed vertex as it sits in the shared window: screen position and
 * depth, exactly what the GTE left behind. */
typedef struct scratch_vertex {
    gte_sxy xy;
    u16     z;
    bool    valid;      /* the slot was written at all — a decode question */
    bool    overflow;   /* the GTE's divide clamped; counted, never rejected */

    /*
     * The lit colour of this slot. The original's window holds it in the same
     * record — "normals run through the lighting op into the same slots"
     * (0x800B23F0's NCT writes the colour FIFO alongside the RTPT results) —
     * and keeping it here rather than recomputing per face is what makes a face
     * that borrows another part's vertex borrow that part's LIGHT too. That is
     * the format's behaviour, not an approximation of it.
     */
    psx_rgb rgb;
} scratch_vertex;

/* m = a * b, both 1.3.12, row-major. */
static void matrix_mul(s16 m[3][3], const s16 a[3][3], const s16 b[3][3])
{
    int r, c;

    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            s32 sum = (s32)a[r][0] * b[0][c] +
                      (s32)a[r][1] * b[1][c] +
                      (s32)a[r][2] * b[2][c];
            m[r][c] = (s16)(sum >> Q2_FRAC_12);
        }
    }
}

static void matrix_apply(const s16 m[3][3], const s32 v[3], s32 out[3])
{
    int r;

    for (r = 0; r < 3; r++) {
        s64 sum = (s64)m[r][0] * v[0] + (s64)m[r][1] * v[1] +
                  (s64)m[r][2] * v[2];
        out[r] = (s32)(sum >> Q2_FRAC_12);
    }
}

/*
 * The same, through the matrix's TRANSPOSE — which for a rotation is its
 * inverse.
 *
 * A model's normals are part-LOCAL and the light directions are in WORLD space,
 * so one of the two has to cross into the other's frame before they can be
 * dotted. The instance matrix carries local -> world, so the directions need
 * its inverse. Rotating them FORWARDS instead, which is what this module used
 * to do, mirrors every model's shading about its own facing: normals pointing
 * at a lamp came out black and normals pointing away came out fully lit.
 */
static void matrix_apply_t(const s16 m[3][3], const s32 v[3], s32 out[3])
{
    int r;

    for (r = 0; r < 3; r++) {
        s64 sum = (s64)m[0][r] * v[0] + (s64)m[1][r] * v[1] +
                  (s64)m[2][r] * v[2];
        out[r] = (s32)(sum >> Q2_FRAC_12);
    }
}

/*
 * Put one built primitive in its bucket.
 *
 * `bucket_override` names an ABSOLUTE bucket, not a depth: the view weapon's
 * place relative to the status bar is structural, and naming it outright is
 * what stops the two arithmetics disagreeing (gpu.h). Everything else buckets
 * by its own mean depth so it can interleave with the geometry it stands among.
 *
 * Neither passes a sort key. An unkeyed link PREPENDS, and the caller walks the
 * model's draw order front to back, so the first face of the order ends up at
 * the tail of the chain and draws last — which is the console's own chaining
 * direction (`swl t1, 2(t6)` at 0x800B2620).
 */
static void link_face(psx_ot *ot, psx_prim *prim, u32 bucket, s32 batch)
{
    if (batch >= 0)
        psx_ot_batch_link_prim(ot, batch, prim);
    else
        psx_ot_link_prim(ot, prim, bucket, PSX_OT_KEY_NONE);
}

/* Map one GLOBAL STORAGE vertex index to its owning part and apply that part's
 * live pose. This is the pair 0x8006D608 / 0x8006C6C8 used by the shadow path;
 * it deliberately ignores each part's scratch-window `vert_base`, which is a
 * face-indexing destination and not the model's storage order. */
static bool shadow_pose_vertex(const q2_model *m, const q2_model_pose *pose,
                               u32 index, s32 out[3])
{
    u32 part, cursor = 0;

    if (!m || !out)
        return false;

    for (part = 0; part < m->hdr.num_parts; part++) {
        q2_model_part p;

        if (!q2_model_get_part(m, part, &p))
            return false;
        if (index < cursor + p.num_verts) {
            q2_model_vertex v;

            if (!q2_model_get_vertex(m, index, &v))
                return false;

            if (pose) {
                s16 rot[3][3];
                s32 local[3] = { v.x, v.y, v.z };

                q2_quat_to_matrix(rot, pose[part].q);
                matrix_apply(rot, local, out);
                out[0] += pose[part].t[0];
                out[1] += pose[part].t[1];
                out[2] += pose[part].t[2];
            } else {
                out[0] = v.x;
                out[1] = v.y;
                out[2] = v.z;
            }
            return true;
        }
        cursor += p.num_verts;
    }
    return false;
}

/*
 * The instance's own model-to-world rotation, UNSCALED: the explicit matrix
 * when a caller hands one over, the yaw-only form everything placed in the
 * world uses, or the three-angle form the view weapon asks for. The yaw is
 * mirrored on the way in; q2_model_build_ot carries the reason at its call.
 *
 * One function so the draw and q2_model_world_vertex cannot disagree about
 * where a vertex is — a crackle spawned on a mesh rotated one way while the
 * mesh is drawn rotated the other would float beside the creature.
 */
static void instance_spin(const q2_model_instance *inst, s16 spin[3][3])
{
    if (inst->rot)
        memcpy(spin, inst->rot, sizeof(s16) * 9);
    else if (inst->pitch == 0 && inst->roll == 0)
        q2_rotation_yaw_pitch(spin, -inst->yaw, 0);
    else
        q2_rotation_euler(spin, inst->pitch, -inst->yaw,
                          inst->roll);
}

/* The port-side uniform scale on top of it; a no-op at Q2_ONE_12. See the
 * note in q2_model_build_ot on why it is not entity+0xFC. */
static void instance_scale(const q2_model_instance *inst, s16 spin[3][3])
{
    if (inst->scale != Q2_ONE_12) {
        s32 s = inst->scale;
        int r, c;

        if (s < 0)
            s = 0;
        if (s > 8 * Q2_ONE_12)
            s = 8 * Q2_ONE_12;

        for (r = 0; r < 3; r++)
            for (c = 0; c < 3; c++)
                spin[r][c] = (s16)(((s32)spin[r][c] * s) >> Q2_FRAC_12);
    }
}

/* ------------------------------------------------------------------------- */
/* 0x8006CC44 / 0x8006D6AC — a posed vertex in the world, and the count       */
/* ------------------------------------------------------------------------- */
u32 q2_model_total_verts(const q2_model *m)
{
    u32 part, total = 0;

    /* 0x8006D6AC `beq a0, zero` — a model-less entity has none. */
    if (!m)
        return 0;

    /* `lh 22(obj)` records at `lw 40(obj)`, summing `lbu +3` of each. A part
     * that does not decode ends the walk; on retail data none fails. */
    for (part = 0; part < m->hdr.num_parts; part++) {
        q2_model_part p;

        if (!q2_model_get_part(m, part, &p))
            break;
        total += p.num_verts;
    }
    return total;
}

bool q2_model_pose_rest(const q2_model *m, q2_model_pose *pose, u32 max)
{
    q2_model_anim clip;
    u32 within = 0;

    if (!m || !pose || m->hdr.num_parts > max)
        return false;
    return q2_model_anim_at_held(m, 0, &clip, &within, NULL) &&
           q2_model_pose_at(m, &clip, within, pose) == Q2_OK;
}

bool q2_model_world_vertex(const q2_model_instance *inst, s32 index,
                           s32 out[3])
{
    s32 posed[3];

    if (!inst || !out)
        return false;

    /* 0x8006CC64 `bltz a1` -> 0x8006CD98: the three words at +0xA4. */
    if (index < 0) {
        out[0] = inst->origin[0];
        out[1] = inst->origin[1];
        out[2] = inst->origin[2];
        return true;
    }

    /* 0x8006CC80's part walk and 0x8006C6C8's pose, by GLOBAL storage index
     * — the pair the item shadow already uses. */
    if (!shadow_pose_vertex(inst->model, inst->pose, (u32)index, posed))
        return false;

    q2_model_local_to_world(inst, posed, out);
    return true;
}

void q2_model_local_to_world(const q2_model_instance *inst,
                             const s32 local[3], s32 out[3])
{
    s16 spin[3][3];
    int r;

    if (!inst || !local || !out)
        return;

    instance_spin(inst, spin);
    instance_scale(inst, spin);

    /*
     * 0x8006FC1C, read rather than assumed to be the GTE: three `mult`/`mflo`
     * pairs summed in 32 bits per row, `sra 12`, and each row STORED AS A
     * HALFWORD (0x8006FD14 `sh`) before 0x8006CD58 reloads it with `lh` and
     * adds the origin. So the rotated offset wraps at sixteen bits rather than
     * saturating. No model on the disc reaches that; the cast is the store.
     */
    for (r = 0; r < 3; r++) {
        /* Formed wide and brought back to 32 bits by conversion, which is the
         * `mflo`/`addu` wrap without signed overflow in C. */
        s64 wide = (s64)spin[r][0] * local[0] +
                   (s64)spin[r][1] * local[1] +
                   (s64)spin[r][2] * local[2];
        s32 sum  = (s32)(u32)(u64)wide;

        out[r] = inst->origin[r] + (s32)(s16)(sum >> Q2_FRAC_12);
    }
}

/* The part that owns a storage vertex, and the raw vertex — 0x8006CA44's walk
 * (`a1 -= lbu 3(part)` until it goes negative). */
static bool vertex_part(const q2_model *m, u32 index, u32 *part_out,
                        s32 raw[3])
{
    u32 part, cursor = 0;

    if (!m)
        return false;
    for (part = 0; part < m->hdr.num_parts; part++) {
        q2_model_part p;

        if (!q2_model_get_part(m, part, &p))
            return false;
        if (index < cursor + p.num_verts) {
            q2_model_vertex v;

            if (!q2_model_get_vertex(m, index, &v))
                return false;
            raw[0] = v.x;
            raw[1] = v.y;
            raw[2] = v.z;
            *part_out = part;
            return true;
        }
        cursor += p.num_verts;
    }
    return false;
}

bool q2_model_attach_blend(const q2_model *m, s32 vert_a, s32 vert_b,
                           q2_model_attach *out)
{
    s32 a[3], b[3];
    u32 pa, pb;
    int k;

    if (!m || !out || vert_a < 0 || vert_b < 0)
        return false;
    if (!vertex_part(m, (u32)vert_a, &pa, a) ||
        !vertex_part(m, (u32)vert_b, &pb, b))
        return false;

    /* 0x8006CBA4: one part, the midpoint; two parts, the first alone. */
    if (pa == pb)
        for (k = 0; k < 3; k++)
            a[k] = (a[k] + b[k]) / 2;

    for (k = 0; k < 3; k++)
        out->off[k] = (s16)a[k];
    out->part = (u16)pa;
    return true;
}

void q2_model_attach_pose(const q2_model_instance *inst,
                          const q2_model_attach *a, s32 out[3])
{
    s32 p[3];
    int k;

    for (k = 0; k < 3; k++)
        p[k] = a->off[k];

    if (inst->pose && inst->model && a->part < inst->model->hdr.num_parts) {
        s16 rot[3][3];

        q2_quat_to_matrix(rot, inst->pose[a->part].q);
        matrix_apply(rot, p, out);
        out[0] += inst->pose[a->part].t[0];
        out[1] += inst->pose[a->part].t[1];
        out[2] += inst->pose[a->part].t[2];
    } else {
        out[0] = p[0];
        out[1] = p[1];
        out[2] = p[2];
    }
}

bool q2_model_blend_local(const q2_model_instance *inst, s32 vert_a,
                          s32 vert_b, s32 out[3])
{
    q2_model_attach a;

    if (!inst || !out ||
        !q2_model_attach_blend(inst->model, vert_a, vert_b, &a))
        return false;
    q2_model_attach_pose(inst, &a, out);
    return true;
}

/* 0x800784CC — one modulated, subtractive POLY_FT4 under a model. */
static bool emit_shadow(const q2_model_instance *inst, const q2_camera *cam,
                        psx_ot *ot, gte_state *gte, const s16 view[3][3],
                        u32 bucket, s32 batch, q2_model_draw_stats *stats)
{
    s32 min_x, max_x, min_z, max_z;
    s32 factor;
    s16 yaw[3][3];
    s32 base_offset[3], camera_space[3];
    psx_prim *prim;
    u32 i, count;
    static const u8 uv[4][2] = {
        { Q2_MODEL_SHADOW_U0, Q2_MODEL_SHADOW_V0 },
        { Q2_MODEL_SHADOW_U1, Q2_MODEL_SHADOW_V0 },
        { Q2_MODEL_SHADOW_U0, Q2_MODEL_SHADOW_V1 },
        { Q2_MODEL_SHADOW_U1, Q2_MODEL_SHADOW_V1 }
    };

    if (!inst->shadow_enabled || inst->shadow_height >= Q2_MODEL_SHADOW_FADE)
        return false;

    min_x = min_z = -inst->shadow_radius;
    max_x = max_z =  inst->shadow_radius;

    count = inst->shadow_vertex_count;
    if (count > Q2_MODEL_SHADOW_VERTEX_MAX)
        count = Q2_MODEL_SHADOW_VERTEX_MAX;
    for (i = 0; i < count; i++) {
        s32 v[3];

        if (!inst->shadow_vertex || inst->shadow_vertex[i] == 0xFFFFu)
            break;
        if (!shadow_pose_vertex(inst->model, inst->pose,
                                inst->shadow_vertex[i], v))
            continue;
        if (v[0] < min_x) min_x = v[0];
        if (v[0] > max_x) max_x = v[0];
        if (v[2] < min_z) min_z = v[2];
        if (v[2] > max_z) max_z = v[2];
    }

    /* The signed divide-by-600 sequence at 0x800786B0..0x80078764. Use a
     * wider product to avoid C overflow while retaining truncation toward zero. */
    factor = Q2_MODEL_SHADOW_FADE - inst->shadow_height;
    min_x = (s32)(((s64)min_x * factor) / Q2_MODEL_SHADOW_FADE);
    max_x = (s32)(((s64)max_x * factor) / Q2_MODEL_SHADOW_FADE);
    min_z = (s32)(((s64)min_z * factor) / Q2_MODEL_SHADOW_FADE);
    max_z = (s32)(((s64)max_z * factor) / Q2_MODEL_SHADOW_FADE);
    if (min_x >= max_x || min_z >= max_z)
        return false;

    prim = psx_ot_alloc(ot);
    if (!prim) {
        if (stats)
            stats->ot_overflow++;
        return false;
    }

    /* Retail applies yaw to each local corner first, stores the s16 result,
     * then applies the view transform based at entity+0x2C. Keep that
     * intermediate rounding instead of collapsing the two matrices. */
    q2_rotation_yaw_pitch(yaw, -inst->yaw, 0);
    base_offset[0] = inst->shadow_origin[0] - cam->pos[0];
    base_offset[1] = inst->shadow_origin[1] - cam->pos[1];
    base_offset[2] = inst->shadow_origin[2] - cam->pos[2];
    matrix_apply(view, base_offset, camera_space);
    {
        gte_matrix gm;
        memcpy(gm.m, view, sizeof(gm.m));
        gte_set_rotation(gte, &gm);
    }
    gte_set_translation(gte, camera_space[0], camera_space[1],
                        camera_space[2]);

    /* libgpu Z order, exactly as the four packet fields are filled:
     * maxX/maxZ, minX/maxZ, maxX/minZ, minX/minZ. */
    for (i = 0; i < 4; i++) {
        s32 local[3], rotated[3];
        gte_sxy xy;
        u16 z;

        local[0] = (i & 1u) ? min_x : max_x;
        local[1] = 0;
        local[2] = (i & 2u) ? min_z : max_z;
        matrix_apply(yaw, local, rotated);
        (void)gte_project_point(gte, (s16)rotated[0], (s16)rotated[1],
                                (s16)rotated[2], &xy, &z);
        prim->xy[i].x = xy.x;
        prim->xy[i].y = xy.y;
        prim->uv[i].u = uv[i][0];
        prim->uv[i].v = uv[i][1];
    }

    prim->kind             = PSX_PRIM_FT4;
    prim->rgb[0].r         = 128;
    prim->rgb[0].g         = 128;
    prim->rgb[0].b         = 128;
    prim->tpage            = psx_make_tpage(0, 1, PSX_BLEND_SUB,
                                             PSX_TEX_4BIT);
    prim->clut             = inst->shadow_clut;
    prim->semi_transparent = true;
    prim->textured_blend   = true;
    prim->quad_zorder      = true;

    /* The shadow call follows model drawing and AddPrim prepends it, so it is
     * visited first and the model paints over it. */
    link_face(ot, prim, bucket, batch);
    if (stats)
        stats->shadows_emitted++;
    return true;
}

void q2_model_instance_init(q2_model_instance *inst)
{
    if (!inst)
        return;
    memset(inst, 0, sizeof(*inst));
    inst->scale   = Q2_ONE_12;
    inst->tint[0] = inst->tint[1] = inst->tint[2] = 128;
    inst->sort_area       = -1;
    inst->bucket_override = -1;
    inst->shadow_clut = psx_make_clut(Q2_MODEL_SHADOW_CLUT_X,
                                      Q2_MODEL_SHADOW_CLUT_Y);
}

void q2_model_shadow_ground(q2_model_instance *inst,
                            const q2_collision *primary,
                            const q2_collision *secondary,
                            const q2_move_world *movers,
                            const s32 origin[3], bool grounded)
{
    q2_collision query;
    const q2_collision *hull = secondary;
    s32 node = -1, end[3], hit[3];
    s32 floor_offset = Q2_EYE_BASE;
    q2_coll_plane plane;

    if (!inst || !origin)
        return;
    inst->shadow_enabled = false;
    memcpy(inst->shadow_origin, origin, sizeof(inst->shadow_origin));
    inst->shadow_origin[1] += Q2_EYE_BASE;
    inst->shadow_height = 0;
    if (grounded) {
        inst->shadow_enabled = true;
        return;
    }

    if (hull)
        node = q2_coll_find_node(hull, origin, -1, true);
    if (node < 0) {
        hull = primary;
        floor_offset = 0;
        if (hull)
            node = q2_coll_find_node(hull, origin, -1, true);
    }
    if (hull && node >= 0) {
        query = *hull;
        memcpy(end, origin, sizeof(end));
        end[1] += 3 * Q2_BODY_RADIUS;
        if (!q2_coll_move(&query, origin, end, node, hit, &node) && node >= 0 &&
            query.hit_plane_index >= 0 &&
            q2_collision_get_plane(&query, (u32)query.hit_plane_index, &plane) &&
            plane.ny > 0) {
            inst->shadow_origin[1] = hit[1] + floor_offset;
            inst->shadow_enabled = true;
        }
    }

    /* Mover boxes are physical surfaces, so cast from the feet without hull
     * inflation. A lift above the map floor wins; liquid/trigger volumes are
     * excluded by q2_move_clip_segment's solid-entity gate. */
    if (movers) {
        q2_move_seg_hit contact;
        s32 feet[3] = { origin[0], origin[1] + Q2_EYE_BASE, origin[2] };
        const s32 inflate[3] = {0, 0, 0};
        memcpy(end, feet, sizeof(end));
        end[1] += 3 * Q2_BODY_RADIUS;
        /* Start just above a contact: a ray beginning exactly on the box
         * top is classified as inside and has no entry normal. */
        feet[1]--;
        if (q2_move_clip_segment(movers, feet, end, inflate, &contact) &&
            contact.normal[1] > 0 &&
            (!inst->shadow_enabled || contact.pos[1] < inst->shadow_origin[1])) {
            inst->shadow_origin[1] = contact.pos[1];
            inst->shadow_enabled = true;
        }
    }
    inst->shadow_height = inst->shadow_origin[1] - origin[1] - Q2_EYE_BASE;
}

u32 q2_model_build_ot(const q2_model_instance *inst,
                      const q2_camera *cam,
                      psx_ot *ot,
                      gte_state *gte,
                      q2_model_draw_stats *stats)
{
    scratch_vertex window[Q2_MODEL_SCRATCH_MAX];
    s16 view[3][3], world[3][3], spin[3][3], spin_unscaled[3][3];
    q2_tpage_table private_tpage;
    const q2_tpage_table *tpage;
    const q2_model *m;
    u8  tint[3];
    u32 part, face_index = 0, emitted = 0, vertex_cursor = 0;

    /*
     * One slot per face: the primitive built for it, linked afterwards in the
     * order block A or block B names (model.h).
     */
    psx_prim *built[Q2_MODEL_MAX_FACES];
    u16       order[Q2_MODEL_MAX_FACES];
    u32       built_count = 0;
    u32       bucket = 0;
    s32       batch = PSX_OT_BATCH_INVALID;
    bool      reorder;

    if (!inst || !inst->model || !cam || !ot || !gte)
        return 0;

    m = inst->model;

    if (stats)
        memset(stats, 0, sizeof(*stats));

    memset(window, 0, sizeof(window));
    memset(built, 0, sizeof(built));

    reorder = (m->hdr.num_faces <= Q2_MODEL_MAX_FACES);

    /* Share the world's table when the caller has one; otherwise a private copy
     * at ABR 0, which is what a page starts at before any opaque world polygon
     * has promoted it. */
    tpage = inst->tpage;
    if (!tpage) {
        q2_tpage_table_init(&private_tpage);
        tpage = &private_tpage;
    }

    /* The same basis the world is drawn with, roll included — models and brush
     * geometry must lean together or the two separate as soon as you strafe. */
    q2_rotation_view_anamorphic(view, cam->yaw, cam->pitch, cam->roll);

    /* 0x8006BEB0 calls the retail screen-record selector before either the
     * origin or a vertex is projected.  Models in a portal area therefore use
     * that area's local GTE origin and are clipped by the matching DRAWENV. */
    q2_camera_apply_area_projection(cam, ot, inst->sort_area, gte);

    /*
     * A MODEL IS ONE THING IN THE TABLE, and this port used to make it N.
     *
     * The console gives an entity a SINGLE ordering-table link point taken from
     * its own projected origin — `rtps` on the origin and `swc2 SZ3` at
     * 0x8006BF34, floored to 1 at 0x8006BF40 — and chains every one of the
     * model's packets into that one point (0x800B25E0). Its vertex loop stores
     * SXY and RGB and no SZ at all (0x800B23A0), so there is no per-face depth
     * on the console to sort by even in principle.
     *
     * Bucketing per face instead is what put monsters UNDER the world. A
     * creature spans a range of depths, so its faces landed in a spread of
     * buckets, and any wall polygon whose own depth fell between them was drawn
     * after part of the creature and over it — which on a table with no depth
     * buffer means the wall wins. Standing directly in front of BASE1's first
     * Soldier drew the wall behind it and no Soldier at all.
     *
     * So: one bucket, from the origin, for the whole model. The origin is run
     * through the GTE exactly as a vertex is — camera-space translation, a zero
     * vector, RTPS — so its depth comes back in the same units every other
     * caller of q2_ot_bucket_for_depth uses, rather than in a second arithmetic
     * that has to be kept in step with the first.
     */
    if (inst->bucket_override >= 0) {
        /*
         * ...unless the caller names the bucket outright. The view weapon's
         * place is structural — it belongs in front of the world and behind the
         * status bar, not at a depth — and naming it is what stops the two
         * arithmetics disagreeing (gpu.h).
         */
        bucket = (u32)inst->bucket_override;
    } else {
        s32 offset[3], camera_space[3];
        u16 otz;

        offset[0] = inst->origin[0] - cam->pos[0];
        offset[1] = inst->origin[1] - cam->pos[1];
        offset[2] = inst->origin[2] - cam->pos[2];
        matrix_apply(view, offset, camera_space);

        gte_set_translation(gte, camera_space[0], camera_space[1],
                            camera_space[2]);
        gte->v[0].x = 0;
        gte->v[0].y = 0;
        gte->v[0].z = 0;
        gte_rtps(gte, false);

        otz = gte->sz[3];
        if (otz == 0)
            otz = 1;              /* the console's own floor, 0x8006BF40 */

        /*
         * TWO STEPS, and collapsing them is what drew a creature behind the
         * wall it stood in front of.
         *
         * `q2_ot_bucket_for_depth` does not return a bucket despite its name —
         * it maps a depth onto the table's own OTZ scale, where LARGER is
         * nearer. `psx_ot_depth_bucket` is what turns that into a bucket, and
         * it INVERTS: the walk draws bucket 0 first, so the farthest primitive
         * has to sit in the lowest bucket. Handing the first function's result
         * straight to the linker put near models in low buckets and let the
         * whole world paint over them.
         */
        if (inst->sort_area >= 0 && psx_ot_area_active(ot)) {
            if (!psx_ot_area_bucket(ot,
                                    (u32)inst->sort_area & 0x7Fu, &bucket)) {
                /* The area's screen record is stale. Retail's active-area
                 * chooser rejects the entity here; falling back to global
                 * depth lets actors in another room paint through its walls. */
                return 0;
            }

            /* The model record is a bounds record on either list. +4 is the
             * projected origin depth written at 0x8006C078, +8 points at the
             * entity's absolute +0x78 AABB (0x8006BCB8), and render flag
             * 0x00800000 alone chooses Quick over Standard. */
            {
                s32 fallback_min[3], fallback_max[3];
                const s32 *bounds_min = inst->sort_bounds_min;
                const s32 *bounds_max = inst->sort_bounds_max;
                int axis;

                if (!inst->sort_bounds_valid) {
                    for (axis = 0; axis < 3; axis++)
                        fallback_min[axis] = fallback_max[axis] =
                            inst->origin[axis];
                    bounds_min = fallback_min;
                    bounds_max = fallback_max;
                }

                batch = psx_ot_batch_begin_box(
                            ot, (u32)inst->sort_area & 0x7Fu,
                            inst->sort_quick, (s16)otz,
                            bounds_min, bounds_max, cam->pos);
            }
        } else {
            bucket = psx_ot_depth_bucket(
                         ot, q2_ot_bucket_for_depth(ot, otz,
                                                    cam->sort_range));
        }
    }

    /*
     * The instance's own facing, composed into the camera's rotation once
     * rather than per part. Everything placed in the world is yaw-only — the
     * rotation integrator writes one axis — so that path is kept exactly as it
     * was; the three-angle form is taken only when a caller asks for it, which
     * today is the view weapon and its RotMatrix at 0x8004F464.
     */
    /*
     * AND THE YAW IS MIRRORED ON THE WAY IN, which is why every creature
     * moonwalked and shot through its own back.
     *
     * `q2_rotation_yaw_pitch` builds Ry with m[0][2] = -sin and m[2][0] = +sin,
     * which is Ry(-yaw) — the WORLD-TO-CAMERA form, and exactly right for the
     * view basis above. Used unchanged as an instance's MODEL-TO-WORLD matrix
     * it turns the mesh the wrong way, so the yaw is mirrored on the way in.
     *
     * THAT MIRROR IS THE WHOLE CORRECTION. This used to add a half turn on top
     * of it — `2048 - yaw` — on the grounds that the Soldier mesh is authored
     * facing -Z, and that second half is what was still turning every creature
     * round. It was measured through this very function, which is circular: the
     * function's own convention was both the instrument and the thing under
     * test.
     *
     * The mesh decides it, and it says +Z. BASE1's Soldier poses to Z bounds
     * -151..+266, asymmetric toward +Z by the length of the outstretched weapon
     * arm, which is the part of a humanoid that points where it is facing. In
     * the game the extra half turn shows plainly: a soldier that has the player
     * down to 8 hp is drawn back-to-camera, and without it the same frame draws
     * it facing the player with its gun across its body.
     *
     * The simulation was never the problem and is now measured rather than
     * assumed — `--trace-cre` reports the angle between the direction a
     * creature travels and the one it faces, and over a hunting soldier's run
     * that angle is 0 on every walking tick. It reads 2048 only while the
     * soldier is backing off under attack_state 1 and while a corpse slides,
     * both of which are meant to be backwards.
     */
    instance_spin(inst, spin);

    /*
     * Optional port-side uniform transform. Retail does not derive this from
     * entity+0xFC/+0xFE: the apparent ScaleMatrix calls at 0x8006B298 operate
     * on the GTE light matrix assembled by 0x8006AFE8. Apply an explicit scale
     * here when a diagnostic/tool asks for one, and keep normal entities at
     * Q2_ONE_12.
     */
    /*
     * The UNSCALED instance rotation, kept for the light basis.
     *
     * An explicit geometric scale belongs on the position path — a part's
     * translation has to follow it or the parts drift apart — but a light
     * DIRECTION is not a position and therefore keeps the unscaled basis.
     */
    memcpy(spin_unscaled, spin, sizeof(spin_unscaled));

    instance_scale(inst, spin);

    matrix_mul(world, view, spin);

    tint[0] = inst->tint[0];
    tint[1] = inst->tint[1];
    tint[2] = inst->tint[2];

    for (part = 0; part < m->hdr.num_parts; part++) {
        q2_model_part p;
        s16 rot[3][3], composed[3][3];
        s32 local[3], camera_space[3];
        u32 v;

        if (!q2_model_get_part(m, part, &p))
            break;

        if (stats)
            stats->parts++;

        if (inst->pose) {
            s16 q[4];
            memcpy(q, inst->pose[part].q, sizeof(q));
            q2_quat_to_matrix(rot, q);
            local[0] = inst->pose[part].t[0];
            local[1] = inst->pose[part].t[1];
            local[2] = inst->pose[part].t[2];
        } else {
            memset(rot, 0, sizeof(rot));
            rot[0][0] = rot[1][1] = rot[2][2] = (s16)Q2_ONE_12;
            local[0] = local[1] = local[2] = 0;
        }

        /* Vertices are part-local, so the part's rotation applies first and the
         * camera's after it. */
        matrix_mul(composed, world, rot);
        {
            gte_matrix gm;
            memcpy(gm.m, composed, sizeof(gm.m));
            gte_set_rotation(gte, &gm);
        }

        /*
         * The light matrix, and why it is composed differently from the
         * rotation matrix.
         *
         * 0x800B1F90 builds both from the part's quaternion and one of the
         * caller's two matrices, and the two callers' matrices differ by
         * exactly the VIEW rotation: positions use view x entity (the draw
         * context's +140), normals use the entity's own matrix alone (its +96).
         * That asymmetry is correct rather than an oversight — the light
         * directions this module is handed are in WORLD space, so rotating them
         * into the camera as well would make a model's shading swing as the
         * player turned.
         *
         * So: rows of the light matrix are the world-space directions carried
         * through the instance matrix and then the part's, which is what
         * 0x800B21B8 does with three MVMVAs against the already-composed
         * matrix.
         */
        if (inst->light) {
            s16 basis[3][3];
            gte_matrix lm;
            u32 j;

            /* Unscaled, and INVERTED below: `basis` carries part-local to
             * world, and what a world-space light direction needs is the trip
             * the other way. See matrix_apply_t. */
            matrix_mul(basis, spin_unscaled, rot);
            memset(&lm, 0, sizeof(lm));

            for (j = 0; j < Q2_LIGHT_ACTIVE_MAX; j++) {
                s32 in[3], out[3];
                int c;

                if (j >= inst->light->active)
                    break;

                for (c = 0; c < 3; c++)
                    in[c] = inst->light->dir[j][c];
                matrix_apply_t(basis, in, out);
                for (c = 0; c < 3; c++)
                    lm.m[j][c] = (s16)out[c];
            }

            gte_set_light_matrix(gte, &lm);
            gte_set_colour_matrix(gte, &inst->light->colour);
            gte_set_back_colour(gte, inst->light->back[0],
                                inst->light->back[1], inst->light->back[2]);
        }

        /* The part's translation is in model space: rotate it by the model's
         * own matrix, offset by where the model stands, and express the result
         * in camera space, because the GTE adds translation after rotating. */
        {
            s32 model_space[3], offset[3];

            /* The same scaled instance matrix the vertices went through, so the
             * offset shrinks with the model. */
            matrix_apply(spin, local, model_space);

            offset[0] = inst->origin[0] + model_space[0] - cam->pos[0];
            offset[1] = inst->origin[1] + model_space[1] - cam->pos[1];
            offset[2] = inst->origin[2] + model_space[2] - cam->pos[2];

            matrix_apply(view, offset, camera_space);
            gte_set_translation(gte, camera_space[0], camera_space[1],
                                camera_space[2]);
        }

        /*
         * Each part's vertices are the next run in the model's vertex array —
         * the part record carries the count, not an offset — and they land in
         * the shared window at vert_base. Later parts may overwrite slots
         * earlier ones wrote; that overlap is the format, not a fault.
         */
        for (v = 0; v < p.num_verts; v++) {
            q2_model_vertex mv;
            u32 slot = p.vert_base + v;

            if (slot >= Q2_MODEL_SCRATCH_MAX)
                break;
            if (!q2_model_get_vertex(m, vertex_cursor + v, &mv))
                break;

            gte->v[0].x = mv.x;
            gte->v[0].y = mv.y;
            gte->v[0].z = mv.z;
            gte_rtps(gte, false);

            window[slot].xy    = gte->sxy[2];
            window[slot].z     = gte->sz[3];
            /*
             * THE DIVIDE OVERFLOWING IS NOT A REJECTION, and treating it as one
             * cut the front off anything close to the eye.
             *
             * `gte_divide` sets this flag when the projection distance reaches
             * twice the depth — at `h` 160 that is any vertex nearer than 80 —
             * and the hardware CLAMPS the quotient to 0x1FFFF rather than
             * trapping. `gte_push_sxy` has already saturated the coordinate to
             * the hardware's own [-1024, 1023], so the slot holds exactly what
             * the GTE would have left behind.
             *
             * Whether the game then throws the face away is a question about
             * the GAME, and the answer is in the instruction census:
             * **the executable contains no `cfc2 rX, $31` at all** — twelve
             * `cfc2` in the whole image and not one of them reads FLAG. Nothing
             * can branch on an overflow it never loads, so the original draws
             * these faces, stretched to the saturation limits. That is the
             * near-plane smear the hardware is known for, and FIDELITY.md's
             * whole argument is that artefacts of the arithmetic are the point.
             *
             * `valid` therefore means "this slot was written" and nothing more.
             * It is what separates a decode fault from geometry.
             */
            window[slot].valid    = true;
            window[slot].overflow = (gte->flag & GTE_FLAG_DIV_OVERFLOW) != 0;

            /*
             * The normal through the same op the original uses. NCS is NCT's
             * one-vertex form: light matrix, colour matrix, back colour, and
             * NOTHING from the primitive colour — so a fully unlit vertex comes
             * out as the back colour alone and a fully lit one saturates at
             * 255, which on the GPU's modulate path is twice the texel.
             */
            if (inst->light) {
                gte->v[0].x = mv.nx;
                gte->v[0].y = mv.ny;
                gte->v[0].z = mv.nz;
                gte_ncs(gte);

                window[slot].rgb.r = gte->rgb_fifo[2].r;
                window[slot].rgb.g = gte->rgb_fifo[2].g;
                window[slot].rgb.b = gte->rgb_fifo[2].b;
            } else {
                window[slot].rgb.r = tint[0];
                window[slot].rgb.g = tint[1];
                window[slot].rgb.b = tint[2];
            }
        }

        vertex_cursor += p.num_verts;

        /* Faces are stored per part in the same order, so this part's faces are
         * the next run — and they can only be drawn once its vertices are in
         * the window, which is exactly why the original interleaves the two. */
        for (v = 0; v < p.num_faces; v++, face_index++) {
            q2_model_face f;
            psx_prim *prim;
            bool ok = true;
            int i;

            if (!q2_model_get_face(m, face_index, &f))
                break;

            if (stats)
                stats->faces_total++;

            for (i = 0; i < 4; i++) {
                if (f.v[i] >= Q2_MODEL_SCRATCH_MAX || !window[f.v[i]].valid) {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                /* A slot the decode never wrote, which is a fault rather than
                 * geometry — the near case no longer reaches here. */
                if (stats)
                    stats->faces_rejected_bad++;
                continue;
            }

            /* Counted so the smear stays visible in the numbers even though it
             * no longer costs a face. */
            if (stats) {
                for (i = 0; i < 4; i++) {
                    if (window[f.v[i]].overflow) {
                        stats->faces_rejected_near++;
                        break;
                    }
                }
            }

            /*
             * The linker's own NCLIP pair, before anything is allocated. An
             * ordering table has no depth buffer, so a face that has been
             * emitted is a face that will paint over whatever the sort put
             * behind it — which on a closed mesh is the far side of the model
             * showing through the near side. See q2_model_quad_faces_camera.
             */
            {
                gte_sxy screen[4];
                for (i = 0; i < 4; i++)
                    screen[i] = window[f.v[i]].xy;
                /*
                 * 0x800B22FC/230C advances the eight-byte part pointer;
                 * 0x800B242C loads its +4 word into t3. BLTZ at 0x800B2498
                 * bypasses both NCLIPs and its delay slot shifts t3 left.
                 * The mask restarts per part and runs out after 32 faces.
                 */
                if (!(v < 32 && (p.force_draw & (0x80000000u >> v))) &&
                    !q2_model_quad_faces_camera(gte, screen)) {
                    if (stats)
                        stats->faces_rejected_back++;
                    continue;
                }
            }

            /*
             * ALLOCATED HERE, LINKED LATER.
             *
             * A face can only be resolved while its own part owns the scratch
             * window, but the order a model DRAWS in is not this loop's order:
             * it comes from block A, which carries eight painter's orders and
             * is the thing this port used to read as a batch directory
             * (model.h). The console keeps the two apart the same way — it
             * parks packet pointers in a flat array at 0x800DDDCC as it builds
             * and chains that array afterwards, walking it in the stored order
             * (0x800B25E0) — so the primitive is taken out of the pool now and
             * put in a bucket at the end of this function.
             */
            prim = psx_ot_alloc(ot);

            if (!prim) {
                if (stats)
                    stats->ot_overflow++;
                continue;
            }

            if (reorder) {
                built[face_index] = prim;
                if (face_index >= built_count)
                    built_count = face_index + 1;
            } else {
                /* Too many faces to hold: link now, in file order, which is
                 * what this did before block A was read. */
                link_face(ot, prim, bucket, batch);
            }

            prim->kind = PSX_PRIM_GT4;

            for (i = 0; i < 4; i++) {
                prim->xy[i].x  = window[f.v[i]].xy.x;
                prim->xy[i].y  = window[f.v[i]].xy.y;
                prim->uv[i].u  = f.uv[i][0];
                prim->uv[i].v  = f.uv[i][1];

                /* Whatever the vertex pass left in the window: the light for a
                 * lit instance, the flat tint otherwise. */
                prim->rgb[i] = window[f.v[i]].rgb;
            }

            /*
             * A face's blend selector is `flags >> 5`, and it reaches the GPU
             * through the same two tables the world uses (0x8006DE40):
             *
             *     POLY.code  = codeTable[sel]  | 0x3C
             *     POLY.tpage = tpageTable[page] | blendTable[sel]
             *
             * so selectors 1..4 are B/2+F/2, B+F, B-F and B+F/4 respectively and
             * 0 and 5..7 are opaque. Reading `sel != 0` as "transparent" was
             * right for four of the eight and wrong for three, and gave every
             * transparent face the additive mode regardless of which one it
             * asked for. Unlike the world's, this path never writes the table
             * back.
             *
             * AND THE SELECTOR IS NOT ALWAYS THE ONE ON THE DISC. No face in
             * any CastList sets a blend bit, which for a long time made this
             * whole branch look inert — `faces_semi` was zero for every model
             * on every map. It is not inert: `PreInitExplosion` (0x8005AAA4)
             * rewrites every face of the `Explosion` model to selector 2 at
             * level load, so the fireball is B+F. `q2_model_get_face` hands
             * that rewritten byte back; see the level-load section in model.h
             * for the walk and the four instructions that do it.
             */
            {
                u32 sel  = q2_model_face_blend(&f);
                u32 page = q2_model_face_page(&f);

                prim->clut = q2_vram_clut_word(
                    q2_model_face_clut_index(&f, inst->clut4_count_a));
                prim->tpage            = q2_tpage_model(tpage, page, sel);
                prim->semi_transparent = q2_surf_selector_semi(sel);
                prim->textured_blend   = true;

                if (stats && prim->semi_transparent)
                    stats->faces_semi++;
            }

            emitted++;
            if (stats)
                stats->faces_emitted++;
        }
    }

    /*
     * AND NOW THE ORDER, which is the model's own and not this loop's.
     *
     * Block A holds eight painter's orders, each an exact permutation of every
     * face, near to far (model.h). The console chains them by PREPENDING, so
     * the first face of a stream ends up drawn last and therefore on top; this
     * links in the same direction for the same reason.
     *
     * Without it a model draws in file order, and on the view weapon that is
     * visible: the body slab paints over the raised blocks on the gun's spine,
     * which the console's own frame shows.
     *
     * A model with no usable order — a malformed block, or more faces than the
     * table can hold — falls back to file order, which is what this did for
     * every model before block A was read correctly.
     *
     * WHICH order is CalcSortList's (0x8006A020): the camera-to-model vector
     * taken into the model's own frame by the transpose of its matrix — the
     * unscaled one, since only the signs are kept — and its octant.
     */
    if (reorder) {
        q2_model_draw_order ord;
        s32 d[3], dir[3];
        u32 n = 0, k;
        int r;

        for (k = 0; k < 3; k++)
            d[k] = inst->origin[k] - cam->pos[k];
        for (r = 0; r < 3; r++) {
            /* 0x8006FF80 keeps the raw sum, so only its sign is taken. */
            s64 sum = (s64)spin_unscaled[0][r] * d[0] +
                      (s64)spin_unscaled[1][r] * d[1] +
                      (s64)spin_unscaled[2][r] * d[2];
            dir[r] = sum > 0 ? 1 : sum < 0 ? -1 : 0;
        }

        if (q2_model_draw_order_select(m, dir, inst->anim_pos, &ord))
            n = q2_model_draw_sequence_from(m, &ord, order,
                                            Q2_MODEL_MAX_FACES);

        if (n == 0) {
            for (k = 0; k < built_count; k++)
                order[k] = (u16)k;
            n = built_count;
        }

        for (k = 0; k < n; k++) {
            u32 face = order[k];
            psx_prim *p;

            if (face >= built_count)
                continue;
            p = built[face];
            if (!p)
                continue;

            link_face(ot, p, bucket, batch);
        }
    }

    if (emit_shadow(inst, cam, ot, gte, view, bucket, batch, stats))
        emitted++;

    if (inst->glint) {
        u32 n = q2_fx_glint_draw_attached(inst->glint, inst->origin,
                         (const s16 (*)[3])spin_unscaled, inst->sort_area,
                         batch, cam, ot, gte);
        emitted += n;
        if (stats) {
            stats->glint_emitted += n;
            stats->glint_draws++;
        }
    }

    return emitted;
}
