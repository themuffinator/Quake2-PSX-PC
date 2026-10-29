#include "world.h"
#include <stdio.h>
#include <string.h>
#include "fixtures/world_resource_reference.h"
static int failures, checks;
#define CHECK(x) do { ++checks; if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);++failures;} } while(0)
static u8 keys[32];
static void resource_automatic(void)
{
    const s32 controls[]={0,33,4096,6600,0x7fffffff};
    q2_world_render r;
    q2_common_file common={0};
    dat_chunk chunk={0};
    u8 resource[44]={0};
    unsigned i,j;
    memcpy(resource,"Zone0",6);
    chunk.data=resource;chunk.size=sizeof(resource);
    common.chunk[Q2_COMMON_RESOURCES]=&chunk;
    q2_world_render_init(&r);
    for(i=0;i<sizeof(world_resource_automatic)/sizeof(world_resource_automatic[0]);++i) {
        const u32 *v=world_resource_automatic[i];
        q2_wr_u32(resource+28,v[0]);
        for(j=0;j<sizeof(controls)/sizeof(controls[0]);++j) {
            q2_wr_u32(resource+12,(u32)controls[j]);
            CHECK(q2_world_subdiv_resources_heap(&r,&common,"Zone0",v[1]));
            CHECK(32u*r.packet_capacity==v[2]);
            CHECK(r.subdiv_capacity==v[3]);
        }
    }
    /* A signed override remains independent of the supplied heap state. */
    q2_wr_u32(resource+12,(u32)-3000);
    CHECK(q2_world_subdiv_resources_heap(&r,&common,"Zone0",0));
    CHECK(32u*r.packet_capacity==3008);
    CHECK(q2_world_subdiv_resources_heap(&r,&common,"Zone0",1117872));
    CHECK(32u*r.packet_capacity==3008);
}
static void resource_overrides(void)
{
    q2_world_render r;
    q2_common_file common={0};
    dat_chunk chunk={0};
    u8 resource[44]={0};
    unsigned i;
    memcpy(resource,"Zone0",6);
    chunk.data=resource;chunk.size=sizeof(resource);
    common.chunk[Q2_COMMON_RESOURCES]=&chunk;
    q2_world_render_init(&r);
    for(i=0;i<sizeof(world_resource_reference)/sizeof(world_resource_reference[0]);++i) {
        const s32 *v=world_resource_reference[i];
        q2_wr_u32(resource+12,(u32)v[0]);
        q2_wr_u32(resource+28,(u32)v[1]);
        CHECK(q2_world_subdiv_resources(&r,&common,"QFRONT/zone0"));
        CHECK(32*r.packet_capacity==v[2]);
        CHECK(r.subdiv_capacity==v[3]);
        CHECK(q2_world_packet_request(&r,1,32)!=0);
    }
    /* A complete twelve-byte name shares the parser used by particle pools. */
    memcpy(resource,"ABCDEFGHIJKL",12);
    q2_wr_u32(resource+12,(u32)-3000);
    CHECK(q2_world_subdiv_resources(&r,&common,"ABCDEFGHIJKL"));
    CHECK(r.packet_capacity==94);
    CHECK(!q2_world_subdiv_resources(&r,&common,"ABCDEFGHIJKLM"));
    CHECK(r.packet_capacity==128 && r.subdiv_capacity==112);
    chunk.size=31;
    CHECK(!q2_world_subdiv_resources(&r,&common,"ABCDEFGHIJKL"));
    CHECK(!q2_world_subdiv_resources(NULL,&common,"ABCDEFGHIJKL"));
}
static void parents(void)
{
    q2_world_render r;
    int i;
    q2_world_render_init(&r);
    CHECK(r.packet_capacity==128 && r.packet_remaining==255);
    q2_world_packet_reset(&r,33);CHECK(r.packet_capacity==2);
    CHECK(q2_world_packet_request(&r,10,33));CHECK(r.packet_used==2);
    CHECK(!q2_world_packet_request(&r,20,1));CHECK(r.packet_remaining==254);
    CHECK(q2_world_packet_request(&r,10,33));CHECK(r.packet_used==2);
    r.subdiv_view=1;q2_world_render_begin_view(&r);
    CHECK(!q2_world_packet_request(&r,10,33));CHECK(r.packet_remaining==255);
    r.subdiv_view=0;q2_world_render_end_frame(&r);q2_world_render_end_view(&r);
    CHECK(r.packet_dirs_used==0 && r.packet_pending_count==2 && r.packet_used==2);
    CHECK(!q2_world_packet_request(&r,20,1)); /* blocks are still GPU-owned */
    q2_world_render_end_frame(&r);CHECK(r.packet_used==0);
    CHECK(q2_world_packet_request(&r,20,1));
    CHECK(r.packet_slot[0].block[0]==1); /* directory and blocks return LIFO */
    CHECK(q2_world_packet_request(&r,30,32));
    CHECK(r.packet_slot[1].block[0]==0);

    q2_world_packet_reset(&r,8192);
    r.packet_quota=1;q2_world_render_begin_view(&r);
    CHECK(!q2_world_packet_request(&r,0,64));CHECK(r.packet_remaining==1);
    CHECK(q2_world_packet_request(&r,1,63));CHECK(r.packet_remaining==0);
    CHECK(q2_world_packet_request(&r,1,63)); /* existing packet bypasses quota */
    CHECK(!q2_world_packet_request(&r,2,1));
    q2_world_render_begin_view(&r);CHECK(q2_world_packet_request(&r,2,1));
    r.subdiv_view=1;q2_world_render_begin_view(&r);
    CHECK(q2_world_packet_request(&r,1,63));CHECK(r.packet_used==5);
    q2_world_render_end_frame(&r);
    r.subdiv_view=0;q2_world_render_begin_view(&r);
    CHECK(q2_world_packet_request(&r,1,63));q2_world_render_end_view(&r);
    CHECK(r.packet_dirs_used==2 && r.packet_pending_count==1);
    r.subdiv_view=1;q2_world_render_end_view(&r);
    CHECK(r.packet_dirs_used==2); /* earlier view's node stamp protects both */
    q2_world_render_end_frame(&r);q2_world_render_end_view(&r);
    CHECK(r.packet_dirs_used==1 && r.packet_pending_count==2);

    q2_world_packet_reset(&r,8192);q2_world_render_begin_view(&r);
    for(i=0;i<255;++i)CHECK(q2_world_packet_request(&r,(u32)i,1));
    q2_world_render_begin_view(&r); /* spare block, but no directory */
    CHECK(!q2_world_packet_request(&r,255,1));CHECK(r.packet_used==255);
    CHECK(q2_world_packet_request(&r,0,1));
    q2_world_packet_reset(&r,-1);CHECK(r.packet_capacity==0);
    CHECK(!q2_world_packet_request(&r,0,1));
    CHECK(q2_world_packet_request(&r,0,0)); /* empty MapMod still gets a directory */
    q2_world_packet_reset(&r,0);CHECK(r.packet_capacity==128);
}
static q2_subdiv_result request(q2_world_render *r,int key,int depth,bool eye)
{
    u16 z[4]={100,(u16)(depth/2),100,(u16)(depth-depth/2)};
    if(eye)z[0]=0;
    return q2_world_subdiv_request(r,&keys[key],0,Q2_SURF_VARIANT_SUBDIVIDE,0,z);
}
int main(void)
{
    resource_automatic();
    q2_world_render r;
    q2_common_file common={0};
    dat_chunk chunk={0};
    u8 resource[44]={0};
    int i;
    parents();
    resource_overrides();
    q2_world_render_init(&r);CHECK(r.subdiv_capacity==112);
    /* Nonnegative records still use provisional legacy estimates. These are
     * lookup/reset checks, not evidence for native automatic capacities. */
    memcpy(resource,"Zone2",6);resource[28]=40;resource[12]=33;
    chunk.data=resource;chunk.size=sizeof(resource);
    common.chunk[Q2_COMMON_RESOURCES]=&chunk;
    CHECK(q2_world_subdiv_resources(&r,&common,"BASE1/ZONE2"));CHECK(r.subdiv_capacity==40);
    CHECK(r.packet_capacity==2);
    resource[28]=180;
    CHECK(q2_world_subdiv_resources(&r,&common,"zone2"));CHECK(r.subdiv_capacity==180);
    CHECK(!q2_world_subdiv_resources(&r,&common,"zone0"));CHECK(r.subdiv_capacity==112);
    CHECK(r.packet_capacity==128);

    q2_world_subdiv_reset(&r,10);r.subdiv_threshold=1000;
    for(i=0;i<4;++i)CHECK(request(&r,i,300,false)==Q2_SUBDIV_MESH);
    CHECK(request(&r,4,300,false)==Q2_SUBDIV_FLAT);CHECK(r.subdiv_used==4);
    CHECK(request(&r,4,251,false)==Q2_SUBDIV_FLAT);
    for(i=4;i<7;++i)CHECK(request(&r,i,250,false)==Q2_SUBDIV_MESH);
    CHECK(request(&r,7,200,false)==Q2_SUBDIV_FLAT);CHECK(r.subdiv_used==7);
    for(i=7;i<10;++i)CHECK(request(&r,i,200,true)==Q2_SUBDIV_MESH);
    CHECK(request(&r,10,200,true)==Q2_SUBDIV_DROP);
    CHECK(request(&r,10,200,false)==Q2_SUBDIV_FLAT);
    /* Repeated visits in one viewport reuse its mesh even under FULL pressure. */
    CHECK(request(&r,6,250,false)==Q2_SUBDIV_MESH);CHECK(r.subdiv_used==10);
    q2_world_render_end_frame(&r);
    CHECK(r.subdiv_used==10);CHECK(r.subdiv_pending_count==6);
    CHECK(r.subdiv_slot[0].poly==NULL);CHECK(r.subdiv_slot[5].poly==NULL);
    CHECK(r.subdiv_slot[6].poly==&keys[6]);
    CHECK(request(&r,10,200,true)==Q2_SUBDIV_DROP);
    q2_world_render_end_frame(&r);
    CHECK(r.subdiv_used==4);CHECK(r.subdiv_pending_count==0);
    CHECK(request(&r,10,250,false)==Q2_SUBDIV_MESH);
    CHECK(r.subdiv_slot[5].poly==&keys[10]); /* free stack is LIFO */

    /* A cached face that takes the flat path queues its release. A later run
     * in this view can reuse it until the frame's packet consumers are done. */
    CHECK(request(&r,10,1000,false)==Q2_SUBDIV_FLAT);
    CHECK(request(&r,10,250,false)==Q2_SUBDIV_MESH);
    CHECK(r.subdiv_pending_count==1);
    q2_world_render_end_frame(&r);CHECK(r.subdiv_used==4);

    q2_world_subdiv_reset(&r,6);r.subdiv_threshold=1000;
    for(i=0;i<6;++i)CHECK(request(&r,i,300,false)==Q2_SUBDIV_MESH);
    CHECK(request(&r,6,100,false)==Q2_SUBDIV_FLAT);
    q2_world_subdiv_reset(&r,-1);CHECK(r.subdiv_capacity==0);
    CHECK(request(&r,0,100,true)==Q2_SUBDIV_DROP);

    /* Viewports share the budget, not the packets: the same face needs two
     * meshes, each paired across the two GPU frame buffers. */
    q2_world_subdiv_reset(&r,16);r.subdiv_threshold=1000;
    CHECK(request(&r,0,300,false)==Q2_SUBDIV_MESH);
    r.subdiv_view=1;
    CHECK(request(&r,0,300,false)==Q2_SUBDIV_MESH);CHECK(r.subdiv_used==2);
    CHECK(request(&r,0,300,false)==Q2_SUBDIV_MESH);CHECK(r.subdiv_used==2);
    q2_world_render_end_frame(&r);
    /* An unseen node is released immediately for this view only. */
    r.subdiv_view=0;q2_world_render_end_view(&r);CHECK(r.subdiv_used==1);
    CHECK(r.subdiv_slot[1].view==1 && r.subdiv_slot[1].allocated);
    /* A node visited by an earlier view survives a later view's cleanup,
     * even when all its faces are culled and no mesh request is made. */
    q2_world_subdiv_touch_node(&r,0);
    r.subdiv_view=1;q2_world_render_end_view(&r);CHECK(r.subdiv_used==1);
    q2_world_render_end_frame(&r);
    q2_world_render_end_view(&r);CHECK(r.subdiv_used==0);
    CHECK(request(&r,1,300,false)==Q2_SUBDIV_MESH);
    CHECK(r.subdiv_slot[1].poly==&keys[1]);
    printf("Subdivision pool: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
