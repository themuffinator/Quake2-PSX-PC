/*
 * creworld.h — a level's live creatures.
 *
 * Every piece of this existed and nothing joined them up. `reloc.[ch]`
 * relocates a `CreAIBin` module, `creature.[ch]` decodes one, `crebind.[ch]`
 * joins it to an implementation, `spawn.[ch]` turns Population records into
 * monsters and `ai.[ch]` runs them — but the only caller was the inspector,
 * which decodes a module to report on it and draws a creature at its spawn
 * point without ever ticking one. The client had no monsters at all: a level
 * was its geometry, its items and nothing that moves.
 *
 * This is the join. Give it a map's COMMON.DAT and it produces a set of live
 * creatures with their AI running.
 *
 * ---------------------------------------------------------------------------
 * Three numbers name a creature, and they are not the same number
 * ---------------------------------------------------------------------------
 * This is the part that has to be got right, so it is written down.
 *
 *   - a Population spawn record carries a **class id**, 0..37, which indexes
 *     the engine's module registry (spawn.h);
 *   - the **class table** in the executable turns that id into a NAME and a
 *     health (classtable.h) — and names are not unique: ids 18, 19 and 20 are
 *     all `Soldier`, with 30, 20 and 40 health;
 *   - a module serves one or more **class bytes**, 64..94, and `Soldier`'s
 *     three are 87, 89 and 88 in that order (creature.h).
 *
 * The module is found by matching its 12-byte header name against the class
 * table's name, which is the direction the engine's own lookup runs
 * (`q2_class_find_name`). The VARIANT — which of the module's class bytes a
 * given spawn is — is taken from the position of that id among the class table
 * entries sharing the name: the first `Soldier` record gets class_byte[0], the
 * second class_byte[1], and so on. Three ids, three class bytes, in the order
 * both tables state them. That correspondence is inferred from the two tables
 * agreeing in length and order rather than read out of code, and it is the one
 * inference in this file.
 *
 * ---------------------------------------------------------------------------
 * What a live creature does and does not do here
 * ---------------------------------------------------------------------------
 * It notices the player, turns, chases, remembers where it last saw them, and
 * plays its own animations off the disc. Its health is the class table's.
 *
 * It collides with the world too, through `aiworld.[ch]` bound to the zone's own
 * `PrimaryColl` — sight, stepping and the ground probe all. That was not true
 * when this file was written: the binding read `q2_coll_move`'s "stopped" return
 * as "could not start", so every creature was told it was buried in the floor it
 * stood on and none of them moved. See openquestions #48.
 *
 * Implemented modules use transcribed callbacks and frame methods through
 * crebind. Unsupported modules remain outside the gameplay registry.
 */
#ifndef Q2PSX_CREWORLD_H
#define Q2PSX_CREWORLD_H

#include "classtable.h"
#include "crebind.h"
#include "creature.h"
#include "dat.h"
#include "disc.h"
#include "ident.h"
#include "level.h"
#include "monster.h"
#include "population.h"
#include "q2psx.h"
#include "spawn.h"

/* The disc contains 22 distinct module names, including zone-local AI. */
#define Q2_CREWORLD_MAX_MODULES 32

/*
 * Model-clip ticks per AI animation frame.
 *
 * Not read out of code — measured. Every one of the Soldier's 31 CastList clip
 * lengths is divisible by three, and dividing by three turns them into the
 * lengths of its module's moves: clips 1..4 are exactly the four consecutive
 * moves 50-54, 55-61, 62-79 and 80-96. `Q2_MODEL_TICKS_PER_FRAME` is 10 and is
 * the view weapon's rate, which is a different thing.
 *
 * How the engine picks a clip is no longer a question: it does not pick one.
 * `0x8006B924` walks the clip list subtracting lengths from a running position,
 * so the clips are one timeline and this scale is what puts an AI frame on it.
 * See `q2_model_anim_at` and openquestions #47.
 */
#define Q2_CRE_TICKS_PER_FRAME 3

/*
 * Where a module image is relocated to. Far from the executable's own address
 * space so a pointer that escaped the module is obvious rather than plausible —
 * the same base the inspector uses, so an address printed by one means the
 * same thing in the other.
 */
#define Q2_CREWORLD_BASE 0x80100000u

typedef struct q2_creature_module {
    char         name[13];
    u8          *image;                       /* relocated body, owned here */
    size_t       size;
    q2_creature  cre;
    q2_cre_bind  bind;
    q2_cre_think think[Q2_CLASS_METHOD_COUNT];

    /*
     * The module's own name for each move, pointing into `image`. Kept here
     * because the bind does not own the image and the names must outlive the
     * call that reads them. They are how the ENGINE reaches an animation --
     * 0x8006D330 walks block D comparing 12-byte names -- so the draw path
     * needs them to pose a creature the way the disc does.
     */
    const char  *move_name[Q2_CRE_MAX_MOVES];

    /*
     * The same table read WHOLE and indexed by frame, which is how the engine
     * actually reaches an animation. `move_name` above pairs records with
     * moves, and a move can span several records — the Soldier's 12-frame
     * attack covers four (Fire 1 Ready/Aim/Shoot/Done) — so the per-move view
     * comes back empty for exactly the moves a creature lives in. See
     * q2_cre_frame_name.
     */
    q2_cre_frame_name frame_name[Q2_CRE_MAX_MOVES];
    u32               frame_name_count;

    bool         ready;
    bool         sounds_ready;
    u32          sound_count;
    q2_cre_sound_bind sound_bind[128];
    q2_sfx_request sound_request[128];
} q2_creature_module;

/* Resolve the actual initializer once per loaded world. Ordinary requests
 * alias the bank; explicit clones retain their module's separate identity. */
struct q2_creature_world;
void q2_creature_world_init_sound_requests(struct q2_creature_world *w,
    bool (*bank_has)(const char *, void *), void *user,
    q2_sfx_request *(*lookup)(const char *, void *));
q2_sfx_request *q2_creature_world_sound_request(struct q2_creature_world *w,
    const q2_monster *m, u32 addr, const char *registered,
    bool (*bank_has)(const char *, void *), void *user,
    q2_sfx_request *(*lookup)(const char *, void *), const char **name);

typedef struct q2_creature_world {
    q2_creature_module mod[Q2_CREWORLD_MAX_MODULES];
    u32                mod_count;

    q2_class_table     classes;
    bool               classes_ready;

    /* class_id -> module, and which of that module's class bytes it is. */
    s8                 class_module[Q2_MONSTER_CLASS_COUNT];
    u8                 class_variant[Q2_MONSTER_CLASS_COUNT];

    q2_monster_set     set;
    q2_spawn_stats     stats;

    /* Borrow the live world objects, not their replaceable target arrays.
     * Scripted batches settle against their state when summoned. */
    q2_collision          *placement_coll;
    const q2_move_world    *placement_world;

    /* Kept rather than dropped, because CREBATCH names a GROUP and the names
     * live here. Borrows COMMON.DAT, which outlives the zone. */
    q2_population      pop;
    bool               pop_ready;

    /*
     * The class table name each placed creature resolved to, one per monster.
     *
     * It has to be kept, because `q2_creature_spawn` OVERWRITES `class_id` with
     * the module's class BYTE — 87 for a Soldier, not the Population id 18 —
     * since that byte is what `q2_cre_bind_for` keys the runtime bind on. After
     * spawning there is no longer anything on the monster that indexes the
     * class table, so the name is taken while it is still there.
     */
    char             (*model_name)[13];

    /*
     * The player, as the AI sees one. The sim owns the real player; this is the
     * entity FindTarget acquires, moved to the player's eye every tick. It is
     * held here because `q2_level_state.sight_client` is a pointer the AI keeps
     * across ticks and it must not dangle.
     */
    q2_monster         sight;

    /*
     * THE PLAYER'S TWO NOISE ENTITIES — PlayerNoise (0x80062B80) spawns them on
     * the first noise and keeps them at who+0xC8 and who+0xCC: class 201, the
     * player as owner (+0x48), svflags 1, and no client (+0x0C) and no object
     * (+0x24), so no health. The noise is where it was made, frozen there:
     * [0] carries types 0 and 1 (the body and the guns) for `sound_entity`, [1]
     * type 2 (impacts) for `sound2_entity`.
     *
     * They are not the sight client, and that difference is the behaviour.
     * A creature hunting one walks to where the noise was rather than to where
     * the player is, does not tell the others (FoundTarget publishes a sighting
     * only for a client, 0x8005D11C), still runs its sight checks on the player
     * and turns on them when it sees them, and after 50 ticks finds the noise
     * "dead" and goes back to what it was doing. Pointing both slots at the
     * sight client gave none of the four.
     */
    q2_monster         noise[2];

    /*
     * THE MAP'S PATH CORNERS — the "PathCorner" group, spawned by 0x8007F390.
     *
     * They are ordinary entities on the console and G_PickTarget hands one
     * back as one, so they are q2_monster here too: monster_start_go reads
     * `class_id` and `pos` off whatever the resolver returns. They are NOT in
     * `set`, and that is deliberate on three counts — `set.count` is the kill
     * denominator the mission HUD divides by, the per-index model and actor
     * arrays in the client are sized from it, and `class_module[]` is indexed
     * by the Population class 0..37 while a corner's 114 belongs to the other
     * class namespace entirely.
     *
     * The array never grows after the load, because q2_pick_target hands out
     * pointers into it and monster_start_go keeps them as movetarget.
     */
    q2_monster        *corner;
    u32                corner_count;

    bool               ready;
} q2_creature_world;

/*
 * HALF THE SIDE OF A PATH CORNER'S TRIGGER CUBE.
 *
 * A corner has no stored hull and is never linked, so its absolute bounds are
 * invented on demand: CalcAbsFromCreBox (Q_SHARED.C 0x8005CB00) short-circuits
 * on the class byte 114 and writes origin - 96 / origin + 96 on all three axes
 * (0x8005CB28, 0x8005CB38, 0x8005CB48 and 0x8005CB58, 0x8005CB68, 0x8005CB78).
 * The per-axis accessors agree — getminsvx/y/z and getmaxsvx/y/z at 0x8005CBBC,
 * 0x8005CBF0, 0x8005CC24, 0x8005CC58, 0x8005CC8C and 0x8005CCC0 return the same
 * ∓96 for a corner, which is where corner_touch's teleport correction gets its
 * mins.y.
 *
 * So arriving at a corner is "the creature's hull overlaps a 192-unit cube",
 * not "the corner's point is inside the creature's hull".
 */
#define Q2_CORNER_ABS_EXTENT  96

/*
 * Has this creature reached that corner? — BoxEdicts' per-candidate overlap,
 * 0x8005C81C (CalcAbsFromCreBox on the candidate) followed by 0x8005C828
 * (VectBoxOverlap), for the one candidate class this port keeps in an array
 * rather than an area tree.
 *
 * Exposed because it is the whole of what "arrival" means and it used to be
 * wrong: the test was `corner->pos` inside the creature's hull, a 286-unit
 * window per axis against the console's 382, and with no vertical slack at all
 * for a corner authored below the floor a creature settles on.
 */
bool q2_creature_corner_overlaps(const q2_monster *m, const q2_monster *corner);

/*
 * Decode every creature module this map ships, and place every spawn record
 * whose class resolves to one of them.
 *
 * `common` and optional `zone` need only outlive the call — modules are copied
 * and relocated into storage this owns. Returns Q2_OK even when a map places no
 * creatures; `w->stats` says what happened.
 *
 * `coll` is the zone's SecondaryCol and is what DROPS each creature onto its
 * floor — the console's shared placement sweep. Passing NULL leaves every
 * creature at the authored height, which is a hint rather than a position.
 * `world` supplies resident solid platforms for the same placement sweep.
 * These two world objects must outlive this creature world; delayed batches
 * borrow them to place fresh creatures after platforms/scene solids change.
 */
q2_result q2_creature_world_load(q2_creature_world *w, const disc *d,
                                 const q2_build_id *id,
                                 const q2_common_file *common,
                                 const q2_zone_file *zone,
                                 q2_collision *coll, const q2_move_world *world);

/*
 * Initialize and wake active creatures and point the AI at the player. Dormant
 * batches wait for summon; saved runtime resumes without repeating export 0.
 * Call once the player is standing where the level starts them.
 */
/*
 * `player_origin` is the player's ENTITY ORIGIN — feet - Q2_EYE_BASE, which is
 * what sim.c already keeps in `player.ent.pos` — NOT the eye. The sight
 * client's own view height converts, exactly as 0x8005B950 does for every
 * other entity. Passing the eye adds the view height twice.
 */
void q2_creature_world_wake(q2_creature_world *w, const s32 player_origin[3],
                            s16 player_health);

/*
 * One AI tick. `player_origin` moves the sight client first, so a creature that
 * looks this tick looks at where the player is now.
 *
 * Returns how many creatures thought, which is zero on a level with none.
 */
u32 q2_creature_world_tick(q2_creature_world *w, const s32 player_origin[3],
                           s16 player_health);
/* Actor handlers installed by a module run on the next engine frame, outside
 * the ten-Hz animation/AI clock (Medic's deferred replacement). */
void q2_creature_world_actor_tick(q2_creature_world *w);

/*
 * The player made a noise at `where`, and creatures may look for it.
 *
 * The console's three types: 0 is the body (the jump 0x8003E208, the dive
 * 0x8003D2B8 and the surfacing 0x8003D460, all at the entity origin), 1 a gun
 * (nine fire functions; not the hand grenade's or the BFG's), 2 an impact
 * (DoNoise 0x8007F15C, for a player's bullet and bolt). Below 2 goes to
 * `sound_entity`; 2 to `sound2_entity`, which an AMBUSH creature ignores.
 * Walking and landing raise nothing. See the note on the definition.
 */
#define Q2_PNOISE_SELF    0
#define Q2_PNOISE_WEAPON  1
#define Q2_PNOISE_IMPACT  2
void q2_creature_world_player_noise(q2_creature_world *w, const s32 where[3],
                                    int type);

/* The model name for a live creature — the class table's name, which is also
 * the name its model carries in the map's CastList. NULL when unresolvable. */
const char *q2_creature_world_model_name(const q2_creature_world *w,
                                         const q2_monster *m);

/*
 * The name record covering `frame`, out of this creature's own module table.
 * The draw wants this rather than the per-move name: a move can span several
 * records, and the ones a creature spends its time in do.
 */
const q2_cre_frame_name *q2_creature_world_frame_name(
        const q2_creature_world *w, const q2_monster *m, s32 frame);

/*
 * The first frame of this creature's death move, or -1 when its module carries
 * no named move that says death. Pair it with `q2_cre_set_move` to put a body
 * into its death animation instead of deleting it.
 */
s32 q2_creature_world_death_frame(const q2_creature_world *w,
                                  const q2_monster *m);

/*
 * The name of sound `index` for this creature, out of its own module's table,
 * or NULL. Every module carries one; only the Soldier's had been read.
 */
const char *q2_creature_world_sound_name(const q2_creature_world *w,
                                         const q2_monster *m, u32 index);

/*
 * The sound a play site ASKS FOR, resolved the way the module resolves it.
 *
 * A decoded SOUND action carries the module ADDRESS of the handle
 * (`q2_cre_action.addr`, creature.h) and `cre_actions.c` passes that address
 * straight through — so treating it as an index into a name list, which this
 * port did, asks for name 0x80101758 of eight. `q2_creature_sound_bindings`
 * decodes the module's own registrations into address/name pairs; this is the
 * lookup that joins the two ends together.
 */
const char *q2_creature_world_sound_for_addr(const q2_creature_world *w,
                                             const q2_monster *m, u32 addr);
const char *q2_creature_world_sound_resolve(const q2_creature_world *w,
                                            const q2_monster *m, u32 addr,
                                            bool (*bank_has)(const char *, void *),
                                            void *user);
/* Compatibility for transcriptions whose play sites still pass a sound index.
 * Resolve that registration's slot, then execute the same bank-dependent init. */
const char *q2_creature_world_sound_resolve_name(const q2_creature_world *w,
        const q2_monster *m, const char *registered,
        bool (*bank_has)(const char *, void *), void *user);


/*
 * SUMMON a Population group — what `CREBATCH` means.
 *
 * A group is not spawned because it exists. `0x80056C60` takes a twelve-byte
 * name, finds the group and sets bit 0 of its flags; the spawn pass runs only
 * the selected ones and sets bit 1 so a group cannot run twice
 * (population.h). The flags word is ZERO ON DISC for all 222 groups of all 49
 * maps, so at load nothing is selected.
 *
 * This port spawns every record up front and holds the ones a script owns
 * DORMANT — `in_use` clear and `spawn_pending` set — because the alternative is re-running the spawn
 * pass mid-level against a set other systems already hold pointers into. The
 * effect is the console's: an ambush is not standing there until it is called
 * for. Waking also runs `q2_monster_start_go`, since dormant records were
 * deliberately skipped by the initial world wake. Returns how many creatures
 * this call woke, which is 0 for a group already summoned — the bit-1 latch.
 * Geometrically excluded and freed records never acquire the pending latch.
 * An explicit selection has no zone-name test, matching retail CREBATCH.
 */
u32 q2_creature_world_summon(q2_creature_world *w, const char *group);

/*
 * Hold back every creature whose group is a script's rather than the level's.
 *
 * WHICH groups stand there is the map's LevelBin's answer, and it is now read
 * rather than guessed at (levelbin.h): `q2_levelbin_selected` decodes the
 * module's calls to the engine's group selector and hands back the names. Pass
 * the module and it is used.
 *
 * Without one, the fallback is the naming rule #79 used — a group called
 * `Zone<N>` is that zone's own population and anything else is a batch. That
 * rule is not a guess any more either: across the disc the modules make 83
 * selector calls, 71 resolve to a real group, and **69 of those 71 claim a
 * zone**. The two that do not are JAIL3's `Jail4Return` and `Jail5Return` —
 * populations for coming BACK to a level, which the module selects
 * conditionally and a static scan cannot gate. The client uses hold_selected
 * with the same mission/key decisions it used for item startup.
 *
 * Call after the world has loaded and before it wakes. Returns how many were
 * held.
 */
u32 q2_creature_world_hold_batches(q2_creature_world *w, int resident_zone,
                                   const u8 *levelbin, u32 levelbin_size);

/* Use the owner's resolved startup selection (one byte per Population group).
 * Items and creatures then share conditional LevelBin decisions. Call after
 * geometric zone filtering and before wake; excluded records stay excluded. */
u32 q2_creature_world_hold_selected(q2_creature_world *w, int resident_zone,
                                     const u8 *selected, u32 count);

void q2_creature_world_free(q2_creature_world *w);

#endif /* Q2PSX_CREWORLD_H */
