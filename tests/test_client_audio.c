/* Exercise the production mixer with synthetic samples: no device or input. */
int q2_test_audio_client_main(int argc, char **argv);
#define main q2_test_audio_client_main
#include "../src/client/main.c"
#undef main

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)
#include "client_fixture.h"
#include "fixtures/audio_register_reference.h"
#include "fixtures/music_playlist_reference.h"

static void test_loop_export(void)
{
    u8 data[32] = {0};
    s16 pcm[112] = {0};
    q2_spu_voice voice;
    data[0] = data[16] = 12;
    data[1] = SPU_FLAG_LOOP_START | SPU_FLAG_REPEAT;
    data[17] = SPU_FLAG_END | SPU_FLAG_REPEAT;
    memset(data + 2, 0x11, 14);
    memset(data + 18, 0x22, 14);
    CHECK(q2_spu_adpcm_decode(data, sizeof(data), pcm, 112) == 56);
    CHECK(pcm[0] == 1 && pcm[27] == 1 && pcm[28] == 2 && pcm[55] == 2);
    q2_spu_voice_start(&voice, data, sizeof(data));
    CHECK(q2_spu_voice_block(&voice, pcm) == 28);
    CHECK(q2_spu_voice_block(&voice, pcm) == 28 && voice.looped);
    CHECK(q2_spu_voice_block(&voice, pcm) == 28 && pcm[0] == 1);
}

/* Choose a constant native DSP state that emits the requested PCM value.
 * Only gain/summation is under test here; the DSP has independent hardware
 * fixtures. No production output converter is bypassed or preloaded. */
static void output_sample(client_voice *v, s16 sample)
{
    s32 lo = -32768, hi = 32767;
    q2_spu_player probe;
    memset(&v->spu, 0, sizeof(v->spu));
    v->spu.active = v->spu.primed = true;
    v->spu.env.phase = Q2_SPU_SUSTAIN;
    v->spu.env.level = 0x7fff;
    v->spu.env.adsr2 = 0x1fc0; /* frozen, pitch zero retains the chosen history */
    while (lo < hi) {
        s32 mid = lo + (hi - lo) / 2;
        int i;
        for (i = 0; i < 4; i++) v->spu.history[i] = (s16)mid;
        probe = v->spu;
        if (q2_spu_player_next(&probe) < sample) lo = mid + 1;
        else hi = mid;
    }
    { int i; for (i = 0; i < 4; i++) v->spu.history[i] = (s16)lo; }
    /* Independent tap truncations can skip one output value with a uniform
     * history. Perturb the two leading taps to represent that PCM exactly. */
    { int a, b;
      for (a = -2; a <= 2; a++) for (b = -2; b <= 2; b++) {
        if (lo + a < -32768 || lo + a > 32767 || lo + b < -32768 || lo + b > 32767) continue;
        v->spu.history[0] = (s16)(lo + a); v->spu.history[1] = (s16)(lo + b);
        probe = v->spu;
        if (q2_spu_player_next(&probe) == sample) return;
      }
    }
    CHECK(false);
}

static void test_native_voice_clock(void)
{
    client_voice v = {0};
    q2_spu_player native;
    u8 data[32] = {0};
    s32 pcm[38] = {0};
    u32 chunk, i;
    data[1] = SPU_FLAG_LOOP_START;
    data[17] = SPU_FLAG_END | SPU_FLAG_REPEAT;
    for (i = 2; i < 16; i++) { data[i] = (u8)(i * 23); data[i + 16] = (u8)(i * 47); }
    q2_spu_player_start(&v.spu, data, sizeof(data), 0x1379, 0, 0);
    native = v.spu;
    v.active = true; v.level = 128; v.vol = 64; v.mono = true;
    for (chunk = 0; chunk < 17; chunk++) {
        memset(pcm, 0, sizeof(pcm));
        client_voice_mix(&v, pcm, 19);
        for (i = 0; i < 19; i++) {
            s32 expected = q2_spu_player_next(&native) >> 2;
            CHECK(pcm[i * 2] == expected && pcm[i * 2 + 1] == expected);
        }
        CHECK(v.spu.counter == native.counter && v.spu.index == native.index);
        CHECK(v.spu.env.level == native.env.level && !v.spu.output_ready);
    }
}

static void test_live_volume(void)
{
    client *c = calloc(1, sizeof(*c));
    CHECK(c != NULL);
    if (!c) return;
    c->voice[0].active = true;
    c->voice[0].vol = 127;
    c->voice[0].req_vol = 63;
    c->settings.v[Q2_SET_SFX] = 0;
    client_voices_update(c);
    CHECK(c->voice[0].vol == 0);
    c->settings.v[Q2_SET_SFX] = 77;
    client_voices_update(c);
    CHECK(c->voice[0].vol == 77);
    free(c);
}

static void test_silent_playlist_entry(void)
{
    client *c = calloc(1, sizeof(*c));
    CHECK(c != NULL);
    if (!c) return;
    c->music_table_ready = true;
    c->build.video = Q2_VIDEO_PAL;
    c->music_table.count = 1;
    c->music_table.entry[0].file = -1;
    c->music_table.entry[0].tenths = 25;
    c->bed_frames = 16;
    c->bed_pos = 4;
    c->bed_resampler.history[0][0] = 12345;
    c->bed_resampler.phase = 5;
    CHECK(client_music_play_id(c, 0));
    CHECK(!c->music_open);
    CHECK(c->music_total == 125 && c->music_left == 125);
    CHECK(c->bed_frames == 0 && c->bed_pos == 0);
    { q2_xa_resampler cleared = {0}; CHECK(!memcmp(&c->bed_resampler, &cleared, sizeof(cleared))); }
    free(c);
}

static client *tone_client(void)
{
    client *c = calloc(1, sizeof(*c));
    u8 *data = calloc(80, 1);
    if (!c || !data) exit(2);
    c->sfx.offsets = calloc(1, sizeof(u32));
    if (!c->sfx.offsets) exit(2);
    c->headless = c->sfx_ready = true;
    c->sfx.buf.data = data;
    c->sfx.buf.size = c->sfx.section_size = 80;
    c->sfx.section = data;
    c->sfx.count = 1;
    memcpy(data, "VAGp", 4);
    data[15] = 32;
    data[18] = 0x56; data[19] = 0x22; /* 22050 Hz, big endian */
    memcpy(data + 32, "tone", 5);
    memset(data + 50, 0x11, 14);
    memset(data + 66, 0x22, 14);
    data[65] = SPU_FLAG_END;
    q2_menu_settings_defaults(&c->settings);
    c->build.video = Q2_VIDEO_PAL;
    return c;
}

static void tone_free(client *c)
{
    client_voices_stop(c);
    q2_sound_bank_free(&c->sfx);
    free(c);
}

static void test_voice_lifetime(void)
{
    client *c = tone_client();
    client_voice *v;
    s32 pcm[80] = {0};
    u32 i, serial;

    c->voice_serial_next = 0xFFFFFFFFu;
    CHECK(client_play_sound(c, "tone"));
    v = c->voice_last;
    CHECK(v && v->serial == 1);
    if (!v) { tone_free(c); return; }
    serial = v->serial;
    v->vol = 127; v->level = v->pan_l = v->pan_r = 255;
    v->spu.pitch = 0x5000; /* hardware caps this to 0x4000 */
    client_voice_mix(v, pcm, 40);
    CHECK(pcm[0] == 0); /* envelope starts at zero */
    CHECK(pcm[8] != 0 && pcm[16] != 0); /* crosses the first block */
    CHECK(!client_voice_playing(v, serial));
    CHECK(pcm[30] == 0); /* capped pitch still finishes within 15 host frames */

    CHECK(client_play_sound(c, "tone"));
    CHECK(c->voice_last == v && !client_voice_playing(v, serial));
    client_voice_stop(v, serial);
    CHECK(v->active); /* A stale owner cannot stop a reused slot. */
    CHECK(!client_play_sound(c, "absent"));
    CHECK(!c->voice_last && !c->voice_last_serial);
    CHECK(v->req_vol == 63);

    client_voices_stop(c);
    CHECK(client_play_sound(c, "tone"));
    for (i = 0; i < 30; i++) client_audio_pump(c, 1.0 / 30.0);
    CHECK(c->audio_frames == CLIENT_AUDIO_RATE);
    CHECK(!c->voice[0].active);
    for (i = 0; i < CLIENT_VOICES; i++) CHECK(client_play_sound(c, "tone"));
    CHECK(!client_play_sound(c, "tone") && c->voice_dropped == 1);
    CHECK(!c->voice_last);
    tone_free(c);
}

static void test_final_sample(void)
{
    client_voice v = {0};
    u8 data[16] = {0};
    s32 pcm[60] = {0};
    data[1] = SPU_FLAG_END;
    memset(data + 2, 0x33, 14);
    q2_spu_player_start(&v.spu, data, sizeof(data), 0x1000, 0, 0);
    v.active = true; v.vol = 127; v.level = v.pan_l = v.pan_r = 255;
    client_voice_mix(&v, pcm, 30);
    CHECK(pcm[54] != 0); /* all 28 native sample intervals survive */
    CHECK(pcm[56] == 0 && pcm[58] == 0 && !v.active);
}

static void test_keyoff_tail(void)
{
    client *c = tone_client();
    client_voice *v;
    s16 pcm[64];
    u32 i, serial;
    c->sfx.buf.data[49] = SPU_FLAG_LOOP_START;
    c->sfx.buf.data[65] = SPU_FLAG_END | SPU_FLAG_REPEAT;
    CHECK(client_play_sound(c, "tone"));
    v = c->voice_last;
    if (!v) { tone_free(c); return; }
    serial = v->serial;
    client_voices_update(c);
    client_audio_mix(c, pcm, 32);
    CHECK(v->active && v->spu.env.level == 0x7fff);
    client_voice_stop(v, serial);
    CHECK(!client_voice_playing(v, serial));
    CHECK(q2_spu_player_alive(&v->spu) && v->spu.env.phase == Q2_SPU_RELEASE);
    /* The mixer visits a released logical slot for its two native release clocks. */
    client_audio_mix(c, pcm, 8);
    for (i = 0; i < 2; i++) CHECK(pcm[i * 2] != 0 && pcm[i * 2 + 1] != 0);
    for (i = 4; i < 16; i++) CHECK(pcm[i] == 0);
    CHECK(!q2_spu_player_alive(&v->spu));

    CHECK(client_play_sound(c, "tone"));
    serial = v->serial;
    client_voices_update(c);
    client_audio_mix(c, pcm, 32);
    client_voice_stop(v, serial);
    CHECK(q2_spu_player_alive(&v->spu));
    CHECK(client_play_sound(c, "tone"));
    CHECK(c->voice_last == v && v->serial != serial);
    CHECK(v->spu.env.level == 0 && v->spu.env.phase == Q2_SPU_ATTACK);
    CHECK(!v->spu.output_ready && !v->spu.previous && !v->spu.next);
    client_voice_stop(v, serial);
    CHECK(v->active && v->spu.env.phase == Q2_SPU_ATTACK);

    client_voices_update(c);
    client_audio_mix(c, pcm, 32);
    CHECK(q2_spu_player_alive(&v->spu));
    client_voices_stop(c);
    CHECK(!q2_spu_player_alive(&v->spu) && !v->spu.dec.adpcm);
    CHECK(!c->voice_last && !c->voice_last_serial);
    q2_sound_bank_free(&c->sfx);
    c->sfx_ready = false;
    client_audio_mix(c, pcm, 32); /* no stale sample access after bank teardown */
    for (i = 0; i < Q2PSX_ARRAY_COUNT(pcm); i++) CHECK(pcm[i] == 0);
    free(c);
}

static void test_mix_cancellation(void)
{
    client *c = tone_client();
    s16 pcm[2], swapped[2];
    int i;
    for (i = 0; i < 3; i++) {
        client_voice *v = &c->voice[i];
        v->active = true;
        output_sample(v, i < 2 ? 30000 : -30000);
        v->vol = 127; v->level = v->pan_l = v->pan_r = 255;
    }
    client_audio_mix(c, pcm, 1);
    output_sample(&c->voice[0], -30000);
    output_sample(&c->voice[1], 30000);
    output_sample(&c->voice[2], 30000);
    client_audio_mix(c, swapped, 1);
    CHECK(pcm[0] > 29000);
    CHECK(pcm[0] == swapped[0] && pcm[1] == swapped[1]);
    for (i = 0; i < 3; i++) output_sample(&c->voice[i], 30000);
    client_audio_mix(c, pcm, 1);
    CHECK(pcm[0] == 32639 && pcm[1] == 32639); /* main gain follows saturation */
    tone_free(c);
}

static void test_cached_bed(void)
{
    client *c = tone_client();
    s16 pcm[2];
    c->bed_frames = 4;
    c->bed[0] = c->bed[2] = c->bed[4] = c->bed[6] = 1000;
    c->settings.v[Q2_SET_STEREO] = 1;
    c->music_level = 255;
    client_bed_read(c, pcm, 1);
    CHECK(pcm[0] == 1992 && pcm[1] == 0);
    c->music_level = 0;
    client_bed_read(c, pcm, 1);
    CHECK(pcm[0] == 0 && pcm[1] == 0);
    c->music_level = 96;
    c->settings.v[Q2_SET_STEREO] = 0;
    client_bed_read(c, pcm, 1);
    CHECK(pcm[0] == 375 && pcm[1] == 375);
    CHECK(c->bed[4] == 1000 && c->bed[5] == 0);
    tone_free(c);
}

static void test_mix_gain_path(void)
{
    client *c = tone_client();
    client_voice *v = &c->voice[0];
    s16 pcm[2];
    u32 i;
    CHECK(audio_reference_main[0] == 0x3fc0 && audio_reference_cd_input[0] == 0x7fff);
    c->settings.v[Q2_SET_STEREO] = 1;
    c->music_level = 128;
    c->bed_frames = 1;
    c->bed[0] = 1000; c->bed[1] = -1000;
    client_audio_mix(c, pcm, 1);
    CHECK(pcm[0] == 995 && pcm[1] == -997); /* CD input gain, then main gain */

    v->level = 128; v->vol = 64; v->mono = true; /* voice register 4096: quarter gain */
    v->active = true; output_sample(v, 4000);
    client_audio_mix(c, pcm, 1);
    CHECK(pcm[0] == 996 && pcm[1] == 996); /* SFX bypass the CD input gain */

    client_voices_stop(c);
    c->music_level = 255; c->bed_frames = 1; c->bed_pos = 0;
    c->bed[0] = 20000; c->bed[1] = -20000;
    client_audio_mix(c, pcm, 1);
    CHECK(pcm[0] == 32638 && pcm[1] == -32640);

    c->bed_frames = 1; c->bed_pos = 0;
    c->bed[0] = c->bed[1] = 20000;
    v->active = true; output_sample(v, -20000);
    client_audio_mix(c, pcm, 1);
    CHECK(pcm[0] == 27657 && pcm[1] == 27657); /* CD clips before SFX cancellation */

    client_voices_stop(c);
    for (i = 0; i < 5; i++) {
        v = &c->voice[i];
        v->level = 128; v->vol = 64; v->mono = true;
        v->active = true; output_sample(v, 30000);
    }
    client_audio_mix(c, pcm, 1);
    CHECK(pcm[0] == 32639 && pcm[1] == 32639); /* sum clips before main volume */
    for (i = 0; i < 5; i++) output_sample(&c->voice[i], -30000);
    client_audio_mix(c, pcm, 1);
    CHECK(pcm[0] == -32640 && pcm[1] == -32640);
    tone_free(c);
}

static void test_playlist_clock(void)
{
    client *c = tone_client();
    q2_level_entry level = {0};
    c->level = &level;
    level.playlist[0] = 1; level.playlist[1] = 2;
    c->music_cursor_at = 0;
    c->music_table_ready = true;
    c->music_table.count = 3;
    c->music_table.entry[1].file = -1;
    c->music_table.entry[2].file = -1;
    c->music_table.entry[1].tenths = 1;
    c->music_table.entry[2].tenths = 2;
    CHECK(client_music_play_id(c, 1) && c->music_playing);
    client_music_tick(c, 0.1);
    CHECK(c->music_id == 2 && c->music_left == 10);
    client_music_tick(c, 0.2);
    CHECK(!c->music_playing && !c->music_open); /* native zero terminator */
    level.playlist[2] = 254; /* An explicit -2 branch is what loops. */
    c->music_cursor_at = 0;
    CHECK(client_music_play_id(c, 1));
    client_music_tick(c, 0.1);
    CHECK(c->music_id == 2 && c->music_left == 10);
    client_music_tick(c, 0.2);
    CHECK(c->music_id == 1 && c->music_left == 5);
    c->build.video = Q2_VIDEO_NTSC;
    CHECK(client_music_play_id(c, 1) && c->music_total == 6);
    c->music_table.entry[1].tenths = 0;
    CHECK(client_music_play_id(c, 1) && c->music_total == 0);
    client_music_tick(c, 1.0 / 60.0);
    CHECK(c->music_id == 2 && c->music_total == 12);
    /* A valid XA entry whose file cannot open retries after thirty seconds. */
    c->music_table.entry[1].file = 0;
    CHECK(client_music_play_id(c, 1));
    CHECK(c->music_playing && !c->music_open && c->music_total == 1800);
    CHECK(!client_music_play_id(c, 99) && !c->music_playing);
    tone_free(c);
}

/* Selection/timer expectations come from executed PAL/US instructions.
 * The fixture includes silent lists and the one-, four- and seven-track loops. */
static void test_disc_music(const char *path)
{
    client *c = open_client(path);
    unsigned i, j;
    c->music_table_ready = q2_music_table_load(&c->music_table, c->disc, &c->build) == Q2_OK;
    c->level_table_ready = q2_level_table_load(&c->level_table, c->disc, &c->build) == Q2_OK;
    CHECK(c->music_table_ready && c->level_table_ready);
    CHECK(c->music_table.count == Q2PSX_ARRAY_COUNT(music_entry_reference));
    CHECK(c->level_table.count == Q2PSX_ARRAY_COUNT(music_playlist_reference) + 1);
    if (!c->music_table_ready || !c->level_table_ready ||
        c->level_table.count < Q2PSX_ARRAY_COUNT(music_playlist_reference)) {
        close_client(c); return;
    }
    for (i = 0; i < Q2PSX_ARRAY_COUNT(music_entry_reference); ++i) {
        const struct music_entry_reference *v = &music_entry_reference[i];
        const q2_music_entry *entry = q2_music_get(&c->music_table, (int)i);
        int fields = (int)(c->build.video == Q2_VIDEO_NTSC ? v->ntsc_fields : v->pal_fields);
        CHECK(entry != NULL);
        if (!entry) continue;
        CHECK(entry->file == v->file && entry->channel == v->channel && entry->tenths == v->tenths);
        CHECK(client_music_play_id(c, (int)i));
        CHECK(c->music_playing && c->music_total == fields && c->music_left == fields);
        CHECK(c->music_open == (v->file >= 0));
        if (c->music_open) CHECK(c->music.channel == v->channel);
    }
    for (i = 0; i < Q2PSX_ARRAY_COUNT(music_playlist_reference); ++i) {
        const struct music_playlist_reference *v = &music_playlist_reference[i];
        CHECK(!strcmp(c->level_table.entries[i].directory, v->directory));
        snprintf(c->map, sizeof(c->map), "%s", v->directory);
        c->bed_frames = 16; c->bed_pos = 3;
        client_music_for_level(c, true, false);
        for (j = 0; j < v->count; ++j) {
            if (j) { c->music_left = 0; client_music_tick(c, 0.0); }
            if (v->ids[j] < 0) {
                CHECK(!c->music_playing && !c->music_open);
                CHECK(c->bed_frames == 0 && c->bed_pos == 0);
            } else {
                const struct music_entry_reference *e = &music_entry_reference[v->ids[j]];
                int fields = (int)(c->build.video == Q2_VIDEO_NTSC ? e->ntsc_fields : e->pal_fields);
                CHECK(c->music_playing && c->music_open);
                CHECK(c->music_id == v->ids[j] && c->music_cursor_at == v->cursors[j]);
                CHECK(c->music_total == fields && c->music_left == fields);
                CHECK(c->music.channel == e->channel && c->music_cursor == 0);
                CHECK(c->bed_frames == 0 && c->bed_pos == 0);
            }
        }
    }
    for (i = 0; i < Q2PSX_ARRAY_COUNT(music_policy_reference); ++i) {
        const struct music_policy_reference *v = &music_policy_reference[i];
        q2_level_entry level = {0};
        for (j = 0; j < Q2PSX_ARRAY_COUNT(v->input); ++j) level.playlist[j] = (u8)v->input[j];
        c->level = &level;
        c->music_cursor_at = -1;
        client_music_advance(c);
        for (j = 0; j < v->count; ++j) {
            if (j) { c->music_left = 0; client_music_tick(c, 0.0); }
            if (v->ids[j] < 0) CHECK(!c->music_playing && !c->music_open);
            else CHECK(c->music_playing && c->music_open && c->music_id == v->ids[j]);
        }
        c->level = NULL;
    }
    close_client(c);
}

static void test_request_pitch(void)
{
    static const struct { const char *name; u32 low, width; } request[] = {
        { "pla_gasp1", 30, 9 }, { "pla_drown1", 30, 9 },
        { "pla_burn1", 33, 2 }, { "pla_burn2", 33, 2 }
    };
    client *c;
    u32 i, seed = q2_random_state();
    for (i = 0; i < sizeof(request) / sizeof(request[0]); i++) {
        u32 value, after;
        c = tone_client();
        memset(c->sfx.buf.data + 32, 0, 16);
        memcpy(c->sfx.buf.data + 32, request[i].name, strlen(request[i].name));
        q2_random_seed(12345);
        value = (u32)q2_random_next();
        after = q2_random_state();
        q2_random_seed(12345);
        CHECK(client_play_player_sound(c, request[i].name, NULL, 0));
        CHECK(q2_random_state() == after);
        CHECK(c->voice_last->spu.pitch == q2_sfx_spu_pitch(22050,
                (u8)(request[i].low + value % request[i].width)));
        tone_free(c);
    }
    c = tone_client();
    q2_random_seed(12345);
    CHECK(!client_play_player_sound(c, "absent", NULL, 0));
    CHECK(q2_random_state() == 12345);
    CHECK(client_play_player_sound(c, "tone", NULL, 0));
    CHECK(q2_random_state() == 12345);
    q2_random_seed(seed);
    tone_free(c);
}

static void test_bank_bounds(void)
{
    client *c = tone_client();
    q2_vag vag;
    CHECK(q2_sound_bank_get(&c->sfx, 0, &vag));
    c->sfx.offsets[0] = 0xFFFFFFF0u;
    CHECK(!q2_sound_bank_get(&c->sfx, 0, &vag));
    c->sfx.offsets[0] = 0;
    memset(c->sfx.buf.data + 12, 0xFF, 4);
    CHECK(!q2_sound_bank_get(&c->sfx, 0, &vag));
    tone_free(c);
}

static void test_retail_requests(void)
{
    client *c = tone_client();
    q2_sfx_request request, clone;
    u32 pitch, volume, after, i, seed = q2_random_state();
    q2_sfx_request_init(&request);
    q2_sfx_pitch_set_range(&request.pitch, 8, 0);
    request.vary_volume = true; request.volume_lower = 20; request.volume_upper = 30;
    clone = request;
    q2_sfx_request_params(&clone, 50, 23, -1);
    CHECK(request.pitch.current == 35 && request.volume == 63);
    q2_random_seed(12345);
    pitch = 35 + (u32)q2_random_next() % 8;
    volume = 20 + (u32)q2_random_next() % 10;
    after = q2_random_state(); q2_random_seed(12345);
    CHECK(client_play_player_sound(c, "tone", &request, 0));
    CHECK(q2_random_state() == after && request.pitch.current == pitch && request.volume == volume);
    CHECK(c->voice_last->spu.pitch == q2_sfx_spu_pitch(22050, (u8)pitch));
    CHECK(client_play_request(c, "tone", &request, CLIENT_SOUND_POINT, 0, c->cam.pos));
    CHECK(q2_random_state() == after && c->voice_last->req_vol == volume);
    CHECK(client_play_request(c, "tone", &clone, CLIENT_SOUND_POINT, 0, c->cam.pos));
    CHECK(q2_random_state() == after && clone.pitch.current == 50 && c->voice_last->req_vol == 23);
    CHECK(!client_play_player_sound(c, "absent", &request, 0) && q2_random_state() == after);
    for (i = 0; i < CLIENT_VOICES; i++) c->voice[i].active = true;
    CHECK(!client_play_player_sound(c, "tone", &request, 0) && q2_random_state() == after);
    client_sound_requests_init(c);
    CHECK(c->chaingun_request.pitch.current == 35 && c->chaingun_request.volume == 23);
    CHECK(c->chaingun_request.pitch.lower == 51 && c->chaingun_request.pitch.upper == 59);
    q2_random_seed(seed); tone_free(c);
}

static void test_spatial_windows(void)
{
    q2_sfx_listener l[4] = {0};
    q2_sfx_spatial s;
    s32 at[3] = {4096, 0, 0};
    u32 i;
    for (i = 0; i < 4; i++) q2_rotation_view(l[i].matrix, 0, 0, 0);
    CHECK(q2_sfx_distance_root(16777215) == 4087); /* SDK table approximation */
    CHECK(q2_sfx_distance_root(0) == 0 && q2_sfx_distance_root(65535) == 255);
    s = q2_sfx_spatialize(at, 63, l, 1);
    CHECK(s.distance == 4096 && s.level == 126 && s.pan == 30);
    at[0] += 65536;
    s = q2_sfx_spatialize(at, 63, l, 1);
    CHECK(s.distance == 4096 && s.pan == 30); /* signed-short subtraction */
    at[0] = 0; at[1] = 4096;
    s = q2_sfx_spatialize(at, 63, l, 1);
    CHECK(s.distance == 1 && s.level == 188);
    q2_rotation_view(l[0].matrix, 0, 1024, 0);
    s = q2_sfx_spatialize(at, 63, l, 1);
    CHECK(s.distance == 4096 && s.level == 126);
    at[0] = 4096; at[1] = 0;
    q2_rotation_view(l[0].matrix, 0, 0, 1024);
    s = q2_sfx_spatialize(at, 63, l, 1);
    CHECK(s.distance == 1 && s.pan == 15);
    q2_rotation_view(l[0].matrix, 0, 0, 0);
    for (i = 0; i < 4; i++) l[i].pos[0] = -(s32)i * 4096;
    s = q2_sfx_spatialize(at, 63, l, 2);
    CHECK(s.distance == 6144 && s.level == 94 && s.projected[0] == 8192 && s.pan == 30);
    s = q2_sfx_spatialize(at, 63, l, 3);
    CHECK(s.distance == 8192 && s.level == 63 && s.projected[0] == 12288);
    s = q2_sfx_spatialize(at, 63, l, 4);
    CHECK(s.distance == 10240 && s.level == 31 && s.projected[0] == 16384);
}

static void test_bank_requests(void)
{
    client *c = tone_client();
    const char *name = q2_weapon_tables_builtin()->sound[Q2_WSND_MACHINEGUN];
    q2_sfx_request *request;
    q2_monster m;
    u32 pitch, after, seed = q2_random_state();
    memset(c->sfx.buf.data + 32, 0, 16);
    memcpy(c->sfx.buf.data + 32, name, strlen(name));
    client_sound_requests_init(c);
    request = client_request_for_sound(name, c);
    CHECK(request && request->pitch.varying && request->pitch.lower == 35);
    CHECK(client_request_for_sound(Q2_CRE_SND_BULLET, c) == request);
    q2_monster_init(&m); m.in_use = true;
    c->creatures.set.monsters = &m; c->creatures.set.count = 1;
    q2_random_seed(12345);
    pitch = 35 + (u32)q2_random_next() % 8;
    after = q2_random_state(); q2_random_seed(12345);
    CHECK(client_play_player_sound(c, name, NULL, 0));
    CHECK(request && request->pitch.current == pitch && q2_random_state() == after);
    pitch = 35 + (u32)q2_random_next() % 8;
    after = q2_random_state(); q2_random_seed(12345); (void)q2_random_next();
    client_cre_named_sound(&m, Q2_CRE_SND_BULLET, c);
    CHECK(request && request->pitch.current == pitch && q2_random_state() == after);
    CHECK(c->voice_last && c->voice_last->source == CLIENT_SOUND_CREATURE);
    CHECK(client_play_sound_at(c, name, c->cam.pos));
    CHECK(c->voice_last->spu.pitch == q2_sfx_spu_pitch(22050, (u8)pitch));
    CHECK(q2_random_state() == after); /* direct playback reuses the last entity pitch */
    CHECK(c->chaingun_request.pitch.current == 35 && c->chaingun_request.volume == 23);
    c->creatures.set.monsters = NULL; c->creatures.set.count = 0;
    q2_random_seed(seed); tone_free(c);
}

static void test_creature_projectile_reports(void)
{
    static const struct { u32 slot; const char *name; } shots[] = {
        { Q2_IMP_FIRE_BLASTER, "wep_blastf1a" },
        { Q2_IMP_FIRE_GRENADE, "wep_grenlf1a" },
        { Q2_IMP_FIRE_ROCKET, "wep_rocklf1a" }
    };
    u32 i, seed = q2_random_state();
    for (i = 0; i < Q2PSX_ARRAY_COUNT(shots); i++) {
        client *c = tone_client();
        q2_monster m;
        q2_cre_shot shot = {0};
        q2_monster_init(&m); m.in_use = true;
        c->creatures_ready = true;
        c->creatures.set.monsters = &m; c->creatures.set.count = 1;
        memset(c->sfx.buf.data + 32, 0, 16);
        memcpy(c->sfx.buf.data + 32, shots[i].name, strlen(shots[i].name));
        shot.slot = shots[i].slot; shot.aimed = true; shot.damage = 1;
        shot.dir[2] = 4096; shot.speed = 600;
        client_cre_shot(&m, &shot, c);
        CHECK(c->voice_last && c->voice_last->source == CLIENT_SOUND_CREATURE);
        m.pos[0] = 1024; client_voices_update(c);
        CHECK(c->voice_last && c->voice_last->pos_world[0] == 1024);
        m.in_use = false; client_voices_update(c);
        CHECK(c->voice_last && !c->voice_last->active);
        c->creatures.set.monsters = NULL; c->creatures.set.count = 0;
        tone_free(c);
    }
    q2_random_seed(seed);
}

static void test_register_gain(void)
{
    client_voice v = {0};
    s32 pcm[2] = {0};
    CHECK(q2_sfx_volume_register(127, 0, 127, true) == 8064);
    CHECK(q2_sfx_volume_register(84, 255, 127, true) == 5334);
    CHECK(q2_sfx_volume_register(255, 255, 127, false) == 16129);
    CHECK(q2_sfx_volume_register(188, 179, 127, false) == 8347);
    CHECK(q2_sfx_volume_register(188, 179, 0, false) == 0);
    CHECK(q2_sfx_volume_register(255, 0, 127, false) == 0);
    output_sample(&v, 16384);
    v.vol = 127; v.level = 127; v.mono = true;
    client_voice_mix(&v, pcm, 1);
    CHECK(pcm[0] == 8064 && pcm[1] == 8064);
    output_sample(&v, 16384);
    v.level = 84; pcm[0] = pcm[1] = 0;
    client_voice_mix(&v, pcm, 1);
    CHECK(pcm[0] == 5334 && pcm[1] == 5334);
}

static void test_entity_owners(void)
{
    client *c = tone_client();
    q2_monster *m = calloc(1, sizeof(*m));
    client_voice *v;
    u32 old;
    if (!m) exit(2);
    c->creatures.set.monsters = m; c->creatures.set.count = 1;
    q2_monster_init(m); m->in_use = true; m->pos[0] = 4096;
    CHECK(client_play_creature_sound(c, "tone", NULL, m)); v = c->voice_last;
    client_voices_update(c);
    CHECK(v->active && v->source == CLIENT_SOUND_CREATURE && v->level == 126);
    m->pos[0] = 8192; client_voices_update(c);
    CHECK(v->pos_world[0] == 8192 && v->level == 63);
    client_cre_named_sound(m, "tone", c);
    CHECK(c->voice_last && c->voice_last->source == CLIENT_SOUND_CREATURE);
    m->dead = true; client_voices_update(c);
    CHECK(v->active); /* A corpse is still the same entity; don't cut its death sound. */
    old = m->audio_generation; q2_monster_init(m); m->in_use = true;
    CHECK(m->audio_generation != old);
    client_voices_update(c); CHECK(!v->active);
    m->sound_duration = 4;
    CHECK(client_play_creature_sound(c, "tone", NULL, m)); v = c->voice_last;
    c->sim[0].level_time = 3; client_voices_update(c); CHECK(v->active);
    c->sim[0].level_time = 4; client_voices_update(c); CHECK(!v->active);
    CHECK(client_play_creature_sound(c, "tone", NULL, m)); v = c->voice_last;
    m->in_use = false; client_voices_update(c); CHECK(!v->active);
    /* Reallocating a roster does not invalidate index+incarnation ownership. */
    m->in_use = true;
    CHECK(client_play_creature_sound(c, "tone", NULL, m)); v = c->voice_last;
    c->creatures.set.monsters = malloc(sizeof(*m));
    if (!c->creatures.set.monsters) exit(2);
    *c->creatures.set.monsters = *m; free(m);
    client_voices_update(c); CHECK(v->active);
    free(c->creatures.set.monsters); c->creatures.set.monsters = NULL; c->creatures.set.count = 0;
    client_voices_update(c); CHECK(!v->active);
    tone_free(c);
}

static void test_multiplayer_sources(void)
{
    client *c = tone_client();
    client_voice *v;
    int players, pi;
    CHECK(client_play_player_sound(c, "tone", NULL, 0));
    client_voices_update(c);
    CHECK(c->voice_last->level == 127 && c->voice_last->flat_mono);
    client_voices_stop(c); c->mp_enabled = true;
    for (players = 2; players <= 4; players++) {
        c->screen.view_count = players;
        for (pi = 1; pi < players; pi++) {
            c->sim_ready[pi] = true;
            c->sim[0].player[pi].pos[0] = pi * 4096;
        }
        CHECK(client_play_player_sound(c, "tone", NULL, players - 1));
        v = c->voice_last; client_voices_update(c);
        CHECK(v->active && v->level == 84 && v->flat_mono && v->pan_l == v->pan_r);
        c->sim[0].player[players - 1].pos[0] += 500;
        client_voices_update(c);
        CHECK(v->pos_world[0] == (players - 1) * 4096 + 500);
        CHECK(client_play_sound_at(c, "tone", (s32[3]){0,0,4096}));
        client_voices_update(c);
        CHECK(c->voice_last->positional && !c->voice_last->flat_mono);
        c->player_audio_generation[players - 1]++;
        client_voices_update(c); CHECK(!v->active);
        client_voices_stop(c);
    }
    c->menu.open = true;
    CHECK(client_play_player_sound(c, "tone", NULL, 1)); client_voices_update(c);
    CHECK(c->voice_last->level == 254 && !c->voice_last->flat_mono);
    c->settings.v[Q2_SET_STEREO] = 0; client_voices_update(c);
    CHECK(!c->voice_last->mono && c->voice_last->pan_l == k_pan[15]);
    CHECK(!client_play_player_sound(c, "tone", NULL, Q2_MP_MAX_PLAYERS));
    client_voices_stop(c); c->menu.open = false;
    c->settings.v[Q2_SET_STEREO] = 1;
    memset(c->sim[0].player, 0, sizeof(c->sim[0].player));
    for (players = 2; players <= 4; players++) {
        c->screen.view_count = players;
        for (pi = 1; pi < players; pi++) c->sim[0].player[pi].pos[0] = -pi * 4096;
        CHECK(client_play_sound_at(c, "tone", (s32[3]){4096,0,0}));
        v = c->voice_last; client_voices_update(c);
        CHECK(v->level == (players == 2 ? 94 : players == 3 ? 63 : 31));
        CHECK(v->pan_l == 0 && v->pan_r == 255);
        /* Fixed-point voices retain their snapshot after the caller moves. */
        CHECK(v->pos_world[0] == 4096);
        client_voices_stop(c);
    }
    tone_free(c);
}

static void test_body_sources(void)
{
    client *c = tone_client();
    const char *name = client_ent_sound_name(c, Q2_SND_FOOTSTEP_WET);
    u32 seed = q2_random_state();
    CHECK(name != NULL);
    memset(c->sfx.buf.data + 32, 0, 16);
    if (name) memcpy(c->sfx.buf.data + 32, name, strlen(name));
    q2_random_seed(12345);
    q2_ent_sound_player(&c->sim[0].ent_world.events, 2, Q2_SND_FOOTSTEP_WET,
                         (s32[3]){100,200,300});
    CHECK(c->sim[0].ent_world.events.e[0].sound_player == 2);
    client_entity_events(c);
    CHECK(c->voice_last && c->voice_last->source == CLIENT_SOUND_POINT);
    CHECK(q2_random_state() == 12345);
    CHECK(c->voice_last && c->voice_last->spu.pitch == q2_sfx_spu_pitch(22050,35));
    {
        q2_entity *e = q2_entity_alloc(&c->sim[0].entities);
        client_voice *v;
        u32 incarnation;
        CHECK(e != NULL);
        if (!e) exit(2);
        name = client_ent_sound_name(c, Q2_SND_TELEPORT);
        memset(c->sfx.buf.data + 32, 0, 16);
        memcpy(c->sfx.buf.data + 32, name, strlen(name));
        e->origin[0] = 4096; incarnation = e->audio_generation;
        q2_ent_sound_entity(&c->sim[0].ent_world.events, Q2_SND_TELEPORT, e);
        client_entity_events(c); v = c->voice_last;
        CHECK(v && v->source == CLIENT_SOUND_ENTITY && v->owner_generation == incarnation);
        e->origin[0] = 8192; client_voices_update(c);
        CHECK(v && v->active && v->pos_world[0] == 8192);
        q2_entity_remove(e); e = q2_entity_alloc(&c->sim[0].entities);
        CHECK(e && e->audio_generation != incarnation);
        client_voices_update(c); CHECK(v && !v->active);
        q2_entity_set_free(&c->sim[0].entities);
    }
    q2_random_seed(seed); tone_free(c);
}

static u32 burn_voices(const client *c, const q2_vag *burn)
{
    u32 i, n = 0;
    for (i = 0; i < CLIENT_VOICES; i++)
        if (c->voice[i].active && c->voice[i].spu.dec.adpcm == burn->body) n++;
    return n;
}

static void test_burn_sound_resolution(void)
{
    client *c = tone_client();
    u8 *data = realloc(c->sfx.buf.data, 160);
    u32 *offsets = realloc(c->sfx.offsets, 2 * sizeof(*offsets));
    u32 state, seed = q2_random_state();
    if (!data || !offsets) exit(2);
    c->sfx.buf.data = data;
    c->sfx.section = data;
    c->sfx.buf.size = c->sfx.section_size = 160;
    c->sfx.offsets = offsets;
    offsets[1] = 80;
    c->sfx.count = 2;
    memcpy(data + 80, data, 80);
    for (state = 0; state < 4; state++) {
        const char *expected = state & 1 ? "pla_burn1" : "pla_burn2";
        u32 before = c->voice_started;
        client_voices_stop(c);
        memset(data + 32, 0, 16); memset(data + 112, 0, 16);
        memcpy(data + 32, state & 1 ? "pla_burn1" : "other1", state & 1 ? 9 : 6);
        memcpy(data + 112, state & 2 ? "pla_burn2" : "other2", state & 2 ? 9 : 6);
        c->requests_ready = false;
        CHECK(!strcmp(client_ent_sound_name(c, Q2_SND_BURN), expected));
        q2_ent_sound_player(&c->sim[0].ent_world.events, 0, Q2_SND_BURN, (s32[3]){0,0,0});
        client_entity_events(c);
        CHECK(c->voice_started == before + (state != 0));
        if (state) CHECK(c->voice_last->spu.dec.adpcm == data + (state & 1 ? 48 : 128));
        else CHECK(q2_random_state() == seed);
        CHECK(c->sim[0].ent_world.events.count == 0);
    }
    q2_random_seed(seed);
    tone_free(c);
}

static void write_pcm_wav(const char *path, const s16 *pcm, u32 frames)
{
    u8 header[44] = {0};
    FILE *f = fopen(path, "wb");
    u32 i, bytes = frames * XA_CHANNELS * 2;
    memcpy(header, "RIFF", 4); q2_wr_u32(header + 4, bytes + 36);
    memcpy(header + 8, "WAVEfmt ", 8); q2_wr_u32(header + 16, 16);
    q2_wr_u16(header + 20, 1); q2_wr_u16(header + 22, XA_CHANNELS);
    q2_wr_u32(header + 24, CLIENT_AUDIO_RATE); q2_wr_u32(header + 28, CLIENT_AUDIO_RATE * XA_CHANNELS * 2);
    q2_wr_u16(header + 32, XA_CHANNELS * 2); q2_wr_u16(header + 34, 16);
    memcpy(header + 36, "data", 4); q2_wr_u32(header + 40, bytes);
    CHECK(f != NULL);
    if (!f) return;
    CHECK(fwrite(header, 1, sizeof(header), f) == sizeof(header));
    for (i = 0; i < frames * XA_CHANNELS; i++) {
        u8 sample[2];
        q2_wr_u16(sample, (u16)pcm[i]);
        if (fwrite(sample, 1, 2, f) != 2) { CHECK(false); break; }
    }
    CHECK(fclose(f) == 0);
}

static void test_disc_gain_audio(const char *path, const char *wav_path)
{
    client *c = open_client(path);
    const u32 frames = CLIENT_AUDIO_RATE * 4, step = CLIENT_AUDIO_RATE / 30;
    s16 *pcm = calloc((size_t)frames * XA_CHANNELS, sizeof(*pcm));
    u32 i, nonzero = 0, seed = q2_random_state();
    if (!pcm) exit(2);
    CHECK(q2_music_table_load(&c->music_table, c->disc, &c->build) == Q2_OK);
    c->music_table_ready = true;
    CHECK(q2_level_table_load(&c->level_table, c->disc, &c->build) == Q2_OK);
    c->level_table_ready = true;
    CHECK(client_load_zone(c, "BASE1", 0));
    CHECK(c->music_open && c->music_total > Q2_MUSIC_FADE_TICKS);
    c->music_level = c->music_target = c->settings.v[Q2_SET_MUSIC] = 96;
    c->settings.v[Q2_SET_STEREO] = 1;
    q2_random_seed(12345);
    for (i = 0; i < 120; i++) {
        if (i == 60) CHECK(client_play_player_sound(c,
            q2_weapon_tables_builtin()->sound[Q2_WSND_MACHINEGUN], NULL, 0));
        client_music_tick(c, 1.0 / 30.0);
        client_voices_update(c);
        client_audio_mix(c, pcm + (size_t)i * step * XA_CHANNELS, step);
    }
    for (i = 0; i < frames * XA_CHANNELS; i++) nonzero += pcm[i] != 0;
    CHECK(nonzero > frames / 2);
    CHECK(c->audio_frames == frames && c->voice_dropped == 0);
    if (wav_path) write_pcm_wav(wav_path, pcm, frames);
    printf("disc gain capture: %u frames, %u nonzero samples\n", frames, nonzero);
    free(pcm);
    close_client(c);
    q2_random_seed(seed);
}

static void test_disc_burn_audio(const char *path, const char *wav_path)
{
    client *c = open_client(path);
    const u32 step = 101 * (CLIENT_AUDIO_RATE / 300), frames = 5 * step + CLIENT_AUDIO_RATE;
    s16 *pcm = calloc((size_t)frames * XA_CHANNELS, sizeof(*pcm));
    q2_vag burn = {0};
    u32 i, before, seed = q2_random_state();
    CHECK(pcm != NULL);
    if (!pcm) exit(2);
    CHECK(client_load_zone(c, "POWER1", 1));
    CHECK(!client_find_sound(c, "pla_burn1", &burn));
    CHECK(client_find_sound(c, "pla_burn2", &burn));
    client_voices_stop(c);
    c->music_open = c->music_playing = false;
    c->bed_frames = c->bed_pos = 0;
    q2_random_seed(12345);
    before = c->voice_started;
    c->sim[0].combat.inv.health = 1000;
    for (i = 0; i < 5; i++) {
        q2_damage_result hit;
        u32 starts = c->voice_started, j, nonzero = 0;
        c->sim[0].level_time = 1000 + (s32)i * 101;
        hit = q2_sim_hurt_player(&c->sim[0], NULL, 1, Q2_MOD_LAVA, NULL);
        CHECK(!hit.blocked && hit.taken == 1);
        client_entity_events(c);
        CHECK(c->voice_started == starts + ((i & 3) == 0 ? 1u : 0u));
        if ((i & 3) == 0) {
            CHECK(c->voice_last && c->voice_last->spu.dec.adpcm == burn.body);
            CHECK(c->voice_last && c->voice_last->source == CLIENT_SOUND_PLAYER &&
                  c->voice_last->owner_index == 0);
            CHECK(c->voice_last &&
                (c->voice_last->spu.pitch == q2_sfx_spu_pitch(burn.sample_rate, 33) ||
                 c->voice_last->spu.pitch == q2_sfx_spu_pitch(burn.sample_rate, 34)));
        }
        client_voices_update(c);
        client_audio_mix(c, pcm + (size_t)i * step * XA_CHANNELS, step);
        for (j = i * step * XA_CHANNELS; j < (i + 1) * step * XA_CHANNELS; j++)
            if (pcm[j]) nonzero++;
        if ((i & 3) == 0) CHECK(nonzero > 0);
    }
    client_audio_mix(c, pcm + (size_t)5 * step * XA_CHANNELS, CLIENT_AUDIO_RATE);
    CHECK(c->voice_started == before + 2 && c->sim[0].lava_burn_calls == 5);
    if (wav_path) write_pcm_wav(wav_path, pcm, frames);
    free(pcm);
    /* This is executable-lifetime state, not saved-game state. */
    CHECK(client_capture(c));
    c->sim[0].lava_burn_calls = 7;
    CHECK(client_apply_save(c, &c->snapshot));
    CHECK(c->sim[0].lava_burn_calls == 7);
    CHECK(!client_load_zone(c, "POWER1", 99));
    CHECK(c->sim[0].lava_burn_calls == 7);
    CHECK(client_load_zone(c, "POWER1", 2));
    CHECK(c->sim[0].lava_burn_calls == 7);
    CHECK(client_load_zone(c, "BASE1", 0));
    CHECK(c->sim[0].lava_burn_calls == 7);
    close_client(c);

    /* Actual late native callback: its request must reach the mixer in this
     * frame, before the following simulation tick clears its event queue. */
    c = open_client(path);
    CHECK(client_load_zone(c, "POWER1", 1));
    CHECK(!c->sim[0].lava_burn_calls);
    CHECK(client_find_sound(c, "pla_burn2", &burn));
    {
        const s32 feet[3] = {-600,4600 + Q2_EYE_BASE,-13120};
        q2_sim_spawn(&c->sim[0], feet, 0);
        c->sim[0].gravity = 0;
        c->sim[0].combat.inv.health = 1000;
        c->sim[0].combat.self.env_next = 0;
        c->sim[0].dt_accum = 0;
        c->sim[0].dt_frac = 0;
        c->sim[0].level_time = 1000;
        c->native_mission.power1_blast_phase = c->native_mission.power1_next_blast = 0;
        client_voices_stop(c);
        for (i = 0; i < 2; i++) {
            c->frame_index++;
            client_input_simulated(c, 1.0f / 30.0f);
        }
        CHECK(c->native_mission.power1_blast_phase == 1);
        CHECK(c->sim[0].lava_burn_calls == 1);
        CHECK(burn_voices(c, &burn) == 1);
        CHECK(c->sim[0].ent_world.events.count == 0);
    }
    close_client(c);
    q2_random_seed(seed);
}

static void test_disc_audio(const char *path)
{
    client *c = open_client(path);
    u32 banks = 0, sounds = 0, loops = 0;
    u32 burn_first = 0, burn_fallback = 0, burn_missing = 0;
    int file;
    s16 pcm[CLIENT_AUDIO_CHUNK_FRAMES * XA_CHANNELS];
    CHECK(q2_music_table_load(&c->music_table, c->disc, &c->build) == Q2_OK);
    c->music_table_ready = true;
    CHECK(q2_level_table_load(&c->level_table, c->disc, &c->build) == Q2_OK);
    c->level_table_ready = true;
    for (file = 0; file < disc_file_count(c->disc); file++) {
        const disc_file *f = disc_file_at(c->disc, file);
        const char *file_path = f->path + (f->path[0] == '/');
        const char *end = strstr(file_path, "/SNDVRAM.DAT");
        char map[64];
        u32 i;
        if (!end || strncmp(file_path, "Q2DATA/LEVELS/", 14)) continue;
        if ((size_t)(end - (file_path + 14)) >= sizeof(map)) continue;
        memcpy(map, file_path + 14, (size_t)(end - (file_path + 14)));
        map[end - (file_path + 14)] = '\0';
        CHECK(q2_sound_bank_load(&c->sfx, c->disc, map) == Q2_OK);
        c->sfx_ready = c->sfx.buf.data != NULL;
        c->requests_ready = false;
        CHECK(c->sfx.count <= Q2PSX_ARRAY_COUNT(c->bank_request));
        banks++;
        for (i = 0; i < c->sfx.count; i++) {
            q2_vag vag;
            u32 block, expected = 0, capacity;
            s16 *decoded;
            CHECK(q2_sound_bank_get(&c->sfx, i, &vag));
            capacity = vag.data_size / SPU_BLOCK_SIZE * SPU_SAMPLES_PER_BLOCK;
            decoded = calloc(capacity, sizeof(s16));
            if (!decoded) exit(2);
            for (block = 0; block + SPU_BLOCK_SIZE <= vag.data_size; block += SPU_BLOCK_SIZE) {
                expected += SPU_SAMPLES_PER_BLOCK;
                if (vag.body[block + 1] & SPU_FLAG_END) break;
            }
            CHECK(q2_spu_adpcm_decode(vag.body, vag.data_size, decoded, capacity) == expected);
            free(decoded);
            CHECK(client_play_sound(c, vag.name));
            if (c->voice_last) {
                /* All real banks cross decode boundaries at the highest request byte. */
                c->voice_last->spu.pitch = (u16)q2_sfx_spu_pitch(vag.sample_rate, 255);
                client_voices_update(c);
                client_audio_mix(c, pcm, CLIENT_AUDIO_CHUNK_FRAMES);
                CHECK(c->voice_last->spu.have <= SPU_SAMPLES_PER_BLOCK);
            }
            sounds++;
            if (vag.looping) loops++;
            client_voices_stop(c);
        }
        {
            q2_vag burn;
            bool first = client_find_sound(c, "pla_burn1", &burn);
            bool present = first || client_find_sound(c, "pla_burn2", &burn);
            u32 before = c->voice_started;
            if (first) burn_first++;
            else if (present) burn_fallback++;
            else burn_missing++;
            q2_ent_sound_player(&c->sim[0].ent_world.events, 0, Q2_SND_BURN, (s32[3]){0,0,0});
            client_entity_events(c);
            CHECK(c->voice_started == before + present);
            if (present) CHECK(c->voice_last && c->voice_last->spu.dec.adpcm == burn.body);
            client_voices_stop(c);
        }
        q2_sound_bank_free(&c->sfx); c->sfx_ready = false;
    }
    CHECK(banks == 49 && sounds == 2475 && loops == 118);
    printf("disc burn bindings: %u burn1, %u burn2 fallback, %u absent\n",
           burn_first, burn_fallback, burn_missing);
    CHECK(client_load_zone(c, "BASE1", 0));
    {
        int first = c->music_id;
        client_music_advance(c);
        CHECK(c->music_cursor_at != 0);
        CHECK(client_load_zone(c, "BASE1", 0)); /* restart, same level record */
        CHECK(c->music_id == first && c->music_cursor_at == 0 && c->music_cursor == 0);
        CHECK(client_capture(c));
        client_music_advance(c);
        CHECK(client_apply_save(c, &c->snapshot)); /* same-level save restore */
        CHECK(c->music_id == first && c->music_cursor_at == 0 && c->music_cursor == 0);
        CHECK(c->music_left == c->music_total && c->bed_frames == 0);
    }
    {
        u32 i, j;
        for (i = 0; i < c->creatures.mod_count; i++) {
            q2_creature_module *mod = &c->creatures.mod[i];
            q2_monster m = {0};
            m.cre_bind = &mod->bind;
            CHECK(mod->sounds_ready); /* initialize at load, before any sounds */
            for (j = 0; j < mod->sound_count; j++) {
                const q2_cre_sound_bind *b = &mod->sound_bind[j];
                q2_sfx_request *shared = client_request_for_sound(b->name, c);
                q2_sfx_request *actual = q2_creature_world_sound_request(&c->creatures,
                    &m, b->addr, NULL, client_cre_bank_has, c, client_request_for_sound, NULL);
                CHECK(actual && shared);
                CHECK(b->cloned ? actual != shared : actual == shared);
            }
        }
    }
    {
        u32 i, checked = 0;
        for (i = 0; i < c->creatures.set.count; i++) {
            q2_monster *m = &c->creatures.set.monsters[i];
            const char *registered, *name;
            q2_sfx_request *request;
            client_voice *v;
            s32 old[3];
            if (!m->in_use) continue;
            registered = q2_creature_world_sound_name(&c->creatures, m, 0);
            if (!registered) continue;
            request = q2_creature_world_sound_request(&c->creatures, m, 0,
                registered, client_cre_bank_has, c, client_request_for_sound, &name);
            if (!request || !name) continue;
            CHECK(client_play_creature_sound(c, name, request, m));
            v = c->voice_last;
            CHECK(v && v->source == CLIENT_SOUND_CREATURE);
            memcpy(old, q2_cre_origin(m), sizeof(old));
            if (m->motion.ready) m->motion.ent.pos[0] += 100;
            else m->pos[0] += 100;
            client_voices_update(c);
            CHECK(v && v->active && v->pos_world[0] == old[0] + 100);
            if (m->motion.ready) memcpy(m->motion.ent.pos, old, sizeof(old));
            else memcpy(m->pos, old, sizeof(old));
            client_voices_stop(c);
            client_cre_sound_owner(m, 0, m, c);
            CHECK(c->voice_last && c->voice_last->source == CLIENT_SOUND_CREATURE);
            client_voices_stop(c);
            client_cre_sound_owner(m, 0, &c->creatures.sight, c);
            client_voices_update(c);
            CHECK(c->voice_last && c->voice_last->source == CLIENT_SOUND_PLAYER &&
                  c->voice_last->level == 127);
            client_voices_stop(c); checked++;
        }
        CHECK(checked > 0);
    }
    {
        const u8 *bank = c->sfx.section;
        q2_vag vag;
        client_voice *v;
        u32 serial;
        CHECK(q2_sound_bank_get(&c->sfx, 0, &vag));
        CHECK(client_play_sound(c, vag.name));
        v = c->voice_last; serial = c->voice_last_serial;
        CHECK(!client_load_zone(c, "BASE1", 99));
        CHECK(client_voice_playing(v, serial)); /* Failed preflight preserves sound. */
        c->carry_player = c->carry_same_map = true;
        CHECK(client_load_zone(c, "BASE1", 1));
        CHECK(c->sfx.section == bank); /* Retained bank, but not retained voices. */
        CHECK(!client_voice_playing(v, serial));
        CHECK(c->music_left == 0);
        client_music_tick(c, 1.0 / 30.0);
        CHECK(c->music_open && c->music_left > 0);
    }
    {
        int id = c->music_id;
        c->music_cursor = c->music.sector_count;
        c->bed_frames = c->bed_pos = 0;
        CHECK(!client_bed_refill(c));
        CHECK(c->music_id == id); /* EOF does not outrun the retail countdown. */
        CHECK(client_film_start(c, c->build.video == Q2_VIDEO_PAL ? "TAKE1BP.STX" : "TAKE1B.STX"));
        CHECK(!c->music_playing && c->film_open);
        client_audio_mix(c, pcm, CLIENT_AUDIO_CHUNK_FRAMES);
        CHECK(c->film.audio_cursor > 0);
        client_film_stop(c);
        CHECK(c->music_playing && c->music_open && !c->film_open);
    }
    printf("disc audio: %u banks, %u samples, %u loops; zone and film transitions passed\n",
            banks, sounds, loops);
    close_client(c);
}

static void test_pause_audio(void)
{
    client *c = tone_client();
    client_voice *motor;
    q2_rotator_set rotators = {0};
    q2_rotator *hatch = q2_rotators_add(&rotators, Q2_ROT_TARGET, 0, 1, 64);
    u32 serial, started, ticks;
    if (!hatch) exit(2);
    hatch->target = 1024;
    q2_rotator_trigger(&rotators, 0);
    q2_rotators_tick(&rotators, 8); /* idle -> delay */
    q2_rotators_tick(&rotators, 8); /* delay -> opening */
    CHECK(hatch->state == Q2_ROTST_OPENING);
    CHECK(q2_rotator_take_loop(&rotators, 0) == Q2_ROTLOOP_START);
    q2_menu_init(&c->menu, &c->settings, Q2_MENU_SCREEN_H);
    q2_prompt_init(&c->prompts);
    c->sfx.buf.data[65] = SPU_FLAG_END | SPU_FLAG_REPEAT;
    CHECK(client_play_sound_at(c, "tone", c->cam.pos));
    motor = c->voice_last; serial = c->voice_last_serial; started = c->voice_started;
    hatch->loop_voice = motor; hatch->loop_serial = serial;
    c->music_playing = true; c->music_left = c->music_total = 500;
    c->music_level = 96;
    CHECK(client_pause_open(c, 0));
    CHECK(!client_voice_playing(motor, serial) && client_music_target(c) == 0);
    client_music_tick(c, 1.0);
    CHECK(c->music_left == 450); /* CD time continues while the world is paused. */
    c->music_target = client_music_target(c); client_music_ramp(c);
    CHECK(c->music_level == 92);
    client_menu_close(c);
    CHECK(client_music_target(c) == c->settings.v[Q2_SET_MUSIC] * 2);
    CHECK(!motor->active && c->voice_started == started); /* no invented loop resurrection */
    q2_rotators_tick(&rotators, 8);
    CHECK(hatch->angle > 0 && hatch->state == Q2_ROTST_OPENING);
    CHECK(q2_rotator_take_loop(&rotators, 0) == Q2_ROTLOOP_NONE);
    CHECK(client_play_sound(c, "tone"));
    for (ticks = 0; ticks < 100 && hatch->state == Q2_ROTST_OPENING; ticks++)
        q2_rotators_tick(&rotators, 8);
    CHECK(hatch->state == Q2_ROTST_OPEN);
    CHECK(q2_rotator_take_loop(&rotators, 0) == Q2_ROTLOOP_STOP);
    /* The old hatch's arrival cannot stop the voice that reused its slot. */
    client_voice_stop((client_voice *)hatch->loop_voice, hatch->loop_serial);
    CHECK(c->voice_last->active);
    c->save_prompt_open = true;
    CHECK(!client_pause_open(c, 0) && !c->menu.open);
    CHECK(client_music_target(c) == 0 && c->voice_last->active);
    c->save_prompt_open = false;
    c->menu.open = true; c->menu.page_id = Q2_PAGE_DEATH;
    CHECK(!client_music_paused(c));
    q2_rotators_free(&rotators);
    tone_free(c);
}

static void test_film_audio_policy(void)
{
    client *c = tone_client();
    s16 pcm[2];
    c->film_open = true; c->film.fps = 25;
    c->film_stop_at = ~(u64)0;
    CHECK(client_film_volume(c, 0) == 15);
    CHECK(client_film_volume(c, 1764) == 31);
    CHECK(client_film_volume(c, 15 * 1764) == 255);
    c->film_audio_mixed = 16 * 1764;
    c->bed_frames = 2; c->bed[0] = c->bed[2] = 1000;
    c->music_level = c->settings.v[Q2_SET_MUSIC] = 0;
    c->settings.v[Q2_SET_STEREO] = 1;
    client_bed_read(c, pcm, 1);
    CHECK(pcm[0] == 1992 && pcm[1] == 0); /* retail FMV ignores MUSIC mute */
    c->settings.v[Q2_SET_STEREO] = 0;
    client_bed_read(c, pcm, 1);
    CHECK(pcm[0] == 992 && pcm[1] == 992);
    c->film_stop_at = 40000;
    CHECK(client_film_volume(c, 40000) == 255);
    CHECK(client_film_volume(c, 40882) == 191);
    CHECK(client_film_volume(c, 41764) == 127);
    CHECK(client_film_volume(c, 42646) == 63);
    CHECK(client_film_volume(c, 43528) == 0);
    CHECK(client_film_audio_end(c) == 43528);
    c->build.video = Q2_VIDEO_NTSC;
    CHECK(client_film_volume(c, 40735) == 191);
    CHECK(client_film_audio_end(c) == 42940);
    c->film_open = false; tone_free(c);
}

static void test_film_queue_clock(void)
{
    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, CLIENT_AUDIO_RATE };
    client *c = tone_client();
    s16 pcm[2400 * 2] = {0};
    c->audio = SDL_CreateAudioStream(&spec, &spec); /* unbound; no device or user input */
    CHECK(c->audio != NULL);
    if (!c->audio) { tone_free(c); return; }
    c->film_open = c->film_stopping = true;
    c->film_stop_at = ~(u64)0;
    c->film_audio_submitted = 2400;
    CHECK(SDL_PutAudioStreamData(c->audio, pcm, sizeof(pcm)));
    client_film_tick(c, 10.0f);
    CHECK(c->film_audio_played == 0); /* queued-ahead audio has not played */
    CHECK(SDL_GetAudioStreamData(c->audio, pcm, 1200 * 4) == 1200 * 4);
    client_film_tick(c, 0.0f);
    CHECK(c->film_audio_played == 1200);
    CHECK(SDL_GetAudioStreamData(c->audio, pcm, 1200 * 4) == 1200 * 4);
    client_film_tick(c, 0.0f);
    CHECK(c->film_audio_played == 2400);
    client_film_tick(c, 10.0f);
    CHECK(c->film_audio_played == 2400); /* underrun holds the picture */
    SDL_DestroyAudioStream(c->audio); c->audio = NULL;
    c->film_open = false; tone_free(c);
}

static void test_film_sync(const char *path)
{
    client *c = open_client(path);
    q2_movie *reference = calloc(1, sizeof(*reference));
    u8 *rgb = malloc(Q2_STX_WIDTH * Q2_STX_HEIGHT * 3);
    unsigned i;
    if (!reference || !rgb) exit(2);
    CHECK(client_film_start(c, "TAKE1BP.STX"));
    CHECK(q2_movie_open(reference, c->disc, "Q2DATA/MOVIES/TAKE1BP.STX"));
    client_film_tick(c, 10.0f);
    CHECK(c->film.frame.number == 1 && c->film.frames_shown == 1);
    client_audio_pump(c, 1.0);
    client_film_tick(c, 0.1f); /* render dt was clamped; audio advanced a whole second */
    CHECK(c->film.frame.number == 26 && c->film.frames_shown == 2 && c->film.frames_skipped == 24);
    for (i = 0; i <= 25; i++) CHECK(q2_movie_advance(reference, i ? 0.04 : 0.0, rgb));
    CHECK(!memcmp(c->film_rgb, rgb, Q2_STX_WIDTH * Q2_STX_HEIGHT * 3));
    client_film_tick(c, 10.0f);
    CHECK(c->film.frame.number == 26 && c->film.clock == 1.0);
    client_audio_pump(c, 0.4); client_film_tick(c, 0.1f);
    CHECK(c->film.frame.number == 36 && c->film.frames_shown == 3);
    client_film_request_stop(c);
    CHECK(c->film_open && c->film_stopping);
    client_audio_pump(c, 1.0); client_film_tick(c, 1.0f);
    CHECK(!c->film_open && c->film_done && c->bed_frames == 0);
    CHECK(c->film_audio_submitted == c->film_stop_at + 3528);
    CHECK(client_film_start(c, "TAKE1BP.STX"));
    c->film.frame_limit = 5; c->film_stop_at = 4 * 1764;
    for (i = 0; i < 4; i++) {
        client_film_tick(c, 0.0f); client_audio_pump(c, 0.04);
    }
    CHECK(c->film.frame.number == 4 && c->film.frames_shown == 4);
    client_film_tick(c, 0.0f); /* last picture stays through the four-field fade */
    CHECK(c->film_open && c->film.frame.number == 4);
    client_audio_pump(c, 0.08); client_film_tick(c, 0.0f);
    CHECK(!c->film_open && c->film_audio_submitted == 10584);
    CHECK(client_film_start(c, "TAKE1BP.STX"));
    c->film.fps = 30; client_film_tick(c, 0.0f);
    client_audio_pump(c, 1.0); client_film_tick(c, 0.0f);
    CHECK(c->film.frame.number == 31); /* same scheduler's NTSC cadence */
    free(reference); free(rgb); free(c->film_rgb); c->film_rgb = NULL;
    close_client(c);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--music")) {
        test_disc_music(argv[2]);
        printf("music client: %d checks, %d failures\n", checks, failures);
        return failures ? 1 : 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--gain")) {
        test_disc_gain_audio(argv[2], argc >= 4 ? argv[3] : NULL);
        printf("gain audio client: %d checks, %d failures\n", checks, failures);
        return failures ? 1 : 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--burn")) {
        test_disc_burn_audio(argv[2], argc >= 4 ? argv[3] : NULL);
        printf("burn audio client: %d checks, %d failures\n", checks, failures);
        return failures ? 1 : 0;
    }
    test_loop_export();
    test_native_voice_clock();
    test_live_volume();
    test_silent_playlist_entry();
    test_voice_lifetime();
    test_final_sample();
    test_keyoff_tail();
    test_mix_cancellation();
    test_cached_bed();
    test_mix_gain_path();
    test_playlist_clock();
    test_pause_audio();
    test_film_audio_policy();
    test_film_queue_clock();
    test_request_pitch();
    test_bank_bounds();
    test_retail_requests();
    test_bank_requests();
    test_creature_projectile_reports();
    test_spatial_windows();
    test_register_gain();
    test_entity_owners();
    test_multiplayer_sources();
    test_body_sources();
    test_burn_sound_resolution();
    if (argc == 2) {
        test_disc_audio(argv[1]); test_film_sync(argv[1]);
        test_disc_burn_audio(argv[1], NULL); test_disc_music(argv[1]);
    }
    else if (argc != 1) return 2;
    printf("audio client: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
