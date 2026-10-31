/* Session check: drives the M6 run loop end to end through the null audio
 * backend. It covers the observable half of PRD FR-05 that unit tests cannot:
 * the timer expires, the fade pauses playback, and the daemon stays alive.
 *
 * Times are scaled down. The product uses minutes and a 20 s fade; waiting for
 * either in a test suite is not acceptable, so the harness passes its own
 * stop-after and fade lengths to wn_platform_run. The sequence is identical.
 *
 * Not part of the shipped binary. POSIX only: the harness needs a second thread
 * to observe a loop that is supposed to keep running, and MSVC has no C11
 * threads; Windows coverage comes from the M8 parity pass. */

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

#include "core/config.h"
#include "core/engine.h"
#include "core/noise.h"
#include "platform/platform.h"

#define STOP_AFTER_MS 300L
#define STOP_FADE_MS 200L

/* Generous ceilings, so a slow CI machine is slow rather than failing. */
#define START_TIMEOUT_MS 3000L
#define PAUSE_TIMEOUT_MS 3000L
#define QUIT_TIMEOUT_MS 3000L

static _Atomic int loop_done;
static _Atomic int loop_result;

static long now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void sleep_ms(long ms)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

static void *run_loop(void *arg)
{
    int result = wn_platform_run((wn_engine *)arg, STOP_AFTER_MS, STOP_FADE_MS);

    atomic_store_explicit(&loop_result, result, memory_order_relaxed);
    atomic_store_explicit(&loop_done, 1, memory_order_relaxed);
    return NULL;
}

int main(void)
{
    wn_config cfg;
    wn_engine *engine;
    pthread_t thread;
    long start;
    int failed = 0;

    cfg.noise = WN_NOISE_WHITE;
    cfg.volume = 30;
    cfg.mono = 0;
    cfg.fade_ms = 250;

    engine = wn_engine_create(&cfg);
    if (engine == NULL) {
        fprintf(stderr, "session_check: cannot create the engine\n");
        return 1;
    }

    if (pthread_create(&thread, NULL, run_loop, engine) != 0) {
        fprintf(stderr, "session_check: cannot start the loop thread\n");
        wn_engine_destroy(engine);
        return 1;
    }

    /* The callback must run before expiry, otherwise the pause below proves
     * nothing. */
    start = now_ms();
    while (wn_engine_frames_rendered(engine) == 0) {
        if (now_ms() - start > START_TIMEOUT_MS) {
            fprintf(stderr, "session_check: the audio callback never ran\n");
            failed = 1;
            break;
        }
        sleep_ms(10);
    }

    /* Expiry plus the fade: the engine must pause itself. */
    if (!failed) {
        start = now_ms();
        while (wn_engine_is_playing(engine)) {
            if (now_ms() - start > PAUSE_TIMEOUT_MS) {
                fprintf(stderr, "session_check: playback did not pause after "
                                "stop-after\n");
                failed = 1;
                break;
            }
            sleep_ms(10);
        }
    }

    /* The daemon stays alive after the timer fires (PRD FR-05). */
    if (!failed) {
        sleep_ms(STOP_FADE_MS / 2);
        if (atomic_load_explicit(&loop_done, memory_order_relaxed)) {
            fprintf(stderr, "session_check: the loop exited after the timer\n");
            failed = 1;
        }
    }

    wn_platform_request_quit();
    start = now_ms();
    while (!atomic_load_explicit(&loop_done, memory_order_relaxed) &&
           now_ms() - start < QUIT_TIMEOUT_MS) {
        sleep_ms(10);
    }
    if (!atomic_load_explicit(&loop_done, memory_order_relaxed)) {
        fprintf(stderr, "session_check: the loop did not quit\n");
        failed = 1;
    }

    (void)pthread_join(thread, NULL);

    if (atomic_load_explicit(&loop_result, memory_order_relaxed) != 0) {
        fprintf(stderr, "session_check: the loop reported a failure\n");
        failed = 1;
    }
    if (wn_engine_is_playing(engine)) {
        fprintf(stderr, "session_check: playback resumed after quit\n");
        failed = 1;
    }

    wn_engine_destroy(engine);

    if (failed) {
        return 1;
    }

    printf("session_check: ok\n");
    return 0;
}
