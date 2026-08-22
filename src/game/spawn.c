#include "spawn.h"

#include "entity.h"       /* q2_entity_drop_to_floor: the shared spawn drop */
#include "worldscale.h"   /* Q2_EYE_BASE: the record is the feet */

#include <stdlib.h>
#include <string.h>

void q2_monster_set_free(q2_monster_set *set)
{
    if (!set)
        return;
    free(set->monsters);
    memset(set, 0, sizeof(*set));
}

void q2_monster_set_register(q2_monster_set *set, u32 class_id)
{
    if (!set || class_id >= Q2_MONSTER_CLASS_COUNT)
        return;
    set->class_present[class_id] = true;
}

static q2_monster *monster_push(q2_monster_set *set)
{
    if (set->count >= set->capacity) {
        u32 want = set->capacity ? set->capacity * 2 : 32;
        q2_monster *bigger =
            (q2_monster *)realloc(set->monsters, want * sizeof(q2_monster));
        if (!bigger)
            return NULL;
        set->monsters = bigger;
        set->capacity = want;
    }

    {
        q2_monster *m = &set->monsters[set->count++];
        q2_monster_init(m);
        return m;
    }
}

q2_result q2_spawn_from_population(q2_monster_set *out, const q2_population *pop,
                                   q2_collision *coll, q2_spawn_stats *stats)
{
    q2_spawn_stats local;
    u32 gi;

    if (!out || !pop)
        return Q2_ERR_INVALID_ARG;

    memset(&local, 0, sizeof(local));

    for (gi = 0; gi < pop->group_count; gi++) {
        q2_pop_group g;
        u32 slot;

        if (!q2_pop_get_group(pop, gi, &g))
            continue;

        /* Path groups carry patrol nodes, not creatures: same stride, entirely
         * different fields. Walking them as spawns would produce monsters at
         * nonsense coordinates. */
        if (q2_pop_group_is_path(&g))
            continue;

        for (slot = 0; ; slot++) {
            q2_pop_spawn rec;
            q2_monster *m;

            if (!q2_pop_get_spawn(pop, &g, slot, &rec))
                break;

            local.records++;

            if (rec.class_id >= Q2_MONSTER_CLASS_COUNT) {
                local.out_of_range++;
                continue;
            }

            /* No module for this class means the creature cannot act. Counting
             * it separately keeps "this level has no monsters" distinguishable
             * from "we failed to resolve its monsters". */
            if (!out->class_present[rec.class_id]) {
                local.no_module++;
                continue;
            }

            m = monster_push(out);
            if (!m) {
                q2_monster_set_free(out);
                return Q2_ERR_NO_MEMORY;
            }

            m->group  = (u16)gi;
            m->pos[0] = rec.x;
            /*
             * THE RECORD'S Y IS THE FEET. `m->pos` is the entity ORIGIN, which
             * is Q2_EYE_BASE above them.
             *
             * This is the shared placement convention, not an item quirk:
             * 0x80050FA0 is called by the item spawner, the save restore and
             * three others, and at 0x80051068 it does `addiu v0, v0, -286` to
             * the record's Y before writing the entity. sim.c already lifts the
             * player (`q2_sim_origin_y`) and item.c already lifts an item
             * (`- Q2_ITEM_SPAWN_LIFT`); creatures were the one entity class
             * that never got it.
             *
             * What it cost: everything downstream — the movement hull, the step
             * trace, M_CheckBottom's 502, the sight line — is defined in the
             * origin frame, so a creature was being reasoned about 286 units
             * below where it was. Fed the eroded hull, 214 of 214 spawns
             * resolved to no cell; lifted, 21 of 22 on four maps are inside it.
             * That measurement is why PrimaryColl was wired in as the movement
             * hull in the first place (openquestions #48) — a sound experiment
             * on a wrong premise.
             */
            /*
             * AND THE FURTHER 30, AND THE DROP. The 286 alone is the frame
             * conversion; the shared placement routine 0x80050FA0 also nudges
             * by 30 (0x800510A4) and then sweeps DOWN by the distance its
             * caller passes. The creature spawner's call at 0x8003B5C4 takes
             * that distance from the word at 0x800AE894, whose halfwords are
             * 0xFFFF and 0x0400 — a zone hint of -1 and 1024 units, the same
             * distance the item spawner passes.
             *
             * This is why "monsters stuck in geometry" survived two rounds of
             * collision fixes that were each individually correct. The hull
             * select, the four-corner CheckBottom and the three-trace
             * movestep were all reasoning correctly about a creature that was
             * never put on the floor in the first place: the authored Y is a
             * HINT and the engine settles it.
             */
            m->pos[1] = rec.y - Q2_EYE_BASE - Q2_ENTITY_SPAWN_NUDGE;
            m->pos[2] = rec.z;

            if (coll && !q2_entity_drop_to_floor(coll, m->pos,
                                                 Q2_ENTITY_DROP_DIST, NULL)) {
                /*
                 * 0x8003B624 error-loops here. The port cannot, and must not
                 * drop the record either: Population is per MAP and a session
                 * is in one ZONE, so most of what fails to place is simply a
                 * creature standing in another zone's rooms — and the caller's
                 * own geometric filter is what decides that, after this. A
                 * CREBATCH can also summon one of them later.
                 *
                 * So it is COUNTED and left at its authored height, which is
                 * exactly where it was before this drop existed.
                 */
                local.unplaced++;
            }

            /* The stored angle is INFERRED to be on the 4096-step circle: the
             * observed range is 0..3958 and the four dominant values are close
             * to the quarter turns. */
            m->angles[2] = (s16)rec.angle;
            m->ideal_yaw = m->angles[2];

            m->class_id   = (u8)rec.class_id;
            m->health     = Q2_SPAWN_DEFAULT_HEALTH;
            m->max_health = Q2_SPAWN_DEFAULT_HEALTH;
            m->gib_health = -Q2_SPAWN_DEFAULT_HEALTH / 2;

            /*
             * AND THE OTHER HALFWORD THE FILLER CARRIES.  0x8007E608 loads
             * Population.spawn.link and 0x8007E614 stores it straight into
             * entity+0x16: there is no sentinel conversion.  In particular,
             * the common 0xFFFF arrives as signed -1.  `monster_start_go`
             * deliberately tests `target > 0`, while an individual module
             * may inspect the raw non-zero value for a different purpose.
             */
            m->target = (s16)rec.link;

            /*
             * THE MAP'S SPAWN FLAGS ARE NOT A MODULE DETAIL.
             *
             * Before the module's spawn export runs, 0x8007E61C..0x8007E62C
             * copies the low nine bits of Population.spawn.flags into bits
             * 18..26 of entity+0x1C:
             *
             *   flags = (flags & 0xF803FFFF) | ((record.flags & 0x1FF) << 18)
             *
             * The high seven source bits are deliberately discarded.  The
             * preserved part contains ordinary entity state, including the
             * in-use bit which is raised below.  This is shared placement
             * state, not a special case for the Insane: its spawn code is
             * simply the first module whose four variants make the missing
             * hand-off conspicuous.
             */
            m->spawnflags = (m->spawnflags & 0xF803FFFFu) |
                            (((u32)rec.flags & Q2_POP_SPAWN_FLAGS_MASK)
                             << Q2_POP_SPAWN_FLAGS_SHIFT);
            m->in_use     = true;
            m->spawnflags |= Q2_SVFLAG_INUSE;

            local.placed++;
        }
    }

    if (stats)
        *stats = local;

    return Q2_OK;
}

/* ------------------------------------------------------------------------- */
/* Driving the creatures                                                      */
/* ------------------------------------------------------------------------- */
void q2_monster_set_wake(q2_monster_set *set, q2_monster *player)
{
    u32 i;

    q2_level_state.sight_client = player;

    if (!set)
        return;

    for (i = 0; i < set->count; i++) {
        q2_monster *m = &set->monsters[i];
        if (!m->in_use || m->dead)
            continue;
        q2_monster_start_go(m);
    }
}

u32 q2_monster_set_tick(q2_monster_set *set)
{
    u32 i, ran = 0;

    /*
     * Both counters move together. `framenum` is what the sight and sound
     * alert windows compare against and `time` is what every timer is measured
     * in; the original increments them as one and separating them would make
     * FindTarget's one-tick window either always or never open.
     */
    q2_level_state.framenum++;
    q2_level_state.time++;

    if (!set)
        return 0;

    for (i = 0; i < set->count; i++) {
        q2_monster *m = &set->monsters[i];

        if (!m->in_use)
            continue;

        /*
         * A corpse still animates. It does NOT think: the AI is what decides
         * where to walk and who to shoot, and a dead creature does neither —
         * but its death move has to play out, and it only plays if something
         * advances the frame. Running the driver alone is the whole of the
         * difference between a body that falls and a body that freezes
         * mid-stride on the frame it was killed, which is what this did before.
         *
         * `q2_M_MoveFrame` stops of its own accord at a move with no next, so a
         * finished death animation holds its last frame rather than looping.
         */
        if (m->dead) {
            /*
             * ...AND IT STOPS WHEN THE MODULE SAYS SO. This used to drive the
             * frame regardless of `next_think`, which meant a body could never
             * be told to stop.
             *
             * Both `*_dead` and every gib arm end with `next_think = 0` — the
             * Soldier's at module+0x2344, and the console's frame loop then
             * skips the think outright (`blez v1, ...` at 0x8007EE88 on
             * entity+0x90). That zero IS the stop signal, and it is most of
             * what "gibbed" even means on this disc: the Soldier's gib arm
             * plays no sound and throws no pieces, it sets deadflag,
             * MOVETYPE_TOSS, SVF_DEADMONSTER and this.
             *
             * `q2_M_MoveFrame` re-arms `next_think` on entry, so a corpse still
             * animates its death through to the end; the zero only takes effect
             * once something writes it after the driver has run.
             */
            /*
             * AND ONCE THE MODULE RAISES SVF_DEADMONSTER THE BODY DETACHES,
             * which is the console's own corpse path and which this port had
             * no step for at all.
             *
             * Every `*_dead` sets that bit, and on the console `0x8007EC28`
             * answers it by calling `0x8007F098`: the edict is freed, the
             * collision volume is rescaled, the class becomes 47 and the
             * actor's per-tick handler is swapped for the corpse one. See
             * `q2_monster_corpse_detach` for the whole of it.
             */
            if ((m->svflags & Q2_SVF_DEADMONSTER) && !m->corpse)
                q2_monster_corpse_detach(m);

            if (m->corpse) {
                q2_monster_corpse_tick(m);
                ran++;
                continue;
            }

            if (m->currentmove && m->next_think != 0) {
                q2_M_MoveFrame(m);
                ran++;
            }
            continue;
        }

        if (!m->think)
            continue;
        if (m->next_think > q2_level_state.time)
            continue;

        m->think(m);
        ran++;
    }

    return ran;
}
