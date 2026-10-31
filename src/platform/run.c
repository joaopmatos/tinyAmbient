/* The daemon main loop: device supervision, the session timer, IPC command
 * handling, and quit.
 *
 * Two threads touch the engine: this loop and miniaudio's audio thread.
 * Everything they share is atomic (AGENTS.md 1.2). The loop owns the session
 * timer and the persisted configuration because both are main-thread state
 * (ARCHITECTURE 1); the audio thread never reads either. */

#if !defined(_WIN32)
/* clock_gettime, nanosleep, fork, and friends need the POSIX feature macros
 * before any system header is included: -std=c11 hides them otherwise. */
#define _POSIX_C_SOURCE 200809L
#endif

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#endif

#include "audio/device.h"
#include "core/command.h"
#include "core/engine.h"
#include "core/status.h"
#include "platform/ipc.h"
#include "platform/platform.h"

/* Device reinitialization schedule from PRD FR-16: 100 ms, 500 ms, 2 s, then
 * every 5 s. The last entry repeats forever: a device that comes back after a
 * long absence must still resume the session. */
static const long backoff_ms[] = {100, 500, 2000, 5000};
#define BACKOFF_STEPS ((int)(sizeof(backoff_ms) / sizeof(backoff_ms[0])))

/* How often the loop wakes up to check the timer, the device, quit, and IPC.
 * It also bounds how late a forwarded command is answered, which is why the
 * FR-07 latency budget of 100 ms has room to spare. 50 ms costs 20 wakeups a
 * second and is far below anything a human notices. */
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

static void log_event(const wn_session *s, const char *text)
{
    if (s->verbose) {
        fprintf(stderr, "tinyambient: %s\n", text);
    }
}

/* Tells the process that forked us whether the daemon came up: 0 is ready, 1 is
 * a failed device start. If the child dies first, the parent sees EOF instead
 * and reports the same failure. */
static void notify_ready(wn_session *s, int ok)
{
    if (s->ready_fd < 0) {
        return;
    }
#if defined(_WIN32)
    char byte = ok ? 0 : 1;

    (void)_write(s->ready_fd, &byte, 1);
    (void)_close(s->ready_fd);
#else
    char byte = ok ? 0 : 1;

    (void)write(s->ready_fd, &byte, 1);
    (void)close(s->ready_fd);
#endif
    s->ready_fd = -1;
}

/* --- Command handling ------------------------------------------------------ */

static int session_supported(wn_cmd_kind kind)
{
    switch (kind) {
        case WN_CMD_NOISE:
        case WN_CMD_VOLUME:
        case WN_CMD_MONO:
        case WN_CMD_STEREO:
        case WN_CMD_PLAY:
        case WN_CMD_PAUSE:
        case WN_CMD_TOGGLE:
        case WN_CMD_STOP_AFTER:
        case WN_CMD_STATUS:
        case WN_CMD_QUIT:
            return 1;
        default:
            return 0;
    }
}

/* Mirrors core/config.c's view of which keys persist (PRD FR-06, D-06). */
static void apply_persisted(wn_config *cfg, const wn_cmd *cmd)
{
    switch (cmd->kind) {
        case WN_CMD_NOISE:
            cfg->noise = cmd->ivalue;
            break;
        case WN_CMD_VOLUME:
            cfg->volume = cmd->ivalue;
            break;
        case WN_CMD_MONO:
            cfg->mono = 1;
            break;
        case WN_CMD_STEREO:
            cfg->mono = 0;
            break;
        default:
            break;
    }
}

/* Applies one forwarded line and fills `reply`.
 *
 * The whole line is validated first, then the persisted settings are written
 * as one atomic save, and only then does the engine see anything. A failed
 * write therefore leaves both the file and the audio state unchanged, which is
 * what "commands apply to the daemon and write configuration" has to mean.
 *
 * Returns 1 when the daemon must quit after replying. */
static int handle_line(wn_engine *e, wn_session *s, long *deadline,
                       const char *line, long now, char *reply, size_t cap)
{
    char buffer[WN_LINE_MAX];
    wn_cmd cmds[WN_CMD_MAX];
    wn_config updated;
    char err[128];
    long new_deadline = *deadline;
    long remaining = WN_STOP_AFTER_SECONDS_OFF;
    int persist = 0;
    int want_status = 0;
    int want_quit = 0;
    size_t length;
    int n;
    int i;

    length = strlen(line);
    if (length >= sizeof(buffer)) {
        (void)snprintf(reply, cap, "error 2 command line is too long\n");
        return 0;
    }
    memcpy(buffer, line, length + 1);

    n = wn_cmd_parse_line(buffer, cmds, WN_CMD_MAX, err, sizeof(err));
    if (n <= 0) {
        (void)snprintf(reply, cap, "error 2 %s\n",
                       err[0] != '\0' ? err : "invalid command");
        return 0;
    }

    for (i = 0; i < n; i++) {
        if (!session_supported(cmds[i].kind)) {
            (void)snprintf(reply, cap,
                           "error 2 that command is not available over IPC\n");
            return 0;
        }
    }

    updated = s->config;
    for (i = 0; i < n; i++) {
        switch (cmds[i].kind) {
            case WN_CMD_NOISE:
            case WN_CMD_VOLUME:
            case WN_CMD_MONO:
            case WN_CMD_STEREO:
                apply_persisted(&updated, &cmds[i]);
                persist = 1;
                break;
            case WN_CMD_STOP_AFTER:
                new_deadline = cmds[i].ivalue == WN_STOP_AFTER_OFF
                                   ? 0
                                   : now + (long)cmds[i].ivalue * 60000L;
                break;
            case WN_CMD_STATUS:
                want_status = 1;
                break;
            case WN_CMD_QUIT:
                want_quit = 1;
                break;
            default:
                break;
        }
    }

    if (persist && s->config_path[0] != '\0' &&
        wn_config_file_save(s->config_path, &updated,
                            s->config_text_len > 0 ? s->config_text : NULL,
                            s->portable, err, sizeof(err)) != 0) {
        (void)snprintf(reply, cap, "error 1 %s\n",
                       err[0] != '\0' ? err : "cannot write the configuration");
        return 0;
    }
    s->config = updated;
    *deadline = new_deadline;

    /* Every kind left is either an engine command or handled above; the engine
     * rejects anything else, which cannot happen after the validation loop. */
    for (i = 0; i < n; i++) {
        (void)wn_engine_apply_cmd(e, &cmds[i]);
    }

    if (want_status) {
        if (*deadline > 0) {
            long left = *deadline - now;

            remaining = left > 0 ? (left + 999) / 1000 : 1;
        }
        (void)wn_status_format(reply, cap, wn_engine_is_playing(e),
                               wn_engine_noise(e), wn_engine_volume(e),
                               wn_engine_is_mono(e), remaining);
    } else {
        (void)snprintf(reply, cap, "ok\n");
    }

    return want_quit;
}

/* --- Run loop -------------------------------------------------------------- */

int wn_platform_run(wn_engine *e, wn_session *s)
{
    char line[WN_LINE_MAX];
    /* Six short lines from FR-08 fit in well under this. */
    char reply[WN_IPC_REPLY_MAX];
    long deadline = 0;
    long next_retry = 0;
    long stop_fade_ms;
    int restart_pending = 0;
    int attempt = 0;
    int index;

    if (e == NULL || s == NULL) {
        return -1;
    }

    atomic_store_explicit(&quit_requested, 0, memory_order_relaxed);

    /* SIGINT and SIGTERM are both ISO C signal numbers, so this works on
     * Windows as well. A second Ctrl+C falls back to the default terminate
     * action, which is a fine escape hatch if the loop ever wedges. */
    (void)signal(SIGINT, quit_handler);
    (void)signal(SIGTERM, quit_handler);
#if !defined(_WIN32)
    /* A client that disconnects mid-reply must not take the daemon down. */
    (void)signal(SIGPIPE, SIG_IGN);
#endif

    stop_fade_ms = s->stop_fade_ms > 0 ? s->stop_fade_ms : WN_STOP_FADE_MS;

    /* A device missing at startup is a hard failure with a clear message; a
     * device lost later is retried forever below (FR-16). */
    if (wn_audio_start(e) != 0) {
        notify_ready(s, 0);
        if (s->ipc != NULL) {
            wn_ipc_server_close(s->ipc);
            s->ipc = NULL;
        }
        return -1;
    }
    notify_ready(s, 1);
    log_event(s, "audio device started");

    if (s->stop_after_ms > 0) {
        deadline = now_ms() + s->stop_after_ms;
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
            log_event(s, "audio device lost; retrying");
        } else if (restart_pending && now >= next_retry) {
            if (wn_audio_start(e) == 0) {
                restart_pending = 0;
                attempt = 0;
                wn_engine_fade_in(e);
                log_event(s, "audio device restored");
            } else {
                index = attempt < BACKOFF_STEPS ? attempt : BACKOFF_STEPS - 1;
                next_retry = now + backoff_ms[index];
                attempt++;
            }
        }

        /* FR-05: at expiry the engine fades to silence and pauses itself; this
         * loop only requests it and consumes the timer, so status reports the
         * timer as off from the moment it fires. The timer is session state
         * and is never written to configuration. */
        if (deadline > 0 && now >= deadline) {
            wn_engine_fade_out(e, (unsigned)stop_fade_ms);
            deadline = 0;
            log_event(s, "stop-after expired; fading out");
        }

        /* Serving a client is also the loop's sleep: poll returns after
         * TICK_MS at the latest, which keeps the timer and the device
         * supervision on the same cadence as before. */
        if (s->ipc != NULL) {
            int ready = wn_ipc_server_wait(s->ipc, line, sizeof(line), TICK_MS);

            if (ready > 0) {
                int want_quit = handle_line(e, s, &deadline, line, now_ms(),
                                            reply, sizeof(reply));

                /* Remove the socket before the final reply, so the client
                 * cannot observe a socket whose daemon is already leaving. */
                if (want_quit) {
                    wn_ipc_server_stop_accepting(s->ipc);
                }
                (void)wn_ipc_server_reply(s->ipc, reply);
                if (want_quit) {
                    log_event(s, "quit requested over IPC");
                    break;
                }
            }
        } else {
            sleep_ms(TICK_MS);
        }
    }

    wn_audio_stop();
    log_event(s, "stopped");

    if (s->ipc != NULL) {
        wn_ipc_server_close(s->ipc);
        s->ipc = NULL;
    }

    return 0;
}

/* --- Client and detach ----------------------------------------------------- */

int wn_platform_ipc_send(const char *line, char *reply, size_t cap)
{
    return wn_ipc_client_send(line, reply, cap);
}

int wn_platform_detach(int *ready_fd, char *err, size_t err_cap)
{
#if defined(_WIN32)
    (void)ready_fd;
    (void)snprintf(err, err_cap,
                   "the background daemon requires Windows parity (M8)");
    return WN_DETACH_ERROR;
#else
    int pipe_fds[2];
    pid_t pid;

    if (ready_fd == NULL) {
        (void)snprintf(err, err_cap, "internal: no readiness pipe");
        return WN_DETACH_ERROR;
    }
    *ready_fd = -1;

    if (pipe(pipe_fds) != 0) {
        (void)snprintf(err, err_cap, "cannot create the startup pipe");
        return WN_DETACH_ERROR;
    }

    pid = fork();
    if (pid < 0) {
        (void)close(pipe_fds[0]);
        (void)close(pipe_fds[1]);
        (void)snprintf(err, err_cap, "cannot fork the daemon");
        return WN_DETACH_ERROR;
    }

    if (pid > 0) {
        /* Parent: wait for the child's one-byte readiness report. The timeout
         * covers a wedged audio backend; the child is killed if it never
         * answers so no half-started daemon keeps the socket. */
        struct pollfd pending;
        char byte = 1;
        int ready;

        (void)close(pipe_fds[1]);
        pending.fd = pipe_fds[0];
        pending.events = POLLIN;
        pending.revents = 0;

        ready = poll(&pending, 1, 5000);
        if (ready > 0 && read(pipe_fds[0], &byte, 1) == 1 && byte == 0) {
            (void)close(pipe_fds[0]);
            return WN_DETACH_PARENT;
        }

        (void)close(pipe_fds[0]);
        (void)kill(pid, SIGKILL);
        (void)snprintf(err, err_cap, "the daemon did not start");
        return WN_DETACH_PARENT_FAIL;
    }

    /* Child: detach from the terminal and lose the inherited standard streams.
     * The readiness pipe stays open; it is already above fd 2. */
    (void)close(pipe_fds[0]);
    (void)setsid();

    {
        int devnull = open("/dev/null", O_RDWR);

        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            (void)dup2(devnull, STDOUT_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                (void)close(devnull);
            }
        }
    }

    *ready_fd = pipe_fds[1];
    return WN_DETACH_CHILD;
#endif
}
