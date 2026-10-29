#include "levelmemory.h"
#include "effect.h"
#include "world.h"

#include <string.h>

bool q2_level_memory_init(q2_level_memory *m, const q2_exe *exe)
{
    u32 hi,lo,ram,stack,base,ceiling;
    if (!m || !exe || !q2_exe_has_layout(exe) ||
        !q2_exe_u32(exe,q2_exe_addr(exe,0x8006e8c0),&hi) ||
        !q2_exe_u32(exe,q2_exe_addr(exe,0x8006e8c4),&lo) ||
        !q2_exe_u32(exe,q2_exe_addr(exe,0x800ae600),&ram) ||
        !q2_exe_u32(exe,q2_exe_addr(exe,0x800ae604),&stack) ||
        (hi & 0xffff0000u)!=0x3c020000u ||
        (lo & 0xffff0000u)!=0x24420000u || ram!=0x200000 || stack>ram/2)
        return false;
    base=(hi<<16)+(u32)(s32)(s16)lo;
    ceiling=0x80000000u+ram-stack-(stack & ~3u);
    memset(m,0,sizeof(*m));
    return q2_native_heap_init(&m->heap,base,ceiling);
}

bool q2_level_memory_files_read(q2_level_memory_files *out,
    const q2_common_file *common, const q2_zone_file *zone, const q2_buf *media)
{
    q2_level_memory_files f;
    const dat_chunk *resources, *scene;
    u32 begin,end;
    if (!out || !common || !zone || !media || !media->data || media->size<16 ||
        common->ar.buf.size>0xffffffffu || zone->ar.buf.size>0xffffffffu ||
        media->size>0xffffffffu) return false;
    resources=dat_find(&common->ar,"Resources");
    scene=dat_find(&zone->ar,"Scene");
    if (!resources || !scene) return false;
    begin=q2_rd_u32(media->data);
    end=q2_rd_u32(media->data+4);
    if (begin>media->size-4 || end<begin+4 || end>media->size ||
        resources->offset>common->ar.buf.size || scene->offset>zone->ar.buf.size)
        return false;
    f.common_size=(u32)common->ar.buf.size;
    f.common_prefix=resources->offset;
    f.zone_size=(u32)zone->ar.buf.size;
    f.zone_prefix=scene->offset;
    f.media_size=(u32)media->size;
    f.media_sound_offset=end;
    f.media_header_size=begin;
    f.palette_bytes=2u*(media->data[begin+2]+media->data[begin+3]);
    *out=f;
    return true;
}

static bool release(q2_level_memory *m,u32 *pointer)
{
    if (!q2_native_heap_free(&m->heap,*pointer)) return false;
    *pointer=0;
    return true;
}

static u32 file_alloc(q2_level_memory *m,u32 size,bool high,u32 prefix)
{
    u32 p;
    if (size>0xfffff800u || prefix>size) return 0;
    p=q2_native_heap_alloc(&m->heap,(size+2047u)&~2047u,high);
    if (!p) return 0;
    p=q2_native_heap_shrink(&m->heap,p,size,high);
    if (!p) return 0;
    if (prefix) {
        q2_native_heap_move_header(&m->heap,p,p+prefix);
        p+=prefix;
    }
    return p;
}

static bool load(q2_level_memory *m,const q2_level_memory_files *f,
    const q2_zone_resources *r,unsigned unit,unsigned views,
    bool multiplayer,bool game_level,bool stream)
{
    static const u32 mission_sizes[]={0,44,208,48,192,280,4};
    u32 reserved, moved, groups, particle_bytes, blocks, parent_bytes;
    if (stream) {
        if (!m->loaded || views!=m->views || multiplayer!=m->multiplayer ||
            !release(m,&m->water) || !release(m,&m->zone) ||
            !release(m,&m->parents) || !release(m,&m->particles)) return false;
    } else {
        /* 0x8006E7D0: Water Moves survives this boundary. */
        if (!release(m,&m->common) || !release(m,&m->zone) ||
            !release(m,&m->palettes) || !release(m,&m->media) ||
            !release(m,&m->paths) || !release(m,&m->actors) ||
            !release(m,&m->origins) || !release(m,&m->speeds) ||
            !release(m,&m->parents) || !release(m,&m->particles) ||
            !release(m,&m->mission)) return false;
        if (unit && !(m->mission=q2_native_heap_alloc(&m->heap,mission_sizes[unit],true)))
            return false;
        m->mission_unit=unit;
        m->media=file_alloc(m,f->media_size,false,0);
        if (!m->media) return false;
        m->media=q2_native_heap_shrink(&m->heap,m->media,f->media_sound_offset,false);
        if (!m->media) return false;
        reserved=q2_native_heap_alloc(&m->heap,556,true);
        if (!reserved) return false;
        m->common=file_alloc(m,f->common_size,true,f->common_prefix);
        if (!m->common || !release(m,&reserved)) return false;
    }
    m->zone=file_alloc(m,f->zone_size,true,f->zone_prefix);
    if (!m->zone) return false;
    if (!stream) {
        /* Every decoded image is freed before the next allocation that
         * survives loading. Its payload leaves no heap header behind. */
        m->palettes=q2_native_heap_alloc(&m->heap,f->palette_bytes,true);
        if (!m->palettes) return false;
        m->media=q2_native_heap_shrink(&m->heap,m->media,f->media_header_size,true);
        if (!m->media) return false;
        moved=q2_native_heap_alloc(&m->heap,f->media_header_size,true);
        if (!moved) return false;
        if (moved>m->media) {
            if (!release(m,&m->media)) return false;
            m->media=moved;
        } else if (!release(m,&moved)) return false;
        /* 0x8007C938, after the new archives/UI have loaded. */
        if (!release(m,&m->water)) return false;
        if (game_level && !multiplayer) {
            m->actors=q2_native_heap_alloc(&m->heap,64*324+12*views,false);
            m->origins=q2_native_heap_alloc(&m->heap,12*views,false);
            m->speeds=q2_native_heap_alloc(&m->heap,4*views,false);
            if (!m->actors || !m->origins || !m->speeds) return false;
        }
        m->paths=q2_native_heap_alloc(&m->heap,game_level && !multiplayer ? 32*36 : 0,false);
        if (!m->paths) return false;
    }
    m->water=q2_native_heap_alloc(&m->heap,(multiplayer?48:24)*48,false);
    if (!m->water) return false;
    groups=r->particle_groups<0 ? 0 : r->particle_groups ? (u32)r->particle_groups : 32;
    particle_bytes=288*groups+80*q2_fx_budget(groups,views);
    if (particle_bytes && !(m->particles=q2_native_heap_alloc(&m->heap,particle_bytes,false)))
        return false;
    m->largest_payload=q2_native_heap_largest(&m->heap);
    blocks=(m->largest_payload-(1882u*(u32)r->subdiv_meshes+2u))/3330u;
    m->packets=r->packet_control<0 ? 0u-(u32)r->packet_control : 32u*(blocks>256?256:blocks);
    if (!m->packets) m->packets=Q2_WORLD_PACKET_DEFAULT;
    m->packets=(m->packets+31u)&~31u;
    m->meshes=r->subdiv_meshes<0 ? 0 : r->subdiv_meshes ? (u32)r->subdiv_meshes : Q2_WORLD_SUBDIV_DEFAULT;
    parent_bytes=104*m->packets+1882*m->meshes+2+2*(m->packets/32);
    m->parents=q2_native_heap_alloc(&m->heap,parent_bytes,false);
    if (!m->parents) return false;
    m->views=views;
    m->multiplayer=multiplayer;
    m->loaded=true;
    return true;
}

bool q2_level_memory_load(q2_level_memory *m,const q2_level_memory_files *f,
    const q2_zone_resources *r,unsigned unit,unsigned views,
    bool multiplayer,bool game_level,bool stream)
{
    q2_level_memory next;
    if (!m || !m->heap.count || !f || !r || unit>6 || !views || views>4 ||
        r->particle_groups>80 || r->subdiv_meshes>Q2_WORLD_SUBDIV_MAX ||
        (r->packet_control<0 && 0u-(u32)r->packet_control>8192u) ||
        f->media_sound_offset>f->media_size || f->media_header_size>f->media_sound_offset)
        return false;
    next=*m;
    if (!load(&next,f,r,unit,views,multiplayer,game_level,stream)) return false;
    *m=next;
    return true;
}
