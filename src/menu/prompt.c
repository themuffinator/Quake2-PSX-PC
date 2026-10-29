#include "prompt.h"

#include <string.h>

/* 0x8009B4D8, read out as it stands. */
const q2_prompt_rec q2_prompt_table[Q2_PROMPT_COUNT] = {
    /*  u    v    w   h  centre  target   x    y  */
    {  96, 224, 76, 16,   384,      0,  346, 252 },  /* X SELECT      */
    { 176, 213, 76, 16,   128,      0,   90, 252 },  /* triangle BACK */
    { 176, 235, 76, 16,   256,      0,  218, 252 }   /* square RULES  */
};

void q2_prompt_init(q2_prompt_bar *b)
{
    if (!b)
        return;

    memcpy(b->rec, q2_prompt_table, sizeof(b->rec));

    /*
     * The shipped targets are all ZERO, and left alone they would march every
     * prompt to the top of the screen — straight through whatever the screen is
     * showing. Nothing on the console ever sees that, because opening a page is
     * what starts the machine and `q2_menu_open` parks all three first. Doing
     * the same here means a caller that forgets gets the console's behaviour
     * rather than three captions climbing over its text.
     */
    q2_prompt_hide_all(b);
    q2_prompt_snap(b);
}

void q2_prompt_show(q2_prompt_bar *b, q2_prompt_id id, int y)
{
    if (!b || (int)id < 0 || (int)id >= Q2_PROMPT_COUNT)
        return;
    /* 0x8001F95C: the setter subtracts 8 so callers can pass the y they mean. */
    b->rec[id].y_target = (s16)(y - Q2_PROMPT_Y_BIAS);
}

void q2_prompt_hide_all(q2_prompt_bar *b)
{
    int i;

    if (!b)
        return;
    /* q2_menu_open passes 260 for all three, which lands on 252 — one line
     * below the 248-line screen. Every page open starts them leaving. */
    for (i = 0; i < Q2_PROMPT_COUNT; i++)
        q2_prompt_show(b, (q2_prompt_id)i,
                       Q2_PROMPT_Y_HIDDEN + Q2_PROMPT_Y_BIAS);
}

void q2_prompt_snap(q2_prompt_bar *b)
{
    int i;

    if (!b)
        return;
    for (i = 0; i < Q2_PROMPT_COUNT; i++)
        b->rec[i].y = b->rec[i].y_target;
}

void q2_prompt_step(q2_prompt_bar *b)
{
    int i;

    if (!b)
        return;

    /*
     * 0x8001C3B0. The clamp is asymmetric in the original — at most +3 going
     * down but at most -3 going up, written as two branches with the
     * comparisons `< 4` and `< -3`. Both come out as three pixels; the
     * asymmetry is in how it is spelled, not in what it does.
     */
    for (i = 0; i < Q2_PROMPT_COUNT; i++) {
        q2_prompt_rec *r = &b->rec[i];
        int delta = r->y_target - r->y;

        if (delta == 0)
            continue;
        if (delta > Q2_PROMPT_SPEED)  delta = Q2_PROMPT_SPEED;
        if (delta < -Q2_PROMPT_SPEED) delta = -Q2_PROMPT_SPEED;
        r->y = (s16)(r->y + delta);
    }
}

void q2_prompt_sync_menu(q2_prompt_bar *b, const q2_menu *m, bool front_end)
{
    const q2_menu_item *item = NULL;
    int y;
    bool rules;

    if (!b)
        return;
    if (!m || !m->open || !m->page) {
        q2_prompt_hide_all(b);
        return;
    }

    /* 0x8001A280..0x8001A348. The test is the current object's action
     * pointer, not general navigability: sliders and toggles are adjustable,
     * but deliberately do not advertise CROSS/SELECT. */
    if (m->cursor >= 0 && m->cursor < (int)m->page->count)
        item = &m->page->items[m->cursor];

    y = front_end ? 216 : 220;
    q2_prompt_show(b, Q2_PROMPT_SELECT,
                   item && item->action != Q2_ACT_NONE ? y : 260);
    q2_prompt_show(b, Q2_PROMPT_BACK,
                   m->page->back != Q2_ACT_NONE ? y : 260);

    /* QFRONT module+0x4618..0x4738: RULES belongs to the three game-mode
     * rows, not LOAD SETTINGS or SAVE SETTINGS, and has its own y = 220. The
     * row test is the engine's own, so that the caption and the button that
     * answers it can never disagree. */
    rules = front_end && q2_menu_front_rules_row(m);
    q2_prompt_show(b, Q2_PROMPT_RULES, rules ? 220 : 260);
}

static u32 keycap(psx_ot *ot, u32 bucket, int x, int y, int w, int h,
                   u8 r, u8 g, u8 blue)
{
    psx_prim *p = psx_ot_add_bucket(ot, bucket);
    int k;
    if (!p) return 0;
    p->kind = PSX_PRIM_F4;
    p->xy[0].x = (s16)x;     p->xy[0].y = (s16)y;
    p->xy[1].x = (s16)(x+w); p->xy[1].y = (s16)y;
    p->xy[2].x = (s16)(x+w); p->xy[2].y = (s16)(y+h);
    p->xy[3].x = (s16)x;     p->xy[3].y = (s16)(y+h);
    for (k = 0; k < 4; k++) {
        p->rgb[k].r = r; p->rgb[k].g = g; p->rgb[k].b = blue;
    }
    return 1;
}

u32 q2_prompt_build_labels(const q2_prompt_bar *b, const q2_menu_font *font,
                           psx_ot *ot, u32 bucket, int ox, int oy,
                           const char *const labels[Q2_PROMPT_COUNT])
{
    u32 n = 0;
    int i;
    if (!b || !font || !ot || !labels) return 0;
    for (i = 0; i < Q2_PROMPT_COUNT; i++) {
        const q2_prompt_rec *r = &b->rec[i];
        const char *action;
        char key[16];
        int kw, aw, x, len;
        if (!labels[i] || !(action = strchr(labels[i], ' '))) continue;
        len = (int)(action-labels[i]);
        if (len >= (int)sizeof(key)) len = (int)sizeof(key)-1;
        memcpy(key, labels[i], (size_t)len); key[len] = 0;
        action++;
        kw = q2_menu_font_width(Q2_MENU_FACE_SMALL, key)+8;
        aw = q2_menu_font_width(Q2_MENU_FACE_SMALL, action);
        x = r->centre-(kw+8+aw)/2;
        n += q2_menu_font_print(font, ot, bucket, Q2_MENU_FACE_SMALL,
            x+kw/2, r->y+8, true, ox, oy, key);
        n += q2_menu_font_print(font, ot, bucket, Q2_MENU_FACE_SMALL,
            x+kw+8+aw/2, r->y+8, false, ox, oy, action);
        /* AddPrim prepends: the inset and border land behind the letters. */
        n += keycap(ot, bucket, ox+x+1, oy+r->y+2, kw-2, 12, 29, 36, 40);
        n += keycap(ot, bucket, ox+x, oy+r->y+1, kw, 14, 112, 100, 76);
    }
    return n;
}

u32 q2_prompt_build_ot(const q2_prompt_bar *b, const q2_menu_font *font,
                       psx_ot *ot, u32 bucket)
{
    u32 n = 0;
    int i;

    if (!b || !font || !ot)
        return 0;

    for (i = 0; i < Q2_PROMPT_COUNT; i++) {
        const q2_prompt_rec *r = &b->rec[i];
        psx_prim *p = psx_ot_add_bucket(ot, bucket);
        int k, x1, y1;
        u8  u1, v1;

        if (!p)
            break;

        /*
         * One quad per prompt, glyph and word together, straight out of the
         * record: xy from +14/+16 with +4/+6 for the far corner, uv from +0/+2
         * with the same +4/+6. Code 0x2C — textured and modulated, unlike the
         * panel frame's raw 0x2D.
         *
         * HALF-OPEN corners, as everywhere else in this port (menufont.c,
         * panel.c). These carried the `- 1` that compensated for a rasteriser
         * which filled both edges; raster.c implements the GPU's own fill rule
         * now, so the far corner is the console's and the coverage comes out
         * the same width it samples.
         */
        x1 = r->x + (int)r->w;
        y1 = r->y + (int)r->h;
        u1 = (u8)(r->u + r->w);
        v1 = (u8)(r->v + r->h);

        p->kind  = PSX_PRIM_FT4;
        p->tpage = font->tpage_item;   /* frontend.lbm */
        p->clut  = font->clut_text;
        p->textured_blend   = true;
        p->semi_transparent = false;

        p->xy[0].x = r->x;      p->xy[0].y = r->y;
        p->xy[1].x = (s16)x1;   p->xy[1].y = r->y;
        p->xy[2].x = (s16)x1;   p->xy[2].y = (s16)y1;
        p->xy[3].x = r->x;      p->xy[3].y = (s16)y1;

        p->uv[0].u = (u8)r->u;  p->uv[0].v = (u8)r->v;
        p->uv[1].u = u1;        p->uv[1].v = (u8)r->v;
        p->uv[2].u = u1;        p->uv[2].v = v1;
        p->uv[3].u = (u8)r->u;  p->uv[3].v = v1;

        for (k = 0; k < 4; k++) {
            p->rgb[k].r = 0x80;
            p->rgb[k].g = 0x80;
            p->rgb[k].b = 0x80;
        }
        n++;
    }
    return n;
}
