#ifndef WN_PLATFORM_CONFIG_FILE_H
#define WN_PLATFORM_CONFIG_FILE_H

#include <stddef.h>

#include "core/config.h"

/* Config file I/O. The parser and formatter stay in core/; this module owns the
 * paths, the directory, and the atomic write (temp file + fsync + rename). */

/* A config file is a few hundred bytes. The fixed cap keeps the loader simple
 * and makes a runaway file a reported condition instead of an allocation. */
#define WN_CONFIG_TEXT_MAX 8192

/* Loads the file into `cfg`. A missing file is not an error: the defaults are
 * used and `text` is empty. `text` receives the original bytes for
 * unknown-key preservation on a later save. Returns 0 on success, -1 when the
 * file exists but cannot be read. Warnings about malformed values are written
 * to `warn`. */
int wn_config_file_load(const char *path, wn_config *cfg, char *text,
                        size_t text_cap, size_t *text_len, char *warn,
                        size_t warn_cap);

/* Writes the configuration atomically. `original` may be NULL or the text
 * returned by wn_config_file_load; unknown keys and sections in it are
 * preserved. On failure a message is written to `err`. */
int wn_config_file_save(const char *path, const wn_config *cfg,
                        const char *original, int portable, char *err,
                        size_t err_cap);

#endif /* WN_PLATFORM_CONFIG_FILE_H */
