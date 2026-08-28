/*
 * test_disc.c — CUE-sheet track layout and backing-file addressing.
 *
 * A CUE INDEX is relative to its current FILE, while cd_track exposes an
 * absolute disc LBA.  The fixture uses different byte markers in the pregap
 * and playable audio sectors so a plausible-looking track table cannot hide a
 * read from the wrong backing file or the wrong offset within that file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <process.h>
#else
#  include <unistd.h>
#endif

#include "disc.h"

#define DATA_SECTORS  20u
#define PREGAP_SECTORS 2u
#define AUDIO_SECTORS  3u
#define RAW_ISO_SECTORS 128u

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what)
{
    g_checks++;
    if (!condition) {
        printf("  FAIL  %s\n", what);
        g_failures++;
    }
}

static void check_eq_u64(u64 got, u64 want, const char *what)
{
    g_checks++;
    if (got != want) {
        printf("  FAIL  %s: got %llu, want %llu\n",
               what, (unsigned long long)got, (unsigned long long)want);
        g_failures++;
    }
}

static const char *temp_dir(void)
{
    const char *dir = getenv("TEMP");

    if (!dir || !*dir)
        dir = getenv("TMPDIR");
    return (dir && *dir) ? dir : ".";
}

static unsigned long test_process_id(void)
{
#if defined(_WIN32)
    return (unsigned long)_getpid();
#else
    return (unsigned long)getpid();
#endif
}

static void temp_path(char *out, size_t cap, const char *name)
{
    const char *dot = strrchr(name, '.');

    if (dot) {
        snprintf(out, cap, "%s/%.*s_%lu%s", temp_dir(),
                 (int)(dot - name), name, test_process_id(), dot);
    } else {
        snprintf(out, cap, "%s/%s_%lu", temp_dir(), name,
                 test_process_id());
    }
}

static bool write_data_sector(FILE *fp, u32 lba)
{
    u8 raw[CD_SECTOR_RAW];
    u8 *payload;

    memset(raw, 0, sizeof(raw));
    raw[15] = 2;
    raw[18] = raw[22] = (u8)CD_SUBMODE_DATA;
    payload = raw + 24;

    if (lba == 16) {
        /* The smallest PVD/root pair disc_open needs. */
        payload[0] = 1;
        memcpy(payload + 1, "CD001", 5);
        q2_wr_u32(payload + 156 + 2, 17);
        q2_wr_u32(payload + 156 + 10, CD_SECTOR_FORM1);
    } else if (lba == 17) {
        /* One tiny file makes the audio-first test cover not only the PVD and
         * root reads, but extent rebasing and the public file-read path too. */
        payload[0] = 34;
        q2_wr_u32(payload + 2, 18);
        q2_wr_u32(payload + 10, 4);
        payload[25] = 0;
        payload[32] = 1;
        payload[33] = 'X';
    } else if (lba == 18) {
        memcpy(payload, "DATA", 4);
    }

    return fwrite(raw, 1, sizeof(raw), fp) == sizeof(raw);
}

static bool write_data_image(const char *path, u32 sectors)
{
    FILE *fp = fopen(path, "wb");
    u32 i;

    if (!fp)
        return false;
    for (i = 0; i < sectors; i++) {
        if (!write_data_sector(fp, i)) {
            fclose(fp);
            return false;
        }
    }
    return fclose(fp) == 0;
}

static bool append_marked_sectors(FILE *fp, u8 first_marker, u32 sectors)
{
    u8 raw[CD_SECTOR_RAW];
    u32 i;

    for (i = 0; i < sectors; i++) {
        memset(raw, (u8)(first_marker + i), sizeof(raw));
        if (fwrite(raw, 1, sizeof(raw), fp) != sizeof(raw))
            return false;
    }
    return true;
}

static bool write_audio_image(const char *path, u8 first_marker, u32 sectors)
{
    FILE *fp = fopen(path, "wb");
    bool ok;

    if (!fp)
        return false;
    ok = append_marked_sectors(fp, first_marker, sectors);
    if (fclose(fp) != 0)
        ok = false;
    return ok;
}

static bool write_single_image(const char *path)
{
    FILE *fp = fopen(path, "wb");
    u32 i;
    bool ok = true;

    if (!fp)
        return false;
    for (i = 0; i < DATA_SECTORS; i++) {
        if (!write_data_sector(fp, i)) {
            ok = false;
            break;
        }
    }
    if (ok)
        ok = append_marked_sectors(fp, 0x70,
                                   PREGAP_SECTORS + AUDIO_SECTORS);
    if (fclose(fp) != 0)
        ok = false;
    return ok;
}

static bool write_text(const char *path, const char *text)
{
    FILE *fp = fopen(path, "wb");
    const size_t len = strlen(text);
    bool ok;

    if (!fp)
        return false;
    ok = fwrite(text, 1, len, fp) == len;
    if (fclose(fp) != 0)
        ok = false;
    return ok;
}

static void test_multifile_rebase(void)
{
    char cue_path[512], data_path[512], audio_path[512];
    char cue_text[2048];
    disc *d = NULL;
    const cd_track *data;
    const cd_track *audio;
    u8 raw[CD_SECTOR_RAW];
    q2_result r;

    temp_path(cue_path, sizeof(cue_path), "q2psx_disc_multi.cue");
    temp_path(data_path, sizeof(data_path), "q2psx_disc_multi_data.bin");
    temp_path(audio_path, sizeof(audio_path), "q2psx_disc_multi_audio.bin");
    snprintf(cue_text, sizeof(cue_text),
             "FILE \"%s\" BINARY\n"
             "  TRACK 01 MODE2/2352\n"
             "    INDEX 01 00:00:00\n"
             "FILE \"%s\" BINARY\n"
             "  TRACK 02 AUDIO\n"
             "    INDEX 00 00:00:00\n"
             "    INDEX 01 00:00:02\n",
             data_path, audio_path);
    remove(cue_path);
    remove(data_path);
    remove(audio_path);

    check(write_data_image(data_path, DATA_SECTORS),
          "write multi-file data fixture");
    check(write_audio_image(audio_path, 0xA0,
                            PREGAP_SECTORS + AUDIO_SECTORS),
          "write multi-file audio fixture");
    check(write_text(cue_path, cue_text), "write multi-file CUE fixture");

    r = disc_open(&d, cue_path);
    check_eq_u64((u64)r, Q2_OK, "open multi-file CUE");
    if (!d)
        goto cleanup;

    check_eq_u64((u64)disc_track_count(d), 2, "multi-file track count");
    data = disc_track(d, 0);
    audio = disc_track(d, 1);
    check(data != NULL, "multi-file data track exists");
    check(audio != NULL, "multi-file audio track exists");
    if (!data || !audio)
        goto close;

    check_eq_u64(data->start_lba, 0, "data INDEX 01 absolute LBA");
    check_eq_u64(data->length_sectors, DATA_SECTORS,
                 "data track stops at end of first FILE");
    check_eq_u64(audio->pregap_lba, DATA_SECTORS,
                 "audio INDEX 00 rebased after data FILE");
    check_eq_u64(audio->start_lba, DATA_SECTORS + PREGAP_SECTORS,
                 "audio INDEX 01 rebased after data FILE and pregap");
    check_eq_u64(audio->file_offset,
                 (u64)PREGAP_SECTORS * CD_SECTOR_RAW,
                 "audio read offset skips in-file pregap");
    check_eq_u64(audio->length_sectors, AUDIO_SECTORS,
                 "audio length excludes INDEX 00 pregap");
    check(audio->start_lba >= data->start_lba + data->length_sectors,
          "multi-file tracks do not overlap");

    memset(raw, 0, sizeof(raw));
    r = disc_read_raw_sector(d, audio->pregap_lba, raw);
    check_eq_u64((u64)r, Q2_OK, "read first multi-file INDEX 00 sector");
    check_eq_u64(raw[0], 0xA0,
                 "pregap LBA reads the first marker from the audio FILE");

    memset(raw, 0, sizeof(raw));
    r = disc_read_raw_sector(d, audio->start_lba - 1, raw);
    check_eq_u64((u64)r, Q2_OK, "read last multi-file INDEX 00 sector");
    check_eq_u64(raw[0], 0xA0 + PREGAP_SECTORS - 1,
                 "the pregap-to-playable boundary keeps FILE-local offsets");

    memset(raw, 0, sizeof(raw));
    r = disc_read_raw_sector(d, audio->start_lba, raw);
    check_eq_u64((u64)r, Q2_OK, "read first playable audio sector");
    check_eq_u64(raw[0], 0xA0 + PREGAP_SECTORS,
                 "audio LBA reads marker from second FILE after pregap");

close:
    disc_close(d);
cleanup:
    remove(cue_path);
    remove(data_path);
    remove(audio_path);
}

static void test_singlefile_unchanged(void)
{
    char cue_path[512], image_path[512];
    char cue_text[2048];
    disc *d = NULL;
    const cd_track *data;
    const cd_track *audio;
    u8 raw[CD_SECTOR_RAW];
    q2_result r;

    temp_path(cue_path, sizeof(cue_path), "q2psx_disc_single.cue");
    temp_path(image_path, sizeof(image_path), "q2psx_disc_single.bin");
    snprintf(cue_text, sizeof(cue_text),
             "FILE \"%s\" BINARY\n"
             "  TRACK 01 MODE2/2352\n"
             "    INDEX 01 00:00:00\n"
             "  TRACK 02 AUDIO\n"
             "    INDEX 00 00:00:20\n"
             "    INDEX 01 00:00:22\n",
             image_path);
    remove(cue_path);
    remove(image_path);

    check(write_single_image(image_path), "write single-file image fixture");
    check(write_text(cue_path, cue_text), "write single-file CUE fixture");

    r = disc_open(&d, cue_path);
    check_eq_u64((u64)r, Q2_OK, "open single-file CUE");
    if (!d)
        goto cleanup;

    data = disc_track(d, 0);
    audio = disc_track(d, 1);
    check(data != NULL, "single-file data track exists");
    check(audio != NULL, "single-file audio track exists");
    if (!data || !audio)
        goto close;

    /* These are the pre-existing one-FILE semantics: track one extends through
     * the in-file pregap and track two begins at its INDEX 01 offset. */
    check_eq_u64(data->start_lba, 0, "single-file data start unchanged");
    check_eq_u64(data->length_sectors,
                 DATA_SECTORS + PREGAP_SECTORS,
                 "single-file data length unchanged");
    check_eq_u64(audio->pregap_lba, DATA_SECTORS,
                 "single-file INDEX 00 unchanged");
    check_eq_u64(audio->start_lba, DATA_SECTORS + PREGAP_SECTORS,
                 "single-file INDEX 01 unchanged");
    check_eq_u64(audio->file_offset,
                 (u64)(DATA_SECTORS + PREGAP_SECTORS) * CD_SECTOR_RAW,
                 "single-file audio offset unchanged");
    check_eq_u64(audio->length_sectors, AUDIO_SECTORS,
                 "single-file audio length unchanged");

    memset(raw, 0, sizeof(raw));
    r = disc_read_raw_sector(d, audio->pregap_lba, raw);
    check_eq_u64((u64)r, Q2_OK, "read single-file INDEX 00 sector");
    check_eq_u64(raw[0], 0x70,
                 "single-file pregap retains its established physical offset");

    memset(raw, 0, sizeof(raw));
    r = disc_read_raw_sector(d, audio->start_lba, raw);
    check_eq_u64((u64)r, Q2_OK, "read single-file audio sector");
    check_eq_u64(raw[0], 0x70 + PREGAP_SECTORS,
                 "single-file audio still reads its INDEX 01 marker");

close:
    disc_close(d);
cleanup:
    remove(cue_path);
    remove(image_path);
}

static void test_audio_first_data_volume(void)
{
    char cue_path[512], audio_path[512], data_path[512];
    char cue_text[2048];
    disc *d = NULL;
    const cd_track *audio;
    const cd_track *data;
    const disc_file *entry;
    q2_buf file = { 0 };
    q2_result r;

    temp_path(cue_path, sizeof(cue_path), "q2psx_disc_audio_first.cue");
    temp_path(audio_path, sizeof(audio_path), "q2psx_disc_audio_first.bin");
    temp_path(data_path, sizeof(data_path), "q2psx_disc_data_second.bin");
    snprintf(cue_text, sizeof(cue_text),
             "FILE \"%s\" BINARY\n"
             "  TRACK 01 AUDIO\n"
             "    INDEX 01 00:00:00\n"
             "FILE \"%s\" BINARY\n"
             "  TRACK 02 MODE2/2352\n"
             "    INDEX 01 00:00:00\n",
             audio_path, data_path);
    remove(cue_path);
    remove(audio_path);
    remove(data_path);

    check(write_audio_image(audio_path, 0x50, AUDIO_SECTORS),
          "write leading audio FILE");
    check(write_data_image(data_path, DATA_SECTORS),
          "write second-file ISO volume");
    check(write_text(cue_path, cue_text), "write audio-first CUE fixture");

    r = disc_open(&d, cue_path);
    check_eq_u64((u64)r, Q2_OK,
                 "open audio-first CUE using the selected data-track base");
    if (!d)
        goto cleanup;

    audio = disc_track(d, 0);
    data = disc_track(d, 1);
    check(audio != NULL, "audio-first leading track exists");
    check(data != NULL, "audio-first data track exists");
    if (!audio || !data)
        goto close;

    check_eq_u64(audio->start_lba, 0, "leading audio starts at absolute zero");
    check_eq_u64(data->start_lba, AUDIO_SECTORS,
                 "data INDEX 01 follows the complete audio FILE");

    entry = disc_find(d, "X");
    check(entry != NULL, "root directory was read relative to the data track");
    if (entry) {
        check_eq_u64(entry->lba, AUDIO_SECTORS + 18,
                     "ISO file extent is exposed as an absolute disc LBA");
        r = disc_read_file(d, "X", &file);
        check_eq_u64((u64)r, Q2_OK,
                     "read a file from the rebased data volume");
        if (r == Q2_OK) {
            check_eq_u64(file.size, 4, "rebased file keeps its ISO byte size");
            check(memcmp(file.data, "DATA", 4) == 0,
                  "rebased file data comes from the second backing");
        }
    }

close:
    q2_buf_free(&file);
    disc_close(d);
cleanup:
    remove(cue_path);
    remove(audio_path);
    remove(data_path);
}

static void test_malformed_sibling_fallback_closes_backings(void)
{
    char cue_path[512], image_path[512], missing_path[512];
    char cue_text[2048];
    disc *d = NULL;
    q2_result r;

    temp_path(cue_path, sizeof(cue_path), "q2psx_disc_fallback.cue");
    temp_path(image_path, sizeof(image_path), "q2psx_disc_fallback.bin");
    temp_path(missing_path, sizeof(missing_path),
              "q2psx_disc_fallback_missing.bin");
    snprintf(cue_text, sizeof(cue_text),
             "FILE \"%s\" BINARY\n"
             "  TRACK 01 MODE2/2352\n"
             "    INDEX 01 00:00:00\n"
             "FILE \"%s\" BINARY\n"
             "  TRACK 02 AUDIO\n"
             "    INDEX 01 00:00:00\n",
             image_path, missing_path);
    remove(cue_path);
    remove(image_path);
    remove(missing_path);

    check(write_data_image(image_path, DATA_SECTORS),
          "write bare-image fallback fixture");
    check(write_text(cue_path, cue_text),
          "write malformed sibling CUE fixture");

    r = disc_open(&d, image_path);
    check_eq_u64((u64)r, Q2_OK,
                 "malformed sibling CUE falls back to the requested BIN");
    if (d) {
        check_eq_u64((u64)disc_track_count(d), 1,
                     "fallback disc has one inferred data track");
        disc_close(d);
    }

    check(remove(cue_path) == 0, "remove malformed sibling CUE after close");
    check(remove(image_path) == 0,
          "partial sibling-CUE backing handle was closed before fallback");
    remove(missing_path);
}

static void test_raw_image_with_iso_suffix(void)
{
    char image_path[512];
    disc *d = NULL;
    const cd_track *track;
    q2_buf file = { 0 };
    q2_result r;

    temp_path(image_path, sizeof(image_path), "q2psx_disc_raw.iso");
    remove(image_path);

    /* 128 * 2352 == 147 * 2048. A size-only probe therefore accepts both
     * layouts; the PVD is what must reject cooked mode and trigger raw mode. */
    check(write_data_image(image_path, RAW_ISO_SECTORS),
          "write raw-sector image with .iso suffix and ambiguous byte size");

    r = disc_open(&d, image_path);
    check_eq_u64((u64)r, Q2_OK,
                 "raw-sector .iso retries after cooked PVD validation fails");
    if (!d)
        goto cleanup;

    track = disc_track(d, 0);
    check(track != NULL, "raw .iso inferred a data track");
    if (track)
        check_eq_u64((u64)track->sector_size, CD_SECTOR_RAW,
                     "raw .iso selected 2352-byte physical sectors");

    r = disc_read_file(d, "X", &file);
    check_eq_u64((u64)r, Q2_OK, "read file through raw .iso fallback");
    if (r == Q2_OK) {
        check_eq_u64(file.size, 4, "raw .iso file keeps its ISO byte size");
        check(memcmp(file.data, "DATA", 4) == 0,
              "raw .iso file payload comes from the 2352-byte layout");
    }

    q2_buf_free(&file);
    disc_close(d);
cleanup:
    remove(image_path);
}

int main(void)
{
    printf("disc CUE layout tests\n");
    test_multifile_rebase();
    test_singlefile_unchanged();
    test_audio_first_data_volume();
    test_malformed_sibling_fallback_closes_backings();
    test_raw_image_with_iso_suffix();

    if (g_failures) {
        printf("%d/%d checks failed\n", g_failures, g_checks);
        return 1;
    }

    printf("all %d checks passed\n", g_checks);
    return 0;
}
