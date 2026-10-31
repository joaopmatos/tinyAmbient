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
    *slash = '\0';
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
    if (slash == NULL || slash == exe) {
        return -1;
    }
    *slash = '\0';
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
