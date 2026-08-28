#include "gpu.h"

#include <stdlib.h>
#include <string.h>

/*
 * The GPU's 4x4 ordered dither. Applied to each colour channel before the
 * truncation to 5 bits per channel that the framebuffer forces.
 */
const s8 psx_dither_matrix[4][4] = {
    { -4,  0, -3,  1 },
    {  2, -2,  3, -1 },
    { -3,  1, -4,  0 },
    {  3, -1,  2, -2 }
};

q2_result psx_ot_init(psx_ot *ot, u32 bucket_count, u32 prim_capacity)
{
    if (!ot || bucket_count == 0 || prim_capacity == 0)
        return Q2_ERR_INVALID_ARG;

    /* The caller counts in the console's buckets; the table holds the
     * subdivided ones. See PSX_OT_SUBDIV. */
    bucket_count *= PSX_OT_SUBDIV;

    memset(ot, 0, sizeof(*ot));

    ot->prims       = (psx_prim *)calloc(prim_capacity, sizeof(psx_prim));
    ot->next        = (s32 *)malloc((size_t)prim_capacity * sizeof(s32));
    ot->bucket_head = (s32 *)malloc((size_t)bucket_count * sizeof(s32));
    ot->batch       = (psx_ot_batch *)calloc(prim_capacity,
                                             sizeof(psx_ot_batch));

    if (!ot->prims || !ot->next || !ot->bucket_head || !ot->batch) {
        psx_ot_free(ot);
        return Q2_ERR_NO_MEMORY;
    }

    ot->prim_capacity = prim_capacity;
    ot->bucket_count  = bucket_count;
    ot->batch_capacity = prim_capacity;
    psx_ot_clear(ot);
    return Q2_OK;
}

void psx_ot_free(psx_ot *ot)
{
    if (!ot)
        return;
    free(ot->prims);
    free(ot->next);
    free(ot->bucket_head);
    free(ot->batch);
    memset(ot, 0, sizeof(*ot));
}

void psx_ot_clear(psx_ot *ot)
{
    u32 i;

    if (!ot)
        return;

    ot->prim_count  = 0;
    ot->batch_count = 0;
    ot->window_base = 0;
    ot->window_len  = 0;
    ot->authored_base = 0;
    ot->authored_len  = 0;
    memset(ot->area_valid, 0, sizeof(ot->area_valid));
    memset(ot->area_screen_valid, 0, sizeof(ot->area_screen_valid));
    ot->area_routing = false;
    for (i = 0; i < ot->bucket_count; i++)
        ot->bucket_head[i] = -1;
}

void psx_ot_area_clear(psx_ot *ot)
{
    if (!ot)
        return;

    /* Moving to the next viewport is one of retail's two drain points. The
     * primitive table survives the move, but the area's freshness map does
     * not, so pending chains must be joined before that map is discarded. */
    psx_ot_flush_batches(ot);
    memset(ot->area_valid, 0, sizeof(ot->area_valid));
    memset(ot->area_screen_valid, 0, sizeof(ot->area_screen_valid));
    ot->area_routing = false;
}

u32 psx_ot_authored_bucket(const psx_ot *ot, u32 console_bucket)
{
    u32 base, span, relative;

    if (!ot || ot->bucket_count == 0)
        return 0;

    if (ot->authored_len) {
        base = ot->authored_base;
        span = ot->authored_len;
    } else if (ot->window_len) {
        base = ot->window_base;
        span = ot->window_len;
    } else {
        base = 0;
        span = ot->bucket_count;
    }
    if (span == 0)
        return base;

    relative = console_bucket * PSX_OT_SUBDIV;
    if (relative >= span)
        relative = span - 1u;
    if (base + relative >= ot->bucket_count)
        return ot->bucket_count - 1u;
    return base + relative;
}

void psx_ot_area_prepare(psx_ot *ot, s16 view_w, s16 view_h,
                         s16 add_x, s16 add_y,
                         s16 view_ofs_x, s16 view_ofs_y)
{
    psx_ot_area_screen full;

    if (!ot)
        return;

    full.min_x = 0;
    full.min_y = 0;
    full.max_x = view_w;
    full.max_y = view_h;
    full.add_x = add_x;
    full.add_y = add_y;

    /* 0x80046E14 treats records 1 and 2 specially.  Area 1 is concatenated
     * unsorted at slice bucket 46; area 2 is dependency-sorted at bucket 1.
     * Neither goes through the freshness test used by records 3..95. */
    ot->area_bucket[1] = psx_ot_authored_bucket(ot, 46);
    ot->area_bucket[2] = psx_ot_authored_bucket(ot, 1);
    ot->area_valid[1] = ot->area_valid[2] = 1;
    ot->area_screen[1] = ot->area_screen[2] = full;
    ot->area_screen_valid[1] = ot->area_screen_valid[2] = 1;
    ot->area_view_ofs_x = view_ofs_x;
    ot->area_view_ofs_y = view_ofs_y;
    ot->area_routing = true;
}

bool psx_ot_area_register_screen(psx_ot *ot, u32 area, u32 console_bucket,
                                 const psx_ot_area_screen *screen)
{
    if (!ot || area >= PSX_OT_AREA_RETAIL_COUNT ||
        ot->bucket_count == 0)
        return false;

    /* The two special records retain their hard-wired drain buckets. */
    if (area != 1 && area != 2)
        ot->area_bucket[area] = psx_ot_authored_bucket(ot, console_bucket);

    ot->area_valid[area]  = 1;
    if (screen) {
        ot->area_screen[area] = *screen;
        ot->area_screen_valid[area] = 1;
    }
    ot->area_routing      = true;
    return true;
}

bool psx_ot_area_register(psx_ot *ot, u32 area, u32 console_bucket)
{
    return psx_ot_area_register_screen(ot, area, console_bucket, NULL);
}

bool psx_ot_area_bucket(const psx_ot *ot, u32 area, u32 *bucket)
{
    if (!ot || area >= PSX_OT_AREA_COUNT || !ot->area_valid[area])
        return false;
    if (bucket)
        *bucket = ot->area_bucket[area];
    return true;
}

bool psx_ot_area_get_screen(const psx_ot *ot, u32 area,
                            psx_ot_area_screen *screen)
{
    if (!ot || area >= PSX_OT_AREA_RETAIL_COUNT ||
        !ot->area_valid[area] || !ot->area_screen_valid[area])
        return false;
    if (screen)
        *screen = ot->area_screen[area];
    return true;
}

bool psx_ot_area_projection(const psx_ot *ot, s32 area,
                            s32 *centre_x, s32 *centre_y)
{
    psx_ot_area_screen screen;
    s32 x, y;

    /* Retail 0x8006568C branches straight to the return for a negative
     * selector. It does not mean "restore full view"; callers which pass -1
     * deliberately retain the projection already installed by their pass. */
    if (!ot || !ot->area_routing || area < 0)
        return false;

    x = ot->area_view_ofs_x;
    y = ot->area_view_ofs_y;
    /* 0x80065694 and 0x800656D8 special-case selectors 0 and 1.  Neither
     * consults the screen-record table: 0 installs the global full-screen
     * extent and 1 installs this viewport's full extent. */
    if (area >= 2) {
        if (!psx_ot_area_get_screen(ot, (u32)area & 0x7Fu, &screen))
            return false;
        x -= screen.min_x;
        y -= screen.min_y;
    }
    if (centre_x) *centre_x = x;
    if (centre_y) *centre_y = y;
    return true;
}

bool psx_ot_area_active(const psx_ot *ot)
{
    return ot && ot->area_routing;
}

/* ------------------------------------------------------------------------- */
/* Retail screen-area batch sorter — 0x80047080..0x80047B98.                 */
/* ------------------------------------------------------------------------- */
#define OT_STANDARD_MAX 32
#define OT_QUICK_MAX   128
#define OT_SORT_MAX    (OT_STANDARD_MAX + OT_QUICK_MAX)

static s32 batch_begin(psx_ot *ot, u32 area, bool quick, bool point,
                       s16 order, const s32 *spatial,
                       const s32 camera[3])
{
    psx_ot_batch *b;
    u32 i, n;

    /* Area zero is the separate global path; 0x80046E14 never drains it. */
    if (!ot || !spatial || !camera || area == 0 ||
        area >= PSX_OT_AREA_RETAIL_COUNT ||
        !ot->area_valid[area] || ot->batch_count >= ot->batch_capacity)
        return PSX_OT_BATCH_INVALID;

    b = &ot->batch[ot->batch_count];
    memset(b, 0, sizeof(*b));
    b->head  = -1;
    b->tail  = -1;
    b->order = order;
    b->area  = (u8)area;
    b->point = point ? 1u : 0u;
    b->quick = quick ? 1u : 0u;

    n = point ? 3u : 6u;
    for (i = 0; i < n; i++)
        b->spatial[i] = spatial[i];
    for (i = 0; i < 3; i++)
        b->camera[i] = camera[i];

    return (s32)ot->batch_count++;
}

s32 psx_ot_batch_begin_box(psx_ot *ot, u32 area, bool quick, s16 order,
                           const s32 min[3], const s32 max[3],
                           const s32 camera[3])
{
    s32 box[6];
    int i;

    if (!min || !max)
        return PSX_OT_BATCH_INVALID;
    for (i = 0; i < 3; i++) {
        box[i]     = min[i];
        box[i + 3] = max[i];
    }
    return batch_begin(ot, area, quick, false, order, box, camera);
}

s32 psx_ot_batch_begin_point(psx_ot *ot, u32 area, bool quick, s16 order,
                             const s32 point[3], const s32 camera[3])
{
    return batch_begin(ot, area, quick, true, order, point, camera);
}

bool psx_ot_batch_link_prim(psx_ot *ot, s32 batch, psx_prim *prim)
{
    psx_ot_batch *b;
    u32 idx;

    if (!ot || !prim || batch < 0 || (u32)batch >= ot->batch_count)
        return false;
    if (prim < ot->prims || prim >= ot->prims + ot->prim_count)
        return false;

    b = &ot->batch[batch];
    idx = (u32)(prim - ot->prims);
    ot->next[idx] = b->head;
    b->head = (s32)idx;
    if (b->tail < 0)
        b->tail = (s32)idx;
    return true;
}

psx_prim *psx_ot_batch_add(psx_ot *ot, s32 batch)
{
    psx_prim *prim;

    if (!ot || batch < 0 || (u32)batch >= ot->batch_count)
        return NULL;

    prim = psx_ot_alloc(ot);
    if (!prim)
        return NULL;
    if (!psx_ot_batch_link_prim(ot, batch, prim))
        return NULL;
    return prim;
}

/* 0x800473C4. Strict AABB overlap followed by a midpoint split on the axis
 * with the smallest penetration. The temporary split is what gives two
 * intersecting boxes a usable painter relation without pretending they do not
 * intersect. */
static bool batch_split_overlap(const s32 a[6], const s32 b[6],
                                s32 out_a[6], s32 out_b[6])
{
    s32 best = 0x7FFFFFFF;
    int side = 6;
    int axis;

    for (axis = 0; axis < 3; axis++) {
        if (a[axis] >= b[axis + 3] || b[axis] >= a[axis + 3])
            return false;
    }

    for (axis = 0; axis < 3; axis++) {
        s32 gap;

        if (b[axis + 3] < a[axis + 3]) {
            gap = b[axis + 3] - a[axis];
            if (gap < best) {
                best = gap;
                side = axis * 2;
            }
        } else if (a[axis] < b[axis]) {
            gap = a[axis + 3] - b[axis];
            if (gap < best) {
                best = gap;
                side = axis * 2 + 1;
            }
        }
    }

    if (side == 6)
        return false;

    memcpy(out_a, a, 6u * sizeof(s32));
    memcpy(out_b, b, 6u * sizeof(s32));
    axis = side >> 1;
    if ((side & 1) == 0) {
        s32 split = out_a[axis] + best / 2;
        out_a[axis]     = split;
        out_b[axis + 3] = split;
    } else {
        s32 split = out_b[axis] + best / 2;
        out_b[axis]     = split;
        out_a[axis + 3] = split;
    }
    return true;
}

/* 0x80047990: one axis of a point-versus-box relation. */
static bool batch_point_separate(s32 point, const s32 box[2], s32 camera)
{
    /* 0x80047998 branches *past* the low-side result when min < camera.
     * The old reconstruction attached that result to the taken branch and
     * consequently judged both outside-camera cases from the wrong face. */
    if (camera <= box[0])
        return point <= box[0] + 10;
    if (camera >= box[1])
        return point >= box[1] - 10;
    return false;
}

/* 0x800479D0: one axis of a box-versus-box relation. */
static bool batch_box_separate(const s32 a[2], const s32 b[2], s32 camera)
{
    /* The same branch shape appears at 0x800479E0: camera on b's low side
     * compares b.min with a.max, camera on its high side compares a.min with
     * b.max, and a camera inside b cannot establish an ordering on this axis. */
    if (camera <= b[0])
        return b[0] >= a[1];
    if (camera >= b[1])
        return a[0] >= b[1];
    return false;
}

/* 0x80047598. True means `a` must wait until `b` has been emitted. */
static bool batch_depends(const psx_ot_batch *a, const psx_ot_batch *b)
{
    s32 split_a[6], split_b[6];
    const s32 *aa = a->spatial;
    const s32 *bb = b->spatial;
    int axis;

    if (a == b)
        return false;

    if (a->point) {
        for (axis = 0; axis < 3; axis++) {
            s32 bounds[2] = { bb[axis], bb[axis + 3] };
            if (batch_point_separate(aa[axis], bounds,
                                     a->camera[axis]))
                return false;
        }
        return true;
    }

    if (batch_split_overlap(aa, bb, split_a, split_b)) {
        aa = split_a;
        bb = split_b;
    }

    for (axis = 0; axis < 3; axis++) {
        s32 a_axis[2] = { aa[axis], aa[axis + 3] };
        s32 b_axis[2] = { bb[axis], bb[axis + 3] };
        if (batch_box_separate(a_axis, b_axis, a->camera[axis]))
            return false;
    }
    return true;
}

static u32 bit_count32(u32 v)
{
    u32 n = 0;
    while (v) {
        v &= v - 1u;
        n++;
    }
    return n;
}

/* Build 0x80047080's output order for one area. `standard` and `quick` are
 * already in linked-list order (newest registration first), exactly as
 * 0x80047B98 flattens them. */
static u32 batch_sort_area(psx_ot *ot,
                           const s32 *standard, u32 standard_count,
                           const s32 *quick, u32 quick_count,
                           s32 out[OT_SORT_MAX])
{
    u32 standard_dep[OT_STANDARD_MAX];
    u32 quick_dep[OT_QUICK_MAX];
    u8 standard_done[OT_STANDARD_MAX];
    u8 quick_done[OT_QUICK_MAX];
    u32 standard_left = standard_count;
    u32 quick_left = quick_count;
    u32 cursor = 0;
    u32 written = 0;
    u32 i, j;

    memset(standard_dep, 0, sizeof(standard_dep));
    memset(quick_dep, 0, sizeof(quick_dep));
    memset(standard_done, 0, sizeof(standard_done));
    memset(quick_done, 0, sizeof(quick_done));

    for (i = 0; i < standard_count; i++) {
        for (j = 0; j < standard_count; j++) {
            /* 0x80047A68 checks the reciprocal row first. Once the earlier
             * newest-first comparison establishes j -> i, retail never adds
             * i -> j as well. This is not just an optimisation: intersecting
             * bounds commonly satisfy the relation in both directions, and
             * retaining both manufactures cycles the console did not have. */
            if ((standard_dep[j] & (1u << i)) != 0)
                continue;
            if (batch_depends(&ot->batch[standard[i]],
                              &ot->batch[standard[j]]))
                standard_dep[i] |= 1u << j;
        }
    }
    for (i = 0; i < quick_count; i++) {
        for (j = 0; j < standard_count; j++) {
            if (batch_depends(&ot->batch[quick[i]],
                              &ot->batch[standard[j]]))
                quick_dep[i] |= 1u << j;
        }
    }

    while (standard_left || quick_left) {
        s32 ready[OT_QUICK_MAX];
        u32 ready_count = 0;

        /* Every currently-unblocked Quick record, stable by signed +4. */
        for (i = 0; i < quick_count; i++) {
            u32 at;

            if (quick_done[i] || quick_dep[i] != 0)
                continue;
            at = ready_count;
            while (at > 0 &&
                   ot->batch[ready[at - 1]].order >
                   ot->batch[quick[i]].order) {
                ready[at] = ready[at - 1];
                at--;
            }
            ready[at] = quick[i];
            ready_count++;
            quick_done[i] = 1;
            quick_left--;
        }
        for (i = 0; i < ready_count; i++)
            out[written++] = ready[i];

        if (!standard_left)
            continue;

        /* One unblocked Standard record, scanning cyclically. */
        {
            u32 scanned;
            s32 pick = -1;

            for (scanned = 0; scanned < standard_count; scanned++) {
                u32 candidate = (cursor + scanned) % standard_count;
                if (!standard_done[candidate] &&
                    standard_dep[candidate] == 0) {
                    pick = (s32)candidate;
                    break;
                }
            }

            if (pick < 0) {
                /* Retail breaks a cycle by clearing the remaining row with
                 * the fewest dependency bits; ties keep the first row. */
                u32 best_bits = OT_STANDARD_MAX + 1u;
                s32 best = -1;

                for (i = 0; i < standard_count; i++) {
                    u32 bits;
                    if (standard_done[i])
                        continue;
                    bits = bit_count32(standard_dep[i]);
                    if (bits < best_bits) {
                        best_bits = bits;
                        best = (s32)i;
                    }
                }
                if (best < 0)
                    break;
                standard_dep[best] = 0;
                cursor = (u32)best;
                continue;
            }

            out[written++] = standard[pick];
            standard_done[pick] = 1;
            standard_left--;
            cursor = ((u32)pick + 1u) % standard_count;

            {
                u32 keep = ~(1u << (u32)pick);
                for (i = 0; i < standard_count; i++)
                    standard_dep[i] &= keep;
                for (i = 0; i < quick_count; i++)
                    quick_dep[i] &= keep;
            }
        }
    }

    return written;
}

static void batch_attach(psx_ot *ot, s32 which, u32 bucket)
{
    psx_ot_batch *b;
    s32 idx;

    if (!ot || which < 0 || (u32)which >= ot->batch_count)
        return;
    b = &ot->batch[which];
    if (b->head < 0 || b->tail < 0)
        return;

    idx = b->head;
    while (idx >= 0) {
        ot->prims[idx].otz = (u16)bucket;
        ot->prims[idx].sort_key = PSX_OT_KEY_NONE;
        if (idx == b->tail)
            break;
        idx = ot->next[idx];
    }
    if (idx != b->tail)
        return;

    ot->next[b->tail] = ot->bucket_head[bucket];
    ot->bucket_head[bucket] = b->head;
}

void psx_ot_flush_batches(psx_ot *ot)
{
    u32 area;

    if (!ot || ot->batch_count == 0)
        return;

    /* Area 1 is not passed to 0x80047080 at all.  0x80046EB4 concatenates its
     * Standard list newest-first, then its Quick list newest-first, at the
     * fixed slice-relative bucket 46. */
    if (ot->area_valid[1]) {
        int quick;
        for (quick = 0; quick <= 1; quick++) {
            u32 i;
            for (i = ot->batch_count; i > 0; i--) {
                psx_ot_batch *b = &ot->batch[i - 1u];
                if (b->area == 1 && b->head >= 0 && b->tail >= 0 &&
                    b->quick == (u8)quick)
                    batch_attach(ot, (s32)(i - 1u), ot->area_bucket[1]);
            }
        }
    }

    /* Area 2 is sorted, but drains at fixed bucket 1 without the freshness
     * gate.  Areas 3..95 use their registered screen record and authored
     * insertion bucket.  Area 0 has a separate global path in retail. */
    for (area = 2; area < PSX_OT_AREA_RETAIL_COUNT; area++) {
        s32 standard[OT_STANDARD_MAX];
        s32 quick[OT_QUICK_MAX];
        s32 order[OT_SORT_MAX];
        u32 standard_count = 0, quick_count = 0, order_count;
        u32 bucket, i;

        if (!psx_ot_area_bucket(ot, area, &bucket))
            continue;

        /* Registration prepends. Walk the port's append-only record pool in
         * reverse to obtain the identical newest-first input, and retain only
         * the list capacities retail passes to 0x80047B98. */
        for (i = ot->batch_count; i > 0; i--) {
            psx_ot_batch *b = &ot->batch[i - 1u];

            if (b->area != area || b->head < 0 || b->tail < 0)
                continue;
            if (b->quick) {
                if (quick_count < OT_QUICK_MAX)
                    quick[quick_count++] = (s32)(i - 1u);
            } else if (standard_count < OT_STANDARD_MAX) {
                standard[standard_count++] = (s32)(i - 1u);
            }
        }

        order_count = batch_sort_area(ot, standard, standard_count,
                                      quick, quick_count, order);

        /* CatPrim prepends, so the final bucket reverses sorter output while
         * retaining every private chain atomically. */
        for (i = 0; i < order_count; i++)
            batch_attach(ot, order[i], bucket);
    }

    /* Records beyond retail's per-area caps, empty records, and records whose
     * area was stale remain unlinked, exactly as the drain drops them. */
    ot->batch_count = 0;
}

void psx_ot_set_window(psx_ot *ot, u32 base, u32 len)
{
    if (!ot)
        return;

    /* Console buckets in, real buckets out — the whole subdivided extent of the
     * named range, so a window still covers exactly what it named. */
    base *= PSX_OT_SUBDIV;
    len  *= PSX_OT_SUBDIV;

    if (base >= ot->bucket_count) {
        ot->window_base = 0;
        ot->window_len  = 0;
        return;
    }
    if (len > ot->bucket_count - base)
        len = ot->bucket_count - base;

    ot->window_base = base;
    ot->window_len  = len;
}

void psx_ot_set_authored_window(psx_ot *ot, u32 base, u32 len)
{
    if (!ot)
        return;

    base *= PSX_OT_SUBDIV;
    len  *= PSX_OT_SUBDIV;

    if (len == 0 || base >= ot->bucket_count) {
        ot->authored_base = 0;
        ot->authored_len  = 0;
        return;
    }
    if (len > ot->bucket_count - base)
        len = ot->bucket_count - base;
    ot->authored_base = base;
    ot->authored_len  = len;
}

u32 psx_ot_bucket_span(const psx_ot *ot)
{
    if (!ot)
        return 0;
    return ot->window_len ? ot->window_len : ot->bucket_count;
}

u32 psx_ot_depth_bucket(const psx_ot *ot, u32 otz)
{
    u32 base, span;

    if (!ot || ot->bucket_count == 0)
        return 0;

    if (ot->window_len) {
        base = ot->window_base;
        span = ot->window_len;
    } else {
        base = 0;
        span = ot->bucket_count;
    }

    if (otz >= span)
        otz = span - 1;

    /* Depth counts down from the far end: the table is drawn bucket 0 first, so
     * the farthest primitive belongs in the lowest bucket of the span. */
    return base + (span - 1u - otz);
}

/* The shared tail of both add paths, once the bucket is decided. */
static psx_prim *ot_link(psx_ot *ot, u32 bucket, u16 otz, u32 key)
{
    u32 idx;
    psx_prim *prim;

    if (bucket >= ot->bucket_count)
        bucket = ot->bucket_count - 1;

    idx  = ot->prim_count++;
    prim = &ot->prims[idx];

    memset(prim, 0, sizeof(*prim));
    prim->otz      = otz;
    prim->sort_key = key;

    if (key == PSX_OT_KEY_NONE) {
        /* Prepend, matching the hardware's list construction. */
        ot->next[idx]           = ot->bucket_head[bucket];
        ot->bucket_head[bucket] = (s32)idx;
        return prim;
    }

    /*
     * Keyed: hold the bucket's list in descending key order from the head, so
     * the walk still draws the farthest thing in the bucket first. Equal keys
     * put the newcomer at the FRONT of its run of equals, which is the prepend
     * rule the hardware has and what every unkeyed caller keeps. A
     * PSX_OT_KEY_NONE entry stops the search — see the header.
     */
    {
        s32 prev = -1;
        s32 cur  = ot->bucket_head[bucket];

        while (cur >= 0 && ot->prims[cur].sort_key != PSX_OT_KEY_NONE &&
               ot->prims[cur].sort_key > key) {
            prev = cur;
            cur  = ot->next[cur];
        }

        ot->next[idx] = cur;
        if (prev < 0)
            ot->bucket_head[bucket] = (s32)idx;
        else
            ot->next[prev] = (s32)idx;
    }

    return prim;
}

u32 q2_ot_bucket_for_depth(const psx_ot *ot, u32 depth, s32 far_z)
{
    u32 span = psx_ot_bucket_span(ot);
    u32 lo, hi, b;

    if (span == 0)
        span = 1;

    /*
     * The depth-addressable part of the slice. A slice too small to hold the
     * reserve on both sides and still have somewhere to put geometry is a test
     * table rather than a viewport, and gets the whole thing.
     */
    if (span > PSX_OT_DEPTH_RESERVE * 2u + 1u) {
        lo = PSX_OT_DEPTH_RESERVE;
        hi = span - 1u - PSX_OT_DEPTH_RESERVE;
    } else {
        lo = 0;
        hi = span - 1u;
    }

    if (far_z > 0) {
        b = lo + (u32)(((u64)depth * (hi - lo)) / (u32)far_z);
    } else {
        b = lo + (depth >> 2);
    }

    if (b > hi)
        b = hi;
    return b;
}

psx_prim *psx_ot_add(psx_ot *ot, u16 otz)
{
    return psx_ot_add_depth(ot, otz, PSX_OT_KEY_NONE);
}

psx_prim *psx_ot_add_depth(psx_ot *ot, u16 otz, u32 key)
{
    if (!ot || ot->prim_count >= ot->prim_capacity)
        return NULL;

    return ot_link(ot, psx_ot_depth_bucket(ot, otz), otz, key);
}

psx_prim *psx_ot_add_bucket_depth(psx_ot *ot, u32 bucket, u16 otz, u32 key)
{
    if (!ot || ot->prim_count >= ot->prim_capacity || ot->bucket_count == 0)
        return NULL;
    return ot_link(ot, bucket, otz, key);
}

psx_prim *psx_ot_add_area_depth(psx_ot *ot, u32 area, u16 otz, u32 key)
{
    u32 bucket;

    if (psx_ot_area_bucket(ot, area, &bucket))
        return psx_ot_add_bucket_depth(ot, bucket, otz, key);
    return psx_ot_add_depth(ot, otz, key);
}

/*
 * ALLOCATE WITHOUT LINKING, and why the model path needs it.
 *
 * A model's faces are BUILT in file order — the scratch window holds one part's
 * transformed vertices at a time, so a face can only be resolved while its own
 * part is current — but they are DRAWN in an order the model carries with it
 * (model.h, block A). The console keeps the two apart by parking packet
 * pointers in a flat array as it builds (0x800DDDCC) and chaining them into the
 * ordering table afterwards, walking that array in the stored order.
 *
 * This is that split. `psx_ot_alloc` takes a primitive out of the pool and
 * leaves it unlinked; `psx_ot_link_prim` puts it in a bucket later. A primitive
 * that is allocated and never linked simply never draws, which is what a culled
 * face wants.
 */
psx_prim *psx_ot_alloc(psx_ot *ot)
{
    u32 idx;
    psx_prim *prim;

    if (!ot || ot->prim_count >= ot->prim_capacity)
        return NULL;

    idx  = ot->prim_count++;
    prim = &ot->prims[idx];
    memset(prim, 0, sizeof(*prim));
    ot->next[idx] = -1;
    return prim;
}

bool psx_ot_link_prim(psx_ot *ot, psx_prim *prim, u32 bucket, u32 key)
{
    u32 idx;

    if (!ot || !prim || ot->bucket_count == 0)
        return false;
    if (prim < ot->prims || prim >= ot->prims + ot->prim_count)
        return false;

    idx = (u32)(prim - ot->prims);

    if (bucket >= ot->bucket_count)
        bucket = ot->bucket_count - 1;

    prim->otz      = (u16)bucket;
    prim->sort_key = key;

    if (key == PSX_OT_KEY_NONE) {
        ot->next[idx]           = ot->bucket_head[bucket];
        ot->bucket_head[bucket] = (s32)idx;
        return true;
    }

    {
        s32 prev = -1;
        s32 cur  = ot->bucket_head[bucket];

        while (cur >= 0 && ot->prims[cur].sort_key != PSX_OT_KEY_NONE &&
               ot->prims[cur].sort_key > key) {
            prev = cur;
            cur  = ot->next[cur];
        }

        ot->next[idx] = cur;
        if (prev < 0)
            ot->bucket_head[bucket] = (s32)idx;
        else
            ot->next[prev] = (s32)idx;
    }
    return true;
}

psx_prim *psx_ot_add_bucket(psx_ot *ot, u32 bucket)
{
    if (!ot || ot->prim_count >= ot->prim_capacity || ot->bucket_count == 0)
        return NULL;

    /*
     * REAL buckets, not console ones. Some callers name a console constant and
     * some hand back a bucket `psx_ot_depth_bucket` has already resolved, and
     * there is no way to tell the two apart here — so the scaling belongs where
     * a console constant is turned into a bucket (q2_screen_view_otz,
     * q2_screen_overlay_otz, and the flash and water buckets in screen.c)
     * rather than at this door. Scaling here instead multiplied the
     * already-resolved ones a second time, which sent the briefing's panel out
     * of its window and drew the world through it.
     */
    return ot_link(ot, bucket, (u16)bucket, PSX_OT_KEY_NONE);
}

void psx_ot_walk(psx_ot *ot, psx_ot_visit_fn fn, void *user)
{
    u32 b;

    if (!ot || !fn)
        return;

    psx_ot_flush_batches(ot);

    /* DrawOTag order: bucket 0 first, higher buckets on top of it. Depth is
     * inverted on the way in (psx_ot_depth_bucket), so this is still far to
     * near for anything that added itself by depth. */
    for (b = 0; b < ot->bucket_count; b++) {
        s32 idx = ot->bucket_head[b];
        while (idx >= 0) {
            fn(&ot->prims[idx], user);
            idx = ot->next[idx];
        }
    }
}
