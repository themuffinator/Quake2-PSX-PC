/* Disc-backed, headless client fixture. Include after client/main.c and CHECK. */
#ifndef Q2_TEST_CLIENT_FIXTURE_H
#define Q2_TEST_CLIENT_FIXTURE_H
static client *open_client(const char *disc)
{
    client *c = calloc(1, sizeof(*c));
    if (!c) exit(2);
    c->headless = true;
    c->use_sort = true;
    c->sim_enabled = true;
    c->trace_cre = -1;
    c->give_powerup = -1;
    c->mission_row = -1;
    c->pad_frame = -2;
    if (disc_open(&c->disc, disc) != Q2_OK ||
        q2_identify(c->disc, &c->build) != Q2_OK ||
        q2_screen_init(&c->screen, c->build.video) != Q2_OK) exit(2);
    c->width = c->screen.disp.width;
    c->height = c->screen.disp.height;
    q2_world_render_init(&c->render);
    psx_ot_init(&c->ot, Q2_SCREEN_OT_ENTRIES, 300000);
    psx_raster_opts_default(&c->opts);
    c->vram = calloc(1, sizeof(*c->vram));
    if (!c->vram) exit(2);
    q2_camera_default(&c->cam, c->width, c->height);
    q2_screen_set_layout(&c->screen, Q2_SCREEN_LAYOUT_ONE, 1);
    q2_menu_settings_defaults(&c->settings);
    q2_menu_reset_video_for(&c->settings, c->height);
    q2_menu_init(&c->menu, &c->settings, Q2_MENU_SCREEN_H);
    q2_menu_set_fb_height(&c->menu, c->height);
    q2_player_death_init(&c->death[0]);
    q2_mission_init(&c->mission);
    q2_briefing_init(&c->briefing);
    q2_briefing_popup_init(&c->popup);
    q2_prompt_init(&c->prompts);
    c->item_table_ready = q2_item_table_load(&c->item_table, c->disc,
                                             &c->build) == Q2_OK;
    c->vm_ready = q2_vm_tables_load(&c->vm_tables, c->disc, &c->build) == Q2_OK;
    CHECK(c->vm_ready);
    c->hud_tables_ready = q2_hud_tables_load(&c->hud_tables, c->disc, &c->build) == Q2_OK;
    c->fx_tables_ready = q2_fx_tables_load_disc(&c->fx_tables, c->disc, &c->build) == Q2_OK;
    CHECK(c->hud_tables_ready && c->fx_tables_ready);
    q2_hud_init(&c->hud[0], &c->hud_tables, 1);
    c->hud_ready = true;
    return c;
}

static void close_client(client *c)
{
    client_free_creatures(c);
    client_voices_stop(c);
    client_film_stop(c);
    if (c->sfx_ready) q2_sound_bank_free(&c->sfx);
    if (c->level_table_ready) q2_level_table_free(&c->level_table);
    q2_save_free(&c->snapshot);
    q2_event_store_free(&c->event_store);
    q2_zone_history_free(&c->zone_history);
    q2_gib_attach(NULL);
    q2_sim_free(&c->sim[0]);
    q2_common_close(&c->common);
    q2_world_free_zone(&c->zone);
    q2_movers_free(&c->movers);
    q2_rotators_free(&c->rotators);
    q2_explosives_free(&c->explosives);
    free(c->node_hidden);
    free(c->vram);
    q2_screen_free(&c->screen);
    q2_vm_tables_free(&c->vm_tables);
    psx_ot_free(&c->ot);
    disc_close(c->disc);
    free(c);
}

#endif
