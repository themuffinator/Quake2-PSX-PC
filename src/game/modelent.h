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
 * This is that subsystem. `0x8005A778` was its first decoded caller and the
 * creature import table already carries the address under the name
 * `spawn_explosion` (creature.h, +0x120), so every creature module on the disc
 * can reach it too. The GIBS are the second family and have their own section
 * at the end of this file: 0x8007CEB4, 0x8005A3D4, 0x8005A0AC, 0x8005AD8C,
 * 0x8005B0AC and the gib think 0x80059DE0.
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
 *
 * ---------------------------------------------------------------------------
 * The mesh is ADDITIVE, and nothing in this file is what makes it so
 * ---------------------------------------------------------------------------
 * The `Explosion` model's faces are opaque on the disc and additive in RAM. The
 * entity class table has a record for the name at 0x800A3CF8 whose +0x14 is
 * `PreInitExplosion` (0x8005AAA4), `RemapModels` (0x8007E6E4) calls it once a
 * level with the model header, and it forces every face's blend selector to 2 —
 * B+F. So the fireball this entity draws lets the wall behind it through, and a
 * spawn is not the thing that decides that: the model is already additive before
 * the first one is raised, for every reader.
 *
 * It lives at the face decode (`q2_model_get_face`, model.h) rather than here
 * for exactly that reason. The gib dispatcher's Boss2/Flyer/Hover and Strider
 * arms raise the same model through 0x8005A778, and the console patched the
 * faces long before any of them ran.
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
 * `size` is the third argument, 1.0.12: Q2_MODEL_ENT_SIZE_FULL from every
 * caller but the gib dispatcher's two Explosion arms, which pass half.
 *
 * A spawn that binds its model ends in two calls, and both sit past
 * 0x8005A894's bail, so a map whose CastList has no `Explosion` makes neither:
 *
 *   0x8005AA54  SoundByCreature(Sound_Explosion, ent) — the explosion's own
 *               bang, queued on `sounds` (below) for the owner to play;
 *   0x8005AA7C  SetShockData — every pad within `size` of `at` rumbles
 *               (q2_shock_explosion).
 *
 * NOT REPRODUCED: the other thing `size` does. 0x8005A96C hands it to
 * 0x80055F3C with the entity's matrix at +0x2C0 as both source and
 * destination, which scales the model by it, so the gib arms' fireball is
 * drawn at half the size of a rocket's. q2_entity has no per-entity draw
 * scale (`scale` is the +0xFC light intensity), and every one is drawn at
 * 4096.
 *
 * `sounds` may be NULL, for a caller with nobody to hear it; the entity is
 * spawned either way.
 *
 * Returns the entity, or NULL when the pool is full or the map's CastList does
 * not carry the name. A NULL return is a real outcome and not an error: three
 * of the disc's banks carry no `Explosion` at all.
 */
#define Q2_MODEL_ENT_SIZE_FULL 4096   /* `addiu a2, zero, 4096` at six sites */
#define Q2_MODEL_ENT_SIZE_GIB  2048   /* 0x8007CF18, 0x8007D010              */

/*
 * THE EXPLOSION'S OWN SOUND — 0x8005AA44..0x8005AA54.
 *
 *     8005AA44  lw   a0, 1180(gp)       ; Sound_Explosion, 0x800AEA9C
 *     8005AA48  addu a1, s1, zero       ; the entity just bound
 *     8005AA54  jal  0x8007270C         ; SoundByCreature
 *
 * Sound_Explosion has two references in the image: that load, and the store
 * at 0x8005AB5C that ends PreInitExplosion (0x8005AAA4), which hands
 * GetSoundNum (0x80073518) the twelve-byte key at 0x800ACE0C. RemapModels
 * calls PreInitExplosion once a level, with a NULL header when the CastList
 * has no `Explosion` — and then the store is skipped, but so is every spawn,
 * so a stale handle is never played.
 *
 * SoundByCreature places the voice AT THE ENTITY. It copies ent+0x54..+0x5C
 * into the voice (0x8007287C..0x80072894), which the spawn has just set to
 * `at` (0x8005A8D8), and keeps the entity pointer so ProcessSounds re-ranges
 * it from ent+0xA4 every frame — also `at`, and an explosion never moves. So
 * the bang is positional at the point the spawn was handed, the same point
 * PSXEXPLODE's report uses. (In split screen, 0x800AEBCC, SoundByCreature
 * plays every voice flat instead; this port positions all of its creature
 * sounds and this one is no different.)
 *
 * IT IS NOT CUT SHORT WITH THE FIREBALL. The voice's tag goes to ent+0x114/
 * +0x116, and the think's removal, KillModel (0x8006D280), hands that tag to
 * SoundStopOnCreDeath (0x80073800) — which only marks the voice state 8. The
 * state-8 arm of ProcessSounds (0x800731F0) keys a voice off early only when
 * its request carries flag 0x4, and FlagIfLoopingSample (0x80073FF4) is the
 * only writer of that bit: it is "this sample loops". `wep_grenlx1a` is a
 * one-shot of 1.34 s on every bank, so the bang plays out long after the
 * 400-unit fireball has gone.
 *
 * Queued rather than played for the reason sim.h's `blast_at` is: the game
 * layer has no mixer. The owner drains the queue between ticks and plays
 * Q2_MODEL_ENT_SOUND at each position.
 *
 * Twenty-four, because ProcessSounds walks twenty-four voices (`slti v0, s2,
 * 24` at 0x800734A4) and GetFreeChannel (0x800740DC) refuses a twenty-fifth:
 * more bangs than that in one tick could not have sounded at once on the
 * console either. A twenty-fifth is dropped and counted.
 */
#define Q2_MODEL_ENT_SOUND      "wep_grenlx1a"   /* 0x800ACE0C, twelve bytes */
#define Q2_MODEL_ENT_SOUNDS_MAX 24

typedef struct q2_model_ent_sounds {
    s32 at[Q2_MODEL_ENT_SOUNDS_MAX][3];
    u32 count;
    u32 dropped;
} q2_model_ent_sounds;

void q2_model_ent_sounds_clear(q2_model_ent_sounds *q);
void q2_model_ent_sound_push(q2_model_ent_sounds *q, const s32 at[3]);

/* Take the oldest queued position, or false when there is none. In the order
 * the spawns happened, which is the order their voices were allocated. */
bool q2_model_ent_sound_next(q2_model_ent_sounds *q, s32 out[3]);

q2_entity *q2_model_ent_spawn(q2_entity_set *set, const q2_model_bank *bank,
                              q2_model_ent_kind kind, const s32 at[3],
                              u8 surface, s32 size,
                              q2_model_ent_sounds *sounds);

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

/* ========================================================================= */
/* GIBS — the second family of transient model entity                        */
/* ========================================================================= */
/*
 * When a body comes apart the engine does not raise a particle burst and call
 * it done. It throws MODELS: `Gib meat` chunks and a `Chest`, each one an
 * entity out of the same 48-slot pool the explosion comes from, each with its
 * own think, its own blood trail and its own lifetime. The port had none of
 * this — a gibbed creature set a bool, a gibbed player changed a stage, and
 * nothing was thrown.
 *
 * ---------------------------------------------------------------------------
 * Who gets here — 0x8007CEB4, the destruction dispatcher
 * ---------------------------------------------------------------------------
 * Three callers, and they are the whole list (`xrefs 0x8007CEB4`):
 *
 *     0x80039578  player corpse_think  — `health <= gib_health`, 0x8003956C
 *     0x8003E2A4  player respawn_think — the same test, 0x8003E270
 *     0x8007F764  the creature CORPSE handler 0x8007F71C, 0x8007F748..0x8007F758
 *
 * All three pass a1 = the address of eight zero bytes (0x800AE86C, 0x800AED04)
 * and the dispatcher never reads a1 before overwriting it — a dead argument.
 *
 * A creature killed straight past its gib threshold goes through the SAME
 * corpse handler: its module's gib arm throws nothing (cre_soldier.c says so
 * at length), it only sets SVF_DEADMONSTER, the body detaches, and the first
 * corpse tick sees `health <= gib_health` and lands here. Every gib on the
 * disc comes out of this function.
 *
 * The class is entity+0xD2, or +0xDA when +0xD2 is 47 (0x8007CEC8..0x8007CEE0:
 * a corpse keeps what it used to be there). `(s16)(class - 2)` indexes the
 * 36-word jump table at 0x800AD648 behind an UNSIGNED `sltiu 36`, so class 0,
 * class 1 and anything past 37 all take the default. Read word by word:
 *
 *     class  2 Boss2, 8 Flyer, 11 Hover  0x8007CF14  Explosion model, then
 *                                                    Bos2Gib1 x16 (7, 1, 8)
 *     class 13 Jorg                      0x8007D054  JorgGib1 x16 (7, 1, 9)
 *     class 14 Rider, 37 RiderStand      0x8007D140  nothing but the free
 *     class 17 Parasite                  0x8007D0BC  ThrowGibs(5, no head)
 *     class 24 Boss1                     0x8007CF84  Bos1Gib1 x16 (7, 1, 8)
 *     class 32 Strider                   0x8007CFE0  Explosion model, then
 *                                                    StridGib1 x16 (8, 1, 7)
 *     everything else                    0x8007D0F8  ThrowGibs(5, "Chest")
 *
 * The Strider is table word 30 (0x800AD6C0) and RiderStand word 35
 * (0x800AD6D4); an earlier reading put them at words 28 and 34, i.e. on
 * classes 30 and 36, which are a Chest arm and a Chest arm. The class names
 * are `q2psx-inspect classes`. A PLAYER is class 39 — 0x8003B2B0 `addiu v0,
 * zero, 39` / `sh v0, 210(s1)` in the player spawn — and so takes the default.
 *
 * The impulse every arm hands down is built at 0x8007CF30..0x8007CF68 as
 * `(s16)(+0xE0 + +0x2F8)` per axis — the body's velocity plus its knockback,
 * both read as HALFWORDS with `lhu` — and every arm ends at 0x8007D140 with
 * `0x8006D280(self)`: the body is freed. The explosion arms call 0x8005A778
 * with a radius of 2048 (0x8007CF18); `q2_model_ent_spawn` carries no radius,
 * which is that function's existing omission and not a new one.
 *
 * MAP DATA DECIDES MOST OF THIS. The model list `q2psx-inspect models` prints
 * for each of the 49 maps: 31 carry `Gib meat`, 30 carry `Chest`, one (BOSS2)
 * carries JorgGib1..JorgGib9, and NONE carries a Bos1Gib, Bos2Gib or StridGib
 * model. Three of the four named arms can only ever throw invisible entities
 * on this disc; the Explosion two of them raise first is on 34 maps.
 *
 * ---------------------------------------------------------------------------
 * ThrowGibs — 0x8005A3D4(entity, count, impulse, head_name)
 * ---------------------------------------------------------------------------
 *   1. `rand()` is the starting yaw (0x8005A404), THEN `4096 / count` is the
 *      step (0x8005A414, with the console's divide-by-zero break at
 *      0x8005A420 — no caller passes zero).
 *   2. 0x8005A440 `jal 0x8005B320(entity)`: the mesh blood spray on ramps 2
 *      and 3, `q2_fx_gib_spray`. Forty-five draws even with no mesh.
 *   3. Per chunk: three draws of `286 * (rand() - 16384)`, rounded TOWARD
 *      ZERO by 16384 (`bgez; addiu 16383; sra 14`, 0x8005A470..0x8005A480),
 *      added to entity+0x54 in x, y, z order — a jitter of -286..285. Then
 *      0x8004E920 clips that point into PRIMARY collision from the parent's
 *      own position and returns the cell it ended in (sp+56).
 *   4. 0x8005A0AC(entity, yaw, impulse, point, cell, 512, "Gib meat"), and
 *      the yaw steps AFTER a1 was loaded (0x8005A568 is the delay slot).
 *   5. If a head name was given, ONE MORE with lifetime 2560, thrown from
 *      entity+0xA4 — the DRAW ORIGIN, unjittered (`addiu a3, a0, 164` at
 *      0x8005A5CC) — at the yaw the loop ended on and the LAST chunk's cell.
 *
 * ---------------------------------------------------------------------------
 * One chunk — 0x8005A0AC, and its boss twin 0x8005AD8C
 * ---------------------------------------------------------------------------
 *   `0x8006C098(0)`: NO eviction, so a full pool drops the chunk — and drops
 *   its six draws with it, since the return is before the first rand().
 *   +0x3C = 0x80059DE0. The twelve name bytes go to 0x8006D008 and the result
 *   to +0x10 with NO test: a name the CastList lacks still makes an entity,
 *   unlike 0x8005A778 which bails at 0x8005A894. +0x10C = 0x01800000
 *   (`lui v0, 0x180`) — the transient bit AND the quick-sort bit. The point
 *   goes to +0xA4 and +0x54. The parent's +0x2AC ambient is copied on top of
 *   a 0x800AEAC8 (40 40 40) write that is therefore dead (0x8005A1E0 then
 *   0x8005A338..0x8005A35C).
 *
 *   Then the throw, in this draw order:
 *
 *                      0x8005A0AC (meat)          0x8005AD8C (boss)
 *     speed  r1        256 + ((3*r1 << 8) >> 15)  768 + (r1 >> 6)
 *     +0xE0            imp.x + trunc(cos*speed/4096)   trunc(cos*speed/4096)
 *     +0xE2  r2        imp.y - 3072 + trunc(-1536*r2/32768)
 *                                                 trunc(-2000*r2/32768) - 3000
 *     +0xE4            imp.z + trunc(sin*speed/4096)   trunc(sin*speed/4096)
 *     +0xE6.. r3 r4 r5 three raw draws, the spin angles
 *     +0x44  r6        (r6 - 16384) >> 6
 *     +0xF4  (r7)      lifetime                    lifetime + (r7 >> 5)
 *     +0xD2            untouched                   48
 *
 *   `cos` and `sin` are `lh 2` and `lh 0` of the 0x800A5430 pair at
 *   `yaw & 0xFFF`. The boss twin clobbers a2 at 0x8005ADF4 without saving
 *   it: the impulse NEVER reaches a boss chunk.
 *
 *   Tail: +0xA2 = -1, +0xA0 = the cell, +0x9E = that cell's byte +32, and
 *   0x8005555C builds a +/-20 box from the three halfwords at 0x800AEACC.
 *
 * ---------------------------------------------------------------------------
 * The boss ring — 0x8005B0AC(entity, count, impulse, name, digit, low, high)
 * ---------------------------------------------------------------------------
 * ThrowGibs without the spray and without the head: the same start yaw, the
 * same step, the same jitter and clip, and before each chunk
 * `name[digit] = '1' + i` where i starts at `low` and wraps back to `low` once
 * it passes `high` (0x8005B1F0 `addiu v1, s0, 49`, bytes 31 00 03 26). So the
 * FIRST name written is '1' + 1 = '2' — the template's own `...1` is never
 * thrown — and for Jorg (low 1, high 9) one piece in nine is `JorgGib:`, a
 * name no CastList carries. The console writes into the .data template
 * itself; every burst rewrites the digit before its first copy, so that
 * persistence is unobservable and a local copy stands in for it.
 *
 * ---------------------------------------------------------------------------
 * The think — 0x80059DE0
 * ---------------------------------------------------------------------------
 *     0x80046B98(ent, 0, 0, 0x0900)       the toss (below)
 *     if (dt >= +0xF4) { +0x3C = 0x8005B358; +0xF4 = dt + 1; }
 *     +0xF4 -= dt
 *     +0xA4 = +0x54
 *     if (!(+0x98 & 0x20)) blood trail    q2_fx_gib_trail, 45 draws
 *     0x80054DD4(+0x54, 2)                +0x9E from the +0xA0 cell
 *
 * 0x8005B358 is the ITEM SHRINK think — `+0xFC -= 16 * dt`, free at zero —
 * which this port already has as `q2_item_shrink_think`, so an expired gib
 * darkens out exactly the way a collected item does. It no longer moves.
 *
 * THE TOSS, 0x80046B98 and the 0x800463E8 mover it calls:
 *   - `+0x98 & 0x60` (at rest, or bit 0x40) skips everything but the matrix.
 *   - Tumble: THREE draws, `+0xE6.. -= 160 + 2 * (rand() & 31)` when +0xE6 is
 *     even, `+=` when it is odd (0x80046BE0..0x80046CA0).
 *   - Gravity: `+0xE2 += [0x800AE924] * dt`, as a halfword, clamped at 8192
 *     (0x80046464..0x800464A0) unless +0x10C carries 0x2000.
 *   - The move is `vel * dt / 320` (0x66666667 and `sra 7`) through PRIMARY
 *     collision from the +0xA0 cell. On a block the new velocity is the
 *     REFLECTED remaining displacement, normalised (0x8008A588) and scaled to
 *     `|v| * 2304 >> 12` (0x800466F4..0x80046A5C) — 2304 is the 0x0900 word
 *     at 0x800AEAC4, the fourth argument. Contact or not, +0xA0 and +0x9E
 *     then take the cell the move ended in (0x80046ACC..0x80046B0C), so the
 *     trail later in the same think is laid in the new area.
 *   - Then 0x80046CF8: no block clears the rest bit; a block with
 *     `|v|^2 > 0xC34FF` clears it and bumps +0xE6 by one (flipping the tumble);
 *     a slower block SETS it. A gib at rest stops, stops tumbling and stops
 *     bleeding.
 *
 * ---------------------------------------------------------------------------
 * What the port carries differently, said once
 * ---------------------------------------------------------------------------
 * - The pool, the CastList, the particle pool, the collision hull and rand()
 *   are all GLOBALS on the console, which is how the dispatcher reaches them
 *   with nothing but an entity pointer. The port's are owned by the sim, so
 *   the owner registers them once with `q2_gib_attach`, and the two death
 *   sites — which have nothing but the dying record — reach them through it.
 * - `q2_entity` has no +0xE0 velocity, no +0x98 movement word, no +0xA0 cell
 *   and no +0x44 of the right meaning for a gib, so those live in a
 *   `q2_gib_body` beside the entity, keyed by set and slot the way item.c
 *   keeps a death drop's flight.
 * - There are Q2_GIB_BODY_MAX of those, the console's whole pool. No console
 *   frame can hold more live gibs than that (it holds fewer: the pool is
 *   shared), and a throw that finds them all held refuses the chunk before
 *   its first draw, the way `0x8006C098(0)` does when the pool is full.
 * - The mover's two ground-normal slots (+0x60, +0x66) are not modelled; the
 *   rest test above overrides the one bit they feed. Its trigger arm
 *   (0x800465F0..0x8004664C) is gated on the high half of the fourth
 *   argument, which 0x80046B98 passes as zero, so a gib never runs it. The
 *   entity sweep (0x80053974) IS modelled, the way item.c's death drop has
 *   it, when the owner hands over `ents`.
 * - The draw consumes only the yaw of +0xE6 (entitydraw.c); the tumble is
 *   kept on all three axes so it is right when the draw is.
 */
/* effect.h, monster.h and playerdeath.h stay out of this header: two tools
 * files include it and need none of them. q2_rng and q2_collision arrive with
 * entity.h already. */
struct q2_fx_world;
struct q2_fx_mesh_src;
struct q2_move_world;
struct q2_monster;
struct q2_player_death;

#define Q2_GIB_NAME_LEN     12      /* three words, 0x8005A534..0x8005A560   */
#define Q2_GIB_MEAT_NAME    "Gib meat"   /* 0x800ACDE8                        */
#define Q2_GIB_CHEST_NAME   "Chest"      /* 0x800AD5FC                        */

#define Q2_GIB_COUNT        5       /* 0x8007D0FC and 0x8007D0C8             */
#define Q2_GIB_RING_COUNT   16      /* 0x8007CF2C and its three siblings     */
#define Q2_GIB_YAW_CIRCLE   4096    /* 0x8005A410, the dividend of the step  */
#define Q2_GIB_JITTER       286     /* 0x8005A45C..0x8005A46C                */

#define Q2_GIB_LIFE_MEAT    512     /* 0x8005A520                            */
#define Q2_GIB_LIFE_HEAD    2560    /* 0x8005A58C                            */
#define Q2_GIB_LIFE_RING    512     /* 0x8005B220, before its r >> 5 jitter  */

/* 0x8005A1A4 `lui v0, 0x180`: Q2_RF_TRANSIENT | Q2_RF_QUICK_SORT. */
#define Q2_GIB_RENDER_FLAGS 0x01800000u
#define Q2_GIB_HALF_EXTENT  20      /* 0x800AEACC..0x800AEAD0 via 0x8005555C */
#define Q2_GIB_BOSS_CLASS   48      /* 0x8005ADD8, the boss chunk's +0xD2    */

#define Q2_GIB_CORPSE_CLASS 47      /* 0x8007CEC0; monster.h Q2_CLASS_CORPSE */
#define Q2_GIB_PLAYER_CLASS 39      /* 0x8003B2B0, the player's +0xD2        */
#define Q2_GIB_TABLE_SIZE   36      /* 0x8007CEEC `sltiu v0, v1, 36`         */

/* The toss, 0x80046B98. The mover it calls, 0x800463E8, is the one a death
 * drop flies with, and its step, terminal speed and no-gravity bit are
 * item.h's Q2_ITEM_TOSS_* — one copy of each figure. What is the gib's own: */
#define Q2_GIB_TUMBLE_BASE  160     /* 0x80046C08 / 0x80046C60               */
#define Q2_GIB_TUMBLE_MASK  31      /* 0x80046BFC, then doubled              */
#define Q2_GIB_RESTITUTION  2304    /* 0x800AEAC4, 0x80046B98's a3           */
#define Q2_GIB_REST_SPEED_SQ 0xC34FF /* 0x80046CFC / 0x80046D2C             */

/* +0x98 bit 0x20: at rest. Bit 0x40 is the other half of 0x80046BC0's mask;
 * no gib writer sets it, and it is named so the test reads as the mask does. */
#define Q2_GIB_MOVE_REST    0x20u
#define Q2_GIB_MOVE_HELD    0x40u

typedef enum q2_gib_style {
    Q2_GIB_STYLE_MEAT = 0,        /* 0x8005A0AC */
    Q2_GIB_STYLE_BOSS,            /* 0x8005AD8C */
    Q2_GIB_STYLE_COUNT
} q2_gib_style;

/* The arms of 0x8007CEB4, named by what they throw. */
typedef enum q2_gib_arm {
    Q2_GIB_ARM_CHEST = 0,         /* 0x8007D0F8, the default               */
    Q2_GIB_ARM_MEAT,              /* 0x8007D0BC, class 17                  */
    Q2_GIB_ARM_BOS2,              /* 0x8007CF14, classes 2, 8, 11          */
    Q2_GIB_ARM_BOS1,              /* 0x8007CF84, class 24                  */
    Q2_GIB_ARM_STRID,             /* 0x8007CFE0, class 32                  */
    Q2_GIB_ARM_JORG,              /* 0x8007D054, class 13                  */
    Q2_GIB_ARM_NONE,              /* 0x8007D140, classes 14 and 37         */
    Q2_GIB_ARM_COUNT
} q2_gib_arm;

/*
 * Everything the dispatcher and the throwers read off the body being taken
 * apart, by value. The console hands them the entity; the port's dying
 * records are a `q2_monster` and a `q2_player_death`, neither of which is one,
 * so the site fills this in.
 */
typedef struct q2_gib_parent {
    bool placed;            /* pos/origin are known — a throw needs them      */
    s32  pos[3];            /* +0x54: the jitter centre and the clip's start  */
    s32  origin[3];         /* +0xA4: where the head is thrown from           */
    s16  velocity[3];       /* +0xE0..+0xE4                                   */
    s16  knockback[3];      /* +0x2F8..+0x2FC, read as HALFWORDS (0x8007CF34) */
    u16  cls;               /* +0xD2                                          */
    u16  was_class;         /* +0xDA, read when +0xD2 is 47                   */
    u8   area;              /* +0x9E, the byte 0x8005B320's groups carry      */
    u8   glow[3];           /* +0x2AC, copied onto every chunk (0x8005A344)   */
    const struct q2_fx_mesh_src *mesh;  /* 0x8005B320's posed mesh, or NULL  */
} q2_gib_parent;

/* A parent with the allocator's values: 0x40 ambient, no mesh, not placed. */
void q2_gib_parent_init(q2_gib_parent *p);

typedef enum q2_gib_victim {
    Q2_GIB_VICTIM_MONSTER = 0,    /* `who` is a const q2_monster *          */
    Q2_GIB_VICTIM_PLAYER          /* `who` is a const q2_player_death *     */
} q2_gib_victim;

/*
 * The owner's chance to say what it knows about a dying record that the
 * record itself does not carry — where a player's body is, a creature's
 * knockback and area byte, a posed mesh for the spray. Called with the
 * parent already filled from the record; set `placed` to throw.
 */
typedef void (*q2_gib_describe_fn)(void *user, q2_gib_victim kind,
                                   const void *who, q2_gib_parent *parent);

/*
 * What a throw needs that the console keeps in globals. `set`, `rng` are
 * required; the rest may be NULL/0 and each says what goes missing:
 *
 *   bank     NULL: no chunk binds a model (they still exist and bleed)
 *   fx       NULL: no spray and no trail (their draws are still made)
 *   coll     NULL: no clip at spawn and no floor under a falling gib
 *   ents     NULL: nothing but the hull stops a gib — no door, no lift
 *            (0x80053974, the entity boxes the mover tries first)
 *   gravity  NULL: Q2_GRAVITY. It is [0x800AE924], per dt, and it is READ
 *            LIVE — the sim's own word, so the GAME VARIABLES menu reaches a
 *            gib in flight the way item.h's q2_item_env lets it reach a drop
 *   sounds   NULL: the boss arms' Explosion is silent — it is still spawned,
 *            but its Sound_Explosion (0x8005AA54) has nowhere to go
 */
typedef struct q2_gib_world {
    q2_entity_set               *set;       /* 0x800CBA28, the pool       */
    const q2_model_bank         *bank;      /* 0x8006D008's model list    */
    struct q2_fx_world          *fx;        /* 0x800B2860, the groups     */
    q2_rng                      *rng;       /* rand(), 0x80089E28         */
    q2_collision                *coll;      /* 0x800C8E90, PRIMARY        */
    const struct q2_move_world  *ents;      /* what 0x80053974 clips      */
    const s32                   *gravity;   /* [0x800AE924]               */
    q2_model_ent_sounds         *sounds;    /* the spawn's bang, queued   */
    q2_gib_describe_fn           describe;
    void                        *user;
} q2_gib_world;

/*
 * Register the world the two death sites throw into. The struct is copied;
 * NULL detaches and forgets every gib body (do so before the set it names is
 * freed). Re-attaching a different set forgets them too.
 */
void q2_gib_attach(const q2_gib_world *w);
const q2_gib_world *q2_gib_attached(void);

/*
 * The half of a live gib that `q2_entity` has no fields for, kept per (set,
 * slot) and valid while that slot's think is q2_gib_think. Forty-eight is
 * 0x800CBA28's record count — see the note above.
 */
#define Q2_GIB_BODY_MAX 48

typedef struct q2_gib_body {
    s16 vel[3];             /* +0xE0..+0xE4 */
    u32 move_flags;         /* +0x98: Q2_GIB_MOVE_REST */
    s32 spin;               /* +0x44, `(rand() - 16384) >> 6`; no reader decoded */
    s32 cell;               /* +0xA0: its PRIMARY collision cell, -1 unknown */
} q2_gib_body;

/* The body of an entity in the attached set, or NULL. */
q2_gib_body *q2_gib_body_of(const q2_entity *e);
bool q2_gib_body_restore(const q2_entity_set *set, u32 slot, const q2_gib_body *body);

/* The arm 0x8007CEB4 takes for a body of this class. */
q2_gib_arm q2_gib_arm_for_class(u16 cls, u16 was_class);

/*
 * The throw arithmetic of both chunk spawners, for a yaw and the two draws
 * `r_speed` and `r_toss` (see the table above). `impulse` is ignored by the
 * boss style, which is the point of taking it.
 */
void q2_gib_throw_velocity(q2_gib_style style, s32 yaw, s32 r_speed,
                           s32 r_toss, const s16 impulse[3], s16 out[3]);

/*
 * One chunk — 0x8005A0AC or 0x8005AD8C. `cell` is the PRIMARY cell the clip
 * produced (or -1); `name` is up to twelve bytes. Returns the entity, valid
 * until the set's next allocation, or NULL when the pool refused.
 */
q2_entity *q2_gib_spawn_one(const q2_gib_world *w, const q2_gib_parent *parent,
                            q2_gib_style style, s32 yaw, const s16 impulse[3],
                            const s32 pos[3], s32 cell, s32 lifetime,
                            const char *name);

/* ThrowGibs, 0x8005A3D4. Returns how many entities it made. */
u32 q2_gib_throw_meat(const q2_gib_world *w, const q2_gib_parent *parent,
                      s32 count, const s16 impulse[3], const char *head_name);

/* The boss ring, 0x8005B0AC. Returns how many entities it made. */
u32 q2_gib_throw_named(const q2_gib_world *w, const q2_gib_parent *parent,
                       s32 count, const s16 impulse[3],
                       const char *name_template, s32 digit, s32 low,
                       s32 high);

typedef struct q2_gib_report {
    q2_gib_arm arm;
    u32  chunks;            /* entities from 0x8005A0AC / 0x8005AD8C */
    bool explosion;         /* 0x8005A778 produced its model         */
} q2_gib_report;

/* 0x8007CEB4 minus its final free, which is the caller's (the record is). */
u32 q2_gib_destroy(const q2_gib_world *w, const q2_gib_parent *parent,
                   q2_gib_report *out);

/* The think installed at +0x3C, 0x80059DE0. */
void q2_gib_think(q2_entity *e, q2_entity_world *w);

/*
 * What the family has done since the counters were last zeroed — the port's,
 * for a caller's run report, the way sim.h keeps `explosive_models`. Nothing
 * reads them back.
 */
typedef struct q2_gib_stats {
    u32 destroyed;      /* bodies handed to 0x8007CEB4                     */
    u32 chunks;         /* entities 0x8005A0AC and 0x8005AD8C made          */
    u32 refused;        /* chunks the full body table turned away           */
    u32 unbound;        /* chunks whose name the attached bank lacks        */
    u32 trails;         /* trail groups the think raised                    */
    u32 landed;         /* gibs that came to rest (+0x98 bit 0x20 set)      */
} q2_gib_stats;

extern q2_gib_stats q2_gib_counters;

/*
 * The two death sites, through the attached world. Each fills a parent from
 * the record it is handed, lets the owner's `describe` add what it knows, and
 * dispatches. Both return how many chunks were thrown; with nothing attached,
 * or a player the owner could not place, that is zero and nothing is drawn.
 *
 *   q2_gib_monster_corpse   0x8007F764, monster.c's corpse tick
 *   q2_gib_player_body      0x80039578 / 0x8003E2A4, playerdeath.c
 */
u32 q2_gib_monster_corpse(const struct q2_monster *m);
u32 q2_gib_player_body(const struct q2_player_death *d);

#endif /* Q2PSX_MODELENT_H */
