/*
 * xa.h — CD-XA ADPCM: the streamed music in Q2DATA/AUD/QUAKE_[A-E].XAI.
 *
 * This is a completely different codec from the SPU-ADPCM in vag.h. Both are
 * 4-bit ADPCM with a two-tap predictor; XA uses
 * 128-byte "sound groups" carrying eight interleaved blocks, decodes stereo, and
 * runs at 37800 Hz off the CD rather than out of sound RAM.
 *
 * ---------------------------------------------------------------------------
 * How the five files are laid out
 * ---------------------------------------------------------------------------
 * Each .XAI holds FOUR independent stereo streams multiplexed by CD-XA channel
 * number, in strict one-sector round-robin:
 *
 *     sector_index % 4 == channel_num
 *
 * with zero violations across 66,608 audio sectors. So a track is every fourth
 * sector, and playing one means demultiplexing rather than reading a range.
 *
 * That gives 20 (file, channel) slots but only 19 distinct tracks: two channels
 * of one file are byte-identical.
 *
 * One audio sector holds 2016 stereo frames = 53.333 ms, and a channel receives
 * one sector in four at 75 sectors/s, i.e. 18.75/s — exactly real time at 1x
 * drive speed. That is not a coincidence; it is why the interleave is 4.
 *
 * ---------------------------------------------------------------------------
 * Two traps that produce plausible garbage
 * ---------------------------------------------------------------------------
 * 1. A Form 2 payload is 2324 bytes but only 2304 of them are ADPCM (18 groups
 *    x 128). The trailing 20 bytes are unused and are zero in all 70,663 audio
 *    sectors. A decoder that consumes 2324 will drift.
 *
 * 2. Null sectors are NOT zero-filled. After a channel emits its EOF sector its
 *    remaining slots carry stale mastering filler — on one file all 1,154 have
 *    non-zero user data and 479 duplicate the payload four sectors earlier.
 *    Decoding them produces noise. A player MUST gate on the submode and channel
 *    and never decode a sector it did not ask for.
 *
 * ---------------------------------------------------------------------------
 * Sound group — 128 bytes, 18 per sector
 * ---------------------------------------------------------------------------
 *     0x00  u8[4]   parameter copy (low)
 *     0x04  u8[4]   parameters for blocks 0..3
 *     0x08  u8[4]   parameter copy (high)
 *     0x0C  u8[4]   parameters for blocks 4..7
 *     0x10  u32[28] sample nibbles; block b sample n is (word[n] >> (4*b)) & 0xF
 *
 * A parameter byte is a shift in its low nibble (0..12) and a filter index in
 * its high nibble (0..3). Bytes 4..7 and 12..15 are the authoritative copies —
 * that is the CD-XA standard's rule, not a finding from this disc, though the
 * copies do agree in 98.2% of sampled groups and the disagreements fail every
 * other sanity check.
 *
 * Even-numbered blocks form one output channel and odd-numbered the other. The
 * separation is confirmed by signal analysis; which one is physically *left* is
 * a standard fact rather than something these bytes reveal.
 */
#ifndef Q2PSX_XA_H
#define Q2PSX_XA_H

#include "disc.h"
#include "q2psx.h"

#define XA_SECTOR_ADPCM_BYTES 2304   /* 18 * 128; NOT the full 2324 payload */
#define XA_SOUND_GROUP_SIZE   128
#define XA_GROUPS_PER_SECTOR  18
#define XA_BLOCKS_PER_GROUP   8
#define XA_SAMPLES_PER_BLOCK  28

/* Every audio sector on this disc reports stereo / 37800 Hz / 4-bit. */
#define XA_SAMPLE_RATE        37800
#define XA_CHANNELS           2
#define XA_OUTPUT_RATE        44100u

/* Frames (stereo sample pairs) produced by one sector. */
#define XA_FRAMES_PER_SECTOR  (XA_GROUPS_PER_SECTOR * XA_BLOCKS_PER_GROUP * XA_SAMPLES_PER_BLOCK / 2)
#define XA_OUTPUT_FRAMES_PER_SECTOR (XA_FRAMES_PER_SECTOR / 6 * 7)

#define XAI_CHANNEL_COUNT     4

/* Decoder state. XA is stride-dependent: each output channel carries its own
 * two-sample history across sectors, so this must persist for a whole track. */
typedef struct q2_xa_decoder {
    s32 prev1[XA_CHANNELS];
    s32 prev2[XA_CHANNELS];
} q2_xa_decoder;

void q2_xa_decoder_reset(q2_xa_decoder *dec);

/* CD controller interpolation, before its volume matrix. One 37,800 Hz
 * sector produces 2,352 stereo frames on the SPU's 44,100 Hz clock.
 * Initialize with reset, retain history across sectors, and reset on stream
 * changes. Input and output buffers must not overlap. */
typedef struct q2_xa_resampler {
    s16 history[XA_CHANNELS][32];
    u32 position, phase;
} q2_xa_resampler;

void q2_xa_resampler_reset(q2_xa_resampler *resampler);
/* Capacities and return value are stereo FRAMES, unlike the decoder below.
 * Insufficient output capacity leaves both state and output untouched. */
u32 q2_xa_resample_sector(q2_xa_resampler *resampler, const s16 *input,
                         s16 *output, u32 output_capacity);

/*
 * Decode one sector's 2304 ADPCM bytes into interleaved stereo PCM16.
 *
 * `out` must hold at least XA_FRAMES_PER_SECTOR * 2 samples. Returns the number
 * of samples written (frames * 2).
 */
u32 q2_xa_decode_sector(q2_xa_decoder *dec, const u8 *adpcm, s16 *out, u32 out_capacity);

/* Validate a sector's sound-group parameters without decoding. Returns the
 * count of structurally invalid blocks; zero is expected for real audio. */
u32 q2_xa_validate_sector(const u8 *adpcm);

/* ------------------------------------------------------------------------- */
/* The other direction                                                        */
/* ------------------------------------------------------------------------- */
/*
 * PCM to XA ADPCM, for writing a film's soundtrack (stxenc.h).
 *
 * The encoder carries the same two-sample history the decoder does and for the
 * same reason — a block's prediction reaches back across the block boundary and
 * across the SECTOR boundary — and it must feed that history the RECONSTRUCTED
 * sample rather than the one it was aiming at, or the error compounds and the
 * stream drifts away from the source over a few seconds. That is the one thing
 * about ADPCM that is easy to get subtly wrong and hard to hear until it is
 * badly wrong.
 *
 * Which filter and shift each block gets is the encoder's choice, not the
 * format's: all four filters and all thirteen shifts decode, so this tries the
 * plausible ones and keeps the pair with the least squared error.
 */
typedef struct q2_xa_encoder {
    s32 prev1[XA_CHANNELS];
    s32 prev2[XA_CHANNELS];
} q2_xa_encoder;

void q2_xa_encoder_reset(q2_xa_encoder *enc);

/*
 * Encode up to `frames` interleaved stereo frames into one sector's 2304 ADPCM
 * bytes. Fewer than XA_FRAMES_PER_SECTOR pads the rest with silence — which is
 * what the last sector of a film needs, since a sector is 53.333 ms and a film
 * does not end on one.
 */
void q2_xa_encode_sector(q2_xa_encoder *enc, const s16 *pcm, u32 frames,
                         u8 *adpcm);

/* ------------------------------------------------------------------------- */
/* Track access                                                               */
/* ------------------------------------------------------------------------- */

/* One demultiplexed music track: a (file, channel) pair. */
typedef struct q2_xa_track {
    const disc *disc;
    u32   first_lba;
    u32   sector_count;   /* total sectors in the file, all channels */
    u8    channel;
} q2_xa_track;

/* Open QUAKE_<letter>.XAI and select one of its four channels.
 * `letter` is 'A'..'E'; `channel` is 0..3. */
q2_result q2_xa_track_open(q2_xa_track *out, const disc *d, char letter, u8 channel);

/*
 * Read and decode the next sector belonging to this track's channel, starting
 * the search at `*cursor` (a sector index within the file) and advancing it.
 *
 * Returns the number of samples written, or 0 at end of stream. Sectors that
 * are not audio, not this channel, or are post-EOF filler are skipped rather
 * than decoded — see the null-sector trap above.
 */
u32 q2_xa_track_read(q2_xa_track *track, q2_xa_decoder *dec,
                     u32 *cursor, s16 *out, u32 out_capacity);

/* ------------------------------------------------------------------------- */
/* The CD volume                                                              */
/* ------------------------------------------------------------------------- */
/*
 * CdCheckMusic (0x800718E4, CDAUDIO.C) keeps TWO volumes, not one: a target at
 * 0x800B2E50 (`targetcdvol`) that the menu and the pause write, and a live
 * value at 0x800B2E74 (`actcdvol`) that it walks toward the target. The mix
 * handed to the drive at 0x80071A20 is built from the LIVE one, which is why a
 * pause fades the music out instead of cutting it.
 *
 * Both halves live here rather than in the client because both are properties
 * of the CD/XA stream — the SPU voice path has its own, separate volume — and
 * because the arithmetic is worth testing on its own.
 */

/*
 * One step of the ramp: 0x800718F8..0x8007194C. Four toward the target, or the
 * target itself if four would carry the live value past it. Halfword arithmetic
 * throughout, because the store at 0x80071914 is `sh`.
 */
s16 q2_xa_cd_volume_step(s16 live, s16 target);

/*
 * Apply the live volume to `samples` interleaved stereo samples, the way
 * 0x800719EC..0x80071A20 builds the four bytes of the CD mix.
 *
 * `stereo` is the STEREO setting at 0x800B333E. Non-zero: left-to-left and
 * right-to-right both carry `volume` and the two cross terms are ZERO
 * (0x800719F4..0x80071A04). Zero: ALL FOUR carry volume/2, so each output is
 * the sum of both channels at half level (0x80071A08..0x80071A1C) — a fold to
 * mono, which is what SetCDVolume's own pair of arms at 0x80071BB0/0x80071BC4
 * does too. The controller's unity byte is 128; 255 is nearly double gain.
 * Each product truncates seven fractional bits before summation and 16-bit
 * saturation. This is the CD matrix only, before the SPU's CD/main gains.
 */
void q2_xa_cd_mix(s16 *pcm, u32 samples, s32 volume, bool stereo);

#endif /* Q2PSX_XA_H */
