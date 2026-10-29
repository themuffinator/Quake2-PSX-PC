/* Flyer, transcribed from BASE2/ZONE1.DAT (PAL), relocated at 0x80100000.
 * MAGDEMO has identical instructions and tables; only the two unused bytes
 * after the second blur-list terminator differ. Moves remain disc data.
 */
#include "ai.h"
#include "aimove.h"
#include "crebind.h"
#include "random.h"

#define FLY_STAND  0x801013ECu
#define FLY_WALK   0x80101484u
#define FLY_RUN    0x8010151Cu

extern void (*q2_cre_sound_fn)(q2_monster *, int, void *);
extern void *q2_cre_sound_user;

static void flyer_play(q2_monster *m, u32 handle)
{
    if (q2_cre_sound_fn)
        q2_cre_sound_fn(m, (int)handle, q2_cre_sound_user);
}

static void flyer_stand(q2_monster *m) { q2_cre_set_move_at(m, FLY_STAND); }
static void flyer_walk(q2_monster *m)  { q2_cre_set_move_at(m, FLY_WALK); }
static void flyer_attack(q2_monster *m) { q2_cre_set_move(m, 79); }
static void flyer_melee(q2_monster *m) { q2_cre_set_move(m, 58); }
static void flyer_loop_melee(q2_monster *m) { q2_cre_set_move(m, 64); }

/* +0xBF0: range is tested before drawing the 80% repeat chance. */
static void flyer_check_melee(q2_monster *m)
{
    if (q2_range(m, m->enemy) == Q2_RANGE_MELEE && q2_random_next() < 26214)
        flyer_loop_melee(m);
    else
        q2_cre_set_move(m, 76);
}

/* +0xC88/+0xCB4: this is module BSS shared by Flyers, not entity state.
 * The registered start method writes 3. The dispatcher also accepts 1/2,
 * although the retail module has no writer of those values.
 */
static void flyer_start(q2_monster *m)
{
    q2_cre_bind *b = q2_cre_bind_for(m);
    if (b) b->flyer_transition = 3;
    q2_cre_set_move(m, 0);
}

static void flyer_next_move(q2_monster *m)
{
    q2_cre_bind *b = q2_cre_bind_for(m);
    if (!b) return;
    switch (b->flyer_transition) {
    case 1: flyer_melee(m); break;
    case 2: flyer_attack(m); break;
    case 3: q2_cre_set_move_at(m, FLY_RUN); break;
    default: break;
    }
}

static void flyer_run(q2_monster *m)
{
    q2_cre_set_move_at(m, (m->aiflags & Q2_AI_STAND_GROUND) ? FLY_STAND : FLY_RUN);
}

/* +0xA04: eight alternating shots. Unlike the Soldier, the Flyer passes
 * the raw vector to the target's eye, without VectorNormalize. The engine
 * divides it by 64 and ignores the nominal speed. Frames 82/85/88 request
 * effect 0x40, selecting the hyperblaster flags and means of death.
 */
static void flyer_fire(q2_monster *m, s32 flash)
{
    q2_cre_shot shot = { 0 };
    int k;
    shot.slot = Q2_IMP_FIRE_BLASTER;
    shot.flash = flash;
    shot.damage = 1;
    shot.speed = 1000;
    shot.count = 1;
    shot.aimed = true;
    shot.effects = (m->frame == 82 || m->frame == 85 || m->frame == 88) ? 64 : 0;
    if (flash == 59)
        q2_cre_muzzle(m, 121, 128, shot.start);
    else
        q2_cre_muzzle(m, 112, 115, shot.start);
    if (m->enemy) {
        for (k = 0; k < 3; k++)
            shot.dir[k] = m->enemy->pos[k] - shot.start[k];
        shot.dir[1] += m->enemy->view_height;
    } else {
        /* Defensive host fallback; the native function requires an enemy. */
        q2_angle_vectors(m->angles, shot.dir, NULL, NULL);
    }
    q2_cre_fire_shot(m, &shot);
}

static void flyer_fire_left(q2_monster *m)  { flyer_fire(m, 58); }
static void flyer_fire_right(q2_monster *m) { flyer_fire(m, 59); }
static void flyer_slash_start(q2_monster *m) { flyer_play(m, 0x80101778u); }

static void flyer_slash(q2_monster *m, s32 side)
{
    const s32 aim[3] = { 1020, 0, side };
    q2_cre_fire_hit(m, aim, 5, 0);
    /* Init +0x78C copies the attack sound handle into +0x1774. */
    flyer_play(m, 0x80101778u);
}

static void flyer_slash_left(q2_monster *m)  { flyer_slash(m, m->mins[0]); }
static void flyer_slash_right(q2_monster *m) { flyer_slash(m, m->maxs[0]); }

static void flyer_sight(q2_monster *m, q2_monster *other)
{
    (void)other;
    flyer_play(m, 0x80101764u);
}

static void flyer_idle(q2_monster *m) { flyer_play(m, 0x80101768u); }

/* +0x108C: skin change precedes debounce; nightmare still sets the deadline
 * but neither draws the RNG nor interrupts the animation. All three arms
 * play flt_pain2: init stores the same handle at +0x176C and +0x1770.
 */
static void flyer_pain(q2_monster *m, s16 damage)
{
    static const s32 first[3] = { 134, 143, 147 };
    s32 choice;
    (void)damage;
    if (m->health < m->max_health / 2) m->skinnum = 1;
    if (q2_level_state.time < m->pain_debounce) return;
    m->pain_debounce = q2_level_state.time + 30;
    if (q2_cre_skill() == 3) return;
    choice = q2_random_next() % 3;
    flyer_play(m, 0x8010176Cu);
    q2_cre_set_move(m, first[choice]);
}

/* +0x11E8: no death animation or explosion. Freeze the current pose and
 * leave a damageable TOSS corpse, retaining the native hull and fly flag.
 */
static void flyer_die(q2_monster *m, s16 damage)
{
    (void)damage;
    flyer_play(m, 0x8010177Cu);
    m->dead = true;
    m->deadflag = Q2_DEAD_DEAD;
    m->takedamage = Q2_DAMAGE_YES;
    m->next_think = 0;
    m->svflags |= Q2_SVF_DEADMONSTER;
    m->movetype = Q2_MOVETYPE_TOSS;
}

static void flyer_spawn(q2_monster *m)
{
    m->mass = 50;
    m->movetype = Q2_MOVETYPE_STEP;
    m->solid = Q2_SOLID_BBOX;
    q2_link_entity(m, 1);
    q2_link_entity(m, 0x81);
    flyer_stand(m);
    m->speed_scale = 10;
    q2_monster_fly_start(m);
}

const q2_cre_impl q2_cre_flyer = {
    "Flyer",
    {
        flyer_stand, flyer_idle, NULL, flyer_walk, flyer_run, NULL,
        flyer_attack, flyer_melee, (q2_class_method)(void *)flyer_sight,
        NULL, NULL, (q2_class_method)(void *)flyer_pain,
        (q2_class_method)(void *)flyer_die
    },
    {
        NULL, flyer_check_melee, flyer_loop_melee, flyer_melee, flyer_start,
        flyer_stand, flyer_next_move, flyer_run, flyer_fire_left,
        flyer_fire_right, flyer_slash_start, flyer_slash_left, flyer_slash_right
    },
    flyer_spawn
};
