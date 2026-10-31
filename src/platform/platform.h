#ifndef WN_PLATFORM_PLATFORM_H
#define WN_PLATFORM_PLATFORM_H

#include <stddef.h>

#include "core/config.h"
#include "core/engine.h"
#include "platform/config_file.h"
#include "platform/ipc.h"
#include "platform/paths.h"

/* The platform layer owns the daemon loop, IPC, and device restart backoff.
 * It is the only layer allowed to call OS APIs (AGENTS.md section 2). */

/* Results from wn_platform_detach. */
#define WN_DETACH_PARENT 0      /* the daemon reported ready; exit 0 */
#define WN_DETACH_CHILD 1       /* this process is the daemon */
#define WN_DETACH_PARENT_FAIL 2 /* the daemon died or never reported */
#define WN_DETACH_ERROR (-1)    /* could not fork at all */

/* Session state for one daemon loop. The loop owns the mutable configuration
 * so a forwarded setting command can persist from the daemon (PRD FR-06)
 * without the client touching the file. */
typedef struct {
    wn_config config; /* current persisted settings */
    char config_path[WN_PATH_MAX];
    char config_text[WN_CONFIG_TEXT_MAX];
    size_t config_text_len;
    int portable;

    /* Initial session timer in milliseconds, or a negative value for "off"
     * (PRD FR-05). The timer is session state and is never persisted. */
    long stop_after_ms;

    /* Length of the fade-out at expiry, so a test build can shorten the
     * product's 20 s without a second code path. 0 selects WN_STOP_FADE_MS. */
    long stop_fade_ms;

    /* Write one byte here once the audio device is up (or on failure), then
     * close it. -1 when the caller needs no readiness report. */
    int ready_fd;

    /* Log lifecycle events to stderr. --foreground sets it (PRD FR-12); a
     * background daemon has stderr on /dev/null anyway. */
    int verbose;

    /* Listening socket, owned and closed by the loop. NULL disables IPC, which
     * is what the in-process session test uses. */
    wn_ipc_server *ipc;
} wn_session;

/* Runs the daemon until quit. Starts the device, serves IPC when
 * `s->ipc` is set, handles the restart backoff and the session timer, and
 * reports readiness on `s->ready_fd` once the device is up. Returns 0 on a
 * clean exit, non-zero when no audio device could be opened. */
int wn_platform_run(wn_engine *e, wn_session *s);

/* Asks wn_platform_run to return. Any thread may call it, including a signal
 * handler. */
void wn_platform_request_quit(void);

/* Client side of IPC: sends one command line and stores the reply. Returns
 * WN_IPC_OK, WN_IPC_ABSENT (no daemon), or WN_IPC_ERROR. */
int wn_platform_ipc_send(const char *line, char *reply, size_t cap);

/* True when this platform implements the daemon and IPC. Windows returns 0
 * until the M8 parity pass; the CLI then keeps settings commands in-process. */
int wn_platform_ipc_supported(void);

/* Detaches the current process into a background daemon.
 *
 * In the parent, waits for the daemon's readiness report and returns
 * WN_DETACH_PARENT (exit 0), WN_DETACH_PARENT_FAIL (exit 1), or
 * WN_DETACH_ERROR. In the child, returns WN_DETACH_CHILD with `ready_fd` set
 * to the pipe it must report on. The child also detaches from the terminal and
 * redirects stdin/stdout/stderr to /dev/null; the parent must not close the
 * IPC server it opened before the fork, because the child still uses it. */
int wn_platform_detach(int *ready_fd, char *err, size_t err_cap);

#endif /* WN_PLATFORM_PLATFORM_H */
