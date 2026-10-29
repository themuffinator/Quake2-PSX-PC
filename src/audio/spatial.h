#ifndef Q2PSX_AUDIO_SPATIAL_H
#define Q2PSX_AUDIO_SPATIAL_H
#include "q2psx.h"

typedef struct q2_sfx_listener {
    s32 pos[3];
    s16 matrix[3][3]; /* GameWindow+192: unscaled view basis */
} q2_sfx_listener;

typedef struct q2_sfx_spatial {
    s32 distance, level, pan;
    s32 projected[3]; /* Retail retains the LAST window's vector for pan. */
} q2_sfx_spatial;

/* 0x80071DD8, 0x8006FB18 and 0x8008A7E8. Sources use signed-short
 * deltas, a 32-bit matrix accumulator, and the SDK's approximate square root. */
u32 q2_sfx_distance_root(u32 square);
q2_sfx_spatial q2_sfx_spatialize(const s32 at[3], u16 volume,
                                const q2_sfx_listener *listeners, u32 count);
/* 0x80071F30: direct SPU register value, with 0x4000 representing unity.
 * Mono uses level*slider/2; stereo uses level*pan*slider/512. */
s32 q2_sfx_volume_register(u16 level, u8 pan, u8 slider, bool mono);
#endif
