#include "nativeheap.h"

#include <string.h>

static u32 span(u32 size)
{
    return (size + 31u) & ~3u;
}

static unsigned find(const q2_native_heap *h, u32 payload)
{
    unsigned i;
    for (i=1; i<h->count; ++i)
        if (h->block[i].header + Q2_NATIVE_HEAP_HEADER == payload) return i;
    return h->count;
}

bool q2_native_heap_init(q2_native_heap *h, u32 base, u32 ceiling)
{
    if (!h || (base & 3u) || (ceiling & 3u) || ceiling < base ||
        ceiling-base < 2*Q2_NATIVE_HEAP_HEADER) return false;
    memset(h,0,sizeof(*h));
    h->block[0].header=base;
    h->ceiling=ceiling;
    h->count=1;
    return true;
}

u32 q2_native_heap_alloc(q2_native_heap *h, u32 size, bool high)
{
    unsigned i, at=0;
    u32 address=0, need;
    if (!h || !h->count || h->count==Q2_NATIVE_HEAP_BLOCKS || size>0xffffffe0u)
        return 0;
    need=span(size);
    /* 0x8006E468: first fitting gap for low, last fitting gap for high. */
    for (i=0; i<h->count; ++i) {
        u32 begin=h->block[i].header+span(h->block[i].size);
        u32 end=i+1<h->count ? h->block[i+1].header : h->ceiling;
        if (end>=begin && end-begin>=need) {
            address=high ? end-need : begin;
            at=i+1;
            if (!high) break;
        }
    }
    if (!at) return 0;
    memmove(h->block+at+1,h->block+at,(h->count-at)*sizeof(h->block[0]));
    h->block[at].header=address;
    h->block[at].size=size;
    ++h->count;
    return address+Q2_NATIVE_HEAP_HEADER;
}

bool q2_native_heap_free(q2_native_heap *h, u32 payload)
{
    unsigned i;
    if (!h || !h->count) return false;
    if (!payload) return true;
    i=find(h,payload);
    if (i==h->count) return false;
    memmove(h->block+i,h->block+i+1,(h->count-i-1)*sizeof(h->block[0]));
    --h->count;
    return true;
}

u32 q2_native_heap_move_header(q2_native_heap *h, u32 payload, u32 replacement)
{
    unsigned i;
    u32 delta;
    if (!h || replacement<payload) return 0;
    i=find(h,payload);
    if (i==h->count) return 0;
    delta=replacement-payload;
    if (delta>h->block[i].size) return 0;
    h->block[i].header+=delta;
    h->block[i].size-=delta;
    return h->block[i].size;
}

u32 q2_native_heap_shrink(q2_native_heap *h, u32 payload, u32 size, bool tail)
{
    unsigned i;
    if (!h) return 0;
    i=find(h,payload);
    if (i==h->count || size>h->block[i].size) return 0;
    if (tail) {
        /* 0x8006E610 really clears bit 1, not both low bits. Retain the
         * instruction's behavior; shipped archive lengths are aligned. */
        u32 delta=(h->block[i].size-size) & 0xfffffffdu;
        q2_native_heap_move_header(h,payload,payload+delta);
        return payload+delta;
    }
    h->block[i].size=size;
    return payload;
}

u32 q2_native_heap_largest(const q2_native_heap *h)
{
    unsigned i;
    u32 largest=0;
    if (!h || !h->count) return 0;
    for (i=0; i<h->count; ++i) {
        u32 begin=h->block[i].header+span(h->block[i].size);
        u32 end=i+1<h->count ? h->block[i+1].header : h->ceiling;
        if (end>=begin && end-begin>largest) largest=end-begin;
    }
    /* 0x8006EACC returns an unsigned subtraction, even below 28 bytes. */
    return largest-Q2_NATIVE_HEAP_HEADER;
}
