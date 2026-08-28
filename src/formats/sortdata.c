#include "sortdata.h"

#include <string.h>

q2_result q2_sortdata_parse(q2_sortdata *out, const q2_zone_file *zone)
{
    const dat_chunk *c;

    if (!out || !zone)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    c = zone->chunk[Q2_ZONE_SORT_DATA];
    if (!c)
        return Q2_ERR_BAD_FORMAT;

    out->data = c->data;
    out->size = c->size;
    return Q2_OK;
}

/* ------------------------------------------------------------------------- */
/* The bit reader                                                             */
/*                                                                            */
/* Transcribed from the seven inlined copies at 0x80066B70 onwards. The mask   */
/* table at 0x8009FBF0 is (1 << n) - 1 at stride 4, so it is computed here     */
/* rather than transcribed — the dump confirms it up to n = 16, which is the   */
/* widest field the header can declare.                                        */
/* ------------------------------------------------------------------------- */
static u32 mask_of(u32 n)
{
    return n >= 32 ? 0xFFFFFFFFu : ((1u << n) - 1u);
}

/* Load the word at `index`, or flag an overrun and hand back zero. The engine
 * has no such guard: it would simply read past the chunk. A port that did the
 * same would turn a misdecode into a wild read, so the condition is surfaced. */
static u32 fetch(q2_sort_reader *r, u32 index)
{
    const u8 *p;

    if ((index + 1u) * 4u > r->size) {
        r->overrun = true;
        return 0;
    }

    p = r->data + (size_t)index * 4u;
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u32 read_bits(q2_sort_reader *r, u32 n)
{
    u32 v;

    if (n == 0 || r->overrun)
        return 0;

    if (r->bits_left >= n) {
        v = (r->word >> (32u - r->bits_left)) & mask_of(n);
        r->bits_left -= n;
        return v;
    }

    /* Straddles a word boundary: take what is left, refill, take the rest.
     * `bits_left == 0` is a real state and must not shift by 32. */
    v = r->bits_left ? (r->word >> (32u - r->bits_left)) : 0u;
    {
        u32 got = r->bits_left;
        u32 need = n - got;

        r->word = fetch(r, r->next_word++);
        v |= (r->word & mask_of(need)) << got;
        r->bits_left = 32u - need;
    }
    return v;
}

/* Advance `n` bits without producing a value — the entity record's skip, which
 * uses the word-stepping arithmetic at 0x800675A8 so it can cross any number of
 * words rather than just one. */
static void skip_bits(q2_sort_reader *r, u32 n)
{
    s32 over;

    if (r->overrun)
        return;

    over = (s32)n - (s32)r->bits_left;
    if (over < 0) {
        r->bits_left -= n;
        return;
    }

    r->next_word += (u32)(over >> 5);
    r->word       = fetch(r, r->next_word++);
    r->bits_left  = 32u - (u32)(over & 31);
}

u32 q2_sort_bit_position(const q2_sort_reader *r)
{
    if (!r)
        return 0;
    return r->next_word * 32u - r->bits_left;
}

/* ------------------------------------------------------------------------- */
bool q2_sort_begin(q2_sort_reader *r, const q2_sortdata *sd,
                   u32 byte_offset, u32 bucket0)
{
    u32 rem;

    if (!r || !sd || !sd->data)
        return false;

    memset(r, 0, sizeof(*r));
    r->data = sd->data;
    r->size = sd->size;

    /*
     * 0x80066B00: the offset is in BYTES, word-aligned down, and the remainder
     * is consumed as bits. That is what lets several viewports' streams share
     * one chunk without any alignment padding between them.
     */
    if (byte_offset >= sd->size)
        return false;

    rem          = byte_offset & 3u;
    r->next_word = byte_offset >> 2;
    r->word      = fetch(r, r->next_word++);
    r->bits_left = 32u - 8u * rem;

    if (r->overrun)
        return false;

    /* The seven widths, each stored as width - 1, in the order the `slti`
     * bounds give them. */
    r->hdr.w_base     = (u8)(read_bits(r, 4) + 1u);
    r->hdr.w_op_short = (u8)(read_bits(r, 3) + 1u);
    r->hdr.w_op_long  = (u8)(read_bits(r, 4) + 1u);
    r->hdr.w_f1       = (u8)(read_bits(r, 3) + 1u);
    r->hdr.w_f3       = (u8)(read_bits(r, 3) + 1u);
    r->hdr.w_f4       = (u8)(read_bits(r, 3) + 1u);
    r->hdr.w_f2       = (u8)(read_bits(r, 4) + 1u);
    r->hdr.base       = read_bits(r, r->hdr.w_base);

    r->bucket   = bucket0;
    r->windowed = true;     /* the sp+128 latch starts at zero */

    return !r->overrun;
}

void q2_sort_entity_resolve(q2_sort_reader *r, bool drawn)
{
    if (!r || !r->pending_entity)
        return;

    r->pending_entity = false;

    /*
     * 0x800674D8. A projected rectangle with no area takes the skip arm; an
     * accepted rectangle takes the continuation arm, where non-zero f2 means
     * that a new windowed-mode base follows.
     */
    if (!drawn)
        skip_bits(r, r->pending_f2);
    else if (r->pending_f2 != 0)
        r->hdr.base = read_bits(r, r->hdr.w_base);

    /* 0x800675E0: the one place the bucket moves. */
    if (r->bucket == 0)
        r->ended = true;
    else
        r->bucket--;

    if ((s32)r->bucket < Q2_SORT_BUCKET_FLOOR)
        r->ended = true;
}

bool q2_sort_next(q2_sort_reader *r, q2_sort_item *out)
{
    u32 op;

    if (!r || !out || r->ended || r->overrun || r->pending_entity)
        return false;

    memset(out, 0, sizeof(*out));

    op = read_bits(r, r->windowed ? r->hdr.w_op_short : r->hdr.w_op_long);

    /*
     * Opcode 2 switches encoding mode and carries the replacement opcode with
     * it, read at the mode it is switching TO (0x8006718C and 0x80067604 are
     * the two directions). Only one switch can be pending at a time — the
     * replacement is dispatched directly, never re-tested for 2 — so this is a
     * single `if`, not a loop.
     */
    if (op == 2) {
        r->windowed = !r->windowed;
        op = read_bits(r, r->windowed ? r->hdr.w_op_short : r->hdr.w_op_long);
    }

    if (r->overrun)
        return false;

    if (op == 0) {
        r->ended  = true;
        out->kind = Q2_SORT_END;
        return false;
    }

    if (op == 1) {
        /* Field order in the stream is f1, f2, f3, f4 — the widths are read in
         * a different order in the header, which is why they are named rather
         * than indexed. */
        out->kind   = Q2_SORT_ENTITY;
        out->f1     = (s32)(s16)(u16)read_bits(r, r->hdr.w_f1);
        out->f2     = read_bits(r, r->hdr.w_f2);
        out->f3     = read_bits(r, r->hdr.w_f3) & 0xFFu;
        out->f4     = read_bits(r, r->hdr.w_f4) & 0xFFu;
        out->bucket = r->bucket;

        r->pending_entity = true;
        r->pending_f2     = out->f2;
        return !r->overrun;
    }

    /* op >= 3: a scene node, offset by the base while in windowed mode. */
    out->kind   = Q2_SORT_NODE;
    out->node   = (r->windowed ? op + r->hdr.base : op) - 3u;
    out->bucket = r->bucket;

    /* 0x80067B28: the node path stops the whole stream once the bucket has
     * fallen below the floor, rather than clamping. */
    if ((s32)r->bucket < Q2_SORT_BUCKET_FLOOR)
        r->ended = true;

    return true;
}

/* ------------------------------------------------------------------------- */
u32 q2_sortdata_enumerate(const q2_sortdata *sd, u32 *offsets, u32 max)
{
    u32 offset = 0, count = 0;

    if (!sd || !sd->data || sd->size < 8)
        return 0;

    while (offset < sd->size) {
        q2_sort_reader r;
        q2_sort_item it;
        u32 guard = 0;
        u32 next;

        if (!q2_sort_begin(&r, sd, offset, Q2_SORT_BUCKET_START))
            break;

        if (offsets && count < max)
            offsets[count] = offset;
        count++;

        while (q2_sort_next(&r, &it)) {
            if (++guard > 1000000u)
                break;
            if (it.kind == Q2_SORT_ENTITY)
                q2_sort_entity_resolve(&r, false);
        }

        /* An overrun means the tail is not another stream; stop rather than
         * report offsets that decode to nothing. */
        if (r.overrun)
            break;

        next = (q2_sort_bit_position(&r) + 7u) / 8u;
        if (next <= offset)
            break;          /* no forward progress; refuse to spin */
        offset = next;
    }

    return count;
}

bool q2_sortdata_stream_offset(const q2_sortdata *sd, u32 index, u32 *out)
{
    u32 offset = 0, i = 0;

    if (!sd || !sd->data || sd->size < 8)
        return false;

    while (offset < sd->size) {
        q2_sort_reader r;
        q2_sort_item it;
        u32 guard = 0;
        u32 next;

        if (!q2_sort_begin(&r, sd, offset, Q2_SORT_BUCKET_START))
            return false;

        if (i == index) {
            if (out)
                *out = offset;
            return true;
        }

        while (q2_sort_next(&r, &it)) {
            if (++guard > 1000000u)
                break;
            if (it.kind == Q2_SORT_ENTITY)
                q2_sort_entity_resolve(&r, false);
        }

        if (r.overrun)
            return false;

        next = (q2_sort_bit_position(&r) + 7u) / 8u;
        if (next <= offset)
            return false;
        offset = next;
        i++;
    }

    return false;
}
