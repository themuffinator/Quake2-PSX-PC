/* Production recovery paths with SDL's dummy output: no speakers or input. */
#include <SDL3/SDL.h>

static bool fail_init, fail_open, fail_resume, fail_queue, fail_put, fail_flush;
static unsigned init_calls, open_calls, resume_calls, destroy_calls, put_calls;
static bool test_audio_init(SDL_InitFlags flags);
static SDL_AudioStream *test_audio_open(SDL_AudioDeviceID device,
    const SDL_AudioSpec *spec, SDL_AudioStreamCallback callback, void *userdata);
static bool test_audio_resume(SDL_AudioStream *stream);
static void test_audio_destroy(SDL_AudioStream *stream);
static int test_audio_queued(SDL_AudioStream *stream);
static bool test_audio_put(SDL_AudioStream *stream, const void *data, int bytes);
static bool test_audio_flush(SDL_AudioStream *stream);

#define SDL_InitSubSystem test_audio_init
#define SDL_OpenAudioDeviceStream test_audio_open
#define SDL_ResumeAudioStreamDevice test_audio_resume
#define SDL_DestroyAudioStream test_audio_destroy
#define SDL_GetAudioStreamQueued test_audio_queued
#define SDL_PutAudioStreamData test_audio_put
#define SDL_FlushAudioStream test_audio_flush
#define main q2_test_audio_device_client_main
int q2_test_audio_device_client_main(int argc, char **argv);
#include "../src/client/main.c"
#undef main
#undef SDL_InitSubSystem
#undef SDL_OpenAudioDeviceStream
#undef SDL_ResumeAudioStreamDevice
#undef SDL_DestroyAudioStream
#undef SDL_GetAudioStreamQueued
#undef SDL_PutAudioStreamData
#undef SDL_FlushAudioStream

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static bool test_audio_init(SDL_InitFlags flags)
{
    init_calls++;
    return fail_init ? SDL_SetError("test: audio initialization failed") : SDL_InitSubSystem(flags);
}

static SDL_AudioStream *test_audio_open(SDL_AudioDeviceID device,
    const SDL_AudioSpec *spec, SDL_AudioStreamCallback callback, void *userdata)
{
    open_calls++;
    CHECK(device == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
    if (fail_open) { SDL_SetError("test: no playback device"); return NULL; }
    return SDL_OpenAudioDeviceStream(device, spec, callback, userdata);
}

static bool test_audio_resume(SDL_AudioStream *stream)
{
    resume_calls++;
    return fail_resume ? SDL_SetError("test: playback could not resume") : SDL_ResumeAudioStreamDevice(stream);
}

static void test_audio_destroy(SDL_AudioStream *stream)
{
    destroy_calls++;
    SDL_DestroyAudioStream(stream);
}

static int test_audio_queued(SDL_AudioStream *stream)
{
    if (fail_queue) { SDL_SetError("test: queue unavailable"); return -1; }
    return SDL_GetAudioStreamQueued(stream);
}

static bool test_audio_put(SDL_AudioStream *stream, const void *data, int bytes)
{
    put_calls++;
    return fail_put ? SDL_SetError("test: submission failed") : SDL_PutAudioStreamData(stream, data, bytes);
}

static bool test_audio_flush(SDL_AudioStream *stream)
{
    return fail_flush ? SDL_SetError("test: flush failed") : SDL_FlushAudioStream(stream);
}

static client *new_client(void)
{
    client *c = calloc(1, sizeof(*c));
    if (!c) exit(2);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    fail_init = fail_open = fail_resume = fail_queue = fail_put = fail_flush = false;
    init_calls = open_calls = resume_calls = destroy_calls = put_calls = 0;
    c->build.video = Q2_VIDEO_PAL;
    c->film.fps = 25;
    c->film_open = c->film_stopping = true;
    c->film_stop_at = ~(u64)0;
    return c;
}

static void free_client(client *c)
{
    if (c->audio) SDL_DestroyAudioStream(c->audio);
    free(c);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

static void test_headless(void)
{
    client *c = new_client();
    c->headless = true;
    client_audio_pump(c, 1.0);
    CHECK(init_calls == 0 && open_calls == 0 && !c->audio);
    CHECK(c->audio_frames == 44100 && c->film_audio_submitted == 44100);
    free_client(c);
}

static void test_missing_backend(void)
{
    client *c = new_client();
    CHECK(SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "q2psx-test-missing-backend", SDL_HINT_OVERRIDE));
    client_audio_pump(c, 0.25); /* actual SDL initialization failure */
    CHECK(!c->audio && init_calls == 1 && open_calls == 0);
    CHECK(!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO));
    CHECK(c->film_audio_submitted == 11025);
    CHECK(SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE));
    client_audio_pump(c, 1.0);
    CHECK(c->audio && init_calls == 2 && open_calls == 1);
    CHECK(c->film_audio_mixed == c->film_audio_submitted);
    free_client(c);
}

/* Each failed stage keeps the silent clock alive and retries at a bounded
 * cadence. The successful retry is a real SDL dummy device, not a fake stream. */
static void test_startup_recovery(int stage)
{
    client *c = new_client();
    SDL_AudioSpec input;
    unsigned i;
    u64 silent_frames;
    if (stage == 0) fail_init = true;
    if (stage == 1) fail_open = true;
    if (stage == 2) fail_resume = true;
    client_audio_pump(c, 0.25);
    CHECK(!c->audio && c->audio_frames == 11025);
    CHECK(init_calls == 1);
    CHECK(open_calls == (stage == 0 ? 0u : 1u));
    CHECK(destroy_calls == (stage == 2 ? 1u : 0u));
    client_audio_pump(c, 0.25);
    client_audio_pump(c, 0.25);
    CHECK(init_calls == 1 && open_calls == (stage == 0 ? 0u : 1u));
    CHECK(c->audio_frames == 33075);
    fail_init = fail_open = fail_resume = false;
    silent_frames = c->film_audio_submitted;
    client_audio_pump(c, 0.5);
    CHECK(c->audio != NULL);
    CHECK(init_calls == (stage == 0 ? 2u : 1u)); /* no subsystem reference leak */
    CHECK(open_calls == (stage == 0 ? 1u : 2u));
    if (!c->audio) { free_client(c); return; }
    CHECK(!strcmp(SDL_GetCurrentAudioDriver(), "dummy"));
    CHECK(SDL_GetAudioStreamFormat(c->audio, &input, NULL));
    CHECK(input.freq == 44100 && input.channels == 2 && input.format == SDL_AUDIO_S16LE);
    CHECK(!SDL_AudioStreamDevicePaused(c->audio));

    /* Freeze this dummy device to make queued-versus-consumed time exact. */
    CHECK(SDL_PauseAudioStreamDevice(c->audio));
    CHECK(SDL_ClearAudioStream(c->audio));
    c->film_audio_submitted = c->film_audio_mixed = silent_frames;
    client_audio_pump(c, 0.0);
    CHECK(SDL_GetAudioStreamQueued(c->audio) >= (int)(CLIENT_AUDIO_TARGET_FRAMES * 4));
    CHECK(SDL_GetAudioStreamQueued(c->audio) < (int)((CLIENT_AUDIO_TARGET_FRAMES + CLIENT_AUDIO_CHUNK_FRAMES) * 4));
    client_film_tick(c, 0.0f);
    CHECK(c->film_audio_played == silent_frames); /* elapsed silent time is not replayed */
    CHECK(c->film_audio_submitted == c->film_audio_mixed);
    CHECK(SDL_FlushAudioStream(c->audio));
    CHECK(SDL_ResumeAudioStreamDevice(c->audio));
    for (i = 0; i < 100 && SDL_GetAudioStreamQueued(c->audio) > 0; i++) SDL_Delay(10);
    CHECK(SDL_GetAudioStreamQueued(c->audio) == 0);
    client_film_tick(c, 0.0f);
    CHECK(c->film_audio_played == c->film_audio_submitted);
    CHECK(open_calls == (stage == 0 ? 1u : 2u)); /* a working default stream is retained */
    free_client(c);
}

static void test_stream_failure(int stage)
{
    const SDL_AudioSpec spec = { SDL_AUDIO_S16LE, 2, 44100 };
    client *c = new_client();
    s16 pcm[512 * 2] = {0};
    c->headless = true; /* no retry device while checking the silent fallback */
    c->audio = SDL_CreateAudioStream(&spec, &spec);
    CHECK(c->audio != NULL);
    if (!c->audio) { free_client(c); return; }
    c->film_audio_mixed = c->film_audio_submitted = 5120;
    c->film_audio_played = 4608;
    if (stage == 0) {
        CHECK(SDL_PutAudioStreamData(c->audio, pcm, sizeof(pcm)));
        fail_queue = true;
    } else if (stage == 1) {
        fail_put = true;
    } else {
        c->film_stop_at = 0;
        c->film_audio_mixed = c->film_audio_submitted = 3521;
        c->film_audio_played = 3521;
        fail_flush = true;
    }
    client_audio_pump(c, stage == 0 ? 0.25 : 0.0);
    CHECK(!c->audio && destroy_calls == 1);
    CHECK(c->film_audio_submitted == c->film_audio_mixed);
    CHECK(c->film_audio_submitted == (stage == 0 ? 16145u : stage == 1 ? 5376u : 3528u));
    CHECK(put_calls == (stage == 0 ? 0u : 1u)); /* never spin on a failing submission */
    fail_queue = fail_put = fail_flush = false;
    client_film_tick(c, 0.0f);
    CHECK(c->film_audio_played == c->film_audio_submitted);
    if (stage == 2) CHECK(c->film_done && !c->film_open);
    else {
        u64 before = c->film_audio_submitted;
        client_audio_pump(c, 0.25);
        client_film_tick(c, 0.0f);
        CHECK(c->film_audio_played == before + 11025);
        c->headless = false;
        client_audio_pump(c, 1.0);
        CHECK(c->audio && open_calls == 1 && init_calls == 1);
        CHECK(c->film_audio_submitted == c->film_audio_mixed);
        CHECK(c->film_audio_submitted > c->film_audio_played);
    }
    free_client(c);
}

int main(void)
{
    int i;
    /* Override even an inherited driver hint: this process cannot use speakers. */
    if (!SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE)) return 2;
    test_headless();
    test_missing_backend();
    for (i = 0; i < 3; i++) test_startup_recovery(i);
    for (i = 0; i < 3; i++) test_stream_failure(i);
    SDL_Quit();
    printf("audio device client: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
