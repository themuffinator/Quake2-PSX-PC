#include "entitydraw.h"

#include "fxtables.h"     /* Q2_FX_ABR_ADD, the blend the bolt draws in */
#include "projectile.h"
#include "weapontables.h"

#include <string.h>

/* The engine's scratch window bound; a model needing more is rejected by the
 * loader, so this is a ceiling rather than a guess. */
#define POSE_MAX 256

bool q2_entity_resolve_model(q2_entity *e, const q2_model_bank *bank)
{
    s32 index;

    if (!e)
        return false;

    /*
     * Already resolved stays resolved, and against the bank it was resolved
     * AGAINST — a map's models are spread over COMMON's CastList and up to
     * eight zones', and an index means nothing against a different bank. A
     * caller with several banks calls this once per bank; the first that has
     * the name wins and the rest are no-ops.
     */
    if (e->model_index >= 0 && e->model_bank)
        return true;

    if (!bank || !e->model[0])
        return false;

    index = q2_model_bank_find(bank, e->model);
    if (index < 0)
        return false;           /* another bank may still have it */

    e->model_index = index;
    e->model_bank  = bank;

    /*
     * The clip length the think wraps the frame against.
     *
     * CORRECTION — this used to say the engine's `lh model[+2]` (0x80059404) is
     * "the duration of the clip the entity is playing". It is not. Compared
     * across all 1,723 models on the disc, +2 equals the first clip's length on
     * 1,496 and those 1,496 are exactly the single-clip models; against the SUM
     * of every clip's frames it agrees 1,723 of 1,723. Soldier reads 1302 there
     * against a first clip of 108. `q2psx-inspect modelents` runs that
     * comparison.
     *
     * So +2 is the model's TOTAL animation length. Every item model on the disc
     * has exactly one clip, and the item renderer always begins at that first
     * clip. The item record's final halfwords are posed SHADOW VERTICES, not
     * clip numbers — the consumer at 0x800784CC reads them only after the model
     * draw and sends each through the vertex poser.
     */
    e->clip_length = 0;
    {
        q2_model m;

        if (q2_model_get(bank, (u32)index, &m) == Q2_OK) {
            q2_model_anim clip;

            if (q2_model_anim_get(&m, 0, &clip))
                e->clip_length = clip.frames;

            /*
             * AND THE MODEL'S OWN VERTICAL BIAS, which is why items sat in the
             * floor.
             *
             * `model_offset` is `lh model[+0x1C]` — the header's ext2 — and the
             * draw origin is the position lowered by 286 and raised again by
             * it (FORMATS §5398, §5422, and the same expression item.c already
             * writes). The parameter was threaded all the way through
             * `q2_item_spawn` and both call sites passed ZERO, because neither
             * has the model bank: the spawn runs before anything is resolved.
             * So every item was drawn sunk by its own bias — a few units for a
             * medkit, much more for a weapon, which is why the guns looked the
             * worst. The pickup burst is spawned at the entity's draw origin
             * too, so it came out under the floor with them.
             *
             * Here is where it belongs: this is the first point at which an
             * entity and its model are both in hand. The origin is recomputed
             * from `pos`, so it does not matter that the spawn already set one.
             */
            e->model_offset = m.hdr.ext2;

            /*
             * ...AND ONLY FOR AN ITEM. A transient MODEL ENTITY is placed by
             * its spawner, not by a Population record: 0x8005A8C0 and
             * 0x8005A8D8 write the SAME argument into ent+0xA4 and ent+0x54,
             * so its draw origin IS its position with no eye base and no bias
             * subtracted. Recomputing it here dropped the `Explosion` model
             * `Q2_EYE_BASE - 370` below the crate it came out of, which put it
             * through the floor — see modelent.h.
             */
            if (!(e->render_flags & Q2_RF_TRANSIENT))
                e->origin[1] = e->pos[1] + Q2_EYE_BASE - e->model_offset;
        }
    }

    return true;
}

u32 q2_entity_build_ot(q2_entity_set *set, const q2_entity_draw_ctx *ctx,
                       const q2_camera *cam, psx_ot *ot, gte_state *gte,
                       q2_entity_draw_stats *stats)
{
    u32 i, emitted = 0;

    if (stats)
        memset(stats, 0, sizeof(*stats));

    if (!set || !ctx || !cam || !ot || !gte)
        return 0;

    for (i = 0; i < set->count; i++) {
        q2_entity *e = &set->ent[i];
        q2_model m;
        q2_model_pose pose[POSE_MAX];
        q2_model_instance inst;
        q2_model_draw_stats ms;
        q2_coll_node cell;
        s32 coll_node;
        s32 sort_area = -1;
        bool posed = false;

        if (!e->in_use)
            continue;

        if (stats)
            stats->considered++;

        if (!q2_entity_visible(e, ctx->player)) {
            if (stats)
                stats->invisible++;
            continue;
        }

        if (!q2_entity_resolve_model(e, ctx->bank)) {
            if (stats)
                stats->no_model++;
            continue;
        }

        /* Its OWN bank, which may not be the context's — see the resolve. */
        if (q2_model_get(e->model_bank, (u32)e->model_index, &m) != Q2_OK) {
            if (stats)
                stats->no_model++;
            continue;
        }

        /*
         * Pose it on the frame its think left — AND POSE IT EVEN WHEN NOTHING
         * ANIMATES IT, which is why the medkits were in the floor.
         *
         * This used to run only when the item record's final list was nonempty,
         * because that list had been mislabelled as clips. It is actually a
         * list of shadow-footprint vertices, so most items skipped their only
         * clip and drew their vertices RAW. A part's rest transform is not the
         * identity: on BASE1, `Large Medi P` reads Y 62..242 raw
         * against −94..86 posed, a translation of −156, and `Medi P` reads
         * 43..168 against −65..60, a translation of −108. Both are health
         * pickups; `Shells P` and `Adrenal P` in the same bank translate by
         * zero and looked right, which is what made this look like a health
         * item problem rather than a posing one.
         *
         * Drawn raw, the large medkit's base lands 156 units below its own
         * origin's floor and 24 units of a 180-unit box stick out. That is the
         * report exactly.
         *
         * A model with no clip at all still cannot be posed and keeps the raw
         * path; every model in the shipped banks carries at least one.
         */
        if (m.hdr.num_parts <= POSE_MAX) {
            q2_model_anim clip;

            if (q2_model_anim_get(&m, 0, &clip)) {
                s32 frame = clip.frames > 0 ? (e->frame % clip.frames) : 0;
                if (q2_model_pose_at(&m, &clip, (u32)frame, pose) == Q2_OK)
                    posed = true;
            }
        }

        q2_light_env env;

        q2_model_instance_init(&inst);
        inst.model         = &m;
        inst.pose          = posed ? pose : NULL;
        inst.origin[0]     = e->origin[0];
        inst.origin[1]     = e->origin[1];
        inst.origin[2]     = e->origin[2];
        inst.yaw           = e->angles[1];
        /*
         * +0xFC/+0xFE DO NOT SCALE GEOMETRY.
         *
         * The only draw-time readers are inside 0x8006AFE8, and their
         * destination at 0x800DDD1C is installed with SetLightMatrix at
         * 0x8006BBD4.  The model rotation installed immediately afterwards
         * comes from entity+0x2C0 and never reads either halfword.  They are
         * light-intensity factors, consumed by q2_light_env_build below.
         *
         * Keeping the generic instance transform neutral matters for every
         * animated value: a materialising pickup brightens without growing,
         * an explosion darkens without shrinking, and a corpse dissolves by
         * illumination rather than collapsing toward its origin.
         */
        inst.scale         = Q2_ONE_12;
        inst.clut4_count_a = ctx->clut4_count_a;
        inst.tpage         = ctx->tpage;

        /*
         * Render flag bit 0 gates 0x800784CC. The item table's final list is
         * copied into the model wrapper's +4 slot at 0x80059AC0 and consumed
         * there as GLOBAL STORAGE vertex indices. Item spawn leaves +0x94 at
         * zero, so a record without such vertices collapses to a degenerate
         * footprint and emits nothing — exactly why only twelve records carry
         * a list.
         *
         * The height expression is retail's
         *     entity+0x30 - entity+0xA8 - entity+0xF8
         * and is zero for an item sitting at its spawn floor because the model
         * offset cancels. Keeping the expression rather than a literal zero
         * preserves the shrink when another path raises the draw origin.
         */
        inst.shadow_enabled      = (e->render_flags & 1u) != 0;
        inst.shadow_vertex       = e->shadow_vertex;
        inst.shadow_vertex_count = e->shadow_vertex_count;
        inst.shadow_radius       = 0; /* item placement never writes +0x94 */
        inst.shadow_height       = e->spawn_origin[1] - e->origin[1]
                                 - e->model_offset;
        inst.shadow_origin[0]    = e->spawn_origin[0];
        inst.shadow_origin[1]    = e->spawn_origin[1];
        inst.shadow_origin[2]    = e->spawn_origin[2];

        /* Movement stores SecondaryColl byte +32 in retail entity byte +0x9E
         * (0x80046B08). Transient model entities already carry the Scene area
         * their spawner supplied; ordinary items recover the same value from
         * the cell they occupy. This is the insertion-point selector, not a
         * material or collision-solidness flag. */
        coll_node = ctx->coll
                  ? (e->node >= 0
                        ? e->node
                        : q2_coll_find_node(ctx->coll, e->pos, -1, true))
                  : ctx->coll_node;
        if (e->render_flags & Q2_RF_TRANSIENT) {
            sort_area = e->surface & 0x7F;
        } else if (ctx->coll && coll_node >= 0 &&
                   q2_collision_get_node(ctx->coll, (u32)coll_node, &cell)) {
            sort_area = cell.contents & 0x7F;
        }
        inst.sort_area = sort_area;
        inst.sort_quick = (e->render_flags & Q2_RF_QUICK_SORT) != 0;
        {
            int axis;

            for (axis = 0; axis < 3; axis++) {
                /* Item linking has already expanded +0x78 into an absolute
                 * box. The transient model spawner retains mins/maxs in the
                 * entity frame, so do the linker's origin addition here. */
                if (e->render_flags & Q2_RF_TRANSIENT) {
                    inst.sort_bounds_min[axis] =
                        e->pos[axis] + e->bounds_min[axis];
                    inst.sort_bounds_max[axis] =
                        e->pos[axis] + e->bounds_max[axis];
                } else {
                    inst.sort_bounds_min[axis] = e->bounds_min[axis];
                    inst.sort_bounds_max[axis] = e->bounds_max[axis];
                }
            }
            inst.sort_bounds_valid = true;
        }

        /* `tint` is only the unlit diagnostic fallback. The retail draw always
         * takes entity+0x2AC through the GTE back-colour path below. */
        if (e->flags & Q2_ITEM_GLOW) {
            inst.tint[0] = e->glow[0];
            inst.tint[1] = e->glow[1];
            inst.tint[2] = e->glow[2];
        }

        /*
         * The lights, gathered per entity because the engine gathers per entity:
         * 0x8006BBCC sits inside the draw, not before the loop, so two items a
         * few units apart legitimately pick different lights.
         *
         * The glow becomes the BACK COLOUR here rather than a vertex tint,
         * because the shading op is NCT and NCT ignores the primitive colour.
         * That is the same "darkened by its own glow" behaviour the tint path
         * has, arriving through the register the hardware actually uses for it.
         */
        if (ctx->lights) {
            q2_light_set lit;
            /* The entity's OWN cell when the hull is available — the engine
             * keeps it at entity+0xA2 — and the caller's single node only as a
             * fallback. The same lookup selected its render area above. */
            q2_light_gather(&lit, ctx->lights, e->origin, coll_node,
                            (s16)e->remove_in);
            /* BOTH intensity fields feed the light environment. 0x8006B298
             * scales the GTE light-matrix rows by their product and 0x8006B468
             * applies the same product to the back colour. +0xFC drives item,
             * logo and corpse fades; the explosion think at 0x8005A68C is the
             * located writer of +0xFE. */
            q2_light_env_build(&env, &lit, e->scale, e->fade, e->glow);
            inst.light = &env;
        }

        emitted += q2_model_build_ot(&inst, cam, ot, gte, &ms);

        if (stats) {
            stats->drawn++;
            stats->faces_emitted += ms.faces_emitted;
            stats->shadows_emitted += ms.shadows_emitted;
            stats->ot_overflow   += ms.ot_overflow;
        }
    }

    return emitted;
}

/* ------------------------------------------------------------------------- */
/* Projectiles in flight — see the note in entitydraw.h                       */
/* ------------------------------------------------------------------------- */
/*
 * The six faces of the eight-corner box, in the corner order the table at
 * 0x8009DB1C stores them:
 *
 *     0 (-10,-10,-50)  1 ( 10,-10,-50)  2 (-10,-10, 50)  3 ( 10,-10, 50)
 *     4 (-10, 10,-50)  5 ( 10, 10,-50)  6 (-10, 10, 50)  7 ( 10, 10, 50)
 *
 * so x picks the low bit, z the second and y the third. Each row below is a
 * perimeter walk, which is the order the rasteriser wants; the table's own
 * order is the Z-order every mesh on this disc is stored in.
 */
static const u8 k_bolt_face[6][4] = {
    { 0, 1, 3, 2 },   /* y = -10 */
    { 4, 6, 7, 5 },   /* y = +10 */
    { 0, 4, 5, 1 },   /* z = -50 */
    { 2, 3, 7, 6 },   /* z = +50 */
    { 0, 2, 6, 4 },   /* x = -10 */
    { 1, 5, 7, 3 }    /* x = +10 */
};

/*
 * A rotation whose +Z axis lies along `dir`.
 *
 * The console builds this with RotMatrix from a Euler triple it derives from
 * the direction (0x8004D834). This port builds the basis directly from the
 * vector, which produces the same box along the same axis without a table of
 * arc-tangents the common layer does not have; the roll about the long axis is
 * arbitrary either way, because the section is square.
 *
 * Returns false for a direction of zero length, which a live projectile never
 * has — its velocity is what makes it live.
 */
static bool bolt_basis(const s32 dir[3], s16 m[3][3])
{
    static const s32 k_up[3] = { 0, -4096, 0 };  /* -Y is up in world space */
    s64 len2;
    s32 fwd[3], right[3], up[3];
    int k;

    len2 = (s64)dir[0] * dir[0] + (s64)dir[1] * dir[1] + (s64)dir[2] * dir[2];
    if (len2 <= 0)
        return false;

    {
        s64 lo = 0, hi = 0x7FFFFFFF, len = 0;
        while (lo <= hi) {
            s64 mid = lo + (hi - lo) / 2;
            if (mid * mid <= len2) { len = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        if (len <= 0)
            return false;
        for (k = 0; k < 3; k++)
            fwd[k] = (s32)(((s64)dir[k] * 4096) / len);
    }

    /* right = up x fwd, and a fallback axis for a bolt fired straight up or
     * straight down, where the cross product degenerates. */
    right[0] = (s32)(((s64)k_up[1] * fwd[2] - (s64)k_up[2] * fwd[1]) >> 12);
    right[1] = (s32)(((s64)k_up[2] * fwd[0] - (s64)k_up[0] * fwd[2]) >> 12);
    right[2] = (s32)(((s64)k_up[0] * fwd[1] - (s64)k_up[1] * fwd[0]) >> 12);

    if (right[0] == 0 && right[1] == 0 && right[2] == 0) {
        right[0] = 4096;
        right[1] = 0;
        right[2] = 0;
    } else {
        s64 r2 = (s64)right[0] * right[0] + (s64)right[1] * right[1] +
                 (s64)right[2] * right[2];
        s64 lo = 0, hi = 0x7FFFFFFF, len = 0;
        while (lo <= hi) {
            s64 mid = lo + (hi - lo) / 2;
            if (mid * mid <= r2) { len = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        if (len <= 0)
            return false;
        for (k = 0; k < 3; k++)
            right[k] = (s32)(((s64)right[k] * 4096) / len);
    }

    up[0] = (s32)(((s64)fwd[1] * right[2] - (s64)fwd[2] * right[1]) >> 12);
    up[1] = (s32)(((s64)fwd[2] * right[0] - (s64)fwd[0] * right[2]) >> 12);
    up[2] = (s32)(((s64)fwd[0] * right[1] - (s64)fwd[1] * right[0]) >> 12);

    /* Columns are the local axes, so m * (x,y,z) = x*right + y*up + z*fwd. */
    for (k = 0; k < 3; k++) {
        m[k][0] = (s16)right[k];
        m[k][1] = (s16)up[k];
        m[k][2] = (s16)fwd[k];
    }
    return true;
}

u32 q2_projectiles_build_ot(const struct q2_projectiles *list,
                            const q2_collision *coll,
                            const q2_camera *cam, psx_ot *ot, gte_state *gte)
{
    const q2_weapon_tables *wt = q2_weapon_tables_builtin();
    u32 i, emitted = 0;

    if (!list || !cam || !ot || !gte)
        return 0;

    for (i = 0; i < Q2_PROJ_MAX; i++) {
        const q2_projectile *p = &list->p[i];
        s16     m[3][3];
        gte_sxy xy[8];
        u16     z[8];
        bool    ok[8];
        psx_rgb tint;
        s32 sort_area = -1;
        s32 area_bucket = -1;
        s32 batch = PSX_OT_BATCH_INVALID;
        u32     f;
        int     v;

        /* Grenade3 state 1 hides all four model parts with flag 0x80 at
         * 0x8004ABF0. It is the hand model that shows the grenade while held;
         * the projectile body appears only after the 411 release crossing. */
        if (!p->in_use || (p->kind == Q2_PROJ_HAND_GRENADE &&
                           p->node == Q2_PROJ_NODE_HELD))
            continue;

        if (psx_ot_area_active(ot) && coll) {
            q2_coll_node cell;
            s32 node = p->node;
            u32 resolved;

            if (node < 0)
                node = q2_coll_find_node(coll, p->pos, -1, true);
            if (node >= 0 &&
                q2_collision_get_node(coll, (u32)node, &cell)) {
                if (!psx_ot_area_bucket(ot, cell.contents & 0x7Fu,
                                        &resolved))
                    continue;
                sort_area = cell.contents & 0x7F;
                area_bucket = (s32)resolved;
            }
        }
        q2_camera_apply_area_projection(cam, ot, sort_area, gte);
        if (!bolt_basis(p->vel, m))
            continue;

        /* The kind's own glow, from the preset the dynamic light reads out of
         * 0x800AE954 — so the body and the light it casts agree. */
        tint.pad = 0;
        if (p->kind == Q2_PROJ_BFG) {
            tint.r = Q2_PROJ_BFG_LIGHT_R;
            tint.g = Q2_PROJ_BFG_LIGHT_G;
            tint.b = Q2_PROJ_BFG_LIGHT_B;
        } else {
            tint.r = Q2_PROJ_LIGHT_R;
            tint.g = Q2_PROJ_LIGHT_G;
            tint.b = Q2_PROJ_LIGHT_B;
        }

        for (v = 0; v < 8; v++) {
            s32 world[3];
            int k;

            for (k = 0; k < 3; k++) {
                s64 sum = (s64)m[k][0] * wt->bolt_shape[v][0] +
                          (s64)m[k][1] * wt->bolt_shape[v][1] +
                          (s64)m[k][2] * wt->bolt_shape[v][2];
                world[k] = p->pos[k] + (s32)(sum >> 12) - cam->pos[k];
            }

            ok[v] = gte_project_point(gte, world[0], world[1], world[2],
                                      &xy[v], &z[v]);
        }

        if (sort_area >= 0) {
            gte_sxy centre_xy;
            u16 centre_z;

            if (gte_project_point(gte,
                                  p->pos[0] - cam->pos[0],
                                  p->pos[1] - cam->pos[1],
                                  p->pos[2] - cam->pos[2],
                                  &centre_xy, &centre_z)) {
                /* Projectile body rendering is inferred (entitydraw.h), but
                 * once present it obeys the same point/Quick contract as the
                 * other small dynamic effect chains. */
                batch = psx_ot_batch_begin_point(
                            ot, (u32)sort_area, true, (s16)centre_z,
                            p->pos, cam->pos);
            }
        }

        for (f = 0; f < 6; f++) {
            const u8 *idx = k_bolt_face[f];
            psx_prim *prim;
            u32 depth = 0;
            bool good = true;
            int c;

            for (c = 0; c < 4; c++) {
                if (!ok[idx[c]]) { good = false; break; }
                depth += z[idx[c]];
            }
            if (!good)
                continue;

            /* The mean depth as the key too, so a bolt and the geometry it
             * shares a bucket with sort by depth rather than by which emitter
             * ran first. See psx_ot_add_depth. */
            if (area_bucket >= 0) {
                prim = batch >= 0
                     ? psx_ot_batch_add(ot, batch)
                     : psx_ot_add_bucket_depth(ot, (u32)area_bucket,
                                               (u16)(depth / 4u),
                                               depth / 4u);
            } else {
                prim = psx_ot_add_depth(
                           ot,
                           (u16)q2_ot_bucket_for_depth(
                               ot, depth / 4u, cam->sort_range),
                           depth / 4u);
            }
            if (!prim)
                break;

            /* Additive and flat: a bolt is a light source, and one that
             * occluded the wall behind it would read as a solid brick. */
            prim->kind             = PSX_PRIM_F4;
            prim->semi_transparent = true;
            prim->tpage            = Q2_FX_ABR_ADD;
            prim->rgb[0]           = tint;

            for (c = 0; c < 4; c++) {
                /* gte_sxy and psx_xy share a layout but are distinct types;
                 * reading one through the other is undefined. */
                prim->xy[c].x = xy[idx[c]].x;
                prim->xy[c].y = xy[idx[c]].y;
            }

            emitted++;
        }
    }

    return emitted;
}
