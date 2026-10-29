add_library(q2psx_render STATIC
    crt.c
    crt.h
    font.c
    font.h
    raster.c
    raster.h
    raster_scan.inc
)

target_include_directories(q2psx_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(q2psx_render PUBLIC q2psx_common q2psx_psx)
