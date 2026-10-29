add_library(q2psx_audio STATIC
    vag.c
    vag.h
    spu.c
    spu.h
    spu_gauss.h
    spatial.c
    spatial.h
    xa.c
    xa.h
    xa_impulse.h
)

target_include_directories(q2psx_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(q2psx_audio PUBLIC q2psx_common q2psx_disc)
