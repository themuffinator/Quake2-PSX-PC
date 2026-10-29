#ifndef Q2PSX_AUDIO_SPU_H
#define Q2PSX_AUDIO_SPU_H

#include "vag.h"

typedef enum q2_spu_phase {
    Q2_SPU_OFF, Q2_SPU_ATTACK, Q2_SPU_DECAY, Q2_SPU_SUSTAIN, Q2_SPU_RELEASE
} q2_spu_phase;

typedef struct q2_spu_envelope {
    u16 adsr1, adsr2;
    s32 level;
    u32 counter;
    q2_spu_phase phase;
} q2_spu_envelope;

/* One step is one 44100 Hz SPU clock, independent of voice pitch. */
void q2_spu_envelope_step(q2_spu_envelope *env);
s16 q2_spu_gaussian(const s16 history[4], u32 fraction);

typedef struct q2_spu_player {
    q2_spu_voice dec;
    q2_spu_envelope env;
    s16 block[SPU_SAMPLES_PER_BLOCK], history[4];
    u32 have, index, counter;
    u16 pitch;
    bool active, primed;

    /* Rational conversion from the native clock to the host mixer clock.
     * This output conversion is separate from the SPU's Gaussian filter. */
    bool output_ready;
    s16 previous, next;
    u64 output_phase;
} q2_spu_player;

/* Borrowed sample storage must outlive playback, including a key-off tail. */
void q2_spu_player_start(q2_spu_player *p, const u8 *data, u32 size,
                         u16 pitch, u16 adsr1, u16 adsr2);
void q2_spu_player_key_off(q2_spu_player *p);
/* Native pre-volume voice output, also the signal written to SPU capture RAM. */
s16 q2_spu_player_next(q2_spu_player *p);
/* Convert native output for a fixed, nonzero host rate. */
s16 q2_spu_player_sample(q2_spu_player *p, u32 output_rate);
bool q2_spu_player_alive(const q2_spu_player *p);

#endif
