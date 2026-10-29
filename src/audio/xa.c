#include "xa.h"

#include <stdio.h>
#include <string.h>

#include "xa_impulse.h"

void q2_xa_resampler_reset(q2_xa_resampler *resampler)
{
    if (resampler) memset(resampler, 0, sizeof(*resampler));
}

u32 q2_xa_resample_sector(q2_xa_resampler *r, const s16 *input,
                         s16 *output, u32 output_capacity)
{
    u32 frame, consumed = 0, ch, tap;
    if (!r || !input || !output || output_capacity < XA_OUTPUT_FRAMES_PER_SECTOR)
        return 0;
    for (frame = 0; frame < XA_OUTPUT_FRAMES_PER_SECTOR; frame++) {
        for (ch = 0; ch < XA_CHANNELS; ch++) {
            s64 sum = 0;
            for (tap = 0; tap < 25; tap++)
                sum += (s32)r->history[ch][(r->position + 32 - 25 + tap) & 31] *
                       XA_IMPULSE[r->phase][tap];
            sum >>= 15;
            if (sum > 32767) sum = 32767;
            if (sum < -32768) sum = -32768;
            output[frame * XA_CHANNELS + ch] = (s16)sum;
        }
        r->phase += 6;
        if (r->phase >= 7) {
            r->phase -= 7;
            for (ch = 0; ch < XA_CHANNELS; ch++)
                r->history[ch][r->position] = input[consumed * XA_CHANNELS + ch];
            consumed++;
            r->position = (r->position + 1) & 31;
        }
    }
    return XA_OUTPUT_FRAMES_PER_SECTOR;
}

/* XA's predictor. Only four filters, unlike SPU-ADPCM's five. */
static const s32 XA_K0[4] = { 0, 60, 115,  98 };
static const s32 XA_K1[4] = { 0,  0, -52, -55 };

void q2_xa_decoder_reset(q2_xa_decoder *dec)
{
    if (dec)
        memset(dec, 0, sizeof(*dec));
}

u32 q2_xa_validate_sector(const u8 *adpcm)
{
    u32 bad = 0;
    int g, b;

    if (!adpcm)
        return 0;

    for (g = 0; g < XA_GROUPS_PER_SECTOR; g++) {
        const u8 *group = adpcm + (size_t)g * XA_SOUND_GROUP_SIZE;

        for (b = 0; b < XA_BLOCKS_PER_GROUP; b++) {
            /* Blocks 0..3 take their parameter from bytes 4..7, blocks 4..7
             * from bytes 12..15. Those are the authoritative copies. */
            u8 param  = (b < 4) ? group[4 + b] : group[12 + (b - 4)];
            u8 shift  = param & 0x0F;
            u8 filter = param >> 4;

            if (shift > 12 || filter > 3)
                bad++;
        }
    }
    return bad;
}

u32 q2_xa_decode_sector(q2_xa_decoder *dec, const u8 *adpcm, s16 *out, u32 out_capacity)
{
    u32 written = 0;
    int g, b, n;

    if (!dec || !adpcm || !out)
        return 0;

    for (g = 0; g < XA_GROUPS_PER_SECTOR; g++) {
        const u8 *group = adpcm + (size_t)g * XA_SOUND_GROUP_SIZE;

        for (b = 0; b < XA_BLOCKS_PER_GROUP; b++) {
            u8  param  = (b < 4) ? group[4 + b] : group[12 + (b - 4)];
            u8  shift  = param & 0x0F;
            u8  filter = param >> 4;
            /* Even blocks feed one output channel, odd blocks the other. */
            int ch     = b & 1;

            if (filter > 3)
                filter = 0;
            if (shift > 12)
                shift = 12;

            for (n = 0; n < XA_SAMPLES_PER_BLOCK; n++) {
                u32 word = q2_rd_u32(group + 16 + (size_t)n * 4);
                s32 nibble = (s32)((word >> (4 * b)) & 0x0F);
                s32 sample;

                /* Sign-extend the 4-bit residual from bit 3, then scale by
                 * (12 - shift). Both XA and SPU encode shift as attenuation. */
                if (nibble > 7)
                    nibble -= 16;

                /* Multiplication also defines the negative residual case in C. */
                sample = nibble * (1 << (12 - shift));
                sample += (XA_K0[filter] * dec->prev1[ch] +
                           XA_K1[filter] * dec->prev2[ch]) / 64;

                if (sample >  32767) sample =  32767;
                if (sample < -32768) sample = -32768;

                dec->prev2[ch] = dec->prev1[ch];
                dec->prev1[ch] = sample;

                /*
                 * Interleave to stereo. Within a group, blocks 0/1 carry the
                 * first 28 frames, 2/3 the next 28, and so on, so the output
                 * frame index is (b/2)*28 + n and the channel is b&1.
                 */
                {
                    u32 frame = (u32)((b / 2) * XA_SAMPLES_PER_BLOCK + n);
                    u32 slot  = (u32)(g * (XA_BLOCKS_PER_GROUP / 2) * XA_SAMPLES_PER_BLOCK
                                      + frame);
                    u32 index = slot * 2 + (u32)ch;

                    if (index >= out_capacity)
                        continue;

                    out[index] = (s16)sample;
                    if (index + 1 > written)
                        written = index + 1;
                }
            }
        }
    }

    return written;
}

/* ------------------------------------------------------------------------- */
/* The other direction                                                        */
/* ------------------------------------------------------------------------- */
void q2_xa_encoder_reset(q2_xa_encoder *enc)
{
    if (enc)
        memset(enc, 0, sizeof(*enc));
}

/*
 * One block: 28 samples of one channel, at one filter and one shift.
 *
 * Every line here is the DECODER's line rearranged, and deliberately so — the
 * prediction, the truncating divide by 64, the shift-as-attenuation and the
 * clamp all have to be the decoder's exactly or the reconstruction the encoder
 * thinks it is producing is not the one that comes back. Returns the squared
 * error, and leaves the predictor where the decoder will leave it.
 */
static u64 xa_try_block(const s16 *src, u32 have, int filter, int shift,
                        s32 in1, s32 in2, u8 *nibbles,
                        s32 *out1, s32 *out2)
{
    const s32 step = (s32)1 << (12 - shift);
    u64 err = 0;
    u32 i;

    for (i = 0; i < XA_SAMPLES_PER_BLOCK; i++) {
        s32 want = (i < have) ? (s32)src[(size_t)i * 2] : 0;
        s32 pred = (XA_K0[filter] * in1 + XA_K1[filter] * in2) / 64;
        s32 res  = want - pred;
        s32 q, rec, d;

        /* Round to nearest rather than truncate: a half-step of bias per
         * sample is a DC offset the predictor then chases. */
        q = (res >= 0) ? (res + step / 2) / step
                       : -((-res + step / 2) / step);
        if (q >  7) q =  7;
        if (q < -8) q = -8;

        rec = q * step + pred;
        if (rec >  32767) rec =  32767;
        if (rec < -32768) rec = -32768;

        nibbles[i] = (u8)(q & 0x0F);
        d = rec - want;
        err += (u64)((s64)d * d);

        in2 = in1;
        in1 = rec;
    }

    *out1 = in1;
    *out2 = in2;
    return err;
}

/*
 * Which shift to start looking at.
 *
 * A residual has to survive `nibble << (12 - shift)` with the nibble in -8..7,
 * so the step must be at least |residual| / 7 — and a LARGER shift is a FINER
 * step, as in SPU-ADPCM. This estimates the residuals against the source rather
 * than the reconstruction, so it can be off by one either way; the caller tries
 * the neighbours.
 */
static int xa_guess_shift(const s16 *src, u32 have, int filter, s32 in1, s32 in2)
{
    s32 peak = 0;
    int shift;
    u32 i;

    for (i = 0; i < XA_SAMPLES_PER_BLOCK; i++) {
        s32 want = (i < have) ? (s32)src[(size_t)i * 2] : 0;
        s32 pred = (XA_K0[filter] * in1 + XA_K1[filter] * in2) / 64;
        s32 res  = want - pred;

        if (res < 0) res = -res;
        if (res > peak) peak = res;

        in2 = in1;
        in1 = want;
    }

    for (shift = 12; shift > 0; shift--)
        if ((s32)7 * ((s32)1 << (12 - shift)) >= peak)
            break;

    return shift;
}

void q2_xa_encode_sector(q2_xa_encoder *enc, const s16 *pcm, u32 frames,
                         u8 *adpcm)
{
    int g, b;

    if (!enc || !adpcm)
        return;

    memset(adpcm, 0, XA_SECTOR_ADPCM_BYTES);

    for (g = 0; g < XA_GROUPS_PER_SECTOR; g++) {
        u8 *group = adpcm + (size_t)g * XA_SOUND_GROUP_SIZE;

        for (b = 0; b < XA_BLOCKS_PER_GROUP; b++) {
            /* The decoder's own addressing, read backwards: block b of group g
             * is channel b&1, frames (b/2)*28 into the group's 112. */
            int ch    = b & 1;
            u32 first = (u32)(g * (XA_BLOCKS_PER_GROUP / 2) *
                              XA_SAMPLES_PER_BLOCK +
                              (b / 2) * XA_SAMPLES_PER_BLOCK);
            u32 have  = (pcm && frames > first) ? frames - first : 0;
            const s16 *src = pcm ? pcm + (size_t)first * 2 + ch : NULL;
            u8  best_nib[XA_SAMPLES_PER_BLOCK];
            u64 best_err = (u64)-1;
            int best_f = 0, best_s = 12, f;
            s32 best1 = enc->prev1[ch], best2 = enc->prev2[ch];

            if (have > XA_SAMPLES_PER_BLOCK)
                have = XA_SAMPLES_PER_BLOCK;

            for (f = 0; f < 4; f++) {
                int guess = xa_guess_shift(src, have, f,
                                           enc->prev1[ch], enc->prev2[ch]);
                int k;

                /*
                 * The guess and its two neighbours. One step coarser is the
                 * safety net for a residual the source-predicted estimate
                 * underestimated; one finer sometimes wins outright because
                 * the feedback keeps the residuals smaller than the estimate.
                 */
                for (k = -1; k <= 1; k++) {
                    int shift = guess + k;
                    u8  nib[XA_SAMPLES_PER_BLOCK];
                    s32 p1, p2;
                    u64 err;

                    if (shift < 0 || shift > 12)
                        continue;

                    err = xa_try_block(src, have, f, shift,
                                       enc->prev1[ch], enc->prev2[ch],
                                       nib, &p1, &p2);
                    if (err < best_err) {
                        best_err = err;
                        best_f   = f;
                        best_s   = shift;
                        best1    = p1;
                        best2    = p2;
                        memcpy(best_nib, nib, sizeof(nib));
                    }
                }
            }

            enc->prev1[ch] = best1;
            enc->prev2[ch] = best2;

            /* The parameter byte, in both of its copies. Bytes 4..7 and 12..15
             * are the authoritative ones and 0..3 / 8..11 are the mirror the
             * standard asks for; writing only one of them produces a file some
             * players read and others do not. */
            {
                u8 param = (u8)(((best_f & 3) << 4) | (best_s & 0x0F));
                int slot = (b < 4) ? b : b - 4;
                int base = (b < 4) ? 0 : 8;

                group[base + slot]     = param;
                group[base + 4 + slot] = param;
            }

            {
                u32 n;

                for (n = 0; n < XA_SAMPLES_PER_BLOCK; n++) {
                    u8 *word = group + 16 + (size_t)n * 4;
                    u32 v    = q2_rd_u32(word);

                    v |= (u32)(best_nib[n] & 0x0F) << (4 * b);
                    q2_wr_u32(word, v);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Track access                                                               */
/* ------------------------------------------------------------------------- */
q2_result q2_xa_track_open(q2_xa_track *out, const disc *d, char letter, u8 channel)
{
    char path[64];
    const disc_file *f;

    if (!out || !d || channel >= XAI_CHANNEL_COUNT)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    snprintf(path, sizeof(path), "Q2DATA/AUD/QUAKE_%c.XAI", letter);

    f = disc_find(d, path);
    if (!f)
        return Q2_ERR_NOT_FOUND;

    out->disc    = d;
    out->channel = channel;
    out->first_lba = f->lba;

    /* The directory size is a Form 1 style byte count, so derive the sector
     * span from it rather than from the Form 2 payload size. */
    out->sector_count = (f->size + CD_SECTOR_FORM1 - 1) / CD_SECTOR_FORM1;

    return Q2_OK;
}

u32 q2_xa_track_read(q2_xa_track *track, q2_xa_decoder *dec,
                     u32 *cursor, s16 *out, u32 out_capacity)
{
    if (!track || !dec || !cursor || !out)
        return 0;

    while (*cursor < track->sector_count) {
        u8 raw[CD_SECTOR_RAW];
        u32 index = (*cursor)++;
        u8 channel, submode;

        if (disc_read_raw_sector(track->disc, track->first_lba + index, raw) != Q2_OK)
            return 0;

        channel = raw[17];
        submode = raw[18];

        /* Only real-time Form 2 audio for our own channel. Everything else is
         * either another track or post-EOF filler, and the filler is NOT zeroed
         * on this disc, so decoding it would emit noise. */
        if (channel != track->channel)
            continue;
        if (!(submode & CD_SUBMODE_AUDIO) || !(submode & CD_SUBMODE_FORM2))
            continue;

        {
            u32 n = q2_xa_decode_sector(dec, raw + 24, out, out_capacity);

            /* An EOF sector is the last real audio in this channel. Decode it,
             * then stop the caller advancing into the filler behind it. */
            if (submode & CD_SUBMODE_EOF)
                *cursor = track->sector_count;

            return n;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* The CD volume                                                              */
/* ------------------------------------------------------------------------- */

s16 q2_xa_cd_volume_step(s16 live, s16 target)
{
    /*
     * 0x800718F8..0x8007194C, instruction for instruction:
     *
     *     lh   a2, actcdvol      lh   a0, targetcdvol
     *     slt  v0, a2, a0        below the target?
     *     addiu v0, v1, 4        ...then four up, stored with `sh`
     *     slt  v0, a0, v0        and if that went past it,
     *     sh   a1, actcdvol      take the target outright
     *
     * with the mirrored arm at 0x80071928 doing the same downward. Equal
     * values fall through both `slt`s and nothing is written.
     *
     * The step is four and it is not scaled by anything: at the default target
     * of 96 (the MUSIC slider's 48, doubled) a fade is 24 calls, and at the
     * maximum 254 it is 64.
     */
    if (live < target) {
        s16 up = (s16)(live + 4);

        return (target < up) ? target : up;
    }

    if (target < live) {
        s16 down = (s16)(live - 4);

        return (down < target) ? target : down;
    }

    return live;
}

void q2_xa_cd_mix(s16 *pcm, u32 samples, s32 volume, bool stereo)
{
    u32 i;

    if (!pcm)
        return;

    if (volume < 0)
        volume = 0;
    if (volume > 255)
        volume = 255;      /* 0x800719D0's clamp */

    if (!stereo) {
        /*
         * 0x80071A08..0x80071A1C writes volume/2 into all four mix bytes, so
         * each input contributes at half the requested gain. The halving is on the
         * VOLUME and truncates toward zero — `srl v0, v1, 31 / addu / sra v0,
         * v0, 1` is the compiler's signed divide by two — which is why this is
         * not tidied into ((l + r) / 2 * volume). Each product has seven
         * fractional bits; truncate it before adding the other channel.
         */
        s32 half = volume / 2;

        for (i = 0; i + 1 < samples; i += XA_CHANNELS) {
            s32 m = ((pcm[i] * half) >> 7) + ((pcm[i + 1] * half) >> 7);
            if (m > 32767) m = 32767;
            if (m < -32768) m = -32768;

            pcm[i]     = (s16)m;
            pcm[i + 1] = (s16)m;
        }
        return;
    }

    /* Stereo: left-to-left and right-to-right carry the volume and both cross
     * terms are zero (0x800719F4..0x80071A04), i.e. each channel scaled on its
     * own. The controller's unity byte is 128; 255 is nearly double gain.
     * Saturation happens in the CD path before SPU input gain and mixing. */
    if (volume == 128)
        return;

    for (i = 0; i < samples; i++) {
        s32 sample = (pcm[i] * volume) >> 7;
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        pcm[i] = (s16)sample;
    }
}
