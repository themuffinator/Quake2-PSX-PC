/*
 * crebind.h — joining a decoded creature to a C implementation of it.
 *
 * `creature.[ch]` reads a module: its class bytes, its speed scale and mass,
 * its callbacks, and every move and frame it owns. None of that needs writing
 * by hand. What does need writing is the body of each think index — the sound,
 * the shot, the claw — and this is where the two meet.
 *
 * ---------------------------------------------------------------------------
 * A move is named by its first frame, not by an index
 * ---------------------------------------------------------------------------
 * The obvious binding is positional: move 0 is stand, move 1 is walk. It is
 * also fragile, because the decoder's order depends on which callback reached
 * a move first, and that changes the moment the decoder improves.
 *
 * A move's FIRST FRAME NUMBER is stable, is meaningful, and is what id's own
 * source names — `FRAME_stand101`, `FRAME_attak101`. So an implementation asks
 * for the move starting at frame 146 and gets the Soldier's stand1 whatever
 * order it was found in. If the disc does not have such a move the request
 * fails visibly rather than silently installing the wrong animation.
 *
 * ---------------------------------------------------------------------------
 * End callbacks resolve themselves
 * ---------------------------------------------------------------------------
 * A move's end callback is a module address, and the C side has no way to call
 * it. It does not need one: that address is almost always also a monsterinfo
 * callback or a method-table entry, both of which the implementation already
 * supplies. So the binder resolves an endfunc by looking the address up in
 * those two sets. An address in neither leaves the move looping, which is what
 * a move with no end callback does anyway.
 */
#ifndef Q2PSX_CREBIND_H
#define Q2PSX_CREBIND_H

#include "creature.h"
#include "monster.h"
#include "q2psx.h"

/*
 * The hand-written half of a creature.
 *
 * `callback` is indexed exactly as `q2_creature.callback`: stand, idle,
 * search, walk, run, dodge, attack, melee, sight, checkattack, bigturn, pain,
 * die. `method` is indexed by the think byte in an animation frame.
 *
 * A NULL entry means "the module installs one here and this port does not
 * implement it yet" — which is a visible gap, not a crash: the frame driver
 * skips a NULL method and the AI skips a NULL callback.
 */
/*
 * Six of the thirteen slots have additional arguments or a return value:
 *
 *     5  dodge        void (*)(q2_monster *, q2_monster *other, s32 eta)
 *     8  sight        void (*)(q2_monster *, q2_monster *other)
 *     9  checkattack  bool (*)(q2_monster *)
 *    10  bigturn      void (*)(q2_monster *, s32 clamped_turn)
 *    11  pain         void (*)(q2_monster *, s16 damage)
 *    12  die          void (*)(q2_monster *, s16 damage)
 *
 * `q2_creature_spawn` installs each of those through its own signature rather
 * than through the flat table, so the entry here must be written with an
 * explicit cast — `(q2_class_method)(void *)soldier_pain`. Writing the bare
 * function name is a compile error, which is the right outcome: the alternative
 * is six separate tables for thirteen slots.
 */
typedef struct q2_cre_impl {
    const char *name;                       /* the module name it matches */
    q2_class_method callback[13];
    q2_class_method method[Q2_CLASS_METHOD_COUNT];

    /* Called after the framework's own spawn setup, for anything the module's
     * spawn function does beyond the callbacks — a Soldier picks its weapon
     * variant here. `variant` is the entity's skinnum. */
    void (*spawn)(q2_monster *m);
    bool scripted; /* No monster_start: direct think/use, no kill denominator. */
} q2_cre_impl;

/* A decoded creature joined to an implementation and ready to spawn. */
typedef struct q2_cre_bind {
    const q2_creature *cre;
    const q2_cre_impl *impl;
    q2_mmove           move[Q2_CRE_MAX_MOVES];
    u32                move_count;

    /* The module's name for each move, parallel to `move[]`. NULL entries are
     * normal: not every move is named. See q2_creature_bind_move_names. */
    const char *const *move_name;
    u32                move_name_count;
    bool               ready;

    /*
     * What each think index does, decoded from the module. Supplied by the
     * caller because decoding needs the relocated image, which the bind does
     * not keep. A creature with these bound performs its real per-frame
     * actions; one without them animates and does nothing.
     */
    const q2_cre_think *think;
    u32                 think_count;
    /* Flyer module+0x1760: shared transition selector, reset with the bind. */
    s32                 flyer_transition;
    u8                  strider_turn;
    u8                  strider_explosion_phase;
    u8                  boss1_explosion_phase;
    u8                  jorg_explosion_phase;
} q2_cre_bind;

typedef struct q2_cre_shared_state {
    bool present;
    s32 flyer_transition;
    u8 strider_turn, strider_explosion_phase, boss1_explosion_phase, jorg_explosion_phase;
} q2_cre_shared_state;
void q2_cre_shared_capture(q2_cre_shared_state *state, const q2_cre_bind *bind);
void q2_cre_shared_restore(q2_cre_bind *bind, const q2_cre_shared_state *state);

/* Attach a decoded action set. Call before spawning from this bind. */
/*
 * The module's own NAME for each decoded move, in move order.
 *
 * Supplied by the caller for the same reason the thinks are: reading them needs
 * the relocated image, which the bind does not keep. Names matter because they
 * are how the ENGINE reaches an animation — `0x8006D330` walks block D comparing
 * 12-byte names — so a move without one cannot be posed the way the disc does
 * it. `names[i]` may be NULL where the module names nothing.
 */
void q2_creature_bind_move_names(q2_cre_bind *b, const char *const *names,
                                 u32 count);

void q2_creature_bind_thinks(q2_cre_bind *b, const q2_cre_think *think,
                             u32 count);

/*
 * Bind and register.
 *
 * Installs `impl->method` into the class method table for every class byte the
 * module serves, and builds the runtime moves with their end callbacks
 * resolved. `b` must outlive every creature spawned from it.
 */
bool q2_creature_bind(q2_cre_bind *b, const q2_creature *c,
                      const q2_cre_impl *impl);

/* Set up `m` as one of this creature: class byte, scale, mass, callbacks. */
void q2_creature_spawn(q2_cre_bind *b, q2_monster *m, u32 class_index);
/* Bind a dormant population record without executing export 0 or using RNG. */
void q2_creature_prepare(q2_cre_bind *b, q2_monster *m, u32 class_index);

/*
 * The move whose animation starts at `first_frame`, or NULL.
 *
 * This is what an implementation's callbacks use, and it is deliberately a
 * lookup rather than an index: a missing animation is then a NULL that shows
 * up as a creature that does not play it, instead of a creature that plays
 * something else.
 */
const q2_mmove *q2_cre_find_move(const q2_monster *m, s32 first_frame);

/* Install that move on `m`. Returns false when the disc has no such move. */
bool q2_cre_set_move(q2_monster *m, s32 first_frame);

/*
 * The same, keyed on the move record's own MODULE ADDRESS.
 *
 * A first frame is the right key almost everywhere and it is not unique: the
 * Arachner's walk (module+0x17DC) and run (module+0x1808) both cover frames
 * 16-24 and differ only in the per-frame AI verb, and the Tank Commander's
 * walk (module+0x1BC0) and run (module+0x1C3C) both cover 34-49. Asking for
 * frame 16 gets whichever the decoder found first, so an Arachner told to run
 * would walk.
 *
 * Both transcriptions had grown a private copy of this before it was shared;
 * this is that copy, once. Use it only where the frame range is genuinely
 * ambiguous — `q2_cre_set_move` stays the readable form, and a module address
 * in a callback is a constant only the disassembly can justify.
 */
const q2_mmove *q2_cre_find_move_at(const q2_monster *m, u32 module_addr);
bool q2_cre_set_move_at(q2_monster *m, u32 module_addr);

/* Which bind a live creature belongs to, by its class byte. */
q2_cre_bind *q2_cre_bind_for(const q2_monster *m);
void         q2_cre_bind_reset(void);

/* Stable saved callback ids: 0 none, 1 M_MoveFrame, 2..7 scripted thinks,
 * 8 deferred Rider launch, 9 Medic revival on the next actor frame.
 * Unknown functions encode as 255 and are rejected by save capture. */
u8 q2_cre_think_id(const q2_monster *m);
bool q2_cre_think_valid(const q2_monster *m, u8 id);
void q2_cre_restore_think(q2_monster *m, u8 id);
void q2_cre_script_callbacks(q2_monster *m);
#define Q2_CRE_THINK_LAST 9
void q2_cre_rider_launch(q2_monster *m);
void q2_cre_medic_revive(q2_monster *m);
bool q2_cre_boss_checkattack(q2_monster *m, s32 cooldown);
q2_class_method q2_cre_method_for(const q2_monster *m, u32 index);
void q2_cre_set_named_sound_hook(void (*fn)(q2_monster *, const char *, void *),
                                 void *user);
void q2_cre_play_named_sound(q2_monster *m, const char *name);
void q2_cre_set_lifecycle_hooks(q2_monster *monsters, u32 count,
    q2_monster *(*spawn)(q2_monster *, u8, void *),
    bool (*revive)(q2_monster *, q2_monster *, void *), void *user);
q2_monster *q2_cre_roster(u32 *count);
q2_monster *q2_cre_spawn_child(q2_monster *m, u8 pop_class);
bool q2_cre_revive(q2_monster *medic, q2_monster *corpse);
/* Direct actor writes made by modules must reach combat/presentation before
 * another creature can damage or inspect that actor in the same AI tick. */
void q2_cre_set_actor_hooks(void (*effect)(q2_monster *, u32, u8, void *),
                            void (*spawn)(q2_monster *, void *), void *user);
void q2_cre_actor_effect(q2_monster *m, u32 index, u8 ticks);

/* The built-in implementations, NULL-terminated. */
const q2_cre_impl *const *q2_cre_impls(void);

/*
 * The two hooks a transcribed creature needs from outside itself: a sound and
 * a shot. Both are the engine's job in the original — the module reaches them
 * through its import table — and keeping them as hooks stops every creature
 * from having to know about combat.
 *
 * The fire hook receives a packed (weapon_table * 8 + flash_number), which is
 * how the Soldier's three muzzle-flash tables are distinguished.
 */
void q2_cre_set_sound_hook(void (*fn)(q2_monster *m, int which, void *user),
                           void *user);
/* A cloned request can change pitch without changing its registered sample.
 * The ordinary hook is the fallback for hosts without a pitched sound path.
 * Setting the ordinary hook clears this optional hook. */
void q2_cre_set_pitched_sound_hook(void (*fn)(q2_monster *, int, u8, void *),
                                   void *user);
void q2_cre_play_pitched_sound(q2_monster *m, u32 handle, u8 pitch);
/* The request belongs to m's module; the emitting entity may be another
 * actor (RiderStand uses the player). Preserve both identities. */
void q2_cre_set_sound_owner_hook(void (*fn)(q2_monster *, u32, const q2_monster *, void *),
                                 void *user);
void q2_cre_play_sound_owner(q2_monster *m, u32 handle, const q2_monster *owner);
/* fire_hit (0x80061118): the contact point and the separate kick after damage.
 * Impulse components are the signed halfwords added at entity+0x2F8. */
bool q2_cre_melee_contact(const q2_monster *m, const s32 aim[3], s32 point[3]);
void q2_cre_melee_impulse(q2_monster *victim, const s32 point[3], s32 kick,
                          s16 impulse[3]);
/* Direct damage (+0xF0) and posed beam attachment (+0x7C). These imports
 * are used by the Parasite and Medic independently of projectile firing. */
void q2_cre_set_contact_hooks(
    void (*hurt)(q2_monster *, q2_monster *, s16, int, const s32 *, void *),
    void (*beam)(q2_monster *, q2_monster *, s16, void *), void *user);
void q2_cre_hurt(q2_monster *self, q2_monster *target, s16 damage,
                  int mod, const s32 point[3]);
void q2_cre_beam(q2_monster *self, q2_monster *target, s16 vertex);
void q2_cre_set_explosion_hook(void (*fn)(q2_monster *, const s32 *, s32, void *),
                                 void *user);
void q2_cre_explosion(q2_monster *self, const s32 point[3], s32 scale);
void q2_cre_set_fire_hook(void (*fn)(q2_monster *m, int flash, void *user),
                          void *user);

/*
 * WHAT A CREATURE'S SOUND ACTUALLY PLAYS, given the name its slot was
 * REGISTERED with and what this map's bank carries.
 *
 * The sound hook hands the host either a module address (every decoded and
 * most transcribed creatures) or, for the Soldier, an index into its own name
 * table; either way the host turns it into the registered name first. Two
 * modules then substitute one handle for another in their own spawn code —
 * the Soldier's pain and death trios and the Tank Commander's idle handle, see
 * `q2_cre_sound_fallback` in creature.h — and that is this. It finds `m`'s
 * module through its bind and applies q2_creature_sound_fallback.
 *
 * `bank_has(name, user)` answers "does this map's SNDVRAM bank carry it". A
 * name outside every group comes back unchanged, so the call is safe to make
 * on every sound.
 */
const char *q2_cre_sound_resolve(const q2_monster *m, const char *registered,
                                 bool (*bank_has)(const char *name, void *user),
                                 void *user);

/* ------------------------------------------------------------------------- */
/*
 * A CREATURE'S SHOT, with the figures its own module carries.
 *
 * The old hook took a single `int`, which is why every creature but the
 * Soldier fired into nothing: it could name the spawner and not the shot. The
 * arguments the modules pass are now read for all seven, and every one of them
 * is id's own number —
 *
 *     Tankcomm  rocket   damage 50, speed 550     blaster damage 16, speed 800
 *               bullet   damage 20, kick 4, spread 300/500
 *     Gunner    grenade  damage 50, speed 600
 *               bullet   damage  3, kick 4, spread 300/500
 *     Infantry  bullet   damage  3, kick 4, spread 300/500
 *     Arachner  railgun  damage 50, kick 100
 *     Soldier   blaster  damage  5, speed 600
 *               shotgun  damage  2, kick 1, spread 1000/500, 12 pellets
 *               bullet   damage  2, kick 4, spread 300/500
 *
 * — `DEFAULT_BULLET_HSPREAD` 300 and `VSPREAD` 500 recurring across four
 * unrelated modules is the check that says the argument positions are right.
 *
 * THE ENGINE READS ONLY THE FIRST FOUR OF THEM. `monster_fire_bullet`
 * (0x80061DFC) and `monster_fire_shotgun` (0x80061ED0) take the module, the
 * start point, the aim and the damage in a0..a3 and touch no stack argument
 * of their own, so `kick`, `hspread`, `vspread` and `count` go no further
 * than this struct: the shotgun's pellet count is the engine's own 5
 * (0x80061FD4) and its spread is a fixed shift on `rand()`. They are carried
 * here because they are what the MODULE passes, which is what a
 * transcription records; a host should not expect them to mean anything on
 * the hitscan slots.
 *
 * `slot` is one of the `Q2_IMP_FIRE_*` offsets, so the host can tell a rocket
 * from a rail without the creature having to. A field a given weapon does not
 * use is zero, and a hook that finds `damage == 0` should decline rather than
 * invent: that is the state a creature whose figures are not read would be in.
 *
 * AND WHERE IT GOES, for a creature whose aim has been read. The spawners'
 * a1 and a2 are the muzzle and a 1.12 unit aim, and the engine traces the
 * shot from the one along the other (0x80061E2C scales the aim `<< 2`, so a
 * round reaches 16384 units). `aimed` says the creature filled `start` and
 * `dir` with what its module computes; the client rejects a shot without it
 * and counts it in the unsupported-fire diagnostic.
 * Without the direction a Soldier killed mid-stride shot the player through
 * the back of its own head: `soldier_fire` aims flashes 5 and 6 — the two
 * Death1 frames — straight along its own forward vector, not at anyone, and
 * the dying Infantry sweeps its burst up and across the ceiling.
 */
typedef struct q2_cre_shot {
    u32 slot;           /* Q2_IMP_FIRE_* — which engine spawner            */
    s32 flash;          /* the muzzle-flash index the module passes        */
    s32 damage;
    s32 speed;          /* projectiles: the bolt's, grenade's or rocket's  */
    s32 kick;
    s32 hspread;        /* hitscan spread, id's DEFAULT_* where it matches */
    s32 vspread;
    s32 count;          /* pellets; 1 for anything that is not a shotgun   */
    bool aimed;         /* start and dir below are the module's own        */
    s32 start[3];       /* a1: the posed muzzle, world units               */
    s32 dir[3];         /* a2: module aim; Flyer passes an unnormalized delta */
    u32 effects;        /* blaster wrapper: bit 0x40 selects hyper bolts  */
} q2_cre_shot;

/*
 * THE MUZZLE — import +0x2C (0x8006C6C8) posing the point the module cached at
 * load, then +0x28 rotating it by the object's matrix and the object's origin
 * added. The cache is `pose_blend_two_positions` (import +0x34, 0x8006CA2C)
 * over two model vertices: their midpoint when both lie in one part, the first
 * alone when they do not (0x8006CBA4). The Soldier blends 228 and 234
 * (module+0xF20..+0xF48), the Infantry 200 and 205.
 *
 * Only the host has a posed model, so it answers through a hook, with the pose
 * it last drew — at most a frame behind the think that asks, the same timing
 * the damage effects already accept (client_fx_pose). With no hook, or a
 * creature it cannot pose, the answer is the origin, which is where every
 * creature shot started before this existed.
 */
void q2_cre_set_muzzle_hook(bool (*fn)(const q2_monster *m, s16 vert_a,
                                       s16 vert_b, s32 out[3], void *user),
                            void *user);
void q2_cre_muzzle(const q2_monster *m, s16 vert_a, s16 vert_b, s32 out[3]);

/*
 * The muzzle in its two halves, for a module that works on the cache between
 * them — the Tank Commander lerps its rocket tubes (cre_tankcomm.c).
 *
 * `q2_cre_attach_blend` is import +0x34 alone: the 8-byte cache a module
 * keeps, three part-local halfwords and the part. It depends on the model and
 * not on the pose, so asking for it at the shot gives what the module cached
 * at load. `q2_cre_attach_place` is +0x2C and +0x28: that point posed, turned
 * by the object's matrix and put at its origin. Without the host's hooks the
 * blend fails and the place answers with the origin, as q2_cre_muzzle does.
 */
typedef struct q2_cre_attach {
    s16 off[3];
    u16 part;
} q2_cre_attach;

void q2_cre_set_attach_hooks(
    bool (*blend)(const q2_monster *m, s16 vert_a, s16 vert_b,
                  q2_cre_attach *out, void *user),
    bool (*place)(const q2_monster *m, const q2_cre_attach *a, s32 out[3],
                  void *user),
    void *user);
bool q2_cre_attach_blend(const q2_monster *m, s16 vert_a, s16 vert_b,
                         q2_cre_attach *out);
void q2_cre_attach_place(const q2_monster *m, const q2_cre_attach *a,
                         s32 out[3]);

void q2_cre_set_shot_hook(void (*fn)(q2_monster *m, const q2_cre_shot *shot,
                                     void *user),
                          void *user);

/* Fire one, from a transcribed creature. Does nothing without a hook. */
void q2_cre_fire_shot(q2_monster *m, const q2_cre_shot *shot);

/*
 * `fire_hit` — import +0xEC, 0x80061118 — the ONE thing a melee frame calls,
 * and the reason a melee swing goes through the engine rather than straight to
 * the host's hook.
 *
 * It opens with a range test, and the range test is the whole of what a module
 * cannot do for itself: 0x80061144..0x80061184 builds `enemy->origin -
 * self->origin`, 0x80061188 is `jal 0x8005C4E8` (VectorLength), and then
 *
 *     80061190  lw   v1, 188(sp)      ; a1, the aim's reach
 *     80061198  slt  v1, v1, s4       ; reach < range ?
 *     8006119C  bne  v1, zero, 0x800614B4
 *     800611A0  addu v0, zero, zero   ; delay slot: return FALSE
 *
 * — 0x800614B4 being the epilogue, so a claw whose target has moved beyond
 * `aim[0]` returns having done nothing at all. The comparison is STRICT, so
 * `range == aim[0]` still lands, and it is the raw origin-to-origin distance:
 * the mins/maxs work at 0x800611A4..0x80061220 runs AFTER the gate and only
 * shapes the hit point, so folding a hull into the test would move the reach.
 *
 * This matters because the AI's decision and the blow are not the same frame.
 * `M_CheckAttack` commits to melee at Q2_RANGE_MELEE, and the swing think runs
 * two or three 10 Hz frames later; a player who backs off in between is out of
 * reach by the time the claw resolves.
 *
 * The module draws its damage roll BEFORE the call (`5 + rand() % 6`), so a
 * swing that misses still consumes the draw — keep the roll in the argument
 * list at the call site rather than moving it in here.
 *
 * WHAT THIS DOES NOT DO, stated rather than hidden: the console's fire_hit also
 * applies the module's KICK itself, after T_Damage and outside it
 * (0x800613CC `VectorMA(zero_vec3, kick, unit_dir, &enemy->velocity)`, so the
 * victim's velocity is SET to `kick * unit`). No path in this port writes a
 * player's velocity that way, so `kick` still reaches the hook unused.
 */
void q2_cre_fire_hit(q2_monster *m, const s32 aim[3], s32 damage, s32 kick);

/* The melee hook the decoded creatures reach: `fire_hit` with the module's own
 * aim vector, damage and kick. */
void q2_cre_set_melee_hook(void (*fn)(q2_monster *m, const s32 aim[3],
                                      s32 damage, s32 kick, void *user),
                           void *user);

/* Run the decoded actions for one think index on `m`. */
/*
 * Where a decoded action went, so "it never fired" can be told apart from "it
 * never got that far". Cumulative; zero it before a run.
 */
typedef struct q2_cre_action_stats {
    u32 thinks_run;
    u32 thinks_unbound;      /* no decoded think for that index               */
    u32 calls_seen;          /* CALL steps reached                            */
    u32 calls_unclassified;  /* an import slot with no meaning yet            */
    u32 fire_calls;          /* CALL steps that named a projectile spawner,
                              * and q2_cre_fire_shot calls                    */
    u32 fire_sent;           /* ...and reached the hook                       */
    u32 fire_no_hook;

    /*
     * REFUSALS, and only the decoded action path makes them: the CALL arm of
     * cre_actions.c's run_step declines a shot with no enemy or a dead one
     * before its one-int hook. So `fire_sent + fire_no_hook + fire_no_enemy +
     * fire_dead_enemy == fire_calls`, and the census line that prints them
     * side by side can be read as a partition.
     */
    u32 fire_no_enemy;
    u32 fire_dead_enemy;

    /*
     * OBSERVATIONS, NOT GATES, for a shot that went through q2_cre_fire_shot.
     * A transcribed fire think fires with no enemy and at a dead one — that is
     * what the modules do (see the block in q2_cre_fire_shot) — so these count
     * how often it happened. Every shot counted here is ALSO in `fire_sent`;
     * they are kept apart from the two refusal counters above so that a shot
     * is never counted both as sent and as refused.
     */
    u32 shot_no_enemy;
    u32 shot_dead_enemy;

    /*
     * The melee swings, partitioned the way the fire counters are:
     * `melee_sent + melee_no_hook + melee_no_enemy + melee_short ==
     * melee_calls`. `melee_short` is the one the console itself refuses — the
     * range test at 0x80061198 — and it is worth a counter of its own because
     * it is the difference between a claw that can reach you and one that
     * cannot.
     */
    u32 melee_calls;
    u32 melee_sent;
    u32 melee_no_hook;
    u32 melee_no_enemy;
    u32 melee_short;

    /*
     * Which callback slots the generic implementation could and could not find
     * a move for — 0 stand, 1 idle, 2 search, 3 walk, 4 run, 6 attack,
     * 7 melee, 11 pain, 12 die.
     *
     * Sized to the creature's own callback table (`u32 callback[13]`) and NOT
     * to eight, which is what it was. Pain and die live at 11 and 12, so the
     * recorder's `slot < 8` guard dropped them both on the floor and the stats
     * line then read two words PAST the array to print them. Two subsystems
     * disagreeing about how many slots there are is exactly the kind of thing
     * a fixed 8 hides.
     */
    u32 move_via_set[Q2_CRE_CALLBACK_SLOTS];
    u32 move_via_missing[Q2_CRE_CALLBACK_SLOTS];

    /* How often each think index actually ran. A move whose fire think sits
     * six frames in only reaches it if the animation is not cut short. */
    u32 think_hits[32];
} q2_cre_action_stats;

extern q2_cre_action_stats q2_cre_actions;

/* The generic implementation and its handlers, so a PARTIAL transcription can
 * write one callback and keep the decoded actions for the rest. */
extern const q2_cre_impl q2_cre_generic;

void q2_cre_generic_stand(q2_monster *m);
void q2_cre_generic_idle(q2_monster *m);
void q2_cre_generic_search(q2_monster *m);
void q2_cre_generic_walk(q2_monster *m);
void q2_cre_generic_run(q2_monster *m);
void q2_cre_generic_attack(q2_monster *m);
void q2_cre_generic_melee(q2_monster *m);
void q2_cre_generic_pain(q2_monster *m);
void q2_cre_generic_die(q2_monster *m);

void q2_cre_run_think(q2_monster *m, u32 index);

/* Skill, which several creatures gate their refire and opening shot on. */
void q2_cre_set_skill(s32 skill);
s32  q2_cre_skill(void);

/*
 * The bank name for one of the Soldier's eleven sounds, or NULL. The module
 * carries the names itself, 12-byte fields at `module+0x1D0` parallel to the
 * handle table at `+0x32A0`; see cre_soldier.c.
 */
const char *q2_cre_soldier_sound_name(int which);

/* Find the implementation for a module name, or NULL when the port has none. */
const q2_cre_impl *q2_cre_impl_find(const char *module_name);

#endif /* Q2PSX_CREBIND_H */
