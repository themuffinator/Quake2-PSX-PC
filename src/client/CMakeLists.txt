add_executable(q2psx main.c gamepad.c launcher.c launchcfg.c)

add_library(q2psx_display STATIC crt_gl.c)
target_include_directories(q2psx_display PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(q2psx_display PUBLIC q2psx_render SDL3::SDL3)

target_link_libraries(q2psx PRIVATE
    q2psx_display
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

# Imported SDL builds need the same runtime placement as fetched SDL builds.
# Otherwise a successful MSVC link produces an executable that cannot start.
if(WIN32)
    set_target_properties(q2psx PROPERTIES WIN32_EXECUTABLE TRUE)
    target_sources(q2psx PRIVATE entry_win.c)
    get_target_property(q2psx_sdl_type SDL3::SDL3 TYPE)
    if(q2psx_sdl_type STREQUAL "SHARED_LIBRARY")
        add_custom_command(TARGET q2psx POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:q2psx>
            VERBATIM)
    endif()

    # Always refresh the local launch directory, even when q2psx is up to date.
    # Copy only build-owned files; disc images, saves and settings stay in place.
    # Stamp the same standalone guide that scripts/package.py ships. No Python
    # dependency is needed to stage a local client. Editing the guide reconfigures.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/USER-GUIDE.html")
    file(READ "${PROJECT_SOURCE_DIR}/USER-GUIDE.html" q2psx_guide)
    set(q2psx_guide_marker "<span id=\"build-version\">Development edition</span>")
    string(FIND "${q2psx_guide}" "${q2psx_guide_marker}" q2psx_guide_marker_pos)
    if(q2psx_guide_marker_pos EQUAL -1)
        message(FATAL_ERROR "USER-GUIDE.html is missing its version marker")
    endif()
    string(REPLACE "${q2psx_guide_marker}"
        "<span id=\"build-version\">Version ${PROJECT_VERSION}</span>"
        q2psx_guide "${q2psx_guide}")
    file(WRITE "${CMAKE_BINARY_DIR}/generated/USER-GUIDE.html" "${q2psx_guide}")
    set(q2psx_stage_files
        $<TARGET_FILE:q2psx>
        "${PROJECT_SOURCE_DIR}/scripts/Play.cmd"
        "${PROJECT_SOURCE_DIR}/scripts/Play.vbs"
        "${CMAKE_BINARY_DIR}/generated/USER-GUIDE.html"
        "${PROJECT_SOURCE_DIR}/CREDITS.md"
        "${PROJECT_SOURCE_DIR}/LICENSE")
    if(q2psx_sdl_type STREQUAL "SHARED_LIBRARY")
        list(APPEND q2psx_stage_files $<TARGET_FILE:SDL3::SDL3>)
    endif()
    add_custom_target(stage-local ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PROJECT_SOURCE_DIR}/.install"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${q2psx_stage_files} "${PROJECT_SOURCE_DIR}/.install"
        DEPENDS q2psx
        VERBATIM)
endif()

# A release archive puts the client and a fetched libSDL3 in the same directory,
# so the loader has to look there. CMake appends this to the rpath it computes
# for the build tree rather than replacing it, so running out of build/bin keeps
# working whether SDL3 came from the system or from FetchContent.
if(APPLE)
    set_target_properties(q2psx PROPERTIES BUILD_RPATH "@executable_path")
elseif(UNIX)
    set_target_properties(q2psx PROPERTIES BUILD_RPATH "$ORIGIN")
endif()
