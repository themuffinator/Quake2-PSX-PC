/*
 * test_stxenc.c — the encoder, checked without a disc.
 *
 * `test_movie.c` checks the decoder's Huffman table for shape and prefix
 * freedom. This checks the other direction, and the thing it is really testing
 * is that the two directions are INVERSES — which is a stronger statement than
 * either one alone, and one that can be made with no film at all:
 *
 *   1. A synthetic frame encodes, and decodes back to something close to
 *      itself. Close, not equal: the quantiser is lossy by design. What would
 *      be a bug is a picture that comes back STRUCTURALLY wrong — a block in
 *      the wrong cell, the chroma planes swapped, the DC scaled — and every one
 *      of those costs tens of dB, so a PSNR floor catches them all at once.
 *
 *   2. Every frame satisfies `bs_num_codes`, computed the way the disc's own
 *      frames are checked. This is the check that settled the format, and an
 *      encoder that cannot satisfy it is not writing the format.
 *
 *   3. The container comes out with the cadence and the interleave the disc
 *      has: audio at slot 7 of every 8, and 6,5,5,5 video sectors per frame.
 *
 *   4. The ADPCM round-trips a signal that is NOT already ADPCM. Re-encoding
 *      the disc's own audio reproduces it exactly — which is a real result, but
 *      it is a result about a signal that already sits on the codec's lattice.
 *      A sine wave does not, so it is what says the quantiser works.
 *
 *   5. A built CD-XA sector satisfies its own EDC and Reed-Solomon parity.
 *      (That the same code reproduces the DISC's parity is checked by
 *      `q2psx-inspect movie encode`, which needs the disc.)
 */
#include "cdxa.h"
#include "stxenc.h"
#include "xa.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

static u8 g_src[Q2_STX_WIDTH * Q2_STX_HEIGHT * 3];
static u8 g_back[Q2_STX_WIDTH * Q2_STX_HEIGHT * 3];

/*
 * A frame with something in it.
 *
 * Deliberately not a flat field and not noise. A flat field encodes to DC-only
 * blocks and would pass with the AC path completely broken; noise is
 * incompressible and says nothing about a codec built for pictures. This has
 * smooth gradients, a hard edge, and coloured discs — so it exercises the DC,
 * the low ACs, the high ACs at the edge, and both chroma planes, and a
 * structural error in any of them is visible in the score.
 */
static void make_frame(u8 *rgb, u32 w, u32 h, u32 t)
{
    u32 x, y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            u8 *p = rgb + ((size_t)y * w + x) * 3;
            int cx = (int)x - (int)(60 + t * 3);
            int cy = (int)y - (int)(90);
            int r  = (int)(x * 255 / w);
            int g  = (int)(y * 255 / h);
            int b  = 40;

            if (x > w / 2)                       /* a hard vertical edge */
                b = 200;
            if (cx * cx + cy * cy < 34 * 34) {   /* a red disc          */
                r = 220; g = 30; b = 30;
            }
            cx = (int)x - (int)(240);
            cy = (int)y - (int)(60 + t * 2);
            if (cx * cx + cy * cy < 26 * 26) {   /* and a green one     */
                r = 20; g = 200; b = 60;
            }

            p[0] = (u8)r;
            p[1] = (u8)g;
            p[2] = (u8)b;
        }
    }
}

static double psnr(const u8 *a, const u8 *b, u32 n)
{
    double sum = 0.0;
    u32 i;

    for (i = 0; i < n; i++) {
        double d = (double)a[i] - (double)b[i];

        sum += d * d;
    }
    return (sum <= 0.0) ? 99.0
                        : 10.0 * log10(255.0 * 255.0 * (double)n / sum);
}

/*
 * The same thing on LUMA alone, and it is reported beside the RGB figure
 * because the two answer different questions.
 *
 * This test frame has saturated red and green discs on a gradient, and chroma
 * is stored at half resolution in both axes — so a hard COLOUR edge is lost to
 * the subsampling no matter how fine the quantiser is, and the RGB figure
 * barely moves between qscale 1 and qscale 16. That is the format working as
 * designed, not the encoder failing, and the luma figure is the one that shows
 * the quantiser doing its job: it falls steadily as qscale rises. Real footage
 * scores far higher on both, because it arrives already subsampled.
 */
static double psnr_luma(const u8 *a, const u8 *b, u32 pixels)
{
    double sum = 0.0;
    u32 i;

    for (i = 0; i < pixels; i++) {
        double ya = 0.299 * a[i * 3] + 0.587 * a[i * 3 + 1] +
                    0.114 * a[i * 3 + 2];
        double yb = 0.299 * b[i * 3] + 0.587 * b[i * 3 + 1] +
                    0.114 * b[i * 3 + 2];

        sum += (ya - yb) * (ya - yb);
    }
    return (sum <= 0.0) ? 99.0
                        : 10.0 * log10(255.0 * 255.0 * (double)pixels / sum);
}

/* ------------------------------------------------------------------------- */
/* The sink: keep every sector, so the container can be inspected             */
/* ------------------------------------------------------------------------- */
#define MAX_SECTORS 256u

static struct {
    u8  form[MAX_SECTORS];
    u32 len[MAX_SECTORS];
    u8 *flat;              /* a 2048-per-sector mirror, for the demuxer */
    u32 count;
} g_cap;

static bool capture(void *user, u32 index, q2_stx_form form,
                    const u8 *payload, u32 len)
{
    (void)user;
    if (index >= MAX_SECTORS)
        return false;

    g_cap.form[index] = (u8)form;
    g_cap.len[index]  = len;
    if (index + 1 > g_cap.count)
        g_cap.count = index + 1;

    memset(g_cap.flat + (size_t)index * Q2_STX_SECTOR_SIZE, 0,
           Q2_STX_SECTOR_SIZE);
    if (form != Q2_STX_FORM_AUDIO)
        memcpy(g_cap.flat + (size_t)index * Q2_STX_SECTOR_SIZE, payload,
               len < Q2_STX_SECTOR_SIZE ? len : Q2_STX_SECTOR_SIZE);
    return true;
}

/* ------------------------------------------------------------------------- */
static void test_frame_roundtrip(void)
{
    static q2_stx_transform t;
    static q2_stx_encoded   e;
    static q2_stx_frame     f;
    u32 want_blocks = ((Q2_STX_WIDTH + 15u) / 16u) *
                      ((Q2_STX_HEIGHT + 15u) / 16u) * 6u;
    u32 q;

    printf("frame round trip, %ux%u, %u blocks\n",
           Q2_STX_WIDTH, Q2_STX_HEIGHT, want_blocks);

    make_frame(g_src, Q2_STX_WIDTH, Q2_STX_HEIGHT, 0);
    CHECK(q2_stx_transform_frame(&t, g_src, Q2_STX_WIDTH, Q2_STX_HEIGHT),
          "the transform refused a %ux%u frame", Q2_STX_WIDTH, Q2_STX_HEIGHT);
    CHECK(t.blocks == want_blocks, "%u blocks transformed, wanted %u",
          t.blocks, want_blocks);

    for (q = 1; q <= 16; q *= 2) {
        u32 blocks = 0, bits = 0, words, half, codes;
        double p;

        CHECK(q2_stx_encode_at(&t, q, &e), "qscale %u would not encode", q);
        CHECK(e.size >= 8 && (e.size % 4) == 0,
              "qscale %u produced a %u-byte frame; the disc's are always a "
              "multiple of four", q, e.size);

        /* Hand it to the decoder exactly as a demuxed frame arrives. */
        memset(&f, 0, sizeof(f));
        f.number = 1;
        f.size   = e.size;
        f.qscale = (u16)q;
        f.width  = Q2_STX_WIDTH;
        f.height = Q2_STX_HEIGHT;
        f.num_codes = (u16)e.num_codes;
        memcpy(f.data, e.data, e.size);

        q2_stx_reset_stats();
        CHECK(q2_stx_frame_decode(&f, g_back, &blocks, &bits),
              "qscale %u did not decode back", q);
        CHECK(blocks == want_blocks,
              "qscale %u came back with %u of %u blocks", q, blocks,
              want_blocks);
        CHECK(q2_stx_fail_unmatched == 0 && q2_stx_fail_overrun == 0 &&
              q2_stx_fail_dry == 0,
              "qscale %u: %u unmatched, %u overran, %u dry", q,
              q2_stx_fail_unmatched, q2_stx_fail_overrun, q2_stx_fail_dry);

        /*
         * The frame's own DMA length, from what the DECODER counted — so this
         * is the encoder's arithmetic checked against the decoder's, which is
         * the same check the disc's 5,301 frames pass.
         */
        words = 2u * blocks + q2_stx_last_pairs;
        half  = (words + 1u) / 2u;
        codes = ((half + 31u) / 32u) * 32u;
        CHECK(codes == e.num_codes,
              "qscale %u states %u codes and decodes to %u", q, e.num_codes,
              codes);

        p = psnr(g_src, g_back, sizeof(g_src));
        printf("  qscale %2u: %5u bytes, %5u pairs, %3u escapes, "
               "PSNR %.2f dB rgb / %.2f dB luma\n", q, e.size, e.pairs,
               e.escapes, p,
               psnr_luma(g_src, g_back, Q2_STX_WIDTH * Q2_STX_HEIGHT));

        /*
         * A floor, not a target. Even at qscale 16 a structural error — a
         * transposed block, swapped chroma, a DC off by its quantiser — lands
         * in the teens, and correct-but-coarse lands in the thirties. 25 dB
         * separates them with room to spare and does not turn a quantiser
         * tweak into a failing test.
         */
        CHECK(p > 25.0, "qscale %u round-trips at only %.2f dB", q, p);
    }
}

static void test_container(void)
{
    q2_stx_writer w;
    u32 i, frames = 8;
    u32 audio_slots = 0, video_slots = 0;
    size_t cursor = 0;
    static q2_stx_frame f;
    u32 read_back = 0;
    static s16 pcm[1512 * 2];

    printf("\ncontainer: %u frames with sound\n", frames);

    g_cap.flat = (u8 *)calloc(MAX_SECTORS, Q2_STX_SECTOR_SIZE);
    if (!g_cap.flat) {
        printf("FAIL: out of memory\n");
        g_fail++;
        return;
    }
    g_cap.count = 0;

    /* A quiet tone, so the audio slots carry something the ADPCM has to work
     * at rather than a run of zeros that any encoder gets right. */
    for (i = 0; i < 1512; i++) {
        double s = sin((double)i * 0.05) * 8000.0;

        pcm[i * 2 + 0] = (s16)s;
        pcm[i * 2 + 1] = (s16)(-s);
    }

    q2_stx_writer_init(&w, Q2_STX_WIDTH, Q2_STX_HEIGHT, true, capture, NULL);
    for (i = 0; i < frames; i++) {
        make_frame(g_src, Q2_STX_WIDTH, Q2_STX_HEIGHT, i);
        CHECK(q2_stx_writer_frame(&w, g_src, pcm, 1512),
              "frame %u would not go into the container", i + 1);
    }
    CHECK(q2_stx_writer_finish(&w), "the container would not close");

    /* Slot 7 of every 8 is audio, in 4,055 of 4,055 sectors on the disc. */
    for (i = 0; i < g_cap.count; i++) {
        bool want_audio = (i % 8u) == 7u;

        if (g_cap.form[i] == Q2_STX_FORM_AUDIO) {
            audio_slots++;
            CHECK(want_audio, "sector %u is audio and is not at slot 7", i);
            CHECK(g_cap.len[i] == CD_SECTOR_FORM2,
                  "audio sector %u is %u bytes, not %u", i, g_cap.len[i],
                  CD_SECTOR_FORM2);
        } else {
            if (g_cap.form[i] == Q2_STX_FORM_VIDEO)
                video_slots++;
            CHECK(!want_audio, "slot 7 sector %u is not audio", i);
            CHECK(g_cap.len[i] == Q2_STX_SECTOR_SIZE,
                  "video sector %u is %u bytes, not %u", i, g_cap.len[i],
                  Q2_STX_SECTOR_SIZE);
        }
    }
    printf("  %u sectors: %u video, %u audio\n", g_cap.count, video_slots,
           audio_slots);
    CHECK(w.frames == frames, "%u frames written, wanted %u", w.frames, frames);

    /* And the cadence, read back off the sectors the way the demuxer reads
     * them: 6, 5, 5, 5 keyed to the frame number. */
    while (q2_stx_frame_next(g_cap.flat,
                             (size_t)g_cap.count * Q2_STX_SECTOR_SIZE,
                             &cursor, &f)) {
        u32 want = (((f.number - 1u) % 4u) == 0u) ? 6u : 5u;
        u32 got  = (f.size + Q2_STX_VIDEO_PAYLOAD - 1u) / Q2_STX_VIDEO_PAYLOAD;
        u32 blocks = 0, bits = 0;

        read_back++;
        CHECK(f.number == read_back, "frame %u arrived as number %u",
              read_back, f.number);
        CHECK(got <= want, "frame %u wants %u sectors and its budget is %u",
              f.number, got, want);
        CHECK(q2_stx_frame_decode(&f, g_back, &blocks, &bits),
              "frame %u would not decode out of the container", f.number);
    }
    CHECK(read_back == frames, "%u frames came back out of %u", read_back,
          frames);

    free(g_cap.flat);
    g_cap.flat = NULL;
}

static void test_adpcm(void)
{
    q2_xa_encoder enc;
    q2_xa_decoder dec;
    static s16 in[XA_FRAMES_PER_SECTOR * 2];
    static s16 out[XA_FRAMES_PER_SECTOR * 2];
    u8  adpcm[XA_SECTOR_ADPCM_BYTES];
    double sig = 0.0, err = 0.0;
    u32 i, n;

    printf("\nADPCM: one sector of a signal that is not already ADPCM\n");

    /* Two tones, one per channel, at a level real audio sits at. */
    for (i = 0; i < XA_FRAMES_PER_SECTOR; i++) {
        in[i * 2 + 0] = (s16)(sin((double)i * 0.017) * 11000.0);
        in[i * 2 + 1] = (s16)(sin((double)i * 0.041) * 9000.0 +
                              sin((double)i * 0.003) * 4000.0);
    }

    q2_xa_encoder_reset(&enc);
    q2_xa_encode_sector(&enc, in, XA_FRAMES_PER_SECTOR, adpcm);

    CHECK(q2_xa_validate_sector(adpcm) == 0,
          "the encoder wrote %u structurally invalid blocks",
          q2_xa_validate_sector(adpcm));

    q2_xa_decoder_reset(&dec);
    n = q2_xa_decode_sector(&dec, adpcm, out, XA_FRAMES_PER_SECTOR * 2);
    CHECK(n == XA_FRAMES_PER_SECTOR * 2, "%u samples back, wanted %u", n,
          XA_FRAMES_PER_SECTOR * 2);

    for (i = 0; i < XA_FRAMES_PER_SECTOR * 2; i++) {
        double a = (double)in[i], b = (double)out[i];

        sig += a * a;
        err += (a - b) * (a - b);
    }
    {
        double snr = (err > 0.0) ? 10.0 * log10(sig / err) : 99.0;

        printf("  SNR %.2f dB\n", snr);
        /*
         * 4-bit ADPCM on a clean tone should manage the high twenties. Below
         * twenty means the predictor is being fed the wrong history — the
         * classic ADPCM mistake, where the encoder remembers what it MEANT to
         * write rather than what it wrote, and the error compounds.
         */
        CHECK(snr > 20.0, "one sector of tone came back at only %.2f dB", snr);
    }

    /*
     * And the drift check: the same signal over eight sectors, encoded as one
     * stream. A predictor that is fed correctly holds its figure; one that is
     * not gets worse every sector, which a single-sector test cannot see.
     */
    {
        double first = 0.0, last = 0.0;
        u32 s;

        q2_xa_encoder_reset(&enc);
        q2_xa_decoder_reset(&dec);
        for (s = 0; s < 8; s++) {
            double ss = 0.0, ee = 0.0;

            for (i = 0; i < XA_FRAMES_PER_SECTOR; i++) {
                double ph = (double)(s * XA_FRAMES_PER_SECTOR + i);

                in[i * 2 + 0] = (s16)(sin(ph * 0.017) * 11000.0);
                in[i * 2 + 1] = (s16)(sin(ph * 0.041) * 9000.0);
            }
            q2_xa_encode_sector(&enc, in, XA_FRAMES_PER_SECTOR, adpcm);
            q2_xa_decode_sector(&dec, adpcm, out, XA_FRAMES_PER_SECTOR * 2);

            for (i = 0; i < XA_FRAMES_PER_SECTOR * 2; i++) {
                double a = (double)in[i], b = (double)out[i];

                ss += a * a;
                ee += (a - b) * (a - b);
            }
            if (s == 0) first = 10.0 * log10(ss / (ee > 0 ? ee : 1e-9));
            if (s == 7) last  = 10.0 * log10(ss / (ee > 0 ? ee : 1e-9));
        }
        printf("  eight sectors as one stream: %.2f dB -> %.2f dB\n",
               first, last);
        CHECK(last > first - 3.0,
              "the stream lost %.2f dB over eight sectors, which is drift",
              first - last);
    }
}

static void test_sector(void)
{
    u8 raw[CD_SECTOR_RAW];
    u8 form1[CD_SECTOR_FORM1];
    u8 form2[CD_SECTOR_FORM2];
    u32 i;

    printf("\nCD-XA sector construction\n");

    for (i = 0; i < sizeof(form1); i++)
        form1[i] = (u8)(i * 31u + 7u);
    for (i = 0; i < sizeof(form2); i++)
        form2[i] = (u8)(i * 17u + 3u);

    cd_sector_build(raw, 150000u, 1, 1, (u8)CD_SUBMODE_DATA, 0, form1, false);
    CHECK(raw[0] == 0x00 && raw[1] == 0xFF && raw[11] == 0x00,
          "the sync pattern is wrong");
    CHECK(raw[15] == 2, "the mode byte is %u, not 2", raw[15]);
    CHECK(memcmp(raw + 16, raw + 20, 4) == 0,
          "the subheader and its copy disagree");
    CHECK(memcmp(raw + 24, form1, sizeof(form1)) == 0,
          "the Form 1 payload did not survive");
    CHECK(cd_sector_check(raw) == 0,
          "a freshly built Form 1 sector fails its own EDC or parity (%u)",
          cd_sector_check(raw));

    cd_sector_build(raw, 150001u, 1, 1,
                    (u8)(CD_SUBMODE_AUDIO | CD_SUBMODE_REALTIME), 0x01,
                    form2, true);
    CHECK((raw[18] & CD_SUBMODE_FORM2) != 0,
          "a Form 2 sector did not get the Form 2 bit");
    CHECK(memcmp(raw + 24, form2, sizeof(form2)) == 0,
          "the Form 2 payload did not survive");
    CHECK(cd_sector_check(raw) == 0,
          "a freshly built Form 2 sector fails its own EDC (%u)",
          cd_sector_check(raw));

    /* A flipped bit anywhere in the user data has to be caught, or the EDC is
     * not being computed over the user data at all. */
    raw[100] ^= 0x01;
    CHECK(cd_sector_check(raw) != 0,
          "a corrupted sector still passes its EDC");
}

int main(void)
{
    test_frame_roundtrip();
    test_container();
    test_adpcm();
    test_sector();

    if (g_fail)
        printf("\n%d check(s) failed\n", g_fail);
    else
        printf("\nall encoder checks passed\n");
    return g_fail ? 1 : 0;
}
