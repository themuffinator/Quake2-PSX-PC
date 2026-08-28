/*
 * vag.h — SNDVRAM.DAT's sound bank: VAG headers and SPU-ADPCM decoding.
 *
 * Despite its name, SNDVRAM.DAT holds two unrelated sections. The first is
 * texture/VRAM images; the second, starting at the header's `ofs_sound_bank`,
 * is the block of sample data the console uploaded verbatim into its 512 KB of
 * sound RAM. This file handles the second.
 *
 * ---------------------------------------------------------------------------
 * Bank layout (offsets relative to ofs_sound_bank)
 * ---------------------------------------------------------------------------
 *     u32 offsets[num_entries];
 *
 * The table is self-describing in the same idiom the rest of the disc uses:
 * offsets[0] is the table's own byte size, so num_entries == offsets[0]/4, and
 * the final entry is a sentinel equal to the section size. Entries 0..n-2 point
 * at VAG headers. Confirmed across 49 files and 2,524 table entries.
 *
 * num_sounds == num_entries - 1, observed 2..87, never above the runtime clamp
 * of 96. Note that 54 entries disc-wide are aliases: two table slots pointing at
 * the same sample.
 *
 * Runtime sound IDs are 1-based — slot 0 is a null-sound placeholder — so sound
 * id N is bank index N-1.
 *
 * ---------------------------------------------------------------------------
 * VAG header — 48 bytes, and its integers are BIG endian
 * ---------------------------------------------------------------------------
 * This is the one place on the disc where byte order flips. Sony's VAG format
 * was defined big-endian and the tooling emitted it that way even for a
 * little-endian console. The reading is proven by construction: big-endian
 * yields exactly two sample rates (11025 and 22050) and 16-aligned body sizes,
 * while little-endian yields values in the hundreds of millions.
 *
 *     0x00  char[4]  'VAGp'
 *     0x04  u32 be   version, always 0x20
 *     0x08  u32 be   reserved, always 0
 *     0x0C  u32 be   data_size, always a multiple of 16
 *     0x10  u32 be   sample_rate, only 11025 or 22050
 *     0x14  u8[12]   reserved, all zero
 *     0x20  char[16] name, NUL-padded
 *
 * The engine truncated names to 11 characters plus a terminator at load, so the
 * runtime name is not always the stored one.
 *
 * ---------------------------------------------------------------------------
 * SPU-ADPCM — 16-byte blocks, 28 samples each
 * ---------------------------------------------------------------------------
 *     0x00  u8      bits 0-3 right-shift, bits 4-7 predictor filter index
 *     0x01  u8      bit0 End, bit1 Repeat, bit2 LoopStart
 *     0x02  u8[14]  28 signed 4-bit residuals, low nibble first
 *
 * Verified over 1,167,540 blocks: zero invalid flag bytes, zero invalid shifts,
 * zero invalid filter indices.
 *
 * Two decoder traps, both of which produce plausible-sounding but wrong output:
 *
 *   - Shift can be 0 or 1. A decoder that assumes the usual 2..12 range will
 *     mis-scale those blocks rather than fail loudly.
 *   - One-shot samples end two ways. 2,181 have flags==1 at block nb-2 followed
 *     by a 0x07 terminator; 176 have flags==1 followed by a plain flags==0 block
 *     with no terminator at all. A decoder that stops only on the terminator
 *     overruns by one block on those 176.
 */
#ifndef Q2PSX_VAG_H
#define Q2PSX_VAG_H

#include "disc.h"
#include "q2psx.h"

/* ------------------------------------------------------------------------- */
/* Sound-request pitch and the SPU rate                                       */
/* ------------------------------------------------------------------------- */
/*
 * A voice's playback pitch is not simply its VAG header rate. The engine
 * first quantises the header rate into the SPU's 0x1000 == 44100 Hz pitch
 * domain, then scales it by a per-SOUND-REQUEST modifier over 32. The default
 * modifier is 35: 1.09375, about one and a half semitones up.
 *
 * A player that ignores it runs every effect in the game 9.375% flat, which is
 * audible as a general dullness rather than as any one wrong sound — which is
 * why it survived a pass that fixed which sounds play and when.
 *
 * The executable settles the whole state transition:
 *
 *   0x80071BF8  initialise byte +1/+4/+5 to 35 and clear flags
 *   0x80072B80  add a nonzero signed delta to current (byte storage wraps)
 *   0x80073B14  upper = current + a1, lower = current + a2, set flag bit 0
 *   0x8007270C  randomising start: current = lower + rand() % (upper-lower)
 *   0x80072A00  direct start: copy current without drawing a random number
 *   0x8007293C  copy current << 3 into the voice pitch fields
 *   0x800724EC  floor(sample_rate * 0x1000 / 44100), once at sample load
 *   0x80071CA0  floor(base_pitch * (current << 3) / 256), at voice update
 *
 * Bounds are therefore upper-EXCLUSIVE. The five core-player registrations at
 * 0x8003C2D4..0x8003C328 configure `pla_gasp1` and `pla_drown1` as 30..38,
 * `pla_burn1` and `pla_burn2` as 33..34, and `pla_wade3` as 32..37. Weapon
 * setup proves why this cannot be a filename lookup: two distinct request
 * records for `wep_machgf1b` get different ranges, and the +24/+16 clone is
 * subsequently played through the DIRECT path, which bypasses its variation.
 * Creature modules can own still more request records for that same filename.
 *
 * This state deliberately carries no name. A later caller must reconstruct
 * which RETAIL REQUEST and which START PATH raised an event; assigning pitch by
 * VAG filename would merge records the executable keeps separate.
 */
#define Q2_SFX_PITCH_UNITY       32u
#define Q2_SFX_PITCH_DEFAULT     35u
#define Q2_SPU_PITCH_ONE       4096u
#define Q2_SPU_OUTPUT_RATE    44100u

typedef struct q2_sfx_pitch {
    u8   current;
    u8   upper;          /* exclusive when `varying` is true */
    u8   lower;
    bool varying;
} q2_sfx_pitch;

/* The exact default request state written by 0x80071BF8. */
void q2_sfx_pitch_init(q2_sfx_pitch *pitch);

/* 0 is the executable's "leave unchanged" sentinel (0x80073A34). */
void q2_sfx_pitch_set(q2_sfx_pitch *pitch, s32 modifier);

/* The identical exports at 0x80072B80 and 0x80072C00 add a nonzero delta and
 * store the low byte. This deliberately retains that byte-wrap behaviour. */
void q2_sfx_pitch_add(q2_sfx_pitch *pitch, s32 delta);

/* Configure the two signed offsets used by 0x80073B14. Invalid or empty
 * ranges are rejected without changing `pitch`; every retail caller is valid. */
bool q2_sfx_pitch_set_range(q2_sfx_pitch *pitch,
                            s32 upper_delta, s32 lower_delta);

/* Whether beginning this request through `randomising_path` needs a rand()
 * draw. Check this BEFORE advancing the caller's RNG: the direct path and a
 * fixed request consume no random number in the executable. */
bool q2_sfx_pitch_needs_random(const q2_sfx_pitch *pitch,
                               bool randomising_path);

/* Begin one voice. The randomising path mutates `current`, just as byte +1 in
 * the retail request record is overwritten; the direct path returns it as-is.
 * `random_value` is the already-drawn non-negative rand() result. */
u8 q2_sfx_pitch_begin(q2_sfx_pitch *pitch, bool randomising_path,
                      u32 random_value);

/* The exact two-stage console conversion. The first result is the value sent
 * to the SPU. The second expresses the same effective rate as a 16.16 source
 * cursor step for a host mixer running at `output_rate`; zero is invalid. */
u32 q2_sfx_spu_pitch(u32 sample_rate, u8 modifier);
u32 q2_sfx_step_16_16(u32 sample_rate, u8 modifier, u32 output_rate);

#define VAG_HEADER_SIZE   48
#define SPU_BLOCK_SIZE    16
#define SPU_SAMPLES_PER_BLOCK 28

/* Total sound RAM, and the low region reserved for the capture buffers. */
#define SPU_RAM_SIZE      0x80000
#define SPU_RAM_RESERVED  0x800

enum {
    SPU_FLAG_END        = 1 << 0,
    SPU_FLAG_REPEAT     = 1 << 1,
    SPU_FLAG_LOOP_START = 1 << 2
};

typedef struct q2_vag {
    char      name[17];
    u32       sample_rate;
    u32       data_size;      /* ADPCM body bytes */
    const u8 *body;           /* borrowed         */
    bool      looping;
} q2_vag;

typedef struct q2_sound_bank {
    q2_buf  buf;              /* owns the whole SNDVRAM.DAT file */
    const u8 *section;        /* start of the sound bank section */
    u32     section_size;
    u32     count;            /* number of sounds (entries - 1)  */
    u32    *offsets;          /* owned; relative to `section`    */
} q2_sound_bank;

/* Load "<map>/SNDVRAM.DAT" and locate its sound bank. */
q2_result q2_sound_bank_load(q2_sound_bank *out, const disc *d, const char *map);
void      q2_sound_bank_free(q2_sound_bank *bank);

/* Decode the VAG header at bank index `index` (0-based; runtime id is index+1). */
bool q2_sound_bank_get(const q2_sound_bank *bank, u32 index, q2_vag *out);

/* Total ADPCM body bytes, for checking the bank fits in sound RAM. */
u32 q2_sound_bank_total_body(const q2_sound_bank *bank);

/*
 * Decode SPU-ADPCM to signed 16-bit PCM.
 *
 * `out` must hold at least (data_size / 16) * 28 samples. Returns the number of
 * samples written, stopping at the sample's true end so the trailing block of a
 * terminator-less one-shot is not emitted.
 */
u32 q2_spu_adpcm_decode(const u8 *adpcm, u32 size, s16 *out, u32 out_capacity);

/* ------------------------------------------------------------------------- */
/* One voice, decoded a block at a time                                       */
/*                                                                            */
/* The hardware never decodes a sample up front: a voice holds an address into */
/* sound RAM and the SPU decodes the next 16-byte block when it needs the next */
/* 28 samples. A mixer wants the same shape — twenty-four of these cost 24 * a  */
/* few dozen bytes instead of twenty-four whole samples — so the block step is  */
/* exposed here and `q2_spu_adpcm_decode` is written in terms of it.            */
/* ------------------------------------------------------------------------- */
typedef struct q2_spu_voice {
    const u8 *adpcm;      /* borrowed: the sample body, NOT owned      */
    u32       size;       /* body bytes                                */
    u32       off;        /* byte offset of the next block             */
    s32       prev1;      /* the predictor's two-sample history        */
    s32       prev2;
    bool      done;       /* the End flag was seen, or the body ran out */

    /*
     * WHERE A LOOP GOES BACK TO — the block whose flags carry LoopStart, or
     * block 0 when none does.
     *
     * On the SPU, End alone stops the voice and End|Repeat JUMPS to the loop
     * address and carries on. The decoder used to stop on End regardless of
     * Repeat, so all 118 of the disc's looping samples — the ambiences, the
     * plasma burn, the spark loops, exactly the sustained ones — played once
     * and were dropped.
     *
     * The predictor history is deliberately NOT reset across the loop point:
     * the hardware carries prev1/prev2 through, and clearing them puts a click
     * on every repeat.
     */
    u32       loop_off;
    bool      looped;     /* it has wrapped at least once              */
} q2_spu_voice;

/*
 * WHO STOPS A LOOPING VOICE. Nothing in this module does — that is the point
 * of a loop — and the client has no per-entity ownership to hang one off yet.
 * What bounds it today is the zone load: `client_voices_stop` silences every
 * voice before the sound bank is freed, because a playing voice borrows the
 * bank's buffer. So a looping ambience runs for the level and dies with it,
 * which is right for an ambience and is NOT right for a loop a script means to
 * stop early. Recorded rather than papered over with a timeout.
 */

/* Point a voice at a sample body. `adpcm` must outlive the voice — for a bank
 * sample that means the bank must not be freed while it is playing. */
void q2_spu_voice_start(q2_spu_voice *v, const u8 *adpcm, u32 size);

/*
 * Decode the next block into `out`, which must hold SPU_SAMPLES_PER_BLOCK
 * samples. Returns the number written, or 0 once the sample has ended.
 *
 * The End flag is acted on AFTER its block is emitted, which is what makes a
 * terminator-less one-shot stop in the right place — see the trap note above.
 */
u32 q2_spu_voice_block(q2_spu_voice *v, s16 *out);

/* Validate an ADPCM stream's block headers without decoding it. Returns the
 * number of structurally invalid blocks — zero is expected for real data. */
u32 q2_spu_adpcm_validate(const u8 *adpcm, u32 size);

#endif /* Q2PSX_VAG_H */
