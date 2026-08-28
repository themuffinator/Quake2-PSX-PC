/*
 * entitydraw.h — putting the entity set into the ordering table.
 *
 * modeldraw.c draws ONE model instance. This walks a whole entity set, resolves
 * each entity's model out of the map's CastList, applies the state its think
 * left behind — the spin, the materialise intensity, the glow — and appends
 * the lot to the same ordering table the world was built into.
 *
 * That last part is why this is a separate module and not something the client
 * does inline: the primitives have to land in the SAME table as the world
 * geometry, or the sort cannot interleave an item with the crate it sits behind.
 * The original has exactly one table per frame and so does this.
 *
 * ---------------------------------------------------------------------------
 * What is faithful here and what is not
 * ---------------------------------------------------------------------------
 * FAITHFUL: +0xFC/+0xFE reach the GPU through the GTE light matrix and back
 * colour (`0x8006B298`, `0x8006B468`) without changing geometry; the yaw is
 * the entity's own +0xE8 on the 4096-step circle; the origin is +0xA4, which
 * the spawner already biased by 286 and the model's own offset.
 *
 * NOT YET: the sparkle particles the materialise ramp emits. The translucent
 * floor shadow is reconstructed from the item table's posed-vertex list and
 * the real 16x16 tile/palette in `chars.lbm` / the executable palette bank.
 * An item that has not finished materialising therefore brightens through the
 * decoded lighting ramp and casts its retail shadow, but has no sparkles yet.
 */
#ifndef Q2PSX_ENTITYDRAW_H
#define Q2PSX_ENTITYDRAW_H

#include "entity.h"
#include "gpu.h"
#include "gte.h"
#include "lighting.h"
#include "model.h"
#include "modeldraw.h"
#include "q2psx.h"
#include "world.h"

typedef struct q2_entity_draw_stats {
    u32 considered;
    u32 drawn;
    u32 no_model;      /* the bank does not carry the name the table gave    */
    u32 invisible;     /* hidden or collected                                */
    u32 faces_emitted;
    u32 shadows_emitted;
    u32 ot_overflow;
} q2_entity_draw_stats;

typedef struct q2_entity_draw_ctx {
    const q2_model_bank *bank;      /* the map's CastList                    */
    u32                  clut4_count_a;  /* model palettes start after these */

    /*
     * The world's texture-page table, or NULL for a private one at ABR 0.
     *
     * An entity standing in a world frame has to share it: the world's opaque
     * path PROMOTES a page from ABR 0 to ABR 1 and writes the result back
     * (0x80068320), so an item on a page the world has already drawn on inherits
     * that promotion. A private table would blend it at the pre-promotion mode
     * — see the note on q2_model_instance.tpage in modeldraw.h. NULL is right
     * for an offline caller that draws no world.
     */
    const q2_tpage_table *tpage;

    /* Which player's view this is. An item already collected by that player is
     * invisible to them and to nobody else, which is the +0x118 per-player
     * block's whole purpose. */
    u32                  player;

    /*
     * The lights of the zone the entities stand in, or NULL to draw everything
     * at its flat tint as this module did before lighting existed.
     *
     * When it is present each entity gets its own gather — the engine does one
     * per entity per frame (0x8006BBCC, called from the draw itself), not one
     * per frame — and shades through the GTE.
     *
     * `coll` is the hull the gather resolves each entity's own cell in, which
     * is what the engine's entity+0xA2 holds. Pass it and every item is lit by
     * the lights of the room it is actually standing in.
     *
     * `coll_node` is the fallback for a caller that has no hull: one node for
     * the whole set, usually the player's. That was all this context used to
     * carry, so an item in the next room took the player's lights, and on any
     * frame where the camera resolved to no cell at all — which happens
     * routinely — every item in the level fell back to the grey default.
     * Negative still means the fallback light.
     */
    const q2_light_world *lights;
    const q2_collision   *coll;
    s32                   coll_node;
} q2_entity_draw_ctx;

/*
 * Draw every visible entity. The ordering table is NOT cleared and the GTE's
 * projection is NOT reconfigured, so this composes after q2_world_build_ot.
 *
 * Returns the number of primitives emitted.
 */
u32 q2_entity_build_ot(q2_entity_set *set, const q2_entity_draw_ctx *ctx,
                       const q2_camera *cam, psx_ot *ot, gte_state *gte,
                       q2_entity_draw_stats *stats);

/*
 * Resolve one entity's model against the bank and cache the result, and fill in
 * the clip length its think wraps the frame against.
 *
 * Called by q2_entity_build_ot, and exposed because a caller that has the bank
 * open at spawn time should do it then: the engine resolves at spawn
 * (0x80058850) and an item whose name is not in the map's bank never spawns at
 * all. Returns false when the bank does not carry the name.
 */
bool q2_entity_resolve_model(q2_entity *e, const q2_model_bank *bank);

/* ------------------------------------------------------------------------- */
/* Projectiles in flight                                                      */
/* ------------------------------------------------------------------------- */
/*
 * The bolt's own body, as the eight corners of a 20 x 20 x 100 box oriented
 * along its direction of travel.
 *
 * WHERE THE GEOMETRY COMES FROM, and what is inferred. The eight local points
 * are `0x8009DB1C`, an executable table this port already decodes
 * (weapontables.h). The spawner at 0x8004D70C builds a rotation from the
 * bolt's direction (`jal 0x80089E38`, RotMatrix) and rotates all eight into
 * the ENTITY, at +12, +20, +28, +36, +44, +52, +60 and +68 — eight SVECTORs,
 * one `jal 0x8006FC1C` each, at 0x8004D848 through 0x8004D8B8.
 *
 * What is NOT established is that the console DRAWS them. It was previously
 * recorded as a collision hull, on the strength of the very next instruction
 * block: 0x8004D8D4 calls q2_coll_probe_point. But that call takes `a1 =
 * s2 + 76`, which is the field AFTER the eight corners, not the corners
 * themselves — so the probe is not their consumer, and nothing else in the
 * three references to the missile array (0x80047CD4, 0x80048B08, 0x80048C58)
 * reads them either.
 *
 * So: the geometry is the disc's, the orientation is the disc's, and the
 * conclusion that these corners are what a bolt looks like is an INFERENCE
 * from "they are written per bolt and nothing else reads them". It is marked
 * here rather than presented as a transcription. Without it a bolt is
 * invisible and only its dynamic light exists, which is worse.
 *
 * The colour is the projectile's own glow from 0x800AE954 — the same preset
 * the light uses — so nothing about the appearance is invented beyond the
 * decision to draw it at all.
 *
 * Returns the number of primitives emitted.
 */
struct q2_projectiles;

u32 q2_projectiles_build_ot(const struct q2_projectiles *list,
                            const q2_collision *coll,
                            const q2_camera *cam, psx_ot *ot, gte_state *gte);

#endif /* Q2PSX_ENTITYDRAW_H */
