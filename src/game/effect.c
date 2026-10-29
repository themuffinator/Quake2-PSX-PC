#include "effect.h"

#include "combat.h"   /* q2_actor: the presentation pass ticks its effect[] slots */
#include "fixed.h"
#include "hud.h"
#include "trig.h"

#include <string.h>
#include <stdlib.h>

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
    { 15,    10,  10000, 9,    1,    1,    1,  0,  0x800596B0u },  /* ITEM_MAT. */
    /*
     * TELEPORT's shift is 13, not 9 — `sra v0, v0, 13` at 0x80028C5C and
     * 0x80028C88 — and it applies to X and Z only: the Y component is
     * `(rand() >> 8) * sign` (0x80028C68), which is not a `draw()` at all. The
     * row is here for its count and its two ramps; the column's own velocities
     * come out of q2_fx_teleport_column, which is the only caller the site has.
     */
    { 15,     0,  0,     13,   1,    0,    1,  0,  0x80028DC8u },  /* TELEPORT  */
    { 15,    25,  3072,  9,    0,    0,    4,  4,  0x8003E0C0u },  /* WATER_SPL */
    { 15,    15,  6144,  10,   1,    1,    1,  2,  0x80049074u }   /* LASER_END */
};

const q2_fx_preset *q2_fx_preset_at(q2_fx_preset_id id)
{
    if ((u32)id >= Q2_FX_PRESET_COUNT)
        return NULL;
    return &k_preset[id];
}

/*
 * The materialise ramp, 0x80059648.
 *
 * The three bits belong to the ITEM, not to a creature: `s3` is `lw 68(s2)`,
 * entity+0x44, and the instruction after the spawn (0x800596B8 `andi v0, s3,
 * 0x70`) is the glow-light test item.c already models as Q2_ITEM_GLOW_*. There
 * is no creature blood table on the disc; see the block in effect.h.
 *
 * The chain has NO final else: an item with none of the three bits set reaches
 * 0x80059680 with the register still holding whatever the function last left in
 * it. That is the same class of defect as the uninitialised fifth argument
 * documented for T_Damage in userfuncs.h, and it is handled the same way — the
 * port picks a defined value and says so, because mirroring an uninitialised
 * register is not reproducible.
 */
u8 q2_fx_item_glow_ramp(u32 item_flags)
{
    if (item_flags & Q2_FX_ITEM_GLOW_R)
        return 1;    /* 0x8009BAE4 */
    if (item_flags & Q2_FX_ITEM_GLOW_G)
        return 11;   /* 0x8009C00C */
    if (item_flags & Q2_FX_ITEM_GLOW_B)
        return 0;    /* 0x8009BA60 */
    return 1;        /* DIVERGENCE: the original leaves this undefined. */
}

s32 q2_fx_item_materialise(q2_fx_world *w, q2_rng *rng, const s32 at[3],
                           u8 area, u32 item_flags)
{
    const q2_fx_preset *p = q2_fx_preset_at(Q2_FX_ITEM_MATERIALISE);
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

    r = q2_fx_ramp_at(w->tab, q2_fx_item_glow_ramp(item_flags));
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

/* InitBlurPoints, 0x8002FD98: the records zeroed (the memset of 192 bytes) and
 * the free stack filled with 1..96, popped from the top. */
static void blur_init(q2_fx_world *w)
{
    u32 i;

    memset(w->blur, 0, sizeof(w->blur));
    for (i = 0; i < Q2_FX_BLUR_POINTS; i++)
        w->blur_free[i] = (u8)(i + 1);
    w->blur_free_top = Q2_FX_BLUR_POINTS;
}

void q2_fx_world_init(q2_fx_world *w, const q2_fx_tables *tab)
{
    if (!w)
        return;

    memset(w, 0, sizeof(*w));
    blur_init(w);
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
    if (!w->flash_clut)
        w->flash_clut = clut;
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
        /* The creature muzzle flash shares the page and takes palette 64:
         * 0x800315AC loads [0x800E3FAC], 128 bytes into the same table. */
        const q2_hud_palette *flash =
            q2_hud_palette_get(font->tab, Q2_FX_FLASH_PALETTE_ID);

        w->flash_clut = flash ? flash->clut_id : font->clut_font;
        q2_fx_set_texture(w, font->tpage,
                          pal ? pal->clut_id : font->clut_font);
    }
}

bool q2_fx_flash_add(q2_fx_world *w, const s32 corner[4][3],
                     const s32 centre[3], u8 area)
{
    q2_fx_flash *f;

    if (!w || !corner || !centre)
        return false;
    if (w->flash_count < Q2_FX_FLASH_MAX) {
        f = &w->flash[w->flash_count];
    } else {
        u32 index = w->flash_count - Q2_FX_FLASH_MAX;
        if (index >= w->flash_extra_capacity) {
            u32 capacity;
            q2_fx_flash *extra;
            if (w->flash_extra_capacity > UINT32_MAX / 2)
                return false;
            capacity = w->flash_extra_capacity ? w->flash_extra_capacity * 2
                                                : Q2_FX_FLASH_MAX;
            if ((u64)capacity * sizeof(*extra) > SIZE_MAX)
                return false;
            extra = realloc(w->flash_extra, (size_t)capacity * sizeof(*extra));
            if (!extra) return false;
            w->flash_extra = extra;
            w->flash_extra_capacity = capacity;
        }
        f = &w->flash_extra[index];
    }
    w->flash_count++;
    memcpy(f->corner, corner, sizeof(f->corner));
    vec_copy(f->centre, centre);
    f->area = area;
    return true;
}

void q2_fx_flash_clear(q2_fx_world *w)
{
    if (w) {
        free(w->flash_extra);
        w->flash_extra = NULL;
        w->flash_extra_capacity = 0;
        w->flash_count = 0;
    }
}

s32 q2_fx_flash_angle(s32 spin, u32 frame)
{
    /* 0x800310FC: `andi v0, a0, 1` on the frame counter. */
    return (frame & 1u) ? (spin & 0xFFF) : ((spin - 128) & 0xFFF);
}

void q2_fx_flash_corners(const s32 centre[3], s32 angle, s32 size, s32 out[4][3])
{
    /* 0x80031288..0x800312B4: the table's cosine and sine scaled by the size
     * and divided by 4096 toward zero. */
    s32 c = (q2_cos12(angle) * size) / 4096;
    s32 s = (q2_sin12(angle) * size) / 4096;
    int k;

    if (!centre || !out)
        return;
    out[0][0] = centre[0] - c;  out[0][1] = centre[1] - s;
    out[1][0] = centre[0] + s;  out[1][1] = centre[1] - c;
    out[2][0] = centre[0] - s;  out[2][1] = centre[1] + c;
    out[3][0] = centre[0] + c;  out[3][1] = centre[1] + s;
    for (k = 0; k < 4; k++)
        out[k][2] = centre[2];
}

void q2_fx_world_resize(q2_fx_world *w, s32 requested, u32 viewports)
{
    u32 groups;
    if (!w)
        return;

    /* 0x80030C88: a zero argument means the shipped default. */
    groups = requested < 0 ? 0 : requested == 0 ? Q2_FX_GROUPS_DEFAULT : (u32)requested;
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
    w->budget_left    = w->budget;
}

void q2_fx_world_clear(q2_fx_world *w)
{
    u32 groups, views;
    s16 scale;
    u16 tpage, clut, flash_clut;
    bool untextured;
    const q2_fx_tables *tab;

    if (!w)
        return;

    q2_fx_flash_clear(w);

    groups = w->group_count;
    views  = w->viewport_count;
    scale  = w->size_scale;
    tab    = w->tab;

    /* The image registration survives a clear. Emptying the pools is what the
     * level start does; it does not un-upload the texture, and letting the
     * memset zero the flag would leave every later quad sampling page 0. */
    tpage      = w->tpage_base;
    clut       = w->clut;
    flash_clut = w->flash_clut;
    untextured = w->untextured;

    memset(w, 0, sizeof(*w));
    blur_init(w);

    w->tab        = tab;
    w->size_scale = scale;
    w->tpage_base = tpage;
    w->clut       = clut;
    w->flash_clut = flash_clut;
    w->untextured = untextured;
    q2_fx_world_resize(w, groups ? (s32)groups : -1, views);
}

/* ------------------------------------------------------------------------- */
/* The blur trail — BLUR.C                                                    */
/* ------------------------------------------------------------------------- */
/* Return a chain to the free stack, following the negated links across
 * samples to the end (0x8002F6C0..0x8002F6FC, 0x8002FCC4..0x8002FD00). */
static void blur_free_chain(q2_fx_world *w, s32 index)
{
    while (index != 0) {
        if (index < 0)
            index = -index;
        if (index > Q2_FX_BLUR_POINTS)
            break;
        if (w->blur_free_top < Q2_FX_BLUR_POINTS)
            w->blur_free[w->blur_free_top++] = (u8)index;
        index = w->blur_pt[index].next;
    }
}

bool q2_fx_blur_add(q2_fx_world *w, u16 owner, u16 first_vert,
                    const s32 (*pts)[3], u32 count, const u8 rgb[3],
                    s16 life, u8 area)
{
    u32 key = (u32)owner | ((u32)first_vert << 16);
    q2_fx_blur *b = NULL;
    u8 *link;
    u8  old_head;
    u32 i;
    int k;

    if (!w || !pts || !rgb || count == 0 || key == 0)
        return false;

    /* 0x8002F2D0: the record already keyed on this owner and vertex... */
    for (i = 0; i < Q2_FX_BLURS; i++)
        if (w->blur[i].key == key) {
            b = &w->blur[i];
            break;
        }

    if (b) {
        b->stamp = w->blur_clock;                 /* 0x8002F314 */
    } else {
        /* ...or the first free one (a zero key, 0x8002F324..0x8002F368). */
        for (i = 0; i < Q2_FX_BLURS; i++)
            if (w->blur[i].key == 0) {
                b = &w->blur[i];
                break;
            }
        if (!b)
            return false;                         /* "Too many blurs" */
        b->key   = key;
        b->head  = 0;
        b->stamp = w->blur_clock;
        b->rgb[0] = rgb[0];
        b->rgb[1] = rgb[1];
        b->rgb[2] = rgb[2];
        b->life  = life;
        for (k = 0; k < 3; k++)
            b->origin[k] = pts[0][k];             /* 0x8002F3D8 */
    }
    b->area = area;

    /*
     * The new sample, prepended: the first point popped becomes the head, each
     * next one the link of the one before, and the last links to the old head
     * negated. An empty stack hands out index 0, which ends the chain there and
     * writes the point into the dummy slot (0x8002F42C).
     */
    old_head = b->head;
    link = &b->head;
    for (i = 0; i < count; i++) {
        u8 index = 0;
        q2_fx_blur_point *p;

        if (w->blur_free_top > 0)
            index = w->blur_free[--w->blur_free_top];
        *link = index;
        p = &w->blur_pt[index];
        for (k = 0; k < 3; k++)
            p->d[k] = (s16)(pts[i][k] - b->origin[k]);
        link = (u8 *)&p->next;
    }
    *link = (u8)(-(s32)old_head);                 /* 0x8002F4DC */
    w->stats.blur_samples++;
    b->span = (s8)(count - 1);                    /* 0x8002F4E0 */
    return true;
}

void q2_fx_blur_release(q2_fx_world *w, u16 owner)
{
    u32 i;

    if (!w)
        return;
    for (i = 0; i < Q2_FX_BLURS; i++) {
        q2_fx_blur *b = &w->blur[i];

        if (b->key == 0 || (u16)(b->key & 0xFFFFu) != owner)
            continue;
        blur_free_chain(w, b->head);
        b->key = 0;
    }
}

/*
 * The housekeeping half of ProcessBlurs, once a frame: a record past its life
 * is freed (0x8002F65C..0x8002F708), and a sample whose share has run below
 * zero is cut from the chain with everything older (0x8002FBA0, 0x8002FCB0).
 * The console does the cut while it draws; here it happens before the first
 * viewport draws, which is the same frame.
 */
static void blur_expire(q2_fx_world *w)
{
    u32 i;

    for (i = 0; i < Q2_FX_BLURS; i++) {
        q2_fx_blur *b = &w->blur[i];
        s32 remaining, index;

        if (b->key == 0)
            continue;
        remaining = b->stamp + b->life - w->blur_clock;
        if (remaining < 1) {
            blur_free_chain(w, b->head);
            b->key = 0;
            continue;
        }

        index = b->head;
        while (index > 0) {
            s32 last = index, next;

            while ((next = w->blur_pt[last].next) > 0)
                last = next;
            if (next == 0)
                break;
            if (--remaining == -1) {
                w->blur_pt[last].next = 0;
                blur_free_chain(w, -next);
                break;
            }
            index = -next;
        }
    }
}

/* The colour of a sample `remaining` frames from its end (0x8002F740..). */
static void blur_shade(const q2_fx_blur *b, s32 remaining, psx_rgb *out)
{
    out->r   = (u8)(((s32)b->rgb[0] * remaining) / b->life);
    out->g   = (u8)(((s32)b->rgb[1] * remaining) / b->life);
    out->b   = (u8)(((s32)b->rgb[2] * remaining) / b->life);
    out->pad = 0;
}

static u32 draw_blurs(q2_fx_world *w, const q2_camera *cam, psx_ot *ot,
                      gte_state *gte)
{
    u32 emitted = 0, i;
    bool area_routed = psx_ot_area_active(ot);

    for (i = 0; i < Q2_FX_BLURS; i++) {
        const q2_fx_blur *b = &w->blur[i];
        gte_sxy xy[2][Q2_FX_BLUR_SPAN_MAX];
        bool    ok[2][Q2_FX_BLUR_SPAN_MAX];
        u32     n[2] = { 0, 0 };
        psx_rgb colour, prev_colour;
        s32 remaining, index, gen = 0;
        u32 bucket = 0;
        int cur = 0;

        if (b->key == 0 || b->life <= 0)
            continue;
        remaining = b->stamp + b->life - w->blur_clock;
        if (remaining < 1)
            continue;

        if (area_routed && !psx_ot_area_bucket(ot, b->area & 0x7Fu, &bucket))
            continue;
        q2_camera_apply_area_projection(cam, ot,
                                        area_routed ? (s32)(b->area & 0x7Fu)
                                                    : -1,
                                        gte);

        blur_shade(b, remaining, &colour);
        prev_colour = colour;
        index = b->head;

        while (index > 0) {
            u16 z = 0;
            s32 next = 0;
            u32 q;

            /* One sample: every point to the screen, the last one's depth
             * kept for the sort (0x8002F8EC..0x8002F9B8). */
            n[cur] = 0;
            while (index > 0) {
                const q2_fx_blur_point *p = &w->blur_pt[index];
                u32 slot = n[cur];

                if (slot < Q2_FX_BLUR_SPAN_MAX) {
                    ok[cur][slot] = gte_project_point(
                        gte,
                        b->origin[0] + p->d[0] - cam->pos[0],
                        b->origin[1] + p->d[1] - cam->pos[1],
                        b->origin[2] + p->d[2] - cam->pos[2],
                        &xy[cur][slot], &z);
                    n[cur]++;
                }
                next  = p->next;
                index = next > 0 ? next : 0;
            }

            /* From the second sample on, a quad per neighbouring pair between
             * this sample and the newer one (0x8002FA14..0x8002FB8C): corners
             * in libgpu's Z order, this sample's colour on the first two. */
            for (q = 0; gen > 0 && (s32)q < b->span; q++) {
                psx_prim *prim;
                int c;

                if (q + 1 >= n[cur] || q + 1 >= n[cur ^ 1])
                    break;
                if (!ok[cur][q] || !ok[cur][q + 1] ||
                    !ok[cur ^ 1][q] || !ok[cur ^ 1][q + 1])
                    continue;

                prim = area_routed
                     ? psx_ot_add_bucket_depth(ot, bucket, z, z)
                     : psx_ot_add_depth(ot,
                                        (u16)bucket_for(ot, z, 1,
                                                        cam->sort_range),
                                        sort_key_for(z, 1));
                if (!prim) {
                    w->stats.ot_overflow++;
                    return emitted;
                }
                prim->kind = PSX_PRIM_G4;
                prim->quad_zorder = true;
                prim->xy[0].x = xy[cur][q].x;
                prim->xy[0].y = xy[cur][q].y;
                prim->xy[1].x = xy[cur][q + 1].x;
                prim->xy[1].y = xy[cur][q + 1].y;
                prim->xy[2].x = xy[cur ^ 1][q].x;
                prim->xy[2].y = xy[cur ^ 1][q].y;
                prim->xy[3].x = xy[cur ^ 1][q + 1].x;
                prim->xy[3].y = xy[cur ^ 1][q + 1].y;
                for (c = 0; c < 4; c++)
                    prim->rgb[c] = c < 2 ? colour : prev_colour;
                prim->semi_transparent = true;
                prim->textured_blend   = false;
                emitted++;
                w->stats.blur_quads++;
            }

            if (next == 0)
                break;
            index = -next;
            if (--remaining < 0)
                break;
            prev_colour = colour;
            blur_shade(b, remaining, &colour);
            cur ^= 1;
            gen++;
        }
    }
    return emitted;
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
    /* The console's integrator is the tail of the draw, so a group spawned
     * between two draws is drawn before it is aged. This port's integrator runs
     * ahead of the draw; the flag buys back that one tick (effect.h). */
    g->fresh   = true;
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

/* ------------------------------------------------------------------------- */
/* The THIRD spawner — 0x8002FDFC                                             */
/* ------------------------------------------------------------------------- */
s32 q2_fx_group_spawn_points(q2_fx_world *w,
                             const s32 (*pts)[3], const s16 (*vel)[3],
                             u32 count,
                             const q2_fx_ramp *ramp0, const q2_fx_ramp *ramp1,
                             u32 life, s32 size, u8 area)
{
    q2_fx_group *g;
    s32 slot;
    u32 i;

    if (!w || !pts || !vel || count == 0)
        return -1;

    /*
     * Everything but the offsets is 0x80030284 instruction for instruction —
     * the free-slot scan at 0x8002FE08, particle 0's three words at 0x8002FE88
     * (which is `pts[0]`, so it doubles as the origin), the six-byte velocity
     * copy at 0x8002FEB8, the relative velocities at 0x8002FF84, the accel
     * memset at 0x8002FFC4, the size divide at 0x8002FFE4 and the view_mask
     * clear at 0x80030000. So this layers on the first spawner the same way
     * q2_fx_group_spawn_offsets does rather than duplicating it.
     *
     * One difference the layering hides: 0x8002FDFC never clears the offset
     * block — its only memset, 0x8002FFC4, is the six bytes of accel — so the
     * followers past count-1 keep whatever the slot's last occupant left. The
     * zeroes the port writes there instead are unobservable, because the
     * integrator and the renderer both stop at `count`.
     */
    slot = q2_fx_group_spawn(w, pts[0], vel, count, ramp0, ramp1,
                             life, size, area);
    if (slot < 0)
        return slot;

    if (count > Q2_FX_GROUP_QUADS)
        count = Q2_FX_GROUP_QUADS;
    g = &w->group[slot];

    /*
     * 0x8002FF38..0x8002FF70. THE SHIFT IS ABSENT, and that is the whole point
     * of this spawner:
     *
     *     8002FF44  subu v0, v0, v1     ; and 0x80030174, in the SECOND
     *     8002FF48  sh   v0, 12(a0)     ; spawner, is `sll 16 / sra 20`
     *
     * so the follower's offset is the raw 16-bit difference between two world
     * points, not that difference divided by sixteen. The mesh drawers below
     * hand it points sampled off a posed model at world scale; putting the >>4
     * back would shrink a soldier's crackle to a sixteenth of the body.
     *
     * The original works in halfwords throughout — `lhu` both, `subu`, `sh` —
     * so the difference is taken modulo 2^16, which the cast reproduces.
     */
    for (i = 1; i < count && i <= Q2_FX_GROUP_FOLLOWERS; i++) {
        int k;
        for (k = 0; k < 3; k++)
            g->offset[i - 1][k] = (s16)(pts[i][k] - pts[0][k]);
    }

    return slot;
}

/* ------------------------------------------------------------------------- */
/* The mesh drawers                                                           */
/* ------------------------------------------------------------------------- */
/*
 * The view-mask nibble, 0x800590D8..0x80059128 and its three clones.
 *
 * The word at group+0xC4 is masked with 0xFFFFFF0F and OR-ed with
 * `((1 << p) & 0xF) << 4`, where p is the player index the original derives by
 * dividing (client - 0x800C7C60) by 224. The port is handed the index instead,
 * because it has one and does not have to recover it from a pointer.
 *
 * `sllv` takes the shift modulo 32, so a nonsense index cannot be undefined on
 * the console; masking with 31 here keeps that true in C, where `1u << 40` is
 * not. The `& 0xF` afterwards is the original's own clamp to four viewports.
 */
static void fx_view_skip(q2_fx_group *g, s32 player)
{
    if (!g || player < 0)
        return;
    g->view_mask = (u8)((g->view_mask & 0x0Fu) |
                        (((1u << ((u32)player & 31u)) & 0xFu) << 4));
}

u32 q2_fx_mesh_crackle(q2_fx_world *w, const q2_fx_mesh_src *src,
                       u32 frame, u8 area, s32 viewport_skip,
                       u8 ramp, u8 life)
{
    s32 pts[Q2_FX_GROUP_QUADS][3];
    s16 vel[Q2_FX_GROUP_QUADS][3];
    const q2_fx_ramp *r;
    s32 remaining, index;
    u32 groups = 0;

    if (!w || !src || !src->vertex)
        return 0;

    /* 0x80058FF8: fifteen memsets of six bytes. Every crackle quad sits still
     * on the body it was sampled from. */
    memset(vel, 0, sizeof(vel));

    /*
     * 0x8005901C `andi s4, [0x800B2DE4], 1` then 0x80059020 `subu s3, s3, s4`.
     * The parity is the STARTING VERTEX, not a frame skip: on even ticks the
     * walk takes 0, 2, 4... and on odd ticks 1, 3, 5..., so the two halves of
     * the mesh alternate and the effect crawls over the whole body.
     */
    index     = (s32)(frame & 1u);
    remaining = (s32)src->total - index;

    r = q2_fx_ramp_at(w->tab, ramp);

    /*
     * 0x80059024 `slti v0, s3, 2` guards the loop and 0x80059130 branches back
     * to it, so this runs until fewer than two vertices are left rather than
     * once: a sixty-vertex model spawns two groups a tick.
     */
    while (remaining >= 2) {
        s32 batch = (remaining < 30) ? (remaining >> 1) : Q2_FX_GROUP_QUADS;
        s32 slot, i;

        /* 0x80059040..0x80059044: s3 -= 2 * s2, BEFORE the sampling loop. */
        remaining -= batch * Q2_FX_CRACKLE_STEP;

        for (i = 0; i < batch; i++) {
            src->vertex(src->ctx, index, pts[i]);
            index += Q2_FX_CRACKLE_STEP;   /* 0x80059068 addiu s4, s4, 2 */
        }

        slot = q2_fx_group_spawn_points(w, (const s32 (*)[3])pts,
                                        (const s16 (*)[3])vel,
                                        (u32)batch, r, r,
                                        life, Q2_FX_CRACKLE_SIZE, area);
        if (slot >= 0) {
            groups++;
            fx_view_skip(&w->group[slot], viewport_skip);
        }
        /* A full pool is NOT an exit: 0x800590B8 skips only the mask write and
         * falls into the same loop test. */
    }

    return groups;
}

s32 q2_fx_spark_scale(s32 t)
{
    /*
     * 0x80058A4C `mult v1, s6` / 0x80058A5C `mfhi t1` / 0x80058A60 `sra v0,
     * t1, 8` / 0x80058A64 `subu v0, v0, v1` where v1 was `sra t, 31`.
     *
     * mfhi followed by `sra 8` is the 64-bit product shifted right by 40, and
     * the trailing subtraction of the sign is the round-toward-zero correction:
     * the compiler's complete signed magic-division sequence.
     *
     * The divisor is 12000. 0x057619F1 == 91625969 == ceil(2^40 / 12000), and
     * the product overshoots 2^40 by 224, which is exactly the residue that
     * magic is supposed to carry. Transcribed as the multiply rather than as
     * `t / 12000` because the multiply is what the instructions do; the two were
     * measured equal on every t in +/-300000, which covers the whole range this
     * site can reach (a 1.3.12 sine times a magnitude of at most 55).
     */
    s64 prod = (s64)t * (s64)Q2_FX_MESH_SPARK_RECIP;
    return (s32)(prod >> 40) + (t < 0 ? 1 : 0);
}

s32 q2_fx_spark_mag(s32 b)
{
    /*
     * 0x80058A18 `addiu v0, v0, -16384`, then 0x80058A1C..0x80058A24
     * `sll s0, v0, 1 / addu s0, s0, v0 / sll s0, s0, 2` — that is 2x + x = 3x,
     * then 3x * 4 = 12x, not 3x — and 0x80058A28 `sra s0, s0, 13`, arithmetic,
     * so a negative draw floors. 0x80058A38 adds the 32 in a delay slot.
     */
    return Q2_FX_MESH_SPARK_MAG +
           ((Q2_FX_MESH_SPARK_MAG_MUL * (b - 16384)) >>
            Q2_FX_MESH_SPARK_MAG_SHIFT);
}

u32 q2_fx_mesh_spark(q2_fx_world *w, q2_rng *rng, const q2_fx_mesh_src *src,
                     u32 frame, u8 area, s32 viewport_skip,
                     const q2_fx_ramp *ramp0, const q2_fx_ramp *ramp1,
                     s32 size, u8 life)
{
    s32 pts[Q2_FX_GROUP_QUADS][3];
    s16 vel[Q2_FX_GROUP_QUADS][3];
    s32 remaining, index;
    u32 groups = 0;
    u32 i;

    /*
     * 0x800589D8 loads entity+0x10 and 0x800589E0 returns on a null model —
     * BEFORE the velocity loop, so an entity with no model costs no draws at
     * all. A model that merely has no vertices is a different case: the draws
     * happen and the walk then finds nothing, which is what falling through
     * with total == 0 reproduces.
     */
    if (!w || !rng || !src || !src->vertex)
        return 0;

    /*
     * 0x80058A08..0x80058AB4. Fifteen quads, THREE draws each in the order
     * A, B, C — 45 for the burst, and the order is behaviour because the
     * generator is shared with the weapons.
     */
    for (i = 0; i < Q2_FX_GROUP_QUADS; i++) {
        s32 a = q2_rng_next(rng);
        s32 b = q2_rng_next(rng);
        s32 angle, mag, c;

        /* 0x80058A14 is in the JAL's delay slot, so it masks A, not B. */
        angle = a & 0xFFF;

        /* 0x80058A1C `sll 1; addu; sll 2` is *12, not *3, and 0x80058A28's
         * `sra 13` is arithmetic. 32 +/- 24, so mag lands in [8, 55]. */
        mag = q2_fx_spark_mag(b);

        /* `lh 0(tab)` is the SINE column and `lh 2(tab)` the cosine, of the
         * same 4096-step entry — the table trig.h reproduces. */
        vel[i][0] = (s16)q2_fx_spark_scale(q2_sin12(angle) * mag);

        c = q2_rng_next(rng);
        vel[i][1] = (s16)((c - 16384) >> Q2_FX_MESH_SPARK_Y_SHIFT);

        vel[i][2] = (s16)q2_fx_spark_scale(q2_cos12(angle) * mag);
    }

    /* 0x80058AC4 `andi s2, [0x800B2DE4], 7`: eight phases, so the sampled
     * eighth of the mesh rotates once every eight ticks. */
    index     = (s32)(frame & 7u);
    remaining = (s32)src->total - index;

    while (remaining >= Q2_FX_MESH_SPARK_STEP) {
        /* 0x80058AD8 `slti v0, s5, 120` — 120 is 8 * 15. */
        s32 batch = (remaining < 120) ? (remaining >> 3) : Q2_FX_GROUP_QUADS;
        s32 slot, k;

        remaining -= batch * Q2_FX_MESH_SPARK_STEP;

        for (k = 0; k < batch; k++) {
            src->vertex(src->ctx, index, pts[k]);
            index += Q2_FX_MESH_SPARK_STEP;   /* 0x80058B10 */
        }

        slot = q2_fx_group_spawn_points(w, (const s32 (*)[3])pts,
                                        (const s16 (*)[3])vel,
                                        (u32)batch, ramp0, ramp1,
                                        life, size, area);
        if (slot >= 0) {
            groups++;
            fx_view_skip(&w->group[slot], viewport_skip);
        }
    }

    return groups;
}

u32 q2_fx_mesh_blood(q2_fx_world *w, q2_rng *rng, const q2_fx_mesh_src *src,
                     u8 area, const q2_fx_ramp *ramp0, const q2_fx_ramp *ramp1,
                     s32 size, s32 mode)
{
    s32 pts[Q2_FX_GROUP_QUADS][3];
    s16 vel[Q2_FX_GROUP_QUADS][3];
    s32 batches, index = 0;
    u32 groups = 0;
    u32 i;
    u8  life;

    if (!w || !rng)
        return 0;

    /*
     * 0x8005ABBC: `total / 15`, the compiler's signed divide by fifteen with
     * magic 0x88888889. Fifteen is the same fifteen as the batch, so this is
     * "one group per fifteen vertices, covering the mesh in order".
     *
     * A NULL `src` is the model-less entity, and it is NOT an early exit here:
     * 0x8005ABAC loads entity+0x10 and hands it straight to 0x8006D6AC, which
     * returns zero for a null model, and nothing in this function tests the
     * pointer. So the total is zero, the batch count is zero, and the draws
     * below still happen.
     */
    batches = (src && src->vertex)
            ? (s32)src->total / Q2_FX_MESH_BLOOD_BATCH : 0;

    /*
     * The velocities are drawn ONCE and reused by every group — 0x8005ABE4 and
     * 0x8005AC38 both sit outside the group loop — which is why a spray looks
     * like one gesture rather than fifteen independent puffs. And they are
     * drawn BEFORE the emptiness test at 0x8005ACCC, so a model-less entity
     * still costs 45 draws here where the spark costs none.
     */
    if (mode) {
        life = Q2_FX_MESH_BLOOD_LIFE;                /* 0x8005ABE0 */
        for (i = 0; i < Q2_FX_GROUP_QUADS; i++) {
            vel[i][0] = (s16)draw(rng, 10);          /* 0x8005ABF8 sra 10 */
            vel[i][1] = (s16)draw(rng, 10);
            vel[i][2] = (s16)draw(rng, 10);
        }
    } else {
        life = Q2_FX_MESH_BLOOD_ALT_LIFE;            /* 0x8005AC34 */
        for (i = 0; i < Q2_FX_GROUP_QUADS; i++) {
            int k;
            for (k = 0; k < 3; k++) {
                /*
                 * 0x8005AC4C `sll 1; addu` is *3, then the round-toward-zero
                 * divide by 16384:
                 *
                 *     8005AC58  bgez  v1, 0x8005AC64
                 *     8005AC60  addiu v1, v1, 16383   ; NEGATIVE values only
                 *     8005AC64  sra   v0, v1, 14
                 *
                 * The bias is added when the product is BELOW zero — the branch
                 * skips it for everything else. Adding it to the other half is
                 * the easy misreading of `bgez`, and it rounds every positive
                 * component up and every negative one down, throwing the whole
                 * spray up to a unit further out on each axis than the console.
                 */
                s32 v = 3 * (q2_rng_next(rng) - 16384);
                if (v < 0)
                    v += 16383;
                vel[i][k] = (s16)(v >> 14);
            }
        }
    }

    while (batches-- > 0) {
        s32 slot;

        /* 0x8005ACE0: consecutive vertices, and s4 is never reset between
         * batches, so the groups tile the mesh rather than overlapping. */
        for (i = 0; i < Q2_FX_GROUP_QUADS; i++) {
            src->vertex(src->ctx, index, pts[i]);
            index++;
        }

        slot = q2_fx_group_spawn_points(w, (const s32 (*)[3])pts,
                                        (const s16 (*)[3])vel,
                                        Q2_FX_GROUP_QUADS, ramp0, ramp1,
                                        life, size, area);
        if (slot >= 0) {
            groups++;
            /* 0x8005AD38..0x8005AD48: only the mode != 0 arm, and only when
             * the spawner returned a slot. accel[1] = 3 — the spray sags. */
            if (mode)
                w->group[slot].accel[1] = Q2_FX_MESH_BLOOD_ACCEL_Y;
        }
    }

    return groups;
}

u32 q2_fx_gib_spray(q2_fx_world *w, q2_rng *rng, const q2_fx_mesh_src *src,
                    u8 area)
{
    const q2_fx_ramp *r0, *r1;

    if (!w)
        return 0;

    /* 0x8005B320: a1 = 0x8009BB68 (ramp 2), a2 = 0x8009BBEC (ramp 3),
     * a3 = 6144, and a fifth argument of 1 at sp+16 — the MODE, not a life. */
    r0 = q2_fx_ramp_at(w->tab, Q2_FX_MESH_BLOOD_RAMP0);
    r1 = q2_fx_ramp_at(w->tab, Q2_FX_MESH_BLOOD_RAMP1);
    return q2_fx_mesh_blood(w, rng, src, area, r0, r1,
                            Q2_FX_MESH_BLOOD_SIZE, 1);
}

/* ------------------------------------------------------------------------- */
/* The gib's blood trail — the particle half of 0x80059DE0                    */
/* ------------------------------------------------------------------------- */
s32 q2_fx_gib_trail(q2_fx_world *w, q2_rng *rng, const s32 at[3],
                    const s16 vel[3], u8 area)
{
    s16 base[3], step[3];
    s16 offs[Q2_FX_GIB_TRAIL_COUNT][3];
    s16 v[Q2_FX_GIB_TRAIL_COUNT][3];
    const q2_fx_ramp *r0, *r1;
    u32 i;
    int k;

    if (!w || !rng || !at || !vel)
        return -1;

    /*
     * 0x80059E9C..0x80059ED4: `lhu` the velocity, `sll 2`, `addiu -8192`,
     * `sh` — so the bias is stored as a halfword and read back with `lh` at
     * 0x80059EE4. -8192 is a QUARTER of the draw's range, not half of it, so
     * with the gib at rest every component still averages (16383 - 8192) >>
     * 11, about +4: the trail is not centred on the gib's own motion. That is
     * what the immediate says (0x2442E000), transcribed rather than corrected.
     */
    for (k = 0; k < 3; k++)
        base[k] = (s16)((s32)vel[k] * 4 + Q2_FX_GIB_TRAIL_BIAS);

    /*
     * 0x80059EDC..0x80059F44: fifteen triples, x then y then z, each
     * `(rand() + base) >> 11` with an ARITHMETIC shift. 45 draws per gib per
     * tick, in this order.
     */
    for (i = 0; i < Q2_FX_GIB_TRAIL_COUNT; i++) {
        for (k = 0; k < 3; k++)
            v[i][k] = (s16)((q2_rng_next(rng) + base[k]) >>
                            Q2_FX_GIB_TRAIL_SHIFT);
    }

    /*
     * 0x80059F4C..0x80059FE0: `vel / 15` per axis — magic 0x88888889, the
     * add-back of the dividend, `sra 3`, then the sign subtracted, which is the
     * compiler's signed divide by fifteen and therefore TRUNCATES toward zero.
     * C's `/` is the same operation.
     */
    for (k = 0; k < 3; k++)
        step[k] = (s16)(vel[k] / Q2_FX_GIB_TRAIL_DIV);

    /*
     * 0x80059FE4 zeroes offs[0] and 0x80059FF4..0x8005A038 builds the rest as
     * a running sum, `offs[i] = offs[i - 1] + step` in halfwords — a straight
     * line of fifteen points along the gib's own motion for this frame.
     */
    memset(offs[0], 0, sizeof(offs[0]));
    for (i = 1; i < Q2_FX_GIB_TRAIL_COUNT; i++) {
        for (k = 0; k < 3; k++)
            offs[i][k] = (s16)(offs[i - 1][k] + step[k]);
    }

    /*
     * 0x8005A07C: the SECOND spawner (0x8003004C), a0 = entity+0xA4, a1 = the
     * offsets, a2 = the velocities, a3 = 0x8009BB68 (ramp 2), sp+16 =
     * 0x8009BBEC (ramp 3), count 15, life 3, size 6144, area entity+0x9E. That
     * spawner shifts each offset right by four as it stores it, so the line
     * the gib drew is laid down at a sixteenth of its length.
     */
    r0 = q2_fx_ramp_at(w->tab, Q2_FX_GIB_TRAIL_RAMP0);
    r1 = q2_fx_ramp_at(w->tab, Q2_FX_GIB_TRAIL_RAMP1);
    return q2_fx_group_spawn_offsets(w, at, (const s16 (*)[3])offs,
                                     (const s16 (*)[3])v,
                                     Q2_FX_GIB_TRAIL_COUNT, r0, r1,
                                     Q2_FX_GIB_TRAIL_LIFE,
                                     Q2_FX_GIB_TRAIL_SIZE, area);
}

/* ------------------------------------------------------------------------- */
/* 0x80048588 — the energy bolt's trail                                        */
/* ------------------------------------------------------------------------- */
s32 q2_fx_bolt_trail(q2_fx_world *w, q2_rng *rng, const s32 at[3],
                     const s16 disp[3], u8 area)
{
    s16 offs[Q2_FX_BOLT_TRAIL_COUNT][3];
    s16 v[Q2_FX_BOLT_TRAIL_COUNT][3];
    s16 base[3], step[3];
    s32 origin[3];
    const q2_fx_ramp *r;
    u32 i;
    int k;

    if (!w || !rng || !at || !disp)
        return -1;

    for (k = 0; k < 3; k++) {
        /*
         * 0x80048328 `sra v1, a0, 17` — the halfword is sign-extended into a
         * word by `sll 16` first, so this is an arithmetic halving that rounds
         * toward negative infinity, not C's truncating `/ 2`.
         */
        s32 half = (s32)disp[k] >> 1;

        origin[k] = (s32)((u32)at[k] - (u32)half);

        /* 0x80048364 `sra v0, a0, 20`: the same sign-extension, then >> 4. */
        base[k] = (s16)((s32)disp[k] >> 4);

        /*
         * 0x80048384 `sra a0, a0, 12` then `div` by record+0x4A. The shift is
         * on the ALREADY sign-extended word, so it is `disp << 4`, and the
         * divide is the compiler's signed one and truncates toward zero.
         */
        step[k] = (s16)(((s32)disp[k] * 16) / Q2_FX_BOLT_TRAIL_COUNT);

        /* 0x80048434..0x8004846C — and the spawner throws this entry away. */
        offs[0][k] = (s16)(-half);
        v[0][k]    = base[k];
    }

    /*
     * 0x800484A8..0x8004854C. The chain is halfword arithmetic and wraps as
     * the original's does; the velocities take three `rand()` draws an axis in
     * x, y, z order, and particle 0's are NOT drawn for (0x80048470 copies
     * them), which is why the loop starts at one.
     */
    for (i = 1; i < Q2_FX_BOLT_TRAIL_COUNT; i++) {
        for (k = 0; k < 3; k++) {
            offs[i][k] = (s16)(offs[i - 1][k] + step[k]);
            v[i][k]    = (s16)(base[k] +
                               ((q2_rng_next(rng) - 16384) >>
                                Q2_FX_BOLT_TRAIL_SHIFT));
        }
    }

    /* Both ends are the same ramp — 0x8009BF04 at a3 and again at sp+16
     * (0x80048560, 0x80048564) — so the group does not cross-fade. */
    r = q2_fx_ramp_at(w->tab, Q2_FX_BOLT_TRAIL_RAMP);
    return q2_fx_group_spawn_offsets(w, origin, (const s16 (*)[3])offs,
                                     (const s16 (*)[3])v,
                                     Q2_FX_BOLT_TRAIL_COUNT, r, r,
                                     Q2_FX_BOLT_TRAIL_LIFE,
                                     Q2_FX_BOLT_TRAIL_SIZE, area);
}

/* ------------------------------------------------------------------------- */
/* The rail's spiral — the tail of 0x8004917C                                  */
/* ------------------------------------------------------------------------- */
/* Floor root, also used below to derive the libgte seed table exactly. */
static s32 rail_sqrt(s64 n)
{
    u64 v = (u64)n, r = 0, bit = (u64)1 << 62;

    if (n <= 0)
        return 0;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (s32)r;
}

/* SquareRootNR, 0x80055CBC. The 192 halfwords at 0x800AA340 are exactly
 * floor(sqrt(index * 2^18)), index 64..255 (checked against the disc).
 * Recreate the table entry, normalize like 0x8008A7E8, then perform the TWO
 * signed integer Newton corrections. A floor root differs at segment/width
 * boundaries; e.g. this returns 256 for 65535, not 255. Positive s32 input. */
s32 q2_fx_sqrt_nr(u32 n)
{
    u32 bits = n, leading = 0, normalized, seed;
    s32 root;
    int i;
    if (!n || n > 0x7FFFFFFFu) return 0;
    while (!(bits & 0x80000000u)) { leading++; bits <<= 1; }
    leading &= ~1u;
    normalized = leading >= 24 ? n << (leading - 24) : n >> (24 - leading);
    seed = (u32)rail_sqrt((s64)normalized << 18);
    root = (s32)((seed << ((31 - leading) / 2)) >> 12);
    for (i = 0; i < 2; i++)
        root -= (s32)((u32)root * (u32)root - n) / (2 * root);
    return root;
}

u32 q2_fx_rail_trail(q2_fx_world *w, const s32 start[3], const s32 end[3],
                     const s16 basis[2][3], bool multiplayer, int windows,
                     u8 (*area_at)(void *user, const s32 at[3]), void *user)
{
    s16 offs[Q2_FX_RAIL_PER_SEG][3], vel[Q2_FX_RAIL_PER_SEG][3];
    s32 d[3], step[3], pstep[3], at[3];
    s32 segs, per, turns, astep, angle = 0, n;
    const q2_fx_ramp *r;
    u32 groups = 0;
    int i, k;
    u8 area;

    if (!w || !start || !end || !basis)
        return 0;

    for (k = 0; k < 3; k++)
        d[k] = end[k] - start[k];

    segs = (rail_sqrt((s64)d[0] * d[0] + (s64)d[1] * d[1] + (s64)d[2] * d[2])
            >> Q2_FX_RAIL_SEG_SHIFT) + 1;

    if (multiplayer) {
        per   = Q2_FX_RAIL_PER_SEG_MP;
        turns = windows < Q2_FX_RAIL_MP_TWO_TURNS ? 1 : 2;
    } else {
        per   = Q2_FX_RAIL_PER_SEG;
        turns = 1;
    }

    /* Both divides are the compiler's signed ones and truncate toward zero. */
    for (k = 0; k < 3; k++) {
        step[k]    = d[k] / segs;
        pstep[k]   = (step[k] << 4) / per;
        offs[0][k] = 0;
    }
    for (i = 1; i < per; i++)
        for (k = 0; k < 3; k++)
            offs[i][k] = (s16)(offs[i - 1][k] + (s16)pstep[k]);

    astep = (turns << 12) / per;
    for (i = 0; i < per; i++, angle += astep) {
        s32 cs = q2_cos12(angle & 0xFFF), sn = q2_sin12(angle & 0xFFF);

        for (k = 0; k < 3; k++) {
            s16 v = (s16)(((s32)basis[0][k] * cs +
                           (s32)basis[1][k] * sn) >> Q2_FRAC_12);

            offs[i][k] = (s16)(offs[i][k] + (v >> Q2_FX_RAIL_RING_SHIFT));
            vel[i][k]  = (s16)(v >> Q2_FX_RAIL_DRIFT_SHIFT);
        }
    }

    r = q2_fx_ramp_at(w->tab, Q2_FX_RAIL_RAMP);
    for (k = 0; k < 3; k++)
        at[k] = start[k];
    area = area_at ? area_at(user, at) : 0;

    for (n = 0; n < (segs & ~1); n += 2) {
        if (q2_fx_group_spawn_offsets(w, at, (const s16 (*)[3])offs,
                                      (const s16 (*)[3])vel, Q2_FX_RAIL_GROUP,
                                      r, r, Q2_FX_RAIL_LIFE, Q2_FX_RAIL_SIZE,
                                      area) >= 0)
            groups++;
        if (per == Q2_FX_RAIL_PER_SEG &&
            q2_fx_group_spawn_offsets(w, at,
                                      (const s16 (*)[3])offs[Q2_FX_RAIL_GROUP],
                                      (const s16 (*)[3])vel[Q2_FX_RAIL_GROUP],
                                      Q2_FX_RAIL_GROUP, r, r, Q2_FX_RAIL_LIFE,
                                      Q2_FX_RAIL_SIZE, area) >= 0)
            groups++;

        /* The step is added as the halfword it was stored as (`lh 72(s2)`). */
        for (k = 0; k < 3; k++)
            at[k] += (s16)step[k];
        if (area_at)
            area = area_at(user, at);
    }

    if (q2_fx_group_spawn_offsets(w, at, (const s16 (*)[3])offs,
                                  (const s16 (*)[3])vel, Q2_FX_RAIL_GROUP,
                                  r, r, Q2_FX_RAIL_LIFE, Q2_FX_RAIL_SIZE_LAST,
                                  area) >= 0)
        groups++;
    return groups;
}

/* ------------------------------------------------------------------------- */
/* 0x80058638 — the effect[1] dispatcher, which also OWNS the countdown       */
/* ------------------------------------------------------------------------- */
void q2_fx_actor_damage_effect(q2_fx_world *w, struct q2_actor *a,
                               const q2_fx_mesh_src *src,
                               u32 frame, u8 area, s32 viewport_skip,
                               u8 dt, q2_fx_present_report *out)
{
    u8 armed;

    if (out)
        memset(out, 0, sizeof(*out));
    if (!w || !a)
        return;

    /* 0x80058650 / 0x80058658: a slot at zero leaves through the exit that
     * SKIPS the subtraction, so the byte never underflows from rest. */
    armed = a->effect[1];
    if (armed == 0)
        return;

    if (armed < 3) {                     /* 0x80058660 sltiu v0, v0, 3 */
        u32 g = q2_fx_mesh_crackle(w, src, frame, area, viewport_skip,
                                   Q2_FX_CRACKLE_DAMAGE_RAMP,
                                   Q2_FX_CRACKLE_DAMAGE_LIFE);
        if (out)
            out->groups += g;

        /*
         * 0x80058700 RE-READS entity+0x2F1 and 0x80058708 branches away unless
         * it is exactly 2, so the `< 3` arm is not silent: at 2 it raises a
         * light through 0x80075D14, which is a DIFFERENT function from the
         * `>= 3` arm's 0x80075C34 and takes a fourth argument of 4
         * (0x80058718). The re-read is the same value here because the drawer
         * cannot touch the byte.
         *
         * 0x80075D14 is AddTimedLight and that 4 is a LIFETIME, not a style or
         * a radius: the light is re-emitted for four frames at 4/4, 3/4, 2/4
         * and 1/4 of 800/1300 (lighting.h, and Q2_ENERGY_PULSE_TICKS in
         * effect.h). The effect's illumination therefore outlives the effect's
         * own countdown, which is the part the port was missing — not just one
         * unraised light but the whole fade-out.
         */
        if (armed == 2 && out)
            out->energy_pulse = true;
    } else {
        u32 g = q2_fx_mesh_crackle(w, src, frame, area, viewport_skip,
                                   Q2_FX_CRACKLE_ENERGY_RAMP,
                                   Q2_FX_CRACKLE_ENERGY_LIFE);
        if (out) {
            out->groups += g;
            /* 0x800586D0: 0x80075C34 with 0x800AEAAC's colour and the radii at
             * 0x800AEAB0 / 0x800AEAB4 — combat.h's Q2_ENERGY_LIGHT_*. */
            out->energy_light = true;
            /* 0x800586E8: the light's own four bytes are copied into
             * entity+0x2AC, so the actor is tinted by it for this tick. */
            out->set_ambient  = true;
        }
    }

    /*
     * 0x80058760..0x8005876C. THIS is where effect[1] runs down — not in the
     * combat step, and not by a fixed 1: by the caller's second argument. Both
     * drivers pass 1. The subtraction is a plain byte `subu`/`sb` with no
     * clamp, so a dt larger than the slot wraps, exactly as the console does.
     */
    a->effect[1] = (u8)(armed - dt);
}

/* ------------------------------------------------------------------------- */
/* 0x8005B880 — the per-actor presentation chain                              */
/* ------------------------------------------------------------------------- */
/*
 * One ticker. 0x8005B708..0x8005B72C and its three clones:
 *
 *     if (slot) { emit(); slot -= dt; }
 *
 * The emit does NOT depend on dt, so a slot ticked with dt 0 emits for as long
 * as its entity is presented. On the console that is bounded from OUTSIDE this
 * chain: a corpse think's gate 0x8005B2A8 hands a body with effect[0] or
 * effect[2] armed to a dissolve handler that frees it (effect.h).
 */
static u32 fx_tick_spark(q2_fx_world *w, q2_rng *rng, const q2_fx_mesh_src *src,
                         u32 frame, u8 area, s32 viewport_skip,
                         u8 *slot, u8 dt)
{
    const q2_fx_ramp *r;
    u32 g;

    if (*slot == 0)
        return 0;

    /* 0x8005B624 / 0x8005B658 / 0x8005B68C are byte-identical: one ramp
     * pointer passed twice, size 32767, life 4. */
    r = q2_fx_ramp_at(w->tab, Q2_FX_MESH_SPARK_RAMP);
    g = q2_fx_mesh_spark(w, rng, src, frame, area, viewport_skip, r, r,
                         Q2_FX_MESH_SPARK_SIZE, Q2_FX_MESH_SPARK_LIFE);

    *slot = (u8)(*slot - dt);
    return g;
}

void q2_fx_actor_present(q2_fx_world *w, q2_rng *rng, struct q2_actor *a,
                         const q2_fx_mesh_src *src,
                         u32 frame, u8 area, s32 viewport_skip,
                         s32 level_time, s32 quad_until,
                         q2_fx_present_report *out)
{
    q2_fx_present_report local;
    u8 dt_alive;

    if (!out)
        out = &local;
    memset(out, 0, sizeof(*out));
    if (!w || !a)
        return;

    /*
     * 0x8005B8A0 `lh s1, 264(s0)` then 0x8005B8AC `slt s1, zero, s1`: dt for
     * slots 0 and 2 is (health > 0). Slots 4 and 5 get a literal 1
     * (0x8005B8D0, 0x8005B8E4), so they run down on a corpse and those two do
     * not. Giving all four the health gate latches effect[4] and effect[5] on
     * for as long as the corpse lasts: the corpse gate 0x8005B2A8 only
     * releases a body for effect[2] or effect[0] (effect.h).
     */
    dt_alive = (a->health > 0) ? 1u : 0u;

    /* 0x8005B8A8, and it zeroes `out`. */
    q2_fx_actor_damage_effect(w, a, src, frame, area, viewport_skip, 1, out);

    /* 0x8005B8B4 — effect[0], armed with 15 by Q2_MOD_2. */
    out->groups += fx_tick_spark(w, rng, src, frame, area, viewport_skip,
                                 &a->effect[0], dt_alive);

    /* 0x8005B8C0 — effect[2], armed with 30 by Q2_MOD_4. */
    out->groups += fx_tick_spark(w, rng, src, frame, area, viewport_skip,
                                 &a->effect[2], dt_alive);

    /* 0x8005B8CC — effect[4], armed with 5 by the hyperblaster's own mod,
     * dt a constant 1. */
    out->groups += fx_tick_spark(w, rng, src, frame, area, viewport_skip,
                                 &a->effect[4], 1);

    /*
     * 0x8005B7E4, FOURTH in the chain. Five instructions:
     *
     *     8005B7EC  lw   v0, 12(a0)    ; the client — no client, no shell,
     *     8005B7F4  beq  v0, zero      ; whatever the inventory says
     *     8005B7FC  lw   v1, 172(v0)   ; client+0xAC, the deadline
     *     8005B804  lw   v0, [0x800AEBAC]
     *     8005B80C  sltu v0, v0, v1    ; UNSIGNED, and strict
     *
     * The compare being unsigned is not decorative: a deadline that has been
     * left negative reads as an enormous positive one and the shell stays on.
     * Strict means `level_time == quad_until` is already expired.
     */
    if (a->has_client && (u32)level_time < (u32)quad_until)
        out->groups += q2_fx_mesh_crackle(w, src, frame, area, viewport_skip,
                                          Q2_FX_QUAD_SHELL_RAMP,
                                          Q2_FX_QUAD_SHELL_LIFE);

    /*
     * 0x8005B830 — effect[5] at entity+0x2F5, dt a constant 1. Its emitter
     * 0x80059168 is a clone of the CRACKLE (step 2, zero velocities), not of
     * the random-velocity spark, and its ramp is 0.
     */
    if (a->effect[5]) {
        out->groups += q2_fx_mesh_crackle(w, src, frame, area, viewport_skip,
                                          Q2_FX_CRACKLE_SLOT5_RAMP,
                                          Q2_FX_CRACKLE_SLOT5_LIFE);
        a->effect[5] = (u8)(a->effect[5] - 1u);
    }

    /* effect[3] is deliberately absent: 0x8005B880 never touches +0x2F3, and
     * `q2psx-inspect access 0x2F3` finds no immediate-offset load or store of
     * it in the main executable (the tool does not scan the relocated
     * modules). So neither does this. */
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
         * A GROUP RAISED SINCE THE LAST DRAW IS NOT AGED YET.
         *
         * On the console this pass is the tail of the draw itself — 0x80030B1C
         * reads the life, 0x80030B24 branches out when it is zero and
         * 0x80030B28/0x80030B30 write it back one lower — so a burst the
         * gameplay code raised during the frame is DRAWN at its full life and
         * only then stepped. The port runs this from the sim tick, ahead of the
         * client's draw, which aged a burst before any frame could show it: a
         * life-1 group (the quad shell, the energy crackle) died unseen and
         * every other burst started one ramp entry down. Skipping the group the
         * one time restores the console's net order (effect.h, q2_fx_group).
         */
        if (g->fresh) {
            g->fresh = false;
            w->stats.groups_live++;
            continue;
        }

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
     * The teleport column draws its lifetime and size before its velocities
     * (0x80028D84 and 0x80028D90), so the generator's sequence differs from
     * every other preset's. Reproducing the order is what keeps a seeded replay
     * identical.
     */
    if (id == Q2_FX_TELEPORT) {
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
/* The teleport column — DrawTelePorts, 0x80028BDC                            */
/* ------------------------------------------------------------------------- */
s32 q2_fx_teleport_sign(s32 level_time)
{
    /*
     * 0x80028BE8..0x80028C24. `s4` starts at +1 and becomes -1 unless
     * `LevelTicks & 0x7F` is below 63 — an `sltiu`, so it is a 128-tick saw
     * with 63 ticks of rise and 65 of fall, not a symmetric one. At 300 ticks
     * to the second that is a little over two cycles a second.
     */
    return ((u32)level_time & 0x7Fu) < 63u ? 1 : -1;
}

void q2_fx_teleport_velocities(q2_rng *rng, s32 sign,
                               s16 vel[Q2_FX_GROUP_QUADS][3])
{
    u32 i;

    if (!rng || !vel)
        return;

    /*
     * DRAWN ONCE PER FRAME AND SHARED BY EVERY PAD. The fifteen triples go
     * into the stack buffer at sp+32 before the walk over the registered list
     * begins (0x80028C34..0x80028C9C is above the `blez` on NumTelePorts at
     * 0x80028CA8), and nothing inside the walk touches that buffer. So a map
     * with seven pads draws 45 velocities a frame, not 315, and every pad's
     * column leans the same way on the same frame.
     *
     * X and Z are `(rand() - 16384) >> 13` — a range of -4..3, much tighter
     * than any other site's shift of 9 or 10 — and Y is `(rand() >> 8) * sign`
     * (0x80028C68 `sra v0, 8`, 0x80028C6C `mult v0, s1`), which is 0..127 with
     * no bias subtracted at all. That asymmetry IS the column: the quads
     * stream steadily up or down and barely spread sideways.
     */
    for (i = 0; i < Q2_FX_GROUP_QUADS; i++) {
        vel[i][0] = (s16)draw(rng, 13);
        vel[i][1] = (s16)((q2_rng_next(rng) >> 8) * sign);
        vel[i][2] = (s16)draw(rng, 13);
    }
}

s32 q2_fx_teleport_column(q2_fx_world *w, q2_rng *rng,
                          const s16 vel[Q2_FX_GROUP_QUADS][3],
                          const s32 centre[3], u8 area)
{
    const q2_fx_preset *p = q2_fx_preset_at(Q2_FX_TELEPORT);
    const q2_fx_ramp *r0, *r1;
    s32 at[3];
    u32 life;
    s32 size;

    if (!w || !rng || !vel || !centre || !p)
        return -1;

    /*
     * The pad's own jitter, in that order: X first (0x80028D54), then Z
     * (0x80028D6C). `centre - 127 + (rand() & 0xFF)` is 128 units either side,
     * so the column wanders across the pad rather than rising from one point.
     * Y is left where the caller put it — the mins or maxs face, chosen by the
     * sign at 0x80028D38.
     */
    at[0] = centre[0] - 127 + (s32)(q2_rng_next(rng) & 0xFF);
    at[1] = centre[1];
    at[2] = centre[2] - 127 + (s32)(q2_rng_next(rng) & 0xFF);

    /* 0x80028D8C and 0x80028DA8, the same two expressions q2_fx_spawn
     * special-cases for this preset — and in the same place in the sequence,
     * after the jitter and before the spawn. */
    life = (u32)((q2_rng_next(rng) + 24) & 0xF);
    size = (s32)(q2_rng_next(rng) & 0x2047) + 12000;

    r0 = q2_fx_ramp_at(w->tab, p->ramp0);   /* index[2], 0x8009BAE4: red  */
    r1 = q2_fx_ramp_at(w->tab, p->ramp1);   /* index[0], 0x8009BA60: blue */

    return q2_fx_group_spawn(w, at, vel, p->count, r0, r1, life, size, area);
}

s32 q2_fx_teleport_light_radius(s32 level_time)
{
    /*
     * 0x80028DDC..0x80028E24. A 512-tick triangle on the level clock —
     * `phase = LevelTicks & 0x1FF`, mirrored about 256 by the `slti 257` at
     * 0x80028DFC — scaled by four and offset by 800, so the outer radius
     * breathes between 800 and 1824 while the inner stays at 400.
     */
    s32 phase = (s32)((u32)level_time & 0x1FFu);

    if (phase >= 257)
        phase = 512 - phase;
    return phase * 4 + 800;
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
static bool beam_timed(q2_fx_world *w, s32 owner, s32 target,
                      const s32 from[3], const s32 to[3],
                      s16 radius, u32 style, s16 life, u8 area, s16 attachment)
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
            w->timed[i].owner == owner && w->timed[i].target == target &&
            w->timed[i].attachment == attachment) {
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
    slot->present = life > 0;
    slot->attachment = attachment;
    slot->radius = radius;
    /* The area the submit will spend, resolved by the caller from the owner's
     * own cell — 0x80048D24's helper does it from the owner entity on every
     * submit instead (effect.h, q2_fx_timed_beam.area). */
    slot->area   = (s16)(area & 0x7Fu);
    vec_copy(slot->from, from);
    vec_copy(slot->to, to);
    slot->tube     = s->tube;
    slot->cap_near = s->cap_near;
    slot->cap_far  = s->cap_far;

    return true;
}

bool q2_fx_beam_timed(q2_fx_world *w, s32 owner, s32 target,
                      const s32 from[3], const s32 to[3],
                      s16 radius, u32 style, s16 life, u8 area)
{
    return beam_timed(w, owner, target, from, to, radius, style, life, area, -1);
}

bool q2_fx_beam_attached(q2_fx_world *w, s32 owner, s32 target, s16 vertex,
                         const s32 from[3], const s32 to[3], u8 area)
{
    return beam_timed(w, owner, target, from, to, 10, 4, 45, area, vertex);
}

void q2_fx_timed_tick(q2_fx_world *w, s32 frame_delta)
{
    u32 i;

    if (!w)
        return;

    /* 0x80048CE0 tests before subtracting. Even the tick that reaches zero
     * continues to the submit at 0x80048D98, in every viewport that frame. */
    for (i = 0; i < Q2_FX_TIMED_BEAMS_MAX; i++) {
        w->timed[i].present = w->timed[i].timer > 0;
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
    {
        /* A union rather than a cast: GCC's -Wcast-qual reports any cast to
         * a pointer-to-array as discarding const, however the source is
         * qualified, and nothing here discards anything. Union punning is
         * defined by every compiler this builds under. */
        union { const u8 *bytes; const s16 (*quads)[4]; } view;

        view.bytes = data + Q2_FX_GLINT_INDEX_BYTES;
        out->vert  = view.quads;
    }
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

static u32 glint_build_ot(const q2_fx_glint_mesh *mesh,
                         const s32 origin[3], const s16 spin[3][3], s32 area,
                         s32 owner_batch,
                         const u8 tint_rgb[3], s32 width, s32 phase,
                         const q2_camera *cam, psx_ot *ot, gte_state *gte)
{
    gte_matrix view;
    gte_sxy xy[Q2_FX_GLINT_VERTS];
    u16     z[Q2_FX_GLINT_VERTS];
    bool    ok[Q2_FX_GLINT_VERTS];
    u8      rgb[Q2_FX_GLINT_VERTS][3];
    s16     vz[Q2_FX_GLINT_VERTS];
    u32     count, i, f, emitted = 0;
    u32     area_bucket = 0;
    bool    routed;
    s32     batch = PSX_OT_BATCH_INVALID;

    if (!mesh || !mesh->vert || !mesh->index || !origin || !tint_rgb ||
        !cam || !ot || !gte || width == 0)
        return 0;

    routed = area >= 0 && psx_ot_area_active(ot);
    if (routed && !psx_ot_area_bucket(ot, (u32)area & 0x7Fu, &area_bucket))
        return 0;
    q2_camera_apply_area_projection(cam, ot, area, gte);
    q2_rotation_view_anamorphic(view.m, cam->yaw, cam->pitch, cam->roll);
    gte_set_rotation(gte, &view);
    gte_set_translation(gte, 0, 0, 0);

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

    for (f = 0; f < mesh->face_count && f < Q2_FX_GLINT_MAX_FACES; f++) {
        const u8 *idx = &mesh->index[f * 4u];
        psx_prim *prim;
        u32 depth = 0;
        bool good = true;
        bool lit = false;
        int c;

        for (c = 0; c < 4; c++) {
            if (idx[c] >= count || !ok[idx[c]]) {
                good = false;
                break;
            }
            depth += z[idx[c]];
            lit |= rgb[idx[c]][0] || rgb[idx[c]][1] || rgb[idx[c]][2];
        }
        /* 0x80064A38 skips wholly black faces; 0x80064BC4 counts visited
         * faces, including skipped ones, rather than emitted packets. */
        if (!good || !lit)
            continue;

        if (routed && batch < 0 && owner_batch < 0)
            batch = psx_ot_batch_begin_point(ot, (u32)area & 0x7Fu, false,
                                             (s16)(depth / 4), origin, cam->pos);
        prim = owner_batch >= 0 ? psx_ot_alloc(ot)
             : batch >= 0 ? psx_ot_batch_add(ot, batch)
             : routed ? psx_ot_add_bucket_depth(ot, area_bucket, (u16)(depth / 4),
                                                 depth / 4)
             : psx_ot_add_depth(ot,
                                (u16)bucket_for(ot, depth, 4, cam->sort_range),
                                sort_key_for(depth, 4));
        if (!prim)
            break;

        if (owner_batch >= 0) {
            /* The disc indices are perimeter order; retail swaps 2/3 when
             * writing hardware's Z-order packet. Its NCLIP uses 0,1,3. */
            gte->sxy[0] = xy[idx[0]];
            gte->sxy[1] = xy[idx[1]];
            gte->sxy[2] = xy[idx[3]];
            gte_nclip(gte);
            if (gte->mac0 < 0)
                psx_ot_batch_link_prim(ot, owner_batch, prim);
            else
                psx_ot_batch_append_prim(ot, owner_batch, prim);
        }

        /* An untextured Gouraud packet: ABE is explicit, but ABR comes from
         * the last textured polygon or draw-mode command in the final OT. */
        prim->kind = PSX_PRIM_G4;
        prim->semi_transparent = true;

        for (c = 0; c < 4; c++) {
            u32 v = idx[c];
            /* Field by field: gte_sxy and psx_xy have the same layout but
             * are distinct types, so reading one through the other is
             * undefined. The compiler emits the same load either way. */
            prim->xy[c].x = xy[v].x;
            prim->xy[c].y = xy[v].y;
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
u32 q2_fx_glint_build_ot(const q2_fx_glint_mesh *mesh,
                         const s32 origin[3], s32 yaw,
                         const u8 tint_rgb[3], s32 width, s32 phase,
                         const q2_camera *cam, psx_ot *ot, gte_state *gte)
{
    s16 spin[3][3];
    q2_rotation_yaw_pitch(spin, yaw, 0);
    return glint_build_ot(mesh, origin, (const s16 (*)[3])spin, -1, -1,
                          tint_rgb, width, phase, cam, ot, gte);
}

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

    /* BIGGUN +0x1B54: address of the authored angle triples, followed by
     * halfword indexing and the table-base addition. This works on the raw
     * module too: relocation changes the LUI, not this small data offset. */
    for (i = out->raise_offset; i + 12 <= size; i += 4) {
        u32 p = q2_rd_u32(levelbin + i);
        u32 off, n, k;
        if ((p & 0xFFFF0000u) != 0x24420000u ||
            q2_rd_u32(levelbin + i + 4) != 0x00161840u ||
            q2_rd_u32(levelbin + i + 8) != 0x00628021u)
            continue;
        off = p & 0xFFFFu;
        n = out->band_count;
        if (!n || n > Q2_FX_GLINT_BANDS_MAX || off > size || n * 6 > size - off)
            break;
        for (k = 0; k < n * 3; k++)
            out->angle[k / 3][k % 3] = q2_rd_s16(levelbin + off + k * 2);
        out->bands_found = true;
        break;
    }
    if (out->bands_found) {
        u32 target = 0, uv = 0, k;
        /* +0x1C94: endpoint table, with the AddBeam copy's index in the
         * call's delay slot. +0x438: eight animated UV pairs. */
        for (i = 0; i + 16 <= size; i += 4) {
            u32 p = q2_rd_u32(levelbin + i);
            if ((p & 0xFFFF0000u) != 0x24420000u)
                continue;
            if (q2_rd_u32(levelbin + i + 12) == 0x02228021u &&
                (q2_rd_u32(levelbin + i + 8) >> 26) == 3)
                target = p & 0xFFFFu;
            if (q2_rd_u32(levelbin + i + 4) == 0x0062B021u)
                uv = p & 0xFFFFu;
        }
        if (target && uv && target <= size && uv <= size &&
            out->band_count * 12u <= size - target && 16 <= size - uv) {
            for (k = 0; k < out->band_count * 3u; k++)
                out->target[k / 3][k % 3] = q2_rd_s32(levelbin + target + k * 4);
            memcpy(out->uv, levelbin + uv, sizeof(out->uv));
            out->lightning_found = true;
        }
        for (i = 0; i + 20 <= size; i += 4) {
            u32 p = q2_rd_u32(levelbin + i), off = p & 0xFFFFu;
            if ((p & 0xFFFF0000u) == 0x24840000u &&
                q2_rd_u32(levelbin + i + 12) == 0x02602821u &&
                q2_rd_u32(levelbin + i + 16) == 0x8CE600DCu &&
                off <= size && 12 <= size - off) {
                for (k = 0; k < 3; k++)
                    out->spark_origin[k] = q2_rd_s32(levelbin + off + k * 4);
                break;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
void q2_fx_glint_refresh(q2_fx_glint *g, const s32 angles[3])
{
    u32 i, k;
    if (!g || !angles || !g->scripted)
        return;
    g->phase = Q2_FX_GLINT_PHASE_START;
    for (i = 0; i < g->band_count && i < Q2_FX_GLINT_BANDS_MAX; i++) {
        u32 rgb = 0;
        /* BIGGUN +0x1B78: signed divide by 256, toward zero. */
        for (k = 0; k < 3; k++)
            g->band[i].angle[k] = (s16)(g->base_angle[i][k] - angles[k] +
                (q2_rng_next(&g->script_rng) - 16384) / 256);
        g->band[i].phase = Q2_FX_GLINT_PHASE_START; /* +0x1D50 */
        for (k = 0; k < 3; k++)
            rgb |= (u32)(u8)q2_rng_next(&g->script_rng) << (k * 8);
        g->band[i].colour = rgb;
    }
}

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
     * The caller advances after the rendered frame, only if the owner was
     * drawn. The original decrements inside each viewport draw; this port
     * shares that frame's phase across viewports before consuming it once.
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
    s32 angles[3] = { 0, yaw, 0 };
    return q2_fx_glint_draw_entity(g, origin, angles, -1, cam, ot, gte);
}

u32 q2_fx_glint_draw_entity(const q2_fx_glint *g, const s32 origin[3],
                            const s32 angles[3], s32 area,
                            const q2_camera *cam, psx_ot *ot, gte_state *gte)
{
    s16 rotation[3][3];
    if (!angles)
        return 0;
    q2_rotation_euler(rotation, angles[0], angles[1], angles[2]);
    return q2_fx_glint_draw_attached(g, origin, (const s16 (*)[3])rotation,
                                    area, -1, cam, ot, gte);
}

u32 q2_fx_glint_draw_attached(const q2_fx_glint *g, const s32 origin[3],
                              const s16 rotation[3][3], s32 area, s32 batch,
                              const q2_camera *cam, psx_ot *ot, gte_state *gte)
{
    u32 emitted = 0, i, n;
    s16 entity[3][3];
    int r, c;

    if (!g || !g->ready || !origin || !rotation || !cam || !ot || !gte ||
        (batch >= 0 && (u32)batch >= ot->batch_count))
        return 0;

    /* 0x80064DB4 calls 0x80055AF8 with the three 768s at 0x800AEB30.
     * That helper divides by 256 (toward zero), NOT the rotation matrix's
     * 4096 unit. The glint's object scale is 3, not 3/16. */
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            entity[r][c] = (s16)(((s32)rotation[r][c] * 768) / 256);

    /* 0x80064CE4: a zero count takes the single-band path, which uses the
     * entity's own colour, phase and width. */
    if (g->band_count == 0) {
        return glint_build_ot(&g->mesh, origin, (const s16 (*)[3])entity, area, batch, g->tint,
                                    Q2_FX_GLINT_ONE_WIDTH, (s32)g->phase,
                                    cam, ot, gte);
    }

    n = g->band_count;
    if (n > Q2_FX_GLINT_BANDS_MAX)
        n = Q2_FX_GLINT_BANDS_MAX;

    for (i = 0; i < n; i++) {
        const q2_fx_glint_band *b = &g->band[i];
        u8 tint[3];
        s16 band[3][3], spin[3][3];

        tint[0] = q2_fx_colour_r(b->colour);
        tint[1] = q2_fx_colour_g(b->colour);
        tint[2] = q2_fx_colour_b(b->colour);

        q2_rotation_euler(band, b->angle[0], b->angle[1], b->angle[2]);
        for (r = 0; r < 3; r++) {
            for (c = 0; c < 3; c++) {
                s32 sum = 0;
                int k;
                for (k = 0; k < 3; k++)
                    sum += (s32)entity[r][k] * band[k][c];
                spin[r][c] = (s16)(sum >> 12);
            }
        }
        emitted += glint_build_ot(&g->mesh, origin, (const s16 (*)[3])spin, area, batch,
                                        tint, Q2_FX_GLINT_BAND_WIDTH,
                                        (s32)b->phase, cam, ot, gte);
    }

    return emitted;
}

/* ------------------------------------------------------------------------- */
/* BIGGUN's lightning — LevelBin +0x138 / +0x38C                              */
/* ------------------------------------------------------------------------- */
void q2_fx_glint_lightning_tick(q2_fx_glint *g, q2_rng *rng)
{
    static const s32 bias[8] = {24000, 20000, 8000, 16384,
                                12000, 6000, 24000, 27000};
    u32 i;
    if (!g || !rng || !g->raised || !g->lightning_ready)
        return;
    for (i = 0; i < 8; i++) {
        s32 v;
        /* Unsigned addition reproduces the MIPS wrap without signed C
         * overflow. The following divide is signed and truncates to zero. */
        g->lightning_noise[i] += (u32)(q2_rng_next(rng) - bias[i]) << 15;
        v = (s32)g->lightning_noise[i] / 131072;
        g->lightning_offset[i] = (v < 0 ? -v : v) - 8192;
    }
}

u32 q2_fx_glint_lightning_draw(const q2_fx_glint *g, u16 tpage, u16 clut,
                               const q2_camera *cam, psx_ot *ot, gte_state *gte)
{
    s16 owner[3][3];
    gte_matrix view;
    u32 b, n, made = 0, area_bucket = 0;
    bool routed;
    const u8 *uv;

    if (!g || !g->ready || !g->raised || !g->lightning_ready ||
        !cam || !ot || !gte)
        return 0;
    routed = psx_ot_area_active(ot);
    if (routed && !psx_ot_area_bucket(ot, g->area, &area_bucket))
        return 0;
    q2_camera_apply_area_projection(cam, ot, g->area, gte);
    q2_rotation_view_anamorphic(view.m, cam->yaw, cam->pitch, cam->roll);
    gte_set_rotation(gte, &view);
    gte_set_translation(gte, 0, 0, 0);
    q2_rotation_euler(owner, g->angles[0], g->angles[1], g->angles[2]);
    uv = g->lightning_uv[((g->lightning_frame + (g->lightning_frame >> 31)) & 14u) >> 1];
    n = g->band_count;
    if (n > Q2_FX_GLINT_BANDS_MAX)
        n = Q2_FX_GLINT_BANDS_MAX;

    /* Two 1280-byte pools, 40 bytes per FT4: 32 polygons for ALL six beams,
     * not 32 per beam (+0x32D8). A full pool leaves later beams undrawn. */
    for (b = 0; b < n && made < 32; b++) {
        const q2_fx_glint_band *band = &g->band[b];
        s16 spin[3][3];
        s32 from[3], delta[3], lo[3], hi[3], length, segments, full;
        gte_sxy xy[33];
        u16 z[33];
        psx_xy edge[33][2];
        s32 dx[33], dy[33], step[3];
        s32 batch = PSX_OT_BATCH_INVALID;
        s32 i;
        int r, k;
        s64 distance = 0;

        q2_rotation_euler(spin, band->angle[0], band->angle[1], band->angle[2]);
        for (r = 0; r < 3; r++) {
            s32 sum = 0;
            for (k = 0; k < 3; k++)
                sum += (((s32)owner[r][k] * 768) / 256) * spin[k][2];
            /* Scale/compose the same matrix as the glow, then transform its
             * (0,0,512) tip (+0x1C10..+0x1C88). */
            from[r] = g->origin[r] + (((sum >> 12) * 512) >> 12);
            delta[r] = g->lightning_target[b][r] - from[r];
            distance += (s64)delta[r] * delta[r];
            lo[r] = delta[r] < 0 ? g->lightning_target[b][r] : from[r];
            hi[r] = delta[r] > 0 ? g->lightning_target[b][r] : from[r];
        }
        length = q2_fx_sqrt_nr((u32)distance);
        if (!length)
            continue;
        segments = (length + 255) / 256;
        if (segments > 32)
            segments = 32;
        full = length / 256;
        if (full > segments)
            full = segments;
        for (k = 0; k < 3; k++)
            step[k] = (s32)((s64)delta[k] * (16777216 / length));
        for (i = 0; i <= segments; i++) {
            s32 rel[3];
            for (k = 0; k < 3; k++) {
                s32 point = i == segments ? g->lightning_target[b][k]
                    : from[k] + (s32)((s64)step[k] * i / 65536);
                /* The original projects signed halfword differences. */
                rel[k] = (s16)(point - cam->pos[k]);
            }
            (void)gte_project_point(gte, rel[0], rel[1], rel[2], &xy[i], &z[i]);
        }
        for (i = 0; i < segments; i++) {
            dx[i] = (s32)xy[i].x - xy[i + 1].x;
            dy[i] = (s32)xy[i].y - xy[i + 1].y;
        }
        dx[segments] = dx[segments - 1];
        dy[segments] = dy[segments - 1];
        for (i = 0; i <= segments; i++) {
            s32 sx = 0, sy = 0, jx = 0, jy = 0;
            s32 denom = q2_fx_sqrt_nr((u32)(dx[i] * dx[i] + dy[i] * dy[i]))
                      * (s16)z[i];
            if (denom) {
                sx = dy[i] * 16384 / denom;
                sy = dx[i] * 16384 / denom;
                if (i && i != segments) {
                    jx = (s32)((s64)dy[i] * 2 * g->lightning_offset[i & 7] / denom);
                    jy = (s32)((s64)dx[i] * 2 * g->lightning_offset[(i + 4) & 7] / denom);
                }
            }
            edge[i][0].x = (s16)(xy[i].x - sx + jx);
            edge[i][0].y = (s16)(xy[i].y + sy + jy);
            edge[i][1].x = (s16)(xy[i].x + sx + jx);
            edge[i][1].y = (s16)(xy[i].y - sy + jy);
        }
        for (i = 0; i < segments && made < 32; i++) {
            s32 width = i < full ? 63 : ((length & 255) * 64 / 256);
            psx_prim *p;
            u32 depth;
            if (!width || (s16)z[i] < 192 || (s16)z[i + 1] < 192)
                continue;
            depth = ((u32)z[i] + z[i + 1]) / 2;
            if (routed && batch < 0)
                batch = psx_ot_batch_begin_box(ot, g->area, false, 0,
                                                lo, hi, cam->pos);
            p = batch >= 0 ? psx_ot_batch_add(ot, batch)
                : routed ? psx_ot_add_bucket(ot, area_bucket)
                : psx_ot_add_depth(ot, (u16)bucket_for(ot, depth, 1, cam->sort_range), depth);
            if (!p)
                return made;
            p->kind = PSX_PRIM_FT4;
            p->quad_zorder = true;
            p->semi_transparent = true;
            p->textured_blend = true;
            p->tpage = tpage;
            p->clut = clut;
            p->rgb[0].r = p->rgb[0].g = p->rgb[0].b = 128;
            p->xy[0] = edge[i][0];
            p->xy[1] = edge[i + 1][0];
            p->xy[2] = edge[i][1];
            p->xy[3] = edge[i + 1][1];
            for (k = 0; k < 4; k++) {
                p->uv[k].u = (u8)(uv[0] + ((k & 1) ? width : 0));
                p->uv[k].v = (u8)(uv[1] + ((k & 2) ? 23 : 0));
            }
            made++;
        }
    }
    return made;
}

/* ------------------------------------------------------------------------- */
/* Drawing                                                                    */
/* ------------------------------------------------------------------------- */
/*
 * The camera the effect emitter projects through is the frame's ONE camera, not
 * a basis of its own. 0x8003058C is `SetRotMatrix(view + 160)` — literally the
 * same matrix the world draw loads at 0x800313CC — and view+160 is what
 * 0x80037F38 fills by calling 0x80055DE4 with the basis at view+192 and the
 * viewport's (vw, vh): the anamorphic camera, roll included. So the row-0 scale
 * and the roll belong here as much as they do in world.c and modeldraw.c.
 *
 * Only row 2 of this matrix leaves the GTE — `to_camera` feeds the near test and
 * the quad size divide, and both read cam_space[2] alone. The anamorphic scale
 * touches row 0 only and the roll mixes rows 0 and 1 only (src/common/trig.c),
 * so every near test and every quad size is unchanged by this; what changes is
 * where the quads land.
 */
static void camera_basis(const q2_camera *cam, s16 view[3][3])
{
    q2_rotation_view_anamorphic(view, cam->yaw, cam->pitch, cam->roll);
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
    s32 budget = (s32)w->budget_left;

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

        /*
         * 0x80030614..0x80030628: the HIGH nibble of the flags byte disables a
         * group in one viewport without touching the others —
         *
         *     80030614  lbu  v0, -2(s3)      ; group+0xC4
         *     8003061C  srl  v0, v0, 4
         *     80030620  srav v0, v0, t2      ; t2 = the viewport
         *     80030624  andi v0, v0, 0x1
         *
         * so viewport n tests bit 4 + n. This used to test bit n, the LOW
         * nibble, which nothing writes: the spawners clear the whole byte and
         * the mesh drawers (fx_view_skip) set bits 4..7, so the skip a drawer
         * asked for was never honoured and a player saw their own crackle.
         */
        if (viewport < 4 && ((g->view_mask >> 4) & (1u << viewport)))
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

            /*
             * A PARTICLE THAT FAILS TO PROJECT IS STILL DRAWN, because the
             * console's inner quad loop has no way out of itself. 0x800308F8
             * runs `rtps` per follower and 0x8003093C stores SXY2 straight
             * into the packet; there is no `cfc2`, no flag test and no
             * `continue` anywhere in the 420 instructions of 0x800304A8. The
             * only near-plane decision is the GROUP-level one already made
             * above (0x80030708, effect.c's Q2_FX_NEAR_DEPTH test), and that
             * one drops the whole group or none of it.
             *
             * Dropping the failures one at a time made a six-particle trail
             * emit five, or four, at the angles where the GTE's divide
             * overflows — a streak that thins and thickens with the camera
             * rather than with the bolt. On a failure `xy` holds what the
             * projection left in SXY2, which is the same halfword the console
             * reads on the same instruction.
             */
            (void)gte_project_point(gte, pt[0] - cam->pos[0],
                                    pt[1] - cam->pos[1],
                                    pt[2] - cam->pos[2], &xy, &z);

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
        w->budget_left = (u32)budget;
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

        if (!t->present || !t->tube)
            continue;

        /* 0x80048D50 `lh a3, 64(sp)` — the area 0x8004E920 resolved for the
         * owner, not a zero. A zero has no screen-change record to drain and
         * draw_beams culls it, which is why the trail never appeared. */
        q2_fx_beam_add(w, t->from, t->to, t->radius, (u8)t->area,
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

/*
 * The creature muzzle flashes — 0x80031324..0x800316D8, once per viewport.
 * See q2_fx_flash in effect.h for where each number comes from.
 */
static u32 draw_flashes(q2_fx_world *w, const q2_camera *cam, psx_ot *ot,
                        gte_state *gte, const s16 view[3][3])
{
    u32 emitted = 0, i;

    for (i = 0; i < w->flash_count; i++) {
        const q2_fx_flash *f = i < Q2_FX_FLASH_MAX ? &w->flash[i]
                                    : &w->flash_extra[i - Q2_FX_FLASH_MAX];
        static const u8 uv[4][2] = {
            { Q2_FX_FLASH_U0, Q2_FX_FLASH_V0 }, { Q2_FX_FLASH_U1, Q2_FX_FLASH_V0 },
            { Q2_FX_FLASH_U0, Q2_FX_FLASH_V1 }, { Q2_FX_FLASH_U1, Q2_FX_FLASH_V1 }
        };
        /* psx_prim runs around the perimeter; the hardware's quad is Z
         * ordered, so its corners 2 and 3 trade places (draw_groups). */
        static const u8 order[4] = { 0, 1, 3, 2 };
        s32 cs[3];
        gte_sxy xy[4];
        u16 z[4];
        u32 bucket = 0;
        bool area_routed = psx_ot_area_active(ot);
        psx_prim *prim;
        int k;

        /* 0x80031468..0x80031474: the muzzle's own camera-space depth, and
         * the flash is not drawn this side of 257. */
        to_camera(view, cam, f->centre, cs);
        if (cs[2] < Q2_FX_FLASH_NEAR)
            continue;

        if (area_routed && !psx_ot_area_bucket(ot, f->area & 0x7Fu, &bucket))
            continue;
        q2_camera_apply_area_projection(cam, ot,
                                        area_routed ? (s32)(f->area & 0x7Fu)
                                                    : -1,
                                        gte);

        for (k = 0; k < 4; k++)
            (void)gte_project_point(gte, f->corner[k][0] - cam->pos[0],
                                    f->corner[k][1] - cam->pos[1],
                                    f->corner[k][2] - cam->pos[2],
                                    &xy[k], &z[k]);

        /* Sorted at SZ3 of the `rtpt` — the third corner (0x80031638). */
        prim = area_routed
             ? psx_ot_add_bucket_depth(ot, bucket, z[2], z[2])
             : psx_ot_add_depth(ot,
                                (u16)bucket_for(ot, z[2], 1, cam->sort_range),
                                sort_key_for(z[2], 1));
        if (!prim) {
            w->stats.ot_overflow++;
            break;
        }

        prim->kind = w->untextured ? PSX_PRIM_F4 : PSX_PRIM_FT4;
        for (k = 0; k < 4; k++) {
            prim->xy[k].x  = xy[order[k]].x;
            prim->xy[k].y  = xy[order[k]].y;
            prim->uv[k].u  = uv[order[k]][0];
            prim->uv[k].v  = uv[order[k]][1];
            prim->rgb[k].r = 255;
            prim->rgb[k].g = 255;
            prim->rgb[k].b = 255;
            prim->rgb[k].pad = 0;
        }
        prim->clut             = w->flash_clut;
        prim->tpage            = (u16)(w->tpage_base | Q2_FX_FLASH_ABR);
        prim->semi_transparent = true;
        prim->textured_blend   = true;

        emitted++;
        w->stats.flashes_drawn++;
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
    if (viewport == 0) {
        w->budget_left = w->budget;
        submit_timed(w);
    }

    emitted += draw_groups(w, cam, viewport, ot, gte, view);
    emitted += draw_beams(w, cam, ot, gte);

    /* draw_groups leaves the GTE on its last group's projection. */
    memcpy(gm.m, view, sizeof(gm.m));
    gte_set_rotation(gte, &gm);
    gte_set_translation(gte, 0, 0, 0);
    emitted += draw_flashes(w, cam, ot, gte, view);

    /* The blur trails: their housekeeping once a frame, as the first
     * viewport's pass of ProcessBlurs does it, then the ribbons, on the
     * frame camera draw_flashes left installed. */
    if (viewport == 0)
        blur_expire(w);
    emitted += draw_blurs(w, cam, ot, gte);

    return emitted;
}
