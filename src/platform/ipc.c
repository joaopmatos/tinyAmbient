/* Single-instance IPC. See platform/ipc.h for the contract and ARCHITECTURE 10
 * for the startup sequence.
 *
 * Why a lock file in addition to the socket: two invocations that both find no
 * daemon would otherwise unlink and bind the same path, and the loser's unlink
 * would orphan the winner's socket. The daemon takes an exclusive flock on a
 * sibling lock file before it removes a stale socket and binds, and keeps the
 * file descriptor open for its whole life. flock is tied to the open file
 * description, so it survives the fork into the daemon; a POSIX record lock
 * would not (it belongs to the process and is dropped when the parent exits). */

#if !defined(_WIN32)
/* flock needs the default feature set, and the socket calls need POSIX. Both
 * must be defined before any system header. */
#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#endif

#include "platform/ipc.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/command.h"
#include "platform/paths.h"

#if defined(_WIN32)

int wn_ipc_client_send(const char *line, char *reply, size_t cap)
{
    (void)line;
    (void)reply;
    (void)cap;
    /* Behave like a platform with no daemon: status falls back to the config
     * and content commands take the in-process path. */
    return WN_IPC_ABSENT;
}

wn_ipc_server *wn_ipc_server_open(char *err, size_t err_cap)
{
    (void)snprintf(err, err_cap,
                   "IPC requires Windows parity, which lands in M8");
    return NULL;
}

int wn_ipc_server_wait(wn_ipc_server *s, char *line, size_t cap, int timeout_ms)
{
    (void)s;
    (void)line;
    (void)cap;
    (void)timeout_ms;
    return -1;
}

int wn_ipc_server_reply(wn_ipc_server *s, const char *text)
{
    (void)s;
    (void)text;
    return -1;
}

void wn_ipc_server_drop(wn_ipc_server *s)
{
    (void)s;
}

void wn_ipc_server_release(wn_ipc_server *s)
{
    (void)s;
}

void wn_ipc_server_abandon(wn_ipc_server *s)
{
    free(s);
}

void wn_ipc_server_close(wn_ipc_server *s)
{
    free(s);
}

int wn_platform_ipc_supported(void)
{
    return 0;
}

#else /* POSIX */

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* A Unix socket path must fit sun_path, which is much shorter than
 * WN_PATH_MAX. */
#define WN_SOCKET_PATH_MAX (sizeof(((struct sockaddr_un *)0)->sun_path))

struct wn_ipc_server {
    int listen_fd;
    int client_fd;
    int lock_fd;
    char path[WN_PATH_MAX];
};

int wn_platform_ipc_supported(void)
{
    return 1;
}

static int write_all(int fd, const char *text, size_t length)
{
    while (length > 0) {
        ssize_t written = send(fd, text, length, 0);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            return -1;
        }
        text += written;
        length -= (size_t)written;
    }
    return 0;
}

/* One connect/send/receive round trip. Returns WN_IPC_OK, WN_IPC_ABSENT,
 * WN_IPC_ERROR, or WN_IPC_RETRY when the connection closed before any reply
 * (the daemon was shutting down) and another attempt may find the next state. */
#define WN_IPC_RETRY 2

static int client_attempt(const char *path, const char *line, char *reply,
                          size_t cap)
{
    struct sockaddr_un addr;
    struct timeval timeout;
    size_t used = 0;
    int fd;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return WN_IPC_ERROR;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int error = errno;

        (void)close(fd);
        /* A missing socket or a socket file whose owner is gone both mean
         * "start a daemon". Everything else is a real failure. */
        if (error == ENOENT || error == ECONNREFUSED || error == ENOTSOCK) {
            return WN_IPC_ABSENT;
        }
        return WN_IPC_ERROR;
    }

    /* A daemon that wedges must not hang the CLI forever. The loop answers in
     * tens of milliseconds; two seconds is a generous safety net. */
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (write_all(fd, line, strlen(line)) != 0 || write_all(fd, "\n", 1) != 0) {
        (void)close(fd);
        return WN_IPC_ERROR;
    }
    (void)shutdown(fd, SHUT_WR);

    while (used + 1 < cap) {
        ssize_t got = read(fd, reply + used, cap - 1 - used);

        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)close(fd);
            /* ECONNRESET after shutdown is the same race as an empty read. */
            if (errno == ECONNRESET) {
                return WN_IPC_RETRY;
            }
            return WN_IPC_ERROR;
        }
        if (got == 0) {
            break;
        }
        used += (size_t)got;
    }
    reply[used] = '\0';
    (void)close(fd);

    if (used == 0) {
        return WN_IPC_RETRY;
    }
    return WN_IPC_OK;
}

int wn_ipc_client_send(const char *line, char *reply, size_t cap)
{
    char path[WN_PATH_MAX];
    int attempt;

    if (line == NULL || reply == NULL || cap == 0) {
        return WN_IPC_ERROR;
    }
    reply[0] = '\0';

    if (wn_paths_ipc_address(path, sizeof(path)) != 0 ||
        strlen(path) >= WN_SOCKET_PATH_MAX) {
        return WN_IPC_ERROR;
    }

    /* Retry a closed connection: `quit` and a fresh start can overlap, and the
     * second connection must see either the old daemon or a clean "absent",
     * never a spurious failure. Three attempts over ~40 ms covers the window in
     * which the old daemon closes its listener and unlinks the socket. */
    for (attempt = 0; attempt < 3; attempt++) {
        int result = client_attempt(path, line, reply, cap);

        if (result != WN_IPC_RETRY) {
            return result;
        }
        if (attempt < 2) {
            struct timespec pause;

            pause.tv_sec = 0;
            pause.tv_nsec = 20000000L;
            (void)nanosleep(&pause, NULL);
        }
    }

    return WN_IPC_ABSENT;
}

wn_ipc_server *wn_ipc_server_open(char *err, size_t err_cap)
{
    struct sockaddr_un addr;
    wn_ipc_server *s;
    char path[WN_PATH_MAX];
    char lock_path[WN_PATH_MAX];
    int written;

    s = calloc(1, sizeof(*s));
    if (s == NULL) {
        (void)snprintf(err, err_cap, "out of memory");
        return NULL;
    }
    s->listen_fd = -1;
    s->client_fd = -1;
    s->lock_fd = -1;

    if (wn_paths_ipc_address(path, sizeof(path)) != 0) {
        (void)snprintf(err, err_cap, "cannot resolve the runtime directory");
        goto fail;
    }
    if (strlen(path) >= WN_SOCKET_PATH_MAX) {
        (void)snprintf(err, err_cap, "the IPC socket path is too long");
        goto fail;
    }

    written = snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    if (written < 0 || (size_t)written >= sizeof(lock_path)) {
        (void)snprintf(err, err_cap, "the IPC lock path is too long");
        goto fail;
    }

    s->lock_fd = open(lock_path, O_RDWR | O_CREAT, 0600);
    if (s->lock_fd < 0) {
        (void)snprintf(err, err_cap, "cannot create the IPC lock file");
        goto fail;
    }

    if (flock(s->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        (void)snprintf(err, err_cap, "another daemon is already running");
        goto fail;
    }

    /* Only now does this handle own the socket path. Before the lock is held,
     * a failure must not unlink anything: the path belongs to the running
     * daemon, and removing it would cut that daemon off from its clients. */
    (void)snprintf(s->path, sizeof(s->path), "%s", path);

    /* With the lock held, an existing socket file has no live owner: a daemon
     * would still hold the lock. Removing it is what makes recovery from a
     * killed daemon automatic. */
    (void)unlink(s->path);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, s->path, strlen(s->path) + 1);

    s->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s->listen_fd < 0) {
        (void)snprintf(err, err_cap, "cannot create the IPC socket");
        goto fail;
    }
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        (void)snprintf(err, err_cap, "cannot bind the IPC socket");
        goto fail;
    }
    /* The umask must not widen this: only the owning user may talk to the
     * daemon (PRD NFR-04). */
    if (chmod(s->path, 0600) != 0) {
        (void)snprintf(err, err_cap, "cannot protect the IPC socket");
        goto fail;
    }
    if (listen(s->listen_fd, 8) != 0) {
        (void)snprintf(err, err_cap, "cannot listen on the IPC socket");
        goto fail;
    }

    return s;

fail:
    wn_ipc_server_close(s);
    return NULL;
}

int wn_ipc_server_wait(wn_ipc_server *s, char *line, size_t cap, int timeout_ms)
{
    struct pollfd pending;
    struct timeval timeout;
    size_t used = 0;
    int ready;

    if (s == NULL || line == NULL || cap == 0 || s->listen_fd < 0) {
        return -1;
    }

    pending.fd = s->listen_fd;
    pending.events = POLLIN;
    pending.revents = 0;

    ready = poll(&pending, 1, timeout_ms);
    if (ready < 0) {
        return errno == EINTR ? 0 : -1;
    }
    if (ready == 0) {
        return 0;
    }

    s->client_fd = accept(s->listen_fd, NULL, NULL);
    if (s->client_fd < 0) {
        return errno == EINTR ? 0 : -1;
    }

    /* The line is at most WN_LINE_MAX bytes and the client sends it before it
     * starts reading, so this wait is a formality; it exists so a client that
     * connects and stalls cannot block the loop. */
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;
    (void)setsockopt(s->client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout));

    while (used + 1 < cap) {
        ssize_t got = read(s->client_fd, line + used, cap - 1 - used);

        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            wn_ipc_server_drop(s);
            return 0;
        }
        if (got == 0) {
            break;
        }
        used += (size_t)got;
        if (memchr(line, '\n', used) != NULL) {
            break;
        }
    }
    line[used] = '\0';

    /* Strip the terminator; reject a request with no complete line. */
    {
        char *newline = strchr(line, '\n');

        if (newline == NULL) {
            wn_ipc_server_drop(s);
            return 0;
        }
        *newline = '\0';
        newline = strchr(line, '\r');
        if (newline != NULL) {
            *newline = '\0';
        }
    }

    return 1;
}

int wn_ipc_server_reply(wn_ipc_server *s, const char *text)
{
    int result;

    if (s == NULL || s->client_fd < 0 || text == NULL) {
        return -1;
    }

    /* SIGPIPE is ignored by the loop, so a vanished client is just an error. */
    result = write_all(s->client_fd, text, strlen(text));
    wn_ipc_server_drop(s);
    return result;
}

void wn_ipc_server_drop(wn_ipc_server *s)
{
    if (s != NULL && s->client_fd >= 0) {
        (void)close(s->client_fd);
        s->client_fd = -1;
    }
}

void wn_ipc_server_release(wn_ipc_server *s)
{
    if (s == NULL) {
        return;
    }
    if (s->listen_fd >= 0) {
        (void)close(s->listen_fd);
        s->listen_fd = -1;
    }
    if (s->path[0] != '\0') {
        (void)unlink(s->path);
        s->path[0] = '\0';
    }
    /* The lock exists only to protect the socket path, and the path is gone.
     * Releasing it here rather than in close() keeps the quit reply honest: the
     * reply is the client's signal that it may start a new daemon, and that
     * start must not collide with this one while it stops the audio device. */
    if (s->lock_fd >= 0) {
        (void)flock(s->lock_fd, LOCK_UN);
        (void)close(s->lock_fd);
        s->lock_fd = -1;
    }
}

void wn_ipc_server_close(wn_ipc_server *s)
{
    if (s == NULL) {
        return;
    }
    wn_ipc_server_release(s);
    wn_ipc_server_drop(s);
    free(s);
}

void wn_ipc_server_abandon(wn_ipc_server *s)
{
    if (s == NULL) {
        return;
    }

    /* Close this process's descriptors but leave the socket file alone: the
     * child shares the same open file description and therefore keeps the
     * flock. Calling LOCK_UN here would release the lock for the child too. */
    wn_ipc_server_drop(s);
    if (s->listen_fd >= 0) {
        (void)close(s->listen_fd);
        s->listen_fd = -1;
    }
    if (s->lock_fd >= 0) {
        (void)close(s->lock_fd);
        s->lock_fd = -1;
    }
    s->path[0] = '\0';
    free(s);
}

#endif /* _WIN32 */
