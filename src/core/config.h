#ifndef WN_CORE_CONFIG_H
#define WN_CORE_CONFIG_H

#include <stddef.h>

#include "core/noise.h"

/* Parsed configuration. Field values are always in range after a successful
 * parse; malformed input falls back to the default for that key. */
typedef struct {
    int noise;   /* wn_noise_type */
    int volume;  /* 0..100 */
    int mono;    /* 0 or 1 */
    int fade_ms; /* 0..2000 */
} wn_config;

void wn_config_defaults(wn_config *cfg);

/* Parses INI text. Never fails: on errors the affected key is set to its
 * default and a message is written to `warn` when warn_cap > 0. */
void wn_config_parse(const char *text, size_t len, wn_config *out, char *warn,
                     size_t warn_cap);

/* Formats the configuration, preserving unknown keys from `original`.
 * Returns the number of bytes the full text needs, excluding the terminator.
 * A return value >= cap means `out` was truncated. */
size_t wn_config_format(const wn_config *cfg, const char *original, char *out,
                        size_t cap);

#endif /* WN_CORE_CONFIG_H */
