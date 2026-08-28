cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Version
#
# The version lives in the VERSION file at the repository root and nowhere
# else. This reads it, project() derives PROJECT_VERSION_* from it, the
# generated header derives from those, and scripts/version.py reads the same
# file — so the binary, the git tag and the release archives cannot disagree
# about what was built.
#
# A plain text file rather than a literal in the project() call below: cutting
# a release means writing the new version, and rewriting a file that holds one
# number cannot go wrong the way a regex against build-system source can.
# ---------------------------------------------------------------------------
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" Q2PSX_VERSION_RAW)
string(STRIP "${Q2PSX_VERSION_RAW}" Q2PSX_VERSION_RAW)

if(NOT Q2PSX_VERSION_RAW MATCHES "^[0-9]+\.[0-9]+\.[0-9]+$")
    message(FATAL_ERROR
        "VERSION must hold a bare MAJOR.MINOR.PATCH; got '${Q2PSX_VERSION_RAW}'")
endif()

project(Q2PSX-PC
        VERSION ${Q2PSX_VERSION_RAW}
        DESCRIPTION "Native PC recreation of Quake II for PlayStation 1"
        LANGUAGES C)

# Editing VERSION re-runs configure, so a bumped version cannot be left behind
# in an incremental build.
set_property(DIRECTORY APPEND PROPERTY
    CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Shared libraries land beside the executables that need them. A fetched SDL3
# is the only one, and a release archive is assembled by copying this directory
# — if the library built somewhere else, the archive ships a client that cannot
# start.
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# Generated version header
#
# The generated header derives from PROJECT_VERSION, which came from the
# VERSION file above, so a binary cannot report a version that disagrees with
# the build system or with the tag the release workflow pushed.
#
# Git state is captured when available. Outside a checkout the describe string
# falls back to the bare version rather than inventing a hash.
# ---------------------------------------------------------------------------
set(Q2PSX_GIT_DESCRIBE "v${PROJECT_VERSION}")
set(Q2PSX_GIT_DIRTY 0)

find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty=-dirty
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_DESC
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    if(GIT_DESC)
        set(Q2PSX_GIT_DESCRIBE "${GIT_DESC}")
        if(GIT_DESC MATCHES "-dirty$")
            set(Q2PSX_GIT_DIRTY 1)
        endif()
    endif()
endif()

configure_file(
    ${CMAKE_SOURCE_DIR}/src/common/version.h.in
    ${CMAKE_BINARY_DIR}/generated/version.h
    @ONLY)

option(Q2PSX_BUILD_TOOLS  "Build offline inspection tools"    ON)
option(Q2PSX_BUILD_CLIENT "Build the playable client (needs SDL3)" ON)
option(Q2PSX_FETCH_SDL3   "Fetch and build SDL3 if not installed"  OFF)
option(Q2PSX_WERROR      "Treat warnings as errors"           OFF)

# ---------------------------------------------------------------------------
# Warning flags
# ---------------------------------------------------------------------------
add_library(q2psx_flags INTERFACE)

if(MSVC)
    target_compile_options(q2psx_flags INTERFACE /W4 /permissive- /utf-8)
    target_compile_definitions(q2psx_flags INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(Q2PSX_WERROR)
        target_compile_options(q2psx_flags INTERFACE /WX)
    endif()
else()
    target_compile_options(q2psx_flags INTERFACE
        -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual
        -Wstrict-prototypes -Wmissing-prototypes -Wno-unused-parameter)
    if(Q2PSX_WERROR)
        target_compile_options(q2psx_flags INTERFACE -Werror)
    endif()
    # clang targeting the MSVC runtime still pulls in the UCRT's deprecation
    # attributes, which fire on every fopen/strncpy in portable code.
    if(WIN32)
        target_compile_definitions(q2psx_flags INTERFACE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()

# Signed integer overflow is well-defined two's-complement here: the GTE and the
# fixed-point math deliberately rely on wrapping to reproduce hardware saturation
# behaviour bit-for-bit.
if(NOT MSVC)
    target_compile_options(q2psx_flags INTERFACE -fwrapv)
endif()

# glibc keeps the math functions in a library of their own; on Windows and macOS
# they are part of the C runtime, which is why nothing here had ever asked for
# them and every Linux link failed on undefined `sin` and `sqrt`. Carried on the
# flags target so every library and executable inherits it, since the trig and
# the fixed-point helpers are used throughout.
if(UNIX)
    target_link_libraries(q2psx_flags INTERFACE m)
endif()

# ---------------------------------------------------------------------------
# Core libraries
# ---------------------------------------------------------------------------
add_subdirectory(src/common)
add_subdirectory(src/disc)
add_subdirectory(src/build)
add_subdirectory(src/formats)
add_subdirectory(src/psx)
add_subdirectory(src/audio)
add_subdirectory(src/render)
add_subdirectory(src/menu)
add_subdirectory(src/screen)
add_subdirectory(src/game)

enable_testing()
add_subdirectory(tests)

if(Q2PSX_BUILD_TOOLS)
    add_subdirectory(tools/q2psx-inspect)
    add_subdirectory(tools/stx2avi)
endif()

# ---------------------------------------------------------------------------
# Client (optional — requires SDL3)
# ---------------------------------------------------------------------------
if(Q2PSX_BUILD_CLIENT)
    find_package(SDL3 QUIET CONFIG)

    # No system SDL3? Optionally fetch and build it. This is off by default
    # because it turns a five-second configure into a multi-minute one, and the
    # core libraries and offline tools do not need it.
    if(NOT SDL3_FOUND AND Q2PSX_FETCH_SDL3)
        message(STATUS "SDL3 not found — fetching it (this takes a while)")
        include(FetchContent)
        FetchContent_Declare(SDL3
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG        release-3.2.8
            GIT_SHALLOW    TRUE)
        set(SDL_SHARED ON  CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
        set(SDL_TEST   OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(SDL3)
        set(SDL3_FOUND TRUE)
    endif()

    if(SDL3_FOUND)
        message(STATUS "building the playable client")
        add_subdirectory(src/client)
    else()
        message(STATUS "SDL3 not found — skipping the client; core libs and tools still build.")
        message(STATUS "  configure with -DQ2PSX_FETCH_SDL3=ON to build SDL3 from source,")
        message(STATUS "  or set SDL3_DIR to an existing install.")
    endif()
endif()

message(STATUS "")
message(STATUS "Q2PSX-PC ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler   : ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  tools      : ${Q2PSX_BUILD_TOOLS}")
message(STATUS "")
