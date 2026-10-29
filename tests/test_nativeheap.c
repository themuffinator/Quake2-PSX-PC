#include "nativeheap.h"
#include "levelmemory.h"

#include <stdio.h>
#include <string.h>

#include "fixtures/heap_reference.h"
#include "fixtures/level_memory_reference.h"
#include "fixtures/level_memory_ntsc_reference.h"
#include "fixtures/level_memory_session_reference.h"
#include "fixtures/level_memory_ntsc_session_reference.h"

static unsigned failures, checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    printf("FAIL line %d: %s\n",__LINE__,#x); ++failures; } } while (0)

static u32 allocation_hash(const q2_native_heap *h)
{
    u32 hash=2166136261u;
    unsigned i, field, byte;
    for (i=0; i<h->count; ++i) {
        u32 values[2]={h->block[i].header,h->block[i].size};
        for (field=0; field<2; ++field)
            for (byte=0; byte<4; ++byte)
                hash=(hash ^ ((values[field]>>(8*byte)) & 255u))*16777619u;
    }
    return hash;
}

static void native_traces(void)
{
    unsigned scenario, step;
    for (scenario=0; scenario<sizeof(heap_reference_cases)/sizeof(heap_reference_cases[0]); ++scenario) {
        const struct heap_reference_case *c=&heap_reference_cases[scenario];
        q2_native_heap h;
        CHECK(q2_native_heap_init(&h,0x800ee718u,0x801ff600u));
        for (step=0; step<c->count; ++step) {
            const struct heap_reference_step *v=&heap_reference_steps[c->first+step];
            u32 actual=0;
            switch (v->operation) {
            case 0: actual=q2_native_heap_alloc(&h,v->a,v->b!=0); break;
            case 1: actual=q2_native_heap_shrink(&h,v->a,v->b,v->c!=0); break;
            case 2: CHECK(q2_native_heap_free(&h,v->a)); break;
            case 3: actual=q2_native_heap_move_header(&h,v->a,v->b); break;
            case 4: actual=q2_native_heap_largest(&h); break;
            default: CHECK(false); break;
            }
            if (actual!=v->result || h.count!=v->count || allocation_hash(&h)!=v->hash)
                printf("case %u step %u operation %u: result %08x/%08x, blocks %u/%u, hash %08x/%08x\n",
                    scenario,step,v->operation,actual,v->result,h.count,v->count,allocation_hash(&h),v->hash);
            CHECK(actual==v->result);
            CHECK(h.count==v->count);
            CHECK(allocation_hash(&h)==v->hash);
        }
    }
}

static void invalid_inputs(void)
{
    q2_native_heap h, saved;
    u32 p;
    CHECK(!q2_native_heap_init(NULL,0x1000,0x2000));
    CHECK(!q2_native_heap_init(&h,0x2000,0x1000));
    CHECK(!q2_native_heap_init(&h,0x1001,0x2000));
    CHECK(q2_native_heap_init(&h,0x1000,0x2000));
    p=q2_native_heap_alloc(&h,64,false);
    CHECK(p!=0);
    saved=h;
    CHECK(!q2_native_heap_free(&h,p+1));
    CHECK(!q2_native_heap_shrink(&h,p,65,false));
    CHECK(!q2_native_heap_move_header(&h,p,p+65));
    CHECK(!q2_native_heap_alloc(&h,0xffffffffu,false));
    CHECK(!memcmp(&h,&saved,sizeof(h)));
    CHECK(q2_native_heap_free(&h,p));
    CHECK(q2_native_heap_free(&h,0));
    CHECK(!q2_native_heap_free(&h,p));
}

static void native_loads(const struct level_memory_reference *reference, unsigned count)
{
    q2_level_memory m={0};
    unsigned i;
    for (i=0;i<count;++i) {
        const struct level_memory_reference *v=&reference[i];
        if (v->reset) {
            memset(&m,0,sizeof(m));
            CHECK(q2_native_heap_init(&m.heap,v->heap_base,0x801ff600u));
        }
        CHECK(q2_level_memory_load(&m,&v->files,&v->resources,v->mission,v->views,
                                   v->multiplayer!=0,v->game_level!=0,v->stream!=0));
        if (m.largest_payload!=v->largest || allocation_hash(&m.heap)!=v->hash)
            printf("load case %u: largest %u/%u, hash %08x/%08x\n",i,
                m.largest_payload,v->largest,allocation_hash(&m.heap),v->hash);
        CHECK(m.largest_payload==v->largest);
        CHECK(m.packets==v->packets);
        CHECK(m.meshes==v->meshes);
        CHECK(m.mission_unit==v->mission);
        CHECK(m.heap.count==v->count);
        CHECK(allocation_hash(&m.heap)==v->hash);
    }
    {
        q2_level_memory saved=m;
        q2_level_memory_files broken=reference[0].files;
        broken.zone_size=0x10000000;
        CHECK(!q2_level_memory_load(&m,&broken,&reference[0].resources,1,1,false,true,false));
        CHECK(!memcmp(&m,&saved,sizeof(m)));
    }
}

int main(void)
{
    native_traces();
    native_loads(level_memory_reference,Q2PSX_ARRAY_COUNT(level_memory_reference));
    native_loads(level_memory_ntsc_reference,Q2PSX_ARRAY_COUNT(level_memory_ntsc_reference));
    native_loads(level_memory_session_reference,Q2PSX_ARRAY_COUNT(level_memory_session_reference));
    native_loads(level_memory_ntsc_session_reference,Q2PSX_ARRAY_COUNT(level_memory_ntsc_session_reference));
    invalid_inputs();
    printf("native heap: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
