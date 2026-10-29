/* Physical actor state, distinct from the AI edict (SLES_015.34). */
#ifndef Q2PSX_CREMOTION_H
#define Q2PSX_CREMOTION_H
#include "trace.h"

typedef struct q2_cre_motion {
    q2_move_ent ent;            /* actor +0x54 */
    s32 target[3];             /* actor +0x20, LinkEntity destination */
    s16 velocity[3];           /* +0xE0, never the edict's velocity */
    s16 delta[3];              /* +0xEC */
    s16 impulse[3];            /* +0x2F8 */
    s16 yaw, target_yaw;       /* +0xE8, +0xDA */
    u32 flags;                 /* +0x10C; 0x4000 arms the impulse */
    s32 clock;                 /* edict +0x30/+0x34 */
    u8 phase_base, phase;      /* edict +0x2C/+0x2D, modulo 30 */
    bool ready;
    u16 anim_base;             /* actor +0xB0 bits 17..31 */
    s16 anim_previous, anim_position; /* edict +0x2E, actor +0x100 */
    u8 anim_flags;             /* +0xB0 bits 0/1: blend, transition */
    s32 queued_yaw;            /* +0x2E8, applied after a transition */
    bool animation_ready, advanced;
    s16 seed_frame;            /* deferred model lookup for 0x8007F474 */
    bool seed_pending;
} q2_cre_motion;

typedef struct q2_cre_motion_world {
    q2_collision *coll;
    const q2_move_world *world;
    const q2_move_bodies *bodies;
    s32 self_id, gravity;
    bool multiplayer, suspended;
} q2_cre_motion_world;

struct q2_monster;
struct q2_area_graph;
struct q2_scene;
/* Native area flood (0x800651D4) and actor activity gate (0x800578D8). */
void q2_cre_active_areas(const struct q2_area_graph *graph,
                         const struct q2_scene *scene, const u8 *hidden,
                         u32 hidden_count, u8 camera_area, bool all, u32 mask[3]);
bool q2_cre_motion_suspend(struct q2_monster *m, const u32 mask[3],
                            s32 area, bool visible, const s32 camera[3]);
void q2_cre_motion_init(struct q2_monster *m);
void q2_cre_motion_link(struct q2_monster *m, u32 what);
const s32 *q2_cre_origin(const struct q2_monster *m);
s16 q2_cre_yaw(const struct q2_monster *m);
/* Caller supplies the general AI clock. Each actor installs its own clock
 * during thinks, then restores the caller's clock. dt is in 1/300 seconds. */
u32 q2_cre_motion_tick(struct q2_monster *m, s32 dt,
                       const q2_cre_motion_world *world);
/* 0x8007E9DC. timeline is the named record's resolved model position. */
void q2_cre_motion_animate(struct q2_monster *m, s16 first, s16 last,
                          s16 timeline);
#endif
