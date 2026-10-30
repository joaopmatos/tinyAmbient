/* Playback check: opens the audio device, lets the callback run, and verifies
 * that frames were produced.
 *
 * With TINYAMBIENT_AUDIO=null it uses miniaudio's null backend, which drives the
 * callback in real time without hardware. That is how CI covers the audio path;
 * without the variable it uses the real default device.
 *
 * Not part of the shipped binary: it exists so the M4 device layer can be tested
 * before the daemon loop (M7) gives it a caller. */

#if !defined(_WIN32)
/* Must precede every system header: -std=c11 is strict, so POSIX declarations
 * (nanosleep) only become visible when the feature macro is requested first. */
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#include "audio/device.h"
#include "core/config.h"
#include "core/engine.h"
#include "core/noise.h"

static void sleep_ms(unsigned milliseconds)
{
#if defined(_WIN32)
    Sleep(milliseconds);
#else
    struct timespec ts;

    ts.tv_sec = (time_t)(milliseconds / 1000u);
    ts.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    (void)nanosleep(&ts, NULL);
#endif
}

int main(int argc, char **argv)
{
    wn_config cfg;
    wn_engine *engine;
    unsigned long frames;
    unsigned long expected;
    int milliseconds = 600;

    if (argc > 1) {
        milliseconds = atoi(argv[1]);
    }
    if (milliseconds <= 0) {
        fprintf(stderr, "play_check: usage: play_check [milliseconds]\n");
        return 2;
    }

    cfg.noise = WN_NOISE_WHITE;
    cfg.volume = 20;
    cfg.mono = 0;
    cfg.fade_ms = 250;

    engine = wn_engine_create(&cfg);
    if (engine == NULL) {
        fprintf(stderr, "play_check: cannot create the engine\n");
        return 1;
    }

    if (wn_audio_start(engine) != 0) {
        fprintf(stderr, "play_check: cannot open an audio device\n");
        wn_engine_destroy(engine);
        return 1;
    }

    sleep_ms((unsigned)milliseconds);
    frames = wn_engine_frames_rendered(engine);
    wn_audio_stop();
    wn_engine_destroy(engine);

    expected =
        (unsigned long)WN_SAMPLE_RATE * (unsigned long)milliseconds / 1000ul;

    printf("play_check: %lu frames in %d ms, %.1f ms of audio\n", frames,
           milliseconds, (double)frames * 1000.0 / (double)WN_SAMPLE_RATE);

    /* The callback starts after the backend's first period, so allow it to be
     * late but not absent. */
    if (frames < expected / 4ul) {
        fprintf(stderr, "play_check: the callback did not run (%lu of ~%lu)\n",
                frames, expected);
        return 1;
    }

    printf("play_check: ok\n");
    return 0;
}
