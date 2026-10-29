/* Retail main-RAM allocation accounting, independent of host allocations.
 * Addresses are console addresses; no payload bytes are stored here. */
#ifndef Q2PSX_NATIVEHEAP_H
#define Q2PSX_NATIVEHEAP_H

#include "q2psx.h"

#define Q2_NATIVE_HEAP_BLOCKS 128
#define Q2_NATIVE_HEAP_HEADER 28u

typedef struct q2_native_heap_block {
    u32 header, size;
} q2_native_heap_block;

typedef struct q2_native_heap {
    q2_native_heap_block block[Q2_NATIVE_HEAP_BLOCKS]; /* Address order. */
    u32 ceiling; /* Already excludes both native stack reservations. */
    unsigned count; /* Includes the zero-size sentinel. */
} q2_native_heap;

bool q2_native_heap_init(q2_native_heap *heap, u32 base, u32 ceiling);
u32 q2_native_heap_alloc(q2_native_heap *heap, u32 size, bool high);
bool q2_native_heap_free(q2_native_heap *heap, u32 payload);
/* The loader only shrinks files. Growing reallocations are unsupported. */
u32 q2_native_heap_shrink(q2_native_heap *heap, u32 payload, u32 size, bool tail);
/* Returns the remaining requested size, as the native helper does. */
u32 q2_native_heap_move_header(q2_native_heap *heap, u32 payload, u32 replacement);
u32 q2_native_heap_largest(const q2_native_heap *heap);

#endif
