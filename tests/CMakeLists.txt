add_executable(test_surface test_surface.c)

target_link_libraries(test_surface PRIVATE
    q2psx_common
    q2psx_formats
)

add_test(NAME surface COMMAND test_surface)

add_executable(test_disc test_disc.c)

target_link_libraries(test_disc PRIVATE
    q2psx_common
    q2psx_disc
)

add_test(NAME disc_cue_layout COMMAND test_disc)

add_executable(test_light test_light.c)

target_link_libraries(test_light PRIVATE
    q2psx_common
    q2psx_game
    q2psx_psx
)

add_test(NAME light_model COMMAND test_light)

add_executable(test_gte test_gte.c)

target_link_libraries(test_gte PRIVATE
    q2psx_common
    q2psx_psx
)

add_test(NAME gte_conformance COMMAND test_gte)

add_executable(test_sim test_sim.c)

target_link_libraries(test_sim PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME sim_behaviour COMMAND test_sim)

add_executable(test_pad test_pad.c)

target_link_libraries(test_pad PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME pad_and_view COMMAND test_pad)

add_executable(test_inventory test_inventory.c)

target_link_libraries(test_inventory PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME inventory COMMAND test_inventory)

add_executable(test_monster test_monster.c)

target_link_libraries(test_monster PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME monster COMMAND test_monster)

add_executable(test_combat test_combat.c)

target_link_libraries(test_combat PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME combat COMMAND test_combat)

add_executable(test_save test_save.c)

target_link_libraries(test_save PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME save COMMAND test_save)

add_executable(test_ai test_ai.c)

target_link_libraries(test_ai PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME ai COMMAND test_ai)

add_executable(test_menu test_menu.c)

target_link_libraries(test_menu PRIVATE
    q2psx_common
    q2psx_menu
)

add_test(NAME menu COMMAND test_menu)

add_executable(test_hud test_hud.c)

target_link_libraries(test_hud PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME hud COMMAND test_hud)

add_executable(test_screen test_screen.c)

target_link_libraries(test_screen PRIVATE
    q2psx_common
    q2psx_screen
)

add_test(NAME screen COMMAND test_screen)

add_executable(test_weapon test_weapon.c)

target_link_libraries(test_weapon PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME weapon COMMAND test_weapon)

add_executable(test_coll test_coll.c)

target_link_libraries(test_coll PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME collision COMMAND test_coll)

add_executable(test_item test_item.c)

target_link_libraries(test_item PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME item COMMAND test_item)

add_executable(test_multiplayer test_multiplayer.c)

target_link_libraries(test_multiplayer PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME multiplayer COMMAND test_multiplayer)

add_executable(test_playerdeath test_playerdeath.c)

target_link_libraries(test_playerdeath PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME player_death COMMAND test_playerdeath)

add_executable(test_effect test_effect.c)

target_link_libraries(test_effect PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME effect COMMAND test_effect)

add_executable(test_rotator test_rotator.c)

target_link_libraries(test_rotator PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME rotator COMMAND test_rotator)

add_executable(test_creature test_creature.c)

target_link_libraries(test_creature PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME creature COMMAND test_creature)

add_executable(test_mission test_mission.c)

target_link_libraries(test_mission PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME mission COMMAND test_mission)

add_executable(test_viewweapon test_viewweapon.c)

target_link_libraries(test_viewweapon PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME viewweapon COMMAND test_viewweapon)

add_executable(test_statusbar test_statusbar.c)

target_link_libraries(test_statusbar PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME statusbar COMMAND test_statusbar)

add_executable(test_panel test_panel.c)

target_link_libraries(test_panel PRIVATE
    q2psx_common
    q2psx_game
    q2psx_menu
)

add_test(NAME panel COMMAND test_panel)

add_executable(test_raster test_raster.c)

target_link_libraries(test_raster PRIVATE
    q2psx_common
    q2psx_psx
    q2psx_render
)

add_test(NAME raster_quads COMMAND test_raster)

add_executable(test_world test_world.c)

target_link_libraries(test_world PRIVATE
    q2psx_common
    q2psx_formats
    q2psx_game
    q2psx_psx
)

add_test(NAME world_quads COMMAND test_world)

add_executable(test_leveltext test_leveltext.c)

target_link_libraries(test_leveltext PRIVATE
    q2psx_common
    q2psx_formats
)

add_test(NAME leveltext COMMAND test_leveltext)

add_executable(test_model test_model.c)

target_link_libraries(test_model PRIVATE
    q2psx_common
    q2psx_formats
)

add_test(NAME model COMMAND test_model)

add_executable(test_movie test_movie.c)

target_link_libraries(test_movie PRIVATE
    q2psx_common
    q2psx_formats
)

add_test(NAME movie_table COMMAND test_movie)

# The encoder. Needs the audio library for the ADPCM half and the disc library
# for the sector builder — which is the shape of the format: a film is a
# picture, a soundtrack and a sector layout, and all three have to be right.
add_executable(test_stxenc test_stxenc.c)

target_link_libraries(test_stxenc PRIVATE
    q2psx_common
    q2psx_formats
    q2psx_audio
    q2psx_disc
)

add_test(NAME movie_encoder COMMAND test_stxenc)

add_executable(test_sound_pitch test_sound_pitch.c)

target_link_libraries(test_sound_pitch PRIVATE
    q2psx_common
    q2psx_audio
)

add_test(NAME sound_pitch COMMAND test_sound_pitch)

add_executable(test_explosive test_explosive.c)

target_link_libraries(test_explosive PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME explosive COMMAND test_explosive)

add_executable(test_modelent test_modelent.c)

target_link_libraries(test_modelent PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME modelent COMMAND test_modelent)
