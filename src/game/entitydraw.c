#include "entitydraw.h"

#include "effect.h"      /* the debris pool this file now draws */
#include "fxtables.h"     /* Q2_FX_ABR_ADD, the blend the bolt draws in */
#include "projectile.h"
#include "trig.h"         /* q2_rotation_euler, RotMatrix's composition */
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

bool q2_entity_draw_rotation(const q2_entity *e, s16 out[3][3])
{
    if (!e || !out || !(e->render_flags & Q2_RF_TRANSIENT))
        return false;

    /*
     * RotMatrix(+0xE6, +0x2C0): 0x80089E38 is handed `addiu a0, s0, 230` and
     * `addiu a1, s0, 704` at both chunk spawners (0x8005A30C/0x8005A310,
     * 0x8005AFD0/0x8005AFD4) and on both arms of the toss (0x80046CA4/
     * 0x80046CAC moving, 0x80046CB8/0x80046CC0 at rest). The angles are the
     * halfwords the tumble keeps (`lhu`/`addiu`/`sh`, 0x80046BE0..0x80046CA0);
     * q2_sin12 wraps them onto the 4096 circle as the table index does.
     *
     * One unit is not reproduced: the rest test's `+1` to +0xE6
     * (0x80046D5C..0x80046D70) lands after that toss's RotMatrix, so the
     * console draws it a tick late. This reads the angles as they stand — a
     * 4096th of a turn, for one tick.
     */
    q2_rotation_euler(out, e->angles[0], e->angles[1], e->angles[2]);
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
        s16 tumble[3][3];
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

        if (!q2_entity_resolve_model(e, ctx->bank) &&
            !q2_entity_resolve_model(e, ctx->zone_bank)) {
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
            bool scripted = ctx->glint && ctx->glint->owner_active &&
                            e->kind == Q2_FX_GLINT_TARGET_KIND &&
                            e->place_id == Q2_FX_GLINT_TARGET_ID;
            q2_model_move move;
            if (scripted && q2_model_move_by_name(&m,
                    ctx->glint->dying ? "Death" : "Activate", &move)) {
                /* BIGGUN +0x1978 selects Activate by name. As with other
                 * named moves, convert block D to the animation cursor's
                 * tenths of a frame before advancing by the engine dt. */
                u32 span = move.end > move.start
                         ? ((u32)move.end - move.start) * 5 : 0;
                u32 time = ctx->glint->dying ? (u32)ctx->glint->death_time
                                             : ctx->glint->script_time;
                u32 frame = ctx->glint->dying ? (time < span ? time : span)
                                             : span && time ? (time - 1) % span + 1 : 0;
                u32 position = (u32)move.start * 5 + frame;
                u32 within;
                if (q2_model_anim_at_position(&m, position, &clip, &within) &&
                    q2_model_pose_at(&m, &clip, within, pose) == Q2_OK)
                    posed = true;
            } else if (q2_model_anim_get(&m, 0, &clip)) {
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
        /* A gib's tumble: all three angles, not the yaw alone — see
         * q2_entity_draw_rotation. Everything else keeps the yaw path. */
        if (ctx->glint && ctx->glint->owner_active &&
            e->kind == Q2_FX_GLINT_TARGET_KIND &&
            e->place_id == Q2_FX_GLINT_TARGET_ID) {
            q2_rotation_euler(tumble, e->angles[0], e->angles[1], e->angles[2]);
            inst.rot = (const s16 (*)[3])tumble;
        } else if (q2_entity_draw_rotation(e, tumble))
            inst.rot = (const s16 (*)[3])tumble;
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
        } else if (ctx->coll && ctx->coll_primary) {
            /*
             * Outside SecondaryCol. The PrimaryColl cell names the same
             * area, as UpdateBoxStuff's mode 2 (0x80054E60) reads +0x9E;
             * and an item that NO cell of the loaded zone holds is not in
             * this zone at all — the test the client already puts creatures
             * through at load. Drawn anyway it has no area, which is global
             * depth: JAIL3's zone 0 places a Large Medi P standing in zone
             * 4's cell 60, ten thousand units past zone 0's last cell, and
             * it painted over the walls from 162 of zone 0's 167 cells.
             * It is the one item on the disc in neither hull.
             */
            s32 primary = q2_coll_find_node(ctx->coll_primary, e->pos, -1,
                                            true);

            if (primary < 0 ||
                !q2_collision_get_node(ctx->coll_primary, (u32)primary,
                                       &cell)) {
                if (stats)
                    stats->outside_zone++;
                continue;
            }
            sort_area = cell.contents & 0x7F;
        }
        inst.sort_area = sort_area;
        if (ctx->glint && ctx->glint->raised &&
            e->kind == Q2_FX_GLINT_TARGET_KIND &&
            e->place_id == Q2_FX_GLINT_TARGET_ID)
            inst.glint = ctx->glint;
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
            stats->glint_emitted += ms.glint_emitted;
            stats->glint_draws += ms.glint_draws;
            stats->ot_overflow   += ms.ot_overflow;
        }
    }

    return emitted;
}

/* ------------------------------------------------------------------------- */
/* Projectiles in flight — see the note in entitydraw.h                       */
/* ------------------------------------------------------------------------- */
/*
 * THE Z-ORDER SHUFFLE, which is the only thing this file does to the disc's own
 * face table.
 *
 * 0x8009D664 holds the six faces in the GPU's quad order: the hardware draws a
 * four-point primitive as the triangles (0,1,2) and (1,2,3), so the fourth
 * corner is diagonally opposite the first. This renderer's quads are perimeter
 * walks (raster.c, and world.c:1468 undoing the same shuffle for map faces), so
 * a corner list is converted by exchanging the last two — {0,1,2,3} becomes
 * {0,1,3,2}. Applied to both the indices and the colours together, which keeps
 * each colour on the corner the disc gave it.
 *
 * The corners themselves are bolt_shape, 0x8009DB1C:
 *
 *     0 (-10,-10,-50)  1 ( 10,-10,-50)  2 (-10,-10, 50)  3 ( 10,-10, 50)
 *     4 (-10, 10,-50)  5 ( 10, 10,-50)  6 (-10, 10, 50)  7 ( 10, 10, 50)
 */
static const int k_bolt_quad_order[4] = { 0, 1, 3, 2 };

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

/*
 * One rocket, grenade or BFG ball, as the model entity its spawner allocates.
 *
 * The spawner looks the model up by NAME in the level's CastList — "Rocket"
 * at 0x800ACBA4, "Grenade2" at 0x800ACB8C, "Grenade3" at 0x800ACB98 and
 * "BFGBlast" at 0x800ACBBC, through GetModelHdr (0x8006D008) —
 * and when the bank carries none it raises the hide bit in every viewport's
 * draw record (0x8004B064..0x8004B094), so the shot flies unseen. It draws
 * through the entity matrix like any model: RotMatrix of entity+0xE6, built
 * from the attitude the projectile keeps (projectile.h), at its origin.
 *
 * A rocket's light back colour is (127, 127, 127): the spawner copies
 * 0x800AE9A8 over both +0x2AC and +0x2B0 (0x8004B10C..0x8004B134) where every
 * other entity keeps the allocator's 0x40. A grenade keeps 0x40.
 */
static u32 projectile_model_build_ot(const q2_projectile *p, s32 index,
                                     const q2_entity_draw_ctx *ctx,
                                     const q2_camera *cam, psx_ot *ot,
                                     gte_state *gte)
{
    static const u8 rocket_glow[3] = { 0x7F, 0x7F, 0x7F };
    static const u8 plain_glow[3]  = { 0x40, 0x40, 0x40 };
    q2_model mdl;
    q2_model_instance inst;
    q2_model_draw_stats st;
    q2_light_env env;
    q2_model_pose pose[Q2_MODEL_REST_POSE_MAX];
    s16 spin[3][3];
    s32 node;
    int k;

    if (index < 0 || q2_model_get(ctx->bank, (u32)index, &mdl) != Q2_OK)
        return 0;

    q2_model_instance_init(&inst);
    inst.model     = &mdl;
    inst.pose      = q2_model_pose_rest(&mdl, pose, Q2_MODEL_REST_POSE_MAX)
                   ? pose : NULL;

    /* The BFG ball turns by its animation, at the position its think keeps
     * (q2_projectile_spin_bfg); everything else stands at the rest pose. */
    if (p->kind == Q2_PROJ_BFG && p->anim_end > 0 &&
        mdl.hdr.num_parts <= Q2_MODEL_REST_POSE_MAX) {
        q2_model_anim clip;
        u32 within = 0;

        if (q2_model_anim_at_held(&mdl, (u32)(p->anim > 0 ? p->anim : 0),
                                  &clip, &within, NULL) &&
            q2_model_pose_at(&mdl, &clip, within, pose) == Q2_OK)
            inst.pose = pose;
        inst.anim_pos = p->anim;
    }
    inst.origin[0] = p->pos[0];
    inst.origin[1] = p->pos[1];
    inst.origin[2] = p->pos[2];

    /* Through `rot`, as a gib and a debris piece are: see
     * q2_entity_draw_rotation for why not the instance's angle fields. */
    q2_rotation_euler(spin, p->angles[0], p->angles[1], p->angles[2]);
    inst.rot           = (const s16 (*)[3])spin;
    inst.scale         = Q2_ONE_12;
    inst.clut4_count_a = ctx->clut4_count_a;
    inst.tpage         = ctx->tpage;

    node = p->node;
    if (ctx->coll && node < 0)
        node = q2_coll_find_node(ctx->coll, p->pos, -1, true);
    if (ctx->coll && node >= 0) {
        q2_coll_node cell;

        if (q2_collision_get_node(ctx->coll, (u32)node, &cell))
            inst.sort_area = cell.contents & 0x7F;
    }
    for (k = 0; k < 3; k++) {
        inst.sort_bounds_min[k] = p->pos[k] - Q2_SWEEP_HALF_EXTENT;
        inst.sort_bounds_max[k] = p->pos[k] + Q2_SWEEP_HALF_EXTENT;
    }
    inst.sort_bounds_valid = true;

    if (ctx->lights) {
        q2_light_set set;

        q2_light_gather(&set, ctx->lights, p->pos, node, 0);
        q2_light_env_build(&env, &set, Q2_LIGHT_ONE, Q2_LIGHT_ONE,
                           p->kind == Q2_PROJ_ROCKET ? rocket_glow
                                                     : plain_glow);
        inst.light = &env;
    }

    q2_model_build_ot(&inst, cam, ot, gte, &st);
    return st.faces_emitted;
}

u32 q2_projectiles_build_ot(const struct q2_projectiles *list,
                            const q2_collision *coll,
                            const q2_camera *cam, psx_ot *ot, gte_state *gte,
                            const q2_entity_draw_ctx *models)
{
    const q2_weapon_tables *wt = q2_weapon_tables_builtin();
    u32 i, emitted = 0;
    gte_matrix rot;
    s32 rocket_model = -1, grenade_model = -1, hand_model = -1;
    s32 bfg_model = -1;

    if (!list || !cam || !ot || !gte)
        return 0;

    if (models && models->bank) {
        rocket_model  = q2_model_bank_find(models->bank, "Rocket");
        grenade_model = q2_model_bank_find(models->bank, "Grenade2");
        hand_model    = q2_model_bank_find(models->bank, "Grenade3");
        bfg_model     = q2_model_bank_find(models->bank, "BFGBlast");
    }

    /*
     * The models first: they install their own camera through
     * q2_model_build_ot, so the box pass below sets its own afterwards.
     */
    if (models && models->bank) {
        for (i = 0; i < Q2_PROJ_MAX; i++) {
            const q2_projectile *p = &list->p[i];

            if (!p->in_use)
                continue;
            if (p->kind == Q2_PROJ_ROCKET)
                emitted += projectile_model_build_ot(p, rocket_model, models,
                                                     cam, ot, gte);
            else if (p->kind == Q2_PROJ_GRENADE)
                emitted += projectile_model_build_ot(p, grenade_model, models,
                                                     cam, ot, gte);
            else if (p->kind == Q2_PROJ_HAND_GRENADE &&
                     p->node != Q2_PROJ_NODE_HELD)
                emitted += projectile_model_build_ot(p, hand_model, models,
                                                     cam, ot, gte);
            else if (p->kind == Q2_PROJ_BFG)
                emitted += projectile_model_build_ot(p, bfg_model, models,
                                                     cam, ot, gte);
        }
    }

    /*
     * The emitter owns the camera it projects through; it does not inherit one.
     * On the console every emitter installs the same frame camera before it
     * draws — 0x8003058C and 0x80047CA4 are both `SetRotMatrix(view + 160)`,
     * the matrix 0x80037F38 builds through 0x80055DE4 and the world draw loads
     * at 0x800313CC. This pass used to run on whatever `q2_fx_build_ot` left
     * behind (src/client/main.c calls it immediately before), which meant bolts
     * picked up that pass's camera by accident and moved with it.
     *
     * Nothing inside the loop disturbs either register set:
     * q2_camera_apply_area_projection touches only the projection, and
     * bolt_basis applies the projectile's own orientation in software.
     */
    q2_rotation_view_anamorphic(rot.m, cam->yaw, cam->pitch, cam->roll);
    gte_set_rotation(gte, &rot);
    gte_set_translation(gte, 0, 0, 0);

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

        /* Drawn as their models above, or hidden as the console hides one
         * whose model the level does not carry. */
        if (models && models->bank &&
            p->kind != Q2_PROJ_BOLT)
            continue;

        /*
         * AND A BLASTER BOLT HAS NO BODY.
         *
         * 0x80047F44 `andi v0, v0, 0x4` on the flags at record+0x22 gates the
         * whole GTE block that projects the eight corners, and 0x8004D7A4 is
         * the same test in the spawner deciding whether to write them at all.
         * The blaster passes 11 and the hyperblaster 14 (projectile.h), so the
         * bit is set for one and clear for the other: the hyperblaster's bolt
         * is this box and the blaster's is its trail (effect.h), and they are
         * mutually exclusive.
         *
         * A bolt is the only projectile that carries flags — the four class
         * spawners take no such argument — so everything else keeps the body
         * it has always been drawn with.
         */
        if (p->kind == Q2_PROJ_BOLT && !(p->flags & Q2_PROJ_FLAG_BODY))
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

        /*
         * ONLY THE KINDS THIS PORT DRAWS A BOX FOR AT ALL take a flat tint.
         *
         * A bolt's colours are the disc's own, per corner, out of the face
         * table below. The other four kinds are named-class entities on the
         * console with models of their own and no missile record, and draw
         * them above when the caller passes a bank. Without one this box is
         * the stand-in, with the glow it was given — the preset at 0x800AE954
         * that their dynamic light also reads.
         */
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

        for (f = 0; f < Q2_WT_BOLT_FACES; f++) {
            const q2_wt_bolt_face *face = &wt->bolt_face[f];
            psx_prim *prim;
            u32 depth = 0;
            bool good = true;
            int c;

            for (c = 0; c < 4; c++) {
                if (!ok[face->idx[c]]) { good = false; break; }
                depth += z[face->idx[c]];
            }
            if (!good)
                continue;

            /*
             * BACKFACE, the console's own test and its own sense.
             *
             * 0x800B1EB0 runs `nclip` on the first three corners IN THE
             * TABLE'S ORDER — before the shuffle above — and 0x800B1EBC
             * `bgez v0, 0x800B1F54` SKIPS the emit when MAC0 is zero or
             * positive. So a face is drawn when the cross product is
             * negative, which is the opposite of the reading this file used to
             * carry, and it is what keeps the far side of an opaque box out of
             * the table.
             */
            {
                const gte_sxy *a = &xy[face->idx[0]];
                const gte_sxy *b = &xy[face->idx[1]];
                const gte_sxy *c2 = &xy[face->idx[2]];
                s32 nclip = ((s32)b->x - a->x) * ((s32)c2->y - a->y) -
                            ((s32)c2->x - a->x) * ((s32)b->y - a->y);

                if (nclip >= 0)
                    continue;
            }

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

            /*
             * OPAQUE AND GOURAUD for a bolt, because that is what 0x800B1E28
             * emits: nine words with four per-vertex colours, no UVs, and the
             * command byte 0x38 that carries no ABE bit. The box is solid and
             * shades from its (255,64,0) tail to its (255,255,0) nose.
             *
             * The other kinds keep the additive flat quad they have always had
             * — it is this port's own stand-in for a model, and the disc has no
             * primitive to copy for them.
             */
            if (p->kind == Q2_PROJ_BOLT) {
                prim->kind             = PSX_PRIM_G4;
                prim->semi_transparent = false;
                prim->tpage            = 0;
            } else {
                prim->kind             = PSX_PRIM_F4;
                prim->semi_transparent = true;
                prim->tpage            = Q2_FX_ABR_ADD;
                prim->rgb[0]           = tint;
            }

            for (c = 0; c < 4; c++) {
                const int s = k_bolt_quad_order[c];

                if (p->kind == Q2_PROJ_BOLT) {
                    prim->rgb[c].r   = face->rgb[s][0];
                    prim->rgb[c].g   = face->rgb[s][1];
                    prim->rgb[c].b   = face->rgb[s][2];
                    prim->rgb[c].pad = 0;
                }

                /* gte_sxy and psx_xy share a layout but are distinct types;
                 * reading one through the other is undefined. */
                prim->xy[c].x = xy[face->idx[s]].x;
                prim->xy[c].y = xy[face->idx[s]].y;
            }

            emitted++;
        }
    }

    return emitted;
}

/* ------------------------------------------------------------------------- */
/* Debris                                                                     */
/* ------------------------------------------------------------------------- */
u32 q2_fx_debris_build_ot(const struct q2_fx_world *fx,
                          const q2_model_bank *bank,
                          const q2_collision *coll,
                          const q2_light_world *lights,
                          const q2_tpage_table *tpage, u32 clut4_count_a,
                          const q2_camera *cam, psx_ot *ot, gte_state *gte)
{
    u32 i, emitted = 0;

    if (!fx || !bank || !cam || !ot || !gte)
        return 0;

    for (i = 0; i < Q2_FX_DEBRIS_MAX; i++) {
        const q2_fx_debris *d = &fx->debris[i];
        q2_model  mdl;
        q2_model_instance inst;
        q2_model_draw_stats st;
        q2_light_env env;
        s16 spin[3][3];
        s32 node;
        int k;

        /* A level that registered no Debris model spawns pieces with model -1
         * — the console's own answer on the 30 banks that carry none — and
         * those really do draw nothing. */
        if (!d->in_use || d->model < 0)
            continue;
        if (q2_model_get(bank, (u32)d->model, &mdl) != Q2_OK)
            continue;

        q2_model_instance_init(&inst);
        inst.model     = &mdl;
        inst.origin[0] = d->pos[0];
        inst.origin[1] = d->pos[1];
        inst.origin[2] = d->pos[2];

        /*
         * THE ATTITUDE, THROUGH `rot` AND NOT THE ANGLE FIELDS.
         *
         * A piece's `spin[3]` is entity+0xE6, the same SVECTOR a gib tumbles
         * with, and both chunk spawners hand it straight to RotMatrix
         * (0x8005A30C, 0x8005AFD0). The instance's pitch/yaw/roll path is
         * `q2_rotation_euler(pitch, -yaw, roll)` (modeldraw.c, instance_spin),
         * which negates the middle angle for the world's yaw convention — so a
         * chunk sent through it would be turned the wrong way (entitydraw.h).
         * Build the matrix directly, exactly as a Q2_RF_TRANSIENT entity does
         * above.
         */
        q2_rotation_euler(spin, d->spin[0], d->spin[1], d->spin[2]);
        inst.rot   = (const s16 (*)[3])spin;
        inst.scale = Q2_ONE_12;
        inst.clut4_count_a = clut4_count_a;
        inst.tpage         = tpage;

        /*
         * Where it sorts. The physics loop already keeps `node` current
         * (simcombat.c commits it on every step), so this costs a hull search
         * only for a piece whose cell is not known yet — and `area` is
         * entity+0x9E, the byte the burst was raised with, which is the
         * fallback when there is no cell at all.
         */
        node = d->node;
        if (coll && node < 0)
            node = q2_coll_find_node(coll, d->pos, -1, true);
        inst.sort_area = (s32)(d->area & 0x7F);
        if (coll && node >= 0) {
            q2_coll_node cell;

            if (q2_collision_get_node(coll, (u32)node, &cell))
                inst.sort_area = cell.contents & 0x7F;
        }
        for (k = 0; k < 3; k++) {
            inst.sort_bounds_min[k] = d->pos[k] - Q2_SWEEP_HALF_EXTENT;
            inst.sort_bounds_max[k] = d->pos[k] + Q2_SWEEP_HALF_EXTENT;
        }
        inst.sort_bounds_valid = true;

        if (lights) {
            q2_light_set set;
            /* A chunk is an ordinary entity: the pool allocator seeds
             * entity+0x2AC at 0x40 (0x8006C1D8) and nothing on this path
             * replaces it, so that is what its back colour is. */
            static const u8 glow[3] = { 0x40, 0x40, 0x40 };

            q2_light_gather(&set, lights, d->pos, node, 0);
            q2_light_env_build(&env, &set, Q2_LIGHT_ONE, Q2_LIGHT_ONE, glow);
            inst.light = &env;
        }

        q2_model_build_ot(&inst, cam, ot, gte, &st);
        emitted += st.faces_emitted;
    }

    return emitted;
}
