/*
 * spawn.h — placing creatures in a level from the Population records.
 *
 * The last link in the chain. Population's spawn list gives a position, an
 * angle and a class_id; the class_id indexes the engine's 38-entry module
 * registry; the module holds that creature's AI. So a spawn record plus a
 * relocated module is a live monster.
 *
 * ---------------------------------------------------------------------------
 * Why class_id can be trusted
 * ---------------------------------------------------------------------------
 * It was an open question for a long time: 25 distinct values in 0..37 whose
 * meaning was unknown, and which demonstrably was NOT a ModelNames index (15 of
 * 673 records exceeded the map's name count, and resolving them by name gave
 * nonsense).
 *
 * The registry settles it. The engine stores each loaded module into a table
 * indexed by the instance's class id, and that table has exactly 38 slots —
 * the same range the spawn records use, bound-checked at the read site. Two
 * independent numbers agreeing at 38 is what makes this a link rather than a
 * coincidence.
 *
 * ---------------------------------------------------------------------------
 * What a spawned creature can and cannot do
 * ---------------------------------------------------------------------------
 * It exists, occupies a position, faces a direction, has health, can be shot
 * and killed — and it now runs the engine's AI: it notices the player, turns,
 * chases through the level, remembers where it last saw you and hunts down
 * your trail. All of that is engine code and is reconstructed in `ai.[ch]`.
 *
 * Its ANIMATIONS come from its own module: `creature.[ch]` follows the
 * module's code to its moves and frames and `crebind.[ch]` hands them to the
 * frame driver, so a creature plays the right walk cycle and the right death
 * without any of it being transcribed.
 *
 * What is still per-creature code is the body of each think function — the
 * shot, the sound, the claw. The Soldier's are written; the other six modules
 * run on the generic implementation, which animates and chases correctly and
 * performs no per-frame action. So a Tank Commander will hunt you down and
 * swing at you without ever firing, which is a visible partial rather than a
 * hidden one.
 *
 * q2_spawn_stats reports how many were placed and how many had no module,
 * because the difference between "no monsters here" and "monsters we could
 * not resolve" is worth being able to see.
 */
#ifndef Q2PSX_SPAWN_H
#define Q2PSX_SPAWN_H

#include "collision.h"
#include "monster.h"
#include "population.h"
#include "q2psx.h"
#include "reloc.h"

/* Default health when a creature's own value is not known. Deliberately not a
 * guess at any particular monster's health — it is a placeholder that makes a
 * spawned creature killable so the rest of the chain can be exercised. */
#define Q2_SPAWN_DEFAULT_HEALTH 100

typedef struct q2_spawn_stats {
    u32 records;        /* spawn records seen                    */
    u32 placed;         /* creatures created                     */
    u32 no_module;      /* class_id with no registered module    */
    u32 out_of_range;   /* class_id >= 38, which should never happen */
    /* The placement drop could not find a cell for the start point at all.
     * The console error-loops on this; the port leaves the creature out and
     * counts it, because a creature the hull cannot place is exactly what
     * three reports of "stuck in geometry" have been about. */
    u32 unplaced;
    /*
     * Records the current difficulty removes outright — SpawnGeneral's
     * 0x800565E8..0x80056664 mask, applied through q2_pop_spawn_allows_skill.
     * Counted separately so the "N of M spawn records placed" line stays
     * honest: these are not failures, they are records this skill does not
     * have.
     */
    u32 skill_filtered;
} q2_spawn_stats;

typedef struct q2_monster_set {
    q2_monster *monsters;
    u32         count;
    u32         capacity;

    /* Which class ids have a module. Indexed by class_id. */
    bool        class_present[Q2_MONSTER_CLASS_COUNT];
} q2_monster_set;

void q2_monster_set_free(q2_monster_set *set);

/* Record that a class has a module available, so spawning can tell a missing
 * creature from an unregistered one. */
void q2_monster_set_register(q2_monster_set *set, u32 class_id);

/* Resolve one authored feet position against the current resident geometry.
 * Used at level load and again when a dormant script batch actually appears. */
bool q2_spawn_place(const q2_pop_spawn *record, q2_collision *coll,
                     const q2_move_world *world, s32 origin[3]);

/*
 * Create creatures from every non-path spawn group in the population.
 *
 * Records whose class_id has no registered module are still counted but not
 * placed, so the caller can see the shortfall rather than silently getting
 * fewer monsters than the level intends.
 */
/*
 * `coll` is the zone's SecondaryCol, and it is what puts a creature ON THE
 * FLOOR: the console's shared placement routine sweeps 1024 units down from
 * the nudged record height and takes the Y it stops at. NULL skips the drop,
 * which is right for a census and wrong for a live level. `world` adds the
 * solid platforms and scene boxes that are absent from this collision mesh.
 */
q2_result q2_spawn_from_population(q2_monster_set *out, const q2_population *pop,
                                   q2_collision *coll, const q2_move_world *world,
                                   q2_spawn_stats *stats);

/*
 * Wake every placed creature: monster_start_go on each, which points its think
 * at the frame driver and schedules it for the next tick.
 *
 * `player` is registered as the level's CLIENT ROSTER — q2_ai_set_clients — and
 * the sight client is then recomputed from it by the rotation, here once and
 * again on every tick. It may be NULL, in which case nothing will ever be
 * acquired; and a player who is already dead leaves the sight client NULL,
 * which is correct, because nothing may notice a corpse.
 *
 * It used to say "`player` becomes the level's sight client". Still true in
 * effect with one client, but the mechanism is the roster plus AI_SetSightClient
 * (0x8005EC60) now, and with more than one client the rotation is what picks.
 */
void q2_monster_set_wake(q2_monster_set *set, q2_monster *player);

/*
 * Advance the AI clock by one tick and run every creature whose think is due.
 *
 * This is the engine's own loop shape: the clock moves, then each entity with
 * `think` set and `next_think` reached is called once. A think that reschedules
 * itself for the same tick is NOT run again, which is what stops a creature
 * whose animation ends on its own first frame from spinning.
 *
 * Returns how many creatures thought.
 */
u32 q2_monster_set_tick(q2_monster_set *set);

#endif /* Q2PSX_SPAWN_H */
