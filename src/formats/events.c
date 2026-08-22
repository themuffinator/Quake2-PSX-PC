#include "events.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
static q2_result events_init(q2_events *out, const dat_chunk *chunk)
{
    u32 cursor, count, walked = 0, p;

    if (!out)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    if (!chunk)
        return Q2_ERR_BAD_FORMAT;

    /* The chunk is always a multiple of four, and the smallest legal one is the
     * 8-byte stub: a zero count and an immediately-terminated directory. */
    if (chunk->size < 8 || (chunk->size & 3u))
        return Q2_ERR_BAD_FORMAT;

    out->data = chunk->data;
    out->size = chunk->size;

    count = q2_rd_u32(chunk->data);
    out->record_count = count;
    out->dir_offset   = 4;

    /* The directory has no count of its own. Each entry is a 12-byte name plus
     * a u32 record offset, and the list ends with a bare u32 zero sitting where
     * the next entry's NAME would start — so the terminator test is on the
     * name, not on the offset. Testing the offset field instead happens to stop
     * in the right place on some files and overruns into record data on others,
     * which then desynchronises the whole record walk. */
    cursor = 4;
    while (cursor + Q2_EVENT_DIR_ENTRY_SIZE <= chunk->size &&
           q2_rd_u32(chunk->data + cursor) != 0) {
        cursor += Q2_EVENT_DIR_ENTRY_SIZE;
        out->dir_count++;
    }

    if (cursor + 4 > chunk->size) {
        Q2_ERROR("events: directory runs past the end of a %u-byte chunk",
                 chunk->size);
        memset(out, 0, sizeof(*out));
        return Q2_ERR_BAD_FORMAT;
    }

    out->first_record = cursor + 4;   /* skip the terminator word */

    /* Validate the whole record area up front. Walking `count` records must
     * land exactly on chunk end — it does on all 164 containers — so anything
     * else means we have misidentified the chunk and should say so now rather
     * than hand out records that happen to parse. */
    p = out->first_record;
    while (walked < count) {
        u16 size;

        if (p + Q2_EVENT_REC_HEADER_SIZE > chunk->size)
            break;

        size = q2_rd_u16(chunk->data + p);

        if (size < Q2_EVENT_REC_HEADER_SIZE || (size & 3u) ||
            (u32)size > chunk->size - p)
            break;

        p += size;
        walked++;
    }

    if (walked != count || p != chunk->size) {
        Q2_ERROR("events: walked %u of %u records and ended at 0x%X of 0x%X",
                 walked, count, p, chunk->size);
        memset(out, 0, sizeof(*out));
        return Q2_ERR_BAD_FORMAT;
    }

    return Q2_OK;
}

q2_result q2_events_parse_common(q2_events *out, const q2_common_file *f)
{
    if (!out || !f)
        return Q2_ERR_INVALID_ARG;
    return events_init(out, f->chunk[Q2_COMMON_EVENTS]);
}

q2_result q2_events_parse_zone(q2_events *out, const q2_zone_file *f)
{
    if (!out || !f)
        return Q2_ERR_INVALID_ARG;
    return events_init(out, f->chunk[Q2_ZONE_EVENTS]);
}

bool q2_events_get_dir_entry(const q2_events *e, u32 index,
                             q2_event_dir_entry *out)
{
    const u8 *p;

    if (!e || !out || index >= e->dir_count)
        return false;

    p = e->data + e->dir_offset + (size_t)index * Q2_EVENT_DIR_ENTRY_SIZE;

    memcpy(out->name, p, 12);
    out->name[12] = '\0';
    out->offset   = q2_rd_u32(p + 12);

    return true;
}

/* ------------------------------------------------------------------------- */
static bool read_record(const q2_events *e, u32 offset, q2_event_record *out)
{
    u16 size;

    if (offset + Q2_EVENT_REC_HEADER_SIZE > e->size)
        return false;

    size = q2_rd_u16(e->data + offset);

    if (size < Q2_EVENT_REC_HEADER_SIZE || (size & 3u) ||
        (u32)size > e->size - offset)
        return false;

    out->offset  = offset;
    out->size    = size;
    out->n_items = q2_rd_u8(e->data + offset + 2);
    out->flags   = q2_rd_u8(e->data + offset + 3);

    return true;
}

bool q2_events_first_record(const q2_events *e, q2_event_record *out)
{
    if (!e || !out || e->record_count == 0)
        return false;
    return read_record(e, e->first_record, out);
}

bool q2_events_next_record(const q2_events *e, const q2_event_record *prev,
                           q2_event_record *out)
{
    u32 next;

    if (!e || !prev || !out)
        return false;

    next = prev->offset + prev->size;
    if (next >= e->size)
        return false;

    return read_record(e, next, out);
}

bool q2_events_record_at(const q2_events *e, u32 offset, q2_event_record *out)
{
    q2_event_record rec;

    if (!e || !out || offset < e->first_record)
        return false;

    /* Walk rather than trust the offset. Every directory entry and every
     * TrigBounds link on the disc lands on a record start, so a caller asking
     * for an offset that does not is a caller with a bug, and returning a
     * plausible-looking record decoded from the middle of another one would
     * hide it. */
    if (!q2_events_first_record(e, &rec))
        return false;

    do {
        if (rec.offset == offset) {
            *out = rec;
            return true;
        }
        if (rec.offset > offset)
            return false;
    } while (q2_events_next_record(e, &rec, &rec));

    return false;
}

bool q2_events_get_item(const q2_events *e, const q2_event_record *rec,
                        u32 index, q2_event_item *out)
{
    u32 p, rec_end, i;

    if (!e || !rec || !out || index >= rec->n_items)
        return false;

    p       = rec->offset + Q2_EVENT_REC_HEADER_SIZE;
    rec_end = rec->offset + rec->size;

    for (i = 0; i <= index; i++) {
        u8 len;

        if (p + Q2_EVENT_ITEM_HEADER_SIZE > rec_end)
            return false;

        len = q2_rd_u8(e->data + p + 1);

        /* Validate the WHOLE item before it is handed out. Checking only that
         * the two header bytes fit would let a caller read a declared payload
         * past the end of the record. */
        if (len < Q2_EVENT_ITEM_HEADER_SIZE || (len & 3u) ||
            (u32)len > rec_end - p)
            return false;

        if (i == index) {
            out->op      = q2_rd_u8(e->data + p);
            out->opcode  = (u8)(out->op & Q2_EVOP_MASK);
            out->len     = len;
            out->offset  = p;
            out->payload = e->data + p + Q2_EVENT_ITEM_HEADER_SIZE;
            return true;
        }

        p += len;
    }

    return false;
}

/* ------------------------------------------------------------------------- */
bool q2_events_get_list(const q2_event_item *item, u32 *count_out,
                        const u8 **offsets_out)
{
    s16 count;
    u32 expect;

    if (!item)
        return false;

    if (item->opcode != Q2_EVOP_TRIGGER &&
        item->opcode != Q2_EVOP_ENABLE &&
        item->opcode != Q2_EVOP_DISABLE)
        return false;

    if (item->len < 4)
        return false;

    /* The count is signed and the engine guards it with blez, so a non-positive
     * count means an empty list rather than an error. */
    count = q2_rd_s16(item->payload);
    if (count <= 0) {
        if (count_out)   *count_out = 0;
        if (offsets_out) *offsets_out = NULL;
        return true;
    }

    /* len == 4 + 2*(count + (count & 1)); the odd entry is padding to keep the
     * item a multiple of four. Exact on all 501 list items on the disc. */
    expect = 4 + 2 * ((u32)count + ((u32)count & 1u));
    if (expect != item->len)
        return false;

    if (count_out)   *count_out = (u32)count;
    if (offsets_out) *offsets_out = item->payload + 2;

    return true;
}

u16 q2_events_list_entry(const u8 *offsets, u32 index)
{
    if (!offsets)
        return 0;
    return q2_rd_u16(offsets + (size_t)index * 2);
}

bool q2_events_get_call_index(const q2_event_item *item, u8 *index_out)
{
    if (!item || item->opcode != Q2_EVOP_CALL || item->len < 4)
        return false;

    if (index_out)
        *index_out = q2_rd_u8(item->payload);

    return true;
}

bool q2_events_get_fx_damage(const q2_event_item *item, s16 *mod_out,
                             s16 *damage_out)
{
    /* 0x80027840 rejects every size except 8 before it reads either operand.
     * The parser always supplies a payload for a valid item, but retaining the
     * guard makes this safe for callers that construct an item for a test. */
    if (!item || item->opcode != Q2_EVOP_FX || item->len != 8 ||
        !item->payload)
        return false;

    if (mod_out)
        *mod_out = q2_rd_s16(item->payload);
    if (damage_out)
        *damage_out = q2_rd_s16(item->payload + 2);

    return true;
}
