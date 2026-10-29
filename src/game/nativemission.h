/* Retail LevelBin mission state: unit save-memory words outside EVE_<map>,
 * plus module-local clocks retained by the port's saved-game snapshots. */
#ifndef Q2PSX_NATIVEMISSION_H
#define Q2PSX_NATIVEMISSION_H
#include "q2psx.h"

enum {
    Q2_NATIVE_BRIDGE       = 1u << 0,
    Q2_NATIVE_COMP1        = 1u << 1,
    Q2_NATIVE_COMP2        = 1u << 2,
    Q2_NATIVE_STAIRS0      = 1u << 3,
    Q2_NATIVE_STAIRS1      = 1u << 4,
    Q2_NATIVE_LASER0_OFF   = 1u << 5,
    Q2_NATIVE_LASER1_OFF   = 1u << 6,
    Q2_NATIVE_PUMP1        = 1u << 7,
    Q2_NATIVE_PUMP2        = 1u << 8,
    Q2_NATIVE_ORB_ACTIVE   = 1u << 9,
    Q2_NATIVE_ORB_DONE     = 1u << 10,
    Q2_NATIVE_LOCK_LIFTS   = 1u << 11,
    Q2_NATIVE_STOP_PLATFORM = 1u << 12,
    Q2_NATIVE_PROJECTION_USED = 1u << 13,
    Q2_NATIVE_COMMANDER_USED = 1u << 14,
    Q2_NATIVE_SECURITY_BOSS = 1u << 15,
    Q2_NATIVE_SECURITY_HATCH = 1u << 16,
    Q2_NATIVE_TRAIN_OPEN   = 1u << 17,
    Q2_NATIVE_ROCK_LASER_OFF = 1u << 18,
    Q2_NATIVE_REACTOR_ACTIVE = 1u << 19,
    Q2_NATIVE_REACTOR_DONE = 1u << 20,
    Q2_NATIVE_REACTOR_DRAINED = 1u << 21,
    Q2_NATIVE_REACTOR_BEAM_OFF = 1u << 22,
    Q2_NATIVE_BASE3_WALL_OFF = 1u << 23,
    Q2_NATIVE_SECURITY_LIFT_HELD = 1u << 24, /* unit 2 save word +44 */
    Q2_NATIVE_FLAGS_ALL    = (1u << 25) - 1u
};

/* Stable disk order: NMIS v5 contains the two LAB entries; v6 adds the five
 * SECURITY/BOSS entries. Each retail entry is 76 bytes including pointers,
 * which are rebound by Scene name instead of being stored in the port. */
enum {
    Q2_NATIVE_LIFT_LAB_A, Q2_NATIVE_LIFT_LAB_C,
    Q2_NATIVE_LIFT_SECURITY_A, Q2_NATIVE_LIFT_SECURITY_B,
    Q2_NATIVE_LIFT_BOSS1_A,
    Q2_NATIVE_LIFT_BOSS2_A, Q2_NATIVE_LIFT_BOSS2_B,
    Q2_NATIVE_LIFT_COUNT
};

typedef struct q2_native_lift {
    u8 valid, state;
    u16 wait;
    s32 offset;
    s32 min[3], max[3], env_min[3], env_max[3], dy;
} q2_native_lift;

#define Q2_NATIVE_QUAKE_INACTIVE (-9999)

typedef struct q2_native_mission {
    u32 flags;
    s32 lab_phase[2];       /* LAB +0x16B0: 8192, decremented by 2*dt to >= -59 */
    s32 boss_phase[2];      /* BOSS1 +0x1E5A/+0x1E3E, 15*dt / 12*dt */
    struct {
        s16 velocity[3], life;
        s32 to[3];
    } orb_arc[5];           /* SECURITY +0x2700, five 20-byte records */
    s32 security_wall[4];   /* SECURITY +0x26E8, delayed wall explosions */
    u32 reactor_start;      /* POWER2 +0x1C10, on the saved level clock */
    s32 reactor_alarm;      /* POWER2 +0x1C08, 300-tick alarm accumulator */
    u32 reactor_glass;      /* consumed entries of POWER2 +0x1BE4 */
    u32 power1_next_blast;  /* POWER1 +0xC98: strict unsigned deadline */
    u32 power1_blast_phase; /* POWER1 +0xC68: 16-step shaft sequence */
    q2_native_lift lift[Q2_NATIVE_LIFT_COUNT];
    s32 security_quake_phase; /* +0x276C; active while wall 4 is pending */
    s32 boss2_quake_time;     /* +0x14D0: -9999 idle, otherwise -3000..3000 */
    s32 boss2_quake_phase;    /* +0x14E8; both pulse phases are -29..0 */
} q2_native_mission;

static inline bool q2_native_mission_valid(const q2_native_mission *s)
{
    u32 i, k;
    if (!s || (s->flags & ~Q2_NATIVE_FLAGS_ALL)) return false;
    for (i = 0; i < 2; i++)
        if (s->lab_phase[i] < -59 || s->lab_phase[i] > 8192 ||
            s->boss_phase[i] < 0 || s->boss_phase[i] > 4095) return false;
    for (i = 0; i < 5; i++) {
        /* A positive timer is decremented by at most 30 before all five
         * beams are submitted. The final frame can therefore store -29. */
        if (s->orb_arc[i].life < -29 || s->orb_arc[i].life > 1800) return false;
        for (k = 0; k < 3; k++)
            if (s->orb_arc[i].velocity[k] < -64 || s->orb_arc[i].velocity[k] > 64 ||
                s->orb_arc[i].to[k] < -16777216 || s->orb_arc[i].to[k] > 16777216)
                return false;
    }
    for (i = 0; i < 4; i++)
        if (s->security_wall[i] < 0 || s->security_wall[i] > 600 + 450 * (s32)i)
            return false;
    if (s->reactor_alarm < 0 || s->reactor_alarm > 300 || (s->reactor_glass & ~63u))
        return false;
    if (s->power1_blast_phase > 15) return false;
    if (s->security_quake_phase < -29 || s->security_quake_phase > 0 ||
        s->boss2_quake_phase < -29 || s->boss2_quake_phase > 0 ||
        (s->boss2_quake_time != Q2_NATIVE_QUAKE_INACTIVE &&
         (s->boss2_quake_time < -3000 || s->boss2_quake_time > 3000))) return false;
    for (i = 0; i < Q2_NATIVE_LIFT_COUNT; i++) {
        const q2_native_lift *lift = &s->lift[i];
        if (lift->valid > 1) return false;
        if (!lift->valid) continue;
        if (lift->state > 6 || lift->offset < -32768 || lift->offset > 32767 ||
            lift->dy < -32768 || lift->dy > 32767) return false;
        for (k = 0; k < 3; k++)
            if (lift->env_min[k] < -16777216 || lift->env_max[k] > 16777216 ||
                lift->env_min[k] > lift->min[k] || lift->min[k] > lift->max[k] ||
                lift->max[k] > lift->env_max[k]) return false;
    }
    return true;
}
#endif
