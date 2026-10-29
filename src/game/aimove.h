/*
 * aimove.h — how a creature actually gets from here to there.
 *
 * The PC lineage keeps this in m_move.c, and so does the PSX: one step
 * routine, one direction chooser, one goal follower, and the yaw integrator
 * they all lean on.
 *
 *     M_ChangeYaw        0x80060964
 *     M_walkmove         0x800607F8
 *     M_MoveToGoal       0x8006087C
 *     SV_StepDirection   0x80060334
 *     SV_NewChaseDir     0x80060544
 *     SV_movestep        0x8005FC78
 *     SV_CloseEnough     inline in M_MoveToGoal
 *
 * ---------------------------------------------------------------------------
 * What is worth knowing before reading the code
 * ---------------------------------------------------------------------------
 * **Step height is 216.** A creature pushes down from 216 above where it
 * wanted to be and accepts the trace's landing point, which is how it climbs
 * stairs without ever computing a slope. With AI_NOSTEP the height drops to
 * 12 — one PC unit — and the creature can only cross flat ground.
 *
 * **A step that turns too far is refused.** SV_StepDirection takes the step,
 * then throws the position away again if the yaw it ended up with is more than
 * 45 degrees off the direction it asked for. That single rule is what stops
 * monsters from sliding sideways along walls while facing you.
 *
 * **Chasing is eight-way, not continuous.** SV_NewChaseDir quantises to 45
 * degrees, tries the diagonal toward the target first, then the two axis
 * directions in an order that a coin flip and the larger delta decide between,
 * then every remaining direction in a randomly chosen rotation, and only then
 * the way it came. The deadband on each axis is 120 units, so a target almost
 * dead ahead produces no lateral component at all.
 *
 * **Flying and swimming creatures never step.** They get two attempts at a
 * direct move, the first with a vertical correction toward the goal's height
 * and the second without, and if both are blocked they simply do not move.
 */
#ifndef Q2PSX_AIMOVE_H
#define Q2PSX_AIMOVE_H

#include "ai.h"
#include "monster.h"
#include "q2psx.h"

/* Step heights, in world units. 216 is id's 18; 12 is id's 1. 0x8005FF0C. */
#define Q2_STEPSIZE        216
#define Q2_STEPSIZE_NOSTEP  12

/* The eight-way quantum and the deadband on each axis. 0x800605C0. */
#define Q2_CHASE_STEP      512
#define Q2_CHASE_DEADBAND  120
#define Q2_DI_NODIR        (-1)

/*
 * Turn toward `ideal_yaw` by at most `yaw_speed`, the short way round.
 * Returns true only when the requested turn was clamped to yaw_speed. The
 * native s1 flag stays zero when this step reaches the ideal angle.
 *
 * The big-turn hook fires here: when the turn the creature was about to make
 * exceeds `bigturn_threshold`, the callback runs INSTEAD of the turn.
 */
bool q2_M_ChangeYaw(q2_monster *m);

/* Move `dist` along `yaw`, through the full step logic. */
bool q2_M_walkmove(q2_monster *m, s32 yaw, s32 dist);

/* Follow `goalentity`, bumping around obstacles. Does nothing once the next
 * step would already touch the enemy. */
void q2_M_MoveToGoal(q2_monster *m, s32 dist);

/* True when a step of `dist` would already put `self` in contact with `goal`. */
bool q2_SV_CloseEnough(const q2_monster *self, const q2_monster *goal, s32 dist);

bool q2_SV_StepDirection(q2_monster *m, s32 yaw, s32 dist);
void q2_SV_NewChaseDir(q2_monster *actor, q2_monster *enemy, s32 dist);

/* The one routine that actually moves a creature. `relink` is the original's
 * second argument: true from M_walkmove, false from SV_StepDirection, which is
 * how a refused step avoids firing triggers on the way through. */
bool q2_SV_movestep(q2_monster *m, const s32 move[3], bool relink);

/* Ground under enough corners to stand on. 0x8005FB24, routed through the
 * world hook so a stand-in world can answer it. */
bool q2_M_CheckBottom(q2_monster *m);

/* The two engine services a completed step calls, exposed so the port can
 * hang its own linking and trigger touching off them. */
void q2_link_entity(q2_monster *m, u32 what);       /* 0x8005C8C8 */
void q2_touch_triggers(q2_monster *m);              /* 0x8005F4A8 */

typedef void (*q2_link_fn)(q2_monster *m, u32 what, void *user);
typedef void (*q2_touch_fn)(q2_monster *m, void *user);
void q2_ai_set_link_hooks(q2_link_fn link, q2_touch_fn touch, void *user);

#endif /* Q2PSX_AIMOVE_H */
