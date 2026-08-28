#include "population.h"

#include <ctype.h>
#include <string.h>

q2_result q2_population_parse(q2_population *out, const q2_common_file *common)
{
    const dat_chunk *chunk;
    u32 offset = 0, count = 0;

    if (!out || !common)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    chunk = common->chunk[Q2_COMMON_POPULATION];
    if (!chunk || chunk->size < 4)
        return Q2_ERR_BAD_FORMAT;

    /*
     * The group table is terminated by FOUR zero bytes, not by a zeroed
     * 24-byte record. That distinction matters more than it looks: the
     * terminator sits where the next record's name would start, and the first
     * list usually begins only 4 bytes later. Requiring a full zero record
     * therefore walks straight into the list data and reads it as groups —
     * on BASE0 that turns 2 real groups into 26, and the 24 spurious ones carry
     * garbage offsets that then produce tens of thousands of phantom actors.
     *
     * An 8-byte chunk is a real, empty population rather than an error.
     */
    while (offset + 4 <= chunk->size) {
        if (q2_rd_u32(chunk->data + offset) == 0)
            break;
        if (offset + Q2_POP_GROUP_SIZE > chunk->size)
            break;
        count++;
        offset += Q2_POP_GROUP_SIZE;
    }

    out->data        = chunk->data;
    out->size        = chunk->size;
    out->group_count = count;

    return Q2_OK;
}

bool q2_pop_get_group(const q2_population *p, u32 index, q2_pop_group *out)
{
    const u8 *rec;

    if (!p || !out || index >= p->group_count)
        return false;

    rec = p->data + (size_t)index * Q2_POP_GROUP_SIZE;

    memcpy(out->name, rec, 12);
    out->name[12] = '\0';

    out->spawn_offset = q2_rd_u32(rec + 0x0C);
    out->place_offset = q2_rd_u32(rec + 0x10);

    return true;
}

bool q2_pop_group_is_path(const q2_pop_group *g)
{
    if (!g)
        return false;

    /* Selected by name because nothing in the records distinguishes the two
     * 24-byte layouts. Matched case-insensitively on the prefix so a build that
     * spells it differently still resolves. */
    {
        const char *n = g->name;
        char upper[13];
        int i;

        for (i = 0; i < 12 && n[i]; i++)
            upper[i] = (char)toupper((unsigned char)n[i]);
        upper[i] = '\0';

        return strstr(upper, "PATH") != NULL || strstr(upper, "CORNER") != NULL;
    }
}

int q2_pop_group_zone(const q2_pop_group *g)
{
    const char *n;
    int zone = 0, digits = 0;

    if (!g)
        return -1;

    n = g->name;
    if (toupper((unsigned char)n[0]) != 'Z' ||
        toupper((unsigned char)n[1]) != 'O' ||
        toupper((unsigned char)n[2]) != 'N' ||
        toupper((unsigned char)n[3]) != 'E')
        return -1;

    /* Only the run of digits straight after the word. A suffix beyond them is a
     * batch name within that zone and is deliberately ignored. */
    while (digits < 4 && n[4 + digits] >= '0' && n[4 + digits] <= '9') {
        zone = zone * 10 + (n[4 + digits] - '0');
        digits++;
    }

    return digits ? zone : -1;
}

/* Common bounds check for a record inside a list. */
static const u8 *list_record(const q2_population *p, u32 list_offset,
                             u32 slot, u32 stride)
{
    size_t at;

    if (!p || list_offset == 0)
        return NULL;

    at = (size_t)list_offset + (size_t)slot * stride;
    if (at + stride > p->size)
        return NULL;

    return p->data + at;
}

bool q2_pop_get_spawn(const q2_population *p, const q2_pop_group *g,
                      u32 slot, q2_pop_spawn *out)
{
    const u8 *rec;

    if (!g || !out || q2_pop_group_is_path(g))
        return false;

    rec = list_record(p, g->spawn_offset, slot, Q2_POP_SPAWN_SIZE);
    if (!rec)
        return false;

    /*
     * Terminated by a single zero word — NOT 0xFFFFFFFF, which is what the
     * place and path lists use, and NOT a doubly-zero record. Requiring two
     * zero words reads 7,901 actors disc-wide instead of 651, because it sails
     * past the terminator into whatever follows.
     *
     * The terminator being class_id == 0 also means class_id 0 never appears as
     * real data, despite the observed range nominally starting at 0.
     */
    if (q2_rd_u32(rec) == 0)
        return false;

    out->class_id = q2_rd_u32(rec + 0x00);
    out->x        = q2_rd_s32(rec + 0x04);
    out->y        = q2_rd_s32(rec + 0x08);
    out->z        = q2_rd_s32(rec + 0x0C);
    out->angle    = q2_rd_u16(rec + 0x10);
    out->link     = q2_rd_u16(rec + 0x12);
    out->flags    = q2_rd_u16(rec + 0x14);
    out->index    = q2_rd_u16(rec + 0x16);

    return true;
}

bool q2_pop_get_path(const q2_population *p, const q2_pop_group *g,
                     u32 slot, q2_pop_path *out)
{
    const u8 *rec;

    if (!g || !out || !q2_pop_group_is_path(g))
        return false;

    rec = list_record(p, g->spawn_offset, slot, Q2_POP_PATH_SIZE);
    if (!rec)
        return false;

    /* Terminated by 0xFFFFFFFF. A zero test would stop early on any node whose
     * X happens to be zero, which is a plausible coordinate. */
    if (q2_rd_u32(rec) == Q2_POP_TERM_FFFF)
        return false;

    /* xyz sits at +0x00 here, unlike a spawn record where it is at +0x04. */
    out->x     = q2_rd_s32(rec + 0x00);
    out->y     = q2_rd_s32(rec + 0x04);
    out->z     = q2_rd_s32(rec + 0x08);
    out->unk0  = q2_rd_u16(rec + 0x0C);
    out->link0 = q2_rd_u32(rec + 0x10);
    out->link1 = q2_rd_u32(rec + 0x14);

    return true;
}

bool q2_pop_get_place(const q2_population *p, const q2_pop_group *g,
                      u32 slot, q2_pop_place *out)
{
    const u8 *rec;

    if (!g || !out)
        return false;

    rec = list_record(p, g->place_offset, slot, Q2_POP_PLACE_SIZE);
    if (!rec)
        return false;

    if (q2_rd_u32(rec) == Q2_POP_TERM_FFFF)
        return false;

    out->x   = q2_rd_s32(rec + 0x00);
    out->y   = q2_rd_s32(rec + 0x04);
    out->z   = q2_rd_s32(rec + 0x08);
    out->angle_flags = q2_rd_u16(rec + 0x0C);
    out->id          = q2_rd_u16(rec + 0x0E);

    return true;
}

bool q2_pop_place_allows_skill(u16 angle_flags, s32 skill)
{
    switch (skill) {
    case 0:  return (angle_flags & Q2_POP_PLACE_NOT_EASY) == 0;
    case 1:  return (angle_flags & Q2_POP_PLACE_NOT_MEDIUM) == 0;
    case 2:  return (angle_flags & Q2_POP_PLACE_NOT_HARD) == 0;
    default: return true;
    }
}
