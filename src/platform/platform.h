#ifndef WN_PLATFORM_PLATFORM_H
#define WN_PLATFORM_PLATFORM_H

#include <stddef.h>

#include "core/engine.h"

/* The platform layer owns the daemon loop, IPC, and device restart backoff.
 * It is the only layer allowed to call OS APIs.
 *
 * The run loop and the backoff live here from M6 (PRD milestone table: FR-16
 * passes at M6). IPC and single-instance enforcement land in M7. These
 * declarations are the authoritative boundary described in AGENTS.md
 * section 2. */

/* Runs the daemon until quit. `stop_after_ms` is the initial session timer in
 * milliseconds, or a negative value for "off" (PRD FR-05); `stop_fade_ms` is
 * the length of the fade-out at expiry, so a test build can shorten the
 * product's 20 s. Returns 0 on a clean exit, non-zero when no audio device
 * could be opened. */
int wn_platform_run(wn_engine *e, long stop_after_ms, long stop_fade_ms);

/* Asks wn_platform_run to return. Any thread may call it, including a signal
 * handler. */
void wn_platform_request_quit(void);

/* Sends one command line to a running daemon. Writes the reply to `reply`.
 * Returns 0 on success, non-zero when no daemon answered. Lands in M7. */
int wn_platform_ipc_send(const char *line, char *reply, size_t cap);

#endif /* WN_PLATFORM_PLATFORM_H */
