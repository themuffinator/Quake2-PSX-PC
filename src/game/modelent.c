/*
 * modelent.c — 0x8005A778 and 0x8005A5F8. See modelent.h for the addresses and
 * for the three things about the think that a plausible version gets wrong.
 */
#include "modelent.h"

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
q2_entity *q2_model_ent_spawn(q2_entity_set *set, const q2_model_bank *bank,
                              q2_model_ent_kind kind, const s32 at[3],
                              u8 surface)
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
