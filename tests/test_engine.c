/* Engine tests: parameters, mono duplication, pause, volume, and the M6 fades.
 *
 * The audio callback is not involved; wn_engine_render is the same function the
 * callback calls, so everything here is also what a DAC would receive.
 *
 * Fades are frame-based, so a test can drive them at full speed by rendering
 * blocks. WN_FADE_MS is 20 ms = 960 frames, which is just under one FRAMES
 * block, so a single block covers a complete short fade. */

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

/* Renders one block and fails the test if the engine could not fill it. */
#define RENDER(engine, buffer) wn_engine_render((engine), (buffer), FRAMES)

static wn_engine *make_engine(int noise, int volume, int mono, int fade_ms)
{
    wn_config cfg;

    cfg.noise = noise;
    cfg.volume = volume;
    cfg.mono = mono;
    cfg.fade_ms = fade_ms;

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

/* Renders until the envelope has settled, so a test that measures a steady
 * level does not measure a fade-in. */
static void settle(wn_engine *engine, float *buffer)
{
    RENDER(engine, buffer);
    RENDER(engine, buffer);
    RENDER(engine, buffer);
}

/* Renders `blocks` blocks and checks FR-04's bounds and step limit across
 * buffer boundaries as well as inside them. */
typedef struct {
    int have_last;
    float last[WN_CHANNELS];
    float max_step;
    int out_of_range;
} signal_meter;

static void meter_feed(signal_meter *meter, const float *buffer,
                       unsigned frames)
{
    unsigned i;
    int ch;

    for (i = 0; i < frames; i++) {
        for (ch = 0; ch < WN_CHANNELS; ch++) {
            float sample = buffer[2 * i + ch];

            if (sample > 1.0f || sample < -1.0f) {
                meter->out_of_range = 1;
            }
            if (meter->have_last) {
                float step = fabsf(sample - meter->last[ch]);

                if (step > meter->max_step) {
                    meter->max_step = step;
                }
            }
            meter->last[ch] = sample;
        }
    }
    meter->have_last = 1;
}

static void test_mono_duplicates(void)
{
    float buffer[FRAMES * WN_CHANNELS];
    wn_engine *engine = make_engine(WN_NOISE_WHITE, 100, 1, 0);
    unsigned frame;

    CHECK(engine != NULL);
    if (engine == NULL) {
        return;
    }

    RENDER(engine, buffer);

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
    wn_engine *engine = make_engine(WN_NOISE_WHITE, 100, 0, 0);
    unsigned frame;
    int differs = 0;

    CHECK(engine != NULL);
    if (engine == NULL) {
        return;
    }

    RENDER(engine, buffer);

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
    wn_engine *engine = make_engine(WN_NOISE_WHITE, 0, 0, 0);

    CHECK(engine != NULL);
    if (engine == NULL) {
        return;
    }

    RENDER(engine, buffer);

    CHECK(buffer_is_silent(buffer, FRAMES));
    CHECK(wn_engine_is_playing(engine));
    CHECK(wn_engine_frames_rendered(engine) == FRAMES);

    wn_engine_destroy(engine);
}

static void test_volume_is_linear(void)
{
    float loud[FRAMES * WN_CHANNELS];
    float quiet[FRAMES * WN_CHANNELS];
    wn_engine *full = make_engine(WN_NOISE_WHITE, 100, 0, 0);
    wn_engine *half = make_engine(WN_NOISE_WHITE, 50, 0, 0);
    double ratio;

    CHECK(full != NULL && half != NULL);
    if (full == NULL || half == NULL) {
        wn_engine_destroy(full);
        wn_engine_destroy(half);
        return;
    }

    RENDER(full, loud);
    RENDER(half, quiet);

    ratio = rms(loud, FRAMES) / rms(quiet, FRAMES);
    CHECK(ratio > 1.99 && ratio < 2.01);

    wn_engine_destroy(full);
    wn_engine_destroy(half);
}

static void test_initial_fade_in(void)
{
    float first[FRAMES * WN_CHANNELS];
    float second[FRAMES * WN_CHANNELS];
    float steady[FRAMES * WN_CHANNELS];
    wn_engine *engine = make_engine(WN_NOISE_WHITE, 100, 0, 0);

    CHECK(engine != NULL);
    if (engine == NULL) {
        return;
    }

    RENDER(engine, first);
    RENDER(engine, second);
    RENDER(engine, steady);

    /* The envelope opens from exactly zero, so the very first sample is
     * silence and nothing is cut off at the DAC (the "no clicks on start"
     * goal). */
    CHECK(first[0] == 0.0f);
    CHECK(first[1] == 0.0f);
    CHECK(buffer_has_signal(first, FRAMES));

    /* The first block holds the ramp, so it is clearly quieter than a settled
     * block; the second is already settled because the fade is 960 frames. */
    CHECK(rms(first, FRAMES) < rms(steady, FRAMES) * 0.8);
    CHECK(rms(second, FRAMES) > rms(steady, FRAMES) * 0.9);

    wn_engine_destroy(engine);
}

static void test_play_pause_toggle(void)
{
    float buffer[FRAMES * WN_CHANNELS];
    float steady[FRAMES * WN_CHANNELS];
    wn_engine *engine = make_engine(WN_NOISE_WHITE, 60, 0, 0);
    int attempt;

    CHECK(engine != NULL);
    if (engine == NULL) {
        return;
    }

    CHECK(wn_engine_is_playing(engine));
    settle(engine, steady);

    /* Pausing is immediate state, but the audio fades over 20 ms. */
    CHECK(wn_engine_command(engine, "pause") == 0);
    CHECK(!wn_engine_is_playing(engine));

    RENDER(engine, buffer);
    CHECK(buffer_has_signal(buffer, FRAMES));
    CHECK(rms(buffer, FRAMES) < rms(steady, FRAMES) * 0.8);

    for (attempt = 0; attempt < 4 && !buffer_is_silent(buffer, FRAMES);
         attempt++) {
        RENDER(engine, buffer);
    }
    CHECK(buffer_is_silent(buffer, FRAMES));

    CHECK(wn_engine_command(engine, "play") == 0);
    CHECK(wn_engine_is_playing(engine));
    RENDER(engine, buffer);
    CHECK(buffer_has_signal(buffer, FRAMES));

    CHECK(wn_engine_command(engine, "toggle") == 0);
    CHECK(!wn_engine_is_playing(engine));
    CHECK(wn_engine_command(engine, "toggle") == 0);
    CHECK(wn_engine_is_playing(engine));

    wn_engine_destroy(engine);
}

static void test_volume_change_fades(void)
{
    float buffer[FRAMES * WN_CHANNELS];
    float steady[FRAMES * WN_CHANNELS];
    float final[FRAMES * WN_CHANNELS];
    wn_engine *engine = make_engine(WN_NOISE_WHITE, 100, 0, 0);
    double steady_rms;
    double fade_rms;

    CHECK(engine != NULL);
    if (engine == NULL) {
        return;
    }

    settle(engine, steady);
    steady_rms = rms(steady, FRAMES);

    CHECK(wn_engine_command(engine, "volume 25") == 0);
    CHECK(wn_engine_volume(engine) == 25);

    /* The block that holds the ramp is between the old and the new level. */
    RENDER(engine, buffer);
    fade_rms = rms(buffer, FRAMES);
    CHECK(fade_rms > steady_rms * 0.2);
    CHECK(fade_rms < steady_rms * 0.8);

    /* After the fade the level is a quarter of the old one. */
    settle(engine, final);

    CHECK(rms(final, FRAMES) > steady_rms * 0.23);
    CHECK(rms(final, FRAMES) < steady_rms * 0.27);

    wn_engine_destroy(engine);
}

static void test_noise_switch_crossfades(void)
{
    float switched[FRAMES * WN_CHANNELS];
    float reference[FRAMES * WN_CHANNELS];
    wn_engine *engine = make_engine(WN_NOISE_BROWN, 100, 0, 250);
    wn_engine *control = make_engine(WN_NOISE_BROWN, 100, 0, 250);
    signal_meter meter;
    unsigned block;

    CHECK(engine != NULL && control != NULL);
    if (engine == NULL || control == NULL) {
        wn_engine_destroy(engine);
        wn_engine_destroy(control);
        return;
    }

    /* Both engines see the same signal until the switch. */
    settle(engine, switched);
    settle(control, reference);

    CHECK(wn_engine_command(engine, "noise pink") == 0);
    CHECK(wn_engine_noise(engine) == WN_NOISE_PINK);

    RENDER(engine, switched);
    RENDER(control, reference);

    /* The outgoing generator was copied, so the first mixed sample is exactly
     * the sample the old signal would have produced next: no discontinuity at
     * the moment of the switch. */
    CHECK(switched[0] == reference[0]);
    CHECK(switched[1] == reference[1]);

    /* The whole crossfade, including both buffer boundaries, stays inside the
     * FR-04 bounds and never steps. Brown and pink are both smooth, so the
     * 0.25 limit applies to the signal itself. */
    meter.have_last = 0;
    meter.max_step = 0.0f;
    meter.out_of_range = 0;

    meter_feed(&meter, reference, FRAMES);
    meter_feed(&meter, switched, FRAMES);
    for (block = 0; block < 16; block++) {
        RENDER(engine, switched);
        meter_feed(&meter, switched, FRAMES);
    }

    CHECK(meter.out_of_range == 0);
    CHECK(meter.max_step <= 0.25f);

    wn_engine_destroy(engine);
    wn_engine_destroy(control);
}

static void test_second_switch_waits_for_the_first(void)
{
    float buffer[FRAMES * WN_CHANNELS];
    wn_engine *engine = make_engine(WN_NOISE_BROWN, 100, 0, 250);
    signal_meter meter;
    unsigned block;

    CHECK(engine != NULL);
    if (engine == NULL) {
        return;
    }

    settle(engine, buffer);

    /* 250 ms is 12000 frames: the second change lands mid-crossfade and must
     * wait for the first one to finish rather than jump the mix. */
    CHECK(wn_engine_command(engine, "noise pink") == 0);
    RENDER(engine, buffer);
    RENDER(engine, buffer);
    RENDER(engine, buffer);
    CHECK(wn_engine_command(engine, "noise brown") == 0);
    CHECK(wn_engine_noise(engine) == WN_NOISE_BROWN);

    meter.have_last = 0;
    meter.max_step = 0.0f;
    meter.out_of_range = 0;

    for (block = 0; block < 40; block++) {
        RENDER(engine, buffer);
        meter_feed(&meter, buffer, FRAMES);
    }

    CHECK(meter.out_of_range == 0);
    CHECK(meter.max_step <= 0.25f);
    CHECK(wn_engine_noise(engine) == WN_NOISE_BROWN);

    wn_engine_destroy(engine);
}

static void test_stop_fade_pauses(void)
{
    float buffer[FRAMES * WN_CHANNELS];
    wn_engine *engine = make_engine(WN_NOISE_WHITE, 100, 0, 0);
    int block;

    CHECK(engine != NULL);
    if (engine == NULL) {
        return;
    }

    settle(engine, buffer);

    /* 50 ms is 2400 frames: it lands in the third 1024-frame block. */
    wn_engine_fade_out(engine, 50);
    CHECK(wn_engine_is_playing(engine));

    RENDER(engine, buffer);
    CHECK(buffer_has_signal(buffer, FRAMES));

    for (block = 0; block < 4; block++) {
        RENDER(engine, buffer);
    }

    CHECK(!wn_engine_is_playing(engine));
    CHECK(buffer_is_silent(buffer, FRAMES));

    /* The session can be resumed after the auto-stop. */
    CHECK(wn_engine_command(engine, "play") == 0);
    RENDER(engine, buffer);
    CHECK(buffer_has_signal(buffer, FRAMES));

    wn_engine_destroy(engine);
}

static void test_stop_fade_cancelled_by_a_change(void)
{
    float buffer[FRAMES * WN_CHANNELS];
    float steady[FRAMES * WN_CHANNELS];
    wn_engine *engine = make_engine(WN_NOISE_WHITE, 100, 0, 0);
    int block;

    CHECK(engine != NULL);
    if (engine == NULL) {
        return;
    }

    settle(engine, steady);

    /* A stop fade is a request, not a lock: the user changing the volume
     * during it means they are still working, so playback continues. */
    wn_engine_fade_out(engine, 200);
    RENDER(engine, buffer);
    CHECK(wn_engine_command(engine, "volume 40") == 0);

    for (block = 0; block < 12; block++) {
        RENDER(engine, buffer);
    }

    CHECK(wn_engine_is_playing(engine));
    CHECK(buffer_has_signal(buffer, FRAMES));

    wn_engine_destroy(engine);
}

static void test_device_restart_fades_in(void)
{
    float buffer[FRAMES * WN_CHANNELS];
    float steady[FRAMES * WN_CHANNELS];
    wn_engine *engine = make_engine(WN_NOISE_WHITE, 100, 0, 0);

    CHECK(engine != NULL);
    if (engine == NULL) {
        return;
    }

    settle(engine, steady);

    /* A re-opened DAC starts from silence: the first sample after the restart
     * request is exactly zero, then the envelope opens over 20 ms (FR-16). */
    wn_engine_fade_in(engine);
    RENDER(engine, buffer);

    CHECK(buffer[0] == 0.0f);
    CHECK(buffer_has_signal(buffer, FRAMES));
    CHECK(rms(buffer, FRAMES) < rms(steady, FRAMES) * 0.8);

    wn_engine_destroy(engine);
}

static void test_mono_switch_mid_stream(void)
{
    float buffer[FRAMES * WN_CHANNELS];
    wn_engine *engine = make_engine(WN_NOISE_WHITE, 100, 0, 0);
    int differs = 0;
    unsigned frame;

    CHECK(engine != NULL);
    if (engine == NULL) {
        return;
    }

    settle(engine, buffer);

    CHECK(wn_engine_command(engine, "mono") == 0);
    RENDER(engine, buffer);
    for (frame = 0; frame < FRAMES; frame++) {
        if (buffer[2 * frame] != buffer[2 * frame + 1]) {
            differs = 1;
            break;
        }
    }
    CHECK(!differs);

    differs = 0;
    CHECK(wn_engine_command(engine, "stereo") == 0);
    RENDER(engine, buffer);
    for (frame = 0; frame < FRAMES; frame++) {
        if (buffer[2 * frame] != buffer[2 * frame + 1]) {
            differs = 1;
            break;
        }
    }
    CHECK(differs);

    wn_engine_destroy(engine);
}

static void test_commands_update_state(void)
{
    wn_engine *engine = make_engine(WN_NOISE_WHITE, 40, 0, 0);

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
    wn_engine *engine = make_engine(WN_NOISE_WHITE, 40, 0, 0);

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
    CHECK(wn_engine_volume(NULL) == 0);
    CHECK(!wn_engine_is_playing(NULL));
    CHECK(!wn_engine_is_mono(NULL));
    CHECK(wn_engine_frames_rendered(NULL) == 0);
    wn_engine_render(NULL, buffer, 1);
    wn_engine_fade_in(NULL);
    wn_engine_fade_out(NULL, 10);
    wn_engine_set_seed(NULL, 1u);
    wn_engine_destroy(NULL);
}

int main(void)
{
    test_mono_duplicates();
    test_stereo_differs();
    test_volume_zero_is_silent_while_playing();
    test_volume_is_linear();
    test_initial_fade_in();
    test_play_pause_toggle();
    test_volume_change_fades();
    test_noise_switch_crossfades();
    test_second_switch_waits_for_the_first();
    test_stop_fade_pauses();
    test_stop_fade_cancelled_by_a_change();
    test_device_restart_fades_in();
    test_mono_switch_mid_stream();
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
