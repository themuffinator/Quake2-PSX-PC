/*
 * ai.h — the shared creature AI: noticing, chasing, attacking.
 *
 * This is the transcription of the executable's own AI, not an approximation
 * of it. Function by function:
 *
 *     ai_stand          0x8005CDA8      FindTarget        0x8005D334
 *     ai_walk           0x8005ED6C      FoundTarget       0x8005D104
 *     ai_run            0x8005E350      ai_checkattack    0x8005DCFC
 *     ai_charge         0x8005EE84      visible           0x8005B950
 *     ai_move           0x8005ED44      range/infront     inline, see monster.h
 *     ai_run_slide      inline in ai_run at 0x8005E460
 *     ai_run_missile    inline in ai_checkattack at 0x8005E264
 *     ai_run_melee      inline in ai_checkattack at 0x8005E2AC
 *     PlayerTrail_*     0x80060BBC / 0x80060F90
 *
 * The five verbs reach a module through the import table the loader builds at
 * 0x8007D990: +0x54 run, +0x58 walk, +0x5C stand, +0x60 move, +0x64 charge.
 * So a creature module does not implement chasing — it asks for it, which is
 * why reconstructing this once makes every creature on the disc behave.
 *
 * ---------------------------------------------------------------------------
 * The state model
 * ---------------------------------------------------------------------------
 * A creature's state is its current move plus the aiflags word. Transitions
 * come from target acquisition and from ai_checkattack, and the interesting
 * part is what happens when the target goes out of sight: aiflags picks up
 * AI_LOST_SIGHT and AI_PURSUIT_LAST_SEEN, the creature walks to where it last
 * saw the player, then follows the player's own breadcrumb trail, then peeks
 * around corners by tracing two sidesteps and taking the one with more room.
 * All three stages are in ai_run and all three are reproduced here.
 *
 * ---------------------------------------------------------------------------
 * Line of sight
 * ---------------------------------------------------------------------------
 * `visible` is a real trace in the original, and the port takes one too — but
 * through the `q2_ai_world` hook below rather than by reaching into the
 * collision model directly, so the AI can be exercised with a stand-in world
 * and still be the same code that runs against a real one. The default world
 * is open: everything is visible, nothing blocks, the floor is flat. That is a
 * deliberate choice of failure — monsters that see through walls are obvious,
 * where monsters that are silently blind look like the AI is broken.
 *
 * ---------------------------------------------------------------------------
 * The globals the AI shares between its own functions
 * ---------------------------------------------------------------------------
 * ai_checkattack computes four values and leaves them in globals for whatever
 * runs next, exactly as the original does at gp+17868..17880. They are part of
 * the contract: a creature's own checkattack reads enemy_range and
 * enemy_infront to decide whether to swing or to fire.
 */
#ifndef Q2PSX_AI_H
#define Q2PSX_AI_H

#include "monster.h"
#include "q2psx.h"

/* ------------------------------------------------------------------------- */
/* The world the AI asks about                                                */
/* ------------------------------------------------------------------------- */
typedef struct q2_ai_trace {
    s32  fraction;      /* 1.12; 4096 is a clear run                          */
    s32  endpos[3];
    bool allsolid;
    bool startsolid;
    q2_monster *ent;    /* what was hit, NULL for the world                   */
} q2_ai_trace;

#define Q2_TRACE_ONE 4096

typedef struct q2_ai_world {
    void *user;

    /* Box trace. `ignore` is never hit. `mask` is one of the Q2_MASK_*. */
    void (*trace)(void *user, const s32 start[3], const s16 mins[3],
                  const s16 maxs[3], const s32 end[3],
                  const q2_monster *ignore, u32 mask, q2_ai_trace *out);

    /* Point-to-point sight test, which is what `visible` needs and nothing
     * more. Separate from `trace` because the original's is a different
     * routine (0x80044C44) against a different structure. */
    bool (*line_of_sight)(void *user, const s32 a[3], const s32 b[3]);

    /* True when the entity has ground under enough of its corners.
     * M_CheckBottom, 0x8005FB24. */
    bool (*check_bottom)(void *user, const q2_monster *m);
} q2_ai_world;

/* Install the world the AI queries. Passing NULL restores the open stand-in. */
void q2_ai_set_world(const q2_ai_world *w);
const q2_ai_world *q2_ai_get_world(void);

/* ------------------------------------------------------------------------- */
/* The globals ai_checkattack publishes                                       */
/* ------------------------------------------------------------------------- */
extern bool          q2_enemy_vis;      /* gp+17868 */
extern s16           q2_enemy_yaw;      /* gp+17872 */
extern bool          q2_enemy_infront;  /* gp+17876 */
extern q2_range_band q2_enemy_range;    /* gp+17880 */

/* ------------------------------------------------------------------------- */
/* Vector and angle helpers, in the original's fixed point                    */
/* ------------------------------------------------------------------------- */
s32  q2_anglemod(s32 a);                            /* 0x8005C7B8 */
s16  q2_vectoyaw(const s32 v[3]);                   /* 0x8005F8E8 */

/*
 * The default checkattack, 0x8005D8C8 — installed on every creature at spawn
 * (0x80061B18) and overridden by no module on the disc, so it decides every
 * attack in the game.
 */
bool q2_M_CheckAttack(q2_monster *m);
s32  q2_vector_length(const s32 v[3]);              /* 0x8005C4E8 */
s64  q2_vector_length_sq(const s32 v[3]);           /* 0x8005C59C */
s32  q2_vector_normalize(s32 v[3]);                 /* 0x8005C634 */
void q2_angle_vectors(const s16 angles[3], s32 forward[3], s32 right[3],
                      s32 up[3]);                   /* 0x8005BB58 */
void q2_vector_ma(const s32 a[3], s32 scale, const s32 b[3],
                  s32 out[3]);                      /* 0x8005C460 */
/* Pitch in out[0], yaw in out[2], roll written zero. */
void q2_vectoangles(const s32 v[3], s16 out[3]);    /* 0x8005F934 */
void q2_project_source(const s32 point[3], const s16 distance[3],
                       const s32 forward[3], const s32 right[3],
                       s32 out[3]);                 /* 0x8005F5FC */

/* ------------------------------------------------------------------------- */
/* Perception                                                                 */
/* ------------------------------------------------------------------------- */
bool q2_visible(const q2_monster *self, const q2_monster *other);

/* ------------------------------------------------------------------------- */
/* The client roster, and the rotation over it                                */
/* ------------------------------------------------------------------------- */
/*
 * game.maxclients, 0x800B2C2C. The executable writes it in five places and
 * only ever writes 1, 2 or 4: 1 for single player (0x80077668, 0x80077D38),
 * 2 for the two-player modes (0x80077960, 0x80077B48) and 4 for the
 * four-player one (0x80077778). Four is therefore the ceiling.
 */
#define Q2_AI_MAX_CLIENTS 4

/*
 * Register the clients the sight rotation walks — the console's
 * g_edicts[1..game.maxclients] (0x800B2FD0, 0x800B2C2C). The pointers are
 * copied, so the caller's array need not outlive the call; the entities must.
 * Passing NULL, or a count of zero, empties the roster.
 */
void q2_ai_set_clients(q2_monster *const *clients, s32 maxclients);

/*
 * AI_SetSightClient, 0x8005EC60 — rotate level.sight_client onto the next
 * client that is in use, alive and not FL_NOTARGET, or NULL when none is.
 *
 * QuakeThinkGeneral calls this every tick, immediately after bumping
 * level.framenum and level.time (0x8007E27C), and it is the whole reason a
 * dead player stops being acquirable: FindTarget's default arm has no health
 * test of its own because on the console it never needed one.
 */
void q2_ai_set_sight_client(void);

/*
 * Look for a target. Returns true on acquisition, having already run
 * FoundTarget and the sight callback.
 *
 * The whole of id's FindTarget is here, including the three ways a monster can
 * be alerted — another monster seeing the player, a loud noise, a quiet noise —
 * and the ambush spawnflag that suppresses two of them.
 */
bool q2_find_target(q2_monster *self);

void q2_found_target(q2_monster *self);     /* 0x8005D104 */
void q2_hunt_target(q2_monster *self);      /* inline in FoundTarget */

/*
 * Who a hurt creature turns on — 0x80062654, called from T_Damage at
 * 0x80062AC0 for anything with SVF_MONSTER set, before the pain handler.
 *
 * This is the ONLY writer of `oldenemy` in the whole original, which is why
 * `ai_run`'s fall-back to it never fired here: nothing had ever set it.
 */
void q2_m_react_to_damage(q2_monster *targ, q2_monster *attacker);

/* self->attack_finished = level.time + t. 0x800622D0. */
void q2_attack_finished(q2_monster *self, s32 t);

/* ------------------------------------------------------------------------- */
/* The player's breadcrumb trail                                              */
/* ------------------------------------------------------------------------- */
#define Q2_TRAIL_LENGTH 8

/*
 * One 20-byte slot of the ring at 0x800D50DC, reached through the pointer
 * array at 0x800D517C. PlayerTrail_Init (0x80060F20) zeroes all eight, so
 * there is no "written yet" byte to carry: an unvisited slot IS a crumb at the
 * world origin with timestamp 0.
 */
typedef struct q2_trail_spot {
    s32 origin[3];
    s32 timestamp;      /* +0x0C */
    s16 yaw;            /* +0x10, the direction of travel between crumbs */
} q2_trail_spot;

/* Portable save state for the pursuit ring, including its next write slot. */
typedef struct q2_trail_state {
    q2_trail_spot spot[Q2_TRAIL_LENGTH];
    u32 head;
    bool active;
} q2_trail_state;

void q2_trail_capture(q2_trail_state *out);
bool q2_trail_restore(const q2_trail_state *state);

void q2_trail_init(void);

/* 0x80060A70. The yaw is derived here, from the step since the previous
 * crumb, and is not the caller's view angle. */
void q2_trail_add(const s32 origin[3]);
const q2_trail_spot *q2_trail_pick_first(q2_monster *self);   /* 0x80060BBC */
const q2_trail_spot *q2_trail_pick_next(q2_monster *self);    /* 0x80060F90 */
const q2_trail_spot *q2_trail_last_spot(void);                /* 0x80060FFC */

/* The gate PlayerTrail_Add's caller applies at 0x8007E2D8: false while the
 * player can still see the previous crumb. `eye` is the player's eye. */
bool q2_trail_needs_spot(const s32 eye[3]);

/* ------------------------------------------------------------------------- */
/* The five shared verbs. `dist` is the frame's advance, already scaled.       */
/* ------------------------------------------------------------------------- */
void q2_ai_stand(q2_monster *m, s32 dist);
void q2_ai_walk(q2_monster *m, s32 dist);
void q2_ai_run(q2_monster *m, s32 dist);
void q2_ai_charge(q2_monster *m, s32 dist);
void q2_ai_move(q2_monster *m, s32 dist);

/* The verb table itself, in the executable's slot order, so the frame driver
 * can index it the way the original does. Slot 0 is NULL. */
typedef void (*q2_ai_verb_fn)(q2_monster *m, s32 dist);
extern const q2_ai_verb_fn q2_ai_verbs[8];

/*
 * The gatekeeper every verb runs through. Returns true when it has taken over
 * the tick — the enemy died, an attack fired, the creature went back to
 * standing — and the calling verb must do nothing else.
 */
/*
 * Why a creature did or did not attack. "It never fires" has several causes and
 * only one of them is the firing code: it may never be asked, asked and refused,
 * or granted and then have no attack callback to run.
 */
typedef struct q2_ai_decision_stats {
    u32 checkattack_calls;    /* ai_checkattack reached                       */
    u32 checkattack_blind;    /* ...and the enemy was not visible             */
    u32 checkattack_decided;  /* ...and the decision function ran             */
    u32 checkattack_yes;      /* ...and said yes                              */
    u32 attack_called;        /* the attack callback ran                      */
    u32 attack_missing;       /* the state said attack and there was none     */
    /*
     * And why a lost-you pursuit did or did not move along. A creature that
     * reaches its waypoint raises AI_PURSUE_NEXT and asks the breadcrumb ring
     * for another; if the ring keeps answering with the same spot, or the spot
     * is in a different coordinate frame from the creature so the arrival test
     * can never fire, the search never leaves its first waypoint and this pair
     * stays near zero.
     */
    u32 pursue_arrived;       /* ai_run reached its waypoint                  */
    u32 pursue_marker;        /* ...and a breadcrumb answered with the next   */
} q2_ai_decision_stats;

extern q2_ai_decision_stats q2_ai_stats;

bool q2_ai_checkattack(q2_monster *m, s32 dist);

/* Sidestep at ninety degrees, alternating hands on a block. Reachable through
 * ai_run when attack_state is AS_SLIDING; exposed because a creature's own
 * checkattack sets that state and it reads better named. */
void q2_ai_run_slide(q2_monster *m, s32 dist);
void q2_ai_run_missile(q2_monster *m);
void q2_ai_run_melee(q2_monster *m);

/* True when the creature is facing its ideal yaw closely enough to act:
 * the turn left to make is 45 degrees or less either way. */
bool q2_facing_ideal(const q2_monster *m);

/* Caller context for Strider's native unassigned-s2 exact-yaw branch.
 * M_MoveFrame carries zero at move-end callbacks and the local frame index
 * during AI/frame callbacks; ai_run replaces it with the nonzero player
 * pointer. Scoped by the callers, never persistent actor/save state. */
extern u32 q2_ai_native_s2;

#endif /* Q2PSX_AI_H */
