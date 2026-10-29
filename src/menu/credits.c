#include "credits.h"

#include <math.h>
#include <string.h>

void q2_credits_start(q2_credits *r, const q2_credit_row *rows, u32 count)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
    if (!rows || !count || count > Q2_CREDITS_MAX) return;
    r->rows = rows;
    r->count = count;
    r->next = 1;
    r->slot[0] = rows[0];
    r->active = true;
}

static void credits_tick(q2_credits *r)
{
    int i, live = 0;
    r->frame = (u8)(r->frame + 1);
    if (!(r->frame & 15) && r->next < r->count) {
        q2_credit_row *slot = &r->slot[r->frame >> 4];
        *slot = r->rows[r->next++];
        slot->y = 210;
    }
    for (i = 0; i < Q2_CREDITS_SLOTS; i++) {
        q2_credit_row *slot = &r->slot[i];
        if (!slot->text) continue;
        slot->y--;
        /* The callback counts before clearing. Return one tick after the
         * final nonempty object passes y=44, as +3908..+39AC does. */
        if (slot->text[0]) live++;
        if (slot->y < 45) slot->text = NULL;
    }
    if (!live) r->active = false;
}

bool q2_credits_advance(q2_credits *r, double dt, int tick_rate)
{
    if (!r || !r->active) return false;
    if (!isfinite(dt) || dt <= 0.0 || tick_rate <= 0) return true;
    r->carry += dt * tick_rate;
    while (r->active && r->carry + 1e-9 >= 1.0) {
        r->carry -= 1.0;
        credits_tick(r);
    }
    if (r->carry < 0.0) r->carry = 0.0;
    return r->active;
}

static void credits_clip(psx_ot *ot, u32 bucket, int x, int y,
                           int w, int h, int offset_y)
{
    psx_prim *p = psx_ot_add_bucket(ot, bucket);
    if (!p) return;
    p->kind = PSX_PRIM_DRAW_ENV;
    p->xy[0].x = (s16)x; p->xy[0].y = (s16)y;
    p->xy[1].x = (s16)w; p->xy[1].y = (s16)h;
    p->xy[2].x = (s16)x; p->xy[2].y = (s16)(y + offset_y);
    p->tpage = 0x060A; /* SetDefDrawEnv */
}

void q2_credits_build_ot(const q2_credits *r, psx_ot *ot,
                          const q2_menu_draw_opts *opts, int screen_h)
{
    int i;
    if (!r || !r->active || !ot || !opts || !opts->font) return;
    q2_menu_font_print(opts->font, ot, Q2_MENU_OT_BUCKET,
                       Q2_MENU_FACE_TITLE, 256, 32, true,
                       opts->origin_x, opts->origin_y, "CREDITS");
    /* +3760..+37E8 narrows the draw area without moving its origin. The
     * title and BACK prompt precede it; size-16 objects follow at OT[213]. */
    credits_clip(ot, 212 * PSX_OT_SUBDIV, opts->origin_x,
                   opts->origin_y + 60, 512, 144, -60);
    for (i = 0; i < Q2_CREDITS_SLOTS; i++) {
        const q2_credit_row *row = &r->slot[i];
        if (row->text && row->text[0])
            q2_menu_font_print(opts->font, ot, Q2_MENU_OT_BUCKET_ITEM,
                               Q2_MENU_FACE_ITEM, row->x, row->y, false,
                               0, 0, row->text);
    }
    /* Later host overlays must not inherit the roll's restricted draw area. */
    credits_clip(ot, 214 * PSX_OT_SUBDIV, 0, 0,
                   Q2_MENU_SCREEN_W, screen_h, 0);
}
