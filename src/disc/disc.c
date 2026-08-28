#include "disc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISC_MAX_FILES   4096
#define DISC_MAX_BACKING 8

#if defined(_MSC_VER)
#  define q2_strtok_r strtok_s
#else
#  define q2_strtok_r strtok_r
#endif

/* ------------------------------------------------------------------------- */
/* Internal state                                                             */
/* ------------------------------------------------------------------------- */
typedef struct backing_file {
    FILE *fp;
    char  path[512];
    u64   size;
} backing_file;

struct disc {
    backing_file backing[DISC_MAX_BACKING];
    int          backing_count;

    cd_track     tracks[CD_MAX_TRACKS];
    int          track_count;
    int          data_track;      /* index into tracks[] */

    disc_file   *files;
    int          file_count;

    char         description[512];
    char         volume_id[33];
    char         system_id[33];
    char         creation_time[18];
    u32          volume_sectors;
};

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */
static void str_copy(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (!cap)
        return;
    n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void str_trim(char *s)
{
    size_t len = strlen(s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                   s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = '\0';
}

static int ascii_casecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = toupper((unsigned char)*a);
        int cb = toupper((unsigned char)*b);
        if (ca != cb)
            return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static bool str_starts_with_ci(const char *s, const char *prefix)
{
    while (*prefix) {
        if (toupper((unsigned char)*s) != toupper((unsigned char)*prefix))
            return false;
        s++;
        prefix++;
    }
    return true;
}

static const char *path_extension(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *s1 = strrchr(path, '/');
    const char *s2 = strrchr(path, '\\');
    const char *slash = s1 > s2 ? s1 : s2;

    if (!dot || (slash && dot < slash))
        return "";
    return dot + 1;
}

/* Resolve `rel` against the directory containing `base`. */
static void path_resolve_sibling(char *out, size_t cap, const char *base, const char *rel)
{
    const char *s1 = strrchr(base, '/');
    const char *s2 = strrchr(base, '\\');
    const char *slash = s1 > s2 ? s1 : s2;
    size_t dirlen;

    if (rel[0] == '/' || rel[0] == '\\' || (rel[0] && rel[1] == ':')) {
        str_copy(out, cap, rel);
        return;
    }
    if (!slash) {
        str_copy(out, cap, rel);
        return;
    }

    dirlen = (size_t)(slash - base) + 1;
    if (dirlen >= cap)
        dirlen = cap - 1;
    memcpy(out, base, dirlen);
    out[dirlen] = '\0';
    strncat(out, rel, cap - strlen(out) - 1);
}

static u64 file_size_of(FILE *fp)
{
    u64 size;
#if defined(_WIN32)
    _fseeki64(fp, 0, SEEK_END);
    size = (u64)_ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);
#else
    fseeko(fp, 0, SEEK_END);
    size = (u64)ftello(fp);
    fseeko(fp, 0, SEEK_SET);
#endif
    return size;
}

static int file_seek(FILE *fp, u64 offset)
{
#if defined(_WIN32)
    return _fseeki64(fp, (long long)offset, SEEK_SET);
#else
    return fseeko(fp, (off_t)offset, SEEK_SET);
#endif
}

static int disc_add_backing(disc *d, const char *path)
{
    backing_file *bf;
    FILE *fp;

    if (d->backing_count >= DISC_MAX_BACKING)
        return -1;

    fp = fopen(path, "rb");
    if (!fp)
        return -1;

    bf = &d->backing[d->backing_count];
    bf->fp = fp;
    str_copy(bf->path, sizeof(bf->path), path);
    bf->size = file_size_of(fp);

    return d->backing_count++;
}

/* Return an opened disc object to its allocation-only state.  In particular,
 * this is used when a sibling CUE was found but could not be completed: the
 * parser may already have opened one or more FILE entries before a later one
 * fails, and erasing the structure without closing them leaks those handles. */
static void disc_reset_loaded(disc *d)
{
    int i;

    if (!d)
        return;

    for (i = 0; i < d->backing_count; i++) {
        if (d->backing[i].fp)
            fclose(d->backing[i].fp);
    }
    free(d->files);
    memset(d, 0, sizeof(*d));
}

/* ------------------------------------------------------------------------- */
/* Sector reading                                                             */
/* ------------------------------------------------------------------------- */
static const cd_track *disc_track_for_lba(const disc *d, u32 lba)
{
    int i;

    /* Prefer INDEX 01 ranges. In a one-FILE mixed-mode image the previous
     * track traditionally owns the same physical sectors as the next track's
     * INDEX 00 range, and preserving that choice keeps existing addressing
     * unchanged. */
    for (i = 0; i < d->track_count; i++) {
        const cd_track *t = &d->tracks[i];
        if (lba >= t->start_lba && lba < t->start_lba + t->length_sectors)
            return t;
    }

    /* A separately backed track can expose a physically stored INDEX 00 gap
     * which no preceding INDEX 01 range covers. It belongs to the upcoming
     * track and must remain raw-readable (CD players may seek or scan it). */
    for (i = 0; i < d->track_count; i++) {
        const cd_track *t = &d->tracks[i];
        if (t->pregap_lba != UINT32_MAX &&
            lba >= t->pregap_lba && lba < t->start_lba)
            return t;
    }
    /* Images often declare a volume larger than the physical data track (the
     * padding files at the end of a PSX disc do exactly this). Resolving those
     * against the data track lets callers get a clean range error instead of a
     * confusing "not found". */
    return d->track_count ? &d->tracks[d->data_track] : NULL;
}

q2_result disc_read_raw_sector(const disc *d, u32 lba, u8 *out)
{
    const cd_track *t;
    const backing_file *bf;
    u64 offset;

    if (!d || !out)
        return Q2_ERR_INVALID_ARG;

    t = disc_track_for_lba(d, lba);
    if (!t)
        return Q2_ERR_NOT_FOUND;
    if (t->file_index < 0 || t->file_index >= d->backing_count)
        return Q2_ERR_IO;

    bf = &d->backing[t->file_index];
    if (lba < t->start_lba) {
        u64 before = (u64)(t->start_lba - lba) * (u64)t->sector_size;

        if (before > t->file_offset)
            return Q2_ERR_RANGE;
        offset = t->file_offset - before;
    } else {
        offset = t->file_offset +
                 (u64)(lba - t->start_lba) * (u64)t->sector_size;
    }

    if (offset + (u64)t->sector_size > bf->size)
        return Q2_ERR_RANGE;
    if (file_seek(bf->fp, offset) != 0)
        return Q2_ERR_IO;

    if (t->sector_size == CD_SECTOR_RAW) {
        if (fread(out, 1, CD_SECTOR_RAW, bf->fp) != CD_SECTOR_RAW)
            return Q2_ERR_IO;
        return Q2_OK;
    }

    /* Cooked image: synthesise a Mode 2 Form 1 sector so callers that inspect
     * the subheader still see something coherent. */
    memset(out, 0, CD_SECTOR_RAW);
    memset(out + 1, 0xFF, 10);
    out[15] = 0x02;                              /* mode 2      */
    out[18] = out[22] = (u8)CD_SUBMODE_DATA;     /* submode     */

    if (fread(out + 24, 1, CD_SECTOR_MODE1, bf->fp) != CD_SECTOR_MODE1)
        return Q2_ERR_IO;

    return Q2_OK;
}

q2_result disc_read_sector_payload(const disc *d, u32 lba, u8 *out, u32 *out_len)
{
    u8 raw[CD_SECTOR_RAW];
    q2_result r;

    if (!out)
        return Q2_ERR_INVALID_ARG;

    r = disc_read_raw_sector(d, lba, raw);
    if (r != Q2_OK)
        return r;

    if (raw[15] == 2) {
        if (raw[18] & CD_SUBMODE_FORM2) {
            memcpy(out, raw + 24, CD_SECTOR_FORM2);
            if (out_len) *out_len = CD_SECTOR_FORM2;
        } else {
            memcpy(out, raw + 24, CD_SECTOR_FORM1);
            if (out_len) *out_len = CD_SECTOR_FORM1;
        }
    } else {
        memcpy(out, raw + 16, CD_SECTOR_MODE1);
        if (out_len) *out_len = CD_SECTOR_MODE1;
    }

    return Q2_OK;
}

/* Read a run of consecutive Form 1 / Mode 1 sectors' user data. */
static q2_result disc_read_form1_run(const disc *d, u32 lba, u32 count, u8 *out)
{
    u32 i;

    for (i = 0; i < count; i++) {
        u8 raw[CD_SECTOR_RAW];
        q2_result r = disc_read_raw_sector(d, lba + i, raw);
        if (r != Q2_OK)
            return r;
        if (raw[15] == 2)
            memcpy(out + (size_t)i * CD_SECTOR_FORM1, raw + 24, CD_SECTOR_FORM1);
        else
            memcpy(out + (size_t)i * CD_SECTOR_FORM1, raw + 16, CD_SECTOR_MODE1);
    }
    return Q2_OK;
}

/* Is the sector at `lba` marked Form 2? Used to pick a read strategy per file. */
static bool disc_sector_is_form2(const disc *d, u32 lba)
{
    u8 raw[CD_SECTOR_RAW];

    if (disc_read_raw_sector(d, lba, raw) != Q2_OK)
        return false;
    return raw[15] == 2 && (raw[18] & CD_SUBMODE_FORM2) != 0;
}

/* ------------------------------------------------------------------------- */
/* CUE sheet parsing                                                          */
/* ------------------------------------------------------------------------- */
static u32 msf_to_lba(int m, int s, int f)
{
    return (u32)((m * 60 + s) * CD_SECTORS_PER_SECOND + f);
}

static int cue_sector_size_for_mode(const char *mode)
{
    if (str_starts_with_ci(mode, "MODE1/2352")) return CD_SECTOR_RAW;
    if (str_starts_with_ci(mode, "MODE2/2352")) return CD_SECTOR_RAW;
    if (str_starts_with_ci(mode, "MODE2/2336")) return 2336;
    if (str_starts_with_ci(mode, "MODE1/2048")) return CD_SECTOR_MODE1;
    if (str_starts_with_ci(mode, "AUDIO"))      return CD_SECTOR_RAW;
    return CD_SECTOR_RAW;
}

/*
 * Turn the INDEX positions parsed from a CUE sheet into the two coordinate
 * systems the rest of the disc layer needs:
 *
 *   file_offset  byte position within the track's backing FILE
 *   start_lba    absolute sector position on the complete disc
 *
 * INDEX positions restart at zero for every FILE.  A one-FILE sheet needs no
 * rebasing; retain its established layout exactly.  With multiple FILEs, each
 * backing starts after all sectors in the preceding backing, while INDEX 00/01
 * remain offsets into the current file.  This matters for the common layout in
 * which a separate audio file contains its own 150-sector INDEX 00 pregap.
 */
static q2_result cue_layout_tracks(disc *d)
{
    int i;

    if (d->backing_count == 1) {
        /* Existing single-file behaviour: offsets are measured from the first
         * track represented by the shared backing file. */
        for (i = 0; i < d->track_count; i++) {
            cd_track *t = &d->tracks[i];
            u32 base_lba = t->start_lba;
            int j;

            for (j = 0; j < i; j++) {
                if (d->tracks[j].file_index == t->file_index) {
                    base_lba = d->tracks[j].pregap_lba != UINT32_MAX
                             ? d->tracks[j].pregap_lba
                             : d->tracks[j].start_lba;
                    break;
                }
            }

            if (t->start_lba < base_lba)
                return Q2_ERR_BAD_FORMAT;

            t->file_offset = (u64)(t->start_lba - base_lba) *
                             (u64)t->sector_size;

            if (t->file_index >= 0 && t->file_index < d->backing_count) {
                const u64 size = d->backing[t->file_index].size;

                if (t->file_offset > size)
                    return Q2_ERR_BAD_FORMAT;
                t->length_sectors = (u32)((size - t->file_offset) /
                                          (u64)t->sector_size);
            }
        }
    } else {
        u64 file_base = 0;
        int file_index;

        for (file_index = 0; file_index < d->backing_count; file_index++) {
            const backing_file *bf = &d->backing[file_index];
            u64 file_sectors;
            int sector_size = 0;

            /* All tracks sharing a binary file necessarily share its physical
             * sector size.  Besides rejecting an ambiguous layout, finding it
             * here lets the complete size of this FILE advance the next one's
             * absolute base. */
            for (i = 0; i < d->track_count; i++) {
                const cd_track *t = &d->tracks[i];

                if (t->file_index != file_index)
                    continue;
                if (sector_size == 0)
                    sector_size = t->sector_size;
                else if (sector_size != t->sector_size)
                    return Q2_ERR_BAD_FORMAT;
            }

            if (sector_size == 0)
                return Q2_ERR_BAD_FORMAT;

            file_sectors = bf->size / (u64)sector_size;
            if (file_sectors > UINT32_MAX ||
                file_base + file_sectors > UINT32_MAX)
                return Q2_ERR_RANGE;

            for (i = 0; i < d->track_count; i++) {
                cd_track *t = &d->tracks[i];
                u32 local_start;
                u32 local_pregap;

                if (t->file_index != file_index)
                    continue;

                local_start  = t->start_lba;
                local_pregap = t->pregap_lba;
                if ((u64)local_start > file_sectors ||
                    (local_pregap != UINT32_MAX &&
                     (u64)local_pregap > file_sectors))
                    return Q2_ERR_BAD_FORMAT;

                t->file_offset = (u64)local_start * (u64)sector_size;
                t->start_lba = (u32)(file_base + local_start);
                if (local_pregap != UINT32_MAX)
                    t->pregap_lba = (u32)(file_base + local_pregap);
                t->length_sectors = (u32)(file_sectors - local_start);
            }

            file_base += file_sectors;
        }
    }

    /* Stop a track where the next INDEX 01 in the same FILE begins.  Keeping
     * this rule unchanged preserves the treatment of an in-file pregap in
     * existing single-file images. */
    for (i = 0; i + 1 < d->track_count; i++) {
        cd_track *t = &d->tracks[i];
        const cd_track *n = &d->tracks[i + 1];

        if (t->file_index == n->file_index && n->start_lba > t->start_lba) {
            u32 len = n->start_lba - t->start_lba;
            if (len < t->length_sectors)
                t->length_sectors = len;
        }
    }

    return Q2_OK;
}

static q2_result disc_load_cue(disc *d, const char *cue_path)
{
    FILE *fp;
    char line[1024];
    int current_backing = -1;
    cd_track *track = NULL;

    fp = fopen(cue_path, "rb");
    if (!fp)
        return Q2_ERR_NOT_FOUND;

    while (fgets(line, sizeof(line), fp)) {
        char *p = line;

        str_trim(p);
        while (*p == ' ' || *p == '\t')
            p++;

        if (str_starts_with_ci(p, "FILE")) {
            char resolved[512];
            char name[512];
            const char *q1 = strchr(p, '"');
            const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;

            if (!q1 || !q2)
                continue;

            {
                size_t n = (size_t)(q2 - q1 - 1);
                if (n >= sizeof(name))
                    n = sizeof(name) - 1;
                memcpy(name, q1 + 1, n);
                name[n] = '\0';
            }

            path_resolve_sibling(resolved, sizeof(resolved), cue_path, name);
            current_backing = disc_add_backing(d, resolved);
            if (current_backing < 0) {
                Q2_ERROR("cue references a file that cannot be opened: %s", resolved);
                fclose(fp);
                return Q2_ERR_NOT_FOUND;
            }
            Q2_DEBUG("cue: backing file %d = %s", current_backing, resolved);

        } else if (str_starts_with_ci(p, "TRACK")) {
            int number = 0;
            char mode[64] = { 0 };

            if (d->track_count >= CD_MAX_TRACKS)
                break;
            if (sscanf(p, "%*s %d %63s", &number, mode) != 2)
                continue;

            track = &d->tracks[d->track_count++];
            memset(track, 0, sizeof(*track));
            track->number      = number;
            track->type        = str_starts_with_ci(mode, "AUDIO") ? CD_TRACK_AUDIO : CD_TRACK_DATA;
            track->sector_size = cue_sector_size_for_mode(mode);
            track->file_index  = current_backing;
            track->pregap_lba  = UINT32_MAX;

        } else if (str_starts_with_ci(p, "INDEX") && track) {
            int index = 0, m = 0, s = 0, f = 0;

            if (sscanf(p, "%*s %d %d:%d:%d", &index, &m, &s, &f) != 4)
                continue;

            if (index == 0)
                track->pregap_lba = msf_to_lba(m, s, f);
            else if (index == 1)
                track->start_lba = msf_to_lba(m, s, f);
        }
    }

    fclose(fp);

    if (d->track_count == 0)
        return Q2_ERR_BAD_FORMAT;

    {
        q2_result r = cue_layout_tracks(d);
        if (r != Q2_OK)
            return r;
    }

    /* Pick the first data track as the filesystem source. */
    d->data_track = 0;
    {
        int i;
        for (i = 0; i < d->track_count; i++) {
            if (d->tracks[i].type == CD_TRACK_DATA) {
                d->data_track = i;
                break;
            }
        }
    }

    return Q2_OK;
}

/* A bare image with no cue: infer the sector size from the file length. */
static q2_result disc_load_bare_image(disc *d, const char *path, int forced_sector_size)
{
    int idx = disc_add_backing(d, path);
    cd_track *t;
    u64 size;
    int sector_size = forced_sector_size;

    if (idx < 0)
        return Q2_ERR_NOT_FOUND;

    size = d->backing[idx].size;

    if (sector_size != 0 && size % (u64)sector_size != 0)
        return Q2_ERR_BAD_FORMAT;

    if (sector_size == 0) {
        if (size % CD_SECTOR_RAW == 0)
            sector_size = CD_SECTOR_RAW;
        else if (size % CD_SECTOR_MODE1 == 0)
            sector_size = CD_SECTOR_MODE1;
        else if (size % 2336 == 0)
            sector_size = 2336;
        else
            return Q2_ERR_BAD_FORMAT;
    }

    t = &d->tracks[d->track_count++];
    memset(t, 0, sizeof(*t));
    t->number         = 1;
    t->type           = CD_TRACK_DATA;
    t->sector_size    = sector_size;
    t->file_index     = idx;
    t->start_lba      = 0;
    t->pregap_lba     = 0;
    t->file_offset    = 0;
    t->length_sectors = (u32)(size / (u64)sector_size);

    d->data_track = 0;
    return Q2_OK;
}

/* ------------------------------------------------------------------------- */
/* ISO9660                                                                    */
/* ------------------------------------------------------------------------- */
#define ISO_PVD_LBA 16

/* ISO9660 logical block numbers are relative to the volume, while the public
 * track and raw-sector APIs use absolute disc LBAs.  They are identical on a
 * normal PSX data-first disc, but differ for a valid mixed-mode layout whose
 * data FILE follows one or more audio FILEs. */
static q2_result iso_absolute_lba(const disc *d, u32 volume_lba,
                                 u32 *out_lba)
{
    const cd_track *data;
    u64 absolute;

    if (!d || !out_lba || d->data_track < 0 ||
        d->data_track >= d->track_count)
        return Q2_ERR_BAD_FORMAT;

    data = &d->tracks[d->data_track];
    absolute = (u64)data->start_lba + (u64)volume_lba;
    if (absolute > UINT32_MAX)
        return Q2_ERR_RANGE;

    *out_lba = (u32)absolute;
    return Q2_OK;
}

static void iso_copy_field(char *dst, size_t cap, const u8 *src, size_t len)
{
    size_t n = len < cap - 1 ? len : cap - 1;

    memcpy(dst, src, n);
    dst[n] = '\0';
    while (n > 0 && dst[n - 1] == ' ')
        dst[--n] = '\0';
}

/* Strip the ISO9660 ";1" version suffix and any trailing '.'. */
static void iso_clean_name(char *name)
{
    char *semi = strchr(name, ';');
    size_t len;

    if (semi)
        *semi = '\0';

    len = strlen(name);
    while (len > 0 && name[len - 1] == '.')
        name[--len] = '\0';
}

static q2_result iso_walk_dir(disc *d, u32 lba, u32 length, const char *prefix, int depth)
{
    q2_buf dir;
    q2_result r;
    u32 absolute_lba;
    u32 sectors;
    size_t off = 0;

    if (depth > 8)
        return Q2_OK;   /* ISO9660 permits 8 levels; deeper means corruption */

    r = iso_absolute_lba(d, lba, &absolute_lba);
    if (r != Q2_OK)
        return r;

    sectors = (length + CD_SECTOR_FORM1 - 1) / CD_SECTOR_FORM1;
    r = q2_buf_alloc(&dir, (size_t)sectors * CD_SECTOR_FORM1);
    if (r != Q2_OK)
        return r;

    r = disc_read_form1_run(d, absolute_lba, sectors, dir.data);
    if (r != Q2_OK) {
        q2_buf_free(&dir);
        return r;
    }

    while (off + 33 <= length) {
        const u8 *rec = dir.data + off;
        u32 rec_len = rec[0];
        u32 ext_lba, ext_len;
        u8  flags, name_len;
        char name[256];
        char full[256];

        if (rec_len == 0) {
            /* Records never straddle a sector; skip to the next one. */
            off = ((off / CD_SECTOR_FORM1) + 1) * CD_SECTOR_FORM1;
            continue;
        }
        if (rec_len < 33 || off + rec_len > length)
            break;

        ext_lba  = q2_rd_u32(rec + 2);
        ext_len  = q2_rd_u32(rec + 10);
        flags    = rec[25];
        name_len = rec[32];

        if (name_len == 0 || 33 + (size_t)name_len > rec_len) {
            off += rec_len;
            continue;
        }

        /* '\0' is ".", '\1' is ".." — skip both. */
        if (name_len == 1 && (rec[33] == 0 || rec[33] == 1)) {
            off += rec_len;
            continue;
        }

        iso_copy_field(name, sizeof(name), rec + 33, name_len);
        iso_clean_name(name);

        snprintf(full, sizeof(full), "%s/%s", prefix, name);

        if (flags & 0x02) {
            r = iso_walk_dir(d, ext_lba, ext_len, full, depth + 1);
            if (r != Q2_OK) {
                q2_buf_free(&dir);
                return r;
            }
        } else if (d->file_count < DISC_MAX_FILES) {
            disc_file *f = &d->files[d->file_count++];
            u32 file_lba;

            r = iso_absolute_lba(d, ext_lba, &file_lba);
            if (r != Q2_OK) {
                q2_buf_free(&dir);
                return r;
            }
            str_copy(f->path, sizeof(f->path), full);
            f->lba   = file_lba;
            f->size  = ext_len;
            f->form2 = disc_sector_is_form2(d, file_lba);
        }

        off += rec_len;
    }

    q2_buf_free(&dir);
    return Q2_OK;
}

static q2_result disc_load_iso9660(disc *d, bool report_error)
{
    u8 pvd[CD_SECTOR_FORM1];
    q2_result r;
    u32 pvd_lba;
    u32 root_lba, root_len;

    r = iso_absolute_lba(d, ISO_PVD_LBA, &pvd_lba);
    if (r != Q2_OK)
        return r;

    r = disc_read_form1_run(d, pvd_lba, 1, pvd);
    if (r != Q2_OK)
        return r;

    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) {
        if (report_error)
            Q2_ERROR("no ISO9660 primary volume descriptor at LBA %u", pvd_lba);
        return Q2_ERR_BAD_FORMAT;
    }

    iso_copy_field(d->system_id, sizeof(d->system_id), pvd + 8, 32);
    iso_copy_field(d->volume_id, sizeof(d->volume_id), pvd + 40, 32);
    d->volume_sectors = q2_rd_u32(pvd + 80);

    memcpy(d->creation_time, pvd + 813, 17);
    d->creation_time[17] = '\0';

    root_lba = q2_rd_u32(pvd + 156 + 2);
    root_len = q2_rd_u32(pvd + 156 + 10);

    d->files = (disc_file *)calloc(DISC_MAX_FILES, sizeof(disc_file));
    if (!d->files)
        return Q2_ERR_NO_MEMORY;

    return iso_walk_dir(d, root_lba, root_len, "", 0);
}

/* A `.iso` suffix normally means cooked 2048-byte sectors, but raw dumps are
 * commonly mislabeled that way.  File-size divisibility alone cannot decide:
 * 128 raw sectors occupy exactly 147 cooked sectors.  Validate the cooked
 * interpretation through ISO9660, then discard it completely and infer the
 * physical sector size when that probe fails. */
static q2_result disc_load_iso_path(disc *d, const char *path)
{
    q2_result r;

    r = disc_load_bare_image(d, path, CD_SECTOR_MODE1);
    if (r == Q2_OK) {
        r = disc_load_iso9660(d, false);
        if (r == Q2_OK)
            return Q2_OK;
    }

    /* A missing/unreadable image or an allocation failure is not evidence of
     * a different sector layout. Preserve that real error instead of masking
     * it with a second open attempt. */
    if (r != Q2_ERR_BAD_FORMAT)
        return r;

    disc_reset_loaded(d);
    r = disc_load_bare_image(d, path, 0);
    if (r != Q2_OK)
        return r;
    return disc_load_iso9660(d, true);
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */
q2_result disc_open(disc **out, const char *path)
{
    disc *d;
    q2_result r;
    const char *ext;
    bool iso_loaded = false;

    if (!out || !path)
        return Q2_ERR_INVALID_ARG;
    *out = NULL;

    d = (disc *)calloc(1, sizeof(disc));
    if (!d)
        return Q2_ERR_NO_MEMORY;

    ext = path_extension(path);

    if (ascii_casecmp(ext, "cue") == 0) {
        r = disc_load_cue(d, path);
    } else if (ascii_casecmp(ext, "iso") == 0) {
        r = disc_load_iso_path(d, path);
        iso_loaded = r == Q2_OK;
    } else if (ascii_casecmp(ext, "bin") == 0 || ascii_casecmp(ext, "img") == 0) {
        /* Prefer a sibling .cue so multi-track discs keep their audio. */
        char cue[512];
        size_t len = strlen(path);

        str_copy(cue, sizeof(cue), path);
        if (len > 3 && len < sizeof(cue)) {
            cue[len - 3] = 'c';
            cue[len - 2] = 'u';
            cue[len - 1] = 'e';
        }

        r = disc_load_cue(d, cue);
        if (r != Q2_OK) {
            disc_reset_loaded(d);
            r = disc_load_bare_image(d, path, 0);
        } else {
            Q2_INFO("using sibling cue sheet: %s", cue);
        }
    } else if (ascii_casecmp(ext, "chd") == 0 ||
               ascii_casecmp(ext, "mds") == 0 ||
               ascii_casecmp(ext, "mdf") == 0 ||
               ascii_casecmp(ext, "pbp") == 0) {
        Q2_ERROR(".%s images are not supported yet", ext);
        free(d);
        return Q2_ERR_UNSUPPORTED;
    } else {
        r = disc_load_bare_image(d, path, 0);
    }

    if (r != Q2_OK) {
        disc_close(d);
        return r;
    }

    if (!iso_loaded) {
        r = disc_load_iso9660(d, true);
        if (r != Q2_OK) {
            disc_close(d);
            return r;
        }
    }

    snprintf(d->description, sizeof(d->description),
             "%s — %s, %d track%s, %u sectors, %d files",
             path,
             d->system_id[0] ? d->system_id : "unknown system",
             d->track_count, d->track_count == 1 ? "" : "s",
             d->volume_sectors, d->file_count);

    *out = d;
    return Q2_OK;
}

void disc_close(disc *d)
{
    if (!d)
        return;

    disc_reset_loaded(d);
    free(d);
}

const char *disc_describe(const disc *d)      { return d ? d->description : ""; }
const char *disc_volume_id(const disc *d)     { return d ? d->volume_id : ""; }
const char *disc_system_id(const disc *d)     { return d ? d->system_id : ""; }
u32         disc_volume_sectors(const disc *d){ return d ? d->volume_sectors : 0; }
const char *disc_creation_time(const disc *d) { return d ? d->creation_time : ""; }

int disc_track_count(const disc *d) { return d ? d->track_count : 0; }

const cd_track *disc_track(const disc *d, int index)
{
    if (!d || index < 0 || index >= d->track_count)
        return NULL;
    return &d->tracks[index];
}

int disc_file_count(const disc *d) { return d ? d->file_count : 0; }

const disc_file *disc_file_at(const disc *d, int index)
{
    if (!d || index < 0 || index >= d->file_count)
        return NULL;
    return &d->files[index];
}

const disc_file *disc_find(const disc *d, const char *path)
{
    char want[256];
    int i;

    if (!d || !path)
        return NULL;

    /* Normalise: leading slash optional, backslashes accepted, no ";1". */
    str_copy(want, sizeof(want), path[0] == '/' || path[0] == '\\' ? path + 1 : path);
    for (i = 0; want[i]; i++) {
        if (want[i] == '\\')
            want[i] = '/';
    }
    iso_clean_name(want);

    for (i = 0; i < d->file_count; i++) {
        const char *have = d->files[i].path;
        if (*have == '/')
            have++;
        if (ascii_casecmp(have, want) == 0)
            return &d->files[i];
    }
    return NULL;
}

q2_result disc_read_file(const disc *d, const char *path, q2_buf *out)
{
    const disc_file *f;
    q2_result r;

    if (!d || !out)
        return Q2_ERR_INVALID_ARG;

    f = disc_find(d, path);
    if (!f)
        return Q2_ERR_NOT_FOUND;

    if (f->form2) {
        /* Streamed media: concatenate 2324-byte payloads. The directory size is
         * a Form 1 style byte count, so derive the sector count from it and
         * return every payload byte — the demuxer needs the whole thing. */
        u32 sectors = (f->size + CD_SECTOR_FORM1 - 1) / CD_SECTOR_FORM1;
        u32 i;

        r = q2_buf_alloc(out, (size_t)sectors * CD_SECTOR_FORM2);
        if (r != Q2_OK)
            return r;

        for (i = 0; i < sectors; i++) {
            u32 len = 0;
            u8 payload[CD_SECTOR_RAW];

            r = disc_read_sector_payload(d, f->lba + i, payload, &len);
            if (r != Q2_OK) {
                q2_buf_free(out);
                return r;
            }
            memcpy(out->data + (size_t)i * CD_SECTOR_FORM2, payload,
                   len < CD_SECTOR_FORM2 ? len : CD_SECTOR_FORM2);
        }
        return Q2_OK;
    }

    {
        u32 sectors = (f->size + CD_SECTOR_FORM1 - 1) / CD_SECTOR_FORM1;

        r = q2_buf_alloc(out, (size_t)sectors * CD_SECTOR_FORM1);
        if (r != Q2_OK)
            return r;

        r = disc_read_form1_run(d, f->lba, sectors, out->data);
        if (r != Q2_OK) {
            q2_buf_free(out);
            return r;
        }
        out->size = f->size;   /* trim the sector padding */
        return Q2_OK;
    }
}

/* ------------------------------------------------------------------------- */
/* SYSTEM.CNF                                                                 */
/* ------------------------------------------------------------------------- */
q2_result disc_read_boot_info(const disc *d, disc_boot_info *out)
{
    q2_buf cnf;
    q2_result r;
    char *line;
    char *save = NULL;

    if (!d || !out)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    r = disc_read_file(d, "SYSTEM.CNF", &cnf);
    if (r != Q2_OK)
        return r;

    for (line = q2_strtok_r((char *)cnf.data, "\r\n", &save);
         line;
         line = q2_strtok_r(NULL, "\r\n", &save)) {
        char *eq = strchr(line, '=');
        char *key, *val;

        if (!eq)
            continue;

        *eq = '\0';
        key = line;
        val = eq + 1;

        while (*key == ' ' || *key == '\t') key++;
        while (*val == ' ' || *val == '\t') val++;
        str_trim(key);
        str_trim(val);

        if (ascii_casecmp(key, "BOOT") == 0) {
            const char *slash;
            char name[64];
            int i;

            str_copy(out->boot_path, sizeof(out->boot_path), val);

            /* "cdrom:\SLES_015.34;1" -> "SLES_015.34" -> "SLES-01534" */
            slash = strrchr(val, '\\');
            if (!slash)
                slash = strrchr(val, '/');
            if (!slash)
                slash = strchr(val, ':');
            str_copy(name, sizeof(name), slash ? slash + 1 : val);
            iso_clean_name(name);

            for (i = 0; name[i]; i++)
                name[i] = (char)toupper((unsigned char)name[i]);
            str_copy(out->exe_name, sizeof(out->exe_name), name);

            /* The serial is the executable name with '_' and '.' removed and a
             * hyphen after the four-letter prefix. */
            {
                char digits[16];
                int n = 0;

                for (i = 4; name[i] && n < (int)sizeof(digits) - 1; i++) {
                    if (isdigit((unsigned char)name[i]))
                        digits[n++] = name[i];
                }
                digits[n] = '\0';

                if (n >= 5) {
                    snprintf(out->serial, sizeof(out->serial), "%.4s-%s", name, digits);
                } else {
                    str_copy(out->serial, sizeof(out->serial), name);
                }
            }
        } else if (ascii_casecmp(key, "TCB") == 0) {
            out->tcb = (u32)strtoul(val, NULL, 0);
        } else if (ascii_casecmp(key, "EVENT") == 0) {
            out->event = (u32)strtoul(val, NULL, 0);
        } else if (ascii_casecmp(key, "STACK") == 0) {
            out->stack = (u32)strtoul(val, NULL, 16);
        }
    }

    q2_buf_free(&cnf);
    return Q2_OK;
}
