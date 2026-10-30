#ifndef WN_PLATFORM_PATHS_H
#define WN_PLATFORM_PATHS_H

#include <stddef.h>

/* Resolves filesystem locations. The only place that reads XDG_CONFIG_HOME,
 * HOME, or APPDATA. All functions write NUL-terminated absolute paths into
 * `out` and return 0, or return non-zero when the path does not fit or the
 * environment is unusable. */

/* <config-dir>/config.ini. With `portable` set, this is config.ini next to the
 * executable instead. */
int wn_paths_config_file(char *out, size_t cap, int portable);

/* <runtime-dir>/tinyambient.sock on Linux. */
int wn_paths_ipc_address(char *out, size_t cap);

#endif /* WN_PLATFORM_PATHS_H */
