/*
 * simcombat.c — where the reconstructed combat meets the reconstructed world.
 *
 * Everything here is glue, and deliberately so: weapon.c holds what the fire
 * functions do, combat.c holds what the damage function does, projectile.c
 * holds what a rocket is, and this file is the only place that knows all three
 * plus the collision hull. Keeping it separate from sim.c also keeps the
 * movement code readable, which is the harder of the two to follow.
 *
 * The one decision made here rather than read: a shot is traced against the
 * SecondaryCol hull the player moves in, because that is the only hull the port
 * can trace a segment through today. The console traces bullets against
 * PrimaryColl (0x80053974 takes the primary context) and only movement against
 * the secondary. The difference is the player's own 286-unit erosion, so a
 * bullet fired flat along a wall stops 286 units early. It is called out here
 * rather than hidden because it is a real divergence, not a rounding one.
 */
#include <stdlib.h>
#include <string.h>

#include "sim.h"
#include "explosive.h"
#include "modelent.h"
#include "trig.h"

/* ------------------------------------------------------------------------- */
/* Effects                                                                    */
/* ------------------------------------------------------------------------- */
void q2_sim_attach_effects(q2_sim *sim, const q2_fx_tables *tab, u32 seed)
{
    if (!sim)
        return;

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
     * ASK THE LEVEL SCRIPT whether it turns a glint on, and take its numbers.
     *
     * The flag, the band count and the phase are all written by `LevelBin`, and
     * this port does not execute one — but it can read it, which is enough. A
     * map whose script raises no glint gets none; a map whose script uses
     * different numbers gets those rather than BIGGUN's.
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

        sim->glint.raised     = true;
        sim->glint.band_count = script.band_count ? script.band_count
                                                  : Q2_FX_GLINT_BANDS;
        sim->glint.phase      = script.phase ? script.phase
                                             : Q2_FX_GLINT_PHASE_START;

        /*
         * The band records themselves are the one thing not readable: the
         * script writes them through its import table (effect.h), into memory
         * rather than into any chunk. The port lays them out evenly and says
         * so — it is the only invented quantity left in the effect system, and
         * it moves where the highlights sit, not whether or how they sweep.
         */
        sim->glint.tint[0] = 255;
        sim->glint.tint[1] = 220;
        sim->glint.tint[2] = 160;

        for (i = 0; i < sim->glint.band_count &&
                    i < Q2_FX_GLINT_BANDS_MAX; i++) {
            sim->glint.band[i].angle[1] =
                (s16)((s32)i * Q2_ONE_12 / (s32)sim->glint.band_count);
            sim->glint.band[i].phase  = (u8)(sim->glint.phase - (i & 3u));
            sim->glint.band[i].colour = 0x3AA0DCFFu;
        }
    }

    return true;
}

/* A spawn that costs nothing when no tables are attached. */
static void fx_at(q2_sim *sim, q2_fx_preset_id id, const s32 at[3])
{
    if (!sim->fx_ready || !at)
        return;
    q2_fx_spawn(&sim->fx, &sim->fx_rng, id, at, 0);
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
 * "all effects are broken". It used to raise Q2_FX_SPARK, whose site
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
static void fx_hitscan_impact(q2_sim *sim, const s32 origin[3],
                              const s32 dir[3], s32 frac, s32 victim,
                              const q2_damage_result *dr, s16 damage)
{
    s32 at[3];
    int k;

    if (!sim->fx_ready)
        return;

    for (k = 0; k < 3; k++)
        at[k] = origin[k] + (s32)(((s64)dir[k] * frac) >> Q2_FRAC_12);

    if (victim >= 0) {
        /* Flesh only: armour taking the whole hit is the case the HUD's damage
         * flash also distinguishes. */
        if (!dr || dr->taken > 0)
            fx_at(sim, Q2_FX_BLOOD, at);
        if (dr && dr->killed)
            q2_fx_gib(&sim->fx, &sim->fx_rng, at, 0, Q2_FX_BLOOD_RED);
        return;
    }

    if (frac < 4096) {
        /*
         * The area byte the console forwards here (`lbu s1, 32(v1)` at
         * 0x80048854, the trace's own area record) has no counterpart in this
         * port yet — nothing maps a contact point to an area key, and the
         * group draw has no visibility test to spend it on either. Zero, and
         * said so, rather than a fabricated key.
         */
        if (sim->fx_ready)
            q2_fx_bullet_puff(&sim->fx, &sim->fx_rng, at, 0);

        /*
         * And the BREAKABLE, tested along the whole shot rather than at its
         * end: the console's sweep runs INSIDE the trace, so a pane in front of
         * the wall is what the shot hits, not the wall behind it.
         */
        q2_sim_breakable_shot(sim, origin, at, damage);
    }
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
 * Every bolt on the disc carries the bit, so every bolt detonates through the
 * world arm — the same orange two-group burst every other missile gets. The
 * blue spark was never any part of it.
 *
 * STILL OWED: the gate ITSELF. This port raises a detonation burst for every
 * kind unconditionally, where the original tests bit 3 per record. It happens
 * to agree for the bolt because the bit is always set there; it will not agree
 * for whatever kind is spawned without it, and that kind has not been looked
 * for.
 */
static q2_fx_preset_id fx_for_projectile(q2_proj_kind kind)
{
    switch (kind) {
    case Q2_PROJ_BFG:  return Q2_FX_BFG_BURST;
    default:           return Q2_FX_EXPLOSION;
    }
}

/* ------------------------------------------------------------------------- */
void q2_sim_combat_init(q2_sim *sim)
{
    if (!sim)
        return;

    memset(&sim->combat, 0, sizeof(sim->combat));

    q2_inventory_init(&sim->combat.inv);
    q2_combat_rules_default(&sim->combat.rules);
    q2_projectiles_init(&sim->combat.projectiles);
    q2_rng_seed(&sim->combat.rng, 0x51ED2701u);

    /* A fresh player has the blaster and nothing else, which is what the
     * spawn path at 0x8003D4FC leaves in the weapon fields. */
    sim->combat.weapon_id        = Q2_WID_BLASTER;
    sim->combat.inv.weapons      = q2_weapon_tables_builtin()->owned_bit[Q2_WID_BLASTER];
    sim->combat.chaingun_bullets = 1;

    q2_actor_from_player(&sim->combat.self, &sim->combat.inv, sim->player[sim->cur_player].pos);
}

/* ------------------------------------------------------------------------- */
void q2_sim_aim(const q2_sim *sim, s16 out[3])
{
    s32 sy, cy, sp, cp;

    if (!out)
        return;
    if (!sim) {
        out[0] = out[1] = out[2] = 0;
        return;
    }

    sy = q2_sin12(sim->player[sim->cur_player].yaw);   cy = q2_cos12(sim->player[sim->cur_player].yaw);
    sp = q2_sin12(sim->player[sim->cur_player].pitch); cp = q2_cos12(sim->player[sim->cur_player].pitch);

    /* The engine keeps this triple at player+0x3C..0x40 as a 1.3.12 unit
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

bool q2_sim_give_weapon(q2_sim *sim, int weapon_id)
{
    const q2_weapon_tables *t = q2_weapon_tables_builtin();

    if (!sim || weapon_id <= 0 || weapon_id > Q2_WID_COUNT)
        return false;
    if (sim->combat.inv.weapons & t->owned_bit[weapon_id])
        return false;

    sim->combat.inv.weapons |= t->owned_bit[weapon_id];

    /*
     * AUTOSWITCH, and it is a DEVIATION from the console rather than a fix to
     * it — stated here so the next reader does not "correct" it back.
     *
     * 0x80037E78 switches to the weapon just picked up only when the BLASTER is
     * the one in hand: take a shotgun while holding a railgun and the railgun
     * stays. That is the disc's behaviour and it is what `autoswitch == false`
     * still does, exactly.
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
    if (!sim->autoswitch) {
        if (sim->combat.weapon_id == Q2_WID_BLASTER)
            sim->combat.weapon_id = weapon_id;
        return true;
    }

    {
        u32 i;
        int rank_new = -1, rank_held = -1;

        for (i = 0; i < t->autoswitch_count; i++) {
            if (t->autoswitch[i] == weapon_id && rank_new < 0)
                rank_new = (int)i;
            if (t->autoswitch[i] == sim->combat.weapon_id && rank_held < 0)
                rank_held = (int)i;
        }

        /* Off the list entirely — an explosive — is never promoted to. */
        if (rank_new < 0)
            return true;

        /* Holding something the list does not rank (the blaster is on it, so
         * this is an explosive in hand) means anything ranked wins. */
        if (rank_held < 0 || rank_new < rank_held) {
            if (q2_weapon_usable(&sim->combat.inv, weapon_id))
                sim->combat.weapon_id = weapon_id;
        }
    }

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
static s32 world_fraction_for(q2_sim *sim, const s32 origin[3],
                              const s32 dir[3])
{
    q2_trace tr;
    s32 end[3];
    int k;

    if (!sim->coll_ready)
        return 4096;

    for (k = 0; k < 3; k++)
        end[k] = origin[k] + dir[k];

    q2_sim_trace(sim, origin, end, &tr);
    return tr.hit ? tr.fraction : 4096;
}

q2_fire_result_v2 q2_sim_fire(q2_sim *sim)
{
    q2_fire_result_v2 r;
    s32 eye[3];
    s16 aim[3];
    s32 prev_next_fire;
    u32 i;

    memset(&r, 0, sizeof(r));
    r.sound = -1;
    if (!sim)
        return r;

    prev_next_fire = sim->combat.next_fire;

    q2_sim_eye(sim, eye);
    q2_sim_aim(sim, aim);

    sim->combat.rules.level_time = sim->level_time;
    q2_actor_from_player(&sim->combat.self, &sim->combat.inv, sim->player[sim->cur_player].pos);

    r = q2_weapon_fire(&sim->combat.inv, &sim->combat.rng, NULL,
                       sim->combat.weapon_id, eye,
                       sim->player[sim->cur_player].yaw,
                       sim->player[sim->cur_player].pitch,
                       sim->player[sim->cur_player].roll, aim,
                       sim->level_time, sim->combat.next_fire,
                       false, sim->combat.rules.deathmatch,
                       sim->combat.chaingun_bullets);

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

    /*
     * The muzzle flash. Only the machinegun and the chaingun carry one, and its
     * radii come from a single rand draw the way 0x8004C978 computes them --
     * see weapon.h. Drawn AFTER the shot so the flash does not perturb the
     * spread sequence, which is what the fire function's own ordering does.
     */
    if (q2_weapon_has_muzzle_light(sim->combat.weapon_id)) {
        static const u8 flash[3] = { Q2_MUZZLE_LIGHT_R, Q2_MUZZLE_LIGHT_G,
                                     Q2_MUZZLE_LIGHT_B };
        s32 inner, outer;

        q2_weapon_muzzle_light(q2_rng_next(&sim->combat.rng), &inner, &outer);
        /*
         * At the player ENTITY ORIGIN, not at the eye and not at the feet.
         * The console passes `addiu a0, s3, 84` — player + 0x54.
         *
         * `sim->player[].pos` is the FEET (sim.h says so at the top of the
         * file, and sim.c converts with q2_sim_origin_y everywhere it wants
         * the entity). Passing it raw moved the flash from 290 above the
         * origin to 286 below it — a correction in the right direction that
         * overshot by the whole body.
         */
        s32 lit[3];

        lit[0] = sim->player[sim->cur_player].pos[0];
        lit[1] = q2_sim_origin_y(sim->player[sim->cur_player].pos[1]);
        lit[2] = sim->player[sim->cur_player].pos[2];
        q2_ent_light_at(&sim->ent_world.events, lit, flash, inner, outer);
    }

    sim->combat.next_fire = r.next_fire;
    sim->combat.kick[0] = r.kick[0];
    sim->combat.kick[1] = r.kick[1];
    sim->combat.kick[2] = r.kick[2];

    /*
     * And on to the view, which is where a kick was always going: the weapon
     * table's figure has had a home in `combat.kick` for a while and nothing
     * read it. `q2_sim_view_angles` composes it over 30 ticks (FORMATS.md
     * §9.12.11a), so the deadline is what turns one number into recoil.
     */
    sim->player[sim->cur_player].kick[0]  = r.kick[0];
    sim->player[sim->cur_player].kick[1]  = r.kick[1];
    sim->player[sim->cur_player].kick[2]  = r.kick[2];
    sim->player[sim->cur_player].kick_time = sim->level_time + Q2_VIEW_KICK_FIRE;

    switch (r.kind) {
    case Q2_FK_BULLET:
        /* Every pellet is its own trace, which is why a shotgun can catch two
         * creatures and a machinegun cannot. */
        for (i = 0; i < r.shot_count; i++) {
            const q2_shot *s = &r.shot[i];
            s32 frac = world_fraction_for(sim, s->origin, s->dir);
            q2_damage_result dr;
            s32 victim;

            victim = q2_combat_fire_bullet(&sim->combat.self, s->origin,
                                           s->dir, s->damage, frac,
                                           Q2_HITSCAN_RADIUS,
                                           sim->combat.targets,
                                           sim->combat.target_count,
                                           &sim->combat.rules, &dr);
            fx_hitscan_impact(sim, s->origin, s->dir, frac, victim, &dr,
                              s->damage);
        }
        break;

    case Q2_FK_RAIL: {
        const q2_shot *s = &r.shot[0];
        s32 frac = world_fraction_for(sim, s->origin, s->dir);
        u32 hits = q2_combat_fire_rail(&sim->combat.self, s->origin, s->dir,
                                       s->damage, frac, Q2_HITSCAN_RADIUS,
                                       sim->combat.targets,
                                       sim->combat.target_count,
                                       &sim->combat.rules);

        /* The rail does not stop at the first target, so it always marks the
         * world where the beam ends and blood is left to the per-target pass
         * this port does not get back from `fire_rail`. */
        fx_hitscan_impact(sim, s->origin, s->dir, frac, -1, NULL,
                          s->damage);
        (void)hits;
        break;
    }

    default:
        /* WHO fired it. The -1 here meant "the world", so a bolt could not say
         * who to credit and a kill by one had no killer. */
        q2_sim_proj_scan.launched++;
        if (q2_projectile_launch(&sim->combat.projectiles, &r,
                                 sim->cur_player, sim->level_time) < 0) {
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
                q2_weapon_refund(&sim->combat.inv, sim->combat.weapon_id);
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

    return r;
}

q2_damage_result q2_sim_hurt_player(q2_sim *sim, q2_actor *attacker,
                                    s16 damage, s16 mod, const s32 point[3])
{
    q2_damage_result out;

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

    /* Health and armour live in the inventory, everything else in the actor, so
     * the two are synchronised around the call rather than duplicated. */
    q2_actor_from_player(&sim->combat.self, &sim->combat.inv, sim->player[sim->cur_player].pos);
    sim->combat.rules.level_time = sim->level_time;

    /*
     * A CONTACT HIT LANDS AT THE ATTACKER, and that is not the caller's choice
     * to make. `0x800612F0` passes the creature's own origin as the damage
     * point — `q2_combat_melee` is that one line — and everything downstream of
     * `point` is DIRECTIONAL: the knockback, where the blood sprays, and the
     * flinch's roll, which is the hit point measured against the view's own
     * right vector.
     *
     * The client used to hand this the PLAYER's own position for a melee, which
     * makes that difference the zero vector. `side` came out 0 on every claw
     * that ever landed, so a hit from the left rolled the view exactly as far
     * as a hit from the right — which is to say not at all — and the blood
     * sprayed from the player's own feet. Overriding the point here rather than
     * trusting the caller is what stops that being reintroduced: a melee is the
     * one mod whose point is not free.
     */
    if (mod == Q2_MOD_MELEE && attacker) {
        out   = q2_combat_melee(attacker, &sim->combat.self, damage,
                                &sim->combat.rules);
        point = attacker->origin;
    } else {
        out = q2_combat_damage(attacker, &sim->combat.self, damage, mod, point,
                               &sim->combat.rules);
    }

    q2_actor_to_player(&sim->combat.self, &sim->combat.inv);

    /* Blood only when flesh actually took some of it: armour absorbing the
     * whole hit is the case the HUD's damage flash also distinguishes. */
    if (out.taken > 0)
        fx_at(sim, Q2_FX_BLOOD, point);

    /*
     * The flinch. Amplitude scaled by how much got through, capped at the same
     * 40 degrees the fall kick is capped at, and pitched UP and rolled with the
     * sign of the knockback so a hit from the left throws the view right.
     *
     * The amplitude is the port's: 0x80038334 reads client+0x9C and +0x9E and
     * this is the only path that could write them, but the write itself sits
     * behind the damage callback rather than in T_Damage, so what the console
     * puts there is not established. The DECAY is the console's — 150 ticks
     * against the pain deadline — and that is the part that is felt.
     */
    if (out.taken > 0) {
        s32 amp = out.taken * 8;
        s32 side = 0;

        if (amp > Q2_FALL_KICK_MAX)
            amp = Q2_FALL_KICK_MAX;

        /* Which side it came from: the hit point against the view's own right
         * vector, so a shot from the left rolls the view right. */
        if (point) {
            s32 sy = q2_sin12(sim->player[sim->cur_player].yaw), cy = q2_cos12(sim->player[sim->cur_player].yaw);
            s32 dx = point[0] - sim->player[sim->cur_player].pos[0];
            s32 dz = point[2] - sim->player[sim->cur_player].pos[2];

            side = (cy * dx - sy * dz) >> Q2_FRAC_12;
        }

        sim->player[sim->cur_player].hurt_kick[0] = (s16)(-amp);
        sim->player[sim->cur_player].hurt_kick[1] = (s16)((side >= 0) ? -amp / 2 : amp / 2);
    }

    return out;
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

    if (owner == &sim->combat.self && sim->invulnerable)
        return;

    if (owner == &sim->combat.self)
        q2_actor_from_player(owner, &sim->combat.inv,
                             sim->player[sim->cur_player].pos);

    one[0] = owner;
    q2_combat_radius_damage(owner, NULL, point ? point : p->pos, p->damage,
                            p->splash_radius, p->mod, one, 1,
                            &sim->combat.rules);

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

        memcpy(where, p->pos, sizeof(where));
        projectile_owner_splash(sim, p, where, targets, count);
        q2_projectile_detonate(&sim->combat.projectiles, (u32)index,
                               attacker_for(sim, p->owner), targets, count,
                               &sim->combat.rules);
        fx_at(sim, Q2_FX_EXPLOSION, where);
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
            /* 0x8004A7C0: after reveal, velocity and throw sound. */
            (void)q2_weapon_consume(&sim->combat.inv,
                                    Q2_WID_HAND_GRENADE);
            return Q2_HAND_GRENADE_RELEASED;
        }
    }

    return Q2_HAND_GRENADE_HELD;
}

/* ------------------------------------------------------------------------- */
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

    /*
     * The energy-bolt effect's green light, one per lit actor. 0x80058638 gates
     * it on effect[1] >= 3 and reads its colour and radii from 0x800AEAAC and
     * 0x800AEAB0 — see combat.h. Raised here rather than at the damage site so
     * it lasts as long as the effect does rather than for the tick that armed it.
     */
    {
        static const u8 energy[3] = { Q2_ENERGY_LIGHT_R, Q2_ENERGY_LIGHT_G,
                                      Q2_ENERGY_LIGHT_B };
        u32 t;

        if (q2_actor_energy_lit(&sim->combat.self))
            q2_ent_light_at(&sim->ent_world.events, sim->combat.self.origin,
                            energy, Q2_ENERGY_LIGHT_INNER,
                            Q2_ENERGY_LIGHT_OUTER);

        for (t = 0; t < sim->world_target_count; t++) {
            const q2_actor *a = sim->world_targets ? sim->world_targets[t] : NULL;
            if (a && q2_actor_energy_lit(a))
                q2_ent_light_at(&sim->ent_world.events, a->origin, energy,
                                Q2_ENERGY_LIGHT_INNER, Q2_ENERGY_LIGHT_OUTER);
        }

        /*
         * AND THEN THE TIMER RUNS DOWN, which nothing did.
         *
         * `q2_mod_effect_timer` is a TIMER and its only writer sets it; no path
         * anywhere decremented it. So the first energy hit an actor took left
         * `effect[1]` at full strength for the rest of the level, and the green
         * light raised above followed that actor from then on, permanently.
         *
         * On the player that is the reported fault. Soldiers fire energy bolts,
         * so the player is hit within seconds of any fight, and from that moment
         * a 1300-unit pure-green light (0,255,0) is parked on them. Creatures
         * gather the dynamic list FIRST and rank by brightness, so any monster
         * that walks near the player has its whole colour matrix replaced by
         * that one light — measured, cell 128 handed a soldier `L0 0,3206,0`
         * with nothing else active, which is a monster dyed green rather than
         * tinted. The map itself carries no such lamp: of BASE1's 254 lights,
         * 248 are mixed, six are pure red and NONE is pure green.
         *
         * Every slot is run down rather than just the one the light reads,
         * because they are all the same kind of field — a per-modifier
         * countdown at entity+0x2F0..0x2F4 — and leaving the others latched
         * would only move the same bug to whichever one is read next.
         */
        {
            u32 s;
            for (s = 0; s < sizeof(sim->combat.self.effect); s++)
                if (sim->combat.self.effect[s])
                    sim->combat.self.effect[s]--;

            for (t = 0; t < sim->world_target_count; t++) {
                q2_actor *a = sim->world_targets ? sim->world_targets[t] : NULL;
                if (!a)
                    continue;
                for (s = 0; s < sizeof(a->effect); s++)
                    if (a->effect[s])
                        a->effect[s]--;
            }
        }
    }

    for (i = 0; i < Q2_PROJ_MAX; i++) {
        q2_projectile *p = &sim->combat.projectiles.p[i];
        q2_proj_step step;
        q2_actor **hit_list;
        u32 hit_count;
        s32 hit_index;
        s32 dir[3];
        bool held;
        int k;

        if (!p->in_use)
            continue;

        q2_sim_proj_scan.stepped++;
        held = (p->kind == Q2_PROJ_HAND_GRENADE &&
                p->node == Q2_PROJ_NODE_HELD);

        /*
         * Every live projectile lights the world, from the preset the sweep at
         * 0x80047C6C reads out of 0x800AE954 -- warm orange, outer radius 800.
         * Raised before the step so the light sits where the bolt was drawn
         * this frame rather than where it is about to be.
         */
        if (!held) {
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

        q2_projectile_step(&sim->combat.projectiles, i, sim->gravity,
                           sim->cur_dt, sim->level_time, &step);

        /* State 1 has no mover, collision body, light or visible model. The
         * owner update after the view-model step attaches it and resolves an
         * elapsed fuse at the current hand position. */
        if (held)
            continue;

        if (step.expired) {
            q2_sim_proj_scan.expired++;
            /* Grenade2's +0xF4 is a fuse and calls its explosion. Rocket and
             * BFGBlast use the same-looking field only as a safety lifetime:
             * 0x8004ADB0 / 0x8004B8E4 free them without damage or an effect.
             * The bolt lifetime is quiet for the same reason. */
            if (p->kind == Q2_PROJ_GRENADE ||
                p->kind == Q2_PROJ_HAND_GRENADE) {
                q2_fx_preset_id fx = fx_for_projectile(p->kind);
                s32 where[3];
                q2_actor **targets = sim->world_targets
                                          ? sim->world_targets
                                          : sim->combat.targets;
                u32 count = sim->world_targets ? sim->world_target_count
                                               : sim->combat.target_count;

                memcpy(where, p->pos, sizeof(where));
                projectile_owner_splash(sim, p, where, targets, count);
                q2_projectile_detonate(&sim->combat.projectiles, i,
                                       attacker_for(sim, p->owner),
                                       targets, count,
                                       &sim->combat.rules);
                fx_at(sim, fx, where);
            } else {
                q2_projectile_expire(&sim->combat.projectiles, i);
            }
            continue;
        }

        /*
         * The BFG's beams — the game's weapon trail.
         *
         * 0x8004BD04 calls the beam maintainer every tick while the ball flies,
         * and it holds a green beam on every target it can see, refreshing each
         * one rather than adding a second (effect.h). The beams outlive the
         * ball's passage by their own timer, which is what makes the BFG leave
         * a lattice behind it rather than a single line.
         *
         * The port's visibility test is the same segment sweep the projectile
         * itself uses, because it has no separate line-of-sight query; the
         * original calls 0x80051874. Called out as the one substitution.
         */
        if (p->kind == Q2_PROJ_BFG && sim->fx_ready) {
            u32 t;

            for (t = 0; t < sim->combat.target_count; t++) {
                const q2_actor *a = sim->combat.targets[t];
                /*
                 * THE ONE PLACE THE CONSOLE READS `takedamage` OUTSIDE
                 * T_Damage, so this filter can be the original's rather than a
                 * stand-in: the beam maintainer at 0x80049B9C does
                 * `lw v0, 748(a1)` / `lw v0, 28(v0)` / `and 0xC0000000` /
                 * `beq v0, zero` at 0x80049CBC..0x80049CD8. It asks whether the
                 * thing can be hurt, not whether it is alive.
                 */
                if (!a || !a->takedamage)
                    continue;

                q2_fx_beam_timed(&sim->fx, (s32)i, (s32)t,
                                 p->pos, a->origin,
                                 Q2_FX_TIMED_BEAM_RADIUS,
                                 Q2_FX_TIMED_BEAM_STYLE,
                                 Q2_FX_TIMED_BEAM_LIFE);
            }
        }

        /* A creature in the way takes it before the world does. */
        for (k = 0; k < 3; k++)
            dir[k] = step.to[k] - step.from[k];

        hit_list  = sim->world_targets ? sim->world_targets
                                       : sim->combat.targets;
        hit_count = sim->world_targets ? sim->world_target_count
                                       : sim->combat.target_count;

        hit_index = q2_combat_nearest_on_segment(step.from, dir,
                                                 Q2_HITSCAN_RADIUS,
                                                 hit_list, hit_count);

        /* Never its own shooter: the world list holds everybody, including the
         * player who fired this. */
        if (hit_index >= 0 && hit_list[hit_index] == attacker_for(sim, p->owner))
            hit_index = -1;

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

                /* Same rule as the sweep in combat.c: a corpse is a target
                 * until its `takedamage` says otherwise, which is what lets a
                 * rocket finish the job on a body already down. */
                if (!t || t == attacker_for(sim, p->owner) || !t->takedamage)
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

        if (hit_index >= 0) {
            q2_actor *victim = hit_list[hit_index];

            q2_sim_proj_scan.hit++;
            q2_fx_preset_id fx = fx_for_projectile(p->kind);
            bool was_alive = victim && victim->health > 0;

            projectile_owner_splash(sim, p, step.to, hit_list, hit_count);
            q2_projectile_impact(&sim->combat.projectiles, i, step.to, NULL,
                                 attacker_for(sim, p->owner), victim,
                                 hit_list, hit_count,
                                 &sim->combat.rules);

            /*
             * Three bursts can come out of one impact and they are separate
             * effects in the original too: the projectile's own, the victim's
             * blood, and the gib puff if that was the killing blow. The gib
             * takes the creature's blood colour, which is a class property
             * rather than an effect parameter (effect.h).
             */
            fx_at(sim, fx, step.to);
            if (victim) {
                fx_at(sim, Q2_FX_BLOOD, step.to);
                if (was_alive && victim->health <= 0 && sim->fx_ready) {
                    q2_fx_gib(&sim->fx, &sim->fx_rng, step.to, 0,
                              Q2_FX_BLOOD_RED);
                }
            }
            continue;
        }

        if (sim->coll_ready || sim->coll_primary_ready) {
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
                /* The hull gives back the clipped point; the port has no
                 * surface normal here, so a grenade landing on geometry stops
                 * rather than bouncing. Called out because the bounce sound is
                 * one of the twenty-two and the behaviour certainly exists. */
                bool consumed;
                q2_fx_preset_id fx = fx_for_projectile(p->kind);
                q2_actor **targets = sim->world_targets
                                          ? sim->world_targets
                                          : sim->combat.targets;
                u32 count = sim->world_targets ? sim->world_target_count
                                               : sim->combat.target_count;

                projectile_owner_splash(sim, p, end, targets, count);
                consumed = q2_projectile_impact(&sim->combat.projectiles, i,
                                                end, NULL,
                                                attacker_for(sim, p->owner),
                                                NULL, targets, count,
                                                &sim->combat.rules);
                /* A grenade that only bounced has not gone off, so it must not
                 * leave a fireball behind. */
                if (consumed)
                    fx_at(sim, fx, end);
                continue;
            }
        }

        q2_projectile_commit(&sim->combat.projectiles, i, step.to);
    }

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
        int k;

        for (k = 0; k < 3; k++)
            centre[k] = (node.bbox_min[k] + node.bbox_max[k]) / 2;

        made += q2_sim_debris_burst(sim, node.bbox_min, node.bbox_max,
                                    centre, count_a, 0);
        made += q2_sim_debris_burst(sim, node.bbox_min, node.bbox_max,
                                    NULL, count_b, 0);
    }

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
 * GLASS owns an ordinary entry in retail's 48-slot entity-box table.
 * Insert those boxes after mover parts and before trigger volumes, preserving
 * the same entity-then-volume walk that q2_move_sweep_world implements.
 */
static bool breakable_solids_add(q2_sim *sim)
{
    q2_move_target *grown;
    u32 count = 0, first, out = 0, i;

    for (i = 0; i < sim->breakable_count; i++)
        if (sim->breakable[i].kind == Q2_BREAKABLE_GLASS)
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

        if (b->kind != Q2_BREAKABLE_GLASS)
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

        if (b->kind != Q2_BREAKABLE_GLASS || b->solid_target < 0 ||
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

/*
 * Turn one destruction into effects and visibility changes.
 *
 * The port's stand-in for `0x8005A778` is the PARTICLE explosion — see
 * explosive.h for why, and for what changes here when the model-entity
 * explosion exists.
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
                                   (u8)b->area))
                sim->explosive_models++;

            sim->explosive_blasts++;

            /* And the report, at the same point (0x8002695C). Queued for the
             * owner, which is the half that has a mixer and a sound bank. */
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
            return made;
        }

        if (b->kind == Q2_BREAKABLE_SHOOTTHEN) {
            if (damage == 0)
                return 0;                    /* 0x8002E840: a script call */
            b->health = (s16)(b->health - damage);
            if (b->health > 0)
                return 0;

            b->broken = true;                /* the box is freed, not reused */
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
            if (b->health > 0)
                return made;
        }

        /* The shatter, across the whole box: 0x8002A3DC passes zero for the
         * origin and the burst then scatters uniformly through the node. */
        made += q2_sim_debris_burst(sim, node.bbox_min, node.bbox_max, NULL,
                                    b->count_b, 0);
        b->broken = true;
        q2_sim_breakables_sync_solidity(sim);
    }

    sim->breakable_pieces += made;
    return made;
}
