#ifndef WN_PLATFORM_PATHS_H
#define WN_PLATFORM_PATHS_H

#include <stddef.h>

/* Large enough for an absolute Windows path plus the filename. */
#define WN_PATH_MAX 4096

/* Resolves filesystem locations. The only place that reads XDG_CONFIG_HOME,
 * HOME, or APPDATA. All functions write NUL-terminated absolute paths into
 * `out` and return 0, or return non-zero when the path does not fit or the
 * environment is unusable. */

/* <config-dir>/config.ini. With `portable` set, this is config.ini next to the
 * executable instead. */
int wn_paths_config_file(char *out, size_t cap, int portable);

/* Creates the parent directory of `file` with mode 0700 when `portable` is
 * clear. A portable configuration must already live in a writable directory;
 * nothing is created next to the executable. Returns 0 on success or non-zero
 * with a message in `err`. */
int wn_paths_make_config_dir(const char *file, int portable, char *err,
                             size_t err_cap);

/* <runtime-dir>/tinyambient.sock on Linux: $XDG_RUNTIME_DIR when set, otherwise
 * $TMPDIR (or /tmp)/tinyambient-<uid>/tinyambient.sock. On Windows:
 * \\.\pipe\tinyambient-<sid>, or $TINYAMBIENT_IPC when the test suite needs a
 * private pipe. */
int wn_paths_ipc_address(char *out, size_t cap);

/* Ensures the directory that holds the IPC address exists and belongs to the
 * current user. With XDG_RUNTIME_DIR set the session manager owns it and this
 * is a no-op; otherwise the fallback directory is created with mode 0700 and an
 * existing one is rejected unless it is a directory owned by this user. Windows
 * has no such directory. Returns 0 on success or non-zero with a message in
 * `err`. */
int wn_paths_make_runtime_dir(char *err, size_t err_cap);

#if defined(_WIN32)
/* Returns a heap copy of the current user's SID, for the DACL that restricts
 * the IPC pipe to that user. Free with free(). Declared as void* so including
 * this header does not pull in windows.h. */
void *wn_paths_user_sid(void);
#endif

#endif /* WN_PLATFORM_PATHS_H */
