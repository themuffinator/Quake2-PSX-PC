#include "loading.h"

#include "menudraw.h"
#include "modeldraw.h"
#include "worldscale.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

q2_result q2_loading_open(q2_loading *l, const disc *d,
                          const q2_hud_tables *tab,
                          q2_menu_settings *settings)
{
    char   path[128];
    q2_buf buf;
    q2_vram_section vs;
    s32    index;

    if (!l || !d)
        return Q2_ERR_INVALID_ARG;

    memset(l, 0, sizeof(*l));

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT",
             Q2_LOADING_MAP);
    if (disc_read_file(d, path, &buf) != Q2_OK)
        return Q2_ERR_NOT_FOUND;

    /* Takes ownership of `buf` on success; on failure the buffer is ours. */
    if (q2_common_open(&l->common, &buf) != Q2_OK) {
        q2_buf_free(&buf);
        return Q2_ERR_BAD_FORMAT;
    }
    if (q2_model_bank_from_common(&l->bank, &l->common) != Q2_OK ||
        l->bank.count == 0) {
        q2_common_close(&l->common);
        return Q2_ERR_NOT_FOUND;
    }

    /* By NAME, not by index. It is model 0 and the only one QDUMMY carries,
     * but which index a name lands on is a property of this build. */
    index = q2_model_bank_find(&l->bank, Q2_LOADING_MODEL);
    if (index < 0 || q2_model_get(&l->bank, (u32)index, &l->logo) != Q2_OK) {
        q2_common_close(&l->common);
        return Q2_ERR_NOT_FOUND;
    }
    l->have_logo = true;

    /*
     * The bank, into an image of this screen's own. Both halves are optional in
     * the sense that either can be missing on a cut-down disc — a screen with
     * no texture page draws an untextured logo and one with no font draws no
     * text — so neither failure stops the rest.
     */
    l->vram = (psx_vram *)calloc(1, sizeof(psx_vram));
    if (!l->vram) {
        q2_common_close(&l->common);
        return Q2_ERR_NO_MEMORY;
    }

    if (q2_vram_load(&vs, d, Q2_LOADING_MAP) == Q2_OK) {
        l->textures      = (q2_vram_upload(&vs, l->vram) == Q2_OK);
        l->clut4_count_a = vs.clut4_count_a;

        /*
         * Single player, one player: QDUMMY carries no icon sheet in any of the
         * three flavours, and this screen draws no status bar to want one. The
         * upload reports Q2_OK on the strength of `FrontEnd.lbm` alone, which
         * is the whole of what the LOADING line needs.
         */
        if (tab)
            l->font_ready = (q2_menu_font_upload(&l->font, tab, &vs, l->vram,
                                                 false, 1) == Q2_OK);
        q2_vram_free(&vs);
    }

    q2_menu_init(&l->menu, settings, Q2_MENU_SCREEN_H);
    l->menu.page   = q2_menu_page_find(Q2_PAGE_LOADING);
    l->menu.cursor = l->menu.page ? (int)l->menu.page->first : 0;
    l->menu.open   = (l->menu.page != NULL);

    l->ready = true;
    return Q2_OK;
}

void q2_loading_close(q2_loading *l)
{
    if (!l)
        return;
    if (l->ready)
        q2_common_close(&l->common);
    free(l->vram);
    memset(l, 0, sizeof(*l));
}

void q2_loading_raise(q2_loading *l)
{
    if (!l || !l->ready)
        return;

    /*
     * The hold RESTARTS rather than accumulating. A transition that loads twice
     * — a level change whose arrival lands in another zone — is one screen to
     * the player, and adding the two would make it linger for a second.
     */
    l->hold = (double)Q2_LOADING_HOLD_UNITS;
    l->open = true;
}

bool q2_loading_step(q2_loading *l, double dt)
{
    if (!l || !l->open)
        return false;

    /*
     * THE HOLD IS A MINIMUM, so the frame that exhausts it is still the
     * screen's and the one after it is the first the world gets back.
     *
     * Testing before the subtract rather than after is the whole of that.
     * After, the last frame's remainder was spent on a frame that then drew the
     * level, which made half a second fourteen frames of screen at the headless
     * 1/30 s step instead of fifteen — 0.467 s for a floor of 0.5. It also
     * keeps `open` in step with what this returns, which matters because the
     * frame loop reads the return and `client_frame` reads the flag.
     */
    if (l->hold <= 0.0) {
        l->hold = 0.0;
        l->open = false;
        return false;
    }

    /* `yaw -= 4 * dt` — module+0x9D24, in the same 1/300 s units the hold is
     * spent in, so the turn and the countdown read one clock. */
    l->yaw = (s32)(s16)(l->yaw - Q2_LB_SCENE_SPIN * (s32)(dt * Q2_DT_HZ));

    l->hold -= dt * (double)Q2_DT_HZ;
    return true;
}

u32 q2_loading_build_ot(const q2_loading *l, psx_ot *ot, gte_state *gte,
                        int width, int height)
{
    q2_model_instance   inst;
    q2_model_draw_stats stats;
    q2_camera           cam;
    q2_menu_draw_opts   mo;
    u32 n = 0;

    if (!l || !l->open || !ot || !gte)
        return 0;

    /*
     * The camera is the front end's: the world origin, no rotation, looking
     * down +z with projection 160 (`engine+0x174(0, 160, 4000)`). Everything
     * this screen shows stands in front of it, so nothing else about a viewport
     * applies and none of it is installed.
     */
    if (l->have_logo) {
        memset(&cam, 0, sizeof(cam));
        cam.projection = Q2_LOADING_PROJ;
        cam.far_z      = 4000;
        cam.sort_range = Q2_CAMERA_SORT_RANGE;

        gte_init(gte);
        gte_set_projection(gte, cam.projection, width / 2, height / 2);
        gte->zsf3 = (s16)(Q2_ONE_12 / 3);
        gte->zsf4 = (s16)(Q2_ONE_12 / 4);

        q2_model_instance_init(&inst);
        inst.model = &l->logo;
        inst.yaw   = l->yaw;
        inst.scale = Q2_LOADING_SCALE;
        inst.clut4_count_a = l->clut4_count_a;
        inst.bucket_override = Q2_LOADING_OT_BUCKET;

        /* The corner, in world units — see Q2_LOADING_WORLD_X for the 3/2 the
         * horizontal carries and why it is not the same divide as the
         * vertical. */
        inst.origin[0] = Q2_LOADING_WORLD_X;
        inst.origin[1] = Q2_LOADING_WORLD_Y;
        inst.origin[2] = Q2_LOADING_DIST;

        /* No light environment, so the vertices take `tint` — the neutral 128
         * a modulated primitive wants (modeldraw.h). The title screen's logo is
         * lit by QFRONT's own lights; this one has no level to be lit by. */
        memset(&stats, 0, sizeof(stats));
        n += q2_model_build_ot(&inst, &cam, ot, gte, &stats);
    }

    /*
     * And the page over it, through the same builder every other page goes
     * through — this one IS a menu page (0x800A3314), so it gets the menu's
     * font, the menu's centring and the menu's bucket.
     */
    if (l->font_ready && l->menu.page) {
        q2_menu_draw_opts_default(&mo, &l->font);
        mo.origin_x = (width  - Q2_MENU_SCREEN_W) / 2;
        mo.origin_y = (height - Q2_MENU_SCREEN_H) / 2;
        mo.view_x   = 0;
        mo.view_w   = width < Q2_MENU_SCREEN_W ? width : Q2_MENU_SCREEN_W;

        n += q2_menu_build_ot(&l->menu, ot, &mo);
    }

    return n;
}
