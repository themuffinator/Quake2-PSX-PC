#include "sim.h"

#include <stdlib.h>
#include <string.h>

#include "entitydraw.h"   /* q2_entity_resolve_model */
#include "levelbin.h"     /* q2_levelbin_scene — the title screen's objects */
#include "pad.h"          /* the default control style */
#include "playerdeath.h" /* the death voice's own rule */
#include "reloc.h"        /* q2_level_module_load */
#include "trig.h"

/* Defined below, but attach_gameplay needs it. */
static void build_volumes(q2_sim *sim);

/*
 * The FX dispatcher, 0x80027840, gets its target from the live event context:
 * the player who entered the trigger. Its fifth T_Damage argument is never
 * prepared by the console code (see userfuncs.h), so this port makes the
 * necessary defined choice explicit: NULL means no fabricated impact point or
 * knockback. The two meaningful operands themselves are exactly retail.
 */
bool q2_sim_apply_event_fx(q2_sim *sim, const q2_event_item *item)
{
    s16 mod, damage;

    if (!sim || !q2_events_get_fx_damage(item, &mod, &damage))
        return false;

    q2_sim_hurt_player(sim, NULL, damage, mod, NULL);
    return true;
}

static void event_fx(void *user, const q2_event_item *item)
{
    q2_sim_apply_event_fx((q2_sim *)user, item);
}

void q2_sim_init(q2_sim *sim, const q2_world_zone *zone, int tick_rate_hz)
{
    if (!sim)
        return;

    memset(sim, 0, sizeof(*sim));
    sim->zone         = zone;
    sim->current_node = -1;
    /* On unless a caller turns it off — see the field. */
    sim->autoswitch   = true;
    sim->player[sim->cur_player].ent.node = -1;

    /*
     * SecondaryCol is the hull entities move in — read out of the zone loader,
     * which points the mover's context (0x800C8FE8) at it, and not guessed from
     * node counts. That also settles the old puzzle about SecondaryCol having
     * FEWER nodes than PrimaryColl on 9 of 115 zones: it is not a refinement of
     * the primary hull, it is a hull for a different job.
     */
    if (zone && q2_collision_parse(&sim->coll, &zone->zone, Q2_COLL_SECONDARY) == Q2_OK)
        sim->coll_ready = true;

    if (zone && q2_collision_parse(&sim->coll_primary, &zone->zone,
                                   Q2_COLL_PRIMARY) == Q2_OK)
        sim->coll_primary_ready = true;

    /*
     * A zone that ships no SecondaryCol still has to be walkable, so fall back
     * to the primary hull rather than to no collision at all. Every zone on the
     * PAL disc carries both, so this never fires there.
     */
    if (!sim->coll_ready && sim->coll_primary_ready) {
        sim->coll       = sim->coll_primary;
        sim->coll_ready = true;
    }

    /* dt advances by 300/field_rate per field. PAL fields run at 50 Hz giving
     * 6, NTSC at 60 giving 5. The engine's own PAL value is 6 (0x80018DB8). */
    sim->dt_per_field = (tick_rate_hz > 0) ? (Q2_DT_HZ / tick_rate_hz) : Q2_DT_PER_FIELD;
    if (sim->dt_per_field <= 0)
        sim->dt_per_field = Q2_DT_PER_FIELD;

    /* Default ON: a caller with a view-weapon machine turns it off, and every
     * other caller — the harness, the tests — keeps the old behaviour. */
    sim->fire_from_input    = true;
    sim->gravity            = Q2_GRAVITY;
    sim->player[sim->cur_player].view_height = Q2_VIEW_STAND;

    /*
     * STANDARD A, which is what 0x8001BDA8 writes for every player. It matters
     * that this is not left at zero: zero is RIGHT MOUSE, and the two are on
     * opposite sides of the branch that decides whether the look rate is eased
     * or set — so a caller that never configures a pad would otherwise get the
     * mouse's instant turn on a keyboard.
     */
    sim->player[sim->cur_player].look_scheme = Q2_PAD_STYLE_STANDARD_A;

    q2_sim_combat_init(sim);
}

void q2_sim_free(q2_sim *sim)
{
    if (!sim)
        return;
    q2_event_rt_free(&sim->event_rt);
    q2_entity_set_free(&sim->entities);
    free(sim->item_group_run);
    free(sim->item_group_order);
    free(sim->trigger_inside);
    free(sim->volumes);
    free(sim->mover_base);
    free(sim->mover_last_off);
    free(sim->volume_damage);
    free(sim->volume_mod);
    free(sim->volume_env);
    memset(sim, 0, sizeof(*sim));
}

q2_result q2_sim_attach_items(q2_sim *sim, const q2_common_file *common,
                              int zone, const q2_item_table *table,
                              const struct q2_model_bank *bank)
{
    const dat_chunk *levelbin;
    q2_population pop;
    q2_result r;

    if (!sim || !common)
        return Q2_ERR_INVALID_ARG;

    q2_entity_set_free(&sim->entities);
    free(sim->item_group_run);
    free(sim->item_group_order);
    sim->item_group_run        = NULL;
    sim->item_group_order      = NULL;
    sim->item_group_order_count = 0;
    sim->item_population_ready = false;
    memset(&sim->item_population, 0, sizeof(sim->item_population));
    sim->item_table = NULL;
    sim->item_bank  = NULL;
    q2_entity_world_init(&sim->ent_world);
    sim->entities_ready = false;

    /* Kept so a detonation can bind the `Explosion` model entity without the
     * bank having to reach five frames down the weapon path (modelent.h). */
    sim->model_bank = bank;

    /*
     * The world's view of the session. `multiplayer` is 0x800AEBCC, which is
     * the same flag the item handlers read to double an ammo box and the same
     * one that decides whether a collected item respawns at all.
     */
    if (!table)
        table = q2_item_table_builtin();

    sim->ent_world.deathmatch = sim->multiplayer;
    sim->ent_world.items      = table;
    sim->ent_world.level_time = sim->level_time;

    /* One player: this sim models one. The touch sweep walks
     * `player_count`, so registering slot 0 is what makes it run. */
    q2_entity_world_add_player(&sim->ent_world, 0, &sim->combat.inv,
                               sim->player[sim->cur_player].pos);

    r = q2_population_parse(&pop, common);
    if (r != Q2_OK)
        return r;

    sim->item_group_run = (u8 *)calloc(pop.group_count ? pop.group_count : 1,
                                       sizeof(*sim->item_group_run));
    if (!sim->item_group_run)
        return Q2_ERR_NO_MEMORY;
    sim->item_group_order = (u32 *)calloc(pop.group_count ? pop.group_count : 1,
                                          sizeof(*sim->item_group_order));
    if (!sim->item_group_order) {
        free(sim->item_group_run);
        sim->item_group_run = NULL;
        return Q2_ERR_NO_MEMORY;
    }

    sim->item_population = pop;
    sim->item_table      = table;
    sim->item_bank       = bank;

    /* Which Population groups exist at level start is code in LevelBin, not a
     * property of the place lists. The selector decoder recovers those calls
     * without executing the module; item.c applies the same resident-zone plus
     * selected-batch rule the creature world already uses. */
    levelbin = common->chunk[Q2_COMMON_LEVEL_BIN];

    /* SecondaryCol, so every item is dropped onto its floor as the console
     * drops it. NULL until the zone's hull is parsed, which is the order
     * q2_sim_attach_zone already establishes. */
    r = q2_item_spawn_zone(&sim->entities, &pop, zone,
                           levelbin ? levelbin->data : NULL,
                           levelbin ? levelbin->size : 0,
                           table,
                           sim->coll_ready ? &sim->coll : NULL,
                           sim->item_group_run, NULL);
    if (r != Q2_OK)
        return r;

    /* q2_item_spawn_zone walks Population order. Record that exact order,
     * including selected groups with empty place lists, so a save can append
     * later CREBATCH groups in the sequence the allocator originally saw. */
    {
        u32 gi;
        for (gi = 0; gi < pop.group_count; gi++) {
            if (sim->item_group_run[gi])
                sim->item_group_order[sim->item_group_order_count++] = gi;
        }
    }

    if (bank) {
        u32 i;
        for (i = 0; i < sim->entities.count; i++)
            q2_entity_resolve_model(&sim->entities.ent[i], bank);
    }

    sim->entities_ready = true;
    sim->item_population_ready = true;
    return Q2_OK;
}

u32 q2_sim_activate_item_group(q2_sim *sim, const char *group)
{
    q2_item_spawn_stats stats;
    q2_result r;
    u32 gi, i;
    bool was_run;

    if (!sim || !sim->entities_ready || !sim->item_population_ready ||
        !group || !group[0])
        return 0;

    for (gi = 0; gi < sim->item_population.group_count; gi++) {
        q2_pop_group g;

        if (!q2_pop_get_group(&sim->item_population, gi, &g))
            continue;
        if (strcmp(g.name, group) == 0)
            break;
    }
    if (gi == sim->item_population.group_count)
        return 0;

    was_run = sim->item_group_run[gi] != 0;
    r = q2_item_spawn_group(&sim->entities, &sim->item_population, group,
                            sim->item_table,
                            sim->coll_ready ? &sim->coll : NULL,
                            sim->item_group_run, &stats);
    if (r != Q2_OK && r != Q2_ERR_NO_MEMORY)
        return 0;

    if (!was_run && sim->item_group_run[gi] &&
        sim->item_group_order_count < sim->item_population.group_count)
        sim->item_group_order[sim->item_group_order_count++] = gi;

    if (sim->item_bank) {
        /* The entity allocator reuses collected holes before it appends.
         * Scanning only the old count..new count tail leaves a CREBATCH item
         * placed into a low slot unresolved at spawn time. Restrict the pass
         * by the stable Population group instead: existing members are cheap
         * no-ops in q2_entity_resolve_model, while both reused and appended
         * members receive the retail spawn-time lookup. */
        for (i = 0; i < sim->entities.count; i++) {
            q2_entity *e = &sim->entities.ent[i];

            if (e->in_use && e->population_group == (s32)gi)
                q2_entity_resolve_model(e, sim->item_bank);
        }
    }

    return stats.spawned;
}

/*
 * The Q2LOGO's own think, which the module installs over the item one on every
 * page change — module+0x9D24 on the title screen, module+0x9E0C on every other
 * front-end page. See levelbin.h for the pair side by side; the only difference
 * is which way the light-intensity ramp runs and where it stops.
 *
 * The spin is `yaw -= 4 * dt` in both, taken from `*(engine+0xD4)` — the same
 * level-clock delta the item think reads, so `w->dt` is it.
 */
static void scene_logo_spin(q2_entity *e, q2_entity_world *w)
{
    e->angles[1] = (s32)(s16)(e->angles[1] - Q2_LB_SCENE_SPIN * (w ? w->dt : 0));
}

static void scene_logo_title_think(q2_entity *e, q2_entity_world *w)
{
    if (!e)
        return;
    /*
     * 0x80109D38: the test is on the OLD value and the add is in the branch's
     * delay slot, so an intensity of 3968 goes to 4224 for one frame and is
     * clamped the next. That overshoot is the original's and is left in.
     */
    if (e->scale < Q2_LB_SCENE_SCALE_FULL)
        e->scale = (s16)(e->scale + Q2_LB_SCENE_SCALE_STEP);
    else
        e->scale = Q2_LB_SCENE_SCALE_FULL;
    scene_logo_spin(e, w);
}

static void scene_logo_sub_think(q2_entity *e, q2_entity_world *w)
{
    if (!e)
        return;
    /* 0x80109E20, the same shape the other way: below the floor it snaps. */
    if (e->scale < Q2_LB_SCENE_SCALE_SUB + 1)
        e->scale = Q2_LB_SCENE_SCALE_SUB;
    else
        e->scale = (s16)(e->scale - Q2_LB_SCENE_SCALE_STEP);
    scene_logo_spin(e, w);
}

void q2_sim_scene_page(q2_sim *sim, bool title, bool visible)
{
    q2_entity *e;
    u32 p;

    if (!sim || !sim->scene_ready || sim->entities.count == 0)
        return;

    /* Object 0 and only object 0 — 0x80103564 indexes the array directly. */
    e = &sim->entities.ent[0];
    if (!e->in_use)
        return;

    for (p = 0; p < Q2_MAX_PLAYERS; p++)
        e->taken[p] = !visible;

    e->think = title ? scene_logo_title_think : scene_logo_sub_think;
}

/*
 * Real time into the engine's 1/300 s units, CARRYING the fraction.
 *
 * `carried` is the fraction left over from last time; `rest`, when given, takes
 * the fraction this call leaves behind. A speculative caller (the mouse's
 * look-ahead, which asks what the next step would be without taking it) passes
 * NULL, or it steals time from the tick that follows. That is also why the
 * carry comes in by value rather than as the sim: the speculative query holds
 * a const one.
 */
static s32 sim_whole_units(double carried, double elapsed_seconds, double *rest)
{
    double frac = carried + elapsed_seconds * (double)Q2_DT_HZ;
    double whole;

    if (frac < 0.0)
        frac = 0.0;
    whole = (double)(s32)frac;      /* toward zero; frac is non-negative */

    if (rest)
        *rest = frac - whole;

    return (s32)whole;
}

u32 q2_sim_scene_advance(q2_sim *sim, double elapsed_seconds)
{
    s32 dt;

    if (!sim || !sim->scene_ready || elapsed_seconds <= 0.0)
        return 0;

    sim->dt_accum += sim_whole_units(sim->dt_frac, elapsed_seconds,
                                     &sim->dt_frac);
    dt = sim->dt_accum;
    if (dt < Q2_DT_NOMINAL)
        return 0;
    if (dt > Q2_DT_MAX)
        dt = Q2_DT_MAX;
    sim->dt_accum = 0;

    sim->level_time          += dt;
    sim->ent_world.dt         = dt;
    sim->ent_world.deathmatch = sim->multiplayer;
    q2_entity_run(&sim->entities, &sim->ent_world);
    return 1;
}

u32 q2_sim_attach_scene(q2_sim *sim, const q2_common_file *common,
                        const q2_item_table *table,
                        const struct q2_model_bank *bank)
{
    q2_ai_module mod;
    q2_lb_scene scene;
    u32 i, n = 0;

    if (!sim || !common)
        return 0;

    if (q2_level_module_load(&mod, common, Q2_SIM_SCENE_BASE) != Q2_OK)
        return 0;
    if (mod.empty ||
        !q2_levelbin_scene(mod.image.data, (u32)mod.image.size, mod.base,
                           &scene)) {
        q2_ai_module_free(&mod);
        return 0;
    }
    q2_ai_module_free(&mod);

    if (!table)
        table = q2_item_table_builtin();

    /*
     * The world has to exist before the thinks can run against it, and the
     * front end never calls q2_sim_attach_items — there is no Population to
     * attach — so this does the same setup that one does. No player is
     * registered: the front end has none, which is also why the touch sweep
     * the module guards against never runs.
     */
    if (!sim->entities_ready) {
        q2_entity_world_init(&sim->ent_world);
        sim->ent_world.deathmatch = sim->multiplayer;
        sim->ent_world.items      = table;
        sim->ent_world.level_time = sim->level_time;
    }

    for (i = 0; i < scene.count; i++) {
        q2_pop_place place;
        q2_entity *e;

        /* module+0xE48, the template the spawner fills: every field zero but
         * the flags nibble and the id. */
        memset(&place, 0, sizeof(place));
        place.angle_flags = Q2_POP_PLACE_UNUSED_1000;
        place.id  = scene.id[i];

        /* The front end's props are a synthetic template at the origin and
         * the builder overwrites their origin below — there is no floor to
         * drop to and nothing that would use it. */
        e = q2_item_spawn(&sim->entities, &place, table, 0, NULL);
        if (!e)
            continue;

        /*
         * RESOLVED FIRST, because resolving fills in the model's vertical bias
         * and recomputes the draw origin from it (entitydraw.c) — and the
         * builder below is entitled to overwrite that origin outright. With the
         * resolve last, the logo was moved to pos + 286 - ext2 = -1111 instead
         * of the module's own 0: still built into the table at 145 faces a
         * frame, but 1111 units above the eye and therefore outside the
         * module's five-light rig, so every vertex shaded black and the title
         * screen came up empty.
         */
        if (bank)
            q2_entity_resolve_model(e, bank);

        /* 0x8010C654..0x8010C66C, in that order. `q2_item_spawn` has already
         * set the draw origin from the place record; this is the builder
         * overwriting it, which is why the two do not have to agree. */
        e->angles[0] = 0;
        e->angles[1] = Q2_LB_SCENE_YAW;
        e->angles[2] = 0;
        e->render_flags = 0;
        e->origin[0] = 0;
        e->origin[1] = 0;
        e->origin[2] = Q2_LB_SCENE_DIST;
        /*
         * `pos` (+0x54) is deliberately left where the place record put it. The
         * builder writes +0xA4 and nothing else, so the two disagree in the
         * original too — the draw reads +0xA4 and the touch sweep reads +0x54,
         * and the sweep is the one the module has already disarmed.
         */

        /*
         * module+0x3414 hides all five — bit 0x80 of every player's block —
         * and then shows exactly one back. So the four coloured player models
         * are spawned and never drawn on any page this port reaches, and that
         * is the module's doing rather than a gap here.
         */
        {
            u32 p;
            for (p = 0; p < Q2_MAX_PLAYERS; p++)
                e->taken[p] = true;
        }

        n++;
    }

    if (n) {
        sim->entities_ready = true;
        sim->scene_ready    = true;
        /*
         * The scale is left where `q2_item_spawn` put it, which for a record
         * that does not materialise is full size (0x80059A98). Nothing in the
         * module zeroes it, so the logo is NOT born small and does not grow in
         * on the first frame — an earlier pass here made it do that and the
         * ramp had no source.
         *
         * What the title screen's ramp is actually for is the way BACK: a
         * sub-page has driven the scale down to 1024, and returning to the
         * title walks it up to 4096 again. It is one animation seen from two
         * ends, and it only runs after you have been somewhere.
         */
        q2_sim_scene_page(sim, true, true);
    }
    return n;
}

const q2_ent_events *q2_sim_entity_events(const q2_sim *sim)
{
    return sim ? &sim->ent_world.events : NULL;
}

/*
 * 0x80027E64's predicate arm, resolved once instead of run every tick.
 *
 * Six of the primitives a volume can call have a body that is one OR into
 * entity+0x98 and nothing else (0x8002E574, 0x8002E594, 0x8002E5B4, 0x8002F214
 * and two more). Which bit each sets is already in userfuncs.h, so a volume's
 * environment is a property of its record and can be read at load.
 *
 * Four of the six are here. INACID and INLAVA are not, and the reason is the
 * frame's own clear at 0x8003A25C: it clears 0x4, 0x8, 0x100, 0x200, 0x400,
 * 0x4000 and 0x20000, and INLAVA's 0x1000 is not among them. A bit the frame
 * does not clear is not a per-tick environment — it latches, and something else
 * owns its lifetime — so asserting it here would set a flag nothing takes back.
 * Their damage is not this function's to do either.
 */
static u32 record_env_mask(const q2_events *ev, const q2_userfuncs *uf,
                           u32 record_offset)
{
    q2_event_record rec;
    u32 mask = 0;
    u32 i;

    if (!q2_events_record_at(ev, record_offset, &rec))
        return 0;

    for (i = 0; i < rec.n_items; i++) {
        q2_event_item item;
        u8            call_index;
        q2_uf_prim    prim;

        if (!q2_events_get_item(ev, &rec, i, &item))
            break;
        if (item.opcode != Q2_EVOP_CALL)
            continue;
        if (!q2_events_get_call_index(&item, &call_index))
            continue;

        prim = q2_userfuncs_prim(uf, call_index);

        switch (prim) {
        case Q2_UF_INWATER:      mask |= Q2_ENV_INWATER;     break;
        case Q2_UF_UNDERWATER:   mask |= Q2_ENV_UNDERWATER;  break;
        case Q2_UF_INCROUCH:     mask |= Q2_ENV_INCROUCH;    break;
        case Q2_UF_INLOWCROUCH:  mask |= Q2_ENV_INLOWCROUCH; break;

        /*
         * The answer to worldscale.h's "nothing in the image sets it". Nothing
         * in the EXECUTABLE does; DONTJUMP does, and it is the same bit. So a
         * no-jump zone is authored per map exactly as a crouch zone is, and the
         * gate in the jump was never dead code — it was waiting for its caller.
         */
        case Q2_UF_DONTJUMP:     mask |= Q2_ENV_DONTJUMP;    break;

        /*
         * The two hazard volumes that are also ENVIRONMENT volumes. 0x8002E4E4
         * and 0x8002E558 both `ori v0, v0, 0x1100` on entity+0x98 before
         * calling the damage function — so being in deep acid or deep lava is
         * being UNDERWATER, with everything that follows from it: the
         * weightless integrator arm, the swim wish rates, swimming up, and the
         * water-exit jump that gets you back out.
         *
         * Only 0x100 is asserted here. 0x1000 is deliberately left alone: it is
         * absent from the clear list at 0x8003A260, so it LATCHES, and a
         * level-triggered function that re-asserts every tick is the wrong
         * owner for a bit that is meant to stick.
         *
         * Live on WASTE3 and WASTE4. UNDERLAVA is bound but declared by no map.
         */
        case Q2_UF_UNDERACID:
        case Q2_UF_UNDERLAVA:    mask |= Q2_ENV_UNDERWATER;  break;

        default: break;
        }
    }

    return mask;
}

/*
 * The same walk, for what the record HURTS you for.
 *
 * Five primitives are damage volumes, and their handlers are as short as the
 * environment ones — an OR of a flag, if any, and one call to the damage
 * function with a constant amount and mod:
 *
 *   INACID     0x8002E49C   damage(0, ent,  1, mod 9)
 *   UNDERACID  0x8002E4C8   ent+0x98 |= 0x1100; damage(0, ent,  1, mod 9)
 *   INLAVA     0x8002E500   ent+0x98 |= 0x1000; damage(0, ent, 20, mod 10)
 *   UNDERLAVA  0x8002E53C   ent+0x98 |= 0x1100; damage(0, ent, 20, mod 10)
 *   LASERWALL  0x8002E1F0   damage(ent, ent, item[+20], mod 11)
 *
 * The flag bits are deliberately NOT asserted here and the reason is above the
 * environment walk: the frame's clear at 0x8003A25C does not include 0x1000, so
 * that bit latches and something other than the volume owns its lifetime.
 * Setting it every tick would set a flag nothing takes back. The damage does
 * not depend on it.
 *
 * LASERWALL's attacker is the TARGET, not nobody — `a0` still holds the entity
 * at its call site where the other four zero it. It changes nothing here (the
 * player is the only thing these volumes can reach) and it is recorded because
 * it is the sort of asymmetry that looks like a transcription slip later.
 *
 * A record with more than one of them takes the LAST, which is the engine's
 * outcome too: the calls run in order and each one hurts, but the throttle the
 * first arms suppresses the rest within its window.
 */
static void record_hazard(const q2_events *ev, const q2_userfuncs *uf,
                          u32 record_offset, s16 *out_damage, s16 *out_mod)
{
    q2_event_record rec;
    u32 i;

    *out_damage = 0;
    *out_mod    = 0;

    if (!q2_events_record_at(ev, record_offset, &rec))
        return;

    for (i = 0; i < rec.n_items; i++) {
        q2_event_item item;
        u8            call_index;
        q2_uf_prim    prim;

        if (!q2_events_get_item(ev, &rec, i, &item))
            break;
        if (item.opcode != Q2_EVOP_CALL)
            continue;
        if (!q2_events_get_call_index(&item, &call_index))
            continue;

        prim = q2_userfuncs_prim(uf, call_index);

        switch (prim) {
        case Q2_UF_INACID:
        case Q2_UF_UNDERACID:
            *out_damage = 1;
            *out_mod    = Q2_MOD_ACID;
            break;
        case Q2_UF_INLAVA:
        case Q2_UF_UNDERLAVA:
            *out_damage = 20;
            *out_mod    = Q2_MOD_LAVA;
            break;
        case Q2_UF_LASERWALL: {
            q2_uf_call call;
            s32        amount = 0;

            if (q2_uf_decode_call(&call, uf, &item) != Q2_OK)
                break;
            if (!q2_uf_operand_s32(&call, 1, 0, &amount))
                break;
            /* `bltz` on the object slot at +18 returns before the damage, so a
             * wall with no object never fires. */
            {
                s16 slot = 0;

                if (q2_uf_operand_slot_raw(&call, 0, 0, &slot) && slot < 0)
                    break;
            }
            *out_damage = (s16)amount;
            *out_mod    = Q2_MOD_LASER;
            break;
        }
        default: break;
        }
    }
}

/* Build the per-volume environment masks. Silently leaves them all zero when
 * the map has no UserFuncs table, which is the same as having no such volume. */
static void build_volume_env(q2_sim *sim)
{
    u32 i;

    free(sim->volume_env);
    free(sim->volume_damage);
    free(sim->volume_mod);
    sim->volume_env    = NULL;
    sim->volume_damage = NULL;
    sim->volume_mod    = NULL;

    if (!sim->triggers_ready || !sim->events_ready ||
        !sim->userfuncs_ready || sim->triggers.count == 0)
        return;

    sim->volume_env    = (u32 *)calloc(sim->triggers.count, sizeof(u32));
    sim->volume_damage = (s16 *)calloc(sim->triggers.count, sizeof(s16));
    sim->volume_mod    = (s16 *)calloc(sim->triggers.count, sizeof(s16));
    if (!sim->volume_env || !sim->volume_damage || !sim->volume_mod)
        return;

    for (i = 0; i < sim->triggers.count; i++) {
        q2_trigger t;

        if (!q2_trigger_get(&sim->triggers, i, &t))
            continue;
        if (t.event_offset == Q2_TRIGGER_NO_EVENT)
            continue;

        sim->volume_env[i] = record_env_mask(&sim->events, &sim->userfuncs,
                                             t.event_offset);
        record_hazard(&sim->events, &sim->userfuncs, t.event_offset,
                      &sim->volume_damage[i], &sim->volume_mod[i]);
    }
}

q2_result q2_sim_attach_gameplay(q2_sim *sim, const q2_common_file *common)
{
    if (!sim || !common)
        return Q2_ERR_INVALID_ARG;

    sim->userfuncs_ready =
        q2_userfuncs_parse(&sim->userfuncs, common) == Q2_OK;

    if (q2_triggers_parse(&sim->triggers, common) == Q2_OK) {
        sim->triggers_ready = true;

        free(sim->trigger_inside);
        sim->trigger_capacity = sim->triggers.count;
        sim->trigger_inside   = (u8 *)calloc(sim->trigger_capacity ? sim->trigger_capacity : 1, 1);
        if (!sim->trigger_inside) {
            sim->triggers_ready = false;
            return Q2_ERR_NO_MEMORY;
        }
    }

    if (q2_events_parse_common(&sim->events, common) == Q2_OK) {
        if (q2_event_rt_init(&sim->event_rt, &sim->events) == Q2_OK) {
            sim->events_ready = true;
            sim->event_rt.on_fx = event_fx;
            sim->event_rt.on_fx_user = sim;
        }
    }

    /* The same volumes serve three jobs: firing scripts, answering the contents
     * query, and being swept against. Build the sweep list once here. */
    build_volumes(sim);

    /* And a fourth: asserting an environment while you stand in one. */
    build_volume_env(sim);

    return Q2_OK;
}

bool q2_sim_take_zone_change(q2_sim *sim, u32 *out_zone)
{
    if (!sim || !sim->zone_change_pending)
        return false;

    if (out_zone)
        *out_zone = sim->zone_change_target;

    sim->zone_change_pending = false;
    return true;
}

/*
 * Feed the player's trigger contacts to the Events record categories.
 *
 * This used to make every volume edge-triggered. Retail does not: record flag
 * 0x08 means enter, 0x10 means every frame inside, and 0x20 means leave
 * (0x80027E64). In particular, liquid/environment records and held-open doors
 * need the continuous arm. The event runtime owns the two record-level contact
 * bits because several volumes can name one record; `trigger_inside` remains
 * the per-volume edge used by diagnostics and save games.
 */
static void update_triggers(q2_sim *sim)
{
    s32 at[3];
    u32 i;

    if (!sim->events_ready)
        return;

    q2_event_rt_contacts_begin(&sim->event_rt);

    /*
     * THE SAMPLE POINT IS THE ENTITY ORIGIN, and this was the whole of "doors
     * are stubborn, and take some moving around to eventually trigger".
     *
     * The engine has exactly one trigger dispatcher — 0x80027E64, reached only
     * from 0x8003A29C with the player entity in a0 — and at 0x80027F0C it takes
     * `addiu a1, fp, 84`, entity+0x54, and hands that ONE POINT to the
     * point-in-box test at 0x80044098. entity+0x54 is the origin: 0x8003A308
     * passes the same address to the find-cell routine, and the eye is built
     * from it at 0x80038630 as `origin.y + 286 - viewOffset`.
     *
     * This tested `player.pos`, the FEET, which is 286 lower. TrigBounds boxes
     * are authored around the body CENTRE, so their floor-side face sits 40 to
     * 220 units above the floor — measured on BASE1, of 27 volumes a player can
     * stand in, 26 contain the origin (with margins of 60 to 303 units) and
     * only 11 contain the feet, every one of those by 1 to 17 units. Fifteen
     * could not be entered at all. A door whose volume the player can only
     * clip the corner of is a door that opens when you shuffle about.
     *
     * The port was already inconsistent with itself: `update_contents` queries
     * the same volume set with `p->ent.pos`, the origin.
     */
    at[0] = sim->player[sim->cur_player].pos[0];
    at[1] = q2_sim_origin_y(sim->player[sim->cur_player].pos[1]);
    at[2] = sim->player[sim->cur_player].pos[2];

    for (i = 0; sim->triggers_ready &&
                i < sim->triggers.count && i < sim->trigger_capacity; i++) {
        bool inside = q2_trigger_contains(&sim->triggers, i, at);
        bool was    = sim->trigger_inside[i] != 0;

        sim->trigger_inside[i] = inside ? 1u : 0u;

        if (!inside)
            continue;

        {
            q2_trigger trig;
            if (!q2_trigger_get(&sim->triggers, i, &trig))
                continue;
            if (trig.event_offset == Q2_TRIGGER_NO_EVENT)
                continue;

            if (sim->trace_zone && !was) {
                u32 k;
                sim->trace_last_trigger = i;
                for (k = 0; k < 3; k++) {
                    sim->trace_last_box[k]     = trig.min[k];
                    sim->trace_last_box[3 + k] = trig.max[k];
                }
                Q2_INFO("[zone] trigger %u entered at (%d,%d,%d)"
                        "  box (%d..%d, %d..%d, %d..%d)  event @%u",
                        i, at[0], at[1], at[2],
                        trig.min[0], trig.max[0], trig.min[1], trig.max[1],
                        trig.min[2], trig.max[2], trig.event_offset);
            }

            q2_event_rt_contact(&sim->event_rt, trig.event_offset);
        }
    }

    q2_event_rt_contacts_end(&sim->event_rt);

    if (q2_event_rt_update(&sim->event_rt) == Q2_EVENT_ZONE_CHANGE) {
        sim->zone_change_pending = true;
        sim->zone_change_target  = sim->event_rt.pending_zone;
        sim->event_rt.has_zone_change = false;

        if (sim->trace_zone)
            Q2_WARN("[zone] ZONEGATE raised by trigger %u"
                    " -> '%s' (zone %u); player origin (%d,%d,%d)",
                    sim->trace_last_trigger,
                    sim->event_rt.pending_zone_name,
                    sim->event_rt.pending_zone, at[0], at[1], at[2]);
    }
}

/* ------------------------------------------------------------------------- */
/* Volumes                                                                    */
/* ------------------------------------------------------------------------- */
/*
 * 0x8006FE3C — move `*p` toward `target` by at most `rate`.
 *
 * Not a lerp and not a helper the port invented: this one function is the whole
 * of the engine's acceleration, deceleration, view-height ease, turn-rate ease
 * and liquid buoyancy. There is no separate friction term anywhere in the
 * player's movement, because a clamped approach toward a zero target already
 * decelerates.
 *
 * Two details that matter for bit-exactness:
 *
 *   - It operates on a HALFWORD. The step is computed in 32 bits and stored
 *     with `sh`, so a step that would overshoot the s16 range wraps rather than
 *     saturating. Callers pass values well inside the range, but the truncation
 *     is what the console does and it is free to reproduce.
 *   - `rate` is used raw. The original does not take its absolute value, so a
 *     negative rate walks AWAY from the target. No caller does that; the port
 *     does not add a guard the hardware does not have.
 *
 * Returns true when the value has arrived, which is what the original's return
 * value at 0x8006FEB4 reports.
 */
static bool ease16(s16 *p, s32 target, s32 rate)
{
    s32 cur = *p;             /* lh: sign-extended to 32 bits           */
    s16 tgt = (s16)target;    /* sll 16 / sra 16 at 0x8006FE3C          */
    s32 r16 = (s16)rate;      /* the TEST uses the 16-bit-truncated rate */

    /*
     * The test is a 32-bit comparison (`addu v0, a3, v0` at 0x8006FE60 with no
     * narrowing) while the store truncates (`sh` at 0x8006FE9C). Narrowing the
     * test as well looks harmless and is not: a step that would overshoot the
     * s16 range wraps to the far side, compares as smaller than the target, and
     * the value is stored instead of being snapped.
     */
    if (cur < tgt)
        *p = (cur + r16 < tgt) ? (s16)(cur + rate) : tgt;
    else
        *p = (tgt < cur - r16) ? (s16)(cur - rate) : tgt;

    return *p == tgt;
}

/* The same, for the s32-typed fields the port keeps wider than the original's
 * halfwords. The arithmetic still wraps at 16 bits so the answer is identical. */
static s32 ease32(s32 v, s32 target, s32 rate)
{
    s16 t = (s16)v;

    ease16(&t, target, rate);
    return t;
}

/* ------------------------------------------------------------------------- */
/* Doors and lifts as solid boxes — 0x800555D8 and 0x80051EC0                 */
/* ------------------------------------------------------------------------- */
/*
 * A mover's displacement used to reach only the RENDERER. `q2_movers_tick`
 * accumulates an offset in the runtime object and the zone draw adds it to the
 * node's camera-space position as it draws it (mover.h) — which is exactly what
 * the console does, and is only half of what the console does. It ALSO
 * registers one 64-byte AABB per mover part in the fixed 48-slot table at
 * 0x800CAE10 — allocator 0x800555D8, which is handed `scene_node_record + 16`
 * and copies the six words verbatim — chains the group through slot+0x3C, and
 * shifts the whole chain every tick through 0x80051EC0.
 *
 * Without that half a door is visually present and physically absent, which is
 * exactly the report: movers "are non-solid and do not do anything".
 */
static void mover_part_box(const q2_scene *scene, s32 node,
                           s32 min_out[3], s32 max_out[3])
{
    q2_scene_node n;
    int k;

    for (k = 0; k < 3; k++) {
        min_out[k] = 0;
        max_out[k] = 0;
    }

    if (!scene || node < 0 || !q2_scene_get_node(scene, (u32)node, &n))
        return;

    /*
     * The RAW record, not q2_scene_node_bounds. That helper inflates every
     * face by Q2_SCENE_BBOX_SLOP so a node's own faces are strictly inside its
     * box; the allocator does no such thing. Four units on a door edge is four
     * units the player is stopped short by, on every door in the game.
     */
    for (k = 0; k < 3; k++) {
        min_out[k] = n.bbox_min[k];
        max_out[k] = n.bbox_max[k];
    }
}

static void mover_targets_drop(q2_sim *sim)
{
    u32 i;

    if (!sim->mover_count || !sim->volumes)
        return;

    memmove(sim->volumes, sim->volumes + sim->mover_count,
            (sim->volume_count - sim->mover_count) * sizeof(*sim->volumes));
    sim->volume_count -= sim->mover_count;

    /* Glass follows the mover prefix in the same entity table. Moving the
     * tail down changes every cached pane index by exactly that prefix. */
    for (i = 0; i < sim->breakable_count; i++)
        if (sim->breakable[i].solid_target >= 0)
            sim->breakable[i].solid_target -= (s32)sim->mover_count;
    sim->mover_count   = 0;

    free(sim->mover_base);
    free(sim->mover_last_off);
    sim->mover_base     = NULL;
    sim->mover_last_off = NULL;
}

q2_result q2_sim_attach_movers(q2_sim *sim, const q2_mover_set *set,
                               const q2_scene *scene)
{
    q2_move_target *grown;
    u32 parts = 0, i, p, out;

    if (!sim)
        return Q2_ERR_INVALID_ARG;

    /* Rebuild from the volumes alone, so a second call cannot accumulate stale
     * boxes from a zone that has been unloaded. */
    mover_targets_drop(sim);

    if (set && scene) {
        for (i = 0; i < set->count; i++) {
            if (set->movers[i].blocks_player)
                parts += set->movers[i].part_count;
        }
    }

    if (parts == 0) {
        sim->move_world.targets = sim->volumes;
        sim->move_world.count   = sim->volume_count;
        return Q2_OK;
    }

    grown = (q2_move_target *)calloc(sim->volume_count + parts, sizeof(*grown));
    if (!grown)
        return Q2_ERR_NO_MEMORY;

    sim->mover_base     = (s32 *)calloc(parts * 6u, sizeof(s32));
    sim->mover_last_off = (s32 *)calloc(parts, sizeof(s32));
    if (!sim->mover_base || !sim->mover_last_off) {
        free(grown);
        free(sim->mover_base);
        free(sim->mover_last_off);
        sim->mover_base     = NULL;
        sim->mover_last_off = NULL;
        return Q2_ERR_NO_MEMORY;
    }

    /*
     * ENTITIES FIRST, then the volumes — 0x80053C58 walks the entity table and
     * then the volume table, and the order is not cosmetic: the sweep keeps the
     * LAST contact rather than the nearest.
     */
    if (sim->volumes && sim->volume_count)
        memcpy(grown + parts, sim->volumes,
               sim->volume_count * sizeof(*grown));
    free(sim->volumes);
    sim->volumes = grown;

    for (i = 0; i < sim->breakable_count; i++)
        if (sim->breakable[i].solid_target >= 0)
            sim->breakable[i].solid_target += (s32)parts;

    out = 0;
    for (i = 0; i < set->count; i++) {
        const q2_mover *m = &set->movers[i];

        if (!m->blocks_player)
            continue;

        for (p = 0; p < m->part_count && out < parts; p++) {
            q2_move_target *t = &sim->volumes[out];
            s32 *base = &sim->mover_base[out * 6u];
            int k;

            mover_part_box(scene, m->node[p], base, base + 3);

            /*
             * A CAGE LIFT IS HOLLOW. Its constructor makes two slabs out of
             * one box — a ceiling of `cage_top` and a floor of `cage_bottom`
             * (mover.h) — and registering the Scene node's whole bounding box
             * instead makes the cage solid through its middle, which is a lift
             * the player cannot get into.
             *
             * The slab is cut from the part's own box rather than from the
             * union of the group's, because the part IS the box the console
             * would have been handed: BASE1's cage is nodes 215 and 216, and
             * 216 is already the 239-unit ceiling while 215 spans the whole
             * interior down to the floor. Cutting each to its own end gives a
             * ceiling and a floor and leaves the ride open.
             *
             * Which end is which is decided by geometry, not by part order:
             * +Y is down, so the part sitting higher (smaller min[1]) is the
             * ceiling. A one-part cage keeps its floor, since that is the slab
             * a rider stands on.
             */
            if ((m->cage_top || m->cage_bottom) && m->part_count > 0) {
                s32 other[6];
                bool is_ceiling = true;

                if (m->part_count > 1) {
                    u32 q = (p == 0) ? 1u : 0u;
                    mover_part_box(scene, m->node[q], other, other + 3);
                    is_ceiling = (base[1] <= other[1]);
                }

                if (is_ceiling && m->cage_top)
                    base[4] = base[1] + (s32)m->cage_top;
                else if (!is_ceiling && m->cage_bottom)
                    base[1] = base[4] - (s32)m->cage_bottom;
            }

            for (k = 0; k < 3; k++) {
                t->min[k] = t->env_min[k] = base[k];
                t->max[k] = t->env_max[k] = base[3 + k];
            }
            Q2_DEBUG("mover %u part %u node %d box (%d,%d,%d)..(%d,%d,%d)",
                     i, p, m->node[p], base[0], base[1], base[2],
                     base[3], base[4], base[5]);

            t->kind   = Q2_MOVE_KIND_ENTITY;
            /* The MOVER's index, not the part's: a contact has to name a door,
             * and a door with two leaves is still one door to open. */
            t->id     = (s32)i;
            t->mask   = 0;
            t->dy     = 0;
            t->active = true;

            {
                /* Seeded with the SAME quantity `q2_sim_movers_update`
                 * differences: the Y displacement, which is `offset` on a
                 * vertical mover and zero on any other. */
                s32 disp[3];

                q2_mover_displacement(m, disp);
                sim->mover_last_off[out] =
                    (m->is_path || (m->axis < 3u ? m->axis : 1u) == 1u)
                    ? disp[1] : 0;
            }
            out++;
        }
    }

    sim->mover_count   = out;
    sim->volume_count += out;

    sim->move_world.targets = sim->volumes;
    sim->move_world.count   = sim->volume_count;
    return Q2_OK;
}

void q2_sim_movers_update(q2_sim *sim, const q2_mover_set *set)
{
    u32 i, p, out = 0;

    if (!sim || !sim->volumes || !sim->mover_count || !sim->mover_base || !set)
        return;

    for (i = 0; i < set->count; i++) {
        const q2_mover *m = &set->movers[i];

        if (!m->blocks_player)
            continue;

        for (p = 0; p < m->part_count && out < sim->mover_count; p++, out++) {
            q2_move_target *t    = &sim->volumes[out];
            const s32      *base = &sim->mover_base[out * 6u];
            u32 axis = m->axis < 3u ? m->axis : 1u;
            s32 disp[3];
            int k;

            /*
             * THE LIVE BOX translates — both corners by the same amount
             * (0x80051F08-0x80051F7C). Derived from the pristine box and the
             * mover's own accumulated displacement rather than integrated,
             * because the zone draw adds that same displacement to the same
             * node and the hull is not allowed to disagree with what is on
             * screen by so much as a unit.
             *
             * Three components rather than one, because a train has three
             * (mover.h). This used to add `m->offset` to `axis` alone, so the
             * one platform on the disc had a hull sunk into the floor under a
             * body that was somewhere else entirely.
             */
            q2_mover_displacement(m, disp);

            for (k = 0; k < 3; k++) {
                t->min[k] = base[k] + disp[k];
                t->max[k] = base[3 + k] + disp[k];
            }

            /*
             * THE ENVELOPE only ever grows — 0x80051FBC onward adds the delta
             * to ONE corner, chosen by its sign, so the box ends up covering
             * the whole of the travel rather than following it. It is what the
             * broad-phase gate at 0x80050CE0 reads, and testing the live box
             * there instead would only arm the entity sweep for a player who
             * is already inside the door.
             */
            for (k = 0; k < 3; k++) {
                if (t->min[k] < t->env_min[k]) t->env_min[k] = t->min[k];
                if (t->max[k] > t->env_max[k]) t->env_max[k] = t->max[k];
            }

            /*
             * And the frame's vertical motion, which q2_move_sweep_box takes as
             * `other_dy`. This is what lets a player ride a lift instead of
             * being left standing in the air as it goes up.
             *
             * A train has a vertical component whenever its path does, so it is
             * measured off the Y displacement rather than off `offset` — which
             * for a train is a distance along a diagonal and not a height. The
             * `mover_last_off` slot holds that displacement for it; for
             * everything else the two are the same number.
             */
            {
                s32 dy_now = m->is_path ? disp[1]
                                        : ((axis == 1u) ? m->offset : 0);

                t->dy = (s16)(dy_now - sim->mover_last_off[out]);
                sim->mover_last_off[out] = dy_now;
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* A moving volume carries, or crushes, the player -- 0x80046234/0x80051E74 */
/* ------------------------------------------------------------------------- */
bool q2_sim_mover_push(q2_sim *sim, const s32 step[3])
{
    q2_player *p;
    s16        delta[3];
    s32        saved_pos[3];
    s32        saved_node;
    int        k;

    if (!sim || !step)
        return true;

    p = &sim->player[sim->cur_player];
    for (k = 0; k < 3; k++)
        delta[k] = (s16)step[k];

    /* The retail helper is reached only after a level collision context has
     * been installed.  Keep the no-hull harness useful, though: without a
     * world there is nothing that can pin the carried player. */
    if (!sim->coll_ready) {
        for (k = 0; k < 3; k++)
            p->ent.pos[k] += delta[k];
    } else {
        /* 0x800462B8 saves these three words and 0x8004638C saves the cached
         * cell.  The failed move's contact state is deliberately NOT restored:
         * the console restores only this position/cell quartet. */
        for (k = 0; k < 3; k++)
            saved_pos[k] = p->ent.pos[k];
        saved_node = p->ent.node;

        /* 0x80046350 passes the {0, 0} mode at 0x800AE930 to 0x800456B0:
         * collision only, no grounded step, unstick or entity retry. */
        if (!q2_move_checked(&sim->coll, &p->ent, delta, 0, false, false,
                             NULL, NULL, NULL)) {
            for (k = 0; k < 3; k++)
                p->ent.pos[k] = saved_pos[k];
            p->ent.node = saved_node;
            return false;
        }
    }

    p->pos[0] = p->ent.pos[0];
    p->pos[1] = q2_sim_feet_y(p->ent.pos[1]);
    p->pos[2] = p->ent.pos[2];
    p->on_ground = (p->ent.flags & Q2_ENT_GROUNDED_MASK) != 0;
    sim->current_node = p->ent.node;
    return true;
}

q2_damage_result q2_sim_mover_crush(q2_sim *sim)
{
    return q2_sim_hurt_player(sim, NULL, Q2_MOVER_CRUSH_DAMAGE,
                               Q2_MOD_CRUSH, NULL);
}

/*
 * Turn the map's trigger volumes into the target list the sweep and the
 * contents query walk. This is the port's stand-in for 0x800C9114, and the
 * records are the same 36-byte TrigBounds triggers the original reads there —
 * `q2_move_target.mask` is the trigger's own `flags` halfword.
 */
static void build_volumes(q2_sim *sim)
{
    u32 i;

    free(sim->volumes);
    sim->volumes      = NULL;
    sim->volume_count = 0;
    /* The mover boxes live at the front of the same array, so rebuilding the
     * volumes discards them too; the caller re-attaches after a zone load. */
    free(sim->mover_base);
    free(sim->mover_last_off);
    sim->mover_base     = NULL;
    sim->mover_last_off = NULL;
    sim->mover_count    = 0;
    sim->breakable_solid_count = 0;

    {
        u32 b;
        for (b = 0; b < sim->breakable_count; b++)
            sim->breakable[b].solid_target = -1;
    }

    memset(&sim->move_world, 0, sizeof(sim->move_world));
    sim->move_world.half_extent = Q2_SWEEP_HALF_EXTENT;

    if (!sim->triggers_ready || sim->triggers.count == 0)
        return;

    sim->volumes = (q2_move_target *)calloc(sim->triggers.count,
                                            sizeof(*sim->volumes));
    if (!sim->volumes)
        return;

    for (i = 0; i < sim->triggers.count; i++) {
        q2_trigger t;
        int k;

        if (!q2_trigger_get(&sim->triggers, i, &t))
            continue;

        for (k = 0; k < 3; k++) {
            sim->volumes[i].min[k] = t.min[k];
            sim->volumes[i].max[k] = t.max[k];
        }
        sim->volumes[i].mask   = t.flags;
        sim->volumes[i].kind   = Q2_MOVE_KIND_VOLUME;
        sim->volumes[i].id     = (s32)i;
        sim->volumes[i].dy     = 0;
        sim->volumes[i].active = true;
    }

    sim->volume_count           = sim->triggers.count;
    sim->mover_count            = 0;
    sim->move_world.targets     = sim->volumes;
    sim->move_world.count       = sim->volume_count;

    /*
     * 0x8005553C: the mask an entity sweeps volumes with is 0x810 when its own
     * flag bit 0 is set and 0 otherwise. Which entities set that bit was not
     * traced, so the port leaves it at 0 — meaning volumes are queried for
     * contents but do not block movement. Set `sim->move_world.mask` to 0x810
     * to turn them solid once that is known.
     */
    sim->move_world.mask = 0;
}

void q2_sim_spawn(q2_sim *sim, const s32 pos[3], s32 yaw)
{
    if (!sim || !pos)
        return;

    sim->player[sim->cur_player].pos[0] = pos[0];
    sim->player[sim->cur_player].pos[1] = pos[1];
    sim->player[sim->cur_player].pos[2] = pos[2];

    sim->player[sim->cur_player].vel[0] = sim->player[sim->cur_player].vel[1] = sim->player[sim->cur_player].vel[2] = 0;
    sim->player[sim->cur_player].yaw    = yaw;
    sim->player[sim->cur_player].pitch  = 0;
    sim->player[sim->cur_player].roll   = 0;

    sim->player[sim->cur_player].wish[0] = sim->player[sim->cur_player].wish[1] = sim->player[sim->cur_player].wish[2] = 0;
    sim->player[sim->cur_player].pitch_rate = sim->player[sim->cur_player].yaw_rate = 0;

    sim->player[sim->cur_player].impulse[0] = sim->player[sim->cur_player].impulse[1] = sim->player[sim->cur_player].impulse[2] = 0;
    sim->player[sim->cur_player].impulse_armed = false;
    sim->player[sim->cur_player].jump_hold     = 0;

    sim->player[sim->cur_player].fall_value    = 0;
    sim->player[sim->cur_player].fall_time     = 0;
    sim->player[sim->cur_player].footstep_time = 0;
    sim->player[sim->cur_player].foot          = 0;

    /* The retail player allocator clears the client-side water state with the
     * rest of a new life.  Keeping it across q2_sim_spawn made a position
     * override (or respawn) inherit the old air counter and deadline, so a
     * player could take a drowning hit on the first frame in a pool. */
    sim->player[sim->cur_player].wade          = 0;
    sim->player[sim->cur_player].water_air     = 0;
    sim->player[sim->cur_player].water_next    = 0;
    sim->player[sim->cur_player].splash_time   = 0;
    sim->player[sim->cur_player].water_voice   = false;

    /*
     * The view's own state. A spawn that kept these would put the player into
     * the world already flinching from a hit taken in the last life, or with
     * the recentre half way through walking their pitch to zero.
     *
     * `prev_health`/`prev_armour` are seeded from the inventory rather than
     * zeroed, because update_pain diffs them: seeded at zero, the first tick
     * would read a 100-point rise and, worse, a later drop back to 100 would
     * read as damage.
     */
    sim->player[sim->cur_player].kick[0] = sim->player[sim->cur_player].kick[1] = sim->player[sim->cur_player].kick[2] = 0;
    sim->player[sim->cur_player].kick_time     = 0;
    sim->player[sim->cur_player].hurt_kick[0]  = sim->player[sim->cur_player].hurt_kick[1] = 0;
    sim->player[sim->cur_player].pain_time     = 0;
    sim->player[sim->cur_player].look_hist     = 0;
    sim->player[sim->cur_player].recentring    = false;
    sim->player[sim->cur_player].autocentre    = 0;
    sim->player[sim->cur_player].ent2_flags    = 0;
    sim->player[sim->cur_player].prev_health   = sim->combat.inv.health;
    sim->player[sim->cur_player].prev_armour   = sim->combat.inv.armour;

    sim->player[sim->cur_player].on_ground   = false;
    sim->player[sim->cur_player].crouching   = false;
    sim->player[sim->cur_player].view_height = Q2_VIEW_STAND;
    sim->player[sim->cur_player].ground_y    = pos[1];

    /*
     * The mover works in the ENTITY ORIGIN's frame, which sits Q2_EYE_BASE
     * above the feet — see the note on q2_sim_origin_y in sim.h. `pos` is a
     * StartPos, i.e. the feet, so it is lifted here and lowered again after
     * every move.
     */
    memset(&sim->player[sim->cur_player].ent, 0, sizeof(sim->player[sim->cur_player].ent));

    /*
     * 0x8006C1B0 — the slope limit, which nothing in this port had ever
     * written. The retail allocator memsets a fresh entity and then stores 2600
     * into entity+0x9C, so this line is the port's equivalent of that store and
     * belongs immediately after the memset for the same reason.
     *
     * Seven places read the field and every one of them was reading zero, which
     * is not a harmless default: it is "any surface facing even slightly upward
     * is a floor". A near-vertical wall declared ground, the jump believed it,
     * and the velocity clip's gate — `n[1] >= max_slope_ny` — was true for
     * every possible normal, so the clip had never once executed.
     *
     * The last_normal contact this same entity carries is the other half of
     * that gate; see the note in q2_move.
     */
    sim->player[sim->cur_player].ent.max_slope_ny = Q2_MAX_SLOPE_NY;

    /*
     * And the contact slot starts at the flat sentinel the mover resets it to
     * every tick, rather than at the memset's (0, 0, 0). A zero ny reads as a
     * perfectly vertical wall, so without this a player who spawns inside a
     * ladder volume gets one spurious tick of wall contact before the first
     * move overwrites it.
     */
    sim->player[sim->cur_player].ent.last_normal[0] = 0;
    sim->player[sim->cur_player].ent.last_normal[1] = 4096;
    sim->player[sim->cur_player].ent.last_normal[2] = 0;

    sim->player[sim->cur_player].ent.pos[0] = pos[0];
    sim->player[sim->cur_player].ent.pos[1] = q2_sim_origin_y(pos[1]);
    sim->player[sim->cur_player].ent.pos[2] = pos[2];

    /*
     * Locate the cell we spawned into. A spawn that lands outside every hull
     * leaves the cached cell at -1, which is what the original stores in a
     * fresh entity: the first move then pays for one brute-force sweep and
     * self-corrects (0x80044C74).
     */
    sim->player[sim->cur_player].ent.node = sim->coll_ready
        ? q2_coll_find_node(&sim->coll, sim->player[sim->cur_player].ent.pos, -1, true)
        : -1;
    sim->current_node = sim->player[sim->cur_player].ent.node;

    /* Clear the entered-set so a spawn inside a volume fires it, rather than
     * being treated as "already inside". */
    if (sim->trigger_inside && sim->trigger_capacity)
        memset(sim->trigger_inside, 0, sim->trigger_capacity);
}

/*
 * 0x800458A0 — query the volumes the entity is standing in and turn the answer
 * into its own flags.
 *
 * Transcribed from 0x800458B4…0x80045970. Three of the mask's bits are
 * understood; the query passes the whole 0x3360 because the original does, and
 * the bits nothing consumes yet are simply not acted on.
 */
static void update_contents(q2_sim *sim)
{
    q2_player *p = &sim->player[sim->cur_player];
    u16 contents;

    if (!sim->volume_count) {
        p->ent.flags &= ~(Q2_ENT_LIQUID_SINK | Q2_ENT_LIQUID_FLOAT | 0x800u);
        return;
    }

    contents = q2_move_contents(&sim->move_world, p->ent.pos, Q2_CONTENTS_MASK);

    /*
     * 0x800458B4: inside a 0x1000 volume, flag 0x800 is set when the entity's
     * last contact normal is nearly horizontal — `-1023 <= ny < 1024` on
     * ent+0x14, which is the |ny|-MINIMISING normal, not the velocity. So the
     * flag means "in this volume AND touching a wall rather than a floor",
     * which is a ladder-shaped condition.
     */
    if (contents & 0x1000u) {
        s32 ny = p->ent.last_normal[1];

        if (ny < 1024 && ny >= -1023)
            p->ent.flags |= 0x800u;
        else
            p->ent.flags &= ~0x800u;
    } else {
        p->ent.flags &= ~0x800u;
    }

    /* 0x800458F8: 0x0200 — sinks slowly. */
    if (contents & 0x0200u)
        p->ent.flags |= Q2_ENT_LIQUID_SINK;
    else
        p->ent.flags &= ~Q2_ENT_LIQUID_SINK;

    /*
     * 0x80045920: 0x2000 — buoyant, and boosted when already on the ground.
     *
     * The boost is POSTED, not written:
     *
     *     80045940  lhu   v0, 762(s0)        ; the impulse's Y, loaded
     *     80045948  addiu v0, v0, -9216      ; accumulated onto
     *     8004594C  ori   v1, v1, 0x4000     ; and the accumulator armed
     *     80045950  sh    v0, 762(s0)
     *
     * `lhu` then `addiu` means it ADDS to whatever the jump already posted, and
     * going through the accumulator is what subjects it to the -3072 ceiling at
     * 0x80046148. Written straight into velocity, as it was here, it bypassed
     * that clamp entirely and launched the player out of shallow water at three
     * times the height a jump can reach. It also has to stay after the jump in
     * the frame — the jump ASSIGNS (0x8003E1EC, no load), so -3072 then -9216
     * sums to -12288 and is then clamped back to -3072, which is why standing
     * in water and jumping is not a super jump on the console.
     */
    if (contents & 0x2000u) {
        if (p->ent.flags & Q2_ENT_GROUNDED_MASK) {
            p->impulse[1]    = (s16)(p->impulse[1] + Q2_LIQUID_BOOST);
            p->impulse_armed = true;
        }
        p->ent.flags |= Q2_ENT_LIQUID_FLOAT;
    } else {
        p->ent.flags &= ~Q2_ENT_LIQUID_FLOAT;
    }
}

/* ------------------------------------------------------------------------- */
/* The player's own frame — 0x8003A1C8                                        */
/* ------------------------------------------------------------------------- */
/*
 * Everything below is one function in the original, transcribed in its own
 * order because the order is load-bearing: the view offset is chosen from LAST
 * frame's environment flags, the jump reads the ground the previous move left,
 * and the look angles integrate before the wish velocity is rotated by them.
 */

/*
 * 0x8003A208 — the view offset target, and the environment flag reset.
 *
 * Read before the flags are cleared, so the height follows the volume the
 * player was in on the previous tick. That one-tick lag is visible when you
 * walk out of a crouch zone and is not worth "fixing".
 */
static void update_view_offset(q2_sim *sim, s32 dt)
{
    q2_player *p = &sim->player[sim->cur_player];
    s32 target;
    s16 v;

    if (p->ent.flags & Q2_ENT_INCROUCH)
        target = Q2_VIEW_MID;
    else if (p->ent.flags & Q2_ENT_INLOWCROUCH)
        target = Q2_VIEW_CROUCH;
    else
        target = Q2_VIEW_STAND;

    v = (s16)p->view_height;
    ease16(&v, target, dt * Q2_VIEW_EASE_RATE);
    p->view_height = v;

    p->crouching = (p->ent.flags & (Q2_ENT_INCROUCH | Q2_ENT_INLOWCROUCH)) != 0;
}

/*
 * 0x8003A25C — clear the environment flags, then let the volumes re-assert them.
 *
 * The original clears them and calls the volume dispatcher at 0x80027E64, whose
 * INWATER / UNDERWATER / INCROUCH / INLOWCROUCH events do nothing but OR a bit
 * back in. `sim->env_flags` stands in for that dispatch until the port can
 * resolve a volume's event to its UserFuncs primitive.
 */
static void update_env_flags(q2_sim *sim)
{
    u32 env = sim->env_flags;
    u32 i;
    s32 at[3];

    /*
     * THE ORIGIN, NOT THE FEET — 0x80027F0C.
     *
     * The dispatcher at 0x80027E64 has exactly one caller and it passes
     * `entity+0x54`, the entity origin, into the point-in-box test at
     * 0x80044098. This queried `p->pos`, which is the FEET, 286 units lower.
     *
     * `update_triggers` had already been corrected to sample the origin (see
     * the note there: of the volumes measured, most do not contain the feet at
     * all), and this was the other half of the same query left behind. It is why
     * water volumes mostly failed to assert INWATER/UNDERWATER, which in turn
     * left the underwater wish rates, swimming up, the water-exit jump and the
     * weightless integrator arm all dormant.
     *
     * Built from `p->pos` rather than `p->ent.pos` deliberately: this runs
     * before the tick refreshes `p->ent.pos` from `p->pos`, and the
     * no-collision fallback advances `p->pos` while leaving `p->ent.pos` stale.
     */
    at[0] = sim->player[sim->cur_player].pos[0];
    at[1] = q2_sim_origin_y(sim->player[sim->cur_player].pos[1]);
    at[2] = sim->player[sim->cur_player].pos[2];

    /*
     * Level-triggered, not edge-triggered, and that is the whole point: these
     * bits describe where the player IS. update_triggers' entered-set is for
     * scripts that must fire once; this must be re-asserted every tick or the
     * player stands up in the middle of a crouch tunnel.
     *
     * Tested against the position the previous tick's move left, because the
     * dispatcher runs at the top of the frame — which is also why walking out
     * of a crouch zone takes one tick to stand up.
     */
    if (sim->volume_env && sim->triggers_ready) {
        for (i = 0; i < sim->triggers.count; i++) {
            bool hazard = sim->volume_damage && sim->volume_damage[i] != 0;

            if (!sim->volume_env[i] && !hazard)
                continue;
            if (!q2_trigger_contains(&sim->triggers, i, at))
                continue;

            env |= sim->volume_env[i];

            /*
             * And the damage, on the same pass and for the same reason: it
             * describes where the player IS. The amount and the mod were read
             * at attach (record_hazard); the RATE is the damage function's own
             * per-target throttle, which is what stops twenty points of lava
             * three hundred times a second.
             *
             * Nobody is the attacker. Lava is not a frag.
             */
            if (hazard) {
                q2_damage_result dr =
                    q2_sim_hurt_player(sim, NULL, sim->volume_damage[i],
                                       sim->volume_mod[i],
                                       sim->player[sim->cur_player].ent.pos);

                if (!dr.blocked)
                    sim->hazard_hits++;
            }
        }
    }

    sim->player[sim->cur_player].ent.flags &= ~(u32)Q2_ENT_ENV_MASK;
    sim->player[sim->cur_player].ent.flags |= env & Q2_ENT_ENV_MASK;
}

/* 0x8003A3B0 — max speed. Three cases in the original's order. */
static s32 max_speed(u32 flags)
{
    if (flags & Q2_ENT_INLOWCROUCH)
        return Q2_SPEED_LOWCROUCH;
    if (flags & (Q2_ENT_INWATER | Q2_ENT_UNDERWATER | Q2_ENT_INCROUCH))
        return Q2_SPEED_WET;
    return Q2_SPEED_NORMAL;
}

/*
 * 0x8003A4EC — the water-exit jump.
 *
 * Having been UNDERWATER last tick and not being now, with the last contact not
 * flat enough to be a floor, synthesises a jump press and hands the entity a
 * flat ground normal so the jump's own grounded test passes. It is what lifts
 * you out of a pool onto its edge; without it you swim into the wall forever.
 */
static void water_exit_jump(q2_sim *sim, bool was_underwater, u32 *buttons)
{
    q2_player *p = &sim->player[sim->cur_player];

    if (!was_underwater)
        return;
    if (p->ent.flags & (Q2_ENT_UNDERWATER | Q2_ENT_WALL_CONTACT))
        return;
    if (p->ent.last_normal[1] >= 2048)
        return;
    /* 0x8003A544: not while flying — a weightless entity has nothing to climb
     * out of. Last of the five tests, and the only one that is not about the
     * water itself. */
    if (p->ent2_flags & Q2_ENT2_FLY)
        return;

    *buttons |= Q2_BTN_JUMP;
    p->ent.flags &= ~(u32)Q2_ENT_JUMP_LATCH;
    p->jump_hold  = 0;

    p->ent.ground_normal[0] = 0;
    p->ent.ground_normal[1] = 4096;   /* 0x800AE928: (0, 4096, 0), flat floor */
    p->ent.ground_normal[2] = 0;
}

/*
 * 0x8003A584 — the wish velocity.
 *
 * One clamped approach per horizontal axis toward `(maxspeed * axis) >> 7`, so
 * the stick asks for a SPEED and the rate limits how fast that speed is
 * reached. The rate is the only thing the environment changes, and the two
 * underwater values are asymmetric on purpose: 30 while the stick is off centre
 * and 14 while it is centred, so swimming drifts.
 */
static void update_wish(q2_sim *sim, const q2_input *in, s32 dt)
{
    q2_player *p     = &sim->player[sim->cur_player];
    s32        speed = max_speed(p->ent.flags);
    bool       moving;
    s32        rate;

    /* 0x8003A4F8: the two axes are OR'd as unsigned halfwords, so any non-zero
     * deflection on either counts — including one that cancels the other. */
    moving = ((u16)in->forward | (u16)in->side) != 0;

    if (p->ent.flags & Q2_ENT_UNDERWATER)
        rate = moving ? Q2_WISH_RATE_SWIM : Q2_WISH_RATE_DRIFT;
    else
        rate = Q2_WISH_RATE;

    rate *= dt;

    /*
     * The >>7 is applied to the 32-bit product and then narrowed, exactly as
     * `sll 9 / sra 16` does: bits above 22 of the product are discarded rather
     * than saturated.
     */
    ease16(&p->wish[2], (s16)(((s32)speed * in->forward) >> Q2_WISH_SHIFT), rate);
    ease16(&p->wish[0], (s16)(((s32)speed * in->side)    >> Q2_WISH_SHIFT), rate);
}

/*
 * 0x8003A658 — the look rates, and 0x8003A990 — the angles.
 *
 * The stick does not move the view. It asks for a TURN RATE, that rate is eased,
 * and the angle integrates from it — which is why the PSX view keeps gliding for
 * a moment after the stick is released, and why a port that adds the stick
 * straight to the angle feels wrong however the sensitivity is tuned.
 *
 * The ease rate is dt normally and dt*4 when the stick opposes the current rate
 * or has returned to centre, so reversing and stopping are both four times
 * sharper than starting.
 */
static void update_look(q2_sim *sim, const q2_input *in, s32 dt)
{
    q2_player *p = &sim->player[sim->cur_player];
    s32 want_pitch = ((s32)in->pitch * Q2_LOOK_SCALE_NUM) >> Q2_LOOK_SCALE_SHIFT;
    s32 want_yaw   = ((s32)in->yaw   * Q2_LOOK_SCALE_NUM) >> Q2_LOOK_SCALE_SHIFT;

    /*
     * 0x8003A670 — `slti v0, style, 6` branching to the EASED arm when the test
     * fails. So the six mouse and stick styles set the rate outright and the
     * three STANDARD ones ease it, which is the right way round for the input
     * each is reading: a stick already carries a proportional rate, a button
     * only carries "now".
     *
     * This comparison used to be inverted here, which made the analogue styles
     * glide after the stick was released and the digital ones snap.
     */
    if (p->look_scheme < Q2_LOOK_SCHEME_ANALOGUE) {
        /* 0x8003A67C. */
        p->pitch_rate = (s16)want_pitch;
        p->yaw_rate   = (s16)want_yaw;
    } else {
        /* 0x8003A6B0: pitch snaps when the product of rate and input is
         * negative, i.e. when they disagree. */
        s32 pr = ((s32)p->pitch_rate * in->pitch < 0)
               ? dt * Q2_LOOK_RATE_SNAP : dt * Q2_LOOK_RATE;
        ease16(&p->pitch_rate, want_pitch, pr);

        /*
         * 0x8003A6F8: yaw is spelled out as a sign comparison rather than a
         * product, and a centred stick takes the snap rate with a target of
         * zero — which is the only reason letting go stops you at all.
         */
        if (in->yaw > 0) {
            ease16(&p->yaw_rate, want_yaw,
                   (p->yaw_rate < 0) ? dt * Q2_LOOK_RATE_SNAP : dt * Q2_LOOK_RATE);
        } else if (in->yaw < 0) {
            ease16(&p->yaw_rate, want_yaw,
                   (p->yaw_rate > 0) ? dt * Q2_LOOK_RATE_SNAP : dt * Q2_LOOK_RATE);
        } else {
            ease16(&p->yaw_rate, 0, dt * Q2_LOOK_RATE_SNAP);
        }
    }
}

/*
 * 0x8003A780 — the view recentre.
 *
 * TWO things arm this, and the note here used to claim only one did.
 *
 * The first is a CHORD: hold both look buttons together and the pitch walks
 * back to zero. The second is the AUTOCENTRE row on the player page, which
 * this said "does not do this" — it does, through the timer at client+0x26.
 * 0x8003A4AC counts dt while the forward axis is deflected and 0x8003A984 sets
 * the same `recentring` flag once that reaches 200, i.e. two thirds of a second
 * of walking, on the ground, in single player. Both arms end here.
 *
 * The arming is a three-tick shift register rather than a flag, and the exact
 * sequence matters. Frame one of the chord sets history bit 0, `(hist & 7)` is
 * 1 so the arm is set — and then `(hist & 3) == 1` immediately cancels it.
 * Frame two has history 0b11, so it arms and is not cancelled. From frame three
 * `(hist & 7) == 7` and the arm is no longer re-asserted, but it is already set
 * and stays set until the pitch reaches zero. So the chord needs a second tick
 * to take, and a single-tick tap does nothing — which is what stops a scheme
 * that maps both look buttons to one control from recentring constantly.
 *
 * `s0` in the original is "the look rate was SET rather than eased", so the
 * decay in the else arm is exactly the styles that ease.
 */
static void update_recentre(q2_sim *sim, const q2_input *in, s32 dt)
{
    q2_player *p = &sim->player[sim->cur_player];
    u32 look_mask = Q2_BTN_LOOK_DOWN | Q2_BTN_LOOK_UP;
    bool eased = p->look_scheme >= Q2_LOOK_SCHEME_ANALOGUE;

    /* 0x8003A780: shift, then set bit 0 when either look button is held. */
    p->look_hist = (u8)(p->look_hist << 1);
    if (in->buttons & look_mask)
        p->look_hist |= 1u;

    /* 0x8003A7A8: both held, and not already held for three ticks. */
    if ((in->buttons & look_mask) == look_mask && (p->look_hist & 7u) != 7u)
        p->recentring = true;

    /* 0x8003A7D4: the first tick of any press cancels it again. */
    if ((p->look_hist & 3u) == 1u)
        p->recentring = false;

    if (in->pitch != 0) {
        /* 0x8003A8AC: looking at all cancels the AUTOCENTRE timer. */
        p->autocentre = 0;
        return;
    }

    if (p->recentring &&
        !(p->ent2_flags & Q2_ENT2_FLY) &&
        !(p->ent.flags & Q2_ENT_WALL_CONTACT)) {
        /*
         * 0x8003A838. Two stages, and both are needed: a geometric 5/6 that
         * makes a large pitch fall away fast, then the same clamped approach
         * everything else uses so the last few units actually reach zero
         * rather than converging on it.
         */
        s16 v;

        p->pitch = ((s32)(s16)p->pitch * Q2_LOOK_CENTRE_NUM) / Q2_LOOK_CENTRE_DEN;

        v = (s16)p->pitch;
        ease16(&v, 0, dt);
        p->pitch = v;

        p->pitch_rate = 0;

        if (p->pitch == 0)
            p->recentring = false;
        return;
    }

    /*
     * 0x8003A88C. With the pitch stick centred and nothing to recentre, the
     * eased styles bleed their pitch rate at dt*4 — four times what
     * update_look's own zero-target approach uses, and the reason letting go of
     * the look button stops the view rather than coasting it.
     */
    if (eased)
        ease16(&p->pitch_rate, 0, dt * Q2_LOOK_RATE_SNAP);
}

/* 0x8003A990 — integrate the angles from the rates and clamp the pitch. */
static void integrate_look(q2_sim *sim, s32 dt)
{
    q2_player *p = &sim->player[sim->cur_player];

    p->pitch = (s16)(p->pitch + ((s32)p->pitch_rate * dt) / Q2_LOOK_DIV);
    p->yaw   = (s16)(p->yaw   + ((s32)p->yaw_rate   * dt) / Q2_LOOK_DIV);

    /*
     * +-1024 is +-90 degrees in the 4096-step circle. Hitting the clamp also
     * zeroes the rate (0x8003AA1C / 0x8003AA38), so the view stops dead at
     * straight up rather than pressing against the limit.
     */
    if (p->pitch < -Q2_PITCH_LIMIT) {
        p->pitch      = -Q2_PITCH_LIMIT;
        p->pitch_rate = 0;
    }
    if (p->pitch > Q2_PITCH_LIMIT) {
        p->pitch      = Q2_PITCH_LIMIT;
        p->pitch_rate = 0;
    }
}

/*
 * 0x8003E110 — the jump.
 *
 * It does not touch velocity. It posts an impulse and arms the accumulator, and
 * the integrator applies it later in the same frame — which is why a jump has no
 * frame of delay even though the position update runs on the previous frame's
 * delta.
 *
 * `jump_hold` is not a hold duration. It blocks a repeat, and it is cancelled
 * the moment vertical velocity turns downward, so on flat ground you can jump
 * again as soon as you start falling rather than after 576 ticks.
 */
static bool player_jump(q2_sim *sim, bool pressed, s32 dt)
{
    q2_player *p = &sim->player[sim->cur_player];
    bool grounded;

    if (p->jump_hold != 0) {
        if (p->vel[1] >= 0 || p->jump_hold < dt)
            p->jump_hold = 0;
        else
            p->jump_hold = (s16)(p->jump_hold - dt);
        return false;
    }

    if (!pressed)
        return false;

    /*
     * 0x8003E198. Nothing in the image ever sets this bit — the player's own
     * frame clears it and no store to entity+0x98 ORs it in — so the gate is
     * dead in practice. It is transcribed rather than dropped because the
     * water-exit path takes the trouble to clear it, which only makes sense if
     * Hammerhead meant it to work.
     */
    if (p->ent.flags & Q2_ENT_JUMP_LATCH)
        return false;

    /* 0x8003E1AC: the same test the drop move uses to declare ground, run
     * against the normal that move left behind. */
    grounded = p->ent.max_slope_ny < p->ent.ground_normal[1];

    if (!grounded && !(p->ent.flags & Q2_ENT_WALL_CONTACT))
        return false;

    /*
     * 0x8003E1D0 — pushing off a wall. The horizontal impulse is the reversed
     * contact normal at full 1.3.12 scale, so a wall jump throws you away from
     * the surface as hard as it throws you up.
     */
    if (p->ent.flags & Q2_ENT_WALL_CONTACT) {
        p->impulse[0] = (s16)(-p->ent.last_normal[0]);
        p->impulse[2] = (s16)(-p->ent.last_normal[2]);
    }

    p->impulse[1]     = Q2_JUMP_IMPULSE;
    p->jump_hold      = Q2_JUMP_HOLD;
    p->impulse_armed  = true;

    /*
     * 0x8003E208 and 0x8003E214 — the two things the jump does after arming the
     * impulse, and the port did neither.
     *
     * The noise first: 0x80062B80 is PlayerNoise, and the jump passes type 0,
     * which 0x80062C68's `sltiu v0, s4, 2` routes into the FIRST pair —
     * level.sound_entity at 0x800E46EC, the one FindTarget honours even for an
     * ambush creature. Then the sound, 0x800B2900, at the entity.
     *
     * Both sit past every rejecting branch: 0x8003E198, 0x8003E1BC and the
     * hold arms all jump to 0x8003E224, which returns zero without reaching
     * here. So a gated jump stays silent, which is what makes the sound a
     * usable signal that the press was actually taken.
     *
     * The queue carries the sound; the client's drain raises the noise beside
     * it, because the creature world is the client's to reach.
     */
    q2_ent_sound_at(&sim->ent_world.events, Q2_SND_JUMP, p->pos);
    return true;
}

/*
 * 0x8003ABC4 — turn the wish velocity into world velocity.
 *
 * Three paths, and which one runs decides whether looking up makes you move up:
 *
 *   - Touching a wall inside a ladder volume: the full view basis, applied
 *     instantly, plus a fixed push along the contact normal (0x8006EFDC).
 *   - The full basis without the push, when the GAME VARIABLES setting is on or
 *     when swimming with the stick off centre (0x8006F11C). Instant, no ease.
 *   - Otherwise the yaw-only rotation, eased into the current velocity
 *     (0x8006FF08 + two 0x8006FE3C calls). This is the normal walking path, and
 *     the ease is why you keep moving briefly after letting go.
 */
static void wish_to_world(q2_sim *sim, bool moving, s32 dt)
{
    q2_player *p = &sim->player[sim->cur_player];

    /* 0x8006EFDC / 0x8006F11C both rotate by the basis matrix RotMatrix built
     * from the view angles, and both negate the Y row because +Y is down. The
     * port rebuilds the same rotation from the angles directly. */
    if ((p->ent.flags & Q2_ENT_WALL_CONTACT) ||
        sim->full_basis_movement ||
        ((p->ent.flags & Q2_ENT_UNDERWATER) && moving)) {
        s32 fwd = p->wish[2], side = p->wish[0];
        s16 m[3][3];
        s32 vx, vy, vz;

        /*
         * THE BASIS IS THE ROLLED ONE, and this used to drop the roll.
         *
         *     8003AB38  sh    v0, 234(s3)        ; roll -> entity+0xEA
         *     8003AB40  addiu a0, s3, 230        ; a0 = &entity.angles
         *     8003AB60  jal   0x80089E38         ; RotMatrix
         *     8003AB64  addiu a1, s3, 704        ; -> entity+0x2C0
         *
         * The strafe roll is stored into the angle triple immediately before
         * RotMatrix reads it (angle.z at 0x80089F08), and the matrix it leaves
         * at entity+0x2C0 is the one 0x8006EFDC and 0x8006F11C rotate the wish
         * by. So the roll is in the movement basis, not only in the camera —
         * strafing tips the ladder and swimming axes very slightly, which the
         * hand-rolled yaw/pitch block here could not express at all.
         *
         * ON THE PITCH SIGN, which an audit flagged and which is NOT a bug:
         * retail's RotMatrix puts -sin(pitch) in m12 and 0x8006F220 then negates
         * the summed middle row, so retail's forward carries +sin(pitch_retail).
         * This port's pitch runs the other way throughout — the camera's own
         * forward axis is `q2_rotation_yaw_pitch`'s row 2, whose Y is -sin, and
         * `q2_sim_aim` is -sin to match. Movement, camera and aim therefore all
         * agree with each other, which is the only invariant that matters:
         * forward goes where you are looking. Flipping the sign here alone would
         * send you DOWN a ladder while looking up. So the port's angle is
         * negated into retail's convention on the way in and the whole retail
         * form is used from there, which reduces exactly to the old expression
         * when roll is zero.
         */
        q2_rotation_euler(m, -(s32)p->pitch, p->yaw, p->roll);

        /* 0x8006F200/0x8006F220: each row is summed, shifted, and only then is
         * the middle one negated — one LSB from negating the product instead. */
        vx =  ((s32)m[0][0] * side + (s32)m[0][1] * p->wish[1] +
               (s32)m[0][2] * fwd) >> Q2_FRAC_12;
        vy = -(((s32)m[1][0] * side + (s32)m[1][1] * p->wish[1] +
                (s32)m[1][2] * fwd) >> Q2_FRAC_12);
        vz =  ((s32)m[2][0] * side + (s32)m[2][1] * p->wish[1] +
               (s32)m[2][2] * fwd) >> Q2_FRAC_12;

        if (p->ent.flags & Q2_ENT_WALL_CONTACT) {
            /* 0x8006F0E4: the normal is added at >>5, i.e. 128 units at full
             * 1.3.12 scale. */
            vx += p->ent.last_normal[0] >> 5;
            vz += p->ent.last_normal[2] >> 5;
        }

        p->vel[0] = (s16)vx;
        p->vel[1] = (s16)vy;
        p->vel[2] = (s16)vz;
    } else {
        s32 sy = q2_sin12(p->yaw), cy = q2_cos12(p->yaw);
        s32 fwd = p->wish[2], side = p->wish[0];

        /* 0x8006FF08. Only X and Z are written; Y is left to gravity. */
        s32 wx = (cy * side + sy * fwd) >> Q2_FRAC_12;
        s32 wz = (cy * fwd  - sy * side) >> Q2_FRAC_12;

        s32 rate = ((p->ent.flags & Q2_ENT_UNDERWATER) && !moving)
                 ? dt * Q2_VEL_RATE_SLOW : dt * Q2_VEL_RATE;

        p->vel[0] = ease32(p->vel[0], wx, rate);
        p->vel[2] = ease32(p->vel[2], wz, rate);
    }

    /*
     * 0x8003AD0C — the ground stick.
     *
     * A downward bias of 768 whenever the player is asking to move, or whenever
     * they are submerged with the full-basis setting off. Combined with the
     * step-down in the mover it is what keeps you glued to a floor across a ledge
     * or a slope instead of taking off; drop it and walking downhill becomes a
     * series of small jumps.
     *
     * The condition is spelled out here rather than folded into the branch above,
     * because it is NOT the same predicate: 0x8003AD1C tests UNDERWATER on its
     * own, whereas the branch above tests UNDERWATER *and* moving.
     */
    if (p->ent.flags & Q2_ENT_WALL_CONTACT)
        return;

    if (moving ||
        ((p->ent.flags & Q2_ENT_UNDERWATER) && !sim->full_basis_movement))
        p->vel[1] = ease32(p->vel[1], Q2_GROUND_STICK_VY,
                           dt * Q2_GROUND_STICK_RATE);
}

/* 0x8003AB98 — swimming up. Only while fully submerged, and only up to -768. */
static void swim_up(q2_sim *sim, const q2_input *in, s32 dt)
{
    q2_player *p = &sim->player[sim->cur_player];

    if (!(p->ent.flags & Q2_ENT_UNDERWATER))
        return;
    if (!(in->buttons & Q2_BTN_SWIM_UP))
        return;
    if (p->vel[1] < Q2_SWIM_UP_VY)
        return;

    p->vel[1] = ease32(p->vel[1], Q2_SWIM_UP_VY, dt * Q2_SWIM_UP_RATE);
}

/* 0x8003AB18 — the strafe roll. Derived from the wish, not from the input, so it
 * decays with the same ease everything else uses. Skipped entirely when the
 * entity carries entity+0x10C bit 19, which nothing on the disc sets. */
static void update_roll(q2_sim *sim)
{
    if (sim->player[sim->cur_player].ent2_flags & 0x80000u)
        return;
    sim->player[sim->cur_player].roll = -(s32)sim->player[sim->cur_player].wish[0] / Q2_ROLL_DIV;
}

/*
 * 0x800459E8 — project the velocity onto the ground plane.
 *
 * This is the rule that was missing from the port, and it is the one that makes
 * standing still work. At the TOP of the mover, before any move runs, an entity
 * that was on the ground last frame has its vertical velocity REPLACED by
 * whatever makes the velocity vector parallel to the surface it is standing on:
 *
 *     vel.y = -(vel.x*n.x + vel.z*n.z) / n.y
 *
 * On flat ground n is (0, 4096, 0) and that is simply zero, which is why gravity
 * does not accumulate while you stand there. On a slope it is the vertical
 * component needed to follow the surface, which is why you walk down a ramp
 * instead of stepping off it and re-landing.
 *
 * It is also what drives fall damage. The mover itself never clears velocity on
 * contact, so a landing leaves the full falling speed in place; it is THIS, on
 * the following tick, that snaps it to the surface — and 0x80039AA4 measures the
 * difference. Fall damage therefore lands one tick after the impact, which is
 * exactly what the console does.
 *
 * The exclusions are as important as the rule: not on a ladder, not underwater,
 * not with flag 0x80 — all three of which need to keep a velocity that is not
 * parallel to anything.
 */
static void ground_project(q2_sim *sim)
{
    q2_player *p = &sim->player[sim->cur_player];
    s32 ny = p->ent.ground_normal[1];

    if (p->ent.max_slope_ny >= ny)
        return;
    if (!(p->ent.flags & (Q2_ENT_ON_GROUND | Q2_ENT_ON_ENTITY | 0x2000u)))
        return;
    if (p->ent.flags & (Q2_ENT_WALL_CONTACT | Q2_ENT_UNDERWATER |
                        Q2_ENT_NO_FOOTSTEP))
        return;
    if (ny == 0)
        return;                 /* the original would trap; it cannot happen */

    p->vel[1] = (s16)(-((s32)(s16)p->vel[0] * p->ent.ground_normal[0] +
                        (s32)(s16)p->vel[2] * p->ent.ground_normal[2]) / ny);
}

/*
 * 0x80045E74..0x80046200 — the integrator, and the delta the NEXT frame moves by.
 *
 * The single most surprising thing about this engine's physics is the order. The
 * three moves at 0x80045B24 consume `entity+0xEC`, and `entity+0xEC` is written
 * HERE, at the end of the same function. So a frame moves by the delta the
 * previous frame computed: velocity and position are permanently one tick apart.
 * Reproducing that is not pedantry — it is a tick of input latency and a tick of
 * gravity, and jump arcs land in different places without it.
 *
 * There are two arms, and which one runs decides whether gravity exists at all:
 *
 *   arm 1, no gravity — UNDERWATER (or flag 0x80), or `entity+0x10C & 0x1000`,
 *                       or on a ladder with no impulse pending. Applies the
 *                       impulse unconditionally and drops the ground flags.
 *   arm 2, gravity    — everything else. Applies the impulse only when armed.
 *
 * So swimming and climbing are weightless, which is why neither needs a special
 * case anywhere else.
 */
static void integrate_vertical(q2_sim *sim, s32 dt, s16 out_delta[3])
{
    q2_player *p  = &sim->player[sim->cur_player];
    s32        g  = sim->gravity ? sim->gravity : Q2_GRAVITY;
    s32        dv = g * dt;
    s32        dvdt;
    s32        half;
    bool       no_gravity;

    /* 0x80045E74: the horizontal deltas are computed for both arms, before any
     * impulse, and only the gravity arm's re-derivation can replace them. */
    out_delta[0] = (s16)(((s32)(s16)p->vel[0] * dt) / Q2_VEL_DIV);
    out_delta[2] = (s16)(((s32)(s16)p->vel[2] * dt) / Q2_VEL_DIV);

    /*
     * 0x80045EE0..0x80045F0C, in the original's own short-circuit order.
     *
     * The FIRST test is entity+0x10C bit 12, which this transcription described
     * and did not implement — so the fly cheat steered movement by the view
     * basis while still falling. It is checked before the +0x98 word because
     * that is the order the branches sit in, and being first is what lets it
     * override the liquid and ladder arms rather than compete with them.
     */
    no_gravity = (p->ent2_flags & Q2_ENT2_FLY) != 0
              || (p->ent.flags & (Q2_ENT_UNDERWATER | Q2_ENT_NO_FOOTSTEP)) != 0
              || ((p->ent.flags & Q2_ENT_WALL_CONTACT) && !p->impulse_armed);

    if (no_gravity) {
        /* 0x80045F14. The vertical delta comes from the velocity BEFORE the
         * impulse, so a jump off a ladder shows up on the following frame. */
        out_delta[1] = (s16)(((s32)(s16)p->vel[1] * dt) / Q2_VEL_DIV);

        p->ent.flags &= ~(u32)(Q2_ENT_ON_GROUND | Q2_ENT_ON_ENTITY);
        p->on_ground  = false;

        p->vel[0] = (s16)(p->vel[0] + p->impulse[0]);
        p->vel[1] = (s16)(p->vel[1] + p->impulse[1]);
        p->vel[2] = (s16)(p->vel[2] + p->impulse[2]);

        /*
         * The three halfwords are zeroed at 0x80045F44..0x80045F4C, and the arm
         * bit is NOT. The whole weightless arm, 0x80045F14..0x80045FA0, contains
         * exactly one word store — 0x80045F68 to entity+0x98 — and never touches
         * entity+0x10C; the 0x4000 bit's only clear site in the function is
         * 0x80046118, down in the gravity arm.
         *
         * Clearing it here disarmed the accumulator a frame early, so an impulse
         * posted while weightless — a jump off a ladder, the buoyancy kick on
         * leaving water — never reached 0x80046094's block on the following
         * frame and the entity kept its ground flags and its ceiling clamp.
         */
        p->impulse[0] = p->impulse[1] = p->impulse[2] = 0;
        return;
    }

    /*
     * 0x80045FA4. (dv*dt)/2 carries the sign-bit bias the compiler emitted at
     * 0x80045FE0, which makes the halving truncate toward zero and not -inf.
     */
    dvdt = dv * dt;
    half = (dvdt + (s32)(((u32)dvdt) >> 31)) >> 1;

    out_delta[1] = (s16)((((s32)(s16)p->vel[1] * dt) + half) / Q2_VEL_DIV);
    p->vel[1]    = (s16)(p->vel[1] + dv);

    /*
     * The three vertical rules, mutually exclusive and in the original's order.
     * The 0x08 arm is the one an earlier pass had inverted: 0x80046050 branches
     * on `vel.y < 1024` into the `-= dt*24` arm, so a 0x0200 volume pushes you
     * UP until you are falling faster than 1024 and only then caps the fall.
     */
    if (p->ent.flags & Q2_ENT_LIQUID_FLOAT) {
        p->vel[1] = ease32(p->vel[1], Q2_LIQUID_FLOAT_VY,
                           dt * Q2_LIQUID_EASE_RATE);
    } else if (p->ent.flags & Q2_ENT_LIQUID_SINK) {
        if ((s16)p->vel[1] < Q2_LIQUID_SINK_VY)
            p->vel[1] = (s16)(p->vel[1] - dt * Q2_LIQUID_SINK_PUSH);
        else
            p->vel[1] = ease32(p->vel[1], Q2_LIQUID_SINK_VY,
                               dt * Q2_LIQUID_EASE_RATE);
    } else if ((s16)p->vel[1] > Q2_TERMINAL_VY) {
        p->vel[1] = Q2_TERMINAL_VY;
    }

    /* 0x80046094 — the impulse, and only when the poster armed it. */
    if (!p->impulse_armed)
        return;

    /*
     * 0x800460A4..0x800460FC: the entity leaves the ground, its ground normal
     * is cleared and its last contact is replaced by the flat (0, 4096, 0) at
     * 0x800AE928. Without this a jump that starts on a floor keeps the floor's
     * normal and the velocity clip takes the jump straight back off.
     */
    p->ent.flags &= ~(u32)(Q2_ENT_ON_GROUND | Q2_ENT_ON_ENTITY |
                           Q2_ENT_WALL_CONTACT | 0x2000u);
    p->on_ground = false;
    p->ent.ground_normal[0] = p->ent.ground_normal[1] = p->ent.ground_normal[2] = 0;
    p->ent.last_normal[0]   = 0;
    p->ent.last_normal[1]   = 4096;
    p->ent.last_normal[2]   = 0;

    p->vel[0] = (s16)(p->vel[0] + p->impulse[0]);
    p->vel[1] = (s16)(p->vel[1] + p->impulse[1]);

    /*
     * 0x80046140. Single player only, and it is the same -3072 the jump posts —
     * so a plain jump lands exactly on the ceiling and no stack of impulses,
     * rocket jump included, can beat it.
     */
    if (!sim->multiplayer && (s16)p->vel[1] < Q2_IMPULSE_CEILING)
        p->vel[1] = Q2_IMPULSE_CEILING;

    p->vel[2] = (s16)(p->vel[2] + p->impulse[2]);

    /*
     * 0x800461DC — all three deltas re-derived from the post-impulse velocity,
     * with no gravity half-step. It still lands on the NEXT frame's move, but it
     * lands one tick earlier than waiting for the following integration would.
     */
    out_delta[0] = (s16)(((s32)(s16)p->vel[0] * dt) / Q2_VEL_DIV);
    out_delta[1] = (s16)(((s32)(s16)p->vel[1] * dt) / Q2_VEL_DIV);
    out_delta[2] = (s16)(((s32)(s16)p->vel[2] * dt) / Q2_VEL_DIV);

    p->impulse[0] = p->impulse[1] = p->impulse[2] = 0;
    p->impulse_armed = false;
}

/*
 * 0x80039AE4 — remove the velocity that went into the surface just hit.
 *
 * Runs only when the contact was NOT flat enough to stand on, which is what
 * keeps a landing out of it: the ground case is handled by the mover and the
 * fall-damage delta below depends on it.
 *
 * Each term of the dot product is shifted separately with a +4095 bias on
 * negatives, so the whole thing truncates toward zero per term rather than once
 * at the end. Doing it in one 64-bit product gives a different answer.
 */
static s32 dot_term(s32 n, s32 v)
{
    s32 prod = n * v;

    if (prod < 0)
        prod += 4095;
    return prod >> Q2_FRAC_12;
}

/*
 * And the multiply BACK is not the same operation. 0x80039B80, 0x80039B90 and
 * 0x80039BA4 are bare `sra ..., 12` with no bias, so the second stage truncates
 * toward -inf while the dot product's terms truncate toward zero. Using one
 * helper for both — the obvious tidy-up — is off by one on negative components.
 */
static s32 scale_term(s32 n, s32 d)
{
    return (n * d) >> Q2_FRAC_12;
}

static void clip_velocity(q2_sim *sim)
{
    q2_player *p = &sim->player[sim->cur_player];
    const s16 *n = p->ent.last_normal;
    s32 d, keep;

    if (n[1] >= p->ent.max_slope_ny)
        return;

    d = dot_term(n[0], p->vel[0]) + dot_term(n[1], p->vel[1]) +
        dot_term(n[2], p->vel[2]);

    /*
     * 0x80039B48..0x80039B58 — captured from the velocity BEFORE the clip, and
     * CAPPED at zero, not floored at it. This had the sense inverted:
     *
     *     80039B48  addu  a1, zero, zero   ; keep = 0
     *     80039B50  bgez  a2, 0x80039B5C   ; vel.y >= 0 -> keep STAYS zero
     *     80039B54  addu  v1, a0, v0       ; delay slot, just the dot sum
     *     80039B58  addu  a1, a2, zero     ; reached only when vel.y < 0
     *
     * +Y is down, so `vel.y < 0` is RISING. Retail preserves a rising entity's
     * climb rate and gives a FALLING one nothing; this kept the fall rate and
     * gave a riser nothing, which is the opposite entity in both arms.
     */
    keep = ((s16)p->vel[1] < 0) ? (s16)p->vel[1] : 0;

    p->vel[0] = (s16)(p->vel[0] - scale_term(n[0], d));
    p->vel[1] = (s16)(p->vel[1] - scale_term(n[1], d));
    p->vel[2] = (s16)(p->vel[2] - scale_term(n[2], d));

    /*
     * 0x80039BB4 — a guard against the clip ever ADDING upward speed.
     *
     * With `keep` read the right way round this is not a fall-rate restorer.
     * `keep` is the rise rate for a climbing entity and zero for a falling one,
     * and the test only fires when the clip left the entity rising FASTER than
     * it was. The vector is then rescaled so the vertical component is put back
     * to what it was and the horizontal components grow to match — which is
     * what makes a steep face shed you sideways instead of launching you.
     *
     * A falling entity has `keep == 0` and a post-clip `vel.y > 0`, so
     * `vel.y >= keep` and the whole block is skipped. Under the inverted read it
     * was the RISING entity that landed on `keep == 0`, and the rescale then
     * multiplied the entire velocity by zero — brushing a doorframe on the way
     * up would have deleted all three components and dropped you straight down.
     */
    if ((s16)p->vel[1] >= keep)
        return;

    if ((s16)p->vel[1] == 0) {
        /* The original divides here and would trap. It cannot be reached from
         * a non-floor contact that leaves vel.y at zero with keep > 0, but the
         * port does not fault where the console would not. */
        return;
    }

    {
        s32 vy = (s16)p->vel[1];

        p->vel[0] = (s16)(((s32)(s16)p->vel[0] * keep) / vy);
        p->vel[2] = (s16)(((s32)(s16)p->vel[2] * keep) / vy);
        p->vel[1] = (s16)keep;
    }
}

/*
 * 0x80039CB4 — fall damage.
 *
 * Driven by how much LANDING changed vertical velocity, not by how far the fall
 * was, so a lift that stops under you hurts and a slope that bleeds the speed
 * off does not.
 */
static void fall_damage(q2_sim *sim, s32 vy_before)
{
    q2_player *p = &sim->player[sim->cur_player];
    s32 delta, sev, dmg;

    if (!(p->ent.flags & Q2_ENT_ON_GROUND))
        return;

    delta = (s16)p->vel[1] - vy_before;
    delta >>= Q2_FALL_SHIFT;
    sev    = (delta * delta) / Q2_FALL_DIV;

    if (sev < Q2_FALL_MIN)
        return;

    /* 0x80039CF4: no fall damage in water, shallow or deep. */
    if (p->ent.flags & (Q2_ENT_INWATER | Q2_ENT_UNDERWATER))
        return;

    /*
     * The view kick, in degrees converted to the 4096-step circle — 4096/360
     * exactly, and the 455 cap is 40 degrees. That the cap is id's own 40 while
     * the conversion is the PSX's own angle unit is the clearest evidence yet
     * that Hammerhead ported the tables and rewrote the arithmetic.
     */
    p->fall_value = (s16)((sev * 4096) / 360);
    if (p->fall_value > Q2_FALL_KICK_MAX)
        p->fall_value = Q2_FALL_KICK_MAX;
    p->fall_time = sim->level_time + Q2_FALL_KICK_TIME;

    /*
     * 0x80039D50 — and this branch was read the wrong way round.
     *
     *     80039D50  slti  v0, s0, 31
     *     80039D54  bne   v0, zero, 0x80039DBC
     *     ...
     *     80039DBC  lw    a0, 17156(gp)      ; 0x800B2904 = pla_land1
     *     80039DC0  jal   0x8007270C
     *
     * The taken arm is `sev < 31`, and it PLAYS something — the soft landing.
     * The note here used to say the band below 31 was silent and that the thump
     * and the damage shared one threshold. They do not: 31 chooses BETWEEN two
     * sounds. Every ordinary hop landed in the quiet band, so between this and
     * the missing jump sound a jump made no noise at either end.
     *
     * The soft arm also has no alive gate — the `blez` at 0x80039D64 sits only
     * on the loud arm below — so a landing that kills you still makes this one.
     */
    if (sev < Q2_FALL_SOFT) {
        q2_ent_sound_at(&sim->ent_world.events, Q2_SND_LAND_SOFT, p->pos);
        return;
    }

    /* 0x80039D5C: the sound is gated on being alive, so a killing fall lands
     * silently and the death sound has the frame to itself. */
    if (sim->combat.inv.health > 0)
        q2_ent_sound_at(&sim->ent_world.events, Q2_SND_LAND, p->pos);

    dmg = (sev - Q2_FALL_DMG_BASE) / 2;
    if (dmg <= 0)
        dmg = 1;

    if (sim->no_fall_damage)
        return;

    q2_sim_hurt_player(sim, &sim->combat.self, (s16)dmg, Q2_MOD_FALLING,
                       sim->player[sim->cur_player].pos);
}

/*
 * 0x8003AA3C — footsteps.
 *
 * Gated on the WISH velocity rather than the actual one, so you get footsteps
 * while walking into a wall and none while sliding to a halt.
 */
static void update_footsteps(q2_sim *sim)
{
    q2_player *p = &sim->player[sim->cur_player];
    s32 half = max_speed(p->ent.flags) / 2;
    s32 side = p->wish[0] < 0 ? -p->wish[0] : p->wish[0];
    s32 fwd  = p->wish[2] < 0 ? -p->wish[2] : p->wish[2];

    if (!(p->ent.flags & (Q2_ENT_ON_GROUND | Q2_ENT_WALL_CONTACT)))
        return;
    if (p->ent.flags & Q2_ENT_NO_FOOTSTEP)
        return;
    if (side <= half && fwd <= half)
        return;
    if (sim->level_time <= p->footstep_time)
        return;

    /*
     * 0x8003AAB8. The wet case does NOT advance the alternation — one splash
     * every 320 ticks rather than a left and a right — so a player wading keeps
     * whichever foot they arrived with.
     */
    if (p->ent.flags & Q2_ENT_INWATER) {
        p->footstep_time = sim->level_time + Q2_FOOTSTEP_PERIOD_WET;
        q2_ent_sound_at(&sim->ent_world.events, Q2_SND_FOOTSTEP_WET, p->pos);
    } else {
        p->footstep_time = sim->level_time + Q2_FOOTSTEP_PERIOD;
        p->foot = p->foot ? 0 : 1;
        q2_ent_sound_at(&sim->ent_world.events,
                        p->foot ? Q2_SND_FOOTSTEP_A : Q2_SND_FOOTSTEP_B,
                        p->pos);
    }
}

/*
 * 0x8003AE10 — the pain sound, and the diff that decides there was any pain.
 *
 * The engine keeps no "was hurt" flag. It compares health and armour with last
 * tick's copies, and either falling is enough — so armour absorbing a hit still
 * makes the player grunt, and a hit healed in the same tick makes no sound at
 * all.
 *
 * The throttle is client+0xD0 and it is the SAME field the damage view kick
 * decays against, which is why the two cannot be separated: 210 ticks of
 * silence, 150 ticks of kick, one deadline.
 */
/*
 * 0x8003D254 — the water transitions, reached from 0x8003B00C, which is the
 * LAST call the player's frame makes.
 *
 * The port had no counterpart, so wading into a pool, going under and climbing
 * back out were all completely silent. The shape is three edges over two
 * counters:
 *
 *     8003D27C  and   v0, v0, 0x80000    ; dead -> nothing at all
 *     8003D290  srl   v0, flags, 8       ; UNDERWATER?
 *     8003D2A8  bne   air, zero, ...     ; already under -> the breathing loop
 *                 PlayerNoise(type 0), then pla_watr_in if the splash
 *                 cooldown at client+0xD8 has expired
 *     8003D450  beq   air, zero, ...     ; was under, is not -> pla_watr_out
 *     8003D474  sw    zero, 132(s0)      ; air = 0
 *     8003D480  srl   v0, flags, 2       ; INWATER (shallow)?
 *     8003D498  bne   wade, zero, ...    ; only on the FIRST tick of a wade
 *                 pla_watr_in again, cooldown = level_time + 100, splash effect
 *     8003D4D8  wade++      /  8003D4E0  wade = 0
 *
 * Both the entry and the wade play `pla_watr_in`; only the exit is its own
 * sound. The cooldown is SET by the wade and READ by the submerge, which is
 * what stops a dive from playing the splash twice as you pass through the
 * surface.
 *
 * The submerged arm is separate from the transition. It accumulates air in
 * client+0x84, starts a 300-tick clock in +0x88, and periodically calls the
 * ordinary damage path with MOD 8 — the one class that skips both armour
 * stages. A rebreather resets the counter to one every frame before that
 * periodic test, which is why its expiry field is tested here instead of being
 * folded into generic invulnerability.
 *
 * At 3601 air units the cadence becomes 750 ticks and the retail build emits
 * `pla_watr_un`; below it, every other 300-tick damage pass emits either the
 * existing drowning voice or `pla_gasp1`, chosen by the game's rand() bit.
 * The silent alternate pass is real: client+0xDF is XORed with one before the
 * sound branch at 0x8003D388.
 */
static void world_effects(q2_sim *sim)
{
    q2_player *p = &sim->player[sim->cur_player];

    /* 0x8003D27C — a corpse makes no splash. */
    if (p->ent2_flags & Q2_ENT2_DEAD)
        return;

    if (p->ent.flags & Q2_ENT_UNDERWATER) {
        /* 0x8003D2A8: the submerge EDGE. */
        if (p->water_air == 0) {
            /* 0x8003D2D0: `sltu`, so the sound is skipped while the wade's own
             * cooldown is still running. */
            if ((u32)p->splash_time < (u32)sim->level_time)
                q2_ent_sound_at(&sim->ent_world.events, Q2_SND_WATER_IN,
                                p->pos);
            p->water_air = 1;
            p->water_next = sim->level_time + 300;
        }

        /* 0x8003D300: the water clock carries the same dt as the level. */
        p->water_air += sim->cur_dt;

        /* client+0xB8. The rebreather suppresses drowning by resetting air,
         * rather than by changing MOD 8's damage rule. */
        if ((u32)sim->level_time < (u32)sim->combat.inv.breather_until)
            p->water_air = 1;

        /* 0x8003D344: the deadline is strict, not inclusive. */
        if ((u32)p->water_next >= (u32)sim->level_time)
            return;

        if (p->water_air < 3601) {
            s32 damage = p->water_air >> 10;

            p->water_next = sim->level_time + 300;
            if (damage > 7)
                damage = 7;

            /* 0x8003D380. Even a zero-point pass reaches T_Damage; doing so
             * preserves the retail MOD state and costs no health. */
            q2_sim_hurt_player(sim, NULL, (s16)damage, Q2_MOD_NO_ARMOUR,
                               p->pos);

            /* 0x8003D388. One pass is silent; the other selects a warning from
             * the game's one-bit random result. */
            p->water_voice = !p->water_voice;
            if (p->water_voice) {
                q2_ent_sound sound = (rand() & 1) ? Q2_SND_DROWN
                                                   : Q2_SND_GASP;

                q2_ent_sound_at(&sim->ent_world.events, sound, p->pos);
            }
        } else {
            /* 0x8003D3E0 / 0x8003D434: long-submerge warning cadence. A live
             * rebreather has already reset air above, so this is the reachable
             * no-rebreather arm on the retail disc. */
            p->water_next = sim->level_time + 750;
            q2_ent_sound_at(&sim->ent_world.events, Q2_SND_WATER_UNDER,
                            p->pos);
        }
        return;
    }

    /* 0x8003D450: the surface EDGE, taken only if we were under. */
    if (p->water_air != 0)
        q2_ent_sound_at(&sim->ent_world.events, Q2_SND_WATER_OUT, p->pos);

    p->water_air = 0;

    /* 0x8003D480: shallow water, and only the tick you step into it. */
    if (p->ent.flags & Q2_ENT_INWATER) {
        if (p->wade == 0) {
            q2_ent_sound_at(&sim->ent_world.events, Q2_SND_WATER_IN, p->pos);
            p->splash_time = sim->level_time + 100;
        }
        p->wade++;
    } else {
        p->wade = 0;
    }
}

static void update_pain(q2_sim *sim)
{
    q2_player *p = &sim->player[sim->cur_player];
    s16 health = sim->combat.inv.health;
    s16 armour = sim->combat.inv.armour;
    bool hurt;

    hurt = (health < p->prev_health) || (armour < p->prev_armour);

    /*
     * THE KILLING TICK IS NOT A PAIN TICK.
     *
     * The death handler raises pla_death4 and jumps PAST the pain raise, so a
     * player whose health has just crossed zero cries out once, in the voice
     * for dying. This port ran the pain bracket unconditionally and the death
     * sound did not exist at all, so a death sounded like a flesh wound —
     * mal_pn25_1, because health under 25 is also health under 0.
     *
     * pla_drown1 takes its place when the client's air field says the player
     * went under rather than being shot.
     */
    if (health <= 0) {
        if (p->prev_health > 0) {
            /*
             * AND NOT EVERY DEATH IS AUDIBLE. The voice belongs to the death
             * handler rather than to this function, and 0x80039728 skips it
             * outright unless entity+222 is -1 — so only a death nobody is
             * credited with cries out. A player shot by somebody dies in
             * silence, and the port had been raising pla_death4 for all of
             * them because this is where it first noticed the crossing.
             *
             * `q2_player_death_cries_out` is that test, including 0x800396CC's
             * correction: acid and lava erase the attacker first, so dying in
             * the level's own hazards IS audible however you came to be
             * standing in them.
             */
            if (q2_player_death_cries_out(sim->combat.self.last_attacker,
                                          sim->combat.self.last_mod))
                q2_ent_sound_at(&sim->ent_world.events,
                                (p->ent.flags & Q2_ENT_UNDERWATER)
                                    ? Q2_SND_DROWN : Q2_SND_DEATH,
                                p->pos);
            /*
             * The camera builder's own gate — see Q2_ENT2_DEAD. The console
             * raises this bit in the CORPSE think (0x80039640/0x80039694) and
             * not here; the sim keeps its own copy a tick early because its
             * movement gates need one whether or not a client is driving the
             * death chain. `playerdeath.c` owns the other copy and the client
             * ORs the two together.
             */
            p->ent2_flags |= Q2_ENT2_DEAD;
        }
    } else if (hurt && sim->level_time > p->pain_time) {
        q2_ent_sound which;

        /* 0x8003AF54: four brackets on the health that is LEFT, so the voice
         * gets worse as the player does. */
        if (health < 25)      which = Q2_SND_PAIN_25;
        else if (health < 50) which = Q2_SND_PAIN_50;
        else if (health < 75) which = Q2_SND_PAIN_75;
        else                  which = Q2_SND_PAIN_100;

        p->pain_time = sim->level_time + 210;
        q2_ent_sound_at(&sim->ent_world.events, which, p->pos);
    }

    /* 0x8003AFA8: unconditionally, at the very end of the frame. */
    p->prev_health = health;
    p->prev_armour = armour;
}

/* ------------------------------------------------------------------------- */
/* Collision seam                                                             */
/* ------------------------------------------------------------------------- */
/*
 * A swept segment through the hull, for callers that want one — weapon fire,
 * line of sight, the walk diagnostic. Entity movement does NOT go through here;
 * it goes through q2_move_step, which is what the original does.
 *
 * AND IT IS THE PRIMARY HULL, not the movement one.
 *
 * SecondaryCol is PrimaryColl eroded by the body's 286-unit half-extent on all
 * six axes, so that a swept POINT stands in for a swept BOX. A ray is already a
 * point: eroding for it subtracts a body that is not there, and every shot
 * stopped 286 units short of the surface it was aimed at. A shot angled down at
 * a creature standing on the floor covered barely half its distance and was
 * then discarded as beyond the world, which is the other half of "you cannot
 * damage the monsters" — the first half being the write-back order in the
 * client. Measured on one ray: 583 units of reach against 1156.
 *
 * 0x80043BDC, the ray marcher, hardcodes 0x800C8E90 — PrimaryColl — at
 * 0x80043C18 no matter which context its caller passed (FORMATS §1493), and the
 * context table assigns the primary hull to rays, line of sight, the AI and
 * spawning, leaving the secondary one to entity movement alone (§1467).
 * simcombat.c's debris path had already worked this out and written it down.
 *
 * `current_node` is not passed on as the search hint: it is a SecondaryCol cell
 * index and means nothing in the other hull.
 *
 * The fraction is reconstructed from the clipped end point along the longest
 * axis. The engine never forms one: it carries an exact rational and scales an
 * int16 delta by it, so any fraction here is the port's convenience and can be
 * one unit out on a long move. Do not feed it back into the geometry.
 */
/*
 * THE SECOND PASS — runtime entity boxes.
 *
 * A mover is an entity, not hull (trace.h), so the walk above answers as if
 * every door and intact glass pane in the level were absent. The console's own
 * bullet path traces the hull and then re-traces against the entity list, and
 * this is that: the segment is clipped against the entity boxes from the ORIGINAL start to
 * wherever the hull left it, so whichever of the two is nearer wins without
 * needing the fractions compared.
 *
 * `ent` is the entity box's caller handle. Movers use their mover index and
 * glass uses its breakable index; the weapon's dedicated damageable-box sweep
 * still supplies the latter's callback routing.
 */
static void trace_clip_entities(q2_sim *sim, const s32 start[3], q2_trace *out)
{
    q2_move_seg_hit mh;
    int k;

    if (!sim->move_world.count)
        return;

    if (!q2_move_clip_segment(&sim->move_world, start, out->end, NULL, &mh))
        return;

    /* The fraction is along start..out->end, which is itself a fraction of the
     * caller's segment; composing them keeps the answer on the ORIGINAL scale
     * the caller is going to multiply its direction by. */
    out->fraction = (s32)(((s64)out->fraction * mh.frac) >> Q2_FRAC_12);
    for (k = 0; k < 3; k++) {
        out->end[k]    = mh.pos[k];
        out->normal[k] = mh.normal[k];
    }
    out->hit      = true;
    out->ent      = mh.id;
    out->contents = 0;
}

void q2_sim_trace(q2_sim *sim, const s32 start[3], const s32 end[3],
                  q2_trace *out)
{
    s32 pos[3];
    s32 node = -1;
    bool complete;
    q2_collision *hull;

    if (!out)
        return;

    memset(out, 0, sizeof(*out));
    out->fraction = Q2_ONE_12;
    out->node     = -1;
    out->ent      = -1;

    if (!sim || !start || !end)
        return;

    out->end[0] = end[0];
    out->end[1] = end[1];
    out->end[2] = end[2];

    if (!sim->coll_ready && !sim->coll_primary_ready)
        return;             /* no hull — move freely rather than wedge */

    hull = sim->coll_primary_ready ? &sim->coll_primary : &sim->coll;

    complete = q2_coll_move(hull, start, end, -1, pos, &node);

    out->end[0] = pos[0];
    out->end[1] = pos[1];
    out->end[2] = pos[2];
    out->node   = node;

    if (node >= 0) {
        q2_coll_node n;
        if (q2_collision_get_node(hull, (u32)node, &n))
            out->contents = n.contents;
    }

    if (complete) {
        /* A segment the hull let through can still meet a door, and this is
         * the arm that used to return before the second pass ran — i.e. the
         * common case: a corridor with a closed door across it is open hull
         * for the whole of the shot's length. */
        trace_clip_entities(sim, start, out);
        return;
    }

    out->hit = true;

    if (hull->hit_plane_index >= 0) {
        q2_coll_plane pl;

        if (q2_collision_get_plane(hull,
                                   (u32)hull->hit_plane_index, &pl)) {
            out->normal[0] = pl.nx;
            out->normal[1] = pl.ny;
            out->normal[2] = pl.nz;
        }
    }

    /* Longest-axis ratio: the axis with the largest intended travel is the one
     * whose quotient carries the least rounding error. */
    {
        int axis = 0, i;
        s32 span = 0;

        for (i = 0; i < 3; i++) {
            s32 d = end[i] - start[i];
            if (d < 0) d = -d;
            if (d > span) { span = d; axis = i; }
        }

        if (span > 0) {
            s64 got = (s64)(pos[axis] - start[axis]);
            s64 all = (s64)(end[axis] - start[axis]);
            s32 f   = (s32)((got * Q2_ONE_12) / all);

            if (f < 0)          f = 0;
            if (f > Q2_ONE_12)  f = Q2_ONE_12;
            out->fraction = f;
        } else {
            out->fraction = Q2_ONE_12;
        }
    }

    trace_clip_entities(sim, start, out);
}

/* ------------------------------------------------------------------------- */
/* One logic tick                                                             */
/* ------------------------------------------------------------------------- */
void q2_sim_tick(q2_sim *sim, const q2_input *input, s32 dt)
{
    q2_player *p;
    q2_input   corpse;
    bool run_world;
    bool was_underwater;
    bool moving;
    u32  buttons;
    s32  vy_before;

    if (!sim || !input || dt <= 0)
        return;

    /*
     * THE TICK'S OWN LENGTH, set FIRST.
     *
     * This used to be assigned near the end of the tick, after
     * `update_triggers` had already run — so anything a trigger called saw the
     * PREVIOUS frame's delta, and on the first tick of a level saw zero. The
     * briefing pop-up is the case that showed it: a HELPCOMPUTER volume arms
     * its countdown as `delay * frame_dt`, and with frame_dt clamped up from
     * zero to 1 the objective screen came up on an arbitrary fraction of the
     * delay it was authored with.
     *
     * Nothing in the tick wants the previous dt: the projectile sweep this
     * field was added for reads it after this point either way.
     */
    sim->cur_dt = dt;

    p = &sim->player[sim->cur_player];

    /*
     * A CORPSE'S PAD IS NEVER READ, and that is not a rule the engine states —
     * it is what the death handler's LAST act makes true.
     *
     * 0x8003A4A4 is the only caller of the pad read (0x80019154), and it is
     * instruction 54 of the player think at 0x8003A1C8. `player_die` ends by
     * overwriting `entity+0x3C` with the corpse think (0x80039818), so the
     * player think is not installed any more and NOTHING in it runs again: not
     * the pad, not the wish, not the look integration, not the jump, and not
     * the view weapon driver at 0x8004EE0C, whose one and only call site is
     * 0x8003AD98 in the same function. What still moves the body is
     * `corpse_think`'s own physics — gravity, the ground plane and the dt * 5
     * friction — which is a different and much smaller integrator.
     *
     * This port has one tick function and no think pointer to swap, so the
     * substitution is made here: a dead player is ticked with a NEUTRAL pad.
     * The body still falls, slides and settles, because the mover still runs;
     * it just has nobody driving it. Without this a corpse walked, turned,
     * jumped and fired — reported from play, and the reason the death page had
     * a live player standing behind it.
     *
     * The three individual gates further down (the jump at 0x8003A8B0, the
     * strafe roll at 0x8003AB08, the splash at 0x8003D260) are kept as they
     * are: each is a real test in the executable, and they are what a dead
     * player would still hit if some other path ever ticked one with input.
     */
    if (p->ent2_flags & Q2_ENT2_DEAD) {
        memset(&corpse, 0, sizeof(corpse));
        input = &corpse;

        /*
         * And the accumulators the think would have been writing, because a
         * neutral pad only stops them GROWING. `yaw_rate` and `pitch_rate` are
         * eased toward the stick and then integrated at 0x8003A990, so a body
         * that died mid-turn went on turning for a second after it fell; the
         * wish is the same thing for the walk, and `recentring` would have
         * walked the corpse's pitch back to level.
         *
         * `vel` is deliberately NOT cleared. That is momentum, and the body is
         * supposed to keep it: on the console `corpse_think` inherits whatever
         * the last move left and takes it down with `approach(v, 0, dt * 5)`.
         */
        p->yaw_rate   = 0;
        p->pitch_rate = 0;
        p->wish[0]    = 0;
        p->wish[1]    = 0;
        p->wish[2]    = 0;
        p->recentring = false;
        p->autocentre = 0;
        p->jump_hold  = 0;
    }

    /*
     * The WORLD half of a tick runs once per frame, not once per player.
     *
     * A tick is two things wearing one name: the player's frame — movement,
     * view, weapon, the volumes they are standing in — and the world's, which
     * is the entity sweep, the effects, the glint and the clock. With one
     * player they are indistinguishable. With four they must not be: running
     * the entity sweep four times would age every item respawn four times as
     * fast and step the effects four times a frame.
     *
     * So the world half is gated on being player 0. `q2_sim_advance` sets
     * `cur_player` to 0 and behaves exactly as it always has; the extra players
     * go through `q2_sim_advance_player`, which runs their frame against the
     * same world without advancing it again.
     */
    run_world = (sim->cur_player == 0);

    /*
     * The order below is 0x8003A1C8's, address by address, because several
     * steps only behave correctly in it. In particular the view offset is
     * chosen from the PREVIOUS tick's environment flags, the jump reads the
     * ground the previous move left behind, and the look angles integrate
     * before the wish velocity is rotated by them.
     */

    /*
     * The event queue is drained by the caller between ticks, so it is cleared
     * HERE rather than beside the entity sweep that used to own it. The player's
     * own frame emits into it — footsteps, the landing thump, the pain grunt —
     * and all three happen before the sweep runs.
     */
    if (run_world)
        q2_ent_events_clear(&sim->ent_world.events);

    /*
     * 0x8003A41C — the fly bit, mirrored out of the GAME VARIABLES word before
     * anything reads it. It has to be here rather than wherever the setting is
     * written, because the original re-derives it every frame and a caller that
     * clears `full_basis_movement` mid-game must see the flag go with it.
     */
    if (sim->full_basis_movement)
        p->ent2_flags |=  (u32)Q2_ENT2_FLY;
    else
        p->ent2_flags &= ~(u32)Q2_ENT2_FLY;

    /* 0x8003A208 — view offset, from flags that have not been cleared yet. */
    update_view_offset(sim, dt);

    /* 0x8003A25C — clear the environment flags, then let the volumes re-assert
     * them. `was_underwater` is latched at 0x8003A264, before the clear. */
    was_underwater = (p->ent.flags & Q2_ENT_UNDERWATER) != 0;
    update_env_flags(sim);

    /*
     * 0x8003A29C — the SCRIPT half of the same dispatcher call, and it belongs
     * here beside the flag half rather than after the move.
     *
     * `xrefs 0x80027E64` gives exactly one call site, at instruction 54 of the
     * player frame: before the pad, before the wish, before the jump and before
     * the move. One call re-runs the events of every volume the entity is
     * inside, and the environment primitives are simply the events that do
     * nothing but OR a bit. The port had split it in two and left this half at
     * the very end of the tick.
     *
     * The point being tested does not change — `update_triggers` already
     * samples the origin. What moving it buys is that whatever a trigger does
     * reaches the move and the entity sweep on the SAME tick the console would,
     * instead of a tick later. A door told to open now opens against the frame
     * you are walking into it on.
     *
     * Safe with respect to the two things a trigger can do that would be
     * alarming here: a TELEPORT and a zone change are both queued rather than
     * applied (`pending_teleport`, `q2_sim_take_zone_change`), so neither can
     * relocate the player into the middle of this tick's move.
     */
    update_triggers(sim);

    /* The mover's frame: the entity origin, Q2_EYE_BASE above the feet. */
    p->ent.pos[0] = p->pos[0];
    p->ent.pos[1] = q2_sim_origin_y(p->pos[1]);
    p->ent.pos[2] = p->pos[2];

    /* 0x8003A4A4 — the pad. 0x8003A4EC may add a jump the player did not ask
     * for, so the button word is taken by value from here on. */
    buttons = input->buttons;

    /*
     * 0x8003A4AC — the AUTOCENTRE timer, and it goes here because it reads the
     * button word the pad JUST returned, before the water-exit jump writes a
     * synthesised bit into it (0x8003A55C). So `input->buttons` and not
     * `buttons`.
     *
     *     8003A4AC  lw    v0, 40(sp)          ; the button word
     *     8003A4B4  bgez  v0, 0x8003A4E8      ; bit 31 clear -> clear the timer
     *     8003A4BC  lh    v0, 13122(v0)       ; 0x800B3342, the setting
     *     8003A4C4  beq   v0, zero, 0x8003A4E8
     *     8003A4CC  lhu   v0, 38(s1)          ; UNSIGNED load
     *     8003A4DC  addu  v0, v0, v1          ; += dt
     *     8003A4E4  sh    v0, 38(s1)
     *
     * `bgez` on the whole word tests bit 31, which is Q2_BTN_MOVING — so the
     * timer counts only while the forward axis is deflected and resets the
     * moment it is not. The add is UNSIGNED and the threshold test below is
     * SIGNED, so the counter is allowed to wrap; that is retail behaviour on an
     * airborne player and is not something to saturate away.
     */
    if ((input->buttons & Q2_BTN_MOVING) && sim->autocentre_setting)
        p->autocentre = (s16)((u16)p->autocentre + (u16)dt);
    else
        p->autocentre = 0;

    water_exit_jump(sim, was_underwater, &buttons);

    /* 0x8003A584 — the wish velocity. */
    moving = ((u16)input->forward | (u16)input->side) != 0;
    update_wish(sim, input, dt);

    /* 0x8003A658 — the look rates. */
    update_look(sim, input, dt);

    /* 0x8003A780 — the look-button chord, and the pitch it walks back to zero.
     * Between the rate and the integration, so a recentring tick contributes no
     * rate of its own. */
    update_recentre(sim, input, dt);

    /*
     * 0x8003A8FC — the jump, and the two gates in front of it: entity+0x10C bit
     * 12 (flying) at 0x8003A8F0, and flags & 0x700, so you cannot jump while
     * crouching, low-crouching or submerged. The whole call is skipped, which
     * means the hold timer does not tick down either — walk into a crouch zone
     * mid-jump and the timer freezes.
     */
    /*
     * And the OUTERMOST of the three gates, which was missing: 0x8003A8B0 tests
     * entity+0x10C bit 19 and 0x8003A8BC branches past the weapon think, the
     * weapon switch AND the jump when it is set. That bit is Q2_ENT2_DEAD, set
     * at 0x80039640/0x80039694 and mirrored here at the death check below, so a
     * corpse could still press jump and hop.
     *
     * Folded into the same `if` because retail's branch skips the whole call:
     * `jump_hold` does not decay while any of these gates hold, which is the
     * behaviour the hold timer's comment describes.
     */
    if (!(p->ent2_flags & (Q2_ENT2_DEAD | Q2_ENT2_FLY)) &&
        !(p->ent.flags & (Q2_ENT_UNDERWATER | Q2_ENT_INCROUCH |
                          Q2_ENT_INLOWCROUCH)))
        player_jump(sim, (buttons & Q2_BTN_JUMP) != 0, dt);

    /*
     * 0x8003A938 — and the AUTOCENTRE timer's other half, which arms the same
     * recentre the look-button chord arms.
     *
     *     8003A93C  lw    v0, 0x800AEBCC   ; deathmatch
     *     8003A944  bne   v0, zero, ...    ; multiplayer never recentres
     *     8003A954  srl   v1, v0, 5        ; ON_GROUND
     *     8003A95C  srl   v0, v0, 6        ; ON_ENTITY
     *     8003A968  beq   v1, zero, ...    ; airborne never recentres
     *     8003A978  slti  v0, v0, 200      ; SIGNED, against the timer
     *     8003A984  sh    v0, 38(s1)       ; clamp AT 200
     *     8003A98C  sb    v0, 37(s1)       ; recentring = true
     *
     * This sits at 0x8003A938, which is where 0x8003A8F4, 0x8003A908,
     * 0x8003A924 and 0x8003A92C all branch to — so it runs whether or not the
     * jump above was gated out, and must stay outside that `if`.
     *
     * The clamp is not a bound on an airborne timer (it cannot be — it is
     * inside the grounded arm). It holds the counter AT the threshold so the
     * flag re-asserts on every later grounded frame until you stop walking.
     */
    if (!sim->multiplayer &&
        (p->ent.flags & (Q2_ENT_ON_GROUND | Q2_ENT_ON_ENTITY)) &&
        p->autocentre >= 200) {
        p->autocentre = 200;
        p->recentring = true;
    }

    /* 0x8003A990 — integrate the angles from the rates. */
    integrate_look(sim, dt);

    /* 0x8003AA3C, 0x8003AB18 — footsteps and the strafe roll. */
    update_footsteps(sim);
    update_roll(sim);

    /* 0x8003AB68 — swimming up. */
    swim_up(sim, input, dt);

    /* 0x8003ABC4 — wish velocity into world velocity, plus the ground stick. */
    wish_to_world(sim, moving, dt);

    /*
     * 0x8003AD80 — the wall-contact flag is cleared immediately before the move,
     * so it describes the PREVIOUS frame's contact everywhere above and is
     * re-established by the contents query below.
     */
    p->ent.flags &= ~(u32)Q2_ENT_WALL_CONTACT;

    /* 0x80039AC4 — the move. Everything from here to fall_damage is 0x80039AA4
     * and the mover it calls. */
    vy_before = (s16)p->vel[1];

    /*
     * 0x800458A0 runs the volume query at the top of the mover, with mask
     * 0x3360, and turns the answer into the entity's own liquid and wall flags.
     * Those flags choose the integrator's arm and halve the step height, so it
     * has to come before both.
     */
    update_contents(sim);

    /*
     * 0x800459E8 — snap the velocity to the surface being stood on. This is the
     * rule that makes standing still stand still, and the one that produces the
     * velocity change fall damage measures.
     */
    ground_project(sim);

    /*
     * 0x80045AC8 — the last contact normal is reset to the flat (0, 4096, 0) at
     * 0x800AE928 before the moves, so a move that hits nothing leaves a normal
     * the velocity clip will ignore rather than last frame's wall.
     */
    p->ent.last_normal[0] = 0;
    p->ent.last_normal[1] = 4096;
    p->ent.last_normal[2] = 0;

    if (sim->coll_ready) {
        /*
         * Lift, slide, drop — 0x80045B24. Ground is decided by the drop alone,
         * which is why walking into a wall does not read as landing on it, and
         * why a step up to Q2_STEP_HEIGHT costs nothing.
         *
         * The delta is LAST tick's, per q2_player.frame_delta.
         */
        q2_move_step(&sim->coll, &p->ent, p->frame_delta,
                     sim->move_world.count ? &sim->move_world : NULL);

        p->pos[0] = p->ent.pos[0];
        p->pos[1] = q2_sim_feet_y(p->ent.pos[1]);
        p->pos[2] = p->ent.pos[2];

        p->on_ground      = (p->ent.flags & Q2_ENT_GROUNDED_MASK) != 0;
        sim->current_node = p->ent.node;
    } else {
        /* Without a hull, fall back to the seeded ground plane so the player
         * does not drop forever in a zone that failed to parse. */
        p->pos[0] += p->frame_delta[0];
        p->pos[1] += p->frame_delta[1];
        p->pos[2] += p->frame_delta[2];

        p->on_ground = false;
        if (p->pos[1] >= p->ground_y) {
            p->pos[1]    = p->ground_y;
            p->ent.flags |= Q2_ENT_ON_GROUND;
            p->ent.ground_normal[1] = 4096;
            p->on_ground = true;
        }
    }

    /* 0x80045E74 — and now next tick's delta. */
    integrate_vertical(sim, dt, p->frame_delta);

    /*
     * 0x80039AE4 — clip the velocity into the surface just hit, but only when
     * that surface was too steep to stand on. A landing is left alone, because
     * the ground state and the fall-damage delta both depend on it.
     */
    clip_velocity(sim);

    /*
     * Nothing zeroes the velocity on landing, and that is deliberate: the mover
     * stops the POSITION and leaves the full falling speed in vel.y. The next
     * tick's ground_project is what removes it, which is why fall damage arrives
     * a tick after the impact. Adding a "landing kills velocity" rule here — the
     * obvious thing to write, and what this function used to do — destroys the
     * measurement fall damage depends on and makes every landing painless.
     */

    /* 0x80039CB4 — fall damage, from how much the landing changed vel.y. */
    fall_damage(sim, vy_before);

    /*
     * The level clock the weapons gate on is this same dt counter: 300 units to
     * the second, which is what makes the universal 30-tick refire a tenth of a
     * second and what the mover scripting's own time unit is (userfuncs.h).
     */
    sim->level_time += dt;

    /* `sim->cur_dt` is set at the TOP of this function — see the note there.
     * The world half of the tick and the projectile sweep (0x80047D40, which
     * advances a missile by `vel * dt`) both read it. */

    /*
     * THE SIM NO LONGER FIRES FROM RAW INPUT.
     *
     * On the console only the view-model machine may call a fire function: the
     * IDLE arm of 0x8004F87C does it through viewmodel+32 (`jalr` at
     * 0x8004FB30) and the four per-weapon arms of 0x8004FEE8 do it per
     * animation frame. `xrefs 0x8004BFBC` finds the blaster's fire function
     * referenced by exactly one word in the whole image — its table entry —
     * and by no call at all.
     *
     * Firing here as well meant two independent things decided when a shot
     * happened, and with the invented 30-tick gate removed this one fired on
     * EVERY tick the trigger was down. The owner drives it now:
     * `q2_vw_wants_fire` asks the machine, and `q2_vw_take_frame_fires` drains
     * what the per-weapon arms asked for.
     *
     * `q2_sim_fire_from_input` keeps the old behaviour for a caller with no
     * view model — the headless harness and the tests — and gates it on the
     * dry deadline so it is not a free-running stream.
     */
    if (sim->fire_from_input && input->attack)
        q2_sim_fire(sim);

    q2_sim_combat_tick(sim);

    /*
     * The effects, after the combat that spawns them, so a burst created this
     * tick does not also integrate this tick. The console runs the integrator
     * inside the draw, once per frame rather than once per viewport, which puts
     * it after everything gameplay did — the same place.
     */
    /* Once per frame, not once per player: the comment below already said
     * "once per tick, not once per viewport", and a second player is a second
     * viewport by another name. */
    if (run_world) {
        q2_fx_tick(&sim->fx);

        /* The timed beams age by the frame delta, as 0x80048CE8 does. */
        q2_fx_timed_tick(&sim->fx, dt);

        /* The glint's bands advance once per tick, not once per viewport — see
         * q2_fx_glint_advance. */
        q2_fx_glint_advance(&sim->glint);
    }

    /*
     * The entity sweep, last, so an item's touch test sees where the player
     * actually ended up this tick — the same reason the trigger update runs
     * after movement.
     *
     * The world's clock is not advanced independently: q2_entity_run adds `dt`
     * to it, and it was seeded from `level_time` at attach, so the two stay in
     * step. Mega health's decay is the player's own and is stepped here too,
     * because the engine keeps it in the client rather than on an entity.
     */
    if (sim->entities_ready) {
        /*
         * Every player publishes where they are — the entity world has taken a
         * player INDEX since it was written, and nothing had ever passed
         * anything but 0 — but only player 0's tick runs the sweep.
         */
        q2_entity_world_move_player(&sim->ent_world, sim->cur_player,
                                    sim->player[sim->cur_player].pos);
        /*
         * NOT WHILE SETTLING. The settle is the port's own drop-to-floor and
         * runs up to 180 ticks before the player sees anything; an item within
         * reach of the start position was collected inside it, with the sound,
         * the burst and the caption all cleared before any of them could be
         * drained. See `sim->settling`.
         */
        if (run_world && !sim->settling) {
            sim->ent_world.dt         = dt;
            sim->ent_world.deathmatch = sim->multiplayer;
            sim->ent_world.cheats     = sim->cheats;
            q2_entity_run(&sim->entities, &sim->ent_world);
        }
    }
    q2_item_mega_health_tick(&sim->combat.inv, sim->level_time);

    /*
     * The hurt-actor's position, every tick.
     *
     * `q2_actor_from_player` only runs inside `q2_sim_hurt_player`, so before
     * this an actor's origin was wherever it was the last time that player was
     * SHOT. Nothing noticed while there was one player, because nothing ever
     * traced at them. With four, a shot aimed at another player traced toward
     * a position they had long since left, and 301 shots in a staged encounter
     * hit nothing at all.
     *
     * Only the origin: `owner` and `last_attacker` must survive, and health is
     * synchronised by the damage path itself.
     */
    /*
     * At the EYE, not the feet.
     *
     * `q2_player.pos` is the feet — that is what a StartPos names — but the
     * thing a shot has to intersect is the body, and a shot leaves another
     * player's eye. With the origin on the floor a perfectly level shot passes
     * exactly Q2_EYE_BASE above it, and the counters caught that precisely: the
     * closest a bolt ever came was 577 units against a reach of 286 + 286 =
     * 572. Missing by five, every time, for the height of a man.
     *
     * World Y grows downward, so the eye is at a SMALLER y.
     */
    sim->combat.self.origin[0] = p->pos[0];
    sim->combat.self.origin[1] = p->pos[1] - Q2_EYE_BASE;
    sim->combat.self.origin[2] = p->pos[2];

    /* 0x8003AE10, the last thing the player's frame does: notice damage, grunt,
     * and take this tick's copy of the two figures. After the item sweep,
     * because a medkit collected this tick counts as healing. */
    update_pain(sim);

    /* 0x8003B00C — and then the water transitions, which really are the last
     * thing the frame does. See world_effects. */
    world_effects(sim);

    /* And drop the armour class bits if the armour they describe is gone —
     * 0x80035580, which the console does from inside the status bar. Here
     * because a headless run has no bar and the state is the inventory's. */
    q2_inventory_armour_upkeep(&sim->combat.inv);

    if (run_world)
        sim->tick_count++;
}

/*
 * One extra player's frame, against the world `q2_sim_advance` has already
 * advanced this frame.
 *
 * `index` must not be 0: player 0 is the one `q2_sim_advance` runs, and running
 * it again here would advance the world twice. Everything a player owns —
 * position, view, the volumes they stand in, their weapon — is theirs; the
 * entity list, the script, the effects and the clock are the world's and are
 * not touched.
 */
/* Park the live player's combat half and load another's. */
static void combat_swap_to(q2_sim *sim, int index)
{
    q2_player_combat *from = &sim->pcombat[sim->cur_player];
    q2_player_combat *to   = &sim->pcombat[index];

    if (index == sim->cur_player)
        return;

    from->inv              = sim->combat.inv;
    from->weapon_id        = sim->combat.weapon_id;
    from->next_fire        = sim->combat.next_fire;
    from->kick[0]          = sim->combat.kick[0];
    from->kick[1]          = sim->combat.kick[1];
    from->kick[2]          = sim->combat.kick[2];
    from->chaingun_bullets = sim->combat.chaingun_bullets;
    from->self             = sim->combat.self;
    from->last_shot        = sim->combat.last_shot;
    from->shot_serial      = sim->combat.shot_serial;

    sim->combat.inv              = to->inv;
    sim->combat.weapon_id        = to->weapon_id;
    sim->combat.next_fire        = to->next_fire;
    sim->combat.kick[0]          = to->kick[0];
    sim->combat.kick[1]          = to->kick[1];
    sim->combat.kick[2]          = to->kick[2];
    sim->combat.chaingun_bullets = to->chaingun_bullets;
    sim->combat.self             = to->self;
    sim->combat.last_shot        = to->last_shot;
    sim->combat.shot_serial      = to->shot_serial;

    sim->cur_player = index;
}

void q2_sim_advance_player(q2_sim *sim, int index, const q2_input *input,
                           s32 dt)
{
    int saved;

    if (!sim || !input || dt <= 0)
        return;
    if (index <= 0 || index >= Q2_SIM_MAX_PLAYERS)
        return;

    saved = sim->cur_player;
    combat_swap_to(sim, index);
    q2_sim_tick(sim, input, dt);
    combat_swap_to(sim, saved);
}

/*
 * Give player `index` the inventory and weapon a level start hands out, and
 * park it. Called once per extra player, after `q2_sim_spawn` has placed them.
 */
void q2_sim_player_reset_combat(q2_sim *sim, int index)
{
    int saved;

    if (!sim || index <= 0 || index >= Q2_SIM_MAX_PLAYERS)
        return;

    saved = sim->cur_player;

    /*
     * What player 0 has, because a deathmatch starts everybody the same way and
     * player 0 has already been through the level's own start. A bare
     * `q2_inventory_init` leaves `weapon_id` at 0 — no weapon — so the extra
     * players spawned holding nothing and could not fire a shot between them.
     */
    {
        q2_inventory     start_inv = sim->combat.inv;
        int              start_wep = sim->combat.weapon_id;

        combat_swap_to(sim, index);
        sim->combat.inv       = start_inv;
        sim->combat.weapon_id = start_wep;
    }
    sim->combat.next_fire = 0;

    /*
     * And the actor, from the inventory, so the pair starts IN STEP. Leaving it
     * zeroed is not harmless: a caller that copies the actor's health back into
     * the inventory — which is what has to happen for a player hit while parked
     * — would write 0 over a full one, and three of four players ended a
     * capture dead without anything having shot them.
     */
    q2_actor_from_player(&sim->combat.self, &sim->combat.inv,
                         sim->player[index].pos);
    combat_swap_to(sim, saved);
}

/* ------------------------------------------------------------------------- */
/*
 * ONE tick per frame, with a VARIABLE dt. Read out of the frame loop at
 * 0x80018440: the step is either `gamespeed * 2` (0x80018468, giving the
 * nominal 12) or the count of fields that actually elapsed (0x80018480), it is
 * clamped to 30 at 0x800184B8, and then 0x800184D8 runs the world exactly once.
 * The engine has no sub-stepping loop anywhere.
 *
 * That is why `q2_sim_advance` does not run several 12-unit ticks to catch up.
 * Every rate in the movement code is `k * dt`, and a clamped approach with rate
 * `k * 24` is NOT two approaches with rate `k * 12` — it overshoots less.
 * Sub-stepping would be the more "correct" integrator and the wrong game.
 *
 * The arithmetic lives HERE, in the function that only reports it, so a caller
 * that asks in advance (see sim.h) cannot be told a step the tick then
 * disagrees with.
 */
s32 q2_sim_next_dt(const q2_sim *sim, double elapsed_seconds)
{
    s32 accum;

    if (!sim || elapsed_seconds <= 0.0)
        return 0;

    /* Real time -> the engine's 1/300 s units, with the sub-unit remainder
     * carried. PEEKED, not consumed: this is the speculative query. */
    accum = sim->dt_accum + sim_whole_units(sim->dt_frac, elapsed_seconds,
                                            NULL);

    if (accum < Q2_DT_NOMINAL)
        return 0;

    return accum > Q2_DT_MAX ? Q2_DT_MAX : accum;
}

u32 q2_sim_advance(q2_sim *sim, const q2_input *input, double elapsed_seconds)
{
    s32 dt;

    if (!sim || !input || elapsed_seconds <= 0.0)
        return 0;

    dt = q2_sim_next_dt(sim, elapsed_seconds);

    sim->dt_accum += sim_whole_units(sim->dt_frac, elapsed_seconds,
                                     &sim->dt_frac);

    if (dt == 0)
        return 0;

    /*
     * The accumulator is emptied either way. Past the clamp the surplus is
     * DISCARDED rather than carried, which is how the original slows down
     * instead of catching up (0x800184C8 writes the clamped value back over
     * it); below the clamp the step IS the accumulator, so subtracting it
     * leaves the same zero.
     */
    sim->dt_accum = 0;

    q2_sim_tick(sim, input, dt);

    return 1;
}

/* ------------------------------------------------------------------------- */
/*
 * SETTLE THE PLAYER ONTO THE FLOOR, at a spawn.
 *
 * A `Population` start position is not a standing position. Measured against
 * the collision hull, the marker sits above the floor by wildly different
 * amounts across the disc -- 154 units on BASE1, 990 on BASE0, 1372 on BASE3,
 * 3814 on WASTE1 -- so it is neither a level author's small placement margin
 * nor the feet under another name. Dropped in under gravity the player visibly
 * falls for the first half-second of every level, which retail does not do.
 *
 * So the drop is resolved BEFORE the first frame, with the real mover rather
 * than a downward ray: `q2_move_step` is lift-slide-drop, so stepping it with a
 * purely downward delta lands the player exactly where walking onto that spot
 * would, including on a slope, and leaves the grounded state and contact normal
 * the mover would have set.
 *
 * NOT read out of the executable, and flagged as such. What the console does at
 * a spawn has not been traced; this matches the observable behaviour, which is
 * that the player is standing when the level starts. If those marker heights
 * ever turn out to mean something, this is the thing to revisit and the spread
 * above is the evidence to explain.
 *
 * Bounded, because a spawn over a hole must not spin here. The cap clears
 * WASTE1's 3814 several times over; anything beyond it falls the old way on the
 * first tick rather than hanging.
 */
/* Ticks allowed to land. WASTE1's 3814-unit marker lands in well under this;
 * a spawn over a hole simply gives up and falls the ordinary way. */
#define Q2_SETTLE_STEPS  180

void q2_sim_settle(q2_sim *sim)
{
    q2_player *p;
    q2_input  in;
    int i;

    if (!sim || !sim->coll_ready)
        return;

    p = &sim->player[sim->cur_player];

    /*
     * Through the TICK, not through `q2_move_step` directly. The mover needs
     * the state the tick sets up around it — the entity's node above all, which
     * a fresh spawn has not established, so a bare move resolves against
     * nothing and the player does not budge. Driving the real path costs a
     * dozen ticks of level clock at a level start and gets the landing, the
     * grounded flag and the contact normal exactly right.
     */
    memset(&in, 0, sizeof(in));

    sim->settling = true;
    for (i = 0; i < Q2_SETTLE_STEPS; i++) {
        q2_sim_tick(sim, &in, Q2_DT_NOMINAL);
        if (p->on_ground)
            break;
    }
    sim->settling = false;

    /*
     * However it ended, the player is not mid-fall: keeping the velocity and
     * the fall accumulator would charge them damage on the first tick for a
     * drop they never saw, and leave the landing view-kick to play over the
     * first frame the player sees.
     */
    p->vel[0] = p->vel[1] = p->vel[2] = 0;
    p->frame_delta[0] = p->frame_delta[1] = p->frame_delta[2] = 0;
    p->fall_value  = 0;
    p->fall_time   = 0;
    p->ground_y    = p->pos[1];
    p->view_height = Q2_VIEW_STAND;
    p->pitch       = 0;
    p->roll        = 0;
}

/* ------------------------------------------------------------------------- */
void q2_sim_eye(const q2_sim *sim, s32 out_pos[3])
{
    if (!sim || !out_pos)
        return;

    out_pos[0] = sim->player[sim->cur_player].pos[0];
    /*
     * `feet - viewOffset`, which IS the console's `origin + 286 - viewOffset`.
     *
     * 0x80038618 builds the view position by copying entity+0x54 and +0x5C
     * straight through and computing the middle component:
     *
     *     80038630  lw    v0, 88(a1)      entity+0x58
     *     80038634  lh    v1, 246(a1)     entity+0xF6, the eased view offset
     *     80038638  addiu v0, v0, 286
     *     8003863C  subu  v0, v0, v1
     *
     * entity+0x58 is the ENTITY ORIGIN, not the feet — the point the mover
     * works in, 286 above the feet (see the note at q2_sim_origin_y). Fold that
     * in and the two 286s cancel:
     *
     *     eye = (feet - 286) + 286 - viewOffset = feet - viewOffset
     *
     * and standing, at 576, puts the eye 576 above the feet, which is the
     * player's own height. `player.pos` here is the feet, so the subtraction
     * below is the whole expression and the constant does not appear.
     *
     * ADDING 286 here is wrong and was briefly done: it reads the disassembly
     * with entity+0x58 taken for the feet, which puts the eye 290 above them —
     * half the player's height, a crouch. The tell is the crouch case rather
     * than the standing one: at viewOffset 286 the expression collapses to
     * `eye = origin`, the middle of the player, which is where a crouched eye
     * belongs and is nonsense if the base is the feet.
     *
     * The weapon in the hands must come out at the SAME point, and it does not
     * get there by this function — see the note in q2_vw_place, which converts
     * the other way for the same reason.
     */
    out_pos[1] = sim->player[sim->cur_player].pos[1]
               - sim->player[sim->cur_player].view_height;
    out_pos[2] = sim->player[sim->cur_player].pos[2];
}

/* ------------------------------------------------------------------------- */
/* 0x80038260 — the view angles                                               */
/* ------------------------------------------------------------------------- */
/*
 * How much of a kick is left, in 1.0.12.
 *
 * `(remaining << 12) / period`, and the shift comes FIRST: the original forms
 * the fixed-point numerator and then divides, so a kick with 7 of 30 ticks left
 * is 955/4096 rather than 0. Dividing first and scaling after — the obvious
 * way — quantises every kick to nothing.
 *
 * A deadline in the past yields nothing at all, and the original memsets the
 * amplitudes to zero when it does (0x80038328). This does not, because the
 * amplitudes are read nowhere else and clearing them would make the composer
 * mutate the player it is handed.
 */
static s32 kick_scale(s32 deadline, s32 now, s32 period)
{
    s32 remaining = deadline - now;

    if (remaining < 0)
        return 0;
    return (remaining << Q2_FRAC_12) / period;
}

void q2_sim_view_angles(const q2_sim *sim, s32 out[3])
{
    const q2_player *p;
    s32 s;

    if (!out)
        return;

    out[0] = out[1] = out[2] = 0;

    if (!sim)
        return;

    p = &sim->player[sim->cur_player];

    out[0] = p->pitch;
    out[1] = p->yaw;
    out[2] = p->roll;

    /* 0x800382A4 — the firing kick, all three axes, over 30 ticks. */
    s = kick_scale(p->kick_time, sim->level_time, Q2_VIEW_KICK_FIRE);
    if (s) {
        out[0] += ((s32)p->kick[0] * s) >> Q2_FRAC_12;
        out[1] += ((s32)p->kick[1] * s) >> Q2_FRAC_12;
        out[2] += ((s32)p->kick[2] * s) >> Q2_FRAC_12;
    }

    /* 0x80038334 — the damage kick, pitch and roll only, over 150. */
    s = kick_scale(p->pain_time, sim->level_time, Q2_VIEW_KICK_HURT);
    if (s) {
        out[0] += ((s32)p->hurt_kick[0] * s) >> Q2_FRAC_12;
        out[2] += ((s32)p->hurt_kick[1] * s) >> Q2_FRAC_12;
    }

    /* 0x800383A4 — the landing kick, pitch alone, over 90. */
    s = kick_scale(p->fall_time, sim->level_time, Q2_VIEW_KICK_FALL);
    if (s)
        out[0] += ((s32)p->fall_value * s) >> Q2_FRAC_12;
}
