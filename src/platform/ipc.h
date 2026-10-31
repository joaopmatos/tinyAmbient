#ifndef WN_PLATFORM_IPC_H
#define WN_PLATFORM_IPC_H

#include <stddef.h>

/* Single-instance IPC (PRD FR-07). One command per newline-terminated line;
 * the daemon answers `ok`, six status lines, or `error <exit-code> <message>`.
 *
 * Linux uses a Unix socket in the runtime directory with mode 0600. The daemon
 * holds an exclusive lock on a sibling lock file for its whole lifetime, so a
 * second start cannot race the first one into binding the same address.
 *
 * Windows named pipes land with M8 parity; this module compiles there and
 * reports IPC as unsupported. */

/* Client results. The caller distinguishes "no daemon" (start one) from a
 * real failure (exit 3). */
#define WN_IPC_OK 0
#define WN_IPC_ABSENT 1
#define WN_IPC_ERROR (-1)

/* The six status lines and any error message fit comfortably here. */
#define WN_IPC_REPLY_MAX 256

/* Server handles are opaque: only platform/ipc.c knows what a socket is. */
typedef struct wn_ipc_server wn_ipc_server;

/* Sends one command line (without the newline) and stores the daemon's reply in
 * `reply`, NUL-terminated. Returns WN_IPC_OK, WN_IPC_ABSENT, or WN_IPC_ERROR. */
int wn_ipc_client_send(const char *line, char *reply, size_t cap);

/* Opens the single-instance address. Returns NULL on failure with a message in
 * `err`. The caller owns the handle until wn_ipc_server_close. */
wn_ipc_server *wn_ipc_server_open(char *err, size_t err_cap);

/* Waits up to `timeout_ms` for one client line. Returns 1 with `line` filled,
 * 0 on timeout, -1 on error. A ready client stays open for exactly one reply or
 * drop, so a malformed request cannot block the daemon. */
int wn_ipc_server_wait(wn_ipc_server *s, char *line, size_t cap,
                       int timeout_ms);

/* Sends the whole reply and closes the client. Returns 0 on success. */
int wn_ipc_server_reply(wn_ipc_server *s, const char *text);

/* Closes the current client without replying. */
void wn_ipc_server_drop(wn_ipc_server *s);

/* Gives up the address while keeping the current client open: stops accepting,
 * removes the socket, and releases the single-instance lock. The quit path calls
 * it before sending the final reply, so a client that sees the reply can never
 * observe a stale socket and can start the next daemon immediately. Holding the
 * lock past the reply would make that start fail with "another daemon is
 * already running" while this one is still tearing the audio device down. */
void wn_ipc_server_release(wn_ipc_server *s);

/* Removes the socket, releases the lock, frees the handle. */
void wn_ipc_server_close(wn_ipc_server *s);

/* Releases this process's copy of the handle without touching the socket. The
 * forking parent uses it after the daemon child has taken over: closing the
 * descriptors is safe (the child has its own copies and still holds the lock),
 * but unlinking the path would disconnect the running daemon. */
void wn_ipc_server_abandon(wn_ipc_server *s);

#endif /* WN_PLATFORM_IPC_H */
