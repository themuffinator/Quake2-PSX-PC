add_executable(q2psx main.c)

target_link_libraries(q2psx PRIVATE
    q2psx_common
    q2psx_disc
    q2psx_build
    q2psx_formats
    q2psx_psx
    q2psx_render
    q2psx_menu
    q2psx_screen
    q2psx_audio
    q2psx_game
    SDL3::SDL3
)

# A release archive puts the client and a fetched libSDL3 in the same directory,
# so the loader has to look there. CMake appends this to the rpath it computes
# for the build tree rather than replacing it, so running out of build/bin keeps
# working whether SDL3 came from the system or from FetchContent.
if(APPLE)
    set_target_properties(q2psx PROPERTIES BUILD_RPATH "@executable_path")
elseif(UNIX)
    set_target_properties(q2psx PROPERTIES BUILD_RPATH "$ORIGIN")
endif()
