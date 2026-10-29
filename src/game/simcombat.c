/*
 * simcombat.c — where the reconstructed combat meets the reconstructed world.
 *
 * Everything here is glue, and deliberately so: weapon.c holds what the fire
 * functions do, combat.c holds what the damage function does, projectile.c
 * holds what a rocket is, and this file is the only place that knows all three
 * plus the collision hull. Keeping it separate from sim.c also keeps the
 * movement code readable, which is the harder of the two to follow.
 *
 * Shots use q2_sim_trace: the PrimaryColl hull followed by the runtime entity
 * boxes. Movement alone uses SecondaryCol's baked-in player hull. See sim.c
 * for the original call sites and the distinction between these contexts.
 */
#include <stdlib.h>
#include <string.h>

#include "sim.h"
#include "ai.h"
#include "random.h"
#include "explosive.h"
#include "lighting.h"     /* q2_light_glow_fade — 0x80075E14 */
#include "modeldraw.h"    /* q2_model_world_vertex, 0x8006CC44: the blur */
#include "modelent.h"
#include "trig.h"

/* ------------------------------------------------------------------------- */
/* Effects                                                                    */
/* ------------------------------------------------------------------------- */
void q2_sim_attach_effects(q2_sim *sim, const q2_fx_tables *tab, u32 seed)
{
    if (!sim)
        return;

    q2_fx_flash_clear(&sim->fx);
    q2_fx_world_init(&sim->fx, tab);
    q2_rng_seed(&sim->fx_rng, seed);
    sim->fx_ready = (tab != NULL && tab->loaded);
}

bool q2_sim_attach_glint(q2_sim *sim, const q2_common_file *common)
{
    const dat_chunk *chunk;

    if (!sim)
        return false;

    memset(&sim->glint, 0, sizeof(sim->glint));

    if (!common)
        return false;

    chunk = common->chunk[Q2_COMMON_GLINT_MOD];
    if (!chunk || !chunk->data)
        return false;

    sim->glint.ready = q2_fx_glint_mesh_decode(&sim->glint.mesh,
                                               chunk->data, chunk->size);
    if (!sim->glint.ready)
        return false;

    /*
     * Decode the known generator script's flag, band count, phase and tables.
     * The frame driver below reconstructs BIGGUN's behavior; the scanner does
     * not execute arbitrary LevelBin code. An unrecognized table stays off.
     */
    {
        const dat_chunk *lb = common->chunk[Q2_COMMON_LEVEL_BIN];
        q2_fx_glint_script script;
        u32 i;

        if (!lb || !lb->data ||
            !q2_fx_glint_scan(&script, lb->data, lb->size)) {
            /* The mesh is loaded and drawable, but nothing turns it on. */
            return true;
        }

        sim->glint.scripted   = script.bands_found;
        sim->glint.lightning_ready = script.lightning_found;
        memcpy(sim->glint.lightning_target, script.target, sizeof(script.target));
        memcpy(sim->glint.lightning_uv, script.uv, sizeof(script.uv));
        memcpy(sim->glint.spark_origin, script.spark_origin, sizeof(script.spark_origin));
        sim->glint.power_left = 3600;
        sim->glint.evacuation_left = 300;
        sim->glint.band_count = script.band_count ? script.band_count
                                                  : Q2_FX_GLINT_BANDS;
        sim->glint.phase      = script.phase ? script.phase
                                             : Q2_FX_GLINT_PHASE_START;

        q2_rng_seed(&sim->glint.script_rng, 0); /* module+0x43B0 */
        for (i = 0; i < sim->glint.band_count &&
                    i < Q2_FX_GLINT_BANDS_MAX; i++) {
            memcpy(sim->glint.base_angle[i], script.angle[i],
                   sizeof(script.angle[i]));
        }
    }

    return true;
}

void q2_sim_glint_tick(q2_sim *sim)
{
    q2_fx_glint *g;
    u32 i;
    if (!sim)
        return;
    g = &sim->glint;
    g->raised = false;
    g->owner_active = false;
    if (!g->ready || !g->scripted || !g->zone_active)
        return;

    /* BIGGUN +0x1870..+0x18C0: the first live item 20, Blackhole P.
     * Never substitute the camera when the owner is absent or collected. */
    for (i = 0; i < sim->entities.count; i++) {
        q2_entity *e = &sim->entities.ent[i];
        if (!e->in_use || e->kind != Q2_FX_GLINT_TARGET_KIND ||
            e->place_id != Q2_FX_GLINT_TARGET_ID)
            continue;
        if (g->finished) {
            e->in_use = false; /* a streamed zone can recreate its Population */
            return;
        }
        if (e->hidden) continue;
        g->owner_active = true;
        if (g->shutting_down) {
            g->power_left -= sim->ent_world.dt;
            if (g->power_left < 0) g->power_left = 0;
        }
        if (g->dying) {
            s32 fade;
            g->death_time += sim->ent_world.dt;
            fade = (1390 - g->death_time) * 4096 / 200;
            if (fade <= 0) {
                e->in_use = false;
                g->owner_active = false;
                g->finished = true;
                return;
            }
            if (fade <= 4096) {
                e->scale = (s16)fade;
                if (!g->burst_done) {
                    g->burst_done = true;
                    g->burst_pending = true;
                }
            }
            if (g->crack_time > 0) {
                g->crack_time -= sim->ent_world.dt;
                if (g->crack_time < 0) g->crack_time = 0;
            }
        }
        /* +0x1A74..+0x1AFC uses two sine-table byte offsets, then a signed
         * divide toward zero. Preserve the unsigned 32-bit clock shift. */
        {
            u32 t = (u32)sim->level_time << 12;
            u32 a = (u32)(((u64)t * 0x6FCFF4A5u) >> 43);
            u32 b = (u32)(((u64)t * 0x10624DD3u) >> 36);
            s32 wave = (q2_sin12((s32)((a & 0x3FFCu) >> 2)) + 8192) * 2;
            wave *= q2_sin12((s32)((b & 0x3FFCu) >> 2));
            e->origin[1] = 1500 + wave / 409600;
        }
        if (sim->ent_world.dt > 0)
            g->script_time += (u32)sim->ent_world.dt;
        /* +0x1578..+0x17E8: the owner alternates a random turn and a pause.
         * Its two private RNG draws precede the six draws for each band. */
        g->turn_left = (s16)(g->turn_left - sim->ent_world.dt);
        if (g->turn_left <= 0) {
            if (g->power_left == 0 && g->shutting_down) {
                g->dying = true;
                g->death_time = 0;
                g->turn_left = 30000;
                g->crack_time = 1600;
            } else {
                s32 power = g->power_left * 4096 / 3600;
                s32 speed = q2_rng_next(&g->script_rng) * 96 / 32768;
                s32 duration;
                if (speed < 48)
                    speed = -speed - 48;
                speed = speed * 3072 / (power / 2 + 1024);
                duration = (q2_rng_next(&g->script_rng) * 300 / 32768 + 140)
                         * (power + 4096) / 8192 + 10;
                if (g->turn_speed) {
                    g->turn_speed = 0;
                    duration /= 2;
                } else {
                    g->turn_speed = (s16)speed;
                }
                g->turn_left = (s16)duration;
            }
        }
        e->angles[1] = (s16)(e->angles[1] + g->turn_speed);
        g->raised = !g->burst_done;
        memcpy(g->origin, e->origin, sizeof(g->origin));
        memcpy(g->angles, e->angles, sizeof(g->angles));
        g->area = q2_sim_fx_area(sim, e->surface, e->origin);
        if (g->raised) {
            q2_fx_glint_refresh(g, e->angles);
        }
        if (!g->burst_done && g->sparks_visible && sim->fx_ready) {
            s16 vel[15][3];
            const q2_fx_ramp *ramp = q2_fx_ramp_by_id(sim->fx.tab, 0);
            u32 p;
            for (p = 0; p < 15; p++) {
                vel[p][0] = (s16)((q2_rng_next(&sim->fx_rng) - 16384) >> 12);
                vel[p][1] = (s16)((q2_rng_next(&sim->fx_rng) - 16384) >> 12);
                vel[p][2] = (s16)(-q2_rng_next(&sim->fx_rng) >> 8);
            }
            (void)q2_fx_group_spawn(&sim->fx, g->spark_origin,
                                     (const s16 (*)[3])vel, 15,
                                     ramp, ramp, 23, 20000, g->area);
        }
        break;
    }
}

/*
 * WHICH AREA A BURST BELONGS TO — 0x800686C4, transcribed.
 *
 *     800686D0  bne  a0, zero, 0x80068720   ; a non-zero byte is taken as is
 *     800686E8  jal  0x80044F54             ; else find the cell for the point
 *     80068710  lbu  v0, 32(v0)             ;   in PrimaryColl (0x800C8E90)
 *
 * The third group spawner (0x8002FDFC) runs this on its own area argument at
 * 0x8002FED0, which is why the console's own call sites can pass a literal 0
 * and still land in a live area chain. The other two spawners rely on their
 * CALLERS supplying a resolved byte — the hitscan's is the trace's own area
 * record, `lbu s1, 32(v1)` at 0x80048854.
 *
 * The port stores whatever byte it is given (effect.c) and the group draw culls
 * a group whose area has no screen-change record to drain (effect.c, the
 * `psx_ot_area_bucket` test). Since no collision cell on the disc carries area
 * 0 — `q2psx-inspect coll` reports contents != 0 on 22767 of 22773 nodes —
 * every burst this file raised with a literal 0 was culled in every zone that
 * ships a SortData stream, which is most of them. So the resolve arm lives
 * here, at the call sites, rather than inside effect.c: that module has no
 * collision dependency and the console only runs the arm in one of its three
 * spawners.
 *
 * PrimaryColl, as 0x800686D8 loads, and not the movement hull.
 */
static u8 fx_area_resolve(q2_sim *sim, u8 area, const s32 at[3])
{
    q2_collision *hull;
    q2_coll_node  cell;
    s32           node;

    if (area)                       /* 0x800686D0 */
        return area;
    if (!sim || !at)
        return 0;

    hull = sim->coll_primary_ready ? &sim->coll_primary
         : (sim->coll_ready ? &sim->coll : NULL);
    if (!hull)
        return 0;

    node = q2_coll_find_node(hull, at, -1, true);
    if (node < 0 || !q2_collision_get_node(hull, (u32)node, &cell))
        /* 0x800686F8 branches to 0x8006871C, which returns zero: a point in no
         * cell has no area record, and the caller is left with the byte the
         * draw culls. There is no better answer to invent. */
        return 0;
    /* The console returns the raw byte (0x80068710); the mask is the port's,
     * applied here rather than at every reader, and matches what world.c and
     * the entity draws register. */
    return (u8)(cell.contents & 0x7F);
}

/* A spawn that costs nothing when no tables are attached. `area` is the byte
 * the console's caller would have had in hand; 0 asks for the 0x800686C4
 * lookup above. */
static void fx_at_area(q2_sim *sim, q2_fx_preset_id id, const s32 at[3],
                       u8 area)
{
    if (!sim->fx_ready || !at)
        return;
    q2_fx_spawn(&sim->fx, &sim->fx_rng, id, at,
                fx_area_resolve(sim, area, at));
}

static void fx_at(q2_sim *sim, q2_fx_preset_id id, const s32 at[3])
{
    fx_at_area(sim, id, at, 0);
}

/*
 * The same resolve, reachable from the other half of the sim.
 *
 * sim.c is a separate translation unit and the player's own per-frame state
 * think lives there — the water splash needs `entity+0x9E` the same way every
 * spawner in this file does, and 0x800686C4 is one function on the console.
 */
u8 q2_sim_fx_area(q2_sim *sim, u8 area, const s32 at[3])
{
    return fx_area_resolve(sim, area, at);
}

/*
 * Where a hitscan shot leaves its mark.
 *
 * The port DOES have a contact point: `world_fraction_for` already runs the
 * pellet through the hull and hands back the 1.0.12 fraction at which the world
 * stopped it, and the direction carries the range, so `origin + dir * frac` is
 * the impact. The original's own hitscan (0x8004874C) does the same thing — it
 * forms the trace end as origin + dir, takes the clipped point back, and either
 * sprays blood there (0x80048980 -> 0x80048B64) or throws a spark (0x800486EC).
 *
 * A pellet that hit nothing and was stopped by nothing marks nothing: a frac of
 * 4096 with no victim means the shot ran out of range in open air.
 *
 * CORRECTION — the world arm is NOT a spark, and this was the loud half of
 * "all effects are broken". It used to raise Q2_FX_WATER_SPLASH, whose site
 * (0x8003E0C0) sits inside 0x8003E014, and `xrefs 0x8003E014` returns exactly
 * one caller: 0x8003D4C4, inside the player's per-frame STATE think. No weapon
 * in the executable reaches it. Its ramp is record 0 — (64,64,255), pure blue,
 * additive — so every pellet that struck a wall painted a blue disc, fifteen
 * quads at a time, and a burst of buckshot painted a screenful of them.
 *
 * The real world arm is 0x80048990 -> 0x800489D8, which spawns through the
 * SECOND group spawner (0x8003004C) with grey and dark-red ramps: a smoke
 * puff. See q2_fx_bullet_puff. The address the old note cited for the spark,
 * 0x800486EC, is the EXPLOSION site and is what the missile tick's world arm
 * uses — not the bullet's.
 */
/*
 * `at` is the point the native caller holds when it gets here: the body's
 * entry from 0x800544EC when one was hit, else the world trace's own stop.
 * `world` says the world stopped the shot short of its range.
 */
static void fx_hitscan_impact(q2_sim *sim, const s32 origin[3],
                              const s32 dir[3], const s32 at[3], bool world,
                              u8 area, s32 victim,
                              const q2_damage_result *dr, s16 damage)
{
    if (!sim->fx_ready)
        return;

    if (victim >= 0) {
        /* Flesh only: armour taking the whole hit is the case the HUD's damage
         * flash also distinguishes. */
        if (!dr || dr->taken > 0)
            fx_at_area(sim, Q2_FX_BLOOD, at, area);
        /*
         * THE SECOND BURST IS GONE, and that is a correction rather than a trim.
         *
         * 0x800596B0 is not a gib burst. It lives inside the ITEM think
         * 0x80059330 — 0x800595AC advances the materialise scale, 0x800595C8
         * clamps it at 4096 — and its ramp is chosen from the ITEM's glow bits
         * at entity+0x44, which is the same word 0x800596B8 tests for the glow
         * light one instruction later. Raising it on a killing blow painted an
         * item's ramp-1 glow at the victim's feet.
         *
         * A kill raises nothing of its own. What the console raises when a
         * body COMES APART is the destruction dispatcher 0x8007CEB4, whose
         * default arm calls ThrowGibs 0x8005A3D4 (0x8007D138), which opens with
         * 0x8005B320 (0x8005A440): the MESH blood spray on ramps 2 and 3 at
         * size 6144, q2_fx_gib_spray in effect.h. That needs the victim's posed
         * model, which this file has no access to. Nothing extra is raised
         * here rather than raising the wrong thing.
         */
        return;
    }

    if (world) {
        /*
         * The area byte the console forwards here is `lbu s1, 32(v1)` at
         * 0x80048854 — the TRACE's own area record, not a fresh lookup of the
         * contact point. `world_fraction_for` already ran that trace and now
         * hands the byte back with the fraction, so this is the console's own
         * value at no extra hull cost. A trace that ended on a door or an
         * intact pane has no cell and therefore no area record (q2_trace.node
         * is -1 there); `fx_area_resolve` falls back to the contact point's own
         * cell in that case rather than storing a 0 the group draw would cull.
         */
        if (sim->fx_ready)
            q2_fx_bullet_puff(&sim->fx, &sim->fx_rng, at,
                              fx_area_resolve(sim, area, at));

        /*
         * And the BREAKABLE, tested along the whole shot rather than at its
         * end: the console's sweep runs INSIDE the trace, so a pane in front of
         * the wall is what the shot hits, not the wall behind it.
         */
        {
            /*
             * AND THE END IS PUSHED A LITTLE PAST THE IMPACT, or a pane can
             * never be shot at all.
             *
             * A pane is SOLID in the hull, so the pellet that is meant to
             * break it is the pellet the pane itself stopped — and the trace
             * hands back a point on the near side of that surface, a unit or
             * so outside the Scene node's own box (trace.c backs off one unit
             * along the dominant axis at 0x800453F8). Testing the segment
             * origin -> impact against the unpadded box therefore missed the
             * very case it exists for: LAB's node 205, fired on from 577 units
             * away, stopped every bullet at x 322..325 against a box whose
             * far face is x 321, and 285 rounds registered six hits — the six
             * that happened to enter across a corner.
             *
             * The console does not have the problem because its pane sweep
             * runs INSIDE the trace (0x8004874C): the pane is one of the
             * things the trace can stop on, and when it does, that IS the hit.
             * This port traces first and asks afterwards, so the question has
             * to be asked a few units further along the ray.
             *
             * Along the RAY and not by padding the box: the build deliberately
             * takes the node's own numbers rather than q2_scene_node_bounds'
             * culling slop, because a padded box would be shootable from
             * beside the pane. Extending the segment keeps the aim exact and
             * only forgives the back-off. The step is measured on the ray's
             * dominant axis, which is the norm 0x8006FA2C computes.
             */
            s32 to[3];
            s32 big = 0;
            int a;

            for (a = 0; a < 3; a++) {
                s32 m = dir[a] < 0 ? -dir[a] : dir[a];
                if (m > big) big = m;
            }
            for (a = 0; a < 3; a++)
                to[a] = big > 0
                      ? at[a] + (s32)(((s64)dir[a] * Q2_BREAKABLE_REACH) / big)
                      : at[a];

            q2_sim_breakable_shot(sim, origin, to, damage);
        }
    }
}

static u8 rail_area_at(void *user, const s32 at[3])
{
    return fx_area_resolve((q2_sim *)user, 0, at);
}

void q2_sim_rail_trail(q2_sim *sim, const s32 start[3], const s32 end[3],
                       s32 yaw, s32 pitch, s32 roll)
{
    /*
     * The shooter's matrix at entity+0x2C0, applied to the ring's (1, 0, 0)
     * and (0, 1, 0) with the Y negated after (0x800496B0, 0x800496C4). That is
     * the same rotate-then-negate the fire functions give their muzzle
     * offsets, which q2_weapon_muzzle_origin takes pre-negated — so the second
     * column goes in as (0, -1, 0).
     */
    static const s16 ring_x[3] = { Q2_ONE_12, 0, 0 };
    static const s16 ring_y[3] = { 0, -Q2_ONE_12, 0 };
    static const s32 none[3]   = { 0, 0, 0 };
    s32 col[3];
    s16 basis[2][3];
    int k;

    if (!sim || !sim->fx_ready || !start || !end)
        return;

    q2_weapon_muzzle_origin(ring_x, none, yaw, pitch, roll, col);
    for (k = 0; k < 3; k++)
        basis[0][k] = (s16)col[k];
    q2_weapon_muzzle_origin(ring_y, none, yaw, pitch, roll, col);
    for (k = 0; k < 3; k++)
        basis[1][k] = (s16)col[k];

    q2_fx_rail_trail(&sim->fx, start, end, (const s16 (*)[3])basis,
                     sim->multiplayer, sim->player_count, rail_area_at, sim);
}

/*
 * Which burst a projectile leaves behind.
 *
 * The BFG has a burst of its own, four times the size of any other
 * (0x8004BDBC). Everything else detonates as the missile tick's world arm
 * does: 0x800486EC, ramp 9, two groups.
 *
 * A BOLT USED TO BE MAPPED TO THE BLUE SPARK, on the strength of a comment
 * saying "0x8004D74C reaches the small blue spark rather than the fireball".
 * That is false, and the address does not say what it was read as saying:
 * 0x8004D74C is `jal 0x80048AFC` inside the bolt SPAWNER at 0x8004D70C — the
 * entity allocator, whose result is tested `beq s2, zero` two instructions
 * later and then filled in as a 104-byte missile record. It is not a burst
 * site at all, and the spark's only reachable caller is in the player's state
 * think (see fx_hitscan_impact).
 *
 * SO WHAT DOES A BOLT DO? Measured, not chosen. The missile tick (0x80047C6C)
 * has exactly two burst arms — 0x80048B64 blood when a victim was hit, and the
 * world arm at 0x8004866C which loads ramp 9 and loops `slti v0,s7,2` — and
 * BOTH are gated on bit 3 of the missile record's flag halfword at +0x22
 * (`lhu v0,-54(s3); andi v0,8; beq -> 0x80048700`, at 0x80048624 and
 * 0x80048658). The spawner writes that halfword from its FIFTH argument:
 * `lhu s1, 128(sp)` at 0x8004D714 against its own `addiu sp, sp, -112`, stored
 * by `sh s1, 34(s2)` at 0x8004D7BC.
 *
 * Reading the fifth argument at every call site settles it. `xrefs 0x8004D70C`
 * gives three:
 *
 *     0x8004C134   sw 11, 16(sp)          0b1011 — bit 3 set
 *     0x8004D400   sw 14, 16(sp)          0b1110 — bit 3 set
 *     0x800620EC   sw 11 or 14, 16(sp)    both arms, bit 3 set
 *
 * Every shipped bolt carries the bit. Its actor arm emits only blood, and its
 * world arm emits the orange burst. bolt_contact_tick keeps those arms and the
 * flag gate separate; the generic projectile path handles the other kinds.
 */
static q2_fx_preset_id fx_for_projectile(q2_proj_kind kind)
{
    switch (kind) {
    case Q2_PROJ_BFG:  return Q2_FX_BFG_BURST;
    default:           return Q2_FX_EXPLOSION;
    }
}

/*
 * QUAKEGENEXPLOSION, FOR A ROCKET OR A GRENADE THAT WENT OFF.
 *
 * Three missile thinks call 0x8005A778 with their own origin (+0x54), their
 * own surface byte (+0x9E), 4096 and kind 0 on every detonation:
 *
 *     PrimaryRocketActive    0x8004ADF0  any impact, before the direct hit
 *                                        and the 1300 blast
 *     PrimaryGrenadeLActive  0x80049F9C  meeting an entity
 *                            0x8004A01C  the fuse running out
 *     PrimaryGrenadeActive   0x8004A8E0  meeting an entity
 *                            0x8004A9B0  the fuse running out — in the hand
 *                                        too: the held arm marks an expired
 *                                        fuse, sets state 2 and re-dispatches
 *                                        (0x8004A464..0x8004A46C), and the
 *                                        release falls into the flying arm in
 *                                        the same call (0x8004A7D4)
 *
 * The bolt and the BFG ball never call it. So on the console a rocket or a
 * grenade bursts as the missile tick's particles AND as the Explosion model,
 * whose think raises the C0/40/31 light every tick (modelent.h) — and it
 * bangs (0x8005AA54, queued on `explosion_sounds`), and every pad within 4096
 * of it shakes (0x8005AA7C). This port drew the particles alone.
 *
 * That bang is the missile's ONLY one. The grenade thinks' own SoundByCreature
 * calls play 0x800B2B1C, `wep_grenlb1b`, the bounce; the weapon PreInit does
 * register `wep_grenlx1a` (0x8004DD88, into 0x800B2B30), but nothing in the
 * image ever reads that handle back.
 *
 * The surface byte is the projectile's cell's area. The port's projectile
 * carries no +0x9E, so it comes from the burst point through the 0x800686C4
 * lookup the bursts already use.
 *
 * A grenade here that meets the WORLD goes off instead of bouncing (the note
 * at the world arm below), so it explodes where the console's would bounce.
 * That is the existing deviation carried through, not a new one.
 */
static void projectile_explosion(q2_sim *sim, q2_proj_kind kind,
                                 const s32 at[3])
{
    if (kind != Q2_PROJ_ROCKET && kind != Q2_PROJ_GRENADE &&
        kind != Q2_PROJ_HAND_GRENADE)
        return;

    sim->projectile_blasts++;
    if (sim->entities_ready &&
        q2_model_ent_spawn(&sim->entities, sim->model_bank,
                           Q2_MODEL_ENT_EXPLOSION, at,
                           fx_area_resolve(sim, 0, at),
                           Q2_MODEL_ENT_SIZE_FULL, &sim->explosion_sounds))
        sim->projectile_models++;
}

/* ------------------------------------------------------------------------- */
static void environment_sound(void *user, const q2_actor *target, s16 mod)
{
    q2_sim *sim = (q2_sim *)user;
    int pi;
    if (sim->settling) return;
    for (pi = 0; pi < sim->player_count && pi < Q2_SIM_MAX_PLAYERS; pi++) {
        const q2_actor *a = pi == sim->cur_player ? &sim->combat.self : &sim->pcombat[pi].self;
        s32 at[3];
        if (a != target) continue;
        /* The retail helper increments before requesting a voice. A full
         * event queue or mixer therefore still consumes this admitted hit. */
        if (mod == Q2_MOD_LAVA && (sim->lava_burn_calls++ & 3u)) return;
        memcpy(at, sim->player[pi].pos, sizeof(at));
        at[1] = q2_sim_origin_y(at[1]);
        q2_ent_sound_player(&sim->ent_world.events, pi, Q2_SND_BURN, at);
        return;
    }
}

void q2_sim_combat_init(q2_sim *sim)
{
    if (!sim)
        return;

    memset(&sim->combat, 0, sizeof(sim->combat));

    q2_inventory_init(&sim->combat.inv);
    q2_combat_rules_default(&sim->combat.rules);
    sim->combat.rules.environment_sound = environment_sound;
    sim->combat.rules.environment_sound_user = sim;
    q2_projectiles_init(&sim->combat.projectiles);
    q2_rng_seed(&sim->combat.rng, 0x51ED2701u);

    /* A fresh player has the blaster and nothing else, which is what the
     * spawn path at 0x8003D4FC leaves in the weapon fields. */
    sim->combat.weapon_id        = Q2_WID_BLASTER;
    sim->combat.inv.weapons      =
        (u16)q2_weapon_tables_builtin()->owned_bit[Q2_WID_BLASTER];
    sim->combat.chaingun_bullets = 1;

    q2_actor_from_player(&sim->combat.self, &sim->combat.inv, sim->player[sim->cur_player].pos);

    /*
     * THE KILLER BYTE A PLAYER IS PLACED WITH. 0x8003DDF8 builds the body on a
     * freshly cleared entity (0x8006C18C's 768-byte memset) and then stores 4:
     * 0x8003DE24 `addiu v0, zero, 4` / 0x8003DE34 `sb v0, 222(s1)`. The memset
     * above leaves 0, which is player 0's index, and the refresh carries the
     * byte rather than re-seeding it (combat.h), so without this a deathmatch
     * kill by a creature nobody has hurt (the 0x80057E5C arm, no store) was
     * scored as player 0's suicide.
     *
     * Seeded here rather than through q2_actor_init, which would also move
     * `owner` from the memset's 0 to -1 and leave player 0 unnamed for every
     * caller that does not set it. The other fields the refresh carries (+223,
     * the effect bytes, client+0x94) are already the memset's 0, which is what
     * the cleared entity and the cleared client record (0x8003B2BC) hold.
     */
    sim->combat.self.last_attacker = (s8)Q2_MP_NOT_A_PLAYER;
}

/* ------------------------------------------------------------------------- */
void q2_sim_set_aim_view(q2_sim *sim, int player, const s32 eye[3], s32 node)
{
    q2_player *p;
    if (!sim || !eye || player < 0 || player >= Q2_SIM_MAX_PLAYERS) return;
    p = &sim->player[player];
    memcpy(p->aim_eye, eye, sizeof(p->aim_eye));
    p->aim_node = node;
    p->aim_view_valid = true;
}

static bool player_aim_visible(void *user, const s32 start[3],
                               const s32 end[3], int ray)
{
    q2_sim *sim = user;
    const q2_player *p = &sim->player[sim->cur_player];
    s32 point[3], node;
    (void)ray;
    /* FindPlayerTarget queries only PrimaryColl, never solid entities. */
    return !sim->coll_primary_ready ||
           q2_coll_move(&sim->coll_primary, start, end,
                        p->aim_view_valid ? p->aim_node : -1, point, &node);
}

void q2_sim_find_target(q2_sim *sim, s32 dt)
{
    q2_player *p;
    s32 eye[3], angles[3], current, target, step;
    if (!sim || dt <= 0) return;
    p = &sim->player[sim->cur_player];
    if (p->aim_view_valid) memcpy(eye, p->aim_eye, sizeof(eye));
    else q2_sim_eye(sim, eye);
    angles[0] = (s32)(0u - (u32)p->pitch); angles[1] = p->yaw; angles[2] = p->roll;
    q2_combat_find_target(eye, angles, &sim->combat.self, sim->combat.targets,
                          sim->combat.target_count, player_aim_visible, sim,
                          p->aim_direction, &p->aim_pitch);
    p->aim_ready = true;
    /* Seek halfword, 0x8006FE3C, with the caller's signed dt*4 halfword. */
    current = p->aim_assist; target = p->aim_pitch;
    step = (s16)((u32)dt * 4u);
    if (current < target) current = current + step < target ? current + step : target;
    else current = target < current - step ? current - step : target;
    p->aim_assist = (s16)current;
}

void q2_sim_aim(const q2_sim *sim, s16 out[3])
{
    s32 sy, cy, sp, cp;

    if (!out)
        return;
    if (!sim) {
        out[0] = out[1] = out[2] = 0;
        return;
    }

    if (sim->player[sim->cur_player].aim_ready) {
        memcpy(out, sim->player[sim->cur_player].aim_direction, sizeof(s16) * 3);
        return;
    }

    sy = q2_sin12(sim->player[sim->cur_player].yaw);   cy = q2_cos12(sim->player[sim->cur_player].yaw);
    sp = q2_sin12(sim->player[sim->cur_player].pitch); cp = q2_cos12(sim->player[sim->cur_player].pitch);

    /* The engine keeps this triple at client+0x3C..0x40 as a 1.3.12 unit
     * vector; every fire function then scales it for itself — >> 6 for a
     * blaster bolt, << 2 for a bullet — so the port has to hand over the same
     * magnitude or every weapon's range and spread come out wrong together. */
    out[0] = (s16)(((s64)cp * sy) >> 12);
    out[1] = (s16)(-sp);          /* world Y grows downward */
    out[2] = (s16)(((s64)cp * cy) >> 12);
}

void q2_sim_set_targets(q2_sim *sim, q2_actor **targets, u32 count)
{
    if (!sim)
        return;
    sim->combat.targets      = targets;
    sim->combat.target_count = targets ? count : 0;
}

void q2_sim_set_bodies(q2_sim *sim, const q2_move_body *bodies, u32 count)
{
    if (!sim)
        return;
    sim->extra_bodies      = bodies;
    sim->extra_body_count  = bodies ? count : 0;
}

int q2_sim_weapon_after_pickup(const q2_sim *sim, const q2_inventory *inv,
                               int held, int weapon_id)
{
    const q2_weapon_tables *t = q2_weapon_tables_builtin();
    u32 i;
    int rank_new = -1, rank_held = -1;

    if (!sim || !inv || weapon_id <= 0 || weapon_id > Q2_WID_COUNT)
        return held;

    /*
     * AUTOSWITCH, and it is a DEVIATION from the console rather than a fix to
     * it — stated here so the next reader does not "correct" it back.
     *
     * 0x80037E78 switches to the weapon just picked up only when the BLASTER is
     * the one in hand: take a shotgun while holding a railgun and the railgun
     * stays. That is the disc's behaviour and it is what `autoswitch == false`
     * still does, exactly — 0x80037E84's store, and nothing else.
     *
     * With it on — the default, at the owner's request — a pickup that ranks
     * ABOVE the held weapon is taken up instead. The ranking is not invented:
     * it is the console's own preference list at 0x8009DB7C, the same order
     * `q2_weapon_autoselect` walks after a shot empties something, so the gun
     * a pickup promotes you to is the gun the engine would have chosen for you
     * anyway. Explosives are absent from that list by design (weapontables.c),
     * which is what keeps a grenade pickup from arming a grenade in your hand.
     *
     * A weapon with no ammo does not win: `q2_weapon_usable` gates the walk, so
     * picking up a railgun you cannot feed leaves you holding what you had.
     */
    if (!sim->autoswitch)
        return held == Q2_WID_BLASTER ? weapon_id : held;

    for (i = 0; i < t->autoswitch_count; i++) {
        if (t->autoswitch[i] == weapon_id && rank_new < 0)
            rank_new = (int)i;
        if (t->autoswitch[i] == held && rank_held < 0)
            rank_held = (int)i;
    }

    /* Off the list entirely — an explosive — is never promoted to. */
    if (rank_new < 0)
        return held;

    /* Holding something the list does not rank (the blaster is on it, so this
     * is an explosive in hand) means anything ranked wins. */
    if (rank_held < 0 || rank_new < rank_held) {
        if (q2_weapon_usable(inv, weapon_id))
            return weapon_id;
    }

    return held;
}

bool q2_sim_give_weapon(q2_sim *sim, int weapon_id)
{
    const q2_weapon_tables *t = q2_weapon_tables_builtin();

    if (!sim || weapon_id <= 0 || weapon_id > Q2_WID_COUNT)
        return false;
    if (sim->combat.inv.weapons & t->owned_bit[weapon_id])
        return false;

    sim->combat.inv.weapons |= t->owned_bit[weapon_id];
    sim->combat.weapon_id   = q2_sim_weapon_after_pickup(
        sim, &sim->combat.inv, sim->combat.weapon_id, weapon_id);

    return true;
}

bool q2_sim_cycle_weapon(q2_sim *sim, int dir)
{
    int next;

    if (!sim)
        return false;

    next = q2_weapon_cycle(&sim->combat.inv, sim->combat.weapon_id, dir);
    if (next == Q2_WID_NONE)
        return false;

    sim->combat.weapon_id = next;
    return true;
}

bool q2_sim_autoselect_weapon(q2_sim *sim)
{
    int best;

    if (!sim)
        return false;

    /*
     * 0x800506C4, the refire pass's own selection: walk the fixed preference
     * list and take the first entry that is both owned and fed. Idempotent —
     * a player already holding the best affordable weapon re-picks it and
     * nothing changes, which is exactly why the console can afford to run it
     * after every shot.
     *
     * This is NOT q2_sim_cycle_weapon. That one transcribes 0x80050758, the
     * +/-1 neighbour scan the console runs twice per pass only to refill its
     * next/previous caches, and it never writes the held weapon. The refire
     * pass used to call it, so every shot advanced the carousel by one.
     */
    best = q2_weapon_autoselect(&sim->combat.inv);
    if (best == Q2_WID_NONE || best == sim->combat.weapon_id)
        return false;

    sim->combat.weapon_id = best;
    return true;
}

/* ------------------------------------------------------------------------- */
/*
 * How far along a shot's direction the world lets it travel, as a 1.0.12
 * fraction. The direction carries the range — 0x80048790 forms the trace end as
 * origin + dir — so the fraction the hull returns is exactly what the entity
 * pass needs to bound itself with.
 */
/*
 * THE COMBAT RULES ARE SESSION STATE, and until now only the clock in them was.
 *
 * `q2_combat_rules_default` memsets the struct and writes skill 1, and the two
 * call sites that touched it afterwards refreshed `level_time` and nothing
 * else. So the other two fields were frozen at their defaults for the whole run
 * and every rule that reads them was dead:
 *
 *   - `skill` never left 1, so 0x800582C8's "a monster hits you at skill 0 for
 *     half" never fired, even though the front end already knows the skill and
 *     hands it to the creature AI through `q2_cre_set_skill`. Easy was as
 *     dangerous as medium. Armour's rounding bias hangs off the same halfword
 *     (0x80057C10 `lh 0x800B334A`, then 0x80057C18 `bne v0, zero` to the 2048
 *     in its delay slot), so easy's 4095 was never used either.
 *   - `deathmatch` never left false, so the railgun's 150 (0x8004D5D8) was
 *     unreachable and the -3072 rocket-jump ceiling was applied inside
 *     deathmatch, where 0x800580E8 does not apply it. The armour bias does not
 *     read it: the deathmatch word 0x800AEBCC is loaded at 0x80057DA8 for the
 *     killer byte and at 0x800580D0 for that ceiling (tested at 0x800580E0),
 *     while the bias's own load, 0x80057C10, is the skill halfword above.
 *
 * Both are read from where the rest of the port already keeps them rather than
 * from a new copy: the skill from the AI's own global, deathmatch from the
 * `sim->multiplayer` flag that `sim->ent_world.deathmatch` is already taken
 * from, so the item half and the combat half of deathmatch cannot disagree.
 *
 * And the third frozen field was `cheats`. The damage function reads the GAME
 * VARIABLES word straight out of its global — 0x80058398 `lhu` of 0x800B29EC —
 * for ONE SHOT KILL, and `q2_combat_rules_default` zeroes it. The client
 * already writes the menu's word into `sim->cheats` (0x8001C698's hand-off),
 * so without this line the cheat reached the sim and stopped one struct short
 * of the only instruction that reads it: a zero word, forever. Copied from the
 * same field the item dispatch already reads bit 0 of, so there is still one
 * 0x800B29EC in the port.
 *
 * And the fourth was `knockback_mass`, BLAST FORCE. 0x80057F84 reads
 * 0x800B3358 on every hit and scales the impulse by 125*(v+64)>>6, or
 * 25*(v+64)>>2 when a player hurt themselves, so a zero there is not a
 * disabled rule but a halved one: every rocket, grenade, rail slug and bullet
 * pushed at half the disc's severity, and gibs were thrown half as far.
 * Carried from `sim->blast_force`, which q2_sim_init seeds with the reset
 * routine's 64 and the menu overwrites with the slider.
 */
static void sync_rules(q2_sim *sim)
{
    sim->combat.rules.level_time = sim->level_time;
    sim->combat.rules.skill      = (s16)q2_cre_skill();
    sim->combat.rules.deathmatch = sim->multiplayer;
    sim->combat.rules.cheats     = sim->cheats;   /* 0x80058398 lhu 0x800B29EC */
    sim->combat.rules.knockback_mass = (s16)sim->blast_force;  /* 0x800B3358 */
}

/*
 * How far the world let the shot go, and WHICH AREA it stopped in.
 *
 * The console's hitscan keeps both: 0x80048854 `lbu s1, 32(v1)` reads the
 * area record straight off the trace's own cell and carries it into the
 * impact burst. The port formed the same trace and threw the cell away, so the
 * puff and the blood had nothing to pass but a literal 0 — which the group
 * draw culls. `area` is optional; it is 0 when the trace ended on a runtime
 * entity box (a door or an intact pane, q2_trace.node == -1), which is not a
 * cell and has no area record at all.
 */
static s32 world_fraction_for(q2_sim *sim, const s32 origin[3],
                              const s32 dir[3], u8 *area)
{
    q2_trace tr;
    s32 end[3];
    int k;

    if (area)
        *area = 0;
    if (!sim->coll_ready)
        return 4096;

    for (k = 0; k < 3; k++)
        end[k] = origin[k] + dir[k];

    q2_sim_trace(sim, origin, end, &tr);
    if (area && tr.node >= 0)
        *area = (u8)(tr.contents & 0x7F);
    return tr.hit ? tr.fraction : 4096;
}

/*
 * Is the SIM'S OWN PLAYER behind this origin pointer? combat.c hands the clear
 * test `t->origin` itself (q2_combat_radius_damage_traced), so a pointer
 * compare against the sim's player actors is exact, and a creature — whose
 * actor the client owns — can never compare equal.
 */
static int player_for_actor_origin(const q2_sim *sim, const s32 *origin)
{
    int k;

    if (origin == sim->combat.self.origin)
        return sim->cur_player;
    for (k = 0; k < Q2_SIM_MAX_PLAYERS; k++)
        if (origin == sim->pcombat[k].self.origin)
            return k;
    return -1;
}

/*
 * "Can the blast see this candidate?" — 0x80050810's two gates, which sit
 * between the falloff and the damage call and skip the candidate on a zero:
 *
 *   0x80050A24  jal 0x80044C44   a0 = 0x800C8E90 (PrimaryColl), a1 = the
 *                                blast, a2 = candidate+0x54, a3 = the
 *                                projectile's own cell (the caller's 2nd arg)
 *   0x80050A3C  jal 0x80053974   a0 = the blast, a1 = candidate+0x54, a2 = 0
 *
 * 0x80044C44 is the hull's swept move — collision.h names it q2_coll_move —
 * and 0x80053974 is the 48-slot entity-box clip at 0x800CAE10, which returns
 * 0 when a box stops the segment. So the pair is "the hull lets the segment
 * through, and then no door, lift or intact pane does", which is exactly the
 * two passes q2_sim_trace makes, in the same order, through the same hull.
 *
 * THE ZONE/NODE QUERY IS STILL NOT MODELLED AS A SEPARATE GATE. combat.h
 * describes 0x80050A24 as a zone/node visibility query and leaves it out;
 * nothing here adds one. Read at the call, that `jal` is the same swept move
 * this trace already runs, so modelling it again would trace the hull twice.
 * The one argument q2_sim_trace does not pass is the cell HINT in a3: it
 * starts from -1, a brute-force sweep (0x80044C74). That only changes which
 * cell a boundary point is placed in, and q2_coll_point_in_node is inclusive,
 * so a blast sitting on the face it struck is still inside that cell.
 *
 * Two host-side adjustments:
 *
 *   - Player actor origins vary across adapters, but the player's simulation
 *     position always holds its feet. Resolve a player by pointer identity
 *     and aim at the canonical entity origin, without subtracting eye height
 *     twice from an actor already seated by player_damage_begin.
 *   - A blast point in NO CELL (tr.node < 0) is let through. The console's
 *     blast is where its projectile's own move left it, always inside a cell;
 *     malformed host inputs can still place one outside the hull. There the
 *     question has no console answer, so it keeps the distance-only result, and
 *     `q2_sim_proj_scan.splash_unplaced` counts every such blast.
 *
 * Behind `coll_ready`, exactly as world_fraction_for is: no movement hull, no
 * world, and everything is visible.
 */
static bool splash_clear(void *ctx, const s32 from[3], const s32 to[3])
{
    q2_sim *sim = (q2_sim *)ctx;
    q2_trace tr;
    s32 aim[3];
    int player;

    /* --god is a host capture aid. Keep the body in collision while refusing
     * its splash damage, including when the published world list contains it.
     * Radius damage passes the candidate's actual origin array to this hook. */
    if (sim && sim->invulnerable && to == sim->combat.self.origin)
        return false;

    if (!sim || !sim->coll_ready || !from || !to)
        return true;

    player = player_for_actor_origin(sim, to);
    aim[0] = to[0];
    aim[1] = player >= 0 ? q2_sim_origin_y(sim->player[player].pos[1]) : to[1];
    aim[2] = to[2];

    q2_sim_proj_scan.splash_asked++;
    q2_sim_trace(sim, from, aim, &tr);

    if (tr.hit && tr.node < 0) {
        q2_sim_proj_scan.splash_unplaced++;
        return true;
    }
    if (tr.hit)
        q2_sim_proj_scan.splash_occluded++;
    return !tr.hit;
}

/* Player inventories own pickups and ammunition; actors own damage during a
 * trace. Synchronise at the damage boundary, before another owner can be
 * selected or an item can mutate an inventory. Health-only write-back loses
 * armour and shield cells, and waiting until after a swap loses hits on player 0.
 * Preserve actor effects/attribution rather than reinitialising a target. */
static void player_damage_begin(q2_sim *sim)
{
    int pi;
    for (pi = 0; pi < sim->player_count && pi < Q2_SIM_MAX_PLAYERS; pi++) {
        q2_actor *a = pi == sim->cur_player ? &sim->combat.self
                                           : &sim->pcombat[pi].self;
        const q2_inventory *inv = pi == sim->cur_player ? &sim->combat.inv
                                                       : &sim->pcombat[pi].inv;
        a->health = inv->health;
        a->armour = inv->armour;
        a->armour_class = inv->armour_class;
        a->cells = inv->ammo[Q2_AMMO_CELLS];
        a->powerups = inv->flags;
        a->invuln_until = inv->invuln_until;
        a->protect_until = inv->enviro_until;
        a->origin[0] = sim->player[pi].pos[0];
        a->origin[1] = q2_sim_origin_y(sim->player[pi].pos[1]);
        a->origin[2] = sim->player[pi].pos[2];
    }
}

static void player_damage_impulse(q2_sim *sim, int pi, q2_actor *actor)
{
    int axis;
    if (!actor->knocked) return;
    for (axis = 0; axis < 3; axis++) {
        sim->player[pi].impulse[axis] =
            (s16)(sim->player[pi].impulse[axis] + actor->knockback[axis]);
        actor->knockback[axis] = 0;
    }
    sim->player[pi].impulse_armed = true;
    actor->knocked = false;
}

static void player_damage_end(q2_sim *sim)
{
    int pi;
    for (pi = 0; pi < sim->player_count && pi < Q2_SIM_MAX_PLAYERS; pi++) {
        q2_actor *a = pi == sim->cur_player ? &sim->combat.self
                                          : &sim->pcombat[pi].self;
        q2_inventory *inv = pi == sim->cur_player ? &sim->combat.inv
                                                 : &sim->pcombat[pi].inv;
        q2_actor_to_player(a, inv);
        player_damage_impulse(sim, pi, a);
    }
}

/* BIGGUN +0x2DC0..+0x2F2C: after the generator disappears, zone 1
 * periodically hurts living clients, creature kinds 0..37 and kind 47.
 * Player zero's entity+0x98 bit 0x4000 pauses the ENTIRE countdown. The
 * module adds 300 once, preserving overshoot, and calls ordinary T_Damage
 * (NULL, target, 5, 9, origin), including its armour/environment throttle. */
void q2_sim_glint_evacuation_tick(q2_sim *sim, s32 dt)
{
    q2_actor **list;
    u32 count, i;
    int pi;
    q2_fx_glint *g;
    if (!sim || sim->settling || dt <= 0) return;
    g = &sim->glint;
    if (!g->scripted || !g->zone_active || !g->finished ||
        (sim->player[0].ent.flags & 0x4000u)) return;
    g->evacuation_left -= dt;
    if (g->evacuation_left > 0) return;
    g->evacuation_left += 300;
    sync_rules(sim);
    player_damage_begin(sim);
    for (pi = 0; pi < sim->player_count && pi < Q2_SIM_MAX_PLAYERS; pi++) {
        q2_actor *a = pi == sim->cur_player ? &sim->combat.self
                                           : &sim->pcombat[pi].self;
        if (a->health > 0) {
            (void)q2_combat_damage(NULL, a, 5, Q2_MOD_ACID, a->origin,
                                   &sim->combat.rules);
        }
    }
    list = sim->world_targets ? sim->world_targets : sim->combat.targets;
    count = sim->world_targets ? sim->world_target_count : sim->combat.target_count;
    for (i = 0; list && i < count; i++) {
        q2_actor *a = list[i];
        bool player = a == &sim->combat.self;
        for (pi = 0; pi < Q2_SIM_MAX_PLAYERS; pi++)
            player = player || a == &sim->pcombat[pi].self;
        if (!a || player || a->health <= 0) continue;
        if (a->has_client || a->entity_kind < 38 || a->entity_kind == 47)
            (void)q2_combat_damage(NULL, a, 5, Q2_MOD_ACID, a->origin,
                                   &sim->combat.rules);
    }
    player_damage_end(sim);
}

/*
 * THE MUZZLE IS PULLED BACK INTO THE ROOM before anything leaves it.
 *
 * Every fire function forms its muzzle point as eye + the rotated offset and
 * then calls Find3dSpaceGivenSecondary (0x80055054) with that point, the
 * shooter's own origin at entity+0x54 and its cell at +0xA2 — the
 * machinegun's at 0x8004C890. That moves from the origin to the muzzle
 * through PrimaryColl (0x80044C44), and what the shot is built from is not
 * the point it was handed but where the move stopped, read back from
 * CollMap+40 (`lw a0, 40(v1)` at 0x8004C8A0, the three words copied over the
 * origin at 0x8004C8AC..0x8004C8C0), with the cell from CollMap+26.
 *
 * WHEN IT BITES. SecondaryCol keeps the origin 286 from every face, and a
 * level muzzle 250 ahead of the eye never reaches one: BASE1 and BASE2 runs
 * clip nothing. The eye sits up to 290 above the origin, though, so aiming up
 * under a low ceiling — or into any face the box erosion is not square to —
 * puts the formed point inside or beyond it, and there the port fired from
 * the far side while the console fires from the face and hits it. Every
 * hitscan and projectile path does the
 * same (0x8004C078 blaster, 0x8004C2D4 shotgun, 0x8004C598 super shotgun,
 * 0x8004CBD8 chaingun, 0x8004CF34 grenade launcher, 0x8004D344
 * hyperblaster, and inside AnyoneFireInstantRail 0x800491CC and
 * AnyoneFireRocketL 0x8004AF6C). AnyoneFireBFG and the hand grenade's spawner
 * have no such call.
 *
 * A shooter standing in no PrimaryColl cell keeps the unclipped point: the
 * console's start comes from the +0xA2 hint and always has a cell, and the
 * port's move would instead leave the muzzle at the origin.
 */
static void clip_muzzle(q2_sim *sim, q2_fire_result_v2 *r)
{
    const q2_player *p = &sim->player[sim->cur_player];
    s32 from[3], to[3], end[3], node = -1;
    u32 i;

    if (!sim->coll_primary_ready || r->shot_count == 0)
        return;
    if (r->kind == Q2_FK_BFG || r->kind == Q2_FK_HAND_GRENADE)
        return;

    from[0] = p->pos[0];
    from[1] = q2_sim_origin_y(p->pos[1]);
    from[2] = p->pos[2];
    memcpy(to, r->shot[0].origin, sizeof(to));

    (void)q2_coll_move(&sim->coll_primary, from, to, -1, end, &node);
    if (sim->coll_primary.current_node < 0) {
        q2_sim_proj_scan.muzzle_unplaced++;
        return;
    }
    if (end[0] != to[0] || end[1] != to[1] || end[2] != to[2]) {
        q2_sim_proj_scan.muzzle_clipped++;
        Q2_DEBUG("muzzle: player at (%d,%d,%d) yaw %d, (%d,%d,%d) clipped to "
                 "(%d,%d,%d)", (int)p->pos[0], (int)p->pos[1], (int)p->pos[2],
                 (int)p->yaw, (int)to[0], (int)to[1], (int)to[2],
                 (int)end[0], (int)end[1], (int)end[2]);
    }

    /* One muzzle per call: every pellet of a shotgun shares it. */
    for (i = 0; i < r->shot_count; i++)
        memcpy(r->shot[i].origin, end, sizeof(end));
}

q2_fire_result_v2 q2_sim_fire(q2_sim *sim)
{
    return q2_sim_fire_weapon(sim, sim ? sim->combat.weapon_id : 0,
                              sim ? sim->combat.chaingun_bullets : 0);
}

q2_fire_result_v2 q2_sim_fire_weapon(q2_sim *sim, int weapon_id,
                                    int chaingun_bullets)
{
    q2_fire_result_v2 r;
    s32 eye[3];
    s16 aim[3];
    s32 prev_next_fire;
    s32 launched;
    u32 i;

    memset(&r, 0, sizeof(r));
    r.sound = -1;
    if (!sim)
        return r;

    prev_next_fire = sim->combat.next_fire;

    q2_sim_eye(sim, eye);
    q2_sim_aim(sim, aim);

    sync_rules(sim);
    q2_actor_from_player(&sim->combat.self, &sim->combat.inv, sim->player[sim->cur_player].pos);

    r = q2_weapon_fire(&sim->combat.inv, &sim->combat.rng, NULL,
                       weapon_id, eye,
                       sim->player[sim->cur_player].yaw,
                       sim->player[sim->cur_player].pitch,
                       sim->player[sim->cur_player].roll, aim,
                       sim->level_time, sim->combat.next_fire,
                       /*
                        * QUAD, which was a hardcoded `false` — so the powerup
                        * was picked up, counted down on the HUD and played
                        * `itm_damage3` on every shot while multiplying nothing.
                        *
                        * Every fire function opens by comparing the level clock
                        * against the player's own expiry word (client+0xAC,
                        * which is `quad_until`) and picks the second immediate
                        * on the near side of it — 8 or 32, 6 or 24, 120 or 480.
                        * That comparison is this argument, and the view model
                        * already forms exactly it for the sound
                        * (`q2_vw.quad_active`).
                        */
                       sim->level_time < sim->combat.inv.quad_until,
                       sim->combat.rules.deathmatch,
                       chaingun_bullets);

    sim->combat.last_shot = r;
    sim->combat.shot_serial++;
    if (!r.fired) {
        /* A dry trigger still takes the gate — see Q2_WEAPON_DRY_REFIRE. A
         * shot blocked by the clock does not, and must not: overwriting the
         * deadline from a blocked tick would push it forward forever and the
         * weapon would never fire again. */
        if (r.dry)
            sim->combat.next_fire = r.next_fire;
        return r;
    }

    clip_muzzle(sim, &r);
    sim->combat.last_shot = r;

    /*
     * The muzzle flash, and there are FOUR weapons with one, not two.
     *
     * `q2_weapon_has_muzzle_light` answers for the machinegun and the chaingun,
     * which are the two that reach AddLight (0x80075C34) — and enumerating that
     * function's call sites is exactly why the other two were missed. The
     * shotgun and the super shotgun each raise a TIMED light instead:
     *
     *   QPlayerFireShotGun       0x8004C3B0 a0 = entity+0xA4, radii 600/1200
     *                            from 0x800AE9D8/0x800AE9DA, ticks 3
     *   QPlayerFireSuperShotGun  0x8004C674 a0 = entity+0x54, radii 800/1600
     *                            from 0x800AE9E4/0x800AE9E6, ticks 4
     *
     * both through AddTimedLight (0x80075D14), both taking their colour from
     * the same `C8 64 64` at 0x800AE9D4 the machinegun uses. So the super
     * shotgun has the largest and longest flash the player owns and it was the
     * one raising nothing at all. `q2_light_muzzle_preset` is the table.
     *
     * Drawn AFTER the shot so the flash does not perturb the spread sequence,
     * which is what the fire function's own ordering does — and for the two
     * timed weapons that ordering is the console's literally, because their
     * AddTimedLight call comes after the pellet loop (0x8004C398's `slti 5`).
     */
    {
        q2_muzzle_light ml;

        if (r.shot_count && q2_light_muzzle_preset((u8)weapon_id, &ml)) {
            s32 inner = ml.inner;
            s32 outer = ml.outer;
            s32 lit[3];

            /*
             * The machinegun and the chaingun draw their radii from TWO
             * rand()s, inner first (0x8004C970, 0x8004C990) — see weapon.h.
             * Sequenced into locals, because the order C evaluates a call's
             * arguments in is unspecified and the order here is the console's.
             * The two shotguns carry fixed halfword pairs and must not touch
             * the RNG, or a fired shotgun would shift every later spread.
             */
            if (ml.random_radii) {
                s32 ra = q2_rng_next(&sim->combat.rng);
                s32 rb = q2_rng_next(&sim->combat.rng);

                q2_weapon_muzzle_light((u8)weapon_id, ra, rb,
                                       &inner, &outer);
            }

            /*
             * At the player ENTITY ORIGIN, not at the eye and not at the feet.
             *
             * `sim->player[].pos` is the FEET (sim.h says so at the top of the
             * file, and sim.c converts with q2_sim_origin_y everywhere it wants
             * the entity). Passing it raw moved the flash from 290 above the
             * origin to 286 below it — a correction in the right direction that
             * overshot by the whole body.
             *
             * AND THE TWO OFFSETS ARE NOT COLLAPSED HERE — they are already the
             * same point. The preset carries 0xA4 for the shotgun and 0x54 for
             * the other three because those are the fields the console passes,
             * but on a PLAYER entity this port makes them equal: entity.c's
             * player update writes `ent.origin[] = ent.pos[]` outright, with
             * the note that for the player +0x54 IS the origin. They diverge
             * for an entity with a model height bias at +0xF8, which a player
             * has not. Kept in the record so the day a player entity grows one,
             * the two arms are already distinguishable.
             */
            lit[0] = sim->player[sim->cur_player].pos[0];
            lit[1] = q2_sim_origin_y(sim->player[sim->cur_player].pos[1]);
            lit[2] = sim->player[sim->cur_player].pos[2];

            if (ml.ticks > 0)
                q2_ent_light_timed_at(&sim->ent_world.events, lit, ml.rgb,
                                      inner, outer, ml.ticks);
            else
                q2_ent_light_at(&sim->ent_world.events, lit, ml.rgb,
                                inner, outer);
        }
    }

    sim->combat.next_fire = r.next_fire;

    /*
     * And on to the view, which is where a kick was always going: the weapon
     * table's figure has had a home in `combat.kick` for a while and nothing
     * read it. `q2_sim_view_angles` composes it over 30 ticks (FORMATS.md
     * §9.12.11a), so the deadline is what turns one number into recoil.
     *
     * ONLY THE FIELDS THE FIRE FUNCTION STORES. All three used to be written
     * on every shot and the deadline re-armed for every weapon, which zeroed a
     * machinegun burst's yaw and roll under a following shotgun's pitch and
     * cancelled any kick in flight when a hyperblaster, a BFG or a hand
     * grenade fired. The console's shotgun writes pitch alone, its chaingun
     * yaw and roll alone, and those three write nothing at all (weapon.h).
     */
    if (r.kick_axes) {
        q2_player *p = &sim->player[sim->cur_player];
        int k;

        for (k = 0; k < 3; k++) {
            if (!(r.kick_axes & (1u << k)))
                continue;
            sim->combat.kick[k] = r.kick[k];
            p->kick[k]          = r.kick[k];
        }
        p->kick_time = sim->level_time + Q2_VIEW_KICK_FIRE;
    }

    player_damage_begin(sim); /* after the fire function spends ammunition */
    switch (r.kind) {
    case Q2_FK_BULLET:
        /* Every pellet is its own trace, which is why a shotgun can catch two
         * creatures and a machinegun cannot. */
        for (i = 0; i < r.shot_count; i++) {
            const q2_shot *s = &r.shot[i];
            u8  frac_area;
            s32 frac = world_fraction_for(sim, s->origin, s->dir,
                                          &frac_area);
            q2_damage_result dr;
            s32 victim, at[3];

            /* The blood lands where the round entered the body — the point
             * 0x800544EC writes back — not at the wall behind it. */
            victim = q2_combat_hitscan(&sim->combat.self, s->origin, s->dir,
                                       s->damage, Q2_MOD_BULLET, frac,
                                       sim->combat.targets,
                                       sim->combat.target_count,
                                       &sim->combat.rules, &dr, at);
            fx_hitscan_impact(sim, s->origin, s->dir, at,
                              victim < 0 && frac < 4096, frac_area,
                              victim, &dr, s->damage);
        }
        break;

    case Q2_FK_RAIL: {
        const q2_shot *s = &r.shot[0];
        u8  frac_area;
        s32 frac = world_fraction_for(sim, s->origin, s->dir, &frac_area);
        q2_damage_result dr;
        s32 at[3], world_end[3], victim;
        int k;

        /*
         * ONE BODY. 0x80049318 asks 0x800544EC once and 0x80049334 deals one
         * DoDamage; the beam itself keeps the world's end (s2+48), so it is
         * drawn through the body it stopped in. This port used to visit every
         * cylinder on the line, which is the PC game's rail, not this one.
         */
        victim = q2_combat_hitscan(&sim->combat.self, s->origin, s->dir,
                                   s->damage, Q2_MOD_RAIL, frac,
                                   sim->combat.targets,
                                   sim->combat.target_count,
                                   &sim->combat.rules, &dr, at);
        if (victim >= 0)
            fx_hitscan_impact(sim, s->origin, s->dir, at, false, frac_area,
                              victim, &dr, s->damage);
        for (k = 0; k < 3; k++)
            world_end[k] = s->origin[k] +
                           (s32)(((s64)s->dir[k] * frac) >> Q2_FRAC_12);
        fx_hitscan_impact(sim, s->origin, s->dir, world_end, frac < 4096,
                          frac_area, -1, NULL, s->damage);

        /* And the spiral, from the muzzle to where the trace stopped. */
        if (sim->fx_ready) {
            const q2_player *p = &sim->player[sim->cur_player];

            q2_sim_rail_trail(sim, s->origin, world_end, p->yaw, p->pitch,
                              p->roll);
        }
        break;
    }

    default:
        /* WHO fired it. The -1 here meant "the world", so a bolt could not say
         * who to credit and a kill by one had no killer. */
        q2_sim_proj_scan.launched++;
        launched = q2_projectile_launch(&sim->combat.projectiles, &r,
                                        sim->cur_player, sim->level_time);
        if (launched >= 0) {
            q2_projectile *lp = &sim->combat.projectiles.p[launched];

            /* The attitude its model is drawn with: the player's yaw for a
             * rocket, three of the same rand()s the shot drew for a grenade
             * (projectile.h). */
            if (lp->kind == Q2_PROJ_ROCKET) {
                q2_projectile_aim_rocket(lp, sim->player[sim->cur_player].yaw);
            } else if (lp->kind == Q2_PROJ_GRENADE) {
                s32 r0 = q2_rng_next(&sim->combat.rng);
                s32 r1 = q2_rng_next(&sim->combat.rng);
                s32 r2 = q2_rng_next(&sim->combat.rng);

                q2_projectile_set_attitude(lp, r0, r1, r2);
            } else if (lp->kind == Q2_PROJ_BFG) {
                const q2_player *pl = &sim->player[sim->cur_player];
                q2_model mdl;
                q2_model_move mv;
                s32 bfg = sim->model_bank
                        ? q2_model_bank_find(sim->model_bank, "BFGBlast")
                        : -1;

                q2_projectile_set_attitude(lp, -pl->pitch, -pl->yaw,
                                           pl->roll);
                if (bfg >= 0 &&
                    q2_model_get(sim->model_bank, (u32)bfg, &mdl) == Q2_OK &&
                    q2_model_move_by_name(&mdl, "Spin", &mv))
                    q2_projectile_spin_bfg(lp, (s32)mv.start * 5,
                                           (s32)mv.end * 5);
            }
        }
        if (launched < 0) {
            /*
             * THE POOL WAS FULL, and the shot has already been paid for:
             * q2_weapon_fire decrements the ammo and advances the refire gate
             * before this line is reached, so swallowing the launch charges the
             * player for nothing. Refund both and report the shot as not fired,
             * which is also what stops the view model playing a fire clip for a
             * projectile that does not exist.
             *
             * This used to be unreachable in practice only because projectiles
             * never terminated (they moved at a twentieth of their speed and
             * filled the pool); with the step fixed it is rare, and it is still
             * wrong to lose a rocket to it.
            */
            q2_sim_proj_scan.dropped_full++;
            /* Grenade3 has only been primed here; its ammo is not charged
             * until the 411 release crossing, so there is nothing to refund. */
            if (r.kind != Q2_FK_HAND_GRENADE)
                q2_weapon_refund(&sim->combat.inv, weapon_id);
            sim->combat.next_fire = prev_next_fire;
            r.fired = false;
            /* The same attempt amended, not a new one, so the serial stays
             * where the write above left it. */
            sim->combat.last_shot = r;
        } else if (r.kind == Q2_FK_HAND_GRENADE && sim->fire_from_input) {
            /* A harness with no view-model machine has no 261/380/411
             * timeline to drive Grenade3. Preserve that API's historical
             * "attack fires" contract by releasing at minimum charge in the
             * same call. The playable client sets fire_from_input false and
             * follows the retail held path below. */
            if (q2_sim_hand_grenade_update(sim, eye, 0, true) ==
                Q2_HAND_GRENADE_RELEASED) {
                r.sound = Q2_WSND_HANDGREN_THROW;
                sim->combat.last_shot = r;
            }
        }
        break;
    }

    /* Projectile allocation failure may refund cells; no trace has run on
     * that arm, so keep its inventory as the source. */
    if (r.kind == Q2_FK_BULLET || r.kind == Q2_FK_RAIL)
        player_damage_end(sim);
    return r;
}

q2_damage_result q2_sim_hurt_player(q2_sim *sim, q2_actor *attacker,
                                    s16 damage, s16 mod, const s32 point[3])
{
    q2_damage_result out;
    s32 origin[3];

    memset(&out, 0, sizeof(out));
    if (!sim)
        return out;

    /*
     * A CAPTURE AID, off unless a harness asks for it: the player takes no
     * damage. It exists because several things worth photographing — a
     * creature's death animation, a corpse settling, a long fight — outlast
     * the player in any engagement dense enough to produce them, and every
     * frame captured of one was a death-cam view instead. Returning the empty
     * result rather than clamping health keeps the whole chain quiet: no
     * flinch, no kick, no blood, no death.
     */
    if (sim->invulnerable)
        return out;

    /*
     * Health and armour live in the inventory, everything else in the actor, so
     * the two are synchronised around the call rather than duplicated.
     *
     * AT THE ENTITY ORIGIN, entity+0x54, which is mid-body: 286 above the feet
     * (q2_sim_origin_y), where player_damage_begin also puts it. DoDamage forms
     * the knockback as `target+0x54 - point` (0x80057EF0..0x80057F28), and
     * only the knockback reads this field (apply_knockback). The feet used to
     * stand here, which left a 286-unit downward shove in every shot that
     * carried a real impact point, and so the shot arm below rebuilt a level
     * point 286 back toward the shooter to cancel it: every creature round
     * pushed the player purely horizontally, where the console's round, which
     * enters the cylinder near its top, pushes back AND down.
     */
    origin[0] = sim->player[sim->cur_player].pos[0];
    origin[1] = q2_sim_origin_y(sim->player[sim->cur_player].pos[1]);
    origin[2] = sim->player[sim->cur_player].pos[2];
    q2_actor_from_player(&sim->combat.self, &sim->combat.inv, origin);
    sync_rules(sim);

    /*
     * The point is the caller's: fire_hit's computed contact at sp+88 for a
     * claw (0x800612E4 stores s0 as T_Damage's fifth argument), the body
     * entry 0x800544EC writes back for a round or a rail (0x80048970,
     * 0x80049338), a projectile's contact. Only the knockback set — mods 3,
     * 13, 15 and 18 (0x80057ED0..0x80057EE8) — reads it as a direction; a
     * claw's kick is fire_hit's own. The attacker's origin is a host fallback
     * for a melee caller that supplies none.
     *
     * Still open: for a CLIENT target the bullet arm adjusts the entry with
     * GenMissileHitPlane (0x80048930..0x80048954) against the previous
     * frame's view plane before DoDamage. That shift is bounded to a few
     * dozen units and is not reproduced here.
     */
    if (mod == Q2_MOD_MELEE && !point && attacker)
        point = attacker->origin;
    out = q2_combat_damage(attacker, &sim->combat.self, damage, mod,
                           point, &sim->combat.rules);

    q2_actor_to_player(&sim->combat.self, &sim->combat.inv);
    player_damage_impulse(sim, sim->cur_player, &sim->combat.self);

    /* Blood only when flesh actually took some of it: armour absorbing the
     * whole hit is the case the HUD's damage flash also distinguishes. */
    if (out.taken > 0)
        fx_at(sim, Q2_FX_BLOOD, point);

    /* Native 0x80057D54 and PlayerThink's 0x8003AE10 feedback block preserve
     * client+0x9C/+0x9E. Do not invent a damage-scaled weapon flinch here.
     * The measured reader and saved amplitudes remain in the view composer;
     * the separate MOD-dependent impact shock is delivered by the hit hook. */

    return out;
}

/*
 * ONE CREATURE ROUND, traced — 0x8004874C as `monster_fire_bullet`
 * (0x80061E9C) and every pellet of `monster_fire_shotgun` (0x80061FCC) reach
 * it: from the muzzle to `start + dir` through the hull, the first entity box
 * the segment enters before the world stops it takes the damage, and the
 * world gets the puff and the breakable test when nothing does.
 *
 * It is the player's own hitscan pointed the other way, and that is the
 * point: the console has ONE bullet path (combat.h), and the creatures'
 * rounds were the one thing in the port that did not go through it. They were
 * a line-of-sight test against the player and a guaranteed hit — so a guard
 * that missed by a mile on the console hit every time here, a dying soldier
 * firing at the floor hit the player standing behind it, and no creature
 * round ever marked a wall.
 *
 * WHAT CAN BE HIT is everything the console's sweep tests: every creature but
 * the shooter (the sweep's ignore argument), and the player. A creature caught
 * in another's line of fire takes the round, as on the console; its reaction
 * is the client's drain, which resolves the damage source back to the
 * creature that fired it.
 *
 * THE PLAYER'S BOX is built here rather than taken from `combat.self`, whose
 * origin holds the FEET (q2_actor_from_player). The console's sweep measures
 * the slab from the entity origin (combat.c, `centre[1] + 286`), so the
 * player is placed at its origin, Q2_EYE_BASE above the feet, with the
 * actor's own 286 x 572 cylinder. The damage then goes through
 * q2_sim_hurt_player with the point it rebuilds for a creature shot, at the
 * height the round actually struck.
 *
 * Returns the creature's index in `sim->combat.targets`, Q2_SIM_HIT_PLAYER,
 * or -1 when the round hit only the world or nothing.
 */
/*
 * A creature round's candidates: every body but the shooter (the sweep's
 * ignore argument) and then the player, at its entity origin. Returns the
 * count and writes the player's slot, -1 when the player is down.
 */
static u32 hitscan_victims(q2_sim *sim, const q2_actor *attacker,
                           q2_actor **list, q2_actor *player,
                           s32 *player_slot)
{
    u32 n = 0, i;

    *player_slot = -1;
    for (i = 0; i < sim->combat.target_count && n < Q2_SIM_CRE_HITSCAN_MAX;
         i++) {
        if (sim->combat.targets[i] && sim->combat.targets[i] != attacker)
            list[n++] = sim->combat.targets[i];
    }

    if (sim->combat.inv.health > 0) {
        const s32 *feet = sim->player[sim->cur_player].pos;

        q2_actor_from_player(&sim->combat.self, &sim->combat.inv, feet);
        *player = sim->combat.self;
        player->origin[1] = q2_sim_origin_y(feet[1]);
        *player_slot = (s32)n;
        list[n++] = player;
    }
    return n;
}

static s32 actor_hitscan(q2_sim *sim, q2_actor *attacker,
                          const s32 start[3], const s32 dir[3], s16 damage,
                          s16 mod, bool clip_world, bool mark_world,
                          s32 impact[3])
{
    q2_actor  *list[Q2_SIM_CRE_HITSCAN_MAX + 1];
    q2_actor   player;
    q2_damage_result dr;
    u32        n, i;
    u8         area = 0;
    s32        frac, idx, player_slot, stop[3], at[3];
    int        k;

    if (!sim || !start || !dir)
        return -1;

    n = hitscan_victims(sim, attacker, list, &player, &player_slot);
    frac = clip_world ? world_fraction_for(sim, start, dir, &area) : 4096;

    /*
     * 0x8004891C (and the mission laser's 0x80048F8C): the native query from
     * the muzzle to where the world stopped the round, which it overwrites
     * with the nearest body's entry. An immune body is in the query, so it
     * stops the round and shields whatever stands behind it; DoDamage is what
     * refuses it. The old cylinder scan skipped such bodies outright.
     */
    for (k = 0; k < 3; k++)
        stop[k] = start[k] + (s32)(((s64)dir[k] * frac) >> Q2_FRAC_12);
    idx = q2_combat_trace_actors(start, stop, NULL, list, n, at);
    if (impact)
        memcpy(impact, at, sizeof(at));

    /* The entry point itself goes to the player's DoDamage, as 0x80048970
     * passes it: the knockback is formed against the entity origin there. */
    if (idx >= 0 && idx == player_slot) {
        q2_sim_hurt_player(sim, attacker, damage, mod, at);
        return Q2_SIM_HIT_PLAYER;
    }

    memset(&dr, 0, sizeof(dr));
    if (idx >= 0) {
        sync_rules(sim);
        dr = q2_combat_damage(attacker, list[idx], damage, mod, at,
                              &sim->combat.rules);
        fx_hitscan_impact(sim, start, dir, at, false, area, idx, &dr, damage);

        for (i = 0; i < sim->combat.target_count; i++)
            if (sim->combat.targets[i] == list[idx])
                return (s32)i;
        return -1;
    }

    if (mark_world)
        fx_hitscan_impact(sim, start, dir, at, frac < 4096, area, -1, NULL,
                          damage);
    return -1;
}

s32 q2_sim_creature_hitscan(q2_sim *sim, q2_actor *attacker,
                            const s32 start[3], const s32 dir[3], s16 damage,
                            s16 mod, bool mark_world, s32 impact[3])
{
    return actor_hitscan(sim, attacker, start, dir, damage, mod,
                          true, mark_world, impact);
}

s32 q2_sim_mission_laser(q2_sim *sim, const s32 start[3], const s32 end[3],
                          s16 damage, s16 mod)
{
    s32 dir[3];
    int k;
    if (!sim || !start || !end) return -1;
    for (k = 0; k < 3; k++) dir[k] = end[k] - start[k];
    /* 0x80048F8C calls the actor-only sweep 0x800544EC directly. The
     * authored segment is already the beam's extent; no world clipping. */
    return actor_hitscan(sim, NULL, start, dir, damage, mod,
                          false, false, NULL);
}

/* 0x8006217C -> 0x8004917C: the player's rail, with the player among the
 * bodies and the creature excluded. Like the player's it hurts the ONE
 * nearest body (0x80049318 / 0x80049334) and draws to the world's end. */
q2_cre_rail_result q2_sim_creature_rail(q2_sim *sim, q2_actor *attacker,
                                       const s32 start[3], const s32 dir[3],
                                       s16 damage)
{
    q2_cre_rail_result result = { 0 };
    q2_actor *list[Q2_SIM_CRE_HITSCAN_MAX + 1], player;
    q2_damage_result dr;
    s32 frac, idx, player_slot, at[3];
    u32 n;
    u8 area;
    int k;

    if (!sim || !start || !dir)
        return result;

    frac = world_fraction_for(sim, start, dir, &area);
    for (k = 0; k < 3; k++)
        result.end[k] = start[k] + (s32)(((s64)dir[k] * frac) / 4096);

    n = hitscan_victims(sim, attacker, list, &player, &player_slot);
    idx = q2_combat_trace_actors(start, result.end, NULL, list, n, at);
    if (idx >= 0 && idx == player_slot) {
        q2_sim_hurt_player(sim, attacker, damage, Q2_MOD_RAIL, at);
        result.player_hit = true;
    } else if (idx >= 0) {
        sync_rules(sim);
        dr = q2_combat_damage(attacker, list[idx], damage, Q2_MOD_RAIL, at,
                              &sim->combat.rules);
        fx_hitscan_impact(sim, start, dir, at, false, area, idx, &dr, damage);
        result.creatures_hit = 1;
    }
    fx_hitscan_impact(sim, start, dir, result.end, frac < 4096, area, -1,
                      NULL, damage);
    return result;
}

q2_cre_rail_result q2_sim_creature_laser(q2_sim *sim, q2_actor *attacker,
                                        const s32 start[3], const s32 aim[3],
                                        s16 damage)
{
    q2_cre_rail_result result = { 0 };
    q2_actor *list[Q2_SIM_CRE_HITSCAN_MAX + 1], player;
    q2_collision *hull;
    q2_trace trace;
    s32 from[3], end[3], delta[3], contact[3], node = -1, hit, player_slot;
    u32 count;
    u8 area;
    int k;
    if (!sim || !start || !aim) return result;
    memcpy(from, start, sizeof(from));
    hull = sim->coll_primary_ready ? &sim->coll_primary :
           sim->coll_ready ? &sim->coll : NULL;
    if (hull && attacker) {
        q2_coll_move(hull, attacker->origin, start, -1, end, &node);
        if (node >= 0) memcpy(from, end, sizeof(from));
    }
    for (k = 0; k < 3; k++) end[k] = from[k] + (s16)aim[k] * 8;
    if (hull) {
        s32 clipped[3];
        q2_coll_move(hull, from, end, -1, clipped, &node);
        if (node >= 0) memcpy(end, clipped, sizeof(end));
    }
    for (k = 0; k < 3; k++) delta[k] = end[k] - from[k];
    count = hitscan_victims(sim, attacker, list, &player, &player_slot);
    /* Retail chooses the body at 0x80049AA8 (the native query, from the
     * hull-clipped start to the hull-clipped end, writing the entry to sp+48)
     * before clipping entity boxes at 0x80049ABC. Keep that order, including
     * its possible hit behind a door. */
    hit = q2_combat_trace_actors(from, end, NULL, list, count, contact);
    q2_sim_trace(sim, from, end, &trace);
    memcpy(result.end, trace.end, sizeof(result.end));
    q2_sim_breakable_shot(sim, from, result.end, damage);
    area = q2_sim_fx_area(sim, 0, from);
    if (hit >= 0) {
        if (hit == player_slot) {
            q2_sim_hurt_player(sim, attacker, damage, Q2_MOD_LASER, contact);
            result.player_hit = true;
        } else {
            q2_damage_result dr;
            sync_rules(sim);
            dr = q2_combat_damage(attacker, list[hit], damage, Q2_MOD_LASER,
                                   contact, &sim->combat.rules);
            fx_hitscan_impact(sim, from, delta, contact, false, area, hit,
                              &dr, damage);
            result.creatures_hit = 1;
        }
    }
    if (sim->fx_ready)
        q2_fx_beam_add_style(&sim->fx, from, result.end, 32, area, 0);
    return result;
}

/*
 * The actor that fired a projectile, from the owner index the launch recorded.
 *
 * The step runs on player 0's tick — projectiles are the world's — so
 * `combat.self` at step time is player 0 whoever fired. Passing that as the
 * attacker credited every bolt in the air to player 0 and gave a
 * player-versus-player kill the wrong killer, or the shooter their own bolt.
 */
/* Where a projectile got to, per the note on q2_combat_scan: "it missed" has
 * several causes and a total cannot tell them apart. */
q2_sim_proj_stats q2_sim_proj_scan;

void q2_sim_set_world_targets(q2_sim *sim, q2_actor **targets, u32 count)
{
    if (!sim)
        return;
    sim->world_targets      = targets;
    sim->world_target_count = count;
}

static q2_actor *attacker_for(q2_sim *sim, s32 owner)
{
    if (owner < 0 || owner >= Q2_SIM_MAX_PLAYERS)
        return &sim->combat.self;
    if (owner == sim->cur_player)
        return &sim->combat.self;
    return &sim->pcombat[owner].self;
}

/* The creature that fired it, or the player `owner` names. */
static q2_actor *proj_attacker(q2_sim *sim, const q2_projectile *p)
{
    return p->shooter ? p->shooter : attacker_for(sim, p->owner);
}

/*
 * The published world list is authoritative when present. In single player,
 * combat.targets normally contains only creatures, so append the player once.
 * Owner exclusion belongs to the query, not this list: a released grenade can
 * return to its shooter. player_damage_begin has placed the actor at its
 * native entity origin. The client publishes at most 96 actors; this caller's
 * buffer also accommodates the 128-actor simulation fixture plus the player.
 */
static q2_actor **proj_targets(q2_sim *sim, q2_actor **buf, u32 cap,
                               u32 *count)
{
    q2_actor **list = sim->world_targets ? sim->world_targets
                                         : sim->combat.targets;
    u32 n = sim->world_targets ? sim->world_target_count
                               : sim->combat.target_count;
    u32 i;

    if (sim->world_targets || !sim->combat.self.takedamage || n >= cap) {
        *count = n;
        return list;
    }
    for (i = 0; i < n; i++) if (list[i] == &sim->combat.self) {
        *count = n;
        return list;
    }
    for (i = 0; i < n; i++)
        buf[i] = list[i];
    buf[n++] = &sim->combat.self;
    *count = n;
    return buf;
}

s32 q2_sim_creature_launch(q2_sim *sim, q2_actor *shooter, q2_fire_kind kind,
                           const s32 start[3], const s32 aim[3], s16 damage,
                           s32 shooter_yaw, s32 speed, u32 effects)
{
    q2_fire_result_v2 r;
    q2_shot *s;
    s32 index;
    s32 toss[3];
    int k;

    if (!sim || !start || !aim ||
        (kind != Q2_FK_BOLT && kind != Q2_FK_ROCKET &&
         kind != Q2_FK_GRENADE && kind != Q2_FK_BFG) ||
        (kind == Q2_FK_GRENADE && speed <= 0))
        return -1;

    memset(&r, 0, sizeof(r));
    r.fired      = true;
    r.kind       = kind;
    r.shot_count = 1;
    s = &r.shot[0];
    memcpy(s->origin, start, sizeof(s->origin));
    s->damage = damage;

    if (kind == Q2_FK_GRENADE) {
        s16 angles[3];
        s32 right[3], up[3], spread;

        /* 0x80061514..0x8006161C: derive up/right from the initial aim,
         * then VectorMA twice. The magic multiply divides by 32767 with
         * truncation, and the up draw precedes the right draw. */
        q2_vectoangles(aim, angles);
        q2_angle_vectors(angles, NULL, right, up);
        spread = ((q2_random_next() - 16384) * 10) / 32767;
        q2_vector_ma(aim, (200 + spread) * 4096 / speed, up, toss);
        spread = ((q2_random_next() - 16384) * 10) / 32767;
        q2_vector_ma(toss, spread * 4096 / speed, right, toss);
        r.projectile_timer = 600; /* 0x80061724, not the player's 900 */
    }

    for (k = 0; k < 3; k++) {
        if (kind == Q2_FK_BOLT) {
            /* 0x80062038..0x80062084: `bgez; addiu 63; sra 6` on each word —
             * the player blaster's own shift, rounding toward zero. */
            s32 a = aim[k];
            s->dir[k] = (a >= 0 ? a : a + 63) >> Q2_AIM_SHIFT_BOLT_BLASTER;
        } else if (kind == Q2_FK_ROCKET) {
            /* 0x80062114..0x80062160: the low halfword times three, back to a
             * halfword, then `sra 17` of it — half again, where the player's
             * launcher doubles (Q2_AIM_SCALE_ROCKET). */
            s->dir[k] = (s16)((u16)aim[k] * 3u) >> 1;
        } else if (kind == Q2_FK_BFG) {
            s->dir[k] = (s16)aim[k];
        } else {
            /* 0x800616A4..0x80061704: signed word * 3 / 2, truncating
             * BEFORE the halfword store (unlike the rocket wrapper). */
            s->dir[k] = (s16)(toss[k] * 3 / 2);
        }
    }
    /* 0x800620A0..0x800620DC: the Flyer selects the hyper pair on three
     * attack frames; the rest use the ordinary blaster pair. */
    r.mod = kind == Q2_FK_BOLT ? ((effects & 0x40) ? Q2_MOD_BOLT_HYPER : Q2_MOD_BOLT)
          : kind == Q2_FK_GRENADE ? Q2_MOD_GRENADE
          : kind == Q2_FK_BFG ? Q2_MOD_ENERGY_BOLT : Q2_MOD_ROCKET;

    q2_sim_proj_scan.launched++;
    index = q2_projectile_launch(&sim->combat.projectiles, &r, -1,
                                 sim->level_time);
    if (index < 0) {
        q2_sim_proj_scan.dropped_full++;
        return -1;
    }
    sim->combat.projectiles.p[index].shooter = shooter;
    if (kind == Q2_FK_ROCKET)
        q2_projectile_aim_rocket(&sim->combat.projectiles.p[index],
                                 shooter_yaw);
    else if (kind == Q2_FK_BFG) {
        q2_model mdl;
        q2_model_move mv;
        s32 bfg = sim->model_bank ? q2_model_bank_find(sim->model_bank, "BFGBlast") : -1;
        q2_projectile_set_attitude(&sim->combat.projectiles.p[index], 0, -shooter_yaw, 0);
        if (bfg >= 0 && q2_model_get(sim->model_bank, (u32)bfg, &mdl) == Q2_OK &&
            q2_model_move_by_name(&mdl, "Spin", &mv))
            q2_projectile_spin_bfg(&sim->combat.projectiles.p[index],
                                    (s32)mv.start * 5, (s32)mv.end * 5);
    } else if (kind == Q2_FK_GRENADE) {
        s32 r0 = q2_random_next();
        s32 r1 = q2_random_next();
        s32 r2 = q2_random_next();
        q2_projectile_set_attitude(&sim->combat.projectiles.p[index],
                                    r0, r1, r2);
    }
    return index;
}

/* The caller's target list normally excludes the shooter so a bolt cannot hit
 * its own muzzle. Radius damage is different: the console deliberately lets a
 * projectile hurt its owner (including the 3.2x self-knockback path). Add that
 * one actor when the published list does not already contain it. */
static void projectile_owner_splash(q2_sim *sim, const q2_projectile *p,
                                    const s32 point[3],
                                    q2_actor **targets, u32 count)
{
    q2_actor *owner;
    q2_actor *one[1];
    u32 i;

    if (!sim || !p || p->owner < 0 || p->owner >= Q2_SIM_MAX_PLAYERS ||
        p->splash_radius <= 0 || p->damage <= 0)
        return;

    owner = attacker_for(sim, p->owner);
    for (i = 0; i < count; i++)
        if (targets && targets[i] == owner)
            return;

    if (!owner->takedamage ||
        (owner == &sim->combat.self && sim->invulnerable))
        return;

    /* The owner is one more candidate of the same 0x80050810 sweep, so it is
     * occluded by the same trace: a rocket that bursts on the far side of a
     * wall does not reach the player who fired it. */
    one[0] = owner;
    q2_combat_radius_damage_traced(owner, NULL, point ? point : p->pos,
                                   p->damage, p->splash_radius, p->mod,
                                   one, 1, &sim->combat.rules,
                                   splash_clear, sim);

    if (owner == &sim->combat.self)
        q2_actor_to_player(owner, &sim->combat.inv);
}

q2_hand_grenade_update q2_sim_hand_grenade_update(
    q2_sim *sim, const s32 attached_pos[3], s32 cook_dt, bool release)
{
    static const s16 release_offset[3] = {
        Q2_HAND_GRENADE_RELEASE_RIGHT,
        Q2_HAND_GRENADE_RELEASE_DOWN,
        Q2_HAND_GRENADE_RELEASE_FORWARD
    };
    s32 index;
    q2_projectile *p;

    if (!sim || !attached_pos)
        return Q2_HAND_GRENADE_NONE;

    index = q2_projectile_hand_held_index(&sim->combat.projectiles,
                                          sim->cur_player);
    if (index < 0)
        return Q2_HAND_GRENADE_NONE;

    q2_projectile_hand_update(&sim->combat.projectiles, sim->cur_player,
                              attached_pos, cook_dt);
    p = &sim->combat.projectiles.p[index];

    /* The fuse is decremented before Grenade3's state dispatch. A held expiry
     * therefore wins over a 411 release reached on the same tick. */
    if (p->expires && sim->level_time >= p->expires) {
        q2_actor **targets = sim->world_targets ? sim->world_targets
                                                : sim->combat.targets;
        u32 count = sim->world_targets ? sim->world_target_count
                                       : sim->combat.target_count;
        s32 where[3];

        player_damage_begin(sim);
        memcpy(where, p->pos, sizeof(where));
        projectile_owner_splash(sim, p, where, targets, count);
        q2_projectile_detonate_traced(&sim->combat.projectiles, (u32)index,
                                      attacker_for(sim, p->owner), targets,
                                      count, &sim->combat.rules,
                                      splash_clear, sim);
        fx_at(sim, Q2_FX_EXPLOSION, where);
        projectile_explosion(sim, p->kind, where);
        player_damage_end(sim);
        return Q2_HAND_GRENADE_EXPIRED;
    }

    if (release) {
        s16 local_dir[3];
        s32 eye[3], origin[3], raw_dir[3];
        static const s32 zero[3] = { 0, 0, 0 };
        s32 charge = q2_projectile_hand_charge(&sim->combat.projectiles,
                                                sim->cur_player);
        q2_player *pl = &sim->player[sim->cur_player];

        q2_sim_eye(sim, eye);
        q2_weapon_muzzle_origin(release_offset, eye,
                                pl->yaw, pl->pitch, pl->roll, origin);

        local_dir[0] = 0;
        local_dir[1] = -Q2_HAND_GRENADE_RELEASE_UP;
        local_dir[2] = (s16)charge;
        q2_weapon_muzzle_origin(local_dir, zero,
                                pl->yaw, pl->pitch, pl->roll, raw_dir);

        if (q2_projectile_hand_release(&sim->combat.projectiles,
                                       sim->cur_player, origin, raw_dir)) {
            /* 0x8004A6FC..0x8004A770: Grenade3 leaves at the thrower's
             * view, pitched down a further 1280 and both negated, and
             * keeps that attitude for the rest of its flight. */
            q2_projectile_set_attitude(&sim->combat.projectiles.p[index],
                                       -(pl->pitch + 1280), -pl->yaw,
                                       pl->roll);
            /* 0x8004A7C0: after reveal, velocity and throw sound. */
            (void)q2_weapon_consume(&sim->combat.inv,
                                    Q2_WID_HAND_GRENADE);
            return Q2_HAND_GRENADE_RELEASED;
        }
    }

    return Q2_HAND_GRENADE_HELD;
}

/* ------------------------------------------------------------------------- */
/*
 * THE CLIENT HALF OF ONE PRESENTATION: which viewport the actor's bursts hide
 * from, and whose quad deadline its shell reads.
 *
 * The console takes both off the entity it is presenting. Every drawer writes
 * the view nibble for ANY entity whose +0x0C client is non-null (0x800590C0
 * `lw v1, 12(s5)` / 0x800590C8 `beq`, and the same at 0x80058B70, 0x80058D38,
 * 0x80058EFC and 0x80059288), from that client's own index; and the shell
 * reads that entity's own client+0xAC (0x8005B7EC, 0x8005B7FC). So the live
 * player is not special. A PARKED deathmatch player in the world list is a
 * client entity too, and it gets its own index and its own deadline; only an
 * actor with no client gets -1 and 0.
 *
 * This used to pass -1 and 0 for everything in the world list and -1 for the
 * live player. Once a mesh arrives that would have drawn a parked player's
 * crackle in that player's own viewport, which the console hides, and would
 * never have shown a quad shell on anyone but the live player.
 *
 * The client is found by POINTER, because the sim owns every player actor it
 * can name: `combat.self` is the live player's, `pcombat[k].self` player k's
 * while it is parked. An actor that claims a client but is none of those has
 * no client block this sim can read, so it keeps -1 and 0 (no nibble, no
 * shell). `has_client` gates both, as the console's client pointer does.
 */
static void present_client_of(const q2_sim *sim, const q2_actor *a,
                              s32 *view_skip, s32 *quad_until)
{
    int k;

    *view_skip  = -1;
    *quad_until = 0;
    if (!a->has_client)
        return;

    if (a == &sim->combat.self) {
        *view_skip  = sim->cur_player;
        *quad_until = sim->combat.inv.quad_until;
        return;
    }
    for (k = 0; k < Q2_SIM_MAX_PLAYERS; k++) {
        if (k != sim->cur_player && a == &sim->pcombat[k].self) {
            *view_skip  = k;
            *quad_until = sim->pcombat[k].inv.quad_until;
            return;
        }
    }
}

/*
 * One actor through 0x8005B880, and the energy light it reports.
 *
 * THE MESH COMES FROM THE OWNER'S HOOK. This file has no posed model for an
 * actor; the client does, so it installs `sim->fx_mesh` and the hook hands back
 * a q2_fx_mesh_src whose vertex callback yields the WORLD-space posed vertex
 * 0x8006CC44 returns. With no hook, or a hook that answers false (the local
 * player's own actor, which has no body in first person), the source is NULL
 * and the drawers take the console's model-less path: 0x8006D6AC returns zero
 * for a null model, every walk finds fewer vertices than it needs and leaves,
 * and the spark leaves at 0x800589E0 before its 45 velocity draws. That last
 * part is exact only for an entity that really has no model. A MODELLED
 * creature the hook cannot pose costs the console those 45 draws per spark
 * firing and the port none (effect.h). The timers and the light reports are
 * exact either way. The hook decides only what `src` is, never whether the
 * call happens, so it cannot make a countdown run twice.
 *
 * The source is built immediately before the call and dies after it, so
 * whatever `ctx` the hook fills in only has to outlive this function, which is
 * the lifetime the hook's contract promises.
 *
 * `frame` is `tick_count`, the port's [0x800B2DE4]: a FRAME COUNTER, +1 a
 * world tick (0x800705C4 `addiu a1, a1, 1`, stored at 0x80070610), and counted
 * before this pass runs, as the console counts it before its thinks (sim.h).
 * It picks which vertices a drawer samples: the crackle starts on vertex
 * `frame & 1`, so the two halves of the mesh alternate, and the spark on
 * `frame & 7`. level_time used to stand in for it. The clock steps by dt, 12 a
 * tick at the nominal rate, so at any even dt its low bit never changed, the
 * crackle sampled the same half of the body forever, and the spark reached two
 * of its eight phases.
 *
 * The energy light, one per lit actor: 0x80058638 raises it on effect[1] >= 3
 * with the colour and radii at 0x800AEAAC and 0x800AEAB0 (combat.h). It follows
 * the report rather than a re-read of the slot, which puts it on the right side
 * of the decrement: 0x800586D0 raises it from the value the tick STARTED with,
 * and the subtraction at 0x80058768 comes afterwards. `q2_actor_energy_lit`
 * still states the same predicate for anyone who wants it without running a
 * presentation pass; combat.c owns it.
 *
 * AND RAISED HERE TOO: `rep.energy_pulse`, the `effect[1] == 2` arm's own light
 * at 0x80058758. It goes through 0x80075D14, a different function from the
 * `>= 3` arm's 0x80075C34 — AddTimedLight, whose fourth argument is a lifetime
 * in frames — so the two arms are not two spellings of the same light. This
 * comment used to end "it is reported so it stops being invisible, and not
 * raised on a guess"; the guess is gone and so is the silence.
 */
static void present_actor(q2_sim *sim, q2_actor *a)
{
    static const u8 energy[3] = { Q2_ENERGY_LIGHT_R, Q2_ENERGY_LIGHT_G,
                                  Q2_ENERGY_LIGHT_B };
    static const u8 creature_target[3] = { Q2_ACTOR_AMBIENT_DEFAULT,
                                          Q2_ACTOR_AMBIENT_DEFAULT,
                                          Q2_ACTOR_AMBIENT_DEFAULT };
    static const u8 player_target[3] = { Q2_PLAYER_AMBIENT_DEFAULT,
                                         Q2_PLAYER_AMBIENT_DEFAULT,
                                         Q2_PLAYER_AMBIENT_DEFAULT };
    q2_fx_present_report rep;
    q2_fx_mesh_src src;
    const q2_fx_mesh_src *sp;
    s32 view_skip, quad_until;

    sp = (sim->fx_mesh && sim->fx_mesh(sim->fx_mesh_user, a, &src))
             ? &src : NULL;
    present_client_of(sim, a, &view_skip, &quad_until);

    /*
     * FIRST IN THE CHAIN, before anything this pass spawns: 0x8005B88C loads 7
     * and 0x8005B894 jumps to the ambient fade 0x80075E14, which walks
     * entity+0x2AC one seventh of the way toward +0x2B0 per call. lighting.c
     * has owned that routine since it was reconstructed and nothing called it,
     * so an actor's own back colour never moved; the creature and body draws
     * fed q2_light_env_build a literal instead.
     *
     * Creature and gameplay-player spawning establish different targets at
     * +0x2B0: respectively 0x40 and 0x20 (combat.h, q2_actor.ambient).
     */
    q2_light_glow_fade(a->ambient, a->has_client ? player_target : creature_target,
                        Q2_ACTOR_AMBIENT_STEPS);

    /*
     * THE AREA EVERY GROUP IN THIS CHAIN IS FILED UNDER. The console passes
     * entity+0x9E — the byte movement caches on the entity from the cell it
     * occupies, `lbu` of the collision record's +32 at 0x80046B08. Nothing in
     * this port caches a cell on an actor (q2_actor has no such field, and
     * q2_actor_from_monster rebuilds the struct every frame), so the byte is
     * resolved from the actor's origin here instead. DEVIATION, stated: the
     * console reads a cached byte and this does one hull descent per actor per
     * world tick. The value is the same cell movement would have recorded; what
     * differs is when it is looked up, and the previous 0 put the whole chain —
     * the damage crackle, the two sparks and the quad shell — into an area the
     * draw always culls.
     */
    q2_fx_actor_present(&sim->fx, &sim->fx_rng, a, sp, sim->tick_count,
                        fx_area_resolve(sim, 0, a->origin),
                        view_skip, sim->level_time, quad_until, &rep);

    if (rep.energy_light)
        q2_ent_light_at(&sim->ent_world.events, a->origin, energy,
                        Q2_ENERGY_LIGHT_INNER, Q2_ENERGY_LIGHT_OUTER);

    /*
     * AND THE OTHER ARM'S LIGHT, which the note above says is "not raised on a
     * guess" — it is not a guess any more. 0x80058758 is AddTimedLight
     * (0x80075D14), not the `>= 3` arm's AddLight, with the same colour word at
     * 0x800AEAAC and the radii packed out of 0x800AEAB0/0x800AEAB2 — the same
     * 800/1300 — and a fourth argument of 4. So the effect[1] == 2 arm parks a
     * slot that re-emits the green at 4/4, 3/4, 2/4 and 1/4 over the four
     * frames after the crackle stops: the tail of the glow, which is exactly
     * why it fires on the tick the bigger effect ends.
     */
    if (rep.energy_pulse)
        q2_ent_light_timed_at(&sim->ent_world.events, a->origin, energy,
                              Q2_ENERGY_LIGHT_INNER, Q2_ENERGY_LIGHT_OUTER,
                              Q2_ENERGY_PULSE_TICKS);

    /*
     * And the ambient write the report has been carrying with no reader:
     * 0x800586E8 copies the four bytes at 0x800AEAAC into entity+0x2AC on the
     * same effect[1] >= 3 arm that raises the light, so the body itself takes
     * the energy colour for that tick and the fade above eases it back out over
     * the following ones. The green is the light's own preset, which is why it
     * is the same triplet.
     */
    if (rep.set_ambient) {
        a->ambient[0] = Q2_ENERGY_LIGHT_R;
        a->ambient[1] = Q2_ENERGY_LIGHT_G;
        a->ambient[2] = Q2_ENERGY_LIGHT_B;
    }
}

/* ------------------------------------------------------------------------- */
/* The blur ribbons projectiles lay — AddBlurPoints from three thinks          */
/* ------------------------------------------------------------------------- */
/* The key a projectile's records carry, standing in for entity+0xD0. */
static u16 proj_blur_owner(u32 slot)
{
    return (u16)(0x100u + slot);
}

/*
 * One think's AddBlurPoints calls (0x8002F238), each over a short vertex list
 * of the projectile's own model, in grey (0x800AE96C) for `life` frames:
 *
 *     Rocket     {15, 21} and {38, 30}, 7   PrimaryRocketActive 0x8004AEB8,
 *                                                                0x8004AEE0
 *     Grenade2   {0, 7}, 6                  PrimaryGrenadeLActive 0x80049EE8
 *     Grenade3   {2, 39}, 6, once thrown    PrimaryGrenadeActive 0x8004A830
 *
 * The lists are 0x800AE998, 0x800AE9A0, 0x800AE964 and 0x800AE988, each ended
 * by -1. Each think samples at entity+0xA4, which it refreshes from +0x54 just
 * before the call and which therefore still holds the position from before
 * this frame's move. A level whose bank lacks the model has nothing to sample:
 * the console reports "No model for blur" and adds nothing.
 */
static void proj_blur(q2_sim *sim, u32 slot, const q2_projectile *p,
                      const s32 at[3])
{
    static const s16 rocket_a[2] = { 15, 21 }, rocket_b[2] = { 38, 30 };
    static const s16 grenade[2]  = { 0, 7 },   hand[2]     = { 2, 39 };
    static const u8  grey[3] = { Q2_FX_BLUR_GREY, Q2_FX_BLUR_GREY,
                                 Q2_FX_BLUR_GREY };
    const s16 *lists[2] = { NULL, NULL };
    const char *name;
    q2_model mdl;
    q2_model_instance inst;
    q2_model_pose pose[Q2_MODEL_REST_POSE_MAX];
    s16 spin[3][3];
    s16 life;
    s32 index;
    u8 area;
    int l;

    if (!sim->fx_ready || !sim->model_bank)
        return;

    switch (p->kind) {
    case Q2_PROJ_ROCKET:
        name = "Rocket";   lists[0] = rocket_a; lists[1] = rocket_b; life = 7;
        break;
    case Q2_PROJ_GRENADE:
        name = "Grenade2"; lists[0] = grenade;  life = 6;
        break;
    case Q2_PROJ_HAND_GRENADE:
        name = "Grenade3"; lists[0] = hand;     life = 6;
        break;
    default:
        return;
    }

    index = q2_model_bank_find(sim->model_bank, name);
    if (index < 0 || q2_model_get(sim->model_bank, (u32)index, &mdl) != Q2_OK)
        return;

    q2_model_instance_init(&inst);
    inst.model = &mdl;
    memcpy(inst.origin, at, sizeof(inst.origin));
    q2_rotation_euler(spin, p->angles[0], p->angles[1], p->angles[2]);
    inst.rot  = (const s16 (*)[3])spin;
    inst.pose = q2_model_pose_rest(&mdl, pose, Q2_MODEL_REST_POSE_MAX)
              ? pose : NULL;

    area = fx_area_resolve(sim, 0, at);
    for (l = 0; l < 2 && lists[l]; l++) {
        s32 pts[2][3];
        int v;

        for (v = 0; v < 2; v++)
            if (!q2_model_world_vertex(&inst, lists[l][v], pts[v]))
                return;
        q2_fx_blur_add(&sim->fx, proj_blur_owner(slot), (u16)lists[l][0],
                       (const s32 (*)[3])pts, 2, grey, life, area);
    }
}

/* Grenade2/3 use the shared toss mover: entity boxes precede PrimaryColl;
 * any geometry contact suppresses the subsequent actor sweep. */
static bool grenade_geometry(q2_sim *sim, q2_projectile *p,
                             const q2_proj_step *step)
{
    s32 desired[3], clipped[3];
    s16 normal[3] = {0, 0, 0};
    q2_move_seg_hit hit;
    bool contact = false;
    int k;
    memcpy(desired, step->to, sizeof(desired));
    if (q2_move_clip_segment(&sim->move_world, step->from, desired, NULL, &hit)) {
        contact = true;
        /* The high half of the native toss argument 0x00030900 is the
         * shootable-box damage, independent of the grenade's blast damage. */
        q2_sim_breakable_shot(sim, step->from, hit.pos, 3);
        for (k = 0; k < 3; k++) {
            normal[k] = hit.normal[k];
            desired[k] = hit.pos[k] - (step->to[k] - step->from[k]);
        }
        q2_sim_proj_scan.stopped_on_entity++;
    }
    memcpy(clipped, desired, sizeof(clipped));
    if (sim->coll_primary_ready || sim->coll_ready) {
        q2_collision *hull = sim->coll_primary_ready ? &sim->coll_primary : &sim->coll;
        s32 node = p->node;
        if (!q2_coll_move(hull, step->from, desired, node, clipped, &node)) {
            q2_coll_plane plane;
            contact = true;
            if (hull->hit_plane_index >= 0 &&
                q2_collision_get_plane(hull, (u32)hull->hit_plane_index, &plane)) {
                normal[0] = plane.nx; normal[1] = plane.ny; normal[2] = plane.nz;
            }
        }
        p->node = node;
    }
    if (contact) {
        q2_projectile_rebound(p, desired, clipped, normal);
        q2_model_ent_sound_push(&sim->grenade_bounce_sounds, p->pos);
    }
    return contact;
}

static void grenade_fuse(q2_sim *sim, u32 index)
{
    q2_projectile *p = &sim->combat.projectiles.p[index];
    q2_proj_kind kind = p->kind;
    s32 where[3];
    q2_actor *candidates[Q2_SIM_CRE_HITSCAN_MAX + 1];
    u32 count;
    q2_actor **targets = proj_targets(sim, candidates,
                                      (u32)Q2PSX_ARRAY_COUNT(candidates), &count);
    memcpy(where, p->pos, sizeof(where));
    q2_sim_proj_scan.expired++;
    projectile_owner_splash(sim, p, where, targets, count);
    q2_projectile_detonate_traced(&sim->combat.projectiles, index,
                                  proj_attacker(sim, p), targets, count,
                                  &sim->combat.rules, splash_clear, sim);
    fx_at(sim, fx_for_projectile(kind), where);
    projectile_explosion(sim, kind, where);
}

/* ------------------------------------------------------------------------- */
/* 0x80047C6C: lifetime/contact decisions precede flight effects; the cached
 * hull check follows them. Full PAL/US sequences are in the bolt fixture. */
static void bolt_flight_effects(q2_sim *sim, const q2_projectile *p)
{
    static const u8 glow[3] = {Q2_PROJ_LIGHT_R, Q2_PROJ_LIGHT_G, Q2_PROJ_LIGHT_B};
    if (p->flags & Q2_PROJ_FLAG_LIGHT)
        q2_ent_light_at(&sim->ent_world.events, p->pos, glow,
                       Q2_PROJ_LIGHT_INNER, Q2_PROJ_LIGHT_OUTER);
    if (p->flags & Q2_PROJ_FLAG_LIGHT_BIG)
        q2_ent_light_at(&sim->ent_world.events, p->pos, glow, 800, 1600);
    if ((p->flags & Q2_PROJ_FLAG_TRAIL) && sim->fx_ready) {
        s16 disp[3];
        int k;
        for (k = 0; k < 3; k++)
            disp[k] = (s16)((u32)q2_projectile_raw_velocity(p, k) * (u32)sim->cur_dt);
        q2_fx_bolt_trail(&sim->fx, &sim->combat.rng, p->pos, disp,
                         fx_area_resolve(sim, 0, p->pos));
    }
}

static void bolt_impact_effect(q2_sim *sim, q2_fx_preset_id id, const s32 at[3])
{
    /* The sweep's trail and impact draws are consecutive BIOS rand calls.
     * Keep them on the same saved stream, including hull-hit frames. */
    if (sim->fx_ready)
        q2_fx_spawn(&sim->fx, &sim->combat.rng, id, at,
                    fx_area_resolve(sim, 0, at));
}

static void bolt_contact_tick(q2_sim *sim, u32 index, const q2_proj_step *step,
                              q2_actor *attacker, q2_actor **targets, u32 count,
                              s32 hit_index, const s32 contact[3])
{
    q2_projectile *p = &sim->combat.projectiles.p[index];
    s32 end[3];
    bool consumed = false;
    q2_move_seg_hit hit;
    memcpy(end, hit_index >= 0 ? contact : step->to, sizeof(end));

    /* 0x80047DF4 tests the actor-limited segment against solid entities before
     * damage. A type-2 contact with bit 8 set branches to sparks while the
     * record still holds its OLD position; other kinds only clip the endpoint. */
    if (q2_move_clip_segment(&sim->move_world, step->from, end, NULL, &hit)) {
        memcpy(end, hit.pos, sizeof(end));
        hit_index = -1;
        q2_sim_proj_scan.stopped_on_entity++;
        /* q2_move_clip_segment considers type-2 entity boxes only. */
        if (sim->breakable_count)
            q2_sim_breakable_shot(sim, step->from, end, (s16)p->damage);
        consumed = true;
        if (p->flags & Q2_PROJ_FLAG_IMPACT) {
            bolt_impact_effect(sim, Q2_FX_EXPLOSION, p->pos);
            q2_projectile_expire(&sim->combat.projectiles, index);
            return;
        }
    }
    if (hit_index >= 0) {
        q2_actor *victim = targets[hit_index];
        q2_actor *direct = sim->invulnerable && victim == &sim->combat.self
                            ? NULL : victim;
        q2_sim_proj_scan.hit++;
        q2_projectile_impact_traced(&sim->combat.projectiles, index, end,
                                    attacker, direct, targets, count,
                                    &sim->combat.rules, splash_clear, sim);
        consumed = true;
        if (p->flags & Q2_PROJ_FLAG_IMPACT) {
            /* The blood call at 0x80048638 does not test the damage result.
             * There is no additional world burst on an actor contact. */
            bolt_impact_effect(sim, Q2_FX_BLOOD, end);
            return;
        }
    }

    /* Flag-8-clear contacts fall through even though their lifetime was
     * cleared. Direct assignment preserves that native edge case. */
    memcpy(p->pos, end, sizeof(p->pos));
    bolt_flight_effects(sim, p);
    if (sim->coll_primary_ready || sim->coll_ready) {
        q2_collision *hull = sim->coll_primary_ready ? &sim->coll_primary : &sim->coll;
        s32 clipped[3], node = p->node;
        bool complete = q2_coll_move(hull, step->from, end, node, clipped, &node);
        p->node = node;
        if (!complete) {
            memcpy(p->pos, clipped, sizeof(p->pos));
            consumed = true;
            if (p->flags & Q2_PROJ_FLAG_IMPACT)
                bolt_impact_effect(sim, Q2_FX_EXPLOSION, p->pos);
        }
    }
    if (consumed) q2_projectile_expire(&sim->combat.projectiles, index);
}

void q2_sim_combat_tick(q2_sim *sim)
{
    u32 i;

    if (!sim)
        return;

    /*
     * The projectiles in flight are the WORLD's, not a player's — one list,
     * shared, exactly like the entity sweep and the effects. So they step once
     * a frame, not once a player: with four players a bolt was advancing four
     * times per frame and a rocket crossed an arena at four times its speed.
     *
     * This is the same class of bug the world-half gate in `q2_sim_tick`
     * exists for, and it was missed because it lives in another file.
     */
    if (sim->cur_player != 0)
        return;

    player_damage_begin(sim);

    /*
     * This tick contains damage sites of its own — the detonations below and,
     * now, the BFG's beam pass — and the console reads the rule globals AT each
     * site rather than from a copy. `sim->combat.rules` was last refreshed by
     * whichever fire or hurt ran most recently, which for a projectile that has
     * been in the air for a second is a long time ago, so refresh it here for
     * the same reason q2_sim_fire and q2_sim_hurt_player do.
     */
    sync_rules(sim);

    /*
     * THE PER-ACTOR PRESENTATION PASS, 0x8005B880, once per actor per world
     * tick: the live player first, then every other actor in the world.
     *
     * THE COUNTDOWN IS NO LONGER THIS LOOP'S, AND THE GATES WERE WRONG.
     *
     * What used to sit here was `for every slot: if (slot) slot--`, over all
     * six bytes, for self and every world target, once per world tick. Its
     * comment said "nothing did", which was true and was the right thing to
     * notice — the first energy hit an actor took left `effect[1]` at full
     * strength for the rest of the level and parked a 1300-unit pure-green
     * light on them, which is the fault it was written to fix.
     *
     * But it decremented the wrong things on the wrong clock:
     *
     *   - effect[1] is run down INSIDE 0x80058638 (0x80058768), by the
     *     caller's dt, and only when it was non-zero on entry.
     *   - effect[0] and effect[2] are run down by (health > 0) (0x8005B8AC
     *     feeds `slt s1, zero, s1` to those two tickers alone), so on a
     *     corpse they hold and keep emitting. What ends that on the console
     *     is the corpse think's gate 0x8005B2A8, which swaps such a body to a
     *     dissolve handler that frees it a few frames later (effect.h). That
     *     gate is the corpse owner's to model, not this pass's.
     *   - effect[4] and effect[5] are run down by a literal 1 whatever the
     *     health (0x8005B8D0, 0x8005B8E4).
     *   - effect[3]: 0x8005B880 never touches +0x2F3, and `q2psx-inspect
     *     access 0x2F3` finds no immediate-offset load or store of it in the
     *     main executable (it does not scan the relocated modules), so
     *     decrementing it was inventing a rule.
     *
     * All five countdowns belong to the presentation pass, which is now
     * q2_fx_actor_present (effect.h). Calling it from here keeps the fix and
     * takes the rules from the executable.
     *
     * WHO IS PRESENTED. The console presents every entity every frame: the
     * creature think calls 0x8005B880 at 0x8007EC0C for every monster, alive
     * or dead (0x8007EBEC..0x8007EC08 only sets a flag when health <= 0), and
     * the player think calls it at 0x8003B004. Here that is `combat.self`
     * and then the world's list — `world_targets`, or `combat.targets` when
     * nobody published one, the same fallback every projectile path below
     * takes. This pass used to walk `world_targets` alone, so a caller that
     * registered its creatures only through q2_sim_set_targets had none of
     * them presented: the effect[1] = 3 an energy bolt armed on a monster was
     * never counted down by this pass and never raised its light. The client
     * now also publishes its single-player creature list as the world list
     * (main.c, after its q2_sim_set_targets); for this pass that call is
     * redundant, since both lists are the same one. The pass no longer
     * depends on what the caller publishes.
     *
     * ONCE PER ACTOR. The live player was presented first, and it can appear in
     * the list under two names, both skipped: `&combat.self`, which a
     * deathmatch world list names for the player who was live when it was
     * built (main.c client_targets_for); and `pcombat[cur_player].self`, that
     * player's PARKED slot. It is stale while the player is live, and
     * combat_swap_to overwrites it on the next swap. Presenting either would
     * run every countdown in effect[] twice a tick. A list is taken to name
     * each other actor once, as both of the client's lists do.
     */
    {
        q2_actor **list  = sim->world_targets ? sim->world_targets
                                              : sim->combat.targets;
        u32        count = sim->world_targets ? sim->world_target_count
                                              : sim->combat.target_count;
        const q2_actor *parked_self = &sim->pcombat[sim->cur_player].self;
        u32 t;

        present_actor(sim, &sim->combat.self);

        for (t = 0; t < count; t++) {
            q2_actor *a = list ? list[t] : NULL;

            if (!a || a == &sim->combat.self || a == parked_self)
                continue;
            present_actor(sim, a);
        }
    }

    for (i = 0; i < Q2_PROJ_MAX; i++) {
        q2_projectile *p = &sim->combat.projectiles.p[i];
        q2_proj_step step;
        q2_actor **hit_list;
        q2_actor *attacker, *ignore;
        q2_actor  *cand[Q2_SIM_CRE_HITSCAN_MAX + 1];
        u32 hit_count;
        s32 hit_index;
        s32 dir[3], contact[3];
        bool held, grenade;
        int k;

        if (!p->in_use)
            continue;

        q2_sim_proj_scan.stepped++;
        held = (p->kind == Q2_PROJ_HAND_GRENADE &&
                p->node == Q2_PROJ_NODE_HELD);
        grenade = p->kind == Q2_PROJ_GRENADE || p->kind == Q2_PROJ_HAND_GRENADE;
        attacker = proj_attacker(sim, p);
        ignore = q2_projectile_ignore_owner(p, attacker) ? attacker : NULL;

        /* Entity projectiles keep their thinker light. Bolts emit theirs after
         * lifetime/contact decisions and the endpoint commit below. */
        if (!held && p->kind != Q2_PROJ_BOLT) {
            static const u8 glow[3]     = { Q2_PROJ_LIGHT_R, Q2_PROJ_LIGHT_G,
                                            Q2_PROJ_LIGHT_B };
            static const u8 bfg_glow[3] = { Q2_PROJ_BFG_LIGHT_R,
                                            Q2_PROJ_BFG_LIGHT_G,
                                            Q2_PROJ_BFG_LIGHT_B };
            bool bfg = (p->kind == Q2_PROJ_BFG);

            q2_ent_light_at(&sim->ent_world.events, p->pos,
                            bfg ? bfg_glow : glow,
                            bfg ? Q2_PROJ_BFG_LIGHT_INNER
                                : Q2_PROJ_LIGHT_INNER,
                            bfg ? Q2_PROJ_BFG_LIGHT_OUTER
                                : Q2_PROJ_LIGHT_OUTER);
        }

        /* The grenade thinker lays its ribbon before toss changes attitude. */
        if (grenade && !held)
            proj_blur(sim, i, p, p->pos);
        q2_projectile_step(&sim->combat.projectiles, i, sim->gravity,
                           sim->cur_dt, sim->level_time, &step);

        /* The blur ribbon its think lays from its own model's vertices, at
         * where it stood before this move (proj_blur). */
        if (!held && !grenade)
            proj_blur(sim, i, p, step.from);

        /* State 1 has no mover, collision body, light or visible model. The
         * owner update after the view-model step attaches it and resolves an
         * elapsed fuse at the current hand position. */
        if (held)
            continue;

        if (grenade && ((p->flags & Q2_PROJ_TOSS_STOPPED) ||
                        grenade_geometry(sim, p, &step))) {
            if (step.expired) grenade_fuse(sim, i);
            continue;
        }

        if (step.expired && !grenade) {
            q2_sim_proj_scan.expired++;
            /* Rocket and BFGBlast use +0xF4 only as a safety lifetime:
             * 0x8004ADB0 / 0x8004B8E4 free them without damage or an effect.
             * The bolt lifetime is quiet for the same reason. */
            q2_projectile_expire(&sim->combat.projectiles, i);
            continue;
        }

        /*
         * The BFG's beams — the game's weapon trail, and MOST OF ITS DAMAGE.
         *
         * 0x8004BD04 calls the beam maintainer every tick while the ball flies,
         * and it holds a green beam on every target it can see, refreshing each
         * one rather than adding a second (effect.h). The beams outlive the
         * ball's passage by their own timer, which is what makes the BFG leave
         * a lattice behind it rather than a single line.
         *
         * THE SAME PASS HURTS WHAT IT BEAMS, and that half was missing: this
         * arm was one q2_fx_beam_timed call, so the lattice was decoration and
         * the weapon was worth its contact hit plus one 1300-radius blast. On
         * the disc a candidate that clears the filters takes 10 points (5 in
         * deathmatch) every tenth of a second while the ball is in the air —
         * 0x80049DE4..0x80049E38, `a3 = 1` so mod 1, `sw zero, 16(sp)` so no
         * point and therefore no knockback.
         *
         * The beam and the damage are SEPARATELY gated: 0x80049DBC `beq v1,
         * zero` falls through to the damage when the 12-slot beam table is
         * full, so the damage cannot live under `fx_ready` the way the beam
         * does.
         *
         * The port's visibility test is the same segment sweep the projectile
         * itself uses, because it has no separate line-of-sight query; the
         * original calls 0x80051874 at 0x80049CE0, and failing it branches past
         * BOTH halves. Called out as the one substitution — and until now the
         * test was only described here, never performed, so beams were held
         * through walls.
         */
        if (p->kind == Q2_PROJ_BFG) {
            q2_actor **list  = sim->world_targets ? sim->world_targets
                                                  : sim->combat.targets;
            u32        count = sim->world_targets ? sim->world_target_count
                                                  : sim->combat.target_count;
            const q2_actor *parked_self = &sim->pcombat[sim->cur_player].self;
            q2_actor *owner = attacker_for(sim, p->owner);
            /* 0x80049DE8 `lw 0x800AEBCC` then 0x80049E10 `addiu a2, zero, 5`
             * against 0x80049E28's 10 — read from the port's own stand-in for
             * that global rather than from the rules copy, as the console reads
             * the word itself at the site. */
            s16  beam_damage = sim->multiplayer ? 5 : 10;
            bool hurt;
            u32  t;

            /*
             * 0x8004BCF0..0x8004BD08: the frame delta at 0x800B2DB4 is added
             * into the ball's halfword at +0x4C and stored back in the
             * maintainer's own delay slot, so the pass below already sees this
             * tick's dt in it.
             */
            p->beam_time = (u16)(p->beam_time + (u16)sim->cur_dt);

            /* 0x80049E00 / 0x80049E1C `slti v0, v0, 30` on the SIGNED halfword:
             * below 30 the branch skips the damage but not the beam. */
            hurt = (s16)p->beam_time >= 30;

            for (t = 0; t < count; t++) {
                q2_actor *a = list ? list[t] : NULL;
                s64 dx, dy, dz;

                /* 0x80049C54 `beq a1, s3, 0x80049E3C`: the ball never beams or
                 * hurts its own owner. The world list names the live player in
                 * deathmatch, so without this the shooter would scythe himself
                 * for the length of his own shot. */
                if (!a || a == owner)
                    continue;

                /*
                 * The live player can appear in a world list under two names,
                 * and the parked `pcombat[cur_player].self` is the stale one:
                 * player_damage_end maps the current player to `combat.self`,
                 * so damage landed on the parked copy is never written back,
                 * and damage landed on both is applied twice. The presentation
                 * pass above skips the same pair for the same reason.
                 */
                if (a == parked_self)
                    continue;

                /*
                 * 0x80049C5C..0x80049CB4: squared origin distance against
                 * `lui v0, 0x90` = 0x900000 = 3072^2. The console ALSO box-
                 * tests the candidate's own bounds first (0x80049C3C, the
                 * 0x800552D8 overlap against entity+0x78) against a +/-3072 box
                 * around the ball; port actors carry no bounds, so only the
                 * sphere is reproduced. It is the tighter of the two for a
                 * point, which is what an actor here is. In s64 because the sum
                 * of three squared world spans does not fit an s32.
                 */
                dx = (s64)p->pos[0] - a->origin[0];
                dy = (s64)p->pos[1] - a->origin[1];
                dz = (s64)p->pos[2] - a->origin[2];
                if (dx * dx + dy * dy + dz * dz > (s64)3072 * 3072)
                    continue;

                /*
                 * THE ONE PLACE THE CONSOLE READS `takedamage` OUTSIDE
                 * T_Damage, so this filter can be the original's rather than a
                 * stand-in: the beam maintainer at 0x80049B9C does
                 * `lw v0, 748(a1)` / `lw v0, 28(v0)` / `and 0xC0000000` /
                 * `beq v0, zero` at 0x80049CBC..0x80049CD8. It asks whether the
                 * thing can be hurt, not whether it is alive.
                 */
                if (!a->takedamage)
                    continue;

                /* 0x80049CE0 `jal 0x80051874`, the visibility test. */
                if (!splash_clear(sim, p->pos, a->origin))
                    continue;

                /*
                 * The area comes from the BALL, which is what 0x80048D24
                 * resolves on the console: the point-clip helper 0x8004E920
                 * runs the owner entity's position through PrimaryColl and
                 * hands the cell's area record to the beam queue as its fourth
                 * argument. Resolved from the ball's position here, on each
                 * refresh, rather than per submit — see q2_fx_timed_beam.area.
                 */
                if (sim->fx_ready) {
                    /*
                     * The far end is the target's origin raised to the middle
                     * of its hull: 0x80048D4C..0x80048D74 adds
                     * `(572 - height) / 2`, truncated toward zero, to +0x58.
                     * Ending the beam at the bare origin put it 14 units off
                     * on a default 600-high creature and further on the tall
                     * ones.
                     */
                    s32 to[3];
                    to[0] = a->origin[0];
                    to[1] = a->origin[1] + (572 - (s32)a->height) / 2;
                    to[2] = a->origin[2];
                    q2_fx_beam_timed(&sim->fx, (s32)i, (s32)t,
                                     p->pos, to,
                                     Q2_FX_TIMED_BEAM_RADIUS,
                                     Q2_FX_TIMED_BEAM_STYLE,
                                     Q2_FX_TIMED_BEAM_LIFE,
                                     fx_area_resolve(sim, 0, p->pos));
                }

                /* 0x80049E34, the damage the beam does when it lands. */
                if (hurt)
                    q2_combat_damage(owner, a, beam_damage,
                                     Q2_MOD_ENERGY_BOLT, NULL,
                                     &sim->combat.rules);
            }

            /*
             * 0x8004BD0C..0x8004BD3C. Two things the obvious reading gets
             * wrong: the drain is a LOOP (0x8004BD3C branches back to
             * 0x8004BD20), and its threshold is 31, ONE MORE than the damage
             * gate's 30. So an accumulator that lands exactly on 30 is not
             * drained and fires again on the very next tick before resetting,
             * which makes the beams hurt somewhat oftener than once per 30
             * ticks — at the PAL frame delta of 12, three ticks in every five.
             * That is the disc's arithmetic, not a rounding choice here.
             */
            while ((s16)p->beam_time >= 31)
                p->beam_time = (u16)(p->beam_time - 30);
        }

        /* Grenades already tested geometry; the other projectile thinkers
         * retain their existing actor-first sweep. */
        for (k = 0; k < 3; k++)
            dir[k] = step.to[k] - step.from[k];

        /* The player stops a creature's shot even in god mode; it is the
         * damage that god mode refuses (q2_sim_hurt_player). */
        hit_list = proj_targets(sim, cand, (u32)Q2PSX_ARRAY_COUNT(cand),
                                &hit_count);

        hit_index = q2_combat_trace_actors(step.from, step.to, ignore,
                                           hit_list, hit_count, contact);

        /*
         * How close it came, whether or not it counted. Measured against the
         * segment's LINE, so "near but past the end" separates a bolt that is
         * badly aimed from one that is aimed correctly and simply has not
         * arrived yet — which a short per-tick step makes the common case.
         */
        {
            u32 hi;
            s64 dl = (s64)dir[0] * dir[0] + (s64)dir[1] * dir[1] +
                     (s64)dir[2] * dir[2];

            /* The square is enough to compare; the length is only printed. */
            q2_sim_proj_scan.seg_len = (s32)dl;

            for (hi = 0; hi < hit_count; hi++) {
                q2_actor *t = hit_list[hi];
                s64 along = 0, d2;
                s64 reach;

                /* Match collision membership, including immune bodies. */
                if (!t || t == ignore)
                    continue;

                d2 = q2_combat_ray_dist_sq(step.from, dir, t->origin, &along);
                if (q2_sim_proj_scan.closest_sq == 0 ||
                    d2 < q2_sim_proj_scan.closest_sq) {
                    q2_sim_proj_scan.closest_sq        = d2;
                    q2_sim_proj_scan.closest_origin[0] = t->origin[0];
                    q2_sim_proj_scan.closest_origin[1] = t->origin[1];
                    q2_sim_proj_scan.closest_origin[2] = t->origin[2];
                    q2_sim_proj_scan.closest_from[0]   = step.from[0];
                    q2_sim_proj_scan.closest_from[1]   = step.from[1];
                    q2_sim_proj_scan.closest_from[2]   = step.from[2];
                    q2_sim_proj_scan.closest_owner     = p->owner;
                }

                reach = (s64)Q2_HITSCAN_RADIUS + t->radius;
                if (d2 <= reach * reach) {
                    q2_sim_proj_scan.near_miss++;
                    if (along > 4096)
                        q2_sim_proj_scan.past_end++;
                }
            }
        }

        if (p->kind == Q2_PROJ_BOLT) {
            bolt_contact_tick(sim, i, &step, attacker, hit_list, hit_count,
                              hit_index, contact);
            continue;
        }

        if (hit_index >= 0) {
            q2_actor *victim = hit_list[hit_index];
            q2_actor *direct = sim->invulnerable && victim == &sim->combat.self
                                ? NULL : victim;
            s32 health_before = victim->health;

            q2_sim_proj_scan.hit++;
            q2_fx_preset_id fx = fx_for_projectile(p->kind);

            projectile_owner_splash(sim, p, contact, hit_list, hit_count);
            q2_projectile_impact_traced(&sim->combat.projectiles, i, contact,
                                        attacker, direct, hit_list, hit_count,
                                        &sim->combat.rules,
                                        splash_clear, sim);

            /*
             * Two bursts, not three. The projectile's own and the victim's
             * blood are both real sites; the third used to be 0x800596B0 on the
             * killing blow, and that address is the ITEM MATERIALISE burst, not
             * a gib — see the note at fx_hitscan_impact and effect.h. What the
             * console raises when a body comes apart is the mesh blood spray
             * (q2_fx_gib_spray) at the head of ThrowGibs, behind 0x8007CEB4,
             * and that needs a posed model this file does not carry.
             */
            fx_at(sim, fx, contact);
            projectile_explosion(sim, p->kind, contact);
            if (victim->health < health_before)
                fx_at(sim, Q2_FX_BLOOD, contact);
            continue;
        }

        if (!grenade && (sim->coll_ready || sim->coll_primary_ready)) {
            s32 end[3], node = p->node;
            bool complete;
            q2_collision *phull = sim->coll_primary_ready
                                      ? &sim->coll_primary : &sim->coll;

            /*
             * Traced from the PROJECTILE's own cell, not the player's. A rocket
             * that has crossed the map is nowhere near the shooter, and asking
             * the hull to clip a segment starting in the shooter's cell answers
             * a different question — one whose answer is usually "no obstacle",
             * which is how a projectile ends up flying through walls.
             *
             * Through PrimaryColl, for the reason q2_sim_trace now states: a
             * bolt is a point and the eroded hull stopped it 286 units short of
             * every surface, leaving rockets to burst in mid-air short of the
             * wall and grenades to stop above the floor. `p->node` is cached
             * against this hull throughout, so the hint stays meaningful.
             */
            complete = q2_coll_move(phull, step.from, step.to, node,
                                    end, &node);
            p->node = node;

            /*
             * AND THE ENTITY BOXES. Doors, lifts and intact panes are not hull,
             * so the walk above otherwise flies a projectile through all of
             * them. Clipped from the step's start to wherever the hull left it,
             * so the nearer pass wins; `complete` goes false because a bolt
             * that met any solid entity has met something.
             */
            if (sim->move_world.count) {
                q2_move_seg_hit mh;
                const s32 *stop = complete ? step.to : end;

                if (q2_move_clip_segment(&sim->move_world, step.from, stop,
                                         NULL, &mh)) {
                    end[0]   = mh.pos[0];
                    end[1]   = mh.pos[1];
                    end[2]   = mh.pos[2];
                    complete = false;
                    q2_sim_proj_scan.stopped_on_entity++;
                }
            }

            /*
             * A pane in the way, tested over the step the projectile just took.
             * The console routes to a breakable from five weapon call sites and
             * a bolt's is one of them (0x80049B18), so this is not confined to
             * hitscan — and a blaster is the weapon a player has in hand when
             * they first meet a window.
             *
             * Tested against the step rather than the impact point, because a
             * pane is thinner than a step: at the projectile's speed the shot
             * would otherwise pass through it between one tick and the next.
             */
            if (sim->breakable_count) {
                u32 pieces = q2_sim_breakable_shot(sim, step.from,
                                                   complete ? step.to : end,
                                                   (s16)p->damage);
                if (pieces)
                    fx_at(sim, fx_for_projectile(p->kind), step.to);
            }

            if (!complete) {
                bool consumed;
                q2_fx_preset_id fx = fx_for_projectile(p->kind);
                u32 count;
                /* A creature's rocket on the wall beside the player reaches
                 * the player too, unless god mode is holding the damage off. */
                q2_actor **targets = proj_targets(sim, cand,
                                                  (u32)Q2PSX_ARRAY_COUNT(cand),
                                                  &count);

                projectile_owner_splash(sim, p, end, targets, count);
                consumed = q2_projectile_impact_traced(
                    &sim->combat.projectiles, i, end,
                    proj_attacker(sim, p), NULL, targets, count,
                    &sim->combat.rules, splash_clear, sim);
                if (consumed) {
                    fx_at(sim, fx, end);
                    projectile_explosion(sim, p->kind, end);
                }
                continue;
            }
        }

        q2_projectile_commit(&sim->combat.projectiles, i, step.to);
        /* Native Grenade2/3 decrement the fuse after movement and contact. */
        if (grenade && step.expired) grenade_fuse(sim, i);
    }

    /*
     * A trail whose projectile is gone goes with it, this frame: ProcessBlurs
     * frees a record whose entity's +0xF4 has reached zero or whose +0xD0 is
     * no longer the key's (0x8002F67C..0x8002F6A0), so a rocket's ribbon
     * vanishes the frame it bursts rather than fading.
     */
    if (sim->fx_ready)
        for (i = 0; i < Q2_PROJ_MAX; i++)
            if (!sim->combat.projectiles.p[i].in_use)
                q2_fx_blur_release(&sim->fx, proj_blur_owner(i));

    /*
     * Debris, through PRIMARY collision.
     *
     * 0x80046DA0 installs 0x800C8E90 as the hull and the entity's +0xA0 as its
     * cell, where the player's path (0x80046DDC) installs SecondaryCol and
     * +0xA2. Tracing shards against the player's eroded hull would leave every
     * one of them floating 286 units off the floor.
     */
    for (i = 0; i < Q2_FX_DEBRIS_MAX; i++) {
        q2_fx_debris *d = &sim->fx.debris[i];
        q2_fx_debris_step step;

        if (!d->in_use)
            continue;

        q2_fx_debris_step_one(&sim->fx, i, sim->gravity, &step);

        if (step.expired) {
            d->in_use = false;
            continue;
        }

        if (sim->coll_primary_ready) {
            s32 end[3], node = d->node;

            if (!q2_coll_move(&sim->coll_primary, step.from, step.to, node,
                              end, &node)) {
                d->node = node;
                q2_fx_debris_impact(&sim->fx, i, end);
                continue;
            }
            d->node = node;
        }

        q2_fx_debris_commit(&sim->fx, i, step.to);
    }
    player_damage_end(sim);
}

/* ------------------------------------------------------------------------- */
u32 q2_sim_debris_burst(q2_sim *sim, const s32 bmin[3], const s32 bmax[3],
                        const s32 *at, u32 count, u8 area)
{
    if (!sim || !sim->fx_ready)
        return 0;
    return q2_fx_debris_burst(&sim->fx, &sim->fx_rng, bmin, bmax, at, count,
                              area);
}

/* ------------------------------------------------------------------------- */
/* PSXGLASS's tail — 0x8002A3E4..0x8002A4F4, shared by both ways in           */
/* ------------------------------------------------------------------------- */
/* Defined with the rest of the visibility queue further down; needed here
 * because a breaking pane hides its own node. */
static void node_vis_push(q2_sim *sim, s16 node, u8 hidden);

/*
 * The centre of a Scene node's box, which is where the smash is played.
 *
 * 0x8002A3E4..0x8002A4A0 builds it a component at a time on the stack: +0x10
 * with +0x1C, +0x14 with +0x20, +0x18 with +0x24, each pair `addu` / `srl 31`
 * / `addu` / `sra 1`. That sequence is the compiler's signed divide by two and
 * rounds TOWARDS ZERO, which a plain `/ 2` on a s32 does as well — so this is
 * the arithmetic transcribed and not an approximation of it. A shift by one
 * would floor, and a pane straddling the origin would sound half a unit out.
 */
static void glass_box_centre(const q2_scene_node *node, s32 out[3])
{
    int k;

    for (k = 0; k < 3; k++)
        out[k] = (node->bbox_min[k] + node->bbox_max[k]) / 2;
}

/*
 * Queue one `amb_brkglas` at `at` for the owner to play — the console's
 * 0x8002A4A4, which passes the handle at 0x800B27FC to SoundAtLocation.
 *
 * Queued rather than played for the reason the detonation report is: the sim
 * has no mixer and no sound bank. Overflow drops the smash rather than
 * overwriting an earlier one, because the earlier one is the one already on its
 * way to the mixer.
 */
static void glass_smash_push(q2_sim *sim, const s32 at[3])
{
    u32 cap = sizeof(sim->glass_at) / sizeof(sim->glass_at[0]);
    int k;

    if (sim->glass_count >= cap)
        return;
    for (k = 0; k < 3; k++)
        sim->glass_at[sim->glass_count][k] = at[k];
    sim->glass_count++;
}

/*
 * AND THE PANE STOPS EXISTING. 0x8002A4AC..0x8002A4F4, which is BELOW the
 * branch at 0x8002A3CC and therefore runs on every break, replay included.
 *
 * Two things happen there and this port had neither:
 *
 *   - 0x8002A4C8 loads Scene2Data (0x800B2C3C), 0x8002A4D4/0x8002A4DC/
 *     0x8002A4E0 read the node's flags at +8, OR in 0x8000 and write them back.
 *     Bit 15 is do-not-draw, so the geometry vanishes the instant the pane
 *     breaks. The port drew every shot-out window for as long as the zone was
 *     loaded: node_vis_push is the port's way of setting that bit, because the
 *     hide array belongs to the owner (sim.h, node_vis).
 *
 *   - 0x8002A4E4/0x8002A4E8 free the damageable box at obj+0x28 and
 *     0x8002A4F0/0x8002A4F4 zero obj+0x28 and obj+0x24, the damage callback.
 *     Here that is `broken` plus the solidity sync: nothing can hit it again
 *     and it stops being solid. The shot path already did this half; the script
 *     path did not, so a pane a trigger had shattered was still a wall.
 *
 * Matched on the SCENE NODE rather than on the registry index because that is
 * the console's own key — the tail reads obj+0x38, the node, and the box it
 * frees is the box that node's constructor allocated (0x8002A518).
 */
static void glass_break_tail(q2_sim *sim, s32 node)
{
    u32 i;

    /* The registry keeps the index in a s32; the console's own field is the
     * signed halfword at obj+0x38 that 0x8002A4AC reads with `lh`, which is the
     * width the visibility queue carries too. */
    node_vis_push(sim, (s16)node, 1);

    for (i = 0; i < sim->breakable_count; i++)
        if (sim->breakable[i].kind == Q2_BREAKABLE_GLASS &&
            sim->breakable[i].scene_node == node)
            sim->breakable[i].broken = true;
    q2_sim_breakables_sync_solidity(sim);
}

u32 q2_sim_breakable_call(q2_sim *sim, const q2_scene *scene,
                          const q2_uf_operands *ops,
                          const q2_event_item *item, u8 call_index)
{
    const u8 *p;
    q2_scene_node node;
    s16 slot;
    u32 count_a, count_b, made = 0;

    if (!sim || !scene || !item || !item->payload || !sim->userfuncs_ready)
        return 0;

    /* Only GLASS. SHOOTTHEN's handler returns at 0x8002E840 when the damage
     * argument is zero, and a script CALL always passes zero, so running it
     * here would be inventing behaviour rather than reproducing it. */
    if (q2_userfuncs_prim(&sim->userfuncs, call_index) != Q2_UF_GLASS)
        return 0;

    /* The item is 16 bytes and the operands live at +4, +6, +10 and +12; the
     * payload points past the two header bytes, so a documented +N is
     * payload[N - 2] — the same convention q2_rotators_call uses. */
    if (item->len < 16)
        return 0;

    p = q2_uf_operand_at(ops, item->payload - 2, 16);

    slot = q2_rd_s16(p + 4);
    if (slot < 0 || !q2_scene_get_node(scene, (u32)slot, &node))
        return 0;

    count_a = q2_rd_u8(p + 10);
    count_b = q2_rd_u8(p + 12);

    /*
     * The node's own box, WITHOUT q2_scene_node_bounds' slop. That margin
     * exists so a node is not culled a pixel before its geometry reaches the
     * edge of it; the console's burst reads +16..+36 raw (0x800645A8), and
     * padding the box here would throw pieces from just outside the pane.
     */

    /*
     * The hit burst first, out of a point, and then the shatter across the
     * whole box — the two calls at 0x8002A384 and 0x8002A3DC in that order.
     * A script call carries no damage, so 0x8002A390 falls straight through
     * between them and both run.
     *
     * The point the hit burst comes out of is the runtime OBJECT, which this
     * port does not allocate; the node's centre is the nearest thing it has
     * and is where the destruction sound is placed (0x8002A4A4), so the two
     * agree. Stated because it is the one invented quantity here.
     */
    {
        s32 centre[3];

        glass_box_centre(&node, centre);

        made += q2_sim_debris_burst(sim, node.bbox_min, node.bbox_max,
                                    centre, count_a, 0);

        /*
         * 0x8002A3C4 `lh v0, 16948(gp)` / 0x8002A3CC `bne` to 0x8002A4AC: while
         * the EVE_ replay runs, the shatter and the sound are skipped. The hit
         * burst above is not — it is 0x8002A384, before the test — so a spent
         * pane replayed across a zone seam puffs once and does not come apart
         * a second time. The console's flag is the global gp+16948; the port's
         * is the runtime's own copy of it (events_rt.h, `initial_pass`).
         */
        if (!sim->event_rt.initial_pass) {
            made += q2_sim_debris_burst(sim, node.bbox_min, node.bbox_max,
                                        NULL, count_b, 0);

            /*
             * AND IT SMASHES. 0x8002A490 loads Sound_GlassSmash and 0x8002A4A4
             * plays it at the centre built just above — inside this branch with
             * the shatter, not below it with the hide, so a pane the replay
             * re-breaks comes back broken in silence. Nothing in this port ever
             * asked for `amb_brkglas` before: every breaking window was mute.
             */
            glass_smash_push(sim, centre);
        }
    }

    /*
     * The tail, OUTSIDE the branch: 0x8002A3CC jumps to 0x8002A4AC rather than
     * past it, so the hide and the freed box happen on the replay pass too.
     * That is what makes a pane the script already broke come back broken and
     * invisible instead of whole.
     */
    glass_break_tail(sim, slot);

    return made;
}

/* ------------------------------------------------------------------------- */
/* Shooting a breakable — the port's 0x80053AA4 sweep and 0x8002EF1C router    */
/* ------------------------------------------------------------------------- */
static void breakable_solids_drop(q2_sim *sim)
{
    u32 first, count, i;

    if (!sim)
        return;

    first = sim->mover_count;
    count = sim->breakable_solid_count;
    if (count && sim->volumes && first <= sim->volume_count &&
        count <= sim->volume_count - first) {
        memmove(sim->volumes + first, sim->volumes + first + count,
                (sim->volume_count - first - count) * sizeof(*sim->volumes));
        sim->volume_count -= count;
    }

    for (i = 0; i < sim->breakable_count; i++)
        sim->breakable[i].solid_target = -1;
    sim->breakable_solid_count = 0;
    sim->move_world.targets = sim->volumes;
    sim->move_world.count   = sim->volume_count;
}

/*
 * GLASS and SHOOTTHEN own ordinary entries in retail's entity-box table.
 * Insert those boxes after mover parts and before trigger volumes, preserving
 * the same entity-then-volume walk that q2_move_sweep_world implements.
 */
static bool breakable_solids_add(q2_sim *sim)
{
    q2_move_target *grown;
    u32 count = 0, first, out = 0, i;

    for (i = 0; i < sim->breakable_count; i++)
        if (sim->breakable[i].kind == Q2_BREAKABLE_GLASS ||
            sim->breakable[i].kind == Q2_BREAKABLE_SHOOTTHEN)
            count++;
    if (!count)
        return true;

    first = sim->mover_count;
    if (first > sim->volume_count)
        return false;

    grown = (q2_move_target *)calloc(sim->volume_count + count,
                                     sizeof(*grown));
    if (!grown)
        return false;

    if (sim->volumes && first)
        memcpy(grown, sim->volumes, first * sizeof(*grown));
    if (sim->volumes && sim->volume_count > first)
        memcpy(grown + first + count, sim->volumes + first,
               (sim->volume_count - first) * sizeof(*grown));

    for (i = 0; i < sim->breakable_count; i++) {
        q2_breakable *b = &sim->breakable[i];
        q2_move_target *t;
        int k;

        if (b->kind != Q2_BREAKABLE_GLASS && b->kind != Q2_BREAKABLE_SHOOTTHEN)
            continue;

        t = &grown[first + out];
        for (k = 0; k < 3; k++) {
            t->min[k] = t->env_min[k] = b->bmin[k];
            t->max[k] = t->env_max[k] = b->bmax[k];
        }
        t->dy           = 0;
        t->mask         = 0;
        t->kind         = Q2_MOVE_KIND_ENTITY;
        t->id           = (s32)i;
        t->active       = !b->broken;
        b->solid_target = (s32)(first + out);
        out++;
    }

    free(sim->volumes);
    sim->volumes                = grown;
    sim->volume_count          += out;
    sim->breakable_solid_count  = out;
    sim->move_world.targets     = sim->volumes;
    sim->move_world.count       = sim->volume_count;
    return true;
}

void q2_sim_breakables_sync_solidity(q2_sim *sim)
{
    u32 i;

    if (!sim || !sim->volumes)
        return;

    for (i = 0; i < sim->breakable_count; i++) {
        q2_breakable *b = &sim->breakable[i];

        if (b->solid_target < 0 ||
            (u32)b->solid_target >= sim->volume_count)
            continue;
        sim->volumes[b->solid_target].active = !b->broken;
    }
}

u32 q2_sim_attach_breakables(q2_sim *sim, const q2_scene *scene,
                             const q2_uf_operands *ops)
{
    q2_event_record rec, prev;
    bool more;
    u32 bi;

    if (!sim)
        return 0;

    breakable_solids_drop(sim);
    sim->breakable_count  = 0;
    sim->breakable_hits   = 0;
    sim->breakable_pieces = 0;
    sim->breakable_fired  = 0;
    sim->breakable_scene  = scene;

    /* The explosive set is rebuilt per zone by the owner, so anything the last
     * zone left pointing at it is stale. */
    sim->explosives           = NULL;
    sim->node_vis_count       = 0;
    sim->blast_count          = 0;
    q2_model_ent_sounds_clear(&sim->explosion_sounds);
    q2_model_ent_sounds_clear(&sim->grenade_bounce_sounds);
    sim->glass_count          = 0;   /* a smash from the last zone is not one
                                      * the new zone's mixer should hear */
    sim->explosive_destroyed  = 0;
    sim->explosive_blasts     = 0;

    for (bi = 0; bi < Q2_SIM_MAX_BREAKABLES; bi++)
        sim->breakable[bi].solid_target = -1;

    if (!scene || !sim->events_ready || !sim->userfuncs_ready)
        return 0;

    for (more = q2_events_first_record(&sim->events, &rec);
         more;
         more = q2_events_next_record(&sim->events, &prev, &rec)) {
        u32 i;

        prev = rec;

        for (i = 0; i < rec.n_items; i++) {
            q2_event_item item;
            q2_scene_node node;
            q2_breakable *b;
            const u8 *p;
            q2_uf_prim prim;
            u8  call_index;
            u32 need;
            s16 slot;
            int k;

            if (!q2_events_get_item(&sim->events, &rec, i, &item))
                break;
            if (!item.payload)
                continue;

            /*
             * A SHOOTABLE DOOR OR BUTTON IS A MOVER_A, not a CALL.
             *
             * 0x80025E98 tests the item's s16 at +20 for > 0 and, when it is,
             * installs the damage callback at object+0x24 and flags the box
             * 0x4 — and bit 0x4 is the only thing a weapon impact gates on. So
             * a panel you shoot to open a door is an ordinary MOVER_A with hit
             * points, and this loop never looked at one. Fourteen of them, in
             * ten maps.
             *
             * The hit points come from `p`, the WALKED copy: 0x80025E98 is
             * `lh v0, 20(s0)` with s0 the item in the record being walked, and
             * only the object slots are rebased (mover.h). The node slot is
             * therefore read from the rebased pointer and the health is not.
             */
            if (item.opcode == Q2_EVOP_MOVER_A) {
                const u8 *mp = item.payload - 2;
                const u8 *mq;
                s16 hp, mslot;

                if (item.len < 24)
                    continue;
                hp = q2_rd_s16(mp + 20);
                if (hp <= 0)
                    continue;               /* an ordinary door */
                if (sim->breakable_count >= Q2_SIM_MAX_BREAKABLES)
                    break;

                mq    = q2_uf_operand_at(ops, mp, item.len);
                mslot = q2_rd_s16(mq + 8);   /* the first node slot */
                if (mslot < 0 || !q2_scene_get_node(scene, (u32)mslot, &node))
                    continue;

                b = &sim->breakable[sim->breakable_count];
                memset(b, 0, sizeof(*b));
                b->solid_target = -1;
                b->scene_node = mslot;
                for (k = 0; k < 3; k++) {
                    b->bmin[k] = node.bbox_min[k];
                    b->bmax[k] = node.bbox_max[k];
                }
                b->health       = hp;
                b->kind         = (u8)Q2_BREAKABLE_MOVER;
                b->item_offset  = item.offset;
                b->record_offset = rec.offset;
                sim->breakable_count++;
                continue;
            }

            if (item.opcode != Q2_EVOP_CALL)
                continue;
            if (!q2_events_get_call_index(&item, &call_index))
                continue;
            prim = q2_userfuncs_prim(&sim->userfuncs, call_index);
            if (prim != Q2_UF_GLASS && prim != Q2_UF_SHOOTTHEN)
                continue;
            need = (prim == Q2_UF_GLASS) ? 16u : 8u;
            if (item.len < need)
                continue;
            if (sim->breakable_count >= Q2_SIM_MAX_BREAKABLES)
                break;

            /* The same rebase the scripted call uses: four of the disc's ten
             * object slots read -1 in COMMON's copy (#66). */
            p = q2_uf_operand_at(ops, item.payload - 2, need);

            slot = q2_rd_s16(p + 4);
            if (slot < 0 || !q2_scene_get_node(scene, (u32)slot, &node))
                continue;

            /*
             * The box, straight off the Scene node and WITHOUT the culling
             * slop q2_scene_node_bounds adds — 0x800555D8 copies the node
             * record's own numbers, and a padded box would be shootable from
             * just outside the pane.
             */
            b = &sim->breakable[sim->breakable_count];
            memset(b, 0, sizeof(*b));
            b->solid_target = -1;
            b->scene_node = slot;
            for (k = 0; k < 3; k++) {
                b->bmin[k] = node.bbox_min[k];
                b->bmax[k] = node.bbox_max[k];
            }
            b->health = q2_rd_s16(p + 6);
            b->kind   = (prim == Q2_UF_GLASS) ? (u8)Q2_BREAKABLE_GLASS
                                              : (u8)Q2_BREAKABLE_SHOOTTHEN;
            if (prim == Q2_UF_GLASS) {
                b->count_a = q2_rd_u8(p + 10);
                b->count_b = q2_rd_u8(p + 12);
            } else {
                /*
                 * SHOOTTHEN's constructor caches the record it is being built
                 * in (gp+16936) in obj+0x40, and its exec hands that back to
                 * the record dispatcher. Here the walk already knows which
                 * record the item came out of, so the cache is the loop's own
                 * variable.
                 */
                b->record_offset = rec.offset;
            }
            sim->breakable_count++;
        }
    }

    /* Allocation failure leaves the pane shootable and visible, but cannot
     * manufacture a valid solid target. The registry count is still the
     * function's documented return value. */
    (void)breakable_solids_add(sim);
    return sim->breakable_count;
}

/* ------------------------------------------------------------------------- */
/* The `func_explosive` groups — opcode 0x08                                  */
/* ------------------------------------------------------------------------- */
u32 q2_sim_attach_explosives(q2_sim *sim, q2_explosive_set *set,
                             const q2_scene *scene)
{
    u32 i, added = 0;

    if (!sim)
        return 0;

    sim->explosives = set;
    if (!set || !scene)
        return 0;

    if (!sim->breakable_scene)
        sim->breakable_scene = scene;

    for (i = 0; i < set->count; i++) {
        const q2_explosive *e = &set->items[i];
        int k;

        /*
         * A group with no hit points has no damage callback — 0x80026B10
         * branches past the arm that installs one — so a shot must find no box
         * to hit. Registering it anyway would make every scripted explosive
         * shootable, which is exactly the distinction the constructor draws.
         */
        if (!e->damageable)
            continue;

        for (k = 0; k < Q2_EXPLOSIVE_MAX_PARTS; k++) {
            q2_scene_node node;
            q2_breakable *b;
            int c;

            if (e->node[k] < 0)
                continue;
            if (sim->breakable_count >= Q2_SIM_MAX_BREAKABLES)
                return added;
            if (!q2_scene_get_node(scene, (u32)e->node[k], &node))
                continue;

            b = &sim->breakable[sim->breakable_count];
            memset(b, 0, sizeof(*b));
            b->solid_target = -1;
            b->scene_node = e->node[k];
            for (c = 0; c < 3; c++) {
                /* 0x800555D8 copies the node record's own six s32 from +16, so
                 * NOT q2_scene_node_bounds, which inflates for culling. */
                b->bmin[c] = node.bbox_min[c];
                b->bmax[c] = node.bbox_max[c];
            }
            b->health       = e->health;
            b->kind         = (u8)Q2_BREAKABLE_FXGROUP;
            b->part         = (u8)k;
            b->owner        = (s16)i;
            b->item_offset  = e->item_offset;
            b->record_offset = e->record_offset;
            sim->breakable_count++;
            added++;
        }
    }

    return added;
}

static void node_vis_push(q2_sim *sim, s16 node, u8 hidden)
{
    u32 cap = sizeof(sim->node_vis) / sizeof(sim->node_vis[0]);

    if (node < 0 || sim->node_vis_count >= cap)
        return;
    sim->node_vis[sim->node_vis_count].node   = node;
    sim->node_vis[sim->node_vis_count].hidden = hidden;
    sim->node_vis_count++;
}

bool q2_sim_next_node_vis(q2_sim *sim, s16 *node, u8 *hidden)
{
    u32 i;

    if (!sim || !sim->node_vis_count)
        return false;

    if (node)   *node   = sim->node_vis[0].node;
    if (hidden) *hidden = sim->node_vis[0].hidden;

    /* A queue rather than a stack: the console applies the hide and the show in
     * the order the handler emits them, and a group that reveals the node it
     * also hides would otherwise settle the wrong way. */
    for (i = 1; i < sim->node_vis_count; i++)
        sim->node_vis[i - 1] = sim->node_vis[i];
    sim->node_vis_count--;
    return true;
}

bool q2_sim_next_blast(q2_sim *sim, s32 out[3])
{
    u32 i;

    if (!sim || !sim->blast_count)
        return false;

    if (out) {
        out[0] = sim->blast_at[0][0];
        out[1] = sim->blast_at[0][1];
        out[2] = sim->blast_at[0][2];
    }
    for (i = 1; i < sim->blast_count; i++) {
        sim->blast_at[i - 1][0] = sim->blast_at[i][0];
        sim->blast_at[i - 1][1] = sim->blast_at[i][1];
        sim->blast_at[i - 1][2] = sim->blast_at[i][2];
    }
    sim->blast_count--;
    return true;
}

bool q2_sim_next_explosion_sound(q2_sim *sim, s32 out[3])
{
    return sim && q2_model_ent_sound_next(&sim->explosion_sounds, out);
}

/* The same drain for the pane smashes, because 0x8002A4A4 is the same
 * SoundAtLocation call with the same kind of argument as 0x8002695C's. */
bool q2_sim_next_glass_smash(q2_sim *sim, s32 out[3])
{
    u32 i;

    if (!sim || !sim->glass_count)
        return false;

    if (out) {
        out[0] = sim->glass_at[0][0];
        out[1] = sim->glass_at[0][1];
        out[2] = sim->glass_at[0][2];
    }
    for (i = 1; i < sim->glass_count; i++) {
        sim->glass_at[i - 1][0] = sim->glass_at[i][0];
        sim->glass_at[i - 1][1] = sim->glass_at[i][1];
        sim->glass_at[i - 1][2] = sim->glass_at[i][2];
    }
    sim->glass_count--;
    return true;
}

/*
 * Turn one destruction into effects and visibility changes.
 *
 * `0x8005A778` spawns a model explosion; explosive.h describes its arguments.
 */
static void explosive_apply(q2_sim *sim, const q2_explosive_result *res)
{
    u32 i;

    for (i = 0; i < res->burst_count; i++) {
        const q2_explosive_burst *b = &res->burst[i];
        q2_scene_node node;

        if (b->explode) {
            /*
             * THE MODEL ENTITY, AND NOTHING ELSE.
             *
             * 0x800267C4 makes exactly five calls and this is the whole list:
             * 0x80064558 twice (the hit burst and the destruction debris),
             * 0x8005A778 once, 0x80073704 for the report, and 0x80068818 to
             * hide the node. A PARTICLE BURST IS NOT AMONG THEM.
             *
             * `fx_at(sim, Q2_FX_EXPLOSION, ...)` used to run here. It was a
             * deliberate stand-in from when this port had no model entities and
             * explosive.h said so out loud — and then it survived the arrival
             * of the real thing, so a detonation drew the ROCKET's burst
             * (0x800486EC, a different site entirely) on top of the model the
             * console actually spawns. That is a bright cyan ball the console
             * never puts there, and it swamped the fireball behind it.
             *
             * A map whose CastList carries no `Explosion` gets no entity and no
             * substitute, which is the console's own behaviour: 0x8005A894
             * abandons the spawn.
             */
            if (sim->entities_ready &&
                q2_model_ent_spawn(&sim->entities, sim->model_bank,
                                   Q2_MODEL_ENT_EXPLOSION, b->at,
                                   (u8)b->area, Q2_MODEL_ENT_SIZE_FULL,
                                   &sim->explosion_sounds))
                sim->explosive_models++;

            sim->explosive_blasts++;

            /* And the report, at the same point (0x8002695C). Queued for the
             * owner, which is the half that has a mixer and a sound bank.
             *
             * ALWAYS, and as well as the spawn's own bang: 0x8002695C does not
             * test 0x8005A778's return, so the brush makes this sound whether
             * or not the model bound, and a second, identical one inside the
             * spawn when it did (sim.h, `explosion_sounds`). */
            if (sim->blast_count <
                    sizeof(sim->blast_at) / sizeof(sim->blast_at[0])) {
                int k;

                for (k = 0; k < 3; k++)
                    sim->blast_at[sim->blast_count][k] = b->at[k];
                sim->blast_count++;
            }
        }

        /* The origin argument is zero at 0x80026970, so the pieces scatter
         * through the node's whole box rather than out of a point. */
        if (sim->breakable_scene && b->pieces &&
            q2_scene_get_node(sim->breakable_scene, (u32)b->node, &node))
            sim->breakable_pieces +=
                q2_sim_debris_burst(sim, node.bbox_min, node.bbox_max, NULL,
                                    b->pieces, 0);
    }

    for (i = 0; i < res->hide_count; i++)
        node_vis_push(sim, res->hide[i], 1);
    for (i = 0; i < res->show_count; i++)
        node_vis_push(sim, res->show[i], 0);

    if (res->destroyed)
        sim->explosive_destroyed++;
}

/* Mark every registered box of a destroyed group dead, so a second shot finds
 * nothing — 0x80068818 frees each node's box as it hides it. */
static void explosive_free_boxes(q2_sim *sim, u32 item_offset)
{
    u32 i;

    for (i = 0; i < sim->breakable_count; i++)
        if (sim->breakable[i].kind == Q2_BREAKABLE_FXGROUP &&
            sim->breakable[i].item_offset == item_offset)
            sim->breakable[i].broken = true;
    q2_sim_scene_solids_update(sim, NULL);
}

bool q2_sim_explosive_trigger_item(q2_sim *sim, u32 item_offset)
{
    q2_explosive_result res;

    if (!sim || !sim->explosives)
        return false;

    if (!q2_explosive_trigger_item(sim->explosives, item_offset, false,
                                   sim->breakable_scene, &res))
        return false;

    explosive_apply(sim, &res);
    explosive_free_boxes(sim, item_offset);
    return true;
}

/*
 * Segment against an axis-aligned box: the standard slab test, in the fixed
 * point everything here is in.
 *
 * The console's own test is 0x80052078 and is not transcribed. What matters for
 * the behaviour is which box the shot crosses first, and that is a property of
 * the geometry rather than of the arithmetic; where the two could differ is a
 * shot that grazes an edge.
 */
static bool segment_hits_box(const s32 from[3], const s32 to[3],
                             const s32 bmin[3], const s32 bmax[3],
                             s32 out[3])
{
    /* Parametrised in 1.0.12 along the segment, as every other fraction in
     * this file is. */
    s32 t0 = 0, t1 = 4096;
    int k;

    for (k = 0; k < 3; k++) {
        s32 d = to[k] - from[k];

        if (d == 0) {
            if (from[k] < bmin[k] || from[k] > bmax[k])
                return false;
            continue;
        }
        {
            s64 a = ((s64)(bmin[k] - from[k]) << 12) / d;
            s64 b = ((s64)(bmax[k] - from[k]) << 12) / d;
            s32 lo = (s32)(a < b ? a : b);
            s32 hi = (s32)(a < b ? b : a);

            if (lo > t0) t0 = lo;
            if (hi < t1) t1 = hi;
            if (t0 > t1)
                return false;
        }
    }

    if (t0 < 0 || t0 > 4096)
        return false;

    for (k = 0; k < 3; k++)
        out[k] = from[k] + (s32)((((s64)(to[k] - from[k])) * t0) >> 12);
    return true;
}

u32 q2_sim_breakable_shot(q2_sim *sim, const s32 from[3], const s32 to[3],
                          s16 damage)
{
    q2_scene_node node;
    s32 best_at[3] = { 0, 0, 0 };
    s32 at[3];
    u32 i, made = 0;
    int best = -1;
    s64 best_d2 = 0;

    if (!sim || !sim->breakable_scene || !sim->breakable_count || !from || !to)
        return 0;

    /* The sweep: every in-use slot, nearest crossing wins — 0x80053AA4 walks
     * all 48 and keeps the closest for the same reason. */
    for (i = 0; i < sim->breakable_count; i++) {
        const q2_breakable *b = &sim->breakable[i];
        s64 d2;
        int k;

        if (b->broken)
            continue;
        if (!segment_hits_box(from, to, b->bmin, b->bmax, at))
            continue;

        d2 = 0;
        for (k = 0; k < 3; k++) {
            s64 d = (s64)at[k] - from[k];
            d2 += d * d;
        }
        if (best < 0 || d2 < best_d2) {
            best    = (int)i;
            best_d2 = d2;
            best_at[0] = at[0]; best_at[1] = at[1]; best_at[2] = at[2];
        }
    }

    if (best < 0)
        return 0;

    {
        q2_breakable *b = &sim->breakable[best];

        if (!q2_scene_get_node(sim->breakable_scene, (u32)b->scene_node, &node))
            return 0;

        sim->breakable_hits++;

        /*
         * SHOOTTHEN is the other primitive with a box, and it throws nothing:
         * `0x8002E81C` subtracts the damage from the item's hit points and, at
         * zero, frees the box and hands the record it was constructed in to the
         * record dispatcher. Shoot the panel, and whatever the rest of that
         * record does happens.
         */
        /*
         * A SHOOTABLE LEAF. 0x8002F050 subtracts the amount from the item's
         * own hit points and opens the door when they reach zero.
         *
         * The box is NOT freed and `broken` is not set: the console leaves the
         * counter at or below zero in the item, so every later shot re-opens
         * the leaf. That is the behaviour, and it is what makes a shoot-to-open
         * door work twice.
         *
         * The open itself is queued rather than done here — the mover set is
         * the caller's, not the sim's.
         */
        if (b->kind == Q2_BREAKABLE_MOVER) {
            if (damage == 0)
                return 0;
            b->health = (s16)(b->health - damage);
            if (b->health > 0)
                return 0;

            if (sim->breakable_open_count <
                    sizeof(sim->breakable_open) / sizeof(sim->breakable_open[0]))
                sim->breakable_open[sim->breakable_open_count++] =
                    b->item_offset;

            /*
             * AND ITS RECORD RUNS ON TOO. 0x8002F050 is the fourth damage
             * callback with 0x800267C4's contract: 0x8002F084 returns 0 while
             * the leaf stands, and the fatal hit falls through to 0x8002F144
             * `jr ra` / 0x8002F148 `addiu v0, a0, 24` — item + 24, the next
             * item — so 0x8002EFA8 lets it through to the executor exactly as
             * it does a crate's. A shoot-to-open door whose record carries a
             * STRING or a second mover behind it runs those as well.
             */
            if (sim->events_ready)
                (void)q2_event_rt_resume_after_item(&sim->event_rt,
                                                    b->record_offset,
                                                    b->item_offset, NULL);
            return 0;
        }

        /*
         * A `func_explosive`. The router (0x8002EF1C) hands the callback the
         * ITEM, the node whose box was hit and the amount, and everything the
         * exec does with them lives in explosive.c — including the hit burst,
         * which comes out of the node's whole box rather than the impact point
         * the way GLASS's does.
         */
        if (b->kind == Q2_BREAKABLE_FXGROUP) {
            q2_explosive_result res;
            q2_scene_node hit;

            if (!sim->explosives || b->owner < 0)
                return 0;

            if (!q2_explosive_damage(sim->explosives, (u32)b->owner,
                                     (int)b->part, damage, false,
                                     sim->breakable_scene, &res)) {
                /* Survived. The per-hit burst still ran — 0x80026820 precedes
                 * the survival test. */
                if (res.hit_pieces && res.hit_node >= 0 &&
                    sim->breakable_scene &&
                    q2_scene_get_node(sim->breakable_scene,
                                      (u32)res.hit_node, &hit))
                    made += q2_sim_debris_burst(sim, hit.bbox_min, hit.bbox_max,
                                                NULL, res.hit_pieces, 0);
                b->health = sim->explosives->items[b->owner].health;
                sim->breakable_pieces += made;
                return made;
            }

            if (res.hit_pieces && res.hit_node >= 0 &&
                sim->breakable_scene &&
                q2_scene_get_node(sim->breakable_scene,
                                  (u32)res.hit_node, &hit))
                made += q2_sim_debris_burst(sim, hit.bbox_min, hit.bbox_max,
                                            NULL, res.hit_pieces, 0);

            explosive_apply(sim, &res);
            explosive_free_boxes(sim, b->item_offset);
            sim->breakable_pieces += made;

            /*
             * AND THE REST OF THE RECORD RUNS, because a weapon killed it.
             *
             * 0x800267C4 returns `item + 28` rather than 0 once the
             * destruction loop has run (0x800269FC), and the weapon-impact
             * router at 0x8002EFA8 takes that as a cue: it reads the record
             * offset out of obj+0x40 and hands the record back to the
             * executor at the NEXT item (0x8002EFC8, a2 = item + len). The
             * visibility swap and the freed boxes happen first, exactly as
             * they do above, because the console's exec does them itself and
             * only then returns to the router.
             *
             * This is what spawns BASE0's jacket armour: record +508 is
             * [FXGROUP, CALL CREBATCH "Amour", CALL INSECRET] and nothing on
             * the map can reach it except a shot. Ten records on the disc
             * have a tail behind a shootable 0x08 item.
             */
            if (sim->events_ready &&
                q2_event_rt_resume_after_item(&sim->event_rt,
                                              b->record_offset,
                                              b->item_offset, NULL))
                sim->breakable_fired++;
            return made;
        }

        if (b->kind == Q2_BREAKABLE_SHOOTTHEN) {
            if (damage == 0)
                return 0;                    /* 0x8002E840: a script call */
            b->health = (s16)(b->health - damage);
            if (b->health > 0)
                return 0;

            b->broken = true;                /* the box is freed, not reused */
            q2_sim_breakables_sync_solidity(sim);
            if (sim->events_ready &&
                q2_event_rt_trigger(&sim->event_rt, b->record_offset))
                sim->breakable_fired++;
            return 0;
        }

        /*
         * The hit burst runs on EVERY call — 0x8002A384 is before the branch
         * that tests the damage — and comes out of the crossing point. Then the
         * hit points, which the console subtracts in the ITEM rather than in
         * the object, so a pane that has taken two shots remembers it.
         */
        made += q2_sim_debris_burst(sim, node.bbox_min, node.bbox_max, best_at,
                                    b->count_a, 0);

        if (damage != 0) {
            b->health = (s16)(b->health - damage);
            if (b->health > 0) {
                /* The hit burst this call made still counts. Returning
                 * straight out skipped the accumulator at the foot of the
                 * function, so every non-fatal hit on a pane threw its piece
                 * and reported none. */
                sim->breakable_pieces += made;
                return made;
            }
        }

        /*
         * The shatter, across the whole box: 0x8002A3DC passes zero for the
         * origin and the burst then scatters uniformly through the node. And
         * the smash with it, at the box centre — 0x8002A490/0x8002A4A4.
         *
         * Both sit under 0x8002A3CC's test of Event_MuteLevFlag, the same one
         * the script path reads. A weapon impact cannot reach PSXGLASS during
         * the EVE_ replay — the replay fires nothing — so this arm is that one
         * branch transcribed where it lives rather than a second rule; it is
         * here because there is one console function and it tests the flag once
         * for both ways in.
         */
        if (!sim->event_rt.initial_pass) {
            s32 centre[3];

            made += q2_sim_debris_burst(sim, node.bbox_min, node.bbox_max,
                                        NULL, b->count_b, 0);

            glass_box_centre(&node, centre);
            glass_smash_push(sim, centre);
        }

        /* And the tail below that branch: the node stops drawing and the box
         * is freed. `b` is marked by the node it occupies, so the pointer is
         * not used again here. */
        glass_break_tail(sim, b->scene_node);
    }

    sim->breakable_pieces += made;
    return made;
}
