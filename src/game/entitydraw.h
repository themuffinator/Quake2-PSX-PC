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
 * Materialise sparkles are emitted by the client's entity-event consumer,
 * using the posed model through q2_model_world_vertex. This draw also handles
 * the translucent floor shadow and the Blackhole P's attached glint chain.
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
    u32 outside_zone;  /* no cell of the loaded zone's hulls holds it        */
    u32 faces_emitted;
    u32 shadows_emitted;
    u32 glint_emitted;
    u32 glint_draws;
    u32 ot_overflow;
} q2_entity_draw_stats;

typedef struct q2_entity_draw_ctx {
    const struct q2_fx_glint *glint;
    const q2_model_bank *bank;      /* the map's CastList                    */
    const q2_model_bank *zone_bank; /* resident zone's CastList fallback     */
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
     *
     * `coll_primary` is the un-eroded hull, consulted only when no `coll`
     * cell holds an item: an origin just inside the eroded floor is still in
     * a PrimaryColl cell, whose byte +32 names the same area (the client's
     * client_cre_area has the census), and an item in neither hull is not
     * in this zone and is not drawn. Without it such an item has no area,
     * and no area is global depth — drawn over every wall in front of it.
     */
    const q2_light_world *lights;
    const q2_collision   *coll;
    const q2_collision   *coll_primary;
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

/*
 * THE MATRIX A TRANSIENT MODEL ENTITY IS DRAWN WITH, which for a gib is all
 * three of its angles and not the yaw alone.
 *
 * The console draws every entity through the matrix at entity+0x2C0 — nine
 * `lhu` at 0x8006BB28..0x8006BB8C, composed with the camera at 0x8006BB94 —
 * and a gib rebuilds that matrix as RotMatrix of the whole +0xE6 SVECTOR:
 * 0x8005A31C at its spawn, 0x80046CA8 / 0x80046CBC on every toss, the pitch
 * and roll its tumble turns included. Drawing only +0xE8 kept each chunk's
 * heading and dropped the tumble.
 *
 * A Q2_RF_TRANSIENT entity therefore gets q2_rotation_euler(+0xE6, +0xE8,
 * +0xEA), which is RotMatrix element for element (trig.c), and this returns
 * true with it in `out`. Everything else returns false, leaves `out` alone and
 * keeps the instance's yaw-only path exactly as it was. The explosion family
 * is the other transient: the port's never turns (every angle stays at the
 * allocator's zero), and at zero both forms are the identity, so its draw does
 * not change either.
 *
 * NOT through q2_model_instance's own pitch/roll fields. That path builds
 * q2_rotation_euler(pitch, -yaw, roll) (modeldraw.c, instance_spin): the yaw
 * negated as q2_rotation_yaw_pitch needs it to come out as RotMatrix(0, yaw,
 * 0), which q2_rotation_euler does not, so a chunk drawn through it would
 * have its heading turned the wrong way. The explicit matrix is RotMatrix's
 * own sense, the one the yaw-only path already reproduces.
 */
bool q2_entity_draw_rotation(const q2_entity *e, s16 out[3][3]);

/* ------------------------------------------------------------------------- */
/* Projectiles in flight                                                      */
/* ------------------------------------------------------------------------- */
/*
 * The bolt's own body, as the eight corners of a 20 x 20 x 100 box oriented
 * along its direction of travel.
 *
 * NO LONGER AN INFERENCE. This note used to say the console might not draw
 * these corners, because the search for a reader stopped at the gate that
 * guards it. The whole chain is read now:
 *
 *   0x8004D7A4  `andi v0, s1, 0x4` — the spawner writes the corners at all
 *               only when the flags it was handed carry bit 0x4. It builds the
 *               rotation from the bolt's direction (`jal 0x80089E38`,
 *               RotMatrix) and rotates the eight points of 0x8009DB1C into the
 *               record at +12, +20, +28, +36, +44, +52, +60 and +68, one
 *               `jal 0x8006FC1C` each, 0x8004D848 through 0x8004D8B8.
 *   0x80047F44  `andi v0, v0, 0x4` — the same bit of the same halfword, and
 *               the per-view arm behind it is the reader: 0x80048088 `lwc2`
 *               the corners in three `rtpt` batches (corner 5 projected twice,
 *               into the same slot), 0x8004813C `swc2 SZ3` for the sort depth.
 *   0x80048160  `jal 0x800B1E28(0x8009D664, 0x8009D6C8, sp+40, [0x800B2744])`
 *               — the emitter, over the six-face table (weapontables.h).
 *   0x80048194  `jal 0x80064FAC` links the result into the ordering table.
 *
 * So the geometry is the disc's, the orientation is the disc's, and so is the
 * decision to draw it. The colour is NOT 0x800AE954 — that preset is read only
 * by the two dynamic-light arms at 0x800481CC and 0x80048244. A bolt's body
 * carries its own four colours per face, in the GPU header words of
 * 0x8009D664, and it is opaque.
 *
 * AND A BLASTER BOLT HAS NO BODY. Bit 0x4 is clear in the 11 the blaster
 * passes and set in the 14 the hyperblaster passes, so one is this box and the
 * other is the particle trail bit 0x1 gives it instead (effect.h). The two are
 * mutually exclusive across all three callers of 0x8004D70C.
 *
 * ONLY A HYPERBLASTER BOLT IS THIS BOX. Every other projectile's spawner
 * allocates a model entity out of the level's CastList — "Rocket"
 * (AnyoneFireRocketL, 0x8004AF7C), "Grenade2" (AnyoneFireGrenadeL,
 * 0x8004A100), "Grenade3" (AnyoneFireGrenade, 0x8004AAB0) and "BFGBlast"
 * (AnyoneFireBFG, 0x8004BE40) — so with `models` given, those draw as their
 * models at the attitude the projectile keeps (projectile.h), the BFG ball
 * posed at its spin, lit with `models->lights`; and draw nothing on a level
 * whose bank lacks the model, which is the spawner's own hide arm. `models`
 * NULL keeps the box for every kind, as an offline caller with no bank needs.
 *
 * Returns the number of primitives emitted.
 */
struct q2_projectiles;

u32 q2_projectiles_build_ot(const struct q2_projectiles *list,
                            const q2_collision *coll,
                            const q2_camera *cam, psx_ot *ot, gte_state *gte,
                            const q2_entity_draw_ctx *models);

/* ------------------------------------------------------------------------- */
/* Debris — the pieces a shattered pane or a destroyed brush group throws      */
/* ------------------------------------------------------------------------- */
/*
 * Every live piece of `fx`'s debris pool, appended to `ot`.
 *
 * These are real entities on the console — 0x80064558 spawns them through
 * 0x80064398 and 0x80064124 thinks them — with a model taken from the 32-slot
 * registration list at 0x800D56B0, so they are drawn by the ordinary entity
 * pass. This port stepped, bounced and expired them and drew none of them:
 * q2_fx_build_ot emits groups and beams and nothing else, and the client's
 * entity pass does not know the pool exists. `q2psx-inspect explosives` counts
 * 94 bursts and 1,597 pieces across the disc, so it is every crate on eighteen
 * maps as well as the ten GLASS panes.
 *
 * `bank` resolves `q2_fx_debris.model`, which is an index into it —
 * q2_fx_debris_register stores indices where the console's list stores model
 * pointers. `coll` and `lights` may be NULL; a piece then sorts on the area
 * byte it was spawned with and draws unlit.
 *
 * Returns the number of faces emitted.
 */
struct q2_fx_world;

u32 q2_fx_debris_build_ot(const struct q2_fx_world *fx,
                          const q2_model_bank *bank,
                          const q2_collision *coll,
                          const q2_light_world *lights,
                          const q2_tpage_table *tpage, u32 clut4_count_a,
                          const q2_camera *cam, psx_ot *ot, gte_state *gte);

#endif /* Q2PSX_ENTITYDRAW_H */
