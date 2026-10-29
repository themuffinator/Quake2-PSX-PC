/* Exercise the actual client glue without a disc, window or physical input.
 * Keeping main in this translation unit lets regressions in its pad latches,
 * respawn callers and owner selection be caught by ordinary CTest runs. */
int q2_test_client_main(int argc, char **argv);
#define main q2_test_client_main
#include "../src/client/main.c"
#undef main

static int failures;
static int checks;
#define CHECK(condition) do { checks++; if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    failures++; } } while (0)

#include "client_fixture.h"
#include "fixtures/player_animation_reference.h"

static client *fixture(int players)
{
    client *c = calloc(1, sizeof(*c));
    q2_inventory inv;
    int pi;
    if (!c) exit(2);
    c->headless = true;
    c->frame_index = 1;
    c->pad_frame = 0;
    c->mp_enabled = true;
    c->sim_enabled = true;
    q2_sim_init(&c->sim[0], NULL, 50);
    c->sim[0].multiplayer = true;
    c->sim[0].fire_from_input = false;
    q2_inventory_init(&inv);
    q2_mp_session_init(&c->mp, Q2_MP_DEATHMATCH, players);
    for (pi = 0; pi < players; pi++) {
        s32 feet[3] = {pi * 2000, 0, 0};
        q2_sim_select_player(&c->sim[0], pi);
        q2_sim_spawn(&c->sim[0], feet, 0);
        q2_sim_player_loadout(&c->sim[0], pi, &inv, 1);
        q2_player_death_init(&c->death[pi]);
        c->sim[0].player[pi].look_scheme = Q2_PAD_STYLE_RIGHT_STICK;
        c->sim_ready[pi] = true;
    }
    q2_sim_select_player(&c->sim[0], 0);
    c->sim_ready[0] = false; /* The client uses this array for EXTRA slots. */
    c->mp_start_valid = true;
    c->mp_start_inv = inv;
    c->mp_start_weapon = 1;
    c->mp_spawn_count = 1;
    c->mp_spawns[0].present = true;
    c->mp_spawns[0].pos[0] = 9000;
    return c;
}

static void release_fixture(client *c)
{
    q2_sim_free(&c->sim[0]);
    free(c);
}

static void buttons(client *c, int pi, SDL_GamepadButton button, bool down)
{
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    event.gbutton.which = c->gamepads.id[pi];
    event.gbutton.button = (Uint8)button;
    event.gbutton.down = down;
    CHECK(q2_gamepads_event(&c->gamepads, &event) == pi);
}

static void test_inputs(void)
{
    client *c = fixture(4);
    q2_input in;
    int pi;
    c->shoot = true;
    client_input_simulated(c, 1.0f / 30.0f);
    CHECK(c->mp_input[0].attack);
    for (pi = 1; pi < 4; pi++) CHECK(!c->mp_input[pi].attack);
    for (pi = 1; pi < 4; pi++) {
        SDL_Event ev;
        c->gamepads.id[pi] = (SDL_JoystickID)(100 + pi);
        memset(&c->mp_pad[pi], 0, sizeof(c->mp_pad[pi]));
        /* A shoulder tap starts and ends between ticks. It must survive,
         * then stop firing on the next tick without affecting another pad. */
        buttons(c, pi, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
        buttons(c, pi, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
        client_extra_input(c, pi, 0, false, &in);
        client_extra_input(c, pi, 12, false, &in);
        CHECK(in.attack && (in.buttons & Q2_BTN_ATTACK_PRESS));
        client_extra_input(c, pi, 12, false, &in);
        CHECK(!in.attack);
        /* Resume remains pending across a non-ticking frame. */
        buttons(c, pi, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
        client_extra_input(c, pi, 0, true, &in);
        client_extra_input(c, pi, 12, false, &in);
        CHECK(in.attack && !(in.buttons & Q2_BTN_ATTACK_PRESS));
        /* Disconnect releases both held input and accumulated taps. */
        memset(&ev, 0, sizeof(ev));
        ev.type = SDL_EVENT_GAMEPAD_REMOVED;
        ev.gdevice.which = c->gamepads.id[pi];
        CHECK(q2_gamepads_event(&c->gamepads, &ev) == pi);
        client_extra_input(c, pi, 12, false, &in);
        CHECK(!in.attack && !in.forward && !in.yaw);
    }
    /* Weapon-cycle edges reach each owner's inventory and carousel. */
    c->sim[0].pcombat[2].inv.weapons |= (u16)q2_weapon_tables_builtin()->owned_bit[Q2_WID_SHOTGUN];
    c->sim[0].pcombat[2].inv.ammo[Q2_AMMO_SHELLS] = 20;
    memset(&in, 0, sizeof(in)); in.buttons = Q2_BTN_WEAP_NEXT;
    client_cycle_input(c, 2, &in);
    CHECK(c->sim[0].pcombat[2].weapon_id == 2);
    CHECK(c->sim[0].combat.weapon_id == 1);
    CHECK(c->sim[0].cur_player == 0);
    release_fixture(c);
}

static void test_single_player_controller(void)
{
    client *c = fixture(1);
    SDL_Event e = {0};
    c->mp_enabled = false; c->sim[0].multiplayer = false;
    c->gamepads.first_slot = 1;
    c->gamepads.id[1] = 101;
    c->gamepads.axis[1][SDL_GAMEPAD_AXIS_LEFTY] = -26000;
    c->gamepads.axis[1][SDL_GAMEPAD_AXIS_RIGHTX] = 22000;
    CHECK(q2_gamepads_primary(&c->gamepads, false) == 1);
    CHECK(q2_gamepads_primary(&c->gamepads, true) == 0);
    client_apply_input(c);
    CHECK(c->settings.v[Q2_SET_PAD_CLASS] == 1);
    buttons(c, 1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
    client_input_simulated(c, 1.0f/30.0f);
    CHECK(c->pad_pend & Q2_PAD_L1);
    c->frame_index++;
    client_input_simulated(c, 1.0f/20.0f);
    CHECK(c->pad.buttons & Q2_PAD_L1);
    CHECK(c->pad.ly < 0 && c->pad.rx > 0);
    e.type = SDL_EVENT_GAMEPAD_REMOVED; e.gdevice.which = 101;
    CHECK(q2_gamepads_event(&c->gamepads, &e) == 1);
    c->frame_index++;
    client_input_simulated(c, 1.0f/20.0f);
    CHECK(!(c->pad.buttons & Q2_PAD_L1) && !c->pad.ly && !c->pad.rx);
    CHECK(c->primary_pad_id == 0);
    release_fixture(c);
}

static void test_controller_profiles(void)
{
    client *c = fixture(4);
    q2_settings_blob saved, legacy;
    q2_input left, right;
    s16 before[Q2_SET_COUNT];
    int pi;
    q2_menu_settings_defaults(&c->settings);
    q2_menu_init(&c->menu, &c->settings, 248);
    q2_menu_set_multiplayer(&c->menu, true);
    c->settings.v[Q2_SET_USE_MOUSE] = 1;
    for (pi = 1; pi < 4; pi++) {
        c->gamepads.id[pi] = (SDL_JoystickID)(100 + pi);
        c->settings.v[q2_menu_player_setting(Q2_SET_PAD_STYLE, pi)] =
            (s16)(Q2_PAD_STYLE_RIGHT_STICK + (pi - 1) % 3);
        c->settings.v[q2_menu_player_setting(Q2_SET_MOUSE_SPEED, pi)] = (s16)(30 + pi);
    }
    client_apply_input(c);
    CHECK(c->sim[0].player[0].look_scheme == Q2_PAD_STYLE_RIGHT_MOUSE);
    for (pi = 1; pi < 4; pi++)
        CHECK(c->sim[0].player[pi].look_scheme == Q2_PAD_STYLE_RIGHT_STICK + (pi - 1) % 3);

    /* The pauser's real menu edits only that player's config. */
    CHECK(client_pause_open(c, 2));
    q2_menu_advance(&c->menu, 0);
    q2_menu_goto(&c->menu, Q2_PAGE_CONTROLLER);
    c->menu.cursor = 0;
    q2_menu_advance(&c->menu, Q2_PAD_RIGHT);
    q2_menu_advance(&c->menu, 0);
    client_apply_input(c);
    CHECK(c->sim[0].player[2].look_scheme == Q2_PAD_STYLE_BOTH_STICKS);
    CHECK(c->sim[0].player[1].look_scheme == Q2_PAD_STYLE_RIGHT_STICK);
    CHECK(c->sim[0].player[0].look_scheme == Q2_PAD_STYLE_RIGHT_MOUSE);
    client_menu_close(c);

    /* Inversion reaches the owning pad's actual input, independently. */
    c->settings.v[q2_menu_player_setting(Q2_SET_PAD_STYLE, 2)] = Q2_PAD_STYLE_RIGHT_STICK;
    c->settings.v[q2_menu_player_setting(Q2_SET_SWAP_Y, 2)] = 1;
    client_apply_input(c);
    c->gamepads.axis[1][SDL_GAMEPAD_AXIS_RIGHTY] = 24000;
    c->gamepads.axis[2][SDL_GAMEPAD_AXIS_RIGHTY] = 24000;
    client_extra_input(c, 1, 12, false, &left);
    client_extra_input(c, 2, 12, false, &right);
    CHECK(left.pitch != 0 && left.pitch == -right.pitch);

    /* Player one's mouse disables only their recenter timer. */
    for (pi = 0; pi < 4; pi++) c->sim[0].player[pi].recentring = true;
    client_apply_input(c);
    CHECK(!c->sim[0].player[0].recentring);
    for (pi = 1; pi < 4; pi++) CHECK(c->sim[0].player[pi].recentring);

    /* Only player two enabled vibration. Headless slots have no SDL device;
     * inspect the generated motor values without driving any hardware. */
    c->frame_index = 1;
    c->settings.v[q2_menu_player_setting(Q2_SET_VIBRATION, 1)] = 1;
    for (pi = 0; pi < 4; pi++)
        CHECK(q2_shock_set(&c->shock[pi], Q2_SHOCK_MACHINEGUN,
                          Q2_SHOCK_FULL, NULL, NULL, 0, 0));
    client_shock_tick(c, 6);
    CHECK(c->shock_out[1][1] == 95 && c->shock_out[1][0] == 1);
    CHECK(!c->shock_out[0][1] && !c->shock_out[2][1] && !c->shock_out[3][1]);

    memcpy(before, c->settings.v, sizeof(before));
    memcpy(c->menu.mp_name[1], "SARGE", 6);
    c->menu.mp_team[1] = 0;
    c->menu.mp_colour[1] = 3;
    client_settings_pack(c, &saved);
    q2_menu_reset_identities(&c->menu);
    q2_menu_settings_defaults(&c->settings);
    CHECK(client_settings_unpack(c, &saved));
    CHECK(!memcmp(before, c->settings.v, sizeof(before)));
    CHECK(!strcmp(c->menu.mp_name[1], "SARGE") &&
          c->menu.mp_team[1] == 0 && c->menu.mp_colour[1] == 3);
    {
        q2_settings_blob malformed = saved;
        malformed.count--;
        CHECK(!client_settings_unpack(c, &malformed));
        CHECK(!strcmp(c->menu.mp_name[1], "SARGE"));
        malformed = saved;
        malformed.value[2 + Q2_SET_COUNT + Q2_MP_SETTINGS_SETUP_WORDS] = 7;
        CHECK(!client_settings_unpack(c, &malformed));
        CHECK(!memcmp(before, c->settings.v, sizeof(before)));
    }
    /* Old schema-1 cards have the same setup, immediately after their shorter
     * value array. Missing profiles must reset, not retain another session. */
    legacy = saved;
    legacy.value[0] = 1;
    legacy.value[1] = Q2_SET_EXTRA_PAD_FIRST;
    memcpy(&legacy.value[2 + Q2_SET_EXTRA_PAD_FIRST], &saved.value[2 + Q2_SET_COUNT],
           Q2_MP_SETTINGS_SETUP_WORDS * sizeof(s16));
    legacy.count = 2 + Q2_SET_EXTRA_PAD_FIRST + Q2_MP_SETTINGS_SETUP_WORDS;
    CHECK(client_settings_unpack(c, &legacy));
    CHECK(!strcmp(c->menu.mp_name[1], "PLAYER 2") &&
          c->menu.mp_team[1] == 1 && c->menu.mp_colour[1] == 1);
    for (pi = 1; pi < 4; pi++) {
        CHECK(c->settings.v[q2_menu_player_setting(Q2_SET_MOUSE_SPEED, pi)] == 64);
        CHECK(c->settings.v[q2_menu_player_setting(Q2_SET_SWAP_Y, pi)] == 0);
    }
    legacy.count--;
    memcpy(before, c->settings.v, sizeof(before));
    CHECK(!client_settings_unpack(c, &legacy));
    CHECK(!memcmp(before, c->settings.v, sizeof(before)));
    release_fixture(c);
}

static void test_pickups_and_respawn(void)
{
    client *c = fixture(4);
    int pi, live;
    /* A real shared item sweep must let each extra player collect a nearby
     * shell box, without granting it to the current combat owner. */
    for (pi = 1; pi < 4; pi++) {
        q2_pop_place place;
        q2_entity *item;
        memset(&place, 0, sizeof(place));
        place.id = 27; /* Shells P, touch effect 18. */
        place.x = c->sim[0].player[pi].pos[0];
        item = q2_item_spawn(&c->sim[0].entities, &place, NULL, 0, NULL);
        CHECK(item != NULL);
    }
    c->sim[0].ent_world.dt = 12;
    c->sim[0].ent_world.deathmatch = true;
    q2_entity_run(&c->sim[0].entities, &c->sim[0].ent_world);
    CHECK(c->sim[0].combat.inv.ammo[Q2_AMMO_SHELLS] == 0);
    for (pi = 1; pi < 4; pi++)
        CHECK(c->sim[0].pcombat[pi].inv.ammo[Q2_AMMO_SHELLS] > 0);
    for (live = 0; live < 4; live++) {
        q2_sim_select_player(&c->sim[0], live);
        for (pi = 0; pi < 4; pi++) {
            q2_inventory *inv = pi == live ? &c->sim[0].combat.inv
                                           : &c->sim[0].pcombat[pi].inv;
            CHECK(c->sim[0].ent_world.player[pi].present);
            CHECK(c->sim[0].ent_world.player[pi].inv == inv);
            /* These are the actual pointers used by the shared item sweep. */
            c->sim[0].ent_world.player[pi].inv->health = (s16)(60 + pi);
        }
    }
    q2_sim_select_player(&c->sim[0], 0);
    c->sim[0].combat.inv.health = 3;
    c->sim[0].combat.weapon_id = 11;
    c->sim[0].pcombat[2].self.last_attacker = 1;
    c->sim[0].pcombat[2].self.effect[2] = 30;
    c->sim[0].pcombat[2].shot_serial = 123;
    c->sim[0].pcombat[2].kick[0] = 55;
    CHECK(client_mp_respawn(c, 2));
    CHECK(c->sim[0].pcombat[2].inv.health == 100);
    CHECK(c->sim[0].pcombat[2].weapon_id == 1);
    CHECK(c->sim[0].pcombat[2].shot_serial == 0);
    CHECK(c->sim[0].pcombat[2].kick[0] == 0);
    CHECK(c->sim[0].pcombat[2].self.last_attacker == Q2_MP_NOT_A_PLAYER);
    CHECK(c->sim[0].pcombat[2].self.effect[2] == 0);
    CHECK(c->sim[0].player[2].prev_health == 100);
    CHECK(c->sim[0].combat.inv.health == 3 && c->sim[0].combat.weapon_id == 11);
    CHECK(c->sim[0].ent_world.player[2].pos[0] == 9000);
    CHECK(c->sim[0].ent_world.player[2].inv == &c->sim[0].pcombat[2].inv);
    release_fixture(c);
}

static void test_fast_frames(void)
{
    const int rates[] = {60, 144, 240};
    int rate;
    for (rate = 0; rate < 3; rate++) {
        client *c = fixture(4);
        q2_input neutral = {0};
        int frame, pi, jumps[4] = {0};
        for (pi = 1; pi < 4; pi++) c->gamepads.id[pi] = (SDL_JoystickID)pi;
        for (frame = 0; frame < 980; frame++) {
            double dt = 1.0 / rates[rate];
            s32 step = q2_sim_next_dt(&c->sim[0], dt);
            for (pi = 1; pi < 4; pi++) {
                q2_input in;
                c->gamepads.held[pi] = frame < 960 && (frame + pi * 7) % 48 == 0
                                          ? Q2_PAD_L2 : 0;
                client_extra_input(c, pi, step, false, &in);
                if (step && (in.buttons & Q2_BTN_JUMP)) jumps[pi]++;
            }
            q2_sim_advance(&c->sim[0], &neutral, dt);
        }
        for (pi = 1; pi < 4; pi++) CHECK(jumps[pi] == 20);
        release_fixture(c);
    }
}

static void test_owned_respawn_input(void)
{
    client *c = fixture(4);
    int frame;
    q2_player_die(&c->death[2], Q2_MP_NOT_A_PLAYER, 0, 2, true, false, NULL);
    q2_player_death_anim_ended(&c->death[2]);
    q2_player_death_tick(&c->death[2], -5, 12, true, 0);
    CHECK(c->death[2].stage == Q2_PDEATH_DOWN);
    c->sim[0].pcombat[2].inv.health = -5;
    c->sim[0].player[2].ent2_flags |= Q2_ENT2_DEAD;
    c->shoot = true;
    for (frame = 0; frame < 3; frame++) {
        client_input_simulated(c, 0.05f);
        c->frame_index++;
    }
    CHECK(c->death[2].stage == Q2_PDEATH_DOWN);
    CHECK(c->sim[0].pcombat[2].inv.health <= 0);
    c->shoot = false;
    c->gamepads.id[2] = 102;
    buttons(c, 2, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
    client_input_simulated(c, 0.05f);
    CHECK(c->death[2].stage == Q2_PDEATH_ALIVE);
    CHECK(c->sim[0].pcombat[2].inv.health == 100);
    CHECK(c->sim[0].pcombat[2].weapon_id == 1);
    CHECK(c->sim[0].player[2].pos[0] == 9000);
    CHECK(c->death[1].stage == Q2_PDEATH_ALIVE);
    release_fixture(c);
}

static void test_virtual_controllers(void)
{
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID ids[4];
    SDL_Joystick *joystick[4];
    q2_gamepads pads;
    SDL_Event event;
    int i;
    memset(&pads, 0, sizeof(pads));
    pads.first_slot = 1;
    CHECK(SDL_Init(SDL_INIT_GAMEPAD));
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    desc.name = "Q2 split regression virtual controller";
    for (i = 0; i < 4; i++) {
        ids[i] = SDL_AttachVirtualJoystick(&desc);
        CHECK(ids[i] != 0);
        joystick[i] = SDL_OpenJoystick(ids[i]);
        CHECK(joystick[i] != NULL);
        memset(&event, 0, sizeof(event));
        event.type = SDL_EVENT_GAMEPAD_ADDED;
        event.gdevice.which = ids[i];
        CHECK(q2_gamepads_event(&pads, &event) == (i < 3 ? i + 1 : -1));
    }
    CHECK(pads.id[0] == 0 && pads.id[1] == ids[0] && pads.id[3] == ids[2]);
    /* SDL generates real mapped controller events from these virtual devices.
     * Only gamepad events naming our virtual IDs are read; there is no window,
     * keyboard/mouse polling, or physical input injection in this test. */
    CHECK(SDL_SetJoystickVirtualAxis(joystick[1], SDL_GAMEPAD_AXIS_RIGHTX, 32767));
    CHECK(SDL_SetJoystickVirtualAxis(joystick[1], SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767));
    CHECK(SDL_SetJoystickVirtualButton(joystick[1], SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true));
    SDL_UpdateJoysticks();
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT,
                          SDL_EVENT_GAMEPAD_AXIS_MOTION,
                          SDL_EVENT_GAMEPAD_UPDATE_COMPLETE) > 0) {
        SDL_JoystickID id = event.gdevice.which;
        for (i = 0; i < 4; i++) if (id == ids[i]) break;
        if (i < 4) (void)q2_gamepads_event(&pads, &event);
    }
    {
        q2_pad_state pad = {0};
        u32 raw = q2_gamepads_read(&pads, 2, Q2_PAD_STYLE_RIGHT_STICK, &pad);
        CHECK((raw & (Q2_PAD_L1 | Q2_PAD_L2)) == (Q2_PAD_L1 | Q2_PAD_L2));
        CHECK(pad.rx == 127);
        CHECK(pads.held[1] == 0 && pads.held[3] == 0);
    }
    memset(&event, 0, sizeof(event));
    event.type = SDL_EVENT_GAMEPAD_REMOVED; event.gdevice.which = ids[1];
    CHECK(q2_gamepads_event(&pads, &event) == 2);
    CHECK(pads.id[3] == ids[2] && !pads.id[2]);
    event.type = SDL_EVENT_GAMEPAD_ADDED; event.gdevice.which = ids[3];
    CHECK(q2_gamepads_event(&pads, &event) == 2);
    q2_gamepads_close(&pads);
    pads.first_slot = 0;
    for (i = 0; i < 4; i++) {
        event.type = SDL_EVENT_GAMEPAD_ADDED; event.gdevice.which = ids[i];
        CHECK(q2_gamepads_event(&pads, &event) == i);
    }
    q2_gamepads_close(&pads);
    for (i = 0; i < 4; i++) {
        SDL_CloseJoystick(joystick[i]);
        CHECK(SDL_DetachVirtualJoystick(ids[i]));
    }
    SDL_Quit();
}

static void test_damage(void)
{
    client *c = fixture(2);
    q2_sim *sim = &c->sim[0];
    q2_input neutral = {0};
    int pi, step;
    /* Each player is the victim once, including player 0 while parked and
     * while live during the shared projectile sweep. */
    for (pi = 0; pi < 2; pi++) {
        q2_fire_result_v2 shot;
        q2_inventory inv;
        int victim = 1 - pi;
        q2_inventory_init(&inv);
        inv.health = 100; inv.armour = 100; inv.armour_class = 2;
        inv.flags |= Q2_INV_POWER_SHIELD;
        inv.ammo[Q2_AMMO_CELLS] = 100;
        q2_sim_player_loadout(sim, victim, &inv, 1);
        sim->player[pi].pos[0] = 0;
        sim->player[pi].pos[2] = 0;
        sim->player[victim].pos[0] = 0;
        sim->player[victim].pos[2] = 1300;
        q2_sim_select_player(sim, pi);
        sim->combat.inv.ammo[Q2_AMMO_SLUGS] = 10;
        sim->combat.weapon_id = Q2_WID_RAILGUN;
        sim->combat.inv.weapons |= (u16)q2_weapon_tables_builtin()->owned_bit[Q2_WID_RAILGUN];
        client_targets_for(c, pi);
        shot = q2_sim_fire(sim);
        CHECK(shot.fired);
        CHECK(sim->pcombat[victim].inv.health < 100);
        CHECK(sim->pcombat[victim].inv.armour < 100);
        CHECK(sim->pcombat[victim].inv.ammo[Q2_AMMO_CELLS] < 100);
        CHECK(sim->player[victim].impulse_armed);
        CHECK(sim->player[victim].impulse[2] > 0);
        CHECK(!sim->pcombat[victim].self.knocked);
        inv = sim->pcombat[victim].inv;
        q2_sim_select_player(sim, victim);
        CHECK(sim->combat.inv.armour == inv.armour);
        CHECK(sim->combat.inv.ammo[Q2_AMMO_CELLS] == inv.ammo[Q2_AMMO_CELLS]);
    }
    q2_sim_select_player(sim, 1);
    sim->combat.weapon_id = 1;
    sim->combat.next_fire = 0;
    sim->pcombat[0].inv.health = 100;
    sim->pcombat[0].inv.armour = 0;
    sim->pcombat[0].inv.flags = 0;
    client_targets_for(c, 1);
    CHECK(q2_sim_fire(sim).fired);
    q2_sim_select_player(sim, 0);
    client_targets_for(c, 0);
    for (step = 0; step < 20; step++) q2_sim_tick(sim, &neutral, 12);
    CHECK(sim->combat.inv.health < 100);
    CHECK(sim->combat.inv.health == sim->combat.self.health);
    /* A shield pickup gained between shots is respected by the next trace;
     * firing an energy weapon must not restore the cells it just consumed. */
    sim->combat.weapon_id = Q2_WID_HYPERBLASTER;
    sim->combat.inv.weapons |= (u16)q2_weapon_tables_builtin()->owned_bit[Q2_WID_HYPERBLASTER];
    sim->combat.inv.ammo[Q2_AMMO_CELLS] = 10;
    sim->combat.next_fire = 0;
    CHECK(q2_sim_fire(sim).fired);
    CHECK(sim->combat.inv.ammo[Q2_AMMO_CELLS] == 9);
    release_fixture(c);
}

/*
 * THE PLAYER FLINCHES. 0x8003ADF8..0x8003AE08 makes PAIN the live player's
 * animation request whenever the damage byte at entity+0xDF is set, and
 * 0x8003AFC8 hands it to the chooser; the port reconstructed the chooser's PAIN
 * arm (0x8003CF74) and its hold rule (0x8003D188) and then asked it for nothing
 * but DEATH, so Male2's Pain 1/2/3 were unreachable.
 */
static void animation_ranges(client *c)
{
    int i;
    for (i = 0; i < Q2_PMOVE_COUNT; i++) {
        c->player_moves[i].start = (u16)(100 * i);
        c->player_moves[i].end = (u16)(100 * i + 80);
        c->player_moves[i].rest = i <= Q2_PMOVE_RUN ? c->player_moves[i].start : c->player_moves[i].end;
    }
    for (i = 0; i < Q2_MP_MAX_PLAYERS; i++) client_player_anim_reset(c, i);
    c->player_anim_base_ok = true;
}

static void test_pain_animation(void)
{
    client *c = fixture(2);
    q2_sim *sim = &c->sim[0];
    q2_input neutral = {0};
    q2_player_move hurt;
    q2_inventory inv;
    u32 random;
    q2_inventory_init(&inv);
    inv.health = 100; inv.armour = 100; inv.armour_class = 2;
    q2_sim_player_loadout(sim, 0, &inv, 1);
    animation_ranges(c);
    sim->player[0].pos[0] = 0; sim->player[0].pos[2] = 1300;
    sim->player[1].pos[0] = sim->player[1].pos[2] = 0;
    q2_sim_select_player(sim, 1);
    sim->combat.inv.ammo[Q2_AMMO_SLUGS] = 10;
    sim->combat.weapon_id = Q2_WID_RAILGUN;
    sim->combat.inv.weapons |= (u16)q2_weapon_tables_builtin()->owned_bit[Q2_WID_RAILGUN];
    sim->combat.next_fire = 0;
    client_targets_for(c, 1);
    CHECK(q2_sim_fire(sim).fired);
    q2_sim_select_player(sim, 0);
    client_targets_for(c, 0);
    q2_sim_tick(sim, &neutral, 12);
    CHECK(sim->combat.inv.health < 100 && sim->combat.self.damage_serial != 0);
    q2_random_seed(1);
    client_player_anim_tick(c, 0, 12, false);
    hurt = client_player_visual_move(c, 0);
    CHECK(hurt == Q2_PMOVE_PAIN3); /* BIOS seed 1: 16838 % 3 == 2. */
    random = q2_random_state();
    client_player_anim_tick(c, 0, 12, false);
    CHECK(client_player_visual_move(c, 0) == hurt && q2_random_state() == random);
    c->player_anim[0].state.position = (s16)(c->player_moves[hurt].end - 12);
    client_player_anim_tick(c, 0, 12, false);
    CHECK(client_player_visual_move(c, 0) == Q2_PMOVE_STAND);
    /* The serial is consumed even when the byte is zero; that does not
     * request PAIN or draw random numbers on the console. */
    sim->combat.self.damage_serial++;
    sim->combat.self.last_mod = 0;
    client_player_anim_tick(c, 0, 12, false);
    CHECK(client_player_visual_move(c, 0) == Q2_PMOVE_STAND && q2_random_state() == random);
    release_fixture(c);
}

static void test_player_animation_clock(void)
{
    client *c = fixture(4);
    q2_sim *sim = &c->sim[0];
    int i, ticks;
    animation_ranges(c);
    /* All owners select their own combat block, including an active extra
     * player and a parked player zero. Running uses wish, not drift speed. */
    for (i = 0; i < 4; i++) {
        q2_sim_select_player(sim, i);
        c->vw[i].result = 1;              /* a shot, in the console's word */
        sim->player[i].wish[0] = 1;
        sim->player[i].vel[0] = 0;
        client_player_anim_tick(c, i, 12, false);
        CHECK(client_player_visual_move(c, i) == Q2_PMOVE_RUN);
        client_player_anim_reset(c, i);
        c->vw[i].result = 0;
        sim->player[i].wish[0] = 0;
        sim->player[i].vel[0] = 1000;
        client_player_anim_tick(c, i, 12, false);
        CHECK(client_player_visual_move(c, i) == Q2_PMOVE_STAND);
        client_player_anim_reset(c, i);
        sim->player[i].vel[0] = 0;
    }
    q2_sim_select_player(sim, 0);
    /* No draw or viewport is needed. Non-ticking input frames leave the
     * bodies still; a real game tick advances all four by the same amount. */
    c->frame_index++;
    client_input_simulated(c, 0.005f);
    for (i = 0; i < 4; i++) CHECK(c->player_anim[i].state.position == 0);
    c->frame_index++;
    client_input_simulated(c, 0.05f);
    for (i = 0; i < 4; i++) CHECK(c->player_anim[i].state.position == sim->cur_dt);
    sim->pcombat[1].inv.health = 0;
    c->frame_index++; client_input_simulated(c, 0.05f);
    CHECK(c->death[1].stage == Q2_PDEATH_DYING && q2_player_move_is_death(c->death[1].move));
    for (ticks = 0; ticks < 20 && c->death[1].stage == Q2_PDEATH_DYING; ticks++) {
        c->frame_index++; client_input_simulated(c, 0.05f);
    }
    CHECK(c->death[1].stage == Q2_PDEATH_DOWN && ticks > 1 && ticks < 20);
    CHECK(client_mp_respawn(c, 1));
    CHECK(c->player_anim[1].state.move == Q2_PMOVE_STAND &&
          c->player_anim[1].state.position == 0 && !c->player_anim[1].state.flags);
    release_fixture(c);
}

static void test_native_player_selection(void)
{
    static const q2_player_move expected[] = {
        Q2_PMOVE_STAND, Q2_PMOVE_RUN, Q2_PMOVE_JUMP,
        Q2_PMOVE_PAIN3, Q2_PMOVE_DEATH1, Q2_PMOVE_ATTAK
    };
    client *c = fixture(4);
    q2_sim *sim = &c->sim[0];
    size_t i;
    int pi;
    animation_ranges(c);
    for (pi = 0; pi < 4; pi++) {
        for (i = 0; i < sizeof(native_player_selection) / sizeof(native_player_selection[0]); i++) {
            const int *row = native_player_selection[i];
            q2_sim_select_player(sim, pi);
            client_player_anim_reset(c, pi);
            sim->player[pi].jumped_this_tick = row[0] != 0;
            sim->player[pi].wish[0] = (s16)row[1];
            sim->player[pi].wish[2] = (s16)row[2];
            c->vw[pi].result = (s16)row[3];   /* 0x8004EE0C's return, dry 2 included */
            sim->combat.self.damage_serial++;
            sim->combat.self.last_mod = (s16)row[4];
            /* Alternate active and parked ownership, including player zero. */
            if (i & 1) q2_sim_select_player(sim, (pi + 1) % 4);
            q2_random_seed(1);
            client_player_anim_tick(c, pi, 12, false);
            CHECK(client_player_visual_move(c, pi) == expected[row[5]]);
            if (row[5] != Q2_PANIM_PAIN) CHECK(q2_random_state() == 1);
        }
    }
    release_fixture(c);
}

static void test_rounds_and_cameras(void)
{
    client *c = fixture(4);
    int pi;
    c->mp.mode = Q2_MP_VERSUS;
    c->mp.round_limit = 2;
    for (pi = 1; pi < 4; pi++) c->sim[0].pcombat[pi].inv.health = 0;
    client_score_deaths(c);
    CHECK(c->mp.end == Q2_MP_END_ROUND_OVER && c->mp.team_frags[0] == 1);
    q2_mp_round_start(&c->mp);
    CHECK(c->mp.end == Q2_MP_RUNNING && c->mp.team_frags[0] == 1);
    client_score_deaths(c);
    CHECK(c->mp.end == Q2_MP_END_MATCH_OVER && c->mp.team_frags[0] == 2);
    c->sim[0].player[2].pitch = 123; c->sim[0].player[2].yaw = 456;
    client_extra_camera(c, 2);
    c->sim[0].player[2].ent2_flags |= Q2_ENT2_DEAD;
    c->sim[0].player[2].pitch = 321; c->sim[0].player[2].yaw = 654;
    for (pi = 0; pi < 200; pi++) client_extra_camera(c, 2);
    CHECK(c->mp_camera_angles[2][0] == 123 && c->mp_camera_angles[2][1] == 456);
    CHECK(c->mp_camera_angles[2][2] == (-384 & 4095));
    CHECK(c->mp_camera_angles[1][2] == 0);
    c->mp.player_count = 2;
    c->gamepads.id[1] = 101;
    CHECK(!client_mp_results_input(c, 1.0f / 30.0f));
    buttons(c, 1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
    CHECK(!client_mp_results_input(c, 1.0f / 30.0f));
    CHECK(c->mp_results.ready == 2);
    CHECK(!q2_mp_results_tick(&c->mp_results, 2, 0, 3000));
    CHECK(c->mp_results.countdown == 150);
    CHECK(!q2_mp_results_tick(&c->mp_results, 2, 1, 150));
    CHECK(q2_mp_results_tick(&c->mp_results, 2, 0, 1));
    release_fixture(c);
}

static void test_teleports_lighting_and_bodies(void)
{
    client *c = fixture(4);
    q2_sim *sim = &c->sim[0];
    q2_start_pos sp = {0};
    s32 original[3];
    const q2_pdeath_stage retired[] = {
        Q2_PDEATH_DISSOLVING, Q2_PDEATH_FADING, Q2_PDEATH_GIBBED, Q2_PDEATH_GONE
    };
    int pi, stage;
    memcpy(original, sim->player[0].pos, sizeof(original));
    c->cam.yaw = 123;
    for (pi = 0; pi < 4; pi++) sim->player[pi].ent.node = pi + 11;
    for (pi = 1; pi < 4; pi++) {
        q2_sim_select_player(sim, pi);
        CHECK(sim->current_node == pi + 11);
        CHECK(client_light_node(c, 0) == 11);
        CHECK(client_light_node(c, pi) == pi + 11);
        sp.x = 7000 + pi * 1000; sp.y = 300 + pi * 100; sp.z = pi * 900;
        sp.angle = (s16)(pi * 512);
        client_queue_teleport(c, &sp);
        sim->combat.inv.health = (s16)(70 + pi);
    }
    q2_sim_select_player(sim, 0);
    CHECK(sim->current_node == 11);
    CHECK(client_apply_teleports(c));
    CHECK(memcmp(original, sim->player[0].pos, sizeof(original)) == 0);
    CHECK(c->cam.yaw == 123 && sim->cur_player == 0 && sim->current_node == 11);
    for (pi = 1; pi < 4; pi++) {
        CHECK(sim->player[pi].pos[0] == 7000 + pi * 1000);
        CHECK(sim->player[pi].pos[2] == pi * 900);
        CHECK(sim->ent_world.player[pi].pos[0] == sim->player[pi].pos[0]);
        CHECK(sim->pcombat[pi].self.origin[1] == q2_sim_origin_y(sim->player[pi].pos[1]));
        CHECK(sim->pcombat[pi].inv.health == 70 + pi);
        CHECK(c->mp_camera_angles[pi][1] == pi * 512);
        CHECK(!c->pending_teleport_have[pi]);
    }
    /* Player one's teleport updates its actual eye, without moving the others. */
    sp.x = 1234; sp.angle = 2048;
    client_queue_teleport(c, &sp);
    CHECK(client_apply_teleports(c));
    CHECK(c->cam.pos[0] == 1234 && c->cam.yaw == 2048);
    CHECK(c->cam.pos[1] == sp.y - Q2_VIEW_STAND);
    CHECK(sim->player[1].pos[0] == 8000);

    CHECK(client_targets_for(c, 0) == 3);
    c->death[1].stage = Q2_PDEATH_DYING;
    c->death[2].stage = Q2_PDEATH_DOWN;
    CHECK(client_targets_for(c, 0) == 3); /* Bodies can still be gibbed. */
    for (stage = 0; stage < 4; stage++) {
        c->death[1].stage = retired[stage];
        CHECK(client_targets_for(c, 0) == 2);
        CHECK(sim->world_target_count == 3);
        CHECK(sim->pcombat[1].self.takedamage == Q2_DAMAGE_NO);
    }
    CHECK(client_mp_respawn(c, 1));
    CHECK(client_targets_for(c, 0) == 3);
    CHECK(sim->pcombat[1].self.takedamage == Q2_DAMAGE_AIM);
    release_fixture(c);
}

/*
 * The GAME VARIABLES rows that have to survive the trip out of the menu.
 *
 * WEAPON STAY is 0x800B3360, a halfword of its own rather than a bit of the
 * cheat word at 0x800B29EC (gamevars.h), and the row wrote nothing on the far
 * end: `q2_entity_world.weapons_stay` had two readers and no writer, so the
 * weapons-stay branches at 0x80037E60 and 0x8005988C were dead code.
 */
static void test_game_variables(void)
{
    client *c = fixture(1);
    q2_sim *sim = &c->sim[0];

    c->menu.multiplayer = true;
    c->settings.v[Q2_SET_WEAPON_STAY] = 1;
    client_apply_settings(c);
    CHECK(sim->weapons_stay);

    c->settings.v[Q2_SET_WEAPON_STAY] = 0;
    client_apply_settings(c);
    CHECK(!sim->weapons_stay);

    /*
     * Single player too: 0x8001C698 zeroes the cheat word on its disabled arm
     * and never touches 0x800B3360, so this setting is not gated on the
     * session type here either. Both readers test deathmatch themselves.
     */
    c->menu.multiplayer = false;
    c->settings.v[Q2_SET_WEAPON_STAY] = 1;
    client_apply_settings(c);
    CHECK(sim->weapons_stay);
    CHECK(sim->cheats == 0);

    release_fixture(c);
}

static void test_multiplayer_parity(void)
{
    client *c = fixture(4);
    int pi, frame;
    q2_menu_init(&c->menu, &c->settings, 248);
    q2_mp_session_init(&c->mp, Q2_MP_TEAM_DEATHMATCH, 4);
    for (pi = 0; pi < 4; pi++) {
        CHECK(client_player_colour(c, pi) == (pi & 1));
        c->gamepads.id[pi] = (SDL_JoystickID)(100 + pi);
        c->gamepads.held[pi] = Q2_PAD_UP | Q2_PAD_L1 | Q2_PAD_R1;
    }
    c->mp.mode = Q2_MP_DEATHMATCH;
    CHECK(client_player_colour(c, 2) == 2);
    c->menu.mp_colour[2] = 3;
    CHECK(client_player_colour(c, 2) == 3);
    c->mp.end = Q2_MP_END_FRAG_LIMIT;
    c->shoot = c->mp_stage = true;
    for (frame = 0; frame < 6; frame++) {
        client_input_simulated(c, 0.05f);
        c->frame_index++;
        for (pi = 0; pi < 4; pi++) {
            CHECK(!c->mp_input[pi].attack && !c->mp_input[pi].forward);
            CHECK(!c->mp_input[pi].buttons && !c->mp_shots[pi]);
        }
    }
    CHECK(c->mp.banner_ticks < Q2_MP_BANNER_TICKS);
    CHECK(c->sim[0].level_time > 0);
    c->shoot = c->mp_stage = false;
    c->gamepads.held[1] = Q2_PAD_L1;
    CHECK(!client_mp_results_input(c, 0.01f));
    CHECK(c->mp_results.ready == 0); /* held on entry is not a press */
    buttons(c, 1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
    buttons(c, 1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
    CHECK(!client_mp_results_input(c, 0.01f));
    CHECK(c->mp_results.ready == 2); /* release/repress between renders */

    client_mp_configure(c, Q2_MP_VERSUS, 4, 10, 1, 3);
    CHECK(c->mp.time_limit == Q2_MP_NO_LIMIT);
    CHECK(c->menu.mp_team[2] == 0 && c->menu.mp_team[3] == 1);
    release_fixture(c);
}

/* Optional disc run verifies the actual transition and render target. A
 * scoreboard made solely of coloured bands passed the old lifecycle checks. */
static void test_results_disc(const char *disc_path, const char *capture)
{
    client *c = open_client(disc_path);
    q2_mp_session saved;
    const psx_framebuffer *fb;
    int pi, x, y, art_pixels = 0, bright_text = 0;

    client_mp_configure(c, Q2_MP_TEAM_DEATHMATCH, 4, 10, 10, 3);
    CHECK(client_load_zone(c, "MATRIX5", 0));
    CHECK(c->screen.view_count == 4);
    c->mp.frags[0] = 1;
    c->mp.frags[1] = 2;
    c->mp.frags[2] = -1;
    c->mp.frags[3] = 5;
    c->mp.team_frags[0] = 0;
    c->mp.team_frags[1] = 7;
    c->mp.end = Q2_MP_END_FRAG_LIMIT;
    c->mp_scoreboard = true;
    for (pi = 0; pi < 4; pi++)
        c->sim[0].player[pi].look_scheme = Q2_PAD_STYLE_RIGHT_STICK + pi % 3;
    saved = c->mp;

    CHECK(client_mp_load_results(c));
    CHECK(client_name_eq(c->map, "QMRESULT"));
    CHECK(c->screen.view_count == 1);
    CHECK(c->menu_font_ready && c->menu_font.item_resident);
    CHECK(memcmp(&c->mp, &saved, sizeof(saved)) == 0);
    for (pi = 0; pi < 4; pi++) {
        CHECK(!c->sim_ready[pi]);
        CHECK(c->sim[0].player[pi].look_scheme == Q2_PAD_STYLE_RIGHT_STICK + pi % 3);
    }
    c->mp_results.ready = 8;
    client_frame(c);
    fb = q2_screen_front(&c->screen);
    for (y = 0; y < fb->height; y++) {
        for (x = 0; x < fb->width; x++) {
            u16 pixel = fb->px[y * fb->width + x];
            int r = pixel & 31, g = (pixel >> 5) & 31, b = (pixel >> 10) & 31;
            /* The artwork occupies the lower centre, below the menu rows.
             * Its half-bright pixels cannot satisfy the bright glyph check. */
            if (y > fb->height - 35 && x > 160 && x < 352 && r + g + b > 6)
                art_pixels++;
            if (r > 23 && g > 23 && b > 23) bright_text++;
        }
    }
    CHECK(art_pixels > 500);
    CHECK(bright_text > 100);
    if (capture) CHECK(psx_fb_write_ppm(fb, capture) == Q2_OK);

    /* Loading again must not erase READY or restart the countdown. */
    c->mp_results.countdown = 42;
    CHECK(client_mp_load_results(c));
    CHECK(c->mp_results.ready == 8 && c->mp_results.countdown == 42);
    client_mp_return_to_menu(c);
    CHECK(client_name_eq(c->map, "QFRONT"));
    CHECK(c->in_front_end && !c->mp_enabled && !c->mp_scoreboard);
    CHECK(c->menu.page_id == Q2_PAGE_FRONT_MULTI);
    CHECK(c->menu_font.item_resident);
    close_client(c);
}

int main(int argc, char **argv)
{
    test_inputs();
    test_single_player_controller();
    test_controller_profiles();
    test_game_variables();
    test_multiplayer_parity();
    test_fast_frames();
    test_owned_respawn_input();
    test_pickups_and_respawn();
    test_damage();
    test_pain_animation();
    test_player_animation_clock();
    test_native_player_selection();
    test_rounds_and_cameras();
    test_teleports_lighting_and_bodies();
    test_virtual_controllers();
    if (argc > 1) test_results_disc(argv[1], argc > 2 ? argv[2] : NULL);
    printf("%d split client checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
