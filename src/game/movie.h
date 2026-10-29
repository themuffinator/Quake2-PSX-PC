/*
 * movie.h — playing a `.STX`: the schedule, the audio and the skip.
 *
 * `stx.h` decodes ONE frame and says so: who schedules frames, against which
 * clock, and what happens to the audio riding in the same file is not the
 * decoder's business. This is that part.
 *
 * ---------------------------------------------------------------------------
 * The clock is the container's, not the host's
 * ---------------------------------------------------------------------------
 * A `.STX` is an 8-sector interleave: slots 0-6 carry video and slot 7 carries
 * CD-XA ADPCM. One XA sector is 2016 stereo frames at 37800 Hz = 53.333 ms, and
 * the file gives one of those per eight sectors, so eight sectors of film is
 * 53.333 ms of sound and seven of picture. Two 320x192 frames fit in those
 * seven video sectors on the disc's cadence (6,5,5,5 sectors per frame), which
 * puts the picture at exactly 25.000 fps.
 *
 * That number is FORCED by the container and is not a choice this port makes.
 * A player therefore advances the film on its own accumulated time and not on
 * the game's tick, which is 30 Hz — driving one from the other would drop or
 * double every fifth frame.
 *
 * The NTSC disc's films are the same length in sectors and in seconds and are
 * packed five sectors to a frame instead of six (cadence 5,4,4,4), which the
 * same arithmetic makes exactly 30.000 fps. `q2_movie_open` measures which a
 * file is rather than being told.
 *
 * ---------------------------------------------------------------------------
 * What the audio needs that the music path does not
 * ---------------------------------------------------------------------------
 * The XA in a movie is the same codec as the streamed music (`xa.h`) with a
 * different multiplex: music is one channel in four of a `.XAI`, a movie is one
 * sector in eight of a `.STX`. The sound-group layout, the 2304-of-2324 trap
 * and the persistent per-channel history are all shared, so this uses `xa.h`'s
 * decoder and only replaces the sector selection.
 *
 * ---------------------------------------------------------------------------
 * Memory
 * ---------------------------------------------------------------------------
 * The three films are 15, 20 and 30 MB. They are read from the disc in a
 * WINDOW rather than whole — a ring of sectors refilled as the cursor advances
 * — because a 30 MB allocation to play a two-minute clip is the sort of thing
 * that works on a development machine and fails on someone else's.
 */
#ifndef Q2PSX_GAME_MOVIE_H
#define Q2PSX_GAME_MOVIE_H

#include "disc.h"
#include "q2psx.h"
#include "stx.h"
#include "xa.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Exactly 25.000 fps, forced by the interleave — see the header comment. The
 * PAL films' rate, and the fallback for a file too short to measure. */
#define Q2_MOVIE_FPS 25.0

/* What the drive delivers at double speed: 75 sectors a second, twice. */
#define Q2_MOVIE_SECTORS_PER_SECOND 150.0

/* How many sectors the reader holds at once. One frame is at most 6 video
 * sectors and they can straddle an audio slot, so 32 is several frames of
 * slack and 64 KB of memory. */
#define Q2_MOVIE_WINDOW 32u

typedef struct q2_movie {
    const disc *disc;
    u32   first_lba;
    u32   sector_count;
    u32   cursor;             /* next sector to consider, file-relative     */

    u8    window[Q2_MOVIE_WINDOW * Q2_STX_SECTOR_SIZE];
    u32   window_base;        /* file-relative sector index of window[0]    */
    u32   window_have;        /* sectors valid in the window                */

    q2_stx_frame frame;       /* the frame most recently demuxed            */
    bool  frame_valid;
    u32   frames_shown;
    u32   frames_skipped;
    bool  frame_pending;      /* demuxed frame waiting for its presentation time */

    /*
     * Stop when a frame's own number reaches this. 0 plays the file out.
     *
     * Not a convenience: it is an ARGUMENT of the console's player, and every
     * one of the three films is cut short by it. See `q2_movie_retail_length`.
     */
    u32   frame_limit;

    double clock;             /* seconds of film presented so far           */
    double fps;               /* measured at open: 25 on PAL, 30 on NTSC    */
    bool   finished;

    /* Audio: the same decoder the music uses, over a different multiplex. */
    q2_xa_decoder xa;
    u32   audio_cursor;       /* next sector to consider for audio          */
} q2_movie;

/*
 * Open `path` (e.g. "Q2DATA/MOVIES/OUTRO1P.STX") on `d`.
 *
 * Fails if the file is not there or is not a `.STX`. Nothing is decoded here;
 * the first frame arrives on the first `q2_movie_advance`.
 */
bool q2_movie_open(q2_movie *m, const disc *d, const char *path);

/*
 * Advance the film by `dt` seconds and decode whatever frame that lands on.
 *
 * Returns true when a NEW frame was decoded into `rgb` (which must hold
 * `Q2_STX_WIDTH * Q2_STX_HEIGHT * 3` bytes). Returns false when the film has
 * not moved on to the next frame yet — in which case the caller keeps showing
 * what it has — or when the film has ended, which `q2_movie_finished` reports.
 *
 * Frame numbers set presentation time. Demux and discard overdue frames, then
 * decode at most one due frame per call. A stalled caller catches up without
 * decoding the skipped pictures or slowing the soundtrack. Device clients
 * supply elapsed consumed-audio time; headless clients use the same sample clock.
 */
bool q2_movie_advance(q2_movie *m, double dt, u8 *rgb);

/* Has the film run out of frames? */
Q2PSX_INLINE bool q2_movie_finished(const q2_movie *m) { return m->finished; }

/*
 * How many frames the console plays of `file` — a bare name, "OUTRO1P.STX".
 * 0 for a film the disc's modules do not name.
 *
 * ---------------------------------------------------------------------------
 * Where this comes from, and why a film is not simply played out
 * ---------------------------------------------------------------------------
 * The player is a function in the shared LevelBin module and it takes five
 * arguments. QFMV's two call sites are
 *
 *     play("TAKE1BP.STX", 1281, 0x10000, 1, 255)      ; module 0x80100958
 *     play("OUTRO1P.STX", 1500, 0x10000, 1, 255)      ; module 0x801009CC
 *
 * and QFRONT's OPENING REEL — the film it plays half a second after a
 * difficulty is confirmed, not the title screen's attract loop — is
 *
 *     play("ROGUEINP.STX", 2457, 0x10000, 1, 255)     ; module 0x80101D4C
 *
 * That address matters, because there are two of it. `module+0xD5F8` is the
 * same call with the same five arguments and it has NO CALLERS anywhere in the
 * 118 KB module: an earlier build's entry point, left in the image, and the one
 * this project recorded for years. The live call sits inside 0x80101CD0, the
 * countdown the EASY, MEDIUM and HARD records arm — which is what makes this
 * film the opening of a new game. See `start_beat` in the client for the chain.
 *
 * The second argument is stored (module+0x7384) and read in the frame handler
 * at 0x80102A5C, where it is compared against the STR frame header's own
 * `frame_number` at +0x08:
 *
 *     lw   v1, 8(a1)            ; StrFrameHeader.frame_number
 *     lw   v0, 0x7384(v0)       ; the argument
 *     sltu v1, v1, v0           ; number < limit ?
 *     bne  v1, zero, +16        ; ...keep going
 *     sw   1, 0x7388(v1)        ; else: done
 *
 * — so it is a stop point, and the frame carrying that number is NOT shown.
 * The disc holds 1283, 1559 and 2459 frames, so the console plays 1,280 of the
 * intro, **1,499 of the 1,559 in the outro** — the last 2.4 seconds are on the
 * disc and are never seen — and 2,456 of the opening reel.
 *
 * The other three arguments are read the same way: 0x10000
 * is the size in bytes of each of the two VLC buffers, allocated by name at
 * 0x80102F0C / 0x80102F28 ("vlc buffer 0", "vlc buffer 1") and defaulting to
 * 0x20400 when zero; the 1 gates two engine calls. The 255 at sp+16 is the
 * movie's CD volume: the player ramps it over 16 picture iterations, then
 * fades out through 255/191/127/63 on four video fields before stopping the CD.
 */
u32 q2_movie_retail_length(const char *file);

/*
 * The opening reel's file name on this disc: `ROGUEINP.STX` on SLES-01534 and
 * `ROGUEIN1.STX` on SLUS-00757, each its own QFRONT's literal at module+0xDC4.
 */
const char *q2_movie_start_reel(const disc *d);

/*
 * Pull up to `max_samples` of interleaved stereo PCM16 out of the audio slots,
 * returning how many were written. Call it as often as the audio device needs
 * feeding; it is independent of the video clock, because on the console the two
 * are independent — the SPU plays the XA the drive delivers whatever the MDEC
 * is doing.
 */
u32 q2_movie_audio(q2_movie *m, s16 *out, u32 max_samples);

#ifdef __cplusplus
}
#endif

#endif /* Q2PSX_GAME_MOVIE_H */
