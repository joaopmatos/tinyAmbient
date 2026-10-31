/* Engine tests: parameter commands, mono duplication, pause, and volume. The
 * audio callback is not involved; wn_engine_render is the same function the
 * callback calls. */

#include <math.h>
#include <stdio.h>

#include "core/config.h"
#include "core/engine.h"
#include "core/noise.h"

static int failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

#define FRAMES 1024

static wn_engine *make_engine(int noise, int volume, int mono)
{
    wn_config cfg;

    cfg.noise = noise;
    cfg.volume = volume;
    cfg.mono = mono;
    cfg.fade_ms = 0;

    return wn_engine_create(&cfg);
}

static int buffer_is_silent(const float *buffer, unsigned frames)
{
    unsigned i;

    for (i = 0; i < frames * WN_CHANNELS; i++) {
        /* signbit catches -0.0: it compares equal to 0.0 but is not the same
         * bytes, and volume 0 must render exact silence. */
        if (buffer[i] != 0.0f || signbit(buffer[i])) {
            return 0;
        }
    }
    return 1;
}

static int buffer_has_signal(const float *buffer, unsigned frames)
{
    return !buffer_is_silent(buffer, frames);
}

static double rms(const float *buffer, unsigned frames)
{
    double sum = 0.0;
    unsigned i;

    for (i = 0; i < frames * WN_CHANNELS; i++) {
        sum += (double)buffer[i] * (double)buffer[i];
    }
    return sqrt(sum / (double)(frames * WN_CHANNELS));
}

static void test_mono_duplicates(void)
{
    float buffer[FRAMES * WN_CHANNELS];
    wn_engine *engine = make_engine(WN_NOISE_WHITE, 100, 1);
    unsigned frame;

    CHECK(engine != NULL);
    if (engine == NULL) {
        return;
    }

    wn_engine_render(engine, buffer, FRAMES);

    for (frame = 0; frame < FRAMES; frame++) {
        if (buffer[2 * frame] != buffer[2 * frame + 1]) {
            CHECK(buffer[2 * frame] == buffer[2 * frame + 1]);
            break;
        }
    }

    wn_engine_destroy(engine);
}

static void test_stereo_differs(void)
{
    float buffer[FRAMES * WN_CHANNELS];
    wn_engine *engine = make_engine(WN_NOISE_WHITE, 100, 0);
    unsigned frame;
    int differs = 0;

    CHECK(engine != NULL);
    if (engine == NULL) {
        return;
    }

    wn_engine_render(engine, buffer, FRAMES);

    for (frame = 0; frame < FRAMES; frame++) {
        if (buffer[2 * frame] != buffer[2 * frame + 1]) {
            differs = 1;
            break;
        }
    }

    CHECK(differs);
    wn_engine_destroy(engine);
}

static void test_volume_zero_is_silent_while_playing(void)
{
    float buffer[FRAMES * WN_CHANNELS];
    wn_engine *engine = make_engine(WN_NOISE_WHITE, 0, 0);

    CHECK(engine != NULL);
    if (engine == NULL) {
        return;
    }

    wn_engine_render(engine, buffer, FRAMES);

    CHECK(buffer_is_silent(buffer, FRAMES));
    CHECK(wn_engine_is_playing(engine));
    CHECK(wn_engine_frames_rendered(engine) == FRAMES);

    wn_engine_destroy(engine);
}

static void test_volume_is_linear(void)
{
    float loud[FRAMES * WN_CHANNELS];
    float quiet[FRAMES * WN_CHANNELS];
    wn_engine *full = make_engine(WN_NOISE_WHITE, 100, 0);
    wn_engine *half = make_engine(WN_NOISE_WHITE, 50, 0);
    double ratio;

    CHECK(full != NULL && half != NULL);
    if (full == NULL || half == NULL) {
        wn_engine_destroy(full);
        wn_engine_destroy(half);
        return;
    }

    wn_engine_render(full, loud, FRAMES);
    wn_engine_render(half, quiet, FRAMES);

    ratio = rms(loud, FRAMES) / rms(quiet, FRAMES);
    CHECK(ratio > 1.99 && ratio < 2.01);

    wn_engine_destroy(full);
    wn_engine_destroy(half);
}

static void test_play_pause_toggle(void)
{
    float buffer[FRAMES * WN_CHANNELS];
    wn_engine *engine = make_engine(WN_NOISE_WHITE, 60, 0);

    CHECK(engine != NULL);
    if (engine == NULL) {
        return;
    }

    CHECK(wn_engine_is_playing(engine));
    CHECK(wn_engine_command(engine, "pause") == 0);
    CHECK(!wn_engine_is_playing(engine));

    wn_engine_render(engine, buffer, FRAMES);
    CHECK(buffer_is_silent(buffer, FRAMES));

    CHECK(wn_engine_command(engine, "play") == 0);
    CHECK(wn_engine_is_playing(engine));
    wn_engine_render(engine, buffer, FRAMES);
    CHECK(buffer_has_signal(buffer, FRAMES));

    CHECK(wn_engine_command(engine, "toggle") == 0);
    CHECK(!wn_engine_is_playing(engine));
    CHECK(wn_engine_command(engine, "toggle") == 0);
    CHECK(wn_engine_is_playing(engine));

    wn_engine_destroy(engine);
}

static void test_commands_update_state(void)
{
    wn_engine *engine = make_engine(WN_NOISE_WHITE, 40, 0);

    CHECK(engine != NULL);
    if (engine == NULL) {
        return;
    }

    CHECK(wn_engine_noise(engine) == WN_NOISE_WHITE);
    CHECK(wn_engine_volume(engine) == 40);
    CHECK(wn_engine_is_mono(engine) == 0);

    CHECK(wn_engine_command(engine, "noise brown") == 0);
    CHECK(wn_engine_noise(engine) == WN_NOISE_BROWN);

    CHECK(wn_engine_command(engine, "volume 70") == 0);
    CHECK(wn_engine_volume(engine) == 70);

    CHECK(wn_engine_command(engine, "mono") == 0);
    CHECK(wn_engine_is_mono(engine) == 1);

    CHECK(wn_engine_command(engine, "stereo") == 0);
    CHECK(wn_engine_is_mono(engine) == 0);

    /* Multiple commands on one line apply in order. */
    CHECK(wn_engine_command(engine, "noise pink volume 20") == 0);
    CHECK(wn_engine_noise(engine) == WN_NOISE_PINK);
    CHECK(wn_engine_volume(engine) == 20);

    wn_engine_destroy(engine);
}

static void test_invalid_commands_leave_state_alone(void)
{
    wn_engine *engine = make_engine(WN_NOISE_WHITE, 40, 0);

    CHECK(engine != NULL);
    if (engine == NULL) {
        return;
    }

    CHECK(wn_engine_command(engine, "bogus") != 0);
    CHECK(wn_engine_command(engine, "volume 101") != 0);
    CHECK(wn_engine_command(engine, "noise gray") != 0);
    CHECK(wn_engine_command(engine, "status") != 0);
    CHECK(wn_engine_command(engine, "stop-after 5") != 0);
    CHECK(wn_engine_command(engine, "volume 80 stop-after 5") != 0);

    CHECK(wn_engine_noise(engine) == WN_NOISE_WHITE);
    CHECK(wn_engine_volume(engine) == 40);
    CHECK(wn_engine_is_playing(engine));

    wn_engine_destroy(engine);
}

static void test_null_arguments(void)
{
    float buffer[WN_CHANNELS];

    CHECK(wn_engine_command(NULL, "play") != 0);
    CHECK(wn_engine_noise(NULL) == WN_NOISE_NONE);
    CHECK(!wn_engine_is_playing(NULL));
    wn_engine_render(NULL, buffer, 1);
}

int main(void)
{
    test_mono_duplicates();
    test_stereo_differs();
    test_volume_zero_is_silent_while_playing();
    test_volume_is_linear();
    test_play_pause_toggle();
    test_commands_update_state();
    test_invalid_commands_leave_state_alone();
    test_null_arguments();

    if (failures != 0) {
        fprintf(stderr, "test_engine: %d check(s) failed\n", failures);
        return 1;
    }

    printf("test_engine: ok\n");
    return 0;
}
