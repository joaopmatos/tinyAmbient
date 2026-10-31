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

/* <runtime-dir>/tinyambient.sock on Linux. Lands in M7. */
int wn_paths_ipc_address(char *out, size_t cap);

#endif /* WN_PLATFORM_PATHS_H */
