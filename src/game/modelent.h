/*
 * modelent.h — MODEL ENTITIES: a CastList model spawned into the world as a
 * short-lived actor, with its own think, its own clock and its own death.
 *
 * The port already had model entities in one narrow sense — an ITEM is one, and
 * `entitydraw.c` will draw anything in the set that names a model. What it did
 * not have is the thing the engine calls when something needs to APPEAR: a
 * transient entity that is not an item, has no pickup, and removes itself when
 * its clip runs out. `explosive.h` had to say so out loud:
 *
 *     "0x8005A778 spawns a MODEL ENTITY ... This port has no such subsystem"
 *
 * This is that subsystem. `0x8005A778` is its only decoded caller so far and
 * the creature import table already carries the address under the name
 * `spawn_explosion` (creature.h, +0x120), so every creature module on the disc
 * can reach it too.
 *
 * ---------------------------------------------------------------------------
 * The spawn — 0x8005A778(origin, surface, radius, kind)
 * ---------------------------------------------------------------------------
 *   1. `0x8006C098(1)` allocates out of the 48-entry, 768-byte pool at
 *      0x800CBA28. The argument is "you may EVICT to make room": with fewer
 *      than 48 free the allocator scans the pool for an entity whose +0xF4 is
 *      non-zero AND whose +0x10C carries bit 0x01000000, frees that one
 *      (0x8006D280) and takes its slot. So a transient entity is exactly one
 *      that carries those two marks, and the marks are what make it
 *      recyclable under pressure rather than a leak.
 *
 *   2. The name is chosen by the fourth argument and there are only two:
 *      0 -> "Explosion" (0x800ACDF4), 1 -> "Hexplosion" (0x800ACE00). Anything
 *      else skips the bind entirely and lands on the `ent+0x10 == 0` test
 *      below, which throws the entity away.
 *
 *      `Hexplosion` IS ON NO MAP. 34 of the disc's 49 CastLists carry
 *      `Explosion` and 0 of 164 banks carry `Hexplosion`, so kind 1 is a
 *      binding with nothing behind it — every caller that asks for it gets
 *      the same nothing an unknown kind gets. Opcode 0x08 always passes 0
 *      (explosive.h), which is consistent with that and not a coincidence.
 *      The other 15 maps are the front-end and cinematic containers.
 *
 *   3. `0x8006D008` walks the model list at gp+18196 comparing TWELVE BYTES at
 *      record+8 and returns the record, or zero. The result goes to ent+0x10,
 *      and `0x8005A894` bails the whole spawn when it is zero — a map whose
 *      CastList does not carry the name gets NO entity, not an invisible one.
 *
 *   4. ent+0x3C = 0x8005A5F8, the think below.
 *
 *   5. ent+0xF8 = `0x8006D100(entity)`, which is `lh(model + 0x1C)` — the
 *      model's own vertical extent. The box is built from it:
 *
 *          mins = (-256, h - 512, -256)      0x8005A97C..0x8005A99C
 *          maxs = ( 256, h,        256)      0x8005A9A4..0x8005A9CC
 *
 *      and then ent+0x78..0x8C is that box biased by the origin at ent+0x54,
 *      which is `entity_absolute_bounds` done inline.
 *
 *   6. ent+0x90 = 128, ent+0x9E = the SURFACE byte (the second argument),
 *      ent+0xA0 = ent+0xA2 = -1.
 *
 * `explosive.h`'s caller passes `scene[node].area & 0x7F` for the surface and
 * a fixed 4096 for the radius, so both of those are already decoded on the
 * other side.
 *
 * ---------------------------------------------------------------------------
 * The think — 0x8005A5F8
 * ---------------------------------------------------------------------------
 *     ent[+0x100] += 2 * dt                       ; dt is 0x800B2DB4
 *     if (ent[+0x100] >= lh(model + 2) * 10)   ; +2 is the TOTAL clip length
 *         remove(ent)                             ; 0x8006D280
 *     ent[+0xFE] = clamp(25 * (320 - ent[+0x100]), 0, 4096)
 *     outer = clamp(51 * (320 - t), 0, 4096) * 1300 / 4096
 *     light(ent+0xA4, C0/40/31, outer * 3 / 4, outer, style 0)
 *
 * THREE THINGS ABOUT THAT ARE LOAD-BEARING.
 *
 * **The clock is the port's `frame` and it does NOT wrap.** +0x100 is the same
 * field an item advances (`e->frame += w->dt`, item.c) and the same one
 * `entitydraw` turns into an animation frame. An item wraps it against
 * `clip_length`; this does not — it runs past the end and the entity dies
 * instead. Wrapping it would give an explosion that loops for ever.
 *
 * **It advances at TWICE the item rate.** `sll v1, v1, 1` at 0x8005A618, before
 * the add. So a 40-frame clip is 400 clock units and 200 ticks of `dt`, not
 * 400.
 *
 * **The lifetime is TOTAL-ANIMATION-LENGTH x 10.** `a0 * 4 + a0` then doubled,
 * at 0x8005A630, over `lh(model + 2)` — which is the sum of every clip's
 * frames and not the current clip's, on 1,723 of 1,723 models
 * (`q2psx-inspect modelents`). It matters only for a multi-clip effect model,
 * and `Explosion` has one clip of 40 — so it lives 400 units. Its lighting
 * ramp has already reached zero by then, as the code below records.
 *
 * ---------------------------------------------------------------------------
 * +0xFE is a SECOND lighting intensity, and the draw multiplies the two
 * ---------------------------------------------------------------------------
 * `0x8006B298` builds the GTE light matrix from BOTH:
 *
 *     8006B298  lh   v1, 252(s2)      ; +0xFC, the one this port already has
 *     8006B29C  lh   v0, 254(s2)      ; +0xFE, the one it did not
 *     8006B2A4  mult v1, v0
 *     8006B2BC  sra  a1, v1, 11       ; ...and the product scales each row
 *
 * The destination passed as `a1` is 0x800DDD1C; 0x8006BBD4 immediately installs
 * it with SetLightMatrix. The model rotation installed next is independently
 * composed from entity+0x2C0. At 0x8006B468 the same product scales the ambient
 * back colour, so +0xFE changes illumination and never geometry.
 *
 * That is why `q2_entity.fade` defaults to Q2_ONE_12 and not to zero: an
 * entity that forgets to set it would otherwise render black.
 *
 * The ramp itself is `25 * (320 - t)` clamped into [0, 4096]. It sits at the
 * ceiling until t = 156 and then falls to nothing at t = 320 — so an explosion
 * holds full brightness for the first 39% of its life and darkens over the
 * rest. Its mesh does not shrink.
 *
 * ---------------------------------------------------------------------------
 * The tail is a dynamic light, not a sprite
 * ---------------------------------------------------------------------------
 * The think's tail (0x8005A6E4..0x8005A764) computes a second ramp —
 * `clamp(51 * (320 - t), 0, 4096) * 1300 / 4096`, then three quarters of it —
 * and hands both radii to `0x80075C34`. That address is the runtime-light
 * appender reconstructed as `q2_light_add_dynamic`: it writes one 28-byte
 * light into the sixteen-entry world list bounded by 0x800E3ED8. It is not a
 * primitive emitter and does not draw a translucent quad.
 *
 * The colour operand at 0x800AEAD4 is C0/40/31 and the style/size bytes at
 * 0x800AEAB4 are both zero. The port raises that exact light through the
 * entity-event seam every tick, so the explosion model lights actors around
 * it just as retail did.
 */
#ifndef Q2PSX_MODELENT_H
#define Q2PSX_MODELENT_H

#include "entity.h"
#include "model.h"
#include "q2psx.h"

/*
 * The two names 0x8005A778 can bind, in the order its fourth argument selects
 * them. There is no third: any other value falls through to the bail.
 */
typedef enum q2_model_ent_kind {
    Q2_MODEL_ENT_EXPLOSION = 0,   /* "Explosion",  0x800ACDF4 */
    Q2_MODEL_ENT_HEXPLOSION,      /* "Hexplosion", 0x800ACE00 */
    Q2_MODEL_ENT_KIND_COUNT
} q2_model_ent_kind;

const char *q2_model_ent_name(q2_model_ent_kind kind);

/* The box, straight off 0x8005A97C and 0x8005A9A4. `h` is the model's own
 * `lh(model + 0x1C)`, which q2_model_ent_height reads. */
#define Q2_MODEL_ENT_HALF_WIDTH 256
#define Q2_MODEL_ENT_HEIGHT     512

/* ent+0x90, written with 128 at 0x8005A9D8. No reader is decoded yet. */
#define Q2_MODEL_ENT_FIELD90    128

/* 0x800AEAC8, copied to entity+0x2AC by 0x8005A8E4..0x8005A910. */
#define Q2_MODEL_ENT_AMBIENT    0x40

/* The think's three constants — see the header comment. */
#define Q2_MODEL_ENT_CLOCK_RATE 2      /* 0x8005A618: dt is doubled       */
#define Q2_MODEL_ENT_LIFE_MUL   10     /* 0x8005A630: clip_length x 10    */
#define Q2_MODEL_ENT_RAMP_BASE  320    /* 0x8005A654                      */
#define Q2_MODEL_ENT_RAMP_SCALE 25     /* 0x8005A668, the model's ramp    */
#define Q2_MODEL_ENT_FLASH_SCALE 51    /* 0x8005A69C, the light radius    */


/*
 * The model's own vertical extent, `lh(model + 0x1C)` via 0x8006D100.
 *
 * Returns false when the bank does not carry `name`, which is the condition
 * 0x8005A894 throws the whole spawn away on.
 */
bool q2_model_ent_height(const q2_model_bank *bank, const char *name,
                         s32 *out_height, s32 *out_clip_length,
                         s32 *out_index);

/*
 * Spawn one — 0x8005A778.
 *
 * `at` is the world point the effect happens at; `surface` is the byte the
 * caller wants at ent+0x9E (opcode 0x08 passes the Scene node's `area & 0x7F`).
 *
 * Returns the entity, or NULL when the pool is full or the map's CastList does
 * not carry the name. A NULL return is a real outcome and not an error: three
 * of the disc's banks carry no `Explosion` at all.
 */
q2_entity *q2_model_ent_spawn(q2_entity_set *set, const q2_model_bank *bank,
                              q2_model_ent_kind kind, const s32 at[3],
                              u8 surface);

/* The think at 0x8005A5F8, installed by the spawn. Exposed so a test can run
 * one tick of it without a world. */
void q2_model_ent_think(q2_entity *e, q2_entity_world *w);

/*
 * The two ramps, separated so both are testable and neither is buried.
 *
 * `q2_model_ent_scale` is the historically named +0xFE lighting ramp.
 * `q2_model_ent_flash` is the dynamic light's outer radius; its think emits
 * three quarters of it as the inner radius.
 */
s32 q2_model_ent_scale(s32 clock);
s32 q2_model_ent_flash(s32 clock);

/* How long an entity playing a clip of `frames` lives, in clock units. */
s32 q2_model_ent_lifetime(s32 clip_length);

#endif /* Q2PSX_MODELENT_H */
