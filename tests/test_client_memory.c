/* Disc-backed loader wiring against independently executed native states. */
int q2_test_memory_client_main(int argc,char **argv);
#define main q2_test_memory_client_main
#include "../src/client/main.c"
#undef main
static unsigned checks,failures;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); ++failures; } } while (0)
#include "client_fixture.h"
#include "fixtures/level_memory_reference.h"
#include "fixtures/level_memory_ntsc_reference.h"
#include "fixtures/level_memory_session_reference.h"
#include "fixtures/level_memory_ntsc_session_reference.h"
#include "fixtures/level_mission_reference.h"

static u32 heap_hash(const q2_native_heap *h)
{
    u32 hash=2166136261u;
    unsigned i,j,k;
    for (i=0;i<h->count;++i) {
        u32 values[2]={h->block[i].header,h->block[i].size};
        for (j=0;j<2;++j) for (k=0;k<4;++k)
            hash=(hash^((values[j]>>(8*k))&255u))*16777619u;
    }
    return hash;
}

static void disc_mission_table(const char *path)
{
    client *c=open_client(path);
    unsigned i;
    c->level_table_ready=q2_level_table_load(&c->level_table,c->disc,&c->build)==Q2_OK;
    CHECK(c->level_table_ready);
    CHECK(c->level_table.count==Q2PSX_ARRAY_COUNT(level_mission_reference)+1);
    if (c->level_table_ready && c->level_table.count>=Q2PSX_ARRAY_COUNT(level_mission_reference)) {
        for (i=0;i<Q2PSX_ARRAY_COUNT(level_mission_reference);++i) {
            const struct level_mission_reference *v=&level_mission_reference[i];
            const q2_level_entry *entry=&c->level_table.entries[i];
            CHECK(!strcmp(entry->display,c->build.video==Q2_VIDEO_NTSC ? v->display_ntsc : v->display));
            CHECK(!strcmp(entry->directory,v->directory));
            CHECK(entry->mission_unit==(s16)v->mission);
        }
    }
    close_client(c);
}

static void disc_loads(const char *path, bool session)
{
    client *c;
    const struct level_memory_reference *reference;
    unsigned i,count;
    q2_level_reset();
    c=open_client(path);
    if (c->build.video==Q2_VIDEO_NTSC) {
        reference=session ? level_memory_ntsc_session_reference : level_memory_ntsc_reference;
        count=session ? Q2PSX_ARRAY_COUNT(level_memory_ntsc_session_reference)
                      : Q2PSX_ARRAY_COUNT(level_memory_ntsc_reference);
    } else {
        reference=session ? level_memory_session_reference : level_memory_reference;
        count=session ? Q2PSX_ARRAY_COUNT(level_memory_session_reference)
                      : Q2PSX_ARRAY_COUNT(level_memory_reference);
    }
    for (i=0;i<count;++i) {
        const struct level_memory_reference *v=&reference[i];
        if (v->reset) {
            if (i) {
                close_client(c);
                q2_level_reset(); /* Each reset models a fresh executable process. */
                c=open_client(path);
            }
        }
        c->in_front_end=!v->game_level;
        if (v->multiplayer && (!c->mp_enabled || c->mp.player_count!=(int)v->views))
            q2_mp_session_init(&c->mp,Q2_MP_DEATHMATCH,(int)v->views);
        c->mp_enabled=v->multiplayer!=0;
        c->carry_player=c->carry_same_map=v->stream!=0;
        CHECK(client_load_zone(c,v->map,(int)v->zone));
        CHECK(c->level_memory_current && !c->level_memory_failures);
        CHECK(c->level_memory.mission_unit==v->mission);
        CHECK(c->level_memory.largest_payload==v->largest);
        CHECK(32u*c->render.packet_capacity==v->packets);
        CHECK(c->render.subdiv_capacity==v->meshes);
        CHECK(c->level_memory.heap.count==v->count);
        if (heap_hash(&c->level_memory.heap)!=v->hash)
            fprintf(stderr,"%s zone %u views %u: hash %08x/%08x\n",v->map,v->zone,v->views,
                heap_hash(&c->level_memory.heap),v->hash);
        CHECK(heap_hash(&c->level_memory.heap)==v->hash);
        if (c->sim[0].scene_ready) {
            unsigned j;
            bool retained=false;
            for (j=0;j<c->zone_history.count;++j)
                if (client_name_eq(c->zone_history.map[j].map,"QFRONT")) retained=true;
            CHECK(!retained);
        }
    }
    if (c) {
        q2_level_memory saved=c->level_memory;
        CHECK(!client_load_zone(c,c->map,9));
        CHECK(!memcmp(&saved,&c->level_memory,sizeof(saved)));
        close_client(c);
    }
}

static void generic_fallback(void)
{
    client *c=calloc(1,sizeof(*c));
    u8 data[32]={0};
    dat_chunk chunk={0};
    if (!c) exit(2);
    c->level_memory_checked=true;
    memcpy(data,"Zone0",6);
    q2_wr_u32(data+12,(u32)-3000);
    q2_wr_u32(data+28,64);
    chunk.data=data;chunk.size=sizeof(data);
    c->common.chunk[Q2_COMMON_RESOURCES]=&chunk;
    snprintf(c->zone.name,sizeof(c->zone.name),"QFRONT/Zone0");
    q2_world_render_init(&c->render);
    client_world_resources(c,false);
    CHECK(!c->level_memory_current && !c->level_memory_failures);
    CHECK(32u*c->render.packet_capacity==3008);
    free(c);
}

int main(int argc,char **argv)
{
    generic_fallback();
    if (argc==3 && !strcmp(argv[1],"--headless")) {
        disc_mission_table(argv[2]);
        disc_loads(argv[2],false);
        disc_loads(argv[2],true);
    }
    else if (argc!=1) return 2;
    printf("client memory: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
