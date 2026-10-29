add_library(q2psx_menu STATIC
    menu.c
    menu.h
    mpselect.c
    credits.c
    credits.h
    menudraw.c
    menudraw.h
    menufont.c
    menufont.h
    menumouse.c
    menumouse.h
    memcard.c
    memcard.h
    panel.c
    panel.h
    prompt.c
    prompt.h
    pages.c
)

target_include_directories(q2psx_menu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(q2psx_menu PUBLIC
    q2psx_common
    q2psx_build
    q2psx_formats
    q2psx_psx
    q2psx_render
)
