#include "model.h"

#include "trig.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Bank walking                                                               */
/* ------------------------------------------------------------------------- */

/* Count the models in a chunk by walking the ofs_end chain. A model whose
 * ofs_end is zero is the last one and owns the rest of the chunk. */
static q2_result bank_init(q2_model_bank *out, const dat_chunk *chunk)
{
    u32 cursor = 0, count = 0;

    if (!out)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    if (!chunk)
        return Q2_ERR_BAD_FORMAT;

    /* A zero-length CastList is legal — 17 zones ship one. */
    if (chunk->size == 0)
        return Q2_OK;

    out->data = chunk->data;
    out->size = chunk->size;

    while (cursor + Q2_MODEL_HEADER_SIZE <= chunk->size) {
        u32 ofs_end = q2_rd_u32(chunk->data + cursor + 0x3C);

        count++;

        if (ofs_end == 0)
            break;                   /* last model; its blocks run to the end */

        if (ofs_end < Q2_MODEL_HEADER_SIZE ||
            ofs_end > chunk->size - cursor) {
            Q2_ERROR("model: model %u at 0x%X declares size %u, which escapes "
                     "the %u-byte chunk", count - 1, cursor, ofs_end,
                     chunk->size);
            memset(out, 0, sizeof(*out));
            return Q2_ERR_BAD_FORMAT;
        }

        cursor += ofs_end;
    }

    out->count = count;
    return Q2_OK;
}

q2_result q2_model_bank_from_common(q2_model_bank *out, const q2_common_file *f)
{
    if (!out || !f)
        return Q2_ERR_INVALID_ARG;
    return bank_init(out, f->chunk[Q2_COMMON_CAST_LIST]);
}

q2_result q2_model_bank_from_zone(q2_model_bank *out, const q2_zone_file *f)
{
    if (!out || !f)
        return Q2_ERR_INVALID_ARG;
    return bank_init(out, f->chunk[Q2_ZONE_CAST_LIST]);
}

/* ------------------------------------------------------------------------- */
/* One model                                                                  */
/* ------------------------------------------------------------------------- */

q2_result q2_model_get(const q2_model_bank *bank, u32 index, q2_model *out)
{
    const u8 *p;
    u32 cursor = 0, i;
    u32 sum_faces = 0, sum_verts = 0, hi = 0;
    q2_model_header *h;

    if (!bank || !out || index >= bank->count)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    /* Chain-walk to the requested model. bank_init has already proved every
     * step in the chain is in bounds, so this cannot run off the end. */
    for (i = 0; i < index; i++)
        cursor += q2_rd_u32(bank->data + cursor + 0x3C);

    p = bank->data + cursor;
    h = &out->hdr;

    memcpy(h->name, p + 0x08, 12);
    h->name[12] = '\0';

    h->num_faces   = q2_rd_u16(p + 0x14);
    h->num_parts   = q2_rd_u16(p + 0x16);
    h->ext0        = q2_rd_s16(p + 0x18);
    h->ext1        = q2_rd_s16(p + 0x1A);
    h->ext2        = q2_rd_s16(p + 0x1C);
    h->ext3        = q2_rd_s16(p + 0x1E);
    h->ofs_faces   = q2_rd_u32(p + 0x20);
    h->ofs_verts   = q2_rd_u32(p + 0x24);
    h->ofs_parts   = q2_rd_u32(p + 0x28);
    h->ofs_block_a = q2_rd_u32(p + 0x2C);
    h->ofs_block_b = q2_rd_u32(p + 0x30);
    h->ofs_block_c = q2_rd_u32(p + 0x34);
    h->ofs_block_d = q2_rd_u32(p + 0x38);
    h->ofs_end     = q2_rd_u32(p + 0x3C);

    out->base = p;
    out->size = h->ofs_end ? h->ofs_end : (bank->size - cursor);

    /* Every identity below holds on all 1,723 models on the disc, so a failure
     * means we have misread the chunk, not that the file is merely unusual. */
    if (h->ofs_verts != Q2_MODEL_HEADER_SIZE) {
        Q2_ERROR("model %u (%s): ofs_verts is 0x%X, not 0x40",
                 index, h->name, h->ofs_verts);
        return Q2_ERR_BAD_FORMAT;
    }
    if (h->ofs_parts < h->ofs_verts || h->ofs_faces < h->ofs_parts ||
        h->ofs_parts > out->size || h->ofs_faces > out->size)
        return Q2_ERR_BAD_FORMAT;
    if ((h->ofs_parts - h->ofs_verts) % Q2_MODEL_VERT_SIZE)
        return Q2_ERR_BAD_FORMAT;
    if (h->ofs_faces - h->ofs_parts != (u32)Q2_MODEL_PART_SIZE * h->num_parts)
        return Q2_ERR_BAD_FORMAT;
    if (out->size - h->ofs_faces < (u32)Q2_MODEL_FACE_SIZE * h->num_faces)
        return Q2_ERR_BAD_FORMAT;

    h->num_verts = (h->ofs_parts - h->ofs_verts) / Q2_MODEL_VERT_SIZE;

    /* Derive the scratch window size and check the part table sums exactly.
     * Both sums are exact on 1,723/1,723, so they are a real integrity gate
     * rather than a heuristic. */
    for (i = 0; i < h->num_parts; i++) {
        const u8 *pp  = p + h->ofs_parts + (size_t)i * Q2_MODEL_PART_SIZE;
        u32 base      = q2_rd_u8(pp + 2);
        u32 nverts    = q2_rd_u8(pp + 3);
        u32 end       = base + nverts;

        if (end > Q2_MODEL_SCRATCH_MAX) {
            Q2_ERROR("model %u (%s): part %u needs scratch slot %u, past the "
                     "engine's %u-entry buffer",
                     index, h->name, i, end, Q2_MODEL_SCRATCH_MAX);
            return Q2_ERR_BAD_FORMAT;
        }
        if (end > hi)
            hi = end;

        sum_faces += q2_rd_u16(pp + 0);
        sum_verts += nverts;
    }

    if (sum_faces != h->num_faces || sum_verts != h->num_verts) {
        Q2_ERROR("model %u (%s): parts sum to %u faces / %u verts, header says "
                 "%u / %u", index, h->name, sum_faces, sum_verts,
                 h->num_faces, h->num_verts);
        return Q2_ERR_BAD_FORMAT;
    }

    out->scratch_size = hi;

    /*
     * And the level-load rewrite the class table asks for, decided here so that
     * every reader of this model sees it — which is the reach the console's
     * in-RAM patch has. `RemapModels` (0x8007E6E4) resolves the class name
     * through the loaded model list and hands the header to the record's
     * PreInit; for "Explosion" that is 0x8005AAA4, and the faces come back
     * additive. See the section in model.h.
     */
    out->bodge_transparent = q2_model_class_bodges_transparent(h->name);

    return Q2_OK;
}

/* The same compare `q2_model_bank_find` does, factored out so the class-table
 * lookup below matches names exactly as `GetModelHdr` (0x8006D008) does. */
static bool name_matches(const char *a, const char *b)
{
    /* An inline compare rather than strcasecmp: that is not in C11 and the
     * names are a fixed 12 bytes with no locale in play. */
    while (*a && *b) {
        int ca = *a >= 'a' && *a <= 'z' ? *a - 32 : *a;
        int cb = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
        if (ca != cb)
            break;
        a++;
        b++;
    }
    return !*a && !*b;
}

/*
 * The class-table records whose PreInit rewrites the model's own face data —
 * see the level-load section in model.h.
 *
 * A TABLE OF ONE, deliberately shaped like the console's. `RemapModels`
 * (0x8007E6E4) calls every record's +0x14 with the model header, and of the 58
 * records only 0x800A3CF8 "Explosion" -> 0x8005AAA4 touches the faces. The
 * weapon PreInits register sounds; PreInitDebris (0x80064F70) appends the header
 * to the burst's pick list, which main.c registers by name already; the rest
 * carry a null pointer. If another one ever turns out to bodge geometry, it
 * belongs in this list beside the first.
 */
static const char *const bodge_transparent_classes[] = {
    "Explosion"          /* 0x800A3CF8 +0x14 = 0x8005AAA4 PreInitExplosion */
};

bool q2_model_class_bodges_transparent(const char *name)
{
    u32 i;

    if (!name || !name[0])
        return false;

    for (i = 0; i < (u32)(sizeof(bodge_transparent_classes) /
                          sizeof(bodge_transparent_classes[0])); i++) {
        if (name_matches(name, bodge_transparent_classes[i]))
            return true;
    }
    return false;
}

s32 q2_model_bank_find(const q2_model_bank *bank, const char *name)
{
    u32 i;

    if (!bank || !name || !name[0])
        return -1;

    for (i = 0; i < bank->count; i++) {
        q2_model probe;

        if (q2_model_get(bank, i, &probe) != Q2_OK)
            continue;

        if (name_matches(probe.hdr.name, name))
            return (s32)i;
    }
    return -1;
}

bool q2_model_get_vertex(const q2_model *m, u32 index, q2_model_vertex *out)
{
    const u8 *v;

    if (!m || !out || index >= m->hdr.num_verts)
        return false;

    v = m->base + m->hdr.ofs_verts + (size_t)index * Q2_MODEL_VERT_SIZE;

    out->x = q2_rd_s16(v + 0);
    out->y = q2_rd_s16(v + 2);
    out->z = q2_rd_s16(v + 4);

    /* The stored order is z, x, y. Reading it as x, y, z — as an earlier pass
     * documented — yields a vector uncorrelated with the surface. */
    out->nz = q2_rd_s16(v + 6);
    out->nx = q2_rd_s16(v + 8);
    out->ny = q2_rd_s16(v + 10);

    return true;
}

bool q2_model_get_part(const q2_model *m, u32 index, q2_model_part *out)
{
    const u8 *p;

    if (!m || !out || index >= m->hdr.num_parts)
        return false;

    p = m->base + m->hdr.ofs_parts + (size_t)index * Q2_MODEL_PART_SIZE;

    out->num_faces = q2_rd_u16(p + 0);
    out->vert_base = q2_rd_u8(p + 2);
    out->num_verts = q2_rd_u8(p + 3);
    out->force_draw = q2_rd_u32(p + 4);

    return true;
}

bool q2_model_get_face(const q2_model *m, u32 index, q2_model_face *out)
{
    const u8 *f;
    int i;

    if (!m || !out || index >= m->hdr.num_faces)
        return false;

    f = m->base + m->hdr.ofs_faces + (size_t)index * Q2_MODEL_FACE_SIZE;

    for (i = 0; i < 4; i++) {
        out->v[i]     = q2_rd_u8(f + i);
        out->uv[i][0] = q2_rd_u8(f + 4 + i * 2);
        out->uv[i][1] = q2_rd_u8(f + 4 + i * 2 + 1);
    }
    out->flags   = q2_rd_u8(f + 12);
    out->texture = q2_rd_u8(f + 13);

    /*
     * BodgeModelDataTransparent, 0x8005B938, on the models whose class asks for
     * it — `lbu v0,12(a0); andi 0x1F; ori 0x40; sb v0,12(a0)`. The texture page
     * survives and the blend selector becomes 2, which is B+F: additive.
     *
     * The console writes this into the loaded chunk once, at level load, so
     * doing it here rather than there is a difference of bookkeeping and not of
     * behaviour — the byte a reader sees is the same byte. What is NOT copied is
     * the walker's off-by-one, which bodges one record past the array into block
     * A's padding; see model.h.
     */
    if (m->bodge_transparent)
        out->flags = (u8)((out->flags & Q2_MODEL_BODGE_KEEP) |
                          Q2_MODEL_BODGE_SET);

    return true;
}

/* ------------------------------------------------------------------------- */
/* Block A — the face draw order                                              */
/* ------------------------------------------------------------------------- */
bool q2_model_get_draw_order(const q2_model *m, u32 index,
                             q2_model_draw_order *out)
{
    const u8 *e;

    if (!m || !out || index >= Q2_MODEL_DRAW_ORDERS || !m->hdr.ofs_block_a)
        return false;
    if (m->hdr.ofs_block_a + Q2_MODEL_DRAW_ORDER_TABLE > m->size)
        return false;

    e = m->base + m->hdr.ofs_block_a + (size_t)index * 8;
    out->start  = q2_rd_u16(e + 0);
    out->offset = q2_rd_u16(e + 2);
    return true;
}

/*
 * Walk one stream — see the block-A note in model.h for where every rule here
 * comes from. In brief: signed byte deltas, taken MOST SIGNIFICANT FIRST out of
 * each 32-bit word, a zero byte meaning "the next step is +256 on top of its
 * own delta", and a single-step wrap rather than a modulo.
 */
u32 q2_model_draw_sequence(const q2_model *m, u32 index, u16 *out, u32 max)
{
    q2_model_draw_order ord;

    if (!q2_model_get_draw_order(m, index, &ord))
        return 0;
    return q2_model_draw_sequence_from(m, &ord, out, max);
}

u32 q2_model_draw_sequence_from(const q2_model *m,
                                const q2_model_draw_order *ord,
                                u16 *out, u32 max)
{
    const u8 *stream;
    u32 n, limit, written = 0, read = 0;
    s32 cur;
    bool pending = false;

    if (!m || !ord || !out || max == 0 || !m->hdr.ofs_block_a)
        return 0;

    n = m->hdr.num_faces;
    if (n == 0)
        return 0;

    if (ord->start >= n)
        return 0;

    /* The stream may not leave the model's own image. */
    if (m->hdr.ofs_block_a + ord->offset >= m->size)
        return 0;
    stream = m->base + m->hdr.ofs_block_a + ord->offset;
    limit  = m->size - (m->hdr.ofs_block_a + ord->offset);

    cur = (s32)ord->start;
    out[written++] = (u16)cur;

    while (written < n && written < max) {
        u32 word = (read >> 2) << 2;
        u32 lane = 3u - (read & 3u);
        s32 d;

        if (word + lane >= limit)
            break;                      /* malformed: stop rather than guess */

        d = (s32)(s8)stream[word + lane];
        read++;

        if (pending) {
            cur += 256;
            pending = false;
        } else if (d == 0) {
            pending = true;
            continue;
        }

        cur += d;
        if (cur >= (s32)n)
            cur -= (s32)n;
        else if (cur < 0)
            cur += (s32)n;

        if (cur < 0 || cur >= (s32)n)
            break;                      /* a step no single wrap can bring back */

        out[written++] = (u16)cur;
    }

    return written;
}

/*
 * CalcSortList, 0x8006A020 — see the note in model.h. The octant is three sign
 * tests (0x8006A058..0x8006A074); block B's chain for it is searched for a node
 * with a range holding the animation position (0x8006A0A8..0x8006A104), and
 * without one the octant indexes block A directly (0x8006A16C).
 */
u32 q2_model_draw_octant(const s32 dir[3])
{
    u32 octant = 0;

    if (!dir)
        return 0;
    if (dir[0] > 0) octant |= 1u;
    if (dir[1] > 0) octant |= 2u;
    if (dir[2] > 0) octant |= 4u;
    return octant;
}

bool q2_model_draw_order_select(const q2_model *m, const s32 dir[3],
                                s32 anim_pos, q2_model_draw_order *out)
{
    u32 octant = q2_model_draw_octant(dir);

    if (!m || !out)
        return false;

    /* Block B is the one header offset InitialiseModelFile leaves relative, so
     * its chain links are offsets from the block, as the console adds them. */
    if (m->hdr.ofs_block_b &&
        m->hdr.ofs_block_b + 2u * Q2_MODEL_DRAW_ORDERS <= m->size) {
        const u8 *b    = m->base + m->hdr.ofs_block_b;
        u32       node = q2_rd_u16(b + 2u * octant);
        u32       hops = 0;

        while (node != 0 && hops++ < 256) {
            u32 at = m->hdr.ofs_block_b + node;
            u32 head, count, k;

            if (at + 4u > m->size)
                break;
            head  = q2_rd_u16(m->base + at);
            count = head & 0x7Fu;
            if (at + 4u + 4u * count > m->size)
                break;

            for (k = 0; k < count; k++) {
                const u8 *e = m->base + at + 4u + 4u * k;
                /* BodgeAnimData multiplies both halfwords by ten at load
                 * (0x8006C214); the port reads the disc, so it scales here.
                 * The compare is `slt` against the entity's signed +0x100. */
                s32 lo = (u16)(q2_rd_u16(e + 0) * 10u);
                s32 hi = (u16)(q2_rd_u16(e + 2) * 10u);

                if (anim_pos >= lo && anim_pos <= hi) {
                    /* The node's own stream follows its ranges, and the
                     * offset the draw takes is from block A (0x8006A15C..
                     * 0x8006A168: the address less block A's low half). */
                    out->start  = (u16)(head >> 7);
                    out->offset = (u16)(at + 4u + 4u * count -
                                        m->hdr.ofs_block_a);
                    return true;
                }
            }
            node = q2_rd_u16(m->base + at + 2u);
        }
    }

    return q2_model_get_draw_order(m, octant, out);
}

bool q2_model_is_static(const q2_model *m)
{
    u32 i;

    if (!m)
        return false;

    for (i = 0; i < m->hdr.num_parts; i++) {
        const u8 *p = m->base + m->hdr.ofs_parts + (size_t)i * Q2_MODEL_PART_SIZE;

        if (q2_rd_u8(p + 2) != 0)
            return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Animation — block C                                                        */
/* ------------------------------------------------------------------------- */

/*
 * Block C runs from ofs_block_c to ofs_block_d. Clip metadata stays inside
 * that span: the clip chain is a linked list of byte deltas read from the file,
 * so a malformed one could otherwise walk the whole chunk, and the keyframe
 * offsets are file data too.
 */
static bool anim_span(const q2_model *m, u32 *begin, u32 *end)
{
    if (!m || m->hdr.ofs_block_c == 0)
        return false;
    if (m->hdr.ofs_block_d <= m->hdr.ofs_block_c)
        return false;
    if (m->hdr.ofs_block_d > m->size)
        return false;

    *begin = m->hdr.ofs_block_c;
    *end   = m->hdr.ofs_block_d;
    return true;
}

/*
 * Block D runs from ofs_block_d to the end of the model, in 20-byte records,
 * exactly as `0x8007EA08` reads it. What ends the run is not a signed sentinel;
 * `q2_model_move_get` below reads the terminator off the bytes.
 */
static bool move_span(const q2_model *m, u32 *begin, u32 *end)
{
    if (!m || m->hdr.ofs_block_d == 0)
        return false;
    if (m->hdr.ofs_block_d + 20 > m->size)
        return false;

    *begin = m->hdr.ofs_block_d;
    *end   = m->size;
    return true;
}

bool q2_model_move_get(const q2_model *m, u32 index, q2_model_move *out)
{
    u32 begin, end, at;
    const u8 *p;

    if (!out || !move_span(m, &begin, &end))
        return false;

    at = begin + index * 20;
    if (at + 20 > end)
        return false;

    p = m->base + at;

    /*
     * A record with no name and no range is the end of the run. Block D has no
     * signed sentinel — its +0 is ASCII — so an all-zero head is the terminator
     * the note describes as "a zero word".
     */
    if (p[0] == 0 && q2_rd_u16(p + 12) == 0 && q2_rd_u16(p + 14) == 0)
        return false;

    memcpy(out->name, p, 12);
    out->name[12] = '\0';
    out->start = q2_rd_u16(p + 12);
    out->end   = q2_rd_u16(p + 14);
    out->rest  = q2_rd_u16(p + 16);
    out->one   = q2_rd_u16(p + 18);
    return true;
}

u32 q2_model_move_frames(const q2_model_move *mv)
{
    if (!mv || mv->end < mv->start)
        return 0;
    return (u32)(mv->end - mv->start) / Q2_MODEL_BLOCKD_PER_FRAME + 1;
}

bool q2_model_clip_for_move(const q2_model *m, u32 index, q2_model_anim *out)
{
    q2_model_move mv;

    if (!out || !q2_model_move_get(m, index, &mv))
        return false;

    /* The k-th move drives the k-th clip; no length matching, no tie to break. */
    if (!q2_model_anim_get(m, index, out))
        return false;

    /*
     * The two agree on the frame count on every model on the disc that has
     * both. A disagreement means we are reading a model this rule does not
     * cover, so say so rather than pose it from the wrong clip.
     */
    if (out->frames != q2_model_move_frames(&mv))
        return false;

    return true;
}

bool q2_model_position_for_move(const q2_model *m, const char *move_name,
                                s32 ai_frame, s32 move_first, u32 *out_pos)
{
    q2_model_move mv;
    s32 pos;

    if (!out_pos || !q2_model_move_by_name(m, move_name, &mv))
        return false;
    if (ai_frame < move_first)
        return false;

    /* block D counts 2 units per animation frame; the position counts 10. */
    pos = (s32)mv.start * 5
        + (ai_frame - move_first) * Q2_MODEL_POS_PER_MOVE_FRAME;

    if (pos < 0)
        return false;

    *out_pos = (u32)pos;
    return true;
}

bool q2_model_move_by_name(const q2_model *m, const char *name,
                           q2_model_move *out)
{
    q2_model_move mv;
    u32 i = 0;

    if (!name || !out)
        return false;

    while (q2_model_move_get(m, i, &mv)) {
        if (strcmp(mv.name, name) == 0) {
            *out = mv;
            return true;
        }
        i++;
    }
    return false;
}

u32 q2_model_move_count(const q2_model *m)
{
    q2_model_move mv;
    u32 n = 0;

    while (q2_model_move_get(m, n, &mv))
        n++;
    return n;
}

/* A clip's header is 8 bytes plus one 4-byte entry per frame. */
static bool anim_read(const q2_model *m, u32 offset, u32 end, q2_model_anim *out)
{
    const u8 *p;

    if (offset + 8 > end)
        return false;

    p = m->base + offset;
    out->offset = offset;
    out->frames = q2_rd_u16(p + 0);
    out->flags  = q2_rd_u16(p + 2);
    out->next   = q2_rd_u32(p + 4);

    /* A clip with no frames has no keys to point at and would make the tick
     * walk spin, so treat it as malformed rather than empty. */
    if (out->frames == 0)
        return false;
    if (offset + 8 + (size_t)out->frames * 4 > end)
        return false;

    return true;
}

bool q2_model_anim_get(const q2_model *m, u32 index, q2_model_anim *out)
{
    u32 begin, end, offset, i;

    if (!m || !out || !anim_span(m, &begin, &end))
        return false;

    offset = begin;
    for (i = 0; ; i++) {
        if (!anim_read(m, offset, end, out))
            return false;
        if (i == index)
            return true;
        if (out->next == 0)
            return false;          /* end of chain */
        if (out->next > end - offset)
            return false;          /* delta escapes the block */
        offset += out->next;
    }
}

bool q2_model_anim_at(const q2_model *m, u32 tick, q2_model_anim *out,
                      u32 *within)
{
    u32 begin, end, offset;

    if (!m || !out || !anim_span(m, &begin, &end))
        return false;

    offset = begin;
    for (;;) {
        if (!anim_read(m, offset, end, out))
            return false;

        /* The engine's own test is `position < clip->frames`, checked before
         * the subtraction, so a clip of zero length is stepped over rather
         * than being a resting place. */
        if (tick < out->frames) {
            if (within)
                *within = tick;
            return true;
        }

        tick -= out->frames;

        if (out->next == 0)
            return false;                  /* the timeline ends here */
        if (out->next > end - offset)
            return false;                  /* delta escapes the block */
        offset += out->next;
    }
}

bool q2_model_anim_at_held(const q2_model *m, u32 tick, q2_model_anim *out,
                           u32 *within, bool *clamped)
{
    u32 begin, end, offset;
    /* `have_last` is what actually guards the read below, but MSVC does not
     * track the correlation between a flag and the value it guards and warns
     * (C4701) that `last` may be used uninitialised. Zeroing it costs nothing
     * and is the honest value for "no animation was walked past". */
    q2_model_anim last = {0};
    bool have_last = false;

    if (clamped)
        *clamped = false;

    if (!m || !out || !anim_span(m, &begin, &end))
        return false;

    offset = begin;
    for (;;) {
        if (!anim_read(m, offset, end, out))
            break;

        if (tick < out->frames) {
            if (within)
                *within = tick;
            return true;
        }

        tick -= out->frames;

        if (out->frames) {
            last      = *out;
            have_last = true;
        }

        if (out->next == 0)
            break;
        if (out->next > end - offset)
            break;
        offset += out->next;
    }

    if (!have_last)
        return false;

    *out = last;
    if (within)
        *within = last.frames ? last.frames - 1 : 0;
    if (clamped)
        *clamped = true;
    return true;
}

bool q2_model_anim_by_length(const q2_model *m, u32 frames, u32 skip,
                             q2_model_anim *out)
{
    u32 i, n, seen = 0;

    if (!m || frames == 0)
        return false;

    n = q2_model_anim_count(m);
    for (i = 0; i < n; i++) {
        q2_model_anim a;

        if (!q2_model_anim_get(m, i, &a))
            break;
        if (a.frames != frames)
            continue;
        if (seen++ < skip)
            continue;
        if (out)
            *out = a;
        return true;
    }

    return false;
}

u32 q2_model_anim_count(const q2_model *m)
{
    q2_model_anim clip;
    u32 n = 0;

    while (q2_model_anim_get(m, n, &clip))
        n++;
    return n;
}

u32 q2_model_anim_base(const q2_model *m, const q2_model_anim *clip)
{
    q2_model_anim at;
    u32 n, base = 0;

    if (!clip)
        return 0;
    for (n = 0; q2_model_anim_get(m, n, &at); n++) {
        if (at.offset == clip->offset)
            return base;
        base += (u32)at.frames * Q2_MODEL_TICKS_PER_FRAME;
    }
    return 0;
}

/* Unpack one key's translation. The shifts are the engine's own, and they are
 * what makes an 11-bit field cover a 12-bit range. */
static void key_translation(u32 t, s16 out[3])
{
    out[0] = (s16)(((s32)(t << 21) >> 21) * 2);
    out[1] = (s16)(((s32)(t << 11) >> 22) * 4);
    out[2] = (s16)(((s32)t >> 21) * 2);
}

/*
 * Unpack one key's rotation. The three fields are HALF angles on the 4096-step
 * circle — which is why they only need to span half of it — and 0x800699E8
 * turns them into a quaternion with the textbook Euler products.
 */
void q2_model_key_rotation(u32 r, s16 out[4])
{
    s32 ha = (s32)(r & 0x7FF);
    s32 hb = (s32)((r >> 11) & 0x3FF) * 2;   /* stored at half resolution */
    s32 hc = (s32)(r >> 21);

    s32 sa = q2_sin12(ha), ca = q2_cos12(ha);
    s32 sb = q2_sin12(hb), cb = q2_cos12(hb);
    s32 sc = q2_sin12(hc), cc = q2_cos12(hc);

    s32 ss  = (sc * sa) >> 12;
    s32 ccp = (cc * ca) >> 12;

    out[0] = (s16)((((cb * cc) >> 12) * sa >> 12) - (((sb * sc) >> 12) * ca >> 12));
    out[1] = (s16)(((sb * ccp) >> 12) + ((cb * ss) >> 12));
    out[2] = (s16)((((cb * sc) >> 12) * ca >> 12) - (((sb * cc) >> 12) * sa >> 12));
    out[3] = (s16)(((cb * ccp) >> 12) + ((sb * ss) >> 12));
}

/* One part's key stream in a variable-rate clip: a run of 4-bit durations in
 * frames, eight to a word, and a parallel list of 8-byte keys. */
static bool rate_nibble(const q2_model *m, u32 rate, u32 end, u32 index, u32 *out)
{
    u32 word_at = rate + (index / 8) * 4;

    if (word_at + 4 > end)
        return false;
    *out = (q2_rd_u32(m->base + word_at) >> ((index % 8) * 4)) & 0xF;
    return true;
}

/*
 * The raw packed rotation word of one part, which `q2_model_pose_at` would
 * decode — for the one caller that has to change a field of it before it is
 * decoded.
 *
 * That caller is the hyperblaster. `0x8006D43C` walks to the same key this
 * function does and OVERWRITES bits of the packed word in place, then leaves
 * the ordinary pose path to read it back; the engine can do that because the
 * model lives in writable RAM. Nothing here writes to a loaded model, so the
 * word is handed out instead and the caller re-decodes its own copy.
 *
 * UNIFORM CLIPS ONLY (`flags` bit 0 clear), which is what the one caller has:
 * a variable-rate clip interpolates between two keys and there is no single
 * word to hand back.
 */
bool q2_model_part_rotation_word(const q2_model *m, const q2_model_anim *clip,
                                 u32 tick, u32 part, u32 *out)
{
    u32 begin, end, entry, keys, duration;

    if (!m || !clip || !out || part >= m->hdr.num_parts)
        return false;
    if (clip->flags & 1)
        return false;
    if (!anim_span(m, &begin, &end))
        return false;

    duration = (u32)clip->frames * Q2_MODEL_TICKS_PER_FRAME;
    if (duration < Q2_MODEL_TICKS_PER_FRAME)
        return false;
    if (tick > duration - Q2_MODEL_TICKS_PER_FRAME)
        tick = duration - Q2_MODEL_TICKS_PER_FRAME;

    entry = clip->offset + 8 + (tick / Q2_MODEL_TICKS_PER_FRAME) * 4;
    if (entry + 4 > end)
        return false;

    keys = entry + q2_rd_u16(m->base + entry + 2);
    if (keys + (size_t)m->hdr.num_parts * 8 > end)
        return false;

    *out = q2_rd_u32(m->base + keys + (size_t)part * 8 + 4);
    return true;
}

q2_result q2_model_pose_at(const q2_model *m, const q2_model_anim *clip,
                           u32 tick, q2_model_pose *out)
{
    return q2_model_pose_transition(m, clip, tick, NULL, out);
}

q2_result q2_model_pose_transition(const q2_model *m, const q2_model_anim *clip,
                                   u32 tick, const q2_model_anim *next,
                                   q2_model_pose *out)
{
    u32 begin, end, entry, keys, part, duration;
    u32 original_tick = tick;

    if (!m || !clip || !out)
        return Q2_ERR_INVALID_ARG;
    if (!anim_span(m, &begin, &end))
        return Q2_ERR_NOT_FOUND;

    /*
     * The engine clamps to the START of the last frame, not to the end of the
     * clip (0x8006B4F8: `if (duration - 10 < tick) tick = duration - 10`), so a
     * caller that overruns holds the final pose.
     *
     * The difference from clamping at the clip end is not cosmetic. Ticks in
     * the last ten subticks are inside the clip but past the last key, and a
     * looser clamp walks the duration stream off its end: the nibbles for the
     * final key are followed by zeros, and a zero duration advances the key
     * index without consuming time, so the walk runs away. That is exactly what
     * 428 of the disc's 123,704 frames do under the looser rule.
     */
    duration = (u32)clip->frames * Q2_MODEL_TICKS_PER_FRAME;
    if (duration < Q2_MODEL_TICKS_PER_FRAME)
        return Q2_ERR_BAD_FORMAT;
    if (tick > duration - Q2_MODEL_TICKS_PER_FRAME)
        tick = duration - Q2_MODEL_TICKS_PER_FRAME;

    if (!(clip->flags & 1)) {
        /* Uniform: one key block per frame, every part keyed every frame. */
        entry = clip->offset + 8 + (tick / Q2_MODEL_TICKS_PER_FRAME) * 4;
        if (entry + 4 > end)
            return Q2_ERR_BAD_FORMAT;

        /* The key offset is relative to the entry, not to the clip. */
        keys = entry + q2_rd_u16(m->base + entry + 2);

        if (keys + (size_t)m->hdr.num_parts * 8 > end)
            return Q2_ERR_BAD_FORMAT;

        for (part = 0; part < m->hdr.num_parts; part++) {
            const u8 *k = m->base + keys + (size_t)part * 8;
            key_translation(q2_rd_u32(k), out[part].t);
            q2_model_key_rotation(q2_rd_u32(k + 4), out[part].q);
        }
        return Q2_OK;
    }

    /*
     * Variable rate: the four-byte entries after the header are PER PART, not
     * per frame, and each names that part's own duration stream and key list.
     * A part holds a key for as many frames as its nibble says, and the pose
     * between keys is interpolated.
     */
    /* 0x8006BA54 leaves the pose scratch untouched for this mixed format. */
    if (next && !(next->flags & 1u)) return Q2_OK;
    for (part = 0; part < m->hdr.num_parts; part++) {
        u32 rate, k = 0, frames_left, sub, span, pos, dur = 0;
        s16 t0[3], t1[3], q0[4], q1[4];
        const u8 *ka, *kb;
        s32 scale;
        int i;

        entry = clip->offset + 8 + part * 4;
        if (entry + 4 > end)
            return Q2_ERR_BAD_FORMAT;

        rate = entry + q2_rd_u16(m->base + entry + 0);
        keys = entry + q2_rd_u16(m->base + entry + 2);

        /* 0x8006B53C has a first-key fast path, including zero-duration keys. */
        if (!tick) {
            if (keys + 8 > end) return Q2_ERR_BAD_FORMAT;
            key_translation(q2_rd_u32(m->base + keys), out[part].t);
            q2_model_key_rotation(q2_rd_u32(m->base + keys + 4), out[part].q);
            continue;
        }

        frames_left = tick / Q2_MODEL_TICKS_PER_FRAME;
        sub         = tick % Q2_MODEL_TICKS_PER_FRAME;

        /*
         * Walk the durations until the current time falls inside one.
         *
         * A zero nibble is legal and common: it advances the key index without
         * consuming a frame, which is how a part gets two keys on the same
         * frame. The walk still terminates because the index advances either
         * way — but bound it anyway, since the stream is file data and a
         * pathological one would otherwise run off the end of the block.
         */
        for (;;) {
            if (k > (u32)clip->frames * 2 + 16)
                return Q2_ERR_BAD_FORMAT;
            if (!rate_nibble(m, rate, end, k, &dur))
                return Q2_ERR_BAD_FORMAT;
            if (frames_left * Q2_MODEL_TICKS_PER_FRAME + sub <=
                dur * Q2_MODEL_TICKS_PER_FRAME)
                break;
            frames_left -= dur;
            k++;
        }

        span = dur * Q2_MODEL_TICKS_PER_FRAME;
        pos  = frames_left * Q2_MODEL_TICKS_PER_FRAME + sub;

        if (keys + (size_t)(k + 1) * 8 > end)
            return Q2_ERR_BAD_FORMAT;

        ka = m->base + keys + (size_t)k * 8;

        /* 0x8006B6DC reads the successor even across block C's end. It is
         * still part of this model's allocated data (usually block D). Keep
         * that native read, bounded by the complete model, not the clip. */
        if (keys + (size_t)(k + 2) * 8 > m->size)
            return Q2_ERR_BAD_FORMAT;
        kb = ka + 8;

        if (next && (next->flags & 1u)) {
            u32 incoming = next->offset + 8 + part * 4;
            if (incoming + 4 > end) return Q2_ERR_BAD_FORMAT;
            incoming += q2_rd_u16(m->base + incoming + 2);
            if (incoming + 8 > end || original_tick - tick > 10)
                return Q2_ERR_BAD_FORMAT;
            ka = kb;
            kb = m->base + incoming;
            span = 10;
            pos = original_tick - tick;
        }

        if (pos == span) {
            key_translation(q2_rd_u32(kb), out[part].t);
            q2_model_key_rotation(q2_rd_u32(kb + 4), out[part].q);
            continue;
        }

        key_translation(q2_rd_u32(ka), t0);
        q2_model_key_rotation(q2_rd_u32(ka + 4), q0);
        key_translation(q2_rd_u32(kb), t1);
        q2_model_key_rotation(q2_rd_u32(kb + 4), q1);

        /* Rounded division, as the original's (pos*4096 + span/2) / span. */
        scale = (s32)((pos * (u32)Q2_ONE_12 + span / 2) / span);

        for (i = 0; i < 3; i++)
            out[part].t[i] = (s16)(t0[i] + (((t1[i] - t0[i]) * scale) >> 12));
        q2_quat_slerp(q0, q1, scale, out[part].q);
    }

    return Q2_OK;
}

q2_result q2_model_bake_indices(const q2_model *m, u16 *out)
{
    s32 map[Q2_MODEL_SCRATCH_MAX];
    u32 p, k, f, i, cv = 0, fi = 0;

    if (!m || !out)
        return Q2_ERR_INVALID_ARG;

    for (i = 0; i < Q2_MODEL_SCRATCH_MAX; i++)
        map[i] = -1;

    /* Parts MUST be walked forward in file order. A part's faces may read slots
     * written by an EARLIER part — 21,217 faces do, all in articulated models —
     * and reversing the walk produces exactly 18,995 reads of never-written
     * slots across the disc. */
    for (p = 0; p < m->hdr.num_parts; p++) {
        const u8 *pp = m->base + m->hdr.ofs_parts + (size_t)p * Q2_MODEL_PART_SIZE;
        u32 nfaces   = q2_rd_u16(pp + 0);
        u32 base     = q2_rd_u8(pp + 2);
        u32 nverts   = q2_rd_u8(pp + 3);

        /*
         * LAST WRITER WINS. This one line is the INFERRED part of the format.
         * A rival rule — storage = cv + (index - base), with no shared buffer —
         * also resolves 100% of the disc's indices in range and differs on
         * 18,283 articulated faces; the geometry cannot separate them. We take
         * this one because it is the only rule reaching 100.0000% storage
         * coverage on every model, where the rival orphans 746 vertices. If the
         * per-part transform matrices ever settle the question the other way,
         * this loop is the only thing that changes.
         */
        for (k = 0; k < nverts; k++)
            map[base + k] = (s32)(cv + k);
        cv += nverts;

        for (f = 0; f < nfaces; f++, fi++) {
            const u8 *fp;

            if (fi >= m->hdr.num_faces)
                return Q2_ERR_BAD_FORMAT;

            fp = m->base + m->hdr.ofs_faces + (size_t)fi * Q2_MODEL_FACE_SIZE;

            for (i = 0; i < 4; i++) {
                u32 ix = q2_rd_u8(fp + i);
                s32 s;

                /* Range-check BEFORE indexing, not after. The natural way to
                 * write this reads map[ix] first and tests the result, which
                 * cannot catch an out-of-range index at all. */
                if (ix >= m->scratch_size)
                    return Q2_ERR_BAD_FORMAT;

                s = map[ix];
                if (s < 0)
                    return Q2_ERR_BAD_FORMAT;   /* never fires on retail data */

                out[fi * 4 + i] = (u16)s;
            }
        }
    }

    return (fi == m->hdr.num_faces) ? Q2_OK : Q2_ERR_BAD_FORMAT;
}

/* ------------------------------------------------------------------------- */
/* The lowest point of a POSE, as opposed to the model's own — see model.h    */
/* ------------------------------------------------------------------------- */
bool q2_model_pose_low_y(const q2_model *m, const q2_model_pose *pose,
                         u32 pose_count, s32 *out_low_y)
{
    u32 part, cursor = 0;
    s32 low = -(1 << 30);
    bool any = false;

    if (!m || !out_low_y)
        return false;

    for (part = 0; part < m->hdr.num_parts; part++) {
        q2_model_part p;
        s16 rot[3][3];
        bool posed = pose && part < pose_count;
        u32 v;

        if (!q2_model_get_part(m, part, &p))
            break;

        if (posed)
            q2_quat_to_matrix(rot, pose[part].q);

        for (v = 0; v < p.num_verts; v++) {
            q2_model_vertex mv;
            s32 y;

            if (!q2_model_get_vertex(m, cursor + v, &mv))
                break;

            /*
             * Only the Y row is needed, so only the Y row is multiplied. The
             * bounds walk in `q2psx-inspect model` does all three because it
             * frames a camera; this places a body on a floor.
             */
            if (posed)
                y = (((s32)rot[1][0] * mv.x +
                      (s32)rot[1][1] * mv.y +
                      (s32)rot[1][2] * mv.z) >> 12) + pose[part].t[1];
            else
                y = mv.y;

            if (y > low)        /* Y points DOWN: the largest is the lowest */
                low = y;
            any = true;
        }
        cursor += p.num_verts;
    }

    if (!any)
        return false;

    *out_low_y = low;
    return true;
}

bool q2_model_anim_at_position(const q2_model *m, u32 position,
                               q2_model_anim *out, u32 *within_tick)
{
    u32 within = 0;
    u32 sub = position % Q2_MODEL_TICKS_PER_FRAME;

    if (!q2_model_anim_at(m, position / Q2_MODEL_TICKS_PER_FRAME,
                          out, &within))
        return false;
    if (within_tick)
        *within_tick = within * Q2_MODEL_TICKS_PER_FRAME + sub;
    return true;
}

bool q2_model_anim_at_position_held(const q2_model *m, u32 position,
                                    q2_model_anim *out, u32 *within_tick,
                                    bool *clamped)
{
    u32 within = 0;
    u32 sub = position % Q2_MODEL_TICKS_PER_FRAME;
    bool held = false;

    if (!q2_model_anim_at_held(m, position / Q2_MODEL_TICKS_PER_FRAME,
                               out, &within, &held))
        return false;

    /* Once the timeline is held there is no next key to interpolate toward. */
    if (within_tick)
        *within_tick = within * Q2_MODEL_TICKS_PER_FRAME + (held ? 0u : sub);
    if (clamped)
        *clamped = held;
    return true;
}

void q2_model_cursor_reset(q2_model_cursor *c, s32 position)
{
    if (!c)
        return;
    c->position = position;
    c->target   = position;
    c->ready    = true;
}

s32 q2_model_cursor_phase(q2_model_cursor *c, s32 base, s32 dt)
{
    s64 next, limit;

    if (!c)
        return base;
    if (!c->ready) {
        q2_model_cursor_reset(c, base);
        return base;
    }

    if (dt < 0)
        dt = 0;

    /* Generic display fallback. The native creature cursor is maintained by
     * q2_cre_motion_animate independently of display-frame sampling. */
    if (base != c->target) {
        q2_model_cursor_reset(c, base);
        return base;
    }

    next  = (s64)c->position + dt;
    limit = (s64)base + Q2_MODEL_POS_PER_MOVE_FRAME - 1;
    c->position = next < limit ? (s32)next : (s32)limit;

    return c->position;
}
