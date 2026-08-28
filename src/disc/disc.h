/*
 * disc.h — access to the user's Quake II PSX disc.
 *
 * Everything above this layer sees "a disc": a flat namespace of files plus a
 * set of audio tracks. Below it, that might be a .cue/.bin pair, a plain .iso,
 * or a real CD spinning in an optical drive. The goal stated in the README —
 * point the app at the disc and it works — lives or dies here.
 *
 * CD-ROM sector shapes we must cope with:
 *
 *   2048 bytes/sector : ISO ("cooked"), Mode 1 user data only.
 *   2352 bytes/sector : raw. 12-byte sync + 4-byte header, then either
 *                       Mode 1  (2048 data + 288 ECC), or
 *                       Mode 2 Form 1 (8-byte subheader + 2048 data + 280 ECC), or
 *                       Mode 2 Form 2 (8-byte subheader + 2324 data + 4 spare).
 *   2336 bytes/sector : raw minus sync/header, occasionally seen in old dumps.
 *
 * PlayStation discs are Mode 2. Ordinary files are Form 1; streamed XA audio and
 * STR video are Form 2, which is why the media files need a different read path
 * from the level data.
 */
#ifndef Q2PSX_DISC_H
#define Q2PSX_DISC_H

#include "q2psx.h"

#define CD_SECTOR_RAW        2352
#define CD_SECTOR_MODE1      2048
#define CD_SECTOR_FORM1      2048
#define CD_SECTOR_FORM2      2324
#define CD_SECTOR_SUBHEADER  8
#define CD_SECTOR_HEADER     16   /* 12-byte sync + 4-byte address/mode */

#define CD_SECTORS_PER_SECOND 75

/* ------------------------------------------------------------------------- */
/* Sector submode bits (byte 2 of the CD-XA subheader).                       */
/* ------------------------------------------------------------------------- */
enum {
    CD_SUBMODE_EOR     = 1 << 0,  /* end of record                */
    CD_SUBMODE_VIDEO   = 1 << 1,
    CD_SUBMODE_AUDIO   = 1 << 2,
    CD_SUBMODE_DATA    = 1 << 3,
    CD_SUBMODE_TRIGGER = 1 << 4,
    CD_SUBMODE_FORM2   = 1 << 5,  /* 2324-byte payload            */
    CD_SUBMODE_REALTIME= 1 << 6,
    CD_SUBMODE_EOF     = 1 << 7
};

/* ------------------------------------------------------------------------- */
/* Track table                                                                */
/* ------------------------------------------------------------------------- */
typedef enum cd_track_type {
    CD_TRACK_DATA = 0,
    CD_TRACK_AUDIO
} cd_track_type;

typedef struct cd_track {
    int           number;
    cd_track_type type;
    u32           start_lba;     /* absolute LBA of INDEX 01     */
    u32           pregap_lba;    /* absolute LBA of INDEX 00     */
    u32           length_sectors;
    int           sector_size;   /* 2352 or 2048                 */
    int           file_index;    /* which backing file           */
    u64           file_offset;   /* byte offset within that file */
} cd_track;

#define CD_MAX_TRACKS 99

/* ------------------------------------------------------------------------- */
/* A file on the disc, as found by the ISO9660 walk.                          */
/* ------------------------------------------------------------------------- */
typedef struct disc_file {
    char  path[256];      /* '/'-separated, uppercase, no ";1" version suffix */
    u32   lba;            /* absolute disc LBA, rebased from the ISO extent    */
    u32   size;
    bool  form2;          /* payload is 2324-byte Form 2 (streamed media)     */
} disc_file;

/* ------------------------------------------------------------------------- */
/* Opaque handle                                                              */
/* ------------------------------------------------------------------------- */
typedef struct disc disc;

/*
 * Open a disc from a path. Accepts:
 *   - a .cue file (the backing .bin files are resolved relative to it)
 *   - a .bin / .img file on its own (a single-track raw image is assumed)
 *   - a .iso file (2048-byte cooked sectors)
 *   - a directory containing an already-extracted Q2DATA tree
 * Returns Q2_ERR_UNSUPPORTED for formats recognised but not yet handled.
 */
q2_result disc_open(disc **out, const char *path);
void      disc_close(disc *d);

/* Human-readable description of what was opened, for logs and the UI. */
const char *disc_describe(const disc *d);

/* --- volume metadata ----------------------------------------------------- */
const char *disc_volume_id(const disc *d);
const char *disc_system_id(const disc *d);
u32         disc_volume_sectors(const disc *d);
/* PVD creation timestamp as the 17-byte ISO9660 digit string, NUL-terminated. */
const char *disc_creation_time(const disc *d);

/* --- tracks -------------------------------------------------------------- */
int             disc_track_count(const disc *d);
const cd_track *disc_track(const disc *d, int index);

/* --- filesystem ---------------------------------------------------------- */
int              disc_file_count(const disc *d);
const disc_file *disc_file_at(const disc *d, int index);
/* Case-insensitive; a leading '/' is optional and ";1" suffixes are ignored. */
const disc_file *disc_find(const disc *d, const char *path);

/* Read a whole file into a freshly allocated buffer. Form 2 files are returned
 * as their concatenated 2324-byte payloads, which is what a streamed-media
 * demuxer wants. */
q2_result disc_read_file(const disc *d, const char *path, q2_buf *out);

/* Raw sector access, for the media demuxers that need the subheader. Declared,
 * physically stored INDEX 00 pregap sectors are addressable too. `out` must
 * hold at least CD_SECTOR_RAW bytes. Returns the full 2352-byte sector when the
 * source has one, or a synthesised sector for cooked images. */
q2_result disc_read_raw_sector(const disc *d, u32 lba, u8 *out);

/* User-data payload of one sector, honouring its form. Writes the payload and
 * its length; `out` must hold CD_SECTOR_RAW bytes to be safe. */
q2_result disc_read_sector_payload(const disc *d, u32 lba, u8 *out, u32 *out_len);

/* ------------------------------------------------------------------------- */
/* Boot configuration (SYSTEM.CNF)                                            */
/* ------------------------------------------------------------------------- */
typedef struct disc_boot_info {
    char boot_path[64];   /* e.g. "cdrom:\\sles_015.34;1"  */
    char exe_name[32];    /* e.g. "SLES_015.34"            */
    char serial[16];      /* e.g. "SLES-01534"             */
    u32  tcb;
    u32  event;
    u32  stack;
} disc_boot_info;

q2_result disc_read_boot_info(const disc *d, disc_boot_info *out);

#endif /* Q2PSX_DISC_H */
