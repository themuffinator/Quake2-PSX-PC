/*
 * movie.c — see movie.h.
 */
#include "movie.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* The sector window                                                          */
/* ------------------------------------------------------------------------- */
/*
 * Why the window is aligned to eight.
 *
 * `q2_stx_frame_next` decides what a sector IS from its index — slot 7 of every
 * eight is audio — so a window that did not begin on a multiple of eight would
 * hand the demuxer sectors whose phase is wrong and it would step over the
 * wrong ones. Aligning the window is one line here and keeps the demuxer the
 * single verified one rather than a second copy that streams.
 */
/* A frame is at most 6 video sectors and they can straddle an audio slot, so
 * this many sectors after the cursor is always enough to hold a whole one. */
#define Q2_MOVIE_HEADROOM 8u

static bool movie_window_load(q2_movie *m, u32 sector)
{
    u32 base = sector & ~(Q2_STX_INTERLEAVE - 1u);
    u32 i;

    /*
     * The window must hold the cursor AND enough after it for a whole frame.
     *
     * Holding only the cursor is not enough and the bug it causes is quiet: a
     * frame beginning near the end of the window cannot be assembled, the
     * demuxer says so, and a reloading player that simply moves to the next
     * window has silently DROPPED that frame. With a 32-sector window and a
     * ~5-sector frame that is one frame in six — a film that plays, and stutters
     * on a beat you would have to count to notice.
     */
    if (m->window_have && sector >= m->window_base &&
        sector + Q2_MOVIE_HEADROOM <= m->window_base + m->window_have)
        return true;
    /* ...unless the file simply ends there, in which case this IS all of it. */
    if (m->window_have && sector >= m->window_base &&
        sector < m->window_base + m->window_have &&
        m->window_base + m->window_have >= m->sector_count)
        return true;

    if (base >= m->sector_count)
        return false;

    m->window_base = base;
    m->window_have = 0;
    memset(m->window, 0, sizeof(m->window));

    for (i = 0; i < Q2_MOVIE_WINDOW && base + i < m->sector_count; i++) {
        u8  payload[CD_SECTOR_RAW];
        u32 len = 0;

        if (disc_read_sector_payload(m->disc, m->first_lba + base + i,
                                     payload, &len) != Q2_OK)
            break;
        /*
         * Form 1 here and Form 2 in the audio slots, so copy what fits and let
         * the audio path re-read its own sectors: this window exists to feed
         * the VIDEO demuxer, which wants a flat 2048-byte stride.
         */
        memcpy(m->window + (size_t)i * Q2_STX_SECTOR_SIZE, payload,
               len < Q2_STX_SECTOR_SIZE ? len : Q2_STX_SECTOR_SIZE);
        m->window_have++;
    }

    return m->window_have > 0;
}

/*
 * The picture rate, read off the film rather than assumed.
 *
 * The drive delivers 150 sectors a second whichever disc it is, and what the
 * encoder chose is how many of them a frame takes: six on the PAL disc, which
 * is 25 fps, and five on the NTSC one, which is 30 — the same films, the same
 * sector counts and running times, re-encoded to the other standard's rate.
 * So the distance between the first sectors of successive frames, averaged
 * across the window, is the whole answer.
 */
static double movie_measure_fps(const q2_movie *m)
{
    u32 i, first_s = 0, last_s = 0, first_f = 0, last_f = 0, spf;
    bool have = false;

    for (i = 0; i < m->window_have; i++) {
        q2_stx_header h;

        if (q2_stx_sector_is_audio(m->window_base + i))
            continue;
        if (!q2_stx_header_read(m->window + (size_t)i * Q2_STX_SECTOR_SIZE, &h) ||
            h.chunk_index != 0)
            continue;
        if (!have) {
            first_s = last_s = i;
            first_f = last_f = h.frame_number;
            have = true;
        } else if (h.frame_number > last_f) {
            last_s = i;
            last_f = h.frame_number;
        }
    }

    if (!have || last_f == first_f)
        return Q2_MOVIE_FPS;

    spf = (u32)((double)(last_s - first_s) / (double)(last_f - first_f) + 0.5);
    return spf ? Q2_MOVIE_SECTORS_PER_SECOND / (double)spf : Q2_MOVIE_FPS;
}

/* ------------------------------------------------------------------------- */
bool q2_movie_open(q2_movie *m, const disc *d, const char *path)
{
    const disc_file *f;

    if (!m || !d || !path)
        return false;

    f = disc_find(d, path);
    if (!f || f->size < Q2_STX_SECTOR_SIZE)
        return false;

    memset(m, 0, sizeof(*m));
    m->disc         = d;
    m->first_lba    = f->lba;
    m->sector_count = (f->size + Q2_STX_SECTOR_SIZE - 1) / Q2_STX_SECTOR_SIZE;
    q2_xa_decoder_reset(&m->xa);

    /* One sector is enough to say whether this is a film at all. */
    if (!movie_window_load(m, 0))
        return false;
    {
        q2_stx_header h;

        if (!q2_stx_header_read(m->window, &h))
            return false;
    }

    m->fps = movie_measure_fps(m);
    return true;
}

u32 q2_movie_retail_length(const char *file)
{
    /*
     * The three call sites, transcribed. Kept as a table rather than folded
     * into the player because these are the MODULES' numbers and not this
     * port's: a disc whose modules said something else would want a different
     * row, and a film nobody's module names has no row at all.
     *
     * And the NTSC disc's modules DO say something else. Its films are named
     * without the P and run at 30 fps, and QLOGOS's `li a1` (module+0x960 and
     * +0x9D4) and QFRONT's (+0x1D38, and the dead copy at +0xD608) carry the
     * frame counts over again: the cut lands within half a second of the same
     * point on both — 51.2 s into the intro, 60.0 s (PAL) and 60.4 s (NTSC)
     * into the outro, 98.3 s into the reel.
     */
    static const struct { const char *file; u32 frames; } k_len[] = {
        { "TAKE1BP.STX",  1281u },   /* QFMV,   "Intro FMV"      */
        { "OUTRO1P.STX",  1500u },   /* QFMV,   "Extro FMV"      */
        { "ROGUEINP.STX", 2457u },   /* QFRONT, the opening reel */
        { "TAKE1B.STX",   1538u },   /* ...and SLUS-00757's      */
        { "OUTRO1.STX",   1811u },
        { "ROGUEIN1.STX", 2949u }
    };
    u32 i;

    if (!file)
        return 0;

    for (i = 0; i < sizeof(k_len) / sizeof(k_len[0]); i++) {
        const char *a = file, *b = k_len[i].file;

        while (*a && *b) {
            char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;

            if (ca != *b)
                break;
            a++; b++;
        }
        if (!*a && !*b)
            return k_len[i].frames;
    }

    return 0;
}

const char *q2_movie_start_reel(const disc *d)
{
    /*
     * QFRONT's literal at module+0xDC4, in each build: SLES-01534 names the
     * PAL film and SLUS-00757 `ROGUEIN1.STX`. The module is loaded after the
     * reel is chosen here, so the disc is asked which of the two it carries.
     */
    static const char *const k_reel[] = { "ROGUEINP.STX", "ROGUEIN1.STX" };
    size_t i;

    for (i = 0; d && i < sizeof(k_reel) / sizeof(k_reel[0]); i++) {
        char path[64];

        snprintf(path, sizeof(path), "Q2DATA/MOVIES/%s", k_reel[i]);
        if (disc_find(d, path))
            return k_reel[i];
    }
    return k_reel[0];
}

bool q2_movie_advance(q2_movie *m, double dt, u8 *rgb)
{
    double fps, due;
    u32 target;

    if (!m || !rgb || m->finished)
        return false;

    if (dt > 0.0) m->clock += dt;
    fps = m->fps > 0.0 ? m->fps : Q2_MOVIE_FPS;
    due = m->clock * fps + 1.0 + 0.0000001;
    target = due < 4294967295.0 ? (u32)due : 0xFFFFFFFFu;
    if (m->frame_valid && !m->frame_pending && target <= m->frame.number)
        return false;

    /* The old scheduler used frames_shown as its clock, so after a stall it
     * decoded every missed frame and permanently lagged behind the audio. */
    for (;;) {
        size_t local;
        u32 blocks = 0, bits = 0;

        if (!m->frame_pending) {
            if (!movie_window_load(m, m->cursor)) {
                m->finished = true;
                return false;
            }

            local = m->cursor - m->window_base;
            if (!q2_stx_frame_next(m->window,
                                   (size_t)m->window_have * Q2_STX_SECTOR_SIZE,
                                   &local, &m->frame)) {
                /* The window holds a whole frame after the cursor. No frame
                 * means the end of VIDEO; audio can continue in later slots. */
                m->finished = true;
                return false;
            }

            m->cursor = m->window_base + (u32)local;
            m->frame_pending = true;
        }
        if (m->frame.number > target) return false;
        m->frame_pending = false;

        /*
         * The console's stop point, checked exactly where it checks it: on the
         * frame's own number, before the frame is decoded, so the frame that
         * carries the limit is not shown (movie.h).
         */
        if (m->frame_limit && m->frame.number >= m->frame_limit) {
            m->finished = true;
            return false;
        }

        if (m->frame.number < target) {
            m->frames_skipped++;
            continue;
        }

        if (!q2_stx_frame_decode(&m->frame, rgb, &blocks, &bits))
            continue;   /* a frame that will not decode is skipped, not fatal */

        m->frame_valid = true;
        m->frames_shown++;
        return true;
    }
}

u32 q2_movie_audio(q2_movie *m, s16 *out, u32 max_samples)
{
    u32 written = 0;

    if (!m || !out)
        return 0;

    while (written + XA_FRAMES_PER_SECTOR * 2 <= max_samples) {
        u8  payload[CD_SECTOR_RAW];
        u32 len = 0;

        /* Step to this film's next audio slot. */
        while (m->audio_cursor < m->sector_count &&
               !q2_stx_sector_is_audio(m->audio_cursor))
            m->audio_cursor++;
        if (m->audio_cursor >= m->sector_count)
            break;

        if (disc_read_sector_payload(m->disc,
                                     m->first_lba + m->audio_cursor,
                                     payload, &len) != Q2_OK)
            break;
        m->audio_cursor++;

        /*
         * A Form 2 payload is 2324 bytes and only 2304 of them are ADPCM; the
         * other 20 are unused. Feeding the decoder 2324 drifts (xa.h). A slot
         * that came back short is not audio and is skipped rather than decoded,
         * which is the same rule the music path uses on post-EOF filler.
         */
        if (len < XA_SECTOR_ADPCM_BYTES)
            continue;
        if (q2_xa_validate_sector(payload) != 0)
            continue;

        written += q2_xa_decode_sector(&m->xa, payload, out + written,
                                       max_samples - written);
    }

    return written;
}
