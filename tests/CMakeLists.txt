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

add_executable(test_build test_build.c)

target_link_libraries(test_build PRIVATE
    q2psx_common
    q2psx_build
)

add_test(NAME build_catalogue COMMAND test_build)

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

add_executable(test_gte_reference test_gte_reference.c)
target_link_libraries(test_gte_reference PRIVATE q2psx_psx)
add_test(NAME gte_reference COMMAND test_gte_reference)

add_executable(test_texture_cache test_texture_cache.c)
target_link_libraries(test_texture_cache PRIVATE q2psx_render)
add_test(NAME texture_cache COMMAND test_texture_cache)

add_executable(test_sim test_sim.c)

target_link_libraries(test_sim PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME sim_behaviour COMMAND test_sim)

add_executable(test_launchcfg test_launchcfg.c ../src/client/launchcfg.c)

target_include_directories(test_launchcfg PRIVATE ../src/client)
target_link_libraries(test_launchcfg PRIVATE
    q2psx_common
)

add_test(NAME launcher_config COMMAND test_launchcfg)

if(TARGET q2psx)
    add_executable(test_bloom test_bloom.c)
    target_link_libraries(test_bloom PRIVATE q2psx_display q2psx_screen)
    add_test(NAME bloom COMMAND test_bloom)
    set_tests_properties(bloom PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(test_crt_gl test_crt_gl.c)
    target_link_libraries(test_crt_gl PRIVATE q2psx_display)
    add_test(NAME crt_gpu COMMAND test_crt_gl)
    set_tests_properties(crt_gpu PROPERTIES SKIP_RETURN_CODE 77)
    set(client_extra_sources
        ../src/client/gamepad.c
        ../src/client/launcher.c
        ../src/client/launchcfg.c)
    add_executable(test_client_split test_client_split.c ${client_extra_sources})
    get_target_property(split_client_libraries q2psx LINK_LIBRARIES)
    add_executable(test_launcher test_launcher.c ../src/client/launchcfg.c)
    target_link_libraries(test_launcher PRIVATE ${split_client_libraries})
    add_test(NAME launcher_render COMMAND test_launcher --headless)
    add_executable(test_client_menu test_client_menu.c ${client_extra_sources})
    target_link_libraries(test_client_menu PRIVATE ${split_client_libraries})
    add_test(NAME menu_client COMMAND test_client_menu --headless)
    add_executable(test_client_crt test_client_crt.c ${client_extra_sources})
    target_link_libraries(test_client_crt PRIVATE ${split_client_libraries})
    add_dependencies(test_client_crt q2psx)
    add_test(NAME crt_client COMMAND test_client_crt)
    target_link_libraries(test_client_split PRIVATE ${split_client_libraries})
    add_dependencies(test_client_split q2psx)
    add_test(NAME split_client COMMAND test_client_split)

    add_executable(test_client_aspect test_client_aspect.c ${client_extra_sources})
    target_link_libraries(test_client_aspect PRIVATE ${split_client_libraries})
    add_dependencies(test_client_aspect q2psx)
    add_test(NAME aspect_client COMMAND test_client_aspect)

    add_executable(test_client_creatures test_client_creatures.c ${client_extra_sources})
    target_link_libraries(test_client_creatures PRIVATE ${split_client_libraries})
    add_dependencies(test_client_creatures q2psx)
    add_test(NAME creature_client COMMAND test_client_creatures)

    add_executable(test_client_save test_client_save.c ${client_extra_sources})
    target_link_libraries(test_client_save PRIVATE ${split_client_libraries})
    add_dependencies(test_client_save q2psx)
    add_test(NAME save_client COMMAND test_client_save)

    add_executable(test_client_weapon test_client_weapon.c ${client_extra_sources})
    target_link_libraries(test_client_weapon PRIVATE ${split_client_libraries})
    add_dependencies(test_client_weapon q2psx)
    add_test(NAME weapon_client COMMAND test_client_weapon)

    add_executable(test_client_audio test_client_audio.c ${client_extra_sources})
    target_link_libraries(test_client_audio PRIVATE ${split_client_libraries})
    add_test(NAME audio_client COMMAND test_client_audio)

    add_executable(test_client_audio_device test_client_audio_device.c ${client_extra_sources})
    target_link_libraries(test_client_audio_device PRIVATE ${split_client_libraries})
    add_test(NAME audio_device_client COMMAND test_client_audio_device)

    add_executable(test_client_muzzle test_client_muzzle.c ${client_extra_sources})
    target_link_libraries(test_client_muzzle PRIVATE ${split_client_libraries})
    add_dependencies(test_client_muzzle q2psx)
    add_test(NAME muzzle_client COMMAND test_client_muzzle)

    add_executable(test_client_placement test_client_placement.c ${client_extra_sources})
    target_link_libraries(test_client_placement PRIVATE ${split_client_libraries})
    add_dependencies(test_client_placement q2psx)
    add_test(NAME placement_client COMMAND test_client_placement)

    add_executable(test_client_effects test_client_effects.c ${client_extra_sources})
    target_link_libraries(test_client_effects PRIVATE ${split_client_libraries})
    add_dependencies(test_client_effects q2psx)
    add_test(NAME effects_client COMMAND test_client_effects)

    add_executable(test_client_movement test_client_movement.c ${client_extra_sources})
    target_link_libraries(test_client_movement PRIVATE ${split_client_libraries})
    add_dependencies(test_client_movement q2psx)
    add_test(NAME movement_client COMMAND test_client_movement)

    add_executable(test_client_population test_client_population.c ${client_extra_sources})
    target_link_libraries(test_client_population PRIVATE ${split_client_libraries})
    add_dependencies(test_client_population q2psx)
    add_test(NAME population_client COMMAND test_client_population)

    add_executable(test_client_zones test_client_zones.c ${client_extra_sources})
    target_link_libraries(test_client_zones PRIVATE ${split_client_libraries})
    add_dependencies(test_client_zones q2psx)
    add_test(NAME zones_client COMMAND test_client_zones)

    add_executable(test_client_memory test_client_memory.c ${client_extra_sources})
    target_link_libraries(test_client_memory PRIVATE ${split_client_libraries})
    add_dependencies(test_client_memory q2psx)
    add_test(NAME memory_client COMMAND test_client_memory)
endif()

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

add_executable(test_loadout test_loadout.c)

target_link_libraries(test_loadout PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME loadout COMMAND test_loadout)

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

add_executable(test_credits test_credits.c)
target_link_libraries(test_credits PRIVATE q2psx_game q2psx_screen)
add_test(NAME credits COMMAND test_credits)

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

add_executable(test_shadow test_shadow.c)
target_link_libraries(test_shadow PRIVATE q2psx_game q2psx_render)
add_test(NAME shadow_blobs COMMAND test_shadow)

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

add_executable(test_creinit test_creinit.c)
target_link_libraries(test_creinit PRIVATE q2psx_game)
add_test(NAME creature_initialization COMMAND test_creinit)

add_executable(test_cremotion test_cremotion.c)
target_link_libraries(test_cremotion PRIVATE q2psx_game q2psx_build)
add_test(NAME creature_motion COMMAND test_cremotion)

add_executable(test_mission test_mission.c)

target_link_libraries(test_mission PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME mission COMMAND test_mission)

add_executable(test_loading test_loading.c)

target_link_libraries(test_loading PRIVATE
    q2psx_common
    q2psx_game
    q2psx_menu
)

add_test(NAME loading COMMAND test_loading)

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

add_executable(test_raster_reference test_raster_reference.c)
target_link_libraries(test_raster_reference PRIVATE q2psx_render)
add_test(NAME raster_reference COMMAND test_raster_reference)

add_executable(test_raster_edges test_raster_edges.c)
target_link_libraries(test_raster_edges PRIVATE q2psx_render)
add_test(NAME raster_edges COMMAND test_raster_edges)

add_executable(test_subdiv_pool test_subdiv_pool.c)
target_link_libraries(test_subdiv_pool PRIVATE q2psx_game)
add_test(NAME subdivision_pool COMMAND test_subdiv_pool)

add_executable(test_crt test_crt.c)
target_link_libraries(test_crt PRIVATE q2psx_render)
add_test(NAME crt_display COMMAND test_crt)

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

add_executable(test_spu test_spu.c)
target_link_libraries(test_spu PRIVATE q2psx_audio)
add_test(NAME spu_hardware COMMAND test_spu)

add_executable(test_xa_resample test_xa_resample.c)
target_link_libraries(test_xa_resample PRIVATE q2psx_audio)
add_test(NAME xa_interpolation COMMAND test_xa_resample)

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

add_executable(test_events test_events.c)

target_link_libraries(test_events PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME event_runtime COMMAND test_events)

add_executable(test_shock test_shock.c)

target_link_libraries(test_shock PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME dualshock COMMAND test_shock)

add_executable(test_nativeheap test_nativeheap.c)
target_link_libraries(test_nativeheap PRIVATE q2psx_game)
add_test(NAME native_heap COMMAND test_nativeheap)
