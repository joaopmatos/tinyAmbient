#ifndef WN_PLATFORM_PLATFORM_H
#define WN_PLATFORM_PLATFORM_H

#include <stddef.h>

#include "core/engine.h"

/* The platform layer owns the daemon loop, IPC, and device restart backoff.
 * It is the only layer allowed to call OS APIs.
 *
 * Implementation lands in M7. These declarations are the authoritative
 * boundary described in AGENTS.md section 2. */

/* Runs the daemon until quit. Returns 0 on a clean exit. */
int wn_platform_run(wn_engine *e);

/* Asks wn_platform_run to return. Any thread may call it. */
void wn_platform_request_quit(void);

/* Sends one command line to a running daemon. Writes the reply to `reply`.
 * Returns 0 on success, non-zero when no daemon answered. */
int wn_platform_ipc_send(const char *line, char *reply, size_t cap);

#endif /* WN_PLATFORM_PLATFORM_H */
