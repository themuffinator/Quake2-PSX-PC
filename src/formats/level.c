#include "level.h"

#include <string.h>

const char *const q2_common_chunk_names[Q2_COMMON_CHUNK_COUNT] = {
    "CreAIRel", "LevelRel", "Resources", "CastList", "TrigBounds",
    "Lights", "ModelNames", "StartPos", "Population", "Strings",
    "CreAIBin", "LevelBin", "UserFuncs", "Events", "GlintMod"
};

const char *const q2_zone_chunk_names[Q2_ZONE_CHUNK_COUNT] = {
    "Events", "Scene", "CastList", "MapNames", "SpaceLights",
    "SortData", "MapMod", "Points", "PrimaryColl", "SecondaryCol",
    "PrimaryRemap", "AreaConx", "CreAIRel", "CreAIBin",
    "TriggerRemap", "SecondaryRem"
};

/* The optional chunks, which the loader tolerates being absent. Everything else
 * is mandatory; a file missing one is not a build we understand. */
static bool common_chunk_is_optional(int index)
{
    return index == Q2_COMMON_GLINT_MOD;
}

static bool zone_chunk_is_optional(int index)
{
    return index == Q2_ZONE_CRE_AI_REL ||
           index == Q2_ZONE_CRE_AI_BIN ||
           /* Neither is on this disc; the executable knows both names, so a
            * build that ships one should open rather than be refused (#33). */
           index == Q2_ZONE_TRIGGER_REMAP ||
           index == Q2_ZONE_SECONDARY_REM;
}

static int lookup(const char *const *names, int count, const char *name)
{
    int i;

    if (!name)
        return -1;

    for (i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0)
            return i;
    }
    return -1;
}

int q2_common_chunk_index(const char *name)
{
    return lookup(q2_common_chunk_names, Q2_COMMON_CHUNK_COUNT, name);
}

int q2_zone_chunk_index(const char *name)
{
    return lookup(q2_zone_chunk_names, Q2_ZONE_CHUNK_COUNT, name);
}

/* ------------------------------------------------------------------------- */
/* Resolution                                                                 */
/* ------------------------------------------------------------------------- */
static q2_result resolve(dat_archive *ar,
                         const dat_chunk **slots,
                         int slot_count,
                         const char *const *names,
                         bool (*is_optional)(int),
                         const char *what)
{
    int i;

    memset(slots, 0, (size_t)slot_count * sizeof(*slots));

    for (i = 0; i < ar->chunk_count; i++) {
        int index = lookup(names, slot_count, ar->chunks[i].name);

        if (index < 0) {
            /* An unknown chunk name means this build's schema differs from the
             * one we catalogued. Refusing here beats misreading it later. */
            Q2_ERROR("%s: unknown chunk '%s' — unsupported build?",
                     what, ar->chunks[i].name);
            return Q2_ERR_UNSUPPORTED;
        }
        slots[index] = &ar->chunks[i];
    }

    for (i = 0; i < slot_count; i++) {
        if (!slots[i] && !is_optional(i)) {
            Q2_ERROR("%s: missing mandatory chunk '%s'", what, names[i]);
            return Q2_ERR_BAD_FORMAT;
        }
    }

    return Q2_OK;
}

q2_result q2_common_open(q2_common_file *out, q2_buf *buf)
{
    q2_result r;

    if (!out || !buf)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    r = dat_open_buf(&out->ar, buf);
    if (r != Q2_OK)
        return r;

    r = resolve(&out->ar, out->chunk, Q2_COMMON_CHUNK_COUNT,
                q2_common_chunk_names, common_chunk_is_optional, "COMMON.DAT");
    if (r != Q2_OK) {
        dat_close(&out->ar);
        return r;
    }

    return Q2_OK;
}

q2_result q2_zone_open(q2_zone_file *out, q2_buf *buf)
{
    q2_result r;

    if (!out || !buf)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    r = dat_open_buf(&out->ar, buf);
    if (r != Q2_OK)
        return r;

    r = resolve(&out->ar, out->chunk, Q2_ZONE_CHUNK_COUNT,
                q2_zone_chunk_names, zone_chunk_is_optional, "ZONE.DAT");
    if (r != Q2_OK) {
        dat_close(&out->ar);
        return r;
    }

    return Q2_OK;
}

q2_result q2_common_move(q2_common_file *dst, q2_common_file *src)
{
    q2_result r;

    if (!dst || !src)
        return Q2_ERR_INVALID_ARG;
    if (dst == src)
        return Q2_OK;

    dst->ar = src->ar;          /* the buffer and the inline directory */

    r = resolve(&dst->ar, dst->chunk, Q2_COMMON_CHUNK_COUNT,
                q2_common_chunk_names, common_chunk_is_optional, "COMMON.DAT");
    if (r != Q2_OK) {
        dat_close(&dst->ar);
        memset(src, 0, sizeof(*src));
        return r;
    }

    /* The source no longer owns the buffer, and must not close it. */
    memset(src, 0, sizeof(*src));
    return Q2_OK;
}

q2_result q2_zone_move(q2_zone_file *dst, q2_zone_file *src)
{
    q2_result r;

    if (!dst || !src)
        return Q2_ERR_INVALID_ARG;
    if (dst == src)
        return Q2_OK;

    dst->ar = src->ar;

    r = resolve(&dst->ar, dst->chunk, Q2_ZONE_CHUNK_COUNT,
                q2_zone_chunk_names, zone_chunk_is_optional, "ZONE.DAT");
    if (r != Q2_OK) {
        dat_close(&dst->ar);
        memset(src, 0, sizeof(*src));
        return r;
    }

    memset(src, 0, sizeof(*src));
    return Q2_OK;
}

void q2_common_close(q2_common_file *f)
{
    if (!f)
        return;
    dat_close(&f->ar);
    memset(f, 0, sizeof(*f));
}

void q2_zone_close(q2_zone_file *f)
{
    if (!f)
        return;
    dat_close(&f->ar);
    memset(f, 0, sizeof(*f));
}

bool q2_common_resources(const q2_common_file *f, const char *zone_name,
                         q2_zone_resources *out)
{
    const dat_chunk *chunk = f ? f->chunk[Q2_COMMON_RESOURCES] : NULL;
    const char *name = zone_name ? strrchr(zone_name, '/') : NULL;
    u32 at;
    name = name ? name + 1 : zone_name;
    if (!out || !name || !chunk || !chunk->data || chunk->size < 32) return false;
    for (at = 0; at <= chunk->size - 32 && chunk->data[at]; at += 32) {
        const u8 *raw = chunk->data + at;
        u32 j;
        for (j = 0; j < 12; j++) {
            int a = raw[j], b = (unsigned char)name[j];
            if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
            if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
            if (a != b || !a) break;
        }
        if ((j < 12 && raw[j] == 0 && name[j] == 0) ||
            (j == 12 && name[j] == 0)) {
            out->packet_control = q2_rd_s32(raw + 12);
            out->particle_groups = q2_rd_s32(raw + 24);
            out->subdiv_meshes = q2_rd_s32(raw + 28);
            return true;
        }
    }
    return false;
}
