#include "world.h"

#include "mover.h"
#include "rotator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* flare.h includes world.h, so it can only be reached from the .c side — which
 * is why q2_world_zone forward-declares the light world rather than including
 * it. */
#include "flare.h"
#include "sortdata.h"
#include "trig.h"
#include "vram.h"

/* ------------------------------------------------------------------------- */
/* SortData screen changes — 0x80065804 / 0x80065D08.                        */
/* ------------------------------------------------------------------------- */

#define Q2_SORT_SCREEN_RECORDS 24

typedef struct sort_screen_state {
    psx_ot_area_screen record[Q2_SORT_SCREEN_RECORDS];
    psx_ot_area_screen current;
    u32 count; /* highest allocated index; record 0 is the seeded viewport */
} sort_screen_state;

static s16 sort_sub16(s32 a, s32 b)
{
    return (s16)((u16)a - (u16)b);
}

static void sort_rect_empty(psx_ot_area_screen *r, s16 add_x, s16 add_y)
{
    memset(r, 0, sizeof(*r));
    r->add_x = add_x;
    r->add_y = add_y;
}

static bool sort_rect_same_state(const psx_ot_area_screen *a,
                                 const psx_ot_area_screen *b)
{
    return a->min_x == b->min_x && a->min_y == b->min_y &&
           a->max_x == b->max_x && a->max_y == b->max_y &&
           a->add_x == b->add_x && a->add_y == b->add_y;
}

static void sort_screen_begin(sort_screen_state *s,
                              s16 clip_w, s16 clip_h,
                              s16 add_x, s16 add_y)
{
    memset(s, 0, sizeof(*s));
    s->record[0].max_x = clip_w;
    s->record[0].max_y = clip_h;
    s->record[0].add_x = add_x;
    s->record[0].add_y = add_y;
    s->current = s->record[0];
}

static const psx_ot_area_screen *sort_screen_parent(
    const sort_screen_state *s, u32 index)
{
    if (index < Q2_SORT_SCREEN_RECORDS && index <= s->count)
        return &s->record[index];
    return &s->record[0];
}

/* SetDefDrawEnv stores both the clip origin and drawing offset at the same
 * relative coordinate.  This packet restores the PREVIOUS screen state while
 * the OT is walked in the reverse of the SortData construction order. */
static bool sort_screen_emit_restore(psx_ot *ot, u32 console_bucket,
                                     const psx_ot_area_screen *old)
{
    psx_prim *prim;

    prim = psx_ot_add_bucket(ot,
                             psx_ot_authored_bucket(ot, console_bucket));
    if (!prim)
        return false;
    prim->kind = PSX_PRIM_DRAW_ENV;
    prim->tpage = 0x060Au; /* SetDefDrawEnv at 0x80089DE4 */
    prim->xy[0].x = (s16)(old->min_x + old->add_x);
    prim->xy[0].y = (s16)(old->min_y + old->add_y);
    prim->xy[1].x = (s16)(old->max_x - old->min_x);
    prim->xy[1].y = (s16)(old->max_y - old->min_y);
    prim->xy[2] = prim->xy[0];
    return true;
}

static void sort_bounds_add(psx_ot_area_screen *r, s16 x, s16 y)
{
    if (x < r->min_x) r->min_x = x;
    if (x > r->max_x) r->max_x = x;
    if (y < r->min_y) r->min_y = y;
    if (y > r->max_y) r->max_y = y;
}

static void sort_bounds_init(psx_ot_area_screen *r)
{
    memset(r, 0, sizeof(*r));
    r->min_x = r->min_y = 1024;
    r->max_x = r->max_y = -1024;
}

static void sort_bounds_merge(psx_ot_area_screen *dst,
                              const psx_ot_area_screen *src)
{
    if (src->min_x < dst->min_x) dst->min_x = src->min_x;
    if (src->min_y < dst->min_y) dst->min_y = src->min_y;
    if (src->max_x > dst->max_x) dst->max_x = src->max_x;
    if (src->max_y > dst->max_y) dst->max_y = src->max_y;
}

/* 0x8006637C..0x800663DC clamps each packed coordinate independently.  The
 * low test reloads the parent's minimum and the high test reloads its maximum;
 * confusing the latter reload for the old minimum collapses every portal which
 * crosses the right or bottom edge to a zero-sized rectangle. */
static void sort_rect_clamp_to_parent(psx_ot_area_screen *r,
                                     const psx_ot_area_screen *parent)
{
    s16 *x[2] = { &r->min_x, &r->max_x };
    s16 *y[2] = { &r->min_y, &r->max_y };
    int i;

    for (i = 0; i < 2; i++) {
        if (*x[i] < parent->min_x)
            *x[i] = parent->min_x;
        else if (*x[i] > parent->max_x)
            *x[i] = parent->max_x;
        if (*y[i] < parent->min_y)
            *y[i] = parent->min_y;
        else if (*y[i] > parent->max_y)
            *y[i] = parent->max_y;
    }
}

/*
 * Opcode 1 is a SCREEN-REGION change, despite its historical ENTITY name in
 * sortdata.h.  Its return value still controls a variable-width bitstream arm,
 * but retail also retains the rectangle, changes SetGeomOffset for the wall run
 * which follows, and associates the new region with f4's area.
 */
static bool sort_entity_projects(const q2_world_zone *z, s32 index,
                                 u32 parent_index, u32 area,
                                 u32 console_bucket,
                                 const q2_camera *cam,
                                 int screen_w, int screen_h,
                                 psx_ot *ot, gte_state *gte,
                                 const gte_matrix *view,
                                 sort_screen_state *state,
                                 q2_world_stats *stats)
{
    const psx_ot_area_screen *parent = sort_screen_parent(state, parent_index);
    psx_ot_area_screen front, zero, out;
    /* `valid_node` is what guards every read of this, but the compiler does
     * not correlate a flag with the value it guards (MSVC C4701). */
    q2_scene_node node = { 0 };
    const q2_point_group *grp = NULL;
    bool all_zero = true, all_positive = true;
    bool valid_node;
    s32 centre_x, centre_y;
    u32 i;

    centre_x = (cam->ofs_x || cam->ofs_y) ? cam->ofs_x : screen_w / 2;
    centre_y = (cam->ofs_x || cam->ofs_y) ? cam->ofs_y : screen_h / 2;

    /* 0x80065DD0 resets projection to the viewport centre before it measures
     * the marker, irrespective of the region used by the preceding wall run. */
    gte_set_projection(gte, cam->projection, centre_x, centre_y);
    gte_set_rotation(gte, view);

    valid_node = z && index >= 0 && (u32)index < z->scene.node_count &&
                 (u32)index < z->points.group_count &&
                 q2_scene_get_node(&z->scene, (u32)index, &node);

    /* Hidden markers pass three packed zeroes to 0x80065804.  The old port
     * returned TRUE here; that selects the wrong SortData arm after
     * OBJDRAWOFF and corrupts every node reference which follows it. */
    if (!valid_node || q2_scene_flags_nodraw(node.flags) ||
        (z->node_hidden && (u32)index < z->node_hidden_count &&
         z->node_hidden[index])) {
        sort_rect_empty(&out, 0, 0);
    } else {
        s16 rel[3];

        grp = &z->points.groups[index];
        rel[0] = sort_sub16(node.origin[0], cam->pos[0]);
        rel[1] = sort_sub16(node.origin[1], cam->pos[1]);
        rel[2] = sort_sub16(node.origin[2], cam->pos[2]);
        gte->v[0].x = rel[0];
        gte->v[0].y = rel[1];
        gte->v[0].z = rel[2];
        gte_mvmva(gte, 1, 0, 0, 3, 0);
        gte_set_translation(gte, gte->mac[0], gte->mac[1], gte->mac[2]);

        sort_bounds_init(&front);
        sort_bounds_init(&zero);

        for (i = 0; i < grp->count; i++) {
            q2_point pt;
            psx_ot_area_screen *bounds;

            if (!q2_points_get(&z->points, grp->first + i, &pt))
                continue;
            gte->v[0].x = pt.x;
            gte->v[0].y = pt.y;
            gte->v[0].z = pt.z;
            gte_rtps(gte, false);

            if (gte->sz[3] != 0) {
                all_zero = false;
                bounds = &front;
            } else {
                all_positive = false;
                bounds = &zero;
            }
            sort_bounds_add(bounds, gte->sxy[2].x, gte->sxy[2].y);
        }

        if (all_zero) {
            s32 mid[3];

            /* A group wholly at/behind SZ=0 is accepted as its parent region
             * unless the transformed stored AABB centre is farther than 200
             * units behind the camera. */
            for (i = 0; i < 3; i++) {
                mid[i] = (node.bbox_min[i] + node.bbox_max[i]) / 2;
                gte->v[0].x = (i == 0) ? sort_sub16(mid[i], cam->pos[i])
                                       : gte->v[0].x;
                gte->v[0].y = (i == 1) ? sort_sub16(mid[i], cam->pos[i])
                                       : gte->v[0].y;
                gte->v[0].z = (i == 2) ? sort_sub16(mid[i], cam->pos[i])
                                       : gte->v[0].z;
            }
            gte_mvmva(gte, 1, 0, 0, 3, 0);
            if (gte->mac[2] < -200)
                sort_rect_empty(&out, 0, 0);
            else
                out = *parent;
        } else if (!all_positive &&
                   ((front.min_x > 0 && front.max_x < screen_w) ||
                    (front.min_y > 0 && front.max_y < screen_h))) {
            /* A mixed near-plane group whose positive-depth projection lies
             * strictly inside either screen axis expands to the parent. */
            out = *parent;
        } else {
            sort_bounds_merge(&front, &zero);
            out = front;
            out.add_x = parent->add_x;
            out.add_y = parent->add_y;
            sort_rect_clamp_to_parent(&out, parent);
        }

        /* 0x80065848 normalises every packed point rectangle to literal zero;
         * it does not retain a non-zero collapsed coordinate. */
        if (!q2_world_sort_region_visible(&out)) {
            s16 add_x = out.add_x, add_y = out.add_y;
            sort_rect_empty(&out, add_x, add_y);
        }
    }

    /* Record zero is seeded; every call increments first and writes record N.
     * Shipped streams stay below the 24-record retail allocation. */
    state->count++;
    if (state->count < Q2_SORT_SCREEN_RECORDS)
        state->record[state->count] = out;
    else if (state->count == Q2_SORT_SCREEN_RECORDS)
        Q2_WARN("SortData screen-change record pool exceeded retail's 24 entries");

    if (!sort_rect_same_state(&out, &state->current)) {
        if (!sort_screen_emit_restore(ot, console_bucket, &state->current) &&
            stats)
            stats->ot_overflow++;
    }
    state->current = out;

    /* The following Scene run is projected relative to the new region. */
    gte_set_projection(gte, cam->projection,
                       centre_x - out.min_x, centre_y - out.min_y);

    if (q2_world_sort_region_visible(&out))
        psx_ot_area_register_screen(ot, area & 0x7Fu,
                                    console_bucket, &out);
    return q2_world_sort_region_visible(&out);
}

q2_result q2_world_load_zone(q2_world_zone *out, const disc *d,
                             const char *map, int zone_index)
{
    char path[256];
    q2_buf buf;
    q2_result r;

    if (!out || !d || !map)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/ZONE%d.DAT", map, zone_index);
    snprintf(out->name, sizeof(out->name), "%s/ZONE%d", map, zone_index);

    r = disc_read_file(d, path, &buf);
    if (r != Q2_OK) {
        /*
         * A MISSING zone file is how a caller learns where the zones stop.
         * Nothing on the disc says how many a map has, so the client counts by
         * probing until one is absent — which made 21 of the disc's 49 maps
         * report an ERROR on a perfectly normal load, and buried the four maps
         * that had a real one. Absent is INFO; anything else is still an error,
         * because a zone that exists and will not read is a fault.
         */
        if (r == Q2_ERR_NOT_FOUND)
            Q2_INFO("no %s — the map's zones end here", path);
        else
            Q2_ERROR("cannot read %s: %s", path, q2_result_str(r));
        return r;
    }

    r = q2_zone_open(&out->zone, &buf);
    if (r != Q2_OK) {
        q2_buf_free(&buf);
        return r;
    }

    r = q2_scene_parse(&out->scene, &out->zone);
    if (r != Q2_OK) {
        q2_zone_close(&out->zone);
        return r;
    }

    r = q2_points_parse(&out->points, &out->zone);
    if (r != Q2_OK) {
        q2_zone_close(&out->zone);
        return r;
    }

    /* The three chunks are indexed in lockstep; if they disagree we have
     * misparsed one of them and must not proceed on guesswork. */
    if (out->points.group_count != out->scene.node_count) {
        Q2_ERROR("%s: %u point groups but %u scene nodes",
                 out->name, out->points.group_count, out->scene.node_count);
        q2_points_free(&out->points);
        q2_zone_close(&out->zone);
        return Q2_ERR_BAD_FORMAT;
    }

    return Q2_OK;
}

q2_result q2_world_move_zone(q2_world_zone *dst, q2_world_zone *src)
{
    q2_world_zone copy;
    q2_result r;

    if (!dst || !src)
        return Q2_ERR_INVALID_ARG;
    if (dst == src)
        return Q2_OK;

    /* Save the fields whose pointers either address the zone buffer directly
     * (Scene/Points) or are caller-owned runtime attachments. The zone file
     * itself must be moved separately so its chunk[] entries are re-resolved
     * against the DESTINATION archive's inline directory. See level.h. */
    copy = *src;

    q2_world_free_zone(dst);
    r = q2_zone_move(&dst->zone, &src->zone);
    if (r != Q2_OK) {
        q2_points_free(&copy.points);
        memset(src, 0, sizeof(*src));
        memset(dst, 0, sizeof(*dst));
        return r;
    }

    dst->scene             = copy.scene;
    dst->points            = copy.points;
    memcpy(dst->name, copy.name, sizeof(dst->name));
    dst->node_filter       = copy.node_filter;
    dst->node_filter_count = copy.node_filter_count;
    dst->movers            = copy.movers;
    dst->node_hidden       = copy.node_hidden;
    dst->node_hidden_count = copy.node_hidden_count;
    dst->sort              = copy.sort;
    dst->sort_offset       = copy.sort_offset;
    dst->sort_area         = copy.sort_area;
    dst->rotators          = copy.rotators;
    dst->lights            = copy.lights;
    dst->light_node        = copy.light_node;

    /* The groups allocation and the zone buffer now belong to dst. */
    memset(src, 0, sizeof(*src));
    return Q2_OK;
}

void q2_world_free_zone(q2_world_zone *z)
{
    if (!z)
        return;
    q2_points_free(&z->points);
    q2_zone_close(&z->zone);
    memset(z, 0, sizeof(*z));
}

void q2_camera_default(q2_camera *cam, int screen_w, int screen_h)
{
    (void)screen_h;

    if (!cam)
        return;

    memset(cam, 0, sizeof(*cam));

    /*
     * The projection distance sets the field of view, and this one is the
     * PORT'S, not the console's — see the header. A distance equal to the
     * buffer's width puts the horizontal half-angle at atan(1/2), about 53
     * degrees across, which frames an offline square-pixel render without
     * claiming to be a viewport.
     *
     * The geometry offset is left at (0,0), which q2_world_build_ot reads as
     * "the middle of the buffer you hand me".
     */
    cam->projection = (u16)screen_w;
    cam->far_z      = Q2_CAMERA_FAR_DEFAULT;
    cam->sort_range = Q2_CAMERA_SORT_RANGE;
}

void q2_world_bounds(const q2_world_zone *z, s32 min_out[3], s32 max_out[3])
{
    u32 n, i;
    s32 mn[3] = { INT32_MAX, INT32_MAX, INT32_MAX };
    s32 mx[3] = { INT32_MIN, INT32_MIN, INT32_MIN };
    bool any = false;

    if (!z)
        return;

    for (n = 0; n < z->scene.node_count; n++) {
        q2_scene_node node;
        const q2_point_group *grp;
        u32 k;

        if (!q2_scene_get_node(&z->scene, n, &node))
            continue;

        /* Scene node count and Points group count come from different headers
         * with nothing in the format tying them together. They agree on every
         * zone of the disc (17,035/17,035), but agreeing is not being
         * guaranteed, and this indexes one by the other. */
        if (n >= z->points.group_count)
            continue;

        grp = &z->points.groups[n];

        for (k = 0; k < grp->count; k++) {
            q2_point pt;
            s32 world[3];
            int c;

            if (!q2_points_get(&z->points, grp->first + k, &pt))
                continue;

            world[0] = pt.x + node.origin[0];
            world[1] = pt.y + node.origin[1];
            world[2] = pt.z + node.origin[2];

            for (c = 0; c < 3; c++) {
                if (world[c] < mn[c]) mn[c] = world[c];
                if (world[c] > mx[c]) mx[c] = world[c];
            }
            any = true;
        }
    }

    if (!any) {
        mn[0] = mn[1] = mn[2] = 0;
        mx[0] = mx[1] = mx[2] = 0;
    }

    for (i = 0; i < 3; i++) {
        if (min_out) min_out[i] = mn[i];
        if (max_out) max_out[i] = mx[i];
    }
}

void q2_world_render_init(q2_world_render *r)
{
    if (!r)
        return;

    memset(r, 0, sizeof(*r));
    q2_tpage_table_init(&r->tpage);
    r->subdiv_threshold = 0;
    r->pressure         = Q2_SURF_POOL_FREE;
    q2_world_subdiv_reset(r, Q2_WORLD_SUBDIV_DEFAULT);
    q2_world_packet_reset(r, Q2_WORLD_PACKET_DEFAULT);
}

void q2_world_packet_reset(q2_world_render *r, int packets)
{
    unsigned i;
    if (!r) return;
    if (!packets) packets=Q2_WORLD_PACKET_DEFAULT;
    if (packets<0) packets=0;
    if (packets>32*Q2_WORLD_PACKET_BLOCKS) packets=32*Q2_WORLD_PACKET_BLOCKS;
    r->packet_capacity=(u16)((packets+31)/32);
    r->packet_used=r->packet_pending_count=r->packet_dirs_used=0;
    r->packet_quota=r->packet_remaining=255;
    memset(r->packet_slot,0,sizeof(r->packet_slot));
    for (i=0;i<r->packet_capacity;++i) r->packet_free[i]=(u16)i;
    for (i=0;i<Q2_WORLD_PACKET_DIRS;++i) r->packet_dir_free[i]=(u8)i;
}

void q2_world_render_begin_view(q2_world_render *r)
{
    if (r) r->packet_remaining=r->packet_quota;
}

u16 q2_world_packet_request(q2_world_render *r, u32 node, unsigned polygons)
{
    unsigned i, blocks;
    q2_world_packet_slot *slot;
    if (!r) return 0;
    for (i=0;i<Q2_WORLD_PACKET_DIRS;++i) {
        slot=&r->packet_slot[i];
        if (slot->allocated && slot->node==node && slot->view==r->subdiv_view) {
            q2_world_subdiv_touch_node(r,node);
            return (u16)(i+1);
        }
    }
    /* 0x80067804 / 67B9C, then 68044: a failed construction restores its
     * directory immediately and leaves the per-walk quota unchanged. */
    if (!r->packet_remaining || r->packet_dirs_used==Q2_WORLD_PACKET_DIRS || polygons>=64)
        return 0;
    blocks=(polygons+31)/32;
    if (blocks>(unsigned)(r->packet_capacity-r->packet_used)) return 0;
    i=r->packet_dir_free[r->packet_dirs_used++];
    slot=&r->packet_slot[i];
    slot->allocated=true;
    slot->node=node;
    slot->view=r->subdiv_view;
    slot->blocks=(u8)blocks;
    for (i=0;i<blocks;++i) slot->block[i]=r->packet_free[r->packet_used++];
    --r->packet_remaining;
    q2_world_subdiv_touch_node(r,node);
    return (u16)(slot-r->packet_slot+1);
}

void q2_world_subdiv_reset(q2_world_render *r, int capacity)
{
    int i;
    if (!r) return;
    if (capacity == 0) capacity=Q2_WORLD_SUBDIV_DEFAULT;
    if (capacity < 0) capacity=0;
    if (capacity > Q2_WORLD_SUBDIV_MAX) capacity=Q2_WORLD_SUBDIV_MAX;
    r->subdiv_capacity=(u16)capacity;
    r->subdiv_used=r->subdiv_pending_count=0;
    r->subdiv_view=0;
    memset(r->subdiv_slot,0,sizeof(r->subdiv_slot));
    for (i=0;i<capacity;++i) r->subdiv_free[i]=(u16)i;
    r->pressure=capacity?Q2_SURF_POOL_FREE:Q2_SURF_POOL_FULL;
}

static bool subdiv_resources(q2_world_render *r, const q2_common_file *common,
                              const char *zone_name, bool have_heap, u32 largest)
{
    q2_zone_resources resources;
    u32 packets;
    if (!r) return false;
    q2_world_subdiv_reset(r,Q2_WORLD_SUBDIV_DEFAULT);
    q2_world_packet_reset(r,Q2_WORLD_PACKET_DEFAULT);
    if (!q2_common_resources(common,zone_name,&resources)) return false;
    /* 0x80070F24 / 0x80071518: a negative RESOURCE value forces its
     * magnitude; only a negative argument to the lower-level allocator
     * disables it. QFRONT -3000 and QFMV -1 therefore request 3008 and 32
     * packets after rounding. Negate unsigned to avoid signed overflow.
     *
     * The heap-aware entry ignores positive magnitudes. Its caller must supply
     * the largest payload after water and particles, before the parent pool.
     * The legacy entry is the fallback for unknown layouts or failed accounting. */
    packets=resources.packet_control<0 ? 0u-(u32)resources.packet_control
                                      : (u32)resources.packet_control;
    if (resources.packet_control>=0 && have_heap) {
        u32 blocks=(largest-(1882u*(u32)resources.subdiv_meshes+2u))/3330u;
        packets=32u*(blocks>256u ? 256u : blocks);
    }
    if (resources.subdiv_meshes>Q2_WORLD_SUBDIV_MAX ||
        packets>32u*Q2_WORLD_PACKET_BLOCKS) return false;
    q2_world_subdiv_reset(r,resources.subdiv_meshes);
    q2_world_packet_reset(r,(int)packets);
    return true;
}

bool q2_world_subdiv_resources(q2_world_render *r, const q2_common_file *common,
                              const char *zone_name)
{
    return subdiv_resources(r,common,zone_name,false,0);
}

bool q2_world_subdiv_resources_heap(q2_world_render *r, const q2_common_file *common,
                                   const char *zone_name, u32 largest_payload)
{
    return subdiv_resources(r,common,zone_name,true,largest_payload);
}

static void subdiv_retire(q2_world_render *r, unsigned i, bool detach)
{
    q2_subdiv_slot *slot=&r->subdiv_slot[i];
    if (!slot->pending) {
        slot->pending=true;
        r->subdiv_pending[r->subdiv_pending_count++]=(u16)i;
    }
    if (detach) slot->poly=NULL;
}

void q2_world_subdiv_touch_node(q2_world_render *r, u32 node)
{
    unsigned i;
    if (!r) return;
    /* node+10 is a FRAME stamp (0x80067A44 / 0x80067D14), shared by all
     * viewports, unlike the four node+4 packet-directory handles. */
    for (i=0;i<r->subdiv_capacity;++i)
        if (r->subdiv_slot[i].allocated && r->subdiv_slot[i].node==node)
            r->subdiv_slot[i].node_seen=true;
    for (i=0;i<Q2_WORLD_PACKET_DIRS;++i)
        if (r->packet_slot[i].allocated && r->packet_slot[i].node==node)
            r->packet_slot[i].node_seen=true;
}

void q2_world_render_end_view(q2_world_render *r)
{
    unsigned i;
    if (!r) return;
    /* 0x80067E8C..68010 releases this view's unseen node packets, calling
     * 0x80069730 for their meshes. Iterate node then face order, as retail
     * does, because the free stack is LIFO. Detached retirements have no
     * parent handle and stay unavailable until the frame boundary. */
    for (;;) {
        unsigned next=Q2_WORLD_SUBDIV_MAX;
        for (i=0;i<r->subdiv_capacity;++i) {
            const q2_subdiv_slot *s=&r->subdiv_slot[i];
            if (!s->poly || s->pending || s->node_seen || s->view!=r->subdiv_view)
                continue;
            if (next==Q2_WORLD_SUBDIV_MAX || s->node<r->subdiv_slot[next].node ||
                (s->node==r->subdiv_slot[next].node && s->poly<r->subdiv_slot[next].poly))
                next=i;
        }
        if (next==Q2_WORLD_SUBDIV_MAX) break;
        memset(&r->subdiv_slot[next],0,sizeof(r->subdiv_slot[next]));
        r->subdiv_free[--r->subdiv_used]=(u16)next;
    }
    for (;;) {
        unsigned next=Q2_WORLD_PACKET_DIRS, j;
        q2_world_packet_slot *slot;
        for (i=0;i<Q2_WORLD_PACKET_DIRS;++i) {
            const q2_world_packet_slot *s=&r->packet_slot[i];
            if (!s->allocated || s->node_seen || s->view!=r->subdiv_view) continue;
            if (next==Q2_WORLD_PACKET_DIRS || s->node<r->packet_slot[next].node)
                next=i;
        }
        if (next==Q2_WORLD_PACKET_DIRS) break;
        slot=&r->packet_slot[next];
        /* 0x8006D6F4 queues block returns; the node's directory ID is reusable
         * immediately, but another viewport cannot take the blocks this frame. */
        for (j=0;j<slot->blocks;++j)
            r->packet_pending[r->packet_pending_count++]=slot->block[j];
        memset(slot,0,sizeof(*slot));
        r->packet_dir_free[--r->packet_dirs_used]=(u8)next;
    }
}

void q2_world_render_end_frame(q2_world_render *r)
{
    unsigned i, reserve;
    if (!r) return;
    /* 0x80069250: release the retirement list, then clear it. */
    for (i=0;i<r->subdiv_pending_count;++i) {
        unsigned index=r->subdiv_pending[i];
        memset(&r->subdiv_slot[index],0,sizeof(r->subdiv_slot[index]));
        r->subdiv_free[--r->subdiv_used]=(u16)index;
    }
    r->subdiv_pending_count=0;
    reserve=r->subdiv_capacity<6?r->subdiv_capacity:6;
    /* 0x800692C8..693BC: farthest LAST requested depth, first slot wins ties.
     * Retired slots remain unavailable until the next call (GPU still owns
     * their packets). All viewports share the capacity and free stack, but
     * own distinct meshes (node+4+viewport at 0x8006780C). */
    for (i=r->subdiv_capacity-r->subdiv_used;i<reserve;++i) {
        unsigned j, farthest=Q2_WORLD_SUBDIV_MAX;
        s32 depth=0;
        for (j=0;j<r->subdiv_capacity;++j)
            if (r->subdiv_slot[j].poly && r->subdiv_slot[j].depth>depth) {
                farthest=j;depth=r->subdiv_slot[j].depth;
            }
        if (farthest==Q2_WORLD_SUBDIV_MAX) break;
        subdiv_retire(r,farthest,true);
    }
    for (i=0;i<r->subdiv_capacity;++i) r->subdiv_slot[i].node_seen=false;
    /* Main calls 0x8006D73C AFTER mesh cleanup, once all views are built. */
    for (i=0;i<r->packet_pending_count;++i)
        r->packet_free[--r->packet_used]=r->packet_pending[i];
    r->packet_pending_count=0;
    for (i=0;i<Q2_WORLD_PACKET_DIRS;++i) r->packet_slot[i].node_seen=false;
}

q2_subdiv_result q2_world_subdiv_request(q2_world_render *r,const u8 *poly,
    u32 node,q2_surf_variant variant,u16 clut,const u16 depth[4])
{
    unsigned i, index=Q2_WORLD_SUBDIV_MAX;
    s32 sum=(s32)depth[1]+depth[3];
    unsigned free_count=r->subdiv_capacity-r->subdiv_used;
    bool near_eye=depth[0]==0||depth[1]==0||depth[2]==0||depth[3]==0;
    for (i=0;i<r->subdiv_capacity;++i)
        if (r->subdiv_slot[i].poly==poly && r->subdiv_slot[i].node==node &&
            r->subdiv_slot[i].view==r->subdiv_view) { index=i;break; }
    q2_world_subdiv_touch_node(r,node);
    if (r->subdiv_threshold<=0 || !q2_surf_should_subdivide(variant,clut,sum,
            r->subdiv_threshold,Q2_SURF_POOL_FREE)) {
        /* Flat-link path 0x800AF9C8 queues a cached handle for release. */
        if (index!=Q2_WORLD_SUBDIV_MAX) subdiv_retire(r,index,false);
        return Q2_SUBDIV_FLAT;
    }
    if (index==Q2_WORLD_SUBDIV_MAX) {
        bool accepted;
        r->pressure=free_count==0?Q2_SURF_POOL_FULL:
            r->subdiv_capacity<7||free_count>6?Q2_SURF_POOL_FREE:
            free_count>3?Q2_SURF_POOL_TIGHT:Q2_SURF_POOL_FULL;
        accepted=q2_surf_should_subdivide(variant,clut,sum,r->subdiv_threshold,r->pressure);
        /* 0x800698F8: any SZ=0 can consume the final three reserved slots.
         * If even those are spent, return success WITHOUT linking a flat
         * near-plane smear (0x80069928..9944). */
        if (!accepted && near_eye) {
            if (!free_count) return Q2_SUBDIV_DROP;
            accepted=true;
        }
        if (!accepted) return Q2_SUBDIV_FLAT;
        index=r->subdiv_free[r->subdiv_used++];
        r->subdiv_slot[index].poly=poly;
        r->subdiv_slot[index].node=node;
        r->subdiv_slot[index].view=r->subdiv_view;
        r->subdiv_slot[index].allocated=true;
        r->subdiv_slot[index].node_seen=true;
    }
    r->subdiv_slot[index].depth=sum;
    return Q2_SUBDIV_MESH;
}

/* ------------------------------------------------------------------------- */
/* One drawable quad, after transform and before it becomes a primitive.      */
/*                                                                            */
/* Split out because subdivision emits sixteen of these from one MapMod        */
/* polygon and every one of them has to go through the same colour, UV, CLUT   */
/* and blend rules as the original.                                            */
/* ------------------------------------------------------------------------- */
/*
 * Does this point's slot list name corner `corner` of polygon `poly`?
 *
 * ProcessMobileLights walks point+6..+9, breaks out of the whole slot loop on
 * the first 0xFF (0x80074A84 `beq v0, t6`) and stops after four (0x80074B74
 * `slti v0, t1, 4`). Each byte decodes through the halfword table InitLights
 * fills at 0x800E1018 (0x80074134..0x80074174):
 *
 *     52 * ((b >> 2) & 31) + 4 + 12 * (b & 3)
 *
 * with bit 7 of `b` choosing the second 32-polygon packet block (0x80074A8C
 * tests the sign of `b << 24`). Since the block adds 32 to the polygon index,
 * that is simply polygon `b >> 2`, corner `b & 3` — which is the reverse index
 * points.h already documents. A polygon past 63 has no encoding and is never
 * painted, and a point used by more than four corners paints only its first
 * four; both are consequences of the storage, not rules laid on top of it.
 *
 * The port keeps six slot bytes because the record has room for six; only the
 * first four are read here, because only the first four are read there.
 */
static bool point_paints_corner(const q2_point *pt, u32 poly, u32 corner)
{
    u32 s;

    if (poly > 63u)
        return false;

    for (s = 0; s < 4; s++) {
        u8 b = pt->slot[s];

        if (b == 0xFFu)
            return false;
        if ((u32)(b >> 2) == poly && (u32)(b & 3u) == corner)
            return true;
    }

    return false;
}

typedef struct world_quad {
    gte_sxy screen[4];
    u16     depth[4];
    psx_rgb rgb[4];
    psx_uv  uv[4];
    bool    textured;
    u16     tpage;
} world_quad;

static psx_prim *emit_quad(psx_ot *ot,
                           const q2_camera *cam,
                           q2_world_render *render,
                           const world_quad *q,
                           const q2_mapmod_poly *poly,
                           s32 forced_bucket,
                           s32 batch,
                           q2_world_stats *stats)
{
    psx_prim *prim;
    u32 otz;
    s32 far;
    int i;

    otz = ((u32)q->depth[0] + q->depth[1] + q->depth[2] + q->depth[3]) / 4;

    if (stats) {
        if (stats->quads_emitted == 0 || otz < stats->depth_min)
            stats->depth_min = otz;
        if (otz > stats->depth_max)
            stats->depth_max = otz;
    }

    (void)render;
    far  = cam->sort_range > 0 ? cam->sort_range : Q2_CAMERA_SORT_RANGE;

    /*
     * A forced bucket is the authored one: the whole node goes into the bucket
     * SortData named, and the quad's own depth is not consulted at all. That is
     * the console's behaviour — see sortdata.h — and the depth mapping below is
     * the port's stand-in for when no order is supplied.
     */
    if (batch >= 0) {
        /* Bit-14 is a Standard (bounds-aware) private chain. It must stay
         * atomic until the regional dependency graph is drained. */
        prim = psx_ot_batch_add(ot, batch);
    } else if (forced_bucket >= 0) {
        /*
         * A CONSOLE bucket, so it is scaled on the way in like every other
         * console constant that names a bucket outright — q2_screen_view_otz,
         * q2_screen_overlay_otz and the flash and water buckets in screen.c are
         * the others, and psx_ot_add_bucket's own comment says the scaling
         * belongs at each of those doors rather than at its. This one was
         * missed: SortData's 0..45 went in as REAL bucket indices, so the whole
         * authored world was packed into the far ninth of a 408-bucket slice
         * while every entity sharing that slice spread across all of it.
         */
        otz = psx_ot_authored_bucket(ot, (u32)forced_bucket);
        prim = psx_ot_add_bucket(ot, otz);
    } else {
        /*
         * ONE MAPPING for everything that shares the slice. The world used to
         * scale depth itself while models, effects and entity draws went
         * through q2_ot_bucket_for_depth, so the two disagreed about which
         * bucket a given distance meant — and a monster and the wall behind it
         * were ordered by two different rules. They are the same rule now, and
         * it is the one that also holds the slice's structural buckets back.
         */
        /*
         * The bucket is a depth SLAB about 100 units wide, and inside one slab
         * the plain add falls back on insertion order — which, for the world,
         * is node index. A light, a shootable button or any other detail
         * surface sits a few units off the wall it is mounted on, so the two
         * share a slab and are ordered by something that has nothing to do with
         * depth; and because the slab boundary DOES follow depth, walking
         * towards such a surface makes the two rules disagree and the surface
         * swap in and out. Handing the quad's own depth over as the key keeps
         * the order inside a slab continuous with the order between slabs, so
         * there is no boundary left to swap across. See psx_ot_add_depth.
         *
         * The key is the same average `otz` the bucket comes from, not the
         * nearest or farthest corner. Measured over a dozen zones a farthest-
         * corner key scores about 2% better on mis-ordered pixels, and it is
         * still the wrong choice: a key that ranks quads differently from the
         * bucket reintroduces exactly the discontinuity this removes.
         */
        prim = psx_ot_add_depth(ot, (u16)q2_ot_bucket_for_depth(ot, otz, far),
                                otz);
    }
    if (!prim) {
        if (stats) stats->ot_overflow++;
        return NULL;
    }

    prim->kind = q->textured ? PSX_PRIM_GT4 : PSX_PRIM_G4;

    for (i = 0; i < 4; i++) {
        prim->xy[i].x = q->screen[i].x;
        prim->xy[i].y = q->screen[i].y;
        prim->rgb[i]  = q->rgb[i];
        prim->uv[i]   = q->uv[i];
    }

    /* The parent's material was resolved before face culling, at allocation.
     * Flat and subdivided packets both inherit that stored page. */
    prim->clut = q2_vram_clut_word(q2_mapmod_clut_index(poly->clut));
    prim->tpage = q->tpage;
    prim->semi_transparent = q2_mapmod_clut_semi(poly->clut) != 0;
    if (stats && prim->semi_transparent) stats->quads_semi++;
    prim->textured_blend = true;

    if (stats) stats->quads_emitted++;
    return prim;
}

/* PAL 0x800AEDD4 constructs XYZ/UV and 0x800B0884 updates RGB with the
 * same midpoint graph. Each add is followed by an arithmetic shift, including
 * at the intermediate points. In particular the centre is midpoint(1,3),
 * not the bilinear average of four corners. Keep the graph shared by all
 * eight components so geometry, texture and lighting stay on the same mesh. */
static void subdiv_midpoint(s32 out[8], const s32 a[8], const s32 b[8])
{
    int i;
    for (i = 0; i < 8; i++) out[i] = (a[i] + b[i]) >> 1;
}

static void subdiv_grid(s32 g[5][5][8], const q2_point pt[4],
                        const world_quad *q)
{
    static const int x[4] = {0,4,4,0}, y[4] = {0,0,4,4};
    int i;
    for (i = 0; i < 4; i++) {
        s32 *v = g[y[i]][x[i]];
        v[0] = pt[i].x; v[1] = pt[i].y; v[2] = pt[i].z;
        v[3] = q->uv[i].u; v[4] = q->uv[i].v;
        v[5] = q->rgb[i].r; v[6] = q->rgb[i].g; v[7] = q->rgb[i].b;
    }
    subdiv_midpoint(g[2][4], g[0][4], g[4][4]);
    subdiv_midpoint(g[2][2], g[0][4], g[4][0]);
    subdiv_midpoint(g[2][3], g[2][4], g[2][2]);
    subdiv_midpoint(g[1][4], g[0][4], g[2][4]);
    subdiv_midpoint(g[3][4], g[4][4], g[2][4]);
    subdiv_midpoint(g[4][2], g[4][4], g[4][0]);
    subdiv_midpoint(g[4][3], g[4][4], g[4][2]);
    subdiv_midpoint(g[3][3], g[4][3], g[2][3]);
    subdiv_midpoint(g[3][2], g[2][2], g[4][2]);
    subdiv_midpoint(g[4][1], g[4][0], g[4][2]);
    subdiv_midpoint(g[1][3], g[0][4], g[2][2]);
    subdiv_midpoint(g[3][1], g[4][0], g[2][2]);
    subdiv_midpoint(g[2][0], g[4][0], g[0][0]);
    subdiv_midpoint(g[3][0], g[4][0], g[2][0]);
    subdiv_midpoint(g[2][1], g[2][2], g[2][0]);
    subdiv_midpoint(g[1][0], g[0][0], g[2][0]);
    subdiv_midpoint(g[0][2], g[0][4], g[0][0]);
    subdiv_midpoint(g[0][1], g[0][0], g[0][2]);
    subdiv_midpoint(g[1][2], g[0][2], g[2][2]);
    subdiv_midpoint(g[1][1], g[1][2], g[1][0]);
    subdiv_midpoint(g[0][3], g[0][4], g[0][2]);
}

/*
 * Replace one quad with the original's 4x4 mesh.
 *
 * 0x800B007C projects a 5x5 grid of vertices — 21 new ones, the four corners
 * arriving already transformed — and writes them into sixteen POLY_GT4 packets
 * at 52-byte stride. Build the rounded midpoint grid in object space, then
 * project each point. The original loads plain SVECTORs and runs RTPT.
 *
 * The point of the exercise is the affine texture warp. UVs interpolate linearly
 * in screen space with no perspective divide, so the error over a quad grows
 * with how much perspective it spans; splitting a near quad sixteen ways cuts it
 * by the same factor. UV and Gouraud values follow that same midpoint graph.
 *
 * Cells touching SZ=0 are omitted; other cells still draw. Returns the number
 * emitted, which can be zero when the entire mesh is behind the eye.
 */
static u32 emit_subdivided(psx_ot *ot,
                           gte_state *gte,
                           const q2_camera *cam,
                           q2_world_render *render,
                           const q2_point pt[4],
                           const world_quad *flat,
                           const q2_mapmod_poly *poly,
                           s32 forced_bucket,
                           s32 batch,
                           q2_world_stats *stats)
{
    enum { N = Q2_SURF_SUBDIV_STEPS, V = Q2_SURF_SUBDIV_STEPS + 1 };

    gte_sxy grid_xy[V][V];
    u16     grid_z[V][V];
    s32     grid[V][V][8];
    /* The alternate retail path rejects SZ=0, not divide overflow. */
    bool    grid_ok[V][V];
    u32     emitted = 0;
    int     gx, gy, i;

    for (gy = 0; gy < V; gy++)
        for (gx = 0; gx < V; gx++)
            grid_ok[gy][gx] = true;

    subdiv_grid(grid, pt, flat);
    for (gy = 0; gy < V; gy++) {
        for (gx = 0; gx < V; gx++) {
            const s32 *v = grid[gy][gx];

            /* Reuse the corners the caller already projected, exactly as the
             * original does — it is handed pointers to them rather than
             * re-transforming. */
            if ((gx == 0 || gx == N) && (gy == 0 || gy == N)) {
                int corner = (gy == 0) ? (gx == 0 ? 0 : 1)
                                       : (gx == 0 ? 3 : 2);

                /*
                 * SZ == 0 — at or behind the eye — is the console's predicate,
                 * not "the divide overflowed". 0x800B00AC..0x800B00C8 tests the
                 * four corners for exactly that, and the alternate path's mask
                 * builder at 0x800B0694..0x800B07C4 does the same per grid
                 * point. Rejecting on the overflow instead hollowed out every
                 * cell within 80 units of the eye, which is a great deal more
                 * of a surface than the console ever removes.
                 */
                if (flat->depth[corner] == 0) {
                    grid_ok[gy][gx] = false;
                    continue;
                }

                grid_xy[gy][gx] = flat->screen[corner];
                grid_z[gy][gx]  = flat->depth[corner];
                continue;
            }

            gte->v[0].x = (s16)v[0];
            gte->v[0].y = (s16)v[1];
            gte->v[0].z = (s16)v[2];
            gte_rtps(gte, false);

            /* Same predicate as the reused corners above: SZ == 0. */
            if (gte->sz[3] == 0) {
                grid_ok[gy][gx] = false;
                continue;
            }

            grid_xy[gy][gx] = gte->sxy[2];
            grid_z[gy][gx]  = gte->sz[3];
        }
    }

    for (gy = 0; gy < N; gy++) {
        for (gx = 0; gx < N; gx++) {
            world_quad sub = *flat;
            static const int dx[4] = { 0, 1, 1, 0 };
            static const int dy[4] = { 0, 0, 1, 1 };

            /* A cell is drawable only if all four of its own corners are. */
            if (!grid_ok[gy][gx]     || !grid_ok[gy][gx + 1] ||
                !grid_ok[gy + 1][gx] || !grid_ok[gy + 1][gx + 1])
                continue;

            for (i = 0; i < 4; i++) {
                int cx = gx + dx[i];
                int cy = gy + dy[i];
                const s32 *v = grid[cy][cx];

                sub.screen[i] = grid_xy[cy][cx];
                sub.depth[i]  = grid_z[cy][cx];

                sub.uv[i].u = (u8)v[3]; sub.uv[i].v = (u8)v[4];
                sub.rgb[i].r = (u8)v[5]; sub.rgb[i].g = (u8)v[6]; sub.rgb[i].b = (u8)v[7];
            }

            if (emit_quad(ot, cam, render, &sub, poly, forced_bucket,
                          batch, stats))
                emitted++;
        }
    }

    return emitted;
}

/* ------------------------------------------------------------------------- */
u32 q2_world_build_ot(const q2_world_zone *z,
                      const q2_camera *cam,
                      int screen_w, int screen_h,
                      psx_ot *ot,
                      gte_state *gte,
                      q2_world_render *render,
                      q2_world_stats *stats)
{
    gte_matrix rot, spin;
    q2_world_render local;
    q2_sort_reader order;
    /* Same: `have_screen_state` guards it. Only `.current` is read under
     * that flag, so zeroing the cursor is enough -- the record array is
     * written by sort_screen_begin, and it is far too big to zero on every
     * call for a warning. */
    sort_screen_state screen_state;
    bool have_order = false;
    bool have_screen_state = false;
    u32 next_node = 0;
    u32 emitted = 0;
    u32 n;

    memset(&screen_state.current, 0, sizeof(screen_state.current));
    screen_state.count = 0;

    if (!z || !cam || !ot || !gte)
        return 0;

    if (!render) {
        q2_world_render_init(&local);
        render = &local;
    }
    q2_world_render_begin_view(render);

    if (stats)
        memset(stats, 0, sizeof(*stats));

    /*
     * An installed window means a caller owns the frame — the screen module has
     * already cleared the table and is filling one viewport's slice, so
     * clearing here would throw away the viewport drawn before this one. A
     * caller with no window keeps the old "hand me a table, I will manage it"
     * contract the offline tools use.
     */
    if (ot->window_len == 0)
        psx_ot_clear(ot);

    /* Area records are per viewport. In split screen the primitive table must
     * retain earlier views while this routing map must not. */
    psx_ot_area_clear(ot);

    gte_init(gte);
    /*
     * The projection is the VIEWPORT's, not the buffer's. A caller driving this
     * through q2_screen has already had `SetGeomOffset(view+266, view+268)` and
     * `SetGeomScreen(view+262)` installed for it by q2_screen_view_begin
     * (0x80076B78 / 0x80076B90); reloading them here from the buffer's own
     * middle would agree by luck for the layouts whose viewport is the whole
     * frame and be wrong for the three splits, whose centres are not the
     * framebuffer's. So the camera carries the centre and the buffer is only the
     * fallback — see q2_camera.ofs_x.
     */
    gte_set_projection(gte, cam->projection,
                       (cam->ofs_x || cam->ofs_y) ? cam->ofs_x : screen_w / 2,
                       (cam->ofs_x || cam->ofs_y) ? cam->ofs_y : screen_h / 2);

    q2_rotation_view_anamorphic(rot.m, cam->yaw, cam->pitch, cam->roll);
    gte_set_rotation(gte, &rot);

    /*
     * `InitGeom` at 0x8008E4C4 leaves ZSF3 = 341 and ZSF4 = 256, and nothing in
     * the image overrides them, so the GTE's average-Z is a quarter of the mean
     * vertex depth. Those are the values the port runs with; the mapping from a
     * depth to a bucket is done below against the viewport's far distance
     * instead, because a quarter of a room-scale depth saturates a 51-entry
     * slice several times over.
     */
    gte->zsf3 = GTE_ZSF3_INIT;
    gte->zsf4 = GTE_ZSF4_INIT;

    /*
     * Two walks over the same body. Without a SortData stream the nodes go in
     * index order and each quad picks its own bucket from its depth; with one,
     * the stream dictates both the order and the bucket. See sortdata.h for why
     * those are genuinely different renderings rather than two granularities of
     * the same one.
     */
    if (z->sort && z->sort->data) {
        have_order = q2_sort_begin(&order, z->sort, z->sort_offset,
                                   Q2_SORT_BUCKET_START);
        if (have_order) {
            s32 centre_x = (cam->ofs_x || cam->ofs_y)
                          ? cam->ofs_x : screen_w / 2;
            s32 centre_y = (cam->ofs_x || cam->ofs_y)
                          ? cam->ofs_y : screen_h / 2;
            s16 clip_w = (s16)(cam->clip_w > 0 ? cam->clip_w : screen_w);
            s16 clip_h = (s16)(cam->clip_h > 0 ? cam->clip_h : screen_h);
            s16 add_x = (s16)(screen_w - clip_w);
            s16 add_y = (s16)(screen_h - clip_h);

            sort_screen_begin(&screen_state, clip_w, clip_h, add_x, add_y);
            have_screen_state = true;
            psx_ot_area_prepare(ot, (s16)screen_w, (s16)screen_h,
                                add_x, add_y,
                                (s16)centre_x, (s16)centre_y);
            psx_ot_area_register_screen(ot, z->sort_area & 0x7Fu,
                                        Q2_SORT_BUCKET_SEED,
                                        &screen_state.record[0]);
            if (stats) stats->sort_areas_registered++;
        }
    }

    for (;;) {
        q2_scene_node node;
        q2_mapmod_rec rec;
        const q2_point_group *grp;
        q2_surf_variant variant;
        q2_world_packet_slot *parent=NULL;
        s16 spin_angles[3], spin_pivot[3];
        bool spin_active = false;
        s32 forced_bucket = -1;
        s32 batch = PSX_OT_BATCH_INVALID;
        s32 deferred_area = -1;
        s32 mover_shift[3] = { 0, 0, 0 };
        s32 active_clip_w = cam->clip_w > 0 ? cam->clip_w : screen_w;
        s32 active_clip_h = cam->clip_h > 0 ? cam->clip_h : screen_h;
        u32 p;
        s32 translation[3];
        /*
         * The mobile lights that reach THIS node — ProcessMobileLights'
         * surviving (light, brush) pairs, hoisted out of the polygon loop the
         * way 0x80074628..0x800746E4 hoists the ramp out of the point loop.
         */
        q2_light_ramp mob[Q2_DYNLIGHT_MAX];
        s32           mob_local[Q2_DYNLIGHT_MAX][3];
        u32           mob_count = 0;

        if (have_order) {
            q2_sort_item it;

            if (!q2_sort_next(&order, &it))
                break;

            if (it.kind == Q2_SORT_ENTITY) {
                bool visible = sort_entity_projects(
                    z, it.f1, it.f3 & 0xFFu, it.f4 & 0xFFu, it.bucket,
                    cam, screen_w, screen_h, ot, gte, &rot,
                    &screen_state, stats);

                if (stats) {
                    stats->sort_entities++;
                    if (visible) stats->sort_entities_visible++;
                    else         stats->sort_entities_degenerate++;
                    if (visible && (it.f4 & 0x7Fu) < PSX_OT_AREA_RETAIL_COUNT)
                        stats->sort_areas_registered++;
                }
                q2_sort_entity_resolve(&order, visible);
                continue;
            }

            n = it.node;
            if (n >= z->scene.node_count)
                continue;
            forced_bucket = (s32)it.bucket;
        } else {
            n = next_node++;
            if (n >= z->scene.node_count)
                break;
        }

        if (z->node_filter && n < z->node_filter_count && !z->node_filter[n])
            continue;

        if (!q2_scene_get_node(&z->scene, n, &node))
            continue;

        /*
         * The two node-level gates, both from the zone draw at 0x80067714.
         *
         * Bit 15 is a hide flag: `andi 0x8000` and straight on to the next node.
         * It is clear on every node on the disc — OBJDRAWOFF sets it at runtime.
         *
         * Draw variant 3 is the same outcome reached from the other direction:
         * the emitter at 0x80066740 dispatches on bits 10-11 and case 3 links
         * nothing. That is how a script hides a surface group with SETWIBBLE.
         */
        if (q2_scene_flags_nodraw(node.flags)) {
            if (stats) stats->nodes_hidden++;
            continue;
        }

        /* The same bit, set at run time rather than on the chunk — see
         * `node_hidden`. A script that has hidden this node hides it here. */
        if (z->node_hidden && n < z->node_hidden_count && z->node_hidden[n]) {
            if (stats) stats->nodes_hidden++;
            continue;
        }

        variant = q2_scene_flags_variant(node.flags);

        /* Parent packets exist before emitter dispatch (including variant 3
         * and deferred nodes whose area has no current screen record). */
        if (!q2_scene_get_mapmod(&z->scene, n, &rec)) {
            if (stats) stats->quads_rejected_bad++;
            continue;
        }
        if (have_order || !q2_mapmod_rec_is_sealing(&rec)) {
            u16 handle=q2_world_packet_request(render,n,rec.num_polys);
            if (!handle) {
                if (stats) stats->nodes_packet_denied++;
                continue;
            }
            parent=&render->packet_slot[handle-1];
            if (!parent->prepared) {
                /* 0x800682C8..68324 runs for EVERY face at construction, before
                 * culling. Later table promotions cannot rewrite an older
                 * transparent parent packet, nor its subdivision children. */
                for (p=0;p<rec.num_polys;++p) {
                    q2_mapmod_poly material;
                    if (q2_mapmod_get_poly(&rec,p,&material)) {
                        u32 page=material.tpage&31u;
                        parent->tpage[p]=q2_mapmod_clut_semi(material.clut)
                            ? q2_tpage_world_semi(&render->tpage,page)
                            : q2_tpage_world_opaque(&render->tpage,page);
                    }
                }
                parent->prepared=true;
            }
        }
        if (variant == Q2_SURF_VARIANT_HIDDEN) {
            if (stats) stats->nodes_hidden++;
            continue;
        }

        /*
         * Bit 14 puts the node on the deferred path (0x80066524): one depth for
         * the whole node from its projected origin, registered against a table
         * keyed on the area byte at +0x0E. Its private packet chain is retained
         * through the other regional emitters and joined at the viewport's
         * 0x80046E14-equivalent final drain.
         */
        if (q2_scene_flags_deferred(node.flags)) {
            u32 resolved;

            if (stats)
                stats->nodes_deferred++;

            /* A stale area is not drained by 0x80046E14. Rendering it in the
             * node's inline bucket is not a conservative fallback: it makes a
             * wall feature from a non-visible portal paint over this one. */
            if (have_order) {
                if (!psx_ot_area_bucket(ot, node.area & 0x7Fu, &resolved)) {
                    if (stats) stats->nodes_deferred_culled++;
                    continue;
                }
                deferred_area = (s32)(node.area & 0x7Fu);
            }
        }

        /*
         * The third node-level gate, and the only one that is not a flag.
         *
         * A node whose every polygon binds CLUT index 0 is sealing geometry —
         * the flat planes the build tool hangs across doorways and openings.
         * Index 0 is a reserved all-0x8000 palette, so drawing one paints
         * opaque black over the room behind it. No SortData stream on the disc
         * names one; see q2_mapmod_rec_is_sealing for the measurement.
         *
         * Only applied on the index-order walk. When a stream IS supplied it
         * has already made this choice, and following it is more faithful than
         * second-guessing it — on this disc the two agree, and if some other
         * build's stream ever named such a node, the stream would be right.
         */
        if (!have_order && q2_mapmod_rec_is_sealing(&rec)) {
            if (stats) stats->nodes_sealing++;
            continue;
        }

        /* See the matching check in q2_world_bounds: these two counts come from
         * separate headers and nothing in the format ties them together. */
        if (n >= z->points.group_count) {
            if (stats) stats->quads_rejected_bad += rec.num_polys;
            continue;
        }

        grp = &z->points.groups[n];

        /* The four world linkers read the packed CURRENT region extent at
         * 0x800B2E90 (0x800AFA8C / AFCEC / AFF50 / B0108), not the viewport's
         * original clip.  The GTE coordinates have already been made local by
         * `centre - region.min`, so testing them against the full viewport lets
         * portal-exterior polygons into a private run and defeats the DRAWENV
         * boundary which is supposed to make that run atomic.
         */
        if (have_screen_state) {
            active_clip_w = screen_state.current.max_x
                          - screen_state.current.min_x;
            active_clip_h = screen_state.current.max_y
                          - screen_state.current.min_y;
        }
        if (stats) {
            stats->nodes_visited++;
            stats->quads_total += rec.num_polys;
        }

        /*
         * The node's origin folds into the GTE translation, so each vertex stays
         * a plain s16 exactly as it is stored.
         *
         * A mover's displacement is added here rather than to the geometry,
         * because that is where the original adds it: the zone draw reads the
         * runtime object's s16 triple at +0x12 and offsets the node's
         * camera-space position by it. Nothing in the Scene chunk moves.
         *
         * Rotation arrives the same way and completes the transform
         * (0x800678B4 - 0x8006793C):
         *
         *     node position = origin - camera - (R . p) + p + d
         *
         * where R is RotMatrix of the object's three Euler angles at +0x0C, p is
         * the pivot at +0x18 relative to the node's origin, and d is the linear
         * displacement at +0x12. The `- R.p + p` is a rotation about p, so the
         * node's own vertices have to go through R as well — which is why the
         * rotation is composed into the GTE matrix below rather than being
         * folded into the translation on its own.
         */
        {
            s32 shift[3] = { 0, 0, 0 };
            s32 native_shift[3];
            int axis;

            if (z->movers) {
                q2_movers_node_offset(z->movers, n, mover_shift);
                shift[0] = mover_shift[0];
                shift[1] = mover_shift[1];
                shift[2] = mover_shift[2];
            }
            q2_rotators_node_offset(z->rotators, n, native_shift);
            for (axis = 0; axis < 3; axis++) {
                mover_shift[axis] += native_shift[axis];
                shift[axis] += native_shift[axis];
            }

            spin_active = z->rotators &&
                          q2_rotators_node_transform(z->rotators, n,
                                                     spin_angles, spin_pivot);

            if (spin_active) {
                s32 rp[3];
                int c;

                q2_rotation_euler(spin.m, spin_angles[0], spin_angles[1],
                                  spin_angles[2]);

                /* R . p at 1.3.12, exactly as 0x8006FB18 computes it: sum the
                 * three products and shift once at the end. */
                for (c = 0; c < 3; c++) {
                    s64 s = (s64)spin.m[c][0] * spin_pivot[0]
                          + (s64)spin.m[c][1] * spin_pivot[1]
                          + (s64)spin.m[c][2] * spin_pivot[2];
                    rp[c] = (s32)(s >> Q2_FRAC_12);
                }

                for (c = 0; c < 3; c++)
                    shift[c] += spin_pivot[c] - rp[c];
            }

            translation[0] = node.origin[0] + shift[0] - cam->pos[0];
            translation[1] = node.origin[1] + shift[1] - cam->pos[1];
            translation[2] = node.origin[2] + shift[2] - cam->pos[2];
        }

        /*
         * A rotating node draws through camera_rot . R, so its own geometry
         * turns; a still one keeps the camera matrix untouched. Setting this per
         * node is what the original does too — RotMatrix is called inside the
         * node loop, not once per frame.
         */
        if (spin_active) {
            gte_matrix composed;
            int r_, c_;

            for (r_ = 0; r_ < 3; r_++) {
                for (c_ = 0; c_ < 3; c_++) {
                    s32 s = (s32)rot.m[r_][0] * spin.m[0][c_]
                          + (s32)rot.m[r_][1] * spin.m[1][c_]
                          + (s32)rot.m[r_][2] * spin.m[2][c_];
                    composed.m[r_][c_] = (s16)(s >> Q2_FRAC_12);
                }
            }
            gte_set_rotation(gte, &composed);
        } else {
            gte_set_rotation(gte, &rot);
        }

        /* Translation is applied after rotation by the GTE, so it must be
         * expressed in camera space. Note it uses the CAMERA matrix, not the
         * composed one: the node's position is not rotated by its own spin, only
         * its vertices are. */
        {
            s64 tx = (s64)rot.m[0][0] * translation[0]
                   + (s64)rot.m[0][1] * translation[1]
                   + (s64)rot.m[0][2] * translation[2];
            s64 ty = (s64)rot.m[1][0] * translation[0]
                   + (s64)rot.m[1][1] * translation[1]
                   + (s64)rot.m[1][2] * translation[2];
            s64 tz = (s64)rot.m[2][0] * translation[0]
                   + (s64)rot.m[2][1] * translation[1]
                   + (s64)rot.m[2][2] * translation[2];

            gte_set_translation(gte,
                                (s32)(tx >> Q2_FRAC_12),
                                (s32)(ty >> Q2_FRAC_12),
                                (s32)(tz >> Q2_FRAC_12));
        }

        /*
         * ---------------------------------------------------------------
         * ProcessMobileLights, 0x80074598 — which lights reach this node
         * ---------------------------------------------------------------
         * NOTHING IN THE WORLD EVER GOT BRIGHTER in this port. Sixteen
         * dynamic lights were being appended every frame by projectiles,
         * muzzle flashes and script primitives, gathered per ENTITY by
         * q2_light_gather, and then dropped: the level's own geometry was
         * drawn from its baked Gouraud bytes and nothing else, so a rocket
         * flying down a corridor lit the rocket and left the corridor black.
         *
         * The console does not light the world through the GTE at all. It
         * walks the brushes DrawScene registered this frame (52-byte records
         * at 0x800DA578, count at 0x800B2C80), rejects the ones the light's
         * box misses, and POKES the Gouraud bytes of the packets it has
         * already emitted — undoing them again on the next frame through
         * UndoMobileLights (0x80075F60), whose per-draw-buffer undo list at
         * 0x800E1458 exists precisely because the packets persist.
         *
         * This port rebuilds its quads every frame, so it needs neither the
         * undo list nor the packet-poking: the ramp is applied where the
         * baked colour is copied into the quad, a few lines below. The
         * subdivision path then inherits it for free, which is what
         * UpdateSubdivisionLighting (0x800695E0) does by hand at 0x80075BC4.
         *
         * THE LIGHT IS MADE LOCAL BY SUBTRACTING object+0x28/+0x2C/+0x30
         * (0x80074940..0x80074980) and nothing else — in particular NOT the
         * runtime displacement at object+0x12 that the node's draw position
         * carries. So a mobile light falling on a moving door is computed
         * against the door's authored position; `mover_shift` is deliberately
         * absent here and present in the draw translation above.
         */
        if (z->lights && z->lights->dynamic_world_count) {
            u32 li;

            for (li = 0; li < z->lights->dynamic_world_count &&
                         mob_count < Q2_DYNLIGHT_MAX; li++) {
                const q2_light *l = &z->lights->dynamic_world[li];
                s32 lp[3];
                q2_light_ramp r;

                lp[0] = l->x;
                lp[1] = l->y;
                lp[2] = l->z;

                q2_light_ramp_begin(&r, l);

                /*
                 * `obj_flags` is the dword at object+0x0C, whose 0x00800000
                 * opts an object out entirely (0x800747D4). A Scene node in
                 * this port has no runtime object and therefore no such word,
                 * so zero is passed: nothing opts out, which is what every
                 * static piece of level geometry does on the console too.
                 */
                if (!q2_light_ramp_reaches(&r, lp, node.bbox_min,
                                           node.bbox_max, 0))
                    continue;

                mob[mob_count] = r;
                mob_local[mob_count][0] = lp[0] - node.origin[0];
                mob_local[mob_count][1] = lp[1] - node.origin[1];
                mob_local[mob_count][2] = lp[2] - node.origin[2];
                mob_count++;
                if (stats) stats->mobile_lights++;
            }
        }

        if (deferred_area >= 0) {
            s32 bounds_min[3], bounds_max[3];
            int axis;

            /* 0x80066560 zeroes a local SVECTOR and RTPS projects it once for
             * the 20-byte deferred batch record. Every quad in the node uses
             * this one signed +4 value; per-quad depth would be a different
             * sorter. +8 points at the node AABB with only the mover's linear
             * +0x12 displacement added (0x80066614..0x8006669C). */
            gte->v[0].x = 0;
            gte->v[0].y = 0;
            gte->v[0].z = 0;
            gte_rtps(gte, false);
            for (axis = 0; axis < 3; axis++) {
                bounds_min[axis] = node.bbox_min[axis] + mover_shift[axis];
                bounds_max[axis] = node.bbox_max[axis] + mover_shift[axis];
            }
            batch = psx_ot_batch_begin_box(
                        ot, (u32)deferred_area, false,
                        (s16)(gte->sz[3] ? gte->sz[3] : 1u),
                        bounds_min, bounds_max, cam->pos);
        }

        for (p = 0; p < rec.num_polys; p++) {
            q2_mapmod_poly poly;
            q2_point pt[4];
            psx_prim *prim;
            bool ok = true;
            int i;

            if (!q2_mapmod_get_poly(&rec, p, &poly)) {
                if (stats) stats->quads_rejected_bad++;
                continue;
            }

            for (i = 0; i < 4; i++) {
                if (poly.vtx[i] >= grp->count ||
                    !q2_points_get(&z->points, grp->first + poly.vtx[i], &pt[i])) {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                if (stats) stats->quads_rejected_bad++;
                continue;
            }

            /* Transform the quad as two GTE triangles: RTPT handles three
             * vertices, so the fourth goes through on a second pass. */
            gte->v[0].x = pt[0].x; gte->v[0].y = pt[0].y; gte->v[0].z = pt[0].z;
            gte->v[1].x = pt[1].x; gte->v[1].y = pt[1].y; gte->v[1].z = pt[1].z;
            gte->v[2].x = pt[2].x; gte->v[2].y = pt[2].y; gte->v[2].z = pt[2].z;
            gte_rtpt(gte);

            {
                world_quad q;
                gte_sxy screen[4];
                u16 depth[4];

                q.tpage = parent->tpage[p];
                screen[0] = gte->sxy[0];
                screen[1] = gte->sxy[1];
                screen[2] = gte->sxy[2];
                depth[0]  = gte->sz[1];
                depth[1]  = gte->sz[2];
                depth[2]  = gte->sz[3];

                /*
                 * NO BLANKET NEAR REJECTION — this is what was eating the
                 * geometry at the edge of the viewport.
                 *
                 * Two `if (gte->flag & GTE_FLAG_DIV_OVERFLOW) continue;` blocks
                 * used to sit here, one after the RTPT and one after the RTPS,
                 * on the claim that "the original rejected these outright". The
                 * divide overflows for any corner nearer than H/2 — 80 units at
                 * the one-player viewport's projection distance of 160 — and
                 * that is not "behind the camera", it is *close*. A wall or a
                 * floor the player is standing next to has one corner inside 80
                 * units and the WHOLE quad went, so 1898 of 7364 quads were
                 * being dropped in an ordinary standing view.
                 *
                 * And near geometry projects to the frame's BORDER, which is why
                 * the holes always appeared at the outskirts of the viewport
                 * rather than in the middle of it.
                 *
                 * The original's near-plane treatment is not rejection, it is
                 * SUBDIVISION (0x800AF7CC and 0x800AFA2C, and see the emit
                 * below): a near quad is replaced by a 4x4 mesh whose pieces
                 * each project sanely — and when the mesh is declined, the flat
                 * quad is linked anyway at 0x800AF9BC. Only draw variant 1
                 * rejects anything, and that test is its own, is address-backed,
                 * and is still applied a few lines down.
                 */
                gte->v[0].x = pt[3].x; gte->v[0].y = pt[3].y; gte->v[0].z = pt[3].z;
                gte_rtps(gte, false);

                screen[3] = gte->sxy[2];
                depth[3]  = gte->sz[3];

                /*
                 * AND THAT IS THE WHOLE OF THE NEAR-PLANE STORY. Nothing is
                 * asked here about how close a corner is, because none of the
                 * three linkers asks it.
                 *
                 * A depth-based rejection stood in this spot until this pass:
                 * "any corner's divide overflowed, and nothing is going to
                 * subdivide this quad, so drop it". `gte_divide` overflows once
                 * the projection distance reaches twice the depth, so at the
                 * one-player viewport's H of 160 it fired for any corner inside
                 * 80 units — and it took the WHOLE quad with it. Draw variant 1
                 * never subdivides at all, so for a variant-1 node the escape
                 * clause could never fire and the rejection was unconditional.
                 *
                 * It was modelling a test that is not in the executable. The
                 * provenance is in world.h beside
                 * q2_world_quad_linker_rejects(); the short version is that the
                 * depth sum at 0x800AF874 is compared once, at 0x800AF974, and
                 * only to decide whether to CALL the subdivision hook — and when
                 * that hook declines and returns 0 the flat quad is linked
                 * regardless at 0x800AF9BC. The smear the old test feared is
                 * handled where the console handles it: the GTE saturates the
                 * clamped corner to +/-1024 and the GPU refuses any polygon
                 * spanning more than 1023x511, which src/render/raster.c already
                 * implements. This block was written before that rule existed
                 * and had been double-counting it ever since.
                 *
                 * WHAT IT ACTUALLY COST, measured rather than assumed, because
                 * the counter overstates it badly. On the BASE1 demo walk `near`
                 * ran 6..43 quads a frame and `flat` stayed at 0 for all 400
                 * frames — but every single one of those 10,886 quads ALSO fails
                 * the 2D bounds test a few lines down, so removing the rejection
                 * changes that walk by exactly zero pixels. The quads it really
                 * lost are rarer: sweeping 26 maps x 4 zones x 200 demo frames
                 * finds 46 of them, and only POWER2 zone 0 has one the GPU's
                 * span rule would then have drawn. That one is worth the whole
                 * change — riding the lift at frames 43..46, a ceiling slab with
                 * all four corners at SZ 0 projects to a clean 239x162 rectangle
                 * and 15,581 pixels, 12% of the frame, were simply absent.
                 */

                /*
                 * The three rejections the console does have, in one place. The
                 * rule and its provenance are in world.h; what belongs here is
                 * what each was costing.
                 *
                 * Bounds: this port had no 2D test at all, and its absence is
                 * what made the invented near rejection look necessary.
                 *
                 * Backface: levels are sealed by brushes whose outward faces are
                 * never meant to be seen, and drawn without a cull those faces
                 * land in front of the rooms they enclose and black them out. It
                 * reads as a visibility fault — as if whole areas were failing
                 * to stream in — when the geometry behind them was being
                 * transformed and emitted correctly all along. It runs
                 * unconditionally, including for a quad with a saturated corner:
                 * the console feeds NCLIP exactly the same +/-1024-saturated
                 * SXY, because the vertex array it reads is what the transform
                 * at 0x800AED30 wrote and the GTE saturates before storing.
                 */
                switch (q2_world_quad_linker_rejects(gte, variant,
                                                     screen, depth,
                                                     active_clip_w,
                                                     active_clip_h)) {
                case Q2_WORLD_QUAD_REJECT_BOUNDS:
                    if (stats) stats->quads_rejected_bounds++;
                    continue;
                case Q2_WORLD_QUAD_REJECT_FLAT:
                    if (stats) stats->quads_rejected_flat++;
                    continue;
                case Q2_WORLD_QUAD_REJECT_BACK:
                    if (stats) stats->quads_rejected_back++;
                    continue;
                case Q2_WORLD_QUAD_DRAW:
                    break;
                }

                for (i = 0; i < 4; i++) {
                    q.screen[i] = screen[i];
                    q.depth[i]  = depth[i];
                }
                q.textured = true;

                /* Per-corner Gouraud colour, indexed through the record's RGB
                 * table. A missing table means flat white rather than a crash —
                 * five nodes on the disc legitimately have no polygons. */
                for (i = 0; i < 4; i++) {
                    u32 ci = poly.col[i];
                    if (rec.rgb && ci < rec.rgb_count) {
                        q.rgb[i].r = rec.rgb[ci * 3 + 0];
                        q.rgb[i].g = rec.rgb[ci * 3 + 1];
                        q.rgb[i].b = rec.rgb[ci * 3 + 2];
                    } else {
                        q.rgb[i].r = q.rgb[i].g = q.rgb[i].b = 128;
                    }
                    q.rgb[i].pad = 0;
                }

                /*
                 * ...and then the mobile lights repaint it, corner by corner.
                 *
                 * WHICH CORNERS A POINT MAY PAINT IS NOT "ALL OF THEM". The
                 * console's inner loop is per POINT: it takes the four slot
                 * bytes at point+6..+9, stops at the first 0xFF (0x80074A84),
                 * and never looks past the fourth (0x80074B74 `slti 4`). Each
                 * slot decodes through the table InitLights builds at
                 * 0x800E1018 into "corner (b & 3) of POLY_GT4 (b >> 2)", so a
                 * vertex shared by five quad corners lights four of them and a
                 * node's sixty-fifth polygon can be named by no slot at all.
                 * Painting every corner instead would be brighter than the
                 * console in exactly the places geometry is densest.
                 *
                 * The packet corner is not the vertex index: the world
                 * renderer's own colour writes (0x80068118..0x800681D8)
                 * establish that packet corner 2 is vertex 3 and corner 3 is
                 * vertex 2, which is the Z order libgpu wants — the same
                 * shuffle the UV block below undoes. The disc agrees: over a
                 * 900-frame BASE1 run this permutation leaves `mobile_unslotted`
                 * at 0, and the straight-through { 0, 1, 2, 3 } leaves 1824
                 * corners the slot list refuses to name.
                 *
                 * `q2_light_ramp_point` hands back SIGNED components on
                 * purpose (lighting.h): `den` is truncated down, so the last
                 * shell inside the outer radius comes out slightly negative and
                 * the outermost ring of a mobile light very faintly DARKENS the
                 * world. Rounding that to a byte first would turn -3 into +253
                 * and paint the ring white.
                 */
                if (mob_count) {
                    static const u32 packet_corner[4] = { 0, 1, 3, 2 };
                    u32 li;

                    for (i = 0; i < 4; i++) {
                        u8 rgb[3];
                        bool any = false;

                        if (!point_paints_corner(&pt[i], p, packet_corner[i])) {
                            if (stats) stats->mobile_unslotted++;
                            continue;
                        }

                        rgb[0] = q.rgb[i].r;
                        rgb[1] = q.rgb[i].g;
                        rgb[2] = q.rgb[i].b;

                        for (li = 0; li < mob_count; li++) {
                            s16 lpt[3];
                            s32 contrib[3];

                            lpt[0] = pt[i].x;
                            lpt[1] = pt[i].y;
                            lpt[2] = pt[i].z;

                            if (!q2_light_ramp_point(&mob[li], lpt,
                                                     mob_local[li], contrib))
                                continue;

                            q2_light_ramp_add(rgb, contrib);
                            any = true;
                            if (stats) stats->mobile_corners++;
                        }

                        if (any) {
                            q.rgb[i].r = rgb[0];
                            q.rgb[i].g = rgb[1];
                            q.rgb[i].b = rgb[2];
                        }
                    }
                }

                /*
                 * UV corners are ROTATED AND REVERSED against the vertices, by
                 * an amount the polygon carries in the top two bits of its UV
                 * index byte:
                 *
                 *     vertex j takes uv[(3 - f - j) & 3],  f = uvIdxFlags >> 6
                 *
                 * This is transcribed from the world renderer at 0x80068118 -
                 * 0x800681D8, which writes the four packet corners from
                 * uv[(3-f)&3], uv[(2-f)&3], uv[(0-f)&3] and uv[(1-f)&3] in
                 * POLY_GT4 order, and whose colour writes in the same loop
                 * establish that packet corner 2 is vertex 3 and corner 3 is
                 * vertex 2. Undoing that Z-order shuffle leaves the rule above.
                 *
                 * Two earlier readings are dead. Straight-through (uv[i] to
                 * vertex i) is a mirror of the truth, and the "UV table is in
                 * libgpu Z order" reading was tested by rendering both ways and
                 * rejected on the evidence. Neither could have been settled
                 * from geometry alone: on a rectangular UV rect a reflection
                 * still tiles cleanly, and only text and asymmetric decals show
                 * it. f is non-zero on 11.7% of polygons.
                 *
                 * The else is not dead code even though the lookup currently
                 * succeeds for all 274,936 polygons on the disc: prim comes from
                 * a zeroed pool, so a silent failure would collapse the quad
                 * onto texel (0,0) and read as a flat blob rather than as a
                 * fault. Mirror what the RGB path above does and be explicit.
                 */
                if (rec.uv && poly.uv_idx < rec.uv_count) {
                    const u8 *uv = rec.uv + (size_t)poly.uv_idx * 8;
                    for (i = 0; i < 4; i++) {
                        u32 c = (u32)((3 - poly.flags - i) & 3);
                        q.uv[i].u = uv[c * 2 + 0];
                        q.uv[i].v = uv[c * 2 + 1];
                    }
                } else {
                    q.textured = false;   /* Gouraud, untextured. */
                    q.uv[0].u = q.uv[0].v = 0;
                    q.uv[1] = q.uv[2] = q.uv[3] = q.uv[0];
                    if (stats) stats->quads_no_uv++;
                }

                /*
                 * Subdivision — the affine texture-warp control, selected per
                 * node by SETWIBBLE and gated per polygon on variant 2. The
                 * depth the original tests is the sum of two opposite corners
                 * (0x800AF874), not the average of four, so use exactly that.
                 *
                 * The mesh may cross the eye plane. Retail rejects individual
                 * cells touching SZ=0, retaining the other cells even when the
                 * original quad has a behind-eye corner. No flat fallback is
                 * emitted when the mesh contributes no drawable cells.
                 */
                q2_subdiv_result decision=q2_world_subdiv_request(render,
                    rec.polys+(size_t)p*12,n,variant,poly.clut,depth);
                if (decision==Q2_SUBDIV_DROP) {
                    if (stats) stats->quads_rejected_near++;
                    continue;
                }
                if (decision==Q2_SUBDIV_MESH) {
                    u32 sub = emit_subdivided(ot, gte, cam, render,
                                              pt, &q, &poly, forced_bucket,
                                              batch, stats);
                    if (sub) {
                        emitted += sub;
                        if (stats) stats->quads_subdivided++;
                    } else if (stats) {
                        stats->quads_rejected_near++;
                    }
                    continue;
                }

                prim = emit_quad(ot, cam, render, &q, &poly, forced_bucket,
                                 batch, stats);
                if (prim)
                    emitted++;
            }
        }
    }

    q2_world_render_end_view(render);
    if (have_screen_state) {
        psx_ot_area_screen full = screen_state.record[0];
        s32 centre_x = (cam->ofs_x || cam->ofs_y)
                       ? cam->ofs_x : screen_w / 2;
        s32 centre_y = (cam->ofs_x || cam->ofs_y)
                       ? cam->ofs_y : screen_h / 2;

        /* The final call uses view+274/+276, not the shake-reduced initial
         * record copied from the draw globals. */
        full.min_x = full.min_y = 0;
        full.max_x = (s16)screen_w;
        full.max_y = (s16)screen_h;

        /* 0x80067DFC..0x80067E68 makes one final 0x80065804 call with the
         * full viewport, bucket 1 and area -1.  This is not the viewport's
         * structural DRAWENV being rewritten: that full env is linked later,
         * so it executes first in bucket 1, then this packet restores the last
         * portal region for the low-numbered run which follows the final
         * marker.  Folding the two into one override puts area-2/private
         * chains on the wrong side of the state change.
         */
        if (!sort_rect_same_state(&full, &screen_state.current) &&
            !sort_screen_emit_restore(ot, 1, &screen_state.current) && stats)
            stats->ot_overflow++;
        screen_state.current = full;
        gte_set_projection(gte, cam->projection, centre_x, centre_y);
    }

    /*
     * The flares, last, because they are additive and the original draws them
     * after the world for the same reason: 0x800759F0 is its own per-viewport
     * pass, run once the geometry is in the table, and it links every flare into
     * a single entry rather than sorting them. §17.3.
     */
    if (z->lights) {
        q2_flare_view view;

        memset(&view, 0, sizeof(view));

        /*
         * The centre is view+266/+268 — the SAME geometry offset the projection
         * above was given, not a bare half-extent. On a split screen the two
         * part company, and an element positioned against the wrong one slides
         * along its own centre-to-light line by the difference.
         */
        view.centre[0] = (s16)((cam->ofs_x || cam->ofs_y) ? cam->ofs_x
                                                          : screen_w / 2);
        view.centre[1] = (s16)((cam->ofs_x || cam->ofs_y) ? cam->ofs_y
                                                          : screen_h / 2);
        /* The 2D EXTENT, not the size — see q2_camera. Paired sentinel, so a
         * layout with a legitimately-zero component on one axis does not fall
         * back on that axis alone. */
        view.extent[0] = (s16)((cam->ext_w || cam->ext_h) ? cam->ext_w
                                                          : screen_w);
        view.extent[1] = (s16)((cam->ext_w || cam->ext_h) ? cam->ext_h
                                                          : screen_h);

        /* Flares are a viewport pass, not a portal-region run. */
        gte_set_projection(gte, cam->projection,
                           (cam->ofs_x || cam->ofs_y) ? cam->ofs_x
                                                     : screen_w / 2,
                           (cam->ofs_x || cam->ofs_y) ? cam->ofs_y
                                                     : screen_h / 2);

        /*
         * One bucket for all of them, as the original has. Nearest in the
         * viewport's slice, so a flare paints over the geometry it is a
         * reflection of rather than being occluded by it — which is what a lens
         * flare is.
         */
        {
            u32 base = ot->window_len ? ot->window_base : 0;
            u32 span = ot->window_len ? ot->window_len  : ot->bucket_count;
            view.bucket = (u16)(base + (span ? span - 1 : 0));
        }

        /* The rotation matrix the flare pass needs is the one the node loop has
         * been leaving behind; restore the view rotation on its own so a MVMVA
         * against it is the camera transform and nothing else. */
        gte_set_rotation(gte, &rot);
        gte_set_translation(gte, 0, 0, 0);

        {
            q2_flare_stats fs;

            memset(&fs, 0, sizeof(fs));
            emitted += q2_flare_draw_all(z->lights, cam, z->light_node, &view,
                                         ot, gte, &fs);

            if (stats) {
                stats->flare_lights += fs.lights_considered;
                stats->flare_styled += fs.lights_styled;
                stats->flare_near   += fs.rejected_near;
                stats->flare_dark   += fs.rejected_dark;
                stats->flare_drawn  += fs.flares_drawn;
                stats->flare_prims  += fs.prims_emitted;
                stats->ot_overflow  += fs.ot_overflow;
            }
        }
    }

    /* How much of the slice the sort actually reached. One bucket means no
     * sorting happened at all, however many quads went in. */
    if (stats) {
        u32 base = ot->window_len ? ot->window_base : 0;
        u32 span = ot->window_len ? ot->window_len  : ot->bucket_count;
        u32 b;

        for (b = base; b < base + span && b < ot->bucket_count; b++)
            if (ot->bucket_head[b] >= 0)
                stats->buckets_used++;
    }

    return emitted;
}
