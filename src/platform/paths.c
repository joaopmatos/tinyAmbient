#if !defined(_WIN32)
/* Must precede every system header: -std=c11 is strict, so POSIX declarations
 * (readlink, mkdir) only become visible when the feature macro is requested
 * first. */
#define _POSIX_C_SOURCE 200809L
#endif

#include "platform/paths.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <errno.h>
#include <sddl.h>
#include <windows.h>
#else
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#define WN_PATH_SEP '\\'
#else
#define WN_PATH_SEP '/'
#endif

static int format_path(char *out, size_t cap, const char *fmt, ...)
{
    va_list args;
    int written;

    if (out == NULL || cap == 0) {
        return -1;
    }

    va_start(args, fmt);
    written = vsnprintf(out, cap, fmt, args);
    va_end(args);

    if (written < 0 || (size_t)written >= cap) {
        return -1;
    }
    return 0;
}

static int executable_dir(char *out, size_t cap)
{
#if defined(_WIN32)
    char exe[WN_PATH_MAX];
    DWORD length;
    char *slash;

    length = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
    if (length == 0 || length >= sizeof(exe)) {
        return -1;
    }
    exe[length] = '\0';

    slash = strrchr(exe, '\\');
    if (slash == NULL) {
        slash = strrchr(exe, '/');
    }
    if (slash == NULL || slash == exe) {
        return -1;
    }
    if (slash == exe + 2 && exe[1] == ':') {
        /* The executable sits in a drive root: "C:" alone means the current
         * directory on C, so the separator has to stay. */
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
#else
    char exe[WN_PATH_MAX];
    ssize_t length;
    char *slash;

    length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (length <= 0 || (size_t)length >= sizeof(exe) - 1) {
        /* readlink truncates silently at the buffer size, so a full buffer
         * means the path did not fit and was cut off mid-way. */
        return -1;
    }
    exe[length] = '\0';

    slash = strrchr(exe, '/');
    if (slash == NULL) {
        return -1;
    }
    if (slash == exe) {
        /* The executable sits in the filesystem root: keep the separator,
         * because an empty directory is not a path. */
        exe[1] = '\0';
    } else {
        *slash = '\0';
    }
#endif

    if (strlen(exe) >= cap) {
        return -1;
    }
    memcpy(out, exe, strlen(exe) + 1);
    return 0;
}

int wn_paths_config_file(char *out, size_t cap, int portable)
{
    char dir[WN_PATH_MAX];

    if (out == NULL || cap == 0) {
        return -1;
    }

    if (portable) {
        if (executable_dir(dir, sizeof(dir)) != 0) {
            return -1;
        }
        return format_path(out, cap, "%s%cconfig.ini", dir, WN_PATH_SEP);
    }

#if defined(_WIN32)
    {
        const char *appdata = getenv("APPDATA");

        if (appdata == NULL || appdata[0] == '\0') {
            return -1;
        }
        /* Camel case on purpose: this directory is user-visible under APPDATA,
         * while the Unix one follows the lowercase XDG convention. Windows
         * compares paths case-insensitively, so the two spellings cannot
         * collide. */
        return format_path(out, cap, "%s\\tinyAmbient\\config.ini", appdata);
    }
#else
    {
        const char *xdg = getenv("XDG_CONFIG_HOME");
        const char *home;

        if (xdg != NULL && xdg[0] != '\0') {
            return format_path(out, cap, "%s/tinyambient/config.ini", xdg);
        }

        home = getenv("HOME");
        if (home == NULL || home[0] == '\0') {
            return -1;
        }
        return format_path(out, cap, "%s/.config/tinyambient/config.ini", home);
    }
#endif
}

static int make_directories(char *path, char *err, size_t err_cap)
{
#if defined(_WIN32)
    char *p = path;

    if (p[0] != '\0' && p[1] == ':') {
        p += 2;
    }
    if (*p == '\\' || *p == '/') {
        p++;
    }

    for (; *p != '\0'; p++) {
        if (*p == '\\' || *p == '/') {
            char separator = *p;

            *p = '\0';
            if (_mkdir(path) != 0 && errno != EEXIST) {
                *p = separator;
                (void)snprintf(err, err_cap,
                               "cannot create the configuration directory");
                return -1;
            }
            *p = separator;
        }
    }

    if (_mkdir(path) != 0 && errno != EEXIST) {
        (void)snprintf(err, err_cap,
                       "cannot create the configuration directory");
        return -1;
    }
#else
    char *p;

    for (p = path + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(path, 0700) != 0 && errno != EEXIST) {
                *p = '/';
                (void)snprintf(err, err_cap,
                               "cannot create the configuration directory");
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(path, 0700) != 0 && errno != EEXIST) {
        (void)snprintf(err, err_cap,
                       "cannot create the configuration directory");
        return -1;
    }
#endif

    return 0;
}

int wn_paths_make_config_dir(const char *file, int portable, char *err,
                             size_t err_cap)
{
    char parent[WN_PATH_MAX];
    char *slash;

    if (file == NULL) {
        return -1;
    }
    if (portable) {
        /* The executable's directory is not ours to create. The write itself
         * reports a permission problem with the full path. */
        return 0;
    }

    if (strlen(file) >= sizeof(parent)) {
        (void)snprintf(err, err_cap, "configuration path is too long");
        return -1;
    }
    memcpy(parent, file, strlen(file) + 1);

#if defined(_WIN32)
    slash = strrchr(parent, '\\');
    if (slash == NULL) {
        slash = strrchr(parent, '/');
    }
#else
    slash = strrchr(parent, '/');
#endif

    if (slash == NULL || slash == parent) {
        (void)snprintf(err, err_cap, "configuration path has no directory");
        return -1;
    }
    *slash = '\0';

    return make_directories(parent, err, err_cap);
}

#if defined(_WIN32)
void *wn_paths_user_sid(void)
{
    HANDLE token = NULL;
    DWORD needed = 0;
    TOKEN_USER *user;
    void *sid = NULL;
    DWORD length;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return NULL;
    }

    /* The first call sizes the buffer; a TOKEN_USER is variable-length because
     * the SID is embedded. */
    (void)GetTokenInformation(token, TokenUser, NULL, 0, &needed);
    if (needed == 0) {
        (void)CloseHandle(token);
        return NULL;
    }

    user = malloc(needed);
    if (user == NULL) {
        (void)CloseHandle(token);
        return NULL;
    }
    if (!GetTokenInformation(token, TokenUser, user, needed, &needed)) {
        free(user);
        (void)CloseHandle(token);
        return NULL;
    }

    length = GetLengthSid(user->User.Sid);
    sid = malloc(length);
    if (sid != NULL && !CopySid(length, sid, user->User.Sid)) {
        free(sid);
        sid = NULL;
    }

    free(user);
    (void)CloseHandle(token);
    return sid;
}
#endif

int wn_paths_ipc_address(char *out, size_t cap)
{
#if defined(_WIN32)
    const char *override;
    void *sid;
    LPSTR text = NULL;
    int result;

    /* Windows has no per-test runtime directory, so the test suite points this
     * at a private pipe instead of the user's real one. */
    override = getenv("TINYAMBIENT_IPC");
    if (override != NULL && override[0] != '\0') {
        return format_path(out, cap, "%s", override);
    }

    /* The SID, not the user name: names are not unique across domains and may
     * contain characters a pipe path cannot carry. */
    sid = wn_paths_user_sid();
    if (sid == NULL) {
        return -1;
    }
    if (!ConvertSidToStringSidA((PSID)sid, &text)) {
        free(sid);
        return -1;
    }

    result = format_path(out, cap, "\\\\.\\pipe\\tinyambient-%s", text);
    LocalFree(text);
    free(sid);
    return result;
#else /* POSIX */

    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *tmp;

    if (runtime != NULL && runtime[0] != '\0') {
        return format_path(out, cap, "%s/tinyambient.sock", runtime);
    }

    /* Without XDG_RUNTIME_DIR there is no per-user directory to inherit, so the
 * fallback gets one of its own: $TMPDIR (or /tmp) is shared and world-writable,
 * but a 0700 directory inside it is not. wn_paths_make_runtime_dir creates and
 * ownership-checks it before the daemon binds anything there. */
    tmp = getenv("TMPDIR");
    if (tmp == NULL || tmp[0] == '\0') {
        tmp = "/tmp";
    }
    return format_path(out, cap, "%s/tinyambient-%lu/tinyambient.sock", tmp,
                       (unsigned long)getuid());
#endif
}

int wn_paths_make_runtime_dir(char *err, size_t err_cap)
{
#if defined(_WIN32)
    /* The pipe lives in the kernel namespace; there is no directory to
     * create. */
    (void)err;
    (void)err_cap;
    return 0;
#else
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *tmp;
    struct stat info;
    char path[WN_PATH_MAX];
    int written;

    if (runtime != NULL && runtime[0] != '\0') {
        /* The session manager owns the directory and its permissions. */
        return 0;
    }

    tmp = getenv("TMPDIR");
    if (tmp == NULL || tmp[0] == '\0') {
        tmp = "/tmp";
    }
    written = snprintf(path, sizeof(path), "%s/tinyambient-%lu", tmp,
                       (unsigned long)getuid());
    if (written < 0 || (size_t)written >= sizeof(path)) {
        (void)snprintf(err, err_cap,
                       "the IPC runtime directory path is too long");
        return -1;
    }

    if (mkdir(path, 0700) == 0) {
        return 0;
    }
    if (errno != EEXIST) {
        (void)snprintf(err, err_cap, "cannot create the IPC runtime directory");
        return -1;
    }

    /* A file already there must be our own directory: lstat rejects a symlink,
     * and the sticky bit on /tmp keeps another user from replacing our entry
     * once the ownership check passes. */
    if (lstat(path, &info) != 0 || !S_ISDIR(info.st_mode)) {
        (void)snprintf(err, err_cap,
                       "the IPC runtime directory is not a directory");
        return -1;
    }
    if (info.st_uid != getuid()) {
        (void)snprintf(err, err_cap,
                       "the IPC runtime directory belongs to another user");
        return -1;
    }
    if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        /* Ours, but a wider umask left it open: close it instead of refusing
         * to start. */
        if (chmod(path, 0700) != 0) {
            (void)snprintf(err, err_cap,
                           "cannot protect the IPC runtime directory");
            return -1;
        }
    }

    return 0;
#endif
}
