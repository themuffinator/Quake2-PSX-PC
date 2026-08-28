#include "aimodule.h"

#include <stdlib.h>
#include <string.h>

/* Animations on this disc run to a few dozen frames; anything much larger is a
 * false match rather than a very long move. */
#define Q2_AI_MAX_FRAMES 128

/* Turn a relocated pointer back into a module-relative offset, or return false
 * when it does not point inside this image. */
static bool to_offset(u32 pointer, u32 base, size_t size, u32 *out)
{
    if (pointer < base)
        return false;
    if ((size_t)(pointer - base) >= size)
        return false;

    *out = pointer - base;
    return true;
}

/* An ai byte is one of the six shared verbs, or the same range with the
 * module-local flag set. */
static bool verb_is_valid(u8 ai)
{
    u8 index = (u8)(ai & ~(unsigned)Q2_AI_LOCAL_FLAG);
    return index < Q2_AI_VERB_COUNT;
}

/* Does a frame array of `count` entries at `offset` decode cleanly? */
static bool frames_validate(const u8 *image, size_t size, u32 offset, u32 count)
{
    u32 i;

    if (count == 0 || count > Q2_AI_MAX_FRAMES)
        return false;
    if ((size_t)offset + (size_t)count * Q2_MFRAME_SIZE > size)
        return false;

    for (i = 0; i < count; i++) {
        q2_mframe f;
        if (!q2_mframe_read(image, size, offset + i * Q2_MFRAME_SIZE, &f))
            return false;
        if (!verb_is_valid(f.ai))
            return false;
    }
    return true;
}

static q2_ai_move *moves_push(q2_ai_moves *set)
{
    if (set->count >= set->capacity) {
        u32 want = set->capacity ? set->capacity * 2 : 32;
        q2_ai_move *bigger =
            (q2_ai_move *)realloc(set->moves, want * sizeof(q2_ai_move));
        if (!bigger)
            return NULL;
        set->moves    = bigger;
        set->capacity = want;
    }
    return &set->moves[set->count++];
}

q2_result q2_ai_moves_scan(q2_ai_moves *out, const u8 *image, size_t size,
                           u32 base)
{
    size_t at;

    if (!out || !image)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    if (size < Q2_MMOVE_SIZE)
        return Q2_OK;

    /* Move records are word-aligned, so step by four rather than by one. That
     * is a 4x reduction in false-match opportunities as well as in work. */
    for (at = 0; at + Q2_MMOVE_SIZE <= size; at += 4) {
        s32 first = q2_rd_s32(image + at + 0);
        s32 last  = q2_rd_s32(image + at + 4);
        u32 frames_ptr = q2_rd_u32(image + at + 8);
        u32 endfunc_ptr = q2_rd_u32(image + at + 12);
        u32 frames_off, endfunc_off = 0;
        u32 count;
        q2_ai_move *mv;

        if (first < 0 || last < first)
            continue;

        count = (u32)(last - first + 1);
        if (count == 0 || count > Q2_AI_MAX_FRAMES)
            continue;

        if (!to_offset(frames_ptr, base, size, &frames_off))
            continue;

        /* endfunc is either null or a real in-image pointer. */
        if (endfunc_ptr != 0 && !to_offset(endfunc_ptr, base, size, &endfunc_off))
            continue;

        /* The decisive filter: the frames the record points at must themselves
         * decode. A 16-byte window can look like a move by chance; a window
         * whose pointer also lands on a valid frame array rarely does. */
        if (!frames_validate(image, size, frames_off, count))
            continue;

        mv = moves_push(out);
        if (!mv) {
            q2_ai_moves_free(out);
            return Q2_ERR_NO_MEMORY;
        }

        mv->offset         = (u32)at;
        mv->first_frame    = first;
        mv->last_frame     = last;
        mv->frame_count    = count;
        mv->frames_offset  = frames_off;
        mv->endfunc_offset = endfunc_off;

        out->total_frames += count;
    }

    return Q2_OK;
}

void q2_ai_moves_free(q2_ai_moves *m)
{
    if (!m)
        return;
    free(m->moves);
    memset(m, 0, sizeof(*m));
}

bool q2_ai_move_frame(const q2_ai_moves *set, u32 move_index, u32 frame_index,
                      const u8 *image, size_t size, q2_mframe *out)
{
    const q2_ai_move *mv;

    if (!set || move_index >= set->count)
        return false;

    mv = &set->moves[move_index];
    if (frame_index >= mv->frame_count)
        return false;

    return q2_mframe_read(image, size,
                          mv->frames_offset + frame_index * Q2_MFRAME_SIZE,
                          out);
}

u32 q2_ai_move_verb_run(const q2_ai_moves *set, u32 move_index,
                        const u8 *image, size_t size)
{
    const q2_ai_move *mv;
    u32 i, best = 0, run = 0;
    int previous = -1;

    if (!set || move_index >= set->count)
        return 0;

    mv = &set->moves[move_index];

    for (i = 0; i < mv->frame_count; i++) {
        q2_mframe f;

        if (!q2_ai_move_frame(set, move_index, i, image, size, &f))
            break;

        if ((int)f.ai == previous) {
            run++;
        } else {
            run = 1;
            previous = (int)f.ai;
        }
        if (run > best)
            best = run;
    }

    return best;
}

/* ------------------------------------------------------------------------- */
/* Fixup-guided scan                                                          */
/* ------------------------------------------------------------------------- */

/* Try to accept a move record at `at`, appending it on success. */
static bool try_move_at(q2_ai_moves *out, const u8 *image, size_t size,
                        u32 base, size_t at)
{
    s32 first, last;
    u32 frames_ptr, endfunc_ptr, frames_off, endfunc_off = 0, count;
    q2_ai_move *mv;
    u32 i;

    if (at + Q2_MMOVE_SIZE > size)
        return false;

    first       = q2_rd_s32(image + at + 0);
    last        = q2_rd_s32(image + at + 4);
    frames_ptr  = q2_rd_u32(image + at + 8);
    endfunc_ptr = q2_rd_u32(image + at + 12);

    if (first < 0 || last < first)
        return false;

    count = (u32)(last - first + 1);
    if (count == 0 || count > Q2_AI_MAX_FRAMES)
        return false;

    if (!to_offset(frames_ptr, base, size, &frames_off))
        return false;
    if (endfunc_ptr != 0 && !to_offset(endfunc_ptr, base, size, &endfunc_off))
        return false;
    if (!frames_validate(image, size, frames_off, count))
        return false;

    /* The fixup stream can offer the same record twice when two candidate
     * pointers resolve to one location; keep it once. */
    for (i = 0; i < out->count; i++) {
        if (out->moves[i].offset == (u32)at)
            return false;
    }

    mv = moves_push(out);
    if (!mv)
        return false;

    mv->offset         = (u32)at;
    mv->first_frame    = first;
    mv->last_frame     = last;
    mv->frame_count    = count;
    mv->frames_offset  = frames_off;
    mv->endfunc_offset = endfunc_off;

    out->total_frames += count;
    return true;
}

q2_result q2_ai_moves_scan_guided(q2_ai_moves *out,
                                  const u8 *image, size_t size,
                                  const u8 *stream, size_t stream_size,
                                  u32 base)
{
    size_t at = 0;

    if (!out || !image || !stream)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    /*
     * Walk the fixup stream and, for each WORD32 site, test whether it is the
     * frames pointer of a move — i.e. whether a valid record begins eight bytes
     * earlier. Nothing else in the image is even considered.
     */
    while (at + 4 <= stream_size) {
        u32 entry = q2_rd_u32(stream + at);
        u32 offset, type;

        at += 4;

        if (entry == Q2_RELOC_TERMINATOR)
            break;

        offset = entry & ~3u;
        type   = entry & 3u;

        /* HI16 owns the following raw word; skipping it would put the walk one
         * word out of phase for everything after. */
        if (type == Q2_RELOC_HI16) {
            if (at + 4 > stream_size)
                break;
            at += 4;
            continue;
        }

        if (type != Q2_RELOC_WORD32)
            continue;

        /* A move's frames pointer sits at record + 8. */
        if (offset >= 8)
            (void)try_move_at(out, image, size, base, offset - 8);
    }

    return Q2_OK;
}
