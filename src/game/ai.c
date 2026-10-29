#include "random.h"
#include "ai.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "aimove.h"
#include "crebind.h"   /* q2_cre_skill — M_CheckAttack scales on it */
#include "trig.h"

/* ------------------------------------------------------------------------- */
/* The stand-in world                                                         */
/* ------------------------------------------------------------------------- */
/*
 * Everything is visible, nothing blocks, and there is always ground. That is a
 * deliberate choice of failure: monsters that see through walls are obvious on
 * sight, where monsters that are silently blind look like the AI itself is
 * broken. Installing a real world through q2_ai_set_world replaces all three.
 */
static void open_trace(void *user, const s32 start[3], const s16 mins[3],
                       const s16 maxs[3], const s32 end[3],
                       const q2_monster *ignore, u32 mask, q2_ai_trace *out)
{
    (void)user; (void)mins; (void)maxs; (void)ignore; (void)mask;

    out->allsolid   = false;
    out->startsolid = false;
    out->ent        = NULL;

    /*
     * A floor, and nothing else.
     *
     * SV_movestep probes downward from a step height above where it wants to
     * be to a step height below, and treats a probe that runs the whole way as
     * having walked off a ledge. So a world that answers "clear" to everything
     * is not an empty world — it is a world with no ground, in which no walker
     * can take a single step. The stand-in therefore stops a downward probe
     * halfway, which is a flat floor at exactly the height the creature is
     * already standing at.
     *
     * Y points down, so a downward probe is one whose end is greater.
     */
    if (end[1] > start[1]) {
        out->fraction  = Q2_TRACE_ONE / 2;
        out->endpos[0] = end[0];
        out->endpos[1] = (start[1] + end[1]) / 2;
        out->endpos[2] = end[2];
        return;
    }

    out->fraction   = Q2_TRACE_ONE;
    out->endpos[0]  = end[0];
    out->endpos[1]  = end[1];
    out->endpos[2]  = end[2];
}

static bool open_los(void *user, const s32 a[3], const s32 b[3])
{
    (void)user; (void)a; (void)b;
    return true;
}

static bool open_bottom(void *user, const q2_monster *m)
{
    (void)user; (void)m;
    return true;
}

static const q2_ai_world g_open_world = {
    NULL, open_trace, open_los, open_bottom
};

static q2_ai_world g_world = { NULL, open_trace, open_los, open_bottom };

void q2_ai_set_world(const q2_ai_world *w)
{
    if (!w) {
        g_world = g_open_world;
        return;
    }

    g_world       = *w;
    if (!g_world.trace)         g_world.trace         = open_trace;
    if (!g_world.line_of_sight) g_world.line_of_sight = open_los;
    if (!g_world.check_bottom)  g_world.check_bottom  = open_bottom;
}

const q2_ai_world *q2_ai_get_world(void)
{
    return &g_world;
}

/* ------------------------------------------------------------------------- */
/* The globals ai_checkattack publishes — gp+17868..17880                     */
/* ------------------------------------------------------------------------- */
bool          q2_enemy_vis;
s16           q2_enemy_yaw;
bool          q2_enemy_infront;
q2_range_band q2_enemy_range;

/* ------------------------------------------------------------------------- */
/* Fixed-point helpers                                                        */
/* ------------------------------------------------------------------------- */
/* 0x8005C7B8 — one instruction. */
s32 q2_anglemod(s32 a)
{
    return a & 0xFFF;
}

/*
 * 0x8005F8E8 — yaw from an offset.
 *
 * The ground plane is XZ, so this is atan2(x, z) and not id's atan2(y, x). The
 * zero case tests z then x, in that order, exactly as the original does.
 */
s16 q2_vectoyaw(const s32 v[3])
{
    /* 0x8005F91C returns ratan2 directly, including its negative half-circle.
     * Normalizing here changes exact-yaw branches in the creature modules. */
    return v ? (s16)q2_ratan2(v[0], v[2]) : 0;
}

/* Integer square root, the shape 0x8008A7E8 has. */
static s32 isqrt64(s64 n)
{
    s64 x, y;

    if (n <= 0)
        return 0;

    x = n;
    y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return (s32)x;
}

/*
 * 0x8005C4E8 / 0x8005C59C — length and length squared.
 *
 * Both refuse any component whose magnitude reaches 26755 (0xB504, the square
 * root of 2^31) and return that same 0xB504 rather than overflowing. That
 * saturation is observable: two entities far enough apart compare equal in
 * range regardless of how far apart they actually are.
 */
#define Q2_LEN_LIMIT 26755

s32 q2_vector_length(const s32 v[3])
{
    if (!v)
        return 0;

    if (llabs((long long)v[0]) >= Q2_LEN_LIMIT ||
        llabs((long long)v[1]) >= Q2_LEN_LIMIT ||
        llabs((long long)v[2]) >= Q2_LEN_LIMIT)
        return 0xB504;

    return isqrt64((s64)v[0] * v[0] + (s64)v[1] * v[1] + (s64)v[2] * v[2]);
}

s64 q2_vector_length_sq(const s32 v[3])
{
    if (!v)
        return 0;

    if (llabs((long long)v[0]) >= Q2_LEN_LIMIT ||
        llabs((long long)v[1]) >= Q2_LEN_LIMIT ||
        llabs((long long)v[2]) >= Q2_LEN_LIMIT)
        return 0xB504;

    return (s64)v[0] * v[0] + (s64)v[1] * v[1] + (s64)v[2] * v[2];
}

/* 0x8005C634 — normalise in place to 1.12, returning the old length. */
s32 q2_vector_normalize(s32 v[3])
{
    s32 len;

    if (!v)
        return 0;

    len = q2_vector_length(v);
    if (len <= 0) {
        v[0] = v[1] = v[2] = 0;
        return 0;
    }

    v[0] = (s32)(((s64)v[0] << 12) / len);
    v[1] = (s32)(((s64)v[1] << 12) / len);
    v[2] = (s32)(((s64)v[2] << 12) / len);

    return len;
}

/*
 * 0x8005BB58 — forward, right and up from all THREE angles.
 *
 * This used to read the yaw alone, on the grounds that a walking creature's
 * pitch and roll stay zero. They do, and for every caller that passes a
 * creature's own angles the full transcription below gives the same three
 * vectors to the unit — cos 0 is 4096, so each product divides back out
 * exactly. It stopped being enough when the dying Infantry became a caller:
 * InfantryMachineGun hands this `self->angles - aimangles[n]`, whose PITCH
 * sweeps from 0 to 90 degrees, and a yaw-only answer fired that whole burst
 * along the floor.
 *
 * The table at 0x800A5430 is {sin, cos} per step (q2_sin12/q2_cos12 are it,
 * 4096 of 4096), read for angles[2] (yaw), angles[0] (pitch) and angles[1]
 * (roll). Every product is `mult; bgez; addiu 4095; sra 12` — a divide by 4096
 * that rounds toward zero, which is C's `/` and not a shift:
 *
 *     forward = (cp*sy, sp, cp*cy)
 *     right   = (-sr*sp*sy + cr*cy,  sr*cp,  -sr*sp*cy - cr*sy)
 *     up      = ( cr*sp*sy + sr*cy, -cr*cp,   cr*sp*cy - sr*sy)
 *
 * Y points down, so a positive pitch looks DOWN, as id's does, and with all
 * three zero forward is (sin, 0, cos) to match M_walkmove's move vector,
 * right is forward turned a quarter circle so the corner peek's +192 lands on
 * the creature's right hand, and up is (0, -4096, 0).
 */
static s32 mul4096(s32 a, s32 b)
{
    return (s32)(((s64)a * b) / 4096);
}

void q2_angle_vectors(const s16 angles[3], s32 forward[3], s32 right[3],
                      s32 up[3])
{
    s32 sy = 0, cy = 4096, sp = 0, cp = 4096, sr = 0, cr = 4096;

    if (angles) {
        sy = q2_sin12(angles[2] & 0xFFF);  cy = q2_cos12(angles[2] & 0xFFF);
        sp = q2_sin12(angles[0] & 0xFFF);  cp = q2_cos12(angles[0] & 0xFFF);
        sr = q2_sin12(angles[1] & 0xFFF);  cr = q2_cos12(angles[1] & 0xFFF);
    }

    if (forward) {                                  /* 0x8005BBAC */
        forward[0] = mul4096(cp, sy);
        forward[1] = sp;
        forward[2] = mul4096(cp, cy);
    }
    if (right) {                                    /* 0x8005BBE8 */
        s32 t = mul4096(sr, -sp);

        right[0] = mul4096(t, sy) + mul4096(cr, cy);
        right[2] = mul4096(t, cy) + mul4096(cr, -sy);
        right[1] = -mul4096(sr, -cp);
    }
    if (up) {                                       /* 0x8005BC90 */
        s32 t = mul4096(cr, sp);

        up[0] = mul4096(t, sy) + mul4096(-sr, -cy);
        up[2] = mul4096(t, cy) + mul4096(-sr, sy);
        up[1] = -mul4096(cr, cp);
    }
}

/*
 * 0x8005C460 — VectorMA. `out = a + scale * b`, the product a divide by 4096
 * rounded toward zero, component by component (`bgez; addiu 4095; sra 12`).
 * `b` is a 1.12 direction and `scale` a distance in world units, which is how
 * every module call site uses it.
 */
void q2_vector_ma(const s32 a[3], s32 scale, const s32 b[3], s32 out[3])
{
    int k;

    if (!a || !b || !out)
        return;
    for (k = 0; k < 3; k++)
        out[k] = a[k] + mul4096(scale, b[k]);
}

/* ratan2 (0x8008A358) — q2_ratan2 in trig.h, and why it is not the table. */
static s32 ratan2_4096(s32 y, s32 x)
{
    return q2_ratan2(y, x);
}

/*
 * 0x8005F934 — vectoangles, id's own shape with the axes renamed: the ground
 * plane is XZ and Y points down, so id's `value1[0], [1], [2]` are v[0], v[2]
 * and v[1] here.
 *
 *   - v[0] and v[2] both zero: yaw 0, pitch 1024 when v[1] > 0 (straight
 *     down) and 3072 otherwise (0x8005F96C..0x8005F980).
 *   - v[0] zero, v[2] not: yaw 1024 when v[2] > 0, else 3072 (0x8005F998).
 *     THAT IS id's `value1[1] > 0 ? 90 : 270` carried over without the axis
 *     swap, and it is wrong for this console's convention — a vector straight
 *     down +Z comes back at yaw 1024, a quarter turn off. Reproduced: it only
 *     fires on an exactly axis-aligned input, and the console does it.
 *   - otherwise yaw = ratan2(v[0], v[2]), pitch = ratan2(v[1], sqrt(v[0]^2 +
 *     v[2]^2)), each lifted by 4096 when negative.
 *
 * Roll (out[1]) is always written zero (0x8005F9FC).
 */
void q2_vectoangles(const s32 v[3], s16 out[3])
{
    s32 yaw, pitch;

    if (!v || !out)
        return;

    if (v[0] == 0 && v[2] == 0) {
        yaw   = 0;
        pitch = v[1] > 0 ? 1024 : 3072;
    } else {
        if (v[0] != 0)
            yaw = ratan2_4096(v[0], v[2]);
        else
            yaw = v[2] > 0 ? 1024 : 3072;
        if (yaw < 0)
            yaw += 4096;

        pitch = ratan2_4096(v[1], isqrt64((s64)v[0] * v[0] +
                                          (s64)v[2] * v[2]));
        if (pitch < 0)
            pitch += 4096;
    }

    out[0] = (s16)pitch;
    out[1] = 0;
    out[2] = (s16)yaw;
}

/*
 * 0x8005F5FC — a local (forward, up, right) triple placed in the world.
 *
 * The offset is three s16 in the original, written straight onto the stack by
 * the caller, and its components are in that order: ai_run fills
 * (d2, 0, ±192) to peek a distance ahead and a body's width to one side.
 */
void q2_project_source(const s32 point[3], const s16 distance[3],
                       const s32 forward[3], const s32 right[3],
                       s32 out[3])
{
    int i;

    if (!point || !distance || !out)
        return;

    for (i = 0; i < 3; i++) {
        /* 0x8005F5FC truncates each product separately, toward zero. */
        out[i] = point[i];
        if (forward) out[i] += (s32)((s64)forward[i] * distance[0]) / 4096;
        if (right) out[i] += (s32)((s64)right[i] * distance[2]) / 4096;
    }
    out[1] += distance[1]; /* 0x8005F6A0..0x8005F6AC */
}

/* ------------------------------------------------------------------------- */
/* visible — 0x8005B950                                                       */
/* ------------------------------------------------------------------------- */
bool q2_visible(const q2_monster *self, const q2_monster *other)
{
    s32 spot1[3], spot2[3];

    if (!self || !other)
        return false;

    spot1[0] = self->pos[0];
    spot1[1] = self->pos[1] + self->view_height;
    spot1[2] = self->pos[2];

    spot2[0] = other->pos[0];
    spot2[1] = other->pos[1] + other->view_height;
    spot2[2] = other->pos[2];

    return g_world.line_of_sight(g_world.user, spot1, spot2);
}

/* ------------------------------------------------------------------------- */
/* The player's breadcrumb trail — 0x800D517C, head at gp+17892              */
/* ------------------------------------------------------------------------- */
static q2_trail_spot g_trail[Q2_TRAIL_LENGTH];
static int  g_trail_head;
static bool g_trail_active;

void q2_trail_capture(q2_trail_state *out)
{
    if (!out) return;
    memcpy(out->spot, g_trail, sizeof(g_trail));
    out->head = (u32)g_trail_head;
    out->active = g_trail_active;
}

bool q2_trail_restore(const q2_trail_state *state)
{
    if (!state || state->head >= Q2_TRAIL_LENGTH) return false;
    memcpy(g_trail, state->spot, sizeof(g_trail));
    g_trail_head = (int)state->head;
    g_trail_active = state->active;
    return true;
}

#define TRAIL_NEXT(i) (((i) + 1) & (Q2_TRAIL_LENGTH - 1))
#define TRAIL_PREV(i) (((i) - 1) & (Q2_TRAIL_LENGTH - 1))

void q2_trail_init(void)
{
    memset(g_trail, 0, sizeof(g_trail));
    g_trail_head   = 0;
    g_trail_active = false;
}

/*
 * 0x80060A70 — lay one crumb.
 *
 * THE HEAD NAMES THE SLOT ABOUT TO BE OVERWRITTEN, NOT THE NEWEST ONE.
 * 0x80060AEC loads the head and 0x80060B04/0x80060B14/0x80060B24/0x80060B38
 * write origin and timestamp straight into that slot; only afterwards, at
 * 0x80060B88..0x80060BA4, does it reload the head, `addiu a0, a0, 1`,
 * `andi a0, a0, 7` and store it back. PlayerTrail_LastSpot (0x80060FFC)
 * confirms the convention from the other side: the most recent crumb is
 * `trail[(head - 1) & 7]`.
 *
 * This had been written the other way round — advance, then write — which put
 * the head on the NEWEST crumb. PickFirst's scan starts at the head, so it
 * started at the end of the age order instead of the beginning and always
 * handed back the crumb the player had just dropped. A creature that lost you
 * walked at where you are now rather than along where you had been.
 *
 * THE STORED YAW IS THE DIRECTION OF TRAVEL, NOT THE PLAYER'S VIEW.
 * 0x80060B34 forms `&trail[(head - 1) & 7]`, 0x80060B3C..0x80060B7C subtract
 * that previous crumb's origin from the new spot and 0x80060B80 hands the
 * difference to vectoyaw (0x8005F8E8); 0x80060BA8 puts the result in the slot
 * just written. So a crumb remembers which way the player was going when it
 * was laid, which is what ai_run snaps the creature's facing to.
 */
void q2_trail_add(const s32 origin[3])
{
    q2_trail_spot *s;
    const q2_trail_spot *prev;
    s32 v[3];

    if (!origin)
        return;

    s    = &g_trail[g_trail_head];
    prev = &g_trail[TRAIL_PREV(g_trail_head)];

    v[0] = origin[0] - prev->origin[0];
    v[1] = origin[1] - prev->origin[1];
    v[2] = origin[2] - prev->origin[2];

    s->origin[0] = origin[0];
    s->origin[1] = origin[1];
    s->origin[2] = origin[2];
    s->timestamp = q2_level_state.time;
    s->yaw       = q2_vectoyaw(v);

    g_trail_head   = TRAIL_NEXT(g_trail_head);
    g_trail_active = true;
}

/*
 * 0x80060BBC — the oldest spot the creature has not already been sent to.
 *
 * Walks forward from the head until it finds a spot newer than the creature's
 * own trail_time, then prefers that one if it can see it, the one before it if
 * it can see that instead, and falls back to the first either way. The
 * fallback is not a mistake: a creature that can see neither still needs
 * somewhere to walk.
 *
 * There is no "is this slot written yet" test, and there was not one on the
 * disc either: PlayerTrail_Init (0x80060F20) memsets all eight 20-byte slots
 * to zero and 0x80060F84 raises the active flag, so an unvisited slot reads as
 * a crumb at the world origin with timestamp 0. The scan walks past those on
 * timestamp alone, exactly as it walks past stale ones; guarding on a `valid`
 * byte instead made pick_next answer NULL where the original answers a stale
 * spot, which is a different behaviour, not a safer one.
 */
const q2_trail_spot *q2_trail_pick_first(q2_monster *self)
{
    int marker, n, prev;

    if (!g_trail_active || !self)
        return NULL;

    marker = g_trail_head;
    for (n = Q2_TRAIL_LENGTH; n; n--) {
        if (g_trail[marker].timestamp <= self->trail_time)
            marker = TRAIL_NEXT(marker);
        else
            break;
    }

    /* The original tests `visible(self, trail[marker])` against a real entity;
     * with spots rather than entities that reduces to a point sight check from
     * the creature's eye. */
    {
        s32 eye[3];
        eye[0] = self->pos[0];
        eye[1] = self->pos[1] + self->view_height;
        eye[2] = self->pos[2];

        if (g_world.line_of_sight(g_world.user, eye, g_trail[marker].origin))
            return &g_trail[marker];

        prev = TRAIL_PREV(marker);
        if (g_world.line_of_sight(g_world.user, eye, g_trail[prev].origin))
            return &g_trail[prev];
    }

    return &g_trail[marker];
}

/*
 * 0x80060FFC — PlayerTrail_LastSpot, the most recent crumb, which under the
 * write-then-advance convention is the slot BEFORE the head: `addiu v0,v0,-1;
 * andi v0,v0,7` at 0x80061008/0x8006100C.
 */
const q2_trail_spot *q2_trail_last_spot(void)
{
    return &g_trail[TRAIL_PREV(g_trail_head)];
}

/*
 * THE CONSOLE DOES NOT LAY A CRUMB ON A TIMER.
 *
 * PlayerTrail_Add's only caller (0x8007E32C, in the per-client frame loop)
 * first calls PlayerTrail_LastSpot at 0x8007E2C4 and `visible` (0x8005B950) at
 * 0x8007E2D0, and at 0x8007E2D8 `bne v0, zero, 0x8007E334` SKIPS the add when
 * the player can still see the previous crumb. Crumbs are corner markers, so
 * eight of them span a whole route; dropping one on a fixed cadence instead
 * makes them dense samples of the last few seconds and a creature following
 * them never rounds a corner.
 *
 * The test lives on this side of the API rather than at the client call site
 * only because the sight hook does. `visible` adds the view height for an
 * argument inside the entity array and not for one outside it
 * (0x8005BA4C..0x8005BA9C), and a trail slot is outside it — so the player end
 * is the EYE and the crumb end is the raw stored point, which is what this
 * passes.
 */
bool q2_trail_needs_spot(const s32 eye[3])
{
    if (!eye)
        return false;

    /* No sight hook bound: lay the crumb rather than silently keeping an
     * empty trail. The console always has one. */
    if (!g_world.line_of_sight)
        return true;

    return !g_world.line_of_sight(g_world.user, eye,
                                  g_trail[TRAIL_PREV(g_trail_head)].origin);
}

/*
 * 0x80060F90 — the next spot along, or NULL when the trail runs out.
 *
 * The original bails immediately when the trail is inactive, which is the only
 * part of it that survives in the image as a distinct function; the walk
 * itself shares PickFirst's loop.
 */
const q2_trail_spot *q2_trail_pick_next(q2_monster *self)
{
    int marker, n;

    if (!g_trail_active || !self)
        return NULL;

    marker = g_trail_head;
    for (n = Q2_TRAIL_LENGTH; n; n--) {
        if (g_trail[marker].timestamp <= self->trail_time)
            marker = TRAIL_NEXT(marker);
        else
            break;
    }

    return &g_trail[marker];
}

/* ------------------------------------------------------------------------- */
/* random() — the game's 0..1 in fixed point                                  */
/* ------------------------------------------------------------------------- */
/*
 * The original computes `(rand() & 0x7fff) * n / 32767` with a magic-number
 * division, which is `random() * n` written for a machine with no divider.
 * Keeping the same form keeps the same distribution and the same endpoints:
 * a full 0x7fff yields exactly n.
 */
static s32 rand15(void)
{
    return (s32)(q2_random_next() & 0x7FFF);
}

static s32 random_scaled(s32 n)
{
    return (s32)(((s64)rand15() * n) / 32767);
}

/* ------------------------------------------------------------------------- */
/* AttackFinished — 0x800622D0                                                */
/* ------------------------------------------------------------------------- */
void q2_attack_finished(q2_monster *self, s32 t)
{
    if (self)
        self->attack_finished = q2_level_state.time + t;
}

/* ------------------------------------------------------------------------- */
/* HuntTarget / FoundTarget — inline at 0x8005DE8C, and 0x8005D104            */
/* ------------------------------------------------------------------------- */
void q2_hunt_target(q2_monster *self)
{
    s32 v[3];

    if (!self || !self->enemy)
        return;

    self->goalentity = self->enemy;

    if (self->aiflags & Q2_AI_STAND_GROUND) {
        if (self->stand)
            self->stand(self);
    } else {
        if (self->run)
            self->run(self);
    }

    v[0] = self->enemy->pos[0] - self->pos[0];
    v[1] = self->enemy->pos[1] - self->pos[1];
    v[2] = self->enemy->pos[2] - self->pos[2];
    self->ideal_yaw = q2_vectoyaw(v);

    /* Wait a beat before the first attack, so a monster that has just turned
     * around does not fire in the same tick. */
    if (!(self->aiflags & Q2_AI_STAND_GROUND))
        q2_attack_finished(self, Q2_AI_SECONDS(1));
}

/* The enemy's position as the AI records it: a client reports its own, which
 * is the branch on entity+0x3C at 0x8005D150. */
static void enemy_position(const q2_monster *e, s32 out[3])
{
    out[0] = e->pos[0];
    out[1] = e->pos[1];
    out[2] = e->pos[2];
}

void q2_found_target(q2_monster *self)
{
    if (!self || !self->enemy)
        return;

    /* Let other monsters see this monster for a while. */
    if (self->enemy->client) {
        q2_level_state.sight_entity          = self;
        q2_level_state.sight_entity_framenum = q2_level_state.framenum;
    }

    self->show_hostile = q2_level_state.time + Q2_AI_SECONDS(1);

    enemy_position(self->enemy, self->last_sighting);
    self->trail_time = q2_level_state.time;

    if (self->blindfire) {
        self->blind_target[0] = self->enemy->pos[0];
        self->blind_target[1] = self->enemy->pos[1];
        self->blind_target[2] = self->enemy->pos[2];
        self->spawnflags &= ~0xFFu;
    }

    if (self->combattarget <= 0) {
        q2_hunt_target(self);
        return;
    }

    self->movetarget = q2_pick_target(self->combattarget);
    self->goalentity = self->movetarget;

    if (!self->movetarget) {
        self->goalentity = self->enemy;
        self->movetarget = self->enemy;
        q2_hunt_target(self);
        return;
    }

    /* A combat point is a one-shot deal: claim it, clear the name so no other
     * creature takes it, and drop the pause so standing does not resume. */
    self->combattarget = 0;
    self->aiflags |= Q2_AI_COMBAT_POINT;
    self->movetarget->targetname = 0;
    self->pausetime = 0;

    if (self->run)
        self->run(self);
}

/* ------------------------------------------------------------------------- */
/* M_ReactToDamage — 0x80062654                                               */
/* ------------------------------------------------------------------------- */
/*
 * WHO A HURT CREATURE TURNS ON, and until now nothing on this side answered it.
 *
 * `oldenemy` (entity+0xC0) was read in `ai_run` — a creature that loses its
 * target falls back to it — and was WRITTEN nowhere in the whole tree. This is
 * the function that writes it, and it is the only one that does: T_Damage calls
 * it at 0x80062AC0, before the pain handler and only for something with
 * SVF_MONSTER set. Without it a creature shot from behind never turned round;
 * it acquired you through `FindTarget`'s sight cone or not at all, which is why
 * shooting one in the back was free.
 *
 * ---------------------------------------------------------------------------
 * The four class bytes it refuses to take offence at
 * ---------------------------------------------------------------------------
 * id's version excludes four monsters by `strcmp` on the classname — tank,
 * supertank, makron and jorg, "they spread out too much". The console has no
 * classnames at this point; it compares the class byte at entity+0x23 against
 * 91, 90, 83 and 82 (0x80062750..0x8006276C). Those four are not a guess: the
 * class descriptor table at 0x800A3518 carries the class byte in each 48-byte
 * record at +0x20, and reading that column out gives
 *
 *     91  Tankcomm     health  750     id's monster_tank
 *     90  Boss1        health 1500     id's monster_supertank
 *     83  Rider        health 3000     id's monster_makron
 *     82  Jorg         health 3000     id's monster_jorg
 *
 * — id's own four, in id's own role, and their health columns are id's numbers
 * as well. Nothing in the decode was arranged to produce that.
 *
 * ---------------------------------------------------------------------------
 * One departure from id, and it is the disc's
 * ---------------------------------------------------------------------------
 * id guards the help-our-buddy arm with `attacker->enemy && attacker->enemy !=
 * targ`. The console does not: 0x800627C4 loads `attacker->enemy` and stores it
 * into `targ->enemy` with no test at all, so a creature caught in another's
 * crossfire can end up with a NULL enemy and still be sent to FoundTarget.
 * Reproduced as written — `q2_found_target` returns on a null enemy, which is
 * what the original's own null deref would not have done, and is the one place
 * this is deliberately safer than the console.
 */
/* The four class bytes are Q2_CLASS_TANKCOMM / BOSS1 / RIDER / JORG, named in
 * monster.h with the descriptor-table column they come from. */

/* FL_FLY | FL_SWIM — the "same base type" test at 0x80062730. */
#define Q2_FL_BASE_TYPE (Q2_FL_FLY | Q2_FL_SWIM)

static bool reacts_by_class(const q2_monster *attacker)
{
    switch (attacker->class_id) {
    case Q2_CLASS_TANKCOMM:
    case Q2_CLASS_BOSS1:
    case Q2_CLASS_RIDER:
    case Q2_CLASS_JORG:
        return false;
    default:
        return true;
    }
}

void q2_m_react_to_damage(q2_monster *targ, q2_monster *attacker)
{
    if (!targ || !attacker)
        return;

    /* Only a player or another creature is worth reacting to — a rocket is
     * not. 0x80062668. */
    if (!attacker->client && !(attacker->svflags & Q2_SVF_MONSTER))
        return;

    if (attacker == targ || attacker == targ->enemy)
        return;

    /* A good guy does not get angry at a player or at another good guy.
     * 0x800626A4. */
    if (targ->aiflags & Q2_AI_GOOD_GUY) {
        if (attacker->client || (attacker->aiflags & Q2_AI_GOOD_GUY))
            return;
    }

    if (attacker->client) {
        /*
         * Already hunting a player and can still SEE them: remember the new
         * attacker for later and carry on with the one in front. Only when the
         * old target has gone out of sight does the new one take over.
         * 0x800626DC.
         */
        if (targ->enemy && targ->enemy->client) {
            if (q2_visible(targ, targ->enemy)) {
                targ->oldenemy = attacker;
                return;
            }
            targ->oldenemy = targ->enemy;
        }
        targ->enemy = attacker;
    } else if ((targ->flags & Q2_FL_BASE_TYPE)
                   == (attacker->flags & Q2_FL_BASE_TYPE)
               && targ->class_id != attacker->class_id
               && reacts_by_class(attacker)) {
        /* Hit by a different kind of creature that moves the same way it does:
         * fight back. 0x80062728. */
        if (targ->enemy && targ->enemy->client)
            targ->oldenemy = targ->enemy;
        targ->enemy = attacker;
    } else {
        /* Otherwise take up whatever the attacker is angry at. 0x800627A0. */
        if (targ->enemy && targ->enemy->client)
            targ->oldenemy = targ->enemy;
        targ->enemy = attacker->enemy;
    }

    /* A creature part-way through a duck does not break out of it to charge.
     * 0x800627D4. */
    if (!(targ->aiflags & Q2_AI_DUCKED))
        q2_found_target(targ);
}

/* ------------------------------------------------------------------------- */
/* AI_SetSightClient — 0x8005EC60, G_AI.C line 61                             */
/* ------------------------------------------------------------------------- */
/*
 * WHO FINDTARGET IS ALLOWED TO NOTICE, recomputed once a tick.
 *
 * This had no counterpart in the port at all, and its absence is why a dead
 * player could still be acquired. `q2_monster_set_wake` wrote
 * `level.sight_client` once, at zone wake, and nothing ever revisited it —
 * so FindTarget's default arm kept handing every creature a pointer to a
 * corpse. The corpse could not be SHOT (M_CheckAttack reads the stand-in's
 * real health), but it could still be acquired, and every acquisition ran
 * FoundTarget: the module's sight sound played, `sight_entity` was published,
 * that woke the next creature through FindTarget's first arm, and
 * ai_checkattack's "he's dead Jim" arm dropped the enemy again on the next
 * tick so the whole thing repeated for as long as the level ran.
 *
 * On the console it never starts, because the acquisition never happens:
 * sight_client is NULL from the first frame after the player's health reaches
 * zero. QuakeThinkGeneral bumps level.framenum and level.time
 * (0x8007E264..0x8007E278) and its very next instruction is `jal 0x8005EC60`
 * (0x8007E27C) — the only call to this function in the whole executable.
 *
 * The rotation itself: from the index of the client currently held it steps
 * FORWARD one, wraps past game.maxclients (0x800B2C2C) back to 1, and takes
 * the first client that passes all three of
 *
 *     ent+0x1C & 0x20000000      in use            0x8005ECFC
 *     ent+0x24 -> lh +0x108 > 0  the actor's health  0x8005ED10 / 0x8005ED18
 *     !(ent+0x20 & 0x20)         FL_NOTARGET clear   0x8005ED28
 *
 * When it comes back round to the index it started from with nothing accepted
 * it stores NULL (0x8005ED3C `sw zero, 8(t1)`). Note the order: the sweep
 * starts at start+1, not at start, so with two live clients the pointer
 * ALTERNATES every tick rather than sticking on whoever was found first. That
 * is deliberate — it is how the original spreads sightings around a deathmatch
 * — and it is transcribed here even though single player only ever has one
 * client, because the four-player mode has four.
 */

/*
 * g_edicts[1..game.maxclients], 0x800B2FD0 + 324 and 0x800B2C2C. The console
 * walks a contiguous edict array and recovers an index by dividing a pointer
 * difference; this port has no such array, so the roster is registered as
 * pointers and the index is a search. `q2_monster_set_wake` is the only writer,
 * which mirrors the console rebuilding g_edicts for every level.
 */
static q2_monster *g_ai_clients[Q2_AI_MAX_CLIENTS];
static s32         g_ai_maxclients;

void q2_ai_set_clients(q2_monster *const *clients, s32 maxclients)
{
    s32 i;

    memset(g_ai_clients, 0, sizeof(g_ai_clients));
    g_ai_maxclients = 0;

    if (!clients || maxclients <= 0)
        return;

    if (maxclients > Q2_AI_MAX_CLIENTS)
        maxclients = Q2_AI_MAX_CLIENTS;

    for (i = 0; i < maxclients; i++)
        g_ai_clients[i] = clients[i];

    g_ai_maxclients = maxclients;
}

/* g_edicts[i] for the console's own 1-based client index. Out of range answers
 * NULL, which the accept gate below then rejects. */
static q2_monster *client_edict(s32 i)
{
    if (i < 1 || i > g_ai_maxclients)
        return NULL;
    return g_ai_clients[i - 1];
}

void q2_ai_set_sight_client(void)
{
    s32 start = 1;
    s32 check;
    s32 i;

    /*
     * Where the sweep starts. With no client held the console jumps straight
     * to index 1 (0x8005EC74/0x8005EC78); otherwise it recovers the held
     * client's index from its offset into g_edicts, as an EXACT division by
     * sizeof(edict_t) = 324 — `lo(offset * 0x781948B1) >> 2` at
     * 0x8005EC8C..0x8005EC9C, 0x781948B1 being the inverse of 81 modulo 2^32
     * and 324 being 4 * 81. Pointers here are not an array, so this is a
     * search, and a sight_client that is not on the roster at all (a level
     * teardown left it behind) falls back to 1 rather than to a garbage index.
     */
    if (q2_level_state.sight_client) {
        for (i = 1; i <= g_ai_maxclients; i++) {
            if (g_ai_clients[i - 1] == q2_level_state.sight_client) {
                start = i;
                break;
            }
        }
    }

    check = start;
    for (;;) {
        q2_monster *ent;

        /*
         * Step forward, then wrap. The increment happens BEFORE the bound test
         * on the console too — a1 is already start+1 when the loop head at
         * 0x8005ECDC runs `slt v0, maxclients, a1`, and the wrap arm at
         * 0x8005ECE8/0x8005ECEC resets both the pointer and the index to 1.
         */
        check++;
        if (check > g_ai_maxclients)
            check = 1;

        ent = client_edict(check);

        /* The three gates, in the console's order. `health` is the ACTOR's
         * halfword at +0x108, reached through entity+0x24; it is the one the
         * port keeps on the monster itself, and on the sight stand-in it is
         * the live player's (q2_creature_world_tick). */
        if (ent && q2_ent_inuse(ent)
            && ent->health > 0
            && !(ent->flags & Q2_FL_NOTARGET)) {
            q2_level_state.sight_client = ent;   /* 0x8005EC80 */
            return;
        }

        /* Back where we began with nothing taken. 0x8005ED34 `bne a1, a2`. */
        if (check == start)
            break;
    }

    /*
     * Nobody. 0x8005ED40 `sw zero, 8(t1)`, t1 being &level.framenum, so this
     * is level.sight_client. An empty roster reaches here after probing one
     * slot, which is also what the console does when game.maxclients is 0.
     */
    q2_level_state.sight_client = NULL;
}

/* ------------------------------------------------------------------------- */
/* FindTarget — 0x8005D334                                                    */
/* ------------------------------------------------------------------------- */
/* The class id of a scripted actor, which a good guy will not shoot. */
#define Q2_CLASS_ACTOR        204

bool q2_find_target(q2_monster *self)
{
    q2_monster *client;
    bool heardit = false;
    q2_range_band r;
    s32 temp[3];

    if (!self)
        return false;

    if (self->aiflags & Q2_AI_GOOD_GUY) {
        /* A friendly follows its goal and never picks a fight on its own. The
         * actor test is present in the original and both arms return false;
         * it is kept because its shape is what identifies the class id. */
        if (self->goalentity && q2_ent_inuse(self->goalentity)
            && self->goalentity->class_id != 0) {
            if (self->goalentity->class_id == Q2_CLASS_ACTOR)
                return false;
        }
        return false;
    }

    if (self->aiflags & Q2_AI_COMBAT_POINT)
        return false;

    /*
     * Three ways to be alerted, tried in order of how recently they happened.
     * The ambush spawnflag suppresses the two that are second-hand: an ambush
     * monster wakes for a loud noise but not for another monster's sighting
     * and not for a quiet one.
     */
    if (q2_level_state.sight_entity_framenum >= q2_level_state.framenum - 1
        && !(self->spawnflags & Q2_SPAWNFLAG_AMBUSH)) {
        client = q2_level_state.sight_entity;
        if (!client)
            return false;
        if (client->enemy == self->enemy)
            return false;
    } else if (q2_level_state.sound_entity_framenum
               >= q2_level_state.framenum - 1) {
        client  = q2_level_state.sound_entity;
        heardit = true;
    } else if (!self->enemy
               && q2_level_state.sound2_entity_framenum
                  >= q2_level_state.framenum - 1
               && !(self->spawnflags & Q2_SPAWNFLAG_AMBUSH)) {
        client  = q2_level_state.sound2_entity;
        heardit = true;
    } else {
        client = q2_level_state.sight_client;
        if (!client)
            return false;
    }

    if (!q2_ent_inuse(client))
        return false;

    if (client == self->enemy)
        return true;            /* already got one */

    if (client->client) {
        if (client->flags & Q2_FL_NOTARGET)
            return false;
    } else if (client->svflags & Q2_SVF_MONSTER) {
        if (!client->enemy)
            return false;
        if (client->enemy->flags & Q2_FL_NOTARGET)
            return false;
    } else if (heardit) {
        /* 0x8005D4FC: a noise is refused if its maker is NOTARGET. */
        if (client->owner && (client->owner->flags & Q2_FL_NOTARGET))
            return false;
    } else {
        return false;
    }

    if (!heardit) {
        r = q2_range(self, client);
        if (r == Q2_RANGE_FAR)
            return false;

        if (!q2_visible(self, client))
            return false;

        if (r == Q2_RANGE_NEAR) {
            /* Close by, a monster that has recently been hostile is noticed
             * even from behind — that is what show_hostile buys. */
            if (client->show_hostile < q2_level_state.time
                && !q2_infront(self, client))
                return false;
        } else if (r == Q2_RANGE_MID) {
            if (!q2_infront(self, client))
                return false;
        }

        self->enemy = client;

        if (self->enemy->class_id != Q2_CLASS_PLAYER_NOISE) {
            self->aiflags &= ~(u32)Q2_AI_SOUND_TARGET;
            if (!self->enemy->client) {
                self->enemy = self->enemy->enemy;
                if (!self->enemy || !self->enemy->client) {
                    self->enemy = NULL;
                    return false;
                }
            }
        }
    } else {
        /* Heard rather than saw. An ambush monster still needs line of sight
         * even to a noise, which is the whole point of the flag. */
        if ((self->spawnflags & Q2_SPAWNFLAG_AMBUSH)
            && !q2_visible(self, client))
            return false;

        temp[0] = client->pos[0] - self->pos[0];
        temp[1] = client->pos[1] - self->pos[1];
        temp[2] = client->pos[2] - self->pos[2];

        if (q2_vector_length_sq(temp)
            > (s64)Q2_RANGE_MID_DIST * Q2_RANGE_MID_DIST)
            return false;

        self->ideal_yaw = q2_vectoyaw(temp);
        q2_M_ChangeYaw(self);

        self->enemy = client;
        self->aiflags |= Q2_AI_SOUND_TARGET;
    }

    q2_found_target(self);

    if (!(self->aiflags & Q2_AI_SOUND_TARGET) && self->sight)
        self->sight(self, self->enemy);

    return true;
}

/* ------------------------------------------------------------------------- */
/* FacingIdeal — inline at 0x8005E274                                         */
/* ------------------------------------------------------------------------- */
bool q2_facing_ideal(const q2_monster *m)
{
    s32 delta;

    if (!m)
        return false;

    delta = q2_anglemod(m->angles[2] - m->ideal_yaw);

    /* 513..3583 is "more than 45 degrees either way". The bounds are the
     * original's own, produced by an unsigned compare against 3071. */
    if (delta >= 513 && delta <= 3583)
        return false;

    return true;
}

/* ------------------------------------------------------------------------- */
/* ai_run_missile / ai_run_melee — inline in ai_checkattack                    */
/* ------------------------------------------------------------------------- */
void q2_ai_run_missile(q2_monster *m)
{
    if (!m)
        return;

    m->ideal_yaw = q2_enemy_yaw;
    q2_M_ChangeYaw(m);

    if (q2_facing_ideal(m)) {
        if (m->attack) {
            q2_ai_stats.attack_called++;
            m->attack(m);
        } else {
            q2_ai_stats.attack_missing++;
        }
        m->attack_state = Q2_AS_STRAIGHT;
    }
}

void q2_ai_run_melee(q2_monster *m)
{
    if (!m)
        return;

    m->ideal_yaw = q2_enemy_yaw;
    q2_M_ChangeYaw(m);

    if (q2_facing_ideal(m)) {
        if (m->melee)
            m->melee(m);
        m->attack_state = Q2_AS_STRAIGHT;
    }
}

/* ------------------------------------------------------------------------- */
/* ai_run_slide — inline in ai_run at 0x8005E460                              */
/* ------------------------------------------------------------------------- */
void q2_ai_run_slide(q2_monster *m, s32 dist)
{
    s32 ofs;

    if (!m)
        return;

    m->ideal_yaw = q2_enemy_yaw;
    q2_M_ChangeYaw(m);

    ofs = m->lefty ? Q2_ANGLE_90 : -Q2_ANGLE_90;

    if (q2_M_walkmove(m, m->ideal_yaw + ofs, dist))
        return;

    /* Blocked on that hand, so try the other and remember which way to lead
     * with next time. */
    m->lefty = !m->lefty;
    q2_M_walkmove(m, m->ideal_yaw - ofs, dist);
}

/* ------------------------------------------------------------------------- */
/* ai_checkattack — 0x8005DCFC                                                */
/* ------------------------------------------------------------------------- */
#define Q2_PAUSE_FOREVER 1000000000

static void note_sighting(q2_monster *self)
{
    enemy_position(self->enemy, self->last_sighting);
    self->aiflags &= ~(u32)Q2_AI_LOST_SIGHT;
    self->trail_time = q2_level_state.time;

    if (self->blindfire) {
        self->blind_target[0] = self->enemy->pos[0];
        self->blind_target[1] = self->enemy->pos[1];
        self->blind_target[2] = self->enemy->pos[2];
        self->spawnflags &= ~0xFFu;
    }
}

q2_ai_decision_stats q2_ai_stats;

bool q2_ai_checkattack(q2_monster *m, s32 dist)
{
    q2_ai_stats.checkattack_calls++;

    s32 temp[3];
    bool hes_dead_jim;

    if (!m)
        return false;

    (void)dist;

    /* Running blindly to a combat point: do not fire on the way. */
    if (m->goalentity) {
        if (m->aiflags & Q2_AI_COMBAT_POINT)
            return false;

        if (m->aiflags & Q2_AI_SOUND_TARGET) {
            if (m->enemy
                && q2_level_state.time - m->enemy->teleport_time > 50) {
                /* The noise is stale. Drop it and go back to what we were
                 * doing, releasing any temporary stand-ground with it. */
                if (m->goalentity == m->enemy)
                    m->goalentity = m->movetarget ? m->movetarget : NULL;

                m->aiflags &= ~(u32)Q2_AI_SOUND_TARGET;
                if (m->aiflags & Q2_AI_TEMP_STAND_GROUND)
                    m->aiflags &= ~(u32)(Q2_AI_STAND_GROUND
                                       | Q2_AI_TEMP_STAND_GROUND);
            } else {
                m->show_hostile = q2_level_state.time + Q2_AI_SECONDS(1);
                return false;
            }
        }
    }

    q2_enemy_vis = false;

    /* See if the enemy is dead. */
    hes_dead_jim = false;
    if (!m->enemy || !q2_ent_inuse(m->enemy)) {
        hes_dead_jim = true;
    } else if (m->aiflags & Q2_AI_MEDIC) {
        /* A medic's "enemy" is a corpse to raise; it stops being interesting
         * the moment it has health again. */
        if (m->enemy->health > 0) {
            hes_dead_jim = true;
            m->aiflags &= ~(u32)Q2_AI_MEDIC;
        }
    } else if (m->aiflags & Q2_AI_BRUTAL) {
        if (m->enemy->health <= -80)
            hes_dead_jim = true;
    } else {
        if (m->enemy->health <= 0)
            hes_dead_jim = true;
    }

    if (hes_dead_jim) {
        m->enemy = NULL;

        if (m->oldenemy && m->oldenemy->health > 0) {
            m->enemy    = m->oldenemy;
            m->oldenemy = NULL;
            q2_hunt_target(m);
        } else {
            if (m->movetarget) {
                m->goalentity = m->movetarget;
                if (m->walk)
                    m->walk(m);
            } else {
                /*
                 * The pause is what keeps a monster with nothing to do from
                 * wandering: without it ai_stand falls straight through to
                 * walking and the creature hunts the world entity.
                 */
                m->pausetime = q2_level_state.time + Q2_PAUSE_FOREVER;
                if (m->stand)
                    m->stand(m);
            }
            return true;
        }
    }

    if (!m->enemy)
        return true;

    m->show_hostile = q2_level_state.time + Q2_AI_SECONDS(1);   /* wake others */

    q2_enemy_vis = q2_visible(m, m->enemy);
    if (q2_enemy_vis) {
        m->search_time = q2_level_state.time + Q2_AI_SECONDS(5);
        note_sighting(m);
    }

    q2_enemy_infront = q2_infront(m, m->enemy);
    q2_enemy_range   = q2_range(m, m->enemy);

    temp[0] = m->enemy->pos[0] - m->pos[0];
    temp[1] = m->enemy->pos[1] - m->pos[1];
    temp[2] = m->enemy->pos[2] - m->pos[2];
    q2_enemy_yaw = q2_vectoyaw(temp);

    if (m->attack_state == Q2_AS_MISSILE) {
        q2_ai_run_missile(m);
        return true;
    }
    if (m->attack_state == Q2_AS_MELEE) {
        q2_ai_run_melee(m);
        return true;
    }

    /* Never attack something that cannot be seen. */
    if (!q2_enemy_vis) {
        q2_ai_stats.checkattack_blind++;
        return false;
    }

    q2_ai_stats.checkattack_decided++;
    {
        bool yes = m->checkattack ? m->checkattack(m) : q2_M_CheckAttack(m);

        if (yes)
            q2_ai_stats.checkattack_yes++;
        return yes;
    }
}

/* ------------------------------------------------------------------------- */
/* M_CheckAttack — 0x8005D8C8                                                 */
/* ------------------------------------------------------------------------- */
/*
 * The default every creature gets, installed at `0x80061B18` under a
 * `bne a0, zero` so a caller can suppress it. NO module on the disc installs a
 * checkattack of its own, so this one decides every attack in the game — which
 * is why the port having only a `return false` in its place meant no creature
 * could ever attack, measured as zero fire and melee hook calls over 1800
 * frames with four creatures hunting.
 *
 * Every constant here is the original's, and every one of them is also id's,
 * which is the check that the read is right rather than merely self-consistent:
 * the four chances are 1638, 819, 410 and 82 out of 4096 — 0.4, 0.2, 0.1 and
 * 0.02 — skill 0 halves them and skill 2 or more doubles them, and the flyer's
 * sliding roll is 9830 of 32768, which is 0.3.
 */
static bool checkattack_decide(q2_monster *m);

bool q2_M_CheckAttack(q2_monster *m)
{
    if (!m || !m->enemy)
        return false;

    /* The ENEMY's health, not this creature's (0x8005D8EC). */
    if (m->enemy->health <= 0)
        return checkattack_decide(m);

    /*
     * Eye to eye. When the trace reaches the enemy the decision is the ordinary
     * one below; when it does not, the original falls into a blind-fire path
     * whose second trace runs to `blind_target` at `+0x5C`.
     *
     * That path is DEAD CODE on this build, so returning false here is not a
     * narrowing — it is what the console does. Its first gate is bit 17 of
     * `entity+0x138` (`0x8005D9EC`: `srl 17; andi 1; beq`), and scanning the
     * whole text segment for writes to that word finds eight outside stack
     * frames: four are this function's own attack-state stores, which mask with
     * `0xFFE3FFFF` and preserve bit 17; one is `ai_checkattack`'s; one writes
     * bits 21 and up; and the two `sh` sites reach only the low halfword. **No
     * instruction in the image ever sets bit 17.** The blindfire flag is
     * therefore always clear and the branch behind it cannot be entered.
     */
    if (!q2_visible(m, m->enemy))
        return false;

    return checkattack_decide(m);
}

/*
 * The half of the decision that does not depend on the trace: everything from
 * `0x8005DAD8`, which is where both the dead-enemy branch and the
 * trace-reached-the-enemy branch land.
 */
static bool checkattack_decide(q2_monster *m)
{
    s32 chance, skill = q2_cre_skill();

    /* Melee range: a coin the lowest skill loses three times in four
     * (0x8005DB04's `rand() & 3`), then melee if there is one and a missile
     * otherwise. */
    if (q2_enemy_range == Q2_RANGE_MELEE) {
        if (skill == 0 && (q2_random_next() & 3) != 0)
            return false;

        m->attack_state = m->melee ? Q2_AS_MELEE : Q2_AS_MISSILE;
        return true;
    }

    if (!m->attack)
        return false;
    if (q2_level_state.time < m->attack_finished)
        return false;
    if (q2_enemy_range == Q2_RANGE_FAR)
        return false;

    if (m->aiflags & Q2_AI_STAND_GROUND)      chance = 1638;   /* 0.40 */
    else if (q2_enemy_range == Q2_RANGE_MELEE) chance = 819;   /* 0.20 */
    else if (q2_enemy_range == Q2_RANGE_NEAR)  chance = 410;   /* 0.10 */
    else if (q2_enemy_range == Q2_RANGE_MID)   chance = 82;    /* 0.02 */
    else                                       return false;

    if (skill == 0)      chance /= 2;
    else if (skill >= 2) chance *= 2;

    if ((q2_random_next() & 0xFFF) < chance) {
        m->attack_state = Q2_AS_MISSILE;
        /*
         * THE RELOAD IS TWO SECONDS, WHICH IS TWENTY TICKS.
         *
         * 0x8005DC4C draws `rand() & 0x7FFF`, 0x8005DC50..0x8005DC58 build
         * `v0*5*4` — twenty — and 0x8005DC5C..0x8005DC7C divide that by 32767
         * with the 0x80010003 magic-number sequence before 0x8005DC80 adds
         * level.time and 0x8005DC88 stores it to entity+0x110. So the draw is
         * `random() * 20` TICKS.
         *
         * This one literal had been left at id's seconds value on the ten-Hz
         * clock, which capped the cooldown at 0.2 s and let every gun-carrying
         * creature fire on the AI tick after the one it fired on. Everything
         * else in this file already goes through Q2_AI_SECONDS — HuntTarget's
         * `addiu a1, zero, 10` at 0x8005D2F0 is the same conversion.
         */
        m->attack_finished = q2_level_state.time
                           + Q2_AI_SECONDS(2) * (q2_random_next() & 0x7FFF) / 32767;
        return true;
    }

    /* A flyer that did not shoot picks a strafe instead — 9830 of 32768. */
    if (m->flags & Q2_FL_FLY)
        m->attack_state = ((q2_random_next() & 0x7FFF) < 9830) ? Q2_AS_SLIDING
                                                     : Q2_AS_STRAIGHT;
    return false;
}

/* ------------------------------------------------------------------------- */
/* ai_move — 0x8005ED44                                                       */
/* ------------------------------------------------------------------------- */
void q2_ai_move(q2_monster *m, s32 dist)
{
    /* Pure animation-driven movement, no turning and no thinking: used by
     * pain and death, which is why it must not run FindTarget. */
    if (m)
        q2_M_walkmove(m, m->angles[2], dist);
}

/* ------------------------------------------------------------------------- */
/* ai_stand — 0x8005CDA8                                                      */
/* ------------------------------------------------------------------------- */
void q2_ai_stand(q2_monster *m, s32 dist)
{
    s32 v[3];

    if (!m)
        return;

    if (dist)
        q2_M_walkmove(m, m->angles[2], dist);

    if (m->aiflags & Q2_AI_STAND_GROUND) {
        if (m->enemy) {
            bool acted;

            v[0] = m->enemy->pos[0] - m->pos[0];
            v[1] = m->enemy->pos[1] - m->pos[1];
            v[2] = m->enemy->pos[2] - m->pos[2];
            m->ideal_yaw = q2_vectoyaw(v);

            /*
             * A creature that only stopped because it was told to will start
             * moving again the moment it has to turn to keep facing — which is
             * how a guard peels off its post when you walk past it.
             */
            if (m->angles[2] != m->ideal_yaw
                && (m->aiflags & Q2_AI_TEMP_STAND_GROUND)) {
                m->aiflags &= ~(u32)(Q2_AI_STAND_GROUND
                                   | Q2_AI_TEMP_STAND_GROUND);
                if (m->run)
                    m->run(m);
            }

            if (!(m->aiflags & Q2_AI_MANUAL_STEERING))
                q2_M_ChangeYaw(m);

            acted = q2_ai_checkattack(m, 0);

            if (m->enemy && (m->enemy->spawnflags & Q2_SVFLAG_INUSE)
                && q2_visible(m, m->enemy)) {
                note_sighting(m);
            } else if (!acted) {
                q2_find_target(m);
            }
        } else {
            q2_find_target(m);
        }
        return;
    }

    if (q2_find_target(m))
        return;

    if (m->pausetime < q2_level_state.time) {
        if (m->walk)
            m->walk(m);
        return;
    }

    /*
     * Idle chatter. The first call only arms the timer, so a monster does not
     * grunt the instant the level starts; every one after it also plays the
     * sound. 15 to 30 seconds, in tenths.
     */
    if (!(m->spawnflags & Q2_SPAWNFLAG_AMBUSH) && m->idle
        && m->idle_time < q2_level_state.time) {
        if (m->idle_time) {
            m->idle(m);
            m->idle_time = q2_level_state.time + 150 + random_scaled(150);
        } else {
            m->idle_time = q2_level_state.time + random_scaled(150);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* ai_walk — 0x8005ED6C                                                       */
/* ------------------------------------------------------------------------- */
void q2_ai_walk(q2_monster *m, s32 dist)
{
    if (!m)
        return;

    q2_M_MoveToGoal(m, dist);

    /* Check for noticing a player. */
    if (q2_find_target(m))
        return;

    /* Walking uses `search`, where standing uses `idle` — two different
     * sounds, and mixing them up makes a patrolling monster sound bored. */
    if (m->search && m->idle_time < q2_level_state.time) {
        if (m->idle_time) {
            m->search(m);
            m->idle_time = q2_level_state.time + 150 + random_scaled(150);
        } else {
            m->idle_time = q2_level_state.time + random_scaled(150);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* ai_charge — 0x8005EE84                                                     */
/* ------------------------------------------------------------------------- */
void q2_ai_charge(q2_monster *m, s32 dist)
{
    s32 v[3];

    if (!m)
        return;

    if (m->blindfire && m->enemy && q2_visible(m, m->enemy)) {
        m->blind_target[0] = m->enemy->pos[0];
        m->blind_target[1] = m->enemy->pos[1];
        m->blind_target[2] = m->enemy->pos[2];
    }

    if (!(m->aiflags & Q2_AI_MANUAL_STEERING) && m->enemy) {
        v[0] = m->enemy->pos[0] - m->pos[0];
        v[1] = m->enemy->pos[1] - m->pos[1];
        v[2] = m->enemy->pos[2] - m->pos[2];
        m->ideal_yaw = q2_vectoyaw(v);
    }

    q2_M_ChangeYaw(m);

    /* Charging commits: it keeps facing the target and does not re-path. */
    if (dist)
        q2_M_walkmove(m, m->angles[2], dist);
}

/* ------------------------------------------------------------------------- */
/* ai_run — 0x8005E350                                                        */
/* ------------------------------------------------------------------------- */
/*
 * The longest function in the AI, and the one that carries the behaviour
 * people remember: a monster that loses you does not give up, it walks to
 * where it last saw you, then follows your footsteps, then leans around the
 * corner to look.
 */
u32 q2_ai_native_s2 = 1;

void q2_ai_run(q2_monster *m, s32 dist)
{
    s32 v[3];
    q2_monster *save;
    q2_monster tempgoal;
    bool isnew = false;
    const q2_trail_spot *marker = NULL;

    if (!m)
        return;

    /* If we are going to a combat point, just proceed. */
    if (m->aiflags & Q2_AI_COMBAT_POINT) {
        q2_M_MoveToGoal(m, dist);
        return;
    }

    if (m->aiflags & Q2_AI_SOUND_TARGET) {
        if (!m->enemy)
            return;

        v[0] = m->pos[0] - m->enemy->pos[0];
        v[1] = m->pos[1] - m->enemy->pos[1];
        v[2] = m->pos[2] - m->enemy->pos[2];

        /* Close enough to the noise: stop and look around rather than walking
         * on top of it. 768 units, id's 64 at this scale. */
        if (q2_vector_length_sq(v) < 768LL * 768LL) {
            m->aiflags |= (Q2_AI_STAND_GROUND | Q2_AI_TEMP_STAND_GROUND);
            if (m->stand)
                m->stand(m);
            return;
        }

        q2_M_MoveToGoal(m, dist);

        if (!q2_find_target(m))
            return;
    }

    {
        u32 saved = q2_ai_native_s2;
        bool acted;
        q2_ai_native_s2 = 1; /* 0x8005E38C loads the live player pointer. */
        acted = q2_ai_checkattack(m, dist);
        q2_ai_native_s2 = saved;
        if (acted) return;
    }

    if (m->attack_state == Q2_AS_SLIDING) {
        q2_ai_run_slide(m, dist);
        return;
    }

    if (q2_enemy_vis) {
        q2_M_MoveToGoal(m, dist);
        note_sighting(m);
        return;
    }

    /* Give up on a search that has run long enough. */
    if (m->search_time
        && q2_level_state.time > m->search_time + Q2_AI_SECONDS(20)) {
        q2_M_MoveToGoal(m, dist);
        m->search_time = 0;
        return;
    }

    /*
     * From here the creature is chasing a memory. It borrows a scratch entity
     * as its goal so that M_MoveToGoal has something with an origin to walk
     * at, and puts the real one back before returning.
     */
    save = m->goalentity;
    q2_monster_init(&tempgoal);
    tempgoal.in_use      = true;
    tempgoal.spawnflags |= Q2_SVFLAG_INUSE;
    m->goalentity = &tempgoal;

    if (!(m->aiflags & Q2_AI_LOST_SIGHT)) {
        /* Just lost sight of the player: decide where to go first. */
        m->aiflags |= (Q2_AI_LOST_SIGHT | Q2_AI_PURSUIT_LAST_SEEN);
        m->aiflags &= ~(u32)(Q2_AI_PURSUE_NEXT | Q2_AI_PURSUE_TEMP);
        isnew = true;
    }

    if (m->aiflags & Q2_AI_PURSUE_NEXT) {
        m->aiflags &= ~(u32)Q2_AI_PURSUE_NEXT;

        /* Getting this far earns more search time. */
        m->search_time = q2_level_state.time + Q2_AI_SECONDS(5);

        if (m->aiflags & Q2_AI_PURSUE_TEMP) {
            /* We were at a corner-peek waypoint; resume the real goal. */
            m->aiflags &= ~(u32)Q2_AI_PURSUE_TEMP;
            marker = NULL;
            m->last_sighting[0] = m->saved_goal[0];
            m->last_sighting[1] = m->saved_goal[1];
            m->last_sighting[2] = m->saved_goal[2];
            isnew = true;
        } else if (m->aiflags & Q2_AI_PURSUIT_LAST_SEEN) {
            m->aiflags &= ~(u32)Q2_AI_PURSUIT_LAST_SEEN;
            marker = q2_trail_pick_first(m);
        } else {
            marker = q2_trail_pick_next(m);
        }

        if (marker) {
            q2_ai_stats.pursue_marker++;
            m->last_sighting[0] = marker->origin[0];
            m->last_sighting[1] = marker->origin[1];
            m->last_sighting[2] = marker->origin[2];
            m->trail_time  = marker->timestamp;
            m->angles[2]   = marker->yaw;
            m->ideal_yaw   = marker->yaw;
            isnew = true;
        }
    }

    v[0] = m->pos[0] - m->last_sighting[0];
    v[1] = m->pos[1] - m->last_sighting[1];
    v[2] = m->pos[2] - m->last_sighting[2];

    /* Arrived at the waypoint — within one frame's step of it — so ask for the
     * next one on the way back through. */
    if ((s64)dist * dist >= q2_vector_length_sq(v)) {
        m->aiflags |= Q2_AI_PURSUE_NEXT;
        q2_ai_stats.pursue_arrived++;
        dist = q2_vector_length(v);
    }

    tempgoal.pos[0] = m->last_sighting[0];
    tempgoal.pos[1] = m->last_sighting[1];
    tempgoal.pos[2] = m->last_sighting[2];

    if (isnew) {
        q2_ai_trace tr;

        g_world.trace(g_world.user, m->pos, m->mins, m->maxs,
                      m->last_sighting, m, Q2_MASK_PLAYERSOLID, &tr);

        if (tr.fraction < Q2_TRACE_ONE) {
            /*
             * The straight line is blocked. Look a body's width to each side
             * at roughly half the remaining distance and walk toward whichever
             * has more room — which reads, from outside, as peeking round the
             * corner.
             */
            s32 d1, d2, center, left, right;
            s32 v_forward[3], v_right[3];
            s32 left_target[3], right_target[3];
            s16 off[3];

            v[0] = m->goalentity->pos[0] - m->pos[0];
            v[1] = m->goalentity->pos[1] - m->pos[1];
            v[2] = m->goalentity->pos[2] - m->pos[2];
            d1 = q2_vector_length(v);

            center = tr.fraction;
            d2 = (s32)(((s64)d1 * ((center + Q2_TRACE_ONE) / 2)) >> 12);

            m->ideal_yaw = q2_vectoyaw(v);
            m->angles[2] = m->ideal_yaw;

            q2_angle_vectors(m->angles, v_forward, v_right, NULL);

            off[0] = (s16)d2;  off[1] = 0;  off[2] = -192;
            q2_project_source(m->pos, off, v_forward, v_right, left_target);
            g_world.trace(g_world.user, m->pos, m->mins, m->maxs,
                          left_target, m, Q2_MASK_PLAYERSOLID, &tr);
            left = tr.fraction;

            off[0] = (s16)d2;  off[1] = 0;  off[2] = 192;
            q2_project_source(m->pos, off, v_forward, v_right, right_target);
            g_world.trace(g_world.user, m->pos, m->mins, m->maxs,
                          right_target, m, Q2_MASK_PLAYERSOLID, &tr);
            right = tr.fraction;

            center = d2 ? (s32)(((s64)d1 * center) / d2) : center;

            if (left >= center && left > right) {
                if (left < Q2_TRACE_ONE) {
                    off[0] = (s16)(((s64)d2 * left) >> 13);
                    off[1] = 0;
                    off[2] = -192;
                    q2_project_source(m->pos, off, v_forward, v_right,
                                      left_target);
                }
                m->saved_goal[0] = m->last_sighting[0];
                m->saved_goal[1] = m->last_sighting[1];
                m->saved_goal[2] = m->last_sighting[2];
                m->aiflags |= Q2_AI_PURSUE_TEMP;
                tempgoal.pos[0] = left_target[0];
                tempgoal.pos[1] = left_target[1];
                tempgoal.pos[2] = left_target[2];
                m->last_sighting[0] = left_target[0];
                m->last_sighting[1] = left_target[1];
                m->last_sighting[2] = left_target[2];
                v[0] = tempgoal.pos[0] - m->pos[0];
                v[1] = tempgoal.pos[1] - m->pos[1];
                v[2] = tempgoal.pos[2] - m->pos[2];
                m->ideal_yaw = q2_vectoyaw(v);
                m->angles[2] = m->ideal_yaw;
            } else if (right >= center && right > left) {
                if (right < Q2_TRACE_ONE) {
                    off[0] = (s16)(((s64)d2 * right) >> 13);
                    off[1] = 0;
                    off[2] = 192;
                    q2_project_source(m->pos, off, v_forward, v_right,
                                      right_target);
                }
                m->saved_goal[0] = m->last_sighting[0];
                m->saved_goal[1] = m->last_sighting[1];
                m->saved_goal[2] = m->last_sighting[2];
                m->aiflags |= Q2_AI_PURSUE_TEMP;
                tempgoal.pos[0] = right_target[0];
                tempgoal.pos[1] = right_target[1];
                tempgoal.pos[2] = right_target[2];
                m->last_sighting[0] = right_target[0];
                m->last_sighting[1] = right_target[1];
                m->last_sighting[2] = right_target[2];
                v[0] = tempgoal.pos[0] - m->pos[0];
                v[1] = tempgoal.pos[1] - m->pos[1];
                v[2] = tempgoal.pos[2] - m->pos[2];
                m->ideal_yaw = q2_vectoyaw(v);
                m->angles[2] = m->ideal_yaw;
            }
        }
    }

    q2_M_MoveToGoal(m, dist);

    m->goalentity = save;
}

/* ------------------------------------------------------------------------- */
/* The verb table, in the executable's slot order — 0x800D561C               */
/* ------------------------------------------------------------------------- */
const q2_ai_verb_fn q2_ai_verbs[8] = {
    NULL,               /* 0 — the original stores zero here                 */
    q2_ai_stand,        /* 1 — 0x8005CDA8                                    */
    q2_ai_walk,         /* 2 — 0x8005ED6C                                    */
    q2_ai_run,          /* 3 — 0x8005E350                                    */
    q2_ai_charge,       /* 4 — 0x8005EE84                                    */
    q2_ai_move,         /* 5 — 0x8005ED44                                    */
    NULL,               /* 6, 7 — the shared do-nothing handler              */
    NULL
};
