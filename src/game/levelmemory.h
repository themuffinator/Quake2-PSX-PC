/* Native main-RAM lifetimes which determine the renderer's packet capacity. */
#ifndef Q2PSX_LEVELMEMORY_H
#define Q2PSX_LEVELMEMORY_H
#include "nativeheap.h"
#include "level.h"
#include "exe.h"

typedef struct q2_level_memory_files {
    u32 common_size, common_prefix, zone_size, zone_prefix;
    u32 media_size, media_sound_offset, media_header_size, palette_bytes;
} q2_level_memory_files;

typedef struct q2_level_memory {
    q2_native_heap heap;
    u32 mission, media, common, zone, palettes;
    u32 actors, origins, speeds, paths, water, particles, parents;
    u32 largest_payload, packets, meshes;
    unsigned views, mission_unit;
    bool multiplayer, loaded;
} q2_level_memory;

bool q2_level_memory_init(q2_level_memory *memory, const q2_exe *exe);
bool q2_level_memory_files_read(q2_level_memory_files *out,
    const q2_common_file *common, const q2_zone_file *zone, const q2_buf *media);
/* Full loads unload the previous map, replace the mission record and retain
 * water until the native loader's later release point. Streams retain COMMON,
 * media/palette headers, mission and session pools. Failure leaves m unchanged.
 * mission_unit is 0 for no record, 1..5 for campaign and 6 for Tribute.
 * Multiplayer uses the native null mission and has no mission record. */
bool q2_level_memory_load(q2_level_memory *m, const q2_level_memory_files *files,
    const q2_zone_resources *resources, unsigned mission_unit, unsigned views,
    bool multiplayer, bool game_level, bool stream);
#endif
