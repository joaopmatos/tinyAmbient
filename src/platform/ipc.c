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

#include <windows.h>

/* A byte-mode named pipe is the Windows equivalent of the Unix socket: one line
 * in, one reply out, then the instance disconnects and listens again. */
#define WN_PIPE_BUFFER 4096

/* Same contract as the POSIX branch: a connection that dies before any reply
 * means the daemon was leaving, and the caller may try again. */
#define WN_IPC_RETRY 2

struct wn_ipc_server {
    HANDLE pipe;
    HANDLE event;
    OVERLAPPED ov;
    int pending;   /* ConnectNamedPipe has not completed yet */
    int connected; /* a client is attached */
    char path[WN_PATH_MAX];
};

/* Bounds one round trip. The daemon answers in tens of milliseconds, so two
 * seconds is a generous safety net and matches the POSIX client's socket
 * timeout: a wedged daemon must never hang the CLI. */
#define WN_IPC_TIMEOUT_MS 2000

/* Completes an overlapped operation that returned ERROR_IO_PENDING. Returns 0
 * with `count` set, WN_IPC_RETRY when the pipe broke before transferring
 * anything, or -1 on any other failure or a timeout. */
static int finish_io(HANDLE pipe, OVERLAPPED *ov, DWORD *count)
{
    DWORD error = GetLastError();

    if (error != ERROR_IO_PENDING) {
        return error == ERROR_BROKEN_PIPE ? WN_IPC_RETRY : -1;
    }
    if (WaitForSingleObject(ov->hEvent, WN_IPC_TIMEOUT_MS) != WAIT_OBJECT_0) {
        (void)CancelIoEx(pipe, ov);
        return -1;
    }
    if (!GetOverlappedResult(pipe, ov, count, FALSE)) {
        return GetLastError() == ERROR_BROKEN_PIPE ? WN_IPC_RETRY : -1;
    }
    return 0;
}

/* Writes every byte, or returns -1. */
static int write_all(HANDLE pipe, OVERLAPPED *ov, const char *text,
                     size_t length)
{
    while (length > 0) {
        DWORD count = 0;

        (void)ResetEvent(ov->hEvent);
        if (!WriteFile(pipe, text, (DWORD)length, &count, ov) &&
            finish_io(pipe, ov, &count) != 0) {
            return -1;
        }
        if (count == 0) {
            return -1;
        }
        text += count;
        length -= count;
    }
    return 0;
}

/* One connect/send/receive round trip. Returns WN_IPC_OK, WN_IPC_ABSENT,
 * WN_IPC_ERROR, or WN_IPC_RETRY. */
static int client_attempt(const char *path, const char *line, char *reply,
                          size_t cap)
{
    OVERLAPPED ov;
    HANDLE pipe;
    HANDLE event;
    size_t used = 0;
    int result = WN_IPC_ERROR;

    pipe = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();

        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return WN_IPC_ABSENT;
        }
        if (error != ERROR_PIPE_BUSY) {
            return WN_IPC_ERROR;
        }
        /* A live daemon is between clients. Wait for its next listen. */
        if (!WaitNamedPipeA(path, 100)) {
            return WN_IPC_RETRY;
        }
        pipe = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            return WN_IPC_ERROR;
        }
    }

    event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (event == NULL) {
        (void)CloseHandle(pipe);
        return WN_IPC_ERROR;
    }
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = event;

    if (write_all(pipe, &ov, line, strlen(line)) != 0 ||
        write_all(pipe, &ov, "\n", 1) != 0) {
        goto done;
    }

    /* Read until the server disconnects. A reply is not necessarily one line:
     * `status` is six, so a newline is not the terminator. The server closes
     * the pipe after writing the reply, and that close ends the read. A broken
     * or disconnected pipe after some data is the normal end of a reply;
     * before any data it is the "daemon was leaving" case that the retry loop
     * in wn_ipc_client_send exists for. */
    while (used + 1 < cap) {
        DWORD got = 0;

        (void)ResetEvent(event);
        if (ReadFile(pipe, reply + used, (DWORD)(cap - 1 - used), &got, &ov)) {
            /* Completed synchronously. */
        } else {
            DWORD error = GetLastError();

            if (error == ERROR_IO_PENDING) {
                if (WaitForSingleObject(event, WN_IPC_TIMEOUT_MS) !=
                    WAIT_OBJECT_0) {
                    (void)CancelIoEx(pipe, &ov);
                    result = WN_IPC_ERROR;
                    goto done;
                }
                if (!GetOverlappedResult(pipe, &ov, &got, FALSE)) {
                    error = GetLastError();
                    if (error == ERROR_BROKEN_PIPE ||
                        error == ERROR_PIPE_NOT_CONNECTED) {
                        break;
                    }
                    result = WN_IPC_ERROR;
                    goto done;
                }
            } else if (error == ERROR_BROKEN_PIPE ||
                       error == ERROR_PIPE_NOT_CONNECTED) {
                break;
            } else {
                result = WN_IPC_ERROR;
                goto done;
            }
        }
        if (got == 0) {
            break;
        }
        used += got;
    }
    reply[used] = '\0';
    result = used == 0 ? WN_IPC_RETRY : WN_IPC_OK;

done:
    (void)CloseHandle(event);
    (void)CloseHandle(pipe);
    return result;
}

int wn_ipc_client_send(const char *line, char *reply, size_t cap)
{
    char path[WN_PATH_MAX];
    int attempt;

    if (line == NULL || reply == NULL || cap == 0) {
        return WN_IPC_ERROR;
    }
    reply[0] = '\0';

    if (wn_paths_ipc_address(path, sizeof(path)) != 0) {
        return WN_IPC_ERROR;
    }

    for (attempt = 0; attempt < 3; attempt++) {
        int result = client_attempt(path, line, reply, cap);

        if (result != WN_IPC_RETRY) {
            return result;
        }
        if (attempt < 2) {
            Sleep(20);
        }
    }

    return WN_IPC_ABSENT;
}

int wn_ipc_client_send_wait(const char *line, char *reply, size_t cap)
{
    int attempt;

    for (attempt = 0; attempt < 10; attempt++) {
        int result = wn_ipc_client_send(line, reply, cap);

        if (result != WN_IPC_ABSENT) {
            return result;
        }
        if (attempt < 9) {
            Sleep(25);
        }
    }

    return WN_IPC_ABSENT;
}

/* Builds the SECURITY_ATTRIBUTES that restrict the pipe to the current user.
 * The ACL is sized from the real SID: domain and Entra accounts can carry more
 * subauthorities than a fixed buffer would hold, and a too-small buffer fails
 * with an unrelated error. *acl_out receives the allocation, which must stay
 * alive until CreateNamedPipeA has copied it. */
static int build_security(SECURITY_ATTRIBUTES *sa, SECURITY_DESCRIPTOR *sd,
                          PACL *acl_out)
{
    DWORD sid_bytes;
    DWORD acl_bytes;
    PACL acl;
    void *sid;
    int result = -1;

    *acl_out = NULL;
    sid = wn_paths_user_sid();
    if (sid == NULL) {
        return -1;
    }

    sid_bytes = GetLengthSid((PSID)sid);
    /* The ACL header plus one ACCESS_ALLOWED_ACE, whose SID is spliced in at
     * the SidStart field that the structure already counts as one DWORD. */
    acl_bytes = (DWORD)(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) -
                        sizeof(DWORD) + sid_bytes);

    acl = malloc(acl_bytes);
    if (acl != NULL && InitializeAcl(acl, acl_bytes, ACL_REVISION) &&
        AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, (PSID)sid) &&
        InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION) &&
        SetSecurityDescriptorDacl(sd, TRUE, acl, FALSE)) {
        sa->nLength = sizeof(*sa);
        sa->lpSecurityDescriptor = sd;
        sa->bInheritHandle = FALSE;
        *acl_out = acl;
        result = 0;
    } else {
        free(acl);
    }

    free(sid);
    return result;
}

wn_ipc_server *wn_ipc_server_open(char *err, size_t err_cap, int *conflict)
{
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;
    PACL acl = NULL;
    wn_ipc_server *s;
    char name[WN_PATH_MAX];
    DWORD create_error;

    if (conflict != NULL) {
        *conflict = 0;
    }

    s = calloc(1, sizeof(*s));
    if (s == NULL) {
        (void)snprintf(err, err_cap, "out of memory");
        return NULL;
    }
    s->pipe = INVALID_HANDLE_VALUE;
    s->event = INVALID_HANDLE_VALUE;

    if (wn_paths_ipc_address(name, sizeof(name)) != 0) {
        (void)snprintf(err, err_cap, "cannot resolve the IPC pipe name");
        goto fail;
    }
    if (build_security(&sa, &sd, &acl) != 0) {
        (void)snprintf(err, err_cap,
                       "cannot restrict the IPC pipe to the current user");
        goto fail;
    }

    /* One instance is the single-instance lock: while this pipe exists no
     * second daemon can create the same name. */
    s->pipe = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                               1, WN_PIPE_BUFFER, WN_PIPE_BUFFER, 0, &sa);
    /* Read the error before freeing the ACL: the heap can touch it. */
    create_error = GetLastError();
    free(acl);
    acl = NULL;

    if (s->pipe == INVALID_HANDLE_VALUE) {
        if (create_error == ERROR_ACCESS_DENIED ||
            create_error == ERROR_PIPE_BUSY) {
            if (conflict != NULL) {
                *conflict = 1;
            }
            (void)snprintf(err, err_cap, "another daemon is already running");
        } else {
            (void)snprintf(err, err_cap, "cannot create the IPC pipe");
        }
        goto fail;
    }

    s->event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (s->event == NULL) {
        (void)snprintf(err, err_cap, "cannot create the IPC event");
        goto fail;
    }
    s->ov.hEvent = s->event;
    (void)snprintf(s->path, sizeof(s->path), "%s", name);
    return s;

fail:
    free(acl);
    wn_ipc_server_close(s);
    return NULL;
}

int wn_ipc_server_wait(wn_ipc_server *s, char *line, size_t cap, int timeout_ms)
{
    DWORD count = 0;
    DWORD wait;
    size_t used = 0;

    if (s == NULL || line == NULL || cap == 0 ||
        s->pipe == INVALID_HANDLE_VALUE) {
        return -1;
    }

    if (!s->connected) {
        if (!s->pending) {
            (void)ResetEvent(s->event);
            if (!ConnectNamedPipe(s->pipe, &s->ov)) {
                DWORD error = GetLastError();

                if (error == ERROR_IO_PENDING) {
                    s->pending = 1;
                } else if (error == ERROR_PIPE_CONNECTED) {
                    s->connected = 1;
                } else if (error == ERROR_NO_DATA) {
                    /* The previous client vanished before the disconnect. */
                    (void)DisconnectNamedPipe(s->pipe);
                    return 0;
                } else {
                    return -1;
                }
            } else {
                s->connected = 1;
            }
        }

        if (!s->connected) {
            wait = WaitForSingleObject(s->event, (DWORD)timeout_ms);
            if (wait == WAIT_TIMEOUT) {
                return 0;
            }
            if (wait != WAIT_OBJECT_0) {
                s->pending = 0;
                return -1;
            }
            if (!GetOverlappedResult(s->pipe, &s->ov, &count, FALSE) &&
                GetLastError() != ERROR_PIPE_CONNECTED) {
                s->pending = 0;
                return -1;
            }
            s->pending = 0;
            s->connected = 1;
            (void)ResetEvent(s->event);
        }
    }

    /* The client sends the whole line before it starts reading, so this
     * normally completes at once. The timeout only bounds a client that
     * connects and then stalls, exactly like the POSIX receive timeout. */
    while (used + 1 < cap) {
        DWORD got = 0;

        (void)ResetEvent(s->event);
        if (!ReadFile(s->pipe, line + used, (DWORD)(cap - 1 - used), &got,
                      &s->ov)) {
            DWORD error = GetLastError();

            if (error == ERROR_IO_PENDING) {
                wait = WaitForSingleObject(s->event, 200);
                if (wait != WAIT_OBJECT_0 ||
                    !GetOverlappedResult(s->pipe, &s->ov, &got, FALSE)) {
                    wn_ipc_server_drop(s);
                    return 0;
                }
            } else if (error == ERROR_BROKEN_PIPE ||
                       error == ERROR_PIPE_NOT_CONNECTED) {
                wn_ipc_server_drop(s);
                return 0;
            } else {
                wn_ipc_server_drop(s);
                return -1;
            }
        }
        if (got == 0) {
            wn_ipc_server_drop(s);
            return 0;
        }
        used += got;
        if (memchr(line, '\n', used) != NULL) {
            break;
        }
    }
    line[used] = '\0';

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

/* Writes the whole reply. Returns 0 on success. */
static int write_reply(wn_ipc_server *s, const char *text)
{
    /* A byte-mode pipe write may complete with fewer bytes than requested, so
     * the loop in write_all is required even though a reply is short. */
    return write_all(s->pipe, &s->ov, text, strlen(text));
}

int wn_ipc_server_reply(wn_ipc_server *s, const char *text)
{
    int result;

    if (s == NULL || !s->connected || text == NULL) {
        return -1;
    }

    result = write_reply(s, text);
    if (result == 0) {
        /* DisconnectNamedPipe discards unread bytes, so wait until the client
         * has consumed the reply. */
        (void)FlushFileBuffers(s->pipe);
    }
    wn_ipc_server_drop(s);
    return result;
}

int wn_ipc_server_reply_and_release(wn_ipc_server *s, const char *text)
{
    int result;

    if (s == NULL || !s->connected || text == NULL) {
        return -1;
    }

    result = write_reply(s, text);
    if (result == 0) {
        (void)FlushFileBuffers(s->pipe);
    }

    /* The instance is the connection on Windows, so destroy it instead of
     * disconnecting: no new client can attach to a daemon that is leaving. */
    s->connected = 0;
    s->pending = 0;
    if (s->pipe != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(s->pipe);
        s->pipe = INVALID_HANDLE_VALUE;
    }
    return result;
}

void wn_ipc_server_drop(wn_ipc_server *s)
{
    if (s != NULL && s->connected) {
        (void)DisconnectNamedPipe(s->pipe);
        s->connected = 0;
        s->pending = 0;
    }
}

void wn_ipc_server_release(wn_ipc_server *s)
{
    if (s == NULL) {
        return;
    }
    wn_ipc_server_drop(s);
    if (s->pipe != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(s->pipe);
        s->pipe = INVALID_HANDLE_VALUE;
    }
}

void wn_ipc_server_close(wn_ipc_server *s)
{
    if (s == NULL) {
        return;
    }
    wn_ipc_server_release(s);
    if (s->event != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(s->event);
        s->event = INVALID_HANDLE_VALUE;
    }
    free(s);
}

void wn_ipc_server_abandon(wn_ipc_server *s)
{
    /* There is no fork on Windows: the handle never has a second owner. */
    wn_ipc_server_close(s);
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
            /* ECONNRESET after shutdown is the same race as an empty read, but
             * a reply that arrived before the reset is still a reply: treat it
             * as the end of the stream instead of replaying the command. */
            if (errno == ECONNRESET) {
                break;
            }
            (void)close(fd);
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

int wn_ipc_client_send_wait(const char *line, char *reply, size_t cap)
{
    int attempt;

    for (attempt = 0; attempt < 10; attempt++) {
        int result = wn_ipc_client_send(line, reply, cap);

        if (result != WN_IPC_ABSENT) {
            return result;
        }
        if (attempt < 9) {
            struct timespec pause;

            pause.tv_sec = 0;
            pause.tv_nsec = 25000000L;
            (void)nanosleep(&pause, NULL);
        }
    }

    return WN_IPC_ABSENT;
}

wn_ipc_server *wn_ipc_server_open(char *err, size_t err_cap, int *conflict)
{
    struct sockaddr_un addr;
    wn_ipc_server *s;
    char path[WN_PATH_MAX];
    char lock_path[WN_PATH_MAX];
    int written;

    if (conflict != NULL) {
        *conflict = 0;
    }

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
    if (wn_paths_make_runtime_dir(err, err_cap) != 0) {
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
        if (conflict != NULL) {
            *conflict = 1;
        }
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

int wn_ipc_server_reply_and_release(wn_ipc_server *s, const char *text)
{
    if (s == NULL) {
        return -1;
    }

    /* Give up the address first: the client takes the reply as permission to
     * start the next daemon. The accepted connection stays open, so the reply
     * still goes out. */
    wn_ipc_server_release(s);
    return wn_ipc_server_reply(s, text);
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
