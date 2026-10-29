#include "xa.h"
#include <stdio.h>
#include <string.h>
#include "fixtures/xa_resample_reference.h"

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

static u64 pcm_hash(const s16 *pcm, u32 samples)
{
    u64 hash = 14695981039346656037ULL;
    u32 i;
    for (i = 0; i < samples; i++) {
        u16 value = (u16)pcm[i];
        hash = (hash ^ (value & 255)) * 1099511628211ULL;
        hash = (hash ^ (value >> 8)) * 1099511628211ULL;
    }
    return hash;
}

int main(void)
{
    q2_xa_resampler r = {0}, unchanged;
    s16 input[XA_FRAMES_PER_SECTOR * XA_CHANNELS] = {0};
    s16 output[XA_OUTPUT_FRAMES_PER_SECTOR * XA_CHANNELS];
    u32 kind, sector, j;
    CHECK(XA_OUTPUT_RATE == 44100 && XA_SAMPLE_RATE == 37800);
    CHECK(XA_FRAMES_PER_SECTOR == 2016 && XA_OUTPUT_FRAMES_PER_SECTOR == 2352);
    for (kind = 0; kind < 6; kind++) {
        u32 rng = 0x12345678u + kind;
        q2_xa_resampler_reset(&r);
        for (sector = 0; sector < 3; sector++) {
            for (j = 0; j < XA_FRAMES_PER_SECTOR; j++) {
                u32 i = sector * XA_FRAMES_PER_SECTOR + j;
                s32 left = 0, right = 0;
                switch (kind) {
                case 0: left = i == 0 ? 32767 : 0; right = i == 12 ? -32768 : 0; break;
                case 1: left = 1000; right = -1000; break;
                case 2: left = 32767; right = -32768; break;
                case 3: left = i & 1 ? 32767 : -32768; right = -left - 1; break;
                case 4:
                    rng = rng * 1664525u + 1013904223u; left = (s16)(rng >> 16);
                    rng = rng * 1664525u + 1013904223u; right = (s16)(rng >> 16); break;
                case 5: left = i % 37 < 18 ? 32767 : -32768; right = i % 31 < 15 ? -32768 : 32767; break;
                }
                input[j * 2] = (s16)left; input[j * 2 + 1] = (s16)right;
            }
            CHECK(q2_xa_resample_sector(&r, input, output, XA_OUTPUT_FRAMES_PER_SECTOR) == 2352);
            CHECK(pcm_hash(output, Q2PSX_ARRAY_COUNT(output)) == xa_reference_hash[kind][sector]);
            CHECK(r.phase == 0 && r.position == 0);
            if (kind == 0 && sector == 0) {
                /* Startup is causal; a left impulse cannot appear on the right. */
                CHECK(output[0] == 0 && output[1] == 0);
                for (j = 0; j < 12; j++) CHECK(output[j * 2 + 1] == 0);
            }
        }
    }
    unchanged = r;
    memset(output, 0x5a, sizeof(output));
    CHECK(q2_xa_resample_sector(&r, input, output, 2351) == 0);
    CHECK(!memcmp(&r, &unchanged, sizeof(r)));
    { bool intact = true;
      for (j = 0; j < Q2PSX_ARRAY_COUNT(output); j++) if (output[j] != 0x5a5a) intact = false;
      CHECK(intact);
    }
    CHECK(q2_xa_resample_sector(NULL, input, output, 2352) == 0);
    CHECK(q2_xa_resample_sector(&r, NULL, output, 2352) == 0);
    CHECK(q2_xa_resample_sector(&r, input, NULL, 2352) == 0);
    q2_xa_resampler_reset(&r);
    memset(input, 0, sizeof(input));
    CHECK(q2_xa_resample_sector(&r, input, output, 2352) == 2352);
    { bool silent = true;
      for (j = 0; j < Q2PSX_ARRAY_COUNT(output); j++) if (output[j]) silent = false;
      CHECK(silent);
    }
    printf("XA interpolation: %d checks, %d failures (software reference)\n", checks, failures);
    return failures != 0;
}
