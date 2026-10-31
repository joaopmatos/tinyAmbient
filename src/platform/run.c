/* The daemon main loop: device supervision, session timer, and quit handling.
 *
 * Two threads touch the engine: this loop and miniaudio's audio thread.
 * Everything they share is atomic (AGENTS.md 1.2). The loop owns the session
 * timer because the stop-after deadline is main-thread state (ARCHITECTURE 1);
 * the audio thread never reads it.
 *
 * IPC and single-instance enforcement land in M7. Until then --foreground is
 * the only caller and Ctrl+C is the only quit path; the loop is written so M7
 * can add IPC polling and daemon forwarding without moving any of this. */

#if !defined(_WIN32)
/* clock_gettime and nanosleep need the POSIX feature macros before any system
 * header is included: -std=c11 hides them otherwise. */
#define _POSIX_C_SOURCE 200809L
#endif

#include <signal.h>
#include <stdatomic.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#include "audio/device.h"
#include "core/engine.h"
#include "platform/platform.h"

/* Device reinitialization schedule from PRD FR-16: 100 ms, 500 ms, 2 s, then
 * every 5 s. The last entry repeats forever: a device that comes back after a
 * long absence must still resume the session. */
static const long backoff_ms[] = {100, 500, 2000, 5000};
#define BACKOFF_STEPS ((int)(sizeof(backoff_ms) / sizeof(backoff_ms[0])))

/* How often the loop wakes up to check the timer, the device, and quit. It
 * also bounds how late a restart attempt or a stop-after expiry can fire.
 * 50 ms costs 20 wakeups a second and is far below anything a human notices. */
#define TICK_MS 50

static _Atomic int quit_requested;

void wn_platform_request_quit(void)
{
    /* Async-signal-safe: a lock-free atomic store, so the handler below may
     * call it. */
    atomic_store_explicit(&quit_requested, 1, memory_order_relaxed);
}

static int quit_pending(void)
{
    return atomic_load_explicit(&quit_requested, memory_order_relaxed) != 0;
}

static void quit_handler(int sig)
{
    (void)sig;
    wn_platform_request_quit();
}

static long now_ms(void)
{
#if defined(_WIN32)
    return (long)GetTickCount64();
#else
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
#endif
}

static void sleep_ms(long ms)
{
    if (ms <= 0) {
        return;
    }
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    {
        struct timespec ts;

        ts.tv_sec = (time_t)(ms / 1000);
        ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        /* EINTR is fine: the next iteration re-checks quit and the timers. */
        (void)nanosleep(&ts, NULL);
    }
#endif
}

int wn_platform_run(wn_engine *e, long stop_after_ms, long stop_fade_ms)
{
    long deadline = 0; /* 0 = no timer armed */
    long next_retry = 0;
    int fading_out = 0;
    int restart_pending = 0;
    int attempt = 0;
    int index;

    if (e == NULL) {
        return -1;
    }

    atomic_store_explicit(&quit_requested, 0, memory_order_relaxed);

    /* SIGINT and SIGTERM are both ISO C signal numbers, so this works on
     * Windows as well. A second Ctrl+C falls back to the default terminate
     * action, which is a fine escape hatch if the loop ever wedges. */
    (void)signal(SIGINT, quit_handler);
    (void)signal(SIGTERM, quit_handler);

    /* A device missing at startup is a hard failure with a clear message; a
     * device lost later is retried forever below (FR-16). */
    if (wn_audio_start(e) != 0) {
        return -1;
    }

    if (stop_after_ms > 0) {
        deadline = now_ms() + stop_after_ms;
    }

    while (!quit_pending()) {
        long now = now_ms();

        /* FR-16: a device that stopped or was removed is rebuilt with
         * backoff. The engine is untouched, so parameters and playback state
         * survive; the fade-in request keeps the resume click-free. */
        if (!restart_pending && wn_audio_needs_restart()) {
            wn_audio_stop();
            restart_pending = 1;
            next_retry = now + backoff_ms[0];
            attempt = 1;
        } else if (restart_pending && now >= next_retry) {
            if (wn_audio_start(e) == 0) {
                restart_pending = 0;
                attempt = 0;
                wn_engine_fade_in(e);
            } else {
                index = attempt < BACKOFF_STEPS ? attempt : BACKOFF_STEPS - 1;
                next_retry = now + backoff_ms[index];
                attempt++;
            }
        }

        /* FR-05: at expiry the engine fades to silence and pauses itself;
         * this loop only requests it and then consumes the timer. The timer
         * is session state and is never written to configuration. */
        if (deadline > 0 && !fading_out && now >= deadline) {
            wn_engine_fade_out(e, stop_fade_ms > 0 ? (unsigned)stop_fade_ms
                                                   : WN_STOP_FADE_MS);
            fading_out = 1;
        }
        if (fading_out && !wn_engine_is_playing(e)) {
            deadline = 0;
            fading_out = 0;
        }

        sleep_ms(TICK_MS);
    }

    wn_audio_stop();
    return 0;
}
