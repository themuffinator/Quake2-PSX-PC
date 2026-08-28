#include "effect.h"

#include "fixed.h"
#include "hud.h"
#include "trig.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

static u32 bucket_for(const psx_ot *ot, u32 depth_sum, u32 corners,
                      s32 far_z);
static u32 sort_key_for(u32 depth_sum, u32 corners);

/*
 * `(a * b) >> 12`, rounding toward zero.
 *
 * The original's idiom is `bgez; addiu 4095; sra 12` — 0x8005644C and its
 * siblings — which is C's own division semantics rather than an arithmetic
 * shift's floor. The difference is one unit on negative results, and on a unit
 * vector's components that is the difference between a hull that closes and one
 * that leaks a pixel at the seam.
 */
static s32 mul12(s32 a, s32 b)
{
    s32 v = a * b;
    if (v < 0)
        v += 4095;
    return v >> 12;
}

/* `rand() - 16384`, arithmetic-shifted: the scatter draw every spawn site uses. */
static s32 draw(q2_rng *rng, int shift)
{
    return (q2_rng_next(rng) - 16384) >> shift;
}

static void vec_copy(s32 out[3], const s32 in[3])
{
    out[0] = in[0];
    out[1] = in[1];
    out[2] = in[2];
}

/* ------------------------------------------------------------------------- */
/* The presets                                                                */
/*                                                                            */
/* Each row is five immediates out of one spawn site. `size` is the value      */
/* passed BEFORE the level's size_scale divide, which is what the instruction  */
/* actually loads.                                                            */
/* ------------------------------------------------------------------------- */
static const q2_fx_preset k_preset[Q2_FX_PRESET_COUNT] = {
    /* count life size   shift ramp0 ramp1 rpt accY site        */
    { 15,    15,  8192,  9,    9,    9,    2,  0,  0x800486ECu },  /* EXPLOSION */
    { 15,    15,  6144,  10,   2,    3,    2,  2,  0x80048C08u },  /* BLOOD     */
    { 15,    15,  20000, 9,    11,   11,   3,  0,  0x8004BDC4u },  /* BFG_BURST */
    { 15,    10,  10000, 9,    1,    1,    1,  0,  0x800596B0u },  /* GIB       */
    { 15,     0,  0,     9,    1,    0,    1,  0,  0x80028DC8u },  /* SCRIPTED  */
    { 15,    25,  3072,  9,    0,    0,    4,  4,  0x8003E0C0u },  /* SPARK     */
    { 15,    15,  6144,  10,   1,    1,    1,  2,  0x80049074u }   /* LASER_END */
};

const q2_fx_preset *q2_fx_preset_at(q2_fx_preset_id id)
{
    if ((u32)id >= Q2_FX_PRESET_COUNT)
        return NULL;
    return &k_preset[id];
}

/*
 * Gib colour, 0x80059648.
 *
 * The burst's ramp is chosen from three bits of the creature's flag word, in
 * this order, and the chain has NO final else: a creature with none of the
 * three bits set reaches 0x80059680 with the register still holding whatever
 * the function last left in it. That is the same class of defect as the
 * uninitialised fifth argument documented for T_Damage in userfuncs.h, and it
 * is handled the same way — the port picks a defined value and says so, because
 * mirroring an uninitialised register is not reproducible.
 */
u8 q2_fx_gib_ramp(u32 creature_flags)
{
    if (creature_flags & Q2_FX_BLOOD_RED)
        return 1;    /* 0x8009BAE4 */
    if (creature_flags & Q2_FX_BLOOD_GREEN)
        return 11;   /* 0x8009C00C */
    if (creature_flags & Q2_FX_BLOOD_BLUE)
        return 0;    /* 0x8009BA60 */
    return 1;        /* DIVERGENCE: the original leaves this undefined. */
}

s32 q2_fx_gib(q2_fx_world *w, q2_rng *rng, const s32 at[3], u8 area,
              u32 creature_flags)
{
    const q2_fx_preset *p = q2_fx_preset_at(Q2_FX_GIB);
    const q2_fx_ramp *r;
    s16 vel[Q2_FX_GROUP_QUADS][3];
    u32 i;

    if (!w || !rng || !at || !p)
        return -1;

    for (i = 0; i < p->count; i++) {
        vel[i][0] = (s16)draw(rng, p->spread_shift);
        vel[i][1] = (s16)draw(rng, p->spread_shift);
        vel[i][2] = (s16)draw(rng, p->spread_shift);
    }

    r = q2_fx_ramp_at(w->tab, q2_fx_gib_ramp(creature_flags));
    return q2_fx_group_spawn(w, at, vel, p->count, r, r,
                             p->life, p->size, area);
}

/* ------------------------------------------------------------------------- */
/* The world                                                                  */
/* ------------------------------------------------------------------------- */
u32 q2_fx_budget(u32 group_count, u32 viewport_count)
{
    /*
     * 0x80030CB4. One viewport spends the whole pool; more than one halves the
     * pool first and then multiplies by the view count, so two views get the
     * same total as one and four get twice as much. That is not a typo in the
     * transcription — `(n/2) * views` is what the code computes, and it is why
     * split screen does not simply halve the effect density.
     */
    if (viewport_count <= 1)
        return group_count * Q2_FX_GROUP_QUADS;
    return (group_count / 2u) * viewport_count * Q2_FX_GROUP_QUADS;
}

void q2_fx_world_init(q2_fx_world *w, const q2_fx_tables *tab)
{
    if (!w)
        return;

    memset(w, 0, sizeof(*w));
    w->tab        = tab;
    w->size_scale = Q2_FX_SIZE_SCALE_UNITY;
    /* No image registered yet, so quads would sample an empty page. */
    w->untextured = true;
    q2_fx_world_resize(w, Q2_FX_GROUPS_DEFAULT, 1);
}

void q2_fx_set_texture(q2_fx_world *w, u16 tpage_base, u16 clut)
{
    if (!w)
        return;

    /* The ABR field belongs to the ramp, not to the page, so it is masked out
     * of whatever the caller hands over — 0x80030830 ORs the two. */
    w->tpage_base = (u16)(tpage_base & ~(u16)Q2_FX_ABR_MASK);
    w->clut       = clut;
    w->untextured = false;
}

void q2_fx_use_hud_atlas(q2_fx_world *w, const struct q2_hud_font *font)
{
    if (!w || !font || !font->resident)
        return;

    /*
     * Both halves are read, not guessed.
     *
     * The page: 0x8001AD14 selects [0x800DDD5A] for the 8-pixel face, which is
     * chars.lbm's, and 0x80030830 hands that same global to the particle draw.
     *
     * The palette: 0x80030DB8 materialises 0x800E3F2C and reads +150, and
     * 0x800E3F2C is the CLUT-id table the boot palette loop fills by record id
     * — so the particle's palette is built-in id 75. Falling back to the font's
     * only happens when a build has no record 75.
     */
    {
        const q2_hud_palette *pal =
            q2_hud_palette_get(font->tab, Q2_FX_CLUT_PALETTE_ID);

        q2_fx_set_texture(w, font->tpage,
                          pal ? pal->clut_id : font->clut_font);
    }
}

void q2_fx_world_resize(q2_fx_world *w, u32 groups, u32 viewports)
{
    if (!w)
        return;

    /* 0x80030C88: a zero argument means the shipped default. */
    if (groups == 0)
        groups = Q2_FX_GROUPS_DEFAULT;
    if (groups > Q2_FX_GROUPS_MAX)
        groups = Q2_FX_GROUPS_MAX;
    if (viewports == 0)
        viewports = 1;

    /* Slots that fall outside the new pool are gone, not orphaned. */
    if (groups < w->group_count) {
        memset(&w->group[groups], 0,
               sizeof(w->group[0]) * (w->group_count - groups));
    }

    w->group_count    = groups;
    w->viewport_count = viewports;
    w->budget         = q2_fx_budget(groups, viewports);
}

void q2_fx_world_clear(q2_fx_world *w)
{
    u32 groups, views;
    s16 scale;
    u16 tpage, clut;
    bool untextured;
    const q2_fx_tables *tab;

    if (!w)
        return;

    groups = w->group_count;
    views  = w->viewport_count;
    scale  = w->size_scale;
    tab    = w->tab;

    /* The image registration survives a clear. Emptying the pools is what the
     * level start does; it does not un-upload the texture, and letting the
     * memset zero the flag would leave every later quad sampling page 0. */
    tpage      = w->tpage_base;
    clut       = w->clut;
    untextured = w->untextured;

    memset(w, 0, sizeof(*w));

    w->tab        = tab;
    w->size_scale = scale;
    w->tpage_base = tpage;
    w->clut       = clut;
    w->untextured = untextured;
    q2_fx_world_resize(w, groups, views);
}

/* ------------------------------------------------------------------------- */
/* Groups                                                                     */
/* ------------------------------------------------------------------------- */
s32 q2_fx_group_spawn(q2_fx_world *w,
                      const s32 origin[3],
                      const s16 (*vel)[3], u32 count,
                      const q2_fx_ramp *ramp0, const q2_fx_ramp *ramp1,
                      u32 life, s32 size, u8 area)
{
    q2_fx_group *g;
    u32 slot, i;
    s32 scaled;

    if (!w || !origin || !vel || count == 0)
        return -1;

    if (count > Q2_FX_GROUP_QUADS)
        count = Q2_FX_GROUP_QUADS;

    /* 0x800302E8 scans upward and takes the first slot whose life is zero. */
    for (slot = 0; slot < w->group_count; slot++) {
        if (w->group[slot].life == 0)
            break;
    }
    if (slot >= w->group_count)
        return -1;

    g = &w->group[slot];
    memset(g, 0, sizeof(*g));

    vec_copy(g->origin, origin);

    /* Particle 0's velocity is absolute; the rest are differences from it, so
     * the whole burst translates together while it spreads. */
    for (i = 0; i < 3; i++)
        g->vel[i] = vel[0][i];

    for (i = 1; i < count; i++) {
        int k;
        for (k = 0; k < 3; k++)
            g->rel_vel[i - 1][k] = (s16)(vel[i][k] - vel[0][k]);
    }

    /* Offsets and the acceleration start at zero — the two memsets at
     * 0x800303B4 and 0x8003040C — which `memset(g)` above has already done. */

    /*
     * 0x80030430: `size = (arg * [0x800D5D46]) / 512`, rounding toward zero.
     * The rounding is explicit in the original (`bgez; addiu 511; sra 9`) and
     * is not the same as an arithmetic shift for a negative argument.
     */
    scaled = size * (s32)w->size_scale;
    if (scaled < 0)
        scaled += 511;
    g->size = (s16)(scaled >> 9);

    g->life    = (u8)(life > 255 ? 255 : life);
    g->count   = (u8)count;
    g->area    = area;
    g->ramp[0] = ramp0;
    g->ramp[1] = ramp1 ? ramp1 : ramp0;

    return (s32)slot;
}

s32 q2_fx_group_spawn_offsets(q2_fx_world *w,
                              const s32 origin[3],
                              const s16 (*offs)[3], const s16 (*vel)[3],
                              u32 count,
                              const q2_fx_ramp *ramp0, const q2_fx_ramp *ramp1,
                              u32 life, s32 size, u8 area)
{
    s32 slot = q2_fx_group_spawn(w, origin, vel, count, ramp0, ramp1,
                                 life, size, area);
    q2_fx_group *g;
    u32 i;

    if (slot < 0 || !offs)
        return slot;

    /*
     * 0x80030174: `sll 16 / sra 20`, i.e. the caller's s16 arithmetic-shifted
     * right by four. offs[0] is DISCARDED — particle 0 is the group's origin
     * and has no offset slot — so the array is read from index 1, exactly as
     * the velocity array is.
     */
    count = count > Q2_FX_GROUP_QUADS ? Q2_FX_GROUP_QUADS : count;
    g = &w->group[slot];

    for (i = 1; i < count && i <= Q2_FX_GROUP_FOLLOWERS; i++) {
        int k;
        for (k = 0; k < 3; k++)
            g->offset[i - 1][k] = (s16)(offs[i][k] >> 4);
    }

    return slot;
}

s32 q2_fx_bullet_puff(q2_fx_world *w, q2_rng *rng, const s32 at[3], u8 area)
{
    s16 offs[Q2_FX_GROUP_QUADS][3], vel[Q2_FX_GROUP_QUADS][3];
    const q2_fx_ramp *r0, *r1;
    u32 i;

    if (!w || !rng || !at)
        return -1;

    /*
     * 0x800489D8, reached from the bullet trace at 0x80048990 when the sweep
     * stopped on the WORLD rather than on an entity. Six draws per particle,
     * interleaved — three offsets then three velocities — which is the order
     * the loop at 0x80048A0C..0x80048A88 makes them in, and reproducing it is
     * what keeps a seeded replay on the generator's own sequence.
     */
    for (i = 0; i < Q2_FX_BULLET_PUFF_COUNT; i++) {
        offs[i][0] = (s16)draw(rng, Q2_FX_BULLET_PUFF_OFFS_SHIFT);
        offs[i][1] = (s16)draw(rng, Q2_FX_BULLET_PUFF_OFFS_SHIFT);
        offs[i][2] = (s16)draw(rng, Q2_FX_BULLET_PUFF_OFFS_SHIFT);
        vel[i][0]  = (s16)draw(rng, Q2_FX_BULLET_PUFF_VEL_SHIFT);
        vel[i][1]  = (s16)draw(rng, Q2_FX_BULLET_PUFF_VEL_SHIFT);
        vel[i][2]  = (s16)draw(rng, Q2_FX_BULLET_PUFF_VEL_SHIFT);
    }

    /* a3 = 0x8009BD78 and sp+16 = 0x8009BC70: ramp records 6 and 4. Grey
     * additive on quads 0-2, 6-8, 12-14 and dark red SUBTRACTIVE on the rest,
     * which is the three-quad ramp swap at 0x80030A14. */
    r0 = q2_fx_ramp_at(w->tab, Q2_FX_BULLET_PUFF_RAMP0);
    r1 = q2_fx_ramp_at(w->tab, Q2_FX_BULLET_PUFF_RAMP1);

    /* No outer loop at this site: one group. */
    return q2_fx_group_spawn_offsets(w, at, offs, vel,
                                     Q2_FX_BULLET_PUFF_COUNT, r0, r1,
                                     Q2_FX_BULLET_PUFF_LIFE,
                                     Q2_FX_BULLET_PUFF_SIZE, area);
}

void q2_fx_group_point(const q2_fx_group *g, u32 i, s32 out[3])
{
    if (!g || !out)
        return;

    if (i == 0 || i > Q2_FX_GROUP_FOLLOWERS) {
        vec_copy(out, g->origin);
        return;
    }

    out[0] = g->origin[0] + g->offset[i - 1][0];
    out[1] = g->origin[1] + g->offset[i - 1][1];
    out[2] = g->origin[2] + g->offset[i - 1][2];
}

u32 q2_fx_group_colour(const q2_fx_group *g, u32 which)
{
    const q2_fx_ramp *r;

    if (!g || which > 1)
        return 0;

    r = g->ramp[which];
    if (!r)
        return 0;

    return r->colour[q2_fx_ramp_index_for_life(g->life)];
}

void q2_fx_tick(q2_fx_world *w)
{
    u32 slot;

    if (!w)
        return;

    w->stats.groups_live = 0;

    for (slot = 0; slot < w->group_count; slot++) {
        q2_fx_group *g = &w->group[slot];
        u32 i;
        int k;

        if (g->life == 0)
            continue;

        /*
         * The order is the original's, at 0x80030B28 onward, and it matters:
         * the position advances by the velocity BEFORE the acceleration is
         * folded in, so a group is one tick behind a naive integrator. Life is
         * decremented first, which is what makes a group spawned with life 1
         * draw exactly once.
         */
        g->life--;

        for (k = 0; k < 3; k++)
            g->origin[k] += g->vel[k];
        for (k = 0; k < 3; k++)
            g->vel[k] = (s16)(g->vel[k] + g->accel[k]);

        for (i = 1; i < g->count && i <= Q2_FX_GROUP_FOLLOWERS; i++) {
            for (k = 0; k < 3; k++)
                g->offset[i - 1][k] =
                    (s16)(g->offset[i - 1][k] + g->rel_vel[i - 1][k]);
        }

        if (g->life)
            w->stats.groups_live++;
    }

    /* Debris ages with the same clock; the entity think at 0x80064124 runs in
     * the same pass as everything else on the entity list. */
    for (slot = 0; slot < Q2_FX_DEBRIS_MAX; slot++) {
        if (w->debris[slot].in_use)
            w->stats.debris_live++;
    }
}

/* ------------------------------------------------------------------------- */
/* The named effects                                                          */
/* ------------------------------------------------------------------------- */
s32 q2_fx_spawn(q2_fx_world *w, q2_rng *rng, q2_fx_preset_id id,
                const s32 at[3], u8 area)
{
    const q2_fx_preset *p = q2_fx_preset_at(id);
    s16 vel[Q2_FX_GROUP_QUADS][3];
    const q2_fx_ramp *r0, *r1;
    u32 life;
    s32 size;
    u32 i, rep, repeat;
    s32 first = -1;

    if (!w || !rng || !p || !at)
        return -1;

    life = p->life;
    size = p->size;

    /*
     * The scripted effect draws its lifetime and size before its velocities
     * (0x80028D84 and 0x80028D90), so the generator's sequence differs from
     * every other preset's. Reproducing the order is what keeps a seeded replay
     * identical.
     */
    if (id == Q2_FX_SCRIPTED) {
        life = (u32)((q2_rng_next(rng) + 24) & 0xF);
        size = (q2_rng_next(rng) & 0x2047) + 12000;
    }

    r0 = q2_fx_ramp_at(w->tab, p->ramp0);
    r1 = q2_fx_ramp_at(w->tab, p->ramp1);

    /*
     * THE OUTER LOOP. Most sites spawn the same burst two, three or four times
     * over, re-drawing all fifteen velocities each pass — see `repeat` in
     * effect.h. The draws stay INSIDE the loop because they are what makes the
     * repeats differ from each other, and because a seeded replay has to
     * consume the generator the same number of times the console does.
     *
     * A pool that fills part-way through is not an error: the original's
     * spawner returns null on a full pool and the loop simply carries on, so a
     * busy frame gets fewer groups rather than none.
     */
    repeat = p->repeat ? p->repeat : 1u;

    for (rep = 0; rep < repeat; rep++) {
        s32 slot;

        for (i = 0; i < p->count; i++) {
            vel[i][0] = (s16)draw(rng, p->spread_shift);
            vel[i][1] = (s16)draw(rng, p->spread_shift);
            vel[i][2] = (s16)draw(rng, p->spread_shift);
        }

        slot = q2_fx_group_spawn(w, at, vel, p->count, r0, r1, life, size,
                                 area);
        if (slot < 0)
            continue;

        /* `sh v0, 98(v1)` — the site's own Y acceleration, written straight
         * after the spawner returns and guarded on it having returned one. */
        if (p->accel_y)
            w->group[slot].accel[1] = p->accel_y;

        if (first < 0)
            first = slot;
    }

    return first;
}

/* ------------------------------------------------------------------------- */
/* Beams                                                                      */
/* ------------------------------------------------------------------------- */
bool q2_fx_beam_add(q2_fx_world *w,
                    const s32 from[3], const s32 to[3],
                    s16 radius, s16 area,
                    const q2_fx_face *tube,
                    const q2_fx_face *cap_near,
                    const q2_fx_face *cap_far)
{
    q2_fx_beam *b;

    if (!w || !from || !to)
        return false;

    /* 0x80064E70: full is a silent no. */
    if (w->beam_count >= Q2_FX_BEAMS_MAX) {
        w->stats.beams_dropped++;
        return false;
    }

    b = &w->beam[w->beam_count++];
    vec_copy(b->from, from);
    vec_copy(b->to, to);
    b->radius   = radius;
    b->area     = area;
    b->tube     = tube;
    b->cap_near = cap_near;
    b->cap_far  = cap_far;

    w->stats.beams_queued++;
    return true;
}

bool q2_fx_beam_add_style(q2_fx_world *w,
                          const s32 from[3], const s32 to[3],
                          s16 radius, s16 area, u32 style)
{
    const q2_fx_beam_style *s;

    if (!w)
        return false;

    s = q2_fx_beam_style_at(w->tab, style);
    if (!s)
        return false;

    return q2_fx_beam_add(w, from, to, radius, area,
                          s->tube, s->cap_near, s->cap_far);
}

void q2_fx_beams_reset(q2_fx_world *w)
{
    if (w)
        w->beam_count = 0;
}

/* ------------------------------------------------------------------------- */
bool q2_fx_beam_timed(q2_fx_world *w, s32 owner, s32 target,
                      const s32 from[3], const s32 to[3],
                      s16 radius, u32 style, s16 life)
{
    const q2_fx_beam_style *s;
    q2_fx_timed_beam *slot = NULL;
    u32 i;

    if (!w || !from || !to)
        return false;

    s = q2_fx_beam_style_at(w->tab, style);
    if (!s)
        return false;

    /*
     * 0x80049D30: look for this pair first. Refreshing the existing record is
     * what keeps one ball-to-creature beam from becoming twelve in twelve
     * frames — and the list has no other way to be freed, since the timer only
     * counts down and never releases the slot.
     */
    for (i = 0; i < Q2_FX_TIMED_BEAMS_MAX; i++) {
        if (w->timed[i].timer > 0 &&
            w->timed[i].owner == owner && w->timed[i].target == target) {
            slot = &w->timed[i];
            break;
        }
    }

    /* 0x80049D98: otherwise the first record whose timer has run out. */
    if (!slot) {
        for (i = 0; i < Q2_FX_TIMED_BEAMS_MAX; i++) {
            if (w->timed[i].timer <= 0) {
                slot = &w->timed[i];
                break;
            }
        }
    }

    if (!slot) {
        w->stats.beams_dropped++;
        return false;
    }

    slot->owner  = owner;
    slot->target = target;
    slot->timer  = life;
    slot->radius = radius;
    vec_copy(slot->from, from);
    vec_copy(slot->to, to);
    slot->tube     = s->tube;
    slot->cap_near = s->cap_near;
    slot->cap_far  = s->cap_far;

    return true;
}

void q2_fx_timed_tick(q2_fx_world *w, s32 frame_delta)
{
    u32 i;

    if (!w)
        return;

    /* 0x80048CE8: subtract and clamp at zero. The record is left in place. */
    for (i = 0; i < Q2_FX_TIMED_BEAMS_MAX; i++) {
        if (w->timed[i].timer <= 0)
            continue;
        w->timed[i].timer = (s16)(w->timed[i].timer - frame_delta);
        if (w->timed[i].timer < 0)
            w->timed[i].timer = 0;
    }
}

u32 q2_fx_timed_live(const q2_fx_world *w)
{
    u32 i, n = 0;

    if (!w)
        return 0;

    for (i = 0; i < Q2_FX_TIMED_BEAMS_MAX; i++) {
        if (w->timed[i].timer > 0)
            n++;
    }
    return n;
}

/*
 * Two unit vectors spanning the plane perpendicular to `n`, which must be a
 * 1.12 unit vector. 0x800563F4, transcribed.
 *
 * The test is on |n.y| against 2897, which is 4096 / sqrt(2) to the unit — a
 * 45-degree cone about the vertical. A MOSTLY VERTICAL beam is crossed with the
 * X axis and everything else with the Y axis, which is the way round that keeps
 * both cross products away from zero: crossing a vertical beam with Y, or a
 * horizontal one with X when it happens to point along X, collapses the whole
 * ring onto the beam and the hull disappears.
 *
 * The branch sense is easy to get backwards, because the comparison names the
 * axis it is NOT going to use. 0x8005640C branches to the Y-axis block when
 * |n.y| < 2897; the fall-through at 0x80056414 is the X-axis block.
 *
 * `v` comes out as `n (n . e) - e`, i.e. the NEGATED in-plane part of the axis.
 * The negation is baked into the construction rather than applied afterwards,
 * so u and v are a left-handed pair — which sets which way round the hexagon is
 * wound and therefore which of its faces the hardware culls.
 */
static void perp_basis(const s32 n[3], s32 u[3], s32 v[3])
{
    s32 ay = n[1] < 0 ? -n[1] : n[1];

    if (ay >= 2897) {
        /* Mostly vertical: cross with X. 0x80056414. */
        u[0] = 0;
        u[1] = -n[2];
        u[2] =  n[1];

        v[0] = mul12(n[0], n[0]) - Q2_ONE_12;
        v[1] = mul12(n[1], n[0]);
        v[2] = mul12(n[2], n[0]);
    } else {
        /* Everything else: cross with Y. 0x80056494. */
        u[0] =  n[2];
        u[1] = 0;
        u[2] = -n[0];

        v[0] = mul12(n[0], n[1]);
        v[1] = mul12(n[1], n[1]) - Q2_ONE_12;
        v[2] = mul12(n[2], n[1]);
    }
}

/* Normalise to 1.12. Returns false for the zero vector, which is where
 * 0x8008A588 reports a zero length and 0x8006350C bails. */
static bool normalise12(const s32 in[3], s32 out[3])
{
    s64 sq = (s64)in[0] * in[0] + (s64)in[1] * in[1] + (s64)in[2] * in[2];
    s64 len = 0, bit;

    if (sq == 0)
        return false;

    /* Integer square root; the console's is a GTE-assisted Newton step
     * (0x8008A7E8) whose result is exact for the magnitudes involved here. */
    for (bit = 1; bit * bit <= sq; bit <<= 1)
        ;
    for (bit >>= 1; bit; bit >>= 1) {
        s64 t = len + bit;
        if (t * t <= sq)
            len = t;
    }
    if (len == 0)
        return false;

    out[0] = (s32)(((s64)in[0] << Q2_FRAC_12) / len);
    out[1] = (s32)(((s64)in[1] << Q2_FRAC_12) / len);
    out[2] = (s32)(((s64)in[2] << Q2_FRAC_12) / len);
    return true;
}

/*
 * The hexagon, 0x800634E4.
 *
 * Three directions at 120 degrees in the plane, then their negations — so the
 * SIX points come out in the index order 0, 120, 240, 180, 300, 60 degrees.
 * That ordering is not decoration: consecutive indices are 120 degrees apart,
 * so the six quads the tube face list builds fold back through the beam's axis
 * instead of wrapping a prism around it. It is what makes a beam read as solid
 * from any angle without the polygon count a real cylinder would cost, and a
 * reader who "fixed" the order into 0,60,120,180,240,300 would get a clean
 * hexagonal tube that looks nothing like the original.
 */
static void hexagon(const s32 dir_unit[3], s32 radius, s32 out[6][3])
{
    s32 u[3], v[3];
    int i, k;

    perp_basis(dir_unit, u, v);

    for (i = 0; i < 3; i++) {
        s32 ang = (i * Q2_ONE_12) / 3;      /* 0, 1365, 2730 */
        s32 su  = -((q2_sin12(ang) * radius) >> Q2_FRAC_12);
        s32 sv  =  ((q2_cos12(ang) * radius) >> Q2_FRAC_12);

        for (k = 0; k < 3; k++) {
            s32 c = (u[k] * su + v[k] * sv) >> Q2_FRAC_12;
            out[i][k]     =  c;
            out[i + 3][k] = -c;
        }
    }
}

bool q2_fx_beam_hull(const q2_fx_beam *b, s32 out[Q2_FX_BEAM_VERTS][3])
{
    s32 delta[3], unit[3], hex[6][3];
    int i, k;

    if (!b || !out)
        return false;

    for (k = 0; k < 3; k++)
        delta[k] = b->to[k] - b->from[k];

    if (!normalise12(delta, unit))
        return false;

    hexagon(unit, b->radius, hex);

    for (i = 0; i < 6; i++) {
        for (k = 0; k < 3; k++) {
            out[i][k]     = b->from[k] + hex[i][k];
            out[i + 6][k] = b->to[k]   + hex[i][k];
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* The laser                                                                  */
/* ------------------------------------------------------------------------- */
bool q2_fx_laser(q2_fx_world *w, q2_rng *rng, u32 kind,
                 const s32 from[3], const s32 to[3],
                 s16 area, u32 ends, q2_fx_laser_result *out)
{
    const q2_fx_laser_kind *lk;
    const q2_fx_ramp *ramp;
    q2_fx_laser_result r;
    u32 i;

    memset(&r, 0, sizeof(r));

    if (!w || !rng || !from || !to)
        return false;

    lk = q2_fx_laser_kind_at(w->tab, kind);
    if (!lk)
        return false;

    r.damage = lk->damage;
    r.mod    = lk->mod;
    r.queued = q2_fx_beam_add_style(w, from, to, lk->radius, area,
                                    lk->style);

    ramp = q2_fx_ramp_at(w->tab, lk->ramp);

    /*
     * Each lit end throws four groups of fifteen. The original's two end loops
     * are separate blocks with the same body (0x80049010 and 0x800490B8), each
     * gated on its own halfword of the packed fifth argument, and each drawing
     * its velocities from the generator afresh — so a laser with both ends lit
     * consumes 8 * 15 * 3 draws.
     */
    for (i = 0; i < 2; i++) {
        const s32 *at = (i == 0) ? from : to;
        u32 g;

        if (!(ends & (i == 0 ? Q2_FX_LASER_END_FROM : Q2_FX_LASER_END_TO)))
            continue;

        for (g = 0; g < Q2_FX_LASER_END_GROUPS; g++) {
            s16 vel[Q2_FX_GROUP_QUADS][3];
            const q2_fx_preset *p = q2_fx_preset_at(Q2_FX_LASER_END);
            u32 q;

            for (q = 0; q < Q2_FX_GROUP_QUADS; q++) {
                vel[q][0] = (s16)draw(rng, p->spread_shift);
                vel[q][1] = (s16)draw(rng, p->spread_shift);
                vel[q][2] = (s16)draw(rng, p->spread_shift);
            }

            s32 slot = q2_fx_group_spawn(w, at, vel, Q2_FX_GROUP_QUADS,
                                         ramp, ramp, p->life, p->size,
                                         (u8)area);

            if (slot >= 0) {
                /* Both end sites write accel[1] straight after the spawner
                 * returns — 0x80049088 and 0x80049134, `sh v0(=2), 98(v1)`.
                 * This path does not go through q2_fx_spawn, so the preset's
                 * accel column has to be applied here by hand. */
                if (p->accel_y)
                    w->group[slot].accel[1] = p->accel_y;
                r.groups++;
            }
        }
    }

    if (out)
        *out = r;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Debris                                                                     */
/* ------------------------------------------------------------------------- */
bool q2_fx_debris_register(q2_fx_world *w, s16 model)
{
    if (!w)
        return false;

    /* 0x80064F80 caps at 32 and drops the rest without complaint. */
    if (w->debris_model_count >= 32)
        return false;

    w->debris_model[w->debris_model_count++] = model;
    return true;
}

/* The lifetime a fresh piece gets, and what an impact costs it. 0x800644C4
 * stores 2100 into +0xF4; 0x8006423C subtracts 300. */
#define Q2_FX_DEBRIS_LIFE     2100
#define Q2_FX_DEBRIS_IMPACT   300

u32 q2_fx_debris_burst(q2_fx_world *w, q2_rng *rng,
                       const s32 bmin[3], const s32 bmax[3],
                       const s32 *at, u32 count, u8 area)
{
    s32 size[3];
    u32 made = 0, i;

    if (!w || !rng || count == 0)
        return 0;

    /* 0x800645E8 takes the box's extent once, outside the loop. */
    if (bmin && bmax) {
        size[0] = bmax[0] - bmin[0];
        size[1] = bmax[1] - bmin[1];
        size[2] = bmax[2] - bmin[2];
    } else {
        size[0] = size[1] = size[2] = 0;
    }

    for (i = 0; i < count; i++) {
        q2_fx_debris *d = NULL;
        u32 slot;
        s32 pos[3];
        int k;

        for (slot = 0; slot < Q2_FX_DEBRIS_MAX; slot++) {
            if (!w->debris[slot].in_use) {
                d = &w->debris[slot];
                break;
            }
        }
        if (!d)
            break;

        if (at) {
            vec_copy(pos, at);
        } else if (bmin) {
            /* 0x80064644: `min + (rand() * extent) >> 15` per axis. */
            for (k = 0; k < 3; k++)
                pos[k] = bmin[k] + ((q2_rng_next(rng) * size[k]) >> 15);
        } else {
            pos[0] = pos[1] = pos[2] = 0;
        }

        memset(d, 0, sizeof(*d));
        d->in_use = true;
        vec_copy(d->pos, pos);

        /*
         * 0x800646AC. Three draws of `3 * (rand() - 16384) >> 5`, with the
         * middle one biased by -1536. Negative Y is up in this engine's frame,
         * so the bias is what makes debris leap rather than spray flat — and it
         * is a whole extra range's worth, so every piece starts moving upward.
         */
        d->vel[0] = (s16)((3 * (q2_rng_next(rng) - 16384)) >> 5);
        d->vel[1] = (s16)(((3 * (q2_rng_next(rng) - 16384)) >> 5) - 1536);
        d->vel[2] = (s16)((3 * (q2_rng_next(rng) - 16384)) >> 5);

        /* 0x80064474: three raw draws, used as an orientation rather than a
         * rate — the piece starts at a random attitude. */
        d->spin[0] = (s16)q2_rng_next(rng);
        d->spin[1] = (s16)q2_rng_next(rng);
        d->spin[2] = (s16)q2_rng_next(rng);

        d->life = Q2_FX_DEBRIS_LIFE;
        d->area = area;
        d->node = -1;

        /* 0x8006473C picks uniformly from the registration list. A level that
         * registered nothing spawns pieces with no model, which is what the
         * original does too — the entity exists and draws nothing. */
        if (w->debris_model_count)
            d->model = w->debris_model[(u32)q2_rng_next(rng) %
                                       w->debris_model_count];
        else
            d->model = -1;

        made++;
    }

    return made;
}

void q2_fx_debris_step_one(q2_fx_world *w, u32 index, s32 gravity,
                           q2_fx_debris_step *out)
{
    q2_fx_debris *d;
    int k;

    if (!w || !out || index >= Q2_FX_DEBRIS_MAX)
        return;

    memset(out, 0, sizeof(*out));
    d = &w->debris[index];
    if (!d->in_use)
        return;

    vec_copy(out->from, d->pos);

    /*
     * 0x80064220 hands the piece to the shared entity mover through
     * 0x80046DA0, so the integration is the same shape the projectiles use and
     * the caller traces the result against PRIMARY collision — see the note by
     * Q2_FX_DEBRIS_RADIUS.
     */
    for (k = 0; k < 3; k++)
        out->to[k] = d->pos[k] + d->vel[k];

    /*
     * 0x80046464, transcribed. `gravity` is the caller's already-scaled step —
     * the mover computes `[0x800AE924] * frame_delta` and the simulation owns
     * both. The clamp is the original's, and it is one-sided: nothing stops a
     * shard being thrown upward faster than terminal velocity, only falling
     * faster than it.
     */
    if (gravity && !(d->flags & Q2_FX_ENT_NO_GRAVITY)) {
        s32 vy = d->vel[1] + gravity;
        if (vy > Q2_FX_TERMINAL_VELOCITY)
            vy = Q2_FX_TERMINAL_VELOCITY;
        if (vy < -32768)
            vy = -32768;
        d->vel[1] = (s16)vy;
    }

    if (d->life <= 0)
        out->expired = true;
}

void q2_fx_debris_commit(q2_fx_world *w, u32 index, const s32 to[3])
{
    if (!w || !to || index >= Q2_FX_DEBRIS_MAX)
        return;
    if (!w->debris[index].in_use)
        return;
    vec_copy(w->debris[index].pos, to);
}

void q2_fx_debris_impact(q2_fx_world *w, u32 index, const s32 point[3])
{
    q2_fx_debris *d;

    if (!w || index >= Q2_FX_DEBRIS_MAX)
        return;

    d = &w->debris[index];
    if (!d->in_use)
        return;

    if (point)
        vec_copy(d->pos, point);

    /* 0x80064230: the spin advances by one and the lifetime drops by 300. The
     * piece does not bounce — the mover slides it — so its velocity is left
     * alone and the next tick will simply be blocked again. */
    d->spin[0] = (s16)(d->spin[0] + 1);
    d->life    = (s16)(d->life - Q2_FX_DEBRIS_IMPACT);

    if (d->life <= 0)
        d->in_use = false;
}

/* ------------------------------------------------------------------------- */
/* Glints                                                                     */
/* ------------------------------------------------------------------------- */
bool q2_fx_glint_mesh_decode(q2_fx_glint_mesh *out, const u8 *data, u32 size)
{
    if (!out || !data)
        return false;

    memset(out, 0, sizeof(*out));

    /* 0x800651C0 refuses a NULL chunk and otherwise splits unconditionally, so
     * a chunk with no room past the split would hand the renderer a pointer off
     * the end. Refusing here is the port declining to reproduce that. */
    if (size <= Q2_FX_GLINT_INDEX_BYTES)
        return false;

    out->index      = data;
    out->face_count = Q2_FX_GLINT_FACE_COUNT;

    /*
     * The vertices are s16 quads and the chunk is a flat file image, so the
     * cast is only safe on a little-endian host with 2-byte alignment. Both
     * hold here — the loader hands over a buffer it allocated — but the mesh is
     * read through a typed pointer rather than copied because 218 vertices per
     * frame is not worth a copy.
     */
    out->vert       = (const s16 (*)[4])(const void *)(data +
                                                       Q2_FX_GLINT_INDEX_BYTES);
    out->vert_count = (size - Q2_FX_GLINT_INDEX_BYTES) / 8u;

    return out->vert_count != 0;
}

void q2_fx_glint_shade(const s16 *vertex_band, u32 count,
                       const u8 tint_rgb[3], s32 width, s32 phase,
                       u8 (*out_rgb)[3])
{
    s32 tint[3];
    s32 centre;
    u32 i;
    int k;

    if (!vertex_band || !tint_rgb || !out_rgb || width == 0)
        return;

    /* 0x800647A0: each channel is `(c << 13) / width`, so a narrower band is a
     * brighter one — the two uses of `width` do not cancel. */
    for (k = 0; k < 3; k++)
        tint[k] = ((s32)tint_rgb[k] << 13) / width;

    /* 0x800648C0: the band's centre walks the mesh as the phase counts down. */
    {
        s32 quarter = width >= 0 ? (width >> 2) : ((width + 3) >> 2);
        centre = quarter * (4 - phase) + 1024;
    }

    for (i = 0; i < count; i++) {
        s32 d = (s32)vertex_band[i] - centre;
        s32 wgt;

        if (d < 0)
            d = -d;

        wgt = (d >= width) ? 0 : (width - d);
        wgt *= phase;

        for (k = 0; k < 3; k++) {
            s32 c = (tint[k] * wgt) >> 15;
            if (c < 0)   c = 0;
            if (c > 255) c = 255;
            out_rgb[i][k] = (u8)c;
        }
    }
}

u32 q2_fx_glint_build_ot(const q2_fx_glint_mesh *mesh,
                         const s32 origin[3], s32 yaw,
                         const u8 tint_rgb[3], s32 width, s32 phase,
                         const q2_camera *cam, psx_ot *ot, gte_state *gte)
{
    /* The Z-order-to-perimeter swap, as everywhere else here. */
    static const int k_perimeter[4] = { 0, 1, 3, 2 };

    s16 spin[3][3];
    gte_sxy xy[Q2_FX_GLINT_VERTS];
    u16     z[Q2_FX_GLINT_VERTS];
    bool    ok[Q2_FX_GLINT_VERTS];
    u8      rgb[Q2_FX_GLINT_VERTS][3];
    s16     vz[Q2_FX_GLINT_VERTS];
    u32     count, i, f, emitted = 0;

    if (!mesh || !mesh->vert || !mesh->index || !origin || !tint_rgb ||
        !cam || !ot || !gte || width == 0)
        return 0;

    /* The retail custom-entity driver selects entity+0x9E before reaching this
     * emitter.  This legacy entry point has no area argument, so it likewise
     * consumes the caller's current projection.  In particular, selector -1
     * must not be used as a reset: 0x80065684 returns immediately for it. */

    /*
     * 0x800649C8 transforms vertices in threes while its counter is under 94,
     * so one call covers the first 96 of the mesh however long the mesh is. A
     * 218-vertex GlintMod is therefore drawn as a prefix, not in full.
     */
    count = mesh->vert_count;
    if (count > Q2_FX_GLINT_VERTS)
        count = Q2_FX_GLINT_VERTS;
    if (count == 0 || mesh->face_count == 0)
        return 0;

    q2_rotation_yaw_pitch(spin, yaw, 0);

    /*
     * The band reads the vertex's FOURTH halfword, before any transform —
     * 0x80064938 takes `+6` off the source array, not off the result. It is a
     * coordinate across the mesh rather than a position in the world, which is
     * why it survives the rotation untouched.
     */
    for (i = 0; i < count; i++)
        vz[i] = mesh->vert[i][3];

    q2_fx_glint_shade(vz, count, tint_rgb, width, phase, rgb);

    for (i = 0; i < count; i++) {
        s32 local[3], world[3];
        int k;

        local[0] = mesh->vert[i][0];
        local[1] = mesh->vert[i][1];
        local[2] = mesh->vert[i][2];

        for (k = 0; k < 3; k++) {
            s64 sum = (s64)spin[k][0] * local[0] + (s64)spin[k][1] * local[1] +
                      (s64)spin[k][2] * local[2];
            world[k] = origin[k] + (s32)(sum >> Q2_FRAC_12) - cam->pos[k];
        }

        ok[i] = gte_project_point(gte, world[0], world[1], world[2],
                                  &xy[i], &z[i]);
    }

    for (f = 0; f < mesh->face_count; f++) {
        const u8 *idx = &mesh->index[f * 4u];
        psx_prim *prim;
        u32 depth = 0;
        bool good = true;
        int c;

        /* 0x80064BC4 stops at 79 primitives, whatever the mesh has left. */
        if (emitted >= Q2_FX_GLINT_MAX_FACES)
            break;

        for (c = 0; c < 4; c++) {
            if (idx[c] >= count || !ok[idx[c]]) {
                good = false;
                break;
            }
            depth += z[idx[c]];
        }
        if (!good)
            continue;

        prim = psx_ot_add_depth(ot,
                                (u16)bucket_for(ot, depth, 4, cam->sort_range),
                                sort_key_for(depth, 4));
        if (!prim)
            break;

        /* Gouraud and additive: the band is a brightness, and a glint that
         * occluded what was behind it would be a solid tube rather than a
         * glow. */
        prim->kind = PSX_PRIM_G4;
        prim->semi_transparent = true;
        prim->tpage = Q2_FX_ABR_ADD;

        for (c = 0; c < 4; c++) {
            u32 v = idx[k_perimeter[c]];
            prim->xy[c] = *(const psx_xy *)&xy[v];
            prim->rgb[c].r = rgb[v][0];
            prim->rgb[c].g = rgb[v][1];
            prim->rgb[c].b = rgb[v][2];
            prim->rgb[c].pad = 0;
        }

        emitted++;
    }

    return emitted;
}

/* ------------------------------------------------------------------------- */
bool q2_fx_glint_scan(q2_fx_glint_script *out, const u8 *levelbin, u32 size)
{
    u32 i;

    if (!out)
        return false;

    memset(out, 0, sizeof(*out));

    if (!levelbin || size < 12)
        return false;

    /*
     * Pass one: does anything raise the flag?
     *
     *     lw   rX, 0x10C(rY) ; lui rZ, 0x0400 ; or rX, rX, rZ
     *
     * The `and` form of the third instruction is the TEST site, not a raise, so
     * the two are told apart by that one function field.
     */
    for (i = 0; i + 12 <= size; i += 4) {
        u32 a = q2_rd_u32(levelbin + i);
        u32 b = q2_rd_u32(levelbin + i + 4);
        u32 c = q2_rd_u32(levelbin + i + 8);

        if ((a >> 26) != 0x23u || (a & 0xFFFFu) != 0x10Cu)
            continue;
        if ((b >> 26) != 0x0Fu || (b & 0xFFFFu) != 0x0400u)
            continue;
        if ((c >> 26) != 0u || (c & 0x3Fu) != 0x25u)
            continue;

        out->raises       = true;
        out->raise_offset = i;
        break;
    }

    if (!out->raises)
        return false;

    /*
     * Pass two: the two immediates, wherever in the module they are.
     *
     * They are NOT together. BIGGUN writes the band count four instructions
     * before the raise and the phase 0x168 bytes later, beside the test site —
     * so a window around the raise finds one and misses the other, which is
     * exactly what a first attempt here did. Scanning the whole module for each
     * store is both simpler and correct for a script that orders them any other
     * way.
     *
     * Each is `addiu rT, zero, <n>` and then a store of rT — but NOT
     * necessarily the next instruction. The band count's pair is adjacent; the
     * phase's `addiu` sits five instructions before its `sh`, with a branch and
     * two unrelated instructions in between. So the source is found by walking
     * back for a load of the STORE'S OWN REGISTER, which is right regardless of
     * how the scheduler moved things.
     */
    for (i = 0; i + 4 <= size; i += 4) {
        u32 w = q2_rd_u32(levelbin + i);
        u32 op = w >> 26, rt = (w >> 16) & 31u, disp = w & 0xFFFFu;
        u8 *slot;
        u32 back;

        if (op == 0x28u && disp == 0x2B7u)       /* sb — band count */
            slot = &out->band_count;
        else if (op == 0x29u && disp == 0x2BEu)  /* sh — phase      */
            slot = &out->phase;
        else
            continue;

        if (*slot)
            continue;

        for (back = i; back >= 4; back -= 4) {
            u32 p = q2_rd_u32(levelbin + back - 4);

            /* addiu rT, zero, imm — the register has to match the store's. */
            if ((p >> 26) == 0x09u && ((p >> 21) & 31u) == 0u &&
                ((p >> 16) & 31u) == rt) {
                *slot = (u8)(p & 0xFFu);
                break;
            }
            /* Give up rather than walk the whole module backwards. */
            if (i - back >= 32u)
                break;
        }
    }

    return true;
}

/* ------------------------------------------------------------------------- */
void q2_fx_glint_advance(q2_fx_glint *g)
{
    u32 i, n;

    if (!g)
        return;

    /*
     * A PLAIN DECREMENT, and it wraps.
     *
     * 0x80064DEC is `lbu a3, 6(s0); addiu v0, a3, 255; sb v0, 6(s0)` — a byte
     * decrement that underflows 0 to 255, not a reset. 0x80064D38 does the same
     * to the single path's halfword. There is no clamp and no reload anywhere
     * in the draw, so a band that runs past zero spends the next ~250 ticks
     * with its centre far off the mesh and lights nothing.
     *
     * That is not a bug to fix here: the phases are the level script's to
     * refresh, the same way the band records are (effect.h). Wrapping them back
     * to the start would look tidier and would be this port inventing a reload
     * the console does not perform.
     *
     * The one divergence is WHERE this happens. The original decrements inside
     * the draw, once per band per call, so a two-viewport frame advances every
     * band twice; doing it on the tick instead keeps split screen honest.
     */
    if (g->band_count == 0) {
        g->phase = (u16)(g->phase - 1u);
        return;
    }

    n = g->band_count;
    if (n > Q2_FX_GLINT_BANDS_MAX)
        n = Q2_FX_GLINT_BANDS_MAX;

    for (i = 0; i < n; i++)
        g->band[i].phase = (u8)(g->band[i].phase - 1u);
}

u32 q2_fx_glint_draw(const q2_fx_glint *g, const s32 origin[3], s32 yaw,
                     const q2_camera *cam, psx_ot *ot, gte_state *gte)
{
    u32 emitted = 0, i, n;

    if (!g || !g->ready || !origin || !cam || !ot || !gte)
        return 0;

    /* 0x80064CE4: a zero count takes the single-band path, which uses the
     * entity's own colour, phase and width. */
    if (g->band_count == 0) {
        return q2_fx_glint_build_ot(&g->mesh, origin, yaw, g->tint,
                                    Q2_FX_GLINT_ONE_WIDTH, (s32)g->phase,
                                    cam, ot, gte);
    }

    n = g->band_count;
    if (n > Q2_FX_GLINT_BANDS_MAX)
        n = Q2_FX_GLINT_BANDS_MAX;

    for (i = 0; i < n; i++) {
        const q2_fx_glint_band *b = &g->band[i];
        u8 tint[3];

        tint[0] = q2_fx_colour_r(b->colour);
        tint[1] = q2_fx_colour_g(b->colour);
        tint[2] = q2_fx_colour_b(b->colour);

        /*
         * Each band composes its own orientation with the entity's — 0x80064DA4
         * builds a matrix from the record's three angles and 0x80064DC4
         * multiplies it by the entity's own. The port folds the band's yaw into
         * the instance yaw, which is the same composition for the single axis
         * the mesh is ever placed on; a band with pitch or roll would need the
         * full multiply, and none on this disc has one.
         *
         * The multi-band width is 4096, HALF the single path's — which by the
         * shading formula makes each band narrower and twice as bright.
         */
        emitted += q2_fx_glint_build_ot(&g->mesh, origin, yaw + b->angle[1],
                                        tint, Q2_FX_GLINT_BAND_WIDTH,
                                        (s32)b->phase, cam, ot, gte);
    }

    return emitted;
}

/* ------------------------------------------------------------------------- */
/* Drawing                                                                    */
/* ------------------------------------------------------------------------- */
static void camera_basis(const q2_camera *cam, s16 view[3][3])
{
    q2_rotation_yaw_pitch(view, cam->yaw, cam->pitch);
}

static void to_camera(const s16 view[3][3], const q2_camera *cam,
                      const s32 world[3], s32 out[3])
{
    s32 rel[3];
    int r;

    rel[0] = world[0] - cam->pos[0];
    rel[1] = world[1] - cam->pos[1];
    rel[2] = world[2] - cam->pos[2];

    for (r = 0; r < 3; r++) {
        s64 sum = (s64)view[r][0] * rel[0] + (s64)view[r][1] * rel[1] +
                  (s64)view[r][2] * rel[2];
        out[r] = (s32)(sum >> Q2_FRAC_12);
    }
}

/*
 * Where an effect's primitive sorts.
 *
 * This used to be `(depth / corners) >> 2`, which is the fixed shift the whole
 * port used before the viewport's far distance was available — and the world
 * and the models both moved off it (world.c, modeldraw.c) while the effects did
 * not. Against a real viewport slice of 51 buckets the shift saturates
 * everything past about 200 units onto the far end of the slice, and the far
 * end is drawn FIRST: so every particle, glint and beam more than a room away
 * was emitted correctly, sorted behind the walls, and painted over.
 *
 * The symptom was a level's LASERBEAMs. Eleven queued, 970 faces drawn, seven
 * pixels different on screen — geometry that reached the table and lost every
 * argument with it. Sharing the world's mapping is the fix, and it is not a
 * tuning choice: an effect and the wall behind it have to be measured against
 * the same far distance or the sort between them means nothing.
 */
static u32 bucket_for(const psx_ot *ot, u32 depth_sum, u32 corners, s32 far_z)
{
    if (corners == 0)
        corners = 1;
    return q2_ot_bucket_for_depth(ot, depth_sum / corners, far_z);
}

/*
 * The same depth the bucket came from, kept at full resolution so an effect and
 * the geometry it shares a bucket with are ordered by depth rather than by
 * which of the two emitters ran first. See psx_ot_add_depth.
 */
static u32 sort_key_for(u32 depth_sum, u32 corners)
{
    if (corners == 0)
        corners = 1;
    return depth_sum / corners;
}

/* Split a table colour word into a psx_rgb and its ABE bit. */
static void unpack_colour(u32 word, psx_rgb *rgb, bool *semi)
{
    rgb->r   = q2_fx_colour_r(word);
    rgb->g   = q2_fx_colour_g(word);
    rgb->b   = q2_fx_colour_b(word);
    rgb->pad = 0;
    *semi    = (q2_fx_colour_code(word) & 0x02u) != 0;
}

static u32 draw_groups(q2_fx_world *w, const q2_camera *cam, u32 viewport,
                       psx_ot *ot, gte_state *gte, const s16 view[3][3])
{
    u32 emitted = 0, slot;
    s32 budget = (s32)w->budget;

    for (slot = 0; slot < w->group_count; slot++) {
        q2_fx_group *g = &w->group[slot];
        s32 cam_space[3], base[3];
        gte_sxy base_xy;
        u16 base_z;
        s32 pixels;
        u32 colour[2];
        bool semi[2];
        psx_rgb rgb[2];
        u32 area_bucket = 0;
        bool area_routed = psx_ot_area_active(ot);
        s32 batch = PSX_OT_BATCH_INVALID;
        u32 i;

        if (g->life == 0)
            continue;

        /* 0x8003061C: the high nibble of the flags byte disables a group in
         * one viewport without touching the others. */
        if (viewport < 4 && (g->view_mask & (1u << viewport)))
            continue;

        /* Particle groups live on retail's area +12 batch list. A stale area
         * has no screen-change record to drain, so the whole private chain is
         * culled instead of falling back to a global depth bucket and painting
         * through the current room. */
        if (area_routed &&
            !psx_ot_area_bucket(ot, g->area & 0x7Fu, &area_bucket))
            continue;

        q2_camera_apply_area_projection(cam, ot,
                                        area_routed ? (s32)(g->area & 0x7Fu)
                                                    : -1,
                                        gte);

        /*
         * 0x80030644. A group whose quad count exceeds what is left of the
         * frame's allowance is dropped WHOLE. Partially drawing it would look
         * better and would not be the original.
         */
        if ((s32)g->count > budget) {
            w->stats.groups_skipped_budget++;
            continue;
        }

        vec_copy(base, g->origin);
        to_camera(view, cam, base, cam_space);
        gte_set_translation(gte, 0, 0, 0);
        {
            gte_matrix gm;
            memcpy(gm.m, view, sizeof(gm.m));
            gte_set_rotation(gte, &gm);
        }

        if (!gte_project_point(gte, base[0] - cam->pos[0],
                               base[1] - cam->pos[1],
                               base[2] - cam->pos[2],
                               &base_xy, &base_z)) {
            w->stats.groups_skipped_near++;
            continue;
        }

        /* 0x80030708: the near cutoff is on the camera-space depth, not on the
         * projected one, and it drops the group rather than clipping it. */
        if (cam_space[2] < Q2_FX_NEAR_DEPTH) {
            w->stats.groups_skipped_near++;
            continue;
        }

        if (area_routed) {
            /* 0x800308B4 registers one point record for the whole group on
             * area +12. Every quad remains in its private chain. */
            batch = psx_ot_batch_begin_point(
                        ot, g->area & 0x7Fu, true, (s16)base_z,
                        base, cam->pos);
        }

        /*
         * 0x800307F4: the on-screen side is `size / z`, clamped up to two
         * pixels so a distant burst never vanishes entirely.
         */
        pixels = (s32)g->size / (cam_space[2] ? cam_space[2] : 1);
        if (pixels < Q2_FX_QUAD_MIN_PIXELS)
            pixels = Q2_FX_QUAD_MIN_PIXELS;

        colour[0] = q2_fx_group_colour(g, 0);
        colour[1] = q2_fx_group_colour(g, 1);
        unpack_colour(colour[0], &rgb[0], &semi[0]);
        unpack_colour(colour[1], &rgb[1], &semi[1]);

        for (i = 0; i < g->count; i++) {
            s32 pt[3];
            gte_sxy xy;
            u16 z;
            psx_prim *prim;
            /* 0x80030A14 swaps the two ramps every three quads. */
            u32 which = (i / 3u) & 1u;
            s16 half  = (s16)(pixels >> 1);

            q2_fx_group_point(g, i, pt);

            if (!gte_project_point(gte, pt[0] - cam->pos[0],
                                   pt[1] - cam->pos[1],
                                   pt[2] - cam->pos[2], &xy, &z))
                continue;

            if (area_routed) {
                prim = batch >= 0
                     ? psx_ot_batch_add(ot, batch)
                     : psx_ot_add_bucket_depth(ot, area_bucket,
                                               base_z, base_z);
            } else {
                prim = psx_ot_add_depth(
                           ot,
                           (u16)bucket_for(ot, z, 1, cam->sort_range),
                           sort_key_for(z, 1));
            }
            if (!prim) {
                w->stats.ot_overflow++;
                break;
            }

            /*
             * 0x80030958 shifts the projected corner back by half the side and
             * lays the quad out as (x,y) (x+s,y) (x,y+s) (x+s,y+s) — the
             * hardware's Z order, two triangles sharing the middle edge.
             *
             * `psx_prim` is NOT in that order. Its quads run around the
             * PERIMETER, because that is how the on-disc world geometry is laid
             * out and the rasteriser fans them (0,1,2)+(0,2,3). Feeding it a
             * Z-ordered quad drops a corner and leaves an arrowhead rather than
             * a square — which looks like a clipping fault, not an ordering
             * one. So corners 2 and 3 are swapped on the way in, here and in
             * every face that comes out of the effect tables.
             */
            prim->kind = w->untextured ? PSX_PRIM_F4 : PSX_PRIM_FT4;
            prim->xy[0].x = (s16)(xy.x - half);
            prim->xy[0].y = (s16)(xy.y - half);
            prim->xy[1].x = (s16)(prim->xy[0].x + pixels);
            prim->xy[1].y = prim->xy[0].y;
            prim->xy[2].x = prim->xy[1].x;                 /* Z corner 3 */
            prim->xy[2].y = (s16)(prim->xy[0].y + pixels);
            prim->xy[3].x = prim->xy[0].x;                 /* Z corner 2 */
            prim->xy[3].y = prim->xy[2].y;

            prim->rgb[0] = rgb[which];
            prim->rgb[1] = rgb[which];
            prim->rgb[2] = rgb[which];
            prim->rgb[3] = rgb[which];

            /*
             * The UVs are constant: 0x80030DB8 bakes a 16x16 patch at
             * (240,240) into every slot of the pool and the per-frame writer
             * never touches them again.
             */
            prim->uv[0].u = Q2_FX_QUAD_U0; prim->uv[0].v = Q2_FX_QUAD_V0;
            prim->uv[1].u = Q2_FX_QUAD_U1; prim->uv[1].v = Q2_FX_QUAD_V0;
            prim->uv[2].u = Q2_FX_QUAD_U1; prim->uv[2].v = Q2_FX_QUAD_V1;
            prim->uv[3].u = Q2_FX_QUAD_U0; prim->uv[3].v = Q2_FX_QUAD_V1;
            prim->clut = w->clut;

            /* The ramp's ABR field is OR-ed into the draw-mode word, which is
             * where the additive and the three subtractive ramps differ. */
            prim->tpage = (u16)(w->tpage_base |
                                (g->ramp[which] ? g->ramp[which]->abr : 0));
            prim->semi_transparent = semi[which];
            prim->textured_blend   = true;

            emitted++;
            w->stats.quads_emitted++;
        }

        budget -= (s32)g->count;
        w->stats.groups_drawn++;
    }

    return emitted;
}

/*
 * A beam is drawn as a chain of 640-unit segments.
 *
 * 0x80063B6C takes the beam's length, computes `ceil(len / 640) - 1` and draws
 * that many segments, stepping along a direction pre-scaled to 640 units
 * (0x80063804 multiplies the unit direction by 640/4096). A beam shorter than
 * 640 units therefore draws NOTHING — the count goes negative and 0x80063BEC
 * bails — which is worth knowing before concluding a short beam is a bug.
 */
#define Q2_FX_BEAM_SEGMENT 640

static u32 draw_beam_ring(psx_ot *ot, gte_state *gte, const q2_camera *cam,
                          const q2_fx_face *faces, u32 face_count,
                          const gte_sxy *xy, const u16 *z, const bool *ok,
                          u32 vert_count, s32 sort_area, s32 area_bucket,
                          const s32 sort_point[3],
                          q2_fx_stats *stats)
{
    u32 emitted = 0, f;
    s32 batch = PSX_OT_BATCH_INVALID;

    /*
     * The table's four indices are a GPU packet's Z order; `psx_prim` wants the
     * perimeter. Corners 2 and 3 swap — see the note in draw_groups.
     */
    static const int k_perimeter[4] = { 0, 1, 3, 2 };

    if (sort_area >= 0 && sort_point) {
        gte_sxy sort_xy;
        u16 sort_z;

        if (gte_project_point(gte,
                              sort_point[0] - cam->pos[0],
                              sort_point[1] - cam->pos[1],
                              sort_point[2] - cam->pos[2],
                              &sort_xy, &sort_z)) {
            batch = psx_ot_batch_begin_point(
                        ot, (u32)sort_area, true, (s16)sort_z,
                        sort_point, cam->pos);
        }
    }

    for (f = 0; f < face_count; f++) {
        psx_prim *prim;
        u32 depth = 0;
        bool good = true;
        int c;

        for (c = 0; c < 4; c++) {
            u32 idx = faces[f].v[c];
            if (idx >= vert_count || !ok[idx]) {
                good = false;
                break;
            }
            depth += z[idx];
        }
        if (!good)
            continue;

        if (sort_area >= 0) {
            prim = batch >= 0
                 ? psx_ot_batch_add(ot, batch)
                 : psx_ot_add_bucket_depth(ot, (u32)area_bucket,
                                           (u16)(depth / 4u),
                                           sort_key_for(depth, 4));
        } else {
            prim = psx_ot_add_depth(
                       ot,
                       (u16)bucket_for(ot, depth, 4, cam->sort_range),
                       sort_key_for(depth, 4));
        }
        if (!prim) {
            stats->ot_overflow++;
            break;
        }

        prim->kind = PSX_PRIM_G4;
        for (c = 0; c < 4; c++) {
            int src = k_perimeter[c];
            u32 idx = faces[f].v[src];
            bool semi;

            prim->xy[c] = *(const psx_xy *)&xy[idx];
            unpack_colour(faces[f].colour[src], &prim->rgb[c], &semi);
            if (c == 0)
                prim->semi_transparent = semi;
        }

        emitted++;
        stats->beam_faces_emitted++;
    }

    return emitted;
}

/*
 * Put every live timed beam into the transient pool.
 *
 * This is 0x80048CA8's whole job: it walks the twelve records and calls the
 * same queue function anything else would (0x80048D98 -> 0x80064E64). A timed
 * beam is not a second kind of beam — it is a persistent RECORD that produces
 * an ordinary queued beam every frame it is alive.
 */
static void submit_timed(q2_fx_world *w)
{
    u32 i;

    for (i = 0; i < Q2_FX_TIMED_BEAMS_MAX; i++) {
        const q2_fx_timed_beam *t = &w->timed[i];

        if (t->timer <= 0 || !t->tube)
            continue;

        q2_fx_beam_add(w, t->from, t->to, t->radius, 0,
                       t->tube, t->cap_near, t->cap_far);
    }
}

static u32 draw_beams(q2_fx_world *w, const q2_camera *cam,
                      psx_ot *ot, gte_state *gte)
{
    u32 emitted = 0, n;

    for (n = 0; n < w->beam_count; n++) {
        q2_fx_beam *b = &w->beam[n];
        s32 delta[3], unit[3], step[3], hex[6][3];
        s32 sort_area = -1;
        s32 area_bucket = -1;
        s64 sq;
        s32 len, segments, s;
        int k;

        if (!b->tube)
            continue;

        if (psx_ot_area_active(ot)) {
            u32 resolved;

            if (!psx_ot_area_bucket(ot,
                                    (u32)b->area & 0x7Fu, &resolved))
                continue;
            sort_area = b->area & 0x7F;
            area_bucket = (s32)resolved;
        }

        q2_camera_apply_area_projection(cam, ot, sort_area, gte);

        for (k = 0; k < 3; k++)
            delta[k] = b->to[k] - b->from[k];

        if (!normalise12(delta, unit))
            continue;

        for (k = 0; k < 3; k++)
            step[k] = (unit[k] * Q2_FX_BEAM_SEGMENT) >> Q2_FRAC_12;

        sq = (s64)delta[0] * delta[0] + (s64)delta[1] * delta[1] +
             (s64)delta[2] * delta[2];
        {
            s64 r = 0, bit;
            for (bit = 1; bit * bit <= sq; bit <<= 1)
                ;
            for (bit >>= 1; bit; bit >>= 1) {
                s64 t = r + bit;
                if (t * t <= sq)
                    r = t;
            }
            len = (s32)r;
        }

        segments = (len + Q2_FX_BEAM_SEGMENT - 1) / Q2_FX_BEAM_SEGMENT - 1;
        if (segments < 0)
            continue;

        hexagon(unit, b->radius, hex);

        for (s = 0; s < segments; s++) {
            gte_sxy xy[Q2_FX_BEAM_VERTS];
            u16     z[Q2_FX_BEAM_VERTS];
            bool    ok[Q2_FX_BEAM_VERTS];
            s32     near_pt[3], far_pt[3];
            u32     i;

            for (k = 0; k < 3; k++) {
                near_pt[k] = b->from[k] + step[k] * s;
                far_pt[k]  = b->from[k] + step[k] * (s + 1);
            }

            for (i = 0; i < Q2_FX_BEAM_VERTS; i++) {
                const s32 *anchor = (i < 6) ? near_pt : far_pt;
                u32 h = i % 6u;
                s32 p[3];

                for (k = 0; k < 3; k++)
                    p[k] = anchor[k] + hex[h][k] - cam->pos[k];

                ok[i] = gte_project_point(gte, p[0], p[1], p[2],
                                          &xy[i], &z[i]);
            }

            emitted += draw_beam_ring(ot, gte, cam, b->tube,
                                      Q2_FX_BEAM_TUBE_FACES,
                                      xy, z, ok, Q2_FX_BEAM_VERTS,
                                      sort_area, area_bucket,
                                      near_pt, &w->stats);

            /*
             * The caps close the near end of the first segment and the far end
             * of the last. Both face lists index 0..5 only, and they are each
             * other's reversed winding — the hardware has no two-sided flag, so
             * an end that must be visible from either side is drawn twice.
             */
            if (s == 0 && b->cap_near) {
                emitted += draw_beam_ring(ot, gte, cam, b->cap_near,
                                          Q2_FX_BEAM_CAP_FACES,
                                          xy, z, ok, 6,
                                          sort_area, area_bucket,
                                          near_pt, &w->stats);
            }
            if (s == segments - 1 && b->cap_far) {
                gte_sxy fxy[6];
                u16     fz[6];
                bool    fok[6];

                for (i = 0; i < 6; i++) {
                    fxy[i] = xy[i + 6];
                    fz[i]  = z[i + 6];
                    fok[i] = ok[i + 6];
                }
                emitted += draw_beam_ring(ot, gte, cam, b->cap_far,
                                          Q2_FX_BEAM_CAP_FACES,
                                          fxy, fz, fok, 6,
                                          sort_area, area_bucket,
                                          far_pt, &w->stats);
            }
        }
    }

    return emitted;
}

u32 q2_fx_build_ot(q2_fx_world *w,
                   const q2_camera *cam,
                   u32 viewport,
                   psx_ot *ot,
                   gte_state *gte)
{
    s16 view[3][3];
    gte_matrix gm;
    u32 emitted = 0;

    if (!w || !cam || !ot || !gte)
        return 0;

    camera_basis(cam, view);
    memcpy(gm.m, view, sizeof(gm.m));
    gte_set_rotation(gte, &gm);
    gte_set_translation(gte, 0, 0, 0);

    /*
     * The timed list feeds the transient pool, then the pool is drawn — the
     * order 0x80039080 (update) and 0x800390A4 (draw) run in. Submitting on
     * every viewport would queue each timed beam twice in split screen, so it
     * happens only for the first.
     */
    if (viewport == 0)
        submit_timed(w);

    emitted += draw_groups(w, cam, viewport, ot, gte, view);
    emitted += draw_beams(w, cam, ot, gte);

    return emitted;
}
