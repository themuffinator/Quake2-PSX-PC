#include "random.h"
#include "monster.h"

#include "combat.h"        /* Q2_HEALTH_FLOOR: one copy of 0x800629B4's clamp */
#include "worldscale.h"

#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "aimove.h"
#include "crebind.h"
#include "modelent.h"      /* q2_gib_monster_corpse: the corpse tick's dispatch */
#include "trig.h"

const char *const q2_ai_verb_names[Q2_AI_VERB_COUNT] = {
    "none", "stand", "walk", "run", "charge", "move"
};

q2_level q2_level_state;

void q2_level_reset(void)
{
    memset(&q2_level_state, 0, sizeof(q2_level_state));
}

void q2_monster_init(q2_monster *m)
{
    static u32 audio_generation;
    if (!m)
        return;

    memset(m, 0, sizeof(*m));
    if (++audio_generation == 0) ++audio_generation;
    m->audio_generation = audio_generation;

    m->class_id    = 0;
    m->speed_scale = 10;      /* neutral: the frame scale is /10 */
    m->render_scale = Q2_ONE_12;

    /*
     * YAW_SPEED STARTS UNSET, and that is the whole point of the `bne`.
     *
     * It used to be 200 for every creature. The console has no such default:
     * the field is zero on a fresh entity and each go-routine installs id's own
     * number only IF IT IS STILL ZERO — 228 for a walker (0x80062434, and the
     * 228 is in the delay slot so only the store is conditional), 114 for a
     * flyer (0x800624E0) and 114 for a swimmer (0x80062574). A module that set
     * its own before waking would keep it, which is what the test is for.
     *
     * So this is now only what a creature carries BEFORE its go-routine runs,
     * and q2_monster_start_go is where the real value arrives.
     */
    m->yaw_speed   = 0;

    /*
     * The movement box, in the ORIGIN frame — a 572 cube centred on the entity.
     *
     * That is what 0x80050FA0 writes, and it is a SHARED placement routine
     * rather than an item quirk: it takes a halfword pair from its caller and
     * builds mins = (-a,-a,-a), maxs = (+b,+b,+b) at obj+0x6C..0x76, and both
     * pairs reachable on this disc (0x800AEAB8 and 0x800AECEC) hold 286 / 572.
     * 0x8005CCF4 then reads those six halfwords back for every step trace.
     *
     * It used to be a FEET-frame box — mins[1] = -560, maxs[1] = 0 — which was
     * asymmetric about a point the rest of the system treats as the centre.
     * Only the sign of "is this box degenerate" depended on it until now; with
     * the hull select in aiworld.c it decides which collision model a trace
     * runs against, and q2_SV_CloseEnough reads it directly.
     *
     * NOT filled per model, AND THAT IS ONLY TRUE OF THIS BOX. The comment
     * here used to go further and say the original does not vary ANY creature
     * volume per model — "every writer reachable from here passes the same
     * 286/572 pair" — which read SetupBoxDrop's `a2` and missed its FIFTH
     * argument. That fifth argument is per model and it is the hit cylinder;
     * see `hit_radius` in monster.h and q2_monster_bind_hit_size below. The
     * movement hull really is the same 286/572 cube for every creature, which
     * is what 0x800AECEC holds and what 0x80050FD4..0x80051022 writes.
     *
     * What differs here is by CLASS: 0x8005CCFC short-circuits class 114, the
     * path corner, to a -96/+96 cube.
     */
    m->mins[0] = m->mins[1] = m->mins[2] = -286;
    m->maxs[0] = m->maxs[1] = m->maxs[2] =  286;

    /*
     * And the hit cylinder at GetCreHitSize's NO-MODEL fallback, 286 and 600
     * (0x8006DC6C `addiu v0, zero, 286` / 0x8006DC74 `addiu v0, zero, 600`).
     * A creature whose model is bound replaces both through
     * q2_monster_bind_hit_size; one whose model never resolves keeps these,
     * which is exactly what the console gives a creature with cre+0x10 null.
     *
     * 600, not 572. The two pairs are different constants and the port had
     * been using the hull's for both.
     */
    m->hit_radius = 286;
    m->hit_height = 600;

    /*
     * The eye, as an offset from the ORIGIN — `visible` (0x8005B950) adds this
     * to the entity's origin, not to its feet.
     *
     * -290 puts a creature's eye exactly where a player's is: the player's
     * origin is feet - Q2_EYE_BASE and the player's eye is feet -
     * Q2_VIEW_STAND, so the offset between them is 290.
     *
     * IT IS NOW ONLY THE PRE-START DEFAULT. walkmonster_go (0x80062448)
     * computes `nor v0, zero, *(u16*)(obj + 0xF8)` from the creature's own
     * model, flymonster_go (0x800624F0) stores -250 and swimmonster_go
     * (0x80062584) -100, and q2_monster_start_go installs whichever applies as
     * soon as the creature wakes. This value survives only for a creature that
     * has not woken, or one whose `model_ext2` nothing filled in.
     *
     * It was -400, which was chosen when `pos` was the FEET and put the eye
     * 400 above them. Left at -400 after the origin lift it becomes 686 above
     * the feet — a hundred units above the creature's own head, and therefore
     * usually inside the ceiling. Measured on BASE1 standing 1600 units from a
     * Soldier: 260 of 260 sight lines blocked and nothing ever hunting.
     */
    m->view_height = -(s16)(Q2_VIEW_STAND - Q2_EYE_BASE);
}

s32 q2_monster_frame_dist(const q2_monster *m, const q2_mframe *frame)
{
    if (!m || !frame)
        return 0;

    /* A held frame plays its animation without advancing the creature, which
     * is how a wind-up or a recoil stays in place. 0x80061924. */
    if (m->aiflags & Q2_AI_HOLD_FRAME)
        return 0;

    /*
     * AND A CORPSE NEVER TRANSLATES. This is the whole of "bodies hang in the
     * air", and it is faithful rather than a hedge.
     *
     * The console's corpse handler (0x8007F71C) makes four calls — the
     * dissolve gate 0x8005B2A8, the presentation chain 0x8005B880, the actor
     * list append 0x800552B4 and the destruction dispatcher 0x8007CEB4 — and
     * NO POSITION WRITE of any kind. It cannot: the detach at 0x8007F098 freed
     * the edict, so there is nothing left for a movement verb to steer. This
     * port keeps one structure for both halves and so kept reaching
     * `ai_move`, which runs the swept step — and that step's LIFT raises the
     * body a full Q2_STEPSIZE before it moves horizontally. Where the
     * movement hull disagrees with the visible floor
     * the drop that should undo the lift returns almost at once. Measured on
     * BASE2: a Soldier corpse left 215 units up over ground `PrimaryColl`
     * reports flat, with the drop trace returning 9 of 4096.
     *
     * A LIVING creature takes the identical lift and recovers by stepping
     * again onto correctly registered ground — about twelve steps to walk back
     * down. A corpse has no next step: the death moves' frame distances fall to
     * zero within a few frames, so it keeps whatever the last one gave it.
     * Suppressing the translation alone, with the hull untouched and the
     * animation still playing out in full, leaves the body on the floor for
     * the whole run — necessary and sufficient, established by counterfactual.
     *
     * THE SCOPE USED TO BE WIDER THAN THE CONSOLE'S and is not any more.
     * `m->corpse` is raised once the module's own `*_dead` has run, which is
     * the END of the death move; the rule here used to be "dead", which is
     * the FIRST tick of it, and the whole death move lost its translation.
     *
     * That is not a small loss. The Infantry's Death1 opens at dist -4 — -48
     * world units once the scale is applied — and carries non-zero steps most
     * of the way through. The body is meant to stagger as it goes down, and
     * instead it played the clip standing on the spot it was shot.
     *
     * The wider rule was taken as a trade against bodies stranded 215 units up
     * over ground `PrimaryColl` reports flat, because `ai_move`'s swept step
     * lifts a full Q2_STEPSIZE before it moves horizontally and the drop that
     * should undo the lift returns almost at once. Measured again on the maps
     * that actually place an Infantry, the trade no longer pays: BASE2's dies
     * through Death1 with steps of 47, 11 and 47 units and settles at y 1632,
     * which is the floor its living self stood on, and a staged BASE2 Soldier
     * death ends at the same positions with the rule either way.
     *
     * The hull disagreement is a real second defect with its own victim (that
     * twelve-step hop) and is still deliberately NOT fixed here.
     */
    if (m->corpse)
        return 0;

    return ((s32)frame->dist * (s32)m->speed_scale * 12) / 10;
}

s64 q2_monster_dist_sq(const q2_monster *m, const s32 target[3])
{
    s64 dx, dy, dz;

    if (!m || !target)
        return 0;

    dx = (s64)target[0] - m->pos[0];
    dy = (s64)target[1] - m->pos[1];
    dz = (s64)target[2] - m->pos[2];

    return dx * dx + dy * dy + dz * dz;
}

/*
 * range() — 0x8005D560..0x8005D5C4, and again identically inside
 * ai_checkattack at 0x8005E194.
 *
 * The comparison is against the SQUARED distance, which is why the melee band
 * is a per-class number: one class id gets double reach and everything else
 * shares the short one.
 */
q2_range_band q2_range(const q2_monster *self, const q2_monster *other)
{
    s64 d2;
    s64 melee;

    if (!self || !other)
        return Q2_RANGE_FAR;

    d2 = q2_monster_dist_sq(self, other->pos);

    melee = (self->class_id == Q2_CLASS_LONG_MELEE)
          ? (s64)Q2_MELEE_DISTANCE_BIG * Q2_MELEE_DISTANCE_BIG
          : (s64)Q2_MELEE_DISTANCE * Q2_MELEE_DISTANCE;

    if (d2 <= melee - 1)
        return Q2_RANGE_MELEE;
    if (d2 <= (s64)Q2_RANGE_NEAR_DIST * Q2_RANGE_NEAR_DIST - 1)
        return Q2_RANGE_NEAR;
    if (d2 <= (s64)Q2_RANGE_MID_DIST * Q2_RANGE_MID_DIST - 1)
        return Q2_RANGE_MID;

    return Q2_RANGE_FAR;
}

/*
 * infront() — 0x8005D608..0x8005D774.
 *
 * AngleVectors, then normalise the offset, then a 1.12 dot product with
 * per-term rounding, compared against 1230. The original really does normalise
 * rather than compare squares, so this does too: the rounding of the
 * normalise is visible in the result at short range.
 */
bool q2_infront(const q2_monster *self, const q2_monster *other)
{
    s32 forward[3];
    s32 vec[3];
    s32 dot;

    if (!self || !other)
        return false;

    q2_angle_vectors(self->angles, forward, NULL, NULL);

    vec[0] = other->pos[0] - self->pos[0];
    vec[1] = other->pos[1] - self->pos[1];
    vec[2] = other->pos[2] - self->pos[2];

    q2_vector_normalize(vec);

    dot = (s32)(((s64)vec[0] * forward[0] + 4095) >> 12)
        + (s32)(((s64)vec[1] * forward[1] + 4095) >> 12)
        + (s32)(((s64)vec[2] * forward[2] + 4095) >> 12);

    return dot >= Q2_INFRONT_DOT;
}

bool q2_monster_damage(q2_monster *m, s16 amount)
{
    if (!m || amount <= 0 || !m->in_use || m->dead)
        return false;

    m->health = (s16)(m->health - amount);

    if (m->health > 0)
        return false;

    m->dead = true;

    /* Below the gib threshold the body is destroyed outright rather than
     * playing a death animation. The threshold is negative and per-creature —
     * 240 health with -60 gib on one, 149 with -70 on another. */
    return true;
}

/* ------------------------------------------------------------------------- */
/* The death drop — the table at 0x800AB79C, read from 0x80020680             */
/* ------------------------------------------------------------------------- */
/*
 * gp+276 = 0x800AE714 — A STATIC, and the Tank Commander's drop comes out of
 * it rather than out of the weapon table it appears to consult.
 *
 * `xrefs 0x800AE714` gives exactly four sites: the store/load pair at
 * 0x80020798/0x8002079C below, and a second pair at 0x80020F24/0x80020F28
 * inside the ammo-picker at 0x80020E88 — and `xrefs 0x80020E88` returns ZERO
 * calls. That picker sits immediately after the `jr ra` of 0x80020E24 and
 * nothing jumps to it or JALs it, so it is unreachable code on this build. In
 * single player the only writer of this word that can ever run is 0x80020798,
 * and the only value it writes is 23. The variable is faithfully reproduced,
 * quirk and all, because the shape is what was read — but the observable
 * result of the Tankcomm arm is invariably Bullets.
 */
static s32 g_last_ammo_pick;

/*
 * The Tank Commander arm, 0x80020754 — a specialised inline of that same
 * unreachable ammo-picker.
 *
 *   0x80020754 `v1 = weapon_id - 1`; 0x80020758 `sltiu v0, v1, 11` bounds it;
 *   a0 is PRE-MASKED `andi a0, a0, 0x91` in the branch's DELAY SLOT at
 *   0x80020760 — 0x01 Blaster | 0x10 Chaingun | 0x80 Rocket Launcher.
 *   The 11-entry jump table at 0x800AB814, dumped byte for byte:
 *     ids 1,2,3,6,7,9,10,11 -> 0x8002079C   (straight to the load)
 *     ids 4,5               -> 0x80020784   `andi v0, a0, 0x18`, and
 *                                            0x18 & 0x91 == 0x10, the Chaingun
 *     id  8                 -> 0x8002078C   `andi v0, a0, 0x80`, the RL
 *   0x80020790 if that is nonzero, 0x80020798 `sw 23, 276(gp)`.
 *   0x8002079C `lw s0, 276(gp)`; 0x800207A4 spawns it if nonzero, else
 *   0x800207B0 sets s0 = 23.
 *
 * Ten of the eleven table entries reach the same two instructions and the one
 * distinguishing arm writes the same 23, so this is NOT a weapon-indexed drop
 * with variety in it. Class rows 25 (Tankcomm) and 26 (no row on the disc) both
 * come here.
 */
static s8 drop_tankcomm(s16 weapon_id, u32 weapon_bits)
{
    u32 masked = weapon_bits & 0x91u;   /* 0x80020760, in the delay slot */
    s32 hit    = 0;

    if ((u32)(weapon_id - 1) < 11u) {
        if (weapon_id == 4 || weapon_id == 5)
            hit = (s32)(masked & 0x18u);    /* 0x80020788 -> the Chaingun bit */
        else if (weapon_id == 8)
            hit = (s32)(masked & 0x80u);    /* 0x8002078C -> the RL bit       */
    }

    if (hit != 0)
        g_last_ammo_pick = 23;              /* 0x80020798 */

    if (g_last_ammo_pick != 0)              /* 0x800207A4 */
        return (s8)g_last_ammo_pick;

    return 23;                              /* 0x800207B0 */
}

s32 q2_monster_drop_ammo_static(void)
{
    return g_last_ammo_pick;
}

/*
 * The Gunner arm, 0x80020730. Delay slots decide this one.
 *
 *   0x80020730 `andi v0, a0, 0x40`      the Grenade Launcher bit (weapon id 7)
 *   0x80020734 `beq v0, zero, 0x800207D0`
 *   0x80020738 `addiu s0, zero, 23`     IN THE DELAY SLOT — unconditional
 *   0x8002073C `addiu v0, a1, -4`       the player's CURRENT weapon
 *   0x80020740 `sltiu v0, v0, 2`        Machinegun 4 or Chaingun 5
 *   0x80020744 `bne v0, zero, 0x800207D0`   ...and it stays 23
 *   0x8002074C `j 0x800207D0` / 0x80020750 `addiu s0, zero, 25`
 *
 * So it is "Bullets unless you own a Grenade Launcher AND are not currently
 * holding a bullet weapon"; the current-weapon test is easy to miss and the
 * unconditional 23 easier still.
 */
static s8 drop_gunner(s16 weapon_id, u32 weapon_bits)
{
    if ((weapon_bits & 0x40u) == 0)
        return 23;                          /* Bullets */
    if ((u32)(weapon_id - 4) < 2u)
        return 23;                          /* Bullets */
    return 25;                              /* Grenades */
}

s8 q2_monster_drop_item_for_class(u8 pop_class, s16 weapon_id, u32 weapon_bits)
{
    /*
     * `v1 = (s16)(slot[+2] - 5)` at 0x800206DC..0x800206E4 and
     * `sltiu v0, v1, 30` at 0x800206E8, so the window is class rows 5..34 and
     * anything outside it falls to 0x800207D0 with s0 == 0 — and 0x800207D0
     * `beq s0, zero, 0x80020848` means NOTHING SPAWNS.
     *
     * The 30 words at 0x800AB79C were dumped and decoded one at a time; each
     * row below carries its table index and the class name from
     * `q2psx-inspect classes`. The three Soldier rows landing on Shells, Cells
     * and Bullets — matching the shotgun, blaster and machinegun variants — is
     * the cross-check that says the read is right.
     */
    s32 idx = (s16)((s16)pop_class - 5);

    if ((u32)idx >= 30u)
        return 0;

    switch (idx) {
    case  0: return 26;   /*  5 Ironmaiden           -> Rockets   0x80020718 */
    case  1: return  0;   /*  6 Flipper              -> nothing   0x800207D0 */
    case  2: return  0;   /*  7 (no class row)       -> nothing              */
    case  3: return 24;   /*  8 Flyer                -> Cells     0x80020720 */
    case  4: return 28;   /*  9 Gladiator            -> Slugs     0x80020728 */
    case  5: return drop_gunner(weapon_id, weapon_bits);
                          /* 10 Gunner                            0x80020730 */
    case  6: return 24;   /* 11 Hover                -> Cells                */
    case  7: return 23;   /* 12 Infantry             -> Bullets   0x800207AC */
    case  8: return  0;   /* 13 Jorg                 -> nothing              */
    case  9: return  0;   /* 14 Rider                -> nothing              */
    case 10: return 24;   /* 15 Medic                -> Cells                */
    case 11: return  0;   /* 16 (no class row)       -> nothing              */
    case 12: return  0;   /* 17 Parasite             -> nothing              */
    case 13: return 27;   /* 18 Soldier shotgun,30hp -> Shells    0x80020710 */
    case 14: return 24;   /* 19 Soldier blaster,20hp -> Cells                */
    case 15: return 23;   /* 20 Soldier machgun,40hp -> Bullets              */
    case 16: return  0;   /* 21 (no class row)       -> nothing              */
    case 17: return  0;   /* 22 (no class row)       -> nothing              */
    case 18: return  0;   /* 23 (no class row)       -> nothing              */
    case 19: return 38;   /* 24 Boss1                -> Rocket Launcher      */
    case 20: return drop_tankcomm(weapon_id, weapon_bits);
                          /* 25 Tankcomm                          0x80020754 */
    case 21: return drop_tankcomm(weapon_id, weapon_bits);
                          /* 26 (no class row) same arm                      */
    case 22: return  0;   /* 27 DeadComm             -> nothing              */
    case 23: return 24;   /* 28 Arachner             -> Cells                */
    case 24: return 26;   /* 29 Blitz                -> Rockets              */
    case 25: return 25;   /* 30 (no class row)       -> Grenades  0x8002074C */
    case 26: return 29;   /* 31 Flamer               -> Fuel      0x800207B4 */
    case 27: return 21;   /* 32 Strider              -> A-M Bomb  0x800207BC */
    case 28: return  0;   /* 33 (no class row)       -> nothing              */
    case 29: return  9;   /* 34 Insane               -> Health    0x800207C4 */
    default: break;
    }

    return 0;
}

/*
 * THE FOUR-SLOT QUEUE, 0x800C6D70..0x800C6ED0.
 *
 * The port keeps the two fields the picker reads — the class row from
 * slot[+2] and the position, which is the first three words of the 80 bytes
 * copied out of obj+0x54 (0x8002085C reads them back at 0x80020984 and writes
 * them to the new item's +0xA4 and +0x54).
 *
 * slot[+4] is the multiplayer arm only: 0x80020DC4 tests 0x800AEBCC and, when
 * it is set, stores `lh (obj+0x0C)+0x66` — the client index — which 0x800206B8
 * then uses to index a byte table at 0x8009B510 instead of the class table.
 * Single player leaves it 0 and this port is single player, so the field is
 * named here and not carried.
 */
#define Q2_MONSTER_DROP_SLOTS 4

typedef struct q2_monster_drop_slot {
    bool used;              /* slot[+0], 0x80020DB0 writes 1               */
    u8   pop_class;         /* slot[+2], 0x80020DB4 `lhu obj+0xD2`         */
    s32  pos[3];            /* the head of the 80 bytes from obj+0x54      */
} q2_monster_drop_slot;

static q2_monster_drop_slot g_drop_queue[Q2_MONSTER_DROP_SLOTS];

static void (*g_drop_fn)(const q2_monster_drop_request *req, void *user);
static void  *g_drop_user;

/* 0x8002069C `lh a1, 102(v0)` and 0x800206A0 `lw a0, 104(v0)` off 0x800C7C60,
 * so 0x800C7CC6 and 0x800C7CC8. The bitmask reading is confirmed at 0x80037E38,
 * a weapon pickup's have-test: `lw v1, 104(s0)` / `and v0, v1, a1`. */
static s16 g_player_weapon_id;
static u32 g_player_weapon_bits;

void q2_monster_set_player_weapon(s16 weapon_id, u32 weapon_bits)
{
    g_player_weapon_id   = weapon_id;
    g_player_weapon_bits = weapon_bits;
}

void q2_monster_set_drop_hook(void (*fn)(const q2_monster_drop_request *req,
                                         void *user),
                              void *user)
{
    g_drop_fn   = fn;
    g_drop_user = user;
}

void q2_monster_drop_reset(void)
{
    memset(g_drop_queue, 0, sizeof(g_drop_queue));

    /*
     * The static is NOT reset with the queue. 0x800AE714 is gp-relative
     * INITIALISED data inside the loaded image, not BSS — the text segment
     * runs 0x80018000..0x800B2800 — and the image stores 0 there (`bytes
     * 0x800AE710`: 44 00 00 00 | 00 00 00 00). The four gp-relative sites
     * `xrefs 0x800AE714` lists are the two store/load pairs named above, and
     * neither store writes 0: 0x80020798 writes 23, and 0x80020F24 (27, 23 or
     * 24) is in the picker nothing calls. (A bulk clear over the gp area would
     * not show up in xrefs; none is known.) Reproducing that means a
     * Tank Commander killed on one map leaves the 23 standing for the next —
     * which, since 23 is also the fallback, cannot be told apart from a fresh
     * zero. Named because it is a real difference in shape even where it is
     * not one in behaviour.
     */
}

u32 q2_monster_drop_pending(void)
{
    u32 i, n = 0;

    for (i = 0; i < Q2_MONSTER_DROP_SLOTS; i++)
        if (g_drop_queue[i].used)
            n++;

    return n;
}

void q2_monster_drop_record(const q2_monster *self)
{
    u32 i;

    if (!self)
        return;

    for (i = 0; i < Q2_MONSTER_DROP_SLOTS; i++) {
        if (g_drop_queue[i].used)
            continue;

        g_drop_queue[i].used      = true;              /* 0x80020DB0 */
        g_drop_queue[i].pop_class = self->pop_class_id; /* 0x80020DB4 */
        memcpy(g_drop_queue[i].pos, q2_cre_origin(self), sizeof(g_drop_queue[i].pos));
        return;
    }

    /* No free slot: 0x80020D74 and 0x80020DA4 both branch to the `jr ra` at
     * 0x80020E1C, so the fifth death in one frame drops nothing at all. */
}

void q2_monster_drop_flush(void)
{
    u32 i;

    /* 0x80020E24: slot 0 to slot 3, in address order, each used one handed to
     * 0x80020680 at 0x80020E5C. */
    for (i = 0; i < Q2_MONSTER_DROP_SLOTS; i++) {
        q2_monster_drop_request req;
        s8 item;

        if (!g_drop_queue[i].used)
            continue;

        g_drop_queue[i].used = false;       /* 0x800206B4, before the pick */

        item = q2_monster_drop_item_for_class(g_drop_queue[i].pop_class,
                                              g_player_weapon_id,
                                              g_player_weapon_bits);

        /*
         * 0x800207D0 `beq s0, zero, 0x80020848` — a zero id spawns nothing, and
         * it jumps straight to the epilogue, PAST the rand() at 0x80020820. So
         * a flagged Parasite consumes no random number, and the stream every
         * later draw sees depends on which creatures died, not merely on how
         * many.
         */
        if (item == 0)
            continue;

        memset(&req, 0, sizeof(req));
        req.item_id = (u8)item;

        /*
         * 0x80020820 `jal 0x80089E28` — BIOS rand() — and 0x8002082C `andi a1,
         * v0, 0xFFF`. The creature layer draws its BIOS rand() from the C
         * library's, as aimove.c and every cre_*.c do, so the one stream the
         * console shares between them is at least one stream here.
         */
        req.heading = (u16)(q2_random_next() & 0xFFF);

        req.pos[0] = g_drop_queue[i].pos[0];
        req.pos[1] = g_drop_queue[i].pos[1];
        req.pos[2] = g_drop_queue[i].pos[2];

        if (g_drop_fn)
            g_drop_fn(&req, g_drop_user);
    }
}

/* ------------------------------------------------------------------------- */
/* monster_death_use — 0x800622E8                                             */
/* ------------------------------------------------------------------------- */
/*
 * Twenty-six instructions, and two of them change how a body behaves.
 *
 *   `flags &= 0xFFFC`      (0x800622F8)  clears FL_FLY and FL_SWIM together, so
 *                                        a dead flyer stops flying and falls.
 *   `aiflags &= 0x100`     (0x80062308)  clears EVERY ai flag but AI_GOOD_GUY —
 *                                        stand-ground, ducked, hold-frame, the
 *                                        three pursuit bits, all of it. A body
 *                                        that died mid-duck is not still ducked.
 *
 * AND THE THIRD THING IS THE DROP, which used to be left as an address here.
 * 0x80062328 `lhu v0, 242(a0)` / 0x80062330 `andi v0, v0, 0x100` / 0x8006233C
 * `jal 0x80020D60` — a flagged creature records itself in the four-slot queue
 * above, and the once-a-frame flush turns that into an item. The block in
 * monster.h over q2_monster_drop_item_for_class has the whole chain. It does
 * the job of id's `Drop_Item` by a different route, and the route matters,
 * because it is indexed on the class row rather than on a per-entity `item`.
 *
 * The one thing it does that is still NOT modelled, named rather than quietly
 * dropped: entity+0xD4 is cleared at 0x80062314 when it is non-zero. In id that
 * field is `self->item`. The drop path does not read it — 0x80020D60 takes the
 * LINK OBJECT (a0 after 0x80062318) and reads its +0x0C, +0xD2 and
 * +0x54..+0xA3, nothing on the edict — so there is no second drop route
 * hiding behind it.
 */
void q2_monster_death_use(q2_monster *self)
{
    if (!self)
        return;

    self->flags  = (u16)(self->flags & ~(unsigned)(Q2_FL_FLY | Q2_FL_SWIM));
    self->aiflags &= Q2_AI_GOOD_GUY;

    if (self->spawnflags & Q2_SPAWNFLAG_DROP_ITEM)
        q2_monster_drop_record(self);
}

/* ------------------------------------------------------------------------- */
/* The hit cylinder — GetCreHitSize, MODELS.C 0x8006DC1C                       */
/* ------------------------------------------------------------------------- */
/*
 * Two facts and a fallback, which is all the function is:
 *
 *     8006DC1C  lw   v0, 16(a1)        the creature's bound model
 *     8006DC24  beq  v0, zero, ...     none: take the literals
 *     8006DC34  lhu  v0, 24(v0)        model header +0x18 -> out[0]
 *     8006DC48  lhu  v0, 26(v0)        model header +0x1A -> out[1]
 *     8006DC6C  addiu v0, zero, 286    no model: out[0] = 286
 *     8006DC74  addiu v0, zero, 600                out[1] = 600
 *
 * The halfwords are read UNSIGNED and then stored back into signed halfwords
 * at obj+0x40/+0x42 (0x80051038/0x80051044) and read back signed by
 * LineIntersectCreCylinder (0x8005474C, 0x80054834), so the round trip is an
 * s16 and the port keeps them as s16. Measured on this disc they are extents:
 * the Soldier's are 124 and 607, the Tank Commander's 327 and 918,
 * Jorg's 590 and 1749.
 */
void q2_monster_bind_hit_size(q2_monster *m, bool has_model,
                              s16 ext0, s16 ext1)
{
    if (!m)
        return;

    m->live_hit_radius = has_model ? ext0 : 286;
    m->live_hit_height = has_model ? ext1 : 600;

    if (has_model) {
        m->hit_radius = ext0;
        m->hit_height = ext1;
        return;
    }

    m->hit_radius = 286;
    m->hit_height = 600;
}

/* ------------------------------------------------------------------------- */
/* Corpses — 0x8007F098 (detach), 0x8007F77C (volume), 0x8007F71C (handler)   */
/* ------------------------------------------------------------------------- */
/*
 * The rescale's two constants, kept as the divides the console writes rather
 * than as a single scale factor, because they are not the same divide: the
 * height is `>> 2` with the +3 bias an arithmetic shift needs on a negative,
 * and the width is `(3 * w) / 2`.
 */
static s16 corpse_shorter(s16 v)
{
    s32 t = v;
    if (t < 0)
        t += 3;                 /* 0x8007F78C */
    return (s16)(t >> 2);       /* 0x8007F794 */
}

static s16 corpse_wider(s16 v)
{
    s32 t = (s32)v * 3;         /* 0x8007F79C..0x8007F7A0 */
    t = (s16)t;                 /* the console truncates to a halfword here */
    return (s16)(t / 2);        /* 0x8007F7AC..0x8007F7B4, toward zero */
}

void q2_monster_corpse_detach(q2_monster *m)
{
    if (!m || m->corpse)
        return;

    m->corpse = true;
    m->view_height = 0; /* Medic's native corpse proxy has no eye offset. */

    /*
     * The class becomes 47 and the old one is kept — 0x8007F0F0..0x8007F100.
     * Keeping it is not decoration: it is how anything downstream can still
     * say what the body used to be, and the console keeps it for the same
     * reason.
     */
    m->corpse_was_class = m->class_id;
    m->class_id         = Q2_CLASS_CORPSE;

    /*
     * The volume, half again as wide and a quarter as tall — AND IT IS THE HIT
     * CYLINDER, not the movement hull.
     *
     * QuakeDeadHitCollision (0x8007F77C, QENTS.C 1810) is handed the collision
     * object at 0x8007F0E0 with `a0 = s1 + 84`, cre+0x54, and touches exactly
     * two halfwords of it: 0x8007F780 `lh v0, 66(a0)` and 0x8007F798
     * `sh v0, 66(a0)` for obj+0x42, and 0x8007F790 / 0x8007F7BC for obj+0x40.
     * Those are entity+0x96 and entity+0x94, `hit_height` and `hit_radius`.
     * obj+0x18..0x22 — the ±286 movement hull — is not written anywhere in the
     * function, so a corpse keeps the box it walked around in.
     *
     * This loop used to run over `mins`/`maxs` instead, because the cylinder
     * was derived from them. That made a body 429 wide and 143 tall whatever
     * it had been, and it shrank a hull the console leaves alone. Both halves
     * are now where the console puts them.
     */
    m->hit_height = corpse_shorter(m->hit_height);
    m->hit_radius = corpse_wider(m->hit_radius);

    /*
     * The console frees the edict here (`entity+0x1C &= 0xDFFFFFFF`) and nulls
     * both cross-links. This port keeps one structure for both halves, so what
     * it can reproduce is the CONSEQUENCE: nothing about the body is a creature
     * any more. The AI callbacks go, the enemy link goes, and the think stops
     * — the corpse is driven by `q2_monster_corpse_tick` from here on.
     *
     * `in_use` deliberately stays set. On the console the actor outlives the
     * edict and is still drawn; dropping it here would make the body vanish,
     * which is the bug this whole path exists to prevent.
     */
    m->think       = NULL;
    m->stand = m->idle = m->search = m->walk = m->run = NULL;
    m->attack = m->melee = NULL;
    m->dodge       = NULL;
    m->sight       = NULL;
    m->checkattack = NULL;
    m->bigturn     = NULL;
    m->enemy       = NULL;
    m->oldenemy    = NULL;
    m->goalentity  = NULL;
    m->movetarget  = NULL;
    m->svflags    &= ~(u32)Q2_SVF_MONSTER;
}

/* ------------------------------------------------------------------------- */
/* The dissolve gate 0x8005B2A8 and its two handlers (monster.h)              */
/* ------------------------------------------------------------------------- */
q2_corpse_dissolve q2_corpse_dissolve_pick(const u8 *effect)
{
    if (!effect)
        return Q2_CORPSE_DISSOLVE_NONE;

    /* 0x8005B2B4 `lbu v0, 754(a0)` comes first, so effect[2] wins when both
     * are set; only on a zero does 0x8005B2D4 `lbu v0, 752(a0)` look at
     * effect[0]. A zero there branches to 0x8005B2F8 past both stores. */
    if (effect[2])
        return Q2_CORPSE_DISSOLVE_FX2;     /* 0x8005B2D0: 0x8005B444 */
    if (effect[0])
        return Q2_CORPSE_DISSOLVE_FX0;     /* 0x8005B2E8: 0x8005B39C */
    return Q2_CORPSE_DISSOLVE_NONE;
}

s16 q2_corpse_dissolve_drain(s16 level, s32 dt)
{
    /*
     * 0x8005B404 `lw v1, [0x800B2DB4]`, 0x8005B410 `sll v1, v1, 6`,
     * 0x8005B40C `lhu v0, 244(s1)`, 0x8005B414 `subu`, 0x8005B418 `sh`: a
     * halfword minus the whole shifted dt, kept to sixteen bits. The shift is
     * done on the unsigned word, as `sll` does it.
     */
    return (s16)(u16)((u32)(u16)level -
                      ((u32)dt << Q2_CORPSE_DISSOLVE_SHIFT));
}

/* The record's version of 0x8006D280, shared by both ways a corpse ends. */
static void corpse_free(q2_monster *m)
{
    m->in_use     = false;
    m->takedamage = Q2_DAMAGE_NO;
}

bool q2_monster_corpse_tick(q2_monster *m)
{ return q2_monster_corpse_tick_dt(m, Q2_AI_TICK_DT); }

bool q2_monster_corpse_tick_dt(q2_monster *m, s32 dt)
{
    q2_corpse_dissolve arm;

    /* A freed record is not ticked: 0x8006D280 has pushed the slot back onto
     * the free stack at 0x800B2BAC and zeroed its +0xF4 (0x8006D2D8). */
    if (!m || !m->corpse || !m->in_use)
        return false;

    /*
     * THE DISSOLVE HANDLER, 0x8005B39C or 0x8005B444, once the gate below has
     * put it in +0x3C. Its presentation chain is simcombat.c's pass here
     * (monster.h), so what is left is the drain and the free. It makes no
     * actor-list append and no gib test, so there is no gib test here either,
     * and no frame driver: nothing in the handler advances one.
     *
     * The live client supplies its 300 Hz frame delta. The AI-only wrapper
     * supplies one AI interval for callers which still tick whole frames.
     */
    if (m->dissolve_arm != Q2_CORPSE_DISSOLVE_NONE) {
        m->dissolve = q2_corpse_dissolve_drain(m->dissolve, dt);
        if (m->dissolve > 0)                /* 0x8005B420 `bgtz` */
            return false;
        corpse_free(m);                     /* 0x8005B428 -> 0x8006D280 */
        return true;
    }

    /*
     * THE GATE, 0x8007F728 `jal 0x8005B2A8`, before anything else — and when
     * it fires, 0x8007F730 returns: no chain, no append, no gib test on that
     * tick. So a body at or past gib_health whose effect[0] or [2] is set
     * dissolves and is never thrown.
     *
     * What it leaves behind, as this record has it: the handler in +0x3C,
     * the level in +0xF4, and a body no sweep can find from the next frame,
     * because the handler never calls 0x800552B4 to put it back on the actor
     * list. The port's sweeps skip on `takedamage`, not on list membership
     * (combat.c, nearest_hit), and q2_actor_from_monster carries it across.
     * 0x8007F288 on the +0x2EC record has nothing to act on: a creature
     * corpse's link was nulled at the detach (monster.h).
     */
    arm = q2_corpse_dissolve_pick(m->effect);
    if (arm != Q2_CORPSE_DISSOLVE_NONE) {
        m->dissolve_arm = (u8)arm;                     /* 0x8005B2EC */
        m->dissolve     = Q2_CORPSE_DISSOLVE_LEVEL;    /* 0x8005B2F4 */
        m->takedamage   = Q2_DAMAGE_NO;
        return false;
    }

    /*
     * The handler's other three calls advance no animation: 0x8007F738 is the
     * presentation chain (simcombat.c's pass here) and 0x8007F740 appends the
     * body to the actor list, which this port's fixed target list stands for.
     * The frame driver below is the port's own, kept from before, so that a
     * corpse whose move has not run out still plays it. Where the console
     * advances a detached body's frame, if anywhere, is not traced here.
     */
    if (m->currentmove)
        q2_M_MoveFrame(m);

    /*
     * AND A SETTLED BODY IS STILL CHECKED FOR GIBBING, every tick, against the
     * threshold copied at detach — `lh v1, 264(s0)` / `lw v0, 68(s0)` /
     * `slt v0, v0, v1` / `bne` at 0x8007F748..0x8007F758, falling through to
     * the destruction dispatcher at 0x8007F764. `gib_health < health` keeps
     * the body, so the boundary is inclusive.
     *
     * AND THE DISPATCHER RUNS — which is where every gib on the disc comes
     * from (modelent.h). It is no longer gated on `gibbed`: each module's gib
     * arm raises that flag itself when the killing hit lands (cre_soldier.c,
     * cre_arachner.c) and throws nothing, so a creature killed straight past
     * gib_health reached here already "gibbed" and never came apart. What
     * makes 0x8007F764 fire once on the console is the dispatcher's own tail,
     * 0x8007D140 `jal 0x8006D280(self)`: the actor is FREED, so nothing ticks
     * it, draws it or can hit it again. `in_use` and `takedamage` are this
     * record's version of that free.
     */
    if (m->health <= m->gib_health && m->in_use) {
        m->gibbed = true;
        q2_gib_monster_corpse(m);           /* 0x8007F764 -> 0x8007CEB4 */
        corpse_free(m);                     /* 0x8007D140 -> 0x8006D280 */
        return true;
    }

    return false;
}

/* ------------------------------------------------------------------------- */
/* The tail of T_Damage — 0x80062940..0x80062B54                              */
/* ------------------------------------------------------------------------- */
/*
 * A corpse's health floors here rather than in the arithmetic: 0x800629B4
 * clamps to -9999 AFTER the subtraction, so a rocket into a body that is
 * already down cannot drive it arbitrarily negative and out of gib range.
 *
 * ONE COPY of that figure. This file used to spell it out a second time under
 * its own name, so the two clamps in the port — this one and combat.c's — could
 * drift apart while both looked cited.
 */
#define Q2_MONSTER_HEALTH_FLOOR Q2_HEALTH_FLOOR

/* Nightmare skill: five seconds on the 10 Hz clock, 0x80062B20. */
#define Q2_PAIN_DEBOUNCE_SKILL3 Q2_AI_SECONDS(5)

void q2_monster_damage_reaction(q2_monster *targ, q2_monster *attacker,
                                s16 damage)
{
    bool was_dead;

    if (!targ)
        return;

    /* ---------------------------------------------------------------- */
    /* Still standing — the pain path, 0x80062AAC                        */
    /* ---------------------------------------------------------------- */
    if (targ->health > 0) {
        if (targ->svflags & Q2_SVF_MONSTER) {
            /*
             * REACT FIRST, FLINCH SECOND, and the order matters: the creature
             * has already turned on its attacker by the time its own pain
             * handler runs, so a Soldier shot in the back is facing you before
             * it picks a flinch animation.
             */
            q2_m_react_to_damage(targ, attacker);

            /* A creature part-way through a duck absorbs the hit without
             * flinching at all. 0x80062AD0. */
            if (targ->aiflags & Q2_AI_DUCKED)
                return;

            if (damage == 0)
                return;

            if (targ->pain)
                targ->pain(targ, damage);

            /*
             * NIGHTMARE MONSTERS DO NOT GO INTO PAIN FRAMES OFTEN — the pain
             * handler has just armed its own three-second debounce, and this
             * overwrites it with five. 0x80062B10 tests the same global the
             * skill is read from everywhere else.
             */
            if (q2_cre_skill() == 3)
                targ->pain_debounce =
                    q2_level_state.time + Q2_PAIN_DEBOUNCE_SKILL3;
            return;
        }

        /* Anything that is not a creature — a player, a breakable — gets the
         * flinch and none of the AI. 0x80062B2C. */
        if (damage != 0 && targ->pain)
            targ->pain(targ, damage);
        return;
    }

    /* ---------------------------------------------------------------- */
    /* Killed — 0x80062978 onward                                        */
    /* ---------------------------------------------------------------- */
    /*
     * `was_dead` is the console's `deadflag == DEAD_DEAD` (entity+0x20 bits
     * 22..23), which the creature's own `die` raises. `deadflag` is now a real
     * field, because the modules write it and the transcriptions read it — but
     * the guard here stays on `dead`, and the two are kept in step at the
     * bottom rather than being allowed to drift. Two fields for one fact is
     * how a body ends up half dead.
     */
    was_dead = targ->dead || targ->deadflag == Q2_DEAD_DEAD;

    /* A body takes no more knockback. 0x80062994. */
    if ((targ->svflags & Q2_SVF_MONSTER) || targ->client)
        targ->flags |= Q2_FL_NO_KNOCKBACK;

    if (targ->health < Q2_MONSTER_HEALTH_FLOOR)
        targ->health = (s16)Q2_MONSTER_HEALTH_FLOOR;

    if (targ->svflags & Q2_SVF_MONSTER) {
        /* Even in death it blames whoever did it — this is what `ai_run`'s
         * corpse handling and the trail both read afterwards. 0x800629D4. */
        targ->enemy = attacker;

        if (!was_dead && !(targ->aiflags & Q2_AI_GOOD_GUY)) {
            q2_level_state.killed_monsters++;

            /* See q2_monster.owner: id's medic exclusion, unreachable on this
             * disc because no Medic module ships. 0x80062A18. */
            if (attacker && attacker->class_id == Q2_CLASS_MEDIC)
                targ->owner = attacker;
        }
    }

    /*
     * The movetype gate at 0x80062A2C — bits 18..21 of entity+0x20 — sends
     * MOVETYPE_NONE, _PUSH and _STOP straight to `die` without the death-use
     * pass. Those are doors, platforms and triggers; every creature on the disc
     * is MOVETYPE_STEP, so the arm this structure can reach is the other one.
     * Stated rather than implemented, because `q2_monster` models creatures and
     * inventing a movetype field for it would be a field nothing sets.
     */
    if (!was_dead && (targ->svflags & Q2_SVF_MONSTER))
        q2_monster_death_use(targ);

    /*
     * AND THE DIE CALL IS NOT GUARDED, which is the whole of "a corpse cannot
     * be gibbed".
     *
     * This used to read `if (!was_dead && targ->die)`. The console has NO such
     * guard — and not merely no dead test: it has no NULL test either. Once
     * 0x80062978 is entered, every branch between there and `jalr v0` at
     * 0x80062A9C converges on the call. The three tests along the way —
     * movetype in {0,2,3} at 0x80062A44/0x80062A4C, `!SVF_MONSTER` at
     * 0x80062A60, `deadflag == DEAD_DEAD` at 0x80062A70 — skip only
     * `touch = NULL` and `monster_death_use`, which is why those two keep
     * their `was_dead` guard above and this does not. Compare the PAIN call at
     * 0x80062B34, which IS null-guarded: the asymmetry is the original saying
     * that anything reaching here has a die handler.
     *
     * A guard here defeated the module's own gib arm, which is written to be
     * re-entered: the Soldier's `die` tests `health <= gib_health` at
     * module+0x2324 and its already-dead guard is twelve instructions further
     * on, INSIDE the other branch (module+0x237C). The same shape on the Gunner
     * (+0x16C8 vs +0x1738) and the Infantry (+0x16A0 vs +0x16F0). So a body
     * already down is meant to be destroyable by a later explosion, and this
     * port was returning before the test that decides it.
     *
     * The NULL check stays, because unlike the console this port can be built
     * with a creature whose module installs no die at all.
     */
    if (targ->die)
        targ->die(targ, damage);

    /* A creature whose module installs no `die` still stops — and the two
     * spellings of "dead" end the call agreeing, whichever of them the
     * module's own handler happened to set. */
    targ->dead     = true;
    targ->deadflag = Q2_DEAD_DEAD;
}

/* ------------------------------------------------------------------------- */
/* The class method table                                                     */
/* ------------------------------------------------------------------------- */
/*
 * 0x80061D10 fills 256 slots with one shared inert table and lets a module
 * overwrite individual methods through 0x80061DE4. The original can do that
 * because every class's table lives in one fixed arena; here the tables are
 * allocated on first write, which is invisible from the outside — an
 * unregistered class still answers NULL for every method, and the frame
 * driver's `if (fn)` guard is what makes that inert rather than fatal.
 */
static q2_class_method *g_class_methods[Q2_CLASS_COUNT];

void q2_class_table_reset(void)
{
    u32 i;
    for (i = 0; i < Q2_CLASS_COUNT; i++) {
        free(g_class_methods[i]);
        g_class_methods[i] = NULL;
    }
}

void q2_class_method_set(u32 class_id, u32 index, q2_class_method fn)
{
    if (class_id >= Q2_CLASS_COUNT || index >= Q2_CLASS_METHOD_COUNT)
        return;

    if (!g_class_methods[class_id]) {
        g_class_methods[class_id] =
            (q2_class_method *)calloc(Q2_CLASS_METHOD_COUNT,
                                      sizeof(q2_class_method));
        if (!g_class_methods[class_id])
            return;
    }

    g_class_methods[class_id][index] = fn;
}

q2_class_method q2_class_method_get(u32 class_id, u32 index)
{
    if (class_id >= Q2_CLASS_COUNT || index >= Q2_CLASS_METHOD_COUNT)
        return NULL;
    if (!g_class_methods[class_id])
        return NULL;
    return g_class_methods[class_id][index];
}

void q2_class_think_set(u32 class_id, u32 think_index, q2_class_method fn)
{
    if (think_index >= Q2_CLASS_VERB_BASE)
        return;
    q2_class_method_set(class_id, think_index, fn);
}

void q2_class_verb_set(u32 class_id, u32 verb_index, q2_class_method fn)
{
    q2_class_method_set(class_id, Q2_CLASS_VERB_BASE + verb_index, fn);
}

/* ------------------------------------------------------------------------- */
/* M_MoveFrame — 0x8006175C                                                   */
/* ------------------------------------------------------------------------- */
static void monster_move_frame(q2_monster *m)
{
    const q2_mmove *move;
    const q2_mframe *frame;
    s32 index;

    if (!m)
        return;

    move = m->currentmove;
    if (!move || !move->frames)
        return;

    /* The think re-arms itself every tick. Note this happens BEFORE anything
     * else, so a move that frees the entity still leaves a sane nextthink. */
    m->next_think = q2_level_state.framenum + 1;

    if (m->nextframe
        && (s32)m->nextframe >= move->first_frame
        && (s32)m->nextframe <= move->last_frame) {
        /* A think function asked for a specific frame; honour it and clear the
         * request rather than advancing. */
        m->frame     = (s16)m->nextframe;
        m->nextframe = 0;
    } else {
        if (m->frame == move->last_frame) {
            if (move->endfunc) {
                move->endfunc(m);

                /* The callback may have freed the entity — 0x800617F8 tests
                 * bit 1 of the svflags word and bails. */
                if (m->svflags & Q2_SVF_DEADMONSTER)
                    return;

                move = m->currentmove;
                if (!move || !move->frames)
                    return;
            } else if (m->dead) {
                /*
                 * A CORPSE STOPS ON ITS LAST FRAME.
                 *
                 * Every death move on the Soldier carries the endfunc
                 * module+0x20F0 — a three-store routine that zeroes entity+0x90,
                 * rewrites entity+0x20's bits 18-21 and ORs 2 into svflags,
                 * which is precisely the bit tested above. `resolve_endfunc`
                 * can only bind an address that is one of the module's 13
                 * callbacks or one of its think methods; 0x801020F0 is neither,
                 * and it installs no move either, so `chain_endfunc` does not
                 * apply. The endfunc resolved to NULL and the wrap below sent
                 * the body back to the move's first frame — a corpse replaying
                 * its own death for ever.
                 *
                 * Raising the same bit here reproduces what that routine does
                 * to this module's own bail-out, for the one case the port can
                 * identify without binding the address: the creature is dead
                 * and its move has run out.
                 */
                m->svflags |= Q2_SVF_DEADMONSTER;
                return;
            }
        }

        if (m->frame < move->first_frame || m->frame > move->last_frame) {
            /* Landed outside the move, which is what happens the tick after a
             * new move is installed. Snap to its first frame and drop the hold,
             * because a hold belongs to the move that set it. */
            m->aiflags &= ~(u32)Q2_AI_HOLD_FRAME;
            m->frame = (s16)move->first_frame;
        } else if (!(m->aiflags & Q2_AI_HOLD_FRAME)) {
            m->frame++;
            if (m->frame > move->last_frame)
                m->frame = (s16)move->first_frame;
        }
    }

    index = m->frame - move->first_frame;
    if (index < 0 || index > move->last_frame - move->first_frame)
        return;

    q2_ai_native_s2 = (u32)index; /* 0x8006188C */
    frame = &move->frames[index];

    if (frame->ai & Q2_AI_LOCAL_FLAG) {
        /* The creature's own verb. It gets no distance: a creature that steers
         * itself does not want the frame's. 0x800618B8. */
        q2_class_method fn =
            q2_cre_method_for(m,
                                Q2_CLASS_VERB_BASE + (frame->ai & 0x7Fu));
        if (fn)
            fn(m);
    } else if (frame->ai < 8) {
        q2_ai_verb_fn fn = q2_ai_verbs[frame->ai];
        if (fn)
            fn(m, q2_monster_frame_dist(m, frame));
    }

    if (frame->think) {
        q2_class_method fn = q2_cre_method_for(m, frame->think);
        if (fn)
            fn(m);
    }
}

void q2_M_MoveFrame(q2_monster *m)
{
    u32 saved = q2_ai_native_s2;
    q2_ai_native_s2 = 0; /* live handler 0x8007EE08, before the think call */
    monster_move_frame(m);
    q2_ai_native_s2 = saved;
}

/* ------------------------------------------------------------------------- */
/* monster_start_go — 0x80061BA4                                              */
/* ------------------------------------------------------------------------- */
/* Q2_PAUSE_FOREVER and Q2_CLASS_PATH_CORNER are in monster.h: path_corner_touch
 * needs both and it lives with the corners, not here. */

/* Resolved by targetname; the port hands the resolver in rather than owning a
 * script namespace. */
static q2_monster *(*g_pick_target)(s16 targetname, void *user);
static void *g_pick_target_user;

void q2_ai_set_pick_target(q2_monster *(*fn)(s16, void *), void *user)
{
    g_pick_target      = fn;
    g_pick_target_user = user;
}

q2_monster *q2_pick_target(s16 targetname)
{
    if (!g_pick_target || targetname <= 0)
        return NULL;
    return g_pick_target(targetname, g_pick_target_user);
}

/*
 * monster_start_go proper, 0x80061BA4 — the shared tail all three go-routines
 * fall into. Kept as its own body so the three can be reproduced above it in
 * the shape the disc has them.
 */
static void monster_start_go_body(q2_monster *m)
{
    if (!m || m->health <= 0)
        return;

    if (m->target > 0) {
        q2_monster *t = q2_pick_target(m->target);

        m->movetarget = t;
        m->goalentity = t;

        if (!t) {
            /* A target that does not resolve leaves the creature standing
             * where it was placed rather than walking to the world origin. */
            m->target    = 0;
            m->pausetime = Q2_PAUSE_FOREVER;
            if (m->stand)
                m->stand(m);
        } else if (t->class_id == Q2_CLASS_PATH_CORNER) {
            s32 v[3];
            q2_link_entity(m, 1);
            v[0] = t->pos[0] - m->pos[0];
            v[1] = t->pos[1] - m->pos[1];
            v[2] = t->pos[2] - m->pos[2];
            m->ideal_yaw  = q2_vectoyaw(v);
            m->angles[2]  = m->ideal_yaw;
            q2_link_entity(m, 4);
            if (m->walk)
                m->walk(m);
            m->target = 0;
        } else {
            /* Targeting something that is not a path corner is a level-design
             * mistake in the original too; it stands rather than pathing. */
            m->movetarget = NULL;
            m->goalentity = NULL;
            m->pausetime  = Q2_PAUSE_FOREVER;
            if (m->stand)
                m->stand(m);
        }
    } else {
        m->ideal_yaw = m->angles[2];
        q2_link_entity(m, 4);
        m->pausetime = Q2_PAUSE_FOREVER;
        if (m->stand)
            m->stand(m);
    }

    m->think      = q2_M_MoveFrame;
    m->next_think = q2_level_state.time + 1;
}

/* ------------------------------------------------------------------------- */
/* The three go-routines — see the block above the declarations in monster.h  */
/* ------------------------------------------------------------------------- */
/*
 * Two things all three do that are NOT reproduced here, named so nobody takes
 * these for complete transcriptions:
 *
 *  - flymonster_go opens with `jal 0x800607F8`, M_walkmove(self, 0, 0), at
 *    0x800624D0 — id's `if (!M_walkmove (self, 0, 0)) gi.dprintf ("in
 *    solid")`. A zero-length move leaves the origin where it is, so its only
 *    products are a relink and a trigger touch. INFERRED harmless, not read
 *    through: the port's step has no dprintf and the spawn drop already
 *    placed the creature.
 *  - each ends, after the jal to 0x80061BA4, with `if ((spawnflags >> 18) &
 *    2)` (0x8006245C..0x8006246C, and the same at 0x800624FC and 0x80062594):
 *    use = 0x800623DC, nextthink = 0, solid and movetype cleared, svflags |= 1
 *    — id's monster_triggered_start for a record carrying map flag 2. That is
 *    a different finding from the eye and the turn rate and it is left for it.
 */
void q2_monster_walk_start_go(q2_monster *m, s16 model_ext2)
{
    if (!m)
        return;

    /* 0x80062434 — conditional STORE, unconditional 228. 20.04 deg/tick. */
    if (m->yaw_speed == 0)
        m->yaw_speed = 228;

    /*
     * 0x80062450 `nor v0, zero, v0` on the halfword at obj+0xF8, stored back
     * through an `sh` at 0x80062458 — so the eye is ~ext2 truncated to 16 bits.
     * ext2 251 gives -252, not -251, and ext2 0 gives -1, not 0.
     */
    m->view_height = (s16)(u16)~(u16)model_ext2;

    monster_start_go_body(m);
}

void q2_monster_fly_start_go(q2_monster *m)
{
    if (!m)
        return;

    if (m->yaw_speed == 0)
        m->yaw_speed = 114;         /* 0x800624E8 — 10.02 deg/tick */

    m->view_height = -250;          /* 0x800624F0 — flat, no model read */

    monster_start_go_body(m);
}

void q2_monster_swim_start_go(q2_monster *m)
{
    if (!m)
        return;

    if (m->yaw_speed == 0)
        m->yaw_speed = 114;         /* 0x8006257C */

    /* -100 on BOTH paths: 0x80062578 loads it in the delay slot of the yaw
     * branch and 0x80062584 loads it again on the fall-through, and the single
     * store is at 0x80062590. */
    m->view_height = -100;

    monster_start_go_body(m);
}

/* ------------------------------------------------------------------------- */
/* The three start wrappers — see the block above the declarations            */
/* ------------------------------------------------------------------------- */
/*
 * THE FLAG IS THE WRAPPER'S, NOT THE GO-ROUTINE'S.
 *
 *   0x800622A8  lhu v1, 32(a0)          \  flymonster_start, 0x8006229C
 *   0x800622B0  sw 0x800624BC, 148(a0)   |  parks the fly go-routine
 *   0x800622B4  ori v1, v1, 0x1          |  FL_FLY
 *   0x800622BC  sh v1, 32(a0)           /   in the jal 0x800619E0's delay slot
 *
 * and 0x80062280 `ori 0x2` with think 0x8006255C for the swim one. The think
 * and the flag are written in the same four instructions, so they always
 * arrive together and they arrive at SPAWN — which is why the prone Insane is a
 * FLYER for its whole life and not merely a creature with a low eye: aimove.c
 * selects the non-stepping movement path off exactly this bit, and the place
 * that takes it away again is monster_death_use (0x800622F8 `andi 0xFFFC`).
 *
 * The walk wrapper writes the think and nothing else.
 *
 * ALL THREE THEN `jal 0x800619E0` (0x80062250, 0x80062284, 0x800622B8), and
 * monster_start's last act draws from the RNG — see monster_start_frame.
 */

/*
 * monster_start's START-FRAME PICK, 0x80061B1C..0x80061B8C — the one part of
 * monster_start that runs here, inside the wrapper, rather than in
 * q2_creature_spawn.
 *
 *   0x80061B1C  lw v0, 216(s0)              currentmove
 *   0x80061B24  beq v0, zero, 0x80061B90    no move: no draw, no frame write
 *   0x80061B2C  jal 0x80089E28              BIOS rand(), A0:2F
 *   0x80061B48  subu v1, v1, a0 / addiu 1   span = last - first + 1
 *   0x80061B50  div v0, v1  (mfhi)          rand() % span
 *   0x80061B7C  lhu v0, 0(a1)               the first frame's low halfword
 *   0x80061B8C  sh v0, 56(s0)               frame = first + remainder
 *
 * That is id's "randomize what frame they start on". 0x80089E28 is the same
 * routine a module's import +0x14 reaches (the loader stores it there at
 * 0x8007DA24/0x8007DA28), so this draw is taken from the one stream the
 * module's own random() draws come from, and it lands INSIDE the wrapper call:
 * after anything the module drew before calling it, before anything it draws
 * after. cre_insane.c's skin draw is the case that shows it.
 *
 * It reads what the module installed, which is why it cannot run in
 * q2_creature_spawn ahead of the module's spawn hook, where most of what this
 * port models of monster_start runs. The `sh` sits in the delay slot of `jal
 * 0x8007F474`, which seeds the physical actor's animation. The client resolves
 * that frame against the loaded model before the actor's first update.
 */
static void monster_start_frame(q2_monster *m)
{
    const q2_mmove *mv = m->currentmove;
    s32 r, span;

    if (!mv)
        return;

    r    = (s32)(q2_random_next() & 0x7FFF);
    span = mv->last_frame - mv->first_frame + 1;

    /*
     * A PORT GUARD: a zero divisor traps on the console (0x80061B5C `break
     * 0x1C00`). The decoder refuses a move with last < first, so a real one
     * never reaches it; the draw has been taken either way, as it has on the
     * console by then. C's `%` truncates toward zero as MIPS `div` does, and r
     * is never negative, so every other span gives the console's remainder.
     */
    if (span == 0)
        return;

    m->frame = (s16)(u16)((u32)(u16)mv->first_frame + (u32)(r % span));
    m->motion.seed_frame = m->frame;
    m->motion.seed_pending = true;
}

void q2_monster_walk_start(q2_monster *m)
{
    if (!m)
        return;
    m->start_kind = Q2_CRE_START_WALK;              /* 0x80062254 */
    monster_start_frame(m);                         /* 0x80062250 -> 0x80061B2C */
}

void q2_monster_fly_start(q2_monster *m)
{
    if (!m)
        return;
    m->start_kind = Q2_CRE_START_FLY;               /* 0x800622B0 */
    m->flags      = (u16)(m->flags | Q2_FL_FLY);    /* 0x800622B4 */
    monster_start_frame(m);                         /* 0x800622B8 -> 0x80061B2C */
}

void q2_monster_swim_start(q2_monster *m)
{
    if (!m)
        return;
    m->start_kind = Q2_CRE_START_SWIM;              /* 0x8006227C */
    m->flags      = (u16)(m->flags | Q2_FL_SWIM);   /* 0x80062280 */
    monster_start_frame(m);                         /* 0x80062284 -> 0x80061B2C */
}

void q2_monster_start_go(q2_monster *m)
{
    if (!m)
        return;

    if (!m->spawn_initialized && m->cre_bind) {
        u32 variant = 0;
        const q2_creature *cre = m->cre_bind->cre;
        while (variant + 1 < cre->class_count && cre->class_byte[variant] != m->class_byte)
            variant++;
        q2_creature_spawn(m->cre_bind, m, variant);
    }

    /*
     * Whichever go-routine the wrapper parked at entity+0x94. None of the
     * three writes the flags word — that was the wrapper's, at spawn.
     */
    switch (m->start_kind) {
    case Q2_CRE_START_SCRIPT:
        return; /* The module already installed its timer and direct think. */
    case Q2_CRE_START_FLY:
        q2_monster_fly_start_go(m);
        return;
    case Q2_CRE_START_SWIM:
        q2_monster_swim_start_go(m);
        return;
    default:
        break;
    }

    /*
     * A PORT GUARD, NOT THE DISC'S: with `model_ext2` still zero the console's
     * arithmetic would put the eye at -1, one unit below the origin, and a
     * creature whose model nobody resolved would go blind. The disc always has
     * the halfword because the loader writes it at spawn (0x80056710); this
     * port fills it from `q2_model_ent_height` only where a model bank is in
     * hand. So an unfilled creature keeps the -290 stand-in from
     * q2_monster_init and only the yaw_speed half applies.
     *
     * INFERRED that no creature model on the disc genuinely has ext2 0 — the
     * five measured are 251, 507, 217, 380 and 304, and the only 0 found with
     * `q2psx-inspect models` was on an ITEM (Adrenal P on BASE0), which never
     * reaches a go-routine.
     */
    if (m->model_ext2 == 0) {
        if (m->yaw_speed == 0)
            m->yaw_speed = 228;
        monster_start_go_body(m);
        return;
    }

    q2_monster_walk_start_go(m, m->model_ext2);
}

/* ------------------------------------------------------------------------- */
bool q2_mmove_read(const u8 *image, size_t size, u32 offset, q2_mmove *out)
{
    if (!image || !out)
        return false;
    if ((size_t)offset + Q2_MMOVE_SIZE > size)
        return false;

    memset(out, 0, sizeof(*out));

    out->image_offset   = offset;
    out->first_frame    = q2_rd_s32(image + offset + 0);
    out->last_frame     = q2_rd_s32(image + offset + 4);
    out->frames_offset  = q2_rd_u32(image + offset + 8);
    out->endfunc_offset = q2_rd_u32(image + offset + 12);

    /* A move must cover at least one frame and run forwards. */
    if (out->last_frame < out->first_frame)
        return false;

    return true;
}

bool q2_mframe_read(const u8 *image, size_t size, u32 offset, q2_mframe *out)
{
    if (!image || !out)
        return false;

    /* Three bytes, unpadded. Stepping by four walks off the end of every
     * animation on the disc. */
    if ((size_t)offset + Q2_MFRAME_SIZE > size)
        return false;

    out->ai    = image[offset + 0];
    out->dist  = (s8)image[offset + 1];
    out->think = image[offset + 2];

    return true;
}
