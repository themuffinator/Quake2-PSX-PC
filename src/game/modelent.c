/*
 * modelent.c — 0x8005A778 and 0x8005A5F8, and the gibs: 0x8007CEB4,
 * 0x8005A3D4, 0x8005A0AC, 0x8005AD8C, 0x8005B0AC and 0x80059DE0. See
 * modelent.h for the addresses and for what a plausible version gets wrong.
 */
#include "modelent.h"

#include "effect.h"        /* q2_fx_gib_spray / q2_fx_gib_trail */
#include "item.h"          /* q2_item_shrink_think — 0x8005B358 */
#include "monster.h"       /* the corpse site's record */
#include "playerdeath.h"   /* the player site's record */
#include "sdkmath.h"       /* SquareRoot0 / VectorNormal — the toss mover's */
#include "shock.h"         /* q2_shock_explosion — 0x8005AA7C */
#include "trace.h"         /* q2_move_push_vector — 0x8005625C */
#include "trig.h"
#include "worldscale.h"    /* Q2_GRAVITY, Q2_VEL_DIV */

#include <stdio.h>
#include <string.h>

/* 0x800ACDF4 and 0x800ACE00, in the order the fourth argument selects them. */
static const char *const g_names[Q2_MODEL_ENT_KIND_COUNT] = {
    "Explosion",
    "Hexplosion"
};

/* 0x800AEAD4, packed into a1 for the 0x80075C34 call at 0x8005A744. */
static const u8 g_explosion_light[3] = { 0xC0, 0x40, 0x31 };

const char *q2_model_ent_name(q2_model_ent_kind kind)
{
    if (kind < 0 || kind >= Q2_MODEL_ENT_KIND_COUNT)
        return NULL;
    return g_names[kind];
}

/* ------------------------------------------------------------------------- */
s32 q2_model_ent_lifetime(s32 clip_length)
{
    /* 0x8005A630: `a0 * 4 + a0` and then doubled. */
    return clip_length * Q2_MODEL_ENT_LIFE_MUL;
}

static s32 ramp(s32 clock, s32 mul)
{
    /* clamp(mul * (320 - t), 0, 4096) — 0x8005A65C..0x8005A684, and the second
     * copy at 0x8005A690..0x8005A6BC with a different multiplier. Both clamp
     * the HIGH side first and the low side second, which matters only for a
     * negative product and is transcribed rather than tidied. */
    s32 v = mul * (Q2_MODEL_ENT_RAMP_BASE - clock);

    if (v > Q2_ONE_12)
        v = Q2_ONE_12;
    if (v < 0)
        v = 0;
    return v;
}

s32 q2_model_ent_scale(s32 clock)
{
    return ramp(clock, Q2_MODEL_ENT_RAMP_SCALE);
}

s32 q2_model_ent_flash(s32 clock)
{
    /*
     * 0x8005A6C4..0x8005A6E0: the clamped ramp times 1300, arithmetic-shifted
     * back down by 12 with the console's own round-toward-zero fixup
     * (`bgez; addiu 4095`). 1300 is built as ((v*5) + (v*5 << 6)) << 2, i.e.
     * v * 5 * 65 * 4.
     */
    s32 v = ramp(clock, Q2_MODEL_ENT_FLASH_SCALE);
    s32 p = v * 1300;

    if (p < 0)
        p += Q2_ONE_12 - 1;
    return p >> 12;
}

/* ------------------------------------------------------------------------- */
bool q2_model_ent_height(const q2_model_bank *bank, const char *name,
                         s32 *out_height, s32 *out_clip_length,
                         s32 *out_index)
{
    q2_model m;
    s32 index;

    if (out_height)      *out_height = 0;
    if (out_clip_length) *out_clip_length = 0;
    if (out_index)       *out_index = -1;

    if (!bank || !name || !name[0])
        return false;

    /*
     * 0x8006D008 walks the model list comparing twelve bytes at record+8. The
     * port's bank lookup is by name over the same records, so this is that
     * search with the container the port already has rather than a second one.
     */
    index = q2_model_bank_find(bank, name);
    if (index < 0)
        return false;
    if (q2_model_get(bank, (u32)index, &m) != Q2_OK)
        return false;

    if (out_index)
        *out_index = index;

    /* 0x8006D100: `lh(model + 0x1C)`, which model.h calls ext2. */
    if (out_height)
        *out_height = m.hdr.ext2;

    /*
     * `lh(model + 2)` — the think's own read, at 0x8005A61C. Taken off the raw
     * header rather than through the animation table, because that is the
     * field the console uses and the two are only equal for a single-clip
     * model. `q2psx-inspect modelents` checks the equality across the disc so
     * the difference is measured rather than assumed.
     */
    if (out_clip_length)
        *out_clip_length = (s32)q2_rd_s16(m.base + 2);

    return true;
}

/* ------------------------------------------------------------------------- */
void q2_model_ent_sounds_clear(q2_model_ent_sounds *q)
{
    if (!q)
        return;
    q->count   = 0;
    q->dropped = 0;
}

void q2_model_ent_sound_push(q2_model_ent_sounds *q, const s32 at[3])
{
    int k;

    if (!q || !at)
        return;
    if (q->count >= Q2_MODEL_ENT_SOUNDS_MAX) {
        q->dropped++;
        return;
    }
    for (k = 0; k < 3; k++)
        q->at[q->count][k] = at[k];
    q->count++;
}

bool q2_model_ent_sound_next(q2_model_ent_sounds *q, s32 out[3])
{
    u32 i;
    int k;

    if (!q || !q->count)
        return false;

    if (out)
        for (k = 0; k < 3; k++)
            out[k] = q->at[0][k];
    for (i = 1; i < q->count; i++)
        for (k = 0; k < 3; k++)
            q->at[i - 1][k] = q->at[i][k];
    q->count--;
    return true;
}

/* ------------------------------------------------------------------------- */
q2_entity *q2_model_ent_spawn(q2_entity_set *set, const q2_model_bank *bank,
                              q2_model_ent_kind kind, const s32 at[3],
                              u8 surface, s32 size,
                              q2_model_ent_sounds *sounds)
{
    const char *name = q2_model_ent_name(kind);
    q2_entity *e;
    s32 height = 0, clip = 0, index = -1;
    int k;

    if (!set || !at)
        return NULL;

    /*
     * THE BIND COMES FIRST, and a failure throws the entity away rather than
     * leaving an invisible one in the pool — 0x8005A894 tests ent+0x10 and
     * jumps past every remaining write to the end of the function.
     *
     * Done before the allocation here for the same outcome with less work: the
     * console allocates, fails to bind, and abandons a slot that its own
     * free-list walk will reclaim. This port has no such walk, so allocating
     * first would leak.
     */
    if (!q2_model_ent_height(bank, name, &height, &clip, &index))
        return NULL;

    /*
     * 0x8006C098(1). The allocator already runs `q2_entity_init` and sets
     * `in_use` — calling init again here memset the slot and cleared the very
     * flag the draw walk gates on, so the entity existed, thought, aged and
     * died without ever being considered. Worth the sentence: the counter said
     * "1 Explosion models spawned" the whole time it was invisible.
     */
    e = q2_entity_alloc(set);
    if (!e)
        return NULL;

    e->think = q2_model_ent_think;     /* ent+0x3C = 0x8005A5F8 */

    for (k = 0; k < 3; k++) {
        e->pos[k]    = at[k];
        e->origin[k] = at[k];
    }

    /*
     * The box — 0x8005A97C and 0x8005A9A4. Y is DOWN in this engine, so `maxs`
     * being the model's own extent and `mins` being that less 512 puts the
     * box mostly BELOW the point the effect happens at. Transcribed rather
     * than corrected: it is what the console's obstruction tests see.
     */
    e->bounds_min[0] = -Q2_MODEL_ENT_HALF_WIDTH;
    e->bounds_min[1] = height - Q2_MODEL_ENT_HEIGHT;
    e->bounds_min[2] = -Q2_MODEL_ENT_HALF_WIDTH;
    e->bounds_max[0] = Q2_MODEL_ENT_HALF_WIDTH;
    e->bounds_max[1] = height;
    e->bounds_max[2] = Q2_MODEL_ENT_HALF_WIDTH;

    snprintf(e->model, sizeof(e->model), "%s", name);
    e->model_index = index;
    e->model_bank  = bank;
    e->clip_length = clip;

    e->frame = 0;                      /* ent+0x100, the clock */
    e->fade  = (s16)Q2_ONE_12;         /* ent+0xFE, full size   */
    e->scale = (s16)Q2_ONE_12;         /* ent+0xFC              */

    e->surface = surface;              /* ent+0x9E */
    e->field90 = Q2_MODEL_ENT_FIELD90; /* ent+0x90 */

    /* 0x8005A8E4..0x8005A910 copies the constant at 0x800AEAC8 into
     * +0x2B0 and +0x2AC. It is 40 40 40 00: retail's ambient floor for the
     * effect mesh, distinct from the C0/40/31 dynamic light below. */
    e->glow[0] = e->glow[1] = e->glow[2] = Q2_MODEL_ENT_AMBIENT;

    /*
     * The two marks that make it EVICTABLE — the allocator at 0x8006C0F0 pairs
     * a non-zero +0xF4 with bit 0x01000000 of +0x10C and recycles anything
     * carrying both. The console sets +0xF4 to 1 in the allocator itself; the
     * bit is what separates a transient effect from a respawning item.
     */
    e->remove_in     = 1;
    e->render_flags |= Q2_RF_TRANSIENT;

    e->hidden = false;
    e->kind   = Q2_ENT_KIND_MODEL;

    /*
     * THE BANG — 0x8005AA54, SoundByCreature(Sound_Explosion, ent), at the
     * entity's own +0x54, which is `at`. Before the rumble, as on the console;
     * the owner plays it (modelent.h).
     */
    q2_model_ent_sound_push(sounds, at);

    /*
     * AND EVERY PAD NEAR IT SHAKES — the function's last call, 0x8005AA7C,
     * SetShockData(4, 13, size, at, size / 2, size). It sits after the bind's
     * bail, so only an explosion that is really there is felt.
     */
    q2_shock_explosion(at, size);

    return e;
}

/* ------------------------------------------------------------------------- */
void q2_model_ent_think(q2_entity *e, q2_entity_world *w)
{
    s32 life;

    if (!e || !w)
        return;

    /*
     * `ent[+0x100] += 2 * dt` — 0x8005A618 doubles the delta BEFORE the add,
     * so this clock runs at twice an item's. And it does not wrap: an item
     * subtracts clip_length in a loop (item.c) and this runs straight past the
     * end into the removal below.
     */
    e->frame += Q2_MODEL_ENT_CLOCK_RATE * w->dt;

    life = q2_model_ent_lifetime(e->clip_length);

    /*
     * 0x8005A63C: `slt` on the SIGNED halfword, so the test is "still short of
     * the end" and the removal is the else. A clip_length of zero therefore
     * removes the entity on its first think rather than leaving it for ever,
     * which is the right answer for a model the bank does not animate.
     */
    if (e->frame >= life) {
        q2_entity_remove(e);           /* 0x8006D280 */
        return;
    }

    /* +0xFE, the second lighting-intensity factor at 0x8006B298/0x8006B468. */
    e->fade = (s16)q2_model_ent_scale(e->frame);

    /*
     * 0x8005A6E4..0x8005A764 builds a WORLD DYNAMIC LIGHT and calls
     * 0x80075C34. The outer radius is the second ramp; the inner is exactly
     * three quarters of it. The packed colour at 0x800AEAD4 is C0/40/31 and
     * both style fields supplied from 0x800AEAB4 are zero.
     *
     * The game layer records the call as an event because the renderer owns
     * the sixteen-entry runtime-light list. `client_drain_entity_events` turns
     * this back into the same q2_light_add_dynamic call later in the frame.
     */
    e->flash = q2_model_ent_flash(e->frame);
    q2_ent_light_at(&w->events, e->origin, g_explosion_light,
                    (e->flash * 3) / 4, e->flash);
}

/* ========================================================================= */
/* GIBS                                                                      */
/* ========================================================================= */

/*
 * The 36 words at 0x800AD648, index `class - 2`, read one at a time
 * (`q2psx-inspect bytes 0x800AD648 144`). Six targets, and the default
 * 0x8007D0F8 in every word not marked. Class names are `q2psx-inspect
 * classes`; a blank name is a class number with no row.
 */
static const u8 k_arm_by_class[Q2_GIB_TABLE_SIZE] = {
    Q2_GIB_ARM_BOS2,    /*  2 Boss2       0x8007CF14 */
    Q2_GIB_ARM_CHEST,   /*  3                        */
    Q2_GIB_ARM_CHEST,   /*  4                        */
    Q2_GIB_ARM_CHEST,   /*  5 Ironmaiden             */
    Q2_GIB_ARM_CHEST,   /*  6 Flipper                */
    Q2_GIB_ARM_CHEST,   /*  7                        */
    Q2_GIB_ARM_BOS2,    /*  8 Flyer       0x8007CF14 */
    Q2_GIB_ARM_CHEST,   /*  9 Gladiator              */
    Q2_GIB_ARM_CHEST,   /* 10 Gunner                 */
    Q2_GIB_ARM_BOS2,    /* 11 Hover       0x8007CF14 */
    Q2_GIB_ARM_CHEST,   /* 12 Infantry               */
    Q2_GIB_ARM_JORG,    /* 13 Jorg        0x8007D054 */
    Q2_GIB_ARM_NONE,    /* 14 Rider       0x8007D140 */
    Q2_GIB_ARM_CHEST,   /* 15 Medic                  */
    Q2_GIB_ARM_CHEST,   /* 16                        */
    Q2_GIB_ARM_MEAT,    /* 17 Parasite    0x8007D0BC */
    Q2_GIB_ARM_CHEST,   /* 18 Soldier                */
    Q2_GIB_ARM_CHEST,   /* 19 Soldier                */
    Q2_GIB_ARM_CHEST,   /* 20 Soldier                */
    Q2_GIB_ARM_CHEST,   /* 21                        */
    Q2_GIB_ARM_CHEST,   /* 22                        */
    Q2_GIB_ARM_CHEST,   /* 23                        */
    Q2_GIB_ARM_BOS1,    /* 24 Boss1       0x8007CF84 */
    Q2_GIB_ARM_CHEST,   /* 25 Tankcomm               */
    Q2_GIB_ARM_CHEST,   /* 26                        */
    Q2_GIB_ARM_CHEST,   /* 27 DeadComm               */
    Q2_GIB_ARM_CHEST,   /* 28 Arachner               */
    Q2_GIB_ARM_CHEST,   /* 29 Blitz                  */
    Q2_GIB_ARM_CHEST,   /* 30                        */
    Q2_GIB_ARM_CHEST,   /* 31 Flamer                 */
    Q2_GIB_ARM_STRID,   /* 32 Strider     0x8007CFE0 */
    Q2_GIB_ARM_CHEST,   /* 33                        */
    Q2_GIB_ARM_CHEST,   /* 34 Insane                 */
    Q2_GIB_ARM_CHEST,   /* 35 Rider2                 */
    Q2_GIB_ARM_CHEST,   /* 36 Rider3                 */
    Q2_GIB_ARM_NONE     /* 37 RiderStand  0x8007D140 */
};

/*
 * The four named rings, exactly as their arms load a3 and sp+16/20/24 —
 * 0x8007CF60..0x8007D0B0 for the Bos2 arm (the high bound is the delay-slot
 * store at 0x8007D0B0, shared by all four), 0x8007CFBC.., 0x8007D038..,
 * 0x8007D08C... StridGib's digit is 8 because its name is one letter longer.
 */
typedef struct gib_ring {
    const char *name;
    s32         digit;
    s32         low;
    s32         high;
    bool        explosion;   /* the arm calls 0x8005A778 before the ring */
} gib_ring;

static const gib_ring k_ring[Q2_GIB_ARM_COUNT] = {
    [Q2_GIB_ARM_BOS2]  = { "Bos2Gib1",  7, 1, 8, true  },   /* 0x800AD5CC */
    [Q2_GIB_ARM_BOS1]  = { "Bos1Gib1",  7, 1, 8, false },   /* 0x800AD5D8 */
    [Q2_GIB_ARM_STRID] = { "StridGib1", 8, 1, 7, true  },   /* 0x800AD5E4 */
    [Q2_GIB_ARM_JORG]  = { "JorgGib1",  7, 1, 9, false },   /* 0x800AD5F0 */
};

/* 0x8005B320's forty-five: fifteen velocity triples, drawn before its
 * emptiness test (effect.h, q2_fx_mesh_blood), and the trail's the same. */
#define GIB_BURST_DRAWS (Q2_FX_GROUP_QUADS * 3)

/* ------------------------------------------------------------------------- */
/* The world and the bodies                                                   */
/* ------------------------------------------------------------------------- */
typedef struct gib_rec {
    const q2_entity_set *set;
    u32                  slot;
    bool                 used;
    q2_gib_body          body;
} gib_rec;

static gib_rec      g_gib_rec[Q2_GIB_BODY_MAX];
static q2_gib_world g_gib_world;
static bool         g_gib_attached;

q2_gib_stats q2_gib_counters;

void q2_gib_attach(const q2_gib_world *w)
{
    if (!w) {
        memset(&g_gib_world, 0, sizeof(g_gib_world));
        memset(g_gib_rec, 0, sizeof(g_gib_rec));
        g_gib_attached = false;
        return;
    }

    if (!g_gib_attached || g_gib_world.set != w->set)
        memset(g_gib_rec, 0, sizeof(g_gib_rec));

    g_gib_world    = *w;
    g_gib_attached = true;
}

const q2_gib_world *q2_gib_attached(void)
{
    return g_gib_attached ? &g_gib_world : NULL;
}

static bool rec_is(const gib_rec *r, const q2_entity *e)
{
    return r->used && r->set && r->set->ent && r->slot < r->set->count &&
           &r->set->ent[r->slot] == e;
}

/* A record whose entity is still a flying gib. One whose slot was freed, or
 * reused, or has gone over to 0x8005B358 (which moves nothing) is spare. */
static bool rec_live(const gib_rec *r)
{
    const q2_entity *owner;

    if (!r->used || !r->set || !r->set->ent || r->slot >= r->set->count)
        return false;
    owner = &r->set->ent[r->slot];
    return owner->in_use && owner->think == q2_gib_think;
}

static bool rec_available(void)
{
    u32 i;

    for (i = 0; i < Q2_GIB_BODY_MAX; i++)
        if (!rec_live(&g_gib_rec[i]))
            return true;
    return false;
}

static gib_rec *rec_claim(const q2_entity_set *set, u32 slot)
{
    u32 i;

    for (i = 0; i < Q2_GIB_BODY_MAX; i++) {
        gib_rec *r = &g_gib_rec[i];

        if (r->used && r->set == set && r->slot == slot)
            return r;
    }
    for (i = 0; i < Q2_GIB_BODY_MAX; i++)
        if (!rec_live(&g_gib_rec[i]))
            return &g_gib_rec[i];
    return NULL;
}

q2_gib_body *q2_gib_body_of(const q2_entity *e)
{
    u32 i;

    if (!e)
        return NULL;
    for (i = 0; i < Q2_GIB_BODY_MAX; i++)
        if (rec_is(&g_gib_rec[i], e))
            return &g_gib_rec[i].body;
    return NULL;
}

bool q2_gib_body_restore(const q2_entity_set *set, u32 slot, const q2_gib_body *body)
{
    gib_rec *r;
    if (!set || slot >= set->count || !body) return false;
    r = rec_claim(set, slot);
    if (!r) return false;
    r->set = set; r->slot = slot; r->used = true; r->body = *body;
    return true;
}

void q2_gib_parent_init(q2_gib_parent *p)
{
    if (!p)
        return;
    memset(p, 0, sizeof(*p));

    /*
     * The allocator's ambient, 40 40 40 from 0x800AEB84 (0x8006C1D8..
     * 0x8006C1FC): what +0x2AC holds on a record nobody has written it on,
     * and so what that record's chunks inherit at 0x8005A344.
     */
    p->glow[0] = p->glow[1] = p->glow[2] = Q2_MODEL_ENT_AMBIENT;
}

/* ------------------------------------------------------------------------- */
/* Small pieces                                                               */
/* ------------------------------------------------------------------------- */
static void burn(q2_rng *rng, u32 n)
{
    while (rng && n--)
        (void)q2_rng_next(rng);
}

/* 0x8005A450..0x8005A480, and the same six instructions twice more. */
static s32 gib_jitter(q2_rng *rng)
{
    /*
     * `addiu -16384`, then `sll 3; addu; sll 4; subu; sll 1` — that is 286x,
     * not 143x twice — then `bgez; addiu 16383; sra 14`: the bias goes on the
     * NEGATIVE products only, so the divide rounds toward zero. A plain shift
     * would floor every negative draw a unit further out.
     */
    s32 v = Q2_GIB_JITTER * (q2_rng_next(rng) - 16384);

    if (v < 0)
        v += 16383;
    return v >> 14;
}

/* A PRIMARY cell's byte +32 — the `lbu 32` at 0x8005A398, 0x80046B04 and
 * 0x80054E88, each off [0x800C8E94] + 36 * cell — or `keep` when there is no
 * hull or no cell. The console indexes a -1 cell anyway, one record before the
 * table; the port does not read that. */
static u8 gib_cell_byte(const q2_gib_world *w, s32 cell, u8 keep)
{
    q2_coll_node cn;

    if (w && w->coll && cell >= 0 &&
        q2_collision_get_node(w->coll, (u32)cell, &cn))
        return cn.contents;
    return keep;
}

/*
 * 0x8004E920(parent, point, &cell): clip the point into PRIMARY collision
 * and report the cell it ended in. It calls 0x80055054(point, parent+0x54,
 * parent+0xA2), which with a -1 cell locates the parent by brute force
 * (0x80044F54, a3 = 1) and moves from the parent's +0x54 to the point
 * (0x80044C44); the clipped point comes back from [0x800C8EB8] and the cell
 * from [0x800C8EAA]. With a known +0xA2 the console seeds the start from the
 * SecondaryCol record instead — the port's parents carry none, so the -1 arm
 * is the one taken.
 */
static s32 gib_clip(const q2_gib_world *w, const q2_gib_parent *p, s32 pt[3])
{
    s32 end[3], node = -1;
    int k;

    if (!w->coll)
        return -1;      /* no hull: the point stands and names no cell */

    (void)q2_coll_move(w->coll, p->pos, pt, -1, end, &node);
    for (k = 0; k < 3; k++)
        pt[k] = end[k];
    return node;
}

/* ------------------------------------------------------------------------- */
/* The arm, and the throw arithmetic                                          */
/* ------------------------------------------------------------------------- */
q2_gib_arm q2_gib_arm_for_class(u16 cls, u16 was_class)
{
    u16 c = cls;
    s32 idx;

    /* 0x8007CEC8..0x8007CEE0: `lh` +0xD2 against 47, and on a match `lhu`
     * +0xDA instead — a corpse is taken apart as what it was. */
    if (c == Q2_GIB_CORPSE_CLASS)
        c = was_class;

    /* `addiu -2`, `sll 16; sra 16`, then `sltiu 36` — UNSIGNED, so a class
     * below 2 wraps huge and joins everything past 37 on the default. */
    idx = (s16)(c - 2);
    if ((u32)idx >= Q2_GIB_TABLE_SIZE)
        return Q2_GIB_ARM_CHEST;
    return (q2_gib_arm)k_arm_by_class[idx];
}

void q2_gib_throw_velocity(q2_gib_style style, s32 yaw, s32 r_speed,
                           s32 r_toss, const s16 impulse[3], s16 out[3])
{
    s16 imp[3] = { 0, 0, 0 };
    s32 speed, t, c, s;
    int k;

    if (!out)
        return;

    /* 0x8005ADF4 overwrites a2 before anything reads it: the boss twin has
     * no impulse to add. */
    if (style == Q2_GIB_STYLE_MEAT && impulse)
        for (k = 0; k < 3; k++)
            imp[k] = impulse[k];

    if (style == Q2_GIB_STYLE_BOSS) {
        /* 0x8005AEF8..0x8005AF20: `sra 6` (its +63 arm is for a negative
         * draw, which rand() never returns), plus 768 — 768..1279. */
        t = r_speed;
        if (t < 0)
            t += 63;
        speed = 768 + (t >> 6);
    } else {
        /* 0x8005A218..0x8005A24C: `sll 1; addu; sll 8` is 768r, `sra 15`
         * behind the same never-taken fixup, plus 256 — 256..1023. */
        t = r_speed * 3 * 256;
        if (t < 0)
            t += 32767;
        speed = 256 + (t >> 15);
    }

    /* `lh 2` (cos) and `lh 0` (sin) of the 0x800A5430 pair at `yaw & 0xFFF`,
     * each product rounded toward zero by 4096 (`bgez; addiu 4095; sra 12`,
     * 0x8005A258 and 0x8005A2B8). */
    c = q2_cos12(yaw & 0xFFF) * speed;
    if (c < 0)
        c += 4095;
    c >>= 12;
    s = q2_sin12(yaw & 0xFFF) * speed;
    if (s < 0)
        s += 4095;
    s >>= 12;

    /* Each lands through `lhu` + `addu` + `sh`: a halfword add. */
    out[0] = (s16)(imp[0] + c);
    out[2] = (s16)(imp[2] + s);

    if (style == Q2_GIB_STYLE_BOSS) {
        /* 0x8005AF44..0x8005AF6C: `-r; sll 5; subu; sll 2; addu; sll 4` is
         * -2000r, toward zero by 32768, then -3000 — -4999..-3000, UP. */
        t = -2000 * r_toss;
        if (t < 0)
            t += 32767;
        out[1] = (s16)((t >> 15) - 3000);
    } else {
        /* 0x8005A278..0x8005A2A4: `-r; sll 1; addu; sll 9` is -1536r, toward
         * zero by 32768, plus impulse.y - 3072 — -4607..-3072 at rest. */
        t = -1536 * r_toss;
        if (t < 0)
            t += 32767;
        out[1] = (s16)(imp[1] - 3072 + (t >> 15));
    }
}

/* ------------------------------------------------------------------------- */
/* One chunk — 0x8005A0AC and 0x8005AD8C                                      */
/* ------------------------------------------------------------------------- */
q2_entity *q2_gib_spawn_one(const q2_gib_world *w, const q2_gib_parent *parent,
                            q2_gib_style style, s32 yaw, const s16 impulse[3],
                            const s32 pos[3], s32 cell, s32 lifetime,
                            const char *name)
{
    char buf[Q2_GIB_NAME_LEN + 1];
    s32 height = 0, clip = 0, index = -1, life;
    s32 r_speed, r_toss;
    q2_entity *e;
    gib_rec *r;
    u32 slot;
    int k;

    if (!w || !w->set || !w->rng || !parent || !pos || !name)
        return NULL;

    /*
     * 0x8006C098 with a0 = 0 (0x8005A0E0 / 0x8005ADB8): NO eviction. A full
     * pool returns null and 0x8005A0F8 leaves before the first rand(), so a
     * refused chunk costs the generator nothing. The port's refusal is the
     * body table (see Q2_GIB_BODY_MAX), tested before the allocation so a
     * refused chunk does not leave a dead slot behind either.
     */
    if (!rec_available()) {
        q2_gib_counters.refused++;
        return NULL;
    }
    e = q2_entity_alloc(w->set);
    if (!e)
        return NULL;
    slot = (u32)(e - w->set->ent);
    r = rec_claim(w->set, slot);
    if (!r) {
        q2_entity_remove(e);
        q2_gib_counters.refused++;
        return NULL;
    }
    memset(r, 0, sizeof(*r));
    r->set  = w->set;
    r->slot = slot;
    r->used = true;

    /* 0x8005ADD8 `sh 48, 210(s0)` is the boss twin's alone; 0x8005A0AC
     * leaves +0xD2 at the allocator's zero, which the port spells
     * Q2_ENT_KIND_MODEL as the explosion does (entity.h). */
    e->kind  = (style == Q2_GIB_STYLE_BOSS) ? Q2_GIB_BOSS_CLASS
                                            : Q2_ENT_KIND_MODEL;
    e->think = q2_gib_think;                /* 0x8005A10C / 0x8005ADEC */

    /*
     * The twelve name bytes, reassembled into three words for 0x8006D008
     * (0x8005A110..0x8005A18C), and the result stored at +0x10 UNTESTED: an
     * unknown name is an entity with no model, not a refused spawn. That is
     * the asymmetry with 0x8005A778, and it is what makes `JorgGib:` a real,
     * invisible, bleeding entity rather than a gap in the ring. The name is
     * kept either way, so a draw with another bank can still resolve it.
     */
    memset(buf, 0, sizeof(buf));
    for (k = 0; k < Q2_GIB_NAME_LEN && name[k]; k++)
        buf[k] = name[k];
    snprintf(e->model, sizeof(e->model), "%s", buf);
    if (q2_model_ent_height(w->bank, buf, &height, &clip, &index)) {
        e->model_index = index;
        e->model_bank  = w->bank;
        e->clip_length = clip;
    } else {
        q2_gib_counters.unbound++;
    }
    e->model_offset = (s16)height;          /* 0x8006D100, 0 with no model */

    e->render_flags = Q2_GIB_RENDER_FLAGS;  /* 0x8005A1A4 / 0x8005A1A8 */

    for (k = 0; k < 3; k++) {               /* 0x8005A1B0..0x8005A1DC */
        e->origin[k] = pos[k];
        e->pos[k]    = pos[k];
    }

    /*
     * 0x8005A1E0..0x8005A20C write 0x800AEAC8 (40 40 40) into +0x2B0 and
     * +0x2AC, and 0x8005A338..0x8005A35C copy the PARENT's two words over
     * both before anything reads them. The first write is dead; only the
     * parent's colour is written here, and that is why.
     */
    for (k = 0; k < 3; k++)
        e->glow[k] = parent->glow[k];

    /* The throw — two draws, in this order. */
    r_speed = q2_rng_next(w->rng);          /* 0x8005A210 / 0x8005AEF0 */
    r_toss  = q2_rng_next(w->rng);          /* 0x8005A270 / 0x8005AF3C */
    q2_gib_throw_velocity(style, yaw, r_speed, r_toss, impulse, r->body.vel);

    /*
     * The spin: three raw draws into +0xE6, +0xE8, +0xEA. The `sh` beside
     * each `jal rand` is in its DELAY SLOT and so stores the PREVIOUS call's
     * result — which lines the three stores up with the three draws in order
     * rather than skewing them. Raw 0..32767, not centred.
     */
    e->angles[0] = q2_rng_next(w->rng);     /* 0x8005A2D0 */
    e->angles[1] = q2_rng_next(w->rng);     /* 0x8005A2D8 */
    e->angles[2] = q2_rng_next(w->rng);     /* 0x8005A2E0 */

    /* 0x8005A304..0x8005A320: +0x44 = (rand() - 16384) >> 6, -256..255. */
    r->body.spin = (q2_rng_next(w->rng) - 16384) >> 6;

    /* +0xF4, `sh s6, 244(s0)` (0x8005A37C); the boss twin adds `rand() >> 5`
     * first (0x8005B010..0x8005B034), 512..1535 for the ring's 512. */
    life = lifetime;
    if (style == Q2_GIB_STYLE_BOSS) {
        s32 j = q2_rng_next(w->rng);

        if (j < 0)
            j += 31;
        life = lifetime + (j >> 5);
    }

    e->node      = -1;                      /* +0xA2, 0x8005A360 */
    e->remove_in = (s16)life;
    r->body.cell = cell;                    /* +0xA0, 0x8005A380 */

    /* +0x9E = byte +32 of [0x800C8E94] + 36 * cell (0x8005A384..0x8005A3A4).
     * The console indexes whatever the cell is; the port reads a real one. */
    e->surface = gib_cell_byte(w, cell, 0);

    /* 0x8005555C(+0x54, 20|20, 20): mins -20, maxs +20 at +0x6C..+0x76, in
     * the entity frame as a transient's box is kept (entitydraw.c). */
    for (k = 0; k < 3; k++) {
        e->bounds_min[k] = -Q2_GIB_HALF_EXTENT;
        e->bounds_max[k] =  Q2_GIB_HALF_EXTENT;
    }

    e->hidden = false;
    q2_gib_counters.chunks++;
    return e;
}

/* ------------------------------------------------------------------------- */
/* ThrowGibs — 0x8005A3D4                                                     */
/* ------------------------------------------------------------------------- */
static void gib_spray(const q2_gib_world *w, const q2_gib_parent *p)
{
    /* 0x8005A440 `jal 0x8005B320`. With no particle pool the console's
     * forty-five draws are still made: the generator is shared, and a missing
     * pool in the port must not move every draw after it. */
    if (w->fx)
        (void)q2_fx_gib_spray(w->fx, w->rng, p->mesh, p->area);
    else
        burn(w->rng, GIB_BURST_DRAWS);
}

u32 q2_gib_throw_meat(const q2_gib_world *w, const q2_gib_parent *parent,
                      s32 count, const s16 impulse[3], const char *head_name)
{
    s32 yaw, step, cell = -1, i;
    u32 made = 0;

    if (!w || !w->set || !w->rng || !parent)
        return 0;

    /* The yaw FIRST (0x8005A404), then the step (0x8005A414). */
    yaw = q2_rng_next(w->rng);
    if (count == 0)
        return 0;       /* 0x8005A420 `break 0x1C00`; no caller passes 0 */
    step = Q2_GIB_YAW_CIRCLE / count;

    gib_spray(w, parent);

    for (i = 0; i < count; i++) {
        s32 pt[3];

        /* x, y, z — sp+40, +44, +48 — each off the parent's +0x54. */
        pt[0] = parent->pos[0] + gib_jitter(w->rng);
        pt[1] = parent->pos[1] + gib_jitter(w->rng);
        pt[2] = parent->pos[2] + gib_jitter(w->rng);
        cell  = gib_clip(w, parent, pt);    /* 0x8005A504 */

        /* 0x8005A564, with `s1 += s5` in its delay slot AFTER a1 took s1. */
        if (q2_gib_spawn_one(w, parent, Q2_GIB_STYLE_MEAT, yaw, impulse, pt,
                             cell, Q2_GIB_LIFE_MEAT, Q2_GIB_MEAT_NAME))
            made++;
        yaw += step;
    }

    /*
     * 0x8005A578: the head, from +0xA4 (`addiu a3, a0, 164`, the delay slot
     * of 0x8005A5C8) — the parent's DRAW ORIGIN, not a jittered point — at
     * the yaw the ring ended on and with sp+56 as the last clip left it.
     */
    if (head_name &&
        q2_gib_spawn_one(w, parent, Q2_GIB_STYLE_MEAT, yaw, impulse,
                         parent->origin, cell, Q2_GIB_LIFE_HEAD, head_name))
        made++;

    return made;
}

/* ------------------------------------------------------------------------- */
/* The boss ring — 0x8005B0AC                                                 */
/* ------------------------------------------------------------------------- */
u32 q2_gib_throw_named(const q2_gib_world *w, const q2_gib_parent *parent,
                       s32 count, const s16 impulse[3],
                       const char *name_template, s32 digit, s32 low,
                       s32 high)
{
    char name[Q2_GIB_NAME_LEN + 1];
    s32 yaw, step, ch, i;
    u32 made = 0;
    int k;

    if (!w || !w->set || !w->rng || !parent || !name_template)
        return 0;

    /* A local copy stands in for the .data template the console writes into;
     * see modelent.h for why the difference cannot be seen. */
    memset(name, 0, sizeof(name));
    for (k = 0; k < Q2_GIB_NAME_LEN && name_template[k]; k++)
        name[k] = name_template[k];

    ch  = low;                              /* 0x8005B0F8, a delay slot */
    yaw = q2_rng_next(w->rng);              /* 0x8005B0F4 */
    if (count == 0)
        return 0;                           /* 0x8005B110 `break 0x1C00` */
    step = Q2_GIB_YAW_CIRCLE / count;       /* 0x8005B104 */

    /* NO SPRAY. There is no 0x8005B320 call in this function. */
    for (i = 0; i < count; i++) {
        s32 pt[3], cell;

        pt[0] = parent->pos[0] + gib_jitter(w->rng);
        pt[1] = parent->pos[1] + gib_jitter(w->rng);
        pt[2] = parent->pos[2] + gib_jitter(w->rng);
        cell  = gib_clip(w, parent, pt);    /* 0x8005B1E8 */

        /*
         * 0x8005B1F0 `addiu v1, s0, 49` (bytes 31 00 03 26) and 0x8005B200
         * `sb v1, 0(name + digit)`: the character is '1' + i, so with low = 1
         * the first name thrown ends in '2' and the template's own '1' never
         * is. Then 0x8005B1F8 `s0++` and 0x8005B204 `slt v0, fp, s0`: past
         * `high` it goes back to `low`. For Jorg (1..9) that makes one piece
         * in nine `JorgGib:`, which no CastList carries.
         */
        if (digit >= 0 && digit < Q2_GIB_NAME_LEN)
            name[digit] = (char)('1' + ch);
        ch++;
        if (high < ch)
            ch = low;

        /* 0x8005B264, a1 = the yaw before 0x8005B218 steps it. */
        if (q2_gib_spawn_one(w, parent, Q2_GIB_STYLE_BOSS, yaw, impulse, pt,
                             cell, Q2_GIB_LIFE_RING, name))
            made++;
        yaw += step;
    }

    return made;
}

/* ------------------------------------------------------------------------- */
/* The dispatcher — 0x8007CEB4                                                */
/* ------------------------------------------------------------------------- */
u32 q2_gib_destroy(const q2_gib_world *w, const q2_gib_parent *parent,
                   q2_gib_report *out)
{
    q2_gib_report rep;
    s16 impulse[3];
    int k;

    memset(&rep, 0, sizeof(rep));
    rep.arm = Q2_GIB_ARM_NONE;

    if (w && parent) {
        q2_gib_counters.destroyed++;
        rep.arm = q2_gib_arm_for_class(parent->cls, parent->was_class);

        /* 0x8007CF30..0x8007CF68: `lhu` +0xE0, `lhu` +0x2F8, `addu`, `sh` —
         * the velocity and the knockback as halfwords, per axis. */
        for (k = 0; k < 3; k++)
            impulse[k] = (s16)(u16)((u32)(u16)parent->velocity[k] +
                                    (u32)(u16)parent->knockback[k]);

        switch (rep.arm) {
        case Q2_GIB_ARM_CHEST:              /* 0x8007D0F8..0x8007D138 */
            rep.chunks = q2_gib_throw_meat(w, parent, Q2_GIB_COUNT, impulse,
                                           Q2_GIB_CHEST_NAME);
            break;

        case Q2_GIB_ARM_MEAT:               /* 0x8007D0BC, a3 = 0 at 0x8007D0F4 */
            rep.chunks = q2_gib_throw_meat(w, parent, Q2_GIB_COUNT, impulse,
                                           NULL);
            break;

        case Q2_GIB_ARM_BOS2:
        case Q2_GIB_ARM_BOS1:
        case Q2_GIB_ARM_STRID:
        case Q2_GIB_ARM_JORG: {
            const gib_ring *ring = &k_ring[rep.arm];

            /* 0x8007CF20 / 0x8007D020 `jal 0x8005A778(+0x54, +0x9E, 2048, 0)`
             * — the Explosion model, before any chunk. It draws nothing from
             * the generator, so its place in the order is only the pool's. */
            if (ring->explosion)
                rep.explosion = q2_model_ent_spawn(w->set, w->bank,
                                                   Q2_MODEL_ENT_EXPLOSION,
                                                   parent->pos,
                                                   parent->area,
                                                   Q2_MODEL_ENT_SIZE_GIB,
                                                   w->sounds)
                                != NULL;

            rep.chunks = q2_gib_throw_named(w, parent, Q2_GIB_RING_COUNT,
                                            impulse, ring->name, ring->digit,
                                            ring->low, ring->high);
            break;
        }

        case Q2_GIB_ARM_NONE:               /* straight to 0x8007D140 */
        default:
            break;
        }
    }

    /* 0x8007D140 `jal 0x8006D280(self)` is the CALLER's: the record being
     * freed is a q2_monster or a player's death state, not a q2_entity. */
    if (out)
        *out = rep;
    return rep.chunks;
}

/* ------------------------------------------------------------------------- */
/* The think — 0x80059DE0, and the toss it opens with                         */
/* ------------------------------------------------------------------------- */
/*
 * 0x800463E8 as 0x80046B98 calls it: PRIMARY collision (0x80046CE0 stores
 * 0x800C8E90 at gp+17676), the +0xA0 cell (0x80046CE8), and a rebound of 2304.
 * Returns whether anything was touched — the mover's s3.
 */
static bool gib_move(const q2_gib_world *g, q2_entity *e, q2_gib_body *b,
                     s32 dt)
{
    s32 start[3], dest[3], end[3];
    s16 delta[3];
    s16 normal[3] = { 0, 0, 0 };
    s32 gravity = g->gravity ? *g->gravity : Q2_GRAVITY;
    bool contact = false;
    int k;

    for (k = 0; k < 3; k++)
        start[k] = e->pos[k];

    /* 0x80046450..0x800464A0: `lhu`, add `[0x800AE924] * dt`, `sh`, then
     * `slti 8193` on the sign-extended result — a gib carries no 0x2000. */
    if (!(e->render_flags & Q2_ITEM_TOSS_NO_GRAVITY)) {
        s16 vy = (s16)(u16)((u32)(u16)b->vel[1] + (u32)(gravity * dt));

        if (vy > Q2_ITEM_TOSS_TERMINAL)
            vy = Q2_ITEM_TOSS_TERMINAL;
        b->vel[1] = vy;
    }

    /* 0x800464F8..0x80046504: +0x98 &= ~0x20 before the move. */
    b->move_flags &= ~Q2_GIB_MOVE_REST;

    /* 0x800464D4..0x800465B8: `vel * dt / 320`, truncated, as a halfword. */
    for (k = 0; k < 3; k++) {
        delta[k] = (s16)(((s32)b->vel[k] * dt) / Q2_ITEM_TOSS_STEP_DIV);
        dest[k]  = start[k] + delta[k];
        end[k]   = dest[k];
    }

    /*
     * 0x800465BC `jal 0x80053974` — the ENTITY boxes first, doors and lifts,
     * a bare point against every active one. A hit is a contact (0x800465E4)
     * with that face's normal, and the destination is pulled back by the
     * whole step (0x80046650..0x8004667C) before the hull move below. The
     * same reading item.c's death drop makes of the same instructions; the
     * trigger arm between them is off for a gib (modelent.h).
     */
    if (g->ents) {
        q2_move_seg_hit hit;

        if (q2_move_clip_segment(g->ents, start, dest, NULL, &hit)) {
            contact = true;
            for (k = 0; k < 3; k++) {
                normal[k] = hit.normal[k];
                dest[k]   = hit.pos[k] - delta[k];
                end[k]    = dest[k];
            }
        }
    }

    if (g->coll) {
        s32 node = b->cell;

        if (!q2_coll_move(g->coll, start, dest, b->cell, end, &node)) {
            q2_coll_plane pl;

            contact = true;
            if (g->coll->hit_plane_index >= 0 &&
                q2_collision_get_plane(g->coll, (u32)g->coll->hit_plane_index,
                                       &pl)) {
                normal[0] = pl.nx;
                normal[1] = pl.ny;
                normal[2] = pl.nz;
            }
        }

        /* 0x80046ACC..0x80046B0C: +0xA0 takes the move's last cell and +0x9E
         * that cell's byte +32 — contact or not, and BEFORE the think's trail
         * reads +0x9E. */
        b->cell    = node;
        e->surface = gib_cell_byte(g, node, e->surface);
    }

    if (!contact) {
        for (k = 0; k < 3; k++)             /* 0x80046AB4 */
            e->pos[k] = dest[k];
        return false;
    }

    {
        /*
         * THE REBOUND, 0x800466C0..0x80046A5C, which the death drop's call
         * (a2 = 0) multiplies away and this one does not.
         *
         *   speed = SquareRoot0(|v|^2) * 2304 >> 12        0x800466F4..0x80046710
         *   hit   = where the trace stopped, less 0x8005625C's unit push
         *   r     = the intended end - hit                 the part not travelled
         *   refl  = end + round(n * (-2 n.r) / 2^24)       r mirrored in the plane
         *   +0x54 = hit                                    0x800469D8
         *   vel   = VectorNormal(refl - hit) * speed >> 12 0x80046A14..0x80046A5C
         *
         * The squares are `mult`/`mflo` summed in 32 bits, as item.c's rest
         * test sums them. The root is the SDK's SquareRoot0 (0x8008A7E8,
         * `jal` at 0x800466F4) and the direction its table-driven VectorNormal
         * (0x8008A588, `jal` at 0x80046A14) — the same mover the launcher and
         * hand grenades take, whose 2,592 native toss cases pass through these
         * two routines. An exact root and `dir * 4096 / |dir|` used to stand
         * in for them; the quantized root of 3200^2 is 3197, so a rebound kept
         * 1800 where the console keeps 1798, and the gib's rest tick and trail
         * draws drifted from there. A stop the port cannot read a plane for (a
         * start outside every cell) has a zero normal: no push and no mirror,
         * so the gib keeps its heading at 2304/4096 of its speed, stays where
         * it was, and so runs down to rest in place over a few ticks rather
         * than escaping the hull. That guard is the port's.
         */
        u32 v2 = (u32)((s32)b->vel[0] * b->vel[0]) +
                 (u32)((s32)b->vel[1] * b->vel[1]) +
                 (u32)((s32)b->vel[2] * b->vel[2]);
        s32 speed = (s32)(q2_sdk_sqrt0(v2) * Q2_GIB_RESTITUTION) >> 12;
        s16 push[3] = { 0, 0, 0 };
        s32 hit[3], rem[3], dot2, dir[3], unit[3];

        q2_move_push_vector(normal, push);
        if (!normal[0] && !normal[1] && !normal[2])
            push[0] = push[1] = push[2] = 0;

        for (k = 0; k < 3; k++) {
            hit[k] = end[k] - push[k];      /* 0x800467D8..0x80046820 */
            rem[k] = dest[k] - hit[k];
        }

        /* 0x8004683C..0x80046878: -(n.r) doubled, in 32 bits. */
        dot2 = -((s32)normal[0] * rem[0] + (s32)normal[1] * rem[1] +
                 (s32)normal[2] * rem[2]) * 2;

        for (k = 0; k < 3; k++) {
            /* 0x80046884..0x800469C8: the 64-bit product, +0x800000, >> 24,
             * then added to the intended end. */
            s32 refl = dest[k] +
                       (s32)(((s64)normal[k] * dot2 + 0x800000) >> 24);

            e->pos[k] = hit[k];
            dir[k]    = refl - hit[k];
        }

        q2_sdk_vector_normal(dir, unit);
        for (k = 0; k < 3; k++)    /* `mult`/`mflo`, then `sra 12`, a floor */
            b->vel[k] = (s16)((s32)((u32)unit[k] * (u32)speed) >> 12);
    }

    return true;
}

static void gib_toss(const q2_gib_world *g, q2_entity *e, q2_gib_body *b,
                     s32 dt)
{
    int k;

    /*
     * 0x80046BB8..0x80046BC4: `andi 0x60` on +0x98 — at rest (or held) the
     * whole move is skipped and only the matrix is rebuilt (0x80046CB8),
     * which the port's draw does for itself. The second gate, [0x800AE8B4],
     * is the menu-open word, and the port does not run the entity sweep
     * behind an open menu at all.
     */
    if (b->move_flags & (Q2_GIB_MOVE_REST | Q2_GIB_MOVE_HELD))
        return;

    /*
     * 0x80046BE0..0x80046CA0: THE TUMBLE, three draws. `lhu +0xE6; andi 1`:
     * an even +0xE6 turns every angle back by 160 + 2 * (rand() & 31), an odd
     * one forward by the same — and each store is a halfword.
     */
    if (g->rng) {
        const bool back = ((u32)e->angles[0] & 1u) == 0;

        for (k = 0; k < 3; k++) {
            s32 turn = Q2_GIB_TUMBLE_BASE +
                       2 * (q2_rng_next(g->rng) & Q2_GIB_TUMBLE_MASK);

            e->angles[k] = (s16)(back ? e->angles[k] - turn
                                      : e->angles[k] + turn);
        }
    }

    /*
     * 0x80046CF8..0x80046D84, on what the mover returned: nothing touched
     * clears the rest bit; a touch at `|v|^2 > 0xC34FF` (`slt`, signed on the
     * 32-bit sum) clears it and bumps +0xE6 by ONE, which flips the tumble; a
     * slower touch SETS it, and from the next tick the gib neither moves nor
     * bleeds.
     */
    if (gib_move(g, e, b, dt)) {
        u32 v2 = (u32)((s32)b->vel[0] * b->vel[0]) +
                 (u32)((s32)b->vel[1] * b->vel[1]) +
                 (u32)((s32)b->vel[2] * b->vel[2]);

        if ((s32)Q2_GIB_REST_SPEED_SQ < (s32)v2) {
            b->move_flags &= ~Q2_GIB_MOVE_REST;
            e->angles[0]   = (s16)(e->angles[0] + 1);
        } else {
            b->move_flags |= Q2_GIB_MOVE_REST;
            q2_gib_counters.landed++;
        }
    } else {
        b->move_flags &= ~Q2_GIB_MOVE_REST;
    }
}

void q2_gib_think(q2_entity *e, q2_entity_world *w)
{
    const q2_gib_world *g = q2_gib_attached();
    q2_gib_body *b;
    s32 dt;
    int k;

    if (!e || !w)
        return;

    dt = w->dt;
    b  = q2_gib_body_of(e);

    /*
     * An entity with no body — one this module did not spawn, or one whose
     * record a detach forgot — has no velocity to fly or bleed with. It keeps
     * the clock and the expiry below and nothing else.
     */

    /* 0x80059E18 `jal 0x80046B98(ent, 0, 0, 0x0900)`. */
    if (b && g)
        gib_toss(g, e, b, dt);

    /*
     * 0x80059E20..0x80059E50: `lh +0xF4`, `slt dt, +0xF4` — once dt reaches
     * what is left, +0x3C becomes 0x8005B358 and +0xF4 dt + 1, so the
     * subtraction below leaves exactly one. 0x8005B358 is the item shrink
     * think: +0xFC falls by 16 * dt and the entity goes at zero.
     */
    if (!(dt < (s32)(s16)e->remove_in)) {
        e->think     = q2_item_shrink_think;
        e->remove_in = (s16)(dt + 1);
    }
    e->remove_in = (s16)(e->remove_in - dt);    /* 0x80059E54..0x80059E68 */

    for (k = 0; k < 3; k++)                     /* 0x80059E6C..0x80059E80 */
        e->origin[k] = e->pos[k];

    /*
     * 0x80059E84..0x80059E94: `srl 5; andi 1` on +0x98 — a gib at rest lays
     * no trail. Otherwise the fifteen-point line on the blood ramps through
     * the second spawner, from the origin just refreshed, in the area byte
     * the mover just wrote. Forty-five draws whether or not there is a pool.
     */
    if (b && g && !(b->move_flags & Q2_GIB_MOVE_REST)) {
        if (!g->fx)
            burn(g->rng, GIB_BURST_DRAWS);
        else if (q2_fx_gib_trail(g->fx, g->rng, e->origin, b->vel,
                                 e->surface) >= 0)
            q2_gib_counters.trails++;
    }

    /* 0x8005A088 `jal 0x80054DD4(+0x54, 2)`: +0x9E again, from +0xA0. The box
     * is kept relative (entitydraw.c adds the position), so that half is the
     * draw's. */
    if (b && g)
        e->surface = gib_cell_byte(g, b->cell, e->surface);
}

/* ------------------------------------------------------------------------- */
/* The two death sites                                                        */
/* ------------------------------------------------------------------------- */
u32 q2_gib_monster_corpse(const struct q2_monster *m)
{
    const q2_gib_world *w = q2_gib_attached();
    q2_gib_parent p;
    int k;

    if (!m || !w)
        return 0;

    q2_gib_parent_init(&p);

    /*
     * The corpse handler hands 0x8007CEB4 the ACTOR. What the port's creature
     * record can say about it:
     *
     *   +0x54 / +0xA4   the physical actor's origin, independently of the
     *                   AI edict's linked destination;
     *   +0xD2 / +0xDA   47 once detached, with the CLASS-TABLE row kept as
     *                   what it was. `pop_class_id` is that row; `class_id` is
     *                   the module's class byte (64..94) and would index the
     *                   dispatcher's table wrongly for every creature;
     *   +0xE0, +0x2F8   physical velocity and unconsumed impulse. The owner's
     *                   `describe` may supply a combat update from this frame.
     */
    p.placed = true;
    for (k = 0; k < 3; k++) {
        p.pos[k]    = q2_cre_origin(m)[k];
        p.origin[k] = q2_cre_origin(m)[k];
        p.velocity[k] = m->motion.velocity[k];
        p.knockback[k] = m->motion.impulse[k];
    }
    p.cls       = m->corpse ? Q2_GIB_CORPSE_CLASS : m->pop_class_id;
    p.was_class = m->pop_class_id;

    if (w->describe)
        w->describe(w->user, Q2_GIB_VICTIM_MONSTER, m, &p);
    if (!p.placed)
        return 0;

    return q2_gib_destroy(w, &p, NULL);
}

u32 q2_gib_player_body(const struct q2_player_death *d)
{
    const q2_gib_world *w = q2_gib_attached();
    q2_gib_parent p;
    int k;

    if (!d || !w)
        return 0;

    q2_gib_parent_init(&p);

    /*
     * The player's +0xD2 is 39 from its spawn (0x8003B2B0), which is past the
     * table and so the default arm: five `Gib meat` and a `Chest`. The death
     * state carries +0xE0 and nothing else of the body — WHERE it is belongs
     * to the sim — so the owner's `describe` must place it, and a player it
     * does not place throws nothing.
     */
    p.placed = false;
    p.cls    = Q2_GIB_PLAYER_CLASS;
    for (k = 0; k < 3; k++)
        p.velocity[k] = d->velocity[k];

    if (w->describe)
        w->describe(w->user, Q2_GIB_VICTIM_PLAYER, d, &p);
    if (!p.placed)
        return 0;

    return q2_gib_destroy(w, &p, NULL);
}
