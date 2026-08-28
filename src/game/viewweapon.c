#include "viewweapon.h"

#include "trig.h"

#include <stdlib.h>
#include <string.h>

/* 1.0.12 one — the hyperblaster's barrel at rest, 0x8004FCD8. */
#define VW_HYPER_RAMP_ONE  4096

/* The part 0x8004FED0 hands 0x8006D43C, in its `a2`. */
#define VW_HYPER_BARREL_PART 6

/* ------------------------------------------------------------------------- */
/* Keys                                                                       */
/* ------------------------------------------------------------------------- */
static const q2_vm_key *current_key(const q2_viewweapon *vw)
{
    const q2_vm_clip *c = q2_vm_clip_get(vw->tab, vw->weapon, vw->state);

    if (!c || vw->frame >= c->count)
        return NULL;
    return &c->key[vw->frame];
}

static u32 clip_length(const q2_viewweapon *vw, q2_vm_state state)
{
    const q2_vm_clip *c = q2_vm_clip_get(vw->tab, vw->weapon, state);
    return c ? c->count : 0;
}

/*
 * 0x8004F1F8. A key with a non-zero jitter takes `((rand() * duration) >> 15 +
 * 1) * 10`; every other key takes its duration as written. The multiply by ten
 * is spelled out in the original as a shift-add chain, and it is why a jittered
 * key is an order of magnitude longer than its stored value rather than a small
 * perturbation of it.
 */
static s32 key_duration(const q2_vm_key *k)
{
    if (!k)
        return 0;
    if (k->jitter == 0)
        return k->duration;

    {
        s32 r = (s32)(rand() & 0x7FFF);
        s32 n = ((r * (s32)k->duration) >> 15) + 1;
        return n * 10;
    }
}

/* Latch the interpolation's starting point and load the new key's clock. */
static void begin_key(q2_viewweapon *vw)
{
    const q2_vm_key *k = current_key(vw);
    int i;

    for (i = 0; i < 3; i++) {
        vw->from_t[i] = vw->cur_t[i];
        vw->from_r[i] = vw->cur_r[i];
    }

    vw->total = key_duration(k);
    vw->left  = vw->total;

    if (k && k->event != Q2_VM_EVENT_NONE) {
        vw->event = k->event;
        vw->event_pending = true;
    }
}

/*
 * `v = from + (key - from) * (total - left) / total`, which is 0x8004EF80 and
 * its five siblings. A zero-duration key is legal — the original's divide would
 * trap, so the engine never reaches it with total == 0, and here it simply
 * snaps.
 */
static s16 lerp_field(s16 from, s16 to, s32 total, s32 left)
{
    s32 elapsed;

    if (total <= 0)
        return to;

    elapsed = total - left;
    if (elapsed <= 0)
        return from;
    if (elapsed >= total)
        return to;

    return (s16)((s32)from + ((s32)to - (s32)from) * elapsed / total);
}

static void interpolate(q2_viewweapon *vw)
{
    const q2_vm_key *k = current_key(vw);
    int i;

    if (!k)
        return;

    for (i = 0; i < 3; i++) {
        vw->cur_t[i] = lerp_field(vw->from_t[i], k->t[i], vw->total, vw->left);
        vw->cur_r[i] = lerp_field(vw->from_r[i], k->r[i], vw->total, vw->left);
    }
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */
void q2_vw_init(q2_viewweapon *vw, const q2_vm_tables *tab, int weapon)
{
    if (!vw)
        return;

    memset(vw, 0, sizeof(*vw));
    /* The entity allocator at 0x8006C1B8..0x8006C1FC establishes these before
     * the view-model constructor writes its positive +0xF4 at 0x8004F750. */
    vw->scale      = Q2_ONE_12;
    vw->fade       = Q2_ONE_12;
    vw->light_selector = 1;
    vw->glow[0]    = 0x40;
    vw->glow[1]    = 0x40;
    vw->glow[2]    = 0x40;
    vw->frame_sound = -1;
    vw->anim_end    = -1;
    vw->hyper_ramp  = VW_HYPER_RAMP_ONE;
    vw->tab     = tab;
    vw->weapon  = (weapon >= 0 && weapon < Q2_VM_SLOTS) ? weapon : 0;
    vw->pending = vw->weapon;

    /*
     * A level start begins in LOWER, not RAISE — 0x8004F7A4, `sw 3, 72(s2)`,
     * with frame 0 (`sw zero, 76`), total 1 (`sw 1, 48`), left 0
     * (`sw zero, 80`) and the dry latch cleared (`sh zero, 216`).
     *
     * The note here used to cite 0x8004FA48, which is the state the machine
     * lands in AFTER A SWAP — a different moment. The difference is visible:
     * a one-tick LOWER completes immediately and goes through the swap arm,
     * which is what resolves the model, so the console shows no weapon at all
     * for the first frames of a level and then raises it. Starting in RAISE
     * skips that and has the gun present from frame zero.
     */
    vw->state = Q2_VM_LOWER;
    vw->frame = 0;
    begin_key(vw);
    vw->total = 1;
    vw->left  = 0;
    interpolate(vw);
}

void q2_vw_select(q2_viewweapon *vw, int weapon)
{
    if (!vw || weapon < 0 || weapon >= Q2_VM_SLOTS)
        return;
    vw->pending = weapon;

    /*
     * 0x8004ECEC. Asking for a weapon arms the 70-tick countdown; LOWER holds
     * at the bottom of its arc until it expires, which is what stops a fast
     * double-tap from swapping the model twice in as many frames.
     */
    vw->switch_ticks = Q2_VW_SWITCH_TICKS;
}

void q2_vw_set_model(q2_viewweapon *vw, const q2_model *model)
{
    if (vw)
        vw->model = model;
}

const char *q2_vw_model_name(const q2_viewweapon *vw)
{
    if (!vw || !vw->tab)
        return NULL;
    return vw->tab->model_name[vw->weapon];
}

bool q2_vw_take_refire(q2_viewweapon *vw)
{
    bool r;

    if (!vw)
        return false;
    r = vw->refire;
    vw->refire = false;
    return r;
}

bool q2_vw_take_event(q2_viewweapon *vw, s16 *event_out)
{
    if (!vw || !vw->event_pending)
        return false;
    if (event_out)
        *event_out = vw->event;
    vw->event_pending = false;
    return true;
}

bool q2_vw_wants_fire(const q2_viewweapon *vw)
{
    return vw && vw->state == Q2_VM_IDLE && !vw->fire_latch;
}

/* ------------------------------------------------------------------------- */
/* The state machine — 0x8004F87C, called once per key boundary               */
/* ------------------------------------------------------------------------- */
/*
 * The fire check the idle state runs — 0x8004FB04…0x8004FB98.
 *
 * The order here is the original's and it is not the obvious one. The latch is
 * tested BEFORE the fire function is consulted, and a set latch does not fire:
 * it clears, and that clearing is the same pass that recomputes the player's
 * next and previous weapons. So one shot per press is not enforced by a timer,
 * it falls out of a latch that costs one pass to release. A port that clears
 * the latch when the fire clip ends instead fires a frame early, every time.
 */
static void q2_vw_idle_fire_check(q2_viewweapon *vw, bool fire_held,
                                  q2_vw_fire_result fired)
{
    if (!fire_held && !vw->fire_latch)
        return;                     /* 0x8004FB18: nothing to do */

    if (vw->fire_latch) {
        /*
         * 0x8004FB5C, and `fire_latch` IS THE DRY LATCH — viewmodel+216.
         *
         * This used to be set on every successful shot, on the reading that
         * "one shot per press falls out of a latch that costs one pass to
         * release". `access 216` says otherwise: inside the state machine
         * (0x8004F87C) the halfword is only ever CLEARED — `sh zero` at
         * 0x8004F968 and 0x8004FB64 — and the only two non-zero writes in the
         * whole image are 0x80050230 and 0x800502FC, the chaingun and
         * hyperblaster arms, both immediately after `beq result, 2`, i.e.
         * after a fire function reported it could not fire.
         *
         * What setting it on a normal shot cost: this arm raises `refire`, the
         * caller turns `refire` into the auto-switch pass (0x800506C4), and a
         * selection change plays a full LOWER then RAISE. So the gun dipped out
         * of view and swung back in after EVERY shot. Measured on BASE1, 120
         * frames, --weapon 1 --shoot: ten model re-binds against one for the
         * same run not firing.
         */
        vw->fire_latch = false;
        vw->refire     = true;
        return;
    }

    if (fired == Q2_VW_FIRE_DENIED) {
        /*
         * Running dry: no clip, the neighbours are recomputed, and THIS is
         * where the latch is set — 0x80050230 / 0x800502FC store it after a
         * fire function returns 2. It costs the next pass, so a held trigger
         * on an empty gun asks once and then waits rather than hammering the
         * auto-switch at tick rate.
         */
        vw->fire_latch = true;
        vw->refire     = true;
        return;
    }

    /*
     * And the port's own third case: the trigger is down, the weapon is fed,
     * and no shot happened because the refire gate had not expired. The
     * original cannot reach this — its idle state calls the fire function
     * rather than being told about it afterwards — so the machine simply stays
     * idle. Without this the fire clip restarted on every held-trigger frame,
     * at RENDER rate rather than tick rate.
     */
    if (fired == Q2_VW_FIRE_NONE)
        return;

    /* A successful shot does NOT set the latch — see above. What limits the
     * rate is the clip: this arm only runs from the IDLE state, so the next
     * shot waits for the fire animation to finish, which is exactly what
     * weapon.h means by "one gate plus animation length". */
    /* 0x8004FB9C sits between the successful shot and `state = FIRE`, so the
     * quad's sound belongs to the shot rather than to the clip. */
    if (vw->quad_active)
        vw->quad_sound = true;

    vw->state           = Q2_VM_FIRE;
    vw->frame           = 0;
    vw->frame_sound     = -1;
    vw->spin_accum      = 0;
    vw->spin_rate       = 1;
    vw->last_fire_frame = -1;
    vw->cook            = false;
    vw->hand_prev_anim  = vw->anim_pos;
    vw->hand_cook_ticks = 0;
    vw->hand_release    = false;
    vw->fires_started++;
    begin_key(vw);
}

/* ------------------------------------------------------------------------- */
/* The per-weapon FIRE-state driver — 0x8004FEE8                              */
/* ------------------------------------------------------------------------- */
/*
 * Called from inside the key loop on every substep (0x8004EF4C), gated on
 * `state == FIRE` (0x8004FF38), then a switch on the weapon id. Only four
 * weapons have an arm; every other weapon's fire clip simply plays out.
 *
 * The shots these arms take are not the idle check's. On the console each arm
 * `jalr`s the weapon's fire function directly, which is how a chaingun fires
 * once per animation frame for as long as its loop runs. Here they raise
 * `frame_fires` and the caller drains it.
 */
#define VW_MACHINEGUN    4
#define VW_CHAINGUN      5
#define VW_HAND_GRENADE  6
#define VW_HYPERBLASTER  9

/* +52 accumulates the step and fires each time it passes 30 (0x8004FFB8). */
#define VW_SPIN_THRESHOLD 30

static void fire_state_step(q2_viewweapon *vw, s32 step, bool fire_held)
{
    if (vw->state != Q2_VM_FIRE)
        return;

    switch (vw->weapon) {
    case VW_MACHINEGUN:
        /*
         * 0x8004FF84. Held: frame 2 wraps back to 0 so the three-key cycle
         * repeats, and the accumulator fires once each time it passes 30.
         *
         * The accumulator is RESET, not decremented (0x80050038's
         * `sw zero, 52`), so a long substep cannot bank a burst — which is
         * what taking `-= 30` in a loop did here, turning one host frame at a
         * low frame rate into several rounds.
         */
        if (fire_held) {
            if (vw->frame == 2)
                vw->frame = 0;

            vw->spin_accum += step;
            if (vw->spin_accum >= VW_SPIN_THRESHOLD) {
                /* 0x8004FFD0 skips the shot on the last key and leaves the
                 * accumulator standing, so the next substep asks again. */
                if (vw->frame != 2) {
                    vw->frame_fires++;
                    if (vw->quad_active)      /* 0x80050004 */
                        vw->quad_sound = true;
                    vw->spin_accum = 0;
                }
            }
        } else {
            /*
             * 0x8005003C. Releasing clears the accumulator, and a release on
             * the MIDDLE key jumps to the last one — so the clip can end
             * instead of hanging on a key the wrap keeps returning to.
             */
            vw->spin_accum = 0;
            if (vw->frame == 1)
                vw->frame = 2;
        }
        break;

    case VW_CHAINGUN: {
        /*
         * 0x80050058. Three bands in one 28-key clip: spin-up 0..8, LOOP
         * 9..17, spin-down 18..27.
         *
         *   held     and frame == 17 -> frame = 9   (stay in the loop)
         *   released and frame ==  9 -> frame = 27
         *
         * A DRY chaingun skips the wrap entirely (0x80050074 jumps straight to
         * the sounds), which is what lets the loop fall out into the spin-down
         * instead of grinding on an empty gun.
         */
        if (fire_held) {
            if (!vw->fire_latch && vw->frame == 17)
                vw->frame = 9;
        } else {
            vw->spin_accum = 0;
            if (vw->frame == 9) {
                /* 0x800500A0 stores the STATE into the rate field, which is 1
                 * — not the 2 this used to write. Any frame that reaches the
                 * band table below overwrites it anyway. */
                vw->spin_rate = 1;
                vw->frame     = 27;
            }
        }

        /* Everything below is once per NEW frame — the cache at +218. */
        if ((s32)vw->frame == vw->last_fire_frame)
            break;
        vw->last_fire_frame = (s32)vw->frame;

        /* The three band boundaries each play their own clip. */
        if (vw->frame == 0)       vw->frame_sound = Q2_WSND_CHAINGUN_UP;
        else if (vw->frame == 10) vw->frame_sound = Q2_WSND_CHAINGUN_LOOP;
        else if (vw->frame == 18) vw->frame_sound = Q2_WSND_CHAINGUN_DOWN;

        if (vw->fire_latch) {
            /* 0x800503C4: a dry gun leaves the FIRE state when the clip comes
             * back round to its first key, rather than at the clip's end. */
            if (vw->frame == 0) {
                vw->state = Q2_VM_IDLE;
                vw->frame = 0;
            }
            break;
        }

        /* 0x8005015C: no shot on the band boundaries or the whole spin-down. */
        if (vw->frame == 0 || vw->frame == 9 || vw->frame == 17 ||
            vw->frame >= 18)
            break;

        /*
         * THE BULLETS PER FRAME, which is openquestions #39c and was defaulted
         * to one here. 0x80050180 reads three bands off the frame —
         *
         *     frame < 5           1
         *     5 <= frame < 9      2 while the trigger is held, else 1
         *     frame >= 10         3
         *
         * — so a chaingun wound all the way up throws three rounds per
         * animation frame and a tapped one throws a single round. Frame 9 is
         * filtered out above and is the one frame that writes no rate at all.
         */
        if (vw->frame < 5)
            vw->spin_rate = 1;
        else if (vw->frame < 9)
            vw->spin_rate = fire_held ? 2 : 1;
        else if (vw->frame >= 10)
            vw->spin_rate = 3;

        /*
         * 0x800501C8 clamps that count to the rounds actually left, reading
         * the ammo type off 0x8009DC5C and the count out of the player's own
         * block. This module cannot see an inventory, and does not need to:
         * the caller turns each unit of `frame_fires` into one call of the
         * weapon's fire function, and a fire function that runs out reports
         * dry and stops. Asking for three and getting two IS the clamp.
         */
        {
            s32 n = vw->spin_rate;

            if (n < 1)
                n = 1;
            vw->frame_fires += (u32)n;
            if (vw->quad_active)              /* 0x80050234 */
                vw->quad_sound = true;
        }
        break;
    }

    case VW_HYPERBLASTER:
        /*
         * 0x8005026C. Frames 1..5 are the loop; a held trigger wraps 5 back to
         * 1, and frame 6 is the tail the loop must not fire on. A dry gun goes
         * straight to the sounds, which is what lets it reach frame 6 and stop.
         */
        if (fire_held && !vw->fire_latch) {
            if (vw->frame == 5)
                vw->frame = 1;

            vw->spin_accum += step;
            if (vw->spin_accum >= VW_SPIN_THRESHOLD) {
                if (vw->frame != 6) {
                    vw->frame_fires++;
                    if (vw->quad_active)      /* 0x80050300 */
                        vw->quad_sound = true;
                }
                vw->spin_accum = 0;
            }
        } else if (!fire_held) {
            vw->spin_accum = 0;
        }

        if ((s32)vw->frame == vw->last_fire_frame)
            break;
        vw->last_fire_frame = (s32)vw->frame;

        /*
         * AND IT LOOPS THE CHAINGUN'S SAMPLE. 0x80050378 loads `0x800B2B3C`,
         * which `0x8004E248` fills from `wep_chngnl1a` — the same handle the
         * chaingun's own loop plays. The hyperblaster's `wep_hyprbf1a` lives at
         * `0x800B2B58` and belongs to the FIRE function, not to this arm. The
         * name-to-slot order was read twice and agrees, so this is the retail
         * build's behaviour rather than a mis-decode.
         */
        if (vw->frame == 0)
            vw->frame_sound = Q2_WSND_CHAINGUN_LOOP;
        else if (vw->frame == 6)
            vw->frame_sound = Q2_WSND_HYPERBLAST_DOWN;

        /* 0x800503C4 again, shared with the chaingun. */
        if (vw->fire_latch && vw->frame == 0) {
            vw->state = Q2_VM_IDLE;
            vw->frame = 0;
        }
        break;

    case VW_HAND_GRENADE:
        /*
         * 0x800503DC — the COOK, and it is the one arm that does not fire.
         *
         * IT IS GATED ON THE MODEL'S OWN ANIMATION, which is the part that was
         * missing: 0x800503F0 refuses to prime until viewmodel+256 has reached
         * 380, and 380 sits inside the `Set` move's 0..470 span. So the arm has
         * to finish bringing the grenade up out of the belt before the hold
         * begins; until then the clip runs on and the throw happens on time
         * whatever the trigger is doing.
         *
         * Once primed the position is PINNED at 380, the frame at 1, and the
         * key's remaining time GROWS by the step, so the clip cannot advance
         * and the grenade is held for as long as the trigger is down.
         */
        /* Grenade3's own think owns both sounds/transitions. It compares the
         * current model position against the prior one at 0x8004A474 and
         * 0x8004A4A8, so a long substep still raises each crossing once. */
        if (vw->anim_pos >= Q2_VW_HAND_PRIME_POSITION &&
            vw->hand_prev_anim < Q2_VW_HAND_PRIME_POSITION)
            vw->frame_sound = Q2_WSND_HANDGREN_PRIME;

        if (fire_held && vw->anim_pos >= Q2_VW_COOK_POSITION) {
            vw->anim_pos = Q2_VW_COOK_POSITION;
            vw->frame    = 1;
            vw->left    += step;
            vw->cook     = true;
            vw->hand_cook_ticks += step;
        } else {
            vw->cook = false;
        }

        if (vw->anim_pos >= Q2_VW_HAND_RELEASE_POSITION &&
            vw->hand_prev_anim < Q2_VW_HAND_RELEASE_POSITION) {
            vw->hand_release = true;
            vw->frame_sound  = Q2_WSND_HANDGREN_THROW;
        }
        vw->hand_prev_anim = vw->anim_pos;
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* The model's own named move — 0x80050454                                    */
/* ------------------------------------------------------------------------- */
/*
 * The jump table at 0x800ACD34, indexed by `weapon - 3` over nine slots, with
 * the outer test at 0x800504D4 admitting only 3, 6, 7, 8 and 11. The three arms
 * it reaches load a 12-byte name and hand it to 0x8006D330, which is
 * `q2_model_move_by_name` — the engine never indexes block D.
 */
static const char *anim_move_name(int weapon)
{
    switch (weapon) {
    case 3:  return "Fire";     /* Supershot G   — 0x800505EC */
    case 6:  return "Set";      /* HandGren G    — 0x800505AC */
    case 7:  return "Spin";     /* GrenLaunch G  — 0x800505CC */
    case 8:  return "Fire";     /* RockLaunch G  — 0x800505EC */
    case 11: return "Fire";     /* Bfg G         — 0x800505EC */
    default: return NULL;
    }
}

/*
 * 0x80050454's second half, run on every substep of the key loop.
 *
 * A move already playing advances by the step and ends when the position walks
 * past the record's last (0x8005053C's `slt end, pos`); otherwise a key
 * carrying event 1 in the FIRE state starts one. The state test is
 * 0x80050508's `state != 1`, which is FIRE — an idle clip's event, if one ever
 * carried this value, would not reach the lookup.
 */
static void anim_step(q2_viewweapon *vw, s32 step)
{
    const q2_vm_key *k;
    const char      *name;
    q2_model_move    mv;

    if (vw->anim_end >= 0) {
        s32 pos = (s32)vw->anim_pos + step;

        if (pos > (s32)vw->anim_end) {
            /* 0x80050548: the record is dropped, the position rewinds to the
             * rest pose, and the two flag bits swap over. */
            vw->anim_pos   = 0;
            vw->anim_end   = -1;
            vw->anim_flags = (u16)((vw->anim_flags | 1u) & ~2u);
            return;
        }
        vw->anim_pos = (s16)pos;
        return;
    }

    if (vw->state != Q2_VM_FIRE)
        return;
    if (vw->anim_flags & 2u)
        return;

    k = current_key(vw);
    if (!k || k->event != 1)
        return;

    name = anim_move_name(vw->weapon);
    if (!name || !vw->model)
        return;
    if (!q2_model_move_by_name(vw->model, name, &mv))
        return;

    /*
     * BLOCK D TIMES FIVE, and the evidence is the GRENADE, not this call site.
     *
     * 0x80050530 stores the record's `+12` into viewmodel+256 with no scale of
     * any kind, and openquestions #51f swept the whole image for a load-time
     * multiply and found none — every `x5` shape is a 20-byte record stride. So
     * the arithmetic here has no instruction behind it, and taking the numbers
     * as written would be the defensible move if one constant did not rule it
     * out.
     *
     * That constant is the 380 at 0x800503F8. The cook arm refuses to prime the
     * hand grenade until viewmodel+256 has reached 380, and `HandGren G`'s `Set`
     * move runs 0..94 on the disc. Unscaled the position tops out at 94 and the
     * grenade could NEVER be cooked; the console cooks grenades. The threshold
     * has to sit inside the move's span, which puts the scale at 4.05 or more,
     * and five is the ratio the two documented unit systems already have — block
     * D counts two per animation frame (#51g), the animation position counts ten
     * (#51d). At five the span is 0..470 and 380 lands four fifths of the way
     * through the arm coming up, which is where a prime belongs.
     *
     * So: the five is inferred, the inference is forced, and the thing that
     * forces it is written down here so the next reader can attack it.
     */
    vw->anim_pos   = (s16)((s32)mv.start * 5);
    vw->anim_end   = (s16)((s32)mv.end   * 5);
    vw->anim_flags = (u16)((vw->anim_flags & ~1u) | 2u);
    vw->anims_started++;
}

/* ------------------------------------------------------------------------- */
/* The hyperblaster's barrel — 0x8004FC78                                     */
/* ------------------------------------------------------------------------- */
/*
 * A third driver, called once per substep beside the other two (0x8004F270)
 * and gated on weapon 9 and nothing else — 0x8004FCB8 returns immediately for
 * every other weapon.
 *
 * The four fire-frame arms behind the jump table at 0x800ACD1C all compute the
 * same shape, `base - 40 * (duration - left + 2)`, with a different base each:
 * 4096, 3296, 2096, 1296 for frames 1, 2, 3 and 4. Frame 6 is a straight
 * `(left << 12) / duration`, seeded to 4096 on the substep the key starts.
 * Frames 0, 5 and everything from 7 write nothing, and neither does any state
 * but FIRE and IDLE — so a raise or a lower holds whatever the last shot left
 * the barrel at, which is what makes the spin carry across the clip boundary.
 *
 * `duration` here is the key's STORED duration (`lh 12(v0)` reads the key, not
 * the viewmodel's clock), so a jittered key would divide by a different number
 * than it counts down — no fire clip carries jitter, so it never arises.
 */
static void hyper_ramp_step(q2_viewweapon *vw)
{
    const q2_vm_key *k;
    s32 dur, elapsed;

    if (vw->weapon != VW_HYPERBLASTER)
        return;

    if (vw->state == Q2_VM_IDLE) {
        vw->hyper_ramp = VW_HYPER_RAMP_ONE;      /* 0x8004FCD8 */
        return;
    }
    if (vw->state != Q2_VM_FIRE)
        return;                                   /* 0x8004FCD0 holds it */

    k = current_key(vw);
    if (!k)
        return;

    dur = k->duration;
    if (dur <= 0)
        return;

    /* 0x8004FD3C…0x8004FD50: the elapsed term is `duration - left + 2`, and
     * the two is the original's, not a rounding. */
    elapsed = dur - vw->left + 2;

    switch (vw->frame) {
    case 1: vw->hyper_ramp = (s16)(4096 - 40 * elapsed); break;
    case 2: vw->hyper_ramp = (s16)(3296 - 40 * elapsed); break;
    case 3: vw->hyper_ramp = (s16)(2096 - 40 * elapsed); break;
    case 4: vw->hyper_ramp = (s16)(1296 - 40 * elapsed); break;

    case 6:
        /* 0x8004FE44: the first substep of the key seeds it at one, and after
         * that it is the key's own remaining fraction. */
        if (vw->left == dur)
            vw->hyper_ramp = VW_HYPER_RAMP_ONE;
        vw->hyper_ramp = (s16)((vw->left << 12) / dur);
        break;

    default:
        break;                                    /* 0, 5 and 7 upward hold */
    }
}

u32 q2_vw_take_frame_fires(q2_viewweapon *vw)
{
    u32 n;

    if (!vw)
        return 0;
    n = vw->frame_fires;
    vw->frame_fires = 0;
    return n;
}

bool q2_vw_take_quad_sound(q2_viewweapon *vw)
{
    bool q;

    if (!vw)
        return false;
    q = vw->quad_sound;
    vw->quad_sound = false;
    return q;
}

s16 q2_vw_take_frame_sound(q2_viewweapon *vw)
{
    s16 snd;

    if (!vw)
        return -1;
    snd = vw->frame_sound;
    vw->frame_sound = -1;
    return snd;
}

s32 q2_vw_take_hand_grenade_cook(q2_viewweapon *vw)
{
    s32 ticks;

    if (!vw)
        return 0;
    ticks = vw->hand_cook_ticks;
    vw->hand_cook_ticks = 0;
    return ticks;
}

bool q2_vw_take_hand_grenade_release(q2_viewweapon *vw)
{
    bool release;

    if (!vw)
        return false;
    release = vw->hand_release;
    vw->hand_release = false;
    return release;
}

void q2_vw_hand_grenade_expired(q2_viewweapon *vw)
{
    if (!vw || vw->weapon != VW_HAND_GRENADE || vw->state != Q2_VM_FIRE)
        return;

    /* 0x8004AA1C..0x8004AA40, after the held grenade detonates. It does not
     * begin a new key: it writes frame and remaining time directly, leaving
     * the current interpolation total exactly as the executable does. */
    vw->frame           = 2;
    vw->left            = 150;
    vw->anim_pos        = 0;
    vw->anim_end        = -1;
    vw->anim_flags      = (u16)((vw->anim_flags | 1u) & ~2u);
    vw->cook            = false;
    vw->hand_prev_anim  = 0;
    vw->hand_cook_ticks = 0;
    vw->hand_release    = false;
    vw->frame_sound     = -1;
}

/*
 * Returns true when the model in hand changed. `clip_done` is the original's
 * `clip_start + 20*frame == clip_end`, i.e. the frame index has walked off the
 * end of the clip.
 */
static bool machine_step(q2_viewweapon *vw, bool clip_done, bool fire_held,
                         q2_vw_fire_result fired)
{
    bool swapped = false;

    switch (vw->state) {
    case Q2_VM_LOWER:
        if (!clip_done)
            break;

        /*
         * 0x8004F944. The countdown gates the swap: while it runs the frame is
         * rewound by one and the weapon hangs at the bottom of its arc. Only
         * when it expires does the pending weapon become the real one.
         */
        if (vw->switch_ticks > 0) {
            if (vw->frame > 0)
                vw->frame--;
            break;
        }

        vw->weapon     = vw->pending;
        vw->state      = Q2_VM_RAISE;
        vw->frame      = 0;
        vw->fire_latch = false;
        swapped        = true;
        /*
         * 0x8004FA44. A new model in the hands starts at position 0 of its own
         * timeline — the rest pose — with nothing playing. (0x8004FA3C is the
         * hyperblaster's exception, and it stores the PLAYER INDEX times ten,
         * which is zero for the only player this port draws a weapon for.)
         */
        vw->anim_pos   = 0;
        vw->anim_end   = -1;
        vw->anim_flags = 0;
        begin_key(vw);
        break;

    case Q2_VM_RAISE:
        /* 0x8004FA80: the raise ends in IDLE, and that is where the fire
         * function is bound for the weapon now in hand. */
        if (clip_done) {
            vw->state = Q2_VM_IDLE;
            vw->frame = 0;
            begin_key(vw);
        }
        break;

    case Q2_VM_FIRE:
        /*
         * 0x8004FC04: the fire clip always returns to idle — and it does NOT
         * clear the latch. That omission is the refire cadence: the shot stays
         * latched into the idle state, and it takes one further pass through
         * the fire check to clear it, so a held trigger fires, idles for one
         * pass, and fires again rather than restarting the clip every tick.
         */
        if (clip_done) {
            vw->state = Q2_VM_IDLE;
            vw->frame = 0;
            begin_key(vw);
        }
        break;

    case Q2_VM_IDLE:
    default:
        q2_vw_idle_fire_check(vw, fire_held, fired);
        if (vw->state == Q2_VM_IDLE && clip_done) {
            /* 0x8004FBD8: idle loops. */
            vw->frame = 0;
            begin_key(vw);
        }
        break;
    }

    /*
     * 0x8004FAB4. A selection that differs from what is in hand starts the
     * lower, from any state except the one already firing — which is why you
     * cannot cancel a shot by switching.
     */
    if (!swapped && vw->pending != vw->weapon &&
        vw->state != Q2_VM_FIRE && vw->state != Q2_VM_LOWER) {
        vw->state = Q2_VM_LOWER;
        vw->frame = 0;
        begin_key(vw);
    }

    return swapped;
}

bool q2_vw_advance(q2_viewweapon *vw, s32 dt, bool fire_held,
                   q2_vw_fire_result fired)
{
    bool swapped = false;
    int guard = 0;

    if (!vw || !vw->tab)
        return false;

    if (vw->switch_ticks > 0) {
        vw->switch_ticks = (s16)(vw->switch_ticks - dt);
        if (vw->switch_ticks < 0)
            vw->switch_ticks = 0;
    }

    /*
     * 0x8004EEA4. The original consumes dt in chunks of `min(left, dt)` and
     * loops, so a long host frame plays a short clip all the way through
     * instead of skipping it — which matters because the events on those keys
     * are what fire the shot.
     *
     * The guard is a port's own: the original cannot spin here because every
     * clip has at least one key with a non-zero duration, but a mis-read table
     * could, and hanging is a worse failure than a stalled animation.
     */
    while (dt > 0 && guard++ < 512) {
        s32 step = dt;

        if (vw->total > 0 && vw->left < step)
            step = vw->left;
        if (step < 0)
            step = 0;

        vw->left -= step;
        dt       -= step;

        /*
         * Two drivers per substep, in the original's order: the MODEL's own
         * named move at 0x8004EF38, then the per-weapon frame driver at
         * 0x8004EF4C. The order is load-bearing — the grenade's cook arm pins
         * the animation position the move driver has just advanced, so running
         * them the other way round un-pins it every substep.
         */
        anim_step(vw, step);
        fire_state_step(vw, step, fire_held);
        hyper_ramp_step(vw);

        interpolate(vw);

        if (vw->left > 0)
            break;

        /* The key is spent: advance, and let the machine see whether that ran
         * off the end of the clip. */
        vw->frame++;
        vw->keys_played++;

        {
            u32 len = clip_length(vw, vw->state);
            bool done = (vw->frame >= len);

            if (machine_step(vw, done, fire_held, fired))
                swapped = true;
            else if (done && vw->state != Q2_VM_LOWER)
                vw->frame = 0;
        }

        begin_key(vw);
        interpolate(vw);

        if (vw->total <= 0 && dt > 0) {
            /* A zero-length key would otherwise consume no time and spin. */
            continue;
        }
    }

    /*
     * The trigger is tested every tick, not every key boundary (0x8004FAF4), so
     * a press between two keys still fires on the tick it arrived.
     */
    if (vw->state == Q2_VM_IDLE) {
        q2_vw_idle_fire_check(vw, fire_held, fired);
        if (vw->state == Q2_VM_FIRE)
            interpolate(vw);
    }

    /* And a selection change likewise. */
    if (vw->pending != vw->weapon &&
        vw->state != Q2_VM_FIRE && vw->state != Q2_VM_LOWER) {
        vw->state = Q2_VM_LOWER;
        vw->frame = 0;
        begin_key(vw);
        interpolate(vw);
    }

    return swapped;
}

/* ------------------------------------------------------------------------- */
/* Placement                                                                  */
/* ------------------------------------------------------------------------- */
void q2_vw_place(const q2_viewweapon *vw,
                 const s32 feet[3], s32 view_offset,
                 const s16 aim[3], const s16 kick[3],
                 s32 origin_out[3], s32 angles_out[3])
{
    s16 rot[3][3];
    s32 ang[3];
    s32 local[3];
    int i;

    if (!vw || !feet)
        return;

    /*
     * 0x8004F40C. The three angles are the player's aim plus the second triple
     * the original adds, and the x component is NEGATED at the sum. The
     * animation's own rotation rides on top, which is what makes a weapon dip
     * as it is raised rather than snapping upright.
     */
    /*
     * `ang` is the VIEW matrix and nothing else. The clip's own rotation is NOT
     * in it: the original builds `sp+40` from the player's aim and the second
     * angle triple only (0x8004F40C…0x8004F448) and hands that to RotMatrix, and
     * the clip's rotation reaches the model through the separate chain at
     * 0x8004F284. Folding the clip's angles in here rotates the offset by them
     * too, and since several clips carry a half-circle component that puts the
     * weapon behind the camera — where every face is rejected at the projection
     * plane and nothing draws at all.
     */
    ang[0] = -((aim ? aim[0] : 0) + (kick ? kick[0] : 0));
    ang[1] =  ((aim ? aim[1] : 0) + (kick ? kick[1] : 0));
    ang[2] =  ((aim ? aim[2] : 0) + (kick ? kick[2] : 0));

    if (angles_out) {
        /* The model's own orientation does carry the clip's rotation, composed
         * on top of the view — 0x8004F474's MulMatrix. */
        angles_out[0] = ang[0] + vw->cur_r[0];
        angles_out[1] = ang[1] + vw->cur_r[1];
        angles_out[2] = ang[2] + vw->cur_r[2];
    }

    if (!origin_out)
        return;

    /*
     * 0x8004F5E0. The animation's translation is rotated before the eye is
     * added — rotate then translate, not the other way round, or the weapon
     * slides across the view instead of swinging with it.
     *
     * The matrix is the camera's INVERSE, and that is not a detail. The view
     * model is authored in view space: `Blaster G` runs from z = 0 at the grip
     * to z = 482 at the muzzle, so its own +Z is "away from the eye", not a
     * world direction. Rotating the offset by the camera's forward matrix
     * instead sends it out along a world axis — which is what put the weapon
     * behind the camera and had every face rejected at the projection plane.
     *
     * A rotation matrix's inverse is its transpose, so this is the same matrix
     * read down its columns.
     */
    /*
     * All THREE angles, because `RotMatrix` takes all three.
     *
     * This used to build a yaw/pitch matrix and apply its transpose, which is
     * the same thing as `q2_rotation_euler(pitch, yaw, 0)` applied directly —
     * correct except that it silently drops the roll. `0x8004F464` hands
     * `RotMatrix` the whole SVECTOR at `sp+40`, and `sp+44` is the z component,
     * so the console rotates the offset by the roll as well. Dropping it means
     * the weapon does not lean with the view: the strafe lean rolls the camera
     * and the gun stays upright and slides, which is the one case where the
     * two visibly separate.
     *
     * `q2_rotation_euler` is `RotMatrix`'s own composition — checked at
     * `0x80089E38`, which writes `m[1][2] = -sin(x)` and
     * `m[0][2] = sin(y)cos(x)`, the signature of Ry·Rx with Z outermost.
     */
    /*
     * THE CAMERA'S OWN BUILDER, TRANSPOSED - and the transpose is the whole
     * correctness argument.
     *
     * This used to be `q2_rotation_euler(rot, -pitch, yaw, roll)` applied
     * directly, and every operand in it had been checked against the
     * executable and agreed. It was still wrong, and openquestions #46 spent
     * three passes re-reading operands, because the defect is not in any of
     * them: it is in the IDENTITY the arrangement rests on.
     *
     * Follow a part origin through modeldraw.c. With `local` zero at the grip,
     * `inst.origin = feet + R_place*t`, and `cam.pos` the eye - which is the
     * same `feet - view_offset` this function computes, deliberately -
     *
     *     camera_space = view * (inst.origin - cam.pos) = view * R_place * t
     *
     * so the weapon lands where the clip authored it, at `t` in view space,
     * only if **view * R_place is the identity**. Nothing asserted that, and it
     * is not: `q2_rotation_view(yaw, pitch, roll)` and
     * `q2_rotation_euler(-pitch, yaw, roll)` agree on yaw and disagree on
     * pitch, so their product is the identity when the player looks level and
     * is off by 3260/4096 at a 26-degree pitch. tests/test_viewweapon.c pins it
     * now.
     *
     * Building the placement matrix with the CAMERA'S OWN function and applying
     * its transpose makes `view * R_place == I` true by construction, for any
     * angles, because a rotation matrix's transpose is its inverse. That is the
     * same argument `q2_vw_build_ot` already makes for the ROTATION half; it
     * was never applied to the translation.
     *
     * The argument ORDER is the camera's too: `q2_rotation_view` takes
     * (yaw, pitch, roll), and `ang` is indexed (pitch, yaw, roll) - with
     * `ang[0]` already negated at the sum above, which is 0x8004F40C's own
     * negation and is why the pitch is passed back through un-negated here.
     */
    q2_rotation_view(rot, ang[1], -ang[0], ang[2]);

    for (i = 0; i < 3; i++) {
        local[i] = ((s32)rot[0][i] * vw->cur_t[0]
                  + (s32)rot[1][i] * vw->cur_t[1]
                  + (s32)rot[2][i] * vw->cur_t[2]) >> Q2_FRAC_12;
    }

    /*
     * 0x8004F5E8…0x8004F640, and the base is the ENTITY ORIGIN.
     *
     * The console's expression is `pos.y + 286 - viewOffset` where `pos` is
     * entity+0x58 — the point the mover works in, 286 above the feet. This
     * function is handed the FEET (that is what `q2_player.pos` holds), so the
     * conversion has to happen before the 286 is added, and then the two
     * cancel:
     *
     *     (feet - 286) + 286 - viewOffset  ==  feet - viewOffset
     *
     * which is exactly what q2_sim_eye computes. That identity is the whole
     * point: the weapon hangs off the eye, so the two must be the same
     * arithmetic, and writing it as `feet + 286 - viewOffset` — adding the
     * constant to a base that is already 286 low — put the weapon 286 units
     * below the camera and dropped it off the bottom of the screen.
     *
     * `Q2_VW_EYE_BASE` is kept for the two tests that pin the constant against
     * 0x8004F608, and deliberately not used in the sum: there is nothing here
     * to add it to that is not immediately subtracted again.
     */
    origin_out[0] = feet[0] + local[0];
    origin_out[1] = feet[1] + local[1] - view_offset;
    origin_out[2] = feet[2] + local[2];
}

u32 q2_vw_build_ot(const q2_viewweapon *vw,
                   const q2_model_instance *proto,
                   const s32 feet[3], s32 view_offset,
                   const s16 aim[3], const s16 kick[3],
                   const q2_camera *cam,
                   psx_ot *ot, gte_state *gte,
                   q2_model_draw_stats *stats)
{
    q2_model_instance inst;
    q2_model_pose     pose[Q2_VW_POSE_MAX];
    bool              posed = false;
    s32 origin[3];
    s32 angles[3];
    s32 angles_view[3];
    s16 rot_out[3][3];

    if (!vw || !vw->model || !cam || !ot || !gte)
        return 0;

    if (proto)
        inst = *proto;
    else
        q2_model_instance_init(&inst);

    q2_vw_place(vw, feet, view_offset, aim, kick, origin, angles);

    /* The view part of those angles, without the clip's — the composition above
     * needs the camera's rotation on its own. */
    angles_view[0] = -((aim ? aim[0] : 0) + (kick ? kick[0] : 0));
    angles_view[1] =  ((aim ? aim[1] : 0) + (kick ? kick[1] : 0));
    angles_view[2] =  ((aim ? aim[2] : 0) + (kick ? kick[2] : 0));
    (void)angles;

    /*
     * POSE IT, which nothing did — and an unposed model draws its vertices RAW.
     *
     * Every other model in the frame goes through this (entitydraw.c makes the
     * argument at length: a part's rest transform is not the identity, and the
     * medkits sat in the floor until it was applied). The weapon in the hands
     * was the one model still drawn from the vertex block, so `HandGren G` —
     * whose raw bounds are a 105-unit lump against a posed 666-unit arm — came
     * out as a fist-sized blob under the crosshair, and `Railgun G` and `Bfg G`
     * were short by their own rest translations.
     *
     * The position is the model's own timeline, held rather than failed off the
     * end (`_held` is the engine's own behaviour: 0x8006B924 has no end test).
     * With no named move playing it is zero, which is the rest pose.
     */
    if (vw->model->hdr.num_parts <= Q2_VW_POSE_MAX) {
        q2_model_anim clip;
        u32 within = 0;
        u32 at = vw->anim_pos > 0 ? (u32)vw->anim_pos : 0u;

        if (q2_model_anim_at_held(vw->model, at, &clip, &within, NULL) &&
            q2_model_pose_at(vw->model, &clip, within, pose) == Q2_OK)
            posed = true;

        /*
         * AND THE HYPERBLASTER'S BARREL, which is the pose plus one patched
         * field — 0x8006D43C.
         *
         * The console walks to part 6's key in the model's own animation and
         * overwrites the low eleven bits of its packed rotation word with
         * `(ramp & 0xFFF) >> 1`, leaving the other two Euler half-angles
         * alone (its `a2` and `a3` arrive as -1 and the two `bltz` tests skip
         * their stores). It can write there because the model is in RAM; here
         * the word is read back out and re-decoded into this instance's own
         * pose, which draws the same barrel without touching the bank.
         */
        if (posed && vw->weapon == VW_HYPERBLASTER &&
            VW_HYPER_BARREL_PART < vw->model->hdr.num_parts) {
            u32 rot;

            if (q2_model_part_rotation_word(vw->model, &clip, within,
                                            VW_HYPER_BARREL_PART, &rot)) {
                u32 half = ((u32)(u16)vw->hyper_ramp & 0xFFFu) >> 1;

                rot = (rot & ~0x7FFu) | half;
                q2_model_key_rotation(rot, pose[VW_HYPER_BARREL_PART].q);
            }
        }
    }

    inst.model     = vw->model;
    inst.pose      = posed ? pose : NULL;
    /* +0xFC/+0xFE scale the GTE light matrix and back colour, not the model
     * transform. The caller passes them to q2_light_env_build; geometry keeps
     * the allocator-neutral transform just like an ordinary entity. */
    inst.scale = Q2_ONE_12;
    /*
     * The view-weapon entity is permanently assigned screen area 1 at
     * 0x8004EE58 (`sb 1, entity+0x9E`).  It later reaches the ordinary model
     * renderer, whose 0x8006BEB0 call selects that area before projecting it.
     *
     * This is observable because 0x80065684 is stateful: its negative selector
     * is a no-op.  Leaving the prototype's default -1 here therefore inherited
     * whichever portal-local origin the last particle or projectile installed,
     * moving and clipping the gun as the visible area list changed.
     */
    inst.sort_area = 1;
    inst.origin[0] = origin[0];
    inst.origin[1] = origin[1];
    inst.origin[2] = origin[2];

    /*
     * The rotation the model is drawn with, and the reason it is a matrix
     * rather than three angles.
     *
     * `q2_model_build_ot` composes the CAMERA's rotation with the instance's,
     * because that is what the console does for anything standing in the world.
     * A view model is not standing in the world: it is authored in view space,
     * so what has to survive that composition is the clip's own wobble and
     * nothing else. The instance matrix is therefore the camera's inverse —
     * its transpose — with the clip's rotation on top:
     *
     *      camera * (camera^T * R_clip)  ==  R_clip
     *
     * which is the port's form of the original composing `RotMatrix(view
     * angles)` into the entity's own matrix at 0x8004F474 and letting the
     * camera undo it.
     */
    {
        s16 inv[3][3], clip[3][3];
        int r, c, k;

        /*
         * The matrix to cancel is the CAMERA's, so it is built from the camera
         * — with the camera's own builder — rather than reconstructed from the
         * aim and the kick.
         *
         * Reconstructing it was exact only by luck. `q2_model_build_ot` uses
         * `q2_rotation_view(cam->yaw, cam->pitch, cam->roll)`, and this built a
         * yaw/pitch matrix from `angles_view`, so the two agreed only while the
         * roll was zero and the caller's aim + kick happened to equal the
         * camera's angles. Taking the camera directly makes
         * `camera * (camera^T * clip) == clip` true by construction for any
         * camera, which is what the cancellation is for.
         */
        q2_rotation_view(inv, cam->yaw, cam->pitch, cam->roll);
        q2_rotation_euler(clip, vw->cur_r[0], vw->cur_r[1], vw->cur_r[2]);

        for (r = 0; r < 3; r++) {
            for (c = 0; c < 3; c++) {
                s32 acc = 0;
                for (k = 0; k < 3; k++)
                    acc += (s32)inv[k][r] * (s32)clip[k][c];   /* inv^T * clip */
                rot_out[r][c] = (s16)(acc >> Q2_FRAC_12);
            }
        }
        inst.rot = (const s16 (*)[3])rot_out;
    }

    return q2_model_build_ot(&inst, cam, ot, gte, stats);
}
