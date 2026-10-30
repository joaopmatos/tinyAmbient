#ifndef WN_CORE_NOISE_H
#define WN_CORE_NOISE_H

#include <stdint.h>

/* Noise generators, in the order the CLI accepts them. WN_NOISE_NONE is the
 * "unset" value returned by wn_noise_type_parse on failure. */
typedef enum {
    WN_NOISE_NONE = -1,
    WN_NOISE_WHITE = 0,
    WN_NOISE_PINK = 1,
    WN_NOISE_BROWN = 2,
    WN_NOISE_COUNT = 3
} wn_noise_type;

/* Static name in lowercase, or NULL for an out-of-range value. */
const char *wn_noise_type_name(int type);

/* Parses a lowercase or mixed-case name. Returns WN_NOISE_NONE on failure. */
int wn_noise_type_parse(const char *name);

#endif /* WN_CORE_NOISE_H */
